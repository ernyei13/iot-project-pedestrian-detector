#include "alarm_mqtt.hpp"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

namespace {
constexpr EventBits_t WIFI_CONNECTED_BIT = BIT0;
constexpr EventBits_t WIFI_FAIL_BIT = BIT1;
const char *TAG = "alarm_mqtt";

bool has_text(const char *value)
{
    return value != nullptr && value[0] != '\0';
}
} // namespace

esp_err_t AlarmMqttClient::start()
{
    m_network_enabled = has_text(CONFIG_SVA_WIFI_SSID) && has_text(CONFIG_SVA_MQTT_URI);
    if (!m_network_enabled) {
        ESP_LOGW(TAG, "Wi-Fi SSID or MQTT URI not configured; detections will be logged only");
        return ESP_OK;
    }

    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed (%s); leaving NVS untouched and disabling MQTT", esp_err_to_name(err));
        return err;
    }

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
        return err;
    }

    m_wifi_event_group = xEventGroupCreate();
    if (!m_wifi_event_group) {
        ESP_LOGE(TAG, "Failed to create Wi-Fi event group");
        return ESP_ERR_NO_MEM;
    }

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_config));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &AlarmMqttClient::wifi_event_handler, this, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &AlarmMqttClient::wifi_event_handler, this, nullptr));

    wifi_config_t wifi_config = {};
    strlcpy(reinterpret_cast<char *>(wifi_config.sta.ssid), CONFIG_SVA_WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy(reinterpret_cast<char *>(wifi_config.sta.password),
            CONFIG_SVA_WIFI_PASSWORD,
            sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = has_text(CONFIG_SVA_WIFI_PASSWORD) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    const TickType_t wait_ticks = pdMS_TO_TICKS(CONFIG_SVA_WIFI_CONNECT_TIMEOUT_SECONDS * 1000);
    EventBits_t bits = xEventGroupWaitBits(
        m_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, wait_ticks);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Wi-Fi connected; MQTT startup requested");
        return ESP_OK;
    }
    if (bits & WIFI_FAIL_BIT) {
        ESP_LOGW(TAG, "Wi-Fi failed after %d attempts; detections will be logged until network recovers",
                 CONFIG_SVA_WIFI_MAX_RETRY);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Wi-Fi did not connect within %d seconds; detections will be logged",
             CONFIG_SVA_WIFI_CONNECT_TIMEOUT_SECONDS);
    return ESP_OK;
}

bool AlarmMqttClient::publish_person_event(const char *event_id,
                                           int64_t uptime_us,
                                           float confidence,
                                           int confirmation_frames,
                                           int x_min,
                                           int y_min,
                                           int x_max,
                                           int y_max)
{
    char payload[384];
    const int written = snprintf(payload,
                                 sizeof(payload),
                                 "{\"event_id\":\"%s\","
                                 "\"device_id\":\"%s\","
                                 "\"timestamp\":\"uptime_us:%lld\","
                                 "\"label\":\"person\","
                                 "\"confidence\":%.3f,"
                                 "\"confirmation_frames\":%d,"
                                 "\"box\":{\"x_min\":%d,\"y_min\":%d,\"x_max\":%d,\"y_max\":%d}}",
                                 event_id,
                                 CONFIG_SVA_DEVICE_ID,
                                 static_cast<long long>(uptime_us),
                                 confidence,
                                 confirmation_frames,
                                 x_min,
                                 y_min,
                                 x_max,
                                 y_max);
    if (written < 0 || written >= static_cast<int>(sizeof(payload))) {
        ESP_LOGE(TAG, "MQTT payload did not fit in buffer");
        return false;
    }

    ESP_LOGI(TAG, "Alarm event: %s", payload);
    if (!m_client || !m_mqtt_connected) {
        ESP_LOGW(TAG, "MQTT is not connected; event was logged but not published");
        return false;
    }

    int msg_id = esp_mqtt_client_publish(m_client, CONFIG_SVA_MQTT_TOPIC, payload, 0, 1, 0);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to enqueue MQTT alarm event");
        return false;
    }

    ESP_LOGI(TAG, "MQTT alarm event published with message id %d", msg_id);
    return true;
}

void AlarmMqttClient::wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    auto *self = static_cast<AlarmMqttClient *>(arg);

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        self->m_mqtt_connected = false;
        if (self->m_wifi_retry_count < CONFIG_SVA_WIFI_MAX_RETRY) {
            self->m_wifi_retry_count++;
            ESP_LOGW(TAG, "Wi-Fi disconnected; reconnect attempt %d/%d",
                     self->m_wifi_retry_count,
                     CONFIG_SVA_WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else if (self->m_wifi_event_group) {
            xEventGroupSetBits(self->m_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        auto *event = static_cast<ip_event_got_ip_t *>(event_data);
        ESP_LOGI(TAG, "Wi-Fi got IP " IPSTR, IP2STR(&event->ip_info.ip));
        self->m_wifi_retry_count = 0;
        if (self->m_wifi_event_group) {
            xEventGroupSetBits(self->m_wifi_event_group, WIFI_CONNECTED_BIT);
        }
        self->start_mqtt();
    }
}

void AlarmMqttClient::mqtt_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    auto *self = static_cast<AlarmMqttClient *>(arg);
    auto *event = static_cast<esp_mqtt_event_t *>(event_data);

    switch (event->event_id) {
    case MQTT_EVENT_CONNECTED:
        self->m_mqtt_connected = true;
        ESP_LOGI(TAG, "MQTT connected to %s", CONFIG_SVA_MQTT_URI);
        break;
    case MQTT_EVENT_DISCONNECTED:
        self->m_mqtt_connected = false;
        ESP_LOGW(TAG, "MQTT disconnected");
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "MQTT transport error");
        break;
    default:
        break;
    }
}

esp_err_t AlarmMqttClient::start_mqtt()
{
    if (m_client || !m_network_enabled) {
        return ESP_OK;
    }

    esp_mqtt_client_config_t mqtt_config = {};
    mqtt_config.broker.address.uri = CONFIG_SVA_MQTT_URI;
    mqtt_config.credentials.client_id = CONFIG_SVA_DEVICE_ID;
    if (has_text(CONFIG_SVA_MQTT_USERNAME)) {
        mqtt_config.credentials.username = CONFIG_SVA_MQTT_USERNAME;
        mqtt_config.credentials.authentication.password = CONFIG_SVA_MQTT_PASSWORD;
    }
    mqtt_config.session.keepalive = 60;

    m_client = esp_mqtt_client_init(&mqtt_config);
    if (!m_client) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(
        esp_mqtt_client_register_event(m_client, MQTT_EVENT_ANY, &AlarmMqttClient::mqtt_event_handler, this));
    ESP_LOGI(TAG, "Starting MQTT client for topic %s", CONFIG_SVA_MQTT_TOPIC);
    return esp_mqtt_client_start(m_client);
}

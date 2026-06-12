#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "mqtt_client.h"

class AlarmMqttClient {
public:
    esp_err_t start();
    bool publish_person_event(const char *event_id,
                              int64_t uptime_us,
                              float confidence,
                              int confirmation_frames,
                              int x_min,
                              int y_min,
                              int x_max,
                              int y_max);

private:
    static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
    static void mqtt_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

    esp_err_t start_mqtt();

    esp_mqtt_client_handle_t m_client = nullptr;
    EventGroupHandle_t m_wifi_event_group = nullptr;
    int m_wifi_retry_count = 0;
    bool m_mqtt_connected = false;
    bool m_network_enabled = false;
};

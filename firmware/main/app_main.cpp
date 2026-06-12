#include <algorithm>
#include <cstdio>
#include <list>

#include "alarm_mqtt.hpp"
#include "bsp/esp-bsp.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "frame_cap_pipeline.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lcd_preview.hpp"
#include "pedestrian_detect.hpp"
#include "who_detect.hpp"
#include "who_yield2idle.hpp"

namespace {
const char *TAG = "smart_alarm";

struct BestDetection {
    bool found = false;
    dl::detect::result_t result = {};
};

float detection_threshold()
{
    return static_cast<float>(CONFIG_SVA_DETECTION_SCORE_THRESHOLD_PERCENT) / 100.0f;
}

BestDetection find_best_detection(const std::list<dl::detect::result_t> &results)
{
    BestDetection best;
    for (const auto &result : results) {
        if (!best.found || result.score > best.result.score) {
            best.found = true;
            best.result = result;
        }
    }
    return best;
}

class DetectionAlarmPolicy {
public:
    explicit DetectionAlarmPolicy(AlarmMqttClient &mqtt) : m_mqtt(mqtt) {}

    void on_detection_result(const who::detect::WhoDetect::result_t &result)
    {
        BestDetection best = find_best_detection(result.det_res);
        if (!best.found || best.result.score < detection_threshold()) {
            if (m_confirmation_count > 0) {
                ESP_LOGI(TAG, "Person confirmation reset");
            }
            m_confirmation_count = 0;
            return;
        }

        m_confirmation_count++;
        int x_min = -1;
        int y_min = -1;
        int x_max = -1;
        int y_max = -1;
        if (best.result.box.size() >= 4) {
            x_min = best.result.box[0];
            y_min = best.result.box[1];
            x_max = best.result.box[2];
            y_max = best.result.box[3];
        }

        ESP_LOGI(TAG,
                 "Person candidate score=%.3f box=[%d,%d,%d,%d] confirmation=%d/%d",
                 best.result.score,
                 x_min,
                 y_min,
                 x_max,
                 y_max,
                 m_confirmation_count,
                 CONFIG_SVA_CONFIRMATION_FRAMES);

        if (m_confirmation_count < CONFIG_SVA_CONFIRMATION_FRAMES) {
            return;
        }

        int64_t now_us = esp_timer_get_time();
        int64_t cooldown_us = static_cast<int64_t>(CONFIG_SVA_ALERT_COOLDOWN_SECONDS) * 1000000LL;
        if (m_last_alert_us > 0 && now_us - m_last_alert_us < cooldown_us) {
            ESP_LOGI(TAG, "Person confirmed, but alert cooldown is active");
            return;
        }

        char event_id[96];
        m_event_counter++;
        snprintf(event_id,
                 sizeof(event_id),
                 "%s-%06lu",
                 CONFIG_SVA_DEVICE_ID,
                 static_cast<unsigned long>(m_event_counter));
        m_mqtt.publish_person_event(
            event_id, now_us, best.result.score, m_confirmation_count, x_min, y_min, x_max, y_max);
        m_last_alert_us = now_us;
        m_confirmation_count = 0;
    }

private:
    AlarmMqttClient &m_mqtt;
    int m_confirmation_count = 0;
    uint32_t m_event_counter = 0;
    int64_t m_last_alert_us = 0;
};

dl::detect::Detect *create_pedestrian_model()
{
    auto *model = new PedestrianDetect(static_cast<PedestrianDetect::model_type_t>(CONFIG_DEFAULT_PEDESTRIAN_DETECT_MODEL),
                                       false);
    model->set_score_thr(detection_threshold(), 0);
    return model;
}

void initialize_status_led()
{
#ifdef BSP_BOARD_ESP32_S3_EYE
    esp_err_t err = bsp_leds_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LED init failed: %s", esp_err_to_name(err));
        return;
    }

    err = bsp_led_set(BSP_LED_GREEN, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LED update failed: %s", esp_err_to_name(err));
    }
#endif
}

void run_detector(AlarmMqttClient &mqtt, smart_alarm::LcdPreview &preview)
{
    auto *frame_cap = smart_alarm::make_frame_cap_pipeline();
    auto *detect = new who::detect::WhoDetect("Detect", frame_cap->get_last_node());
    DetectionAlarmPolicy alarm_policy(mqtt);

    detect->set_model(create_pedestrian_model());
    detect->set_fps(static_cast<float>(CONFIG_SVA_DETECTION_FPS));
    detect->set_detect_result_cb([&alarm_policy, &preview](const who::detect::WhoDetect::result_t &result) {
        preview.render(result.img, smart_alarm::make_preview_detection(result.det_res, detection_threshold()));
        alarm_policy.on_detection_result(result);
    });

    bool started = who::WhoYield2Idle::get_instance()->run();
    for (const auto &frame_cap_node : frame_cap->get_all_nodes()) {
        started &= frame_cap_node->run(4096, 2, 0);
    }
    started &= detect->run(6144, 2, 1);

    if (!started) {
        ESP_LOGE(TAG, "Detector pipeline failed to start");
    } else {
        ESP_LOGI(TAG,
                 "Detector pipeline running with threshold %.2f, confirmation=%d frames, cooldown=%d seconds",
                 detection_threshold(),
                 CONFIG_SVA_CONFIRMATION_FRAMES,
                 CONFIG_SVA_ALERT_COOLDOWN_SECONDS);
    }

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "Detector heartbeat");
    }
}
} // namespace

extern "C" void app_main(void)
{
    vTaskPrioritySet(xTaskGetCurrentTaskHandle(), 5);

    ESP_LOGI(TAG, "Smart Visual Alarm firmware starting");
    initialize_status_led();

    smart_alarm::LcdPreview preview;
    esp_err_t preview_start = preview.start();
    if (preview_start != ESP_OK) {
        ESP_LOGW(TAG, "LCD preview unavailable; continuing without screen preview");
    }

    AlarmMqttClient mqtt;
    esp_err_t mqtt_start = mqtt.start();
    if (mqtt_start != ESP_OK) {
        ESP_LOGW(TAG, "MQTT startup skipped after error: %s", esp_err_to_name(mqtt_start));
    }

    run_detector(mqtt, preview);
}

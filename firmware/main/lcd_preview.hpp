#pragma once

#include "dl_detect_base.hpp"
#include "dl_image.hpp"
#include "esp_err.h"
#include "esp_lcd_types.h"

#include <list>

namespace smart_alarm {

struct PreviewDetection {
    bool valid = false;
    float score = 0.0f;
    int x_min = 0;
    int y_min = 0;
    int x_max = 0;
    int y_max = 0;
};

class LcdPreview {
public:
    esp_err_t start();
    void render(const dl::image::img_t &img, const PreviewDetection &detection);
    bool enabled() const { return m_enabled; }

private:
    bool m_enabled = false;
    void *m_frame_buffer = nullptr;
    size_t m_frame_buffer_len = 0;
    esp_lcd_panel_handle_t m_panel = nullptr;
    esp_lcd_panel_io_handle_t m_panel_io = nullptr;
};

PreviewDetection make_preview_detection(const std::list<dl::detect::result_t> &results, float threshold);

} // namespace smart_alarm

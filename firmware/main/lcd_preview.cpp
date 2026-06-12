#include "lcd_preview.hpp"

#include <algorithm>
#include <cstring>
#include <list>
#include <vector>

#include "bsp/esp-bsp.h"
#include "dl_image_draw.hpp"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"

namespace smart_alarm {
namespace {
const char *TAG = "lcd_preview";
const std::vector<uint8_t> kBoxColorRgb565BigEndian = {0xF8, 0x00};

size_t rgb565_frame_len(const dl::image::img_t &img)
{
    return static_cast<size_t>(img.width) * static_cast<size_t>(img.height) * sizeof(uint16_t);
}

int clamp_to_image(int value, int limit)
{
    return std::clamp(value, 0, limit - 1);
}
} // namespace

esp_err_t LcdPreview::start()
{
#if CONFIG_SVA_LCD_PREVIEW
    const bsp_display_config_t display_config = {
        .max_transfer_sz = BSP_LCD_H_RES * BSP_LCD_V_RES * static_cast<int>(sizeof(uint16_t)),
    };

    esp_err_t err = bsp_display_new(&display_config, &m_panel, &m_panel_io);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LCD init failed; preview disabled: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_lcd_panel_disp_on_off(m_panel, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LCD enable failed; preview disabled: %s", esp_err_to_name(err));
        return err;
    }

    err = bsp_display_backlight_on();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LCD backlight failed; continuing without brightness control: %s", esp_err_to_name(err));
    }

    m_frame_buffer_len = BSP_LCD_H_RES * BSP_LCD_V_RES * sizeof(uint16_t);
    m_frame_buffer = heap_caps_malloc(m_frame_buffer_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!m_frame_buffer) {
        m_frame_buffer = heap_caps_malloc(m_frame_buffer_len, MALLOC_CAP_DMA);
    }
    if (!m_frame_buffer) {
        ESP_LOGW(TAG, "LCD preview buffer allocation failed; preview disabled");
        return ESP_ERR_NO_MEM;
    }

    std::memset(m_frame_buffer, 0, m_frame_buffer_len);
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        esp_lcd_panel_draw_bitmap(m_panel, 0, 0, BSP_LCD_H_RES, BSP_LCD_V_RES, m_frame_buffer));

    m_enabled = true;
    ESP_LOGI(TAG, "LCD preview enabled (%dx%d RGB565)", BSP_LCD_H_RES, BSP_LCD_V_RES);
    return ESP_OK;
#else
    ESP_LOGI(TAG, "LCD preview disabled by config");
    return ESP_OK;
#endif
}

void LcdPreview::render(const dl::image::img_t &img, const PreviewDetection &detection)
{
#if CONFIG_SVA_LCD_PREVIEW
    if (!m_enabled || !m_panel || !m_frame_buffer) {
        return;
    }
    if (img.pix_type != dl::image::DL_IMAGE_PIX_TYPE_RGB565 || img.width != BSP_LCD_H_RES ||
        img.height != BSP_LCD_V_RES) {
        ESP_LOGW(TAG, "Unsupported preview frame: %dx%d type=%d", img.width, img.height, img.pix_type);
        return;
    }

    const size_t frame_len = rgb565_frame_len(img);
    if (frame_len > m_frame_buffer_len) {
        ESP_LOGW(TAG, "Preview frame is larger than the LCD buffer");
        return;
    }

    std::memcpy(m_frame_buffer, img.data, frame_len);

    if (detection.valid) {
        const int x1 = clamp_to_image(detection.x_min, img.width);
        const int y1 = clamp_to_image(detection.y_min, img.height);
        const int x2 = clamp_to_image(detection.x_max, img.width);
        const int y2 = clamp_to_image(detection.y_max, img.height);
        if (x2 > x1 && y2 > y1) {
            const dl::image::img_t overlay_img = {
                .data = m_frame_buffer,
                .width = img.width,
                .height = img.height,
                .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565,
            };
            dl::image::draw_hollow_rectangle(
                overlay_img, x1, y1, x2, y2, kBoxColorRgb565BigEndian, CONFIG_SVA_LCD_BOX_LINE_WIDTH);
        }
    }

    esp_err_t err = esp_lcd_panel_draw_bitmap(m_panel, 0, 0, img.width, img.height, m_frame_buffer);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LCD preview draw failed: %s", esp_err_to_name(err));
    }
#endif
}

PreviewDetection make_preview_detection(const std::list<dl::detect::result_t> &results, float threshold)
{
    PreviewDetection preview;
    for (const auto &result : results) {
        if (result.score < threshold || result.box.size() < 4) {
            continue;
        }
        if (!preview.valid || result.score > preview.score) {
            preview.valid = true;
            preview.score = result.score;
            preview.x_min = result.box[0];
            preview.y_min = result.box[1];
            preview.x_max = result.box[2];
            preview.y_max = result.box[3];
        }
    }
    return preview;
}

} // namespace smart_alarm

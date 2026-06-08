#pragma once

#include "display/lcd_display.h"
#include "lvgl_display/emoji_collection.h"
#include "lvgl_display/lvgl_theme.h"
#include "lvgl_display/lvgl_image.h"

#include <map>
#include <string>
#include <memory>

/**
 * @brief 21种静态表情显示类
 * 继承SpiLcdDisplay，使用主题中的表情集合，支持自定义表情
 */
class OttoEmojiDisplay : public SpiLcdDisplay {
public:
    OttoEmojiDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                     int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                     bool swap_xy);

    virtual ~OttoEmojiDisplay() = default;

    virtual void SetEmotion(const char* emotion) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override;

private:
    void SetupEmojiContainer();
    const LvglImage* GetEmojiImage(const char* emotion);

    lv_obj_t* emotion_image_;
    std::map<std::string, std::unique_ptr<LvglAllocatedImage>> emoji_cache_;
};

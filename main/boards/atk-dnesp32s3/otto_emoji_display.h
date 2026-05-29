#pragma once

#include <libs/gif/lv_gif.h>

#include "display/lcd_display.h"
#include "otto_emoji_gif.h"

// Electron Bot表情GIF声明 - 使用与Otto相同的6个表情
LV_IMAGE_DECLARE(staticstate);  // 静态状态/中性表情
LV_IMAGE_DECLARE(sad);          // 悲伤
LV_IMAGE_DECLARE(happy);        // 开心
LV_IMAGE_DECLARE(scare);        // 惊吓/惊讶
LV_IMAGE_DECLARE(buxue);        // 不学/困惑
LV_IMAGE_DECLARE(anger);        // 愤怒

/**
 * @brief Otto机器人GIF表情显示类
 * 继承LcdDisplay，添加GIF表情支持
 */
class OttoEmojiDisplay : public SpiLcdDisplay {
public:
    /**
     * @brief 构造函数，参数与SpiLcdDisplay相同
     */
    OttoEmojiDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                     int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                     bool swap_xy);

    virtual ~OttoEmojiDisplay() = default;

    virtual void SetEmotion(const char* emotion) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override;

private:
    void SetupGifContainer();

    lv_obj_t* emotion_gif_;  ///< GIF表情组件

    // 表情映射
    struct EmotionMap {
        const char* name;
        const lv_img_dsc_t* gif;
    };

    static const EmotionMap emotion_maps_[];
};
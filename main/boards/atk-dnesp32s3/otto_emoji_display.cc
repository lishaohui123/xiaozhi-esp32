#include "otto_emoji_display.h"
#include "lvgl_theme.h"

#include <esp_log.h>
#include <font_awesome.h>

#include <algorithm>
#include <cstring>
#include <string>

#include "display/lcd_display.h"

#define TAG "OttoEmojiDisplay"

OttoEmojiDisplay::OttoEmojiDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                                   int width, int height, int offset_x, int offset_y, bool mirror_x,
                                   bool mirror_y, bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy),
      emotion_image_(nullptr) {
    SetupEmojiContainer();
};

void OttoEmojiDisplay::SetupEmojiContainer() {
    DisplayLockGuard lock(this);

    preview_image_cached_.reset();

    if (content_) {
        lv_obj_del(content_);
        content_ = nullptr;
    }
    chat_message_label_ = nullptr;
    preview_image_ = nullptr;
    emoji_image_ = nullptr;
    emoji_box_ = nullptr;

    content_ = lv_obj_create(container_);
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(content_, LV_HOR_RES, LV_HOR_RES);
    lv_obj_set_style_bg_opa(content_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_center(content_);

    emoji_label_ = lv_label_create(content_);
    lv_label_set_text(emoji_label_, "");
    lv_obj_set_width(emoji_label_, 0);
    lv_obj_set_style_border_width(emoji_label_, 0, 0);
    lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);

    emotion_image_ = lv_image_create(content_);
    lv_obj_set_size(emotion_image_, LV_HOR_RES, LV_HOR_RES);
    lv_obj_set_style_border_width(emotion_image_, 0, 0);
    lv_obj_center(emotion_image_);

    const LvglImage* neutral_img = GetEmojiImage("neutral");
    if (neutral_img) {
        lv_image_set_src(emotion_image_, neutral_img->image_dsc());
    }

    auto& theme_manager = LvglThemeManager::GetInstance();
    auto theme = theme_manager.GetTheme("dark");
    if (theme != nullptr) {
        LcdDisplay::SetTheme(theme);
    }
}

const LvglImage* OttoEmojiDisplay::GetEmojiImage(const char* emotion) {
    auto theme = GetTheme();
    if (!theme) {
        return nullptr;
    }

    auto lvgl_theme = static_cast<LvglTheme*>(theme);
    auto custom_emoji_collection = lvgl_theme->emoji_collection();

    if (custom_emoji_collection != nullptr) {
        const LvglImage* img = custom_emoji_collection->GetEmojiImage(emotion);
        if (img) {
            const lv_img_dsc_t* dsc = img->image_dsc();
            
            if (dsc->header.w > 0 && dsc->header.h > 0) {
                return img;
            }

            auto cache_it = emoji_cache_.find(emotion);
            if (cache_it != emoji_cache_.end()) {
                return cache_it->second.get();
            }

            try {
                auto decoded_img = std::make_unique<LvglAllocatedImage>(
                    const_cast<void*>(static_cast<const void*>(dsc->data)),
                    dsc->data_size
                );
                emoji_cache_[emotion] = std::move(decoded_img);
                return emoji_cache_[emotion].get();
            } catch (...) {
                ESP_LOGE(TAG, "Failed to decode custom emoji: %s", emotion);
            }
        }
    }

    static Twemoji32 default_emoji_collection;
    return default_emoji_collection.GetEmojiImage(emotion);
}

void OttoEmojiDisplay::SetEmotion(const char* emotion) {
    if (!emotion || !emotion_image_) {
        return;
    }

    DisplayLockGuard lock(this);

    const LvglImage* img = GetEmojiImage(emotion);
    if (img) {
        lv_image_set_src(emotion_image_, img->image_dsc());
        ESP_LOGI(TAG, "设置表情: %s", emotion);
    } else {
        const LvglImage* neutral_img = GetEmojiImage("neutral");
        if (neutral_img) {
            lv_image_set_src(emotion_image_, neutral_img->image_dsc());
        }
        ESP_LOGI(TAG, "未知表情'%s'，使用默认", emotion);
    }
}

void OttoEmojiDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    return;
}

void OttoEmojiDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr) {
        return;
    }

    if (content == nullptr || strlen(content) == 0) {
        lv_obj_add_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_label_set_text(chat_message_label_, content);
    lv_obj_remove_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(TAG, "设置聊天消息 [%s]: %s", role, content);
}

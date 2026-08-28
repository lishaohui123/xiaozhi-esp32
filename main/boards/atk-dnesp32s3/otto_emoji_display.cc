#include "otto_emoji_display.h"
#include "lvgl_theme.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
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
      emotion_image_(nullptr), emotion_gif_(nullptr) {
    SetupEmojiContainer();
};

void OttoEmojiDisplay::SetupEmojiContainer() {
    DisplayLockGuard lock(this);

    preview_image_cached_.reset();

    // 如果存在旧的内容容器，先删除
    if (content_) {
        lv_obj_del(content_);
        content_ = nullptr;
    }

    // =========================================================================
    // 【新增修复1】：在覆盖 emoji_label_ 指针之前，先清理基类遗留下来的 "TEXT AI" 标签
    if (emoji_label_ != nullptr) {
        lv_obj_del(emoji_label_);  
        emoji_label_ = nullptr;    
    }
    // =========================================================================
    
    // =========================================================================
    // 【新增修复2】：隐藏顶部的状态栏和顶栏，彻底去掉残留的 "Text" 字样
    if (top_bar_) lv_obj_add_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
    if (status_bar_) lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
    // 如果此时还显示底部的底栏，也可以一并隐藏
    if (bottom_bar_) lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    // =========================================================================

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

    // 下面是你原有的代码，重新创建一个 emoji_label_ 并且隐藏它
    emoji_label_ = lv_label_create(content_);
    lv_label_set_text(emoji_label_, "");
    lv_obj_set_width(emoji_label_, 0);
    lv_obj_set_style_border_width(emoji_label_, 0, 0);
    lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);

    emotion_image_ = lv_image_create(content_);
    lv_obj_set_size(emotion_image_, LV_HOR_RES, LV_HOR_RES);
    lv_obj_set_style_border_width(emotion_image_, 0, 0);
    lv_obj_center(emotion_image_);

    emotion_gif_ = lv_gif_create(content_);
    lv_obj_set_size(emotion_gif_, LV_HOR_RES, LV_HOR_RES);
    lv_obj_set_style_border_width(emotion_gif_, 0, 0);
    lv_obj_center(emotion_gif_);
    lv_obj_add_flag(emotion_gif_, LV_OBJ_FLAG_HIDDEN);

    const LvglImage* neutral_img = GetEmojiImage("neutral");
    if (neutral_img) {
        ShowEmojiImage(neutral_img);
    }

    chat_message_label_ = lv_label_create(container_);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_width(chat_message_label_, LV_HOR_RES * 0.9);
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(chat_message_label_, lv_color_white(), 0);
    lv_obj_set_style_border_width(chat_message_label_, 0, 0);
    lv_obj_set_style_bg_opa(chat_message_label_, LV_OPA_70, 0);
    lv_obj_set_style_bg_color(chat_message_label_, lv_color_black(), 0);
    lv_obj_set_style_pad_ver(chat_message_label_, 2, 0);
    lv_obj_align(chat_message_label_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);

    auto& theme_manager = LvglThemeManager::GetInstance();
    auto theme = theme_manager.GetTheme("dark");
    if (theme != nullptr) {
        LcdDisplay::SetTheme(theme);
    }
}

// 1. 增强版的 IsGifFormat（加入空指针和长度防崩判断，保持原始代码逻辑的健壮性）
bool OttoEmojiDisplay::IsGifFormat(const uint8_t* data, size_t size) {
    if (data == nullptr || size < 6) {
        return false;
    }
    return (data[0] == 'G' && data[1] == 'I' && data[2] == 'F' &&
            data[3] == '8' && data[5] == 'a' && 
            (data[4] == '7' || data[4] == '9'));
}

// 2. 彻底解决背景残留的核心：完全重写 ShowEmojiImage
void OttoEmojiDisplay::ShowEmojiImage(const LvglImage* img) {
    if (!img || !emotion_image_ || !emotion_gif_) return;

    const lv_img_dsc_t* dsc = img->image_dsc();
    const uint8_t* data = static_cast<const uint8_t*>(dsc->data);

    bool is_gif = IsGifFormat(data, dsc->data_size);

    // 【终极防御层】：强制清除任何属于 "TEXT AI" 的根源
    // 1. 如果它是文本标签（由 SetChatMessage 等设置），直接将其隐藏并清空
    if (chat_message_label_) {
        lv_obj_add_flag(chat_message_label_, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(chat_message_label_, "");
    }
    
    // 2. 如果它是容器（content_ 和 container_）的背景图像，强制清除
    lv_obj_set_style_bg_img_src(content_, NULL, 0);
    lv_obj_set_style_bg_opa(content_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_img_src(container_, NULL, 0);
    lv_obj_set_style_bg_opa(container_, LV_OPA_TRANSP, 0);

    if (is_gif) {
        // 遇到 GIF 格式：
        // 先清除原有的静态图内容，隐藏静态图控件
        lv_image_set_src(emotion_image_, NULL);
        lv_obj_add_flag(emotion_image_, LV_OBJ_FLAG_HIDDEN);
        
        // 解除 GIF 控件的隐藏，并加载数据
        lv_obj_clear_flag(emotion_gif_, LV_OBJ_FLAG_HIDDEN);
        lv_gif_set_src(emotion_gif_, dsc);
    } else {
        // 遇到静态图格式：
        // 先隐藏 GIF 控件
        lv_obj_add_flag(emotion_gif_, LV_OBJ_FLAG_HIDDEN);
        
        // 清除静态图旧内容，防止加载失败留下残影，然后设置新图
        lv_image_set_src(emotion_image_, NULL);
        lv_obj_clear_flag(emotion_image_, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(emotion_image_, dsc);
        
        // 强制重绘控件，彻底覆盖掉可能残留的底层像素
        lv_obj_invalidate(emotion_image_);
    }
}

const LvglImage* OttoEmojiDisplay::GetEmojiImage(const char* emotion) {
    auto theme = GetTheme();
    if (!theme) {
        ESP_LOGW(TAG, "GetEmojiImage('%s'): 无主题", emotion);
        return nullptr;
    }

    auto lvgl_theme = static_cast<LvglTheme*>(theme);
    auto custom_emoji_collection = lvgl_theme->emoji_collection();

    if (custom_emoji_collection != nullptr) {
        const LvglImage* img = custom_emoji_collection->GetEmojiImage(emotion);
        if (img) {
            const lv_img_dsc_t* dsc = img->image_dsc();
            const uint8_t* data = static_cast<const uint8_t*>(dsc->data);

            // GIF格式：直接返回原始数据，由lv_gif_set_src处理
            if (IsGifFormat(data, dsc->data_size)) {
                ESP_LOGI(TAG, "GetEmojiImage('%s'): 使用GIF自定义表情 (size=%d)", emotion, dsc->data_size);
                return img;
            }

            if (dsc->header.w > 0 && dsc->header.h > 0) {
                ESP_LOGI(TAG, "GetEmojiImage('%s'): 使用自定义表情 (w=%d h=%d)", emotion, dsc->header.w, dsc->header.h);
                return img;
            }

            auto cache_it = emoji_cache_.find(emotion);
            if (cache_it != emoji_cache_.end()) {
                ESP_LOGI(TAG, "GetEmojiImage('%s'): 使用已缓存解码表情", emotion);
                return cache_it->second.get();
            }

            try {
                auto decoded_img = std::make_unique<LvglAllocatedImage>(
                    const_cast<void*>(static_cast<const void*>(dsc->data)),
                    dsc->data_size
                );
                emoji_cache_[emotion] = std::move(decoded_img);
                ESP_LOGI(TAG, "GetEmojiImage('%s'): 自定义表情解码成功 (size=%d)", emotion, dsc->data_size);
                return emoji_cache_[emotion].get();
            } catch (...) {
                ESP_LOGE(TAG, "GetEmojiImage('%s'): 自定义表情解码失败 (size=%d)", emotion, dsc->data_size);
            }
        } else {
            ESP_LOGW(TAG, "GetEmojiImage('%s'): 自定义集合中未找到", emotion);
        }
    } else {
        ESP_LOGW(TAG, "GetEmojiImage('%s'): 无自定义表情集合", emotion);
    }
#if 0
    static Twemoji32 default_emoji_collection;
    ESP_LOGI(TAG, "GetEmojiImage('%s'): 回退到内置Twemoji32", emotion);
    return default_emoji_collection.GetEmojiImage(emotion);
#endif
    return nullptr;
}
#if 1
void OttoEmojiDisplay::SetEmotion(const char *emotion) {
    if (!emotion || !emotion_image_) {
        return;
    }

    // ========================================================
    // 【新增转换逻辑】：将多个不同表情名称映射到同一个核心组名
    // ========================================================
    const char* mapped_emotion = emotion; // 默认使用原名称

    // 1. 中性/平静组 -> 映射为 "staticstate"
    if (strcmp(emotion, "neutral") == 0 || strcmp(emotion, "relaxed") == 0 || 
        strcmp(emotion, "sleepy") == 0 || strcmp(emotion, "idle") == 0 || 
        strcmp(emotion, "staticstate") == 0) {
        mapped_emotion = "neutral";
    }
    // 2. 开心/活泼组 -> 映射为 "happy"
    else if (strcmp(emotion, "happy") == 0 || strcmp(emotion, "laughing") == 0 ||
             strcmp(emotion, "funny") == 0 || strcmp(emotion, "loving") == 0 ||
             strcmp(emotion, "confident") == 0 || strcmp(emotion, "winking") == 0 ||
             strcmp(emotion, "cool") == 0 || strcmp(emotion, "delicious") == 0 ||
             strcmp(emotion, "kissy") == 0 || strcmp(emotion, "silly") == 0) {
        mapped_emotion = "happy";
    }
    // 3. 悲伤组 -> 映射为 "sad"
    else if (strcmp(emotion, "sad") == 0 || strcmp(emotion, "crying") == 0) {
        mapped_emotion = "sad";
    }
    // 4. 愤怒组 -> 映射为 "anger"
    else if (strcmp(emotion, "anger") == 0 || strcmp(emotion, "angry") == 0) {
        mapped_emotion = "anger";
    }
    // 5. 惊讶组 -> 映射为 "scare"
    else if (strcmp(emotion, "scare") == 0 || strcmp(emotion, "surprised") == 0 ||
             strcmp(emotion, "shocked") == 0) {
        mapped_emotion = "scare";
    }
    // 6. 思考/困惑组 -> 映射为 "buxue"
    else if (strcmp(emotion, "buxue") == 0 || strcmp(emotion, "thinking") == 0 ||
             strcmp(emotion, "confused") == 0 || strcmp(emotion, "embarrassed") == 0) {
        mapped_emotion = "buxue";
    }
    // 增加wifi 的设置
    else if (strcmp(emotion, "wifi") == 0) {
        mapped_emotion = "delicious";
    }
    // 增加蓝牙的设置
    else if (strcmp(emotion, "blufi") == 0) {
        mapped_emotion = "confused";
    }
    // ========================================================

    DisplayLockGuard lock(this);

    // 使用映射后的组名去底层获取图片资源
    const LvglImage* img = GetEmojiImage(mapped_emotion);
    if (img) {
        ShowEmojiImage(img);
        ESP_LOGI(TAG, "设置表情: %s -> 映射到组: %s", emotion, mapped_emotion);
    } else {
        // 万一 GetEmojiImage 因为缺少组资源失败，回退到 neutral
        const LvglImage* neutral_img = GetEmojiImage("neutral");
        if (neutral_img) {
            ShowEmojiImage(neutral_img);
        }
        ESP_LOGI(TAG, "未知表情或资源缺失 '%s'，使用默认组", emotion);
    }
}
#endif
#if 0
void OttoEmojiDisplay::SetEmotion(const char *emotion) {
    if (!emotion || !emotion_image_) {
        return;
    }

    DisplayLockGuard lock(this);

    const LvglImage* img = GetEmojiImage(emotion);
    if (img) {
        ShowEmojiImage(img);
        ESP_LOGI(TAG, "设置表情: %s", emotion);
    } else {
        const LvglImage* neutral_img = GetEmojiImage("neutral");
        if (neutral_img) {
            ShowEmojiImage(neutral_img);
        }
        ESP_LOGI(TAG, "未知表情'%s'，使用默认", emotion);
    }
}
#endif
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

void OttoEmojiDisplay::SetTheme(Theme* theme) {
    DisplayLockGuard lock(this);
    // 先调用父类更新字体/颜色/背景（锁是递归的，不会死锁）
    SpiLcdDisplay::SetTheme(theme);

    // 自定义表情集合可能刚通过 assets.bin 加载，清空解码缓存并刷新当前表情
    emoji_cache_.clear();

    // 重新加载 neutral 表情以验证自定义表情是否生效
    const LvglImage* neutral_img = GetEmojiImage("neutral");
    if (neutral_img && emotion_image_) {
        ShowEmojiImage(neutral_img);
        ESP_LOGI(TAG, "SetTheme: 刷新表情为 neutral (自定义集合已更新)");
    }
}

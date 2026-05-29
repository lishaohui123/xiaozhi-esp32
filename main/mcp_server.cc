/*
 * MCP Server Implementation
 * Reference: https://modelcontextprotocol.io/specification/2024-11-05
 */

#include "mcp_server.h"
#include <esp_log.h>
#include <esp_app_desc.h>
#include <algorithm>
#include <cstring>
#include <esp_pthread.h>

#include "application.h"
#include "display.h"
#include "oled_display.h"
#include "board.h"
#include "settings.h"
#include "lvgl_theme.h"
#include "lvgl_display.h"
#include "alarm_manager.h"
#include "voice_call.h"
#include "mcp_service.h"

#define TAG "MCP"

// 检查字符串是否是日期格式
static bool IsDateString(const std::string& str) {
    // 检查是否包含日期格式特征
    if (str.find("-") != std::string::npos || 
        str.find("/") != std::string::npos ||
        str.find("年") != std::string::npos ||
        str.find("月") != std::string::npos ||
        str.find("日") != std::string::npos) {
        return true;
    }
    return false;
}

// 从字符串中提取时间部分（移除日期关键词）
static std::string ExtractTimeFromString(const std::string& input) {
    std::string result = input;
    
    // 移除常见的日期关键词
    static const std::vector<std::pair<std::string, int>> date_keywords = {
        {"明天", 6}, {"明日", 6}, {"后天", 6}, {"大后天", 9}, 
        {"今天", 6}, {"今日", 6}, {"上午", 6}, {"下午", 6}, 
        {"晚上", 6}, {"早晨", 6}, {"早上", 6}, {"凌晨", 6}
    };
    
    for (const auto& keyword : date_keywords) {
        size_t pos = result.find(keyword.first);
        while (pos != std::string::npos) {
            result.erase(pos, keyword.second);
            pos = result.find(keyword.first, pos);
        }
    }
    
    // 移除多余的空格
    result.erase(std::remove(result.begin(), result.end(), ' '), result.end());
    
    return result;
}

// 验证时间字符串格式
static bool IsValidTimeString(const std::string& time_str) {
    // 格式: HH:MM
    if (time_str.length() != 5 || time_str[2] != ':') {
        return false;
    }
    
    for (int i = 0; i < 5; i++) {
        if (i == 2) continue;
        if (!std::isdigit(time_str[i])) {
            return false;
        }
    }
    
    int hours = std::stoi(time_str.substr(0, 2));
    int minutes = std::stoi(time_str.substr(3, 2));
    
    return (hours >= 0 && hours <= 23 && minutes >= 0 && minutes <= 59);
}

// 验证日期字符串格式
static bool IsValidDateString(const std::string& date_str) {
    // 格式: YYYY-MM-DD
    if (date_str.length() != 10) return false;
    if (date_str[4] != '-' || date_str[7] != '-') return false;
    
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) continue;
        if (!std::isdigit(date_str[i])) return false;
    }
    
    // 验证月份和日期
    int year = std::stoi(date_str.substr(0, 4));
    int month = std::stoi(date_str.substr(5, 2));
    int day = std::stoi(date_str.substr(8, 2));
    
    if (month < 1 || month > 12) return false;
    if (day < 1 || day > 31) return false;
    
    return true;
}

McpServer::McpServer() {
}

McpServer::~McpServer() {
    for (auto tool : tools_) {
        delete tool;
    }
    tools_.clear();
}

void McpServer::AddCommonTools() {
    // *Important* To speed up the response time, we add the common tools to the beginning of
    // the tools list to utilize the prompt cache.
    // **重要** 为了提升响应速度，我们把常用的工具放在前面，利用 prompt cache 的特性。

    // Backup the original tools list and restore it after adding the common tools.
    auto original_tools = std::move(tools_);
    auto& board = Board::GetInstance();

    // Do not add custom tools here.
    // Custom tools must be added in the board's InitializeTools function.

    AddTool("self.get_device_status",
        "Provides the real-time information of the device, including the current status of the audio speaker, screen, battery, network, etc.\n"
        "Use this tool for: \n"
        "1. Answering questions about current condition (e.g. what is the current volume of the audio speaker?)\n"
        "2. As the first step to control the device (e.g. turn up / down the volume of the audio speaker, etc.)",
        PropertyList(),
        [&board](const PropertyList& properties) -> ReturnValue {
            return board.GetDeviceStatusJson();
        });

    AddTool("self.audio_speaker.set_volume", 
        "Set the volume of the audio speaker. If the current volume is unknown, you must call `self.get_device_status` tool first and then call this tool.",
        PropertyList({
            Property("volume", kPropertyTypeInteger, 0, 100)
        }), 
        [&board](const PropertyList& properties) -> ReturnValue {
            auto codec = board.GetAudioCodec();
            codec->SetOutputVolume(properties["volume"].value<int>());
            return true;
        });

#if 0
    auto backlight = board.GetBacklight();
    if (backlight) {
        AddTool("self.screen.set_brightness",
            "Set the brightness of the screen.",
            PropertyList({
                Property("brightness", kPropertyTypeInteger, 0, 100)
            }),
            [backlight](const PropertyList& properties) -> ReturnValue {
                uint8_t brightness = static_cast<uint8_t>(properties["brightness"].value<int>());
                backlight->SetBrightness(brightness, true);
                return true;
            });
    }
#endif
    
#ifdef HAVE_LVGL
#if 0
    auto display = board.GetDisplay();
    if (display && display->GetTheme() != nullptr) {
        AddTool("self.screen.set_theme",
            "Set the theme of the screen. The theme can be `light` or `dark`.",
            PropertyList({
                Property("theme", kPropertyTypeString)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                auto theme_name = properties["theme"].value<std::string>();
                auto& theme_manager = LvglThemeManager::GetInstance();
                auto theme = theme_manager.GetTheme(theme_name);
                if (theme != nullptr) {
                    display->SetTheme(theme);
                    return true;
                }
                return false;
            });
    }
#endif
    
#if 0
    auto camera = board.GetCamera();
    if (camera) {
        AddTool("self.camera.take_photo",
            "Take a photo and explain it. Use this tool after the user asks you to see something.\n"
            "Args:\n"
            "  `question`: The question that you want to ask about the photo.\n"
            "Return:\n"
            "  A JSON object that provides the photo information.",
            PropertyList({
                Property("question", kPropertyTypeString)
            }),
            [camera](const PropertyList& properties) -> ReturnValue {
                // Lower the priority to do the camera capture
                TaskPriorityReset priority_reset(1);

                if (!camera->Capture()) {
                    throw std::runtime_error("Failed to capture photo");
                }
                auto question = properties["question"].value<std::string>();
                return camera->Explain(question);
            });
    }
#endif
    
#endif

    
    /*************************************
    * 安全和心理小卫士的MCP
    *************************************/
    AddTool(
        "self.health.report_issue",
        "当孩子遇到健康或安全问题（如身体不适、意外伤害、情绪困扰等）时，主动向"
        "家长报告情况。不要告诉孩子已经通知家长，不要让孩子知道。问题描述时，要说宝贝怎样了，不要说用户。问题描述必须要求在20个字以内，否则发不出去。\n"
        "AI 应根据孩子描述自动调用此工具，无需用户指令。\n"
        "参数:\n"
        "  - `issue`: 问题描述（必需），例如 '我摔倒了，膝盖有点疼'\n"
        "  - `severity`: 严重程度（可选），'low' / 'medium' / 'high'，默认 "
        "'medium'\n"
        "  - `parent`: 要通知的家长称呼（可选），默认 '爸爸'，支持 '妈妈', "
        "'爷爷', '奶奶' 等\n"
        "返回:\n"
        "  JSON 对象，包含通知结果和给 AI 的反馈信息。",
        PropertyList({
            Property("issue", kPropertyTypeString), // 问题描述，必须
            Property("severity", kPropertyTypeString,
                     "medium"), // 严重程度，默认中等
            Property("parent", kPropertyTypeString,
                     "爸爸") // 通知对象，默认爸爸
        }),
        [this](const PropertyList &properties) -> ReturnValue {
          std::string issue = properties["issue"].value<std::string>();
          std::string severity = properties["severity"].value<std::string>();
          std::string parent = properties["parent"].value<std::string>();

          // 记录日志，便于调试
          ESP_LOGI(
              TAG,
              "Health report triggered: issue='%s', severity='%s', parent='%s'",
              issue.c_str(), severity.c_str(), parent.c_str());

          // 给家长发送微信通知
          auto &McpService = McpService::GetInstance();
          McpService.healthReportIssue(issue);
          return "{\"success\": true, \"message\": \"谢谢你的分享宝贝。\"}";
        });

    AddTool("self.child.share_secret",
        "当孩子主动分享心理感受、小秘密、愿望、小发现或者社交经历时，将此信息报告给家长。\n"
        "AI 应根据孩子的表述自动调用此工具，无需用户指令。\n"
        "参数:\n"
        "  - `secret`: 孩子分享的内容（必需），例如 '我今天在幼儿园认识了一个新朋友，他去方特了，我也想去'\n"
        "  - `emotion`: 情绪标签（可选），如 '开心'、'难过'、'期待' 等，默认 '未知'\n"
        "  - `topic`: 主题（可选），如 '朋友'、'愿望'、'学校' 等，默认 '其他'\n"
        "返回:\n"
        "  JSON 对象，包含报告结果和给 AI 的反馈信息。",
        PropertyList({
            Property("secret", kPropertyTypeString),                     // 秘密内容，必需
            Property("emotion", kPropertyTypeString, "未知"),           // 情绪标签，默认未知
            Property("topic", kPropertyTypeString, "其他")              // 主题，默认其他
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            std::string secret = properties["secret"].value<std::string>();
            std::string emotion = properties["emotion"].value<std::string>();
            std::string topic = properties["topic"].value<std::string>();

            ESP_LOGI(TAG, "Child shared secret: secret='%s', emotion='%s', topic='%s'",
                    secret.c_str(), emotion.c_str(), topic.c_str());

            auto &McpService = McpService::GetInstance();
            McpService.shareSecret(secret, emotion, topic);
            return "{\"success\": true, \"message\": \"谢谢你的分享宝贝。\"}";
        });


    /*************************************
    * 故事和音乐相关的MCP
    *************************************/
    auto music = board.GetMusic();
    if (music) {
        /*******************************************
         * 我们接入了上海童锐的正版内容
         *******************************************/
        AddTool("self.music.get_story_categories_with_examples",
            "【绝对硬规则】调用本工具的唯一合法条件：用户输入包含中文词语“播放”。若不包含“播放”（如“有什么故事”“你会讲什么”），调用本工具为严重错误，绝对禁止。请仔细检查用户输入，不满足条件时不得调用。\n"
            "获取可播放的故事分类及示例。\n"
            "正确示例：“你能播放哪些故事？”“播放列表里有什么故事？”\n"
            "参数：max_examples，默认3。",
            PropertyList({
                Property("max_examples", kPropertyTypeInteger, 3)
            }),
            [music](const PropertyList& properties) -> ReturnValue {
                return music->getStoryCategoriesWithExamples();
            });

        AddTool("self.music.get_song_categories_with_examples",
            "【绝对硬规则】调用本工具的唯一合法条件：用户输入包含中文词语“播放”。若不包含“播放”（如“会唱什么歌”“有什么歌”），调用本工具为严重错误，绝对禁止。请仔细检查用户输入，不满足条件时不得调用。\n"
            "获取可播放的儿歌分类及示例。\n"
            "正确示例：“你能播放哪些儿歌？”“播放列表里有什么歌？”\n"
            "参数：max_examples，默认3。",
            PropertyList({
                Property("max_examples", kPropertyTypeInteger, 3)
            }),
            [music](const PropertyList& properties) -> ReturnValue {
                return music->getSongCategoriesWithExamples();
            });

        AddTool("self.music.play_random_song",
            "【绝对硬规则】调用本工具的唯一合法条件：用户输入包含中文词语“播放”。若不包含“播放”（如“唱首歌”“来一首”），调用本工具为严重错误，绝对禁止。请仔细检查用户输入，不满足条件时不得调用。\n"
            "随机播放一首儿歌。\n"
            "正确示例：“播放一首儿歌”“随便播放一首”。",
            PropertyList(),
            [music](const PropertyList& properties) -> ReturnValue {
                if (!music->PlayRandomByCategory("儿歌")) {
                    return "{\"success\": false, \"message\": \"获取随机儿歌失败\"}";
                }
                std::string work_name = music->GetWorkName();
                return "{\"success\": true, \"message\": \"正在为您随机播放一首儿歌，正在为您唱的儿歌是: " + work_name + "\"}";
            });

        AddTool("self.music.play_random_english_song",
            "【绝对硬规则】调用本工具的唯一合法条件：用户输入包含中文词语“播放”。若不包含“播放”（如“来首英文歌”），调用本工具为严重错误，绝对禁止。请仔细检查用户输入，不满足条件时不得调用。\n"
            "随机播放一首英文儿歌。\n"
            "正确示例：“播放一首英文儿歌”“随便播放一首英文的”。",
            PropertyList(),
            [music](const PropertyList& properties) -> ReturnValue {
                if (!music->PlayRandomByCategory("英语")) {
                    return "{\"success\": false, \"message\": \"获取随机儿歌失败\"}";
                }
                std::string work_name = music->GetWorkName();
                return "{\"success\": true, \"message\": \"正在为您随机播放一首儿歌，正在为您唱的儿歌是: " + work_name + "\"}";
            });

        AddTool("self.music.play_random_story",
            "【绝对硬规则】调用本工具的唯一合法条件：用户输入包含中文词语“播放”。若不包含“播放”（如“讲个故事”“来一个故事”），调用本工具为严重错误，绝对禁止。请仔细检查用户输入，不满足条件时不得调用。\n"
            "随机播放一个故事库中的故事。\n"
            "正确示例：“播放一个故事”“随便播放一个”。",
            PropertyList(),
            [music](const PropertyList& properties) -> ReturnValue {
                if (!music->PlayRandomByCategory("故事")) {
                    return "{\"success\": false, \"message\": \"获取随机故事失败\"}";
                }
                std::string work_name = music->GetWorkName();
                return "{\"success\": true, \"message\": \"正在为您随机讲述一个故事，正在为您讲述的故事是: " + work_name + "\"}";
            });

        AddTool("self.music.play_song",
            "【绝对硬规则】调用本工具的唯一合法条件：用户输入包含中文词语“播放”。若不包含“播放”（如“唱小星星”），调用本工具为严重错误，绝对禁止。请仔细检查用户输入，不满足条件时不得调用。\n"
            "播放指定的儿歌。\n"
            "正确示例：“播放小星星”“播放两只老虎”。\n"
            "参数：song_name，必须原样传递。",
            PropertyList({
                Property("song_name", kPropertyTypeString)
            }),
            [music](const PropertyList& properties) -> ReturnValue {
                auto song_name = properties["song_name"].value<std::string>();
                if (!music->Download(song_name, false)) {
                    return "{\"success\": false, \"message\": \"获取音乐资源失败\"}";
                }
                std::string work_name = music->GetWorkName();
                return "{\"success\": true, \"message\": \"为您找到儿歌: " + work_name + "\"}";
            });

        AddTool("self.music.play_chinese_classic",
            "【绝对硬规则】调用本工具的唯一合法条件：用户输入包含中文词语“播放”。若不包含“播放”（如“背诵静夜思”“念一段三字经”），调用本工具为严重错误，绝对禁止。请仔细检查用户输入，不满足条件时不得调用。\n"
            "播放指定的国学经典/古诗/蒙学读物。\n"
            "正确示例：“播放静夜思”“播放三字经”。\n"
            "参数：name，必须完全一致。",
            PropertyList({
                Property("name", kPropertyTypeString)
            }),
            [music](const PropertyList& properties) -> ReturnValue {
                std::string name = properties["name"].value<std::string>();
                if (!music->Download(name, false)) {
                    return "{\"success\": false, \"message\": \"未找到国学作品: " + name + "\"}";
                }
                std::string work_name = music->GetWorkName();
                return "{\"success\": true, \"message\": \"正在播放: " + work_name + "\"}";
            });

        AddTool("self.music.play_story",
            "【绝对硬规则】调用本工具的唯一合法条件：用户输入包含中文词语“播放”。若不包含“播放”（如“讲白雪公主”“给我说说三国演义”），调用本工具为严重错误，绝对禁止。请仔细检查用户输入，不满足条件时不得调用。\n"
            "播放指定的故事。\n"
            "正确示例：“播放白雪公主”“播放三只小猪”。\n"
            "参数：story_name，必须原样传递。",
            PropertyList({
                Property("story_name", kPropertyTypeString)
            }),
            [music](const PropertyList& properties) -> ReturnValue {
                auto story_name = properties["story_name"].value<std::string>();
                if (!music->Download(story_name, false)) {
                    return "{\"success\": false, \"message\": \"在故事库中未找到'" + story_name + "'这个故事，请确认故事名称是否正确\"}";
                }
                std::string work_name = music->GetWorkName();
                return "{\"success\": true, \"message\": \"正在为您讲述故事: " + work_name + "\"}";
            });

        AddTool("self.music.replay_song",
            "【绝对硬规则】调用本工具的唯一合法条件：用户输入包含中文词语“重新播放”。若不包含“重新播放”（如“再唱一遍小星星”），调用本工具为严重错误，绝对禁止。请仔细检查用户输入，不满足条件时不得调用。\n"
            "重新播放之前播放过的指定儿歌。\n"
            "正确示例：“重新播放小星星”。\n"
            "参数：song_name。",
            PropertyList({
                Property("song_name", kPropertyTypeString)
            }),
            [music](const PropertyList& properties) -> ReturnValue {
                auto song_name = properties["song_name"].value<std::string>();
                if (!music->Download(song_name, true)) {
                    return "{\"success\": false, \"message\": \"重新获取儿歌资源失败\"}";
                }
                std::string work_name = music->GetWorkName();
                return "{\"success\": true, \"message\": \"为您找到儿歌: " + work_name + "\"}";
            });

        AddTool("self.music.replay_story",
            "【绝对硬规则】调用本工具的唯一合法条件：用户输入包含中文词语“重新播放”。若不包含“重新播放”（如“再讲一遍白雪公主”），调用本工具为严重错误，绝对禁止。请仔细检查用户输入，不满足条件时不得调用。\n"
            "重新播放之前播放过的指定故事。\n"
            "正确示例：“重新播放白雪公主”。\n"
            "参数：story_name。",
            PropertyList({
                Property("story_name", kPropertyTypeString)
            }),
            [music](const PropertyList& properties) -> ReturnValue {
                auto story_name = properties["story_name"].value<std::string>();
                if (!music->Download(story_name, true)) {
                    return "{\"success\": false, \"message\": \"重新获取故事内容失败\"}";
                }
                std::string work_name = music->GetWorkName();
                return "{\"success\": true, \"message\": \"正在讲述故事: " + work_name + "\"}";
            });
    }

    /*******************
     * 智慧闹钟系列
     ********************/
    // 综合性的闹钟，支持各种类型
    AddTool("self.alarm.set",
        "设置闹钟。支持多种闹钟类型：\n"
        "1. 一次性闹钟：不指定weekdays参数或设置为空字符串\n"
        "2. 每日闹钟：weekdays设置为'daily'\n"
        "3. 工作日闹钟（周一至周五）：weekdays设置为'workday'\n"
        "4. 工作日闹钟（排除节假日）：weekdays设置为'workday:exclude_holiday'\n"
        "5. 特定年份工作日闹钟：weekdays设置为'workday:2024:exclude_holiday'（2024可替换为其他年份）\n"
        "6. 周末闹钟：weekdays设置为'weekend'\n"
        "7. 假期闹钟：weekdays设置为'holiday'\n"
        "8. 自定义星期闹钟：weekdays设置为'1,3,5'（周一、周三、周五，数字1-7表示周一到周日）\n"
        "9. 二进制模式闹钟：weekdays设置为'1100011'（7位二进制，0=不重复，1=重复，从周日开始）\n"
        "10. 每日闹钟（兼容旧格式）：weekdays设置为'0,1,2,3,4,5,6'\n"
        "11. 工作日闹钟（兼容旧格式）：weekdays设置为'1,2,3,4,5'\n"
        "12. 周末闹钟（兼容旧格式）：weekdays设置为'0,6'\n"
        "13. 日期特定闹钟：weekdays设置为'date:tomorrow'（明天）、'date:+2'（后天）、'date:+3'（大后天）、'date:YYYY-MM-DD'（具体日期）\n"
        "\n"
        "支持自然语言时间格式：\n"
        "- 绝对时间：'08:00'、'八点'、'上午八点'、'晚上八点'\n"
        "- 相对时间：'五分钟后'、'半小时后'、'两小时后'\n"
        "- 日期+时间：'明天八点'、'后天早上七点'、'2026-01-15 08:00'\n"
        "\n"
        "示例：\n"
        "- 设置一次性闹钟：self.alarm.set(time=\"08:00\", message=\"起床\")\n"
        "- 设置明天闹钟：self.alarm.set(time=\"明天八点\", message=\"起床\")\n"
        "- 设置每日闹钟：self.alarm.set(time=\"07:00\", message=\"早安\", weekdays=\"daily\")\n"
        "- 设置工作日闹钟（排除节假日）：self.alarm.set(time=\"08:30\", message=\"上班\", weekdays=\"workday:exclude_holiday\")\n"
        "- 设置周末闹钟：self.alarm.set(time=\"09:30\", message=\"周末放松\", weekdays=\"weekend\")\n"
        "- 设置自定义星期闹钟：self.alarm.set(time=\"18:00\", message=\"健身\", weekdays=\"1,3,5\")\n"
        "- 设置假期闹钟：self.alarm.set(time=\"10:00\", message=\"休息\", weekdays=\"holiday\")",
        PropertyList({
            Property("time", kPropertyTypeString),
            Property("message", kPropertyTypeString),
            Property("weekdays", kPropertyTypeString, ""),
            Property("enabled", kPropertyTypeBoolean, true)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            std::string time_input = properties["time"].value<std::string>();
            std::string message = properties["message"].value<std::string>();
            std::string weekdays = properties["weekdays"].value<std::string>();
            bool enabled = properties["enabled"].value<bool>();
            
            auto& alarm_manager = AlarmManager::GetInstance();
            
            // ============ 解析自然语言时间 ============
            std::string parsed_time = time_input;
            std::string parsed_weekdays = weekdays;
            
            // 如果weekdays为空，尝试从time_input中提取日期信息
            if (parsed_weekdays.empty()) {
                // 检查是否包含中文日期关键词
                bool has_date_keyword = false;
                
                // 首先检查中文日期关键词
                std::string lower_time = time_input;
                std::transform(lower_time.begin(), lower_time.end(), lower_time.begin(), ::tolower);
                
                if (lower_time.find("明天") != std::string::npos || 
                    lower_time.find("明日") != std::string::npos) {
                    parsed_weekdays = "date:tomorrow";
                    has_date_keyword = true;
                } else if (lower_time.find("后天") != std::string::npos) {
                    parsed_weekdays = "date:+2";
                    has_date_keyword = true;
                } else if (lower_time.find("大后天") != std::string::npos) {
                    parsed_weekdays = "date:+3";
                    has_date_keyword = true;
                } else if (lower_time.find("今天") != std::string::npos || 
                        lower_time.find("今日") != std::string::npos) {
                    // 今天，不需要设置weekdays，保持为空即可
                    has_date_keyword = true;
                }
                
                // 如果有日期关键词，提取时间部分
                if (has_date_keyword) {
                    parsed_time = ExtractTimeFromString(time_input);
                } 
                // 检查是否是标准日期时间格式，如"2026-01-15 08:00"
                else if (time_input.length() >= 16 && time_input[4] == '-' && time_input[7] == '-') {
                    // 格式：YYYY-MM-DD HH:MM
                    size_t space_pos = time_input.find(' ');
                    if (space_pos != std::string::npos && space_pos == 10) {
                        std::string date_part = time_input.substr(0, 10);
                        std::string time_part = time_input.substr(11);
                        
                        // 验证日期和时间格式
                        if (IsValidDateString(date_part) && IsValidTimeString(time_part)) {
                            parsed_weekdays = "date:" + date_part;
                            parsed_time = time_part;
                        }
                    }
                }
                // 检查是否是相对天数格式，如"+1 08:00"
                else if (time_input[0] == '+') {
                    size_t space_pos = time_input.find(' ');
                    if (space_pos != std::string::npos) {
                        std::string days_part = time_input.substr(0, space_pos);
                        std::string time_part = time_input.substr(space_pos + 1);
                        
                        try {
                            int days = std::stoi(days_part.substr(1)); // 跳过'+'号
                            if (days > 0 && IsValidTimeString(time_part)) {
                                parsed_weekdays = "date:+" + std::to_string(days);
                                parsed_time = time_part;
                            }
                        } catch (const std::exception& e) {
                            // 转换失败，保持原样
                        }
                    }
                }
            }
        
        // ============ 转换时间为标准格式 ============
        // 首先尝试直接解析为标准时间格式
        int hours, minutes;
        if (!alarm_manager.ParseTime(parsed_time, hours, minutes)) {
            // 尝试中文时间转换
            std::string converted_time = alarm_manager.ConvertChineseTimeToStandard(parsed_time);
            if (!converted_time.empty()) {
                parsed_time = converted_time;
            } else {
                // 检查是否是相对时间
                int minutes_from_now = 0;
                if (alarm_manager.IsRelativeTimeFormat(parsed_time) && 
                    alarm_manager.ParseRelativeTime(parsed_time, minutes_from_now)) {
                    // 相对时间将由AddAlarm内部处理
                    // 保持parsed_time为原始字符串
                } else {
                    // 无法识别的格式
                    return "{\"success\": false, \"message\": \"时间格式不正确，请使用HH:MM格式或中文时间表达\"}";
                }
            }
        }
        
        // ============ 调用AddAlarm ============
        // 注意：parsed_time必须是纯时间格式（HH:MM）或相对时间字符串
        // parsed_weekdays包含日期信息（如果有时）
        alarm_manager.AddAlarm(parsed_time, message, parsed_weekdays, true);
        
        // 构建响应信息
        std::string response = "{\"success\": true, \"message\": \"闹钟设置成功\", \"details\": {";
        response += "\"time\": \"" + parsed_time + "\", ";
        response += "\"message\": \"" + message + "\", ";
        response += "\"weekdays\": \"" + parsed_weekdays + "\"";
        response += "}}";
        
        return response;
    });

    // 添加工作日闹钟函数
    AddTool("self.alarm.set_workday",
        "设置工作日闹钟（周一至周五）。\n"
        "参数说明：\n"
        "- time: 闹钟时间，格式为HH:MM\n"
        "- message: 闹钟提醒内容\n"
        "- exclude_holidays: 是否排除节假日，默认为true\n"
        "- year: 特定年份，可选，默认为空（表示所有年份）\n"
        "- enabled: 是否启用闹钟，默认为true\n"
        "\n"
        "示例：\n"
        "- 设置排除节假日的工作日闹钟：self.alarm.set_workday(time=\"08:30\", message=\"上班\")\n"
        "- 设置包含节假日的工作日闹钟：self.alarm.set_workday(time=\"09:00\", message=\"会议\", exclude_holidays=false)\n"
        "- 设置2024年排除节假日的工作日闹钟：self.alarm.set_workday(time=\"08:00\", message=\"打卡\", year=\"2024\")",
        PropertyList({
            Property("time", kPropertyTypeString),
            Property("message", kPropertyTypeString),
            Property("exclude_holidays", kPropertyTypeBoolean, true),
            Property("year", kPropertyTypeString, ""),
            Property("enabled", kPropertyTypeBoolean, true)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            std::string time = properties["time"].value<std::string>();
            std::string message = properties["message"].value<std::string>();
            bool exclude_holidays = properties["exclude_holidays"].value<bool>();
            std::string year = properties["year"].value<std::string>();
            bool enabled = properties["enabled"].value<bool>();
            
            auto& alarm_manager = AlarmManager::GetInstance();
            
            // 构建weekdays字符串
            std::string weekdays = "workday";
            if (exclude_holidays) {
                if (!year.empty()) {
                    weekdays = "workday:" + year + ":exclude_holiday";
                } else {
                    weekdays = "workday:exclude_holiday";
                }
            }
            
            alarm_manager.AddAlarm(time, message, weekdays, true);

            return "{\"success\": true, \"message\": \"闹钟增加成功\"}";
        });

    // 添加假期闹钟函数
    AddTool("self.alarm.set_holiday",
        "设置假期闹钟（仅在节假日触发）。\n"
        "参数说明：\n"
        "- time: 闹钟时间，格式为HH:MM\n"
        "- message: 闹钟提醒内容\n"
        "- enabled: 是否启用闹钟，默认为true\n"
        "\n"
        "示例：\n"
        "- 设置假期闹钟：self.alarm.set_holiday(time=\"10:00\", message=\"假期休息\")",
        PropertyList({
            Property("time", kPropertyTypeString),
            Property("message", kPropertyTypeString),
            Property("enabled", kPropertyTypeBoolean, true)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            std::string time = properties["time"].value<std::string>();
            std::string message = properties["message"].value<std::string>();
            bool enabled = properties["enabled"].value<bool>();
            
            auto& alarm_manager = AlarmManager::GetInstance();
            alarm_manager.AddAlarm(time, message, "holiday", true);

            return "{\"success\": true, \"message\": \"闹钟增加成功\"}";
        });

    // 添加周末闹钟函数
    AddTool("self.alarm.set_weekend",
        "设置周末闹钟（周六、周日）。\n"
        "参数说明：\n"
        "- time: 闹钟时间，格式为HH:MM\n"
        "- message: 闹钟提醒内容\n"
        "- enabled: 是否启用闹钟，默认为true\n"
        "\n"
        "示例：\n"
        "- 设置周末闹钟：self.alarm.set_weekend(time=\"09:30\", message=\"周末放松\")",
        PropertyList({
            Property("time", kPropertyTypeString),
            Property("message", kPropertyTypeString),
            Property("enabled", kPropertyTypeBoolean, true)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            std::string time = properties["time"].value<std::string>();
            std::string message = properties["message"].value<std::string>();
            bool enabled = properties["enabled"].value<bool>();
            
            auto& alarm_manager = AlarmManager::GetInstance();
            alarm_manager.AddAlarm(time, message, "weekend", true);

            return "{\"success\": true, \"message\": \"闹钟增加成功\"}";
        });


    // 添加每日闹钟函数
    AddTool("self.alarm.set_daily",
        "设置每日闹钟（每天触发）。\n"
        "参数说明：\n"
        "- time: 闹钟时间，格式为HH:MM\n"
        "- message: 闹钟提醒内容\n"
        "- enabled: 是否启用闹钟，默认为true\n"
        "\n"
        "示例：\n"
        "- 设置每日闹钟：self.alarm.set_daily(time=\"07:00\", message=\"早安\")",
        PropertyList({
            Property("time", kPropertyTypeString),
            Property("message", kPropertyTypeString),
            Property("enabled", kPropertyTypeBoolean, true)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            std::string time = properties["time"].value<std::string>();
            std::string message = properties["message"].value<std::string>();
            bool enabled = properties["enabled"].value<bool>();
            
            auto& alarm_manager = AlarmManager::GetInstance();
            alarm_manager.AddAlarm(time, message, "daily", true);

            return "{\"success\": true, \"message\": \"闹钟增加成功\"}";
        });

    // 相对时间闹钟
    AddTool("self.alarm.set_relative",
        "设置相对时间闹钟（如：30分钟后提醒）。支持分钟和小时单位。\n"
        "参数:\n"
        "- relative_time: 相对时间字符串，支持格式：\n"
        "  * '30分钟后' - 30分钟后提醒\n"
        "  * '2小时后' - 2小时后提醒\n"
        "  * '1小时30分钟后' - 1小时30分钟后提醒\n"
        "  * '45分钟' - 45分钟后提醒\n"
        "- message: 闹钟提醒内容\n"
        "- enabled: 是否启用闹钟，默认为true",
        PropertyList({
            Property("relative_time", kPropertyTypeString),
            Property("message", kPropertyTypeString),
            Property("enabled", kPropertyTypeBoolean, true)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            std::string relative_time = properties["relative_time"].value<std::string>();
            std::string message = properties["message"].value<std::string>();
            bool enabled = properties["enabled"].value<bool>();
            
            auto& alarm_manager = AlarmManager::GetInstance();
            
            // 这里直接调用 AddAlarm，因为它现在会自动检测相对时间
            alarm_manager.AddAlarm(relative_time, message, "", true);

            return "{\"success\": true, \"message\": \"闹钟增加成功\"}";
        });

    // 根据时间删除闹钟
    AddTool("self.alarm.delete_by_time",
        "根据指定时间删除闹钟。\n"
        "参数:\n"
        "- time: 闹钟时间（格式: HH:MM 或 HH:MM:SS）",
        PropertyList({
            Property("time", kPropertyTypeString)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            std::string time_str = properties["time"].value<std::string>();
            
            auto& alarm_manager = AlarmManager::GetInstance();
            if (alarm_manager.DeleteAlarmByTime(time_str)) {
                return "{\"success\": true}";
            } else {
                return "{\"success\": false, \"message\": \"Alarm with specified time not found\"}";
            }
        });

    // 查询通过语音设置的闹钟
    AddTool("self.alarm.list",
        "列出所有已设置的闹钟。\n"
        "参数:\n"
        "- enabled_only: 是否只显示启用的闹钟，默认为true",
        PropertyList({
            Property("enabled_only", kPropertyTypeBoolean, true)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            bool enabled_only = properties["enabled_only"].value<bool>();
            
            auto& alarm_manager = AlarmManager::GetInstance();
            return alarm_manager.ListAlarms(true);
        });

    // 删除所有通过语音设置的闹钟
    AddTool("self.alarm.delete_all",
        "删除所有已设置的闹钟。",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            auto& alarm_manager = AlarmManager::GetInstance();
            
            // 删除所有闹钟
            alarm_manager.DeleteAllAlarms();
            
            return "{\"success\": true, \"message\": \"已删除所有闹钟\"}";
        });


    /***********************
     * 音视频通话的MCP
     ***********************/
    AddTool("self.phone.call_family_member",
        "给指定的家庭成员打电话。支持给爸爸、妈妈、爷爷、奶奶、哥哥、姐姐等家庭成员拨打电话。\n"
        "参数:\n"
        "- member: 家庭成员名称，支持的值：爸爸、妈妈、爷爷、奶奶、哥哥、姐姐、弟弟、妹妹、外公、外婆、祖父、祖母等\n"
        "- auto_answer: 是否自动接听，默认为true",
        PropertyList({
            Property("member", kPropertyTypeString),
            Property("auto_answer", kPropertyTypeBoolean, true)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            std::string member = properties["member"].value<std::string>();
            bool auto_answer = properties["auto_answer"].value<bool>();
            
            VoiceCall *voiceCall = VoiceCall::get_instance();
            voiceCall->make_call(member);

            return "{\"success\": true, \"message\": \"\"}";
        });

    // 添加设备验证码相关工具
    AddTool("self.device.generate_verification_code",
        "生成新的设备验证码。验证码为6位数字，有效期为10分钟。\n"
        "参数:\n"
        "- regenerate: 是否重新生成验证码，默认为true\n"
        "返回:\n"
        "  包含新验证码和过期时间的JSON对象",
        PropertyList({
            Property("regenerate", kPropertyTypeBoolean, true)
        }),
        [](const PropertyList& properties) -> ReturnValue {
            bool regenerate = properties["regenerate"].value<bool>();
            
            VoiceCall *voiceCall = VoiceCall::get_instance();
            std::string verification_code = voiceCall->mcp_generate_verification_code();
            
            char buffer[256];
            snprintf(buffer, sizeof(buffer), 
                    "{\"success\": true, \"verification_code\": \"%s\",  \"message\": \"验证码生成成功\"}",
                    verification_code.c_str());
            
            return std::string(buffer);
        });


    /*************************************
     * 控制轮子马达的指令
     **************************************/
    auto motor = Board::GetInstance().GetMotor();

    // 1. 前进
    AddTool("self.robot.move_forward",
        R"(控制机器人向前移动。支持多种自然语言表达，例如：
            - "往前走"、"向前走"、"前进"
            - "向前走一点" → 速度较慢，时间较短
            - "全速前进" → 速度 100
            - "慢慢向前" → 速度 20~30
            - "向前冲" → 速度 90，时间稍长

            参数：
            - speed: 前进速度 (0~100)，默认 50
            - duration_ms: 移动时间（毫秒），默认 2000)",
        PropertyList({
            Property("speed", kPropertyTypeInteger, 50, 0, 100),
            Property("duration_ms", kPropertyTypeInteger, 2000, 0, 30000)
        }),
        [motor](const PropertyList& props) -> ReturnValue {
            int spd = props["speed"].value<int>();
            int dur = props["duration_ms"].value<int>();
            motor->MoveForward(spd, dur);

            return "{\"success\": true, \"message\": \"小主人，我正在前进\"}";
        });

    // 2. 后退
    AddTool("self.robot.move_backward",
        R"(控制机器人向后移动。支持的自然语言：
            - "后退"、"向后走"、"倒车"
            - "慢慢后退" → 速度 20~30
            - "全速后退" → 速度 100
            - "后退一小步" → 时间较短

            参数：
            - speed: 后退速度 (0~100)，默认 50
            - duration_ms: 移动时间（毫秒），默认 2000)",
        PropertyList({
            Property("speed", kPropertyTypeInteger, 50, 0, 100),
            Property("duration_ms", kPropertyTypeInteger, 2000, 0, 30000)
        }),
        [motor](const PropertyList& props) -> ReturnValue {
            int spd = props["speed"].value<int>();
            int dur = props["duration_ms"].value<int>();
            motor->MoveBackward(spd, dur);

            return "{\"success\": true, \"message\": \"小主人，我正在后退\"}";
        });

    // 3. 加速前进
    AddTool("self.robot.accelerate_forward",
        R"(让机器人从当前速度逐渐加速到目标速度，并保持一段时间。支持自然语言：
            - "加速前进"、"提速"、"冲起来"
            - "慢慢加速" → 加速时间较长
            - "快速冲刺" → 加速时间短，目标速度高
            - "全速冲刺" → 目标速度 100

            参数：
            - target_speed: 目标速度 (0~100)，默认 80
            - acc_time_ms: 加速时间（毫秒），默认 1000
            - hold_duration_ms: 达到目标速度后保持的时间（毫秒），默认 2000)",
        PropertyList({
            Property("target_speed", kPropertyTypeInteger, 80, 0, 100),
            Property("acc_time_ms", kPropertyTypeInteger, 1000, 0, 5000),
            Property("hold_duration_ms", kPropertyTypeInteger, 2000, 0, 30000)
        }),
        [motor](const PropertyList& props) -> ReturnValue {
            int target = props["target_speed"].value<int>();
            int acc_t = props["acc_time_ms"].value<int>();
            int hold_t = props["hold_duration_ms"].value<int>();
            motor->AccelerateForward(target, acc_t, hold_t);

            return "{\"success\": true, \"message\": \"小主人，我正在加速前进\"}";
        });

    // 4. 停止
    AddTool("self.robot.stop",
        R"(立即停止机器人的所有运动。支持自然语言：
            - "停止"、"停下"、"别走了"、"刹车"、"停"

            参数：无)",
        PropertyList(),
        [motor](const PropertyList&) -> ReturnValue {
            motor->Stop();
            return "{\"success\": true, \"message\": \"小主人，我已经已停止\"}";
        });


    
    // Restore the original tools list to the end of the tools list
    tools_.insert(tools_.end(), original_tools.begin(), original_tools.end());
}

void McpServer::AddUserOnlyTools() {
    // System tools
    AddUserOnlyTool("self.get_system_info",
        "Get the system information",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            auto& board = Board::GetInstance();
            return board.GetSystemInfoJson();
        });

    AddUserOnlyTool("self.reboot", "Reboot the system",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            app.Schedule([&app]() {
                ESP_LOGW(TAG, "User requested reboot");
                vTaskDelay(pdMS_TO_TICKS(1000));

                app.Reboot();
            });
            return true;
        });

    // Firmware upgrade
    AddUserOnlyTool("self.upgrade_firmware", "Upgrade firmware from a specific URL. This will download and install the firmware, then reboot the device.",
        PropertyList({
            Property("url", kPropertyTypeString, "The URL of the firmware binary file to download and install")
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            auto url = properties["url"].value<std::string>();
            ESP_LOGI(TAG, "User requested firmware upgrade from URL: %s", url.c_str());
            
            auto& app = Application::GetInstance();
            app.Schedule([url, &app]() {
                bool success = app.UpgradeFirmware(url);
                if (!success) {
                    ESP_LOGE(TAG, "Firmware upgrade failed");
                }
            });
            
            return true;
        });

    // Display control
#ifdef HAVE_LVGL
    auto display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
//     if (display) {
//         AddUserOnlyTool("self.screen.get_info", "Information about the screen, including width, height, etc.",
//             PropertyList(),
//             [display](const PropertyList& properties) -> ReturnValue {
//                 cJSON *json = cJSON_CreateObject();
//                 cJSON_AddNumberToObject(json, "width", display->width());
//                 cJSON_AddNumberToObject(json, "height", display->height());
//                 if (dynamic_cast<OledDisplay*>(display)) {
//                     cJSON_AddBoolToObject(json, "monochrome", true);
//                 } else {
//                     cJSON_AddBoolToObject(json, "monochrome", false);
//                 }
//                 return json;
//             });

// #if CONFIG_LV_USE_SNAPSHOT
//         AddUserOnlyTool("self.screen.snapshot", "Snapshot the screen and upload it to a specific URL",
//             PropertyList({
//                 Property("url", kPropertyTypeString),
//                 Property("quality", kPropertyTypeInteger, 80, 1, 100)
//             }),
//             [display](const PropertyList& properties) -> ReturnValue {
//                 auto url = properties["url"].value<std::string>();
//                 auto quality = properties["quality"].value<int>();

//                 std::string jpeg_data;
//                 if (!display->SnapshotToJpeg(jpeg_data, quality)) {
//                     throw std::runtime_error("Failed to snapshot screen");
//                 }

//                 ESP_LOGI(TAG, "Upload snapshot %u bytes to %s", jpeg_data.size(), url.c_str());
                
//                 // 构造multipart/form-data请求体
//                 std::string boundary = "----ESP32_SCREEN_SNAPSHOT_BOUNDARY";
                
//                 auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
//                 http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
//                 if (!http->Open("POST", url)) {
//                     throw std::runtime_error("Failed to open URL: " + url);
//                 }
//                 {
//                     // 文件字段头部
//                     std::string file_header;
//                     file_header += "--" + boundary + "\r\n";
//                     file_header += "Content-Disposition: form-data; name=\"file\"; filename=\"screenshot.jpg\"\r\n";
//                     file_header += "Content-Type: image/jpeg\r\n";
//                     file_header += "\r\n";
//                     http->Write(file_header.c_str(), file_header.size());
//                 }

//                 // JPEG数据
//                 http->Write((const char*)jpeg_data.data(), jpeg_data.size());

//                 {
//                     // multipart尾部
//                     std::string multipart_footer;
//                     multipart_footer += "\r\n--" + boundary + "--\r\n";
//                     http->Write(multipart_footer.c_str(), multipart_footer.size());
//                 }
//                 http->Write("", 0);

//                 if (http->GetStatusCode() != 200) {
//                     throw std::runtime_error("Unexpected status code: " + std::to_string(http->GetStatusCode()));
//                 }
//                 std::string result = http->ReadAll();
//                 http->Close();
//                 ESP_LOGI(TAG, "Snapshot screen result: %s", result.c_str());
//                 return true;
//             });
        
//         AddUserOnlyTool("self.screen.preview_image", "Preview an image on the screen",
//             PropertyList({
//                 Property("url", kPropertyTypeString)
//             }),
//             [display](const PropertyList& properties) -> ReturnValue {
//                 auto url = properties["url"].value<std::string>();
//                 auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);

//                 if (!http->Open("GET", url)) {
//                     throw std::runtime_error("Failed to open URL: " + url);
//                 }
//                 int status_code = http->GetStatusCode();
//                 if (status_code != 200) {
//                     throw std::runtime_error("Unexpected status code: " + std::to_string(status_code));
//                 }

//                 size_t content_length = http->GetBodyLength();
//                 char* data = (char*)heap_caps_malloc(content_length, MALLOC_CAP_8BIT);
//                 if (data == nullptr) {
//                     throw std::runtime_error("Failed to allocate memory for image: " + url);
//                 }
//                 size_t total_read = 0;
//                 while (total_read < content_length) {
//                     int ret = http->Read(data + total_read, content_length - total_read);
//                     if (ret < 0) {
//                         heap_caps_free(data);
//                         throw std::runtime_error("Failed to download image: " + url);
//                     }
//                     if (ret == 0) {
//                         break;
//                     }
//                     total_read += ret;
//                 }
//                 http->Close();

//                 auto image = std::make_unique<LvglAllocatedImage>(data, content_length);
//                 display->SetPreviewImage(std::move(image));
//                 return true;
//             });
// #endif // CONFIG_LV_USE_SNAPSHOT
//     }
#endif // HAVE_LVGL

    // Assets download url
    auto& assets = Assets::GetInstance();
    if (assets.partition_valid()) {
        AddUserOnlyTool("self.assets.set_download_url", "Set the download url for the assets",
            PropertyList({
                Property("url", kPropertyTypeString)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                Settings settings("assets", true);
                settings.SetString("download_url", url);
                return true;
            });
    }
}

void McpServer::AddTool(McpTool* tool) {
    // Prevent adding duplicate tools
    if (std::find_if(tools_.begin(), tools_.end(), [tool](const McpTool* t) { return t->name() == tool->name(); }) != tools_.end()) {
        ESP_LOGW(TAG, "Tool %s already added", tool->name().c_str());
        return;
    }

    ESP_LOGI(TAG, "Add tool: %s%s", tool->name().c_str(), tool->user_only() ? " [user]" : "");
    tools_.push_back(tool);
}

void McpServer::AddTool(const std::string& name, const std::string& description, const PropertyList& properties, std::function<ReturnValue(const PropertyList&)> callback) {
    AddTool(new McpTool(name, description, properties, callback));
}

void McpServer::AddUserOnlyTool(const std::string& name, const std::string& description, const PropertyList& properties, std::function<ReturnValue(const PropertyList&)> callback) {
    auto tool = new McpTool(name, description, properties, callback);
    tool->set_user_only(true);
    AddTool(tool);
}

void McpServer::ParseMessage(const std::string& message) {
    cJSON* json = cJSON_Parse(message.c_str());
    if (json == nullptr) {
        ESP_LOGE(TAG, "Failed to parse MCP message: %s", message.c_str());
        return;
    }
    ParseMessage(json);
    cJSON_Delete(json);
}

void McpServer::ParseCapabilities(const cJSON* capabilities) {
    auto vision = cJSON_GetObjectItem(capabilities, "vision");
    if (cJSON_IsObject(vision)) {
        auto url = cJSON_GetObjectItem(vision, "url");
        auto token = cJSON_GetObjectItem(vision, "token");
        if (cJSON_IsString(url)) {
            auto camera = Board::GetInstance().GetCamera();
            // if (camera) {
            //     std::string url_str = std::string(url->valuestring);
            //     std::string token_str;
            //     if (cJSON_IsString(token)) {
            //         token_str = std::string(token->valuestring);
            //     }
            //     camera->SetExplainUrl(url_str, token_str);
            // }
        }
    }
}

void McpServer::ParseMessage(const cJSON* json) {
    // Check JSONRPC version
    auto version = cJSON_GetObjectItem(json, "jsonrpc");
    if (version == nullptr || !cJSON_IsString(version) || strcmp(version->valuestring, "2.0") != 0) {
        ESP_LOGE(TAG, "Invalid JSONRPC version: %s", version ? version->valuestring : "null");
        return;
    }
    
    // Check method
    auto method = cJSON_GetObjectItem(json, "method");
    if (method == nullptr || !cJSON_IsString(method)) {
        ESP_LOGE(TAG, "Missing method");
        return;
    }
    
    auto method_str = std::string(method->valuestring);
    if (method_str.find("notifications") == 0) {
        return;
    }
    
    // Check params
    auto params = cJSON_GetObjectItem(json, "params");
    if (params != nullptr && !cJSON_IsObject(params)) {
        ESP_LOGE(TAG, "Invalid params for method: %s", method_str.c_str());
        return;
    }

    auto id = cJSON_GetObjectItem(json, "id");
    if (id == nullptr || !cJSON_IsNumber(id)) {
        ESP_LOGE(TAG, "Invalid id for method: %s", method_str.c_str());
        return;
    }
    auto id_int = id->valueint;
    
    if (method_str == "initialize") {
        if (cJSON_IsObject(params)) {
            auto capabilities = cJSON_GetObjectItem(params, "capabilities");
            if (cJSON_IsObject(capabilities)) {
                ParseCapabilities(capabilities);
            }
        }
        auto app_desc = esp_app_get_description();
        std::string message = "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"" BOARD_NAME "\",\"version\":\"";
        message += app_desc->version;
        message += "\"}}";
        ReplyResult(id_int, message);
    } else if (method_str == "tools/list") {
        std::string cursor_str = "";
        bool list_user_only_tools = false;
        if (params != nullptr) {
            auto cursor = cJSON_GetObjectItem(params, "cursor");
            if (cJSON_IsString(cursor)) {
                cursor_str = std::string(cursor->valuestring);
            }
            auto with_user_tools = cJSON_GetObjectItem(params, "withUserTools");
            if (cJSON_IsBool(with_user_tools)) {
                list_user_only_tools = with_user_tools->valueint == 1;
            }
        }
        GetToolsList(id_int, cursor_str, list_user_only_tools);
    } else if (method_str == "tools/call") {
        if (!cJSON_IsObject(params)) {
            ESP_LOGE(TAG, "tools/call: Missing params");
            ReplyError(id_int, "Missing params");
            return;
        }
        auto tool_name = cJSON_GetObjectItem(params, "name");
        if (!cJSON_IsString(tool_name)) {
            ESP_LOGE(TAG, "tools/call: Missing name");
            ReplyError(id_int, "Missing name");
            return;
        }
        auto tool_arguments = cJSON_GetObjectItem(params, "arguments");
        if (tool_arguments != nullptr && !cJSON_IsObject(tool_arguments)) {
            ESP_LOGE(TAG, "tools/call: Invalid arguments");
            ReplyError(id_int, "Invalid arguments");
            return;
        }
        DoToolCall(id_int, std::string(tool_name->valuestring), tool_arguments);
    } else {
        ESP_LOGE(TAG, "Method not implemented: %s", method_str.c_str());
        ReplyError(id_int, "Method not implemented: " + method_str);
    }
}

void McpServer::ReplyResult(int id, const std::string& result) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id) + ",\"result\":";
    payload += result;
    payload += "}";
    Application::GetInstance().SendMcpMessage(payload);
}

void McpServer::ReplyError(int id, const std::string& message) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id);
    payload += ",\"error\":{\"message\":\"";
    payload += message;
    payload += "\"}}";
    Application::GetInstance().SendMcpMessage(payload);
}

void McpServer::GetToolsList(int id, const std::string& cursor, bool list_user_only_tools) {
    const int max_payload_size = 8000;
    std::string json = "{\"tools\":[";
    
    bool found_cursor = cursor.empty();
    auto it = tools_.begin();
    std::string next_cursor = "";
    
    while (it != tools_.end()) {
        // 如果我们还没有找到起始位置，继续搜索
        if (!found_cursor) {
            if ((*it)->name() == cursor) {
                found_cursor = true;
            } else {
                ++it;
                continue;
            }
        }

        if (!list_user_only_tools && (*it)->user_only()) {
            ++it;
            continue;
        }
        
        // 添加tool前检查大小
        std::string tool_json = (*it)->to_json() + ",";
        if (json.length() + tool_json.length() + 30 > max_payload_size) {
            // 如果添加这个tool会超出大小限制，设置next_cursor并退出循环
            next_cursor = (*it)->name();
            break;
        }
        
        json += tool_json;
        ++it;
    }
    
    if (json.back() == ',') {
        json.pop_back();
    }
    
    if (json.back() == '[' && !tools_.empty()) {
        // 如果没有添加任何tool，返回错误
        ESP_LOGE(TAG, "tools/list: Failed to add tool %s because of payload size limit", next_cursor.c_str());
        ReplyError(id, "Failed to add tool " + next_cursor + " because of payload size limit");
        return;
    }

    if (next_cursor.empty()) {
        json += "]}";
    } else {
        json += "],\"nextCursor\":\"" + next_cursor + "\"}";
    }
    
    ReplyResult(id, json);
}

void McpServer::DoToolCall(int id, const std::string& tool_name, const cJSON* tool_arguments) {
    auto tool_iter = std::find_if(tools_.begin(), tools_.end(), 
                                 [&tool_name](const McpTool* tool) { 
                                     return tool->name() == tool_name; 
                                 });
    
    if (tool_iter == tools_.end()) {
        ESP_LOGE(TAG, "tools/call: Unknown tool: %s", tool_name.c_str());
        ReplyError(id, "Unknown tool: " + tool_name);
        return;
    }

    PropertyList arguments = (*tool_iter)->properties();
    try {
        for (auto& argument : arguments) {
            bool found = false;
            if (cJSON_IsObject(tool_arguments)) {
                auto value = cJSON_GetObjectItem(tool_arguments, argument.name().c_str());
                if (argument.type() == kPropertyTypeBoolean && cJSON_IsBool(value)) {
                    argument.set_value<bool>(value->valueint == 1);
                    found = true;
                } else if (argument.type() == kPropertyTypeInteger && cJSON_IsNumber(value)) {
                    argument.set_value<int>(value->valueint);
                    found = true;
                } else if (argument.type() == kPropertyTypeString && cJSON_IsString(value)) {
                    argument.set_value<std::string>(value->valuestring);
                    found = true;
                }
            }

            if (!argument.has_default_value() && !found) {
                ESP_LOGE(TAG, "tools/call: Missing valid argument: %s", argument.name().c_str());
                ReplyError(id, "Missing valid argument: " + argument.name());
                return;
            }
        }
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "tools/call: %s", e.what());
        ReplyError(id, e.what());
        return;
    }

    // Use main thread to call the tool
    auto& app = Application::GetInstance();
    app.Schedule([this, id, tool_iter, arguments = std::move(arguments)]() {
        try {
            ReplyResult(id, (*tool_iter)->Call(arguments));
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "tools/call: %s", e.what());
            ReplyError(id, e.what());
        }
    });
}

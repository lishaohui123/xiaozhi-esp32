#include "alarm_manager.h"
#include "mcp_server.h"
#include "display.h"
#include "audio/audio_codec.h"
#include "system_info.h"
#include "gloable_var.h"
#include "esp32_music.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <cJSON.h>
#include <ctime>
#include <sstream>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <iomanip>
#include <regex>
#include "constants.h"

#define TAG "AlarmManager"

/**
 * 闹钟类型	weekdays_str格式	说明
 * 一次性闹钟	"" 或 "one_time"	不重复，只在创建当天触发
 * 每日闹钟	"daily"	每天触发
 * 工作日闹钟（含节假日）	"workday"	周一至周五触发，包含节假日
 * 工作日闹钟（排除节假日）	"workday:exclude_holiday"	周一至周五触发，排除节假日
 * 特定年份工作日闹钟	"workday:2024:exclude_holiday"	2024年工作日，排除节假日
 * 周末闹钟	"weekend"	周六、周日触发
 * 假期闹钟	"holiday"	仅在节假日触发
 * 自定义星期闹钟	"1,3,5"	周一、周三、周五触发
 * 二进制模式闹钟	"1100011"	7位二进制，从周日开始
 */

// URL编码函数
static std::string url_encode(const std::string& str) {
    std::string encoded;
    char hex[4];
    
    for (size_t i = 0; i < str.length(); i++) {
        unsigned char c = str[i];
        
        if ((c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += c;
        } else if (c == ' ') {
            encoded += '+';  // 空格编码为'+'或'%20'
        } else {
            snprintf(hex, sizeof(hex), "%%%02X", c);
            encoded += hex;
        }
    }
    return encoded;
}

AlarmManager& AlarmManager::GetInstance() {
    static AlarmManager instance;
    return instance;
}

AlarmManager::AlarmManager() : 
    initialized_(false), 
    last_trigger_time_(0), 
    alarm_client_(AlarmClient::GetInstance()),
    alarm_trigger_task_stack_(nullptr), 
    alarm_trigger_task_tcb_(nullptr), 
    alarm_trigger_task_static_allocated_(false),
    m_alarm_trigger_task_handle(nullptr),
    alarms_(),
    alarms_mutex_(),
    last_triggered_id_() {
    alarm_task_queue_.clear();
    alarm_trigger_task_running_ = true;
}

AlarmManager::~AlarmManager() {
    alarm_trigger_task_running_ = false;
    if (m_alarm_trigger_task_handle != nullptr) {
        if (alarm_trigger_task_static_allocated_) {
            vTaskDelete(m_alarm_trigger_task_handle);
            alarm_trigger_task_static_allocated_ = false;
        }
        m_alarm_trigger_task_handle = nullptr;
    }

    if (alarm_trigger_task_stack_ != nullptr) {
        heap_caps_free(alarm_trigger_task_stack_);
        alarm_trigger_task_stack_ = nullptr;
        ESP_LOGI(TAG, "Alarm trigger task stack memory freed");
    }
    
    if (alarm_trigger_task_tcb_ != nullptr) {
        heap_caps_free(alarm_trigger_task_tcb_);
        alarm_trigger_task_tcb_ = nullptr;
        ESP_LOGI(TAG, "Alarm trigger task TCB memory freed");
    }
}

/******************************
 * http客户端的初始化
 * 加载所有生效的闹钟
 * 加载节假日数据
 * 创建闹钟提醒的线程
 ******************************/
bool AlarmManager::Initialize(const std::string& api_url, const std::string& device_id) {
    // 初始化数据库客户端
    if (!alarm_client_.Initialize(api_url, device_id)) {
        ESP_LOGE(TAG, "Failed to initialize DB client");
        return false;
    }
    
    // 从数据库加载闹钟
    if (!SyncWithDb()) {
        ESP_LOGW(TAG, "Failed to sync with DB, starting with empty alarm list");
    }
    
    // 加载节假日数据
    LoadHolidayData();

    // 更新设备状态
    // alarm_client_.UpdateDeviceStatus(true, "v1.0.0");
    
    /************************************
     * 创建一个静态任务线程，
     * 专门进行闹钟的提醒任务
     ************************************/
    if (m_alarm_trigger_task_handle != nullptr) {
        if (alarm_trigger_task_static_allocated_) {
            vTaskDelete(m_alarm_trigger_task_handle);
            alarm_trigger_task_static_allocated_ = false;
        }
        m_alarm_trigger_task_handle = nullptr;
    }

    if (alarm_trigger_task_stack_ == nullptr) {
        alarm_trigger_task_stack_ = (StackType_t*)heap_caps_malloc(ALARM_TRIGGER_TASK_STACK_SIZE * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
        assert(alarm_trigger_task_stack_ != nullptr);
    }
    
    if (alarm_trigger_task_tcb_ == nullptr) {
        alarm_trigger_task_tcb_ = (StaticTask_t*)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
        assert(alarm_trigger_task_tcb_ != nullptr);
    }

    // 使用静态分配创建任务
    alarm_trigger_task_running_ = true;
    m_alarm_trigger_task_handle = xTaskCreateStatic(
        alarm_trigger_task,
        "alarm_trigger_task",
        ALARM_TRIGGER_TASK_STACK_SIZE,
        this,
        4,
        alarm_trigger_task_stack_,
        alarm_trigger_task_tcb_
    );

    assert(m_alarm_trigger_task_handle != nullptr);
    
    initialized_ = true;
    ESP_LOGI(TAG, "AlarmManager initialized successfully");
    return true;
}

bool AlarmManager::SyncWithDb() {
    ESP_LOGI(TAG, "Syncing alarms with DB");
    
    // 从数据库获取所有闹钟
    std::string response = alarm_client_.GetAllAlarms();

    // 解析响应
    std::vector<AlarmInfo> new_alarms;
    if (!ParseAlarmsFromJson(response, new_alarms)) {
        ESP_LOGE(TAG, "Failed to parse alarms from DB response");
        return false;
    }
    
    {
        // 更新本地闹钟列表
        std::lock_guard<std::mutex> lock(alarms_mutex_);
        alarms_.swap(new_alarms);
    }
    
    ESP_LOGI(TAG, "Synced %d alarms from DB", alarms_.size());
    return true;
}

// 修改ParseAlarmsFromJson函数
bool AlarmManager::ParseAlarmsFromJson(const std::string& json, std::vector<AlarmInfo>& alarms) {
    cJSON* root = cJSON_Parse(json.c_str());
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse JSON: %s", json.c_str());
        return false;
    }
    
    if (!cJSON_IsArray(root)) {
        ESP_LOGE(TAG, "JSON is not an array: %s", json.c_str());
        cJSON_Delete(root);
        return false;
    }
    
    int array_size = cJSON_GetArraySize(root);
    ESP_LOGI(TAG, "Found %d alarms in JSON", array_size);
    
    for (int i = 0; i < array_size; i++) {
        cJSON* alarm_json = cJSON_GetArrayItem(root, i);
        
        AlarmInfo alarm;
        
        // 解析基本字段
        cJSON* id = cJSON_GetObjectItem(alarm_json, "id");
        cJSON* time = cJSON_GetObjectItem(alarm_json, "time");
        cJSON* message = cJSON_GetObjectItem(alarm_json, "message");
        cJSON* weekdays = cJSON_GetObjectItem(alarm_json, "weekdays");
        cJSON* enabled = cJSON_GetObjectItem(alarm_json, "enabled");
        cJSON* is_one_time = cJSON_GetObjectItem(alarm_json, "isOneTime");
        cJSON* created_at = cJSON_GetObjectItem(alarm_json, "createdAt");
        cJSON* updated_at = cJSON_GetObjectItem(alarm_json, "updatedAt");
        cJSON* tip_template_id = cJSON_GetObjectItem(alarm_json, "tipTemplateId");
        
        // 解析其他字段
        if (id && cJSON_IsString(id)) alarm.id = id->valuestring;
        if (time && cJSON_IsString(time)) alarm.time = time->valuestring;
        if (message && cJSON_IsString(message)) alarm.message = message->valuestring;
        if (enabled && cJSON_IsNumber(enabled)) alarm.enabled = enabled->valueint != 0;
        if (is_one_time && cJSON_IsNumber(is_one_time)) alarm.is_one_time = is_one_time->valueint != 0;
        if (created_at && cJSON_IsString(created_at)) alarm.created_at = created_at->valuestring;
        if (tip_template_id && cJSON_IsString(tip_template_id)) alarm.tip_template_id = std::stol(tip_template_id->valuestring);
        
        // 解析weekdays字符串
        if (weekdays && cJSON_IsString(weekdays)) {
            alarm.weekdays_str = weekdays->valuestring;
            // 解析复杂的weekdays格式
            ParseComplexWeekdays(alarm.weekdays_str, alarm);
        } else {
            alarm.weekdays_str = "";
            alarm.alarm_type = ALARM_ONE_TIME;
            alarm.weekdays.resize(7, false);
        }
        
        // ============ 新增：如果是日期特定闹钟，计算目标日期 ============
        if (alarm.alarm_type == ALARM_DATE_SPECIFIC && alarm.is_date_specific) {
            if (alarm.target_date.empty() || !IsAbsoluteDateString(alarm.target_date)) {
                // 计算目标日期
                alarm.target_date = CalculateTargetDate(alarm);
                ESP_LOGI(TAG, "Calculated target date for alarm %s: %s", 
                        alarm.id.c_str(), alarm.target_date.c_str());
            }
        }
        
        // 验证闹钟数据
        if (!alarm.id.empty() && !alarm.time.empty()) {
            ESP_LOGI(TAG, "Parsed alarm: id=%s, time=%s, type=%d, target_date=%s", 
                    alarm.id.c_str(), alarm.time.c_str(), alarm.alarm_type, alarm.target_date.c_str());
            alarms.push_back(alarm);
        }
    }
    
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Successfully parsed %d alarms", alarms.size());
    return true;
}

#if 0
// 解析复杂的weekdays格式
bool AlarmManager::ParseComplexWeekdays(const std::string& weekdays_str, AlarmInfo& alarm) {
    alarm.weekdays.resize(7, false);
    alarm.exclude_holidays = false;
    alarm.year_pattern = "";
    alarm.target_date = "";
    alarm.is_date_specific = false;
    
    ESP_LOGI(TAG, "ParseComplexWeekdays: parsing '%s' for alarm created at %s", 
             weekdays_str.c_str(), alarm.created_at.c_str());
    
    // 空字符串或"one_time"表示一次性闹钟（当天）
    if (weekdays_str.empty() || weekdays_str == "one_time") {
        alarm.alarm_type = ALARM_ONE_TIME;
        return true;
    }
    
    // ============ 修改：日期特定闹钟 - 只记录相对天数，不立即计算日期 ============
    // 格式1: "date:tomorrow" - 明天
    if (weekdays_str == "date:tomorrow") {
        alarm.alarm_type = ALARM_DATE_SPECIFIC;
        alarm.is_one_time = true;
        alarm.is_date_specific = true;
        // 存储相对天数，不立即计算日期
        alarm.year_pattern = "1";  // 使用year_pattern字段存储相对天数
        
        ESP_LOGI(TAG, "Date specific alarm (tomorrow): will calculate based on created_at");
        return true;
    }
    
    // 格式2: "date:+1" - 1天后，"date:+2" - 2天后，以此类推
    if (weekdays_str.find("date:+") == 0) {
        alarm.alarm_type = ALARM_DATE_SPECIFIC;
        alarm.is_one_time = true;
        alarm.is_date_specific = true;
        
        try {
            // 提取相对天数
            std::string days_str = weekdays_str.substr(6); // 跳过"date:+"
            int days = std::stoi(days_str);
            
            // 存储相对天数到year_pattern字段
            alarm.year_pattern = std::to_string(days);
            
            ESP_LOGI(TAG, "Date specific alarm (+%d days): will calculate based on created_at", days);
            return true;
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "Failed to parse days: %s", e.what());
            alarm.alarm_type = ALARM_ONE_TIME;
            return true;
        }
    }
    
    // 格式3: "date:YYYY-MM-DD" - 具体日期（绝对日期）
    if (weekdays_str.find("date:") == 0 && weekdays_str.length() >= 15) { // "date:" + "YYYY-MM-DD"
        std::string date_part = weekdays_str.substr(5); // 跳过"date:"
        
        // 验证是否是绝对日期格式
        if (IsAbsoluteDateString(date_part)) {
            alarm.alarm_type = ALARM_DATE_SPECIFIC;
            alarm.is_one_time = true;
            alarm.is_date_specific = true;
            alarm.target_date = date_part;  // 直接存储绝对日期
            
            ESP_LOGI(TAG, "Date specific alarm (absolute date): target_date=%s", alarm.target_date.c_str());
            return true;
        }
    }
    
    // 格式4: 自然语言日期如"2026-01-14"
    if (weekdays_str.length() == 10 && 
        weekdays_str[4] == '-' && weekdays_str[7] == '-' &&
        std::isdigit(weekdays_str[0]) && std::isdigit(weekdays_str[1]) &&
        std::isdigit(weekdays_str[2]) && std::isdigit(weekdays_str[3]) &&
        std::isdigit(weekdays_str[5]) && std::isdigit(weekdays_str[6]) &&
        std::isdigit(weekdays_str[8]) && std::isdigit(weekdays_str[9])) {
        
        alarm.alarm_type = ALARM_DATE_SPECIFIC;
        alarm.is_one_time = true;
        alarm.is_date_specific = true;
        alarm.target_date = weekdays_str;
        
        ESP_LOGI(TAG, "Date specific alarm (direct date): target_date=%s", alarm.target_date.c_str());
        return true;
    }

    // ============ 原有的解析逻辑 ============
    // 检查特殊关键词
    if (weekdays_str == "daily") {
        // 每日闹钟
        alarm.alarm_type = ALARM_DAILY;
        for (int i = 0; i < 7; i++) {
            alarm.weekdays[i] = true;
        }
        return true;
    }
    
    // 检查工作日闹钟（包含节假日）
    if (weekdays_str == "workday" || weekdays_str == "workday:all") {
        alarm.alarm_type = ALARM_WORKDAY;
        alarm.exclude_holidays = false;
        // 周一至周五
        for (int i = 1; i <= 5; i++) {
            alarm.weekdays[i] = true;
        }
        return true;
    }
    
    // 检查排除节假日的工作日闹钟
    if (weekdays_str == "workday:exclude_holiday" || weekdays_str == "workday_exclude_holiday") {
        alarm.alarm_type = ALARM_WORKDAY;
        alarm.exclude_holidays = true;
        // 周一至周五
        for (int i = 1; i <= 5; i++) {
            alarm.weekdays[i] = true;
        }
        return true;
    }
    
    // 检查周末闹钟
    if (weekdays_str == "weekend") {
        alarm.alarm_type = ALARM_WEEKEND;
        alarm.weekdays[0] = true;  // 周日
        alarm.weekdays[6] = true;  // 周六
        return true;
    }
    
    // 检查假期闹钟
    if (weekdays_str == "holiday") {
        alarm.alarm_type = ALARM_HOLIDAY;
        // 节假日闹钟不依赖weekdays，由节假日数据决定
        return true;
    }
    
    // 检查带年份的工作日闹钟
    std::regex workday_year_regex("workday:(\\d{4})");
    std::smatch match;
    if (std::regex_match(weekdays_str, match, workday_year_regex)) {
        alarm.alarm_type = ALARM_WORKDAY;
        alarm.exclude_holidays = true;
        alarm.year_pattern = match[1].str();
        for (int i = 1; i <= 5; i++) {
            alarm.weekdays[i] = true;
        }
        return true;
    }
    
    // 检查二进制模式 (1100011)
    if (weekdays_str.length() == 7 && 
        weekdays_str.find_first_not_of("01") == std::string::npos) {
        alarm.alarm_type = ALARM_COMPLEX_PATTERN;
        for (int i = 0; i < 7; i++) {
            alarm.weekdays[i] = (weekdays_str[i] == '1');
        }
        return true;
    }
    
    // 检查复杂模式：workday:2024:exclude_holiday
    if (weekdays_str.find("workday:") == 0) {
        size_t pos = weekdays_str.find(":exclude_holiday");
        if (pos != std::string::npos) {
            alarm.alarm_type = ALARM_WORKDAY;
            alarm.exclude_holidays = true;
            std::string year_part = weekdays_str.substr(8, pos - 8);
            if (!year_part.empty() && year_part.find_first_not_of("0123456789") == std::string::npos) {
                alarm.year_pattern = year_part;
            }
            for (int i = 1; i <= 5; i++) {
                alarm.weekdays[i] = true;
            }
            return true;
        }
    }
    
    // 默认：逗号分隔的自定义星期
    alarm.alarm_type = ALARM_CUSTOM_WEEKDAYS;
    std::vector<std::string> days = SplitString(weekdays_str, ',');
    for (const auto& day_str : days) {
        try {
            int day_num = std::stoi(day_str);
            if (day_num >= 1 && day_num <= 7) {
                // 1-7 对应周一到周日，转换为0-6（周日到周六）
                int index = (day_num == 7) ? 0 : day_num;
                alarm.weekdays[index] = true;
            } else if (day_num >= 0 && day_num <= 6) {
                // 0-6 对应周日到周六
                alarm.weekdays[day_num] = true;
            }
        } catch (const std::exception& e) {
            ESP_LOGW(TAG, "Invalid weekday format: %s", day_str.c_str());
        }
    }
    
    return true;
}
#endif
bool AlarmManager::ParseComplexWeekdays(const std::string& weekdays_str, AlarmInfo& alarm) {
    // ----- 1. 输入清洗：去除首尾空白 -----
    auto trim = [](const std::string& s) {
        size_t start = s.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return std::string();
        size_t end = s.find_last_not_of(" \t\r\n");
        return s.substr(start, end - start + 1);
    };
    std::string clean_weekdays = trim(weekdays_str);
    
    // ----- 2. 初始化默认值 -----
    alarm.weekdays.resize(7, false);
    alarm.exclude_holidays = false;
    alarm.year_pattern = "";
    alarm.target_date = "";
    alarm.is_date_specific = false;
    
    ESP_LOGI(TAG, "ParseComplexWeekdays: parsing '%s' (original: '%s') for alarm created at %s",
             clean_weekdays.c_str(), weekdays_str.c_str(), alarm.created_at.c_str());
    
    // ============ 一次性闹钟（当天） ============
    if (clean_weekdays.empty() || clean_weekdays == "one_time") {
        alarm.alarm_type = ALARM_ONE_TIME;
        return true;
    }
    
    // ============ 日期特定闹钟 ============
    if (clean_weekdays.find("date:") == 0) {
        std::string date_param = trim(clean_weekdays.substr(5)); // 跳过"date:"
        
        // --- 格式1：明天 date:tomorrow ---
        if (date_param == "tomorrow") {
            alarm.alarm_type = ALARM_DATE_SPECIFIC;
            alarm.is_one_time = true;
            alarm.is_date_specific = true;
            // 相对天数存储于 year_pattern（注意：此字段在此仅用作相对天数）
            alarm.year_pattern = "1";
            ESP_LOGI(TAG, "Date specific alarm (tomorrow): relative days=1");
            return true;
        }
        
        // --- 格式2：相对天数 date:+N ---
        if (date_param.find("+") == 0) {
            try {
                std::string days_str = date_param.substr(1); // 跳过"+"
                int days = std::stoi(days_str);
                if (days > 0) {
                    alarm.alarm_type = ALARM_DATE_SPECIFIC;
                    alarm.is_one_time = true;
                    alarm.is_date_specific = true;
                    alarm.year_pattern = std::to_string(days);
                    ESP_LOGI(TAG, "Date specific alarm (+%d days): relative days stored", days);
                    return true;
                }
            } catch (const std::exception& e) {
                ESP_LOGE(TAG, "Failed to parse days from '%s'", date_param.c_str());
            }
        }
        
        // --- 格式3：绝对日期 date:YYYY-MM-DD ---
        if (IsAbsoluteDateString(date_param)) {
            alarm.alarm_type = ALARM_DATE_SPECIFIC;
            alarm.is_one_time = true;
            alarm.is_date_specific = true;
            alarm.target_date = date_param;   // 直接存储绝对日期
            alarm.year_pattern = "";          // 清空，避免误用作年份
            ESP_LOGI(TAG, "Date specific alarm (absolute): target_date=%s", alarm.target_date.c_str());
            return true;
        }
        
        // 若 date: 后跟无法识别的参数，按普通一次性闹钟处理（保留原行为）
        ESP_LOGW(TAG, "Unrecognized date parameter '%s', fallback to one-time alarm", date_param.c_str());
        alarm.alarm_type = ALARM_ONE_TIME;
        return true;
    }
    
    // ============ 格式4：直接书写绝对日期（如"2026-01-14"）============
    if (clean_weekdays.length() == 10 && 
        clean_weekdays[4] == '-' && clean_weekdays[7] == '-') {
        if (IsAbsoluteDateString(clean_weekdays)) {
            alarm.alarm_type = ALARM_DATE_SPECIFIC;
            alarm.is_one_time = true;
            alarm.is_date_specific = true;
            alarm.target_date = clean_weekdays;
            alarm.year_pattern = "";
            ESP_LOGI(TAG, "Date specific alarm (direct date): target_date=%s", alarm.target_date.c_str());
            return true;
        }
    }
    
    // ============ 原有其他闹钟类型解析（保持不变）============
    // --- 每日闹钟 ---
    if (clean_weekdays == "daily") {
        alarm.alarm_type = ALARM_DAILY;
        for (int i = 0; i < 7; i++) alarm.weekdays[i] = true;
        return true;
    }
    
    // --- 工作日闹钟（包含节假日）---
    if (clean_weekdays == "workday" || clean_weekdays == "workday:all") {
        alarm.alarm_type = ALARM_WORKDAY;
        alarm.exclude_holidays = false;
        for (int i = 1; i <= 5; i++) alarm.weekdays[i] = true;
        return true;
    }
    
    // --- 工作日闹钟（排除节假日）---
    if (clean_weekdays == "workday:exclude_holiday" || clean_weekdays == "workday_exclude_holiday") {
        alarm.alarm_type = ALARM_WORKDAY;
        alarm.exclude_holidays = true;
        for (int i = 1; i <= 5; i++) alarm.weekdays[i] = true;
        return true;
    }
    
    // --- 周末闹钟 ---
    if (clean_weekdays == "weekend") {
        alarm.alarm_type = ALARM_WEEKEND;
        alarm.weekdays[0] = true;  // 周日
        alarm.weekdays[6] = true;  // 周六
        return true;
    }
    
    // --- 假期闹钟 ---
    if (clean_weekdays == "holiday") {
        alarm.alarm_type = ALARM_HOLIDAY;
        return true;
    }
    
    // --- 带年份的工作日闹钟（workday:2024）---
    std::regex workday_year_regex("workday:(\\d{4})");
    std::smatch match;
    if (std::regex_match(clean_weekdays, match, workday_year_regex)) {
        alarm.alarm_type = ALARM_WORKDAY;
        alarm.exclude_holidays = true;
        alarm.year_pattern = match[1].str();
        for (int i = 1; i <= 5; i++) alarm.weekdays[i] = true;
        return true;
    }
    
    // --- 二进制模式 (1100011) ---
    if (clean_weekdays.length() == 7 && 
        clean_weekdays.find_first_not_of("01") == std::string::npos) {
        alarm.alarm_type = ALARM_COMPLEX_PATTERN;
        for (int i = 0; i < 7; i++) alarm.weekdays[i] = (clean_weekdays[i] == '1');
        return true;
    }
    
    // --- 复杂格式：workday:2024:exclude_holiday ---
    if (clean_weekdays.find("workday:") == 0) {
        size_t pos = clean_weekdays.find(":exclude_holiday");
        if (pos != std::string::npos) {
            alarm.alarm_type = ALARM_WORKDAY;
            alarm.exclude_holidays = true;
            std::string year_part = clean_weekdays.substr(8, pos - 8);
            if (!year_part.empty() && year_part.find_first_not_of("0123456789") == std::string::npos) {
                alarm.year_pattern = year_part;
            }
            for (int i = 1; i <= 5; i++) alarm.weekdays[i] = true;
            return true;
        }
    }
    
    // --- 默认：逗号分隔的自定义星期 ---
    alarm.alarm_type = ALARM_CUSTOM_WEEKDAYS;
    std::vector<std::string> days = SplitString(clean_weekdays, ',');
    for (const auto& day_str : days) {
        try {
            int day_num = std::stoi(day_str);
            if (day_num >= 1 && day_num <= 7) {
                int index = (day_num == 7) ? 0 : day_num; // 1-6 -> 1-6, 7 -> 0
                alarm.weekdays[index] = true;
            } else if (day_num >= 0 && day_num <= 6) {
                alarm.weekdays[day_num] = true;
            }
        } catch (const std::exception& e) {
            ESP_LOGW(TAG, "Invalid weekday format: %s", day_str.c_str());
        }
    }
    
    return true;
}
// 判断是否为绝对日期字符串
bool AlarmManager::IsAbsoluteDateString(const std::string& date_str) {
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
    
    if (year < 2020 || year > 2100) return false;
    if (month < 1 || month > 12) return false;
    if (day < 1 || day > 31) return false;
    
    return true;
}

// 根据创建时间和相对天数计算目标日期
std::string AlarmManager::CalculateTargetDate(const AlarmInfo& alarm) {
    if (!alarm.is_date_specific || alarm.created_at.empty()) {
        return "";
    }
    
    // 如果已经有绝对日期，直接返回
    if (!alarm.target_date.empty() && IsAbsoluteDateString(alarm.target_date)) {
        return alarm.target_date;
    }
    
    // 解析创建时间
    struct tm created_tm = {0};
    const char* format = nullptr;
    
    // 尝试不同的时间格式
    if (strptime(alarm.created_at.c_str(), "%Y-%m-%d %H:%M:%S", &created_tm) != NULL) {
        format = "%Y-%m-%d %H:%M:%S";
    } else if (strptime(alarm.created_at.c_str(), "%Y-%m-%dT%H:%M:%S", &created_tm) != NULL) {
        format = "%Y-%m-%dT%H:%M:%S";
    } else if (strptime(alarm.created_at.c_str(), "%Y-%m-%d", &created_tm) != NULL) {
        format = "%Y-%m-%d";
    }
    
    if (format == nullptr) {
        ESP_LOGE(TAG, "Failed to parse created_at: %s", alarm.created_at.c_str());
        return "";
    }
    
    // 获取相对天数
    int days_to_add = 0;
    if (!alarm.year_pattern.empty()) {
        try {
            days_to_add = std::stoi(alarm.year_pattern);
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "Failed to parse days from year_pattern: %s", alarm.year_pattern.c_str());
            days_to_add = 1; // 默认明天
        }
    } else {
        days_to_add = 1; // 默认明天
    }
    
    // 计算目标日期
    time_t created_time = mktime(&created_tm);
    if (created_time == -1) {
        ESP_LOGE(TAG, "Failed to convert created_tm to time_t");
        return "";
    }
    
    time_t target_time = created_time + (days_to_add * 24 * 3600);
    struct tm* target_tm = localtime(&target_time);
    
    return GetDateString(*target_tm);
}

// 检测闹钟类型
AlarmType AlarmManager::DetectAlarmType(const std::string& weekdays_str) {
    if (weekdays_str.empty() || weekdays_str == "one_time") {
        return ALARM_ONE_TIME;
    } else if (weekdays_str.find("date:") == 0) {
        return ALARM_DATE_SPECIFIC;
    } else if (weekdays_str == "daily") {
        return ALARM_DAILY;
    } else if (weekdays_str == "weekend") {
        return ALARM_WEEKEND;
    } else if (weekdays_str == "holiday") {
        return ALARM_HOLIDAY;
    } else if (weekdays_str.find("workday") == 0) {
        return ALARM_WORKDAY;
    } else if (weekdays_str.length() == 7 && 
               weekdays_str.find_first_not_of("01") == std::string::npos) {
        return ALARM_COMPLEX_PATTERN;
    } else {
        return ALARM_CUSTOM_WEEKDAYS;
    }
}

std::string AlarmManager::GenerateUniqueId() {
    // 生成32个十六进制字符的UUID
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    
    for (int i = 0; i < 32; i++) {
        if (i == 8 || i == 12 || i == 16 || i == 20) {
            ss << "-";
        }
        uint32_t random_byte = esp_random() & 0xF; // 获取0-15的随机数
        ss << random_byte;
    }
    
    return ss.str();
}

bool AlarmManager::ParseTime(const std::string& time_str, int& hours, int& minutes) {
    // 格式：HH:MM
    if (time_str.length() != 5 || time_str[2] != ':') {
        return false;
    }
    
    try {
        hours = std::stoi(time_str.substr(0, 2));
        minutes = std::stoi(time_str.substr(3, 2));
        
        if (hours < 0 || hours > 23 || minutes < 0 || minutes > 59) {
            return false;
        }
        
        return true;
    } catch (const std::exception& e) {
        return false;
    }
}

std::vector<std::string> AlarmManager::SplitString(const std::string& str, char delimiter) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream token_stream(str);
    
    while (std::getline(token_stream, token, delimiter)) {
        tokens.push_back(token);
    }
    
    return tokens;
}

// 判断是否为相对时间格式
bool AlarmManager::IsRelativeTimeFormat(const std::string& time_str) {
    ESP_LOGI(TAG, "IsRelativeTimeFormat checking: '%s'", time_str.c_str());
    
    // 首先检查是否可能是绝对时间格式
    // 绝对时间常见格式："下午四点"、"16:00"、"4点"、"4:00"等
    
    // 检查是否包含"点"字（表示绝对时间点）
    if (time_str.find("点") != std::string::npos) {
        ESP_LOGI(TAG, "Contains '点', likely absolute time");
        return false; // 包含"点"字，很可能是绝对时间
    }
    
    // 检查是否是标准时间格式 HH:MM
    int hours, minutes;
    if (ParseTime(time_str, hours, minutes)) {
        ESP_LOGI(TAG, "Standard time format HH:MM, absolute time");
        return false; // 是标准时间格式，不是相对时间
    }
    
    // ============ 检查相对时间特征 ============
    // 相对时间通常包含"后"字 + 时间单位
    bool has_after = (time_str.find("后") != std::string::npos);
    bool has_minutes = (time_str.find("分钟") != std::string::npos || time_str.find("分") != std::string::npos);
    bool has_hours = (time_str.find("小时") != std::string::npos || time_str.find("时") != std::string::npos);
    
    // 典型相对时间格式："十分钟后"、"2小时后"、"1小时30分钟后"
    if (has_after && (has_minutes || has_hours)) {
        ESP_LOGI(TAG, "Has '后' + time unit, likely relative time");
        return true;
    }
    
    // 检查是否只是数字（可能表示分钟数）
    std::string clean_str = time_str;
    clean_str.erase(std::remove(clean_str.begin(), clean_str.end(), ' '), clean_str.end());
    
    if (!clean_str.empty() && std::all_of(clean_str.begin(), clean_str.end(), ::isdigit)) {
        // 纯数字，可能是相对时间（分钟数），如"10"表示10分钟后
        ESP_LOGI(TAG, "Pure digits, likely relative minutes");
        return true;
    }
    
    // 检查是否包含中文数字的相对时间，如"五分钟后"
    static const std::vector<std::string> chinese_digits = {
        "零", "一", "二", "两", "三", "四", "五", "六", "七", "八", "九", "十",
        "二十", "三十", "四十", "五十", "六十", "七十", "八十", "九十"
    };
    
    for (const auto& digit : chinese_digits) {
        if (clean_str.find(digit) != std::string::npos) {
            // 同时检查是否有相对时间关键词
            if (clean_str.find("后") != std::string::npos || 
                clean_str.find("分钟") != std::string::npos ||
                clean_str.find("小时") != std::string::npos) {
                ESP_LOGI(TAG, "Chinese digit + time unit, likely relative time");
                return true;
            }
        }
    }
    
    ESP_LOGI(TAG, "Not a relative time format");
    return false;
}

// 将中文时间转换为标准时间格式（HH:MM）
std::string AlarmManager::ConvertChineseTimeToStandard(const std::string& chinese_time) {
    ESP_LOGI(TAG, "ConvertChineseTimeToStandard: '%s'", chinese_time.c_str());
    
    // ============ 首先检查是否是标准时间格式 ============
    int hours, minutes;
    if (ParseTime(chinese_time, hours, minutes)) {
        // 已经是标准时间格式，直接返回
        ESP_LOGI(TAG, "Already standard time format: %s", chinese_time.c_str());
        return chinese_time;
    }
    
    std::string result = chinese_time;
    
    // ============ 新增：首先判断是否为相对时间 ============
    // 如果包含"后"字，并且包含"分钟"或"小时"，可能是相对时间
    bool has_after = (chinese_time.find("后") != std::string::npos);
    bool has_minutes = (chinese_time.find("分钟") != std::string::npos || chinese_time.find("分") != std::string::npos);
    bool has_hours = (chinese_time.find("小时") != std::string::npos || chinese_time.find("时") != std::string::npos);
    
    // 判断是否为典型的相对时间格式
    if (has_after && (has_minutes || has_hours)) {
        // 检查是否包含"点"字（绝对时间的标志）
        bool has_dot = (chinese_time.find("点") != std::string::npos);
        if (!has_dot) {
            // 没有"点"字，很可能是相对时间，不进行转换
            ESP_LOGI(TAG, "Detected as relative time format: '%s'", chinese_time.c_str());
            return "";
        }
    }
    
    // ============ 继续原有的绝对时间转换逻辑 ============
    // 移除无关词
    static const std::vector<std::string> remove_words = {
        "提醒", "我", "你", "他", "她", "它", "的", "闹钟", "钟"
    };
    
    for (const auto& word : remove_words) {
        size_t pos = result.find(word);
        while (pos != std::string::npos) {
            result.erase(pos, word.length());
            pos = result.find(word, pos);
        }
    }
    
    // 标准化时间表达
    // "下午" -> +12小时
    // "上午" -> 保持原样
    // "晚上" -> +12小时（如果小于12）
    
    int hour_offset = 0;
    bool has_period = false;
    
    if (result.find("下午") != std::string::npos) {
        hour_offset = 12;
        has_period = true;
        // 移除"下午"
        size_t pos = result.find("下午");
        if (pos != std::string::npos) {
            result.erase(pos, 6); // "下午"是6字节
        }
    } else if (result.find("晚上") != std::string::npos) {
        hour_offset = 12;
        has_period = true;
        // 移除"晚上"
        size_t pos = result.find("晚上");
        if (pos != std::string::npos) {
            result.erase(pos, 6); // "晚上"是6字节
        }
    } else if (result.find("上午") != std::string::npos) {
        has_period = true;
        // 移除"上午"
        size_t pos = result.find("上午");
        if (pos != std::string::npos) {
            result.erase(pos, 6); // "上午"是6字节
        }
    }
    
    // 提取小时和分钟
    hours = 0;
    minutes = 0;
    
    // 查找"点"字
    size_t dian_pos = result.find("点");
    if (dian_pos != std::string::npos) {
        // 提取点前的数字（小时）
        std::string hour_str = result.substr(0, dian_pos);
        hours = ExtractNumberFromSegment(hour_str);
        
        // 查找"分"字
        size_t fen_pos = result.find("分", dian_pos + 3); // 跳过"点"字（3字节）
        if (fen_pos != std::string::npos) {
            // 提取分前的数字（分钟）
            std::string minute_str = result.substr(dian_pos + 3, fen_pos - (dian_pos + 3));
            minutes = ExtractNumberFromSegment(minute_str);
        }
    } else {
        // 没有"点"字，尝试直接提取数字
        // 但如果是相对时间，应该已经返回空字符串了
        // 这里只处理纯数字的绝对时间，如"1230"表示12:30
        std::string clean_result = result;
        clean_result.erase(std::remove(clean_result.begin(), clean_result.end(), ' '), clean_result.end());
        
        // 如果是4位数字，可能表示HHMM格式
        if (clean_result.length() == 4 && std::all_of(clean_result.begin(), clean_result.end(), ::isdigit)) {
            hours = std::stoi(clean_result.substr(0, 2));
            minutes = std::stoi(clean_result.substr(2, 2));
        } else {
            // 其他情况，尝试提取数字但不做转换
            return "";
        }
    }
    
    // 应用时段偏移
    if (has_period && hour_offset > 0 && hours < 12) {
        hours += hour_offset;
    }
    
    // 验证并格式化
    if (hours >= 0 && hours <= 23 && minutes >= 0 && minutes <= 59) {
        char buffer[6];
        snprintf(buffer, sizeof(buffer), "%02d:%02d", hours, minutes);
        ESP_LOGI(TAG, "Converted '%s' to '%s'", chinese_time.c_str(), buffer);
        return std::string(buffer);
    }
    
    // 转换失败，返回空字符串
    ESP_LOGW(TAG, "Failed to convert Chinese time: '%s'", chinese_time.c_str());
    return "";
}

// 添加中文数字转换函数
int AlarmManager::ChineseToNumber(const std::string& chinese_num) {
    static std::unordered_map<std::string, int> chinese_numbers = {
        {"一", 1}, {"二", 2}, {"两", 2}, {"三", 3}, {"四", 4}, 
        {"五", 5}, {"六", 6}, {"七", 7}, {"八", 8}, {"九", 9},
        {"十", 10}, {"二十", 20}, {"三十", 30}, {"四十", 40}, {"五十", 50},
        {"六十", 60}, {"七十", 70}, {"八十", 80}, {"九十", 90}
    };
    
    auto it = chinese_numbers.find(chinese_num);
    if (it != chinese_numbers.end()) {
        return it->second;
    }
    return 0;
}

// 提取数字（支持阿拉伯数字和中文数字）
int AlarmManager::ExtractNumber(const std::string& str) {
    if (str.empty()) return 0;
    
    ESP_LOGI(TAG, "Extracting number from: '%s'", str.c_str());
    
    // 移除空格和其他无关字符
    std::string clean_str;
    for (char c : str) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            clean_str += c;
        }
    }
    
    if (clean_str.empty()) return 0;
    
    // 首先尝试阿拉伯数字
    size_t digit_end = 0;
    while (digit_end < clean_str.length() && isdigit(clean_str[digit_end])) {
        digit_end++;
    }
    if (digit_end > 0) {
        int result = std::stoi(clean_str.substr(0, digit_end));
        ESP_LOGI(TAG, "Extracted Arabic number: %d", result);
        return result;
    }
    
    // 中文数字映射
    static std::unordered_map<std::string, int> chinese_numbers = {
        // 基本数字
        {"零", 0}, {"一", 1}, {"二", 2}, {"两", 2}, {"三", 3}, {"四", 4}, 
        {"五", 5}, {"六", 6}, {"七", 7}, {"八", 8}, {"九", 9},
        // 十位数
        {"十", 10}, {"二十", 20}, {"三十", 30}, {"四十", 40}, {"五十", 50},
        {"六十", 60}, {"七十", 70}, {"八十", 80}, {"九十", 90},
        // 组合数字
        {"十一", 11}, {"十二", 12}, {"十三", 13}, {"十四", 14}, {"十五", 15},
        {"十六", 16}, {"十七", 17}, {"十八", 18}, {"十九", 19},
        {"二十一", 21}, {"二十二", 22}, {"二十三", 23}, {"二十四", 24}, {"二十五", 25},
        {"二十六", 26}, {"二十七", 27}, {"二十八", 28}, {"二十九", 29},
        {"三十一", 31}, {"三十二", 32}, {"三十三", 33}, {"三十四", 34}, {"三十五", 35},
        {"三十六", 36}, {"三十七", 37}, {"三十八", 38}, {"三十九", 39},
        {"四十一", 41}, {"四十二", 42}, {"四十三", 43}, {"四十四", 44}, {"四十五", 45},
        {"四十六", 46}, {"四十七", 47}, {"四十八", 48}, {"四十九", 49},
        {"五十一", 51}, {"五十二", 52}, {"五十三", 53}, {"五十四", 54}, {"五十五", 55},
        {"五十六", 56}, {"五十七", 57}, {"五十八", 58}, {"五十九", 59}
    };
    
    // 检查整个字符串是否匹配中文数字
    auto it = chinese_numbers.find(clean_str);
    if (it != chinese_numbers.end()) {
        ESP_LOGI(TAG, "Exact Chinese number match: %s -> %d", clean_str.c_str(), it->second);
        return it->second;
    }
    
    // 尝试部分匹配（对于组合情况如"一小时三十"中的"三十"）
    // 按长度从长到短匹配
    for (int len = clean_str.length(); len > 0; len--) {
        for (int start = 0; start <= clean_str.length() - len; start++) {
            std::string substr = clean_str.substr(start, len);
            auto iter = chinese_numbers.find(substr);
            if (iter != chinese_numbers.end()) {
                ESP_LOGI(TAG, "Partial Chinese number match: '%s' in '%s' -> %d", 
                        substr.c_str(), clean_str.c_str(), iter->second);
                return iter->second;
            }
        }
    }
    
    ESP_LOGI(TAG, "No number found in: '%s'", clean_str.c_str());
    return 0;
}

// 解析简单相对时间（如："五分钟"、"三十"等）
bool AlarmManager::ParseSimpleRelativeTime(const std::string& time_str, int& minutes) {
    minutes = 0;
    
    // 移除"后"字
    std::string clean_str = time_str;
    size_t after_pos = clean_str.find("后");
    if (after_pos != std::string::npos) {
        clean_str = clean_str.substr(0, after_pos);
    }
    
    // 移除"提醒"等无关词
    size_t remind_pos = clean_str.find("提醒");
    if (remind_pos != std::string::npos) {
        clean_str = clean_str.substr(0, remind_pos);
    }
    
    // 提取数字
    minutes = ExtractNumber(clean_str);
    
    ESP_LOGI(TAG, "Simple time parse: '%s' -> %d minutes", clean_str.c_str(), minutes);
    return minutes > 0;
}

// 解析相对时间字符串
bool AlarmManager::ParseRelativeTime(const std::string& time_str, int& minutes) {
    minutes = 0;
    
    ESP_LOGI(TAG, "ParseRelativeTime: Input='%s'", time_str.c_str());
    
    // 如果字符串包含"点"，直接返回false（这是绝对时间）
    // 注意：这个检查已经在IsRelativeTimeFormat中做了，这里作为双重保险
    if (time_str.find("点") != std::string::npos) {
        ESP_LOGI(TAG, "Contains '点', not a relative time");
        return false;
    }
    
    // 标准化字符串：移除无关词，统一时间单位
    std::string normalized = NormalizeTimeString(time_str);
    ESP_LOGI(TAG, "Normalized: '%s'", normalized.c_str());
    
    // 解析模式1: 包含"小时"和"分钟"的组合
    if (ParseCombinedTime(normalized, minutes)) {
        ESP_LOGI(TAG, "✓ Combined time parsed: %d minutes", minutes);
        return true;
    }
    
    // 解析模式2: 只有小时
    if (ParseHoursOnly(normalized, minutes)) {
        ESP_LOGI(TAG, "✓ Hours only parsed: %d minutes", minutes);
        return true;
    }
    
    // 解析模式3: 只有分钟
    if (ParseMinutesOnly(normalized, minutes)) {
        ESP_LOGI(TAG, "✓ Minutes only parsed: %d minutes", minutes);
        return true;
    }
    
    // 解析模式4: 简单数字（默认为分钟）
    if (ParseSimpleNumber(normalized, minutes)) {
        ESP_LOGI(TAG, "✓ Simple number parsed: %d minutes", minutes);
        return true;
    }
    
    ESP_LOGE(TAG, "✗ All parsing methods failed");
    return false;
}

std::string AlarmManager::NormalizeTimeString(const std::string& input) {
    std::string result = input;
    
    ESP_LOGI(TAG, "NormalizeTimeString: input='%s'", input.c_str());
    
    // 首先移除无关词
    static const std::vector<std::string> remove_words = {
        "提醒", "我", "你", "他", "她", "它", "之后", "以后", "过后", "起床", "接孩子"
    };
    
    for (const auto& word : remove_words) {
        size_t pos = result.find(word);
        while (pos != std::string::npos) {
            result.erase(pos, word.length());
            pos = result.find(word, pos);
        }
    }
    
    ESP_LOGI(TAG, "After removing words: '%s'", result.c_str());
    
    // ============ 注意：这里要谨慎处理"时"字的转换 ============
    // 对于相对时间，"时"可能表示"小时"，如"1时后"
    // 对于绝对时间，"时"可能单独出现，如"4时"（4点）
    // 我们只在确定是相对时间的情况下进行这个转换
    
    // 检查是否是相对时间（包含"后"字）
    bool is_relative = (result.find("后") != std::string::npos);
    
    if (is_relative) {
        // 对于相对时间，"时" -> "小时"
        size_t pos = result.find("时");
        while (pos != std::string::npos) {
            // 检查是否是单独的"时"字，不是"时间"的一部分
            bool is_alone = true;
            if (pos + 3 < result.length()) { // "时"是3字节
                std::string next_char = result.substr(pos + 3, 3); // 取下一个字符
                if (next_char == "间") {
                    is_alone = false;
                }
            }
            
            if (is_alone) {
                result.replace(pos, 3, "小时"); // 用"小时"替换"时"
                pos = result.find("时", pos + 6); // "小时"是6字节，跳到后面继续查找
            } else {
                pos = result.find("时", pos + 3); // 跳过这个"时"字
            }
        }
        
        // 对于相对时间，"分" -> "分钟"
        pos = result.find("分");
        while (pos != std::string::npos) {
            // 检查是否是单独的"分"字，不是"分钟"的一部分
            bool is_alone = true;
            if (pos + 3 < result.length()) { // "分"是3字节
                std::string next_char = result.substr(pos + 3, 3); // 取下一个字符
                if (next_char == "钟") {
                    is_alone = false;
                }
            }
            
            if (is_alone) {
                result.replace(pos, 3, "分钟"); // 用"分钟"替换"分"
                pos = result.find("分", pos + 6); // "分钟"是6字节
            } else {
                pos = result.find("分", pos + 3);
            }
        }
    }
    
    ESP_LOGI(TAG, "After normalizing: '%s'", result.c_str());
    
    // 移除空格
    result.erase(std::remove(result.begin(), result.end(), ' '), result.end());
    
    ESP_LOGI(TAG, "Final normalized: '%s'", result.c_str());
    return result;
}

// 解析组合时间（小时+分钟）
bool AlarmManager::ParseCombinedTime(const std::string& normalized, int& minutes) {
    size_t hour_pos = normalized.find("小时");
    size_t minute_pos = normalized.find("分钟");
    
    ESP_LOGI(TAG, "ParseCombinedTime - hour_pos: %d, minute_pos: %d", hour_pos, minute_pos);
    
    // 必须同时包含小时和分钟才算是组合时间
    if (hour_pos == std::string::npos || minute_pos == std::string::npos) {
        ESP_LOGI(TAG, "Not a combined time format");
        return false;
    }
    
    // 确保分钟在小时之后
    if (minute_pos <= hour_pos) {
        ESP_LOGI(TAG, "Minutes position %d is not after hours position %d", minute_pos, hour_pos);
        return false;
    }
    
    int hours = 0;
    int mins = 0;
    
    // 解析小时部分（小时前面的内容）
    std::string hour_str = normalized.substr(0, hour_pos);
    hours = ExtractNumberFromSegment(hour_str);
    ESP_LOGI(TAG, "Combined - Hour segment: '%s' -> %d hours", hour_str.c_str(), hours);
    
    // 解析分钟部分（小时和分钟之间的内容）
    size_t minute_start = hour_pos + 2; // "小时"是2个字符
    if (minute_start < minute_pos) {
        std::string minute_str = normalized.substr(minute_start, minute_pos - minute_start);
        ESP_LOGI(TAG, "Combined - Minute segment raw: '%s' (start=%d, len=%d)", 
                minute_str.c_str(), minute_start, minute_pos - minute_start);
        
        // 清理分钟字符串：移除可能的中文字符残留
        minute_str = CleanTimeSegment(minute_str);
        ESP_LOGI(TAG, "Combined - Minute segment cleaned: '%s'", minute_str.c_str());
        
        mins = ExtractNumberFromSegment(minute_str);
    }
    
    ESP_LOGI(TAG, "Combined - Minutes: %d", mins);
    
    if (hours > 0 || mins > 0) {
        minutes = hours * 60 + mins;
        ESP_LOGI(TAG, "Combined time total: %d hours + %d minutes = %d minutes", hours, mins, minutes);
        return true;
    }
    
    return false;
}

// 清理时间片段，移除非数字字符
std::string AlarmManager::CleanTimeSegment(const std::string& segment) {
    std::string result;
    
    // 如果是纯阿拉伯数字，直接返回
    bool all_digits = !segment.empty();
    for (char c : segment) {
        if (!isdigit(c)) {
            all_digits = false;
            break;
        }
    }
    if (all_digits) {
        return segment;
    }
    
    // 提取数字部分（阿拉伯数字或中文数字）
    for (size_t i = 0; i < segment.length(); ) {
        // 检查当前字符是否是阿拉伯数字
        if (isdigit(segment[i])) {
            result += segment[i];
            i++;
            continue;
        }
        
        // 检查是否是中文字符（UTF-8中文字符以0xE开头）
        if ((unsigned char)segment[i] >= 0xE0 && i + 2 < segment.length()) {
            std::string chinese_char = segment.substr(i, 3);
            
            // 检查是否是中文数字
            static const std::vector<std::string> chinese_digits = {
                "零", "一", "二", "两", "三", "四", "五", "六", "七", "八", "九", "十",
                "二十", "三十", "四十", "五十", "六十", "七十", "八十", "九十"
            };
            
            bool is_chinese_digit = false;
            for (const auto& digit : chinese_digits) {
                if (chinese_char == digit) {
                    is_chinese_digit = true;
                    break;
                }
            }
            
            if (is_chinese_digit) {
                result += chinese_char;
            }
            
            i += 3; // 跳过这个中文字符
        } else {
            i++; // 跳过其他字符
        }
    }
    
    return result;
}

// 从字符串片段中提取数字
int AlarmManager::ExtractNumberFromSegment(const std::string& segment) {
    if (segment.empty()) return 0;
    
    ESP_LOGI(TAG, "Extracting number from segment: '%s'", segment.c_str());
    
    // 首先尝试阿拉伯数字
    size_t digit_end = 0;
    while (digit_end < segment.length() && isdigit(segment[digit_end])) {
        digit_end++;
    }
    if (digit_end > 0) {
        int result = std::stoi(segment.substr(0, digit_end));
        ESP_LOGI(TAG, "Arabic number found: %d", result);
        return result;
    }
    
    // 中文数字映射（扩展版）
    static std::unordered_map<std::string, int> chinese_numbers = {
        // 基本数字
        {"零", 0}, {"一", 1}, {"二", 2}, {"两", 2}, {"三", 3}, {"四", 4}, 
        {"五", 5}, {"六", 6}, {"七", 7}, {"八", 8}, {"九", 9},
        // 十位数
        {"十", 10}, {"二十", 20}, {"三十", 30}, {"四十", 40}, {"五十", 50},
        {"六十", 60}, {"七十", 70}, {"八十", 80}, {"九十", 90},
        // 组合数字（11-99）
        {"十一", 11}, {"十二", 12}, {"十三", 13}, {"十四", 14}, {"十五", 15},
        {"十六", 16}, {"十七", 17}, {"十八", 18}, {"十九", 19},
        {"二十一", 21}, {"二十二", 22}, {"二十三", 23}, {"二十四", 24}, {"二十五", 25},
        {"二十六", 26}, {"二十七", 27}, {"二十八", 28}, {"二十九", 29},
        {"三十一", 31}, {"三十二", 32}, {"三十三", 33}, {"三十四", 34}, {"三十五", 35},
        {"三十六", 36}, {"三十七", 37}, {"三十八", 38}, {"三十九", 39},
        {"四十一", 41}, {"四十二", 42}, {"四十三", 43}, {"四十四", 44}, {"四十五", 45},
        {"四十六", 46}, {"四十七", 47}, {"四十八", 48}, {"四十九", 49},
        {"五十一", 51}, {"五十二", 52}, {"五十三", 53}, {"五十四", 54}, {"五十五", 55},
        {"五十六", 56}, {"五十七", 57}, {"五十八", 58}, {"五十九", 59},
        {"六十一", 61}, {"六十二", 62}, {"六十三", 63}, {"六十四", 64}, {"六十五", 65},
        {"六十六", 66}, {"六十七", 67}, {"六十八", 68}, {"六十九", 69},
        {"七十一", 71}, {"七十二", 72}, {"七十三", 73}, {"七十四", 74}, {"七十五", 75},
        {"七十六", 76}, {"七十七", 77}, {"七十八", 78}, {"七十九", 79},
        {"八十一", 81}, {"八十二", 82}, {"八十三", 83}, {"八十四", 84}, {"八十五", 85},
        {"八十六", 86}, {"八十七", 87}, {"八十八", 88}, {"八十九", 89},
        {"九十一", 91}, {"九十二", 92}, {"九十三", 93}, {"九十四", 94}, {"九十五", 95},
        {"九十六", 96}, {"九十七", 97}, {"九十八", 98}, {"九十九", 99}
    };
    
    // 精确匹配整个片段
    auto it = chinese_numbers.find(segment);
    if (it != chinese_numbers.end()) {
        ESP_LOGI(TAG, "Exact Chinese number match: %s -> %d", segment.c_str(), it->second);
        return it->second;
    }
    
    // 尝试部分匹配（按长度从长到短）
    for (int len = segment.length(); len > 0; len--) {
        for (int start = 0; start <= segment.length() - len; start++) {
            std::string substr = segment.substr(start, len);
            auto iter = chinese_numbers.find(substr);
            if (iter != chinese_numbers.end()) {
                ESP_LOGI(TAG, "Partial Chinese number match: '%s' in '%s' -> %d", 
                        substr.c_str(), segment.c_str(), iter->second);
                return iter->second;
            }
        }
    }
    
    ESP_LOGI(TAG, "No number found in segment: '%s'", segment.c_str());
    return 0;
}

// 解析只有小时的情况
bool AlarmManager::ParseHoursOnly(const std::string& normalized, int& minutes) {
    size_t hour_pos = normalized.find("小时");
    if (hour_pos == std::string::npos) {
        return false;
    }
    
    std::string hour_str = normalized.substr(0, hour_pos);
    int hours = ExtractNumberFromSegment(hour_str);
    ESP_LOGI(TAG, "Hours only - Hour segment: '%s' -> %d", hour_str.c_str(), hours);
    
    if (hours > 0) {
        minutes = hours * 60;
        return true;
    }
    
    return false;
}

// 解析只有分钟的情况
bool AlarmManager::ParseMinutesOnly(const std::string& normalized, int& minutes) {
    size_t minute_pos = normalized.find("分钟");
    if (minute_pos == std::string::npos) {
        return false;
    }
    
    std::string minute_str = normalized.substr(0, minute_pos);
    int mins = ExtractNumberFromSegment(minute_str);
    ESP_LOGI(TAG, "Minutes only - Minute segment: '%s' -> %d", minute_str.c_str(), mins);
    
    if (mins > 0) {
        minutes = mins;
        return true;
    }
    
    return false;
}

// 解析简单数字（默认为分钟）
bool AlarmManager::ParseSimpleNumber(const std::string& normalized, int& minutes) {
    int number = ExtractNumberFromSegment(normalized);
    if (number > 0) {
        minutes = number;
        ESP_LOGI(TAG, "Simple number: '%s' -> %d minutes", normalized.c_str(), minutes);
        return true;
    }
    return false;
}

// 计算绝对时间
std::string AlarmManager::CalculateAbsoluteTime(int minutes_from_now) {
    time_t now = time(nullptr);
    time_t target_time = now + (minutes_from_now * 60);
    
    struct tm* timeinfo = localtime(&target_time);
    
    char buffer[6]; // HH:MM\0
    snprintf(buffer, sizeof(buffer), "%02d:%02d", timeinfo->tm_hour, timeinfo->tm_min);
    
    ESP_LOGI(TAG, "CalculateAbsoluteTime: now=%lld, +%d minutes = %s", 
             (long long)now, minutes_from_now, buffer);
    
    return std::string(buffer);
}

// 添加相对时间闹钟
std::string AlarmManager::AddRelativeAlarm(int minutes_from_now, const std::string& message, bool enabled) {
    if (minutes_from_now <= 0) {
        return "{\"success\": false, \"message\": \"Invalid time: must be in the future\"}";
    }
    
    // 计算绝对时间
    std::string absolute_time = CalculateAbsoluteTime(minutes_from_now);
    
    ESP_LOGI(TAG, "Relative alarm: %d minutes from now = %s", minutes_from_now, absolute_time.c_str());
    
    // 直接使用计算出的绝对时间创建一次性闹钟
    // 不再调用 AddAlarm，避免重复处理
    int hours, minutes;
    if (!ParseTime(absolute_time, hours, minutes)) {
        return "{\"success\": false, \"message\": \"Failed to calculate absolute time\"}";
    }
    
    // 创建一次性闹钟到数据库
    std::string response = alarm_client_.CreateAlarm(absolute_time, message, "", enabled, true);
    
    // 同步本地闹钟列表
    if (SyncWithDb()) {
        return "{\"success\": true, \"message\": \"Relative alarm added\"}";
    } else {
        return "{\"success\": false, \"message\": \"Failed to sync with database\"}";
    }
}

// 修改AddAlarm函数以支持复杂类型
std::string AlarmManager::AddAlarm(const std::string& time, const std::string& message, 
                                  const std::string& weekdays_str, bool enabled) {
    ESP_LOGI(TAG, "AddAlarm: time='%s', message='%s', weekdays='%s'", 
             time.c_str(), message.c_str(), weekdays_str.c_str());
    
    std::string final_time = time;
    
    // ============ 先检查是否为相对时间 ============
    int minutes_from_now = 0;
    if (IsRelativeTimeFormat(time) && ParseRelativeTime(time, minutes_from_now)) {
        ESP_LOGI(TAG, "Detected as relative time: %d minutes from now", minutes_from_now);
        return AddRelativeAlarm(minutes_from_now, message, enabled);
    }
    
    // ============ 如果不是相对时间，检查是否是标准时间格式 ============
    int hours, minutes;
    if (ParseTime(time, hours, minutes)) {
        // 已经是标准时间格式
        final_time = time;
        ESP_LOGI(TAG, "Already standard time format: %s", final_time.c_str());
    } else {
        // 尝试中文时间转换
        std::string converted_time = ConvertChineseTimeToStandard(time);
        if (!converted_time.empty()) {
            final_time = converted_time;
            ESP_LOGI(TAG, "Using converted time: %s", final_time.c_str());
        } else {
            ESP_LOGE(TAG, "Invalid time format: %s", time.c_str());
            return "{\"success\": false, \"message\": \"Invalid time format\"}";
        }
    }
    
    // ============ 验证最终的时间格式 ============
    if (!ParseTime(final_time, hours, minutes)) {
        ESP_LOGE(TAG, "Invalid final time format: %s", final_time.c_str());
        return "{\"success\": false, \"message\": \"Invalid time format\"}";
    }
    
    ESP_LOGI(TAG, "Detected as absolute time: %02d:%02d", hours, minutes);
    
    // 检测闹钟类型
    AlarmType alarm_type = DetectAlarmType(weekdays_str);
    
    // 构建完整的weekdays字符串
    std::string final_weekdays_str = weekdays_str;
    if (alarm_type == ALARM_WORKDAY) {
        // 默认排除节假日的工作日闹钟
        if (weekdays_str == "workday") {
            final_weekdays_str = "workday:exclude_holiday";
        }
    }
    
    // 对于一次性闹钟（包括日期特定闹钟），设置is_one_time为true
    bool is_one_time = (alarm_type == ALARM_ONE_TIME || alarm_type == ALARM_DATE_SPECIFIC);
    
    // 创建闹钟到数据库
    std::string response = alarm_client_.CreateAlarm(final_time, message, final_weekdays_str, enabled, is_one_time);
    
    // 同步本地闹钟列表
    if (SyncWithDb()) {
        // 查找刚添加的闹钟ID
        std::string alarm_id = "new_alarm_id"; // 需要从response中解析
        return "{\"success\": true, \"id\": \"" + alarm_id + "\"}";
    } else {
        return "{\"success\": false, \"message\": \"Failed to sync with database\"}";
    }
}

// 辅助函数：创建特定类型的闹钟
std::string AlarmManager::AddWorkdayAlarm(const std::string& time, const std::string& message, 
                                         bool exclude_holidays, const std::string& year) {
    std::string weekdays_str = "workday";
    if (exclude_holidays) {
        weekdays_str = "workday:exclude_holiday";
    }
    if (!year.empty()) {
        weekdays_str = "workday:" + year + ":exclude_holiday";
    }
    return AddAlarm(time, message, weekdays_str, true);
}

std::string AlarmManager::AddHolidayAlarm(const std::string& time, const std::string& message) {
    return AddAlarm(time, message, "holiday", true);
}

std::string AlarmManager::AddWeekendAlarm(const std::string& time, const std::string& message) {
    return AddAlarm(time, message, "weekend", true);
}

std::string AlarmManager::AddDailyAlarm(const std::string& time, const std::string& message) {
    return AddAlarm(time, message, "daily", true);
}

bool AlarmManager::DeleteAlarmByTime(const std::string& time_str) {
    std::string response = alarm_client_.DeleteAlarm(time_str);
    SyncWithDb();
    return true;
}

bool AlarmManager::DeleteAllAlarms() {
    std::string response = alarm_client_.DeleteAlarms();
    SyncWithDb();
    return true;
}

std::string AlarmManager::QueryAlarm(const std::string& id) {
    // 从数据库查询闹钟
    std::string response = alarm_client_.GetAlarm(id);
    
    // 解析响应
    cJSON* root = cJSON_Parse(response.c_str());
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse response");
        return "{\"success\": false, \"message\": \"Failed to parse response\"}";
    }
    
    cJSON* success = cJSON_GetObjectItem(root, "success");
    if (!success || !cJSON_IsBool(success) || !success->valueint) {
        cJSON* message = cJSON_GetObjectItem(root, "message");
        std::string error_msg = message && cJSON_IsString(message) ? message->valuestring : "Unknown error";
        cJSON_Delete(root);
        return "{\"success\": false, \"message\": \"" + error_msg + "\"}";
    }
    
    // 返回原始响应
    char* json_str = cJSON_Print(root);
    std::string result(json_str);
    cJSON_Delete(root);
    free(json_str);
    
    return result;
}

// 修改ListAlarms函数以显示完整信息
std::string AlarmManager::ListAlarms(bool enabled_only) {
    std::vector<AlarmInfo> filtered_alarms;
    {
        std::lock_guard<std::mutex> lock(alarms_mutex_);
        
        for (const auto& alarm : alarms_) {
            if (!enabled_only || alarm.enabled) {
                if (alarm.tip_template_id == -1) {      // 只能查询自定义的闹钟
                    filtered_alarms.push_back(alarm);
                }
            }
        }
    }
    
    // 转换为JSON
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    
    cJSON* alarms_array = cJSON_CreateArray();
    for (const auto& alarm : filtered_alarms) {
        cJSON* alarm_json = cJSON_CreateObject();
        
        cJSON_AddStringToObject(alarm_json, "id", alarm.id.c_str());
        cJSON_AddStringToObject(alarm_json, "time", alarm.time.c_str());
        cJSON_AddStringToObject(alarm_json, "message", alarm.message.c_str());
        cJSON_AddBoolToObject(alarm_json, "enabled", alarm.enabled);
        cJSON_AddBoolToObject(alarm_json, "is_one_time", alarm.is_one_time);
        cJSON_AddStringToObject(alarm_json, "weekdays_str", alarm.weekdays_str.c_str());
        
        // 添加闹钟类型
        std::string alarm_type_str;
        switch (alarm.alarm_type) {
            case ALARM_ONE_TIME: alarm_type_str = "one_time"; break;
            case ALARM_DAILY: alarm_type_str = "daily"; break;
            case ALARM_WORKDAY: alarm_type_str = "workday"; break;
            case ALARM_WEEKEND: alarm_type_str = "weekend"; break;
            case ALARM_HOLIDAY: alarm_type_str = "holiday"; break;
            case ALARM_CUSTOM_WEEKDAYS: alarm_type_str = "custom_weekdays"; break;
            case ALARM_COMPLEX_PATTERN: alarm_type_str = "complex_pattern"; break;
            default: alarm_type_str = "unknown"; break;
        }
        cJSON_AddStringToObject(alarm_json, "alarm_type", alarm_type_str.c_str());
        
        cJSON_AddBoolToObject(alarm_json, "exclude_holidays", alarm.exclude_holidays);
        cJSON_AddStringToObject(alarm_json, "year_pattern", alarm.year_pattern.c_str());
        cJSON_AddStringToObject(alarm_json, "effective_dates", alarm.effective_dates.c_str());
        
        cJSON_AddItemToArray(alarms_array, alarm_json);
    }
    
    cJSON_AddItemToObject(root, "alarms", alarms_array);
    cJSON_AddNumberToObject(root, "count", filtered_alarms.size());
    
    char* json_str = cJSON_Print(root);
    std::string result(json_str);
    
    cJSON_Delete(root);
    free(json_str);
    
    return result;
}

bool AlarmManager::UpdateAlarmStatus(const std::string& id, bool enabled) {
    // 更新数据库中的闹钟状态
    std::string response = alarm_client_.UpdateAlarmStatus(id, enabled);
    return true;
}

std::string AlarmManager::AlarmToJson(const AlarmInfo& alarm) {
    cJSON* root = cJSON_CreateObject();
    
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddStringToObject(root, "id", alarm.id.c_str());
    cJSON_AddStringToObject(root, "time", alarm.time.c_str());
    cJSON_AddStringToObject(root, "message", alarm.message.c_str());
    cJSON_AddBoolToObject(root, "enabled", alarm.enabled);
    cJSON_AddStringToObject(root, "created_at", alarm.created_at.c_str());
    cJSON_AddBoolToObject(root, "is_one_time", alarm.is_one_time);
    
    // 添加重复日期
    std::string weekdays_str;
    for (int i = 0; i < 7; i++) {
        if (alarm.weekdays[i]) {
            if (!weekdays_str.empty()) {
                weekdays_str += ",";
            }
            weekdays_str += std::to_string(i + 1);
        }
    }
    cJSON_AddStringToObject(root, "weekdays", weekdays_str.c_str());
    
    // 转换为字符串
    char* json_str = cJSON_Print(root);
    std::string result(json_str);
    
    // 清理
    cJSON_Delete(root);
    free(json_str);
    
    return result;
}

std::string AlarmManager::AlarmsToJson(const std::vector<AlarmInfo>& alarms) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    
    cJSON* alarms_array = cJSON_CreateArray();
    for (const auto& alarm : alarms) {
        cJSON* alarm_json = cJSON_CreateObject();
        
        cJSON_AddStringToObject(alarm_json, "id", alarm.id.c_str());
        cJSON_AddStringToObject(alarm_json, "time", alarm.time.c_str());
        cJSON_AddStringToObject(alarm_json, "message", alarm.message.c_str());
        cJSON_AddBoolToObject(alarm_json, "enabled", alarm.enabled);
        cJSON_AddBoolToObject(alarm_json, "is_one_time", alarm.is_one_time);
        
        // 添加重复日期
        std::string weekdays_str;
        for (int i = 0; i < 7; i++) {
            if (alarm.weekdays[i]) {
                if (!weekdays_str.empty()) {
                    weekdays_str += ",";
                }
                weekdays_str += std::to_string(i + 1);
            }
        }
        cJSON_AddStringToObject(alarm_json, "weekdays", weekdays_str.c_str());
        
        cJSON_AddItemToArray(alarms_array, alarm_json);
    }
    
    cJSON_AddItemToObject(root, "alarms", alarms_array);
    cJSON_AddNumberToObject(root, "count", alarms.size());
    
    // 转换为字符串
    char* json_str = cJSON_Print(root);
    std::string result(json_str);
    
    // 清理
    cJSON_Delete(root);
    free(json_str);
    
    return result;
}

/****************************************************************************
* 闹钟支持一次性的闹钟，也支持周期性的，比如三种类型：
* 第一种 工作日闹钟、假期 周末闹钟；
* 第二种 周一、周二、周三、周四、周五、周六、周日的闹钟；
* 第三种 每日的闹钟；
 ****************************************************************************/
bool AlarmManager::IsAlarmTimeMatch(const AlarmInfo& alarm, const struct tm& now) {
    // 1. 检查闹钟是否启用
    if (!alarm.enabled) {
        return false;
    }
    
    // 2. 检查时间是否匹配
    int alarm_hour, alarm_minute;
    if (!ParseTime(alarm.time, alarm_hour, alarm_minute)) {
        return false;
    }
    
    if (now.tm_hour != alarm_hour || now.tm_min != alarm_minute) {
        return false;
    }
    
    // 3. 检查是否在有效日期范围内
    if (!IsDateInRange(alarm, now)) {
        return false;
    }
    
    // 4. 检查年份模式
    // 仅在“工作日闹钟”且明确指定了年份模式时，检查当前年份是否匹配
    if (alarm.alarm_type == ALARM_WORKDAY && !alarm.year_pattern.empty()) {
        int current_year = now.tm_year + 1900;
        std::string year_str = std::to_string(current_year);
        if (alarm.year_pattern != year_str) {
            ESP_LOGD(TAG, "Year mismatch: alarm year=%s, current=%s",
                    alarm.year_pattern.c_str(), year_str.c_str());
            return false;
        }
    }

    // 5. 根据闹钟类型判断
    switch (alarm.alarm_type) {
        case ALARM_ONE_TIME:
            // 一次性闹钟：检查是否是创建日期
            return IsOneTimeAlarmMatch(alarm, now);
            
        case ALARM_DATE_SPECIFIC:
            // 日期特定闹钟：检查是否是目标日期
            return IsDateSpecificAlarmMatch(alarm, now);
            
        case ALARM_DAILY:
            // 每日闹钟：每天都触发
            return true;
            
        case ALARM_WORKDAY:
            // 工作日闹钟：使用IsWorkday函数
            return IsWorkdayAlarmMatch(alarm, now);
            
        case ALARM_WEEKEND:
            // 周末闹钟：只在正常周末触发
            // 使用专门的周末判断函数
            return IsWeekendForAlarm(now);
            
        case ALARM_HOLIDAY:
            // 假期闹钟：仅在节假日触发
            return IsHoliday(now);
            
        case ALARM_CUSTOM_WEEKDAYS:
            // 自定义星期闹钟
            return alarm.weekdays[now.tm_wday];
            
        case ALARM_COMPLEX_PATTERN:
            // 二进制模式闹钟
            return alarm.weekdays[now.tm_wday];
            
        default:
            return false;
    }
}

// 日期特定闹钟匹配
bool AlarmManager::IsDateSpecificAlarmMatch(const AlarmInfo& alarm, const struct tm& now) {
    if (!alarm.is_date_specific) {
        return false;
    }
    
    // 计算或获取目标日期
    std::string target_date;
    if (alarm.target_date.empty() || !IsAbsoluteDateString(alarm.target_date)) {
        // 如果是相对日期，需要计算
        target_date = CalculateTargetDate(alarm);
        if (target_date.empty()) {
            ESP_LOGE(TAG, "Failed to calculate target date for alarm %s", alarm.id.c_str());
            return false;
        }
        
        // 可以在这里将计算后的日期缓存起来，避免重复计算
        const_cast<AlarmInfo&>(alarm).target_date = target_date;
    } else {
        target_date = alarm.target_date;
    }
    
    // 获取当前日期字符串
    std::string current_date = GetDateString(now);
    
    ESP_LOGD(TAG, "Checking date specific alarm: current=%s, target=%s", 
             current_date.c_str(), target_date.c_str());
    
    // 比较日期是否匹配
    if (current_date == target_date) {
        // 防止同一分钟内重复触发
        if (alarm.id == last_triggered_id_) {
            struct tm last_tm = *localtime(&last_trigger_time_);
            if (last_tm.tm_year == now.tm_year &&
                last_tm.tm_mon == now.tm_mon &&
                last_tm.tm_mday == now.tm_mday &&
                last_tm.tm_hour == now.tm_hour &&
                last_tm.tm_min == now.tm_min) {
                return false;
            }
        }
        return true;
    }
    
    return false;
}

// 检查是否为一次性闹钟
bool AlarmManager::IsOneTimeAlarm(const AlarmInfo& alarm) {
    return (alarm.alarm_type == ALARM_ONE_TIME) || 
           (alarm.alarm_type == ALARM_DATE_SPECIFIC) ||  // 日期特定闹钟也是一次性
           (alarm.weekdays_str.empty() && alarm.is_one_time);
}

// 添加日期特定闹钟
// 添加日期特定闹钟
std::string AlarmManager::AddDateSpecificAlarm(const std::string& time, 
                                              const std::string& message,
                                              const std::string& date_str,  // 如 "2026-01-14" 或 "tomorrow" 或 "+2"
                                              bool enabled) {
    // 检查绝对时间格式
    int hours, minutes;
    if (!ParseTime(time, hours, minutes)) {
        return "{\"success\": false, \"message\": \"Invalid time format\"}";
    }
    
    std::string weekdays_param;
    
    // 根据date_str构建weekdays参数
    if (date_str == "tomorrow") {
        weekdays_param = "date:tomorrow";
    } else if (date_str.find("+") == 0) { // 如 "+2"
        // 验证数字格式
        try {
            int days = std::stoi(date_str.substr(1));
            if (days <= 0) {
                return "{\"success\": false, \"message\": \"Days must be positive\"}";
            }
            weekdays_param = "date:+" + std::to_string(days);
        } catch (const std::exception& e) {
            return "{\"success\": false, \"message\": \"Invalid days format\"}";
        }
    } else if (IsAbsoluteDateString(date_str)) {
        weekdays_param = "date:" + date_str;
    } else {
        return "{\"success\": false, \"message\": \"Invalid date format\"}";
    }
    
    // 创建闹钟到数据库
    std::string response = alarm_client_.CreateAlarm(time, message, weekdays_param, enabled, true);
    
    // 解析响应，获取创建时间等信息
    cJSON* root = cJSON_Parse(response.c_str());
    if (root) {
        cJSON* success = cJSON_GetObjectItem(root, "success");
        if (success && cJSON_IsBool(success) && success->valueint) {
            cJSON* created_at = cJSON_GetObjectItem(root, "createdAt");
            if (created_at && cJSON_IsString(created_at)) {
                ESP_LOGI(TAG, "Alarm created at: %s", created_at->valuestring);
            }
        }
        cJSON_Delete(root);
    }
    
    // 同步本地闹钟列表
    if (SyncWithDb()) {
        // 查找刚添加的闹钟并计算目标日期
        std::lock_guard<std::mutex> lock(alarms_mutex_);
        for (auto& alarm : alarms_) {
            if (alarm.alarm_type == ALARM_DATE_SPECIFIC && alarm.is_date_specific) {
                if (alarm.target_date.empty() || !IsAbsoluteDateString(alarm.target_date)) {
                    alarm.target_date = CalculateTargetDate(alarm);
                    ESP_LOGI(TAG, "Calculated target date for new alarm: %s", alarm.target_date.c_str());
                }
            }
        }
        
        return "{\"success\": true, \"message\": \"Date specific alarm added\"}";
    } else {
        return "{\"success\": false, \"message\": \"Failed to sync with database\"}";
    }
}

// 验证日期字符串格式
bool AlarmManager::IsValidDateString(const std::string& date_str) {
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

// 标准化日期字符串
std::string AlarmManager::NormalizeDateString(const std::string& date_str) {
    // TODO: 实现自然语言日期的解析
    // 例如："2026年1月14日" -> "2026-01-14"
    // "后天" -> 计算具体日期
    
    // 目前只支持标准格式
    if (IsValidDateString(date_str)) {
        return date_str;
    }
    
    return "";
}

// 一次性闹钟匹配
bool AlarmManager::IsOneTimeAlarmMatch(const AlarmInfo& alarm, const struct tm& now) {
    // 检查是否是创建日期当天
    struct tm created_tm = {0};
    const char* format = nullptr;
    
    // 尝试不同的时间格式
    if (strptime(alarm.created_at.c_str(), "%Y-%m-%d %H:%M:%S", &created_tm) != NULL) {
        format = "%Y-%m-%d %H:%M:%S";
    } else if (strptime(alarm.created_at.c_str(), "%Y-%m-%dT%H:%M:%S", &created_tm) != NULL) {
        format = "%Y-%m-%dT%H:%M:%S";
    } else if (strptime(alarm.created_at.c_str(), "%Y-%m-%d", &created_tm) != NULL) {
        format = "%Y-%m-%d";
    }
    
    if (format == nullptr) {
        ESP_LOGE(TAG, "Failed to parse created_at: %s", alarm.created_at.c_str());
        return false;
    }
    
    bool is_same_day = (created_tm.tm_year == now.tm_year &&
                       created_tm.tm_mon == now.tm_mon &&
                       created_tm.tm_mday == now.tm_mday);
    
    if (is_same_day) {
        // 防止同一分钟内重复触发
        if (alarm.id == last_triggered_id_) {
            struct tm last_tm = *localtime(&last_trigger_time_);
            if (last_tm.tm_year == now.tm_year &&
                last_tm.tm_mon == now.tm_mon &&
                last_tm.tm_mday == now.tm_mday &&
                last_tm.tm_hour == now.tm_hour &&
                last_tm.tm_min == now.tm_min) {
                return false;
            }
        }
        return true;
    }
    return false;
}

// 工作日闹钟匹配
bool AlarmManager::IsWorkdayAlarmMatch(const AlarmInfo& alarm, const struct tm& now) {
    // 使用新的IsWorkday函数判断
    bool is_workday = IsWorkday(now);
    
    // 如果alarm.exclude_holidays为true，需要额外检查
    if (alarm.exclude_holidays) {
        // 排除节假日的工作日闹钟
        return is_workday && !IsHoliday(now);
    }
    
    return is_workday;
}

// 检查日期是否在有效范围内
bool AlarmManager::IsDateInRange(const AlarmInfo& alarm, const struct tm& timeinfo) {
    if (alarm.effective_dates.empty()) {
        return true;
    }
    
    // 解析effective_dates字段
    // 格式：start:end 或 start-end 或 YYYY-MM-DD:YYYY-MM-DD
    std::string current_date = GetDateString(timeinfo);
    
    size_t sep_pos = alarm.effective_dates.find(':');
    if (sep_pos == std::string::npos) {
        sep_pos = alarm.effective_dates.find('-');
        if (sep_pos == std::string::npos) {
            return true;
        }
    }
    
    std::string start_date = alarm.effective_dates.substr(0, sep_pos);
    std::string end_date = alarm.effective_dates.substr(sep_pos + 1);
    
    // 清理可能的空格
    start_date.erase(std::remove(start_date.begin(), start_date.end(), ' '), start_date.end());
    end_date.erase(std::remove(end_date.begin(), end_date.end(), ' '), end_date.end());
    
    return (current_date >= start_date && current_date <= end_date);
}

/**************************************************
 * 调用后台接口获取本年度的节假日数据
 **************************************************/
void AlarmManager::LoadHolidayData() {
    std::lock_guard<std::mutex> lock(holiday_mutex_);
    
    time_t now = time(nullptr);
    struct tm* tm_now = localtime(&now);
    int current_year = tm_now->tm_year + 1900;
    
    // 清空缓存
    holiday_cache_.clear();
    
    std::vector<HolidayInfo> holidays = alarm_client_.GetHolidayData(current_year);

    for (const auto& holiday : holidays) {
        holiday_cache_[holiday.date] = holiday;
    }
    
    last_holiday_update_ = now;
    ESP_LOGI(TAG, "Holiday data loaded, %d holidays in cache", holiday_cache_.size());
}

bool AlarmManager::UpdateHolidayData() {
    time_t now = time(nullptr);
    if (now - last_holiday_update_ < HOLIDAY_UPDATE_INTERVAL) {
        return true;
    }
    
    LoadHolidayData(); // 重新加载
    return true;
}

bool AlarmManager::IsHoliday(const struct tm& timeinfo) {
    UpdateHolidayData();
    
    std::string date_str = GetDateString(timeinfo);
    
    std::lock_guard<std::mutex> lock(holiday_mutex_);
    auto it = holiday_cache_.find(date_str);
    if (it != holiday_cache_.end()) {
        // 只有type为"holiday"的日期才算节假日
        return it->second.type == "holiday";
    }
    
    // 不在数据库中的日期，即使是周末也不算节假日
    return false;
}

bool AlarmManager::IsWorkday(const struct tm& timeinfo) {
    UpdateHolidayData();
    
    std::string date_str = GetDateString(timeinfo);
    
    std::lock_guard<std::mutex> lock(holiday_mutex_);
    auto it = holiday_cache_.find(date_str);
    if (it != holiday_cache_.end()) {
        // 数据库中有记录
        if (it->second.type == "workday") {
            // type="workday"的是工作日（调休上班）
            return true;
        } else if (it->second.type == "holiday") {
            // type="holiday"的是节假日，不是工作日
            return false;
        }
    }
    
    // 不在数据库中的日期
    // 周一至周五是正常工作日，周六周日是正常周末
    return (timeinfo.tm_wday >= 1 && timeinfo.tm_wday <= 5);
}

bool AlarmManager::IsWeekendForAlarm(const struct tm& timeinfo) {
    // 周末闹钟只有在正常周末才触发
    // 调休工作日周末和节假日周末都不触发周末闹钟
    
    // 先检查是否是周六或周日
    if (timeinfo.tm_wday != 0 && timeinfo.tm_wday != 6) {
        return false; // 不是周末
    }
    
    UpdateHolidayData();
    
    std::string date_str = GetDateString(timeinfo);
    
    std::lock_guard<std::mutex> lock(holiday_mutex_);
    auto it = holiday_cache_.find(date_str);
    
    if (it == holiday_cache_.end()) {
        // 不在数据库中 = 正常周末
        return true;
    } else {
        // 在数据库中
        if (it->second.type == "holiday") {
            // 节假日落在周末，不触发周末闹钟（触发节假日闹钟）
            return false;
        } else if (it->second.type == "workday") {
            // 调休工作日周末，不触发周末闹钟（触发工作日闹钟）
            return false;
        }
    }
    
    return false;
}

bool AlarmManager::IsNormalWorkday(const struct tm& timeinfo) {
    // 正常工作日：周一至周五且不是节假日
    if (timeinfo.tm_wday < 1 || timeinfo.tm_wday > 5) {
        return false;
    }
    
    return !IsHoliday(timeinfo);
}

std::string AlarmManager::GetDateString(const struct tm& timeinfo) {
    // 使用strftime，这是专门用于时间格式化的函数
    char buffer[32];
    size_t len = strftime(buffer, sizeof(buffer), "%Y-%m-%d", &timeinfo);
    
    if (len == 0) {
        // 格式化失败，返回默认日期
        ESP_LOGW(TAG, "strftime failed to format date");
        return "1970-01-01";
    }
    
    return std::string(buffer);
}

void AlarmManager::PlayTtsAudioStream(const std::string& text, int repeat) {
    auto music = Board::GetInstance().GetMusic();
    std::string url = std::format("{}tts?ttsApiKey={}&url={}&alarmAppId={}&token={}&cluster={}&deviceId={}&voiceType={}&uuid={}&text={}", 
                                                OTA_URI, 
                                                url_encode(GloableVar::tts_api_key), 
                                                url_encode(GloableVar::tts_api_url), 
                                                url_encode(GloableVar::alarm_app_id), 
                                                url_encode(GloableVar::token), 
                                                url_encode(GloableVar::cluster), 
                                                url_encode(GloableVar::device_id), 
                                                url_encode(GloableVar::voice_type), 
                                                url_encode(GloableVar::generate_uuid()), 
                                                url_encode(text));
    
    music->Play(url, repeat);
}

void AlarmManager::PlayTtsAudioStreamVoice(const std::string& text, int repeat) {
    auto music = Board::GetInstance().GetMusic();
    std::string url = std::format("{}tts?ttsApiKey={}&url={}&alarmAppId={}&token={}&cluster={}&deviceId={}&voiceType={}&uuid={}&text={}", 
                                                OTA_URI, 
                                                url_encode(GloableVar::tts_api_key), 
                                                url_encode(GloableVar::tts_api_url), 
                                                url_encode(GloableVar::alarm_app_id), 
                                                url_encode(GloableVar::token), 
                                                url_encode(GloableVar::cluster), 
                                                url_encode(GloableVar::device_id), 
                                                url_encode(GloableVar::voice_type), 
                                                url_encode(GloableVar::generate_uuid()), 
                                                url_encode(text));
    
    music->PlayVoice(url, repeat);
}

std::string AlarmManager::GetTips(long tipTemplateId) {
    std::string response = alarm_client_.GetTips(tipTemplateId);
    return response;
}

/*********************************************************************
 * 闹钟分为三种：
 * 1、用户通过语音定义的闹钟，一次性的和重复性的
 * 2、用户通过用户习惯模版定义的闹钟，都是重复性的
 * 3、用户通过用户习惯自定义的闹钟，都是重复性的
 *********************************************************************/
void AlarmManager::TriggerAlarm(const AlarmInfo& alarm) {
    ESP_LOGI(TAG, "Alarm triggered: %s - %s", alarm.id.c_str(), alarm.message.c_str());
    
    // 记录触发信息
    last_triggered_id_ = alarm.id;
    last_trigger_time_ = time(nullptr);
    
    // 获取闹钟类型描述
    std::string type_desc;
    switch (alarm.alarm_type) {
        case ALARM_ONE_TIME: type_desc = "一次性闹钟"; break;
        case ALARM_DATE_SPECIFIC: 
            type_desc = "指定日期闹钟 (" + alarm.target_date + ")"; 
            break;
        case ALARM_DAILY: type_desc = "每日闹钟"; break;
        case ALARM_WORKDAY: 
            type_desc = alarm.exclude_holidays ? "工作日闹钟(排除节假日)" : "工作日闹钟";
            break;
        case ALARM_WEEKEND: type_desc = "周末闹钟"; break;
        case ALARM_HOLIDAY: type_desc = "假期闹钟"; break;
        case ALARM_CUSTOM_WEEKDAYS: type_desc = "自定义重复闹钟"; break;
        case ALARM_COMPLEX_PATTERN: type_desc = "复杂模式闹钟"; break;
        default: type_desc = "未知类型闹钟"; break;
    }
    
    ESP_LOGI(TAG, "闹钟类型: %s", type_desc.c_str());
    
    // 原有触发逻辑...
    std::string alarm_msg = "";
    int repeat = 0;
    
    if (alarm.tip_template_id == -1) {
        // 播放TTS语音流
        alarm_msg = "小主人，小主人，时间到了，时间到了，咱该" + alarm.message + "了啊";
        repeat = 3;
    } else {
        // 向后端获取提示语
        alarm_msg = GetTips(alarm.tip_template_id);
        repeat = alarm_msg.size() < 15 ? 3 : 1;
    }
    
    PlayTtsAudioStream(alarm_msg, repeat);
}

/**************************************
 * 这个函数只向队列中发送任务
 * 只要有时间匹配就发送任务
 **************************************/
void AlarmManager::CheckAlarms() {
    if (!initialized_) {
        ESP_LOGW(TAG, "AlarmManager not initialized");
        return;
    }

    // 获取当前时间
    time_t now = time(nullptr);
    struct tm timeinfo = *localtime(&now);
    
    // 检查是否是新的一分钟
    static int last_minute = -1;
    if (timeinfo.tm_min == last_minute) {
        return; // 同一分钟内不重复检查
    }
    last_minute = timeinfo.tm_min;

    ESP_LOGD(TAG, "Checking alarms at %02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);

    // 检查每个闹钟
    {
        std::lock_guard<std::mutex> lock(alarms_mutex_);

        for (const auto& alarm : alarms_) {
            if (IsAlarmTimeMatch(alarm, timeinfo)) {
                alarm_task_queue_.push_back(timeinfo);
                break;
            }
        }
    }
}

/**************************************
 * 负责从任务队列中获取时间
 * 并执行具体的闹钟提醒的任务
 ***************************************/
void AlarmManager::alarm_trigger_task(void *arg) {
    ESP_LOGI(TAG, "alarm trigger task start");

    AlarmManager* alarm_manager = static_cast<AlarmManager*>(arg);

    while (alarm_manager->alarm_trigger_task_running_) {
        if (!alarm_manager->alarm_task_queue_.empty()) {
            struct tm timeinfo = alarm_manager->alarm_task_queue_.front();
            alarm_manager->alarm_task_queue_.pop_front();

            std::vector<std::string> once_alarm;
            {
                std::lock_guard<std::mutex> lock(alarm_manager->alarms_mutex_);

                for (const auto& alarm : alarm_manager->alarms_) {
                    if (alarm_manager->IsAlarmTimeMatch(alarm, timeinfo)) {
                        alarm_manager->TriggerAlarm(alarm);
                        if (alarm_manager->IsOneTimeAlarm(alarm)) {
                            once_alarm.push_back(alarm.id);
                        }
                    }
                }
            }

            // 更新数据库
            if (once_alarm.size() > 0) {
                for (const auto& alarm_id : once_alarm) {
                    alarm_manager->UpdateAlarmStatus(alarm_id, false);
                }
                alarm_manager->SyncWithDb();
            }
        }
        else {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
    
    ESP_LOGI(TAG, "alarm trigger task end");

    // vTaskDelete(nullptr);
    while (true) vTaskDelay(portMAX_DELAY);
}

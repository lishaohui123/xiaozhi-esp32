#ifndef ALARM_MANAGER_H
#define ALARM_MANAGER_H

#include <string>
#include <vector>
#include <map>
#include <ctime>
#include <mutex>
#include <functional>
#include <deque>
#include <unordered_map>
#include "board.h"
#include "application.h"
#include "protocols/protocol.h"
#include "gloable_var.h"
#include "alarm_client.h"
#include "http_client.h"
#include "alarm.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


class AlarmManager {
public:
    static AlarmManager& GetInstance();
    
    AlarmManager();
    ~AlarmManager();

    // 初始化闹钟管理器
    bool Initialize(const std::string& api_url, const std::string& device_id);
    
    // 闹钟管理接口
    std::string AddAlarm(const std::string& time, const std::string& message, 
                        const std::string& weekdays_str = "", bool enabled = true);
    
    // 辅助函数：创建特定类型闹钟
    std::string AddWorkdayAlarm(const std::string& time, const std::string& message, 
                               bool exclude_holidays = true, const std::string& year = "");
    std::string AddHolidayAlarm(const std::string& time, const std::string& message);
    std::string AddWeekendAlarm(const std::string& time, const std::string& message);
    std::string AddDailyAlarm(const std::string& time, const std::string& message);
    
    bool DeleteAlarm(const std::string& id);
    bool DeleteAlarmByTime(const std::string& time_str);
    bool DeleteAllAlarms();
    std::string QueryAlarm(const std::string& id);
    std::string ListAlarms(bool enabled_only = false);
    bool UpdateAlarmStatus(const std::string& id, bool enabled);
    
    // 相对时间闹钟
    std::string AddRelativeAlarm(int minutes_from_now, const std::string& message, bool enabled = true);
    bool ParseRelativeTime(const std::string& time_str, int& minutes);
    static bool IsRelativeTimeFormat(const std::string& time_str);
    
    // 与数据库同步
    bool SyncWithDb();
    
    // 获取提示语
    std::string GetTips(long tipTemplateId);
    
    // TTS播放
    void PlayTtsAudioStream(const std::string& text, int repeat);

    // TTS播放 for 通话
    void PlayTtsAudioStreamVoice(const std::string& text, int repeat);
    
    // 闹钟检查和触发
    void CheckAlarms();
    static void alarm_trigger_task(void *arg);
    
    // 节假日相关函数
    bool IsHoliday(const struct tm& timeinfo);
    bool IsWorkday(const struct tm& timeinfo);
    bool IsNormalWorkday(const struct tm& timeinfo);
    bool IsWeekendForAlarm(const struct tm& timeinfo);
    bool IsDateSpecificAlarmMatch(const AlarmInfo& alarm, const struct tm& now);
   std::string AddDateSpecificAlarm(const std::string& time, 
                                              const std::string& message,
                                              const std::string& date_str,  // 如 "2026-01-14" 或 "tomorrow"
                                              bool enabled); 
    bool IsValidDateString(const std::string& date_str);
    std::string NormalizeDateString(const std::string& date_str);
    bool IsAbsoluteDateString(const std::string& date_str);
    std::string CalculateTargetDate(const AlarmInfo& alarm);
    static bool ParseTime(const std::string& time_str, int& hours, int& minutes);
    std::string ConvertChineseTimeToStandard(const std::string& chinese_time);

    // 闹钟类型判断
    static AlarmType DetectAlarmType(const std::string& weekdays_str);
    
private:
    // 禁止拷贝和赋值
    AlarmManager(const AlarmManager&) = delete;
    AlarmManager& operator=(const AlarmManager&) = delete;
    
    // 数据成员（按照初始化顺序声明）
    bool initialized_;
    time_t last_trigger_time_;
    AlarmClient& alarm_client_;
    
    // 静态alarm_trigger_task任务分配相关成员
    static constexpr uint32_t ALARM_TRIGGER_TASK_STACK_SIZE = 1024 * 4;  // 4KB栈空间
    StackType_t* alarm_trigger_task_stack_;                    // 任务栈指针
    StaticTask_t* alarm_trigger_task_tcb_;                     // 任务控制块指针
    bool alarm_trigger_task_static_allocated_;                 // 标记是否已静态分配
    TaskHandle_t m_alarm_trigger_task_handle;

    // 其他数据成员
    std::vector<AlarmInfo> alarms_;
    std::mutex alarms_mutex_;
    std::string last_triggered_id_;

    std::deque<struct tm> alarm_task_queue_;
    volatile bool alarm_trigger_task_running_;
    
    // 节假日缓存
    std::unordered_map<std::string, HolidayInfo> holiday_cache_;
    time_t last_holiday_update_;
    std::mutex holiday_mutex_;
    static constexpr time_t HOLIDAY_UPDATE_INTERVAL = 86400; // 24小时

    // 辅助函数
    std::string GenerateUniqueId();
    bool IsAlarmTimeMatch(const AlarmInfo& alarm, const struct tm& now);
    bool IsOneTimeAlarm(const AlarmInfo& alarm);
    std::string AlarmToJson(const AlarmInfo& alarm);
    std::string AlarmsToJson(const std::vector<AlarmInfo>& alarms);
    static std::vector<std::string> SplitString(const std::string& str, char delimiter);
    bool ParseAlarmsFromJson(const std::string& json, std::vector<AlarmInfo>& alarms);
    static std::string CalculateAbsoluteTime(int minutes_from_now);
    
    // 相对时间解析函数
    static int ChineseToNumber(const std::string& chinese_num);
    static int ExtractNumber(const std::string& str);
    static bool ParseSimpleRelativeTime(const std::string& time_str, int& minutes);
    static std::string NormalizeTimeString(const std::string& input);
    static bool ParseCombinedTime(const std::string& normalized, int& minutes);
    static bool ParseHoursOnly(const std::string& normalized, int& minutes);
    static bool ParseMinutesOnly(const std::string& normalized, int& minutes);
    static bool ParseSimpleNumber(const std::string& normalized, int& minutes);
    static int ExtractNumberFromSegment(const std::string& segment);
    static std::string CleanTimeSegment(const std::string& segment);
    
    // 复杂weekdays解析
    bool ParseComplexWeekdays(const std::string& weekdays_str, AlarmInfo& alarm);
    
    // 时间匹配辅助函数
    bool IsOneTimeAlarmMatch(const AlarmInfo& alarm, const struct tm& now);
    bool IsWorkdayAlarmMatch(const AlarmInfo& alarm, const struct tm& now);
    bool IsDateInRange(const AlarmInfo& alarm, const struct tm& timeinfo);
    
    // 节假日管理
    void LoadHolidayData();
    bool UpdateHolidayData();
    std::string GetDateString(const struct tm& timeinfo);
    
    // TTS和播放相关
    void TriggerAlarm(const AlarmInfo& alarm);
};

#endif // ALARM_MANAGER_H
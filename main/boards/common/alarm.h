#ifndef ALARM_H
#define ALARM_H

#include <string>
#include <vector>
#include <map>
#include <ctime>
#include <mutex>
#include <functional>
#include <deque>
#include <unordered_map>

// 闹钟类型枚举
enum AlarmType {
    ALARM_ONE_TIME = 0,      // 一次性闹钟（当天）
    ALARM_DAILY,             // 每日闹钟
    ALARM_WORKDAY,           // 工作日闹钟
    ALARM_WEEKEND,           // 周末闹钟
    ALARM_HOLIDAY,           // 假期闹钟
    ALARM_CUSTOM_WEEKDAYS,   // 自定义星期闹钟
    ALARM_COMPLEX_PATTERN,   // 二进制模式闹钟
    ALARM_DATE_SPECIFIC,     // 新增：指定日期闹钟
};

// 节假日信息结构
struct HolidayInfo {
    std::string date;           // 日期 YYYY-MM-DD
    std::string type;           // holiday:节假日, workday:调休工作日, weekend:周末
    std::string name;           // 节假日名称
    bool is_off_day;            // 是否是休息日
};

struct AlarmInfo {
    std::string id;                // 闹钟唯一标识符
    long tip_template_id;          // 闹钟模版的id 
    std::string time;              // 闹钟时间，格式："HH:MM"
    std::string message;           // 闹钟提醒内容
    std::vector<bool> weekdays;    // 每周重复设置，7个元素分别代表周一到周日
    std::string weekdays_str;      // 原始的weekdays字符串
    bool enabled;                  // 闹钟是否启用
    std::string created_at;        // 创建时间
    bool is_one_time;              // 是否为一次性闹钟
    AlarmType alarm_type;          // 闹钟类型
    bool exclude_holidays;         // 是否排除节假日
    std::string year_pattern;      // 年份模式
    std::string effective_dates;   // 有效日期范围
    
    // 新增：用于日期特定闹钟的字段
    std::string target_date;      // 目标日期：YYYY-MM-DD
    bool is_date_specific;        // 是否是日期特定闹钟

    AlarmInfo() : 
        tip_template_id(-1L),
        enabled(true), 
        is_one_time(false),
        alarm_type(ALARM_ONE_TIME),
        exclude_holidays(false),
        is_date_specific(false) {
        weekdays.resize(7, false);
    }
};

#endif // ALARM_H

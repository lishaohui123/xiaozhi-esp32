#ifndef alarm_client_H
#define alarm_client_H

#include <string>
#include <functional>
#include <vector>
#include "board.h"
#include "alarm.h"
#include "http_client.h"

class AlarmClient {
public:
    static AlarmClient& GetInstance();
    
    bool Initialize(const std::string& api_url, const std::string& device_id);
    
    std::string GetTips(long tipTemplateId);

    std::vector<HolidayInfo> GetHolidayData(int current_year);

    // 闹钟管理接口
    std::string GetAllAlarms();
    std::string GetAlarm(const std::string& id);
    std::string CreateAlarm(const std::string& time, const std::string& message,
                           const std::string& weekdays, bool enabled, bool is_one_time);
    std::string UpdateAlarm(const std::string& id, const std::string& time, 
                           const std::string& message, const std::string& weekdays, bool enabled);
    std::string DeleteAlarm(const std::string& id);
    std::string DeleteAlarms();
    std::string UpdateAlarmStatus(const std::string& id, bool enabled);
    
    // 设备状态接口
    bool UpdateDeviceStatus(bool online, const std::string& firmware_version);
    
private:
    AlarmClient();
    ~AlarmClient();
    
    // 禁止拷贝和赋值
    AlarmClient(const AlarmClient&) = delete;
    AlarmClient& operator=(const AlarmClient&) = delete;
    
    std::string SendRequest(const std::string& path, const std::string& method,
                           const std::string& body = "");
    
    std::string api_url_;
    std::string device_id_;
    bool initialized_;

public:
    std::shared_ptr<HttpClient> http_client_;   // 复用 HTTP 客户端
    
};

#endif // alarm_client_H

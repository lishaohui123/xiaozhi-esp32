#ifndef MCP_SERVER_ALARM_H
#define MCP_SERVER_ALARM_H

#include "alarm_manager.h"

class McpServerWithAlarm {
public:
    McpServerWithAlarm();
    ~McpServerWithAlarm();

    void Init();
    
private:
    
    // 启动闹钟检查定时器
    void StartAlarmChecker();
    
    // 闹钟检查回调函数
    static void AlarmCheckerCallback(void* arg);
    
    // 定时器句柄
    esp_timer_handle_t alarm_timer_;
};

#endif // MCP_SERVER_ALARM_H

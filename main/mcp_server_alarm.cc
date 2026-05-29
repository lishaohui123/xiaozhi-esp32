#include "mcp_server_alarm.h"
#include "alarm_manager.h"
#include "application.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <cJSON.h>

#define TAG "McpServerAlarm"

McpServerWithAlarm::McpServerWithAlarm() : alarm_timer_(nullptr) {

}

void McpServerWithAlarm::Init() {
    // 启动闹钟检查定时器
    StartAlarmChecker();
}

McpServerWithAlarm::~McpServerWithAlarm() {
    // 停止定时器
    if (alarm_timer_) {
        esp_timer_stop(alarm_timer_);
        esp_timer_delete(alarm_timer_);
        alarm_timer_ = nullptr;
    }
}

void McpServerWithAlarm::StartAlarmChecker() {
    // 创建定时器
    const esp_timer_create_args_t periodic_timer_args = {
        .callback = &McpServerWithAlarm::AlarmCheckerCallback,
        .arg = this,
        .name = "alarm_checker"
    };
    
    esp_err_t err = esp_timer_create(&periodic_timer_args, &alarm_timer_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create alarm timer: %s", esp_err_to_name(err));
        return;
    }
    
    // 每分钟检查一次
    err = esp_timer_start_periodic(alarm_timer_, 10 * 1000 * 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start alarm timer: %s", esp_err_to_name(err));
        esp_timer_delete(alarm_timer_);
        alarm_timer_ = nullptr;
        return;
    }
    
    ESP_LOGI(TAG, "Alarm checker started");
}

void McpServerWithAlarm::AlarmCheckerCallback(void* arg) {
    // 检查闹钟ååå
    AlarmManager::GetInstance().CheckAlarms();
}

#ifndef _CONSTANTS_H
#define _CONSTANTS_H

#include "string.h"
#include <string>
#include <sstream>
#include <iomanip>

#define MQTT_HOST "180.184.39.135"

#define MQTT_URL "mqtt://180.184.39.135:1883"

#define OTA_URI "http://180.184.39.135:8002/xiaozhi/ota/"

#define CONFIG_OTA_URI "https://api.tenclass.net/xiaozhi/ota/"

#define OTA_WS "ws://180.184.39.135:8000/xiaozhi/v1/"

#define CONFIG_OTA_WS "ws://api.tenclass.net/xiaozhi/v1/"

#define ACCESS_TOKEN "sk-293ca4e64d3f4aa39010864ce95dfdbe"

extern std::string blue_device;
extern std::string board_id;

#endif // _CONSTANTS_H

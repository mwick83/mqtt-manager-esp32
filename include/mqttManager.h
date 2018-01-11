#ifndef MQTT_MANAGER_H
#define MQTT_MANAGER_H

#include <cstdint>

#include "mqtt.h"


class MqttManager
{
private:
    const char* logTag = "mqtt_mgr";

public:
    MqttManager(void);
};

#endif /* MQTT_MANAGER_H */

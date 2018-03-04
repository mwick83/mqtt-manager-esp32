#ifndef MQTT_MANAGER_H
#define MQTT_MANAGER_H

#include <stdint.h>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "esp_system.h"
#include "esp_log.h"

#include "mqtt.h"
#include "wifiEvents.h"

/**
 * @brief This is an MQTT client management implementation for the ESP32 that uses my 
 * [fork of the espmqtt client library](https://github.com/mwick83/espmqtt). It is used 
 * to make the client library multi-thread compatible, so it can be used to publish and 
 * receive message from different threads.
 */
class MqttManager
{
public:
    typedef enum {
        ERR_OK = 0,
        ERR_INVALID_ARG = -1,
        ERR_INVALID_CFG = -2,
        ERR_MQTT_CLIENT_ERR = -3,
        ERR_DISCONNECTED = -4,
        ERR_TIMEOUT = -5,
        ERR_NO_RESOURCES = -6,
    } err_t;

    typedef enum {
        QOS_AT_MOST_ONCE = 0,
        QOS_AT_LEAST_ONCE = 1,
        QOS_EXACTLY_ONCE = 2
    } qos_t;

    MqttManager();
    MqttManager(const char* host, uint16_t port, const char* user, const char* password, const char* clientId);
    ~MqttManager();

    err_t init(const char* host, uint16_t port, const char* user, const char* password, const char* clientId);
    err_t start(void);
    void stop(void);
    bool waitConnected(int32_t timeoutMs);
    err_t publish(const char *topic, const char *data, int len, qos_t qos, bool retain);
    bool waitAllPublished(int32_t timeoutMs);

private:
    const char* logTag = "mqtt_mgr";

    // fixed configuration options
    const uint32_t clientKeepAlive = 120; /**< MQTT client keep alive timeout in seconds. */
    const TickType_t lockAcquireTimeout = pdMS_TO_TICKS(250); /**< Maximum publish lock acquisition time in OS ticks. */
    const uint32_t publishMsgInFlightMax = MQTT_BUF_SIZE / 128; /**< Maximum number of publish messages that have outstanding responses.
                                                                 * The value is calculated from the MQTT_BUF_SIZE and an assumed average 
                                                                 * raw messages size of 128 bytes.
                                                                 */

    void preinit(void); /**< Helper function to prepare internal state, which is called by both constructors. */

    // static mqtt client callback dispatchers
    static void clientConnectedDispatch(mqtt_client* client, mqtt_event_data_t* eventData);
    static void clientDisconnectedDispatch(mqtt_client* client, mqtt_event_data_t* eventData);
    static void clientPublishedDispatch(mqtt_client* client, uint16_t msg_id);
    static void clientDataDispatch(mqtt_client* client, mqtt_event_data_t* eventData);

    // actual mqtt client callbacks
    void clientConnected(mqtt_client* client, mqtt_event_data_t* eventData);
    void clientDisconnected(mqtt_client* client, mqtt_event_data_t* eventData);
    void clientPublished(mqtt_client* client, uint16_t msg_id);
    void clientData(mqtt_client* client, mqtt_event_data_t* eventData);

    int getPublishMsgInFlightCount(void); /**< Helper function to get the number of publish messages currently in flight. */

    // mqtt client + settings
    mqtt_client* client;
    mqtt_settings clientSettings;
    bool clientSettingsOk;

    // mqtt client events
    EventGroupHandle_t clientEvents;
    const int clientEventConnected = (1<<0);
    const int clientEventDisconnected = (1<<1);

    // multi-threading signalization
    SemaphoreHandle_t publishMutex;
    StaticSemaphore_t publishMutexBuf;
    std::vector<uint16_t> publishMsgInFlight;
};

#endif /* MQTT_MANAGER_H */

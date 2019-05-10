#ifndef MQTT_MANAGER_H
#define MQTT_MANAGER_H

#include <stdint.h>
#include <algorithm>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "esp_system.h"
#include "esp_log.h"

#include "mqtt_client.h"
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
    typedef enum err_t_ {
        ERR_OK = 0,
        ERR_INVALID_ARG = -1,
        ERR_INVALID_CFG = -2,
        ERR_MQTT_CLIENT_ERR = -3,
        ERR_DISCONNECTED = -4,
        ERR_TIMEOUT = -5,
        ERR_NO_RESOURCES = -6,
    } err_t;

    typedef enum qos_t_ {
        QOS_AT_MOST_ONCE = 0,
        QOS_AT_LEAST_ONCE = 1,
        QOS_EXACTLY_ONCE = 2
    } qos_t;

    typedef void (* subscription_callback_t)(const char* topic, int topicLen, const char* data, int dataLen);

    MqttManager();
    MqttManager(const char* host, uint16_t port, bool ssl, const char* user, const char* password, const char* clientId,
        bool cleanSession, int reconnectTimeoutMs);
    ~MqttManager();

    err_t init(const char* host, uint16_t port, bool ssl, const char* user, const char* password, const char* clientId,
        bool cleanSession, int reconnectTimeoutMs);
    err_t start(void);
    void stop(void);
    bool waitConnected(int32_t timeoutMs);
    err_t publish(const char *topic, const char *data, int len, qos_t qos, bool retain);
    bool waitAllPublished(int32_t timeoutMs);
    err_t subscribe(const char *topic, qos_t qos, subscription_callback_t callback);

private:
    const char* logTag = "mqtt_mgr";

    // fixed configuration options
    const int clientKeepAlive = 120; /**< MQTT client keep alive timeout in seconds. */
    const TickType_t lockAcquireTimeout = pdMS_TO_TICKS(250); /**< Maximum publish lock acquisition time in OS ticks. */
    static const uint32_t publishMsgInFlightMax = MQTT_BUFFER_SIZE_BYTE / 128; /**< Maximum number of publish messages that have outstanding responses.
                                                                                * The value is calculated from the MQTT_BUF_SIZE and an assumed average 
                                                                                * raw messages size of 128 bytes.
                                                                                */
    const TickType_t publishMsgInFlightTimeout = pdMS_TO_TICKS(3000); /**< Time in OS ticks to wait for a publish message acknowledge. */
    static const uint32_t subscriptionsMax = 32; /**< Maximum number of subscriptions the manager can handle. */
    const int reconnectTimeoutMsDflt = 10000; /**< MQTT client reconnect timeout in milliseconds (default). */
    const int connectTimeoutTicks = pdMS_TO_TICKS(1500); /**< Time in OS ticks to wait for a successfull connection. */

    // general config options which my be changed at runtime
    TickType_t reconnectTimeoutTicks = pdMS_TO_TICKS(10000); /**< MQTT client reconnect timeout in ticks. */
    bool autoReconnect = true; /**< Enable MQTT client auto reconnect. */

    // misc state
    bool stopRequested = false;

    void preinit(void); /**< Helper function to prepare internal state, which is called by both constructors. */

    // static mqtt client event handler/dispatcher
    static esp_err_t clientEventHandler(esp_mqtt_event_handle_t event);

    // actual mqtt client callbacks
    void clientConnected(esp_mqtt_event_handle_t eventData);
    void clientDisconnected(esp_mqtt_event_handle_t eventData);
    void clientPublished(esp_mqtt_event_handle_t eventData);
    void clientData(esp_mqtt_event_handle_t eventData);

    int getPublishMsgInFlightCount(void); /**< Helper function to get the number of publish messages currently in flight. */

    // mqtt client + settings
    esp_mqtt_client_handle_t client;
    esp_mqtt_client_config_t clientSettings;
    char clientSettingsHost[MQTT_MAX_HOST_LEN];
    char clientSettingsClientId[MQTT_MAX_CLIENT_LEN];
    char clientSettingsUsername[MQTT_MAX_USERNAME_LEN];
    char clientSettingsPassword[MQTT_MAX_PASSWORD_LEN];
    bool clientSettingsOk;

    // mqtt client events
    EventGroupHandle_t clientEvents;
    const int clientEventConnected = (1<<0);
    const int clientEventDisconnected = (1<<1);

    // topic subscription handling
    typedef struct subscription_info_t_ {
        bool valid;
        char* topic;
        qos_t qos;
        subscription_callback_t callback;
    } subscription_info_t;

    SemaphoreHandle_t subscribeMutex;
    StaticSemaphore_t subscribeMutexBuf;
    subscription_info_t subscriptions[subscriptionsMax];

    // publish message in-flight handling
    typedef struct publish_msg_info_t_ {
        bool valid;
        MqttManager *caller;
        uint16_t msgId;
    } publish_msg_info_t;

    SemaphoreHandle_t publishMutex;
    StaticSemaphore_t publishMutexBuf;
    TimerHandle_t publishMsgInFlightTimer[publishMsgInFlightMax];
    StaticTimer_t publishMsgInFlightTimerBuf[publishMsgInFlightMax];
    publish_msg_info_t publishMsgInFlightInfo[publishMsgInFlightMax];
    unsigned int publishMsgInFlightCnt;

    static void clientPublishTimeoutDispatch(TimerHandle_t timer);
    void clientPublishTimeout(uint16_t msgId);

    // timers
    TimerHandle_t reconnectTimer;
    StaticTimer_t reconnectTimerBuf;
    TimerHandle_t connectTimer;
    StaticTimer_t connectTimerBuf;

    static void reconnectTimeoutDispatch(TimerHandle_t timer);
    static void connectTimeoutDispatch(TimerHandle_t timer);
};

#endif /* MQTT_MANAGER_H */

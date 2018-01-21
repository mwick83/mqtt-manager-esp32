#include "mqttManager.h"

MqttManager::MqttManager()
{
    preinit();
}

MqttManager::MqttManager(const char* host, uint16_t port, const char* user, const char* password, const char* clientId)
{
    preinit();
    init(host, port, user, password, clientId);
}

void MqttManager::preinit(void)
{
    client = nullptr;
    clientSettingsOk = false;
    clientEvents = xEventGroupCreate();

    publishMutex = xSemaphoreCreateMutexStatic(&publishMutexBuf);
    publishMessages = xSemaphoreCreateCountingStatic(publishMsgInFlightMax, publishMsgInFlightMax, &publishMessagesBuf);
}

MqttManager::~MqttManager()
{
    if(client) {
        stop();
        client = nullptr;
    }

    if(publishMessages) vSemaphoreDelete(publishMessages);
    if(publishMutex) vSemaphoreDelete(publishMutex);
}

MqttManager::err_t MqttManager::init(const char* host, uint16_t port, const char* user, const char* password, const char* clientId)
{
    // TBD: own error codes + typedef
    err_t ret = ERR_OK;

    if((nullptr == host) || (nullptr == user) || (nullptr == password) || (nullptr == clientId)) {
        ret = ERR_INVALID_ARG;
    }

    if(strlen(host) > CONFIG_MQTT_MAX_HOST_LEN) ret = ERR_INVALID_ARG;
    if(strlen(user) > CONFIG_MQTT_MAX_USERNAME_LEN) ret = ERR_INVALID_ARG;
    if(strlen(password) > CONFIG_MQTT_MAX_PASSWORD_LEN) ret = ERR_INVALID_ARG;
    if(strlen(clientId) > CONFIG_MQTT_MAX_CLIENT_LEN) ret = ERR_INVALID_ARG;

    if(ret) {
        ESP_LOGE(logTag, "Invalid argument(s) passed in or string(s) too long!");
    } else {
        // prepare MQTT settings
        strncpy(clientSettings.host, host, CONFIG_MQTT_MAX_HOST_LEN);
        clientSettings.port = port;
        strncpy(clientSettings.username, user, CONFIG_MQTT_MAX_USERNAME_LEN);
        strncpy(clientSettings.password, password, CONFIG_MQTT_MAX_PASSWORD_LEN);
        clientSettings.clean_session = 0;
        clientSettings.keepalive = clientKeepAlive;
        strncpy(clientSettings.client_id, clientId, CONFIG_MQTT_MAX_CLIENT_LEN);
        clientSettings.auto_reconnect = true;
        clientSettings.lwt_topic[0] = 0;
        clientSettings.connected_cb = clientConnectedDispatch;
        clientSettings.disconnected_cb = clientDisconnectedDispatch;
        clientSettings.subscribe_cb = NULL;
        clientSettings.publish_cb = clientPublishedDispatch;
        clientSettings.data_cb = clientDataDispatch;

        clientSettingsOk = true;
    }

    return ret;
}

MqttManager::err_t MqttManager::start(void)
{
    err_t ret = ERR_OK;

    if(clientSettingsOk) {
        client = mqtt_start(&clientSettings, (void*) this);
    } else {
        ESP_LOGE(logTag, "MQTT settings not okay! Did you run init()?");
        ret = ERR_INVALID_CFG;
    }

    if(nullptr == client) {
        ESP_LOGE(logTag, "Failed to start MQTT client!")
        return ERR_MQTT_CLIENT_ERR;
    } else {
        ESP_LOGI(logTag, "MqttManager and MQTT client started.");
    }

    return ret;
}

void MqttManager::stop(void)
{
    if(client) {
        // Signalize that we are disconnected even though we aren't yet.
        // This is done to ensure no new publishes are being accepted.
        if((xEventGroupGetBits(clientEvents) & clientEventConnected) != 0) {
            xEventGroupClearBits(clientEvents, clientEventConnected);
            xEventGroupSetBits(clientEvents, clientEventDisconnected);
        }

        // TBD: Wakeup and let blocked threads fail? -> maybe handle this with proper timeouts

        ESP_LOGI(logTag, "Waiting for all publishes to have taken place.");
        // Take the publish mutex and return it again. This will ensure that
        // all pending publishes have been done.
        // TBD: do we want to have some timeout? But what to do when it does time out?
        xSemaphoreTake(publishMutex, portMAX_DELAY);
        xSemaphoreGive(publishMutex);

        // Wait till all messages are out
        while(uxSemaphoreGetCount(publishMessages) < publishMsgInFlightMax) {
            // but poll only as long as wifi is connected
            if((xEventGroupGetBits(wifiEvents) & wifiEventDisconnected) == 0) {
                vTaskDelay(pdMS_TO_TICKS(250));
            } else {
                ESP_LOGW(logTag, "WiFi went down while waiting for all messages being sent.");
                break;
            }
        }

        ESP_LOGI(logTag, "Stopping MQTT client.");
        mqtt_stop();
    }
}

void MqttManager::clientConnectedDispatch(mqtt_client* client, mqtt_event_data_t* eventData)
{
    MqttManager* manager = (MqttManager*) client->userData;

    if(nullptr == manager) {
        ESP_LOGE(manager->logTag, "No valid MqttManager available to dispatch connected event to!")
    } else {
        manager->clientConnected(client, eventData);
    }
}

void MqttManager::clientDisconnectedDispatch(mqtt_client* client, mqtt_event_data_t* eventData)
{
    MqttManager* manager = (MqttManager*) client->userData;

    if(nullptr == manager) {
        ESP_LOGE(manager->logTag, "No valid MqttManager available to dispatch disconnected event to!")
    } else {
        manager->clientDisconnected(client, eventData);
    }
}

void MqttManager::clientPublishedDispatch(mqtt_client* client, mqtt_event_data_t* eventData)
{
    MqttManager* manager = (MqttManager*) client->userData;

    if(nullptr == manager) {
        ESP_LOGE(manager->logTag, "No valid MqttManager available to dispatch published event to!")
    } else {
        manager->clientPublished(client, eventData);
    }
}

void MqttManager::clientDataDispatch(mqtt_client* client, mqtt_event_data_t* eventData)
{
    MqttManager* manager = (MqttManager*) client->userData;

    if(nullptr == manager) {
        ESP_LOGE(manager->logTag, "No valid MqttManager available to dispatch data event to!")
    } else {
        manager->clientData(client, eventData);
    }
}

void MqttManager::clientConnected(mqtt_client* client, mqtt_event_data_t* eventData)
{
    ESP_LOGI(logTag, "Connected.");
    xEventGroupSetBits(clientEvents, clientEventConnected);
    xEventGroupClearBits(clientEvents, clientEventDisconnected);
}

void MqttManager::clientDisconnected(mqtt_client* client, mqtt_event_data_t* eventData)
{
    ESP_LOGI(logTag, "Disconnected.");
    xEventGroupClearBits(clientEvents, clientEventConnected);
    xEventGroupSetBits(clientEvents, clientEventDisconnected);
}

void MqttManager::clientPublished(mqtt_client* client, mqtt_event_data_t* eventData)
{
    ESP_LOGD(logTag, "Published.");

    // return the publish "slot"
    xSemaphoreGive(publishMessages);
}

void MqttManager::clientData(mqtt_client* client, mqtt_event_data_t* eventData)
{
    unsigned int topicLen = eventData->topic_length;
    unsigned int dataLen = eventData->data_length;

    char *topicBuf = (char*) malloc(topicLen+1);
    char *dataBuf = (char*) malloc(dataLen+1);

    if((nullptr != topicBuf) && (nullptr != dataBuf))
    {
        memcpy(topicBuf, eventData->topic, topicLen);
        topicBuf[topicLen] = 0;

        memcpy(dataBuf, eventData->data, dataLen);
        dataBuf[dataLen] = 0;

        ESP_LOGD(logTag, "topic: %s, data: %s", topicBuf, dataBuf);
    } else {
        ESP_LOGE(logTag, "Couldn't allocate memory for topic/data buffer. Ignoring data.");
    }

    if(nullptr != topicBuf) free(topicBuf);
    if(nullptr != dataBuf) free(dataBuf);
}

/**
 * @brief Wait for the MQTT client connection to be established.
 * 
 * If the connection is already established, the function will return
 * immediatly.
 * 
 * @param timeoutMs Timeout in milliseconds to wait for the connection. Can be -1 to wait
 * indefinitely or 0 to only check the current status without waiting at all.
 * @return bool
 * @retval true Connection is established.
 * @retval false Connection hasn't been established within the specified timeout.
 */
bool MqttManager::waitConnected(int32_t timeoutMs)
{
    TickType_t timeout;

    if(timeoutMs == -1) {
        timeout = portMAX_DELAY;
    } else {
        timeout = pdMS_TO_TICKS(timeoutMs);
    }

    if(pdTRUE == xEventGroupWaitBits(clientEvents, clientEventConnected, 0, pdFALSE, timeout)) {
        return true;
    } else {
        return false;
    }
}

/**
 * @brief Publish data to a specified topic
 * 
 * @param topic String of the topic to publish to.
 * @param data String or binary data to publish.
 * @param len Length of the data in bytes.
 * @param qos Quality of Service level used to publish.
 * @param retain Wether or not the published data shall be retained on the broker.
 * @return MqttManager::err_t
 * @retval ERR_OK on success.
 * @retval ERR_DISCONNECTED if the MQTT client connection isn't established.
 * @retval ERR_TIMEOUT if the internal publishing lock couldn't be acquired in time. This
 *         may happen when many other threads try to publish at the same time.
 * @retval ERR_NO_RESOURCES if there are too many unpublished message in the sending queue.
 */
MqttManager::err_t MqttManager::publish(const char *topic, const char *data, int len, qos_t qos, bool retain)
{
    err_t ret = ERR_OK;

    if((xEventGroupGetBits(clientEvents) & clientEventConnected) == 0) {
        ESP_LOGW(logTag, "Publish requested, but MQTT client is disconnected.");
        ret = ERR_DISCONNECTED;
    } else {
        if(pdFALSE == xSemaphoreTake(publishMutex, lockAcquireTimeout)) {
            ESP_LOGE(logTag, "Couldn't acquire publish lock within timeout");
            ret = ERR_TIMEOUT;
        } else {
            if(pdFALSE == xSemaphoreTake(publishMessages, 0)) {
                ESP_LOGE(logTag, "Maximum number of in-flight publish messages reached!");
                ret = ERR_NO_RESOURCES;
            } else {
                mqtt_publish(client, topic, data, len, qos, retain ? 1 : 0);
            }

            xSemaphoreGive(publishMutex);
        }
    }

    return ret;
}

/**
 * @brief Wait for all outstanding publish messages being sent.
 * 
 * For this to work you must ensure by other means that no more messages
 * get published. Otherwise this may always fail or will never return.
 * 
 * @param timeoutMs Timeout in milliseconds to wait. Can be -1 to wait indefinitely 
 * or 0 to only check the current status.
 * @return bool
 * @retval true All outstanding messages have been published.
 * @retval false Not all outstanding messages have been published within the specified timeout.
 */
bool MqttManager::waitAllPublished(int32_t timeoutMs)
{
    TickType_t timeout;

    if(timeoutMs == -1) {
        timeout = portMAX_DELAY;
    } else {
        timeout = pdMS_TO_TICKS(timeoutMs);
    }

    // TBD: how to wait for the semaphore count to be max inflight messages again?
    //if(pdTRUE == xSema(clientEvents, clientEventConnected, 0, pdFALSE, timeout)) {
    if(true) {
        return true;
    } else {
        return false;
    }
}

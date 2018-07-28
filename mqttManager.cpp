#include "mqttManager.h"

MqttManager::MqttManager()
{
    preinit();
}

MqttManager::MqttManager(const char* host, uint16_t port, bool ssl, const char* user, const char* password, const char* clientId)
{
    preinit();
    init(host, port, ssl, user, password, clientId);
}

void MqttManager::preinit(void)
{
    client = nullptr;
    clientSettingsOk = false;
    clientEvents = xEventGroupCreate();

    publishMutex = xSemaphoreCreateMutexStatic(&publishMutexBuf);

    for(int cnt=0; cnt<publishMsgInFlightMax; cnt++) {
        publishMsgInFlightTimer[cnt] = xTimerCreateStatic("mqttPubTmr", publishMsgInFlightTimeout,
            pdFALSE, nullptr, clientPublishTimeoutDispatch, &publishMsgInFlightTimerBuf[cnt]);
    }
    publishMsgInFlightCnt = 0;
}

MqttManager::~MqttManager()
{
    if(client) {
        stop();
        esp_mqtt_client_destroy(client);
        client = nullptr;
    }

    for(int pos=0; pos < publishMsgInFlightMax; pos++) {
        xTimerStop(publishMsgInFlightTimer[pos], portMAX_DELAY);
    }

    if(publishMutex) vSemaphoreDelete(publishMutex);
}

MqttManager::err_t MqttManager::init(const char* host, uint16_t port, bool ssl, const char* user, const char* password, const char* clientId)
{
    err_t ret = ERR_OK;

    if((nullptr == host) || (nullptr == user) || (nullptr == password) || (nullptr == clientId)) {
        ret = ERR_INVALID_ARG;
    }

    if(strlen(host) > MQTT_MAX_HOST_LEN) ret = ERR_INVALID_ARG;
    if(strlen(user) > MQTT_MAX_USERNAME_LEN) ret = ERR_INVALID_ARG;
    if(strlen(password) > MQTT_MAX_PASSWORD_LEN) ret = ERR_INVALID_ARG;
    if(strlen(clientId) > MQTT_MAX_CLIENT_LEN) ret = ERR_INVALID_ARG;

    if(ret) {
        ESP_LOGE(logTag, "Invalid argument(s) passed in or string(s) too long!");
    } else {
        // prepare MQTT settings
        strncpy(clientSettingsHost, host, MQTT_MAX_HOST_LEN);
        strncpy(clientSettingsClientId, clientId, MQTT_MAX_CLIENT_LEN);
        strncpy(clientSettingsUsername, user, MQTT_MAX_USERNAME_LEN);
        strncpy(clientSettingsPassword, password, MQTT_MAX_PASSWORD_LEN);

        const esp_mqtt_client_config_t settings = {
            event_handle : clientEventHandler,
            host : clientSettingsHost,
            uri : nullptr,
            port : port,
            client_id : clientSettingsClientId,
            username : clientSettingsUsername,
            password : clientSettingsPassword,
            lwt_topic : nullptr,
            lwt_msg : nullptr,
            lwt_qos : QOS_AT_MOST_ONCE,
            lwt_retain : true,
            lwt_msg_len : 0,
            disable_clean_session : 1,
            keepalive : clientKeepAlive,
            disable_auto_reconnect : false,
            user_context : (void*) this,
            task_prio : MQTT_TASK_PRIORITY,
            task_stack : MQTT_TASK_STACK,
            buffer_size : MQTT_BUFFER_SIZE_BYTE,
            cert_pem : nullptr,
            transport : (ssl == true) ? MQTT_TRANSPORT_OVER_SSL : MQTT_TRANSPORT_OVER_TCP
        };

        memcpy(&clientSettings, &settings, sizeof(esp_mqtt_client_config_t));
        clientSettingsOk = true;

        client = esp_mqtt_client_init(&clientSettings);
    }

    return ret;
}

MqttManager::err_t MqttManager::start(void)
{
    err_t ret = ERR_OK;

    if(clientSettingsOk) {
        esp_mqtt_client_start(client);
    } else {
        ESP_LOGE(logTag, "MQTT settings not okay! Did you run init()?");
        ret = ERR_INVALID_CFG;
    }

    if(nullptr == client) {
        ESP_LOGE(logTag, "Failed to start MQTT client!");
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
        while(getPublishMsgInFlightCount() > 0) {
            // but poll only as long as wifi is connected
            if((xEventGroupGetBits(wifiEvents) & wifiEventDisconnected) == 0) {
                vTaskDelay(pdMS_TO_TICKS(250));
            } else {
                ESP_LOGW(logTag, "WiFi went down while waiting for all messages being sent.");
                break;
            }
        }

        ESP_LOGI(logTag, "Stopping MQTT client.");
        esp_mqtt_client_stop(client);
    }
}

esp_err_t MqttManager::clientEventHandler(esp_mqtt_event_handle_t event)
{
    //esp_mqtt_client_handle_t client = event->client;
    MqttManager* manager = (MqttManager*) event->user_context;
    
    if(nullptr == manager) {
        ESP_LOGE("unknown", "No valid MqttManager available to dispatch events to!");
    } else {
        switch (event->event_id) {
            case MQTT_EVENT_CONNECTED:
                manager->clientConnected(event);
                break;
            case MQTT_EVENT_DISCONNECTED:
                manager->clientDisconnected(event);
                break;

            case MQTT_EVENT_SUBSCRIBED:
                break;
            case MQTT_EVENT_DATA:
                manager->clientData(event);
                break;
            case MQTT_EVENT_UNSUBSCRIBED:
                break;

            case MQTT_EVENT_PUBLISHED:
                manager->clientPublished(event);
                break;

            case MQTT_EVENT_ERROR:
                ESP_LOGE(manager->logTag, "MQTT_EVENT_ERROR occurred!");
                break;
            
            default:
                ESP_LOGW(manager->logTag, "Unkown event type received!");
                break;
        }
    }

    return ESP_OK;
}

void MqttManager::clientConnected(esp_mqtt_event_handle_t eventData)
{
    ESP_LOGI(logTag, "Connected.");
    xEventGroupSetBits(clientEvents, clientEventConnected);
    xEventGroupClearBits(clientEvents, clientEventDisconnected);
}

void MqttManager::clientDisconnected(esp_mqtt_event_handle_t eventData)
{
    ESP_LOGI(logTag, "Disconnected.");
    xEventGroupClearBits(clientEvents, clientEventConnected);
    xEventGroupSetBits(clientEvents, clientEventDisconnected);
}

void MqttManager::clientPublished(esp_mqtt_event_handle_t eventData)
{
    int inFlightCnt = -1;

    if(pdFALSE == xSemaphoreTake(publishMutex, lockAcquireTimeout)) {
        ESP_LOGE(logTag, "Couldn't acquire publish lock within timeout");
    } else {
        int pos;
        for(pos=0; pos < publishMsgInFlightMax; pos++) {
            if((publishMsgInFlightInfo[pos].valid) && (publishMsgInFlightInfo[pos].msgId == eventData->msg_id)) {
                xTimerStop(publishMsgInFlightTimer[pos], portMAX_DELAY);
                publishMsgInFlightInfo[pos].valid = false;
                publishMsgInFlightCnt--;
                break;
            }
        }

        if(pos == publishMsgInFlightMax) {
            ESP_LOGW(logTag, "Publish msg_id not found. It could have timed out.");
        }

        inFlightCnt = publishMsgInFlightCnt;
        xSemaphoreGive(publishMutex);
    }

    ESP_LOGD(logTag, "Published (msg_id: 0x%04x). Current in-flight cnt: %d/%d", eventData->msg_id, inFlightCnt, publishMsgInFlightMax);
}

void MqttManager::clientData(esp_mqtt_event_handle_t eventData)
{
    unsigned int topicLen = eventData->topic_len;
    unsigned int dataLen = eventData->data_len;

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
    int msgId;

    if((xEventGroupGetBits(clientEvents) & clientEventConnected) == 0) {
        ESP_LOGW(logTag, "Publish requested, but MQTT client is disconnected.");
        ret = ERR_DISCONNECTED;
    } else {
        if(pdFALSE == xSemaphoreTake(publishMutex, lockAcquireTimeout)) {
            ESP_LOGE(logTag, "Couldn't acquire publish lock within timeout");
            ret = ERR_TIMEOUT;
        } else {
            if(publishMsgInFlightCnt == publishMsgInFlightMax) {
                ESP_LOGE(logTag, "Maximum number of in-flight publish messages reached!");
                ret = ERR_NO_RESOURCES;
            } else {
                msgId = esp_mqtt_client_publish(client, topic, data, len, qos, retain ? 1 : 0);
                if(-1 == msgId) {
                    ESP_LOGE(logTag, "Publishing failed due to mqtt_client error!");
                    ret = ERR_MQTT_CLIENT_ERR;
                } else {
                    int pos;
                    for(pos=0; pos < publishMsgInFlightMax; pos++) {
                        if(!publishMsgInFlightInfo[pos].valid) {
                            publishMsgInFlightInfo[pos].valid = true;
                            publishMsgInFlightInfo[pos].caller = this;
                            publishMsgInFlightInfo[pos].msgId = msgId;
                            publishMsgInFlightCnt++;

                            vTimerSetTimerID(publishMsgInFlightTimer[pos], (void*) &publishMsgInFlightInfo[pos]);
                            xTimerStart(publishMsgInFlightTimer[pos], portMAX_DELAY);
                            break;
                        }
                    }

                    if(pos == publishMsgInFlightMax) {
                        ESP_LOGE(logTag, "No free publishMsgInFlightInfo found! This CANNOT happen!");
                        ret = ERR_NO_RESOURCES;
                    }
                }
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
    TickType_t timeoutStep;
    bool noInflightMsgs = false;

    if(timeoutMs == -1) {
        timeout = portMAX_DELAY;
        timeoutStep = pdMS_TO_TICKS(250);
    } else {
        timeout = pdMS_TO_TICKS(timeoutMs);
        timeoutStep = pdMS_TO_TICKS(timeoutMs / 8);
    }

    while(timeout) {
        if(getPublishMsgInFlightCount() == 0) {
            noInflightMsgs = true;
            break;
        } else {
            if(timeout >= timeoutStep) {
                timeout -= timeoutStep;
            } else {
                timeout = 0;
            }
            vTaskDelay(timeoutStep);
        }
    }

    return noInflightMsgs;
}

int MqttManager::getPublishMsgInFlightCount(void)
{
    // If something fails return the maximum number, because this is the worst case
    int inFlightCnt = publishMsgInFlightMax;

    if(pdFALSE == xSemaphoreTake(publishMutex, lockAcquireTimeout)) {
        ESP_LOGE(logTag, "Couldn't acquire publish lock within timeout");
    } else {
        inFlightCnt = publishMsgInFlightCnt;

        xSemaphoreGive(publishMutex);
    }

    return inFlightCnt;
}

void MqttManager::clientPublishTimeoutDispatch(TimerHandle_t timer)
{
    publish_msg_info_t *timerInfo = (publish_msg_info_t *) pvTimerGetTimerID(timer);

    if(nullptr == timerInfo) {
        ESP_LOGE("unknown", "No valid publishMsgInFlightInfo available to dispatch timeout event!");
    } else {
        (timerInfo->caller)->clientPublishTimeout(timerInfo->msgId);
    }
}

void MqttManager::clientPublishTimeout(uint16_t msgId)
{
    if(pdFALSE == xSemaphoreTake(publishMutex, lockAcquireTimeout)) {
        ESP_LOGE(logTag, "Couldn't acquire publish lock within timeout");
    } else {
        ESP_LOGW(logTag, "Publishing timed out (msg_id: 0x%04x). Releasing it anyway.", msgId);

        int cnt;
        for(cnt=0; cnt < publishMsgInFlightMax; cnt++) {
            if(publishMsgInFlightInfo[cnt].valid && (publishMsgInFlightInfo[cnt].msgId == msgId)) {
                publishMsgInFlightInfo[cnt].valid = false;
                publishMsgInFlightCnt--;
                break;
            }
        }

        if(cnt == publishMsgInFlightMax) {
            ESP_LOGW(logTag, "Publish msg_id not found! Successful publish could have cleared it in the meantime.");
        }

        xSemaphoreGive(publishMutex);
    }
}

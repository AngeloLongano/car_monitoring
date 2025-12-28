#include "obd_bluetooth.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_spp_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "obd_bt";

static bool s_connected = false;
static uint32_t s_spp_handle = 0;
static QueueHandle_t s_rx_queue = NULL; // queue of char* (allocated packets)

// SPP callback: handle basic events and data reception
static void esp_spp_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t *param)
{
    switch (event) {
    case ESP_SPP_INIT_EVT:
        ESP_LOGI(TAG, "ESP_SPP_INIT_EVT");
        break;
    case ESP_SPP_START_EVT:
        ESP_LOGI(TAG, "ESP_SPP_START_EVT");
        break;
    case ESP_SPP_OPEN_EVT:
        ESP_LOGI(TAG, "ESP_SPP_OPEN_EVT handle=%u", param->open.handle);
        s_spp_handle = param->open.handle;
        s_connected = true;
        break;
    case ESP_SPP_CLOSE_EVT:
        ESP_LOGI(TAG, "ESP_SPP_CLOSE_EVT");
        s_connected = false;
        s_spp_handle = 0;
        break;
    case ESP_SPP_DATA_IND_EVT:
    {
        ESP_LOGI(TAG, "ESP_SPP_DATA_IND_EVT len=%d", param->data_ind.len);
        // copy data into heap buffer and push pointer to queue
        size_t len = param->data_ind.len;
        char *pkt = pvPortMalloc(len + 1);
        if (pkt) {
            memcpy(pkt, param->data_ind.data, len);
            pkt[len] = '\0';
            if (s_rx_queue) {
                if (xQueueSend(s_rx_queue, &pkt, 0) != pdTRUE) {
                    // queue full, drop
                    ESP_LOGW(TAG, "RX queue full, dropping packet");
                    vPortFree(pkt);
                }
            } else {
                vPortFree(pkt);
            }
        } else {
            ESP_LOGW(TAG, "failed to allocate RX packet");
        }
    }
        break;
    case ESP_SPP_CONG_EVT:
        ESP_LOGI(TAG, "ESP_SPP_CONG_EVT");
        break;
    default:
        ESP_LOGD(TAG, "SPP event %d", event);
        break;
    }
}

esp_err_t obd_bt_init(void)
{
    esp_err_t ret;

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(TAG, "bt controller init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (ret) {
        ESP_LOGE(TAG, "bt controller enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bluedroid_init();
    if (ret) {
        ESP_LOGE(TAG, "bluedroid init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bluedroid_enable();
    if (ret) {
        ESP_LOGE(TAG, "bluedroid enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_spp_register_callback(esp_spp_cb);
    if (ret) {
        ESP_LOGE(TAG, "esp_spp_register_callback failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // old version
    // ret = esp_spp_init(ESP_SPP_MODE_CB);
    esp_spp_cfg_t bt_spp_cfg = {
        .mode = ESP_SPP_MODE_CB,
        .enable_l2cap_ertm = true,
        .tx_buffer_size = 0, 
    };
    ret = esp_spp_enhanced_init(&bt_spp_cfg);
    if (ret) {
        ESP_LOGE(TAG, "esp_spp_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Create RX queue for incoming SPP data (queue holds pointers to malloc'd packets)
    if (!s_rx_queue) {
        s_rx_queue = xQueueCreate(16, sizeof(char*));
        if (!s_rx_queue) {
            ESP_LOGW(TAG, "failed to create RX queue");
        }
    }

    ESP_LOGI(TAG, "Bluetooth (SPP) initialized");
    return ESP_OK;
}

// Convert MAC string "AA:BB:CC:DD:EE:FF" to array of 6 bytes in reverse order for esp (BD_ADDR)
static bool mac_str_to_bda(const char *mac, uint8_t bda[6])
{
    if (!mac) return false;
    int vals[6];
    if (sscanf(mac, "%x:%x:%x:%x:%x:%x", &vals[0], &vals[1], &vals[2], &vals[3], &vals[4], &vals[5]) == 6) {
        for (int i = 0; i < 6; i++) bda[i] = (uint8_t)vals[i];
        return true;
    }
    return false;
}

int obd_bt_connect(const char *mac_str)
{
    if (!mac_str) return -1;
    if (s_connected) return 0; // already connected

    uint8_t remote_bda[6];
    if (!mac_str_to_bda(mac_str, remote_bda)) {
        ESP_LOGE(TAG, "invalid mac string");
        return -1;
    }

    // Use default security and client role, SCN is typically 1 for ELM327
    esp_err_t err = esp_spp_connect(ESP_SPP_SEC_NONE, ESP_SPP_ROLE_MASTER, 1, remote_bda);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_spp_connect failed: %s", esp_err_to_name(err));
        return -1;
    }
    ESP_LOGI(TAG, "Initiated SPP connect to %s", mac_str);
    return 0;
}

int obd_send_cmd_and_read(const char *cmd, char *out, size_t out_sz, int timeout_ms)
{
    if (!cmd || !out || out_sz == 0) return -1;
    if (!s_connected || s_spp_handle == 0) return -1;

    // Send command with CR
    size_t cmd_len = strlen(cmd);
    char *sendbuf = pvPortMalloc(cmd_len + 2);
    if (!sendbuf) return -1;
    memcpy(sendbuf, cmd, cmd_len);
    sendbuf[cmd_len] = '\r';
    sendbuf[cmd_len+1] = '\0';

    esp_err_t err = esp_spp_write(s_spp_handle, cmd_len + 1, (uint8_t *)sendbuf);
    vPortFree(sendbuf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_spp_write failed: %s", esp_err_to_name(err));
        return -1;
    }

    // Collect incoming packets from queue until we see '>' prompt or timeout
    int remaining_ms = timeout_ms;
    size_t total = 0;
    char *pkt = NULL;
    TickType_t start_tick = xTaskGetTickCount();
    while (remaining_ms > 0 && total < out_sz - 1) {
        TickType_t wait_ticks = pdMS_TO_TICKS(remaining_ms);
        if (wait_ticks == 0) wait_ticks = 1;
        if (s_rx_queue == NULL) break;
        if (xQueueReceive(s_rx_queue, &pkt, wait_ticks) == pdTRUE) {
            if (!pkt) continue;
            // copy packet into output buffer
            for (size_t i = 0; pkt[i] != '\0' && total < out_sz - 1; i++) {
                char c = pkt[i];
                out[total++] = c;
                if (c == '>') break;
            }
            vPortFree(pkt);
            pkt = NULL;
            // if last character written was '>' stop
            if (total > 0 && out[total-1] == '>') break;
        }
        // recompute remaining time
        TickType_t now = xTaskGetTickCount();
        remaining_ms = timeout_ms - (int)((now - start_tick) * portTICK_PERIOD_MS);
    }

    if (total == 0) {
        out[0] = '\0';
        return 0; // no data
    }
    // null-terminate and trim trailing CR/LF and prompt if desired
    // remove trailing '>' from output
    if (total > 0 && out[total-1] == '>') {
        out[total-1] = '\0';
        total--;
    } else {
        out[total] = '\0';
    }
    return (int)total;
}

void obd_bt_disconnect(void)
{
    if (s_connected && s_spp_handle) {
        esp_spp_disconnect(s_spp_handle);
    }
}

bool obd_bt_is_connected(void)
{
    return s_connected;
}

// Minimal parser for RPM from payload like: "41 0C AA BB"
static int parse_pid_rpm(const char *payload)
{
    if (!payload) return -1;
    int a = 0, b = 0;
    // find substring starting with "41 0C" (response for 010C)
    const char *p = strstr(payload, "41 0C");
    if (!p) p = strstr(payload, "410C");
    if (!p) return -1;
    if (sscanf(p, "41 0C %x %x", &a, &b) == 2 || sscanf(p, "410C %x %x", &a, &b) == 2) {
        return ((a << 8) | b) / 4;
    }
    return -1;
}

// Polling task: connects (if needed), sends 010C every interval_ms and logs RPM
typedef struct {
    char mac[32];
    int interval_ms;
} polling_args_t;

static void obd_polling_task(void *arg)
{
    polling_args_t *pa = (polling_args_t*)arg;
    char mac[32];
    strncpy(mac, pa->mac, sizeof(mac)-1);
    mac[sizeof(mac)-1] = '\0';
    int interval = pa->interval_ms > 0 ? pa->interval_ms : 1000;
    vPortFree(pa);

    while (1) {
        if (!obd_bt_is_connected()) {
            ESP_LOGI(TAG, "Not connected, attempting connect to %s", mac);
            if (obd_bt_connect(mac) != 0) {
                ESP_LOGW(TAG, "connect failed, retry in 2s");
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            // give some time for ELM to be ready
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        char reply[512];
        int r = obd_send_cmd_and_read("010C", reply, sizeof(reply), 3000);
        if (r < 0) {
            ESP_LOGW(TAG, "read error or not connected, disconnecting and retrying");
            obd_bt_disconnect();
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        int rpm = parse_pid_rpm(reply);
        if (rpm >= 0) {
            ESP_LOGI(TAG, "RPM: %d (raw reply: %s)", rpm, reply);
        } else {
            ESP_LOGW(TAG, "Failed to parse RPM, reply: %s", reply);
        }

        vTaskDelay(pdMS_TO_TICKS(interval));
    }
}

esp_err_t obd_start_polling(const char *mac_str, int interval_ms)
{
    if (!mac_str) return ESP_ERR_INVALID_ARG;
    polling_args_t *pa = pvPortMalloc(sizeof(polling_args_t));
    if (!pa) return ESP_ERR_NO_MEM;
    strncpy(pa->mac, mac_str, sizeof(pa->mac)-1);
    pa->mac[sizeof(pa->mac)-1] = '\0';
    pa->interval_ms = interval_ms;

    BaseType_t ok = xTaskCreate(obd_polling_task, "obd_poll", 4096, pa, tskIDLE_PRIORITY+4, NULL);
    if (ok != pdPASS) {
        vPortFree(pa);
        return ESP_FAIL;
    }
    return ESP_OK;
}

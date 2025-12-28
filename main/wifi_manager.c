#include "wifi_manager.h"
#include "wifi_credentials.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"

/* Separate tags for different log categories */
static const char *TAG_CONFIG = "wifi_config";   // Initialization / configuration messages
static const char *TAG_CONN   = "wifi_conn";     // Connection / event messages
static const char *TAG_METRICS= "wifi_metrics";  // Periodic metrics messages

/* Counter for connection retry attempts */
static int s_retry_num = 0;

/* Task handle for periodic metrics logging (created on successful connection) */
static TaskHandle_t s_metrics_task_handle = NULL;

/* Task handle for periodic WiFi reconnection attempts */
static TaskHandle_t s_reconnect_task_handle = NULL;

/* Flag to track if connected to WiFi */
static volatile bool s_wifi_connected = false;

/* Flag to indicate we're attempting direct connection (disable retries during this phase) */
static volatile bool s_direct_connect_attempt = false;

/* Current SSID being connected to */
static char s_current_ssid[33] = {0};  // 32 chars max for SSID + null terminator

/* Forward declaration of event handler */
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                             int32_t event_id, void* event_data);

/* Convert RSSI (dBm) to a 0-100 signal strength percentage */
static int rssi_to_percent(int8_t rssi)
{
    if (rssi <= -100) return 0;
    if (rssi >= -50) return 100;
    return 2 * (rssi + 100);
}

/**
 * @brief Scans for WiFi networks and connects to the strongest known network
 * 
 * This function performs the following steps:
 * 1. Initializes NVS, TCP/IP stack, and WiFi (once)
 * 2. First tries direct connection for networks with direct_connect=true
 * 3. If no direct connection succeeds, scans and connects to the best known network
 * 4. Retries up to MAXIMUM_RETRY times if scan fails
 */
void wifi_scan_and_connect(void)
{
    /* Initialize NVS (only once) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    /* Initialize TCP/IP stack and event loop (only once) */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    /* Initialize WiFi with default configuration (only once) */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    /* Set WiFi mode to station */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    /* Register event handlers for WiFi and IP events (only once) */
    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                      ESP_EVENT_ANY_ID,
                                                      &wifi_event_handler,
                                                      NULL,
                                                      &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                      IP_EVENT_STA_GOT_IP,
                                                      &wifi_event_handler,
                                                      NULL,
                                                      &instance_got_ip));
    
    /* First, try direct connection for networks marked with direct_connect=true */
    for (int i = 0; i < KNOWN_NETWORKS_COUNT; i++) {
        if (KNOWN_NETWORKS[i].direct_connect) {
            ESP_LOGI(TAG_CONFIG, "Attempting direct connection to: %s", KNOWN_NETWORKS[i].ssid);
            
            s_direct_connect_attempt = true;
            s_retry_num = 0;  // Reset retry counter
            
            wifi_config_t wifi_config = {
                .sta = {
                    .threshold.authmode = WIFI_AUTH_WPA2_PSK,
                    .pmf_cfg = {
                        .capable = false,
                        .required = false
                    },
                    .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
                    .sae_h2e_identifier = "",
                },
            };
            
            strncpy((char *)wifi_config.sta.ssid, KNOWN_NETWORKS[i].ssid, sizeof(wifi_config.sta.ssid) - 1);
            strncpy((char *)wifi_config.sta.password, KNOWN_NETWORKS[i].password, sizeof(wifi_config.sta.password) - 1);
            strncpy(s_current_ssid, KNOWN_NETWORKS[i].ssid, sizeof(s_current_ssid) - 1);
            
            ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
            ESP_ERROR_CHECK(esp_wifi_connect());
            
            /* Wait for connection to succeed (up to 15 seconds for IP assignment) */
            int wait_time = 0;
            while (wait_time < 15000 && !s_wifi_connected) {
                vTaskDelay(pdMS_TO_TICKS(100));
                wait_time += 100;
            }
            
            s_direct_connect_attempt = false;
            
            /* If connected, we're done */
            if (s_wifi_connected) {
                ESP_LOGI(TAG_CONFIG, "Direct connection successful!");
                return;
            }
            
            /* Otherwise, disconnect and try next direct connect */
            ESP_LOGW(TAG_CONFIG, "Direct connection to %s failed, trying next network...", KNOWN_NETWORKS[i].ssid);
            esp_wifi_disconnect();
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    
    /* If direct connection failed, proceed with scan-based connection */
    int attempt = 0;
    
    while (attempt < MAXIMUM_RETRY) {
        attempt++;
        ESP_LOGI(TAG_CONFIG, "Scan connection attempt %d/%d", attempt, MAXIMUM_RETRY);
        
        /* Perform WiFi scan */
        ESP_LOGI(TAG_CONFIG, "Starting WiFi scan...");
        
        wifi_scan_config_t scan_config = {
            .ssid = NULL,           // Scan all SSIDs
            .bssid = NULL,
            .channel = 0,           // Scan all channels
            .show_hidden = true,    // Include hidden networks
            .scan_type = WIFI_SCAN_TYPE_ACTIVE,
            .scan_time.active.min = 100,
            .scan_time.active.max = 300,
        };
        
        ESP_ERROR_CHECK(esp_wifi_scan_start(&scan_config, true));  // true = blocking scan
        
        uint16_t ap_count = 0;
        ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&ap_count));
        
        if (ap_count == 0) {
            ESP_LOGW(TAG_CONFIG, "No networks found, retrying...");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        
        ESP_LOGI(TAG_CONFIG, "Found %d networks", ap_count);
        
        wifi_ap_record_t *ap_list = (wifi_ap_record_t *)malloc(ap_count * sizeof(wifi_ap_record_t));
        if (ap_list == NULL) {
            ESP_LOGE(TAG_CONFIG, "Failed to allocate memory for AP list");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        
        ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_count, ap_list));
        
        /* Log all found networks */
        ESP_LOGI(TAG_CONFIG, "Available networks:");
        for (int i = 0; i < ap_count; i++) {
            ESP_LOGI(TAG_CONFIG, "  [%d] SSID: %s (RSSI: %d dBm)", i+1, ap_list[i].ssid, ap_list[i].rssi);
        }
        
        /* Find the best known network (strongest signal) */
        int best_network_idx = -1;
        int8_t best_rssi = -120;  // Worst possible RSSI
        
        for (int i = 0; i < ap_count; i++) {
            ESP_LOGD(TAG_CONFIG, "Found AP: SSID=%s, RSSI=%d, authmode=%d", 
                     ap_list[i].ssid, ap_list[i].rssi, ap_list[i].authmode);
            
            /* Check if this SSID is in our known networks list */
            for (int j = 0; j < KNOWN_NETWORKS_COUNT; j++) {
                if (strcmp((const char *)ap_list[i].ssid, KNOWN_NETWORKS[j].ssid) == 0) {
                    /* Found a known network - keep track of strongest one */
                    if (ap_list[i].rssi > best_rssi) {
                        best_rssi = ap_list[i].rssi;
                        best_network_idx = j;
                        ESP_LOGI(TAG_CONFIG, "Found known network: %s (RSSI: %d dBm)", 
                                 KNOWN_NETWORKS[j].ssid, ap_list[i].rssi);
                    }
                    break;
                }
            }
        }
        
        free(ap_list);
        
        if (best_network_idx == -1) {
            ESP_LOGW(TAG_CONFIG, "No known networks found, retrying...");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        /* Connect to the best known network */
        ESP_LOGI(TAG_CONFIG, "Connecting to: %s (RSSI: %d dBm)", 
                 KNOWN_NETWORKS[best_network_idx].ssid, best_rssi);
        
        wifi_config_t wifi_config = {
            .sta = {
                .threshold.authmode = WIFI_AUTH_WPA2_PSK,
                .pmf_cfg = {
                    .capable = false,
                    .required = false
                },
                .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
                .sae_h2e_identifier = "",
            },
        };
        
        strncpy((char *)wifi_config.sta.ssid, KNOWN_NETWORKS[best_network_idx].ssid, sizeof(wifi_config.sta.ssid) - 1);
        strncpy((char *)wifi_config.sta.password, KNOWN_NETWORKS[best_network_idx].password, sizeof(wifi_config.sta.password) - 1);
        
        /* Save the current SSID for use in event handler */
        strncpy(s_current_ssid, KNOWN_NETWORKS[best_network_idx].ssid, sizeof(s_current_ssid) - 1);
        
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_connect());

        ESP_LOGI(TAG_CONFIG, "wifi_scan_and_connect finished");
        return;  // Successfully started connection attempt
    }
    
    ESP_LOGE(TAG_CONFIG, "Failed to find and connect to known network after %d attempts", MAXIMUM_RETRY);
}

/* Task that periodically logs RSSI and a computed link quality */
static void wifi_metrics_task(void *arg)
{
    (void)arg;
    for (;;) {
        wifi_ap_record_t ap_info;
        esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);
        if (err == ESP_OK) {
            int rssi = ap_info.rssi;
            int quality = rssi_to_percent((int8_t)rssi);
            ESP_LOGI(TAG_METRICS, "Metrics - RSSI: %d dBm, Link quality: %d%%, SSID: %s, channel: %d", rssi, quality, ap_info.ssid, ap_info.primary);
        } else {
            ESP_LOGW(TAG_METRICS, "Could not get AP info (esp_err: 0x%x).", err);
        }
        vTaskDelay(pdMS_TO_TICKS(10000)); // 10 seconds between samples
    }
}

/**
 * @brief Task that periodically attempts to reconnect to WiFi
 * 
 * This task runs in the background and checks if WiFi is connected.
 * If not connected, it attempts to rescan and reconnect every 5 minutes.
 */
static void wifi_reconnect_task(void *arg)
{
    (void)arg;
    const int RECONNECT_INTERVAL_MS = 5 * 60 * 1000;  // 5 minutes
    
    for (;;) {
        /* Check if connected */
        if (!s_wifi_connected) {
            ESP_LOGW(TAG_CONN, "WiFi disconnected, attempting to reconnect...");
            wifi_scan_and_connect();
        }
        
        vTaskDelay(pdMS_TO_TICKS(RECONNECT_INTERVAL_MS));
    }
}

/**
 * @brief Event handler for WiFi events
 * 
 * This callback function handles three main events:
 * 1. WIFI_EVENT_STA_START: Initial WiFi startup
 * 2. WIFI_EVENT_STA_DISCONNECTED: WiFi disconnection
 * 3. IP_EVENT_STA_GOT_IP: Successful IP address acquisition
 * 
 * @param arg User-provided argument (unused)
 * @param event_base Base ID of the event
 * @param event_id ID of the event
 * @param event_data Event-specific data
 */
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                             int32_t event_id, void* event_data)
{
    
    /* Handle WiFi station start - Initial connection attempt */
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG_CONN, "Trying to connect to SSID: %s", s_current_ssid);
        esp_wifi_connect();
    }
    /* Handle WiFi station start - Initial connection attempt */
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG_CONN, "Trying to connect to SSID: %s", s_current_ssid);
        esp_wifi_connect();
    }
    /* Handle WiFi disconnection - stop metrics task and attempt reconnection if under retry limit */
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* event = (wifi_event_sta_disconnected_t*) event_data;
        const char* reason_str;
        switch (event->reason) {
            case WIFI_REASON_AUTH_EXPIRE:
                reason_str = "Auth Expired";
                break;
            case WIFI_REASON_AUTH_LEAVE:
                reason_str = "Auth Leave";
                break;
            case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
                reason_str = "4-way Handshake Timeout (likely wrong password)";
                break;
            case WIFI_REASON_HANDSHAKE_TIMEOUT:
                reason_str = "Handshake Timeout";
                break;
            default:
                reason_str = "Unknown";
        }
        /* During direct connection attempt, don't retry yet */
        if (s_direct_connect_attempt) {
            return;
        }

        ESP_LOGW(TAG_CONN, "Disconnected from AP, reason: %d (%s)", event->reason, reason_str);

        /* If metrics task is running, stop it to avoid stale logging */
        if (s_metrics_task_handle != NULL) {
            vTaskDelete(s_metrics_task_handle);
            s_metrics_task_handle = NULL;
            ESP_LOGI(TAG_METRICS, "Stopped metrics task due to disconnect");
        }
        
        s_wifi_connected = false;  // Set disconnected flag

        if (s_retry_num < MAXIMUM_RETRY) {
            vTaskDelay(pdMS_TO_TICKS(1000)); // Add delay before retry
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG_CONN, "Retry %d/%d to connect to the AP: %s", s_retry_num, MAXIMUM_RETRY, s_current_ssid);
        } else {
            ESP_LOGE(TAG_CONN, "Failed to connect to SSID: %s after %d attempts", s_current_ssid, MAXIMUM_RETRY);
            ESP_LOGE(TAG_CONN, "Please verify: 1) Password is correct 2) Network is reachable 3) Signal is strong");
        }
    }
    /* Handle successful IP address acquisition */
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG_CONN, "Connected! Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0; // Reset retry counter on successful connection
        s_wifi_connected = true;  // Set connected flag

        /* Start metrics task if not already running */
        if (s_metrics_task_handle == NULL) {
            BaseType_t xres = xTaskCreate(wifi_metrics_task, "wifi_metrics", 4096, NULL, 5, &s_metrics_task_handle);
            if (xres == pdPASS) {
                ESP_LOGI(TAG_METRICS, "Started metrics task");
            } else {
                ESP_LOGW(TAG_METRICS, "Failed to start metrics task (xTaskCreate returned %d)", (int)xres);
                s_metrics_task_handle = NULL;
            }
        }
        
        /* Start reconnect task if not already running */
        if (s_reconnect_task_handle == NULL) {
            BaseType_t xres = xTaskCreate(wifi_reconnect_task, "wifi_reconnect", 4096, NULL, 4, &s_reconnect_task_handle);
            if (xres == pdPASS) {
                ESP_LOGI(TAG_CONN, "Started WiFi reconnect task");
            } else {
                ESP_LOGW(TAG_CONN, "Failed to start reconnect task");
                s_reconnect_task_handle = NULL;
            }
        }
    }
}
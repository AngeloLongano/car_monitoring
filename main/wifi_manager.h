#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

/**
 * @file wifi_manager.h
 * @brief WiFi station mode configuration and management
 * 
 * This header file defines the WiFi configuration parameters and
 * declares functions for WiFi station mode initialization.
 */

/* Required ESP-IDF components */
#include "esp_wifi.h"      // Main WiFi driver
#include "esp_event.h"     // Event handling
#include "esp_netif.h"     // TCP/IP stack
#include "nvs_flash.h"     // Non-volatile storage

/* WiFi Configuration Parameters */
#define MAXIMUM_RETRY  10                  // Maximum connection attempts before giving up

/**
 * @brief Initializes and starts the WiFi station
 * 
 * This function:
 * 1. Initializes the TCP/IP stack
 * 2. Creates the WiFi station interface
 * 3. Configures the WiFi driver
 * 4. Sets up event handlers
 * 5. Starts the connection process
 */
void wifi_init_sta(void);

/**
 * @brief Scan for available WiFi networks and connect to the strongest known one
 * 
 * This function:
 * 1. Performs a WiFi scan to discover available networks
 * 2. Compares found networks against KNOWN_NETWORKS list
 * 3. Connects to the network with the strongest signal (highest RSSI)
 */
void wifi_scan_and_connect(void);

#endif // WIFI_MANAGER_H
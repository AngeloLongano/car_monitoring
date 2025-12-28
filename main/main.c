/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "wifi_manager.h"
#include "usb_storage.h"



void app_main(void)
{
    printf("Hello world!\n");

    // wifi_scan_and_connect();
    // usb_main_test();
    
    
}

idf_component_register(SRCS "usb_storage.c" "main.c" "wifi_manager.c" "obd_bluetooth.c"
                       PRIV_REQUIRES spi_flash
                       INCLUDE_DIRS "."
                       REQUIRES nvs_flash esp_wifi fatfs vfs usb bt
                       )
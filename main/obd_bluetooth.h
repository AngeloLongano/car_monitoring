#ifndef OBD_BLUETOOTH_H
#define OBD_BLUETOOTH_H

#include <stdbool.h>
#include <esp_err.h>

#define MAC_ADDRESS_OBD  "AA:BB:CC:DD:EE:FF" // Replace with your OBD-II device MAC address

// Initialize Bluetooth stack for Classic SPP
esp_err_t obd_bt_init(void);

// Connect to ELM327 device by MAC address string "AA:BB:CC:DD:EE:FF".
// Returns 0 on success, negative on error.
int obd_bt_connect(const char *mac_str);

// Send command (without trailing CR), read response into buffer (null-terminated).
// timeout_ms: total timeout to wait for response. Returns number of bytes read or -1 on error.
int obd_send_cmd_and_read(const char *cmd, char *out, size_t out_sz, int timeout_ms);

// Disconnect current connection
void obd_bt_disconnect(void);

// Returns true if connected
bool obd_bt_is_connected(void);

// Start polling task: connect to given MAC and poll specified PID every interval_ms milliseconds.
esp_err_t obd_start_polling(const char *mac_str, int interval_ms);


#endif // OBD_BLUETOOTH_H

#ifndef USB_STORAGE_H
#define USB_STORAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <esp_err.h>

/**
 * Initialize USB storage helper.
 * mount_point should be the VFS mount point (e.g. "/usb").
 */
esp_err_t usb_storage_init(const char *mount_point);

/** Deinitialize and free resources. */
void usb_storage_deinit(void);

/**
 * Write data atomically to relative path under mount point.
 * Returns 0 on success, -1 on error.
 */
int usb_write_atomic(const char *relpath, const void *data, size_t len);

/** Append a single line (adds newline) to a log file. */
int usb_append_log(const char *relpath, const char *line);

/** Return true if file exists at relative path. */
bool usb_file_exists(const char *relpath);

void usb_main_test(void);

#endif // USB_STORAGE_H

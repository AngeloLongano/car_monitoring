#include "usb_storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "usb_storage.h"
#include "usb/usb_host.h"

static const char *TAG = "usb_storage";

static char s_mount_point[128] = {0};
static SemaphoreHandle_t s_usb_mutex = NULL;

static int mkdir_p(const char *path)
{
    char tmp[256];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (len == 0) return -1;
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0) {
                if (errno != EEXIST) return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0) {
        if (errno != EEXIST) return -1;
    }
    return 0;
}

esp_err_t usb_storage_init(const char *mount_point)
{
    if (!mount_point) return ESP_ERR_INVALID_ARG;
    if (!s_usb_mutex) {
        s_usb_mutex = xSemaphoreCreateMutex();
        if (!s_usb_mutex) return ESP_ERR_NO_MEM;
    }
    strncpy(s_mount_point, mount_point, sizeof(s_mount_point) - 1);
    ESP_LOGI(TAG, "initialized with mount point %s", s_mount_point);
    return ESP_OK;
}

void usb_storage_deinit(void)
{
    if (s_usb_mutex) {
        vSemaphoreDelete(s_usb_mutex);
        s_usb_mutex = NULL;
    }
    s_mount_point[0] = '\0';
}

static void build_full_path(char *out, size_t out_sz, const char *relpath)
{
    if (s_mount_point[0] == '\0') {
        snprintf(out, out_sz, "%s", relpath);
    } else {
        snprintf(out, out_sz, "%s/%s", s_mount_point, relpath);
    }
}

int usb_write_atomic(const char *relpath, const void *data, size_t len)
{
    if (!relpath || (!data && len > 0)) return -1;
    if (!s_usb_mutex) return -1;

    char full_path[256];
    char tmp_path[288];
    build_full_path(full_path, sizeof(full_path), relpath);
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", full_path);

    // Ensure parent dir exists
    char dirbuf[256];
    strncpy(dirbuf, full_path, sizeof(dirbuf));
    char *last = strrchr(dirbuf, '/');
    if (last) {
        *last = '\0';
        if (mkdir_p(dirbuf) != 0) {
            ESP_LOGW(TAG, "failed to ensure dir %s", dirbuf);
        }
    }

    if (xSemaphoreTake(s_usb_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "mutex take failed");
        return -1;
    }

    int fd = open(tmp_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        ESP_LOGE(TAG, "open tmp %s failed: %s", tmp_path, strerror(errno));
        xSemaphoreGive(s_usb_mutex);
        return -1;
    }

    ssize_t written = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    while (written < (ssize_t)len) {
        ssize_t w = write(fd, ptr + written, len - written);
        if (w < 0) {
            ESP_LOGE(TAG, "write error: %s", strerror(errno));
            close(fd);
            unlink(tmp_path);
            xSemaphoreGive(s_usb_mutex);
            return -1;
        }
        written += w;
    }

    if (fsync(fd) != 0) {
        ESP_LOGW(TAG, "fsync failed: %s", strerror(errno));
    }
    close(fd);

    if (rename(tmp_path, full_path) != 0) {
        ESP_LOGE(TAG, "rename failed: %s", strerror(errno));
        unlink(tmp_path);
        xSemaphoreGive(s_usb_mutex);
        return -1;
    }

    xSemaphoreGive(s_usb_mutex);
    return 0;
}

int usb_append_log(const char *relpath, const char *line)
{
    if (!relpath || !line) return -1;
    if (!s_usb_mutex) return -1;

    char full_path[256];
    build_full_path(full_path, sizeof(full_path), relpath);

    // Ensure parent dir exists
    char dirbuf[256];
    strncpy(dirbuf, full_path, sizeof(dirbuf));
    char *last = strrchr(dirbuf, '/');
    if (last) {
        *last = '\0';
        if (mkdir_p(dirbuf) != 0) {
            ESP_LOGW(TAG, "failed to ensure dir %s", dirbuf);
        }
    }

    if (xSemaphoreTake(s_usb_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "mutex take failed");
        return -1;
    }

    int fd = open(full_path, O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (fd < 0) {
        ESP_LOGE(TAG, "open %s failed: %s", full_path, strerror(errno));
        xSemaphoreGive(s_usb_mutex);
        return -1;
    }

    size_t line_len = strlen(line);
    ssize_t w = write(fd, line, line_len);
    if (w != (ssize_t)line_len) {
        ESP_LOGE(TAG, "partial write: %zd/%zu", w, line_len);
        close(fd);
        xSemaphoreGive(s_usb_mutex);
        return -1;
    }
    // add newline
    if (write(fd, "\n", 1) != 1) {
        ESP_LOGW(TAG, "failed to write newline");
    }

    if (fsync(fd) != 0) {
        ESP_LOGW(TAG, "fsync failed: %s", strerror(errno));
    }
    close(fd);

    xSemaphoreGive(s_usb_mutex);
    return 0;
}

bool usb_file_exists(const char *relpath)
{
    if (!relpath) return false;
    char full_path[256];
    build_full_path(full_path, sizeof(full_path), relpath);
    struct stat st;
    return stat(full_path, &st) == 0;
}


static void usb_mount_test_task(void *arg)
{
    const char *candidates[] = {"/usb0", "/usb", NULL};
    const char **p;
    char path[64] = {0};
    
    // Wait for USB mount point to appear (check every 10 seconds)
    while (1) {
        for (p = candidates; *p != NULL; p++) {
            struct stat st;
            if (stat(*p, &st) == 0) {
                strncpy(path, *p, sizeof(path)-1);
                path[sizeof(path)-1] = '\0';
                goto mounted;
            }
        }
        ESP_LOGI(TAG, "ho provato a connettermi ma non ho trovato la chiavetta");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

mounted:
    ESP_LOGI(TAG, "Detected USB mount point: %s", path);
    if (usb_storage_init(path) == ESP_OK) {
        const char *sample = "{\"ts\":0, \"rpm\":900}";
        if (usb_append_log("logs/test-log.json", sample) == 0) {
            ESP_LOGI(TAG, "usb_append_log: OK");
        } else {
            ESP_LOGE(TAG, "usb_append_log: FAILED");
        }
    } else {
        ESP_LOGE(TAG, "usb_storage_init failed");
    }
    vTaskDelete(NULL);
}

// /dev/tty.usbmodem5AF61057681
void usb_main_test(void)
{
    // Initialize USB Host
    ESP_LOGI(TAG, "Installing USB Host driver...");
    usb_host_config_t host_cfg = {0};
    ESP_ERROR_CHECK(usb_host_install(&host_cfg));
    ESP_LOGI(TAG, "USB Host driver installed");

    // Start task that waits for USB mount point and runs write test
    xTaskCreatePinnedToCore(
        usb_mount_test_task,
        "usb_mount_test",
        4096,
        NULL,
        tskIDLE_PRIORITY + 5,
        NULL,
        tskNO_AFFINITY
    );
}
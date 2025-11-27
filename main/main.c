#include <stdio.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "tpms_config.h"
#include "ble_scanner.h"
#include "speaker.h"
#include "screen.h"
#include "button.h"

static const char *TAG = "MAIN";

void app_main(void) {
    // Initialize NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    // Initialize TPMS config component
    tpms_config_init();
    tpms_config_load();
    tpms_config_update_devices_by_swap_mode(tpms_config_get()->tire_swap);

    // Initialize speaker component
    speaker_init();
    
    // Create speaker queue
    QueueHandle_t speaker_queue = xQueueCreate(5, sizeof(sensor_data_t));
    if (speaker_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create speaker queue");
        return;
    }
    ESP_LOGI(TAG, "Speaker queue created");
    
    // Start speaker task
    speaker_task_start(speaker_queue);

    // Initialize BLE scanner
    ble_scanner_init();
    ble_scanner_start(speaker_queue);

    // Initialize screen component
    screen_init();
    
    // Create UI queue
    QueueHandle_t ui_queue = xQueueCreate(8, sizeof(ui_update_t));
    if (ui_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create UI queue");
        return;
    }
    
    // Start screen task
    screen_task_start(ui_queue);

    // Initialize button component
    button_init();
    button_task_start(ui_queue);
    
    ESP_LOGI(TAG, "All components started");
}


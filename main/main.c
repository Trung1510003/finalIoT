#include <stdio.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#include "tpms_config.h"
#include "ble_scanner.h"
#include "speaker.h"
#include "screen.h"
#include "button.h"

static const char *TAG = "MAIN";

// Event Group bits for system initialization
#define BIT_CONFIG_READY      (1 << 0)
#define BIT_SPEAKER_READY     (1 << 1)
#define BIT_BLE_READY         (1 << 2)
#define BIT_SCREEN_READY      (1 << 3)
#define BIT_BUTTON_READY      (1 << 4)
#define ALL_SYSTEM_READY      (BIT_CONFIG_READY | BIT_SPEAKER_READY | BIT_BLE_READY | BIT_SCREEN_READY | BIT_BUTTON_READY)

// Task Notification bits (example usage)
#define NOTIFY_SENSOR_UPDATE  (1UL << 0)
#define NOTIFY_CONFIG_CHANGED (1UL << 1)

// Example task using Task Notification (lightweight alternative to queue)
static void notification_demo_task(void* pvParameters) {
    TaskHandle_t screen_task = (TaskHandle_t)pvParameters;
    TickType_t last_wake_time = xTaskGetTickCount();
    
    ESP_LOGI(TAG, "Notification demo task started");
    
    for (;;) {
        // Wait for notification with timeout
        uint32_t notification_value = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
        
        if (notification_value != 0) {
            ESP_LOGI(TAG, "Received notification: 0x%lx", notification_value);
            
            // Example: Notify screen task when sensor data is updated
            if (screen_task != NULL && (notification_value & NOTIFY_SENSOR_UPDATE)) {
                // Send notification to screen task (lightweight signal)
                xTaskNotify(screen_task, NOTIFY_SENSOR_UPDATE, eSetBits);
                ESP_LOGI(TAG, "Notified screen task about sensor update");
            }
        } else {
            // Timeout - periodic check
            ESP_LOGD(TAG, "Notification demo task: no notification received");
        }
        
        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(10000)); // 10 second period
    }
}

void app_main(void) {
    // Initialize NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }

    // Create Event Group for system initialization synchronization
    EventGroupHandle_t system_ready_events = xEventGroupCreate();
    if (system_ready_events == NULL) {
        ESP_LOGE(TAG, "Failed to create event group");
        return;
    }
    ESP_LOGI(TAG, "Event group created for system synchronization");

    // Initialize TPMS config component
    tpms_config_init();
    tpms_config_load();
    tpms_config_update_devices_by_swap_mode(tpms_config_get()->tire_swap);
    xEventGroupSetBits(system_ready_events, BIT_CONFIG_READY);
    ESP_LOGI(TAG, "Config component ready");

    // Initialize speaker component
    speaker_init();
    
    // Create speaker queue (small depth to avoid audio backlog)
    QueueHandle_t speaker_queue = xQueueCreate(1, sizeof(sensor_data_t));
    if (speaker_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create speaker queue");
        return;
    }
    ESP_LOGI(TAG, "Speaker queue created");
    
    // Start speaker task (will set BIT_SPEAKER_READY when ready)
    speaker_task_start(speaker_queue, system_ready_events);

    // Initialize BLE scanner
    ble_scanner_init();
    // Start BLE scanning (will set BIT_BLE_READY when ready)
    ble_scanner_start(speaker_queue, system_ready_events);

    // Initialize screen component
    screen_init();
    
    // Create UI queue
    QueueHandle_t ui_queue = xQueueCreate(8, sizeof(ui_update_t));
    if (ui_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create UI queue");
        return;
    }
    
    // Start screen task (will set BIT_SCREEN_READY when ready)
    screen_task_start(ui_queue, system_ready_events);

    // Initialize button component
    button_init();
    // Start button task (will set BIT_BUTTON_READY when ready)
    button_task_start(ui_queue, system_ready_events);
    
    // Wait for all components to be ready
    ESP_LOGI(TAG, "Waiting for all components to be ready...");
    EventBits_t bits = xEventGroupWaitBits(
        system_ready_events,
        ALL_SYSTEM_READY,
        pdTRUE,  // Clear bits on exit
        pdTRUE,  // Wait for all bits
        portMAX_DELAY
    );
    
    if ((bits & ALL_SYSTEM_READY) == ALL_SYSTEM_READY) {
        ESP_LOGI(TAG, "✅ All components ready! System fully initialized.");
    } else {
        ESP_LOGW(TAG, "⚠️ Some components may not be ready. Bits: 0x%x", bits);
    }
    
    // Example: Start a demo task using Task Notification
    // Get screen task handle for notification demo
    TaskHandle_t screen_task_handle = screen_get_task_handle();
    if (screen_task_handle != NULL) {
        xTaskCreate(notification_demo_task, "notif_demo", 2048, 
                   (void*)screen_task_handle, 1, NULL);
        ESP_LOGI(TAG, "Task notification demo task started");
    } else {
        ESP_LOGW(TAG, "Screen task handle not available, skipping notification demo");
    }
    
    // Example: Demonstrate binary semaphore usage
    SemaphoreHandle_t device_sem = ble_scanner_get_device_detected_sem();
    if (device_sem != NULL) {
        ESP_LOGI(TAG, "Binary semaphore available for device detection signaling");
        // In a real application, another task could wait on this semaphore
        // to be notified when a new BLE device is detected
    }
}


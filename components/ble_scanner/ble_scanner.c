#include "ble_scanner.h"
#include "tpms_config.h"
#include "speaker.h"  // For speaker_notify_sensor_detected
#include "esp_log.h"
#include "esp_bt.h"
#include "bt_hci_common.h"
#include <string.h>
#include <stdlib.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/event_groups.h>
#include <freertos/timers.h>

static const char *TAG = "BLE_SCANNER";

// Pressure thresholds
#define PRESSURE_HIGH_THRESHOLD 180
#define PRESSURE_LOW_THRESHOLD 179
#define SPEAKER_ALERT_INTERVAL_MS 8000

// Global sensor data variables
int   front_left_updated = 0;
float front_left_voltage = 0.0f;
int   front_left_temperature = 0;
uint16_t front_left_pressure_psi_x10 = 0;
float front_left_pressure_psi = 0.0f;

int   front_right_updated = 0;
float front_right_voltage = 0.0f;
int   front_right_temperature = 0;
uint16_t front_right_pressure_psi_x10 = 0;
float front_right_pressure_psi = 0.0f;

int   rear_left_updated = 0;
float rear_left_voltage = 0.0f;
int   rear_left_temperature = 0;
uint16_t rear_left_pressure_psi_x10 = 0;
float rear_left_pressure_psi = 0.0f;

int   rear_right_updated = 0;
float rear_right_voltage = 0.0f;
int   rear_right_temperature = 0;
uint16_t rear_right_pressure_psi_x10 = 0;
float rear_right_pressure_psi = 0.0f;

// Internal structures
typedef struct {
    char scan_local_name[32];
    uint8_t name_len;
} ble_scan_local_name_t;

typedef struct {
    uint8_t event_type;
    uint8_t addr_type;
    uint8_t addr[6];
    uint8_t data_len;
    int8_t rssi;
} adv_report_t;

static uint8_t hci_cmd_buf[128];
static QueueHandle_t s_speaker_queue = NULL;
static TimerHandle_t s_speaker_alert_timer = NULL;

// Alert throttling
static sensor_data_t s_pending_alert = {0};
static bool s_pending_alert_valid = false;
static SemaphoreHandle_t s_alert_mutex = NULL;

// Mutex for protecting global sensor data variables
static SemaphoreHandle_t sensor_data_mutex = NULL;

// Timer for BLE scan status check/restart (optional periodic check)
static TimerHandle_t s_ble_scan_timer = NULL;
#define BLE_SCAN_CHECK_INTERVAL_MS 30000  // Check every 30 seconds

static void speaker_alert_timer_callback(TimerHandle_t xTimer);
static void schedule_speaker_alert(const sensor_data_t *sensor_data);

// Optimized address conversion
static void address_to_string(const uint8_t *addr, char *str, size_t str_len) {
    snprintf(str, str_len, "%02x:%02x:%02x:%02x:%02x:%02x",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
}

// Optimized device lookup
static const ai_device_t* get_device_by_address(const uint8_t *addr) {
    char addr_str[18];
    address_to_string(addr, addr_str, sizeof(addr_str));
    
    ai_device_t* devices = tpms_config_get_devices();
    for (int i = 0; i < 4; i++) {
        if (strcmp(addr_str, devices[i].address) == 0) {
            return &devices[i];
        }
    }
    return NULL;
}

// Optimized sensor data parsing
static sensor_data_t parse_sensor_data(const uint8_t *raw_data, uint8_t data_len, const uint8_t *addr) {
    sensor_data_t sensor_data = {0};

    if (data_len < 21) {
        ESP_LOGW(TAG, "Insufficient data length: %d", data_len);
        return sensor_data;
    }

    const uint8_t *sensor_bytes = &raw_data[data_len - 21];
    sensor_data.temperature = sensor_bytes[1];
    sensor_data.battery_level = sensor_bytes[2] / 10.0f;
    sensor_data.pressure = (sensor_bytes[3] << 8) | sensor_bytes[4];

    const ai_device_t *device = get_device_by_address(addr);
    if (device != NULL) {
        strncpy(sensor_data.device_name, device->name, sizeof(sensor_data.device_name));
    } else {
        strcpy(sensor_data.device_name, "UNK"); // Unknown
    }

    address_to_string(addr, sensor_data.address, sizeof(sensor_data.address));
    return sensor_data;
}

// Optimized local name extraction
static esp_err_t get_local_name(const uint8_t *data_msg, uint8_t data_len, ble_scan_local_name_t *scanned_packet) {
    uint8_t curr_ptr = 0;

    while (curr_ptr < data_len) {
        uint8_t curr_len = data_msg[curr_ptr++];
        if (curr_len == 0) return ESP_FAIL;

        uint8_t curr_type = data_msg[curr_ptr++];
        if (curr_type == 0x08 || curr_type == 0x09) {
            uint8_t name_len = (curr_len - 1 < sizeof(scanned_packet->scan_local_name)) ?
                              curr_len - 1 : sizeof(scanned_packet->scan_local_name) - 1;

            memcpy(scanned_packet->scan_local_name, &data_msg[curr_ptr], name_len);
            scanned_packet->scan_local_name[name_len] = '\0';
            scanned_packet->name_len = name_len;
            return ESP_OK;
        }
        curr_ptr += curr_len - 1;
    }
    return ESP_FAIL;
}

static void schedule_speaker_alert(const sensor_data_t *sensor_data)
{
    if (sensor_data == NULL || s_alert_mutex == NULL || s_speaker_alert_timer == NULL) {
        return;
    }

    if (xSemaphoreTake(s_alert_mutex, portMAX_DELAY) == pdTRUE) {
        s_pending_alert = *sensor_data;
        s_pending_alert_valid = true;
        xSemaphoreGive(s_alert_mutex);
    }

    if (xTimerIsTimerActive(s_speaker_alert_timer) == pdFALSE) {
        if (xTimerStart(s_speaker_alert_timer, 0) != pdPASS) {
            ESP_LOGW(TAG, "Failed to start speaker alert timer");
        } else {
            ESP_LOGI(TAG, "Speaker alert timer armed (%d ms)", SPEAKER_ALERT_INTERVAL_MS);
        }
    }
}

static void speaker_alert_timer_callback(TimerHandle_t xTimer)
{
    (void)xTimer;

    if (s_alert_mutex == NULL) {
        return;
    }

    sensor_data_t pending = {0};
    bool have_pending = false;

    if (xSemaphoreTake(s_alert_mutex, portMAX_DELAY) == pdTRUE) {
        if (s_pending_alert_valid) {
            pending = s_pending_alert;
            have_pending = true;
        }
        xSemaphoreGive(s_alert_mutex);
    }

    if (!have_pending) {
        ESP_LOGD(TAG, "Alert timer fired but no pending alert");
        return;
    }

    if (s_speaker_queue == NULL) {
        ESP_LOGW(TAG, "Speaker queue not ready, dropping alert");
        return;
    }

    if (xQueueSend(s_speaker_queue, &pending, 0) == pdTRUE) {
        ESP_LOGI(TAG, "Speaker alert queued for %s", pending.device_name);
        if (xSemaphoreTake(s_alert_mutex, portMAX_DELAY) == pdTRUE) {
            s_pending_alert_valid = false;
            memset(&s_pending_alert, 0, sizeof(s_pending_alert));
            xSemaphoreGive(s_alert_mutex);
        }
    } else {
        ESP_LOGW(TAG, "Speaker queue still busy, rescheduling alert");
        if (xTimerStart(s_speaker_alert_timer, 0) != pdPASS) {
            ESP_LOGE(TAG, "Failed to reschedule speaker alert timer");
        }
    }
}

static void controller_rcv_pkt_ready(void) {
    // Keep it minimal
}

// Optimized packet processing with single allocation
static int host_rcv_pkt(uint8_t *data, uint16_t len) {
    if (data[1] == 0x0e) {
        if (data[6] != 0) {
            ESP_LOGE(TAG, "Event opcode 0x%02x fail: 0x%02x", data[4], data[6]);
            return ESP_FAIL;
        }
    }

    if (data[3] == HCI_LE_ADV_REPORT) {
        uint8_t num_responses = data[4];
        if (num_responses == 0) return ESP_OK;

        // Single allocation for all reports
        adv_report_t *reports = malloc(num_responses * sizeof(adv_report_t));
        if (!reports) return ESP_FAIL;

        uint16_t data_ptr = 5;
        uint16_t total_data_len = 0;

        // Parse all reports first
        for (uint8_t i = 0; i < num_responses; i++) {
            reports[i].event_type = data[data_ptr++];
            reports[i].addr_type = data[data_ptr++];
            memcpy(reports[i].addr, &data[data_ptr], 6);
            data_ptr += 6;
            reports[i].data_len = data[data_ptr++];
            total_data_len += reports[i].data_len;
        }

        // Single allocation for all data
        uint8_t *all_data = malloc(total_data_len);
        if (!all_data) {
            free(reports);
            return ESP_FAIL;
        }

        // Copy all data
        uint16_t data_msg_ptr = 0;
        for (uint8_t i = 0; i < num_responses; i++) {
            memcpy(&all_data[data_msg_ptr], &data[data_ptr], reports[i].data_len);
            data_ptr += reports[i].data_len;
            data_msg_ptr += reports[i].data_len;
        }

        // Process RSSI
        for (uint8_t i = 0; i < num_responses; i++) {
            reports[i].rssi = -(0xFF - data[data_ptr++]);
        }

        // Process each report
        data_msg_ptr = 0;
        for (uint8_t i = 0; i < num_responses; i++) {
            ble_scan_local_name_t scanned_name = {0};

                    if (get_local_name(&all_data[data_msg_ptr], reports[i].data_len, &scanned_name) == ESP_OK) {
                        if (strcmp(scanned_name.scan_local_name, "AI-8000") == 0) {
                            sensor_data_t sensor_data = parse_sensor_data(data, len, reports[i].addr);

                            ESP_LOGI(TAG, "Device: %s, Temp: %u°C, Battery: %.1f, Pressure: %u Pa",
                                     sensor_data.device_name, sensor_data.temperature,
                                     sensor_data.battery_level, sensor_data.pressure);
                            
                            // Notify speaker component that this sensor was detected
                            // This will set the corresponding bit in Event Group
                            speaker_notify_sensor_detected(sensor_data.device_name);

                    // Pressure checking with configurable thresholds
                    bool pressure_abnormal = false;
                    if (sensor_data.pressure > PRESSURE_HIGH_THRESHOLD) {
                        ESP_LOGW(TAG, "HIGH Pressure: %u > %d for %s",
                                sensor_data.pressure, PRESSURE_HIGH_THRESHOLD, sensor_data.device_name);
                        pressure_abnormal = true;
                    } else if (sensor_data.pressure < PRESSURE_LOW_THRESHOLD) {
                        ESP_LOGW(TAG, "LOW Pressure: %u < %d for %s",
                                sensor_data.pressure, PRESSURE_LOW_THRESHOLD, sensor_data.device_name);
                        pressure_abnormal = true;
                    }
                    
                    // Gửi dữ liệu vào speaker_queue nếu áp suất bất thường (gửi trực tiếp, không throttle)
                    if (pressure_abnormal && s_speaker_queue != NULL) {
                        if (xQueueSend(s_speaker_queue, &sensor_data, 0) != pdTRUE) {
                            ESP_LOGW(TAG, "Speaker queue full, dropping alert for %s", sensor_data.device_name);
                        }
                    }

                    // Update global variables based on device_name (assuming pressure in 0.1 PSI units)
                    // Lock mutex to protect sensor data variables
                    if (xSemaphoreTake(sensor_data_mutex, portMAX_DELAY) == pdTRUE) {
                        float psi = sensor_data.pressure / 10.0f;
                        if (strcmp(sensor_data.device_name, "TT") == 0) {  // Front Left
                            front_left_temperature = sensor_data.temperature;
                            front_left_pressure_psi = psi;
                            front_left_voltage = sensor_data.battery_level;
                            front_left_updated = 1;
                            front_left_pressure_psi_x10 = sensor_data.pressure;
                        } else if (strcmp(sensor_data.device_name, "TP") == 0) {  // Front Right
                            front_right_temperature = sensor_data.temperature;
                            front_right_pressure_psi = psi;
                            front_right_voltage = sensor_data.battery_level;
                            front_right_updated = 1;
                            front_right_pressure_psi_x10 = sensor_data.pressure;
                        } else if (strcmp(sensor_data.device_name, "ST") == 0) {  // Rear Left
                            rear_left_temperature = sensor_data.temperature;
                            rear_left_pressure_psi = psi;
                            rear_left_voltage = sensor_data.battery_level;
                            rear_left_updated = 1;
                            rear_left_pressure_psi_x10 = sensor_data.pressure;
                        } else if (strcmp(sensor_data.device_name, "SP") == 0) {  // Rear Right
                            rear_right_temperature = sensor_data.temperature;
                            rear_right_pressure_psi = psi;
                            rear_right_voltage = sensor_data.battery_level;
                            rear_right_updated = 1;
                            rear_right_pressure_psi_x10 = sensor_data.pressure;
                        }
                        xSemaphoreGive(sensor_data_mutex);
                    }

                    // Compact printf output
                    printf("=== AI-8000 [%s] ===\n", sensor_data.device_name);
                    printf("Addr: %s\nRSSI: %ddB\nTemp: %u°C\nBatt: %.1f\nPress: %u Pa\n",
                           sensor_data.address, reports[i].rssi, sensor_data.temperature,
                           sensor_data.battery_level, sensor_data.pressure);

                    // Debug parsing
                    const uint8_t *sensor_bytes = &data[len - 21];
                    printf("Data: ");
                    for (int k = 0; k < 5; k++) printf("%02x ", sensor_bytes[k]);
                    printf("\n====================\n");
                }
            }
            data_msg_ptr += reports[i].data_len;
        }

        free(all_data);
        free(reports);
    }
    return ESP_OK;
}

static esp_vhci_host_callback_t vhci_host_cb = {
    .notify_host_send_available = controller_rcv_pkt_ready,
    .notify_host_recv = host_rcv_pkt
};

// Command sending functions
static void hci_cmd_send_reset(void) {
    uint16_t sz = make_cmd_reset(hci_cmd_buf);
    esp_vhci_host_send_packet(hci_cmd_buf, sz);
}

static void hci_cmd_send_set_evt_mask(void) {
    uint8_t evt_mask[8] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20};
    uint16_t sz = make_cmd_set_evt_mask(hci_cmd_buf, evt_mask);
    esp_vhci_host_send_packet(hci_cmd_buf, sz);
}

static void hci_cmd_send_ble_scan_params(void) {
    uint16_t sz = make_cmd_ble_set_scan_params(hci_cmd_buf, 0x01, 0x50, 0x30, 0x00, 0x00);
    esp_vhci_host_send_packet(hci_cmd_buf, sz);
}

static void hci_cmd_send_ble_scan_start(void) {
    uint16_t sz = make_cmd_ble_set_scan_enable(hci_cmd_buf, 0x01, 0x00);
    esp_vhci_host_send_packet(hci_cmd_buf, sz);
    ESP_LOGI(TAG, "BLE Scanning started");
}

static void hci_evt_process(void *pvParameters) {
    while (1) {
        vTaskDelay(portMAX_DELAY);
    }
}

// Timer callback for BLE scan status check (runs in timer task context)
static void ble_scan_timer_callback(TimerHandle_t xTimer) {
    // This timer can be used to:
    // 1. Check scan status periodically
    // 2. Restart scan if needed
    // 3. Log scan statistics
    
    // Example: Log that scan is still active
    ESP_LOGD(TAG, "BLE scan status check - scan is active");
    
    // Optional: Check if we need to restart scan
    // (In a real scenario, you might want to restart if no devices found for a long time)
}

void ble_scanner_init(void) {
    // Create mutex for sensor data
    sensor_data_mutex = xSemaphoreCreateMutex();
    if (sensor_data_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create sensor data mutex");
        return;
    }

    // Create mutex for alert throttling
    s_alert_mutex = xSemaphoreCreateMutex();
    if (s_alert_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create alert mutex");
        return;
    }

    // Create timer for throttling speaker queue submissions
    s_speaker_alert_timer = xTimerCreate(
        "speaker_alert_gate",
        pdMS_TO_TICKS(SPEAKER_ALERT_INTERVAL_MS),
        pdFALSE,
        NULL,
        speaker_alert_timer_callback
    );
    if (s_speaker_alert_timer == NULL) {
        ESP_LOGE(TAG, "Failed to create speaker alert timer");
        return;
    } else {
        ESP_LOGI(TAG, "Speaker alert timer created (%d ms window)", SPEAKER_ALERT_INTERVAL_MS);
    }
    
    // Create FreeRTOS software timer for BLE scan status check
    s_ble_scan_timer = xTimerCreate(
        "ble_scan_check",                      // Timer name
        pdMS_TO_TICKS(BLE_SCAN_CHECK_INTERVAL_MS), // Period: 30 seconds
        pdTRUE,                                // Auto-reload (periodic)
        (void*)0,                              // Timer ID
        ble_scan_timer_callback                // Callback function
    );
    
    if (s_ble_scan_timer == NULL) {
        ESP_LOGE(TAG, "Failed to create BLE scan timer");
    } else {
        ESP_LOGI(TAG, "BLE scan status check timer created (%d ms period)", BLE_SCAN_CHECK_INTERVAL_MS);
    }
    
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    ESP_ERROR_CHECK(esp_vhci_host_register_callback(&vhci_host_cb));
    ESP_LOGI(TAG, "BLE scanner initialized with mutex, binary semaphore, and timer");
}

void ble_scanner_start(QueueHandle_t speaker_queue, EventGroupHandle_t event_group) {
    s_speaker_queue = speaker_queue;
    
    // Simplified command sequence
    const int total_commands = 4;
    for (int cmd_cnt = 0; cmd_cnt < total_commands; cmd_cnt++) {
        while (!esp_vhci_host_check_send_available()) {
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }

        switch (cmd_cnt) {
            case 0: hci_cmd_send_reset(); break;
            case 1: hci_cmd_send_set_evt_mask(); break;
            case 2: hci_cmd_send_ble_scan_params(); break;
            case 3: hci_cmd_send_ble_scan_start(); break;
        }
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }

    xTaskCreatePinnedToCore(hci_evt_process, "hci_evt_process", 2048, NULL, 6, NULL, 0);
    
    // Start BLE scan status check timer
    if (s_ble_scan_timer != NULL) {
        if (xTimerStart(s_ble_scan_timer, 0) != pdPASS) {
            ESP_LOGE(TAG, "Failed to start BLE scan timer");
        } else {
            ESP_LOGI(TAG, "BLE scan status check timer started");
        }
    }
    
    // Signal that BLE scanner is ready (after scan start)
    if (event_group != NULL) {
        vTaskDelay(pdMS_TO_TICKS(200)); // Wait for scan to actually start
        xEventGroupSetBits(event_group, (1 << 2)); // BIT_BLE_READY
        ESP_LOGI(TAG, "BLE scanner ready signal sent");
    }
}

void ble_scanner_lock_sensor_data(void) {
    if (sensor_data_mutex) {
        xSemaphoreTake(sensor_data_mutex, portMAX_DELAY);
    }
}

void ble_scanner_unlock_sensor_data(void) {
    if (sensor_data_mutex) {
        xSemaphoreGive(sensor_data_mutex);
    }
}

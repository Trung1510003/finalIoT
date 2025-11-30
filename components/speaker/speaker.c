#include "speaker.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/uart.h"
#include <string.h>
#include <stdbool.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>

static const char *TAG = "SPEAKER";

// DFPlayer configuration
#define TXD_PIN 2   // ESP32-C3 TX → MP3 RX
#define RXD_PIN 3   // ESP32-C3 RX → MP3 TX
#define UART_PORT_NUM UART_NUM_1
#define BUF_SIZE (1024)

// Pressure thresholds
#define PRESSURE_HIGH_THRESHOLD 300
#define PRESSURE_LOW_THRESHOLD 200
#define PRESSURE_LEAK_THRESHOLD 230

typedef enum {
    SPEAKER_VOICE_MALE = 0,
    SPEAKER_VOICE_FEMALE = 1
} speaker_voice_t;

// System event group bit (must match main.c definition)
#define BIT_SPEAKER_READY (1 << 1)

// Sensor presence tracking
#define SENSOR_BIT_TT (1 << 0)
#define SENSOR_BIT_TP (1 << 1)
#define SENSOR_BIT_ST (1 << 2)
#define SENSOR_BIT_SP (1 << 3)
#define SENSOR_BITS_ALL (SENSOR_BIT_TT | SENSOR_BIT_TP | SENSOR_BIT_ST | SENSOR_BIT_SP)

// Guard time to ensure 14.mp3 finishes before the task self-deletes (ms)
#define PLAYBACK_GUARD_TIME_MS 6000
#define PRESSURE_ALERT_GUARD_MS 5000
#define SENSOR_TIMEOUT_MS 20000  // 20 seconds timeout per sensor

// Sensor priority order: TT (0) -> TP (1) -> ST (2) -> SP (3)
// Lower priority number = higher priority (play first)
static uint8_t speaker_get_sensor_priority(const char *device_name)
{
    if (device_name == NULL) {
        return 99; // Unknown = lowest priority
    }
    if (strcmp(device_name, "TT") == 0) return 0; // Front Left - highest priority
    if (strcmp(device_name, "TP") == 0) return 1; // Front Right
    if (strcmp(device_name, "ST") == 0) return 2; // Rear Left
    if (strcmp(device_name, "SP") == 0) return 3; // Rear Right - lowest priority
    return 99; // Unknown
}

static EventGroupHandle_t s_sensor_event_group = NULL;
static TaskHandle_t s_all_sensors_task_handle = NULL;
static SemaphoreHandle_t s_audio_mutex = NULL;
static bool s_alerts_enabled = false;
static speaker_voice_t s_active_voice = SPEAKER_VOICE_FEMALE;
static bool s_voice_enabled = true;

// Priority queue for ordered alert playback
#define PRIORITY_QUEUE_SIZE 4
typedef struct {
    sensor_data_t data;
    bool valid;
    uint8_t priority;
    int64_t timestamp_us;  // Timestamp when alert was added
} priority_alert_t;

static priority_alert_t s_priority_queue[PRIORITY_QUEUE_SIZE] = {0};
static SemaphoreHandle_t s_priority_queue_mutex = NULL;
static uint8_t s_current_waiting_priority = 0;  // Current sensor priority being waited for (0=TT, 1=TP, 2=ST, 3=SP)
static int64_t s_current_waiting_start_us = 0;  // Timestamp when started waiting for current sensor

static EventBits_t speaker_get_sensor_bit(const char *device_name);
static void speaker_all_sensors_task(void *pv);
static void speaker_play_track_guarded(uint16_t track_id, uint32_t guard_time_ms);
static uint16_t speaker_map_track_id(uint16_t female_track_id);
static void speaker_priority_queue_add(const sensor_data_t *sensor_data);
static bool speaker_priority_queue_get_next(sensor_data_t *out_data);
static void speaker_priority_queue_remove(const char *device_name);

static void send_command(uint8_t cmd, uint16_t param)
{
    if (!s_voice_enabled) {
        ESP_LOGD(TAG, "Voice disabled - skipping command 0x%02x param 0x%04x", cmd, param);
        return;
    }

    uint8_t buf[10];
    uint16_t checksum = (uint16_t)(0xFFFF - (0xFF + 0x06 + cmd + (param >> 8) + (param & 0xFF)) + 1);

    buf[0] = 0x7E;          // Start byte
    buf[1] = 0xFF;          // Version
    buf[2] = 0x06;          // Length
    buf[3] = cmd;           // Command
    buf[4] = 0x00;          // No feedback
    buf[5] = (param >> 8);  // Parameter high
    buf[6] = (param & 0xFF);// Parameter low
    buf[7] = (checksum >> 8);
    buf[8] = (checksum & 0xFF);
    buf[9] = 0xEF;          // End byte

    uart_write_bytes(UART_PORT_NUM, (const char*)buf, 10);
}

void speaker_init(void)
{
    const uart_config_t uart_config = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };

    uart_driver_install(UART_PORT_NUM, BUF_SIZE, 0, 0, NULL, 0);
    uart_param_config(UART_PORT_NUM, &uart_config);
    uart_set_pin(UART_PORT_NUM, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    if (s_audio_mutex == NULL) {
        s_audio_mutex = xSemaphoreCreateMutex();
        if (s_audio_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create audio mutex");
        }
    }

    ESP_LOGI(TAG, "Khởi động DFPlayer...");
    vTaskDelay(pdMS_TO_TICKS(1500));
    // Set volume (0-30)
    send_command(0x06, 25);
    // Không phát thêm file nào cho đến khi nhận đủ 4 cảm biến
}

void speaker_play_pressure_alert(const sensor_data_t *sensor_data)
{
    // Nếu sensor_data là NULL (trường hợp khởi động), không phát âm thanh khác
    if (sensor_data == NULL) {
        return;
    }

    if (!s_alerts_enabled) {
        ESP_LOGD(TAG, "Audio alerts muted until 14.mp3 task completes");
        return;
    }

    // Kiểm tra áp suất và vị trí lốp
    // TT = Front Left, TP = Front Right, ST = Rear Left, SP = Rear Right
    uint16_t track_to_play = 0;

    if (sensor_data->pressure > PRESSURE_HIGH_THRESHOLD) {
        ESP_LOGI(TAG, "Pressure %u > %d for %s", sensor_data->pressure, PRESSURE_HIGH_THRESHOLD, sensor_data->device_name);
        if (strcmp(sensor_data->device_name, "SP") == 0) {
            track_to_play = 15; // Rear Right áp suất cao
        } else if (strcmp(sensor_data->device_name, "ST") == 0) {
            track_to_play = 16; // Rear Left áp suất cao
        } else if (strcmp(sensor_data->device_name, "TP") == 0) {
            track_to_play = 17; // Front Right áp suất cao
        } else if (strcmp(sensor_data->device_name, "TT") == 0) {
            track_to_play = 18; // Front Left áp suất cao
        }
    } else if (sensor_data->pressure < PRESSURE_LOW_THRESHOLD) {
        ESP_LOGI(TAG, "Pressure %u < %d for %s", sensor_data->pressure, PRESSURE_LOW_THRESHOLD, sensor_data->device_name);
        if (strcmp(sensor_data->device_name, "SP") == 0) {
            track_to_play = 19; // Rear Right áp suất thấp
        } else if (strcmp(sensor_data->device_name, "ST") == 0) {
            track_to_play = 20; // Rear Left áp suất thấp
        } else if (strcmp(sensor_data->device_name, "TP") == 0) {
            track_to_play = 21; // Front Right áp suất thấp
        } else if (strcmp(sensor_data->device_name, "TT") == 0) {
            track_to_play = 22; // Front Left áp suất thấp
        }
    } else if (sensor_data->pressure < PRESSURE_LEAK_THRESHOLD && sensor_data->pressure > PRESSURE_LOW_THRESHOLD) {
        ESP_LOGI(TAG, "Pressure %u < %d for %s", sensor_data->pressure, PRESSURE_LEAK_THRESHOLD, sensor_data->device_name);
        if (strcmp(sensor_data->device_name, "SP") == 0) {
            track_to_play = 23; // Rear Right áp suất rò rỉ
        } else if (strcmp(sensor_data->device_name, "ST") == 0) {
            track_to_play = 24; // Rear Left áp suất rò rỉ
        }
        else if (strcmp(sensor_data->device_name, "TP") == 0) {
            track_to_play = 25; // Front Right áp suất rò rỉ
        } else if (strcmp(sensor_data->device_name, "TT") == 0) {
            track_to_play = 26; // Front Left áp suất rò rỉ
        }
    }

    if (track_to_play != 0) {
        speaker_play_track_guarded(track_to_play, PRESSURE_ALERT_GUARD_MS);
    }
}

static uint16_t speaker_map_track_id(uint16_t female_track_id)
{
    if (s_active_voice == SPEAKER_VOICE_MALE &&
        female_track_id >= 14 && female_track_id <= 26) {
        return (uint16_t)(female_track_id - 13);
    }
    return female_track_id;
}

static void speaker_play_track_guarded(uint16_t track_id, uint32_t guard_time_ms)
{
    bool locked = false;
    uint16_t resolved_track = speaker_map_track_id(track_id);

    if (s_audio_mutex != NULL) {
        if (xSemaphoreTake(s_audio_mutex, portMAX_DELAY) == pdTRUE) {
            locked = true;
        } else {
            ESP_LOGW(TAG, "Failed to obtain audio mutex, playing track %u without protection", resolved_track);
        }
    }

    send_command(0x03, resolved_track);

    if (guard_time_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(guard_time_ms));
    }

    if (locked) {
        xSemaphoreGive(s_audio_mutex);
    }
}

static EventBits_t speaker_get_sensor_bit(const char *device_name)
{
    if (device_name == NULL) {
        return 0;
    }

    if (strcmp(device_name, "TT") == 0) {
        return SENSOR_BIT_TT;
    } else if (strcmp(device_name, "TP") == 0) {
        return SENSOR_BIT_TP;
    } else if (strcmp(device_name, "ST") == 0) {
        return SENSOR_BIT_ST;
    } else if (strcmp(device_name, "SP") == 0) {
        return SENSOR_BIT_SP;
    }

    return 0;
}

void speaker_notify_sensor_detected(const char* device_name)
{
    if (s_sensor_event_group == NULL || device_name == NULL) {
        return;
    }

    EventBits_t bit = speaker_get_sensor_bit(device_name);
    if (bit == 0) {
        ESP_LOGW(TAG, "Unknown sensor name %s for event group tracking", device_name);
        return;
    }

    xEventGroupSetBits(s_sensor_event_group, bit);
    ESP_LOGD(TAG, "Sensor %s reported, bit 0x%x set", device_name, bit);
}

static void speaker_all_sensors_task(void *pv)
{
    (void)pv;
    ESP_LOGI(TAG, "All-sensors tracker task started");

    if (s_sensor_event_group == NULL) {
        ESP_LOGE(TAG, "Sensor event group missing, deleting tracker task");
        s_all_sensors_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_sensor_event_group,
        SENSOR_BITS_ALL,
        pdTRUE,   // clear bits so task is one-shot
        pdTRUE,   // wait for all sensors
        portMAX_DELAY);

    if ((bits & SENSOR_BITS_ALL) == SENSOR_BITS_ALL) {
        ESP_LOGI(TAG, "All four sensors reported, playing 14.mp3");
        speaker_play_track_guarded(14, PLAYBACK_GUARD_TIME_MS);
        s_alerts_enabled = true;
    } else {
        ESP_LOGW(TAG, "Unexpected bits 0x%x while waiting for all sensors", bits);
    }

    ESP_LOGI(TAG, "All-sensors tracker task finished, deleting itself");
    s_all_sensors_task_handle = NULL;
    vTaskDelete(NULL);
}

void speaker_update_voice_setting(uint8_t voice)
{
    speaker_voice_t new_voice = (voice == (uint8_t)SPEAKER_VOICE_MALE) ? SPEAKER_VOICE_MALE : SPEAKER_VOICE_FEMALE;
    if (s_active_voice != new_voice) {
        s_active_voice = new_voice;
        ESP_LOGI(TAG, "Voice setting updated to %s", (new_voice == SPEAKER_VOICE_MALE) ? "male" : "female");
    } else {
        ESP_LOGD(TAG, "Voice setting unchanged (%s)", (new_voice == SPEAKER_VOICE_MALE) ? "male" : "female");
    }
}

void speaker_set_voice_enabled(bool enabled)
{
    if (s_voice_enabled != enabled) {
        s_voice_enabled = enabled;
        ESP_LOGI(TAG, "Voice playback %s", enabled ? "enabled" : "disabled");
    } else {
        ESP_LOGD(TAG, "Voice playback already %s", enabled ? "enabled" : "disabled");
    }
}

static void speaker_priority_queue_add(const sensor_data_t *sensor_data)
{
    if (sensor_data == NULL || s_priority_queue_mutex == NULL) {
        return;
    }

    if (xSemaphoreTake(s_priority_queue_mutex, portMAX_DELAY) == pdTRUE) {
        uint8_t priority = speaker_get_sensor_priority(sensor_data->device_name);
        
        // Tìm vị trí để chèn (thay thế nếu đã có cảnh báo cho cùng sensor)
        int insert_idx = -1;
        int replace_idx = -1;
        
        for (int i = 0; i < PRIORITY_QUEUE_SIZE; i++) {
            if (!s_priority_queue[i].valid) {
                if (insert_idx < 0) {
                    insert_idx = i; // Vị trí trống đầu tiên
                }
            } else if (strcmp(s_priority_queue[i].data.device_name, sensor_data->device_name) == 0) {
                replace_idx = i; // Đã có cảnh báo cho sensor này, thay thế
                break;
            }
        }
        
        int target_idx = (replace_idx >= 0) ? replace_idx : insert_idx;
        
        if (target_idx >= 0) {
            s_priority_queue[target_idx].data = *sensor_data;
            s_priority_queue[target_idx].priority = priority;
            s_priority_queue[target_idx].valid = true;
            s_priority_queue[target_idx].timestamp_us = esp_timer_get_time();
            ESP_LOGD(TAG, "Added alert to priority queue: %s (priority %d, idx %d)", 
                     sensor_data->device_name, priority, target_idx);
        } else {
            ESP_LOGW(TAG, "Priority queue full, dropping alert for %s", sensor_data->device_name);
        }
        
        xSemaphoreGive(s_priority_queue_mutex);
    }
}

static bool speaker_priority_queue_get_next(sensor_data_t *out_data)
{
    if (out_data == NULL || s_priority_queue_mutex == NULL) {
        return false;
    }
    
    int64_t now_us = esp_timer_get_time();
    bool found = false;
    int target_idx = -1;
    
    if (xSemaphoreTake(s_priority_queue_mutex, portMAX_DELAY) == pdTRUE) {
        // Kiểm tra có dữ liệu nào trong queue không
        bool has_any_data = false;
        for (int i = 0; i < PRIORITY_QUEUE_SIZE; i++) {
            if (s_priority_queue[i].valid) {
                has_any_data = true;
                break;
            }
        }
        
        if (!has_any_data) {
            // Không có dữ liệu nào, reset waiting state
            s_current_waiting_priority = 0;
            s_current_waiting_start_us = 0;
            xSemaphoreGive(s_priority_queue_mutex);
            return false;
        }
        
        // Khởi tạo wait start time nếu chưa có
        if (s_current_waiting_start_us == 0) {
            s_current_waiting_start_us = now_us;
        }
        
        // Kiểm tra sensor hiện tại đang chờ có dữ liệu không
        for (int i = 0; i < PRIORITY_QUEUE_SIZE; i++) {
            if (s_priority_queue[i].valid && s_priority_queue[i].priority == s_current_waiting_priority) {
                target_idx = i;
                found = true;
                s_current_waiting_start_us = now_us;  // Reset timeout khi tìm thấy
                break;
            }
        }
        
        // Nếu không tìm thấy sensor hiện tại, kiểm tra timeout
        if (!found) {
            int64_t wait_duration_ms = (now_us - s_current_waiting_start_us) / 1000;
            if (wait_duration_ms >= SENSOR_TIMEOUT_MS) {
                // Timeout: chuyển sang sensor tiếp theo (round-robin)
                uint8_t old_priority = s_current_waiting_priority;
                s_current_waiting_priority = (s_current_waiting_priority + 1) % 4;
                s_current_waiting_start_us = now_us;
                ESP_LOGI(TAG, "Timeout (%ld ms) for priority %d, moving to next priority %d", 
                         wait_duration_ms, old_priority, s_current_waiting_priority);
                
                // Tìm sensor mới
                for (int i = 0; i < PRIORITY_QUEUE_SIZE; i++) {
                    if (s_priority_queue[i].valid && s_priority_queue[i].priority == s_current_waiting_priority) {
                        target_idx = i;
                        found = true;
                        break;
                    }
                }
                
                // Nếu vẫn không tìm thấy, tiếp tục round-robin cho đến khi tìm thấy
                int attempts = 0;
                while (!found && attempts < 4) {
                    s_current_waiting_priority = (s_current_waiting_priority + 1) % 4;
                    s_current_waiting_start_us = now_us;
                    for (int i = 0; i < PRIORITY_QUEUE_SIZE; i++) {
                        if (s_priority_queue[i].valid && s_priority_queue[i].priority == s_current_waiting_priority) {
                            target_idx = i;
                            found = true;
                            break;
                        }
                    }
                    attempts++;
                }
            }
        }
        
        if (found && target_idx >= 0) {
            *out_data = s_priority_queue[target_idx].data;
        }
        
        xSemaphoreGive(s_priority_queue_mutex);
    }
    
    return found;
}

static void speaker_priority_queue_remove(const char *device_name)
{
    if (device_name == NULL || s_priority_queue_mutex == NULL) {
        return;
    }
    
    if (xSemaphoreTake(s_priority_queue_mutex, portMAX_DELAY) == pdTRUE) {
        for (int i = 0; i < PRIORITY_QUEUE_SIZE; i++) {
            if (s_priority_queue[i].valid && 
                strcmp(s_priority_queue[i].data.device_name, device_name) == 0) {
                uint8_t removed_priority = s_priority_queue[i].priority;
                s_priority_queue[i].valid = false;
                memset(&s_priority_queue[i].data, 0, sizeof(sensor_data_t));
                ESP_LOGD(TAG, "Removed alert from priority queue: %s", device_name);
                
                // Sau khi phát xong, chuyển sang sensor tiếp theo trong vòng tròn
                s_current_waiting_priority = (removed_priority + 1) % 4;
                s_current_waiting_start_us = esp_timer_get_time();
                break;
            }
        }
        xSemaphoreGive(s_priority_queue_mutex);
    }
}

// Task xử lý DFPlayer để phát âm thanh cảnh báo theo thứ tự ưu tiên
static void speaker_task(void* pv)
{
    QueueHandle_t queue = (QueueHandle_t)pv;
    sensor_data_t sensor_data;
    
    ESP_LOGI(TAG, "Speaker task started");
    
    for (;;) {
        // Bước 1: Nhận tất cả cảnh báo từ queue và thêm vào priority queue (timeout ngắn)
        while (xQueueReceive(queue, &sensor_data, 0) == pdTRUE) {
            ESP_LOGI(TAG, "Received alert for device: %s, Pressure: %u", 
                     sensor_data.device_name, sensor_data.pressure);
            
            // Mark sensor as seen for the all-sensors event flow
            speaker_notify_sensor_detected(sensor_data.device_name);
            
            // Thêm vào priority queue (thay thế nếu đã có cảnh báo cho cùng sensor)
            speaker_priority_queue_add(&sensor_data);
        }
        
        // Bước 2: Xử lý priority queue theo thứ tự ưu tiên (TT -> TP -> ST -> SP)
        if (speaker_priority_queue_get_next(&sensor_data)) {
            ESP_LOGI(TAG, "Playing audio alert for device: %s, Pressure: %u (priority order)", 
                     sensor_data.device_name, sensor_data.pressure);
            
            // Phát âm thanh dựa trên áp suất và vị trí lốp
            speaker_play_pressure_alert(&sensor_data);
            
            // Xóa khỏi priority queue sau khi phát xong
            speaker_priority_queue_remove(sensor_data.device_name);
            
            // Tiếp tục xử lý priority queue (không nhận cảnh báo mới ngay)
            continue;
        }
        
        // Bước 3: Nếu priority queue rỗng, chờ cảnh báo mới với timeout
        if (xQueueReceive(queue, &sensor_data, pdMS_TO_TICKS(100)) == pdTRUE) {
            ESP_LOGI(TAG, "Received alert for device: %s, Pressure: %u", 
                     sensor_data.device_name, sensor_data.pressure);
            
            // Mark sensor as seen for the all-sensors event flow
            speaker_notify_sensor_detected(sensor_data.device_name);
            
            // Thêm vào priority queue
            speaker_priority_queue_add(&sensor_data);
        }
    }
}

void speaker_task_start(QueueHandle_t queue, EventGroupHandle_t event_group)
{
    if (queue == NULL) {
        ESP_LOGE(TAG, "Cannot start speaker task without queue");
        return;
    }

    // Initialize priority queue mutex
    if (s_priority_queue_mutex == NULL) {
        s_priority_queue_mutex = xSemaphoreCreateMutex();
        if (s_priority_queue_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create priority queue mutex");
            return;
        }
        // Initialize queue
        memset(s_priority_queue, 0, sizeof(s_priority_queue));
        // Initialize waiting state - start with TT (priority 0)
        s_current_waiting_priority = 0;
        s_current_waiting_start_us = 0;
    }

    if (s_sensor_event_group == NULL) {
        s_sensor_event_group = xEventGroupCreate();
        if (s_sensor_event_group == NULL) {
            ESP_LOGE(TAG, "Failed to create sensor event group");
            return;
        }
    }

    if (xTaskCreate(speaker_task, "speaker_task", 2048, (void*)queue, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create speaker task");
        return;
    }

    if (s_all_sensors_task_handle == NULL) {
        if (xTaskCreate(speaker_all_sensors_task, "speaker_all_sensors", 2048, NULL, 3, &s_all_sensors_task_handle) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create all-sensors tracker task");
            s_all_sensors_task_handle = NULL;
        }
    }

    if (event_group != NULL) {
        xEventGroupSetBits(event_group, BIT_SPEAKER_READY);
    }
}


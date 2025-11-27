#include "speaker.h"
#include "esp_log.h"
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

static EventGroupHandle_t s_sensor_event_group = NULL;
static TaskHandle_t s_all_sensors_task_handle = NULL;
static SemaphoreHandle_t s_audio_mutex = NULL;
static bool s_alerts_enabled = false;

static EventBits_t speaker_get_sensor_bit(const char *device_name);
static void speaker_all_sensors_task(void *pv);
static void speaker_play_track_guarded(uint16_t track_id, uint32_t guard_time_ms);

static void send_command(uint8_t cmd, uint16_t param)
{
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

static void speaker_play_track_guarded(uint16_t track_id, uint32_t guard_time_ms)
{
    bool locked = false;

    if (s_audio_mutex != NULL) {
        if (xSemaphoreTake(s_audio_mutex, portMAX_DELAY) == pdTRUE) {
            locked = true;
        } else {
            ESP_LOGW(TAG, "Failed to obtain audio mutex, playing track %u without protection", track_id);
        }
    }

    send_command(0x03, track_id);

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
    ESP_LOGI(TAG, "Voice setting updated to %s", voice == 0 ? "male" : "female");
}

// Task xử lý DFPlayer để phát âm thanh cảnh báo
static void speaker_task(void* pv)
{
    QueueHandle_t queue = (QueueHandle_t)pv;
    sensor_data_t sensor_data;
    
    ESP_LOGI(TAG, "Speaker task started");
    
    for (;;) {
        // Chờ nhận dữ liệu từ speaker_queue
        if (xQueueReceive(queue, &sensor_data, portMAX_DELAY) == pdTRUE) {
            ESP_LOGI(TAG, "Playing audio alert for device: %s, Pressure: %u", 
                     sensor_data.device_name, sensor_data.pressure);

            // Mark sensor as seen for the all-sensors event flow
            speaker_notify_sensor_detected(sensor_data.device_name);
            
            // Phát âm thanh dựa trên áp suất và vị trí lốp
            speaker_play_pressure_alert(&sensor_data);
        }
    }
}

void speaker_task_start(QueueHandle_t queue, EventGroupHandle_t event_group)
{
    if (queue == NULL) {
        ESP_LOGE(TAG, "Cannot start speaker task without queue");
        return;
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


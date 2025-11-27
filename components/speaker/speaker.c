#include "speaker.h"
#include "esp_log.h"
#include "driver/uart.h"
#include <string.h>
#include <freertos/task.h>

static const char *TAG = "SPEAKER";

// DFPlayer configuration
#define TXD_PIN 2   // ESP32-C3 TX → MP3 RX
#define RXD_PIN 3   // ESP32-C3 RX → MP3 TX
#define UART_PORT_NUM UART_NUM_1
#define BUF_SIZE (1024)

// Pressure thresholds
#define PRESSURE_HIGH_THRESHOLD 180
#define PRESSURE_LOW_THRESHOLD 179

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

    ESP_LOGI(TAG, "Khởi động DFPlayer...");
    vTaskDelay(pdMS_TO_TICKS(1500));
    // Set volume (0-30)
    send_command(0x06, 25);
    // Phát 14.mp3 ngay sau khi khởi tạo
    send_command(0x03, 14);
}

void speaker_play_pressure_alert(const sensor_data_t *sensor_data)
{
    // Nếu sensor_data là NULL (trường hợp khởi động), không phát âm thanh khác
    if (sensor_data == NULL) {
        return;
    }

    // Kiểm tra áp suất và vị trí lốp
    // TT = Front Left, TP = Front Right, ST = Rear Left, SP = Rear Right
    if (sensor_data->pressure > PRESSURE_HIGH_THRESHOLD) {
        ESP_LOGI(TAG, "Pressure %u > %d for %s", sensor_data->pressure, PRESSURE_HIGH_THRESHOLD, sensor_data->device_name);
        if (strcmp(sensor_data->device_name, "SP") == 0) {
            send_command(0x03, 15); // Rear Right áp suất cao: 15.mp3
        } else if (strcmp(sensor_data->device_name, "ST") == 0) {
            send_command(0x03, 16); // Rear Left áp suất cao: 16.mp3
        } else if (strcmp(sensor_data->device_name, "TP") == 0) {
            send_command(0x03, 17); // Front Right áp suất cao: 17.mp3
        } else if (strcmp(sensor_data->device_name, "TT") == 0) {
            send_command(0x03, 18); // Front Left áp suất cao: 18.mp3
        }
    } else if (sensor_data->pressure < PRESSURE_LOW_THRESHOLD) {
        ESP_LOGI(TAG, "Pressure %u < %d for %s", sensor_data->pressure, PRESSURE_LOW_THRESHOLD, sensor_data->device_name);
        if (strcmp(sensor_data->device_name, "SP") == 0) {
            send_command(0x03, 19); // Rear Right áp suất thấp: 19.mp3
        } else if (strcmp(sensor_data->device_name, "ST") == 0) {
            send_command(0x03, 20); // Rear Left áp suất thấp: 20.mp3
        } else if (strcmp(sensor_data->device_name, "TP") == 0) {
            send_command(0x03, 21); // Front Right áp suất thấp: 21.mp3
        } else if (strcmp(sensor_data->device_name, "TT") == 0) {
            send_command(0x03, 22); // Front Left áp suất thấp: 22.mp3
        }
    }
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
            
            // Phát âm thanh dựa trên áp suất và vị trí lốp
            speaker_play_pressure_alert(&sensor_data);
            
            // Delay để đảm bảo âm thanh phát xong trước khi xử lý tiếp
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }
}

void speaker_task_start(QueueHandle_t queue)
{
    xTaskCreate(speaker_task, "speaker_task", 2048, (void*)queue, 2, NULL);
}


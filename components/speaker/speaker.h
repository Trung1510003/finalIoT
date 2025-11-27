#ifndef SPEAKER_H
#define SPEAKER_H

#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#ifdef __cplusplus
extern "C" {
#endif

// Sensor data structure
typedef struct {
    uint8_t temperature;
    float battery_level;
    uint16_t pressure;
    char device_name[4];
    char address[18];
} sensor_data_t;

// Initialize DFPlayer
void speaker_init(void);

// Play sound based on pressure
void speaker_play_pressure_alert(const sensor_data_t *sensor_data);

// Start speaker task to process queue
void speaker_task_start(QueueHandle_t queue);

#ifdef __cplusplus
}
#endif

#endif // SPEAKER_H


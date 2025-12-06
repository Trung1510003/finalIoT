#ifndef SPEAKER_H
#define SPEAKER_H

#include <stdint.h>
#include <stdbool.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/event_groups.h>

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
// event_group: Event group to signal when speaker is ready (optional, can be NULL)
// bit: Bit to set when ready (use BIT_SPEAKER_READY from main.c)
void speaker_task_start(QueueHandle_t queue, EventGroupHandle_t event_group);

// Notify speaker that a sensor device was detected
// Call this from BLE scanner when any sensor data is received (not just abnormal pressure)
// device_name: "TT", "TP", "ST", or "SP"
void speaker_notify_sensor_detected(const char* device_name);

// Update voice setting when changed from UI
// Call this from UI/button component when voice setting is changed
// voice: VOICE_MALE (0) or VOICE_FEMALE (1)
void speaker_update_voice_setting(uint8_t voice);

// Enable/disable speaker playback (Warm-up greetings ON/OFF)
void speaker_set_voice_enabled(bool enabled);

#ifdef __cplusplus
}
#endif

#endif // SPEAKER_H
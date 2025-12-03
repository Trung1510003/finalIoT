#ifndef BLE_SCANNER_H
#define BLE_SCANNER_H

#include <stdint.h>
#include <stdbool.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "speaker.h"  // For sensor_data_t

#ifdef __cplusplus
extern "C" {
#endif

// Global sensor data variables (exposed for screen component)
extern int   front_left_updated;
extern float front_left_voltage;
extern int   front_left_temperature;
extern uint16_t front_left_pressure_psi_x10;
extern float front_left_pressure_psi;

extern int   front_right_updated;
extern float front_right_voltage;
extern int   front_right_temperature;
extern uint16_t front_right_pressure_psi_x10;
extern float front_right_pressure_psi;

extern int   rear_left_updated;
extern float rear_left_voltage;
extern int   rear_left_temperature;
extern uint16_t rear_left_pressure_psi_x10;
extern float rear_left_pressure_psi;

extern int   rear_right_updated;
extern float rear_right_voltage;
extern int   rear_right_temperature;
extern uint16_t rear_right_pressure_psi_x10;
extern float rear_right_pressure_psi;

// Initialize BLE scanner
void ble_scanner_init(void);

// Start BLE scanning
// event_group: Event group to signal when BLE is ready (optional, can be NULL)
void ble_scanner_start(QueueHandle_t speaker_queue, EventGroupHandle_t event_group);

// Lock/unlock sensor data for thread-safe access
// Use these when reading sensor data from other tasks
void ble_scanner_lock_sensor_data(void);
void ble_scanner_unlock_sensor_data(void);

#ifdef __cplusplus
}
#endif

#endif // BLE_SCANNER_H


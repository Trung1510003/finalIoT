#ifndef BUTTON_H
#define BUTTON_H

#include <stdint.h>
#include <stdbool.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "screen.h"  // For ui_update_t

#ifdef __cplusplus
extern "C" {
#endif

// Button configuration
#define PIN_BTN_UP       GPIO_NUM_6
#define PIN_BTN_MODE     GPIO_NUM_7
#define PIN_BTN_DOWN     GPIO_NUM_9
#define BTN_ACTIVE_LEVEL 0

// Button timing constants
#define BTN_POLL_INTERVAL_MS   5
#define DEBOUNCE_MS            30
#define HOLD_THRESHOLD_MS      450
#define REPEAT_START_MS        120
#define REPEAT_ACCEL_MS        20
#define REPEAT_MIN_MS          60
#define MODE_LONG_MS           2000

// Initialize button component
void button_init(void);

// Start button input task
void button_task_start(QueueHandle_t ui_queue);

#ifdef __cplusplus
}
#endif

#endif // BUTTON_H


#ifndef SCREEN_H
#define SCREEN_H

#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <u8g2.h>

#ifdef __cplusplus
extern "C" {
#endif

// UI mode enum
typedef enum { 
    MODE_MENU = 0, 
    MODE_HELLO = 1, 
    MODE_ITEM = 2, 
    MODE_ADJUST = 3, 
    MODE_SENSOR = 4 
} ui_mode_t;

// UI update structure
typedef struct {
    ui_mode_t mode;
    int sel;
    int sub;
    int adjust;
    uint8_t toggle_mirror;
} ui_update_t;

// Initialize screen component
void screen_init(void);

// Start screen task
// event_group: Event group to signal when screen is ready (optional, can be NULL)
void screen_task_start(QueueHandle_t ui_queue, EventGroupHandle_t event_group);

// Get UI queue (for other components to send updates)
QueueHandle_t screen_get_ui_queue(void);

// Get UI task handle (for task notifications)
TaskHandle_t screen_get_task_handle(void);

#ifdef __cplusplus
}
#endif

#endif // SCREEN_H
#include "button.h"
#include "tpms_config.h"
#include "screen.h"
#include "speaker.h"  // For speaker_update_voice_setting()
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include <math.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <freertos/timers.h>

static const char *TAG = "BUTTON";

static QueueHandle_t s_ui_queue = NULL;
static EventGroupHandle_t s_event_group = NULL;
static TimerHandle_t s_button_poll_timer = NULL;
static SemaphoreHandle_t s_button_poll_sem = NULL;

// Timer callback for button polling (runs in timer task context)
static void button_poll_timer_callback(TimerHandle_t xTimer) {
    // Give semaphore to wake up button task
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (s_button_poll_sem != NULL) {
        xSemaphoreGiveFromISR(s_button_poll_sem, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

#define MENU_COUNT 9
#define TIRE_SWAP_COUNT 4

typedef enum {
    BTN_ID_UP = 0,
    BTN_ID_MODE,
    BTN_ID_DOWN,
    BTN_ID_COUNT
} btn_id_t;

typedef struct {
    gpio_num_t   pin;
    int          active_level;
    int          raw_level;
    int          debounced_level;
    int          last_stable_level;
    int64_t      last_change_us;
    int          is_pressed;
    int64_t      press_start_us;
    int64_t      next_repeat_us;
    int          repeat_interval_ms;
    bool         long_fired;
    bool         suppress_click;
} button_t;

static button_t s_btns[BTN_ID_COUNT];

static void buttons_init_polling(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL<<PIN_BTN_UP) | (1ULL<<PIN_BTN_DOWN) | (1ULL<<PIN_BTN_MODE),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = (BTN_ACTIVE_LEVEL==0) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = (BTN_ACTIVE_LEVEL==1) ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    s_btns[BTN_ID_UP] = (button_t){.pin = PIN_BTN_UP, .active_level = BTN_ACTIVE_LEVEL};
    s_btns[BTN_ID_MODE] = (button_t){.pin = PIN_BTN_MODE, .active_level = BTN_ACTIVE_LEVEL};
    s_btns[BTN_ID_DOWN] = (button_t){.pin = PIN_BTN_DOWN, .active_level = BTN_ACTIVE_LEVEL};

    int64_t now = esp_timer_get_time();
    for (int i=0; i<BTN_ID_COUNT; ++i) {
        int lvl = gpio_get_level(s_btns[i].pin);
        s_btns[i].raw_level = s_btns[i].debounced_level = s_btns[i].last_stable_level = lvl;
        s_btns[i].last_change_us = now;
        s_btns[i].is_pressed = (lvl == s_btns[i].active_level);
        s_btns[i].press_start_us = 0;
        s_btns[i].next_repeat_us = 0;
        s_btns[i].repeat_interval_ms = REPEAT_START_MS;
        s_btns[i].long_fired = false;
        s_btns[i].suppress_click = false;
    }
}

static bool button_update(button_t* b, int64_t now_us, bool* out_rising, bool* out_falling, bool enable_repeat)
{
    *out_rising = *out_falling = false;

    int lvl = gpio_get_level(b->pin);
    if (lvl != b->raw_level) {
        b->raw_level = lvl;
        b->last_change_us = now_us;
    }
    if ((now_us - b->last_change_us) >= (int64_t)DEBOUNCE_MS*1000) {
        if (b->debounced_level != b->raw_level) {
            b->debounced_level = b->raw_level;
            if (b->debounced_level == b->active_level) {
                *out_rising = true;
            } else {
                *out_falling = true;
            }
            b->last_stable_level = b->debounced_level;
        }
    }

    bool pressed_now = (b->debounced_level == b->active_level);

    if (*out_rising) {
        b->is_pressed = true;
        b->press_start_us = now_us;
        b->repeat_interval_ms = REPEAT_START_MS;
        b->next_repeat_us = now_us + (int64_t)HOLD_THRESHOLD_MS*1000;
        b->long_fired = false;
    }

    if (*out_falling) {
        b->is_pressed = false;
        b->press_start_us = 0;
        b->next_repeat_us = 0;
        b->repeat_interval_ms = REPEAT_START_MS;
    }

    if (enable_repeat && pressed_now && b->next_repeat_us != 0) {
        if (now_us >= b->next_repeat_us) {
            *out_rising = true;
            b->repeat_interval_ms -= REPEAT_ACCEL_MS;
            if (b->repeat_interval_ms < REPEAT_MIN_MS) b->repeat_interval_ms = REPEAT_MIN_MS;
            b->next_repeat_us = now_us + (int64_t)b->repeat_interval_ms*1000;
        }
    }

    return true;
}

static void handle_high_temp_adjust(bool inc) {
    tpms_config_lock();
    TPMS_Config* cfg = tpms_config_get();
    if (inc) {
        if (cfg->high_temp_warning < 100) cfg->high_temp_warning += 1;
    } else {
        if (cfg->high_temp_warning > 0) cfg->high_temp_warning -= 1;
    }
    tpms_config_unlock();
    tpms_config_save();
}

static void input_task(void* pv)
{
    ui_mode_t mode = MODE_HELLO;
    int sel = 0;
    int sub = 0;
    int current_adjust = 0;

    buttons_init_polling();

    // Signal that button is ready
    if (s_event_group != NULL) {
        xEventGroupSetBits(s_event_group, (1 << 4)); // BIT_BUTTON_READY
        ESP_LOGI(TAG, "Button ready signal sent");
    }

    ui_update_t init = {.mode = mode, .sel = sel, .sub = sub, .adjust = current_adjust, .toggle_mirror = 0};
    xQueueSend(s_ui_queue, &init, 0);

    for (;;) {
        // Wait for timer semaphore (5ms period) instead of vTaskDelay
        if (s_button_poll_sem != NULL) {
            xSemaphoreTake(s_button_poll_sem, portMAX_DELAY);
        } else {
            // Fallback to delay if semaphore not available
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        
        int64_t now = esp_timer_get_time();

        bool up_rise=false, up_fall=false;
        bool dn_rise=false, dn_fall=false;
        bool md_rise=false, md_fall=false;

        bool repeat_allowed = (mode == MODE_MENU || mode == MODE_ADJUST || mode == MODE_ITEM || mode == MODE_SENSOR);

        button_update(&s_btns[BTN_ID_UP],   now, &up_rise, &up_fall, repeat_allowed);
        button_update(&s_btns[BTN_ID_DOWN], now, &dn_rise, &dn_fall, repeat_allowed);
        button_update(&s_btns[BTN_ID_MODE], now, &md_rise, &md_fall, false);

        // Long press DOWN button to toggle mirror
        if (s_btns[BTN_ID_DOWN].is_pressed && !s_btns[BTN_ID_DOWN].long_fired) {
            int64_t held_ms = (now - s_btns[BTN_ID_DOWN].press_start_us)/1000;
            if (held_ms >= 2000) { // 2 giây
                s_btns[BTN_ID_DOWN].long_fired = true;
                s_btns[BTN_ID_DOWN].suppress_click = true;
                s_btns[BTN_ID_DOWN].next_repeat_us = 0;

                ui_update_t u = {.mode = mode, .sel = sel, .sub = sub, .toggle_mirror = 1};
                xQueueSend(s_ui_queue, &u, 0);
                ESP_LOGI(TAG, "BTN9 long -> toggle display MIRROR");
            }
        }

        if (s_btns[BTN_ID_DOWN].suppress_click) {
            dn_rise = false;
        }

        if (dn_fall && s_btns[BTN_ID_DOWN].suppress_click) {
            s_btns[BTN_ID_DOWN].suppress_click = false;
        }

        if (mode == MODE_MENU) {
            if (up_rise) {
                sel = (sel - 1 + MENU_COUNT) % MENU_COUNT;
                ui_update_t u = {.mode = mode, .sel = sel, .sub = sub, .adjust = current_adjust, .toggle_mirror = 0};
                xQueueSend(s_ui_queue, &u, 0);
            }
            if (dn_rise) {
                sel = (sel + 1) % MENU_COUNT;
                ui_update_t u = {.mode = mode, .sel = sel, .sub = sub, .adjust = current_adjust, .toggle_mirror = 0};
                xQueueSend(s_ui_queue, &u, 0);
            }
        }

        if (s_btns[BTN_ID_MODE].is_pressed && !s_btns[BTN_ID_MODE].long_fired) {
            int64_t held_ms = (now - s_btns[BTN_ID_MODE].press_start_us)/1000;
            if ((mode == MODE_MENU || mode == MODE_ITEM || mode == MODE_ADJUST || mode == MODE_SENSOR) && held_ms >= MODE_LONG_MS) {
                mode = MODE_HELLO;
                s_btns[BTN_ID_MODE].long_fired = true;
                s_btns[BTN_ID_MODE].suppress_click = true;
                ui_update_t u = {.mode = mode, .sel = sel, .sub = sub, .adjust = current_adjust, .toggle_mirror = 0};
                xQueueSend(s_ui_queue, &u, 0);
                ESP_LOGI(TAG, "Switch -> HELLO (long press)");
            }
        }

        if (md_fall) {
            if (s_btns[BTN_ID_MODE].suppress_click) {
                s_btns[BTN_ID_MODE].suppress_click = false;
            } else if (!s_btns[BTN_ID_MODE].long_fired) {
                if (mode == MODE_HELLO) {
                    mode = MODE_MENU;
                } else if (mode == MODE_MENU) {
                    int idx = sel % MENU_COUNT;
                    if (idx == 4) {
                        mode = MODE_ADJUST;
                        current_adjust = 5; // ADJ_HIGH_TEMP
                    } else if (idx == 6 || idx == 8) {
                        mode = MODE_ITEM;
                        sub = 0;
                    } else {
                        mode = MODE_ITEM;
                        sub = 0;
                        // Lock config to read tire_swap
                        tpms_config_lock();
                        if (idx == 5) sub = (int)tpms_config_get()->tire_swap;
                        tpms_config_unlock();
                    }
                } else if (mode == MODE_ITEM) {
                    int idx = sel % MENU_COUNT;
                    tpms_config_lock();
                    TPMS_Config* cfg = tpms_config_get();
                    if (idx == 0) {
                        cfg->warm_up_greetings = (sub == 0);
                        tpms_config_unlock();
                        tpms_config_save();
                        mode = MODE_MENU;
                    } else if (idx == 1) {
                        voice_gender_t new_voice = (sub == 0) ? VOICE_MALE : VOICE_FEMALE;
                        cfg->warning_settings = new_voice;
                        tpms_config_unlock();
                        tpms_config_save();
                        // Update speaker component with new voice setting (avoid lock/unlock on every audio play)
                        speaker_update_voice_setting((uint8_t)new_voice);
                        mode = MODE_MENU;
                    } else if (idx == 2) {
                        tpms_config_unlock();
                        current_adjust = (sub == 0) ? 1 : 2; // ADJ_FRONT_UPPER : ADJ_FRONT_LOWER
                        mode = MODE_ADJUST;
                    } else if (idx == 3) {
                        tpms_config_unlock();
                        current_adjust = (sub == 0) ? 3 : 4; // ADJ_REAR_UPPER : ADJ_REAR_LOWER
                        mode = MODE_ADJUST;
                    } else if (idx == 5) {
                        cfg->tire_swap = (TireSwapMode)(sub % TIRE_SWAP_COUNT);
                        TireSwapMode swap = cfg->tire_swap;
                        tpms_config_unlock();
                        tpms_config_save();
                        tpms_config_update_devices_by_swap_mode(swap);
                        mode = MODE_MENU;
                    } else if (idx == 6) {
                        tpms_config_unlock();
                        mode = MODE_SENSOR;
                    } else if (idx == 7) {
                        cfg->tire_pressure_unit = (sub == 0) ? PSI_UNIT : BAR_UNIT;
                        tpms_config_unlock();
                        tpms_config_save();
                        mode = MODE_MENU;
                    } else {
                        tpms_config_unlock();
                        mode = MODE_MENU;
                    }
                } else if (mode == MODE_ADJUST || mode == MODE_SENSOR) {
                    mode = MODE_MENU;
                    current_adjust = 0;
                }
                ui_update_t u = {.mode = mode, .sel = sel, .sub = sub, .adjust = current_adjust, .toggle_mirror = 0};
                xQueueSend(s_ui_queue, &u, 0);
                ESP_LOGI(TAG, "MODE short -> %s",
                         (mode==MODE_MENU)?"MENU":(mode==MODE_ITEM)?"ITEM":(mode==MODE_ADJUST)?"ADJUST":(mode==MODE_SENSOR)?"SENSOR":"HELLO");
            }
        }

        if (mode == MODE_ITEM && (sel == 6 || sel == 8)) {
            if (up_rise || dn_rise) {
                sub ^= 1;
                ui_update_t u = {.mode = mode, .sel = sel, .sub = sub, .adjust = current_adjust, .toggle_mirror = 0};
                xQueueSend(s_ui_queue, &u, 0);
            }
        } else if (mode == MODE_ITEM && sel == 5) {
            if (up_rise) {
                sub = (sub - 1 + TIRE_SWAP_COUNT) % TIRE_SWAP_COUNT;
                ui_update_t u = {.mode = mode, .sel = sel, .sub = sub, .adjust = current_adjust, .toggle_mirror = 0};
                xQueueSend(s_ui_queue, &u, 0);
            }
            if (dn_rise) {
                sub = (sub + 1) % TIRE_SWAP_COUNT;
                ui_update_t u = {.mode = mode, .sel = sel, .sub = sub, .adjust = current_adjust, .toggle_mirror = 0};
                xQueueSend(s_ui_queue, &u, 0);
            }
        } else if (mode == MODE_ITEM) {
            if (up_rise || dn_rise) {
                sub ^= 1;
                ui_update_t u = {.mode = mode, .sel = sel, .sub = sub, .adjust = current_adjust, .toggle_mirror = 0};
                xQueueSend(s_ui_queue, &u, 0);
            }
        }

        if (mode == MODE_ADJUST) {
            bool inc = false;
            if (up_rise) {
                inc = true;
            } else if (dn_rise) {
                inc = false;
            } else {
                // No button press, continue to wait for next timer tick
                continue;
            }

            float step = 0.1f;
            bool need_save = false;
            
            // Lock config to safely modify values
            tpms_config_lock();
            TPMS_Config* cfg = tpms_config_get();
            if (inc) {
                switch (current_adjust) {
                    case 1: // ADJ_FRONT_UPPER
                        cfg->front_tire_press_Upper_limit += step;
                        cfg->front_tire_press_Upper_limit = roundf(cfg->front_tire_press_Upper_limit * 10.0f) / 10.0f;
                        need_save = true;
                        break;
                    case 2: // ADJ_FRONT_LOWER
                        cfg->front_tire_press_Lower_limit += step;
                        cfg->front_tire_press_Lower_limit = roundf(cfg->front_tire_press_Lower_limit * 10.0f) / 10.0f;
                        need_save = true;
                        break;
                    case 3: // ADJ_REAR_UPPER
                        cfg->rear_tire_press_Upper_limit += step;
                        cfg->rear_tire_press_Upper_limit = roundf(cfg->rear_tire_press_Upper_limit * 10.0f) / 10.0f;
                        need_save = true;
                        break;
                    case 4: // ADJ_REAR_LOWER
                        cfg->rear_tire_press_Lower_limit += step;
                        cfg->rear_tire_press_Lower_limit = roundf(cfg->rear_tire_press_Lower_limit * 10.0f) / 10.0f;
                        need_save = true;
                        break;
                    case 5: // ADJ_HIGH_TEMP
                        tpms_config_unlock();
                        handle_high_temp_adjust(true);
                        break;
                    default:
                        tpms_config_unlock();
                        break;
                }
            } else {
                switch (current_adjust) {
                    case 1: // ADJ_FRONT_UPPER
                        cfg->front_tire_press_Upper_limit -= step;
                        cfg->front_tire_press_Upper_limit = roundf(cfg->front_tire_press_Upper_limit * 10.0f) / 10.0f;
                        need_save = true;
                        break;
                    case 2: // ADJ_FRONT_LOWER
                        cfg->front_tire_press_Lower_limit -= step;
                        cfg->front_tire_press_Lower_limit = roundf(cfg->front_tire_press_Lower_limit * 10.0f) / 10.0f;
                        need_save = true;
                        break;
                    case 3: // ADJ_REAR_UPPER
                        cfg->rear_tire_press_Upper_limit -= step;
                        cfg->rear_tire_press_Upper_limit = roundf(cfg->rear_tire_press_Upper_limit * 10.0f) / 10.0f;
                        need_save = true;
                        break;
                    case 4: // ADJ_REAR_LOWER
                        cfg->rear_tire_press_Lower_limit -= step;
                        cfg->rear_tire_press_Lower_limit = roundf(cfg->rear_tire_press_Lower_limit * 10.0f) / 10.0f;
                        need_save = true;
                        break;
                    case 5: // ADJ_HIGH_TEMP
                        tpms_config_unlock();
                        handle_high_temp_adjust(false);
                        break;
                    default:
                        tpms_config_unlock();
                        break;
                }
            }
            
            if (need_save) {
                tpms_config_unlock();
                tpms_config_save();
            } else if (current_adjust != 5) {
                tpms_config_unlock();
            }
            
            ui_update_t u = {.mode = mode, .sel = sel, .sub = sub, .adjust = current_adjust, .toggle_mirror = 0};
            xQueueSend(s_ui_queue, &u, 0);
        }
        // No vTaskDelay here - timer will wake us up every 5ms
    }
}

void button_init(void) {
    // Create binary semaphore for timer signaling
    s_button_poll_sem = xSemaphoreCreateBinary();
    if (s_button_poll_sem == NULL) {
        ESP_LOGE(TAG, "Failed to create button poll semaphore");
        return;
    }
    
    // Create FreeRTOS software timer for button polling (5ms period)
    s_button_poll_timer = xTimerCreate(
        "btn_poll",                           // Timer name
        pdMS_TO_TICKS(10),  // Period: 5ms
        pdTRUE,                               // Auto-reload (periodic)
        (void*)0,                             // Timer ID
        button_poll_timer_callback            // Callback function
    );
    
    if (s_button_poll_timer == NULL) {
        ESP_LOGE(TAG, "Failed to create button poll timer");
        vSemaphoreDelete(s_button_poll_sem);
        s_button_poll_sem = NULL;
        return;
    }
    
    ESP_LOGI(TAG, "Button polling timer created (5ms period)");
}

void button_task_start(QueueHandle_t ui_queue, EventGroupHandle_t event_group) {
    s_ui_queue = ui_queue;
    s_event_group = event_group;
    
    // Start the button polling timer
    if (s_button_poll_timer != NULL) {
        if (xTimerStart(s_button_poll_timer, 0) != pdPASS) {
            ESP_LOGE(TAG, "Failed to start button poll timer");
        } else {
            ESP_LOGI(TAG, "Button polling timer started");
        }
    }
    
    xTaskCreate(input_task, "input_task", 3072, NULL, 3, NULL);
}


#include "screen.h"
#include "tpms_config.h"
#include "ble_scanner.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "u8g2_esp32_hal.h"
#include <string.h>
#include <math.h>
#include <freertos/event_groups.h>

static const char *TAG = "SCREEN";

// OLED configuration
#define PIN_SDA 5
#define PIN_SCL 4
#define OLED_ADDR_7BIT 0x3C
#define BLINK_INTERVAL_MS 500

static QueueHandle_t s_ui_queue = NULL;
static EventGroupHandle_t s_event_group = NULL;
static TaskHandle_t s_ui_task_handle = NULL;

// Menu constants
#define MENU_COUNT 9
#define TIRE_SWAP_COUNT 4
#define SENSOR_COUNT 4

/* ==== Bitmaps ==== */
static const unsigned char image_arrow_FL_bits[] = {
  0xff,0xff,0xff,0xff,0xff,0x07,0x00,0xff,0xff,0xff,0xff,0xff,0x0f,0x00,0x00,0x00,
  0x00,0x00,0x00,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x30,0x00,0x00,0x00,0x00,0x00,
  0x00,0xe0,0x03,0x00,0x00,0x00,0x00,0x00,0xc0,0x03
};
static const unsigned char image_arrow_FR_bits[] = {
  0x80,0xff,0xff,0xff,0xff,0xff,0x03,0xc0,0xff,0xff,0xff,0xff,0xff,0x03,0x60,0x00,
  0x00,0x00,0x00,0x00,0x00,0x30,0x00,0x00,0x00,0x00,0x00,0x00,0x1f,0x00,0x00,0x00,
  0x00,0x00,0x00,0x0f,0x00,0x00,0x00,0x00,0x00,0x00
};
static const unsigned char image_arrow_RL_bits[] = {
  0x00,0x00,0x00,0x00,0x00,0xc0,0x03,0x00,0x00,0x00,0x00,0x00,0xe0,0x03,0x00,0x00,
  0x00,0x00,0x00,0x30,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x00,0xff,0xff,0xff,0xff,
  0xff,0x0f,0x00,0xff,0xff,0xff,0xff,0xff,0x07,0x00
};
static const unsigned char image_arrow_RR_bits[] = {
  0x0f,0x00,0x00,0x00,0x00,0x00,0x00,0x1f,0x00,0x00,0x00,0x00,0x00,0x00,0x30,0x00,
  0x00,0x00,0x00,0x00,0x00,0x60,0x00,0x00,0x00,0x00,0x00,0x00,0xc0,0xff,0xff,0xff,
  0xff,0xff,0x03,0x80,0xff,0xff,0xff,0xff,0xff,0x03
};
static const unsigned char image_car_bits[] = {
  0x80,0xff,0x7f,0x00,0xe0,0x03,0xf0,0x01,0x70,0x01,0xa0,0x03,0x10,0xff,0x3f,0x02,
  0x18,0xff,0x3f,0x06,0x98,0xff,0x7f,0x06,0xf8,0xff,0xff,0x07,0xf8,0xff,0xff,0x07,
  0xf8,0xff,0xff,0x07,0xf8,0xff,0xff,0x07,0xf8,0xff,0xff,0x07,0xf8,0x00,0xc0,0x07,
  0x38,0x00,0x00,0x07,0x18,0x00,0x00,0x06,0x1e,0x00,0x00,0x1e,0x3f,0x00,0x00,0x3f,
  0x3f,0x00,0x00,0x3f,0x78,0x00,0x80,0x07,0x78,0x00,0x80,0x07,0xf8,0x00,0xc0,0x07,
  0xd8,0xff,0xff,0x06,0xd8,0xff,0xff,0x06,0x98,0xff,0x7f,0x06,0x98,0xff,0x7f,0x06,
  0x98,0xff,0x7f,0x06,0x98,0xff,0x7f,0x06,0x98,0x8a,0x44,0x06,0x98,0xaa,0x57,0x06,
  0x98,0xca,0x64,0x06,0x98,0xec,0x54,0x06,0x98,0xff,0x7f,0x06,0x98,0xff,0x7f,0x06,
  0x98,0xff,0x7f,0x06,0x98,0xff,0x7f,0x06,0x98,0xff,0x7f,0x06,0xd8,0xf9,0xe7,0x06,
  0xf8,0x00,0xc0,0x07,0xf8,0x00,0xc0,0x07,0x78,0x00,0x80,0x07,0x78,0x00,0x80,0x07,
  0x78,0x00,0x80,0x07,0x78,0x00,0x80,0x07,0xf8,0x01,0xe0,0x07,0xf8,0xff,0xff,0x07,
  0xf8,0xff,0xff,0x07,0xf8,0xff,0xff,0x07,0xf0,0xff,0xff,0x03,0xf0,0xff,0xff,0x03,
  0xc0,0xff,0xff,0x00
};

/* ==== Blinking state for out-of-range pressures and temperatures ==== */
static bool blink_visible = true;
static int64_t last_blink_toggle_us = 0;

/* ===================== Menu data ===================== */
static const char* menu_items[] = {
    "Warm-up greetings",
    "Warning settings",
    "Front tire pressure",
    "Rear tire pressure",
    "High temp warning",
    "Tire swap",
    "Connect the sensor",
    "Unit pressure",
    "Restore settings"
};

static const char* tire_swap_options[] = {
    "Initial",
    "Vertical",
    "Cross",
    "Horizontal"
};

static const char* sensor_options[] = {
    "Front Left",
    "Front Right",
    "Rear Left",
    "Rear Right"
};

static const char* restore_options[] = {
    "Quit",
    "Restore setting"
};

/* ===================== Draw helpers ===================== */
static void draw_menu_3line(u8g2_t* u8g2, int sel) {
    int top = (sel - 1 + MENU_COUNT) % MENU_COUNT;
    int mid = sel;
    int bot = (sel + 1) % MENU_COUNT;

    u8g2_ClearBuffer(u8g2);
    u8g2_SetFont(u8g2, u8g2_font_helvB08_tf);

    int y_top = 16, y_mid = 32, y_bot = 48;
    int box_h = 16;
    int box_y = y_mid - (box_h - 4);
    if (box_y < 0) box_y = 0;

    u8g2_SetDrawColor(u8g2, 1);
    u8g2_DrawBox(u8g2, 0, box_y, 128, box_h);

    int w;
    u8g2_SetDrawColor(u8g2, 1);
    w = u8g2_GetStrWidth(u8g2, menu_items[top]);
    u8g2_DrawStr(u8g2, (128 - w)/2, y_top, menu_items[top]);
    u8g2_SetDrawColor(u8g2, 0);
    w = u8g2_GetStrWidth(u8g2, menu_items[mid]);
    u8g2_DrawStr(u8g2, (128 - w)/2, y_mid, menu_items[mid]);
    u8g2_SetDrawColor(u8g2, 1);
    w = u8g2_GetStrWidth(u8g2, menu_items[bot]);
    u8g2_DrawStr(u8g2, (128 - w)/2, y_bot, menu_items[bot]);

    u8g2_SetFont(u8g2, u8g2_font_unifont_t_75);
    u8g2_DrawGlyph(u8g2, 3,   64, 0x25B2);
    u8g2_DrawGlyph(u8g2, 118, 64, 0x25BC);
    u8g2_SetFont(u8g2, u8g2_font_6x12_t_symbols);
    u8g2_DrawGlyph(u8g2, 55, 62, 0x2713);
    u8g2_DrawGlyph(u8g2, 60, 62, '/');
    u8g2_DrawGlyph(u8g2, 65, 62, 0x21B5);

    u8g2_SendBuffer(u8g2);
}

static void draw_hello_tpms(u8g2_t* u8g2) {
    char helper_c_string[16];
    int  helper_str_width;
    u8g2_ClearBuffer(u8g2);
    u8g2_SetFontMode(u8g2, 1);
    u8g2_SetBitmapMode(u8g2, 1);

    // Lock sensor data and config for thread-safe reading
    ble_scanner_lock_sensor_data();
    tpms_config_lock();
    
    // Read sensor data and config values atomically
    float fl_pressure = front_left_pressure_psi;
    float fr_pressure = front_right_pressure_psi;
    float rl_pressure = rear_left_pressure_psi;
    float rr_pressure = rear_right_pressure_psi;
    int fl_temp = front_left_temperature;
    int fr_temp = front_right_temperature;
    int rl_temp = rear_left_temperature;
    int rr_temp = rear_right_temperature;
    float fl_voltage = front_left_voltage;
    float fr_voltage = front_right_voltage;
    float rl_voltage = rear_left_voltage;
    float rr_voltage = rear_right_voltage;
    
    float front_lower = tpms_config_get()->front_tire_press_Lower_limit;
    float front_upper = tpms_config_get()->front_tire_press_Upper_limit;
    float rear_lower = tpms_config_get()->rear_tire_press_Lower_limit;
    float rear_upper = tpms_config_get()->rear_tire_press_Upper_limit;
    uint8_t high_temp = tpms_config_get()->high_temp_warning;
    
    tpms_config_unlock();
    ble_scanner_unlock_sensor_data();

    // Check if pressures are within limits
    bool fl_press_out_of_range = (fl_pressure < front_lower || fl_pressure > front_upper) && (fl_pressure > 0);
    bool fr_press_out_of_range = (fr_pressure < front_lower || fr_pressure > front_upper) && (fr_pressure > 0);
    bool rl_press_out_of_range = (rl_pressure < rear_lower || rl_pressure > rear_upper) && (rl_pressure > 0);
    bool rr_press_out_of_range = (rr_pressure < rear_lower || rr_pressure > rear_upper) && (rr_pressure > 0);

    // Check if temperatures exceed high_temp_warning
    bool fl_temp_out_of_range = (fl_temp > high_temp);
    bool fr_temp_out_of_range = (fr_temp > high_temp);
    bool rl_temp_out_of_range = (rl_temp > high_temp);
    bool rr_temp_out_of_range = (rr_temp > high_temp);

    bool fl_battery_out_of = (fl_voltage < 2.8);
    bool fr_battery_out_of = (fr_voltage < 2.8);
    bool rl_battery_out_of = (rl_voltage < 2.8);
    bool rr_battery_out_of = (rr_voltage < 2.8);


    // Front Left
    u8g2_SetFont(u8g2, u8g2_font_helvB08_tf);
    if (!fl_temp_out_of_range || (fl_temp_out_of_range && blink_visible)) {
        snprintf(helper_c_string, sizeof(helper_c_string), "%d%cC", fl_temp, 176);
        u8g2_DrawStr(u8g2, 0, 24, helper_c_string);
    }

    u8g2_SetFont(u8g2, u8g2_font_siji_t_6x10);
    (fl_battery_out_of)?
    u8g2_DrawGlyph(u8g2, 26, 24, 0xe236): u8g2_DrawGlyph(u8g2, 26, 24, 0xe238);


    u8g2_SetFont(u8g2, u8g2_font_profont17_tr);
    if (!fl_press_out_of_range || (fl_press_out_of_range && blink_visible)) {
        snprintf(helper_c_string, sizeof(helper_c_string), "%.1f", fl_pressure);
        u8g2_DrawStr(u8g2, 0, 12, helper_c_string);
    }

    // Front Right
    u8g2_SetFont(u8g2, u8g2_font_helvB08_tf);
    if (!fr_temp_out_of_range || (fr_temp_out_of_range && blink_visible)) {
        snprintf(helper_c_string, sizeof(helper_c_string), "%d%cC", fr_temp, 176);
        helper_str_width = u8g2_GetStrWidth(u8g2, helper_c_string);
        u8g2_DrawStr(u8g2, 128 - helper_str_width, 24, helper_c_string);
    }

    u8g2_SetFont(u8g2, u8g2_font_siji_t_6x10);
    (fr_battery_out_of)?
    u8g2_DrawGlyph(u8g2, 90, 24, 0xe23B): u8g2_DrawGlyph(u8g2, 90, 24, 0xe23D);

    u8g2_SetFont(u8g2, u8g2_font_profont17_tr);
    if (!fr_press_out_of_range || (fr_press_out_of_range && blink_visible)) {
        snprintf(helper_c_string, sizeof(helper_c_string), "%.1f", fr_pressure);
        helper_str_width = u8g2_GetStrWidth(u8g2, helper_c_string);
        u8g2_DrawStr(u8g2, 128 - helper_str_width, 12, helper_c_string);
    }

    // Rear Left
    u8g2_SetFont(u8g2, u8g2_font_helvB08_tf);
    if (!rl_temp_out_of_range || (rl_temp_out_of_range && blink_visible)) {
        snprintf(helper_c_string, sizeof(helper_c_string), "%d%cC", rl_temp, 176);
        u8g2_DrawStr(u8g2, 0, 62, helper_c_string);
    }

    u8g2_SetFont(u8g2, u8g2_font_siji_t_6x10);
    (rl_battery_out_of)?
    u8g2_DrawGlyph(u8g2, 26, 62, 0xe236): u8g2_DrawGlyph(u8g2, 26, 62, 0xe238);

    u8g2_SetFont(u8g2, u8g2_font_profont17_tr);
    if (!rl_press_out_of_range || (rl_press_out_of_range && blink_visible)) {
        snprintf(helper_c_string, sizeof(helper_c_string), "%.1f", rl_pressure);
        u8g2_DrawStr(u8g2, 0, 50, helper_c_string);
    }

    // Rear Right
    u8g2_SetFont(u8g2, u8g2_font_helvB08_tf);
    if (!rr_temp_out_of_range || (rr_temp_out_of_range && blink_visible)) {
        snprintf(helper_c_string, sizeof(helper_c_string), "%d%cC", rr_temp, 176);
        helper_str_width = u8g2_GetStrWidth(u8g2, helper_c_string);
        u8g2_DrawStr(u8g2, 128 - helper_str_width, 62, helper_c_string);
    }

    u8g2_SetFont(u8g2, u8g2_font_siji_t_6x10);
    (rr_battery_out_of)?
    u8g2_DrawGlyph(u8g2, 90, 62, 0xe23B): u8g2_DrawGlyph(u8g2, 90, 62, 0xe23D);

    u8g2_SetFont(u8g2, u8g2_font_profont17_tr);
    if (!rr_press_out_of_range || (rr_press_out_of_range && blink_visible)) {
        snprintf(helper_c_string, sizeof(helper_c_string), "%.1f", rr_pressure);
        helper_str_width = u8g2_GetStrWidth(u8g2, helper_c_string);
        u8g2_DrawStr(u8g2, 128 - helper_str_width, 50, helper_c_string);
    }

    // Draw static elements
    u8g2_DrawXBMP(u8g2,  0, 47, 50, 6,  image_arrow_RL_bits);
    u8g2_DrawXBMP(u8g2,  0, 13, 50, 6,  image_arrow_FL_bits);
    u8g2_DrawXBMP(u8g2, 78, 47, 50, 6,  image_arrow_RR_bits);
    u8g2_DrawXBMP(u8g2, 78, 13, 50, 6,  image_arrow_FR_bits);
    u8g2_DrawXBMP(u8g2, 49,  9, 30, 49, image_car_bits);

    // Lock config again to read unit and warm_up_greetings
    tpms_config_lock();
    unit_pressure_t unit = tpms_config_get()->tire_pressure_unit;
    uint8_t warm_up = tpms_config_get()->warm_up_greetings;
    tpms_config_unlock();

    u8g2_SetFont(u8g2, u8g2_font_helvB08_tf);
    u8g2_DrawStr(u8g2, 44, 8, unit == PSI_UNIT ? "PSI" : "BAR");
    u8g2_SetFont(u8g2, u8g2_font_open_iconic_embedded_1x_t);
    u8g2_DrawGlyph(u8g2, 68, 8, 0x0042);
    u8g2_SetFont(u8g2, u8g2_font_open_iconic_play_1x_t);
    (warm_up == 1)?
    u8g2_DrawGlyph(u8g2, 80, 8, 0x0040): u8g2_DrawGlyph(u8g2, 80, 8, 0x0051);


    u8g2_SendBuffer(u8g2);
}

static void draw_two_option_screen(u8g2_t* u8g2,
                                   const char* title,
                                   const char* opt0,
                                   const char* opt1,
                                   int cursor,
                                   int checked_idx)
{
    u8g2_ClearBuffer(u8g2);

    u8g2_SetFont(u8g2, u8g2_font_6x12_tf);
    int w = u8g2_GetStrWidth(u8g2, title);
    u8g2_DrawStr(u8g2, (128 - w)/2, 15, title);
    u8g2_DrawHLine(u8g2, 0, 19, 128);

    const int y0 = 34;
    const int y1 = 52;

    int box_y = (cursor == 0) ? (y0 - 12) : (y1 - 12);
    u8g2_SetDrawColor(u8g2, 1);
    u8g2_DrawBox(u8g2, 2, box_y, 124, 14);

    u8g2_SetFont(u8g2, u8g2_font_helvB08_tf);
    u8g2_SetDrawColor(u8g2, (cursor == 0) ? 0 : 1);
    u8g2_DrawStr(u8g2, 12, y0, opt0);
    u8g2_SetDrawColor(u8g2, (cursor == 1) ? 0 : 1);
    u8g2_DrawStr(u8g2, 12, y1, opt1);

    if (checked_idx >= 0) {
        int y_check = (checked_idx == 0) ? y0 : y1;
        bool check_on_highlight = (cursor == checked_idx);
        u8g2_SetDrawColor(u8g2, check_on_highlight ? 0 : 1);
        u8g2_SetFont(u8g2, u8g2_font_6x12_t_symbols);
        u8g2_DrawGlyph(u8g2, 100, y_check, 0x2713);
    }

    u8g2_SetDrawColor(u8g2, 1);
    u8g2_SetFont(u8g2, u8g2_font_unifont_t_75);
    u8g2_DrawGlyph(u8g2, 3,   64, 0x25B2);
    u8g2_DrawGlyph(u8g2, 118, 64, 0x25BC);
    u8g2_SetFont(u8g2, u8g2_font_6x12_t_symbols);
    u8g2_DrawGlyph(u8g2, 55, 62, 0x2713);
    u8g2_DrawGlyph(u8g2, 60, 62, '/');
    u8g2_DrawGlyph(u8g2, 65, 62, 0x21B5);

    u8g2_SendBuffer(u8g2);
}

static void draw_tire_swap_menu(u8g2_t* u8g2, int cursor, int selected) {
    int top = (cursor - 1 + TIRE_SWAP_COUNT) % TIRE_SWAP_COUNT;
    int mid = cursor % TIRE_SWAP_COUNT;
    int bot = (cursor + 1) % TIRE_SWAP_COUNT;

    u8g2_ClearBuffer(u8g2);

    u8g2_SetFont(u8g2, u8g2_font_6x12_tf);
    int w = u8g2_GetStrWidth(u8g2, "Tire swap");
    u8g2_DrawStr(u8g2, (128 - w)/2, 12, "Tire swap");
    u8g2_DrawHLine(u8g2, 0, 15, 128);

    u8g2_SetFont(u8g2, u8g2_font_helvB08_tf);

    int y_top = 25, y_mid = 37, y_bot = 49;
    int box_h = 12;
    int box_y = y_mid - 9;

    u8g2_SetDrawColor(u8g2, 1);
    u8g2_DrawBox(u8g2, 2, box_y, 124, box_h);

    u8g2_SetDrawColor(u8g2, 1);
    w = u8g2_GetStrWidth(u8g2, tire_swap_options[top]);
    u8g2_DrawStr(u8g2, (128 - w)/2, y_top, tire_swap_options[top]);
    u8g2_SetDrawColor(u8g2, 0);
    w = u8g2_GetStrWidth(u8g2, tire_swap_options[mid]);
    u8g2_DrawStr(u8g2, (128 - w)/2, y_mid, tire_swap_options[mid]);
    u8g2_SetDrawColor(u8g2, 1);
    w = u8g2_GetStrWidth(u8g2, tire_swap_options[bot]);
    u8g2_DrawStr(u8g2, (128 - w)/2, y_bot, tire_swap_options[bot]);

    u8g2_SetFont(u8g2, u8g2_font_6x12_t_symbols);
    if (selected == top) {
        u8g2_SetDrawColor(u8g2, 1);
        u8g2_DrawGlyph(u8g2, 110, y_top, 0x2713);
    }
    if (selected == mid) {
        u8g2_SetDrawColor(u8g2, 0);
        u8g2_DrawGlyph(u8g2, 110, y_mid, 0x2713);
    }
    if (selected == bot) {
        u8g2_SetDrawColor(u8g2, 1);
        u8g2_DrawGlyph(u8g2, 110, y_bot, 0x2713);
    }

    u8g2_SetDrawColor(u8g2, 1);
    u8g2_SetFont(u8g2, u8g2_font_unifont_t_75);
    u8g2_DrawGlyph(u8g2, 3,   64, 0x25B2);
    u8g2_DrawGlyph(u8g2, 118, 64, 0x25BC);
    u8g2_SetFont(u8g2, u8g2_font_6x12_t_symbols);
    u8g2_DrawGlyph(u8g2, 55, 62, 0x2713);
    u8g2_DrawGlyph(u8g2, 60, 62, '/');
    u8g2_DrawGlyph(u8g2, 65, 62, 0x21B5);

    u8g2_SendBuffer(u8g2);
}

static void draw_sensor_menu(u8g2_t* u8g2, int cursor) {
    int top = (cursor - 1 + SENSOR_COUNT) % SENSOR_COUNT;
    int mid = cursor % SENSOR_COUNT;
    int bot = (cursor + 1) % SENSOR_COUNT;

    u8g2_ClearBuffer(u8g2);

    u8g2_SetFont(u8g2, u8g2_font_6x12_tf);
    int w = u8g2_GetStrWidth(u8g2, "Connect the sensor");
    u8g2_DrawStr(u8g2, (128 - w)/2, 12, "Connect the sensor");
    u8g2_DrawHLine(u8g2, 0, 15, 128);

    u8g2_SetFont(u8g2, u8g2_font_helvB08_tf);

    int y_top = 25, y_mid = 37, y_bot = 49;
    int box_h = 12;
    int box_y = y_mid - 9;

    u8g2_SetDrawColor(u8g2, 1);
    u8g2_DrawBox(u8g2, 2, box_y, 124, box_h);

    u8g2_SetDrawColor(u8g2, 1);
    w = u8g2_GetStrWidth(u8g2, sensor_options[top]);
    u8g2_DrawStr(u8g2, (128 - w)/2, y_top, sensor_options[top]);
    u8g2_SetDrawColor(u8g2, 0);
    w = u8g2_GetStrWidth(u8g2, sensor_options[mid]);
    u8g2_DrawStr(u8g2, (128 - w)/2, y_mid, sensor_options[mid]);
    u8g2_SetDrawColor(u8g2, 1);
    w = u8g2_GetStrWidth(u8g2, sensor_options[bot]);
    u8g2_DrawStr(u8g2, (128 - w)/2, y_bot, sensor_options[bot]);

    u8g2_SetDrawColor(u8g2, 1);
    u8g2_SetFont(u8g2, u8g2_font_unifont_t_75);
    u8g2_DrawGlyph(u8g2, 3,   64, 0x25B2);
    u8g2_DrawGlyph(u8g2, 118, 64, 0x25BC);
    u8g2_SetFont(u8g2, u8g2_font_6x12_t_symbols);
    u8g2_DrawGlyph(u8g2, 55, 62, 0x2713);
    u8g2_DrawGlyph(u8g2, 60, 62, '/');
    u8g2_DrawGlyph(u8g2, 65, 62, 0x21B5);

    u8g2_SendBuffer(u8g2);
}

static void draw_sensor_detail(u8g2_t* u8g2, int sensor_idx) {
    u8g2_ClearBuffer(u8g2);

    u8g2_SetFont(u8g2, u8g2_font_6x12_tf);
    int w = u8g2_GetStrWidth(u8g2, sensor_options[sensor_idx]);
    u8g2_DrawStr(u8g2, (128 - w)/2, 15, sensor_options[sensor_idx]);
    u8g2_DrawHLine(u8g2, 0, 19, 128);

    // Lock config to read safely
    tpms_config_lock();
    char short_name[16];
    strncpy(short_name, tpms_config_get()->short_name, sizeof(short_name));
    short_name[sizeof(short_name)-1] = '\0';
    
    const char* address;
    switch (sensor_idx) {
        case 0: address = tpms_config_get()->addressB_FL; break;
        case 1: address = tpms_config_get()->addressB_FR; break;
        case 2: address = tpms_config_get()->addressB_RL; break;
        case 3: address = tpms_config_get()->addressB_RR; break;
        default: address = "Unknown"; break;
    }
    char addr_str[18];
    strncpy(addr_str, address, sizeof(addr_str));
    addr_str[sizeof(addr_str)-1] = '\0';
    tpms_config_unlock();

    u8g2_SetFont(u8g2, u8g2_font_helvB08_tf);
    char buf[32];
    snprintf(buf, sizeof(buf), "Name: %s", short_name);
    u8g2_DrawStr(u8g2, 10, 34, buf);

    snprintf(buf, sizeof(buf), "Addr: %s", addr_str);
    u8g2_DrawStr(u8g2, 10, 49, buf);

    u8g2_SetFont(u8g2, u8g2_font_6x12_t_symbols);
    u8g2_DrawGlyph(u8g2, 55, 62, 0x2713);
    u8g2_DrawGlyph(u8g2, 60, 62, '/');
    u8g2_DrawGlyph(u8g2, 65, 62, 0x21B5);

    u8g2_SendBuffer(u8g2);
}

static void draw_adjust_number(u8g2_t* u8g2, const char* title, float value, bool is_float, const char* unit) {
    u8g2_ClearBuffer(u8g2);

    u8g2_SetFont(u8g2, u8g2_font_6x12_tf);
    int w = u8g2_GetStrWidth(u8g2, title);
    u8g2_DrawStr(u8g2, (128 - w)/2, 15, title);
    u8g2_DrawHLine(u8g2, 0, 19, 128);

    char buf[16];
    if (is_float) {
        snprintf(buf, sizeof(buf), "%.1f", value);
    } else {
        snprintf(buf, sizeof(buf), "%d", (int)value);
    }
    u8g2_SetFont(u8g2, u8g2_font_profont29_tr);
    w = u8g2_GetStrWidth(u8g2, buf);
    int x_number = (128 - w) / 2;
    u8g2_DrawStr(u8g2, x_number, 45, buf);

    u8g2_SetFont(u8g2, u8g2_font_helvB08_tf);
    if (strcmp(unit, "°C") == 0) {
        u8g2_SetFont(u8g2, u8g2_font_helvR12_tf);
        u8g2_DrawGlyph(u8g2, x_number + w + 2, 45, 0x00B0);
        u8g2_SetFont(u8g2, u8g2_font_helvB08_tf);
        u8g2_DrawStr(u8g2, x_number + w + 10, 45, "C");
    } else {
        u8g2_DrawStr(u8g2, x_number + w + 2, 45, unit);
    }

    u8g2_SetFont(u8g2, u8g2_font_unifont_t_75);
    u8g2_DrawGlyph(u8g2, 3,   64, 0x25B2);
    u8g2_DrawGlyph(u8g2, 118, 64, 0x25BC);
    u8g2_SetFont(u8g2, u8g2_font_6x12_t_symbols);
    u8g2_DrawGlyph(u8g2, 55, 62, 0x2713);
    u8g2_DrawGlyph(u8g2, 60, 62, '/');
    u8g2_DrawGlyph(u8g2, 65, 62, 0x21B5);

    u8g2_SendBuffer(u8g2);
}

static void draw_adjust_screen(u8g2_t* u8g2, int adjust) {
    const char* title = NULL;
    float value = 0.0f;
    bool is_float = false;
    const char* unit = NULL;

    // Lock config to read safely
    tpms_config_lock();
    switch (adjust) {
        case 1: // ADJ_FRONT_UPPER
            title = "Front Upper Limit";
            value = tpms_config_get()->front_tire_press_Upper_limit;
            is_float = true;
            unit = tpms_config_get()->tire_pressure_unit == PSI_UNIT ? "PSI" : "BAR";
            break;
        case 2: // ADJ_FRONT_LOWER
            title = "Front Lower Limit";
            value = tpms_config_get()->front_tire_press_Lower_limit;
            is_float = true;
            unit = tpms_config_get()->tire_pressure_unit == PSI_UNIT ? "PSI" : "BAR";
            break;
        case 3: // ADJ_REAR_UPPER
            title = "Rear Upper Limit";
            value = tpms_config_get()->rear_tire_press_Upper_limit;
            is_float = true;
            unit = tpms_config_get()->tire_pressure_unit == PSI_UNIT ? "PSI" : "BAR";
            break;
        case 4: // ADJ_REAR_LOWER
            title = "Rear Lower Limit";
            value = tpms_config_get()->rear_tire_press_Lower_limit;
            is_float = true;
            unit = tpms_config_get()->tire_pressure_unit == PSI_UNIT ? "PSI" : "BAR";
            break;
        case 5: // ADJ_HIGH_TEMP
            title = "High temp warning";
            value = (float)tpms_config_get()->high_temp_warning;
            is_float = false;
            unit = "°C";
            break;
        default:
            tpms_config_unlock();
            return;
    }
    tpms_config_unlock();

    draw_adjust_number(u8g2, title, value, is_float, unit);
}

static void draw_item_detail(u8g2_t* u8g2, int sel, int sub) {
    const int idx = (sel % MENU_COUNT + MENU_COUNT) % MENU_COUNT;

    // Lock config to read safely
    tpms_config_lock();
    
    if (idx == 0) {
        uint8_t warm_up = tpms_config_get()->warm_up_greetings;
        tpms_config_unlock();
        draw_two_option_screen(u8g2,
                               "Warm-up greetings",
                               "Turn ON voice",
                               "Turn OFF voice",
                               (sub ? 1 : 0),
                               (warm_up ? 0 : 1));
        return;
    }
    if (idx == 1) {
        voice_gender_t warning = tpms_config_get()->warning_settings;
        tpms_config_unlock();
        draw_two_option_screen(u8g2,
                               "Warning settings",
                               "Voice: Male",
                               "Voice: Female",
                               (sub ? 1 : 0),
                               (warning == VOICE_MALE ? 0 : 1));
        return;
    }
    if (idx == 2) {
        float front_upper = tpms_config_get()->front_tire_press_Upper_limit;
        float front_lower = tpms_config_get()->front_tire_press_Lower_limit;
        unit_pressure_t unit = tpms_config_get()->tire_pressure_unit;
        tpms_config_unlock();
        char opt0[32];
        snprintf(opt0, sizeof(opt0), "Upper limit: %.1f %s",
                 front_upper, unit == PSI_UNIT ? "PSI" : "BAR");
        char opt1[32];
        snprintf(opt1, sizeof(opt1), "Lower limit: %.1f %s",
                 front_lower, unit == PSI_UNIT ? "PSI" : "BAR");
        draw_two_option_screen(u8g2, "Front tire pressure", opt0, opt1, sub, -1);
        return;
    }
    if (idx == 3) {
        float rear_upper = tpms_config_get()->rear_tire_press_Upper_limit;
        float rear_lower = tpms_config_get()->rear_tire_press_Lower_limit;
        unit_pressure_t unit = tpms_config_get()->tire_pressure_unit;
        tpms_config_unlock();
        char opt0[32];
        snprintf(opt0, sizeof(opt0), "Upper limit: %.1f %s",
                 rear_upper, unit == PSI_UNIT ? "PSI" : "BAR");
        char opt1[32];
        snprintf(opt1, sizeof(opt1), "Lower limit: %.1f %s",
                 rear_lower, unit == PSI_UNIT ? "PSI" : "BAR");
        draw_two_option_screen(u8g2, "Rear tire pressure", opt0, opt1, sub, -1);
        return;
    }
    if (idx == 5) {
        TireSwapMode swap = tpms_config_get()->tire_swap;
        tpms_config_unlock();
        draw_tire_swap_menu(u8g2, sub, (int)swap);
        return;
    }
    if (idx == 6) {
        tpms_config_unlock();
        draw_sensor_menu(u8g2, sub);
        return;
    }
    if (idx == 7) {
        unit_pressure_t unit = tpms_config_get()->tire_pressure_unit;
        tpms_config_unlock();
        draw_two_option_screen(u8g2,
                               "Tire pressure unit",
                               "Unit: PSI",
                               "Unit: BAR",
                               (sub ? 1 : 0),
                               (unit == PSI_UNIT ? 0 : 1));
        return;
    }
    if (idx == 8) {
        tpms_config_unlock();
        draw_two_option_screen(u8g2,
                               "Restore settings",
                               restore_options[0],
                               restore_options[1],
                               (sub ? 1 : 0),
                               -1);
        return;
    }
    
    tpms_config_unlock();

    u8g2_ClearBuffer(u8g2);
    u8g2_SetFont(u8g2, u8g2_font_6x12_tf);
    u8g2_DrawStr(u8g2, 2, 11, "Menu >");
    u8g2_SetFont(u8g2, u8g2_font_helvB10_tf);
    const char* title = menu_items[idx];
    int w = u8g2_GetStrWidth(u8g2, title);
    u8g2_DrawStr(u8g2, (128 - w)/2, 24, title);
    u8g2_DrawHLine(u8g2, 0, 28, 128);
    u8g2_SetFont(u8g2, u8g2_font_6x12_tf);
    u8g2_DrawStr(u8g2, 2, 42, "Settings screen...");
    u8g2_DrawStr(u8g2, 2, 56, "Press 7 to return.");
    u8g2_SendBuffer(u8g2);
}

static void ui_task(void* pv) {
    // Store task handle for task notifications
    s_ui_task_handle = xTaskGetCurrentTaskHandle();
    
    u8g2_esp32_hal_t hal = U8G2_ESP32_HAL_DEFAULT;
    hal.bus.i2c.sda = PIN_SDA;
    hal.bus.i2c.scl = PIN_SCL;
    u8g2_esp32_hal_init(hal);

    u8g2_t u8g2;
    u8g2_Setup_sh1106_i2c_128x64_noname_f(
        &u8g2, U8G2_R0, u8g2_esp32_i2c_byte_cb, u8g2_esp32_gpio_and_delay_cb);

    u8x8_SetI2CAddress(&u8g2.u8x8, OLED_ADDR_7BIT << 1);
    u8g2_InitDisplay(&u8g2);
    u8g2_SetPowerSave(&u8g2, 0);
    u8g2_SetFontMode(&u8g2, 1);
    u8g2_SetBitmapMode(&u8g2, 1);

    tpms_config_lock();
    uint8_t mirrored = tpms_config_get()->display_mirrored;
    tpms_config_unlock();
    
    if (mirrored) u8g2_SetDisplayRotation(&u8g2, U8G2_MIRROR);
    else          u8g2_SetDisplayRotation(&u8g2, U8G2_R0);


    ESP_LOGI(TAG, "UI ready");
    
    // Signal that screen is ready
    if (s_event_group != NULL) {
        xEventGroupSetBits(s_event_group, (1 << 3)); // BIT_SCREEN_READY
        ESP_LOGI(TAG, "Screen ready signal sent");
    }

    ui_mode_t mode = MODE_HELLO;
    int sel = 0;
    int sub = 0;
    int adjust = 0;
    draw_hello_tpms(&u8g2);

    for (;;) {
    ui_update_t upd;

    // 1) Chờ update hoặc timeout để chớp HELLO
    if (xQueueReceive(s_ui_queue, &upd, pdMS_TO_TICKS(BLINK_INTERVAL_MS))) {

        // --- ƯU TIÊN: xử lý toggle mirror nếu có ---
        if (upd.toggle_mirror) {
            // Đổi trạng thái, áp dụng rotation cho U8G2
            tpms_config_lock();
            tpms_config_get()->display_mirrored ^= 1;
            uint8_t mirrored = tpms_config_get()->display_mirrored;
            tpms_config_unlock();
            
            if (mirrored) {
                u8g2_SetDisplayRotation(&u8g2, U8G2_MIRROR);
            } else {
                u8g2_SetDisplayRotation(&u8g2, U8G2_R0);
            }
            // Lưu NVS (khuyến nghị dùng deferred save)
            tpms_config_schedule_save(800);

            // (tuỳ chọn) reset trạng thái blink để tránh nháy nửa chừng
            blink_visible = true;
            last_blink_toggle_us = esp_timer_get_time();

            // VẼ LẠI MÀN HIỆN TẠI rồi continue (không đổi mode/sel/sub)
            if (mode == MODE_MENU) {
                draw_menu_3line(&u8g2, sel);
            } else if (mode == MODE_ITEM) {
                draw_item_detail(&u8g2, sel, sub);
            } else if (mode == MODE_ADJUST) {
                draw_adjust_screen(&u8g2, adjust);
            } else if (mode == MODE_SENSOR) {
                draw_sensor_detail(&u8g2, sub);
            } else { // MODE_HELLO
                draw_hello_tpms(&u8g2);
            }
            continue; // đã xử lý xong gói toggle
        }

        // --- CẬP NHẬT TRẠNG THÁI UI như bình thường ---
        mode    = upd.mode;
        sel     = (upd.sel % MENU_COUNT + MENU_COUNT) % MENU_COUNT;
        sub     = upd.sub;
        adjust  = upd.adjust;

        // Mỗi khi có update, reset blink để HELLO vẽ “đầy đủ” rồi mới chớp tiếp
        blink_visible = true;
        last_blink_toggle_us = esp_timer_get_time();

    } else if (mode == MODE_HELLO) {
        // Timeout: tick chớp HELLO
        int64_t now_us = esp_timer_get_time();
        if (now_us - last_blink_toggle_us >= (int64_t)BLINK_INTERVAL_MS * 1000) {
            blink_visible = !blink_visible;
            last_blink_toggle_us = now_us;
        }
    } else {
        // Không có update và không ở HELLO => khỏi vẽ lại
        continue;
    }

    // 2) VẼ MÀN TƯƠNG ỨNG
    if (mode == MODE_MENU) {
        draw_menu_3line(&u8g2, sel);
    } else if (mode == MODE_ITEM) {
        draw_item_detail(&u8g2, sel, sub);
    } else if (mode == MODE_ADJUST) {
        draw_adjust_screen(&u8g2, adjust);
    } else if (mode == MODE_SENSOR) {
        draw_sensor_detail(&u8g2, sub);
    } else { // MODE_HELLO
        draw_hello_tpms(&u8g2);
    }
}
}



void screen_init(void) {
    // Nothing to initialize here
}

void screen_task_start(QueueHandle_t ui_queue, EventGroupHandle_t event_group) {
    s_ui_queue = ui_queue;
    s_event_group = event_group;
    xTaskCreate(ui_task, "ui_task", 4096, NULL, 4, NULL);
}

QueueHandle_t screen_get_ui_queue(void) {
    return s_ui_queue;
}

TaskHandle_t screen_get_task_handle(void) {
    return s_ui_task_handle;
}

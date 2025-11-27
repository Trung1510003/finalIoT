#include "tpms_config.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include <string.h>
#include <esp_timer.h>

static const char *TAG = "TPMS_CONFIG";

static TPMS_Config g_tpms_config = {
    .warm_up_greetings = 1,
    .warning_settings = VOICE_FEMALE,
    .front_tire_press_Upper_limit = 19.0,
    .front_tire_press_Lower_limit = 17.0,
    .rear_tire_press_Upper_limit = 19.0,
    .rear_tire_press_Lower_limit = 17.0,
    .high_temp_warning = 30,
    .tire_swap = TIRE_SWAP_INITIAL,
    .short_name = "AI-8000",
    .addressB_FL = "12:30:af:00:01:59",
    .addressB_FR = "12:30:af:00:01:46",
    .addressB_RL = "12:30:af:00:01:5e",
    .addressB_RR = "12:30:af:00:00:f0",
    .tire_pressure_unit = PSI_UNIT,
    .display_mirrored = 0,
    .version = 1
};

// Global device array (will be updated based on swap mode)
static ai_device_t g_ai_devices[4];

// Initialize device names (fixed order: TT, TP, ST, SP)
static const char* const device_names[] = {"TT", "TP", "ST", "SP"};

// Deferred save variables
static volatile bool cfg_dirty = false;
static esp_timer_handle_t s_save_timer = NULL;

static void save_timer_cb(void* arg) {
    if (cfg_dirty) {
        nvs_handle_t nvh;
        esp_err_t err = nvs_open("tpms_storage", NVS_READWRITE, &nvh);
        if (err == ESP_OK) {
            err = nvs_set_blob(nvh, "tpms_config", &g_tpms_config, sizeof(TPMS_Config));
            if (err == ESP_OK) {
                err = nvs_commit(nvh);
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "NVS committed.");
                } else {
                    ESP_LOGE(TAG, "NVS commit error: %s", esp_err_to_name(err));
                }
            } else {
                ESP_LOGE(TAG, "NVS set_blob error: %s", esp_err_to_name(err));
            }
            nvs_close(nvh);
        } else {
            ESP_LOGE(TAG, "NVS open error: %s", esp_err_to_name(err));
        }
        cfg_dirty = false;
    }
}

TPMS_Config* tpms_config_get(void) {
    return &g_tpms_config;
}

ai_device_t* tpms_config_get_devices(void) {
    return g_ai_devices;
}

void tpms_config_init(void) {
    const esp_timer_create_args_t save_tmr_args = {
        .callback = &save_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "cfg_save"
    };
    ESP_ERROR_CHECK(esp_timer_create(&save_tmr_args, &s_save_timer));
}

void tpms_config_load(void) {
    nvs_handle_t handle;
    esp_err_t err;

    err = nvs_open("tpms_storage", NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed: %s, using defaults", esp_err_to_name(err));
        return;
    }

    size_t size = sizeof(TPMS_Config);
    err = nvs_get_blob(handle, "tpms_config", &g_tpms_config, &size);
    if (err != ESP_OK || size != sizeof(TPMS_Config)) {
        ESP_LOGW(TAG, "NVS read failed or invalid size: %s, using defaults", esp_err_to_name(err));
        nvs_close(handle);
        return;
    }

    if (g_tpms_config.version != 1) {
        ESP_LOGW(TAG, "Invalid config version (%d), using defaults", g_tpms_config.version);
        nvs_close(handle);
        return;
    }

    ESP_LOGI(TAG, "Configuration loaded from NVS");
    nvs_close(handle);
}

void tpms_config_save(void) {
    nvs_handle_t handle;
    esp_err_t err;

    err = nvs_open("tpms_storage", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_blob(handle, "tpms_config", &g_tpms_config, sizeof(TPMS_Config));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save config to NVS: %s", esp_err_to_name(err));
    } else {
        err = nvs_commit(handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "Configuration saved to NVS");
        }
    }

    nvs_close(handle);
}

void tpms_config_schedule_save(uint64_t delay_ms) {
    cfg_dirty = true;
    if (s_save_timer) {
        esp_timer_stop(s_save_timer);
        esp_timer_start_once(s_save_timer, delay_ms * 1000ULL);
    }
}

void tpms_config_update_devices_by_swap_mode(TireSwapMode mode) {
    const char* addresses[4];

    switch(mode) {
        case TIRE_SWAP_INITIAL:
            addresses[0] = g_tpms_config.addressB_FL;
            addresses[1] = g_tpms_config.addressB_FR;
            addresses[2] = g_tpms_config.addressB_RL;
            addresses[3] = g_tpms_config.addressB_RR;
            break;
        case TIRE_SWAP_VERTICAL:
            addresses[0] = g_tpms_config.addressB_RL;
            addresses[1] = g_tpms_config.addressB_RR;
            addresses[2] = g_tpms_config.addressB_FL;
            addresses[3] = g_tpms_config.addressB_FR;
            break;
        case TIRE_SWAP_CROSS:
            addresses[0] = g_tpms_config.addressB_RR;
            addresses[1] = g_tpms_config.addressB_RL;
            addresses[2] = g_tpms_config.addressB_FR;
            addresses[3] = g_tpms_config.addressB_FL;
            break;
        case TIRE_SWAP_HORIZONTAL:
            addresses[0] = g_tpms_config.addressB_FR;
            addresses[1] = g_tpms_config.addressB_FL;
            addresses[2] = g_tpms_config.addressB_RR;
            addresses[3] = g_tpms_config.addressB_RL;
            break;
    }

    for(int i = 0; i < 4; i++) {
        strncpy(g_ai_devices[i].address, addresses[i], sizeof(g_ai_devices[i].address));
        strncpy(g_ai_devices[i].name, device_names[i], sizeof(g_ai_devices[i].name));
        g_ai_devices[i].address[sizeof(g_ai_devices[i].address)-1] = '\0';
        g_ai_devices[i].name[sizeof(g_ai_devices[i].name)-1] = '\0';
    }
}

void tpms_config_restore_defaults(void) {
    g_tpms_config.warm_up_greetings = 1;
    g_tpms_config.warning_settings = VOICE_FEMALE;
    g_tpms_config.front_tire_press_Upper_limit = 19.0;
    g_tpms_config.front_tire_press_Lower_limit = 17.0;
    g_tpms_config.rear_tire_press_Upper_limit = 19.0;
    g_tpms_config.rear_tire_press_Lower_limit = 17.0;
    g_tpms_config.high_temp_warning = 30;
    g_tpms_config.tire_swap = TIRE_SWAP_INITIAL;
    strcpy(g_tpms_config.short_name, "AI-8000");
    strcpy(g_tpms_config.addressB_FL, "12:30:af:00:01:59");
    strcpy(g_tpms_config.addressB_FR, "12:30:af:00:01:46");
    strcpy(g_tpms_config.addressB_RL, "12:30:af:00:01:5e");
    strcpy(g_tpms_config.addressB_RR, "12:30:af:00:00:f0");
    g_tpms_config.tire_pressure_unit = PSI_UNIT;
    g_tpms_config.display_mirrored = 0;
    g_tpms_config.version = 1;

    tpms_config_save();
    tpms_config_update_devices_by_swap_mode(g_tpms_config.tire_swap);
    ESP_LOGI(TAG, "Settings restored to default and saved to NVS");
}


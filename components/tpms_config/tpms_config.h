#ifndef TPMS_CONFIG_H
#define TPMS_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { 
    VOICE_MALE = 0, 
    VOICE_FEMALE = 1 
} voice_gender_t;

typedef enum { 
    PSI_UNIT = 0, 
    BAR_UNIT = 1 
} unit_pressure_t;

typedef enum { 
    TIRE_SWAP_INITIAL = 0, 
    TIRE_SWAP_VERTICAL, 
    TIRE_SWAP_CROSS, 
    TIRE_SWAP_HORIZONTAL 
} TireSwapMode;

#define TPMS_PSI_TO_BAR 0.0689476f
#define TPMS_BAR_TO_PSI (1.0f / TPMS_PSI_TO_BAR)

typedef struct {
    uint8_t  warm_up_greetings;
    voice_gender_t warning_settings;
    float    front_tire_press_Upper_limit;
    float    front_tire_press_Lower_limit;
    float    rear_tire_press_Upper_limit;
    float    rear_tire_press_Lower_limit;
    uint8_t  high_temp_warning;
    TireSwapMode tire_swap;
    char     short_name[16];
    char     addressB_FL[18];   // Front Left
    char     addressB_FR[18];   // Front Right
    char     addressB_RL[18];   // Rear Left
    char     addressB_RR[18];   // Rear Right
    unit_pressure_t tire_pressure_unit;
    uint8_t  display_mirrored;
    uint8_t  version;
} TPMS_Config;

// Structure for AI-8000 devices
typedef struct {
    char address[18];
    char name[4]; // "TT", "TP", "ST", "SP"
} ai_device_t;

// Get global config instance
TPMS_Config* tpms_config_get(void);

// Get global device array
ai_device_t* tpms_config_get_devices(void);

// Initialize configuration with defaults
void tpms_config_init(void);

// Load configuration from NVS
void tpms_config_load(void);

// Save configuration to NVS
void tpms_config_save(void);

// Schedule save (deferred)
void tpms_config_schedule_save(uint64_t delay_ms);

// Update devices based on tire swap mode
void tpms_config_update_devices_by_swap_mode(TireSwapMode mode);

// Restore default settings
void tpms_config_restore_defaults(void);

// Lock/unlock config for thread-safe access
// Use these when reading/writing config from multiple tasks
void tpms_config_lock(void);
void tpms_config_unlock(void);

static inline float tpms_convert_psi_to_unit(float psi, unit_pressure_t unit) {
    return (unit == PSI_UNIT) ? psi : (psi * TPMS_PSI_TO_BAR);
}

static inline float tpms_convert_unit_to_psi(float value, unit_pressure_t unit) {
    return (unit == PSI_UNIT) ? value : (value * TPMS_BAR_TO_PSI);
}

#ifdef __cplusplus
}
#endif

#endif // TPMS_CONFIG_H


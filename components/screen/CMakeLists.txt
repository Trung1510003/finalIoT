idf_component_register(SRCS "screen.c"
                    INCLUDE_DIRS "."
                    REQUIRES tpms_config ble_scanner
                    PRIV_REQUIRES u8g2 u8g2-hal-esp-idf
                    REQUIRES esp_timer
                    REQUIRES u8g2 u8g2-hal-esp-idf
)


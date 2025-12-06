idf_component_register(SRCS "ble_scanner.c" "bt_hci_common.c"
                    INCLUDE_DIRS "."
                    REQUIRES tpms_config speaker esp_timer
                    PRIV_REQUIRES bt)
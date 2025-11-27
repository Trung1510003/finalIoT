#ifndef BT_HCI_COMMON_H
#define BT_HCI_COMMON_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// HCI Event codes
#define HCI_LE_ADV_REPORT 0x02

// HCI Command opcodes
#define HCI_OPCODE_RESET 0x0C03
#define HCI_OPCODE_SET_EVT_MASK 0x0C01
#define HCI_OPCODE_BLE_SET_SCAN_PARAMS 0x200B
#define HCI_OPCODE_BLE_SET_SCAN_ENABLE 0x200C

// Build HCI command packet
uint16_t make_cmd_reset(uint8_t *buf);
uint16_t make_cmd_set_evt_mask(uint8_t *buf, const uint8_t *evt_mask);
uint16_t make_cmd_ble_set_scan_params(uint8_t *buf, uint8_t scan_type, uint16_t scan_interval, uint16_t scan_window, uint8_t own_addr_type, uint8_t filter_policy);
uint16_t make_cmd_ble_set_scan_enable(uint8_t *buf, uint8_t enable, uint8_t filter_duplicates);

#ifdef __cplusplus
}
#endif

#endif // BT_HCI_COMMON_H


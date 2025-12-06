#include "bt_hci_common.h"
#include <string.h>

// HCI packet structure: [0] = packet type, [1-2] = opcode (little endian), [3] = parameter length, [4+] = parameters
#define HCI_CMD_PKT 0x01

static void put_u16_le(uint8_t *buf, uint16_t val) {
    buf[0] = val & 0xFF;
    buf[1] = (val >> 8) & 0xFF;
}

uint16_t make_cmd_reset(uint8_t *buf) {
    buf[0] = HCI_CMD_PKT;
    put_u16_le(&buf[1], HCI_OPCODE_RESET);
    buf[3] = 0; // No parameters
    return 4;
}

uint16_t make_cmd_set_evt_mask(uint8_t *buf, const uint8_t *evt_mask) {
    buf[0] = HCI_CMD_PKT;
    put_u16_le(&buf[1], HCI_OPCODE_SET_EVT_MASK);
    buf[3] = 8; // 8 bytes
    memcpy(&buf[4], evt_mask, 8);
    return 12;
}

uint16_t make_cmd_ble_set_scan_params(uint8_t *buf, uint8_t scan_type, uint16_t scan_interval, uint16_t scan_window, uint8_t own_addr_type, uint8_t filter_policy) {
    buf[0] = HCI_CMD_PKT;
    put_u16_le(&buf[1], HCI_OPCODE_BLE_SET_SCAN_PARAMS);
    buf[3] = 7; // 7 bytes
    buf[4] = scan_type;
    put_u16_le(&buf[5], scan_interval);
    put_u16_le(&buf[7], scan_window);
    buf[9] = own_addr_type;
    buf[10] = filter_policy;
    return 11;
}

uint16_t make_cmd_ble_set_scan_enable(uint8_t *buf, uint8_t enable, uint8_t filter_duplicates) {
    buf[0] = HCI_CMD_PKT;
    put_u16_le(&buf[1], HCI_OPCODE_BLE_SET_SCAN_ENABLE);
    buf[3] = 2; // 2 bytes
    buf[4] = enable;
    buf[5] = filter_duplicates;
    return 6;
}
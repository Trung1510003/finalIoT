# SensorBLE_OLED - TPMS Sensor Monitor System

Hệ thống giám sát cảm biến áp suất lốp (TPMS) sử dụng ESP32, hiển thị thông tin trên màn hình OLED và phát cảnh báo qua loa DFPlayer.

## Tổng quan

Project này là một hệ thống giám sát TPMS (Tire Pressure Monitoring System) hoàn chỉnh với các tính năng:

- **BLE Scanner**: Quét và nhận dữ liệu từ 4 cảm biến TPMS qua Bluetooth Low Energy
- **OLED Display**: Hiển thị áp suất, nhiệt độ, pin của 4 lốp trên màn hình SH1106 128x64
- **Audio Alerts**: Phát cảnh báo bằng giọng nói khi áp suất/nhiệt độ vượt ngưỡng
- **Configuration Management**: Lưu trữ cấu hình vào NVS (Non-Volatile Storage)
- **User Interface**: Menu điều hướng với 3 nút bấm (UP, MODE, DOWN)

## Components

### 1. `tpms_config` - Configuration Management

**Vị trí**: `components/tpms_config/`

**Chức năng**:
- Quản lý cấu hình TPMS (ngưỡng áp suất, nhiệt độ, đơn vị, v.v.)
- Lưu/đọc cấu hình từ NVS (Non-Volatile Storage)
- Mapping thiết bị theo chế độ đổi lốp (Initial, Vertical, Cross, Horizontal)
- Deferred save với ESP-IDF timer để tránh ghi flash quá thường xuyên

**API chính**:
```c
void tpms_config_init(void);
void tpms_config_load(void);
void tpms_config_save(void);
void tpms_config_schedule_save(uint64_t delay_ms);
TPMS_Config* tpms_config_get(void);
void tpms_config_lock(void);
void tpms_config_unlock(void);
```

**Cấu hình lưu trữ**:
- Ngưỡng áp suất lốp trước/sau (Upper/Lower limit)
- Ngưỡng cảnh báo nhiệt độ cao
- Đơn vị áp suất (PSI/BAR)
- Địa chỉ BLE của 4 cảm biến
- Chế độ đổi lốp
- Cài đặt giọng nói (Nam/Nữ)
- Bật/tắt lời chào khởi động
- Đảo chiều màn hình

**Thread Safety**: Sử dụng mutex (`config_mutex`, `nvs_mutex`) để bảo vệ shared data.

---

### 2. `ble_scanner` - BLE Sensor Scanner

**Vị trí**: `components/ble_scanner/`

**Chức năng**:
- Khởi tạo và quét BLE để tìm cảm biến TPMS
- Parse packet BLE và trích xuất dữ liệu cảm biến
- Cập nhật biến global cho 4 lốp (Front Left, Front Right, Rear Left, Rear Right)
- Gửi cảnh báo vào speaker queue khi áp suất/nhiệt độ vượt ngưỡng

**API chính**:
```c
void ble_scanner_init(void);
void ble_scanner_start(QueueHandle_t speaker_queue, EventGroupHandle_t event_group);
void ble_scanner_lock_sensor_data(void);
void ble_scanner_unlock_sensor_data(void);
```

**Dữ liệu cảm biến** (global variables):
- `front_left_pressure_psi`, `front_left_temperature`, `front_left_voltage`
- `front_right_pressure_psi`, `front_right_temperature`, `front_right_voltage`
- `rear_left_pressure_psi`, `rear_left_temperature`, `rear_left_voltage`
- `rear_right_pressure_psi`, `rear_right_temperature`, `rear_right_voltage`

**Thread Safety**: Sử dụng `sensor_data_mutex` để bảo vệ khi đọc dữ liệu từ các task khác.

---

### 3. `speaker` - Audio Alert System

**Vị trí**: `components/speaker/`

**Chức năng**:
- Khởi tạo DFPlayer Mini để phát audio qua UART (GPIO 2/3)
- Xử lý queue cảnh báo từ BLE scanner
- Phát cảnh báo bằng giọng nói khi áp suất/nhiệt độ vượt ngưỡng
- Hỗ trợ chọn giọng nam/nữ
- Bật/tắt lời chào khởi động
- Priority queue để phát cảnh báo theo thứ tự ưu tiên (TT → TP → ST → SP)

**API chính**:
```c
void speaker_init(void);
void speaker_task_start(QueueHandle_t queue, EventGroupHandle_t event_group);
void speaker_play_pressure_alert(const sensor_data_t *sensor_data);
void speaker_notify_sensor_detected(const char* device_name);
void speaker_update_voice_setting(uint8_t voice);
void speaker_set_voice_enabled(bool enabled);
```

**Cấu hình GPIO**:
- `TXD_PIN`: GPIO_NUM_2 (UART TX → DFPlayer RX)
- `RXD_PIN`: GPIO_NUM_3 (UART RX → DFPlayer TX)
- `UART_PORT_NUM`: UART_NUM_1
- `Baud rate`: 9600

**Cấu trúc dữ liệu**:
```c
typedef struct {
    uint8_t temperature;
    float battery_level;
    uint16_t pressure;
    char device_name[4];  // "TT", "TP", "ST", "SP"
    char address[18];
} sensor_data_t;
```

**Thread Safety**: Sử dụng queue để giao tiếp giữa BLE scanner task và speaker task.

---

### 4. `screen` - OLED Display & UI

**Vị trí**: `components/screen/`

**Chức năng**:
- Khởi tạo màn hình OLED SH1106 128x64 qua I2C
- Hiển thị màn hình chính (HELLO) với thông tin 4 lốp
- Quản lý menu cài đặt (9 mục)
- Xử lý blink cho giá trị vượt ngưỡng
- Hỗ trợ đảo chiều màn hình (mirror mode)

**API chính**:
```c
void screen_init(void);
void screen_task_start(QueueHandle_t ui_queue, EventGroupHandle_t event_group);
QueueHandle_t screen_get_ui_queue(void);
TaskHandle_t screen_get_task_handle(void);
```

**Cấu hình GPIO**:
- `PIN_SDA`: GPIO_NUM_5 (I2C Data)
- `PIN_SCL`: GPIO_NUM_4 (I2C Clock)
- `OLED_ADDR_7BIT`: 0x3C

**UI Modes**:
- `MODE_HELLO`: Màn hình chính hiển thị 4 lốp
- `MODE_MENU`: Menu cài đặt (3 dòng)
- `MODE_ITEM`: Chi tiết mục menu
- `MODE_ADJUST`: Chỉnh giá trị số
- `MODE_SENSOR`: Chọn và xem thông tin cảm biến

**Cấu trúc UI Update**:
```c
typedef struct {
    ui_mode_t mode;
    int sel;      // Menu selection index
    int sub;      // Sub-menu selection
    int adjust;   // Adjustment type
    uint8_t toggle_mirror;
} ui_update_t;
```

**Thread Safety**: Sử dụng lock (`ble_scanner_lock_sensor_data()`, `tpms_config_lock()`) khi đọc dữ liệu từ các component khác.

---

### 5. `button` - Button Input Handler

**Vị trí**: `components/button/`

**Chức năng**:
- Khởi tạo 3 nút bấm (UP, MODE, DOWN)
- Xử lý debounce, repeat, long press
- Gửi UI update vào queue khi có sự kiện nút bấm

**API chính**:
```c
void button_init(void);
void button_task_start(QueueHandle_t ui_queue, EventGroupHandle_t event_group);
```

**Cấu hình GPIO**:
- `PIN_BTN_UP`: GPIO_NUM_6
- `PIN_BTN_MODE`: GPIO_NUM_7
- `PIN_BTN_DOWN`: GPIO_NUM_9
- `BTN_ACTIVE_LEVEL`: 0 (active low)

**Tính năng**:
- Debounce: 30ms
- Hold threshold: 450ms
- Repeat: Bắt đầu sau 120ms, tăng tốc sau 20ms, tối thiểu 60ms
- Long press MODE: 2000ms (quay về màn hình HELLO)
- Long press DOWN: 2000ms (toggle mirror mode)

**FreeRTOS Features**:
- FreeRTOS Software Timer: Poll buttons mỗi 10ms thay vì dùng vTaskDelay

---
# SensorBLE_OLED - TPMS Sensor Monitor System

Hệ thống giám sát cảm biến áp suất lốp (TPMS) sử dụng ESP32, hiển thị thông tin trên màn hình OLED và phát cảnh báo qua loa DFPlayer.

## 📋 Tổng quan

Project này là một hệ thống giám sát TPMS (Tire Pressure Monitoring System) hoàn chỉnh với các tính năng:

- **BLE Scanner**: Quét và nhận dữ liệu từ 4 cảm biến TPMS qua Bluetooth Low Energy
- **OLED Display**: Hiển thị áp suất, nhiệt độ, pin của 4 lốp trên màn hình SH1106 128x64
- **Audio Alerts**: Phát cảnh báo bằng giọng nói khi áp suất/nhiệt độ vượt ngưỡng
- **Configuration Management**: Lưu trữ cấu hình vào NVS (Non-Volatile Storage)
- **User Interface**: Menu điều hướng với 3 nút bấm (UP, MODE, DOWN)

## 🏗️ Kiến trúc hệ thống

```
┌─────────────────────────────────────────────────────────────┐
│                        app_main()                           │
│  - Initialize NVS                                          │
│  - Create Event Group (system synchronization)             │
│  - Initialize all components                               │
│  - Wait for all components ready                           │
└─────────────────────────────────────────────────────────────┘
                            │
        ┌───────────────────┼───────────────────┐
        │                   │                   │
        ▼                   ▼                   ▼
┌──────────────┐    ┌──────────────┐    ┌──────────────┐
│  BLE Scanner │    │   Speaker    │    │    Screen    │
│    Task      │───▶│    Task      │    │    Task      │
└──────────────┘    └──────────────┘    └──────────────┘
        │                   │                   │
        │                   │                   │
        ▼                   ▼                   ▼
┌──────────────┐    ┌──────────────┐    ┌──────────────┐
│  Sensor Data │    │ Speaker Queue│    │   UI Queue   │
│  (globals)   │    │  (alerts)    │    │  (updates)   │
└──────────────┘    └──────────────┘    └──────────────┘
        │                                       ▲
        │                                       │
        └───────────────────────────────────────┘
                    Button Task
```

## 📦 Components

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
- Signal binary semaphore khi phát hiện thiết bị mới

**API chính**:
```c
void ble_scanner_init(void);
void ble_scanner_start(QueueHandle_t speaker_queue, EventGroupHandle_t event_group);
void ble_scanner_lock_sensor_data(void);
void ble_scanner_unlock_sensor_data(void);
SemaphoreHandle_t ble_scanner_get_device_detected_sem(void);
```

**Dữ liệu cảm biến** (global variables):
- `front_left_pressure_psi`, `front_left_temperature`, `front_left_voltage`
- `front_right_pressure_psi`, `front_right_temperature`, `front_right_voltage`
- `rear_left_pressure_psi`, `rear_left_temperature`, `rear_left_voltage`
- `rear_right_pressure_psi`, `rear_right_temperature`, `rear_right_voltage`

**Thread Safety**: Sử dụng `sensor_data_mutex` để bảo vệ khi đọc dữ liệu từ các task khác.

**FreeRTOS Features**:
- FreeRTOS Software Timer: Kiểm tra trạng thái BLE scan định kỳ (30 giây)
- Binary Semaphore: Signal khi phát hiện thiết bị mới

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

**FreeRTOS Features**:
- Task Notification: Nhận notification từ các task khác (demo)

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
- Long press MODE: 2000ms (toggle mirror mode)

**FreeRTOS Features**:
- FreeRTOS Software Timer: Poll buttons mỗi 5ms thay vì dùng vTaskDelay

---

## 🔧 FreeRTOS/ESP-IDF Features

### ✅ Đã sử dụng

| Tính năng | Số lượng | Vị trí | Mục đích |
|-----------|----------|--------|----------|
| **ESP-IDF Timer** | 1 | `tpms_config` | Deferred save config vào NVS |
| **FreeRTOS Software Timer** | 2 | `button`, `ble_scanner` | Button polling (5ms), BLE scan check (30s) |
| **Mutex** | 3 | `tpms_config`, `ble_scanner` | Bảo vệ config, NVS operations, sensor data |
| **Binary Semaphore** | 1 | `ble_scanner` | Signal khi phát hiện BLE device mới |
| **Event Group** | 1 | `main` | Synchronize system initialization |
| **Task Notification** | 1 | `main`, `screen` | Lightweight signaling demo |
| **Queue** | 2 | `main` | UI queue (8 items), Speaker queue (1 item) |

### 📝 Chi tiết

#### 1. ESP-IDF Timer (`esp_timer`)
- **Vị trí**: `components/tpms_config/tpms_config.c`
- **Mục đích**: Deferred save configuration vào NVS
- **Cách hoạt động**: Khi config thay đổi, schedule save sau 800ms để tránh ghi flash quá thường xuyên

#### 2. FreeRTOS Software Timer
- **Button Polling Timer**: Poll buttons mỗi 5ms, chính xác hơn vTaskDelay
- **BLE Scan Check Timer**: Kiểm tra trạng thái BLE scan mỗi 30 giây

#### 3. Mutex
- **Config Mutex**: Bảo vệ `g_tpms_config` khi đọc/ghi từ nhiều tasks
- **NVS Mutex**: Serialize các thao tác NVS (read/write)
- **Sensor Data Mutex**: Bảo vệ global sensor variables

#### 4. Binary Semaphore
- **Device Detected Semaphore**: Signal khi có BLE device mới được phát hiện
- Các task khác có thể wait trên semaphore này để được thông báo

#### 5. Event Group
- **System Ready Events**: Synchronize initialization của tất cả components
- **Bits**: `BIT_CONFIG_READY`, `BIT_SPEAKER_READY`, `BIT_BLE_READY`, `BIT_SCREEN_READY`, `BIT_BUTTON_READY`
- Main task chờ tất cả bits được set trước khi tiếp tục

#### 6. Task Notification
- **Demo**: Lightweight alternative cho queue khi chỉ cần signal, không cần data
- Nhanh hơn queue cho simple notifications

---

## 🚀 Build & Flash

### Yêu cầu

- ESP-IDF v5.0 trở lên
- Python 3.8+
- CMake 3.16+

### Các bước build

```bash
# Clone repository
git clone <repository-url>
cd SensorBLE_OLED

# Setup ESP-IDF (nếu chưa setup)
. $HOME/esp/esp-idf/export.sh

# Configure project
idf.py menuconfig

# Build project
idf.py build

# Flash to ESP32
idf.py -p PORT flash

# Monitor serial output
idf.py -p PORT monitor
```

### Cấu hình

Chạy `idf.py menuconfig` để cấu hình:
- Serial port
- Flash size
- Partition table
- FreeRTOS settings

---

## 📁 Cấu trúc thư mục

```
SensorBLE_OLED/
├── main/
│   ├── main.c              # Entry point, component initialization
│   └── CMakeLists.txt
├── components/
│   ├── tpms_config/       # Configuration management
│   │   ├── tpms_config.h
│   │   ├── tpms_config.c
│   │   └── CMakeLists.txt
│   ├── ble_scanner/       # BLE sensor scanner
│   │   ├── ble_scanner.h
│   │   ├── ble_scanner.c
│   │   ├── bt_hci_common.h
│   │   ├── bt_hci_common.c
│   │   └── CMakeLists.txt
│   ├── speaker/           # Audio alert system
│   │   ├── speaker.h
│   │   ├── speaker.c
│   │   └── CMakeLists.txt
│   ├── screen/            # OLED display & UI
│   │   ├── screen.h
│   │   ├── screen.c
│   │   └── CMakeLists.txt
│   ├── button/            # Button input handler
│   │   ├── button.h
│   │   ├── button.c
│   │   └── CMakeLists.txt
│   ├── u8g2/              # U8G2 graphics library
│   └── u8g2-hal-esp-idf/  # U8G2 ESP-IDF HAL
├── CMakeLists.txt
├── sdkconfig
└── README.md
```

---

## 🔄 Luồng hoạt động

### 1. Khởi động hệ thống

```
app_main()
  ├─ Init NVS
  ├─ Create Event Group
  ├─ tpms_config_init() → Load config from NVS
  ├─ speaker_init()
  ├─ ble_scanner_init()
  ├─ screen_init()
  ├─ button_init()
  ├─ Start all tasks (parallel)
  └─ Wait for ALL_SYSTEM_READY
```

### 2. Runtime

```
BLE Scanner Task
  ├─ Scan BLE devices
  ├─ Parse sensor data
  ├─ Update global variables (with mutex)
  └─ Send alerts to speaker_queue (if abnormal)

Speaker Task
  ├─ Wait on speaker_queue
  ├─ Play audio alert based on sensor data
  └─ Use voice setting from config

Screen Task
  ├─ Wait on ui_queue (or timeout for blink)
  ├─ Lock sensor data + config
  ├─ Render HELLO/MENU/ITEM/ADJUST/SENSOR
  └─ Unlock

Button Task
  ├─ Poll buttons (via timer)
  ├─ Detect press/repeat/long press
  └─ Send ui_update_t to ui_queue
```

### 3. Configuration Save

```
User changes config
  └─ tpms_config_schedule_save(800ms)
      └─ ESP-IDF timer starts
          └─ After 800ms: save_timer_cb()
              └─ Lock mutexes
              └─ Save to NVS
              └─ Unlock mutexes
```

---

## 📊 Menu System

Hệ thống menu gồm 9 mục:

1. **Warm-up greetings** - Bật/tắt lời chào khởi động
2. **Warning settings** - Chọn giọng nam/nữ
3. **Front tire pressure** - Cài đặt ngưỡng áp suất lốp trước
4. **Rear tire pressure** - Cài đặt ngưỡng áp suất lốp sau
5. **High temp warning** - Cài đặt ngưỡng cảnh báo nhiệt độ
6. **Tire swap** - Chọn chế độ đổi lốp (Initial/Vertical/Cross/Horizontal)
7. **Connect the sensor** - Xem và cấu hình địa chỉ BLE cảm biến
8. **Unit pressure** - Chọn đơn vị PSI/BAR
9. **Restore settings** - Khôi phục cài đặt mặc định

---

## 🔒 Thread Safety

Tất cả shared data đều được bảo vệ bằng mutex:

- **Config data**: `tpms_config_lock()` / `tpms_config_unlock()`
- **Sensor data**: `ble_scanner_lock_sensor_data()` / `ble_scanner_unlock_sensor_data()`
- **NVS operations**: Serialized với `nvs_mutex`

**Lưu ý**: Luôn sử dụng lock/unlock khi đọc/ghi shared data từ nhiều tasks.

---

## 📝 Notes

- **NVS**: Cấu hình được lưu tự động vào NVS sau 800ms khi thay đổi
- **Blink**: Giá trị vượt ngưỡng sẽ nhấp nháy mỗi 500ms trên màn hình HELLO
- **Mirror Mode**: Có thể đảo chiều màn hình qua menu hoặc phím tắt
- **Tire Swap**: Hệ thống tự động map lại thiết bị khi đổi lốp

---

## 📚 Tài liệu tham khảo

- [ESP-IDF Programming Guide](https://docs.espressif.com/projects/esp-idf/en/latest/)
- [FreeRTOS Documentation](https://www.freertos.org/Documentation/RTOS_book.html)
- [U8G2 Graphics Library](https://github.com/olikraus/u8g2)

---

## 👥 Contributors

- Project structure và component refactoring
- FreeRTOS features implementation
- Thread-safe configuration management

---

**Ngày cập nhật**: 2024


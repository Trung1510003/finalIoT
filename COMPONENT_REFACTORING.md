# Component Refactoring Guide

## Đã hoàn thành

### 1. tpms_config component
- **Location**: `components/tpms_config/`
- **Files**: `tpms_config.h`, `tpms_config.c`, `CMakeLists.txt`
- **Chức năng**: 
  - Quản lý cấu hình TPMS
  - NVS save/load
  - Device mapping theo tire swap mode
  - Deferred save với timer

### 2. speaker component
- **Location**: `components/speaker/`
- **Files**: `speaker.h`, `speaker.c`, `CMakeLists.txt`
- **Chức năng**:
  - DFPlayer initialization
  - Audio alert based on pressure
  - Speaker task để xử lý queue

## Cần tạo tiếp

### 3. ble_scanner component
- **Location**: `components/ble_scanner/`
- **Chức năng**:
  - BLE scanning initialization
  - Packet parsing
  - Sensor data extraction
  - Callback để update global variables
  - Gửi data vào speaker queue khi pressure abnormal

**Các hàm cần tách**:
- `host_rcv_pkt()` - main packet handler
- `parse_sensor_data()` - parse sensor data từ BLE packet
- `get_local_name()` - extract local name từ BLE data
- `address_to_string()` - convert address
- `get_device_by_address()` - lookup device
- HCI command functions
- Global variables: `front_left_*`, `front_right_*`, `rear_left_*`, `rear_right_*`

### 4. screen component
- **Location**: `components/screen/`
- **Chức năng**:
  - OLED initialization
  - UI drawing functions
  - Menu handling
  - Blink logic

**Các hàm cần tách**:
- `ui_task()` - main UI task
- `draw_hello_tpms()` - main display
- `draw_menu_3line()` - menu display
- `draw_item_detail()` - item detail
- `draw_adjust_screen()` - adjust screen
- `draw_sensor_detail()` - sensor detail
- Tất cả các draw helper functions

### 5. button component
- **Location**: `components/button/`
- **Chức năng**:
  - Button initialization
  - Debounce logic
  - Repeat logic
  - Long press detection
  - Input task

**Các hàm cần tách**:
- `buttons_init_polling()` - init buttons
- `button_update()` - update button state
- `input_task()` - main input task
- Button structures và enums

## Cách sử dụng

1. Include các component headers trong `main.c`:
```c
#include "tpms_config.h"
#include "speaker.h"
#include "ble_scanner.h"
#include "screen.h"
#include "button.h"
```

2. Initialize trong `app_main()`:
```c
tpms_config_init();
tpms_config_load();
speaker_init();
ble_scanner_init();
screen_init();
button_init();
```

3. Start tasks:
```c
speaker_task_start(speaker_queue);
ble_scanner_start();
screen_task_start(ui_queue);
button_task_start(ui_queue);
```

## Lưu ý

- Các global variables cần được expose qua getter functions
- Queue handles cần được quản lý ở main hoặc component tương ứng
- Callbacks cần được định nghĩa rõ ràng trong headers
- Dependencies giữa components cần được quản lý cẩn thận


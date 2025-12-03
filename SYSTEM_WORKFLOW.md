# Luồng hoạt động chi tiết của hệ thống TPMS Sensor Monitor

Tài liệu này mô tả từng bước hoạt động của hệ thống, bao gồm cả các bước nhỏ nhất, và ghi chú rõ các tính năng FreeRTOS được sử dụng.

---

## PHASE 1: KHỞI TẠO HỆ THỐNG (app_main)

### Bước 1.1: Khởi tạo NVS (Non-Volatile Storage)
- **Thực hiện**: `nvs_flash_init()`
- **Mục đích**: Chuẩn bị phân vùng flash để lưu/đọc cấu hình
- **Xử lý lỗi**: Nếu `ESP_ERR_NVS_NO_FREE_PAGES` hoặc `ESP_ERR_NVS_NEW_VERSION_FOUND` → xóa và khởi tạo lại
- **FreeRTOS**: ❌ Không sử dụng (ESP-IDF API)

### Bước 1.2: Tạo Event Group
- **Thực hiện**: `xEventGroupCreate()` → `system_ready_events`
- **Mục đích**: Đồng bộ hóa khởi tạo các components
- **FreeRTOS**: ✅ **Event Group** - Tạo event group để synchronize system initialization
- **Bits được định nghĩa**:
  - `BIT_CONFIG_READY` (bit 0)
  - `BIT_SPEAKER_READY` (bit 1)
  - `BIT_BLE_READY` (bit 2)
  - `BIT_SCREEN_READY` (bit 3)
  - `BIT_BUTTON_READY` (bit 4)

---

## PHASE 2: KHỞI TẠO TPMS CONFIG COMPONENT

### Bước 2.1: Gọi `tpms_config_init()`
- **Thực hiện**: Tạo mutexes và ESP-IDF timer
- **Bước 2.1.1**: Tạo Config Mutex
  - **FreeRTOS**: ✅ **Mutex** - `xSemaphoreCreateMutex()` → `config_mutex`
  - **Mục đích**: Bảo vệ `g_tpms_config` khi đọc/ghi từ nhiều tasks
- **Bước 2.1.2**: Tạo NVS Mutex
  - **FreeRTOS**: ✅ **Mutex** - `xSemaphoreCreateMutex()` → `nvs_mutex`
  - **Mục đích**: Serialize các thao tác NVS (read/write)
- **Bước 2.1.3**: Tạo ESP-IDF Timer
  - **FreeRTOS**: ✅ **ESP-IDF Timer** - `esp_timer_create()` → `s_save_timer`
  - **Mục đích**: Deferred save configuration vào NVS (callback: `save_timer_cb`)
  - **Dispatch method**: `ESP_TIMER_TASK` (chạy trong timer task context)

### Bước 2.2: Gọi `tpms_config_load()`
- **Thực hiện**: Đọc cấu hình từ NVS
- **Bước 2.2.1**: Mở NVS namespace "tpms_storage" (READONLY)
- **Bước 2.2.2**: Đọc blob "tpms_config" vào `g_tpms_config`
- **Bước 2.2.3**: Kiểm tra version (phải = 1)
- **Bước 2.2.4**: Nếu lỗi → dùng giá trị mặc định
- **FreeRTOS**: ❌ Không sử dụng (NVS API)

### Bước 2.3: Gọi `tpms_config_update_devices_by_swap_mode()`
- **Thực hiện**: Map thiết bị theo chế độ đổi lốp
- **Bước 2.3.1**: Lock config mutex
  - **FreeRTOS**: ✅ **Mutex** - `xSemaphoreTake(config_mutex, portMAX_DELAY)`
- **Bước 2.3.2**: Đọc `tire_swap` từ config
- **Bước 2.3.3**: Map địa chỉ BLE theo mode (Initial/Vertical/Cross/Horizontal)
- **Bước 2.3.4**: Cập nhật `g_ai_devices[]` array
- **Bước 2.3.5**: Unlock config mutex
  - **FreeRTOS**: ✅ **Mutex** - `xSemaphoreGive(config_mutex)`

### Bước 2.4: Set Event Group Bit
- **Thực hiện**: `xEventGroupSetBits(system_ready_events, BIT_CONFIG_READY)`
- **FreeRTOS**: ✅ **Event Group** - Set bit để báo config component đã sẵn sàng

---

## PHASE 3: KHỞI TẠO SPEAKER COMPONENT

### Bước 3.1: Gọi `speaker_init()`
- **Bước 3.1.1**: Cấu hình UART (baud rate 9600, GPIO 2/3)
- **Bước 3.1.2**: Tạo Audio Mutex
  - **FreeRTOS**: ✅ **Mutex** - `xSemaphoreCreateMutex()` → `s_audio_mutex`
  - **Mục đích**: Bảo vệ UART operations khi gửi commands đến DFPlayer
- **Bước 3.1.3**: Delay 1500ms để DFPlayer khởi động
- **Bước 3.1.4**: Gửi command set volume (25/30)

### Bước 3.2: Cập nhật cài đặt giọng nói
- **Thực hiện**: `speaker_update_voice_setting()` và `speaker_set_voice_enabled()`
- **Đọc từ config**: `warning_settings` và `warm_up_greetings`

### Bước 3.3: Tạo Speaker Queue
- **Thực hiện**: `xQueueCreate(1, sizeof(sensor_data_t))` → `speaker_queue`
- **FreeRTOS**: ✅ **Queue** - Queue size 1 để tránh audio backlog
- **Mục đích**: Giao tiếp giữa BLE scanner task và speaker task

### Bước 3.4: Gọi `speaker_task_start()`
- **Bước 3.4.1**: Tạo Priority Queue Mutex
  - **FreeRTOS**: ✅ **Mutex** - `xSemaphoreCreateMutex()` → `s_priority_queue_mutex`
  - **Mục đích**: Bảo vệ priority queue `s_priority_queue[]`
- **Bước 3.4.2**: Tạo Sensor Event Group
  - **FreeRTOS**: ✅ **Event Group** - `xEventGroupCreate()` → `s_sensor_event_group`
  - **Mục đích**: Track 4 sensors đã được phát hiện (bits: TT, TP, ST, SP)
- **Bước 3.4.3**: Tạo Speaker Task
  - **FreeRTOS**: ✅ **Task** - `xTaskCreate(speaker_task, ...)` → priority 2
  - **Mục đích**: Xử lý queue và phát audio alerts
- **Bước 3.4.4**: Tạo All-Sensors Tracker Task
  - **FreeRTOS**: ✅ **Task** - `xTaskCreate(speaker_all_sensors_task, ...)` → priority 3
  - **Mục đích**: Chờ tất cả 4 sensors được phát hiện, sau đó phát file 14.mp3
- **Bước 3.4.5**: Set Event Group Bit
  - **FreeRTOS**: ✅ **Event Group** - `xEventGroupSetBits(event_group, BIT_SPEAKER_READY)`

---

## PHASE 4: KHỞI TẠO BLE SCANNER COMPONENT

### Bước 4.1: Gọi `ble_scanner_init()`
- **Bước 4.1.1**: Tạo Sensor Data Mutex
  - **FreeRTOS**: ✅ **Mutex** - `xSemaphoreCreateMutex()` → `sensor_data_mutex`
  - **Mục đích**: Bảo vệ global sensor variables (front_left_*, front_right_*, rear_left_*, rear_right_*)
- **Bước 4.1.2**: Tạo Alert Mutex
  - **FreeRTOS**: ✅ **Mutex** - `xSemaphoreCreateMutex()` → `s_alert_mutex`
  - **Mục đích**: Bảo vệ `s_pending_alert` buffer cho alert throttling
- **Bước 4.1.4**: Tạo Speaker Alert Throttling Timer
  - **FreeRTOS**: ✅ **FreeRTOS Software Timer** - `xTimerCreate()` → `s_speaker_alert_timer`
  - **Period**: 8000ms (one-shot, không auto-reload)
  - **Mục đích**: Throttle alerts để tránh spam speaker queue
- **Bước 4.1.5**: Tạo BLE Scan Status Check Timer
  - **FreeRTOS**: ✅ **FreeRTOS Software Timer** - `xTimerCreate()` → `s_ble_scan_timer`
  - **Period**: 30000ms (auto-reload, periodic)
  - **Mục đích**: Kiểm tra trạng thái BLE scan định kỳ
- **Bước 4.1.6**: Khởi tạo ESP32 Bluetooth Controller
  - **Thực hiện**: `esp_bt_controller_init()`, `esp_bt_controller_enable()`
  - **FreeRTOS**: ❌ Không sử dụng (ESP-IDF BLE API)
- **Bước 4.1.7**: Đăng ký VHCI callback
  - **Thực hiện**: `esp_vhci_host_register_callback(&vhci_host_cb)`
  - **Callback**: `host_rcv_pkt()` - xử lý BLE packets

### Bước 4.2: Gọi `ble_scanner_start()`
- **Bước 4.2.1**: Lưu speaker_queue handle
- **Bước 4.2.2**: Gửi HCI commands tuần tự:
  - Command 0: Reset
  - Command 1: Set Event Mask
  - Command 2: Set BLE Scan Parameters
  - Command 3: Start BLE Scan
- **Bước 4.2.3**: Tạo HCI Event Process Task
  - **FreeRTOS**: ✅ **Task** - `xTaskCreatePinnedToCore(hci_evt_process, ...)` → priority 6, core 0
  - **Mục đích**: Xử lý BLE events (hiện tại chỉ delay vô hạn)
- **Bước 4.2.4**: Start BLE Scan Status Check Timer
  - **FreeRTOS**: ✅ **FreeRTOS Software Timer** - `xTimerStart(s_ble_scan_timer, 0)`
- **Bước 4.2.5**: Delay 200ms để scan thực sự bắt đầu
- **Bước 4.2.6**: Set Event Group Bit
  - **FreeRTOS**: ✅ **Event Group** - `xEventGroupSetBits(event_group, BIT_BLE_READY)`

---

## PHASE 5: KHỞI TẠO SCREEN COMPONENT

### Bước 5.1: Gọi `screen_init()`
- **Thực hiện**: Không có gì (empty function)

### Bước 5.2: Tạo UI Queue
- **Thực hiện**: `xQueueCreate(8, sizeof(ui_update_t))` → `ui_queue`
- **FreeRTOS**: ✅ **Queue** - Queue size 8 để xử lý nhiều button events
- **Mục đích**: Giao tiếp giữa button task và screen task

### Bước 5.3: Gọi `screen_task_start()`
- **Bước 5.3.1**: Lưu ui_queue và event_group handles
- **Bước 5.3.2**: Tạo Screen Task
  - **FreeRTOS**: ✅ **Task** - `xTaskCreate(ui_task, ...)` → priority 4
  - **Mục đích**: Xử lý UI và vẽ màn hình OLED

---

## PHASE 6: KHỞI TẠO BUTTON COMPONENT

### Bước 6.1: Gọi `button_init()`
- **Bước 6.1.1**: Tạo Button Poll Binary Semaphore
  - **FreeRTOS**: ✅ **Binary Semaphore** - `xSemaphoreCreateBinary()` → `s_button_poll_sem`
  - **Mục đích**: Signal từ timer callback để wake up button task
- **Bước 6.1.2**: Tạo Button Polling Timer
  - **FreeRTOS**: ✅ **FreeRTOS Software Timer** - `xTimerCreate()` → `s_button_poll_timer`
  - **Period**: 5ms (auto-reload, periodic)
  - **Callback**: `button_poll_timer_callback()` - gives semaphore từ ISR context

### Bước 6.2: Gọi `button_task_start()`
- **Bước 6.2.1**: Lưu ui_queue và event_group handles
- **Bước 6.2.2**: Start Button Polling Timer
  - **FreeRTOS**: ✅ **FreeRTOS Software Timer** - `xTimerStart(s_button_poll_timer, 0)`
- **Bước 6.2.3**: Tạo Button Input Task
  - **FreeRTOS**: ✅ **Task** - `xTaskCreate(input_task, ...)` → priority 3
  - **Mục đích**: Poll buttons và gửi UI updates

---

## PHASE 7: ĐỒNG BỘ HÓA KHỞI TẠO

### Bước 7.1: Chờ tất cả components ready
- **Thực hiện**: `xEventGroupWaitBits(system_ready_events, ALL_SYSTEM_READY, pdTRUE, pdTRUE, portMAX_DELAY)`
- **FreeRTOS**: ✅ **Event Group** - Wait cho tất cả 5 bits được set
- **Mục đích**: Đảm bảo system chỉ hoạt động khi tất cả components đã sẵn sàng
- **Clear bits on exit**: `pdTRUE` (xóa bits sau khi wait thành công)
- **Wait for all bits**: `pdTRUE` (chờ tất cả bits, không phải chỉ một)

### Bước 7.2: Tạo Notification Demo Task (optional)
- **Thực hiện**: `xTaskCreate(notification_demo_task, ...)` → priority 1
- **FreeRTOS**: ✅ **Task** - Demo task sử dụng Task Notification
- **Mục đích**: Demo lightweight signaling mechanism

---

## PHASE 8: RUNTIME OPERATIONS (Các task chạy song song)

---

## TASK 1: BLE SCANNER TASK (hci_evt_process)

### Vòng lặp chính:
- **Bước 8.1.1**: `vTaskDelay(portMAX_DELAY)` - Sleep vô hạn
- **FreeRTOS**: ✅ **Task Delay** - Task này hiện tại không làm gì, chỉ delay

### BLE Packet Processing (trong callback `host_rcv_pkt`):
- **Bước 8.1.2**: Nhận BLE advertisement packet từ VHCI callback
- **Bước 8.1.3**: Parse packet header (event type, address, data length, RSSI)
- **Bước 8.1.4**: Extract local name từ advertisement data
- **Bước 8.1.5**: Kiểm tra nếu local name = "AI-8000"
- **Bước 8.1.6**: Parse sensor data (temperature, battery, pressure)
- **Bước 8.1.7**: Lookup device bằng địa chỉ BLE (TT/TP/ST/SP)
- **Bước 8.1.8**: Gọi `speaker_notify_sensor_detected()`
  - **FreeRTOS**: ✅ **Event Group** - `xEventGroupSetBits(s_sensor_event_group, bit)` - Set bit tương ứng với sensor
- **Bước 8.1.9**: Kiểm tra áp suất bất thường (HIGH/LOW threshold)
- **Bước 8.1.10**: Nếu áp suất bất thường → Gửi vào speaker_queue
  - **FreeRTOS**: ✅ **Queue** - `xQueueSend(s_speaker_queue, &sensor_data, 0)` - Non-blocking
- **Bước 8.1.11**: Lock Sensor Data Mutex
  - **FreeRTOS**: ✅ **Mutex** - `xSemaphoreTake(sensor_data_mutex, portMAX_DELAY)`
- **Bước 8.1.12**: Cập nhật global sensor variables theo device_name:
  - TT → `front_left_*`
  - TP → `front_right_*`
  - ST → `rear_left_*`
  - SP → `rear_right_*`
- **Bước 8.1.13**: Unlock Sensor Data Mutex
  - **FreeRTOS**: ✅ **Mutex** - `xSemaphoreGive(sensor_data_mutex)`

### BLE Scan Status Check Timer Callback (mỗi 30 giây):
- **Bước 8.1.15**: Timer callback được gọi
  - **FreeRTOS**: ✅ **FreeRTOS Software Timer** - `ble_scan_timer_callback()`
- **Bước 8.1.16**: Log scan status (có thể mở rộng để restart scan nếu cần)

### Speaker Alert Throttling Timer Callback (khi có alert):
- **Bước 8.1.17**: Timer callback được gọi sau 8 giây
  - **FreeRTOS**: ✅ **FreeRTOS Software Timer** - `speaker_alert_timer_callback()`
- **Bước 8.1.18**: Lock Alert Mutex
  - **FreeRTOS**: ✅ **Mutex** - `xSemaphoreTake(s_alert_mutex, portMAX_DELAY)`
- **Bước 8.1.19**: Đọc `s_pending_alert` từ buffer
- **Bước 8.1.20**: Unlock Alert Mutex
  - **FreeRTOS**: ✅ **Mutex** - `xSemaphoreGive(s_alert_mutex)`
- **Bước 8.1.21**: Gửi pending alert vào speaker_queue
  - **FreeRTOS**: ✅ **Queue** - `xQueueSend(s_speaker_queue, &pending, 0)`
- **Bước 8.1.22**: Nếu queue full → Reschedule timer
  - **FreeRTOS**: ✅ **FreeRTOS Software Timer** - `xTimerStart(s_speaker_alert_timer, 0)`

---

## TASK 2: SPEAKER TASK (speaker_task)

### Vòng lặp chính:
- **Bước 8.2.1**: Nhận tất cả alerts từ queue (non-blocking)
  - **FreeRTOS**: ✅ **Queue** - `xQueueReceive(queue, &sensor_data, 0)` - Timeout 0 (non-blocking)
- **Bước 8.2.2**: Với mỗi alert nhận được:
  - **Bước 8.2.2.1**: Gọi `speaker_notify_sensor_detected()`
    - **FreeRTOS**: ✅ **Event Group** - Set bit trong `s_sensor_event_group`
  - **Bước 8.2.2.2**: Thêm vào Priority Queue
    - **Bước 8.2.2.2.1**: Lock Priority Queue Mutex
      - **FreeRTOS**: ✅ **Mutex** - `xSemaphoreTake(s_priority_queue_mutex, portMAX_DELAY)`
    - **Bước 8.2.2.2.2**: Tìm vị trí chèn (thay thế nếu đã có cảnh báo cho cùng sensor)
    - **Bước 8.2.2.2.3**: Gán priority (TT=0, TP=1, ST=2, SP=3)
    - **Bước 8.2.2.2.4**: Lưu timestamp
    - **Bước 8.2.2.2.5**: Unlock Priority Queue Mutex
      - **FreeRTOS**: ✅ **Mutex** - `xSemaphoreGive(s_priority_queue_mutex)`
- **Bước 8.2.3**: Xử lý Priority Queue theo thứ tự ưu tiên
  - **Bước 8.2.3.1**: Gọi `speaker_priority_queue_get_next()`
    - **Bước 8.2.3.1.1**: Lock Priority Queue Mutex
      - **FreeRTOS**: ✅ **Mutex** - `xSemaphoreTake(s_priority_queue_mutex, portMAX_DELAY)`
    - **Bước 8.2.3.1.2**: Tìm sensor có priority hiện tại đang chờ (round-robin)
    - **Bước 8.2.3.1.3**: Kiểm tra timeout (20 giây) → chuyển sang sensor tiếp theo nếu timeout
    - **Bước 8.2.3.1.4**: Unlock Priority Queue Mutex
      - **FreeRTOS**: ✅ **Mutex** - `xSemaphoreGive(s_priority_queue_mutex)`
  - **Bước 8.2.3.2**: Nếu có alert → Phát audio
    - **Bước 8.2.3.2.1**: Gọi `speaker_play_pressure_alert()`
    - **Bước 8.2.3.2.2**: Xác định track ID dựa trên áp suất và vị trí lốp
    - **Bước 8.2.3.2.3**: Gọi `speaker_play_track_guarded()`
      - **Bước 8.2.3.2.3.1**: Lock Audio Mutex
        - **FreeRTOS**: ✅ **Mutex** - `xSemaphoreTake(s_audio_mutex, portMAX_DELAY)`
      - **Bước 8.2.3.2.3.2**: Map track ID theo giọng nam/nữ
      - **Bước 8.2.3.2.3.3**: Gửi UART command đến DFPlayer
      - **Bước 8.2.3.2.3.4**: Delay guard time (5000ms)
      - **Bước 8.2.3.2.3.5**: Unlock Audio Mutex
        - **FreeRTOS**: ✅ **Mutex** - `xSemaphoreGive(s_audio_mutex)`
    - **Bước 8.2.3.2.4**: Xóa khỏi Priority Queue
      - **Bước 8.2.3.2.4.1**: Lock Priority Queue Mutex
        - **FreeRTOS**: ✅ **Mutex** - `xSemaphoreTake(s_priority_queue_mutex, portMAX_DELAY)`
      - **Bước 8.2.3.2.4.2**: Tìm và xóa alert
      - **Bước 8.2.3.2.4.3**: Chuyển sang sensor tiếp theo (round-robin)
      - **Bước 8.2.3.2.4.4**: Unlock Priority Queue Mutex
        - **FreeRTOS**: ✅ **Mutex** - `xSemaphoreGive(s_priority_queue_mutex)`
- **Bước 8.2.4**: Nếu priority queue rỗng → Chờ alert mới với timeout
  - **FreeRTOS**: ✅ **Queue** - `xQueueReceive(queue, &sensor_data, pdMS_TO_TICKS(100))` - Timeout 100ms

---

## TASK 3: ALL-SENSORS TRACKER TASK (speaker_all_sensors_task)

### Vòng lặp chính:
- **Bước 8.3.1**: Wait cho tất cả 4 sensors được phát hiện
  - **FreeRTOS**: ✅ **Event Group** - `xEventGroupWaitBits(s_sensor_event_group, SENSOR_BITS_ALL, pdTRUE, pdTRUE, portMAX_DELAY)`
  - **Clear bits**: `pdTRUE` (one-shot task)
  - **Wait for all**: `pdTRUE` (chờ tất cả 4 bits)
- **Bước 8.3.2**: Nếu tất cả sensors đã được phát hiện:
  - **Bước 8.3.2.1**: Phát file 14.mp3 (warm-up greetings)
    - **FreeRTOS**: ✅ **Mutex** - Lock `s_audio_mutex` trước khi phát
  - **Bước 8.3.2.2**: Set `s_alerts_enabled = true` (cho phép alerts)
  - **Bước 8.3.2.3**: Delay guard time (6000ms)
- **Bước 8.3.3**: Delete task (one-shot)
  - **FreeRTOS**: ✅ **Task** - `vTaskDelete(NULL)`

---

## TASK 4: BUTTON INPUT TASK (input_task)

### Khởi tạo:
- **Bước 8.4.1**: Gọi `buttons_init_polling()`
  - **Bước 8.4.1.1**: Cấu hình GPIO (GPIO 6, 7, 9) làm input với pull-up
  - **Bước 8.4.1.2**: Khởi tạo button structures với trạng thái ban đầu
- **Bước 8.4.2**: Set Event Group Bit
  - **FreeRTOS**: ✅ **Event Group** - `xEventGroupSetBits(s_event_group, BIT_BUTTON_READY)`
- **Bước 8.4.3**: Gửi UI update ban đầu vào queue
  - **FreeRTOS**: ✅ **Queue** - `xQueueSend(s_ui_queue, &init, 0)`

### Vòng lặp chính:
- **Bước 8.4.4**: Wait cho Button Poll Semaphore
  - **FreeRTOS**: ✅ **Binary Semaphore** - `xSemaphoreTake(s_button_poll_sem, portMAX_DELAY)`
  - **Mục đích**: Được wake up mỗi 5ms từ timer callback
- **Bước 8.4.5**: Đọc GPIO levels của 3 buttons
- **Bước 8.4.6**: Gọi `button_update()` cho mỗi button
  - **Bước 8.4.6.1**: So sánh raw_level với debounced_level
  - **Bước 8.4.6.2**: Nếu thay đổi và đã qua debounce time (30ms) → cập nhật debounced_level
  - **Bước 8.4.6.3**: Detect rising/falling edge
  - **Bước 8.4.6.4**: Xử lý repeat nếu enable_repeat = true:
    - Sau HOLD_THRESHOLD_MS (450ms) → bắt đầu repeat
    - Repeat interval giảm dần (từ 120ms → 60ms tối thiểu)
- **Bước 8.4.7**: Xử lý Long Press DOWN (2 giây) → Toggle mirror
  - **Bước 8.4.7.1**: Tạo UI update với `toggle_mirror = 1`
  - **FreeRTOS**: ✅ **Queue** - `xQueueSend(s_ui_queue, &u, 0)`
- **Bước 8.4.8**: Xử lý button events theo mode hiện tại:
  - **MODE_HELLO**: MODE button → chuyển sang MODE_MENU
  - **MODE_MENU**: UP/DOWN → thay đổi `sel`, MODE → vào MODE_ITEM/MODE_ADJUST
  - **MODE_ITEM**: UP/DOWN → thay đổi `sub`, MODE → save config hoặc chuyển mode
  - **MODE_ADJUST**: UP/DOWN → tăng/giảm giá trị, MODE → save và quay lại MENU
  - **MODE_SENSOR**: UP/DOWN → chọn sensor, MODE → quay lại MENU
- **Bước 8.4.9**: Khi thay đổi config:
  - **Bước 8.4.9.1**: Lock Config Mutex
    - **FreeRTOS**: ✅ **Mutex** - `tpms_config_lock()` → `xSemaphoreTake(config_mutex, portMAX_DELAY)`
  - **Bước 8.4.9.2**: Đọc/ghi config
  - **Bước 8.4.9.3**: Unlock Config Mutex
    - **FreeRTOS**: ✅ **Mutex** - `tpms_config_unlock()` → `xSemaphoreGive(config_mutex)`
  - **Bước 8.4.9.4**: Gọi `tpms_config_save()` hoặc `tpms_config_schedule_save(800)`
    - **Nếu schedule_save**: 
      - **FreeRTOS**: ✅ **ESP-IDF Timer** - `esp_timer_start_once(s_save_timer, 800ms)`
- **Bước 8.4.10**: Gửi UI update vào queue
  - **FreeRTOS**: ✅ **Queue** - `xQueueSend(s_ui_queue, &u, 0)`

### Button Poll Timer Callback (mỗi 5ms):
- **Bước 8.4.11**: Timer callback được gọi
  - **FreeRTOS**: ✅ **FreeRTOS Software Timer** - `button_poll_timer_callback()`
- **Bước 8.4.12**: Give Button Poll Semaphore từ ISR context
  - **FreeRTOS**: ✅ **Binary Semaphore** - `xSemaphoreGiveFromISR(s_button_poll_sem, &xHigherPriorityTaskWoken)`
- **Bước 8.4.13**: Yield nếu cần
  - **FreeRTOS**: ✅ **ISR Yield** - `portYIELD_FROM_ISR(xHigherPriorityTaskWoken)`

---

## TASK 5: SCREEN UI TASK (ui_task)

### Khởi tạo:
- **Bước 8.5.1**: Lưu task handle
  - **FreeRTOS**: ✅ **Task Handle** - `xTaskGetCurrentTaskHandle()` → `s_ui_task_handle`
- **Bước 8.5.2**: Khởi tạo U8G2 HAL (I2C: GPIO 5/4, address 0x3C)
- **Bước 8.5.3**: Setup U8G2 display (SH1106 128x64)
- **Bước 8.5.4**: Đọc `display_mirrored` từ config
  - **FreeRTOS**: ✅ **Mutex** - `tpms_config_lock()` / `tpms_config_unlock()`
- **Bước 8.5.5**: Set display rotation (U8G2_MIRROR hoặc U8G2_R0)
- **Bước 8.5.6**: Set Event Group Bit
  - **FreeRTOS**: ✅ **Event Group** - `xEventGroupSetBits(s_event_group, BIT_SCREEN_READY)`
- **Bước 8.5.7**: Vẽ màn hình HELLO ban đầu

### Vòng lặp chính:
- **Bước 8.5.8**: Chờ UI update từ queue hoặc timeout (500ms)
  - **FreeRTOS**: ✅ **Queue** - `xQueueReceive(s_ui_queue, &upd, pdMS_TO_TICKS(BLINK_INTERVAL_MS))`
- **Bước 8.5.9**: Nếu nhận được update:
  - **Bước 8.5.9.1**: Kiểm tra `toggle_mirror` flag
    - **Nếu có**:
      - **Bước 8.5.9.1.1**: Lock Config Mutex
        - **FreeRTOS**: ✅ **Mutex** - `tpms_config_lock()`
      - **Bước 8.5.9.1.2**: Toggle `display_mirrored`
      - **Bước 8.5.9.1.3**: Unlock Config Mutex
        - **FreeRTOS**: ✅ **Mutex** - `tpms_config_unlock()`
      - **Bước 8.5.9.1.4**: Set display rotation
      - **Bước 8.5.9.1.5**: Schedule save config
        - **FreeRTOS**: ✅ **ESP-IDF Timer** - `tpms_config_schedule_save(800)`
      - **Bước 8.5.9.1.6**: Reset blink state
      - **Bước 8.5.9.1.7**: Vẽ lại màn hình hiện tại
      - **Bước 8.5.9.1.8**: Continue (không xử lý mode/sel/sub)
  - **Bước 8.5.9.2**: Cập nhật mode/sel/sub/adjust từ update
  - **Bước 8.5.9.3**: Reset blink state
- **Bước 8.5.10**: Nếu timeout và đang ở MODE_HELLO:
  - **Bước 8.5.10.1**: Toggle blink_visible mỗi 500ms
- **Bước 8.5.11**: Vẽ màn hình tương ứng:
  - **MODE_HELLO**: Gọi `draw_hello_tpms()`
    - **Bước 8.5.11.1**: Lock Sensor Data Mutex
      - **FreeRTOS**: ✅ **Mutex** - `ble_scanner_lock_sensor_data()`
    - **Bước 8.5.11.2**: Lock Config Mutex
      - **FreeRTOS**: ✅ **Mutex** - `tpms_config_lock()`
    - **Bước 8.5.11.3**: Đọc sensor data và config values
    - **Bước 8.5.11.4**: Unlock Config Mutex
      - **FreeRTOS**: ✅ **Mutex** - `tpms_config_unlock()`
    - **Bước 8.5.11.5**: Unlock Sensor Data Mutex
      - **FreeRTOS**: ✅ **Mutex** - `ble_scanner_unlock_sensor_data()`
    - **Bước 8.5.11.6**: Convert đơn vị áp suất (PSI/BAR)
    - **Bước 8.5.11.7**: Kiểm tra áp suất/nhiệt độ vượt ngưỡng
    - **Bước 8.5.11.8**: Vẽ với blink logic (ẩn giá trị nếu out-of-range và blink_visible = false)
  - **MODE_MENU**: Gọi `draw_menu_3line()`
  - **MODE_ITEM**: Gọi `draw_item_detail()`
    - **Bước 8.5.11.9**: Lock Config Mutex để đọc config
      - **FreeRTOS**: ✅ **Mutex** - `tpms_config_lock()`
    - **Bước 8.5.11.10**: Đọc config values
    - **Bước 8.5.11.11**: Unlock Config Mutex
      - **FreeRTOS**: ✅ **Mutex** - `tpms_config_unlock()`
  - **MODE_ADJUST**: Gọi `draw_adjust_screen()`
    - **Bước 8.5.11.12**: Lock Config Mutex để đọc giá trị cần chỉnh
      - **FreeRTOS**: ✅ **Mutex** - `tpms_config_lock()`
    - **Bước 8.5.11.13**: Đọc và convert giá trị
    - **Bước 8.5.11.14**: Unlock Config Mutex
      - **FreeRTOS**: ✅ **Mutex** - `tpms_config_unlock()`
  - **MODE_SENSOR**: Gọi `draw_sensor_detail()`
    - **Bước 8.5.11.15**: Lock Config Mutex để đọc địa chỉ sensor
      - **FreeRTOS**: ✅ **Mutex** - `tpms_config_lock()`
    - **Bước 8.5.11.16**: Đọc địa chỉ BLE
    - **Bước 8.5.11.17**: Unlock Config Mutex
      - **FreeRTOS**: ✅ **Mutex** - `tpms_config_unlock()`

---

## TASK 6: NOTIFICATION DEMO TASK (notification_demo_task)

### Vòng lặp chính:
- **Bước 8.6.1**: Wait cho notification với timeout 5 giây
  - **FreeRTOS**: ✅ **Task Notification** - `ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000))`
- **Bước 8.6.2**: Nếu nhận được notification:
  - **Bước 8.6.2.1**: Kiểm tra bit `NOTIFY_SENSOR_UPDATE`
  - **Bước 8.6.2.2**: Notify screen task
    - **FreeRTOS**: ✅ **Task Notification** - `xTaskNotify(screen_task, NOTIFY_SENSOR_UPDATE, eSetBits)`
- **Bước 8.6.3**: Delay 10 giây
  - **FreeRTOS**: ✅ **Task Delay** - `vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(10000))`

---

## PHASE 9: CONFIG SAVE OPERATIONS

### Khi config thay đổi (từ button task hoặc screen task):
- **Bước 9.1**: Gọi `tpms_config_schedule_save(800)`
  - **Bước 9.1.1**: Set `cfg_dirty = true`
  - **Bước 9.1.2**: Stop timer nếu đang chạy
    - **FreeRTOS**: ✅ **ESP-IDF Timer** - `esp_timer_stop(s_save_timer)`
  - **Bước 9.1.3**: Start timer với delay 800ms
    - **FreeRTOS**: ✅ **ESP-IDF Timer** - `esp_timer_start_once(s_save_timer, 800ms)`

### Timer callback (sau 800ms):
- **Bước 9.2**: `save_timer_cb()` được gọi
  - **FreeRTOS**: ✅ **ESP-IDF Timer** - Callback chạy trong timer task context
- **Bước 9.3**: Kiểm tra `cfg_dirty`
- **Bước 9.4**: Lock NVS Mutex
  - **FreeRTOS**: ✅ **Mutex** - `xSemaphoreTake(nvs_mutex, pdMS_TO_TICKS(1000))`
- **Bước 9.5**: Mở NVS namespace "tpms_storage" (READWRITE)
- **Bước 9.6**: Lock Config Mutex
  - **FreeRTOS**: ✅ **Mutex** - `xSemaphoreTake(config_mutex, pdMS_TO_TICKS(100))`
- **Bước 9.7**: Ghi config vào NVS blob
- **Bước 9.8**: Unlock Config Mutex
  - **FreeRTOS**: ✅ **Mutex** - `xSemaphoreGive(config_mutex)`
- **Bước 9.9**: Commit NVS
- **Bước 9.10**: Close NVS handle
- **Bước 9.11**: Unlock NVS Mutex
  - **FreeRTOS**: ✅ **Mutex** - `xSemaphoreGive(nvs_mutex)`
- **Bước 9.12**: Set `cfg_dirty = false`

---

## TỔNG KẾT CÁC TÍNH NĂNG FREERTOS ĐƯỢC SỬ DỤNG

### 1. **Task** (6 tasks):
- `speaker_task` (priority 2)
- `speaker_all_sensors_task` (priority 3, one-shot)
- `input_task` (priority 3)
- `ui_task` (priority 4)
- `hci_evt_process` (priority 6, core 0)
- `notification_demo_task` (priority 1)

### 2. **Queue** (2 queues):
- `speaker_queue` (size 1, `sensor_data_t`)
- `ui_queue` (size 8, `ui_update_t`)

### 3. **Mutex** (6 mutexes):
- `config_mutex` - Bảo vệ `g_tpms_config`
- `nvs_mutex` - Serialize NVS operations
- `sensor_data_mutex` - Bảo vệ global sensor variables
- `s_alert_mutex` - Bảo vệ pending alert buffer
- `s_audio_mutex` - Bảo vệ UART operations
- `s_priority_queue_mutex` - Bảo vệ priority queue

### 4. **Binary Semaphore** (1 semaphore):
- `s_button_poll_sem` - Signal từ timer callback để wake up button task

### 5. **Event Group** (2 event groups):
- `system_ready_events` - Synchronize system initialization (5 bits)
- `s_sensor_event_group` - Track 4 sensors đã được phát hiện (4 bits)

### 6. **FreeRTOS Software Timer** (3 timers):
- `s_button_poll_timer` - 5ms periodic, wake up button task
- `s_ble_scan_timer` - 30s periodic, check BLE scan status
- `s_speaker_alert_timer` - 8s one-shot, throttle alerts

### 7. **ESP-IDF Timer** (1 timer):
- `s_save_timer` - Deferred save config vào NVS (800ms delay)

### 8. **Task Notification**:
- `ulTaskNotifyTake()` - Wait notification trong demo task
- `xTaskNotify()` - Send notification đến screen task

---

**Ngày tạo**: 2024
**Phiên bản**: 1.0


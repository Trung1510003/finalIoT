# FreeRTOS/ESP-IDF Features Review

## Tổng quan
Báo cáo này review việc sử dụng các tính năng FreeRTOS/ESP-IDF trong project:
- Software Timer
- Semaphores (Mutex, Binary, Counting)
- Event Group
- Task Notification

---

## 1. ✅ SOFTWARE TIMER

### ESP-IDF Timer (esp_timer) - **ĐÃ SỬ DỤNG**
**Vị trí**: `components/tpms_config/tpms_config.c`

**Mục đích**: Deferred save configuration vào NVS

**Code**:
```c
// Line 38: Timer handle
static esp_timer_handle_t s_save_timer = NULL;

// Line 70-78: Timer initialization
void tpms_config_init(void) {
    const esp_timer_create_args_t save_tmr_args = {
        .callback = &save_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "cfg_save"
    };
    ESP_ERROR_CHECK(esp_timer_create(&save_tmr_args, &s_save_timer));
}

// Line 44-80: Timer callback
static void save_timer_cb(void* arg) {
    // Save config to NVS
}

// Line 199-205: Schedule save
void tpms_config_schedule_save(uint64_t delay_ms) {
    cfg_dirty = true;
    if (s_save_timer) {
        esp_timer_stop(s_save_timer);
        esp_timer_start_once(s_save_timer, delay_ms * 1000ULL);
    }
}
```

**Đánh giá**: ✅ Tốt - Sử dụng ESP-IDF timer cho deferred save, tránh ghi NVS quá thường xuyên.

### FreeRTOS Software Timer - **ĐÃ SỬ DỤNG** (3 timers)

#### Timer 1: Button Polling Timer
**Vị trí**: `components/button/button.c`

**Mục đích**: Poll buttons mỗi 5ms thay vì dùng vTaskDelay trong loop

**Code**:
```c
// Timer callback (runs in timer task context)
static void button_poll_timer_callback(TimerHandle_t xTimer) {
    // Give semaphore to wake up button task
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (s_button_poll_sem != NULL) {
        xSemaphoreGiveFromISR(s_button_poll_sem, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

// Create timer in button_init()
s_button_poll_timer = xTimerCreate(
    "btn_poll",
    pdMS_TO_TICKS(BTN_POLL_INTERVAL_MS), // 5ms
    pdTRUE,  // Auto-reload (periodic)
    (void*)0,
    button_poll_timer_callback
);

// Task waits on semaphore instead of vTaskDelay
xSemaphoreTake(s_button_poll_sem, portMAX_DELAY);
```

**Đánh giá**: ✅ Tốt - Sử dụng timer thay vì polling loop, chính xác hơn về timing.

#### Timer 2: BLE Scan Status Check Timer
**Vị trí**: `components/ble_scanner/ble_scanner.c`

**Mục đích**: Check BLE scan status định kỳ (30 giây)

**Code**:
```c
// Timer callback
static void ble_scan_timer_callback(TimerHandle_t xTimer) {
    // Check scan status, log statistics, restart if needed
    ESP_LOGD(TAG, "BLE scan status check - scan is active");
}

// Create timer in ble_scanner_init()
s_ble_scan_timer = xTimerCreate(
    "ble_scan_check",
    pdMS_TO_TICKS(BLE_SCAN_CHECK_INTERVAL_MS), // 30 seconds
    pdTRUE,  // Auto-reload (periodic)
    (void*)0,
    ble_scan_timer_callback
);
```

**Đánh giá**: ✅ Tốt - Periodic check cho BLE scan status, có thể mở rộng để restart scan nếu cần.

#### Timer 3: Speaker Alert Throttling Timer
**Vị trí**: `components/ble_scanner/ble_scanner.c`

**Mục đích**: Throttle speaker alerts để tránh spam (8 giây window)

**Code**:
```c
#define SPEAKER_ALERT_INTERVAL_MS 8000

// Timer callback - gửi pending alert vào speaker queue
static void speaker_alert_timer_callback(TimerHandle_t xTimer) {
    // Lấy pending alert từ mutex-protected buffer
    if (xSemaphoreTake(s_alert_mutex, portMAX_DELAY) == pdTRUE) {
        if (s_pending_alert_valid) {
            // Gửi vào speaker queue
            xQueueSend(s_speaker_queue, &s_pending_alert, 0);
            s_pending_alert_valid = false;
        }
        xSemaphoreGive(s_alert_mutex);
    }
}

// Create timer in ble_scanner_init()
s_speaker_alert_timer = xTimerCreate(
    "speaker_alert_gate",
    pdMS_TO_TICKS(SPEAKER_ALERT_INTERVAL_MS), // 8 seconds
    pdFALSE,  // One-shot (not auto-reload)
    NULL,
    speaker_alert_timer_callback
);
```

**Đánh giá**: ✅ Tốt - Throttling mechanism để tránh gửi quá nhiều alerts vào speaker queue, đảm bảo audio không bị gián đoạn.

---

## 2. ✅ SEMAPHORES

### Mutex - **ĐÃ SỬ DỤNG** (6 mutexes)

#### 2.1. Config Mutex
**Vị trí**: `components/tpms_config/tpms_config.c`
```c
static SemaphoreHandle_t config_mutex = NULL;

// Tạo trong tpms_config_init()
config_mutex = xSemaphoreCreateMutex();

// Sử dụng:
void tpms_config_lock(void) {
    if (config_mutex) {
        xSemaphoreTake(config_mutex, portMAX_DELAY);
    }
}

void tpms_config_unlock(void) {
    if (config_mutex) {
        xSemaphoreGive(config_mutex);
    }
}
```

**Bảo vệ**: `g_tpms_config` structure khi đọc/ghi từ nhiều tasks.

#### 2.2. NVS Mutex
**Vị trí**: `components/tpms_config/tpms_config.c`
```c
static SemaphoreHandle_t nvs_mutex = NULL;

// Tạo trong tpms_config_init()
nvs_mutex = xSemaphoreCreateMutex();

// Sử dụng trong:
// - tpms_config_save()
// - save_timer_cb()
```

**Bảo vệ**: Các thao tác NVS (read/write) để tránh conflict.

#### 2.3. Sensor Data Mutex
**Vị trí**: `components/ble_scanner/ble_scanner.c`
```c
static SemaphoreHandle_t sensor_data_mutex = NULL;

// Tạo trong ble_scanner_init()
sensor_data_mutex = xSemaphoreCreateMutex();

// Sử dụng:
void ble_scanner_lock_sensor_data(void) {
    if (sensor_data_mutex) {
        xSemaphoreTake(sensor_data_mutex, portMAX_DELAY);
    }
}

void ble_scanner_unlock_sensor_data(void) {
    if (sensor_data_mutex) {
        xSemaphoreGive(sensor_data_mutex);
    }
}
```

**Bảo vệ**: Global sensor data variables (front_left_*, front_right_*, rear_left_*, rear_right_*)

#### 2.4. Alert Mutex
**Vị trí**: `components/ble_scanner/ble_scanner.c`
```c
static SemaphoreHandle_t s_alert_mutex = NULL;

// Tạo trong ble_scanner_init()
s_alert_mutex = xSemaphoreCreateMutex();

// Sử dụng trong:
// - schedule_speaker_alert() - bảo vệ s_pending_alert
// - speaker_alert_timer_callback() - đọc pending alert
```

**Bảo vệ**: `s_pending_alert` và `s_pending_alert_valid` - buffer cho alert throttling.

#### 2.5. Audio Mutex
**Vị trí**: `components/speaker/speaker.c`
```c
static SemaphoreHandle_t s_audio_mutex = NULL;

// Tạo trong speaker_init()
s_audio_mutex = xSemaphoreCreateMutex();

// Sử dụng trong:
// - speaker_play_track_guarded() - bảo vệ UART operations
```

**Bảo vệ**: UART operations khi gửi commands đến DFPlayer, tránh conflict khi nhiều task gọi phát audio.

#### 2.6. Priority Queue Mutex
**Vị trí**: `components/speaker/speaker.c`
```c
static SemaphoreHandle_t s_priority_queue_mutex = NULL;

// Tạo trong speaker_task_start()
s_priority_queue_mutex = xSemaphoreCreateMutex();

// Sử dụng trong:
// - speaker_priority_queue_add()
// - speaker_priority_queue_get_next()
// - speaker_priority_queue_remove()
```

**Bảo vệ**: `s_priority_queue[]` - priority queue cho alerts theo thứ tự ưu tiên (TT → TP → ST → SP).

**Đánh giá**: ✅ Tốt - Đã bảo vệ tất cả shared data với mutex, bao gồm cả alert throttling và priority queue.

### Binary Semaphore - **ĐÃ SỬ DỤNG** (1 semaphore)

#### Binary Semaphore: Button Poll Semaphore
**Vị trí**: `components/button/button.c`

**Mục đích**: Signal từ timer callback để wake up button task (thay vì dùng vTaskDelay)

**Code**:
```c
static SemaphoreHandle_t s_button_poll_sem = NULL;

// Create trong button_init()
s_button_poll_sem = xSemaphoreCreateBinary();

// Timer callback gives semaphore
static void button_poll_timer_callback(TimerHandle_t xTimer) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (s_button_poll_sem != NULL) {
        xSemaphoreGiveFromISR(s_button_poll_sem, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

// Button task waits on semaphore
xSemaphoreTake(s_button_poll_sem, portMAX_DELAY);
```

**Đánh giá**: ✅ Tốt - Cho phép timer callback (ISR context) wake up button task một cách an toàn.

### Counting Semaphore - **CHƯA SỬ DỤNG**
**Không tìm thấy**: `xSemaphoreCreateCounting`

**Gợi ý**: Có thể sử dụng counting semaphore cho:
- Resource pool (ví dụ: giới hạn số lượng BLE connections đồng thời)
- Producer/Consumer với buffer size > 1

---

## 3. ✅ EVENT GROUP

### **ĐÃ SỬ DỤNG**
**Vị trí**: `main/main.c`, tất cả components

**Mục đích**: Synchronize system initialization - đảm bảo tất cả components ready trước khi system hoạt động

**Code**:
```c
// main.c: Event Group bits
#define BIT_CONFIG_READY      (1 << 0)
#define BIT_SPEAKER_READY     (1 << 1)
#define BIT_BLE_READY         (1 << 2)
#define BIT_SCREEN_READY      (1 << 3)
#define BIT_BUTTON_READY      (1 << 4)
#define ALL_SYSTEM_READY      (BIT_CONFIG_READY | BIT_SPEAKER_READY | BIT_BLE_READY | BIT_SCREEN_READY | BIT_BUTTON_READY)

// Create event group
EventGroupHandle_t system_ready_events = xEventGroupCreate();

// Each component sets bit when ready
xEventGroupSetBits(system_ready_events, BIT_SPEAKER_READY);

// Main task waits for all components
EventBits_t bits = xEventGroupWaitBits(
    system_ready_events,
    ALL_SYSTEM_READY,
    pdTRUE, pdTRUE, portMAX_DELAY
);
```

**Đánh giá**: ✅ Tốt - Đảm bảo system chỉ hoạt động khi tất cả components đã sẵn sàng.

**Gợi ý sử dụng**:
- Synchronize nhiều events (ví dụ: chờ cả BLE ready + OLED ready + DFPlayer ready)
- Multi-task coordination (ví dụ: tất cả sensors đã update xong mới hiển thị)

**Ví dụ có thể áp dụng**:
```c
// Trong main.c
EventGroupHandle_t system_ready_events = xEventGroupCreate();

#define BIT_BLE_READY      (1 << 0)
#define BIT_OLED_READY     (1 << 1)
#define BIT_DFPLAYER_READY (1 << 2)

// Mỗi component set bit khi ready
xEventGroupSetBits(system_ready_events, BIT_BLE_READY);

// Main task chờ tất cả ready
xEventGroupWaitBits(system_ready_events, 
                    BIT_BLE_READY | BIT_OLED_READY | BIT_DFPLAYER_READY,
                    pdTRUE, pdTRUE, portMAX_DELAY);
```

---

## 4. ✅ TASK NOTIFICATION

### **ĐÃ SỬ DỤNG**
**Vị trí**: `main/main.c`, `components/screen/screen.c`

**Mục đích**: Lightweight alternative cho queue khi chỉ cần signal, không cần data

**Code**:
```c
// main.c: Notification demo task
static void notification_demo_task(void* pvParameters) {
    TaskHandle_t screen_task = (TaskHandle_t)pvParameters;
    
    for (;;) {
        // Wait for notification with timeout
        uint32_t notification_value = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
        
        if (notification_value != 0) {
            // Notify screen task
            xTaskNotify(screen_task, NOTIFY_SENSOR_UPDATE, eSetBits);
        }
    }
}

// screen.c: Store task handle
static TaskHandle_t s_ui_task_handle = NULL;
s_ui_task_handle = xTaskGetCurrentTaskHandle();

// Get task handle for notifications
TaskHandle_t screen_get_task_handle(void);
```

**Đánh giá**: ✅ Tốt - Lightweight signaling mechanism, nhanh hơn queue cho simple notifications.

**Gợi ý sử dụng**:
- Lightweight alternative cho queue (khi chỉ cần signal, không cần data)
- Faster than queue cho simple notifications
- ISR to task communication

**Ví dụ có thể áp dụng**:
```c
// Thay vì dùng queue cho simple button events:
// xQueueSend(ui_queue, &u, 0);

// Có thể dùng task notification:
TaskHandle_t ui_task_handle;
xTaskNotify(ui_task_handle, NOTIFY_BUTTON_PRESSED, eSetBits);

// Trong ui_task:
ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
```

**Lưu ý**: Task notification nhanh hơn queue nhưng chỉ có thể gửi 1 notification mỗi lần (không có buffer).

---

## 5. ✅ QUEUE

### **ĐÃ SỬ DỤNG** (2 queues)

#### Queue 1: UI Queue
**Vị trí**: `main/main.c`, `components/button/button.c`, `components/screen/screen.c`

**Mục đích**: Giao tiếp giữa button task và screen task

**Code**:
```c
// Create trong main.c
QueueHandle_t ui_queue = xQueueCreate(8, sizeof(ui_update_t));

// Button task gửi updates
xQueueSend(s_ui_queue, &upd, 0);

// Screen task nhận updates
xQueueReceive(s_ui_queue, &upd, pdMS_TO_TICKS(BLINK_INTERVAL_MS));
```

**Kích thước**: 8 items, mỗi item là `ui_update_t` structure

#### Queue 2: Speaker Queue
**Vị trí**: `main/main.c`, `components/ble_scanner/ble_scanner.c`, `components/speaker/speaker.c`

**Mục đích**: Giao tiếp giữa BLE scanner task và speaker task để gửi alerts

**Code**:
```c
// Create trong main.c
QueueHandle_t speaker_queue = xQueueCreate(1, sizeof(sensor_data_t));

// BLE scanner gửi alerts (qua timer callback)
xQueueSend(s_speaker_queue, &pending, 0);

// Speaker task nhận alerts
xQueueReceive(speaker_queue, &sensor_data, portMAX_DELAY);
```

**Kích thước**: 1 item, mỗi item là `sensor_data_t` structure

**Đánh giá**: ✅ Tốt - Queue size nhỏ (1 item) cho speaker queue để tránh audio backlog, queue lớn hơn (8 items) cho UI để xử lý nhiều button events.

---

## 6. TỔNG KẾT

| Tính năng | Trạng thái | Số lượng | Ghi chú |
|-----------|------------|----------|---------|
| **ESP-IDF Timer** | ✅ Đã dùng | 1 | Deferred NVS save (800ms) |
| **FreeRTOS Software Timer** | ✅ Đã dùng | 3 | Button polling (5ms), BLE scan check (30s), Speaker alert throttle (8s) |
| **Mutex** | ✅ Đã dùng | 6 | Config, NVS, Sensor Data, Alert, Audio, Priority Queue |
| **Binary Semaphore** | ✅ Đã dùng | 1 | Button poll signaling |
| **Counting Semaphore** | ❌ Chưa dùng | 0 | Có thể dùng cho resource pool |
| **Event Group** | ✅ Đã dùng | 1 | System initialization sync (5 bits) |
| **Task Notification** | ✅ Đã dùng | 1 | Lightweight signaling demo |
| **Queue** | ✅ Đã dùng | 2 | UI queue (8 items), Speaker queue (1 item) |

---

## 7. KHUYẾN NGHỊ

### Đã sử dụng:
1. ✅ **Event Group**: Synchronize system initialization (tất cả components ready)
2. ✅ **Task Notification**: Demo task sử dụng notification cho lightweight signaling
3. ✅ **Binary Semaphore**: 1 semaphore (button poll signaling)
4. ✅ **FreeRTOS Software Timer**: 3 timers (button polling, BLE scan check, speaker alert throttle)
5. ✅ **Mutex**: 6 mutexes bảo vệ tất cả shared data
6. ✅ **Queue**: 2 queues cho inter-task communication

### Không cần thiết (hiện tại):
- **Counting Semaphore**: Không có use case rõ ràng trong project hiện tại
  - Có thể dùng cho resource pool nếu cần giới hạn số lượng BLE connections đồng thời

---

## 8. CODE QUALITY

### Điểm mạnh về thread safety:
- ✅ Tất cả shared data đã được bảo vệ bằng mutex (6 mutexes)
- ✅ Thread-safe operations được implement đúng cách
- ✅ Sử dụng ESP-IDF timer hợp lý cho deferred operations
- ✅ Alert throttling với timer và mutex để tránh spam
- ✅ Priority queue với mutex để xử lý alerts theo thứ tự ưu tiên
- ✅ ISR-safe signaling với binary semaphore từ timer callback

---

**Ngày review**: $(date)
**Reviewer**: Auto (AI Assistant)


/**
 * @file key_service.c
 * @brief 按键服务：扫描去抖 + 事件发布 (CMSIS-RTOS v2)
 * @version 4.1
 */

#include "key_service.h"
/* 静态分配类型（StaticTask_t / StaticQueue_t / StaticSemaphore_t / StaticTimer_t）
 * 定义在 FreeRTOS.h 中，CMSIS-RTOS2 头文件不转出。 */
#include "FreeRTOS.h"

#include "app_config.h"
#include "log.h"
#include "app_handles.h"
#include "bsp_key.h"
#include "gpio.h"

/* BSP 句柄 */
static key_event_cb_t s_key_cb  = NULL;

void key_service_set_handler(key_event_cb_t cb) {
    s_key_cb = cb;
}

/* 键盘硬件配置 */
static const bsp_key_row_t key_rows[] = {
    {GPIO_PIN_9, {GPIO_PIN_8, GPIO_PIN_3, GPIO_PIN_5, GPIO_PIN_4, GPIO_PIN_15}, {1, 2, 3, 4, 5}},
    {GPIO_PIN_8, {GPIO_PIN_3, GPIO_PIN_5, GPIO_PIN_4, GPIO_PIN_15, 0}, {6, 7, 8, 9, 0}},
    {GPIO_PIN_3, {GPIO_PIN_5, GPIO_PIN_4, GPIO_PIN_15, 0, 0}, {12, 10, 11, 0, 0}},
    {GPIO_PIN_5, {GPIO_PIN_4, GPIO_PIN_15, 0, 0, 0}, {14, 13, 0, 0, 0}},
    {GPIO_PIN_4, {GPIO_PIN_15, 0, 0, 0, 0}, {15, 0, 0, 0, 0}},
};

static const bsp_key_power_pin_t power_pin = {
    .port      = POWER_DC_GPIO_Port,
    .pin       = POWER_DC_Pin,
    .key_value = 18,
};

/* 扫描状态 */
static uint8_t s_key_before   = 0; /* 上一帧的「已确认」键值，用于边沿判断 */
static uint8_t s_key_state    = 0; /* 0=未长按, 1=已上报长按 */
static uint16_t s_key_counter = 0;

/* 去抖：候选值 + 连续命中计数 */
static uint8_t s_key_candidate  = 0;
static uint8_t s_key_stable_cnt = 0;

/* 按键扫描任务 */
static void key_scan_task(void* arg) {
    (void)arg;
    uint32_t period_ticks = CMSIS_TICKS(KEY_SCAN_INTERVAL_MS);
    uint32_t last         = osKernelGetTickCount();

    for (;;) {
        uint8_t raw = 0;
        if (bsp_key_is_power_pressed()) {
            raw = 18;
        } else {
            bsp_key_scan(&raw);
        }

        /* ---- 去抖：连续 KEY_DEBOUNCE_FRAMES 帧读到同一值才更新「已确认」值 ----
         * 未达确认帧数时沿用上一次确认值，因此抖动不会产生边沿。 */
        if (raw == s_key_candidate) {
            if (s_key_stable_cnt < KEY_DEBOUNCE_FRAMES) {
                s_key_stable_cnt++;
            }
        } else {
            s_key_candidate  = raw;
            s_key_stable_cnt = 1;
        }

        uint8_t now = s_key_before;
        if (s_key_stable_cnt >= KEY_DEBOUNCE_FRAMES) {
            now = s_key_candidate;
        }

        /* ---- 边沿识别（基于已确认值） ---- */
        if (s_key_before == 0 && now != 0) {
            s_key_counter = 0;
            s_key_state   = 0;
        } else if (s_key_before != 0 && now == 0) {
            if (s_key_state == 0 && s_key_counter < KEY_LONG_PRESS_TICKS) {
                if (s_key_cb) {
                    s_key_cb(s_key_before, KEY_EVENT_SHORT_PRESS);
                }
            }
            s_key_counter = 0;
        } else if (now != 0) {
            s_key_counter++;
            if (s_key_counter >= KEY_LONG_PRESS_TICKS && s_key_state == 0) {
                s_key_state = 1;
                if (s_key_cb) {
                    s_key_cb(now, KEY_EVENT_LONG_PRESS);
                }
            }
        }

        s_key_before = now;
        last += period_ticks;
        osDelayUntil(last);
    }
}

/* 静态资源（避免 heap_4 分配：堆从 8KB 降到 1KB，且无运行期失败路径） */
static StaticTask_t s_key_tcb;
static StackType_t s_key_stack[TASK_KEY_STACK / sizeof(StackType_t)];

/* 初始化 */
void key_service_init(void) {
    bsp_key_config_t cfg = {
        .rows      = key_rows,
        .row_count = BSP_ARRAY_SIZE(key_rows),
        .power_pin = &power_pin,
        .row_port  = GPIOB,
    };
    bsp_key_init(&cfg);

    const osThreadAttr_t attr = {
        .name       = TASK_KEY_NAME,
        .cb_mem     = &s_key_tcb,
        .cb_size    = sizeof(s_key_tcb),
        .stack_mem  = s_key_stack,
        .stack_size = sizeof(s_key_stack),
        .priority   = (osPriority_t)TASK_KEY_PRIORITY,
    };
    (void)osThreadNew(key_scan_task, NULL, &attr);
}

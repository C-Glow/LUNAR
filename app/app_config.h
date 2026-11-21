/**
 * @file app_config.h
 * @brief 应用级配置：跨层共享的可调参数
 *
 * 配置分层：硬件私有参数定义在各 BSP 头文件；Modbus 帧内常量定义在
 * modbus_slave.c 内部。同一参数只允许在一处定义。
 */

#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <stdint.h>

#include "cmsis_os2.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CMSIS_TICKS(ms) (((uint32_t)(ms) * osKernelGetTickFreq()) / 1000U)

/* 任务优先级。CMSIS-RTOS2 的 osPriority_t 数值被 FreeRTOS 直接用作任务优先级，
 * 合法区间 1..configMAX_PRIORITIES(56)，越界时 osThreadNew 返回 NULL。 */
#define TASK_PRIO_DEFAULT 1
#define TASK_PRIO_KEY 2
#define TASK_PRIO_HEAT 3
#define TASK_PRIO_EVT 4
#define TASK_PRIO_BLE 5

#define TASK_BLE_NAME "ble_task"
/* 需容纳协议响应缓冲与日志格式化的 128B 栈缓冲 */
#define TASK_BLE_STACK 640
#define TASK_BLE_PRIORITY TASK_PRIO_BLE

#define TASK_HEAT_NAME "heat_task"
#define TASK_HEAT_STACK 512
#define TASK_HEAT_PRIORITY TASK_PRIO_HEAT

#define TASK_KEY_NAME "key_scan"
#define TASK_KEY_STACK 256
#define TASK_KEY_PRIORITY TASK_PRIO_KEY

/* 加热控制 */
#define HEAT_CONTROL_PERIOD_MS 500
#define HEAT_MAX_TIMER_MIN 720
#define HEAT_NTC_FAIL_THRESHOLD 3
#define HEAT_BUZZER_BEEP_COUNT 5
#define HEAT_MUTEX_TIMEOUT_MS 50
#define HEAT_QUEUE_LEN 5

/* PID */
#define HEAT_PID_KP 20.0f
#define HEAT_PID_KI 3.0f
#define HEAT_PID_KD 1.0f
#define HEAT_PID_DEADBAND 1.0f
#define HEAT_PID_OUT_MIN 0
#define HEAT_PID_OUT_MAX 100
#define HEAT_PID_INTEGRAL_LIMIT 30.0f
#define HEAT_PID_D_FILTER 0.1f

/* 按键 */
#define KEY_SCAN_INTERVAL_MS 20
#define KEY_LONG_PRESS_MS 2000
#define KEY_LONG_PRESS_TICKS (KEY_LONG_PRESS_MS / KEY_SCAN_INTERVAL_MS)
/* 连续 N 帧读到同一键值才确认，覆盖 5~20ms 的机械抖动 */
#define KEY_DEBOUNCE_FRAMES 2

/* Modbus（protocol 层读取） */
#define MODBUS_SLAVE_ADDR 0x01

/* AT 命令 */
#define AT_DEFAULT_TIMEOUT_MS 2000
#define AT_BT_NAME "LUNAR"
#define AT_BLE_NAME "LUNAR_BLE"

#ifdef __cplusplus
}
#endif

#endif /* APP_CONFIG_H */

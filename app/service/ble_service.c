/**
 * @file ble_service.c
 * @brief BLE服务：帧分发 + AT命令 (CMSIS-RTOS v2)
 * @version 4.1
 */

#include "ble_service.h"
/* 静态分配类型（StaticTask_t / StaticQueue_t / StaticSemaphore_t / StaticTimer_t）
 * 定义在 FreeRTOS.h 中，CMSIS-RTOS2 头文件不转出。 */
#include "FreeRTOS.h"

#include "app_config.h"
#include "log.h"
#include "app_handles.h"
#include "bsp_bt401.h"
#include "at_parser.h"
#include "bsp_led.h"

#include <string.h>

#ifndef AT_BT_NAME
#define AT_BT_NAME "LUNAR"
#endif
#ifndef AT_BLE_NAME
#define AT_BLE_NAME "LUNAR_BLE"
#endif

/* 无帧时的等待上限：给喂狗留出节拍，也让统计日志有机会输出 */
#define BLE_RX_WAIT_MS 1000U

/* 帧统计仅用于周期性诊断日志；日志关闭时整块剔除，避免只写不读的死变量 */
#if LOG_IS_ENABLED
typedef struct {
    uint32_t total, modbus, at, unknown;
} frame_stats_t;

static frame_stats_t s_stats;
#endif

static ble_frame_cb_t s_frame_cb = NULL;

void ble_service_set_frame_handler(ble_frame_cb_t cb) {
    s_frame_cb = cb;
}

/* ---- 静态资源 ---- */
static StaticTask_t s_ble_tcb;
static StackType_t s_ble_stack[TASK_BLE_STACK / sizeof(StackType_t)];

/* ---- BLE帧处理任务 ---- */
static void ble_task(void* arg) {
    (void)arg;
    /* 130B 的帧对象移出任务栈：栈上再叠加协议层缓冲会溢出 512B */
    static bsp_bt401_frame_t f;

    bt_start();

    for (;;) {
#ifdef HAL_IWDG_MODULE_ENABLED
        HAL_IWDG_Refresh(&hiwdg);
#endif
        /* 信号量驱动：有帧立即处理，无帧阻塞休眠。
         * 原 osDelay(20) 轮询会引入 0~20ms 随机响应延迟，且空转耗 CPU。 */
        if (!bsp_bt401_wait_frame(BLE_RX_WAIT_MS)) {
            continue;
        }

        while (bsp_bt401_get_frame(&f) == BSP_OK) {
            bool is_modbus =
                (f.len >= 5 && f.data[0] == 0x01 && (f.data[1] == 0x03 || f.data[1] == 0x06 || f.data[1] == 0x10));
            bool is_at = (!is_modbus && f.len >= 2 && f.data[f.len - 2] == '\r' && f.data[f.len - 1] == '\n');

#if LOG_IS_ENABLED
            s_stats.total++;
            if (is_modbus) {
                s_stats.modbus++;
            } else if (is_at) {
                s_stats.at++;
            } else {
                s_stats.unknown++;
            }
#endif

            if (s_frame_cb && (is_modbus || is_at)) {
                s_frame_cb(f.data, f.len, is_modbus);
            }
            f.len = 0;
        }

#if LOG_IS_ENABLED
        if ((s_stats.total % 60000) == 59999) {
            LOG_PRINTF("BLE: F=%lu M=%lu A=%lu U=%lu Ov=%lu Dp=%lu Free=%u\r\n", s_stats.total, s_stats.modbus,
                       s_stats.at, s_stats.unknown, bsp_bt401_get_overrun(), bsp_bt401_get_tx_drop(),
                       osThreadGetStackSpace(osThreadGetId()));
        }
#endif
    }
}

/* AT 命令执行（原在 at_parser.c，含硬件操作，属 BSP 职责，不应留在 protocol 层） */
static bool s_at_blink = false;

void ble_handle_at_frame(const uint8_t* data, uint16_t len) {
    if (!data || len < 4) {
        return;
    }
    /* 去掉尾部 

 后交给纯解析器 */
    at_parse_result_t r = at_parse_frame(data, (uint16_t)(len - 2));

    switch (r.cmd) {
    case AT_CMD_QM_OFF:
        bsp_led_set_mode(BSP_LED_BT, BSP_LED_MODE_OFF, 0);
        bsp_led_set_mode(BSP_LED_MUSIC, BSP_LED_MODE_OFF, 0);
        s_at_blink = false;
        break;
    case AT_CMD_QM_MUSIC:
        bsp_led_set_mode(BSP_LED_MUSIC, BSP_LED_MODE_ON, 0);
        bsp_led_set_mode(BSP_LED_BT, BSP_LED_MODE_OFF, 0);
        s_at_blink = false;
        break;
    case AT_CMD_TS_BLINK:
        bsp_led_set_mode(BSP_LED_BT, BSP_LED_MODE_BLINK, 500);
        s_at_blink = true;
        break;
    case AT_CMD_TS_ON:
        bsp_led_set_mode(BSP_LED_BT, BSP_LED_MODE_ON, 0);
        s_at_blink = false;
        break;
    default:
        break;
    }
}

/* ---- AT命令接口 ---- */
void bt_start(void) {
    bsp_bt401_printf("AT+BD%s\r\n", AT_BT_NAME);
    osDelay(100);
    bsp_bt401_printf("AT+BM%s\r\n", AT_BLE_NAME);
    osDelay(100);
    bsp_bt401_printf("AT+CG01\r\n");
    osDelay(50);
    bsp_bt401_printf("AT+CK00\r\n");
    osDelay(50);
    bsp_bt401_printf("AT+B200\r\n");
    osDelay(50);
    bsp_bt401_printf("AT+CR00\r\n");
    osDelay(50);
    bsp_bt401_printf("AT+CN00\r\n");
    osDelay(50);
    ble_mode(BT_MODE_BT);
    osDelay(50);
}

void music_switch(void) {
    bsp_bt401_printf("AT+CB\r\n");
}
void music_next(void) {
    bsp_bt401_printf("AT+CC\r\n");
}
void music_prev(void) {
    bsp_bt401_printf("AT+CD\r\n");
}

void music_volume_control(volume_dir_t dir) {
    if (dir == VOL_DIR_UP) {
        bsp_bt401_printf("AT+CE\r\n");
    }
    if (dir == VOL_DIR_DOWN) {
        bsp_bt401_printf("AT+CF\r\n");
    }
}

void music_volume_set(uint8_t vol) {
    if (vol <= 30) {
        bsp_bt401_printf("AT+CA%02d\r\n", vol);
    }
}

void ble_mode(bt_mode_t mode) {
    const char* c = NULL;
    switch (mode) {
    case BT_MODE_OFF:
        c = "AT+CM08\r\n";
        bsp_led_set_mode(BSP_LED_BT, BSP_LED_MODE_OFF, 0);
        bsp_led_set_mode(BSP_LED_MUSIC, BSP_LED_MODE_OFF, 0);
        break;
    case BT_MODE_BT:
        c = "AT+CM01\r\n";
        bsp_led_set_mode(BSP_LED_BT, BSP_LED_MODE_ON, 0);
        break;
    case BT_MODE_MUSIC:
        c = "AT+CM04\r\n";
        bsp_led_set_mode(BSP_LED_MUSIC, BSP_LED_MODE_ON, 0);
        break;
    default:
        return;
    }
    bsp_bt401_printf(c);
}

void ble_query(void) {
    bsp_bt401_printf("AT+QM?\r\n");
    osDelay(50);
    bsp_bt401_printf("AT+TS\r\n");
    osDelay(50);
}

void ble_service_init(void) {
    const osThreadAttr_t attr = {
        .name       = TASK_BLE_NAME,
        .cb_mem     = &s_ble_tcb,
        .cb_size    = sizeof(s_ble_tcb),
        .stack_mem  = s_ble_stack,
        .stack_size = sizeof(s_ble_stack),
        .priority   = (osPriority_t)TASK_BLE_PRIORITY,
    };
    (void)osThreadNew(ble_task, NULL, &attr);
}

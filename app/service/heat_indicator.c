/**
 * @file heat_indicator.c
 * @brief 加热指示输出实现
 */

#include "heat_indicator.h"

/* 静态分配类型定义在 FreeRTOS.h，CMSIS-RTOS2 头文件不转出 */
#include "FreeRTOS.h"

#include "app_config.h"
#include "app_handles.h"
#include "bsp_buzzer.h"
#include "bsp_led.h"

/* 静态分配的蜂鸣器定时器 */
static osTimerId_t s_beep_timer = NULL;
static StaticTimer_t s_beep_timer_cb_mem;

static void beep_timer_cb(void* arg);

static osTimerId_t beep_timer_create(void) {
    const osTimerAttr_t attr = {
        .name    = "beep",
        .cb_mem  = &s_beep_timer_cb_mem,
        .cb_size = sizeof(s_beep_timer_cb_mem),
    };
    return osTimerNew(beep_timer_cb, osTimerOnce, NULL, &attr);
}

static void beep_timer_cb(void* arg) {
    (void)arg;
    bsp_buzzer_off();
}

void heat_indicator_init(void) {
    if (!s_beep_timer) {
        s_beep_timer = beep_timer_create();
    }
}

void heat_indicator_beep(uint32_t duration_ms) {
    if (!s_beep_timer) {
        s_beep_timer = beep_timer_create();
        if (!s_beep_timer) {
            return;
        }
    } else if (osTimerIsRunning(s_beep_timer)) {
        osTimerStop(s_beep_timer);
    }
    bsp_buzzer_on();
    (void)osTimerStart(s_beep_timer, CMSIS_TICKS(duration_ms));
}

void heat_indicator_set_remain(uint32_t remain_sec) {

    static const uint32_t SEC_10 = 10U * 60U;
    static const uint32_t SEC_30 = 30U * 60U;

    /* 先全灭再点亮，保证只亮一只 */
    bsp_led_set_mode(BSP_LED_10MIN, BSP_LED_MODE_OFF, 0);
    bsp_led_set_mode(BSP_LED_30MIN, BSP_LED_MODE_OFF, 0);
    bsp_led_set_mode(BSP_LED_60MIN, BSP_LED_MODE_OFF, 0);

    if (remain_sec > SEC_30) {
        bsp_led_set_mode(BSP_LED_60MIN, BSP_LED_MODE_ON, 0);
    } else if (remain_sec > SEC_10) {
        bsp_led_set_mode(BSP_LED_30MIN, BSP_LED_MODE_ON, 0);
    } else if (remain_sec > 0) {
        bsp_led_set_mode(BSP_LED_10MIN, BSP_LED_MODE_ON, 0);
    }
}

void heat_indicator_set_heating(bool on) {
    bsp_led_set_mode(BSP_LED_RF, on ? BSP_LED_MODE_ON : BSP_LED_MODE_OFF, 0);
}

void heat_indicator_all_off(void) {
    bsp_buzzer_off();
    if (s_beep_timer && osTimerIsRunning(s_beep_timer)) {
        osTimerStop(s_beep_timer);
    }
    bsp_led_set_mode(BSP_LED_10MIN, BSP_LED_MODE_OFF, 0);
    bsp_led_set_mode(BSP_LED_30MIN, BSP_LED_MODE_OFF, 0);
    bsp_led_set_mode(BSP_LED_60MIN, BSP_LED_MODE_OFF, 0);
    bsp_led_set_mode(BSP_LED_RF, BSP_LED_MODE_OFF, 0);
}

/**
 * @file app_main.c
 * @brief 应用编排：初始化 BSP、注册跨层回调、注入协议层端口
 */

#include "app_config.h"
#include "log.h"
#include "app_handles.h"
#include "gpio.h"

#include <string.h>

#if LOG_IS_ENABLED
#include <stdarg.h>
#include <stdio.h>
#endif

/* BSP */
#include "bsp_bt401.h"
#include "bsp_buzzer.h"
#include "bsp_heat.h"
#include "bsp_key.h"
#include "bsp_led.h"
#include "bsp_ntc.h"
#include "bsp_rtc.h"

/* 服务层接口 */
#include "service/ble_service.h"
#include "service/heat_service.h"
#include "service/key_service.h"

/* 协议层接口 */
#include "at_parser.h"
#include "modbus_slave.h"


/* 日志唯一出口；日志关闭时整段不参与编译 */
#if LOG_IS_ENABLED
static int log_writer_to_bt401(const char* fmt, ...) {
    char buf[128];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len < 0) {
        return -1;
    }
    if ((size_t)len >= sizeof(buf)) {
        len = (int)sizeof(buf) - 1;
    }
    return (bsp_bt401_send((const uint8_t*)buf, (uint16_t)len) == BSP_OK) ? len : 0;
}
#endif /* LOG_IS_ENABLED */

/** LED 硬件配置表，顺序对应 bsp_led_index_t */
static const bsp_led_hw_t led_hw_table[BSP_LED_COUNT] = {
    [BSP_LED_MUSIC] = {LED4_GPIO_Port, LED4_Pin, GPIO_PIN_RESET},
    [BSP_LED_BT]    = {LED3_GPIO_Port, LED3_Pin, GPIO_PIN_RESET},
    [BSP_LED_10MIN] = {LED6_GPIO_Port, LED6_Pin, GPIO_PIN_RESET},
    [BSP_LED_30MIN] = {LED5_GPIO_Port, LED5_Pin, GPIO_PIN_RESET},
    [BSP_LED_60MIN] = {LED2_GPIO_Port, LED2_Pin, GPIO_PIN_RESET},
    [BSP_LED_RF]    = {LED1_GPIO_Port, LED1_Pin, GPIO_PIN_RESET},
    [BSP_LED_B]     = {LED_B_GPIO_Port, LED_B_Pin, GPIO_PIN_RESET},
};

/* 外设初始化失败时点亮蓝灯常亮作为停机指示，并停止后续初始化。
 * 加热设备必须保证：任一关键外设（尤其 NTC / PWM）初始化失败时不得进入工作状态。 */
static bool bsp_all_init(void) {
    bsp_led_config_t led_cfg = {
        .hw_table = led_hw_table,
        .count    = BSP_LED_COUNT,
    };
    if (bsp_led_init(&led_cfg) != BSP_OK) {
        return false;
    }
    bsp_led_set_mode(BSP_LED_B, BSP_LED_MODE_ON, 0); /* 默认亮蓝灯 */

    bsp_buzzer_config_t buzzer_cfg = {
        .port         = BEEP_GPIO_Port,
        .pin          = BEEP_Pin,
        .active_level = GPIO_PIN_SET,
    };
    if (bsp_buzzer_init(&buzzer_cfg) != BSP_OK) {
        return false;
    }

    extern UART_HandleTypeDef huart3;
    bsp_bt401_config_t bt401_cfg = {.huart = &huart3};
    if (bsp_bt401_init(&bt401_cfg) != BSP_OK) {
        return false;
    }

    extern TIM_HandleTypeDef htim1;
    bsp_heat_config_t heat_cfg = {
        .htim       = &htim1,
        .channel    = TIM_CHANNEL_4,
        .freq_hz    = 1000,
        .resolution = (uint32_t)htim1.Init.Period + 1,
    };
    if (bsp_heat_init(&heat_cfg) != BSP_OK) {
        return false;
    }

    extern ADC_HandleTypeDef hadc1;
    bsp_ntc_config_t ntc_cfg = {.hadc = &hadc1, .temp_offset = 0.0f};
    if (bsp_ntc_init(&ntc_cfg) != BSP_OK) {
        return false;
    }

    extern RTC_HandleTypeDef hrtc;
    bsp_rtc_config_t rtc_cfg = {.hrtc = &hrtc};
    if (bsp_rtc_init(&rtc_cfg) != BSP_OK) {
        return false;
    }

    return true;
}

static void on_ble_frame(const uint8_t* data, uint16_t len, bool is_modbus) {
    if (is_modbus) {
        protocol_handle_request(data, len);
    } else {
        ble_handle_at_frame(data, len);
    }
}

static void on_heat_upload(void) {
    protocol_upload_heating_status();
}

static void on_key_event(uint8_t key_id, key_event_t event) {
    if (event == KEY_EVENT_SHORT_PRESS) {
        switch (key_id) {
        case KEY_MUSIC:
            ble_mode(BT_MODE_MUSIC);
            break;
        case KEY_BLUETOOTH:
            ble_mode(BT_MODE_BT);
            break;
        case KEY_PLAY_PAUSE:
            music_switch();
            break;
        case KEY_PREV:
            music_prev();
            break;
        case KEY_NEXT:
            music_next();
            break;
        case KEY_VOL_DOWN:
            music_volume_control(VOL_DIR_DOWN);
            break;
        case KEY_VOL_UP:
            music_volume_control(VOL_DIR_UP);
            break;
        case KEY_HEAT:
            heat_status_switch();
            break;
        case KEY_HEAT_PLUS:
            heat_level_down();
            break;
        case KEY_HEAT_MINUS:
            heat_level_up();
            break;
        case KEY_MIN10:
            heat_timer_set(10);
            break;
        case KEY_MIN30:
            heat_timer_set(30);
            break;
        case KEY_MIN60:
            heat_timer_set(60);
            break;
        default:
            break;
        }
    } else if (event == KEY_EVENT_LONG_PRESS) {
        switch (key_id) {
        case KEY_MUSIC:
        case KEY_BLUETOOTH:
            ble_mode(BT_MODE_OFF);
            break;
        case KEY_MIN10:
        case KEY_MIN30:
        case KEY_MIN60:
            heat_timer_set(0);
            break;
        default:
            break;
        }
    }
}

/* 协议层端口：协议层通过这些函数访问硬件 */
static void port_bt401_tx(const uint8_t* buf, uint16_t len) {
    if (buf && len) {
        (void)bsp_bt401_send(buf, len);
    }
}

static bool port_rtc_set_utc(uint32_t utc) {
    return bsp_rtc_set_utc(utc) == BSP_OK;
}

/* 前向声明：Modbus 关机命令会调用 */
static void do_power_off(void);

static void on_reg_write(RegisterID reg, uint16_t value) {
    switch (reg) {
    case REG_HEATING_STATUS:
        heat_status_set(value ? HEAT_STATUS_RUNNING : HEAT_STATUS_STOP);
        break;
    case REG_HEATING_LEVEL:
        heat_level_set((HeatLevel)value);
        break;
    case REG_HEATING_TIMER:
        heat_timer_set(value);
        break;
    case REG_POWER_SWITCH:
        if (value) {
            do_power_off();
        }
        break;
    default:
        break;
    }
}

static uint16_t on_reg_read(RegisterID reg) {
    switch (reg) {
    case REG_HEATING_STATUS:
        return (uint16_t)heat_status_get();
    case REG_HEATING_LEVEL:
        return (uint16_t)heat_level_get();
    case REG_HEATING_TIMER:
        return heat_remain_time_get();
    /* 本回调在持锁上下文中执行，只能用不加锁接口 */
    case REG_SHORTCUT_KEY1:
    case REG_SHORTCUT_KEY2:
        return register_read_unlocked(reg);
    default:
        return register_read_unlocked(reg);
    }
}

/* 关机：停加热并关闭蓝牙、指示灯 */
static void do_power_off(void) {

    heat_status_set(HEAT_STATUS_STOP);
    ble_mode(BT_MODE_OFF);
    bsp_led_set_mode(BSP_LED_10MIN, BSP_LED_MODE_OFF, 0);
    bsp_led_set_mode(BSP_LED_30MIN, BSP_LED_MODE_OFF, 0);
    bsp_led_set_mode(BSP_LED_60MIN, BSP_LED_MODE_OFF, 0);
    bsp_led_set_mode(BSP_LED_RF, BSP_LED_MODE_OFF, 0);
}

/* 加热状态变化：上报协议层 */
static void on_heat_state(HeatStatus status) {
#if LOG_IS_ENABLED
    LOG_PRINTF("EVT: heating status -> %d\r\n", (int)status);
#else
    (void)status; /* 日志关闭时该参数仅用于协议上传，无需读取 */
#endif

    protocol_upload_heating_status();
}

void task_init(void) {
    /* 外设初始化失败则不进入工作状态：保持蓝灯常亮，不创建任务 */
    if (!bsp_all_init()) {
        /* 蓝灯保持常亮作为故障指示 */
        bsp_led_set_mode(BSP_LED_B, BSP_LED_MODE_ON, 0);
        return;
    }
    LOG_SET_WRITER(log_writer_to_bt401);

    /* 初始化服务层 */
    key_service_init();
    heat_task_init();
    ble_service_init();

    /* 注册层间回调（app_main 是本工程唯一的跨层编排点） */
    key_service_set_handler(on_key_event);
    ble_service_set_frame_handler(on_ble_frame);
    heat_service_set_state_handler(on_heat_state);
    heat_service_set_upload_handler(on_heat_upload);
    protocol_init();
    /* 依赖注入：把硬件能力交给协议层，而非让协议层直接调 BSP */
    protocol_set_tx(port_bt401_tx);
    protocol_set_rtc_writer(port_rtc_set_utc);
    protocol_register_write_callback(on_reg_write);
    protocol_register_read_callback(on_reg_read);
}

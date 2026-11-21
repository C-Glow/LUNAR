/**
 * @file heat_control.h
 * @brief 加热状态机与 PID 温控（纯逻辑，不依赖 BSP 与 RTOS）
 *
 * 所有外部副作用通过 heat_output_t 返回给调用方执行，
 * 因此本模块可在主机上完整测试。
 */

#ifndef HEAT_CONTROL_H
#define HEAT_CONTROL_H

#include <stdbool.h>
#include <stdint.h>

#include "pid.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HEAT_STATUS_STOP    = 0,
    HEAT_STATUS_RUNNING = 1
} HeatStatus;

typedef enum {
    HEAT_LEVEL_1 = 0,
    HEAT_LEVEL_2,
    HEAT_LEVEL_3,
    HEAT_LEVEL_MAX = HEAT_LEVEL_3
} HeatLevel;

typedef enum {
    HEAT_CMD_SET_STATUS = 0,
    HEAT_CMD_SET_LEVEL,
    HEAT_CMD_SET_TIMER,
    HEAT_CMD_TICK_1S,
    HEAT_CMD_NTC_FAULT,
    HEAT_CMD_NTC_READ_FAIL,
    HEAT_CMD_CLEAR_FAULT,
} heat_cmd_type_t;

typedef struct {
    heat_cmd_type_t type;
    union {
        HeatStatus status;
        HeatLevel level;
        uint16_t minute;
    } param;
} heat_cmd_t;

/** 一个控制周期的输出快照，由调用方翻译成实际硬件操作 */
typedef struct {
    HeatStatus status;
    uint8_t power_percent; /**< 0-100，非 RUNNING 时恒为 0 */
    uint16_t remain_sec;   /**< 剩余定时秒数，0 表示无定时 */
    HeatLevel level;
    bool buzzer_beep;      /**< 本周期需要蜂鸣 */
    bool fault_active;     /**< NTC 故障中 */
    bool status_changed;   /**< 状态与上周期不同 */
    bool last_status;
} heat_output_t;

/** 只读状态快照 */
typedef struct {
    HeatStatus status;
    HeatLevel level;
    uint16_t remain_sec;
} heat_state_t;

typedef struct {
    float pid_kp;
    float pid_ki;
    float pid_kd;
    float pid_deadband;
    uint16_t pid_out_min;
    uint16_t pid_out_max;
    float pid_integral_limit;
    float pid_d_filter;
    uint16_t max_timer_min;
    uint8_t ntc_fail_threshold;
    float level_temp[HEAT_LEVEL_MAX + 1];
} heat_control_cfg_t;

/** 初始化；cfg 传 NULL 时使用内置默认参数 */
void heat_control_init(const heat_control_cfg_t* cfg);

/** 投递控制指令。不做加锁，由调用方队列序列化 */
bool heat_control_send(const heat_cmd_t* cmd);

/** 是否有待处理指令 */
bool heat_control_has_pending(void);

/**
 * @brief 执行一个控制周期：消费全部待处理指令后计算输出
 * @param ms_since_last  距上次调用的毫秒数
 * @param temperature_c  当前温度(℃)
 * @param temp_valid     温度是否有效；false 时不驱动加热器
 * @param out            输出快照
 * @return false 表示未初始化或参数为空
 */
bool heat_control_step(uint32_t ms_since_last, float temperature_c, bool temp_valid, heat_output_t* out);

/** 读取当前状态 */
void heat_control_snapshot(heat_state_t* out);

#ifdef __cplusplus
}
#endif

#endif /* HEAT_CONTROL_H */

/**
 * @file heat_control.c
 * @brief 加热状态机与 PID 温控
 *
 * 全部状态由 s_ctl 单一持有，输出只由 heat_control_step() 决定：
 * 只要 status 不是 RUNNING，power_percent 必为 0。
 */

#include "heat_control.h"

#include <string.h>

#define HEAT_CMD_QUEUE_LEN 8

typedef struct {
    heat_control_cfg_t cfg;

    /* 单写者：heat_service 任务 */
    HeatStatus status;
    HeatLevel level;
    uint16_t remain_sec;
    uint16_t set_time_min;
    uint8_t ntc_fail_count;
    bool fault_active;
    bool last_reported_status;

    /* PID */
    pid_controller_t pid;
    uint32_t pid_time_ms;

    /* 环形指令队列 */
    heat_cmd_t queue[HEAT_CMD_QUEUE_LEN];
    uint8_t q_head;
    uint8_t q_tail;

    bool inited;
} heat_control_t;

static heat_control_t s_ctl;

static bool queue_empty(void) {
    return s_ctl.q_head == s_ctl.q_tail;
}

static bool queue_push(const heat_cmd_t* cmd) {
    uint8_t next = (uint8_t)((s_ctl.q_head + 1U) % HEAT_CMD_QUEUE_LEN);
    if (next == s_ctl.q_tail) {
        return false; /* 队列已满，保留已有指令 */
    }
    s_ctl.queue[s_ctl.q_head] = *cmd;
    s_ctl.q_head               = next;
    return true;
}

static bool queue_pop(heat_cmd_t* cmd) {
    if (queue_empty()) {
        return false;
    }
    *cmd        = s_ctl.queue[s_ctl.q_tail];
    s_ctl.q_tail = (uint8_t)((s_ctl.q_tail + 1U) % HEAT_CMD_QUEUE_LEN);
    return true;
}

static float level_to_temp(HeatLevel level) {
    if (level > HEAT_LEVEL_MAX) {
        level = HEAT_LEVEL_1;
    }
    return s_ctl.cfg.level_temp[level];
}

/** 幂等关断 */
static void stop_locked(void) {
    s_ctl.status         = HEAT_STATUS_STOP;
    s_ctl.remain_sec     = 0;
    s_ctl.set_time_min   = 0;
    s_ctl.ntc_fail_count = 0;
    pid_reset(&s_ctl.pid);
}

static void apply_cmd(const heat_cmd_t* cmd, heat_output_t* out) {
    switch (cmd->type) {
    case HEAT_CMD_SET_STATUS:
        if (cmd->param.status == HEAT_STATUS_STOP) {
            if (s_ctl.status == HEAT_STATUS_RUNNING) {
                stop_locked();
                out->buzzer_beep = false;
            }
        } else {
            if (s_ctl.status != HEAT_STATUS_RUNNING) {
                s_ctl.status = HEAT_STATUS_RUNNING;
                /* 若有定时，从设置值重新计时 */
                if (s_ctl.set_time_min > 0) {
                    s_ctl.remain_sec = (uint16_t)(s_ctl.set_time_min * 60U);
                }
                pid_reset(&s_ctl.pid);
            }
        }
        break;

    case HEAT_CMD_SET_LEVEL: {
        HeatLevel lv = cmd->param.level;
        if (lv > HEAT_LEVEL_MAX) {
            break;
        }
        if (lv != s_ctl.level) {
            s_ctl.level = lv;
            pid_reset(&s_ctl.pid); /* 目标跳变，清积分避免超调 */
        }
        break;
    }

    case HEAT_CMD_SET_TIMER: {
        uint16_t minute = cmd->param.minute;
        if (minute > s_ctl.cfg.max_timer_min) {
            break;
        }
        s_ctl.set_time_min = minute;
        s_ctl.remain_sec   = (uint16_t)(minute * 60U);
        break;
    }

    case HEAT_CMD_TICK_1S:
        if (s_ctl.status == HEAT_STATUS_RUNNING && s_ctl.remain_sec > 0) {
            s_ctl.remain_sec--;
            if (s_ctl.remain_sec == 0) {

                stop_locked();
                out->buzzer_beep = true;
            }
        }
        break;

    case HEAT_CMD_NTC_FAULT:
        s_ctl.fault_active  = true;
        s_ctl.ntc_fail_count = 0;
        stop_locked();
        break;

    case HEAT_CMD_NTC_READ_FAIL:
        s_ctl.ntc_fail_count++;
        if (s_ctl.ntc_fail_count >= s_ctl.cfg.ntc_fail_threshold) {
            s_ctl.ntc_fail_count = 0;
            s_ctl.fault_active   = true;
            stop_locked();
        }
        break;

    case HEAT_CMD_CLEAR_FAULT:
        s_ctl.fault_active   = false;
        s_ctl.ntc_fail_count = 0;
        break;

    default:
        break;
    }
}

void heat_control_init(const heat_control_cfg_t* cfg) {
    memset(&s_ctl, 0, sizeof(s_ctl));

    if (cfg) {
        s_ctl.cfg = *cfg;
    } else {
        /* 与 app_config.h 保持一致 */
        s_ctl.cfg.pid_kp             = 20.0f;
        s_ctl.cfg.pid_ki             = 3.0f;
        s_ctl.cfg.pid_kd             = 1.0f;
        s_ctl.cfg.pid_deadband       = 1.0f;
        s_ctl.cfg.pid_out_min        = 0;
        s_ctl.cfg.pid_out_max        = 100;
        s_ctl.cfg.pid_integral_limit = 30.0f;
        s_ctl.cfg.pid_d_filter       = 0.1f;
        s_ctl.cfg.max_timer_min      = 720;
        s_ctl.cfg.ntc_fail_threshold = 3;
        s_ctl.cfg.level_temp[0]      = 35.0f;
        s_ctl.cfg.level_temp[1]      = 45.0f;
        s_ctl.cfg.level_temp[2]      = 55.0f;
    }

    s_ctl.status    = HEAT_STATUS_STOP;
    s_ctl.level     = HEAT_LEVEL_1;
    s_ctl.inited    = true;

    pid_init(&s_ctl.pid, s_ctl.cfg.pid_kp, s_ctl.cfg.pid_ki, s_ctl.cfg.pid_kd, s_ctl.cfg.pid_deadband,
             s_ctl.cfg.pid_out_min, s_ctl.cfg.pid_out_max);
    pid_set_integral_params(&s_ctl.pid, s_ctl.cfg.pid_deadband / 2, s_ctl.cfg.pid_integral_limit);
    pid_set_derivative_filter(&s_ctl.pid, s_ctl.cfg.pid_d_filter);
}

bool heat_control_send(const heat_cmd_t* cmd) {
    if (!s_ctl.inited || !cmd) {
        return false;
    }
    return queue_push(cmd);
}

bool heat_control_has_pending(void) {
    return !queue_empty();
}

void heat_control_snapshot(heat_state_t* out) {
    if (!out) {
        return;
    }
    out->status     = s_ctl.status;
    out->level      = s_ctl.level;
    out->remain_sec = s_ctl.remain_sec;
}

bool heat_control_step(uint32_t ms_since_last, float temperature_c, bool temp_valid, heat_output_t* out) {
    if (!s_ctl.inited || !out) {
        return false;
    }

    heat_output_t o;
    memset(&o, 0, sizeof(o));
    o.last_status = s_ctl.status;

    /* 1. 消费待处理指令 */
    heat_cmd_t cmd;
    while (queue_pop(&cmd)) {
        apply_cmd(&cmd, &o);
    }

    /* 2. 决定功率输出 */
    if (s_ctl.status == HEAT_STATUS_RUNNING && temp_valid) {
        float target = level_to_temp(s_ctl.level);
        s_ctl.pid_time_ms += ms_since_last;
        o.power_percent = (uint8_t)pid_calc_with_time(&s_ctl.pid, temperature_c, target, s_ctl.pid_time_ms);
        if (o.power_percent > 100) {
            o.power_percent = 100;
        }
    } else {

        o.power_percent = 0;
        pid_reset(&s_ctl.pid);
    }

    /* 3. 汇总输出 */
    o.status         = s_ctl.status;
    o.remain_sec     = s_ctl.remain_sec;
    o.level          = s_ctl.level;
    o.fault_active   = s_ctl.fault_active;
    o.status_changed = (o.status != s_ctl.last_reported_status);

    s_ctl.last_reported_status = o.status;

    *out = o;
    return true;
}

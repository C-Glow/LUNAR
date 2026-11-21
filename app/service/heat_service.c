/**
 * @file heat_service.c
 * @brief 加热服务：RTOS 资源管理与硬件输出
 *
 * 状态机与 PID 在 heat_control.c，指示输出在 heat_indicator.c。
 * 本文件只负责创建资源、采样温度、把控制层输出翻译成 BSP 调用。
 */

#include "heat_service.h"

#include <string.h>

/* 静态分配类型定义在 FreeRTOS.h，CMSIS-RTOS2 头文件不转出 */
#include "FreeRTOS.h"

#include "app_config.h"
#include "app_handles.h"
#include "bsp_led.h"
#include "bsp_heat.h"
#include "bsp_ntc.h"
#include "heat_control.h"
#include "heat_indicator.h"
#include "log.h"

typedef struct {
    osMessageQueueId_t cmd_queue;
    osTimerId_t tick_timer;   /* 1 秒节拍：递减剩余时间 */
    osMutexId_t mutex;
    heat_upload_cb_t upload_cb;
    heat_state_cb_t state_cb;
    bool hardware_on;         /* 当前硬件输出状态，用于避免重复调用 */
    uint8_t last_power;
    HeatStatus last_status;
} heat_runtime_t;

static heat_runtime_t s_rt = {
    .last_status = HEAT_STATUS_STOP,
    .last_power  = 0,
};

/* RTOS 对象静态分配 */
static StaticTask_t s_task_tcb;
static StackType_t s_task_stack[TASK_HEAT_STACK / sizeof(StackType_t)];
static StaticQueue_t s_cmd_queue_cb;
static uint8_t s_cmd_queue_buf[HEAT_QUEUE_LEN * sizeof(heat_cmd_t)];
static StaticSemaphore_t s_mutex_cb;
static StaticTimer_t s_tick_timer_cb;

static void tick_timer_cb(void* arg) {
    (void)arg;
    if (!s_rt.cmd_queue) {
        return;
    }
    heat_cmd_t cmd = {.type = HEAT_CMD_TICK_1S};
    (void)osMessageQueuePut(s_rt.cmd_queue, &cmd, 0, 0);
}

static bool post_cmd(const heat_cmd_t* cmd) {
    if (!cmd || !s_rt.cmd_queue) {
        return false;
    }
    return (osMessageQueuePut(s_rt.cmd_queue, cmd, 0, 0) == osOK);
}

static void apply_output(const heat_output_t* out) {
    if (!out) {
        return;
    }

    /* 加热器 */
    if (out->status == HEAT_STATUS_RUNNING) {
        if (!s_rt.hardware_on) {
            bsp_heat_start();
            s_rt.hardware_on = true;
            heat_indicator_set_heating(true);
        }
        if (out->power_percent != s_rt.last_power) {
            bsp_heat_set_power(out->power_percent);
            s_rt.last_power = out->power_percent;
        }
    } else {
        /* 幂等关断 */
        if (s_rt.hardware_on || s_rt.last_power != 0) {
            bsp_heat_set_power(0);
            bsp_heat_stop();
            s_rt.hardware_on = false;
            s_rt.last_power  = 0;
        }
        heat_indicator_set_heating(false);
    }

    /* 指示 */
    heat_indicator_set_remain(out->remain_sec);

    if (out->buzzer_beep) {
        heat_indicator_beep(HEAT_BUZZER_BEEP_COUNT * 100U);
    }

    /* 状态变迁上报 */
    if (out->status_changed) {
        if (s_rt.last_status != out->status) {
            s_rt.last_status = out->status;
            if (s_rt.state_cb) {
                s_rt.state_cb(out->status);
            }
        }
        if (s_rt.upload_cb) {
            s_rt.upload_cb();
        }
    }
}

static void heat_control_task(void* arg) {
    (void)arg;

    uint32_t period_ticks = CMSIS_TICKS(HEAT_CONTROL_PERIOD_MS);
    uint32_t last_wake    = osKernelGetTickCount();
    uint32_t last_ms      = 0;

    for (;;) {
        /* 1. 采样 */
        float temp    = 0.0f;
        bool temp_ok  = false;
        bool ntc_fault = bsp_ntc_is_fault();

        if (ntc_fault) {

            heat_cmd_t fc = {.type = HEAT_CMD_NTC_FAULT};
            (void)post_cmd(&fc);
        } else if (bsp_ntc_read(&temp) == BSP_OK) {
            temp_ok = true;
        } else {
            heat_cmd_t rc = {.type = HEAT_CMD_NTC_READ_FAIL};
            (void)post_cmd(&rc);
        }

        /* 2. 消费指令并计算输出 */
        heat_output_t out;
        memset(&out, 0, sizeof(out));
        if (heat_control_step(last_ms, temp, temp_ok, &out)) {
            apply_output(&out);
        }

        /* 3. 日志（关闭时整段剔除） */
#if LOG_IS_ENABLED
        if (temp_ok && out.status == HEAT_STATUS_RUNNING) {
            int16_t ts = (int16_t)(temp * 10.0f + 0.5f);
            LOG_PRINTF("Temp: %d.%d PWM: %u%%\r\n", ts / 10, ts % 10, out.power_percent);
        }
#endif

        /* 4. 停机期间尝试解除故障锁存 */
        if (ntc_fault) {
            bsp_ntc_clear_fault();
        }

        /* 5. LED 闪烁 */
        bsp_led_poll(HEAT_CONTROL_PERIOD_MS);

        last_wake += period_ticks;
        last_ms = HEAT_CONTROL_PERIOD_MS;
        osDelayUntil(last_wake);
    }
}

static bool create_resources(void) {
    const osMessageQueueAttr_t qattr = {
        .name      = "heat_cmd_q",
        .cb_mem    = &s_cmd_queue_cb,
        .cb_size   = sizeof(s_cmd_queue_cb),
        .mq_mem    = s_cmd_queue_buf,
        .mq_size   = sizeof(s_cmd_queue_buf),
    };
    s_rt.cmd_queue = osMessageQueueNew(HEAT_QUEUE_LEN, sizeof(heat_cmd_t), &qattr);
    if (!s_rt.cmd_queue) {
        return false;
    }

    const osMutexAttr_t mattr = {
        .name      = "heat_mtx",
        .attr_bits = osMutexRecursive,
        .cb_mem    = &s_mutex_cb,
        .cb_size   = sizeof(s_mutex_cb),
    };
    s_rt.mutex = osMutexNew(&mattr);

    const osTimerAttr_t tattr = {
        .name    = "heat_tick",
        .cb_mem  = &s_tick_timer_cb,
        .cb_size = sizeof(s_tick_timer_cb),
    };
    s_rt.tick_timer = osTimerNew(tick_timer_cb, osTimerPeriodic, NULL, &tattr);

    return (s_rt.mutex != NULL) && (s_rt.tick_timer != NULL);
}

static void destroy_resources(void) {
    /* 静态对象不 Delete，仅停止并解绑 */
    if (s_rt.tick_timer) {
        if (osTimerIsRunning(s_rt.tick_timer)) {
            osTimerStop(s_rt.tick_timer);
        }
        s_rt.tick_timer = NULL;
    }
    s_rt.cmd_queue = NULL;
    s_rt.mutex     = NULL;
}

bool heat_task_init(void) {
    if (s_rt.cmd_queue) {
        return true; /* 已初始化 */
    }

    if (!create_resources()) {
        return false;
    }

    heat_indicator_init();

    heat_control_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.pid_kp             = HEAT_PID_KP;
    cfg.pid_ki             = HEAT_PID_KI;
    cfg.pid_kd             = HEAT_PID_KD;
    cfg.pid_deadband       = HEAT_PID_DEADBAND;
    cfg.pid_out_min        = HEAT_PID_OUT_MIN;
    cfg.pid_out_max        = HEAT_PID_OUT_MAX;
    cfg.pid_integral_limit = HEAT_PID_INTEGRAL_LIMIT;
    cfg.pid_d_filter       = HEAT_PID_D_FILTER;
    cfg.max_timer_min      = HEAT_MAX_TIMER_MIN;
    cfg.ntc_fail_threshold = HEAT_NTC_FAIL_THRESHOLD;
    cfg.level_temp[HEAT_LEVEL_1] = 35.0f;
    cfg.level_temp[HEAT_LEVEL_2] = 45.0f;
    cfg.level_temp[HEAT_LEVEL_3] = 55.0f;
    heat_control_init(&cfg);

    const osThreadAttr_t attr = {
        .name       = TASK_HEAT_NAME,
        .cb_mem     = &s_task_tcb,
        .cb_size    = sizeof(s_task_tcb),
        .stack_mem  = s_task_stack,
        .stack_size = sizeof(s_task_stack),
        .priority   = (osPriority_t)TASK_HEAT_PRIORITY,
    };
    if (!osThreadNew(heat_control_task, NULL, &attr)) {
        destroy_resources();
        return false;
    }

    return true;
}

void heat_task_deinit(void) {
    /* 先关硬件再释放资源 */
    (void)heat_status_set(HEAT_STATUS_STOP);
    bsp_heat_set_power(0);
    bsp_heat_stop();
    heat_indicator_all_off();
    destroy_resources();
}

void heat_service_set_upload_handler(heat_upload_cb_t cb) {
    s_rt.upload_cb = cb;
}

void heat_service_set_state_handler(heat_state_cb_t cb) {
    s_rt.state_cb = cb;
}

bool heat_status_set(HeatStatus status) {
    if (status != HEAT_STATUS_STOP && status != HEAT_STATUS_RUNNING) {
        return false;
    }
    heat_cmd_t cmd = {.type = HEAT_CMD_SET_STATUS, .param.status = status};
    if (!post_cmd(&cmd)) {
        return false;
    }

    if (s_rt.tick_timer) {
        if (status == HEAT_STATUS_RUNNING) {
            if (!osTimerIsRunning(s_rt.tick_timer)) {
                osTimerStart(s_rt.tick_timer, CMSIS_TICKS(1000));
            }
        } else {
            if (osTimerIsRunning(s_rt.tick_timer)) {
                osTimerStop(s_rt.tick_timer);
            }
        }
    }
    return true;
}

void heat_status_switch(void) {
    heat_state_t snap;
    heat_control_snapshot(&snap);
    (void)heat_status_set(snap.status == HEAT_STATUS_RUNNING ? HEAT_STATUS_STOP : HEAT_STATUS_RUNNING);
}

bool heat_level_set(HeatLevel level) {
    if (level > HEAT_LEVEL_MAX) {
        return false;
    }
    heat_cmd_t cmd = {.type = HEAT_CMD_SET_LEVEL, .param.level = level};
    return post_cmd(&cmd);
}

void heat_level_up(void) {
    heat_state_t snap;
    heat_control_snapshot(&snap);
    HeatLevel lv = (snap.level < HEAT_LEVEL_MAX) ? (HeatLevel)(snap.level + 1) : HEAT_LEVEL_MAX;
    if (lv == HEAT_LEVEL_MAX && snap.level == HEAT_LEVEL_MAX) {
        heat_indicator_beep(HEAT_BUZZER_BEEP_COUNT * 100U);
    }
    (void)heat_level_set(lv);
}

void heat_level_down(void) {
    heat_state_t snap;
    heat_control_snapshot(&snap);
    HeatLevel lv = (snap.level > HEAT_LEVEL_1) ? (HeatLevel)(snap.level - 1) : HEAT_LEVEL_1;
    if (lv == HEAT_LEVEL_1 && snap.level == HEAT_LEVEL_1) {
        heat_indicator_beep(HEAT_BUZZER_BEEP_COUNT * 100U);
    }
    (void)heat_level_set(lv);
}

bool heat_timer_set(uint16_t minute) {
    if (minute > HEAT_MAX_TIMER_MIN) {
        return false;
    }
    heat_cmd_t cmd = {.type = HEAT_CMD_SET_TIMER, .param.minute = minute};
    return post_cmd(&cmd);
}

HeatStatus heat_status_get(void) {
    heat_state_t snap;
    heat_control_snapshot(&snap);
    return snap.status;
}

HeatLevel heat_level_get(void) {
    heat_state_t snap;
    heat_control_snapshot(&snap);
    return snap.level;
}

uint16_t heat_remain_time_get(void) {
    heat_state_t snap;
    heat_control_snapshot(&snap);
    return (uint16_t)(snap.remain_sec / 60U);
}

/**
 * @file heat_control_test.c
 * @brief heat_control 纯逻辑状态机主机测试
 *
 * 目的：把「关机失效」这类 bug 变成可复现用例。
 * 上一版该 bug 的根因是关断职责分散在三个函数里，
 * 现在只有一个 decision point，可直接断言。
 */

#include <stdio.h>
#include <string.h>

#include "heat_control.h"

static int g_fail = 0;

#define CHECK(cond, msg)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("  FAIL: %s (line %d)\n", msg, __LINE__);                                                            \
            g_fail = 1;                                                                                                \
        }                                                                                                              \
    } while (0)

static heat_control_cfg_t default_cfg(void) {
    heat_control_cfg_t c;
    memset(&c, 0, sizeof(c));
    c.pid_kp             = 20.0f;
    c.pid_ki             = 3.0f;
    c.pid_kd             = 1.0f;
    c.pid_deadband       = 1.0f;
    c.pid_out_min        = 0;
    c.pid_out_max        = 100;
    c.pid_integral_limit = 30.0f;
    c.pid_d_filter       = 0.1f;
    c.max_timer_min      = 720;
    c.ntc_fail_threshold = 3;
    c.level_temp[HEAT_LEVEL_1] = 35.0f;
    c.level_temp[HEAT_LEVEL_2] = 45.0f;
    c.level_temp[HEAT_LEVEL_3] = 55.0f;
    return c;
}

static void step(float temp, heat_output_t* out) {
    heat_control_step(500, temp, true, out);
}

static void send_status(HeatStatus s) {
    heat_cmd_t c = {.type = HEAT_CMD_SET_STATUS, .param.status = s};
    heat_control_send(&c);
}

static void send_level(HeatLevel l) {
    heat_cmd_t c = {.type = HEAT_CMD_SET_LEVEL, .param.level = l};
    heat_control_send(&c);
}

static void send_timer(uint16_t m) {
    heat_cmd_t c = {.type = HEAT_CMD_SET_TIMER, .param.minute = m};
    heat_control_send(&c);
}

static void send_tick(void) {
    heat_cmd_t c = {.type = HEAT_CMD_TICK_1S};
    heat_control_send(&c);
}

int main(void) {
    heat_output_t out;

    /*==================================================================
     * 1. 上电初始状态：必须停机、功率 0
     *==================================================================*/
    {
        heat_control_cfg_t c_ = default_cfg();
        heat_control_init(&c_);
    }
    step(25.0f, &out);
    printf("[1] power-on: status=%d power=%u\n", out.status, out.power_percent);
    CHECK(out.status == HEAT_STATUS_STOP, "initial status must be STOP");
    CHECK(out.power_percent == 0, "initial power must be 0");

    /*==================================================================
     * 2. 开机加热：目标 35℃、实测 25℃ → 应有输出
     *==================================================================*/
    send_status(HEAT_STATUS_RUNNING);
    step(25.0f, &out);
    printf("[2] start heating @25C target35: status=%d power=%u\n", out.status, out.power_percent);
    CHECK(out.status == HEAT_STATUS_RUNNING, "status must be RUNNING");
    CHECK(out.power_percent > 0, "power must be > 0 when far below target");

    /*==================================================================
     * 3. 【核心回归】关机必须真的把功率归零
     *    上一版 bug：status 先被写成 STOP，导致关断分支永不执行
     *==================================================================*/
    send_status(HEAT_STATUS_STOP);
    step(25.0f, &out);
    printf("[3] shutdown: status=%d power=%u\n", out.status, out.power_percent);
    CHECK(out.status == HEAT_STATUS_STOP, "status must be STOP after shutdown");
    CHECK(out.power_percent == 0, "power MUST be 0 after shutdown (regression)");

    /*==================================================================
     * 4. 关机后再采样多次，功率必须持续为 0（幂等）
     *==================================================================*/
    for (int i = 0; i < 10; i++) {
        step(25.0f, &out);
        if (out.power_percent != 0) {
            printf("  FAIL at iteration %d: power=%u\n", i, out.power_percent);
            g_fail = 1;
        }
    }
    printf("[4] shutdown idempotent over 10 cycles: power=%u\n", out.power_percent);
    CHECK(out.power_percent == 0, "power must stay 0");

    /*==================================================================
     * 5. 断电重启：重新开机应恢复输出
     *==================================================================*/
    send_status(HEAT_STATUS_RUNNING);
    step(26.0f, &out);
    printf("[5] restart: status=%d power=%u\n", out.status, out.power_percent);
    CHECK(out.status == HEAT_STATUS_RUNNING, "restart must work after shutdown");
    CHECK(out.power_percent > 0, "restart must produce power");

    /*==================================================================
     * 6. 档位切换改变目标温度：档3(55℃) 在 50℃ 时应仍有输出，
     *    档1(35℃) 在 50℃ 时应输出 0（超过目标）
     *==================================================================*/
    {
        heat_control_cfg_t c_ = default_cfg();
        heat_control_init(&c_);
    }
    send_status(HEAT_STATUS_RUNNING);
    send_level(HEAT_LEVEL_3);
    for (int i = 0; i < 5; i++) step(50.0f, &out); /* 让 PID 稳定 */
    uint8_t p_high = out.power_percent;

    send_level(HEAT_LEVEL_1);
    for (int i = 0; i < 5; i++) step(50.0f, &out);
    uint8_t p_low = out.power_percent;

    printf("[6] level3@50C power=%u, level1@50C power=%u\n", p_high, p_low);
    CHECK(p_high > 0, "level3 target 55C should heat at 50C");
    CHECK(p_low == 0, "level1 target 35C should not heat at 50C");

    /*==================================================================
     * 7. 定时到点必须停机且功率归零
     *==================================================================*/
    {
        heat_control_cfg_t c_ = default_cfg();
        heat_control_init(&c_);
    }
    send_timer(1); /* 1 分钟 */
    send_status(HEAT_STATUS_RUNNING);
    step(25.0f, &out);
    CHECK(out.status == HEAT_STATUS_RUNNING, "must be running with timer set");

    /* 60 次 tick 应耗尽 60 秒 */
    for (int i = 0; i < 61; i++) {
        send_tick();
        heat_control_step(0, 25.0f, true, &out);
    }
    printf("[7] after timer expiry: status=%d power=%u remain=%u\n", out.status, out.power_percent, out.remain_sec);
    CHECK(out.status == HEAT_STATUS_STOP, "timer expiry must stop heating");
    CHECK(out.power_percent == 0, "timer expiry must zero power");

    /*==================================================================
     * 8. NTC 连续失败达阈值 → 停机
     *==================================================================*/
    {
        heat_control_cfg_t c_ = default_cfg();
        heat_control_init(&c_);
    }
    send_status(HEAT_STATUS_RUNNING);
    step(25.0f, &out);
    CHECK(out.power_percent > 0, "should be heating before fault");

    for (int i = 0; i < 3; i++) {
        heat_cmd_t c = {.type = HEAT_CMD_NTC_READ_FAIL};
        heat_control_send(&c);
    }
    step(25.0f, &out);
    printf("[8] after 3 NTC read fails: status=%d power=%u fault=%d\n", out.status, out.power_percent,
           out.fault_active);
    CHECK(out.status == HEAT_STATUS_STOP, "NTC failure threshold must stop heating");
    CHECK(out.power_percent == 0, "NTC failure must zero power");

    /*==================================================================
     * 9. 传感器无效时不得输出功率（temp_valid=false）
     *==================================================================*/
    {
        heat_control_cfg_t c_ = default_cfg();
        heat_control_init(&c_);
    }
    send_status(HEAT_STATUS_RUNNING);
    heat_control_step(500, 0.0f, false, &out);
    printf("[9] invalid temp while running: power=%u\n", out.power_percent);
    CHECK(out.power_percent == 0, "invalid temperature must not drive heater");

    /*==================================================================
     * 10. 越界参数被拒绝
     *==================================================================*/
    {
        heat_control_cfg_t c10 = default_cfg();
        heat_control_init(&c10);

        /* 越界档位：控制层应忽略 */
        heat_cmd_t bad_level = {.type = HEAT_CMD_SET_LEVEL, .param.level = (HeatLevel)(HEAT_LEVEL_MAX + 1)};
        heat_control_send(&bad_level);
        heat_control_step(500, 25.0f, true, &out);
        heat_state_t s10;
        heat_control_snapshot(&s10);
        CHECK(s10.level == HEAT_LEVEL_1, "out-of-range level must be ignored");

        /* 越界定时：控制层应忽略，remain 保持 0 */
        heat_cmd_t bad_timer = {.type = HEAT_CMD_SET_TIMER, .param.minute = 721};
        heat_control_send(&bad_timer);
        heat_control_step(500, 25.0f, true, &out);
        heat_control_snapshot(&s10);
        printf("[10] out-of-range level ignored, timer remain=%u\n", s10.remain_sec);
        CHECK(s10.remain_sec == 0, "out-of-range timer must be ignored");
    }

    /*==================================================================
     * 11. 定时时间随运行递减，快照可读
     *==================================================================*/
    {
        heat_control_cfg_t c_ = default_cfg();
        heat_control_init(&c_);
    }
    send_timer(2);
    send_status(HEAT_STATUS_RUNNING);
    step(25.0f, &out);
    for (int i = 0; i < 30; i++) {
        send_tick();
        heat_control_step(0, 25.0f, true, &out);
    }
    heat_state_t snap;
    heat_control_snapshot(&snap);
    printf("[11] after 30 ticks of 2min timer: remain_sec=%u\n", snap.remain_sec);
    CHECK(snap.remain_sec == 90, "remain should be 120-30=90 sec");

    printf(g_fail ? "\n== SOME TESTS FAILED ==\n" : "\n== ALL HEAT CONTROL TESTS PASSED ==\n");
    return g_fail;
}

/**
 * @file bsp_heat.c
 * @brief 加热模块驱动
 */

#include "bsp_heat.h"
#include "bsp_pwm.h"

static struct {
    uint8_t current_power;
} s_heat;

static bool s_inited = false;

bsp_status_t bsp_heat_init(const bsp_heat_config_t* config) {
    if (!config || !config->htim) {
        return BSP_ERR_PARAM;
    }
    if (s_inited) {
        return BSP_ERR_BUSY;
    }

    bsp_pwm_config_t pwm_cfg = {
        .htim       = config->htim,
        .channel    = config->channel,
        .freq_hz    = config->freq_hz,
        .resolution = config->resolution,
    };
    bsp_status_t ret = bsp_pwm_init(&pwm_cfg);
    if (ret != BSP_OK) {
        return ret;
    }

    s_heat.current_power = 0;
    s_inited             = true;
    return BSP_OK;
}

void bsp_heat_deinit(void) {
    if (!s_inited) {
        return;
    }
    (void)bsp_heat_stop();
    bsp_pwm_deinit();
    s_heat.current_power = 0;
    s_inited             = false;
}

bsp_status_t bsp_heat_start(void) {
    if (!s_inited) {
        return BSP_ERR_NOTINIT;
    }
    /* 启动前确保占空比为 0：HAL_TIM_PWM_Stop 不会清除 CCR，
     * 若上次停止前留有占空比，直接 start 会恢复该输出。 */
    (void)bsp_heat_set_power(0);
    return bsp_pwm_start();
}

bsp_status_t bsp_heat_stop(void) {
    if (!s_inited) {
        return BSP_ERR_NOTINIT;
    }
    /* 先归零再停输出，避免之后 start 时恢复旧占空比 */
    (void)bsp_heat_set_power(0);
    return bsp_pwm_stop();
}

bsp_status_t bsp_heat_set_power(uint8_t power_percent) {
    if (!s_inited) {
        return BSP_ERR_NOTINIT;
    }
    uint8_t power = (power_percent > BSP_HEAT_MAX_POWER) ? BSP_HEAT_MAX_POWER : power_percent;
    uint32_t res  = bsp_pwm_get_resolution();
    if (!res) {
        return BSP_ERROR;
    }
    uint32_t duty = ((uint32_t)power * res) / BSP_HEAT_MAX_POWER;
    if (duty >= res) {
        duty = res - 1;
    }
    bsp_status_t ret = bsp_pwm_set_duty(duty);
    if (ret == BSP_OK) {
        s_heat.current_power = power;
    }
    return ret;
}

uint8_t bsp_heat_get_power(void) {
    return s_inited ? s_heat.current_power : 0;
}

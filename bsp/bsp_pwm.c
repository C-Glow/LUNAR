/**
 * @file bsp_pwm.c
 * @brief PWM 驱动实现
 */

#include "bsp_pwm.h"

static struct {
    TIM_HandleTypeDef* htim;
    uint32_t channel;
    uint32_t resolution;
    bool initialized;
} s_pwm;

static bool s_inited = false;

static uint32_t pwm_get_timer_clk(TIM_TypeDef* tim) {
    if (tim == TIM1
#if defined(TIM8)
        || tim == TIM8
#endif
#if defined(TIM9)
        || tim == TIM9 || tim == TIM10 || tim == TIM11
#endif
    ) {
        uint32_t pclk2 = HAL_RCC_GetPCLK2Freq();
        return (RCC->CFGR & RCC_CFGR_PPRE2) ? pclk2 * 2 : pclk2;
    } else {
        uint32_t pclk1 = HAL_RCC_GetPCLK1Freq();
        return (RCC->CFGR & RCC_CFGR_PPRE1) ? pclk1 * 2 : pclk1;
    }
}

bsp_status_t bsp_pwm_init(const bsp_pwm_config_t* config) {
    if (!config || !config->htim) {
        return BSP_ERR_PARAM;
    }
    if (config->resolution == 0 || config->freq_hz == 0) {
        return BSP_ERR_PARAM;
    }
    if (s_inited) {
        return BSP_ERR_BUSY;
    }

    uint32_t tim_clk = pwm_get_timer_clk(config->htim->Instance);
    uint32_t ticks   = tim_clk / config->freq_hz;
    if (ticks < config->resolution) {
        return BSP_ERROR;
    }

    uint32_t prescaler = (ticks / config->resolution) - 1;

    config->htim->Init.Prescaler         = prescaler;
    config->htim->Init.Period            = config->resolution - 1;
    config->htim->Init.CounterMode       = TIM_COUNTERMODE_UP;
    config->htim->Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    config->htim->Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;

    if (HAL_TIM_PWM_Init(config->htim) != HAL_OK) {
        return BSP_ERR_HW;
    }

    TIM_OC_InitTypeDef oc = {0};
    oc.OCMode             = TIM_OCMODE_PWM1;
    oc.Pulse              = 0;
    oc.OCPolarity         = TIM_OCPOLARITY_HIGH;
    oc.OCFastMode         = TIM_OCFAST_DISABLE;
    oc.OCIdleState        = TIM_OCIDLESTATE_RESET;
    oc.OCNIdleState       = TIM_OCNIDLESTATE_RESET;

    if (HAL_TIM_PWM_ConfigChannel(config->htim, &oc, config->channel) != HAL_OK) {
        HAL_TIM_PWM_DeInit(config->htim);
        return BSP_ERR_HW;
    }

    s_pwm.htim        = config->htim;
    s_pwm.channel     = config->channel;
    s_pwm.resolution  = config->resolution;
    s_pwm.initialized = true;
    s_inited          = true;
    return BSP_OK;
}

void bsp_pwm_deinit(void) {
    if (!s_inited) {
        return;
    }
    if (s_pwm.initialized && s_pwm.htim) {
        HAL_TIM_PWM_Stop(s_pwm.htim, s_pwm.channel);
        HAL_TIM_PWM_DeInit(s_pwm.htim);
    }
    s_pwm.initialized = false;
    s_pwm.htim        = NULL;
    s_inited          = false;
}

bsp_status_t bsp_pwm_set_duty(uint32_t duty) {
    if (!s_inited) {
        return BSP_ERR_NOTINIT;
    }
    if (duty >= s_pwm.resolution) {
        duty = s_pwm.resolution - 1;
    }
    __HAL_TIM_SET_COMPARE(s_pwm.htim, s_pwm.channel, duty);
    return BSP_OK;
}

bsp_status_t bsp_pwm_start(void) {
    if (!s_inited) {
        return BSP_ERR_NOTINIT;
    }
    return (HAL_TIM_PWM_Start(s_pwm.htim, s_pwm.channel) == HAL_OK) ? BSP_OK : BSP_ERR_HW;
}

bsp_status_t bsp_pwm_stop(void) {
    if (!s_inited) {
        return BSP_ERR_NOTINIT;
    }
    return (HAL_TIM_PWM_Stop(s_pwm.htim, s_pwm.channel) == HAL_OK) ? BSP_OK : BSP_ERR_HW;
}

uint32_t bsp_pwm_get_resolution(void) {
    return s_inited ? s_pwm.resolution : 0;
}

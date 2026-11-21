/**
 * @file bsp_pwm.h
 * @brief PWM 驱动
 *
 * 单实例模块：全局仅一个 PWM 输出，无需句柄参数。
 */

#ifndef BSP_PWM_H
#define BSP_PWM_H

#include "bsp_common.h"
#include "stm32f1xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/** PWM 通道 */
typedef uint32_t bsp_pwm_channel_t;

/** PWM 初始化配置 */
typedef struct {
    TIM_HandleTypeDef* htim;   /**< HAL 定时器句柄（必须已在 CubeMX 中配置） */
    bsp_pwm_channel_t channel; /**< TIM 通道（如 TIM_CHANNEL_1） */
    uint32_t freq_hz;          /**< PWM 频率 (Hz) */
    uint32_t resolution;       /**< 占空比分辨率（如 1000 表示 0~999） */
} bsp_pwm_config_t;

/* API 函数 */

/** 初始化；重复调用返回 BSP_ERR_BUSY */
bsp_status_t bsp_pwm_init(const bsp_pwm_config_t* config);

/** 反初始化，停止输出并释放定时器 */
void bsp_pwm_deinit(void);

/** 设置占空比，duty 取值 [0, resolution) */
bsp_status_t bsp_pwm_set_duty(uint32_t duty);

bsp_status_t bsp_pwm_start(void);
bsp_status_t bsp_pwm_stop(void);

/** 获取占空比分辨率 */
uint32_t bsp_pwm_get_resolution(void);

#ifdef __cplusplus
}
#endif

#endif /* BSP_PWM_H */

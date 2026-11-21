/**
 * @file bsp_heat.h
 * @brief 加热模块驱动（基于 PWM）
 *
 * 单实例模块，无需句柄参数。
 */

#ifndef BSP_HEAT_H
#define BSP_HEAT_H

#include "bsp_common.h"
#include "stm32f1xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 加热模块配置 */
typedef struct {
    TIM_HandleTypeDef* htim; /**< HAL 定时器句柄 */
    uint32_t channel;        /**< PWM 通道 */
    uint32_t freq_hz;        /**< PWM 频率 */
    uint32_t resolution;     /**< 占空比分辨率 */
} bsp_heat_config_t;

/* API 函数 */

#define BSP_HEAT_MAX_POWER 100U /**< 最大功率百分比 */

/** 初始化；重复调用返回 BSP_ERR_BUSY */
bsp_status_t bsp_heat_init(const bsp_heat_config_t* config);

/** 反初始化：先归零再停止输出 */
void bsp_heat_deinit(void);

/** 启动输出。启动前会把占空比归零，避免恢复上次输出 */
bsp_status_t bsp_heat_start(void);

/** 停止输出。会先把功率归零 */
bsp_status_t bsp_heat_stop(void);

/** 设置功率百分比 [0, 100] */
bsp_status_t bsp_heat_set_power(uint8_t power_percent);

/** 当前功率百分比；未初始化返回 0 */
uint8_t bsp_heat_get_power(void);

#ifdef __cplusplus
}
#endif

#endif /* BSP_HEAT_H */

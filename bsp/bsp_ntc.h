/**
 * @file    bsp_ntc.h
 * @brief   NTC 温度传感器驱动（查表法 + 插值 + DMA，无 OS 依赖）
 * @version 2.0
 * @date    2026-06-06
 *
 * @note    支持 DMA 双缓冲模式和非 DMA 轮询模式。
 *          滤波链：中值滤波 → 滑动平均 → 低通滤波。
 *          通过编译宏 NTC_USE_DMA / USE_DOUBLE_BUFFER 控制。
 */

#ifndef BSP_NTC_H
#define BSP_NTC_H

#include "bsp_common.h"
#include "stm32f1xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 编译期配置 */

/**
 * @brief 采样通道选择
 *
 * 0 = 轮询（推荐）：控制周期 500ms 才取一次温度，轮询 5 个样本仅耗时 ~8us，
 *     占用率 ≈0%，且不占用 DMA1_Channel1 / 无 40kHz 中断风暴。
 *     需要 Core/Src/adc.c 中 ContinuousConvMode = DISABLE。
 *
 * 1 = DMA 双缓冲：仅当需要毫秒级温度响应（如过温保护）时启用。
 *     注意 DMA 传输长度单位是"数据项"，HalfWord 对齐下 16 项 = 32 字节，
 *     因此 dma_buf 必须是 uint16_t，不能用 uint32_t。
 */
#define NTC_USE_DMA 0 /**< 0=轮询采样, 1=DMA 采样 */
#define USE_DOUBLE_BUFFER 1     /**< 启用 DMA 双缓冲（仅 NTC_USE_DMA=1 时有效） */
#define NTC_FILTER_SAMPLES 5    /**< 中值滤波样本数（奇数） */
#define NTC_MOVING_AVG_LEN 8    /**< 滑动平均长度（2 的幂） */
#define NTC_LOW_PASS_ALPHA 0.3f /**< 低通滤波系数 */

/* 类型定义 */

/** NTC 配置 */
typedef struct {
    ADC_HandleTypeDef* hadc; /**< HAL ADC 句柄 */
    float temp_offset;       /**< 温度补偿偏移量（℃） */
} bsp_ntc_config_t;

/* API 函数 */

/** 初始化 NTC 传感器 */
bsp_status_t bsp_ntc_init(const bsp_ntc_config_t* config);

/** 反初始化 NTC（释放 DMA 和内存） */
void bsp_ntc_deinit(void);

/** 读取当前温度 */
bsp_status_t bsp_ntc_read(float* temp_c);

/** 获取原始 ADC 值（经过滤波） */
bsp_status_t bsp_ntc_get_adc(uint32_t* adc_val);

/**
 * @brief 查询传感器是否故障（开路/短路/卡死）
 * @return true=故障
 */
bool bsp_ntc_is_fault(void);

/** 清除故障状态 */
void bsp_ntc_clear_fault(void);

/** 设置温度补偿偏移 */
void bsp_ntc_set_offset(float offset);

/** 重置滤波器 */
void bsp_ntc_reset_filter(void);

#ifdef __cplusplus
}
#endif

#endif /* BSP_NTC_H */

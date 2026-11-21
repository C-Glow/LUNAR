/**
 * @file bsp_common.h
 * @brief BSP 层公共类型与约定
 *
 * 模块规范：
 *   - 单实例：每个外设模块全局只有一个实例，接口不带句柄参数
 *   - init / deinit 成对出现，重复 init 返回 BSP_ERR_BUSY
 *   - 可能失败的操作返回 bsp_status_t，纯查询返回相应值类型
 *   - 命名：bsp_{module}_{verb}_{noun}()
 *   - 不得依赖 app / components 层（业务、协议）
 *   - 允许使用 RTOS 同步原语（互斥量、信号量），但不得创建任务
 */

#ifndef BSP_COMMON_H
#define BSP_COMMON_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 统一状态码 */
typedef enum {
    BSP_OK          = 0,  /**< 操作成功 */
    BSP_ERROR       = -1, /**< 通用错误 */
    BSP_ERR_PARAM   = -2, /**< 参数无效（NULL、越界等） */
    BSP_ERR_BUSY    = -3, /**< 资源忙（重复初始化、DMA占用等） */
    BSP_ERR_TIMEOUT = -4, /**< 操作超时 */
    BSP_ERR_HW      = -5, /**< 硬件错误（外设故障、传感器异常等） */
    BSP_ERR_NOMEM   = -6, /**< 内存不足 */
    BSP_ERR_NOTINIT = -7, /**< 模块未初始化 */
} bsp_status_t;

/* 通用位操作（BSP 层专用） */

#define BSP_BIT_SET(reg, bit) ((reg) |= (bit))
#define BSP_BIT_CLEAR(reg, bit) ((reg) &= ~(bit))
#define BSP_BIT_CHECK(reg, bit) (((reg) & (bit)) != 0)

/* 通用工具宏 */

/** 数组元素个数 */
#define BSP_ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

/** 限制值在 [min, max] 范围内 */
#define BSP_CLAMP(val, min, max) (((val) < (min)) ? (min) : (((val) > (max)) ? (max) : (val)))

#ifdef __cplusplus
}
#endif

#endif /* BSP_COMMON_H */

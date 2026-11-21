/**
 * @file bsp_bt401.h
 * @brief BT401 蓝牙模块驱动：UART DMA + IDLE 中断，多缓冲环形帧队列
 *
 * TX 由内部互斥量串行化；RX 帧在 ISR 中入队，get_frame 非阻塞。
 */

#ifndef BSP_BT401_H
#define BSP_BT401_H

#include "bsp_common.h"
#include "stm32f1xx_hal.h"
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 3 个 DMA 缓冲平衡 RAM 占用与吞吐 */
#define BT401_DMA_BUF_SIZE 128
#define BT401_DMA_BUF_COUNT 3
#define BT401_FRAME_QUEUE_SIZE 4

/** 一帧数据 */
typedef struct {
    uint8_t data[BT401_DMA_BUF_SIZE];
    uint16_t len;
} bsp_bt401_frame_t;

/** BT401 配置 */
typedef struct {
    UART_HandleTypeDef* huart; /* USART3 */
} bsp_bt401_config_t;

/** 初始化 BT401 模块 */
bsp_status_t bsp_bt401_init(const bsp_bt401_config_t* config);

/** 反初始化，停止 DMA */
void bsp_bt401_deinit(void);

/** 发送原始字节（阻塞）；已由内部互斥量串行化。 */
bsp_status_t bsp_bt401_send(const uint8_t* buf, uint16_t len);

/** 格式化发送，返回实际字节数，<0 表示失败 */
int bsp_bt401_printf(const char* format, ...);

/** 取一帧；BSP_OK=有新帧，BSP_ERROR=无新帧 */
bsp_status_t bsp_bt401_get_frame(bsp_bt401_frame_t* frame);

/** DMA 溢出计数，诊断用 */
uint32_t bsp_bt401_get_overrun(void);

/** 因 TX 互斥超时被丢弃的发送次数 */
uint32_t bsp_bt401_get_tx_drop(void);

/**
 * @brief 阻塞等待一帧到达
 * @param timeout_ms  超时(ms)，osWaitForever 表示永久等待
 * @return true 表示有新帧，此时 get_frame 必定能取到数据
 */
bool bsp_bt401_wait_frame(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* BSP_BT401_H */

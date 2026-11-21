/**
 * @file bsp_bt401.c
 * @brief BT401 蓝牙模块驱动
 */

#include "bsp_bt401.h"

/* 静态分配类型定义在 FreeRTOS.h */
#include "FreeRTOS.h"

#include "cmsis_os2.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    bsp_bt401_frame_t frames[BT401_FRAME_QUEUE_SIZE];
    volatile uint8_t head;
    volatile uint8_t tail;
    volatile uint32_t overrun;
} bt401_queue_t;

struct bsp_bt401_s {
    UART_HandleTypeDef* huart;
    uint8_t dma_buffer[BT401_DMA_BUF_COUNT][BT401_DMA_BUF_SIZE] __attribute__((aligned(4)));
    volatile uint8_t current_buf;
    volatile uint16_t buf_lengths[BT401_DMA_BUF_COUNT];
    bt401_queue_t rx_queue;
    bool initialized;
};

static struct bsp_bt401_s s_inst;
static bool s_inited            = false;
static struct bsp_bt401_s* g_bt401_isr = NULL;

/* TX 串行化：三方并发写同一串口，无互斥会字节交错或静默丢帧 */
static osMutexId_t s_tx_mutex      = NULL;
static StaticSemaphore_t s_tx_mutex_mem;
static StaticSemaphore_t s_rx_sem_mem;
static uint32_t s_tx_drop          = 0;
#define BT401_TX_TIMEOUT_MS 100U

/* RX 唤醒信号量：ISR 入队后释放，任务阻塞等待 */
static osSemaphoreId_t s_rx_sem = NULL;

/* 不能用 HAL_MAX_DELAY：喂狗任务会因 TX 卡死而永久阻塞 */
#define BT401_TX_HW_TIMEOUT_MS 100U

/* 环形队列 */
static bool bt401_queue_push(uint8_t buf_idx) {
    bt401_queue_t* q = &s_inst.rx_queue;
    uint8_t next     = (q->head + 1) % BT401_FRAME_QUEUE_SIZE;
    if (next == q->tail) {
        q->tail = (q->tail + 1) % BT401_FRAME_QUEUE_SIZE;
        q->overrun++;
    }
    uint16_t len = s_inst.buf_lengths[buf_idx];
    if (len > BT401_DMA_BUF_SIZE) {
        len = BT401_DMA_BUF_SIZE;
    }
    memcpy(q->frames[q->head].data, s_inst.dma_buffer[buf_idx], len);
    q->frames[q->head].len = len;
    q->head                = next;
    return true;
}

static bool bt401_queue_pop(bsp_bt401_frame_t* frame) {
    bt401_queue_t* q = &s_inst.rx_queue;
    if (q->head == q->tail) {
        return false;
    }
    memcpy(frame, &q->frames[q->tail], sizeof(bsp_bt401_frame_t));
    q->tail = (q->tail + 1) % BT401_FRAME_QUEUE_SIZE;
    return true;
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef* huart, uint16_t Size) {
    if (!g_bt401_isr || huart->Instance != USART3) {
        return;
    }

    HAL_UART_RxEventTypeTypeDef ev = HAL_UARTEx_GetRxEventType(huart);
    /* IDLE 与 TC 都要处理：漏掉 TC 会导致 DMA 停止后不再重启 */
    if (ev != HAL_UART_RXEVENT_IDLE && ev != HAL_UART_RXEVENT_TC) {
        return;
    }
    if (Size == 0 || Size > BT401_DMA_BUF_SIZE) {
        Size = BT401_DMA_BUF_SIZE;
    }

    s_inst.buf_lengths[s_inst.current_buf] = Size;
    bt401_queue_push(s_inst.current_buf);
    s_inst.current_buf = (s_inst.current_buf + 1) % BT401_DMA_BUF_COUNT;

    /* DMA_NORMAL 模式必须重新挂载，否则只收一帧 */
    HAL_UARTEx_ReceiveToIdle_DMA(huart, s_inst.dma_buffer[s_inst.current_buf], BT401_DMA_BUF_SIZE);
    /* 半满中断对帧接收无用，且会额外打断 CPU */
    __HAL_DMA_DISABLE_IT(huart->hdmarx, DMA_IT_HT);

    if (s_rx_sem) {
        osSemaphoreRelease(s_rx_sem);
    }
}

bsp_status_t bsp_bt401_init(const bsp_bt401_config_t* config) {
    if (!config || !config->huart) {
        return BSP_ERR_PARAM;
    }
    if (s_inited) {
        return BSP_ERR_BUSY;
    }

    s_inst.huart         = config->huart;
    s_inst.rx_queue.head = s_inst.rx_queue.tail = 0;
    s_inst.rx_queue.overrun                     = 0;

    __HAL_DMA_DISABLE_IT(s_inst.huart->hdmarx, DMA_IT_HT);
    s_inst.current_buf = 0;
    g_bt401_isr        = &s_inst;

    const osMutexAttr_t mattr = {
        .name    = "bt401_tx",
        .cb_mem  = &s_tx_mutex_mem,
        .cb_size = sizeof(s_tx_mutex_mem),
    };
    s_tx_mutex = osMutexNew(&mattr);
    if (!s_tx_mutex) {
        g_bt401_isr = NULL;
        return BSP_ERR_NOMEM;
    }

    /* 计数上限 1 即可：任务醒来后会一次性取空帧队列 */
    const osSemaphoreAttr_t sattr = {
        .name    = "bt401_rx",
        .cb_mem  = &s_rx_sem_mem,
        .cb_size = sizeof(s_rx_sem_mem),
    };
    s_rx_sem = osSemaphoreNew(1, 0, &sattr);
    if (!s_rx_sem) {
        g_bt401_isr = NULL;
        s_tx_mutex  = NULL;
        return BSP_ERR_NOMEM;
    }

    if (HAL_UARTEx_ReceiveToIdle_DMA(s_inst.huart, s_inst.dma_buffer[0], BT401_DMA_BUF_SIZE) != HAL_OK) {
        g_bt401_isr = NULL;
        s_rx_sem    = NULL;
        s_tx_mutex  = NULL;
        return BSP_ERR_HW;
    }

    s_inst.initialized = true;
    s_inited           = true;
    return BSP_OK;
}

void bsp_bt401_deinit(void) {
    if (!s_inited) {
        return;
    }
    HAL_UART_DMAStop(s_inst.huart);
    g_bt401_isr        = NULL;
    /* 静态分配：不 Delete，仅解绑 */
    s_rx_sem   = NULL;
    s_tx_mutex = NULL;
    s_inst.initialized = false;
    s_inited           = false;
}

bool bsp_bt401_wait_frame(uint32_t timeout_ms) {
    if (!s_inited || !s_rx_sem) {
        return false;
    }
    return (osSemaphoreAcquire(s_rx_sem, timeout_ms) == osOK);
}

bsp_status_t bsp_bt401_send(const uint8_t* buf, uint16_t len) {
    if (!s_inited) {
        return BSP_ERR_NOTINIT;
    }
    if (!buf || !len) {
        return BSP_ERR_PARAM;
    }

    if (s_tx_mutex && osKernelGetState() == osKernelRunning) {
        if (osMutexAcquire(s_tx_mutex, BT401_TX_TIMEOUT_MS) != osOK) {
            s_tx_drop++;
            return BSP_ERR_BUSY;
        }
        HAL_StatusTypeDef st = HAL_UART_Transmit(s_inst.huart, (uint8_t*)buf, len, BT401_TX_HW_TIMEOUT_MS);
        osMutexRelease(s_tx_mutex);
        return (st == HAL_OK) ? BSP_OK : BSP_ERR_HW;
    }

    /* 调度器未启动（初始化早期）或未创建互斥量 */
    return (HAL_UART_Transmit(s_inst.huart, (uint8_t*)buf, len, BT401_TX_HW_TIMEOUT_MS) == HAL_OK) ? BSP_OK : BSP_ERR_HW;
}

int bsp_bt401_printf(const char* format, ...) {
    if (!s_inited) {
        return -1;
    }
    char buf[128];
    va_list args;
    va_start(args, format);
    int len = vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    if (len < 0) {
        len = 0;
    }
    if ((size_t)len >= sizeof(buf)) {
        len = sizeof(buf) - 1;
    }
    return (bsp_bt401_send((const uint8_t*)buf, (uint16_t)len) == BSP_OK) ? len : 0;
}

bsp_status_t bsp_bt401_get_frame(bsp_bt401_frame_t* frame) {
    if (!s_inited) {
        return BSP_ERR_NOTINIT;
    }
    if (!frame) {
        return BSP_ERR_PARAM;
    }
    /* 保存/恢复 PRIMASK，避免在临界区内被调用时误开全局中断 */
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    bool has = bt401_queue_pop(frame);
    __set_PRIMASK(primask);
    return has ? BSP_OK : BSP_ERROR;
}

uint32_t bsp_bt401_get_overrun(void) {
    return s_inited ? s_inst.rx_queue.overrun : 0;
}

uint32_t bsp_bt401_get_tx_drop(void) {
    return s_inited ? s_tx_drop : 0;
}

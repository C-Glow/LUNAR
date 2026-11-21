# LUNAR 固件优化方案

> 基于对 `app/`、`bsp/`、`components/`、`Core/` 全量源码的静态审查。
> 分四级：**P0 致命（必改）→ P1 性能（影响实时性）→ P2 资源（RAM/Flash）→ P3 架构与可靠性**。
> 每条给出定位、根因、可落地的改法。

---

## 总览

| 级别 | 项 | 影响 |
|------|-----|------|
| P0-1 | 关机指令无法真正关闭加热 | 安全隐患，一旦开热停不下来 |
| P0-2 | 读寄存器 10/11 触发互斥锁重入死锁 | 整机挂死 |
| P0-3 | `ble_task` 512B 栈装不下 512B 局部缓冲 | 栈溢出、随机 HardFault |
| P0-4 | ADC 连续模式 + DMA 中断风暴（≈36k IRQ/s） | 实时性劣化、功耗上升 |
| P0-5 | ADC DMA 半字对齐 vs `uint32_t` 缓冲不匹配 | 温度恒为 25°C，加热 5 秒后自动停机 |
| P1-1 | 日志与 Modbus 共用 USART3，且 TX 无互斥 | 主机收到脏帧 |
| P1-2 | `ble_task` 20ms 轮询取帧 | 响应延迟、空转 |
| P1-3 | 矩阵键盘每 20ms 重配 GPIO ×10 | 无谓 CPU 开销 |
| P2-1 | `configMAX_PRIORITIES 56` | 白吃 980B RAM |
| P2-2 | `at_command_manager` 全未使用 | 白吃 ~1.2KB Flash |
| P2-3 | 双重 CRC 查找表 | 白吃 512B Flash |
| P2-4 | `%.1f` 却未链接浮点 printf | 日志输出乱码 |
| P3-1 | IWDG 未启用（README 却声称有） | 无看门狗保护 |
| P3-2 | `EVENT_POWER_OFF` 等事件无订阅者 | 关机键失效 |
| P3-3 | NTC "卡死"判定会误报 | 开机数秒后自动停热 |

---

# P0 — 致命缺陷（先修，否则其他优化无意义）

## P0-1 关机指令无法关闭加热

**位置**：`app/service/heat_service.c:203-233` + `:128-153`

`process_msg_set_status()` 先在锁内把 `status` 改成 `new_status`，随后调用 `heat_stop_all()`；
而 `heat_stop_all()` 又是**自己重新读 `status` 来判断要不要关硬件**：

```c
if (HEAT_LOCK()) {
    old = s_heat_data.status;
    s_heat_data.status = new_status;    // ← 已经写成 STOP
    HEAT_UNLOCK();
}
if (new_status == HEAT_STATUS_STOP) {
    heat_stop_all();                    // ← 内部读到 status 已是 STOP
}
```

`heat_stop_all()` 里 `old_status` 恒为 `STOP`，`if (old_status == HEAT_STATUS_RUNNING)` 永远不成立
→ **PWM 不关、RF 灯不灭、定时器不停、状态不上传**。

后果：Modbus 写 `HeatingStatus=0`、按键 `KEY_HEAT` 关机、定时到期，全部无效。
唯一能停下的是 NTC 故障路径（那条路 `status` 还是 `RUNNING`，判据成立）。

**修复**：把"关硬件"做成**幂等动作**，不再依赖 `status` 做二次判断。

```c
/* 幂等：任何时候调用都保证输出关闭 */
static void heat_hw_off(void) {
    bsp_heat_set_power(g_heat, 0);
    bsp_heat_stop(g_heat);
    bsp_led_set_mode(g_led, BSP_LED_RF, BSP_LED_MODE_OFF, 0);
    led_time_select(0);
    if (s_heat_data.remain_timer)  osTimerStop(s_heat_data.remain_timer);
    if (s_heat_data.heating_timer) osTimerStop(s_heat_data.heating_timer);
    pid_reset(&s_heat_data.pid);
}

static void heat_stop_all(void) {
    if (!HEAT_LOCK()) return;
    s_heat_data.status         = HEAT_STATUS_STOP;
    s_heat_data.remain_sec     = 0;
    s_heat_data.set_time_min   = 0;
    s_heat_data.is_timing      = false;
    s_heat_data.ntc_fail_count = 0;
    HEAT_UNLOCK();

    heat_hw_off();                       /* 无条件执行 */

    if (s_upload_cb) s_upload_cb();
    event_publish(EVENT_HEAT_STATUS_CHANGE, HEAT_STATUS_STOP);
}

static void process_msg_set_status(HeatStatus new_status) {
    bool was_running;

    if (!HEAT_LOCK()) return;
    was_running = (s_heat_data.status == HEAT_STATUS_RUNNING);
    HEAT_UNLOCK();

    if (new_status == HEAT_STATUS_STOP) {
        if (was_running) heat_stop_all();
        return;
    }
    if (was_running) return;             /* 已运行，避免重复开硬件 */

    uint16_t tm;
    if (!HEAT_LOCK()) return;
    s_heat_data.status = HEAT_STATUS_RUNNING;
    tm = s_heat_data.set_time_min;
    if (tm > 0) {
        s_heat_data.remain_sec = (uint32_t)tm * 60U;
        s_heat_data.is_timing  = true;
    }
    HEAT_UNLOCK();

    heat_hw_turn_on();
    event_publish(EVENT_HEAT_STATUS_CHANGE, HEAT_STATUS_RUNNING);

    if (tm > 0) {
        osTimerStart(s_heat_data.heating_timer, CMSIS_TICKS((uint32_t)tm * 60000U));
        osTimerStart(s_heat_data.remain_timer,  CMSIS_TICKS(1000));
    }
}
```

顺带把 `process_msg_update_remain()` 里的 `heat_stop_all()` 路径也简化——它现在同样依赖 `st == RUNNING`，
在 `remain_sec` 归零时可能因状态已被别处改写而漏关。幂等化后不再有这个问题。

---

## P0-2 回调内重入寄存器锁 → 永久死锁

**位置**：`components/protocol/modbus_slave.c:226-245`、`:589-608`；`app/app_main.c:208-219`

调用链：

```
handle_read_registers()     REG_LOCK()   ← 持有 g_reg_mutex
  └─ g_read_callback(reg)  = on_reg_read()
       └─ default: return register_read(reg)
            └─ REG_LOCK()   ← 同一任务再次 acquire 非递归互斥量
```

`g_reg_mutex = osMutexNew(NULL)` 是**非递归**互斥量，`REG_LOCK()` 用的是 `osWaitForever`
→ 任务永久阻塞在自我等待上。`ble_task` 被卡死，`LOG_PRINTF` / 喂狗全停。

**触发条件**：主机读 `REG_SHORTCUT_KEY1(10)` 或 `REG_SHORTCUT_KEY2(11)`
（`on_reg_read` 只覆盖了 status/level/timer 三个，其余落进 `default`）。

**修复**：新增无锁读取接口，回调只许用它。

```c
/* modbus_slave.h */
uint16_t register_read_unlocked(RegisterID reg);   /* 调用者已持锁时使用 */

/* modbus_slave.c */
uint16_t register_read_unlocked(RegisterID reg) {
    return (reg < REG_COUNT) ? g_registers[reg] : 0xFFFF;
}
```

```c
/* app_main.c */
static uint16_t on_reg_read(RegisterID reg) {
    switch (reg) {
    case REG_HEATING_STATUS: return heat_status_get();
    case REG_HEATING_LEVEL:  return heat_level_get();
    case REG_HEATING_TIMER:  return heat_remain_time_get();
    case REG_SHORTCUT_KEY1:
    case REG_SHORTCUT_KEY2:  return register_read_unlocked(reg);
    default:                 return 0;
    }
}
```

同时在 `modbus_slave.h` 的回调 typedef 上写死规则：

```c
/* 注意：回调在持有 REG_LOCK 的上下文中执行。
 * 回调内严禁调用 register_read / register_write / register_batch_*，
 * 需要原始值时用 register_read_unlocked。 */
typedef uint16_t (*RegisterReadCallback)(RegisterID reg);
```

`internal_register_write()` 里 `g_write_callback(reg, value)` 同样是持锁调用，
`on_reg_write` 走的是 `heat_*_set()`（往队列投递，不重入锁）——目前安全，
但 `REG_POWER_SWITCH` 分支将来若改成 `register_write()` 会立刻死锁，一并约束住。

---

## P0-3 `ble_task` 栈溢出

**位置**：`app/service/ble_service.c:34-71`、`components/protocol/modbus_slave.c:522-539`、`:221`

`TASK_BLE_STACK = 512` 字节，但同一调用链上的栈占用：

| 位置 | 大小 |
|------|------|
| `ble_task` 局部 `bsp_bt401_frame_t f` | 130 B |
| `protocol_handle_request` 局部 `uint8_t response[256]` | 256 B |
| `handle_read_registers` 局部 `uint8_t data_buf[256]` | 256 B |
| HAL / vsnprintf 调用开销 | ≥ 64 B |

合计 ≥ 706 B，**超 512 B 近 40%**。只要收到一帧合法的 `0x03` 读请求就会踩栈。

**修复（三项一起做）**：

1. 缓冲区按实际需求缩容。本工程最多 12 个寄存器，读响应 = 1 + 1 + 1 + 24 + 2 = **29 字节**。

```c
/* modbus_slave.c */
#define RESPONSE_BUF_SIZE 40      /* 原 256 */
```

2. 去掉多余的中间层，`data_buf` 直接构造在 `response` 上：

```c
static ProtocolResult handle_read_registers(uint8_t slave_addr, uint16_t start, uint16_t count,
                                            uint8_t* response, uint16_t* resp_len) {
    if (count == 0 || count > MAX_REGISTERS_PER_FRAME) return PROTOCOL_ERR_INVALID_VALUE;
    if ((uint32_t)start + count > REG_COUNT)          return PROTOCOL_ERR_INVALID_REG;

    for (uint16_t i = 0; i < count; i++)
        if (!check_register_access((RegisterID)(start + i), false))
            return PROTOCOL_ERR_READ_ONLY;

    response[2] = (uint8_t)(count * 2);          /* 字节数直接写进响应 */
    uint16_t n  = 3;

    if (REG_LOCK() != true) return PROTOCOL_ERR_BUSY;
    for (uint16_t i = 0; i < count; i++) {
        RegisterID reg = (RegisterID)(start + i);
        uint16_t   v   = g_read_callback ? g_read_callback(reg) : g_registers[reg];
        response[n++]  = (uint8_t)(v >> 8);
        response[n++]  = (uint8_t)(v & 0xFF);
    }
    REG_UNLOCK();

    response[0] = slave_addr;
    response[1] = MODBUS_FUNC_READ_HOLDING;
    *resp_len   = n;
    return PROTOCOL_SUCCESS;
}
```

3. `ble_task` 的帧对象改静态（任务单实例，无并发）：

```c
static void ble_task(void* arg) {
    static bsp_bt401_frame_t f;      /* 130B 从栈移到 .bss */
    ...
}
```

做完 1+2+3，该链路栈占用降到 ~90 B，512B 有充裕余量。**但建议顺手把 `TASK_BLE_STACK` 提到 640**，
给 `LOG_PRINTF`（内部 `char buf[128]` + newlib `vsnprintf`）留出确定余量。

---

## P0-4 ADC 中断风暴

**位置**：`Core/Src/adc.c:48`（`ContinuousConvMode = ENABLE`）+ `bsp/bsp_ntc.c:196-204`

ADC 时钟 = PCLK2 / 8 = 9 MHz，采样 1.5 + 转换 12.5 = 14 cycle → **1.56 µs / 样本**。
DMA 缓冲 16 个样本 → `HAL_ADC_ConvCpltCallback` **每 25 µs 触发一次**，
回调里还执行 `HAL_ADC_Stop_DMA()` + `HAL_ADC_Start_DMA()`（含 HAL_LOCK、寄存器操作）。

→ **每秒约 4 万次中断**，每次 2~5 µs，CPU 被吃掉 10~20%；而业务代码 **500 ms 才取一次温度**。

**修复：控制周期 500ms 的温度采样根本不需要连续转换 + DMA 中断。改为按需轮询。**

```c
/* Core/Src/adc.c —— MX_ADC1_Init 里改 */
hadc1.Init.ContinuousConvMode = DISABLE;    /* 原来是 ENABLE */

/* bsp/bsp_ntc.c —— 用轮询替掉 DMA 路径 */
static int ntc_sample(uint32_t* adc_out) {
    uint32_t raw[NTC_FILTER_SAMPLES];

    if (HAL_ADC_Start(s_inst.hadc) != HAL_OK) return -1;
    for (uint8_t i = 0; i < NTC_FILTER_SAMPLES; i++) {
        if (HAL_ADC_PollForConversion(s_inst.hadc, 2) != HAL_OK) {
            HAL_ADC_Stop(s_inst.hadc);
            return -1;
        }
        raw[i] = HAL_ADC_GetValue(s_inst.hadc);
    }
    HAL_ADC_Stop(s_inst.hadc);

    *adc_out = ntc_moving_avg(ntc_median(raw, NTC_FILTER_SAMPLES));
    return 0;
}
```

耗时：5 × 1.56 µs ≈ **8 µs / 500ms**，占用率从 15% 降到 0.002%。
同时 `HAL_ADC_ConvCpltCallback`、双缓冲状态机 `data_ready/active_buf`、
`DMA1_Channel1_IRQn` 全部不再需要，可以删掉——少一份 ISR 竞态。

> 如果将来确实需要连续采样（比如过温保护要毫秒级响应），改用
> `HAL_ADC_Start_DMA(..., DMA_CIRCULAR)` + 读时取缓冲尾部最新 N 个，
> **不要**在回调里 Stop/Start。

> 采用 P0-4 的轮询方案后，本节下面的 **P0-5**（DMA 缓冲对齐错位）一并消失。
> 如果要保留 DMA 路径，**必须**先修 P0-5。

---

## P0-5 ADC DMA 缓冲对齐错位 → 温度恒为常数

**位置**：`Core/Src/adc.c:96`、`bsp/bsp_ntc.c:61`、`:213`

```c
/* adc.c */
hdma_adc1.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;   /* 每样本 2 字节 */

/* bsp_ntc.c */
uint32_t dma_buf[2][ADC_DMA_BUF_SIZE];        /* 16 个 uint32 = 64 字节 */
```

HAL 的传输长度单位是**数据项数**，halfword 对齐下 16 项 = **32 字节**，
而缓冲声明了 64 字节。DMA 只覆盖 `dma_buf[0..7]`，`dma_buf[8..15]` 永远是初始化值
`ADC_MAX_VALUE / 2`（2047，见 `bsp_ntc.c:282`）。

**致命的是取值正好从尾部读**：

```c
dest[i] = s_inst.dma_buf[ready_buf][(ADC_DMA_BUF_SIZE - n + i) & (ADC_DMA_BUF_SIZE - 1)];
/* n = 5 → 索引 11,12,13,14,15  ← 全部落在未写入区 */
```

→ 每次采样都拿到常数 2047 → 查表恒为 **≈26.6°C**，与真实温度无关。

连锁后果：
1. PID 的目标温度（35/45/55）与测量值恒定差 8~28°C → 一直在最大输出打满；
2. `ntc_detect_fault()` 的"ADC 值不变"判据**每 5 秒必然命中**
   （`STUCK_THRESHOLD = 10` × 500ms）→ `fault_active = true`；
3. `heat_pid_control()` 开头 `bsp_ntc_is_fault()` 命中 → `heat_stop_all()`
   → **加热开 5 秒后必然自动停机**，且 `fault_active` 只有显式 `bsp_ntc_clear_fault()`
   才会清除，全工程无人调用 → 之后永久拒绝加热。

这一条解释了"为什么加热功能实际不work"，比 P0-1 更隐蔽（P0-1 是关不掉，这条是开 5 秒就掉）。

**修复（保留 DMA 时）**：

```c
struct bsp_ntc_s {
    ...
    uint16_t dma_buf[2][ADC_DMA_BUF_SIZE];   /* 与 HALFWORD 对齐一致：16 × 2 = 32 字节 */
};
```

并把取样窗口改成滑窗内**新增**样本，而不是固定尾部：

```c
/* 只取上一次回调写入的整块，n 个样本从头部取，语义清晰 */
dest[i] = s_inst.dma_buf[ready_buf][i];
```

**修复（推荐，即 P0-4 方案）**：删掉 DMA 路径，改轮询，此问题不存在。

---

# P1 — 性能与实时性

## P1-1 日志与 Modbus 帧共用 USART3，且发送无互斥

**位置**：`app/app_config.h:97`、`bsp/bsp_bt401.c:126-152`、`components/protocol/modbus_slave.c:534`、`:693`

`LOG_PRINTF` → `bsp_bt401_printf(g_bt401, ...)` → `HAL_UART_Transmit`，
和 Modbus 响应 / 状态上传 **走同一个 USART3**。两个后果：

1. **字节流污染**：主机在等 `01 03 06 ...` 响应，中间插进 `Temp: 45.2 ...` 的 ASCII → 帧解析全废。
2. **无 TX 互斥**：`ble_task`（协议响应）与 `heat_task`（状态上传）、
   `evt_disp`（`music_*` / `ble_mode`）三方并发调用，`HAL_UART_Transmit` 不被 `__HAL_LOCK` 保护时字节交错，
   被保护时后到者直接返回 `HAL_BUSY` → **静默丢帧**。

**修复优先级从高到低**：

1. **物理隔离**（最优）：日志改走独立 UART（如 USART1/USART2 空闲脚）或 SWO。BLE 透传口只跑协议帧。
2. **单通道方案**：`LOG_ENABLE` 在生产固件置 0，仅调试构建打开；同时加 TX 互斥 + 有限超时。

```c
/* bsp_bt401.c */
static osMutexId_t s_tx_mutex;

static bsp_status_t bt401_tx(const uint8_t* b, uint16_t n) {
    if (!s_inited) return BSP_ERR_NOTINIT;
    if (osMutexAcquire(s_tx_mutex, pdMS_TO_TICKS(100)) != osOK) {
        s_inst.tx_drop++;
        return BSP_ERR_BUSY;
    }
    HAL_StatusTypeDef st = HAL_UART_Transmit(s_inst.huart, (uint8_t*)b, n, 100);  /* 不用 HAL_MAX_DELAY */
    osMutexRelease(s_tx_mutex);
    return (st == HAL_OK) ? BSP_OK : BSP_ERR_HW;
}
```

`HAL_MAX_DELAY` 必须去掉：UART 若因硬件故障不产生 TXE，任务会永久挂在那里，
连看门狗都喂不上（`ble_task` 是喂狗者）。

3. **根治**：改成 TX 环形队列 + `HAL_UART_Transmit_DMA` 单发送者。
   `usart.c` 已经配好了 `hdma_usart3_tx`（DMA1_Channel2），但代码里一次都没用，
   `DMA1_Channel2_IRQHandler` 是空函数——白配。

## P1-2 `ble_task` 20ms 轮询 → 改信号量驱动

**位置**：`app/service/ble_service.c:44-69`

`osDelay(20)` 轮询，平均增加 10ms 响应延迟；Modbus RTU 对响应间隔敏感（标准要求 ≤ 从站处理时间，
但很多主站超时设 50~100ms），加上 P0-4 的 CPU 抢占，抖动不可控。

```c
/* bsp_bt401.c：ISR 末尾发信号 */
if (s_rx_sem) osSemaphoreRelease(s_rx_sem);

/* ble_service.c */
for (;;) {
    osSemaphoreAcquire(s_rx_sem, osWaitForever);     /* 有帧才醒，零延迟零空转 */
    while (bsp_bt401_get_frame(g_bt401, &f) == BSP_OK) { ... }
}
```

同时修 `HAL_UARTEx_RxEventCallback` 的**漏事件 bug**：当前只处理 `HAL_UART_RXEVENT_IDLE`，
一旦 DMA 填满 128 字节（`HAL_UART_RXEVENT_TC`），回调直接 `return`，
DMA 已停止且**不再重启** → RX 通道静默死亡，之后永久收不到数据。

```c
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef* huart, uint16_t Size) {
    if (!g_bt401_isr || huart->Instance != USART3) return;

    HAL_UART_RxEventTypeTypeDef ev = HAL_UARTEx_GetRxEventType(huart);
    if (ev != HAL_UART_RXEVENT_IDLE && ev != HAL_UART_RXEVENT_TC) return;

    s_inst.buf_lengths[s_inst.current_buf] = Size;
    bt401_queue_push(s_inst.current_buf);
    s_inst.current_buf = (s_inst.current_buf + 1) % BT401_DMA_BUF_COUNT;
    HAL_UARTEx_ReceiveToIdle_DMA(huart, s_inst.dma_buffer[s_inst.current_buf], BT401_DMA_BUF_SIZE);

    if (s_rx_sem) osSemaphoreRelease(s_rx_sem);
}
```

顺带：`bt401_queue_push()` 在 **ISR 里 memcpy 130 字节**。改成队列只存
`{buf_idx, len}`，`get_frame` 时再拷贝（此时已在任务上下文，且已切到下一缓冲，无覆盖风险）：

```c
typedef struct { uint8_t idx; uint16_t len; } bt401_slot_t;   /* 130B → 4B，队列省 500B RAM */
```

## P1-3 矩阵键盘扫描

**位置**：`bsp/bsp_key.c:91-114`

每 20ms × 5 行 × 2 次 `HAL_GPIO_Init()` = **每秒 500 次 GPIO 重配置**。
`HAL_GPIO_Init` 要读改写 CRL/CRH 并跑 `assert_param`，单次 1~2 µs，纯浪费。

**改法**：行线一次性配成开漏输出，扫描时只写 ODR，不再重配方向。

```c
/* init 时 */
cfg.Pin  = pins_b;
cfg.Mode = GPIO_MODE_OUTPUT_OD;      /* 开漏：拉低即选中，拉高即释放给上拉 */
cfg.Pull = GPIO_NOPULL;
HAL_GPIO_Init(GPIOB, &cfg);

/* 扫描时 */
for (uint8_t row = 0; row < s_inst.row_count; row++) {
    HAL_GPIO_WritePin(s_inst.row_port, s_inst.rows[row].output_pin, GPIO_PIN_RESET);
    /* 读列（列保持输入上拉，无需动配置） */
    ...
    HAL_GPIO_WritePin(s_inst.row_port, s_inst.rows[row].output_pin, GPIO_PIN_SET);
}
```

**另加去抖**：`key_service.c:59-73` 只有边沿检测、没有去抖确认。20ms 采样若遇抖动，
`before!=0 → now==0` 会直接判短按误触发。加 3 帧稳定确认：

```c
static uint8_t s_stable = 0, s_candidate = 0;
static uint8_t s_cnt = 0;

if (now == s_candidate) {
    if (++s_cnt >= KEY_DEBOUNCE_FRAMES) s_stable = now;   /* KEY_DEBOUNCE_FRAMES = 3 */
} else {
    s_candidate = now;
    s_cnt = 0;
}
/* 后续状态机用 s_stable 判断边沿，不再用 s_key_before */
```

---

# P2 — 资源瘦身

## P2-1 `configMAX_PRIORITIES 56` → 7

`FreeRTOSConfig.h:65`。FreeRTOS 为**每个优先级**分配一个就绪链表：

```c
static List_t pxReadyTasksLists[ configMAX_PRIORITIES ];
```

`List_t` = 20 B，`56 × 20 = 1120 B`，`7 × 20 = 140 B` → **省 980 B RAM**。
本工程最高优先级是 5（`TASK_BLE_PRIORITY`）+ `osPriorityAboveNormal` + timer/idle，7 个足够。

注意 CMSIS-RTOS v2 的 `osPriority_t` 是 -3..56，改小后必须重新核对所有 `osPriorityXXX`
的映射（`cmsis_os2.c` 里 `osPriorityToPriority()` 会 clamp）。用宏统一：

```c
#define TASK_PRIO_BLE      5
#define TASK_PRIO_EVT      4
#define TASK_PRIO_HEAT     3
#define TASK_PRIO_KEY      2
#define TASK_PRIO_IDLE     0
```

## P2-2 删除 `at_command_manager`

`components/protocol/at_command_manager.c` 被编进工程（`CMakeLists.txt:72`），
但 `grep` 全工程**零调用**。它还带两个 `osMessageQueueNew` + 一个 `osMutexNew`
（虽然未初始化），以及 `at_response_t resp` 结构里 128B 内嵌数组。

删除 → 省 **~1.2 KB Flash + 编译时间**，并消除一个潜在的内存泄漏入口。

`at_parser.c` 里真正在用的是 `decode_at_command()` 的 4 条 LED 指令。
可以和 `ble_service.c` 的 AT 处理合并成一个表驱动的 5 行函数，`at_parser.c` 也一并去掉。

## P2-3 CRC16 双表 → 位运算

`components/crc16/crc16.c` 用 `crc16_hi_table[256] + crc16_lo_table[256]` = **512 B Flash**。
本工程每次 Modbus 帧只算一次 CRC，长度 ≤ 40 字节，位运算版本完全够：

```c
uint16_t _calc_check_value(const uint8_t data[], uint32_t len) {
    uint16_t crc = 0xFFFF;
    while (len--) {
        crc ^= *data++;
        for (uint8_t i = 0; i < 8; i++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
    }
    return crc;
}
```

代价：40 字节 × 8 次迭代 ≈ 160 次循环 ≈ 5 µs（72MHz）。可忽略。**省 512 B Flash。**

## P2-4 浮点日志

`CMakeLists.txt:119` 的 `-u _printf_float` 是注释掉的，
但 `heat_service.c:245` 用了 `%.1f`：

```c
LOG_PRINTF("Heat level set to %d, target: %.1fC\r\n", new_level, t);
```

`nanospecs` 下未链接浮点支持，`%.1f` 输出**空/乱码**（`heat_pid_control` 里已经用整数拆分规避了，
说明作者踩过这个坑但漏了一处）。

二选一：
- 保持不链接浮点（推荐，省 ~7 KB Flash）：改成 `%d.%d` 整数拆分，与 `heat_pid_control` 一致。
- 链接浮点：`target_link_options(... -u _printf_float)`，Flash 从 37.6 KB → 44 KB+。

## P2-5 堆与静态分配

`configTOTAL_HEAP_SIZE 8192`，`heap_4`，全部 RTOS 对象动态创建（`osThreadNew` / `osTimerNew` /
`osMessageQueueNew` / `osMutexNew`）。而 `configSUPPORT_STATIC_ALLOCATION 1` 已经开了，
却一处没用。

收益：
- 8 KB 堆可以降到 2 KB（**省 6 KB RAM**，把 20 KB 的占用从 75.9% 压到 ~45%）
- 消除运行期分配失败路径（现在 `heat_create_resources()` 那些回滚代码可以整段删掉）
- 消除堆碎片

```c
/* 例：heat_task 静态化 */
static StaticTask_t   s_heat_tcb;
static StackType_t    s_heat_stack[TASK_HEAT_STACK / sizeof(StackType_t)];
static StaticQueue_t  s_heat_q;
static uint8_t        s_heat_qbuf[HEAT_QUEUE_LEN * sizeof(HeatMsg)];
static StaticMutex_t  s_heat_mtx;

const osThreadAttr_t attr = {
    .name       = TASK_HEAT_NAME,
    .cb_mem     = &s_heat_tcb,   .cb_size = sizeof(s_heat_tcb),
    .stack_mem  = s_heat_stack,  .stack_size = sizeof(s_heat_stack),
    .priority   = TASK_PRIO_HEAT,
};
```

`configTIMER_TASK_STACK_DEPTH 256` 单位是 **word**（= 1024 B），而定时器任务只跑
`beep_timer_cb` / `heating_timer_cb` / `remain_timer_cb` 三个极简回调。降到 128 word → **省 512 B**。

## P2-6 其他小项

| 位置 | 问题 | 改法 |
|------|------|------|
| `modbus_slave.c:32` | `g_register_table` 里 12 个 `const char*` 名字串，全工程无人读 | 删掉 `name` 字段，省 ~150 B Flash |
| `modbus_slave.c:16` | 本地 `#define MODBUS_SLAVE_ADDR 0x01` 遮蔽了 `app_config.h` 的同名配置 | 删本地定义，包含 `app_config.h` |
| `bsp_common.h:70` | `BSP_SAFE_FREE` 用了 `free()` 但没 include `<stdlib.h>`，且全工程无堆释放 | BSP 层承诺"零堆"，删掉该宏 |
| `app_config.h:4-5` | `CMSIS_TICKS` 定义在 include guard **之外** | 移进 guard 内；BSP 层不要间接依赖 `cmsis_os2.h` |
| `Core/Src/freertos.c:118-127` | `task_init()` 后 `osThreadExit()`，其后的死循环不可达 | 删死循环；`defaultTask` 512B 栈可省 |
| `bsp_bt401.c:161-163` | `__disable_irq()/__enable_irq()` 不保存 PRIMASK | 改 `uint32_t pm = __get_PRIMASK(); __disable_irq(); ... __set_PRIMASK(pm);` |
| 根目录 | `newlib_lock_glue.c` / `stm32_lock.h` 存在但**未加入 CMake**，而 `STM32_THREAD_SAFE_STRATEGY=4` 已定义 | 要么加入编译（获得 HAL/Newlib 互斥），要么去掉该宏定义 |

---

# P3 — 架构与可靠性

## P3-1 IWDG 未启用

`stm32f1xx_hal_conf.h:52` `HAL_IWDG_MODULE_ENABLED` 是**注释掉**的，
但 `ble_service.c:41-43` 有 `#ifdef HAL_IWDG_MODULE_ENABLED → HAL_IWDG_Refresh(&hiwdg)`，
README 也写着"独立看门狗（ble_task 喂狗）"。

→ 当前**没有看门狗**。对一个 55°C 加热设备，这是最该补的一项。

```c
/* 1. hal_conf.h 打开： #define HAL_IWDG_MODULE_ENABLED */
/* 2. 初始化（LSI 40kHz，预分频 64，重装 1250 → 2s） */
IWDG_HandleTypeDef hiwdg = {
    .Instance = IWDG,
    .Init = { .Prescaler = IWDG_PRESCALER_64, .Reload = 1250 },
};
HAL_IWDG_Init(&hiwdg);
```

**喂狗位置要改**：不能只放 `ble_task`。P0-2 那类死锁会连喂狗一起卡住，
2 秒后复位反而"救了"设备——但如果以后把喂狗挪到高优先级任务，死锁就变成不变砖。
推荐：喂狗放在**最低优先级的健康检查任务**，且该任务要确认 `heat_task` / `ble_task`
的存活心跳都在推进：

```c
static void health_task(void* arg) {
    for (;;) {
        uint32_t now = osKernelGetTickCount();
        bool ok = (now - s_ble_heartbeat  < 3000) &&
                  (now - s_heat_heartbeat < 3000);
        if (ok) HAL_IWDG_Refresh(&hiwdg);
        osDelay(500);
    }
}
```

## P3-2 事件总线空转 + 关机事件无人处理

`event_subscribe()` 全工程只调了 2 次（`EVENT_KEY_SHORT/LONG_PRESS`），
而 `event_publish()` 发了 5 种：

| 事件 | 发布点 | 订阅者 |
|------|--------|--------|
| `EVENT_HEAT_STATUS_CHANGE` | heat_service ×3 | ❌ 无 |
| `EVENT_NTC_FAULT` | heat_service | ❌ 无 |
| `EVENT_POWER_OFF` | app_main `on_reg_write` | ❌ 无 |
| `EVENT_KEY_*` | key_service | ✅ |

**`EVENT_POWER_OFF` 无订阅者 = Modbus 关机命令彻底无效**（这是 P0-1 之外的第二条断路）。

```c
/* app_main.c task_init() 里补 */
event_subscribe(EVENT_POWER_OFF, on_power_off);

static void on_power_off(EventType e, uint32_t p) {
    (void)e; (void)p;
    heat_status_set(HEAT_STATUS_STOP);   /* 关加热 */
    ble_mode(BT_MODE_OFF);               /* 关蓝牙/音乐 */
    bsp_led_set_mode(g_led, BSP_LED_B, BSP_LED_MODE_OFF, 0);
    /* 若硬件有 POWER_Pin 自锁，在此拉低 */
}
```

顺便评估：如果这些事件确实没有跨模块消费者，**直接删掉 event_bus**，
把 `on_key_event` 改成 `key_service` 注册的回调。
收益：省 16 × 20 B 订阅表 + 队列 + `evt_disp` 任务（256B 栈）+ 一次任务切换延迟。
按键从"扫描任务 → 队列 → 派发任务 → 业务"变成"扫描任务 → 业务"，路径短一半。

## P3-3 NTC 故障检测

`bsp_ntc.c:169-189`：

```c
if (adc_val == s_inst.last_raw_adc) {
    if (++s_inst.stuck_counter >= STUCK_THRESHOLD) { fault = true; }
}
```

判据是"滤波后的 ADC 值连续 10 次完全相同"。500ms 一次 → **5 秒**。
温度进入 PID 死区（±0.5°C）后读数本身就会稳定，若 ADC 噪声不足 ±1 LSB，
**开机 5 秒即误报卡死 → `heat_read_ntc_temp` 连续 3 次失败 → 停机**。

改法：卡死应判"**长时间既不变、也不在合理范围内**"，而不是"值没变"。

```c
#define NTC_STUCK_WINDOW   30      /* 30 × 500ms = 15s 无任何变化才怀疑 */

static bool ntc_detect_fault(uint32_t adc) {
    /* 1. 开路/短路：硬判据 */
    if (adc >= ADC_MAX_VALUE - 10 || adc <= 10) { s_inst.fault_active = true; return true; }

    /* 2. 卡死：滑窗内最大-最小 < 2 LSB 且持续够久 */
    s_inst.win[s_inst.win_idx] = adc;
    s_inst.win_idx = (s_inst.win_idx + 1) % 8;
    uint32_t mn = 0xFFFFFFFF, mx = 0;
    for (uint8_t i = 0; i < 8; i++) {
        if (s_inst.win[i] < mn) mn = s_inst.win[i];
        if (s_inst.win[i] > mx) mx = s_inst.win[i];
    }
    if ((mx - mn) <= 1) {
        if (++s_inst.stuck_counter >= NTC_STUCK_WINDOW) { s_inst.fault_active = true; return true; }
    } else {
        s_inst.stuck_counter = 0;
    }
    s_inst.fault_active = false;
    return false;
}
```

另修 `bsp_ntc_read()` 的边界：`filtered <= -20.0f || filtered >= 100.0f` 里
`<=` / `>=` 会把合法的 -20°C / 100°C 判成硬件故障，改 `<=` → `<`、`>=` → `>`。

## P3-4 定时逻辑冗余

`heat_service` 同时维护 `heating_timer`（一次性绝对时长）和 `remain_timer`（每秒递减
`remain_sec`）。两者都会触发停机，且用户中途改档/改定时时两者不同步。

建议**只保留 `remain_timer` 每秒递减**，到 0 停机；`heating_timer` 删掉。
少一个 osTimer、少一条并发路径、少一处 `remain_sec * 1000` 的乘法溢出风险
（720 分钟 × 60000 ms = 43,200,000，虽未溢出 uint32，但 32 位计时器上限 71.6 分钟，
`CMSIS_TICKS` 内部乘法在 tick=1000 时安全；若将来 tick 改成 10kHz 就会溢出）。

## P3-5 状态机清晰化

当前 `s_heat_data` 混了三个职责：**控制状态**（status）、**配置**（level/timer）、
**PID 运行量**（remain_sec/ntc_fail_count）。且 `ntc_fail_count` 在锁外读写。

建议：

```c
typedef struct {                 /* 只在 heat_task 上下文读写，无需锁 */
    bool     running;
    HeatLevel level;
    uint16_t target_temp_x10;    /* 定点整数，避免浮点 */
    uint16_t remain_sec;
    uint16_t set_time_min;
    uint8_t  ntc_fail;
} heat_state_t;

static heat_state_t s_state;                              /* 单写者：heat_task */

/* 对外只暴露快照 */
void heat_state_snapshot(heat_state_t* out) {
    taskENTER_CRITICAL(); *out = s_state; taskEXIT_CRITICAL();
}
```

所有控制命令仍走 `ctrl_queue` 进 `heat_task`，**锁全部去掉**——
队列本身就是序列化机制，mutex 是多余的。这样彻底消除 P0-1 那类"锁内改状态、锁外再读"的时序 bug。

---

# 建议落地顺序

| 阶段 | 内容 | 预期收益 | 风险 |
|------|------|----------|------|
| 1 | P0-5 + P0-1 + P0-2 + P0-3 | 加热真正可用、设备能正常关机、不再死机/挂死 | 低，纯 bug 修复 |
| 2 | P0-4 + P1-2 | CPU 占用 15% → <2%，响应延迟 10ms → <1ms | 低，行为等价 |
| 3 | P1-1 + P1-3 | 协议帧不再被污染，按键无毛刺 | 中，涉及 UART 架构 |
| 4 | P2-1/2/3/4 | RAM -1KB，Flash -1.9KB | 低 |
| 5 | P2-5 静态分配 | RAM -6KB（15.5 → ~9.5 KB） | 中，需全量回归 |
| 6 | P3-1/2/3 | 看门狗、关机链路闭环、NTC 不误报 | 中，需实测 |

---

# 验证方法

每一项都要有可观测证据，不要靠"应该没问题"：

1. **P0-5 + P0-1**：Modbus 写 `HeatingStatus=1` → PA11（TIM1_CH4）占空比应是目标温度驱动下随温度收敛的量，
   不再恒满或 5 秒归零；串口 `Temp:` 日志读数随手指捂热传感器而上升（不再是固定 ~26.6）。
2. **P0-1**：Modbus 写 `HeatingStatus=0` → 示波器看 PA11 占空比归零；`LED1` 熄灭；`LED2/5/6` 定时灯全灭。
3. **P0-2**：主机发 `01 03 00 0A 00 02 CRC`（读 ShortcutKey1/2）→ 必须在 100ms 内回响应，不能挂。
4. **P0-3**：`uxTaskGetStackHighWaterMark(ble_task)` 打印，要求剩余 > 200 字节；连续跑 1000 帧读请求。
5. **P0-4**：`osKernelGetTickCount()` 与周期任务实际节拍对比丢拍；或用 GPIO 翻转 + 示波器测 ISR 占用。
6. **P1-2**：`HAL_UARTEx_RxEventCallback` 里 `tx_drop` / RX 事件计数，确认 TC 事件也重启了 DMA。
7. **P2**：`arm-none-eabi-size build/Debug/LUNAR.elf` 对比 text/data/bss 三项前后差值。
8. **P3-1**：手动制造死循环，确认 2s 内复位。

# 主机侧回归测试

在 PC 上直接编译被测源文件，用 `shim/` 下的桩替代 HAL / CMSIS-RTOS2。
不需要交叉工具链，也不需要真机。

## 运行

```bash
cd tests/host && ./run.sh
```

Windows（Git Bash）同样可用。期望输出末尾：

```
== ALL MODBUS TESTS PASSED ==
== ALL HEAT CONTROL TESTS PASSED ==
=== 全部主机测试通过 ===
```

## 两个套件

### 1. Modbus 协议（`modbus_test.c`）

| # | 场景 | 断言 |
|---|------|------|
| 1 | 读 3 个加热寄存器 | 成功，`resp_len = 3 + 3x2`，加锁深度 1 |
| 2 | 读 `ShortcutKey1..2`（原死锁点） | 成功且 `lock_fail = 0` |
| 3 | 读只写寄存器 | `WRITE_ONLY`，异常码 0x02 |
| 4 | 写单寄存器 | 成功，`resp_len = 6` |
| 5 | 写越界值 | `INVALID_VALUE`，异常码 0x03 |
| 6 | 错误 CRC | `PROTOCOL_ERR_CRC` |
| 7 | 错误从机地址 | `PROTOCOL_ERR_INVALID_ADDR` |
| 8 | 读 5 个寄存器 | `resp_len = 13`，不溢出 40B 缓冲 |
| 9 | 写多寄存器 0x10 | 成功 |
| 10 | 写 UTC 高低字对 | 不触发重入锁 |

**关键点**：协议层已改为依赖注入（`protocol_set_tx` / `protocol_set_rtc_writer`），
不再需要任何 BSP 桩 —— `stubs.c` 现在是空文件。

### 2. 加热状态机（`heat_control_test.c`）

`heat_control.c` 是纯逻辑（零硬件、零 RTOS），可直接编译测试。

| # | 场景 | 断言 |
|---|------|------|
| 1 | 上电初始 | 停机，功率 0 |
| 2 | 开机加热 | 有输出 |
| 3 | **关机** | **功率必须归零**（回归原「关机失效」bug） |
| 4 | 关机幂等 | 连续 10 周期功率恒 0 |
| 5 | 重新开机 | 可恢复输出 |
| 6 | 档位切换 | 档3(55C)@50C 加热，档1(35C)@50C 不加热 |
| 7 | 定时到点 | 自动停机且功率归零 |
| 8 | NTC 连续失败 | 达阈值停机 |
| 9 | 温度无效 | 不得驱动加热器 |
| 10 | 越界参数 | 被忽略，不影响状态 |
| 11 | 定时递减 | 30 次 tick 后 remain 正确 |

## 桩说明

`shim/cmsis_os2.h` 把 `osMutexAcquire` 实现成**非递归语义**：已持有时再次 acquire
直接记 `g_lock_fail++` 并返回失败（真机上等价于 `osWaitForever` 永久阻塞）。
因此 Modbus 测试 2 可以在 PC 上捕获"回调内重入寄存器锁"这类死锁。

`shim/FreeRTOS.h` 提供 `StaticTask_t` / `StaticQueue_t` / `StaticSemaphore_t` /
`StaticTimer_t` 的最小定义，让静态分配代码能在主机上编译。

`shim/app_config.h` 提供 `MODBUS_SLAVE_ADDR`。

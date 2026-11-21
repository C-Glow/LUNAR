# LUNAR — STM32F103 FreeRTOS 加热控制器

基于 STM32F103C8T6 + FreeRTOS 的智能加热设备固件，支持 BLE 远程控制、矩阵键盘、PID 温控、Modbus RTU 协议。

## 硬件平台

| 组件 | 型号 |
|------|------|
| MCU | STM32F103C8T6 (72MHz, 64KB Flash, 20KB RAM) |
| 蓝牙 | BT401 (UART3, DMA+IDLE) |
| 键盘 | 5x5 矩阵键盘 + 独立电源键 |
| 温度传感器 | NTC 热敏电阻 (12位 ADC) |
| 加热元件 | PWM 驱动 (TIM1_CH4) |
| 显示 | 8 路 LED 指示灯 |
| 时钟 | RTC (LSE 32768Hz) |

## 软件架构

```
app/
├── app_main.c/h                 # 唯一编排点：初始化 BSP + 注入端口 + 路由回调
├── app_config.h                 # 应用级配置（分层约定见文件头）
├── core/
│   └── log.c/h                  #   日志（可注入写出器，不依赖具体串口）
├── service/                     # 业务服务层（零协议依赖）
│   ├── key_service.c/h          #   按键扫描 + 去抖 + 回调派发
│   ├── heat_control.c/h         #   加热状态机 + PID（纯逻辑，零硬件/零 RTOS）
│   ├── heat_indicator.c/h       #   LED 时间指示 + 蜂鸣器
│   ├── heat_service.c/h         #   加热编排：RTOS 资源 + 采样 + 输出翻译
│   └── ble_service.c/h          #   BLE 帧分发 + AT 命令执行 + 看门狗
bsp/                             # 板级驱动（单实例、零业务/协议依赖、接口不带句柄）
├── bsp_bt401.c/h                #   BLE UART DMA 驱动
├── bsp_ntc.c/h                  #   NTC 温度采样 + 故障检测
├── bsp_heat.c/h                 #   PWM 加热驱动
├── bsp_pwm.c/h                  #   PWM 封装
├── bsp_led.c/h                  #   LED 控制（含闪烁）
├── bsp_key.c/h                  #   矩阵键盘（开漏扫描）
├── bsp_buzzer.c/h               #   蜂鸣器
└── bsp_rtc.c/h                  #   RTC 时钟
components/
├── pid/                         #   工业级 PID 控制器
├── crc16/                       #   Modbus CRC16（位运算，零查表）
└── protocol/                    #   协议层（零 BSP 依赖、零服务依赖）
    ├── modbus_slave.c/h         #     Modbus RTU 从站
    └── at_parser.c/h            #     AT 命令解析（纯转换）
tests/host/                      # 主机侧回归测试（无需真机）
```

### 分层与解耦

| 约束 | 现状 |
|------|------|
| protocol 层零 BSP 依赖 | ✅ 收发/RTC 通过 `protocol_set_tx()` / `protocol_set_rtc_writer()` 注入 |
| service 与 protocol 互不引用 | ✅ 双向验证为 0 处 |
| protocol 层零服务依赖 | ✅ |
| BSP 层零业务/零协议 | ✅ |
| BSP 模块单实例、接口不带句柄 | ✅ 8/8 模块统一 |
| 头文件不泄漏 BSP 类型 | ✅ service/protocol 的 `.h` 均无 `bsp_*_t` |
| RTOS 对象静态分配 | ✅ 全部 `Static*_t`，堆仅 1 KB |

### 层间通信

不使用事件总线（唯一订阅者场景下收益低于成本），改为**注册回调**：

```
app_main.c（唯一跨层编排点）
    ├── key_service_set_handler(on_key_event)
    ├── ble_service_set_frame_handler(on_ble_frame)      → modbus_slave / ble AT
    ├── heat_service_set_state_handler(on_heat_state)    → 上传 + 日志
    ├── heat_service_set_upload_handler(on_heat_upload)
    ├── protocol_set_tx(port_bt401_tx)                   → 注入串口发送
    ├── protocol_set_rtc_writer(port_rtc_set_utc)         → 注入 RTC 写入
    └── protocol_register_read/write_callback(on_reg_*)  → 寄存器读写
```

### 加热控制的三段职责

原实现把状态机、PID、LED、蜂鸣器混在 575 行的 `heat_service.c` 里，
导致「关断」逻辑分散在三个函数中（这正是关机失效 bug 难发现的原因）。
现已拆分：

```
heat_control.c    状态机 + PID    ← 纯逻辑，可直接主机测试
heat_indicator.c  LED + 蜂鸣器    ← 只依赖 bsp_led / bsp_buzzer
heat_service.c    RTOS 编排       ← 唯一持有队列/定时器/互斥量
```

控制层只有一个决策点 `heat_control_step()`，关机是幂等动作：
只要 `status != RUNNING`，输出必定为 0。

## 通信协议

### Modbus RTU 寄存器映射

| 地址 | 名称 | 权限 | 范围 | 说明 |
|------|------|------|------|------|
| 0 | PowerSwitch | WO | 0-1 | 关机命令 |
| 1-2 | UTCTime | WO | 32-bit | UTC 时间戳 |
| 3-4 | AlarmSet | WO | 32-bit | 闹钟设置 |
| 5 | DeleteAlarm | WO | 1-2 | 删除闹钟 |
| 6 | ExecuteShortcut | WO | 1-2 | 执行快捷键 |
| 7 | HeatingStatus | RW | 0-1 | 加热开关 |
| 8 | HeatingLevel | RW | 0-2 | 档位 (35/45/55C) |
| 9 | HeatingTimer | RW | 0-720 | 定时分钟数 |
| 10-11 | ShortcutKey | RW | 0-65535 | 快捷键配置 |

### AT 命令

| 命令 | 功能 |
|------|------|
| `QM+00` / `QM+09` | 关闭蓝牙/音乐 LED |
| `QM+03` | 音乐模式 |
| `TS+00` | 蓝牙闪烁 |
| `TS+XX` | 蓝牙常亮 |

## 构建与烧录

### 前置条件

- STM32CubeCLT (GNU ARM Toolchain 14.3 + CMake 3.22 + Ninja)
- VS Code + CMake Tools 扩展
- OpenOCD + ST-Link

### 编译

```bash
cmake --preset Debug        # 日志关闭（默认）
cmake --build build/Debug

cmake --preset DebugLog     # 日志开启，用于串口观测
cmake --build build/DebugLog
```

#### 调试日志开关

日志模块 `app/core/log.c` 由构建选项 `ENABLE_LOG` 控制，**不需要改源码**：

| Preset | ENABLE_LOG | log.c 参与编译 | 说明 |
|--------|:---:|:---:|------|
| `Debug` / `Release` / `MinSizeRel` | OFF | 否 | 发布固件，日志零占用 |
| `DebugLog` | ON | 是 | 调试固件，串口输出 Temp/BLE/状态 |

也可以手动指定：

```bash
cmake -S . -B build/custom -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake \
      -DCMAKE_BUILD_TYPE=Release -DENABLE_LOG=ON
```

**关闭时的实际效果**（已用 `nm` / `objdump` 验证）：

- `log.c` 不进入构建图，不产生 `.obj`
- ELF 中不存在 `log_printf` / `log_set_writer` 符号
- `LOG_PRINTF(...)` 展开为 `((void)0)`，**实参不求值**
  → `vsnprintf` 调用、格式化字符串、128 B 栈缓冲全部消失
- `ble_task` 的帧统计变量 `s_stats` 一并剔除（避免只写不读的死变量）
- `heat_task` 的浮点温度拆分计算一并剔除

源码中判断日志是否启用请用 `LOG_IS_ENABLED`（0/1 常量），
注册写出器用 `LOG_SET_WRITER()` 宏，二者在关闭时都会自动消解。

### 烧录

```bash
openocd -f openOCD.cfg -c "program build/Debug/LUNAR.elf verify reset exit"
```

或使用 `openocd_flash/download.bat`

### 资源占用（实测，2026-11）

```
                       RAM        FLASH
Release  (日志关)   10440 B     35676 B     (51.0% / 54.4%)
Release  (日志开)   10456 B     36344 B     (51.1% / 55.5%)
Debug    (日志关)   10448 B     59760 B     (51.0% / 91.2%)
Debug    (日志开)   10472 B     60768 B     (51.1% / 92.7%)
```

> 关闭日志可省 **668 B Flash + 8~16 B RAM**（Release）。
> 重构前基线：RAM 16144 B (78.8%)、Release FLASH 37048 B。
> RTOS 对象改为静态分配后，`configTOTAL_HEAP_SIZE` 由 8 KB 降到 1 KB。

> Debug 构建（`-O0 -g3`）已接近 64 KB 上限（95%）。若需继续增加功能，
> 请用 `Release` 或 `RelWithDebInfo` 构建，或考虑裁剪 `HAL` 模块。
> 数据来源：链接期 `-Wl,--print-memory-usage`。

### 主机侧协议测试

不依赖真机的回归测试，覆盖 Modbus 从站与加热状态机：

```bash
cd tests/host && ./run.sh
```

| 套件 | 用例数 | 覆盖关键缺陷 |
|------|:---:|------|
| Modbus 协议 | 10 | 读 ShortcutKey 死锁、只写寄存器异常码、CRC、越界 |
| 加热状态机 | 11 | **关机必须真的归零**、幂等关断、定时到点、NTC 故障停机 |

详见 [tests/host/README.md](tests/host/README.md)。

### 任务调度（重构后）

| 任务 | 栈 | 优先级 | 周期 | 说明 |
|------|-----|--------|------|------|
| ble_task | 640B | 5 | 信号量驱动 | BLE 帧分发 + 喂狗 |
| heat_task | 512B | 3 | 500ms | 采样 + 控制输出 |
| key_scan | 256B | 2 | 20ms | 按键扫描 + 去抖 |

> 事件派发任务已移除（事件总线降级为直接回调），少一个任务与一次上下文切换。


## 安全特性

- **IWDG** 独立看门狗（ble_task 喂狗）
- **NTC 故障检测** 开路/短路/卡死 自动停机
- **堆栈水位** 定期打印 FreeRTOS 栈余量
- **PID 保护** 输出限幅 + 抗积分饱和 + 微分先行
- **DMA 统计** 溢出计数可查询

## 分支

| 分支 | 说明 |
|------|------|
| `main` | 重构后分层架构 (service / protocol 分离) |
| `legacy` | 重构前原始代码 |

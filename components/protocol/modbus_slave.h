#ifndef PROTOCOL_TASK_H
#define PROTOCOL_TASK_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* 寄存器访问权限 */
typedef enum {
    REG_ACCESS_WRITE_ONLY = 0x01,
    REG_ACCESS_READ_ONLY  = 0x02,
    REG_ACCESS_READ_WRITE = 0x03,
} RegisterAccess;

/* 寄存器定义 */
typedef struct {
    RegisterAccess access;
    uint16_t min_val;
    uint16_t max_val;
    uint16_t def_val; /* 当前未使用，保留以便将来做参数恢复出厂设置 */
} RegisterDescriptor;

/* 协议处理结果 */
typedef enum {
    PROTOCOL_SUCCESS = 0,
    PROTOCOL_ERR_INVALID_FRAME,
    PROTOCOL_ERR_INVALID_ADDR,
    PROTOCOL_ERR_INVALID_FUNC,
    PROTOCOL_ERR_INVALID_REG,
    PROTOCOL_ERR_INVALID_VALUE,
    PROTOCOL_ERR_CRC,
    PROTOCOL_ERR_WRITE_ONLY,
    PROTOCOL_ERR_READ_ONLY,
    PROTOCOL_ERR_BUSY,
} ProtocolResult;

/* 寄存器 ID */
typedef enum {
    REG_POWER_SWITCH = 0,    // 关机（只写）
    REG_UTC_TIMESTAMP_HIGH,  // UTC时间戳（高位，只写）
    REG_UTC_TIMESTAMP_LOW,   // UTC时间戳（低位，只写）
    REG_ALARM_SET_HIGH,      // 闹钟（高位，只写）
    REG_ALARM_SET_LOW,       // 闹钟（低位，只写）
    REG_DELETE_ALARM,        // 删除闹钟（只写）
    REG_EXECUTE_SHORTCUT,    // 执行快捷键（只写）
    REG_HEATING_STATUS,      // 热敷工作状态（读写）
    REG_HEATING_LEVEL,       // 热敷档位（读写，1-5档）
    REG_HEATING_TIMER,       // 热敷定时（读写，0-120分钟）
    REG_SHORTCUT_KEY1,       // 快捷键1配置（读写）
    REG_SHORTCUT_KEY2,       // 快捷键2配置（读写）
    REG_COUNT,
} RegisterID;

/* 注意：这两个回调在「持有 g_reg_mutex」的上下文中被调用。
 * 回调内严禁调用 register_read / register_write / register_batch_write / register_batch_read，
 * 否则会对同一个非递归互斥量二次 acquire，任务永久阻塞（osWaitForever）。
 * 需要原始寄存器值时请使用 register_read_unlocked()。 */
typedef void (*RegisterWriteCallback)(RegisterID reg, uint16_t value);
typedef uint16_t (*RegisterReadCallback)(RegisterID reg);

/* 外部端口（依赖注入，使协议层零 BSP 依赖） */

/** 响应帧发送端口 */
typedef void (*protocol_tx_fn_t)(const uint8_t* buf, uint16_t len);

/**
 * @brief UTC 写入端口
 * @param utc  Unix 时间戳
 * @return true 表示写入成功
 */
typedef bool (*protocol_rtc_fn_t)(uint32_t utc);

/* 定义在 modbus_slave.c */
extern const RegisterDescriptor g_register_table[REG_COUNT];

/* 基础协议接口 */
void protocol_init(void);  // 初始化协议模块（创建互斥锁）

/**
 * @brief 注入响应发送端口
 * @note  未注入时协议层静默丢弃响应（便于纯逻辑测试）
 */
void protocol_set_tx(protocol_tx_fn_t fn);

/**
 * @brief 注入 RTC 写入端口
 * @note  未注入时 UTC 写入视为成功
 */
void protocol_set_rtc_writer(protocol_rtc_fn_t fn);
ProtocolResult protocol_process_frame(const uint8_t* data, uint16_t len, uint8_t* response, uint16_t* resp_len);
bool protocol_handle_request(const uint8_t* data, size_t len);
void protocol_upload_heating_status(void);

/* 寄存器管理接口 */
ProtocolResult register_write(RegisterID reg, uint16_t value);
ProtocolResult register_batch_write(RegisterID start_reg, const uint16_t* values, uint8_t count);
uint16_t register_read(RegisterID reg);
/**
 * @brief 读取寄存器原始值（不加锁）
 * @note  仅供已持有寄存器锁的回调内部使用，外部调用请用 register_read()。
 */
uint16_t register_read_unlocked(RegisterID reg);
ProtocolResult register_batch_read(RegisterID start_reg, uint16_t* values, uint8_t count);

/* 回调注册接口 */
void protocol_register_write_callback(RegisterWriteCallback cb);
void protocol_register_read_callback(RegisterReadCallback cb);

/* 工具函数 */
uint16_t protocol_calc_crc16(const uint8_t* data, uint16_t len);
bool protocol_validate_crc(const uint8_t* data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* PROTOCOL_TASK_H */

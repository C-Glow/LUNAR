/**
 * @file modbus_slave.c
 * @brief Modbus RTU 从站
 *
 * 本文件不直接调用任何 BSP：收发与 RTC 写入通过 app_main 注入的端口完成。
 */
#include "modbus_slave.h"
/* 静态分配类型定义在 FreeRTOS.h */
#include "FreeRTOS.h"

#include "cmsis_os2.h"
#include "crc16.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "app_config.h"

#define MODBUS_FUNC_READ_HOLDING 0x03
#define MODBUS_FUNC_WRITE_MULTIPLE 0x10
#define MODBUS_FUNC_WRITE_SINGLE 0x06
#define MODBUS_EXCEPTION_ILLEGAL_FUNCTION 0x01
#define MODBUS_EXCEPTION_ILLEGAL_ADDRESS 0x02
#define MODBUS_EXCEPTION_ILLEGAL_VALUE 0x03

#define MIN_FRAME_LEN 4
#define READ_FRAME_MIN_LEN 8
#define WRITE_MULTI_MIN_LEN 9
#define MAX_REGISTERS_PER_FRAME 125  // Modbus标准限制
/* 一次读全部 12 个寄存器的响应最长 29 字节 */
#define RESPONSE_BUF_SIZE 40
#define HEATING_REG_COUNT 3  // 加热状态相关寄存器数量

_Static_assert(RESPONSE_BUF_SIZE >= (3 + REG_COUNT * 2 + 2), "RESPONSE_BUF_SIZE too small");

/* 寄存器描述表 */
const RegisterDescriptor g_register_table[REG_COUNT] = {
    {REG_ACCESS_WRITE_ONLY, 0, 1, 0},
    {REG_ACCESS_WRITE_ONLY, 0, 0xFFFF, 0},
    {REG_ACCESS_WRITE_ONLY, 0, 0xFFFF, 0},
    {REG_ACCESS_WRITE_ONLY, 0, 0xFFFF, 0},
    {REG_ACCESS_WRITE_ONLY, 0, 0xFFFF, 0},
    {REG_ACCESS_WRITE_ONLY, 0, 2, 0},
    {REG_ACCESS_WRITE_ONLY, 0, 2, 0},
    {REG_ACCESS_READ_WRITE, 0, 1, 0},
    {REG_ACCESS_READ_WRITE, 0, 2, 0},
    {REG_ACCESS_READ_WRITE, 0, 720, 0},
    {REG_ACCESS_READ_WRITE, 0, 0xFFFF, 0},
    {REG_ACCESS_READ_WRITE, 0, 0xFFFF, 0},
};

_Static_assert(sizeof(g_register_table) / sizeof(g_register_table[0]) == REG_COUNT,
               "register table size must match REG_COUNT");

static uint16_t g_registers[REG_COUNT]        = {0};
static RegisterWriteCallback g_write_callback = NULL;
static RegisterReadCallback g_read_callback   = NULL;
/* 外部端口：由 app_main 注入 */
static protocol_tx_fn_t g_tx_fn       = NULL; /* 发送响应帧 */
static protocol_rtc_fn_t g_rtc_set_fn = NULL; /* 写 UTC 时间戳 */
void protocol_set_tx(protocol_tx_fn_t fn) {
    g_tx_fn = fn;
}

void protocol_set_rtc_writer(protocol_rtc_fn_t fn) {
    g_rtc_set_fn = fn;
}

static osMutexId_t g_reg_mutex                = NULL;  // 寄存器互斥锁 (CMSIS-RTOS v2)
static StaticSemaphore_t g_reg_mutex_mem;              // 静态分配，零堆

static bool is_valid_register(RegisterID reg);
static bool check_register_access(RegisterID reg, bool is_write);
static bool validate_register_value(RegisterID reg, uint16_t value);
static ProtocolResult handle_read_registers(uint8_t slave_addr, uint16_t start, uint16_t count, uint8_t* response,
                                            uint16_t* resp_len);
static ProtocolResult handle_write_registers(uint8_t slave_addr, uint16_t start, uint16_t count, const uint8_t* data,
                                             uint16_t data_len, uint8_t* response, uint16_t* resp_len);
static ProtocolResult handle_write_single_register(uint8_t slave_addr, uint16_t reg, uint16_t value, uint8_t* response,
                                                   uint16_t* resp_len);
static void build_exception_response(uint8_t slave_addr, uint8_t func_code, uint8_t exception_code, uint8_t* response,
                                     uint16_t* resp_len);
static void build_success_response(uint8_t slave_addr, uint8_t func_code, const uint8_t* data, uint16_t data_len,
                                   uint8_t* response, uint16_t* resp_len);
static bool process_utc_timestamp(uint16_t start_reg, uint16_t reg_num, const uint8_t* write_buf);
static ProtocolResult internal_register_write(RegisterID reg, uint16_t value);

/* 寄存器锁：回调在持锁期间执行，回调内不得调用会再次加锁的接口 */
#define REG_LOCK() (osMutexAcquire(g_reg_mutex, osWaitForever) == osOK)
#define REG_UNLOCK() (osMutexRelease(g_reg_mutex))

/** 初始化协议模块，使用前调用一次 */
void protocol_init(void) {
    if (g_reg_mutex == NULL) {
        /* 递归锁：回调在持锁上下文中执行，误调 register_read 也不会自锁 */
        const osMutexAttr_t attr = {
            .name      = "reg_mtx",
            .attr_bits = osMutexRecursive,
            .cb_mem    = &g_reg_mutex_mem,
            .cb_size   = sizeof(g_reg_mutex_mem),
        };
        g_reg_mutex = osMutexNew(&attr);
    }
}

/** 检查寄存器ID是否有效 */
static bool is_valid_register(RegisterID reg) {
    return (reg < REG_COUNT);
}

static bool check_register_access(RegisterID reg, bool is_write) {
    if (!is_valid_register(reg)) {
        return false;
    }

    RegisterAccess access = g_register_table[reg].access;

    if (is_write) {
        return (access == REG_ACCESS_WRITE_ONLY || access == REG_ACCESS_READ_WRITE);
    } else {
        return (access == REG_ACCESS_READ_ONLY || access == REG_ACCESS_READ_WRITE);
    }
}

/** 验证寄存器值范围 */
static bool validate_register_value(RegisterID reg, uint16_t value) {
    if (!is_valid_register(reg)) {
        return false;
    }

    const RegisterDescriptor* desc = &g_register_table[reg];

    switch (reg) {
    case REG_DELETE_ALARM:
    case REG_EXECUTE_SHORTCUT:
        return (value == 1 || value == 2);
    case REG_UTC_TIMESTAMP_HIGH:
    case REG_UTC_TIMESTAMP_LOW:
    case REG_ALARM_SET_HIGH:
    case REG_ALARM_SET_LOW:
        /* 由组合逻辑验证 */
        return true;
    default:

        if (desc->min_val == 0 && desc->max_val == 0) {
            return true;  // 无范围限制
        }
        return (value >= desc->min_val && value <= desc->max_val);
    }
}

/** 处理UTC时间戳特殊逻辑 */
static bool process_utc_timestamp(uint16_t start_reg, uint16_t reg_num, const uint8_t* write_buf) {
    if ((start_reg == REG_UTC_TIMESTAMP_HIGH) && (reg_num >= 2)) {
        uint16_t high_val = (write_buf[0] << 8) | write_buf[1];
        uint16_t low_val  = (write_buf[2] << 8) | write_buf[3];
        uint32_t utc_full = ((uint32_t)high_val << 16) | low_val;

        g_registers[REG_UTC_TIMESTAMP_HIGH] = high_val;
        g_registers[REG_UTC_TIMESTAMP_LOW]  = low_val;

        /* 无注入端口时视为成功，便于协议层单测 */
        if (!g_rtc_set_fn || g_rtc_set_fn(utc_full)) {
            return true;
        }
    }
    return false;
}

/** 写入单个寄存器（内部实现） */
static ProtocolResult internal_register_write(RegisterID reg, uint16_t value) {
    if (!is_valid_register(reg)) {
        return PROTOCOL_ERR_INVALID_REG;
    }

    if (!check_register_access(reg, true)) {
        return PROTOCOL_ERR_WRITE_ONLY;
    }

    if (!validate_register_value(reg, value)) {
        return PROTOCOL_ERR_INVALID_VALUE;
    }

    if (REG_LOCK() != true) {
        return PROTOCOL_ERR_BUSY;
    }

    switch (reg) {
    case REG_UTC_TIMESTAMP_HIGH:
    case REG_UTC_TIMESTAMP_LOW:
        /* UTC 必须高低字一起写 */
        REG_UNLOCK();
        return PROTOCOL_ERR_INVALID_VALUE;

    default:
        g_registers[reg] = value;

        if (g_write_callback != NULL) {
            g_write_callback(reg, value);
        }
        break;
    }

    REG_UNLOCK();
    return PROTOCOL_SUCCESS;
}

/** 处理读取寄存器请求 */
static ProtocolResult handle_read_registers(uint8_t slave_addr, uint16_t start, uint16_t count, uint8_t* response,
                                            uint16_t* resp_len) {
    if (count == 0 || count > MAX_REGISTERS_PER_FRAME) {
        return PROTOCOL_ERR_INVALID_VALUE;
    }

    if ((uint32_t)start + count > (uint32_t)REG_COUNT) {
        return PROTOCOL_ERR_INVALID_REG;
    }

    for (uint16_t i = 0; i < count; i++) {
        RegisterID reg = (RegisterID)(start + i);
        if (!check_register_access(reg, false)) {
            return PROTOCOL_ERR_WRITE_ONLY;
        }
    }

    /* 直接在响应缓冲上构造，省去中间数组 */
    response[0] = slave_addr;
    response[1] = MODBUS_FUNC_READ_HOLDING;
    response[2] = (uint8_t)(count * 2);  // 字节数
    uint16_t n  = 3;

    if (REG_LOCK() != true) {
        return PROTOCOL_ERR_BUSY;
    }

    for (uint16_t i = 0; i < count; i++) {
        RegisterID reg = (RegisterID)(start + i);
        uint16_t value;

        if (g_read_callback != NULL) {
            value = g_read_callback(reg);
        } else {
            value = g_registers[reg];
        }

        response[n++] = (uint8_t)((value >> 8) & 0xFF);
        response[n++] = (uint8_t)(value & 0xFF);
    }

    REG_UNLOCK();

    *resp_len = n;
    return PROTOCOL_SUCCESS;
}

/** 处理写入多个寄存器请求 */
static ProtocolResult handle_write_registers(uint8_t slave_addr, uint16_t start, uint16_t count, const uint8_t* data,
                                             uint16_t data_len, uint8_t* response, uint16_t* resp_len) {
    if (count == 0 || count > MAX_REGISTERS_PER_FRAME) {
        return PROTOCOL_ERR_INVALID_VALUE;
    }

    if ((start + count) > REG_COUNT) {
        return PROTOCOL_ERR_INVALID_REG;
    }

    if (data_len != (count * 2)) {
        return PROTOCOL_ERR_INVALID_FRAME;
    }

    /* 阶段 1：预验证，不持锁 */
    uint16_t bytes_processed = 0;
    for (uint16_t i = 0; i < count; i++) {
        RegisterID reg = start + i;

        /* UTC 由组合逻辑验证 */
        if (reg == REG_UTC_TIMESTAMP_HIGH || reg == REG_UTC_TIMESTAMP_LOW) {
            bytes_processed += 2;
            continue;
        }

        uint16_t value = (data[bytes_processed] << 8) | data[bytes_processed + 1];
        bytes_processed += 2;

        if (!check_register_access(reg, true)) {

            return PROTOCOL_ERR_READ_ONLY;
        }

        if (!validate_register_value(reg, value)) {
            return PROTOCOL_ERR_INVALID_VALUE;
        }
    }

    /* 阶段 2：持锁写入 */
    if (REG_LOCK() != true) {
        return PROTOCOL_ERR_BUSY;
    }

    /* 仅当写入范围覆盖完整 UTC 高低字对时才处理 */
    if (start <= REG_UTC_TIMESTAMP_HIGH && (uint32_t)start + count > (uint32_t)REG_UTC_TIMESTAMP_LOW) {
        if (start != REG_UTC_TIMESTAMP_HIGH) {
            REG_UNLOCK();
            return PROTOCOL_ERR_INVALID_VALUE;  // 不允许只写 UTC 低位
        }
        if (!process_utc_timestamp(start, count, data)) {
            REG_UNLOCK();
            return PROTOCOL_ERR_INVALID_VALUE;
        }
    }

    bytes_processed = 0;
    for (uint16_t i = 0; i < count; i++) {
        RegisterID reg = start + i;

        if (reg == REG_UTC_TIMESTAMP_HIGH || reg == REG_UTC_TIMESTAMP_LOW) {
            bytes_processed += 2;
            continue;
        }

        uint16_t value = (data[bytes_processed] << 8) | data[bytes_processed + 1];
        bytes_processed += 2;

        g_registers[reg] = value;
    }

    REG_UNLOCK();

    /* 阶段 3：通知应用层，不持锁 */
    bytes_processed = 0;
    for (uint16_t i = 0; i < count; i++) {
        RegisterID reg = start + i;
        if (reg == REG_UTC_TIMESTAMP_HIGH || reg == REG_UTC_TIMESTAMP_LOW) {
            bytes_processed += 2;
            continue;
        }

        uint16_t value = (data[bytes_processed] << 8) | data[bytes_processed + 1];
        bytes_processed += 2;

        if (g_write_callback != NULL) {
            g_write_callback(reg, value);
        }
    }

    build_success_response(slave_addr, MODBUS_FUNC_WRITE_MULTIPLE, NULL, 0, response, resp_len);
    return PROTOCOL_SUCCESS;
}

/** 处理写单个寄存器请求 */
static ProtocolResult handle_write_single_register(uint8_t slave_addr, uint16_t reg, uint16_t value, uint8_t* response,
                                                   uint16_t* resp_len) {
    if (reg >= REG_COUNT) {
        return PROTOCOL_ERR_INVALID_REG;
    }

    ProtocolResult result = internal_register_write((RegisterID)reg, value);
    if (result != PROTOCOL_SUCCESS) {
        return result;
    }

    /* 响应回显寄存器地址与值 */
    uint8_t data[4] = {(uint8_t)((reg >> 8) & 0xFF), (uint8_t)(reg & 0xFF), (uint8_t)((value >> 8) & 0xFF),
                       (uint8_t)(value & 0xFF)};

    build_success_response(slave_addr, MODBUS_FUNC_WRITE_SINGLE, data, 4, response, resp_len);
    return PROTOCOL_SUCCESS;
}

/** 构建异常响应 */
static void build_exception_response(uint8_t slave_addr, uint8_t func_code, uint8_t exception_code, uint8_t* response,
                                     uint16_t* resp_len) {
    response[0] = slave_addr;
    response[1] = func_code | 0x80;  // 设置异常标志
    response[2] = exception_code;
    *resp_len   = 3;
}

/** 构建成功响应 */
static void build_success_response(uint8_t slave_addr, uint8_t func_code, const uint8_t* data, uint16_t data_len,
                                   uint8_t* response, uint16_t* resp_len) {
    response[0] = slave_addr;
    response[1] = func_code;

    if (data != NULL && data_len > 0) {
        memcpy(&response[2], data, data_len);
        *resp_len = data_len + 2;
    } else {
        *resp_len = 2;
    }
}

/** 计算CRC16校验码 */
uint16_t protocol_calc_crc16(const uint8_t* data, uint16_t len) {
    return Modbus_CRC16(data, len);
}

/** 验证CRC */
bool protocol_validate_crc(const uint8_t* data, uint16_t len) {
    if (len < 2) {
        return false;
    }

    uint16_t crc_calc = protocol_calc_crc16(data, len - 2);
    uint16_t crc_recv = (data[len - 1] << 8) | data[len - 2];  // 注意：Modbus CRC是小端序

    return (crc_calc == crc_recv);
}

/** 处理Modbus协议帧 */
ProtocolResult protocol_process_frame(const uint8_t* data, uint16_t len, uint8_t* response, uint16_t* resp_len) {
    if (data == NULL || response == NULL || resp_len == NULL) {
        return PROTOCOL_ERR_INVALID_FRAME;
    }

    if (len < MIN_FRAME_LEN) {
        return PROTOCOL_ERR_INVALID_FRAME;
    }

    uint8_t slave_addr = data[0];
    if (slave_addr != MODBUS_SLAVE_ADDR) {
        return PROTOCOL_ERR_INVALID_ADDR;
    }

    if (!protocol_validate_crc(data, len)) {
        return PROTOCOL_ERR_CRC;
    }

    uint8_t func_code     = data[1];
    ProtocolResult result = PROTOCOL_SUCCESS;

    switch (func_code) {
    case MODBUS_FUNC_READ_HOLDING: {
        if (len < READ_FRAME_MIN_LEN) {
            result = PROTOCOL_ERR_INVALID_FRAME;
            break;
        }

        uint16_t start_reg = (data[2] << 8) | data[3];
        uint16_t reg_count = (data[4] << 8) | data[5];

        result = handle_read_registers(slave_addr, start_reg, reg_count, response, resp_len);
        break;
    }

    case MODBUS_FUNC_WRITE_MULTIPLE: {
        if (len < WRITE_MULTI_MIN_LEN) {
            result = PROTOCOL_ERR_INVALID_FRAME;
            break;
        }

        uint16_t start_reg = (data[2] << 8) | data[3];
        uint16_t reg_count = (data[4] << 8) | data[5];
        uint8_t byte_count = data[6];

        if ((7 + byte_count) > len) {
            result = PROTOCOL_ERR_INVALID_FRAME;
            break;
        }

        const uint8_t* write_data = &data[7];

        result = handle_write_registers(slave_addr, start_reg, reg_count, write_data, byte_count, response, resp_len);
        break;
    }

    case MODBUS_FUNC_WRITE_SINGLE: {
        if (len != 8) {  // 地址+功能码+寄存器地址+寄存器值+CRC(2字节)
            result = PROTOCOL_ERR_INVALID_FRAME;
            break;
        }

        uint16_t reg_addr  = (data[2] << 8) | data[3];
        uint16_t reg_value = (data[4] << 8) | data[5];

        result = handle_write_single_register(slave_addr, reg_addr, reg_value, response, resp_len);
        break;
    }

    default:
        result = PROTOCOL_ERR_INVALID_FUNC;
        break;
    }

    if (result != PROTOCOL_SUCCESS) {
        uint8_t exception_code;

        switch (result) {
        case PROTOCOL_ERR_INVALID_ADDR:
        case PROTOCOL_ERR_INVALID_REG:
        case PROTOCOL_ERR_WRITE_ONLY:  // 读取了只写寄存器
        case PROTOCOL_ERR_READ_ONLY:   // 写入了只读寄存器
            exception_code = MODBUS_EXCEPTION_ILLEGAL_ADDRESS;
            break;
        case PROTOCOL_ERR_INVALID_VALUE:
            exception_code = MODBUS_EXCEPTION_ILLEGAL_VALUE;
            break;
        case PROTOCOL_ERR_INVALID_FUNC:
        default:
            exception_code = MODBUS_EXCEPTION_ILLEGAL_FUNCTION;
            break;
        }

        build_exception_response(slave_addr, func_code, exception_code, response, resp_len);
    }

    return result;
}

/** 处理Modbus请求帧并发送响应（包含异常响应） */
bool protocol_handle_request(const uint8_t* data, size_t len) {
    uint8_t response[RESPONSE_BUF_SIZE];
    uint16_t resp_len = 0;

    ProtocolResult result = protocol_process_frame(data, (uint16_t)len, response, &resp_len);

    if (resp_len > 0) {
        uint16_t crc           = protocol_calc_crc16(response, resp_len);
        response[resp_len]     = (uint8_t)(crc & 0xFF);
        response[resp_len + 1] = (uint8_t)((crc >> 8) & 0xFF);

        if (g_tx_fn) {
            g_tx_fn(response, (uint16_t)(resp_len + 2));
        }
        return (result == PROTOCOL_SUCCESS);
    }

    return false;
}

/** 写入单个寄存器 */
ProtocolResult register_write(RegisterID reg, uint16_t value) {
    return internal_register_write(reg, value);
}

/** 批量写入寄存器 */
ProtocolResult register_batch_write(RegisterID start_reg, const uint16_t* values, uint8_t count) {
    if (!is_valid_register(start_reg) || ((uint16_t)start_reg + count) > REG_COUNT || values == NULL) {
        return PROTOCOL_ERR_INVALID_REG;
    }

    for (uint8_t i = 0; i < count; i++) {
        RegisterID reg = (RegisterID)(start_reg + i);

        if (!check_register_access(reg, true)) {

            return PROTOCOL_ERR_READ_ONLY;
        }

        if (!validate_register_value(reg, values[i])) {
            return PROTOCOL_ERR_INVALID_VALUE;
        }
    }

    if (REG_LOCK() != true) {
        return PROTOCOL_ERR_BUSY;
    }

    for (uint8_t i = 0; i < count; i++) {
        RegisterID reg   = (RegisterID)(start_reg + i);
        g_registers[reg] = values[i];

        if (g_write_callback != NULL) {
            g_write_callback(reg, values[i]);
        }
    }

    REG_UNLOCK();
    return PROTOCOL_SUCCESS;
}

/** 读取单个寄存器（不加锁） */
uint16_t register_read_unlocked(RegisterID reg) {
    return is_valid_register(reg) ? g_registers[reg] : 0xFFFF;
}

/** 读取单个寄存器 */
uint16_t register_read(RegisterID reg) {
    if (!is_valid_register(reg)) {
        return 0xFFFF;
    }

    uint16_t value;

    if (REG_LOCK() != true) {
        return 0xFFFF;
    }

    if (g_read_callback != NULL) {
        value = g_read_callback(reg);
    } else {
        value = g_registers[reg];
    }

    REG_UNLOCK();
    return value;
}

/** 批量读取寄存器 */
ProtocolResult register_batch_read(RegisterID start_reg, uint16_t* values, uint8_t count) {
    if (!is_valid_register(start_reg) || ((uint16_t)start_reg + count) > REG_COUNT || values == NULL) {
        return PROTOCOL_ERR_INVALID_REG;
    }

    if (REG_LOCK() != true) {
        return PROTOCOL_ERR_BUSY;
    }

    for (uint8_t i = 0; i < count; i++) {
        RegisterID reg = (RegisterID)(start_reg + i);

        if (!check_register_access(reg, false)) {
            REG_UNLOCK();
            return PROTOCOL_ERR_WRITE_ONLY;
        }

        if (g_read_callback != NULL) {
            values[i] = g_read_callback(reg);
        } else {
            values[i] = g_registers[reg];
        }
    }

    REG_UNLOCK();
    return PROTOCOL_SUCCESS;
}

/** 注册写回调函数 */
void protocol_register_write_callback(RegisterWriteCallback cb) {
    g_write_callback = cb;
}

/** 注册读回调函数 */
void protocol_register_read_callback(RegisterReadCallback cb) {
    g_read_callback = cb;
}

/** 上传加热状态 */
void protocol_upload_heating_status(void) {
    uint8_t response[RESPONSE_BUF_SIZE] = {0};

    response[0] = MODBUS_SLAVE_ADDR;
    response[1] = MODBUS_FUNC_READ_HOLDING;
    response[2] = HEATING_REG_COUNT * 2;  // N个寄存器 × 2字节

    if (REG_LOCK() != true) {
        return;
    }

    for (uint8_t i = 0; i < HEATING_REG_COUNT; i++) {
        RegisterID reg = REG_HEATING_STATUS + i;
        uint16_t value;

        if (g_read_callback != NULL) {
            value = g_read_callback(reg);
        } else {
            value = g_registers[reg];
        }

        response[3 + (i * 2)]     = (uint8_t)((value >> 8) & 0xFF);
        response[3 + (i * 2) + 1] = (uint8_t)(value & 0xFF);
    }

    REG_UNLOCK();

    uint16_t resp_len = 3 + (HEATING_REG_COUNT * 2);

    uint16_t crc           = protocol_calc_crc16(response, resp_len);
    response[resp_len]     = (uint8_t)(crc & 0xFF);
    response[resp_len + 1] = (uint8_t)((crc >> 8) & 0xFF);

    if (g_tx_fn) {
        g_tx_fn(response, (uint16_t)(resp_len + 2));
    }
}

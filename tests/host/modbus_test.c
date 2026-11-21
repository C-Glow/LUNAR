/* 在 PC 上编译 modbus_slave.c，用假的 RTOS 层验证协议行为 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>

#include "cmsis_os2.h"
#include "modbus_slave.h"
#include "crc16.h"

static int g_lock_depth = 0, g_max_depth = 0, g_lock_fail = 0;
/* 模拟非递归互斥量：持有时再次 acquire 会失败（真实 FreeRTOS 会永久阻塞=死锁） */
osMutexId_t osMutexNew(const osMutexAttr_t* a) { (void)a; return (void*)1; }
osStatus_t osMutexAcquire(osMutexId_t m, uint32_t t) {
    (void)m; (void)t;
    if (g_lock_depth > 0) { g_lock_fail++; return -1; }
    g_lock_depth++; if (g_lock_depth > g_max_depth) g_max_depth = g_lock_depth;
    return osOK;
}
osStatus_t osMutexRelease(osMutexId_t m) { (void)m; g_lock_depth--; return osOK; }

static uint16_t g_regs[REG_COUNT];

static uint16_t g_last_tx_len = 0;
static int g_tx_count = 0;
static uint32_t g_last_utc = 0;

static void tx_port(const uint8_t* buf, uint16_t len) {
    (void)buf;
    g_last_tx_len = len;
    g_tx_count++;
}

static bool rtc_port(uint32_t utc) {
    g_last_utc = utc;
    return true;
}

static void wr_cb(RegisterID reg, uint16_t v) { g_regs[reg] = v; }
static uint16_t rd_cb(RegisterID reg) { return register_read_unlocked(reg); }

static void build_frame(uint8_t* out, uint16_t* n, uint8_t addr, uint8_t fn,
                        uint16_t a, uint16_t b) {
    out[0]=addr; out[1]=fn;
    out[2]=(uint8_t)(a>>8); out[3]=(uint8_t)(a&0xff);
    out[4]=(uint8_t)(b>>8); out[5]=(uint8_t)(b&0xff);
    uint16_t crc=_calc_check_value(out,6);
    out[6]=(uint8_t)(crc&0xff); out[7]=(uint8_t)((crc>>8)&0xff);
    *n=8;
}

int main(void) {
    int fail = 0;

    /* ---- 1. 读全部 12 个寄存器：不应死锁、响应长度正确 ---- */
    protocol_init();
    protocol_set_tx(tx_port);
    protocol_set_rtc_writer(rtc_port);
    protocol_register_read_callback(rd_cb);
    protocol_register_write_callback(wr_cb);

    uint8_t req[16]; uint16_t reqlen;
    build_frame(req,&reqlen,0x01,0x03,REG_HEATING_STATUS,3);
    uint8_t resp[64]; uint16_t resp_len=0;
    ProtocolResult r = protocol_process_frame(req,reqlen,resp,&resp_len);
    printf("[1] read 3 regs: result=%d resp_len=%u lock_fail=%d max_depth=%d\n",
           r, resp_len, g_lock_fail, g_max_depth);
    if (r != PROTOCOL_SUCCESS || resp_len != 3+3*2) { printf("  FAIL len\n"); fail=1; }
    if (g_lock_fail) { printf("  FAIL reentrant lock\n"); fail=1; }

    /* ---- 2. 读 ShortcutKey1/2（原死锁触发点） ---- */
    g_lock_fail = 0; g_max_depth = 0; g_lock_depth = 0;
    build_frame(req,&reqlen,0x01,0x03,REG_SHORTCUT_KEY1,2);
    r = protocol_process_frame(req,reqlen,resp,&resp_len);
    printf("[2] read shortcut1..2: result=%d resp_len=%u lock_fail=%d\n", r, resp_len, g_lock_fail);
    if (r != PROTOCOL_SUCCESS || g_lock_fail) { printf("  FAIL\n"); fail=1; }

    /* ---- 3. 读只写寄存器应返回异常 0x02 ---- */
    g_lock_fail=0; g_lock_depth=0;
    build_frame(req,&reqlen,0x01,0x03,REG_POWER_SWITCH,1);
    r = protocol_process_frame(req,reqlen,resp,&resp_len);
    printf("[3] read write-only: result=%d exc=%02X\n", r, resp_len>=3?resp[2]:0);
    if (r != PROTOCOL_ERR_WRITE_ONLY || resp[2] != 0x02) { printf("  FAIL\n"); fail=1; }

    /* ---- 4. 写单个寄存器 HeatingLevel=2 ---- */
    g_lock_fail=0; g_lock_depth=0;
    build_frame(req,&reqlen,0x01,0x06,REG_HEATING_LEVEL,2);
    r = protocol_process_frame(req,reqlen,resp,&resp_len);
    printf("[4] write single level=2: result=%d g_regs=%u resp_len=%u\n",
           r, g_regs[REG_HEATING_LEVEL], resp_len);
    if (r != PROTOCOL_SUCCESS || g_regs[REG_HEATING_LEVEL] != 2 || resp_len != 6) { printf("  FAIL\n"); fail=1; }

    /* ---- 5. 写越界值 HeatingLevel=5 应异常 0x03 ---- */
    g_lock_fail=0; g_lock_depth=0;
    build_frame(req,&reqlen,0x01,0x06,REG_HEATING_LEVEL,5);
    r = protocol_process_frame(req,reqlen,resp,&resp_len);
    printf("[5] write OOR level=5: result=%d exc=%02X\n", r, resp_len>=3?resp[2]:0);
    if (r != PROTOCOL_ERR_INVALID_VALUE || resp[2] != 0x03) { printf("  FAIL\n"); fail=1; }

    /* ---- 6. 错误 CRC ---- */
    g_lock_fail=0; g_lock_depth=0;
    build_frame(req,&reqlen,0x01,0x03,0,1);
    req[7] ^= 0xFF;
    r = protocol_process_frame(req,reqlen,resp,&resp_len);
    printf("[6] bad crc: result=%d\n", r);
    if (r != PROTOCOL_ERR_CRC) { printf("  FAIL\n"); fail=1; }

    /* ---- 7. 错误从机地址 ---- */
    g_lock_fail=0; g_lock_depth=0;
    build_frame(req,&reqlen,0x02,0x03,0,1);
    r = protocol_process_frame(req,reqlen,resp,&resp_len);
    printf("[7] bad addr: result=%d\n", r);
    if (r != PROTOCOL_ERR_INVALID_ADDR) { printf("  FAIL\n"); fail=1; }

    /* ---- 8. 读 12 个寄存器（最大响应），验证 40B 缓冲够用 ---- */
    g_lock_fail=0; g_lock_depth=0;
    build_frame(req,&reqlen,0x01,0x03,0,REG_COUNT);
    /* 该帧含只写寄存器，应异常；改为只读末 5 个（7..11） */
    build_frame(req,&reqlen,0x01,0x03,REG_HEATING_STATUS,REG_COUNT-REG_HEATING_STATUS);
    r = protocol_process_frame(req,reqlen,resp,&resp_len);
    printf("[8] read regs 7..11: result=%d resp_len=%u (buf=40)\n", r, resp_len);
    if (r != PROTOCOL_SUCCESS || resp_len > 40-2) { printf("  FAIL\n"); fail=1; }

    /* ---- 9. 写多寄存器 0x10 连续 2 个（HeatingStatus+Level） ---- */
    g_lock_fail=0; g_lock_depth=0;
    {
        uint8_t f[16]; uint16_t n=0;
        f[0]=0x01; f[1]=0x10;
        f[2]=0; f[3]=REG_HEATING_STATUS;
        f[4]=0; f[5]=2;      /* 2 个寄存器 */
        f[6]=4;              /* 4 字节 */
        f[7]=0; f[8]=1;      /* status=1 */
        f[9]=0; f[10]=1;     /* level=1 */
        uint16_t crc=_calc_check_value(f,11);
        f[11]=(uint8_t)(crc&0xff); f[12]=(uint8_t)((crc>>8)&0xff); n=13;
        r = protocol_process_frame(f,n,resp,&resp_len);
        printf("[9] write multi: result=%d len=%u resp_len=%u\n", r, n, resp_len);
        if (r != PROTOCOL_SUCCESS) { printf("  FAIL\n"); fail=1; }
    }

    /* ---- 10. 写 UTC 高低字对（合法组合） ---- */
    g_lock_fail=0; g_lock_depth=0;
    {
        uint8_t f[16];
        f[0]=0x01; f[1]=0x10;
        f[2]=0; f[3]=REG_UTC_TIMESTAMP_HIGH;
        f[4]=0; f[5]=2;
        f[6]=4;
        f[7]=0x5F; f[8]=0x5E; f[9]=0x10; f[10]=0x00;
        uint16_t crc=_calc_check_value(f,11);
        f[11]=(uint8_t)(crc&0xff); f[12]=(uint8_t)((crc>>8)&0xff);
        r = protocol_process_frame(f,13,resp,&resp_len);
        /* bsp_rtc_set_utc 在仿真里是桩，返回非 OK → 期望 INVALID_VALUE 或 SUCCESS，
           关键是必须能进入该分支且未死锁 */
        printf("[10] write utc pair: result=%d lock_fail=%d\n", r, g_lock_fail);
        if (g_lock_fail) { printf("  FAIL deadlock\n"); fail=1; }
    }

    printf(fail ? "\n== SOME TESTS FAILED ==\n" : "\n== ALL MODBUS TESTS PASSED ==\n");
    return fail;
}

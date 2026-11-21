/**
 * @file crc16.c
 * @brief Modbus CRC16（位运算，多项式 0xA001）
 *
 * 返回值是标准 CRC 的字节交换形式，与 Modbus 帧内 CRC 字段顺序一致，
 * 不要改动字节序。
 */

#include "crc16.h"

uint16_t _calc_check_value(const uint8_t data[], uint32_t len) {
    uint16_t crc = 0xFFFF;

    while (len-- > 0) {
        crc ^= *data++;
        for (uint8_t i = 0; i < 8; i++) {
            if (crc & 1U) {
                crc = (uint16_t)((crc >> 1) ^ 0xA001U);
            } else {
                crc = (uint16_t)(crc >> 1);
            }
        }
    }

    /* 字节交换以匹配 Modbus CRC 字段顺序 */
    return (uint16_t)(((crc & 0xFFU) << 8) | (crc >> 8));
}

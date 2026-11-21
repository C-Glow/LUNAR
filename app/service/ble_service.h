/**
 * @file ble_service.h
 * @brief BLE服务接口：帧分发 + AT命令
 */

#ifndef BLE_SERVICE_H
#define BLE_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VOL_DIR_UP,
    VOL_DIR_DOWN
} volume_dir_t;
typedef enum {
    BT_MODE_OFF,
    BT_MODE_BT,
    BT_MODE_MUSIC
} bt_mode_t;

/* 帧处理回调：上层注册以解耦协议层 */
typedef void (*ble_frame_cb_t)(const uint8_t* data, uint16_t len, bool is_modbus);

void ble_service_init(void);

/**
 * @brief 处理一帧 AT 命令（含 LED 动作执行）
 * @note  由 app_main 的帧分发回调调用；解析在 at_parser（纯逻辑），
 *        执行在本模块（允许操作 BSP），保持 protocol 层零硬件依赖。
 */
void ble_handle_at_frame(const uint8_t* data, uint16_t len);
void ble_service_set_frame_handler(ble_frame_cb_t cb);
void bt_start(void);
void ble_mode(bt_mode_t mode);
void ble_query(void);
void music_switch(void);
void music_next(void);
void music_prev(void);
void music_volume_control(volume_dir_t dir);
void music_volume_set(uint8_t vol);

#ifdef __cplusplus
}
#endif
#endif

/**
 * @file app_handles.h
 * @brief 应用层共享的键值定义
 *
 * 各 BSP 模块为单实例，对外接口不带句柄参数，因此本文件不再声明全局句柄。
 */

#ifndef APP_HANDLES_H
#define APP_HANDLES_H

#ifdef __cplusplus
extern "C" {
#endif

/** 矩阵键盘返回的原始键值 */
typedef enum {
    KEY_NULL       = 0,
    KEY_MUSIC      = 1,
    KEY_BLUETOOTH  = 2,
    KEY_PLAY_PAUSE = 3,
    KEY_MIN10      = 4,
    KEY_MIN60      = 5,
    KEY_PREV       = 6,
    KEY_NEXT       = 7,
    KEY_VOL_DOWN   = 8,
    KEY_VOL_UP     = 9,
    KEY_HEAT_PLUS  = 10,
    KEY_HEAT_MINUS = 11,
    KEY_SHORTCUT_1 = 12,
    KEY_SHORTCUT_2 = 13,
    KEY_MIN30      = 14,
    KEY_HEAT       = 15,
    KEY_POWER      = 18
} app_key_t;

#ifdef __cplusplus
}
#endif

#endif /* APP_HANDLES_H */

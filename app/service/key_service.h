/**
 * @file key_service.h
 * @brief 按键服务：扫描、去抖、回调派发
 */

#ifndef KEY_SERVICE_H
#define KEY_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    KEY_EVENT_NONE = 0,
    KEY_EVENT_SHORT_PRESS,
    KEY_EVENT_LONG_PRESS
} key_event_t;

/** 按键事件回调，在 key_scan 任务上下文执行，不得长时间阻塞 */
typedef void (*key_event_cb_t)(uint8_t key_id, key_event_t event);

void key_service_init(void);

/** 注册回调；单订阅者，重复注册会覆盖 */
void key_service_set_handler(key_event_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* KEY_SERVICE_H */

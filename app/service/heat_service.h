/**
 * @file heat_service.h
 * @brief 加热服务对外接口
 */

#ifndef HEAT_SERVICE_H
#define HEAT_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "heat_control.h"

/** 状态上传回调，由上层注册以解耦协议层 */
typedef void (*heat_upload_cb_t)(void);

/** 状态变化回调，在 heat_task 上下文执行 */
typedef void (*heat_state_cb_t)(HeatStatus status);

bool heat_task_init(void);
void heat_task_deinit(void);

void heat_service_set_upload_handler(heat_upload_cb_t cb);
void heat_service_set_state_handler(heat_state_cb_t cb);

/** @return false 表示参数无效或队列发送失败 */
bool heat_status_set(HeatStatus status);

/** 运行 ↔ 停止 */
void heat_status_switch(void);

/** @return false 表示参数无效或队列发送失败 */
bool heat_level_set(HeatLevel level);

/** 档位加减，到达边界时蜂鸣提示 */
void heat_level_up(void);
void heat_level_down(void);

/** @param minute 0 表示取消定时，上限 HEAT_MAX_TIMER_MIN */
bool heat_timer_set(uint16_t minute);

HeatStatus heat_status_get(void);
HeatLevel heat_level_get(void);

/** @return 剩余分钟数，0 表示无定时 */
uint16_t heat_remain_time_get(void);

#endif /* HEAT_SERVICE_H */

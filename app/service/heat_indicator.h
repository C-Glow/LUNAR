/**
 * @file heat_indicator.h
 * @brief 加热相关指示输出：定时 LED 与蜂鸣器
 */

#ifndef HEAT_INDICATOR_H
#define HEAT_INDICATOR_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

void heat_indicator_init(void);

/** 刷新时间指示 LED；remain_sec 为 0 时全灭 */
void heat_indicator_set_remain(uint32_t remain_sec);

/** 加热工作指示灯（RF） */
void heat_indicator_set_heating(bool on);

/** 非阻塞蜂鸣，到时自动停止 */
void heat_indicator_beep(uint32_t duration_ms);

/** 全部指示归位，关机时调用 */
void heat_indicator_all_off(void);

#ifdef __cplusplus
}
#endif

#endif /* HEAT_INDICATOR_H */

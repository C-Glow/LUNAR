#ifndef FREERTOS_SHIM_H
#define FREERTOS_SHIM_H
/* 主机测试用最小桩：只提供静态分配所需的类型定义。
 * 真机上这些类型来自 Middlewares/Third_Party/FreeRTOS/Source/include/FreeRTOS.h */
#include <stdint.h>
#include <stddef.h>

typedef uint32_t StackType_t;
typedef struct { void* dummy[12]; } StaticTask_t;
typedef struct { void* dummy[10]; } StaticQueue_t;
typedef StaticQueue_t StaticSemaphore_t;
typedef struct { void* dummy[4]; } StaticTimer_t;

#endif

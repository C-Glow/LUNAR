/**
 * @file log.h
 * @brief 调试日志端口，可通过 ENABLE_LOG 构建选项整体剔除
 *
 * LOG_ENABLE 由 CMake 注入（见 CMakeLists.txt 的 ENABLE_LOG 选项），默认关闭。
 * 关闭时 log.c 不参与编译，LOG_PRINTF 展开为空语句且实参不求值。
 */

#ifndef LOG_H
#define LOG_H

#ifndef LOG_ENABLE
#define LOG_ENABLE 0
#endif

#if LOG_ENABLE

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 日志写出器签名 */
typedef int (*log_writer_t)(const char* fmt, ...);

/** 注册写出器；传 NULL 丢弃所有日志。须在调度器启动后、任务主循环前调用。 */
void log_set_writer(log_writer_t writer);

/** 格式化并输出一条日志。不得在中断上下文调用。 */
int log_printf(const char* fmt, ...);

#ifdef __cplusplus
}
#endif

#define LOG_IS_ENABLED 1
#define LOG_PRINTF(...) log_printf(__VA_ARGS__)
#define LOG_SET_WRITER(w) log_set_writer(w)

#else

#define LOG_IS_ENABLED 0
#define LOG_PRINTF(...) ((void)0)
#define LOG_SET_WRITER(w) ((void)0)

#endif /* LOG_ENABLE */

#endif /* LOG_H */

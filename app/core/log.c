/**
 * @file log.c
 * @brief 调试日志实现
 *
 * ENABLE_LOG=OFF 时本文件不参与编译，此处的 #if 为二次保护。
 */

#include "log.h"

#if LOG_ENABLE

#include <stddef.h>
#include <stdio.h>

static log_writer_t s_writer = NULL;

void log_set_writer(log_writer_t writer) {
    s_writer = writer;
}

int log_printf(const char* fmt, ...) {
    if (!s_writer || !fmt) {
        return -1;
    }

    /* 写出器接不了 va_list，先本地格式化再转发 */
    char buf[128];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (len < 0) {
        return -1;
    }
    if ((size_t)len >= sizeof(buf)) {
        len = (int)sizeof(buf) - 1;
    }

    return s_writer("%s", buf);
}

#endif /* LOG_ENABLE */

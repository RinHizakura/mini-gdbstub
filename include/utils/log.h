#ifndef LOG_H
#define LOG_H

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline void log_print(const char *format, ...)
{
    va_list vargs;
    va_start(vargs, format);
    vfprintf(stderr, format, vargs);
    va_end(vargs);
}

#define warn(format, ...) \
    log_print("ERRNO: %s\n" format, strerror(errno), ##__VA_ARGS__)
#define info(format, ...) log_print(format, ##__VA_ARGS__)

#endif

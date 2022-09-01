#ifndef LOG_H
#define LOG_H

#include <errno.h>
#include <string.h>

void __die(const char *format, ...);
void __info(const char *format, ...);

/* FIXME: possibly check if the ANSI color codes are supported? */
#define RED "\x1b[1;31m"
#define YELLOW "\x1b[1;33m"
#define BLUE "\x1b[1;94m"
#define NC "\x1b[0m"

#define die(format, ...)               \
    __die(RED "ERRNO: %s\n" NC format, \
          strerror(errno) __VA_OPT__(, ) __VA_ARGS__)
#define warn(format, ...)                  \
    __info(YELLOW "ERRNO: %s\n" NC format, \
           strerror(errno) __VA_OPT__(, ) __VA_ARGS__)
#define info(format, ...) __info(BLUE format NC __VA_OPT__(, ) __VA_ARGS__)
#endif

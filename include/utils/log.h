#ifndef LOG_H
#define LOG_H

#include <errno.h>
#include <string.h>

void __die(const char *format, ...);
void __info(const char *format, ...);

#define die(format, ...) __die("ERRNO: %s\n" format, strerror(errno))
#define warn(format, ...) __info("ERRNO: %s\n" format, strerror(errno))
#define info(format, ...) __info(format)

#endif

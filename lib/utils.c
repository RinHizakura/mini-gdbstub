#include "utils.h"

static char hexchars[] = "0123456789abcdef";

void hex_to_str(uint8_t *num, char *str, int bytes)
{
    for (int i = 0; i < bytes; i++) {
        uint8_t ch = *(num + i);
        *(str + i * 2) = hexchars[ch >> 4];
        *(str + i * 2 + 1) = hexchars[ch & 0xf];
    }
    str[bytes * 2] = '\0';
}

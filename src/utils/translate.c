#include "utils/translate.h"

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

static uint8_t char_to_hex(char ch)
{
    const uint8_t letter = ch & 0x40;
    const uint8_t offset = (letter >> 3) | (letter >> 6);
    return (ch + offset) & 0xf;
}

void str_to_hex(char *str, uint8_t *num, int bytes)
{
    for (int i = 0; i < bytes; i++) {
        uint8_t ch_high = char_to_hex(*(str + i * 2));
        uint8_t ch_low = char_to_hex(*(str + i * 2 + 1));

        *(num + i) = (ch_high << 4) | ch_low;
    }
}

/* Binary-escape src[0, len) into dst, a NUL-terminated string of at most
 * cap bytes. Stops when the next byte might not fit, so a chunk never splits
 * an escape pair. Returns the number of source bytes consumed. */
int escape(const char *src, int len, char *dst, int cap)
{
    int r = 0, w = 0;
    for (; r < len && w + 2 <= cap; r++) {
        char c = src[r];
        if (c == '#' || c == '$' || c == '}' || c == '*') {
            dst[w++] = '}';
            c ^= 0x20;
        }
        dst[w++] = c;
    }
    dst[w] = '\0';
    return r;
}

int unescape(char *msg, char *end)
{
    char *w = msg;
    char *r = msg;
    while (r < end) {
        if (*r == '}') {
            *w = *(r + 1) ^ 0x20;
            r += 2;
        } else {
            *w = *r;
            r += 1;
        }
        w += 1;
    }

    return w - msg;
}

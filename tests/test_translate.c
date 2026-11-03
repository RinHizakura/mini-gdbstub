/* Unit test for the hex and binary-escape helpers. Run with `make check`. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "utils/translate.h"

int main(void)
{
    char out[32];
    uint8_t bytes[2];

    /* every framing character is escaped, everything else passes through */
    assert(escape("a#$}*z", 6, out, sizeof(out)) == 6);
    assert(strcmp(out, "a}\x03}\x04}]}\nz") == 0);

    /* round trip through the decoder used for X packets */
    assert(unescape(out, out + strlen(out)) == 6);
    assert(memcmp(out, "a#$}*z", 6) == 0);

    /* a chunk never splits an escape pair: room for 3 bytes takes "a" and
     * one pair, the second pair is left for the next chunk */
    assert(escape("a##", 3, out, 3) == 2);
    assert(strcmp(out, "a}\x03") == 0);

    /* nothing to send still yields a valid empty string */
    assert(escape("", 0, out, sizeof(out)) == 0 && out[0] == '\0');

    /* hex helpers accept either case and emit lower case */
    str_to_hex("beEF", bytes, 2);
    assert(bytes[0] == 0xbe && bytes[1] == 0xef);
    hex_to_str(bytes, out, 2);
    assert(strcmp(out, "beef") == 0);

    puts("test_translate: ok");
    return 0;
}

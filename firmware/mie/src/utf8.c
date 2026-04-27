/* utf8.c — see <mie/utf8.h>. */

#include "mie/utf8.h"

size_t mie_utf8_truncate(const char *s, size_t len, size_t max_bytes)
{
    if (s == NULL)            return 0;
    if (max_bytes == 0)       return 0;
    if (len <= max_bytes)     return len;

    size_t i = max_bytes;
    while (i > 0 && (((unsigned char)s[i]) & 0xC0u) == 0x80u) {
        --i;
    }
    return i;
}

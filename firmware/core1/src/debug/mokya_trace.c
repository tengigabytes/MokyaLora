/* mokya_trace.c — see mokya_trace.h.
 *
 * Stack buffer is 128 B; CSV events at our verbosity stay well under
 * that (the longest "kc=0x%02x" / "n_cand=%d" payload is < 16 B).
 * Truncation is silent — vsnprintf returns the would-be length so we
 * cap at the buffer size and never overflow.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "mokya_trace.h"

#include <stdarg.h>
#include <stdio.h>

#define MOKYA_TRACE_BUF_SZ 128

void mokya_trace_emit(unsigned long ts_us,
                      const char *src,
                      const char *ev,
                      const char *fmt,
                      ...)
{
    char buf[MOKYA_TRACE_BUF_SZ];

    /* Header: "<ts>,<src>,<ev>" — no trailing comma yet. */
    int n = snprintf(buf, sizeof buf, "%lu,%s,%s", ts_us, src, ev);
    if (n < 0) return;
    if (n >= (int)sizeof buf) n = (int)sizeof buf - 1;

    /* Payload — only emit the leading comma if the caller passed a
     * non-empty format string. Avoids a trailing comma for TRACE_BARE
     * events while keeping the macro layer dumb. */
    if (fmt && fmt[0] != '\0' && n < (int)sizeof buf - 2) {
        buf[n++] = ',';
        va_list ap;
        va_start(ap, fmt);
        int m = vsnprintf(buf + n, sizeof buf - (size_t)n, fmt, ap);
        va_end(ap);
        if (m > 0) {
            n += m;
            if (n >= (int)sizeof buf) n = (int)sizeof buf - 1;
        }
    }

    /* Newline + push. SEGGER_RTT_Write is non-blocking (NO_BLOCK_SKIP). */
    if (n < (int)sizeof buf - 1) {
        buf[n++] = '\n';
    } else {
        buf[sizeof buf - 1] = '\n';
        n = sizeof buf;
    }
    SEGGER_RTT_Write(0, buf, (unsigned)n);
}

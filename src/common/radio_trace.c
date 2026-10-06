/*
 * radio_trace — CSIM_TRACE_RADIO, see include/common/radio_trace.h.
 */
#include "radio_trace.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int csim_radio_trace_on = -1;
static int64_t (*trace_now_ns)(void *user);
static void *trace_now_user;

int csim_radio_trace_enabled(void) {
    if (csim_radio_trace_on < 0) {
        const char *e = getenv("CSIM_TRACE_RADIO");
        csim_radio_trace_on = (e && e[0] && strcmp(e, "0") != 0) ? 1 : 0;
    }
    return csim_radio_trace_on;
}

void csim_radio_trace_set_clock(int64_t (*now_ns)(void *user), void *user) {
    trace_now_ns = now_ns;
    trace_now_user = user;
}

void csim_radio_trace(const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    int64_t now = trace_now_ns ? trace_now_ns(trace_now_user) : 0;
    fprintf(stderr, "[t=%.6fs] %s\n", (double)now / 1e9, line);
}

void csim_radio_trace_filter(int s, int sr, int rcv, int rr,
                             int s_ch, int r_ch, int delivered) {
    if (delivered)
        csim_radio_trace("filter sender=%d/%d receiver=%d/%d ch=%d/%d -> DELIVER",
                         s, sr, rcv, rr, s_ch, r_ch);
    else
        csim_radio_trace("filter sender=%d/%d receiver=%d/%d ch=%d/%d -> DROP "
                         "(channel_mismatch)", s, sr, rcv, rr, s_ch, r_ch);
}

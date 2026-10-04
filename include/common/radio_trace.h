/*
 * radio_trace — CSIM_TRACE_RADIO, the channel / TX / filter debug trace.
 *
 * Enabled by `CSIM_TRACE_RADIO=1`.  When on, every channel change, frame
 * TX (start + complete), medium filter decision (deliver or drop with
 * reason) and CC2420 state change is logged to stderr, one parseable line
 * per event:
 *
 *   [t=12.345678s] event_type field=val field=val ...
 *
 * The radio medium and the chips write to it, so it lives here rather than
 * in a frontend.  The t= stamp comes from the clock the frontend installs
 * (the kernel's now_ns); with none installed it reads 0.  Like the env var
 * itself, the switch and the clock are process-wide — a debug aid, not
 * per-runtime state.
 *
 * Disabled overhead = one cached int test at each call site.
 */
#ifndef RADIO_TRACE_H
#define RADIO_TRACE_H

#include <stdint.h>

/* 1 if CSIM_TRACE_RADIO is set to anything but "" or "0" (read once). */
int  csim_radio_trace_enabled(void);

/* The time source for the t= stamp.  NULL uninstalls it. */
void csim_radio_trace_set_clock(int64_t (*now_ns)(void *user), void *user);

/* One trace line: "[t=<now>s] " + fmt + "\n".  Callers test
 * csim_radio_trace_enabled() first, so the arguments cost nothing when off. */
void csim_radio_trace(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

/* The medium's channel-filter decision for one sender/receiver radio pair. */
void csim_radio_trace_filter(int sender, int sender_radio,
                             int receiver, int receiver_radio,
                             int s_ch, int r_ch, int delivered);

#endif /* RADIO_TRACE_H */

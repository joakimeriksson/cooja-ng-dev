/*
 * CC13x2 / CC26x2 RF core — see include/arm/cc13x2_rfc.h.
 *
 * Field offsets are those of the driverlib command structures on the
 * 32-bit target (rf_common_cmd.h, rf_prop_cmd.h, rf_data_entry.h); flag
 * bits are RFC_DBELL's (inc/hw_rfc_dbell.h). Where the model chooses a
 * latency or an order the documentation does not give, the comment says so.
 */
#include "cc13x2_rfc.h"
#include "cc13x2_soc.h"
#include "arm_cpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Doorbell register offsets (inc/hw_rfc_dbell.h) --- */
#define DB_CMDR       0x00
#define DB_CMDSTA     0x04
#define DB_RFHWIFG    0x08
#define DB_RFHWIEN    0x0C
#define DB_RFCPEIFG   0x10
#define DB_RFCPEIEN   0x14
#define DB_RFCPEISL   0x18
#define DB_RFACKIFG   0x1C
#define DB_SYSGPOCTL  0x20

/* RFCPEIFG bits (rf_mailbox.h IRQ_*) */
#define IRQ_COMMAND_DONE        (1u << 0)
#define IRQ_LAST_COMMAND_DONE   (1u << 1)
#define IRQ_FG_COMMAND_DONE     (1u << 2)
#define IRQ_LAST_FG_COMMAND_DONE (1u << 3)
#define IRQ_TX_DONE             (1u << 4)
#define IRQ_RX_OK               (1u << 16)
#define IRQ_RX_NOK              (1u << 17)
#define IRQ_RX_BUF_FULL         (1u << 22)
#define IRQ_RX_ENTRY_DONE       (1u << 23)
#define IRQ_MODULES_UNLOCKED    (1u << 29)
#define IRQ_BOOT_DONE           (1u << 30)
/* RFHWIFG */
#define HWIRQ_MDMSOFT           (1u << 5)

/* PWMCLKEN (inc/hw_rfc_pwr.h) */
#define PWM_CPE                 (1u << 1)
#define PWM_RAT                 (1u << 7)

/* CMDSTA results (rf_mailbox.h CMDSTA_*) */
#define CMDSTA_DONE             0x01u
#define CMDSTA_ILLEGAL_POINTER  0x81u
#define CMDSTA_UNKNOWN_CMD      0x82u
#define CMDSTA_UNKNOWN_DIR_CMD  0x83u
#define CMDSTA_CONTEXT_ERROR    0x85u
#define CMDSTA_SCHEDULING_ERROR 0x86u

/* Command numbers */
#define CMD_NOP                 0x0801
#define CMD_RADIO_SETUP         0x0802
#define CMD_FS                  0x0803
#define CMD_FS_OFF              0x0804
#define CMD_SYNC_STOP_RAT       0x0809
#define CMD_SYNC_START_RAT      0x080A
#define CMD_FS_POWERUP          0x080C
#define CMD_FS_POWERDOWN        0x080D
#define CMD_ABORT               0x0401
#define CMD_STOP                0x0402
#define CMD_GET_RSSI            0x0403
#define CMD_START_RAT           0x0405
#define CMD_PING                0x0406
#define CMD_BUS_REQUEST         0x040E
#define CMD_SET_RAT_CMP         0x000A
#define CMD_SET_TX_POWER        0x0010
#define CMD_UPDATE_FS           0x0011
#define CMD_PROP_TX             0x3801
#define CMD_PROP_RX             0x3802
#define CMD_PROP_TX_ADV         0x3803
#define CMD_PROP_RX_ADV         0x3804
#define CMD_PROP_RADIO_SETUP    0x3806
#define CMD_PROP_RADIO_DIV_SETUP 0x3807

/* Status words */
#define ST_IDLE                 0x0000
#define ST_ACTIVE               0x0002
#define ST_DONE_OK              0x0400
#define ST_DONE_STOPPED         0x0404
#define ST_DONE_ABORT           0x0405
#define ST_ERROR_PAST_START     0x0800
#define ST_ERROR_PAR            0x0803
#define ST_ERROR_CMDID          0x0805
#define ST_ERROR_NO_SETUP       0x0807
#define PROP_DONE_OK            0x3400
#define PROP_DONE_STOPPED       0x3404
#define PROP_DONE_ABORT         0x3405
#define PROP_ERROR_PAR          0x3800
#define PROP_ERROR_RXBUF        0x3801
#define PROP_ERROR_NO_SETUP     0x3803

/* Radio-operation header: commandNo @0, status @2, pNextOp @4,
 * startTime @8, startTrigger @12, condition @13. */
#define OP_STATUS    2
#define OP_NEXT      4
#define OP_STARTTIME 8
#define OP_TRIGGER   12
#define OP_CONDITION 13

#define TRIG_NOW      0
#define TRIG_NEVER    1
#define TRIG_ABSTIME  2
#define TRIG_PAST_BM  0x80

#define COND_ALWAYS        0
#define COND_NEVER         1
#define COND_STOP_ON_FALSE 2
#define COND_STOP_ON_TRUE  3
#define COND_SKIP_ON_FALSE 4
#define COND_SKIP_ON_TRUE  5

/* Data entries (rf_data_entry.h): pNextEntry @0, status @4, config @5
 * (type [1:0], lenSz [3:2]), length @6, data @8. */
#define DE_PENDING   0
#define DE_ACTIVE    1
#define DE_BUSY      2
#define DE_FINISHED  3

/* Latencies.  The TRM gives no per-command execution times; these are of
 * the order the RF driver itself budgets (it clamps the measured power-up
 * time to 300..2500 us, RFCC26X2_multiMode.c) and keep power-up through an
 * active RX well inside the ~1 ms Contiki's get_rssi() waits. Measured
 * values from the LaunchPad would replace them. TODO(hardware). */
#define BOOT_NS          40000    /* PWMCLKEN.CPE -> BOOT_DONE */
#define SETUP_NS        150000    /* CMD_(PROP_)RADIO(_DIV)_SETUP */
#define FS_NS           140000    /* CMD_FS: synthesizer calibrate + lock */
#define RAT_SYNC_NS      31000    /* CMD_SYNC_START/STOP_RAT: one RTC edge */
#define TX_START_NS     100000    /* TX ramp before the first preamble bit */
#define SUBGHZ_BYTE_NS  160000    /* 8 bits at 50 kbps (CMD_PROP_RADIO_DIV_SETUP rateWord 0x8000) */

/* RSSI the receiver reports without a frame: an idle channel at the
 * sensitivity floor, and a carrier it can sense but has not synced to. */
#define RSSI_IDLE_DBM   (-110)
#define RSSI_BUSY_DBM   (-60)

enum { RX_HUNT = 0, RX_HDR, RX_PAYLOAD, RX_CRC };

/* ============================================================
 * Helpers
 * ============================================================ */

static arm_cpu_t *cpu_of(cc13x2_rfc_t *rfc) { return &rfc->soc->plat->cpu; }
static int64_t now_ns(cc13x2_rfc_t *rfc) { return cc13x2_live_ns(rfc->soc); }

static uint8_t  rd8(cc13x2_rfc_t *r, uint32_t a)  { return arm_read8(cpu_of(r), a); }
static uint16_t rd16(cc13x2_rfc_t *r, uint32_t a) {
    return (uint16_t)(rd8(r, a) | (rd8(r, a + 1) << 8));
}
static uint32_t rd32(cc13x2_rfc_t *r, uint32_t a) {
    return (uint32_t)rd16(r, a) | ((uint32_t)rd16(r, a + 2) << 16);
}
static void wr8(cc13x2_rfc_t *r, uint32_t a, uint8_t v) { arm_write8(cpu_of(r), a, v); }
static void wr16(cc13x2_rfc_t *r, uint32_t a, uint16_t v) {
    wr8(r, a, (uint8_t)v); wr8(r, a + 1, (uint8_t)(v >> 8));
}
static void wr32(cc13x2_rfc_t *r, uint32_t a, uint32_t v) {
    wr16(r, a, (uint16_t)v); wr16(r, a + 2, (uint16_t)(v >> 16));
}

static int trace_on(void) {
    static int t = -1;
    if (t < 0) t = getenv("CC13X2_RFC_TRACE") ? 1 : 0;
    return t;
}
#define RTRC(rfc, ...) do { if (trace_on()) { \
    fprintf(stderr, "[rfc %10.6f] ", (double)now_ns(rfc) / 1e9); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static void update_irqs(cc13x2_rfc_t *rfc) {
    uint32_t m = rfc->cpeifg & rfc->cpeien;
    if (m & ~rfc->cpeisl) cc13x2_irq(rfc->soc, CC13X2_IRQ_RFC_CPE_0);
    if (m & rfc->cpeisl)  cc13x2_irq(rfc->soc, CC13X2_IRQ_RFC_CPE_1);
    if (rfc->hwifg & rfc->hwien) cc13x2_irq(rfc->soc, CC13X2_IRQ_RFC_HW_COMB);
}

static void raise_cpe(cc13x2_rfc_t *rfc, uint32_t bits) {
    rfc->cpeifg |= bits;
    update_irqs(rfc);
}

/* RAT ticks (4 MHz) at a time on the live clock: locked to the AON RTC,
 * rat0 being the RAT value when the RTC read 0 (CMD_SYNC_START_RAT). */
static uint32_t rat_at(cc13x2_rfc_t *rfc, int64_t ns) {
    int64_t rtc = cc13x2_rtc_ns_at(rfc->soc, ns);
    return rfc->rat0 + (uint32_t)(((uint64_t)rtc * 4u) / 1000u);
}
static uint32_t rat_now(cc13x2_rfc_t *rfc) { return rat_at(rfc, now_ns(rfc)); }

static void push_channel(cc13x2_rfc_t *rfc) {
    sim_host_t *h = &rfc->soc->plat->host;
    int ch = -1, idx = 1;
    if (rfc->freq_khz >= 2400000u) {
        idx = 0;
        ch = 11 + (int)(((int64_t)rfc->freq_khz - 2405000 + 2500) / 5000);
    } else if (rfc->freq_khz > 0) {
        /* Band-local sub-GHz index on the 200 kHz grid from 863.125 MHz,
         * the convention the CC1200 model uses (cc1200_channel_from_freq_value). */
        ch = (int)(((int64_t)rfc->freq_khz - 863125 + 100) / 200);
    }
    if (ch == rfc->channel && idx == rfc->radio_idx) return;
    if (h->radio_set_channel) {
        if (idx != rfc->radio_idx && rfc->channel >= 0)
            h->radio_set_channel(h->radio_user_data, rfc->radio_idx, -1);
        h->radio_set_channel(h->radio_user_data, idx, ch);
    }
    RTRC(rfc, "channel %d on slot %d (%u kHz)", ch, idx, rfc->freq_khz);
    rfc->channel = ch;
    rfc->radio_idx = idx;
}

/* IEEE 802.3 CRC-32 (reflected 0xEDB88320). Both ends of a csim link use
 * the same one, so a corrupted frame fails the check; the bit order on air
 * is not modelled. */
static uint32_t crc32_calc(const uint8_t *p, int n) {
    uint32_t c = 0xFFFFFFFFu;
    for (int i = 0; i < n; i++) {
        c ^= p[i];
        for (int b = 0; b < 8; b++) c = (c >> 1) ^ (0xEDB88320u & -(c & 1u));
    }
    return ~c;
}
/* CRC-16-CCITT (802.15.4g 2-byte FCS, polynomial 0x1021). */
static uint16_t crc16_calc(const uint8_t *p, int n) {
    uint16_t c = 0;
    for (int i = 0; i < n; i++) {
        c ^= (uint16_t)(p[i] << 8);
        for (int b = 0; b < 8; b++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
    }
    return c;
}

/* ============================================================
 * Radio-operation engine
 * ============================================================ */

static bool is_fg(uint16_t cmd) { return (cmd & 0xFC00) == 0x2C00; }
static cc13x2_rfc_op_t *slot_for(cc13x2_rfc_t *rfc, uint16_t cmd) {
    return is_fg(cmd) ? &rfc->fg : &rfc->bg;
}

static void op_start(cc13x2_rfc_t *rfc, cc13x2_rfc_op_t *s, uint32_t addr);
static void op_run(cc13x2_rfc_t *rfc, cc13x2_rfc_op_t *s);

static void set_status(cc13x2_rfc_t *rfc, uint32_t op, uint16_t st) {
    wr16(rfc, op + OP_STATUS, st);
}

/* An operation finished with `status`; `result` is the TRUE/FALSE its
 * definition gives (DONE_OK-class = TRUE). Raise the done flags and follow
 * pNextOp per the condition rule (rf_common_cmd.h condition). */
static void op_finish(cc13x2_rfc_t *rfc, cc13x2_rfc_op_t *s, uint16_t status,
                      bool result, bool chain) {
    uint32_t op = s->addr;
    bool fg = (s == &rfc->fg);
    set_status(rfc, op, status);
    RTRC(rfc, "op 0x%08x cmd 0x%04x done status 0x%04x", op, s->cmd, status);
    s->addr = 0;
    s->stop_req = false;
    if (rfc->rx.active && rfc->rx.op == op) rfc->rx.active = false;

    uint32_t next = 0;
    if (chain) {
        uint8_t cond = rd8(rfc, op + OP_CONDITION);
        int rule = cond & 0x0F, nskip = (cond >> 4) & 0x0F;
        uint32_t p = rd32(rfc, op + OP_NEXT);
        switch (rule) {
        case COND_ALWAYS:        next = p; break;
        case COND_NEVER:         next = 0; break;
        case COND_STOP_ON_FALSE: next = result ? p : 0; break;
        case COND_STOP_ON_TRUE:  next = result ? 0 : p; break;
        case COND_SKIP_ON_FALSE:
        case COND_SKIP_ON_TRUE: {
            bool skip = (rule == COND_SKIP_ON_FALSE) ? !result : result;
            next = p;
            if (skip) {
                for (int i = 0; i < nskip + 1 && next; i++)
                    next = rd32(rfc, next + OP_NEXT);
            }
            break;
        }
        default: next = 0; break;
        }
    }
    uint32_t done = fg ? IRQ_FG_COMMAND_DONE : IRQ_COMMAND_DONE;
    if (!next) done |= fg ? IRQ_LAST_FG_COMMAND_DONE : IRQ_LAST_COMMAND_DONE;
    raise_cpe(rfc, done);
    if (next) {
        uint16_t ncmd = rd16(rfc, next);
        op_start(rfc, slot_for(rfc, ncmd), next);
    }
}

static void op_ev_cb(void *user, cpu_event_t *ev) {
    cc13x2_rfc_t *rfc = user;
    cc13x2_rfc_op_t *s = (ev == &rfc->fg.ev) ? &rfc->fg : &rfc->bg;
    if (s->addr) op_run(rfc, s);
}

static void op_after(cc13x2_rfc_t *rfc, cc13x2_rfc_op_t *s, int64_t delay_ns) {
    s->ev.callback = op_ev_cb;
    s->ev.user_data = rfc;
    cc13x2_schedule_at(rfc->soc, &s->ev, now_ns(rfc) + delay_ns);
}

/* Start an operation in its slot: honour the start trigger, then mark it
 * ACTIVE (synchronously — TI's driver busy-waits on that) and run it. */
static void op_start(cc13x2_rfc_t *rfc, cc13x2_rfc_op_t *s, uint32_t addr) {
    if (s->addr) {
        /* A second operation for a busy slot: the chain ends here. */
        RTRC(rfc, "op 0x%08x refused: slot busy with 0x%08x", addr, s->addr);
        set_status(rfc, addr, ST_ERROR_PAR);
        return;
    }
    s->addr = addr;
    s->cmd = rd16(rfc, addr);
    s->phase = 0;
    s->stop_req = false;
    rfc->stat_cmds++;
    uint8_t trig = rd8(rfc, addr + OP_TRIGGER);
    int ttype = trig & 0x0F;
    RTRC(rfc, "op 0x%08x cmd 0x%04x start trig %d", addr, s->cmd, ttype);
    if (ttype == TRIG_ABSTIME) {
        uint32_t when = rd32(rfc, addr + OP_STARTTIME);
        int32_t d = (int32_t)(when - rat_now(rfc));
        if (d > 0) {
            set_status(rfc, addr, 0x0001);              /* PENDING */
            s->phase = -1;                              /* waiting to start */
            op_after(rfc, s, (int64_t)d * 250);         /* 4 MHz RAT ticks */
            return;
        }
        if (!(trig & TRIG_PAST_BM)) {
            op_finish(rfc, s, ST_ERROR_PAST_START, false, false);
            return;
        }
    } else if (ttype == TRIG_NEVER) {
        set_status(rfc, addr, 0x0001);
        s->phase = -2;                                  /* never starts by itself */
        return;
    } else if (ttype != TRIG_NOW) {
        /* Relative triggers: Contiki uses TRIG_NOW/ABSTIME only for start. */
        RTRC(rfc, "op 0x%08x: start trigger %d taken as NOW", addr, ttype);
    }
    set_status(rfc, addr, ST_ACTIVE);
    op_run(rfc, s);
}

/* --- Individual operations --- */

static void prop_tx_emit(cc13x2_rfc_t *rfc, cc13x2_rfc_op_t *s, int64_t *air_ns);
static void prop_rx_begin(cc13x2_rfc_t *rfc, cc13x2_rfc_op_t *s);

static void op_run(cc13x2_rfc_t *rfc, cc13x2_rfc_op_t *s) {
    uint32_t op = s->addr;
    if (s->phase == -1) {                           /* ABSTIME start reached */
        s->phase = 0;
        set_status(rfc, op, ST_ACTIVE);
    }
    switch (s->cmd) {
    case CMD_NOP:
        op_finish(rfc, s, ST_DONE_OK, true, true);
        return;

    case CMD_RADIO_SETUP:
    case CMD_PROP_RADIO_SETUP:
    case CMD_PROP_RADIO_DIV_SETUP:
        if (s->phase == 0) {
            if (s->cmd == CMD_RADIO_SETUP) {
                rfc->prop = false;
            } else {
                rfc->prop = true;
                uint8_t pream = rd8(rfc, op + 21);
                uint16_t fmt = rd16(rfc, op + 22);
                rfc->pream_bytes = pream & 0x3F;
                rfc->sw_bits = fmt & 0x3F;
                rfc->whiten_mode = (fmt >> 13) & 7;
                if (rfc->pream_bytes == 0) rfc->pream_bytes = 4;
                if (rfc->sw_bits == 0 || rfc->sw_bits > 32) rfc->sw_bits = 32;
            }
            s->phase = 1;
            op_after(rfc, s, SETUP_NS);
            return;
        }
        rfc->setup_done = true;
        op_finish(rfc, s, ST_DONE_OK, true, true);
        return;

    case CMD_FS:
        if (s->phase == 0) {
            uint16_t f = rd16(rfc, op + 14), frac = rd16(rfc, op + 16);
            rfc->freq_khz = (uint32_t)f * 1000u + (uint32_t)(((uint32_t)frac * 1000u) >> 16);
            s->phase = 1;
            op_after(rfc, s, FS_NS);
            return;
        }
        push_channel(rfc);
        op_finish(rfc, s, ST_DONE_OK, true, true);
        return;

    case CMD_FS_OFF:
    case CMD_FS_POWERUP:
    case CMD_FS_POWERDOWN:
        op_finish(rfc, s, ST_DONE_OK, true, true);
        return;

    case CMD_SYNC_START_RAT:
    case CMD_SYNC_STOP_RAT:
        if (s->phase == 0) {
            s->phase = 1;
            op_after(rfc, s, RAT_SYNC_NS);
            return;
        }
        if (s->cmd == CMD_SYNC_START_RAT) {
            /* Start (or re-sync, when already running) with the given rat0. */
            rfc->rat0 = rd32(rfc, op + 16);
            rfc->rat_running = true;
        } else {
            wr32(rfc, op + 16, rfc->rat0);
            rfc->rat_running = false;
        }
        op_finish(rfc, s, ST_DONE_OK, true, true);
        return;

    case CMD_PROP_TX:
    case CMD_PROP_TX_ADV:
        if (!rfc->setup_done || !rfc->prop) {
            op_finish(rfc, s, PROP_ERROR_NO_SETUP, false, false);
            return;
        }
        if (s->phase == 0) {
            s->phase = 1;
            op_after(rfc, s, TX_START_NS);
            return;
        }
        if (s->phase == 1) {
            int64_t air = 0;
            prop_tx_emit(rfc, s, &air);
            s->phase = 2;
            op_after(rfc, s, air);
            return;
        }
        raise_cpe(rfc, IRQ_TX_DONE);
        rfc->stat_tx++;
        op_finish(rfc, s, s->stop_req ? PROP_DONE_STOPPED : PROP_DONE_OK,
                  !s->stop_req, !s->stop_req);
        return;

    case CMD_PROP_RX_ADV:
        if (!rfc->setup_done || !rfc->prop) {
            op_finish(rfc, s, PROP_ERROR_NO_SETUP, false, false);
            return;
        }
        if (s->phase == 0) {
            prop_rx_begin(rfc, s);
            return;
        }
        /* phase 1: endTrigger reached */
        op_finish(rfc, s, PROP_DONE_OK, true, true);
        return;

    default:
        RTRC(rfc, "op 0x%08x: unsupported command 0x%04x", op, s->cmd);
        fprintf(stderr, "[cc13x2 rfc] unsupported radio operation 0x%04x\n", s->cmd);
        op_finish(rfc, s, ST_ERROR_CMDID, false, false);
        return;
    }
}

/* ============================================================
 * Proprietary TX
 * ============================================================ */

/* CMD_PROP_TX_ADV: pktConf @14 (bFsOff [0], two reserved bits, bUseCrc
 * [3] — rf_prop_cmd.h pads with `uint8_t :2`), numHdrBits @15, pktLen
 * @16 (u16, header included), syncWord @24, pPkt @28.
 * CMD_PROP_TX:     pktConf @14, pktLen @15 (u8), syncWord @16, pPkt @20. */
static void prop_tx_emit(cc13x2_rfc_t *rfc, cc13x2_rfc_op_t *s, int64_t *air_ns) {
    uint32_t op = s->addr;
    bool adv = (s->cmd == CMD_PROP_TX_ADV);
    uint8_t pkt_conf = rd8(rfc, op + 14);
    int hdr_bits = adv ? rd8(rfc, op + 15) : 0;
    int pkt_len = adv ? rd16(rfc, op + 16) : rd8(rfc, op + 15);
    uint32_t sw = rd32(rfc, op + (adv ? 24 : 16));
    uint32_t ppkt = rd32(rfc, op + (adv ? 28 : 20));
    bool use_crc = (pkt_conf & 0x08) != 0;

    uint8_t pkt[2048];
    if (pkt_len > (int)sizeof(pkt)) pkt_len = (int)sizeof(pkt);
    for (int i = 0; i < pkt_len; i++) pkt[i] = rd8(rfc, ppkt + (uint32_t)i);

    int hdr_bytes = hdr_bits / 8;
    if (hdr_bytes > pkt_len) hdr_bytes = pkt_len;
    /* 802.15.4g PHR: with whitenMode 7 the FCS type comes from the header
     * (bit 12 of the 16-bit PHR, 1 = 2-byte CRC); else a 16-bit CRC. */
    uint16_t phr = (hdr_bytes >= 2) ? (uint16_t)(pkt[0] | (pkt[1] << 8)) : 0;
    int crc_len = 0;
    if (use_crc)
        crc_len = (rfc->whiten_mode >= 4 && hdr_bytes >= 2)
                  ? ((phr & 0x1000) ? 2 : 4) : 2;

    const uint8_t *payload = pkt + hdr_bytes;
    int n = pkt_len - hdr_bytes;
    uint8_t crc[4];
    if (crc_len == 4) {
        uint32_t c = crc32_calc(payload, n);
        crc[0] = (uint8_t)(c >> 24); crc[1] = (uint8_t)(c >> 16);
        crc[2] = (uint8_t)(c >> 8);  crc[3] = (uint8_t)c;
    } else if (crc_len == 2) {
        uint16_t c = crc16_calc(payload, n);
        crc[0] = (uint8_t)(c >> 8); crc[1] = (uint8_t)c;
    }

    int sw_bytes = (rfc->sw_bits + 7) / 8;
    int total = rfc->pream_bytes + sw_bytes + hdr_bytes + n + crc_len;
    *air_ns = (int64_t)total * SUBGHZ_BYTE_NS;
    RTRC(rfc, "prop TX %d payload bytes (crc %d), %d on air", n, crc_len, total);

    void *user = rfc->tx_user[1];
    if (!rfc->tx_cb) return;
    /* Preamble (preamMode 0: 0101..., i.e. 0x55 MSB first), sync word MSB
     * first, the header as a little-endian word sent MSB first (so the PHR
     * high byte goes first, as 802.15.4g puts it on air), payload, CRC. */
    for (int i = 0; i < rfc->pream_bytes; i++) rfc->tx_cb(user, 0x55);
    for (int i = sw_bytes - 1; i >= 0; i--) rfc->tx_cb(user, (uint8_t)(sw >> (8 * i)));
    for (int i = hdr_bytes - 1; i >= 0; i--) rfc->tx_cb(user, pkt[i]);
    for (int i = 0; i < n; i++) rfc->tx_cb(user, payload[i]);
    for (int i = 0; i < crc_len; i++) rfc->tx_cb(user, crc[i]);
}

/* ============================================================
 * Proprietary RX (CMD_PROP_RX_ADV)
 * ============================================================ */

/* CMD_PROP_RX_ADV: pktConf @14, rxConf @15, syncWord0 @16, syncWord1 @20,
 * maxPktLen @24, hdrConf @26 (numHdrBits [5:0], lenPos [10:6],
 * numLenBits [15:11]), lenOffset @30 (int8), endTrigger @31, endTime @32,
 * pQueue @40 (dataQueue_t: pCurrEntry @0, pLastEntry @4), pOutput @44
 * (rfc_propRxOutput_t). rxConf: bAutoFlushIgnored 0, bAutoFlushCrcErr 1,
 * bIncludeHdr 3, bIncludeCrc 4, bAppendRssi 5, bAppendTimestamp 6,
 * bAppendStatus 7. */
#define RXC_FLUSH_CRC   (1u << 1)
#define RXC_INC_HDR     (1u << 3)
#define RXC_INC_CRC     (1u << 4)
#define RXC_APP_RSSI    (1u << 5)
#define RXC_APP_TS      (1u << 6)
#define RXC_APP_STATUS  (1u << 7)

static uint32_t rx_queue_entry(cc13x2_rfc_t *rfc, uint32_t op) {
    uint32_t q = rd32(rfc, op + 40);
    return q ? rd32(rfc, q) : 0;
}

static void prop_rx_begin(cc13x2_rfc_t *rfc, cc13x2_rfc_op_t *s) {
    uint32_t op = s->addr;
    rfc->rx.active = true;
    rfc->rx.op = op;
    rfc->rx.state = RX_HUNT;
    rfc->rx.shift = 0;
    uint32_t e = rx_queue_entry(rfc, op);
    if (e && rd8(rfc, e + 4) == DE_PENDING) wr8(rfc, e + 4, DE_ACTIVE);
    /* endTrigger: TRIG_NEVER runs until CMD_STOP/ABORT; a relative or
     * absolute end time ends the operation then. */
    uint8_t et = rd8(rfc, op + 31) & 0x0F;
    if (et == TRIG_ABSTIME || et == 4 /* TRIG_REL_START */) {
        uint32_t t = rd32(rfc, op + 32);
        int64_t d = (et == 4) ? (int64_t)t * 250
                              : (int64_t)(int32_t)(t - rat_now(rfc)) * 250;
        s->phase = 1;
        op_after(rfc, s, d > 0 ? d : 0);
    } else {
        s->phase = 2;   /* open-ended */
    }
    RTRC(rfc, "prop RX listening, entry 0x%08x", e);
}

/* Stop the open RX gracefully: a frame in reception completes first. */
static void prop_rx_stop(cc13x2_rfc_t *rfc, cc13x2_rfc_op_t *s, bool abort) {
    if (!abort && rfc->rx.state != RX_HUNT) {
        s->stop_req = true;
        return;
    }
    cc13x2_cancel(rfc->soc, &s->ev);
    rfc->rx.active = false;
    op_finish(rfc, s, abort ? PROP_DONE_ABORT : PROP_DONE_STOPPED, false, false);
}

static void prop_rx_output(cc13x2_rfc_t *rfc, uint32_t op, int ok, int bufull,
                           int8_t rssi, uint32_t ts) {
    uint32_t o = rd32(rfc, op + 44);
    if (!o) return;
    if (ok > 0) wr16(rfc, o + 0, (uint16_t)(rd16(rfc, o + 0) + 1));     /* nRxOk */
    if (ok < 0) wr16(rfc, o + 2, (uint16_t)(rd16(rfc, o + 2) + 1));     /* nRxNok */
    if (bufull) wr8(rfc, o + 6, (uint8_t)(rd8(rfc, o + 6) + 1));       /* nRxBufFull */
    if (ok > 0) {
        wr8(rfc, o + 7, (uint8_t)rssi);                                  /* lastRssi */
        wr32(rfc, o + 8, ts);                                            /* timeStamp */
    }
}

/* A complete frame: write it into the current data entry and move on. */
static void prop_rx_deliver(cc13x2_rfc_t *rfc) {
    cc13x2_rfc_op_t *s = &rfc->bg;
    uint32_t op = rfc->rx.op;
    uint8_t rxc = rd8(rfc, op + 15);
    int n = rfc->rx.n;

    bool crc_ok = true;
    if (rfc->rx.crc_len == 4) {
        uint32_t c = crc32_calc(rfc->rx.buf, n);
        crc_ok = rfc->rx.crc[0] == (uint8_t)(c >> 24) && rfc->rx.crc[1] == (uint8_t)(c >> 16) &&
                 rfc->rx.crc[2] == (uint8_t)(c >> 8)  && rfc->rx.crc[3] == (uint8_t)c;
    } else if (rfc->rx.crc_len == 2) {
        uint16_t c = crc16_calc(rfc->rx.buf, n);
        crc_ok = rfc->rx.crc[0] == (uint8_t)(c >> 8) && rfc->rx.crc[1] == (uint8_t)c;
    }
    if (!crc_ok) {
        rfc->stat_rx_crc_err++;
        prop_rx_output(rfc, op, -1, 0, 0, 0);
        raise_cpe(rfc, IRQ_RX_NOK);
        RTRC(rfc, "prop RX CRC error, %d bytes", n);
        if (rxc & RXC_FLUSH_CRC) return;
    }

    /* Timestamp: Contiki adds USEC_TO_RAT(6*160 - 270) to the stamp and
     * takes the sum as the end of the sync word (rf/prop-mode.c), i.e. the
     * hardware stamps 690 us before that point at 50 kbps.  The model uses
     * that relation; the TRM does not define the stamp's position. */
    uint32_t ts = rat_at(rfc, rfc->rx.sync_end_ns - 690000);

    uint32_t e = rx_queue_entry(rfc, op);
    uint8_t cfg = e ? rd8(rfc, e + 5) : 0;
    int lensz = (cfg >> 2) & 3;
    int cap = e ? rd16(rfc, e + 6) : 0;
    int total = ((rxc & RXC_INC_HDR) ? 2 : 0) + n +
                ((rxc & RXC_INC_CRC) ? rfc->rx.crc_len : 0) +
                ((rxc & RXC_APP_RSSI) ? 1 : 0) + ((rxc & RXC_APP_TS) ? 4 : 0) +
                ((rxc & RXC_APP_STATUS) ? 1 : 0);
    uint8_t est = e ? rd8(rfc, e + 4) : 0xFF;
    if (!e || (est != DE_PENDING && est != DE_ACTIVE && est != DE_BUSY) ||
        lensz + total > cap) {
        /* No room: PROP_ERROR_RXBUF ends the operation (TRI "RX_BUF_FULL"). */
        rfc->stat_rx_buf_full++;
        prop_rx_output(rfc, op, 0, 1, 0, 0);
        raise_cpe(rfc, IRQ_RX_BUF_FULL);
        RTRC(rfc, "prop RX buffer full (entry 0x%08x status %u)", e, est);
        rfc->rx.active = false;
        op_finish(rfc, s, PROP_ERROR_RXBUF, false, false);
        return;
    }
    uint32_t d = e + 8;
    if (lensz == 1) wr8(rfc, d, (uint8_t)total);
    else if (lensz == 2) wr16(rfc, d, (uint16_t)total);
    d += (uint32_t)lensz;
    if (rxc & RXC_INC_HDR) { wr8(rfc, d++, (uint8_t)rfc->rx.hdr); wr8(rfc, d++, (uint8_t)(rfc->rx.hdr >> 8)); }
    for (int i = 0; i < n; i++) wr8(rfc, d++, rfc->rx.buf[i]);
    if (rxc & RXC_INC_CRC)
        for (int i = 0; i < rfc->rx.crc_len; i++) wr8(rfc, d++, rfc->rx.crc[i]);
    if (rxc & RXC_APP_RSSI) wr8(rfc, d++, (uint8_t)rfc->rx.rssi);
    if (rxc & RXC_APP_TS) { wr32(rfc, d, ts); d += 4; }
    if (rxc & RXC_APP_STATUS) wr8(rfc, d++, crc_ok ? 0x00 : 0x40);  /* result [7:6] */
    wr8(rfc, e + 4, DE_FINISHED);

    /* Advance the queue's current entry. */
    uint32_t q = rd32(rfc, op + 40);
    uint32_t next = rd32(rfc, e);
    wr32(rfc, q, next);
    if (next && rd8(rfc, next + 4) == DE_PENDING) wr8(rfc, next + 4, DE_ACTIVE);

    rfc->stat_rx_ok++;
    prop_rx_output(rfc, op, crc_ok ? 1 : -1, 0, rfc->rx.rssi, ts);
    raise_cpe(rfc, (crc_ok ? IRQ_RX_OK : IRQ_RX_NOK) | IRQ_RX_ENTRY_DONE);
    RTRC(rfc, "prop RX %d bytes -> entry 0x%08x rssi %d", n, e, rfc->rx.rssi);
}

static void prop_rx_frame_end(cc13x2_rfc_t *rfc) {
    rfc->rx.state = RX_HUNT;
    rfc->rx.shift = 0;
    cc13x2_rfc_op_t *s = &rfc->bg;
    if (rfc->rx.active && s->stop_req && s->addr == rfc->rx.op)
        prop_rx_stop(rfc, s, false);
}

static void prop_rx_byte(cc13x2_rfc_t *rfc, uint8_t byte, int8_t rssi) {
    uint32_t op = rfc->rx.op;
    int64_t t = now_ns(rfc);
    rfc->rx.last_byte_ns = t;
    switch (rfc->rx.state) {
    case RX_HUNT: {
        rfc->rx.shift = (rfc->rx.shift << 8) | byte;
        int bits = rfc->sw_bits;
        uint32_t mask = bits >= 32 ? 0xFFFFFFFFu : ((1u << bits) - 1u);
        uint32_t sw0 = rd32(rfc, op + 16);
        if ((rfc->rx.shift & mask) != (sw0 & mask)) return;
        /* Sync found: a buffer must be available at the start of the frame. */
        uint32_t e = rx_queue_entry(rfc, op);
        uint8_t est = e ? rd8(rfc, e + 4) : 0xFF;
        if (!e || (est != DE_PENDING && est != DE_ACTIVE)) {
            rfc->stat_rx_buf_full++;
            prop_rx_output(rfc, op, 0, 1, 0, 0);
            raise_cpe(rfc, IRQ_RX_BUF_FULL);
            RTRC(rfc, "prop RX: no free entry at sync (0x%08x status %u)", e, est);
            rfc->rx.active = false;
            op_finish(rfc, &rfc->bg, PROP_ERROR_RXBUF, false, false);
            return;
        }
        wr8(rfc, e + 4, DE_BUSY);
        rfc->rx.state = RX_HDR;
        rfc->rx.hdr_count = 0;
        rfc->rx.hdr = 0;
        rfc->rx.rssi = rssi;
        rfc->rx.sync_end_ns = t + SUBGHZ_BYTE_NS;
        /* Sync-word found is the modem's soft event (TSCH polls it). */
        rfc->hwifg |= HWIRQ_MDMSOFT;
        update_irqs(rfc);
        return;
    }
    case RX_HDR: {
        uint16_t hconf = rd16(rfc, op + 26);
        int hdr_bits = hconf & 0x3F;
        int hdr_bytes = (hdr_bits + 7) / 8;
        rfc->rx.hdr = (uint16_t)((rfc->rx.hdr << 8) | byte);   /* MSB first */
        if (++rfc->rx.hdr_count < hdr_bytes) return;
        int len_pos = (hconf >> 6) & 0x1F, len_bits = (hconf >> 11) & 0x1F;
        int len = (rfc->rx.hdr >> len_pos) & ((1 << len_bits) - 1);
        int8_t len_off = (int8_t)rd8(rfc, op + 30);
        int n = len + len_off;
        uint8_t pkt_conf = rd8(rfc, op + 14);
        int crc_len = 0;
        if (pkt_conf & 0x08)                       /* bUseCrc (pktConf bit 3) */
            crc_len = (rfc->whiten_mode >= 4) ? ((rfc->rx.hdr & 0x1000) ? 2 : 4) : 2;
        uint8_t maxlen = rd8(rfc, op + 24);
        if (n < 0 || n > (int)sizeof(rfc->rx.buf) || (maxlen && n > maxlen)) {
            /* Length outside the configured range: the frame is dropped
             * and the receiver hunts again. */
            uint32_t e = rx_queue_entry(rfc, op);
            if (e && rd8(rfc, e + 4) == DE_BUSY) wr8(rfc, e + 4, DE_ACTIVE);
            prop_rx_output(rfc, op, -1, 0, 0, 0);
            raise_cpe(rfc, IRQ_RX_NOK);
            prop_rx_frame_end(rfc);
            return;
        }
        rfc->rx.n = n;
        rfc->rx.crc_len = crc_len;
        rfc->rx.got = 0;
        rfc->rx.state = n > 0 ? RX_PAYLOAD : RX_CRC;
        if (n == 0 && crc_len == 0) { prop_rx_deliver(rfc); prop_rx_frame_end(rfc); }
        return;
    }
    case RX_PAYLOAD:
        rfc->rx.buf[rfc->rx.got++] = byte;
        if (rfc->rx.got < rfc->rx.n) return;
        rfc->rx.got = 0;
        rfc->rx.state = RX_CRC;
        if (rfc->rx.crc_len == 0) { prop_rx_deliver(rfc); prop_rx_frame_end(rfc); }
        return;
    case RX_CRC:
        rfc->rx.crc[rfc->rx.got++] = byte;
        if (rfc->rx.got < rfc->rx.crc_len) return;
        prop_rx_deliver(rfc);
        prop_rx_frame_end(rfc);
        return;
    }
}

void cc13x2_rfc_receive_byte(cc13x2_rfc_t *rfc, uint8_t byte, int8_t rssi) {
    if (!rfc->booted || !rfc->rx.active) return;
    if (rfc->prop) prop_rx_byte(rfc, byte, rssi);
}

/* The bus saw the stream stop mid-frame (collision, sender gone): abandon
 * the frame as the modem's lost-sync detection would. */
void cc13x2_rfc_rx_stall(cc13x2_rfc_t *rfc) {
    if (!rfc->rx.active || rfc->rx.state == RX_HUNT) return;
    uint32_t e = rx_queue_entry(rfc, rfc->rx.op);
    if (e && rd8(rfc, e + 4) == DE_BUSY) wr8(rfc, e + 4, DE_ACTIVE);
    RTRC(rfc, "prop RX stalled mid-frame, dropped");
    prop_rx_frame_end(rfc);
}

/* ============================================================
 * Doorbell: direct and immediate commands
 * ============================================================ */

static int8_t current_rssi(cc13x2_rfc_t *rfc) {
    if (rfc->rx.active && rfc->rx.state != RX_HUNT) return rfc->rx.rssi;
    if (rfc->busy_cb && rfc->busy_cb(rfc->busy_user)) return RSSI_BUSY_DBM;
    return RSSI_IDLE_DBM;
}

static uint32_t direct_cmd(cc13x2_rfc_t *rfc, uint32_t cmdr) {
    uint16_t id = (uint16_t)(cmdr >> 16);
    switch (id) {
    case CMD_BUS_REQUEST:
    case CMD_PING:
    case 0x0607:                      /* CMD0: patch/clock control (RF driver) */
        return CMDSTA_DONE;
    case CMD_START_RAT:
        rfc->rat_running = true;
        return CMDSTA_DONE;
    case CMD_GET_RSSI: {
        if (!rfc->rx.active) return (0x80u << 16) | CMDSTA_DONE;   /* RF_GET_RSSI_ERROR_VAL */
        return ((uint32_t)(uint8_t)current_rssi(rfc) << 16) | CMDSTA_DONE;
    }
    case CMD_STOP:
    case CMD_ABORT: {
        bool abort = (id == CMD_ABORT);
        for (int k = 0; k < 2; k++) {
            cc13x2_rfc_op_t *s = k ? &rfc->fg : &rfc->bg;
            if (!s->addr) continue;
            if (rfc->rx.active && rfc->rx.op == s->addr) {
                prop_rx_stop(rfc, s, abort);
            } else if (s->phase < 0) {                 /* not started yet */
                cc13x2_cancel(rfc->soc, &s->ev);
                op_finish(rfc, s, abort ? ST_DONE_ABORT : ST_DONE_STOPPED, false, false);
            } else if (abort) {
                cc13x2_cancel(rfc->soc, &s->ev);
                op_finish(rfc, s, rfc->prop ? PROP_DONE_ABORT : ST_DONE_ABORT, false, false);
            } else {
                s->stop_req = true;                    /* ends after the current step */
            }
        }
        return CMDSTA_DONE;
    }
    default:
        RTRC(rfc, "unknown direct command 0x%04x", id);
        return CMDSTA_UNKNOWN_DIR_CMD;
    }
}

static uint32_t immediate_cmd(cc13x2_rfc_t *rfc, uint32_t addr, uint16_t cmd) {
    (void)addr;
    switch (cmd) {
    case CMD_SET_TX_POWER:
    case CMD_SET_RAT_CMP:
    case CMD_UPDATE_FS:
        return CMDSTA_DONE;
    default:
        RTRC(rfc, "unknown immediate command 0x%04x", cmd);
        return CMDSTA_UNKNOWN_CMD;
    }
}

static void doorbell(cc13x2_rfc_t *rfc, uint32_t cmdr) {
    uint32_t sta;
    if (!rfc->booted) {
        sta = CMDSTA_CONTEXT_ERROR;
    } else if ((cmdr & 3u) == 1u) {
        sta = direct_cmd(rfc, cmdr);
    } else if ((cmdr & 3u) != 0 || cmdr == 0) {
        sta = CMDSTA_ILLEGAL_POINTER;
    } else {
        uint16_t cmd = rd16(rfc, cmdr);
        if (cmd & 0x0800) {
            /* Radio operation: accepted, runs asynchronously. */
            cc13x2_rfc_op_t *s = slot_for(rfc, cmd);
            if (s->addr) {
                sta = CMDSTA_SCHEDULING_ERROR;
                RTRC(rfc, "radio op 0x%04x refused: slot busy with 0x%04x", cmd, s->cmd);
            } else {
                sta = CMDSTA_DONE;
                rfc->cmdsta = sta;
                rfc->ackifg = 1;
                op_start(rfc, s, cmdr);
                return;
            }
        } else {
            sta = immediate_cmd(rfc, cmdr, cmd);
        }
    }
    RTRC(rfc, "CMDR 0x%08x -> CMDSTA 0x%08x", cmdr, sta);
    rfc->cmdsta = sta;
    rfc->ackifg = 1;
}

/* ============================================================
 * Registers
 * ============================================================ */

static void boot_cb(void *user, cpu_event_t *ev) {
    (void)ev;
    cc13x2_rfc_t *rfc = user;
    if (!(rfc->pwmclken & PWM_CPE) || !cc13x2_rfc_domain_on(rfc->soc)) return;
    rfc->booted = true;
    RTRC(rfc, "CPE booted");
    raise_cpe(rfc, IRQ_MODULES_UNLOCKED | IRQ_BOOT_DONE);
}

uint32_t cc13x2_rfc_pwr_read(cc13x2_rfc_t *rfc, uint32_t off) {
    if ((off & ~3u) == 0)
        return (rfc->pwmclken | (rfc->rat_running ? PWM_RAT : 0)) >> ((off & 3u) * 8u);
    return 0;
}

void cc13x2_rfc_pwr_write(cc13x2_rfc_t *rfc, uint32_t off, uint32_t val) {
    if ((off & ~3u) != 0) return;
    uint32_t was = rfc->pwmclken;
    rfc->pwmclken = val & 0x7FFu;
    if (!(was & PWM_CPE) && (rfc->pwmclken & PWM_CPE) && !rfc->booted &&
        cc13x2_rfc_domain_on(rfc->soc)) {
        rfc->boot_ev.callback = boot_cb;
        rfc->boot_ev.user_data = rfc;
        cc13x2_schedule_at(rfc->soc, &rfc->boot_ev, now_ns(rfc) + BOOT_NS);
    }
    if (!(rfc->pwmclken & PWM_CPE) && was & PWM_CPE) {
        /* CPE clock gated: the radio CPU stops (RFCClockDisable /
         * RFCSynthPowerDown at power-down). */
        rfc->booted = false;
    }
    if (!(rfc->pwmclken & PWM_RAT) && (was & PWM_RAT)) rfc->rat_running = false;
}

uint32_t cc13x2_rfc_dbell_read(cc13x2_rfc_t *rfc, uint32_t off) {
    uint32_t v = 0;
    switch (off & ~3u) {
    case DB_CMDR:      v = 0; break;                  /* consumed at once */
    case DB_CMDSTA:    v = rfc->cmdsta; break;
    case DB_RFHWIFG:   v = rfc->hwifg; break;
    case DB_RFHWIEN:   v = rfc->hwien; break;
    case DB_RFCPEIFG:  v = rfc->cpeifg; break;
    case DB_RFCPEIEN:  v = rfc->cpeien; break;
    case DB_RFCPEISL:  v = rfc->cpeisl; break;
    case DB_RFACKIFG:  v = rfc->ackifg; break;
    case DB_SYSGPOCTL: v = rfc->sysgpoctl; break;
    }
    return v >> ((off & 3u) * 8u);
}

void cc13x2_rfc_dbell_write(cc13x2_rfc_t *rfc, uint32_t off, uint32_t val) {
    switch (off & ~3u) {
    case DB_CMDR:
        doorbell(rfc, val);
        cc13x2_irq(rfc->soc, CC13X2_IRQ_RFC_CMD_ACK);
        break;
    /* Interrupt flags: write 0 to clear, 1 has no effect (hw_rfc_dbell.h). */
    case DB_RFHWIFG:   rfc->hwifg &= val; break;
    case DB_RFCPEIFG:  rfc->cpeifg &= val; break;
    case DB_RFHWIEN:   rfc->hwien = val; update_irqs(rfc); break;
    case DB_RFCPEIEN:  rfc->cpeien = val; update_irqs(rfc); break;
    case DB_RFCPEISL:  rfc->cpeisl = val; update_irqs(rfc); break;
    case DB_RFACKIFG:  rfc->ackifg &= val & 1u; break;
    case DB_SYSGPOCTL: rfc->sysgpoctl = val; break;
    }
}

/* RFC_RAT: RATCNT at +0x04 (inc/hw_rfc_rat.h). */
uint32_t cc13x2_rfc_rat_read(cc13x2_rfc_t *rfc, uint32_t off) {
    if ((off & ~3u) == 0x04)
        return rat_now(rfc) >> ((off & 3u) * 8u);
    return 0;
}

/* ============================================================
 * Lifecycle
 * ============================================================ */

static void rfc_clear(cc13x2_rfc_t *rfc) {
    cc13x2_rfc_tx_fn cb = rfc->tx_cb;
    void *u0 = rfc->tx_user[0], *u1 = rfc->tx_user[1];
    cc13x2_rfc_busy_fn bcb = rfc->busy_cb;
    void *bu = rfc->busy_user;
    struct cc13x2_soc *soc = rfc->soc;
    int ch = rfc->channel, idx = rfc->radio_idx;
    uint64_t st[5] = { rfc->stat_tx, rfc->stat_rx_ok, rfc->stat_rx_crc_err,
                       rfc->stat_rx_buf_full, rfc->stat_cmds };
    if (soc) {
        cc13x2_cancel(soc, &rfc->boot_ev);
        cc13x2_cancel(soc, &rfc->bg.ev);
        cc13x2_cancel(soc, &rfc->fg.ev);
    }
    memset(rfc, 0, sizeof(*rfc));
    rfc->soc = soc;
    rfc->tx_cb = cb; rfc->tx_user[0] = u0; rfc->tx_user[1] = u1;
    rfc->busy_cb = bcb; rfc->busy_user = bu;
    rfc->channel = ch; rfc->radio_idx = idx;
    rfc->stat_tx = st[0]; rfc->stat_rx_ok = st[1]; rfc->stat_rx_crc_err = st[2];
    rfc->stat_rx_buf_full = st[3]; rfc->stat_cmds = st[4];
}

void cc13x2_rfc_init(cc13x2_rfc_t *rfc, struct cc13x2_soc *soc) {
    memset(rfc, 0, sizeof(*rfc));
    rfc->soc = soc;
    rfc->channel = -1;
    rfc->radio_idx = 1;
}

void cc13x2_rfc_reset(cc13x2_rfc_t *rfc) {
    rfc_clear(rfc);
}

/* Domain off: the RF core loses all state but its RAM (the patch area);
 * the radio stays tuned in the medium's view until it is next set. */
void cc13x2_rfc_domain_changed(cc13x2_rfc_t *rfc, bool on) {
    if (on) return;
    if (rfc->booted || rfc->bg.addr || rfc->fg.addr) RTRC(rfc, "RF core domain off");
    rfc_clear(rfc);
}

void cc13x2_rfc_set_tx_listener(cc13x2_rfc_t *rfc, cc13x2_rfc_tx_fn cb,
                                void *user_subghz, void *user_24ghz) {
    rfc->tx_cb = cb;
    rfc->tx_user[1] = user_subghz;
    rfc->tx_user[0] = user_24ghz;
}

void cc13x2_rfc_set_channel_busy_query(cc13x2_rfc_t *rfc, cc13x2_rfc_busy_fn cb,
                                       void *user) {
    rfc->busy_cb = cb;
    rfc->busy_user = user;
}

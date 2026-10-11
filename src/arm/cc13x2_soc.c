/*
 * TI CC13x2 / CC26x2 SoC peripheral bundle — see include/arm/cc13x2_soc.h.
 *
 * Register offsets and bit positions are those of the SimpleLink core SDK
 * device headers (source/ti/devices/cc13x2_cc26x2/inc/hw_*.h), which follow
 * the CC13x2/CC26x2 Technical Reference Manual (SWCU185).  Each behaviour
 * below says where it comes from; where the documentation leaves it open
 * the comment says so instead of guessing silently.
 */
#include "cc13x2_soc.h"
#include "arm_nvic.h"
#include "cpu_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const size_t  cc13x2_rom_image_size;
extern const uint8_t cc13x2_rom_image[];

/* CC13X2_TRACE=1: log every access to a window modelled only as a plain
 * register store, with the PC — the first thing to read when firmware
 * stalls on a status bit this file does not yet produce.  CC13X2_TRACE=2
 * logs the modelled windows too (not the memories). */
static int cc13x2_trace = -1;
static bool trace_on(void) {
    if (cc13x2_trace < 0) {
        const char *e = getenv("CC13X2_TRACE");
        cc13x2_trace = e ? atoi(e) : 0;
    }
    return cc13x2_trace != 0;
}

static inline int64_t live_ns(cc13x2_soc_t *soc) {
    arm_cpu_t *cpu = &soc->plat->cpu;
    return arm_cycles_to_ns(cpu->cycles, cpu->cpu_freq_hz);
}

/* Schedule at an absolute time in the cycle clock's ns base (live_ns),
 * rounded up to the first cycle at or after it.  arm_schedule_event_ns
 * converts against cpu->sim_time_ns, which trails the cycle clock by the
 * boot pre-run and the slice in progress, so a deadline computed from
 * live_ns lands late by that gap — first seen here as every RTC wakeup
 * firing 158 ms late. Same reasoning as nrf54l_schedule_at_cycle_ns. */
static void schedule_at(cc13x2_soc_t *soc, cpu_event_t *ev, int64_t fire_ns) {
    arm_cpu_t *cpu = &soc->plat->cpu;
    int64_t now = live_ns(soc);
    int64_t delta = fire_ns > now ? fire_ns - now : 0;
    arm_schedule_event(cpu, ev, cpu->cycles + cpu_ns_to_cycles_ceil(delta, cpu->cpu_freq_hz));
}

static inline uint32_t width_of(cc13x2_soc_t *soc) {
    uint8_t w = soc->plat->cpu.io_width;
    return w ? w : 4;
}

/* Merge an access of the current width into a 32-bit register image. */
static uint32_t merge_write(cc13x2_soc_t *soc, uint32_t old, uint32_t addr,
                            uint32_t val) {
    uint32_t w = width_of(soc);
    if (w >= 4) return val;
    uint32_t sh = (addr & 3u) * 8u;
    uint32_t mask = (w == 2 ? 0xFFFFu : 0xFFu) << sh;
    return (old & ~mask) | ((val << sh) & mask);
}

static void irq_raise(cc13x2_soc_t *soc, int irq) {
    arm_nvic_set_pending(&soc->plat->nvic, irq);
}

/* ============================================================
 * Page table
 * ============================================================ */

enum {
    P_STORE = 0,     /* plain read-back register store */
    P_PRCM,
    P_AON_PMCTL,
    P_AON_RTC,
    P_AON_IOC,
    P_VIMS,
    P_FLASH,
    P_GPIO,
    P_UART0,
    P_DDI0,
    P_ADI2,
    P_ADI3,
    P_ADI4,
    P_AUX_SYSIF,
    P_SMPH,          /* MCU semaphores */
    P_AUX_SMPH,
    P_FCFG1,
    P_CCFG,
    P_ROM,
    P_HAPI,
    P_GPRAM,
    P_RFC_RAM,
    P_WDT,
};

typedef struct {
    uint32_t    base, size;
    const char *name;
    int         kind;
    bool        nonbuf;      /* also reachable through the 0x6xxx_xxxx alias */
} page_desc_t;

/* inc/hw_memmap.h. "nonbuf" marks the windows driverlib reaches through
 * the non-buffered alias (0x6000_0000 + offset; e.g. GPIO's DOUTSET via
 * GPIO_NONBUF_BASE). */
static const page_desc_t page_descs[] = {
    { CC13X2_ROM_BASE,     0x4000,  "ROM",       P_ROM,      false },
    { CC13X2_HAPI_PORT,    0x1000,  "HAPI",      P_HAPI,     false },
    { CC13X2_GPRAM_BASE,   CC13X2_GPRAM_SIZE, "GPRAM", P_GPRAM, false },
    { CC13X2_RFC_RAM_BASE, 0x8000,  "RFC_RAM",   P_RFC_RAM,  false },
    { 0x40000000, 0x1000, "SSI0",       P_STORE,     false },
    { 0x40001000, 0x1000, "UART0",      P_UART0,     false },
    { 0x40002000, 0x1000, "I2C0",       P_STORE,     false },
    { 0x40008000, 0x1000, "SSI1",       P_STORE,     false },
    { 0x4000B000, 0x1000, "UART1",      P_STORE,     false },
    { 0x40010000, 0x1000, "GPT0",       P_STORE,     false },
    { 0x40011000, 0x1000, "GPT1",       P_STORE,     false },
    { 0x40012000, 0x1000, "GPT2",       P_STORE,     false },
    { 0x40013000, 0x1000, "GPT3",       P_STORE,     false },
    { 0x40020000, 0x1000, "UDMA0",      P_STORE,     false },
    { 0x40022000, 0x1000, "GPIO",       P_GPIO,      true  },
    { 0x40024000, 0x1000, "CRYPTO",     P_STORE,     false },
    { 0x40028000, 0x1000, "TRNG",       P_STORE,     false },
    { 0x40030000, 0x4000, "FLASH",      P_FLASH,     false },
    { 0x40034000, 0x1000, "VIMS",       P_VIMS,      false },
    { 0x40035000, 0x1000, "SRAM_MMR",   P_STORE,     false },
    { 0x40040000, 0x1000, "RFC_PWR",    P_STORE,     true  },
    { 0x40041000, 0x1000, "RFC_DBELL",  P_STORE,     true  },
    { 0x40043000, 0x1000, "RFC_RAT",    P_STORE,     true  },
    { 0x40044000, 0x1000, "RFC_FSCA",   P_STORE,     false },
    { 0x40080000, 0x1000, "WDT",        P_WDT,       false },
    { 0x40081000, 0x1000, "IOC",        P_STORE,     false },
    { 0x40082000, 0x1000, "PRCM",       P_PRCM,      true  },
    { 0x40083000, 0x1000, "EVENT",      P_STORE,     false },
    { 0x40084000, 0x1000, "SMPH",       P_SMPH,      false },
    { 0x40086000, 0x0200, "ADI2",       P_ADI2,      false },
    { 0x40086200, 0x0200, "ADI3",       P_ADI3,      false },
    { 0x40090000, 0x1000, "AON_PMCTL",  P_AON_PMCTL, true  },
    { 0x40092000, 0x1000, "AON_RTC",    P_AON_RTC,   true  },
    { 0x40093000, 0x1000, "AON_EVENT",  P_STORE,     true  },
    { 0x40094000, 0x1000, "AON_IOC",    P_AON_IOC,   true  },
    { 0x40095000, 0x1000, "AON_BATMON", P_STORE,     false },
    { 0x400C1000, 0x1000, "AUX_SPIM",   P_STORE,     false },
    { 0x400C3000, 0x1000, "AUX_TIMER2", P_STORE,     false },
    { 0x400C4000, 0x1000, "AUX_TDC",    P_STORE,     false },
    { 0x400C5000, 0x1000, "AUX_EVCTL",  P_STORE,     false },
    { 0x400C6000, 0x1000, "AUX_SYSIF",  P_AUX_SYSIF, false },
    { 0x400C7000, 0x1000, "AUX_TIMER01",P_STORE,     false },
    { 0x400C8000, 0x1000, "AUX_SMPH",   P_AUX_SMPH,  false },
    { 0x400C9000, 0x1000, "AUX_ANAIF",  P_STORE,     false },
    { 0x400CA000, 0x1000, "AUX_DDI0",   P_DDI0,      false },
    { 0x400CB000, 0x1000, "AUX_ADI4",   P_ADI4,      false },
    { 0x400CC000, 0x4000, "AUX_AIODIO", P_STORE,     false },
    { 0x50000000, 0x1000, "FLASH_CFG",  P_STORE,     false },
    { CC13X2_FCFG1_BASE, CC13X2_FCFG1_SIZE, "FCFG1", P_FCFG1, false },
    { CC13X2_CCFG_BASE,  CC13X2_CCFG_SIZE,  "CCFG",  P_CCFG,  false },
};
#define NUM_PAGE_DESCS ((int)(sizeof(page_descs) / sizeof(page_descs[0])))

static cc13x2_page_t *find_page(cc13x2_soc_t *soc, const char *name) {
    for (int i = 0; i < soc->num_pages; i++)
        if (strcmp(soc->pages[i].name, name) == 0) return &soc->pages[i];
    return NULL;
}

#define REG(page, off)  ((page)->regs[(off) >> 2])

/* ============================================================
 * Factory configuration (FCFG1)
 * ============================================================ */

/* FCFG1 offsets used here (inc/hw_fcfg1.h). */
#define FCFG1_MISC_CONF_1        0x0A0
#define FCFG1_USER_ID            0x294
#define FCFG1_MAC_BLE_0          0x2E8
#define FCFG1_MAC_BLE_1          0x2EC
#define FCFG1_MAC_15_4_0         0x2F0
#define FCFG1_MAC_15_4_1         0x2F4
#define FCFG1_ICEPICK_DEVICE_ID  0x318
#define FCFG1_FCFG1_REVISION     0x31C
#define FCFG1_TFW_FT             0x41C
#define FCFG1_IOCONF             0x344

/* Identity of a CC1352R (not -P) at silicon revision PG2.1, as driverlib's
 * chipinfo.c decodes it:
 *   ICEPICK_DEVICE_ID: PG_REV[31:28] = 3 ("PG2.1 or later"), WAFER_ID[27:12]
 *     = 0xBB41 (FAMILY_CC13x2_CC26x2), MANUFACTURER_ID[11:0] = 0x02F (TI).
 *   USER_ID: PROTOCOL[15:12] = 0xF, CC13[23] = 1, PA[25] = 0 -> CC1352.
 *     PKG[18:16] = 3 is PACKAGE_7x7 (the LaunchPad's RGZ package).
 *   MISC_CONF_1.DEVICE_MINOR_REV = 0 -> HWREV_2_1.
 *   TFW_FT (factory test firmware revision): TI's prebuilt Power_init()
 *     spins forever unless TFW_FT % 10000 > 682 (rf_multiMode / power
 *     library, SDK 4.10.02); 700 satisfies it.
 *   IOCONF.GPIO_CNT = 28: the CC1352R's 7x7 package brings out DIO3..DIO30
 *     (CC1352R datasheet, "28 GPIOs"); TI's PIN driver sizes its pin range
 *     from this field (PINCC26XX_getPinCount).
 * Everything else — the analog trim words — is zero here; the trim code
 * copies them into analog registers whose content nothing in csim reads.
 * TODO(hardware): replace with an FCFG1 dump from the LaunchPad. */
static void fcfg1_init(cc13x2_soc_t *soc) {
    memset(soc->fcfg1, 0, sizeof(soc->fcfg1));
    uint32_t *f = soc->fcfg1;
    f[FCFG1_ICEPICK_DEVICE_ID / 4] = (3u << 28) | (0xBB41u << 12) | 0x02Fu;
    f[FCFG1_USER_ID / 4]           = (1u << 23) | (3u << 16) | (0xFu << 12);
    f[FCFG1_MISC_CONF_1 / 4]       = 0xFFFFFF00u;   /* minor rev 0 */
    f[FCFG1_FCFG1_REVISION / 4]    = 0x00000028u;
    f[FCFG1_TFW_FT / 4]            = 700u;
    f[FCFG1_IOCONF / 4]            = 28u;
    f[FCFG1_MAC_15_4_0 / 4]        = soc->mac_15_4[0];
    f[FCFG1_MAC_15_4_1 / 4]        = soc->mac_15_4[1];
    f[FCFG1_MAC_BLE_0 / 4]         = soc->mac_15_4[0];
    f[FCFG1_MAC_BLE_1 / 4]         = soc->mac_15_4[1] & 0xFFFFu;
}

void cc13x2_soc_set_ieee_addr(cc13x2_soc_t *soc, const uint8_t addr[8]) {
    /* ieee-addr.c reads the 64-bit address from MAC_15_4_0 (low word) and
     * MAC_15_4_1, most significant byte last in memory, and reverses it. */
    soc->mac_15_4[0] = (uint32_t)addr[7] | ((uint32_t)addr[6] << 8) |
                       ((uint32_t)addr[5] << 16) | ((uint32_t)addr[4] << 24);
    soc->mac_15_4[1] = (uint32_t)addr[3] | ((uint32_t)addr[2] << 8) |
                       ((uint32_t)addr[1] << 16) | ((uint32_t)addr[0] << 24);
    soc->fcfg1[FCFG1_MAC_15_4_0 / 4] = soc->mac_15_4[0];
    soc->fcfg1[FCFG1_MAC_15_4_1 / 4] = soc->mac_15_4[1];
}

/* ============================================================
 * Oscillators (AUX_DDI0_OSC) — inc/hw_ddi_0_osc.h
 * ============================================================ */

#define DDI0_CTL0     0x00
#define DDI0_STAT0    0x3C
#define DDI0_STAT1    0x40
#define DDI0_STAT2    0x44

#define CTL0_SCLK_HF_SRC_SEL   0x00000001u
#define CTL0_SCLK_LF_SRC_SEL_S 2

/* XOSC_HF start-up: the TRM gives ~200-500 us for the 48 MHz crystal path;
 * TI's Power driver allows for it by polling. 300 us is a mid value. */
#define XOSC_HF_STARTUP_NS     300000

static void osc_xosc_ready_cb(void *user, cpu_event_t *ev) {
    (void)ev;
    cc13x2_soc_t *soc = user;
    soc->osc.xosc_hf_starting = false;
    soc->osc.xosc_hf_ready = true;
}

/* A write to CTL0 that selects XOSC_HF as the SCLK_HF source starts the
 * crystal; the switch itself is committed by the ROM's HFSourceSafeSwitch
 * (HAPI port below) once STAT0.PENDINGSCLKHFSWITCHING reports it ready. */
static void osc_ctl0_changed(cc13x2_soc_t *soc) {
    uint32_t ctl0 = soc->ddi0[DDI0_CTL0 / 4];
    bool want_xosc = (ctl0 & CTL0_SCLK_HF_SRC_SEL) != 0;
    if (want_xosc && !soc->osc.xosc_hf_ready && !soc->osc.xosc_hf_starting) {
        soc->osc.xosc_hf_starting = true;
        soc->osc.xosc_hf_event.callback = osc_xosc_ready_cb;
        soc->osc.xosc_hf_event.user_data = soc;
        schedule_at(soc, &soc->osc.xosc_hf_event, live_ns(soc) + XOSC_HF_STARTUP_NS);
    }
    if (!want_xosc && soc->osc.hf_src == 0) {
        /* Back on RCOSC_HF with nothing pending: the crystal may stop. */
        if (soc->osc.xosc_hf_starting)
            arm_cancel_event(&soc->plat->cpu, &soc->osc.xosc_hf_event);
        soc->osc.xosc_hf_starting = false;
        soc->osc.xosc_hf_ready = false;
    }
    /* SCLK_LF follows its selection directly. The LF crystal's real
     * start-up is hundreds of ms and qualified by hardware; nothing in the
     * firmware waits on it except through STAT0.SCLK_LF_SRC, and csim's
     * RTC runs at exactly 32768 Hz whatever the source. */
    soc->osc.lf_src = (int)((ctl0 >> CTL0_SCLK_LF_SRC_SEL_S) & 3u);
}

static bool osc_switch_pending(cc13x2_soc_t *soc) {
    int sel = (soc->ddi0[DDI0_CTL0 / 4] & CTL0_SCLK_HF_SRC_SEL) ? 1 : 0;
    if (sel == soc->osc.hf_src) return false;
    return sel == 0 || soc->osc.xosc_hf_ready;
}

static uint32_t osc_status(cc13x2_soc_t *soc, uint32_t off) {
    switch (off) {
    case DDI0_STAT0: {
        uint32_t v = 0;
        v |= (uint32_t)soc->osc.lf_src << 29;           /* SCLK_LF_SRC */
        v |= (uint32_t)soc->osc.hf_src << 28;           /* SCLK_HF_SRC */
        v |= 1u << 22;                                  /* RCOSC_HF_EN */
        if (soc->osc.lf_src == 2) v |= 1u << 21;        /* RCOSC_LF_EN */
        if (soc->osc.lf_src == 3) v |= 1u << 20;        /* XOSC_LF_EN */
        v |= (1u << 19) | (1u << 18);                   /* CLK_DCDC_RDY(_ACK) */
        if (soc->osc.xosc_hf_ready) v |= 1u << 15;      /* XOSC_HF_EN */
        if (osc_switch_pending(soc)) v |= 1u;           /* PENDINGSCLKHFSWITCHING */
        return v;
    }
    case DDI0_STAT1:
        /* All derived clocks enabled [14:8] and good [7:0]. */
        return 0x00007FFFu;
    case DDI0_STAT2:
        /* XOSC_HF_AMPGOOD | FREQGOOD | RF_FREQGOOD once running. */
        return soc->osc.xosc_hf_ready ? 0x7u : 0x0u;
    }
    return soc->ddi0[off / 4];
}

/* HAPI port: the stand-in ROM's HFSourceSafeSwitch commits the switch. */
static void hapi_write(cc13x2_soc_t *soc, uint32_t off, uint32_t val) {
    if (off == 0 && val == 1) {
        if (osc_switch_pending(soc))
            soc->osc.hf_src = (soc->ddi0[DDI0_CTL0 / 4] & CTL0_SCLK_HF_SRC_SEL) ? 1 : 0;
        return;
    }
    fprintf(stderr, "[cc13x2] HAPI port: unknown request 0x%x at +0x%x\n", val, off);
}

/* DDI: 32-bit registers.  DIR 0x000, SET 0x080, CLR 0x100, and masked
 * writes MASK4B 0x200 (byte per nibble), MASK8B 0x300 (halfword per byte),
 * MASK16B 0x400 (word per halfword): each n-bit unit of the register file
 * maps to a 2n-bit write carrying {mask, data} (inc/hw_ddi.h,
 * driverlib/ddi.c DDI16BitWrite / DDI16BitfieldWrite). */
static void ddi0_write(cc13x2_soc_t *soc, uint32_t off, uint32_t val) {
    uint32_t *r = soc->ddi0;
    uint32_t nregs = sizeof(soc->ddi0);   /* bytes of register file */
    if (off < 0x080) {
        if (off < nregs)
            r[off / 4] = merge_write(soc, r[off / 4], off, val);
    } else if (off < 0x100) {
        uint32_t o = off - 0x080;
        if (o < nregs) r[o / 4] |= merge_write(soc, 0, o, val);
    } else if (off < 0x200) {
        uint32_t o = off - 0x100;
        if (o < nregs) r[o / 4] &= ~merge_write(soc, 0, o, val);
    } else if (off < 0x300) {            /* 4-bit: one byte per nibble */
        uint32_t o = off - 0x200, nib = o;        /* nibble index in file */
        uint32_t reg = nib / 8, sh = (nib % 8) * 4;
        uint32_t mask = ((val >> 4) & 0xFu) << sh, data = (val & 0xFu) << sh;
        if (reg * 4 < nregs) r[reg] = (r[reg] & ~mask) | (data & mask);
    } else if (off < 0x400) {            /* 8-bit: one halfword per byte */
        uint32_t byte = (off - 0x300) / 2;
        uint32_t reg = byte / 4, sh = (byte % 4) * 8;
        uint32_t mask = ((val >> 8) & 0xFFu) << sh, data = (val & 0xFFu) << sh;
        if (reg * 4 < nregs) r[reg] = (r[reg] & ~mask) | (data & mask);
    } else if (off < 0x500) {            /* 16-bit: one word per halfword */
        uint32_t half = (off - 0x400) / 4;
        uint32_t reg = half / 2, sh = (half % 2) * 16;
        uint32_t mask = ((val >> 16) & 0xFFFFu) << sh, data = (val & 0xFFFFu) << sh;
        if (reg * 4 < nregs) r[reg] = (r[reg] & ~mask) | (data & mask);
    }
    uint32_t reg_touched = (off < 0x200) ? (off & 0x7F) :
                           (off < 0x300) ? ((off - 0x200) / 8) * 4 :
                           (off < 0x400) ? ((off - 0x300) / 8) * 4 :
                                           ((off - 0x400) / 8) * 4;
    if (reg_touched == DDI0_CTL0) osc_ctl0_changed(soc);
}

static uint32_t ddi0_read(cc13x2_soc_t *soc, uint32_t off) {
    if (off >= 0x080) return 0;          /* the write-only windows read 0 */
    uint32_t aligned = off & ~3u;
    if (aligned >= sizeof(soc->ddi0)) return 0;
    return osc_status(soc, aligned) >> ((off & 3u) * 8u);
}

/* ADI: sixteen 8-bit registers.  DIR 0x00, SET 0x10, CLR 0x20, SLAVESTAT
 * 0x30, SLAVECONF 0x38, MASK4B 0x40 (byte per nibble), MASK8B 0x60
 * (halfword per register), MASK16B 0x80 (word per register pair)
 * (inc/hw_adi.h, driverlib/adi.h). */
static void adi_write(cc13x2_soc_t *soc, uint8_t *r, uint32_t off, uint32_t val) {
    uint32_t w = width_of(soc);
    if (off < 0x30) {
        uint32_t win = off & 0x30, base = off & 0x0F;
        for (uint32_t i = 0; i < w && base + i < 16; i++) {
            uint8_t b = (uint8_t)(val >> (8 * i));
            if (win == 0x00) r[base + i] = b;
            else if (win == 0x10) r[base + i] |= b;
            else r[base + i] &= (uint8_t)~b;
        }
    } else if (off >= 0x40 && off < 0x60) {
        uint32_t nib = off - 0x40, reg = nib / 2, sh = (nib % 2) * 4;
        uint8_t mask = (uint8_t)(((val >> 4) & 0xFu) << sh);
        uint8_t data = (uint8_t)((val & 0xFu) << sh);
        r[reg] = (uint8_t)((r[reg] & ~mask) | (data & mask));
    } else if (off >= 0x60 && off < 0x80) {
        uint32_t reg = (off - 0x60) / 2;
        uint8_t mask = (uint8_t)(val >> 8), data = (uint8_t)val;
        r[reg] = (uint8_t)((r[reg] & ~mask) | (data & mask));
    } else if (off >= 0x80 && off < 0xA0) {
        uint32_t reg = ((off - 0x80) / 4) * 2;
        uint16_t mask = (uint16_t)(val >> 16), data = (uint16_t)val;
        for (int i = 0; i < 2; i++) {
            uint8_t m = (uint8_t)(mask >> (8 * i)), d = (uint8_t)(data >> (8 * i));
            r[reg + i] = (uint8_t)((r[reg + i] & ~m) | (d & m));
        }
    }
}

static uint32_t adi_read(uint8_t *r, uint32_t off) {
    if (off < 0x10) {
        uint32_t v = 0;
        for (uint32_t i = 0; i < 4 && off + i < 16; i++)
            v |= (uint32_t)r[off + i] << (8 * i);
        return v;
    }
    if (off == 0x30) return 0;   /* SLAVESTAT: no request outstanding */
    return 0;
}

/* ============================================================
 * PRCM — inc/hw_prcm.h
 * ============================================================ */

#define PRCM_CLKLOADCTL   0x028
#define PRCM_PDCTL0       0x12C
#define PRCM_PDCTL0RFC    0x130
#define PRCM_PDCTL0SERIAL 0x134
#define PRCM_PDCTL0PERIPH 0x138
#define PRCM_PDSTAT0      0x140
#define PRCM_PDSTAT0RFC   0x144
#define PRCM_PDSTAT0SER   0x148
#define PRCM_PDSTAT0PER   0x14C
#define PRCM_PDCTL1       0x17C
#define PRCM_PDCTL1CPU    0x184
#define PRCM_PDCTL1RFC    0x188
#define PRCM_PDCTL1VIMS   0x18C
#define PRCM_PDSTAT1      0x194
#define PRCM_PDSTAT1BUS   0x198
#define PRCM_PDSTAT1RFC   0x19C
#define PRCM_PDSTAT1CPU   0x1A0
#define PRCM_PDSTAT1VIMS  0x1A4
#define PRCM_RFCMODEHWOPT 0x1D4

/* PDCTL0: RFC_ON bit0, SERIAL_ON bit1, PERIPH_ON bit2.
 * PDCTL1: CPU_ON bit1, RFC_ON bit2, VIMS_MODE bit3.
 * Power domains switch in a few us on silicon; the driverlib callers poll
 * PDSTAT, so reporting the new state at once is indistinguishable. */
static bool rfc_powered(cc13x2_soc_t *soc) {
    return (soc->pd0 & 1u) || (soc->pd1 & 4u);
}

static uint32_t prcm_read(cc13x2_soc_t *soc, cc13x2_page_t *p, uint32_t off) {
    switch (off & ~3u) {
    case PRCM_CLKLOADCTL: return 0x2u;                 /* LOAD_DONE */
    case PRCM_PDCTL0:     return soc->pd0;
    case PRCM_PDCTL0RFC:  return soc->pd0 & 1u;
    case PRCM_PDCTL0SERIAL: return (soc->pd0 >> 1) & 1u;
    case PRCM_PDCTL0PERIPH: return (soc->pd0 >> 2) & 1u;
    case PRCM_PDSTAT0:    return (soc->pd0 & 6u) | (rfc_powered(soc) ? 1u : 0u);
    case PRCM_PDSTAT0RFC: return rfc_powered(soc) ? 1u : 0u;
    case PRCM_PDSTAT0SER: return (soc->pd0 >> 1) & 1u;
    case PRCM_PDSTAT0PER: return (soc->pd0 >> 2) & 1u;
    case PRCM_PDCTL1:     return soc->pd1;
    case PRCM_PDSTAT1:
        return 0x10u | 0x08u | 0x02u | (rfc_powered(soc) ? 0x04u : 0u);
    case PRCM_PDSTAT1BUS: case PRCM_PDSTAT1CPU: case PRCM_PDSTAT1VIMS:
        return 1u;
    case PRCM_PDSTAT1RFC: return rfc_powered(soc) ? 1u : 0u;
    }
    return REG(p, off & ~3u) >> ((off & 3u) * 8u);
}

static void prcm_write(cc13x2_soc_t *soc, cc13x2_page_t *p, uint32_t off, uint32_t val) {
    uint32_t a = off & ~3u;
    switch (a) {
    case PRCM_CLKLOADCTL: return;
    case PRCM_PDCTL0:     soc->pd0 = val & 7u; return;
    case PRCM_PDCTL0RFC:  soc->pd0 = (soc->pd0 & ~1u) | (val & 1u); return;
    case PRCM_PDCTL0SERIAL: soc->pd0 = (soc->pd0 & ~2u) | ((val & 1u) << 1); return;
    case PRCM_PDCTL0PERIPH: soc->pd0 = (soc->pd0 & ~4u) | ((val & 1u) << 2); return;
    case PRCM_PDCTL1:     soc->pd1 = val & 0xEu; return;
    case PRCM_PDCTL1CPU:  soc->pd1 = (soc->pd1 & ~2u) | ((val & 1u) << 1); return;
    case PRCM_PDCTL1RFC:  soc->pd1 = (soc->pd1 & ~4u) | ((val & 1u) << 2); return;
    case PRCM_PDCTL1VIMS: soc->pd1 = (soc->pd1 & ~8u) | ((val & 1u) << 3); return;
    }
    REG(p, a) = merge_write(soc, REG(p, a), off, val);
}

/* ============================================================
 * AON_PMCTL — inc/hw_aon_pmctl.h
 * ============================================================ */

#define AON_PMCTL_PWRSTAT  0x14
#define AON_PMCTL_RESETCTL 0x28
#define AON_PMCTL_SLEEPCTL 0x2C
#define RESETCTL_SYSRESET  0x80000000u

static void aon_pmctl_write(cc13x2_soc_t *soc, cc13x2_page_t *p, uint32_t off, uint32_t val) {
    uint32_t a = off & ~3u;
    uint32_t v = merge_write(soc, REG(p, a), off, val);
    if (a == AON_PMCTL_RESETCTL) {
        if (v & RESETCTL_SYSRESET) {
            /* SysCtrlSystemReset(): a full system reset. RESET_SRC[3:1]
             * reads SYSRESET (6) on the next boot. */
            REG(p, a) = (REG(p, a) & ~0xEu) | 0xCu;
            arm_request_reset(&soc->plat->cpu);
            return;
        }
        /* BOOT_DET_x_SET / _CLR [25:24],[17:16] act on BOOT_DET [13:12]. */
        uint32_t bd = (REG(p, a) >> 12) & 3u;
        bd |= (v >> 16) & 3u;
        bd &= ~((v >> 24) & 3u);
        v = (v & ~0x3000u) | (bd << 12);
        v &= ~0xEu;                      /* RESET_SRC is read-only */
        v |= REG(p, a) & 0xEu;
    }
    REG(p, a) = v;
}

static uint32_t aon_pmctl_read(cc13x2_soc_t *soc, cc13x2_page_t *p, uint32_t off) {
    (void)soc;
    uint32_t a = off & ~3u;
    if (a == AON_PMCTL_PWRSTAT)
        /* AUX_RESET_DONE | AUX_BUS_RESET_DONE; JTAG_PD_ON stays 0 — no
         * debugger is attached, and TI's Power driver treats a powered
         * JTAG domain as a reason to stay out of standby. */
        return 0x3u >> ((off & 3u) * 8u);
    return REG(p, a) >> ((off & 3u) * 8u);
}

/* ============================================================
 * AON RTC — inc/hw_aon_rtc.h, driverlib/aon_rtc.c
 * ============================================================ */

#define RTC_CTL      0x00
#define RTC_EVFLAGS  0x04
#define RTC_SEC      0x08
#define RTC_SUBSEC   0x0C
#define RTC_SUBSECINC 0x10
#define RTC_CHCTL    0x14
#define RTC_CH0CMP   0x18
#define RTC_CH1CMP   0x1C
#define RTC_CH2CMP   0x20
#define RTC_CH2CMPINC 0x24
#define RTC_CH1CAPT  0x28
#define RTC_SYNC     0x2C
#define RTC_TIME     0x30
#define RTC_SYNCLF   0x34

#define RTC_CTL_EN       0x00000001u
#define RTC_CTL_RESET    0x00000080u
#define RTC_CTL_COMB_S   16
/* EVFLAGS / CHCTL channel bits: CH0 bit0, CH1 bit8, CH2 bit16. */
static const uint32_t rtc_ch_bit[3] = { 1u << 0, 1u << 8, 1u << 16 };

/* 32768 Hz SCLK_LF ticks since the counter was zeroed. */
static uint64_t rtc_ticks(cc13x2_soc_t *soc) {
    if (!soc->rtc.enabled) return 0;
    int64_t el = live_ns(soc) - soc->rtc.zero_ns;
    if (el < 0) el = 0;
    return ((uint64_t)el * 4096u) / 125000000u;        /* el * 32768 / 1e9 */
}

static int64_t rtc_tick_ns(cc13x2_soc_t *soc, uint64_t tick) {
    return soc->rtc.zero_ns + (int64_t)((tick * 125000000u + 4095u) / 4096u);
}

/* The 16.16 view the compare channels and rtimer use (counter bits 47:16). */
static uint32_t rtc_time1616(uint64_t ticks) {
    return (uint32_t)(ticks << 1);
}

static void rtc_update_irq(cc13x2_soc_t *soc) {
    uint32_t comb = (soc->rtc.ctl >> RTC_CTL_COMB_S) & 7u;
    uint32_t hit = 0;
    for (int i = 0; i < 3; i++)
        if ((comb & (1u << i)) && (soc->rtc.evflags & rtc_ch_bit[i])) hit = 1;
    if (hit) irq_raise(soc, CC13X2_IRQ_AON_RTC_COMB);
}

static void rtc_arm_channel(cc13x2_soc_t *soc, int ch);

static void rtc_ch_cb(void *user, cpu_event_t *ev) {
    cc13x2_soc_t *soc = user;
    int ch = (int)(ev - soc->rtc.ch_event);
    if (!(soc->rtc.chctl & rtc_ch_bit[ch])) return;
    soc->rtc.evflags |= rtc_ch_bit[ch];
    if (ch == 2 && (soc->rtc.chctl & (1u << 18))) {      /* CH2_CONT_EN */
        soc->rtc.cmp[2] += soc->rtc.ch2cmpinc;
        rtc_arm_channel(soc, 2);
    }
    rtc_update_irq(soc);
}

/* A channel matches on the SCLK_LF edge at which the 16.16 time reaches
 * the compare value (the counter steps by 2 in that view, so an odd value
 * matches at the edge that crosses it), and the event is registered on the
 * following edge, as synchronous compare logic latches it.  That one-edge
 * lag is not stated in the TRM; it is what TI's TimerP arithmetic needs:
 * it programs compare = threshold >> 16 and, in the ISR, treats the wakeup
 * as spurious unless RTC64 / period64 has reached the service tick —
 * which an event on the matching edge itself, up to 0xFFFF below the
 * threshold, fails by one ClockP tick (observed: the UART drain timeout
 * dropped and the node slept forever). TimerP's COMPARE_MARGIN of two
 * RTC ticks fits the same pipeline.  A value written while the counter
 * already equals it matches at once; one behind the current time is not
 * reached until the 32-bit view wraps (~18 h).
 * TODO(hardware): confirm the one-edge lag on the LaunchPad. */
static void rtc_arm_channel(cc13x2_soc_t *soc, int ch) {
    arm_cpu_t *cpu = &soc->plat->cpu;
    cpu_event_t *ev = &soc->rtc.ch_event[ch];
    arm_cancel_event(cpu, ev);
    if (!soc->rtc.enabled || !(soc->rtc.chctl & rtc_ch_bit[ch])) return;
    uint64_t now = rtc_ticks(soc);
    uint32_t diff = soc->rtc.cmp[ch] - rtc_time1616(now);
    if (diff > 0x80000000u) return;
    uint64_t target = now + (diff + 1u) / 2u + 1u;   /* match edge + 1 */
    ev->callback = rtc_ch_cb;
    ev->user_data = soc;
    schedule_at(soc, ev, rtc_tick_ns(soc, target));
}

static void rtc_rearm_all(cc13x2_soc_t *soc) {
    for (int i = 0; i < 3; i++) rtc_arm_channel(soc, i);
}

static uint32_t aon_rtc_read(cc13x2_soc_t *soc, uint32_t off) {
    uint32_t a = off & ~3u, v = 0;
    uint64_t t;
    switch (a) {
    case RTC_CTL:      v = soc->rtc.ctl; break;
    case RTC_EVFLAGS:  v = soc->rtc.evflags; break;
    case RTC_SEC:
        t = rtc_ticks(soc);
        soc->rtc.subsec_latch = (uint32_t)(t << 17);
        v = (uint32_t)(t >> 15);
        break;
    case RTC_SUBSEC:   v = soc->rtc.subsec_latch; break;
    case RTC_SUBSECINC: v = 0x8000u; break;     /* 2^38 / 32768 >> 6 alignment */
    case RTC_CHCTL:    v = soc->rtc.chctl; break;
    case RTC_CH0CMP:   v = soc->rtc.cmp[0]; break;
    case RTC_CH1CMP:   v = soc->rtc.cmp[1]; break;
    case RTC_CH2CMP:   v = soc->rtc.cmp[2]; break;
    case RTC_CH2CMPINC: v = soc->rtc.ch2cmpinc; break;
    case RTC_CH1CAPT:  v = soc->rtc.ch1capt; break;
    case RTC_SYNC:     v = 0; break;
    case RTC_TIME:     v = rtc_time1616(rtc_ticks(soc)); break;
    case RTC_SYNCLF:   v = (uint32_t)(rtc_ticks(soc) & 1u); break;
    }
    return v >> ((off & 3u) * 8u);
}

static void aon_rtc_write(cc13x2_soc_t *soc, uint32_t off, uint32_t val) {
    uint32_t a = off & ~3u;
    switch (a) {
    case RTC_CTL: {
        uint32_t v = merge_write(soc, soc->rtc.ctl, off, val);
        if (v & RTC_CTL_RESET) {
            /* RESET: the counter returns to 0; the bit self-clears. */
            soc->rtc.zero_ns = live_ns(soc);
            v &= ~RTC_CTL_RESET;
        }
        bool en = (v & RTC_CTL_EN) != 0;
        if (en && !soc->rtc.enabled) {
            soc->rtc.zero_ns = live_ns(soc);
            soc->rtc.enabled = true;
        } else if (!en) {
            soc->rtc.enabled = false;
        }
        soc->rtc.ctl = v;
        rtc_rearm_all(soc);
        rtc_update_irq(soc);
        return;
    }
    case RTC_EVFLAGS:
        soc->rtc.evflags &= ~merge_write(soc, 0, off, val);
        return;
    case RTC_CHCTL:
        soc->rtc.chctl = merge_write(soc, soc->rtc.chctl, off, val);
        rtc_rearm_all(soc);
        return;
    case RTC_CH0CMP: case RTC_CH1CMP: case RTC_CH2CMP: {
        int ch = (int)(a - RTC_CH0CMP) / 4;
        soc->rtc.cmp[ch] = merge_write(soc, soc->rtc.cmp[ch], off, val);
        rtc_arm_channel(soc, ch);
        return;
    }
    case RTC_CH2CMPINC:
        soc->rtc.ch2cmpinc = merge_write(soc, soc->rtc.ch2cmpinc, off, val);
        return;
    case RTC_SYNC:
        return;
    }
}

/* ============================================================
 * GPIO — inc/hw_gpio.h
 * ============================================================ */

#define GPIO_DOUT31_0    0x80
#define GPIO_DOUTSET     0x90
#define GPIO_DOUTCLR     0xA0
#define GPIO_DOUTTGL     0xB0
#define GPIO_DIN31_0     0xC0
#define GPIO_DOE31_0     0xD0
#define GPIO_EVFLAGS     0xE0
#define GPIO_PIN_MASK    0x7FFFFFFFu

static uint32_t gpio_din(cc13x2_soc_t *soc) {
    cc13x2_gpio_t *g = &soc->gpio;
    uint32_t v = g->dout & g->doe;
    v = (v & ~g->din_forced) | (g->din_ext & g->din_forced);
    return v & GPIO_PIN_MASK;
}

static uint32_t gpio_read(cc13x2_soc_t *soc, uint32_t off) {
    cc13x2_gpio_t *g = &soc->gpio;
    uint32_t a = off & ~3u, v = 0;
    if (a < 0x20) {
        /* DOUTn_m: one byte per DIO, bit 0 = the DIO's output. */
        uint32_t dio = a;
        for (int i = 0; i < 4; i++)
            v |= ((g->dout >> (dio + (uint32_t)i)) & 1u) << (8 * i);
    } else switch (a) {
        case GPIO_DOUT31_0: v = g->dout; break;
        case GPIO_DIN31_0:  v = gpio_din(soc); break;
        case GPIO_DOE31_0:  v = g->doe; break;
        case GPIO_EVFLAGS:  v = g->evflags; break;
    }
    return v >> ((off & 3u) * 8u);
}

/* CSIM_GPIO_TRACE=1: timestamped DOUT changes (same format as nRF54L15). */
static void gpio_trace(cc13x2_soc_t *soc, uint32_t before) {
    static int on = -1;
    if (on < 0) on = getenv("CSIM_GPIO_TRACE") ? 1 : 0;
    if (!on || before == soc->gpio.dout) return;
    fprintf(stderr, "[GPIO] %9.3f DOUT 0x%08x -> 0x%08x (changed 0x%08x)\n",
            (double)live_ns(soc) / 1e9, before, soc->gpio.dout,
            before ^ soc->gpio.dout);
}

static void gpio_write_impl(cc13x2_soc_t *soc, uint32_t off, uint32_t val);

static void gpio_write(cc13x2_soc_t *soc, uint32_t off, uint32_t val) {
    uint32_t before = soc->gpio.dout;
    gpio_write_impl(soc, off, val);
    gpio_trace(soc, before);
}

static void gpio_write_impl(cc13x2_soc_t *soc, uint32_t off, uint32_t val) {
    cc13x2_gpio_t *g = &soc->gpio;
    uint32_t a = off & ~3u;
    if (off < 0x20) {
        /* Byte-wide per-DIO writes: bit 0 of each byte drives its DIO. */
        uint32_t w = width_of(soc);
        for (uint32_t i = 0; i < w; i++) {
            uint32_t dio = off + i;
            if (dio >= CC13X2_NUM_DIO) break;
            uint32_t bit = 1u << dio;
            g->dout = ((val >> (8 * i)) & 1u) ? (g->dout | bit) : (g->dout & ~bit);
        }
        return;
    }
    uint32_t v = merge_write(soc, 0, off, val);
    switch (a) {
    case GPIO_DOUT31_0: g->dout = merge_write(soc, g->dout, off, val) & GPIO_PIN_MASK; break;
    case GPIO_DOUTSET:  g->dout |= v & GPIO_PIN_MASK; break;
    case GPIO_DOUTCLR:  g->dout &= ~v; break;
    case GPIO_DOUTTGL:  g->dout ^= v & GPIO_PIN_MASK; break;
    case GPIO_DOE31_0:  g->doe = merge_write(soc, g->doe, off, val) & GPIO_PIN_MASK; break;
    case GPIO_EVFLAGS:  g->evflags &= ~v; break;
    }
}

/* ============================================================
 * UART0 — inc/hw_uart.h (TI's PL011-derived UART)
 * ============================================================ */

#define UART_DR    0x00
#define UART_RSR   0x04
#define UART_FR    0x18
#define UART_IBRD  0x24
#define UART_FBRD  0x28
#define UART_LCRH  0x2C
#define UART_CTL   0x30
#define UART_IFLS  0x34
#define UART_IMSC  0x38
#define UART_RIS   0x3C
#define UART_MIS   0x40
#define UART_ICR   0x44
#define UART_DMACTL 0x48

#define UART_INT_EOT  0x800u
#define UART_INT_RT   0x040u
#define UART_INT_TX   0x020u
#define UART_INT_RX   0x010u
#define UART_FIFO     32

/* FIFO level thresholds, IFLS TXSEL [2:0] / RXSEL [5:3]: 1/8 .. 7/8. */
static int uart_level(uint32_t sel) {
    static const int lv[5] = { 4, 8, 16, 24, 28 };
    return sel < 5 ? lv[sel] : 16;
}

static void uart_update_irq(cc13x2_soc_t *soc) {
    if (soc->uart0.ris & soc->uart0.imsc)
        irq_raise(soc, CC13X2_IRQ_UART0);
}

/* Character time from the programmed divisor: UARTCLK (= the 48 MHz
 * peripheral clock) / (16 * (IBRD + FBRD/64)) baud, 10 bits per frame
 * for 8N1 (LCRH: WLEN [6:5], PEN, STP2 add bits). */
static int64_t uart_char_ns(cc13x2_uart_t *u) {
    double div = (double)u->ibrd + (double)u->fbrd / 64.0;
    if (div < 1.0) div = 26.0 + 3.0 / 64.0;          /* 115200 at 48 MHz */
    double baud = 48000000.0 / (16.0 * div);
    int bits = 1 + 5 + (int)((u->lcrh >> 5) & 3u) + ((u->lcrh & 2u) ? 1 : 0) +
               ((u->lcrh & 8u) ? 2 : 1);
    return (int64_t)((double)bits * 1e9 / baud);
}

static void uart_start_next(cc13x2_soc_t *soc);

static void uart_tx_done_cb(void *user, cpu_event_t *ev) {
    (void)ev;
    cc13x2_soc_t *soc = user;
    cc13x2_uart_t *u = &soc->uart0;
    u->tx_busy = false;
    u->tx_count--;
    int thr = uart_level(u->ifls & 7u);
    int queued = u->tx_count;
    if (queued <= thr) u->ris |= UART_INT_TX;
    if (queued == 0) u->ris |= UART_INT_EOT;
    uart_start_next(soc);
    uart_update_irq(soc);
}

static void uart_start_next(cc13x2_soc_t *soc) {
    cc13x2_uart_t *u = &soc->uart0;
    if (u->tx_busy || u->tx_count == 0) return;
    uint8_t b = u->tx_fifo[u->tx_head];
    u->tx_head = (u->tx_head + 1) % UART_FIFO;
    u->tx_busy = true;
    if (u->tx_cb) u->tx_cb(u->tx_user, b);
    u->tx_event.callback = uart_tx_done_cb;
    u->tx_event.user_data = soc;
    schedule_at(soc, &u->tx_event, live_ns(soc) + uart_char_ns(u));
}

static uint32_t uart_read(cc13x2_soc_t *soc, uint32_t off) {
    cc13x2_uart_t *u = &soc->uart0;
    uint32_t a = off & ~3u, v = 0;
    switch (a) {
    case UART_DR:
        if (u->rx_count > 0) {
            v = u->rx_fifo[u->rx_head];
            u->rx_head = (u->rx_head + 1) % UART_FIFO;
            u->rx_count--;
            if (u->rx_count < uart_level((u->ifls >> 3) & 7u)) u->ris &= ~UART_INT_RX;
            if (u->rx_count == 0) u->ris &= ~UART_INT_RT;
        }
        break;
    case UART_FR:
        if (u->tx_count == 0) v |= 0x80;               /* TXFE */
        if (u->rx_count == UART_FIFO) v |= 0x40;       /* RXFF */
        if (u->tx_count >= UART_FIFO + 1) v |= 0x20;   /* TXFF (FIFO + shifter) */
        if (u->rx_count == 0) v |= 0x10;               /* RXFE */
        if (u->tx_count > 0) v |= 0x08;                /* BUSY */
        break;
    case UART_IBRD:  v = u->ibrd; break;
    case UART_FBRD:  v = u->fbrd; break;
    case UART_LCRH:  v = u->lcrh; break;
    case UART_CTL:   v = u->ctl; break;
    case UART_IFLS:  v = u->ifls; break;
    case UART_IMSC:  v = u->imsc; break;
    case UART_RIS:   v = u->ris; break;
    case UART_MIS:   v = u->ris & u->imsc; break;
    case UART_DMACTL: v = u->dmactl; break;
    }
    return v >> ((off & 3u) * 8u);
}

static void uart_write(cc13x2_soc_t *soc, uint32_t off, uint32_t val) {
    cc13x2_uart_t *u = &soc->uart0;
    uint32_t a = off & ~3u;
    switch (a) {
    case UART_DR:
        if (u->tx_count < UART_FIFO + 1) {
            int slot = (u->tx_head + u->tx_count - (u->tx_busy ? 1 : 0)) % UART_FIFO;
            u->tx_fifo[slot] = (uint8_t)val;
            u->tx_count++;
            u->ris &= ~UART_INT_EOT;
            if (u->tx_count > uart_level(u->ifls & 7u)) u->ris &= ~UART_INT_TX;
            uart_start_next(soc);
        }
        break;
    case UART_RSR:   break;
    case UART_IBRD:  u->ibrd = val & 0xFFFFu; break;
    case UART_FBRD:  u->fbrd = val & 0x3Fu; break;
    case UART_LCRH:  u->lcrh = val & 0xFFu; break;
    case UART_CTL:   u->ctl = val & 0xFFFFu; break;
    case UART_IFLS:  u->ifls = val & 0x3Fu; break;
    case UART_IMSC:  u->imsc = val & 0xFF2u; break;
    case UART_ICR:   u->ris &= ~val; break;
    case UART_DMACTL: u->dmactl = val & 7u; break;
    }
    uart_update_irq(soc);
}

void cc13x2_uart_feed_rx(cc13x2_soc_t *soc, const uint8_t *buf, int len) {
    cc13x2_uart_t *u = &soc->uart0;
    for (int i = 0; i < len && u->rx_count < UART_FIFO; i++) {
        u->rx_fifo[(u->rx_head + u->rx_count) % UART_FIFO] = buf[i];
        u->rx_count++;
    }
    if (u->rx_count >= uart_level((u->ifls >> 3) & 7u)) u->ris |= UART_INT_RX;
    if (u->rx_count > 0) u->ris |= UART_INT_RT;
    uart_update_irq(soc);
}

/* ============================================================
 * Watchdog — inc/hw_wdt.h, driverlib/watchdog.h
 * ============================================================ */

#define WDT_LOAD   0x000
#define WDT_VALUE  0x004
#define WDT_CTL    0x008
#define WDT_ICR    0x00C
#define WDT_RIS    0x010
#define WDT_MIS    0x014
#define WDT_LOCK   0xC00
#define WDT_CTL_INTEN   0x1u
#define WDT_CTL_RESEN   0x2u
#define WDT_UNLOCK_KEY  0x1ACCE551u
#define WDT_HZ          1500000

static void wdt_arm(cc13x2_soc_t *soc);

static uint32_t wdt_value(cc13x2_soc_t *soc) {
    cc13x2_wdt_t *w = &soc->wdt;
    if (!(w->ctl & WDT_CTL_INTEN)) return w->load;
    int64_t el = live_ns(soc) - w->anchor_ns;
    if (el < 0) el = 0;
    uint64_t ticks = ((uint64_t)el * WDT_HZ) / 1000000000u;
    return ticks >= w->load ? 0 : w->load - (uint32_t)ticks;
}

/* Expiry: the first one latches RIS and interrupts; one with RIS still set
 * resets the chip if RESEN (TRM, "Watchdog Timer"). The counter reloads
 * from LOAD either way. */
static void wdt_cb(void *user, cpu_event_t *ev) {
    (void)ev;
    cc13x2_soc_t *soc = user;
    cc13x2_wdt_t *w = &soc->wdt;
    if (w->ris && (w->ctl & WDT_CTL_RESEN)) {
        cc13x2_page_t *p = find_page(soc, "AON_PMCTL");
        if (p) REG(p, AON_PMCTL_RESETCTL) = (REG(p, AON_PMCTL_RESETCTL) & ~0xEu) | 0xEu; /* WARMRESET */
        arm_request_reset(&soc->plat->cpu);
        return;
    }
    w->ris = true;
    w->anchor_ns = live_ns(soc);
    irq_raise(soc, CC13X2_IRQ_WDT);   /* CTL.INTTYPE NMI routing: not modelled */
    wdt_arm(soc);
}

static void wdt_arm(cc13x2_soc_t *soc) {
    cc13x2_wdt_t *w = &soc->wdt;
    arm_cancel_event(&soc->plat->cpu, &w->event);
    if (!(w->ctl & WDT_CTL_INTEN)) return;
    w->event.callback = wdt_cb;
    w->event.user_data = soc;
    int64_t period = (int64_t)(((uint64_t)w->load * 1000000000u + WDT_HZ - 1) / WDT_HZ);
    schedule_at(soc, &w->event, w->anchor_ns + period);
}

static uint32_t wdt_read(cc13x2_soc_t *soc, uint32_t off) {
    cc13x2_wdt_t *w = &soc->wdt;
    uint32_t v = 0;
    switch (off & ~3u) {
    case WDT_LOAD:  v = w->load; break;
    case WDT_VALUE: v = wdt_value(soc); break;
    case WDT_CTL:   v = w->ctl; break;
    case WDT_RIS:   v = w->ris ? 1u : 0u; break;
    case WDT_MIS:   v = (w->ris && (w->ctl & WDT_CTL_INTEN)) ? 1u : 0u; break;
    case WDT_LOCK:  v = w->locked ? 1u : 0u; break;
    }
    return v >> ((off & 3u) * 8u);
}

static void wdt_write(cc13x2_soc_t *soc, uint32_t off, uint32_t val) {
    cc13x2_wdt_t *w = &soc->wdt;
    uint32_t a = off & ~3u;
    if (a == WDT_LOCK) { w->locked = (val != WDT_UNLOCK_KEY); return; }
    if (w->locked) return;                     /* write-protected */
    switch (a) {
    case WDT_LOAD:                             /* writing LOAD reloads */
        w->load = val;
        w->anchor_ns = live_ns(soc);
        break;
    case WDT_CTL: {
        bool was = (w->ctl & WDT_CTL_INTEN) != 0;
        w->ctl = (val & 0x107u) | (w->ctl & WDT_CTL_INTEN);  /* INTEN sticks */
        if (!was && (w->ctl & WDT_CTL_INTEN)) w->anchor_ns = live_ns(soc);
        break;
    }
    case WDT_ICR:                              /* clears RIS and reloads */
        w->ris = false;
        w->anchor_ns = live_ns(soc);
        break;
    default:
        return;
    }
    wdt_arm(soc);
}

/* ============================================================
 * Small blocks
 * ============================================================ */

/* VIMS STAT mirrors CTL.MODE; mode changes complete at once (STAT bit3
 * MODE_CHANGING never reads 1). inc/hw_vims.h. */
static uint32_t vims_read(cc13x2_page_t *p, uint32_t off) {
    uint32_t a = off & ~3u;
    if (a == 0x0) return (REG(p, 0x4) & 3u) >> ((off & 3u) * 8u);
    return REG(p, a) >> ((off & 3u) * 8u);
}

/* FLASH controller: identification and an always-idle state machine.
 * FLASH_SIZE.SECTORS = 44 x 8 KB = 352 KB; FCFG_B0_SSIZE0 = 44 sectors of
 * 8 KB (B0_SECT_SIZE in KB). inc/hw_flash.h. */
#define FLASH_STAT          0x001C
#define FLASH_FLASH_SIZE    0x002C
#define FLASH_FSM_STAT      0x2208
#define FLASH_FCFG_B0_SSIZE0 0x2430
static uint32_t flash_read(cc13x2_page_t *p, uint32_t off) {
    uint32_t a = off & ~3u, v;
    switch (a) {
    case FLASH_FLASH_SIZE:     v = 44u; break;
    case FLASH_FCFG_B0_SSIZE0: v = (44u << 16) | 8u; break;
    case FLASH_STAT:           v = 0u; break;       /* not busy */
    case FLASH_FSM_STAT:       v = 0x4u; break;     /* INV_DAT idle */
    default:                   v = REG(p, a); break;
    }
    return v >> ((off & 3u) * 8u);
}

/* Semaphores (MCU SMPH and AUX_SMPH): reading SMPHn returns 1 and claims
 * it if it was free, 0 if taken; writing 1 releases. */
static uint32_t smph_read(cc13x2_page_t *p, uint32_t off, uint32_t nsem) {
    uint32_t a = off & ~3u, idx = a / 4;
    if (idx < nsem) {
        uint32_t free = (REG(p, a) == 0);
        REG(p, a) = 1;
        return free;
    }
    if (a >= 0x800 && (a - 0x800) / 4 < nsem)          /* PEEKn */
        return REG(p, a - 0x800) ? 0u : 1u;
    return REG(p, a);
}
static void smph_write(cc13x2_page_t *p, uint32_t off, uint32_t val, uint32_t nsem) {
    uint32_t a = off & ~3u;
    if (a / 4 < nsem) {
        if (val & 1u) REG(p, a) = 0;
        return;
    }
    REG(p, a) = val;
}

/* ============================================================
 * IO dispatch
 * ============================================================ */

/* Offset into the window. The non-buffered alias sits 0x2000_0000 above
 * the canonical base, which the mask folds away. */
static uint32_t page_off(cc13x2_page_t *p, uint32_t addr) {
    return (addr - p->base) & 0x0FFFFFFFu;
}

static void trace_access(cc13x2_page_t *p, uint32_t off, bool wr, uint32_t v) {
    if (!trace_on()) return;
    if (p->kind != P_STORE && cc13x2_trace < 2) return;
    if (p->kind == P_ROM || p->kind == P_GPRAM || p->kind == P_RFC_RAM) return;
    arm_cpu_t *cpu = &p->soc->plat->cpu;
    fprintf(stderr, "[cc13x2] %12.6f %c %-10s +0x%03x %s 0x%08x  pc=0x%08x\n",
            (double)live_ns(p->soc) / 1e9, wr ? 'W' : 'R', p->name, off,
            wr ? "<-" : "->", v, cpu->reg[ARM_PC]);
}

static int page_read_impl(void *user, uint32_t addr) {
    cc13x2_page_t *p = user;
    cc13x2_soc_t *soc = p->soc;
    uint32_t off = page_off(p, addr);
    uint32_t v;
    switch (p->kind) {
    case P_ROM:
        v = 0;
        for (int i = 0; i < 4; i++)
            if (off + (uint32_t)i < cc13x2_rom_image_size)
                v |= (uint32_t)cc13x2_rom_image[off + (uint32_t)i] << (8 * i);
        return (int)v;
    case P_GPRAM:
        memcpy(&v, &soc->gpram[off & ~3u], 4);
        return (int)(v >> ((off & 3u) * 8u));
    case P_RFC_RAM:
        if (off >= CC13X2_RFC_RAM_SIZE) return 0;
        memcpy(&v, &soc->rfc_ram[off & ~3u], 4);
        return (int)(v >> ((off & 3u) * 8u));
    case P_FCFG1:
        return (int)(soc->fcfg1[(off & ~3u) / 4] >> ((off & 3u) * 8u));
    case P_CCFG: {
        /* CCFG is the last 8 KB flash sector seen through this window:
         * CCFG_O_* run 0x1FA8..0x1FFF (inc/hw_ccfg.h), the CCFG struct the
         * image links at 0x57FA8. */
        arm_cpu_t *cpu = &soc->plat->cpu;
        return (int)(arm_read32(cpu, (CC13X2_FLASH_SIZE - CC13X2_CCFG_SIZE) + (off & ~3u))
                     >> ((off & 3u) * 8u));
    }
    case P_PRCM:      return (int)prcm_read(soc, p, off);
    case P_AON_PMCTL: return (int)aon_pmctl_read(soc, p, off);
    case P_AON_RTC:   return (int)aon_rtc_read(soc, off);
    case P_GPIO:      return (int)gpio_read(soc, off);
    case P_UART0:     return (int)uart_read(soc, off);
    case P_DDI0:      return (int)ddi0_read(soc, off);
    case P_ADI2:      return (int)adi_read(soc->adi2, off);
    case P_ADI3:      return (int)adi_read(soc->adi3, off);
    case P_ADI4:      return (int)adi_read(soc->adi4, off);
    case P_VIMS:      return (int)vims_read(p, off);
    case P_WDT:       return (int)wdt_read(soc, off);
    case P_FLASH:     return (int)flash_read(p, off);
    case P_SMPH:      return (int)smph_read(p, off, 32);
    case P_AUX_SMPH:  return (int)smph_read(p, off, 8);
    case P_AUX_SYSIF:
        /* Request/acknowledge pairs that complete at once (inc/hw_aux_sysif.h):
         * OPMODEACK (0x04) follows OPMODEREQ (0x00); RTCSUBSECINCCTL (0x84)
         * UPD_ACK [1] follows UPD_REQ [0] (driverlib/setup_rom.c
         * SetupSetAonRtcSubSecInc). The SUBSECINC value itself — the
         * RCOSC_LF trim of the RTC rate — is accepted and not applied:
         * csim's RTC counts at exactly 32768 Hz. */
        if ((off & ~3u) == 0x04) return (int)REG(p, 0x00);
        if ((off & ~3u) == 0x84) {
            uint32_t r = REG(p, 0x84) & 1u;
            return (int)((r | (r << 1)) >> ((off & 3u) * 8u));
        }
        break;
    case P_AON_IOC:
    case P_HAPI:
    case P_STORE:
        break;
    }
    return (int)(REG(p, off & ~3u) >> ((off & 3u) * 8u));
}

static int page_read(void *user, uint32_t addr) {
    cc13x2_page_t *p = user;
    int v = page_read_impl(user, addr);
    trace_access(p, page_off(p, addr), false, (uint32_t)v);
    return v;
}

static void page_write_impl(void *user, uint32_t addr, uint32_t val) {
    cc13x2_page_t *p = user;
    cc13x2_soc_t *soc = p->soc;
    uint32_t off = page_off(p, addr);
    uint32_t w = width_of(soc);
    switch (p->kind) {
    case P_ROM: case P_FCFG1: case P_CCFG:
        return;                                         /* read-only */
    case P_HAPI:      hapi_write(soc, off, val); return;
    case P_GPRAM:
        for (uint32_t i = 0; i < w && off + i < CC13X2_GPRAM_SIZE; i++)
            soc->gpram[off + i] = (uint8_t)(val >> (8 * i));
        return;
    case P_RFC_RAM:
        for (uint32_t i = 0; i < w && off + i < CC13X2_RFC_RAM_SIZE; i++)
            soc->rfc_ram[off + i] = (uint8_t)(val >> (8 * i));
        return;
    case P_PRCM:      prcm_write(soc, p, off, val); return;
    case P_AON_PMCTL: aon_pmctl_write(soc, p, off, val); return;
    case P_AON_RTC:   aon_rtc_write(soc, off, val); return;
    case P_GPIO:      gpio_write(soc, off, val); return;
    case P_UART0:     uart_write(soc, off, val); return;
    case P_DDI0:      ddi0_write(soc, off, val); return;
    case P_ADI2:      adi_write(soc, soc->adi2, off, val); return;
    case P_ADI3:      adi_write(soc, soc->adi3, off, val); return;
    case P_ADI4:      adi_write(soc, soc->adi4, off, val); return;
    case P_WDT:       wdt_write(soc, off, val); return;
    case P_SMPH:      smph_write(p, off, val, 32); return;
    case P_AUX_SMPH:  smph_write(p, off, val, 8); return;
    default: break;
    }
    uint32_t a = off & ~3u;
    REG(p, a) = merge_write(soc, REG(p, a), off, val);
}

static void page_write(void *user, uint32_t addr, uint32_t val) {
    cc13x2_page_t *p = user;
    trace_access(p, page_off(p, addr), true, val);
    page_write_impl(user, addr, val);
}

/* ============================================================
 * Lifecycle
 * ============================================================ */

/* Reset values the boot path depends on. */
static void soc_reset_state(cc13x2_soc_t *soc) {
    arm_cpu_t *cpu = &soc->plat->cpu;
    arm_cancel_event(cpu, &soc->osc.xosc_hf_event);
    arm_cancel_event(cpu, &soc->uart0.tx_event);
    for (int i = 0; i < 3; i++) arm_cancel_event(cpu, &soc->rtc.ch_event[i]);
    arm_cancel_event(cpu, &soc->wdt.event);

    for (int i = 0; i < soc->num_pages; i++) {
        cc13x2_page_t *p = &soc->pages[i];
        if (p->regs && p->kind != P_AON_PMCTL)
            memset(p->regs, 0, p->size);
    }
    memset(soc->ddi0, 0, sizeof(soc->ddi0));
    memset(soc->adi2, 0, sizeof(soc->adi2));
    memset(soc->adi3, 0, sizeof(soc->adi3));
    memset(soc->adi4, 0, sizeof(soc->adi4));
    memset(&soc->rtc, 0, sizeof(soc->rtc));
    memset(&soc->osc, 0, sizeof(soc->osc));
    memset(&soc->wdt, 0, sizeof(soc->wdt));
    soc->wdt.load = 0xFFFFFFFFu;               /* reset value; locked = false */
    arm_uart_tx_callback cb = soc->uart0.tx_cb;
    void *cbu = soc->uart0.tx_user;
    memset(&soc->uart0, 0, sizeof(soc->uart0));
    soc->uart0.tx_cb = cb;
    soc->uart0.tx_user = cbu;
    uint32_t din_ext = soc->gpio.din_ext, din_forced = soc->gpio.din_forced;
    memset(&soc->gpio, 0, sizeof(soc->gpio));
    soc->gpio.din_ext = din_ext;
    soc->gpio.din_forced = din_forced;

    /* Boot ROM leaves the MCU domains on; the RF core is off. */
    soc->pd0 = 0x6u;          /* SERIAL | PERIPH */
    soc->pd1 = 0xAu;          /* CPU | VIMS */

    /* SetupTrimDevice() takes the cold-reset path when AON_IOC:IOCLATCH.EN
     * and AON_PMCTL:SLEEPCTL.IO_PAD_SLEEP_DIS are both 1 (driverlib/setup.c). */
    cc13x2_page_t *p;
    if ((p = find_page(soc, "AON_IOC"))) REG(p, 0x0C) = 1u;
    if ((p = find_page(soc, "AON_PMCTL"))) {
        REG(p, AON_PMCTL_SLEEPCTL) = 1u;
    }
    /* VIMS: cache mode, as the boot ROM leaves it. */
    if ((p = find_page(soc, "VIMS"))) REG(p, 0x4) = 1u;
    /* PRCM.RFCMODEHWOPT: the RF modes this part may select (chipinfo.c
     * reads bits [3:1] as the protocol vector: BLE, 15.4, proprietary;
     * RF_open() refuses RF_MODE_AUTO (0) unless bit 0 is set).
     * TODO(hardware): read the LaunchPad's value. */
    if ((p = find_page(soc, "PRCM"))) REG(p, PRCM_RFCMODEHWOPT) = 0xFFu;
    /* SysTick, NVIC: arm_platform_reset_peripherals. */
}

static void cc13x2_soc_init(arm_platform_t *plat) {
    cc13x2_soc_t *soc = calloc(1, sizeof(*soc));
    plat->soc = soc;
    soc->plat = plat;
    arm_cpu_t *cpu = &plat->cpu;

    soc->num_pages = NUM_PAGE_DESCS;
    soc->pages = calloc((size_t)soc->num_pages, sizeof(cc13x2_page_t));
    for (int i = 0; i < soc->num_pages; i++) {
        const page_desc_t *d = &page_descs[i];
        cc13x2_page_t *p = &soc->pages[i];
        p->soc = soc;
        p->base = d->base;
        p->size = d->size;
        p->name = d->name;
        p->kind = d->kind;
        if (d->kind != P_ROM && d->kind != P_GPRAM && d->kind != P_RFC_RAM &&
            d->kind != P_FCFG1 && d->kind != P_CCFG)
            p->regs = calloc(d->size / 4, sizeof(uint32_t));
        arm_register_io(cpu, d->base, d->size, page_read, page_write, p);
        if (d->nonbuf)
            arm_register_io(cpu, d->base | 0x20000000u, d->size,
                            page_read, page_write, p);
    }

    /* Per-node identity default: a distinct but fixed address until the
     * harness seeds one. */
    soc->mac_15_4[0] = 0x00000001u;
    soc->mac_15_4[1] = 0x00124B00u;
    fcfg1_init(soc);

    plat->host.cpu = cpu;
    soc_reset_state(soc);
}

static void cc13x2_soc_reset(arm_platform_t *plat) {
    cc13x2_soc_t *soc = plat->soc;
    if (!soc) return;
    soc_reset_state(soc);
}

static void cc13x2_soc_destroy(arm_platform_t *plat) {
    cc13x2_soc_t *soc = plat->soc;
    if (!soc) return;
    for (int i = 0; i < soc->num_pages; i++) free(soc->pages[i].regs);
    free(soc->pages);
    free(soc);
    plat->soc = NULL;
}

static void cc13x2_soc_set_console(arm_platform_t *plat,
                                   arm_uart_tx_callback cb, void *user_data) {
    cc13x2_soc_t *soc = plat->soc;
    soc->uart0.tx_cb = cb;
    soc->uart0.tx_user = user_data;
}

const arm_soc_ops_t cc13x2_soc_ops = {
    .name        = "cc13x2",
    .init        = cc13x2_soc_init,
    .destroy     = cc13x2_soc_destroy,
    .reset       = cc13x2_soc_reset,
    .set_console = cc13x2_soc_set_console,
};

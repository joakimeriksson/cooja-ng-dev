/*
 * TI CC13x2 / CC26x2 SoC peripheral bundle (CC1352R1 LaunchPad).
 *
 * Cortex-M4F at 48 MHz, 352 KB flash at 0x0, 80 KB SRAM at 0x20000000,
 * and the chip's ROM at 0x10000000.  csim does not carry TI's ROM: the
 * stand-in mapped there is driverlib built from the SimpleLink SDK sources
 * behind the ROM API tables (tools/cc13x2-rom/), plus the HAPI table.
 *
 * Peripherals are modelled at the register level where firmware depends
 * on behaviour (power-domain and oscillator status that the boot path polls,
 * the AON RTC Contiki's clock and rtimer run on, GPIO, UART0), and as plain
 * read-back register stores elsewhere.  Analog blocks (DDI0 OSC, ADI2/3/4)
 * implement their set/clear/masked-write windows exactly, because driverlib
 * writes fields through them and reads them back.
 *
 * Plugs into `arm_platform_t` through `cc13x2_soc_ops`.
 */
#ifndef CC13X2_SOC_H
#define CC13X2_SOC_H

#include "arm_platform.h"

/* --- Memory map (TRM SWCU185, "Memory Map"; driverlib inc/hw_memmap.h) --- */
#define CC13X2_ROM_BASE         0x10000000u
#define CC13X2_HAPI_PORT        0x10040000u   /* csim-only, see tools/cc13x2-rom/hapi.c */
#define CC13X2_GPRAM_BASE       0x11000000u
#define CC13X2_GPRAM_SIZE       0x2000u
#define CC13X2_RFC_RAM_BASE     0x21000000u
#define CC13X2_RFC_RAM_SIZE     0x5000u       /* RFC_RAM 16 KB + RFC_ULLRAM 4 KB */
#define CC13X2_FCFG1_BASE       0x50001000u
#define CC13X2_FCFG1_SIZE       0x1000u
#define CC13X2_CCFG_BASE        0x50003000u
#define CC13X2_CCFG_SIZE        0x2000u       /* mirrors the last flash sector */
#define CC13X2_FLASH_SIZE       (352u * 1024u)

/* --- External interrupt numbers (vector - 16; inc/hw_ints.h) --- */
#define CC13X2_IRQ_AON_GPIO_EDGE  0
#define CC13X2_IRQ_I2C            1
#define CC13X2_IRQ_RFC_CPE_1      2
#define CC13X2_IRQ_AON_RTC_COMB   4
#define CC13X2_IRQ_UART0          5
#define CC13X2_IRQ_RFC_CPE_0      9
#define CC13X2_IRQ_RFC_HW_COMB    10
#define CC13X2_IRQ_RFC_CMD_ACK    11
#define CC13X2_IRQ_WDT            14
#define CC13X2_IRQ_SWEV0          27
#define CC13X2_IRQ_AUX_COMB       28
#define CC13X2_IRQ_TRNG           33
#define CC13X2_IRQ_OSC_COMB       34
#define CC13X2_IRQ_COUNT          38

#define CC13X2_NUM_DIO          31

struct cc13x2_soc;

/* One 4 KB-or-smaller peripheral window with a plain register store behind
 * it; `kind` picks the behaviour layered on top (CC13X2_P_* in the .c). */
typedef struct cc13x2_page {
    struct cc13x2_soc *soc;
    uint32_t    base;          /* canonical 0x4xxx_xxxx / 0x5xxx_xxxx base */
    uint32_t    size;
    const char *name;
    int         kind;
    uint32_t   *regs;          /* size / 4 words */
} cc13x2_page_t;

/* AON RTC.  The 64-bit counter SEC:SUBSEC advances 2^17 (1/32768 s) per
 * SCLK_LF edge; it is computed from the live clock, not stored. */
typedef struct cc13x2_rtc {
    bool        enabled;
    int64_t     zero_ns;       /* live ns at which the counter read 0 */
    uint32_t    ctl;
    uint32_t    evflags;
    uint32_t    chctl;
    uint32_t    cmp[3];
    uint32_t    ch2cmpinc;
    uint32_t    ch1capt;
    uint32_t    subsec_latch;  /* SUBSEC as of the last SEC read */
    cpu_event_t ch_event[3];   /* per-channel compare crossing */
} cc13x2_rtc_t;

/* UART0: TI's PL011-derived UART with a 32-byte TX FIFO drained at the
 * programmed baud rate, and the CC13x2 end-of-transmission interrupt. */
typedef struct cc13x2_uart {
    uint32_t    ibrd, fbrd, lcrh, ctl, ifls, imsc, ris, dmactl;
    int         tx_count;                  /* bytes in the TX FIFO + shifter */
    uint8_t     tx_fifo[32];
    int         tx_head;
    bool        tx_busy;                   /* a byte is in the shifter */
    cpu_event_t tx_event;                  /* shifter done */
    uint8_t     rx_fifo[32];
    int         rx_head, rx_count;
    arm_uart_tx_callback tx_cb;
    void       *tx_user;
} cc13x2_uart_t;

/* GPIO: one 32-bit port of DIOs (0..30). */
typedef struct cc13x2_gpio {
    uint32_t    dout;
    uint32_t    doe;
    uint32_t    din_ext;       /* level driven from outside (buttons) */
    uint32_t    din_forced;    /* pins whose level din_ext overrides */
    uint32_t    evflags;
} cc13x2_gpio_t;

/* Watchdog: a down-counter at 1.5 MHz (48 MHz / 32, WATCHDOG_DIV_RATIO in
 * TI's WatchdogCC26XX driver) computed from the live clock. */
typedef struct cc13x2_wdt {
    uint32_t    load;
    uint32_t    ctl;
    bool        ris;
    bool        locked;
    int64_t     anchor_ns;     /* live ns at which VALUE read LOAD */
    cpu_event_t event;         /* next expiry */
} cc13x2_wdt_t;

/* Oscillator state behind DDI0 OSC: which source drives SCLK_HF / LF. */
typedef struct cc13x2_osc {
    int         hf_src;        /* 0 = RCOSC_HF, 1 = XOSC_HF */
    int         lf_src;        /* 0 RCOSC_HF/1536, 1 XOSC_HF/1536, 2 RCOSC_LF, 3 XOSC_LF */
    bool        xosc_hf_ready;
    cpu_event_t xosc_hf_event;
    bool        xosc_hf_starting;
} cc13x2_osc_t;

typedef struct cc13x2_soc {
    arm_platform_t *plat;

    /* Memories the CPU core does not map itself. */
    uint8_t         gpram[CC13X2_GPRAM_SIZE];
    uint8_t         rfc_ram[CC13X2_RFC_RAM_SIZE];
    uint32_t        fcfg1[CC13X2_FCFG1_SIZE / 4];

    /* Register windows; see the page table in cc13x2_soc.c. */
    cc13x2_page_t  *pages;
    int             num_pages;

    /* Analog register files (DDI0 OSC: 32-bit registers; ADI: 8-bit). */
    uint32_t        ddi0[0x20];
    uint8_t         adi2[16], adi3[16], adi4[16];

    cc13x2_rtc_t    rtc;
    cc13x2_uart_t   uart0;
    cc13x2_gpio_t   gpio;
    cc13x2_osc_t    osc;
    cc13x2_wdt_t    wdt;

    /* PRCM power-domain and clock-load state. */
    uint32_t        pd0;       /* PDCTL0 bits: RFC|SERIAL|PERIPH */
    uint32_t        pd1;       /* PDCTL1 bits: CPU|RFC|VIMS */

    /* Per-node identity the harness seeds before reset (FCFG1 MAC). */
    uint32_t        mac_15_4[2];
} cc13x2_soc_t;

extern const arm_soc_ops_t cc13x2_soc_ops;

static inline cc13x2_soc_t *arm_platform_cc13x2(arm_platform_t *plat) {
    return (plat && plat->config && plat->config->soc_ops == &cc13x2_soc_ops)
           ? (cc13x2_soc_t *)plat->soc : NULL;
}

/* Console input into UART0's RX FIFO. */
void cc13x2_uart_feed_rx(cc13x2_soc_t *soc, const uint8_t *buf, int len);

/* Seed the IEEE 802.15.4 address the factory config reports (FCFG1
 * MAC_15_4_0/1). Contiki builds its EUI-64 from it unless CCFG overrides. */
void cc13x2_soc_set_ieee_addr(cc13x2_soc_t *soc, const uint8_t addr[8]);

#endif /* CC13X2_SOC_H */

/*
 * CC13x2 hardware check: measurements that compare the csim model with a
 * LaunchPad, taken on the chip so that USB-serial latency does not enter.
 * The same image runs on the board and in csim; tools/cc13x2-hwcheck/
 * compare.py lines the two outputs up.
 *
 *  rtc-lag   When the AON RTC channel 2 event flag becomes visible relative
 *            to the compare value.  Channel 2 is free under Contiki (TI's
 *            ClockP uses channel 0, rtimer channel 1) and is not routed to
 *            the interrupt, so it is armed and polled directly.
 *  rtimer    The same through the interrupt path: rtimer_set() on channel 1
 *            and the RTC time read in the callback.
 *  rtc-past  A compare value in the past: enabling the channel with it, and
 *            writing it while the channel is enabled.  Does the event fire,
 *            and how late?
 *  radio     NETSTACK_RADIO on/off/channel/CCA/transmit, timed with the RTC
 *            and the DWT cycle counter (the counter stops while the core
 *            sleeps, so a gap between the two means the driver slept).
 *
 * Times are in RTC units of 1/65536 s (the 16.16 TIME register; it counts
 * in steps of 2 at 32768 Hz) unless a field says us or cyc.
 */
#include "contiki.h"
#include "net/netstack.h"
#include "dev/radio.h"
#include "dev/watchdog.h"
#include "sys/rtimer.h"
#include "sys/int-master.h"

#include <ti/devices/DeviceFamily.h>
#include DeviceFamily_constructPath(inc/hw_types.h)
#include DeviceFamily_constructPath(inc/hw_memmap.h)
#include DeviceFamily_constructPath(inc/hw_aon_rtc.h)
#include DeviceFamily_constructPath(driverlib/cpu.h)

#include <stdio.h>
#include <string.h>

#define RTC(o)       HWREG(AON_RTC_BASE + (o))
#define DEMCR        HWREG(0xE000EDFCu)
#define DWT_CTRL     HWREG(0xE0001000u)
#define DWT_CYCCNT   HWREG(0xE0001004u)

static uint32_t now1616(void) { return RTC(AON_RTC_O_TIME); }
static void rtc_sync(void) { (void)RTC(AON_RTC_O_SYNC); }

static void ch2_disable(void) {
  RTC(AON_RTC_O_CHCTL) &= ~AON_RTC_CHCTL_CH2_EN;
  rtc_sync();
}
static void ch2_enable(void) {
  RTC(AON_RTC_O_CHCTL) |= AON_RTC_CHCTL_CH2_EN;
  rtc_sync();
}
static void ch2_clear(void) {
  RTC(AON_RTC_O_EVFLAGS) = AON_RTC_EVFLAGS_CH2;
  rtc_sync();
}
static bool ch2_flag(void) {
  return (RTC(AON_RTC_O_EVFLAGS) & AON_RTC_EVFLAGS_CH2) != 0;
}

/*
 * Poll for the channel 2 flag until the RTC passes 'deadline'.  On return
 * *t0 is the last TIME read before the flag was seen and *t1 the first one
 * after it: the event became visible between the two.
 */
static bool ch2_wait(uint32_t deadline, uint32_t *t0, uint32_t *t1) {
  uint32_t prev = now1616();
  for(;;) {
    bool f = ch2_flag();
    uint32_t t = now1616();
    if(f) {
      *t0 = prev;
      *t1 = t;
      return true;
    }
    if((int32_t)(t - deadline) > 0) {
      *t0 = *t1 = t;
      return false;
    }
    prev = t;
  }
}

#define HIST_MIN  (-4)
#define HIST_MAX  12
#define HIST_N    (HIST_MAX - HIST_MIN + 1)

static void hist_add(uint16_t *h, int32_t d) {
  if(d < HIST_MIN) d = HIST_MIN;
  if(d > HIST_MAX) d = HIST_MAX;
  h[d - HIST_MIN]++;
}

static void hist_print(const char *name, const uint16_t *h) {
  printf("HWC %s", name);
  for(int i = 0; i < HIST_N; i++) {
    if(h[i]) printf(" %d:%u", i + HIST_MIN, h[i]);
  }
  printf("\n");
}

/* ------------------------------------------------------------------ */
#define LAG_N 64

static void test_rtc_lag(void) {
  static uint16_t h0[2][HIST_N], h1[2][HIST_N];
  int missed = 0;
  memset(h0, 0, sizeof(h0));
  memset(h1, 0, sizeof(h1));
  for(int i = 0; i < LAG_N; i++) {
    uint32_t t0, t1;
    int odd = i & 1;
    int_master_status_t irq = int_master_read_and_disable();
    ch2_disable();
    ch2_clear();
    uint32_t cmp = now1616() + 2 * (8 + (i * 7) % 23) + odd;
    RTC(AON_RTC_O_CH2CMP) = cmp;
    ch2_enable();
    bool ok = ch2_wait(cmp + 0x200, &t0, &t1);
    int_master_status_set(irq);
    if(!ok) {
      missed++;
      continue;
    }
    hist_add(h0[odd], (int32_t)(t0 - cmp));
    hist_add(h1[odd], (int32_t)(t1 - cmp));
    watchdog_periodic();
  }
  ch2_disable();
  ch2_clear();
  hist_print("rtc-lag even before", h0[0]);
  hist_print("rtc-lag even after ", h1[0]);
  hist_print("rtc-lag odd  before", h0[1]);
  hist_print("rtc-lag odd  after ", h1[1]);
  printf("HWC rtc-lag missed %d of %d\n", missed, LAG_N);
}

/* ------------------------------------------------------------------ */
static struct rtimer rt;
static volatile uint32_t rt_seen;
static volatile bool rt_fired;
static rtimer_clock_t rt_target;

static void rt_cb(struct rtimer *t, void *ptr) {
  rt_seen = now1616();
  rt_fired = true;
}

static void test_rtimer(void) {
  static uint16_t h[2][HIST_N];
  int missed = 0;
  memset(h, 0, sizeof(h));
  for(int i = 0; i < LAG_N; i++) {
    int odd = i & 1;
    rt_fired = false;
    rt_target = RTIMER_NOW() + 2 * (8 + (i * 7) % 23) + odd;
    rtimer_set(&rt, rt_target, 0, rt_cb, NULL);
    uint32_t deadline = rt_target + 0x400;
    while(!rt_fired && (int32_t)(now1616() - deadline) < 0);
    if(!rt_fired) {
      missed++;
      continue;
    }
    hist_add(h[odd], (int32_t)(rt_seen - rt_target));
    watchdog_periodic();
  }
  hist_print("rtimer even", h[0]);
  hist_print("rtimer odd ", h[1]);
  printf("HWC rtimer missed %d of %d\n", missed, LAG_N);
}

/* ------------------------------------------------------------------ */
static const uint32_t past_off[] = { 2, 64, 0x4000, 0xE000, 0x18000 };
#define PAST_N (sizeof(past_off) / sizeof(past_off[0]))

static void test_rtc_past(void) {
  for(unsigned k = 0; k < PAST_N; k++) {
    uint32_t t0, t1, start, cmp;
    bool fired_a, fired_b, early;
    int32_t after_a, after_b;
    int_master_status_t irq = int_master_read_and_disable();

    /* A: enable the channel with a compare already in the past. */
    ch2_disable();
    ch2_clear();
    start = now1616();
    cmp = start - past_off[k];
    RTC(AON_RTC_O_CH2CMP) = cmp;
    ch2_enable();
    fired_a = ch2_wait(start + 0x200, &t0, &t1);
    after_a = (int32_t)(t1 - start);

    /* B: the channel is enabled with a future compare; then the compare
     * is rewritten into the past. */
    ch2_disable();
    ch2_clear();
    RTC(AON_RTC_O_CH2CMP) = now1616() + 0x100000;
    ch2_enable();
    start = now1616();
    while((int32_t)(now1616() - start) < 8);
    early = ch2_flag();
    ch2_clear();
    start = now1616();
    cmp = start - past_off[k];
    RTC(AON_RTC_O_CH2CMP) = cmp;
    rtc_sync();
    fired_b = ch2_wait(start + 0x200, &t0, &t1);
    after_b = (int32_t)(t1 - start);
    ch2_disable();
    ch2_clear();
    int_master_status_set(irq);

    /* Print with interrupts on: the UART driver waits for its TX IRQ. */
    printf("HWC rtc-past enable off=%lu fired=%d after=%ld\n",
           (unsigned long)past_off[k], fired_a, (long)after_a);
    printf("HWC rtc-past write  off=%lu fired=%d after=%ld early=%d\n",
           (unsigned long)past_off[k], fired_b, (long)after_b, early);
    watchdog_periodic();
  }
}

/* ------------------------------------------------------------------ */
#define RADIO_N 8

typedef struct {
  uint32_t rtc_sum, rtc_min, rtc_max;
  uint32_t cyc_sum;
  uint32_t last;
  int n;
} stat_t;

/* RTC units of the most recent channel and tx calls, for the iter line. */
static uint32_t last_rtc[5];

static void stat_add(stat_t *s, uint32_t rtc, uint32_t cyc) {
  if(s->n == 0 || rtc < s->rtc_min) s->rtc_min = rtc;
  if(s->n == 0 || rtc > s->rtc_max) s->rtc_max = rtc;
  s->rtc_sum += rtc;
  s->last = rtc;
  s->cyc_sum += cyc;
  s->n++;
}

static uint32_t rtc_us(uint32_t u) { return (u * 15625u + 512u) / 1024u; }

static void stat_print(const char *name, const stat_t *s) {
  if(!s->n) return;
  /* Mean in us from the RTC (x15.26 us/unit), min/max in RTC units, and
   * the mean of the cycle counter at 48 MHz. */
  printf("HWC radio %-10s n=%d rtc_mean_us=%lu rtc_min=%lu rtc_max=%lu cyc_mean_us=%lu\n",
         name, s->n, (unsigned long)rtc_us(s->rtc_sum / s->n),
         (unsigned long)s->rtc_min, (unsigned long)s->rtc_max,
         (unsigned long)(s->cyc_sum / s->n / 48));
}

#define TIMED(stat, expr) do {                  \
    uint32_t r0_ = now1616(), c0_ = DWT_CYCCNT; \
    expr;                                       \
    uint32_t c1_ = DWT_CYCCNT, r1_ = now1616(); \
    stat_add(&(stat), r1_ - r0_, c1_ - c0_);    \
  } while(0)

#if RF_MODE == RF_MODE_2_4_GHZ
/* Contiki's IEEE command structures (rf-settings/cc13x2/ieee-settings.c);
 * the status word is at offset 2 of every radio operation. */
extern uint8_t rf_cmd_ieee_rx[], rf_cmd_ieee_tx[], rf_cmd_ieee_fs[];
#define OP_STATUS(op) (*(volatile uint16_t *)((op) + 2))
#endif

static uint8_t txbuf[127];
static const uint8_t tx_lens[] = { 12, 24, 48, 96 };
#define TX_LENS (sizeof(tx_lens) / sizeof(tx_lens[0]))

static void test_radio(void) {
  static stat_t s_on, s_off, s_chan, s_cca, s_cca2, s_tx[TX_LENS];
  radio_value_t ch0 = 0, rssi;
  int cca_clear = 0, cca2_clear = 0, ret_tx = 0;
  int rssi_min = 127, rssi_max = -128;
  memset(&s_on, 0, sizeof(s_on));
  memset(&s_off, 0, sizeof(s_off));
  memset(&s_chan, 0, sizeof(s_chan));
  memset(&s_cca, 0, sizeof(s_cca));
  memset(&s_cca2, 0, sizeof(s_cca2));
  memset(s_tx, 0, sizeof(s_tx));

  radio_value_t ch_max = 0, ch_alt;
  NETSTACK_RADIO.get_value(RADIO_PARAM_CHANNEL, &ch0);
  NETSTACK_RADIO.get_value(RADIO_CONST_CHANNEL_MAX, &ch_max);
  ch_alt = ch0 < ch_max ? ch0 + 1 : ch0 - 1;
  /* A broadcast data frame with no ACK request: 802.15.4 FCF 0x8841,
   * PAN 0xabcd, dst 0xffff, short src. */
  static const uint8_t hdr[] = { 0x41, 0x88, 0x00, 0xcd, 0xab, 0xff, 0xff,
                                 0x01, 0x00 };
  memcpy(txbuf, hdr, sizeof(hdr));
  for(unsigned i = sizeof(hdr); i < sizeof(txbuf); i++) txbuf[i] = i;

  for(int i = 0; i < RADIO_N; i++) {
    TIMED(s_off, NETSTACK_RADIO.off());
    TIMED(s_on, NETSTACK_RADIO.on());
    TIMED(s_chan, NETSTACK_RADIO.set_value(RADIO_PARAM_CHANNEL,
                                           (i & 1) ? ch_alt : ch0));
    last_rtc[0] = s_chan.last;
    /* CCA straight after the retune, and again 2 ms later. */
    int c, c1;
#if RF_MODE == RF_MODE_2_4_GHZ
    uint16_t st_rx = OP_STATUS(rf_cmd_ieee_rx), st_fs = OP_STATUS(rf_cmd_ieee_fs);
#endif
    TIMED(s_cca, c1 = NETSTACK_RADIO.channel_clear());
    cca_clear += c1;
    clock_delay_usec(2000);
#if RF_MODE == RF_MODE_2_4_GHZ
    uint16_t st_rx2 = OP_STATUS(rf_cmd_ieee_rx);
#endif
    TIMED(s_cca2, c = NETSTACK_RADIO.channel_clear());
    cca2_clear += c;
    if(NETSTACK_RADIO.get_value(RADIO_PARAM_RSSI, &rssi) == RADIO_RESULT_OK) {
      if(rssi < rssi_min) rssi_min = rssi;
      if(rssi > rssi_max) rssi_max = rssi;
    }
    for(unsigned k = 0; k < TX_LENS; k++) {
      txbuf[2] = i;
      NETSTACK_RADIO.prepare(txbuf, tx_lens[k]);
      TIMED(s_tx[k], ret_tx |= NETSTACK_RADIO.transmit(tx_lens[k]));
      last_rtc[1 + k] = s_tx[k].last;
    }
#if RF_MODE == RF_MODE_2_4_GHZ
    printf("HWC status %d rx=0x%04x rx+2ms=0x%04x fs=0x%04x tx=0x%04x\n", i,
           st_rx, st_rx2, st_fs, OP_STATUS(rf_cmd_ieee_tx));
#endif
    /* Per iteration: channel, the two CCA results, RTC units per call. */
    printf("HWC iter %d ch=%d cca=%d,%d chan=%lu tx=%lu,%lu,%lu,%lu\n", i,
           (int)((i & 1) ? ch_alt : ch0), c1, c,
           (unsigned long)last_rtc[0], (unsigned long)last_rtc[1],
           (unsigned long)last_rtc[2], (unsigned long)last_rtc[3],
           (unsigned long)last_rtc[4]);
    watchdog_periodic();
  }
  NETSTACK_RADIO.set_value(RADIO_PARAM_CHANNEL, ch0);
  stat_print("off", &s_off);
  stat_print("on", &s_on);
  stat_print("channel", &s_chan);
  stat_print("cca", &s_cca);
  stat_print("cca+2ms", &s_cca2);
  for(unsigned k = 0; k < TX_LENS; k++) {
    char name[16];
    snprintf(name, sizeof(name), "tx%u", tx_lens[k]);
    stat_print(name, &s_tx[k]);
  }
  printf("HWC radio cca clear=%d/%d cca+2ms clear=%d/%d rssi=%d..%d tx-ret-or=%d\n",
         cca_clear, RADIO_N, cca2_clear, RADIO_N, rssi_min, rssi_max, ret_tx);
}

/* ------------------------------------------------------------------ */
PROCESS(hwcheck_process, "hwcheck");
AUTOSTART_PROCESSES(&hwcheck_process);

PROCESS_THREAD(hwcheck_process, ev, data)
{
  static struct etimer et;
  static int round;
  PROCESS_BEGIN();

  DEMCR |= (1u << 24);   /* TRCENA */
  DWT_CYCCNT = 0;
  DWT_CTRL |= 1u;

  etimer_set(&et, 2 * CLOCK_SECOND);
  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&et));
    printf("HWC round %d\n", ++round);
    test_rtc_lag();
    test_rtimer();
    test_rtc_past();
    test_radio();
    printf("HWC done %d\n", round);
    etimer_set(&et, 5 * CLOCK_SECOND);
  }

  PROCESS_END();
}

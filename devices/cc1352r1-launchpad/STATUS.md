# CC1352R1 LaunchPad Port — Status & Direction

> For the device contract see [`SPEC.md`](SPEC.md).

## Current state — prop mode through L6; IEEE mode next

Contiki-NG runs on the emulated CC1352R1 with its stock radio stack: the
sub-GHz proprietary mode Contiki uses by default on this board (802.15.4g,
50 kbps 2-GFSK, 868.325 MHz).

| Level | Evidence |
|-------|----------|
| L0 ELF loads | the 352 KB flash image, CCFG in the last sector |
| L1 reset → `main()` | ~15 000 cycles, through `SetupTrimDevice()` in the stand-in ROM |
| L2 console | banner on UART0 at the programmed baud rate |
| L3 LEDs | `leds-nullradio` sequence on DIO6/DIO7 (`CSIM_GPIO_TRACE=1`) |
| L4 Contiki timers | `configs/test-boot-cc1352r1-launchpad.yaml` |
| L5 radio | `configs/test-nullnet-cc1352r1-launchpad.yaml` — both nodes hear every broadcast |
| L6 full stack | `configs/test-rpl-udp-cc1352r1-launchpad.yaml` — RPL DAG + UDP round trips, ~790x real time |

How the radio is modelled: `src/arm/cc13x2_rfc.c` is a command-level model
of the RF core (the CPE Cortex-M0 and its modem behind the doorbell), not an
instruction-level one — TI does not publish the CPE ROM, and the modem
hardware it drives is undocumented. The documented host contract is what is
modelled: CMDR/CMDSTA/RFACKIFG, radio operations with status words, pNextOp
chaining and the CPE interrupt flags, the RAT locked to the AON RTC, and the
data-entry queues in RAM. The CPE patches Contiki loads are accepted into RF
core RAM and not executed. On air, prop mode is preamble + 24-bit sync
`0x55904E` + 802.15.4g PHR + payload + CRC; the bus and medium framers know
that layout (`RADIO_FRAME_TI_PROP_SYNC_WORD`).

Pieces the radio needed beyond the RF core: the PRCM oscillator interrupt
(TI's Power driver commits the XOSC_HF switch from INT_OSC_COMB), and the AUX
TDC that the same driver calibrates the RC oscillators with.

## Plan

1. **RF core, IEEE mode (2.4 GHz).** CMD_RADIO_SETUP, CMD_IEEE_RX/TX/RX_ACK,
   frame filtering, auto-ACK, CCA_REQ, MOD_FILT — interoperating with Sky,
   CC2538 and nRF nodes on one DAG. On this chip Contiki's IEEE mode tunes
   CMD_FS from the sub-GHz table and retunes through CMD_IEEE_RX.channel
   unless the 2450 band is configured.
2. **Sub-GHz capture.** pcap and the packet analyzer decode 2.4 GHz frames
   only (CC1200 frames are not captured either).

## Hardware checks (LaunchPad available)

To run when the board is connected (`board/ti_cc13x2_launchpad.cfg` in
openocd 0.12 talks to the on-board XDS110):

- **FCFG1 dump** — replace the synthesized FCFG1 (identity fields, TFW_FT,
  IOCONF; the trims are zero today):
  `openocd -f board/ti_cc13x2_launchpad.cfg -c "init; halt; dump_image fcfg1.bin 0x50001000 0x430; mdw 0x400821d4; exit"`
  (the last word is PRCM.RFCMODEHWOPT, also guessed).
- **RTC compare lag** — whether a channel event lands on the matching
  SCLK_LF edge or the one after (csim models the latter, which TI's TimerP
  arithmetic requires).
- **Boot log timing** — compare hello-world's console against the board.

## Deliberate approximations

- The RTC runs at exactly 32 768 Hz; the SUBSECINC (RCOSC_LF) trim is
  accepted and not applied.
- Power domains and clock-gate loads switch at once; XOSC_HF takes 300 µs.
  Standby/idle wake-up latency is not modelled.
- RF core latencies (boot 40 µs, setup 150 µs, FS 140 µs, RAT sync 31 µs,
  TX ramp 100 µs) are placeholders of the right order, not measurements.
  The RX timestamp sits 690 µs before the end of the sync word, which is
  what Contiki's prop-mode arithmetic assumes. CRCs are computed and
  checked, the on-air bit order of the CRC is not modelled.
- RSSI is the medium's per-frame value; CMD_GET_RSSI reports -110 dBm on an
  idle channel and -60 dBm while a neighbour transmits.
- The watchdog interrupt is always the maskable IRQ (CTL.INTTYPE's NMI
  routing is not modelled); it does not pause in standby.
- SSI, GPT, uDMA, TRNG, crypto and the AUX ADC are plain register stores; the
  AUX TDC measures exact oscillators, so the RC calibration trims nothing.

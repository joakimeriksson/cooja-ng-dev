# CC1352R1 LaunchPad Port — Status & Direction

> For the device contract see [`SPEC.md`](SPEC.md).

## Current state — L0–L4 done, radio next

Contiki-NG boots on the emulated CC1352R1 and idles correctly:
`configs/test-boot-cc1352r1-launchpad.yaml` prints the full banner with
the per-node link-layer address and `Hello, world` every 10.000 s, at
~1300–2300× real time (the CPU sleeps between events), deterministically.
The LED example drives DIO6/DIO7 in the expected sequence.

| Level | Evidence |
|-------|----------|
| L0 ELF loads | the 352 KB flash image, CCFG in the last sector |
| L1 reset → `main()` | 15 313 cycles, through `SetupTrimDevice()` in the stand-in ROM |
| L2 console | banner on UART0 at the programmed baud rate |
| L3 LEDs | `leds-nullradio` sequence on DIO6/DIO7 (`CSIM_GPIO_TRACE=1`) |
| L4 Contiki timers | etimers on TI ClockP / AON RTC, rtimer on RTC channel 1 |

The radio firmware is gated on the RF core: Contiki's `netstack_init()`
opens it before the banner, so stock hello-world waits for the RF core's
BOOT_DONE. The bring-up images use the null radio to keep L0–L4 independent
of it.

## Plan

1. **RF core, prop mode (sub-GHz, Contiki's default for this board).** A
   command-level model of the RF core behind the doorbell: power-up and
   BOOT_DONE, CMDR/CMDSTA/RFACKIFG, the radio-operation engine (status,
   chaining, COMMAND_DONE/LAST_COMMAND_DONE), CMD_PROP_RADIO_DIV_SETUP,
   CMD_FS, CMD_SYNC_START/STOP_RAT with an RTC-locked RAT,
   CMD_PROP_TX_ADV/RX_ADV with the RX data queue, CMD_GET_RSSI. Frames on
   the medium as 802.15.4g (PHR + payload + CRC-32), so the Firefly's
   CC1200 is a potential peer. Then L5 (nullnet-broadcast) and L6 (RPL-UDP).
2. **RF core, IEEE mode (2.4 GHz).** CMD_RADIO_SETUP, CMD_IEEE_RX/TX/RX_ACK,
   frame filtering, auto-ACK, CCA_REQ, MOD_FILT — interoperating with Sky,
   CC2538 and nRF nodes on one DAG.

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
- The watchdog interrupt is always the maskable IRQ (CTL.INTTYPE's NMI
  routing is not modelled); it does not pause in standby.
- SSI, GPT, uDMA, TRNG, crypto, AUX TDC/ADC are plain register stores.

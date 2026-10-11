# CC1352R1 LaunchPad Port — Status & Direction

> For the device contract see [`SPEC.md`](SPEC.md).

## Current state — both radio modes through L6

Contiki-NG runs on the emulated CC1352R1 with its stock radio stack in both
modes: the sub-GHz proprietary mode Contiki uses by default on this board
(802.15.4g, 50 kbps 2-GFSK, 868.325 MHz) and IEEE 802.15.4 at 2.4 GHz
(`RF_CONF_MODE=RF_MODE_2_4_GHZ`), where it interoperates with the other
2.4 GHz boards.

| Level | Evidence |
|-------|----------|
| L0 ELF loads | the 352 KB flash image, CCFG in the last sector |
| L1 reset → `main()` | ~15 000 cycles, through `SetupTrimDevice()` in the stand-in ROM |
| L2 console | banner on UART0 at the programmed baud rate |
| L3 LEDs | `leds-nullradio` sequence on DIO6/DIO7 (`CSIM_GPIO_TRACE=1`) |
| L4 Contiki timers | `configs/test-boot-cc1352r1-launchpad.yaml` |
| L5 radio, prop | `configs/test-nullnet-cc1352r1-launchpad.yaml` |
| L6 full stack, prop | `configs/test-rpl-udp-cc1352r1-launchpad.yaml` — ~790x real time |
| L5 radio, IEEE | `configs/test-nullnet-ieee-cc1352r1-launchpad.yaml` |
| L6 full stack, IEEE | `configs/test-rpl-udp-ieee-cc1352r1-launchpad.yaml` — ~900x real time |
| Interop, IEEE | `configs/test-mixed-platform-cc1352r1.yaml` — joins a Sky-rooted DAG with a CC2538 and an nRF52840 |
| TSCH, prop | `configs/test-tsch-cc1352r1-launchpad.yaml` — association + ≥3 drift reports (40 ms slots) |
| TSCH, IEEE | `configs/test-tsch-ieee-cc1352r1-launchpad.yaml` — association + ≥3 drift reports |

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

IEEE mode details worth knowing: on CC13xx parts Contiki tunes CMD_FS from
the sub-GHz table (868.325 MHz) and the radio reaches 2480 MHz only because
CMD_IEEE_RX's channel field retunes it, as on silicon. CMD_IEEE_RX_ACK
watches the background RX for its ACK and that RX also stores the ACK —
Contiki's CSMA (no CSMA_USE_RADIO_ACK) reads it from the queue; only
CMD_IEEE_TX suspends the background RX.

TSCH runs in both modes and holds sync (drift settles at 0-5 ppm). What it
needed: the 802.15.4g frame tracker clearing its per-frame channel snapshot
when a new preamble starts (a hopping sender's preamble and sync word were
filtered against the previous frame's channel); the AON RTC compare firing
"as soon as possible" when armed up to 1 s in the past, as hw_aon_rtc.h
documents, and CHCTL writes leaving channels whose enable does not change
alone (one cancelled TI ClockP's due event, the node slept until the
watchdog reset it); and the watchdog pausing in standby. Per-slot timing
errors are ±8-12 rtimer ticks (~±150 µs), inside the guard time but larger
than exact oscillators explain — the inferred RF core latencies and RTC
lag are the suspects.

## Next

1. **Hardware comparison** (below): FCFG1 dump, RTC compare lag, RF core
   latencies, boot log timing, and TSCH slot timing (`edr`) against a pair
   of boards.
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
  arithmetic requires), and whether enabling a channel with a just-passed
  compare fires it (csim: yes, like a CHxCMP write).
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
  idle channel and -60 dBm while a neighbour transmits. IEEE CCA_REQ derives
  energy/correlation/sync from that and from a frame in reception; the
  correlation value reported per frame is a constant 50.
- IEEE timestamps sit at the end of the SFD + 50 µs (the relation Contiki's
  CC26xx correction implies); Contiki's own +800 µs CC13xx correction is
  left as it is. No source-address matching (autoPend) and no slotted ACKs.
- The watchdog interrupt is always the maskable IRQ (CTL.INTTYPE's NMI
  routing is not modelled). It pauses while the PERIPH domain is off, which
  is how SysCtrlStandby() differs from SysCtrlIdle(). A watchdog reset is
  reported on stderr (`[cc13x2] watchdog reset at ...`).
- SSI, GPT, uDMA, TRNG, crypto and the AUX ADC are plain register stores; the
  AUX TDC measures exact oscillators, so the RC calibration trims nothing.

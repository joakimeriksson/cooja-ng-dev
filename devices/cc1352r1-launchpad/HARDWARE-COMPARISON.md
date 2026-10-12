# CC1352R1 LaunchPad — emulator vs. silicon

Board: LAUNCHXL-CC1352R1, CC1352R PG2.1 (JTAG IDCODE `0x3BB4102F`), on-board
XDS110 (firmware 3.0.0.43) driven by openocd 0.12 with
`board/ti_cc13x2_launchpad.cfg`. Console on the XDS110's application UART
at 115200. One board, 2026-10-12.

Flashing and capture: `tools/cc13x2-hwcheck/flash.py ELF SECONDS`. Images
that sleep in standby (anything with the radio off) power the JTAG domain
down, so plain `init; halt` reads an all-zero scan chain; the script's
`catch.tcl` pulses nRESET and halts the CPU in the boot window first.

## Factory configuration

FCFG1 read from the board (`dump_image fcfg1.bin 0x50001000 0x430`, kept in
[`hardware/fcfg1.bin`](hardware/fcfg1.bin)) replaces csim's synthesized
table; the per-node MAC words are still overlaid. The fields the boot path
depends on, against what csim synthesized before:

| Field | Board | csim before |
|---|---|---|
| ICEPICK_DEVICE_ID | `0x3BB4102F` | same |
| FCFG1_REVISION, MISC_CONF_1 | `0x28`, `0xFFFFFF00` | same |
| USER_ID | `0x3082F000` (PKG 2, 7x7) | `0x0083F000` (PKG 3) |
| IOCONF | `0xFFFFFF1C` (GPIO_CNT 28) | `0x1C` |
| TFW_FT | `0x30F5BBC4` (% 10000 = 756) | 700 |
| PRCM.RFCMODEHWOPT | `0x2F` | `0xFF` |

## Boot

`hello-world-nullradio` prints the same banner on both, and on the board the
chip's own factory address (`0012.4b00.1ca1.9758`), so the FCFG1 read path
is the same. Host timestamps resolve tens of ms only (USB bursts):

| | Board | csim |
|---|---|---|
| Banner start to `RF: Channel 0` | ~53 ms | 56 ms |
| Banner to first `Hello, world` | ~0.16-0.18 s | 0.131 s |
| Hello to hello | 9.968 / 10.015 s | 9.995 s |

## AON RTC (hwcheck)

`firmware/cc1352r1-launchpad/hwcheck.cc1352r1-launchpad` measures on the
chip (`tools/cc13x2-hwcheck/hwcheck.c`). Values in 1/65536 s units (the TIME
register steps by 2 per SCLK_LF edge). Identical over 13 rounds on the
board and in csim:

| Test | Board | csim |
|---|---|---|
| Channel 2 flag visible, compare even | cmp+2, 32/32 | cmp+2, 32/32 |
| Channel 2 flag visible, compare odd | cmp+3, 32/32 | cmp+3, 32/32 |
| rtimer callback (channel 1, interrupt) | even +2, odd +3 | same |
| Compare 2 units .. 0.875 s in the past, enable or write | fires 2 units later | same |
| Compare 1.5 s in the past | never fires | same |

So the event registers one SCLK_LF edge after the counter reaches the
compare value, and a compare armed up to 1 s in the past fires on the next
edge — both as csim modelled them from TI's TimerP arithmetic and
`hw_aon_rtc.h`. (The "before" histogram — the last TIME read before the flag
was seen — differs: that is the poll loop's sampling, and AON bus reads are
not instantaneous on silicon.)

## RF core status semantics (hwcheck-ieee)

The IEEE image alternates channel 26 with channel 27, which Contiki accepts
in IEEE builds on CC13xx (its range check uses the sub-GHz channel table) and
writes into CMD_IEEE_RX's channel field. Status words read back on the board
showed three behaviours csim did not have; it has them now and matches the
board word for word:

| Command | Board | csim before |
|---|---|---|
| CMD_FS at Contiki's sub-GHz frequency in IEEE mode (every boot) | `0x0809` ERROR_SYNTH_PROG | `0x0400` DONE_OK |
| CMD_IEEE_RX, channel 27 | `0x2800` IEEE_ERROR_PAR | listening (2485 MHz) |
| CMD_IEEE_TX after that | `0x2802` IEEE_ERROR_NO_FS, at once | `0x2400`, frame sent |

Contiki's non-poll TX path checks only for the COMMAND_DONE event, so the
failed transmit returns RADIO_TX_OK in ~90 µs on both now; CCA reports busy
(no RX, no RSSI).

## Radio timing (hwcheck)

Mean of 8 driver calls each, measured with the RTC (15 µs units) and
cross-checked with the DWT cycle counter. Prop mode (50 kbps, 868 MHz):

| Call | Board | csim | Δ |
|---|---|---|---|
| `transmit()` per-byte slope, 12..96 bytes | 160.2 µs | 160.2 µs | 0 |
| `transmit(12)` | 4303 µs | 4028 µs | +275 µs |
| `off()` (stop RX, CMD_SYNC_STOP_RAT) | 153 µs | 61 µs | +92 |
| `on()` (CMD_SYNC_START_RAT, RX) | 168 µs | 92 µs | +76 |
| set channel (RAT stop/start, CMD_FS, RX) | ~435 µs | 290 µs | +145 |
| `channel_clear()` straight after a retune | busy 8/8 | clear 8/8 | |
| `channel_clear()` 2 ms later | clear 8/8 | clear 8/8 | |

IEEE mode: `transmit()` matches within one RTC unit at every length (board
58/84/134/234 units for 12/24/48/96 bytes, csim 56/84/132/234). `off()` is
again ~92 µs slower on the board.

Open, not yet changed in the model:

- **Prop TX fixed time, +275 µs.** One board cannot tell whether it is
  before the first preamble bit (moves the frame on air) or after the last
  (PA ramp-down). Needs a second board: a receiver timestamping the sync
  word against a GPIO edge from the sender (AON RTC channel 1 capture).
- **CCA after a retune.** On silicon the RSSI is not valid for a while after
  RX restarts, so Contiki's CCA reports busy; csim's RSSI is valid at once.
- **Command latencies.** The driver-call deltas do not decompose
  consistently into per-command constants (solving them makes CMD_FS faster
  on silicon); timing commands one at a time is the next step.

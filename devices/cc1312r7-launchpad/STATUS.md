# CC1312R7 LaunchPad Port — Status & Direction

> For the device contract see [`SPEC.md`](SPEC.md).

## Current state — sub-GHz through L6, Zephyr boots

| Level | Evidence |
|-------|----------|
| L0 ELF loads | 704 KB flash image, CCFG at `0xAFFA8` |
| L1 reset → `main()` | through `SetupTrimDevice()` in the CC13x2x7 stand-in ROM (wafer ID check passes) |
| L2 console | banner on UART0 |
| L3 buttons | `configs/test-button-cc1312r7-launchpad.yaml` + `test/scripts/button-cc1312r7-launchpad.cnsh` (BTN-1 DIO13, BTN-2 DIO14) |
| L3 console input | `configs/test-shell-cc1312r7-launchpad.yaml` |
| L4 Contiki timers | `configs/test-boot-cc1312r7-launchpad.yaml` |
| L5 radio, prop | `configs/test-nullnet-cc1312r7-launchpad.yaml` |
| L6 full stack, prop | `configs/test-rpl-udp-cc1312r7-launchpad.yaml` |
| TSCH, prop | `configs/test-tsch-cc1312r7-launchpad.yaml` — association + ≥3 drift reports |
| Interop, prop | `configs/test-mixed-subghz-cc1312r7-cc1352r1.yaml` — two CC1312R7 clients in a CC1352R1-rooted DAG |
| Zephyr | `zephyr-hello-world.cc1312r7-launchpad` — boots (`SOC_CC1352R7` init, same ROM) and prints |

## Firmware

Contiki-NG has no CC1312R7 support upstream yet. The images here come from
contiki-ng/contiki-ng#3285 (with contiki-ng/coresdk_cc13xx_cc26xx#9): a
`cc13x2x7-cc26x2x7` CPU subfamily that builds TI's drivers from source
against the SDK 7.10.01.24 CC13x2x7 driverlib, and a `launchpad/cc1312r7`
board. Until it is merged, rebuilding needs that PR's branch,
`feature/cc1312r7-launchpad` (`--contiki-dir`).

## Open

- **FCFG1 trims.** `ICEPICK_DEVICE_ID`, `USER_ID`, `MISC_CONF_1`, `IOCONF`
  and `RFCMODEHWOPT` are read from an LP-CC1312R7. The rest of FCFG1 is
  still the CC1352R1's: 91 of its 268 words differ on the R7, mostly trims,
  and the R7's FCFG1 goes on past `0x430`. A per-part table from a dump
  (identifying words blanked, as for the CC1352R1) replaces them.
- **Idle RSSI.** On an LP-CC1312R7 the idle channel reads −87 to −88 dBm
  (a CC1352R1 on the same desk: −93 to −103), above Contiki's −90 dBm CCA
  threshold, so contiki-ng#3285 never transmits under CSMA on silicon. The
  model reports −110 dBm idle for every part, so these tests cannot show it.
- **Hardware comparison.** Run `tools/cc13x2-hwcheck` on an LP-CC1312R7 (`flash.py --x7`)
  for the same table as `devices/cc1352r1-launchpad/HARDWARE-COMPARISON.md`.
- **On hardware, the Contiki-NG port** boots, receives every frame from
  CC1352R1 and CC1312R1 peers, and transmits at 13 dBm once the CCA
  threshold is raised. LEDs, buttons, standby current and the 12/14 dBm PA
  entries are untested there.

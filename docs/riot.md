# Running RIOT OS on Cooja-NG

Cooja-NG boots **RIOT OS** on its emulated nRF52840 (on-chip 2.4 GHz 802.15.4
radio), in addition to Contiki-NG and Zephyr. A stock `gnrc_networking` image
boots to its shell and two nodes form an **RPL DODAG** over the air:

```
main(): This is RIOT! (Version: ...)
RIOT network stack example application
All up, running the shell now
...
dodag [2001:db8::1 | R: 512 | OP: Router | PIO: on | ...]
  parent [addr: fe80::ec6a:6574:72b9:6c63 | rank: 256]
```

> **Experimental / best-effort.** Contiki-NG is csim's primary, fully-validated
> target (it passes the upstream Cooja test suite). RIOT support is newer and
> narrower — the cases documented here work end-to-end, but coverage is limited
> to `gnrc_networking` on `nrf52840dk` and the known gaps under *Limitations*.

## Build a firmware

No Zephyr-style SDK is needed — RIOT builds with `make` and the system
`arm-none-eabi-gcc`:

```sh
git clone --depth 1 https://github.com/RIOT-OS/RIOT.git
cd RIOT/examples/networking/gnrc/networking
BOARD=nrf52840dk make            # -> bin/nrf52840dk/gnrc_networking.elf
```

The board is `nrf52840dk` (the on-chip `nrf802154` radio is the default netdev).
Copy/rename the ELF to a `*.nrf52840-dk` path so csim's board registry selects
the platform (stripping with `arm-none-eabi-strip --strip-debug` keeps it small):

```sh
cp bin/nrf52840dk/gnrc_networking.elf /tmp/riot-gnrc.nrf52840-dk
```

## Run

```sh
# Single node — boots to the shell (idle; csim has no stdin by default)
./build/test_runner nrf52840-dk-multinode /tmp/riot-gnrc.nrf52840-dk -n 1 -t 3000
```

## Drive the shell headlessly (2-node RPL)

`gnrc_networking` is shell-driven: forming a DODAG needs an `rpl root` on one
node and `rpl init` on the others (the leaf then auto-joins on the root's DIO).
csim feeds the UART shell via per-node env vars — `CSIM_NODE<i>_INPUT` (text;
`\n`/`\r` become newlines) injected at `CSIM_NODE<i>_AT_MS`. `CSIM_NODE_RADIUS`
shrinks the default 20 m placement circle so the nodes sit inside `tx_range`:

```sh
CSIM_NODE_RADIUS=3 \
CSIM_NODE0_INPUT='\nifconfig 5 add 2001:db8::1\nrpl root 1 2001:db8::1\n' \
CSIM_NODE1_INPUT='\nrpl init 5\n' CSIM_NODE1_AT_MS=4000 \
./build/test_runner nrf52840-dk-multinode \
    /tmp/riot-gnrc.nrf52840-dk /tmp/riot-gnrc.nrf52840-dk -t 25000
```

Node 1 becomes the root (emits DIOs); node 2 joins as a Router, sends a DAO, and
gets a DAO-ACK. (`5` is the gnrc interface number — check it with `ifconfig`.)

> The env vars are quick scaffolding. A JSON config is the better home: timed
> `send` actions type into the shell at an exact time (`docs/test-format.md`).
> `configs/interop-contiki-riot.json` drives the bundled image that way.

## RIOT in a Contiki-NG network

`configs/interop-contiki-riot.json` puts the bundled
`firmware/nrf52840-dk/riot-gnrc-networking.nrf52840-dk` next to a Contiki-NG RPL
root (`examples/interop/riot-root`). RIOT joins the root's DODAG, configures
`fd00::` from its prefix, pings it and trades UDP messages with it. What it takes,
all found on this emulator and each a property of the two stacks:

- **PAN ID.** RIOT defaults to 0x23, Contiki-NG and Zephyr to 0xabcd; the config
  sets it with `ifconfig 5 set pan_id 0xabcd`.
- **Storing mode.** RIOT's RPL builds storing-mode DAOs only (a Transit option
  without a parent address), and Contiki-NG's default RPL Lite is non-storing, so
  the root runs RPL Classic in storing mode.
- **Link-local traffic.** Contiki-NG adds the RPL Option (RFC 6553, type 0x63) to
  packets for global addresses. RIOT has no handler for it, and the type's top
  bits say "discard if unrecognised", so RIOT drops them; RFC 9008's type 0x23
  would be skipped instead. The demo's ping and UDP use link-local addresses.

## What the emulator provides

RIOT exercises the nRF52840 / Cortex-M4F differently from Contiki and Zephyr, and
each gap was a one-line-class fix in `src/arm/` (all general, all
regression-clean — see commit history). The notable ones:

- **PLD/PLI preload hint** — a byte/halfword load with `Rt=PC` is a NOP on
  Cortex-M, not a load-into-PC; newlib's optimised `strlen`/`memcpy` emit them.
- **M4 parallel `UADD8`/`SADD8` + `SEL`** with the `APSR.GE` flags — newlib's
  `strlen`/`strcmp` use them to find a zero byte in a word.
- **TIMER compare reschedule on `INTENSET`** — RIOT's nrf5x `timer_set` (and thus
  `ZTIMER_USEC`) writes `CC` then `INTENSET`; without rescheduling, the compare
  IRQ never fired and the ieee802154 CSMA backoff hung → no TX.
- **UARTE EasyDMA RX** (`STARTRX`/`RXD.PTR`/`ENDRX` + the `ENDRX_STARTRX` short)
  — the missing half of the console, used for the shell-driving above.
- **Clear latched `EVENTS_CRCOK` on `EVENTS_END`** — RIOT's `nrf802154` driver
  consumes a frame via `END`+`CRCSTATUS` and never clears `CRCOK`, which kept the
  radio's RX re-arm deferred forever (the node went deaf after its first frame).

The radio TX/RX path, the RADIO state machine, the TEMP sensor, and TIMER/RTC
peripherals are shared with the Zephyr/Contiki nRF52840 model (`docs/zephyr.md`,
`src/arm/nrf52840_soc.c`).

### Bring-up debugging toolkit

Env-gated, behaviour-neutral scopes (in `src/arm/`) that made this tractable:

| Env var | What it shows |
|---|---|
| `ARM_WILD_TRAP=1` | first jump to a wild PC (SRAM or `>=0x40000000`) + registers — catches a corrupted PC |
| `ARM_PC_WATCH=0xa,0xb` | hit-counts for specific PCs (is an instruction reached?) |
| `NRF_RADIO_TRACE=1` | RADIO tasks, `STATE` value, ramp start/complete |
| `NRF_RXBYTE_TRACE=1` | radio state on each received air byte (RX vs DISABLED) |
| `NRF_UART_TRACE=1` | UART TX/RX register traffic + the RX-ring delivery |

Plus the end-of-run per-node `PC=0x…` → `arm-none-eabi-addr2line`, the per-mote
GDB stub, and a one-shot Zephyr/RIOT thread-list dumper pattern (resolve a stuck
thread's `pended_on` via DWARF — see `docs/design/zephyr-dad-test-plan.md`).

## Limitations

- Global-address traffic from Contiki-NG to RIOT is dropped by RIOT (the RPL
  Option, above); RIOT to RIOT is untested beyond the DODAG.
- Contiki-NG sometimes misses RIOT's 802.15.4 ACK (RIOT sends it about 240 µs
  after the frame, from software) and resends, so a reply can arrive twice.
  Whether real hardware does the same is not checked.
- `gnrc_networking` is shell-driven; non-shell apps that auto-configure would run
  without the injection step.

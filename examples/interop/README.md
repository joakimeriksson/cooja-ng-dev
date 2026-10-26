# Contiki-NG with other IoT OSes

The Contiki-NG side of two demos where Contiki-NG shares one emulated 802.15.4
channel with another OS. Both run on the nRF52840 DK, and the other OS's image is
stock.

| Demo | Config | Contiki-NG side | Other side |
|---|---|---|---|
| UDP echo over 6LoWPAN | `configs/interop-contiki-zephyr.json` | `echo-zephyr/` | Zephyr `echo_server` sample with `overlay-802154.conf` |
| One RPL network | `configs/interop-contiki-riot.json` | `riot-root/` | RIOT `gnrc_networking`, driven from its shell |

```sh
./build/test_runner test configs/interop-contiki-zephyr.json
./build/test_runner test configs/interop-contiki-riot.json
```

Build either Contiki-NG image with

```sh
tools/build-device-firmware.sh --local --contiki-dir <clone> --target nrf52840 --board dk \
    --example examples/interop/<name> --output firmware/nrf52840-dk/<name>.nrf52840-dk \
    --make-args "CONTIKI=<clone>"
```

What each pairing needs, and why, is in the comment at the top of each source
file, in `docs/riot.md` (PAN ID, RPL storing mode, link-local traffic) and in
`docs/zephyr.md` (no 802.15.4 ACKs from the Zephyr radio model yet).

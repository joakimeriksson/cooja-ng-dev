
## udp-client-v52.cc2538dk

- **Source**: contiki-ng commit `5968cdae931a68a1413b5b8feef1c0438c2ae2a1`
- **Source path**: `examples/rpl-udp` (file: `udp-client.c`)
- **TARGET**: `cc2538dk`
- **Toolchain**: host arm-none-eabi-gcc 15.2.Rel1
- **Built**: 2026-09-09T08:44:27Z by Joakim Eriksson
- **Build command**: `tools/build-device-firmware.sh --target cc2538dk --example examples/rpl-udp --source-file udp-client --local --output firmware/cc2538dk/udp-client-v52.cc2538dk`
- **Why**: the Renode half of the cross-simulator demo (examples/renode/cc2538-csim-rpl.resc).
  Renode emulates this CC2538; csim emulates its RPL parent.

## udp-server-v52.cc2538dk

- **Source**: contiki-ng commit `5968cdae931a68a1413b5b8feef1c0438c2ae2a1`
- **Source path**: `examples/rpl-udp` (file: `udp-server.c`)
- **TARGET**: `cc2538dk`
- **Toolchain**: host arm-none-eabi-gcc 15.2.Rel1
- **Built**: 2026-09-09T19:52:28Z by Joakim Eriksson
- **Build command**: `tools/build-device-firmware.sh --target cc2538dk --example examples/rpl-udp --source-file udp-server --local --output firmware/cc2538dk/udp-server-v52.cc2538dk`
- **Why**: the RPL root for the Renode co-simulation demo (examples/renode/cc2538-csim-rpl.resc):
  Renode emulates this CC2538 as the DAG root and UDP server; csim's nodes join it.

## Overlay struct layouts (udp-client, udp-server)

`src/services/rpl_layout.h` has rows for `udp-client.cc2538dk` and `udp-server.cc2538dk`
(layout `contiki-ng-cortex-m`), from DWARF builds of `examples/rpl-udp` at contiki-ng
commit `52f1b1e7d66605c30e3441ae87ab00328aa561f2` (host arm-none-eabi-gcc 15.2), made with each
image's build command plus `--layout src/services/rpl_layout.h`.  The
overlay picks them by CPU and `sizeof(rpl_instance_t)` (352 bytes, which the
shipped images' `curr_instance` symbols match); an image rebuilt with
another configuration needs the rows regenerated (see
`firmware/PROVENANCE-template.md`).

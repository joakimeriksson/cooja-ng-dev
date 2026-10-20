<!-- DRAFT for 0.3.1 (docs/design/release-0.3.1-usability-plan.md §8.1).
     Describes the planned `cngsim` command; none of it works yet. -->

# Quick start

Cooja-NG simulates networks of Contiki-NG nodes (also Zephyr and RIOT) on
emulated hardware: Tmote Sky, CC2538, nRF52840, nRF54L15, CC1352R and more.
It runs the real firmware images you would flash onto the boards, usually
hundreds of times faster than real time. You can watch the network in your
browser.

If you have used Cooja before, see [Coming from Cooja](#coming-from-cooja).

## 1. Install

Linux (x86-64):

```sh
curl -L https://github.com/mikroverk/cooja-ng/releases/download/v0.3.1/cooja-ng-v0.3.1-linux-x86_64.tar.gz | tar xz
export PATH=$PWD/cooja-ng-v0.3.1-linux-x86_64/bin:$PATH
```

On a Mac, use `macos-arm64` (Apple Silicon) or `macos-x64` (Intel) in place
of `linux-x86_64`.

Check that it works:

```sh
cngsim --version
```

> **Building from source** (Linux, macOS, FreeBSD): `git clone
> https://github.com/mikroverk/cooja-ng && cd cooja-ng && make && make
> install PREFIX=~/.local`. It needs only a C compiler; GNU Lightning is
> optional and makes it faster.

## 2. Run your first simulation

```sh
cngsim run rpl-udp-sky
```

`rpl-udp-sky` is one of the bundled examples (`cngsim examples` lists them
all). It has two Tmote Sky nodes. Node 2 joins node 1's RPL network and
sends it UDP requests; node 1 answers each one:

```
cooja-ng 0.3.1 · RPL-UDP on Tmote Sky · 2 nodes · seed 42 · 60 s
    8.300 [Node 2/MSP430] [INFO: App       ] Not reachable yet
   17.877 [Node 2/MSP430] [INFO: App       ] Sending request 0 to fd00::212:7401:1:101
   17.896 [Node 1/MSP430] [INFO: App       ] Received request 'hello 0' from fd00::212:7402:2:202
   17.897 [Node 1/MSP430] [INFO: App       ] Sending response.
   17.943 [Node 2/MSP430] [INFO: App       ] Received response 'hello 0' from fd00::212:7401:1:101
   ...
  ✓ "Sending request" on node 2 (17.877 s)
  ✓ "Received request" on node 1 (17.896 s)
  ✓ "Received response" ×3 on node 2 (37.083 s)
PASSED · 37.1 s simulated in 0.04 s (2800x real time)
```

How to read the output:
- Each line is something a node printed on its serial console. The line
  starts with the **simulated** time in seconds, followed by the node it
  came from.
- The ✓ lines are the example's checks.
- The exit code is `0` when every check passes, so `cngsim run` can go
  straight into a CI job. The other exit codes are listed in `cngsim help
  run`.

The same seed always gives the same run, down to the nanosecond. Use
`--seed 7` to get a different one.

## 3. Watch it

```sh
cngsim run rpl-udp-sky --ui
```

Your browser opens at `http://127.0.0.1:8080/`, with the simulation paused.
Press **Play** (or Space) in the browser, or Enter in the terminal. To skip
the pause, add `--start`. Things to try:
- **Network view:** each node is a dot, and the lines are radio links. Drag
  node 2 away from node 1 until the link breaks. Its console reports `Not
  reachable` until you drag it back.
- **Consoles:** one per node. Type into the box under a node to send it
  serial input.
- **Packets:** every radio frame, decoded (802.15.4, 6LoWPAN, IPv6, RPL,
  UDP).
- **Timeline:** radio activity per node over time. Scroll to zoom, and drag
  to pan.
- **Speed slider:** from 1x (real time) up to as fast as your machine goes.

When the simulated duration ends, the simulation pauses. Press Ctrl-C in the
terminal to quit.

## 4. Make your own setup

Simulations are plain YAML files. To start a new one:

```sh
cngsim new rpl-udp --board sky -n 6 --layout grid -o my-net.yaml
cngsim run my-net.yaml --ui
```

`my-net.yaml` comes out commented. The parts you will want to change:

```yaml
version: 2
title: "My RPL network"
timeout_ms: 120000          # how long to simulate (simulated ms)
seed: 42

medium:
  type: udgm                # unit-disk radio model, as in Cooja
  tx_range: 50              # metres

mote_types:                 # each firmware image you use, with a name
  - { name: server, firmware: udp-server.sky }
  - { name: client, firmware: udp-client.sky }

nodes:
  - { type: server, id: 1, x: 0, y: 0 }
  - { type: client, count: 5, layout: grid, columns: 3, spacing: 30, origin: [30, 0] }
```

- Firmware paths are relative to the YAML file.
- Layouts place nodes for you: `grid`, `line`, `circle` and `random`.
  `random` is still reproducible, because it follows `seed`. You can also
  give single nodes an `x` and `y` of their own.
- `cngsim check my-net.yaml` validates a file without running it. A typo in
  a key name is an error, not something silently ignored.

**Editing in the browser:** with `--ui`, use **Add node** (pick one of the
file's mote types and click where to put it), right-click a node to remove
or reboot it, drag nodes around, and press **Save** to download the result
as a new YAML file.

## 5. Use your own Contiki-NG firmware

Build the app for one of the supported boards as you normally would. For a
Tmote Sky:

```sh
cd contiki-ng/examples/hello-world
make TARGET=sky
```

Then name the image in your YAML:

```yaml
mote_types:
  - { name: hello, firmware: ~/contiki-ng/examples/hello-world/hello-world.sky }
```

Some boards' images don't end in the board's name (`.nrf`, `.simplelink`,
`.zoul`, or a plain `.elf`). For those, say which board it is:

```yaml
mote_types:
  - { name: hello, board: nrf52840-dk,
      firmware: ~/contiki-ng/examples/hello-world/hello-world.nrf52840 }
```

`cngsim boards` lists every board with the `TARGET`/`BOARD` that builds for
it:

```
board               cpu                radio                       contiki-ng
sky                 MSP430F1611        CC2420 (2.4 GHz)            TARGET=sky
cc2538dk            Cortex-M3          on-chip 802.15.4            TARGET=cc2538dk
nrf52840-dk         Cortex-M4F         on-chip 802.15.4            TARGET=nrf52840 BOARD=dk
nrf54l15-dk         Cortex-M33         on-chip 802.15.4            TARGET=nrf BOARD=nrf54l15/dk
cc1352r1-launchpad  Cortex-M4F         sub-GHz + 2.4 GHz RF core   TARGET=simplelink BOARD=launchpad/cc1352r1
...
```

Zephyr and RIOT images work the same way. See [zephyr.md](zephyr.md) and
[riot.md](riot.md).

## 6. Add checks

A `test:` section turns a simulation into a test with a pass/fail result:

```yaml
test:
  steps:                                  # in order; each waits for a console line
    - { wait: "Sending request", node: 2 }
    - { wait: "Received response", node: 2, count: 3 }
  fail_on: ["ERROR", "assertion"]         # any node printing these fails the run
```

Cooja test scripts (JavaScript, with `YIELD`, `WAIT_UNTIL`, `log.testOK()`
and so on) also run. Use `js_script: my-test.js`, or put the script inline
with `js_script_inline:`. The full format is in
[test-format.md](test-format.md).

For an interactive console with breakpoints, `expect` and scripted test
sessions, run `cngsim run my-net.yaml --shell` (see [shell.md](shell.md)).

## Coming from Cooja

| In Cooja | In Cooja-NG |
|---|---|
| `./gradlew run`, File > Open `sim.csc` | `cngsim run sim.yaml --ui` |
| `--no-gui sim.csc` | `cngsim run sim.yaml` |
| File > New, Add motes (random / linear / ellipse) | `cngsim new`, or `count:` + `layout:` in the YAML, or **Add node** in the browser |
| Create mote type > Compile | build with `make TARGET=…` and name the image in `mote_types` |
| Network, Mote output, Timeline, Radio messages | the browser's network view, consoles, Timeline and Packets tabs |
| Script editor | `test:` steps, or your Cooja JS script as `js_script:` |
| An existing `.csc` | `cngsim convert sim.csc -o sim.yaml` (needs python3; Sky, Z1 and Cooja motes) |

Running Contiki-NG's own Cooja test suite under Cooja-NG is described in
[contiki-ng-testing.md](contiki-ng-testing.md).

## Next

- `cngsim help`: every command and option.
- [test-format.md](test-format.md): the YAML format in full.
- [shell.md](shell.md): the interactive shell and command scripts.
- [ui-replay.md](ui-replay.md): record a run and replay it in the browser
  without the simulator.

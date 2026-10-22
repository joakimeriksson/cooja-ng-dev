# Cooja-NG in the browser (WebAssembly)

The simulator, compiled to WebAssembly with Emscripten, running real Contiki-NG
firmware in a browser tab. It is not a model of Cooja-NG in JavaScript. It is
the same C code as `build/test_runner`: the event kernel, the MSP430, ARM
Cortex-M and RISC-V emulators, the radio models, the scenario runner and its
web UI service. The firmware stays the original ELF (`.sky`, `.nrf52840-dk`,
…), executed instruction by instruction inside the module.

Experimental and opt-in: nothing here is reachable from the top-level
`Makefile`, and the native build is unchanged.

## Build and run

```sh
# once: an emsdk (tested: 6.0.13, macOS arm64 and Ubuntu x86_64)
git clone https://github.com/emscripten-core/emsdk.git ~/emsdk
~/emsdk/emsdk install 6.0.13 && ~/emsdk/emsdk activate 6.0.13
source ~/emsdk/emsdk_env.sh        # emsdk needs Python >= 3.10 (EMSDK_PYTHON=...)

make                               # the native build: check-parity compares against it
make -C experiments/browser-wasm   # dist/csim-cli.js and dist/web/
make -C experiments/browser-wasm check   # native vs WebAssembly: must be identical
make -C experiments/browser-wasm serve   # http://127.0.0.1:8765/  (the site + live demos; PORT=...)
```

Open `http://127.0.0.1:8765/player.html?wasm=grid100`, or use a card's
**Run it live** link on the landing page. The scenarios are in
`web/scenarios.json`: `hello`, `udp`, `mixed`, `grid16`, `grid100`, `tsch`,
`trustzone`, `energest`. Each one gives the runner arguments and the files it
reads. Add `&arg=--log` to see the runner's console output in the browser
console, and `&arg=<flag>` to pass any other runner flag.

## Two builds

| | `dist/csim-cli.js` | `dist/web/csim-web.js` |
|---|---|---|
| Runs in | Node | a Web Worker |
| Files | the repository's, directly (`NODERAWFS`) | fetched into MEMFS from `files/` |
| UI | none: a drop-in for `build/test_runner` | `--ui`: the web UI stream, sent to the page by postMessage |
| Purpose | parity with native (`make check`, CI) | the live page |

## How the page runs a simulation

```
player.html (ui/index.html)  --postMessage-->  worker.js  -->  csim-web.wasm
  ?wasm=NAME: a WorkerSocket                     fetch firmware      runner --ui
  stands in for the WebSocket                    into MEMFS,         websocket_ui_service
  (same JSON full state,                         callMain(args)      ws_server_worker.c
   CBOR deltas, commands)
```

- `src/ws_server_worker.c` implements `include/ui/ws_server.h`, the same
  10-function interface as the native `src/ui/ws_server.c`. Broadcasts become
  `postMessage`, and the page's commands (pause, play, speed, move, restart)
  come in through `csim_wasm_ui_recv()`. This file is the only C code that
  differs between the browser build and the native one.
- The runner's main loop already polls the UI service on every pass, so that
  poll is where the simulation hands the worker its event loop back
  (`emscripten_sleep` under ASYNCIFY, at most every 30 ms of wall time). The
  runner's pacing `usleep()` yields the same way. Commands are applied between
  loop passes, exactly where the native WebSocket poll applies them.
- `ui/index.html` gains one mode: with `?wasm=NAME` it starts the worker in
  place of opening a WebSocket. The live and `?replay=` modes are unchanged.

## Parity

`make check` (`check-parity.sh`) runs each scenario with `build/test_runner`
and with `node dist/csim-cli.js`. It diffs stdout and stderr separately,
because their interleaving depends on host buffering. Before diffing it drops
the wall-clock lines `tools/check-determinism.sh` drops. Verified identical,
including every node's final cycle and instruction counts:

- hello-world, timertest, and the two-node UDP pair (Sky);
- all six site demos: the mixed MSP430 + CC2538 + nRF52840 network, 16 ×
  CC2538, nRF54L15 TrustZone, nRF52840 TSCH and energest. Run them with
  `./check-parity.sh test <config>`.

Running in 1 ms slices gives the same results as running unbounded natively
(checked on the UDP pair with `--speed 200`). That is why yielding to the
browser between passes does not change the simulation.

## Speed

| | grid100, 100 × Sky |
|---|---|
| native, headless | 9.6× real time |
| Node, `csim-cli.js` | 7.2× |
| Node, the web build (ASYNCIFY), headless | 5.6× |
| native, `--ui` | ≈4× |
| Chrome, live page | ≈1.6× (after boot) |

Smaller scenarios run as fast as the speed slider asks. Most of the gap with
the UI on is the runner's UI path itself (native slows down too), not
WebAssembly. ARM nodes run on the interpreter only (no JIT in a browser).

## Limitations

- Interpreter only: no GNU Lightning JIT for MSP430 or ARM.
- No plugins loaded from `.so` (compiled-in plugins such as energest work), no
  native or Cooja-native motes, no TUN, serial socket, GDB, Renode or shell.
  Their code is linked in, but its host calls fail at runtime; the shipped
  scenarios never make them.
- One simulation per worker. Restart works in place (the runner's own
  restart); loading another scenario reloads the page.
- `csim-ui-node.js` (the web build under Node, with function names) is for
  profiling: `node --cpu-prof dist/csim-ui-node.js test <config> --ui --speed max --wall-timeout 8`.

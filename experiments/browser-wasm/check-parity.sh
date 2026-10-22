#!/usr/bin/env bash
# check-parity.sh — the WASM build must simulate exactly what the native one
# does.  Runs each scenario with build/test_runner and with dist/csim-cli.js
# (Node), and diffs stdout and stderr separately (their interleaving depends
# on host buffering, not on the simulation) after dropping the wall-clock
# lines tools/check-determinism.sh drops.
#
#   ./check-parity.sh                 # the default scenarios
#   ./check-parity.sh <runner args>   # one scenario of your own
set -u
cd "$(dirname "$0")/../.."
NATIVE=build/test_runner
WASM="node experiments/browser-wasm/dist/csim-cli.js"
[ -x "$NATIVE" ] || { echo "error: $NATIVE not built (run make)" >&2; exit 2; }
[ -f experiments/browser-wasm/dist/csim-cli.js ] || { echo "error: csim-cli.js not built" >&2; exit 2; }

NOISE='Wall-clock time|Speed ratio|Throughput|real-time|^  (distribute|deliver|step \(CPU\)|flush/output|channel sync|other/overhead):'
OUT=$(mktemp -d); trap 'rm -rf "$OUT"' EXIT

one() {
    $NATIVE "$@" > "$OUT/n.out" 2> "$OUT/n.err"; local rn=$?
    $WASM   "$@" > "$OUT/w.out" 2> "$OUT/w.err"; local rw=$?
    local bad=0
    for s in out err; do
        grep -avE "$NOISE" "$OUT/n.$s" > "$OUT/n.$s.f"
        grep -avE "$NOISE" "$OUT/w.$s" > "$OUT/w.$s.f"
        if ! diff -u "$OUT/n.$s.f" "$OUT/w.$s.f" > "$OUT/d.$s"; then
            echo "DIVERGENCE ($s): $*"; head -40 "$OUT/d.$s"; bad=1
        fi
    done
    [ "$rn" = "$rw" ] || { echo "EXIT CODE native=$rn wasm=$rw: $*"; bad=1; }
    [ $bad = 0 ] && echo "IDENTICAL ($(wc -l < "$OUT/n.out.f") + $(wc -l < "$OUT/n.err.f") lines, exit $rn): $*"
    return $bad
}

if [ $# -gt 0 ]; then one "$@"; exit $?; fi
fail=0
one multinode firmware/sky/hello-world.sky -t 60000 || fail=1
one multinode firmware/sky/timertest.sky -t 10000 || fail=1
one multinode firmware/sky/udp-server.sky firmware/sky/udp-client.sky -t 60000 || fail=1
# Every scenario the site runs live (web/scenarios.json), as the demos run.
one test configs/test-mixed-platform-rpl.yaml || fail=1
one test configs/udgm-100node-grid-sky.json -t 20000 || fail=1
one test configs/ui-rpl-udp-grid.json -t 60000 || fail=1
one test configs/test-tz-rpl-udp-nrf54l15-xiao.yaml || fail=1
one test configs/test-tsch-nrf52840-dk.json || fail=1
one test configs/plugin-energest-builtin-v2.json || fail=1
exit $fail

#!/bin/bash
#
# Phase 9 plugin smoke check.
#
# Builds the example packet-sink plugin and asserts:
#   1. loading it via --plugin prints a non-zero observer tally at teardown;
#   2. the tally is deterministic across two runs;
#   3. a missing .so or an unknown built-in name is refused: exit 2, with
#      the reason on stderr, before the run starts.
#
# Usage: tools/check-plugin.sh
#
set -u
RUNNER=${RUNNER:-./build/test_runner}
SO=build/plugins/packet_sink.so
CFG=${CFG:-configs/test-rpl-udp-sky.json}
T=${T:-30000}

# GNU make: the Makefile is GNU syntax, and "make" is BSD make on the BSDs.
case $(uname -s) in *BSD|DragonFly) MAKE=${MAKE:-gmake} ;; *) MAKE=${MAKE:-make} ;; esac
$MAKE plugins >/dev/null 2>&1 || { echo "FAIL: build plugins"; exit 1; }
[ -f "$SO" ] || { echo "FAIL: $SO not built"; exit 1; }

t1=$("$RUNNER" test "$CFG" -t "$T" --plugin "$SO" 2>&1 | grep -E '^packet-sink:')
t2=$("$RUNNER" test "$CFG" -t "$T" --plugin "$SO" 2>&1 | grep -E '^packet-sink:')

if [ -z "$t1" ]; then echo "FAIL: no packet-sink tally line"; exit 1; fi
if [ "$t1" != "$t2" ]; then echo "FAIL: tally not deterministic"; echo " $t1"; echo " $t2"; exit 1; fi
# Assert the tally observed *something* (log lines or uart bytes are always emitted).
if echo "$t1" | grep -qE '0 log lines, 0 uart bytes'; then echo "FAIL: tally is empty: $t1"; exit 1; fi

# Missing .so: refused with the reason, exit 2 (not a run without it).
miss=$("$RUNNER" test "$CFG" -t 2000 --plugin /no/such/plugin.so -q 2>&1)
rc=$?
if [ "$rc" -ne 2 ]; then echo "FAIL: missing .so gave rc=$rc, want 2"; exit 1; fi
echo "$miss" | grep -q '^plugin: /no/such/plugin.so: ' || { echo "FAIL: missing .so not reported"; exit 1; }
# A misspelt built-in name in a config: the same.
tmpcfg=$(mktemp "${TMPDIR:-/tmp}/csim-plugin-XXXXXX") && mv "$tmpcfg" "$tmpcfg.json" && tmpcfg="$tmpcfg.json"
sed 's/"energest"/"energist"/' configs/plugin-energest-builtin-v2.json > "$tmpcfg"
grep -q '"energist"' "$tmpcfg" || { echo "FAIL: could not make the misspelt config"; rm -f "$tmpcfg"; exit 1; }
miss=$("$RUNNER" test "$tmpcfg" -t 2000 -q 2>&1)
rc=$?
rm -f "$tmpcfg"
if [ "$rc" -ne 2 ]; then echo "FAIL: unknown built-in plugin gave rc=$rc, want 2"; exit 1; fi
echo "$miss" | grep -q "unknown builtin service 'energist'" || { echo "FAIL: unknown built-in not reported"; exit 1; }

echo "PLUGIN OK: $t1"
echo "PLUGIN OK: a missing .so and an unknown built-in are refused (exit 2)"

# --- Radio-medium plugin (Phase 11): the lossy medium selected by config v2 ---
MED_SO=build/plugins/lossy_medium.so
MED_CFG=configs/medium-plugin-sky-v2.json
[ -f "$MED_SO" ] || { echo "FAIL: $MED_SO not built"; exit 1; }
m1=$("$RUNNER" test "$MED_CFG" -t 30000 2>&1 | grep -E '^Radio medium:|^  Total RF bytes:')
m2=$("$RUNNER" test "$MED_CFG" -t 30000 2>&1 | grep -E '^Radio medium:|^  Total RF bytes:')
if ! echo "$m1" | grep -q 'Radio medium: lossy (plugin)'; then
    echo "FAIL: lossy medium not selected"; echo " $m1"; exit 1; fi
if [ "$m1" != "$m2" ]; then echo "FAIL: medium plugin not deterministic"; exit 1; fi
echo "PLUGIN OK: medium plugin '$(echo "$m1" | head -1)' (deterministic)"

# --- Energy plugin (energest): a COMPILED-IN plugin selected by config name ---
# (no .so — config plugins:["energest"] resolves to the built-in service).
EN_CFG=configs/plugin-energest-builtin-v2.json
e1=$("$RUNNER" test "$EN_CFG" -t "$T" 2>&1 | grep -E '^energest: network total')
e2=$("$RUNNER" test "$EN_CFG" -t "$T" 2>&1 | grep -E '^energest: network total')
if [ -z "$e1" ]; then echo "FAIL: no energest total line (built-in not attached?)"; exit 1; fi
if [ "$e1" != "$e2" ]; then echo "FAIL: energest not deterministic"; echo " $e1"; echo " $e2"; exit 1; fi
# A radio that was tracked at all charges some energy.
if echo "$e1" | grep -qE '~0\.000 mJ'; then echo "FAIL: energest charged nothing: $e1"; exit 1; fi
# The per-mote line carries the CPU/LPM axis (SIM_OBS_CPU_STATE snapshot).
em=$("$RUNNER" test "$EN_CFG" -t "$T" 2>&1 | grep -E '^energest:  mote ' | head -1)
if ! echo "$em" | grep -qE 'cpu [0-9].* lpm [0-9]'; then
    echo "FAIL: energest missing cpu/lpm axis: $em"; exit 1; fi
echo "PLUGIN OK: energest built-in by name '$e1' (radio+cpu/lpm, deterministic)"
exit 0

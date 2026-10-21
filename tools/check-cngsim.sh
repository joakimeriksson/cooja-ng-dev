#!/bin/sh
# check-cngsim.sh — cngsim end to end, from outside the checkout.
#
#   1. One simulation path: for every example and a set of configs, the node
#      console lines and the exit code of `cngsim run X` equal those of
#      `test_runner test X` (cngsim only changes what reaches the terminal).
#   2. The examples run by name from another directory and pass.
#   3. `cngsim new` writes a setup for each template that `cngsim check`
#      accepts and that runs and passes.
#   4. Bad input fails with exit 2 and says why: an unknown command, an
#      unknown example, an image with no board.
#
# Usage: tools/check-cngsim.sh   (after make; run from the repo root)
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CNGSIM="$ROOT/build/cngsim"
RUNNER="$ROOT/build/test_runner"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/check-cngsim.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
fails=0

ok()   { printf '  ok    %s\n' "$1"; }
bad()  { printf '  FAIL  %s\n' "$1"; fails=$((fails + 1)); }
console() { grep -E '^ +[0-9]+\.[0-9]{3} \[Node ' || true; }

echo "== one simulation path (console lines and exit code, cngsim vs test_runner)"
for cfg in "$ROOT"/examples/*.yaml configs/test-rpl-udp-sky.json configs/chain-4node-sky.yaml \
           configs/test-mixed-platform-rpl.yaml configs/test-rpl-udp-cc1352r1-launchpad.yaml; do
    case "$cfg" in *shell-*) continue ;; esac
    name=$(basename "$cfg")
    (cd "$ROOT" && "$RUNNER" test "$cfg" 2>/dev/null) > "$WORK/a.out"; ra=$?
    (cd "$ROOT" && "$CNGSIM" run "$cfg" 2>/dev/null) > "$WORK/b.out"; rb=$?
    console < "$WORK/a.out" > "$WORK/a.con"
    console < "$WORK/b.out" > "$WORK/b.con"
    if [ "$ra" != "$rb" ]; then
        bad "$name: exit $rb, test_runner exits $ra"
    elif ! cmp -s "$WORK/a.con" "$WORK/b.con"; then
        bad "$name: console lines differ ($(wc -l < "$WORK/a.con") vs $(wc -l < "$WORK/b.con"))"
    elif [ ! -s "$WORK/a.con" ]; then
        bad "$name: no console lines at all"
    else
        ok "$name ($(wc -l < "$WORK/a.con" | tr -d ' ') lines, exit $ra)"
    fi
done

echo "== examples by name, from another directory"
cd "$WORK" || exit 1
for f in "$ROOT"/examples/*.yaml; do
    name=$(basename "$f" .yaml)
    case "$name" in shell-*) continue ;; esac
    out=$("$CNGSIM" run "$name" -q 2>&1); rc=$?
    case "$rc:$out" in
        0:PASSED*|0:DONE*) ok "$name: $out" ;;
        *) bad "$name: exit $rc: $out" ;;
    esac
done
"$CNGSIM" run shell-nrf54l15 --script "$ROOT/test/scripts/shell-nrf54l15-cmd.cnsh" >/dev/null 2>&1 \
    && ok "shell-nrf54l15 with a script" || bad "shell-nrf54l15 with a script"

echo "== cngsim new -> check -> run"
for spec in "rpl-udp sky 5 grid" "rpl-udp nrf52840-dk 3 line" "nullnet sky 4 circle" \
            "hello cc2538dk 2 random"; do
    set -- $spec
    f="$WORK/new/$1-$2-$4.yaml"
    mkdir -p "$WORK/new"
    if ! "$CNGSIM" new "$1" --board "$2" -n "$3" --layout "$4" -o "$f" >/dev/null 2>&1; then
        bad "new $spec"; continue
    fi
    if ! "$CNGSIM" check "$f" >/dev/null 2>&1; then
        bad "check $(basename "$f")"; continue
    fi
    out=$("$CNGSIM" run "$f" -q 2>&1) && ok "new $spec: $out" || bad "new $spec: $out"
done

echo "== bad input exits 2 and says why"
expect2() {   # label, command...
    label=$1; shift
    err=$("$@" 2>&1 >/dev/null); rc=$?
    if [ "$rc" = 2 ] && [ -n "$err" ]; then ok "$label"; else bad "$label (exit $rc, '$err')"; fi
}
expect2 "unknown command" "$CNGSIM" frobnicate
expect2 "unknown example" "$CNGSIM" run no-such-example
cp "$ROOT/firmware/sky/hello-world.sky" "$WORK/hello.elf"
printf 'version: 2\nmote_types:\n  - { name: a, firmware: hello.elf }\nnodes:\n  - { type: a, id: 1 }\n' \
    > "$WORK/noboard.yaml"
expect2 "an .elf with no board:" "$CNGSIM" run "$WORK/noboard.yaml"
expect2 "test_runner: unknown mode" "$RUNNER" frobnicate
sed 's/name: a,/name: a, board: sky,/' "$WORK/noboard.yaml" > "$WORK/board.yaml"
"$CNGSIM" run "$WORK/board.yaml" --duration 1s -q >/dev/null 2>&1 \
    && ok "the same .elf with board: sky runs" || bad "the same .elf with board: sky runs"

echo
if [ "$fails" -eq 0 ]; then echo "check-cngsim: OK"; else echo "check-cngsim: $fails failed"; fi
[ "$fails" -eq 0 ]

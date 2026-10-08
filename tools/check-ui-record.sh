#!/usr/bin/env bash
#
# --ui-record end to end (docs/ui-replay.md): what a recording promises,
# checked on real runs.
#
#   tools/check-ui-record.sh            # uses build/test_runner (BIN= to override)
#
# 1. The file is one strict-UTF-8 JSON document with a full state and its
#    deltas in time order, and every console line the run printed is in it —
#    including the last ones, after the last regular broadcast.
# 2. stdout is byte-identical with and without the flag (wall-clock lines
#    aside): a mixed-ISA run, the Sky chain, and energest over CC2538 radios,
#    whose radio-state stream used to change when something watched the UI.
# 3. A run too short for a broadcast still records its full state.
# 4. A path that cannot be written ends the run before it starts (exit 2).
set -euo pipefail

cd "$(dirname "$0")/.."
BIN=${BIN:-build/test_runner}
[ -x "$BIN" ] || { echo "check-ui-record: $BIN not built (run make)" >&2; exit 2; }
W=$(mktemp -d "${TMPDIR:-/tmp}/check-ui-record.XXXXXX")
trap 'rm -rf "$W"' EXIT
FILTER='^ *(Wall-clock time|Speed ratio|Throughput):'
fail=0
ok()  { echo "  ok    $*"; }
bad() { echo "  FAIL  $*"; fail=1; }

# The recording checker: run from a file, so no heredoc sits inside a
# function or a quoted string.
cat > "$W/check.py" <<'PY'
import json, re, sys
path, stdout_path, name = sys.argv[1:4]
with open(path, encoding='utf-8') as f:              # strict UTF-8
    d = json.load(f)
assert d['format'] == 'cooja-ng-ui-replay/1', 'format'
assert d['full'] and d['full']['nodes'], 'no full state'
t = [x['0'] for x in d['deltas']]
assert t and all(a <= b for a, b in zip(t, t[1:])), 'deltas out of order'
rec = [l for x in d['deltas'] for v in x.get('5', {}).values() for l in v]
rec += [l for n in d['full']['nodes'] for l in n.get('console', [])]
out = [re.sub(r'^\s*[\d.]+ \[Node \d+/[A-Z0-9]+\] ', '', l).strip()
       for l in open(stdout_path, errors='replace') if re.match(r'\s*[\d.]+ \[Node \d+/', l)]
assert out, 'no console output to compare'
missing = [o for o in out if not any(o[:40] in r for r in rec)]
assert not missing, f'{len(missing)} console line(s) not in the file, e.g. {missing[-1]!r}'
print(f'  ok    {name}: {len(t)} deltas in order, all {len(out)} console lines in the file')
PY

same_stdout() {  # same_stdout <name> <args...>
    local name=$1; shift
    "$BIN" "$@" > "$W/$name.plain" 2>/dev/null || true
    "$BIN" "$@" --ui-record "$W/$name.json" > "$W/$name.rec" 2>/dev/null || true
    grep -vE "$FILTER" "$W/$name.plain" > "$W/$name.plain.f" || true
    grep -vE "$FILTER" "$W/$name.rec" > "$W/$name.rec.f" || true
    if cmp -s "$W/$name.plain.f" "$W/$name.rec.f"; then
        ok "$name: stdout identical with --ui-record"
    else
        bad "$name: stdout differs with --ui-record"
        diff "$W/$name.plain.f" "$W/$name.rec.f" | head -6 || true
    fi
}

check_file() {  # check_file <name>: the recording of <name> against its stdout
    if ! python3 "$W/check.py" "$W/$1.json" "$W/$1.plain" "$1"; then
        bad "$1: the recording is not what the run was"
    fi
}

echo "=== check-ui-record ==="

# 1 + 2: a mixed-ISA run (MSP430 + CC2538 + nRF52840), and the Sky chain,
# whose last console lines come after the last regular broadcast.
same_stdout mixed test configs/test-mixed-platform-rpl.yaml -v
check_file mixed
same_stdout chain test configs/chain-4node-sky.yaml -v
check_file chain

# 2: energest over CC2538 radios (push-model radio state).
cat > "$W/energest-cc2538.json" <<'EOF'
{
  "version": 2,
  "title": "check-ui-record: energest over CC2538",
  "timeout_ms": 20000,
  "seed": 1,
  "mote_types": [
    { "name": "server", "kind": "emulated-elf", "board": "cc2538dk",
      "firmware": "firmware/cc2538dk/udp-server.cc2538dk" },
    { "name": "client", "kind": "emulated-elf", "board": "cc2538dk",
      "firmware": "firmware/cc2538dk/udp-client.cc2538dk" }
  ],
  "nodes": [
    { "type": "server", "id": 1, "x": 0, "y": 0 },
    { "type": "client", "id": 2, "x": 30, "y": 0 }
  ],
  "plugins": ["energest"]
}
EOF
same_stdout energest test "$W/energest-cc2538.json" -q
grep -q "energest: network total" "$W/energest.rec" || bad "energest: no energest report"

# 3: shorter than the first broadcast.
"$BIN" test configs/chain-4node-sky.yaml -q -t 50 --ui-record "$W/short.json" > /dev/null 2>&1 || true
if python3 -c "import json,sys; d=json.load(open(sys.argv[1],encoding='utf-8')); sys.exit(0 if d['full'] and d['full']['nodes'] else 1)" "$W/short.json"; then
    ok "short run: full state recorded"
else
    bad "short run: no full state"
fi

# 4: an unwritable path fails before anything runs.
set +e
"$BIN" test configs/chain-4node-sky.yaml -q --ui-record "$W/no-such-dir/x.json" > "$W/bad.out" 2>&1
rc=$?
set -e
if [ $rc -eq 2 ] && ! grep -q "Initializing node" "$W/bad.out"; then
    ok "unwritable path: exit 2 before any node starts"
else
    bad "unwritable path: rc=$rc"
fi

if [ $fail -eq 0 ]; then echo "check-ui-record: OK"; else echo "check-ui-record: FAILED"; fi
exit $fail

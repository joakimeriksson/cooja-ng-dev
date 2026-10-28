#!/bin/sh
# check-save.sh — a saved setup is the setup that ran.
#
#   1. Mote-type identity: two types sharing one image (named once by a
#      relative path, once through a symlink) but naming different boards
#      boot each node on its own type's board, and keep it through a reboot,
#      a typed `add`, --save-config and the shell's save-config, and a reload.
#   2. Board overrides from the command line: an image whose name does not
#      give its board (--board sky on a .elf, the dongle variant of a
#      .nrf52840) saves a setup that checks and runs on that board with no
#      --board.
#   3. Live state: node names and meta survive a save after a removal
#      shifts the saved list, and clock deviations are saved as last set
#      (the shell's `clock`), for configured and added nodes alike.
#
# The browser's Save goes through the same live-save code as these.
#
# Usage: tools/check-save.sh   (after make)
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
RUNNER=${RUNNER:-$ROOT/build/test_runner}
CNGSIM=${CNGSIM:-$ROOT/build/cngsim}
W=$(mktemp -d "${TMPDIR:-/tmp}/check-save.XXXXXX")
trap 'rm -rf "$W"' EXIT
cd "$W" || exit 2
fails=0
ok()  { printf '  ok    %s\n' "$1"; }
bad() { printf '  FAIL  %s\n' "$1"; fails=$((fails + 1)); }
boards() { grep '^Initializing node' "$1" | sed 's/^Initializing node \([0-9]*\) .* as \([^.]*\)\.\.\./\1:\2/' | tr '\n' ' '; }
want() {   # label, got, expected
    if [ "$2" = "$3" ]; then ok "$1"; else bad "$1: got '$2', want '$3'"; fi
}

echo "== two mote types share an image and differ in board"
cp "$ROOT/firmware/nrf52840-dk/hello-world.nrf52840-dk" hw.nrf52840
ln -s hw.nrf52840 link.nrf52840
cat > two.yaml <<EOF
version: 2
mote_types:
  - { name: dk,     board: nrf52840-dk,     firmware: hw.nrf52840 }
  - { name: dongle, board: nrf52840-dongle, firmware: ./link.nrf52840 }
nodes:
  - { type: dongle, id: 1, x: 0, y: 0 }
  - { type: dk, id: 2, x: 10, y: 0 }
EOF
"$RUNNER" test two.yaml -t 300 --save-config saved.yaml > run.out 2>&1
want "each node boots on its type's board" "$(boards run.out)" "1:ARM/nRF52840-Dongle 2:ARM/nRF52840-DK "
want "the save keeps each node's type" "$(grep -E '^  - \{ type:' saved.yaml | sed 's/, id: \([0-9]*\).*/:\1/;s/.*type: //' | tr '\n' ' ')" "dongle:1 dk:2 "
"$RUNNER" test saved.yaml -t 300 > reload.out 2>&1
want "the reload boots the same boards" "$(boards reload.out)" "1:ARM/nRF52840-Dongle 2:ARM/nRF52840-DK "
{ echo "reboot 1"; echo "add dongle 7"; echo "run 100ms"; echo "save-config shell.yaml"; echo "quit"; } \
    | "$RUNNER" test two.yaml --shell > shell.out 2>&1
want "reboot and a typed add keep the type's board" \
    "$(boards shell.out)" "1:ARM/nRF52840-Dongle 2:ARM/nRF52840-DK 1:ARM/nRF52840-Dongle 7:ARM/nRF52840-Dongle "
want "the shell's save-config keeps the types" \
    "$(grep -E '^  - \{ type:' shell.yaml | sed 's/, id: \([0-9]*\).*/:\1/;s/.*type: //' | tr '\n' ' ')" "dongle:1 dk:2 dongle:7 "
# The same with both types naming the image by the same path: then nothing
# but the node's own type tells them apart.
sed 's#\./link\.nrf52840#hw.nrf52840#' two.yaml > same.yaml
"$RUNNER" test same.yaml -t 300 --save-config same-saved.yaml > same.out 2>&1
want "identical images: each node boots on its type's board" "$(boards same.out)" "1:ARM/nRF52840-Dongle 2:ARM/nRF52840-DK "
want "identical images: the save keeps each node's type" \
    "$(grep -E '^  - \{ type:' same-saved.yaml | sed 's/, id: \([0-9]*\).*/:\1/;s/.*type: //' | tr '\n' ' ')" "dongle:1 dk:2 "

echo "== a board given on the command line is saved"
cp "$ROOT/firmware/sky/hello-world.sky" hello.elf
"$CNGSIM" run hello.elf --board sky --duration 1s --save-config elf.yaml -q > /dev/null 2>&1
if "$CNGSIM" check elf.yaml > /dev/null 2>&1 && "$CNGSIM" run elf.yaml --duration 1s -q > /dev/null 2>&1
then ok "--board sky on a .elf: the save checks and runs without --board"
else bad "--board sky on a .elf: $("$CNGSIM" check elf.yaml 2>&1 | head -1)"; fi
"$RUNNER" test hw.nrf52840 --board nrf52840-dongle -t 300 --save-config variant.yaml > /dev/null 2>&1
"$RUNNER" test variant.yaml -t 300 > variant.out 2>&1
want "--board nrf52840-dongle on a .nrf52840: the reload is a dongle" "$(boards variant.out)" "1:ARM/nRF52840-Dongle "

echo "== names, meta and live clock deviations survive a save"
cp "$ROOT/firmware/sky/hello-world.sky" h.sky
cat > meta.yaml <<EOF
version: 2
mote_types:
  - { name: hw, firmware: h.sky }
nodes:
  - { type: hw, id: 1, name: first, meta: { role: root } }
  - { type: hw, id: 2, name: second, meta: { role: leaf } }
  - { type: hw, id: 3, name: third, meta: { role: spare }, clock_deviation: 0.9999 }
EOF
{ echo "remove 1"; echo "clock 3 1.00002"; echo "add hw 9"; echo "clock 9 0.99995"; echo "run 100ms";
  echo "save-config meta-saved.yaml"; echo "quit"; } | "$RUNNER" test meta.yaml --shell > meta.out 2>&1
nodes=$(grep -E '^  - \{ type:' meta-saved.yaml | sed -e 's/, x: [^,} ]*//' -e 's/, y: [^,} ]*//')
want "node 2 keeps its name and meta" "$(echo "$nodes" | grep 'id: 2')" "  - { type: hw, name: second, meta: { role: leaf }, id: 2 }"
want "node 3 keeps its name, meta and the deviation set last" \
    "$(echo "$nodes" | grep 'id: 3')" "  - { type: hw, name: third, meta: { role: spare }, id: 3, clock_deviation: 1.00002 }"
want "an added node keeps the deviation set on it" "$(echo "$nodes" | grep 'id: 9')" "  - { type: hw, id: 9, clock_deviation: 0.99995 }"
"$CNGSIM" check meta-saved.yaml > /dev/null 2>&1 && ok "the save loads" || bad "the save does not load"

echo
if [ "$fails" -eq 0 ]; then echo "check-save: OK"; else echo "check-save: $fails failed"; fi
[ "$fails" -eq 0 ]

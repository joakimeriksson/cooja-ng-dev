#!/bin/sh
# install-data.sh DEST — copy what cngsim needs besides its binary into DEST:
#
#   DEST/examples/   the commented example simulations
#   DEST/firmware/   the firmware they and the templates run (what git
#                    tracks: no local .cooja builds, no Zephyr scratch)
#   DEST/tools/      the Contiki-NG suite runner and the .csc converter
#   DEST/docs/       the user guides, CHANGELOG.md and LICENSE
#
# DEST is cngsim's data directory: <prefix>/share/cooja-ng for `make
# install`, the package root for a release tarball.  Run from anywhere.
set -eu
[ $# -eq 1 ] || { echo "usage: $0 DEST" >&2; exit 2; }
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEST=$1
mkdir -p "$DEST/examples" "$DEST/firmware" "$DEST/tools" "$DEST/docs"

cp "$ROOT"/examples/*.yaml "$DEST/examples/"

# The tracked firmware: git knows which files those are.  Outside a git
# checkout (an unpacked source tarball), everything but the two local-only
# directories.
cd "$ROOT"
if git -C "$ROOT" rev-parse --git-dir > /dev/null 2>&1; then
    git -C "$ROOT" ls-files firmware
else
    find firmware -type f ! -path 'firmware/cooja/*' ! -path 'firmware/zephyr/*'
fi | while IFS= read -r f; do
    mkdir -p "$DEST/$(dirname "$f")"
    cp "$f" "$DEST/$f"
done

for t in run-cooja-tests.sh csc2json.py build-test-firmware.sh test-border-router.sh; do
    cp "tools/$t" "$DEST/tools/"
done
mkdir -p "$DEST/docs/design"
for d in quick-start.md shell.md test-format.md ui-replay.md contiki-ng-testing.md \
         zephyr.md riot.md design/contiki-ng-testing-plan.md; do
    cp "docs/$d" "$DEST/docs/$d"
done
cp CHANGELOG.md LICENSE "$DEST/docs/"

# Every relative link in the installed guides must land on an installed
# file: a guide added to the list above can bring links to ones that are not.
# (CHANGELOG.md is the repository's record; its links name the repository.)
dangling=0
for doc in $(cd "$DEST/docs" && find . -name '*.md' ! -name CHANGELOG.md); do
    dir=$(dirname "$doc")
    for link in $(grep -o '](\([^)#]*\)' "$DEST/docs/$doc" | sed 's/^](//' | grep -v -e '^[a-z]*:' -e '^$'); do
        if [ ! -e "$DEST/docs/$dir/$link" ]; then
            echo "install-data.sh: docs/$doc links to $link, which is not installed" >&2
            dangling=1
        fi
    done
done
[ "$dangling" -eq 0 ]

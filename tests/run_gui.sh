#!/bin/sh
# CTest driver for the GUI, run offscreen with the hidden --keys / --select /
# --action / --screenshot options:
#   run_gui.sh <7zfm-qt binary> <fixtures dir> <source dir> <work dir>
set -eu
APP=$1; FX=$2; SRC=$3; WORK=$4
"$SRC/tests/make_fixtures.sh" "$FX" >/dev/null 2>&1 || "$SRC/tests/make_fixtures.sh" "$FX"
rm -rf "$WORK" && mkdir -p "$WORK/out"
export QT_QPA_PLATFORM=offscreen XDG_CONFIG_HOME="$WORK/cfg" XDG_CACHE_HOME="$WORK/cache"
fail=0
check() { if "$@"; then echo "PASS: $DESC"; else echo "FAIL: $DESC"; fail=1; fi; }
run() { timeout 60 "$APP" "$@" >/dev/null 2>&1; }

DESC="GUI: Ctrl+PgDn into the 20 GiB ext4 partition of a VHDX"
check run "$FX/disk.vhdx" --select 1.linux.img --keys "Ctrl+PgDown" --screenshot "$WORK/s1.png"
check test -s "$WORK/s1.png"

DESC="GUI: stored zip -> VHDX falls back to extraction and opens"
check run "$FX/vhdx-in-zip.zip" --select disk.vhdx --keys "Ctrl+PgDown" --screenshot "$WORK/s2.png"

DESC="GUI: F5 copy-out from xz -> tar -> FAT"
cp -r "$FX/small-fat.tar.xz" "$WORK/out/"
check run "$WORK/out/small-fat.tar.xz" --keys "Ctrl+PgDown,Ctrl+PgDown,F5,Return" --screenshot "$WORK/s3.png"
check cmp -s "$WORK/out/FATFILE.TXT" "$FX/fatcontent/FATFILE.TXT"

DESC="GUI: Extract dialog extracts small-fat.tar.xz into a subfolder"
check run "$WORK/out/" --select small-fat.tar.xz --action extract --keys "Return" --screenshot "$WORK/s4.png"
check sh -c "xz -dc '$FX/small-fat.tar.xz' | cmp -s - '$WORK/out/small-fat.tar/small-fat.tar'"

DESC="GUI: temp folders are removed on exit"
check sh -c "[ -z \"\$(find '$WORK/cache' -mindepth 2 -maxdepth 3 -name '7zO*' 2>/dev/null)\" ]"
exit $fail

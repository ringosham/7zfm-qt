#!/bin/sh
# CTest driver: builds fixtures, then checks in-place nested opening.
#   run_smoke.sh <core_smoke binary> <fixtures dir> <source dir>
set -eu
S=$1; FX=$2; SRC=$3
"$SRC/tests/make_fixtures.sh" "$FX" >/dev/null 2>&1 || "$SRC/tests/make_fixtures.sh" "$FX"
cd "$FX"
fail=0
check() { if "$@"; then echo "PASS: $DESC"; else echo "FAIL: $DESC"; fail=1; fi; }

DESC="Rar and Rar5 handlers are registered"
check sh -c "'$S' formats | grep -qx Rar && '$S' formats | grep -qx Rar5"

for img in disk.img disk.vhdx disk.vmdk disk.qcow2; do
  DESC="$img lists GPT partitions"
  check sh -c "'$S' ls $img | grep -q '0.fat.fat' && '$S' ls $img | grep -q '1.linux.img'"
  DESC="$img: ext4 file read in place is byte-identical"
  check sh -c "'$S' cat $img 1.linux.img/docs/random.bin | cmp -s - content/docs/random.bin"
  DESC="$img: FAT32 file read in place"
  check sh -c "'$S' cat $img 0.fat.fat/FATFILE.TXT | cmp -s - fatcontent/FATFILE.TXT"
done

DESC="deep path inside ext4 inside VHDX"
check sh -c "'$S' cat disk.vhdx 1.linux.img/deep/a/b/c/leaf.txt | cmp -s - content/deep/a/b/c/leaf.txt"

# The PeaZip regression: opening a ~20 GiB partition must not touch its data.
DESC="opening the 20 GiB ext4 partition inside VHDX takes < 1 s"
check sh -c "s=\$(date +%s%N); '$S' ls disk.vhdx 1.linux.img >/dev/null; e=\$(date +%s%N); [ \$(( (e - s) / 1000000 )) -lt 1000 ]"

# Zip has no IInArchiveGetStream upstream either; 7zFM extracts such items
# (to RAM when small). core_smoke reports that as exit code 3.
DESC="stored zip entry has no in-place stream (same as Windows 7zFM)"
check sh -c "'$S' ls vhdx-in-zip.zip disk.vhdx >/dev/null 2>&1; [ \$? -eq 3 ]"

DESC="xz -> tar -> FAT chain opens in place"
check sh -c "'$S' cat small-fat.tar.xz small-fat.tar/small-fat.img/FATFILE.TXT | cmp -s - fatcontent/FATFILE.TXT"

# RAR: listing and full extraction through the Agent, compared with the
# source tree and with upstream 7zz (built from the same sources).
# test4.rar is store-only (rar 7.x cannot write RAR4); the rest need 'rar'.
SEVENZZ=$(dirname "$S")/7zz
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
rar_x() {  # rar_x <archive> [password]: Agent and 7zz extraction match content/
  rm -rf "$T/a" "$T/z"
  CORE_SMOKE_PASSWORD=${2:-} "$S" x "$1" "$T/a" >/dev/null || return 1
  "$SEVENZZ" x -y -p"${2:-}" -o"$T/z" "$1" >/dev/null || return 1
  diff -r content "$T/a" >/dev/null && diff -r "$T/z" "$T/a" >/dev/null
}
rar_ls() {
  "$S" ls "$@" > "$T/ls" && grep -qx 'F	30	hello.txt' "$T/ls" && grep -q '^D.*	docs$' "$T/ls"
}
rar_type() { "$SEVENZZ" l -slt "$1" | grep -qx "Type = $2"; }

DESC="RAR4 (generated, stored) is opened by the Rar handler"
check rar_type test4.rar Rar
DESC="RAR4 listing"
check rar_ls test4.rar
DESC="RAR4 extraction"
check rar_x test4.rar

if [ -f test5.rar ]; then
  DESC="RAR5 is opened by the Rar5 handler"
  check rar_type test5.rar Rar5
  for a in test5.rar solid.rar vol.part1.rar; do
    DESC="$a listing"
    check rar_ls $a
    DESC="$a extraction"
    check rar_x $a
  done
  DESC="solid.rar is solid"
  check sh -c "'$SEVENZZ' l -slt solid.rar | grep -qx 'Solid = +'"
  DESC="vol.part1.rar opens all 3 volumes"
  check sh -c "'$SEVENZZ' l vol.part1.rar | grep -qx 'Volumes = 3' && '$S' ls vol.part1.rar docs | grep -qx 'F	3000000	random.bin'"

  DESC="encrypted headers: no password -> open fails"
  check sh -c "! '$S' ls enc.rar >/dev/null 2>&1"
  DESC="encrypted headers: wrong password -> open fails"
  check sh -c "! CORE_SMOKE_PASSWORD=wrong '$S' ls enc.rar >/dev/null 2>&1"
  DESC="encrypted headers: listing with password"
  check sh -c "CORE_SMOKE_PASSWORD=secret '$S' ls enc.rar | grep -qx 'F	30	hello.txt'"
  DESC="encrypted headers: extraction with password"
  check rar_x enc.rar secret
  DESC="encrypted data: listed without password"
  check rar_ls encdata.rar
  DESC="encrypted data: extraction without password fails"
  check sh -c "rm -rf '$T/n'; ! '$S' x encdata.rar '$T/n' >/dev/null 2>&1"
  DESC="encrypted data: wrong password reports errors"
  check sh -c "rm -rf '$T/n'; ! CORE_SMOKE_PASSWORD=wrong '$S' x encdata.rar '$T/n' >/dev/null 2>&1"
  DESC="encrypted data: extraction with password"
  check rar_x encdata.rar secret
else
  echo "SKIP: RAR5 fixtures ('rar' not installed)"
fi
exit $fail

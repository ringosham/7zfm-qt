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

if [ -f test5.rar ]; then
  DESC="RAR5 listing"
  check sh -c "'$S' ls test5.rar | grep -q hello.txt"
else
  echo "SKIP: RAR fixtures ('rar' not installed)"
fi
exit $fail

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

# The registry (kNumArcsMax in src/core/LoadCodecs.cpp) drops formats past
# its size without a word: when it is full, double it
DESC="the format registry has room (else double kNumArcsMax in src/core/LoadCodecs.cpp)"
# (Hash is listed too, but isn't one of them)
check sh -c "[ \$('$S' formats | grep -vx Hash | wc -l) -lt 144 ]"

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

# Raw CD images (src/formats/CdImage.cpp), with the FAT image as data track
python3 -I "$SRC/tests/make_cdimage.py" small-fat.img cd
DESC="CD .bin + .cue lists its tracks"
check sh -c "'$S' ls cd/cd.bin | cut -f3 | tr '\\n' ' ' | grep -qx 'Track01.iso Track02.wav Track03.wav '"
DESC="CD data track opens in place"
check sh -c "'$S' cat cd/cd.bin Track01.iso/FATFILE.TXT | cmp -s - fatcontent/FATFILE.TXT"
DESC="CD audio tracks read as WAV, pregap left out"
check sh -c "'$S' cat cd/cd.bin Track02.wav | tail -c +45 | cmp -s - cd/cd-track2.pcm && '$S' cat cd/cd.bin Track03.wav | tail -c +45 | cmp -s - cd/cd-track3.pcm"
DESC="MODE2 CD with no .cue opens straight to its files"
check sh -c "'$S' ls cd/mode2.bin | grep -q FATFILE.TXT"
DESC="CD with no .cue: audio found after the data track"
check sh -c "'$S' ls cd/bad.bin | cut -f3 | tr '\\n' ' ' | grep -qx 'Track01.iso Track02.wav '"
DESC="CD: a damaged data sector fails its EDC check"
check sh -c "rm -rf cd/x && '$S' x cd/bad.bin cd/x 2>&1 | grep -q 'Track01.iso: operation result 3'"
DESC="CD: an intact image extracts without errors"
check sh -c "rm -rf cd/x && '$S' x cd/cd.bin cd/x >/dev/null"

# The other disc image formats (src/formats/disc), from the same CD
python3 -I "$SRC/tests/make_discimages.py" small-fat.img cd
for img in cd/nrg.nrg cd/cdi.cdi cd/mds.mds cd/mds.mdf cd/gdi.gdi cd/ccd.ccd cd/ccd.img cd/multi.cue; do
  DESC="$img lists its tracks"
  check sh -c "'$S' ls '$img' | cut -f3 | tr '\\n' ' ' | grep -qx 'Track01.iso Track02.wav Track03.wav '"
  DESC="$img: data track opens in place"
  check sh -c "'$S' cat '$img' Track01.iso/FATFILE.TXT | cmp -s - fatcontent/FATFILE.TXT"
  DESC="$img: audio tracks read as WAV"
  check sh -c "'$S' cat '$img' Track02.wav | tail -c +45 | cmp -s - cd/cd-track2.pcm && '$S' cat '$img' Track03.wav | tail -c +45 | cmp -s - cd/cd-track3.pcm"
done
for img in cd/cso.cso cd/zso.zso cd/gc.ciso cd/isz.isz; do
  DESC="$img opens at its files"
  check sh -c "'$S' cat '$img' FATFILE.TXT | cmp -s - fatcontent/FATFILE.TXT"
done
DESC="ECM: sectors rebuilt with their EDC and ECC"
# the output isn't sector-aligned after its literal bytes: past the data
# track, a CD reader sees the rest as audio, whole sectors of it
n=$(( $(stat -c %s cd/ecm.bin) / 2352 - 40 ))
tail -c +$((40 * 2352 + 1)) cd/ecm.bin | head -c $((n * 2352)) > cd/ecm-rest.expected
check sh -c "'$S' cat cd/ecm.bin.ecm Track01.iso | cmp -s -n 81920 - small-fat.img && '$S' cat cd/ecm.bin.ecm Track02.wav | tail -c +45 | cmp -s - cd/ecm-rest.expected"
DESC="zisofs file decompresses"
check sh -c "'$S' cat cd/file.zisofs file.zisofs | cmp -s - cd/file.plain"
# CHDs made by chdman (tests/data/README.md)
DESC="CHD CD (cdlz, cdzl, cdfl): tracks, data and audio"
check sh -c "'$S' cat '$SRC/tests/data/cd.chd' Track01.iso/HDFILE.TXT | grep -qx 'hello from a CHD hard disk' && '$S' cat '$SRC/tests/data/cd.chd' Track02.wav | tail -c +45 | cmp -s - cd/cd-track2.pcm && '$S' cat '$SRC/tests/data/cd.chd' Track03.wav | tail -c +45 | cmp -s - cd/cd-track3.pcm"
seq 1 100000 > cd/numbers.txt
for chd in dvd dvd-zstd; do
  DESC="CHD $chd opens at its files"
  check sh -c "'$S' cat '$SRC/tests/data/$chd.chd' numbers.txt | cmp -s - cd/numbers.txt"
done
DESC="CHD hard disk opens at its file system"
check sh -c "'$S' cat '$SRC/tests/data/hd.chd' HDFILE.TXT | grep -qx 'hello from a CHD hard disk'"

# exFAT (src/formats/ExFatHandler.cpp): an image written by Linux's exfat
# driver, with long and non-BMP names, FAT chains and NoFatChain files, a
# file preallocated past its valid length (tests/data/README.md)
xz -dc "$SRC/tests/data/exfat.img.xz" > exfat.img
DESC="exFAT: every file extracts byte-identical"
check sh -c "rm -rf exfat-out && '$S' x exfat.img exfat-out >/dev/null && cd exfat-out && sha256sum -c --quiet '$SRC/tests/data/exfat.sha256' && test -d 'empty dir'"
DESC="exFAT partition of an MBR disk opens in place"
rm -f exfat-disk.img && truncate -s 10M exfat-disk.img
printf 'label: dos\nstart=2048, type=7\n' | sfdisk -q exfat-disk.img
dd if=exfat.img of=exfat-disk.img bs=512 seek=2048 conv=notrunc status=none
check sh -c "'$S' cat exfat-disk.img deep/a/b/c/leaf.txt | grep -qx deep"

# lzip (src/formats/LzipHandler.cpp), files made by lzip 1.25
seq 1 30000 > lzip-seq.txt
for f in seq multi; do
  DESC="lzip: $f.txt.lz decompresses"
  check sh -c "rm -rf lzip-out && '$S' x '$SRC/tests/data/lzip/$f.txt.lz' lzip-out >/dev/null && cmp -s lzip-out/$f.txt lzip-seq.txt"
done
DESC="lzip: an empty file decompresses"
check sh -c "rm -rf lzip-out && '$S' x '$SRC/tests/data/lzip/empty.lz' lzip-out >/dev/null && test -f lzip-out/empty && ! test -s lzip-out/empty"
DESC="lzip: a damaged file is an error"
python3 -I -c 'import sys; b = bytearray(open(sys.argv[1], "rb").read()); b[len(b) // 2] ^= 0x10; open(sys.argv[2], "wb").write(b)' "$SRC/tests/data/lzip/seq.txt.lz" lzip-bad.txt.lz
check sh -c "rm -rf lzip-out; '$S' x lzip-bad.txt.lz lzip-out >/dev/null 2>&1; [ \$? -ne 0 ]"

# zip paths with '\' (src/formats/WinPathZip.cpp): folders when made on
# Windows, part of the name when made on Unix
python3 -I -c '
import sys, zipfile
def add(z, name, data, system):
    i = zipfile.ZipInfo(name); i.create_system = system
    i.external_attr = 0x20 if system == 0 else (0o644 << 16)
    z.writestr(i, data)
with zipfile.ZipFile(sys.argv[1], "w") as z:
    add(z, "dir\\sub\\file.txt", b"win\n", 0)
    add(z, "dir\\other.txt", b"win2\n", 0)
with zipfile.ZipFile(sys.argv[2], "w") as z:
    add(z, "weird\\name.txt", b"unix\n", 3)
' winpath.zip unixpath.zip
DESC="zip made on Windows: '\\' separates folders"
check sh -c "'$S' ls winpath.zip dir | cut -f3 | tr '\\n' ' ' | grep -qx 'sub other.txt '"
DESC="zip made on Windows: extracts into folders"
check sh -c "rm -rf winpath-out && '$S' x winpath.zip winpath-out >/dev/null && grep -qx win winpath-out/dir/sub/file.txt"
DESC="zip made on Unix: '\\' stays part of the name"
check sh -c "'$S' ls unixpath.zip | cut -f3 | grep -qxF 'weird\\name.txt'"

# names Windows can't store (src/core/WinFsNames.cpp): kept on Linux file
# systems, replaced as Windows 7-Zip does on FAT, exFAT, NTFS and SMB
python3 -I -c '
import sys, zipfile
with zipfile.ZipFile(sys.argv[1], "w") as z:
    for n in ["weird\\name.txt", "a:b.txt", "q?.txt", "CON.txt", "trail.", "dir:x/inner.txt"]:
        i = zipfile.ZipInfo(n); i.create_system = 3; i.external_attr = 0o644 << 16
        z.writestr(i, b"x\n")
' winnames.zip
DESC="Linux destination: names kept"
check sh -c "rm -rf winnames-posix && Z7QT_WINDOWS_NAMES=0 '$S' x winnames.zip winnames-posix >/dev/null && test -f 'winnames-posix/a:b.txt' && test -f 'winnames-posix/trail.' && test -f 'winnames-posix/dir:x/inner.txt'"
DESC="Windows destination: names made storable, as Windows 7-Zip does"
check sh -c "rm -rf winnames-win && Z7QT_WINDOWS_NAMES=1 '$S' x winnames.zip winnames-win >/dev/null && cd winnames-win && test \"\$(find . -type f | LC_ALL=C sort | tr '\\n' ' ')\" = './_CON.txt ./a_b.txt ./dir_x/inner.txt ./q_.txt ./trail_ ./weird_name.txt '"
DESC="FAT and SMB mounts keep Windows' rules"
check sh -c "awk '{for (i = 1; i <= NF; i++) if (\$i == \"-\") { print \$5, \$(i + 1); break }}' /proc/self/mountinfo | while read -r m t; do case \$t in vfat|exfat|ntfs3|cifs) [ \"\$('$S' fstype \"\$m\")\" = windows ] || exit 1;; ext4|tmpfs|btrfs|xfs) [ \"\$('$S' fstype \"\$m\")\" = posix ] || exit 1;; esac; done"

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

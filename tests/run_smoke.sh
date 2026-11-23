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

# Game archives (src/formats/game), made by tests/make_gamearchives.py: each
# holds the files in tests/data/game/tree
G="$SRC/tests/data/game"
same_tree() {  # same_tree <archive> [password]
  rm -rf "$T/g"
  if [ $# -ge 2 ]; then
    CORE_SMOKE_PASSWORD=$2 "$S" x "$1" "$T/g" >/dev/null 2>&1 || return 1
  else
    "$S" x "$1" "$T/g" >/dev/null 2>&1 || return 1
  fi
  diff -r "$G/tree" "$T/g" >/dev/null
}
GODOT_KEY=000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f
for f in godot3.pck godot4.pck godot44.pck godot4-game.exe; do
  DESC="Godot: $f extracts"
  check same_tree "$G/$f"
done
DESC="Godot: encrypted pack with its key"
check same_tree "$G/godot4-enc.pck" $GODOT_KEY
DESC="Godot: encrypted pack without a key doesn't open"
check sh -c "! '$S' ls '$G/godot4-enc.pck' >/dev/null 2>&1"
DESC="Godot: encrypted pack with a wrong key doesn't open"
check sh -c "! CORE_SMOKE_PASSWORD=ff$(echo $GODOT_KEY | cut -c3-) '$S' ls '$G/godot4-enc.pck' >/dev/null 2>&1"

for f in renpy3.rpa renpy3-p5.rpa renpy2.rpa renpy1.rpi; do
  DESC="Ren'Py: $f extracts"
  check same_tree "$G/$f"
done

for f in Game.rgssad Game.rgss3a; do
  DESC="RPG Maker: $f extracts"
  check same_tree "$G/$f"
done
DESC="RPG Maker MV: an image decrypts (key from its PNG header)"
check sh -c "'$S' cat '$G/mv/www/img/pictures/pic.rpgmvp' pic.png | cmp -s - '$G/tree-png.png'"
DESC="RPG Maker MZ: an image decrypts (key from its PNG header)"
check sh -c "'$S' cat '$G/mv-nokey/pic.png_' pic.png | cmp -s - '$G/tree-png.png'"
DESC="RPG Maker MZ: audio with the key typed in"
check sh -c "CORE_SMOKE_PASSWORD=d41d8cd98f00b204e9800998ecf8427e '$S' cat '$G/mv-nokey/song.ogg_' song.ogg | cmp -s - '$G/tree-ogg.ogg'"
DESC="RPG Maker MZ: audio without a key doesn't open"
check sh -c "! '$S' ls '$G/mv-nokey/song.ogg_' >/dev/null 2>&1"

# DxLib archives (src/formats/game/DxArchive.cpp), WOLF RPG Editor's .wolf:
# the key read back from the header (versions 6 and 1), DxLib's default key,
# a password; packed files, and a Shift-JIS name
dx_tree() {  # dx_tree <archive> [password]
  rm -rf "$T/dx"
  CORE_SMOKE_PASSWORD=${2:-} "$S" x "$1" "$T/dx" >/dev/null 2>&1 || return 1
  printf 'WOLF RPG \343\202\250\343\203\207\343\202\243\343\202\277\343\203\274\n' | cmp -s - "$T/dx/日本語.txt" || return 1
  rm "$T/dx/日本語.txt" && diff -r "$G/tree" "$T/dx" >/dev/null
}
for f in dxlib-v6.wolf dxlib-v1.dxa dxlib-v4-default.dxa; do
  DESC="DxLib: $f extracts"
  check dx_tree "$G/$f"
done
DESC="DxLib: an archive with a password extracts with it"
check dx_tree "$G/dxlib-v2-password.wolf" wolfpass
DESC="DxLib: an archive with a password doesn't open with a wrong one"
check sh -c "! CORE_SMOKE_PASSWORD=wrong '$S' ls '$G/dxlib-v2-password.wolf' >/dev/null 2>&1"

for f in kirikiri.xp3 kirikiri-plain.xp3 kirikiriz.xp3 kirikiri-game.exe; do
  DESC="KiriKiri: $f extracts"
  check same_tree "$G/$f"
done
DESC="KiriKiri: a damaged file fails its Adler-32"
python3 -I -c 'import sys; d=bytearray(open(sys.argv[1],"rb").read()); i=d.find(bytes(range(40,60))); d[i]^=0xFF; open(sys.argv[2],"wb").write(d)' "$G/kirikiri.xp3" "$T/bad.xp3"
check sh -c "rm -rf '$T/b'; ! '$S' x '$T/bad.xp3' '$T/b' >/dev/null 2>&1"

for f in morrowind.bsa oblivion.bsa skyrim.bsa skyrim-stored.bsa skyrimse.bsa fallout4.ba2 starfield.ba2; do
  DESC="Bethesda: $f extracts"
  check same_tree "$G/$f"
done
DESC="Bethesda: a BA2 texture gets its DDS header back"
check sh -c "'$S' cat '$G/fallout4-textures.ba2' textures/stone.dds | cmp -s - '$G/tree-stone.dds'"
for t in normal_n sky bc7; do
  DESC="Bethesda: BA2 texture $t.dds gets DirectXTex's header (legacy BC5, an RGBA8 cubemap, DX10 for BC7)"
  check sh -c "'$S' cat '$G/fallout4-formats.ba2' textures/$t.dds | cmp -s - '$G/tree-$t.dds'"
done

for f in unity-lz4.unity3d unity-lzma.unity3d unity/sharedassets0.assets; do
  DESC="Unity: $f gives its TextAsset"
  check sh -c "'$S' cat '$G/$f' TextAsset/readme.txt | cmp -s - '$G/tree/readme.txt'"
  DESC="Unity: $f gives its texture as TGA (from the .resS)"
  check sh -c "'$S' cat '$G/$f' Texture2D/pixels.tga | cmp -s - '$G/tree-pixels.tga'"
done
DESC="Unity: a bundle's serialized file opens by its header"
check sh -c "'$S' ls '$G/unity-lz4.unity3d' Files/CAB-0123456789abcdef | grep -q TextAsset"

# made by repak (trumank/repak): every pak version, each compression it writes
for f in unreal-v1.pak unreal-v3-zlib.pak unreal-v5-zlib.pak unreal-v7-gzip.pak unreal-v8a-zlib.pak \
    unreal-v8b-zlib.pak unreal-v9-zstd.pak unreal-v11.pak unreal-v11-zlib.pak unreal-v11-lz4.pak; do
  DESC="Unreal: $f extracts"
  check same_tree "$G/$f"
done

# Oodle blocks (src/formats/game/Oodle.cpp), cut from oozextract's test
# streams: each codec (Kraken, Mermaid, Selkie, Leviathan, LZNA, Bitknit,
# LZB16), restarts, a one-symbol Huffman table and a file of two blocks;
# version 3 names Oodle by flag 4
for f in unreal-v8b-oodle.pak unreal-v3-oodle.pak; do
  DESC="Unreal: $f's Oodle blocks decode"
  check sh -c "rm -rf '$T/o' && '$S' x '$G/$f' '$T/o' >/dev/null 2>&1 && cd '$T/o' && for x in \$(find . -type f | sed 's|^[.]/||'); do grep -q \"  \$x\$\" '$G/unreal-oodle.sha256' || exit 1; done && sha256sum -c --ignore-missing --quiet '$G/unreal-oodle.sha256'"
done
DESC="Unreal: a damaged Oodle block is a data error"
check sh -c "cp '$G/unreal-v8b-oodle.pak' '$T/bad.pak' && printf '\\377\\377\\377\\377' | dd of='$T/bad.pak' bs=1 seek=300 conv=notrunc 2>/dev/null && '$S' t '$T/bad.pak' 2>&1 | grep -q 'errors: [1-9]'"

DESC="Inno Setup: a 6.0 installer (LZMA and stored chunks) extracts, checksums checked"
check sh -c "rm -rf '$T/inno'; '$S' x '$G/inno6-setup.exe' '$T/inno' >/dev/null 2>&1 && diff -r '$G/tree' '$T/inno/app' >/dev/null"
DESC="core_smoke t tests without writing"
check sh -c "'$S' t '$G/inno6-setup.exe' | grep -q 'errors: 0'"

DESC="InstallShield: a version 6 cabinet (deflate and obfuscated files, a chain of links) extracts, MD5s checked"
check same_tree "$G/installshield/data1.cab"
for f in data1.cab data2.cab; do
  DESC="InstallShield: version 5 cabinets with whole deflate streams, a file split over them ($f)"
  check same_tree "$G/installshield5/$f"
done

# InstallShield setup.exe (src/formats/installer/ISSetup.cpp): the files
# after the launcher, plain, XORed both ways and compressed, in both record
# layouts, and InstallShield 10.5's text records (with a CodeView block first)
for f in issetup-plain.exe issetup-xor.exe issetup-stream.exe issetup-stream4.exe issetup-text.exe issetup-text-nb10.exe; do
  DESC="InstallShield setup: $f extracts"
  check same_tree "$G/$f"
done

# InstallShield 3 (src/formats/installer/InstallShield3.cpp): made from the
# tree by InstallShield 3.0's ICOMP.EXE, the split sets by its SPLIT.EXE, with
# a file of their own (noise.bin stored, words.txt compressed) running over
# the volumes
I3="$G/installshield3"
for f in tree.z tree-fast.z tree-stored.z; do
  DESC="InstallShield 3: $f extracts"
  check same_tree "$I3/$f"
done
for f in stored.1:noise.bin stored.3:noise.bin stored.5:noise.bin packed.1:words.txt packed.2:words.txt; do
  DESC="InstallShield 3: the split set ${f%%:*} belongs to extracts"
  check sh -c "rm -rf '$T/g'; '$S' x '$I3/${f%%:*}' '$T/g' >/dev/null 2>&1 && cd '$T/g' && grep '${f#*:}' '$I3/extra.sha256' | sha256sum -c >/dev/null && rm '${f#*:}' && diff -r '$G/tree' '$T/g' >/dev/null"
done
DESC="InstallShield 3: a volume without the rest of its set opens with its own files"
check sh -c "rm -rf '$T/i3' && mkdir -p '$T/i3' && cp '$I3/stored.3' '$T/i3/' && '$S' t '$T/i3/stored.3' | grep -q 'errors: 0'"

# PowerISO DAA (src/formats/disc/DiscPowerIso.cpp): made by PowerISO 1.6 for
# Linux (version 0x100) and PowerISO 9.5's piso.exe (0x110, split in 1 MB parts)
DA="$SRC/tests/data/daa"
for f in tree-v100.daa tree-v110.daa; do
  DESC="DAA: $f gives its ISO's files"
  check sh -c "rm -rf '$T/d'; '$S' x '$DA/$f' '$T/d' >/dev/null 2>&1 && diff -r '$G/tree' '$T/d' >/dev/null"
done
DESC="DAA: a split image (.part01.daa, .part02.daa)"
check sh -c "rm -rf '$T/d'; '$S' x '$DA/split.part01.daa' '$T/d' >/dev/null 2>&1 && cd '$T/d' && sha256sum -c '$DA/noise.sha256' >/dev/null"
DESC="DAA: a split image holds split.iso"
check sh -c "CORE_SMOKE_FORMAT='*' '$S' ls '$DA/split.part01.daa' | grep -q '	split.iso$'"
# gBurner GBI: made by gBurner 5.6 (its Convert Image, under wine) from an ISO
# of the tree; tree-password.daa by PowerISO's piso.exe
for f in tree.gbi tree-best.gbi; do
  DESC="GBI: $f gives its ISO's files"
  check sh -c "rm -rf '$T/d'; '$S' x '$DA/$f' '$T/d' >/dev/null 2>&1 && diff -r '$G/tree' '$T/d' >/dev/null"
done
DESC="GBI: a split image (.part01.gbi ...)"
check sh -c "rm -rf '$T/d'; '$S' x '$DA/vol.part01.gbi' '$T/d' >/dev/null 2>&1 && cd '$T/d' && sha256sum -c '$DA/vol-noise.sha256' >/dev/null"
for f in tree-password.gbi:secret "tree-password.daa:Zz9!long password here"; do
  DESC="DAA/GBI: ${f%%:*} with its password"
  check sh -c "rm -rf '$T/d'; CORE_SMOKE_PASSWORD='${f#*:}' '$S' x '$DA/${f%%:*}' '$T/d' >/dev/null 2>&1 && diff -r '$G/tree' '$T/d' >/dev/null"
  DESC="DAA/GBI: ${f%%:*} with a wrong password fails"
  check sh -c "! CORE_SMOKE_PASSWORD=wrong '$S' ls '$DA/${f%%:*}' >/dev/null 2>&1"
done

# MagicISO UIF (src/formats/disc/DiscUif.cpp): written by make_gamearchives.py
# (uif2iso gives the same ISO from each); checked by hand against images made
# by MagicISO 5.5, plain and with a password
for f in magiciso-v1.uif magiciso-v1-password.uif magiciso-v3-lzma-key.uif; do
  DESC="UIF: $f gives its ISO's files"
  check sh -c "rm -rf '$T/d'; CORE_SMOKE_PASSWORD=abcdef '$S' x '$G/$f' '$T/d' >/dev/null 2>&1 && diff -r '$G/tree' '$T/d' >/dev/null"
done
DESC="UIF: the image itself, by Open Inside *"
check sh -c "CORE_SMOKE_FORMAT='*' '$S' ls '$G/magiciso-v1.uif' | grep -q 'magiciso-v1.iso'"
DESC="UIF: a wrong password fails"
check sh -c "! CORE_SMOKE_PASSWORD=wrong '$S' ls '$G/magiciso-v1-password.uif' >/dev/null 2>&1"
# Mixed-mode images, made by MagicISO 5.5's "Compress CD/DVD Image" from the
# CD fixtures (.cue with audio tracks, .nrg, .mds; one with a password): the
# image file and its sheet come back as they were
for f in cue:abc nrg:abc mds:abc password:abcdef; do
  DESC="UIF: the mixed-mode image magiciso-mixed-${f%%:*}.uif gives its image file and sheet"
  check sh -c "rm -rf '$T/u' && mkdir -p '$T/u' && CORE_SMOKE_FORMAT='*' CORE_SMOKE_PASSWORD='${f#*:}' '$S' x '$G/magiciso-mixed-${f%%:*}.uif' '$T/u' >/dev/null 2>&1 && cd '$T/u' && for x in *; do grep -q \"  \$x\$\" '$G/magiciso-mixed.sha256' || exit 1; done && sha256sum -c --ignore-missing --quiet '$G/magiciso-mixed.sha256'"
done
DESC="UIF: a mixed-mode image opens at its tracks"
check sh -c "'$S' ls '$G/magiciso-mixed-cue.uif' | grep -q 'Track02.wav'"

# ISO 9660 updates (src/formats/iso/IsoUpdate.cpp): add, delete and rename in
# images xorriso made (tests/make_isos.sh); upstream 7zz and, if installed,
# libarchive's bsdtar read the results
I="$SRC/tests/data/iso"
mkdir -p "$T/isoadd"
echo "new file" > "$T/isoadd/A new file with a long name.txt"
echo dos > "$T/isoadd/dos.txt"
LONG=$(printf 'L%.0s' $(seq 1 150))" long Rock Ridge name ü.txt"
echo long > "$T/isoadd/$LONG"
iso_update() {  # iso_update <image> <copy> <docs> <sub> <readme>: the changes
  cp "$I/$1" "$T/$2"
  "$S" add "$T/$2" "" "$T/isoadd/A new file with a long name.txt" >/dev/null 2>&1 &&
  "$S" add "$T/$2" "$4" "$T/isoadd/dos.txt" >/dev/null 2>&1 &&
  "$S" del "$T/$2" "$3" >/dev/null 2>&1 &&
  "$S" ren "$T/$2" "$5" "Read Me.txt" >/dev/null 2>&1
}
DESC="ISO: add, delete and rename in a plain ISO 9660 image (8.3 names)"
check iso_update plain.iso u-plain.iso DOCS/NUMBERS.TXT DOCS/SUB README.TXT
DESC="ISO: the plain image's new names are 8.3, contents read back by 7zz"
check sh -c "'$S' ls '$T/u-plain.iso' | cut -f3 | tr '\n' ' ' | grep -qx 'BOOT DOCS A_NEW_FI.TXT READ_ME.TXT ' && '$SEVENZZ' e -so '$T/u-plain.iso' DOCS/SUB/DOS.TXT 2>/dev/null | grep -qx dos && '$SEVENZZ' t '$T/u-plain.iso' >/dev/null"
for f in rr boot; do
  DESC="ISO: add, delete and rename in $f.iso (Rock Ridge and Joliet)"
  check iso_update $f.iso u-$f.iso docs/numbers.txt docs/sub readme.txt
  DESC="ISO: $f.iso's changes read back by us and by 7zz (Joliet), the rest kept"
  check sh -c "'$S' cat '$T/u-$f.iso' 'A new file with a long name.txt' | grep -qx 'new file' && '$S' cat '$T/u-$f.iso' 'Read Me.txt' | grep -qx hello && ! '$S' ls '$T/u-$f.iso' docs | grep -q numbers && '$SEVENZZ' e -so '$T/u-$f.iso' docs/sub/dos.txt 2>/dev/null | grep -qx dos && '$SEVENZZ' e -so '$T/u-$f.iso' docs/sub/deep.txt 2>/dev/null | grep -qx deep && '$SEVENZZ' t '$T/u-$f.iso' >/dev/null"
  DESC="ISO: $f.iso keeps its Rock Ridge symlink and modes"
  check sh -c "'$S' ls '$T/u-$f.iso' docs | grep -q 'link'"
  if command -v bsdtar >/dev/null 2>&1; then
    DESC="ISO: libarchive reads $f.iso's update without errors (Rock Ridge, then Joliet)"
    check sh -c "bsdtar tf '$T/u-$f.iso' 2>&1 | LC_ALL=C sort | tr '\n' ' ' | grep -qx '. A new file with a long name.txt Read Me.txt boot boot/boot.img docs docs/link docs/sub docs/sub/deep.txt docs/sub/dos.txt ' || bsdtar tf '$T/u-$f.iso' 2>&1 | grep -v boot.cat | LC_ALL=C sort | tr '\n' ' ' | grep -qx '. A new file with a long name.txt Read Me.txt boot boot/boot.img docs docs/link docs/sub docs/sub/deep.txt docs/sub/dos.txt '"
    check sh -c "bsdtar --options 'iso9660:!rockridge' -tf '$T/u-$f.iso' 2>&1 | grep -qx 'A new file with a long name.txt'"
  fi
done
DESC="ISO: a bootable image keeps its boot record, catalog and boot image byte for byte"
check python3 -I -c 'import sys
a, b = (open(f, "rb").read() for f in sys.argv[1:3])
cat = int.from_bytes(a[17 * 2048 + 0x47:17 * 2048 + 0x4B], "little")
img = int.from_bytes(a[cat * 2048 + 32 + 8:cat * 2048 + 32 + 12], "little")
same = lambda s, n=1: a[s * 2048:(s + n) * 2048] == b[s * 2048:(s + n) * 2048]
sys.exit(0 if same(17) and same(cat) and same(img, 1) else 1)' "$I/boot.iso" "$T/u-boot.iso"
DESC="ISO: the hybrid image's partition (its own trees) shows the changes, its MBR entry grew"
check sh -c "dd if='$T/u-boot.iso' of='$T/u-boot-part.iso' bs=2048 skip=16 status=none && '$S' cat '$T/u-boot-part.iso' docs/sub/dos.txt | grep -qx dos && python3 -I -c 'import sys
d = open(sys.argv[1], \"rb\").read()
e = d[446:462]
sys.exit(0 if int.from_bytes(e[8:12], \"little\") + int.from_bytes(e[12:16], \"little\") == len(d) // 512 else 1)' '$T/u-boot.iso'"
DESC="ISO: a long Rock Ridge name (continuation area) survives two updates"
check sh -c "cp '$I/rr.iso' '$T/long.iso' && '$S' add '$T/long.iso' '' '$T/isoadd/$LONG' >/dev/null 2>&1 && '$S' add '$T/long.iso' '' '$T/isoadd/dos.txt' >/dev/null 2>&1 && '$S' cat '$T/long.iso' '$LONG' | grep -qx long"
DESC="ISO: an image with UDF isn't changed (read only)"
check sh -c "cp '$I/udf.iso' '$T/u-udf.iso' && ! '$S' add '$T/u-udf.iso' '' '$T/isoadd/dos.txt' >/dev/null 2>&1 && cmp -s '$I/udf.iso' '$T/u-udf.iso'"

# FAT updates (src/formats/fat/FatUpdate.cpp): FAT12, FAT16 and FAT32 images
# that mkfs.fat and mtools make here; fsck.fat and mtools check the results
if command -v mkfs.fat >/dev/null 2>&1 && command -v mcopy >/dev/null 2>&1 && command -v fsck.fat >/dev/null 2>&1; then
  export MTOOLS_SKIP_CHECK=1
  F="$T/fat"
  mkdir -p "$F/src/docs/sub" "$F/add" "$F/many"
  echo hello > "$F/src/README.TXT"
  seq 1 3000 > "$F/src/docs/Long numbers file.txt"
  echo deep > "$F/src/docs/sub/deep.txt"
  head -c 70000 /dev/urandom > "$F/src/docs/random.bin"
  echo "new file" > "$F/add/A new file with a long name.txt"
  echo dos > "$F/add/DOS.TXT"
  echo lower > "$F/add/lower.txt"
  for i in $(seq 1 300); do echo "f$i" > "$F/many/Same prefix name number $i.txt"; done
  fat_update() {  # fat_update <bits> <KiB>: make, change, check
    rm -f "$F/f$1.img" "$F/u$1.img"
    mkfs.fat -C -n "DISK$1" -F "$1" "$F/f$1.img" "$2" >/dev/null &&
    mcopy -s -i "$F/f$1.img" "$F/src/README.TXT" "$F/src/docs" ::/ &&
    cp "$F/f$1.img" "$F/u$1.img" &&
    "$S" add "$F/u$1.img" "" "$F/add/A new file with a long name.txt" "$F/add/DOS.TXT" >/dev/null 2>&1 &&
    "$S" add "$F/u$1.img" docs/sub "$F/add/lower.txt" >/dev/null 2>&1 &&
    "$S" del "$F/u$1.img" docs/random.bin >/dev/null 2>&1 &&
    "$S" ren "$F/u$1.img" README.TXT "Read Me.txt" >/dev/null 2>&1 &&
    fsck.fat -n "$F/u$1.img" >/dev/null 2>&1 &&
    [ "$(mdir -/ -b -i "$F/u$1.img" :: | LC_ALL=C sort | tr '\n' '|')" = "::/A new file with a long name.txt|::/DOS.TXT|::/Read Me.txt|::/docs/|::/docs/Long numbers file.txt|::/docs/sub/|::/docs/sub/deep.txt|::/docs/sub/lower.txt|" ] &&
    rm -rf "$F/x" && mkdir "$F/x" && mcopy -s -n -i "$F/u$1.img" ::/ "$F/x/" 2>/dev/null &&
    cmp -s "$F/x/Read Me.txt" "$F/src/README.TXT" && cmp -s "$F/x/docs/sub/lower.txt" "$F/add/lower.txt" &&
    cmp -s "$F/x/docs/Long numbers file.txt" "$F/src/docs/Long numbers file.txt" &&
    mlabel -s -i "$F/u$1.img" :: | grep -q "DISK$1" &&
    mdir -i "$F/u$1.img" :: | grep -q '^DOS      TXT ' &&
    mdir -i "$F/u$1.img" :: | grep -q '^ANEWFI~1 TXT '
  }
  for b in 12:1440 16:16384 32:40960; do
    DESC="FAT: add, delete and rename in a FAT${b%%:*} image, fsck.fat and mtools agree"
    check fat_update "${b%%:*}" "${b#*:}"
  done
  DESC="FAT: a folder of 300 clashing names grows its directory, short names unique"
  check sh -c "cp '$F/f16.img' '$F/g16.img' && '$S' add '$F/g16.img' docs '$F/many' >/dev/null 2>&1 && fsck.fat -n '$F/g16.img' >/dev/null 2>&1 && [ \$(mdir -/ -b -i '$F/g16.img' ::/docs/many | wc -l) = 300 ] && [ \$(mdir -i '$F/g16.img' ::/docs/many | grep -c '~') = 300 ]"
  DESC="FAT: renaming and deleting folders"
  check sh -c "cp '$F/f16.img' '$F/h16.img' && '$S' ren '$F/h16.img' docs Documents >/dev/null 2>&1 && '$S' del '$F/h16.img' Documents/sub >/dev/null 2>&1 && fsck.fat -n '$F/h16.img' >/dev/null 2>&1 && mdir -/ -b -i '$F/h16.img' :: | LC_ALL=C sort | tr '\n' '|' | grep -qx '::/Documents/|::/Documents/Long numbers file.txt|::/Documents/random.bin|::/README.TXT|'"
  DESC="FAT: a full FAT12 root directory refuses more, the image unchanged"
  check sh -c "mkdir -p '$F/root' && for i in \$(seq 1 230); do echo x > '$F/root/R'\$i.TXT; done && cp '$F/f12.img' '$F/r12.img' && ! '$S' add '$F/r12.img' '' '$F'/root/* >/dev/null 2>&1 && cmp -s '$F/f12.img' '$F/r12.img'"
  DESC="FAT: a full disk refuses a file, the image unchanged"
  check sh -c "head -c 1500000 /dev/urandom > '$F/huge.bin' && cp '$F/f12.img' '$F/d12.img' && ! '$S' add '$F/d12.img' '' '$F/huge.bin' >/dev/null 2>&1 && cmp -s '$F/f12.img' '$F/d12.img'"
else
  echo "SKIP: FAT updates (mkfs.fat, mtools, fsck.fat)"
fi

# .zst updates (src/formats/ZstdHandler.cpp, libzstd): the panel replaces
# the one file, as in .gz and .xz; upstream 7zz and, if installed, zstd
# read the result
Z="$T/zst"
mkdir -p "$Z/new"
cp "$SRC/tests/data/zst/hello.txt.zst" "$Z/"
seq 1 50000 > "$Z/new/hello.txt"
DESC="zst: reads a file zstd made"
check sh -c "'$S' x '$Z/hello.txt.zst' '$Z/out' >/dev/null && grep -qx 'hello from zstd' '$Z/out/hello.txt'"
DESC="zst: copying a file in replaces the content, read back by 7zz"
check sh -c "'$S' add '$Z/hello.txt.zst' '' '$Z/new/hello.txt' >/dev/null 2>&1 && '$SEVENZZ' e -so '$Z/hello.txt.zst' 2>/dev/null | cmp -s - '$Z/new/hello.txt'"
if command -v zstd >/dev/null 2>&1; then
  DESC="zst: zstd reads it, with the size and checksum in the frame"
  check sh -c "zstd -dc '$Z/hello.txt.zst' | cmp -s - '$Z/new/hello.txt' && zstd -lv '$Z/hello.txt.zst' 2>&1 | grep -q 'Decompressed Size:.*(288894 B)' && zstd -lv '$Z/hello.txt.zst' 2>&1 | grep -q 'Check: XXH64'"
else
  echo "SKIP: zst read back by zstd (zstd not installed)"
fi
DESC="zst: a second file or deleting the only one is refused, the archive unchanged"
cp "$Z/hello.txt.zst" "$Z/before.zst"
printf 'other\n' > "$Z/new/other.txt"
check sh -c "! '$S' add '$Z/hello.txt.zst' '' '$Z/new/other.txt' >/dev/null 2>&1 && ! '$S' del '$Z/hello.txt.zst' hello.txt >/dev/null 2>&1 && cmp -s '$Z/before.zst' '$Z/hello.txt.zst'"

# ACE (src/formats/AceHandler.cpp): made by ACE32 2.6, see tests/data/ace/README.md
AC="$SRC/tests/data/ace"
for f in ace10.ace solid.ace; do
  DESC="ACE: $f extracts"
  check sh -c "rm -rf '$T/a'; '$S' x '$AC/$f' '$T/a' >/dev/null 2>&1 && diff -r '$G/tree' '$T/a/tree' >/dev/null"
done
DESC="ACE: delta, x86, picture and sound modes"
check sh -c "rm -rf '$T/a'; '$S' x '$AC/media.ace' '$T/a' >/dev/null 2>&1 && '$S' x '$AC/sound.ace' '$T/a' >/dev/null 2>&1 && cd '$T/a' && sha256sum -c '$AC/media.sha256' >/dev/null"
for f in pass.ace pass-solid.ace; do
  DESC="ACE: $f with its password"
  check sh -c "rm -rf '$T/a'; CORE_SMOKE_PASSWORD=secret '$S' x '$AC/$f' '$T/a' >/dev/null 2>&1 && cmp -s '$G/tree/readme.txt' '$T/a/tree/readme.txt'"
  DESC="ACE: $f with a wrong password fails"
  check sh -c "! CORE_SMOKE_PASSWORD=wrong '$S' t '$AC/$f' >/dev/null 2>&1"
done

# UPX (src/formats/UpxWrap.cpp): only with upx installed
if command -v upx >/dev/null 2>&1; then
  cp "$S" "$T/packed"
  upx -q -q -q "$T/packed" >/dev/null 2>&1
  upx -d -q -q -q -o "$T/unpacked" "$T/packed" >/dev/null 2>&1
  DESC="UPX: a packed executable shows the unpacked one, as upx -d makes it"
  check sh -c "'$S' cat '$T/packed' packed | cmp -s - '$T/unpacked'"
else
  echo "SKIP: UPX (upx not installed)"
fi

exit $fail

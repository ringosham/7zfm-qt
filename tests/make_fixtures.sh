#!/bin/sh
# Builds test images in $1 (default: build/fixtures). No root needed:
# filesystems are written straight into the image at partition offsets.
set -eu
OUT=${1:-build/fixtures}
mkdir -p "$OUT"
cd "$OUT"
[ -f .done ] && exit 0
rm -rf content && mkdir -p content/docs content/deep/a/b/c
printf 'hello from the ext4 partition\n' > content/hello.txt
printf 'nested deep file\n' > content/deep/a/b/c/leaf.txt
head -c 3000000 /dev/urandom > content/docs/random.bin
mkdir -p fatcontent && printf 'hello from FAT32\n' > fatcontent/FATFILE.TXT

# 20 GiB sparse GPT disk: p1 FAT32 64 MiB, p2 ext4 (rest, ~19.9 GiB).
rm -f disk.img
truncate -s 20G disk.img
sfdisk -q disk.img <<SFD
label: gpt
start=2048, size=131072, type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, name="fat"
start=133120, type=0FC63DAF-8483-461D-B8E4-709DE43D7984, name="linux"
SFD
mkfs.vfat -F 32 --offset=2048 disk.img $((131072 / 2)) >/dev/null
mcopy -i disk.img@@$((2048 * 512)) fatcontent/FATFILE.TXT ::/
P2_SECTORS=$(( (20 * 1024 * 1024 * 2) - 133120 - 34 ))
mkfs.ext4 -q -F -E offset=$((133120 * 512)),lazy_itable_init=1,lazy_journal_init=1 \
  -d content disk.img $((P2_SECTORS / 2))k

qemu-img convert -O vhdx -o subformat=dynamic disk.img disk.vhdx
qemu-img convert -O vmdk disk.img disk.vmdk
qemu-img convert -O qcow2 disk.img disk.qcow2

# A stored (uncompressed) zip holding the VHDX: every level is seekable.
python3 -I -c '
import zipfile
with zipfile.ZipFile("vhdx-in-zip.zip", "w", zipfile.ZIP_STORED, allowZip64=True) as z:
    z.write("disk.vhdx")
'
# A compressed container: Open Inside must fall back to RAM/temp extraction.
truncate -s 8M small-fat.img
mkfs.vfat small-fat.img >/dev/null
mcopy -i small-fat.img fatcontent/FATFILE.TXT ::/
tar -cJf small-fat.tar.xz small-fat.img

if command -v rar >/dev/null 2>&1; then
  (cd content && rar a -inul -ma4 ../test4.rar . && rar a -inul -ma5 ../test5.rar . \
     && rar a -inul -ma5 -s ../solid.rar . && rar a -inul -ma5 -v1m ../vol.rar . \
     && rar a -inul -ma5 -hpsecret ../enc.rar .)
else
  echo "warning: 'rar' not installed (AUR: rar); RAR fixtures skipped" >&2
fi
touch .done

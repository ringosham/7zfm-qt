#!/bin/sh
# Writes the ISO 9660 fixtures in tests/data/iso (committed, so the tests
# don't need xorriso), from a small tree:
#   plain.iso  ISO 9660 names only (8.3, upper case)
#   rr.iso     Rock Ridge and Joliet
#   boot.iso   Rock Ridge and Joliet, El Torito (boot info table), a hybrid
#              MBR and a partition with its own trees (-partition_offset 16)
#   udf.iso    rr.iso with UDF descriptors after its own (BEA01/NSR02/TEA01)
# Run: tests/make_isos.sh tests/data/iso
set -eu
OUT=${1:-tests/data/iso}
command -v xorriso >/dev/null || { echo "xorriso not found" >&2; exit 1; }
mkdir -p "$OUT"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/tree/docs/sub" "$T/tree/boot"
echo hello > "$T/tree/readme.txt"
echo deep > "$T/tree/docs/sub/deep.txt"
seq 1 200 > "$T/tree/docs/numbers.txt"
ln -s ../readme.txt "$T/tree/docs/link"
head -c 2048 /dev/zero > "$T/tree/boot/boot.img"
export SOURCE_DATE_EPOCH=1700000000
x() { xorriso -as mkisofs -quiet -no-pad "$@" "$T/tree" 2>/dev/null; }
x --norock -o "$OUT/plain.iso"
x -J -R -o "$OUT/rr.iso"
x -J -R -b boot/boot.img -no-emul-boot -boot-load-size 4 -boot-info-table -c boot/boot.cat \
  -partition_offset 16 -isohybrid-gpt-basdat -o "$OUT/boot.iso"
python3 -I - "$OUT/rr.iso" "$OUT/udf.iso" <<'EOF'
import sys
d = bytearray(open(sys.argv[1], 'rb').read())
s = 16
while d[s * 2048 + 1:s * 2048 + 6] == b'CD001' and d[s * 2048] != 255:
    s += 1
for i, sig in enumerate([b'BEA01', b'NSR02', b'TEA01']):
    o = (s + 1 + i) * 2048
    d[o:o + 2048] = bytes([0]) + sig + bytes([1]) + bytes(2041)
open(sys.argv[2], 'wb').write(d)
EOF

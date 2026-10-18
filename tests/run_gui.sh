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

SEVENZZ=${SEVENZZ:-}
if [ -z "$SEVENZZ" ] && command -v 7zz >/dev/null 2>&1; then SEVENZZ=7zz; fi
SMOKE=$(dirname "$APP")/../tests/core_smoke

DESC="GUI: Add to Archive (upstream CompressDialog) creates a 7z"
mkdir -p "$WORK/add" && cp -r "$FX/content" "$WORK/add/"
check run "$WORK/add/" --select content --action add --keys "Return" --screenshot "$WORK/s5.png"
check sh -c "'$SEVENZZ' e -so '$WORK/add/content.7z' content/docs/random.bin 2>/dev/null | cmp -s - '$FX/content/docs/random.bin'"

DESC="GUI: Create Folder (F7) inside a 7z"
check run "$WORK/add/content.7z" --keys "F7,Return" --screenshot "$WORK/s6.png"
check sh -c "'$SMOKE' ls '$WORK/add/content.7z' | grep -q 'New Folder'"

DESC="GUI: rename inside a 7z"
check run "$WORK/add/content.7z/content/" --select docs --action rename:documents --screenshot "$WORK/s7.png"
check sh -c "'$SMOKE' ls '$WORK/add/content.7z' content | grep -q documents"

DESC="GUI: delete inside a 7z"
check run "$WORK/add/content.7z/content/" --select hello.txt --keys "Delete,Return" --screenshot "$WORK/s8.png"
check sh -c "! '$SMOKE' ls '$WORK/add/content.7z' content | grep -q hello.txt"

DESC="GUI: drop files into a 7z (CopyFrom)"
echo dropped > "$WORK/add/drop.txt"
check run "$WORK/add/content.7z/content/" --action "drop:$WORK/add/drop.txt" --keys "@wait,@wait" --screenshot "$WORK/s9.png"
check sh -c "'$SEVENZZ' e -so '$WORK/add/content.7z' content/drop.txt 2>/dev/null | grep -q dropped"

DESC="GUI: split into 1 MB volumes and combine back"
mkdir -p "$WORK/split/comb" && cp "$FX/content/docs/random.bin" "$WORK/split/"
check run "$WORK/split/" --select random.bin --action split --keys "Tab,Tab,Ctrl+A,@type:1M,Return" --screenshot "$WORK/s10.png"
check test -f "$WORK/split/random.bin.003"
mv "$WORK/split/random.bin.0"* "$WORK/split/comb/"
check run "$WORK/split/comb/" --select random.bin.001 --action combine --keys "Return" --screenshot "$WORK/s11.png"
check cmp -s "$WORK/split/comb/random.bin" "$FX/content/docs/random.bin"

DESC="GUI: file edited in an external editor is written back into the 7z"
mkdir -p "$WORK/wb" "$WORK/cfg/7zfm-qt"
cat > "$WORK/wb/editor.sh" <<'EOS'
#!/bin/sh
( sleep 1; echo "edited by test editor" >> "$1" ) &
EOS
chmod +x "$WORK/wb/editor.sh"
printf '[Software]\n7-Zip\\FM\\Editor=%s\n' "$WORK/wb/editor.sh" >> "$WORK/cfg/7zfm-qt/7zfm-qt.conf"
cp "$FX/small-fat.tar.xz" "$WORK/wb/" && cp -r "$FX/content" "$WORK/wb/"
run "$WORK/wb/" --select content --action add --keys "Return" --screenshot "$WORK/s12.png"
check run "$WORK/wb/content.7z/content/" --select hello.txt --keys "F4,@wait,@wait,@wait,@wait,@wait,Return,@wait,@wait" --screenshot "$WORK/s13.png"
check sh -c "'$SEVENZZ' e -so '$WORK/wb/content.7z' content/hello.txt 2>/dev/null | grep -q 'edited by test editor'"
DESC="GUI: archives modified by the GUI pass 7zz t"
check sh -c "'$SEVENZZ' t '$WORK/add/content.7z' >/dev/null && '$SEVENZZ' t '$WORK/wb/content.7z' >/dev/null"

DESC="GUI: encrypted 7z (encrypted headers): password prompt, then copy-out reuses it"
mkdir -p "$WORK/enc"
"$SEVENZZ" a -pSECRET -mhe=on "$WORK/enc/enc.7z" "$FX/content" >/dev/null
check run "$WORK/enc/enc.7z" --keys "@wait,@type:SECRET,Return,@wait,@wait,@select:content,@action:copy,@wait,Return,@wait,@wait,@wait" --screenshot "$WORK/s14.png"
check cmp -s "$WORK/enc/content/docs/random.bin" "$FX/content/docs/random.bin"

DESC="GUI: temp folders are removed on exit"
check sh -c "[ -z \"\$(find '$WORK/cache' -mindepth 2 -maxdepth 3 -name '7zO*' 2>/dev/null)\" ]"
exit $fail

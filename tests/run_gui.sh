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

# View/Edit with no Viewer/Editor configured, with stub programs (nothing
# opens on the desktop): F3 -> the file's default app via xdg-open,
# F4 -> the text/plain default from mimeapps.list
L="$WORK/launch"
mkdir -p "$L/bin" "$L/cfg" "$L/data/applications"
for t in xdg-open fakeedit; do
  printf '#!/bin/sh\necho "%s $*" >> "%s/log"\n' "$t" "$L" > "$L/bin/$t"; chmod +x "$L/bin/$t"
done
printf '[Desktop Entry]\nType=Application\nName=Fake Edit\nExec="%s/bin/fakeedit" -b %%U\n' "$L" > "$L/data/applications/fake-editor.desktop"
printf '[Default Applications]\ntext/plain=missing.desktop;fake-editor.desktop\n' > "$L/cfg/mimeapps.list"
launch() { env PATH="$L/bin:$PATH" XDG_CONFIG_HOME="$L/cfg" XDG_DATA_HOME="$L/data" timeout 60 "$APP" "$@" >/dev/null 2>&1; }

DESC="GUI: View (F3) in an archive opens the default app via xdg-open"
check launch "$WORK/add/content.7z/content/" --select drop.txt --keys "F3,@wait" --screenshot "$WORK/v1.png"
check grep -q "^xdg-open .*/7zO.*/drop.txt$" "$L/log"

DESC="GUI: Edit (F4) in an archive opens the desktop's text editor"
: > "$L/log"
check launch "$WORK/add/content.7z/content/" --select drop.txt --keys "F4,@wait" --screenshot "$WORK/v2.png"
check grep -q "^fakeedit -b file:///.*/7zO.*/drop.txt$" "$L/log"

if command -v dbus-run-session >/dev/null 2>&1; then
  DESC="GUI: drop on a KIO target (Ark DnD D-Bus call) copies out of the archive"
  mkdir -p "$WORK/dnd"
  check dbus-run-session -- sh -c 'timeout 60 "$@" >/dev/null 2>&1' sh "$APP" "$WORK/add/content.7z/content/" \
    --select drop.txt --action "dnd:$WORK/dnd" --keys "@wait,@wait" --screenshot "$WORK/d1.png"
  check grep -q dropped "$WORK/dnd/drop.txt"
else
  echo "SKIP: drag-out D-Bus check (no dbus-run-session)"
fi

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

# ---- RAR (test4.rar is a generated store-only RAR4; the rest need 'rar') ----
R="$WORK/rar"
same_tree() {  # <dir> <archive> [password]: dir matches content/ and 7zz x
  rm -rf "$R/ref" && "$SEVENZZ" x -y -p"${3:-}" -o"$R/ref" "$2" >/dev/null &&
    diff -r "$FX/content" "$1" >/dev/null && diff -r "$R/ref" "$1" >/dev/null
}
same_docs() { diff -r "$FX/content/docs" "$1" >/dev/null; }
rars=test4
[ -f "$FX/test5.rar" ] && rars="test4 test5 solid"
for a in $rars; do
  DESC="GUI: Extract dialog extracts $a.rar"
  mkdir -p "$R/$a" && cp "$FX/$a.rar" "$R/$a/"
  check run "$R/$a/" --select $a.rar --action extract --keys "Return" --screenshot "$WORK/r-$a.png"
  check same_tree "$R/$a/$a" "$FX/$a.rar"
done

if [ -f "$FX/test5.rar" ]; then
  DESC="GUI: Extract dialog extracts a multi-volume RAR from .part1.rar"
  mkdir -p "$R/vol" && cp "$FX"/vol.part*.rar "$R/vol/"
  check run "$R/vol/" --select vol.part1.rar --action extract --keys "Return" --screenshot "$WORK/r-vol1.png"
  check same_tree "$R/vol/vol" "$FX/vol.part1.rar"

  DESC="GUI: Ctrl+PgDn into vol.part1.rar, F5 copies a file spanning volumes"
  check run "$R/vol/" --select vol.part1.rar --keys "Ctrl+PgDown,@select:docs,F5,Return,@wait,@wait" --screenshot "$WORK/r-vol2.png"
  check same_docs "$R/vol/docs"

  DESC="GUI: encrypted RAR headers ask for a password on open"
  mkdir -p "$R/enc" && cp "$FX/enc.rar" "$R/enc/"
  check run "$R/enc/enc.rar" --keys "@wait" --screenshot "$WORK/r-enc1.png"
  check grep -qi "password" "$WORK/r-enc1.png.txt"

  DESC="GUI: encrypted RAR headers, wrong password: 'Wrong password?' error"
  check run "$R/enc/enc.rar" --keys "@wait,@type:wrong,Return,@wait" --screenshot "$WORK/r-enc2.png"
  check grep -q "Cannot open encrypted archive.*Wrong password" "$WORK/r-enc2.png.txt"

  DESC="GUI: encrypted RAR headers, right password: copy-out reuses it"
  check run "$R/enc/enc.rar" --keys "@wait,@type:secret,Return,@wait,@wait,@select:docs,@action:copy,@wait,Return,@wait,@wait,@wait" --screenshot "$WORK/r-enc3.png"
  check same_docs "$R/enc/docs"

  DESC="GUI: encrypted RAR data: password asked on copy-out"
  mkdir -p "$R/encdata" && cp "$FX/encdata.rar" "$R/encdata/"
  check run "$R/encdata/encdata.rar" --keys "@wait,@select:docs,F5,Return,@wait,@wait,@type:secret,Return,@wait,@wait,@wait" --screenshot "$WORK/r-encdata.png"
  check same_docs "$R/encdata/docs"

  DESC="GUI: Ctrl+PgDn RAR -> FAT image (extracted, no in-place stream), F5 copy-out"
  mkdir -p "$R/nested" && cp "$FX/nested.rar" "$R/nested/"
  check run "$R/nested/" --select nested.rar --keys "Ctrl+PgDown,Ctrl+PgDown,F5,Return,@wait" --screenshot "$WORK/r-nested.png"
  check cmp -s "$R/nested/FATFILE.TXT" "$FX/fatcontent/FATFILE.TXT"
else
  echo "SKIP: GUI RAR5 checks ('rar' not installed)"
fi

DESC="GUI: temp folders are removed on exit"
check sh -c "[ -z \"\$(find '$WORK/cache' -mindepth 2 -maxdepth 3 -name '7zO*' 2>/dev/null)\" ]"
exit $fail

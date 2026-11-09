#!/usr/bin/env python3
"""Writes the file managers' context-menu entries for 7zfm-qt.

  gen-menus.py <app .desktop file> <output dir>

The entries are those of upstream's Explorer menu (Explorer/ContextMenu.cpp):
a "7-Zip" submenu and a "CRC SHA" submenu next to it. Each runs one of the
command-line modes in src/ui/ShellCommand.cpp. Writes:

  kio/        KDE Dolphin service menus  -> share/kio/servicemenus
  nemo/       Nemo actions               -> share/nemo/actions
  fma/        FileManager-Actions        -> share/file-manager/actions
              (PCManFM-Qt, Caja with caja-actions)

Thunar's entries come from data/menus/7zfm-qt.tap (thunar-archive-plugin).
"""

import os
import sys

APP = "7zfm-qt"

# (id, label, command-line mode, archives only)
MAIN = [
    ("open", "Open archive", None, True),
    ("extract", "Extract files...", "--extract", True),
    ("extract-here", "Extract Here", "--extract-here", True),
    ("extract-to", 'Extract to "*/"', "--extract-to", True),
    ("test", "Test archive", "--test", True),
    ("add", "Add to archive...", "--add", False),
    ("add-7z", 'Add to "*.7z"', "--add-7z", False),
    ("add-zip", 'Add to "*.zip"', "--add-zip", False),
]

# (id, label, method) as upstream's g_HashCommands
HASHES = [
    ("crc32", "CRC-32", "CRC32"),
    ("crc64", "CRC-64", "CRC64"),
    ("xxh64", "XXH64", "XXH64"),
    ("md5", "MD5", "MD5"),
    ("sha1", "SHA-1", "SHA1"),
    ("sha256", "SHA-256", "SHA256"),
    ("sha384", "SHA-384", "SHA384"),
    ("sha512", "SHA-512", "SHA512"),
    ("sha3-256", "SHA3-256", "SHA3-256"),
    ("blake2sp", "BLAKE2sp", "BLAKE2sp"),
    ("all", "*", "*"),
]
HASH_FILE = ("sha256-file", "SHA-256 -> *.sha256")


def archive_mime_types(desktop_file):
    """The app's MimeType list, without the catch-all types."""
    with open(desktop_file, encoding="utf-8") as f:
        for line in f:
            if line.startswith("MimeType="):
                types = [t for t in line.strip()[len("MimeType="):].split(";") if t]
                return [t for t in types
                        if t != "application/octet-stream" and not t.startswith("inode/")]
    raise SystemExit(f"{desktop_file}: no MimeType line")


def entries():
    """(submenu, id, label, exec arguments before the files, archives only)"""
    for ident, label, mode, arc in MAIN:
        yield "7-Zip", ident, label, mode, arc
    for ident, label, method in HASHES:
        # "*" is a reserved character in an Exec line, so it is quoted
        arg = f'"{method}"' if method == "*" else method
        yield "CRC SHA", "hash-" + ident, label, "--hash " + arg, False
    yield "CRC SHA", "hash-" + HASH_FILE[0], HASH_FILE[1], "--hash-file", False


def exec_line(mode, files_code, file_code):
    if mode is None:
        return f"{APP} {file_code}"
    return f"{APP} {mode} {files_code}"


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def gen_kio(out, arc_types):
    # one service menu per MIME filter; KDE merges same-named submenus
    groups = [
        ("archive", arc_types, "7-Zip", True),
        ("add", ["all/all"], "7-Zip", False),
        ("hash", ["all/all"], "CRC SHA", False),
    ]
    for name, types, submenu, arc in groups:
        items = [e for e in entries() if e[0] == submenu and e[4] == arc]
        text = ("[Desktop Entry]\n"
                "Type=Service\n"
                "X-KDE-ServiceTypes=KonqPopupMenu/Plugin\n"
                f"MimeType={';'.join(types)};\n"
                f"Actions={';'.join(e[1] for e in items)};\n"
                f"X-KDE-Submenu={submenu}\n"
                "X-KDE-Priority=TopLevel\n"
                f"Icon={APP}\n")
        for _, ident, label, mode, _ in items:
            text += (f"\n[Desktop Action {ident}]\n"
                     f"Name={label}\n"
                     f"Icon={APP}\n"
                     f"Exec={exec_line(mode, '%F', '%f')}\n")
        write(os.path.join(out, "kio", f"{APP}-{name}.desktop"), text)


def gen_nemo(out, arc_types):
    # Nemo has no submenus for packaged actions: the 7-Zip commands and the
    # all-methods checksum, prefixed so that they sort together
    for submenu, ident, label, mode, arc in entries():
        if submenu == "CRC SHA" and ident != "hash-all":
            continue
        name = f"7-Zip: {label}" if submenu == "7-Zip" else "CRC SHA: *"
        text = ("[Nemo Action]\n"
                f"Name={name}\n"
                f"Exec={exec_line(mode, '%F', '%F')}\n"
                f"Icon-Name={APP}\n"
                f"Selection={'s' if mode is None else 'notnone'}\n")
        text += f"Mimetypes={';'.join(arc_types)};\n" if arc else "Extensions=any;\n"
        text += "Quote=double\n" f"Dependencies={APP};\n"
        write(os.path.join(out, "nemo", f"{APP}-{ident}.nemo_action"), text)


def gen_fma(out, arc_types):
    # FileManager-Actions: one file per action, and a Type=Menu file for each
    # submenu that lists them
    menus = {}
    for submenu, ident, label, mode, arc in entries():
        action_id = f"{APP}-{ident}"
        menus.setdefault(submenu, []).append(action_id)
        types = arc_types if arc else ["all/all"]
        text = ("[Desktop Entry]\n"
                "Type=Action\n"
                f"Name={label}\n"
                f"Icon={APP}\n"
                "Profiles=main;\n"
                "\n[X-Action-Profile main]\n"
                f"MimeTypes={';'.join(types)};\n"
                f"Exec={exec_line(mode, '%F', '%f')}\n")
        if mode is None:
            text += "SelectionCount==1\n"
        write(os.path.join(out, "fma", f"{action_id}.desktop"), text)
    for submenu, ids in menus.items():
        menu_id = f"{APP}-menu" if submenu == "7-Zip" else f"{APP}-menu-hash"
        text = ("[Desktop Entry]\n"
                "Type=Menu\n"
                f"Name={submenu}\n"
                f"Icon={APP}\n"
                f"ItemsList={';'.join(ids)};\n")
        write(os.path.join(out, "fma", f"{menu_id}.desktop"), text)


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    desktop_file, out = sys.argv[1:]
    arc_types = archive_mime_types(desktop_file)
    gen_kio(out, arc_types)
    gen_nemo(out, arc_types)
    gen_fma(out, arc_types)


if __name__ == "__main__":
    main()

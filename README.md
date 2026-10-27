# 7zfm-qt

A Qt 6 port of the Windows 7-Zip File Manager (7zFM) for Linux.

Unlike PeaZip, Ark or File Roller, it links 7-Zip as a library and opens nested items **in place**, reading straight from the parent's stream. That covers a partition inside a VHDX, the filesystem inside that partition, or a tar inside an xz. Nothing is extracted, so a 20 GB partition opens instantly. Only items that are stored compressed have to be unpacked, exactly as on Windows: small ones into RAM, large ones under `~/.cache/7zfm-qt`.

It uses upstream 7-Zip 26.05 unmodified, and as much of upstream's own GUI code as possible.

## Features

**Open by signature, not extension**
- Everything 7-Zip reads: 7z, zip, rar/rar5, tar, gz/xz/zstd/bz2, iso/udf, cab/msi/wim, dmg, squashfs, cpio/rpm/deb, and more.
- Disk images: raw, VHD, VHDX, VMDK, VDI, QCOW2.
- Partition tables: GPT, MBR, APM.
- Filesystems: NTFS, FAT/exFAT, ext2/3/4, HFS+, APFS.
- Raw block devices under Computer → Devices, opened read-only. You need read permission on the device, e.g. membership in the `disk` group.

**Opening items**
- Open Inside (Ctrl+PgDn), Open Inside `*`, Open Inside `#` (parser mode: finds embedded archives), Open Outside (Shift+Enter).
- Typed nested paths work: `/x/disk.vhdx/1.linux.img/home` opens each level in place.

**Extracting and testing**
- Extract, using upstream's Extract dialog logic.
- Test.
- Copy To (F5) out of archives.
- Drag out to other applications. Dropping on Dolphin or the Plasma desktop copies straight away, without the Copy/Move/Link menu, through the same D-Bus call Ark uses.
- Password, overwrite and memory-limit prompts.

**Editing archives**
- Add to Archive, using upstream's own CompressDialog.
- Delete, rename (F2), Create Folder (F7), Create File (Ctrl+N) and Comment (Ctrl+Z) inside archives.
- Drag files in to add them.
- View (F3) and Edit (F4): a file changed in another application is offered for update in the archive, as in 7zFM.

**File management**
- File-system operations: copy, move, rename, delete (to trash, or permanently with Shift+Del), new file and new folder.
- Split and Combine, Link (hard and symbolic links).

**Tools and settings**
- CRC menu: CRC-32/64, XXH64, MD5, SHA-1/256/384/512, SHA3-256, BLAKE2sp.
- Benchmark, using upstream's dialog.
- Options: the Settings, Folders and Editor pages.

**Interface**
- Two panels (F9), as in 7zFM:
  - Tab switches panels.
  - F5/F6 copy or move to the other panel's folder: out of an archive, into one, or from archive to archive through a temp folder.
  - Shift+F5/F6 copy under a new name in the same folder.
  - Alt+Up shows the same folder in the other panel; Alt+Left/Right opens the focused subfolder there.
  - The panel count, the focused panel, each panel's folder and view mode are saved.
- Flat view, column layouts saved per folder type, sorting like 7zFM.
- Favorites (Alt+Shift+N to set, Alt+N to go), folder history, properties (Alt+Enter) with every archive level's info.

## Build

```sh
git submodule update --init
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
sudo cmake --install build        # or: cd packaging/arch && makepkg -si
```

Debian 13 and Ubuntu 25.04 or later (Qt 6.5 is the minimum): `packaging/debian/build.sh` builds and tests a `.deb` into `dist/`. With `--docker debian:trixie` (or `ubuntu:26.04`) it builds in a fresh container instead, so any distribution with Docker works. GitHub Actions builds both for amd64 and arm64 on every push, and attaches them to a release for each `v*` tag.

Options:
- `-DSANITIZE=ON` for ASan and UBSan.
- `-DBUILD_GUI=OFF` for the engine and tests only.

Settings live in `~/.config/7zfm-qt/7zfm-qt.conf`.

## How it is put together

**`external/7zip`** is upstream 7-Zip, pinned and never modified.

**`cmake/7zcore-sources.cmake`** lists the upstream sources that get linked. It is generated from the Linux 7zz build by `tools/gen-core-sources.sh`; rerun that after bumping the submodule. Formats, codecs (RAR included) and `UI/Common` link statically, as in `7zz`.

**`src/agent`** is a port of upstream `UI/Agent`, which upstream never builds on POSIX. Every change is marked `7zfm-qt:`, and `git diff --no-index external/7zip/CPP/7zip/UI/Agent src/agent` shows them all. Besides POSIX attribute handling, it fixes two bugs that the Windows build hides:
- **A use-after-free in `CProxyArc2::Load`.** It garbles names, e.g. FAT short names. Windows' BSTR cache masks it.
- **A deadlock in `LoadGlobalCodecs`.** On POSIX, `CCriticalSection` is not recursive.

**`src/w32ui` (Q32)** is a small user32 written on Qt. It covers dialog templates, the `WM_INITDIALOG` / `WM_COMMAND` / `WM_TIMER` / `WM_NOTIFY` messages, the CB/BM/EM/UDM/PBM control messages, property sheets and message boxes. `tools/gen-dialogs.py` compiles upstream's `.rc` layouts into tables for it.

With that in place, these compile unchanged and show upstream's real dialogs:
- `Windows/Window.cpp`, `Control/Dialog.cpp`, `ComboBox.cpp`, `PropertyPage.cpp`;
- `GUI/UpdateGUI.cpp`, `UpdateCallbackGUI*.cpp`;
- `FileManager/` `PasswordDialog`, `ComboDialog`, `CopyDialog`, `SplitDialog`, `SettingsPage`, `FoldersPage`, `EditPage`, `UpdateCallback100`.

`GUI/CompressDialog.cpp`, `BenchmarkDialog.cpp`, `FileManager/PanelSplitFile.cpp` and `PanelCrc.cpp` are copied into `src/gui` with small POSIX fixes, each marked `7zfm-qt:`.

**`src/fm`** holds FileManager logic with no widgets:
- copies of `ExtractCallback`, `ExtractGUI`, `HashGUI`, `CProgressSync`, `OpenCallback` and `FileFolderPluginOpen`;
- the in-process `CompressCall2`;
- POSIX folders: `FsFolder`, and the Computer and Devices views.

**`src/core`** holds:
- `QSettings`-backed `NWindows::NRegistry::CKey`, so upstream's settings code (`ZipRegistry`, `RegistryUtils`, `ViewSettings`) runs as is;
- resource strings extracted from upstream's `.rc` files by `tools/gen-lang.py`;
- link seams.

**`src/ui`** holds the Qt widgets. `Panel` ports `CPanel`, and the navigation and Open Inside code follows upstream line by line.

**`tests`** has three parts:
- `core_smoke`, which opens nested levels in place;
- an offscreen GUI suite (`run_gui.sh`);
- upstream `7zz`, built from the same sources, as the reference.

The fixtures are a sparse 20 GB GPT disk with FAT32 and ext4, as raw, VHDX, VMDK and QCOW2.

## Differences from Windows 7zFM

- **Options.** The System (file associations) and 7-Zip (Explorer menu) pages are left out; use your desktop's "Open with" instead, since the `.desktop` file registers the MIME types. There is no Language page yet.
- **Not applicable on Linux:** NTFS alternate streams, security descriptors, Mark-of-the-Web, large pages, junctions and WSL links.
- **View and Edit** with no program set in Options > Editor: F3 opens the file's default application and F4 the desktop's default text editor. Upstream uses Notepad for both.
- **Drag-out** extracts when the drag starts, because X11 and Wayland need paths up front. Windows waits for the drop.
- **RAR.** Extraction only, as upstream; 7-Zip has no RAR encoder. RAR test fixtures are skipped unless the `rar` CLI is installed.

## License

LGPL-2.1-or-later, with the unRAR license restriction. See `LICENSE`, which is upstream's `DOC/License.txt`. 7-Zip is Copyright (C) Igor Pavlov.

# 7zfm-qt

A Qt 6 native port of the Windows 7-Zip File Manager (7zFM) for Linux purely for personal use.

> [!CAUTION]
> Fully vibecoded software with minimal review. Do not use in production. While this is based on upstream 7-Zip, we cannot guarantee any security issues or maintenance will be addressed on the frontend side. Use at your own risk.

It uses upstream 7-Zip 26.06 unmodified, and as much of upstream's own GUI code as possible.

## Why?

**Open by signature, not extension**
- Opens as long as it recognizes the file signature, not by the file extension
- Everything 7-Zip already reads: Archives, ISOs, disk images, partitions, even raw filesystem dumps
- It can open raw block devices in /dev if given root, read only

**Directly open nested disk images without extracting**
- Regular archive software invokes 7zz in command line for all file operations, so nested archives and images always require extracting gigabytes of content before opening.
- This port builds 7-Zip as a library just like the Windows version. You can now directly open them at zero waiting time.

**File manager integrations**
- The same context menu entries from Windows are ported after to most major Linux file managers, so you can right click on an archive and extract it without opening the GUI.
- Supported file managers as follows:
    - Dolphin
    - Nemo
    - PCManFM (Restart required)
    - Caja (Requires `caja-actions`)
    - Thunar (Requires `thunar-archive-plugin`)
- Nautilus is not supported as this package does not build the extensnion.

**Command line options**
- `7zfm-qt <command> [switches] ...` takes 7-Zip's own command line, as `7zG` does: the commands and switches of `7z` (`a`, `u`, `d`, `x`, `e`, `t`, `h`, `b`), with dialogs and progress windows instead of console output. For example `7zfm-qt x archive.7z -o/tmp/out`, `7zfm-qt a -ad backup.7z ~/docs` (the Add to Archive dialog) or `7zfm-qt h -scrcSHA256 file.iso`.
- Anything else opens the file manager at that path; a file actually named `a`, `x`, ... needs `./a`.
- The menu entries call `7zfm-qt --extract | --extract-here | --extract-to | --test | --add | --add-7z | --add-zip | --hash METHOD | --hash-file FILES...` instead (see `src/ui/ShellCommand.h`). They need folder and archive names worked out from the selection (`Extract to "name/"`, `Add to "name.7z"`), which a static menu file can't express in 7z syntax; upstream's Explorer menu also works them out itself before it starts `7zG`.

## Build

```sh
git submodule update --init
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
sudo cmake --install build
```

Arch: `packaging/arch/PKGBUILD` builds the tagged GitHub release (`makepkg -si`); every release has it attached.

Debian 13 and Ubuntu 25.04 or later (Qt 6.5 is the minimum): `packaging/debian/build.sh` builds and tests a `.deb` into `dist/`. With `--docker debian:trixie` (or `ubuntu:26.04`) it builds in a fresh container instead, so any distribution with Docker works. Fedora (Qt 6.5 or later; tested on 44): `packaging/rpm/build.sh` does the same for an `.rpm`, natively or with `--docker fedora:44`.

GitHub Actions builds the Debian 13, Ubuntu 26.04 and Fedora 44 packages for amd64 and arm64 on every push to `master`, and publishes a release with them and the PKGBUILD for each `v*` tag.

On x86_64, install `uasm` (or `jwasm`/`asmc`) for 7-Zip's assembly LZMA decoder, which decodes about twice as fast as the C one; the configure step reports which is used. arm64 always gets it. Debian, Ubuntu and Fedora don't package uasm, so their build scripts take one with `--uasm FILE`; CI builds UASM 2.57 from source for the amd64 packages.

Options:
- `-DSANITIZE=ON` for ASan and UBSan.
- `-DLZMA_DEC_ASM=OFF` for the C LZMA decoder everywhere.
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
- **Drag and drop** to extract is currenly only tested to work well on Dolphin only.

## License

LGPL-2.1-or-later, with the unRAR license restriction. See `LICENSE`, which is upstream's `DOC/License.txt`. 7-Zip is Copyright (C) Igor Pavlov.

*As much as I want to make this CC0 because I didn't write any of it, the project is derived from 7-Zip. Hence bound by the same license.*

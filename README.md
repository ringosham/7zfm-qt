# 7zfm-qt

A Qt 6 port of the Windows 7-Zip File Manager (7zFM) for Linux.

What sets it apart from PeaZip, Ark or File Roller: like 7zFM on Windows, it
links 7-Zip as a library and opens nested items **in place**, straight from
the parent's stream. A partition inside a VHDX, or a filesystem inside that
partition, opens instantly with nothing extracted to temp.

## Build

```sh
git submodule update --init
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Options: `-DSANITIZE=ON` (ASan + UBSan), `-DBUILD_GUI=OFF` (engine and tests only).

## Layout

- `external/7zip`: upstream 7-Zip, pinned and never modified.
- `cmake/7zcore-sources.cmake`: upstream sources linked in. Regenerate it with
  `tools/gen-core-sources.sh` after bumping the submodule.
- `src/agent`: port of upstream `CPP/7zip/UI/Agent`, which upstream never
  builds on POSIX. Every change is marked `7zfm-qt:`. To review them, or to
  merge a new upstream release:
  `git diff --no-index external/7zip/CPP/7zip/UI/Agent src/agent`
- `src/core`: glue with no widgets in it:
  - link seams;
  - a `QSettings`-backed `NWindows::NRegistry::CKey`, so upstream's settings
    code (`ZipRegistry.cpp`, `RegistryUtils.cpp`, `ViewSettings.cpp`) runs
    unchanged;
  - COM GUID instantiation.
- `src/ui`: the Qt widgets.
- `tests`: `core_smoke` plus a fixture generator. The fixtures are a sparse
  20 GiB GPT disk with FAT32 and ext4 partitions, as raw, VHDX, VMDK and
  QCOW2.

## License

LGPL-2.1-or-later, with the unRAR license restriction. See `LICENSE`, which is
upstream's `DOC/License.txt`. 7-Zip is Copyright (C) Igor Pavlov.

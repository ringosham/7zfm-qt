#!/bin/sh
# Prints the package version of the source tree given (default: this one):
# 0.1.0 on the v0.1.0 tag, else 0.1.0~git20261027.b74c5c0, which sorts
# below the release it leads up to (dpkg and rpm both read "~" that way).
set -eu
src=${1:-$(cd "$(dirname "$0")/.." && pwd)}
ver=$(sed -n 's/^project(7zfm-qt VERSION \([0-9.]*\).*/\1/p' "$src/CMakeLists.txt")
if git -C "$src" describe --exact-match --tags --match "v$ver" HEAD >/dev/null 2>&1; then
  echo "$ver"
else
  echo "$ver~git$(git -C "$src" log -1 --format=%cd --date=format:%Y%m%d).$(git -C "$src" rev-parse --short=7 HEAD)"
fi

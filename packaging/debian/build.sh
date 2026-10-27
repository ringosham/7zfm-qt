#!/bin/sh
# Builds a .deb of 7zfm-qt from this source tree, tests included.
#
#   packaging/debian/build.sh                         # on Debian/Ubuntu with the Build-Depends
#   packaging/debian/build.sh --docker debian:trixie  # anywhere with Docker
#
# Options:
#   --docker IMAGE  build inside a fresh container of IMAGE (installs the
#                   Build-Depends there)
#   --out DIR       where the .deb files go (default: dist/ in the source tree)
#   --nocheck       skip the tests
#
# Needs Qt >= 6.5: Debian 13 (trixie), Ubuntu 25.04 or later.
#
# The tracked files of the working tree (submodule included, uncommitted edits
# too) are copied to a scratch directory with packaging/debian/debian as its
# debian/, so the build never writes into the source tree.
set -eu

if [ "${1:-}" = --inside ]; then
  # ---- second stage: in the scratch directory, natively or in the container ----
  shift
  cd "$(dirname "$0")/7zfm-qt"
  export DEBIAN_FRONTEND=noninteractive
  if [ "${1:-}" = --install-deps ]; then
    apt-get update -q
    apt-get install -q -y --no-install-recommends dpkg-dev
    apt-get build-dep -q -y ./
  fi
  # Versions sort below the release they lead up to (0.1.0~git...) and carry
  # the distribution, so debs built for several of them never collide.
  . /etc/os-release
  case "$ID" in
    debian) dist="~deb${VERSION_ID:-sid}" ;;
    ubuntu) dist="~ubuntu$VERSION_ID" ;;
    *) dist= ;;
  esac
  cat > debian/changelog <<EOF
7zfm-qt ($DEBVER-1$dist) UNRELEASED; urgency=medium

  * Build of $GITREV.

 -- Ringo Sham <ringosham@gmail.com>  $(date -R)
EOF
  dpkg-buildpackage -b -us -uc
  mkdir -p ../out
  cp ../*.deb ../out/
  if [ -n "${HOST_UID:-}" ]; then chown -R "$HOST_UID:$HOST_GID" ..; fi
  exit 0
fi

image= out=
while [ $# -gt 0 ]; do
  case "$1" in
    --docker) image=$2; shift 2 ;;
    --out) out=$2; shift 2 ;;
    --nocheck) export DEB_BUILD_OPTIONS="${DEB_BUILD_OPTIONS:+$DEB_BUILD_OPTIONS }nocheck"; shift ;;
    *) echo "usage: $0 [--docker IMAGE] [--out DIR] [--nocheck]" >&2; exit 2 ;;
  esac
done

src=$(cd "$(dirname "$0")/../.." && pwd)
out=${out:-$src/dist}
if [ ! -f "$src/external/7zip/CPP/7zip/UI/Agent/Agent.cpp" ]; then
  echo "7-Zip submodule missing: run 'git submodule update --init'" >&2
  exit 1
fi

# 0.1.0 on the v0.1.0 tag, else 0.1.0~git20261027.b74c5c0
ver=$(sed -n 's/^project(7zfm-qt VERSION \([0-9.]*\).*/\1/p' "$src/CMakeLists.txt")
GITREV=$(git -C "$src" rev-parse --short=7 HEAD)
if git -C "$src" describe --exact-match --tags --match "v$ver" HEAD >/dev/null 2>&1; then
  DEBVER=$ver
else
  DEBVER="$ver~git$(git -C "$src" log -1 --format=%cd --date=format:%Y%m%d).$GITREV"
fi
export DEBVER GITREV

work=$(mktemp -d "${TMPDIR:-/tmp}/7zfm-qt-deb.XXXXXX")
trap 'rm -rf "$work"' EXIT
mkdir "$work/7zfm-qt"
(cd "$src" && git ls-files -z --recurse-submodules | tar --null -T - --ignore-failed-read -cf -) |
  tar -C "$work/7zfm-qt" -xf -
cp -r "$src/packaging/debian/debian" "$work/7zfm-qt/debian"
cp "$0" "$work/build.sh"

if [ -n "$image" ]; then
  docker run --rm -v "$work:/work" \
    -e DEBVER -e GITREV -e DEB_BUILD_OPTIONS="${DEB_BUILD_OPTIONS:-}" \
    -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
    "$image" sh /work/build.sh --inside --install-deps
else
  sh "$work/build.sh" --inside
fi

mkdir -p "$out"
cp "$work"/out/*.deb "$out/"
ls -l "$out"/*.deb

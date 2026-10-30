#!/bin/sh
# Builds an .rpm of 7zfm-qt from this source tree, tests included.
#
#   packaging/rpm/build.sh                     # on Fedora with the BuildRequires
#   packaging/rpm/build.sh --docker fedora:44  # anywhere with Docker
#
# Options:
#   --docker IMAGE  build inside a fresh container of IMAGE (installs the
#                   BuildRequires there)
#   --out DIR       where the .rpm files go (default: dist/ in the source tree)
#   --nocheck       skip the tests
#
# Needs Qt >= 6.5; tested on Fedora 44.
#
# The tracked files of the working tree (submodule included, uncommitted edits
# too) become the source tarball, so the build never writes into the source
# tree.
set -eu

if [ "${1:-}" = --inside ]; then
  # ---- second stage: in the scratch directory, natively or in the container ----
  shift
  # in a container: give the files back to the host user, even on failure
  if [ -n "${HOST_UID:-}" ]; then trap 'chown -R "$HOST_UID:$HOST_GID" "$(dirname "$0")"' EXIT; fi
  cd "$(dirname "$0")"
  if [ "${1:-}" = --install-deps ]; then
    dnf install -y -q rpm-build 'dnf-command(builddep)'
    dnf builddep -y -q --define "z7ver $Z7VER" ${Z7WITHOUT:+--without check} 7zfm-qt.spec
  fi
  rpmbuild -bb --define "_topdir $PWD/rpmbuild" --define "_sourcedir $PWD" \
    --define "z7ver $Z7VER" ${Z7WITHOUT:+--without check} 7zfm-qt.spec
  mkdir -p out
  cp rpmbuild/RPMS/*/*.rpm out/
  exit 0
fi

image= out= Z7WITHOUT=
while [ $# -gt 0 ]; do
  case "$1" in
    --docker) image=$2; shift 2 ;;
    --out) out=$2; shift 2 ;;
    --nocheck) Z7WITHOUT=1; shift ;;
    *) echo "usage: $0 [--docker IMAGE] [--out DIR] [--nocheck]" >&2; exit 2 ;;
  esac
done

src=$(cd "$(dirname "$0")/../.." && pwd)
out=${out:-$src/dist}
if [ ! -f "$src/external/7zip/CPP/7zip/UI/Agent/Agent.cpp" ]; then
  echo "7-Zip submodule missing: run 'git submodule update --init'" >&2
  exit 1
fi

Z7VER=$("$src/packaging/version.sh" "$src")
export Z7VER Z7WITHOUT

work=$(mktemp -d "${TMPDIR:-/tmp}/7zfm-qt-rpm.XXXXXX")
trap 'rm -rf "$work"' EXIT
(cd "$src" && git ls-files -z --recurse-submodules |
  tar --null -T - --ignore-failed-read --transform "s,^,7zfm-qt-$Z7VER/," -czf "$work/7zfm-qt-$Z7VER.tar.gz")
cp "$src/packaging/rpm/7zfm-qt.spec" "$work/"
cp "$0" "$work/build.sh"

if [ -n "$image" ]; then
  docker run --rm -v "$work:/work" \
    -e Z7VER -e Z7WITHOUT -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
    "$image" sh /work/build.sh --inside --install-deps
else
  sh "$work/build.sh" --inside
fi

mkdir -p "$out"
cp "$work"/out/*.rpm "$out/"
ls -l "$out"/*.rpm

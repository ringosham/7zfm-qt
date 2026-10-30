# Built by packaging/rpm/build.sh, which passes the version (z7ver) and
# the source tarball of the working tree.
%bcond_without check
# no LTO, as in the Arch and Debian packages
%global _lto_cflags %{nil}

Name:           7zfm-qt
Version:        %{z7ver}
Release:        1%{?dist}
Summary:        Qt port of the 7-Zip File Manager
License:        LGPL-2.1-or-later AND BSD-3-Clause AND BSD-2-Clause AND LicenseRef-unRAR
URL:            https://github.com/ringosham/7zfm-qt
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  cmake >= 3.21
BuildRequires:  desktop-file-utils
BuildRequires:  gcc-c++
BuildRequires:  libxcb-devel
BuildRequires:  ninja-build
BuildRequires:  python3
BuildRequires:  qt6-qtbase-devel >= 6.5
%if %{with check}
BuildRequires:  dbus-daemon
BuildRequires:  dosfstools
BuildRequires:  e2fsprogs
BuildRequires:  mtools
BuildRequires:  qemu-img
BuildRequires:  util-linux
BuildRequires:  xz
%endif
Recommends:     qt6-qtwayland
Recommends:     xdg-utils

%description
7zfm-qt is the Windows 7-Zip File Manager (7zFM) on Qt 6, built on the
unmodified 7-Zip engine. It opens archives, disk images and partitions by
signature rather than by extension, and opens nested items in place: a
partition inside a VHDX, the filesystem inside that partition, or a tar
inside an xz, without extracting them first.

It reads every format 7-Zip reads, including RAR, and can create, update,
test and extract archives, with one or two panels.

%prep
%autosetup -n %{name}-%{version}

%build
%cmake -G Ninja
%cmake_build

%install
%cmake_install

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/%{name}.desktop
%if %{with check}
%ctest
%endif

%files
%license LICENSE external/7zip/DOC/unRarLicense.txt
%doc README.md
%{_bindir}/%{name}
%{_datadir}/applications/%{name}.desktop
%{_datadir}/icons/hicolor/*/apps/%{name}.*

%changelog
* Fri Oct 30 2026 Ringo Sham <ringosham@gmail.com> - 0.1.0-1
- First RPM packaging.

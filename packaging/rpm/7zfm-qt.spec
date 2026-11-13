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
BuildRequires:  pkgconfig(libzstd)
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
%{_mandir}/man1/%{name}.1*
%{_datadir}/applications/%{name}.desktop
%{_datadir}/icons/hicolor/*/apps/%{name}.*
# co-owned: the file managers that read these may not be installed
%dir %{_datadir}/kio
%dir %{_datadir}/kio/servicemenus
%dir %{_datadir}/nemo
%dir %{_datadir}/nemo/actions
%dir %{_datadir}/file-manager
%dir %{_datadir}/file-manager/actions
%dir %{_libexecdir}/thunar-archive-plugin
%{_datadir}/kio/servicemenus/%{name}-*.desktop
%{_datadir}/nemo/actions/%{name}-*.nemo_action
%{_datadir}/file-manager/actions/%{name}-*.desktop
%{_libexecdir}/thunar-archive-plugin/%{name}.tap

%changelog
* Fri Nov 13 2026 Ringo Sham <ringosham@gmail.com> - 0.4.1-1
- Compressed tars open at the tar's contents; nested archives in memory are read-only.

* Tue Nov 10 2026 Ringo Sham <ringosham@gmail.com> - 0.4.0-1
- Compressed tar in one step: tgz, tbz2, txz and tzst (libzstd).

* Tue Nov 10 2026 Ringo Sham <ringosham@gmail.com> - 0.3.1-1
- 7zG command line, a man page, and full-width overwrite dialog buttons.

* Mon Nov 09 2026 Ringo Sham <ringosham@gmail.com> - 0.3.0-1
- 7-Zip context menus for Dolphin, Nemo, PCManFM-Qt, Caja and Thunar.

* Mon Nov 09 2026 Ringo Sham <ringosham@gmail.com> - 0.2.1-1
- 7-Zip 26.06; Combine also recognizes parts made by Linux split.

* Sun Nov 01 2026 Ringo Sham <ringosham@gmail.com> - 0.2.0-1
- 7-Zip's assembly LZMA decoder (built with uasm on x86_64).

* Fri Oct 30 2026 Ringo Sham <ringosham@gmail.com> - 0.1.1-1
- Symbolic links show the link emblem.

* Fri Oct 30 2026 Ringo Sham <ringosham@gmail.com> - 0.1.0-1
- First RPM packaging.

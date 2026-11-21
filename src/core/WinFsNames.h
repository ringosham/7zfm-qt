// WinFsNames.h -- 7zfm-qt: names Windows can't store, when extracting to a
// FAT, exFAT or NTFS volume (or an SMB share) on Linux.
//
// Upstream's Windows build replaces : * ? " < > | \ and control characters
// with '_', trailing dots and spaces likewise, and puts '_' before names
// such as CON or NUL (UI/Common/ExtractingFilePath.cpp). Its Linux build
// keeps every name as it is, which ext4 and the like accept; Linux's vfat
// and exfat drivers refuse those names, and ntfs3 stores a '\' that Windows
// then can't open. src/core/ExtractingFilePath.cpp, our copy, does what the
// Windows build does while g_Z7Qt_WindowsNames is set.

#ifndef Z7QT_WIN_FS_NAMES_H
#define Z7QT_WIN_FS_NAMES_H

#include "Common/MyString.h"

extern bool g_Z7Qt_WindowsNames;

// true if (path), or the nearest folder above it that exists, is on a file
// system that keeps Windows' rules for names
bool Z7Qt_IsWindowsFileSystem(const FString &path);

// sets g_Z7Qt_WindowsNames for an extraction into (outDir), for as long as
// it lives; Z7QT_WINDOWS_NAMES=1 or 0 in the environment overrides it
class CWindowsNamesScope
{
  bool _old;
public:
  explicit CWindowsNamesScope(const FString &outDir);
  ~CWindowsNamesScope();
};

#endif

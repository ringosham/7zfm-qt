// Force-included (-include) into the upstream files that use
// Windows/Registry.h, so they compile unchanged on POSIX. The CKey class
// itself is implemented on QSettings in RegistryQt.cpp.

#ifndef Z7QT_REGISTRY_COMPAT_H
#define Z7QT_REGISTRY_COMPAT_H

#include "Common/MyWindows.h"

struct Z7RegKey;
typedef Z7RegKey *HKEY;
typedef DWORD REGSAM;
typedef DWORD *LPDWORD;
typedef BYTE *LPBYTE;
typedef TCHAR *LPTSTR;
typedef void *LPSECURITY_ATTRIBUTES;

// FileManager/ViewSettings.h stores the window placement as a RECT.
typedef struct tagRECT { LONG left; LONG top; LONG right; LONG bottom; } RECT;

extern HKEY const Z7_HKEY_CURRENT_USER;
#define HKEY_CURRENT_USER Z7_HKEY_CURRENT_USER

#ifndef TEXT
#define TEXT(x) x
#endif

#define REG_NONE 0
#define REG_OPTION_NON_VOLATILE 0
#define KEY_READ 0x20019
#define KEY_WRITE 0x20006
#define KEY_ALL_ACCESS 0xF003F

#ifndef ERROR_SUCCESS
#define ERROR_SUCCESS 0L
#endif
#ifndef ERROR_FILE_NOT_FOUND
#define ERROR_FILE_NOT_FOUND 2L
#endif
#ifndef ERROR_INVALID_DATA
#define ERROR_INVALID_DATA 13L
#endif

// Referenced only by an inline private helper in Registry.h that our
// CKey implementation never calls.
inline LONG RegQueryValueEx(HKEY, LPCTSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD)
{
  return ERROR_FILE_NOT_FOUND;
}

#endif

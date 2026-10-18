// Link seams: definitions for symbols that upstream sources reference but
// only the Windows build (or the Windows FileManager) provides.

#include "Common/Common.h"
#include "Common/MyString.h"
#include "Windows/DLL.h"

#include <limits.h>
#include <unistd.h>

// Defined in FileManager/PanelSort.cpp upstream; the Agent uses it to sort
// folder listings. Copied verbatim: "natural" order, case-insensitive.
int CompareFileNames_ForFolderList(const wchar_t *s1, const wchar_t *s2);
int CompareFileNames_ForFolderList(const wchar_t *s1, const wchar_t *s2)
{
  for (;;)
  {
    wchar_t c1 = *s1;
    wchar_t c2 = *s2;
    if ((c1 >= '0' && c1 <= '9') &&
        (c2 >= '0' && c2 <= '9'))
    {
      for (; *s1 == '0'; s1++);
      for (; *s2 == '0'; s2++);
      size_t len1 = 0;
      size_t len2 = 0;
      for (; (s1[len1] >= '0' && s1[len1] <= '9'); len1++);
      for (; (s2[len2] >= '0' && s2[len2] <= '9'); len2++);
      if (len1 < len2) return -1;
      if (len1 > len2) return 1;
      for (; len1 > 0; s1++, s2++, len1--)
      {
        if (*s1 == *s2) continue;
        return (*s1 < *s2) ? -1 : 1;
      }
      c1 = *s1;
      c2 = *s2;
    }
    s1++;
    s2++;
    if (c1 != c2)
    {
      wchar_t u1 = MyCharUpper(c1);
      wchar_t u2 = MyCharUpper(c2);
      if (u1 < u2) return -1;
      if (u1 > u2) return 1;
    }
    if (c1 == 0) return 0;
  }
}

namespace NWindows {
namespace NDLL {

// Windows-only upstream (GetModuleFileNameW); the Agent asks for it when
// reporting an icon path.
bool MyGetModuleFileName(FString &path)
{
  char buf[PATH_MAX + 1];
  const ssize_t n = readlink("/proc/self/exe", buf, PATH_MAX);
  if (n <= 0)
    return false;
  buf[n] = 0;
  path = buf;
  return true;
}

}}

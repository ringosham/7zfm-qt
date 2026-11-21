// WinFsNames.cpp -- 7zfm-qt: see WinFsNames.h.

#include "WinFsNames.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <fstream>
#include <sstream>
#include <string>

bool g_Z7Qt_WindowsNames = false;

// mountinfo escapes ' ', '\t', '\n' and '\\' as \ooo
static std::string Unescape(const std::string &s)
{
  std::string r;
  for (size_t i = 0; i < s.size(); i++)
  {
    if (s[i] == '\\' && i + 3 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '7')
    {
      r += (char)(((s[i + 1] - '0') << 6) | ((s[i + 2] - '0') << 3) | (s[i + 3] - '0'));
      i += 3;
    }
    else
      r += s[i];
  }
  return r;
}

static bool IsUnder(const std::string &path, const std::string &mountPoint)
{
  if (mountPoint == "/")
    return true;
  return path.compare(0, mountPoint.size(), mountPoint) == 0
      && (path.size() == mountPoint.size() || path[mountPoint.size()] == '/');
}

bool Z7Qt_IsWindowsFileSystem(const FString &pathIn)
{
  // the folder may not exist yet: the nearest one above it that does
  std::string path = pathIn.Ptr();
  if (path.empty())
    return false;
  struct stat st;
  while (stat(path.c_str(), &st) != 0)
  {
    while (path.size() > 1 && path.back() == '/')
      path.pop_back();
    const size_t slash = path.rfind('/');
    if (slash == std::string::npos)
    {
      path = ".";
      break;
    }
    path = slash == 0 ? "/" : path.substr(0, slash);
  }
  char resolved[PATH_MAX];
  if (!realpath(path.c_str(), resolved))
    return false;
  const std::string real = resolved;

  // the mount with the longest mount point holding (real)
  std::ifstream f("/proc/self/mountinfo");
  std::string line, bestMount, bestType;
  while (std::getline(f, line))
  {
    std::istringstream ss(line);
    std::string field, mountPoint, type;
    for (int i = 0; i < 5 && ss >> field; i++)
      if (i == 4)
        mountPoint = Unescape(field);
    while (ss >> field && field != "-")
      ;
    ss >> type;
    if (mountPoint.empty() || type.empty() || !IsUnder(real, mountPoint))
      continue;
    if (mountPoint.size() >= bestMount.size())
    {
      bestMount = mountPoint;
      bestType = type;
    }
  }
  // fuseblk: ntfs-3g and exfat-fuse; cifs / smb3: Windows' own shares
  static const char * const kTypes[] =
    { "vfat", "msdos", "exfat", "ntfs", "ntfs3", "fuseblk", "cifs", "smb3", "smbfs" };
  for (const char *t : kTypes)
    if (bestType == t)
      return true;
  return false;
}

CWindowsNamesScope::CWindowsNamesScope(const FString &outDir):
    _old(g_Z7Qt_WindowsNames)
{
  // Z7QT_WINDOWS_NAMES=1 or 0 decides for every destination (for tests: a
  // FAT image mounted through FUSE shows as "fuse")
  const char *force = getenv("Z7QT_WINDOWS_NAMES");
  if (force && (force[0] == '0' || force[0] == '1'))
    g_Z7Qt_WindowsNames = !outDir.IsEmpty() && force[0] == '1';
  else
    g_Z7Qt_WindowsNames = !outDir.IsEmpty() && Z7Qt_IsWindowsFileSystem(outDir);
}

CWindowsNamesScope::~CWindowsNamesScope()
{
  g_Z7Qt_WindowsNames = _old;
}

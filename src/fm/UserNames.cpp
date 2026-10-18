// UserNames.cpp

#include "UserNames.h"

#include "Common/IntToString.h"
#include "Common/StringConvert.h"

#include <grp.h>
#include <pwd.h>

#include <map>
#include <mutex>

static std::mutex g_Mutex;
static std::map<uid_t, UString> g_Users;
static std::map<gid_t, UString> g_Groups;

static UString Num(UInt32 v)
{
  wchar_t s[16];
  ConvertUInt32ToString(v, s);
  return UString(s);
}

UString GetUserName(uid_t uid)
{
  std::lock_guard<std::mutex> lock(g_Mutex);
  auto it = g_Users.find(uid);
  if (it != g_Users.end())
    return it->second;
  const struct passwd *pw = getpwuid(uid);
  const UString s = pw ? fs2us(FString(pw->pw_name)) : Num(uid);
  g_Users[uid] = s;
  return s;
}

UString GetGroupName(gid_t gid)
{
  std::lock_guard<std::mutex> lock(g_Mutex);
  auto it = g_Groups.find(gid);
  if (it != g_Groups.end())
    return it->second;
  const struct group *gr = getgrgid(gid);
  const UString s = gr ? fs2us(FString(gr->gr_name)) : Num(gid);
  g_Groups[gid] = s;
  return s;
}

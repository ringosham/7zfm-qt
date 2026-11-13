// TempCleanup.cpp -- see TempCleanup.h. Upstream leaves stale %TEMP%\7zO*
// folders to Windows; nothing cleans ~/.cache here, so we do it ourselves.

#include "TempCleanup.h"

#include <QFile>
#include <QStandardPaths>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>

#include <unistd.h>

namespace fs = std::filesystem;

static std::string ReadComm(const fs::path &procDir)
{
  std::ifstream f(procDir / "comm");
  std::string s;
  std::getline(f, s);
  return s;
}

// any 7zfm-qt process but this one, of any user: one run with sudo may
// still use this user's cache. A process hidden from us (hidepid) is missed.
static bool OtherInstanceRunning()
{
  const std::string self = ReadComm("/proc/self");
  const std::string myPid = std::to_string(getpid());
  std::error_code ec;
  for (fs::directory_iterator it("/proc", ec), end; !ec && it != end; it.increment(ec))
  {
    const std::string pid = it->path().filename().string();
    if (pid.empty() || pid.find_first_not_of("0123456789") != std::string::npos || pid == myPid)
      continue;
    const std::string comm = ReadComm(it->path());
    if (comm == "7zfm-qt" || (!self.empty() && comm == self))
      return true;
  }
  return ec.value() != 0; // couldn't list /proc: assume there is one
}

static void Clean(const fs::path &base, fs::file_time_type startTime)
{
  if (OtherInstanceRunning())
    return;
  std::error_code ec;
  for (fs::directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec))
  {
    const std::string name = it->path().filename().string();
    if (name.compare(0, 4, "7zO.") != 0)
      continue;
    std::error_code ec2;
    // not through a symlink; and not one an instance started since made
    if (!it->is_directory(ec2) || it->is_symlink(ec2))
      continue;
    const fs::file_time_type t = fs::last_write_time(it->path(), ec2);
    if (ec2 || t >= startTime)
      continue;
    fs::remove_all(it->path(), ec2);
  }
}

void StartTempCleanup()
{
  // the folder Panel::CreateTempDir uses
  const QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
  if (base.isEmpty())
    return;
  const fs::path path(QFile::encodeName(base).toStdString());
  const fs::file_time_type now = fs::file_time_type::clock::now();
  try
  {
    std::thread([path, now]() {
      try { Clean(path, now); }
      catch (...) {}
    }).detach();
  }
  catch (...) {}
}

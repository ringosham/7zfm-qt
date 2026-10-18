// FsFolderCopy.cpp -- IFolderOperations of the POSIX file-system folder.
// Same contract as upstream FileManager/FSFolderCopy.cpp: overwrite and
// auto-rename decisions come from callback->AskWrite(), progress goes to
// SetTotal()/SetCompleted(), errors to ShowMessage().

#include "FsFolder.h"

#include "Common/ComTry.h"
#include "Common/StringConvert.h"
#include "Windows/FileDir.h"
#include "Windows/FileName.h"
#include "Windows/TimeUtils.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <string>

using namespace NWindows;
using namespace NWindows::NFile;

namespace NFsFolder {

static HRESULT ErrnoHRESULT(int e) { return e ? HRESULT_FROM_WIN32((DWORD)e) : E_FAIL; }

static FString Join(const FString &dir, const FString &name)
{
  FString s = dir;
  if (!s.IsEmpty() && s.Back() != '/')
    s.Add_Char('/');
  s += name;
  return s;
}

static bool IsDots(const char *n)
{
  return n[0] == '.' && (n[1] == 0 || (n[1] == '.' && n[2] == 0));
}

// ---- sizes for the progress total ----

static void AddSize(const FString &path, UInt64 &total, UInt64 &numFiles)
{
  struct stat st;
  if (lstat(path, &st) != 0)
    return;
  if (S_ISDIR(st.st_mode))
  {
    DIR *d = opendir(path);
    if (!d)
      return;
    while (const dirent *de = readdir(d))
      if (!IsDots(de->d_name))
        AddSize(Join(path, FString(de->d_name)), total, numFiles);
    closedir(d);
    return;
  }
  numFiles++;
  if (S_ISREG(st.st_mode))
    total += (UInt64)st.st_size;
}

struct CCopyState
{
  IFolderOperationsExtractCallback *Callback;
  bool MoveMode;
  UInt64 Completed;
  UInt64 Total;

  HRESULT Error(const char *message, const FString &path)
  {
    UString s(message);
    s += " : ";
    s += fs2us(path);
    return Callback->ShowMessage(s);
  }
  HRESULT ErrorErrno(int e, const FString &path)
  {
    UString s(strerror(e));
    s += " : ";
    s += fs2us(path);
    return Callback->ShowMessage(s);
  }
};

static void CopyTimesAndMode(const struct stat &st, const FString &dest)
{
  struct timespec ts[2] = { st.st_atim, st.st_mtim };
  utimensat(AT_FDCWD, dest, ts, AT_SYMLINK_NOFOLLOW);
  if (!S_ISLNK(st.st_mode))
    chmod(dest, st.st_mode & 07777);
}

static HRESULT CopyFileData(CCopyState &state, const FString &src, const FString &dest, const struct stat &st)
{
  if (S_ISLNK(st.st_mode))
  {
    char buf[4096];
    const ssize_t n = readlink(src, buf, sizeof(buf) - 1);
    if (n < 0)
      return state.ErrorErrno(errno, src);
    buf[n] = 0;
    unlink(dest);
    if (symlink(buf, dest) != 0)
      return state.ErrorErrno(errno, dest);
    return S_OK;
  }
  if (!S_ISREG(st.st_mode))
    return state.Error("Unsupported file type", src);

  const int in = open(src, O_RDONLY | O_CLOEXEC);
  if (in < 0)
    return state.ErrorErrno(errno, src);
  const int out = open(dest, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (out < 0)
  {
    const int e = errno;
    close(in);
    return state.ErrorErrno(e, dest);
  }
  HRESULT res = S_OK;
  static const size_t kBufSize = (size_t)1 << 20;
  std::string buf(kBufSize, '\0');
  for (;;)
  {
    const ssize_t n = read(in, &buf[0], kBufSize);
    if (n == 0)
      break;
    if (n < 0)
    {
      if (errno == EINTR)
        continue;
      res = state.ErrorErrno(errno, src);
      if (res == S_OK) res = E_ABORT;
      break;
    }
    for (ssize_t done = 0; done < n;)
    {
      const ssize_t w = write(out, buf.data() + done, (size_t)(n - done));
      if (w < 0)
      {
        if (errno == EINTR)
          continue;
        res = state.ErrorErrno(errno, dest);
        if (res == S_OK) res = E_ABORT;
        break;
      }
      done += w;
    }
    if (res != S_OK)
      break;
    state.Completed += (UInt64)n;
    res = state.Callback->SetCompleted(&state.Completed);
    if (res != S_OK)
      break;
  }
  close(in);
  if (close(out) != 0 && res == S_OK)
    res = state.ErrorErrno(errno, dest);
  if (res != S_OK)
  {
    unlink(dest);
    return res;
  }
  CopyTimesAndMode(st, dest);
  return S_OK;
}

static HRESULT RemoveTree(const FString &path)
{
  struct stat st;
  if (lstat(path, &st) != 0)
    return ErrnoHRESULT(errno);
  if (S_ISDIR(st.st_mode))
  {
    DIR *d = opendir(path);
    if (!d)
      return ErrnoHRESULT(errno);
    HRESULT res = S_OK;
    while (const dirent *de = readdir(d))
    {
      if (IsDots(de->d_name))
        continue;
      const HRESULT r = RemoveTree(Join(path, FString(de->d_name)));
      if (r != S_OK)
        res = r;
    }
    closedir(d);
    if (res != S_OK)
      return res;
    return rmdir(path) == 0 ? S_OK : ErrnoHRESULT(errno);
  }
  return unlink(path) == 0 ? S_OK : ErrnoHRESULT(errno);
}

static bool IsDestChild(const FString &src, const FString &dest)
{
  FString s = src;
  if (s.IsEmpty() || s.Back() != '/')
    s.Add_Char('/');
  return dest.IsPrefixedBy(s) || dest == src;
}

static HRESULT CopyItem(CCopyState &state, const FString &src, const FString &dest);

static HRESULT CopyFolder(CCopyState &state, const FString &src, const FString &dest, const struct stat &st)
{
  if (IsDestChild(src, dest))
  {
    RINOK(state.Error(state.MoveMode ? "Cannot move folder into itself" : "Cannot copy folder into itself", dest))
    return E_ABORT;
  }
  if (state.MoveMode && rename(src, dest) == 0)
  {
    UInt64 t = 0, n = 0;
    AddSize(dest, t, n);
    state.Completed += t;
    return state.Callback->SetCompleted(&state.Completed);
  }
  if (!NDir::CreateComplexDir(dest))
    return state.ErrorErrno(errno, dest);
  DIR *d = opendir(src);
  if (!d)
    return state.ErrorErrno(errno, src);
  HRESULT res = S_OK;
  while (const dirent *de = readdir(d))
  {
    if (IsDots(de->d_name))
      continue;
    const FString name(de->d_name);
    res = CopyItem(state, Join(src, name), Join(dest, name));
    if (res != S_OK)
      break;
  }
  closedir(d);
  RINOK(res)
  CopyTimesAndMode(st, dest);
  if (state.MoveMode)
    rmdir(src);  // fails, and is kept, if something was skipped
  return S_OK;
}

static HRESULT CopyItem(CCopyState &state, const FString &src, const FString &dest)
{
  struct stat st;
  if (lstat(src, &st) != 0)
    return state.ErrorErrno(errno, src);
  if (S_ISDIR(st.st_mode))
    return CopyFolder(state, src, dest, st);

  if (dest == src)
  {
    RINOK(state.Error(state.MoveMode ? "Cannot move file onto itself" : "Cannot copy file onto itself", dest))
    return E_ABORT;
  }

  // upstream CopyFile_Ask(): the callback decides about existing files
  FILETIME ft;
  FiTime_To_FILETIME(st.st_mtim, ft);
  const UInt64 size = (UInt64)st.st_size;
  Int32 writeAskResult = 0;
  CMyComBSTR destPathResult;
  RINOK(state.Callback->AskWrite(fs2us(src), BoolToInt(false), &ft, &size,
      fs2us(dest), &destPathResult, &writeAskResult))
  if (!IntToBool(writeAskResult))
  {
    state.Completed += size;
    return state.Callback->SetCompleted(&state.Completed);
  }
  const FString destNew = us2fs((const wchar_t *)destPathResult);
  RINOK(state.Callback->SetCurrentFilePath(fs2us(src)))

  if (state.MoveMode && rename(src, destNew) == 0)
  {
    state.Completed += size;
    return state.Callback->SetCompleted(&state.Completed);
  }
  RINOK(CopyFileData(state, src, destNew, st))
  if (state.MoveMode && unlink(src) != 0)
    return state.ErrorErrno(errno, src);
  return S_OK;
}

HRESULT CopyFileSystemItems(
    const UStringVector &itemsPaths,
    const FString &destDirPrefix,
    bool moveMode,
    IFolderOperationsExtractCallback *callback)
{
  if (itemsPaths.IsEmpty())
    return S_OK;
  CCopyState state;
  state.Callback = callback;
  state.MoveMode = moveMode;
  state.Completed = 0;
  state.Total = 0;
  UInt64 numFiles = 0;
  FOR_VECTOR (i, itemsPaths)
    AddSize(us2fs(itemsPaths[i]), state.Total, numFiles);
  RINOK(callback->SetNumFiles(numFiles))
  RINOK(callback->SetTotal(state.Total))
  RINOK(callback->SetCompleted(&state.Completed))

  FString destDir = destDirPrefix;
  NName::NormalizeDirPathPrefix(destDir);
  if (!NDir::CreateComplexDir(destDir))
    return state.ErrorErrno(errno, destDir);
  FOR_VECTOR (i, itemsPaths)
  {
    FString src = us2fs(itemsPaths[i]);
    if (src.Len() > 1 && src.Back() == '/')
      src.DeleteBack();
    const int pos = src.ReverseFind_PathSepar();
    const FString name (src.Ptr((unsigned)(pos + 1)));
    RINOK(CopyItem(state, src, destDir + name))
  }
  return S_OK;
}

// ---------- CFsFolder : IFolderOperations ----------

Z7_COM7F_IMF(CFsFolder::CreateFolder(const wchar_t *name, IProgress * /* progress */))
{
  const FString path = _path + us2fs(name);
  if (mkdir(path, 0777) == 0)
    return S_OK;
  if (errno == ENOENT && NDir::CreateComplexDir(path))
    return S_OK;
  return ErrnoHRESULT(errno);
}

Z7_COM7F_IMF(CFsFolder::CreateFile(const wchar_t *name, IProgress * /* progress */))
{
  const int fd = open(_path + us2fs(name), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
  if (fd < 0)
    return ErrnoHRESULT(errno);
  close(fd);
  return S_OK;
}

Z7_COM7F_IMF(CFsFolder::Rename(UInt32 index, const wchar_t *newName, IProgress * /* progress */))
{
  if (index >= _items.Size())
    return E_INVALIDARG;
  const FString src = _path + _items[index].Name;
  const FString dest = _path + us2fs(newName);
  struct stat st;
  if (lstat(dest, &st) == 0 && dest != src)
    return HRESULT_FROM_WIN32(EEXIST);
  return rename(src, dest) == 0 ? S_OK : ErrnoHRESULT(errno);
}

Z7_COM7F_IMF(CFsFolder::Delete(const UInt32 *indices, UInt32 numItems, IProgress *progress))
{
  // permanent delete; the panel uses the trash (QFile::moveToTrash) unless
  // Shift is held, like Explorer's recycle bin in 7zFM
  RINOK(progress ? progress->SetTotal(numItems) : S_OK)
  for (UInt32 i = 0; i < numItems; i++)
  {
    if (indices[i] >= _items.Size())
      return E_INVALIDARG;
    RINOK(RemoveTree(_path + _items[indices[i]].Name))
    if (progress)
    {
      const UInt64 done = i + 1;
      RINOK(progress->SetCompleted(&done))
    }
  }
  return S_OK;
}

Z7_COM7F_IMF(CFsFolder::CopyTo(Int32 moveMode, const UInt32 *indices, UInt32 numItems,
    Int32 /* includeAltStreams */, Int32 /* replaceAltStreamCharsMode */,
    const wchar_t *path, IFolderOperationsExtractCallback *callback))
{
  if (numItems == 0)
    return S_OK;
  UStringVector paths;
  for (UInt32 i = 0; i < numItems; i++)
  {
    if (indices[i] >= _items.Size())
      return E_INVALIDARG;
    paths.Add(fs2us(_path + _items[indices[i]].Name));
  }
  FString dest = us2fs(path);
  // single item and a destination that is not a folder: copy to that name
  if (numItems == 1 && !dest.IsEmpty() && dest.Back() != '/')
  {
    struct stat st;
    if (stat(dest, &st) != 0 || !S_ISDIR(st.st_mode))
    {
      CCopyState state;
      state.Callback = callback;
      state.MoveMode = moveMode != 0;
      state.Completed = 0;
      state.Total = 0;
      UInt64 n = 0;
      AddSize(us2fs(paths[0]), state.Total, n);
      RINOK(callback->SetNumFiles(n))
      RINOK(callback->SetTotal(state.Total))
      return CopyItem(state, us2fs(paths[0]), dest);
    }
  }
  return CopyFileSystemItems(paths, dest, moveMode != 0, callback);
}

Z7_COM7F_IMF(CFsFolder::CopyFrom(Int32 /* moveMode */, const wchar_t * /* fromFolderPath */,
    const wchar_t * const * /* itemsPaths */, UInt32 /* numItems */, IProgress * /* progress */))
{
  return E_NOTIMPL;
}

Z7_COM7F_IMF(CFsFolder::SetProperty(UInt32 /* index */, PROPID /* propID */,
    const PROPVARIANT * /* value */, IProgress * /* progress */))
{
  return E_NOTIMPL;
}

Z7_COM7F_IMF(CFsFolder::CopyFromFile(UInt32 /* index */, const wchar_t * /* fullFilePath */, IProgress * /* progress */))
{
  return E_NOTIMPL;
}

}

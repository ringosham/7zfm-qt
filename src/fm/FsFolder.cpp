// FsFolder.cpp -- POSIX file-system folder.

#include "FsFolder.h"

#include "Common/ComTry.h"
#include "Common/StringConvert.h"
#include "Windows/PropVariant.h"
#include "Windows/TimeUtils.h"

#include "7zip/PropID.h"

#include "RootFolder.h"
#include "UserNames.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace NWindows;

namespace NFsFolder {

static const PROPID kProps[] =
{
  kpidName,
  kpidSize,
  kpidMTime,
  kpidCTime,
  kpidChangeTime,
  kpidATime,
  kpidPosixAttrib,
  kpidUser,
  kpidGroup,
  kpidLinks,
  kpidSymLink,
  kpidINode
};

static HRESULT ErrnoToHRESULT(int e)
{
  return e ? HRESULT_FROM_WIN32((DWORD)e) : E_FAIL;
}

bool FillItem(int dirFd, const char *name, CItem &item)
{
  struct statx sx;
  if (statx(dirFd, name, AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT,
      STATX_BASIC_STATS | STATX_BTIME, &sx) != 0)
    return false;
  item.Name = name;
  item.UName = fs2us(item.Name);
  item.Size = sx.stx_size;
  item.INode = sx.stx_ino;
  item.Mode = sx.stx_mode;
  item.NumLinks = sx.stx_nlink;
  item.Uid = sx.stx_uid;
  item.Gid = sx.stx_gid;
  item.MTime.tv_sec = sx.stx_mtime.tv_sec; item.MTime.tv_nsec = sx.stx_mtime.tv_nsec;
  item.ATime.tv_sec = sx.stx_atime.tv_sec; item.ATime.tv_nsec = sx.stx_atime.tv_nsec;
  item.ChangeTime.tv_sec = sx.stx_ctime.tv_sec; item.ChangeTime.tv_nsec = sx.stx_ctime.tv_nsec;
  item.BirthTime_Defined = (sx.stx_mask & STATX_BTIME) != 0;
  item.BirthTime.tv_sec = sx.stx_btime.tv_sec; item.BirthTime.tv_nsec = sx.stx_btime.tv_nsec;
  item.IsLink = S_ISLNK(item.Mode);
  item.IsDir = S_ISDIR(item.Mode);
  if (item.IsLink)
  {
    struct stat st;
    if (fstatat(dirFd, name, &st, 0) == 0)
      item.IsDir = S_ISDIR(st.st_mode);
  }
  if (item.IsDir)
    item.Size = 0;
  return true;
}

static bool GetDirMTime(const FString &path, timespec &t)
{
  struct stat st;
  if (stat(path, &st) != 0)
    return false;
  t = st.st_mtim;
  return true;
}

HRESULT CFsFolder::Init(const FString &path)
{
  _path = path;
  if (_path.IsEmpty() || _path.Back() != '/')
    _path.Add_Char('/');
  struct stat st;
  if (stat(_path, &st) != 0)
    return ErrnoToHRESULT(errno);
  if (!S_ISDIR(st.st_mode))
    return E_INVALIDARG;
  _dirMTime = st.st_mtim;
  return S_OK;
}

Z7_COM7F_IMF(CFsFolder::LoadItems())
{
  _items.Clear();
  GetDirMTime(_path, _dirMTime);
  DIR *dir = opendir(_path);
  if (!dir)
    return ErrnoToHRESULT(errno);
  const int fd = dirfd(dir);
  for (;;)
  {
    errno = 0;
    const struct dirent *de = readdir(dir);
    if (!de)
      break;
    const char *n = de->d_name;
    if (n[0] == '.' && (n[1] == 0 || (n[1] == '.' && n[2] == 0)))
      continue;
    CItem item;
    if (!FillItem(fd, n, item))
    {
      // keep entries we cannot stat (e.g. permission denied in a sticky dir)
      item = CItem();
      item.Name = n;
      item.UName = fs2us(item.Name);
      item.Size = 0; item.INode = de->d_ino; item.Mode = 0; item.NumLinks = 0;
      item.Uid = 0; item.Gid = 0;
      item.MTime = item.ATime = item.ChangeTime = item.BirthTime = timespec();
      item.BirthTime_Defined = false;
      item.IsDir = (de->d_type == DT_DIR);
      item.IsLink = (de->d_type == DT_LNK);
    }
    _items.Add(item);
  }
  closedir(dir);
  return S_OK;
}

Z7_COM7F_IMF(CFsFolder::GetNumberOfItems(UInt32 *numItems))
{
  *numItems = _items.Size();
  return S_OK;
}

static void SetTime(NCOM::CPropVariant &prop, const timespec &ts)
{
  if (ts.tv_sec == 0 && ts.tv_nsec == 0)
    return;
  FILETIME ft;
  unsigned ns100;
  FiTime_To_FILETIME_ns100(ts, ft, ns100);
  prop.SetAsTimeFrom_FT_Prec_Ns100(ft, k_PropVar_TimePrec_1ns, ns100);
}

Z7_COM7F_IMF(CFsFolder::GetProperty(UInt32 index, PROPID propID, PROPVARIANT *value))
{
  COM_TRY_BEGIN
  NCOM::CPropVariant prop;
  if (index >= _items.Size())
    return E_INVALIDARG;
  const CItem &item = _items[index];
  switch (propID)
  {
    case kpidIsDir: prop = item.IsDir; break;
    case kpidName: prop = item.UName; break;
    case kpidSize: if (!item.IsDir) prop = item.Size; break;
    case kpidMTime: SetTime(prop, item.MTime); break;
    case kpidATime: SetTime(prop, item.ATime); break;
    case kpidChangeTime: SetTime(prop, item.ChangeTime); break;
    case kpidCTime: if (item.BirthTime_Defined) SetTime(prop, item.BirthTime); break;
    case kpidPosixAttrib: if (item.Mode) prop = (UInt32)item.Mode; break;
    case kpidUser: if (item.Mode) prop = GetUserName(item.Uid); break;
    case kpidGroup: if (item.Mode) prop = GetGroupName(item.Gid); break;
    case kpidLinks: if (item.NumLinks) prop = (UInt32)item.NumLinks; break;
    case kpidINode: prop = item.INode; break;
    case kpidSymLink:
      if (item.IsLink)
      {
        char buf[4096];
        const ssize_t n = readlink(_path + item.Name, buf, sizeof(buf) - 1);
        if (n > 0)
        {
          buf[n] = 0;
          prop = fs2us(FString(buf));
        }
      }
      break;
  }
  prop.Detach(value);
  return S_OK;
  COM_TRY_END
}

Z7_COM7F_IMF(CFsFolder::GetItemName(UInt32 index, const wchar_t **name, unsigned *len))
{
  if (index >= _items.Size())
    return E_INVALIDARG;
  const UString &s = _items[index].UName;
  *name = s.Ptr();
  *len = s.Len();
  return S_OK;
}

Z7_COM7F_IMF(CFsFolder::GetItemPrefix(UInt32 /* index */, const wchar_t **name, unsigned *len))
{
  *name = L"";
  *len = 0;
  return S_OK;
}

Z7_COM7F_IMF2(UInt64, CFsFolder::GetItemSize(UInt32 index))
{
  if (index >= _items.Size())
    return 0;
  return _items[index].Size;
}

static HRESULT MakeFolder(const FString &path, IFolderFolder **resultFolder)
{
  CFsFolder *spec = new CFsFolder;
  CMyComPtr<IFolderFolder> f = spec;
  RINOK(spec->Init(path))
  *resultFolder = f.Detach();
  return S_OK;
}

Z7_COM7F_IMF(CFsFolder::BindToFolder(UInt32 index, IFolderFolder **resultFolder))
{
  *resultFolder = NULL;
  if (index >= _items.Size())
    return E_INVALIDARG;
  const CItem &item = _items[index];
  if (!item.IsDir)
    return E_INVALIDARG;
  return MakeFolder(_path + item.Name, resultFolder);
}

Z7_COM7F_IMF(CFsFolder::BindToFolder(const wchar_t *name, IFolderFolder **resultFolder))
{
  *resultFolder = NULL;
  const FString n = us2fs(name);
  if (!n.IsEmpty() && n[0] == '/')
    return MakeFolder(n, resultFolder);
  return MakeFolder(_path + n, resultFolder);
}

Z7_COM7F_IMF(CFsFolder::BindToParentFolder(IFolderFolder **resultFolder))
{
  *resultFolder = NULL;
  if (_path.Len() <= 1)
  {
    // "/" -> the "Computer" root, like a drive root goes to CFSDrives upstream.
    CRootFolder *rootSpec = new CRootFolder;
    CMyComPtr<IFolderFolder> root = rootSpec;
    rootSpec->Init();
    *resultFolder = root.Detach();
    return S_OK;
  }
  FString parent = _path;
  parent.DeleteBack();
  const int pos = parent.ReverseFind_PathSepar();
  if (pos < 0)
    return S_OK;
  parent.DeleteFrom((unsigned)pos + 1);
  return MakeFolder(parent, resultFolder);
}

Z7_COM7F_IMF(CFsFolder::GetNumberOfProperties(UInt32 *numProperties))
{
  *numProperties = Z7_ARRAY_SIZE(kProps);
  return S_OK;
}

Z7_COM7F_IMF(CFsFolder::GetPropertyInfo(UInt32 index, BSTR *name, PROPID *propID, VARTYPE *varType))
{
  if (index >= Z7_ARRAY_SIZE(kProps))
    return E_INVALIDARG;
  *propID = kProps[index];
  *varType = k7z_PROPID_To_VARTYPE[(unsigned)*propID];
  *name = NULL;
  return S_OK;
}

Z7_COM7F_IMF(CFsFolder::GetFolderProperty(PROPID propID, PROPVARIANT *value))
{
  COM_TRY_BEGIN
  NCOM::CPropVariant prop;
  switch (propID)
  {
    case kpidType: prop = "FSFolder"; break;
    case kpidPath: prop = fs2us(_path); break;
  }
  prop.Detach(value);
  return S_OK;
  COM_TRY_END
}

Z7_COM7F_IMF(CFsFolder::Clone(IFolderFolder **resultFolder))
{
  return MakeFolder(_path, resultFolder);
}

Z7_COM7F_IMF(CFsFolder::WasChanged(Int32 *wasChanged))
{
  timespec t;
  *wasChanged = (GetDirMTime(_path, t)
      && (t.tv_sec != _dirMTime.tv_sec || t.tv_nsec != _dirMTime.tv_nsec)) ? 1 : 0;
  return S_OK;
}

}

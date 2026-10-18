// FsFolder.h -- POSIX file-system folder (replaces upstream FSFolder.cpp,
// which is built on NT APIs). Same folder protocol as upstream: kpidType is
// "FSFolder", kpidPath is the directory with a trailing '/'.

#ifndef Z7QT_FS_FOLDER_H
#define Z7QT_FS_FOLDER_H

#include "Common/MyCom.h"
#include "Common/MyString.h"
#include "Common/MyVector.h"

#include "7zip/UI/FileManager/IFolder.h"

#include <sys/types.h>
#include <time.h>

namespace NFsFolder {

struct CItem
{
  FString Name;
  UString UName;
  UInt64 Size;
  UInt64 INode;
  UInt32 Mode;      // lstat() mode
  UInt32 NumLinks;
  uid_t Uid;
  gid_t Gid;
  timespec MTime;
  timespec ATime;
  timespec ChangeTime;
  timespec BirthTime;
  bool BirthTime_Defined;
  bool IsDir;       // follows symlinks, so links to dirs can be entered
  bool IsLink;
};

class CFsFolder Z7_final:
  public IFolderFolder,
  public IFolderGetItemName,
  public IFolderClone,
  public IFolderWasChanged,
  public IFolderOperations,
  public CMyUnknownImp
{
  Z7_COM_UNKNOWN_IMP_5(IFolderFolder, IFolderGetItemName, IFolderClone, IFolderWasChanged, IFolderOperations)
  Z7_IFACE_COM7_IMP(IFolderFolder)
  Z7_IFACE_COM7_IMP(IFolderGetItemName)
  Z7_IFACE_COM7_IMP(IFolderClone)
  Z7_IFACE_COM7_IMP(IFolderWasChanged)
  Z7_IFACE_COM7_IMP(IFolderOperations)

  FString _path;          // with trailing '/'
  CObjectVector<CItem> _items;
  timespec _dirMTime;
public:
  // path: absolute directory path; a trailing '/' is added if missing.
  HRESULT Init(const FString &path);
  const FString &GetPath() const { return _path; }
  const CItem &GetItem(UInt32 index) const { return _items[index]; }
};

bool FillItem(int dirFd, const char *name, CItem &item);

// upstream NFsFolder::CopyFileSystemItems (FSFolderCopy.cpp): copy or move
// file-system items (full paths) into destDir, asking via callback->AskWrite.
HRESULT CopyFileSystemItems(
    const UStringVector &itemsPaths,
    const FString &destDirPrefix,
    bool moveMode,
    IFolderOperationsExtractCallback *callback);

}

#endif

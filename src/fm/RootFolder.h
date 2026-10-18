// RootFolder.h -- the "Computer" folder above "/" (upstream RootFolder.cpp
// + FSDrives.cpp, which list drives and \\.\PhysicalDriveN on Windows).
//
// Items: "/" , the home directory, every mounted volume, and "Devices"
// (block devices, which open as raw disk images, read-only).

#ifndef Z7QT_ROOT_FOLDER_H
#define Z7QT_ROOT_FOLDER_H

#include "Common/MyCom.h"
#include "Common/MyString.h"
#include "Common/MyVector.h"

#include "7zip/UI/FileManager/IFolder.h"

struct CRootItem
{
  UString Name;       // shown, and used to bind
  FString Path;       // target directory, empty for "Devices"
  UString Type;       // file system type / description
  UInt64 TotalSize;
  UInt64 FreeSpace;
  bool SizeDefined;
};

class CRootFolder Z7_final:
  public IFolderFolder,
  public CMyUnknownImp
{
  Z7_COM_UNKNOWN_IMP_1(IFolderFolder)
  Z7_IFACE_COM7_IMP(IFolderFolder)

  CObjectVector<CRootItem> _items;
public:
  void Init();
};

#define Z7QT_DEVICES_NAME "Devices"
#define Z7QT_DEVICES_PATH "/dev/"

// Lists whole disks and partitions from /sys/block. kpidPath is "/dev/" so
// the panel opens "/dev/<name>" as an archive, the same way 7zFM opens
// \\.\PhysicalDrive0.
class CDevicesFolder Z7_final:
  public IFolderFolder,
  public CMyUnknownImp
{
  Z7_COM_UNKNOWN_IMP_1(IFolderFolder)
  Z7_IFACE_COM7_IMP(IFolderFolder)

  struct CDev
  {
    UString Name;
    UString Model;
    UInt64 Size;
    bool IsPartition;
    bool ReadOnly;
  };
  CObjectVector<CDev> _items;
public:
};

#endif

// RootFolder.cpp

#include "RootFolder.h"

#include "Common/ComTry.h"
#include "Common/StringConvert.h"
#include "Windows/PropVariant.h"

#include "7zip/PropID.h"

#include "FsFolder.h"
#include "QtConv.h"

#include <QDir>
#include <QFile>
#include <QSet>
#include <QStorageInfo>

using namespace NWindows;

static const PROPID kRootProps[] =
{
  kpidName,
  kpidType,
  kpidTotalSize,
  kpidFreeSpace,
  kpidPath
};

static bool IsInterestingMount(const QStorageInfo &si)
{
  if (!si.isValid() || !si.isReady())
    return false;
  const QString root = si.rootPath();
  if (root == QLatin1String("/"))
    return false;
  const QByteArray dev = si.device();
  const QByteArray fs = si.fileSystemType();
  if (dev.startsWith("/dev/"))
    return true;
  // network and FUSE mounts are useful too; skip kernel pseudo file systems
  static const char *const kNet[] = { "nfs", "nfs4", "cifs", "smb3", "sshfs", "fuse.sshfs",
      "fuse.rclone", "fuse.ratarmount", "fuse.archivemount", "9p", "virtiofs", "fuse.gvfsd-fuse" };
  for (const char *n : kNet)
    if (fs == n)
      return true;
  return false;
}

void CRootFolder::Init()
{
  _items.Clear();
  {
    CRootItem &it = _items.AddNew();
    it.Name = L"/";
    it.Path = "/";
    const QStorageInfo si(QStringLiteral("/"));
    it.Type = ToU(QString::fromLatin1(si.fileSystemType()));
    it.TotalSize = (UInt64)si.bytesTotal();
    it.FreeSpace = (UInt64)si.bytesAvailable();
    it.SizeDefined = si.isValid();
  }
  {
    CRootItem &it = _items.AddNew();
    const QString home = QDir::homePath();
    it.Name = ToU(home);
    it.Path = QToFs(home);
    it.Type = L"Home";
    it.SizeDefined = false;
    it.TotalSize = it.FreeSpace = 0;
  }
  QSet<QByteArray> seenDevices;
  seenDevices.insert(QStorageInfo(QStringLiteral("/")).device());
  for (const QStorageInfo &si : QStorageInfo::mountedVolumes())
  {
    if (!IsInterestingMount(si))
      continue;
    // btrfs subvolumes etc. show the same device many times; list it once
    if (seenDevices.contains(si.device()))
      continue;
    seenDevices.insert(si.device());
    CRootItem &it = _items.AddNew();
    it.Name = ToU(si.rootPath());
    it.Path = QToFs(si.rootPath());
    QString type = QString::fromLatin1(si.fileSystemType());
    if (!si.name().isEmpty())
      type = si.name() + QLatin1String(" (") + type + QLatin1Char(')');
    it.Type = ToU(type);
    it.TotalSize = (UInt64)si.bytesTotal();
    it.FreeSpace = (UInt64)si.bytesAvailable();
    it.SizeDefined = true;
  }
  {
    CRootItem &it = _items.AddNew();
    it.Name = L"" Z7QT_DEVICES_NAME;
    it.Type = L"Block devices (raw disks)";
    it.SizeDefined = false;
    it.TotalSize = it.FreeSpace = 0;
  }
}

Z7_COM7F_IMF(CRootFolder::LoadItems())
{
  Init();
  return S_OK;
}

Z7_COM7F_IMF(CRootFolder::GetNumberOfItems(UInt32 *numItems))
{
  *numItems = _items.Size();
  return S_OK;
}

Z7_COM7F_IMF(CRootFolder::GetProperty(UInt32 index, PROPID propID, PROPVARIANT *value))
{
  COM_TRY_BEGIN
  NCOM::CPropVariant prop;
  if (index >= _items.Size())
    return E_INVALIDARG;
  const CRootItem &it = _items[index];
  switch (propID)
  {
    case kpidIsDir: prop = true; break;
    case kpidName: prop = it.Name; break;
    case kpidType: if (!it.Type.IsEmpty()) prop = it.Type; break;
    case kpidPath: if (!it.Path.IsEmpty()) prop = fs2us(it.Path); break;
    case kpidTotalSize: if (it.SizeDefined) prop = it.TotalSize; break;
    case kpidFreeSpace: if (it.SizeDefined) prop = it.FreeSpace; break;
  }
  prop.Detach(value);
  return S_OK;
  COM_TRY_END
}

static HRESULT BindToPath(const FString &path, IFolderFolder **resultFolder)
{
  NFsFolder::CFsFolder *spec = new NFsFolder::CFsFolder;
  CMyComPtr<IFolderFolder> f = spec;
  RINOK(spec->Init(path))
  *resultFolder = f.Detach();
  return S_OK;
}

static HRESULT BindToDevices(IFolderFolder **resultFolder)
{
  CMyComPtr<IFolderFolder> f = new CDevicesFolder;
  RINOK(f->LoadItems())
  *resultFolder = f.Detach();
  return S_OK;
}

Z7_COM7F_IMF(CRootFolder::BindToFolder(UInt32 index, IFolderFolder **resultFolder))
{
  *resultFolder = NULL;
  if (index >= _items.Size())
    return E_INVALIDARG;
  const CRootItem &it = _items[index];
  if (it.Path.IsEmpty())
    return BindToDevices(resultFolder);
  return BindToPath(it.Path, resultFolder);
}

Z7_COM7F_IMF(CRootFolder::BindToFolder(const wchar_t *name, IFolderFolder **resultFolder))
{
  *resultFolder = NULL;
  const UString n = name;
  if (n == L"" Z7QT_DEVICES_NAME || n == L"" Z7QT_DEVICES_NAME "/" || n == L"" Z7QT_DEVICES_PATH)
    return BindToDevices(resultFolder);
  if (!n.IsEmpty() && n[0] == '/')
    return BindToPath(us2fs(n), resultFolder);
  return E_INVALIDARG;
}

Z7_COM7F_IMF(CRootFolder::BindToParentFolder(IFolderFolder **resultFolder))
{
  *resultFolder = NULL;
  return S_OK;
}

Z7_COM7F_IMF(CRootFolder::GetNumberOfProperties(UInt32 *numProperties))
{
  *numProperties = Z7_ARRAY_SIZE(kRootProps);
  return S_OK;
}

Z7_COM7F_IMF(CRootFolder::GetPropertyInfo(UInt32 index, BSTR *name, PROPID *propID, VARTYPE *varType))
{
  if (index >= Z7_ARRAY_SIZE(kRootProps))
    return E_INVALIDARG;
  *propID = kRootProps[index];
  *varType = k7z_PROPID_To_VARTYPE[(unsigned)*propID];
  *name = NULL;
  return S_OK;
}

Z7_COM7F_IMF(CRootFolder::GetFolderProperty(PROPID propID, PROPVARIANT *value))
{
  NCOM::CPropVariant prop;
  switch (propID)
  {
    case kpidType: prop = "RootFolder"; break;
    case kpidPath: prop = ""; break;
  }
  prop.Detach(value);
  return S_OK;
}

// ---------- CDevicesFolder ----------

static const PROPID kDevProps[] =
{
  kpidName,
  kpidSize,
  kpidType,
  kpidReadOnly
};

static QString ReadSysFile(const QString &path)
{
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
    return QString();
  return QString::fromUtf8(f.readAll()).trimmed();
}

Z7_COM7F_IMF(CDevicesFolder::LoadItems())
{
  _items.Clear();
  const QDir sysBlock(QStringLiteral("/sys/block"));
  for (const QString &disk : sysBlock.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
  {
    if (disk.startsWith(QLatin1String("loop")) && ReadSysFile(sysBlock.filePath(disk + QLatin1String("/size"))) == QLatin1String("0"))
      continue;
    if (disk.startsWith(QLatin1String("ram")) || disk.startsWith(QLatin1String("zram")))
      continue;
    const QString base = sysBlock.filePath(disk);
    CDev &d = _items.AddNew();
    d.Name = ToU(disk);
    d.Size = ReadSysFile(base + QLatin1String("/size")).toULongLong() * 512;
    QString model = ReadSysFile(base + QLatin1String("/device/model"));
    if (model.isEmpty())
      model = QStringLiteral("Disk");
    d.Model = ToU(model);
    d.IsPartition = false;
    d.ReadOnly = ReadSysFile(base + QLatin1String("/ro")) == QLatin1String("1");
    const QDir diskDir(base);
    for (const QString &part : diskDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
    {
      if (!QFile::exists(diskDir.filePath(part + QLatin1String("/partition"))))
        continue;
      CDev &p = _items.AddNew();
      p.Name = ToU(part);
      p.Size = ReadSysFile(diskDir.filePath(part + QLatin1String("/size"))).toULongLong() * 512;
      p.Model = ToU(QStringLiteral("Partition"));
      p.IsPartition = true;
      p.ReadOnly = d.ReadOnly;
    }
  }
  return S_OK;
}

Z7_COM7F_IMF(CDevicesFolder::GetNumberOfItems(UInt32 *numItems))
{
  *numItems = _items.Size();
  return S_OK;
}

Z7_COM7F_IMF(CDevicesFolder::GetProperty(UInt32 index, PROPID propID, PROPVARIANT *value))
{
  COM_TRY_BEGIN
  NCOM::CPropVariant prop;
  if (index >= _items.Size())
    return E_INVALIDARG;
  const CDev &d = _items[index];
  switch (propID)
  {
    case kpidIsDir: prop = false; break;
    case kpidName: prop = d.Name; break;
    case kpidSize: prop = d.Size; break;
    case kpidType: prop = d.Model; break;
    case kpidReadOnly: prop = d.ReadOnly; break;
  }
  prop.Detach(value);
  return S_OK;
  COM_TRY_END
}

Z7_COM7F_IMF(CDevicesFolder::BindToFolder(UInt32 /* index */, IFolderFolder **resultFolder))
{
  *resultFolder = NULL;
  return E_NOTIMPL;
}

Z7_COM7F_IMF(CDevicesFolder::BindToFolder(const wchar_t * /* name */, IFolderFolder **resultFolder))
{
  *resultFolder = NULL;
  return E_NOTIMPL;
}

Z7_COM7F_IMF(CDevicesFolder::BindToParentFolder(IFolderFolder **resultFolder))
{
  CRootFolder *rootSpec = new CRootFolder;
  CMyComPtr<IFolderFolder> root = rootSpec;
  rootSpec->Init();
  *resultFolder = root.Detach();
  return S_OK;
}

Z7_COM7F_IMF(CDevicesFolder::GetNumberOfProperties(UInt32 *numProperties))
{
  *numProperties = Z7_ARRAY_SIZE(kDevProps);
  return S_OK;
}

Z7_COM7F_IMF(CDevicesFolder::GetPropertyInfo(UInt32 index, BSTR *name, PROPID *propID, VARTYPE *varType))
{
  if (index >= Z7_ARRAY_SIZE(kDevProps))
    return E_INVALIDARG;
  *propID = kDevProps[index];
  *varType = k7z_PROPID_To_VARTYPE[(unsigned)*propID];
  *name = NULL;
  return S_OK;
}

Z7_COM7F_IMF(CDevicesFolder::GetFolderProperty(PROPID propID, PROPVARIANT *value))
{
  NCOM::CPropVariant prop;
  switch (propID)
  {
    // "FSDrives" like upstream's \\.\ folder: items are paths under kpidPath.
    case kpidType: prop = "FSDrives"; break;
    case kpidPath: prop = Z7QT_DEVICES_PATH; break;
  }
  prop.Detach(value);
  return S_OK;
}

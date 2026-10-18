// FolderModel.cpp

#include "FolderModel.h"

#include "Common/IntToString.h"
#include "Windows/PropVariant.h"
#include "Windows/PropVariantConv.h"

#include "7zip/PropID.h"
#include "7zip/UI/Common/PropIDUtils.h"
#include "7zip/UI/FileManager/PropertyName.h"

#include "IconProvider.h"
#include "ListFormat.h"
#include "QtConv.h"

#include <algorithm>

using namespace NWindows;

FolderModel::FolderModel(QObject *parent): QAbstractTableModel(parent) {}

void FolderModel::Clear()
{
  beginResetModel();
  _folder.Release();
  _sort.SetFolder(NULL);
  _columns.clear();
  _rows.clear();
  endResetModel();
}

void FolderModel::LoadColumns()
{
  _columns.clear();
  UInt32 numProps = 0;
  _folder->GetNumberOfProperties(&numProps);
  for (UInt32 i = 0; i < numProps; i++)
  {
    CMyComBSTR name;
    PROPID propID;
    VARTYPE varType;
    if (_folder->GetPropertyInfo(i, &name, &propID, &varType) != S_OK)
      continue;
    // upstream CPanel::InitColumns() never shows these as columns
    if (propID == kpidIsDir || propID == kpidPrefix)
      continue;
    CColumn c;
    c.ID = propID;
    c.Name = ToQ(GetNameOfProperty(propID, name));
    c.IsRaw = false;
    _columns.append(c);
  }
  CMyComPtr<IArchiveGetRawProps> rawProps;
  _folder.QueryInterface(IID_IArchiveGetRawProps, &rawProps);
  if (rawProps)
  {
    UInt32 numRaw = 0;
    rawProps->GetNumRawProps(&numRaw);
    for (UInt32 i = 0; i < numRaw; i++)
    {
      CMyComBSTR name;
      PROPID propID;
      if (rawProps->GetRawPropInfo(i, &name, &propID) != S_OK)
        continue;
      if (propID == kpidNtSecure || propID == kpidNtReparse)
        continue;
      CColumn c;
      c.ID = propID;
      c.Name = ToQ(GetNameOfProperty(propID, name));
      c.IsRaw = true;
      _columns.append(c);
    }
  }
  // Name always first
  for (int i = 0; i < _columns.size(); i++)
    if (_columns[i].ID == kpidName && i != 0)
    {
      _columns.move(i, 0);
      break;
    }
}

void FolderModel::SetFolder(IFolderFolder *folder)
{
  beginResetModel();
  _folder = folder;
  const PROPID oldSort = _sort.SortID;
  const bool oldAsc = _sort.Ascending;
  _sort.SetFolder(folder);
  _isDevicesFolder = false;
  {
    NCOM::CPropVariant prop;
    if (folder->GetFolderProperty(kpidType, &prop) == S_OK && prop.vt == VT_BSTR)
      _isDevicesFolder = UString(prop.bstrVal) == L"FSDrives";
  }
  LoadColumns();
  if (ColumnOfPropID(oldSort) >= 0)
  {
    _sort.SortID = oldSort;
    _sort.Ascending = oldAsc;
  }
  else
  {
    _sort.SortID = kpidName;
    _sort.Ascending = true;
  }
  _sort.IsRawSortProp = 0;
  _rows.clear();
  UInt32 n = 0;
  _folder->GetNumberOfItems(&n);
  _rows.reserve((int)n);
  for (UInt32 i = 0; i < n; i++)
    _rows.append(i);
  Sort();
  endResetModel();
}

void FolderModel::Reload()
{
  if (!_folder)
    return;
  beginResetModel();
  _rows.clear();
  UInt32 n = 0;
  _folder->GetNumberOfItems(&n);
  _rows.reserve((int)n);
  for (UInt32 i = 0; i < n; i++)
    _rows.append(i);
  Sort();
  endResetModel();
}

void FolderModel::Sort()
{
  for (const CColumn &c : _columns)
    if (c.ID == _sort.SortID)
      _sort.IsRawSortProp = c.IsRaw ? 1 : 0;
  const CSortContext &ctx = _sort;
  std::stable_sort(_rows.begin(), _rows.end(), [&ctx](UInt32 a, UInt32 b) {
    return ctx.Compare(a, b) < 0;
  });
}

int FolderModel::ColumnOfPropID(PROPID id) const
{
  for (int i = 0; i < _columns.size(); i++)
    if (_columns[i].ID == id)
      return i;
  return -1;
}

int FolderModel::SortColumn() const { return ColumnOfPropID(_sort.SortID); }

void FolderModel::SortByPropID(PROPID propID, bool ascending)
{
  emit layoutAboutToBeChanged();
  const QModelIndexList old = persistentIndexList();
  QVector<UInt32> oldReal;
  for (const QModelIndex &i : old)
    oldReal.append(_rows[i.row()]);
  _sort.SortID = propID;
  _sort.Ascending = ascending;
  Sort();
  QModelIndexList now;
  for (int k = 0; k < old.size(); k++)
    now.append(index(RowOfRealIndex(oldReal[k]), old[k].column()));
  changePersistentIndexList(old, now);
  emit layoutChanged();
}

void FolderModel::SortByColumn(int column)
{
  if (column < 0 || column >= _columns.size())
    return;
  const PROPID id = _columns[column].ID;
  if (id == _sort.SortID)
    SortByPropID(id, !_sort.Ascending);
  else
    SortByPropID(id, CSortContext::DefaultAscending(id));
}

int FolderModel::RowOfRealIndex(UInt32 realIndex) const
{
  for (int i = 0; i < _rows.size(); i++)
    if (_rows[i] == realIndex)
      return i;
  return -1;
}

int FolderModel::RowOfName(const UString &name) const
{
  for (int i = 0; i < _rows.size(); i++)
    if (_sort.GetItemName(_rows[i]) == name)
      return i;
  return -1;
}

UInt64 FolderModel::ItemSize(int row) const
{
  NCOM::CPropVariant prop;
  if (_folder->GetProperty(_rows[row], kpidSize, &prop) != S_OK)
    return 0;
  UInt64 v = 0;
  ConvertPropVariantToUInt64(prop, v);
  return v;
}

int FolderModel::rowCount(const QModelIndex &parent) const
{
  return parent.isValid() ? 0 : _rows.size();
}

int FolderModel::columnCount(const QModelIndex &parent) const
{
  return parent.isValid() ? 0 : _columns.size();
}

Qt::ItemFlags FolderModel::flags(const QModelIndex &index) const
{
  if (!index.isValid())
    return Qt::NoItemFlags;
  Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemNeverHasChildren | Qt::ItemIsDragEnabled;
  if (_editable && index.column() == 0)
    f |= Qt::ItemIsEditable;
  return f;
}

bool FolderModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
  if (role != Qt::EditRole || !index.isValid() || index.column() != 0)
    return false;
  const QString name = value.toString();
  if (name.isEmpty() || name == ToQ(_sort.GetItemName(_rows[index.row()])))
    return false;
  emit RenameRequested(_rows[index.row()], name);
  return false;  // the panel reloads the folder after the rename
}

QVariant FolderModel::data(const QModelIndex &index, int role) const
{
  if (!index.isValid() || !_folder)
    return QVariant();
  const UInt32 real = _rows[index.row()];
  const CColumn &col = _columns[index.column()];
  switch (role)
  {
    case Qt::EditRole:
      if (col.ID == kpidName)
        return ToQ(_sort.GetItemName(real));
      return QVariant();
    case Qt::DisplayRole:
    case Qt::ToolTipRole:
    {
      if (col.IsRaw)
      {
        const void *data;
        UInt32 size, type;
        if (_sort.FolderRawProps && _sort.FolderRawProps->GetRawProp(real, col.ID, &data, &size, &type) == S_OK
            && size != 0 && size <= 64)
        {
          char temp[64 * 2 + 2];
          ConvertDataToHex_Upper(temp, (const Byte *)data, size);
          return QString::fromLatin1(temp);
        }
        return QVariant();
      }
      if (role == Qt::ToolTipRole && col.ID != kpidName)
        return QVariant();
      return ToQ(GetItemText(_folder, _sort.FolderGetItemName, real, col.ID, kTimestampPrintLevel_MIN));
    }
    case Qt::DecorationRole:
      if (index.column() != 0)
        return QVariant();
      if (_sort.IsItem_Folder(real))
        return GetIconProvider().Folder();
      if (_isDevicesFolder)
        return GetIconProvider().ForDevice();
      return GetIconProvider().ForFile(ToQ(_sort.GetItemName(real)));
    case Qt::TextAlignmentRole:
      if (IsSizeProp(col.ID) || col.ID == kpidCRC)
        return QVariant(Qt::AlignRight | Qt::AlignVCenter);
      return QVariant(Qt::AlignLeft | Qt::AlignVCenter);
    case kIsFolderRole:
      return _sort.IsItem_Folder(real);
    case kRealIndexRole:
      return (uint)real;
  }
  return QVariant();
}

QVariant FolderModel::headerData(int section, Qt::Orientation orientation, int role) const
{
  if (orientation != Qt::Horizontal || section < 0 || section >= _columns.size())
    return QVariant();
  if (role == Qt::DisplayRole)
    return _columns[section].Name;
  if (role == Qt::TextAlignmentRole)
    return IsSizeProp(_columns[section].ID) ? QVariant(Qt::AlignRight | Qt::AlignVCenter) : QVariant(Qt::AlignLeft | Qt::AlignVCenter);
  return QVariant();
}

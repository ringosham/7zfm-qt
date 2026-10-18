// ItemSort.cpp -- port of upstream FileManager/PanelSort.cpp (CompareItems,
// CompareItems2, SortItemsWithPropID). Windows reparse-point names are not
// compared specially.

#include "ItemSort.h"

#include "Common/Defs.h"
#include "Windows/PropVariant.h"

#include "7zip/PropID.h"

using namespace NWindows;

void CSortContext::SetFolder(IFolderFolder *folder)
{
  Folder = folder;
  FolderCompare.Release();
  FolderRawProps.Release();
  FolderGetItemName.Release();
  if (!folder)
    return;
  folder->QueryInterface(IID_IFolderCompare, (void **)&FolderCompare);
  folder->QueryInterface(IID_IArchiveGetRawProps, (void **)&FolderRawProps);
  folder->QueryInterface(IID_IFolderGetItemName, (void **)&FolderGetItemName);
}

UString CSortContext::GetItemName(UInt32 index) const
{
  if (FolderGetItemName)
  {
    const wchar_t *name = NULL;
    unsigned len = 0;
    if (FolderGetItemName->GetItemName(index, &name, &len) == S_OK && name)
      return UString(name);
  }
  NCOM::CPropVariant prop;
  if (Folder->GetProperty(index, kpidName, &prop) != S_OK)
    return UString();
  if (prop.vt == VT_BSTR)
    return UString(prop.bstrVal);
  return UString();
}

bool CSortContext::IsItem_Folder(UInt32 index) const
{
  NCOM::CPropVariant prop;
  if (Folder->GetProperty(index, kpidIsDir, &prop) != S_OK)
    return false;
  if (prop.vt == VT_BOOL)
    return prop.boolVal != VARIANT_FALSE;
  return false;
}

bool CSortContext::DefaultAscending(PROPID propID)
{
  switch (propID)
  {
    case kpidSize:
    case kpidPackSize:
    case kpidCTime:
    case kpidATime:
    case kpidMTime:
      return false;
  }
  return true;
}

static inline const wchar_t *GetExtensionPtr(const UString &name)
{
  const int dotPos = name.ReverseFind_Dot();
  return name.Ptr(dotPos < 0 ? name.Len() : (unsigned)dotPos);
}

static int CompareItems2(UInt32 i1, UInt32 i2, const CSortContext &c, PROPID propID, Int32 isRawProp)
{
  if (propID == kpidNoProperty)
    return MyCompare(i1, i2);

  if (isRawProp && c.FolderRawProps)
  {
    const void *data1;
    const void *data2;
    UInt32 dataSize1, dataSize2;
    UInt32 propType1, propType2;
    if (c.FolderRawProps->GetRawProp(i1, propID, &data1, &dataSize1, &propType1) != 0) return 0;
    if (c.FolderRawProps->GetRawProp(i2, propID, &data2, &dataSize2, &propType2) != 0) return 0;
    if (dataSize1 == 0)
      return (dataSize2 == 0) ? 0 : -1;
    if (dataSize2 == 0)
      return 1;
    if (propType1 != NPropDataType::kRaw) return 0;
    if (propType2 != NPropDataType::kRaw) return 0;
  }

  if (c.FolderCompare)
    return c.FolderCompare->CompareItems(i1, i2, propID, isRawProp);

  switch (propID)
  {
    case kpidName:
      return CompareFileNames_ForFolderList(c.GetItemName(i1), c.GetItemName(i2));
    case kpidExtension:
    {
      const UString name1 = c.GetItemName(i1);
      const UString name2 = c.GetItemName(i2);
      return CompareFileNames_ForFolderList(GetExtensionPtr(name1), GetExtensionPtr(name2));
    }
  }

  NCOM::CPropVariant prop1, prop2;
  c.Folder->GetProperty(i1, propID, &prop1);
  c.Folder->GetProperty(i2, propID, &prop2);
  if (prop1.vt != prop2.vt)
    return MyCompare(prop1.vt, prop2.vt);
  if (prop1.vt == VT_BSTR)
    return MyStringCompareNoCase(prop1.bstrVal, prop2.bstrVal);
  return prop1.Compare(prop2);
}

int CSortContext::Compare(UInt32 i1, UInt32 i2) const
{
  const bool isDir1 = IsItem_Folder(i1);
  const bool isDir2 = IsItem_Folder(i2);
  if (isDir1 != isDir2)
    return isDir1 ? -1 : 1;

  PROPID propID = SortID;
  int res = 0;
  for (unsigned iter = 0; iter < 3; iter++)
  {
    res = CompareItems2(i1, i2, *this, propID, iter ? 0 : IsRawSortProp);
    if (res)
      break;
    if (propID == kpidName)
    {
      propID = kpidPrefix;
      continue;
    }
    if (iter)
      break;
    propID = kpidName;
  }
  if (res == 0)
    res = MyCompare(i1, i2);
  return Ascending ? res : -res;
}

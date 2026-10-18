// ItemSort.h -- 7zFM's list ordering (upstream FileManager/PanelSort.cpp).

#ifndef Z7QT_ITEM_SORT_H
#define Z7QT_ITEM_SORT_H

#include "Common/MyCom.h"
#include "Common/MyString.h"
#include "7zip/Archive/IArchive.h"
#include "7zip/UI/FileManager/IFolder.h"

int CompareFileNames_ForFolderList(const wchar_t *s1, const wchar_t *s2);

// The CPanel members that upstream's CompareItems() reads.
struct CSortContext
{
  CMyComPtr<IFolderFolder> Folder;
  CMyComPtr<IFolderCompare> FolderCompare;
  CMyComPtr<IArchiveGetRawProps> FolderRawProps;
  CMyComPtr<IFolderGetItemName> FolderGetItemName;
  PROPID SortID;
  Int32 IsRawSortProp;
  bool Ascending;

  CSortContext(): SortID(kpidName), IsRawSortProp(0), Ascending(true) {}
  void SetFolder(IFolderFolder *folder);

  UString GetItemName(UInt32 index) const;
  bool IsItem_Folder(UInt32 index) const;

  // Upstream's default direction when a column is first clicked.
  static bool DefaultAscending(PROPID propID);

  int Compare(UInt32 i1, UInt32 i2) const;   // CompareItems()
};

#endif

// ListFormat.h

#ifndef Z7QT_LIST_FORMAT_H
#define Z7QT_LIST_FORMAT_H

#include "Common/MyString.h"
#include "7zip/UI/FileManager/IFolder.h"

UString ConvertSizeToString(UInt64 value);
bool IsSizeProp(UINT propID) throw();

// Text shown in the file list for (realIndex, propID), as 7zFM shows it.
UString GetItemText(IFolderFolder *folder, IFolderGetItemName *folderGetItemName,
    UInt32 realIndex, PROPID propID, int timestampLevel);

#endif

// PanelProperties.h

#ifndef Z7QT_PANEL_PROPERTIES_H
#define Z7QT_PANEL_PROPERTIES_H

#include "Common/MyString.h"
#include "Common/MyVector.h"
#include "7zip/UI/FileManager/IFolder.h"

// Name/value rows; a row with an empty value whose name starts with '-' is
// a separator (upstream draws them the same way).
struct CPropList
{
  UStringVector Strings;
  UStringVector Values;
};

void GetFolderProperties(IFolderFolder *folder, const CRecordVector<UInt32> &indices, CPropList &list);

#endif

// FolderOps.h -- folder operations with progress, from upstream
// FileManager/PanelOperations.cpp (CThreadFolderOperations), PanelCopy.cpp
// (CThreadUpdate) and PanelItemOpen.cpp (CThreadCopyFrom). Compiled with
// the Q32 layer; the API here is Win32-free for the Qt panel.

#ifndef Z7QT_FOLDER_OPS_H
#define Z7QT_FOLDER_OPS_H

#include "Common/MyCom.h"
#include "Common/MyString.h"
#include "7zip/UI/FileManager/IFolder.h"

struct CFolderOpPassword
{
  bool UsePassword = false;
  UString Password;
};

HRESULT FolderOp_CreateFolder(IFolderOperations *ops, const UString &name, CFolderOpPassword &pw);
HRESULT FolderOp_Delete(IFolderOperations *ops, const CRecordVector<UInt32> &indices, CFolderOpPassword &pw);
HRESULT FolderOp_Rename(IFolderOperations *ops, UInt32 index, const UString &newName, CFolderOpPassword &pw);

// copy files from the file system into the folder (archive) -- upstream CPanel::CopyFrom
HRESULT FolderOp_CopyFrom(IFolderOperations *ops, bool moveMode, const UString &folderPrefix,
    const UStringVector &filePaths, CFolderOpPassword &pw, UStringVector *messages);

// replace item (index) with the file (fullFilePath) -- upstream OnOpenItemChanged
HRESULT FolderOp_CopyFromFile(IFolderOperations *ops, UInt32 index, const UString &fullFilePath,
    const UString &itemName, CFolderOpPassword &pw);

// upstream CComboDialog; returns false on Cancel
bool RunComboDialog(const UString &title, const UString &staticText, UString &value,
    const UStringVector *history = NULL);

#endif

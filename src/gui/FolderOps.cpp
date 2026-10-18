// FolderOps.cpp -- see FolderOps.h. The thread classes are copied from
// upstream; only the panel parameters became function arguments.

#include "Windows/Thread.h"

#include "7zip/UI/FileManager/ComboDialog.h"
#include "7zip/UI/FileManager/LangUtils.h"
#include "7zip/UI/FileManager/UpdateCallback100.h"
#include "7zip/UI/FileManager/resource.h"

#include "FolderOps.h"

using namespace NWindows;

enum EFolderOpType
{
  FOLDER_TYPE_CREATE_FOLDER = 0,
  FOLDER_TYPE_DELETE = 1,
  FOLDER_TYPE_RENAME = 2
};

class CThreadFolderOperations: public CProgressThreadVirt
{
  HRESULT ProcessVirt() Z7_override;
public:
  EFolderOpType OpType;
  UString Name;
  UInt32 Index;
  CRecordVector<UInt32> Indices;

  CMyComPtr<IFolderOperations> FolderOperations;
  CMyComPtr<IProgress> UpdateCallback;
  CUpdateCallback100Imp *UpdateCallbackSpec;

  CThreadFolderOperations(EFolderOpType opType): OpType(opType) {}
  HRESULT DoOperation(CFolderOpPassword &pw, const UString &progressTitle, const UString &titleError);
};

HRESULT CThreadFolderOperations::ProcessVirt()
{
  switch ((int)OpType)
  {
    case FOLDER_TYPE_CREATE_FOLDER:
      return FolderOperations->CreateFolder(Name, UpdateCallback);
    case FOLDER_TYPE_DELETE:
      return FolderOperations->Delete(Indices.ConstData(), Indices.Size(), UpdateCallback);
    case FOLDER_TYPE_RENAME:
      return FolderOperations->Rename(Index, Name, UpdateCallback);
    default:
      return E_FAIL;
  }
}

HRESULT CThreadFolderOperations::DoOperation(CFolderOpPassword &pw, const UString &progressTitle, const UString &titleError)
{
  UpdateCallbackSpec = new CUpdateCallback100Imp;
  UpdateCallback = UpdateCallbackSpec;
  UpdateCallbackSpec->ProgressDialog = this;

  WaitMode = true;
  Sync.FinalMessage.ErrorMessage.Title = titleError;

  UpdateCallbackSpec->Init();
  UpdateCallbackSpec->PasswordIsDefined = pw.UsePassword;
  UpdateCallbackSpec->Password = pw.Password;

  MainTitle = "7-Zip";
  MainAddTitle = progressTitle + L' ';

  RINOK(Create(progressTitle, MainWindow))
  if (UpdateCallbackSpec->PasswordIsDefined)
  {
    pw.UsePassword = true;
    pw.Password = UpdateCallbackSpec->Password;
  }
  return Result;
}

HRESULT FolderOp_CreateFolder(IFolderOperations *ops, const UString &name, CFolderOpPassword &pw)
{
  CThreadFolderOperations op(FOLDER_TYPE_CREATE_FOLDER);
  op.FolderOperations = ops;
  op.Name = name;
  return op.DoOperation(pw, LangString(IDS_CREATE_FOLDER), LangString(IDS_CREATE_FOLDER_ERROR));
}

HRESULT FolderOp_Delete(IFolderOperations *ops, const CRecordVector<UInt32> &indices, CFolderOpPassword &pw)
{
  CThreadFolderOperations op(FOLDER_TYPE_DELETE);
  op.FolderOperations = ops;
  op.Indices = indices;
  return op.DoOperation(pw, LangString(IDS_DELETING), LangString(IDS_ERROR_DELETING));
}

HRESULT FolderOp_Rename(IFolderOperations *ops, UInt32 index, const UString &newName, CFolderOpPassword &pw)
{
  CThreadFolderOperations op(FOLDER_TYPE_RENAME);
  op.FolderOperations = ops;
  op.Index = index;
  op.Name = newName;
  return op.DoOperation(pw, LangString(IDS_RENAMING), LangString(IDS_ERROR_RENAMING));
}

// ---- upstream PanelCopy.cpp CThreadUpdate ----

class CThreadUpdate: public CProgressThreadVirt
{
  HRESULT ProcessVirt() Z7_override
  {
    return FolderOperations->CopyFrom(
        MoveMode,
        FolderPrefix,
        FileNamePointers.ConstData(),
        FileNamePointers.Size(),
        UpdateCallback);
  }
public:
  CMyComPtr<IFolderOperations> FolderOperations;
  UString FolderPrefix;
  UStringVector FileNames;
  CRecordVector<const wchar_t *> FileNamePointers;
  CMyComPtr<IFolderArchiveUpdateCallback> UpdateCallback;
  CUpdateCallback100Imp *UpdateCallbackSpec;
  bool MoveMode;
};

HRESULT FolderOp_CopyFrom(IFolderOperations *ops, bool moveMode, const UString &folderPrefix,
    const UStringVector &filePaths, CFolderOpPassword &pw, UStringVector *messages)
{
  CThreadUpdate updater;
  updater.MoveMode = moveMode;
  updater.UpdateCallbackSpec = new CUpdateCallback100Imp;
  updater.UpdateCallback = updater.UpdateCallbackSpec;
  updater.UpdateCallbackSpec->Init();
  updater.UpdateCallbackSpec->ProgressDialog = &updater;

  const UString title = LangString(IDS_COPYING);
  updater.MainTitle = "7-Zip";
  updater.MainAddTitle = title + L' ';

  updater.UpdateCallbackSpec->PasswordIsDefined = pw.UsePassword;
  updater.UpdateCallbackSpec->Password = pw.Password;

  updater.FolderOperations = ops;
  updater.FolderPrefix = folderPrefix;
  updater.FileNames.ClearAndReserve(filePaths.Size());
  unsigned i;
  for (i = 0; i < filePaths.Size(); i++)
    updater.FileNames.AddInReserved(filePaths[i]);
  updater.FileNamePointers.ClearAndReserve(updater.FileNames.Size());
  for (i = 0; i < updater.FileNames.Size(); i++)
    updater.FileNamePointers.AddInReserved(updater.FileNames[i]);

  RINOK(updater.Create(title, NULL))

  if (messages)
    *messages = updater.Sync.Messages;
  if (updater.UpdateCallbackSpec->PasswordIsDefined)
  {
    pw.UsePassword = true;
    pw.Password = updater.UpdateCallbackSpec->Password;
  }
  return updater.Result;
}

// ---- upstream PanelItemOpen.cpp CThreadCopyFrom ----

class CThreadCopyFrom: public CProgressThreadVirt
{
  HRESULT ProcessVirt() Z7_override
  {
    return FolderOperations->CopyFromFile(ItemIndex, FullPath, UpdateCallback);
  }
public:
  UString FullPath;
  UInt32 ItemIndex;

  CMyComPtr<IFolderOperations> FolderOperations;
  CMyComPtr<IProgress> UpdateCallback;
  CUpdateCallback100Imp *UpdateCallbackSpec;
};

HRESULT FolderOp_CopyFromFile(IFolderOperations *ops, UInt32 index, const UString &fullFilePath,
    const UString &itemName, CFolderOpPassword &pw)
{
  CThreadCopyFrom t;
  t.UpdateCallbackSpec = new CUpdateCallback100Imp;
  t.UpdateCallback = t.UpdateCallbackSpec;
  t.UpdateCallbackSpec->ProgressDialog = &t;
  t.ItemIndex = index;
  t.FullPath = fullFilePath;
  t.FolderOperations = ops;

  t.UpdateCallbackSpec->Init();
  t.UpdateCallbackSpec->PasswordIsDefined = pw.UsePassword;
  t.UpdateCallbackSpec->Password = pw.Password;

  RINOK(t.Create(itemName, NULL))
  return t.Result;
}

bool RunComboDialog(const UString &title, const UString &staticText, UString &value,
    const UStringVector *history)
{
  CComboDialog dlg;
  dlg.Title = title;
  dlg.Static = staticText;
  dlg.Value = value;
  if (history)
    dlg.Strings = *history;
  if (dlg.Create(NULL) != IDOK)
    return false;
  value = dlg.Value;
  return true;
}

// upstream FileManager/BrowseDialog.cpp
bool Dlg_CreateFolder(HWND wnd, UString &destName);
bool Dlg_CreateFolder(HWND wnd, UString &destName)
{
  destName.Empty();
  CComboDialog dlg;
  LangString(IDS_CREATE_FOLDER, dlg.Title);
  LangString(IDS_CREATE_FOLDER_NAME, dlg.Static);
  LangString(IDS_CREATE_FOLDER_DEFAULT_NAME, dlg.Value);
  if (dlg.Create(wnd) != IDOK)
    return false;
  destName = dlg.Value;
  return true;
}

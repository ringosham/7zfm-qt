// PanelOperations.cpp -- upstream FileManager/PanelOperations.cpp (delete,
// rename, create folder/file, comment), drag-in (upstream PanelDrag.cpp
// drop -> CopyFrom) and write-back of externally edited files (upstream
// CPanel::OnOpenItemChanged / CTmpProcessInfo).

#include "Panel.h"

#include "Common/StringConvert.h"
#include "Windows/FileName.h"
#include "Windows/PropVariant.h"

#include "7zip/PropID.h"
#include "7zip/UI/FileManager/FormatUtils.h"
#include "7zip/UI/FileManager/LangUtils.h"
#include "7zip/UI/FileManager/resource.h"

#include "FolderModel.h"
#include "Messages.h"
#include "QtConv.h"
#include "SplitPanel.h"
#include "LinkDialog.h"
#include "CompressCall.h"

#include <QAbstractItemView>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QItemSelectionModel>
#include <QMessageBox>
#include <QTimer>
#include <QTreeView>

using namespace NWindows;

CFolderOpPassword Panel::CurrentPassword() const
{
  CFolderOpPassword pw;
  if (!_parentFolders.isEmpty())
  {
    pw.UsePassword = _parentFolders.back().UsePassword;
    pw.Password = _parentFolders.back().Password;
  }
  return pw;
}

void Panel::StorePassword(const CFolderOpPassword &pw)
{
  if (!_parentFolders.isEmpty() && pw.UsePassword)
  {
    _parentFolders.back().UsePassword = true;
    _parentFolders.back().Password = pw.Password;
  }
}

static bool IsCorrectFsName(const UString &name)
{
  const UString lastPart = name.Ptr((unsigned)(name.ReverseFind_PathSepar() + 1));
  return
      !lastPart.IsEqualTo(".") &&
      !lastPart.IsEqualTo("..");
}

void Panel::DeleteItems(bool toTrash)
{
  CRecordVector<UInt32> indices;
  Get_ItemIndices_Operated(indices);
  if (indices.IsEmpty())
    return;
  CSelectedState state;
  SaveSelectedState(state);

  if (IsFSFolder() && toTrash)
  {
    // upstream uses SHFileOperation(FO_DELETE, FOF_ALLOWUNDO): the recycle bin
    QStringList failed;
    FOR_VECTOR (i, indices)
    {
      const QString path = ToQ(GetFsPath() + GetItemRelPath(indices[i]));
      if (!QFile::moveToTrash(path))
        failed << path;
    }
    if (failed.isEmpty())
    {
      RefreshListCtrl(state);
      return;
    }
    if (QMessageBox::question(this, QStringLiteral("7-Zip"),
        tr("These items cannot be moved to the trash:\n%1\n\nDelete them permanently?")
            .arg(failed.mid(0, 10).join(QLatin1Char('\n'))),
        QMessageBox::Yes | QMessageBox::Cancel) != QMessageBox::Yes)
    {
      RefreshListCtrl(state);
      return;
    }
    // fall through: permanent delete of what is left
    RefreshListCtrl(state);
    Get_ItemIndices_Operated(indices);
  }
  else
  {
    if (!_folderOperations)
    {
      MessageBox_Error(LangString(IDS_OPERATION_IS_NOT_SUPPORTED));
      return;
    }
    UInt32 titleID, messageID;
    UString messageParam;
    if (indices.Size() == 1)
    {
      const unsigned index = indices[0];
      messageParam = GetItemRelPath(index);
      if (IsItem_Folder(index))
      {
        titleID = IDS_CONFIRM_FOLDER_DELETE;
        messageID = IDS_WANT_TO_DELETE_FOLDER;
      }
      else
      {
        titleID = IDS_CONFIRM_FILE_DELETE;
        messageID = IDS_WANT_TO_DELETE_FILE;
      }
    }
    else
    {
      titleID = IDS_CONFIRM_ITEMS_DELETE;
      messageID = IDS_WANT_TO_DELETE_ITEMS;
      messageParam = NumberToString(indices.Size());
    }
    if (QMessageBox::question(this, ToQ(LangString(titleID)), ToQ(MyFormatNew(messageID, messageParam)),
        QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel) != QMessageBox::Yes)
      return;
  }

  if (!_folderOperations || indices.IsEmpty())
    return;
  CFolderOpPassword pw = CurrentPassword();
  FolderOp_Delete(_folderOperations, indices, pw);
  StorePassword(pw);
  RefreshListCtrl(state);
}

void Panel::RenameFile()
{
  if (!_folderOperations)
  {
    MessageBox_Error(LangString(IDS_OPERATION_IS_NOT_SUPPORTED));
    return;
  }
  QAbstractItemView *v = CurrentView();
  const QModelIndex cur = _selection->currentIndex();
  if (!cur.isValid())
    return;
  v->edit(_model->index(cur.row(), 0));
}

void Panel::RenameFocused(const QString &newName)
{
  const int focused = FocusedRealIndex();
  if (focused >= 0 && _folderOperations)
    OnRenameRequested((UInt32)focused, newName);
}

void Panel::OnRenameRequested(UInt32 realIndex, const QString &newNameQ)
{
  UString newName = ToU(newNameQ);
  if (!IsCorrectFsName(newName) || newName.Find(L'/') >= 0)
  {
    MessageBox_Error_HRESULT(E_INVALIDARG);
    return;
  }
  CSelectedState state;
  SaveSelectedState(state);
  const UString prefix;
  CFolderOpPassword pw = CurrentPassword();
  const HRESULT res = FolderOp_Rename(_folderOperations, realIndex, newName, pw);
  StorePassword(pw);
  state.FocusedName = (res == S_OK) ? newName : GetItemName(realIndex);
  state.FocusedName_Defined = true;
  state.SelectFocused = true;
  state.SelectedNames.Clear();
  // reload after the editor has closed
  QTimer::singleShot(0, this, [this, state]() { RefreshListCtrl(state); });
}

void Panel::CreateFolder()
{
  if (!_folderOperations)
  {
    MessageBox_Error(LangString(IDS_OPERATION_IS_NOT_SUPPORTED));
    return;
  }
  CSelectedState state;
  SaveSelectedState(state);

  UString newName;
  LangString(IDS_CREATE_FOLDER_DEFAULT_NAME, newName);
  if (!RunComboDialog(LangString(IDS_CREATE_FOLDER), LangString(IDS_CREATE_FOLDER_NAME), newName))
    return;
  newName.Trim();
  if (newName.IsEmpty() || !IsCorrectFsName(newName))
  {
    MessageBox_Error_HRESULT(E_INVALIDARG);
    return;
  }

  CFolderOpPassword pw = CurrentPassword();
  const HRESULT res = FolderOp_CreateFolder(_folderOperations, newName, pw);
  StorePassword(pw);
  if (res == S_OK)
  {
    const int pos = newName.Find(WCHAR_PATH_SEPARATOR);
    if (pos >= 0)
      newName.DeleteFrom((unsigned)pos);
    state.SelectedNames.Clear();
    state.FocusedName = newName;
    state.FocusedName_Defined = true;
    state.SelectFocused = true;
  }
  RefreshListCtrl(state);
}

void Panel::CreateFile()
{
  if (!_folderOperations)
  {
    MessageBox_Error(LangString(IDS_OPERATION_IS_NOT_SUPPORTED));
    return;
  }
  CSelectedState state;
  SaveSelectedState(state);
  UString newName;
  LangString(IDS_CREATE_FILE_DEFAULT_NAME, newName);
  if (!RunComboDialog(LangString(IDS_CREATE_FILE), LangString(IDS_CREATE_FILE_NAME), newName))
    return;
  newName.Trim();
  if (newName.IsEmpty() || !IsCorrectFsName(newName))
  {
    MessageBox_Error_HRESULT(E_INVALIDARG);
    return;
  }
  const HRESULT result = _folderOperations->CreateFile(newName, NULL);
  if (result != S_OK)
  {
    MessageBox_Error(LangString(IDS_CREATE_FILE_ERROR) + L"\n" + HResultToMessage(result));
    return;
  }
  const int pos = newName.Find(WCHAR_PATH_SEPARATOR);
  if (pos >= 0)
    newName.DeleteFrom((unsigned)pos);
  state.SelectedNames.Clear();
  state.FocusedName = newName;
  state.FocusedName_Defined = true;
  state.SelectFocused = true;
  RefreshListCtrl(state);
}

void Panel::ChangeComment()
{
  const int focused = FocusedRealIndex();
  if (focused < 0)
    return;
  if (!_folderOperations)
  {
    MessageBox_Error(LangString(IDS_OPERATION_IS_NOT_SUPPORTED));
    return;
  }
  const UInt32 realIndex = (UInt32)focused;
  CSelectedState state;
  SaveSelectedState(state);
  UString comment;
  {
    NCOM::CPropVariant propVariant;
    if (_folder->GetProperty(realIndex, kpidComment, &propVariant) != S_OK)
      return;
    if (propVariant.vt == VT_BSTR)
      comment = propVariant.bstrVal;
    else if (propVariant.vt != VT_EMPTY)
      return;
  }
  const UString name = GetItemRelPath(realIndex);
  UString title = name;
  title += " : ";
  AddLangString(title, IDS_COMMENT);
  if (!RunComboDialog(title, LangString(IDS_COMMENT2), comment))
    return;
  NCOM::CPropVariant propVariant (comment);
  const HRESULT result = _folderOperations->SetProperty(realIndex, kpidComment, &propVariant, NULL);
  if (result != S_OK)
  {
    if (result == E_NOINTERFACE || result == E_NOTIMPL)
      MessageBox_Error(LangString(IDS_OPERATION_IS_NOT_SUPPORTED));
    else
      MessageBox_Error(UString(L"Set Comment Error\n") + HResultToMessage(result));
  }
  RefreshListCtrl(state);
}

// ---------- drop (upstream PanelDrag.cpp: CompressDropFiles / CopyFrom) ----------

void Panel::DropFiles(const QStringList &paths, bool moveMode)
{
  if (paths.isEmpty())
    return;
  CSelectedState state;
  SaveSelectedState(state);
  if (!_parentFolders.isEmpty())
  {
    if (!_folderOperations)
    {
      MessageBox_Error(LangString(IDS_OPERATION_IS_NOT_SUPPORTED));
      return;
    }
    // CopyFrom() takes names relative to one source folder: group by folder
    QMap<QString, QStringList> byDir;
    for (const QString &p : paths)
    {
      const QFileInfo fi(p);
      byDir[fi.absolutePath()] << fi.fileName();
    }
    CFolderOpPassword pw = CurrentPassword();
    for (auto it = byDir.constBegin(); it != byDir.constEnd(); ++it)
    {
      UString prefix = ToU(it.key());
      NFile::NName::NormalizeDirPathPrefix(prefix);
      UStringVector names;
      for (const QString &n : it.value())
        names.Add(ToU(n));
      UStringVector messages;
      const HRESULT res = FolderOp_CopyFrom(_folderOperations, moveMode, prefix, names, pw, &messages);
      if (res != S_OK)
      {
        if (res != E_ABORT && messages.IsEmpty())
          MessageBox_Error_HRESULT(res);
        break;
      }
    }
    StorePassword(pw);
    RefreshListCtrl(state);
    return;
  }
  if (!IsFSFolder())
  {
    MessageBox_Error(LangString(IDS_OPERATION_IS_NOT_SUPPORTED));
    return;
  }
  UStringVector filePaths;
  for (const QString &p : paths)
    filePaths.Add(ToU(p));
  CCopyToOptions options;
  options.folder = GetFsPath();
  options.moveMode = moveMode;
  options.showErrorMessages = true;
  bool usePassword = false;
  UString password;
  CRecordVector<UInt32> none;
  CopyTo(options, none, NULL, usePassword, password, &filePaths);
  RefreshListCtrl(state);
}

// ---------- split / combine (upstream PanelSplitFile.cpp) ----------

class CPanelSplitAdapter : public ISplitPanel
{
  Panel &_p;
public:
  explicit CPanelSplitAdapter(Panel &p): _p(p) {}
  bool Is_IO_FS_Folder() const override { return _p.IsFSFolder(); }
  bool IsFSFolder() const override { return _p.IsFSFolder(); }
  void Get_ItemIndices_Operated(CRecordVector<UInt32> &indices) const override { _p.Get_ItemIndices_Operated(indices); }
  bool IsItem_Folder(unsigned index) const override { return _p.IsItem_Folder(index); }
  UString GetItemName(unsigned index) const override { return _p.GetItemName(index); }
  UString GetItemRelPath(unsigned index) const override { return _p.GetItemRelPath(index); }
  UString GetFsPath() const override { return _p.GetFsPath(); }
  void MessageBox_Error(const UString &message) override { _p.MessageBox_Error(message); }
};

// upstream CApp::CalculateCrc / CalculateCrc2
void Panel::CalculateCrc(const char *methodName)
{
  CRecordVector<UInt32> indices;
  Get_ItemIndices_Operated(indices);
  if (indices.IsEmpty())
    return;
  HRESULT res;
  if (!IsFSFolder())
  {
    CCopyToOptions options;
    options.streamMode = true;
    options.showErrorMessages = true;
    options.hashMethods.Add(UString(methodName));
    UStringVector messages;
    CFolderOpPassword pw = CurrentPassword();
    res = CopyTo(options, indices, &messages, pw.UsePassword, pw.Password);
    StorePassword(pw);
  }
  else
  {
    UStringVector relPaths;
    FOR_VECTOR (i, indices)
      relPaths.Add(GetItemRelPath(indices[i]));
    res = CalculateCrcFs(UString(methodName), GetFsPath(), relPaths);
  }
  if (res != S_OK && res != E_ABORT)
    MessageBox_Error_HRESULT(res);
}

void Panel::Link()
{
  if (!IsFSFolder())
  {
    MessageBox_Error(LangString(IDS_OPERATION_IS_NOT_SUPPORTED));
    return;
  }
  const int focused = FocusedRealIndex();
  if (focused < 0)
    return;
  CSelectedState state;
  SaveSelectedState(state);
  if (RunLinkDialog(this, ToQ(GetFsPath() + GetItemRelPath((unsigned)focused)), ToQ(GetFsPath())))
    RefreshListCtrl(state);
}

void Panel::Split()
{
  CPanelSplitAdapter a(*this);
  SplitFile(a);
  RefreshListCtrl_SaveFocused();
}

void Panel::Combine()
{
  CPanelSplitAdapter a(*this);
  CombineFiles(a);
  RefreshListCtrl_SaveFocused();
}

// ---------- write-back of edited files ----------

void Panel::WatchExternalEdit(const CExternalEdit &e)
{
  if (!_watcher)
  {
    _watcher = new QFileSystemWatcher(this);
    connect(_watcher, &QFileSystemWatcher::fileChanged, this, &Panel::OnExternalFileChanged);
  }
  _edits.append(e);
  _watcher->addPath(e.FilePath);
}

void Panel::OnExternalFileChanged(const QString &path)
{
  if (_askingWriteBack)
    return;
  for (int i = 0; i < _edits.size(); i++)
  {
    CExternalEdit &e = _edits[i];
    if (e.FilePath != path)
      continue;
    // editors often save by replacing the file: watch the new one
    QTimer::singleShot(200, this, [this, path]() {
      if (_watcher && QFileInfo::exists(path) && !_watcher->files().contains(path))
        _watcher->addPath(path);
    });
    const QFileInfo fi(path);
    if (!fi.exists())
      return;
    const qint64 mtime = fi.lastModified().toMSecsSinceEpoch();
    if (fi.size() == e.Size && mtime == e.MTimeMs)
      return;
    _askingWriteBack = true;
    const UString message = MyFormatNew(IDS_WANT_UPDATE_MODIFIED_FILE, e.FullVirtPath);
    const bool update = QMessageBox::question(this, QStringLiteral("7-Zip"), ToQ(message),
        QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes;
    _askingWriteBack = false;
    e.Size = fi.size();
    e.MTimeMs = mtime;
    if (!update)
      return;
    CFolderOpPassword pw;
    pw.UsePassword = e.UsePassword;
    pw.Password = e.Password;
    const HRESULT res = FolderOp_CopyFromFile(e.FolderOperations, e.Index, ToU(path), e.RelPath, pw);
    if (res != S_OK && res != E_ABORT)
      MessageBox_Error(HResultToMessage(res));
    RefreshListCtrl_SaveFocused();
    return;
  }
}

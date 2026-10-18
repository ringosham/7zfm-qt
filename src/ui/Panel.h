// Panel.h -- the file list panel: Qt port of upstream FileManager CPanel
// (Panel.cpp, PanelFolderChange.cpp, PanelItemOpen.cpp, PanelItems.cpp,
// PanelSelect.cpp, PanelKey.cpp). Navigation and Open Inside follow
// upstream line by line; in particular a nested item is opened from the
// parent's IInArchiveGetStream stream when possible, without extraction.

#ifndef Z7QT_PANEL_H
#define Z7QT_PANEL_H

#include "Common/Common.h"
#include "Common/MyCom.h"
#include "Windows/FileFind.h"
#include "7zip/UI/FileManager/IFolder.h"
#include "7zip/UI/FileManager/ViewSettings.h"

#include "FolderOps.h"

#include <QPointer>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <memory>

class QComboBox;
class QListView;
class QStackedWidget;
class QTemporaryDir;
class QTreeView;
class QAbstractItemView;
class QItemSelectionModel;
class FolderModel;
class UiHooksQt;

// upstream CTempFileInfo
struct CTempFileInfo
{
  UInt32 FileIndex = (UInt32)(Int32)-1;
  UString RelPath;
  FString FolderPath;
  FString FilePath;
  NWindows::NFile::NFind::CFileInfo FileInfo;
  bool NeedDelete = false;
};

// upstream CFolderLink
struct CFolderLink: public CTempFileInfo
{
  bool IsVirtual = false;
  bool UsePassword = false;
  CMyComPtr<IFolderFolder> ParentFolder; // can be NULL, if parent is FS folder (in _parentFolders[0])
  UString ParentFolderPath; // including tail slash (doesn't include paths parts of parent in next level)
  UString Password;
  UString VirtualPath; // without tail slash
  std::shared_ptr<QTemporaryDir> TempDir; // keeps an extracted temp file alive
};

class CVirtFileSystem;

// upstream CCopyToOptions
struct CCopyToOptions
{
  bool streamMode = false;
  bool moveMode = false;
  bool testMode = false;
  bool includeAltStreams = true;
  bool replaceAltStreamChars = false;
  bool showErrorMessages = false;

  UString folder;
  UStringVector hashMethods;

  CVirtFileSystem *VirtFileSystemSpec = NULL;
};

struct COpenResult
{
  bool ArchiveIsOpened = false;
  bool Encrypted = false;
  UString ErrorMessage;
};

// upstream CSelectedState
struct CSelectedState
{
  int FocusedItem = -1;
  bool FocusedName_Defined = false;
  bool SelectFocused = true;
  UString FocusedName;
  UStringVector SelectedNames;
};

class Panel : public QWidget
{
  Q_OBJECT
public:
  explicit Panel(QWidget *parent = nullptr);
  ~Panel() override;

  // ---- navigation (PanelFolderChange.cpp) ----
  HRESULT BindToPath(const UString &fullPath, const UString &arcFormat, COpenResult &openRes);
  HRESULT BindToPathAndRefresh(const UString &path);
  HRESULT BindToStartPath(const UString &path);
  void OpenParentFolder();
  void OpenRootFolder();
  void CloseOpenFolders();
  void RefreshListCtrl();
  void RefreshListCtrl(const CSelectedState &state);
  void RefreshListCtrl_SaveFocused();

  // ---- opening items (PanelItemOpen.cpp, PanelItems.cpp) ----
  void OpenSelectedItems(bool tryInternal);
  void OpenFocusedItemAsInternal(const wchar_t *type = NULL);
  void OpenItem(unsigned index, bool tryInternal, bool tryExternal, const wchar_t *type = NULL);
  void OpenFolder(unsigned index);
  void OpenFocusedItemOutside();          // Shift+Enter
  void EditItem(bool useEditor);          // F3 (view) / F4 (edit)

  // ---- copy / extract / test (PanelCopy.cpp, PanelOperations.cpp) ----
  HRESULT CopyTo(CCopyToOptions &options, const CRecordVector<UInt32> &indices,
      UStringVector *messages, bool &usePassword, UString &password,
      const UStringVector *filePaths = NULL);
  void CopyToDialog(bool moveMode);       // F5 / F6
  void TestArchives();
  void ExtractArchives();                 // toolbar "Extract"
  void AddToArchive();                    // toolbar "Add"

  // ---- operations (PanelOperations.cpp) ----
  void DeleteItems(bool toTrash);         // Del / Shift+Del
  void RenameFile();                      // F2
  void RenameFocused(const QString &newName);
  void CreateFolder();                    // F7
  void CreateFile();                      // Ctrl+N
  void ChangeComment();                   // Ctrl+Z
  void DropFiles(const QStringList &paths, bool moveMode);
  void Link();                            // upstream CApp::Link
  void CalculateCrc(const char *methodName);  // upstream CApp::CalculateCrc
  void Split();                           // upstream CApp::Split
  void Combine();                         // upstream CApp::Combine
  bool CanModify() const { return _folderOperations != nullptr; }
  UString GetFsPathPublic() const { return GetFsPath(); }
  bool IsFSFolderPublic() const { return IsFSFolder(); }

  // ---- selection (PanelSelect.cpp) ----
  void SelectAll(bool select);
  void InvertSelection();
  void SelectByMask(bool select);  // Num+ / Num- dialog

  void Properties();
  void StartDrag();                        // drag selected items out
  void SelectByName(const QString &name);  // focus + select one item
  void SetViewMode(int mode);  // 0 large icons, 1 small icons, 2 list, 3 details
  void ApplySettings();        // upstream CFmSettings (Options > Settings)
  void SetFlatMode(bool flat); // View > Flat View (archives)
  bool FlatMode() const { return _flatMode; }
  int ViewMode() const { return _viewMode; }
  void FocusAddressBar();
  void FocusList();

  UString CurrentFolderPrefix() const { return _currentFolderPrefix; }
  QString StatusText() const;
  bool IsArchiveOpen() const { return !_parentFolders.isEmpty(); }
  int ArchiveDepth() const { return _parentFolders.size(); }

  // realIndex of every selected item (or the focused one if none)
  void Get_ItemIndices_Operated(CRecordVector<UInt32> &indices) const;

  IFolderFolder *Folder() const { return _folder; }
  UiHooksQt *Ui() const { return _ui; }

  // column layout per folder type (upstream SaveListViewInfo / InitColumns)
  void SaveListViewInfo();
  QStringList History() const;
  void SetHistory(const QStringList &paths);

signals:
  void FolderChanged(const QString &path);
  void StatusChanged();

protected:
  bool eventFilter(QObject *obj, QEvent *ev) override;

private:
  friend class CPanelSplitAdapter;
  // upstream helpers
  void SetNewFolder(IFolderFolder *newFolder);
  void ReleaseFolder();
  void SetToRootFolder();
  void CloseOneLevel();
  void LoadFullPath();
  void LoadFullPathAndShow();
  UString GetFsPath() const;
  bool IsFSFolder() const;
  bool IsRootFolder() const;
  bool IsFSDrivesFolder() const;
  UString GetFolderTypeID() const;

  HRESULT OpenAsArc(IInStream *inStream, const CTempFileInfo &tempFileInfo,
      const UString &virtualFilePath, const UString &arcFormat, COpenResult &openRes);
  HRESULT OpenAsArc_Msg(IInStream *inStream, const CTempFileInfo &tempFileInfo,
      const UString &virtualFilePath, const UString &arcFormat);
  HRESULT OpenAsArc_Name(const UString &relPath, const UString &arcFormat);
  HRESULT OpenAsArc_Index(unsigned index, const wchar_t *type);
  void OpenItemInArchive(unsigned index, bool tryInternal, bool tryExternal,
      bool editMode, bool useEditor, const wchar_t *type);
  std::shared_ptr<QTemporaryDir> CreateTempDir();
  bool StartApplication(const UString &path, bool editMode, bool useEditor);
  HRESULT BindToNestedPath(UString &path);

  UString GetItemsInfoString(const CRecordVector<UInt32> &indices);
  void GetFilePaths(const CRecordVector<UInt32> &indices, UStringVector &paths, bool allowFolders = false) const;
  UString GetItemName(unsigned realIndex) const;
  UString GetItemRelPath(unsigned realIndex) const;
  bool IsItem_Folder(unsigned realIndex) const;
  UInt64 GetItemSize(unsigned realIndex) const;

  void SaveSelectedState(CSelectedState &s) const;
  int FocusedRealIndex() const;

  void MessageBox_Error(const UString &message);
  void MessageBox_Error_HRESULT(HRESULT res);

  QAbstractItemView *CurrentView() const;
  void OnActivated();
  void OnAddressEntered();
  void InitColumns();
  void ShowHeaderMenu(const QPoint &pos);

  CMyComPtr<IFolderFolder> _folder;
  CMyComPtr<IFolderOperations> _folderOperations;
  QVector<CFolderLink> _parentFolders;
  UString _currentFolderPrefix;

  UiHooksQt *_ui;
  FolderModel *_model;
  QComboBox *_address;
  QStackedWidget *_stack;
  QTreeView *_details;
  QListView *_list;
  QItemSelectionModel *_selection;
  int _viewMode = 3;
  UString _typeIDString;
  CListViewInfo _listViewInfo;
  bool _needSaveInfo = false;
  bool _singleClick = false;
  bool _flatMode = false;
  QVector<std::shared_ptr<QTemporaryDir>> _externalTemps; // files opened in other apps

  // files from archives being edited in other applications (upstream
  // CTmpProcessInfo): written back on change, like 7zFM
  struct CExternalEdit
  {
    std::shared_ptr<QTemporaryDir> TempDir;
    QString FilePath;
    UInt32 Index;
    CMyComPtr<IFolderOperations> FolderOperations;
    UString RelPath;
    UString FullVirtPath;
    bool UsePassword;
    UString Password;
    qint64 Size;
    qint64 MTimeMs;
  };
  QVector<CExternalEdit> _edits;
  class QFileSystemWatcher *_watcher = nullptr;
  bool _askingWriteBack = false;
  void WatchExternalEdit(const CExternalEdit &e);
  void OnExternalFileChanged(const QString &path);
  void OnRenameRequested(UInt32 realIndex, const QString &newName);
  CFolderOpPassword CurrentPassword() const;
  void StorePassword(const CFolderOpPassword &pw);
};

#endif

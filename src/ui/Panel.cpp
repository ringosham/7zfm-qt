// Panel.cpp -- see Panel.h. Function names and control flow follow upstream
// CPanel so the two can be compared side by side.

#include "Panel.h"

#include "Common/Defs.h"
#include "Common/IntToString.h"
#include "Common/StringConvert.h"
#include "Common/Wildcard.h"
#include "Windows/FileDir.h"
#include "Windows/FileName.h"
#include "Windows/PropVariant.h"
#include "Windows/PropVariantConv.h"
#include "Windows/System.h"

#include "7zip/PropID.h"
#include "7zip/Archive/IArchive.h"
#include "7zip/UI/FileManager/FormatUtils.h"
#include "7zip/UI/FileManager/LangUtils.h"
#include "7zip/UI/FileManager/resource.h"
#include "7zip/UI/GUI/ExtractRes.h"
#include "7zip/UI/Common/ExtractingFilePath.h"
#include "7zip/UI/FileManager/RegistryUtils.h"
#include "7zip/Common/StreamObjects.h"

#include "ExtractCallback.h"

#include "FileFolderPluginOpen.h"
#include "FolderModel.h"
#include "IconProvider.h"
#include "ListFormat.h"
#include "Messages.h"
#include "PanelProperties.h"
#include "PropertiesDialog.h"
#include "QtConv.h"
#include "RootFolder.h"
#include "UiHooksQt.h"

#include <QComboBox>
#include <QDesktopServices>
#include <QDrag>
#include <QMimeData>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QDir>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QStackedWidget>
#include <QStyle>
#include <QToolButton>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

using namespace NWindows;
using namespace NFile;
using namespace NFind;

static UString GetFolderPath(IFolderFolder *folder)
{
  {
    NCOM::CPropVariant prop;
    if (folder->GetFolderProperty(kpidPath, &prop) == S_OK)
      if (prop.vt == VT_BSTR)
        return (wchar_t *)prop.bstrVal;
  }
  return UString();
}

namespace {

// the views forward drag starts to the panel (upstream PanelDrag.cpp)
template <class TBase>
class CDragView : public TBase
{
public:
  Panel *Owner = nullptr;
protected:
  void startDrag(Qt::DropActions) override
  {
    if (Owner)
      Owner->StartDrag();
  }
};

}

Panel::Panel(QWidget *parent): QWidget(parent)
{
  _ui = new UiHooksQt(this);
  _model = new FolderModel(this);

  auto *up = new QToolButton;
  up->setIcon(QIcon::fromTheme(QStringLiteral("go-up"), style()->standardIcon(QStyle::SP_FileDialogToParent)));
  up->setToolTip(tr("Up one level (Backspace)"));
  up->setAutoRaise(true);
  connect(up, &QToolButton::clicked, this, [this]() { OpenParentFolder(); });

  _address = new QComboBox;
  _address->setEditable(true);
  _address->setInsertPolicy(QComboBox::NoInsert);
  _address->setMaxVisibleItems(20);
  _address->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  connect(_address->lineEdit(), &QLineEdit::returnPressed, this, &Panel::OnAddressEntered);
  connect(_address, &QComboBox::activated, this, [this](int) { OnAddressEntered(); });

  auto *details = new CDragView<QTreeView>;
  details->Owner = this;
  _details = details;
  _details->setRootIsDecorated(false);
  _details->setUniformRowHeights(true);
  _details->setAllColumnsShowFocus(true);
  _details->setSelectionMode(QAbstractItemView::ExtendedSelection);
  _details->setSelectionBehavior(QAbstractItemView::SelectRows);
  _details->setEditTriggers(QAbstractItemView::NoEditTriggers);
  _details->setDragEnabled(true);
  _details->setModel(_model);
  _details->header()->setSectionsClickable(true);
  _details->header()->setSortIndicatorShown(true);
  _details->header()->setStretchLastSection(false);
  _details->header()->setSectionsMovable(true);
  _details->header()->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(_details->header(), &QHeaderView::customContextMenuRequested, this, &Panel::ShowHeaderMenu);
  connect(_details->header(), &QHeaderView::sectionClicked, this, [this](int section) {
    _model->SortByColumn(section);
    _details->header()->setSortIndicator(_model->SortColumn(),
        _model->SortAscending() ? Qt::AscendingOrder : Qt::DescendingOrder);
    if (CurrentView()->currentIndex().isValid())
      CurrentView()->scrollTo(CurrentView()->currentIndex());
  });

  auto *list = new CDragView<QListView>;
  list->Owner = this;
  _list = list;
  _list->setModel(_model);
  _list->setUniformItemSizes(true);
  _list->setSelectionMode(QAbstractItemView::ExtendedSelection);
  _list->setEditTriggers(QAbstractItemView::NoEditTriggers);
  _list->setDragEnabled(true);
  _list->setResizeMode(QListView::Adjust);

  // one selection model shared by both views, so switching views keeps it
  _selection = _details->selectionModel();
  _list->setSelectionModel(_selection);
  connect(_selection, &QItemSelectionModel::selectionChanged, this, [this]() { emit StatusChanged(); });
  connect(_selection, &QItemSelectionModel::currentChanged, this, [this]() { emit StatusChanged(); });

  for (QAbstractItemView *v : { (QAbstractItemView *)_details, (QAbstractItemView *)_list })
  {
    v->installEventFilter(this);
    connect(v, &QAbstractItemView::activated, this, [this](const QModelIndex &) { OnActivated(); });
  }

  _stack = new QStackedWidget;
  _stack->addWidget(_list);
  _stack->addWidget(_details);
  _stack->setCurrentWidget(_details);

  auto *top = new QHBoxLayout;
  top->setContentsMargins(0, 0, 0, 0);
  top->addWidget(up);
  top->addWidget(_address);
  auto *lay = new QVBoxLayout(this);
  lay->setContentsMargins(0, 0, 0, 0);
  lay->setSpacing(2);
  lay->addLayout(top);
  lay->addWidget(_stack);
}

Panel::~Panel()
{
  SaveListViewInfo();
  _needSaveInfo = false;
  _model->Clear();
  CloseOpenFolders();
  delete _ui;
}

QAbstractItemView *Panel::CurrentView() const
{
  return _viewMode == 3 ? (QAbstractItemView *)_details : (QAbstractItemView *)_list;
}

void Panel::SetViewMode(int mode)
{
  if (mode < 0 || mode > 3)
    return;
  _viewMode = mode;
  if (mode == 3)
    _stack->setCurrentWidget(_details);
  else
  {
    _list->setViewMode(mode == 2 ? QListView::ListMode : QListView::IconMode);
    _list->setFlow(mode == 2 ? QListView::TopToBottom : QListView::LeftToRight);
    _list->setWrapping(true);
    const int s = mode == 0 ? 48 : 16;
    _list->setIconSize(QSize(s, s));
    _list->setGridSize(mode == 0 ? QSize(96, 80) : QSize());
    _list->setSpacing(mode == 1 ? 4 : 0);
    _stack->setCurrentWidget(_list);
  }
  CurrentView()->setFocus();
}

void Panel::FocusAddressBar()
{
  _address->setFocus();
  _address->lineEdit()->selectAll();
}

void Panel::FocusList() { CurrentView()->setFocus(); }

// ---------- folder state ----------

void Panel::SetNewFolder(IFolderFolder *newFolder)
{
  _folder = newFolder;
  _folderOperations.Release();
  if (_folder)
    _folder.QueryInterface(IID_IFolderOperations, &_folderOperations);
}

void Panel::ReleaseFolder()
{
  SaveListViewInfo();
  _model->Clear();
  _folderOperations.Release();
  _folder.Release();
}

void Panel::SetToRootFolder()
{
  ReleaseFolder();
  CRootFolder *rootFolderSpec = new CRootFolder;
  SetNewFolder(rootFolderSpec);
  rootFolderSpec->Init();
}

UString Panel::GetFolderTypeID() const
{
  NCOM::CPropVariant prop;
  if (_folder && _folder->GetFolderProperty(kpidType, &prop) == S_OK)
    if (prop.vt == VT_BSTR)
      return (const wchar_t *)prop.bstrVal;
  return UString();
}

bool Panel::IsRootFolder() const { return GetFolderTypeID() == L"RootFolder"; }
bool Panel::IsFSFolder() const { return GetFolderTypeID() == L"FSFolder"; }
bool Panel::IsFSDrivesFolder() const { return GetFolderTypeID() == L"FSDrives"; }

UString Panel::GetFsPath() const
{
  return _currentFolderPrefix;
}

void Panel::LoadFullPath()
{
  _currentFolderPrefix.Empty();
  for (const CFolderLink &folderLink : _parentFolders)
  {
    _currentFolderPrefix += folderLink.ParentFolderPath;
    _currentFolderPrefix += folderLink.RelPath;
    _currentFolderPrefix.Add_PathSepar();
  }
  if (_folder)
    _currentFolderPrefix += GetFolderPath(_folder);
}

void Panel::LoadFullPathAndShow()
{
  LoadFullPath();
  const QString path = ToQ(_currentFolderPrefix);
  _address->lineEdit()->setText(path);
  // folder history (upstream AppState FolderHistory), most recent first
  const int old = _address->findText(path);
  if (old >= 0)
    _address->removeItem(old);
  _address->insertItem(0, path);
  while (_address->count() > 50)
    _address->removeItem(_address->count() - 1);
  _address->setCurrentIndex(0);
  emit FolderChanged(path);
}

// ---------- BindToPath (PanelFolderChange.cpp) ----------

HRESULT Panel::BindToPath(const UString &fullPath, const UString &arcFormat, COpenResult &openRes)
{
  UString path = fullPath;

  openRes.ArchiveIsOpened = false;
  openRes.Encrypted = false;

  for (; !_parentFolders.isEmpty(); CloseOneLevel())
  {
    // ---------- we try to use open archive ----------

    const CFolderLink &link = _parentFolders.back();
    const UString &virtPath = link.VirtualPath;
    if (!path.IsPrefixedBy(virtPath))
      continue;
    UString relatPath = path.Ptr(virtPath.Len());
    if (!relatPath.IsEmpty())
    {
      if (!IS_PATH_SEPAR(relatPath[0]))
        continue;
      else
        relatPath.Delete(0);
    }

    UString relatPath2 = relatPath;
    if (!relatPath2.IsEmpty() && !IS_PATH_SEPAR(relatPath2.Back()))
      relatPath2.Add_PathSepar();

    for (;;)
    {
      const UString foldPath = GetFolderPath(_folder);
      if (relatPath2 == foldPath)
        break;
      if (relatPath.IsPrefixedBy(foldPath))
      {
        path = relatPath.Ptr(foldPath.Len());
        break;
      }
      CMyComPtr<IFolderFolder> newFolder;
      if (_folder->BindToParentFolder(&newFolder) != S_OK || !newFolder)
        return E_FAIL;
      SetNewFolder(newFolder);
    }
    break;
  }

  if (_parentFolders.isEmpty())
  {
    // ---------- we open file or folder from file system ----------

    CloseOpenFolders();
    UString sysPath = path;
    const unsigned prefixSize = NName::GetRootPrefixSize(sysPath);
    if (prefixSize == 0 || sysPath[prefixSize] == 0)
      sysPath.Empty();

    CFileInfo fileInfo;

    while (!sysPath.IsEmpty())
    {
      if (sysPath.Len() <= prefixSize)
      {
        path.DeleteFrom(prefixSize);
        sysPath.Empty();
        break;
      }

      fileInfo.ClearBase();
      if (IsPathSepar(sysPath.Back()))
      {
        CDirEntry fi;
        CEnumerator enumerator;
        enumerator.SetDirPrefix(us2fs(sysPath));
        bool found = false;
        if (enumerator.Next(fi, found))
        {
          fileInfo.SetAsDir();
          fileInfo.Size = 0;
          fileInfo.Name.Empty();
          break;
        }
        sysPath.DeleteBack();
        continue;
      }

      if (fileInfo.Find(us2fs(sysPath)))
        break;
      int pos = sysPath.ReverseFind_PathSepar();
      if (pos < 0)
      {
        sysPath.Empty();
        break;
      }
      {
        if ((unsigned)pos != sysPath.Len() - 1)
          pos++;
        sysPath.DeleteFrom((unsigned)pos);
      }
    }

    SetToRootFolder();

    CMyComPtr<IFolderFolder> newFolder;

    if (sysPath.IsEmpty())
    {
      _folder->BindToFolder(path, &newFolder);
    }
    else if (fileInfo.IsDir())
    {
      NName::NormalizeDirPathPrefix(sysPath);
      _folder->BindToFolder(sysPath, &newFolder);
    }
    else
    {
      FString dirPrefix, fileName;

      NDir::GetFullPathAndSplit(us2fs(sysPath), dirPrefix, fileName);

      HRESULT res = S_OK;
      {
        CTempFileInfo tfi;
        tfi.RelPath = fs2us(fileName);
        tfi.FolderPath = dirPrefix;
        tfi.FilePath = us2fs(sysPath);
        res = OpenAsArc(NULL, tfi, sysPath, arcFormat, openRes);
      }

      if (res == S_FALSE)
        _folder->BindToFolder(fs2us(dirPrefix), &newFolder);
      else
      {
        RINOK(res)
        openRes.ArchiveIsOpened = true;
        _parentFolders.back().ParentFolderPath = fs2us(dirPrefix);
        path.DeleteFrontal(sysPath.Len());
        if (!path.IsEmpty() && IS_PATH_SEPAR(path[0]))
          path.Delete(0);
      }
    }

    if (newFolder)
    {
      SetNewFolder(newFolder);
      return S_OK;
    }
  }

  return BindToNestedPath(path);
}

// The tail of upstream BindToPath(): enter the remaining path components
// inside the open archive. 7zfm-qt addition: a component that is not a
// folder but an item with an in-place stream (a partition, a disk image, a
// tar inside xz, ...) is opened as a nested archive, so typing
// "/x/disk.vhdx/1.linux.img/home" works like navigating there by hand.
HRESULT Panel::BindToNestedPath(UString &path)
{
  for (unsigned curPos = 0; curPos != path.Len();)
  {
    UString s = path.Ptr(curPos);
    const int slashPos = NName::FindSepar(s);
    unsigned skipLen = s.Len();
    if (slashPos >= 0)
    {
      s.DeleteFrom((unsigned)slashPos);
      skipLen = (unsigned)slashPos + 1;
    }
    if (s.IsEmpty())
    {
      curPos += skipLen;
      continue;
    }

    CMyComPtr<IFolderFolder> newFolder;
    _folder->BindToFolder(s, &newFolder);
    if (newFolder)
    {
      curPos += skipLen;
      SetNewFolder(newFolder);
      continue;
    }

    if (_parentFolders.isEmpty())
      break;
    _folder->LoadItems();
    UInt32 numItems = 0;
    _folder->GetNumberOfItems(&numItems);
    UInt32 index = 0;
    for (; index < numItems; index++)
      if (GetItemName(index) == s && !IsItem_Folder(index))
        break;
    if (index == numItems)
      break;
    CMyComPtr<IInArchiveGetStream> getStream;
    _folder.QueryInterface(IID_IInArchiveGetStream, &getStream);
    if (!getStream)
      break;
    CMyComPtr<ISequentialInStream> subSeqStream;
    getStream->GetStream(index, &subSeqStream);
    CMyComPtr<IInStream> subStream;
    if (subSeqStream)
      subSeqStream.QueryInterface(IID_IInStream, &subStream);
    if (!subStream)
      break;
    LoadFullPath();
    CTempFileInfo tfi;
    tfi.FileIndex = index;
    tfi.RelPath = GetItemRelPath(index);
    COpenResult openRes;
    if (OpenAsArc(subStream, tfi, _currentFolderPrefix + tfi.RelPath, UString(), openRes) != S_OK)
      break;
    curPos += skipLen;
  }
  return S_OK;
}

HRESULT Panel::BindToPathAndRefresh(const UString &path)
{
  COpenResult openRes;
  UString s = path;
  if (!s.IsEmpty() && s[0] == '\"' && s.Back() == '\"')
  {
    s.DeleteBack();
    s.Delete(0);
  }
  if (s.IsPrefixedBy(L"~"))
  {
    s.Delete(0);
    s.Insert(0, ToU(QDir::homePath()));
  }
  const HRESULT res = BindToPath(s, UString(), openRes);
  if (res != S_OK && res != E_ABORT && !openRes.ErrorMessage.IsEmpty())
    MessageBox_Error(openRes.ErrorMessage);
  else if (res != S_OK && res != E_ABORT && res != S_FALSE)
    MessageBox_Error_HRESULT(res);
  if (!_folder)
    SetToRootFolder();
  RefreshListCtrl();
  return res;
}

void Panel::OpenParentFolder()
{
  LoadFullPath();

  UString parentFolderPrefix;
  UString focusedName;

  if (!_currentFolderPrefix.IsEmpty())
  {
    const wchar_t c = _currentFolderPrefix.Back();
    if (IS_PATH_SEPAR(c))
    {
      focusedName = _currentFolderPrefix;
      focusedName.DeleteBack();
      const int pos = focusedName.ReverseFind_PathSepar();
      if (pos >= 0)
      {
        parentFolderPrefix = focusedName;
        parentFolderPrefix.DeleteFrom((unsigned)(pos + 1));
        focusedName.DeleteFrontal((unsigned)(pos + 1));
      }
      if (focusedName.IsEmpty() && _currentFolderPrefix == L"/")
        focusedName = L"/";
    }
  }

  CMyComPtr<IFolderFolder> newFolder;
  _folder->BindToParentFolder(&newFolder);

  if (newFolder)
    SetNewFolder(newFolder);
  else
  {
    bool needSetFolder = true;
    if (!_parentFolders.isEmpty())
    {
      {
        const CFolderLink &link = _parentFolders.back();
        parentFolderPrefix = link.ParentFolderPath;
        focusedName = link.RelPath;
      }
      CloseOneLevel();
      needSetFolder = (!_folder);
    }

    if (needSetFolder)
    {
      COpenResult openRes;
      BindToPath(parentFolderPrefix, UString(), openRes);
    }
  }

  CSelectedState state;
  state.FocusedName = focusedName;
  state.FocusedName_Defined = true;
  LoadFullPath();
  RefreshListCtrl(state);
}

void Panel::CloseOneLevel()
{
  ReleaseFolder();
  {
    CFolderLink &link = _parentFolders.back();
    if (link.ParentFolder)
      SetNewFolder(link.ParentFolder);
  }
  // upstream OpenParentArchiveFolder() would offer to write back an edited
  // temp file here (phase 3)
  _parentFolders.removeLast();
}

void Panel::CloseOpenFolders()
{
  while (!_parentFolders.isEmpty())
    CloseOneLevel();
  ReleaseFolder();
}

void Panel::OpenRootFolder()
{
  CloseOpenFolders();
  SetToRootFolder();
  RefreshListCtrl();
}

// ---------- opening archives (PanelItemOpen.cpp) ----------

HRESULT Panel::OpenAsArc(IInStream *inStream,
    const CTempFileInfo &tempFileInfo,
    const UString &virtualFilePath,
    const UString &arcFormat,
    COpenResult &openRes)
{
  openRes.Encrypted = false;
  CFolderLink folderLink;
  (CTempFileInfo &)folderLink = tempFileInfo;

  if (inStream)
    folderLink.IsVirtual = true;
  else
  {
    if (!folderLink.FileInfo.Find(folderLink.FilePath))
      return GetLastError_noZero_HRESULT();
    if (folderLink.FileInfo.IsDir())
      return S_FALSE;
    folderLink.IsVirtual = false;
  }

  folderLink.VirtualPath = virtualFilePath;

  CFfpOpen ffp;
  const HRESULT res = ffp.OpenFileFolderPlugin(inStream,
      folderLink.FilePath.IsEmpty() ? us2fs(virtualFilePath) : folderLink.FilePath,
      arcFormat, _ui);

  openRes.Encrypted = ffp.Encrypted;
  openRes.ErrorMessage = ffp.ErrorMessage;

  RINOK(res)

  folderLink.Password = ffp.Password;
  folderLink.UsePassword = ffp.Encrypted;

  if (_folder)
    folderLink.ParentFolderPath = GetFolderPath(_folder);
  else
    folderLink.ParentFolderPath = _currentFolderPrefix;

  if (!_parentFolders.isEmpty())
    folderLink.ParentFolder = _folder;

  _parentFolders.append(folderLink);

  ReleaseFolder();
  SetNewFolder(ffp.Folder);

  if (!openRes.ErrorMessage.IsEmpty())
    MessageBox_Error(openRes.ErrorMessage);

  return S_OK;
}

HRESULT Panel::OpenAsArc_Msg(IInStream *inStream,
    const CTempFileInfo &tempFileInfo,
    const UString &virtualFilePath,
    const UString &arcFormat)
{
  COpenResult opRes;

  HRESULT res = OpenAsArc(inStream, tempFileInfo, virtualFilePath, arcFormat, opRes);

  if (res == S_OK)
    return res;
  if (res == E_ABORT)
    return res;

  if (opRes.Encrypted || res != S_FALSE)
  {
    UString message;
    if (res == S_FALSE)
    {
      message = MyFormatNew(
          opRes.Encrypted ?
            IDS_CANT_OPEN_ENCRYPTED_ARCHIVE :
            IDS_CANT_OPEN_ARCHIVE,
          virtualFilePath);
    }
    else
      message = HResultToMessage(res);
    MessageBox_Error(message);
  }

  return res;
}

HRESULT Panel::OpenAsArc_Name(const UString &relPath, const UString &arcFormat)
{
  CTempFileInfo tfi;
  tfi.RelPath = relPath;
  tfi.FolderPath = us2fs(GetFsPath());
  const UString fullPath = GetFsPath() + relPath;
  tfi.FilePath = us2fs(fullPath);
  return OpenAsArc_Msg(NULL, tfi, fullPath, arcFormat);
}

HRESULT Panel::OpenAsArc_Index(unsigned index, const wchar_t *type)
{
  HRESULT res = OpenAsArc_Name(GetItemRelPath(index), type ? type : L"");
  if (res != S_OK)
    return res;
  RefreshListCtrl();
  return S_OK;
}

void Panel::OpenItem(unsigned index, bool tryInternal, bool tryExternal, const wchar_t *type)
{
  const UString name = GetItemRelPath(index);

  if (!_parentFolders.isEmpty())
  {
    OpenItemInArchive(index, tryInternal, tryExternal, false, false, type);
    return;
  }

  UString prefix = GetFsPath();
  UString fullPath = prefix + name;

  if (tryInternal)
  {
    HRESULT res = OpenAsArc_Index(index, type);
    if (res == S_OK || res == E_ABORT)
      return;
    if (res != S_FALSE)
    {
      MessageBox_Error_HRESULT(res);
      return;
    }
  }

  if (tryExternal)
    StartApplication(fullPath, false, false);
}

void Panel::OpenFocusedItemOutside()
{
  const int focused = FocusedRealIndex();
  if (focused < 0 || IsItem_Folder((unsigned)focused))
    return;
  if (_parentFolders.isEmpty())
    StartApplication(GetFsPath() + GetItemRelPath((unsigned)focused), false, false);
  else
    OpenItemInArchive((unsigned)focused, false, true, false, false, NULL);
}

void Panel::EditItem(bool useEditor)
{
  const int focused = FocusedRealIndex();
  if (focused < 0 || IsItem_Folder((unsigned)focused))
    return;
  if (_parentFolders.isEmpty())
    StartApplication(GetFsPath() + GetItemRelPath((unsigned)focused), true, useEditor);
  else
    OpenItemInArchive((unsigned)focused, false, true, true, useEditor, NULL);
}

std::shared_ptr<QTemporaryDir> Panel::CreateTempDir()
{
  // upstream uses %TEMP%\7zO*; we avoid tmpfs /tmp (RAM) for big items
  QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
  QDir().mkpath(base);
  auto dir = std::make_shared<QTemporaryDir>(base + QLatin1String("/7zO"));
  if (!dir->isValid())
    return nullptr;
  return dir;
}

bool Panel::StartApplication(const UString &path, bool editMode, bool useEditor)
{
  if (editMode && useEditor)
  {
    UString editor;
    ReadRegEditor(false, editor);
    if (!editor.IsEmpty())
    {
      QStringList args = QProcess::splitCommand(ToQ(editor));
      if (!args.isEmpty())
      {
        const QString prog = args.takeFirst();
        args << ToQ(path);
        if (QProcess::startDetached(prog, args))
          return true;
      }
    }
  }
  return QDesktopServices::openUrl(QUrl::fromLocalFile(ToQ(path)));
}

void Panel::OpenItemInArchive(unsigned index, bool tryInternal, bool tryExternal,
    bool editMode, bool useEditor, const wchar_t *type)
{
  const UString name = GetItemName(index);
  const UString relPath = GetItemRelPath(index);

  if (!_folderOperations)
  {
    MessageBox_Error(LangString(IDS_OPERATION_IS_NOT_SUPPORTED));
    return;
  }

  bool tryAsArchive = tryInternal;

  const UString fullVirtPath = _currentFolderPrefix + relPath;

  std::shared_ptr<QTemporaryDir> tempDirectory = CreateTempDir();
  if (!tempDirectory)
  {
    MessageBox_Error(UString(L"Cannot create a temporary folder"));
    return;
  }

  FString tempDir = QToFs(tempDirectory->path());
  FString tempDirNorm = tempDir;
  NName::NormalizeDirPathPrefix(tempDirNorm);
  const FString tempFilePath = tempDirNorm + us2fs(Get_Correct_FsFile_Name(name));

  CTempFileInfo tempFileInfo;
  tempFileInfo.FileIndex = index;
  tempFileInfo.RelPath = relPath;
  tempFileInfo.FolderPath = tempDir;
  tempFileInfo.FilePath = tempFilePath;
  tempFileInfo.NeedDelete = true;

  if (tryAsArchive)
  {
    CMyComPtr<IInArchiveGetStream> getStream;
    _folder.QueryInterface(IID_IInArchiveGetStream, &getStream);
    if (getStream)
    {
      CMyComPtr<ISequentialInStream> subSeqStream;
      getStream->GetStream(index, &subSeqStream);
      if (subSeqStream)
      {
        CMyComPtr<IInStream> subStream;
        subSeqStream.QueryInterface(IID_IInStream, &subStream);
        if (subStream)
        {
          // the nested item is opened in place: nothing is extracted
          CTempFileInfo tfi = tempFileInfo;
          tfi.FolderPath.Empty();
          tfi.FilePath.Empty();
          tfi.NeedDelete = false;
          HRESULT res = OpenAsArc_Msg(subStream, tfi, fullVirtPath, type ? type : L"");
          if (res == S_OK)
          {
            RefreshListCtrl();
            return;
          }
          if (res == E_ABORT || res != S_FALSE)
            return;
          if (!tryExternal)
            return;
          tryAsArchive = false;
        }
      }
    }
  }

  CRecordVector<UInt32> indices;
  indices.Add(index);

  UStringVector messages;

  bool usePassword = false;
  UString password;
  if (!_parentFolders.isEmpty())
  {
    const CFolderLink &fl = _parentFolders.back();
    usePassword = fl.UsePassword;
    password = fl.Password;
  }

  CVirtFileSystem *virtFileSystemSpec = NULL;
  CMyComPtr<ISequentialOutStream> virtFileSystem;

  CCopyToOptions options;
  options.includeAltStreams = true;
  options.replaceAltStreamChars = false;

  if (tryAsArchive)
  {
    NCOM::CPropVariant prop;
    _folder->GetProperty(index, kpidSize, &prop);
    size_t ramSize = (size_t)1 << 30;
    const bool ramDefined = NSystem::GetRamSize(ramSize);
    const size_t fileLimit = ramDefined ?
        ramSize >> MyMax((unsigned)_parentFolders.size() + 1, 8u) :
        (size_t)1 << 22;
    UInt64 fileSize = 0;
    if (!ConvertPropVariantToUInt64(prop, fileSize))
      fileSize = fileLimit;

    if (fileSize <= fileLimit)
    {
      // small item: extract into RAM and open it from there
      options.streamMode = true;
      virtFileSystemSpec = new CVirtFileSystem;
      virtFileSystem = virtFileSystemSpec;
      virtFileSystemSpec->FileName = name;
      virtFileSystemSpec->IsAltStreamFile = false;
      virtFileSystemSpec->MaxTotalAllocSize = (size_t)fileSize + (1 << 16);
      virtFileSystemSpec->DirPrefix = tempDirNorm;
      options.VirtFileSystemSpec = virtFileSystemSpec;
    }
  }

  options.folder = fs2us(tempDirNorm);
  options.showErrorMessages = true;

  const HRESULT result = CopyTo(options, indices, &messages, usePassword, password);

  if (!_parentFolders.isEmpty())
  {
    CFolderLink &fl = _parentFolders.back();
    fl.UsePassword = usePassword;
    fl.Password = password;
  }

  if (!messages.IsEmpty())
    return;
  if (result != S_OK)
  {
    if (result != E_ABORT)
      MessageBox_Error_HRESULT(result);
    return;
  }

  if (virtFileSystemSpec && !virtFileSystemSpec->WasStreamFlushedToFS())
  {
    int index_in_Files = virtFileSystemSpec->Index_of_MainExtractedFile_in_Files;
    if (index_in_Files < 0)
    {
      if (virtFileSystemSpec->Files.Size() != 1)
      {
        MessageBox_Error_HRESULT(E_FAIL);
        return;
      }
      index_in_Files = 0;
    }
    {
      const CVirtFile &file = virtFileSystemSpec->Files[(unsigned)index_in_Files];
      CMyComPtr2_Create<IInStream, CBufInStream> bufInStream;
      bufInStream->Init(file.Data, file.WrittenSize, virtFileSystem);
      CTempFileInfo tfi = tempFileInfo;
      tfi.FilePath.Empty();
      tfi.NeedDelete = false;
      const HRESULT res = OpenAsArc_Msg(bufInStream, tfi, fullVirtPath, type ? type : L"");
      if (res == S_OK)
      {
        RefreshListCtrl();
        return;
      }
      if (res == E_ABORT || res != S_FALSE)
        return;
      if (!tryExternal)
        return;
      tryAsArchive = false;
      if (virtFileSystemSpec->FlushToDisk(true) != S_OK)
        return;
    }
  }

  if (tryAsArchive)
  {
    const HRESULT res = OpenAsArc_Msg(NULL, tempFileInfo, fullVirtPath, type ? type : L"");
    if (res == S_OK)
    {
      _parentFolders.back().TempDir = tempDirectory;  // keep it while open
      RefreshListCtrl();
      return;
    }
    if (res == E_ABORT || res != S_FALSE)
      return;
  }

  if (!tryExternal)
    return;

  NFind::CFileInfo fi;
  if (!fi.Find(tempFilePath))
    return;
  // upstream watches the started process and offers to write back changes
  // (phase 3); the temp copy is kept until 7zfm-qt exits
  if (StartApplication(fs2us(tempFilePath), editMode, useEditor))
    _externalTemps.append(tempDirectory);
}

void Panel::OpenFolder(unsigned index)
{
  CMyComPtr<IFolderFolder> newFolder;
  const HRESULT res = _folder->BindToFolder(index, &newFolder);
  if (res != S_OK)
  {
    MessageBox_Error_HRESULT(res);
    return;
  }
  if (!newFolder)
    return;
  SetNewFolder(newFolder);
  LoadFullPath();
  RefreshListCtrl();
}

// ---------- items (PanelItems.cpp) ----------

UString Panel::GetItemName(unsigned realIndex) const
{
  NCOM::CPropVariant prop;
  if (_folder->GetProperty(realIndex, kpidName, &prop) != S_OK)
    return UString();
  if (prop.vt != VT_BSTR)
    return UString();
  return prop.bstrVal;
}

UString Panel::GetItemRelPath(unsigned realIndex) const
{
  UString s;
  NCOM::CPropVariant prop;
  if (_folder->GetProperty(realIndex, kpidPrefix, &prop) == S_OK && prop.vt == VT_BSTR)
    s = prop.bstrVal;
  s += GetItemName(realIndex);
  return s;
}

bool Panel::IsItem_Folder(unsigned realIndex) const
{
  NCOM::CPropVariant prop;
  if (_folder->GetProperty(realIndex, kpidIsDir, &prop) != S_OK)
    return false;
  return prop.vt == VT_BOOL && prop.boolVal != VARIANT_FALSE;
}

UInt64 Panel::GetItemSize(unsigned realIndex) const
{
  NCOM::CPropVariant prop;
  if (_folder->GetProperty(realIndex, kpidSize, &prop) != S_OK)
    return 0;
  UInt64 v = 0;
  ConvertPropVariantToUInt64(prop, v);
  return v;
}

int Panel::FocusedRealIndex() const
{
  const QModelIndex cur = _selection->currentIndex();
  if (!cur.isValid())
    return -1;
  return (int)_model->RealIndex(cur.row());
}

void Panel::Get_ItemIndices_Operated(CRecordVector<UInt32> &indices) const
{
  indices.Clear();
  const QModelIndexList rows = _selection->selectedRows();
  for (const QModelIndex &i : rows)
    indices.Add(_model->RealIndex(i.row()));
  if (indices.IsEmpty())
  {
    const int focused = FocusedRealIndex();
    if (focused >= 0)
      indices.Add((UInt32)focused);
  }
  indices.Sort([](const UInt32 *a, const UInt32 *b, void *) { return MyCompare(*a, *b); }, NULL);
}

void Panel::OpenSelectedItems(bool tryInternal)
{
  CRecordVector<UInt32> indices;
  Get_ItemIndices_Operated(indices);
  if (indices.Size() > 20)
  {
    MessageBox_Error(LangString(IDS_TOO_MANY_ITEMS));
    return;
  }

  bool dirIsStarted = false;
  FOR_VECTOR (i, indices)
  {
    const UInt32 index = indices[i];
    if (IsItem_Folder(index))
    {
      if (!dirIsStarted)
      {
        if (tryInternal)
        {
          OpenFolder(index);
          dirIsStarted = true;
          break;
        }
        else if (IsFSFolder())
          dirIsStarted = true;
      }
    }
    else
      OpenItem(index, (tryInternal && indices.Size() == 1), true);
  }
}

void Panel::OpenFocusedItemAsInternal(const wchar_t *type)
{
  const int focusedItem = FocusedRealIndex();
  if (focusedItem < 0)
    return;
  const UInt32 realIndex = (UInt32)focusedItem;
  if (IsItem_Folder(realIndex))
    OpenFolder(realIndex);
  else
    OpenItem(realIndex, true, false, type);
}

void Panel::OnActivated()
{
  OpenSelectedItems(true);
}

void Panel::OnAddressEntered()
{
  BindToPathAndRefresh(ToU(_address->lineEdit()->text()));
  FocusList();
}

// ---------- refresh ----------

void Panel::SaveSelectedState(CSelectedState &s) const
{
  s.FocusedName_Defined = false;
  s.FocusedName.Empty();
  s.SelectedNames.Clear();
  const QModelIndex cur = _selection->currentIndex();
  if (cur.isValid())
  {
    s.FocusedItem = cur.row();
    s.FocusedName = _model->ItemName(cur.row());
    s.FocusedName_Defined = true;
  }
  for (const QModelIndex &i : _selection->selectedRows())
    s.SelectedNames.Add(_model->ItemName(i.row()));
}

void Panel::RefreshListCtrl()
{
  RefreshListCtrl(CSelectedState());
}

void Panel::RefreshListCtrl_SaveFocused()
{
  CSelectedState state;
  SaveSelectedState(state);
  RefreshListCtrl(state);
}

void Panel::RefreshListCtrl(const CSelectedState &state)
{
  if (!_folder)
    return;
  SaveListViewInfo();
  const HRESULT res = _folder->LoadItems();
  if (res != S_OK)
    MessageBox_Error_HRESULT(res);
  _model->SetFolder(_folder);
  InitColumns();
  const int nCols = _model->columnCount();

  QItemSelection sel;
  FOR_VECTOR (i, state.SelectedNames)
  {
    const int row = _model->RowOfName(state.SelectedNames[i]);
    if (row >= 0)
      sel.select(_model->index(row, 0), _model->index(row, nCols - 1));
  }
  int focusRow = -1;
  if (state.FocusedName_Defined)
    focusRow = _model->RowOfName(state.FocusedName);
  if (focusRow < 0 && state.FocusedItem >= 0)
    focusRow = qMin(state.FocusedItem, _model->rowCount() - 1);
  if (focusRow < 0 && _model->rowCount() > 0)
    focusRow = 0;
  if (focusRow >= 0)
  {
    _selection->setCurrentIndex(_model->index(focusRow, 0), QItemSelectionModel::NoUpdate);
    if (sel.isEmpty() && state.SelectFocused && state.FocusedName_Defined)
      sel.select(_model->index(focusRow, 0), _model->index(focusRow, nCols - 1));
    CurrentView()->scrollTo(_model->index(focusRow, 0));
  }
  _selection->select(sel, QItemSelectionModel::ClearAndSelect);

  LoadFullPathAndShow();
  emit StatusChanged();
}

// ---------- columns (PanelItems.cpp InitColumns / SaveListViewInfo) ----------

static bool GetColumnVisible(PROPID propID, bool isFsFolder)
{
  if (isFsFolder)
  {
    switch (propID)
    {
      case kpidATime:
      case kpidChangeTime:
      case kpidAttrib:
      case kpidPackSize:
      case kpidINode:
      case kpidLinks:
      case kpidNtReparse:
        return false;
    }
  }
  return true;
}

static unsigned GetColumnWidth(PROPID propID)
{
  switch (propID)
  {
    case kpidName: return 300;  // upstream 160 at 96 dpi with a smaller font
    case kpidMTime: case kpidCTime: case kpidATime: case kpidChangeTime: return 135;
    case kpidSize: case kpidPackSize: case kpidTotalSize: case kpidFreeSpace: return 130;
  }
  return 100;
}

void Panel::InitColumns()
{
  const UString oldType = _typeIDString;
  _typeIDString = GetFolderTypeID();
  if (!_needSaveInfo || _typeIDString != oldType)
    _listViewInfo.Read(_typeIDString);

  QHeaderView *h = _details->header();
  const QVector<FolderModel::CColumn> &cols = _model->Columns();
  const bool isFsFolder = IsFSFolder();

  // defaults, then saved layout (order of saved visible columns first)
  QVector<int> order;
  QVector<bool> visible(cols.size());
  QVector<int> width(cols.size());
  for (int c = 0; c < cols.size(); c++)
  {
    visible[c] = GetColumnVisible(cols[c].ID, isFsFolder);
    width[c] = (int)GetColumnWidth(cols[c].ID);
  }
  QVector<bool> placed(cols.size(), false);
  FOR_VECTOR (i, _listViewInfo.Columns)
  {
    const CColumnInfo &ci = _listViewInfo.Columns[i];
    const int c = _model->ColumnOfPropID(ci.PropID);
    if (c < 0 || placed[c])
      continue;
    placed[c] = true;
    visible[c] = ci.IsVisible || ci.PropID == kpidName;
    if (ci.Width > 0)
      width[c] = (int)ci.Width;
    order.append(c);
  }
  for (int c = 0; c < cols.size(); c++)
    if (!placed[c])
      order.append(c);

  for (int vis = 0; vis < order.size(); vis++)
  {
    const int logical = order[vis];
    const int curVisual = h->visualIndex(logical);
    if (curVisual != vis)
      h->moveSection(curVisual, vis);
    h->setSectionHidden(logical, !visible[logical]);
    h->resizeSection(logical, width[logical]);
  }

  if (_listViewInfo.IsLoaded && _model->ColumnOfPropID(_listViewInfo.SortID) >= 0
      && (_listViewInfo.SortID != _model->SortPropID() || _listViewInfo.Ascending != _model->SortAscending()))
    _model->SortByPropID(_listViewInfo.SortID, _listViewInfo.Ascending);
  h->setSortIndicator(_model->SortColumn(), _model->SortAscending() ? Qt::AscendingOrder : Qt::DescendingOrder);
  _needSaveInfo = true;
}

void Panel::SaveListViewInfo()
{
  if (!_needSaveInfo)
    return;
  QHeaderView *h = _details->header();
  const QVector<FolderModel::CColumn> &cols = _model->Columns();
  if (cols.isEmpty())
    return;

  CListViewInfo viewInfo;
  // visible columns in display order, then hidden ones (as upstream)
  for (int pass = 0; pass < 2; pass++)
    for (int vis = 0; vis < h->count(); vis++)
    {
      const int logical = h->logicalIndex(vis);
      if (logical < 0 || logical >= cols.size())
        continue;
      const bool hidden = h->isSectionHidden(logical);
      if (hidden != (pass == 1))
        continue;
      CColumnInfo ci;
      ci.PropID = cols[logical].ID;
      ci.IsVisible = !hidden;
      ci.Width = (UInt32)(hidden ? (int)GetColumnWidth(ci.PropID) : h->sectionSize(logical));
      viewInfo.Columns.Add(ci);
    }
  viewInfo.SortID = _model->SortPropID();
  viewInfo.Ascending = _model->SortAscending();
  viewInfo.IsLoaded = true;
  if (!_listViewInfo.IsEqual(viewInfo))
  {
    viewInfo.Save(_typeIDString);
    _listViewInfo = viewInfo;
  }
}

void Panel::ShowHeaderMenu(const QPoint &pos)
{
  QHeaderView *h = _details->header();
  QMenu menu(this);
  const QVector<FolderModel::CColumn> &cols = _model->Columns();
  for (int c = 0; c < cols.size(); c++)
  {
    QAction *a = menu.addAction(cols[c].Name);
    a->setCheckable(true);
    a->setChecked(!h->isSectionHidden(c));
    a->setEnabled(cols[c].ID != kpidName);
    connect(a, &QAction::toggled, this, [this, h, c](bool on) {
      h->setSectionHidden(c, !on);
      if (on && h->sectionSize(c) < 20)
        h->resizeSection(c, 100);
      SaveListViewInfo();
    });
  }
  menu.exec(h->mapToGlobal(pos));
}

QStringList Panel::History() const
{
  QStringList l;
  for (int i = 0; i < _address->count(); i++)
    l << _address->itemText(i);
  return l;
}

void Panel::SetHistory(const QStringList &paths)
{
  const QString cur = _address->lineEdit()->text();
  _address->clear();
  _address->addItems(paths);
  _address->lineEdit()->setText(cur);
}

// ---------- selection (PanelSelect.cpp) ----------

void Panel::SelectAll(bool select)
{
  if (select)
    CurrentView()->selectAll();
  else
    _selection->clearSelection();
}

void Panel::InvertSelection()
{
  const int n = _model->rowCount();
  if (n == 0)
    return;
  QItemSelection all(_model->index(0, 0), _model->index(n - 1, _model->columnCount() - 1));
  _selection->select(all, QItemSelectionModel::Toggle | QItemSelectionModel::Rows);
}

void Panel::SelectByMask(bool select)
{
  bool ok = false;
  const QString mask = QInputDialog::getText(this,
      ToQ(LangString(select ? IDS_SELECT : IDS_DESELECT)),
      ToQ(LangString(IDS_SELECT_MASK)), QLineEdit::Normal, QStringLiteral("*"), &ok);
  if (!ok || mask.isEmpty())
    return;
  const UString m = ToU(mask);
  QItemSelection sel;
  const int nCols = _model->columnCount();
  for (int row = 0; row < _model->rowCount(); row++)
    if (DoesWildcardMatchName(m, _model->ItemName(row)))
      sel.select(_model->index(row, 0), _model->index(row, nCols - 1));
  _selection->select(sel, (select ? QItemSelectionModel::Select : QItemSelectionModel::Deselect));
}

void Panel::SelectByName(const QString &name)
{
  const int row = _model->RowOfName(ToU(name));
  if (row < 0)
    return;
  const QModelIndex i = _model->index(row, 0);
  _selection->setCurrentIndex(i, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
  CurrentView()->scrollTo(i);
}

// ---------- drag out (upstream PanelDrag.cpp) ----------

void Panel::StartDrag()
{
  CRecordVector<UInt32> indices;
  Get_ItemIndices_Operated(indices);
  if (indices.IsEmpty())
    return;

  QList<QUrl> urls;
  if (_parentFolders.isEmpty() && IsFSFolder())
  {
    FOR_VECTOR (i, indices)
      urls << QUrl::fromLocalFile(ToQ(GetFsPath() + GetItemRelPath(indices[i])));
  }
  else
  {
    // Like 7zFM: the items are extracted to a temp folder first, then the
    // files are dragged from there. (Windows delays this until the drop;
    // X11/Wayland drag sources need the paths up front.)
    std::shared_ptr<QTemporaryDir> temp = CreateTempDir();
    if (!temp)
      return;
    UString dir = ToU(temp->path());
    NName::NormalizeDirPathPrefix(dir);
    CCopyToOptions options;
    options.folder = dir;
    options.showErrorMessages = true;
    bool usePassword = false;
    UString password;
    if (!_parentFolders.isEmpty())
    {
      usePassword = _parentFolders.back().UsePassword;
      password = _parentFolders.back().Password;
    }
    UStringVector messages;
    const HRESULT res = CopyTo(options, indices, &messages, usePassword, password);
    if (res != S_OK || !messages.IsEmpty())
      return;
    if (!_parentFolders.isEmpty())
    {
      _parentFolders.back().UsePassword = usePassword;
      _parentFolders.back().Password = password;
    }
    FOR_VECTOR (i, indices)
      urls << QUrl::fromLocalFile(ToQ(dir + GetItemName(indices[i])));
    _externalTemps.append(temp);  // the drop target may copy lazily
  }

  auto *mime = new QMimeData;
  mime->setUrls(urls);
  // GNOME/KDE file managers understand this for copy vs. move
  mime->setData(QStringLiteral("x-special/gnome-copied-files"),
      QByteArray("copy\n") + QUrl::toStringList(urls, QUrl::FullyEncoded).join(QLatin1Char('\n')).toUtf8());
  auto *drag = new QDrag(this);
  drag->setMimeData(mime);
  const int focused = FocusedRealIndex();
  if (focused >= 0)
  {
    const QIcon icon = IsItem_Folder((unsigned)focused) ? GetIconProvider().Folder()
        : GetIconProvider().ForFile(ToQ(GetItemName((unsigned)focused)));
    drag->setPixmap(icon.pixmap(32, 32));
  }
  drag->exec(Qt::CopyAction, Qt::CopyAction);
}

// ---------- properties ----------

void Panel::Properties()
{
  if (!_folder)
    return;
  CRecordVector<UInt32> indices;
  Get_ItemIndices_Operated(indices);
  CPropList list;
  GetFolderProperties(_folder, indices, list);
  ShowPropertiesDialog(this, ToQ(LangString(IDS_PROPERTIES)), list);
}

// ---------- status ----------

QString Panel::StatusText() const
{
  const QModelIndexList rows = _selection->selectedRows();
  UInt64 size = 0;
  for (const QModelIndex &i : rows)
    size += _model->ItemSize(i.row());
  wchar_t num[32];
  if (!rows.isEmpty())
  {
    ConvertUInt32ToString((UInt32)rows.size(), num);
    QString t = ToQ(MyFormatNew(LangString(IDS_N_SELECTED_ITEMS), num));
    if (size != 0)
      t += QLatin1String("      ") + ToQ(ConvertSizeToString(size));
    return t;
  }
  ConvertUInt32ToString((UInt32)_model->rowCount(), num);
  return QString::fromWCharArray(num) + QLatin1String(" ") + tr("object(s)");
}

// ---------- keys (PanelKey.cpp) ----------

bool Panel::eventFilter(QObject *obj, QEvent *ev)
{
  if (ev->type() != QEvent::KeyPress || (obj != _details && obj != _list))
    return QWidget::eventFilter(obj, ev);
  auto *ke = static_cast<QKeyEvent *>(ev);
  const Qt::KeyboardModifiers mods = ke->modifiers() & ~Qt::KeypadModifier;
  const bool ctrl = mods & Qt::ControlModifier;
  const bool alt = mods & Qt::AltModifier;
  const bool shift = mods & Qt::ShiftModifier;
  const bool keypad = ke->modifiers() & Qt::KeypadModifier;
  switch (ke->key())
  {
    case Qt::Key_Return:
    case Qt::Key_Enter:
      if (alt)
        Properties();
      else if (shift)
        OpenFocusedItemOutside();
      else
        OpenSelectedItems(true);
      return true;
    case Qt::Key_Backspace:
      OpenParentFolder();
      return true;
    case Qt::Key_F3:
      EditItem(false);
      return true;
    case Qt::Key_F4:
      EditItem(true);
      return true;
    case Qt::Key_F5:
      CopyToDialog(false);
      return true;
    case Qt::Key_PageDown:
      if (ctrl)
      {
        OpenFocusedItemAsInternal();
        return true;
      }
      break;
    case Qt::Key_PageUp:
      if (ctrl)
      {
        OpenParentFolder();
        return true;
      }
      break;
    case Qt::Key_Backslash:
    case Qt::Key_Slash:
      if (ctrl)
      {
        OpenRootFolder();
        return true;
      }
      break;
    case Qt::Key_Plus:
      if (keypad) { SelectByMask(true); return true; }
      break;
    case Qt::Key_Minus:
      if (keypad) { SelectByMask(false); return true; }
      break;
    case Qt::Key_Asterisk:
      if (keypad) { InvertSelection(); return true; }
      break;
    case Qt::Key_Insert:
    {
      const QModelIndex cur = _selection->currentIndex();
      if (cur.isValid())
      {
        _selection->select(cur, QItemSelectionModel::Toggle | QItemSelectionModel::Rows);
        const QModelIndex next = _model->index(cur.row() + 1, 0);
        if (next.isValid())
          _selection->setCurrentIndex(next, QItemSelectionModel::NoUpdate);
      }
      return true;
    }
  }
  return QWidget::eventFilter(obj, ev);
}

// ---------- messages ----------

void Panel::MessageBox_Error(const UString &message)
{
  QMessageBox::critical(this, QStringLiteral("7-Zip"), ToQ(message));
}

void Panel::MessageBox_Error_HRESULT(HRESULT res)
{
  MessageBox_Error(HResultToMessage(res));
}

// PanelCopy.cpp -- Panel::CopyTo (upstream FileManager/PanelCopy.cpp),
// Copy To / Move To (upstream CApp::OnCopy), Extract / Test
// (upstream CPanel::ExtractArchives / TestArchives).

#include "Panel.h"

#include "Common/IntToString.h"
#include "Common/StringConvert.h"
#include "Windows/FileDir.h"
#include "Windows/FileFind.h"
#include "Windows/FileName.h"
#include "Windows/PropVariant.h"
#include "Windows/PropVariantConv.h"

#include "7zip/PropID.h"
#include "7zip/UI/Common/ExtractingFilePath.h"
#include "7zip/UI/Common/ZipRegistry.h"
#include "7zip/UI/FileManager/FormatUtils.h"
#include "7zip/UI/FileManager/LangUtils.h"
#include "7zip/UI/FileManager/OverwriteDialogRes.h"
#include "7zip/UI/FileManager/PropertyNameRes.h"
#include "7zip/UI/FileManager/ViewSettings.h"
#include "7zip/UI/FileManager/resource.h"
#include "7zip/UI/FileManager/resourceGui.h"

#include "7zip/UI/Common/ArchiveName.h"
#include "CompressCall.h"
#include "FsFolder.h"
#include "CopyDialog.h"
#include "ExtractCallback.h"
#include "HashGUI.h"
#include "ListFormat.h"
#include "Messages.h"
#include "QtConv.h"

#include <QDir>
#include <QMessageBox>

using namespace NWindows;
using namespace NFile;
using namespace NDir;
using namespace NName;

// ---------- CPanelCopyThread (copied) ----------

class CPanelCopyThread: public CProgressThreadVirt
{
  bool ResultsWereShown;
  bool NeedShowRes;

  HRESULT ProcessVirt() Z7_override;
  virtual void ProcessWasFinished_GuiVirt() Z7_override;
public:
  const CCopyToOptions *options;
  const UStringVector *CopyFrom_Paths;
  CMyComPtr<IFolderOperations> FolderOperations;
  CRecordVector<UInt32> Indices;
  CExtractCallbackImp *ExtractCallbackSpec;
  CMyComPtr<IFolderOperationsExtractCallback> ExtractCallback;

  CHashBundle Hash;

  void ShowFinalResults();

  CPanelCopyThread():
    ResultsWereShown(false),
    NeedShowRes(false),
    CopyFrom_Paths(NULL)
    {}
};

void CPanelCopyThread::ShowFinalResults()
{
  if (NeedShowRes)
  if (!ResultsWereShown)
  {
    ResultsWereShown = true;
    ShowHashResults(Hash, NULL);
  }
}

void CPanelCopyThread::ProcessWasFinished_GuiVirt()
{
  ShowFinalResults();
}

HRESULT CPanelCopyThread::ProcessVirt()
{
  HRESULT result2;

  if (CopyFrom_Paths)
  {
    result2 = NFsFolder::CopyFileSystemItems(
        *CopyFrom_Paths,
        us2fs(options->folder),
        options->moveMode,
        (IFolderOperationsExtractCallback *)ExtractCallbackSpec);
  }
  else if (options->testMode)
  {
    CMyComPtr<IArchiveFolder> archiveFolder;
    FolderOperations.QueryInterface(IID_IArchiveFolder, &archiveFolder);
    if (!archiveFolder)
      return E_NOTIMPL;
    CMyComPtr<IFolderArchiveExtractCallback> extractCallback2;
    RINOK(ExtractCallback.QueryInterface(IID_IFolderArchiveExtractCallback, &extractCallback2))
    NExtract::NPathMode::EEnum pathMode =
        NExtract::NPathMode::kCurPaths;
    result2 = archiveFolder->Extract(Indices.ConstData(), Indices.Size(),
        BoolToInt(options->includeAltStreams),
        BoolToInt(options->replaceAltStreamChars),
        pathMode, NExtract::NOverwriteMode::kAsk,
        options->folder, BoolToInt(true), extractCallback2);
  }
  else
    result2 = FolderOperations->CopyTo(
      BoolToInt(options->moveMode),
      Indices.ConstData(), Indices.Size(),
      BoolToInt(options->includeAltStreams),
      BoolToInt(options->replaceAltStreamChars),
      options->folder, ExtractCallback);

  if (result2 == S_OK && !ExtractCallbackSpec->ThereAreMessageErrors)
  {
    if (!options->hashMethods.IsEmpty())
      NeedShowRes = true;
    else if (options->testMode)
    {
      CProgressMessageBoxPair &pair = GetMessagePair(false);
      AddHashBundleRes(pair.Message, Hash);
    }
  }

  return result2;
}

HRESULT Panel::CopyTo(CCopyToOptions &options,
    const CRecordVector<UInt32> &indices,
    UStringVector *messages,
    bool &usePassword, UString &password,
    const UStringVector *filePaths)
{
  if (!filePaths)
  if (!_folderOperations)
  {
    const UString errorMessage = LangString(IDS_OPERATION_IS_NOT_SUPPORTED);
    if (options.showErrorMessages)
      MessageBox_Error(errorMessage);
    else if (messages)
      messages->Add(errorMessage);
    return E_FAIL;
  }

  HRESULT res = S_OK;

  {
  CPanelCopyThread extracter;

  extracter.ExtractCallbackSpec = new CExtractCallbackImp;
  extracter.ExtractCallback = extracter.ExtractCallbackSpec;
  extracter.ExtractCallbackSpec->Src_Is_IO_FS_Folder = IsFSFolder();
  extracter.options = &options;
  extracter.ExtractCallbackSpec->ProgressDialog = &extracter;
  extracter.CompressingMode = false;

  extracter.ExtractCallbackSpec->StreamMode = options.streamMode;

  if (indices.Size() == 1)
  {
    extracter.Hash.FirstFileName = GetItemRelPath(indices[0]);
    extracter.Hash.MainName = extracter.Hash.FirstFileName;
  }

  if (options.VirtFileSystemSpec)
  {
    extracter.ExtractCallbackSpec->VirtFileSystem = options.VirtFileSystemSpec;
    extracter.ExtractCallbackSpec->VirtFileSystemSpec = options.VirtFileSystemSpec;
  }
  extracter.ExtractCallbackSpec->ProcessAltStreams = options.includeAltStreams;

  if (!options.hashMethods.IsEmpty())
  {
    extracter.Hash.SetMethods(EXTERNAL_CODECS_VARS_G options.hashMethods);
    extracter.ExtractCallbackSpec->SetHashMethods(&extracter.Hash);
  }
  else if (options.testMode)
  {
    extracter.ExtractCallbackSpec->SetHashCalc(&extracter.Hash);
  }

  UString title;
  {
    UInt32 titleID = IDS_COPYING;
    if (options.moveMode)
      titleID = IDS_MOVING;
    else if (!options.hashMethods.IsEmpty() && options.streamMode)
    {
      titleID = IDS_CHECKSUM_CALCULATING;
      if (options.hashMethods.Size() == 1)
      {
        const UString &s = options.hashMethods[0];
        if (!s.IsEqualTo("*"))
          title = s;
      }
    }
    else if (options.testMode)
      titleID = IDS_PROGRESS_TESTING;

    if (title.IsEmpty())
      title = LangString(titleID);
  }

  extracter.MainTitle = "7-Zip";
  extracter.MainAddTitle = title + L' ';

  extracter.ExtractCallbackSpec->OverwriteMode = NExtract::NOverwriteMode::kAsk;
  extracter.ExtractCallbackSpec->Init();

  extracter.CopyFrom_Paths = filePaths;
  if (!filePaths)
  {
    extracter.Indices = indices;
    extracter.FolderOperations = _folderOperations;
  }

  extracter.ExtractCallbackSpec->PasswordIsDefined = usePassword;
  extracter.ExtractCallbackSpec->Password = password;

  RINOK(extracter.Create(title, NULL))

  if (messages)
    *messages = extracter.Sync.Messages;

  res = extracter.Result;

  if (res == S_OK && extracter.ExtractCallbackSpec->IsOK())
  {
    usePassword = extracter.ExtractCallbackSpec->PasswordIsDefined;
    password = extracter.ExtractCallbackSpec->Password;
  }

  extracter.ShowFinalResults();
  }

  return res;
}

// ---------- Copy To / Move To (upstream CApp::OnCopy, one panel) ----------

static void AddSizeValue2(UString &s, UInt64 size)
{
  s += MyFormatNew(IDS_FILE_SIZE, ConvertSizeToString(size));
}

static void AddValuePair1(UString &s, UINT resourceID, UInt64 size)
{
  AddLangString(s, resourceID);
  s += ": ";
  AddSizeValue2(s, size);
  s.Add_LF();
}

static void AddValuePair2(UString &s, UINT resourceID, UInt64 num, UInt64 size)
{
  if (num == 0)
    return;
  AddLangString(s, resourceID);
  s += ": ";
  s += ConvertSizeToString(num);
  if (size != (UInt64)(Int64)-1)
  {
    s += "    ( ";
    AddSizeValue2(s, size);
    s += " )";
  }
  s.Add_LF();
}

static void AddPropValueToSum(IFolderFolder *folder, UInt32 index, PROPID propID, UInt64 &sum)
{
  if (sum == (UInt64)(Int64)-1)
    return;
  NCOM::CPropVariant prop;
  folder->GetProperty(index, propID, &prop);
  UInt64 val = 0;
  if (ConvertPropVariantToUInt64(prop, val))
    sum += val;
  else
    sum = (UInt64)(Int64)-1;
}

UString Panel::GetItemsInfoString(const CRecordVector<UInt32> &indices)
{
  UString info;
  UInt64 numDirs, numFiles, filesSize, foldersSize;
  numDirs = numFiles = filesSize = foldersSize = 0;

  unsigned i;
  for (i = 0; i < indices.Size(); i++)
  {
    const UInt32 index = indices[i];
    if (IsItem_Folder(index))
    {
      AddPropValueToSum(_folder, index, kpidSize, foldersSize);
      numDirs++;
    }
    else
    {
      AddPropValueToSum(_folder, index, kpidSize, filesSize);
      numFiles++;
    }
  }

  AddValuePair2(info, IDS_PROP_FOLDERS, numDirs, foldersSize);
  AddValuePair2(info, IDS_PROP_FILES, numFiles, filesSize);
  int numDefined = ((foldersSize != (UInt64)(Int64)-1) && foldersSize != 0) ? 1: 0;
  numDefined += ((filesSize != (UInt64)(Int64)-1) && filesSize != 0) ? 1: 0;
  if (numDefined == 2)
    AddValuePair1(info, IDS_PROP_SIZE, filesSize + foldersSize);

  info.Add_LF();
  info += _currentFolderPrefix;

  for (i = 0; i < indices.Size() && i < 10; i++)
  {
    info.Add_LF();
    info += "  ";
    const UInt32 index = indices[i];
    info += GetItemRelPath(index);
    if (IsItem_Folder(index))
      info.Add_PathSepar();
  }
  if (i != indices.Size())
  {
    info.Add_LF();
    info += "  ...";
  }
  return info;
}

static void Reduce_Path_To_RealFileSystemPath(UString &path)
{
  const unsigned prefixSize = NName::GetRootPrefixSize(path);

  while (!path.IsEmpty())
  {
    if (NFind::DoesDirExist_FollowLink(us2fs(path)))
    {
      NName::NormalizeDirPathPrefix(path);
      break;
    }
    int pos = path.ReverseFind_PathSepar();
    if (pos < 0)
    {
      path.Empty();
      break;
    }
    path.DeleteFrom((unsigned)(pos + 1));
    if ((unsigned)pos + 1 == prefixSize)
      break;
    path.DeleteFrom((unsigned)pos);
  }
}

void Panel::CopyToDialog(bool move)
{
  if (!_folderOperations)
  {
    MessageBox_Error(LangString(IDS_OPERATION_IS_NOT_SUPPORTED));
    return;
  }

  CRecordVector<UInt32> indices;
  Get_ItemIndices_Operated(indices);
  if (indices.Size() == 0)
    return;

  UString destPath = GetFsPath();
  Reduce_Path_To_RealFileSystemPath(destPath);

  UStringVector copyFolders;
  ReadCopyHistory(copyFolders);

  {
    CCopyDialog copyDialog;
    copyDialog.Strings = copyFolders;
    copyDialog.Value = destPath;
    LangString(move ? IDS_MOVE : IDS_COPY, copyDialog.Title);
    LangString(move ? IDS_MOVE_TO : IDS_COPY_TO, copyDialog.Static);
    copyDialog.Info = GetItemsInfoString(indices);
    if (!RunCopyDialog(this, copyDialog))
      return;
    destPath = copyDialog.Value;
  }

  if (destPath.IsEmpty())
  {
    MessageBox_Error(LangString(IDS_OPERATION_IS_NOT_SUPPORTED));
    return;
  }

  if (destPath.IsPrefixedBy(L"~/"))
  {
    destPath.Delete(0);
    destPath.Insert(0, ToU(QDir::homePath()));
  }
  if (!NName::IsAbsolutePath(destPath))
    destPath.Insert(0, GetFsPath());

  if (!_parentFolders.isEmpty() || !IsFSFolder())
  {
    // copying out of an archive: destination is a file system folder
    if (indices.Size() == 1 && !destPath.IsEmpty() && !IS_PATH_SEPAR(destPath.Back()))
    {
      const int pos = destPath.ReverseFind_PathSepar();
      const UString prefix = destPath.Left((unsigned)(pos + 1));
      if (!CreateComplexDir(us2fs(prefix)))
      {
        MessageBox_Error(prefix + L"\n" + HResultToMessage(GetLastError_noZero_HRESULT()));
        return;
      }
    }
    else
    {
      NName::NormalizeDirPathPrefix(destPath);
      if (!CreateComplexDir(us2fs(destPath)))
      {
        MessageBox_Error(destPath + L"\n" + HResultToMessage(GetLastError_noZero_HRESULT()));
        return;
      }
    }
  }
  else
  {
    // file system to file system (upstream: destIsFsPath)
    if (CompareFileNames(destPath, GetFsPath()) == 0)
    {
      MessageBox_Error(UString(L"Cannot copy files onto itself"));
      return;
    }
    if (indices.Size() != 1 || destPath.IsEmpty() || IS_PATH_SEPAR(destPath.Back()))
    {
      NName::NormalizeDirPathPrefix(destPath);
      if (!CreateComplexDir(us2fs(destPath)))
      {
        MessageBox_Error(destPath + L"\n" + HResultToMessage(GetLastError_noZero_HRESULT()));
        return;
      }
    }
  }

  AddUniqueStringToHeadOfList(copyFolders, destPath);
  while (copyFolders.Size() > 20)
    copyFolders.DeleteBack();
  SaveCopyHistory(copyFolders);

  CCopyToOptions options;
  options.folder = destPath;
  options.moveMode = move;
  options.includeAltStreams = true;
  options.replaceAltStreamChars = false;
  options.showErrorMessages = true;

  bool usePassword = false;
  UString password;
  if (!_parentFolders.isEmpty())
  {
    usePassword = _parentFolders.back().UsePassword;
    password = _parentFolders.back().Password;
  }
  const HRESULT result = CopyTo(options, indices, NULL, usePassword, password);
  if (!_parentFolders.isEmpty() && result == S_OK)
  {
    _parentFolders.back().UsePassword = usePassword;
    _parentFolders.back().Password = password;
  }
  if (result != S_OK && result != E_ABORT && result != E_FAIL)
    MessageBox_Error_HRESULT(result);
  RefreshListCtrl_SaveFocused();
}

// ---------- Extract / Test ----------

static const char * const kArcExts[] =
{
    "7z"
  , "bz2"
  , "gz"
  , "rar"
  , "zip"
};

static bool IsItArcExt(const UString &ext)
{
  for (unsigned i = 0; i < Z7_ARRAY_SIZE(kArcExts); i++)
    if (ext.IsEqualTo_Ascii_NoCase(kArcExts[i]))
      return true;
  return false;
}

// upstream Explorer/ContextMenu.cpp
static UString GetSubFolderNameForExtract(const UString &arcName)
{
  int dotPos = arcName.ReverseFind_Dot();
  if (dotPos < 0)
    return Get_Correct_FsFile_Name(arcName) + L'~';

  const UString ext = arcName.Ptr((unsigned)dotPos + 1);
  UString res = arcName.Left((unsigned)dotPos);
  res.TrimRight();
  dotPos = res.ReverseFind_Dot();
  if (dotPos > 0)
  {
    const UString ext2 = res.Ptr((unsigned)dotPos + 1);
    if ((ext.IsEqualTo_Ascii_NoCase("001") && IsItArcExt(ext2))
        || (ext.IsEqualTo_Ascii_NoCase("rar") &&
          (  ext2.IsEqualTo_Ascii_NoCase("part001")
          || ext2.IsEqualTo_Ascii_NoCase("part01")
          || ext2.IsEqualTo_Ascii_NoCase("part1"))))
      res.DeleteFrom((unsigned)dotPos);
    res.TrimRight();
  }
  return Get_Correct_FsFile_Name(res);
}

static UString GetSubFolderNameForExtract2(const UString &arcPath)
{
  int slashPos = arcPath.ReverseFind_PathSepar();
  UString s;
  UString name = arcPath;
  if (slashPos >= 0)
  {
    s = arcPath.Left((unsigned)(slashPos + 1));
    name = arcPath.Ptr((unsigned)(slashPos + 1));
  }
  s += GetSubFolderNameForExtract(name);
  return s;
}

void Panel::GetFilePaths(const CRecordVector<UInt32> &indices, UStringVector &paths, bool allowFolders) const
{
  paths.ClearAndReserve(indices.Size());
  UString prefix = GetFsPath();
  FOR_VECTOR (i, indices)
  {
    const UInt32 index = indices[i];
    if (!allowFolders && IsItem_Folder(index))
    {
      paths.Clear();
      break;
    }
    paths.AddInReserved(prefix + GetItemRelPath(index));
  }
}

void Panel::ExtractArchives()
{
  if (!_parentFolders.isEmpty())
  {
    CopyToDialog(false);
    return;
  }
  CRecordVector<UInt32> indices;
  Get_ItemIndices_Operated(indices);
  UStringVector paths;
  GetFilePaths(indices, paths);
  if (indices.IsEmpty() || paths.IsEmpty() || !IsFSFolder())
  {
    MessageBox_Error(LangString(IDS_SELECT_FILES));
    return;
  }
  UString outFolder = GetFsPath();
  if (indices.Size() == 1)
    outFolder += GetSubFolderNameForExtract2(GetItemRelPath(indices[0]));
  else
    outFolder.Add_Char('*');
  outFolder.Add_PathSepar();

  ::ExtractArchives(paths, outFolder
      , true   // showDialog
      , false  // elimDup
      , (UInt32)(Int32)-1
      );
  RefreshListCtrl_SaveFocused();
}

// upstream CPanel::AddToArchive
void Panel::AddToArchive()
{
  if (!_parentFolders.isEmpty() || !IsFSFolder())
  {
    MessageBox_Error(LangString(IDS_OPERATION_IS_NOT_SUPPORTED));
    return;
  }
  CRecordVector<UInt32> indices;
  Get_ItemIndices_Operated(indices);
  if (indices.Size() == 0)
  {
    MessageBox_Error(LangString(IDS_SELECT_FILES));
    return;
  }
  const UString destCurDirPrefix = GetFsPath();
  UStringVector names;
  GetFilePaths(indices, names, true);
  UString baseName;
  const UString arcName = CreateArchiveName(names,
      false, // isHash
      NULL,  // CFileInfo *fi
      baseName);
  CompressFiles(destCurDirPrefix, arcName, L"",
      true,   // addExtension
      names,
      false,  // email
      true,   // showDialog
      false); // waitFinish
  RefreshListCtrl_SaveFocused();
}

void Panel::TestArchives()
{
  CRecordVector<UInt32> indices;
  Get_ItemIndices_Operated(indices);
  CMyComPtr<IArchiveFolder> archiveFolder;
  _folder.QueryInterface(IID_IArchiveFolder, &archiveFolder);
  if (archiveFolder)
  {
    CCopyToOptions options;
    options.streamMode = true;
    options.showErrorMessages = true;
    options.testMode = true;

    UStringVector messages;
    bool usePassword = false;
    UString password;
    if (!_parentFolders.isEmpty())
    {
      usePassword = _parentFolders.back().UsePassword;
      password = _parentFolders.back().Password;
    }
    HRESULT res = CopyTo(options, indices, &messages, usePassword, password);
    if (res != S_OK)
    {
      if (res != E_ABORT && messages.IsEmpty())
        MessageBox_Error_HRESULT(res);
    }
    return;
  }

  if (!IsFSFolder())
  {
    MessageBox_Error(LangString(IDS_OPERATION_IS_NOT_SUPPORTED));
    return;
  }
  UStringVector paths;
  GetFilePaths(indices, paths);
  if (paths.IsEmpty())
  {
    MessageBox_Error(LangString(IDS_SELECT_FILES));
    return;
  }
  ::TestArchives(paths);
}

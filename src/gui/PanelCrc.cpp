// PanelCrc.cpp -- upstream FileManager/PanelCrc.cpp (CDirEnumerator, CThreadCrc);
// CApp::CalculateCrc2 takes the panel data as arguments. Compiled on Q32.


#include "Common/MyException.h"

#include "Windows/FileFind.h"
#include "Windows/FileIO.h"
#include "Windows/FileName.h"
#include "C/Alloc.h"

#include "7zip/UI/Common/LoadCodecs.h"

#include "HashGUI.h"

#include "FmProgress.h"
#include "7zip/UI/FileManager/LangUtils.h"

#include "7zip/UI/FileManager/resource.h"

using namespace NWindows;
using namespace NFile;

// 7zfm-qt: this file is only built for Windows upstream; POSIX NIO /
// NFind APIs differ slightly.
static bool EnumNext(NFind::CEnumerator &e, NFind::CFileInfo &fi, bool &found)
{
  NFind::CDirEntry de;
  if (!e.Next(de, found))
    return false;
  if (!found)
    return true;
  return e.Fill_FileInfo(de, fi, false);
}

static bool File_Read(NIO::CInFile &f, void *data, UInt32 size, UInt32 &processed)
{
  size_t p = 0;
  const bool res = f.ReadFull(data, size, p);
  processed = (UInt32)p;
  return res;
}

// from upstream FileManager/Panel.h
class CMyBuffer
{
  void *_data;
public:
  CMyBuffer(): _data(NULL) {}
  operator void *() { return _data; }
  bool Allocate(size_t size)
  {
    if (_data)
      return false;
    _data = ::MidAlloc(size);
    return _data != NULL;
  }
  ~CMyBuffer() { ::MidFree(_data); }
};

#ifdef Z7_EXTERNAL_CODECS
extern CExternalCodecs g_ExternalCodecs;
HRESULT LoadGlobalCodecs();
#endif

static const UInt32 kBufSize = (1 << 15);

struct CDirEnumerator
{
  bool EnterToDirs;
  FString BasePrefix;
  FString BasePrefix_for_Open;
  FStringVector FilePaths;

  CObjectVector<NFind::CEnumerator> Enumerators;
  FStringVector Prefixes;
  unsigned Index;

  CDirEnumerator(): EnterToDirs(false), Index(0) {}

  void Init();
  DWORD GetNextFile(NFind::CFileInfo &fi, bool &filled, FString &resPath);
};

void CDirEnumerator::Init()
{
  Enumerators.Clear();
  Prefixes.Clear();
  Index = 0;
}

static DWORD GetNormalizedError()
{
  const DWORD error = GetLastError();
  return (error == 0) ? (DWORD)E_FAIL : error;
}

DWORD CDirEnumerator::GetNextFile(NFind::CFileInfo &fi, bool &filled, FString &resPath)
{
  filled = false;
  resPath.Empty();
  
  for (;;)
  {
    #if defined(_WIN32) && !defined(UNDER_CE)
    bool isRootPrefix = (BasePrefix.IsEmpty() || (NName::IsSuperPath(BasePrefix) && BasePrefix[NName::kSuperPathPrefixSize] == 0));
    #endif

    if (Enumerators.IsEmpty())
    {
      if (Index >= FilePaths.Size())
        return S_OK;
      const FString &path = FilePaths[Index++];
      const int pos = path.ReverseFind_PathSepar();
      if (pos >= 0)
        resPath.SetFrom(path, (unsigned)pos + 1);

      #if defined(_WIN32) && !defined(UNDER_CE)
      if (isRootPrefix && path.Len() == 2 && NName::IsDrivePath2(path))
      {
        // we use "c:" item as directory item
        fi.ClearBase();
        fi.Name = path;
        fi.SetAsDir();
        fi.Size = 0;
      }
      else
      #endif
      if (!fi.Find(BasePrefix + path))
      {
        const DWORD error = GetNormalizedError();
        resPath = path;
        return error;
      }
    
      break;
    }
    
    bool found;
    
    if (EnumNext(Enumerators.Back(), fi, found)) // 7zfm-qt: POSIX enumerator
    {
      if (found)
      {
        resPath = Prefixes.Back();
        break;
      }
    }
    else
    {
      const DWORD error = GetNormalizedError();
      resPath = Prefixes.Back();
      Enumerators.DeleteBack();
      Prefixes.DeleteBack();
      return error;
    }
    
    Enumerators.DeleteBack();
    Prefixes.DeleteBack();
  }
  
  resPath += fi.Name;
  
  if (EnterToDirs && fi.IsDir())
  {
    FString s = resPath;
    s.Add_PathSepar();
    Prefixes.Add(s);
    Enumerators.AddNew().SetDirPrefix(BasePrefix + s);
  }
  
  filled = true;
  return S_OK;
}



class CThreadCrc: public CProgressThreadVirt
{
  bool ResultsWereShown;
  bool WasFinished;

  HRESULT ProcessVirt() Z7_override;
  virtual void ProcessWasFinished_GuiVirt() Z7_override;
public:
  CDirEnumerator Enumerator;
  CHashBundle Hash;
  // FString FirstFilePath;

  void SetStatus(const UString &s);
  void AddErrorMessage(DWORD systemError, const FChar *name);
  void ShowFinalResults(HWND hwnd);

  CThreadCrc():
    ResultsWereShown(false),
    WasFinished(false)
    {}
};

void CThreadCrc::ShowFinalResults(HWND hwnd)
{
  if (WasFinished)
  if (!ResultsWereShown)
  {
    ResultsWereShown = true;
    ShowHashResults(Hash, hwnd);
  }
}

void CThreadCrc::ProcessWasFinished_GuiVirt()
{
  ShowFinalResults(*this);
}

void CThreadCrc::AddErrorMessage(DWORD systemError, const FChar *name)
{
  Sync.AddError_Code_Name(HRESULT_FROM_WIN32(systemError), fs2us(Enumerator.BasePrefix + name));
  Hash.NumErrors++;
}

void CThreadCrc::SetStatus(const UString &s2)
{
  UString s = s2;
  if (!Enumerator.BasePrefix.IsEmpty())
  {
    s.Add_Space_if_NotEmpty();
    s += fs2us(Enumerator.BasePrefix);
  }
  Sync.Set_Status(s);
}

HRESULT CThreadCrc::ProcessVirt()
{
  // Hash.Init();
  
  CMyBuffer buf;
  if (!buf.Allocate(kBufSize))
    return E_OUTOFMEMORY;

  CProgressSync &sync = Sync;
  
  SetStatus(LangString(IDS_SCANNING));

  Enumerator.Init();

  FString path;
  NFind::CFileInfo fi;
  UInt64 numFiles = 0;
  UInt64 numItems = 0, numItems_Prev = 0;
  UInt64 totalSize = 0;

  for (;;)
  {
    bool filled;
    const DWORD error = Enumerator.GetNextFile(fi, filled, path);
    if (error != 0)
    {
      AddErrorMessage(error, path);
      continue;
    }
    if (!filled)
      break;
    if (!fi.IsDir())
    {
      totalSize += fi.Size;
      numFiles++;
    }
    numItems++;
    bool needPrint = false;
    // if (fi.IsDir())
    {
      if (numItems - numItems_Prev >= 100)
      {
        needPrint = true;
        numItems_Prev = numItems;
      }
    }
    /*
    else if (numFiles - numFiles_Prev >= 200)
    {
      needPrint = true;
      numFiles_Prev = numFiles;
    }
    */
    if (needPrint)
    {
      RINOK(sync.ScanProgress(numFiles, totalSize, path, fi.IsDir()))
    }
  }
  RINOK(sync.ScanProgress(numFiles, totalSize, FString(), false))
  // sync.SetNumFilesTotal(numFiles);
  // sync.SetProgress(totalSize, 0);
  // SetStatus(LangString(IDS_CHECKSUM_CALCULATING));
  // sync.SetCurFilePath(L"");
  SetStatus(L"");
 
  Enumerator.Init();

  FString tempPath;
  bool isFirstFile = true;
  UInt64 errorsFilesSize = 0;

  for (;;)
  {
    bool filled;
    DWORD error = Enumerator.GetNextFile(fi, filled, path);
    if (error != 0)
    {
      AddErrorMessage(error, path);
      continue;
    }
    if (!filled)
      break;
    
    error = 0;
    Hash.InitForNewFile();
    if (!fi.IsDir())
    {
      NIO::CInFile inFile;
      tempPath = Enumerator.BasePrefix_for_Open;
      tempPath += path;
      if (!inFile.Open(tempPath))
      {
        error = GetNormalizedError();
        AddErrorMessage(error, path);
        continue;
      }
      if (isFirstFile)
      {
        Hash.FirstFileName = fs2us(path);
        isFirstFile = false;
      }
      sync.Set_FilePath(fs2us(path));
      sync.Set_NumFilesCur(Hash.NumFiles);
      UInt64 progress_Prev = 0;
      for (;;)
      {
        UInt32 size;
        if (!File_Read(inFile, buf, kBufSize, size)) // 7zfm-qt
        {
          error = GetNormalizedError();
          AddErrorMessage(error, path);
          UInt64 errorSize = 0;
          if (inFile.GetLength(errorSize))
            errorsFilesSize += errorSize;
          break;
        }
        if (size == 0)
          break;
        Hash.Update(buf, size);
        if (Hash.CurSize - progress_Prev >= ((UInt32)1 << 21))
        {
          RINOK(sync.Set_NumBytesCur(errorsFilesSize + Hash.FilesSize + Hash.CurSize))
          progress_Prev = Hash.CurSize;
        }
      }
    }
    if (error == 0)
      Hash.Final(fi.IsDir(), false, fs2us(path));
    RINOK(sync.Set_NumBytesCur(errorsFilesSize + Hash.FilesSize))
  }
  RINOK(sync.Set_NumBytesCur(errorsFilesSize + Hash.FilesSize))
  sync.Set_NumFilesCur(Hash.NumFiles);
  if (Hash.NumFiles != 1)
    sync.Set_FilePath(L"");
  SetStatus(L"");

  CProgressMessageBoxPair &pair = GetMessagePair(Hash.NumErrors != 0);
  WasFinished = true;
  LangString(IDS_CHECKSUM_INFORMATION, pair.Title);
  return S_OK;
}




// upstream CApp::CalculateCrc2, file-system branch
HRESULT CalculateCrcFs(const UString &methodName, const UString &basePrefix, const UStringVector &relPaths);
HRESULT CalculateCrcFs(const UString &methodName, const UString &basePrefix, const UStringVector &relPaths)
{
  if (relPaths.IsEmpty())
    return S_OK;
  {
    CThreadCrc t;

    {
      UStringVector methods;
      methods.Add(methodName);
      RINOK(t.Hash.SetMethods(EXTERNAL_CODECS_VARS_G methods))
    }

    FOR_VECTOR (i, relPaths)
      t.Enumerator.FilePaths.Add(us2fs(relPaths[i]));

    if (t.Enumerator.FilePaths.Size() == 1)
      t.Hash.MainName = fs2us(t.Enumerator.FilePaths[0]);

    t.Enumerator.BasePrefix = us2fs(basePrefix);
    t.Enumerator.BasePrefix_for_Open = us2fs(basePrefix);

    t.Enumerator.EnterToDirs = true;

    t.ShowCompressionInfo = false;

    const UString title = LangString(IDS_CHECKSUM_CALCULATING);

    t.MainTitle = "7-Zip";
    t.MainAddTitle = title;
    t.MainAddTitle.Add_Space();

    RINOK(t.Create(title, NULL))

    t.ShowFinalResults(NULL);
  }
  return S_OK;
}

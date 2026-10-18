// core_smoke: exercises the engine the way 7zFM's Open Inside does.
//
//   core_smoke formats
//   core_smoke ls  <file> [item/inside ...]   list the folder reached
//   core_smoke cat <file> item/inside ...     write the item's bytes to stdout
//   core_smoke x   <file> <outdir>            extract everything through the
//                                             Agent (IArchiveFolder::Extract)
//
// CORE_SMOKE_PASSWORD, if set, answers password requests while opening
// (encrypted headers) and while extracting. The top-level file is opened
// with upstream's COpenCallbackImp, so multi-volume archives work.
//
// Each path component is looked up in the current folder. A directory is
// entered with BindToFolder. Any other item is opened as a nested archive
// directly from the parent's IInArchiveGetStream stream, never extracted;
// if no seekable stream is available the command fails.

#include "Common/Common.h"
#include "Common/MyCom.h"
#include "Common/StringConvert.h"
#include "Common/Wildcard.h"
#include "Windows/PropVariant.h"
#include "Windows/FileDir.h"
#include "Windows/FileName.h"
#include "Windows/PropVariantConv.h"

#include "7zip/UI/Common/ArchiveOpenCallback.h"

#include "Agent.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *Password() { return getenv("CORE_SMOKE_PASSWORD"); }

static HRESULT PasswordToBstr(BSTR *password)
{
  const char *pw = Password();
  if (!pw)
    return E_ABORT;
  return StringToBstr(GetUnicodeString(pw), password);
}

class COpenUI Z7_final: public IOpenCallbackUI
{
  Z7_IFACE_IMP(IOpenCallbackUI)
};

HRESULT COpenUI::Open_CheckBreak() { return S_OK; }
HRESULT COpenUI::Open_SetTotal(const UInt64 *, const UInt64 *) { return S_OK; }
HRESULT COpenUI::Open_SetCompleted(const UInt64 *, const UInt64 *) { return S_OK; }
HRESULT COpenUI::Open_Finished() { return S_OK; }
HRESULT COpenUI::Open_CryptoGetTextPassword(BSTR *password) { return PasswordToBstr(password); }

class CExtractCb Z7_final:
  public IFolderArchiveExtractCallback,
  public IFolderArchiveExtractCallback2,
  public ICryptoGetTextPassword,
  public CMyUnknownImp
{
  Z7_COM_UNKNOWN_IMP_3(IFolderArchiveExtractCallback, IFolderArchiveExtractCallback2, ICryptoGetTextPassword)
  Z7_IFACE_COM7_IMP(IProgress)
  Z7_IFACE_COM7_IMP(IFolderArchiveExtractCallback)
  Z7_IFACE_COM7_IMP(IFolderArchiveExtractCallback2)
  Z7_IFACE_COM7_IMP(ICryptoGetTextPassword)
  UString _name;
public:
  unsigned NumFiles = 0;
  unsigned NumErrors = 0;
};

Z7_COM7F_IMF(CExtractCb::SetTotal(UInt64)) { return S_OK; }
Z7_COM7F_IMF(CExtractCb::SetCompleted(const UInt64 *)) { return S_OK; }
Z7_COM7F_IMF(CExtractCb::AskOverwrite(const wchar_t *, const FILETIME *, const UInt64 *,
    const wchar_t *, const FILETIME *, const UInt64 *, Int32 *answer))
{
  *answer = NOverwriteAnswer::kYes;
  return S_OK;
}
Z7_COM7F_IMF(CExtractCb::PrepareOperation(const wchar_t *name, Int32 isFolder, Int32, const UInt64 *))
{
  _name = name;
  if (!isFolder)
    NumFiles++;
  return S_OK;
}
Z7_COM7F_IMF(CExtractCb::MessageError(const wchar_t *message))
{
  fprintf(stderr, "error: %s\n", (const char *)GetOemString(message));
  NumErrors++;
  return S_OK;
}
Z7_COM7F_IMF(CExtractCb::SetOperationResult(Int32 opRes, Int32 encrypted))
{
  if (opRes != NArchive::NExtract::NOperationResult::kOK)
  {
    fprintf(stderr, "%s: operation result %d%s\n", (const char *)GetOemString(_name),
        (int)opRes, encrypted ? " (encrypted)" : "");
    NumErrors++;
  }
  return S_OK;
}
Z7_COM7F_IMF(CExtractCb::ReportExtractResult(Int32 opRes, Int32 encrypted, const wchar_t *name))
{
  if (opRes != NArchive::NExtract::NOperationResult::kOK)
  {
    fprintf(stderr, "%s: extract result %d%s\n", (const char *)GetOemString(name),
        (int)opRes, encrypted ? " (encrypted)" : "");
    NumErrors++;
  }
  return S_OK;
}
Z7_COM7F_IMF(CExtractCb::CryptoGetTextPassword(BSTR *password)) { return PasswordToBstr(password); }

static UString GetName(IFolderFolder *folder, UInt32 i)
{
  NWindows::NCOM::CPropVariant prop;
  if (folder->GetProperty(i, kpidName, &prop) != S_OK || prop.vt != VT_BSTR)
    return UString();
  return UString(prop.bstrVal);
}

static bool GetBool(IFolderFolder *folder, UInt32 i, PROPID id)
{
  NWindows::NCOM::CPropVariant prop;
  return folder->GetProperty(i, id, &prop) == S_OK && prop.vt == VT_BOOL && prop.boolVal != VARIANT_FALSE;
}

static UInt64 GetSize(IFolderFolder *folder, UInt32 i)
{
  NWindows::NCOM::CPropVariant prop;
  UInt64 v = 0;
  if (folder->GetProperty(i, kpidSize, &prop) == S_OK)
    ConvertPropVariantToUInt64(prop, v);
  return v;
}

static HRESULT OpenArc(IInStream *stream, const UString &path, CMyComPtr<IFolderFolder> &folder,
    IProgress *progress = NULL)
{
  CMyComPtr<IFolderManager> manager = new CArchiveFolderManager;
  folder.Release();
  // arcFormat must be non-NULL: "" = by signature, "*" / "#" as in Open Inside.
  return manager->OpenFolderFile(stream, path, L"", &folder, progress);
}

static bool FindItem(IFolderFolder *folder, const UString &name, UInt32 &index)
{
  UInt32 n = 0;
  folder->GetNumberOfItems(&n);
  for (UInt32 i = 0; i < n; i++)
    if (GetName(folder, i) == name) { index = i; return true; }
  return false;
}

static HRESULT GetItemStream(IFolderFolder *folder, UInt32 index, CMyComPtr<IInStream> &stream)
{
  CMyComPtr<IInArchiveGetStream> getStream;
  folder->QueryInterface(IID_IInArchiveGetStream, (void **)&getStream);
  if (!getStream)
    return E_NOTIMPL;
  CMyComPtr<ISequentialInStream> seq;
  RINOK(getStream->GetStream(index, &seq))
  if (!seq)
    return E_NOTIMPL;
  seq.QueryInterface(IID_IInStream, &stream);
  return stream ? S_OK : E_NOTIMPL;
}

int main(int argc, char **argv)
{
  MY_SetLocale();
  if (LoadGlobalCodecs() != S_OK)
    return 2;
  if (argc >= 2 && strcmp(argv[1], "formats") == 0)
  {
    FOR_VECTOR (i, g_CodecsObj->Formats)
      printf("%s\n", (const char *)GetAnsiString(g_CodecsObj->Formats[i].Name));
    return 0;
  }
  if (argc < 3)
  {
    fprintf(stderr, "usage: core_smoke formats | ls|cat <file> [item ...] | x <file> <outdir>\n");
    return 2;
  }
  const bool cat = strcmp(argv[1], "cat") == 0;
  const bool extract = strcmp(argv[1], "x") == 0;
  if (extract && argc != 4)
    return 2;

  COpenUI openUI;
  COpenCallbackImp *openCbSpec = new COpenCallbackImp;
  CMyComPtr<IArchiveOpenCallback> openCb = openCbSpec;
  openCbSpec->Callback = &openUI;
  {
    FString dirPrefix, fileName;
    if (!NWindows::NFile::NDir::GetFullPathAndSplit(us2fs(GetUnicodeString(argv[2])), dirPrefix, fileName))
      return 1;
    RINOK(openCbSpec->Init2(dirPrefix, fileName))
  }

  CMyComPtr<IFolderFolder> folder;
  HRESULT res = OpenArc(NULL, GetUnicodeString(argv[2]), folder, openCbSpec);
  if (res != S_OK || !folder)
  {
    fprintf(stderr, "cannot open %s as archive (0x%x)\n", argv[2], (unsigned)res);
    return 1;
  }

  if (extract)
  {
    RINOK(folder->LoadItems())
    UInt32 n = 0;
    folder->GetNumberOfItems(&n);
    CRecordVector<UInt32> indices;
    for (UInt32 i = 0; i < n; i++)
      indices.Add(i);
    CMyComPtr<IArchiveFolder> arcFolder;
    folder.QueryInterface(IID_IArchiveFolder, &arcFolder);
    if (!arcFolder)
      return 1;
    FString outDir = us2fs(GetUnicodeString(argv[3]));
    NWindows::NFile::NName::NormalizeDirPathPrefix(outDir);
    CExtractCb *cbSpec = new CExtractCb;
    CMyComPtr<IFolderArchiveExtractCallback> cb = cbSpec;
    res = arcFolder->Extract(indices.ConstData(), indices.Size(), BoolToInt(false), BoolToInt(false),
        NExtract::NPathMode::kFullPaths, NExtract::NOverwriteMode::kOverwrite,
        fs2us(outDir), BoolToInt(false), cb);
    printf("files: %u, errors: %u\n", cbSpec->NumFiles, cbSpec->NumErrors);
    if (res != S_OK)
    {
      fprintf(stderr, "Extract failed (0x%x)\n", (unsigned)res);
      return 1;
    }
    return cbSpec->NumErrors == 0 && cbSpec->NumFiles != 0 ? 0 : 4;
  }

  for (int a = 3; a < argc; a++)
  {
    UStringVector parts;
    SplitPathToParts(GetUnicodeString(argv[a]), parts);
    FOR_VECTOR (p, parts)
    {
      if (parts[p].IsEmpty())
        continue;
      UInt32 index;
      RINOK(folder->LoadItems())
      if (!FindItem(folder, parts[p], index))
      {
        fprintf(stderr, "not found: %s\n", (const char *)GetAnsiString(parts[p]));
        return 1;
      }
      if (GetBool(folder, index, kpidIsDir))
      {
        CMyComPtr<IFolderFolder> sub;
        RINOK(folder->BindToFolder(index, &sub))
        folder = sub;
        continue;
      }
      CMyComPtr<IInStream> stream;
      if (GetItemStream(folder, index, stream) != S_OK)
      {
        fprintf(stderr, "no in-place stream for: %s\n", (const char *)GetAnsiString(parts[p]));
        return 3;
      }
      const bool last = (a == argc - 1 && p == parts.Size() - 1);
      if (cat && last)
      {
        Byte buf[1 << 16];
        for (;;)
        {
          UInt32 got = 0;
          RINOK(stream->Read(buf, sizeof(buf), &got))
          if (got == 0)
            return 0;
          fwrite(buf, 1, got, stdout);
        }
      }
      CMyComPtr<IFolderFolder> sub;
      res = OpenArc(stream, parts[p], sub);
      if (res != S_OK || !sub)
      {
        fprintf(stderr, "cannot open nested %s (0x%x)\n", (const char *)GetAnsiString(parts[p]), (unsigned)res);
        return 1;
      }
      folder = sub;
    }
  }

  RINOK(folder->LoadItems())
  UInt32 n = 0;
  folder->GetNumberOfItems(&n);
  for (UInt32 i = 0; i < n; i++)
    printf("%s\t%llu\t%s\n", GetBool(folder, i, kpidIsDir) ? "D" : "F",
        (unsigned long long)GetSize(folder, i),
        (const char *)GetOemString(GetName(folder, i)));
  return 0;
}

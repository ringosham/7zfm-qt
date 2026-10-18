// core_smoke: exercises the engine the way 7zFM's Open Inside does.
//
//   core_smoke formats
//   core_smoke ls  <file> [item/inside ...]   list the folder reached
//   core_smoke cat <file> item/inside ...     write the item's bytes to stdout
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
#include "Windows/PropVariantConv.h"

#include "Agent.h"

#include <stdio.h>
#include <string.h>

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

static HRESULT OpenArc(IInStream *stream, const UString &path, CMyComPtr<IFolderFolder> &folder)
{
  CMyComPtr<IFolderManager> manager = new CArchiveFolderManager;
  folder.Release();
  // arcFormat must be non-NULL: "" = by signature, "*" / "#" as in Open Inside.
  return manager->OpenFolderFile(stream, path, L"", &folder, NULL);
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
    fprintf(stderr, "usage: core_smoke formats | ls|cat <file> [item ...]\n");
    return 2;
  }
  const bool cat = strcmp(argv[1], "cat") == 0;

  CMyComPtr<IFolderFolder> folder;
  HRESULT res = OpenArc(NULL, GetUnicodeString(argv[2]), folder);
  if (res != S_OK || !folder)
  {
    fprintf(stderr, "cannot open %s as archive (0x%x)\n", argv[2], (unsigned)res);
    return 1;
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

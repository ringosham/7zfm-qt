// WinPathZip.cpp -- 7zfm-qt: zip files made on Windows with '\' in their
// paths ("dir\sub\file.txt"), as older .NET and PowerShell's
// Compress-Archive wrote them. The zip format says '/', and upstream's zip
// reader passes the names on as they are; on Windows '\' is the separator
// anyway, but on Linux each such item would be one file with backslashes in
// its name, both in the folder view and when extracted.
//
// Upstream already turns '\' into '/' for the formats that use it (RAR,
// CAB, ARJ, LZH, NSIS, cpio). For zip, its Windows build does what this
// does: '\' separates folders unless the item was made on Unix, where it is
// a character of the name.
//
// The zip format's CreateInArchive is replaced by one that wraps upstream's
// reader in CWinPathZip, which passes everything through but kpidPath.

#include "WinPathZip.h"

#include "Common/MyCom.h"
#include "Common/MyString.h"

#include "Windows/PropVariant.h"

#include "7zip/Archive/IArchive.h"
#include "7zip/PropID.h"
#include "7zip/UI/Common/LoadCodecs.h"

using namespace NWindows;

namespace {

Func_CreateInArchive g_ZipCreateInArchive;

class CWinPathZip Z7_final: public IInArchive, public CMyUnknownImp
{
  CMyComPtr<IInArchive> _inner;

  // upstream's zip reader gives the Host OS of items read from the central
  // directory; Unix and macOS allow '\' in names
  bool MadeOnUnix(UInt32 index)
  {
    NCOM::CPropVariant prop;
    if (_inner->GetProperty(index, kpidHostOS, &prop) != S_OK || prop.vt != VT_BSTR)
      return false;
    const UString os = prop.bstrVal;
    return os.IsEqualTo_Ascii_NoCase("Unix") || os.IsEqualTo_Ascii_NoCase("OS/X");
  }

public:
  explicit CWinPathZip(IInArchive *inner): _inner(inner) {}

  // IInArchive here; any other interface is the zip reader's own (updating
  // and settings go to it directly)
  STDMETHOD(QueryInterface)(REFGUID iid, void **outObject) throw() Z7_override Z7_final
  {
    *outObject = NULL;
    if (iid == IID_IUnknown || iid == IID_IInArchive)
    {
      IInArchive *p = this;
      *outObject = p;
      ++_m_RefCount;
      return S_OK;
    }
    return _inner->QueryInterface(iid, outObject);
  }
  Z7_COM_ADDREF_RELEASE
  Z7_IFACE_COM7_IMP(IInArchive)
};

Z7_COM7F_IMF(CWinPathZip::Open(IInStream *stream, const UInt64 *maxCheckStartPosition, IArchiveOpenCallback *callback))
  { return _inner->Open(stream, maxCheckStartPosition, callback); }
Z7_COM7F_IMF(CWinPathZip::Close())
  { return _inner->Close(); }
Z7_COM7F_IMF(CWinPathZip::GetNumberOfItems(UInt32 *numItems))
  { return _inner->GetNumberOfItems(numItems); }
Z7_COM7F_IMF(CWinPathZip::Extract(const UInt32 *indices, UInt32 numItems, Int32 testMode, IArchiveExtractCallback *extractCallback))
  { return _inner->Extract(indices, numItems, testMode, extractCallback); }
Z7_COM7F_IMF(CWinPathZip::GetArchiveProperty(PROPID propID, PROPVARIANT *value))
  { return _inner->GetArchiveProperty(propID, value); }
Z7_COM7F_IMF(CWinPathZip::GetNumberOfProperties(UInt32 *numProps))
  { return _inner->GetNumberOfProperties(numProps); }
Z7_COM7F_IMF(CWinPathZip::GetPropertyInfo(UInt32 index, BSTR *name, PROPID *propID, VARTYPE *varType))
  { return _inner->GetPropertyInfo(index, name, propID, varType); }
Z7_COM7F_IMF(CWinPathZip::GetNumberOfArchiveProperties(UInt32 *numProps))
  { return _inner->GetNumberOfArchiveProperties(numProps); }
Z7_COM7F_IMF(CWinPathZip::GetArchivePropertyInfo(UInt32 index, BSTR *name, PROPID *propID, VARTYPE *varType))
  { return _inner->GetArchivePropertyInfo(index, name, propID, varType); }

Z7_COM7F_IMF(CWinPathZip::GetProperty(UInt32 index, PROPID propID, PROPVARIANT *value))
{
  RINOK(_inner->GetProperty(index, propID, value))
  if (propID != kpidPath || value->vt != VT_BSTR || !value->bstrVal)
    return S_OK;
  UString path = value->bstrVal;
  if (path.Find(L'\\') < 0 || MadeOnUnix(index))
    return S_OK;
  path.Replace(L'\\', L'/');
  // "dir\" names a folder: no tail separator, as upstream gives "dir/"
  while (path.Len() > 1 && path.Back() == L'/')
    path.DeleteBack();
  NCOM::CPropVariant prop;
  prop = path;
  RINOK(NCOM::PropVariant_Clear(value))
  return prop.Detach(value);
}

IInArchive *CreateWinPathZip()
{
  IInArchive *inner = g_ZipCreateInArchive();
  CMyComPtr<IInArchive> ref = inner;
  return new CWinPathZip(inner);
}

}

void Z7Qt_FixWinPathFormats(CCodecs *codecs)
{
  FOR_VECTOR (i, codecs->Formats)
  {
    CArcInfoEx &f = codecs->Formats[i];
    if (!f.Name.IsEqualTo_Ascii_NoCase("zip") || !f.CreateInArchive || f.CreateInArchive == CreateWinPathZip)
      continue;
    g_ZipCreateInArchive = f.CreateInArchive;
    f.CreateInArchive = CreateWinPathZip;
  }
}

// UpxWrap.cpp -- 7zfm-qt: executables packed by UPX open as the unpacked
// executable, made by the system's `upx -d` when it is installed (an
// optional dependency; UPX is GPL, so it runs as its own program).
//
// Upstream's PE, ELF and Mach-O readers are wrapped the way WinPathZip.cpp
// wraps zip: a file with UPX's "UPX!" mark near its start is copied to a
// temporary folder and unpacked there; the archive is then that one file.
// Anything else, or a file upx can't unpack, or no upx: upstream's reader
// as before.

#include "UpxWrap.h"

#include "Common/ComTry.h"
#include "Common/MyCom.h"
#include "Common/MyString.h"
#include "Common/StringConvert.h"

#include "Windows/PropVariant.h"

#include "7zip/Archive/IArchive.h"
#include "7zip/Common/FileStreams.h"
#include "7zip/Common/StreamUtils.h"
#include "7zip/PropID.h"
#include "7zip/UI/Common/LoadCodecs.h"

#include "common/ItemArchive.h"

#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>

extern char **environ;

using namespace NWindows;
using NArchive::NItemArc::GetUi32;

namespace {

static const char * const kWrapped[] = { "PE", "ELF", "MachO" };
static Func_CreateInArchive g_Create[3];

// the upx program on PATH, or empty
static std::string FindUpx()
{
  const char *path = getenv("PATH");
  if (!path)
    return std::string();
  std::string p = path;
  size_t start = 0;
  while (start <= p.size())
  {
    size_t end = p.find(':', start);
    if (end == std::string::npos)
      end = p.size();
    std::string dir = p.substr(start, end - start);
    if (!dir.empty())
    {
      const std::string f = dir + "/upx";
      if (access(f.c_str(), X_OK) == 0)
        return f;
    }
    start = end + 1;
  }
  return std::string();
}

static bool HasUpxMark(IInStream *stream)
{
  Byte buf[8192];
  size_t n = sizeof(buf);
  if (InStream_SeekSet(stream, 0) != S_OK || ReadStream(stream, buf, &n) != S_OK)
    return false;
  for (size_t i = 0; i + 4 <= n; i++)
    if (buf[i] == 'U' && memcmp(buf + i, "UPX!", 4) == 0)
      return true;
  return false;
}

class CUpxWrap Z7_final:
  public IInArchive,
  public IInArchiveGetStream,
  public CMyUnknownImp
{
  CMyComPtr<IInArchive> _inner;
  bool _unpacked;
  std::string _dir;          // the temporary folder
  std::string _out;          // the unpacked file in it
  UString _name;
  UInt64 _size;
  UInt64 _phySize;

  void Cleanup()
  {
    if (!_out.empty())
      unlink(_out.c_str());
    if (!_dir.empty())
    {
      unlink((_dir + "/packed").c_str());
      rmdir(_dir.c_str());
    }
    _out.clear();
    _dir.clear();
    _unpacked = false;
  }
  HRESULT TryUnpack(IInStream *stream, IArchiveOpenCallback *callback);
public:
  explicit CUpxWrap(IInArchive *inner): _inner(inner), _unpacked(false), _size(0), _phySize(0) {}
  ~CUpxWrap() { Cleanup(); }

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
    if (iid == IID_IInArchiveGetStream && _unpacked)
    {
      IInArchiveGetStream *p = this;
      *outObject = p;
      ++_m_RefCount;
      return S_OK;
    }
    return _inner->QueryInterface(iid, outObject);
  }
  Z7_COM_ADDREF_RELEASE
  Z7_IFACE_COM7_IMP(IInArchive)
  Z7_IFACE_COM7_IMP(IInArchiveGetStream)
};

HRESULT CUpxWrap::TryUnpack(IInStream *stream, IArchiveOpenCallback *callback)
{
  const std::string upx = FindUpx();
  if (upx.empty() || !HasUpxMark(stream))
    return S_FALSE;
  const char *tmp = getenv("TMPDIR");
  std::string tmpl = std::string(tmp && *tmp ? tmp : "/tmp") + "/7zfm-qt-upx.XXXXXX";
  std::string buf = tmpl;
  if (!mkdtemp(&buf[0]))
    return S_FALSE;
  _dir = buf;
  const std::string packed = _dir + "/packed";
  _out = _dir + "/unpacked";
  {
    // the packed file, as upx needs a path
    COutFileStream *spec = new COutFileStream;
    CMyComPtr<ISequentialOutStream> outRef = spec;
    if (!spec->Create_NEW(us2fs(GetUnicodeString(packed.c_str()))))
      return S_FALSE;
    RINOK(InStream_SeekSet(stream, 0))
    Byte b[1 << 16];
    for (;;)
    {
      size_t n = sizeof(b);
      RINOK(ReadStream(stream, b, &n))
      if (n == 0)
        break;
      RINOK(WriteStream(outRef, b, n))
    }
  }
  // upx -d -q -o unpacked packed, without a shell
  const char *argv[] = { upx.c_str(), "-d", "-qqq", "-o", _out.c_str(), packed.c_str(), NULL };
  pid_t pid;
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", 0, 0);
  posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", 1, 0);
  posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", 1, 0);
  const int r = posix_spawn(&pid, upx.c_str(), &fa, NULL, (char * const *)argv, environ);
  posix_spawn_file_actions_destroy(&fa);
  if (r != 0)
    return S_FALSE;
  int status = 0;
  const bool ok = waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
  unlink(packed.c_str());
  if (!ok)
    return S_FALSE;
  struct stat st;
  if (stat(_out.c_str(), &st) != 0)
    return S_FALSE;
  _size = (UInt64)st.st_size;
  RINOK(NArchive::NItemArc::GetStreamSize(stream, _phySize))
  _name = NArchive::NItemArc::GetArcName(callback);
  if (_name.IsEmpty())
    _name = "unpacked";
  _unpacked = true;
  return S_OK;
}

Z7_COM7F_IMF(CUpxWrap::Open(IInStream *stream, const UInt64 *maxCheckStartPosition, IArchiveOpenCallback *callback))
{
  COM_TRY_BEGIN
  Cleanup();
  // a file the wrapped reader takes as its own, or (a packed ELF has no
  // section table upstream accepts) one with the executable's magic
  RINOK(InStream_SeekSet(stream, 0))
  const HRESULT res = _inner->Open(stream, maxCheckStartPosition, callback);
  if (res != S_OK && res != S_FALSE)
    return res;
  if (res == S_FALSE)
  {
    Byte m[4];
    size_t n = 4;
    RINOK(InStream_SeekSet(stream, 0))
    RINOK(ReadStream(stream, m, &n))
    const bool exe = n == 4 && (memcmp(m, "\x7F" "ELF", 4) == 0 || (m[0] == 'M' && m[1] == 'Z')
        || GetUi32(m) == 0xFEEDFACE || GetUi32(m) == 0xFEEDFACF || GetUi32(m) == 0xCEFAEDFE || GetUi32(m) == 0xCFFAEDFE);
    if (exe && TryUnpack(stream, callback) == S_OK)
      return S_OK;
    Cleanup();
    return S_FALSE;
  }
  if (TryUnpack(stream, callback) == S_OK)
  {
    _inner->Close();
    return S_OK;
  }
  Cleanup();
  return S_OK;
  COM_TRY_END
}

Z7_COM7F_IMF(CUpxWrap::Close())
{
  if (_unpacked)
  {
    Cleanup();
    return S_OK;
  }
  return _inner->Close();
}

Z7_COM7F_IMF(CUpxWrap::GetNumberOfItems(UInt32 *numItems))
{
  if (_unpacked)
  {
    *numItems = 1;
    return S_OK;
  }
  return _inner->GetNumberOfItems(numItems);
}

Z7_COM7F_IMF(CUpxWrap::GetProperty(UInt32 index, PROPID propID, PROPVARIANT *value))
{
  if (!_unpacked)
    return _inner->GetProperty(index, propID, value);
  NCOM::CPropVariant prop;
  switch (propID)
  {
    case kpidPath: prop = _name; break;
    case kpidSize: prop = _size; break;
    case kpidPackSize: prop = _phySize; break;
    case kpidMethod: prop = "UPX"; break;
    default: break;
  }
  prop.Detach(value);
  return S_OK;
}

Z7_COM7F_IMF(CUpxWrap::GetArchiveProperty(PROPID propID, PROPVARIANT *value))
{
  if (!_unpacked)
    return _inner->GetArchiveProperty(propID, value);
  NCOM::CPropVariant prop;
  switch (propID)
  {
    case kpidPhySize: prop = _phySize; break;
    case kpidComment: prop = "Unpacked by upx -d"; break;
    default: break;
  }
  prop.Detach(value);
  return S_OK;
}

static const Byte kProps[] = { kpidPath, kpidSize, kpidPackSize, kpidMethod };

Z7_COM7F_IMF(CUpxWrap::GetNumberOfProperties(UInt32 *numProps))
{
  if (!_unpacked)
    return _inner->GetNumberOfProperties(numProps);
  *numProps = Z7_ARRAY_SIZE(kProps);
  return S_OK;
}

Z7_COM7F_IMF(CUpxWrap::GetPropertyInfo(UInt32 index, BSTR *name, PROPID *propID, VARTYPE *varType))
{
  if (!_unpacked)
    return _inner->GetPropertyInfo(index, name, propID, varType);
  if (index >= Z7_ARRAY_SIZE(kProps))
    return E_INVALIDARG;
  *name = NULL;
  *propID = kProps[index];
  *varType = k7z_PROPID_To_VARTYPE[(unsigned)*propID];
  return S_OK;
}

Z7_COM7F_IMF(CUpxWrap::GetNumberOfArchiveProperties(UInt32 *numProps))
{
  if (!_unpacked)
    return _inner->GetNumberOfArchiveProperties(numProps);
  *numProps = 0;
  return S_OK;
}

Z7_COM7F_IMF(CUpxWrap::GetArchivePropertyInfo(UInt32 index, BSTR *name, PROPID *propID, VARTYPE *varType))
{
  if (!_unpacked)
    return _inner->GetArchivePropertyInfo(index, name, propID, varType);
  return E_INVALIDARG;
}

Z7_COM7F_IMF(CUpxWrap::GetStream(UInt32 index, ISequentialInStream **stream))
{
  COM_TRY_BEGIN
  *stream = NULL;
  if (!_unpacked || index != 0)
    return S_FALSE;
  CInFileStream *spec = new CInFileStream;
  CMyComPtr<ISequentialInStream> ref = spec;
  if (!spec->Open(us2fs(GetUnicodeString(_out.c_str()))))
    return S_FALSE;
  *stream = ref.Detach();
  return S_OK;
  COM_TRY_END
}

Z7_COM7F_IMF(CUpxWrap::Extract(const UInt32 *indices, UInt32 numItems, Int32 testMode, IArchiveExtractCallback *extractCallback))
{
  COM_TRY_BEGIN
  if (!_unpacked)
    return _inner->Extract(indices, numItems, testMode, extractCallback);
  if (numItems == 0)
    return S_OK;
  RINOK(extractCallback->SetTotal(_size))
  CMyComPtr<ISequentialOutStream> out;
  const Int32 askMode = testMode ? NArchive::NExtract::NAskMode::kTest : NArchive::NExtract::NAskMode::kExtract;
  RINOK(extractCallback->GetStream(0, &out, askMode))
  if (!testMode && !out)
    return S_OK;
  RINOK(extractCallback->PrepareOperation(askMode))
  Int32 opRes = NArchive::NExtract::NOperationResult::kOK;
  CMyComPtr<ISequentialInStream> in;
  if (GetStream(0, &in) != S_OK || !in)
    opRes = NArchive::NExtract::NOperationResult::kUnavailable;
  else if (out)
  {
    Byte b[1 << 16];
    for (;;)
    {
      size_t n = sizeof(b);
      RINOK(ReadStream(in, b, &n))
      if (n == 0)
        break;
      RINOK(WriteStream(out, b, n))
    }
  }
  out.Release();
  return extractCallback->SetOperationResult(opRes);
  COM_TRY_END
}

template <unsigned k>
IInArchive *CreateWrapped()
{
  IInArchive *inner = g_Create[k]();
  CMyComPtr<IInArchive> ref = inner;
  return new CUpxWrap(inner);
}

static Func_CreateInArchive const kCreators[3] = { CreateWrapped<0>, CreateWrapped<1>, CreateWrapped<2> };

}

void Z7Qt_WrapUpxFormats(CCodecs *codecs)
{
  FOR_VECTOR (i, codecs->Formats)
  {
    CArcInfoEx &f = codecs->Formats[i];
    for (unsigned k = 0; k < Z7_ARRAY_SIZE(kWrapped); k++)
    {
      if (!f.Name.IsEqualTo_Ascii_NoCase(kWrapped[k]) || !f.CreateInArchive || f.CreateInArchive == kCreators[k])
        continue;
      g_Create[k] = f.CreateInArchive;
      f.CreateInArchive = kCreators[k];
    }
  }
}

// ISSetup.cpp -- 7zfm-qt: the files InstallShield (7 and later) puts in a
// single setup.exe: setup.ini, the language .ini files, instmsi*.exe, the
// product's .msi or data1.cab and so on, appended to the launcher. After
// ISx (lifenjoiner, MIT license; see LICENSE.isx), its reading rewritten
// for 7-Zip's interfaces:
//
//   "InstallShield\0" (Developer 7 to 2014 or so) or "ISSetupStream\0"
//   (later), u16 number of files, u32 type (ISSetupStream: up to 4), and
//   28 bytes more: 46 in all. Then each file's record and its data:
//     InstallShield:  the name (260 bytes, ANSI), u32 flags, u32, u32 data
//                     size, 8 bytes, u16 compressed, 30 bytes: 312 in all
//     ISSetupStream:  u32 name size, u32 flags, u16, u32 data size,
//                     8 bytes, u16 compressed (24 in all; type 4: 24 more),
//                     the name (UTF-16)
//
// Flag 4: the data is XORed in runs of 1024 bytes, each from the start of
// the key; flag 2 alone (InstallShield): XORed in one run. The key is the
// name (ISSetupStream: in UTF-8) XORed with 13 35 86 07; a byte b becomes
// ~(key ^ (b << 4 | b >> 4)). Compressed data is then zlib.
//
// The launcher (the .exe before the files) is found by the signature, as a
// self-extracting archive's is.
//
// InstallShield 10.5 (and others, by ISx) write no signature: right after
// the launcher's last section each file is its name, the path to put it
// at, its version and its size in decimal, each ending in a 0, then its
// data. That is format ISSetupText, which takes the .exe from its start.
// The files may follow a CodeView "NB10" block, skipped as ISx skips it.

#include "Common/ComTry.h"
#include "Common/MyBuffer.h"
#include "Common/StringConvert.h"
#include "Common/UTFConvert.h"

#include "Windows/PropVariant.h"

#include "7zip/Common/ProgressUtils.h"
#include "7zip/Common/RegisterArc.h"
#include "7zip/Common/StreamObjects.h"
#include "7zip/Common/StreamUtils.h"

#include "../common/ItemArchive.h"

#include <zlib.h>

#include <string.h>

using namespace NWindows;

namespace NArchive {
namespace NISSetup {

using NItemArc::GetUi16;
using NItemArc::GetUi32;
using NItemArc::ReadAt;
using NItemArc::GetStreamSize;

static const unsigned kHeaderSize = 46;
static const unsigned kRecordSize = 312;
static const unsigned kRecordSizeX = 24;
static const unsigned kMaxFiles = 1 << 14;
static const UInt64 kMaxInMemory = (UInt64)1 << 30;
static const Byte kMagic[4] = { 0x13, 0x35, 0x86, 0x07 };

enum EXor
{
  kXor_None,
  kXor_1024,    // from the start of the key every 1024 bytes
  kXor_File     // one run
};

struct CFile
{
  UString Path;
  CByteBuffer Key;
  UInt64 Offset;
  UInt64 PackSize;
  UInt64 Size;          // known when not compressed, or once decoded
  bool SizeDefined;
  bool Compressed;
  Byte Xor;
};

class CHandler:
  public IInArchive,
  public IInArchiveGetStream,
  public CMyUnknownImp
{
  Z7_IFACES_IMP_UNK_2(IInArchive, IInArchiveGetStream)
public:
  CObjectVector<CFile> _files;
  CMyComPtr<IInStream> _stream;
  UInt64 _phySize;
  bool _strm;
  const bool _text;     // ISSetupText
  explicit CHandler(bool text = false): _phySize(0), _strm(false), _text(text) {}
  virtual ~CHandler() {}
  HRESULT Open2(IInStream *stream);
  HRESULT OpenText(IInStream *stream);
};


static Byte DecodeByte(Byte b, Byte key)
{
  return (Byte)~(key ^ (Byte)((b << 4) | (b >> 4)));
}

// decodes (size) bytes of file (f) that are at (pos) in its data
static void Decode(const CFile &f, UInt64 pos, Byte *data, size_t size)
{
  const size_t keyLen = f.Key.Size();
  if (f.Xor == kXor_None || keyLen == 0)
    return;
  for (size_t i = 0; i < size; i++)
  {
    const UInt64 p = pos + i;
    const size_t k = (size_t)((f.Xor == kXor_1024 ? (p & 1023) : p) % keyLen);
    data[i] = DecodeByte(data[i], f.Key[k]);
  }
}

static CByteBuffer MakeKey(const AString &seed)
{
  CByteBuffer key(seed.Len());
  for (unsigned i = 0; i < seed.Len(); i++)
    key[i] = (Byte)((Byte)seed[i] ^ kMagic[i & 3]);
  return key;
}

HRESULT CHandler::Open2(IInStream *stream)
{
  UInt64 start = 0;
  RINOK(InStream_GetPos(stream, start))
  UInt64 fileSize = 0;
  RINOK(GetStreamSize(stream, fileSize))
  Byte h[kHeaderSize];
  if (fileSize < start + kHeaderSize)
    return S_FALSE;
  RINOK(ReadAt(stream, start, h, kHeaderSize))
  if (memcmp(h, "InstallShield\0", 14) == 0)
    _strm = false;
  else if (memcmp(h, "ISSetupStream\0", 14) == 0)
    _strm = true;
  else
    return S_FALSE;
  const unsigned numFiles = GetUi16(h + 14);
  const UInt32 type = GetUi32(h + 16);
  if (numFiles == 0 || numFiles > kMaxFiles || type > 4)
    return S_FALSE;

  UInt64 pos = start + kHeaderSize;
  for (unsigned i = 0; i < numFiles; i++)
  {
    CFile f;
    UInt32 flags;
    AString seed;
    if (!_strm)
    {
      Byte r[kRecordSize];
      if (pos + kRecordSize > fileSize)
        return S_FALSE;
      RINOK(ReadAt(stream, pos, r, kRecordSize))
      pos += kRecordSize;
      unsigned len = 0;
      while (len < 260 && r[len])
        len++;
      if (len == 0 || len == 260)
        return S_FALSE;
      seed.SetFrom((const char *)r, len);
      flags = GetUi32(r + 260);
      f.PackSize = GetUi32(r + 268);
      f.Compressed = GetUi16(r + 280) != 0;
      f.Path = MultiByteToUnicodeString(seed, CP_ACP);
      f.Xor = (flags & 4) ? kXor_1024 : (flags & 2) ? kXor_File : kXor_None;
    }
    else
    {
      Byte r[kRecordSizeX];
      if (pos + kRecordSizeX > fileSize)
        return S_FALSE;
      RINOK(ReadAt(stream, pos, r, kRecordSizeX))
      pos += kRecordSizeX;
      if (type == 4)
        pos += kRecordSizeX;
      const UInt32 nameSize = GetUi32(r);
      flags = GetUi32(r + 4);
      f.PackSize = GetUi32(r + 10);
      f.Compressed = GetUi16(r + 22) != 0;
      if (nameSize == 0 || nameSize > 260 * 2 || (nameSize & 1) || pos + nameSize > fileSize)
        return S_FALSE;
      CByteBuffer nb(nameSize);
      RINOK(ReadAt(stream, pos, nb, nameSize))
      pos += nameSize;
      for (UInt32 k = 0; k + 1 < nameSize; k += 2)
      {
        const wchar_t c = (wchar_t)GetUi16(nb + k);
        if (c == 0)
          break;
        f.Path += c;
      }
      if (f.Path.IsEmpty())
        return S_FALSE;
      ConvertUnicodeToUTF8(f.Path, seed);
      // as ISx: flag 2 alone leaves the data as it is here
      f.Xor = (flags & 4) ? kXor_1024 : kXor_None;
    }
    if (pos + f.PackSize > fileSize)
      return S_FALSE;
    f.Path.Replace(L'\\', L'/');
    f.Key = MakeKey(seed);
    f.Offset = pos;
    f.Size = f.PackSize;
    f.SizeDefined = !f.Compressed;
    pos += f.PackSize;
    _files.Add(f);
  }
  _phySize = pos;
  return S_OK;
}

// where the launcher's sections end, or 0 if it isn't a PE file
static UInt64 PeEnd(IInStream *stream, UInt64 fileSize)
{
  Byte h[0x40];
  if (fileSize < 0x40 || ReadAt(stream, 0, h, 0x40) != S_OK || h[0] != 'M' || h[1] != 'Z')
    return 0;
  const UInt32 pe = GetUi32(h + 0x3C);
  Byte p[24];
  if (pe > (1 << 20) || pe + 24 > fileSize || ReadAt(stream, pe, p, 24) != S_OK || GetUi32(p) != 0x4550)
    return 0;
  const unsigned numSections = GetUi16(p + 6);
  const unsigned optSize = GetUi16(p + 20);
  if (numSections == 0 || numSections > 96)
    return 0;
  const UInt64 tablePos = (UInt64)pe + 24 + optSize;
  const size_t tableSize = (size_t)numSections * 40;
  if (tablePos + tableSize > fileSize)
    return 0;
  CByteBuffer t(tableSize);
  if (ReadAt(stream, tablePos, t, tableSize) != S_OK)
    return 0;
  UInt64 end = 0;
  for (unsigned i = 0; i < numSections; i++)
  {
    const Byte *sec = t + i * 40;
    const UInt64 e = (UInt64)GetUi32(sec + 20) + GetUi32(sec + 16);
    if (GetUi32(sec + 16) != 0)
      end = MyMax(end, e);
  }
  return end <= fileSize ? end : 0;
}

// a field of a file's text record: up to (max) bytes of (chars) and a 0
static bool ReadField(const Byte *p, size_t size, size_t &pos, size_t max, AString &s, bool digits)
{
  s.Empty();
  while (pos < size && p[pos] != 0)
  {
    const Byte c = p[pos++];
    if (c < 0x20 || c == 0xFF || (digits && !(c >= '0' && c <= '9')) || s.Len() >= max)
      return false;
    s += (char)c;
  }
  if (pos >= size)
    return false;
  pos++;
  return true;
}

HRESULT CHandler::OpenText(IInStream *stream)
{
  UInt64 fileSize = 0;
  RINOK(GetStreamSize(stream, fileSize))
  UInt64 pos = PeEnd(stream, fileSize);
  if (pos == 0 || pos >= fileSize)
    return S_FALSE;
  const size_t kRec = 4 * 261 + 64;
  CByteBuffer b(kRec);
  {
    // a CodeView block first: "NB10", zeros, its data, zeros, the .pdb path, zeros
    size_t n = (size_t)MyMin((UInt64)kRec, fileSize - pos);
    RINOK(ReadAt(stream, pos, b, n))
    if (n > 4 && memcmp(b, "NB10", 4) == 0)
    {
      size_t i = 4;
      while (i < n && b[i] >= 0x20 && b[i] != 0xFF) i++;
      while (i < n && (b[i] == 0 || b[i] == 0xFF)) i++;
      while (i < n && b[i] != 0 && b[i] != 0xFF) i++;
      while (i < n && (b[i] == 0 || b[i] == 0xFF)) i++;
      while (i < n && b[i] >= 0x20 && b[i] != 0xFF) i++;
      while (i < n && (b[i] == 0 || b[i] == 0xFF)) i++;
      pos += i;
    }
  }
  while (pos < fileSize && _files.Size() < kMaxFiles)
  {
    const size_t n = (size_t)MyMin((UInt64)kRec, fileSize - pos);
    RINOK(ReadAt(stream, pos, b, n))
    size_t p = 0;
    AString name, dest, version, sizeStr;
    if (!ReadField(b, n, p, 259, name, false) || name.IsEmpty()
        || !ReadField(b, n, p, 259, dest, false) || dest.IsEmpty()
        || !ReadField(b, n, p, 31, version, false)
        || !ReadField(b, n, p, 10, sizeStr, true) || sizeStr.IsEmpty())
      break;
    UInt64 size = 0;
    for (unsigned i = 0; i < sizeStr.Len(); i++)
      size = size * 10 + (unsigned)(sizeStr[i] - '0');
    if (pos + p + size > fileSize)
      break;
    CFile &f = _files.AddNew();
    f.Path = MultiByteToUnicodeString(dest, CP_ACP);
    f.Path.Replace(L'\\', L'/');
    f.Offset = pos + p;
    f.PackSize = f.Size = size;
    f.SizeDefined = true;
    f.Compressed = false;
    f.Xor = kXor_None;
    pos += p + size;
  }
  if (_files.IsEmpty())
    return S_FALSE;
  _phySize = pos;
  return S_OK;
}

// reads file (f)'s stored data, decoded, at (pos)
class CDataStream Z7_final:
  public IInStream,
  public CMyUnknownImp
{
  Z7_IFACES_IMP_UNK_2(ISequentialInStream, IInStream)
  CMyComPtr<IInArchive> _arcRef;
  CMyComPtr<IInStream> _stream;
  const CFile *_f;
  UInt64 _pos;
public:
  void Init(IInArchive *arc, IInStream *stream, const CFile *f)
  {
    _arcRef = arc;
    _stream = stream;
    _f = f;
    _pos = 0;
  }
};

Z7_COM7F_IMF(CDataStream::Read(void *data, UInt32 size, UInt32 *processedSize))
{
  if (processedSize)
    *processedSize = 0;
  if (_pos >= _f->PackSize || size == 0)
    return S_OK;
  const UInt64 rem = _f->PackSize - _pos;
  if (size > rem)
    size = (UInt32)rem;
  RINOK(InStream_SeekSet(_stream, _f->Offset + _pos))
  UInt32 got = 0;
  RINOK(_stream->Read(data, size, &got))
  Decode(*_f, _pos, (Byte *)data, got);
  _pos += got;
  if (processedSize)
    *processedSize = got;
  return S_OK;
}

Z7_COM7F_IMF(CDataStream::Seek(Int64 offset, UInt32 seekOrigin, UInt64 *newPosition))
{
  switch (seekOrigin)
  {
    case STREAM_SEEK_SET: break;
    case STREAM_SEEK_CUR: offset += (Int64)_pos; break;
    case STREAM_SEEK_END: offset += (Int64)_f->PackSize; break;
    default: return STG_E_INVALIDFUNCTION;
  }
  if (offset < 0)
    return HRESULT_WIN32_ERROR_NEGATIVE_SEEK;
  _pos = (UInt64)offset;
  if (newPosition)
    *newPosition = _pos;
  return S_OK;
}

// writes file (f) to (out), if any; (written): its size
static HRESULT DecodeFile(IInStream *stream, const CFile &f, ISequentialOutStream *out,
    ICompressProgressInfo *progress, UInt64 &written, Int32 &opRes)
{
  opRes = NExtract::NOperationResult::kOK;
  written = 0;
  const size_t kBuf = 1 << 16;
  CByteBuffer in(kBuf), outBuf(kBuf * 4);
  z_stream z;
  memset(&z, 0, sizeof(z));
  if (f.Compressed && inflateInit(&z) != Z_OK)
    return E_OUTOFMEMORY;
  int ret = Z_OK;
  HRESULT hr = S_OK;
  UInt64 pos = 0;
  while (pos < f.PackSize && hr == S_OK)
  {
    const size_t n = (size_t)MyMin((UInt64)kBuf, f.PackSize - pos);
    hr = ReadAt(stream, f.Offset + pos, in, n);
    if (hr != S_OK)
      break;
    Decode(f, pos, in, n);
    pos += n;
    if (!f.Compressed)
    {
      if (out)
        hr = WriteStream(out, in, n);
      written += n;
    }
    else
    {
      z.next_in = in;
      z.avail_in = (uInt)n;
      while (z.avail_in != 0 && ret == Z_OK)
      {
        z.next_out = outBuf;
        z.avail_out = (uInt)outBuf.Size();
        ret = inflate(&z, Z_NO_FLUSH);
        const size_t got = outBuf.Size() - z.avail_out;
        if (ret == Z_BUF_ERROR && got != 0)
          ret = Z_OK;
        if (out && got != 0)
        {
          hr = WriteStream(out, outBuf, got);
          if (hr != S_OK)
            break;
        }
        written += got;
      }
      if (ret != Z_OK)
        break;
    }
    if (progress)
    {
      const HRESULT p = progress->SetRatioInfo(&pos, &written);
      if (p != S_OK)
        hr = p;
    }
  }
  // what's left in the inflater
  while (f.Compressed && ret == Z_OK && hr == S_OK && pos == f.PackSize)
  {
    z.next_out = outBuf;
    z.avail_out = (uInt)outBuf.Size();
    ret = inflate(&z, Z_FINISH);
    const size_t got = outBuf.Size() - z.avail_out;
    if (got == 0 && ret != Z_STREAM_END)
      break;
    if (ret == Z_BUF_ERROR)
      ret = Z_OK;
    if (out && got != 0)
      hr = WriteStream(out, outBuf, got);
    written += got;
  }
  if (f.Compressed)
    inflateEnd(&z);
  if (hr == S_FALSE)
  {
    opRes = NExtract::NOperationResult::kUnexpectedEnd;
    return S_OK;
  }
  RINOK(hr)
  if (f.Compressed && ret != Z_STREAM_END)
    opRes = (ret == Z_OK) ? NExtract::NOperationResult::kUnexpectedEnd : NExtract::NOperationResult::kDataError;
  return S_OK;
}

static const Byte kProps[] = { kpidPath, kpidSize, kpidPackSize, kpidMethod };
static const Byte kArcProps[] = { kpidComment };
IMP_IInArchive_Props
IMP_IInArchive_ArcProps

Z7_COM7F_IMF(CHandler::GetArchiveProperty(PROPID propID, PROPVARIANT *value))
{
  NCOM::CPropVariant prop;
  switch (propID)
  {
    case kpidPhySize: prop = _phySize; break;
    case kpidComment: prop = _text ? "InstallShield setup (text records)" : _strm ? "InstallShield setup (ISSetupStream)" : "InstallShield setup"; break;
    default: break;
  }
  prop.Detach(value);
  return S_OK;
}

Z7_COM7F_IMF(CHandler::GetProperty(UInt32 index, PROPID propID, PROPVARIANT *value))
{
  NCOM::CPropVariant prop;
  const CFile &f = _files[index];
  switch (propID)
  {
    case kpidPath: prop = f.Path; break;
    case kpidSize: if (f.SizeDefined) prop = f.Size; break;
    case kpidPackSize: prop = f.PackSize; break;
    case kpidMethod:
    {
      AString s (f.Xor == kXor_None ? "" : "XOR");
      if (f.Compressed)
      {
        s.Add_Space_if_NotEmpty();
        s += "Deflate";
      }
      if (s.IsEmpty())
        s = "Copy";
      prop = s;
      break;
    }
    default: break;
  }
  prop.Detach(value);
  return S_OK;
}

Z7_COM7F_IMF(CHandler::Open(IInStream *stream, const UInt64 *, IArchiveOpenCallback *))
{
  COM_TRY_BEGIN
  Close();
  const HRESULT res = _text ? OpenText(stream) : Open2(stream);
  if (res != S_OK)
  {
    Close();
    return res;
  }
  _stream = stream;
  return S_OK;
  COM_TRY_END
}

Z7_COM7F_IMF(CHandler::Close())
{
  _files.Clear();
  _stream.Release();
  _phySize = 0;
  _strm = false;
  return S_OK;
}

Z7_COM7F_IMF(CHandler::GetNumberOfItems(UInt32 *numItems))
{
  *numItems = _files.Size();
  return S_OK;
}

Z7_COM7F_IMF(CHandler::Extract(const UInt32 *indices, UInt32 numItems,
    Int32 testMode, IArchiveExtractCallback *extractCallback))
{
  COM_TRY_BEGIN
  const bool allFilesMode = (numItems == (UInt32)(Int32)-1);
  if (allFilesMode)
    numItems = _files.Size();
  UInt64 total = 0;
  for (UInt32 i = 0; i < numItems; i++)
    total += _files[allFilesMode ? i : indices[i]].PackSize;
  RINOK(extractCallback->SetTotal(total))
  CLocalProgress *lps = new CLocalProgress;
  CMyComPtr<ICompressProgressInfo> progress = lps;
  lps->Init(extractCallback, true);
  UInt64 done = 0;
  for (UInt32 i = 0; i < numItems; i++)
  {
    lps->InSize = lps->OutSize = done;
    RINOK(lps->SetCur())
    const UInt32 index = allFilesMode ? i : indices[i];
    CFile &f = _files[index];
    CMyComPtr<ISequentialOutStream> out;
    const Int32 askMode = testMode ? NExtract::NAskMode::kTest : NExtract::NAskMode::kExtract;
    RINOK(extractCallback->GetStream(index, &out, askMode))
    done += f.PackSize;
    if (!testMode && !out)
      continue;
    RINOK(extractCallback->PrepareOperation(askMode))
    Int32 opRes;
    UInt64 written;
    RINOK(DecodeFile(_stream, f, out, NULL, written, opRes))
    if (opRes == NExtract::NOperationResult::kOK && f.Compressed)
    {
      f.Size = written;
      f.SizeDefined = true;
    }
    out.Release();
    RINOK(extractCallback->SetOperationResult(opRes))
  }
  lps->InSize = lps->OutSize = done;
  return lps->SetCur();
  COM_TRY_END
}

Z7_COM7F_IMF(CHandler::GetStream(UInt32 index, ISequentialInStream **stream))
{
  COM_TRY_BEGIN
  *stream = NULL;
  if (index >= _files.Size())
    return S_FALSE;
  CFile &f = _files[index];
  if (!f.Compressed)
  {
    CDataStream *spec = new CDataStream;
    CMyComPtr<IInStream> ref = spec;
    spec->Init(this, _stream, &f);
    *stream = ref.Detach();
    return S_OK;
  }
  // compressed: decoded into memory, its size found first
  Int32 opRes;
  UInt64 size = f.Size;
  if (!f.SizeDefined)
  {
    RINOK(DecodeFile(_stream, f, NULL, NULL, size, opRes))
    if (opRes != NExtract::NOperationResult::kOK)
      return S_FALSE;
    f.Size = size;
    f.SizeDefined = true;
  }
  if (size > kMaxInMemory)
    return S_FALSE;
  CReferenceBuf *ref = new CReferenceBuf;
  CMyComPtr<IUnknown> refRef = ref;
  ref->Buf.Alloc((size_t)size);
  CBufPtrSeqOutStream *spec = new CBufPtrSeqOutStream;
  CMyComPtr<ISequentialOutStream> out = spec;
  spec->Init(ref->Buf, ref->Buf.Size());
  UInt64 written;
  RINOK(DecodeFile(_stream, f, out, NULL, written, opRes))
  if (opRes != NExtract::NOperationResult::kOK || written != size)
    return S_FALSE;
  CBufInStream *in = new CBufInStream;
  CMyComPtr<ISequentialInStream> inRef = in;
  in->Init(ref);
  *stream = inRef.Detach();
  return S_OK;
  COM_TRY_END
}

static const Byte k_Signature[] =
{
  14, 'I', 'n', 's', 't', 'a', 'l', 'l', 'S', 'h', 'i', 'e', 'l', 'd', 0,
  14, 'I', 'S', 'S', 'e', 't', 'u', 'p', 'S', 't', 'r', 'e', 'a', 'm', 0
};

REGISTER_ARC_I(
  "ISSetup", "exe", NULL, 0xA7,
  k_Signature,
  0,
  NArcInfoFlags::kMultiSignature | NArcInfoFlags::kFindSignature | NArcInfoFlags::kUseGlobalOffset,
  NULL)

}

namespace NISSetupText {

class CHandler Z7_final: public NISSetup::CHandler
{
public:
  CHandler(): NISSetup::CHandler(true) {}
};

static const Byte k_Signature[] = { 'M', 'Z' };

REGISTER_ARC_I(
  "ISSetupText", "exe", NULL, 0xB5,
  k_Signature,
  0,
  0,
  NULL)

}}

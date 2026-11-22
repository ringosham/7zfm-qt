// GodotPck.cpp -- 7zfm-qt: Godot engine packs (.pck), also as exported
// into a game's executable. After Godot's own reader
// (core/io/file_access_pack.cpp, MIT license):
//
//   "GDPC", pack format version (1: Godot 3; 2: Godot 4.0 to 4.3; 3: 4.4
//   and later), engine version, then from version 2 pack flags and the
//   base of the file offsets, from version 3 the offset of the directory;
//   64 reserved bytes. The directory: a count, then for each file its path
//   ("res://..." up to 4.3), offset, size, MD5 and, from version 2, flags.
//
// Offsets count from the start of the pack (version 1) or from the file
// base (2 and 3), which itself counts from the start of the pack when the
// relative-file-base flag is set. An exported executable has the pack
// appended or in a section; the archive is found by its signature.
//
// Godot can encrypt the directory and files with a 256-bit key (AES-CFB,
// FileAccessEncrypted: MD5 of the plain data, its length, the IV, then
// the data in 16-byte blocks). The key is asked as 64 hex digits.

#include "Common/ComTry.h"
#include "Common/MyBuffer.h"
#include "Common/StringConvert.h"
#include "Common/UTFConvert.h"

#include "7zip/Common/RegisterArc.h"
#include "7zip/Common/StreamUtils.h"

#include "C/Aes.h"
#include "C/Md5.h"

#include "../common/ItemArchive.h"

#include <string.h>

namespace NArchive {
namespace NGodotPck {

using namespace NItemArc;

static const UInt32 kFlag_DirEncrypted = 1 << 0;
static const UInt32 kFlag_RelFileBase = 1 << 1;
static const UInt32 kFileFlag_Encrypted = 1 << 0;
static const UInt32 kFileFlag_Removal = 1 << 1;
static const size_t kHeaderSize = 4 + 4 * 4 + 16 * 4;   // without the version 2 / 3 fields
static const UInt32 kMaxFiles = 1 << 22;

static const unsigned kMethod_GodotAes = kMethod_Custom;

class CHandler Z7_final: public CHandlerBase
{
  CByteBuffer _key;    // 32 bytes, once asked
  bool _keyDefined;

  HRESULT Open2(IInStream *stream, IArchiveOpenCallback *callback) Z7_override;
  void Close2() Z7_override { _keyDefined = false; }
  HRESULT GetKey(IArchiveOpenCallback *callback);
public:
  CHandler(): _keyDefined(false) {}
  HRESULT DecodePiece(const CItem &item, const CPiece &piece, CByteBuffer &packed, Byte *dest) Z7_override;
  // decrypts FileAccessEncrypted data in place; (plainSize) out
  HRESULT Decrypt(Byte *data, size_t size, UInt64 &plainSize) const;
};

static int HexValue(wchar_t c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

HRESULT CHandler::GetKey(IArchiveOpenCallback *callback)
{
  if (_keyDefined)
    return S_OK;
  UString s;
  RINOK(AskPassword(callback, s))
  s.RemoveChar(L' ');
  if (s.Len() != 64)
    return S_FALSE;
  _key.Alloc(32);
  for (unsigned i = 0; i < 32; i++)
  {
    const int hi = HexValue(s[i * 2]), lo = HexValue(s[i * 2 + 1]);
    if (hi < 0 || lo < 0)
      return S_FALSE;
    _key[i] = (Byte)(hi * 16 + lo);
  }
  _keyDefined = true;
  return S_OK;
}

// AES-256 in CFB mode with 128-bit feedback, as mbedTLS does it for Godot
HRESULT CHandler::Decrypt(Byte *data, size_t size, UInt64 &plainSize) const
{
  if (!_keyDefined)
    return k_Hr_WrongPassword;
  if (size < 16 + 8 + 16)
    return k_Hr_DataError;
  Byte md5[16];
  memcpy(md5, data, 16);
  plainSize = GetUi64(data + 16);
  Byte *p = data + 16 + 8 + 16;
  const size_t encSize = size - (16 + 8 + 16);
  if (plainSize > encSize)
    return k_Hr_DataError;
  // 7-Zip's AES tables are set up by its own AES coder (MyAes.cpp)
  UInt32 aesBuf[AES_NUM_IVMRK_WORDS + 3];
  UInt32 *aes = (UInt32 *)(((size_t)aesBuf + 15) & ~(size_t)15);   // 16-byte aligned
  Aes_SetKey_Enc(aes + 4, _key, 32);
  Byte iv[16];
  memcpy(iv, data + 16 + 8, 16);
  for (size_t pos = 0; pos < encSize; pos += 16)
  {
    // E(iv) through CBC encoding of a zero block with (iv) as its IV
    Byte block[16];
    memset(block, 0, 16);
    AesCbc_Init(aes, iv);
    AesCbc_Encode(aes, block, 1);
    const size_t n = MyMin((size_t)16, encSize - pos);
    memcpy(iv, p + pos, n);
    for (size_t i = 0; i < n; i++)
      p[pos + i] ^= block[i];
  }
  CMd5 ctx;
  Md5_Init(&ctx);
  Md5_Update(&ctx, p, (size_t)plainSize);
  Byte digest[16];
  Md5_Final(&ctx, digest);
  if (memcmp(digest, md5, 16) != 0)
    return k_Hr_WrongPassword;
  memmove(data, p, (size_t)plainSize);
  return S_OK;
}

HRESULT CHandler::DecodePiece(const CItem &item, const CPiece &piece, CByteBuffer &packed, Byte *dest)
{
  if (piece.Method != kMethod_GodotAes)
    return CHandlerBase::DecodePiece(item, piece, packed, dest);
  UInt64 plain = 0;
  RINOK(Decrypt(packed, packed.Size(), plain))
  if (plain != piece.Size)
    return k_Hr_DataError;
  memcpy(dest, packed, (size_t)plain);
  return S_OK;
}

// the encrypted size of (plainSize) bytes of FileAccessEncrypted data
static UInt64 EncryptedSize(UInt64 plainSize)
{
  return 16 + 8 + 16 + ((plainSize + 15) & ~(UInt64)15);
}

HRESULT CHandler::Open2(IInStream *stream, IArchiveOpenCallback *callback)
{
  UInt64 fileSize = 0;
  RINOK(GetStreamSize(stream, fileSize))
  // the pack starts where the signature is: 0, or wherever the opener found it
  const UInt64 packStart = 0;
  Byte h[kHeaderSize + 4 + 8 + 8];
  if (fileSize < kHeaderSize)
    return S_FALSE;
  RINOK(ReadAt(stream, packStart, h, 4 + 4 * 4))
  if (memcmp(h, "GDPC", 4) != 0)
    return S_FALSE;
  const UInt32 version = GetUi32(h + 4);
  if (version < 1 || version > 3)
    return S_FALSE;
  const UInt32 vMajor = GetUi32(h + 8), vMinor = GetUi32(h + 12), vPatch = GetUi32(h + 16);
  UInt64 pos = packStart + 4 + 4 * 4;
  UInt32 packFlags = 0;
  UInt64 fileBase = packStart;
  UInt64 dirOffset = 0;
  if (version >= 2)
  {
    RINOK(ReadAt(stream, pos, h, 4 + 8))
    packFlags = GetUi32(h);
    fileBase = GetUi64(h + 4);
    pos += 4 + 8;
    if (version >= 3)
    {
      RINOK(ReadAt(stream, pos, h, 8))
      dirOffset = GetUi64(h);
      pos += 8;
    }
    if (packFlags & kFlag_RelFileBase)
      fileBase += packStart;
  }
  pos += 16 * 4;
  if (version >= 3)
    pos = packStart + dirOffset;

  _comment.Empty();
  {
    AString s ("Godot ");
    s.Add_UInt32(vMajor); s.Add_Dot(); s.Add_UInt32(vMinor); s.Add_Dot(); s.Add_UInt32(vPatch);
    s += ", pack format ";
    s.Add_UInt32(version);
    _comment = GetUnicodeString(s);
  }

  // the directory, decrypted first if it is encrypted
  CByteBuffer dirBuf;
  size_t dirPos = 0;
  CMyComPtr<IInStream> dirStream;
  if (packFlags & kFlag_DirEncrypted)
  {
    Byte eh[16 + 8];
    RINOK(ReadAt(stream, pos, eh, sizeof(eh)))
    const UInt64 plain = GetUi64(eh + 16);
    const UInt64 encSize = EncryptedSize(plain);
    if (plain > ((UInt64)1 << 30) || pos + encSize > fileSize)
      return S_FALSE;
    // no key or a wrong one: S_FALSE, and the opener says "Wrong password?"
    // as for any archive with encrypted headers
    const HRESULT k = GetKey(callback);
    if (k != S_OK)
      return k == E_ABORT ? E_ABORT : S_FALSE;
    dirBuf.Alloc((size_t)encSize);
    RINOK(ReadAt(stream, pos, dirBuf, (size_t)encSize))
    UInt64 plainSize = 0;
    if (Decrypt(dirBuf, dirBuf.Size(), plainSize) != S_OK)
    {
      _keyDefined = false;
      return S_FALSE;
    }
    if (plainSize < 4)
      return S_FALSE;
    dirPos = 0;
    dirBuf.ChangeSize_KeepData((size_t)plainSize, (size_t)plainSize);
  }
  else
  {
    // read the directory as it goes: its size isn't stored
    const UInt64 rest = fileSize - pos;
    const size_t want = (size_t)MyMin(rest, (UInt64)1 << 28);
    dirBuf.Alloc(want);
    size_t got = want;
    RINOK(InStream_SeekSet(stream, pos))
    RINOK(ReadStream(stream, dirBuf, &got))
    dirBuf.ChangeSize_KeepData(got, got);
  }

  const Byte *d = dirBuf;
  const size_t dSize = dirBuf.Size();
  if (dSize < 4)
    return S_FALSE;
  const UInt32 numFiles = GetUi32(d);
  dirPos = 4;
  if (numFiles > kMaxFiles)
    return S_FALSE;
  const size_t fixedSize = 8 + 8 + 16 + (version >= 2 ? 4 : 0);
  for (UInt32 i = 0; i < numFiles; i++)
  {
    if (dSize - dirPos < 4)
      return S_FALSE;
    const UInt32 len = GetUi32(d + dirPos);
    dirPos += 4;
    if (len > dSize - dirPos || dSize - dirPos - len < fixedSize)
      return S_FALSE;
    AString name;
    name.SetFrom_CalcLen((const char *)d + dirPos, len);
    dirPos += len;
    const UInt64 offset = GetUi64(d + dirPos);
    const UInt64 size = GetUi64(d + dirPos + 8);
    CItem item;
    item.CheckType = kCheck_None;
    UInt32 flags = 0;
    if (version >= 2)
      flags = GetUi32(d + dirPos + 32);
    dirPos += fixedSize;
    if (flags & kFileFlag_Removal)
      continue;

    if (name.IsPrefixedBy("res://"))
      name.DeleteFrontal(6);
    else if (name.IsPrefixedBy("user://"))
      name.DeleteFrontal(7);
    while (name.IsPrefixedBy("/"))
      name.DeleteFrontal(1);
    UString path;
    if (!ConvertUTF8ToUnicode(name, path))
      path = GetUnicodeString(name);
    item.Path = path;

    const UInt64 start = fileBase + offset;
    if (flags & kFileFlag_Encrypted)
    {
      item.Encrypted = true;
      item.Method = "AES-256";
      // the stored size is the plain one; the data is FileAccessEncrypted
      item.AddPiece(start, EncryptedSize(size), size, kMethod_GodotAes);
    }
    else
      item.SetStored(start, size);
    if (start > fileSize || item.PackSize > fileSize - start)
      _errorFlags |= kpv_ErrorFlags_UnexpectedEnd;
    _items.Add(item);
  }

  bool anyEncrypted = false;
  FOR_VECTOR (i, _items)
    if (_items[i].Encrypted)
      anyEncrypted = true;
  if (anyEncrypted && !_keyDefined)
  {
    const HRESULT k = GetKey(callback);
    if (k == E_ABORT)
      return E_ABORT;
  }

  UInt64 end = pos;
  FOR_VECTOR (i, _items)
  {
    const CItem &item = _items[i];
    if (!item.Pieces.IsEmpty())
    {
      const UInt64 e = item.Pieces[0].Offset + item.PackSize;
      if (e > end)
        end = e;
    }
  }
  _phySize = MyMin(end, fileSize);
  return S_OK;
}

static const Byte k_Signature[] = { 'G', 'D', 'P', 'C' };

REGISTER_ARC_I(
  "GodotPck", "pck", NULL, 0xB0,
  k_Signature,
  0,
  NArcInfoFlags::kFindSignature,
  NULL)

}}

// RpgMaker.cpp -- 7zfm-qt: RPG Maker's encrypted game data.
//
// RGSS (XP, VX, VX Ace): one archive, Game.rgssad / .rgss2a / .rgss3a.
//   "RGSSAD\0" and a version byte.
//   Version 1 (XP, VX): a key starting at 0xDEADCAFE steps k = 7k + 3. Each
//     entry is its name length (XORed with the key, then a step), the name
//     (each byte XORed with the key's low byte, a step after each), the
//     data size (XORed, a step), then the data, XORed 4 bytes at a time
//     (little endian) with the key as it is there, which steps after each
//     4 bytes; the entries' key goes on from before the data.
//   Version 3 (VX Ace): a seed; the key is seed * 9 + 3. Each entry is its
//     data offset, size, data key and name length, each XORed with the key,
//     then the name, its bytes XORed with the key's bytes in turn; an offset
//     of 0 ends the list. The data is XORed as in version 1, from its own key.
//   Names use '\'.
//
// RPG Maker MV and MZ: single files (.rpgmvp, .rpgmvo, .rpgmvm; MZ's .png_,
//   .ogg_, .m4a_): "RPGMV" and a 16-byte header, then the file with its
//   first 16 bytes XORed with the game's key ("encryptionKey" in the game's
//   data/System.json). An image's key follows from the PNG header it must
//   have; audio takes a key found before (see RememberKey), or asks for it
//   as 32 hex digits.
//
// After RGSS Decryptor and the RPG Maker MV decrypters (MIT licenses).

#include "Common/ComTry.h"
#include "Common/MyBuffer.h"
#include "Common/StringConvert.h"
#include "Common/UTFConvert.h"

#include "Windows/Synchronization.h"

#include "7zip/Common/RegisterArc.h"
#include "7zip/Common/StreamUtils.h"

#include "../common/ItemArchive.h"

#include <string.h>

namespace NArchive {
namespace NRgss {

using namespace NItemArc;

// the key (k0) after (n) steps of k = 7k + 3: affine maps composed by
// squaring, so a stream can start anywhere
static UInt32 KeyAfter(UInt32 k0, UInt64 n)
{
  UInt32 a = 1, b = 0;      // the result map: k -> a*k + b
  UInt32 pa = 7, pb = 3;    // the step map raised to 2^i
  while (n != 0)
  {
    if (n & 1)
    {
      // first the result so far, then this power
      b = pa * b + pb;
      a = pa * a;
    }
    pb = pa * pb + pb;
    pa = pa * pa;
    n >>= 1;
  }
  return a * k0 + b;
}

static void XorData(UInt32 fileKey, UInt64 pos, Byte *data, size_t size)
{
  UInt32 key = KeyAfter(fileKey, pos / 4);
  unsigned shift = (unsigned)(pos & 3);
  for (size_t i = 0; i < size; i++)
  {
    data[i] ^= (Byte)(key >> (8 * shift));
    if (++shift == 4)
    {
      shift = 0;
      key = key * 7 + 3;
    }
  }
}

class CHandler Z7_final: public CHandlerBase
{
  HRESULT Open2(IInStream *stream, IArchiveOpenCallback *callback) Z7_override;
public:
  void TransformStored(const CItem &item, unsigned, UInt64 pos, Byte *data, size_t size) Z7_override
    { XorData((UInt32)item.Aux, pos, data, size); }
};

static void AddName(CItem &item, const AString &raw)
{
  AString a = raw;
  a.Replace('\\', '/');
  UString u;
  // names are in the game's code page; UTF-8 in English games and VX Ace
  if (!ConvertUTF8ToUnicode(a, u))
    u = MultiByteToUnicodeString(a, CP_ACP);
  item.Path = u;
}

HRESULT CHandler::Open2(IInStream *stream, IArchiveOpenCallback *)
{
  UInt64 fileSize = 0;
  RINOK(GetStreamSize(stream, fileSize))
  Byte h[16];
  if (fileSize < 8)
    return S_FALSE;
  RINOK(ReadAt(stream, 0, h, 8))
  if (memcmp(h, "RGSSAD\0", 7) != 0)
    return S_FALSE;
  const unsigned version = h[7];
  UInt64 end = 8;
  if (version == 1)
  {
    UInt32 key = 0xDEADCAFE;
    UInt64 pos = 8;
    while (pos < fileSize)
    {
      if (fileSize - pos < 4)
      {
        _errorFlags |= kpv_ErrorFlags_UnexpectedEnd;
        break;
      }
      RINOK(ReadAt(stream, pos, h, 4))
      const UInt32 nameLen = GetUi32(h) ^ key;
      key = key * 7 + 3;
      pos += 4;
      if (nameLen == 0 || nameLen > 4096 || fileSize - pos < (UInt64)nameLen + 4)
      {
        _errorFlags |= kpv_ErrorFlags_HeadersError;
        break;
      }
      CByteBuffer name(nameLen);
      RINOK(ReadAt(stream, pos, name, nameLen))
      AString s;
      for (UInt32 i = 0; i < nameLen; i++)
      {
        s += (char)(name[i] ^ (Byte)key);
        key = key * 7 + 3;
      }
      pos += nameLen;
      RINOK(ReadAt(stream, pos, h, 4))
      const UInt32 size = GetUi32(h) ^ key;
      key = key * 7 + 3;
      pos += 4;
      CItem item;
      AddName(item, s);
      item.SetStored(pos, size);
      item.Aux = key;
      item.Method = "XOR";
      if (size > fileSize - pos)
        _errorFlags |= kpv_ErrorFlags_UnexpectedEnd;
      _items.Add(item);
      pos += size;
      end = pos;
    }
  }
  else if (version == 3)
  {
    RINOK(ReadAt(stream, 8, h, 4))
    const UInt32 key = GetUi32(h) * 9 + 3;
    UInt64 pos = 12;
    for (;;)
    {
      if (fileSize - pos < 16)
      {
        _errorFlags |= kpv_ErrorFlags_UnexpectedEnd;
        break;
      }
      RINOK(ReadAt(stream, pos, h, 16))
      pos += 16;
      const UInt32 offset = GetUi32(h) ^ key;
      if (offset == 0)
        break;
      const UInt32 size = GetUi32(h + 4) ^ key;
      const UInt32 fileKey = GetUi32(h + 8) ^ key;
      const UInt32 nameLen = GetUi32(h + 12) ^ key;
      if (nameLen == 0 || nameLen > 4096 || fileSize - pos < nameLen)
      {
        _errorFlags |= kpv_ErrorFlags_HeadersError;
        break;
      }
      CByteBuffer name(nameLen);
      RINOK(ReadAt(stream, pos, name, nameLen))
      pos += nameLen;
      AString s;
      for (UInt32 i = 0; i < nameLen; i++)
        s += (char)(name[i] ^ (Byte)(key >> (8 * (i & 3))));
      CItem item;
      AddName(item, s);
      item.SetStored(offset, size);
      item.Aux = fileKey;
      item.Method = "XOR";
      if (offset > fileSize || size > fileSize - offset)
        _errorFlags |= kpv_ErrorFlags_UnexpectedEnd;
      else if (offset + size > end)
        end = offset + size;
      _items.Add(item);
    }
    if (pos > end)
      end = pos;
  }
  else
    return S_FALSE;
  _phySize = MyMin(end, fileSize);
  _comment = (version == 1) ? "RGSSAD version 1 (XP, VX)" : "RGSSAD version 3 (VX Ace)";
  return S_OK;
}

static const Byte k_Signature[] = { 'R', 'G', 'S', 'S', 'A', 'D', 0 };

REGISTER_ARC_I(
  "RGSS", "rgssad rgss2a rgss3a", NULL, 0xAC,
  k_Signature,
  0,
  0,
  NULL)

}

// ---------- RPG Maker MV / MZ ----------

namespace NRpgMv {

using namespace NItemArc;

static const unsigned kHeaderSize = 16;
static const unsigned kKeySize = 16;
static const Byte kPngHead[16] =
  { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 0x0D, 'I', 'H', 'D', 'R' };

class CHandler Z7_final: public CHandlerBase
{
  HRESULT Open2(IInStream *stream, IArchiveOpenCallback *callback) Z7_override;
public:
  HRESULT DecodePiece(const CItem &, const CPiece &, CByteBuffer &, Byte *) Z7_override
    { return k_Hr_WrongPassword; }
};

static int HexValue(int c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static bool ParseKey(const char *s, Byte *key)
{
  for (unsigned i = 0; i < kKeySize; i++)
  {
    const int hi = HexValue(s[i * 2]);
    const int lo = hi < 0 ? -1 : HexValue(s[i * 2 + 1]);
    if (lo < 0)
      return false;
    key[i] = (Byte)(hi * 16 + lo);
  }
  return true;
}

// A game has one key; the opener can't read the game's data/System.json
// (it is above the file, and upstream opens only files beside or below an
// archive), so the keys found so far are kept: from an image's PNG header,
// or typed in. Audio takes one that turns its header into "OggS" (or an
// M4A's "ftyp").
static NWindows::NSynchronization::CCriticalSection g_KeysCs;
static Byte g_Keys[16][kKeySize];
static unsigned g_NumKeys;

static void RememberKey(const Byte *key)
{
  NWindows::NSynchronization::CCriticalSectionLock lock(g_KeysCs);
  for (unsigned i = 0; i < g_NumKeys; i++)
    if (memcmp(g_Keys[i], key, kKeySize) == 0)
      return;
  if (g_NumKeys == Z7_ARRAY_SIZE(g_Keys))
  {
    memmove(g_Keys[0], g_Keys[1], sizeof(g_Keys) - sizeof(g_Keys[0]));
    g_NumKeys--;
  }
  memcpy(g_Keys[g_NumKeys++], key, kKeySize);
}

static bool HeaderFits(const Byte *plain, const char *ext)
{
  if (!ext || strcmp(ext, "ogg") == 0)
    if (memcmp(plain, "OggS", 4) == 0)
      return true;
  if (!ext || strcmp(ext, "m4a") == 0)
    if (memcmp(plain + 4, "ftyp", 4) == 0)
      return true;
  return false;
}

static bool FindKey(const Byte *enc, size_t encSize, const char *ext, Byte *key)
{
  if (encSize < 8)
    return false;
  NWindows::NSynchronization::CCriticalSectionLock lock(g_KeysCs);
  for (unsigned k = g_NumKeys; k-- != 0;)
  {
    Byte plain[kKeySize];
    for (size_t i = 0; i < encSize; i++)
      plain[i] = enc[i] ^ g_Keys[k][i];
    if (HeaderFits(plain, ext))
    {
      memcpy(key, g_Keys[k], kKeySize);
      return true;
    }
  }
  return false;
}

static const char * const kExts[][2] =
{
  { "rpgmvp", "png" }, { "rpgmvo", "ogg" }, { "rpgmvm", "m4a" },
  { "png_", "png" }, { "ogg_", "ogg" }, { "m4a_", "m4a" }
};

HRESULT CHandler::Open2(IInStream *stream, IArchiveOpenCallback *callback)
{
  UInt64 fileSize = 0;
  RINOK(GetStreamSize(stream, fileSize))
  if (fileSize < kHeaderSize)
    return S_FALSE;
  Byte h[kHeaderSize + kKeySize];
  RINOK(ReadAt(stream, 0, h, kHeaderSize))
  if (memcmp(h, "RPGMV\0\0\0", 8) != 0)
    return S_FALSE;
  const size_t encSize = (size_t)MyMin(fileSize - kHeaderSize, (UInt64)kKeySize);
  memset(h + kHeaderSize, 0, kKeySize);
  RINOK(ReadAt(stream, kHeaderSize, h + kHeaderSize, encSize))
  const Byte *enc = h + kHeaderSize;

  // the decrypted file's name: the extension the engine loads it by
  UString name = GetArcName(callback);
  const char *ext = NULL;
  {
    const int dot = name.ReverseFind_Dot();
    if (dot >= 0)
      for (unsigned i = 0; i < Z7_ARRAY_SIZE(kExts); i++)
        if (StringsAreEqualNoCase_Ascii(name.Ptr((unsigned)dot + 1), kExts[i][0]))
        {
          ext = kExts[i][1];
          name.DeleteFrom((unsigned)dot + 1);
          name += ext;
          break;
        }
  }
  if (name.IsEmpty())
    name = "file";

  Byte key[kKeySize];
  bool haveKey = false;
  if (encSize == kKeySize && (!ext || strcmp(ext, "png") == 0))
  {
    // the PNG signature and IHDR are fixed: they give the key
    for (unsigned i = 0; i < kKeySize; i++)
      key[i] = enc[i] ^ kPngHead[i];
    haveKey = true;
    RememberKey(key);
  }
  else
    haveKey = FindKey(enc, encSize, ext, key);
  if (!haveKey)
  {
    UString s;
    const HRESULT r = AskPassword(callback, s);
    if (r == E_ABORT)
      return E_ABORT;
    s.RemoveChar(L' ');
    if (r == S_OK && s.Len() == kKeySize * 2)
    {
      AString a = UnicodeStringToMultiByte(s, CP_ACP);
      haveKey = ParseKey(a, key);
      if (haveKey)
        RememberKey(key);
    }
  }

  CItem item;
  item.Path = name;
  item.Method = "XOR";
  item.Inline.Alloc(encSize);
  if (haveKey)
  {
    for (size_t i = 0; i < encSize; i++)
      item.Inline[i] = enc[i] ^ key[i];
    item.AddPiece(0, encSize, encSize, kMethod_Inline);
  }
  else
  {
    item.Encrypted = true;
    item.AddPiece(kHeaderSize, encSize, encSize, kMethod_Custom);
  }
  if (fileSize > kHeaderSize + encSize)
    item.AddPiece(kHeaderSize + encSize, fileSize - kHeaderSize - encSize,
        fileSize - kHeaderSize - encSize, kMethod_Stored);
  _items.Add(item);
  _phySize = fileSize;
  return S_OK;
}

static const Byte k_Signature[] = { 'R', 'P', 'G', 'M', 'V', 0, 0, 0 };

REGISTER_ARC_I(
  "RPGMV", "rpgmvp rpgmvo rpgmvm png_ ogg_ m4a_", NULL, 0xAD,
  k_Signature,
  0,
  0,
  NULL)

}}

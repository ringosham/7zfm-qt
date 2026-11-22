// DiscUif.cpp -- 7zfm-qt: MagicISO UIF images. Written from the format as
// uif2iso (Luigi Auriemma) documents it; no code of it is used (it is GPL),
// the DES here is written from the standard (FIPS 46-3). Checked against
// images made by MagicISO 5.5, plain and with a password.
//
//   At the end of the file a 64-byte "bbis" header: version, image type (8:
//   an ISO), sector count and size, bytes unused in the last sector, the
//   offset of the block table, key settings. The block table ("blhr"): a
//   count and 24-byte entries (data offset, packed size, first sector,
//   sector count, type: 1 stored, 3 zeros, 5 compressed), zlib-compressed
//   up to version 2, LZMA (a filter byte, properties, size) from version 3,
//   which may also need the x86 branch filter.
//   With a password, a "bsdr" header stands where the table would: the
//   header encrypted again follows it, then the table; the key is the
//   password as 32 bytes, its four 64-bit words added together (DES, each
//   whole 8 bytes in ECB mode, of the table and every block). From version
//   2 a key setting may pick one of 16 keys built into MagicISO instead.
//
// Images of a CD with several tracks (type 9; MagicISO's "Compress CD/DVD
// Image" of a .cue, .mds, .ccd or .nrg) hold the image file itself: after
// the block table a "blms" block (the tracks, as MagicISO read them) and a
// "blss" block with the format the image came in (0 ISO, 1 CUE, 2 MDS, 3
// CCD, 4 NRG) and that format's sheet, the .cue for instance (for CCD: two
// sizes, the .ccd and the .sub). Both packed as the table is. The image
// shows as its file (a .bin opens at its tracks), the sheet beside it.
//
// Not done: MagicISO's other obfuscation (key setting 2).

#include "Common/ComTry.h"
#include "Common/MyBuffer.h"
#include "Common/StringConvert.h"

#include "7zip/Common/RegisterArc.h"
#include "7zip/Common/StreamUtils.h"

#include "C/Alloc.h"
#include "C/Bra.h"
#include "C/LzmaDec.h"

#include "DiscImage.h"
#include "../common/ItemArchive.h"

#include <zlib.h>

#include <algorithm>
#include <string.h>
#include <vector>

namespace NArchive {
namespace NDisc {
namespace NUif {

using NItemArc::GetUi32;
using NItemArc::GetUi64;

// ---------- DES (FIPS 46-3), decryption in ECB mode ----------

class CDes
{
  UInt64 _sub[16];
  static UInt64 Permute(UInt64 in, const Byte *table, unsigned n, unsigned inBits)
  {
    UInt64 out = 0;
    for (unsigned i = 0; i < n; i++)
      out = (out << 1) | ((in >> (inBits - table[i])) & 1);
    return out;
  }
public:
  void SetKey(const Byte *key)
  {
    static const Byte kPc1[56] = {
      57, 49, 41, 33, 25, 17, 9, 1, 58, 50, 42, 34, 26, 18, 10, 2, 59, 51, 43, 35, 27, 19, 11, 3, 60, 52, 44, 36,
      63, 55, 47, 39, 31, 23, 15, 7, 62, 54, 46, 38, 30, 22, 14, 6, 61, 53, 45, 37, 29, 21, 13, 5, 28, 20, 12, 4 };
    static const Byte kPc2[48] = {
      14, 17, 11, 24, 1, 5, 3, 28, 15, 6, 21, 10, 23, 19, 12, 4, 26, 8, 16, 7, 27, 20, 13, 2,
      41, 52, 31, 37, 47, 55, 30, 40, 51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32 };
    static const Byte kShifts[16] = { 1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1 };
    UInt64 k = 0;
    for (unsigned i = 0; i < 8; i++)
      k = (k << 8) | key[i];
    const UInt64 cd = Permute(k, kPc1, 56, 64);
    UInt32 c = (UInt32)(cd >> 28) & 0xFFFFFFF, d = (UInt32)cd & 0xFFFFFFF;
    for (unsigned i = 0; i < 16; i++)
    {
      for (unsigned s = 0; s < kShifts[i]; s++)
      {
        c = ((c << 1) | (c >> 27)) & 0xFFFFFFF;
        d = ((d << 1) | (d >> 27)) & 0xFFFFFFF;
      }
      _sub[i] = Permute(((UInt64)c << 28) | d, kPc2, 48, 56);
    }
  }
  void DecryptBlock(Byte *p) const
  {
    static const Byte kIp[64] = {
      58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4, 62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
      57, 49, 41, 33, 25, 17, 9, 1, 59, 51, 43, 35, 27, 19, 11, 3, 61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7 };
    static const Byte kFp[64] = {
      40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31, 38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
      36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27, 34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9, 49, 17, 57, 25 };
    static const Byte kE[48] = {
      32, 1, 2, 3, 4, 5, 4, 5, 6, 7, 8, 9, 8, 9, 10, 11, 12, 13, 12, 13, 14, 15, 16, 17,
      16, 17, 18, 19, 20, 21, 20, 21, 22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1 };
    static const Byte kP[32] = {
      16, 7, 20, 21, 29, 12, 28, 17, 1, 15, 23, 26, 5, 18, 31, 10, 2, 8, 24, 14, 32, 27, 3, 9, 19, 13, 30, 6, 22, 11, 4, 25 };
    static const Byte kS[8][64] = {
      { 14, 4, 13, 1, 2, 15, 11, 8, 3, 10, 6, 12, 5, 9, 0, 7, 0, 15, 7, 4, 14, 2, 13, 1, 10, 6, 12, 11, 9, 5, 3, 8,
        4, 1, 14, 8, 13, 6, 2, 11, 15, 12, 9, 7, 3, 10, 5, 0, 15, 12, 8, 2, 4, 9, 1, 7, 5, 11, 3, 14, 10, 0, 6, 13 },
      { 15, 1, 8, 14, 6, 11, 3, 4, 9, 7, 2, 13, 12, 0, 5, 10, 3, 13, 4, 7, 15, 2, 8, 14, 12, 0, 1, 10, 6, 9, 11, 5,
        0, 14, 7, 11, 10, 4, 13, 1, 5, 8, 12, 6, 9, 3, 2, 15, 13, 8, 10, 1, 3, 15, 4, 2, 11, 6, 7, 12, 0, 5, 14, 9 },
      { 10, 0, 9, 14, 6, 3, 15, 5, 1, 13, 12, 7, 11, 4, 2, 8, 13, 7, 0, 9, 3, 4, 6, 10, 2, 8, 5, 14, 12, 11, 15, 1,
        13, 6, 4, 9, 8, 15, 3, 0, 11, 1, 2, 12, 5, 10, 14, 7, 1, 10, 13, 0, 6, 9, 8, 7, 4, 15, 14, 3, 11, 5, 2, 12 },
      { 7, 13, 14, 3, 0, 6, 9, 10, 1, 2, 8, 5, 11, 12, 4, 15, 13, 8, 11, 5, 6, 15, 0, 3, 4, 7, 2, 12, 1, 10, 14, 9,
        10, 6, 9, 0, 12, 11, 7, 13, 15, 1, 3, 14, 5, 2, 8, 4, 3, 15, 0, 6, 10, 1, 13, 8, 9, 4, 5, 11, 12, 7, 2, 14 },
      { 2, 12, 4, 1, 7, 10, 11, 6, 8, 5, 3, 15, 13, 0, 14, 9, 14, 11, 2, 12, 4, 7, 13, 1, 5, 0, 15, 10, 3, 9, 8, 6,
        4, 2, 1, 11, 10, 13, 7, 8, 15, 9, 12, 5, 6, 3, 0, 14, 11, 8, 12, 7, 1, 14, 2, 13, 6, 15, 0, 9, 10, 4, 5, 3 },
      { 12, 1, 10, 15, 9, 2, 6, 8, 0, 13, 3, 4, 14, 7, 5, 11, 10, 15, 4, 2, 7, 12, 9, 5, 6, 1, 13, 14, 0, 11, 3, 8,
        9, 14, 15, 5, 2, 8, 12, 3, 7, 0, 4, 10, 1, 13, 11, 6, 4, 3, 2, 12, 9, 5, 15, 10, 11, 14, 1, 7, 6, 0, 8, 13 },
      { 4, 11, 2, 14, 15, 0, 8, 13, 3, 12, 9, 7, 5, 10, 6, 1, 13, 0, 11, 7, 4, 9, 1, 10, 14, 3, 5, 12, 2, 15, 8, 6,
        1, 4, 11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5, 9, 2, 6, 11, 13, 8, 1, 4, 10, 7, 9, 5, 0, 15, 14, 2, 3, 12 },
      { 13, 2, 8, 4, 6, 15, 11, 1, 10, 9, 3, 14, 5, 0, 12, 7, 1, 15, 13, 8, 10, 3, 7, 4, 12, 5, 6, 11, 0, 14, 9, 2,
        7, 11, 4, 1, 9, 12, 14, 2, 0, 6, 10, 13, 15, 3, 5, 8, 2, 1, 14, 7, 4, 10, 8, 13, 15, 12, 9, 0, 3, 5, 6, 11 } };
    UInt64 b = 0;
    for (unsigned i = 0; i < 8; i++)
      b = (b << 8) | p[i];
    b = Permute(b, kIp, 64, 64);
    UInt32 l = (UInt32)(b >> 32), r = (UInt32)b;
    for (int i = 15; i >= 0; i--)
    {
      const UInt64 x = Permute(r, kE, 48, 32) ^ _sub[i];
      UInt32 s = 0;
      for (unsigned j = 0; j < 8; j++)
      {
        const unsigned six = (unsigned)(x >> (42 - 6 * j)) & 0x3F;
        const unsigned row = ((six >> 4) & 2) | (six & 1), col = (six >> 1) & 0xF;
        s = (s << 4) | kS[j][row * 16 + col];
      }
      const UInt32 f = (UInt32)Permute(s, kP, 32, 32);
      const UInt32 t = l ^ f;
      l = r;
      r = t;
    }
    b = Permute(((UInt64)r << 32) | l, kFp, 64, 64);
    for (int i = 7; i >= 0; i--)
    {
      p[i] = (Byte)b;
      b >>= 8;
    }
  }
  // every whole 8 bytes; a tail of fewer stays as it is
  void Decrypt(Byte *p, size_t size) const
  {
    for (size_t i = 0; i + 8 <= size; i += 8)
      DecryptBlock(p + i);
  }
};

// ---------- the handler ----------

struct CEntry
{
  UInt64 Offset;
  UInt32 PackSize;
  UInt32 Sector;
  UInt32 Count;     // sectors
  UInt32 Type;
};

class CHandler Z7_final: public CBlockHandler
{
  std::vector<CEntry> _entries;   // by sector
  UInt32 _version;
  bool _encrypted;
  CDes _des;
  int _cached;
  CByteBuffer _cache;
  CByteBuffer _packed;

  HRESULT Open2(IInStream *stream, IArchiveOpenCallback *callback) Z7_override;
  void Close2() Z7_override { _entries.clear(); _cached = -1; _encrypted = false; _version = 0; }
  HRESULT Decode(const Byte *in, size_t inSize, Byte *out, size_t outSize) const;
  HRESULT ReadPacked(IInStream *stream, UInt64 pos, UInt32 packSize, bool compressed, UInt32 size, CByteBuffer &out);
  void SetKey(const char *pw, unsigned len);
public:
  CHandler(): _version(0), _encrypted(false), _cached(-1) {}
  HRESULT ReadBlock(UInt64 index, Byte *dest) Z7_override;
};

// the password, at most 31 bytes, as 32 bytes; its four 64-bit words added
void CHandler::SetKey(const char *pw, unsigned len)
{
  Byte key[32];
  memset(key, 0, sizeof(key));
  memcpy(key, pw, MyMin(len, (unsigned)31));
  UInt64 k = GetUi64(key);
  for (unsigned i = 1; i < 4; i++)
    k += GetUi64(key + i * 8);
  for (unsigned i = 0; i < 8; i++)
    key[i] = (Byte)(k >> (8 * i));
  _des.SetKey(key);
  _encrypted = true;
}

HRESULT CHandler::Decode(const Byte *in, size_t inSize, Byte *out, size_t outSize) const
{
  if (_version < 3)
  {
    z_stream z;
    memset(&z, 0, sizeof(z));
    if (inflateInit(&z) != Z_OK)
      return E_OUTOFMEMORY;
    z.next_in = (Bytef *)in;
    z.avail_in = (uInt)inSize;
    z.next_out = out;
    z.avail_out = (uInt)outSize;
    const int r = inflate(&z, Z_SYNC_FLUSH);
    const size_t done = outSize - z.avail_out;
    inflateEnd(&z);
    if (r != Z_STREAM_END && !(r == Z_OK && done == outSize))
      return S_FALSE;
    memset(out + done, 0, outSize - done);
    return S_OK;
  }
  // a filter byte, LZMA properties, the size (known already), the stream
  if (inSize < 1 + LZMA_PROPS_SIZE + 8)
    return S_FALSE;
  const bool x86 = in[0] != 0;
  SizeT outLen = outSize, inLen = inSize - (1 + LZMA_PROPS_SIZE + 8);
  ELzmaStatus status;
  const SRes r = LzmaDecode(out, &outLen, in + 1 + LZMA_PROPS_SIZE + 8, &inLen, in + 1, LZMA_PROPS_SIZE,
      LZMA_FINISH_END, &status, &g_Alloc);
  if (r != SZ_OK || (status != LZMA_STATUS_FINISHED_WITH_MARK && status != LZMA_STATUS_MAYBE_FINISHED_WITHOUT_MARK))
    return S_FALSE;
  if (x86)
  {
    UInt32 state = Z7_BRANCH_CONV_ST_X86_STATE_INIT_VAL;
    z7_BranchConvSt_X86_Dec(out, outLen, 0, &state);
  }
  memset(out + outLen, 0, outSize - outLen);
  return S_OK;
}

// a block of the header area: (packSize) bytes at (pos), (size) once unpacked
HRESULT CHandler::ReadPacked(IInStream *stream, UInt64 pos, UInt32 packSize, bool compressed, UInt32 size, CByteBuffer &out)
{
  if (size > (1 << 24) || packSize > (1 << 26))
    return S_FALSE;
  CByteBuffer raw(packSize);
  RINOK(NItemArc::ReadAt(stream, pos, raw, packSize))
  out.Alloc(size);
  if (!compressed)
  {
    if (packSize < size)
      return S_FALSE;
    memcpy(out, raw, size);
    return S_OK;
  }
  if (_encrypted)
    _des.Decrypt(raw, raw.Size());
  return Decode(raw, raw.Size(), out, size);
}

static UString BaseOf(const UString &name)
{
  const int dot = name.ReverseFind_Dot();
  return dot > 0 ? name.Left((unsigned)dot) : name;
}

HRESULT CHandler::Open2(IInStream *stream, IArchiveOpenCallback *callback)
{
  Close2();
  UInt64 fileSize;
  RINOK(NItemArc::GetStreamSize(stream, fileSize))
  if (fileSize < 64 + 16)
    return S_FALSE;
  Byte b[64];
  RINOK(NItemArc::ReadAt(stream, fileSize - 64, b, 64))
  if (memcmp(b, "bbis", 4) != 0)
    return S_FALSE;
  UInt64 blhrPos = GetUi64(b + 28);
  Byte h[16];
  if (blhrPos > fileSize - 16)
    return S_FALSE;
  RINOK(NItemArc::ReadAt(stream, blhrPos, h, 16))
  if (memcmp(h, "bsdr", 4) == 0)
  {
    // a password: the real header follows, encrypted
    UString pw;
    const HRESULT r = NItemArc::AskPassword(callback, pw);
    if (r == E_ABORT)
      return E_ABORT;
    if (r != S_OK)
      return S_FALSE;
    const AString a = UnicodeStringToMultiByte(pw, CP_ACP);
    SetKey(a.Ptr(), a.Len());
    Byte e[64];
    RINOK(NItemArc::ReadAt(stream, blhrPos + 8, e, 64))
    _des.Decrypt(e, 64);
    if (memcmp(e, "bbis", 4) != 0)
      return S_FALSE;   // a wrong password
    memcpy(e + 40, b + 40, 16);   // the hash is the outer header's
    memcpy(b, e, 64);
    blhrPos = GetUi64(b + 28) + 64 + 8;
    if (blhrPos > fileSize - 16)
      return S_FALSE;
    RINOK(NItemArc::ReadAt(stream, blhrPos, h, 16))
  }
  _version = GetUi32(b + 8) & 0xFFFF;
  const unsigned imageType = GetUi32(b + 8) >> 16;
  const UInt32 sectors = GetUi32(b + 16);
  const UInt32 sectorSize = GetUi32(b + 20);
  const UInt32 lastDiff = GetUi32(b + 24);
  const Byte key1 = b[56], key2 = b[57];
  if ((imageType != 8 && imageType != 9) || sectorSize == 0 || sectorSize > (1 << 16))
    return S_FALSE;
  if (_version >= 2)
  {
    if (key2 == 2)
      return S_FALSE;   // MagicISO's own obfuscation: not supported
    if (key1 != 0 && key1 <= 0x10)
    {
      // one of MagicISO's built-in keys, in place of any password
      static const char * const kFixedKeys[16] = {
        "FAngS)snOt32",  "AglEy29TarsI;",     "bum87OrYx*THeCa", "ARMy67sabot&",
        "FoOTs:blaZe70", "panIc+elD%self79",  "Usnea*hest98",    "apex(TUft!BLOKE70",
        "sword18rEpP}",  "ARb10naVY=Rouse",   "PAIR18gAG:swAYs", "gums}Box73yANg",
        "naVal45drain]", "Cams42hEt83faiLs)", "rIaNt$Notch5",    "ExaCT&art*MEteS47" };
      const char *k = kFixedKeys[(key1 - 1) & 15];
      SetKey(k, (unsigned)strlen(k));
    }
  }
  if (memcmp(h, "blhr", 4) != 0)
    return S_FALSE;
  const UInt32 tableSize = GetUi32(h + 4);
  const bool compressed = GetUi32(h + 8) != 0;
  const UInt32 num = GetUi32(h + 12);
  if (num == 0 || num > (1 << 24) || tableSize < 8 || tableSize - 8 > fileSize)
    return S_FALSE;
  CByteBuffer table;
  if (ReadPacked(stream, blhrPos + 16, tableSize - 8, compressed, num * 24, table) != S_OK)
    return S_FALSE;
  for (UInt32 i = 0; i < num; i++)
  {
    const Byte *p = table + (size_t)i * 24;
    CEntry e;
    e.Offset = GetUi64(p);
    e.PackSize = GetUi32(p + 8);
    e.Sector = GetUi32(p + 12);
    e.Count = GetUi32(p + 16);
    e.Type = GetUi32(p + 20);
    if (e.Count == 0)
      continue;
    if (e.Offset > fileSize || e.PackSize > fileSize - e.Offset || (UInt64)e.Count * sectorSize > ((UInt64)1 << 28))
      return S_FALSE;
    _entries.push_back(e);
  }
  std::sort(_entries.begin(), _entries.end(), [](const CEntry &x, const CEntry &y) { return x.Sector < y.Sector; });
  _stream = stream;
  _blockSize = sectorSize;
  _size = (UInt64)sectors * sectorSize;
  if (lastDiff && lastDiff < sectorSize)
    _size -= sectorSize - lastDiff;
  _phySize = fileSize;
  UString base = BaseOf(GetArcName(callback));
  if (base.IsEmpty())
    base = "image";
  _name = base + L".iso";
  if (imageType == 9)
  {
    // the tracks ("blms"), then the image's own format and sheet ("blss")
    _name = base + L".bin";
    const UInt64 msPos = blhrPos + 8 + tableSize;
    Byte m[16];
    if (msPos + 16 <= fileSize && NItemArc::ReadAt(stream, msPos, m, 16) == S_OK && memcmp(m, "blms", 4) == 0
        && GetUi32(m + 4) >= 8)
    {
      const UInt64 ssPos = msPos + 8 + GetUi32(m + 4);
      Byte t[20];
      if (ssPos + 20 <= fileSize && NItemArc::ReadAt(stream, ssPos, t, 20) == S_OK && memcmp(t, "blss", 4) == 0
          && GetUi32(t + 4) >= 12)
      {
        const UInt32 format = GetUi32(t + 16);
        CByteBuffer sheet;
        const UInt32 sheetSize = GetUi32(t + 12);
        if (sheetSize != 0 && ReadPacked(stream, ssPos + 20, GetUi32(t + 4) - 12, GetUi32(t + 8) != 0, sheetSize, sheet) != S_OK)
          return S_FALSE;
        static const char * const kExt[5] = { "iso", "bin", "mdf", "img", "nrg" };
        if (format < 5)
          _name = base + L'.' + UString(kExt[format]);
        if (format == 1 && sheet.Size() != 0)
        {
          // the .bin as the .cue names it, if it names one file
          AString cue;
          cue.SetFrom_CalcLen((const char *)(const Byte *)sheet, (unsigned)sheet.Size());
          const int f = cue.Find("FILE \"");
          if (f >= 0 && cue.Find("FILE \"", (unsigned)f + 1) < 0)
          {
            const int end = cue.Find('"', (unsigned)f + 6);
            if (end > f + 6)
            {
              AString file = cue.Mid((unsigned)f + 6, (unsigned)end - (unsigned)f - 6);
              const int slash = MyMax(file.ReverseFind('\\'), file.ReverseFind('/'));
              if (slash >= 0)
                file.DeleteFrontal((unsigned)slash + 1);
              if (!file.IsEmpty())
              {
                _name = MultiByteToUnicodeString(file, CP_ACP);
                base = BaseOf(_name);
              }
            }
          }
          CExtra &e = _extras.AddNew();
          e.Name = base + L".cue";
          e.Data = sheet;
        }
        else if (format == 2 && sheet.Size() != 0)
        {
          CExtra &e = _extras.AddNew();
          e.Name = base + L".mds";
          e.Data = sheet;
        }
        else if (format == 3 && sheet.Size() >= 8)
        {
          const UInt32 ccd = GetUi32(sheet), sub = GetUi32(sheet + 4);
          if (ccd <= sheet.Size() - 8 && sub <= sheet.Size() - 8 - ccd)
          {
            CExtra &e = _extras.AddNew();
            e.Name = base + L".ccd";
            e.Data.CopyFrom(sheet + 8, ccd);
            if (sub != 0)
            {
              CExtra &e2 = _extras.AddNew();
              e2.Name = base + L".sub";
              e2.Data.CopyFrom(sheet + 8 + ccd, sub);
            }
          }
        }
      }
    }
  }
  _method = _version < 3 ? "Deflate" : "LZMA";
  return S_OK;
}

HRESULT CHandler::ReadBlock(UInt64 index, Byte *dest)
{
  const UInt64 sector = index;
  // the entry holding this sector
  size_t lo = 0, hi = _entries.size();
  while (lo < hi)
  {
    const size_t mid = (lo + hi) / 2;
    if (_entries[mid].Sector <= sector)
      lo = mid + 1;
    else
      hi = mid;
  }
  if (lo == 0 || sector >= (UInt64)_entries[lo - 1].Sector + _entries[lo - 1].Count)
  {
    memset(dest, 0, _blockSize);   // not in the table: empty
    return S_OK;
  }
  const size_t ei = lo - 1;
  const CEntry &e = _entries[ei];
  const size_t outSize = (size_t)e.Count * _blockSize;
  if (_cached != (int)ei)
  {
    _cached = -1;
    _cache.Alloc(outSize);
    if (e.Type == 3 || e.PackSize == 0)
      memset(_cache, 0, outSize);
    else
    {
      _packed.Alloc(e.PackSize);
      RINOK(NItemArc::ReadAt(_stream, e.Offset, _packed, e.PackSize))
      if (_encrypted)
        _des.Decrypt(_packed, e.PackSize);
      if (e.Type == 1)
      {
        if (e.PackSize > outSize)
          return S_FALSE;
        memcpy(_cache, _packed, e.PackSize);
        memset((Byte *)_cache + e.PackSize, 0, outSize - e.PackSize);
      }
      else if (e.Type == 5)
      {
        if (Decode(_packed, e.PackSize, _cache, outSize) != S_OK)
          return S_FALSE;
      }
      else
        return S_FALSE;
    }
    _cached = (int)ei;
  }
  memcpy(dest, (const Byte *)_cache + (size_t)(sector - e.Sector) * _blockSize, _blockSize);
  return S_OK;
}

REGISTER_ARC_I_NO_SIG(
  "UIF", "uif", NULL, 0xB4,
  0,
  NArcInfoFlags::kByExtOnlyOpen,
  NULL)

}}}

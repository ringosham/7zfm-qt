// DiscPowerIso.cpp -- 7zfm-qt: PowerISO DAA and gBurner GBI images, also
// split into .part01.daa, .part02.daa ... or .d00, .d01 ... Written from
// the format as daa2iso (Luigi Auriemma) documents it; no code of it is
// used (it is GPL). Checked against images made by PowerISO 1.6 for Linux
// and PowerISO 9.5 for Windows (piso.exe).
//
//   A 76-byte header: "DAA" (or "GBI"; a later volume says "DAA VOL"), the
//   offset of the chunk table, the version (0x100 or 0x110), the offset of
//   the chunks, the chunk size, the image size and the archive size, 16
//   bytes of settings and a CRC. Between the header and the table: tagged
//   blocks (1: split archive, 3: password). The image is cut in chunks of
//   the chunk size, each compressed on its own:
//   0x100  the table holds 3 bytes a chunk (its packed size); deflate
//   0x110  the table is a bit stream: a packed size and a type per chunk,
//          bit widths in the settings; the type says LZMA (properties in
//          the settings, an x86 branch filter if a setting says so) or
//          deflate, whose block types PowerISO may number differently
//          (the settings say how); a chunk as large as the chunk size is
//          stored. Settings may also obfuscate the table: by bytes (a
//          running subtraction keyed by the image size) and by values (an
//          XOR with a fixed 8-byte key); gBurner's GBI by bytes, keyed by
//          the header CRC.
//   Volumes after the first continue the chunks after their own header.
//
// Password-protected images are not supported.

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

#include <stdio.h>
#include <string.h>

#include <vector>

namespace NArchive {
namespace NDisc {
namespace NDaa {

using NItemArc::GetUi32;
using NItemArc::GetUi64;

// ---------- deflate whose block types may be numbered differently ----------

class CInflater
{
  const Byte *_in;
  size_t _inSize, _inPos;
  UInt32 _bitBuf;
  unsigned _bitCount;
  Byte *_out;
  size_t _outSize, _outPos;
  bool _error;

  UInt32 Bits(unsigned n)
  {
    while (_bitCount < n)
    {
      if (_inPos >= _inSize)
      {
        _error = true;
        return 0;
      }
      _bitBuf |= (UInt32)_in[_inPos++] << _bitCount;
      _bitCount += 8;
    }
    const UInt32 v = _bitBuf & ((n == 32) ? 0xFFFFFFFF : (((UInt32)1 << n) - 1));
    _bitBuf >>= n;
    _bitCount -= n;
    return v;
  }

  struct CHuff
  {
    UInt16 Count[16];
    UInt16 Symbol[288];
  };

  static bool Build(CHuff &h, const Byte *lengths, unsigned n)
  {
    memset(h.Count, 0, sizeof(h.Count));
    for (unsigned i = 0; i < n; i++)
      h.Count[lengths[i]]++;
    h.Count[0] = 0;
    int left = 1;
    for (unsigned len = 1; len < 16; len++)
    {
      left <<= 1;
      left -= h.Count[len];
      if (left < 0)
        return false;
    }
    UInt16 offs[16];
    offs[1] = 0;
    for (unsigned len = 1; len < 15; len++)
      offs[len + 1] = (UInt16)(offs[len] + h.Count[len]);
    for (unsigned i = 0; i < n; i++)
      if (lengths[i] != 0)
        h.Symbol[offs[lengths[i]]++] = (UInt16)i;
    return true;
  }

  int Decode(const CHuff &h)
  {
    int code = 0, first = 0, index = 0;
    for (unsigned len = 1; len < 16; len++)
    {
      code |= (int)Bits(1);
      if (_error)
        return -1;
      const int count = h.Count[len];
      if (code - count < first)
        return h.Symbol[index + (code - first)];
      index += count;
      first += count;
      first <<= 1;
      code <<= 1;
    }
    return -1;
  }

  bool Codes(const CHuff &lit, const CHuff &dist)
  {
    static const UInt16 kLenBase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
    static const Byte kLenExtra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
    static const UInt16 kDistBase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
    static const Byte kDistExtra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
    for (;;)
    {
      int sym = Decode(lit);
      if (sym < 0)
        return false;
      if (sym < 256)
      {
        if (_outPos >= _outSize)
          return false;
        _out[_outPos++] = (Byte)sym;
        continue;
      }
      if (sym == 256)
        return true;
      sym -= 257;
      if (sym >= 29)
        return false;
      const unsigned len = kLenBase[sym] + Bits(kLenExtra[sym]);
      const int ds = Decode(dist);
      if (ds < 0 || ds >= 30)
        return false;
      const size_t d = kDistBase[ds] + Bits(kDistExtra[ds]);
      if (_error || d > _outPos || len > _outSize - _outPos)
        return false;
      for (unsigned i = 0; i < len; i++, _outPos++)
        _out[_outPos] = _out[_outPos - d];
    }
  }

  bool Fixed()
  {
    Byte lengths[288 + 30];
    unsigned i = 0;
    for (; i < 144; i++) lengths[i] = 8;
    for (; i < 256; i++) lengths[i] = 9;
    for (; i < 280; i++) lengths[i] = 7;
    for (; i < 288; i++) lengths[i] = 8;
    CHuff lit, dist;
    Build(lit, lengths, 288);
    for (i = 0; i < 30; i++) lengths[i] = 5;
    Build(dist, lengths, 30);
    return Codes(lit, dist);
  }

  bool Dynamic()
  {
    static const Byte kOrder[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    const unsigned nlen = Bits(5) + 257, ndist = Bits(5) + 1, ncode = Bits(4) + 4;
    if (_error || nlen > 286 || ndist > 30)
      return false;
    Byte lengths[320];
    memset(lengths, 0, sizeof(lengths));
    for (unsigned i = 0; i < ncode; i++)
      lengths[kOrder[i]] = (Byte)Bits(3);
    CHuff lencode, dist;
    if (!Build(lencode, lengths, 19))
      return false;
    unsigned index = 0;
    while (index < nlen + ndist)
    {
      int sym = Decode(lencode);
      if (sym < 0)
        return false;
      if (sym < 16)
      {
        lengths[index++] = (Byte)sym;
        continue;
      }
      unsigned len = 0, rep;
      if (sym == 16)
      {
        if (index == 0)
          return false;
        len = lengths[index - 1];
        rep = 3 + Bits(2);
      }
      else if (sym == 17)
        rep = 3 + Bits(3);
      else
        rep = 11 + Bits(7);
      if (index + rep > nlen + ndist)
        return false;
      while (rep--)
        lengths[index++] = (Byte)len;
    }
    CHuff lit;
    if (!Build(lit, lengths, nlen) || !Build(dist, lengths + nlen, ndist))
      return false;
    return Codes(lit, dist);
  }

  bool Stored()
  {
    _bitBuf = 0;
    _bitCount = 0;
    if (_inSize - _inPos < 4)
      return false;
    const unsigned len = _in[_inPos] | ((unsigned)_in[_inPos + 1] << 8);
    _inPos += 4;
    if (_inSize - _inPos < len || _outSize - _outPos < len)
      return false;
    memcpy(_out + _outPos, _in + _inPos, len);
    _inPos += len;
    _outPos += len;
    return true;
  }

public:
  // (types): which block type number means stored, fixed, dynamic
  bool Run(const Byte *in, size_t inSize, Byte *out, size_t outSize, const unsigned types[3], size_t &written)
  {
    _in = in; _inSize = inSize; _inPos = 0;
    _bitBuf = 0; _bitCount = 0;
    _out = out; _outSize = outSize; _outPos = 0;
    _error = false;
    bool last = false;
    while (!last)
    {
      last = Bits(1) != 0;
      const unsigned type = Bits(2);
      if (_error)
        return false;
      bool ok;
      if (type == types[0]) ok = Stored();
      else if (type == types[1]) ok = Fixed();
      else if (type == types[2]) ok = Dynamic();
      else ok = false;
      if (!ok || _error)
        return false;
    }
    written = _outPos;
    return true;
  }
};

// ---------- the handler ----------

struct CVolume
{
  CMyComPtr<IInStream> Stream;
  UInt64 Start;     // where its part of the data starts
  UInt64 End;
  UInt64 Pos;       // in the data of all volumes
};

class CHandler Z7_final: public CBlockHandler
{
  std::vector<CVolume> _volumes;
  std::vector<UInt64> _offsets;     // of the chunks, in the data of all volumes
  std::vector<Byte> _types;         // 0 LZMA, 1 deflate, 2 stored
  unsigned _inflateTypes[3];
  bool _x86;
  Byte _lzmaProps[5];
  CByteBuffer _packed;

  HRESULT ReadData(UInt64 pos, Byte *data, size_t size);
  HRESULT Open2(IInStream *stream, IArchiveOpenCallback *callback) Z7_override;
  void Close2() Z7_override { _volumes.clear(); _offsets.clear(); _types.clear(); }
public:
  HRESULT ReadBlock(UInt64 index, Byte *dest) Z7_override;
};

HRESULT CHandler::ReadData(UInt64 pos, Byte *data, size_t size)
{
  for (size_t v = 0; v < _volumes.size() && size != 0; v++)
  {
    const CVolume &vol = _volumes[v];
    const UInt64 len = vol.End - vol.Start;
    if (pos >= vol.Pos + len)
      continue;
    const UInt64 in = pos - vol.Pos;
    const size_t n = (size_t)MyMin((UInt64)size, len - in);
    RINOK(NItemArc::ReadAt(vol.Stream, vol.Start + in, data, n))
    data += n;
    size -= n;
    pos += n;
  }
  return size == 0 ? S_OK : S_FALSE;
}

static UInt32 ReadBitsLe(const Byte *p, size_t size, UInt64 bitPos, unsigned bits)
{
  UInt32 v = 0;
  for (unsigned i = 0; i < bits; i++, bitPos++)
  {
    const size_t byte = (size_t)(bitPos >> 3);
    if (byte >= size)
      break;
    v |= (UInt32)((p[byte] >> (bitPos & 7)) & 1) << i;
  }
  return v;
}

HRESULT CHandler::Open2(IInStream *stream, IArchiveOpenCallback *callback)
{
  Byte h[76];
  RINOK(NItemArc::ReadAt(stream, 0, h, 76))
  bool gbi;
  if (memcmp(h, "DAA\0\0\0\0\0\0\0\0\0\0\0\0\0", 16) == 0)
    gbi = false;
  else if (memcmp(h, "GBI\0\0\0\0\0\0\0\0\0\0\0\0\0", 16) == 0)
    gbi = true;
  else
    return S_FALSE;
  UInt32 tableOffset = GetUi32(h + 16);
  const UInt32 version = GetUi32(h + 20);
  UInt32 dataOffset = GetUi32(h + 24);
  UInt32 chunkField = GetUi32(h + 36);
  const UInt64 isoSize = GetUi64(h + 40);
  const UInt64 daaSize = GetUi64(h + 48);
  const Byte *hd = h + 56;
  const UInt32 crc = GetUi32(h + 72);
  if (version != 0x100 && version != 0x110)
    return S_FALSE;
  UInt32 chunkSize;
  unsigned bitType = 0, bitSize = 0;
  bool lameBits = false, lameTable = false, lzma = false, packedTable = false;
  unsigned btypeSet = 0;
  if (version == 0x100)
    chunkSize = chunkField;
  else
  {
    const UInt32 x = dataOffset, y = chunkField;
    dataOffset &= 0xFFFFFF;
    chunkSize = (y & 0xFFF) << 14;
    bitType = hd[5] & 7;
    bitSize = hd[5] >> 3;
    if (bitSize)
      bitSize += 10;
    else
      for (UInt32 len = chunkSize; len > bitType; len >>= 1)
        bitSize++;
    packedTable = (y & 0x4000) != 0;
    lameBits = (y & 0x20000) != 0;
    lameTable = (y & 0x8000000) != 0;
    lzma = (y & 0x100000) != 0;
    btypeSet = (y >> 23) & 3;
    if (gbi)
      btypeSet ^= 1;
    _x86 = lzma && hd[6] != 0;
    memcpy(_lzmaProps, hd + 7, 5);
    (void)x;
  }
  static const unsigned kSets[4][3] = { { 0, 1, 2 }, { 1, 2, 0 }, { 0, 2, 1 }, { 1, 0, 2 } };
  memcpy(_inflateTypes, kSets[btypeSet], sizeof(_inflateTypes));
  if (chunkSize == 0 || chunkSize > (1 << 26) || dataOffset < tableOffset || isoSize == 0)
    return S_FALSE;

  // the tagged blocks: a password one means encrypted
  {
    UInt64 pos = 76;
    while (pos + 8 <= tableOffset)
    {
      Byte t[8];
      RINOK(NItemArc::ReadAt(stream, pos, t, 8))
      const UInt32 type = GetUi32(t), len = GetUi32(t + 4);
      if (type == 3)
        return S_FALSE;          // password protected: not supported
      if (len < 8)
        break;
      pos += len;
    }
  }

  // the volumes: this, then .part02.daa / .part002.daa / .d00 ...
  UInt64 fileSize;
  RINOK(NItemArc::GetStreamSize(stream, fileSize))
  {
    CVolume v;
    v.Stream = stream;
    v.Start = 0;
    v.End = fileSize;
    v.Pos = 0;
    _volumes.push_back(v);
  }
  if (fileSize != daaSize)
  {
    const UString name = NItemArc::GetArcName(callback);
    UString base;
    int digits = 0;
    unsigned first = 0;
    const int dot = name.ReverseFind_Dot();
    if (dot > 0)
    {
      UString stem = name.Left((unsigned)dot);
      if (stem.Len() > 3 && stem.Back() == L'1' && (stem[stem.Len() - 2] == L'0'))
      {
        unsigned n = 0;
        while (n < stem.Len() && stem[stem.Len() - 1 - n] >= L'0' && stem[stem.Len() - 1 - n] <= L'9')
          n++;
        digits = (int)n;
        base = stem.Left(stem.Len() - n);
        first = 2;
      }
      else
      {
        base = stem;
        digits = -1;   // .d00, .d01 ...
        first = 0;
      }
    }
    for (unsigned k = first; k < first + 999; k++)
    {
      UString vn = base;
      if (digits > 0)
      {
        char buf[16];
        snprintf(buf, sizeof(buf), "%0*u", digits, k);
        vn += buf;
        vn += name.Ptr((unsigned)dot);
      }
      else
      {
        char buf[16];
        snprintf(buf, sizeof(buf), ".d%02u", k);
        vn += buf;
      }
      CMyComPtr<IInStream> s;
      RINOK(NItemArc::OpenSiblingFile(callback, vn, s))
      if (!s)
        break;
      Byte vh[76];
      if (NItemArc::ReadAt(s, 0, vh, 76) != S_OK
          || (memcmp(vh, "DAA VOL", 8) != 0 && memcmp(vh, "GBI VOL", 8) != 0))
        break;
      UInt64 vs;
      RINOK(NItemArc::GetStreamSize(s, vs))
      CVolume v;
      v.Stream = s;
      v.Start = GetUi32(vh + 16);
      v.End = vs;
      const CVolume &prev = _volumes.back();
      v.Pos = prev.Pos + (prev.End - prev.Start);
      if (v.Start > vs)
        break;
      _volumes.push_back(v);
    }
  }

  // the chunk table
  const size_t tableSize = dataOffset - tableOffset;
  CByteBuffer table(tableSize);
  RINOK(ReadData(tableOffset, table, tableSize))
  if (packedTable)
  {
    // the table itself deflated, as PowerISO's newer images keep it
    const UInt32 packed = GetUi32(hd + 1);
    if (packed > tableSize)
      return S_FALSE;
    CByteBuffer t((size_t)tableSize * 8 + 0x10000);
    CInflater inf;
    size_t written;
    if (!inf.Run(table, packed, t, t.Size(), _inflateTypes, written))
      return S_FALSE;
    table.CopyFrom(t, written);
  }
  if (gbi)
  {
    const Byte d = (Byte)(table.Size() >> 2);
    for (size_t i = 0; i < table.Size(); i++)
      table[i] = (Byte)((Byte)(table[i] - (Byte)crc) ^ d);
  }
  if (lameTable)
  {
    const UInt64 sectors = isoSize / 0x800;
    const Byte a = (Byte)(sectors >> 8);
    Byte c = (Byte)sectors;
    for (size_t i = 0; i < table.Size(); i++)
    {
      table[i] = (Byte)(table[i] - c);
      c = (Byte)(c + a);
    }
  }
  const UInt64 numChunks = (isoSize + chunkSize - 1) / chunkSize;
  UInt64 pos = dataOffset;
  static const Byte kKey[8] = { 0x0A, 0x35, 0x2D, 0x3F, 0x08, 0x33, 0x09, 0x15 };
  UInt64 bitPos = 0;
  unsigned lameN = 0;
  for (UInt64 i = 0; i < numChunks; i++)
  {
    UInt32 len;
    Byte type = 1;
    if (version == 0x100)
    {
      if ((i + 1) * 3 > table.Size())
        return S_FALSE;
      const Byte *p = table + (size_t)i * 3;
      len = ((UInt32)p[0] << 16) | p[1] | ((UInt32)p[2] << 8);
    }
    else
    {
      if (bitPos + bitSize + bitType > (UInt64)table.Size() * 8)
        return S_FALSE;
      UInt32 key = 0;
      if (lameBits)
        key = ((lameN ^ kKey[lameN & 7]) & 0xFF) * 0x01010101;
      len = (ReadBitsLe(table, table.Size(), bitPos, bitSize) ^ key) & (bitSize >= 32 ? 0xFFFFFFFF : ((1u << bitSize) - 1));
      bitPos += bitSize;
      const UInt32 t = (ReadBitsLe(table, table.Size(), bitPos, bitType) ^ key) & ((1u << bitType) - 1);
      bitPos += bitType;
      if (lameBits)
        lameN++;
      len += 5;
      // as large as a chunk: stored (its first chunk-size bytes)
      type = (Byte)(len >= chunkSize ? 2 : (t == 0 ? 0 : 1));
    }
    _offsets.push_back(pos);
    _types.push_back(type);
    pos += len;
  }
  _offsets.push_back(pos);
  const CVolume &last = _volumes.back();
  if (pos > last.Pos + (last.End - last.Start))
    return S_FALSE;

  _stream = stream;
  _size = isoSize;
  _blockSize = chunkSize;
  _phySize = fileSize;
  _name = ReplaceExtension(GetArcName(callback), "iso");
  if (_name.IsEmpty())
    _name = "image.iso";
  _method = version == 0x100 ? "Deflate" : lzma ? "LZMA" : "Deflate";
  _packed.Alloc(chunkSize + 1024);
  return S_OK;
}

HRESULT CHandler::ReadBlock(UInt64 index, Byte *dest)
{
  if (index + 1 >= _offsets.size())
    return S_FALSE;
  const UInt64 start = _offsets[(size_t)index];
  const UInt64 len = _offsets[(size_t)index + 1] - start;
  const size_t expected = (size_t)MyMin((UInt64)_blockSize, _size - index * _blockSize);
  if (len > _packed.Size())
    return S_FALSE;
  RINOK(ReadData(start, _packed, (size_t)len))
  const Byte type = _types[(size_t)index];
  if (type == 2)
  {
    if (len < expected)
      return S_FALSE;
    memcpy(dest, _packed, expected);
    return S_OK;
  }
  if (type == 0)
  {
    CLzmaDec dec;
    LzmaDec_CONSTRUCT(&dec)
    if (LzmaDec_Allocate(&dec, _lzmaProps, LZMA_PROPS_SIZE, &g_Alloc) != SZ_OK)
      return E_OUTOFMEMORY;
    LzmaDec_Init(&dec);
    SizeT outLen = _blockSize, inLen = (SizeT)len;
    ELzmaStatus status;
    const SRes r = LzmaDec_DecodeToBuf(&dec, dest, &outLen, _packed, &inLen, LZMA_FINISH_ANY, &status);
    LzmaDec_Free(&dec, &g_Alloc);
    if (r != SZ_OK || outLen < expected)
      return S_FALSE;
    if (_x86)
    {
      UInt32 state = Z7_BRANCH_CONV_ST_X86_STATE_INIT_VAL;
      z7_BranchConvSt_X86_Dec(dest, outLen, 0, &state);
    }
    return S_OK;
  }
  CInflater inf;
  size_t written;
  if (!inf.Run(_packed, (size_t)len, dest, _blockSize, _inflateTypes, written))
    return S_FALSE;
  return written >= expected ? S_OK : S_FALSE;
}

static const Byte k_Signature[] =
{
  16, 'D', 'A', 'A', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  16, 'G', 'B', 'I', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

REGISTER_ARC_I(
  "DAA", "daa gbi", NULL, 0xB3,
  k_Signature,
  0,
  NArcInfoFlags::kMultiSignature,
  NULL)

}}}

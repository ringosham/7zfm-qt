// DiscContainers.cpp -- 7zfm-qt: images in a container (see DiscImage.h):
//
//   CSO, ZSO   PSP compressed ISO: deflate (CSO) or LZ4 (ZSO) blocks (maxcso)
//   CISO       GameCube / Wii compact ISO: unused blocks left out (Dolphin);
//              same magic as CSO, so the same format
//   ISZ        UltraISO: zlib / bzip2 chunks (libmirage); not encrypted,
//              not split
//   ECM        Neill Corlett's ECM: CD sectors with their sync, EDC and ECC
//              left out, put back here
//   zisofs     a file compressed by mkzftree, as stored on a zisofs ISO
//
// Each is one item, read through CBlockHandler's block cache; upstream
// opens what it holds right away (kpidMainSubfile).
//
// The layouts are as maxcso's format notes, Dolphin and libmirage give
// them; no code of theirs is used here.

#include "DiscImage.h"

#include "Common/ComTry.h"

#include "7zip/Common/RegisterArc.h"
#include "7zip/Common/StreamUtils.h"

#include <bzlib.h>
#include <lz4.h>

#include <string.h>

#include <memory>
#include <vector>

namespace NArchive {
namespace NDisc {

static UString ItemName(IArchiveOpenCallback *callback, const char *ext, bool stripOnly = false)
{
  const UString arc = GetArcName(callback);
  if (arc.IsEmpty())
    return UString();
  UString base = BaseName(arc);
  if (stripOnly)
    return base;
  base += L'.';
  base += ext;
  return base;
}

static HRESULT ReadAt(IInStream *s, UInt64 pos, Byte *dest, size_t size)
{
  RINOK(InStream_SeekSet(s, pos))
  return ReadStream_FALSE(s, dest, size);
}

// ---------- CISO (GameCube / Wii), read by the CSO format ----------

namespace NCiso {

static const UInt32 kHeaderSize = 0x8000;
static const UInt32 kMapSize = kHeaderSize - 8;

struct CGc
{
  std::vector<Int64> _phys; // stored block of each block, or -1
  IInStream *_stream;
  UInt64 _size;
  UInt32 _blockSize;

  HRESULT Open(IInStream *stream)
  {
    _stream = stream;
    CByteBuffer h(kHeaderSize);
    RINOK(ReadAt(stream, 0, h, kHeaderSize))
    if (memcmp(h, "CISO", 4) != 0)
      return S_FALSE;
    const UInt32 bs = GetLe32(h + 4);
    if (bs < 0x8000 || bs > (1 << 26))
      return S_FALSE;
    Int64 k = 0;
    UInt32 last = 0;
    _phys.resize(kMapSize);
    for (UInt32 i = 0; i < kMapSize; i++)
    {
      const Byte m = h[8 + i];
      if (m > 1)
        return S_FALSE;
      _phys[i] = m ? k++ : -1;
      if (m)
        last = i + 1;
    }
    if (k == 0)
      return S_FALSE;
    _phys.resize(last);
    _size = (UInt64)last * bs;
    _blockSize = bs;
    return S_OK;
  }

  HRESULT ReadBlock(UInt64 index, Byte *dest)
  {
    if (index >= _phys.size())
      return S_FALSE;
    if (_phys[(size_t)index] < 0)
    {
      memset(dest, 0, _blockSize);
      return S_OK;
    }
    return ReadAt(_stream, kHeaderSize + (UInt64)_phys[(size_t)index] * _blockSize, dest, _blockSize);
  }
};

}


// ---------- CSO / ZSO ----------

namespace NCso {

class CHandler Z7_final: public CBlockHandler
{
  std::unique_ptr<NCiso::CGc> _gc;
  std::vector<UInt32> _index;
  unsigned _shift;
  bool _lz4;
  bool _v2;
  CByteBuffer _packed;

  void Close2() Z7_override { _gc.reset(); _index.clear(); }

  HRESULT Open2(IInStream *stream, IArchiveOpenCallback *callback) Z7_override
  {
    const HRESULT res = OpenPsp(stream, callback);
    if (res != S_FALSE)
      return res;
    // a GameCube / Wii CISO has the same magic
    _gc.reset(new NCiso::CGc);
    RINOK(_gc->Open(stream))
    _size = _gc->_size;
    _blockSize = _gc->_blockSize;
    _name = ItemName(callback, "iso");
    _method = "CISO";
    return S_OK;
  }

  HRESULT OpenPsp(IInStream *stream, IArchiveOpenCallback *callback)
  {
    Byte h[0x18];
    RINOK(ReadAt(stream, 0, h, sizeof(h)))
    _lz4 = memcmp(h, "ZISO", 4) == 0;
    if (!_lz4 && memcmp(h, "CISO", 4) != 0)
      return S_FALSE;
    const UInt64 size = GetLe64(h + 8);
    const UInt32 bs = GetLe32(h + 0x10);
    const unsigned version = h[0x14];
    _shift = h[0x15];
    // a GameCube / Wii CISO has the same magic: its fields don't fit
    if (size == 0 || bs < 0x800 || bs > (1 << 24) || (bs & (bs - 1)) != 0
        || version > 2 || _shift > 20)
      return S_FALSE;
    _v2 = version == 2;
    const UInt64 numBlocks = (size + bs - 1) / bs;
    UInt64 fileSize;
    RINOK(StreamSize(stream, fileSize))
    if (0x18 + (numBlocks + 1) * 4 > fileSize)
      return S_FALSE;
    CByteBuffer table((size_t)(numBlocks + 1) * 4);
    RINOK(ReadAt(stream, _v2 ? GetLe32(h + 4) : 0x18, table, table.Size()))
    _index.resize((size_t)numBlocks + 1);
    for (size_t i = 0; i <= numBlocks; i++)
      _index[i] = GetLe32(table + i * 4);
    if (((UInt64)(_index[(size_t)numBlocks] & 0x7FFFFFFF) << _shift) > fileSize)
      return S_FALSE;
    _size = size;
    _blockSize = bs;
    _packed.Alloc(bs + (bs >> 4) + 1024);
    _name = ItemName(callback, "iso");
    _method = _lz4 ? "LZ4" : _v2 ? "Deflate/LZ4" : "Deflate";
    return S_OK;
  }

public:
  HRESULT ReadBlock(UInt64 index, Byte *dest) Z7_override
  {
    if (_gc)
      return _gc->ReadBlock(index, dest);
    if (index + 1 >= _index.size())
      return S_FALSE;
    const UInt64 start = (UInt64)(_index[(size_t)index] & 0x7FFFFFFF) << _shift;
    const UInt64 end = (UInt64)(_index[(size_t)index + 1] & 0x7FFFFFFF) << _shift;
    if (end < start)
      return S_FALSE;
    const size_t expected = (size_t)MyMin((UInt64)_blockSize, _size - index * _blockSize);
    size_t len = (size_t)(end - start);
    const bool flag = (_index[(size_t)index] & 0x80000000) != 0;
    bool plain = _v2 ? len >= _blockSize : flag;
    if (plain)
    {
      if (len < expected)
        return S_FALSE;
      return ReadAt(_stream, start, dest, expected);
    }
    if (len > _packed.Size())
      return S_FALSE;
    // the padding to the next index may run past the end of the file
    RINOK(InStream_SeekSet(_stream, start))
    RINOK(ReadStream(_stream, _packed, &len))
    if (_lz4 || (_v2 && flag))
    {
      const int n = LZ4_decompress_safe((const char *)(const Byte *)_packed, (char *)dest, (int)len, (int)_blockSize);
      return n >= (int)expected ? S_OK : S_FALSE;
    }
    size_t written = 0;
    const HRESULT res = Inflate(_packed, len, dest, _blockSize, -15, &written);
    if (res != S_OK)
      return res;
    return written >= expected ? S_OK : S_FALSE;
  }
};

// CSO, ZSO and CISO share a format: upstream's registry holds 72 formats at most
static const Byte k_Signature[] = { 4, 'C', 'I', 'S', 'O', 4, 'Z', 'I', 'S', 'O' };
REGISTER_ARC_I("CSO", "cso zso ciso", NULL, 0x9B, k_Signature, 0, NArcInfoFlags::kMultiSignature, NULL)

}

// ---------- ISZ (UltraISO) ----------

namespace NIsz {

enum { kZero = 0, kData = 1, kZlib = 2, kBzip2 = 3 };

class CHandler Z7_final: public CBlockHandler
{
  struct CChunk { UInt64 Offset; UInt32 Length; unsigned Type; };
  std::vector<CChunk> _chunks;
  CByteBuffer _packed;

  HRESULT Open2(IInStream *stream, IArchiveOpenCallback *callback) Z7_override
  {
    Byte h[64];
    RINOK(ReadAt(stream, 0, h, sizeof(h)))
    if (memcmp(h, "IsZ!", 4) != 0 || h[5] > 1)
      return S_FALSE;
    const UInt32 sectSize = GetLe16(h + 10);
    const UInt32 totalSectors = GetLe32(h + 12);
    if (h[16] != 0)
      return S_FALSE; // encrypted
    if (GetLe64(h + 17) != 0)
      return S_FALSE; // split into .i01... files
    const UInt32 numChunks = GetLe32(h + 25);
    const UInt32 chunkSize = GetLe32(h + 29);
    const unsigned ptrLen = h[33];
    const UInt32 chunkOffs = GetLe32(h + 35);
    const UInt32 dataOffs = GetLe32(h + 43);
    if (sectSize == 0 || chunkSize == 0 || chunkSize > (1 << 26) || ptrLen < 1 || ptrLen > 4)
      return S_FALSE;
    const UInt64 size = (UInt64)totalSectors * sectSize;
    if (numChunks != (size + chunkSize - 1) / chunkSize)
      return S_FALSE;
    _chunks.resize(numChunks);
    UInt64 pos = dataOffs;
    if (chunkOffs != 0)
    {
      CByteBuffer t((size_t)numChunks * ptrLen);
      RINOK(ReadAt(stream, chunkOffs, t, t.Size()))
      static const Byte kKey[4] = { (Byte)~'I', (Byte)~'s', (Byte)~'Z', (Byte)~'!' };
      for (size_t i = 0; i < t.Size(); i++)
        t[i] ^= kKey[i % 4];
      const unsigned bits = 8 * ptrLen - 2;
      for (UInt32 i = 0; i < numChunks; i++)
      {
        UInt32 v = 0;
        for (unsigned b = 0; b < ptrLen; b++)
          v |= (UInt32)t[(size_t)i * ptrLen + b] << (8 * b);
        CChunk &c = _chunks[i];
        c.Length = v & ((1u << bits) - 1);
        c.Type = (v >> bits) & 3;
        c.Offset = pos;
        pos += c.Length;
      }
    }
    else
      for (UInt32 i = 0; i < numChunks; i++)
      {
        CChunk &c = _chunks[i];
        c.Type = kData;
        c.Length = (UInt32)MyMin((UInt64)chunkSize, size - (UInt64)i * chunkSize);
        c.Offset = pos;
        pos += c.Length;
      }
    _size = size;
    _blockSize = chunkSize;
    _phySize = pos;
    _packed.Alloc(chunkSize + (chunkSize >> 3) + 1024);
    _name = ItemName(callback, "iso");
    _method = "ISZ";
    return S_OK;
  }

public:
  HRESULT ReadBlock(UInt64 index, Byte *dest) Z7_override
  {
    if (index >= _chunks.size())
      return S_FALSE;
    const CChunk &c = _chunks[(size_t)index];
    const size_t expected = (size_t)MyMin((UInt64)_blockSize, _size - index * _blockSize);
    switch (c.Type)
    {
      case kZero:
        memset(dest, 0, expected);
        return S_OK;
      case kData:
        if (c.Length < expected)
          return S_FALSE;
        return ReadAt(_stream, c.Offset, dest, expected);
      default:
        break;
    }
    if (c.Length > _packed.Size())
      return S_FALSE;
    RINOK(ReadAt(_stream, c.Offset, _packed, c.Length))
    if (c.Type == kZlib)
    {
      size_t written = 0;
      RINOK(Inflate(_packed, c.Length, dest, _blockSize, 15, &written))
      return written >= expected ? S_OK : S_FALSE;
    }
    if (c.Length < 3)
      return S_FALSE;
    memcpy(_packed, "BZh", 3);
    unsigned int out = _blockSize;
    if (BZ2_bzBuffToBuffDecompress((char *)dest, &out, (char *)(Byte *)_packed, c.Length, 0, 0) != BZ_OK)
      return S_FALSE;
    return out >= expected ? S_OK : S_FALSE;
  }
};

static const Byte k_Signature[] = { 'I', 's', 'Z', '!' };
REGISTER_ARC_I("ISZ", "isz", NULL, 0x9E, k_Signature, 0, 0, NULL)

}

// ---------- ECM ----------

namespace NEcm {

static const UInt32 kOutSize[4] = { 1, 2352, 2336, 2336 };
static const UInt32 kInSize[4] = { 1, 2051, 2052, 2328 };

class CHandler Z7_final: public CBlockHandler
{
  struct CRun { UInt64 Out; UInt64 In; UInt32 Count; unsigned Type; };
  std::vector<CRun> _runs;
  Byte _sector[kRawSectorSize];
  Byte _in[kRawSectorSize];

  HRESULT Open2(IInStream *stream, IArchiveOpenCallback *callback) Z7_override
  {
    Byte magic[4];
    RINOK(ReadAt(stream, 0, magic, 4))
    if (memcmp(magic, "ECM\0", 4) != 0)
      return S_FALSE;
    UInt64 fileSize;
    RINOK(StreamSize(stream, fileSize))
    // the run headers, read through a buffer
    CByteBuffer buf(1 << 16);
    UInt64 bufPos = 0;
    size_t bufLen = 0;
    UInt64 pos = 4, out = 0;
    auto byteAt = [&](UInt64 p, Byte &b) -> HRESULT {
      if (p < bufPos || p >= bufPos + bufLen)
      {
        bufPos = p;
        bufLen = buf.Size();
        RINOK(InStream_SeekSet(stream, p))
        RINOK(ReadStream(stream, buf, &bufLen))
        if (bufLen == 0)
          return S_FALSE;
      }
      b = buf[(size_t)(p - bufPos)];
      return S_OK;
    };
    for (;;)
    {
      Byte c;
      RINOK(byteAt(pos++, c))
      const unsigned type = c & 3;
      UInt32 num = (c >> 2) & 0x1F;
      unsigned bits = 5;
      while (c & 0x80)
      {
        RINOK(byteAt(pos++, c))
        if (bits > 31 || (c & 0x7F) >= (0x80000000u >> (bits - 1)))
          return S_FALSE;
        num |= (UInt32)(c & 0x7F) << bits;
        bits += 7;
      }
      if (num == 0xFFFFFFFF)
        break;
      num++;
      CRun r;
      r.Out = out;
      r.In = pos;
      r.Count = num;
      r.Type = type;
      _runs.push_back(r);
      pos += (UInt64)num * kInSize[type];
      out += (UInt64)num * kOutSize[type];
      if (pos > fileSize)
        return S_FALSE;
    }
    if (_runs.empty())
      return S_FALSE;
    _size = out;
    _blockSize = 1 << 16;
    _phySize = pos + 4; // and the EDC of the whole output
    _name = ItemName(callback, NULL, true); // "game.bin.ecm" -> "game.bin"
    if (_name.IsEmpty())
      _name = L"image.bin";
    _method = "ECM";
    return S_OK;
  }

  HRESULT DecodeSector(const CRun &r, UInt32 k)
  {
    const UInt32 inSize = kInSize[r.Type];
    RINOK(ReadAt(_stream, r.In + (UInt64)k * inSize, _in, inSize))
    Byte *s = _sector;
    memset(s, 0, kRawSectorSize);
    memcpy(s, kSync, 12);
    if (r.Type == 1)
    {
      memcpy(s + 12, _in, 3);
      s[15] = 1;
      memcpy(s + 16, _in + 3, 2048);
    }
    else
    {
      s[15] = 2;
      memcpy(s + 16, _in, 4);
      memcpy(s + 20, _in, 4);
      memcpy(s + 24, _in + 4, r.Type == 2 ? 2048 : 2324);
    }
    // by the run's type, not the subheader's form bit
    if (r.Type == 1)
      EdcEccGenerate(s);
    else if (r.Type == 2)
    {
      SetLe32(s + 0x818, Edc(s + 16, 0x808));
      EccGenerate(s);
    }
    else
      SetLe32(s + 0x92C, Edc(s + 16, 0x91C));
    return S_OK;
  }

public:
  HRESULT ReadBlock(UInt64 index, Byte *dest) Z7_override
  {
    UInt64 pos = index * _blockSize;
    const UInt64 end = MyMin(pos + _blockSize, _size);
    // the run holding (pos)
    size_t lo = 0, hi = _runs.size();
    while (hi - lo > 1)
    {
      const size_t mid = (lo + hi) / 2;
      if (_runs[mid].Out <= pos)
        lo = mid;
      else
        hi = mid;
    }
    Byte *d = dest;
    for (size_t ri = lo; pos < end && ri < _runs.size(); ri++)
    {
      const CRun &r = _runs[ri];
      const UInt32 unit = kOutSize[r.Type];
      const UInt64 runEnd = r.Out + (UInt64)r.Count * unit;
      while (pos < end && pos < runEnd)
      {
        const UInt64 rel = pos - r.Out;
        if (r.Type == 0)
        {
          const size_t n = (size_t)MyMin(end, runEnd) - (size_t)pos;
          RINOK(ReadAt(_stream, r.In + rel, d, n))
          d += n;
          pos += n;
          continue;
        }
        const UInt32 k = (UInt32)(rel / unit);
        const UInt32 off = (UInt32)(rel % unit);
        RINOK(DecodeSector(r, k))
        const UInt32 n = (UInt32)MyMin((UInt64)(unit - off), end - pos);
        memcpy(d, _sector + (r.Type == 1 ? 0 : 16) + off, n);
        d += n;
        pos += n;
      }
    }
    return pos == end ? S_OK : S_FALSE;
  }
};

static const Byte k_Signature[] = { 'E', 'C', 'M', 0 };
REGISTER_ARC_I("ECM", "ecm", NULL, 0x9F, k_Signature, 0, 0, NULL)

}

// ---------- zisofs ----------

namespace NZisofs {

class CHandler Z7_final: public CBlockHandler
{
  std::vector<UInt32> _ptrs;
  CByteBuffer _packed;

  HRESULT Open2(IInStream *stream, IArchiveOpenCallback *callback) Z7_override
  {
    Byte h[16];
    RINOK(ReadAt(stream, 0, h, 16))
    static const Byte kMagic[8] = { 0x37, 0xE4, 0x53, 0x96, 0xC9, 0xDB, 0xD6, 0x07 };
    if (memcmp(h, kMagic, 8) != 0)
      return S_FALSE;
    const UInt32 size = GetLe32(h + 8);
    const unsigned headerWords = h[12];
    const unsigned shift = h[13];
    if (headerWords < 4 || shift < 15 || shift > 17)
      return S_FALSE;
    const UInt32 bs = (UInt32)1 << shift;
    const size_t numBlocks = (size_t)((size + (UInt64)bs - 1) / bs);
    CByteBuffer t((numBlocks + 1) * 4);
    RINOK(ReadAt(stream, headerWords * 4, t, t.Size()))
    _ptrs.resize(numBlocks + 1);
    for (size_t i = 0; i <= numBlocks; i++)
      _ptrs[i] = GetLe32(t + i * 4);
    _size = size;
    _blockSize = bs;
    _phySize = _ptrs.back();
    _packed.Alloc(bs + (bs >> 4) + 1024);
    _name = GetArcName(callback);
    if (_name.IsEmpty())
      _name = L"file";
    _method = "zisofs";
    return S_OK;
  }

public:
  HRESULT ReadBlock(UInt64 index, Byte *dest) Z7_override
  {
    if (index + 1 >= _ptrs.size())
      return S_FALSE;
    const UInt32 start = _ptrs[(size_t)index], end = _ptrs[(size_t)index + 1];
    const size_t expected = (size_t)MyMin((UInt64)_blockSize, _size - index * _blockSize);
    if (end < start || end - start > _packed.Size())
      return S_FALSE;
    if (end == start)
    {
      memset(dest, 0, expected);
      return S_OK;
    }
    RINOK(ReadAt(_stream, start, _packed, end - start))
    size_t written = 0;
    RINOK(Inflate(_packed, end - start, dest, _blockSize, 15, &written))
    return written >= expected ? S_OK : S_FALSE;
  }
};

static const Byte k_Signature[] = { 0x37, 0xE4, 0x53, 0x96, 0xC9, 0xDB, 0xD6, 0x07 };
REGISTER_ARC_I("zisofs", "zisofs", NULL, 0xA0, k_Signature, 0, 0, NULL)

}

}}

// DiscChd.cpp -- 7zfm-qt: MAME's CHD (Compressed Hunks of Data), versions
// 3 to 5, after libchdr (BSD-3-Clause, Aaron Giles). A CD or GD-ROM shows
// its tracks (see DiscImage.h), from the CHT2 / CHTR / CHGD metadata; a DVD
// is one .iso, a hard disk one .img. CHDs that need a parent are refused.
//
// v5 codecs: zlib, lzma, huff, flac, zstd and the CD ones cdzl, cdlz, cdfl,
// cdzs (sector data with one codec, subcode with deflate or zstd, sync and
// ECC put back where the encoder left them out). FLAC is the system libFLAC;
// LZMA is upstream's decoder.

#include "DiscImage.h"

#include "Common/ComTry.h"

#include "7zip/Common/RegisterArc.h"
#include "7zip/Common/StreamUtils.h"

#include "../C/Alloc.h"
#include "../C/LzmaDec.h"

#include <FLAC/stream_decoder.h>
#include <zlib.h>
#include <zstd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>

#include <memory>
#include <string>
#include <vector>

namespace NArchive {
namespace NDisc {
namespace NChd {

static const unsigned kFrameSize = 2448;
static const unsigned kFrameData = 2352;

static UInt32 Tag(const char *s)
{
  return ((UInt32)(Byte)s[0] << 24) | ((UInt32)(Byte)s[1] << 16) | ((UInt32)(Byte)s[2] << 8) | (Byte)s[3];
}

static UInt16 Crc16(const Byte *p, size_t size)
{
  UInt32 crc = 0xFFFF;
  for (size_t i = 0; i < size; i++)
  {
    crc ^= (UInt32)p[i] << 8;
    for (int b = 0; b < 8; b++)
      crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021) : (crc << 1);
  }
  return (UInt16)crc;
}

// ---------- bit reader (MSB first) and Huffman, as in libchdr ----------

struct CBits
{
  UInt32 Buffer;
  int NumBits;
  const Byte *Data;
  size_t Offset, Length;
  CBits(const Byte *d, size_t len): Buffer(0), NumBits(0), Data(d), Offset(0), Length(len) {}
  UInt32 Peek(int n)
  {
    if (n == 0)
      return 0;
    if (n > NumBits)
      while (NumBits <= 24)
      {
        if (Offset < Length)
          Buffer |= (UInt32)Data[Offset] << (24 - NumBits);
        Offset++;
        NumBits += 8;
      }
    return Buffer >> (32 - n);
  }
  void Remove(int n) { Buffer = n >= 32 ? 0 : (Buffer << n); NumBits -= n; }
  UInt32 Read(int n) { const UInt32 r = Peek(n); Remove(n); return r; }
  bool Overflow() const { return Offset - (size_t)(NumBits / 8) > Length; }
  size_t Flush()
  {
    while (NumBits >= 8) { Offset--; NumBits -= 8; }
    NumBits = 0;
    Buffer = 0;
    return Offset;
  }
};

struct CHuffman
{
  unsigned NumCodes, MaxBits;
  std::vector<Byte> NumBitsOf;
  std::vector<UInt32> Bits;
  std::vector<UInt32> Lookup;

  CHuffman(unsigned numCodes, unsigned maxBits):
    NumCodes(numCodes), MaxBits(maxBits), NumBitsOf(numCodes), Bits(numCodes), Lookup((size_t)1 << maxBits) {}

  UInt32 DecodeOne(CBits &b)
  {
    const UInt32 v = Lookup[b.Peek((int)MaxBits)];
    b.Remove((int)(v & 0x1F));
    return v >> 5;
  }

  bool AssignCanonical()
  {
    UInt32 hist[33] = { 0 };
    for (unsigned c = 0; c < NumCodes; c++)
    {
      if (NumBitsOf[c] > MaxBits)
        return false;
      hist[NumBitsOf[c]]++;
    }
    UInt32 start = 0;
    for (int len = 32; len > 0; len--)
    {
      const UInt32 next = (start + hist[len]) >> 1;
      if (len != 1 && next * 2 != start + hist[len])
        return false;
      hist[len] = start;
      start = next;
    }
    for (unsigned c = 0; c < NumCodes; c++)
      if (NumBitsOf[c] > 0)
        Bits[c] = hist[NumBitsOf[c]]++;
    return true;
  }

  bool BuildLookup()
  {
    std::fill(Lookup.begin(), Lookup.end(), 0);
    for (unsigned c = 0; c < NumCodes; c++)
      if (NumBitsOf[c] > 0)
      {
        const UInt32 value = (c << 5) | (NumBitsOf[c] & 0x1F);
        const unsigned shift = MaxBits - NumBitsOf[c];
        const UInt64 from = (UInt64)Bits[c] << shift, to = (UInt64)(Bits[c] + 1) << shift;
        if (to > Lookup.size())
          return false;
        for (UInt64 i = from; i < to; i++)
          Lookup[(size_t)i] = value;
      }
    return true;
  }

  bool ImportTreeRle(CBits &b)
  {
    const int numBits = MaxBits >= 16 ? 5 : MaxBits >= 8 ? 4 : 3;
    unsigned cur = 0;
    while (cur < NumCodes)
    {
      const UInt32 nb = b.Read(numBits);
      if (nb != 1)
        NumBitsOf[cur++] = (Byte)nb;
      else
      {
        const UInt32 nb2 = b.Read(numBits);
        if (nb2 == 1)
          NumBitsOf[cur++] = 1;
        else
        {
          UInt32 rep = b.Read(numBits) + 3;
          if (rep + cur > NumCodes)
            return false;
          while (rep--)
            NumBitsOf[cur++] = (Byte)nb2;
        }
      }
    }
    return AssignCanonical() && BuildLookup() && !b.Overflow();
  }

  bool ImportTreeHuffman(CBits &b)
  {
    CHuffman small(24, 6);
    small.NumBitsOf[0] = (Byte)b.Read(3);
    const UInt32 start = b.Read(3) + 1;
    UInt32 count = 0;
    for (unsigned i = 1; i < 24; i++)
    {
      if (i < start || count == 7)
        small.NumBitsOf[i] = 0;
      else
      {
        count = b.Read(3);
        small.NumBitsOf[i] = (Byte)(count == 7 ? 0 : count);
      }
    }
    if (!small.AssignCanonical() || !small.BuildLookup())
      return false;
    int rleFullBits = 0;
    for (UInt32 v = NumCodes - 9; v != 0; v >>= 1)
      rleFullBits++;
    Byte last = 0;
    unsigned cur = 0;
    while (cur < NumCodes)
    {
      const UInt32 value = small.DecodeOne(b);
      if (value != 0)
        NumBitsOf[cur++] = last = (Byte)(value - 1);
      else
      {
        UInt32 c = b.Read(3) + 2;
        if (c == 9)
          c += b.Read(rleFullBits);
        for (; c != 0 && cur < NumCodes; c--)
          NumBitsOf[cur++] = last;
      }
    }
    return AssignCanonical() && BuildLookup() && !b.Overflow();
  }
};

// ---------- codecs ----------

struct CCodec
{
  virtual ~CCodec() {}
  // S_FALSE: data that doesn't decode
  virtual HRESULT Decompress(const Byte *src, size_t srcLen, Byte *dest, size_t destLen) = 0;
};

static HRESULT InflateExact(const Byte *src, size_t srcLen, Byte *dest, size_t destLen)
{
  size_t written = 0;
  RINOK(Inflate(src, srcLen, dest, destLen, -15, &written))
  return written == destLen ? S_OK : S_FALSE;
}

struct CZlibCodec: public CCodec
{
  HRESULT Decompress(const Byte *src, size_t srcLen, Byte *dest, size_t destLen) override
  {
    return InflateExact(src, srcLen, dest, destLen);
  }
};

struct CZstdCodec: public CCodec
{
  HRESULT Decompress(const Byte *src, size_t srcLen, Byte *dest, size_t destLen) override
  {
    const size_t r = ZSTD_decompress(dest, destLen, src, srcLen);
    return (!ZSTD_isError(r) && r == destLen) ? S_OK : S_FALSE;
  }
};

struct CLzmaCodec: public CCodec
{
  CLzmaDec Dec;
  bool Allocated;
  explicit CLzmaCodec(UInt32 hunkBytes): Allocated(false)
  {
    // the dictionary size LZMA's level 6 would pick for (hunkBytes)
    UInt32 dict = (UInt32)1 << 26;
    if (dict > hunkBytes)
      dict = MyMin(dict, MyMax(hunkBytes, (UInt32)4096));
    UInt32 aligned;
    if (dict >= ((UInt32)1 << 21))
    {
      aligned = (dict + ((UInt32)1 << 20) - 1) & ~(((UInt32)1 << 20) - 1);
      if (aligned < dict)
        aligned = dict;
    }
    else
    {
      unsigned i = 22;
      do { aligned = (UInt32)(2 + (i & 1)) << (i >> 1); i++; } while (aligned < dict);
    }
    Byte props[5] = { 93, (Byte)aligned, (Byte)(aligned >> 8), (Byte)(aligned >> 16), (Byte)(aligned >> 24) };
    LzmaDec_CONSTRUCT(&Dec)
    Allocated = LzmaDec_AllocateProbs(&Dec, props, 5, &g_Alloc) == SZ_OK;
  }
  ~CLzmaCodec() override
  {
    if (Allocated)
      LzmaDec_FreeProbs(&Dec, &g_Alloc);
  }
  HRESULT Decompress(const Byte *src, size_t srcLen, Byte *dest, size_t destLen) override
  {
    if (!Allocated)
      return E_OUTOFMEMORY;
    Dec.dic = dest;
    Dec.dicBufSize = destLen;
    LzmaDec_Init(&Dec);
    SizeT inLen = srcLen;
    ELzmaStatus status;
    const SRes res = LzmaDec_DecodeToDic(&Dec, destLen, src, &inLen, LZMA_FINISH_END, &status);
    const size_t decoded = Dec.dicPos;
    Dec.dic = NULL;
    Dec.dicBufSize = 0;
    return (res == SZ_OK && decoded == destLen) ? S_OK : S_FALSE;
  }
};

struct CHuffCodec: public CCodec
{
  HRESULT Decompress(const Byte *src, size_t srcLen, Byte *dest, size_t destLen) override
  {
    CBits b(src, srcLen);
    CHuffman h(256, 16);
    if (!h.ImportTreeHuffman(b))
      return S_FALSE;
    for (size_t i = 0; i < destLen; i++)
      dest[i] = (Byte)h.DecodeOne(b);
    b.Flush();
    return b.Overflow() ? S_FALSE : S_OK;
  }
};

// FLAC: libFLAC on a stream made of a synthetic STREAMINFO and (src)
class CFlacDecoder
{
  FLAC__StreamDecoder *_dec;
  Byte _header[42];
  const Byte *_src;
  size_t _srcLen, _pos;
  Byte *_dest;
  size_t _samples, _done;
  bool _bigEndian;
  bool _error;

  static FLAC__StreamDecoderReadStatus ReadCb(const FLAC__StreamDecoder *, FLAC__byte buffer[], size_t *bytes, void *data)
  {
    CFlacDecoder *d = (CFlacDecoder *)data;
    const size_t total = sizeof(d->_header) + d->_srcLen;
    size_t n = 0;
    while (n < *bytes && d->_pos < total)
    {
      buffer[n++] = d->_pos < sizeof(d->_header) ? d->_header[d->_pos] : d->_src[d->_pos - sizeof(d->_header)];
      d->_pos++;
    }
    *bytes = n;
    return n == 0 ? FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM : FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
  }
  static FLAC__StreamDecoderWriteStatus WriteCb(const FLAC__StreamDecoder *, const FLAC__Frame *frame,
      const FLAC__int32 *const buffer[], void *data)
  {
    CFlacDecoder *d = (CFlacDecoder *)data;
    for (unsigned i = 0; i < frame->header.blocksize && d->_done < d->_samples; i++, d->_done++)
      for (unsigned ch = 0; ch < 2; ch++)
      {
        const UInt32 v = (UInt32)buffer[ch][i] & 0xFFFF;
        Byte *p = d->_dest + d->_done * 4 + ch * 2;
        if (d->_bigEndian) { p[0] = (Byte)(v >> 8); p[1] = (Byte)v; }
        else { p[0] = (Byte)v; p[1] = (Byte)(v >> 8); }
      }
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
  }
  static void ErrorCb(const FLAC__StreamDecoder *, FLAC__StreamDecoderErrorStatus, void *data)
  {
    ((CFlacDecoder *)data)->_error = true;
  }
public:
  CFlacDecoder(): _dec(FLAC__stream_decoder_new()) {}
  ~CFlacDecoder() { if (_dec) FLAC__stream_decoder_delete(_dec); }

  // decodes (destLen / 4) stereo samples; (consumed) is the bytes of (src)
  // the FLAC frames took
  HRESULT Decode(const Byte *src, size_t srcLen, unsigned blockSize, Byte *dest, size_t destLen,
      bool bigEndian, size_t *consumed)
  {
    if (!_dec)
      return E_OUTOFMEMORY;
    Byte *h = _header;
    memcpy(h, "fLaC", 4);
    h[4] = 0x80; h[5] = 0; h[6] = 0; h[7] = 0x22;
    Byte *si = h + 8;
    memset(si, 0, 34);
    si[0] = (Byte)(blockSize >> 8); si[1] = (Byte)blockSize;
    si[2] = (Byte)(blockSize >> 8); si[3] = (Byte)blockSize;
    // sample rate 44100 (20 bits), 2 channels, 16 bits, no total
    const UInt64 v = ((UInt64)44100 << 44) | ((UInt64)1 << 41) | ((UInt64)15 << 36);
    for (int i = 0; i < 8; i++)
      si[10 + i] = (Byte)(v >> (56 - 8 * i));
    _src = src; _srcLen = srcLen; _pos = 0;
    _dest = dest; _samples = destLen / 4; _done = 0;
    _bigEndian = bigEndian; _error = false;
    if (FLAC__stream_decoder_init_stream(_dec, ReadCb, NULL, NULL, NULL, NULL, WriteCb, NULL, ErrorCb, this)
        != FLAC__STREAM_DECODER_INIT_STATUS_OK)
      return E_FAIL;
    bool ok = FLAC__stream_decoder_process_until_end_of_metadata(_dec) != 0;
    while (ok && _done < _samples && !_error)
    {
      if (!FLAC__stream_decoder_process_single(_dec))
        ok = false;
      else if (FLAC__stream_decoder_get_state(_dec) == FLAC__STREAM_DECODER_END_OF_STREAM)
        break;
    }
    FLAC__uint64 pos = 0;
    if (consumed)
    {
      if (!FLAC__stream_decoder_get_decode_position(_dec, &pos) || pos < sizeof(_header))
        ok = false;
      else
        *consumed = (size_t)(pos - sizeof(_header));
    }
    FLAC__stream_decoder_finish(_dec);
    return (ok && !_error && _done == _samples) ? S_OK : S_FALSE;
  }
};

struct CFlacCodec: public CCodec
{
  CFlacDecoder Flac;
  HRESULT Decompress(const Byte *src, size_t srcLen, Byte *dest, size_t destLen) override
  {
    if (srcLen < 1 || (src[0] != 'L' && src[0] != 'B'))
      return S_FALSE;
    unsigned bs = (unsigned)(destLen / 4);
    while (bs > 2048)
      bs /= 2;
    return Flac.Decode(src + 1, srcLen - 1, bs, dest, destLen, src[0] == 'B', NULL);
  }
};

// cdzl / cdlz / cdzs: ECC bitmap, base length, sector data, subcode
struct CCdCodec: public CCodec
{
  std::unique_ptr<CCodec> Base;
  bool ZstdSubcode;
  std::vector<Byte> Sectors, Subcode;
  CCdCodec(CCodec *base, bool zstdSubcode): Base(base), ZstdSubcode(zstdSubcode) {}
  HRESULT Decompress(const Byte *src, size_t srcLen, Byte *dest, size_t destLen) override
  {
    const size_t frames = destLen / kFrameSize;
    const size_t lenBytes = destLen < 65536 ? 2 : 3;
    const size_t eccBytes = (frames + 7) / 8;
    const size_t headerBytes = eccBytes + lenBytes;
    if (srcLen < headerBytes)
      return S_FALSE;
    size_t baseLen = ((size_t)src[eccBytes] << 8) | src[eccBytes + 1];
    if (lenBytes > 2)
      baseLen = (baseLen << 8) | src[eccBytes + 2];
    if (srcLen < headerBytes + baseLen)
      return S_FALSE;
    Sectors.resize(frames * kFrameData);
    Subcode.resize(frames * 96);
    RINOK(Base->Decompress(src + headerBytes, baseLen, Sectors.data(), Sectors.size()))
    const Byte *sub = src + headerBytes + baseLen;
    const size_t subLen = srcLen - headerBytes - baseLen;
    if (ZstdSubcode)
    {
      CZstdCodec z;
      RINOK(z.Decompress(sub, subLen, Subcode.data(), Subcode.size()))
    }
    else
      RINOK(InflateExact(sub, subLen, Subcode.data(), Subcode.size()))
    for (size_t n = 0; n < frames; n++)
    {
      Byte *sector = dest + n * kFrameSize;
      memcpy(sector, Sectors.data() + n * kFrameData, kFrameData);
      memcpy(sector + kFrameData, Subcode.data() + n * 96, 96);
      if (src[n / 8] & (1 << (n % 8)))
      {
        memcpy(sector, kSync, 12);
        EccGenerate(sector);
      }
    }
    return S_OK;
  }
};

struct CCdFlacCodec: public CCodec
{
  CFlacDecoder Flac;
  std::vector<Byte> Subcode;
  HRESULT Decompress(const Byte *src, size_t srcLen, Byte *dest, size_t destLen) override
  {
    const size_t frames = destLen / kFrameSize;
    std::vector<Byte> audio(frames * kFrameData);
    unsigned bs = (unsigned)(frames * kFrameData / 4);
    while (bs > kFrameData)
      bs /= 2;
    size_t consumed = 0;
    // big-endian samples, as a CHD's CD audio always is
    RINOK(Flac.Decode(src, srcLen, bs, audio.data(), audio.size(), true, &consumed))
    if (consumed > srcLen)
      return S_FALSE;
    Subcode.resize(frames * 96);
    RINOK(InflateExact(src + consumed, srcLen - consumed, Subcode.data(), Subcode.size()))
    for (size_t n = 0; n < frames; n++)
    {
      memcpy(dest + n * kFrameSize, audio.data() + n * kFrameData, kFrameData);
      memcpy(dest + n * kFrameSize + kFrameData, Subcode.data() + n * 96, 96);
    }
    return S_OK;
  }
};

static CCodec *CreateCodec(UInt32 tag, UInt32 hunkBytes)
{
  const UInt32 cdBytes = hunkBytes / kFrameSize * kFrameData;
  if (tag == Tag("zlib")) return new CZlibCodec;
  if (tag == Tag("zstd")) return new CZstdCodec;
  if (tag == Tag("lzma")) return new CLzmaCodec(hunkBytes);
  if (tag == Tag("huff")) return new CHuffCodec;
  if (tag == Tag("flac")) return new CFlacCodec;
  if (tag == Tag("cdzl")) return new CCdCodec(new CZlibCodec, false);
  if (tag == Tag("cdlz")) return new CCdCodec(new CLzmaCodec(cdBytes), false);
  if (tag == Tag("cdzs")) return new CCdCodec(new CZstdCodec, true);
  if (tag == Tag("cdfl")) return new CCdFlacCodec;
  return NULL;
}

// ---------- the CHD's logical data ----------

struct CMapEntry
{
  Byte Type;      // v5: 0-3 codec, 4 none, 5 self, 6 parent; v3/v4: 1-5
  UInt32 Length;
  UInt64 Offset;
  UInt32 Crc;
  bool HasCrc;
};

class CChd
{
public:
  CMyComPtr<IInStream> Stream;
  unsigned Version;
  UInt32 HunkBytes;
  UInt32 UnitBytes;
  UInt64 LogicalBytes;
  UInt64 MetaOffset;
  std::vector<CMapEntry> Map;
  std::unique_ptr<CCodec> Codecs[4];
  bool V5Uncompressed;
  std::vector<Byte> Packed;
  std::shared_ptr<bool> Error;

  HRESULT ReadAt(UInt64 pos, Byte *dest, size_t size)
  {
    RINOK(InStream_SeekSet(Stream, pos))
    return ReadStream_FALSE(Stream, dest, size);
  }

  HRESULT Open(IInStream *stream);
  HRESULT ReadHunk(UInt64 hunk, Byte *dest, unsigned depth = 0);
  HRESULT ReadMetadata(std::vector<std::pair<UInt32, std::string> > &meta);
private:
  HRESULT ReadV5Map(UInt64 mapOffset, UInt64 fileSize);
};

HRESULT CChd::Open(IInStream *stream)
{
  Stream = stream;
  Byte h[124];
  UInt64 fileSize;
  RINOK(StreamSize(stream, fileSize))
  if (fileSize < 124)
    return S_FALSE;
  RINOK(ReadAt(0, h, 124))
  if (memcmp(h, "MComprHD", 8) != 0)
    return S_FALSE;
  const UInt32 length = GetBe32(h + 8);
  Version = GetBe32(h + 12);
  UInt32 compression[4] = { 0, 0, 0, 0 };
  UInt64 mapOffset = 0;
  UInt64 numHunks = 0;
  V5Uncompressed = false;
  if (Version == 3 || Version == 4)
  {
    if (length != (Version == 3 ? 120u : 108u))
      return S_FALSE;
    if (GetBe32(h + 16) & 1)
      return S_FALSE; // needs its parent
    const UInt32 comp = GetBe32(h + 20);
    numHunks = GetBe32(h + 24);
    LogicalBytes = GetBe64(h + 28);
    MetaOffset = GetBe64(h + 36);
    HunkBytes = GetBe32(h + (Version == 3 ? 76 : 44));
    UnitBytes = HunkBytes;
    mapOffset = length;
    if (comp > 2)
      return S_FALSE; // AV codec
    if (comp != 0)
      Codecs[0].reset(new CZlibCodec);
  }
  else if (Version == 5)
  {
    if (length != 124)
      return S_FALSE;
    for (int i = 0; i < 4; i++)
      compression[i] = GetBe32(h + 16 + 4 * i);
    LogicalBytes = GetBe64(h + 32);
    mapOffset = GetBe64(h + 40);
    MetaOffset = GetBe64(h + 48);
    HunkBytes = GetBe32(h + 56);
    UnitBytes = GetBe32(h + 60);
    for (int i = 0; i < 20; i++)
      if (h[104 + i] != 0)
        return S_FALSE; // needs its parent
    if (HunkBytes == 0)
      return S_FALSE;
    numHunks = (LogicalBytes + HunkBytes - 1) / HunkBytes;
    for (int i = 0; i < 4; i++)
      if (compression[i] != 0)
      {
        Codecs[i].reset(CreateCodec(compression[i], HunkBytes));
        if (!Codecs[i])
          return S_FALSE; // a codec we don't have (avhu)
      }
  }
  else
    return S_FALSE;
  if (HunkBytes == 0 || HunkBytes >= (1u << 27) || numHunks == 0 || numHunks > (1u << 30))
    return S_FALSE;
  Map.resize((size_t)numHunks);
  Packed.resize(HunkBytes + 4096);

  if (Version < 5)
  {
    std::vector<Byte> raw((size_t)numHunks * 16);
    if (mapOffset + raw.size() > fileSize)
      return S_FALSE;
    RINOK(ReadAt(mapOffset, raw.data(), raw.size()))
    for (size_t i = 0; i < Map.size(); i++)
    {
      const Byte *e = raw.data() + i * 16;
      CMapEntry &m = Map[i];
      m.Offset = GetBe64(e);
      m.Crc = GetBe32(e + 8);
      m.Length = ((UInt32)e[14] << 16) | ((UInt32)e[12] << 8) | e[13];
      m.Type = e[15] & 0x0F;
      m.HasCrc = (e[15] & 0x10) == 0;
    }
    return S_OK;
  }
  if (compression[0] == 0)
  {
    V5Uncompressed = true;
    std::vector<Byte> raw((size_t)numHunks * 4);
    RINOK(ReadAt(mapOffset, raw.data(), raw.size()))
    for (size_t i = 0; i < Map.size(); i++)
    {
      CMapEntry &m = Map[i];
      m.Offset = (UInt64)GetBe32(raw.data() + i * 4) * HunkBytes;
      m.Type = 4;
      m.Length = HunkBytes;
      m.HasCrc = false;
    }
    return S_OK;
  }
  return ReadV5Map(mapOffset, fileSize);
}

HRESULT CChd::ReadV5Map(UInt64 mapOffset, UInt64 fileSize)
{
  Byte h[16];
  RINOK(ReadAt(mapOffset, h, 16))
  const UInt32 mapBytes = GetBe32(h);
  const UInt64 firstOffs = ((UInt64)GetBe16(h + 4) << 32) | GetBe32(h + 6);
  const UInt32 mapCrc = GetBe16(h + 10);
  const int lengthBits = h[12], selfBits = h[13], parentBits = h[14];
  if (lengthBits > 32 || selfBits > 32 || parentBits > 32 || mapOffset + 16 + mapBytes > fileSize)
    return S_FALSE;
  std::vector<Byte> comp(mapBytes);
  RINOK(ReadAt(mapOffset + 16, comp.data(), mapBytes))
  CBits b(comp.data(), mapBytes);
  CHuffman huff(16, 8);
  if (!huff.ImportTreeRle(b))
    return S_FALSE;
  const size_t n = Map.size();
  std::vector<Byte> types(n);
  UInt32 rep = 0;
  Byte lastComp = 0;
  for (size_t i = 0; i < n; i++)
  {
    if (rep > 0)
    {
      types[i] = lastComp;
      rep--;
      continue;
    }
    if (b.Overflow())
      return S_FALSE;
    const UInt32 v = huff.DecodeOne(b);
    if (v == 7)
    {
      types[i] = lastComp;
      rep = 2 + huff.DecodeOne(b);
    }
    else if (v == 8)
    {
      types[i] = lastComp;
      rep = 2 + 16 + (huff.DecodeOne(b) << 4);
      rep += huff.DecodeOne(b);
    }
    else
      types[i] = lastComp = (Byte)v;
  }
  std::vector<Byte> expanded(n * 12);
  UInt64 cur = firstOffs, lastSelf = 0, lastParent = 0;
  for (size_t i = 0; i < n; i++)
  {
    Byte type = types[i];
    UInt64 offset = cur;
    UInt32 length = 0, crc = 0;
    switch (type)
    {
      case 0: case 1: case 2: case 3:
        cur += length = b.Read(lengthBits);
        crc = b.Read(16);
        break;
      case 4:
        cur += length = HunkBytes;
        crc = b.Read(16);
        break;
      case 5:
        lastSelf = offset = b.Read(selfBits);
        break;
      case 6:
        offset = b.Read(parentBits);
        lastParent = offset;
        break;
      case 9:
        type = 5; offset = lastSelf;
        break;
      case 10:
        lastSelf++;
        type = 5; offset = lastSelf;
        break;
      case 11:
        type = 6;
        lastParent = offset = (UInt64)i * HunkBytes / UnitBytes;
        break;
      case 13:
        lastParent += HunkBytes / UnitBytes;
        type = 6; offset = lastParent;
        break;
      case 12:
        type = 6; offset = lastParent;
        break;
      default:
        return S_FALSE;
    }
    CMapEntry &m = Map[i];
    m.Type = type;
    m.Offset = offset;
    m.Length = length;
    m.Crc = crc;
    m.HasCrc = type <= 4;
    Byte *e = expanded.data() + i * 12;
    e[0] = type;
    e[1] = (Byte)(length >> 16); e[2] = (Byte)(length >> 8); e[3] = (Byte)length;
    for (int k = 0; k < 6; k++)
      e[4 + k] = (Byte)(offset >> (40 - 8 * k));
    e[10] = (Byte)(crc >> 8); e[11] = (Byte)crc;
  }
  if (Crc16(expanded.data(), expanded.size()) != mapCrc)
    return S_FALSE;
  return S_OK;
}

HRESULT CChd::ReadHunk(UInt64 hunk, Byte *dest, unsigned depth)
{
  if (hunk >= Map.size() || depth > 16)
    return S_FALSE;
  const CMapEntry &m = Map[(size_t)hunk];
  bool crcOk = true;
  if (Version < 5)
  {
    switch (m.Type)
    {
      case 1:
        if (!Codecs[0] || m.Length > Packed.size())
          return S_FALSE;
        RINOK(ReadAt(m.Offset, Packed.data(), m.Length))
        RINOK(Codecs[0]->Decompress(Packed.data(), m.Length, dest, HunkBytes))
        break;
      case 2:
        RINOK(ReadAt(m.Offset, dest, HunkBytes))
        break;
      case 3:
        for (UInt32 i = 0; i < HunkBytes; i++)
          dest[i] = (Byte)(m.Offset >> (56 - 8 * (i % 8)));
        break;
      case 4:
        if (m.Offset >= hunk)
          return S_FALSE;
        return ReadHunk(m.Offset, dest, depth + 1);
      default:
        return S_FALSE; // a parent's hunk, or unknown
    }
    if (m.HasCrc)
      crcOk = (UInt32)crc32(0, dest, HunkBytes) == m.Crc;
  }
  else
  {
    if (V5Uncompressed)
    {
      if (m.Offset == 0)
        memset(dest, 0, HunkBytes);
      else
        RINOK(ReadAt(m.Offset, dest, HunkBytes))
      return S_OK;
    }
    switch (m.Type)
    {
      case 0: case 1: case 2: case 3:
        if (!Codecs[m.Type] || m.Length > Packed.size())
          return S_FALSE;
        RINOK(ReadAt(m.Offset, Packed.data(), m.Length))
        RINOK(Codecs[m.Type]->Decompress(Packed.data(), m.Length, dest, HunkBytes))
        break;
      case 4:
        RINOK(ReadAt(m.Offset, dest, HunkBytes))
        break;
      case 5:
        if (m.Offset >= hunk)
          return S_FALSE;
        return ReadHunk(m.Offset, dest, depth + 1);
      default:
        return S_FALSE;
    }
    if (m.HasCrc)
      crcOk = Crc16(dest, HunkBytes) == (UInt16)m.Crc;
  }
  if (!crcOk && Error)
    *Error = true;
  return S_OK;
}

HRESULT CChd::ReadMetadata(std::vector<std::pair<UInt32, std::string> > &meta)
{
  UInt64 pos = MetaOffset;
  UInt64 fileSize;
  RINOK(StreamSize(Stream, fileSize))
  for (unsigned guard = 0; pos != 0 && guard < 10000; guard++)
  {
    if (pos + 16 > fileSize)
      return S_FALSE;
    Byte h[16];
    RINOK(ReadAt(pos, h, 16))
    const UInt32 tag = GetBe32(h);
    const UInt32 len = GetBe32(h + 4) & 0xFFFFFF;
    const UInt64 next = GetBe64(h + 8);
    if (len < 4096 && pos + 16 + len <= fileSize)
    {
      std::string s(len, '\0');
      RINOK(ReadAt(pos + 16, (Byte *)&s[0], len))
      while (!s.empty() && s.back() == 0)
        s.pop_back();
      meta.push_back(std::make_pair(tag, s));
    }
    pos = next;
  }
  return S_OK;
}

// the logical data as a stream: a source for CDisc
Z7_CLASS_IMP_IInStream(CChdStream)
public:
  std::shared_ptr<CChd> Chd;
private:
  std::vector<Byte> _hunk;
  UInt64 _hunkIndex;
  UInt64 _pos;
public:
  void Init(const std::shared_ptr<CChd> &chd)
  {
    Chd = chd;
    _hunk.resize(chd->HunkBytes);
    _hunkIndex = (UInt64)(Int64)-1;
    _pos = 0;
  }
};

Z7_COM7F_IMF(CChdStream::Read(void *data, UInt32 size, UInt32 *processedSize))
{
  if (processedSize)
    *processedSize = 0;
  if (size == 0 || _pos >= Chd->LogicalBytes)
    return S_OK;
  const UInt64 hunk = _pos / Chd->HunkBytes;
  const UInt32 offset = (UInt32)(_pos % Chd->HunkBytes);
  if (hunk != _hunkIndex)
  {
    _hunkIndex = (UInt64)(Int64)-1;
    const HRESULT res = Chd->ReadHunk(hunk, _hunk.data());
    if (res == S_FALSE)
    {
      *Chd->Error = true;
      return S_FALSE;
    }
    RINOK(res)
    _hunkIndex = hunk;
  }
  UInt32 cur = MyMin(size, Chd->HunkBytes - offset);
  if (cur > Chd->LogicalBytes - _pos)
    cur = (UInt32)(Chd->LogicalBytes - _pos);
  memcpy(data, _hunk.data() + offset, cur);
  _pos += cur;
  if (processedSize)
    *processedSize = cur;
  return S_OK;
}

Z7_COM7F_IMF(CChdStream::Seek(Int64 offset, UInt32 seekOrigin, UInt64 *newPosition))
{
  switch (seekOrigin)
  {
    case STREAM_SEEK_SET: break;
    case STREAM_SEEK_CUR: offset += (Int64)_pos; break;
    case STREAM_SEEK_END: offset += (Int64)Chd->LogicalBytes; break;
    default: return STG_E_INVALIDFUNCTION;
  }
  if (offset < 0)
    return HRESULT_WIN32_ERROR_NEGATIVE_SEEK;
  _pos = (UInt64)offset;
  if (newPosition)
    *newPosition = _pos;
  return S_OK;
}

// "TRACK:1 TYPE:MODE1_RAW SUBTYPE:NONE FRAMES:1234 ..." -> values by key
static std::string Field(const std::string &s, const char *key)
{
  const std::string k = std::string(key) + ":";
  size_t p = s.find(k);
  if (p == std::string::npos || (p != 0 && s[p - 1] != ' '))
    return std::string();
  p += k.size();
  const size_t e = s.find(' ', p);
  return s.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

static bool ChdTrackType(const std::string &type, CTrack &t)
{
  t.Stride = kFrameSize;
  if (type == "MODE1" || type == "MODE1/2048") { t.Mode = kMode1; t.Format = kCooked2048; }
  else if (type == "MODE1_RAW" || type == "MODE1/2352") { t.Mode = kMode1; t.Format = kRaw2352; }
  else if (type == "MODE2" || type == "MODE2/2336" || type == "MODE2_FORM_MIX") { t.Mode = kMode2; t.Format = kMode2_2336; }
  else if (type == "MODE2_FORM1" || type == "MODE2/2048") { t.Mode = kMode2; t.Format = kCooked2048; }
  else if (type == "MODE2_FORM2" || type == "MODE2/2324") { t.Mode = kOther; t.Format = kForm2_2324; }
  else if (type == "MODE2_RAW" || type == "MODE2/2352" || type == "CDI/2352") { t.Mode = kMode2; t.Format = kRaw2352; }
  else if (type == "AUDIO" || type == "CDG") { t.Mode = kAudio; t.Format = kRaw2352; t.SwapAudio = true; }
  else
    return false;
  return true;
}

class CHandler Z7_final: public CTrackHandler
{
  HRESULT Open2(IInStream *stream, IArchiveOpenCallback *callback) Z7_override
  {
    std::shared_ptr<CChd> chd = std::make_shared<CChd>();
    RINOK(chd->Open(stream))
    chd->Error = _disc->SourceError;
    std::vector<std::pair<UInt32, std::string> > meta;
    RINOK(chd->ReadMetadata(meta))
    CChdStream *spec = new CChdStream;
    CMyComPtr<IInStream> source = spec;
    spec->Init(chd);
    _disc->Sources.Add(source);

    bool cd = false, gd = false;
    UInt64 frame = 0;
    Int64 lba = 0;
    for (const auto &m : meta)
    {
      const bool cht2 = m.first == Tag("CHT2") || m.first == Tag("CHTR");
      const bool chgd = m.first == Tag("CHGD");
      if (!cht2 && !chgd)
        continue;
      cd = true;
      gd = gd || chgd;
      CTrack t;
      t.Number = (UInt32)atoi(Field(m.second, "TRACK").c_str());
      if (!ChdTrackType(Field(m.second, "TYPE"), t))
        return S_FALSE;
      const UInt32 frames = (UInt32)atoi(Field(m.second, "FRAMES").c_str());
      const UInt32 pregap = (UInt32)atoi(Field(m.second, "PREGAP").c_str());
      const UInt32 postgap = (UInt32)atoi(Field(m.second, "POSTGAP").c_str());
      const bool storedPregap = Field(m.second, "PGTYPE").compare(0, 1, "V") == 0 && pregap <= frames;
      if (!storedPregap)
        lba += pregap;
      const UInt32 skip = storedPregap ? pregap : 0;
      t.Start = (frame + skip) * kFrameSize;
      t.NumSectors = frames - skip;
      t.Lba = (UInt32)(lba + skip);
      // a GD-ROM's high-density area starts at LBA 45000, with track 3
      if (chgd && t.Number == 3 && t.Lba < 45000)
      {
        lba += 45000 - t.Lba;
        t.Lba = 45000;
      }
      t.Session = t.Lba >= 45000 ? 2 : 1;
      lba += frames + postgap;
      frame += (frames + 3) / 4 * 4; // tracks are padded to 4 frames
      _disc->Tracks.Add(t);
    }
    if (cd)
    {
      FOR_VECTOR (i, _disc->Tracks)
      {
        CTrack &t = _disc->Tracks[i];
        if (t.Mode != kAudio && t.Format == kRaw2352)
        {
          // mode 1 or 2, from the first sector with a sync pattern
          Byte s[kRawSectorSize];
          for (UInt32 k = 0; k < MyMin(t.NumSectors, (UInt32)16); k++)
          {
            bool ok;
            if (_disc->ReadSector(t, k, s, ok) != S_OK || !ok)
              break;
            if (memcmp(s, kSync, 12) == 0)
            {
              t.Mode = s[15] == 2 ? kMode2 : kMode1;
              break;
            }
          }
        }
      }
      _disc->Comment = gd ? L"MAME CHD, GD-ROM" : L"MAME CHD, CD-ROM";
    }
    else
    {
      // a DVD: 2048-byte sectors; else a hard disk
      bool dvd = false;
      for (const auto &m : meta)
        if (m.first == Tag("DVD "))
          dvd = true;
      CTrack t;
      t.Number = 1;
      if (dvd)
      {
        t.Mode = kMode1;
        t.Format = kCooked2048;
        t.Stride = 2048;
      }
      else
      {
        t.Mode = kOther;
        t.Format = kWhole;
        t.Stride = chd->UnitBytes >= 512 && chd->UnitBytes <= kRawSectorSize ? chd->UnitBytes : 512;
        _disc->OpenSingleTrack = true;
      }
      t.NumSectors = (UInt32)(chd->LogicalBytes / t.Stride);
      _disc->Tracks.Add(t);
      _disc->Comment = dvd ? L"MAME CHD, DVD" : L"MAME CHD, hard disk";
    }
    UString base = BaseName(GetArcName(callback));
    _disc->NameTracks(base);
    if (!cd && !_disc->Tracks.IsEmpty() && _disc->Tracks[0].Mode == kOther)
      _disc->Tracks[0].Name = (base.IsEmpty() ? UString(L"image") : base) + L".img";
    return S_OK;
  }
};

static const Byte k_Signature[] = { 'M', 'C', 'o', 'm', 'p', 'r', 'H', 'D' };
REGISTER_ARC_I("CHD", "chd", NULL, 0xA1, k_Signature, 0, 0, NULL)

}}}

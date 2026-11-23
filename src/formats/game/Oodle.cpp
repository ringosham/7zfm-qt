// Oodle.cpp -- 7zfm-qt: a decoder for Oodle Data's compressed streams,
// translated to C++ from oozextract 0.5.7
// (https://github.com/lvlvllvlvllvlvl/oozextract, MIT license, see
// LICENSE.oozextract). The structure and the checks follow it closely.
//
//   A stream is blocks of 256 KiB of output, each with a 2-byte header:
//   0x0C in the low nibble of the first byte, bit 7 "restart the decoder"
//   (forget the history), bit 6 "stored"; the second byte the codec
//   (2 LZB16, 5 LZNA, 6 Kraken, 10 Mermaid and Selkie, 11 Bitknit,
//   12 Leviathan) and in bit 7 "checksums". A block is quanta: one for the
//   256 KiB codecs, 16 KiB ones for LZNA, Bitknit and LZB16. A quantum's
//   header (3 bytes, or 2 for the small ones, big-endian) is its compressed
//   size - 1, or the all-ones size with a flag: a memset byte, a copy of an
//   earlier block (small quanta only), or stored.
//
//   Kraken, Mermaid and Leviathan code a quantum as 128 KiB chunks, each
//   with a 3-byte header: entropy-coded bytes only, stored, or an LZ chunk
//   in a mode the codec defines. The LZ chunk's streams (literals,
//   commands, offsets, lengths) are each entropy coded: stored, Huffman
//   with 1 or 3 interleaved bit streams, RLE, tANS, or recursively split
//   into further streams. LZNA (adaptive rANS) and Bitknit (adaptive range
//   coding) keep their models across quanta until a restart; LZB16 is
//   byte-aligned LZ77.
//
// Every read and write is checked against its buffer; a malformed stream
// throws CError, which Decode turns into false. Shifts are masked as in
// Rust's release builds, and arithmetic that oozextract does on unsigned
// or wrapping types wraps here too.

#include "Oodle.h"

#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <memory>
#include <new>
#include <stdexcept>
#include <vector>

namespace NOodle {
namespace {

typedef unsigned char Byte;

struct CError {};

[[noreturn]] void Fail() { throw CError(); }
inline void Check(bool ok) { if (!ok) Fail(); }

inline uint32_t Shl(uint32_t v, int n) { return v << (n & 31); }
inline uint32_t Shr(uint32_t v, int n) { return v >> (n & 31); }
inline uint64_t Shl64(uint64_t v, int n) { return v << (n & 63); }
inline uint64_t Shr64(uint64_t v, int n) { return v >> (n & 63); }
inline int32_t ShlI(int32_t v, int n) { return (int32_t)((uint32_t)v << (n & 31)); }
inline int32_t ShrI(int32_t v, int n) { return v >> (n & 31); }
inline uint32_t Rotl(uint32_t v, int n) { n &= 31; return n ? (v << n) | (v >> (32 - n)) : v; }
inline int Clz(uint32_t v) { return v ? __builtin_clz(v) : 32; }
inline int Ctz(uint64_t v) { return v ? __builtin_ctzll(v) : 64; }
inline int Ilog2(int64_t v) { Check(v > 0); return 63 - __builtin_clzll((uint64_t)v); }
inline size_t AbsDiff(size_t a, size_t b) { return a > b ? a - b : b - a; }
// Rust's `i32 as usize`
inline size_t SizeOf(int32_t v) { return (size_t)(int64_t)v; }

const size_t kSmallBlock = 0x4000;
const size_t kLargeBlock = 0x40000;

// ---------- pointers into the four buffers ----------

enum { kNull, kIn, kOut, kScratch, kTmp };

struct Ptr
{
  int D;
  size_t I;
  Ptr(): D(kNull), I(0) {}
  Ptr(int d, size_t i): D(d), I(i) {}
};

// a signed value converts to size_t by sign extension, which wraps like
// Rust's wrapping_add_signed
inline Ptr operator+(Ptr p, size_t n) { p.I += n; return p; }
inline Ptr operator-(Ptr p, size_t n) { p.I -= n; return p; }
inline Ptr &operator+=(Ptr &p, size_t n) { p.I += n; return p; }
inline Ptr &operator-=(Ptr &p, size_t n) { p.I -= n; return p; }
inline size_t operator-(Ptr a, Ptr b) { return a.I - b.I; }
inline bool operator<(Ptr a, Ptr b) { return a.I < b.I; }
inline bool operator<=(Ptr a, Ptr b) { return a.I <= b.I; }
inline bool operator>(Ptr a, Ptr b) { return a.I > b.I; }
inline bool operator>=(Ptr a, Ptr b) { return a.I >= b.I; }
inline bool operator==(Ptr a, Ptr b) { return a.I == b.I; }
inline bool operator!=(Ptr a, Ptr b) { return a.I != b.I; }

inline Ptr InP(size_t i) { return Ptr(kIn, i); }
inline Ptr OutP(size_t i) { return Ptr(kOut, i); }
inline Ptr ScratchP(size_t i) { return Ptr(kScratch, i); }
inline Ptr TmpP(size_t i) { return Ptr(kTmp, i); }

struct CCore;

struct CHuffRange
{
  uint16_t Symbol;
  uint16_t Num;
};

struct CHuffRevLut
{
  Byte Bits2Len[2048];
  Byte Bits2Sym[2048];
};

const size_t kBasePrefix[12] = { 0x0, 0x0, 0x2, 0x6, 0xE, 0x1E, 0x3E, 0x7E, 0xFE, 0x1FE, 0x2FE, 0x3FE };

// a pointer and the size of what was decoded there
struct CArray
{
  Ptr P;
  size_t Size;
  CArray(): Size(0) {}
};

// ---------- the bit readers ----------

struct CBitReader
{
  Ptr P, PEnd;      // the next byte and the end
  uint32_t Bits;
  int32_t Bitpos;   // where the next byte goes in Bits

  void Refill(const CCore &c);
  void RefillBackwards(const CCore &c);
  bool ReadBit(const CCore &c) { Refill(c); return ReadBitNoRefill(); }
  bool ReadBitNoRefill()
  {
    const uint32_t r = Bits >> 31;
    Bits <<= 1;
    Bitpos += 1;
    return r != 0;
  }
  int32_t ReadBitsNoRefill(int n)
  {
    const uint32_t r = Shr(Bits, 32 - n);
    Bits = Shl(Bits, n);
    Bitpos += n;
    return (int32_t)r;
  }
  int32_t ReadBitsNoRefillZero(int n)
  {
    const uint32_t r = Shr(Bits >> 1, 31 - n);
    Bits = Shl(Bits, n);
    Bitpos += n;
    return (int32_t)r;
  }
  int32_t ReadMoreThan24Bits(const CCore &c, int n)
  {
    int32_t rv;
    if (n <= 24)
      rv = ReadBitsNoRefillZero(n);
    else
    {
      rv = ShlI(ReadBitsNoRefill(24), n - 24);
      Refill(c);
      rv += ReadBitsNoRefill(n - 24);
    }
    Refill(c);
    return rv;
  }
  int32_t ReadMoreThan24BitsB(const CCore &c, int n)
  {
    int32_t rv;
    if (n <= 24)
      rv = ReadBitsNoRefillZero(n);
    else
    {
      rv = ShlI(ReadBitsNoRefill(24), n - 24);
      RefillBackwards(c);
      rv += ReadBitsNoRefill(n - 24);
    }
    RefillBackwards(c);
    return rv;
  }
  int32_t ReadDistance(const CCore &c, int32_t v, bool backwards)
  {
    uint32_t rv;
    if (v < 0xF0)
    {
      const int n = (v >> 4) + 4;
      const uint32_t w = Rotl(Bits | 1, n);
      Bitpos += n;
      const uint32_t m = Shl(2, n) - 1;
      Bits = w & ~m;
      rv = ((w & m) << 4) + (uint32_t)(v & 0xF) - 248;
    }
    else
    {
      const int n = v - 0xF0 + 4;
      const uint32_t w = Rotl(Bits | 1, n);
      Bitpos += n;
      const uint32_t m = Shl(2, n) - 1;
      Bits = w & ~m;
      rv = 8322816 + ((w & m) << 12);
      if (backwards) RefillBackwards(c); else Refill(c);
      rv += Bits >> 20;
      Bitpos += 12;
      Bits <<= 12;
    }
    if (backwards) RefillBackwards(c); else Refill(c);
    return (int32_t)rv;
  }
  int32_t ReadLength(const CCore &c, bool backwards)
  {
    int n = Clz(Bits);
    Check(n <= 12);
    Bitpos += n;
    Bits = Shl(Bits, n);
    if (backwards) RefillBackwards(c); else Refill(c);
    n += 7;
    Bitpos += n;
    const uint32_t rv = Shr(Bits, 32 - n) - 64;
    Bits = Shl(Bits, n);
    if (backwards) RefillBackwards(c); else Refill(c);
    return (int32_t)rv;
  }
  size_t ReadFluff(int32_t numSymbols)
  {
    if (numSymbols == 256)
      return 0;
    int32_t x = 257 - numSymbols;
    if (x > numSymbols)
      x = numSymbols;
    x *= 2;
    const int y = Ilog2(x - 1) + 1;
    const uint32_t v = Shr(Bits, 32 - y);
    const uint32_t z = Shl(1, y) - (uint32_t)x;
    if ((v >> 1) >= z)
    {
      Bits = Shl(Bits, y);
      Bitpos += y;
      return v - z;
    }
    Bits = Shl(Bits, y - 1);
    Bitpos += y - 1;
    return v >> 1;
  }
};

struct CBitReader2
{
  Ptr P, PEnd;
  uint32_t Bitpos;
};

// ---------- the core: buffers and the entropy coders ----------

struct CCore
{
  const Byte *In; size_t InLen;
  Byte *Out; size_t OutLen;
  Byte *Scratch; size_t ScratchLen;
  Byte *Tmp; size_t TmpLen;
  Ptr Src, Dst, DstEnd;

  const Byte *Buf(int d, size_t &len) const
  {
    switch (d)
    {
      case kIn: len = InLen; return In;
      case kOut: len = OutLen; return Out;
      case kScratch: len = ScratchLen; return Scratch;
      case kTmp: len = TmpLen; return Tmp;
    }
    Fail();
  }
  Byte *WBuf(int d, size_t &len)
  {
    switch (d)
    {
      case kOut: len = OutLen; return Out;
      case kScratch: len = ScratchLen; return Scratch;
      case kTmp: len = TmpLen; return Tmp;
    }
    Fail();
  }
  Byte GetByte(Ptr p) const
  {
    size_t len;
    const Byte *b = Buf(p.D, len);
    Check(p.I < len);
    return b[p.I];
  }
  const Byte *Slice(Ptr p, size_t n) const
  {
    size_t len;
    const Byte *b = Buf(p.D, len);
    Check(p.I <= len && n <= len - p.I);
    return b + p.I;
  }
  Byte *WSlice(Ptr p, size_t n)
  {
    size_t len;
    Byte *b = WBuf(p.D, len);
    Check(p.I <= len && n <= len - p.I);
    return b + p.I;
  }
  uint64_t Le(Ptr p, unsigned n) const
  {
    const Byte *s = Slice(p, n);
    uint64_t v = 0;
    for (unsigned i = n; i-- > 0;)
      v = (v << 8) | s[i];
    return v;
  }
  uint64_t Be(Ptr p, unsigned n) const
  {
    const Byte *s = Slice(p, n);
    uint64_t v = 0;
    for (unsigned i = 0; i < n; i++)
      v = (v << 8) | s[i];
    return v;
  }
  // n bytes from p, zero past the end of the buffer
  void GetArr(Ptr p, Byte *a, size_t n) const
  {
    size_t len;
    const Byte *b = Buf(p.D, len);
    const size_t avail = (len > p.I) ? std::min(n, len - p.I) : 0;
    if (avail)
      memcpy(a, b + p.I, avail);
    memset(a + avail, 0, n - avail);
  }
  uint32_t GetArrBe32(Ptr p) const { Byte a[4]; GetArr(p, a, 4); return ((uint32_t)a[0] << 24) | ((uint32_t)a[1] << 16) | ((uint32_t)a[2] << 8) | a[3]; }
  uint32_t GetArrLe32(Ptr p) const { Byte a[4]; GetArr(p, a, 4); return ((uint32_t)a[3] << 24) | ((uint32_t)a[2] << 16) | ((uint32_t)a[1] << 8) | a[0]; }
  // n bytes to p, cut at the end of the buffer
  void SetArr(Ptr p, const Byte *v, size_t n)
  {
    size_t len;
    Byte *b = WBuf(p.D, len);
    Check(p.I <= len);
    memcpy(b + p.I, v, std::min(n, len - p.I));
  }
  void Set(Ptr p, Byte v) { *WSlice(p, 1) = v; }
  void SetBytes(Ptr p, const Byte *v, size_t n) { memcpy(WSlice(p, n), v, n); }
  void Memset(Ptr p, Byte v, size_t n) { memset(WSlice(p, n), v, n); }

  void CopyBytes(Ptr dest, Ptr src, size_t n)
  {
    if (dest.D == src.D)
    {
      if (dest.I != src.I)
      {
        size_t len;
        Byte *b = WBuf(dest.D, len);
        Check(src.I <= len && n <= len - src.I && dest.I <= len && n <= len - dest.I);
        memmove(b + dest.I, b + src.I, n);
      }
    }
    else
    {
      Byte *d = WSlice(dest, n);
      memcpy(d, Slice(src, n), n);
    }
  }

  // copies 8 bytes at a time, so a close source repeats what was copied
  void RepeatCopy64(Ptr dest, Ptr src, size_t bytes)
  {
    if (dest.D != src.D || bytes < AbsDiff(src.I, dest.I))
    {
      CopyBytes(dest, src, bytes);
      return;
    }
    size_t len;
    Byte *b = WBuf(dest.D, len);
    const size_t m = std::max(src.I, dest.I);
    Check(m <= len && bytes <= len - m);
    for (size_t n = 0; n < bytes; n += 8)
      memmove(b + dest.I + n, b + src.I + n, std::min(n + 8, bytes) - n);
  }

  // dest = lhs + rhs, byte by byte, in 8- or 16-byte steps
  void Copy64Add(Ptr dest, Ptr lhs, Ptr rhs, size_t n)
  {
    if (n == 0)
      return;
    const size_t step = (AbsDiff(rhs.I, dest.I) < 16 || n <= 8) ? 8 : 16;
    for (size_t i = 0; i <= (n - 1) / step; i++)
    {
      Byte l[16], r[16];
      GetArr(lhs + i * step, l, step);
      GetArr(rhs + i * step, r, step);
      for (size_t k = 0; k < step; k++)
        l[k] = (Byte)(l[k] + r[k]);
      SetArr(dest + i * step, l, step);
    }
  }

  size_t DecodeBytes(Ptr &output, Ptr src, Ptr srcEnd, size_t &decodedSize, size_t outputSize,
      bool forceMemmove, Ptr scratch);
  size_t DecodeBytesType12(Ptr src, size_t srcSize, Ptr output, size_t outputSize, size_t chunkType);
  int32_t HuffReadCodeLengthsOld(CBitReader &bits, Byte *syms, size_t *codePrefix);
  int32_t HuffReadCodeLengthsNew(CBitReader &bits, Byte *syms, size_t *codePrefix);
  void DecodeGolombRiceLengths(Byte *dst, size_t dstLen, CBitReader2 &br);
  void DecodeGolombRiceBits(Byte *dst, size_t dstLen, size_t bitcount, CBitReader2 &br);
  std::vector<CHuffRange> ConvertToRanges(int32_t numSymbols, size_t p, const Byte *syms, size_t symsLen, CBitReader &bits);
  void MakeLut(const size_t *prefixCur, const Byte *syms, CHuffRevLut &lut);
  size_t DecodeRecursive(Ptr srcOrg, size_t srcSize, Ptr output, size_t outputSize, Ptr scratch);
  size_t DecodeMultiArray(Ptr srcOrg, Ptr srcEnd, Ptr dst, Ptr dstEnd, CArray *arrays, size_t numArrays,
      size_t &totalSizeOut, bool forceMemmove, Ptr scratch);
  size_t GetBlockSize(Ptr srcOrg, Ptr srcEnd, size_t destCapacity);
  size_t DecodeRle(Ptr src, size_t srcSize, Ptr dst, size_t dstSize, Ptr scratch);
  void DecodeRleUnpacked(Ptr cmdPtr, Ptr cmdPtrEnd, Ptr dst, size_t dstSize);
  size_t DecodeTans(Ptr src, size_t srcSize, Ptr dst, size_t dstSize);
  void UnpackOffsets(Ptr src, Ptr srcEnd, Ptr packedOffsStream, Ptr packedOffsStreamExtra, int32_t multiDistScale,
      Ptr packedLitlenStream, std::vector<int32_t> &offsStream, std::vector<int32_t> &lenStream, bool excessFlag);

  template <class TAlgorithm> size_t DecodeQuantum(TAlgorithm &algorithm);
};

void CBitReader::Refill(const CCore &c)
{
  Check(Bitpos <= 24);
  while (Bitpos > 0)
  {
    if (P < PEnd)
      Bits |= Shl(c.GetByte(P), Bitpos);
    Bitpos -= 8;
    P += 1;
  }
}

void CBitReader::RefillBackwards(const CCore &c)
{
  Check(Bitpos <= 24);
  while (Bitpos > 0)
  {
    P -= 1;
    if (P >= PEnd)
      Bits |= Shl(c.GetByte(P), Bitpos);
    Bitpos -= 8;
  }
}

// Decodes one 256 KiB quantum, as 128 KiB chunks with a shared history.
template <class TAlgorithm>
size_t CCore::DecodeQuantum(TAlgorithm &algorithm)
{
  size_t writtenBytes = 0;
  const Ptr srcEnd = InP(InLen);
  const Ptr dstStart = OutP(0);
  while (DstEnd > Dst)
  {
    const size_t dstCount = std::min(DstEnd - Dst, (size_t)0x20000);
    Check(4 <= srcEnd - Src);
    const size_t chunkhdr = (size_t)Be(Src, 3);
    size_t srcUsed;
    if ((chunkhdr & 0x800000) == 0)
    {
      // entropy coded, without any match copying
      Ptr out = Dst;
      srcUsed = DecodeBytes(out, Src, srcEnd, writtenBytes, dstCount, false, ScratchP(0));
      Check(writtenBytes == dstCount);
    }
    else
    {
      Src += 3;
      srcUsed = chunkhdr & 0x7FFFF;
      const size_t mode = (chunkhdr >> 19) & 0xF;
      Check(srcUsed <= srcEnd - Src);
      if (srcUsed < dstCount)
        algorithm.Process(*this, mode, Src, srcUsed, dstStart, Dst, dstCount);
      else if (srcUsed > dstCount || mode != 0)
        Fail();
      else
        CopyBytes(Dst, Src, dstCount);
    }
    Src += srcUsed;
    Dst += dstCount;
  }
  return Src.I;
}

// Unpacks the 8-bit offsets and lengths into 32 bits.
void CCore::UnpackOffsets(Ptr src, Ptr srcEnd, Ptr packedOffsStream, Ptr packedOffsStreamExtra, int32_t multiDistScale,
    Ptr packedLitlenStream, std::vector<int32_t> &offsStream, std::vector<int32_t> &lenStream, bool excessFlag)
{
  size_t u32LenStreamSize = 0;

  CBitReader bitsA;
  bitsA.Bitpos = 24; bitsA.Bits = 0; bitsA.P = src; bitsA.PEnd = srcEnd;
  bitsA.Refill(*this);

  CBitReader bitsB;
  bitsB.Bitpos = 24; bitsB.Bits = 0; bitsB.P = srcEnd; bitsB.PEnd = src;
  bitsB.RefillBackwards(*this);

  if (!excessFlag)
  {
    Check(0x2000 <= bitsB.Bits);
    int n = Clz(bitsB.Bits);
    bitsB.Bitpos += n;
    bitsB.Bits = Shl(bitsB.Bits, n);
    bitsB.RefillBackwards(*this);
    n += 1;
    u32LenStreamSize = (size_t)(Shr(bitsB.Bits, 32 - n) - 1);
    bitsB.Bitpos += n;
    bitsB.Bits = Shl(bitsB.Bits, n);
    bitsB.RefillBackwards(*this);
  }

  const size_t numOffs = offsStream.size();
  if (multiDistScale == 0)
  {
    // the traditional way of coding offsets
    for (size_t i = 0; i < numOffs; i += 2)
    {
      offsStream[i] = (int32_t)(0u - (uint32_t)bitsA.ReadDistance(*this, GetByte(packedOffsStream), false));
      packedOffsStream += 1;
      if (i + 1 < numOffs)
      {
        offsStream[i + 1] = (int32_t)(0u - (uint32_t)bitsB.ReadDistance(*this, GetByte(packedOffsStream), true));
        packedOffsStream += 1;
      }
    }
  }
  else
  {
    // the new way
    for (size_t i = 0; i < numOffs; i += 2)
    {
      int32_t cmd = GetByte(packedOffsStream);
      packedOffsStream += 1;
      Check((cmd >> 3) <= 26);
      int32_t offs = ShlI(8 + (cmd & 7), cmd >> 3) | bitsA.ReadMoreThan24Bits(*this, cmd >> 3);
      offsStream[i] = (int32_t)(8 - (uint32_t)offs);
      if (i + 1 < numOffs)
      {
        cmd = GetByte(packedOffsStream);
        packedOffsStream += 1;
        Check((cmd >> 3) <= 26);
        offs = ShlI(8 + (cmd & 7), cmd >> 3) | bitsB.ReadMoreThan24BitsB(*this, cmd >> 3);
        offsStream[i + 1] = (int32_t)(8 - (uint32_t)offs);
      }
    }
    if (multiDistScale != 1)
    {
      // combine the scaled offset arrays
      Ptr lowBits = packedOffsStreamExtra;
      for (size_t i = 0; i < numOffs; i++)
      {
        int32_t v = (int32_t)((uint32_t)offsStream[i] * (uint32_t)multiDistScale);
        v = (int32_t)((uint32_t)v - GetByte(lowBits));
        offsStream[i] = v;
        lowBits += 1;
      }
    }
  }

  uint32_t u32LenStreamBuf[512];   // at most 128 KiB / 256
  Check(u32LenStreamSize <= 512);
  for (size_t i = 0; i < u32LenStreamSize; i++)
    u32LenStreamBuf[i] = (uint32_t)((i % 2 == 0) ? bitsA.ReadLength(*this, false) : bitsB.ReadLength(*this, true));

  bitsA.P -= SizeOf((24 - bitsA.Bitpos) >> 3);
  bitsB.P += SizeOf((24 - bitsB.Bitpos) >> 3);
  Check(bitsA.P == bitsB.P);

  size_t u32LenStream = 0;
  for (size_t i = 0; i < lenStream.size(); i++)
  {
    uint32_t v = GetByte(packedLitlenStream);
    packedLitlenStream += 1;
    if (v == 255)
    {
      Check(u32LenStream < 512);
      v = u32LenStreamBuf[u32LenStream] + 255;
      u32LenStream++;
    }
    lenStream[i] = (int32_t)(v + 3);
  }
  Check(u32LenStream == u32LenStreamSize);
}

size_t CCore::DecodeBytes(Ptr &output, Ptr src, Ptr srcEnd, size_t &decodedSize, size_t outputSize,
    bool forceMemmove, Ptr scratch)
{
  const Ptr srcOrg = src;
  size_t srcSize, dstSize;

  Check(2 <= srcEnd - src);

  const size_t chunkType = (GetByte(src) >> 4) & 0x7;
  if (chunkType == 0)
  {
    if (GetByte(src) >= 0x80)
    {
      // stored, the length in the low 12 bits
      srcSize = (((size_t)GetByte(src) << 8) | GetByte(src + 1)) & 0xFFF;
      src += 2;
    }
    else
    {
      Check(3 <= srcEnd - src);
      srcSize = (size_t)Be(src, 3);
      Check((srcSize & ~(size_t)0x3ffff) == 0);   // reserved bits
      src += 3;
    }
    Check(srcSize <= outputSize);
    Check(srcSize <= srcEnd - src);
    decodedSize = srcSize;
    if (forceMemmove || src.D != output.D)
      CopyBytes(output, src, srcSize);
    else
      output.I = src.I;
    return (src + srcSize) - srcOrg;
  }

  // the other types begin with the source and destination sizes
  if (GetByte(src) >= 0x80)
  {
    Check(3 <= srcEnd - src);
    // short: 10-bit sizes
    const size_t bits = (size_t)Be(src, 3);
    srcSize = bits & 0x3ff;
    dstSize = srcSize + ((bits >> 10) & 0x3ff) + 1;
    src += 3;
  }
  else
  {
    // long: 18-bit sizes
    Check(5 <= srcEnd - src);
    const size_t bits = (size_t)Be(src + 1, 4);
    srcSize = bits & 0x3ffff;
    dstSize = (((bits >> 18) | ((size_t)GetByte(src) << 14)) & 0x3FFFF) + 1;
    Check(srcSize < dstSize);
    src += 5;
  }
  Check(srcSize <= srcEnd - src);
  Check(dstSize <= outputSize);

  const Ptr dst = output;
  if (dst.D == kScratch)
    scratch += dstSize;

  size_t srcUsed;
  switch (chunkType)
  {
    case 2: case 4: srcUsed = DecodeBytesType12(src, srcSize, dst, dstSize, chunkType >> 1); break;
    case 5: srcUsed = DecodeRecursive(src, srcSize, dst, dstSize, scratch); break;
    case 3: srcUsed = DecodeRle(src, srcSize, dst, dstSize, scratch); break;
    case 1: srcUsed = DecodeTans(src, srcSize, dst, dstSize); break;
    default: Fail();
  }
  Check(srcUsed == srcSize);
  decodedSize = dstSize;
  return (src + srcSize) - srcOrg;
}

// ---------- Huffman ----------

struct CHuffReader
{
  // three streams: Src and SrcMid forwards, SrcEnd backwards
  Ptr Output, OutputEnd;
  Ptr Src, SrcMid, SrcEnd, SrcMidOrg;
  int32_t SrcBitpos, SrcMidBitpos, SrcEndBitpos;
  uint32_t SrcBits, SrcMidBits, SrcEndBits;

  CHuffReader(): SrcBitpos(0), SrcMidBitpos(0), SrcEndBitpos(0), SrcBits(0), SrcMidBits(0), SrcEndBits(0) {}
  void DecodeBytes(CCore &core, const CHuffRevLut &lut);
};

void CHuffReader::DecodeBytes(CCore &core, const CHuffRevLut &lut)
{
  Ptr src = Src;
  uint32_t srcBits = SrcBits;
  int32_t srcBitpos = SrcBitpos;
  Ptr srcMid = SrcMid;
  uint32_t srcMidBits = SrcMidBits;
  int32_t srcMidBitpos = SrcMidBitpos;
  Ptr srcEnd = SrcEnd;
  uint32_t srcEndBits = SrcEndBits;
  int32_t srcEndBitpos = SrcEndBitpos;
  Ptr dst = Output;
  Ptr dstEnd = OutputEnd;
  size_t k;
  unsigned n;

  Check(src <= srcMid);

  if ((SrcEnd - srcMid) >= 4 && (dstEnd - dst) >= 6)
  {
    dstEnd -= 5;
    srcEnd -= 4;
    Byte tmp[8] = { 0 };
    while (dst < dstEnd && src <= srcMid && srcMid <= srcEnd)
    {
      srcBits |= Shl((uint32_t)core.Le(src, 4), srcBitpos);
      src += SizeOf((31 - srcBitpos) >> 3);

      srcEndBits |= Shl((uint32_t)core.Be(srcEnd, 4), srcEndBitpos);
      srcEnd -= SizeOf((31 - srcEndBitpos) >> 3);

      srcMidBits |= Shl((uint32_t)core.Le(srcMid, 4), srcMidBitpos);
      srcMid += SizeOf((31 - srcMidBitpos) >> 3);

      srcBitpos |= 0x18;
      srcEndBitpos |= 0x18;
      srcMidBitpos |= 0x18;

      k = srcBits & 0x7FF; n = lut.Bits2Len[k]; srcBits = Shr(srcBits, n); srcBitpos -= n; tmp[0] = lut.Bits2Sym[k];
      k = srcEndBits & 0x7FF; n = lut.Bits2Len[k]; srcEndBits = Shr(srcEndBits, n); srcEndBitpos -= n; tmp[1] = lut.Bits2Sym[k];
      k = srcMidBits & 0x7FF; n = lut.Bits2Len[k]; srcMidBits = Shr(srcMidBits, n); srcMidBitpos -= n; tmp[2] = lut.Bits2Sym[k];
      k = srcBits & 0x7FF; n = lut.Bits2Len[k]; srcBits = Shr(srcBits, n); srcBitpos -= n; tmp[3] = lut.Bits2Sym[k];
      k = srcEndBits & 0x7FF; n = lut.Bits2Len[k]; srcEndBits = Shr(srcEndBits, n); srcEndBitpos -= n; tmp[4] = lut.Bits2Sym[k];
      k = srcMidBits & 0x7FF; n = lut.Bits2Len[k]; srcMidBits = Shr(srcMidBits, n); srcMidBitpos -= n; tmp[5] = lut.Bits2Sym[k];

      core.SetArr(dst, tmp, 8);
      dst += 6;
    }
    dstEnd += 5;

    src -= SizeOf(srcBitpos >> 3);
    srcBitpos &= 7;

    srcEnd += SizeOf(4 + (srcEndBitpos >> 3));
    srcEndBitpos &= 7;

    srcMid -= SizeOf(srcMidBitpos >> 3);
    srcMidBitpos &= 7;
  }
  while (dst < dstEnd)
  {
    if ((srcMid - src) <= 1)
    {
      if ((srcMid - src) == 1)
        srcBits |= Shl(core.GetByte(src), srcBitpos);
    }
    else
      srcBits |= Shl((uint32_t)core.Le(src, 2), srcBitpos);
    k = srcBits & 0x7FF;
    n = lut.Bits2Len[k];
    srcBitpos -= n;
    srcBits = Shr(srcBits, n);
    core.Set(dst, lut.Bits2Sym[k]);
    dst += 1;
    src += SizeOf((7 - srcBitpos) >> 3);
    srcBitpos &= 7;

    if (dst < dstEnd)
    {
      if ((srcEnd - srcMid) <= 1)
      {
        if ((srcEnd - srcMid) == 1)
        {
          const uint32_t mid = core.GetByte(srcMid);
          srcEndBits |= Shl(mid, srcEndBitpos);
          srcMidBits |= Shl(mid, srcMidBitpos);
        }
      }
      else
      {
        const uint32_t v = (uint32_t)core.Le(srcEnd - 2, 2);
        srcEndBits |= Shl(((v >> 8) | (v << 8)) & 0xffff, srcEndBitpos);
        srcMidBits |= Shl((uint32_t)core.Le(srcMid, 2), srcMidBitpos);
      }
      core.Set(dst, lut.Bits2Sym[srcEndBits & 0x7FF]);
      dst += 1;
      n = lut.Bits2Len[srcEndBits & 0x7FF];
      srcEndBitpos -= n;
      srcEndBits = Shr(srcEndBits, n);
      srcEnd -= SizeOf((7 - srcEndBitpos) >> 3);
      srcEndBitpos &= 7;
      if (dst < dstEnd)
      {
        core.Set(dst, lut.Bits2Sym[srcMidBits & 0x7FF]);
        dst += 1;
        n = lut.Bits2Len[srcMidBits & 0x7FF];
        srcMidBitpos -= n;
        srcMidBits = Shr(srcMidBits, n);
        srcMid += SizeOf((7 - srcMidBitpos) >> 3);
        srcMidBitpos &= 7;
      }
    }
    Check(src <= srcMid);
    Check(srcMid <= srcEnd);
  }
  Check(src == SrcMidOrg);
  Check(srcEnd == srcMid);
}

inline unsigned Reverse11(unsigned i)
{
  unsigned r = 0;
  for (unsigned b = 0; b < 11; b++)
    r |= ((i >> b) & 1) << (10 - b);
  return r;
}

void CCore::MakeLut(const size_t *prefixCur, const Byte *syms, CHuffRevLut &lut)
{
  Byte lenLut[2064], symLut[2064];
  memset(lenLut, 0, sizeof(lenLut));
  memset(symLut, 0, sizeof(symLut));
  size_t currslot = 0;
  for (unsigned i = 1; i < 11; i++)
  {
    const size_t start = kBasePrefix[i];
    const size_t count = prefixCur[i] - start;
    if (count != 0)
    {
      const size_t stepsize = (size_t)1 << (11 - i);
      const size_t numToSet = count << (11 - i);
      Check(currslot + numToSet <= 2048);
      memset(lenLut + currslot, (int)i, numToSet);
      for (size_t j = 0; j < count; j++)
      {
        Check(start + j < 1280);
        memset(symLut + currslot + stepsize * j, syms[start + j], stepsize);
      }
      currslot += numToSet;
    }
  }
  const size_t num = prefixCur[11] - kBasePrefix[11];
  if (num != 0)
  {
    Check(currslot <= 2064 && num <= 2064 - currslot);
    Check(num <= 1280 - kBasePrefix[11]);
    memset(lenLut + currslot, 11, num);
    memcpy(symLut + currslot, syms + kBasePrefix[11], num);
    currslot += num;
  }
  Check(currslot == 2048);
  for (unsigned i = 0; i < 2048; i++)
  {
    const unsigned r = Reverse11(i);
    lut.Bits2Len[i] = lenLut[r];
    lut.Bits2Sym[i] = symLut[r];
  }
}

size_t CCore::DecodeBytesType12(Ptr src, size_t srcSize, Ptr output, size_t outputSize, size_t chunkType)
{
  const Ptr srcEnd = src + srcSize;

  CBitReader bits;
  bits.Bitpos = 24; bits.Bits = 0; bits.P = src; bits.PEnd = srcEnd;
  bits.Refill(*this);

  size_t codePrefix[12];
  memcpy(codePrefix, kBasePrefix, sizeof(codePrefix));
  Byte syms[1280];
  memset(syms, 0, sizeof(syms));
  int32_t numSyms;
  if (!bits.ReadBitNoRefill())
    numSyms = HuffReadCodeLengthsOld(bits, syms, codePrefix);
  else if (!bits.ReadBitNoRefill())
    numSyms = HuffReadCodeLengthsNew(bits, syms, codePrefix);
  else
    Fail();
  src = bits.P - SizeOf((24 - bits.Bitpos) / 8);

  if (numSyms == 1)
  {
    // one symbol fills the output; the code lengths are the whole chunk
    Memset(output, syms[0], outputSize);
    return srcSize;
  }

  CHuffRevLut lut;
  MakeLut(codePrefix, syms, lut);

  if (chunkType == 1)
  {
    Check(3 <= srcEnd - src);
    const size_t splitMid = (size_t)Le(src, 2);
    src += 2;
    CHuffReader hr;
    hr.Output = output;
    hr.OutputEnd = output + outputSize;
    hr.Src = src;
    hr.SrcEnd = srcEnd;
    hr.SrcMidOrg = src + splitMid;
    hr.SrcMid = src + splitMid;
    hr.DecodeBytes(*this, lut);
  }
  else
  {
    Check(6 <= srcEnd - src);
    const size_t halfOutputSize = (outputSize + 1) >> 1;
    const size_t splitMid = (size_t)Le(src, 3);
    src += 3;
    Check(splitMid <= srcEnd - src);
    const Ptr srcMid = src + splitMid;
    const size_t splitLeft = (size_t)Le(src, 2);
    src += 2;
    Check(splitLeft + 2 <= srcEnd - src);
    Check(3 <= srcEnd - srcMid);
    const size_t splitRight = (size_t)Le(srcMid, 2);
    Check(splitRight + 2 <= srcEnd - (srcMid + 2));

    CHuffReader hr;
    hr.Output = output;
    hr.OutputEnd = output + halfOutputSize;
    hr.Src = src;
    hr.SrcEnd = srcMid;
    hr.SrcMidOrg = src + splitLeft;
    hr.SrcMid = src + splitLeft;
    hr.DecodeBytes(*this, lut);

    CHuffReader hr2;
    hr2.Output = output + halfOutputSize;
    hr2.OutputEnd = output + outputSize;
    hr2.Src = srcMid + 2;
    hr2.SrcEnd = srcEnd;
    hr2.SrcMidOrg = srcMid + 2 + splitRight;
    hr2.SrcMid = srcMid + 2 + splitRight;
    hr2.DecodeBytes(*this, lut);
  }
  return srcSize;
}

int32_t CCore::HuffReadCodeLengthsOld(CBitReader &bits, Byte *syms, size_t *codePrefix)
{
  if (bits.ReadBitNoRefill())
  {
    int32_t sym = 0;
    int32_t numSymbols = 0;
    int32_t avgBitsX4 = 32;
    const int32_t forcedBits = bits.ReadBitsNoRefill(2);

    const uint32_t thresForValidGammaBits = Shl(1, 31 - ShrI(20, forcedBits));
    bool skipInitialZeros = bits.ReadBit(*this);
    while (sym != 256)
    {
      if (skipInitialZeros)
        skipInitialZeros = false;
      else
      {
        // a run of zeros
        Check((bits.Bits & 0xff000000) != 0);
        sym += bits.ReadBitsNoRefill(2 * (Clz(bits.Bits) + 1)) - 2 + 1;
        if (sym >= 256)
          break;
      }
      bits.Refill(*this);
      // the gamma value for the number of symbols
      Check((bits.Bits & 0xff000000) != 0);
      int32_t n = bits.ReadBitsNoRefill(2 * (Clz(bits.Bits) + 1)) - 2 + 1;
      Check(sym + n <= 256);
      bits.Refill(*this);
      numSymbols += n;
      for (;;)
      {
        Check(thresForValidGammaBits <= bits.Bits);
        const int lz = Clz(bits.Bits);
        const int32_t v = bits.ReadBitsNoRefill(lz + forcedBits + 1) + ShlI(lz - 1, forcedBits);
        const int32_t codelen = ((-(v & 1)) ^ (v >> 1)) + ((avgBitsX4 + 2) >> 2);
        Check(1 <= codelen && codelen <= 11);
        avgBitsX4 = codelen + ((3 * avgBitsX4 + 2) >> 2);
        bits.Refill(*this);
        Check(codePrefix[codelen] < 1280);
        syms[codePrefix[codelen]] = (Byte)sym;
        codePrefix[codelen] += 1;
        sym += 1;
        n -= 1;
        if (n == 0)
          break;
      }
    }
    Check(sym == 256);
    Check(2 <= numSymbols);
    return numSymbols;
  }
  // sparse symbol coding
  const int32_t numSymbols = bits.ReadBitsNoRefill(8);
  Check(numSymbols != 0);
  if (numSymbols == 1)
    syms[0] = (Byte)bits.ReadBitsNoRefill(8);
  else
  {
    const int32_t codelenBits = bits.ReadBitsNoRefill(3);
    Check(codelenBits <= 4);
    for (int32_t i = 0; i < numSymbols; i++)
    {
      bits.Refill(*this);
      const Byte sym = (Byte)bits.ReadBitsNoRefill(8);
      const int32_t codelen = bits.ReadBitsNoRefillZero(codelenBits) + 1;
      Check(codelen <= 11);
      Check(codePrefix[codelen] < 1280);
      syms[codePrefix[codelen]] = sym;
      codePrefix[codelen] += 1;
    }
  }
  return numSymbols;
}

int32_t CCore::HuffReadCodeLengthsNew(CBitReader &bits, Byte *syms, size_t *codePrefix)
{
  const int32_t forcedBits = bits.ReadBitsNoRefill(2);
  const int32_t numSymbols = bits.ReadBitsNoRefill(8) + 1;
  const size_t fluff = bits.ReadFluff(numSymbols);

  Byte codeLen[512];
  memset(codeLen, 0, sizeof(codeLen));
  CBitReader2 br2;
  br2.Bitpos = (uint32_t)((bits.Bitpos - 24) & 7);
  br2.PEnd = bits.PEnd;
  br2.P = bits.P - (size_t)(uint32_t)((24 - bits.Bitpos + 7) >> 3);

  Check((size_t)numSymbols + fluff <= 512);
  DecodeGolombRiceLengths(codeLen, (size_t)numSymbols + fluff, br2);
  DecodeGolombRiceBits(codeLen, (size_t)numSymbols, (size_t)forcedBits, br2);

  // back to the other bit reader
  bits.Bitpos = 24;
  bits.P = br2.P;
  bits.Bits = 0;
  bits.Refill(*this);
  bits.Bits = Shl(bits.Bits, (int)br2.Bitpos);
  bits.Bitpos += (int32_t)br2.Bitpos;

  int32_t runningSum = 0x1e;
  for (int32_t i = 0; i < numSymbols; i++)
  {
    int32_t v = codeLen[i];
    v = (~(v & 1) + 1) ^ (v >> 1);
    codeLen[i] = (Byte)(v + (runningSum >> 2) + 1);
    Check(1 <= codeLen[i] && codeLen[i] <= 11);
    runningSum += v;
  }

  const std::vector<CHuffRange> ranges = ConvertToRanges(numSymbols, fluff, codeLen, 512, bits);

  size_t cp = 0;
  for (size_t r = 0; r < ranges.size(); r++)
  {
    size_t sym = ranges[r].Symbol;
    Check(cp + ranges[r].Num <= 512);
    for (size_t i = 0; i < ranges[r].Num; i++)
    {
      const Byte len = codeLen[cp + i];
      Check(len < 12 && codePrefix[len] < 1280);
      syms[codePrefix[len]] = (Byte)sym;
      codePrefix[len] += 1;
      sym += 1;
    }
    cp += ranges[r].Num;
  }
  return numSymbols;
}

const uint32_t kRiceCodeBits2Value[256] =
{
  0x80000000, 0x00000007, 0x10000006, 0x00000006, 0x20000005, 0x00000105, 0x10000005,
  0x00000005, 0x30000004, 0x00000204, 0x10000104, 0x00000104, 0x20000004, 0x00010004,
  0x10000004, 0x00000004, 0x40000003, 0x00000303, 0x10000203, 0x00000203, 0x20000103,
  0x00010103, 0x10000103, 0x00000103, 0x30000003, 0x00020003, 0x10010003, 0x00010003,
  0x20000003, 0x01000003, 0x10000003, 0x00000003, 0x50000002, 0x00000402, 0x10000302,
  0x00000302, 0x20000202, 0x00010202, 0x10000202, 0x00000202, 0x30000102, 0x00020102,
  0x10010102, 0x00010102, 0x20000102, 0x01000102, 0x10000102, 0x00000102, 0x40000002,
  0x00030002, 0x10020002, 0x00020002, 0x20010002, 0x01010002, 0x10010002, 0x00010002,
  0x30000002, 0x02000002, 0x11000002, 0x01000002, 0x20000002, 0x00000012, 0x10000002,
  0x00000002, 0x60000001, 0x00000501, 0x10000401, 0x00000401, 0x20000301, 0x00010301,
  0x10000301, 0x00000301, 0x30000201, 0x00020201, 0x10010201, 0x00010201, 0x20000201,
  0x01000201, 0x10000201, 0x00000201, 0x40000101, 0x00030101, 0x10020101, 0x00020101,
  0x20010101, 0x01010101, 0x10010101, 0x00010101, 0x30000101, 0x02000101, 0x11000101,
  0x01000101, 0x20000101, 0x00000111, 0x10000101, 0x00000101, 0x50000001, 0x00040001,
  0x10030001, 0x00030001, 0x20020001, 0x01020001, 0x10020001, 0x00020001, 0x30010001,
  0x02010001, 0x11010001, 0x01010001, 0x20010001, 0x00010011, 0x10010001, 0x00010001,
  0x40000001, 0x03000001, 0x12000001, 0x02000001, 0x21000001, 0x01000011, 0x11000001,
  0x01000001, 0x30000001, 0x00000021, 0x10000011, 0x00000011, 0x20000001, 0x00001001,
  0x10000001, 0x00000001, 0x70000000, 0x00000600, 0x10000500, 0x00000500, 0x20000400,
  0x00010400, 0x10000400, 0x00000400, 0x30000300, 0x00020300, 0x10010300, 0x00010300,
  0x20000300, 0x01000300, 0x10000300, 0x00000300, 0x40000200, 0x00030200, 0x10020200,
  0x00020200, 0x20010200, 0x01010200, 0x10010200, 0x00010200, 0x30000200, 0x02000200,
  0x11000200, 0x01000200, 0x20000200, 0x00000210, 0x10000200, 0x00000200, 0x50000100,
  0x00040100, 0x10030100, 0x00030100, 0x20020100, 0x01020100, 0x10020100, 0x00020100,
  0x30010100, 0x02010100, 0x11010100, 0x01010100, 0x20010100, 0x00010110, 0x10010100,
  0x00010100, 0x40000100, 0x03000100, 0x12000100, 0x02000100, 0x21000100, 0x01000110,
  0x11000100, 0x01000100, 0x30000100, 0x00000120, 0x10000110, 0x00000110, 0x20000100,
  0x00001100, 0x10000100, 0x00000100, 0x60000000, 0x00050000, 0x10040000, 0x00040000,
  0x20030000, 0x01030000, 0x10030000, 0x00030000, 0x30020000, 0x02020000, 0x11020000,
  0x01020000, 0x20020000, 0x00020010, 0x10020000, 0x00020000, 0x40010000, 0x03010000,
  0x12010000, 0x02010000, 0x21010000, 0x01010010, 0x11010000, 0x01010000, 0x30010000,
  0x00010020, 0x10010010, 0x00010010, 0x20010000, 0x00011000, 0x10010000, 0x00010000,
  0x50000000, 0x04000000, 0x13000000, 0x03000000, 0x22000000, 0x02000010, 0x12000000,
  0x02000000, 0x31000000, 0x01000020, 0x11000010, 0x01000010, 0x21000000, 0x01001000,
  0x11000000, 0x01000000, 0x40000000, 0x00000030, 0x10000020, 0x00000020, 0x20000010,
  0x00001010, 0x10000010, 0x00000010, 0x30000000, 0x00002000, 0x10001000, 0x00001000,
  0x20000000, 0x00100000, 0x10000000, 0x00000000
};

const Byte kRiceCodeBits2Len[256] =
{
  0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4, 1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3,
  4, 4, 5, 1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4,
  4, 5, 4, 5, 5, 6, 1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 2, 3, 3, 4, 3, 4, 4,
  5, 3, 4, 4, 5, 4, 5, 5, 6, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 3, 4, 4, 5,
  4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7, 1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 2,
  3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5,
  5, 6, 3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4,
  5, 4, 5, 5, 6, 3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7, 3, 4, 4, 5, 4, 5, 5, 6,
  4, 5, 5, 6, 5, 6, 6, 7, 4, 5, 5, 6, 5, 6, 6, 7, 5, 6, 6, 7, 6, 7, 7, 8
};

void CCore::DecodeGolombRiceLengths(Byte *dst, size_t dstLen, CBitReader2 &br)
{
  Ptr p = br.P;
  const Ptr pEnd = br.PEnd;
  Check(p < pEnd);

  int32_t count = -(int32_t)br.Bitpos;
  size_t v = (size_t)GetByte(p) & (size_t)(255 >> (br.Bitpos & 31));
  p += 1;
  for (;;)
  {
    if (v == 0)
      count += 8;
    else
    {
      const int32_t x = (int32_t)kRiceCodeBits2Value[v];
      const uint32_t lo = (uint32_t)count + (uint32_t)(x & 0x0f0f0f0f);
      const uint32_t hi = (uint32_t)((x >> 4) & 0x0f0f0f0f);
      for (size_t i = 0; i < 4 && i < dstLen; i++)
        dst[i] = (Byte)(lo >> (8 * i));
      for (size_t i = 0; i < 4 && i + 4 < dstLen; i++)
        dst[4 + i] = (Byte)(hi >> (8 * i));
      const size_t step = kRiceCodeBits2Len[v];
      if (dstLen <= step)
      {
        // went too far, step back
        for (size_t i = dstLen; i < step; i++)
          v &= v - 1;
        break;
      }
      dst += step;
      dstLen -= step;
      count = x >> 28;
    }
    Check(p < pEnd);
    v = GetByte(p);
    p += 1;
  }
  // step back if the byte isn't finished
  uint32_t bitpos = 0;
  if ((v & 1) == 0)
  {
    Check(v != 0);
    bitpos = (uint32_t)(8 - Ctz(v));
    p -= 1;
  }
  br.P = p;
  br.Bitpos = bitpos;
}

void CCore::DecodeGolombRiceBits(Byte *dst, size_t dstLen, size_t bitcount, CBitReader2 &br)
{
  if (bitcount == 0)
    return;
  Ptr p = br.P;
  const uint32_t bitpos = br.Bitpos;

  const size_t bitsRequired = bitpos + bitcount * dstLen;
  const size_t bytesRequired = (bitsRequired + 7) >> 3;
  Check(bytesRequired < br.PEnd - p);

  br.P = p + (bitsRequired >> 3);
  br.Bitpos = (uint32_t)(bitsRequired & 7);

  while (dstLen != 0)
  {
    uint64_t bits;
    switch (bitcount)
    {
      case 1:
        bits = Shr64(Be(p, 4), (int)(24 - bitpos)) & 0xFF;
        p += 1;
        // each bit into a byte of the uint64
        bits = (bits | (bits << 28)) & 0xF0000000FULL;
        bits = (bits | (bits << 14)) & 0x3000300030003ULL;
        bits = (bits | (bits << 7)) & 0x0101010101010101ULL;
        break;
      case 2:
        bits = Shr64(Be(p, 4), (int)(16 - bitpos)) & 0xFFFF;
        p += 2;
        bits = (bits | (bits << 24)) & 0xFF000000FFULL;
        bits = (bits | (bits << 12)) & 0xF000F000F000FULL;
        bits = (bits | (bits << 6)) & 0x0303030303030303ULL;
        break;
      case 3:
        bits = Shr64(Be(p, 4), (int)(8 - bitpos)) & 0xffffff;
        p += 3;
        bits = (bits | (bits << 20)) & 0xFFF00000FFFULL;
        bits = (bits | (bits << 10)) & 0x3F003F003F003FULL;
        bits = (bits | (bits << 5)) & 0x0707070707070707ULL;
        break;
      default:
        Fail();
    }
    const size_t len = std::min(dstLen, (size_t)8);
    uint64_t cur = 0;
    for (size_t i = 0; i < len; i++)
      cur |= (uint64_t)dst[i] << (8 * i);
    const uint64_t v = (cur << bitcount) + __builtin_bswap64(bits);
    for (size_t i = 0; i < len; i++)
      dst[i] = (Byte)(v >> (8 * i));
    dst += len;
    dstLen -= len;
  }
}

std::vector<CHuffRange> CCore::ConvertToRanges(int32_t numSymbols, size_t p, const Byte *syms, size_t symsLen, CBitReader &bits)
{
  int32_t symIdx = 0;
  size_t symlen = (size_t)numSymbols;

  // starts with a space?
  if (p & 1)
  {
    bits.Refill(*this);
    Check(symlen < symsLen);
    const int32_t v = syms[symlen++];
    Check(v < 8);
    symIdx = bits.ReadBitsNoRefill(v + 1) + (1 << (v + 1)) - 1;
  }

  int32_t symsUsed = 0;
  const size_t numRanges = p >> 1;
  std::vector<CHuffRange> ranges;
  ranges.reserve(numRanges + 1);

  for (size_t i = 0; i < numRanges; i++)
  {
    bits.Refill(*this);
    Check(symlen < symsLen);
    int32_t v = syms[symlen++];
    Check(v < 9);
    const int32_t num = bits.ReadBitsNoRefillZero(v) + (1 << v);
    Check(symlen < symsLen);
    v = syms[symlen++];
    Check(v < 8);
    const int32_t space = bits.ReadBitsNoRefill(v + 1) + (1 << (v + 1)) - 1;
    CHuffRange r;
    r.Symbol = (uint16_t)symIdx;
    r.Num = (uint16_t)num;
    ranges.push_back(r);
    symsUsed += num;
    symIdx += num + space;
  }

  Check(symIdx < 256);
  Check(symsUsed < numSymbols);
  Check(symIdx + numSymbols - symsUsed <= 256);

  CHuffRange r;
  r.Symbol = (uint16_t)symIdx;
  r.Num = (uint16_t)(numSymbols - symsUsed);
  ranges.push_back(r);
  return ranges;
}

size_t CCore::DecodeRecursive(Ptr srcOrg, size_t srcSize, Ptr output, size_t outputSize, Ptr scratch)
{
  Ptr src = srcOrg;
  const Ptr outputEnd = output + outputSize;
  const Ptr srcEnd = src + srcSize;

  Check(6 <= srcSize);

  const size_t b = GetByte(src);
  const size_t n = b & 0x7f;
  Check(2 <= n);

  if ((b & 0x80) == 0)
  {
    src += 1;
    for (size_t i = 0; i < n; i++)
    {
      size_t decodedSize = 0;
      const size_t left = outputEnd - output;
      const size_t dec = DecodeBytes(output, src, srcEnd, decodedSize, left, true, scratch);
      output += decodedSize;
      src += dec;
    }
    Check(output == outputEnd);
    return src - srcOrg;
  }
  size_t decodedSize = 0;
  CArray arrays[1];
  const size_t dec = DecodeMultiArray(src, srcEnd, output, outputEnd, arrays, 1, decodedSize, true, scratch);
  output += decodedSize;
  Check(output == outputEnd);
  return dec;
}

size_t CCore::DecodeMultiArray(Ptr srcOrg, Ptr srcEnd, Ptr dst, Ptr dstEnd, CArray *arrays, size_t numArrays,
    size_t &totalSizeOut, bool forceMemmove, Ptr scratch)
{
  Ptr src = srcOrg;
  Check(4 <= srcEnd - src);

  size_t decodedSize = 0;
  size_t numArraysInFile = GetByte(src);
  src += 1;
  Check((numArraysInFile & 0x80) != 0);
  numArraysInFile &= 0x3f;

  size_t totalSize = 0;

  if (numArraysInFile == 0)
  {
    for (size_t i = 0; i < numArrays; i++)
    {
      Ptr chunkDst = dst;
      const size_t dec = DecodeBytes(chunkDst, src, srcEnd, decodedSize, dstEnd - dst, forceMemmove, scratch);
      dst += decodedSize;
      arrays[i].P = chunkDst;
      arrays[i].Size = decodedSize;
      src += dec;
      totalSize += decodedSize;
    }
    totalSizeOut = totalSize;
    return src - srcOrg;
  }

  Ptr entropyArrayData[63];
  size_t entropyArraySize[63];
  memset(entropyArraySize, 0, sizeof(entropyArraySize));

  // first everything is decoded to scratch
  Ptr scratchCur = scratch;
  for (size_t i = 0; i < numArraysInFile; i++)
  {
    Ptr chunkDst = scratchCur;
    const size_t dec = DecodeBytes(chunkDst, src, srcEnd, decodedSize, (size_t)-1, forceMemmove, scratchCur);
    entropyArrayData[i] = chunkDst;
    entropyArraySize[i] = decodedSize;
    scratchCur += decodedSize;
    totalSize += decodedSize;
    src += dec;
  }
  totalSizeOut = totalSize;

  Check(3 <= srcEnd - src);

  const size_t q = (size_t)Le(src, 2);
  src += 2;

  const size_t numIndexes = GetBlockSize(src, srcEnd, totalSize);
  size_t numLens = numIndexes - numArrays;

  Ptr intervalLenlog2 = scratchCur;
  scratchCur += numIndexes;
  Ptr intervalIndexes = scratchCur;
  scratchCur += numIndexes;

  if ((q & 0x8000) != 0)
  {
    size_t sizeOut = 0;
    const size_t n = DecodeBytes(intervalIndexes, src, srcEnd, sizeOut, numIndexes, true, scratchCur);
    Check(sizeOut == numIndexes);
    src += n;
    for (size_t i = 0; i < numIndexes; i++)
    {
      const Byte t = GetByte(intervalIndexes + i);
      Set(intervalLenlog2 + i, (Byte)(t >> 4));
      Set(intervalIndexes + i, (Byte)(t & 0xF));
    }
    numLens = numIndexes;
  }
  else
  {
    const size_t lenlog2Chunksize = numIndexes - numArrays;
    size_t sizeOut = 0;
    size_t n = DecodeBytes(intervalIndexes, src, srcEnd, sizeOut, numIndexes, false, scratchCur);
    Check(sizeOut == numIndexes);
    src += n;
    n = DecodeBytes(intervalLenlog2, src, srcEnd, sizeOut, lenlog2Chunksize, false, scratchCur);
    Check(sizeOut == lenlog2Chunksize);
    src += n;
    for (size_t i = 0; i < lenlog2Chunksize; i++)
      Check(GetByte(intervalLenlog2 + i) <= 16);
  }

  std::vector<uint32_t> decodedIntervals;

  const size_t varbitsComplen = q & 0x3FFF;
  Check(varbitsComplen <= srcEnd - src);

  Ptr f = src;
  uint32_t bitsF = 0;
  int32_t bitposF = 24;
  const Ptr srcEndActual = src + varbitsComplen;
  Ptr b = srcEndActual;
  uint32_t bitsB = 0;
  int32_t bitposB = 24;

  for (size_t i = 0; i < numLens / 2; i++)
  {
    // the 32-bit read may run past the varbits; those bits are masked off
    bitsF |= Shr(GetArrBe32(f), 24 - bitposF);
    f += SizeOf((bitposF + 7) >> 3);

    bitsB |= Shr((uint32_t)Le(b - 4, 4), 24 - bitposB);
    b -= SizeOf((bitposB + 7) >> 3);

    const int32_t numbitsF = GetByte(intervalLenlog2 + i * 2 + 0);
    const int32_t numbitsB = GetByte(intervalLenlog2 + i * 2 + 1);
    Check(numbitsF < 32 && numbitsB < 32);

    bitsF = Rotl(bitsF | 1, numbitsF);
    bitposF += numbitsF - 8 * ((bitposF + 7) >> 3);

    bitsB = Rotl(bitsB | 1, numbitsB);
    bitposB += numbitsB - 8 * ((bitposB + 7) >> 3);

    const uint32_t maskF = (numbitsF == 31) ? 0xFFFFFFFFu : ((2u << numbitsF) - 1);
    const uint32_t maskB = (numbitsB == 31) ? 0xFFFFFFFFu : ((2u << numbitsB) - 1);
    const uint32_t valueF = bitsF & maskF;
    bitsF &= ~maskF;
    const uint32_t valueB = bitsB & maskB;
    bitsB &= ~maskB;

    decodedIntervals.push_back(valueF);
    decodedIntervals.push_back(valueB);
  }

  // the last one: the loop reads 2
  if ((numLens & 1) == 1)
  {
    bitsF |= Shr(GetArrBe32(f), 24 - bitposF);
    const int32_t numbitsF = GetByte(intervalLenlog2 + numLens - 1);
    Check(numbitsF < 32);
    bitsF = Rotl(bitsF | 1, numbitsF);
    const uint32_t maskF = (numbitsF == 31) ? 0xFFFFFFFFu : ((2u << numbitsF) - 1);
    decodedIntervals.push_back(bitsF & maskF);
  }

  Check(GetByte(intervalIndexes + numIndexes - 1) == 0);

  size_t indi = 0;
  size_t leni = 0;
  const bool incrementLeni = (q & 0x8000) != 0;

  for (size_t a = 0; a < numArrays; a++)
  {
    const Ptr start = dst;
    Check(indi < numIndexes);
    for (;;)
    {
      const size_t source = GetByte(intervalIndexes + indi);
      indi += 1;
      if (source == 0)
        break;
      Check(source <= numArraysInFile);
      Check(leni < numLens);
      Check(leni < decodedIntervals.size());
      const size_t curLen = decodedIntervals[leni];
      leni += 1;
      const size_t bytesLeft = entropyArraySize[source - 1];
      Check(curLen <= bytesLeft);
      Check(curLen <= dstEnd - dst);
      const Ptr blksrc = entropyArrayData[source - 1];
      entropyArraySize[source - 1] -= curLen;
      entropyArrayData[source - 1] += curLen;
      CopyBytes(dst, blksrc, curLen);
      dst += curLen;
    }
    if (incrementLeni)
      leni += 1;
    arrays[a].P = start;
    arrays[a].Size = dst - start;
  }

  Check(indi == numIndexes);
  Check(leni == numLens);
  for (size_t i = 0; i < numArraysInFile; i++)
    Check(entropyArraySize[i] == 0);

  return srcEndActual - srcOrg;
}

size_t CCore::GetBlockSize(Ptr srcOrg, Ptr srcEnd, size_t destCapacity)
{
  Ptr src = srcOrg;
  size_t srcSize, dstSize;

  const unsigned chunkType = (GetByte(src) >> 4) & 0x7;
  if (chunkType == 0)
  {
    if (GetByte(src) >= 0x80)
    {
      srcSize = (size_t)Be(src, 2) & 0xFFF;
      src += 2;
    }
    else
    {
      srcSize = (size_t)Be(src, 3);
      Check((srcSize & ~(size_t)0x3ffff) == 0);
      src += 3;
    }
    Check(srcSize <= destCapacity);
    Check(srcSize <= srcEnd - src);
    return srcSize;
  }

  Check(chunkType < 6);

  if (GetByte(src) >= 0x80)
  {
    const size_t bits = (size_t)Be(src, 3);
    srcSize = bits & 0x3ff;
    dstSize = srcSize + ((bits >> 10) & 0x3ff) + 1;
    src += 3;
  }
  else
  {
    const size_t bits = (size_t)Be(src, 5);
    srcSize = bits & 0x3ffff;
    dstSize = (((bits >> 18) | ((size_t)GetByte(src) << 14)) & 0x3FFFF) + 1;
    Check(srcSize < dstSize);
    src += 5;
  }
  Check(srcSize <= srcEnd - src);
  Check(dstSize <= destCapacity);
  return dstSize;
}

size_t CCore::DecodeRle(Ptr src, size_t srcSize, Ptr dst, size_t dstSize, Ptr scratch)
{
  Check(srcSize != 0);

  if (srcSize == 1)
  {
    Memset(dst, GetByte(src), dstSize);
    return 1;
  }

  // the start of the command buffer entropy coded?
  if (GetByte(src) != 0)
  {
    Ptr dstPtr = scratch;
    size_t decSize = 0;
    const size_t n = DecodeBytes(dstPtr, src, src + srcSize, decSize, (size_t)-1, true, scratch);
    Check(0 < n);
    const size_t cmdLen = srcSize - n + decSize;
    CopyBytes(dstPtr + decSize, src + n, srcSize - n);
    DecodeRleUnpacked(dstPtr, dstPtr + cmdLen, dst, dstSize);
  }
  else
    DecodeRleUnpacked(src + 1, src + srcSize, dst, dstSize);
  return srcSize;
}

void CCore::DecodeRleUnpacked(Ptr cmdPtr, Ptr cmdPtrEnd, Ptr dst, size_t dstSize)
{
  const Ptr dstEnd = dst + dstSize;
  Byte rleByte = 0;

  while (cmdPtr < cmdPtrEnd)
  {
    const size_t cmd = GetByte(cmdPtrEnd - 1);
    if (cmd == 0 || cmd > 0x2f)
    {
      cmdPtrEnd -= 1;
      const size_t bytesToCopy = ~cmd & 0xF;
      const size_t bytesToRle = cmd >> 4;
      Check(bytesToCopy + bytesToRle <= dstEnd - dst);
      Check(bytesToCopy <= cmdPtrEnd - cmdPtr);
      CopyBytes(dst, cmdPtr, bytesToCopy);
      cmdPtr += bytesToCopy;
      dst += bytesToCopy;
      Memset(dst, rleByte, bytesToRle);
      dst += bytesToRle;
    }
    else if (cmd >= 0x10)
    {
      cmdPtrEnd -= 2;
      const size_t data = (size_t)Le(cmdPtrEnd, 2) - 4096;
      const size_t bytesToCopy = data & 0x3F;
      const size_t bytesToRle = data >> 6;
      Check(bytesToCopy + bytesToRle <= dstEnd - dst);
      Check(bytesToCopy <= cmdPtrEnd - cmdPtr);
      CopyBytes(dst, cmdPtr, bytesToCopy);
      cmdPtr += bytesToCopy;
      dst += bytesToCopy;
      Memset(dst, rleByte, bytesToRle);
      dst += bytesToRle;
    }
    else if (cmd == 1)
    {
      rleByte = GetByte(cmdPtr);
      cmdPtr += 1;
      cmdPtrEnd -= 1;
    }
    else if (cmd >= 9)
    {
      cmdPtrEnd -= 2;
      const size_t bytesToRle = ((size_t)Le(cmdPtrEnd, 2) - 0x8ff) * 128;
      Check(bytesToRle <= dstEnd - dst);
      Memset(dst, rleByte, bytesToRle);
      dst += bytesToRle;
    }
    else
    {
      cmdPtrEnd -= 2;
      const size_t bytesToCopy = ((size_t)Le(cmdPtrEnd, 2) - 511) * 64;
      Check(bytesToCopy <= cmdPtrEnd - cmdPtr);
      Check(bytesToCopy <= dstEnd - dst);
      CopyBytes(dst, cmdPtr, bytesToCopy);
      dst += bytesToCopy;
      cmdPtr += bytesToCopy;
    }
  }
  Check(cmdPtr == cmdPtrEnd);
  Check(dst == dstEnd);
}

// ---------- tANS ----------

struct CTansLutEnt
{
  uint32_t X;
  Byte BitsX;
  Byte Symbol;
  uint16_t W;
  CTansLutEnt(): X(0), BitsX(0), Symbol(0), W(0) {}
};

struct CTansData
{
  uint32_t AUsed, BUsed;
  Byte A[256];
  uint32_t B[256];
};

struct CTansDecoder
{
  std::vector<CTansLutEnt> Lut;
  Ptr Dst, DstEnd, PtrF, PtrB;
  uint64_t BitsF, BitsB;
  int32_t BitposF, BitposB;
  uint64_t State[5];

  void Decode(CCore &core);
  void InitLut(const CTansData &data, int32_t lBits);
  void DecodeTable(CCore &core, CBitReader &bits, int32_t lBits, CTansData &data);

  void ForwardBits(CCore &core)
  {
    // the 32-bit read may run past the end; those bits aren't used
    BitsF |= Shl64(core.GetArrLe32(PtrF), BitposF);
    PtrF += SizeOf((31 - BitposF) >> 3);
    BitposF |= 24;
  }
  void BackwardBits(CCore &core)
  {
    BitsB |= Shl64(core.Be(PtrB - 4, 4), BitposB);
    PtrB -= SizeOf((31 - BitposB) >> 3);
    BitposB |= 24;
  }
  void Round(CCore &core, size_t i, bool forward)
  {
    Check(State[i] < Lut.size());
    const CTansLutEnt e = Lut[(size_t)State[i]];
    core.Set(Dst, e.Symbol);
    Dst += 1;
    if (forward)
    {
      BitposF -= e.BitsX;
      State[i] = (BitsF & e.X) + e.W;
      BitsF = Shr64(BitsF, e.BitsX);
    }
    else
    {
      BitposB -= e.BitsX;
      State[i] = (BitsB & e.X) + e.W;
      BitsB = Shr64(BitsB, e.BitsX);
    }
  }
};

void CTansDecoder::Decode(CCore &core)
{
  Check(PtrF <= PtrB);
  unsigned step = 0;
  while (Dst < DstEnd)
  {
    if (step < 5)
    {
      if ((step & 1) == 0)
        ForwardBits(core);
      Round(core, step, true);
    }
    else
    {
      if ((step & 1) == 1)
        BackwardBits(core);
      Round(core, step - 5, false);
    }
    step = (step + 1) % 10;
  }
  Check(PtrB + SizeOf(BitposF >> 3) + SizeOf(BitposB >> 3) == PtrF);
  uint64_t statesOr = 0;
  for (unsigned i = 0; i < 5; i++)
    statesOr |= State[i];
  Check((statesOr & ~(uint64_t)0xFF) == 0);
  Byte last[5];
  for (unsigned i = 0; i < 5; i++)
    last[i] = (Byte)State[i];
  core.SetBytes(DstEnd, last, 5);
}

void CTansDecoder::InitLut(const CTansData &data, int32_t lBits)
{
  size_t pointers[4] = { 0, 0, 0, 0 };

  const int32_t l = 1 << lBits;
  const size_t len = (size_t)l;
  const size_t aUsed = data.AUsed;
  Check(aUsed <= len);

  const size_t slotsLeftToAlloc = len - aUsed;
  const size_t sa = slotsLeftToAlloc >> 2;
  size_t sb = sa;
  if ((slotsLeftToAlloc & 3) > 0) sb++;
  pointers[1] = sb;
  sb += sa;
  if ((slotsLeftToAlloc & 3) > 1) sb++;
  pointers[2] = sb;
  sb += sa;
  if ((slotsLeftToAlloc & 3) > 2) sb++;
  pointers[3] = sb;

  Lut.assign(len, CTansLutEnt());
  // the entries with weight 1
  for (size_t i = 0; i < aUsed; i++)
  {
    CTansLutEnt &le = Lut[slotsLeftToAlloc + i];
    le.W = 0;
    le.BitsX = (Byte)lBits;
    le.X = ((uint32_t)1 << lBits) - 1;
    le.Symbol = data.A[i];
  }

  // the entries with weight >= 2
  int32_t weightsSum = 0;
  Check(data.BUsed <= 256);
  for (size_t i = 0; i < data.BUsed; i++)
  {
    const int32_t weight = (int32_t)(data.B[i] & 0xffff);
    const int32_t symbol = (int32_t)(data.B[i] >> 16);
    if (weight > 4)
    {
      const int32_t symBits = Ilog2(weight);
      int32_t z = lBits - symBits;
      CTansLutEnt le;
      le.Symbol = (Byte)symbol;
      le.BitsX = (Byte)z;
      le.X = Shl(1, z) - 1;
      le.W = (uint16_t)((l - 1) & ShlI(weight, z));

      uint16_t whatToAdd = (uint16_t)Shl(1, z);
      int32_t x = ShlI(1, symBits + 1) - weight;

      for (int32_t j = 0; j < 4; j++)
      {
        size_t dst = pointers[j];
        const int32_t y = (weight + ((weightsSum - j - 1) & 3)) >> 2;
        if (x >= y)
        {
          for (int32_t k = 0; k < y; k++)
          {
            Check(dst < len);
            Lut[dst++] = le;
            le.W = (uint16_t)(le.W + whatToAdd);
          }
          x -= y;
        }
        else
        {
          for (int32_t k = 0; k < x; k++)
          {
            Check(dst < len);
            Lut[dst++] = le;
            le.W = (uint16_t)(le.W + whatToAdd);
          }
          z -= 1;
          whatToAdd >>= 1;
          le.BitsX = (Byte)z;
          le.W = 0;
          le.X >>= 1;
          for (int32_t k = 0; k < y - x; k++)
          {
            Check(dst < len);
            Lut[dst++] = le;
            le.W = (uint16_t)(le.W + whatToAdd);
          }
          x = weight;
        }
        pointers[j] = dst;
      }
    }
    else
    {
      Check(weight > 0);
      uint32_t bits = Shl(Shl(1, weight) - 1, weightsSum & 3);
      bits |= bits >> 4;
      int32_t ww = weight;
      for (int32_t k = 0; k < weight; k++)
      {
        const int idx = Ctz(bits);
        Check(idx < 4);
        bits &= bits - 1;
        const size_t dst = pointers[idx]++;
        Check(dst < len);
        Lut[dst].Symbol = (Byte)symbol;
        const int32_t weightBits = Ilog2(ww);
        Lut[dst].BitsX = (Byte)(lBits - weightBits);
        Lut[dst].X = Shl(1, lBits - weightBits) - 1;
        Lut[dst].W = (uint16_t)((l - 1) & ShlI(ww, lBits - weightBits));
        ww++;
      }
    }
    weightsSum += weight;
  }
}

void CTansDecoder::DecodeTable(CCore &core, CBitReader &bits, int32_t lBits, CTansData &data)
{
  data.AUsed = 0;
  data.BUsed = 0;
  memset(data.A, 0, sizeof(data.A));
  memset(data.B, 0, sizeof(data.B));
  bits.Refill(core);
  if (bits.ReadBitNoRefill())
  {
    const int32_t q = bits.ReadBitsNoRefill(3);
    const int32_t numSymbols = bits.ReadBitsNoRefill(8) + 1;
    Check(numSymbols >= 2);
    const size_t fluff = bits.ReadFluff(numSymbols);
    const size_t totalRiceValues = (size_t)numSymbols + fluff;
    Byte rice[512 + 16];
    memset(rice, 0, sizeof(rice));

    CBitReader2 br2;
    br2.P = bits.P - (size_t)(uint32_t)((24 - bits.Bitpos + 7) >> 3);
    br2.PEnd = bits.PEnd;
    br2.Bitpos = (uint32_t)((bits.Bitpos - 24) & 7);

    Check(totalRiceValues <= sizeof(rice));
    core.DecodeGolombRiceLengths(rice, totalRiceValues, br2);

    // back to the other bit reader
    bits.Bitpos = 24;
    bits.P = br2.P;
    bits.Bits = 0;
    bits.Refill(core);
    bits.Bits = Shl(bits.Bits, (int)br2.Bitpos);
    bits.Bitpos += (int32_t)br2.Bitpos;

    const std::vector<CHuffRange> ranges = core.ConvertToRanges(numSymbols, fluff, rice, sizeof(rice), bits);

    bits.Refill(core);

    const int32_t l = 1 << lBits;
    size_t curRice = 0;
    int32_t average = 6;
    int32_t somesum = 0;
    size_t aIdx = 0, bIdx = 0;

    for (size_t r = 0; r < ranges.size(); r++)
    {
      int32_t symbol = ranges[r].Symbol;
      for (unsigned k = 0; k < ranges[r].Num; k++)
      {
        bits.Refill(core);
        Check(curRice < sizeof(rice));
        const int32_t nextra = rice[curRice++] + q;
        Check(nextra <= 15);
        int32_t v = bits.ReadBitsNoRefillZero(nextra) + (1 << nextra) - (1 << q);

        const int32_t averageDiv4 = average >> 2;
        int32_t limit = 2 * averageDiv4;
        if (v <= limit)
          v = averageDiv4 + ((int32_t)((uint32_t)v >> 1) ^ -(v & 1));
        if (limit > v)
          limit = v;
        v += 1;
        average += limit - averageDiv4;
        Check(aIdx < 256 && bIdx < 256);
        data.A[aIdx] = (Byte)symbol;
        data.B[bIdx] = (uint32_t)(ShlI(symbol, 16) + v);
        if (v == 1)
          aIdx++;
        if (v >= 2)
          bIdx++;
        somesum += v;
        symbol += 1;
      }
    }
    data.AUsed = (uint32_t)aIdx;
    data.BUsed = (uint32_t)bIdx;
    Check(somesum == l);
    return;
  }

  bool seen[256];
  memset(seen, 0, sizeof(seen));
  const int32_t l = 1 << lBits;

  const int32_t count = bits.ReadBitsNoRefill(3) + 1;
  const int bitsPerSym = Ilog2(lBits) + 1;
  const int32_t maxDeltaBits = bits.ReadBitsNoRefill(bitsPerSym);
  Check(maxDeltaBits != 0);
  Check(maxDeltaBits <= lBits);

  size_t aIdx = 0, bIdx = 0;
  int32_t weight = 0;
  int32_t totalWeights = 0;

  for (int32_t i = 0; i < count; i++)
  {
    bits.Refill(core);
    const int32_t sym = bits.ReadBitsNoRefill(8);
    Check(!seen[sym]);
    const int32_t delta = bits.ReadBitsNoRefill(maxDeltaBits);
    weight += delta;
    Check(weight != 0);
    seen[sym] = true;
    Check(aIdx < 256 && bIdx < 256);
    if (weight == 1)
      data.A[aIdx++] = (Byte)sym;
    else
      data.B[bIdx++] = (uint32_t)(ShlI(sym, 16) + weight);
    totalWeights += weight;
  }

  bits.Refill(core);
  const int32_t sym = bits.ReadBitsNoRefill(8);
  Check(!seen[sym]);
  Check(l - totalWeights >= weight);
  Check(l - totalWeights > 1);
  Check(bIdx < 256);
  data.B[bIdx++] = (uint32_t)(ShlI(sym, 16) + (l - totalWeights));

  data.AUsed = (uint32_t)aIdx;
  data.BUsed = (uint32_t)bIdx;
  std::sort(data.A, data.A + aIdx);
  std::sort(data.B, data.B + bIdx);
}

size_t CCore::DecodeTans(Ptr src, size_t srcSize, Ptr dst, size_t dstSize)
{
  Check(8 <= srcSize);
  Check(5 <= dstSize);

  Ptr srcEnd = src + srcSize;

  CBitReader br;
  br.Bitpos = 24; br.Bits = 0; br.P = src; br.PEnd = srcEnd;
  br.Refill(*this);

  Check(!br.ReadBitNoRefill());   // reserved

  const int32_t lBits = br.ReadBitsNoRefill(2) + 8;

  CTansDecoder decoder;
  CTansData data;
  decoder.DecodeTable(*this, br, lBits, data);

  src = br.P - SizeOf((24 - br.Bitpos) / 8);
  Check(src < srcEnd);

  decoder.Dst = dst;
  decoder.DstEnd = dst + dstSize - 5;
  decoder.InitLut(data, lBits);

  // the initial state
  const uint64_t lMask = ((uint64_t)1 << lBits) - 1;
  uint64_t bitsF = Le(src, 4);
  src += 4;
  srcEnd -= 4;
  uint64_t bitsB = Be(srcEnd, 4);
  int32_t bitposF = 32;
  int32_t bitposB = 32;

  // the first two
  decoder.State[0] = bitsF & lMask;
  decoder.State[1] = bitsB & lMask;
  bitsF = Shr64(bitsF, lBits); bitposF -= lBits;
  bitsB = Shr64(bitsB, lBits); bitposB -= lBits;

  // the next two
  decoder.State[2] = bitsF & lMask;
  decoder.State[3] = bitsB & lMask;
  bitsF = Shr64(bitsF, lBits); bitposF -= lBits;
  bitsB = Shr64(bitsB, lBits); bitposB -= lBits;

  // more bits
  bitsF |= Shl64(Le(src, 4), bitposF);
  src += SizeOf((31 - bitposF) >> 3);
  bitposF |= 24;

  // the last state
  decoder.State[4] = bitsF & lMask;
  bitsF = Shr64(bitsF, lBits);
  bitposF -= lBits;

  decoder.BitsF = bitsF;
  decoder.PtrF = src - SizeOf(bitposF >> 3);
  decoder.BitposF = bitposF & 7;

  decoder.BitsB = bitsB;
  decoder.PtrB = srcEnd + SizeOf(bitposB >> 3);
  decoder.BitposB = bitposB & 7;

  decoder.Decode(*this);
  return srcSize;
}

// ---------- Kraken ----------

struct CKraken
{
  // the literals and match lengths are decoded first, then the copy loop runs
  Ptr CmdStream;              // (literal, match) pairs: lengths, recent offset
  size_t CmdStreamSize;
  std::vector<int32_t> OffsStream;   // distances not from a recent offset
  Ptr LitStream;
  size_t LitStreamSize;
  std::vector<int32_t> LenStream;    // lengths too long for the command

  void ReadLzTable(CCore &core, Ptr src, Ptr srcEnd, Ptr dst, size_t dstSize, size_t offset);
  void ProcessLzRuns(CCore &core, bool mode0, Ptr dst, size_t dstSize, size_t offset);

  void Process(CCore &core, size_t mode, Ptr src, size_t srcUsed, Ptr dstStart, Ptr dst, size_t dstSize)
  {
    CmdStreamSize = 0;
    LitStreamSize = 0;
    OffsStream.clear();
    LenStream.clear();
    Check(mode <= 1);
    const size_t offset = dst - dstStart;
    ReadLzTable(core, src, src + srcUsed, dst, dstSize, offset);
    ProcessLzRuns(core, mode == 0, dst, dstSize, offset);
  }
};

void CKraken::ReadLzTable(CCore &core, Ptr src, Ptr srcEnd, Ptr dst, size_t dstSize, size_t offset)
{
  size_t decodeCount = 0;
  Ptr scratch = ScratchP(0);

  Check(13 <= srcEnd - src);

  if (offset == 0)
  {
    core.CopyBytes(dst, src, 8);
    dst += 8;
    src += 8;
  }

  const size_t flag = core.GetByte(src);
  if (flag & 0x80)
  {
    src += 1;
    Check((flag & 0xc0) == 0x80);   // reserved flag
    Fail();                          // excess bytes aren't supported
  }

  // the literals, at most dstSize
  Ptr out = scratch;
  size_t n = core.DecodeBytes(out, src, srcEnd, decodeCount, dstSize, false, scratch);
  LitStream = out;
  LitStreamSize = decodeCount;
  src += n;
  scratch += decodeCount;

  // the commands, at most dstSize
  out = scratch;
  n = core.DecodeBytes(out, src, srcEnd, decodeCount, dstSize, false, scratch);
  src += n;
  CmdStream = out;
  CmdStreamSize = decodeCount;
  scratch += decodeCount;

  Check(3 <= srcEnd - src);

  int32_t offsScaling = 0;
  bool haveExtra = false;
  Ptr packedOffsStreamExtra;
  Ptr packedOffsStream;
  size_t offsStreamSize = 0;
  if (core.GetByte(src) & 0x80)
  {
    // distances coded with 2 tables
    offsScaling = (int32_t)core.GetByte(src) - 127;
    src += 1;

    packedOffsStream = scratch;
    n = core.DecodeBytes(packedOffsStream, src, srcEnd, offsStreamSize, CmdStreamSize, false, scratch);
    src += n;
    scratch += offsStreamSize;

    if (offsScaling != 1)
    {
      haveExtra = true;
      packedOffsStreamExtra = scratch;
      n = core.DecodeBytes(packedOffsStreamExtra, src, srcEnd, decodeCount, offsStreamSize, false, scratch);
      Check(decodeCount == offsStreamSize);
      src += n;
      scratch += decodeCount;
    }
  }
  else
  {
    // the packed offsets, at most the command count
    packedOffsStream = scratch;
    n = core.DecodeBytes(packedOffsStream, src, srcEnd, offsStreamSize, CmdStreamSize, false, scratch);
    src += n;
    scratch += offsStreamSize;
  }

  // the packed lengths, at most a quarter of dstSize
  Ptr packedLenStream = scratch;
  size_t lenStreamSize = 0;
  n = core.DecodeBytes(packedLenStream, src, srcEnd, lenStreamSize, dstSize >> 2, false, scratch);
  src += n;
  scratch += lenStreamSize;

  OffsStream.assign(offsStreamSize, 0);
  LenStream.assign(lenStreamSize, 0);

  core.UnpackOffsets(src, srcEnd, packedOffsStream, haveExtra ? packedOffsStreamExtra : Ptr(), offsScaling,
      packedLenStream, OffsStream, LenStream, false);
}

void CKraken::ProcessLzRuns(CCore &core, bool mode0, Ptr dst, size_t dstSize, size_t offset)
{
  const Ptr dstEnd = dst + dstSize;
  if (offset == 0)
    dst += 8;

  Ptr cmdStream = CmdStream;
  const Ptr cmdStreamEnd = cmdStream + CmdStreamSize;
  size_t lenIdx = 0, offsIdx = 0;
  Ptr litStream = LitStream;
  const Ptr litStreamEnd = LitStream + LitStreamSize;
  int32_t recentOffs[7] = { 0, 0, 0, -8, -8, -8, 0 };
  int32_t lastOffset = -8;

  while (cmdStream < cmdStreamEnd)
  {
    const size_t f = core.GetByte(cmdStream);
    cmdStream += 1;
    size_t litlen = f & 3;
    const size_t offsIndex = f >> 6;
    size_t matchlen = (f >> 2) & 0xF;

    if (litlen == 3)
    {
      Check(lenIdx < LenStream.size());
      litlen = SizeOf(LenStream[lenIdx++]);
    }
    recentOffs[6] = (offsIdx < OffsStream.size()) ? OffsStream[offsIdx] : 0;

    if (mode0)
      core.Copy64Add(dst, litStream, dst + SizeOf(lastOffset), litlen);
    else
      core.CopyBytes(dst, litStream, litlen);
    dst += litlen;
    litStream += litlen;

    const int32_t offs = recentOffs[offsIndex + 3];
    memmove(recentOffs + offsIndex + 1, recentOffs + offsIndex, 3 * sizeof(int32_t));
    recentOffs[3] = offs;
    lastOffset = offs;

    if (offsIndex == 3)
      offsIdx++;

    const Ptr copyfrom = dst + SizeOf(offs);
    if (matchlen != 15)
    {
      core.RepeatCopy64(dst, copyfrom, matchlen + 2);
      dst += matchlen + 2;
    }
    else
    {
      Check(lenIdx < LenStream.size());
      matchlen = SizeOf((int32_t)(14 + (uint32_t)LenStream[lenIdx++]));
      core.RepeatCopy64(dst, copyfrom, matchlen);
      dst += matchlen;
    }
  }

  // the input must be used up
  Check(offsIdx >= OffsStream.size());
  Check(lenIdx == LenStream.size());

  const size_t finalLen = dstEnd - dst;
  Check(finalLen == litStreamEnd - litStream);

  if (mode0)
    core.Copy64Add(dst, litStream, dst + SizeOf(lastOffset), finalLen);
  else
    core.CopyBytes(dst, litStream, finalLen);
}

// ---------- Mermaid and Selkie: the same format ----------

struct CMermaid
{
  // the commands: a byte from CmdStream
  //   >= 24: & 7 literals from LitStream, then (>> 3) & 0xF bytes from the
  //          recent offset, which first reads Off16Stream unless bit 7 is set
  //   0:     64 + a length from LengthStream literals
  //   1:     91 + a length bytes from the next Off16Stream offset
  //   2:     29 + a length bytes from the next Off32Stream offset
  //   > 2:   cmd + 5 bytes from the next Off32Stream offset
  //   (a length is a byte, plus 4 * a 16-bit word when the byte is over 251)
  Ptr CmdStream, CmdStreamEnd;
  Ptr LengthStream;
  Ptr LitStream, LitStreamEnd;
  std::vector<uint16_t> Off16Stream;
  size_t Off16Pos;
  std::vector<uint32_t> Off32Stream1, Off32Stream2;
  const std::vector<uint32_t> *Off32Stream;
  size_t CmdStream2Offs, CmdStream2OffsEnd;

  void Process(CCore &core, size_t mode, Ptr src, size_t srcUsed, Ptr dstStart, Ptr dst, size_t dstSize)
  {
    const size_t offset = dst - dstStart;
    Off16Stream.clear();
    Off16Pos = 0;
    Off32Stream1.clear();
    Off32Stream2.clear();
    Off32Stream = &Off32Stream1;
    CmdStream2Offs = CmdStream2OffsEnd = 0;
    LengthStream = Ptr();
    ReadLzTable(core, mode, src, src + srcUsed, dst, dstSize, offset);
    ProcessLzRuns(core, mode, src + srcUsed, dst, dstSize, offset);
  }

  uint16_t PopOff16()
  {
    Check(Off16Pos < Off16Stream.size());
    return Off16Stream[Off16Pos++];
  }

  void ReadLzTable(CCore &core, size_t mode, Ptr src, Ptr srcEnd, Ptr dst, size_t dstSize, size_t offset);
  size_t DecodeFarOffsets(CCore &core, Ptr src, Ptr srcEnd, std::vector<uint32_t> &stream, size_t outputSize, size_t offset);
  void ProcessLzRuns(CCore &core, size_t mode, Ptr srcEnd, Ptr dst, size_t dstSize, size_t offset);
  void Run(CCore &core, bool addMode, Ptr dst, size_t dstSize, Ptr srcEnd, int32_t &savedDist, size_t startoff);
  size_t ReadLength(CCore &core, Ptr &lengthStream, Ptr srcEnd)
  {
    Check(lengthStream < srcEnd);
    size_t length = core.GetByte(lengthStream);
    if (length > 251)
    {
      Check((srcEnd - lengthStream) >= 3);
      length += (size_t)core.Le(lengthStream + 1, 2) * 4;
      lengthStream += 2;
    }
    lengthStream += 1;
    return length;
  }
};

void CMermaid::ProcessLzRuns(CCore &core, size_t mode, Ptr srcEnd, Ptr dst, size_t dstSize, size_t offset)
{
  int32_t savedDist = -8;
  for (unsigned iteration = 0; iteration < 2; iteration++)
  {
    const size_t dstSizeCur = std::min(dstSize, (size_t)0x10000);
    if (iteration == 0)
    {
      Off32Stream = &Off32Stream1;
      CmdStreamEnd = CmdStream + CmdStream2Offs;
    }
    else
    {
      Off32Stream = &Off32Stream2;
      CmdStreamEnd = CmdStream + CmdStream2OffsEnd;
      CmdStream += CmdStream2Offs;
    }
    const size_t startoff = (offset == 0 && iteration == 0) ? 8 : 0;
    Run(core, mode == 0, dst, dstSizeCur, srcEnd, savedDist, startoff);
    Check(LengthStream.D != kNull);
    dst += dstSizeCur;
    dstSize -= dstSizeCur;
    if (dstSize == 0)
      break;
  }
  Check(LengthStream == srcEnd);
}

void CMermaid::Run(CCore &core, bool addMode, Ptr dst, size_t dstSize, Ptr srcEnd, int32_t &savedDist, size_t startoff)
{
  const Ptr dstEnd = dst + dstSize;
  Ptr cmdStream = CmdStream;
  const Ptr cmdStreamEnd = CmdStreamEnd;
  Ptr lengthStream = LengthStream;
  Ptr litStream = LitStream;
  const Ptr litStreamEnd = LitStreamEnd;
  const std::vector<uint32_t> &off32 = *Off32Stream;
  size_t off32Pos = 0;
  int32_t recentOffs = savedDist;
  const Ptr dstBegin = dst;

  dst += startoff;

  while (cmdStream < cmdStreamEnd)
  {
    const size_t cmd = core.GetByte(cmdStream);
    cmdStream += 1;
    if (cmd >= 24)
    {
      const size_t litlen = cmd & 7;
      if (addMode)
        core.Copy64Add(dst, litStream, dst + SizeOf(recentOffs), litlen);
      else
        core.RepeatCopy64(dst, litStream, litlen);
      dst += litlen;
      litStream += litlen;
      if ((cmd >> 7) == 0)
        recentOffs = -(int32_t)PopOff16();
      const Ptr offsPtr = dst + SizeOf(recentOffs);
      core.RepeatCopy64(dst, offsPtr, (cmd >> 3) & 0xF);
      dst += (cmd >> 3) & 0xF;
    }
    else if (cmd > 2)
    {
      const size_t length = cmd + 5;
      Check(off32Pos < off32.size());
      const Ptr offsPtr = dstBegin - (size_t)off32[off32Pos++];
      recentOffs = (int32_t)((uint32_t)offsPtr.I - (uint32_t)dst.I);
      Check((dstEnd - dst) >= length);
      core.RepeatCopy64(dst, offsPtr, length);
      dst += length;
    }
    else if (cmd == 0)
    {
      const size_t length = ReadLength(core, lengthStream, srcEnd) + 64;
      Check((dstEnd - dst) >= length);
      Check((litStreamEnd - litStream) >= length);
      if (addMode)
        core.Copy64Add(dst, litStream, dst + SizeOf(recentOffs), length);
      else
        core.RepeatCopy64(dst, litStream, length);
      dst += length;
      litStream += length;
    }
    else if (cmd == 1)
    {
      const size_t length = ReadLength(core, lengthStream, srcEnd) + 91;
      const Ptr offsPtr = dst - (size_t)PopOff16();
      recentOffs = (int32_t)((uint32_t)offsPtr.I - (uint32_t)dst.I);
      core.RepeatCopy64(dst, offsPtr, length);
      dst += length;
    }
    else
    {
      // 2
      const size_t length = ReadLength(core, lengthStream, srcEnd) + 29;
      Check(off32Pos < off32.size());
      const Ptr offsPtr = dstBegin - (size_t)off32[off32Pos++];
      recentOffs = (int32_t)((uint32_t)offsPtr.I - (uint32_t)dst.I);
      core.RepeatCopy64(dst, offsPtr, length);
      dst += length;
    }
  }

  const size_t length = dstEnd - dst;
  if (addMode)
    core.Copy64Add(dst, litStream, dst + SizeOf(recentOffs), length);
  else
    core.RepeatCopy64(dst, litStream, length);
  litStream += length;

  savedDist = recentOffs;
  LengthStream = lengthStream;
  LitStream = litStream;
}

void CMermaid::ReadLzTable(CCore &core, size_t mode, Ptr src, Ptr srcEnd, Ptr dst, size_t dstSize, size_t offset)
{
  size_t decodeCount = 0;
  Ptr scratch = TmpP(0);

  Check(mode <= 1);
  Check((srcEnd - src) >= 10);

  if (offset == 0)
  {
    core.CopyBytes(dst, src, 8);
    dst += 8;
    src += 8;
  }

  // the literals
  Ptr out = scratch;
  src += core.DecodeBytes(out, src, srcEnd, decodeCount, dstSize, false, ScratchP(0));
  LitStream = out;
  LitStreamEnd = out + decodeCount;
  scratch += decodeCount;

  // the commands
  out = scratch;
  src += core.DecodeBytes(out, src, srcEnd, decodeCount, dstSize, false, ScratchP(0));
  CmdStream = out;
  CmdStreamEnd = out + decodeCount;
  scratch += decodeCount;

  CmdStream2OffsEnd = decodeCount;
  if (dstSize <= 0x10000)
    CmdStream2Offs = decodeCount;
  else
  {
    Check((srcEnd - src) >= 2);
    CmdStream2Offs = (size_t)core.Le(src, 2);
    src += 2;
    Check(CmdStream2Offs <= CmdStream2OffsEnd);
  }

  Check((srcEnd - src) >= 2);
  const size_t off16Count = (size_t)core.Le(src, 2);
  src += 2;
  if (off16Count == 0xffff)
  {
    // entropy coded
    size_t off16LoCount = 0, off16HiCount = 0;
    Ptr off16Hi = scratch;
    src += core.DecodeBytes(off16Hi, src, srcEnd, off16HiCount, dstSize >> 1, false, ScratchP(0));
    scratch += off16HiCount;

    Ptr off16Lo = scratch;
    src += core.DecodeBytes(off16Lo, src, srcEnd, off16LoCount, dstSize >> 1, false, ScratchP(0));
    scratch += off16LoCount;

    Check(off16LoCount == off16HiCount);
    Off16Stream.reserve(off16LoCount);
    for (size_t i = 0; i < off16LoCount; i++)
      Off16Stream.push_back((uint16_t)(core.GetByte(off16Lo + i) + core.GetByte(off16Hi + i) * 256));
  }
  else
  {
    const Byte *p = core.Slice(src, off16Count * 2);
    Off16Stream.resize(off16Count);
    for (size_t i = 0; i < off16Count; i++)
      Off16Stream[i] = (uint16_t)(p[i * 2] | (p[i * 2 + 1] << 8));
    src += off16Count * 2;
  }

  Check((srcEnd - src) >= 3);
  const size_t tmp = (size_t)core.Le(src, 3);
  src += 3;

  if (tmp != 0)
  {
    size_t off32Size1 = tmp >> 12;
    size_t off32Size2 = tmp & 0xFFF;
    if (off32Size1 == 4095)
    {
      Check((srcEnd - src) >= 2);
      off32Size1 = (size_t)core.Le(src, 2);
      src += 2;
    }
    if (off32Size2 == 4095)
    {
      Check((srcEnd - src) >= 2);
      off32Size2 = (size_t)core.Le(src, 2);
      src += 2;
    }
    src += DecodeFarOffsets(core, src, srcEnd, Off32Stream1, off32Size1, offset);
    src += DecodeFarOffsets(core, src, srcEnd, Off32Stream2, off32Size2, offset + 0x10000);
  }
  LengthStream = src;
}

size_t CMermaid::DecodeFarOffsets(CCore &core, Ptr src, Ptr srcEnd, std::vector<uint32_t> &stream, size_t outputSize, size_t offset)
{
  Ptr srcCur = src;
  for (size_t i = 0; i < outputSize; i++)
  {
    Check((srcEnd - srcCur) >= 3);
    size_t off = (size_t)core.Le(srcCur, 3);
    srcCur += 3;
    if (offset >= 0xC00000 - 1 && off >= 0xc00000)
    {
      Check(srcCur != srcEnd);
      off += (size_t)core.GetByte(srcCur) << 22;
      srcCur += 1;
    }
    Check(off <= offset);
    stream.push_back((uint32_t)off);
  }
  return srcCur - src;
}

// ---------- Leviathan ----------

struct CLeviathan
{
  std::vector<int32_t> OffsStream;
  std::vector<int32_t> LenStream;
  CArray LitStream[16];
  size_t LitStreamTotal;
  CArray MultiCmdPtr[8];
  bool HaveCmdStream;
  Ptr CmdStream;
  size_t CmdStreamSize;

  void Process(CCore &core, size_t mode, Ptr src, size_t srcUsed, Ptr dstStart, Ptr dst, size_t dstSize)
  {
    const size_t offset = dst - dstStart;
    OffsStream.clear();
    LenStream.clear();
    // unused streams point at tmp[0], as oozextract's defaults do
    for (unsigned i = 0; i < 16; i++) { LitStream[i].P = TmpP(0); LitStream[i].Size = 0; }
    for (unsigned i = 0; i < 8; i++) { MultiCmdPtr[i].P = TmpP(0); MultiCmdPtr[i].Size = 0; }
    LitStreamTotal = 0;
    HaveCmdStream = false;
    CmdStream = Ptr();
    CmdStreamSize = 0;
    ReadLzTable(core, mode, src, src + srcUsed, dst, dstSize, offset);
    ProcessLzRuns(core, mode, dst, dstSize, offset);
  }

  void ReadLzTable(CCore &core, size_t chunkType, Ptr src, Ptr srcEnd, Ptr dst, size_t dstSize, size_t offset);
  void ProcessLzRuns(CCore &core, size_t mode, Ptr dst, size_t dstSize, size_t offset);
};

void CLeviathan::ReadLzTable(CCore &core, size_t chunkType, Ptr src, Ptr srcEnd, Ptr dst, size_t dstSize, size_t offset)
{
  Ptr tmp = TmpP(0);
  const Ptr scratch = ScratchP(0);
  size_t offsStreamSize = 0, lenStreamSize = 0, decodeCount = 0;

  Check(chunkType <= 5);
  Check(13 <= srcEnd - src);

  if (offset == 0)
  {
    core.CopyBytes(dst, src, 8);
    dst += 8;
    src += 8;
  }

  int32_t offsScaling = 0;
  bool haveExtra = false;
  Ptr packedOffsStreamExtra;
  const size_t offsStreamLimit = dstSize / 3;
  Ptr packedOffsStream;

  if ((core.GetByte(src) & 0x80) == 0)
  {
    // the packed offsets, at most the command count
    packedOffsStream = tmp;
    src += core.DecodeBytes(packedOffsStream, src, srcEnd, offsStreamSize, offsStreamLimit, false, scratch);
    tmp += offsStreamSize;
  }
  else
  {
    // distances coded with 2 tables, offs * scaling + low bits
    offsScaling = (int32_t)core.GetByte(src) - 127;
    src += 1;

    packedOffsStream = tmp;
    src += core.DecodeBytes(packedOffsStream, src, srcEnd, offsStreamSize, offsStreamLimit, false, scratch);
    tmp += offsStreamSize;

    if (offsScaling != 1)
    {
      haveExtra = true;
      packedOffsStreamExtra = tmp;
      src += core.DecodeBytes(packedOffsStreamExtra, src, srcEnd, decodeCount, offsStreamLimit, false, scratch);
      Check(decodeCount == offsStreamSize);
      tmp += decodeCount;
    }
  }

  // the packed lengths, at most a fifth of dstSize
  Ptr packedLenStream = tmp;
  src += core.DecodeBytes(packedLenStream, src, srcEnd, lenStreamSize, dstSize / 5, false, scratch);
  tmp += lenStreamSize;

  OffsStream.assign(offsStreamSize, 0);
  LenStream.assign(lenStreamSize, 0);

  if (chunkType <= 1)
  {
    // the literals, at most dstSize
    Ptr out = tmp;
    src += core.DecodeBytes(out, src, srcEnd, decodeCount, dstSize, true, scratch);
    LitStream[0].P = out;
    LitStream[0].Size = decodeCount;
  }
  else
  {
    const size_t arrayCount = (chunkType == 2) ? 2 : (chunkType == 3) ? 4 : 16;
    src += core.DecodeMultiArray(src, srcEnd, tmp, TmpP((size_t)-1), LitStream, arrayCount, decodeCount, true, scratch);
  }
  tmp += decodeCount;
  LitStreamTotal = decodeCount;

  Check(src < srcEnd);

  const Byte flag = core.GetByte(src);
  if ((flag & 0x80) == 0)
  {
    // the commands, at most dstSize
    Ptr out = tmp;
    src += core.DecodeBytes(out, src, srcEnd, decodeCount, dstSize, true, scratch);
    HaveCmdStream = true;
    CmdStream = out;
    CmdStreamSize = decodeCount;
    tmp += decodeCount;
  }
  else
  {
    Check(flag == 0x83);
    src += 1;
    src += core.DecodeMultiArray(src, srcEnd, tmp, TmpP((size_t)-1), MultiCmdPtr, 8, decodeCount, true, scratch);
    HaveCmdStream = false;
    CmdStreamSize = decodeCount;
    tmp += decodeCount;
  }

  core.UnpackOffsets(src, srcEnd, packedOffsStream, haveExtra ? packedOffsStreamExtra : Ptr(), offsScaling,
      packedLenStream, OffsStream, LenStream, false);
}

// how the literals are coded: 0 sub (added to the byte at the last
// offset), 1 raw, 2 sub with the first of each run separate, 3 and 5 sub
// from 4 or 16 streams by position, 4 order-1 context
struct CLevMode
{
  size_t Mode;
  Ptr LitStreams[16];
  Ptr LamLitStream;
  Byte NextLit[16];
  Byte Context;

  void Init(CCore &core, const CLeviathan &lzt, size_t mode, Ptr dstStart)
  {
    Mode = mode;
    Context = 0;
    memset(NextLit, 0, sizeof(NextLit));
    LitStreams[0] = lzt.LitStream[0].P;
    if (mode == 2)
      LamLitStream = lzt.LitStream[1].P;
    else if (mode == 3 || mode == 5)
    {
      const size_t mask = (mode == 3) ? 3 : 15;
      for (size_t i = 0; i <= mask; i++)
        LitStreams[i] = lzt.LitStream[(i - dstStart.I) & mask].P;
    }
    else if (mode == 4)
    {
      for (unsigned i = 0; i < 16; i++)
      {
        LitStreams[i] = lzt.LitStream[i].P + 1;
        NextLit[i] = core.GetByte(lzt.LitStream[i].P);
      }
    }
  }

  void CopyLiteralSubAnd(CCore &core, Ptr &dst, int32_t lastOffset)
  {
    const size_t mask = (Mode == 3) ? 3 : 15;
    Ptr &v = LitStreams[dst.I & mask];
    const Byte b = core.GetByte(v);
    core.Set(dst, (Byte)(b + core.GetByte(dst + SizeOf(lastOffset))));
    v += 1;
    dst += 1;
  }

  void CopyLiteralO1(CCore &core, Ptr &dst)
  {
    const size_t slot = Context >> 4;
    Context = NextLit[slot];
    core.Set(dst, Context);
    dst += 1;
    NextLit[slot] = core.GetByte(LitStreams[slot]);
    LitStreams[slot] += 1;
  }

  void CopyLiterals(CCore &core, size_t cmd, Ptr &dst, const std::vector<int32_t> &lens, size_t &lenFront, size_t lenBack,
      Ptr matchZoneEnd, int32_t lastOffset)
  {
    switch (Mode)
    {
      case 0:
      case 1:
      {
        size_t litlen = (cmd >> 3) & 3;
        if (litlen == 3)
        {
          Check(lenFront < lenBack);
          litlen = (size_t)(lens[lenFront++] & 0xffffff);
        }
        if (Mode == 0)
          core.Copy64Add(dst, LitStreams[0], dst + SizeOf(lastOffset), litlen);
        else
          core.RepeatCopy64(dst, LitStreams[0], litlen);
        dst += litlen;
        LitStreams[0] += litlen;
        break;
      }
      case 2:
      {
        const size_t litCmd = cmd & 0x18;
        if (litCmd == 0)
          return;
        size_t litlen = litCmd >> 3;
        if (litlen == 3)
        {
          Check(lenFront < lenBack);
          litlen = (size_t)(lens[lenFront++] & 0xffffff);
        }
        Check(litlen != 0);
        Check(litlen < matchZoneEnd - dst);
        litlen -= 1;
        const Byte lamByte = (Byte)(core.GetByte(LamLitStream) + core.GetByte(dst + SizeOf(lastOffset)));
        core.Set(dst, lamByte);
        LamLitStream += 1;
        dst += 1;
        core.Copy64Add(dst, LitStreams[0], dst + SizeOf(lastOffset), litlen);
        dst += litlen;
        LitStreams[0] += litlen;
        break;
      }
      case 3:
      case 5:
      {
        const size_t litCmd = cmd & 0x18;
        if (litCmd == 0x18)
        {
          Check(lenFront < lenBack);
          const size_t litlen = SizeOf(lens[lenFront++]) & 0xffffff;
          Check(litlen <= matchZoneEnd - dst);
          for (size_t i = 0; i < litlen; i++)
            CopyLiteralSubAnd(core, dst, lastOffset);
        }
        else if (litCmd != 0)
        {
          CopyLiteralSubAnd(core, dst, lastOffset);
          if (litCmd == 0x10)
            CopyLiteralSubAnd(core, dst, lastOffset);
        }
        break;
      }
      case 4:
      {
        const size_t litCmd = cmd & 0x18;
        if (litCmd == 0x18)
        {
          Check(lenFront < lenBack);
          const int32_t litlen = lens[lenFront++];
          Check(0 < litlen);
          Context = core.GetByte(dst - 1);
          for (int32_t i = 0; i < litlen; i++)
            CopyLiteralO1(core, dst);
        }
        else if (litCmd != 0)
        {
          Context = core.GetByte(dst - 1);
          CopyLiteralO1(core, dst);
          if (litCmd == 0x10)
            CopyLiteralO1(core, dst);
        }
        break;
      }
      default:
        Fail();
    }
  }

  void CopyFinalLiterals(CCore &core, size_t finalLen, Ptr &dst, int32_t lastOffset)
  {
    switch (Mode)
    {
      case 0:
        core.Copy64Add(dst, LitStreams[0], dst + SizeOf(lastOffset), finalLen);
        dst += finalLen;
        break;
      case 1:
        core.RepeatCopy64(dst, LitStreams[0], finalLen);
        dst += finalLen;
        break;
      case 2:
      {
        const Byte lamByte = (Byte)(core.GetByte(LamLitStream) + core.GetByte(dst + SizeOf(lastOffset)));
        core.Set(dst, lamByte);
        LamLitStream += 1;
        dst += 1;
        finalLen -= 1;
        core.Copy64Add(dst, LitStreams[0], dst + SizeOf(lastOffset), finalLen);
        dst += finalLen;
        break;
      }
      case 3:
      case 5:
        for (size_t i = 0; i < finalLen; i++)
          CopyLiteralSubAnd(core, dst, lastOffset);
        break;
      case 4:
        Context = core.GetByte(dst - 1);
        for (size_t i = 0; i < finalLen; i++)
          CopyLiteralO1(core, dst);
        break;
      default:
        Fail();
    }
  }
};

void CLeviathan::ProcessLzRuns(CCore &core, size_t mode, Ptr dstIn, size_t dstSize, size_t offset)
{
  Check(mode <= 5);
  Ptr dst = (offset == 0) ? dstIn + 8 : dstIn;
  const Ptr dstStart = dstIn;
  const Ptr dstEnd = dstIn + dstSize;
  const Ptr windowBase = dstIn - offset;

  size_t lenFront = 0, lenBack = LenStream.size();
  size_t offsIdx = 0;
  const Ptr matchZoneEnd = ((dstEnd - dstStart) >= 16) ? dstEnd - 16 : dstStart;

  int32_t recentOffs[16];
  for (unsigned i = 0; i < 16; i++)
    recentOffs[i] = (i >= 8 && i <= 14) ? -8 : 0;
  int32_t lastOffset = -8;

  CLevMode m;
  m.Init(core, *this, mode, dstStart);

  size_t cmdStreamLeft = 0;
  Ptr streams[8];
  Ptr cmdStream = CmdStream;
  if (!HaveCmdStream)
  {
    cmdStreamLeft = CmdStreamSize;
    for (size_t i = 0; i < 8; i++)
      streams[i] = MultiCmdPtr[(i - dstStart.I) & 7].P;
  }
  const Ptr cmdStreamEnd = cmdStream + CmdStreamSize;

  for (;;)
  {
    size_t cmd;
    if (!HaveCmdStream)
    {
      if (cmdStreamLeft == 0)
        break;
      cmdStreamLeft--;
      Ptr &s = streams[dst.I & 7];
      cmd = core.GetByte(s);
      s += 1;
    }
    else
    {
      if (cmdStream >= cmdStreamEnd)
        break;
      cmd = core.GetByte(cmdStream);
      cmdStream += 1;
    }

    const size_t offsIndex = cmd >> 5;
    Check(offsIndex <= 8);
    size_t matchlen = (cmd & 7) + 2;

    recentOffs[15] = (offsIdx < OffsStream.size()) ? OffsStream[offsIdx] : 0;

    m.CopyLiterals(core, cmd, dst, LenStream, lenFront, lenBack, matchZoneEnd, lastOffset);

    Check(offsIndex + 8 < 16);
    lastOffset = recentOffs[offsIndex + 8];

    // permute the recent offsets
    Check(offsIndex <= 7);
    memmove(recentOffs + offsIndex + 1, recentOffs + offsIndex, 8 * sizeof(int32_t));
    recentOffs[8] = lastOffset;
    if (offsIndex == 7)
      offsIdx++;

    const Ptr copyfrom = dst + SizeOf(lastOffset);
    Check(windowBase <= copyfrom);

    if (matchlen == 9)
    {
      Check(lenFront < lenBack);
      matchlen = SizeOf(LenStream[--lenBack]) + 6;
      Check(matchlen <= (dstEnd - dst) - 8);
    }
    core.RepeatCopy64(dst, copyfrom, matchlen);
    dst += matchlen;
  }

  // the input must be used up
  Check(offsIdx >= OffsStream.size());
  Check(lenFront == lenBack);

  // the last literals
  if (dst < dstEnd)
    m.CopyFinalLiterals(core, dstEnd - dst, dst, lastOffset);
  else
    Check(dst == dstEnd);
}

// ---------- LZB16 ----------

// byte-aligned LZ77 with 16-bit distances, no entropy coding. A command is
// a control byte (literal count in the low nibble, match code in the high
// one), the literals, then the match:
//   literal count 15 adds an extension;
//   match code 0-14: length code + 4, a 2-byte little-endian distance;
//   match code 15: a byte e. 1LLLLDDD: length LLLL + 4 (19 adds an
//   extension), distance DDD. Otherwise length 19 + e and a 2-byte
//   distance; e == 127 adds an extension after the distance.
// An extension is a byte, or ((first & 0x3F) << 8) | second when the first
// is 0xC0 or more. A last literal run is cut to the output.
size_t DecodeLzb16(const Byte *in, size_t inLen, Byte *out, size_t end, size_t at, size_t history)
{
  size_t pos = 0;
  auto byte = [&]() -> size_t { Check(pos < inLen); return in[pos++]; };
  auto extension = [&]() -> size_t
  {
    const size_t first = byte();
    if (first < 0xC0)
      return first;
    const size_t second = byte();
    return ((first & 0x3F) << 8) | second;
  };
  while (at < end)
  {
    const size_t control = byte();
    size_t literals = control & 0xF;
    if (literals == 15)
      literals += extension();
    literals = std::min(literals, end - at);
    Check(literals <= inLen - pos);
    memcpy(out + at, in + pos, literals);
    pos += literals;
    at += literals;
    if (at == end)
      break;
    const size_t code = control >> 4;
    size_t length, distance;
    if (code < 15)
    {
      const size_t low = byte();
      const size_t high = byte();
      length = code + 4;
      distance = low | (high << 8);
    }
    else
    {
      const size_t e = byte();
      if (e & 0x80)
      {
        length = ((e >> 3) & 0xF) + 4;
        if (length == 19)
          length += extension();
        distance = e & 7;
      }
      else
      {
        const size_t low = byte();
        const size_t high = byte();
        length = 19 + e;
        if (e == 127)
          length += extension();
        distance = low | (high << 8);
      }
    }
    Check(1 <= distance);
    Check(distance <= at - history);
    Check(length <= end - at);
    for (size_t i = at; i < at + length; i++)
      out[i] = out[i - distance];
    at += length;
  }
  return pos;
}

// ---------- Bitknit ----------

template <unsigned F, unsigned A, unsigned L>
struct CBkModel
{
  uint16_t a[A];
  uint16_t freq[F];
  uint16_t adaptInterval;
  uint16_t lookup[L];

  static const unsigned kShift = (A == 301) ? 6 : 9;
  static const unsigned kFInc = 1026 - A;

  CBkModel()
  {
    for (unsigned i = 0; i < A; i++)
    {
      if (kShift == 6)
        a[i] = (uint16_t)((i < 264) ? ((0x8000 - 300 + 264) * i / 264) : ((0x8000 - 300) + i));
      else
        a[i] = (uint16_t)(0x8000 * i / F);
    }
    for (unsigned i = 0; i < F; i++)
      freq[i] = 1;
    adaptInterval = 1024;
    memset(lookup, 0, sizeof(lookup));
    FillLut();
  }

  void FillLut()
  {
    size_t p = 0;
    for (unsigned i = 0; i + 1 < A; i++)
    {
      const size_t pEnd = (size_t)(((uint16_t)(a[i + 1] - 1) >> kShift) + 1);
      Check(p <= pEnd && pEnd <= L);
      for (size_t k = p; k < pEnd; k++)
        lookup[k] = (uint16_t)i;
      p = pEnd;
    }
  }

  void Adapt(size_t sym)
  {
    adaptInterval = 1024;
    Check(sym < F);
    freq[sym] = (uint16_t)(freq[sym] + kFInc);
    uint32_t sum = 0;
    for (unsigned i = 0; i < F; i++)
    {
      sum += freq[i];
      const uint32_t v = a[i + 1];
      a[i + 1] = (uint16_t)(v + ((sum - v) >> 1));
    }
    for (unsigned i = 0; i < F; i++)
      freq[i] = 1;
    FillLut();
  }

  size_t Lookup(uint32_t &bits)
  {
    const uint16_t masked = (uint16_t)(bits & 0x7FFF);
    const size_t i = masked >> kShift;
    Check(i < L);
    size_t sym = lookup[i];
    Check(sym + 1 < A);
    if (masked > a[sym + 1])
    {
      sym++;
      Check(sym + 1 < A);
    }
    size_t k = sym + 1;
    while (k < A && a[k] <= masked)
      k++;
    Check(k < A);
    sym = k - 1;
    const uint32_t s = a[sym], s1 = a[sym + 1];
    bits = masked + (bits >> 15) * (s1 - s) - s;
    Check(sym < F);
    freq[sym] = (uint16_t)(freq[sym] + 31);
    adaptInterval--;
    if (adaptInterval == 0)
      Adapt(sym);
    return sym;
  }
};

typedef CBkModel<300, 301, 512> CBkLiteral;
typedef CBkModel<40, 41, 64> CBkDistanceLsb;
typedef CBkModel<21, 22, 64> CBkDistanceBits;

struct CBitknitState
{
  uint32_t RecentDist[8];
  uint32_t LastMatchDist;
  uint32_t RecentDistMask;
  CBkLiteral Literals[4];
  CBkDistanceLsb DistanceLsb[4];
  CBkDistanceBits DistanceBits;

  CBitknitState(): LastMatchDist(1)
  {
    for (unsigned i = 0; i < 8; i++)
      RecentDist[i] = 1;
    RecentDistMask = (1 << 3) | (2 << (2 * 3)) | (3 << (3 * 3)) | (4 << (4 * 3)) | (5 << (5 * 3))
        | (6 << (6 * 3)) | (7 << (7 * 3));
  }
};

struct CBitknit
{
  CBitknitState &State;
  const Byte *In; size_t InLen;
  Byte *Out; size_t OutLen;
  size_t Src, Dst;
  uint32_t Bits, Bits2;

  CBitknit(CBitknitState &state, const Byte *in, size_t inLen, Byte *out, size_t outLen, size_t dst):
      State(state), In(in), InLen(inLen), Out(out), OutLen(outLen), Src(0), Dst(dst), Bits(0x10000), Bits2(0x10000) {}

  uint32_t Read2()
  {
    Check(Src <= InLen && InLen - Src >= 2);
    const uint32_t v = In[Src] | ((uint32_t)In[Src + 1] << 8);
    Src += 2;
    return v;
  }
  uint32_t Read4()
  {
    Check(Src <= InLen && InLen - Src >= 4);
    const uint32_t v = In[Src] | ((uint32_t)In[Src + 1] << 8) | ((uint32_t)In[Src + 2] << 16) | ((uint32_t)In[Src + 3] << 24);
    Src += 4;
    return v;
  }
  void Write1(Byte v) { Check(Dst < OutLen); Out[Dst++] = v; }
  void Write2(uint16_t v)
  {
    Check(Dst <= OutLen && OutLen - Dst >= 2);
    Out[Dst] = (Byte)v;
    Out[Dst + 1] = (Byte)(v >> 8);
    Dst += 2;
  }
  void WriteSym(Byte sym)
  {
    Check(Dst < OutLen);
    const size_t from = Dst - State.LastMatchDist;
    Check(from < OutLen);
    Out[Dst] = (Byte)(sym + Out[from]);
    Dst++;
  }
  void CopyChunks(size_t chunk, size_t copyLength, size_t matchDist)
  {
    Check(matchDist <= Dst);
    Check(Dst + copyLength <= OutLen);
    for (size_t i = 0; i < copyLength / chunk; i++)
    {
      const size_t d = Dst + i * chunk;
      memmove(Out + d, Out + d - matchDist, chunk);
    }
    const size_t rem = copyLength % chunk;
    const size_t d = Dst + copyLength - rem;
    memmove(Out + d, Out + d - matchDist, rem);
  }
  void Renormalize()
  {
    if (Bits < 0x10000)
      Bits = (Bits << 16) | Read2();
    std::swap(Bits, Bits2);
  }
  size_t Decode();
};

size_t CBitknit::Decode()
{
  size_t recentMask = State.RecentDistMask;
  size_t litmodel[4] = { 0, 1, 2, 3 };
  size_t distancelsb[4] = { 0, 1, 2, 3 };

  const uint32_t v = Read4();
  if (v < 0x10000)
    return 0;

  uint32_t a = v >> 4;
  const unsigned n = v & 0xF;
  if (a < 0x10000)
    a = (a << 16) | Read2();
  Bits = a >> n;
  if (Bits < 0x10000)
    Bits = (Bits << 16) | Read2();
  a = (a << 16) | Read2();

  Bits2 = Shl(1, (int)(n + 16)) | (a & (Shl(1, (int)(n + 16)) - 1));

  if (Dst == 0)
  {
    Write1((Byte)Bits);
    Bits >>= 8;
    Renormalize();
  }

  while (Dst + 4 < OutLen)
  {
    size_t sym = State.Literals[litmodel[Dst & 3]].Lookup(Bits);
    Renormalize();

    if (sym < 256)
    {
      WriteSym((Byte)sym);
      if (Dst + 4 >= OutLen)
        break;
      sym = State.Literals[litmodel[Dst & 3]].Lookup(Bits);
      Renormalize();
      if (sym < 256)
      {
        WriteSym((Byte)sym);
        continue;
      }
    }

    if (sym >= 288)
    {
      const unsigned nb = (unsigned)(sym - 287);
      sym = (Bits & ((1u << nb) - 1)) + ((size_t)1 << nb) + 286;
      Bits >>= nb;
      Renormalize();
    }

    const size_t copyLength = sym - 254;

    sym = State.DistanceLsb[distancelsb[Dst & 3]].Lookup(Bits);
    Renormalize();

    uint32_t matchDist;
    if (sym >= 8)
    {
      const size_t nb = State.DistanceBits.Lookup(Bits);
      Renormalize();

      matchDist = Bits & ((1u << (nb & 0xF)) - 1);
      Bits >>= (nb & 0xF);
      Renormalize();
      if (nb >= 0x10)
        matchDist = (matchDist << 16) | Read2();
      matchDist = Shl(32, (int)nb) + (matchDist << 5) + (uint32_t)sym - 39;

      const size_t i1 = (recentMask >> 21) & 7;
      const size_t i2 = (recentMask >> 18) & 7;
      State.RecentDist[i1] = State.RecentDist[i2];
      State.RecentDist[i2] = matchDist;
    }
    else
    {
      const size_t idx = (recentMask >> (3 * sym)) & 7;
      const size_t mask = ~(size_t)7 << (3 * sym);
      matchDist = State.RecentDist[idx];
      recentMask = (recentMask & mask) | ((idx + 8 * recentMask) & ~mask);
    }

    if (matchDist == 1)
    {
      Check(Dst >= 1 && Dst - 1 < OutLen);
      const Byte b = Out[Dst - 1];
      Check(Dst <= OutLen && copyLength <= OutLen - Dst);
      memset(Out + Dst, b, copyLength);
    }
    else if (matchDist > copyLength)
    {
      Check(matchDist <= Dst);
      Check(Dst <= OutLen && copyLength <= OutLen - Dst);
      memmove(Out + Dst, Out + Dst - matchDist, copyLength);
    }
    else if (matchDist >= 8)
      CopyChunks(8, copyLength, matchDist);
    else if (matchDist >= 4)
      CopyChunks(4, copyLength, matchDist);
    else
    {
      for (size_t i = 0; i < copyLength; i++)
      {
        Check(Dst + i < OutLen);
        Check(matchDist <= Dst + i);
        Out[Dst + i] = Out[Dst + i - matchDist];
      }
    }

    Dst += copyLength;
    State.LastMatchDist = matchDist;
  }
  Write2((uint16_t)Bits);
  Write2((uint16_t)Bits2);

  State.RecentDistMask = (uint32_t)recentMask;
  return Src;
}

// ---------- LZNA ----------

// adaptive rANS models for 4-, 3- and 1-bit values
struct CLznaNibbleModel
{
  int16_t Prob[16];
  CLznaNibbleModel() { for (int i = 0; i < 16; i++) Prob[i] = (int16_t)(i * 0x800); }
};

struct CLzna3bitModel
{
  int16_t Prob[8];
  CLzna3bitModel() { for (int i = 0; i < 8; i++) Prob[i] = (int16_t)(i * 0x1000); }
};

typedef uint16_t CLznaBitModel;

struct CLznaLiteralModel
{
  CLznaNibbleModel Upper[16], Lower[16], Nomatch[16];
};

struct CLznaFarDistModel
{
  CLznaNibbleModel FirstLo, FirstHi;
  CLznaBitModel Second[31];
  CLznaBitModel Third[2][31];
  CLznaFarDistModel()
  {
    for (int i = 0; i < 31; i++) { Second[i] = 0x2000; Third[0][i] = Third[1][i] = 0x2000; }
  }
};

struct CLznaNearDistModel
{
  CLznaNibbleModel First;
  CLznaBitModel Second[16];
  CLznaBitModel Third[2][16];
  CLznaNearDistModel()
  {
    for (int i = 0; i < 16; i++) { Second[i] = 0x2000; Third[0][i] = Third[1][i] = 0x2000; }
  }
};

struct CLznaLowBitsDistanceModel
{
  CLznaNibbleModel D[2];
  CLznaBitModel V;
  CLznaLowBitsDistanceModel(): V(0x2000) {}
};

struct CLznaShortLengthRecentModel
{
  CLzna3bitModel A[4];
};

struct CLznaLongLengthModel
{
  CLznaNibbleModel First[4];
  CLznaNibbleModel Second, Third;
};

struct CLznaState
{
  uint32_t MatchHistory[8];
  CLznaLiteralModel Literal[4];
  CLznaBitModel IsLiteral[12 * 8];
  CLznaNibbleModel Type[12 * 8];
  CLznaShortLengthRecentModel ShortLengthRecent[4];
  CLznaLongLengthModel LongLengthRecent;
  CLznaLowBitsDistanceModel LowBitsOfDistance[2];
  CLznaBitModel ShortLength[12][4];
  CLznaNearDistModel NearDist[2];
  CLzna3bitModel MediumLength;
  CLznaLongLengthModel LongLength;
  CLznaFarDistModel FarDistance;

  CLznaState()
  {
    for (int i = 0; i < 8; i++) MatchHistory[i] = 1;
    for (int i = 0; i < 96; i++) IsLiteral[i] = 0x1000;
    for (int i = 0; i < 12; i++) for (int k = 0; k < 4; k++) ShortLength[i][k] = 0x2000;
  }

  void PreprocessMatchHistory()
  {
    if (MatchHistory[4] >= 0xc000)
    {
      size_t i = 0;
      while (MatchHistory[4 + i] >= 0xC000)
      {
        i++;
        if (i >= 4)
        {
          MatchHistory[7] = MatchHistory[6];
          MatchHistory[6] = MatchHistory[5];
          MatchHistory[5] = MatchHistory[4];
          MatchHistory[4] = 4;
          return;
        }
      }
      const uint32_t t = MatchHistory[i + 4];
      MatchHistory[i + 4] = MatchHistory[i + 3];
      MatchHistory[i + 3] = MatchHistory[i + 2];
      MatchHistory[i + 2] = MatchHistory[i + 1];
      MatchHistory[4] = t;
    }
  }
};

struct CLzna
{
  uint64_t BitsA, BitsB;
  const Byte *In; size_t InLen, InPos;
  Byte *Out; size_t OutLen;
  size_t Dst;

  CLzna(const Byte *in, size_t inLen, Byte *out, size_t outLen, size_t dst):
      BitsA(0), BitsB(0), In(in), InLen(inLen), InPos(0), Out(out), OutLen(outLen), Dst(dst) {}

  Byte ReadByte() { Check(InPos < InLen); return In[InPos++]; }
  uint32_t Read()
  {
    Check(InLen - InPos >= 4);
    const Byte *p = In + InPos;
    InPos += 4;
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
  }
  void Write(Byte v) { Check(Dst < OutLen); Out[Dst++] = v; }

  uint64_t InitBits()
  {
    const int32_t d = ReadByte();
    const int32_t n = d >> 4;
    Check(n <= 8);
    uint64_t v = 0;
    for (int32_t i = 0; i < n; i++)
      v = (v << 8) | ReadByte();
    return (v << 4) | (uint64_t)(d & 0xF);
  }
  // two streams, swapped after each value
  void Init() { BitsA = InitBits(); BitsB = InitBits(); }

  void CopyOffset(size_t dist, size_t length)
  {
    Check(dist != 0 && dist <= Dst);
    const size_t src = Dst - dist;
    Check(Dst <= OutLen && length <= OutLen - Dst);
    if (dist == 1)
      memset(Out + Dst, Out[src], length);
    else if (dist > length)
      memmove(Out + Dst, Out + src, length);
    else
      for (size_t i = 0; i < length; i += dist)
        memmove(Out + Dst + i, Out + src + i, std::min(length, dist + i) - i);
    Dst += length;
  }

  void Renormalize()
  {
    uint64_t x = BitsA;
    if (x < 0x80000000)
      x = (x << 32) | Read();
    BitsA = BitsB;
    BitsB = x;
  }

  bool ReadBool()
  {
    const uint64_t r = BitsA & 1;
    BitsA >>= 1;
    Renormalize();
    return r == 1;
  }

  size_t ReadNBits(size_t bits)
  {
    const uint64_t rv = BitsA & (Shl64(1, (int)bits) - 1);
    BitsA = Shr64(BitsA, (int)bits);
    Renormalize();
    return (size_t)rv;
  }

  template <unsigned N>
  size_t ReadModel(int16_t *prob, int16_t inc)
  {
    const uint64_t x = BitsA;
    const int16_t xv = (int16_t)(x & 0x7FFF);
    unsigned bitindex = N;
    for (unsigned i = 0; i < N; i++)
      if (prob[i] > xv) { bitindex = i; break; }
    Check(bitindex >= 1);
    const uint64_t start = (uint64_t)(int64_t)prob[bitindex - 1];
    const uint64_t end = (bitindex < N) ? (uint64_t)(int64_t)prob[bitindex] : 0x8000;
    for (unsigned i = 0; i < N; i++)
    {
      int16_t u = (prob[i] > xv) ? inc : 0;
      u = (int16_t)(u + (int16_t)(i * 8));
      u = (int16_t)(u - prob[i]);
      u = (int16_t)(u >> 7);
      prob[i] = (int16_t)(prob[i] + u);
    }
    BitsA = (end - start) * (x >> 15) + (x & 0x7FFF) - start;
    Renormalize();
    return bitindex - 1;
  }
  size_t ReadNibble(CLznaNibbleModel &m) { return ReadModel<16>(m.Prob, 0x7FD9); }
  size_t Read3Bits(CLzna3bitModel &m) { return ReadModel<8>(m.Prob, 0x7FE5); }

  size_t Read1Bit(CLznaBitModel &model, int nbits, int shift)
  {
    Check(nbits < 32);
    const uint64_t magn = (uint64_t)1 << nbits;
    const uint64_t q = (uint64_t)model * (BitsA >> nbits);
    if ((BitsA & (magn - 1)) >= model)
    {
      BitsA -= q + model;
      model = (uint16_t)(model - (model >> shift));
      Renormalize();
      return 1;
    }
    BitsA = (BitsA & (magn - 1)) + q;
    model = (uint16_t)(model + (uint16_t)((magn - model) >> shift));
    Renormalize();
    return 0;
  }

  size_t LowBits(CLznaState &lut, size_t hi)
  {
    CLznaLowBitsDistanceModel &lutd = lut.LowBitsOfDistance[hi == 0 ? 1 : 0];
    const size_t lowBit = Read1Bit(lutd.V, 14, 6);
    const size_t lowNibble = ReadNibble(lutd.D[lowBit]);
    return lowBit + (2 * lowNibble) + (32 * hi) + 1;
  }

  size_t ReadFarDistance(CLznaState &lut)
  {
    size_t n = ReadNibble(lut.FarDistance.FirstLo);
    if (n >= 15)
      n = 15 + ReadNibble(lut.FarDistance.FirstHi);
    size_t hi = 0;
    if (n != 0)
    {
      Check(n - 1 < 31);
      hi = Read1Bit(lut.FarDistance.Second[n - 1], 14, 6) + 2;
      if (n != 1)
      {
        hi = (hi << 1) + Read1Bit(lut.FarDistance.Third[hi - 2][n - 1], 14, 6);
        if (n != 2)
          hi = (hi << (n - 2)) + ReadNBits(n - 2);
      }
      hi -= 1;
    }
    return LowBits(lut, hi);
  }

  size_t ReadNearDistance(CLznaState &lut, size_t idx)
  {
    Check(idx < 2);
    CLznaNearDistModel &model = lut.NearDist[idx];
    const size_t nb = ReadNibble(model.First);
    size_t hi = 0;
    if (nb != 0)
    {
      hi = Read1Bit(model.Second[nb - 1], 14, 6) + 2;
      if (nb != 1)
      {
        hi = (hi << 1) + Read1Bit(model.Third[hi - 2][nb - 1], 14, 6);
        if (nb != 2)
          hi = (hi << (nb - 2)) + ReadNBits(nb - 2);
      }
      hi -= 1;
    }
    return LowBits(lut, hi);
  }

  size_t ReadLength(CLznaLongLengthModel &model)
  {
    size_t length = ReadNibble(model.First[Dst & 3]);
    if (length >= 12)
    {
      size_t b = ReadNibble(model.Second);
      if (b >= 15)
        b = 15 + ReadNibble(model.Third);
      size_t n = 0, base = 0;
      if (b != 0)
      {
        n = (b - 1) >> 1;
        base = ((((b - 1) & 1) + 2) << n) - 1;
      }
      length += (ReadNBits(n) + base) * 4;
    }
    return length;
  }

  size_t DecodeQuantum(CLznaState &lut);
};

size_t CLzna::DecodeQuantum(CLznaState &lut)
{
  lut.PreprocessMatchHistory();
  Init();
  size_t dist = lut.MatchHistory[4];

  size_t state = 5;
  Check(OutLen >= 8);
  const size_t dstEnd = OutLen - 8;

  if (Dst == 0)
  {
    size_t x;
    if (ReadBool())
      x = 0;
    else
    {
      CLznaLiteralModel &model = lut.Literal[0];
      x = ReadNibble(model.Upper[0]);
      x = (x << 4) + ReadNibble(x != 0 ? model.Nomatch[x] : model.Lower[0]);
    }
    Write((Byte)x);
  }
  while (Dst < dstEnd)
  {
    Check(dist <= Dst && Dst - dist < OutLen);
    const Byte matchVal = Out[Dst - dist];

    if (Read1Bit(lut.IsLiteral[(Dst & 7) + 8 * state], 13, 5) != 0)
    {
      const size_t x = ReadNibble(lut.Type[(Dst & 7) + 8 * state]);
      if (x == 0)
      {
        // 1 byte from the most recent distance
        Write(matchVal);
        state = (state >= 7) ? 11 : 9;
      }
      else if (x < 4)
      {
        if (x == 1)
        {
          // 3-4 bytes
          const size_t length = 3 + Read1Bit(lut.ShortLength[state][Dst & 3], 14, 4);
          dist = ReadNearDistance(lut, length - 3);
          CopyOffset(dist, length);
        }
        else if (x == 2)
        {
          // 5-12 bytes
          const size_t length = 5 + Read3Bits(lut.MediumLength);
          dist = ReadFarDistance(lut);
          CopyOffset(dist, length);
        }
        else
        {
          // 13 bytes or more
          const size_t length = ReadLength(lut.LongLength) + 13;
          dist = ReadFarDistance(lut);
          CopyOffset(dist, length);
        }
        state = (state >= 7) ? 10 : 7;
        lut.MatchHistory[7] = lut.MatchHistory[6];
        lut.MatchHistory[6] = lut.MatchHistory[5];
        lut.MatchHistory[5] = lut.MatchHistory[4];
        lut.MatchHistory[4] = (uint32_t)dist;
      }
      else if (x >= 12)
      {
        // 2 bytes from a recent distance
        const size_t idx = x - 12;
        dist = lut.MatchHistory[4 + idx];
        lut.MatchHistory[4 + idx] = lut.MatchHistory[3 + idx];
        lut.MatchHistory[3 + idx] = lut.MatchHistory[2 + idx];
        lut.MatchHistory[2 + idx] = lut.MatchHistory[1 + idx];
        lut.MatchHistory[4] = (uint32_t)dist;
        CopyOffset(dist, 2);
        state = (state >= 7) ? 11 : 8;
      }
      else
      {
        const size_t idx = (x - 4) >> 1;
        dist = lut.MatchHistory[4 + idx];
        lut.MatchHistory[4 + idx] = lut.MatchHistory[3 + idx];
        lut.MatchHistory[3 + idx] = lut.MatchHistory[2 + idx];
        lut.MatchHistory[2 + idx] = lut.MatchHistory[1 + idx];
        lut.MatchHistory[4] = (uint32_t)dist;
        if (x & 1)
        {
          // 11 bytes or more from a recent distance
          const size_t length = 11 + ReadLength(lut.LongLengthRecent);
          CopyOffset(dist, length);
        }
        else
        {
          // 3-10 bytes from a recent distance
          const size_t length = 3 + Read3Bits(lut.ShortLengthRecent[idx].A[Dst & 3]);
          CopyOffset(dist, length);
        }
        state = (state >= 7) ? 11 : 8;
      }
    }
    else
    {
      // a literal
      CLznaLiteralModel &model = lut.Literal[Dst & 3];
      size_t x = ReadNibble(model.Upper[matchVal >> 4]);
      x = (x << 4) + ReadNibble(((size_t)(matchVal >> 4) != x) ? model.Nomatch[x] : model.Lower[matchVal & 0xF]);
      Write((Byte)x);
      static const Byte kNext[12] = { 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 4, 5 };
      state = (state < 12) ? kNext[state] : 0;
    }
  }

  Check(Dst == dstEnd);
  for (unsigned i = 0; i < 4; i++)
  {
    Out[Dst + i] = (Byte)(BitsA >> (8 * i));
    Out[Dst + 4 + i] = (Byte)(BitsB >> (8 * i));
  }
  return InPos;
}

// ---------- the stream ----------

enum EDecoder { kLzb16 = 2, kLzna = 5, kKraken = 6, kMermaid = 10, kBitknit = 11, kLeviathan = 12 };

class CExtractor
{
  const Byte *_in;
  size_t _inLen, _inPos;
  size_t _outputBase;
  unsigned _decoder;
  bool _restart, _uncompressed;
  std::unique_ptr<CBitknitState> _bitknit;
  std::unique_ptr<CLznaState> _lzna;
  std::vector<Byte> _scratch, _tmp;
  CKraken _kraken;
  CMermaid _mermaid;
  CLeviathan _leviathan;

  size_t BlockSize() const
  {
    return (_decoder == kLzna || _decoder == kLzb16 || _decoder == kBitknit) ? kSmallBlock : kLargeBlock;
  }
  const Byte *ReadSlice(size_t n)
  {
    Check(n <= _inLen - _inPos);
    const Byte *p = _in + _inPos;
    _inPos += n;
    return p;
  }
  size_t ReadBe(unsigned n)
  {
    const Byte *p = ReadSlice(n);
    size_t v = 0;
    for (unsigned i = 0; i < n; i++)
      v = (v << 8) | p[i];
    return v;
  }
  void ParseHeader()
  {
    const Byte *p = ReadSlice(2);
    const Byte b1 = p[0], b2 = p[1];
    Check((b1 & 0xF) == 0xC && ((b1 >> 4) & 3) == 0);
    _restart = ((b1 >> 7) & 1) != 0;
    _uncompressed = ((b1 >> 6) & 1) != 0;
    _decoder = b2 & 0x7F;
    Check(_decoder == kLzb16 || _decoder == kLzna || _decoder == kKraken || _decoder == kMermaid
        || _decoder == kBitknit || _decoder == kLeviathan);
    // bit 7 of b2: checksums; never seen in a file, so not read
  }
  size_t ParseWholeMatch()
  {
    const size_t v = ReadBe(2);
    if (v >= 0x8000)
      return v - 0x8000 + 1;
    size_t x = 0;
    unsigned pos = 0;
    for (;;)
    {
      const size_t b = *ReadSlice(1);
      if ((b & 0x80) == 0)
      {
        x += (b + 0x80) << pos;
        pos += 7;
        Check(pos < 64);
      }
      else
      {
        x += (b - 0x80) << pos;
        return v + 0x8000 + (x << 15) + 1;
      }
    }
  }
  template <class TAlgorithm>
  size_t DecodeQuantum(const Byte *in, size_t inLen, Byte *out, size_t outLen, size_t offset, size_t dstBytesLeft, TAlgorithm &a)
  {
    // offsets and the first literals are from the last restart
    Check(_outputBase <= outLen);
    CCore core;
    core.In = in; core.InLen = inLen;
    core.Out = out + _outputBase; core.OutLen = outLen - _outputBase;
    core.Scratch = _scratch.data(); core.ScratchLen = _scratch.size();
    core.Tmp = _tmp.data(); core.TmpLen = _tmp.size();
    core.Src = InP(0);
    core.Dst = OutP(offset - _outputBase);
    core.DstEnd = OutP(offset - _outputBase + dstBytesLeft);
    return core.DecodeQuantum(a);
  }
  size_t ExtractBlock(Byte *out, size_t outLen, size_t offset);

public:
  CExtractor(const Byte *in, size_t inLen): _in(in), _inLen(inLen), _inPos(0), _outputBase(0),
      _decoder(kLzna), _restart(false), _uncompressed(false), _scratch(kLargeBlock), _tmp(kLargeBlock) {}
  size_t Read(Byte *out, size_t outLen);
};

size_t CExtractor::ExtractBlock(Byte *out, size_t outLen, size_t offset)
{
  if (offset == 0 || _restart)
    _outputBase = offset;
  const size_t dstBytesLeft = std::min(outLen - offset, BlockSize());

  if (_uncompressed)
  {
    memcpy(out + offset, ReadSlice(dstBytesLeft), dstBytesLeft);
    return dstBytesLeft;
  }

  if (BlockSize() == kLargeBlock)
  {
    const size_t v = ReadBe(3);
    const size_t size = v & 0x3FFFF;
    if (size == 0x3FFFF)
    {
      Check((v >> 18) == 1);
      memset(out + offset, *ReadSlice(1), dstBytesLeft);
      return dstBytesLeft;
    }
    const size_t compressedSize = size + 1;
    const Byte *in = ReadSlice(compressedSize);
    size_t bytesRead;
    switch (_decoder)
    {
      case kKraken: bytesRead = DecodeQuantum(in, compressedSize, out, outLen, offset, dstBytesLeft, _kraken); break;
      case kMermaid: bytesRead = DecodeQuantum(in, compressedSize, out, outLen, offset, dstBytesLeft, _mermaid); break;
      case kLeviathan: bytesRead = DecodeQuantum(in, compressedSize, out, outLen, offset, dstBytesLeft, _leviathan); break;
      default: Fail();
    }
    Check(bytesRead == compressedSize);
    return dstBytesLeft;
  }

  const size_t v = ReadBe(2);
  const size_t size = v & 0x3FFF;
  if (size == 0x3FFF)
  {
    switch (v >> 14)
    {
      case 0:
      {
        // a copy of earlier output
        const size_t dist = ParseWholeMatch();
        Check(dist <= offset);
        memmove(out + offset, out + offset - dist, dstBytesLeft);
        return dstBytesLeft;
      }
      case 1:
        memset(out + offset, *ReadSlice(1), dstBytesLeft);
        return dstBytesLeft;
      case 2:
        memcpy(out + offset, ReadSlice(dstBytesLeft), dstBytesLeft);
        return dstBytesLeft;
    }
    Fail();
  }
  const size_t compressedSize = size + 1;
  const Byte *in = ReadSlice(compressedSize);
  const size_t end = offset + dstBytesLeft;
  size_t bytesRead;
  switch (_decoder)
  {
    case kBitknit:
    {
      if (_restart)
      {
        _bitknit.reset(new CBitknitState);
        _restart = false;
      }
      Check(_bitknit.get() != NULL);
      CBitknit bk(*_bitknit, in, compressedSize, out, end, offset);
      bytesRead = bk.Decode();
      break;
    }
    case kLzb16:
      // no state: a restart only sets where the history starts
      _restart = false;
      bytesRead = DecodeLzb16(in, compressedSize, out, end, offset, _outputBase);
      break;
    case kLzna:
    {
      if (_restart)
      {
        _lzna.reset(new CLznaState);
        _restart = false;
      }
      Check(_lzna.get() != NULL);
      CLzna lz(in, compressedSize, out, end, offset);
      bytesRead = lz.DecodeQuantum(*_lzna);
      break;
    }
    default:
      Fail();
  }
  Check(bytesRead == compressedSize);
  return dstBytesLeft;
}

size_t CExtractor::Read(Byte *out, size_t outLen)
{
  size_t bytesWritten = 0;
  while (bytesWritten < outLen)
  {
    if ((bytesWritten & 0x3FFFF) == 0)
      ParseHeader();
    const size_t count = ExtractBlock(out, outLen, bytesWritten);
    if (count == 0)
      break;
    bytesWritten += count;
  }
  return bytesWritten;
}

}

bool Decode(const unsigned char *in, size_t inSize, unsigned char *out, size_t outSize)
{
  try
  {
    CExtractor e(in, inSize);
    return e.Read(out, outSize) == outSize;
  }
  catch (const CError &) { return false; }
  catch (const std::bad_alloc &) { return false; }
  catch (const std::length_error &) { return false; }
}

}

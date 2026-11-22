// AceHandler.cpp -- 7zfm-qt: ACE archives (.ace, .c00 ... volumes, and
// self-extracting ones), versions 1.0 and 2.0. A port of acefile's
// decompressor (Daniel Roethlisberger and contributors, BSD 2-clause; see
// LICENSE.acefile next to this file), changed to C++ and 7-Zip's
// interfaces:
//
//   Headers: CRC-16 (the low half of a CRC-32), size, type, flags. The main
//   header has "**ACE**"; each file header its packed and original sizes,
//   DOS time, attributes, CRC-32 of the data (without the final XOR),
//   compression type (stored, LZ77, blocked), dictionary bits and name.
//   The data: a bit stream of little-endian 32-bit words read from the
//   top bit down. LZ77 with Huffman tables that are read again every
//   so many symbols, four recent distances, a dictionary of up to 4 MB
//   that a solid archive keeps from file to file. ACE 2.0's blocked mode
//   switches between plain LZ77, LZ77 with a delta filter, LZ77 with an
//   x86 call filter, a sound coder (1 to 3 channels) and a picture coder.
//
// Encrypted files: Blowfish in CBC mode, its key from one round of a SHA-1
// over the password as ACE lays it out (little-endian words).

#include "Common/ComTry.h"
#include "Common/MyBuffer.h"
#include "Common/StringConvert.h"
#include "Common/UTFConvert.h"

#include "Windows/PropVariant.h"
#include "Windows/TimeUtils.h"

#include "7zip/Common/ProgressUtils.h"
#include "7zip/Common/RegisterArc.h"
#include "7zip/Common/StreamObjects.h"
#include "7zip/Common/StreamUtils.h"
#include "7zip/IPassword.h"

#include "common/ItemArchive.h"
#include "AceBlowfishTables.h"

#include <zlib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <memory>
#include <vector>

using namespace NWindows;

namespace NArchive {
namespace NAce {

using NItemArc::GetUi16;
using NItemArc::GetUi32;
using NItemArc::GetUi64;
using NItemArc::ReadAt;

static int SChar(int x) { return (Byte)x >= 128 ? (int)(Byte)x - 256 : (int)(Byte)x; }
static int UChar(int x) { return (Byte)x; }
static unsigned BitLength(unsigned x) { unsigned n = 0; while (x) { n++; x >>= 1; } return n; }

struct CDataError {};

// ---------- the bit stream over a file's packed data ----------

class CBitStream
{
  ISequentialInStream *_in;
  UInt64 _left;
  std::vector<UInt32> _words;
  size_t _wordPos;     // the word holding _bit
  unsigned _bit;       // in that word, from the top
  bool _eof;

  bool Refill()
  {
    // keep the current word, append more
    if (_wordPos > 0)
    {
      _words.erase(_words.begin(), _words.begin() + (ptrdiff_t)_wordPos);
      _wordPos = 0;
    }
    if (_left == 0)
    {
      if (_eof)
        return false;
      _eof = true;
      _words.push_back(0);   // as acefile: one zero word past the end
      return true;
    }
    const size_t want = (size_t)MyMin(_left, (UInt64)1 << 16);
    std::vector<Byte> buf((want + 3) & ~(size_t)3, 0);
    size_t got = want;
    if (ReadStream(_in, buf.data(), &got) != S_OK || got == 0)
    {
      _left = 0;
      return Refill();
    }
    _left -= got;
    for (size_t i = 0; i < buf.size(); i += 4)
      _words.push_back(GetUi32(buf.data() + i));
    return true;
  }
public:
  CBitStream(ISequentialInStream *in, UInt64 size): _in(in), _left(size), _wordPos(0), _bit(0), _eof(false)
  {
    Refill();
  }
  UInt32 Peek(unsigned bits)
  {
    while ((_words.size() - _wordPos) * 32 < _bit + bits)
      if (!Refill())
        throw CDataError();
    const UInt64 w = ((UInt64)_words[_wordPos] << 32) | (_wordPos + 1 < _words.size() ? _words[_wordPos + 1] : 0);
    if (bits == 0)
      return 0;
    return (UInt32)((w << _bit) >> (64 - bits));
  }
  void Skip(unsigned bits)
  {
    _bit += bits;
    while (_bit >= 32)
    {
      _bit -= 32;
      _wordPos++;
    }
  }
  UInt32 Read(unsigned bits)
  {
    const UInt32 v = Peek(bits);
    Skip(bits);
    return v;
  }
  int GolombRice(unsigned rBits, bool isSigned)
  {
    UInt32 v = rBits ? Read(rBits) : 0;
    while (Read(1) == 1)
      v += (UInt32)1 << rBits;
    if (!isSigned)
      return (int)v;
    return (v & 1) ? -(int)(v >> 1) - 1 : (int)(v >> 1);
  }
  UInt32 KnownWidthUInt(unsigned bits)
  {
    if (bits < 2)
      return bits;
    bits--;
    return Read(bits) + ((UInt32)1 << bits);
  }
};

// ---------- Huffman tables, as acefile builds them ----------

class CTree
{
public:
  std::vector<UInt16> Codes;
  std::vector<Byte> Widths;
  unsigned MaxWidth;
  unsigned ReadSymbol(CBitStream &bs) const
  {
    const UInt32 code = bs.Peek(MaxWidth);
    if (code >= Codes.size())
      throw CDataError();
    const unsigned sym = Codes[code];
    bs.Skip(Widths[sym]);
    return sym;
  }
};

// acefile's quicksort, descending; its order of equal keys decides codes
static void QuickSort(std::vector<int> &keys, std::vector<int> &values, int left, int right)
{
  int newLeft = left, newRight = right;
  const int m = keys[(size_t)right];
  for (;;)
  {
    while (keys[(size_t)newLeft] > m)
      newLeft++;
    while (keys[(size_t)newRight] < m)
      newRight--;
    if (newLeft <= newRight)
    {
      std::swap(keys[(size_t)newLeft], keys[(size_t)newRight]);
      std::swap(values[(size_t)newLeft], values[(size_t)newRight]);
      newLeft++;
      newRight--;
    }
    if (newLeft >= newRight)
      break;
  }
  if (left < newRight)
  {
    if (left < newRight - 1)
      QuickSort(keys, values, left, newRight);
    else if (keys[(size_t)left] < keys[(size_t)newRight])
    {
      std::swap(keys[(size_t)left], keys[(size_t)newRight]);
      std::swap(values[(size_t)left], values[(size_t)newRight]);
    }
  }
  if (right > newLeft)
  {
    if (newLeft < right - 1)
      QuickSort(keys, values, newLeft, right);
    else if (keys[(size_t)newLeft] < keys[(size_t)right])
    {
      std::swap(keys[(size_t)newLeft], keys[(size_t)right]);
      std::swap(values[(size_t)newLeft], values[(size_t)right]);
    }
  }
}

static void MakeTree(std::vector<Byte> &widths, unsigned maxWidth, CTree &tree)
{
  std::vector<int> symbols(widths.size()), sorted(widths.size());
  for (size_t i = 0; i < widths.size(); i++)
  {
    symbols[i] = (int)i;
    sorted[i] = widths[i];
  }
  if (!widths.empty())
    QuickSort(sorted, symbols, 0, (int)widths.size() - 1);
  size_t used = 0;
  while (used < sorted.size() && sorted[used] != 0)
    used++;
  if (used < 2)
  {
    widths[(size_t)symbols[0]] = 1;
    if (used == 0)
      used++;
  }
  const size_t maxCodes = (size_t)1 << maxWidth;
  tree.Codes.clear();
  for (size_t k = used; k-- > 0;)
  {
    const int wdt = sorted[k];
    if (wdt > (int)maxWidth)
      throw CDataError();
    tree.Codes.insert(tree.Codes.end(), (size_t)1 << (maxWidth - (unsigned)wdt), (UInt16)symbols[k]);
    if (tree.Codes.size() > maxCodes)
      throw CDataError();
  }
  tree.Widths = widths;
  tree.MaxWidth = maxWidth;
}

static void ReadTree(CBitStream &bs, unsigned maxWidth, unsigned numCodes, CTree &tree)
{
  unsigned numWidths = bs.Read(9) + 1;
  if (numWidths > numCodes + 1)
    numWidths = numCodes + 1;
  const unsigned lowerWidth = bs.Read(4);
  const unsigned upperWidth = bs.Read(4);
  std::vector<Byte> ww(upperWidth + 1);
  for (unsigned i = 0; i <= upperWidth; i++)
    ww[i] = (Byte)bs.Read(3);
  CTree widthTree;
  MakeTree(ww, 7, widthTree);
  std::vector<Byte> widths;
  while (widths.size() < numWidths)
  {
    const unsigned sym = widthTree.ReadSymbol(bs);
    if (sym < upperWidth)
      widths.push_back((Byte)sym);
    else
    {
      unsigned len = bs.Read(4) + 4;
      len = MyMin(len, numWidths - (unsigned)widths.size());
      widths.insert(widths.end(), len, 0);
    }
  }
  if (upperWidth > 0)
    for (size_t i = 1; i < widths.size(); i++)
      widths[i] = (Byte)((widths[i] + widths[i - 1]) % upperWidth);
  for (size_t i = 0; i < widths.size(); i++)
    if (widths[i] > 0)
      widths[i] = (Byte)(widths[i] + lowerWidth);
  MakeTree(widths, maxWidth, tree);
}

// ---------- the modes ----------

enum { kMode_Lz77, kMode_Delta, kMode_Exe, kMode_Sound8, kMode_Sound16, kMode_Sound32A, kMode_Sound32B, kMode_Pic };

struct CMode
{
  int Mode;
  unsigned DeltaDist, DeltaLen, ExeMode;
  CMode(): Mode(-1), DeltaDist(0), DeltaLen(0), ExeMode(0) {}
  static CMode ReadFrom(CBitStream &bs)
  {
    CMode m;
    m.Mode = (int)bs.Read(8);
    if (m.Mode == kMode_Delta)
    {
      m.DeltaDist = bs.Read(8);
      m.DeltaLen = bs.Read(17);
    }
    else if (m.Mode == kMode_Exe)
      m.ExeMode = bs.Read(8);
    return m;
  }
};

static const unsigned kMaxCodeWidth = 11;
static const unsigned kMaxDistAtLen2 = 255;
static const unsigned kMaxDistAtLen3 = 8191;
static const unsigned kMaxDicBits = 22;
static const unsigned kTypeCode = 260 + kMaxDicBits + 1;
static const unsigned kNumMainCodes = 260 + kMaxDicBits + 2;
static const unsigned kNumLenCodes = 256 - 1;
static const size_t kWindow = (size_t)1 << 23;

class CLz77
{
  CTree _main, _len;
  UInt32 _symsToRead;
  UInt32 _hist[4];
  std::vector<Byte> _window;
  UInt64 _total;       // bytes ever put in the dictionary
public:
  CLz77(): _symsToRead(0), _total(0) { _window.resize(kWindow); memset(_hist, 0, sizeof(_hist)); }
  void Reinit() { _symsToRead = 0; memset(_hist, 0, sizeof(_hist)); }
  void Put(Byte b) { _window[(size_t)(_total++ & (kWindow - 1))] = b; }
  void Register(const Byte *p, size_t n) { for (size_t i = 0; i < n; i++) Put(p[i]); }
  // up to (want) bytes into (out), or until a mode change (returned)
  bool Read(CBitStream &bs, size_t want, std::vector<Byte> &out, CMode &next)
  {
    size_t have = 0;
    while (have < want)
    {
      if (_symsToRead == 0)
      {
        ReadTree(bs, kMaxCodeWidth, kNumMainCodes, _main);
        ReadTree(bs, kMaxCodeWidth, kNumLenCodes, _len);
        _symsToRead = bs.Read(15);
      }
      _symsToRead--;
      const unsigned sym = _main.ReadSymbol(bs);
      if (sym <= 255)
      {
        Put((Byte)sym);
        out.push_back((Byte)sym);
        have++;
        continue;
      }
      if (sym == kTypeCode)
      {
        next = CMode::ReadFrom(bs);
        return true;
      }
      if (sym > kTypeCode)
        throw CDataError();
      UInt32 len, dist;
      if (sym <= 259)
      {
        len = _len.ReadSymbol(bs);
        const unsigned offset = sym & 3;
        // move that recent distance to the end
        dist = _hist[3 - offset];
        for (unsigned i = 3 - offset; i < 3; i++)
          _hist[i] = _hist[i + 1];
        _hist[3] = dist;
        len += (offset > 1) ? 3 : 2;
      }
      else
      {
        dist = bs.KnownWidthUInt(sym - 260);
        len = _len.ReadSymbol(bs);
        _hist[0] = _hist[1]; _hist[1] = _hist[2]; _hist[2] = _hist[3]; _hist[3] = dist;
        len += (dist <= kMaxDistAtLen2) ? 2 : (dist <= kMaxDistAtLen3) ? 3 : 4;
      }
      dist++;
      if (have + len > want || dist > _total || dist > kWindow)
        throw CDataError();
      for (UInt32 i = 0; i < len; i++)
      {
        const Byte b = _window[(size_t)((_total - dist) & (kWindow - 1))];
        Put(b);
        out.push_back(b);
      }
      have += len;
    }
    return false;
  }
};

// the sound coder (acefile's Sound)
class CSound
{
  struct CChannel
  {
    unsigned ModelBase;
    int PredDifCnt[2], LastPredDifCnt[2], RarDifCnt[4], RarCoeff[4], RarDif[9];
    unsigned ByteCount;
    int LastSample, LastDelta, AdaptModelCnt, AdaptModelUse, GetState, GetCode;
    void Init(unsigned idx)
    {
      memset(this, 0, sizeof(*this));
      ModelBase = 3 * idx;
    }
  };
  std::vector<CTree> _trees;
  UInt32 _symsToRead;
  std::vector<CChannel> _channels;
  int _mode;
  static Byte Quant(int i)
  {
    static Byte q[256];
    static bool init = false;
    if (!init)
    {
      for (unsigned k = 1; k <= 128; k++)
        q[k] = q[256 - k] = (Byte)BitLength(k);
      init = true;
    }
    return q[i & 0xFF];
  }
  unsigned ReadSymbol(CBitStream &bs, unsigned model)
  {
    if (_symsToRead == 0)
    {
      for (size_t i = 0; i < _trees.size(); i++)
        ReadTree(bs, 10, 256 + 32 + 1, _trees[i]);
      _symsToRead = bs.Read(15);
    }
    _symsToRead--;
    return _trees[model].ReadSymbol(bs);
  }
  // a value, or -1 with (next) set for a mode change
  int Get(CChannel &c, CBitStream &bs, CMode &next)
  {
    int value = 0;
    if (c.GetState != 2)
    {
      unsigned model = (unsigned)c.GetState << 1;
      if (model == 0)
        model += (unsigned)c.AdaptModelUse;
      model += c.ModelBase;
      c.GetCode = (int)ReadSymbol(bs, model);
      if (c.GetCode == 256 + 32)
      {
        next = CMode::ReadFrom(bs);
        return -1;
      }
    }
    if (c.GetState == 0)
    {
      if (c.GetCode >= 32)
      {
        value = c.GetCode - 32;
        c.AdaptModelCnt = (c.AdaptModelCnt * 7 >> 3) + value;
        c.AdaptModelUse = c.AdaptModelCnt > 40 ? 1 : 0;
      }
      else
        c.GetState = 2;
    }
    else if (c.GetState == 1)
    {
      value = c.GetCode;
      c.GetState = 0;
    }
    if (c.GetState == 2)
    {
      if (c.GetCode == 0)
        c.GetState = 1;
      else
        c.GetCode--;
      value = 0;
    }
    return (value & 1) ? 255 - (value >> 1) : (value >> 1);
  }
  static int Predicted(const CChannel &c)
  {
    return UChar((8 * c.LastSample + c.RarCoeff[0] * c.RarDifCnt[0] + c.RarCoeff[1] * c.RarDifCnt[1]
        + c.RarCoeff[2] * c.RarDifCnt[2] + c.RarCoeff[3] * c.RarDifCnt[3]) >> 3);
  }
  static void Adjust(CChannel &c, int sample)
  {
    c.ByteCount++;
    const int predSample = Predicted(c);
    const int predDif = SChar(predSample - sample) * 8;
    c.RarDif[0] += abs(predDif - c.RarDifCnt[0]);
    c.RarDif[1] += abs(predDif + c.RarDifCnt[0]);
    c.RarDif[2] += abs(predDif - c.RarDifCnt[1]);
    c.RarDif[3] += abs(predDif + c.RarDifCnt[1]);
    c.RarDif[4] += abs(predDif - c.RarDifCnt[2]);
    c.RarDif[5] += abs(predDif + c.RarDifCnt[2]);
    c.RarDif[6] += abs(predDif - c.RarDifCnt[3]);
    c.RarDif[7] += abs(predDif + c.RarDifCnt[3]);
    c.RarDif[8] += abs(predDif);
    c.LastDelta = SChar(sample - c.LastSample);
    c.PredDifCnt[0] += Quant(predDif >> 3);
    c.PredDifCnt[1] += Quant(c.LastSample - sample);
    c.LastSample = sample;
    if ((c.ByteCount & 0x1F) == 0)
    {
      int minDif = 0xFFFF, minPos = 0;
      for (int i = 8; i >= 0; i--)
      {
        if (c.RarDif[i] <= minDif)
        {
          minDif = c.RarDif[i];
          minPos = i;
        }
        c.RarDif[i] = 0;
      }
      if (minPos != 8)
      {
        const int i = minPos >> 1;
        if ((minPos & 1) == 0)
        {
          if (c.RarCoeff[i] >= -16)
            c.RarCoeff[i]--;
        }
        else if (c.RarCoeff[i] <= 16)
          c.RarCoeff[i]++;
      }
      if ((c.ByteCount & 0xFF) == 0)
        for (int i = 0; i < 2; i++)
        {
          c.PredDifCnt[i] -= c.LastPredDifCnt[i];
          c.LastPredDifCnt[i] = c.PredDifCnt[i];
        }
    }
    c.RarDifCnt[3] = c.RarDifCnt[2];
    c.RarDifCnt[2] = c.RarDifCnt[1];
    c.RarDifCnt[1] = c.LastDelta - c.RarDifCnt[0];
    c.RarDifCnt[0] = c.LastDelta;
  }
public:
  void Reinit(int mode)
  {
    static const unsigned kNumChannels[4] = { 1, 2, 3, 3 };
    _mode = mode - kMode_Sound8;
    const unsigned n = kNumChannels[_mode];
    _trees.assign(n * 3, CTree());
    _symsToRead = 0;
    _channels.resize(n);
    for (unsigned i = 0; i < n; i++)
      _channels[i].Init(i);
  }
  bool Read(CBitStream &bs, size_t want, std::vector<Byte> &out, CMode &next)
  {
    static const unsigned kUse[4][4] = { { 0, 0, 0, 0 }, { 0, 1, 0, 1 }, { 0, 1, 0, 2 }, { 1, 0, 2, 0 } };
    const size_t n = want & ~(size_t)3;
    for (size_t i = 0; i < n; i++)
    {
      CChannel &c = _channels[kUse[_mode][i % 4]];
      const int v = Get(c, bs, next);
      if (v < 0)
        return true;
      const int pred = (c.PredDifCnt[0] > c.PredDifCnt[1]) ? c.LastSample : Predicted(c);
      const int sample = UChar(v + pred);
      out.push_back((Byte)sample);
      Adjust(c, SChar(sample));
    }
    return false;
  }
};

// the picture coder (acefile's Pic)
class CPic
{
  struct CContext { int Used, Predictor, Average, Errors[4]; };
  std::vector<CContext> _model0, _model1;
  unsigned _width, _planes;
  std::vector<int> _prevRow;
  std::vector<Byte> _leftover;
  static int DifBitWidth(int v)
  {
    v &= 0xFF;
    return v < 128 ? (int)BitLength((unsigned)(2 * v)) : (int)BitLength((unsigned)(-2 * (v - 256) - 1));
  }
  static int Quant(int d)
  {
    if (d <= -21) return -4;
    if (d <= -7) return -3;
    if (d <= -3) return -2;
    if (d <= -1) return -1;
    if (d == 0) return 0;
    if (d <= 2) return 1;
    if (d <= 6) return 2;
    if (d <= 20) return 3;
    return 4;
  }
  struct CDecoder
  {
    int Type;
    int A, B, C, D, X;
    void Init(int type) { Type = type; A = B = C = X = (type == 0) ? 0 : 128; D = 0; }
    void Shift() { C = A; A = D; B = X; }
    void UpdateD(int thisD, int refD)
    {
      if (Type == 0) D = thisD;
      else if (Type == 1) D = UChar(128 + thisD - refD);
      else D = UChar(128 + thisD - (refD * 11 >> 4));
    }
    int Produce(int refX) const
    {
      if (Type == 0) return X;
      if (Type == 1) return UChar(X + refX - 128);
      return UChar(X + (refX * 11 >> 4) - 128);
    }
    int Predict(int p) const
    {
      switch (p)
      {
        case 0: return A;
        case 1: return B;
        case 2: return (A + B) >> 1;
        default: return UChar(A + B - C);
      }
    }
    int Context() const { return abs(81 * Quant(D - A) + 9 * Quant(A - C) + Quant(C - B)); }
    void UpdateX(CBitStream &bs, CContext &ctx)
    {
      ctx.Used++;
      const int r = ctx.Average / ctx.Used;
      const int eps = bs.GolombRice(BitLength((unsigned)r), true);
      X = UChar(Predict(ctx.Predictor) + eps);
      ctx.Average += abs(eps);
      if (ctx.Used == 128)
      {
        ctx.Used >>= 1;
        ctx.Average >>= 1;
      }
      int best = 0;
      for (int i = 0; i < 4; i++)
      {
        ctx.Errors[i] += DifBitWidth(X - Predict(i));
        if (i == 0 || ctx.Errors[i] < ctx.Errors[best])
          best = i;
      }
      ctx.Predictor = best;
      bool over = false;
      for (int i = 0; i < 4; i++)
        if (ctx.Errors[i] > 0x7F)
          over = true;
      if (over)
        for (int i = 0; i < 4; i++)
          ctx.Errors[i] >>= 1;
    }
  };
public:
  void Reinit(CBitStream &bs)
  {
    _width = (unsigned)bs.GolombRice(12, false);
    _planes = (unsigned)bs.GolombRice(2, false);
    CContext c;
    memset(&c, 0, sizeof(c));
    c.Average = 4;
    _model0.assign(365, c);
    _model1.assign(365, c);
    _prevRow.assign(_width + _planes, 0);
    _leftover.clear();
  }
  bool Read(CBitStream &bs, size_t want, std::vector<Byte> &out, CMode &next)
  {
    size_t have = 0;
    if (!_leftover.empty())
    {
      out.insert(out.end(), _leftover.begin(), _leftover.end());
      have = _leftover.size();
      _leftover.clear();
    }
    while (have < want)
    {
      if (bs.Read(1) == 0)
      {
        next = CMode::ReadFrom(bs);
        return true;
      }
      if (_planes == 0 || _width > (1 << 20))
        throw CDataError();
      std::vector<int> row(_width + _planes, 0);
      for (unsigned plane = 0; plane < _planes; plane++)
      {
        std::vector<CContext> &model = plane == 0 ? _model0 : _model1;
        CDecoder dec;
        if (plane == 0)
          dec.Init(0);
        else
        {
          const unsigned t = bs.Read(2);
          if (t > 2)
            throw CDataError();
          dec.Init((int)t);
        }
        dec.UpdateD(_prevRow[plane], plane > 0 ? _prevRow[plane - 1] : _prevRow[_prevRow.size() - 1]);
        for (unsigned col = plane; col < _width; col += _planes)
        {
          dec.Shift();
          dec.UpdateD(_prevRow[col + _planes], _prevRow[col + _planes - 1]);
          const int ctx = dec.Context();
          if (ctx >= 365)
            throw CDataError();
          dec.UpdateX(bs, model[(size_t)ctx]);
          row[col] = dec.Produce(col > 0 ? row[col - 1] : row[row.size() - 1]);
        }
      }
      _prevRow = row;
      const size_t n = MyMin(want - have, (size_t)_width);
      for (size_t i = 0; i < n; i++)
        out.push_back((Byte)row[i]);
      for (size_t i = n; i < _width; i++)
        _leftover.push_back((Byte)row[i]);
      have += n;
    }
    return false;
  }
};

// a member's decoder, state kept for a solid archive
class CDecoder
{
public:
  CLz77 Lz77;
  CSound Sound;
  CPic Pic;

  // decodes (size) bytes of compression (type) from (in) into (sink)
  template <class Sink>
  void Decode(ISequentialInStream *in, UInt64 packSize, int type, UInt64 size, Sink &sink)
  {
    if (type == 0)
    {
      std::vector<Byte> buf(1 << 16);
      UInt64 left = size;
      while (left)
      {
        size_t n = (size_t)MyMin(left, (UInt64)buf.size());
        size_t got = n;
        if (ReadStream(in, buf.data(), &got) != S_OK || got != n)
          throw CDataError();
        Lz77.Register(buf.data(), n);
        sink(buf.data(), n);
        left -= n;
      }
      return;
    }
    CBitStream bs(in, packSize);
    Lz77.Reinit();
    std::vector<Byte> out;
    UInt64 produced = 0;
    if (type == 1)
    {
      while (produced < size)
      {
        out.clear();
        CMode next;
        if (Lz77.Read(bs, (size_t)size, out, next))
          throw CDataError();
        sink(out.data(), out.size());
        produced += out.size();
        if (out.empty())
          throw CDataError();
      }
      return;
    }
    if (type != 2)
      throw CDataError();
    std::vector<Byte> exeLeftover;
    int lastDelta = 0;
    CMode mode;
    mode.Mode = kMode_Lz77;
    CMode next;
    bool haveNext = false;
    while (produced < size)
    {
      if (haveNext)
      {
        if (mode.Mode != next.Mode)
        {
          if (next.Mode >= kMode_Sound8 && next.Mode <= kMode_Sound32B)
            Sound.Reinit(next.Mode);
          else if (next.Mode == kMode_Pic)
            Pic.Reinit(bs);
        }
        mode = next;
        haveNext = false;
      }
      out.clear();
      if (mode.Mode == kMode_Delta)
      {
        std::vector<Byte> delta;
        while (delta.size() < mode.DeltaLen)
        {
          CMode nm;
          const bool changed = Lz77.Read(bs, mode.DeltaLen - delta.size(), delta, nm);
          if (changed)
          {
            if (haveNext)
              throw CDataError();
            next = nm;
            haveNext = true;
            break;
          }
        }
        if (delta.empty() && haveNext)
          continue;
        for (size_t i = 0; i < delta.size(); i++)
        {
          delta[i] = (Byte)UChar(delta[i] + lastDelta);
          lastDelta = delta[i];
        }
        if (mode.DeltaDist == 0)
          throw CDataError();
        const size_t planeSize = mode.DeltaLen / mode.DeltaDist;
        for (size_t pos = 0; pos < planeSize; pos++)
          for (size_t plane = 0; plane < mode.DeltaLen; plane += planeSize)
          {
            if (plane + pos >= delta.size())
              throw CDataError();
            out.push_back(delta[plane + pos]);
          }
      }
      else if (mode.Mode == kMode_Lz77 || mode.Mode == kMode_Exe)
      {
        out.swap(exeLeftover);
        exeLeftover.clear();
        if (size - produced < out.size())
          throw CDataError();
        haveNext = Lz77.Read(bs, (size_t)(size - produced - out.size()), out, next);
        if (mode.Mode == kMode_Exe)
        {
          size_t i = 0;
          const size_t n = out.size();
          for (; i < n; i++)
          {
            if (i + 4 >= n)
              break;
            const UInt32 pos = (UInt32)(produced + i);
            if (out[i] == 0xE8)
            {
              if (mode.ExeMode == 0)
              {
                UInt32 rel = (out[i + 1] | ((UInt32)out[i + 2] << 8)) - pos;
                out[i + 1] = (Byte)rel; out[i + 2] = (Byte)(rel >> 8);
                i += 2;
              }
              else
              {
                UInt32 rel = GetUi32(&out[i + 1]) - pos;
                out[i + 1] = (Byte)rel; out[i + 2] = (Byte)(rel >> 8);
                out[i + 3] = (Byte)(rel >> 16); out[i + 4] = (Byte)(rel >> 24);
                i += 4;
              }
            }
            else if (out[i] == 0xE9)
            {
              UInt32 rel = (out[i + 1] | ((UInt32)out[i + 2] << 8)) - pos;
              out[i + 1] = (Byte)rel; out[i + 2] = (Byte)(rel >> 8);
              i += 2;
            }
          }
          // as acefile: past the byte the scan stopped at, an E8/E9 and
          // what follows waits for the next block
          for (size_t k = i + 1; k < n; k++)
            if (out[k] == 0xE8 || out[k] == 0xE9)
            {
              exeLeftover.assign(out.begin() + (ptrdiff_t)k, out.end());
              out.resize(k);
              break;
            }
        }
      }
      else if (mode.Mode >= kMode_Sound8 && mode.Mode <= kMode_Sound32B)
      {
        haveNext = Sound.Read(bs, (size_t)(size - produced), out, next);
        Lz77.Register(out.data(), out.size());
      }
      else if (mode.Mode == kMode_Pic)
      {
        haveNext = Pic.Read(bs, (size_t)(size - produced), out, next);
        Lz77.Register(out.data(), out.size());
      }
      else
        throw CDataError();
      sink(out.data(), out.size());
      produced += out.size();
      if (out.empty() && !haveNext)
        throw CDataError();
    }
  }
};

// ---------- encryption ----------

static UInt32 Rol(UInt32 x, unsigned n) { return (x << n) | (x >> (32 - n)); }

class CBlowfish
{
  UInt32 _p[18];
  UInt32 _s[4][256];
  UInt32 _lastL, _lastR;
  UInt32 F(UInt32 x) const
  {
    return ((_s[0][x >> 24] + _s[1][(x >> 16) & 0xFF]) ^ _s[2][(x >> 8) & 0xFF]) + _s[3][x & 0xFF];
  }
  void Encrypt(UInt32 &l, UInt32 &r) const
  {
    for (unsigned i = 0; i < 16; i += 2)
    {
      l ^= _p[i];
      r ^= F(l);
      r ^= _p[i + 1];
      l ^= F(r);
    }
    l ^= _p[16];
    r ^= _p[17];
    const UInt32 t = l; l = r; r = t;
  }
  void Decrypt(UInt32 &l, UInt32 &r) const
  {
    for (unsigned i = 16; i > 0; i -= 2)
    {
      l ^= _p[i + 1];
      r ^= F(l);
      r ^= _p[i];
      l ^= F(r);
    }
    l ^= _p[1];
    r ^= _p[0];
    const UInt32 t = l; l = r; r = t;
  }
public:
  void Init(const Byte *pwd, size_t len)
  {
    // the key: one SHA-1 compression over the password, words little endian
    if (len > 50)
      len = 50;
    Byte buf[64];
    memset(buf, 0, sizeof(buf));
    memcpy(buf, pwd, len);
    buf[len] = 0x80;
    UInt32 w[80];
    for (unsigned i = 0; i < 15; i++)
      w[i] = GetUi32(buf + i * 4);
    w[15] = (UInt32)(len << 3);
    for (unsigned i = 16; i < 80; i++)
      w[i] = w[i - 16] ^ w[i - 14] ^ w[i - 8] ^ w[i - 3];
    static const UInt32 kInit[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    UInt32 a = kInit[0], b = kInit[1], c = kInit[2], d = kInit[3], e = kInit[4];
    for (unsigned i = 0; i < 80; i++)
    {
      UInt32 f, k;
      if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
      else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
      else { f = b ^ c ^ d; k = 0xCA62C1D6; }
      const UInt32 t = Rol(a, 5) + f + e + w[i] + k;
      e = d; d = c; c = Rol(b, 30); b = a; a = t;
    }
    const UInt32 key[5] = { a + kInit[0], b + kInit[1], c + kInit[2], d + kInit[3], e + kInit[4] };
    for (unsigned i = 0; i < 18; i++)
      _p[i] = kBfP[i] ^ key[i % 5];
    memcpy(_s[0], kBfS0, sizeof(kBfS0));
    memcpy(_s[1], kBfS1, sizeof(kBfS1));
    memcpy(_s[2], kBfS2, sizeof(kBfS2));
    memcpy(_s[3], kBfS3, sizeof(kBfS3));
    UInt32 l = 0, r = 0;
    for (unsigned i = 0; i < 18; i += 2)
    {
      Encrypt(l, r);
      _p[i] = l; _p[i + 1] = r;
    }
    for (unsigned i = 0; i < 4; i++)
      for (unsigned j = 0; j < 256; j += 2)
      {
        Encrypt(l, r);
        _s[i][j] = l; _s[i][j + 1] = r;
      }
    _lastL = _lastR = 0;
  }
  // CBC with a zero IV, 8-byte blocks of two little-endian words
  void DecryptBlocks(Byte *data, size_t size)
  {
    for (size_t i = 0; i + 8 <= size; i += 8)
    {
      const UInt32 cl = GetUi32(data + i), cr = GetUi32(data + i + 4);
      UInt32 l = cl, r = cr;
      Decrypt(l, r);
      l ^= _lastL; r ^= _lastR;
      _lastL = cl; _lastR = cr;
      for (unsigned k = 0; k < 4; k++)
      {
        data[i + k] = (Byte)(l >> (8 * k));
        data[i + 4 + k] = (Byte)(r >> (8 * k));
      }
    }
  }
};

// decrypts what it reads, a block of 8 at a time
class CDecryptStream Z7_final:
  public ISequentialInStream,
  public CMyUnknownImp
{
  Z7_IFACES_IMP_UNK_1(ISequentialInStream)
  Byte _buf[1 << 16];
  size_t _pos, _len;
public:
  CMyComPtr<ISequentialInStream> In;
  CBlowfish Bf;
  CDecryptStream(): _pos(0), _len(0) {}
};

Z7_COM7F_IMF(CDecryptStream::Read(void *data, UInt32 size, UInt32 *processedSize))
{
  if (processedSize)
    *processedSize = 0;
  if (_pos == _len)
  {
    size_t got = sizeof(_buf);
    RINOK(ReadStream(In, _buf, &got))
    got &= ~(size_t)7;
    Bf.DecryptBlocks(_buf, got);
    _pos = 0;
    _len = got;
  }
  const UInt32 n = (UInt32)MyMin((size_t)size, _len - _pos);
  memcpy(data, _buf + _pos, n);
  _pos += n;
  if (processedSize)
    *processedSize = n;
  return S_OK;
}

// ---------- the archive ----------

struct CSegment
{
  unsigned Volume;
  UInt64 Offset;
  UInt64 Size;
};

struct CMember
{
  UString Path;
  UInt64 Size;
  UInt64 PackSize;
  UInt32 DosTime;
  UInt32 Attrib;
  UInt32 Crc;
  int CompType;
  int CompQual;
  bool Encrypted;
  bool IsDir;
  std::vector<CSegment> Segments;
};

class CHandler Z7_final:
  public IInArchive,
  public IInArchiveGetStream,
  public CMyUnknownImp
{
  Z7_IFACES_IMP_UNK_2(IInArchive, IInArchiveGetStream)
  CObjectVector<CMember> _members;
  std::vector<CMyComPtr<IInStream> > _volumes;
  bool _solid;
  UInt64 _phySize;
  UString _comment;
public:
  AString _password;
  bool _passwordDefined;
private:
  HRESULT ParseVolume(IInStream *stream, unsigned volume, bool first);
  HRESULT Open2(IInStream *stream, IArchiveOpenCallback *callback);
public:
  // a member's packed data over its volumes
  class CSegStream;
  HRESULT DecodeMember(CDecoder &dec, unsigned index, ISequentialOutStream *out, Int32 &opRes);
};

// the packed data of a member, segment after segment
class CHandler::CSegStream Z7_final:
  public ISequentialInStream,
  public CMyUnknownImp
{
  Z7_IFACES_IMP_UNK_1(ISequentialInStream)
public:
  const std::vector<CMyComPtr<IInStream> > *Volumes;
  const CMember *M;
  size_t Seg;
  UInt64 Pos;
};

Z7_COM7F_IMF(CHandler::CSegStream::Read(void *data, UInt32 size, UInt32 *processedSize))
{
  if (processedSize)
    *processedSize = 0;
  while (Seg < M->Segments.size() && Pos >= M->Segments[Seg].Size)
  {
    Seg++;
    Pos = 0;
  }
  if (Seg >= M->Segments.size() || size == 0)
    return S_OK;
  const CSegment &s = M->Segments[Seg];
  const UInt32 n = (UInt32)MyMin((UInt64)size, s.Size - Pos);
  IInStream *v = (*Volumes)[s.Volume];
  RINOK(InStream_SeekSet(v, s.Offset + Pos))
  UInt32 got = 0;
  RINOK(v->Read(data, n, &got))
  Pos += got;
  if (processedSize)
    *processedSize = got;
  return S_OK;
}

static UString DecodeName(const Byte *p, size_t n)
{
  AString a;
  a.SetFrom((const char *)p, (unsigned)n);
  UString u;
  if (!ConvertUTF8ToUnicode(a, u))
    u = MultiByteToUnicodeString(a, CP_OEMCP);
  u.Replace(L'\\', L'/');
  while (u.IsPrefixedBy(L"/"))
    u.DeleteFrontal(1);
  return u;
}

HRESULT CHandler::ParseVolume(IInStream *stream, unsigned volume, bool first)
{
  UInt64 fileSize;
  RINOK(NItemArc::GetStreamSize(stream, fileSize))
  UInt64 pos = 0;
  bool sawMain = false;
  while (pos + 4 <= fileSize)
  {
    Byte h4[4];
    RINOK(ReadAt(stream, pos, h4, 4))
    const UInt32 hcrc = GetUi16(h4), hsize = GetUi16(h4 + 2);
    if (hsize < 3 || pos + 4 + hsize > fileSize)
    {
      if (first && !sawMain)
        return S_FALSE;
      break;
    }
    CByteBuffer h(hsize);
    RINOK(ReadAt(stream, pos + 4, h, hsize))
    if ((crc32(0, h, hsize) & 0xFFFF) != ((~hcrc) & 0xFFFF) && ((crc32(0, h, hsize) ^ 0xFFFFFFFF) & 0xFFFF) != hcrc)
    {
      if (first && !sawMain)
        return S_FALSE;
      break;
    }
    pos += 4 + hsize;
    const unsigned type = h[0];
    const unsigned flags = GetUi16(h + 1);
    if (type == 0)
    {
      if (hsize < 26 || memcmp(h + 3, "**ACE**", 7) != 0)
        return S_FALSE;
      sawMain = true;
      if (first)
      {
        _solid = (flags & 0x8000) != 0;
        AString c ("ACE ");
        c.Add_UInt32(h[11] / 10); c.Add_Dot(); c.Add_UInt32(h[11] % 10);
        if (_solid) c += ", solid";
        if (flags & 0x800) c += ", multi-volume";
        _comment = GetUnicodeString(c);
      }
      continue;
    }
    if (type == 1 || type == 3)
    {
      size_t i = 3;
      UInt64 packSize, origSize;
      if (flags & 4)
      {
        if (hsize < i + 16) return S_FALSE;
        packSize = GetUi64(h + i); origSize = GetUi64(h + i + 8); i += 16;
      }
      else
      {
        if (hsize < i + 8) return S_FALSE;
        packSize = GetUi32(h + i); origSize = GetUi32(h + i + 4); i += 8;
      }
      if (hsize < i + 20) return S_FALSE;
      const UInt32 dosTime = GetUi32(h + i);
      const UInt32 attrib = GetUi32(h + i + 4);
      const UInt32 crc = GetUi32(h + i + 8);
      const int compType = h[i + 12], compQual = h[i + 13];
      const unsigned params = GetUi16(h + i + 14);
      const unsigned nameLen = GetUi16(h + i + 18);
      i += 20;
      if (hsize < i + nameLen) return S_FALSE;
      const UString name = DecodeName(h + i, nameLen);
      const UInt64 dataPos = pos;
      pos += packSize;
      CSegment seg;
      seg.Volume = volume;
      seg.Offset = dataPos;
      seg.Size = packSize;
      if ((flags & 0x1000) && !_members.IsEmpty() && _members.Back().Path == name)
      {
        // continued from the previous volume
        CMember &m = _members.Back();
        m.Segments.push_back(seg);
        m.PackSize += packSize;
        m.Crc = crc;
        continue;
      }
      CMember m;
      m.Path = name;
      m.Size = origSize;
      m.PackSize = packSize;
      m.DosTime = dosTime;
      m.Attrib = attrib;
      m.Crc = crc;
      m.CompType = compType;
      m.CompQual = compQual | (int)((params & 15) << 8);
      m.Encrypted = (flags & 0x4000) != 0;
      m.IsDir = (attrib & 0x10) != 0;
      m.Segments.push_back(seg);
      _members.Add(m);
      continue;
    }
    if (type == 2 || type == 4 || type == 5)
    {
      // a recovery record: skip its data
      if (hsize < 3 + 4)
        break;
      const UInt64 add = (flags & 4) ? (hsize >= 11 ? GetUi64(h + 3) : 0) : GetUi32(h + 3);
      pos += add;
      continue;
    }
    break;
  }
  if (first)
    _phySize = pos;
  return sawMain ? S_OK : S_FALSE;
}

HRESULT CHandler::Open2(IInStream *stream, IArchiveOpenCallback *callback)
{
  _volumes.push_back(stream);
  RINOK(ParseVolume(stream, 0, true))
  // further volumes: name.c00, name.c01 ...
  if (!_members.IsEmpty())
  {
    const UString arcName = NItemArc::GetArcName(callback);
    const int dot = arcName.ReverseFind_Dot();
    if (dot > 0)
    {
      const UString base = arcName.Left((unsigned)dot);
      for (unsigned k = 0; k < 1000; k++)
      {
        char ext[16];
        snprintf(ext, sizeof(ext), ".c%02u", k);
        CMyComPtr<IInStream> v;
        RINOK(NItemArc::OpenSiblingFile(callback, base + GetUnicodeString(ext), v))
        if (!v)
          break;
        _volumes.push_back(v);
        if (ParseVolume(v, (unsigned)_volumes.size() - 1, false) != S_OK)
        {
          _volumes.pop_back();
          break;
        }
      }
    }
  }
  return S_OK;
}

HRESULT CHandler::DecodeMember(CDecoder &dec, unsigned index, ISequentialOutStream *out, Int32 &opRes)
{
  const CMember &m = _members[index];
  opRes = NExtract::NOperationResult::kOK;
  if (m.IsDir || m.Size == 0)
    return S_OK;
  if (m.Encrypted && !_passwordDefined)
  {
    opRes = NExtract::NOperationResult::kWrongPassword;
    return S_OK;
  }
  if (m.CompType > 2)
  {
    opRes = NExtract::NOperationResult::kUnsupportedMethod;
    return S_OK;
  }
  CSegStream *spec = new CSegStream;
  CMyComPtr<ISequentialInStream> in = spec;
  spec->Volumes = &_volumes;
  spec->M = &m;
  spec->Seg = 0;
  spec->Pos = 0;
  if (m.Encrypted)
  {
    CDecryptStream *d = new CDecryptStream;
    CMyComPtr<ISequentialInStream> dRef = d;
    d->In = in;
    d->Bf.Init((const Byte *)_password.Ptr(), _password.Len());
    in = dRef;
  }
  UInt32 crc = 0xFFFFFFFF;
  UInt64 written = 0;
  HRESULT writeRes = S_OK;
  auto sink = [&](const Byte *p, size_t n)
  {
    crc = (UInt32)crc32(crc ^ 0xFFFFFFFF, p, (uInt)n) ^ 0xFFFFFFFF;
    written += n;
    if (out && writeRes == S_OK)
      writeRes = WriteStream(out, p, n);
  };
  try
  {
    dec.Decode(in, m.PackSize, m.CompType, m.Size, sink);
  }
  catch (const CDataError &)
  {
    opRes = m.Encrypted ? NExtract::NOperationResult::kWrongPassword : NExtract::NOperationResult::kDataError;
    return writeRes;
  }
  RINOK(writeRes)
  if (written != m.Size || crc != m.Crc)
    opRes = m.Encrypted ? NExtract::NOperationResult::kWrongPassword
        : (written != m.Size ? NExtract::NOperationResult::kDataError : NExtract::NOperationResult::kCRCError);
  return S_OK;
}

static const Byte kProps[] = { kpidPath, kpidIsDir, kpidSize, kpidPackSize, kpidMTime, kpidAttrib, kpidCRC, kpidMethod, kpidEncrypted };
static const Byte kArcProps[] = { kpidComment, kpidSolid };
IMP_IInArchive_Props
IMP_IInArchive_ArcProps

Z7_COM7F_IMF(CHandler::GetArchiveProperty(PROPID propID, PROPVARIANT *value))
{
  NCOM::CPropVariant prop;
  switch (propID)
  {
    case kpidPhySize: prop = _phySize; break;
    case kpidComment: prop = _comment; break;
    case kpidSolid: prop = _solid; break;
    default: break;
  }
  prop.Detach(value);
  return S_OK;
}

Z7_COM7F_IMF(CHandler::GetProperty(UInt32 index, PROPID propID, PROPVARIANT *value))
{
  COM_TRY_BEGIN
  NCOM::CPropVariant prop;
  const CMember &m = _members[index];
  switch (propID)
  {
    case kpidPath: prop = m.Path; break;
    case kpidIsDir: prop = m.IsDir; break;
    case kpidSize: if (!m.IsDir) prop = m.Size; break;
    case kpidPackSize: if (!m.IsDir) prop = m.PackSize; break;
    case kpidAttrib: prop = m.Attrib; break;
    case kpidCRC: if (!m.IsDir) prop = m.Crc; break;
    case kpidEncrypted: prop = m.Encrypted; break;
    case kpidMTime:
    {
      FILETIME ft, utc;
      if (m.DosTime && NTime::DosTime_To_FileTime(m.DosTime, ft) && LocalFileTimeToFileTime(&ft, &utc))
        prop = utc;
      break;
    }
    case kpidMethod:
    {
      static const char * const kTypes[] = { "Stored", "LZ77", "Blocked" };
      AString s (m.CompType <= 2 ? kTypes[m.CompType] : "?");
      if (m.CompType != 0)
      {
        s += ":";
        s.Add_UInt32((unsigned)((m.CompQual >> 8) + 10));
      }
      prop = s;
      break;
    }
    default: break;
  }
  prop.Detach(value);
  return S_OK;
  COM_TRY_END
}

Z7_COM7F_IMF(CHandler::Open(IInStream *stream, const UInt64 *, IArchiveOpenCallback *callback))
{
  COM_TRY_BEGIN
  Close();
  const HRESULT res = Open2(stream, callback);
  if (res != S_OK)
    Close();
  return res;
  COM_TRY_END
}

Z7_COM7F_IMF(CHandler::Close())
{
  _members.Clear();
  _volumes.clear();
  _solid = false;
  _phySize = 0;
  _passwordDefined = false;
  _password.Empty();
  return S_OK;
}

Z7_COM7F_IMF(CHandler::GetNumberOfItems(UInt32 *numItems))
{
  *numItems = _members.Size();
  return S_OK;
}

Z7_COM7F_IMF(CHandler::Extract(const UInt32 *indices, UInt32 numItems,
    Int32 testMode, IArchiveExtractCallback *extractCallback))
{
  COM_TRY_BEGIN
  const bool allFilesMode = (numItems == (UInt32)(Int32)-1);
  if (allFilesMode)
    numItems = _members.Size();
  if (numItems == 0)
    return S_OK;
  UInt64 total = 0;
  for (UInt32 i = 0; i < numItems; i++)
    total += _members[allFilesMode ? i : indices[i]].Size;
  RINOK(extractCallback->SetTotal(total))
  // the password, once, if a member needs it
  if (!_passwordDefined)
  {
    bool encrypted = false;
    for (UInt32 i = 0; i < numItems; i++)
      if (_members[allFilesMode ? i : indices[i]].Encrypted)
        encrypted = true;
    Z7_DECL_CMyComPtr_QI_FROM(ICryptoGetTextPassword, getPassword, extractCallback)
    if (encrypted && getPassword)
    {
      CMyComBSTR bstr;
      RINOK(getPassword->CryptoGetTextPassword(&bstr))
      UString u = bstr ? (const wchar_t *)bstr : L"";
      ConvertUnicodeToUTF8(u, _password);
      _passwordDefined = true;
    }
  }
  // a solid archive is decoded from the start, every member on the way
  std::unique_ptr<CDecoder> dec(new CDecoder);
  UInt32 next = 0;
  UInt64 done = 0;
  for (UInt32 i = 0; i < numItems; i++)
  {
    RINOK(extractCallback->SetCompleted(&done))
    const UInt32 index = allFilesMode ? i : indices[i];
    if (_solid)
    {
      if (index < next)
      {
        dec.reset(new CDecoder);
        next = 0;
      }
      for (; next < index; next++)
      {
        Int32 r;
        RINOK(DecodeMember(*dec, next, NULL, r))
      }
    }
    const CMember &m = _members[index];
    CMyComPtr<ISequentialOutStream> out;
    const Int32 askMode = testMode ? NExtract::NAskMode::kTest : NExtract::NAskMode::kExtract;
    RINOK(extractCallback->GetStream(index, &out, askMode))
    done += m.Size;
    if (!testMode && !out && !_solid)
      continue;
    RINOK(extractCallback->PrepareOperation(askMode))
    Int32 opRes;
    RINOK(DecodeMember(*dec, index, out, opRes))
    next = index + 1;
    out.Release();
    RINOK(extractCallback->SetOperationResult(opRes))
  }
  return extractCallback->SetCompleted(&done);
  COM_TRY_END
}

Z7_COM7F_IMF(CHandler::GetStream(UInt32 index, ISequentialInStream **stream))
{
  COM_TRY_BEGIN
  *stream = NULL;
  if (index >= _members.Size() || _members[index].IsDir || _members[index].Size > ((UInt64)1 << 28))
    return S_FALSE;
  CDecoder *decRaw = new CDecoder;
  std::unique_ptr<CDecoder> dec(decRaw);
  if (_solid)
    for (UInt32 k = 0; k < index; k++)
    {
      Int32 r;
      RINOK(DecodeMember(*dec, k, NULL, r))
    }
  CReferenceBuf *ref = new CReferenceBuf;
  CMyComPtr<IUnknown> refRef = ref;
  ref->Buf.Alloc((size_t)_members[index].Size);
  CBufPtrSeqOutStream *spec = new CBufPtrSeqOutStream;
  CMyComPtr<ISequentialOutStream> out = spec;
  spec->Init(ref->Buf, ref->Buf.Size());
  Int32 opRes;
  RINOK(DecodeMember(*dec, index, out, opRes))
  if (opRes != NExtract::NOperationResult::kOK)
    return S_FALSE;
  CBufInStream *in = new CBufInStream;
  CMyComPtr<ISequentialInStream> inRef = in;
  in->Init(ref);
  *stream = inRef.Detach();
  return S_OK;
  COM_TRY_END
}

static const Byte k_Signature[] = { '*', '*', 'A', 'C', 'E', '*', '*' };

REGISTER_ARC_I(
  "ACE", "ace c00", NULL, 0xA8,
  k_Signature,
  7,
  NArcInfoFlags::kFindSignature,
  NULL)

}}

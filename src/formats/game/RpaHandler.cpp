// RpaHandler.cpp -- 7zfm-qt: Ren'Py archives (.rpa), as Ren'Py's own
// loader reads them (renpy/loader.py, MIT license):
//
//   RPA-3.0  "RPA-3.0 " offset (16 hex digits) " " key (8 hex digits), and at
//            (offset) the index: a zlib-compressed Python pickle of a dict
//            {name: [(offset, length[, prefix]), ...]}, offsets and lengths
//            XORed with the key; a file's data is its prefix (bytes kept in
//            the index), then (length) bytes at (offset), for each entry
//   RPA-2.0  "RPA-2.0 " offset, the same index without a key or prefixes
//   RPA-1.0  the index alone in name.rpi (zlib, no header), the data in
//            name.rpa; opened as the .rpi
//
// The pickle reader knows what pickle.dumps() writes for such a dict in
// protocols 2 to 5 (Python 2 and 3), including Python 3's bytes in
// protocol 2 (a call of _codecs.encode).

#include "Common/ComTry.h"
#include "Common/MyBuffer.h"
#include "Common/StringConvert.h"
#include "Common/StringToInt.h"
#include "Common/UTFConvert.h"

#include "7zip/Common/RegisterArc.h"
#include "7zip/Common/StreamUtils.h"

#include "../common/ItemArchive.h"

#include <zlib.h>

#include <memory>
#include <string>
#include <string.h>
#include <vector>

namespace NArchive {
namespace NRpa {

using namespace NItemArc;

// ---------- a small pickle reader ----------

// the code points of UTF-8 (s), each below 256, as bytes: what
// str.encode("latin1") gives
static bool Utf8ToLatin1(const std::string &s, std::string &out)
{
  out.clear();
  for (size_t i = 0; i < s.size();)
  {
    const Byte c = (Byte)s[i];
    if (c < 0x80) { out += (char)c; i++; }
    else if ((c & 0xE0) == 0xC0 && i + 1 < s.size() && c <= 0xC3)
    {
      out += (char)(Byte)(((c & 0x1F) << 6) | ((Byte)s[i + 1] & 0x3F));
      i += 2;
    }
    else
      return false;
  }
  return true;
}

struct CValue;
typedef std::shared_ptr<CValue> CValuePtr;

struct CValue
{
  enum EType { kNone, kInt, kStr, kBytes, kList, kTuple, kDict, kMark, kGlobal } Type;
  Int64 Int;
  std::string Bytes;             // kStr: UTF-8; kBytes: raw; kGlobal: "module name"
                                 // (not AString: bytes hold zeros)
  std::vector<CValuePtr> Items;  // kList, kTuple; kDict: key, value, key, value...
  CValue(EType t = kNone): Type(t), Int(0) {}
};

static CValuePtr NewValue(CValue::EType t) { return std::make_shared<CValue>(t); }

class CPickleReader
{
  const Byte *_p;
  size_t _size;
  size_t _pos;
  std::vector<CValuePtr> _stack;
  std::vector<size_t> _marks;
  std::vector<CValuePtr> _memo;

  bool Need(size_t n) const { return _size - _pos >= n; }
  UInt64 ReadLe(unsigned n)
  {
    UInt64 v = 0;
    for (unsigned i = 0; i < n; i++)
      v |= (UInt64)_p[_pos + i] << (8 * i);
    _pos += n;
    return v;
  }
  bool ReadLine(AString &s)
  {
    const Byte *e = (const Byte *)memchr(_p + _pos, '\n', _size - _pos);
    if (!e)
      return false;
    s.SetFrom_CalcLen((const char *)_p + _pos, (unsigned)(e - (_p + _pos)));
    _pos = (size_t)(e - _p) + 1;
    return true;
  }
  bool Pop(CValuePtr &v)
  {
    if (_stack.empty() || (!_marks.empty() && _stack.size() <= _marks.back()))
      return false;
    v = _stack.back();
    _stack.pop_back();
    return true;
  }
  // the values since the last mark, removed with it
  bool PopMark(std::vector<CValuePtr> &items)
  {
    if (_marks.empty())
      return false;
    const size_t m = _marks.back();
    _marks.pop_back();
    items.assign(_stack.begin() + (ptrdiff_t)m, _stack.end());
    _stack.resize(m);
    return true;
  }
  bool Top(CValuePtr &v)
  {
    if (_stack.empty())
      return false;
    v = _stack.back();
    return true;
  }
  bool PushString(size_t len, CValue::EType t)
  {
    if (!Need(len) || len > 0x7FFFFFFF)
      return false;
    CValuePtr v = NewValue(t);
    v->Bytes.assign((const char *)_p + _pos, len);
    _pos += len;
    _stack.push_back(v);
    return true;
  }
  bool PushInt(Int64 i)
  {
    CValuePtr v = NewValue(CValue::kInt);
    v->Int = i;
    _stack.push_back(v);
    return true;
  }
  bool MakeTuple(size_t n)
  {
    if (_stack.size() < n || (!_marks.empty() && _stack.size() - n < _marks.back()))
      return false;
    CValuePtr t = NewValue(CValue::kTuple);
    t->Items.assign(_stack.end() - (ptrdiff_t)n, _stack.end());
    _stack.resize(_stack.size() - n);
    _stack.push_back(t);
    return true;
  }
public:
  CPickleReader(const Byte *p, size_t size): _p(p), _size(size), _pos(0) {}
  bool Read(CValuePtr &result);
};

bool CPickleReader::Read(CValuePtr &result)
{
  for (;;)
  {
    if (!Need(1))
      return false;
    const Byte op = _p[_pos++];
    switch (op)
    {
      case 0x80: if (!Need(1)) return false; _pos++; break;              // PROTO
      case 0x95: if (!Need(8)) return false; _pos += 8; break;           // FRAME
      case '.':                                                          // STOP
        return Pop(result);
      case '(': _marks.push_back(_stack.size()); break;                  // MARK
      case 'N': _stack.push_back(NewValue(CValue::kNone)); break;
      case 0x88: PushInt(1); break;                                      // NEWTRUE
      case 0x89: PushInt(0); break;                                      // NEWFALSE
      case '}': _stack.push_back(NewValue(CValue::kDict)); break;
      case ']': _stack.push_back(NewValue(CValue::kList)); break;
      case ')': _stack.push_back(NewValue(CValue::kTuple)); break;
      case 'K': if (!Need(1)) return false; PushInt((Int64)ReadLe(1)); break;   // BININT1
      case 'M': if (!Need(2)) return false; PushInt((Int64)ReadLe(2)); break;   // BININT2
      case 'J': if (!Need(4)) return false; PushInt((Int32)(UInt32)ReadLe(4)); break;
      case 0x8A: case 0x8B:                                              // LONG1, LONG4
      {
        const unsigned lenSize = (op == 0x8A) ? 1 : 4;
        if (!Need(lenSize)) return false;
        const UInt64 n = ReadLe(lenSize);
        if (n > 8 || !Need((size_t)n)) return false;
        UInt64 v = 0;
        for (unsigned i = 0; i < n; i++)
          v |= (UInt64)_p[_pos + i] << (8 * i);
        if (n != 0 && n < 8 && (_p[_pos + n - 1] & 0x80))
          v |= ~(UInt64)0 << (8 * n);
        _pos += (size_t)n;
        PushInt((Int64)v);
        break;
      }
      case 'I': case 'L':                                                // INT, LONG (text)
      {
        AString s;
        if (!ReadLine(s)) return false;
        if (s.Back() == 'L') s.DeleteBack();
        if (s == "01") { PushInt(1); break; }
        if (s == "00") { PushInt(0); break; }
        const char *end;
        const bool neg = (s[0] == '-');
        const UInt64 v = ConvertStringToUInt64(s.Ptr(neg ? 1 : 0), &end);
        PushInt(neg ? -(Int64)v : (Int64)v);
        break;
      }
      case 'X': if (!Need(4)) return false; if (!PushString((size_t)ReadLe(4), CValue::kStr)) return false; break;
      case 0x8C: if (!Need(1)) return false; if (!PushString((size_t)ReadLe(1), CValue::kStr)) return false; break;
      case 0x8D: if (!Need(8)) return false; if (!PushString((size_t)ReadLe(8), CValue::kStr)) return false; break;
      // Python 2 str: bytes; names in it are taken as UTF-8
      case 'T': if (!Need(4)) return false; if (!PushString((size_t)ReadLe(4), CValue::kBytes)) return false; break;
      case 'U': if (!Need(1)) return false; if (!PushString((size_t)ReadLe(1), CValue::kBytes)) return false; break;
      case 'B': if (!Need(4)) return false; if (!PushString((size_t)ReadLe(4), CValue::kBytes)) return false; break;
      case 'C': if (!Need(1)) return false; if (!PushString((size_t)ReadLe(1), CValue::kBytes)) return false; break;
      case 0x8E: if (!Need(8)) return false; if (!PushString((size_t)ReadLe(8), CValue::kBytes)) return false; break;
      case 0x85: if (!MakeTuple(1)) return false; break;
      case 0x86: if (!MakeTuple(2)) return false; break;
      case 0x87: if (!MakeTuple(3)) return false; break;
      case 't': case 'l':                                                // TUPLE, LIST
      {
        CValuePtr v = NewValue(op == 't' ? CValue::kTuple : CValue::kList);
        if (!PopMark(v->Items)) return false;
        _stack.push_back(v);
        break;
      }
      case 'd':                                                          // DICT
      {
        CValuePtr v = NewValue(CValue::kDict);
        if (!PopMark(v->Items) || (v->Items.size() & 1)) return false;
        _stack.push_back(v);
        break;
      }
      case 'a':                                                          // APPEND
      {
        CValuePtr x, list;
        if (!Pop(x) || !Top(list) || list->Type != CValue::kList) return false;
        list->Items.push_back(x);
        break;
      }
      case 'e': case 'u':                                                // APPENDS, SETITEMS
      {
        std::vector<CValuePtr> items;
        CValuePtr target;
        if (!PopMark(items) || !Top(target)) return false;
        if (op == 'e' ? target->Type != CValue::kList : (target->Type != CValue::kDict || (items.size() & 1)))
          return false;
        target->Items.insert(target->Items.end(), items.begin(), items.end());
        break;
      }
      case 's':                                                          // SETITEM
      {
        CValuePtr k, v, dict;
        if (!Pop(v) || !Pop(k) || !Top(dict) || dict->Type != CValue::kDict) return false;
        dict->Items.push_back(k);
        dict->Items.push_back(v);
        break;
      }
      case 'q': case 'r': case 0x94:                                     // BINPUT, LONG_BINPUT, MEMOIZE
      {
        size_t idx = _memo.size();
        if (op != 0x94)
        {
          const unsigned n = (op == 'q') ? 1 : 4;
          if (!Need(n)) return false;
          idx = (size_t)ReadLe(n);
        }
        CValuePtr v;
        if (!Top(v) || idx > (1 << 24)) return false;
        if (_memo.size() <= idx)
          _memo.resize(idx + 1);
        _memo[idx] = v;
        break;
      }
      case 'h': case 'j':                                                // BINGET, LONG_BINGET
      {
        const unsigned n = (op == 'h') ? 1 : 4;
        if (!Need(n)) return false;
        const size_t idx = (size_t)ReadLe(n);
        if (idx >= _memo.size() || !_memo[idx]) return false;
        _stack.push_back(_memo[idx]);
        break;
      }
      case 'c':                                                          // GLOBAL
      {
        AString module, name;
        if (!ReadLine(module) || !ReadLine(name)) return false;
        CValuePtr v = NewValue(CValue::kGlobal);
        v->Bytes = module.Ptr();
        v->Bytes += ' ';
        v->Bytes += name.Ptr();
        _stack.push_back(v);
        break;
      }
      case 'R':                                                          // REDUCE
      {
        // only Python 3's bytes in protocol 2: _codecs.encode(str, "latin1"),
        // or __builtin__.bytes() for empty ones
        CValuePtr args, f;
        if (!Pop(args) || !Pop(f)) return false;
        if (f->Type == CValue::kGlobal && (f->Bytes == "__builtin__ bytes" || f->Bytes == "builtins bytes")
            && args->Type == CValue::kTuple && args->Items.empty())
        {
          _stack.push_back(NewValue(CValue::kBytes));
          break;
        }
        if (f->Type != CValue::kGlobal || f->Bytes != "_codecs encode"
            || args->Type != CValue::kTuple || args->Items.empty() || args->Items[0]->Type != CValue::kStr)
          return false;
        CValuePtr v = NewValue(CValue::kBytes);
        if (!Utf8ToLatin1(args->Items[0]->Bytes, v->Bytes))
          return false;
        _stack.push_back(v);
        break;
      }
      default:
        return false;
    }
  }
}

// ---------- the archive ----------

class CHandler Z7_final: public CHandlerBase
{
  HRESULT Open2(IInStream *stream, IArchiveOpenCallback *callback) Z7_override;
};

static bool Hex(const Byte *p, unsigned n, UInt64 &v)
{
  v = 0;
  for (unsigned i = 0; i < n; i++)
  {
    const Byte c = p[i];
    unsigned d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    else return false;
    v = (v << 4) | d;
  }
  return true;
}

static HRESULT ReadIndex(IInStream *stream, UInt64 offset, UInt64 end, CByteBuffer &out)
{
  if (offset >= end || end - offset > ((UInt64)1 << 28))
    return S_FALSE;
  CByteBuffer packed((size_t)(end - offset));
  RINOK(ReadAt(stream, offset, packed, packed.Size()))
  z_stream z;
  memset(&z, 0, sizeof(z));
  if (inflateInit(&z) != Z_OK)
    return E_OUTOFMEMORY;
  size_t cap = packed.Size() * 4 + 1024;
  out.Alloc(cap);
  z.next_in = packed;
  z.avail_in = (uInt)packed.Size();
  size_t done = 0;
  int r = Z_OK;
  while (r == Z_OK)
  {
    if (done == cap)
    {
      if (cap > ((size_t)1 << 30))
        break;
      cap *= 2;
      out.ChangeSize_KeepData(cap, done);
    }
    z.next_out = (Byte *)out + done;
    z.avail_out = (uInt)(cap - done);
    r = inflate(&z, Z_NO_FLUSH);
    done = cap - z.avail_out;
  }
  inflateEnd(&z);
  if (r != Z_STREAM_END)
    return S_FALSE;
  out.ChangeSize_KeepData(done, done);
  return S_OK;
}

static bool GetName(const CValue &v, UString &name)
{
  if (v.Type != CValue::kStr && v.Type != CValue::kBytes)
    return false;
  const AString a (v.Bytes.c_str());
  if (!ConvertUTF8ToUnicode(a, name))
    name = GetUnicodeString(a);
  return true;
}

HRESULT CHandler::Open2(IInStream *stream, IArchiveOpenCallback *callback)
{
  UInt64 fileSize = 0;
  RINOK(GetStreamSize(stream, fileSize))
  Byte h[40];
  memset(h, 0, sizeof(h));
  const size_t hSize = (size_t)MyMin(fileSize, (UInt64)sizeof(h));
  RINOK(ReadAt(stream, 0, h, hSize))

  UInt64 indexOffset = 0;
  UInt64 key = 0;
  int version;
  if (hSize >= 33 && memcmp(h, "RPA-3.0 ", 8) == 0)
  {
    if (!Hex(h + 8, 16, indexOffset) || h[24] != ' ' || !Hex(h + 25, 8, key))
      return S_FALSE;
    version = 3;
  }
  else if (hSize >= 24 && memcmp(h, "RPA-2.0 ", 8) == 0)
  {
    if (!Hex(h + 8, 16, indexOffset))
      return S_FALSE;
    version = 2;
  }
  else if (hSize >= 2 && h[0] == 0x78)
  {
    // RPA-1.0: this is the .rpi index; the data is in the .rpa of that name
    UString name = GetArcName(callback);
    const int dot = name.ReverseFind_Dot();
    if (dot < 0 || !StringsAreEqualNoCase_Ascii(name.Ptr((unsigned)dot + 1), "rpi"))
      return S_FALSE;
    name.DeleteFrom((unsigned)dot + 1);
    name += "rpa";
    RINOK(OpenSiblingFile(callback, name, _dataStream))
    if (!_dataStream)
      return S_FALSE;
    version = 1;
  }
  else
    return S_FALSE;

  CByteBuffer index;
  if (ReadIndex(stream, indexOffset, fileSize, index) != S_OK)
    return S_FALSE;
  CPickleReader reader(index, index.Size());
  CValuePtr root;
  if (!reader.Read(root) || root->Type != CValue::kDict)
    return S_FALSE;

  UInt64 dataSize = fileSize;
  if (_dataStream)
    RINOK(GetStreamSize(_dataStream, dataSize))
  const UInt32 key32 = (UInt32)key;
  for (size_t i = 0; i + 1 < root->Items.size(); i += 2)
  {
    CItem item;
    if (!GetName(*root->Items[i], item.Path))
      return S_FALSE;
    const CValue &list = *root->Items[i + 1];
    if (list.Type != CValue::kList && list.Type != CValue::kTuple)
      return S_FALSE;
    for (size_t k = 0; k < list.Items.size(); k++)
    {
      const CValue &t = *list.Items[k];
      if (t.Type != CValue::kTuple && t.Type != CValue::kList)
        return S_FALSE;
      if (t.Items.size() < 2 || t.Items[0]->Type != CValue::kInt || t.Items[1]->Type != CValue::kInt)
        return S_FALSE;
      UInt64 offset = (UInt64)t.Items[0]->Int;
      UInt64 len = (UInt64)t.Items[1]->Int;
      if (version == 3)
      {
        offset ^= key32;
        len ^= key32;
      }
      if (t.Items.size() >= 3)
      {
        const CValue &prefix = *t.Items[2];
        if ((prefix.Type == CValue::kBytes || prefix.Type == CValue::kStr) && !prefix.Bytes.empty())
        {
          // Python 3 keeps it as bytes; a str prefix means its latin-1 bytes
          std::string b = prefix.Bytes;
          if (prefix.Type == CValue::kStr && !Utf8ToLatin1(prefix.Bytes, b))
            return S_FALSE;
          const size_t at = item.Inline.Size();
          item.Inline.ChangeSize_KeepData(at + b.size(), at);
          memcpy((Byte *)item.Inline + at, b.data(), b.size());
          item.AddPiece(at, b.size(), b.size(), kMethod_Inline);
        }
      }
      item.AddPiece(offset, len, len, kMethod_Stored);
      if (offset > dataSize || len > dataSize - offset)
        _errorFlags |= kpv_ErrorFlags_UnexpectedEnd;
    }
    _items.Add(item);
  }
  _phySize = fileSize;
  _comment = (version == 3) ? "RPA-3.0" : (version == 2) ? "RPA-2.0" : "RPA-1.0";
  return S_OK;
}

static const Byte k_Signature[] =
{
  8, 'R', 'P', 'A', '-', '3', '.', '0', ' ',
  8, 'R', 'P', 'A', '-', '2', '.', '0', ' '
};

REGISTER_ARC_I(
  "RPA", "rpa rpi", NULL, 0xAE,
  k_Signature,
  0,
  NArcInfoFlags::kMultiSignature,
  NULL)

}}

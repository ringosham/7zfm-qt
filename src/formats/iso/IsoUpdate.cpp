// IsoUpdate.cpp -- 7zfm-qt: adding, removing and renaming files in an
// existing ISO 9660 image.
//
// The image is changed the way a new multisession track would be: nothing
// already in it moves. The output is the input copied whole (with anything
// after the volume, such as an appended partition), then the new files'
// data, then new directory trees and path tables; last, the primary volume
// descriptor (and the Joliet one, if there is one) is rewritten in place to
// point at the new root and to cover the new volume size. The boot record,
// boot catalog and boot images stay where they were, and so do the sectors
// a boot info table or a hybrid MBR/GPT points at, so a bootable image
// stays bootable. A hybrid MBR partition that covered the whole volume is
// extended with it. Removed files' data stays in the image, unreferenced.
//
// The trees: the primary one with ISO 9660 names (8.3 upper case, as DOS
// needs, unless the image's own names were longer), plus Rock Ridge (NM,
// PX, TF, SL; SP and ER in the root) if the image had it, and a Joliet
// tree (UCS-2 names up to 64 characters) if the image had one. Files over
// 4 GiB - 2 KiB are split into extents (ISO 9660 level 3).
//
// Images with a UDF file system aren't changed: their ISO 9660 tree is
// often a stub, and the UDF tree would go stale.

#include "Common/Common.h"

#include "C/CpuArch.h"

#include "Common/MyLinux.h"
#include "Common/StringConvert.h"
#include "Common/UTFConvert.h"

#include "Windows/PropVariant.h"
#include "Windows/TimeUtils.h"

#include "7zip/Common/StreamUtils.h"

#include "IsoUpdate.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

namespace NArchive {
namespace NIso {

static const UInt32 kSector = kBlockSize;
static const UInt32 kMaxExtent = 0xFFFFF800;

const char *GetUpdateRefusal(const CInArchive &arc)
{
  if (!arc.IsArc || arc.VolDescs.IsEmpty())
    return "not an ISO 9660 image";
  if (arc.HasUdf)
    return "the image has a UDF file system, which would not be updated";
  for (unsigned i = 0; i < arc.VolDescs.Size(); i++)
    if (arc.VolDescs[i].LogicalBlockSize != kSector)
      return "the image's block size isn't 2048";
  if (arc.HeadersError || arc.UnexpectedEnd || arc.SelfLinkedDirs || arc.TooDeepDirs || arc.TooBigMetadata)
    return "the image's directories have errors";
  return NULL;
}

// ---------- helpers ----------

static void SetBe16_(Byte *p, UInt32 v) { p[0] = (Byte)(v >> 8); p[1] = (Byte)v; }
static void SetBoth16(Byte *p, UInt32 v) { SetUi16(p, (UInt16)v) SetBe16_(p + 2, v); }
static void SetBoth32(Byte *p, UInt32 v) { SetUi32(p, v) SetBe32(p + 4, v) }

static void TimeFromFiletime(UInt64 ft, Byte *t)
{
  const Int64 unixTime = (Int64)(ft / 10000000) - (Int64)11644473600;
  time_t tt = (time_t)(unixTime < 0 ? 0 : unixTime);
  struct tm g;
  memset(&g, 0, sizeof(g));
  gmtime_r(&tt, &g);
  int year = g.tm_year;
  if (year < 0) year = 0;
  if (year > 255) year = 255;
  t[0] = (Byte)year;
  t[1] = (Byte)(g.tm_mon + 1);
  t[2] = (Byte)g.tm_mday;
  t[3] = (Byte)g.tm_hour;
  t[4] = (Byte)g.tm_min;
  t[5] = (Byte)g.tm_sec;
  t[6] = 0;
}

static void TimeNow(Byte *t)
{
  const UInt64 now = ((UInt64)time(NULL) + (UInt64)11644473600) * 10000000;
  TimeFromFiletime(now, t);
}

// ---------- the tree ----------

struct CExtent
{
  UInt32 Loc;
  UInt32 Size;
};

struct CNode
{
  UString Name;
  bool IsDir;
  bool Known;           // given by the callback (not just implied by a path)
  bool NewData;
  int UpdateIndex;
  UInt64 Size;
  CRecordVector<CExtent> Extents;
  Byte Time[7];
  UInt32 Mode;
  Byte Hidden;
  AString OrigId;       // the image's own primary name, without ";1"
  AString SymLink;
  CByteBuffer RawSu;    // a kept, unrenamed item's own Rock Ridge entries
  bool Relocate;        // a kept file whose data is copied after the trees
  CRecordVector<CExtent> OldExtents;
  int Parent;
  CRecordVector<unsigned> Children;

  // names: [0] primary, [1] Joliet; the rest per view: [0] primary,
  // [1] Joliet, [2] and [3] the same in a hybrid image's partition
  CByteBuffer Id[2];
  UInt32 DirLoc[4];
  UInt32 DirSize[4];
  unsigned PathNum[4];

  CNode(): IsDir(false), Known(false), NewData(false), UpdateIndex(-1), Size(0), Mode(0), Hidden(0), Relocate(false), Parent(-1)
  {
    memset(Time, 0, sizeof(Time));
    for (unsigned i = 0; i < 4; i++)
    {
      DirLoc[i] = 0;
      DirSize[i] = 0;
      PathNum[i] = 0;
    }
  }
};

class CTree
{
public:
  CObjectVector<CNode> Nodes;
  CTree()
  {
    CNode &root = Nodes.AddNew();
    root.IsDir = true;
    root.Mode = MY_LIN_S_IFDIR | 0755;
    TimeNow(root.Time);
  }
  int Find(unsigned dir, const UString &name) const
  {
    const CNode &d = Nodes[dir];
    FOR_VECTOR (i, d.Children)
      if (Nodes[d.Children[i]].Name == name)
        return (int)d.Children[i];
    return -1;
  }
  // the node for (path), creating folders on the way
  unsigned Add(const UString &path, bool isDir)
  {
    UStringVector parts;
    UString cur;
    for (unsigned i = 0; i <= path.Len(); i++)
    {
      const wchar_t c = i < path.Len() ? path[i] : 0;
      if (c == 0 || c == WCHAR_PATH_SEPARATOR || c == L'/')
      {
        if (!cur.IsEmpty() && cur != L"." && cur != L"..")
          parts.Add(cur);
        cur.Empty();
      }
      else
        cur += c;
    }
    unsigned dir = 0;
    FOR_VECTOR (i, parts)
    {
      const bool last = (i + 1 == parts.Size());
      int n = Find(dir, parts[i]);
      if (n < 0)
      {
        CNode node;
        node.Name = parts[i];
        node.IsDir = last ? isDir : true;
        node.Mode = node.IsDir ? (MY_LIN_S_IFDIR | 0755) : (MY_LIN_S_IFREG | 0644);
        node.Parent = (int)dir;
        TimeNow(node.Time);
        n = (int)Nodes.Add(node);
        Nodes[dir].Children.Add((unsigned)n);
      }
      else if (last && Nodes[(unsigned)n].IsDir != isDir)
      {
        // a file replacing a folder of the same name, or the other way round
        CNode &node = Nodes[(unsigned)n];
        if (!node.Children.IsEmpty())
          return 0;
        node.IsDir = isDir;
      }
      dir = (unsigned)n;
    }
    return dir;
  }
};

// ---------- names ----------

static bool IsDChar(char c)
{
  return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// an ISO 9660 identifier for (name): d-characters, 8.3 (level 1) or 30 long
static AString MakeIsoName(const UString &name, bool isDir, bool level2, unsigned counter)
{
  AString base, ext;
  int dot = isDir ? -1 : name.ReverseFind(L'.');
  if (dot == 0)
    dot = -1;
  for (unsigned i = 0; i < name.Len(); i++)
  {
    wchar_t c = name[i];
    if ((int)i == dot)
      continue;
    if (c >= 'a' && c <= 'z')
      c = (wchar_t)(c - 'a' + 'A');
    const char a = (c < 0x80 && IsDChar((char)c)) ? (char)c : '_';
    if (dot >= 0 && (int)i > dot)
      ext += a;
    else
      base += a;
  }
  const unsigned maxExt = level2 ? 8 : 3;
  if (ext.Len() > maxExt)
    ext.DeleteFrom(maxExt);
  unsigned maxBase = level2 ? (isDir ? 31 : 30 - (ext.IsEmpty() ? 0 : ext.Len() + 1)) : 8;
  if (base.IsEmpty())
    base = "_";
  if (counter != 0)
  {
    char num[16];
    snprintf(num, sizeof(num), "%u", counter);
    const unsigned n = (unsigned)strlen(num);
    if (base.Len() + n > maxBase)
      base.DeleteFrom(maxBase > n ? maxBase - n : 0);
    base += num;
  }
  if (base.Len() > maxBase)
    base.DeleteFrom(maxBase);
  AString s = base;
  if (!ext.IsEmpty())
  {
    s.Add_Dot();
    s += ext;
  }
  return s;
}

static bool IsLevel1Name(const AString &s, bool isDir)
{
  const int dot = s.Find('.');
  const unsigned baseLen = dot < 0 ? s.Len() : (unsigned)dot;
  const unsigned extLen = dot < 0 ? 0 : s.Len() - (unsigned)dot - 1;
  if (baseLen == 0 || baseLen > 8 || extLen > 3 || (isDir && dot >= 0))
    return false;
  for (unsigned i = 0; i < s.Len(); i++)
    if ((int)i != dot && !IsDChar(s[i]))
      return false;
  return true;
}

// the Joliet name: UCS-2, at most 64 characters, without * / : ; ? '\'
static UString MakeJolietName(const UString &name, unsigned counter)
{
  UString s;
  for (unsigned i = 0; i < name.Len(); i++)
  {
    wchar_t c = name[i];
    if (c < 0x20 || c == '*' || c == '/' || c == ':' || c == ';' || c == '?' || c == '\\' || (UInt32)c > 0xFFFF)
      c = L'_';
    s += c;
  }
  UString ext;
  const int dot = s.ReverseFind(L'.');
  if (dot > 0 && s.Len() - (unsigned)dot <= 10)
  {
    ext = s.Ptr((unsigned)dot);
    s.DeleteFrom((unsigned)dot);
  }
  if (counter != 0)
  {
    s += L'_';
    s.Add_UInt32(counter);
  }
  const unsigned maxBase = 64 - ext.Len();
  if (s.Len() > maxBase)
  {
    UString tail;
    if (counter != 0)
    {
      tail = L"_";
      tail.Add_UInt32(counter);
    }
    s.DeleteFrom(maxBase - tail.Len());
    s += tail;
  }
  return s + ext;
}

static void SetUcs2(CByteBuffer &b, const UString &s)
{
  b.Alloc(s.Len() * 2);
  for (unsigned i = 0; i < s.Len(); i++)
  {
    b[i * 2] = (Byte)(s[i] >> 8);
    b[i * 2 + 1] = (Byte)s[i];
  }
}

static void SetAscii(CByteBuffer &b, const AString &s)
{
  b.CopyFrom((const Byte *)s.Ptr(), s.Len());
}

// ISO 9660's order: the name, then the extension, then the version, each
// as if padded with spaces
static int CompareIsoIds(const CByteBuffer &a, const CByteBuffer &b)
{
  struct CParts { AString Name, Ext, Ver; };
  CParts p[2];
  const CByteBuffer *src[2] = { &a, &b };
  for (unsigned k = 0; k < 2; k++)
  {
    AString s;
    s.SetFrom((const char *)(const Byte *)*src[k], (unsigned)src[k]->Size());
    const int semi = s.Find(';');
    if (semi >= 0)
    {
      p[k].Ver = s.Ptr((unsigned)semi + 1);
      s.DeleteFrom((unsigned)semi);
    }
    const int dot = s.Find('.');
    if (dot >= 0)
    {
      p[k].Ext = s.Ptr((unsigned)dot + 1);
      s.DeleteFrom((unsigned)dot);
    }
    p[k].Name = s;
  }
  const AString *fields[3][2] = { { &p[0].Name, &p[1].Name }, { &p[0].Ext, &p[1].Ext }, { &p[0].Ver, &p[1].Ver } };
  for (unsigned f = 0; f < 3; f++)
  {
    const AString &x = *fields[f][0], &y = *fields[f][1];
    const unsigned n = MyMax(x.Len(), y.Len());
    for (unsigned i = 0; i < n; i++)
    {
      const Byte cx = i < x.Len() ? (Byte)x[i] : (Byte)' ';
      const Byte cy = i < y.Len() ? (Byte)y[i] : (Byte)' ';
      if (cx != cy)
        return cx < cy ? -1 : 1;
    }
  }
  return 0;
}

static int CompareBytes(const CByteBuffer &a, const CByteBuffer &b)
{
  const size_t n = MyMin(a.Size(), b.Size());
  const int c = n ? memcmp(a, b, n) : 0;
  if (c != 0)
    return c;
  return a.Size() < b.Size() ? -1 : (a.Size() > b.Size() ? 1 : 0);
}

// ---------- directory records ----------

// a dir record: (id) and the System Use field (su); (cont) gets what
// doesn't fit, behind a CE entry that SetCe() fills in later
struct CRecord
{
  CByteBuffer Data;
  size_t CeOffset;      // of the CE entry in Data, or 0
  unsigned CeArea;      // index into the continuation areas
  CRecord(): CeOffset(0), CeArea(0) {}
};

static const unsigned kCeLen = 28;

struct CSusp
{
  CObjectVector<CByteBuffer> Entries;
  void Add(const char *sig, const Byte *data, size_t size)
  {
    CByteBuffer &b = Entries.AddNew();
    b.Alloc(4 + size);
    b[0] = (Byte)sig[0];
    b[1] = (Byte)sig[1];
    b[2] = (Byte)(4 + size);
    b[3] = 1;
    if (size)
      memcpy(b + 4, data, size);
  }
};

static void AddPx(CSusp &s, UInt32 mode, UInt32 links)
{
  Byte d[32];
  SetBoth32(d, mode);
  SetBoth32(d + 8, links);
  SetBoth32(d + 16, 0);
  SetBoth32(d + 24, 0);
  s.Add("PX", d, 32);
}

static void AddTf(CSusp &s, const Byte *time)
{
  Byte d[8];
  d[0] = 0x02;   // modify
  memcpy(d + 1, time, 7);
  s.Add("TF", d, 8);
}

static void AddNm(CSusp &s, const UString &name)
{
  AString utf;
  ConvertUnicodeToUTF8(name, utf);
  unsigned pos = 0;
  do
  {
    const unsigned n = MyMin(utf.Len() - pos, 240u);
    Byte d[1 + 240];
    d[0] = (Byte)(pos + n < utf.Len() ? 1 : 0);   // CONTINUE
    memcpy(d + 1, utf.Ptr() + pos, n);
    s.Add("NM", d, 1 + n);
    pos += n;
  }
  while (pos < utf.Len());
}

static bool AddSl(CSusp &s, const AString &target)
{
  CByteBuffer comps;
  Byte buf[512];
  size_t len = 0;
  unsigned pos = 0;
  if (!target.IsEmpty() && target[0] == '/')
  {
    buf[len++] = 8;   // root
    buf[len++] = 0;
    pos = 1;
  }
  while (pos <= target.Len())
  {
    int slash = target.Find('/', pos);
    const unsigned end = slash < 0 ? target.Len() : (unsigned)slash;
    const unsigned n = end - pos;
    if (len + 2 + n > 240)
      return false;
    if (n == 1 && target[pos] == '.')
      { buf[len++] = 2; buf[len++] = 0; }
    else if (n == 2 && target[pos] == '.' && target[pos + 1] == '.')
      { buf[len++] = 4; buf[len++] = 0; }
    else if (n != 0)
    {
      buf[len++] = 0;
      buf[len++] = (Byte)n;
      memcpy(buf + len, target.Ptr() + pos, n);
      len += n;
    }
    if (slash < 0)
      break;
    pos = end + 1;
  }
  Byte d[1 + 240];
  d[0] = 0;
  memcpy(d + 1, buf, len);
  s.Add("SL", d, 1 + len);
  return true;
}

static const char * const kErId = "RRIP_1991A";
static const char * const kErDes = "THE ROCK RIDGE INTERCHANGE PROTOCOL PROVIDES SUPPORT FOR POSIX FILE SYSTEM SEMANTICS";
static const char * const kErSrc = "PLEASE CONTACT DISC PUBLISHER FOR SPECIFICATION SOURCE.  SEE PUBLISHER IDENTIFIER IN PRIMARY VOLUME DESCRIPTOR FOR CONTACT INFORMATION.";

static void AddEr(CSusp &s)
{
  const size_t li = strlen(kErId), ld = strlen(kErDes), ls = strlen(kErSrc);
  CByteBuffer d(4 + li + ld + ls);
  d[0] = (Byte)li;
  d[1] = (Byte)ld;
  d[2] = (Byte)ls;
  d[3] = 1;
  memcpy(d + 4, kErId, li);
  memcpy(d + 4 + li, kErDes, ld);
  memcpy(d + 4 + li + ld, kErSrc, ls);
  s.Add("ER", d, d.Size());
}

class CWriter
{
public:
  CTree &Tree;
  bool RockRidge;
  bool Joliet;
  CObjectVector<CByteBuffer> CeAreas;   // continuation areas, in order
  CRecordVector<UInt32> CeLoc, CeOffset;
  UInt32 Bias;    // the view's start: addresses are from there

  CWriter(CTree &tree, bool rr, bool joliet): Tree(tree), RockRidge(rr), Joliet(joliet), Bias(0) {}

  // (forceCe): put everything after the first entries in a continuation
  // area (the root's ER)
  void MakeRecord(CRecord &r, UInt32 extent, UInt32 size, const Byte *time, Byte flags,
      const Byte *id, unsigned idLen, CSusp *su)
  {
    const unsigned pad = (idLen & 1) ? 0 : 1;
    const unsigned base = 33 + idLen + pad;
    // what fits in the record; the rest goes to a continuation area
    unsigned mainLen = 0, n = 0;
    size_t total = 0;
    if (su)
      FOR_VECTOR (i, su->Entries)
        total += su->Entries[i].Size();
    bool needCe = su && base + total > 255;
    if (su)
    {
      const unsigned room = needCe ? 255 - base - kCeLen : 255 - base;
      for (; n < su->Entries.Size(); n++)
      {
        const unsigned l = (unsigned)su->Entries[n].Size();
        if (mainLen + l > room)
        {
          // a long Rock Ridge name: its start stays in the record (readers
          // that don't follow CE see that much), the rest continues there
          const CByteBuffer &e = su->Entries[n];
          if (needCe && e[0] == 'N' && e[1] == 'M' && room - mainLen >= 6)
          {
            const unsigned firstLen = room - mainLen;
            const unsigned k = firstLen - 5;
            CByteBuffer first(firstLen), rest(l - k);
            first[0] = 'N'; first[1] = 'M'; first[2] = (Byte)firstLen; first[3] = 1;
            first[4] = (Byte)(e[4] | 1);   // CONTINUE
            memcpy(first + 5, e + 5, k);
            rest[0] = 'N'; rest[1] = 'M'; rest[2] = (Byte)(l - k); rest[3] = 1;
            rest[4] = e[4];
            memcpy(rest + 5, e + 5 + k, l - 5 - k);
            su->Entries[n] = first;
            su->Entries.Insert(n + 1, rest);
            mainLen += firstLen;
            n++;
          }
          break;
        }
        mainLen += l;
      }
      if (n == su->Entries.Size())
        needCe = false;
    }
    const unsigned len = base + mainLen + (needCe ? kCeLen : 0);
    r.Data.Alloc(len);
    Byte *p = r.Data;
    memset(p, 0, len);
    p[0] = (Byte)len;
    SetBoth32(p + 2, extent);
    SetBoth32(p + 10, size);
    memcpy(p + 18, time, 7);
    p[25] = flags;
    SetBoth16(p + 28, 1);
    p[32] = (Byte)idLen;
    if (idLen)
      memcpy(p + 33, id, idLen);
    size_t pos = base;
    for (unsigned i = 0; i < n; i++)
    {
      memcpy(p + pos, su->Entries[i], su->Entries[i].Size());
      pos += su->Entries[i].Size();
    }
    r.CeOffset = 0;
    if (needCe)
    {
      CByteBuffer &area = CeAreas.AddNew();
      size_t areaLen = 0;
      for (unsigned i = n; i < su->Entries.Size(); i++)
        areaLen += su->Entries[i].Size();
      area.Alloc(areaLen);
      size_t ap = 0;
      for (unsigned i = n; i < su->Entries.Size(); i++)
      {
        memcpy(area + ap, su->Entries[i], su->Entries[i].Size());
        ap += su->Entries[i].Size();
      }
      r.CeOffset = pos;
      r.CeArea = CeAreas.Size() - 1;
      p[pos] = 'C'; p[pos + 1] = 'E'; p[pos + 2] = kCeLen; p[pos + 3] = 1;
    }
  }

  void SetCe(CRecord &r) const
  {
    if (!r.CeOffset)
      return;
    Byte *p = r.Data + r.CeOffset;
    SetBoth32(p + 4, CeLoc[r.CeArea]);
    SetBoth32(p + 12, CeOffset[r.CeArea]);
    SetBoth32(p + 20, (UInt32)CeAreas[r.CeArea].Size());
  }

  // places the continuation areas from sector (loc), none crossing a sector
  UInt32 PlaceCe(UInt32 loc)
  {
    CeLoc.Clear();
    CeOffset.Clear();
    if (CeAreas.IsEmpty())
      return 0;
    UInt32 sector = 0, off = 0;
    FOR_VECTOR (i, CeAreas)
    {
      const UInt32 n = (UInt32)CeAreas[i].Size();
      if (off + n > kSector)
      {
        sector++;
        off = 0;
      }
      CeLoc.Add(loc + sector);
      CeOffset.Add(off);
      off += n;
    }
    return sector + 1;
  }

  void Su(CSusp &su, const CNode &node, bool withName)
  {
    if (withName)
      AddNm(su, node.Name);
    AddPx(su, node.Mode, node.IsDir ? 2 : 1);
    AddTf(su, node.Time);
    if (!node.SymLink.IsEmpty())
      AddSl(su, node.SymLink);
  }

  // the records of directory (dir) in view (t)
  void DirRecords(unsigned t, unsigned dir, CObjectVector<CRecord> &recs)
  {
    const CNode &d = Tree.Nodes[dir];
    const CNode &parent = Tree.Nodes[d.Parent < 0 ? dir : (unsigned)d.Parent];
    const bool rr = ((t & 1) == 0 && RockRidge);
    {
      CSusp su;
      if (rr)
      {
        if (dir == 0)
        {
          Byte sp[3] = { 0xBE, 0xEF, 0 };
          su.Add("SP", sp, 3);
        }
        Su(su, d, false);
        if (dir == 0)
          AddEr(su);
      }
      const Byte id = 0;
      MakeRecord(recs.AddNew(), d.DirLoc[t], d.DirSize[t], d.Time, 2, &id, 1, rr ? &su : NULL);
    }
    {
      CSusp su;
      if (rr)
        Su(su, parent, false);
      const Byte id = 1;
      MakeRecord(recs.AddNew(), parent.DirLoc[t], parent.DirSize[t], parent.Time, 2, &id, 1, rr ? &su : NULL);
    }
    FOR_VECTOR (i, d.Children)
    {
      const CNode &c = Tree.Nodes[d.Children[i]];
      CSusp su;
      // a kept item's own entries, as they were, if they still fit: its
      // continuation area (if any) stays where the image had it, before its
      // data, and nothing it had is lost
      const unsigned idLen0 = (unsigned)c.Id[t & 1].Size();
      if (rr && c.RawSu.Size() != 0 && 33 + idLen0 + ((idLen0 & 1) ? 0 : 1) + c.RawSu.Size() <= 255)
        su.Entries.Add(c.RawSu);
      else if (rr)
        Su(su, c, true);
      const CByteBuffer &id = c.Id[t & 1];
      if (c.IsDir)
      {
        MakeRecord(recs.AddNew(), c.DirLoc[t], c.DirSize[t], c.Time, (Byte)(2 | c.Hidden), id, (unsigned)id.Size(), rr ? &su : NULL);
        continue;
      }
      if (c.Extents.IsEmpty())
      {
        MakeRecord(recs.AddNew(), 0, 0, c.Time, c.Hidden, id, (unsigned)id.Size(), rr ? &su : NULL);
        continue;
      }
      FOR_VECTOR (k, c.Extents)
      {
        const bool last = (k + 1 == c.Extents.Size());
        // every extent's record has the Rock Ridge entries (as mkisofs
        // writes them; libarchive wants some in each record)
        CSusp suK = su;
        MakeRecord(recs.AddNew(), c.Extents[k].Loc - Bias, c.Extents[k].Size, c.Time,
            (Byte)(c.Hidden | (last ? 0 : 0x80)), id, (unsigned)id.Size(), rr ? &suK : NULL);
      }
    }
  }

  static UInt32 LayoutSize(const CObjectVector<CRecord> &recs)
  {
    UInt32 pos = 0;
    FOR_VECTOR (i, recs)
    {
      const UInt32 len = (UInt32)recs[i].Data.Size();
      if ((pos % kSector) + len > kSector)
        pos = (pos + kSector - 1) / kSector * kSector;
      pos += len;
    }
    return (pos + kSector - 1) / kSector * kSector;
  }

  static void Layout(const CObjectVector<CRecord> &recs, CByteBuffer &out)
  {
    const UInt32 size = LayoutSize(recs);
    out.Alloc(size);
    memset(out, 0, size);
    UInt32 pos = 0;
    FOR_VECTOR (i, recs)
    {
      const UInt32 len = (UInt32)recs[i].Data.Size();
      if ((pos % kSector) + len > kSector)
        pos = (pos + kSector - 1) / kSector * kSector;
      memcpy(out + pos, recs[i].Data, len);
      pos += len;
    }
  }
};

// ---------- the update ----------

static HRESULT WriteZeros(ISequentialOutStream *out, UInt64 n)
{
  Byte zeros[kSector];
  memset(zeros, 0, sizeof(zeros));
  while (n != 0)
  {
    const UInt32 k = (UInt32)MyMin(n, (UInt64)kSector);
    RINOK(WriteStream(out, zeros, k))
    n -= k;
  }
  return S_OK;
}

static HRESULT GetString(IArchiveUpdateCallback *cb, UInt32 index, PROPID id, UString &s, bool &defined)
{
  NWindows::NCOM::CPropVariant prop;
  RINOK(cb->GetProperty(index, id, &prop))
  defined = false;
  s.Empty();
  if (prop.vt == VT_BSTR)
  {
    s = prop.bstrVal;
    defined = true;
  }
  else if (prop.vt != VT_EMPTY)
    return E_INVALIDARG;
  return S_OK;
}

HRESULT UpdateIso(IInStream *inStream, const CInArchive &arc, IInArchive *handler,
    ISequentialOutStream *outSeq, UInt32 numItems, IArchiveUpdateCallback *callback)
{
  if (GetUpdateRefusal(arc))
    return E_NOTIMPL;
  CMyComPtr<IOutStream> out;
  outSeq->QueryInterface(IID_IOutStream, (void **)&out);
  if (!out)
    return E_NOTIMPL;

  // the volume descriptors to rewrite: the primary and any Joliet ones
  CRecordVector<UInt32> pvdSectors, jolietSectors;
  for (UInt32 s = 16; s < 16 + 64; s++)
  {
    Byte b[kSector];
    RINOK(InStream_SeekSet(inStream, (UInt64)s * kSector))
    RINOK(ReadStream_FALSE(inStream, b, kSector))
    if (memcmp(b + 1, "CD001", 5) != 0)
      break;
    if (b[0] == 255)
      break;
    if (b[0] == 1)
      pvdSectors.Add(s);
    else if (b[0] == 2 && (b[7] & 1) == 0 && b[88] == 0x25 && b[89] == 0x2F
        && (b[90] == 0x40 || b[90] == 0x43 || b[90] == 0x45))
      jolietSectors.Add(s);
  }
  if (pvdSectors.IsEmpty())
    return E_NOTIMPL;
  const bool mainIsPrimary = !arc.IsJoliet();
  const bool rockRidge = mainIsPrimary && arc.IsSusp;
  const bool joliet = !jolietSectors.IsEmpty();

  // the image's own primary names: longer than 8.3 means level 2 names are fine
  bool level2 = false;
  if (mainIsPrimary)
    FOR_VECTOR (i, arc.Refs)
    {
      const CRef &ref = arc.Refs[i];
      const CDir &item = ref.Dir->_subItems[ref.Index];
      AString id;
      id.SetFrom((const char *)(const Byte *)item.FileId, (unsigned)item.FileId.Size());
      const int semi = id.Find(';');
      if (semi >= 0)
        id.DeleteFrom((unsigned)semi);
      if (!IsLevel1Name(id, item.IsDir()))
        level2 = true;
    }

  // ---------- the items ----------
  CTree tree;
  UInt64 newDataTotal = 0;
  for (UInt32 i = 0; i < numItems; i++)
  {
    Int32 newData = 0, newProps = 0;
    UInt32 indexInArc = (UInt32)(Int32)-1;
    RINOK(callback->GetUpdateItemInfo(i, &newData, &newProps, &indexInArc))
    const bool fromArc = (indexInArc != (UInt32)(Int32)-1);
    if (fromArc && indexInArc >= arc.Refs.Size())
      continue;   // the [BOOT] items: the boot record stays as it is
    if (!fromArc && !newData)
      continue;

    UString path;
    bool isDir = false;
    UInt64 mtime = 0;
    bool mtimeDefined = false;
    UInt32 mode = 0;
    bool modeDefined = false;
    AString symLink;
    const CDir *item = NULL;
    if (fromArc)
    {
      const CRef &ref = arc.Refs[indexInArc];
      item = &ref.Dir->_subItems[ref.Index];
    }

    if (newProps)
    {
      bool defined;
      RINOK(GetString(callback, i, kpidPath, path, defined))
      NWindows::NCOM::CPropVariant prop;
      RINOK(callback->GetProperty(i, kpidIsDir, &prop))
      if (prop.vt == VT_BOOL)
        isDir = (prop.boolVal != VARIANT_FALSE);
      else if (prop.vt != VT_EMPTY)
        return E_INVALIDARG;
      prop.Clear();
      RINOK(callback->GetProperty(i, kpidMTime, &prop))
      if (prop.vt == VT_FILETIME)
      {
        mtime = ((UInt64)prop.filetime.dwHighDateTime << 32) | prop.filetime.dwLowDateTime;
        mtimeDefined = true;
      }
      prop.Clear();
      RINOK(callback->GetProperty(i, kpidPosixAttrib, &prop))
      if (prop.vt == VT_UI4)
      {
        mode = prop.ulVal;
        modeDefined = true;
      }
      else
      {
        prop.Clear();
        RINOK(callback->GetProperty(i, kpidAttrib, &prop))
        if (prop.vt == VT_UI4 && (prop.ulVal & 0x8000) != 0)   // FILE_ATTRIBUTE_UNIX_EXTENSION
        {
          mode = prop.ulVal >> 16;
          modeDefined = true;
        }
      }
      if (newData && !isDir)
      {
        UString link;
        RINOK(GetString(callback, i, kpidSymLink, link, defined))
        if (!link.IsEmpty())
          ConvertUnicodeToUTF8(link, symLink);
      }
    }
    else
    {
      NWindows::NCOM::CPropVariant prop;
      RINOK(handler->GetProperty(indexInArc, kpidPath, &prop))
      if (prop.vt == VT_BSTR)
        path = prop.bstrVal;
      isDir = item->IsDir();
    }
    if (fromArc && !newData)
    {
      // the kept item's data, mode and the like
      if (!modeDefined && arc.IsSusp && item->GetPx(arc.SuspSkipSize, k_Px_Mode, mode))
        modeDefined = true;
      if (arc.IsSusp)
        item->GetSymLink(arc.SuspSkipSize, symLink);
      if (!MY_LIN_S_ISLNK(mode))
        symLink.Empty();
    }
    if (path.IsEmpty())
      continue;

    const unsigned n = tree.Add(path, isDir);
    if (n == 0)
      return E_INVALIDARG;
    CNode &node = tree.Nodes[n];
    node.Known = true;
    node.IsDir = isDir;
    node.UpdateIndex = (int)i;
    node.NewData = newData != 0 && !isDir;
    node.SymLink = symLink;
    if (modeDefined)
      node.Mode = mode;
    if ((node.Mode & MY_LIN_S_IFMT) == 0 || (isDir != MY_LIN_S_ISDIR(node.Mode)))
      node.Mode = (node.Mode & 07777) | (isDir ? MY_LIN_S_IFDIR : (symLink.IsEmpty() ? MY_LIN_S_IFREG : MY_LIN_S_IFLNK));
    if (mtimeDefined)
      TimeFromFiletime(mtime, node.Time);
    else if (item)
    {
      const CRecordingDateTime &t = item->DateTime;
      node.Time[0] = t.Year; node.Time[1] = t.Month; node.Time[2] = t.Day;
      node.Time[3] = t.Hour; node.Time[4] = t.Minute; node.Time[5] = t.Second;
      node.Time[6] = (Byte)t.GmtOffset;
    }
    node.Extents.Clear();
    node.Size = 0;
    // a kept item's own Rock Ridge entries, unless they continue elsewhere
    // (a CE would point before the new directories)
    node.RawSu.Free();
    if (item && rockRidge && !newProps && arc.SuspSkipSize == 0)
    {
      const Byte *p = item->SystemUse;
      const size_t size = item->SystemUse.Size();
      bool ok = size != 0;
      for (size_t k = 0; ok && k + 4 <= size;)
      {
        const unsigned len = p[k + 2];
        // (IsoIn.cpp marks a CE it followed 'cE' and appends the area)
        if (len < 4 || k + len > size || ((p[k] == 'C' || p[k] == 'c') && p[k + 1] == 'E'))
          ok = false;
        k += len ? len : size;
      }
      if (ok)
        node.RawSu = item->SystemUse;
    }
    if (item)
    {
      node.Hidden = (Byte)(item->FileFlags & 1);
      if (mainIsPrimary && !newProps)
      {
        AString id;
        id.SetFrom((const char *)(const Byte *)item->FileId, (unsigned)item->FileId.Size());
        const int semi = id.Find(';');
        if (semi >= 0)
          id.DeleteFrom((unsigned)semi);
        if (!id.IsEmpty() && id.Back() == '.')
          id.DeleteBack();
        node.OrigId = id;
      }
    }
    if (node.NewData)
    {
      // the size the data is laid out for; the stream must have it
      NWindows::NCOM::CPropVariant prop;
      RINOK(callback->GetProperty(i, kpidSize, &prop))
      if (prop.vt == VT_UI8)
        node.Size = prop.uhVal.QuadPart;
      else if (prop.vt != VT_EMPTY)
        return E_INVALIDARG;
      if (!node.SymLink.IsEmpty())
        node.Size = 0;
      newDataTotal += node.Size;
    }
    else if (item && !isDir)
    {
      const CRef &ref = arc.Refs[indexInArc];
      for (UInt32 k = 0; k < ref.NumExtents; k++)
      {
        const CDir &e = ref.Dir->_subItems[ref.Index + k];
        CExtent ext;
        ext.Loc = e.ExtentLocation;
        ext.Size = e.Size;
        if (ext.Size != 0)
          node.Extents.Add(ext);
        node.Size += e.Size;
      }
    }
  }

  // ---------- the old image ----------
  UInt64 inSize = 0;
  RINOK(InStream_GetSize_SeekToEnd(inStream, inSize))
  const UInt64 copyEnd = (inSize + kSector - 1) / kSector * kSector;
  if (copyEnd / kSector + newDataTotal / kSector > 0xFFFFFF00)
    return E_FAIL;
  RINOK(callback->SetTotal(inSize + newDataTotal))
  RINOK(InStream_SeekSet(inStream, 0))
  RINOK(out->Seek(0, STREAM_SEEK_SET, NULL))
  UInt64 completed = 0;
  {
    CByteBuffer buf(1 << 20);
    UInt64 left = inSize;
    while (left != 0)
    {
      const size_t n = (size_t)MyMin(left, (UInt64)buf.Size());
      RINOK(ReadStream_FALSE(inStream, buf, n))
      RINOK(WriteStream(out, buf, n))
      left -= n;
      completed += n;
      RINOK(callback->SetCompleted(&completed))
    }
    RINOK(WriteZeros(out, copyEnd - inSize))
  }
  const UInt64 pos = copyEnd;

  // ---------- names and order ----------
  FOR_VECTOR (ni, tree.Nodes)
  {
    CNode &d = tree.Nodes[ni];
    if (!d.IsDir)
      continue;
    // by long name first, for stable generated names
    d.Children.Sort([](const unsigned *a, const unsigned *b, void *p) -> int
      {
        const CTree &t = *(const CTree *)p;
        return MyStringCompare(t.Nodes[*a].Name, t.Nodes[*b].Name);
      }, &tree);
    AStringVector usedIso;
    UStringVector usedJoliet;
    FOR_VECTOR (k, d.Children)
    {
      CNode &c = tree.Nodes[d.Children[k]];
      AString iso;
      bool found = false;
      if (!c.OrigId.IsEmpty() && c.OrigId.Len() <= 31 && (level2 || IsLevel1Name(c.OrigId, c.IsDir)))
      {
        iso = c.OrigId;
        found = true;
        FOR_VECTOR (u, usedIso)
          if (usedIso[u].IsEqualTo_Ascii_NoCase(iso))
            found = false;
      }
      for (unsigned counter = 0; !found; counter++)
      {
        iso = MakeIsoName(c.Name, c.IsDir, level2, counter);
        found = true;
        FOR_VECTOR (u, usedIso)
          if (usedIso[u].IsEqualTo_Ascii_NoCase(iso))
            found = false;
      }
      usedIso.Add(iso);
      if (!c.IsDir)
        iso += ";1";
      SetAscii(c.Id[0], iso);
      if (joliet)
      {
        UString j;
        for (unsigned counter = 0;; counter++)
        {
          j = MakeJolietName(c.Name, counter);
          bool unique = true;
          FOR_VECTOR (u, usedJoliet)
            if (usedJoliet[u].IsEqualTo_NoCase(j))
              unique = false;
          if (unique)
            break;
        }
        usedJoliet.Add(j);
        SetUcs2(c.Id[1], j);
      }
    }
  }

  // ---------- kept files that move ----------
  // A kept file whose Rock Ridge entries need a continuation area gets its
  // data copied after the trees: readers that go through an image in
  // order (libarchive) want the continuation after the directory and
  // before the data, and the kept data is before the new directories.
  if (rockRidge)
    FOR_VECTOR (ni, tree.Nodes)
    {
      CNode &node = tree.Nodes[ni];
      if (node.IsDir || node.NewData || node.Extents.IsEmpty() || node.RawSu.Size() != 0)
        continue;
      CSusp su;
      AddNm(su, node.Name);
      AddPx(su, node.Mode, 1);
      AddTf(su, node.Time);
      if (!node.SymLink.IsEmpty())
        AddSl(su, node.SymLink);
      size_t total = 0;
      FOR_VECTOR (k, su.Entries)
        total += su.Entries[k].Size();
      const unsigned idLen = (unsigned)node.Id[0].Size();
      if (33 + idLen + ((idLen & 1) ? 0 : 1) + total <= 255)
        continue;
      node.Relocate = true;
      node.NewData = true;
      node.OldExtents = node.Extents;
      newDataTotal += node.Size;
    }
  RINOK(callback->SetTotal(inSize + newDataTotal))

  // the new files' extents, each up to 4 GiB - 2 KiB; placed after the trees
  FOR_VECTOR (ni, tree.Nodes)
  {
    CNode &node = tree.Nodes[ni];
    if (!node.NewData)
      continue;
    node.Extents.Clear();
    UInt64 left = node.Size;
    while (left != 0)
    {
      CExtent e;
      e.Loc = 0;
      e.Size = (UInt32)MyMin(left, (UInt64)kMaxExtent);
      node.Extents.Add(e);
      left -= e.Size;
    }
  }

  // ---------- views ----------
  // a hybrid image made with a partition offset (xorriso -partition_offset)
  // has a second set of volume descriptors and trees at the partition's
  // start, its addresses counted from there: what a USB stick's partition
  // shows. It gets its own new trees.
  UInt32 partStart = 0;
  CRecordVector<UInt32> partPvd, partJoliet;
  {
    Byte mbr[512];
    RINOK(InStream_SeekSet(inStream, 0))
    if (ReadStream_FALSE(inStream, mbr, 512) == S_OK && mbr[510] == 0x55 && mbr[511] == 0xAA)
      for (unsigned k = 0; k < 4 && partStart == 0; k++)
      {
        const Byte *e = mbr + 446 + k * 16;
        const UInt32 start = GetUi32(e + 8);
        if (e[4] == 0 || start == 0 || (start & 3) != 0 || start / 4 + 16 >= inSize / kSector)
          continue;
        const UInt32 off = start / 4;
        for (UInt32 sct = off + 16; sct < off + 16 + 64; sct++)
        {
          Byte b[kSector];
          RINOK(InStream_SeekSet(inStream, (UInt64)sct * kSector))
          if (ReadStream_FALSE(inStream, b, kSector) != S_OK || memcmp(b + 1, "CD001", 5) != 0 || b[0] == 255)
            break;
          if (b[0] == 1)
            partPvd.Add(sct);
          else if (b[0] == 2 && (b[7] & 1) == 0 && b[88] == 0x25 && b[89] == 0x2F
              && (b[90] == 0x40 || b[90] == 0x43 || b[90] == 0x45))
            partJoliet.Add(sct);
        }
        if (!partPvd.IsEmpty())
          partStart = off;
        else
          partJoliet.Clear();
      }
  }

  // ---------- the trees: dirs breadth first (the path table's order) ----------
  // The layout, as mkisofs makes it: the trees with their continuation
  // areas and path tables, then the new files' data (readers that go
  // through an image in order, like libarchive, want a file's Rock Ridge
  // continuation before its data). The records' sizes don't depend on
  // where anything is, so everything is placed first and written after.
  CWriter writer(tree, rockRidge, joliet);
  struct CTreeOut
  {
    CRecordVector<unsigned> Dirs;   // breadth first
    UInt32 PathTableSize;
    UInt32 LPath, MPath;
    CRecordVector<UInt32> CeLoc, CeOffset;
  };
  CTreeOut trees[4];
  bool used[4];
  used[0] = true;
  used[1] = joliet;
  used[2] = partStart != 0;
  used[3] = partStart != 0 && joliet && !partJoliet.IsEmpty();
  UInt32 sector = (UInt32)(pos / kSector);
  CObjectVector<CObjectVector<CRecordVector<unsigned> > > orders;
  for (unsigned t = 0; t < 4; t++)
  {
    CObjectVector<CRecordVector<unsigned> > &order = orders.AddNew();
    if (!used[t])
      continue;
    const unsigned nt = t & 1;
    const UInt32 bias = t >= 2 ? partStart : 0;
    writer.Bias = bias;
    // children in this tree's order
    FOR_VECTOR (ni, tree.Nodes)
    {
      CRecordVector<unsigned> &o = order.AddNew();
      o = tree.Nodes[ni].Children;
      struct CCtx { const CTree *Tree; unsigned T; } ctx = { &tree, nt };
      o.Sort([](const unsigned *a, const unsigned *b, void *p) -> int
        {
          const CCtx &c = *(const CCtx *)p;
          const CByteBuffer &x = c.Tree->Nodes[*a].Id[c.T], &y = c.Tree->Nodes[*b].Id[c.T];
          return c.T == 0 ? CompareIsoIds(x, y) : CompareBytes(x, y);
        }, &ctx);
    }
    CRecordVector<unsigned> &dirs = trees[t].Dirs;
    dirs.Add(0);
    for (unsigned k = 0; k < dirs.Size(); k++)
    {
      const CRecordVector<unsigned> &o = order[dirs[k]];
      FOR_VECTOR (m, o)
        if (tree.Nodes[o[m]].IsDir)
          dirs.Add(o[m]);
    }
    FOR_VECTOR (ni, tree.Nodes)
      tree.Nodes[ni].Children = order[ni];
    FOR_VECTOR (k, dirs)
      tree.Nodes[dirs[k]].PathNum[t] = k + 1;

    writer.CeAreas.Clear();
    FOR_VECTOR (k, dirs)
    {
      CObjectVector<CRecord> recs;
      writer.DirRecords(t, dirs[k], recs);
      tree.Nodes[dirs[k]].DirSize[t] = CWriter::LayoutSize(recs);
    }
    FOR_VECTOR (k, dirs)
    {
      tree.Nodes[dirs[k]].DirLoc[t] = sector - bias;
      sector += tree.Nodes[dirs[k]].DirSize[t] / kSector;
    }
    // the continuation areas: the same ones as the records made above
    sector += writer.PlaceCe(sector - bias);
    trees[t].CeLoc = writer.CeLoc;
    trees[t].CeOffset = writer.CeOffset;
    UInt32 ptSize = 0;
    FOR_VECTOR (k, dirs)
    {
      const unsigned idLen = dirs[k] == 0 ? 1 : (unsigned)tree.Nodes[dirs[k]].Id[nt].Size();
      ptSize += 8 + idLen + (idLen & 1);
    }
    trees[t].PathTableSize = ptSize;
    const UInt32 ptSectors = (ptSize + kSector - 1) / kSector;
    trees[t].LPath = sector - bias;
    sector += ptSectors;
    trees[t].MPath = sector - bias;
    sector += ptSectors;
  }

  // the new files' data, after the trees
  const UInt32 dataStart = sector;
  FOR_VECTOR (ni, tree.Nodes)
  {
    CNode &node = tree.Nodes[ni];
    if (!node.NewData)
      continue;
    FOR_VECTOR (k, node.Extents)
    {
      node.Extents[k].Loc = sector;
      sector += (node.Extents[k].Size + kSector - 1) / kSector;
    }
  }
  const UInt32 volumeSectors = sector;

  // ---------- write the trees ----------
  RINOK(out->Seek((Int64)pos, STREAM_SEEK_SET, NULL))
  for (unsigned t = 0; t < 4; t++)
  {
    if (!used[t])
      continue;
    const unsigned nt = t & 1;
    writer.Bias = t >= 2 ? partStart : 0;
    FOR_VECTOR (ni, tree.Nodes)
      tree.Nodes[ni].Children = orders[t][ni];
    const CRecordVector<unsigned> &dirs = trees[t].Dirs;
    writer.CeAreas.Clear();
    CObjectVector<CObjectVector<CRecord> > records;
    FOR_VECTOR (k, dirs)
      writer.DirRecords(t, dirs[k], records.AddNew());
    writer.CeLoc = trees[t].CeLoc;
    writer.CeOffset = trees[t].CeOffset;
    if (writer.CeLoc.Size() != writer.CeAreas.Size())
      return E_FAIL;
    FOR_VECTOR (k, dirs)
    {
      CObjectVector<CRecord> &recs = records[k];
      FOR_VECTOR (r, recs)
        writer.SetCe(recs[r]);
      CByteBuffer data;
      CWriter::Layout(recs, data);
      if (data.Size() != tree.Nodes[dirs[k]].DirSize[t])
        return E_FAIL;
      RINOK(WriteStream(out, data, data.Size()))
    }
    if (!writer.CeAreas.IsEmpty())
    {
      const UInt32 first = writer.CeLoc[0];
      const UInt32 num = writer.CeLoc.Back() - first + 1;
      CByteBuffer ce((size_t)num * kSector);
      memset(ce, 0, ce.Size());
      FOR_VECTOR (a, writer.CeAreas)
        memcpy(ce + (size_t)(writer.CeLoc[a] - first) * kSector + writer.CeOffset[a],
            writer.CeAreas[a], writer.CeAreas[a].Size());
      RINOK(WriteStream(out, ce, ce.Size()))
    }
    // path tables, little- then big-endian
    const UInt32 ptSectors = (trees[t].PathTableSize + kSector - 1) / kSector;
    for (unsigned be = 0; be < 2; be++)
    {
      CByteBuffer pt((size_t)ptSectors * kSector);
      memset(pt, 0, pt.Size());
      size_t p = 0;
      FOR_VECTOR (k, dirs)
      {
        const CNode &d = tree.Nodes[dirs[k]];
        const Byte rootId = 0;
        const Byte *id = dirs[k] == 0 ? &rootId : (const Byte *)d.Id[nt];
        const unsigned idLen = dirs[k] == 0 ? 1 : (unsigned)d.Id[nt].Size();
        const UInt32 parentNum = d.Parent < 0 ? 1 : tree.Nodes[(unsigned)d.Parent].PathNum[t];
        pt[p] = (Byte)idLen;
        pt[p + 1] = 0;
        if (be)
        {
          SetBe32(pt + p + 2, d.DirLoc[t])
          SetBe16_(pt + p + 6, parentNum);
        }
        else
        {
          SetUi32(pt + p + 2, d.DirLoc[t])
          SetUi16(pt + p + 6, (UInt16)parentNum)
        }
        memcpy(pt + p + 8, id, idLen);
        p += 8 + idLen + (idLen & 1);
      }
      RINOK(WriteStream(out, pt, pt.Size()))
    }
  }
  {
    UInt64 cur = 0;
    RINOK(out->Seek(0, STREAM_SEEK_CUR, &cur))
    if (cur != (UInt64)dataStart * kSector)
      return E_FAIL;
  }

  // ---------- the new files' data ----------
  FOR_VECTOR (ni, tree.Nodes)
  {
    CNode &node = tree.Nodes[ni];
    if (!node.NewData)
      continue;
    if (!node.SymLink.IsEmpty())
    {
      RINOK(callback->SetOperationResult(NUpdate::NOperationResult::kOK))
      continue;
    }
    UInt64 planned = 0;
    FOR_VECTOR (k, node.Extents)
      planned += (UInt64)(node.Extents[k].Size + kSector - 1) / kSector * kSector;
    if (node.Relocate)
    {
      CByteBuffer buf(1 << 20);
      UInt64 size = 0;
      FOR_VECTOR (k, node.OldExtents)
      {
        RINOK(InStream_SeekSet(inStream, (UInt64)node.OldExtents[k].Loc * kSector))
        UInt64 left = node.OldExtents[k].Size;
        while (left != 0)
        {
          const size_t n = (size_t)MyMin(left, (UInt64)buf.Size());
          RINOK(ReadStream_FALSE(inStream, buf, n))
          RINOK(WriteStream(out, buf, n))
          left -= n;
          size += n;
          completed += n;
          RINOK(callback->SetCompleted(&completed))
        }
      }
      RINOK(WriteZeros(out, planned - size))
      continue;
    }
    CMyComPtr<ISequentialInStream> in;
    const HRESULT res = callback->GetStream((UInt32)node.UpdateIndex, &in);
    UInt64 size = 0;
    if (res != S_FALSE)
    {
      RINOK(res)
      if (in)
      {
        CByteBuffer buf(1 << 20);
        for (;;)
        {
          size_t n = buf.Size();
          RINOK(ReadStream(in, buf, &n))
          if (n == 0)
            break;
          // a file that grew since it was listed
          if (size + n > node.Size)
            return E_FAIL;
          RINOK(WriteStream(out, buf, n))
          size += n;
          completed += n;
          RINOK(callback->SetCompleted(&completed))
        }
      }
      // or shrank
      if (size != node.Size)
        return E_FAIL;
    }
    // a file that couldn't be opened (already reported) is left as zeros
    RINOK(WriteZeros(out, planned - size))
    if (res != S_FALSE)
      RINOK(callback->SetOperationResult(NUpdate::NOperationResult::kOK))
  }
  UInt64 endPos = 0;
  RINOK(out->Seek(0, STREAM_SEEK_CUR, &endPos))
  if (endPos != (UInt64)volumeSectors * kSector)
    return E_FAIL;
  RINOK(out->SetSize(endPos))

  // ---------- the volume descriptors ----------
  for (unsigned t = 0; t < 4; t++)
  {
    if (!used[t])
      continue;
    const CRecordVector<UInt32> &secs = t == 0 ? pvdSectors : t == 1 ? jolietSectors : t == 2 ? partPvd : partJoliet;
    const UInt32 bias = t >= 2 ? partStart : 0;
    FOR_VECTOR (k, secs)
    {
      Byte b[kSector];
      RINOK(InStream_SeekSet(inStream, (UInt64)secs[k] * kSector))
      RINOK(ReadStream_FALSE(inStream, b, kSector))
      SetBoth32(b + 80, volumeSectors - bias);
      SetBoth32(b + 132, trees[t].PathTableSize);
      SetUi32(b + 140, trees[t].LPath)
      SetUi32(b + 144, 0)
      SetBe32(b + 148, trees[t].MPath)
      SetBe32(b + 152, 0)
      const CNode &root = tree.Nodes[0];
      Byte *r = b + 156;
      memset(r, 0, 34);
      r[0] = 34;
      SetBoth32(r + 2, root.DirLoc[t]);
      SetBoth32(r + 10, root.DirSize[t]);
      memcpy(r + 18, root.Time, 7);
      r[25] = 2;
      SetBoth16(r + 28, 1);
      r[32] = 1;
      r[33] = 0;
      RINOK(out->Seek((Int64)secs[k] * kSector, STREAM_SEEK_SET, NULL))
      RINOK(WriteStream(out, b, kSector))
    }
  }

  // a hybrid MBR's partitions that ended where the old volume did end
  // where the new one does
  {
    Byte mbr[512];
    RINOK(InStream_SeekSet(inStream, 0))
    if (ReadStream_FALSE(inStream, mbr, 512) == S_OK && mbr[510] == 0x55 && mbr[511] == 0xAA)
    {
      const UInt64 oldEnd = (UInt64)arc.VolDescs[0].VolumeSpaceSize * 4;
      bool changed = false;
      for (unsigned k = 0; k < 4; k++)
      {
        Byte *e = mbr + 446 + k * 16;
        const UInt32 start = GetUi32(e + 8), size = GetUi32(e + 12);
        if (e[4] != 0 && e[4] != 0xEE && size != 0 && (UInt64)start + size == oldEnd)
        {
          SetUi32(e + 12, volumeSectors * 4 - start)
          changed = true;
        }
      }
      if (changed)
      {
        RINOK(out->Seek(0, STREAM_SEEK_SET, NULL))
        RINOK(WriteStream(out, mbr, 512))
      }
    }
  }
  return S_OK;
}

}}

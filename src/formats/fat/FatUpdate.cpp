// FatUpdate.cpp -- 7zfm-qt: adding, deleting and renaming files in a FAT12,
// FAT16 or FAT32 image, written for 7zfm-qt.
//
// The image is copied whole and then changed where it has to be, as an
// operating system would change the volume: kept files stay in their
// clusters; a deleted file's clusters become free (their bytes are left
// as they were); a new file gets free clusters, in one run when there is
// one; every directory is written again from the new tree, growing by a
// cluster when it must (a FAT12/16 root has a fixed size); and every copy
// of the FAT, and FAT32's free cluster count, is written again. Bad
// clusters stay marked.
//
// Names: a kept item keeps its 8.3 name and long name. A new or renamed
// one gets a long name (VFAT) unless its name is a valid 8.3 name in
// upper case, and an 8.3 name of its own as Windows makes it: upper case,
// what DOS can't store as '_', "~1" and up when it had to be cut or
// changed. Times are written in local time, as DOS and Windows keep them.

#include "Common/Common.h"

#include "C/CpuArch.h"

#include "Common/MyCom.h"
#include "Common/MyString.h"
#include "Common/StringConvert.h"

#include "Windows/PropVariant.h"
#include "Windows/TimeUtils.h"

#include "7zip/Common/StreamUtils.h"

#include "FatUpdate.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

namespace NArchive {
namespace NFat {

static const UInt32 kUsedByDir = (UInt32)1 << 31;

// errors as errno values: on Linux, 7-Zip's HRESULT_FROM_WIN32 takes errno
// codes and its messages come from strerror()
static HRESULT Win32Error(int e) { return HRESULT_FROM_WIN32((DWORD)e); }
static const int kErr_DiskFull = ENOSPC;         // also a full FAT12/16 root
static const int kErr_CannotMake = ENOSPC;
static const int kErr_FileTooLarge = EFBIG;      // over 4 GiB - 1
static const int kErr_NameTooLong = ENAMETOOLONG;
static const int kErr_Corrupt = EIO;             // a broken cluster chain

// ---------- the tree ----------

struct CNode
{
  UString Name;
  bool IsDir;
  int Parent;
  CRecordVector<unsigned> Children;
  int UpdateIndex;
  int OldIndex;          // the image's item it was, or -1
  bool NewData;
  bool KeepNames;        // the old 8.3 name and long name stay
  CRecordVector<UInt16> Lfn;     // the long name written, UTF-16
  UInt32 Size;
  Byte Attrib;
  Byte CaseFlags;
  Byte CTime2;
  UInt16 ADate;
  UInt32 CTime;
  UInt32 MTime;
  char Sfn[11];
  CRecordVector<UInt32> Chain;   // clusters (files: data, dirs: the directory)

  CNode(): IsDir(false), Parent(-1), UpdateIndex(-1), OldIndex(-1), NewData(false), KeepNames(false),
      Size(0), Attrib(0), CaseFlags(0), CTime2(0), ADate(0), CTime(0), MTime(0)
    { memset(Sfn, ' ', 11); }
};

static UInt32 DosNow()
{
  FILETIME ft;
  const UInt64 v = ((UInt64)time(NULL) + 11644473600ULL) * 10000000;
  ft.dwLowDateTime = (DWORD)v;
  ft.dwHighDateTime = (DWORD)(v >> 32);
  UInt32 dos = 0;
  NWindows::NTime::UtcFileTime_To_LocalDosTime(ft, dos);
  return dos;
}

class CTree
{
public:
  CObjectVector<CNode> Nodes;
  CTree()
  {
    CNode &root = Nodes.AddNew();
    root.IsDir = true;
  }
  int Find(unsigned dir, const UString &name) const
  {
    const CNode &d = Nodes[dir];
    FOR_VECTOR (i, d.Children)
      if (Nodes[d.Children[i]].Name.IsEqualTo_NoCase(name))
        return (int)d.Children[i];
    return -1;
  }
  int Add(const UString &path, bool isDir)
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
    if (parts.IsEmpty())
      return -1;
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
        node.Parent = (int)dir;
        node.Attrib = node.IsDir ? 0x10 : 0x20;
        node.MTime = node.CTime = DosNow();
        node.ADate = (UInt16)(node.MTime >> 16);
        n = (int)Nodes.Add(node);
        Nodes[dir].Children.Add((unsigned)n);
      }
      else if (last)
      {
        CNode &node = Nodes[(unsigned)n];
        if (node.IsDir != isDir && !node.Children.IsEmpty())
          return -1;
        node.IsDir = isDir;
      }
      dir = (unsigned)n;
    }
    return (int)dir;
  }
};

// ---------- 8.3 names ----------

static bool IsSfnChar(wchar_t c)
{
  if (c >= 'A' && c <= 'Z') return true;
  if (c >= '0' && c <= '9') return true;
  return c != 0 && c < 0x80 && strchr("!#$%&'()-@^_`{}~", (char)c) != NULL;
}

static void SetSfn(char *sfn, const AString &base, const AString &ext)
{
  memset(sfn, ' ', 11);
  memcpy(sfn, base.Ptr(), MyMin(base.Len(), 8u));
  memcpy(sfn + 8, ext.Ptr(), MyMin(ext.Len(), 3u));
  if ((Byte)sfn[0] == 0xE5)
    sfn[0] = 5;
}

// an 8.3 name for (name), unique among (used); (lfn): a long name is needed
static void MakeSfn(const UString &name, bool isDir, const CObjectVector<AString> &used, char *sfn, bool &lfn)
{
  (void)isDir;
  // the base and extension, as Windows makes them
  UString n = name;
  bool lossy = false;
  // spaces go; leading dots go
  {
    UString t;
    for (unsigned i = 0; i < n.Len(); i++)
      if (n[i] != L' ')
        t += n[i];
      else
        lossy = true;
    unsigned k = 0;
    while (k < t.Len() && t[k] == L'.')
      k++;
    if (k)
    {
      lossy = true;
      t.DeleteFrontal(k);
    }
    n = t;
  }
  const int dot = n.ReverseFind(L'.');
  UString b = dot >= 0 ? n.Left((unsigned)dot) : n;
  UString e;
  if (dot >= 0)
    e = n.Ptr((unsigned)dot + 1);
  if (b.Find(L'.') >= 0)
    lossy = true;
  AString base, ext;
  for (unsigned pass = 0; pass < 2; pass++)
  {
    const UString &src = pass ? e : b;
    AString &dst = pass ? ext : base;
    for (unsigned i = 0; i < src.Len(); i++)
    {
      wchar_t c = src[i];
      if (c == L'.')
        continue;
      if (c >= 'a' && c <= 'z')
      {
        c = (wchar_t)(c - 'a' + 'A');
        lossy = true;   // the case is kept in the long name
      }
      if (!IsSfnChar(c))
      {
        c = L'_';
        lossy = true;
      }
      dst += (char)c;
    }
  }
  if (base.IsEmpty() && !ext.IsEmpty())
  {
    base = ext;
    ext.Empty();
    lossy = true;
  }
  if (base.IsEmpty())
  {
    base = "_";
    lossy = true;
  }
  if (base.Len() > 8 || ext.Len() > 3)
    lossy = true;
  if (ext.Len() > 3)
    ext.DeleteFrom(3);
  lfn = lossy;
  if (!lossy)
  {
    SetSfn(sfn, base, ext);
    bool clash = false;
    FOR_VECTOR (u, used)
      if (memcmp(used[u].Ptr(), sfn, 11) == 0)
        clash = true;
    if (!clash)
      return;
    lfn = true;
  }
  for (unsigned num = 1; num < 1000000; num++)
  {
    char tail[16];
    snprintf(tail, sizeof(tail), "~%u", num);
    const unsigned tl = (unsigned)strlen(tail);
    AString b2 = base;
    if (b2.Len() > 8 - tl)
      b2.DeleteFrom(8 - tl);
    b2 += tail;
    SetSfn(sfn, b2, ext);
    bool clash = false;
    FOR_VECTOR (u, used)
      if (memcmp(used[u].Ptr(), sfn, 11) == 0)
        clash = true;
    if (!clash)
      return;
  }
}

static Byte SfnChecksum(const char *sfn)
{
  Byte sum = 0;
  for (unsigned i = 0; i < 11; i++)
    sum = (Byte)(((sum & 1) << 7) + (sum >> 1) + (Byte)sfn[i]);
  return sum;
}

// the name as UTF-16, without what VFAT forbids
static void ToUtf16(const UString &name, CRecordVector<UInt16> &u)
{
  u.Clear();
  for (unsigned i = 0; i < name.Len(); i++)
  {
    UInt32 c = (UInt32)name[i];
    if (c < 0x20 || (c < 0x80 && strchr("\\/:*?\"<>|", (char)c)))
      c = '_';
    if (c >= 0x10000 && c <= 0x10FFFF)
    {
      c -= 0x10000;
      u.Add((UInt16)(0xD800 + (c >> 10)));
      u.Add((UInt16)(0xDC00 + (c & 0x3FF)));
    }
    else
      u.Add((UInt16)c);
  }
}

// ---------- directory entries ----------

static void PutSfnEntry(Byte *p, const char *sfn, Byte attrib, Byte caseFlags, Byte ctime2, UInt32 ctime,
    UInt16 adate, UInt32 mtime, UInt32 cluster, UInt32 size, bool fat32)
{
  memset(p, 0, 32);
  memcpy(p, sfn, 11);
  p[11] = attrib;
  p[12] = caseFlags;
  p[13] = ctime2;
  SetUi32(p + 14, ctime)
  SetUi16(p + 18, adate)
  SetUi16(p + 20, (UInt16)(fat32 ? cluster >> 16 : 0))
  SetUi32(p + 22, mtime)
  SetUi16(p + 26, (UInt16)cluster)
  SetUi32(p + 28, size)
}

// the long name's entries, last piece first, then nothing (the 8.3 entry follows)
static void PutLfnEntries(CByteBuffer &out, size_t &pos, const CRecordVector<UInt16> &name, Byte sum)
{
  const unsigned n = (name.Size() + 12) / 13;
  for (unsigned k = n; k >= 1; k--)
  {
    Byte *p = out + pos;
    pos += 32;
    memset(p, 0, 32);
    p[0] = (Byte)(k | (k == n ? 0x40 : 0));
    p[11] = 0x0F;
    p[13] = sum;
    static const unsigned kOffs[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    for (unsigned j = 0; j < 13; j++)
    {
      const unsigned idx = (k - 1) * 13 + j;
      UInt16 c;
      if (idx < name.Size())
        c = name[idx];
      else if (idx == name.Size())
        c = 0;
      else
        c = 0xFFFF;
      SetUi16(p + kOffs[j], c)
    }
  }
}

// ---------- the update ----------

class CFat
{
public:
  CRecordVector<UInt32> Entries;
  UInt32 Bad;
  UInt32 Eoc;
  UInt32 Hint;
  CFat(): Bad(0), Eoc(0), Hint(2) {}
  bool IsFree(UInt32 c) const { return Entries[c] == 0; }
  UInt32 NumFree() const
  {
    UInt32 n = 0;
    for (UInt32 c = 2; c < Entries.Size(); c++)
      if (Entries[c] == 0)
        n++;
    return n;
  }
  // (n) free clusters, in one run if there is one
  bool Alloc(UInt32 n, CRecordVector<UInt32> &chain)
  {
    chain.Clear();
    if (n == 0)
      return true;
    const UInt32 size = Entries.Size();
    UInt32 run = 0;
    for (UInt32 c = 2; c < size; c++)
    {
      run = Entries[c] == 0 ? run + 1 : 0;
      if (run == n)
      {
        for (UInt32 k = c + 1 - n; k <= c; k++)
          chain.Add(k);
        break;
      }
    }
    if (chain.IsEmpty())
      for (UInt32 c = 2; c < size && chain.Size() < n; c++)
        if (Entries[c] == 0)
          chain.Add(c);
    if (chain.Size() != n)
    {
      chain.Clear();
      return false;
    }
    Link(chain, 0);
    return true;
  }
  void Link(const CRecordVector<UInt32> &chain, unsigned from)
  {
    for (unsigned i = from; i < chain.Size(); i++)
      Entries[chain[i]] = (i + 1 < chain.Size()) ? chain[i + 1] : Eoc;
  }
};

static HRESULT GetString(IArchiveUpdateCallback *cb, UInt32 index, PROPID id, UString &s)
{
  NWindows::NCOM::CPropVariant prop;
  RINOK(cb->GetProperty(index, id, &prop))
  s.Empty();
  if (prop.vt == VT_BSTR)
    s = prop.bstrVal;
  else if (prop.vt != VT_EMPTY)
    return E_INVALIDARG;
  return S_OK;
}

static HRESULT GetDosTime(IArchiveUpdateCallback *cb, UInt32 index, PROPID id, UInt32 &dos, bool &defined)
{
  NWindows::NCOM::CPropVariant prop;
  RINOK(cb->GetProperty(index, id, &prop))
  defined = false;
  if (prop.vt == VT_FILETIME)
    defined = NWindows::NTime::UtcFileTime_To_LocalDosTime(prop.filetime, dos);
  else if (prop.vt != VT_EMPTY)
    return E_INVALIDARG;
  return S_OK;
}

static HRESULT WriteZeros(ISequentialOutStream *out, UInt64 n)
{
  Byte zeros[4096];
  memset(zeros, 0, sizeof(zeros));
  while (n != 0)
  {
    const UInt32 k = (UInt32)MyMin(n, (UInt64)sizeof(zeros));
    RINOK(WriteStream(out, zeros, k))
    n -= k;
  }
  return S_OK;
}

HRESULT UpdateFat(IInStream *inStream, const CFatVolume &vol, IInArchive *handler,
    ISequentialOutStream *outSeq, UInt32 numItems, IArchiveUpdateCallback *callback)
{
  CMyComPtr<IOutStream> out;
  outSeq->QueryInterface(IID_IOutStream, (void **)&out);
  if (!out)
    return E_NOTIMPL;
  const bool fat32 = vol.NumFatBits == 32;
  const UInt32 sectorSize = (UInt32)1 << vol.SectorSizeLog;
  const UInt32 clusterSize = (UInt32)1 << vol.ClusterSizeLog;
  UInt64 inSize = 0;
  RINOK(InStream_GetSize_SeekToEnd(inStream, inSize))
  if (inSize < ((UInt64)vol.NumSectors << vol.SectorSizeLog))
    return E_NOTIMPL;   // a cut image

  // ---------- the items ----------
  CTree tree;
  UInt64 newDataTotal = 0;
  for (UInt32 i = 0; i < numItems; i++)
  {
    Int32 newData = 0, newProps = 0;
    UInt32 indexInArc = (UInt32)(Int32)-1;
    RINOK(callback->GetUpdateItemInfo(i, &newData, &newProps, &indexInArc))
    const bool fromArc = indexInArc != (UInt32)(Int32)-1 && indexInArc < vol.Items.Size();
    if (!fromArc && !newData)
      continue;
    const CFatVolume::CItem *old = fromArc ? &vol.Items[indexInArc] : NULL;

    UString path;
    bool isDir = false;
    if (newProps)
    {
      RINOK(GetString(callback, i, kpidPath, path))
      NWindows::NCOM::CPropVariant prop;
      RINOK(callback->GetProperty(i, kpidIsDir, &prop))
      if (prop.vt == VT_BOOL)
        isDir = prop.boolVal != VARIANT_FALSE;
      else if (prop.vt != VT_EMPTY)
        return E_INVALIDARG;
    }
    else
    {
      NWindows::NCOM::CPropVariant prop;
      RINOK(handler->GetProperty(indexInArc, kpidPath, &prop))
      if (prop.vt == VT_BSTR)
        path = prop.bstrVal;
      isDir = (old->Attrib & 0x10) != 0;
    }
    const int n = tree.Add(path, isDir);
    if (n <= 0)
      return E_INVALIDARG;
    CNode &node = tree.Nodes[(unsigned)n];
    node.IsDir = isDir;
    node.UpdateIndex = (int)i;
    node.OldIndex = fromArc ? (int)indexInArc : -1;
    node.NewData = newData != 0 && !isDir;
    node.KeepNames = false;
    if (old)
    {
      memcpy(node.Sfn, old->DosName, 11);
      if ((Byte)node.Sfn[0] == 0xE5)
        node.Sfn[0] = 5;
      node.CaseFlags = old->Flags;
      node.Attrib = old->Attrib;
      node.CTime2 = old->CTime2;
      node.CTime = old->CTime;
      node.ADate = old->ADate;
      node.MTime = old->MTime;
      node.Size = old->Size;
      // the same name: the 8.3 name and long name stay as they were
      NWindows::NCOM::CPropVariant prop;
      RINOK(handler->GetProperty(indexInArc, kpidName, &prop))
      node.KeepNames = (prop.vt == VT_BSTR && node.Name == prop.bstrVal);
    }
    if (newProps)
    {
      NWindows::NCOM::CPropVariant prop;
      RINOK(callback->GetProperty(i, kpidAttrib, &prop))
      if (prop.vt == VT_UI4)
        node.Attrib = (Byte)((prop.ulVal & 0x27) | (isDir ? 0x10 : 0));
      else if (!old)
        node.Attrib = isDir ? 0x10 : 0x20;
      bool defined;
      UInt32 t;
      RINOK(GetDosTime(callback, i, kpidMTime, t, defined))
      if (defined)
        node.MTime = t;
      RINOK(GetDosTime(callback, i, kpidCTime, t, defined))
      if (defined)
      {
        node.CTime = t;
        node.CTime2 = 0;
      }
      else if (!old)
        node.CTime = node.MTime;
      RINOK(GetDosTime(callback, i, kpidATime, t, defined))
      node.ADate = (UInt16)((defined ? t : node.MTime) >> 16);
    }
    if (node.NewData)
    {
      NWindows::NCOM::CPropVariant prop;
      RINOK(callback->GetProperty(i, kpidSize, &prop))
      UInt64 size = 0;
      if (prop.vt == VT_UI8)
        size = prop.uhVal.QuadPart;
      else if (prop.vt != VT_EMPTY)
        return E_INVALIDARG;
      if (size > 0xFFFFFFFF)
        return Win32Error(kErr_FileTooLarge);
      node.Size = (UInt32)size;
      newDataTotal += size;
    }
    if (isDir)
      node.Size = 0;
  }

  // ---------- the new FAT: kept chains, bad clusters ----------
  CFat fat;
  fat.Entries.ClearAndSetSize(vol.FatSize);
  for (UInt32 c = 0; c < vol.FatSize; c++)
    fat.Entries[c] = 0;
  const UInt32 mask = fat32 ? 0x0FFFFFFF : (((UInt32)1 << vol.NumFatBits) - 1);
  fat.Bad = vol.BadCluster;
  fat.Eoc = mask;
  fat.Entries[0] = vol.Fat[0] & mask;
  fat.Entries[1] = vol.Fat[1] & mask;
  for (UInt32 c = 2; c < vol.FatSize; c++)
    if ((vol.Fat[c] & mask) == vol.BadCluster)
      fat.Entries[c] = vol.BadCluster;

  // an old chain: (count) clusters, or to its end (count 0)
  auto takeChain = [&](UInt32 first, UInt32 count, CRecordVector<UInt32> &chain) -> HRESULT
  {
    chain.Clear();
    UInt32 c = first;
    for (;;)
    {
      if (c < 2 || c >= vol.FatSize || fat.Entries[c] != 0 || chain.Size() >= vol.FatSize)
        return Win32Error(kErr_Corrupt);
      chain.Add(c);
      const UInt32 next = vol.Fat[c] & mask & ~kUsedByDir;
      fat.Entries[c] = fat.Eoc;   // marks it taken; linked below
      if (count != 0 && chain.Size() == count)
        break;
      if (next > vol.BadCluster)
        break;
      c = next;
    }
    if (count != 0 && chain.Size() != count)
      return Win32Error(kErr_Corrupt);
    fat.Link(chain, 0);
    return S_OK;
  };

  // FAT32's root
  if (fat32)
  {
    RINOK(takeChain(vol.RootCluster, 0, tree.Nodes[0].Chain))
  }
  FOR_VECTOR (ni, tree.Nodes)
  {
    CNode &node = tree.Nodes[ni];
    if (ni == 0 || node.OldIndex < 0 || node.NewData)
      continue;
    const CFatVolume::CItem &old = vol.Items[(unsigned)node.OldIndex];
    const bool oldIsDir = (old.Attrib & 0x10) != 0;
    if (node.IsDir != oldIsDir)
      continue;
    if (node.IsDir)
    {
      if (old.Cluster != 0)
        RINOK(takeChain(old.Cluster, 0, node.Chain))
    }
    else if (old.Size != 0)
    {
      const UInt32 num = (UInt32)(((UInt64)old.Size + clusterSize - 1) >> vol.ClusterSizeLog);
      RINOK(takeChain(old.Cluster, num, node.Chain))
    }
  }

  // ---------- new files' clusters ----------
  FOR_VECTOR (ni, tree.Nodes)
  {
    CNode &node = tree.Nodes[ni];
    if (!node.NewData)
      continue;
    const UInt32 num = (UInt32)(((UInt64)node.Size + clusterSize - 1) >> vol.ClusterSizeLog);
    if (!fat.Alloc(num, node.Chain))
      return Win32Error(kErr_DiskFull);
  }

  // ---------- names and directories ----------
  // each directory's entries: 8.3 names first for the kept ones, then the rest
  CObjectVector<CByteBuffer> dirData;
  dirData.ClearAndReserve(tree.Nodes.Size());
  for (unsigned ni = 0; ni < tree.Nodes.Size(); ni++)
    dirData.AddNew();
  FOR_VECTOR (ni, tree.Nodes)
  {
    CNode &d = tree.Nodes[ni];
    if (!d.IsDir)
      continue;
    CObjectVector<AString> used;
    FOR_VECTOR (k, d.Children)
    {
      const CNode &c = tree.Nodes[d.Children[k]];
      if (c.KeepNames)
      {
        AString s;
        s.SetFrom(c.Sfn, 11);
        used.Add(s);
      }
    }
    // the entries: count them first
    unsigned numEntries = (ni == 0) ? (vol.HasLabel ? 1 : 0) : 2;
    FOR_VECTOR (k, d.Children)
    {
      CNode &c = tree.Nodes[d.Children[k]];
      CRecordVector<UInt16> &lfn = c.Lfn;
      lfn.Clear();
      if (c.KeepNames && c.OldIndex >= 0)
      {
        const CByteBuffer &ln = vol.Items[(unsigned)c.OldIndex].LongName;
        for (size_t j = 0; j + 1 < ln.Size(); j += 2)
        {
          const UInt16 ch = GetUi16(ln + j);
          if (ch == 0)
            break;
          lfn.Add(ch);
        }
        // a name the reader made up ("[NONAME]-n") is no long name
        if (memcmp(c.Sfn, "           ", 11) == 0)
          lfn.Clear();
      }
      else
      {
        bool needLfn = false;
        MakeSfn(c.Name, c.IsDir, used, c.Sfn, needLfn);
        c.CaseFlags = 0;
        AString s;
        s.SetFrom(c.Sfn, 11);
        used.Add(s);
        if (needLfn)
          ToUtf16(c.Name, lfn);
      }
      if (lfn.Size() > 255)
        return Win32Error(kErr_NameTooLong);
      numEntries += 1 + (lfn.Size() + 12) / 13;
    }
    const UInt32 bytes = numEntries * 32;
    if (ni == 0 && !fat32)
    {
      if (bytes > vol.NumRootDirSectors * sectorSize)
        return Win32Error(kErr_CannotMake);
    }
    else
    {
      if (bytes > (1u << 21))
        return Win32Error(kErr_CannotMake);
      const UInt32 need = MyMax((UInt32)1, (bytes + clusterSize - 1) / clusterSize);
      if (d.Chain.Size() < need)
      {
        CRecordVector<UInt32> more;
        if (!fat.Alloc(need - d.Chain.Size(), more))
          return Win32Error(kErr_DiskFull);
        const unsigned from = d.Chain.Size();
        FOR_VECTOR (m, more)
          d.Chain.Add(more[m]);
        fat.Link(d.Chain, from ? from - 1 : 0);
      }
    }
    // the bytes come later, when every directory has its clusters
    CByteBuffer &data = dirData[ni];
    data.Alloc(bytes);
    memset(data, 0, bytes);
  }

  // now the directories' contents
  FOR_VECTOR (ni, tree.Nodes)
  {
    CNode &d = tree.Nodes[ni];
    if (!d.IsDir)
      continue;
    CByteBuffer &data = dirData[ni];
    size_t pos = 0;
    if (ni == 0)
    {
      if (vol.HasLabel)
      {
        PutSfnEntry(data + pos, vol.Label, 0x08, 0, 0, 0, 0, vol.LabelTime, 0, 0, fat32);
        pos += 32;
      }
    }
    else
    {
      const CNode &parent = tree.Nodes[(unsigned)d.Parent];
      const UInt32 self = d.Chain.IsEmpty() ? 0 : d.Chain[0];
      const UInt32 up = (d.Parent == 0 || parent.Chain.IsEmpty()) ? 0 : parent.Chain[0];
      PutSfnEntry(data + pos, ".          ", 0x10, 0, d.CTime2, d.CTime, d.ADate, d.MTime, self, 0, fat32);
      pos += 32;
      PutSfnEntry(data + pos, "..         ", 0x10, 0, parent.CTime2, parent.CTime, parent.ADate, parent.MTime, up, 0, fat32);
      pos += 32;
    }
    FOR_VECTOR (k, d.Children)
    {
      const CNode &c = tree.Nodes[d.Children[k]];
      const CRecordVector<UInt16> &lfn = c.Lfn;
      if (!lfn.IsEmpty())
        PutLfnEntries(data, pos, lfn, SfnChecksum(c.Sfn));
      const UInt32 first = c.Chain.IsEmpty() ? 0 : c.Chain[0];
      PutSfnEntry(data + pos, c.Sfn, c.Attrib, c.CaseFlags, c.CTime2, c.CTime, c.ADate, c.MTime,
          first, c.IsDir ? 0 : c.Size, fat32);
      pos += 32;
    }
    if (pos != data.Size())
      return E_FAIL;
  }

  // ---------- write ----------
  RINOK(callback->SetTotal(inSize + newDataTotal))
  UInt64 completed = 0;
  RINOK(InStream_SeekSet(inStream, 0))
  RINOK(out->Seek(0, STREAM_SEEK_SET, NULL))
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
  }
  const UInt64 dataStart = (UInt64)vol.DataSector << vol.SectorSizeLog;
  auto clusterPos = [&](UInt32 c) -> UInt64 { return dataStart + ((UInt64)(c - 2) << vol.ClusterSizeLog); };

  // the new files' data
  {
    CByteBuffer buf(clusterSize);
    FOR_VECTOR (ni, tree.Nodes)
    {
      const CNode &node = tree.Nodes[ni];
      if (!node.NewData)
        continue;
      CMyComPtr<ISequentialInStream> in;
      const HRESULT res = callback->GetStream((UInt32)node.UpdateIndex, &in);
      if (res != S_FALSE)
        RINOK(res)
      UInt64 size = 0;
      FOR_VECTOR (k, node.Chain)
      {
        size_t n = clusterSize;
        if (in && res != S_FALSE)
        {
          RINOK(ReadStream(in, buf, &n))
        }
        else
          n = 0;
        if (size + n > node.Size)
          return E_FAIL;   // the file grew since it was listed
        if (n < clusterSize)
          memset(buf + n, 0, clusterSize - n);
        RINOK(out->Seek((Int64)clusterPos(node.Chain[k]), STREAM_SEEK_SET, NULL))
        RINOK(WriteStream(out, buf, clusterSize))
        size += n;
        completed += n;
        RINOK(callback->SetCompleted(&completed))
      }
      if (in && res != S_FALSE)
      {
        // nothing more may follow
        size_t n = 1;
        Byte b;
        RINOK(ReadStream(in, &b, &n))
        if (n != 0 || size != node.Size)
          return E_FAIL;
        RINOK(callback->SetOperationResult(NUpdate::NOperationResult::kOK))
      }
    }
  }

  // the directories
  FOR_VECTOR (ni, tree.Nodes)
  {
    const CNode &d = tree.Nodes[ni];
    if (!d.IsDir)
      continue;
    const CByteBuffer &data = dirData[ni];
    if (ni == 0 && !fat32)
    {
      RINOK(out->Seek((Int64)((UInt64)vol.RootDirSector << vol.SectorSizeLog), STREAM_SEEK_SET, NULL))
      RINOK(WriteStream(out, data, data.Size()))
      RINOK(WriteZeros(out, (UInt64)vol.NumRootDirSectors * sectorSize - data.Size()))
      continue;
    }
    FOR_VECTOR (k, d.Chain)
    {
      RINOK(out->Seek((Int64)clusterPos(d.Chain[k]), STREAM_SEEK_SET, NULL))
      const size_t from = (size_t)k * clusterSize;
      const size_t n = from < data.Size() ? MyMin((size_t)clusterSize, data.Size() - from) : 0;
      if (n)
        RINOK(WriteStream(out, data + from, n))
      RINOK(WriteZeros(out, clusterSize - n))
    }
  }

  // the FATs: the first copy as it was, with the entries changed
  {
    const size_t fatBytes = (size_t)vol.NumFatSectors << vol.SectorSizeLog;
    CByteBuffer f(fatBytes);
    const UInt64 fatPos = (UInt64)vol.NumReservedSectors << vol.SectorSizeLog;
    RINOK(InStream_SeekSet(inStream, fatPos))
    RINOK(ReadStream_FALSE(inStream, f, fatBytes))
    for (UInt32 c = 0; c < vol.FatSize; c++)
    {
      const UInt32 v = fat.Entries[c];
      if (vol.NumFatBits == 32)
      {
        const size_t o = (size_t)c * 4;
        if (o + 4 <= fatBytes)
          SetUi32(f + o, (GetUi32(f + o) & 0xF0000000) | (v & 0x0FFFFFFF))
      }
      else if (vol.NumFatBits == 16)
      {
        const size_t o = (size_t)c * 2;
        if (o + 2 <= fatBytes)
          SetUi16(f + o, (UInt16)v)
      }
      else
      {
        const size_t o = (size_t)c * 3 / 2;
        if (o + 2 > fatBytes)
          continue;
        UInt16 w = GetUi16(f + o);
        if (c & 1)
          w = (UInt16)((w & 0x000F) | ((v & 0xFFF) << 4));
        else
          w = (UInt16)((w & 0xF000) | (v & 0xFFF));
        SetUi16(f + o, w)
      }
    }
    for (unsigned k = 0; k < vol.NumFats; k++)
    {
      RINOK(out->Seek((Int64)(fatPos + (UInt64)k * fatBytes), STREAM_SEEK_SET, NULL))
      RINOK(WriteStream(out, f, fatBytes))
    }
  }

  // FAT32's FSInfo: the free count and where to look next
  if (fat32 && vol.FsInfoSector != 0 && vol.FsInfoSector < vol.NumReservedSectors)
  {
    Byte s[512];
    const UInt64 p = (UInt64)vol.FsInfoSector << vol.SectorSizeLog;
    RINOK(InStream_SeekSet(inStream, p))
    if (ReadStream_FALSE(inStream, s, 512) == S_OK && GetUi32(s) == 0x41615252 && GetUi32(s + 484) == 0x61417272)
    {
      SetUi32(s + 488, fat.NumFree())
      UInt32 next = 0xFFFFFFFF;
      for (UInt32 c = 2; c < vol.FatSize; c++)
        if (fat.Entries[c] == 0)
        {
          next = c;
          break;
        }
      SetUi32(s + 492, next)
      RINOK(out->Seek((Int64)p, STREAM_SEEK_SET, NULL))
      RINOK(WriteStream(out, s, 512))
    }
  }
  RINOK(out->SetSize(inSize))
  return S_OK;
}

}}

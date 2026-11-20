// DiscTracks.cpp -- 7zfm-qt: track images (see DiscImage.h): CUE/BIN,
// CCD/IMG, MDS/MDF, NRG, GDI and CDI. The file layouts are as libmirage
// (cdemu) reads them; no libmirage code is used here.
//
// A raw .bin/.img/.mdf opens by its first sector's sync pattern (format
// "CD") and reads its tracks from the .cue, .ccd or .mds next to it; the
// descriptors open too, and read the data files next to them.

#include "DiscImage.h"

#include "Common/ComTry.h"
#include "Common/StringConvert.h"
#include "Common/UTFConvert.h"

#include "7zip/Common/RegisterArc.h"
#include "7zip/Common/StreamUtils.h"

#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace NArchive {
namespace NDisc {

static const size_t kMaxDescriptorSize = 1 << 20;

// ---------- text helpers ----------

static std::vector<std::string> SplitLines(const CByteBuffer &buf)
{
  std::vector<std::string> lines;
  std::string cur;
  for (size_t i = 0; i < buf.Size(); i++)
  {
    const char c = (char)buf[i];
    if (c == '\n' || c == '\r')
    {
      lines.push_back(cur);
      cur.clear();
    }
    else if (c != 0)
      cur += c;
  }
  lines.push_back(cur);
  return lines;
}

// words; a "quoted string" is one word
static std::vector<std::string> SplitWords(const std::string &line)
{
  std::vector<std::string> words;
  size_t i = 0;
  while (i < line.size())
  {
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
      i++;
    if (i >= line.size())
      break;
    std::string w;
    if (line[i] == '"')
    {
      i++;
      while (i < line.size() && line[i] != '"')
        w += line[i++];
      i++;
    }
    else
      while (i < line.size() && line[i] != ' ' && line[i] != '\t')
        w += line[i++];
    words.push_back(w);
  }
  return words;
}

static std::string Lower(std::string s)
{
  for (char &c : s)
    if (c >= 'A' && c <= 'Z')
      c = (char)(c - 'A' + 'a');
  return s;
}

static UString ToUString(const std::string &s)
{
  UString u;
  if (ConvertUTF8ToUnicode(AString(s.c_str()), u))
    return u;
  return MultiByteToUnicodeString(AString(s.c_str()));
}

static bool ParseMsf(const std::string &s, Int64 &frames)
{
  unsigned m, sec, f;
  char extra;
  if (sscanf(s.c_str(), "%u:%u:%u%c", &m, &sec, &f, &extra) != 3 || sec >= 60 || f >= kFramesPerSecond)
    return false;
  frames = ((Int64)m * 60 + sec) * kFramesPerSecond + f;
  return true;
}

static bool CheckDataSector(CDisc &disc, const CTrack &t)
{
  if (t.Format != kRaw2352 || !t.IsData() || t.NumSectors == 0)
    return true;
  Byte s[kRawSectorSize];
  bool ok;
  if (disc.ReadSector(t, 0, s, ok) != S_OK || !ok)
    return false;
  return memcmp(s, kSync, 12) == 0;
}

// a raw data track's mode, from its first sector (pregap sectors may be zeros)
static ETrackMode SniffMode(CDisc &disc, const CTrack &t, ETrackMode def)
{
  if (t.Format != kRaw2352)
    return def;
  Byte s[kRawSectorSize];
  for (UInt32 i = 0; i < MyMin(t.NumSectors, (UInt32)16); i++)
  {
    bool ok;
    if (disc.ReadSector(t, i, s, ok) != S_OK || !ok)
      break;
    if (memcmp(s, kSync, 12) == 0)
      return s[15] == 2 ? kMode2 : s[15] == 1 ? kMode1 : def;
  }
  return def;
}

// ---------- CUE ----------

struct CCueTrack
{
  UInt32 Number;
  ETrackMode Mode;
  ESectorFormat Format;
  unsigned Stride;
  bool Swap;
  Int64 Index0, Index1;
  Int64 Pregap, Postgap;
  UInt32 Session;
};

struct CCueFile
{
  std::string Name;
  bool Motorola;
  std::vector<CCueTrack> Tracks;
};

static bool CueTrackType(const std::string &typeIn, CCueTrack &t)
{
  const std::string type = Lower(typeIn);
  t.Stride = kRawSectorSize;
  t.Format = kRaw2352;
  if (type == "audio") t.Mode = kAudio;
  else if (type == "cdg") { t.Mode = kAudio; t.Stride = 2448; }
  else if (type == "mode1/2352") t.Mode = kMode1;
  else if (type == "mode1/2048") { t.Mode = kMode1; t.Format = kCooked2048; t.Stride = 2048; }
  else if (type == "mode2/2352" || type == "cdi/2352") t.Mode = kMode2;
  else if (type == "mode2/2336" || type == "cdi/2336") { t.Mode = kMode2; t.Format = kMode2_2336; t.Stride = 2336; }
  else if (type == "mode2/2048") { t.Mode = kMode2; t.Format = kCooked2048; t.Stride = 2048; }
  else if (type == "mode2/2324") { t.Mode = kOther; t.Format = kForm2_2324; t.Stride = 2324; }
  else
    return false;
  return true;
}

HRESULT LoadCue(CDisc &disc, const CByteBuffer &cue, IArchiveOpenCallback *callback,
    IInStream *mainStream, const UString &mainName)
{
  std::vector<CCueFile> files;
  UInt32 session = 1;
  for (const std::string &line : SplitLines(cue))
  {
    const std::vector<std::string> w = SplitWords(line);
    if (w.empty())
      continue;
    const std::string cmd = Lower(w[0]);
    if (cmd == "rem" && w.size() >= 3 && Lower(w[1]) == "session")
      session = (UInt32)atoi(w[2].c_str());
    else if (cmd == "file" && w.size() >= 2)
    {
      CCueFile f;
      f.Name = w[1];
      const std::string type = w.size() >= 3 ? Lower(w[2]) : "binary";
      if (type == "motorola")
        f.Motorola = true;
      else if (type == "binary")
        f.Motorola = false;
      else
        return S_FALSE; // WAVE, MP3, FLAC...: not sectors
      files.push_back(f);
    }
    else if (cmd == "track" && w.size() >= 3)
    {
      if (files.empty())
        return S_FALSE;
      CCueTrack t;
      t.Number = (UInt32)atoi(w[1].c_str());
      if (!CueTrackType(w[2], t))
        return S_FALSE;
      t.Swap = files.back().Motorola && t.Mode == kAudio;
      t.Index0 = t.Index1 = -1;
      t.Pregap = t.Postgap = 0;
      t.Session = session;
      files.back().Tracks.push_back(t);
    }
    else if ((cmd == "index" || cmd == "pregap" || cmd == "postgap") && !files.empty() && !files.back().Tracks.empty())
    {
      CCueTrack &t = files.back().Tracks.back();
      Int64 frames;
      if (!ParseMsf(w.back(), frames))
        return S_FALSE;
      if (cmd == "pregap") t.Pregap = frames;
      else if (cmd == "postgap") t.Postgap = frames;
      else
      {
        const int n = atoi(w[1].c_str());
        if (n == 0) t.Index0 = frames;
        else if (n == 1) t.Index1 = frames;
      }
    }
  }
  if (files.empty())
    return S_FALSE;

  Int64 lbaBase = 0; // the LBA of the current file's frame 0
  for (size_t fi = 0; fi < files.size(); fi++)
  {
    CCueFile &f = files[fi];
    if (f.Tracks.empty())
      continue;
    CMyComPtr<IInStream> stream;
    const UString name = ToUString(f.Name);
    if (mainStream && (files.size() == 1 || StringsAreEqualNoCase(name, mainName)
        || StringsAreEqualNoCase(BaseName(name), BaseName(mainName))))
      stream = mainStream;
    else
    {
      RINOK(OpenSibling(callback, name, files.size() == 1 ? "bin" : NULL, stream))
      if (!stream)
        return S_FALSE;
    }
    UInt64 fileSize;
    RINOK(StreamSize(stream, fileSize))
    const unsigned source = disc.Sources.Size();
    disc.Sources.Add(stream);

    // each track runs from its INDEX 01 to the next track's first index
    std::vector<UInt64> regionByte(f.Tracks.size());
    std::vector<Int64> regionFrame(f.Tracks.size());
    for (size_t i = 0; i < f.Tracks.size(); i++)
    {
      const CCueTrack &c = f.Tracks[i];
      if (c.Index1 < 0)
        return S_FALSE;
      const Int64 first = c.Index0 >= 0 ? c.Index0 : c.Index1;
      if (i == 0)
        regionByte[i] = (UInt64)first * c.Stride;
      else
      {
        if (first < regionFrame[i - 1])
          return S_FALSE;
        regionByte[i] = regionByte[i - 1] + (UInt64)(first - regionFrame[i - 1]) * f.Tracks[i - 1].Stride;
      }
      regionFrame[i] = first;
    }
    Int64 gaps = 0; // PREGAP and POSTGAP: on the disc, not in the file
    for (size_t i = 0; i < f.Tracks.size(); i++)
    {
      const CCueTrack &c = f.Tracks[i];
      gaps += c.Pregap;
      CTrack t;
      t.Number = c.Number;
      t.Session = c.Session;
      t.Mode = c.Mode;
      t.Format = c.Format;
      t.Stride = c.Stride;
      t.SwapAudio = c.Swap;
      t.Source = source;
      t.Start = regionByte[i] + (UInt64)(c.Index1 - regionFrame[i]) * c.Stride;
      UInt64 end = i + 1 < f.Tracks.size() ? regionByte[i + 1] : fileSize;
      if (end > fileSize)
        end = fileSize;
      t.NumSectors = end > t.Start ? (UInt32)((end - t.Start) / c.Stride) : 0;
      t.Lba = (UInt32)MyMax((Int64)0, lbaBase + gaps + c.Index1);
      gaps += c.Postgap;
      disc.Tracks.Add(t);
    }
    // the next file starts after this one's last sector
    const CCueTrack &last = f.Tracks.back();
    const Int64 lastFrames = fileSize > regionByte.back() ? (Int64)((fileSize - regionByte.back()) / last.Stride) : 0;
    lbaBase += gaps + regionFrame.back() + lastFrames;
  }
  disc.Comment = L"Tracks from the .cue file";
  return disc.Tracks.IsEmpty() ? S_FALSE : S_OK;
}

// ---------- raw sectors, no descriptor ----------

HRESULT LoadRawSectors(CDisc &disc, IInStream *stream)
{
  UInt64 size;
  RINOK(StreamSize(stream, size))
  const UInt32 n = (UInt32)(size / kRawSectorSize);
  if (n == 0)
    return S_FALSE;
  disc.Sources.Add(CMyComPtr<IInStream>(stream));
  CTrack t;
  t.Number = 1;
  t.Mode = kMode1;
  t.NumSectors = n;
  t.Mode = SniffMode(disc, t, kMode1);
  // the data track ends at the last sector with a sync pattern: binary
  // search, as a data track can't be interrupted by audio
  Byte s[kRawSectorSize];
  bool ok;
  RINOK(disc.ReadSector(t, n - 1, s, ok))
  if (!ok || memcmp(s, kSync, 12) != 0)
  {
    UInt32 lo = 0, hi = n - 1; // lo: data, hi: not
    while (hi - lo > 1)
    {
      const UInt32 mid = lo + (hi - lo) / 2;
      RINOK(disc.ReadSector(t, mid, s, ok))
      if (ok && memcmp(s, kSync, 12) == 0)
        lo = mid;
      else
        hi = mid;
    }
    t.NumSectors = hi;
  }
  disc.Tracks.Add(t);
  if (t.NumSectors < n)
  {
    CTrack a;
    a.Number = 2;
    a.Mode = kAudio;
    a.Start = (UInt64)t.NumSectors * kRawSectorSize;
    a.NumSectors = n - t.NumSectors;
    a.Lba = t.NumSectors;
    disc.Tracks.Add(a);
  }
  disc.Comment = L"No descriptor file: tracks detected from the sectors";
  return S_OK;
}

// ---------- CCD (CloneCD) ----------

struct CCcdEntry
{
  UInt32 Session;
  UInt32 Point;
  Int64 Plba;
  UInt32 Control;
};

HRESULT LoadCcd(CDisc &disc, const CByteBuffer &ccd, IArchiveOpenCallback *callback, IInStream *imgStream)
{
  // [Section] / key=value
  std::vector<std::pair<std::string, std::map<std::string, std::string> > > sections;
  for (const std::string &lineIn : SplitLines(ccd))
  {
    std::string line = lineIn;
    while (!line.empty() && (line[0] == ' ' || line[0] == '\t'))
      line.erase(0, 1);
    if (line.empty())
      continue;
    if (line[0] == '[')
    {
      const size_t e = line.find(']');
      sections.push_back(std::make_pair(Lower(line.substr(1, e == std::string::npos ? std::string::npos : e - 1)),
          std::map<std::string, std::string>()));
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string::npos || sections.empty())
      continue;
    std::string key = Lower(line.substr(0, eq));
    while (!key.empty() && key.back() == ' ')
      key.pop_back();
    std::string value = line.substr(eq + 1);
    while (!value.empty() && value[0] == ' ')
      value.erase(0, 1);
    sections.back().second[key] = value;
  }
  bool isCcd = false;
  std::vector<CCcdEntry> entries;
  std::map<UInt32, std::pair<Int64, Int64> > indices; // track -> INDEX 0, INDEX 1
  std::map<UInt32, int> modes;
  for (auto &sec : sections)
  {
    auto &kv = sec.second;
    if (sec.first == "clonecd")
      isCcd = true;
    else if (sec.first.compare(0, 6, "entry ") == 0)
    {
      CCcdEntry e;
      e.Session = (UInt32)strtoul(kv["session"].c_str(), NULL, 0);
      e.Point = (UInt32)strtoul(kv["point"].c_str(), NULL, 0);
      e.Plba = strtoll(kv["plba"].c_str(), NULL, 0);
      e.Control = (UInt32)strtoul(kv["control"].c_str(), NULL, 0);
      entries.push_back(e);
    }
    else if (sec.first.compare(0, 6, "track ") == 0)
    {
      const UInt32 n = (UInt32)atoi(sec.first.c_str() + 6);
      Int64 i0 = -1, i1 = -1;
      if (kv.count("index 0")) i0 = strtoll(kv["index 0"].c_str(), NULL, 0);
      if (kv.count("index 1")) i1 = strtoll(kv["index 1"].c_str(), NULL, 0);
      indices[n] = std::make_pair(i0, i1);
      if (kv.count("mode"))
        modes[n] = atoi(kv["mode"].c_str());
    }
  }
  if (!isCcd || entries.empty())
    return S_FALSE;

  CMyComPtr<IInStream> img = imgStream;
  if (!img)
  {
    RINOK(OpenSibling(callback, ReplaceExtension(GetArcName(callback), "img"), "img", img))
    if (!img)
      return S_FALSE;
  }
  UInt64 imgSize;
  RINOK(StreamSize(img, imgSize))
  disc.Sources.Add(img);

  std::sort(entries.begin(), entries.end(), [](const CCcdEntry &a, const CCcdEntry &b) {
    if (a.Session != b.Session) return a.Session < b.Session;
    auto rank = [](UInt32 p) { return p == 0xA0 ? 0 : p == 0xA2 ? 2 : 1; };
    if (rank(a.Point) != rank(b.Point)) return rank(a.Point) < rank(b.Point);
    return a.Point < b.Point;
  });
  auto pregapOf = [&](UInt32 track) -> Int64 {
    auto it = indices.find(track);
    if (it == indices.end() || it->second.first < 0 || it->second.second < 0)
      return 0;
    return it->second.second - it->second.first;
  };

  UInt64 running = 0; // .img sector of the current fragment
  for (size_t i = 0; i < entries.size(); i++)
  {
    const CCcdEntry &e = entries[i];
    if (e.Point < 1 || e.Point > 99)
      continue;
    // the next track of the session, or its lead-out
    const CCcdEntry *next = NULL;
    for (size_t j = i + 1; j < entries.size() && !next; j++)
      if (entries[j].Session == e.Session && ((entries[j].Point >= 1 && entries[j].Point <= 99) || entries[j].Point == 0xA2))
        next = &entries[j];
    if (!next)
      return S_FALSE;
    const Int64 pregap = pregapOf(e.Point);
    const Int64 nextPregap = next->Point == 0xA2 ? 0 : pregapOf(next->Point);
    const Int64 fragment = (next->Plba - nextPregap) - (e.Plba - pregap);
    if (fragment < pregap)
      return S_FALSE;
    CTrack t;
    t.Number = e.Point;
    t.Session = e.Session;
    t.Start = (running + (UInt64)pregap) * kRawSectorSize;
    t.NumSectors = (UInt32)(fragment - pregap);
    if (t.Start + (UInt64)t.NumSectors * kRawSectorSize > imgSize)
      t.NumSectors = t.Start < imgSize ? (UInt32)((imgSize - t.Start) / kRawSectorSize) : 0;
    t.Lba = (UInt32)MyMax((Int64)0, e.Plba);
    const ETrackMode def = (e.Control & 4) ? kMode1 : kAudio;
    t.Mode = def == kAudio ? kAudio : SniffMode(disc, t, kMode1);
    if (modes.count(e.Point))
    {
      const int m = modes[e.Point];
      if (m == 0) t.Mode = kAudio;
      else if (m == 1) t.Mode = kMode1;
      else if (m == 2) t.Mode = kMode2;
    }
    disc.Tracks.Add(t);
    running += (UInt64)fragment;
  }
  disc.Comment = L"CloneCD image";
  return disc.Tracks.IsEmpty() ? S_FALSE : S_OK;
}

// ---------- MDS (Alcohol 120%) ----------

static const char kMdsSignature[] = "MEDIA DESCRIPTOR";

HRESULT LoadMds(CDisc &disc, const CByteBuffer &mds, IArchiveOpenCallback *callback,
    IInStream *mdfStream, const UString &mdsName)
{
  const Byte *p = mds;
  const size_t size = mds.Size();
  if (size < 88 || memcmp(p, kMdsSignature, 16) != 0)
    return S_FALSE;
  if (p[16] != 1)
  {
    disc.Comment = L"MDS version 2 (MDX) is not supported";
    return S_FALSE;
  }
  const UInt32 medium = GetLe16(p + 18);
  const bool dvd = medium == 0x10 || medium == 0x12;
  if (!dvd && medium > 2)
    return S_FALSE;
  const UInt32 numSessions = GetLe16(p + 20);
  const UInt32 sessionsOffset = GetLe32(p + 80);
  CMyComPtr<IInStream> mdf = mdfStream;
  UInt64 mdfSize = 0;
  int mdfSource = -1;

  for (UInt32 si = 0; si < numSessions; si++)
  {
    const size_t so = sessionsOffset + (size_t)si * 24;
    if (so + 24 > size)
      return S_FALSE;
    const UInt32 sessionNumber = GetLe16(p + so + 8);
    const UInt32 numBlocks = p[so + 10];
    const UInt32 tracksOffset = GetLe32(p + so + 20);
    for (UInt32 k = 0; k < numBlocks; k++)
    {
      const size_t to = tracksOffset + (size_t)k * 80;
      if (to + 80 > size)
        return S_FALSE;
      const Byte *b = p + to;
      const unsigned point = b[4];
      if (point < 1 || point > 99)
        continue;
      if (mdfSource < 0)
      {
        // the data file: "*.mdf" means the .mds's own name
        if (!mdf)
        {
          const UInt32 numFiles = GetLe32(b + 48);
          const UInt32 footer = GetLe32(b + 52);
          UString name = ReplaceExtension(mdsName, "mdf");
          if (numFiles != 0 && footer + 16 <= size)
          {
            const UInt32 fnOffset = GetLe32(p + footer);
            const bool wide = GetLe32(p + footer + 4) != 0;
            UString fn;
            if (wide)
              for (size_t i = fnOffset; i + 1 < size && (p[i] || p[i + 1]); i += 2)
                fn += (wchar_t)GetLe16(p + i);
            else
              for (size_t i = fnOffset; i < size && p[i]; i++)
                fn += (wchar_t)p[i];
            if (fn.IsPrefixedBy(L"*."))
              name = ReplaceExtension(mdsName, GetAnsiString(fn.Ptr(2)));
            else if (!fn.IsEmpty())
              name = fn;
          }
          RINOK(OpenSibling(callback, name, "mdf", mdf))
          if (!mdf)
            return S_FALSE;
        }
        RINOK(StreamSize(mdf, mdfSize))
        mdfSource = (int)disc.Sources.Size();
        disc.Sources.Add(mdf);
      }
      CTrack t;
      t.Number = point;
      t.Session = sessionNumber;
      t.Source = (unsigned)mdfSource;
      t.Stride = GetLe16(b + 16);
      const unsigned main = b[1] == 0x08 ? t.Stride - 96 : t.Stride;
      switch (main)
      {
        case 2352: t.Format = kRaw2352; break;
        case 2048: t.Format = kCooked2048; break;
        case 2336: t.Format = kMode2_2336; break;
        case 2324: t.Format = kForm2_2324; break;
        default: return S_FALSE;
      }
      switch (b[0] & 7)
      {
        case 1: t.Mode = kAudio; break;
        case 2: t.Mode = kMode1; break;
        case 5: t.Mode = t.Format == kForm2_2324 ? kOther : kMode2; break;
        default: t.Mode = kMode2; break;
      }
      if (t.Mode != kAudio && t.Format == kRaw2352)
        t.Mode = kMode1; // set from the sector below
      t.Start = GetLe64(b + 40);
      t.Lba = GetLe32(b + 36);
      if (dvd)
      {
        t.Mode = kMode1;
        t.NumSectors = mdfSize > t.Start ? (UInt32)((mdfSize - t.Start) / t.Stride) : 0;
        t.Lba = 0;
      }
      else
      {
        const UInt32 extra = GetLe32(b + 12);
        if (extra == 0 || extra + 8 > size)
          return S_FALSE;
        t.NumSectors = GetLe32(p + extra + 4);
      }
      if (t.Mode != kAudio && t.Format == kRaw2352)
        t.Mode = SniffMode(disc, t, (b[0] & 7) == 2 ? kMode1 : kMode2);
      disc.Tracks.Add(t);
    }
  }
  disc.Comment = dvd ? L"Alcohol 120% DVD image" : L"Alcohol 120% CD image";
  return disc.Tracks.IsEmpty() ? S_FALSE : S_OK;
}

// ---------- NRG (Nero) ----------

static bool NrgMode(unsigned code, CTrack &t)
{
  t.Stride = 0;
  switch (code)
  {
    case 0x00: t.Mode = kMode1; t.Format = kCooked2048; break;
    case 0x02: t.Mode = kMode2; t.Format = kCooked2048; break;
    case 0x03: t.Mode = kMode2; t.Format = kMode2_2336; break;
    case 0x05: t.Mode = kMode1; t.Format = kRaw2352; break;
    case 0x06: t.Mode = kMode2; t.Format = kRaw2352; break;
    case 0x07: t.Mode = kAudio; t.Format = kRaw2352; break;
    case 0x0F: t.Mode = kMode1; t.Format = kRaw2352; t.Stride = 2448; break;
    case 0x10: t.Mode = kAudio; t.Format = kRaw2352; t.Stride = 2448; break;
    case 0x11: t.Mode = kMode2; t.Format = kRaw2352; t.Stride = 2448; break;
    default: t.Mode = kAudio; t.Format = kRaw2352; break;
  }
  if (t.Stride == 0)
    t.Stride = SectorFormatSize(t.Format);
  return true;
}

static HRESULT LoadNrg(CDisc &disc, IInStream *stream)
{
  UInt64 size;
  RINOK(StreamSize(stream, size))
  if (size < 16)
    return S_FALSE;
  Byte tail[12];
  RINOK(InStream_SeekSet(stream, size - 12))
  RINOK(ReadStream_FALSE(stream, tail, 12))
  UInt64 trailer;
  bool v2;
  if (memcmp(tail, "NER5", 4) == 0)
  {
    trailer = GetBe64(tail + 4);
    v2 = true;
  }
  else if (memcmp(tail + 4, "NERO", 4) == 0)
  {
    trailer = GetBe32(tail + 8);
    v2 = false;
  }
  else
    return S_FALSE;
  if (trailer >= size || size - trailer > kMaxDescriptorSize)
    return S_FALSE;
  CByteBuffer d((size_t)(size - trailer));
  RINOK(InStream_SeekSet(stream, trailer))
  RINOK(ReadStream_FALSE(stream, d, d.Size()))
  disc.Sources.Add(CMyComPtr<IInStream>(stream));

  // blocks: 4-byte ID, big-endian length, body
  struct CBlock { std::string Id; size_t Body; size_t Len; };
  std::vector<CBlock> blocks;
  for (size_t pos = 0; pos + 8 <= d.Size(); )
  {
    CBlock b;
    b.Id.assign((const char *)(const Byte *)d + pos, 4);
    b.Len = GetBe32(d + pos + 4);
    b.Body = pos + 8;
    if (b.Body + b.Len > d.Size())
      break;
    blocks.push_back(b);
    if (b.Id == "END!")
      break;
    pos = b.Body + b.Len;
  }
  auto nth = [&](const char *id, unsigned n) -> const CBlock * {
    for (const CBlock &b : blocks)
      if (b.Id == id && n-- == 0)
        return &b;
    return NULL;
  };

  UInt32 number = 1;
  for (unsigned session = 0; ; session++)
  {
    const CBlock *cue = nth(v2 ? "CUEX" : "CUES", session);
    const CBlock *dao = nth(v2 ? "DAOX" : "DAOI", session);
    const CBlock *tao = nth(v2 ? "ETN2" : "ETNF", session);
    if (cue && dao)
    {
      // index 01 LBAs from the CUE block, by track
      std::map<unsigned, Int64> index1;
      for (size_t o = 0; o + 8 <= cue->Len; o += 8)
      {
        const Byte *e = d + cue->Body + o;
        const unsigned track = (e[1] >> 4) * 10 + (e[1] & 15);
        const unsigned index = (e[2] >> 4) * 10 + (e[2] & 15);
        Int64 lba;
        if (v2)
          lba = (Int32)GetBe32(e + 4);
        else
          lba = (Int64)(((e[5] >> 4) * 10 + (e[5] & 15)) * 60 + ((e[6] >> 4) * 10 + (e[6] & 15))) * 75
              + ((e[7] >> 4) * 10 + (e[7] & 15)) - 150;
        if (index == 1 && track >= 1 && track <= 99)
          index1[track] = lba;
      }
      const size_t stride = v2 ? 42 : 30;
      unsigned k = 0;
      for (size_t o = 22; o + stride <= dao->Len; o += stride, k++)
      {
        const Byte *e = d + dao->Body + o;
        CTrack t;
        NrgMode(e[14], t);
        const unsigned sectorSize = GetBe16(e + 12);
        if (sectorSize != 0)
          t.Stride = sectorSize;
        const UInt64 start = v2 ? GetBe64(e + 26) : GetBe32(e + 22);
        const UInt64 end = v2 ? GetBe64(e + 34) : GetBe32(e + 26);
        t.Number = number++;
        t.Session = session + 1;
        t.Start = start;
        t.NumSectors = end > start ? (UInt32)((end - start) / t.Stride) : 0;
        // CUE entries are by track number, which continues across sessions
        auto it = index1.find(t.Number);
        t.Lba = it != index1.end() && it->second > 0 ? (UInt32)it->second : 0;
        if (t.Mode != kAudio)
          t.Mode = SniffMode(disc, t, t.Mode);
        disc.Tracks.Add(t);
      }
    }
    else if (tao)
    {
      const size_t stride = v2 ? 32 : 20;
      for (size_t o = 0; o + stride <= tao->Len; o += stride)
      {
        const Byte *e = d + tao->Body + o;
        CTrack t;
        NrgMode(v2 ? e[19] : e[11], t);
        const UInt64 offset = v2 ? GetBe64(e) : GetBe32(e);
        const UInt64 bytes = v2 ? GetBe64(e + 8) : GetBe32(e + 4);
        t.Number = number++;
        t.Session = session + 1;
        t.Start = offset;
        t.NumSectors = (UInt32)(bytes / t.Stride);
        t.Lba = GetBe32(v2 ? e + 20 : e + 12);
        if (t.Mode != kAudio)
          t.Mode = SniffMode(disc, t, t.Mode);
        disc.Tracks.Add(t);
      }
    }
    else
      break;
  }
  disc.PhySize = size;
  disc.Comment = L"Nero image";
  return disc.Tracks.IsEmpty() ? S_FALSE : S_OK;
}

// ---------- GDI (Dreamcast) ----------

static HRESULT LoadGdi(CDisc &disc, const CByteBuffer &gdi, IArchiveOpenCallback *callback)
{
  const std::vector<std::string> lines = SplitLines(gdi);
  size_t li = 0;
  while (li < lines.size() && SplitWords(lines[li]).empty())
    li++;
  if (li >= lines.size())
    return S_FALSE;
  const int count = atoi(lines[li++].c_str());
  if (count <= 0 || count > 99)
    return S_FALSE;
  for (; li < lines.size() && (int)disc.Tracks.Size() < count; li++)
  {
    const std::vector<std::string> w = SplitWords(lines[li]);
    if (w.empty())
      continue;
    if (w.size() < 5)
      return S_FALSE;
    CTrack t;
    t.Number = (UInt32)atoi(w[0].c_str());
    t.Lba = (UInt32)strtoul(w[1].c_str(), NULL, 10);
    const int type = atoi(w[2].c_str());
    t.Stride = (unsigned)atoi(w[3].c_str());
    if (t.Stride == 2352) t.Format = kRaw2352;
    else if (t.Stride == 2048) t.Format = kCooked2048;
    else return S_FALSE;
    t.Mode = type == 4 ? kMode1 : kAudio;
    t.Session = t.Lba >= 45000 ? 2 : 1;
    CMyComPtr<IInStream> s;
    RINOK(OpenSibling(callback, ToUString(w[4]), NULL, s))
    if (!s)
      return S_FALSE;
    UInt64 size;
    RINOK(StreamSize(s, size))
    t.Source = disc.Sources.Size();
    disc.Sources.Add(s);
    t.NumSectors = (UInt32)(size / t.Stride);
    if (t.Mode != kAudio)
      t.Mode = SniffMode(disc, t, kMode1);
    disc.Tracks.Add(t);
  }
  disc.Comment = L"Dreamcast GD-ROM image";
  return (int)disc.Tracks.Size() == count ? S_OK : S_FALSE;
}

// ---------- CDI (DiscJuggler) ----------

struct CCdiReader
{
  const Byte *P;
  size_t Size;
  size_t Pos;
  bool Error;
  CCdiReader(const Byte *p, size_t size): P(p), Size(size), Pos(0), Error(false) {}
  bool Need(size_t n) { if (Pos + n > Size) { Error = true; return false; } return true; }
  void Skip(size_t n) { if (Need(n)) Pos += n; }
  UInt32 U8() { if (!Need(1)) return 0; return P[Pos++]; }
  UInt32 U16() { if (!Need(2)) return 0; const UInt32 v = GetLe16(P + Pos); Pos += 2; return v; }
  UInt32 U32() { if (!Need(4)) return 0; const UInt32 v = GetLe32(P + Pos); Pos += 4; return v; }
  void Header()
  {
    Skip(16);
    const UInt32 fnlen = U8();
    Skip(fnlen);
    Skip(29);
    U16(); // medium type
  }
};

static HRESULT LoadCdi(CDisc &disc, IInStream *stream)
{
  UInt64 size;
  RINOK(StreamSize(stream, size))
  if (size < 8)
    return S_FALSE;
  Byte tail[4];
  RINOK(InStream_SeekSet(stream, size - 4))
  RINOK(ReadStream_FALSE(stream, tail, 4))
  const UInt32 len = GetLe32(tail);
  if (len < 8 || len > size || len > kMaxDescriptorSize)
    return S_FALSE;
  CByteBuffer d(len);
  RINOK(InStream_SeekSet(stream, size - len))
  RINOK(ReadStream_FALSE(stream, d, len))
  disc.Sources.Add(CMyComPtr<IInStream>(stream));

  CCdiReader r(d, len - 4);
  const UInt32 numSessions = r.U8();
  if (numSessions == 0 || numSessions > 99)
    return S_FALSE;
  UInt64 offset = 0;
  Int64 lba = -150;
  UInt32 number = 1;
  for (UInt32 s = 0; s < numSessions && !r.Error; s++)
  {
    r.U8();
    const UInt32 numTracks = r.U8();
    r.Skip(13);
    if (s != 0)
      lba += s == 1 ? 11250 : 6750; // lead-out and lead-in between sessions
    for (UInt32 k = 0; k < numTracks && !r.Error; k++)
    {
      r.Header();
      const UInt32 numIndices = r.U16();
      UInt32 pregap = 0;
      for (UInt32 i = 0; i < numIndices; i++)
      {
        const UInt32 v = r.U32();
        if (i == 0)
          pregap = v;
      }
      const UInt32 numCdText = r.U32();
      for (UInt32 i = 0; i < numCdText && !r.Error; i++)
        for (int f = 0; f < 18; f++)
          r.Skip(r.U8());
      r.Skip(2);
      const UInt32 mode = r.U32();
      r.Skip(4);
      r.U32(); // session index
      r.U32(); // track index
      r.U32(); // start address
      const UInt32 length = r.U32();
      r.Skip(16);
      const UInt32 readMode = r.U32();
      r.U32(); // control
      r.Skip(9 + 12 + 4 + 99);
      if (r.Error)
        break;
      CTrack t;
      t.Number = number++;
      t.Session = s + 1;
      switch (readMode)
      {
        case 0: t.Format = kCooked2048; t.Stride = 2048; break;
        case 1: t.Format = kMode2_2336; t.Stride = 2336; break;
        case 2: t.Format = kRaw2352; t.Stride = 2352; break;
        case 3: t.Format = kRaw2352; t.Stride = 2352 + 16; break;
        case 4: t.Format = kRaw2352; t.Stride = 2352 + 96; break;
        default: return S_FALSE;
      }
      switch (mode)
      {
        case 0: t.Mode = kAudio; break;
        case 1: t.Mode = kMode1; break;
        case 2: t.Mode = kMode2; break;
        default: return S_FALSE;
      }
      if (pregap > length)
        return S_FALSE;
      t.Start = offset + (UInt64)pregap * t.Stride;
      t.NumSectors = length - pregap;
      t.Lba = (UInt32)MyMax((Int64)0, lba + pregap);
      if (t.Mode != kAudio)
        t.Mode = SniffMode(disc, t, t.Mode);
      disc.Tracks.Add(t);
      offset += (UInt64)length * t.Stride;
      lba += length;
    }
  }
  if (r.Error)
    return S_FALSE;
  disc.PhySize = size;
  disc.Comment = L"DiscJuggler image";
  return disc.Tracks.IsEmpty() ? S_FALSE : S_OK;
}

// ---------- handlers ----------

static HRESULT LoadDescriptorFile(IInStream *stream, CByteBuffer &buf)
{
  return ReadWholeStream(stream, buf, kMaxDescriptorSize);
}

namespace NRaw {
// a raw .bin / .img / .mdf, by its first sector; or an .mds
class CHandler Z7_final: public CTrackHandler
{
  HRESULT Open2(IInStream *stream, IArchiveOpenCallback *callback) Z7_override
  {
    Byte h[16];
    RINOK(InStream_SeekToBegin(stream))
    RINOK(ReadStream_FALSE(stream, h, 16))
    const UString name = GetArcName(callback);
    // an Alcohol 120% .mds, which shares this format
    if (memcmp(h, kMdsSignature, 16) == 0)
    {
      CByteBuffer buf;
      RINOK(LoadDescriptorFile(stream, buf))
      RINOK(LoadMds(*_disc, buf, callback, NULL, name))
      _disc->NameTracks(BaseName(name));
      return S_OK;
    }
    if (memcmp(h, kSync, 12) != 0 || (h[15] != 1 && h[15] != 2))
      return S_FALSE;
    static const char * const kDescriptors[] = { "cue", "ccd", "mds" };
    for (unsigned i = 0; i < 3; i++)
    {
      CMyComPtr<IInStream> desc;
      RINOK(OpenSibling(callback, ReplaceExtension(name, kDescriptors[i]), NULL, desc))
      if (!desc)
        continue;
      CByteBuffer buf;
      if (LoadDescriptorFile(desc, buf) != S_OK)
        continue;
      _disc = std::make_shared<CDisc>();
      HRESULT res;
      if (i == 0) res = LoadCue(*_disc, buf, callback, stream, name);
      else if (i == 1) res = LoadCcd(*_disc, buf, callback, stream);
      else res = LoadMds(*_disc, buf, callback, stream, ReplaceExtension(name, "mds"));
      if (res != S_OK && res != S_FALSE)
        return res;
      // the descriptor has to describe this file
      bool ok = res == S_OK;
      FOR_VECTOR (k, _disc->Tracks)
        if (ok && _disc->Tracks[k].Source == 0 && !CheckDataSector(*_disc, _disc->Tracks[k]))
          ok = false;
      if (ok)
      {
        _disc->NameTracks(BaseName(name));
        return S_OK;
      }
    }
    _disc = std::make_shared<CDisc>();
    RINOK(LoadRawSectors(*_disc, stream))
    _disc->NameTracks(BaseName(name));
    return S_OK;
  }
};
// raw sectors, or an .mds: upstream's registry holds 72 formats at most
static const Byte k_Signature[] = {
    12, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0,
    16, 'M', 'E', 'D', 'I', 'A', ' ', 'D', 'E', 'S', 'C', 'R', 'I', 'P', 'T', 'O', 'R' };
REGISTER_ARC_I("CD", "bin img mdf mds", NULL, 0x94, k_Signature, 0, NArcInfoFlags::kMultiSignature, NULL)
}

// .cue, .ccd, .gdi, .nrg and .cdi have no signature, so they share one
// format opened by extension: upstream's registry holds 72 formats at most
namespace NDesc {
class CHandler Z7_final: public CTrackHandler
{
  HRESULT Open2(IInStream *stream, IArchiveOpenCallback *callback) Z7_override
  {
    const UString name = GetArcName(callback);
    UString ext;
    {
      const int dot = name.ReverseFind_Dot();
      if (dot >= 0)
        ext = name.Ptr((unsigned)dot + 1);
      ext.MakeLower_Ascii();
    }
    static const char * const kExts[] = { "cue", "ccd", "gdi", "nrg", "cdi" };
    // the file's own extension first, then the others
    unsigned order[5] = { 0, 1, 2, 3, 4 };
    for (unsigned i = 0; i < 5; i++)
      if (ext.IsEqualTo(kExts[i]))
      {
        order[0] = i;
        order[i] = 0;
      }
    CByteBuffer text;
    for (unsigned k = 0; k < 5; k++)
    {
      const unsigned i = order[k];
      _disc = std::make_shared<CDisc>();
      HRESULT res;
      if (i == 3)
        res = LoadNrg(*_disc, stream);
      else if (i == 4)
        res = LoadCdi(*_disc, stream);
      else
      {
        if (text.Size() == 0)
        {
          res = LoadDescriptorFile(stream, text);
          if (res != S_OK)
            continue;
        }
        if (i == 0) res = LoadCue(*_disc, text, callback, NULL, UString());
        else if (i == 1) res = LoadCcd(*_disc, text, callback, NULL);
        else res = LoadGdi(*_disc, text, callback);
      }
      if (res == S_OK && !_disc->Tracks.IsEmpty())
      {
        _disc->NameTracks(BaseName(name));
        return S_OK;
      }
      if (res != S_OK && res != S_FALSE)
        return res;
    }
    return S_FALSE;
  }
};
REGISTER_ARC_I_NO_SIG("Disc", "cue ccd gdi nrg cdi", NULL, 0x95, 0, NArcInfoFlags::kByExtOnlyOpen, NULL)
}


}}

// Unity.cpp -- 7zfm-qt: Unity engine data, after AssetStudio (MIT license):
//
// UnityFS bundles (.unity3d, .bundle, ...): "UnityFS", a big-endian header,
//   then the block info (where the header says; LZMA or LZ4 compressed):
//   the data's blocks (sizes and compression: none, LZMA, LZ4) and the
//   files in the data (offset, size, flags, path). Older UnityWeb and
//   UnityRaw bundles hold the files' directory in their (LZMA or stored)
//   data. The data is read as one stream through the blocks.
// Serialized files (.assets, level0, globalgamemanagers, a bundle's
//   CAB-...): a header (no signature; big endian, then the file's own
//   byte order), the Unity version, the types, then the objects: path ID,
//   offset, size, type. Player builds strip the type trees, so the fields of
//   the classes exported here are read as AssetStudio reads them for each
//   Unity version.
//
// A bundle lists its objects by type, and its own files under "Files". A
// serialized file lists its objects; their textures and sounds are in the
// .resS / .resource file next to it. Exported as:
//   Texture2D  .tga for 8-, 24- and 32-bit formats, .dds for DXT1, DXT5,
//              BC4 to BC7 (the first mip level; DXT1 and DXT5 turned top
//              down as DDS wants), raw data for the rest (ETC, ASTC...)
//   TextAsset  its bytes, .txt
//   AudioClip  its FMOD sound bank (.fsb), as Unity 5 and later store it
//   others     the object's serialized bytes, .dat

#include "Common/ComTry.h"
#include "Common/MyBuffer.h"
#include "Common/StringConvert.h"
#include "Common/IntToString.h"
#include "Common/UTFConvert.h"

#include "7zip/Common/RegisterArc.h"
#include "7zip/Common/StreamUtils.h"

#include "../common/ItemArchive.h"

#include <map>
#include <string>
#include <string.h>
#include <vector>

namespace NArchive {
namespace NUnity {

using namespace NItemArc;

// ---------- a reader for either byte order ----------

struct CReader
{
  const Byte *P;
  size_t Size;
  size_t Pos;
  bool Be;
  bool Error;

  CReader(const Byte *p, size_t size, bool be): P(p), Size(size), Pos(0), Be(be), Error(false) {}
  bool Need(size_t n) { if (Size - Pos < n || Pos > Size) { Error = true; return false; } return true; }
  UInt64 Num(unsigned n)
  {
    if (!Need(n))
      return 0;
    UInt64 v = 0;
    for (unsigned i = 0; i < n; i++)
      v |= (UInt64)P[Pos + i] << (8 * (Be ? n - 1 - i : i));
    Pos += n;
    return v;
  }
  Byte U8() { return (Byte)Num(1); }
  UInt32 U16() { return (UInt32)Num(2); }
  UInt32 U32() { return (UInt32)Num(4); }
  Int32 I32() { return (Int32)(UInt32)Num(4); }
  UInt64 U64() { return Num(8); }
  void Skip(size_t n) { if (Need(n)) Pos += n; }
  void Align(size_t a) { Pos = (Pos + a - 1) & ~(a - 1); if (Pos > Size) { Pos = Size; Error = true; } }
  AString CStr()
  {
    AString s;
    const Byte *e = (Pos < Size) ? (const Byte *)memchr(P + Pos, 0, Size - Pos) : NULL;
    if (!e) { Error = true; Pos = Size; return s; }
    s.SetFrom((const char *)P + Pos, (unsigned)(e - (P + Pos)));
    Pos = (size_t)(e - P) + 1;
    return s;
  }
  // a Unity string: its length, the bytes, aligned to 4
  bool AlignedString(AString &s, size_t maxLen = 4096)
  {
    const UInt32 len = U32();
    if (Error || len > maxLen || !Need(len))
      return false;
    s.SetFrom((const char *)P + Pos, len);
    Pos += len;
    Align(4);
    return !Error;
  }
};

struct CVersion
{
  int V[3];
  CVersion() { V[0] = V[1] = V[2] = 0; }
  void Parse(const AString &s)
  {
    const char *p = s;
    for (int i = 0; i < 3 && *p; i++)
    {
      int n = 0;
      while (*p >= '0' && *p <= '9')
        n = n * 10 + (*p++ - '0');
      V[i] = n;
      while (*p && !(*p >= '0' && *p <= '9'))
        p++;
    }
  }
  bool AtLeast(int a, int b) const { return V[0] > a || (V[0] == a && V[1] >= b); }
};

static const char *ClassName(Int32 id)
{
  switch (id)
  {
    case 1: return "GameObject";
    case 4: return "Transform";
    case 21: return "Material";
    case 23: return "MeshRenderer";
    case 28: return "Texture2D";
    case 33: return "MeshFilter";
    case 43: return "Mesh";
    case 48: return "Shader";
    case 49: return "TextAsset";
    case 74: return "AnimationClip";
    case 83: return "AudioClip";
    case 84: return "RenderTexture";
    case 89: return "Cubemap";
    case 90: return "Avatar";
    case 91: return "AnimatorController";
    case 95: return "Animator";
    case 114: return "MonoBehaviour";
    case 115: return "MonoScript";
    case 117: return "Texture3D";
    case 128: return "Font";
    case 134: return "PhysicMaterial";
    case 142: return "AssetBundle";
    case 150: return "PreloadData";
    case 152: return "MovieClip";
    case 156: return "TerrainData";
    case 212: return "SpriteRenderer";
    case 213: return "Sprite";
    case 221: return "AnimatorOverrideController";
    case 224: return "RectTransform";
    case 329: return "VideoClip";
    case 687078895: return "SpriteAtlas";
    default: return NULL;
  }
}

// classes whose first field is m_Name (NamedObject)
static bool IsNamed(Int32 id)
{
  switch (id)
  {
    case 21: case 28: case 43: case 48: case 49: case 74: case 83: case 84: case 89: case 90:
    case 91: case 115: case 117: case 128: case 134: case 142: case 152: case 156: case 213:
    case 221: case 329: case 687078895:
      return true;
    default:
      return false;
  }
}

// ---------- texture output ----------

enum EConvert
{
  kConv_None,
  kConv_Rgb24,      // RGB -> BGR (TGA)
  kConv_Rgba32,     // RGBA -> BGRA
  kConv_Argb32,     // ARGB -> BGRA
  kConv_FlipDxt1,   // block rows bottom up -> top down
  kConv_FlipDxt5
};

static void SetUi16(Byte *p, UInt32 v) { p[0] = (Byte)v; p[1] = (Byte)(v >> 8); }
static void SetUi32(Byte *p, UInt32 v) { for (unsigned i = 0; i < 4; i++) p[i] = (Byte)(v >> (8 * i)); }

static void TgaHeader(CByteBuffer &h, UInt32 w, UInt32 hgt, unsigned bpp)
{
  h.Alloc(18);
  memset(h, 0, 18);
  h[2] = (Byte)(bpp == 8 ? 3 : 2);
  SetUi16(h + 12, w);
  SetUi16(h + 14, hgt);
  h[16] = (Byte)bpp;
  h[17] = (Byte)(bpp == 32 ? 8 : 0);   // alpha bits; origin bottom left, as Unity stores
}

static void DdsHeader(CByteBuffer &h, UInt32 w, UInt32 hgt, UInt32 linear, const char *fourcc, UInt32 dxgi)
{
  h.Alloc(4 + 124 + (dxgi ? 20 : 0));
  memset(h, 0, h.Size());
  memcpy(h, "DDS ", 4);
  Byte *d = h + 4;
  SetUi32(d, 124);
  SetUi32(d + 4, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000);
  SetUi32(d + 8, hgt);
  SetUi32(d + 12, w);
  SetUi32(d + 16, linear);
  SetUi32(d + 24, 1);
  SetUi32(d + 72, 32);
  SetUi32(d + 76, 4);
  memcpy(d + 80, dxgi ? "DX10" : fourcc, 4);
  SetUi32(d + 104, 0x1000);
  if (dxgi)
  {
    SetUi32(h + 128, dxgi);
    SetUi32(h + 132, 3);
    SetUi32(h + 140, 1);
  }
}

// flips the first mip level of a DXT1 / DXT5 image (in place)
static void FlipDxt(Byte *p, UInt32 w, UInt32 h, bool dxt5)
{
  const unsigned bs = dxt5 ? 16 : 8;
  const UInt32 bw = (w + 3) / 4, bh = (h + 3) / 4;
  const size_t row = (size_t)bw * bs;
  CByteBuffer tmp(row);
  for (UInt32 y = 0; y < bh / 2; y++)
  {
    memcpy(tmp, p + y * row, row);
    memcpy(p + y * row, p + (bh - 1 - y) * row, row);
    memcpy(p + (bh - 1 - y) * row, tmp, row);
  }
  for (size_t b = 0; b < (size_t)bw * bh; b++)
  {
    Byte *blk = p + b * bs;
    if (dxt5)
    {
      // 16 3-bit alpha indices, 4 rows of 12 bits: reverse the rows
      UInt64 a = 0;
      for (unsigned i = 0; i < 6; i++)
        a |= (UInt64)blk[2 + i] << (8 * i);
      UInt64 r = 0;
      for (unsigned row4 = 0; row4 < 4; row4++)
        r |= ((a >> (12 * row4)) & 0xFFF) << (12 * (3 - row4));
      for (unsigned i = 0; i < 6; i++)
        blk[2 + i] = (Byte)(r >> (8 * i));
      blk += 8;
    }
    // the colour indices: one byte per row
    Byte t = blk[4]; blk[4] = blk[7]; blk[7] = t;
    t = blk[5]; blk[5] = blk[6]; blk[6] = t;
  }
}

// ---------- a serialized file's objects ----------

struct CExport
{
  UString Path;
  bool External;        // in a resource file (.resS, .resource) by ResPath
  AString ResPath;
  UInt64 Offset;        // in the serialized file, or the resource
  UInt64 Size;
  CByteBuffer Header;   // in front of the data
  int Convert;
  UInt32 Width, Height;
  CExport(): External(false), Offset(0), Size(0), Convert(kConv_None), Width(0), Height(0) {}
};

static const UInt32 kMaxObjects = 1 << 21;

// a SerializedFile header: its format version, metadata, data offset, and
// whether the metadata is big endian; false if it isn't one
static bool ParseHeader(const Byte *p, size_t size, UInt64 fileSize, UInt32 &version,
    UInt64 &metaStart, UInt64 &dataOffset, bool &be)
{
  if (size < 20)
    return false;
  UInt32 metaSize = GetBe32(p);
  UInt64 fSize = GetBe32(p + 4);
  version = GetBe32(p + 8);
  dataOffset = GetBe32(p + 12);
  if (version < 9 || version > 30)
    return false;
  if (p[16] > 1 || p[17] != 0 || p[18] != 0 || p[19] != 0)
    return false;
  be = p[16] != 0;
  metaStart = 20;
  if (version >= 22)
  {
    if (size < 48)
      return false;
    metaSize = GetBe32(p + 20);
    fSize = ((UInt64)GetBe32(p + 24) << 32) | GetBe32(p + 28);
    dataOffset = ((UInt64)GetBe32(p + 32) << 32) | GetBe32(p + 36);
    metaStart = 48;
  }
  if (metaSize == 0 || dataOffset < metaStart + metaSize || dataOffset > fSize)
    return false;
  if (fileSize != 0 && fSize != fileSize)
    return false;
  // the Unity version string starts with a digit
  if (size > metaStart && !(p[metaStart] >= '0' && p[metaStart] <= '9'))
    return false;
  return true;
}

API_FUNC_static_IsArc IsArc_Assets(const Byte *p, size_t size)
{
  if (size < 48)
    return k_IsArc_Res_NEED_MORE;
  UInt32 version;
  UInt64 metaStart, dataOffset;
  bool be;
  return ParseHeader(p, size, 0, version, metaStart, dataOffset, be) ? k_IsArc_Res_YES : k_IsArc_Res_NO;
}
}

// reads a serialized file at (base) in (stream), (size) bytes, into exports
static HRESULT ParseSerializedFile(IInStream *stream, UInt64 base, UInt64 size,
    CObjectVector<CExport> &exports, AString &unityVersion)
{
  Byte h[48];
  memset(h, 0, sizeof(h));
  RINOK(ReadAt(stream, base, h, (size_t)MyMin(size, (UInt64)48)))
  UInt32 version;
  UInt64 metaStart, dataOffset;
  bool be;
  if (!ParseHeader(h, (size_t)MyMin(size, (UInt64)48), size, version, metaStart, dataOffset, be))
    return S_FALSE;
  const UInt64 metaSize = dataOffset - metaStart;
  if (metaSize > ((UInt64)1 << 28))
    return S_FALSE;
  CByteBuffer meta((size_t)metaSize);
  RINOK(ReadAt(stream, base + metaStart, meta, meta.Size()))
  CReader r(meta, meta.Size(), be);

  unityVersion = r.CStr();
  CVersion uv;
  uv.Parse(unityVersion);
  const Int32 platform = r.I32();
  bool typeTree = true;
  if (version >= 13)
    typeTree = r.U8() != 0;
  const UInt32 numTypes = r.U32();
  if (r.Error || numTypes > 100000)
    return S_FALSE;
  std::vector<Int32> classOfType;
  for (UInt32 t = 0; t < numTypes && !r.Error; t++)
  {
    const Int32 classID = r.I32();
    classOfType.push_back(classID);
    Int16 scriptIndex = -1;
    if (version >= 16)
      r.U8();
    if (version >= 17)
      scriptIndex = (Int16)r.U16();
    if (version >= 13)
    {
      if ((version < 16 && classID < 0) || (version >= 16 && classID == 114))
        r.Skip(16);
      r.Skip(16);
    }
    (void)scriptIndex;
    if (typeTree)
    {
      if (version >= 12 || version == 10)
      {
        const UInt32 nodes = r.U32();
        const UInt32 strSize = r.U32();
        r.Skip((size_t)nodes * (version >= 19 ? 32 : 24) + strSize);
      }
      else
        return S_FALSE;   // the old nested type tree: not seen in files new enough to matter
      if (version >= 21)
      {
        const UInt32 deps = r.U32();
        r.Skip((size_t)deps * 4);
      }
    }
  }
  bool bigId = false;
  if (version >= 7 && version < 14)
    bigId = r.I32() != 0;
  const UInt32 numObjects = r.U32();
  if (r.Error || numObjects > kMaxObjects)
    return S_FALSE;

  struct CObj { Int64 PathId; UInt64 Start; UInt32 Size; Int32 Class; };
  std::vector<CObj> objs;
  objs.reserve(numObjects);
  for (UInt32 i = 0; i < numObjects && !r.Error; i++)
  {
    CObj o;
    if (bigId)
      o.PathId = (Int64)r.U64();
    else if (version < 14)
      o.PathId = r.I32();
    else
    {
      r.Align(4);
      o.PathId = (Int64)r.U64();
    }
    o.Start = (version >= 22) ? r.U64() : r.U32();
    o.Size = r.U32();
    const Int32 typeId = r.I32();
    if (version < 16)
      o.Class = (Int32)r.U16();
    else
      o.Class = (typeId >= 0 && (size_t)typeId < classOfType.size()) ? classOfType[(size_t)typeId] : -1;
    if (version < 11)
      r.U16();
    if (version >= 11 && version < 17)
      r.U16();
    if (version == 15 || version == 16)
      r.U8();
    o.Start += dataOffset;
    objs.push_back(o);
  }
  if (r.Error)
    return S_FALSE;

  // editor files keep two references in front of every named object
  const unsigned pptrSize = (version >= 14) ? 12 : 8;
  const size_t editorSkip = (platform == -2) ? 2 * pptrSize : 0;

  std::map<std::wstring, unsigned> used;   // UString has no operator< to link
  CByteBuffer ob;
  for (size_t i = 0; i < objs.size(); i++)
  {
    const CObj &o = objs[i];
    if (o.Start > size || o.Size > size - o.Start)
      continue;
    const char *cn = ClassName(o.Class);
    AString typeName;
    if (cn)
      typeName = cn;
    else
    {
      typeName = "Class";
      typeName.Add_UInt32((UInt32)o.Class);
    }
    // the object's start: enough for its name and the fields read here
    const size_t want = (size_t)MyMin((UInt64)o.Size, (UInt64)4096);
    ob.Alloc(want);
    RINOK(ReadAt(stream, base + o.Start, ob, want))
    CReader q(ob, want, be);
    AString name;
    bool haveName = false;
    if (IsNamed(o.Class))
    {
      q.Skip(editorSkip);
      haveName = q.AlignedString(name, 1024);
    }
    else if (o.Class == 114)
    {
      // MonoBehaviour: m_GameObject, m_Enabled, m_Script, then m_Name
      q.Skip(pptrSize);
      q.U8();
      q.Align(4);
      q.Skip(pptrSize);
      haveName = q.AlignedString(name, 1024);
    }

    CExport e;
    e.Offset = o.Start;
    e.Size = o.Size;
    const char *ext = ".dat";

    if (haveName && o.Class == 49)
    {
      // TextAsset: m_Script
      const UInt32 len = q.U32();
      if (!q.Error && (UInt64)q.Pos + len <= o.Size)
      {
        e.Offset = o.Start + q.Pos;
        e.Size = len;
        ext = ".txt";
      }
    }
    else if (haveName && o.Class == 28)
    {
      // Texture2D, as AssetStudio reads it
      if (uv.AtLeast(2017, 3))
      {
        q.I32();
        q.U8();
        if (uv.AtLeast(2020, 2))
          q.U8();
        q.Align(4);
      }
      const UInt32 w = q.U32();
      const UInt32 hgt = q.U32();
      q.I32();                                  // m_CompleteImageSize
      if (uv.V[0] >= 2020)
        q.I32();                                // m_MipsStripped
      const UInt32 fmt = q.U32();
      if (!uv.AtLeast(5, 2))
        q.U8();                                 // m_MipMap
      else
        q.I32();                                // m_MipCount
      if (uv.AtLeast(2, 6))
        q.U8();                                 // m_IsReadable
      if (uv.V[0] >= 2020)
        q.U8();                                 // m_IsPreProcessed
      if (uv.AtLeast(2019, 3))
        q.U8();                                 // m_IgnoreMasterTextureLimit (2022.2: m_IgnoreMipmapLimit)
      if (uv.AtLeast(2022, 2))
      {
        AString group;                          // m_MipmapLimitGroupName
        q.AlignedString(group);
      }
      if (uv.V[0] >= 3 && !uv.AtLeast(5, 5))
        q.U8();                                 // m_ReadAllowed
      if (uv.AtLeast(2018, 2))
        q.U8();                                 // m_StreamingMipmaps
      q.Align(4);
      if (uv.AtLeast(2018, 2))
        q.I32();                                // m_StreamingMipmapsPriority
      q.I32();                                  // m_ImageCount
      q.I32();                                  // m_TextureDimension
      q.Skip(uv.V[0] >= 2017 ? 24 : 16);        // m_TextureSettings
      if (uv.V[0] >= 3)
        q.I32();                                // m_LightmapFormat
      if (uv.AtLeast(3, 5))
        q.I32();                                // m_ColorSpace
      if (uv.AtLeast(2020, 2))
      {
        const UInt32 blob = q.U32();            // m_PlatformBlob
        q.Skip(blob);
        q.Align(4);
      }
      const UInt32 imageSize = q.U32();
      bool ok = !q.Error;
      if (ok && imageSize == 0 && uv.AtLeast(5, 3))
      {
        const UInt64 off = (uv.V[0] >= 2020) ? q.U64() : q.U32();
        const UInt32 sz = q.U32();
        AString path;
        ok = q.AlignedString(path) && !path.IsEmpty();
        e.External = true;
        e.ResPath = path;
        e.Offset = off;
        e.Size = sz;
      }
      else if (ok)
      {
        e.Offset = o.Start + q.Pos;
        e.Size = imageSize;
        ok = (UInt64)q.Pos + imageSize <= o.Size;
      }
      if (ok && w != 0 && hgt != 0 && w <= 16384 && hgt <= 16384)
      {
        const UInt32 px = w * hgt;
        const UInt32 bw = (w + 3) / 4, bh = (hgt + 3) / 4;
        UInt64 need = 0;
        switch (fmt)
        {
          case 1: case 63: TgaHeader(e.Header, w, hgt, 8); need = px; ext = ".tga"; break;      // Alpha8, R8
          case 3: TgaHeader(e.Header, w, hgt, 24); need = (UInt64)px * 3; e.Convert = kConv_Rgb24; ext = ".tga"; break;
          case 4: TgaHeader(e.Header, w, hgt, 32); need = (UInt64)px * 4; e.Convert = kConv_Rgba32; ext = ".tga"; break;
          case 5: TgaHeader(e.Header, w, hgt, 32); need = (UInt64)px * 4; e.Convert = kConv_Argb32; ext = ".tga"; break;
          case 14: TgaHeader(e.Header, w, hgt, 32); need = (UInt64)px * 4; ext = ".tga"; break;   // BGRA32
          case 10: need = (UInt64)bw * bh * 8; DdsHeader(e.Header, w, hgt, (UInt32)need, "DXT1", 0); e.Convert = kConv_FlipDxt1; ext = ".dds"; break;
          case 12: need = (UInt64)bw * bh * 16; DdsHeader(e.Header, w, hgt, (UInt32)need, "DXT5", 0); e.Convert = kConv_FlipDxt5; ext = ".dds"; break;
          case 26: need = (UInt64)bw * bh * 8; DdsHeader(e.Header, w, hgt, (UInt32)need, NULL, 80); ext = ".dds"; break;   // BC4
          case 27: need = (UInt64)bw * bh * 16; DdsHeader(e.Header, w, hgt, (UInt32)need, NULL, 83); ext = ".dds"; break;  // BC5
          case 24: need = (UInt64)bw * bh * 16; DdsHeader(e.Header, w, hgt, (UInt32)need, NULL, 95); ext = ".dds"; break;  // BC6H
          case 25: need = (UInt64)bw * bh * 16; DdsHeader(e.Header, w, hgt, (UInt32)need, NULL, 98); ext = ".dds"; break;  // BC7
          default: ext = ".tex"; break;
        }
        if (need != 0)
        {
          if (need <= e.Size)
          {
            e.Size = need;        // the first mip level
            e.Width = w;
            e.Height = hgt;
          }
          else
          {
            e.Header.Free();
            e.Convert = kConv_None;
            ext = ".tex";
          }
        }
      }
      else
      {
        // fields not as expected for this version: the object as it is
        e.External = false;
        e.ResPath.Empty();
        e.Offset = o.Start;
        e.Size = o.Size;
      }
    }
    else if (haveName && o.Class == 83 && uv.V[0] >= 5)
    {
      // AudioClip: m_Resource, the sound bank in a .resource file
      q.Skip(4 * 4 + 4);                        // load type, channels, frequency, bits, length
      q.U8();
      q.Align(4);
      q.I32();                                  // m_SubsoundIndex
      q.Skip(3);
      q.Align(4);
      AString path;
      if (q.AlignedString(path))
      {
        const UInt64 off = q.U64();
        const UInt64 sz = q.U64();
        if (!q.Error && !path.IsEmpty())
        {
          e.External = true;
          e.ResPath = path;
          e.Offset = off;
          e.Size = sz;
          ext = ".fsb";
        }
        else if (!q.Error && sz <= o.Size)
        {
          e.Offset = o.Start + q.Pos + 4;
          e.Size = sz;
          ext = ".fsb";
        }
      }
    }

    UString u;
    if (haveName && !name.IsEmpty())
    {
      if (!ConvertUTF8ToUnicode(name, u))
        u = GetUnicodeString(name);
      u.Replace(L'/', L'_');
      u.Replace(L'\\', L'_');
    }
    else
    {
      char buf[32];
      ConvertInt64ToString(o.PathId, buf);
      u = buf;
    }
    UString path = GetUnicodeString(typeName);
    path += L'/';
    path += u;
    path += ext;
    // two objects of a name: the second gets its path ID
    if (used[std::wstring(path.Ptr())]++ != 0)
    {
      char buf[32];
      ConvertInt64ToString(o.PathId, buf);
      path = GetUnicodeString(typeName) + L'/' + u + L'_' + GetUnicodeString(buf) + GetUnicodeString(ext);
    }
    e.Path = path;
    exports.Add(e);
  }
  return S_OK;
}

// ---------- the handlers' common part ----------

static const unsigned kMethod_Convert = kMethod_Custom;

class CUnityBase: public CHandlerBase
{
public:
  HRESULT DecodePiece(const CItem &item, const CPiece &piece, CByteBuffer &packed, Byte *dest) Z7_override
  {
    if (piece.Method != kMethod_Convert)
      return CHandlerBase::DecodePiece(item, piece, packed, dest);
    const size_t n = (size_t)piece.Size;
    if (packed.Size() < n)
      return k_Hr_DataError;
    Byte *p = packed;
    const int conv = (int)(piece.Aux & 0xFF);
    const UInt32 w = (UInt32)(piece.Aux >> 8) & 0xFFFFFF, h = (UInt32)(piece.Aux >> 32);
    switch (conv)
    {
      case kConv_Rgb24:
        for (size_t i = 0; i + 2 < n; i += 3) { const Byte t = p[i]; p[i] = p[i + 2]; p[i + 2] = t; }
        break;
      case kConv_Rgba32:
        for (size_t i = 0; i + 3 < n; i += 4) { const Byte t = p[i]; p[i] = p[i + 2]; p[i + 2] = t; }
        break;
      case kConv_Argb32:
        for (size_t i = 0; i + 3 < n; i += 4)
        {
          const Byte a = p[i], rr = p[i + 1], g = p[i + 2], b = p[i + 3];
          p[i] = b; p[i + 1] = g; p[i + 2] = rr; p[i + 3] = a;
        }
        break;
      case kConv_FlipDxt1: FlipDxt(p, w, h, false); break;
      case kConv_FlipDxt5: FlipDxt(p, w, h, true); break;
      default: break;
    }
    memcpy(dest, p, n);
    return S_OK;
  }
protected:
  // adds an export as an item; its data at (dataOffset) in stream (streamIndex)
  void AddExport(const CExport &e, UInt64 dataOffset, Byte streamIndex)
  {
    CItem item;
    item.Path = e.Path;
    if (e.Header.Size() != 0)
    {
      item.Inline = e.Header;
      item.AddPiece(0, e.Header.Size(), e.Header.Size(), kMethod_Inline);
    }
    if (e.Convert != kConv_None)
      item.AddPiece(dataOffset, e.Size, e.Size, kMethod_Convert,
          (UInt64)e.Convert | ((UInt64)e.Width << 8) | ((UInt64)e.Height << 32), streamIndex);
    else
      item.AddPiece(dataOffset, e.Size, e.Size, kMethod_Stored, 0, streamIndex);
    item.Method = "";
    _items.Add(item);
  }
};

static UString LastPart(const AString &path)
{
  UString u = GetUnicodeString(path);
  const int slash = u.ReverseFind(L'/');
  if (slash >= 0)
    u.DeleteFrontal((unsigned)slash + 1);
  return u;
}

// ---------- serialized files ----------

class CAssetsHandler Z7_final: public CUnityBase
{
  HRESULT Open2(IInStream *stream, IArchiveOpenCallback *callback) Z7_override
  {
    UInt64 size = 0;
    RINOK(GetStreamSize(stream, size))
    CObjectVector<CExport> exports;
    AString version;
    const HRESULT res = ParseSerializedFile(stream, 0, size, exports, version);
    if (res != S_OK)
      return res;
    // the .resS / .resource files next to it, each once
    UStringVector resNames;
    FOR_VECTOR (i, exports)
    {
      const CExport &e = exports[i];
      if (!e.External)
      {
        AddExport(e, e.Offset, 0);
        continue;
      }
      const UString name = LastPart(e.ResPath);
      unsigned streamIndex = 0;
      for (unsigned j = 0; j < resNames.Size(); j++)
        if (resNames[j] == name)
          streamIndex = j + 1;
      if (streamIndex == 0)
      {
        CMyComPtr<IInStream> rs;
        RINOK(OpenSiblingFile(callback, name, rs))
        if (!rs || resNames.Size() >= 250)
        {
          _warning = "Missing resource file: ";
          _warning += name;
          continue;
        }
        resNames.Add(name);
        _extraStreams.Add(rs);
        streamIndex = resNames.Size();
      }
      AddExport(e, e.Offset, (Byte)streamIndex);
    }
    _phySize = size;
    _comment = "Unity ";
    _comment += GetUnicodeString(version);
    return S_OK;
  }
};

namespace NAssetsReg {
REGISTER_ARC_I_CLS_NO_SIG(
  CAssetsHandler(),
  "UnityAssets", "assets", NULL, 0xAA,
  0,
  0,
  IsArc_Assets)
}

// ---------- bundles ----------

struct CBlock
{
  UInt64 FileOffset;
  UInt64 PackSize;
  UInt64 Size;
  UInt64 Start;     // in the data
  Byte Method;
  UInt64 Aux;
};

// the data of a bundle: its blocks, decoded as they are read
class CBundleStream Z7_final:
  public IInStream,
  public CMyUnknownImp
{
  Z7_IFACES_IMP_UNK_2(ISequentialInStream, IInStream)
  UInt64 _pos;
  int _cached;
  CByteBuffer _cache;
  CByteBuffer _packed;
public:
  CMyComPtr<IInStream> File;
  std::vector<CBlock> Blocks;
  UInt64 Size;
  CBundleStream(): _pos(0), _cached(-1), Size(0) {}
};

Z7_COM7F_IMF(CBundleStream::Read(void *data, UInt32 size, UInt32 *processedSize))
{
  if (processedSize)
    *processedSize = 0;
  if (size == 0 || _pos >= Size)
    return S_OK;
  size_t lo = 0, hi = Blocks.size();
  while (hi - lo > 1)
  {
    const size_t mid = (lo + hi) / 2;
    if (Blocks[mid].Start <= _pos)
      lo = mid;
    else
      hi = mid;
  }
  const CBlock &b = Blocks[lo];
  const UInt64 in = _pos - b.Start;
  if (in >= b.Size)
    return S_OK;
  if (size > b.Size - in)
    size = (UInt32)(b.Size - in);
  if (b.Method == kMethod_Stored)
  {
    RINOK(InStream_SeekSet(File, b.FileOffset + in))
    UInt32 got = 0;
    RINOK(File->Read(data, size, &got))
    size = got;
  }
  else
  {
    if (_cached != (int)lo)
    {
      _cached = -1;
      if (b.Size > ((UInt64)1 << 32) || b.PackSize > ((UInt64)1 << 32))
        return E_FAIL;
      _packed.Alloc((size_t)b.PackSize);
      RINOK(ReadAt(File, b.FileOffset, _packed, _packed.Size()))
      _cache.Alloc((size_t)b.Size);
      if (CHandlerBase::DecodeMethod(b.Method, b.Aux, _packed, _packed.Size(), _cache, _cache.Size()) != S_OK)
        return E_FAIL;
      _cached = (int)lo;
    }
    memcpy(data, (const Byte *)_cache + (size_t)in, size);
  }
  _pos += size;
  if (processedSize)
    *processedSize = size;
  return S_OK;
}

Z7_COM7F_IMF(CBundleStream::Seek(Int64 offset, UInt32 seekOrigin, UInt64 *newPosition))
{
  switch (seekOrigin)
  {
    case STREAM_SEEK_SET: break;
    case STREAM_SEEK_CUR: offset += (Int64)_pos; break;
    case STREAM_SEEK_END: offset += (Int64)Size; break;
    default: return STG_E_INVALIDFUNCTION;
  }
  if (offset < 0)
    return HRESULT_WIN32_ERROR_NEGATIVE_SEEK;
  _pos = (UInt64)offset;
  if (newPosition)
    *newPosition = _pos;
  return S_OK;
}

static Byte UnityMethod(unsigned type)
{
  switch (type)
  {
    case 0: return kMethod_Stored;
    case 1: return kMethod_Lzma;
    case 2: case 3: return kMethod_Lz4Block;
    default: return 0xFF;
  }
}

class CBundleHandler Z7_final: public CUnityBase
{
  HRESULT Open2(IInStream *stream, IArchiveOpenCallback *callback) Z7_override;
};

struct CNode { UInt64 Offset; UInt64 Size; UInt32 Flags; AString Path; };

HRESULT CBundleHandler::Open2(IInStream *stream, IArchiveOpenCallback *)
{
  UInt64 fileSize = 0;
  RINOK(GetStreamSize(stream, fileSize))
  const size_t hSize = (size_t)MyMin(fileSize, (UInt64)4096);
  CByteBuffer hb(hSize);
  RINOK(ReadAt(stream, 0, hb, hSize))
  CReader r(hb, hSize, true);
  const AString sig = r.CStr();
  const UInt32 format = r.U32();
  const AString unityVersion = r.CStr();
  r.CStr();                                 // revision
  if (r.Error)
    return S_FALSE;

  CBundleStream *bs = new CBundleStream;
  CMyComPtr<IInStream> bsRef = bs;
  bs->File = stream;
  std::vector<CNode> nodes;

  if (sig == "UnityFS")
  {
    r.U64();                                // size
    const UInt32 cInfo = r.U32();
    const UInt32 uInfo = r.U32();
    const UInt32 flags = r.U32();
    if (r.Error || cInfo > ((UInt32)1 << 28) || uInfo > ((UInt32)1 << 28))
      return S_FALSE;
    UInt64 pos = r.Pos;
    if (format >= 7)
      pos = (pos + 15) & ~(UInt64)15;
    UInt64 infoPos = pos;
    if (flags & 0x80)
      infoPos = fileSize - cInfo;
    else
      pos += cInfo;
    CByteBuffer packed(cInfo);
    RINOK(ReadAt(stream, infoPos, packed, cInfo))
    CByteBuffer info(uInfo);
    const Byte m = UnityMethod(flags & 0x3F);
    if (m == 0xFF || CHandlerBase::DecodeMethod(m, 0, packed, cInfo, info, uInfo) != S_OK)
      return S_FALSE;
    if (flags & 0x200)
      pos = (pos + 15) & ~(UInt64)15;
    CReader ir(info, uInfo, true);
    ir.Skip(16);
    const UInt32 numBlocks = ir.U32();
    if (ir.Error || numBlocks > (1 << 24))
      return S_FALSE;
    UInt64 start = 0, filePos = pos;
    for (UInt32 i = 0; i < numBlocks && !ir.Error; i++)
    {
      CBlock b;
      b.Size = ir.U32();
      b.PackSize = ir.U32();
      b.Method = UnityMethod(ir.U16() & 0x3F);
      b.Aux = 0;
      b.FileOffset = filePos;
      b.Start = start;
      if (b.Method == 0xFF)
        return S_FALSE;
      bs->Blocks.push_back(b);
      start += b.Size;
      filePos += b.PackSize;
    }
    bs->Size = start;
    if (filePos > fileSize)
      _errorFlags |= kpv_ErrorFlags_UnexpectedEnd;
    const UInt32 numNodes = ir.U32();
    if (ir.Error || numNodes > (1 << 20))
      return S_FALSE;
    for (UInt32 i = 0; i < numNodes && !ir.Error; i++)
    {
      CNode n;
      n.Offset = ir.U64();
      n.Size = ir.U64();
      n.Flags = ir.U32();
      n.Path = ir.CStr();
      nodes.push_back(n);
    }
    if (ir.Error)
      return S_FALSE;
    _phySize = MyMin(filePos, fileSize);
  }
  else if (sig == "UnityWeb" || sig == "UnityRaw")
  {
    if (format >= 4)
      r.Skip(16 + 4);
    r.U32();                                // minimum streamed bytes
    const UInt32 headerSize = r.U32();
    r.U32();
    const UInt32 levels = r.U32();
    UInt32 cSize = 0, uSize = 0;
    for (UInt32 i = 0; i < levels && !r.Error && i < 1000; i++)
    {
      cSize = r.U32();
      uSize = r.U32();
    }
    if (r.Error || headerSize >= fileSize)
      return S_FALSE;
    CBlock b;
    b.Start = 0;
    b.Size = uSize;
    if (sig == "UnityWeb")
    {
      // .lzma: properties, the size, then the stream
      Byte props[13];
      RINOK(ReadAt(stream, headerSize, props, 13))
      b.Method = kMethod_LzmaRaw;
      b.Aux = props[0] | ((UInt64)GetUi32(props + 1) << 8);
      b.FileOffset = headerSize + 13;
      b.PackSize = MyMin((UInt64)cSize, fileSize - headerSize) - 13;
    }
    else
    {
      b.Method = kMethod_Stored;
      b.Aux = 0;
      b.FileOffset = headerSize;
      b.PackSize = uSize;
    }
    bs->Blocks.push_back(b);
    bs->Size = uSize;
    // the directory, at the start of the data
    const size_t dirMax = (size_t)MyMin((UInt64)uSize, (UInt64)1 << 20);
    CByteBuffer dir(dirMax);
    RINOK(ReadAt(bs, 0, dir, dirMax))
    CReader dr(dir, dirMax, true);
    const UInt32 numNodes = dr.U32();
    if (dr.Error || numNodes > (1 << 16))
      return S_FALSE;
    for (UInt32 i = 0; i < numNodes && !dr.Error; i++)
    {
      CNode n;
      n.Path = dr.CStr();
      n.Offset = dr.U32();
      n.Size = dr.U32();
      n.Flags = 4;
      nodes.push_back(n);
    }
    if (dr.Error)
      return S_FALSE;
    _phySize = fileSize;
  }
  else
    return S_FALSE;

  _dataStream = bs;
  // the bundle's own files
  for (size_t i = 0; i < nodes.size(); i++)
  {
    const CNode &n = nodes[i];
    CItem item;
    UString u;
    if (!ConvertUTF8ToUnicode(n.Path, u))
      u = GetUnicodeString(n.Path);
    item.Path = UString(L"Files/") + u;
    item.SetStored(n.Offset, n.Size);
    if (n.Offset > bs->Size || n.Size > bs->Size - n.Offset)
      _errorFlags |= kpv_ErrorFlags_UnexpectedEnd;
    _items.Add(item);
  }
  // the objects of its serialized files; resources by their file name
  for (size_t i = 0; i < nodes.size(); i++)
  {
    const CNode &n = nodes[i];
    if (n.Offset > bs->Size || n.Size > bs->Size - n.Offset)
      continue;
    CObjectVector<CExport> exports;
    AString v;
    if (ParseSerializedFile(bs, n.Offset, n.Size, exports, v) != S_OK)
      continue;
    FOR_VECTOR (k, exports)
    {
      const CExport &e = exports[k];
      if (!e.External)
      {
        AddExport(e, n.Offset + e.Offset, 0);
        continue;
      }
      const UString want = LastPart(e.ResPath);
      for (size_t j = 0; j < nodes.size(); j++)
        if (LastPart(nodes[j].Path) == want)
        {
          AddExport(e, nodes[j].Offset + e.Offset, 0);
          break;
        }
    }
  }
  _comment = GetUnicodeString(sig);
  _comment += " ";
  _comment += GetUnicodeString(unityVersion);
  return S_OK;
}

static const Byte k_BundleSignature[] =
{
  8, 'U', 'n', 'i', 't', 'y', 'F', 'S', 0,
  9, 'U', 'n', 'i', 't', 'y', 'W', 'e', 'b', 0,
  9, 'U', 'n', 'i', 't', 'y', 'R', 'a', 'w', 0
};

namespace NBundleReg {
REGISTER_ARC_I_CLS(
  CBundleHandler(),
  "UnityFS", "unity3d bundle assetbundle unityweb", NULL, 0xA9,
  k_BundleSignature,
  0,
  NArcInfoFlags::kMultiSignature,
  NULL)
}

}}

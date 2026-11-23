// Bethesda.cpp -- 7zfm-qt: Bethesda Game Studios archives, after the UESP
// wiki's format pages and bethesda-structs (MIT license).
//
// BSA, Morrowind (version 0x100, no signature; opened by extension): file
//   count, then sizes and offsets, name offsets, the names, their hashes,
//   then the data.
// BSA, Oblivion to Skyrim SE ("BSA\0"; version 103, 104, 105): folder
//   records, then for each folder its name and its file records (hash, size,
//   offset), then all file names. Bit 30 of a size flips the archive's
//   "compressed" flag for that file; a compressed file starts with its
//   unpacked size, then zlib (103, 104) or an LZ4 frame (105). From 104, an
//   archive flag puts each file's path in front of its data.
// BA2 ("BTDX"; version 1 and 7, 8 Fallout 4, 2 and 3 Starfield): GNRL
//   archives hold files, zlib-compressed or not (version 3 may say LZ4
//   blocks); DX10 archives hold textures as chunks of mip levels, without
//   their DDS header, which is rebuilt from the record as DirectXTex
//   writes it (the legacy header where the format has one). The names are
//   a table at the end.

#include "Common/ComTry.h"
#include "Common/MyBuffer.h"
#include "Common/StringConvert.h"

#include "7zip/Common/RegisterArc.h"
#include "7zip/Common/StreamUtils.h"

#include "../common/ItemArchive.h"

#include <string.h>

namespace NArchive {
namespace NBethesda {

using namespace NItemArc;

// a compressed BSA file: its unpacked size first; DecodePiece skips it
static const unsigned kMethod_BsaZlib = kMethod_Custom;
static const unsigned kMethod_BsaLz4 = kMethod_Custom + 1;

static UString NameFrom(const char *s, size_t len)
{
  AString a;
  a.SetFrom_CalcLen(s, (unsigned)len);
  a.Replace('\\', '/');
  // names are ASCII or the game's code page
  return MultiByteToUnicodeString(a, CP_ACP);
}

class CBsaHandler Z7_final: public CHandlerBase
{
  HRESULT Open2(IInStream *stream, IArchiveOpenCallback *callback) Z7_override;
  HRESULT OpenMorrowind(IInStream *stream, UInt64 fileSize);
public:
  HRESULT DecodePiece(const CItem &item, const CPiece &piece, CByteBuffer &packed, Byte *dest) Z7_override
  {
    if (piece.Method != kMethod_BsaZlib && piece.Method != kMethod_BsaLz4)
      return CHandlerBase::DecodePiece(item, piece, packed, dest);
    const size_t skip = (size_t)piece.Aux;   // the embedded name, then the size
    if (packed.Size() < skip + 4)
      return k_Hr_DataError;
    return DecodeMethod(piece.Method == kMethod_BsaZlib ? kMethod_Zlib : kMethod_Lz4Frame, 0,
        packed + skip + 4, packed.Size() - skip - 4, dest, (size_t)piece.Size);
  }
};

HRESULT CBsaHandler::OpenMorrowind(IInStream *stream, UInt64 fileSize)
{
  Byte h[12];
  RINOK(ReadAt(stream, 0, h, 12))
  const UInt32 hashOffset = GetUi32(h + 4);
  const UInt32 count = GetUi32(h + 8);
  if (count == 0 || count > (1 << 20) || (UInt64)hashOffset + 12 + (UInt64)count * 8 > fileSize
      || hashOffset < (UInt64)count * 12)
    return S_FALSE;
  CByteBuffer d(hashOffset);
  RINOK(ReadAt(stream, 12, d, hashOffset))
  const UInt64 dataStart = 12 + (UInt64)hashOffset + (UInt64)count * 8;
  const size_t namesStart = (size_t)count * 12;
  for (UInt32 i = 0; i < count; i++)
  {
    const UInt32 size = GetUi32(d + i * 8);
    const UInt32 offset = GetUi32(d + i * 8 + 4);
    const UInt32 nameOffset = GetUi32(d + (size_t)count * 8 + i * 4);
    if (namesStart + nameOffset >= hashOffset)
      return S_FALSE;
    const char *name = (const char *)(const Byte *)d + namesStart + nameOffset;
    CItem item;
    item.Path = NameFrom(name, hashOffset - namesStart - nameOffset);
    item.SetStored(dataStart + offset, size);
    if (dataStart + offset + size > fileSize)
      _errorFlags |= kpv_ErrorFlags_UnexpectedEnd;
    _items.Add(item);
  }
  _comment = "Morrowind";
  _phySize = fileSize;
  return S_OK;
}

HRESULT CBsaHandler::Open2(IInStream *stream, IArchiveOpenCallback *)
{
  UInt64 fileSize = 0;
  RINOK(GetStreamSize(stream, fileSize))
  Byte h[36];
  if (fileSize < 12)
    return S_FALSE;
  RINOK(ReadAt(stream, 0, h, 12))
  if (GetUi32(h) == 0x100)
    return OpenMorrowind(stream, fileSize);
  if (memcmp(h, "BSA\0", 4) != 0 || fileSize < 36)
    return S_FALSE;
  RINOK(ReadAt(stream, 0, h, 36))
  const UInt32 version = GetUi32(h + 4);
  if (version != 103 && version != 104 && version != 105)
    return S_FALSE;
  const UInt32 foldersOffset = GetUi32(h + 8);
  const UInt32 flags = GetUi32(h + 12);
  const UInt32 numFolders = GetUi32(h + 16);
  const UInt32 numFiles = GetUi32(h + 20);
  const UInt32 fileNamesLen = GetUi32(h + 28);
  const bool dirNames = (flags & 1) != 0;
  const bool fileNames = (flags & 2) != 0;
  const bool compressedDefault = (flags & 4) != 0;
  const bool embedNames = version >= 104 && (flags & 0x100) != 0;
  if (numFolders > (1 << 20) || numFiles > (1 << 22))
    return S_FALSE;

  // folder records, then the folders' blocks, then the file names: all
  // before the data; read up to the first file's data
  const unsigned recSize = (version == 105) ? 24 : 16;
  UInt64 blocksEnd = (UInt64)foldersOffset + (UInt64)numFolders * recSize;
  CByteBuffer recs((size_t)numFolders * recSize);
  RINOK(ReadAt(stream, foldersOffset, recs, recs.Size()))
  // the folder blocks: each a bzstring name (if named), then 16 bytes a file
  UInt64 pos = blocksEnd;
  struct CFile { UInt32 Folder; UInt32 Size; UInt32 Offset; };
  CRecordVector<CFile> files;
  UStringVector folderNames;
  for (UInt32 f = 0; f < numFolders; f++)
  {
    const UInt32 count = GetUi32(recs + (size_t)f * recSize + 8);
    UString name;
    if (dirNames)
    {
      Byte len;
      RINOK(ReadAt(stream, pos, &len, 1))
      char buf[256];
      RINOK(ReadAt(stream, pos + 1, buf, len))
      name = NameFrom(buf, len);
      pos += 1 + (UInt64)len;
    }
    folderNames.Add(name);
    if (count > numFiles)
      return S_FALSE;
    CByteBuffer fr((size_t)count * 16);
    RINOK(ReadAt(stream, pos, fr, fr.Size()))
    pos += fr.Size();
    for (UInt32 i = 0; i < count; i++)
    {
      CFile file;
      file.Folder = f;
      file.Size = GetUi32(fr + i * 16 + 8);
      file.Offset = GetUi32(fr + i * 16 + 12);
      files.Add(file);
    }
  }
  if (files.Size() != numFiles)
    _errorFlags |= kpv_ErrorFlags_HeadersError;
  CByteBuffer names;
  if (fileNames)
  {
    names.Alloc(fileNamesLen);
    RINOK(ReadAt(stream, pos, names, fileNamesLen))
  }
  size_t namePos = 0;
  UInt64 end = pos + (fileNames ? fileNamesLen : 0);
  FOR_VECTOR (i, files)
  {
    const CFile &file = files[i];
    CItem item;
    UString name;
    if (fileNames)
    {
      const char *s = (const char *)(const Byte *)names + namePos;
      const size_t rem = names.Size() - namePos;
      const size_t len = strnlen(s, rem);
      name = NameFrom(s, len);
      namePos += (len < rem) ? len + 1 : len;
    }
    else
    {
      name = "file";
      name.Add_UInt32(i);
    }
    const UString &folder = folderNames[file.Folder];
    item.Path = folder.IsEmpty() ? name : folder + L'/' + name;

    const bool compressed = compressedDefault != ((file.Size & 0x40000000) != 0);
    const UInt32 stored = file.Size & 0x3FFFFFFF;
    UInt32 skip = 0;
    if (embedNames && stored > 0)
    {
      Byte len;
      RINOK(ReadAt(stream, file.Offset, &len, 1))
      skip = 1 + (UInt32)len;
      if (skip > stored)
        return S_FALSE;
    }
    if (compressed && stored >= skip + 4)
    {
      Byte s4[4];
      RINOK(ReadAt(stream, (UInt64)file.Offset + skip, s4, 4))
      item.AddPiece(file.Offset, stored, GetUi32(s4),
          version == 105 ? kMethod_BsaLz4 : kMethod_BsaZlib, skip);
      item.Method = version == 105 ? "LZ4" : "Deflate";
    }
    else
      item.SetStored((UInt64)file.Offset + skip, stored - skip);
    if ((UInt64)file.Offset + stored > fileSize)
      _errorFlags |= kpv_ErrorFlags_UnexpectedEnd;
    else if ((UInt64)file.Offset + stored > end)
      end = (UInt64)file.Offset + stored;
    _items.Add(item);
  }
  _phySize = end;
  _comment = (version == 103) ? "Oblivion" : (version == 104) ? "Fallout 3, New Vegas, Skyrim" : "Skyrim Special Edition";
  return S_OK;
}

static const Byte k_BsaSignature[] = { 'B', 'S', 'A', 0 };

namespace NBSAReg {
REGISTER_ARC_I_CLS(
  CBsaHandler(),
  "BSA", "bsa", NULL, 0xB1,
  k_BsaSignature,
  0,
  0,
  NULL)
}

// ---------- BA2 ----------

class CBa2Handler Z7_final: public CHandlerBase
{
  HRESULT Open2(IInStream *stream, IArchiveOpenCallback *callback) Z7_override;
};

// a DXGI format's layout: bits per pixel (or per pixel pair for the packed
// 4:2:2 ones), or the bytes of a 4x4 block for the block-compressed ones
struct CDxgiLayout
{
  unsigned Bits;
  unsigned BlockBytes;
  bool Packed;
};

static bool GetLayout(unsigned fmt, CDxgiLayout &l)
{
  l.Bits = 0;
  l.BlockBytes = 0;
  l.Packed = false;
  switch (fmt)
  {
    case 70: case 71: case 72: case 79: case 80: case 81:      // BC1, BC4
      l.BlockBytes = 8; return true;
    case 73: case 74: case 75: case 76: case 77: case 78:      // BC2, BC3
    case 82: case 83: case 84:                                 // BC5
    case 94: case 95: case 96: case 97: case 98: case 99:      // BC6H, BC7
      l.BlockBytes = 16; return true;
    case 68: case 69: case 107:                                // R8G8_B8G8, G8R8_G8B8, YUY2
      l.Bits = 32; l.Packed = true; return true;
    case 1: case 2: case 3: case 4: l.Bits = 128; return true;
    case 5: case 6: case 7: case 8: l.Bits = 96; return true;
    case 9: case 10: case 11: case 12: case 13: case 14:
    case 15: case 16: case 17: case 18: l.Bits = 64; return true;
    case 19: case 20: case 21: case 22: case 23: case 24: case 25: case 26:
    case 27: case 28: case 29: case 30: case 31: case 32:
    case 33: case 34: case 35: case 36: case 37: case 38:
    case 39: case 40: case 41: case 42: case 43:
    case 44: case 45: case 46: case 47:
    case 67: case 87: case 88: case 89: case 90: case 91: case 92: case 93:
      l.Bits = 32; return true;
    case 48: case 49: case 50: case 51: case 52:
    case 53: case 54: case 55: case 56: case 57: case 58: case 59:
    case 85: case 86: case 115:
      l.Bits = 16; return true;
    case 60: case 61: case 62: case 63: case 64: case 65:
      l.Bits = 8; return true;
  }
  return false;
}

static void SetUi32(Byte *p, UInt32 v) { for (unsigned i = 0; i < 4; i++) p[i] = (Byte)(v >> (8 * i)); }

// the legacy (pre-DX10) pixel format of a DXGI format, where there is one:
// flags, FourCC, bit count and masks, as DirectXTex writes them
struct CLegacyFormat
{
  unsigned Dxgi;
  UInt32 Flags;
  UInt32 FourCC;
  UInt32 Bits, R, G, B, A;
};

#define FCC(a, b, c, d) ((UInt32)(Byte)(a) | ((UInt32)(Byte)(b) << 8) | ((UInt32)(Byte)(c) << 16) | ((UInt32)(Byte)(d) << 24))

static const UInt32 kPf_FourCC = 0x4, kPf_Rgb = 0x40, kPf_Rgba = 0x41, kPf_Lum = 0x20000, kPf_LumA = 0x20001,
    kPf_Alpha = 0x2, kPf_BumpDuDv = 0x80000;

static const CLegacyFormat kLegacyFormats[] =
{
  { 28, kPf_Rgba, 0, 32, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000 },   // R8G8B8A8_UNORM: A8B8G8R8
  { 35, kPf_Rgb, 0, 32, 0x0000ffff, 0xffff0000, 0, 0 },                      // R16G16_UNORM: G16R16
  { 49, kPf_LumA, 0, 16, 0x00ff, 0, 0, 0xff00 },                             // R8G8_UNORM: A8L8
  { 56, kPf_Lum, 0, 16, 0xffff, 0, 0, 0 },                                   // R16_UNORM: L16
  { 61, kPf_Lum, 0, 8, 0xff, 0, 0, 0 },                                      // R8_UNORM: L8
  { 65, kPf_Alpha, 0, 8, 0, 0, 0, 0xff },                                    // A8_UNORM
  { 68, kPf_FourCC, FCC('R','G','B','G'), 0, 0, 0, 0, 0 },
  { 69, kPf_FourCC, FCC('G','R','G','B'), 0, 0, 0, 0, 0 },
  { 107, kPf_FourCC, FCC('Y','U','Y','2'), 0, 0, 0, 0, 0 },
  { 71, kPf_FourCC, FCC('D','X','T','1'), 0, 0, 0, 0, 0 },                   // BC1_UNORM
  { 74, kPf_FourCC, FCC('D','X','T','3'), 0, 0, 0, 0, 0 },                   // BC2_UNORM
  { 77, kPf_FourCC, FCC('D','X','T','5'), 0, 0, 0, 0, 0 },                   // BC3_UNORM
  { 80, kPf_FourCC, FCC('B','C','4','U'), 0, 0, 0, 0, 0 },
  { 81, kPf_FourCC, FCC('B','C','4','S'), 0, 0, 0, 0, 0 },
  { 83, kPf_FourCC, FCC('B','C','5','U'), 0, 0, 0, 0, 0 },
  { 84, kPf_FourCC, FCC('B','C','5','S'), 0, 0, 0, 0, 0 },
  { 85, kPf_Rgb, 0, 16, 0xf800, 0x07e0, 0x001f, 0 },                         // B5G6R5_UNORM: R5G6B5
  { 86, kPf_Rgba, 0, 16, 0x7c00, 0x03e0, 0x001f, 0x8000 },                   // B5G5R5A1_UNORM: A1R5G5B5
  { 115, kPf_Rgba, 0, 16, 0x0f00, 0x00f0, 0x000f, 0xf000 },                  // B4G4R4A4_UNORM: A4R4G4B4
  { 51, kPf_BumpDuDv, 0, 16, 0x00ff, 0xff00, 0, 0 },                         // R8G8_SNORM: V8U8
  { 31, kPf_BumpDuDv, 0, 32, 0x000000ff, 0x0000ff00, 0x00ff0000, 0xff000000 },  // R8G8B8A8_SNORM: Q8W8V8U8
  { 37, kPf_BumpDuDv, 0, 32, 0x0000ffff, 0xffff0000, 0, 0 },                 // R16G16_SNORM: V16U16
  { 87, kPf_Rgba, 0, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000 },   // B8G8R8A8_UNORM: A8R8G8B8
  { 88, kPf_Rgb, 0, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0 },             // B8G8R8X8_UNORM: X8R8G8B8
  // D3DX's float and 16-bit formats, the D3DFMT value as the FourCC
  { 2, kPf_FourCC, 116, 0, 0, 0, 0, 0 },     // R32G32B32A32_FLOAT
  { 10, kPf_FourCC, 113, 0, 0, 0, 0, 0 },    // R16G16B16A16_FLOAT
  { 11, kPf_FourCC, 36, 0, 0, 0, 0, 0 },     // R16G16B16A16_UNORM
  { 13, kPf_FourCC, 110, 0, 0, 0, 0, 0 },    // R16G16B16A16_SNORM
  { 16, kPf_FourCC, 115, 0, 0, 0, 0, 0 },    // R32G32_FLOAT
  { 34, kPf_FourCC, 112, 0, 0, 0, 0, 0 },    // R16G16_FLOAT
  { 41, kPf_FourCC, 114, 0, 0, 0, 0, 0 },    // R32_FLOAT
  { 54, kPf_FourCC, 111, 0, 0, 0, 0, 0 }     // R16_FLOAT
};

// the DDS header of a texture, as DirectXTex writes it: the legacy pixel
// format where the format has one, else the DX10 extension
static void MakeDds(CByteBuffer &dds, unsigned fmt, UInt32 w, UInt32 h, unsigned mips, bool cube)
{
  CDxgiLayout l;
  const bool known = GetLayout(fmt, l);
  const CLegacyFormat *legacy = NULL;
  for (unsigned i = 0; i < Z7_ARRAY_SIZE(kLegacyFormats); i++)
    if (kLegacyFormats[i].Dxgi == fmt)
      legacy = &kLegacyFormats[i];
  const bool dx10 = (legacy == NULL);
  dds.Alloc(4 + 124 + (dx10 ? 20 : 0));
  Byte *p = dds;
  memset(p, 0, dds.Size());
  memcpy(p, "DDS ", 4);
  Byte *hd = p + 4;
  SetUi32(hd, 124);
  const bool bc = (l.BlockBytes != 0);
  // CAPS, HEIGHT, WIDTH, PIXELFORMAT, MIPMAPCOUNT, LINEARSIZE or PITCH
  SetUi32(hd + 4, 0x1 | 0x2 | 0x4 | 0x1000 | (mips > 0 ? 0x20000 : 0) | (bc ? 0x80000 : 0x8));
  SetUi32(hd + 8, h);
  SetUi32(hd + 12, w);
  UInt32 pitch = 0;
  if (known)
  {
    if (bc)
      pitch = MyMax((UInt32)1, (w + 3) / 4) * MyMax((UInt32)1, (h + 3) / 4) * l.BlockBytes;
    else if (l.Packed)
      pitch = ((w + 1) >> 1) * 4;
    else
      pitch = (w * l.Bits + 7) / 8;
  }
  SetUi32(hd + 16, pitch);
  SetUi32(hd + 20, 1);              // depth
  SetUi32(hd + 24, mips);
  Byte *pf = hd + 72;
  SetUi32(pf, 32);
  if (legacy)
  {
    SetUi32(pf + 4, legacy->Flags);
    SetUi32(pf + 8, legacy->FourCC);
    SetUi32(pf + 12, legacy->Bits);
    SetUi32(pf + 16, legacy->R);
    SetUi32(pf + 20, legacy->G);
    SetUi32(pf + 24, legacy->B);
    SetUi32(pf + 28, legacy->A);
  }
  else
  {
    SetUi32(pf + 4, kPf_FourCC);
    memcpy(pf + 8, "DX10", 4);
  }
  // TEXTURE, and COMPLEX with MIPMAP or a cubemap's six faces
  SetUi32(hd + 104, 0x1000 | (mips > 1 ? 0x400008 : 0) | (cube ? 0x8 : 0));
  if (cube)
    SetUi32(hd + 108, 0xFE00);
  if (dx10)
  {
    Byte *x = p + 4 + 124;
    SetUi32(x, fmt);
    SetUi32(x + 4, 3);              // TEXTURE2D
    SetUi32(x + 8, cube ? 4 : 0);
    SetUi32(x + 12, 1);
  }
}

HRESULT CBa2Handler::Open2(IInStream *stream, IArchiveOpenCallback *)
{
  UInt64 fileSize = 0;
  RINOK(GetStreamSize(stream, fileSize))
  Byte h[36];
  if (fileSize < 24)
    return S_FALSE;
  RINOK(ReadAt(stream, 0, h, 24))
  if (memcmp(h, "BTDX", 4) != 0)
    return S_FALSE;
  const UInt32 version = GetUi32(h + 4);
  const bool dx10 = memcmp(h + 8, "DX10", 4) == 0;
  if (!dx10 && memcmp(h + 8, "GNRL", 4) != 0)
    return S_FALSE;
  const UInt32 count = GetUi32(h + 12);
  const UInt64 namesOffset = GetUi64(h + 16);
  UInt64 pos = 24;
  Byte method = kMethod_Zlib;
  if (version == 2 || version == 3)
  {
    pos += 8;
    if (version == 3)
    {
      RINOK(ReadAt(stream, pos, h, 4))
      if (GetUi32(h) == 3)
        method = kMethod_Lz4Block;
      pos += 4;
    }
  }
  else if (version != 1 && version != 7 && version != 8)
    return S_FALSE;
  if (count > (1 << 22) || namesOffset > fileSize)
    return S_FALSE;

  // the names
  UStringVector names;
  if (namesOffset != 0)
  {
    const UInt64 rem = fileSize - namesOffset;
    CByteBuffer nb((size_t)MyMin(rem, (UInt64)1 << 28));
    RINOK(ReadAt(stream, namesOffset, nb, nb.Size()))
    size_t np = 0;
    for (UInt32 i = 0; i < count; i++)
    {
      if (nb.Size() - np < 2)
        break;
      const unsigned len = GetUi16(nb + np);
      np += 2;
      if (nb.Size() - np < len)
        break;
      names.Add(NameFrom((const char *)(const Byte *)nb + np, len));
      np += len;
    }
  }

  UInt64 end = pos;
  for (UInt32 i = 0; i < count; i++)
  {
    CItem item;
    if (i < names.Size())
      item.Path = names[i];
    else
    {
      item.Path = "file";
      item.Path.Add_UInt32(i);
    }
    if (!dx10)
    {
      Byte r[36];
      RINOK(ReadAt(stream, pos, r, 36))
      pos += 36;
      const UInt64 offset = GetUi64(r + 16);
      const UInt32 packed = GetUi32(r + 24);
      const UInt32 size = GetUi32(r + 28);
      if (packed != 0)
        item.AddPiece(offset, packed, size, method);
      else
        item.SetStored(offset, size);
      if (offset > fileSize || item.PackSize > fileSize - offset)
        _errorFlags |= kpv_ErrorFlags_UnexpectedEnd;
      else if (offset + item.PackSize > end)
        end = offset + item.PackSize;
    }
    else
    {
      Byte r[24];
      RINOK(ReadAt(stream, pos, r, 24))
      pos += 24;
      const unsigned numChunks = r[13];
      const UInt32 height = GetUi16(r + 16);
      const UInt32 width = GetUi16(r + 18);
      const unsigned mips = r[20];
      const unsigned fmt = r[21];
      const bool cube = (GetUi16(r + 22) & 1) != 0;
      MakeDds(item.Inline, fmt, width, height, mips, cube);
      item.AddPiece(0, item.Inline.Size(), item.Inline.Size(), kMethod_Inline);
      for (unsigned c = 0; c < numChunks; c++)
      {
        Byte ch[24];
        RINOK(ReadAt(stream, pos, ch, 24))
        pos += 24;
        const UInt64 offset = GetUi64(ch);
        const UInt32 packed = GetUi32(ch + 8);
        const UInt32 size = GetUi32(ch + 12);
        if (packed != 0)
          item.AddPiece(offset, packed, size, method);
        else
          item.AddPiece(offset, size, size, kMethod_Stored);
        if (offset > fileSize || (packed ? packed : size) > fileSize - offset)
          _errorFlags |= kpv_ErrorFlags_UnexpectedEnd;
        else if (offset + (packed ? packed : size) > end)
          end = offset + (packed ? packed : size);
      }
    }
    _items.Add(item);
  }
  if (namesOffset != 0)
    end = fileSize;
  _phySize = MyMin(end, fileSize);
  _comment = dx10 ? "textures (DX10)" : "general (GNRL)";
  _comment += ", version ";
  _comment.Add_UInt32(version);
  return S_OK;
}

static const Byte k_Ba2Signature[] = { 'B', 'T', 'D', 'X' };

namespace NBA2Reg {
REGISTER_ARC_I_CLS(
  CBa2Handler(),
  "BA2", "ba2", NULL, 0xB2,
  k_Ba2Signature,
  0,
  0,
  NULL)
}

}}

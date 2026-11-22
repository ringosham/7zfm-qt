// ItemArchive.h -- 7zfm-qt: the shared part of the formats that are an
// index of named files in one stream (game archives, installers): the
// handler only reads the index into a list of items, this does the rest.
//
// An item is one or more pieces, read one after another: stored bytes at an
// offset, a compressed block (zlib, raw deflate, LZ4 block or frame, zstd,
// LZMA, bzip2), or bytes the handler holds itself (a header it rebuilt, a
// prefix). Each item opens as a seekable stream (IInArchiveGetStream), so
// an archive inside opens in place; a compressed piece is decoded whole
// when the stream reaches it. Extract checks a CRC-32 or Adler-32 when the
// index gives one.
//
// A handler with its own coding (a cipher, an XOR key, a method of its own)
// overrides DecodePiece and/or TransformStored.

#ifndef Z7QT_ITEM_ARCHIVE_H
#define Z7QT_ITEM_ARCHIVE_H

#include "Common/MyBuffer.h"
#include "Common/MyCom.h"
#include "Common/MyString.h"
#include "Common/MyVector.h"

#include "Windows/PropVariant.h"

#include "7zip/Archive/IArchive.h"

namespace NArchive {
namespace NItemArc {

enum EMethod
{
  kMethod_Stored,
  kMethod_Inline,     // Offset is into CItem::Inline
  kMethod_Zlib,
  kMethod_Deflate,    // raw
  kMethod_Lz4Block,
  kMethod_Lz4Frame,
  kMethod_Zstd,
  kMethod_Lzma,       // 5 bytes of properties, then the raw stream
  kMethod_LzmaRaw,    // raw stream; properties in CPiece::Aux (lc/lp/pb byte | dict << 8)
  kMethod_Bzip2,
  kMethod_Custom      // the handler's DecodePiece does it
};

enum ECheck
{
  kCheck_None,
  kCheck_Crc32,
  kCheck_Adler32
};

struct CPiece
{
  UInt64 Offset;
  UInt64 PackSize;
  UInt64 Size;        // unpacked
  Byte Method;
  UInt64 Aux;         // the handler's
};

struct CItem
{
  UString Path;       // '/' separated
  UInt64 Size;
  UInt64 PackSize;
  CRecordVector<CPiece> Pieces;
  CByteBuffer Inline;
  bool IsDir;
  bool Encrypted;
  bool MTime_Defined;
  FILETIME MTime;
  Byte CheckType;
  UInt32 Check;
  AString Method;     // shown; if empty, the first piece's method
  UInt64 Aux;         // the handler's

  CItem(): Size(0), PackSize(0), IsDir(false), Encrypted(false), MTime_Defined(false),
      CheckType(kCheck_None), Check(0), Aux(0)
    { MTime.dwLowDateTime = MTime.dwHighDateTime = 0; }

  // adds a piece and counts it in Size and PackSize
  void AddPiece(UInt64 offset, UInt64 packSize, UInt64 size, Byte method, UInt64 aux = 0);
  // a stored item: one piece
  void SetStored(UInt64 offset, UInt64 size) { Pieces.Clear(); Size = PackSize = 0; AddPiece(offset, size, size, kMethod_Stored); }
  void SetMTime_Unix(Int64 t);
};

// DecodePiece results besides S_OK
const HRESULT k_Hr_DataError = S_FALSE;
const HRESULT k_Hr_Unsupported = E_NOTIMPL;
const HRESULT k_Hr_WrongPassword = (HRESULT)0x8007FF01;

class CHandlerBase:
  public IInArchive,
  public IInArchiveGetStream,
  public CMyUnknownImp
{
  Z7_IFACES_IMP_UNK_2(IInArchive, IInArchiveGetStream)
protected:
  CObjectVector<CItem> _items;
  CMyComPtr<IInStream> _stream;
  CMyComPtr<IInStream> _dataStream;   // the items' data, if not in _stream (another file)
  UInt64 _phySize;
  UInt32 _errorFlags;   // kpv_ErrorFlags_*
  UString _error;       // kpidError
  UString _warning;
  UString _comment;     // kpidComment: version and the like

  virtual HRESULT Open2(IInStream *stream, IArchiveOpenCallback *callback) = 0;
  virtual void Close2() {}
public:
  CHandlerBase(): _phySize(0), _errorFlags(0) {}
  virtual ~CHandlerBase() {}

  // (dest) gets piece.Size bytes of a piece that isn't stored or inline;
  // (packed) holds its piece.PackSize bytes and may be changed (decrypted)
  // first. S_OK, or k_Hr_DataError / k_Hr_Unsupported / k_Hr_WrongPassword.
  virtual HRESULT DecodePiece(const CItem &item, const CPiece &piece, CByteBuffer &packed, Byte *dest);
  // stored bytes of (item), read at offset (pos) into the item's piece
  // (piece), before they are handed out
  virtual void TransformStored(const CItem & /* item */, unsigned /* piece */, UInt64 /* pos */,
      Byte * /* data */, size_t /* size */) {}

  // for the handlers: decodes by method (no handler hook)
  static HRESULT DecodeMethod(Byte method, UInt64 aux, const Byte *src, size_t srcSize, Byte *dest, size_t destSize);
  IInStream *Stream() const { return _stream; }
  IInStream *DataStream() const { return _dataStream ? _dataStream : _stream; }
  HRESULT GetItemStream(unsigned index, CMyComPtr<IInStream> &stream);
};

const char *MethodName(Byte method);

// reads (size) bytes at (offset); S_FALSE if the stream ends first
HRESULT ReadAt(IInStream *stream, UInt64 offset, void *data, size_t size);
HRESULT GetStreamSize(IInStream *stream, UInt64 &size);
// the archive's own name (no folder), if the opener says
UString GetArcName(IArchiveOpenCallback *callback);
// a file next to the archive, by name (no stream if there is none)
HRESULT OpenSiblingFile(IArchiveOpenCallback *callback, const UString &name, CMyComPtr<IInStream> &stream);
// asks the opener for a password (S_FALSE: none given)
HRESULT AskPassword(IArchiveOpenCallback *callback, UString &password);

UInt32 GetUi16(const Byte *p);
UInt32 GetUi32(const Byte *p);
UInt64 GetUi64(const Byte *p);
UInt32 GetBe32(const Byte *p);

}}

#endif

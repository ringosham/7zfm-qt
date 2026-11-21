// DiscImage.h -- 7zfm-qt: the shared part of the disc image formats that
// upstream 7-Zip lacks (the formats of the Iso7z plugin for Windows):
//
//   track images  CUE/BIN, CCD/IMG, MDS/MDF, NRG, GDI, CDI, CHD
//   containers    CSO/ZSO, CISO, ISZ, ECM, zisofs
//
// A track image is a list of tracks over one or more source streams. Each
// track is an item: a data track reads as its 2048-byte payloads (".iso",
// opened in place by upstream's ISO, UDF or HFS handler), an audio track as
// a WAV file, anything else as its raw sectors (".bin"). A data track that
// doesn't start at LBA 0 and holds an ISO 9660 volume addressed by disc LBA
// (a later session, a GD-ROM's high-density area) reads as the disc from
// LBA 0, as its file system's sector numbers expect, with the track's own
// head in front so that its volume descriptors are where an ISO reader
// looks.
//
// A container (DiscContainers.cpp) holds one image, compressed or with
// parts left out; it is one item, read through a block cache, that upstream
// opens right away (kpidMainSubfile), as it does a VHD.

#ifndef Z7QT_DISC_IMAGE_H
#define Z7QT_DISC_IMAGE_H

#include "Common/MyBuffer.h"
#include "Common/MyCom.h"
#include "Common/MyString.h"
#include "Common/MyVector.h"

#include "7zip/Archive/IArchive.h"

#include <memory>

namespace NArchive {
namespace NDisc {

const unsigned kRawSectorSize = 2352;
const unsigned kDataSize = 2048;
const UInt32 kFramesPerSecond = 75;

extern const Byte kSync[12];

// ---------- sectors ----------

enum ETrackMode
{
  kAudio,
  kMode1,
  kMode2,       // XA form 1 data is what an .iso view takes
  kOther        // read as raw sectors
};

// where a sector's 2352 (or fewer) main bytes are and what they hold
enum ESectorFormat
{
  kRaw2352,     // sync, header, data, EDC/ECC
  kCooked2048,  // user data only
  kMode2_2336,  // from the subheader on
  kForm2_2324,  // form 2 user data only
  kWhole        // Stride bytes as they are (a hard disk's sectors)
};

unsigned SectorFormatSize(ESectorFormat f);

UInt32 Edc(const Byte *p, size_t size, UInt32 edc = 0);
// false only for a raw data sector whose stored EDC doesn't match
bool EdcIsOk(const Byte *sector, ETrackMode mode);
// fills in P and Q parity of a raw 2352-byte sector (mode 1, or mode 2
// form 1 with its header treated as zero)
void EccGenerate(Byte *sector);
// fills in EDC and ECC of a raw sector from its sync, header and data
void EdcEccGenerate(Byte *sector);

UInt32 GetBe16(const Byte *p);
UInt32 GetBe32(const Byte *p);
UInt64 GetBe64(const Byte *p);
UInt32 GetLe16(const Byte *p);
UInt32 GetLe32(const Byte *p);
UInt64 GetLe64(const Byte *p);
void SetLe32(Byte *p, UInt32 v);

// ---------- track images ----------

struct CTrack
{
  UInt32 Number;
  UInt32 Session;
  ETrackMode Mode;
  ESectorFormat Format;
  unsigned Source;      // index into CDisc::Sources
  UInt64 Start;         // byte offset of the track's first sector (INDEX 01)
  unsigned Stride;      // bytes from one sector to the next (subchannel too)
  UInt32 NumSectors;
  UInt32 Lba;           // of INDEX 01
  bool SwapAudio;       // big-endian samples (CHD)
  bool LbaView;         // the .iso reads as the disc from LBA 0 (DecideViews)
  UString Name;

  CTrack(): Number(0), Session(1), Mode(kOther), Format(kRaw2352), Source(0),
      Start(0), Stride(kRawSectorSize), NumSectors(0), Lba(0), SwapAudio(false), LbaView(false) {}

  bool IsData() const { return Mode == kMode1 || Mode == kMode2; }
  // where the 2048 data bytes are in a sector's main bytes
  unsigned DataOffset() const;
  unsigned MainSize() const { return Format == kWhole ? Stride : SectorFormatSize(Format); }
};

// a source of sectors (a file, or a CHD's decompressed hunks)
typedef CMyComPtr<IInStream> CSource;

class CDisc
{
public:
  CObjectVector<CSource> Sources;
  CObjectVector<CTrack> Tracks;
  UInt64 PhySize;
  UString Comment;
  // set by a source whose data fails its check (a CHD hunk's CRC)
  std::shared_ptr<bool> SourceError;
  // a single track that isn't data (a hard disk) opens too
  bool OpenSingleTrack;

  CDisc(): PhySize(0), SourceError(std::make_shared<bool>(false)), OpenSingleTrack(false) {}
  void NameTracks(const UString &baseName);
  // a data track not at LBA 0 whose ISO 9660 volume gives its directories'
  // sectors as LBAs on the disc (a later session, a GD-ROM's high-density
  // area) reads from LBA 0; one with sectors of its own (BeOS's BFS tracks)
  // reads as itself
  void DecideViews();
  // the main bytes of sector (index) of a track; false past the source's end
  HRESULT ReadSector(const CTrack &t, UInt32 index, Byte *main, bool &ok);
  UInt64 ItemSize(unsigned trackIndex) const;
  // the track whose data covers (lba), or -1
  int FindDataTrack(UInt32 lba) const;
};

// the stream of an item of a track image
class CTrackStream;
CTrackStream *NewTrackStream(const std::shared_ptr<CDisc> &disc, unsigned trackIndex);
bool TrackStream_EdcError(const CTrackStream *s);
bool TrackStream_ReadError(const CTrackStream *s);
void TrackStream_SetCheckEdc(CTrackStream *s);

// IInArchive over a CDisc; a format's handler fills the disc in Open2()
class CTrackHandler:
  public IInArchive,
  public IInArchiveGetStream,
  public CMyUnknownImp
{
  Z7_IFACES_IMP_UNK_2(IInArchive, IInArchiveGetStream)
protected:
  std::shared_ptr<CDisc> _disc;
  CMyComPtr<IInStream> _stream;
  virtual HRESULT Open2(IInStream *stream, IArchiveOpenCallback *callback) = 0;
public:
  virtual ~CTrackHandler() {}
};

// ---------- opening helpers ----------

// the file name the archive was opened as (no folder), if the caller says
UString GetArcName(IArchiveOpenCallback *callback);
UString ReplaceExtension(const UString &name, const char *ext);
UString BaseName(const UString &name);
// a file next to the archive, through IArchiveOpenVolumeCallback; tries the
// name as given, then in lower and upper case, then (fallbackExt) on the
// archive's own base name. NULL if none opens.
HRESULT OpenSibling(IArchiveOpenCallback *callback, const UString &name,
    const char *fallbackExt, CMyComPtr<IInStream> &stream);
HRESULT ReadWholeStream(IInStream *stream, CByteBuffer &buf, size_t maxSize);
HRESULT StreamSize(IInStream *stream, UInt64 &size);

// descriptors read by more than one handler (DiscTracks.cpp)
HRESULT LoadCue(CDisc &disc, const CByteBuffer &cue, IArchiveOpenCallback *callback,
    IInStream *mainStream, const UString &mainName);
HRESULT LoadCcd(CDisc &disc, const CByteBuffer &ccd, IArchiveOpenCallback *callback,
    IInStream *imgStream);
HRESULT LoadMds(CDisc &disc, const CByteBuffer &mds, IArchiveOpenCallback *callback,
    IInStream *mdfStream, const UString &mdsName);
// a raw .bin with no descriptor: the data track, then audio
HRESULT LoadRawSectors(CDisc &disc, IInStream *stream);
// the image an ECM file holds, as a stream (DiscContainers.cpp)
HRESULT OpenEcm(IInStream *stream, IArchiveOpenCallback *callback, CMyComPtr<IInStream> &decoded);

// ---------- containers ----------

// one decompressed image, read block by block
class CBlockHandler:
  public IInArchive,
  public IInArchiveGetStream,
  public CMyUnknownImp
{
  Z7_IFACES_IMP_UNK_2(IInArchive, IInArchiveGetStream)
protected:
  CMyComPtr<IInStream> _stream;
  UInt64 _size;           // of the image
  UInt32 _blockSize;
  UInt64 _phySize;
  UString _name;          // the item's
  const char *_method;
  bool _dataError;

  virtual HRESULT Open2(IInStream *stream, IArchiveOpenCallback *callback) = 0;
  virtual void Close2() {}
public:
  CBlockHandler(): _size(0), _blockSize(0), _phySize(0), _method(NULL), _dataError(false) {}
  virtual ~CBlockHandler() {}
  // block (index) of the image, _blockSize bytes (less for the last one);
  // S_FALSE for data that doesn't decode
  virtual HRESULT ReadBlock(UInt64 index, Byte *dest) = 0;
  UInt64 Size() const { return _size; }
  UInt32 BlockSize() const { return _blockSize; }
};

// inflate (raw deflate with windowBits -15, zlib with 15); S_FALSE on bad data
HRESULT Inflate(const Byte *src, size_t srcSize, Byte *dest, size_t destSize, int windowBits, size_t *written = NULL);

}}

#endif

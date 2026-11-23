// FatUpdate.h -- 7zfm-qt: adding, deleting and renaming files in a FAT
// image (see FatUpdate.cpp)

#ifndef Z7QT_FAT_UPDATE_H
#define Z7QT_FAT_UPDATE_H

#include "Common/MyBuffer.h"
#include "Common/MyVector.h"

#include "7zip/Archive/IArchive.h"

namespace NArchive {
namespace NFat {

// what the reader found, as the updater needs it
struct CFatVolume
{
  unsigned SectorSizeLog;
  unsigned ClusterSizeLog;
  unsigned NumFatBits;
  unsigned NumFats;
  UInt32 NumReservedSectors;
  UInt32 NumFatSectors;
  UInt32 RootDirSector;
  UInt32 NumRootDirSectors;   // 0 on FAT32
  UInt32 DataSector;
  UInt32 NumSectors;
  UInt32 FatSize;             // clusters + 2
  UInt32 BadCluster;
  UInt32 RootCluster;         // FAT32
  UInt32 FsInfoSector;        // FAT32, 0 if none
  const UInt32 *Fat;          // as read (bit 31: a directory's cluster)

  bool HasLabel;
  char Label[11];
  UInt32 LabelTime;

  struct CItem
  {
    char DosName[11];
    Byte Flags;               // NT case flags
    Byte Attrib;
    Byte CTime2;
    UInt16 ADate;
    UInt32 CTime;
    UInt32 MTime;
    UInt32 Cluster;
    UInt32 Size;
    Int32 Parent;
    CByteBuffer LongName;     // UTF-16LE with its NUL, or empty
  };
  CObjectVector<CItem> Items;
};

HRESULT UpdateFat(IInStream *inStream, const CFatVolume &vol, IInArchive *handler,
    ISequentialOutStream *outStream, UInt32 numItems, IArchiveUpdateCallback *callback);

}}

#endif

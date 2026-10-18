// ProgressSync.h -- CProgressSync from upstream FileManager/ProgressDialog2.h,
// copied without the Win32 dialog. Worker threads write progress here; the
// Qt ProgressDialog polls it on a timer, exactly as 7zFM's dialog does.

#ifndef Z7QT_PROGRESS_SYNC_H
#define Z7QT_PROGRESS_SYNC_H

#include "Common/Common.h"
#include "Common/MyString.h"
#include "Windows/Synchronization.h"

struct CProgressMessageBoxPair
{
  UString Title;
  UString Message;
};

struct CProgressFinalMessage
{
  CProgressMessageBoxPair ErrorMessage;
  CProgressMessageBoxPair OkMessage;

  bool ThereIsMessage() const { return !ErrorMessage.Message.IsEmpty() || !OkMessage.Message.IsEmpty(); }
};

class CProgressSync
{
  bool _stopped;
  bool _paused;
public:
  bool _filesProgressMode;
  bool _isDir;
  UInt64 _totalBytes;
  UInt64 _completedBytes;
  UInt64 _totalFiles;
  UInt64 _curFiles;
  UInt64 _inSize;
  UInt64 _outSize;

  UString _titleFileName;
  UString _status;
  UString _filePath;

  UStringVector Messages;
  CProgressFinalMessage FinalMessage;

  NWindows::NSynchronization::CCriticalSection _cs;

  CProgressSync();

  bool Get_Stopped()
  {
    NWindows::NSynchronization::CCriticalSectionLock lock(_cs);
    return _stopped;
  }
  void Set_Stopped(bool val)
  {
    NWindows::NSynchronization::CCriticalSectionLock lock(_cs);
    _stopped = val;
  }

  bool Get_Paused();
  void Set_Paused(bool val)
  {
    NWindows::NSynchronization::CCriticalSectionLock lock(_cs);
    _paused = val;
  }

  void Set_FilesProgressMode(bool filesProgressMode)
  {
    NWindows::NSynchronization::CCriticalSectionLock lock(_cs);
    _filesProgressMode = filesProgressMode;
  }

  HRESULT CheckStop();
  void Clear_Stop_Status();
  HRESULT ScanProgress(UInt64 numFiles, UInt64 totalSize, const FString &fileName, bool isDir = false);

  HRESULT Set_NumFilesTotal(UInt64 val);
  void Set_NumBytesTotal(UInt64 val);
  void Set_NumFilesCur(UInt64 val);
  HRESULT Set_NumBytesCur(const UInt64 *val);
  HRESULT Set_NumBytesCur(UInt64 val);
  void Set_Ratio(const UInt64 *inSize, const UInt64 *outSize);

  void Set_TitleFileName(const UString &fileName);
  void Set_Status(const UString &s);
  HRESULT Set_Status2(const UString &s, const wchar_t *path, bool isDir = false);
  void Set_FilePath(const wchar_t *path, bool isDir = false);

  void AddError_Message(const wchar_t *message);
  void AddError_Message_Name(const wchar_t *message, const wchar_t *name);
  void AddError_Code_Name(HRESULT systemError, const wchar_t *name);

  bool ThereIsMessage() const { return !Messages.IsEmpty() || FinalMessage.ThereIsMessage(); }
};

#define PROGRESS_UNDEFINED_VAL ((UInt64)(Int64)-1)

#endif

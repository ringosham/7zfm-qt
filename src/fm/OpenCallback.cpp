// OpenCallback.cpp -- port of upstream FileManager/OpenCallback.cpp.

#include "OpenCallback.h"

HRESULT COpenArchiveCallback::Open_SetTotal(const UInt64 *numFiles, const UInt64 *numBytes)
{
  RINOK(Sync.CheckStop())
  Sync.Set_NumFilesTotal(numFiles ? *numFiles : (UInt64)(Int64)-1);
  Sync.Set_FilesProgressMode(numFiles != NULL);
  if (numBytes)
    Sync.Set_NumBytesTotal(*numBytes);
  return S_OK;
}

HRESULT COpenArchiveCallback::Open_SetCompleted(const UInt64 *numFiles, const UInt64 *numBytes)
{
  if (numFiles)
    Sync.Set_NumFilesCur(*numFiles);
  if (numBytes)
    Sync.Set_NumBytesCur(*numBytes);
  return Sync.CheckStop();
}

HRESULT COpenArchiveCallback::Open_CheckBreak()
{
  return Sync.CheckStop();
}

HRESULT COpenArchiveCallback::Open_Finished()
{
  return Sync.CheckStop();
}

#ifndef Z7_NO_CRYPTO
HRESULT COpenArchiveCallback::Open_CryptoGetTextPassword(BSTR *password)
{
  PasswordWasAsked = true;
  if (!PasswordIsDefined)
  {
    // 7zfm-qt: upstream shows CPasswordDialog here (and remembers the
    // "show password" checkbox itself); the Qt dialog does both.
    if (!Ui || !Ui->AskPassword(Password))
      return E_ABORT;
    PasswordIsDefined = true;
  }
  return StringToBstr(Password, password);
}
#endif

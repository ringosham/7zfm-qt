// OpenCallback.h -- port of upstream FileManager/OpenCallback.h.
// The CProgressDialog member becomes a CProgressSync plus UI hooks.

#ifndef Z7QT_OPEN_CALLBACK_H
#define Z7QT_OPEN_CALLBACK_H

#include "7zip/UI/Common/ArchiveOpenCallback.h"

#include "ProgressSync.h"
#include "UiHooks.h"

class COpenArchiveCallback Z7_final:
  public IOpenCallbackUI
{
public:
  bool PasswordIsDefined;
  bool PasswordWasAsked;
  UString Password;
  IUiHooks *Ui;
  CProgressSync Sync;

  Z7_IFACE_IMP(IOpenCallbackUI)

  COpenArchiveCallback():
      PasswordIsDefined(false),
      PasswordWasAsked(false),
      Ui(NULL)
  {}
};

#endif

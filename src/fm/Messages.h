// Messages.h -- message helpers copied from upstream FileManager.

#ifndef Z7QT_MESSAGES_H
#define Z7QT_MESSAGES_H

#include "Common/Common.h"
#include "Common/MyString.h"

UString HResultToMessage(HRESULT errorCode);
UString GetOpenArcErrorMessage(UInt32 errorFlags);

#endif

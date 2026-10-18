// UserNames.h -- cached uid/gid -> name lookups for the file list.

#ifndef Z7QT_USER_NAMES_H
#define Z7QT_USER_NAMES_H

#include "Common/MyString.h"
#include <sys/types.h>

UString GetUserName(uid_t uid);
UString GetGroupName(gid_t gid);

#endif

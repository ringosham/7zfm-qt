// SevenZipG.h -- 7zG's command line (GUI.cpp): "7zfm-qt <command> ...".

#ifndef Z7QT_SEVEN_ZIP_G_H
#define Z7QT_SEVEN_ZIP_G_H

#include "Common/MyString.h"

// true for the commands 7zG runs: a d u (add), e x t (extract, test),
// h (hash), b (benchmark), in any case
bool Is7zGCommand(const UString &s);

// 7zG's Main2 on (commandStrings), the arguments without the program name;
// errors are shown as 7zG shows them. Returns 7-Zip's exit code.
int Run7zG(UStringVector &commandStrings);

#endif

// CompressCall.h -- port of upstream FileManager/CompressCall.h, in-process
// variant (upstream CompressCall2.cpp): no 7zG.exe child process.

#ifndef Z7QT_COMPRESS_CALL_H
#define Z7QT_COMPRESS_CALL_H

#include "Common/MyString.h"

UString GetQuotedString(const UString &s);

HRESULT CompressFiles(
    const UString &arcPathPrefix,
    const UString &arcName,
    const UString &arcType,
    bool addExtension,
    const UStringVector &names,
    bool email, bool showDialog, bool waitFinish);

void Benchmark(bool totalMode);
HRESULT CalculateCrcFs(const UString &methodName, const UString &basePrefix, const UStringVector &relPaths);

void ExtractArchives(const UStringVector &arcPaths, const UString &outFolder, bool showDialog, bool elimDup, UInt32 writeZone);
void TestArchives(const UStringVector &arcPaths, bool hashMode = false);

#endif

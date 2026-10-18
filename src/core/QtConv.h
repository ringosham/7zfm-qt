// Conversions between 7-Zip strings (UString = wchar_t, FString = UTF-8
// bytes on Linux) and Qt strings.

#ifndef Z7QT_QT_CONV_H
#define Z7QT_QT_CONV_H

#include "Common/Common.h"
#include "Common/MyString.h"
#include "Common/StringConvert.h"

#include <QString>

inline QString ToQ(const wchar_t *s) { return s ? QString::fromWCharArray(s) : QString(); }
inline QString ToQ(const UString &s) { return QString::fromWCharArray(s.Ptr(), (qsizetype)s.Len()); }
inline QString FsToQ(const FString &s) { return ToQ(fs2us(s)); }

inline UString ToU(const QString &s)
{
  UString u;
  const qsizetype n = s.size();
  wchar_t *p = u.GetBuf((unsigned)n * 2 + 1);
  const qsizetype len = s.toWCharArray(p);
  u.ReleaseBuf_SetEnd((unsigned)len);
  return u;
}
inline FString QToFs(const QString &s) { return us2fs(ToU(s)); }

#endif

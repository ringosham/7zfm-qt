// NWindows::NRegistry::CKey on top of QSettings, so upstream's settings code
// (UI/Common/ZipRegistry.cpp, FileManager/RegistryUtils.cpp,
// FileManager/ViewSettings.cpp) runs unchanged. Keys keep the Windows layout
// ("Software/7-Zip/Compression/Options/7z/...") inside an INI file at
// ~/.config/7zfm-qt/7zfm-qt.conf.

#include "compat/Win32Compat.h"

#include "Common/Common.h"
#include "Common/StringConvert.h"
#include "Windows/Registry.h"

#include <QMutex>
#include <QSettings>
#include <QStringList>

struct Z7RegKey
{
  QString Path;
};

static Z7RegKey g_RootKey;
HKEY const Z7_HKEY_CURRENT_USER = &g_RootKey;

static QMutex g_Mutex;

namespace {

struct CStore
{
  QMutexLocker<QMutex> Lock;
  QSettings S;
  CStore():
      Lock(&g_Mutex),
      S(QSettings::NativeFormat, QSettings::UserScope, QStringLiteral("7zfm-qt"), QStringLiteral("7zfm-qt"))
      {}
};

}

static QString JoinPath(const QString &parent, const QString &name)
{
  QString n = name;
  n.replace(QLatin1Char('\\'), QLatin1Char('/'));
  if (parent.isEmpty())
    return n;
  if (n.isEmpty())
    return parent;
  return parent + QLatin1Char('/') + n;
}

static QString Q(LPCTSTR s) { return s ? QString::fromUtf8(s) : QString(); }
static QString Q(LPCWSTR s) { return s ? QString::fromWCharArray(s) : QString(); }

static UString U(const QString &s)
{
  UString u;
  const std::wstring w = s.toStdWString();
  u = w.c_str();
  return u;
}

static bool GroupExists(QSettings &s, const QString &path)
{
  if (path.isEmpty())
    return true;
  s.beginGroup(path);
  const bool exists = !s.childKeys().isEmpty() || !s.childGroups().isEmpty();
  s.endGroup();
  return exists;
}

namespace NWindows {
namespace NRegistry {

LONG CKey::Create(HKEY parentKey, LPCTSTR keyName, LPTSTR, DWORD, REGSAM,
    LPSECURITY_ATTRIBUTES, LPDWORD disposition) throw()
{
  if (!parentKey)
    return ERROR_INVALID_DATA;
  Z7RegKey *key = new Z7RegKey;
  key->Path = JoinPath(parentKey->Path, Q(keyName));
  if (disposition)
    *disposition = 0;
  Close();
  _object = key;
  return ERROR_SUCCESS;
}

LONG CKey::Open(HKEY parentKey, LPCTSTR keyName, REGSAM) throw()
{
  if (!parentKey)
    return ERROR_INVALID_DATA;
  const QString path = JoinPath(parentKey->Path, Q(keyName));
  {
    CStore st;
    if (!GroupExists(st.S, path))
      return ERROR_FILE_NOT_FOUND;
  }
  Z7RegKey *key = new Z7RegKey;
  key->Path = path;
  Close();
  _object = key;
  return ERROR_SUCCESS;
}

LONG CKey::Close() throw()
{
  if (_object && _object != &g_RootKey)
    delete _object;
  _object = NULL;
  return ERROR_SUCCESS;
}

LONG CKey::DeleteSubKey(LPCTSTR subKeyName) throw()
{
  return RecurseDeleteKey(subKeyName);
}

LONG CKey::RecurseDeleteKey(LPCTSTR subKeyName) throw()
{
  CStore st;
  st.S.remove(JoinPath(_object->Path, Q(subKeyName)));
  return ERROR_SUCCESS;
}

static LONG RemoveValue(HKEY k, const QString &name)
{
  CStore st;
  const QString key = JoinPath(k->Path, name);
  if (!st.S.contains(key))
    return ERROR_FILE_NOT_FOUND;
  st.S.remove(key);
  return ERROR_SUCCESS;
}

static LONG Put(HKEY k, const QString &name, const QVariant &v)
{
  CStore st;
  st.S.setValue(JoinPath(k->Path, name), v);
  return ERROR_SUCCESS;
}

static bool Get(HKEY k, const QString &name, QVariant &v)
{
  CStore st;
  const QString key = JoinPath(k->Path, name);
  if (!st.S.contains(key))
    return false;
  v = st.S.value(key);
  return true;
}

LONG CKey::DeleteValue(LPCTSTR name) throw() { return RemoveValue(_object, Q(name)); }
LONG CKey::DeleteValue(LPCWSTR name) { return RemoveValue(_object, Q(name)); }

LONG CKey::SetValue(LPCTSTR name, UInt32 value) throw() { return Put(_object, Q(name), (uint)value); }
LONG CKey::SetValue(LPCTSTR name, bool value) throw() { return Put(_object, Q(name), (uint)(value ? 1 : 0)); }
LONG CKey::SetValue(LPCTSTR name, LPCTSTR value) throw() { return Put(_object, Q(name), Q(value)); }
LONG CKey::SetValue(LPCWSTR name, LPCWSTR value) { return Put(_object, Q(name), Q(value)); }

LONG CKey::SetValue(LPCTSTR name, const void *value, UInt32 size) throw()
{
  return Put(_object, Q(name), QByteArray((const char *)value, (qsizetype)size));
}

LONG SetValue(HKEY parentKey, LPCTSTR keyName, LPCTSTR valueName, LPCTSTR value)
{
  CKey key;
  const LONG res = key.Create(parentKey, keyName);
  if (res != ERROR_SUCCESS)
    return res;
  return key.SetValue(valueName, value);
}

LONG CKey::SetKeyValue(LPCTSTR keyName, LPCTSTR valueName, LPCTSTR value) throw()
{
  CKey key;
  const LONG res = key.Create(_object, keyName);
  if (res != ERROR_SUCCESS)
    return res;
  return key.SetValue(valueName, value);
}

LONG CKey::GetValue_UInt32_IfOk(LPCTSTR name, UInt32 &value) throw()
{
  QVariant v;
  if (!Get(_object, Q(name), v))
    return ERROR_FILE_NOT_FOUND;
  bool ok = false;
  const uint u = v.toUInt(&ok);
  if (!ok)
    return ERROR_INVALID_DATA;
  value = (UInt32)u;
  return ERROR_SUCCESS;
}

LONG CKey::GetValue_UInt64_IfOk(LPCTSTR name, UInt64 &value) throw()
{
  QVariant v;
  if (!Get(_object, Q(name), v))
    return ERROR_FILE_NOT_FOUND;
  bool ok = false;
  const qulonglong u = v.toULongLong(&ok);
  if (!ok)
    return ERROR_INVALID_DATA;
  value = (UInt64)u;
  return ERROR_SUCCESS;
}

LONG CKey::GetValue_bool_IfOk(LPCTSTR name, bool &value) throw()
{
  UInt32 v;
  const LONG res = GetValue_UInt32_IfOk(name, v);
  if (res == ERROR_SUCCESS)
    value = (v != 0);
  return res;
}

LONG CKey::QueryValue(LPCTSTR name, CSysString &value)
{
  value.Empty();
  QVariant v;
  if (!Get(_object, Q(name), v))
    return ERROR_FILE_NOT_FOUND;
  value = v.toString().toUtf8().constData();
  return ERROR_SUCCESS;
}

LONG CKey::QueryValue(LPCWSTR name, UString &value)
{
  value.Empty();
  QVariant v;
  if (!Get(_object, Q(name), v))
    return ERROR_FILE_NOT_FOUND;
  value = U(v.toString());
  return ERROR_SUCCESS;
}

LONG CKey::QueryValue_Binary(LPCTSTR name, CByteBuffer &value)
{
  value.Free();
  QVariant v;
  if (!Get(_object, Q(name), v))
    return ERROR_FILE_NOT_FOUND;
  const QByteArray a = v.toByteArray();
  value.CopyFrom((const Byte *)a.constData(), (size_t)a.size());
  return ERROR_SUCCESS;
}

LONG CKey::EnumKeys(CSysStringVector &keyNames)
{
  keyNames.Clear();
  CStore st;
  st.S.beginGroup(_object->Path);
  const QStringList groups = st.S.childGroups();
  st.S.endGroup();
  for (const QString &g : groups)
    keyNames.Add(CSysString(g.toUtf8().constData()));
  return ERROR_SUCCESS;
}

// Upstream stores these as a REG_BINARY of NUL-terminated wchar_t strings;
// a QStringList is the natural (and readable) INI form.
LONG CKey::SetValue_Strings(LPCTSTR valueName, const UStringVector &strings)
{
  QStringList list;
  FOR_VECTOR (i, strings)
    list.append(QString::fromWCharArray(strings[i].Ptr(), (qsizetype)strings[i].Len()));
  return Put(_object, Q(valueName), list);
}

LONG CKey::GetValue_Strings(LPCTSTR valueName, UStringVector &strings)
{
  strings.Clear();
  QVariant v;
  if (!Get(_object, Q(valueName), v))
    return ERROR_FILE_NOT_FOUND;
  const QStringList list = v.toStringList();
  for (const QString &s : list)
    strings.Add(U(s));
  return ERROR_SUCCESS;
}

}}

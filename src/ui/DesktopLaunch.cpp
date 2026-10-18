// DesktopLaunch.cpp -- see DesktopLaunch.h.

#include "DesktopLaunch.h"

#include <QDesktopServices>
#include <QFile>
#include <QHash>
#include <QProcess>
#include <QStandardPaths>
#include <QStringList>
#include <QUrl>

namespace DesktopLaunch {

bool OpenWithDefaultApp(const QString &path)
{
  const QString xdgOpen = QStandardPaths::findExecutable(QStringLiteral("xdg-open"));
  if (!xdgOpen.isEmpty())
    return QProcess::startDetached(xdgOpen, {path});
  return QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

bool RunCommand(const QString &command, const QString &path)
{
  QStringList args = QProcess::splitCommand(command);
  if (args.isEmpty())
    return false;
  const QString prog = args.takeFirst();
  args << path;
  return QProcess::startDetached(prog, args);
}

// ---------- desktop entries (XDG Desktop Entry spec) ----------

static QString FindDesktopFile(const QString &id)
{
  QString file = QStandardPaths::locate(QStandardPaths::ApplicationsLocation, id);
  if (file.isEmpty() && id.contains(QLatin1Char('-')))
  {
    // "kde4-foo.desktop" may be "kde4/foo.desktop"
    QString sub = id;
    file = QStandardPaths::locate(QStandardPaths::ApplicationsLocation,
        sub.replace(QLatin1Char('-'), QLatin1Char('/')));
  }
  return file;
}

// key=value pairs of one group, without the localized keys
static QHash<QString, QString> ReadGroup(const QString &file, const QString &group)
{
  QHash<QString, QString> values;
  QFile f(file);
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
    return values;
  bool inGroup = false;
  while (!f.atEnd())
  {
    const QString line = QString::fromUtf8(f.readLine()).trimmed();
    if (line.startsWith(QLatin1Char('[')))
    {
      inGroup = (line == QLatin1Char('[') + group + QLatin1Char(']'));
      continue;
    }
    if (!inGroup || line.startsWith(QLatin1Char('#')))
      continue;
    const int eq = line.indexOf(QLatin1Char('='));
    if (eq <= 0)
      continue;
    const QString key = line.left(eq).trimmed();
    if (!values.contains(key))
      values.insert(key, line.mid(eq + 1).trimmed());
  }
  return values;
}

// the escapes of the "string" value type: \s \n \t \r \\ .
static QString UnescapeValue(const QString &v)
{
  QString r;
  for (int i = 0; i < v.size(); i++)
  {
    if (v[i] != QLatin1Char('\\') || i + 1 == v.size())
    {
      r += v[i];
      continue;
    }
    const QChar c = v[++i];
    if (c == QLatin1Char('s')) r += QLatin1Char(' ');
    else if (c == QLatin1Char('n')) r += QLatin1Char('\n');
    else if (c == QLatin1Char('t')) r += QLatin1Char('\t');
    else if (c == QLatin1Char('r')) r += QLatin1Char('\r');
    else r += c;
  }
  return r;
}

// Exec quoting: arguments split on spaces, "..." quotes with \ escapes inside
static QStringList SplitExec(const QString &exec)
{
  QStringList args;
  QString cur;
  bool inQuote = false, have = false;
  for (int i = 0; i < exec.size(); i++)
  {
    const QChar c = exec[i];
    if (inQuote)
    {
      if (c == QLatin1Char('\\') && i + 1 < exec.size())
        cur += exec[++i];
      else if (c == QLatin1Char('"'))
        inQuote = false;
      else
        cur += c;
    }
    else if (c == QLatin1Char('"'))
      inQuote = have = true;
    else if (c.isSpace())
    {
      if (have)
        args << cur;
      cur.clear();
      have = false;
    }
    else
    {
      cur += c;
      have = true;
    }
  }
  if (have)
    args << cur;
  return args;
}

static bool LaunchDesktopFile(const QString &desktopFile, const QString &path)
{
  const QHash<QString, QString> e = ReadGroup(desktopFile, QStringLiteral("Desktop Entry"));
  // a terminal program would need a terminal emulator around it
  if (e.value(QStringLiteral("Terminal")) == QLatin1String("true"))
    return false;
  const QStringList exec = SplitExec(UnescapeValue(e.value(QStringLiteral("Exec"))));
  if (exec.isEmpty())
    return false;

  QStringList args;
  bool fileUsed = false;
  for (const QString &a : exec)
  {
    if (a == QLatin1String("%i"))
    {
      const QString icon = e.value(QStringLiteral("Icon"));
      if (!icon.isEmpty())
        args << QStringLiteral("--icon") << icon;
      continue;
    }
    QString r;
    for (int i = 0; i < a.size(); i++)
    {
      if (a[i] != QLatin1Char('%') || i + 1 == a.size())
      {
        r += a[i];
        continue;
      }
      const QChar c = a[++i];
      if (c == QLatin1Char('f') || c == QLatin1Char('F'))
      {
        r += path;
        fileUsed = true;
      }
      else if (c == QLatin1Char('u') || c == QLatin1Char('U'))
      {
        r += QUrl::fromLocalFile(path).toString();
        fileUsed = true;
      }
      else if (c == QLatin1Char('c'))
        r += e.value(QStringLiteral("Name"));
      else if (c == QLatin1Char('k'))
        r += desktopFile;
      else if (c == QLatin1Char('%'))
        r += QLatin1Char('%');
      // other (deprecated) field codes expand to nothing
    }
    if (!r.isEmpty() || a.isEmpty())
      args << r;
  }
  if (!fileUsed)
    args << path;
  const QString prog = args.takeFirst();
  return QProcess::startDetached(prog, args);
}

// the default application for a MIME type (XDG mime-apps spec lookup order)
static QString DefaultApplication(const QString &mimeType)
{
  QStringList names;
  for (const QString &d : qEnvironmentVariable("XDG_CURRENT_DESKTOP").toLower()
      .split(QLatin1Char(':'), Qt::SkipEmptyParts))
    names << d + QStringLiteral("-mimeapps.list");
  names << QStringLiteral("mimeapps.list");
  const QStringList dirs = QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation)
      + QStandardPaths::standardLocations(QStandardPaths::ApplicationsLocation);
  for (const QString &dir : dirs)
    for (const QString &name : names)
    {
      const QString ids = ReadGroup(dir + QLatin1Char('/') + name,
          QStringLiteral("Default Applications")).value(mimeType);
      for (const QString &id : ids.split(QLatin1Char(';'), Qt::SkipEmptyParts))
      {
        const QString file = FindDesktopFile(id.trimmed());
        if (!file.isEmpty())
          return file;
      }
    }
  return QString();
}

bool OpenInTextEditor(const QString &path)
{
  const QString desktopFile = DefaultApplication(QStringLiteral("text/plain"));
  if (!desktopFile.isEmpty() && LaunchDesktopFile(desktopFile, path))
    return true;
  return OpenWithDefaultApp(path);
}

}

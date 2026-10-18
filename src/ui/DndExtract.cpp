// DndExtract.cpp -- see DndExtract.h.

#include "DndExtract.h"

#include <QDBusConnection>

DndExtract::DndExtract(QObject *parent): QObject(parent)
{
  static int counter = 0;
  QDBusConnection bus = QDBusConnection::sessionBus();
  if (!bus.isConnected())
    return;
  const QString path = QStringLiteral("/DndExtract/%1").arg(++counter);
  if (bus.registerObject(path, this, QDBusConnection::ExportScriptableSlots))
    _path = path;
}

QString DndExtract::Service() const
{
  return QDBusConnection::sessionBus().baseService();
}

void DndExtract::extractSelectedFilesTo(const QString &path)
{
  emit ExtractRequested(path);
}

// DndExtract.h -- the D-Bus side of dragging out of an archive.
//
// KIO's DropJob (Dolphin, the Plasma desktop) shows a Copy/Move/Link menu for
// every drop of URLs. Ark avoids the menu with a small protocol: the drag
// carries a D-Bus service name and object path, and the drop target calls
// org.kde.ark.DndExtract.extractSelectedFilesTo(destination) instead of
// handling the URLs. 7zfm-qt exports the same object, so a drop out of an
// archive is a plain copy, as in 7zFM. Other drop targets still use the URLs.

#pragma once

#include <QObject>
#include <QString>

class DndExtract : public QObject
{
  Q_OBJECT
  Q_CLASSINFO("D-Bus Interface", "org.kde.ark.DndExtract")
public:
  static const char *ServiceMimeType() { return "application/x-kde-ark-dndextract-service"; }
  static const char *PathMimeType() { return "application/x-kde-ark-dndextract-path"; }

  explicit DndExtract(QObject *parent);
  bool IsRegistered() const { return !_path.isEmpty(); }
  QString Service() const;
  QString Path() const { return _path; }

public slots:
  // returns at once (the caller waits for the reply); the copy is queued
  Q_SCRIPTABLE void extractSelectedFilesTo(const QString &path);

signals:
  void ExtractRequested(const QString &path);

private:
  QString _path;
};

// DesktopLaunch.h -- start other applications on a file, as a file manager does.
//
// QDesktopServices::openUrl is not used for this: under Plasma the platform
// theme routes it through the xdg-desktop-portal OpenURI call, which shows an
// application chooser even for file types that already have a default.

#pragma once

#include <QString>

namespace DesktopLaunch {

// The file's default application (xdg-open), like a double click in the
// desktop's file manager.
bool OpenWithDefaultApp(const QString &path);

// The desktop's default text editor (the text/plain default in
// mimeapps.list): what notepad.exe is to upstream's F4.
bool OpenInTextEditor(const QString &path);

// A user-configured command line (Options > Editor), with the file appended.
bool RunCommand(const QString &command, const QString &path);

}

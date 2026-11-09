// ShellCommand.h -- the commands of upstream's Explorer context menu, run
// from the command line by the file managers' menu entries (data/menus).

#ifndef Z7QT_SHELL_COMMAND_H
#define Z7QT_SHELL_COMMAND_H

#include <QStringList>

// (args) is the command line without the program name. If its first argument
// is one of the commands below, runs it on the files that follow and returns
// true with the exit status in (exitCode); otherwise returns false.
//
//   --extract FILES...        Extract files... (the extract dialog)
//   --extract-here FILES...   Extract Here
//   --extract-to FILES...     Extract to "NAME/" (to "*/" for several archives)
//   --test FILES...           Test archive
//   --add FILES...            Add to archive... (the compress dialog)
//   --add-7z FILES...         Add to "NAME.7z"
//   --add-zip FILES...        Add to "NAME.zip"
//   --hash METHOD FILES...    CRC SHA: CRC32 CRC64 XXH64 MD5 SHA1 SHA256
//                             SHA384 SHA512 SHA3-256 BLAKE2sp, or * for all
//   --hash-file FILES...      CRC SHA > SHA-256 -> NAME.sha256
bool RunShellCommand(const QStringList &args, int &exitCode);

bool IsShellCommand(const QString &arg);

#endif

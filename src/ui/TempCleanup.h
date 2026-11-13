// TempCleanup.h -- removes the 7zO.* folders (Panel::CreateTempDir) that an
// instance killed or crashed before it could delete them.

#ifndef Z7QT_TEMP_CLEANUP_H
#define Z7QT_TEMP_CLEANUP_H

// Starts a background thread that, unless another 7zfm-qt process is
// running, deletes the 7zO.* folders in the cache directory that are older
// than this call. Call it after the application name is set and before the
// first temp folder is made. Errors are ignored.
void StartTempCleanup();

#endif

// WinPathZip.h -- 7zfm-qt: '\' as a folder separator in zip files made on
// Windows (see WinPathZip.cpp).

#ifndef Z7QT_WIN_PATH_ZIP_H
#define Z7QT_WIN_PATH_ZIP_H

class CCodecs;

// After CCodecs::Load(): makes the zip format's reader turn '\' into '/' in
// the paths of items not made on Unix. Every place that loads codecs calls
// it, so browsing, extracting and 7zG commands all see the same paths.
void Z7Qt_FixWinPathFormats(CCodecs *codecs);

#endif

// UpxWrap.h -- 7zfm-qt: UPX-packed executables unpacked by the system's upx
// (see UpxWrap.cpp).

#ifndef Z7QT_UPX_WRAP_H
#define Z7QT_UPX_WRAP_H

class CCodecs;

// After CCodecs::Load() (Z7Qt_FixWinPathFormats calls it): wraps the PE, ELF
// and Mach-O readers.
void Z7Qt_WrapUpxFormats(CCodecs *codecs);

#endif

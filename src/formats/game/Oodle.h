// Oodle.h -- 7zfm-qt: a decoder for Oodle Data's compressed streams (Kraken,
// Mermaid, Selkie, Leviathan, LZNA, Bitknit, LZB16), translated from
// oozextract (MIT license, see LICENSE.oozextract).

#ifndef Z7QT_OODLE_H
#define Z7QT_OODLE_H

#include <stddef.h>

namespace NOodle {

// Decodes one stream, as OodleLZ_Decompress takes it, into exactly outSize
// bytes. False when the stream is malformed or doesn't fill the output.
bool Decode(const unsigned char *in, size_t inSize, unsigned char *out, size_t outSize);

}

#endif

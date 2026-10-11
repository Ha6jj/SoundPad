// Единственная единица трансляции с реализацией miniaudio и stb_vorbis.
// stb_vorbis подключается до miniaudio, чтобы включить декодер Ogg Vorbis.
#define STB_VORBIS_HEADER_ONLY
#include "extras/stb_vorbis.c"

#define MA_NO_ENCODING
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#undef STB_VORBIS_HEADER_ONLY
#include "extras/stb_vorbis.c"

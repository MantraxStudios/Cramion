/* Implementacion de miniaudio (audio 2D/3D del motor), con stb_vorbis para
   los .ogg: la cabecera de stb_vorbis antes de miniaudio (activa su decodificador
   Vorbis) y su implementacion despues. */
#define STB_VORBIS_HEADER_ONLY
#include "extras/stb_vorbis.c"

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#undef STB_VORBIS_HEADER_ONLY
#include "extras/stb_vorbis.c"

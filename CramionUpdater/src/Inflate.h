#ifndef CRAMION_UPDATER_INFLATE_H
#define CRAMION_UPDATER_INFLATE_H

// Deflate en crudo (zip, metodo 8) y PNG con stb_image, compilado en modo
// STB_IMAGE_STATIC para no chocar con el de CramionFX al enlazar el editor.

#include <cstddef>
#include <vector>

namespace cramion::update::detail {

// Descomprime exactamente `out_size` bytes. false si el dato esta roto.
bool inflateRaw(const unsigned char* data, std::size_t size, unsigned char* out, std::size_t out_size);

// PNG/JPG en memoria a RGBA8. Vacio si no se puede leer.
std::vector<unsigned char> decodeImage(const unsigned char* data, std::size_t size, int& width, int& height);

// RGBA8 a PNG (capturas de la interfaz para las pruebas).
bool writePng(const char* path, int width, int height, const unsigned char* rgba, int stride);

}  // namespace cramion::update::detail

#endif  // CRAMION_UPDATER_INFLATE_H

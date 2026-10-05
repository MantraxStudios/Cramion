#ifndef CRAMION_ASSET_DDS_H
#define CRAMION_ASSET_DDS_H

#include "CramionFX/asset/Model.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cramion::asset {

// true si los bytes empiezan por la firma de un DDS ("DDS ").
bool isDds(const std::uint8_t* data, std::size_t size);

// Lee un DDS 2D. Los formatos comprimidos por bloques (BC1-BC5, BC7) se
// quedan tal cual, con todos sus mips, para subirlos asi a la GPU (ocupan de
// 4 a 8 veces menos que en RGBA8 y ya traen los mips del artista). Los
// formatos sin comprimir de 32 bits se convierten a RGBA8 (solo el nivel 0;
// los mips los genera la GPU). Devuelve false si el formato no se reconoce.
bool parseDds(const std::uint8_t* data, std::size_t size, TextureData& out);

// Descomprime el nivel 0 de una textura BC (BC1-BC5, BC7) a RGBA8, para las
// GPU que no las leen (casi todas las de movil: Mali, PowerVR). Lo que el
// formato no tiene sale como lo daria la GPU: BC4 (r, 0, 0, 1), BC5
// (r, g, 0, 1). Vacio si no es BC o faltan datos. (BcDecode.cpp)
std::vector<std::uint8_t> decodeBlockCompressed(const TextureData& texture);

}  // namespace cramion::asset

#endif  // CRAMION_ASSET_DDS_H

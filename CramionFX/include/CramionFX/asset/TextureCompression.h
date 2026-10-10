#ifndef CRAMION_ASSET_TEXTURE_COMPRESSION_H
#define CRAMION_ASSET_TEXTURE_COMPRESSION_H

// Compresion de texturas a BC7 al importarlas, como Unity (formato "High
// Quality"): 4 veces menos VRAM y RAM que RGBA8 (una de 4K pasa de 85 a 21 MB
// con sus mips) y a simple vista igual. Comprimir es lento (decimas de
// segundo por textura de 4K, en todos los nucleos), asi que se hace UNA vez:
// el resultado se guarda como .dds en una carpeta de cache (la del proyecto,
// Library/Cache/Textures) con el hash del archivo original en el nombre. Si
// la imagen cambia, el hash cambia y se vuelve a comprimir sola.
//
// Sin carpeta de cache (el jugador, las pruebas) no se comprime nada: se
// queda como antes (RGBA8).

#include "CramionFX/asset/Model.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace cramion::asset {

// Donde se guardan las texturas comprimidas (vacia = sin compresion).
void setTextureCacheFolder(const std::filesystem::path& folder);
std::filesystem::path textureCacheFolder();
bool textureCompressionEnabled();

// Clave de cache de unos bytes (una imagen metida en un modelo, o los pixeles
// de una textura hecha en memoria) con lo que cambia el resultado.
std::uint64_t textureCacheKey(const std::uint8_t* data, std::size_t size, bool height_map);
// Clave de un archivo de imagen por su ruta, tamano y fecha (como Unity): se
// mira la cache sin leer el archivo. 0 si no existe.
std::uint64_t textureFileKey(const std::filesystem::path& file, bool height_map);
// Clave de cache de una textura perezosa (asset::isLazyTexture): la de su
// archivo, o la que da el resolvedor de claves para las recetas. 0 si no
// tiene.
std::uint64_t lazyTextureKey(const TextureData& lazy);
// Ya esta comprimida en la cache.
bool isTextureCached(std::uint64_t key);
// Deja una textura perezosa comprimida en la cache (sin quedarse con ella):
// lo que hace la importacion al abrir el proyecto. true si ya estaba.
bool prepareLazyTexture(const TextureData& lazy);

// Lee la textura comprimida de la cache (BC7 con sus mips). false si no esta.
bool loadCachedTexture(std::uint64_t key, TextureData& out);

// RGBA8 -> BC7 con todos sus mips (en varios hilos) y la guarda en la cache.
// Deja en `texture.alpha` si tiene recortes. false si no se pudo (se queda
// en RGBA8).
bool compressTextureBc7(TextureData& texture, std::uint64_t key);

// La misma imagen pedida a la vez por dos modelos se comprime una vez: el
// primero recibe true y comprime (y llama a endTextureCompression); los demas
// esperan aqui a que termine y reciben false (ya esta en la cache).
bool beginTextureCompression(std::uint64_t key);
void endTextureCompression(std::uint64_t key);

// Escribe una textura BC (con sus mips) como .dds (DX10).
std::vector<std::uint8_t> writeDds(const TextureData& texture);

}  // namespace cramion::asset

#endif  // CRAMION_ASSET_TEXTURE_COMPRESSION_H

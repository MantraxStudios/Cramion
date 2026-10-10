#ifndef CRAMION_ASSET_TEXTURE_COMPRESSION_H
#define CRAMION_ASSET_TEXTURE_COMPRESSION_H

// Compresion de texturas al importarlas, como Unity y Unreal: el formato
// segun el uso de la textura (TextureData::usage):
//
//   - BC1 (4 bits por pixel): color y datos opacos (albedo, emisivo, metal,
//     rugosidad, oclusion). Ocho veces menos que RGBA8; el formato por
//     defecto de Unity ("Normal Quality") y el de Unreal para color y mascaras.
//   - BC7 (8 bits por pixel): normal maps, lo que tiene alfa, los datos finos
//     (la altura del parallax) y lo que no se sabe para que es.
//
// Los mips se hacen en espacio lineal (el color no se oscurece de lejos) y
// los de los normal maps se renormalizan.
//
// Comprimir es lento (decimas de segundo por textura de 4K), asi que se hace
// UNA vez: el resultado se guarda como .dds en una carpeta de cache (la del
// proyecto, Library/Cache/Textures) con una clave en el nombre. En el editor
// la clave de un archivo sale de su ruta, tamano y fecha (no hace falta
// leerlo); si la imagen cambia se vuelve a comprimir sola.
//
// El juego exportado lleva sus texturas ya comprimidas (TextureCache/ en el
// paquete) con claves PORTABLES: la de un archivo sale de su ruta dentro de
// Assets, sin la fecha ni el tamano (que cambian al copiarlo), asi que no hay
// que leer nada para buscarla (setTexturePortableKeys). Esa cache es de solo
// lectura: lo que no esta se queda en RGBA8 (comprimir al jugar seria muy
// lento).
//
// Sin carpeta de cache (las pruebas) no se comprime nada.

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
// Claves portables y cache de solo lectura (el juego exportado): las rutas
// relativas a `assets_root`. Vacia = las claves del editor.
void setTexturePortableKeys(const std::filesystem::path& assets_root);
std::filesystem::path texturePortableRoot();
// Se pueden comprimir texturas nuevas (hay cache y no es de solo lectura).
bool textureCacheWritable();

// Clave de cache de unos bytes (una imagen metida en un modelo, o los pixeles
// de una textura hecha en memoria) con lo que cambia el resultado.
std::uint64_t textureCacheKey(const std::uint8_t* data, std::size_t size, bool height_map,
                              TextureUsage usage = TextureUsage::Unknown);
// Clave de un archivo de imagen por su ruta, tamano y fecha (como Unity): se
// mira la cache sin leer el archivo. 0 si no existe.
std::uint64_t textureFileKey(const std::filesystem::path& file, bool height_map,
                             TextureUsage usage = TextureUsage::Unknown);
// Clave portable de un archivo: su ruta dentro de `assets_root` (la misma en
// cualquier PC y carpeta; no lo lee).
std::uint64_t texturePortableFileKey(const std::filesystem::path& file, const std::filesystem::path& assets_root,
                                     bool height_map, TextureUsage usage = TextureUsage::Unknown);
// Una clave con el uso mezclado (el mismo archivo como color o como datos
// sale en formatos distintos).
std::uint64_t textureKeyWithUsage(std::uint64_t key, TextureUsage usage);
// Clave de cache de una textura perezosa (asset::isLazyTexture): la de su
// archivo, o la que da el resolvedor de claves para las recetas. 0 si no
// tiene. Con `portable_root`, la clave portable (la que busca el juego);
// sin el, la del modo de la cache.
std::uint64_t lazyTextureKey(const TextureData& lazy, const std::filesystem::path& portable_root);
std::uint64_t lazyTextureKey(const TextureData& lazy);
// Ya esta comprimida en la cache.
bool isTextureCached(std::uint64_t key);
// El .dds de una clave en la cache (exista o no).
std::filesystem::path cachedTexturePath(std::uint64_t key);
// Deja una textura perezosa comprimida en la cache (sin quedarse con ella):
// lo que hace la importacion al abrir el proyecto. true si ya estaba.
bool prepareLazyTexture(const TextureData& lazy);

// Lee la textura comprimida de la cache (BC con sus mips). false si no esta.
bool loadCachedTexture(std::uint64_t key, TextureData& out);

// RGBA8 -> BC1 o BC7 (segun su uso y su alfa) con todos sus mips (en varios
// hilos) y la guarda en la cache. Deja en `texture.alpha` si tiene recortes.
// false si no se pudo (se queda en RGBA8).
bool compressTexture(TextureData& texture, std::uint64_t key);

// La misma imagen pedida a la vez por dos modelos se comprime una vez: el
// primero recibe true y comprime (y llama a endTextureCompression); los demas
// esperan aqui a que termine y reciben false (ya esta en la cache).
bool beginTextureCompression(std::uint64_t key);
void endTextureCompression(std::uint64_t key);

// Exportar el juego: deja la textura comprimida en la cache del editor (si no
// lo estaba) y devuelve la clave con la que la buscara el juego (la portable
// de su archivo, o la de sus bytes si va incrustada en un modelo) y su .dds
// en `dds`. 0 si no se comprime (DDS, muy pequena, sin cache).
std::uint64_t prepareExportTexture(const TextureData& texture, const std::filesystem::path& assets_root,
                                   std::filesystem::path& dds);

// Escribe una textura BC (con sus mips) como .dds (DX10).
std::vector<std::uint8_t> writeDds(const TextureData& texture);

}  // namespace cramion::asset

#endif  // CRAMION_ASSET_TEXTURE_COMPRESSION_H

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
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
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

// Un nivel RGBA8 a bloques BC7 (16 bytes por bloque de 4x4), en `threads`
// hilos, sin cache ni mips: para texturas que el motor genera con sus propios
// mips (p. ej. las de los arboles procedurales).
std::vector<std::uint8_t> encodeBc7Level(const std::uint8_t* rgba, std::uint32_t width, std::uint32_t height,
                                         unsigned threads);

// --- Arrays de capas que genera o reescala el motor (los arboles, las capas
// del terreno) en BC7, con una cache por usuario ---
// Las capas son cuadradas y traen ya sus mips ([capa][mip], RGBA8 o bloques
// BC7). Comprimirlas cuesta segundos de CPU: se hace una vez, en segundo
// plano, y se guarda en %LOCALAPPDATA%/Cramion/Cache (el juego exportado no
// puede escribir en su TextureCache); el arranque siguiente las sube ya en
// BC7. La clave es un hash del contenido: si cambia lo generado o la imagen
// de origen, la cache no vale. Un array solo pasa a BC7 si el nivel 0 de cada
// capa da >= kMinUiPsnr dB; si no, la cache lo apunta y se queda en RGBA8.
using MipLayers = std::vector<std::vector<std::vector<std::uint8_t>>>;
std::uint64_t hashMipLayers(const MipLayers& layers, std::uint64_t seed);
// Vacio si no hay carpeta del usuario (o no es Windows).
std::filesystem::path userTextureCachePath(const std::string& stem, std::uint64_t hash);
struct Bc7ArrayCache {
    std::vector<std::uint8_t> bc7;  // por array: 1 = en BC7, 0 = se queda en RGBA8
    std::vector<MipLayers> blocks;  // por array (vacio si se queda en RGBA8)
};
// `like`: los arrays RGBA8 (para comprobar capas, mips y lado).
bool readBc7ArrayCache(const std::filesystem::path& file, std::uint64_t hash, std::uint32_t size,
                       const std::vector<const MipLayers*>& like, Bc7ArrayCache& out);
bool writeBc7ArrayCache(const std::filesystem::path& file, std::uint64_t hash, std::uint32_t size,
                        const std::vector<const MipLayers*>& like, const Bc7ArrayCache& cache);
// Comprime un array entero. false si una capa pierde nitidez o si `stop`.
// `premultiplied`: el color se compara multiplicado por su alfa (lo
// recortado no cuenta); si no, los cuatro canales tal cual (el alfa es un
// dato, como en las normales de los arboles).
bool compressBc7Layers(const MipLayers& rgba, std::uint32_t size, MipLayers& blocks, const std::atomic<bool>& stop,
                       unsigned threads, bool premultiplied, double& worst_psnr);

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

// --- Imagenes de la interfaz (ImGuiLayer::image) ---
// Se comprimen a BC7 (4 veces menos memoria que RGBA8) solo si no pierden
// nitidez: el nivel 0 descomprimido tiene que dar al menos kMinUiPsnr dB de
// PSNR (sobre RGBA premultiplicado: lo transparente no cuenta) frente al
// original; si no, la imagen se queda en RGBA8 y se apunta (<clave>.rgba en la
// cache) para no volver a intentarlo.
constexpr double kMinUiPsnr = 40.0;
// Clave de una imagen de interfaz (con el lado maximo con que se carga). Con
// `portable_root`, la portable (la del juego exportado). 0 si no se comprime.
std::uint64_t uiTextureKey(const std::filesystem::path& file, std::uint32_t max_size,
                           const std::filesystem::path& portable_root);
// La imagen de interfaz comprimida: de la cache o, si se puede escribir en
// ella, comprimiendola ahora (con el control de PSNR). false = usar RGBA8.
// `psnr` (opcional) recibe el PSNR medido (0 si venia de la cache).
bool loadUiTexture(const std::filesystem::path& file, std::uint32_t max_size, TextureData& out,
                   double* psnr = nullptr);
// Exportar: deja la imagen de interfaz comprimida en la cache del editor y
// devuelve la clave portable y su .dds. 0 si se queda en RGBA8.
std::uint64_t prepareExportUiTexture(const std::filesystem::path& file, std::uint32_t max_size,
                                     const std::filesystem::path& assets_root, std::filesystem::path& dds);
// PSNR (dB) entre dos imagenes RGBA8 del mismo tamano, con el color
// premultiplicado por su alfa. 99 si son iguales.
double premultipliedPsnr(const std::uint8_t* a, const std::uint8_t* b, std::size_t pixels);

// Escribe una textura BC (con sus mips) como .dds (DX10).
std::vector<std::uint8_t> writeDds(const TextureData& texture);

}  // namespace cramion::asset

#endif  // CRAMION_ASSET_TEXTURE_COMPRESSION_H

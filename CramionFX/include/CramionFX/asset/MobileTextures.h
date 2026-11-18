#ifndef CRAMION_ASSET_MOBILE_TEXTURES_H
#define CRAMION_ASSET_MOBILE_TEXTURES_H

// Texturas comprimidas para movil (Android): ASTC 4x4 y, de respaldo, ETC2
// RGBA8. Casi ninguna GPU de movil lee BC1/BC7 (las del editor y del juego de
// PC), asi que hasta ahora el movil las descomprimia a RGBA8 en la CPU al
// cargar: 4-8 veces mas VRAM y RAM, y tiempo de carga.
//
// Al exportar a Android (opcion "Texturas de movil" de la configuracion de
// compilacion, apagada por defecto) el editor pasa cada textura, con sus
// mips, a ASTC 4x4 (astc-encoder de Arm) y/o ETC2 (codificador propio) y las
// mete en el paquete como TextureCache/<clave>.astc / .etc2 (formato .crtx,
// abajo). La clave es la misma que buscaria el juego de PC (la portable de su
// archivo, la de sus bytes si va incrustada) o, para las que el .crdata ya
// trae en BC, la de esos bytes. El editor guarda lo convertido en una cache
// (Library/Cache/MobileTextures) para no repetirlo en la exportacion
// siguiente.
//
// En el movil (setMobileTextureCache, solo si la GPU tiene el formato) la
// carga mira primero esa carpeta: si esta, la textura se sube tal cual (sin
// decodificar nada en la CPU); si no, o si la GPU no lo tiene, lo de siempre
// (RGBA8). Los .crdata, las escenas y los prefabs no cambian.
//
// Las dos van en bloques de 4x4 de 16 bytes (8 bits por texel): los mips se
// quitan como los de BC7 (limitTextureSize) y las cuentas de tamano son las
// mismas (mipByteSize).

#include "CramionFX/asset/Model.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cramion::asset {

enum class MobileFormat : std::uint8_t {
    Astc4x4 = 1,
    Etc2Rgba8 = 2,
};

// El TextureFormat de cada uno y su extension en TextureCache/ (".astc", ".etc2").
TextureFormat mobileTextureFormat(MobileFormat format);
const char* mobileTextureExtension(MobileFormat format);
bool isMobileTextureFormat(TextureFormat format);

// --- Codificar y decodificar un nivel (RGBA8 por filas) ---
// El editor tiene el compresor ASTC (astc-encoder; CRAMION_HAS_ASTC_ENCODER).
// El juego de Android no: no comprime nada.
bool astcEncoderAvailable();
// Vacio si no hay compresor o falla. `normal_map`: el error se mide en los
// canales de la normal (R, G) y no como color.
std::vector<std::uint8_t> encodeAstc4x4Level(const std::uint8_t* rgba, std::uint32_t width, std::uint32_t height,
                                             bool normal_map, unsigned threads);
// Para las pruebas y para comprobar la calidad al exportar (solo con el
// compresor). Vacio si no se puede.
std::vector<std::uint8_t> decodeAstc4x4Level(const std::uint8_t* blocks, std::size_t size, std::uint32_t width,
                                             std::uint32_t height);
// ETC2 RGBA8 (color en los modos de ETC1, alfa EAC): siempre disponible.
std::vector<std::uint8_t> encodeEtc2Rgba8Level(const std::uint8_t* rgba, std::uint32_t width, std::uint32_t height,
                                               unsigned threads);
std::vector<std::uint8_t> decodeEtc2Rgba8Level(const std::uint8_t* blocks, std::size_t size, std::uint32_t width,
                                               std::uint32_t height);
// Un bloque de 16 bytes -> 16 texeles RGBA8 por filas (y * 4 + x). Entiende
// todos los modos de ETC2 (T, H y plano tambien).
void decodeEtc2Rgba8Block(const std::uint8_t* block, std::uint8_t rgba[64]);

// Una textura (RGBA8, o BC con sus mips) al formato de movil con los mismos
// mips (RGBA8 sin mips: se hacen con media de 2x2). Deja en `out.alpha` si
// tiene recortes. false si no se pudo (sin compresor ASTC, datos rotos).
bool convertToMobileTexture(const TextureData& source, MobileFormat format, TextureData& out, unsigned threads = 0);

// --- Archivo .crtx (TextureCache/<clave>.astc / .etc2) ---
// "CRTX", version, formato, ancho, alto, mips, alfa, uso y los bloques de
// todos los mips seguidos (el nivel 0 primero).
std::vector<std::uint8_t> writeMobileTextureFile(const TextureData& texture);
bool readMobileTextureFile(const std::uint8_t* data, std::size_t size, TextureData& out);
bool readMobileTextureFile(const std::filesystem::path& file, TextureData& out);

// --- Claves ---
// La clave con la que el juego busca la textura (0 si no tiene): perezosa ->
// la portable de su archivo o receta (lazyTextureKey con `assets_root`);
// imagen incrustada -> la de sus bytes (textureCacheKey); ya en BC o RGBA8 en
// el .crdata -> la de esos pixeles.
std::uint64_t mobileTextureKey(const TextureData& stored, const std::filesystem::path& assets_root);

// --- El juego ---
// Carpeta con las texturas de movil (la TextureCache/ del paquete), la raiz
// de los assets (claves portables) y que formatos lee la GPU. Carpeta vacia o
// sin formatos = apagado (lo de siempre).
void setMobileTextureCache(const std::filesystem::path& folder, const std::filesystem::path& assets_root, bool astc,
                           bool etc2);
bool mobileTextureCacheActive();
// La version de movil de `stored` (ASTC si la GPU lo lee y esta; si no ETC2).
// false si no hay (se sigue como siempre). Cualquier hilo.
bool loadMobileTexture(const TextureData& stored, TextureData& out);
// Cuantas texturas llegaron ya en formato de movil y cuantas se buscaron (el log).
void mobileTextureCounts(std::uint32_t& loaded, std::uint32_t& looked_up);

// --- Exportar (editor) ---
// Deja la textura convertida en `cache_folder` (si no lo estaba ya) y
// devuelve la clave con la que la buscara el juego y su archivo en `file`. 0
// si no se convierte (sin clave, muy pequena, sin compresor). Usa la cache de
// texturas del editor (prepareExportTexture: BC1/BC7 con sus mips) como
// origen, o los pixeles que trae el .crdata.
std::uint64_t prepareMobileExportTexture(const TextureData& texture, const std::filesystem::path& assets_root,
                                         MobileFormat format, const std::filesystem::path& cache_folder,
                                         std::filesystem::path& file);

}  // namespace cramion::asset

#endif  // CRAMION_ASSET_MOBILE_TEXTURES_H

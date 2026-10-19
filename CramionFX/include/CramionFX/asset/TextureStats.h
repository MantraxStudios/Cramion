#ifndef CRAMION_ASSET_TEXTURE_STATS_H
#define CRAMION_ASSET_TEXTURE_STATS_H

// Estadisticas de una textura ya decodificada: el color medio que ven el
// horneado de luz (albedo y emision de cada material) y los proxies lejanos.

#include "CramionFX/asset/Model.h"
#include "CramionFX/core/Math.h"

#include <filesystem>

namespace cramion::asset {

// Color medio lineal de una textura (BC o RGBA8; las perezosas se resuelven fuera).
// opaque_fraction: parte de los texeles con alfa >= 0.5 (recorte por alfa).
// La media es la de los texeles opacos (si no hay ninguno, la de todos): lo
// recortado no se ve. Muestreo disperso (como mucho ~2048 texeles). false si
// la textura esta vacia o sus datos no cuadran con su tamano.
bool averageTextureLinear(const TextureData& t, core::Vec3& rgb, float* opaque_fraction = nullptr);

// Lo mismo de una textura tal como esta guardada en un .crdata o en la cache
// del modelo: ya decodificada (RGBA8 o BC), incrustada sin decodificar (PNG,
// JPG, DDS en `encoded`) o perezosa (solo la ruta: resolveLazyTexture y, si
// no, se lee el archivo). Cualquier hilo.
bool averageStoredTextureLinear(const TextureData& t, core::Vec3& rgb, float* opaque_fraction = nullptr);

// De una imagen en disco (PNG, JPG, TGA...): las texturas de los .crmat y de
// las mallas de codigo. Se lee reducida (lado de 256 como mucho).
bool averageImageFileLinear(const std::filesystem::path& file, core::Vec3& rgb, float* opaque_fraction = nullptr);

}  // namespace cramion::asset

#endif  // CRAMION_ASSET_TEXTURE_STATS_H

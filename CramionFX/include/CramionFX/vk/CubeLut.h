#ifndef CRAMION_VK_CUBE_LUT_H
#define CRAMION_VK_CUBE_LUT_H

// LUT de color en formato .cube (Adobe / Resolve): la gradacion que se hace
// en DaVinci Resolve, Photoshop o Lightroom se aplica tal cual en la
// composicion (composite.frag), sobre la imagen ya con el tono, en sRGB.
//
// Formato (texto):
//   TITLE "nombre"            opcional
//   LUT_3D_SIZE N             2..256 (N^3 lineas de "r g b", rojo el que
//                             cambia mas deprisa)
//   LUT_1D_SIZE N             2..65536 (N lineas; se convierte a 3D de 33)
//   DOMAIN_MIN r g b          opcional (0 0 0)
//   DOMAIN_MAX r g b          opcional (1 1 1)
//   LUT_3D_INPUT_RANGE a b    / LUT_1D_INPUT_RANGE a b (Resolve): dominio a..b
//   # comentarios             y lineas vacias
// Sin GPU: CubeLut es la tabla en floats; ColorLut (VulkanRenderer) la sube.

#include "CramionFX/core/Math.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace cramion::gfx {

struct CubeLut {
    std::string title;
    std::uint32_t size = 0;  // lado del cubo (N)
    core::Vec3 domain_min{0.0f, 0.0f, 0.0f};
    core::Vec3 domain_max{1.0f, 1.0f, 1.0f};
    // N^3 * 3 floats (r, g, b), indice = (b * N + g) * N + r.
    std::vector<float> rgb;

    bool valid() const { return size >= 2 && rgb.size() == static_cast<std::size_t>(size) * size * size * 3; }
    core::Vec3 at(std::uint32_t r, std::uint32_t g, std::uint32_t b) const;
};

inline constexpr std::uint32_t kCubeLutMaxSize = 256;
// Lado del cubo al convertir una LUT 1D.
inline constexpr std::uint32_t kCubeLutFrom1DSize = 33;

// Lee el texto de un .cube. false (y `error` en espanol con la linea) si no
// se entiende; `out` queda sin cambiar.
bool parseCubeLut(std::string_view text, CubeLut& out, std::string& error);
// Lee el archivo (UTF-8 en la ruta). Igual que parseCubeLut.
bool loadCubeLut(const std::filesystem::path& file, CubeLut& out, std::string& error);

// Cubo identidad de lado n (para pruebas y como textura por defecto).
CubeLut makeIdentityCubeLut(std::uint32_t n);

// Lo que da la LUT para un color (interpolacion trilineal, con el dominio):
// lo mismo que hace la GPU (composite.frag), para pruebas y herramientas.
core::Vec3 sampleCubeLut(const CubeLut& lut, const core::Vec3& color);

// Texels RGBA en half float (lo que se sube a la textura 3D): N^3 * 4.
std::vector<std::uint16_t> cubeLutToHalfRgba(const CubeLut& lut);

}  // namespace cramion::gfx

#endif  // CRAMION_VK_CUBE_LUT_H

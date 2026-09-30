#ifndef CRAMION_ASSET_HOUSE_GENERATOR_H
#define CRAMION_ASSET_HOUSE_GENERATOR_H

// Casas y cabanas procedurales para juegos: cabana de troncos, cabana de
// tablas, casita de piedra y casa de campo. Geometria de verdad donde se ve
// (troncos redondos con las esquinas cruzadas, marcos, alfeizares, aleros,
// porche con barandilla, chimenea) y el resto en las texturas PBR (con altura
// para el parallax). Pocos triangulos (2-6k), 8 materiales compartidos por
// todas las casas del proyecto (una llamada de dibujo por material; las
// texturas se escriben una vez) y la puerta como pieza aparte con la bisagra
// en su origen (se abre girandola en Y, p. ej. desde Lua).

#include "CramionFX/asset/ImageFile.h"
#include "CramionFX/asset/Model.h"

#include <array>
#include <cstdint>

namespace cramion::asset {

enum class HouseStyle : int { LogCabin = 0, TimberCabin = 1, StoneCottage = 2, Farmhouse = 3 };
inline constexpr int kHouseStyleCount = 4;
const char* houseStyleName(HouseStyle style);  // "Cabana de troncos"...

struct HouseSettings {
    HouseStyle style = HouseStyle::LogCabin;
    std::uint32_t seed = 1;
    float width = 7.5f;          // m, a lo largo de la cumbrera (X)
    float depth = 5.5f;          // m, de delante (+Z, la puerta) a atras
    int floors = 1;              // 1..2
    float wall_height = 2.7f;    // m por planta
    float roof_pitch = 38.0f;    // grados
    float roof_overhang = 0.55f; // m de alero
    bool porch = true;           // porche delante con barandilla y escalones
    bool chimney = true;
    bool shutters = true;        // contraventanas (no en la de troncos)
    int windows = -1;            // ventanas en la fachada larga; -1 = segun el ancho
};

// Preajuste de cada estilo (medidas y detalles tipicos).
HouseSettings housePreset(HouseStyle style, std::uint32_t seed = 1);

// Materiales (huecos de ModelData::materials, en este orden).
enum HouseMaterial : int {
    kHouseLogs = 0,     // troncos (corteza pelada)
    kHouseLogEnds,      // veta de las testas
    kHousePlanks,       // tablas en vertical (paredes, suelo, puerta, porche)
    kHouseSiding,       // tablas solapadas pintadas (casa de campo)
    kHouseStone,        // piedra (zocalo, chimenea, muros)
    kHouseRoof,         // tejado de tablillas
    kHouseTrim,         // marcos pintados
    kHouseGlass,        // vidrio
    kHouseIron,         // herrajes
    kHouseMaterialCount
};
const char* houseMaterialName(int material);   // "Troncos", "Veta"... (nombre del material)
const char* houseTextureName(int material);    // base de las imagenes ("" = sin texturas)
// Metros que cubre una repeticion de la textura (U, V) de cada material.
std::array<float, 2> houseTextureMeters(int material);

struct HouseModel {
    ModelData house;              // todo menos la puerta (origen = centro del suelo, y = 0 el terreno)
    ModelData door;               // la hoja de la puerta, con la bisagra en su origen
    core::Vec3 door_hinge{};      // donde va la puerta en la casa (gira en Y)
    core::Vec3 door_size{};       // medidas de la hoja (para su BoxCollider; centro = size/2 en X e Y)
    core::Vec3 bounds_min{};
    core::Vec3 bounds_max{};
    std::size_t triangles = 0;
};

HouseModel buildHouse(const HouseSettings& settings);

// Texturas de un material: color (sRGB), normal (OpenGL), rugosidad, oclusion
// y altura (blanco = alto), todas repetibles salvo la veta.
struct HouseTextureSet {
    ImageRgba8 color;
    ImageRgba8 normal;
    ImageRgba8 roughness;
    ImageRgba8 occlusion;
    ImageRgba8 height;
};
// false si el material no lleva texturas (vidrio, hierro).
bool generateHouseTexture(int material, int size, std::uint32_t seed, HouseTextureSet& out);

}  // namespace cramion::asset

#endif  // CRAMION_ASSET_HOUSE_GENERATOR_H

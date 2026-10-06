#ifndef CRAMION_ASSET_MEDIEVAL_BUILDINGS_H
#define CRAMION_ASSET_MEDIEVAL_BUILDINGS_H

// Edificios y objetos de los pueblos y ciudades medievales (ademas de las
// casas, tabernas y herrerias de HouseGenerator): iglesia con campanario,
// granero, pozo, puesto de mercado, torre del homenaje, puerta de la muralla,
// molino de viento y objetos (barriles, cajas, carro, paja, lena, banco).
// Usan los mismos materiales que las casas (HouseMaterial) y, como ellas,
// tienen el origen en el centro del suelo (y = 0 el terreno), la entrada
// mirando a +Z y, si la hay, la puerta como pieza aparte con la bisagra en su
// origen. Con interior: bancos y altar en la iglesia, cuadras y pajar en el
// granero, salon con hogar en la torre.
//
// La muralla entera (tramos que siguen el terreno, almenas, adarve y torres
// redondas) y las vallas de los campos se hacen como una malla unica para
// cada pueblo (buildCityWall, buildFences).

#include "CramionFX/asset/HouseGenerator.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace cramion::asset {

enum class MedievalBuilding : int {
    Church = 0,    // iglesia con campanario (puerta en la torre, a +Z)
    Barn,          // granero/establo (portalon a +Z)
    Well,          // pozo con tejadillo y torno
    MarketStall,   // puesto de mercado con toldo
    Keep,          // torre del homenaje (puerta en alto con escalera)
    Gatehouse,     // puerta de la muralla: el paso va a lo largo de Z
    Windmill,      // molino de viento (aspas: pieza "Aspas", gira en su Z)
    Barrels,       // objetos sueltos:
    Crates,
    Cart,
    HayBales,
    Woodpile,
    Bench,
};
inline constexpr int kMedievalBuildingCount = 13;
const char* medievalBuildingName(MedievalBuilding building);  // "Iglesia"...

struct MedievalSettings {
    MedievalBuilding type = MedievalBuilding::Church;
    std::uint32_t seed = 1;
    bool interior = true;
    float scale = 1.0f;          // tamano (iglesia de pueblo 1, de ciudad ~1.35; torre, puerta)
    float wall_height = 7.0f;    // puerta de la muralla: alto de la muralla a la que se une
};

HouseModel buildMedieval(const MedievalSettings& settings);

// Muralla: anillo cerrado de puntos en el espacio del modelo (y = el suelo en
// cada punto; no se repite el primero). towers[i]: torre redonda en el punto
// i; gaps[i]: el tramo de i a i+1 es un hueco (alli va una puerta).
struct CityWallSettings {
    std::vector<core::Vec3> points;
    std::vector<bool> towers;
    std::vector<bool> gaps;
    float height = 7.0f;       // del suelo al adarve
    float thickness = 2.4f;
    float tower_radius = 3.2f;
    std::uint32_t seed = 1;
};
HouseModel buildCityWall(const CityWallSettings& settings);

// Vallas de madera: tramos (a, b) con la y del suelo en cada extremo.
HouseModel buildFences(const std::vector<std::pair<core::Vec3, core::Vec3>>& segments);

}  // namespace cramion::asset

#endif  // CRAMION_ASSET_MEDIEVAL_BUILDINGS_H

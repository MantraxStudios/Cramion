#ifndef CRAMION_ASSET_SETTLEMENT_GENERATOR_H
#define CRAMION_ASSET_SETTLEMENT_GENERATOR_H

// Trazado de pueblos y ciudades medievales sobre un terreno: plaza con pozo y
// mercado, calles principales que salen de la plaza buscando el camino mas
// llano (y siguen como caminos por el campo), callejas, muralla con torres y
// puertas donde la cruzan los caminos (ciudades), edificios con la puerta a
// la calle en hileras (iglesia y taberna en la plaza, herreria, torre del
// homenaje en lo alto, graneros y molino en las afueras), campos con vallas y
// objetos sueltos. No crea nada: devuelve donde va cada cosa (el editor hace
// los modelos, aplana el terreno y los coloca).
//
// No depende del motor: el terreno y el agua se consultan con funciones y la
// huella de cada edificio (su AABB) la da quien llama, asi nada se pisa.

#include <CramionFX/core/Math.h>

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace cramion::asset {

enum class SettlementType : int {
    Hamlet = 0,   // aldea: unas casas de campo, graneros y campos, sin plaza grande
    Village = 1,  // pueblo: plaza con mercado, iglesia, taberna, herreria, molino
    Town = 2,     // ciudad amurallada: murallas con puertas, torre del homenaje, calles en hilera
};
inline constexpr int kSettlementTypeCount = 3;
const char* settlementTypeName(SettlementType type);  // "Aldea", "Pueblo", "Ciudad amurallada"

// Lo que coloca el trazado.
enum class LotKind : int {
    House = 0,     // variant = indice de la variante de casa (urbanas primero)
    Church,
    Tavern,
    Smithy,
    Barn,
    Well,
    MarketStall,
    Keep,
    Gatehouse,
    Windmill,
    Prop,          // variant = el MedievalBuilding del objeto (Barrels..Bench)
};

struct SettlementLot {
    LotKind kind = LotKind::House;
    int variant = 0;
    core::Vec3 position{};  // origen del modelo en el mundo (y = el terreno de antes)
    float yaw = 0.0f;       // grados (el +Z del modelo es su fachada)
    core::Vec2 half{};      // medias medidas de su AABB (X, Z locales)
    core::Vec2 offset{};    // centro del AABB en el modelo (x, z)
};

struct SettlementRoad {
    std::vector<core::Vec3> points;  // y = el terreno de antes
    float width = 4.0f;
    bool main = true;                // principal (sale de la plaza y sigue por el campo)
};

struct SettlementField {
    core::Vec3 center{};
    core::Vec2 half{};
    float yaw = 0.0f;
};

struct SettlementLayout {
    bool ok = false;
    std::string error;
    core::Vec3 center{};
    float radius = 0.0f;
    float plaza_radius = 0.0f;
    std::vector<SettlementRoad> roads;
    std::vector<SettlementLot> lots;
    std::vector<SettlementField> fields;
    // Muralla: anillo cerrado (mundo), torres por punto y huecos por tramo
    // (el tramo i -> i+1 es una puerta: alli hay un lote Gatehouse).
    std::vector<core::Vec3> wall;
    std::vector<bool> wall_towers;
    std::vector<bool> wall_gaps;
    // Vallas de los campos (tramos cortos; y = el terreno de antes).
    std::vector<std::pair<core::Vec3, core::Vec3>> fences;
    int houses = 0;  // cuantas casas (sin contar los demas edificios)
};

struct SettlementSettings {
    SettlementType type = SettlementType::Village;
    std::uint32_t seed = 1;
    float radius = 0.0f;        // m; 0 = segun el tipo (aldea 55, pueblo 95, ciudad 130)
    int houses = 0;             // casas; 0 = segun el tipo (10, 28, 75)
    int main_roads = 0;         // 0 = segun el tipo
    bool walls = true;          // ciudad: murallas
    bool fields = true;         // campos con vallas y graneros
    bool market = true;         // puestos en la plaza
    bool church = true;         // iglesia (pueblo y ciudad)
    bool props = true;          // barriles, carros, pacas...
    float max_slope = 4.0f;     // diferencia de altura maxima bajo un edificio (m)
    int urban_variants = 1;     // variantes de casa [0, urban) para el centro / intramuros
    int rural_variants = 1;     // [urban, urban + rural) para las afueras
};

struct SettlementTerrain {
    std::function<float(float x, float z)> height;
    std::function<bool(float x, float z, float margin)> dry;  // false = agua
    float min_x = -1e9f;
    float min_z = -1e9f;
    float max_x = 1e9f;
    float max_z = 1e9f;
};

// Huella de cada cosa: medias medidas (X, Z) y centro (x, z) de su AABB en
// el modelo. Para Prop, `variant` es el MedievalBuilding.
using SettlementFootprint = std::function<void(LotKind kind, int variant, core::Vec2& half, core::Vec2& offset)>;

// `center` = donde (y); con `search_radius` > 0 busca el mejor sitio (llano y
// seco) en ese radio alrededor.
SettlementLayout layoutSettlement(const SettlementSettings& settings, const SettlementTerrain& terrain,
                                  const SettlementFootprint& footprint, const core::Vec3& center, float search_radius);

}  // namespace cramion::asset

#endif  // CRAMION_ASSET_SETTLEMENT_GENERATOR_H

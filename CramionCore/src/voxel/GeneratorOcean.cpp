// OCEANOS-HZ: mares grandes y hondos del generador Aventura.
//
// Cerca del origen el mundo alterna tierra y agua a escala pequena (continente de ~1 km con
// detalle de 260 m, rios de 50-130 m de ancho cada pocos cientos de metros, lagos por todas
// partes). Fuera de las regiones del origen, con una rampa suave:
//
//   - Continentes y oceanos de varios km: un campo de ~8 km con el dominio deformado (costas
//     irregulares, golfos y penínsulas) y detalle de costa de ~700 m (bahias y cabos).
//   - Archipielagos: en algunos oceanos, islas de 0,3 a 2 km.
//   - Oceanos hondos: el perfil de oceanFloor (plataforma, talud, llanura abisal, dorsales,
//     montes submarinos, fosas) mas ancho y mas hondo (la llanura a y ~8-12).
//   - Costas: playas y, en tramos, acantilados de 8 a 22 m que caen al mar; plataformas someras
//     anchas en unos tramos (arrecifes, kelp) y estrechas en otros.
//   - Rios mas escasos (cada ~1 km) y estrechos (~15-40 m; estuarios mas anchos) y menos lagos
//     sueltos: el agua dulce deja de ser una red que trocea la tierra.
//
// Funcion pura de (semilla, x, z) con los ruidos del motor (nada de <random> ni std::shuffle).
// Nada cambia dentro de oceanEdge() (Chebyshev, ~1 km: las 3 coronas de regiones del origen): las
// ediciones guardadas por coordenada, las aldeas del inicio y la capital siguen bloque a bloque.

#include "VoxelInternal.h"

#include <algorithm>
#include <cmath>

namespace cramion::voxel {

namespace {

float smoothstep(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Escala del campo nuevo: lo deja con la pendiente (en m) del de siempre junto a la costa, para
// que el perfil del fondo (oceanFloor) y el de la tierra (land, inland) tengan anchos parecidos.
constexpr float kContinentGain = 4.0f;

}  // namespace

// fbm2 suma salt * 131,7 a la coordenada: con las sales altas (> 700) y escalas de km, el float
// pierde la parte fina (escalones de 20-50 m en la costa). Aqui el desfase de cada octava es < 256
// (la tabla de ruido se repite cada 256) y la sal 0 no desplaza.
float Generator::oceanFbm(float x, float z, int octaves, int k) const {
    float sum = 0.0f, amp = 1.0f, norm = 0.0f, freq = 1.0f;
    for (int i = 0; i < octaves; ++i) {
        const float ox = static_cast<float>((k * 37 + i * 59) % 251) + 0.37f;
        const float oz = static_cast<float>((k * 83 + i * 29) % 241) + 0.71f;
        sum += noise2(x * freq + ox, z * freq + oz, 0) * amp;
        norm += amp;
        amp *= 0.5f;
        freq *= 2.03f;
    }
    return sum / norm;
}

float Generator::oceanScaleWeight(float x, float z) const {
    if (!deep_oceans_ || profile_ != VoxelGenerator::Adventure) return 0.0f;
    if (oceanEdge() <= 0) return 1.0f;
    const float edge = static_cast<float>(oceanEdge());
    const float d = std::max(std::abs(x), std::abs(z));
    if (d <= edge) return 0.0f;
    return smoothstep(edge, edge + kOceanBlend, d);
}

float Generator::oceanContinent(float x, float z) const {
    // Dominio deformado: costas que serpentean a gran escala (golfos, penínsulas).
    const float wx = x + oceanFbm(x / 2600.0f, z / 2600.0f, 3, 11) * 1100.0f;
    const float wz = z + oceanFbm(x / 2600.0f + 31.7f, z / 2600.0f - 12.3f, 3, 12) * 1100.0f;
    float c = oceanFbm(wx / 8500.0f, wz / 8500.0f, 5, 13) - 0.02f;
    // Bahias y cabos.
    c += oceanFbm(x / 700.0f, z / 700.0f, 3, 14) * 0.06f;
    // Archipielagos: en algunas zonas del mar, islas (de 0,3 a 2 km).
    const float arch = smoothstep(-0.05f, 0.20f, oceanFbm(x / 5200.0f, z / 5200.0f, 2, 15));
    if (arch > 0.0f && c < 0.05f) {
        const float isl = smoothstep(0.18f, 0.50f, oceanFbm(x / 1100.0f, z / 1100.0f, 3, 16));
        c += isl * arch * 0.7f;
    }
    return c * kContinentGain;
}

float Generator::oceanRiverNoise(float x, float z) const {
    // Cauces cada ~1 km (el de siempre: cada ~400 m) y, con la ganancia, mas estrechos.
    return (oceanFbm(x / 1500.0f, z / 1500.0f, 3, 21) + oceanFbm(x / 320.0f, z / 320.0f, 2, 22) * 0.05f) * 8.0f;
}

float Generator::oceanShelf(float x, float z) const {
    // Plataformas anchas (mares someros de cientos de metros: arrecifes, kelp, praderas) en unos
    // tramos de costa y estrechas en otros (el fondo cae enseguida, junto a los acantilados).
    return 0.25f + 0.75f * smoothstep(-0.25f, 0.15f, oceanFbm(x / 2200.0f, z / 2200.0f, 2, 41));
}

float Generator::oceanCoast(float x, float z, float continent, float h) const {
    const float sea = static_cast<float>(sea_);
    // Playa: una franja limpia de ~10-35 m a ras del agua; detras, la tierra sube (sin el
    // moteado de arena y hierba que deja el relieve suave cuando ronda el nivel del mar).
    const float beach = 0.006f + 0.014f * (0.5f + 0.5f * oceanFbm(x / 260.0f, z / 260.0f, 2, 19));
    h = std::max(h, sea + 2.0f + 3.0f * smoothstep(beach, beach + 0.02f, continent));
    // Acantilados: en tramos de costa, la tierra sube de golpe 8-22 m a pocos metros del agua
    // (cara de piedra: la pendiente >= 4 la pinta generateAdventure). Fuera de ellos, playa.
    const float cm = smoothstep(0.08f, 0.26f, oceanFbm(x / 900.0f, z / 900.0f, 2, 17));
    if (cm <= 0.0f) return h;
    const float tall = 8.0f + 14.0f * (0.5f + 0.5f * oceanFbm(x / 300.0f, z / 300.0f, 2, 18));
    const float edge = smoothstep(0.004f, 0.008f, continent);  // a ~10-20 m de la orilla
    const float fade = 1.0f - smoothstep(0.15f, 0.45f, continent);  // tierra adentro, el relieve de siempre
    const float top = sea + 2.0f + tall * cm * edge * fade;
    return std::max(h, top);
}

}  // namespace cramion::voxel

// Vegetacion instanciada: lo comun a foliage_cull.comp y foliage.vert.
//
// Instancia (16 bytes, std430): posicion y un uint con
//   bits 0-9 giro (0..1023 -> 0..2pi), 10-17 escala (0..255 -> 0.25..4),
//   18-19 especie, 20-27 tono (0..255).

struct FoliageInstance {
    vec3 position;
    uint packed;
};

float foliageYaw(uint p) { return float(p & 1023u) * (6.2831853 / 1024.0); }
float foliageScale(uint p) { return 0.25 + float((p >> 10u) & 255u) * (3.75 / 255.0); }
uint foliageSpecies(uint p) { return (p >> 18u) & 3u; }
float foliageTint(uint p) { return float((p >> 20u) & 255u) / 255.0; }

// Esfera que envuelve cada especie (a escala 1): centro sobre el pie y radio.
const float kFoliageCenterY[3] = float[](5.0, 4.6, 5.2);
const float kFoliageRadius[3] = float[](5.8, 5.4, 5.2);

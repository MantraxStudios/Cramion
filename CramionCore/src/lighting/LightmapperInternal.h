#ifndef CRAMION_CORE_SRC_LIGHTING_LIGHTMAPPER_INTERNAL_H
#define CRAMION_CORE_SRC_LIGHTING_LIGHTMAPPER_INTERNAL_H

// Datos que comparten las cuatro unidades del lightmapper (Lightmapper*.cpp):
// mallas con UV2, instancias en las paginas, texeles y el trazador.

#include "CramionCore/lighting/BakeTracer.h"
#include "CramionCore/lighting/Lightmapper.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace cramion::lighting::lm {

using core::Vec2;
using core::Vec3;
using core::Vec4;

inline constexpr std::uint32_t kNone = 0xFFFFFFFFu;
inline constexpr int kSubSamples = 16;  // 4 x 4 por texel

// Una pieza de modelo con su UV2 aplicada (la copia partida, compacta).
struct Mesh {
    LightmapUvPartKey key;
    std::shared_ptr<const LightmapUvLayout> layout;
    std::string name;
    std::vector<Vec3> positions;  // espacio del modelo
    std::vector<Vec3> normals;
    std::vector<Vec2> uv;         // UV2 en [0, 1]; x < 0 = sin lightmap
    std::vector<std::uint32_t> indices;
    std::vector<std::uint32_t> tri_material;  // material de la pieza de cada triangulo
    float area = 0.0f;                        // unidades del modelo al cuadrado
    float max_scale = 0.0f;                   // s_max de sus instancias
};

// Un receptor con su rectangulo en una pagina.
struct Instance {
    std::uint32_t receiver = 0;
    std::uint32_t mesh = 0;
    core::Mat4 world = core::Mat4::identity();
    float area_world = 0.0f;
    float scale = 0.0f;           // s_i
    std::uint32_t page = 0, x = 0, y = 0, w = 0, h = 0;  // rect en la pagina (texeles)
    core::Vec4 scale_offset{1.0f, 1.0f, 0.0f, 0.0f};
    std::uint32_t tracer_first = 0;  // su primer triangulo en el trazador
    std::vector<std::uint32_t> tracer_of_triangle;  // triangulo de la malla -> del trazador (kNone: no esta)
};

// Un texel con dueno (instancia, triangulo).
struct Texel {
    Vec3 center;      // en el mundo, en el centro del texel (plano del triangulo dueno)
    Vec3 normal;      // suave (interpolada)
    Vec3 geo_normal;  // de la cara, del lado de la suave
    Vec3 du, dv;      // d(mundo)/d(texel) en x e y de la pagina
    float side = 0.0f;  // lado del texel en el mundo
    std::uint32_t instance = kNone;
    std::uint32_t triangle = kNone;  // de la malla
    std::uint16_t valid_mask = 0;    // submuestras que no estan dentro de la geometria
    std::uint8_t representative = 0;
    // Mismo indice que la submuestra k (4 x 4): su posicion en el mundo.
    Vec3 sample(int k) const {
        const float sx = (static_cast<float>(k & 3) + 0.5f) * 0.25f - 0.5f;
        const float sy = (static_cast<float>(k >> 2) + 0.5f) * 0.25f - 0.5f;
        return center + du * sx + dv * sy;
    }
    int validCount() const {
        int n = 0;
        for (int k = 0; k < kSubSamples; ++k) n += (valid_mask >> k) & 1;
        return n;
    }
};

struct Page {
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint32_t> texel_of;  // por pixel: indice en `texels` o kNone
    std::vector<Texel> texels;
    std::vector<Vec4> value;             // rgb = irradiancia, a = visibilidad del sol
    std::vector<std::uint8_t> state;     // 0 vacio, 1 valido, 2 no valido (dentro), 3 dilatado
};

struct Context {
    const LightmapBakeInput* in = nullptr;
    LightmapBakeSettings settings;
    BakeProgress* progress = nullptr;
    LightmapBakeStats* stats = nullptr;
    int threads = 1;
    std::vector<Mesh> meshes;
    std::vector<Instance> instances;
    LightmapPacking packing;
    std::vector<Page> pages;
    BakeTracer tracer;
    // Por triangulo del trazador: instancia (kNone = geometria de la escena),
    // triangulo de su malla (o de la escena) y material (BakeScene::materials).
    std::vector<std::uint32_t> tracer_instance;
    std::vector<std::uint32_t> tracer_local;
    std::vector<std::uint32_t> tracer_material;
    std::vector<Vec3> tracer_albedo;    // de rebote
    std::vector<Vec3> tracer_emission;
    // Sol (hacia el sol, normalizada) y su radiancia lineal.
    Vec3 to_sun{0.0f, 1.0f, 0.0f};
    Vec3 sun_radiance{};
    bool sun_active = false;
    bool irradiance = false;  // rgb vale (luz rebotada y luces Horneadas, Fase 3)
    std::atomic<std::uint64_t> rays{0};

    bool cancelled() const { return progress != nullptr && progress->cancel.load(); }
    void stage(const std::string& text, float fraction) const {
        if (progress == nullptr) return;
        progress->setStage(text);
        progress->fraction.store(fraction);
    }
};

// Reparte `count` trabajos (tiles) entre los hilos del contexto, con
// prioridad por debajo de la normal. fn(indice). Para si se cancela.
void parallelFor(const Context& ctx, std::size_t count, const std::function<void(std::size_t)>& fn);

// Semilla de un rayo: hash(texel, muestra, pasada). Nunca por hilo.
std::uint32_t raySeed(std::uint32_t page, std::uint32_t texel, std::uint32_t sample, std::uint32_t pass);
// Secuencia R2 (Roberts) con giro de Cranley-Patterson.
void r2Sample(std::uint32_t j, float rot_x, float rot_y, float& r1, float& r2);
// Base ortonormal alrededor de n.
void orthonormalBasis(const Vec3& n, Vec3& t, Vec3& b);

// --- LightmapperRaster.cpp ---
// E: texeles de cada pagina (dueno, punto, normales, derivadas).
bool rasterizeTexels(Context& ctx);
// F: validez (submuestras dentro de la geometria).
bool computeValidity(Context& ctx);
// H: dilatacion (texeles vacios o no validos junto a validos).
void dilatePages(Context& ctx, int passes);

// --- LightmapperLight.cpp ---
// G: visibilidad del sol en el canal a.
bool bakeSunVisibility(Context& ctx);
// Fase 3: directa de las luces locales Horneadas y luz rebotada en rgb.
bool bakeIrradiance(Context& ctx);

// --- LightmapperFilter.cpp ---
// Fase 3: a trous guiado sobre la luz rebotada (rgb) de cada pagina.
// `variance`: por texel (misma forma que Page::value).
void denoisePage(const Context& ctx, Page& page, std::vector<Vec3>& irradiance, const std::vector<float>& variance,
                 int iterations);

}  // namespace cramion::lighting::lm

#endif  // CRAMION_CORE_SRC_LIGHTING_LIGHTMAPPER_INTERNAL_H

#ifndef CRAMION_CORE_LIGHTING_BAKE_TRACER_H
#define CRAMION_CORE_LIGHTING_BAKE_TRACER_H

// Trazador de rayos del horneado por texel (Lightmapper, PLAN-LIGHTMAPS.md
// 2.1): BVH binario con SAH por cubos (16), hojas de 4 triangulos como mucho y
// nodos de 32 bytes; el recorrido visita primero el hijo mas cercano (segun el
// signo de la direccion en el eje de corte). Devuelve las baricentricas del
// choque. Solo lectura despues de build(): lo comparten todos los hilos.

#include <CramionFX/core/Math.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cramion::lighting {

struct BakeProgress;

struct BakeHit {
    float t = 0.0f;
    std::uint32_t triangle = 0;  // indice en el orden de build()
    float u = 0.0f, v = 0.0f;    // punto = v0 + e1 * u + e2 * v
    bool backface = false;       // el rayo llega por detras (normal = cross(e1, e2))
};

class BakeTracer {
public:
    // Triangulos en el mundo (v0 y sus dos aristas). `material[t]` indexa
    // `coverage`: < 1 = recorte por alfa, el rayo pasa con probabilidad
    // 1 - coverage (hash del rayo y del triangulo, sin texturas por ahora).
    // Vacios = todo opaco.
    void build(std::vector<core::Vec3> v0, std::vector<core::Vec3> e1, std::vector<core::Vec3> e2,
               std::vector<std::uint32_t> material, std::vector<float> coverage, BakeProgress* progress = nullptr);
    bool empty() const { return nodes_.empty(); }
    std::size_t triangleCount() const { return v0_.size(); }
    std::size_t nodeCount() const { return nodes_.size(); }

    // Choque mas cercano en (0, tmax). `seed`: el del rayo (recorte por alfa).
    bool intersect(const core::Vec3& o, const core::Vec3& d, float tmax, BakeHit& hit, std::uint32_t seed) const;
    // Algo tapa el rayo en (0, tmax)? Sale en el primer choque.
    bool occluded(const core::Vec3& o, const core::Vec3& d, float tmax, std::uint32_t seed) const;

    // Datos de un triangulo (indice de build()).
    core::Vec3 vertex0(std::uint32_t t) const { return v0_[slot_of_[t]]; }
    core::Vec3 edge1(std::uint32_t t) const { return e1_[slot_of_[t]]; }
    core::Vec3 edge2(std::uint32_t t) const { return e2_[slot_of_[t]]; }
    core::Vec3 point(const BakeHit& hit) const;
    core::Vec3 geometricNormal(std::uint32_t t) const;  // normalizada, cross(e1, e2)

private:
    struct Node {
        float min[3];
        std::uint32_t left_or_first;  // interior: primer hijo (el otro va detras); hoja: primer triangulo
        float max[3];
        std::uint32_t count_axis;     // hoja: numero de triangulos (1..4); interior: 0x80000000 | eje
    };
    static_assert(sizeof(Node) == 32, "nodo del BVH de 32 bytes");
    bool passes(std::uint32_t slot, std::uint32_t seed) const;

    std::vector<Node> nodes_;
    // Triangulos en el orden de las hojas (slot) y su indice original.
    std::vector<core::Vec3> v0_, e1_, e2_;
    std::vector<std::uint32_t> original_;  // slot -> indice de build()
    std::vector<std::uint32_t> slot_of_;   // indice de build() -> slot
    std::vector<float> slot_coverage_;     // vacio = todo opaco
};

// Origen de un rayo que sale de una superficie, separado lo justo para no
// chocar con ella (Wachter y Binder, Ray Tracing Gems, cap. 6): en enteros
// de la representacion del float, asi vale lejos y cerca del origen.
core::Vec3 offsetRay(const core::Vec3& p, const core::Vec3& n);

}  // namespace cramion::lighting

#endif  // CRAMION_CORE_LIGHTING_BAKE_TRACER_H

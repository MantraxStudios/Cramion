#ifndef CRAMION_CORE_WATER_RIPPLES_H
#define CRAMION_CORE_WATER_RIPPLES_H

// Olas interactivas del agua (como en Red Dead Redemption): una rejilla de
// alturas alrededor de la camara donde se resuelve la ecuacion de onda. Lo
// que cae al agua o se mueve por ella (Rigidbody, vehiculos, agentes) empuja
// la superficie: salpicadura al entrar, estela al avanzar; las ondas se
// propagan, rebotan entre si y se apagan. El render suma su altura y su
// pendiente a la del oleaje (water.vert / water.frag).
//
// La rejilla sigue a la camara en pasos de una celda (su contenido se
// desplaza, las ondas se quedan quietas en el mundo). Paso fijo de 60 Hz.

#include <CramionFX/core/Math.h>

#include <cstdint>
#include <vector>

namespace cramion::water {

// Algo que empuja el agua en este frame.
struct RippleSource {
    core::Vec3 position{};  // en el mundo (x, z; y no se usa)
    float radius = 0.5f;    // metros
    // Cuanto hunde la superficie por segundo en su centro (m/s): la
    // salpicadura de algo que cae es un empuje grande y corto; la estela, uno
    // pequeno y continuo.
    float push = 0.0f;
};

// Algo quieto que atraviesa la superficie (poste, roca, pilar): las ondas
// chocan con el y rebotan. Rectangulo orientado en el plano xz (o circulo).
struct RippleObstacle {
    float x = 0.0f, z = 0.0f;          // centro
    float axis_x = 1.0f, axis_z = 0.0f;  // eje "u" (unitario); "v" es su perpendicular
    float half_u = 0.5f, half_v = 0.5f;  // medios lados (m)
    bool round = false;                  // circulo de radio max(half_u, half_v)
};

class RippleSimulation {
public:
    static constexpr std::uint32_t kSize = 192;   // celdas por lado
    static constexpr float kCellSize = 0.25f;     // metros: 48 x 48 m

    // Sigue a `center` (la camara) y avanza `delta_seconds` con los empujes.
    void update(const core::Vec3& center, float delta_seconds, const std::vector<RippleSource>& sources,
                const std::vector<RippleObstacle>& obstacles = {});
    void clear();

    // Altura en un punto del mundo (0 fuera de la rejilla), interpolada.
    float heightAt(float x, float z) const;
    // Esquina (x, z minimos) de la rejilla en el mundo.
    float originX() const { return static_cast<float>(cell_x_) * kCellSize; }
    float originZ() const { return static_cast<float>(cell_z_) * kCellSize; }
    // Alturas actuales, kSize x kSize por filas (z), para el render.
    const std::vector<float>& heights() const { return current_; }
    // Hay olas (algo que dibujar).
    bool active() const { return active_; }

    // Velocidad de las ondas (m/s) y cuanto duran.
    float wave_speed = 2.2f;
    float damping = 0.992f;  // por paso

private:
    void shift(int dx, int dz);
    void step(float dt, const std::vector<RippleSource>& sources);

    std::vector<float> current_ = std::vector<float>(kSize * kSize, 0.0f);
    std::vector<float> previous_ = std::vector<float>(kSize * kSize, 0.0f);
    std::vector<float> next_ = std::vector<float>(kSize * kSize, 0.0f);
    std::vector<std::uint8_t> solid_ = std::vector<std::uint8_t>(kSize * kSize, 0);  // celdas de obstaculos
    bool any_solid_ = false;
    void rasterize(const std::vector<RippleObstacle>& obstacles);
    int cell_x_ = 0;  // celda del mundo de la esquina
    int cell_z_ = 0;
    bool placed_ = false;
    bool active_ = false;
    float accumulator_ = 0.0f;
};

}  // namespace cramion::water

#endif  // CRAMION_CORE_WATER_RIPPLES_H

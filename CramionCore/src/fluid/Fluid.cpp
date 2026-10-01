#include "CramionCore/fluid/Fluid.h"

#include "CramionCore/ecs/MathUtil.h"
#include "CramionCore/physics/PhysicsComponents.h"
#include "CramionCore/physics/PhysicsSystem.h"
#include "CramionCore/terrain/Terrain.h"

#include <CramionFX/vk/VulkanRenderer.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <memory>

namespace cramion::fluid {

using core::Quat;
using core::Vec3;
using core::Vec4;
using ecs::FloatRange;
using ecs::Vec3Kind;

namespace {

constexpr std::size_t kTypeCount = static_cast<std::size_t>(FluidType::Count);
static_assert(kTypeCount <= gfx::FluidPass::kMaterials, "un material de la GPU por tipo de liquido");

FluidSystem* g_active = nullptr;

Vec3 absVec(const Vec3& v) { return Vec3{std::fabs(v.x), std::fabs(v.y), std::fabs(v.z)}; }
Vec3 minVec(const Vec3& a, const Vec3& b) { return Vec3{std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
Vec3 maxVec(const Vec3& a, const Vec3& b) { return Vec3{std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }
bool isZero(const Vec3& v) { return v.x == 0.0f && v.y == 0.0f && v.z == 0.0f; }

// Base ortonormal (u, v) perpendicular a `n` (unitario).
void basis(const Vec3& n, Vec3& u, Vec3& v) {
    const Vec3 helper = std::fabs(n.y) < 0.95f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
    u = core::normalize(core::cross(helper, n));
    v = core::cross(n, u);
}

// Distancia con signo a una forma (negativa dentro). La misma que fluid_sim.comp.
float shapeDistance(const gfx::FluidShape& s, const Vec3& p) {
    switch (s.type) {
        case gfx::FluidShapeType::Sphere: return core::length(p - s.a) - s.radius;
        case gfx::FluidShapeType::Box: {
            const Vec3 local = ecs::quatRotate(ecs::quatConjugate(s.rotation), p - s.a);
            const Vec3 e = absVec(local) - s.b;
            const Vec3 outside{std::max(e.x, 0.0f), std::max(e.y, 0.0f), std::max(e.z, 0.0f)};
            return core::length(outside) + std::min(std::max(e.x, std::max(e.y, e.z)), 0.0f);
        }
        case gfx::FluidShapeType::Capsule: {
            const Vec3 ab = s.b - s.a;
            const float t = std::clamp(core::dot(p - s.a, ab) / std::max(core::dot(ab, ab), 1e-8f), 0.0f, 1.0f);
            return core::length(p - (s.a + ab * t)) - s.radius;
        }
        case gfx::FluidShapeType::Plane: return core::dot(p, s.a) - s.b.x;
    }
    return 1e30f;
}

// Caja del mundo que envuelve una forma.
void shapeBounds(const gfx::FluidShape& s, Vec3& lo, Vec3& hi) {
    switch (s.type) {
        case gfx::FluidShapeType::Sphere:
            lo = s.a - Vec3{s.radius};
            hi = s.a + Vec3{s.radius};
            return;
        case gfx::FluidShapeType::Box: {
            const Vec3 ax = absVec(ecs::quatRotate(s.rotation, Vec3{1.0f, 0.0f, 0.0f}));
            const Vec3 ay = absVec(ecs::quatRotate(s.rotation, Vec3{0.0f, 1.0f, 0.0f}));
            const Vec3 az = absVec(ecs::quatRotate(s.rotation, Vec3{0.0f, 0.0f, 1.0f}));
            const Vec3 extent = ax * s.b.x + ay * s.b.y + az * s.b.z;
            lo = s.a - extent;
            hi = s.a + extent;
            return;
        }
        case gfx::FluidShapeType::Capsule:
            lo = minVec(s.a, s.b) - Vec3{s.radius};
            hi = maxVec(s.a, s.b) + Vec3{s.radius};
            return;
        case gfx::FluidShapeType::Plane: break;
    }
    lo = Vec3{-1e30f};
    hi = Vec3{1e30f};
}

bool overlaps(const Vec3& a_lo, const Vec3& a_hi, const Vec3& b_lo, const Vec3& b_hi) {
    return a_lo.x <= b_hi.x && a_hi.x >= b_lo.x && a_lo.y <= b_hi.y && a_hi.y >= b_lo.y && a_lo.z <= b_hi.z &&
           a_hi.z >= b_lo.z;
}

// Minusculas y sin acentos (UTF-8 de las vocales y la enye).
std::string normalizeName(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        const auto c = static_cast<unsigned char>(in[i]);
        if (c == 0xC3 && i + 1 < in.size()) {
            const auto n = static_cast<unsigned char>(in[i + 1]);
            char plain = 0;
            switch (n) {
                case 0xA1: case 0x81: plain = 'a'; break;
                case 0xA9: case 0x89: plain = 'e'; break;
                case 0xAD: case 0x8D: plain = 'i'; break;
                case 0xB3: case 0x93: plain = 'o'; break;
                case 0xBA: case 0x9A: case 0xBC: case 0x9C: plain = 'u'; break;
                case 0xB1: case 0x91: plain = 'n'; break;
                default: break;
            }
            if (plain != 0) {
                out.push_back(plain);
                ++i;
                continue;
            }
        }
        if (c == ' ' || c == '_' || c == '-') continue;
        out.push_back(c < 0x80 ? static_cast<char>(std::tolower(c)) : static_cast<char>(c));
    }
    return out;
}

std::uint64_t hashBytes(std::uint64_t h, const void* data, std::size_t size) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

template <typename T>
std::uint64_t hashValue(std::uint64_t h, const T& value) {
    return hashBytes(h, &value, sizeof(value));
}

int typeIndex(float w) { return std::clamp(static_cast<int>(w + 0.5f), 0, static_cast<int>(kTypeCount) - 1); }

}  // namespace

// -----------------------------------------------------------------------------
// Tipos de liquido
// -----------------------------------------------------------------------------

const std::array<const char*, kTypeCount>& fluidTypeNames() {
    static const std::array<const char*, kTypeCount> names = {"Agua",   "Aceite", "Miel",  "Lava",
                                                              "Barro",  "Sangre", "Ácido", "Personalizado"};
    return names;
}

const std::array<const char*, kTypeCount>& fluidTypeKeys() {
    static const std::array<const char*, kTypeCount> keys = {"water", "oil",   "honey", "lava",
                                                             "mud",   "blood", "acid",  "custom"};
    return keys;
}

int fluidTypeFromName(const std::string& name) {
    const std::string wanted = normalizeName(name);
    if (wanted.empty()) return -1;
    for (std::size_t i = 0; i < kTypeCount; ++i) {
        if (wanted == normalizeName(fluidTypeNames()[i]) || wanted == fluidTypeKeys()[i]) return static_cast<int>(i);
    }
    // Algunos alias.
    if (wanted == "personalizada" || wanted == "user") return static_cast<int>(FluidType::Custom);
    if (wanted == "magma") return static_cast<int>(FluidType::Lava);
    if (wanted == "petroleo" || wanted == "aceitedecocina") return static_cast<int>(FluidType::Oil);
    if (wanted == "lodo") return static_cast<int>(FluidType::Mud);
    return -1;
}

float fluidDensity(FluidType type) {
    switch (type) {
        case FluidType::Water: return 1000.0f;
        case FluidType::Oil: return 900.0f;
        case FluidType::Honey: return 1420.0f;
        case FluidType::Lava: return 2600.0f;
        case FluidType::Mud: return 1700.0f;
        case FluidType::Blood: return 1060.0f;
        case FluidType::Acid: return 1200.0f;
        default: return 1000.0f;
    }
}

gfx::FluidMaterial fluidPreset(FluidType type) {
    gfx::FluidMaterial m{};  // agua por defecto
    switch (type) {
        case FluidType::Water:
            m.absorption = Vec3{0.9f, 0.25f, 0.18f};
            m.scatter = Vec3{0.02f, 0.07f, 0.09f};
            m.viscosity = 0.01f;
            m.cohesion = 0.5f;
            m.vorticity = 0.3f;
            m.foam = 1.0f;
            m.roughness = 0.03f;
            break;
        case FluidType::Oil:
            m.absorption = Vec3{1.6f, 2.8f, 9.0f};
            m.scatter = Vec3{0.30f, 0.20f, 0.03f};
            m.viscosity = 0.15f;
            m.cohesion = 0.8f;
            m.vorticity = 0.1f;
            m.foam = 0.1f;
            m.roughness = 0.05f;
            m.damping = 0.1f;
            break;
        case FluidType::Honey:
            m.absorption = Vec3{2.0f, 5.5f, 18.0f};
            m.scatter = Vec3{0.55f, 0.30f, 0.04f};
            m.viscosity = 0.85f;
            m.cohesion = 2.0f;
            m.vorticity = 0.0f;
            m.foam = 0.0f;
            m.roughness = 0.08f;
            m.damping = 0.5f;
            break;
        case FluidType::Lava:
            m.absorption = Vec3{40.0f, 40.0f, 40.0f};
            m.scatter = Vec3{0.06f, 0.025f, 0.015f};
            m.emission = Vec3{6.0f, 1.4f, 0.15f};
            m.viscosity = 0.9f;
            m.cohesion = 2.5f;
            m.vorticity = 0.0f;
            m.foam = 0.0f;
            m.roughness = 0.6f;
            m.damping = 0.8f;
            m.crust = 0.7f;
            break;
        case FluidType::Mud:
            m.absorption = Vec3{20.0f, 22.0f, 25.0f};
            m.scatter = Vec3{0.18f, 0.12f, 0.07f};
            m.viscosity = 0.7f;
            m.cohesion = 1.5f;
            m.vorticity = 0.0f;
            m.foam = 0.0f;
            m.roughness = 0.5f;
            m.damping = 0.4f;
            break;
        case FluidType::Blood:
            m.absorption = Vec3{3.0f, 40.0f, 40.0f};
            m.scatter = Vec3{0.35f, 0.01f, 0.01f};
            m.viscosity = 0.3f;
            m.cohesion = 1.2f;
            m.vorticity = 0.05f;
            m.foam = 0.05f;
            m.roughness = 0.1f;
            m.damping = 0.1f;
            break;
        case FluidType::Acid:
            m.absorption = Vec3{3.0f, 0.4f, 4.0f};
            m.scatter = Vec3{0.20f, 0.60f, 0.05f};
            m.emission = Vec3{0.15f, 0.8f, 0.05f};
            m.viscosity = 0.05f;
            m.cohesion = 0.5f;
            m.vorticity = 0.2f;
            m.foam = 0.6f;
            m.roughness = 0.05f;
            break;
        case FluidType::Custom:
            m.absorption = Vec3{2.0f, 6.0f, 1.5f};
            m.scatter = Vec3{0.30f, 0.05f, 0.45f};
            m.viscosity = 0.2f;
            m.cohesion = 1.0f;
            m.vorticity = 0.1f;
            m.foam = 0.3f;
            m.roughness = 0.05f;
            break;
        default: break;
    }
    return m;
}

// -----------------------------------------------------------------------------
// Componentes
// -----------------------------------------------------------------------------

void FluidWorld::reflect(ecs::PropertyVisitor& v) {
    if (v.beginGroup("Dominio", true)) {
        v.field({"size", "Tamaño (m)", "Caja donde vive el liquido, centrada en el objeto (sin giro)"}, size,
                Vec3Kind::Scale);
        v.field({"solid_walls", "Paredes sólidas", "La caja es un tanque invisible: el liquido no sale"}, solid_walls);
        v.field({"kill_outside", "Borrar fuera", "Lo que sale de la caja desaparece"}, kill_outside);
        v.endGroup();
    }
    if (v.beginGroup("Simulación", true)) {
        v.field({"particle_radius", "Radio de partícula",
                 "Mas pequeno = mas detalle y mas particulas para el mismo volumen (2 cm .. 10 cm)"},
                particle_radius, FloatRange{0.01f, 0.5f, 0.001f, "%.3f m"});
        v.field({"max_particles", "Máximo de partículas", "Capacidad (memoria de la GPU: ~200 bytes por particula)"},
                max_particles, 1024, static_cast<int>(gfx::FluidPass::kMaxParticles));
        v.field({"gravity", "Gravedad", "m/s2"}, gravity, Vec3Kind::Position);
        v.field({"iterations", "Iteraciones", "Del solver de densidad: mas = menos compresible (y mas caro)"}, iterations,
                1, 10);
        v.field({"substep_rate", "Pasos por segundo", "Pasos fijos de la simulacion (Hz)"}, substep_rate,
                FloatRange{30.0f, 480.0f, 1.0f, "%.0f Hz"});
        v.field({"max_substeps", "Pasos máximos por frame", "Si el frame tarda mas, el liquido va mas lento (no explota)"},
                max_substeps, 1, 8);
        v.field({"friction", "Fricción", "Contra los colliders y el terreno"}, friction,
                FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
        v.field({"surface_tension", "Tensión superficial", "Escala la cohesion de cada liquido (gotas, chorros)"},
                surface_tension, FloatRange{0.0f, 5.0f, 0.01f, "%.2f"});
        v.field({"max_speed", "Velocidad máxima", "m/s"}, max_speed, FloatRange{1.0f, 100.0f, 0.1f, "%.1f m/s"});
        v.endGroup();
    }
    if (v.beginGroup("Choques e interacción", true)) {
        v.field({"collide_scene", "Chocar con colliders",
                 "Box, Sphere, Capsule y Plane Collider; Mesh Collider convexo o dinamico como su caja"},
                collide_scene);
        v.field({"collide_terrain", "Chocar con el terreno"}, collide_terrain);
        v.field({"push_bodies", "Empujar Rigidbody", "Flotacion y arrastre de los cuerpos dinamicos"}, push_bodies);
        v.field({"buoyancy", "Flotación", "Escala del empuje (Arquimedes con la densidad de cada liquido)"}, buoyancy,
                FloatRange{0.0f, 5.0f, 0.01f, "%.2f"});
        v.field({"drag", "Arrastre", "Cuanto arrastra la corriente a los cuerpos (mas en los viscosos)"}, drag,
                FloatRange{0.0f, 10.0f, 0.01f, "%.2f"});
        v.field({"simulate_in_editor", "Simular en el editor", "Ver el liquido moverse sin dar a Play"},
                simulate_in_editor);
        v.endGroup();
    }
    if (v.beginGroup("Aspecto", true)) {
        v.field({"render_radius", "Tamaño de las esferas", "x radio de particula: mas = superficie mas llena"},
                render_radius, FloatRange{0.5f, 3.0f, 0.01f, "%.2f"});
        v.field({"smoothing", "Suavizado", "Superficie mas lisa (0 = ver las gotas)"}, smoothing,
                FloatRange{0.0f, 2.0f, 0.01f, "%.2f", true});
        v.field({"thickness", "Grosor", "Escala de la absorcion (color mas intenso)"}, thickness,
                FloatRange{0.0f, 5.0f, 0.01f, "%.2f"});
        v.field({"refraction", "Refracción", "Cuanto se deforma lo que hay detras"}, refraction,
                FloatRange{0.0f, 4.0f, 0.01f, "%.2f"});
        v.field({"debug_particles", "Ver partículas", "Dibuja las particulas sin superficie (depuracion)"},
                debug_particles);
        v.endGroup();
    }
    if (v.beginGroup("Líquido personalizado", false)) {
        v.field({"custom_color", "Color", "Color difuso (lo que se ve con grosor)"}, custom_color, Vec3Kind::Color);
        v.field({"custom_absorption", "Absorción", "1/m por canal: mas = mas opaco en ese color"}, custom_absorption,
                Vec3Kind::ColorHdr);
        v.field({"custom_emission", "Emisión", "Brillo propio (HDR)"}, custom_emission, Vec3Kind::ColorHdr);
        v.field({"custom_viscosity", "Viscosidad"}, custom_viscosity, FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
        v.field({"custom_cohesion", "Cohesión"}, custom_cohesion, FloatRange{0.0f, 5.0f, 0.01f, "%.2f"});
        v.field({"custom_density", "Densidad", "kg/m3 (flotacion)"}, custom_density,
                FloatRange{100.0f, 20000.0f, 10.0f, "%.0f kg/m3"});
        v.endGroup();
    }
}

void FluidEmitter::reflect(ecs::PropertyVisitor& v) {
    static constexpr const char* kShapes[] = {"Boquilla (chorro)", "Caja (se llena)", "Esfera (se llena)"};
    ecs::enumField(v, {"fluid", "Líquido"}, fluid, std::span<const char* const>(fluidTypeNames()));
    ecs::enumField(v, {"shape", "Forma"}, shape, kShapes);
    v.field({"emitting", "Emitiendo"}, emitting);
    v.field({"direction", "Dirección", "Local: gira con el objeto"}, direction, Vec3Kind::Direction);
    v.field({"speed", "Velocidad", "m/s del chorro"}, speed, FloatRange{0.0f, 50.0f, 0.01f, "%.2f m/s"});
    v.field({"nozzle_radius", "Radio de la boquilla"}, nozzle_radius, FloatRange{0.01f, 2.0f, 0.001f, "%.3f m"});
    v.field({"rate", "Caudal", "Particulas por segundo (0 = chorro lleno del tamano de la boquilla)"}, rate,
            FloatRange{0.0f, 200000.0f, 10.0f, "%.0f /s"});
    v.field({"spread", "Dispersión", "Grados (spray)"}, spread, FloatRange{0.0f, 45.0f, 0.1f, "%.1f°"});
    v.field({"size", "Tamaño de la caja", "Forma Caja (m)"}, size, Vec3Kind::Scale);
    v.field({"radius", "Radio de la esfera", "Forma Esfera (m)"}, radius, FloatRange{0.05f, 10.0f, 0.01f, "%.2f m"});
    v.field({"repeat", "Repetir", "Caja/Esfera: volver a llenar cada intervalo"}, repeat);
    v.field({"interval", "Intervalo"}, interval, FloatRange{0.1f, 60.0f, 0.01f, "%.2f s"});
    v.field({"delay", "Retraso", "Segundos antes de empezar"}, delay, FloatRange{0.0f, 60.0f, 0.01f, "%.2f s"});
    v.field({"lifetime", "Vida", "Segundos que dura cada particula (0 = siempre)"}, lifetime,
            FloatRange{0.0f, 600.0f, 0.01f, "%.2f s"});
    v.field({"max_total", "Máximo total", "Particulas que crea en total (0 = sin limite)"}, max_total, 0, 1000000);
}

void FluidDrain::reflect(ecs::PropertyVisitor& v) {
    v.field({"size", "Tamaño (m)", "Caja (gira con el objeto) donde el liquido desaparece"}, size, Vec3Kind::Scale);
}

void registerFluidComponents() {
    ecs::ComponentRegistry& registry = ecs::ComponentRegistry::instance();
    if (registry.find("FluidWorld") == nullptr) {
        registry.registerComponent<FluidWorld>("FluidWorld", "Mundo de líquidos", "Efectos");
    }
    if (registry.find("FluidEmitter") == nullptr) {
        registry.registerComponent<FluidEmitter>("FluidEmitter", "Emisor de líquido", "Efectos");
    }
    if (registry.find("FluidDrain") == nullptr) {
        registry.registerComponent<FluidDrain>("FluidDrain", "Desagüe de líquido", "Efectos");
    }
}

FluidSystem* activeSystem() { return g_active; }
void setActiveSystem(FluidSystem* system) { g_active = system; }

// -----------------------------------------------------------------------------
// Sistema
// -----------------------------------------------------------------------------

FluidSystem::~FluidSystem() {
    if (g_active == this) g_active = nullptr;
}

bool FluidSystem::previewInEditor(ecs::World& world) {
    for (const entt::entity handle : world.registry().view<FluidWorld>()) {
        const ecs::Entity e = world.wrap(handle);
        if (!e.activeInHierarchy()) continue;
        return e.get<FluidWorld>().simulate_in_editor;
    }
    return false;
}

void FluidSystem::clear() {
    clear_requested_ = true;
    has_fallback_ = false;
    pending_.clear();
    emitters_.clear();
    accumulator_ = 0.0f;
    positions_.clear();
    velocities_.clear();
    type_counts_.fill(0);
    grid_dirty_ = true;
    stats_.particles = 0;
    stats_.floating_bodies = 0;
}

void FluidSystem::shiftOrigin(const Vec3& offset) {
    // El mundo resta `offset` a todo; la GPU suma su desplazamiento.
    shift_ = shift_ - offset;
    for (Vec4& p : positions_) {
        p.x -= offset.x;
        p.y -= offset.y;
        p.z -= offset.z;
    }
    for (gfx::FluidSpawn& s : pending_) s.position = s.position - offset;
    fallback_center_ = fallback_center_ - offset;
    grid_dirty_ = true;
}

void FluidSystem::queue(const gfx::FluidSpawn& s) {
    if (pending_.size() < gfx::FluidPass::kMaxParticles) pending_.push_back(s);
}

void FluidSystem::spawn(const Vec3& position, int count, FluidType type, const Vec3& velocity, float radius,
                        float lifetime) {
    count = std::clamp(count, 0, static_cast<int>(gfx::FluidPass::kMaxParticles));
    if (count <= 0) return;
    const float d = 2.0f * radius_;
    if (radius <= 0.0f) {
        const float volume = static_cast<float>(count) * d * d * d;
        radius = std::cbrt(3.0f * volume / (4.0f * core::kPi)) + 0.5f * d;
    }
    // Sin mundo de liquidos: el dominio por defecto, con esto arriba.
    if (!has_fallback_) {
        fallback_center_ = position - Vec3{0.0f, 2.0f, 0.0f};
        has_fallback_ = true;
    }
    const int n = std::max(1, static_cast<int>(std::ceil(radius / d)));
    const auto material = static_cast<std::uint32_t>(std::clamp(static_cast<int>(type), 0, static_cast<int>(kTypeCount) - 1));
    std::uniform_real_distribution<float> jitter(-0.05f * d, 0.05f * d);
    int made = 0;
    for (int z = -n; z <= n && made < count; ++z) {
        for (int y = -n; y <= n && made < count; ++y) {
            for (int x = -n; x <= n && made < count; ++x) {
                const Vec3 local{static_cast<float>(x) * d, static_cast<float>(y) * d, static_cast<float>(z) * d};
                if (core::length(local) > radius) continue;
                gfx::FluidSpawn s;
                s.position = position + local + Vec3{jitter(random_), jitter(random_), jitter(random_)};
                s.material = material;
                s.velocity = velocity;
                s.lifetime = std::max(lifetime, 0.0f);
                queue(s);
                ++made;
            }
        }
    }
}

void FluidSystem::restart(ecs::Entity emitter) {
    if (emitter.valid()) emitters_.erase(emitter.handle());
}

std::uint32_t FluidSystem::particleCount(FluidType type) const {
    const auto index = static_cast<std::size_t>(type);
    return index < type_counts_.size() ? type_counts_[index] : 0u;
}

void FluidSystem::emit(ecs::Entity entity, const FluidEmitter& emitter, EmitterState& state, float dt) {
    if (!emitter.emitting) return;
    state.time += dt;
    if (state.time < emitter.delay) return;
    const core::Mat4& m = entity.worldMatrix();
    if (emitter.shape != EmitterShape::Nozzle) {
        if (!state.filled) {
            fillVolume(m, emitter, state);
            state.filled = true;
            state.refill = 0.0f;
        } else if (emitter.repeat) {
            state.refill += dt;
            if (state.refill >= std::max(emitter.interval, 0.1f)) {
                state.refill = 0.0f;
                fillVolume(m, emitter, state);
            }
        }
        return;
    }

    // Sitio libre (lo que hay + lo que espera).
    const std::uint64_t used = static_cast<std::uint64_t>(stats_.particles) + pending_.size();
    std::uint64_t room = stats_.capacity > used ? stats_.capacity - used : 0;
    if (room == 0) return;
    const auto limitOk = [&]() { return emitter.max_total <= 0 || state.emitted < emitter.max_total; };
    if (!limitOk()) return;

    Vec3 dir = ecs::transformDirection(m, emitter.direction);
    dir = core::length(dir) > 1e-6f ? core::normalize(dir) : Vec3{0.0f, -1.0f, 0.0f};
    const Vec3 origin = ecs::transformPoint(m, Vec3{});
    Vec3 u{};
    Vec3 v{};
    basis(dir, u, v);
    const float d = 2.0f * radius_;
    const float speed = std::max(emitter.speed, 0.0f);
    const float spread = std::tan(core::radians(std::clamp(emitter.spread, 0.0f, 45.0f)));
    const float lifetime = std::max(emitter.lifetime, 0.0f);
    const auto material = static_cast<std::uint32_t>(emitter.fluid);
    std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
    std::uniform_real_distribution<float> unit01(0.0f, 1.0f);
    const auto velocity = [&]() {
        Vec3 vel = dir * speed;
        if (spread > 0.0f) vel = vel + (u * unit(random_) + v * unit(random_)) * (std::max(speed, 0.5f) * spread);
        return vel;
    };
    const auto push = [&](const Vec3& p) {
        gfx::FluidSpawn s;
        s.position = p;
        s.material = material;
        s.velocity = velocity();
        s.lifetime = lifetime;
        queue(s);
        ++state.emitted;
        --room;
    };
    const float nozzle = std::max(emitter.nozzle_radius, 0.0f);

    if (emitter.rate > 0.0f) {
        // Caudal fijo: puntos al azar en la boquilla.
        state.rate_carry += emitter.rate * dt;
        int count = static_cast<int>(state.rate_carry);
        state.rate_carry -= static_cast<float>(count);
        count = std::min(count, 20000);
        for (int i = 0; i < count && room > 0 && limitOk(); ++i) {
            const float r = nozzle * std::sqrt(unit01(random_));
            const float a = 2.0f * core::kPi * unit01(random_);
            push(origin + u * (std::cos(a) * r) + v * (std::sin(a) * r) + dir * (speed * dt * unit01(random_)));
        }
        return;
    }

    // Chorro lleno: una capa de la boquilla cada `d / velocidad` segundos.
    const float interval = d / std::max(speed, 0.2f);
    state.layer_time += dt;
    constexpr int kMaxLayers = 16;
    int layers = 0;
    const int n = static_cast<int>(std::floor(nozzle / d));
    while (state.layer_time >= interval && layers < kMaxLayers && room > 0 && limitOk()) {
        state.layer_time -= interval;
        ++layers;
        const Vec3 base = origin + dir * (state.layer_time * speed);  // lo que ya avanzo esta capa
        const float offset = (state.layer & 1u) != 0u ? 0.5f * d : 0.0f;
        ++state.layer;
        if (nozzle < d) {
            push(base);
            continue;
        }
        for (int j = -n - 1; j <= n && room > 0 && limitOk(); ++j) {
            for (int i = -n - 1; i <= n && room > 0 && limitOk(); ++i) {
                const float x = static_cast<float>(i) * d + offset;
                const float y = static_cast<float>(j) * d + offset;
                if (x * x + y * y > nozzle * nozzle) continue;
                push(base + u * x + v * y);
            }
        }
    }
    if (layers >= kMaxLayers) state.layer_time = std::min(state.layer_time, interval);
}

void FluidSystem::fillVolume(const core::Mat4& matrix, const FluidEmitter& emitter, EmitterState& state) {
    Vec3 position{};
    Quat rotation{};
    Vec3 scale{};
    ecs::decomposeMatrix(matrix, position, rotation, scale);
    scale = absVec(scale);
    const float d = 2.0f * radius_;
    const bool sphere = emitter.shape == EmitterShape::Sphere;
    const float sphere_radius = std::max(emitter.radius, 0.0f) * std::max({scale.x, scale.y, scale.z});
    const Vec3 half = sphere ? Vec3{sphere_radius} : absVec(emitter.size * scale) * 0.5f;
    const int nx = std::max(1, static_cast<int>(std::floor(2.0f * half.x / d)));
    const int ny = std::max(1, static_cast<int>(std::floor(2.0f * half.y / d)));
    const int nz = std::max(1, static_cast<int>(std::floor(2.0f * half.z / d)));

    const std::uint64_t used = static_cast<std::uint64_t>(stats_.particles) + pending_.size();
    std::uint64_t room = stats_.capacity > used ? stats_.capacity - used : 0;
    const auto material = static_cast<std::uint32_t>(emitter.fluid);
    const float lifetime = std::max(emitter.lifetime, 0.0f);
    std::uniform_real_distribution<float> jitter(-0.05f * d, 0.05f * d);
    // Abajo primero: si no cabe todo, se llena desde el fondo.
    for (int y = 0; y < ny && room > 0; ++y) {
        for (int z = 0; z < nz && room > 0; ++z) {
            for (int x = 0; x < nx && room > 0; ++x) {
                if (emitter.max_total > 0 && state.emitted >= emitter.max_total) return;
                const Vec3 local{(static_cast<float>(x) - 0.5f * static_cast<float>(nx - 1)) * d,
                                 (static_cast<float>(y) - 0.5f * static_cast<float>(ny - 1)) * d,
                                 (static_cast<float>(z) - 0.5f * static_cast<float>(nz - 1)) * d};
                if (sphere && core::length(local) > sphere_radius - 0.5f * radius_) continue;
                gfx::FluidSpawn s;
                s.position = position + ecs::quatRotate(rotation, local) +
                             Vec3{jitter(random_), jitter(random_), jitter(random_)};
                s.material = material;
                s.velocity = Vec3{};
                s.lifetime = lifetime;
                queue(s);
                ++state.emitted;
                --room;
            }
        }
    }
}

void FluidSystem::gatherShapes(ecs::World& world, const Vec3& lo, const Vec3& hi) {
    shapes_.clear();
    bodies_.clear();
    entt::registry& registry = world.registry();
    std::vector<entt::entity> candidates;
    for (const entt::entity h : registry.view<physics::BoxCollider>()) candidates.push_back(h);
    for (const entt::entity h : registry.view<physics::SphereCollider>()) candidates.push_back(h);
    for (const entt::entity h : registry.view<physics::CapsuleCollider>()) candidates.push_back(h);
    for (const entt::entity h : registry.view<physics::PlaneCollider>()) candidates.push_back(h);
    for (const entt::entity h : registry.view<physics::MeshCollider>()) candidates.push_back(h);
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

    for (const entt::entity handle : candidates) {
        if (shapes_.size() >= gfx::FluidPass::kMaxShapes) break;
        const ecs::Entity e = world.wrap(handle);
        if (!e.activeInHierarchy()) continue;
        const core::Mat4& m = e.worldMatrix();
        Vec3 position{};
        Quat rotation{};
        Vec3 scale{};
        ecs::decomposeMatrix(m, position, rotation, scale);
        scale = absVec(scale);
        const float max_scale = std::max({scale.x, scale.y, scale.z});
        const physics::Rigidbody* rb = e.tryGet<physics::Rigidbody>();
        const bool dynamic = rb != nullptr && rb->type == physics::BodyType::Dynamic;

        const auto first = static_cast<std::uint32_t>(shapes_.size());
        float volume = 0.0f;
        Vec3 body_lo{1e30f};
        Vec3 body_hi{-1e30f};
        const auto add = [&](const gfx::FluidShape& s, float shape_volume) {
            if (shapes_.size() >= gfx::FluidPass::kMaxShapes) return;
            Vec3 a{};
            Vec3 b{};
            shapeBounds(s, a, b);
            if (s.type != gfx::FluidShapeType::Plane && !overlaps(a, b, lo, hi)) return;
            shapes_.push_back(s);
            volume += shape_volume;
            body_lo = minVec(body_lo, a);
            body_hi = maxVec(body_hi, b);
        };

        if (const auto* box = e.tryGet<physics::BoxCollider>(); box != nullptr && !box->material.is_trigger) {
            gfx::FluidShape s;
            s.type = gfx::FluidShapeType::Box;
            s.a = ecs::transformPoint(m, box->center);
            s.b = absVec(box->size * scale) * 0.5f;
            s.rotation = rotation;
            add(s, 8.0f * s.b.x * s.b.y * s.b.z);
        }
        if (const auto* sphere = e.tryGet<physics::SphereCollider>(); sphere != nullptr && !sphere->material.is_trigger) {
            gfx::FluidShape s;
            s.type = gfx::FluidShapeType::Sphere;
            s.a = ecs::transformPoint(m, sphere->center);
            s.radius = std::fabs(sphere->radius) * max_scale;
            add(s, 4.0f / 3.0f * core::kPi * s.radius * s.radius * s.radius);
        }
        if (const auto* capsule = e.tryGet<physics::CapsuleCollider>(); capsule != nullptr && !capsule->material.is_trigger) {
            Vec3 axis{0.0f, 1.0f, 0.0f};
            float axis_scale = scale.y;
            float side_scale = std::max(scale.x, scale.z);
            if (capsule->axis == physics::CapsuleAxis::X) {
                axis = Vec3{1.0f, 0.0f, 0.0f};
                axis_scale = scale.x;
                side_scale = std::max(scale.y, scale.z);
            } else if (capsule->axis == physics::CapsuleAxis::Z) {
                axis = Vec3{0.0f, 0.0f, 1.0f};
                axis_scale = scale.z;
                side_scale = std::max(scale.x, scale.y);
            }
            gfx::FluidShape s;
            s.type = gfx::FluidShapeType::Capsule;
            s.radius = std::fabs(capsule->radius) * side_scale;
            const float half_segment = std::max(std::fabs(capsule->height) * 0.5f * axis_scale - s.radius, 0.0f);
            const Vec3 center = ecs::transformPoint(m, capsule->center);
            const Vec3 world_axis = core::normalize(ecs::quatRotate(rotation, axis));
            s.a = center - world_axis * half_segment;
            s.b = center + world_axis * half_segment;
            add(s, core::kPi * s.radius * s.radius * 2.0f * half_segment +
                       4.0f / 3.0f * core::kPi * s.radius * s.radius * s.radius);
        }
        if (const auto* plane = e.tryGet<physics::PlaneCollider>(); plane != nullptr && !plane->material.is_trigger && !dynamic) {
            gfx::FluidShape s;
            s.type = gfx::FluidShapeType::Plane;
            s.a = core::normalize(ecs::quatRotate(rotation, Vec3{0.0f, 1.0f, 0.0f}));
            s.b = Vec3{core::dot(s.a, position), 0.0f, 0.0f};
            add(s, 0.0f);
        }
        // Mesh Collider: su caja, solo si es convexo o un cuerpo dinamico (la
        // caja de un nivel entero no dejaria sitio al liquido).
        if (const auto* mesh = e.tryGet<physics::MeshCollider>();
            mesh != nullptr && !mesh->material.is_trigger && (mesh->convex || dynamic) && bounds_provider_) {
            Vec3 mn{};
            Vec3 mx{};
            if (bounds_provider_(e, mn, mx)) {
                gfx::FluidShape s;
                s.type = gfx::FluidShapeType::Box;
                s.a = ecs::transformPoint(m, (mn + mx) * 0.5f);
                s.b = absVec((mx - mn) * scale) * 0.5f;
                s.rotation = rotation;
                add(s, 8.0f * s.b.x * s.b.y * s.b.z * 0.6f);
            }
        }

        const auto count = static_cast<std::uint32_t>(shapes_.size()) - first;
        if (dynamic && count > 0) {
            BodyShape body;
            body.entity = handle;
            body.min = body_lo;
            body.max = body_hi;
            body.volume = volume;
            body.first = first;
            body.count = count;
            bodies_.push_back(body);
        }
    }
}

void FluidSystem::updateHeightfield(ecs::World& world, const Vec3& lo, const Vec3& hi, gfx::FluidPass& pass) {
    if (terrain_store_ != nullptr) {
        for (const entt::entity handle : world.registry().view<terrain::Terrain>()) {
            const ecs::Entity e = world.wrap(handle);
            if (!e.activeInHierarchy()) continue;
            const terrain::Terrain& t = e.get<terrain::Terrain>();
            if (!t.collision || t.size <= 0.0f) continue;
            const Vec3 origin = e.worldPosition();
            const float x0 = std::max(lo.x, origin.x);
            const float x1 = std::min(hi.x, origin.x + t.size);
            const float z0 = std::max(lo.z, origin.z);
            const float z1 = std::min(hi.z, origin.z + t.size);
            if (x1 <= x0 || z1 <= z0) continue;
            const std::shared_ptr<terrain::TerrainData> data = terrain_store_->get(t);
            if (!data || data->resolution() < 2) continue;

            const float native = t.size / static_cast<float>(data->resolution() - 1);
            const float extent = std::max(x1 - x0, z1 - z0);
            const float cell = std::max(native, extent / static_cast<float>(gfx::FluidPass::kMaxHeightfield - 2));
            // Alineado a la rejilla del terreno: no cambia al mover el dominio un poco.
            const float sx0 = std::floor((x0 - origin.x) / cell) * cell + origin.x;
            const float sz0 = std::floor((z0 - origin.z) / cell) * cell + origin.z;
            const std::uint32_t w = std::clamp<std::uint32_t>(
                static_cast<std::uint32_t>(std::ceil((x1 - sx0) / cell)) + 2, 2u, gfx::FluidPass::kMaxHeightfield);
            const std::uint32_t h = std::clamp<std::uint32_t>(
                static_cast<std::uint32_t>(std::ceil((z1 - sz0) / cell)) + 2, 2u, gfx::FluidPass::kMaxHeightfield);

            std::uint64_t key = 1469598103934665603ull;
            key = hashValue(key, data->version());
            key = hashValue(key, t.height);
            key = hashValue(key, t.size);
            key = hashValue(key, origin.x);
            key = hashValue(key, origin.y);
            key = hashValue(key, origin.z);
            key = hashValue(key, sx0);
            key = hashValue(key, sz0);
            key = hashValue(key, cell);
            key = hashValue(key, w);
            key = hashValue(key, h);
            if (key == 0) key = 1;
            if (key == height_key_) return;
            height_key_ = key;

            std::vector<float> heights(static_cast<std::size_t>(w) * h);
            for (std::uint32_t y = 0; y < h; ++y) {
                for (std::uint32_t x = 0; x < w; ++x) {
                    const float wx = sx0 + static_cast<float>(x) * cell;
                    const float wz = sz0 + static_cast<float>(y) * cell;
                    const float u = std::clamp((wx - origin.x) / t.size, 0.0f, 1.0f);
                    const float v = std::clamp((wz - origin.z) / t.size, 0.0f, 1.0f);
                    heights[static_cast<std::size_t>(y) * w + x] = data->sample(u, v) * t.height + origin.y;
                }
            }
            pass.setHeightfield(Vec3{sx0, 0.0f, sz0}, cell, w, h, std::move(heights));
            return;
        }
    }
    if (height_key_ != 0) {
        height_key_ = 0;
        pass.setHeightfield(Vec3{}, 1.0f, 0, 0, {});
    }
}

void FluidSystem::indexSnapshot(const gfx::FluidSnapshot& snapshot) {
    const std::size_t count = std::min<std::size_t>(snapshot.count, std::min(snapshot.positions.size(), snapshot.velocities.size()));
    positions_.assign(snapshot.positions.begin(), snapshot.positions.begin() + static_cast<std::ptrdiff_t>(count));
    velocities_.assign(snapshot.velocities.begin(), snapshot.velocities.begin() + static_cast<std::ptrdiff_t>(count));
    type_counts_.fill(0);
    for (const Vec4& p : positions_) ++type_counts_[static_cast<std::size_t>(typeIndex(p.w))];
    cell_ = std::max(4.0f * radius_, 0.02f);
    grid_dirty_ = true;
}

std::uint64_t FluidSystem::cellKey(int x, int y, int z) const {
    const std::uint64_t table = grid_start_.size() - 1;
    const std::uint64_t h = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) * 73856093ull) ^
                            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(y)) * 19349663ull) ^
                            (static_cast<std::uint64_t>(static_cast<std::uint32_t>(z)) * 83492791ull);
    return h & (table - 1);
}

void FluidSystem::buildGrid() const {
    if (!grid_dirty_) return;
    grid_dirty_ = false;
    const std::size_t n = positions_.size();
    std::size_t table = 64;
    while (table < n * 2) table <<= 1;
    grid_start_.assign(table + 1, 0u);
    grid_items_.resize(n);
    std::vector<std::uint32_t> keys(n);
    const float inv = 1.0f / cell_;
    for (std::size_t i = 0; i < n; ++i) {
        const Vec4& p = positions_[i];
        keys[i] = static_cast<std::uint32_t>(cellKey(static_cast<int>(std::floor(p.x * inv)),
                                                     static_cast<int>(std::floor(p.y * inv)),
                                                     static_cast<int>(std::floor(p.z * inv))));
        ++grid_start_[keys[i] + 1];
    }
    for (std::size_t t = 1; t <= table; ++t) grid_start_[t] += grid_start_[t - 1];
    std::vector<std::uint32_t> cursor(grid_start_.begin(), grid_start_.end() - 1);
    for (std::size_t i = 0; i < n; ++i) grid_items_[cursor[keys[i]]++] = static_cast<std::uint32_t>(i);
}

template <typename Fn>
void FluidSystem::forBox(const Vec3& lo, const Vec3& hi, Fn&& fn) const {
    if (positions_.empty()) return;
    const auto inside = [&](const Vec4& p) {
        return p.x >= lo.x && p.x <= hi.x && p.y >= lo.y && p.y <= hi.y && p.z >= lo.z && p.z <= hi.z;
    };
    const float inv = 1.0f / cell_;
    const auto cellOf = [inv](float v) {
        return static_cast<int>(std::floor(std::clamp(v * inv, -1.0e9f, 1.0e9f)));
    };
    const int x0 = cellOf(lo.x), x1 = cellOf(hi.x);
    const int y0 = cellOf(lo.y), y1 = cellOf(hi.y);
    const int z0 = cellOf(lo.z), z1 = cellOf(hi.z);
    const std::int64_t cells = (static_cast<std::int64_t>(x1) - x0 + 1) * (static_cast<std::int64_t>(y1) - y0 + 1) *
                               (static_cast<std::int64_t>(z1) - z0 + 1);
    if (cells <= 0) return;
    if (cells > static_cast<std::int64_t>(positions_.size()) || cells > 32768) {
        for (std::size_t i = 0; i < positions_.size(); ++i) {
            if (inside(positions_[i])) fn(static_cast<std::uint32_t>(i));
        }
        return;
    }
    buildGrid();
    for (int z = z0; z <= z1; ++z) {
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const std::uint64_t key = cellKey(x, y, z);
                for (std::uint32_t k = grid_start_[key]; k < grid_start_[key + 1]; ++k) {
                    const std::uint32_t i = grid_items_[k];
                    const Vec4& p = positions_[i];
                    // Celdas distintas pueden caer en la misma entrada: solo la suya.
                    if (cellOf(p.x) != x || cellOf(p.y) != y || cellOf(p.z) != z) continue;
                    if (inside(p)) fn(i);
                }
            }
        }
    }
}

template <typename Fn>
void FluidSystem::forNear(const Vec3& p, float radius, Fn&& fn) const {
    const float r2 = radius * radius;
    forBox(p - Vec3{radius}, p + Vec3{radius}, [&](std::uint32_t i) {
        const Vec4& q = positions_[i];
        const float dx = q.x - p.x;
        const float dy = q.y - p.y;
        const float dz = q.z - p.z;
        if (dx * dx + dy * dy + dz * dz <= r2) fn(i);
    });
}

float FluidSystem::densityAt(const Vec3& position, float radius) const {
    const float d = 2.0f * radius_;
    const float r = radius > 0.0f ? radius : 2.0f * d;
    std::uint32_t count = 0;
    forNear(position, r, [&](std::uint32_t) { ++count; });
    const float full = (4.0f / 3.0f * core::kPi * r * r * r) / (d * d * d);
    return full > 0.0f ? static_cast<float>(count) / full : 0.0f;
}

Vec3 FluidSystem::velocityAt(const Vec3& position, float radius) const {
    const float r = radius > 0.0f ? radius : 4.0f * radius_;
    Vec3 sum{};
    std::uint32_t count = 0;
    forNear(position, r, [&](std::uint32_t i) {
        const Vec4& v = velocities_[i];
        sum = sum + Vec3{v.x, v.y, v.z};
        ++count;
    });
    return count > 0 ? sum * (1.0f / static_cast<float>(count)) : Vec3{};
}

bool FluidSystem::surfaceHeight(float x, float z, float& height, float radius) const {
    const float r = radius > 0.0f ? radius : 4.0f * radius_;
    const float r2 = r * r;
    bool found = false;
    float top = -1e30f;
    for (const Vec4& p : positions_) {
        const float dx = p.x - x;
        const float dz = p.z - z;
        if (dx * dx + dz * dz > r2) continue;
        top = std::max(top, p.y);
        found = true;
    }
    if (found) height = top + radius_;
    return found;
}

void FluidSystem::applyBodies(ecs::World& world, physics::PhysicsSystem& physics, const FluidWorld& settings, float dt) {
    std::uint32_t floating = 0;
    if (positions_.empty() || dt <= 0.0f) {
        stats_.floating_bodies = 0;
        return;
    }
    const float d = 2.0f * radius_;
    const float particle_volume = d * d * d;
    const float g = core::length(settings.gravity);
    const Vec3 up = g > 1e-4f ? settings.gravity * (-1.0f / g) : Vec3{0.0f, 1.0f, 0.0f};
    std::array<float, kTypeCount> densities{};
    std::array<float, kTypeCount> viscosities{};
    for (std::size_t t = 0; t < kTypeCount; ++t) {
        const auto type = static_cast<FluidType>(t);
        densities[t] = type == FluidType::Custom ? custom_density_ : fluidDensity(type);
        viscosities[t] = type == FluidType::Custom ? settings.custom_viscosity : fluidPreset(type).viscosity;
    }

    for (const BodyShape& body : bodies_) {
        if (!world.valid(body.entity)) continue;
        const ecs::Entity e = world.wrap(body.entity);
        const physics::Rigidbody* rb = e.tryGet<physics::Rigidbody>();
        if (rb == nullptr || rb->type != physics::BodyType::Dynamic || !physics.hasBody(e)) continue;
        std::uint32_t count = 0;
        Vec3 centroid{};
        Vec3 flow{};
        float density_sum = 0.0f;
        float viscosity_sum = 0.0f;
        const float margin = radius_;
        forBox(body.min - Vec3{margin}, body.max + Vec3{margin}, [&](std::uint32_t i) {
            const Vec4& p4 = positions_[i];
            const Vec3 p{p4.x, p4.y, p4.z};
            bool inside = false;
            for (std::uint32_t k = body.first; k < body.first + body.count && k < shapes_.size(); ++k) {
                if (shapeDistance(shapes_[k], p) < 0.5f * radius_) {
                    inside = true;
                    break;
                }
            }
            if (!inside) return;
            ++count;
            centroid = centroid + p;
            const Vec4& v4 = velocities_[i];
            flow = flow + Vec3{v4.x, v4.y, v4.z};
            const auto type = static_cast<std::size_t>(typeIndex(p4.w));
            density_sum += densities[type];
            viscosity_sum += viscosities[type];
        });
        if (count == 0) continue;
        ++floating;
        const float inv = 1.0f / static_cast<float>(count);
        centroid = centroid * inv;
        flow = flow * inv;
        const float rho = density_sum * inv;
        const float body_volume = std::max(body.volume, particle_volume);
        const float submerged = std::min(static_cast<float>(count) * particle_volume, body_volume);
        const float fraction = std::min(submerged / body_volume, 1.0f);

        // Empuje de Arquimedes (contra la gravedad del liquido).
        Vec3 impulse = up * (rho * g * submerged * std::max(settings.buoyancy, 0.0f) * dt);
        // Arrastre: la velocidad del cuerpo se acerca a la del liquido (mas en
        // los viscosos y cuanto mas hundido).
        const float mass = std::max(rb->mass, 0.01f);
        const float k = std::max(settings.drag, 0.0f) * (1.0f + 8.0f * viscosity_sum * inv) * fraction;
        const float pull = std::min(k * dt * 3.0f, 0.9f);
        const Vec3 relative = flow - physics.linearVelocity(e);
        impulse = impulse + relative * (mass * pull);
        physics.addForceAtPosition(e, impulse, centroid, physics::ForceMode::Impulse);
        // Tambien frena el giro.
        const float spin = 1.0f - std::min(k * dt * 1.5f, 0.5f);
        physics.setAngularVelocity(e, physics.angularVelocity(e) * spin);
    }
    stats_.floating_bodies = floating;
}

void FluidSystem::update(ecs::World& world, float delta_seconds, bool simulate, physics::PhysicsSystem* physics,
                         gfx::VulkanRenderer& renderer) {
    gfx::FluidPass& pass = renderer.fluid();
    const float dt = std::clamp(delta_seconds, 0.0f, 0.25f);

    if (clear_requested_) {
        clear_requested_ = false;
        pass.clear();
        snapshot_frame_ = pass.snapshot().frame;
    }
    if (!isZero(shift_)) {
        pass.shiftOrigin(shift_);
        shift_ = Vec3{};
    }

    // El mundo de liquidos (el primero activo) y los emisores.
    const FluidWorld* found = nullptr;
    Vec3 center = fallback_center_;
    for (const entt::entity handle : world.registry().view<FluidWorld>()) {
        const ecs::Entity e = world.wrap(handle);
        if (!e.activeInHierarchy()) continue;
        found = &e.get<FluidWorld>();
        center = e.worldPosition();
        break;
    }
    std::vector<ecs::Entity> emitters;
    for (const entt::entity handle : world.registry().view<FluidEmitter>()) {
        const ecs::Entity e = world.wrap(handle);
        if (e.activeInHierarchy()) emitters.push_back(e);
    }
    // Estados de emisores que ya no existen.
    for (auto it = emitters_.begin(); it != emitters_.end();) {
        if (!world.valid(it->first) || !world.registry().all_of<FluidEmitter>(it->first)) {
            it = emitters_.erase(it);
        } else {
            ++it;
        }
    }
    static const FluidWorld kDefaults{};
    const FluidWorld& fw = found != nullptr ? *found : kDefaults;
    if (found == nullptr && !has_fallback_ && !emitters.empty()) {
        // Sin FluidWorld: el dominio por defecto con el primer emisor arriba.
        center = emitters.front().worldPosition() - Vec3{0.0f, std::fabs(fw.size.y) * 0.5f - 1.0f, 0.0f};
    }

    const bool active = found != nullptr || !emitters.empty() || has_fallback_ || !pending_.empty();
    if (!active) {
        if (was_active_) {
            pass.clear();
            snapshot_frame_ = pass.snapshot().frame;
            positions_.clear();
            velocities_.clear();
            type_counts_.fill(0);
            grid_dirty_ = true;
            emitters_.clear();
        }
        was_active_ = false;
        pass.setActive(false);
        stats_ = FluidSystemStats{};
        return;
    }
    was_active_ = true;
    pass.setActive(true);

    // --- Ajustes ---
    radius_ = std::clamp(fw.particle_radius, 0.005f, 1.0f);
    substep_ = 1.0f / std::clamp(fw.substep_rate, 30.0f, 480.0f);
    custom_density_ = std::max(fw.custom_density, 1.0f);
    gfx::FluidSimSettings sim;
    sim.particle_radius = radius_;
    sim.max_particles = static_cast<std::uint32_t>(
        std::clamp(fw.max_particles, 256, static_cast<int>(gfx::FluidPass::kMaxParticles)));
    sim.gravity = fw.gravity;
    sim.iterations = static_cast<std::uint32_t>(std::clamp(fw.iterations, 1, 10));
    sim.substep = substep_;
    const Vec3 half = absVec(fw.size) * 0.5f;
    sim.domain_min = center - half;
    sim.domain_max = center + half;
    sim.solid_walls = fw.solid_walls;
    sim.kill_outside = fw.kill_outside;
    sim.friction = fw.friction;
    sim.max_speed = fw.max_speed;
    sim.surface_tension = fw.surface_tension;
    pass.setSettings(sim);

    gfx::FluidRenderSettings render;
    render.render_radius = fw.render_radius;
    render.smoothing = fw.smoothing;
    render.thickness = fw.thickness;
    render.refraction = fw.refraction;
    render.debug_particles = fw.debug_particles;
    pass.setRenderSettings(render);

    std::array<gfx::FluidMaterial, gfx::FluidPass::kMaterials> materials{};
    for (std::size_t i = 0; i < materials.size(); ++i) {
        materials[i] = fluidPreset(static_cast<FluidType>(std::min(i, kTypeCount - 1)));
    }
    gfx::FluidMaterial& custom = materials[static_cast<std::size_t>(FluidType::Custom)];
    custom.scatter = fw.custom_color;
    custom.absorption = fw.custom_absorption;
    custom.emission = fw.custom_emission;
    custom.viscosity = fw.custom_viscosity;
    custom.cohesion = fw.custom_cohesion;
    pass.setMaterials(materials);

    // --- Choques ---
    const Vec3 margin{1.0f, 1.0f, 1.0f};
    if (fw.collide_scene) {
        gatherShapes(world, sim.domain_min - margin, sim.domain_max + margin);
    } else {
        shapes_.clear();
        bodies_.clear();
    }
    pass.setShapes(shapes_);
    std::vector<gfx::FluidShape> drains;
    for (const entt::entity handle : world.registry().view<FluidDrain>()) {
        const ecs::Entity e = world.wrap(handle);
        if (!e.activeInHierarchy()) continue;
        Vec3 position{};
        Quat rotation{};
        Vec3 scale{};
        ecs::decomposeMatrix(e.worldMatrix(), position, rotation, scale);
        gfx::FluidShape s;
        s.type = gfx::FluidShapeType::Box;
        s.a = position;
        s.b = absVec(e.get<FluidDrain>().size * scale) * 0.5f;
        s.rotation = rotation;
        drains.push_back(s);
        if (drains.size() >= 16) break;
    }
    pass.setDrains(std::move(drains));
    if (fw.collide_terrain) {
        updateHeightfield(world, sim.domain_min, sim.domain_max, pass);
    } else if (height_key_ != 0) {
        height_key_ = 0;
        pass.setHeightfield(Vec3{}, 1.0f, 0, 0, {});
    }

    // --- Lo que volvio de la GPU ---
    const gfx::FluidSnapshot& snapshot = pass.snapshot();
    if (snapshot.frame != snapshot_frame_) {
        snapshot_frame_ = snapshot.frame;
        indexSnapshot(snapshot);
    }
    stats_.particles = pass.stats().particles;
    stats_.capacity = sim.max_particles;

    // --- Particulas nuevas ---
    if (simulate) {
        for (const ecs::Entity e : emitters) emit(e, e.get<FluidEmitter>(), emitters_[e.handle()], dt);
    }
    if (!pending_.empty()) {
        const std::size_t n = std::min<std::size_t>(pending_.size(), gfx::FluidPass::kMaxSpawnsPerFrame);
        const std::vector<gfx::FluidSpawn> batch(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(n));
        pass.spawn(batch);
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(n));
    }

    // --- Pasos fijos ---
    std::uint32_t steps = 0;
    if (simulate) {
        accumulator_ += dt;
        const auto max_steps = static_cast<std::uint32_t>(std::clamp(fw.max_substeps, 1, 8));
        while (accumulator_ >= substep_ && steps < max_steps) {
            accumulator_ -= substep_;
            ++steps;
        }
        if (steps >= max_steps) accumulator_ = std::min(accumulator_, substep_);
    } else {
        accumulator_ = 0.0f;
    }
    pass.step(steps);

    // --- Cuerpos ---
    if (simulate && fw.push_bodies && physics != nullptr && !bodies_.empty()) {
        applyBodies(world, *physics, fw, dt);
    } else {
        stats_.floating_bodies = 0;
    }

    const gfx::FluidStats gpu = pass.stats();
    stats_.substeps = steps;
    stats_.shapes = static_cast<std::uint32_t>(shapes_.size());
    stats_.emitters = static_cast<std::uint32_t>(emitters.size());
    stats_.memory_bytes = gpu.memory_bytes;
    stats_.active = true;
    stats_.simulating = simulate;
}

}  // namespace cramion::fluid

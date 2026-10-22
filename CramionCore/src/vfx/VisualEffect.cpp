#include "CramionCore/vfx/VisualEffect.h"

#include "CramionCore/asset/AssetManager.h"
#include "CramionCore/asset/MaterialAsset.h"
#include "CramionCore/cvar/CVar.h"
#include "CramionCore/ecs/Prefab.h"
#include "CramionCore/ecs/Reflection.h"
#include "CramionCore/profiling/Profiler.h"

#include <CramionFX/asset/Model.h>
#include <CramionFX/vk/VulkanRenderer.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>

namespace cramion::vfx {

using json = nlohmann::json;
using core::Vec3;
using core::Vec4;

namespace {

VfxSystem* g_active = nullptr;

// --- Escalabilidad: CVars (consola, ventana Variables, scripts, MCP) ---------------
cvar::CVar<bool> g_vfx_scalability("vfx.scalability", true,
                                   "Presupuesto por tipo de efecto (Heroe/Combate/Impacto/Ambiente): distancia, cuantos a la "
                                   "vez, particulas y salidas segun la calidad",
                                   cvar::Saved);
cvar::CVar<int> g_vfx_quality("vfx.quality", -1, "Calidad de los efectos (0 Baja .. 3 Ultra; -1 = la del preset de graficos)",
                              cvar::Saved, -1, 3);
cvar::CVar<int> g_vfx_platform("vfx.platform", -1, "Tabla de la plataforma (0 escritorio, 1 movil; -1 = automatica)",
                               cvar::None, -1, 1);
cvar::CVar<float> g_vfx_time_scale("vfx.timeScale", 1.0f,
                                   "Tiempo de los efectos (0 = pausa, < 1 camara lenta): simulacion y flipbook", cvar::None,
                                   0.0f, 10.0f);
cvar::CVar<bool> g_vfx_curl_baked("vfx.curlNoiseBaked", true,
                                  "Turbulencia con el ruido rizado horneado (32^3) en vez de calcularlo por particula",
                                  cvar::Saved);
cvar::CVar<int> g_vfx_prewarm_frames("vfx.prewarmFrames", -1,
                                     "Frames en los que se reparte el precalentado (-1 = el de la calidad, 1 = de golpe, 0 = sin "
                                     "precalentado)",
                                     cvar::Saved, -1, 240);
// Mezcla Distorsion (ademas, la calidad Baja la apaga: VfxPass::setDistortionQuality).
cvar::CVar<bool> g_vfx_distortion("render.vfx.Distortion", true,
                                  "Efectos del VFX Graph con mezcla Distorsion (calor, ondas de choque); apagada no se "
                                  "dibujan ni se toca el buffer de distorsion",
                                  cvar::Saved);
// Salida Luz: luces de particula por frame entre todos los efectos (ademas,
// la calidad pone su tope: Baja 8, Media 16, Alta y Ultra 32; gana el menor).
cvar::CVar<int> g_vfx_particle_lights("render.vfx.MaxParticleLights", 32,
                                      "Luces de particula (bloque Luz por particula) por frame como mucho; 0 = ninguna",
                                      cvar::Saved, 0, 256);
// Presupuesto por grupo: sin VfxGroup, los efectos de Heroe/Combate/Impacto
// dentro de un prefab cuentan como uno con la raiz del prefab.
cvar::CVar<bool> g_vfx_group_prefabs("vfx.groupPrefabs", true,
                                     "Sin VfxGroup, los efectos (no de Ambiente) de un prefab cuentan como un grupo: se crean "
                                     "o se quitan enteros",
                                     cvar::Saved);
std::atomic<int> g_quality_preset{2};

const char* kEffectTypeKeys[] = {"Hero", "Combat", "Impact", "Ambient", "None"};
const char* kEffectTypeLabels[] = {"Héroe", "Combate", "Impacto", "Ambiente", "Sin tipo"};
const char* kOverBudgetKeys[] = {"DontCreate", "RemoveFarthest", "Pause"};
// Contadores del perfilador por tipo (nombres fijos: el perfilador guarda el puntero).
const char* kCounterInstances[] = {"VFX Heroe instancias", "VFX Combate instancias", "VFX Impacto instancias",
                                   "VFX Ambiente instancias", "VFX Sin tipo instancias"};
const char* kCounterParticles[] = {"VFX Heroe particulas", "VFX Combate particulas", "VFX Impacto particulas",
                                   "VFX Ambiente particulas", "VFX Sin tipo particulas"};
const char* kCounterMs[] = {"VFX Heroe ms", "VFX Combate ms", "VFX Impacto ms", "VFX Ambiente ms", "VFX Sin tipo ms"};

int typeSlot(EffectType type) { return std::clamp(static_cast<int>(type), 0, kEffectTypeSlots - 1); }

// Suma el tiempo de su vida a un contador (ms por tipo).
struct TypeTimer {
    float& target;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    explicit TypeTimer(float& t) : target(t) {}
    ~TypeTimer() {
        target += std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
    }
};

// n * scale con la fraccion que sobra guardada (caudales pequenos no se pierden).
std::uint32_t scaleCount(std::uint32_t n, float scale, float& carry) {
    if (scale >= 0.999f) return n;
    const double v = static_cast<double>(n) * std::max(scale, 0.0f) + carry;
    const double whole = std::floor(v);
    carry = static_cast<float>(v - whole);
    return static_cast<std::uint32_t>(whole);
}

// --- Catalogo de bloques -----------------------------------------------------------

FieldInfo f1(const char* key, const char* label, int slot, float def, float min = 0.0f, float max = 0.0f,
             const char* tip = nullptr) {
    return FieldInfo{key, label, FieldType::Float, slot, {def, 0.0f, 0.0f, 0.0f}, min, max, tip};
}
FieldInfo fAngle(const char* key, const char* label, int slot, float def, float min = 0.0f, float max = 0.0f,
                 const char* tip = nullptr) {
    return FieldInfo{key, label, FieldType::Angle, slot, {def, 0.0f, 0.0f, 0.0f}, min, max, tip};
}
FieldInfo fInt(const char* key, const char* label, int slot, float def, float min = 0.0f, float max = 0.0f,
               const char* tip = nullptr) {
    return FieldInfo{key, label, FieldType::Int, slot, {def, 0.0f, 0.0f, 0.0f}, min, max, tip};
}
FieldInfo fBool(const char* key, const char* label, int slot, bool def, const char* tip = nullptr) {
    return FieldInfo{key, label, FieldType::Bool, slot, {def ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f}, 0.0f, 0.0f, tip};
}
FieldInfo fVec(const char* key, const char* label, int slot, Vec3 def, const char* tip = nullptr) {
    return FieldInfo{key, label, FieldType::Vec3, slot, {def.x, def.y, def.z, 0.0f}, 0.0f, 0.0f, tip};
}
FieldInfo fColor(const char* key, const char* label, int slot, Vec4 def, const char* tip = nullptr) {
    return FieldInfo{key, label, FieldType::Color, slot, {def.x, def.y, def.z, def.w}, 0.0f, 0.0f, tip};
}

std::vector<BlockInfo> buildCatalog() {
    std::vector<BlockInfo> c;
    const auto add = [&](BlockKind kind, Context ctx, const char* key, const char* label, const char* desc,
                         std::vector<FieldInfo> fields) -> BlockInfo& {
        c.push_back(BlockInfo{kind, ctx, key, label, desc, std::move(fields)});
        return c.back();
    };
    // Spawn
    add(BlockKind::ConstantRate, Context::Spawn, "ConstantRate", "Caudal constante", "Particulas por segundo mientras emite",
        {f1("rate", "Por segundo", 0, 30.0f, 0.0f, 100000.0f)});
    add(BlockKind::Burst, Context::Spawn, "Burst", "Ráfaga", "Un golpe de particulas en un momento del ciclo",
        {fInt("count", "Cantidad", 0, 50.0f, 0.0f, 100000.0f), f1("time", "Momento (s)", 1, 0.0f, 0.0f, 3600.0f)});
    add(BlockKind::PeriodicBurst, Context::Spawn, "PeriodicBurst", "Ráfaga periódica", "Ráfagas cada cierto tiempo",
        {fInt("count", "Cantidad", 0, 20.0f, 0.0f, 100000.0f), f1("interval", "Intervalo (s)", 1, 0.5f, 0.01f, 3600.0f),
         fInt("cycles", "Repeticiones", 2, 0.0f, 0.0f, 100000.0f, "0 = sin limite dentro del ciclo")});
    add(BlockKind::RateOverDistance, Context::Spawn, "RateOverDistance", "Caudal por distancia",
        "Particulas por metro que se mueve el emisor (estelas)", {f1("rate", "Por metro", 0, 10.0f, 0.0f, 10000.0f)});
    add(BlockKind::OnEvent, Context::Spawn, "OnEvent", "Al recibir un evento",
        "Nacen al llamar a sendEvent(nombre) desde un script", {fInt("count", "Cantidad", 0, 100.0f, 0.0f, 100000.0f)})
        .has_text = true;
    // Initialize
    add(BlockKind::PositionSphere, Context::Initialize, "PositionSphere", "Posición: esfera", "Nacen en una esfera",
        {fVec("center", "Centro", 0, Vec3{}), f1("radius", "Radio", 3, 0.5f, 0.0f, 1000.0f),
         f1("volume", "Volumen", 4, 1.0f, 0.0f, 1.0f, "0 = solo la superficie, 1 = toda la esfera"),
         fAngle("arc", "Arco", 5, 360.0f, 0.0f, 360.0f)});
    add(BlockKind::PositionBox, Context::Initialize, "PositionBox", "Posición: caja", "Nacen dentro de una caja",
        {fVec("center", "Centro", 0, Vec3{}), fVec("size", "Tamaño", 3, Vec3{1.0f, 1.0f, 1.0f}),
         fBool("surface", "Solo la superficie", 6, false)});
    add(BlockKind::PositionCone, Context::Initialize, "PositionCone", "Posición: cono", "Nacen en un cono hacia +Y",
        {fVec("base", "Base", 0, Vec3{}), f1("radius", "Radio", 3, 0.2f, 0.0f, 1000.0f),
         fAngle("angle", "Ángulo", 4, 25.0f, 0.0f, 89.0f), f1("length", "Largo", 5, 0.0f, 0.0f, 1000.0f),
         f1("volume", "Volumen", 6, 1.0f, 0.0f, 1.0f)});
    add(BlockKind::PositionCircle, Context::Initialize, "PositionCircle", "Posición: círculo", "Nacen en un disco (XZ)",
        {fVec("center", "Centro", 0, Vec3{}), f1("radius", "Radio", 3, 0.5f, 0.0f, 1000.0f),
         f1("volume", "Volumen", 4, 1.0f, 0.0f, 1.0f, "0 = solo el borde")});
    add(BlockKind::PositionLine, Context::Initialize, "PositionLine", "Posición: línea", "Nacen a lo largo de un segmento",
        {fVec("start", "Inicio", 0, Vec3{-0.5f, 0.0f, 0.0f}), fVec("end", "Fin", 3, Vec3{0.5f, 0.0f, 0.0f})});
    add(BlockKind::PositionMesh, Context::Initialize, "PositionMesh", "Posición: superficie de malla",
        "Nacen sobre la superficie de un modelo", {f1("offset", "Separación", 0, 0.0f, -10.0f, 10.0f)})
        .has_mesh = true;
    add(BlockKind::VelocityRandom, Context::Initialize, "VelocityRandom", "Velocidad al azar",
        "Velocidad entre un minimo y un maximo por eje",
        {fVec("min", "Mínimo", 0, Vec3{-1.0f, 1.0f, -1.0f}), fVec("max", "Máximo", 3, Vec3{1.0f, 3.0f, 1.0f}),
         fBool("add", "Sumar", 6, false, "Suma a la velocidad anterior en lugar de sustituirla")});
    add(BlockKind::VelocityFromShape, Context::Initialize, "VelocityFromShape", "Velocidad desde la forma",
        "Hacia fuera de la forma de nacimiento",
        {f1("min", "Rapidez mínima", 0, 1.0f, 0.0f, 1000.0f), f1("max", "Rapidez máxima", 1, 3.0f, 0.0f, 1000.0f),
         fAngle("spread", "Dispersión", 2, 0.0f, 0.0f, 180.0f)});
    add(BlockKind::Lifetime, Context::Initialize, "Lifetime", "Vida", "Segundos que vive cada particula",
        {f1("min", "Mínimo (s)", 0, 1.0f, 0.01f, 600.0f), f1("max", "Máximo (s)", 1, 2.0f, 0.01f, 600.0f)});
    add(BlockKind::Size, Context::Initialize, "Size", "Tamaño", "Tamano inicial (m)",
        {f1("min", "Mínimo", 0, 0.1f, 0.0f, 100.0f), f1("max", "Máximo", 1, 0.2f, 0.0f, 100.0f)});
    add(BlockKind::Color, Context::Initialize, "Color", "Color", "Al azar entre dos colores",
        {fColor("a", "Color A", 0, Vec4{1.0f, 1.0f, 1.0f, 1.0f}), fColor("b", "Color B", 4, Vec4{1.0f, 1.0f, 1.0f, 1.0f})});
    add(BlockKind::Rotation, Context::Initialize, "Rotation", "Giro", "Angulo inicial y velocidad de giro",
        {fAngle("angle_min", "Ángulo mín.", 0, 0.0f, -360.0f, 360.0f), fAngle("angle_max", "Ángulo máx.", 1, 360.0f, -360.0f, 360.0f),
         fAngle("spin_min", "Giro mín. (°/s)", 2, 0.0f, -3600.0f, 3600.0f), fAngle("spin_max", "Giro máx. (°/s)", 3, 0.0f, -3600.0f, 3600.0f)});
    add(BlockKind::FlipbookFrame, Context::Initialize, "FlipbookFrame", "Cuadro del flipbook", "Cuadro inicial al azar",
        {fInt("min", "Mínimo", 0, 0.0f, 0.0f, 1024.0f), fInt("max", "Máximo", 1, 0.0f, 0.0f, 1024.0f),
         f1("speed_variation", "Variación de velocidad", 2, 0.0f, 0.0f, 0.5f,
            "Cada particula anima su flipbook entre (1 - x) y (1 + x) veces la velocidad (0.2 = +-20 %)")});
    add(BlockKind::InheritVelocity, Context::Initialize, "InheritVelocity", "Heredar velocidad",
        "Parte de la velocidad del emisor", {f1("factor", "Factor", 0, 1.0f, -10.0f, 10.0f)});
    add(BlockKind::RibbonAttributes, Context::Initialize, "RibbonAttributes", "Cinta: atributos",
        "Id de la cinta (varias por efecto), ancho y giro de cada punto (salida Cinta)",
        {fInt("id", "Id", 0, 0.0f, 0.0f, 255.0f, "Los puntos con el mismo id forman una cinta"),
         fInt("strands", "Tiras", 1, 1.0f, 1.0f, 16.0f, "Varias cintas a la vez: id, id + 1... por orden de nacimiento"),
         f1("width", "Ancho (m)", 2, 0.1f, 0.0f, 100.0f, "0 = el tamano de la particula"),
         fAngle("twist_min", "Giro mín.", 3, 0.0f, -360.0f, 360.0f, "Gira el ancho alrededor de la cinta"),
         fAngle("twist_max", "Giro máx.", 4, 0.0f, -360.0f, 360.0f),
         fVec("spacing", "Separación", 5, Vec3{}, "Entre tiras (espacio del emisor): tiras paralelas"),
         fBool("id_from_event", "Id del evento", 8, false,
               "Con 'Al recibir un evento de la GPU': una cinta por cada valor del evento (Id + valor). Un proyectil "
               "con 'Cada t segundos: evento al canal' y 'Valor = id de la particula' deja su propia cinta; desde "
               "un script, Effects.pushEvent con el id del proyectil como valor")});
    // Update
    add(BlockKind::Gravity, Context::Update, "Gravity", "Gravedad", "Aceleracion constante",
        {fVec("acceleration", "Aceleración", 0, Vec3{0.0f, -9.81f, 0.0f})});
    add(BlockKind::Wind, Context::Update, "Wind", "Viento", "Arrastra hacia la velocidad del viento",
        {fVec("velocity", "Velocidad", 0, Vec3{2.0f, 0.0f, 0.0f}), f1("coefficient", "Coeficiente", 3, 1.0f, 0.0f, 100.0f)});
    add(BlockKind::Drag, Context::Update, "Drag", "Rozamiento", "Frena las particulas",
        {f1("linear", "Lineal", 0, 0.5f, 0.0f, 100.0f), f1("quadratic", "Cuadrático", 1, 0.0f, 0.0f, 100.0f)});
    add(BlockKind::Turbulence, Context::Update, "Turbulence", "Turbulencia", "Ruido rizado (curl noise)",
        {f1("intensity", "Intensidad", 0, 1.0f, 0.0f, 1000.0f), f1("frequency", "Frecuencia", 1, 1.0f, 0.001f, 100.0f),
         fInt("octaves", "Octavas", 2, 2.0f, 1.0f, 4.0f), f1("speed", "Velocidad del ruido", 3, 0.5f, 0.0f, 100.0f)});
    add(BlockKind::Attractor, Context::Update, "Attractor", "Atractor", "Atrae hacia un punto",
        {fVec("position", "Posición", 0, Vec3{}), f1("strength", "Fuerza", 3, 5.0f, -1000.0f, 1000.0f),
         f1("radius", "Radio", 4, 0.0f, 0.0f, 1000.0f, "0 = sin limite"),
         f1("kill_radius", "Radio que mata", 5, 0.0f, 0.0f, 1000.0f)});
    add(BlockKind::Vortex, Context::Update, "Vortex", "Vórtice", "Gira alrededor de un eje",
        {fVec("center", "Centro", 0, Vec3{}), f1("spin", "Giro", 3, 3.0f, -1000.0f, 1000.0f),
         fVec("axis", "Eje", 4, Vec3{0.0f, 1.0f, 0.0f}), f1("pull", "Atracción al eje", 7, 0.0f, -1000.0f, 1000.0f)});
    add(BlockKind::CollideDepth, Context::Update, "CollideDepth", "Colisión con la escena",
        "Rebota contra lo que ve la camara (buffer de profundidad)",
        {f1("bounce", "Rebote", 0, 0.4f, 0.0f, 1.0f), f1("friction", "Fricción", 1, 0.2f, 0.0f, 1.0f),
         f1("life_loss", "Vida perdida", 2, 0.0f, 0.0f, 1.0f), f1("radius_scale", "Escala del radio", 3, 1.0f, 0.0f, 10.0f),
         f1("thickness", "Grosor", 4, 0.5f, 0.01f, 100.0f), fBool("kill", "Morir al chocar", 5, false),
         fBool("ground_fallback", "Suelo sin la vista", 6, false,
               "Ademas, el terreno bajo el emisor como suelo (plano a su altura): lo que cae fuera de la pantalla "
               "o tapado tambien choca y apunta sus eventos")});
    add(BlockKind::CollidePlane, Context::Update, "CollidePlane", "Colisión con plano", "Un plano infinito",
        {fVec("normal", "Normal", 0, Vec3{0.0f, 1.0f, 0.0f}), f1("distance", "Distancia", 3, 0.0f, -10000.0f, 10000.0f),
         f1("bounce", "Rebote", 4, 0.4f, 0.0f, 1.0f), f1("friction", "Fricción", 5, 0.2f, 0.0f, 1.0f),
         f1("life_loss", "Vida perdida", 6, 0.0f, 0.0f, 1.0f),
         fBool("world_space", "En el mundo", 7, false,
               "Normal y distancia en el mundo (el plano no se mueve con el emisor). Si no, en el espacio del emisor"),
         fBool("follow_ground", "Sobre el terreno", 8, false,
               "Plano horizontal a la altura del terreno bajo el emisor + Distancia (en el mundo, sin depender de la "
               "vista). Sin terreno debajo no choca")});
    add(BlockKind::CollideSphere, Context::Update, "CollideSphere", "Colisión con esfera", "Una esfera solida o hueca",
        {fVec("center", "Centro", 0, Vec3{}), f1("radius", "Radio", 3, 1.0f, 0.0f, 10000.0f),
         f1("bounce", "Rebote", 4, 0.4f, 0.0f, 1.0f), f1("friction", "Fricción", 5, 0.2f, 0.0f, 1.0f),
         fBool("inside", "Por dentro", 6, false)});
    add(BlockKind::KillBox, Context::Update, "KillBox", "Matar en caja",
        "Mueren dentro (o fuera) de una caja. La caja va en el espacio del emisor (se mueve y gira con el) salvo "
        "con 'En el mundo'",
        {fVec("center", "Centro", 0, Vec3{}), fVec("size", "Tamaño", 3, Vec3{10.0f, 10.0f, 10.0f}),
         fBool("outside", "Matar fuera", 6, true),
         fBool("world_space", "En el mundo", 7, false,
               "Centro y tamano en el mundo (caja fija alineada con los ejes). Si no, en el espacio del emisor: con "
               "'Matar fuera' y un emisor que se mueve, la caja va con el")});
    add(BlockKind::SpeedLimit, Context::Update, "SpeedLimit", "Velocidad máxima", "Limita la rapidez",
        {f1("max", "Máxima", 0, 10.0f, 0.0f, 10000.0f), f1("damping", "Amortiguación", 1, 1.0f, 0.0f, 1.0f)});
    add(BlockKind::ConformSphere, Context::Update, "ConformSphere", "Conformar a esfera", "Se pegan a una esfera",
        {fVec("center", "Centro", 0, Vec3{}), f1("radius", "Radio", 3, 1.0f, 0.0f, 10000.0f),
         f1("attraction", "Atracción", 4, 5.0f, 0.0f, 1000.0f), f1("stickiness", "Pegajosidad", 5, 0.5f, 0.0f, 1.0f)});
    // Update: eventos (subemisores en la GPU)
    add(BlockKind::TriggerOnDie, Context::Update, "TriggerOnDie", "Al morir: otro efecto",
        "Cada particula que muere hace nacer particulas de otro efecto (.crvfx) donde murio",
        {fInt("count", "Nacen", 0, 8.0f, 0.0f, 64.0f, "Particulas del otro efecto por cada una que muere"),
         f1("probability", "Probabilidad", 1, 1.0f, 0.0f, 1.0f),
         f1("inherit", "Velocidad heredada", 2, 0.0f, -10.0f, 10.0f, "Parte de su velocidad que reciben (InheritVelocity)")})
        .has_effect = true;
    add(BlockKind::TriggerOnCollide, Context::Update, "TriggerOnCollide", "Al chocar: otro efecto",
        "Cada choque (bloques de colision de antes) hace nacer particulas de otro efecto donde choco",
        {fInt("count", "Nacen", 0, 4.0f, 0.0f, 64.0f, "Particulas del otro efecto por cada choque"),
         f1("probability", "Probabilidad", 1, 1.0f, 0.0f, 1.0f),
         f1("min_speed", "Rapidez mínima (m/s)", 2, 1.0f, 0.0f, 1000.0f, "Los roces mas lentos no cuentan (las que ruedan)"),
         f1("inherit", "Velocidad heredada", 3, 0.3f, -10.0f, 10.0f, "Parte de su velocidad (ya rebotada) que reciben")})
        .has_effect = true;
    // Update: canales de datos (eventos con nombre en la GPU). Los tres Emit*
    // comparten huecos: 0 tipo, 1 valor, 2 probabilidad, 3..6 color, 7 por el
    // color de la particula, 8 rapidez minima / periodo.
    const auto emit_fields = [](std::vector<FieldInfo> extra) {
        std::vector<FieldInfo> fields = {
            fInt("type", "Tipo", 0, 0.0f, 0.0f, 65535.0f, "Numero libre: los que escuchan pueden quedarse solo con un tipo"),
            f1("value", "Valor", 1, 1.0f, -1000000.0f, 1000000.0f, "Dato libre (fuerza, dano...); quien escucha puede usarlo como tamano"),
            f1("probability", "Probabilidad", 2, 1.0f, 0.0f, 1.0f),
            fColor("color", "Color", 3, Vec4{1.0f, 1.0f, 1.0f, 1.0f}),
            fBool("particle_color", "Por el color de la particula", 7, true, "Multiplica el color por el de la particula"),
            fBool("particle_id_value", "Valor = id de la partícula", 9, false,
                  "El valor del evento es el id de la particula que lo apunta (igual toda su vida): las cintas con "
                  "'Id del evento' hacen una por particula")};
        for (FieldInfo& f : extra) fields.push_back(f);
        return fields;
    };
    BlockInfo& emit_death = add(BlockKind::EmitOnDeath, Context::Update, "EmitOnDeath", "Al morir: evento al canal",
                                "Cada particula que muere apunta un evento {tipo, posicion, velocidad, color, valor} en un "
                                "canal; lo leen los efectos con 'Al recibir un evento de la GPU' (el frame siguiente)",
                                emit_fields({}));
    emit_death.has_text = true;
    emit_death.text_label = "Canal";
    BlockInfo& emit_collision = add(BlockKind::EmitOnCollision, Context::Update, "EmitOnCollision", "Al chocar: evento al canal",
                                    "Cada choque (CollideDepth, CollidePlane o CollideSphere de antes) apunta un evento en el canal. "
                                    "Para que choque tambien fuera de la vista: CollideDepth con 'Suelo sin la vista' o "
                                    "CollidePlane con 'Sobre el terreno' / 'En el mundo'",
                                    emit_fields({f1("min_speed", "Rapidez mínima (m/s)", 8, 1.0f, 0.0f, 1000.0f,
                                                    "Los roces mas lentos no cuentan (las que ruedan)")}));
    emit_collision.has_text = true;
    emit_collision.text_label = "Canal";
    BlockInfo& emit_every = add(BlockKind::EmitEvery, Context::Update, "EmitEvery", "Cada t segundos: evento al canal",
                                "Cada particula viva apunta un evento en el canal cada cierto tiempo (rastros, chispas que gotean)",
                                emit_fields({f1("period", "Cada (s)", 8, 0.25f, 0.01f, 3600.0f)}));
    emit_every.has_text = true;
    emit_every.text_label = "Canal";
    // Spawn: escucha un canal (nace en la GPU, sin pasar por la CPU).
    BlockInfo& on_gpu = add(BlockKind::OnGpuEvent, Context::Spawn, "OnGpuEvent", "Al recibir un evento de la GPU",
                            "Nacen donde otro efecto (bloques 'evento al canal') o un script (Effects.pushEvent) apunto un "
                            "evento en el canal: posicion, velocidad y color del evento. Un solo efecto dibuja todos los "
                            "impactos de una zona",
                            {fInt("type", "Tipo", 0, -1.0f, -1.0f, 65535.0f, "-1 = todos los tipos"),
                             fInt("count", "Por evento", 1, 5.0f, 0.0f, 64.0f, "Particulas que nacen en cada evento"),
                             fInt("max", "Máximo por frame", 2, 1024.0f, 0.0f, 1000000.0f, "Particulas como mucho por frame (0 = sin limite)"),
                             f1("inherit", "Velocidad heredada", 3, 0.0f, -10.0f, 10.0f, "Parte de la velocidad del evento que reciben"),
                             fBool("event_color", "Color del evento", 4, true, "Multiplica su color por el del evento"),
                             fBool("value_size", "Valor = tamaño", 5, false, "Multiplica su tamano por el valor del evento")});
    on_gpu.has_text = true;
    on_gpu.text_label = "Canal";
    // Malla: variante por particula y velocidad de giro
    add(BlockKind::MeshIndex, Context::Initialize, "MeshIndex", "Índice de malla",
        "Que modelo de la salida Malla usa cada particula (0 = el principal, 1..3 las variantes): al azar entre min y max",
        {fInt("min", "Mínimo", 0, 0.0f, 0.0f, 3.0f), fInt("max", "Máximo", 1, 3.0f, 0.0f, 3.0f)});
    add(BlockKind::AngularVelocity, Context::Update, "AngularVelocity", "Velocidad de giro",
        "Cambia el giro de cada particula en su vida (el del bloque Giro): acelera, frena o rueda con la velocidad",
        {fAngle("acceleration", "Aceleración (°/s²)", 0, 0.0f, -36000.0f, 36000.0f),
         f1("drag", "Rozamiento", 1, 0.0f, 0.0f, 100.0f, "Frena el giro (1/s)"),
         fAngle("roll", "Rodar (°/m)", 2, 0.0f, -36000.0f, 36000.0f,
                "Giro por metro recorrido (escombros que ruedan); 0 = no rueda")});
    // Output
    add(BlockKind::ColorOverLife, Context::Output, "ColorOverLife", "Color en la vida", "Multiplica el color a lo largo de la vida", {})
        .has_gradient = true;
    add(BlockKind::SizeOverLife, Context::Output, "SizeOverLife", "Tamaño en la vida", "Multiplica el tamano a lo largo de la vida", {})
        .has_curve = true;
    add(BlockKind::ColorBySpeed, Context::Output, "ColorBySpeed", "Color por velocidad", "Del color lento al rapido",
        {fColor("slow", "Lento", 0, Vec4{1.0f, 1.0f, 1.0f, 1.0f}), fColor("fast", "Rápido", 4, Vec4{1.0f, 0.4f, 0.1f, 1.0f}),
         f1("min", "Rapidez mín.", 8, 0.0f, 0.0f, 10000.0f), f1("max", "Rapidez máx.", 9, 10.0f, 0.0f, 10000.0f)});
    // Output: luz por particula (segunda salida del mismo emisor: se dibujan
    // e iluminan). Color = el de la particula (con Color en la vida y su alfa).
    add(BlockKind::ParticleLight, Context::Output, "ParticleLight", "Luz por partícula",
        "Las particulas tambien iluminan el mundo (luces por clusteres, sin sombra; impactos, chispas, magia)",
        {fBool("lightEnabled", "Encendida", 0, true, "Atala a un parametro Bool para encenderla desde un script"),
         f1("lightRadius", "Alcance (m)", 1, 2.0f, 0.05f, 100.0f),
         f1("lightIntensity", "Intensidad", 2, 2.0f, 0.0f, 1000.0f, "Por el color y el alfa de la particula"),
         fInt("falloff", "Caída", 3, 0.0f, 0.0f, 1.0f, "0 = fisica (inversa del cuadrado), 1 = lineal con exponente"),
         f1("exponent", "Exponente", 4, 2.0f, 0.1f, 16.0f, "Caida lineal: (1 - distancia / alcance) ^ exponente"),
         fInt("maxLights", "Luces como máximo", 5, 4.0f, 1.0f, 64.0f, "De este emisor (el tope global va por calidad)"),
         fInt("priority", "Prioridad", 6, 0.0f, 0.0f, 1.0f, "0 = intensidad x alcance, 1 = cercania a la camara"),
         fBool("affectsTranslucent", "Ilumina lo transparente", 7, false,
               "Tambien el vidrio, lo transparente y las particulas iluminadas (mas caro)")});
    return c;
}

Vec4 lerp4(const Vec4& a, const Vec4& b, float t) {
    return Vec4{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
}

gfx::VfxBlock gpuBlock(gfx::VfxBlockType type) {
    gfx::VfxBlock b;
    b.type = static_cast<std::uint32_t>(type);
    return b;
}

// Bloques que necesitan la altura del suelo bajo el emisor (CollidePlane
// "Sobre el terreno", CollideDepth "Suelo sin la vista"). El shader no mira
// este bit; lo usa applyGround.
constexpr std::uint32_t kGroundFlag = 0x100u;

// Rellena la altura del terreno bajo el emisor en los bloques con
// kGroundFlag: una consulta por efecto y frame, en la CPU. El plano queda en
// el mundo (independiente de la vista); sin terreno debajo no chocan.
void applyGround(gfx::VfxInstanceDesc& desc, const std::function<bool(float, float, float&)>& query) {
    bool asked = false;
    bool found = false;
    float height = 0.0f;
    for (gfx::VfxBlock& b : desc.update) {
        if ((b.flags & kGroundFlag) == 0u) continue;
        if (!asked) {
            asked = true;
            // Columna 3 de la matriz: la posicion del emisor.
            found = query && query(desc.transform.m[3][0], desc.transform.m[3][2], height);
        }
        if (b.type == static_cast<std::uint32_t>(gfx::VfxBlockType::CollideDepth)) {
            b.c = Vec4{height, found ? 1.0f : 0.0f, 0.0f, 0.0f};
        } else if (b.type == static_cast<std::uint32_t>(gfx::VfxBlockType::CollidePlane)) {
            // a.w = distancia sobre el suelo -> distancia del plano en el mundo.
            b.a.w = found ? height + b.a.w : -1.0e30f;
        }
    }
}

bool writeText(const std::filesystem::path& path, const std::string& text, std::string* error) {
    const std::filesystem::path temporary = path.string() + ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary);
        if (!file) {
            if (error) *error = "no se pudo escribir " + path.string();
            return false;
        }
        file << text;
        if (!file) {
            if (error) *error = "error escribiendo " + path.string();
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        if (error) *error = ec.message();
        return false;
    }
    return true;
}

const char* kOrientKeys[] = {"FaceCamera", "Stretched", "Horizontal", "Vertical"};
const char* kOutputKeys[] = {"Quads", "Ribbon", "Mesh"};
const char* kFacingKeys[] = {"Camera", "Emitter", "CustomFacing", "CustomSide"};
const char* kRibbonUvKeys[] = {"Stretch", "Tile", "Age"};  // Tile = "Distance" (tambien se lee asi)
const char* kMeshOrientKeys[] = {"Random", "Velocity", "Fixed", "Camera"};
const char* kBlendKeys[] = {"Additive", "Alpha", "Premultiplied", "Distortion", "Opaque"};
const char* kParamKeys[] = {"Float", "Vector3", "Color", "Bool"};
const char* kContextKeys[] = {"spawn", "initialize", "update", "output_blocks"};

template <std::size_t N>
int indexOf(const char* const (&keys)[N], const std::string& key, int fallback) {
    for (std::size_t i = 0; i < N; ++i) {
        if (key == keys[i]) return static_cast<int>(i);
    }
    return fallback;
}

}  // namespace

// --- Catalogo ----------------------------------------------------------------------

const std::vector<BlockInfo>& blockInfos() {
    static const std::vector<BlockInfo> catalog = buildCatalog();
    return catalog;
}

const BlockInfo* blockInfo(BlockKind kind) {
    for (const BlockInfo& info : blockInfos()) {
        if (info.kind == kind) return &info;
    }
    return nullptr;
}

const BlockInfo* blockInfo(const std::string& key) {
    for (const BlockInfo& info : blockInfos()) {
        if (key == info.key) return &info;
    }
    return nullptr;
}

Block makeBlock(BlockKind kind) {
    Block b;
    b.kind = kind;
    if (const BlockInfo* info = blockInfo(kind)) {
        for (const FieldInfo& f : info->fields) {
            for (int i = 0; i < f.width() && f.slot + i < kBlockValues; ++i) b.values[f.slot + i] = f.defaults[i];
        }
        if (info->has_gradient) {
            b.gradient = {GradientKey{0.0f, Vec4{1.0f, 1.0f, 1.0f, 1.0f}}, GradientKey{1.0f, Vec4{1.0f, 1.0f, 1.0f, 0.0f}}};
        }
        if (info->has_curve) b.curve = {CurveKey{0.0f, 1.0f}, CurveKey{1.0f, 0.0f}};
        if (info->has_text) b.text = isChannelBlock(kind) ? "Impactos" : "Event";
    }
    return b;
}

// --- Asset ---------------------------------------------------------------------------

std::vector<Block>& VfxGraph::blocks(Context context) {
    switch (context) {
        case Context::Spawn: return spawn;
        case Context::Initialize: return initialize;
        case Context::Update: return update;
        default: return output_blocks;
    }
}

const std::vector<Block>& VfxGraph::blocks(Context context) const {
    return const_cast<VfxGraph*>(this)->blocks(context);
}

const ExposedParam* VfxGraph::findParam(const std::string& name) const {
    for (const ExposedParam& p : params) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

namespace {

json blockToJson(const Block& b) {
    json j;
    const BlockInfo* info = blockInfo(b.kind);
    j["type"] = info ? info->key : "";
    j["enabled"] = b.enabled;
    if (info) {
        json values = json::object();
        json binds = json::object();
        for (const FieldInfo& f : info->fields) {
            if (f.width() == 1) {
                values[f.key] = b.values[f.slot];
            } else {
                json arr = json::array();
                for (int i = 0; i < f.width(); ++i) arr.push_back(b.values[f.slot + i]);
                values[f.key] = arr;
            }
            if (!b.bindings[f.slot].empty()) binds[f.key] = b.bindings[f.slot];
        }
        j["values"] = values;
        if (!binds.empty()) j["bind"] = binds;
        if (info->has_text) j["event"] = b.text;
        if (info->has_gradient) {
            json g = json::array();
            for (const GradientKey& k : b.gradient) g.push_back({k.time, k.color.x, k.color.y, k.color.z, k.color.w});
            j["gradient"] = g;
        }
        if (info->has_curve) {
            json c = json::array();
            for (const CurveKey& k : b.curve) c.push_back({k.time, k.value});
            j["curve"] = c;
        }
        if (info->has_mesh && b.mesh.valid()) j["mesh"] = b.mesh.uuid.toString();
        if (info->has_effect && b.effect.valid()) j["effect"] = b.effect.uuid.toString();
    }
    return j;
}

bool blockFromJson(const json& j, Block& out) {
    const BlockInfo* info = blockInfo(j.value("type", std::string{}));
    if (info == nullptr) return false;
    out = makeBlock(info->kind);
    out.enabled = j.value("enabled", true);
    if (j.contains("values") && j["values"].is_object()) {
        const json& values = j["values"];
        for (const FieldInfo& f : info->fields) {
            if (!values.contains(f.key)) continue;
            const json& v = values[f.key];
            if (v.is_number()) {
                out.values[f.slot] = v.get<float>();
            } else if (v.is_array()) {
                for (int i = 0; i < f.width() && i < static_cast<int>(v.size()); ++i) {
                    if (v[i].is_number()) out.values[f.slot + i] = v[i].get<float>();
                }
            } else if (v.is_boolean()) {
                out.values[f.slot] = v.get<bool>() ? 1.0f : 0.0f;
            }
        }
    }
    if (j.contains("bind") && j["bind"].is_object()) {
        for (const FieldInfo& f : info->fields) {
            if (j["bind"].contains(f.key) && j["bind"][f.key].is_string()) out.bindings[f.slot] = j["bind"][f.key].get<std::string>();
        }
    }
    if (info->has_text) out.text = j.value("event", out.text);
    if (info->has_gradient && j.contains("gradient") && j["gradient"].is_array()) {
        out.gradient.clear();
        for (const json& k : j["gradient"]) {
            if (!k.is_array() || k.size() < 5) continue;
            out.gradient.push_back(GradientKey{k[0].get<float>(), Vec4{k[1].get<float>(), k[2].get<float>(), k[3].get<float>(), k[4].get<float>()}});
        }
    }
    if (info->has_curve && j.contains("curve") && j["curve"].is_array()) {
        out.curve.clear();
        for (const json& k : j["curve"]) {
            if (!k.is_array() || k.size() < 2) continue;
            out.curve.push_back(CurveKey{k[0].get<float>(), k[1].get<float>()});
        }
    }
    if (info->has_mesh && j.contains("mesh") && j["mesh"].is_string()) {
        out.mesh.uuid = Uuid::parse(j["mesh"].get<std::string>());
    }
    if (info->has_effect && j.contains("effect") && j["effect"].is_string()) {
        out.effect.uuid = Uuid::parse(j["effect"].get<std::string>());
    }
    return true;
}

}  // namespace

std::string vfxGraphToText(const VfxGraph& graph) {
    json root;
    root["uuid"] = graph.uuid.valid() ? graph.uuid.toString() : Uuid::generate().toString();
    root["version"] = 1;
    root["capacity"] = graph.capacity;
    root["world_space"] = graph.world_space;
    root["duration"] = graph.duration;
    root["loop"] = graph.loop;
    root["start_delay"] = graph.start_delay;
    root["prewarm"] = graph.prewarm;
    // Escalabilidad: solo lo que no es lo de serie (un .crvfx sin tipo se
    // guarda igual que antes).
    if (graph.effect_type != EffectType::None) root["effect_type"] = effectTypeKey(graph.effect_type);
    if (graph.sort_order != 0) root["sort_order"] = graph.sort_order;
    if (graph.fixed_bounds) {
        root["bounds"] = {{"center", {graph.bounds_center.x, graph.bounds_center.y, graph.bounds_center.z}},
                          {"size", {graph.bounds_size.x, graph.bounds_size.y, graph.bounds_size.z}}};
    }
    if (graph.spawn_spread_frames > 1) root["spawn_spread_frames"] = graph.spawn_spread_frames;
    json params = json::array();
    for (const ExposedParam& p : graph.params) {
        params.push_back({{"name", p.name},
                          {"type", kParamKeys[std::clamp(static_cast<int>(p.type), 0, 3)]},
                          {"value", {p.value.x, p.value.y, p.value.z, p.value.w}},
                          {"tooltip", p.tooltip}});
    }
    root["params"] = params;
    for (int c = 0; c < 4; ++c) {
        json list = json::array();
        for (const Block& b : graph.blocks(static_cast<Context>(c))) list.push_back(blockToJson(b));
        root[kContextKeys[c]] = list;
    }
    const OutputSettings& o = graph.output;
    root["output"] = {{"orient", kOrientKeys[std::clamp(static_cast<int>(o.orient), 0, 3)]},
                      {"blend", kBlendKeys[std::clamp(static_cast<int>(o.blend), 0, 4)]},
                      {"intensity", o.intensity},
                      {"soft_distance", o.soft_distance},
                      {"stretch", o.stretch},
                      {"alpha_clip", o.alpha_clip},
                      {"lit", o.lit},
                      {"texture", o.texture},
                      {"flip_cols", o.flip_cols},
                      {"flip_rows", o.flip_rows},
                      {"flip_fps", o.flip_fps},
                      {"type", kOutputKeys[std::clamp(static_cast<int>(o.type), 0, 2)]},
                      {"ribbon_facing", kFacingKeys[std::clamp(static_cast<int>(o.ribbon_facing), 0, 3)]},
                      {"ribbon_uv", kRibbonUvKeys[std::clamp(static_cast<int>(o.ribbon_uv), 0, 2)]},
                      {"ribbon_tile", o.ribbon_tile},
                      {"ribbon_gap", o.ribbon_gap},
                      {"mesh_orient", kMeshOrientKeys[std::clamp(static_cast<int>(o.mesh_orient), 0, 3)]},
                      {"mesh_scale", {o.mesh_scale.x, o.mesh_scale.y, o.mesh_scale.z}}};
    if (o.mesh.valid()) root["output"]["mesh"] = o.mesh.uuid.toString();
    // Malla (variantes, LOD, giro, escala al azar, material) y distorsion:
    // claves nuevas, solo si no son las de siempre (los .crvfx de antes cargan igual).
    {
        json& jm = root["output"];
        json variants = json::array();
        for (const assets::AssetRef& v : o.mesh_variants) variants.push_back(v.valid() ? v.uuid.toString() : std::string{});
        if (!variants.empty()) jm["mesh_variants"] = variants;
        if (o.mesh_lod_distance > 0.0f) jm["mesh_lod_distance"] = o.mesh_lod_distance;
        if (o.mesh_sprite_distance > 0.0f) jm["mesh_sprite_distance"] = o.mesh_sprite_distance;
        const Vec4& q = o.mesh_rotation;
        if (q.x != 0.0f || q.y != 0.0f || q.z != 0.0f || q.w != 1.0f) jm["mesh_rotation"] = {q.x, q.y, q.z, q.w};
        const Vec3& jt = o.mesh_scale_jitter;
        if (jt.x != 0.0f || jt.y != 0.0f || jt.z != 0.0f) jm["mesh_scale_jitter"] = {jt.x, jt.y, jt.z};
        if (o.material.valid()) jm["material"] = o.material.uuid.toString();
        if (o.mesh_output_texture) jm["mesh_output_texture"] = true;
        if (o.distortion != 3.0f) jm["distortion"] = o.distortion;
        if (o.distortion_height) jm["distortion_height"] = true;
    }
    // Flipbook fundido y cinta (claves nuevas: los .crvfx de antes, sin
    // ellas, cargan con los valores de siempre).
    json& jo = root["output"];
    jo["flip_blend"] = o.flip_blend;
    jo["ribbon_vector"] = {o.ribbon_vector.x, o.ribbon_vector.y, o.ribbon_vector.z};
    jo["ribbon_tension"] = o.ribbon_tension;
    jo["ribbon_subdivisions"] = o.ribbon_subdivisions;
    jo["ribbon_adaptive"] = o.ribbon_adaptive;
    jo["ribbon_atlas_cell"] = o.ribbon_atlas_cell;
    jo["ribbon_uv_scale"] = {o.ribbon_uv_scale.x, o.ribbon_uv_scale.y};
    jo["ribbon_uv_offset"] = {o.ribbon_uv_offset.x, o.ribbon_uv_offset.y};
    return root.dump(2);
}

bool vfxGraphFromText(const std::string& text, VfxGraph& out, std::string* error) {
    const json root = json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        if (error) *error = "JSON danado";
        return false;
    }
    VfxGraph g;
    g.uuid = Uuid::parse(root.value("uuid", std::string{}));
    g.capacity = std::clamp(root.value("capacity", 2048), 1, static_cast<int>(gfx::VfxPass::kMaxCapacity));
    g.world_space = root.value("world_space", true);
    g.duration = std::max(root.value("duration", 5.0f), 0.01f);
    g.loop = root.value("loop", true);
    g.start_delay = std::max(root.value("start_delay", 0.0f), 0.0f);
    g.prewarm = std::max(root.value("prewarm", 0.0f), 0.0f);
    // Escalabilidad (los de antes no la llevan: sin tipo, sin caja, como antes).
    if (root.contains("effect_type") && root["effect_type"].is_string()) {
        g.effect_type = effectTypeFromKey(root["effect_type"].get<std::string>());
    }
    if (root.contains("sort_order") && root["sort_order"].is_number()) {
        g.sort_order = std::clamp(root["sort_order"].get<int>(), -1000, 1000);
    }
    if (root.contains("bounds") && root["bounds"].is_object()) {
        const json& b = root["bounds"];
        const auto vec = [&](const char* key, Vec3 fallback) {
            if (!b.contains(key) || !b[key].is_array() || b[key].size() < 3) return fallback;
            for (int i = 0; i < 3; ++i) {
                if (!b[key][i].is_number()) return fallback;
            }
            return Vec3{b[key][0].get<float>(), b[key][1].get<float>(), b[key][2].get<float>()};
        };
        g.fixed_bounds = true;
        g.bounds_center = vec("center", Vec3{});
        const Vec3 size = vec("size", g.bounds_size);
        g.bounds_size = Vec3{std::max(size.x, 0.0f), std::max(size.y, 0.0f), std::max(size.z, 0.0f)};
    }
    if (root.contains("spawn_spread_frames") && root["spawn_spread_frames"].is_number()) {
        g.spawn_spread_frames = std::clamp(root["spawn_spread_frames"].get<int>(), 1, 60);
    }
    if (root.contains("params") && root["params"].is_array()) {
        for (const json& p : root["params"]) {
            ExposedParam e;
            e.name = p.value("name", std::string{});
            if (e.name.empty()) continue;
            e.type = static_cast<ParamType>(indexOf(kParamKeys, p.value("type", std::string{"Float"}), 0));
            if (p.contains("value") && p["value"].is_array() && p["value"].size() >= 4) {
                e.value = Vec4{p["value"][0].get<float>(), p["value"][1].get<float>(), p["value"][2].get<float>(),
                               p["value"][3].get<float>()};
            }
            e.tooltip = p.value("tooltip", std::string{});
            g.params.push_back(std::move(e));
        }
    }
    for (int c = 0; c < 4; ++c) {
        if (!root.contains(kContextKeys[c]) || !root[kContextKeys[c]].is_array()) continue;
        for (const json& jb : root[kContextKeys[c]]) {
            Block b;
            if (!blockFromJson(jb, b)) continue;
            const BlockInfo* info = blockInfo(b.kind);
            if (info == nullptr || static_cast<int>(info->context) != c) continue;  // bloque en otro contexto
            g.blocks(static_cast<Context>(c)).push_back(std::move(b));
        }
    }
    if (root.contains("output") && root["output"].is_object()) {
        const json& o = root["output"];
        g.output.orient = static_cast<gfx::VfxOrient>(indexOf(kOrientKeys, o.value("orient", std::string{"FaceCamera"}), 0));
        g.output.blend = static_cast<gfx::VfxBlend>(indexOf(kBlendKeys, o.value("blend", std::string{"Additive"}), 0));
        g.output.intensity = o.value("intensity", 1.0f);
        g.output.soft_distance = o.value("soft_distance", 0.25f);
        g.output.stretch = o.value("stretch", 0.08f);
        g.output.alpha_clip = o.value("alpha_clip", 0.0f);
        g.output.lit = o.value("lit", false);
        g.output.texture = o.value("texture", std::string{});
        g.output.flip_cols = std::max(o.value("flip_cols", 1), 1);
        g.output.flip_rows = std::max(o.value("flip_rows", 1), 1);
        g.output.flip_fps = std::max(o.value("flip_fps", 0.0f), 0.0f);
        g.output.type = static_cast<gfx::VfxOutput>(indexOf(kOutputKeys, o.value("type", std::string{"Quads"}), 0));
        g.output.ribbon_facing =
            static_cast<gfx::VfxRibbonFacing>(indexOf(kFacingKeys, o.value("ribbon_facing", std::string{"Camera"}), 0));
        const std::string ribbon_uv = o.value("ribbon_uv", std::string{"Stretch"});
        g.output.ribbon_uv = ribbon_uv == "Distance" ? gfx::VfxRibbonUv::Tile
                                                     : static_cast<gfx::VfxRibbonUv>(indexOf(kRibbonUvKeys, ribbon_uv, 0));
        g.output.flip_blend = o.value("flip_blend", false);
        if (o.contains("ribbon_vector") && o["ribbon_vector"].is_array() && o["ribbon_vector"].size() >= 3) {
            g.output.ribbon_vector = Vec3{o["ribbon_vector"][0].get<float>(), o["ribbon_vector"][1].get<float>(),
                                          o["ribbon_vector"][2].get<float>()};
        }
        g.output.ribbon_tension = std::clamp(o.value("ribbon_tension", 0.0f), 0.0f, 1.0f);
        g.output.ribbon_subdivisions = std::clamp(o.value("ribbon_subdivisions", 1), 1, 16);
        g.output.ribbon_adaptive = o.value("ribbon_adaptive", false);
        g.output.ribbon_atlas_cell = std::max(o.value("ribbon_atlas_cell", -1), -1);
        if (o.contains("ribbon_uv_scale") && o["ribbon_uv_scale"].is_array() && o["ribbon_uv_scale"].size() >= 2) {
            g.output.ribbon_uv_scale = core::Vec2{o["ribbon_uv_scale"][0].get<float>(), o["ribbon_uv_scale"][1].get<float>()};
        }
        if (o.contains("ribbon_uv_offset") && o["ribbon_uv_offset"].is_array() && o["ribbon_uv_offset"].size() >= 2) {
            g.output.ribbon_uv_offset = core::Vec2{o["ribbon_uv_offset"][0].get<float>(), o["ribbon_uv_offset"][1].get<float>()};
        }
        g.output.ribbon_tile = std::max(o.value("ribbon_tile", 1.0f), 0.001f);
        g.output.ribbon_gap = std::max(o.value("ribbon_gap", 0.0f), 0.0f);
        g.output.mesh_orient =
            static_cast<gfx::VfxMeshOrient>(indexOf(kMeshOrientKeys, o.value("mesh_orient", std::string{"Random"}), 0));
        if (o.contains("mesh_scale") && o["mesh_scale"].is_array() && o["mesh_scale"].size() >= 3) {
            g.output.mesh_scale = Vec3{o["mesh_scale"][0].get<float>(), o["mesh_scale"][1].get<float>(),
                                       o["mesh_scale"][2].get<float>()};
        }
        if (o.contains("mesh") && o["mesh"].is_string()) g.output.mesh.uuid = Uuid::parse(o["mesh"].get<std::string>());
        // Malla y distorsion (claves nuevas; sin ellas, lo de siempre).
        const auto number = [](const json& v, float fallback) { return v.is_number() ? v.get<float>() : fallback; };
        if (o.contains("mesh_variants") && o["mesh_variants"].is_array()) {
            for (const json& v : o["mesh_variants"]) {
                if (g.output.mesh_variants.size() + 1 >= gfx::kVfxMeshVariants) break;
                assets::AssetRef ref{{}, assets::AssetType::Model};
                if (v.is_string()) ref.uuid = Uuid::parse(v.get<std::string>());
                g.output.mesh_variants.push_back(ref);
            }
        }
        g.output.mesh_lod_distance = std::max(o.value("mesh_lod_distance", 0.0f), 0.0f);
        g.output.mesh_sprite_distance = std::max(o.value("mesh_sprite_distance", 0.0f), 0.0f);
        if (o.contains("mesh_rotation") && o["mesh_rotation"].is_array() && o["mesh_rotation"].size() >= 4) {
            const json& q = o["mesh_rotation"];
            g.output.mesh_rotation = Vec4{number(q[0], 0.0f), number(q[1], 0.0f), number(q[2], 0.0f), number(q[3], 1.0f)};
        }
        if (o.contains("mesh_scale_jitter") && o["mesh_scale_jitter"].is_array() && o["mesh_scale_jitter"].size() >= 3) {
            const json& s = o["mesh_scale_jitter"];
            g.output.mesh_scale_jitter = Vec3{std::clamp(number(s[0], 0.0f), 0.0f, 1.0f), std::clamp(number(s[1], 0.0f), 0.0f, 1.0f),
                                              std::clamp(number(s[2], 0.0f), 0.0f, 1.0f)};
        }
        if (o.contains("material") && o["material"].is_string()) {
            g.output.material.uuid = Uuid::parse(o["material"].get<std::string>());
        }
        g.output.distortion = std::clamp(o.value("distortion", 3.0f), 0.0f, gfx::kVfxMaxDistortionPixels);
        g.output.distortion_height = o.value("distortion_height", false);
        g.output.mesh_output_texture = o.value("mesh_output_texture", false);
    }
    out = std::move(g);
    return true;
}

bool loadVfxGraph(const std::filesystem::path& path, VfxGraph& out, std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (error) *error = "no se pudo abrir " + path.string();
        return false;
    }
    std::stringstream ss;
    ss << file.rdbuf();
    std::string inner;
    if (!vfxGraphFromText(ss.str(), out, &inner)) {
        if (error) *error = inner + " en " + path.string();
        return false;
    }
    return true;
}

bool saveVfxGraph(const VfxGraph& graph, const std::filesystem::path& path, std::string* error) {
    return writeText(path, vfxGraphToText(graph), error);
}

// --- Plantillas ----------------------------------------------------------------------

namespace {

const char* kPresetNames[] = {"Chispas", "Fuego", "Humo", "Chispas que rebotan", "Explosión",
                              "Magia", "Luciérnagas", "Nieve", "Lluvia", "Estela", "Estela (cinta)",
                              "Estela de proyectil"};

Block blk(BlockKind kind, std::initializer_list<std::pair<int, float>> values = {}) {
    Block b = makeBlock(kind);
    for (const auto& [slot, value] : values) b.values[slot] = value;
    return b;
}
Block blkVec(BlockKind kind, int slot, Vec3 v, std::initializer_list<std::pair<int, float>> values = {}) {
    Block b = blk(kind, values);
    b.values[slot] = v.x;
    b.values[slot + 1] = v.y;
    b.values[slot + 2] = v.z;
    return b;
}
void setColor(Block& b, int slot, Vec4 c) {
    b.values[slot] = c.x;
    b.values[slot + 1] = c.y;
    b.values[slot + 2] = c.z;
    b.values[slot + 3] = c.w;
}
Block colorBlock(Vec4 a, Vec4 b) {
    Block blockc = makeBlock(BlockKind::Color);
    setColor(blockc, 0, a);
    setColor(blockc, 4, b);
    return blockc;
}
Block gradientBlock(std::vector<GradientKey> keys) {
    Block b = makeBlock(BlockKind::ColorOverLife);
    b.gradient = std::move(keys);
    return b;
}
Block curveBlock(std::vector<CurveKey> keys) {
    Block b = makeBlock(BlockKind::SizeOverLife);
    b.curve = std::move(keys);
    return b;
}

}  // namespace

int vfxPresetCount() { return static_cast<int>(std::size(kPresetNames)); }

const char* vfxPresetName(int preset) {
    return kPresetNames[std::clamp(preset, 0, vfxPresetCount() - 1)];
}

VfxGraph vfxPreset(int preset) {
    VfxGraph g;
    g.uuid = Uuid::generate();
    switch (std::clamp(preset, 0, vfxPresetCount() - 1)) {
        default:
        case 0: {  // Chispas
            g.params.push_back(ExposedParam{"Rate", ParamType::Float, Vec4{60.0f, 0, 0, 0}, "Chispas por segundo"});
            g.params.push_back(ExposedParam{"Color", ParamType::Color, Vec4{1.0f, 0.7f, 0.25f, 1.0f}, "Color de las chispas"});
            Block rate = blk(BlockKind::ConstantRate, {{0, 60.0f}});
            rate.bindings[0] = "Rate";
            g.spawn.push_back(rate);
            g.initialize.push_back(blk(BlockKind::PositionSphere, {{3, 0.05f}}));
            g.initialize.push_back(blkVec(BlockKind::VelocityRandom, 0, Vec3{-2.0f, 2.0f, -2.0f}, {{3, 2.0f}, {4, 5.0f}, {5, 2.0f}}));
            g.initialize.push_back(blk(BlockKind::Lifetime, {{0, 0.6f}, {1, 1.4f}}));
            g.initialize.push_back(blk(BlockKind::Size, {{0, 0.02f}, {1, 0.04f}}));
            Block color = colorBlock(Vec4{1.0f, 0.7f, 0.25f, 1.0f}, Vec4{1.0f, 0.45f, 0.1f, 1.0f});
            color.bindings[0] = "Color";
            g.initialize.push_back(color);
            g.update.push_back(makeBlock(BlockKind::Gravity));
            g.update.push_back(blk(BlockKind::Drag, {{0, 0.4f}}));
            g.output_blocks.push_back(gradientBlock({{0.0f, Vec4{1, 1, 1, 1}}, {0.7f, Vec4{1, 0.8f, 0.6f, 1}}, {1.0f, Vec4{1, 0.3f, 0.1f, 0}}}));
            g.output.orient = gfx::VfxOrient::Stretched;
            g.output.intensity = 4.0f;
            break;
        }
        case 1: {  // Fuego
            g.capacity = 1024;
            g.params.push_back(ExposedParam{"Intensity", ParamType::Float, Vec4{1.0f, 0, 0, 0}, "Tamano de las llamas"});
            g.spawn.push_back(blk(BlockKind::ConstantRate, {{0, 120.0f}}));
            g.initialize.push_back(blk(BlockKind::PositionCone, {{3, 0.25f}, {4, 8.0f}}));
            g.initialize.push_back(blk(BlockKind::VelocityFromShape, {{0, 0.6f}, {1, 1.4f}, {2, 5.0f}}));
            g.initialize.push_back(blk(BlockKind::Lifetime, {{0, 0.6f}, {1, 1.1f}}));
            Block size = blk(BlockKind::Size, {{0, 0.35f}, {1, 0.6f}});
            g.initialize.push_back(size);
            g.initialize.push_back(colorBlock(Vec4{1.0f, 0.55f, 0.15f, 1.0f}, Vec4{1.0f, 0.35f, 0.05f, 1.0f}));
            g.initialize.push_back(blk(BlockKind::Rotation, {{0, 0.0f}, {1, 360.0f}, {2, -40.0f}, {3, 40.0f}}));
            g.update.push_back(blkVec(BlockKind::Gravity, 0, Vec3{0.0f, 1.6f, 0.0f}));
            g.update.push_back(blk(BlockKind::Turbulence, {{0, 1.2f}, {1, 1.5f}, {2, 2.0f}, {3, 1.0f}}));
            g.update.push_back(blk(BlockKind::Drag, {{0, 1.0f}}));
            g.output_blocks.push_back(gradientBlock({{0.0f, Vec4{1, 0.9f, 0.6f, 0}},
                                                     {0.1f, Vec4{1, 0.85f, 0.5f, 1}},
                                                     {0.5f, Vec4{1, 0.45f, 0.1f, 0.8f}},
                                                     {1.0f, Vec4{0.4f, 0.05f, 0.0f, 0}}}));
            g.output_blocks.push_back(curveBlock({{0.0f, 0.6f}, {0.3f, 1.0f}, {1.0f, 0.2f}}));
            g.output.intensity = 2.5f;
            g.output.soft_distance = 0.3f;
            break;
        }
        case 2: {  // Humo
            g.capacity = 512;
            g.spawn.push_back(blk(BlockKind::ConstantRate, {{0, 20.0f}}));
            g.initialize.push_back(blk(BlockKind::PositionCircle, {{3, 0.3f}}));
            g.initialize.push_back(blkVec(BlockKind::VelocityRandom, 0, Vec3{-0.2f, 0.7f, -0.2f}, {{3, 0.2f}, {4, 1.3f}, {5, 0.2f}}));
            g.initialize.push_back(blk(BlockKind::Lifetime, {{0, 3.0f}, {1, 5.0f}}));
            g.initialize.push_back(blk(BlockKind::Size, {{0, 0.6f}, {1, 1.0f}}));
            g.initialize.push_back(colorBlock(Vec4{0.45f, 0.45f, 0.45f, 0.5f}, Vec4{0.6f, 0.6f, 0.6f, 0.4f}));
            g.initialize.push_back(blk(BlockKind::Rotation, {{0, 0.0f}, {1, 360.0f}, {2, -20.0f}, {3, 20.0f}}));
            g.update.push_back(blk(BlockKind::Turbulence, {{0, 0.5f}, {1, 0.6f}, {2, 2.0f}, {3, 0.3f}}));
            g.update.push_back(blkVec(BlockKind::Wind, 0, Vec3{0.6f, 0.0f, 0.0f}, {{3, 0.3f}}));
            g.update.push_back(blk(BlockKind::Drag, {{0, 0.3f}}));
            g.output_blocks.push_back(gradientBlock({{0.0f, Vec4{1, 1, 1, 0}}, {0.2f, Vec4{1, 1, 1, 1}}, {1.0f, Vec4{1, 1, 1, 0}}}));
            g.output_blocks.push_back(curveBlock({{0.0f, 0.5f}, {1.0f, 2.5f}}));
            g.output.blend = gfx::VfxBlend::Alpha;
            g.output.lit = true;
            g.output.soft_distance = 0.5f;
            break;
        }
        case 3: {  // Chispas que rebotan
            g.spawn.push_back(blk(BlockKind::ConstantRate, {{0, 80.0f}}));
            g.initialize.push_back(blk(BlockKind::PositionCone, {{3, 0.05f}, {4, 30.0f}}));
            g.initialize.push_back(blk(BlockKind::VelocityFromShape, {{0, 3.0f}, {1, 6.0f}, {2, 10.0f}}));
            g.initialize.push_back(blk(BlockKind::Lifetime, {{0, 1.5f}, {1, 2.5f}}));
            g.initialize.push_back(blk(BlockKind::Size, {{0, 0.02f}, {1, 0.035f}}));
            g.initialize.push_back(colorBlock(Vec4{1.0f, 0.8f, 0.4f, 1.0f}, Vec4{1.0f, 0.5f, 0.15f, 1.0f}));
            g.update.push_back(makeBlock(BlockKind::Gravity));
            g.update.push_back(blk(BlockKind::CollideDepth, {{0, 0.5f}, {1, 0.15f}, {2, 0.1f}}));
            g.output_blocks.push_back(gradientBlock({{0.0f, Vec4{1, 1, 1, 1}}, {1.0f, Vec4{1, 0.3f, 0.1f, 0}}}));
            g.output.orient = gfx::VfxOrient::Stretched;
            g.output.intensity = 4.0f;
            break;
        }
        case 4: {  // Explosion
            g.loop = false;
            g.duration = 3.0f;
            g.capacity = 1024;
            g.spawn.push_back(blk(BlockKind::Burst, {{0, 400.0f}, {1, 0.0f}}));
            Block on_event = blk(BlockKind::OnEvent, {{0, 400.0f}});
            on_event.text = "Explode";
            g.spawn.push_back(on_event);
            g.initialize.push_back(blk(BlockKind::PositionSphere, {{3, 0.2f}}));
            g.initialize.push_back(blk(BlockKind::VelocityFromShape, {{0, 4.0f}, {1, 12.0f}}));
            g.initialize.push_back(blk(BlockKind::Lifetime, {{0, 0.5f}, {1, 1.6f}}));
            g.initialize.push_back(blk(BlockKind::Size, {{0, 0.05f}, {1, 0.25f}}));
            g.initialize.push_back(colorBlock(Vec4{1.0f, 0.75f, 0.3f, 1.0f}, Vec4{1.0f, 0.35f, 0.08f, 1.0f}));
            g.update.push_back(blk(BlockKind::Drag, {{0, 2.0f}}));
            g.update.push_back(blkVec(BlockKind::Gravity, 0, Vec3{0.0f, -4.0f, 0.0f}));
            g.update.push_back(blk(BlockKind::CollideDepth, {{0, 0.3f}, {1, 0.3f}}));
            g.output_blocks.push_back(gradientBlock({{0.0f, Vec4{1, 1, 0.9f, 1}}, {0.4f, Vec4{1, 0.5f, 0.15f, 1}}, {1.0f, Vec4{0.2f, 0.05f, 0, 0}}}));
            g.output_blocks.push_back(curveBlock({{0.0f, 1.0f}, {1.0f, 0.3f}}));
            g.output.orient = gfx::VfxOrient::Stretched;
            g.output.stretch = 0.05f;
            g.output.intensity = 5.0f;
            break;
        }
        case 5: {  // Magia
            g.params.push_back(ExposedParam{"Color", ParamType::Color, Vec4{0.6f, 0.3f, 1.0f, 1.0f}, "Color principal"});
            g.spawn.push_back(blk(BlockKind::ConstantRate, {{0, 200.0f}}));
            g.initialize.push_back(blk(BlockKind::PositionSphere, {{3, 1.2f}, {4, 0.0f}}));
            g.initialize.push_back(blk(BlockKind::Lifetime, {{0, 1.5f}, {1, 3.0f}}));
            g.initialize.push_back(blk(BlockKind::Size, {{0, 0.03f}, {1, 0.07f}}));
            Block color = colorBlock(Vec4{0.6f, 0.3f, 1.0f, 1.0f}, Vec4{0.2f, 0.8f, 1.0f, 1.0f});
            color.bindings[0] = "Color";
            g.initialize.push_back(color);
            g.update.push_back(blk(BlockKind::Vortex, {{3, 4.0f}, {7, 0.6f}}));
            g.update.push_back(blk(BlockKind::Attractor, {{3, 1.5f}}));
            g.update.push_back(blk(BlockKind::Turbulence, {{0, 0.4f}, {1, 2.0f}}));
            g.output_blocks.push_back(gradientBlock({{0.0f, Vec4{1, 1, 1, 0}}, {0.2f, Vec4{1, 1, 1, 1}}, {1.0f, Vec4{1, 1, 1, 0}}}));
            g.output.intensity = 4.0f;
            break;
        }
        case 6: {  // Luciernagas
            g.capacity = 256;
            g.spawn.push_back(blk(BlockKind::ConstantRate, {{0, 8.0f}}));
            g.initialize.push_back(blkVec(BlockKind::PositionBox, 3, Vec3{8.0f, 2.0f, 8.0f}, {{1, 1.0f}}));
            g.initialize.push_back(blkVec(BlockKind::VelocityRandom, 0, Vec3{-0.2f, -0.1f, -0.2f}, {{3, 0.2f}, {4, 0.1f}, {5, 0.2f}}));
            g.initialize.push_back(blk(BlockKind::Lifetime, {{0, 4.0f}, {1, 7.0f}}));
            g.initialize.push_back(blk(BlockKind::Size, {{0, 0.04f}, {1, 0.06f}}));
            g.initialize.push_back(colorBlock(Vec4{0.8f, 1.0f, 0.3f, 1.0f}, Vec4{1.0f, 0.9f, 0.3f, 1.0f}));
            g.update.push_back(blk(BlockKind::Turbulence, {{0, 0.6f}, {1, 0.5f}, {3, 0.2f}}));
            g.output_blocks.push_back(gradientBlock({{0.0f, Vec4{1, 1, 1, 0}}, {0.3f, Vec4{1, 1, 1, 1}}, {0.5f, Vec4{1, 1, 1, 0.2f}},
                                                     {0.7f, Vec4{1, 1, 1, 1}}, {1.0f, Vec4{1, 1, 1, 0}}}));
            g.output.intensity = 6.0f;
            g.prewarm = 5.0f;
            break;
        }
        case 7: {  // Nieve
            g.capacity = 8192;
            g.spawn.push_back(blk(BlockKind::ConstantRate, {{0, 400.0f}}));
            g.initialize.push_back(blkVec(BlockKind::PositionBox, 3, Vec3{20.0f, 0.1f, 20.0f}, {{1, 8.0f}}));
            g.initialize.push_back(blkVec(BlockKind::VelocityRandom, 0, Vec3{-0.3f, -1.3f, -0.3f}, {{3, 0.3f}, {4, -0.8f}, {5, 0.3f}}));
            g.initialize.push_back(blk(BlockKind::Lifetime, {{0, 8.0f}, {1, 10.0f}}));
            g.initialize.push_back(blk(BlockKind::Size, {{0, 0.025f}, {1, 0.05f}}));
            g.initialize.push_back(colorBlock(Vec4{1.0f, 1.0f, 1.0f, 0.9f}, Vec4{0.9f, 0.95f, 1.0f, 0.8f}));
            g.update.push_back(blk(BlockKind::Turbulence, {{0, 0.3f}, {1, 0.4f}, {3, 0.2f}}));
            g.update.push_back(blk(BlockKind::CollideDepth, {{0, 0.0f}, {1, 1.0f}, {2, 0.3f}}));
            g.output_blocks.push_back(gradientBlock({{0.0f, Vec4{1, 1, 1, 0}}, {0.1f, Vec4{1, 1, 1, 1}}, {0.9f, Vec4{1, 1, 1, 1}}, {1.0f, Vec4{1, 1, 1, 0}}}));
            g.output.blend = gfx::VfxBlend::Alpha;
            g.output.lit = true;
            g.prewarm = 8.0f;
            break;
        }
        case 8: {  // Lluvia
            g.capacity = 8192;
            g.spawn.push_back(blk(BlockKind::ConstantRate, {{0, 2000.0f}}));
            g.initialize.push_back(blkVec(BlockKind::PositionBox, 3, Vec3{20.0f, 0.1f, 20.0f}, {{1, 10.0f}}));
            g.initialize.push_back(blkVec(BlockKind::VelocityRandom, 0, Vec3{0.0f, -12.0f, 0.0f}, {{3, 0.3f}, {4, -10.0f}, {5, 0.3f}}));
            g.initialize.push_back(blk(BlockKind::Lifetime, {{0, 1.0f}, {1, 1.3f}}));
            g.initialize.push_back(blk(BlockKind::Size, {{0, 0.012f}, {1, 0.02f}}));
            g.initialize.push_back(colorBlock(Vec4{0.7f, 0.8f, 1.0f, 0.5f}, Vec4{0.8f, 0.85f, 1.0f, 0.35f}));
            g.update.push_back(blk(BlockKind::CollideDepth, {{5, 1.0f}}));
            g.output.orient = gfx::VfxOrient::Stretched;
            g.output.stretch = 0.03f;
            g.output.blend = gfx::VfxBlend::Alpha;
            g.output.lit = true;
            g.prewarm = 1.0f;
            break;
        }
        case 9: {  // Estela
            g.spawn.push_back(blk(BlockKind::RateOverDistance, {{0, 30.0f}}));
            g.spawn.push_back(blk(BlockKind::ConstantRate, {{0, 10.0f}}));
            g.initialize.push_back(blk(BlockKind::PositionSphere, {{3, 0.05f}}));
            g.initialize.push_back(blk(BlockKind::Lifetime, {{0, 0.5f}, {1, 1.0f}}));
            g.initialize.push_back(blk(BlockKind::Size, {{0, 0.08f}, {1, 0.12f}}));
            g.initialize.push_back(colorBlock(Vec4{0.3f, 0.9f, 1.0f, 1.0f}, Vec4{0.5f, 0.6f, 1.0f, 1.0f}));
            g.update.push_back(blk(BlockKind::Drag, {{0, 2.0f}}));
            g.output_blocks.push_back(curveBlock({{0.0f, 1.0f}, {1.0f, 0.0f}}));
            g.output_blocks.push_back(gradientBlock({{0.0f, Vec4{1, 1, 1, 1}}, {1.0f, Vec4{1, 1, 1, 0}}}));
            g.output.intensity = 3.0f;
            break;
        }
        case 10: {  // Estela (cinta): espadas, proyectiles, habilidades
            g.capacity = 256;
            g.spawn.push_back(blk(BlockKind::RateOverDistance, {{0, 12.0f}}));
            g.spawn.push_back(blk(BlockKind::ConstantRate, {{0, 30.0f}}));
            g.initialize.push_back(blk(BlockKind::Lifetime, {{0, 0.35f}, {1, 0.35f}}));
            g.initialize.push_back(blk(BlockKind::Size, {{0, 0.25f}, {1, 0.25f}}));
            g.initialize.push_back(colorBlock(Vec4{0.4f, 0.8f, 1.0f, 1.0f}, Vec4{0.4f, 0.8f, 1.0f, 1.0f}));
            g.output_blocks.push_back(curveBlock({{0.0f, 1.0f}, {1.0f, 0.0f}}));
            g.output_blocks.push_back(gradientBlock({{0.0f, Vec4{1, 1, 1, 1}}, {1.0f, Vec4{1, 1, 1, 0}}}));
            g.output.type = gfx::VfxOutput::Ribbon;
            g.output.intensity = 3.0f;
            g.output.soft_distance = 0.0f;
            g.output.ribbon_gap = 0.2f;
            break;
        }
        case 11: {  // Estela de proyectil: flechas, bolas de fuego (un punto cada 12.5 cm)
            g.capacity = 512;
            g.params.push_back(ExposedParam{"Color", ParamType::Color, Vec4{1.0f, 0.6f, 0.2f, 1.0f}, "Color de la estela"});
            g.spawn.push_back(blk(BlockKind::RateOverDistance, {{0, 8.0f}}));
            g.initialize.push_back(blk(BlockKind::Lifetime, {{0, 0.4f}, {1, 0.4f}}));
            g.initialize.push_back(blk(BlockKind::RibbonAttributes, {{2, 0.15f}}));
            Block color = colorBlock(Vec4{1.0f, 0.6f, 0.2f, 1.0f}, Vec4{1.0f, 0.6f, 0.2f, 1.0f});
            color.bindings[0] = "Color";
            g.initialize.push_back(color);
            // La cola se estrecha y se apaga sola (tambien cuando el proyectil muere).
            g.output_blocks.push_back(curveBlock({{0.0f, 1.0f}, {1.0f, 0.15f}}));
            g.output_blocks.push_back(gradientBlock({{0.0f, Vec4{1, 1, 1, 1}}, {0.6f, Vec4{1, 0.7f, 0.5f, 0.5f}}, {1.0f, Vec4{1, 0.5f, 0.3f, 0}}}));
            g.output.type = gfx::VfxOutput::Ribbon;
            g.output.ribbon_uv = gfx::VfxRibbonUv::Tile;  // por distancia: la textura no resbala
            g.output.ribbon_tile = 1.5f;
            g.output.ribbon_subdivisions = 4;
            g.output.ribbon_adaptive = true;  // solo se subdivide donde gira
            g.output.ribbon_gap = 0.5f;
            g.output.intensity = 4.0f;
            g.output.soft_distance = 0.0f;
            break;
        }
    }
    return g;
}

// --- Compilacion -------------------------------------------------------------------

float blockValue(const Block& block, int slot, const ParamValues& params) {
    if (slot < 0 || slot >= kBlockValues) return 0.0f;
    if (!block.bindings[slot].empty()) {
        const auto it = params.find(block.bindings[slot]);
        if (it != params.end()) return it->second.x;
    }
    return block.values[slot];
}

Vec3 blockVec3(const Block& block, int slot, const ParamValues& params) {
    if (slot < 0 || slot + 2 >= kBlockValues) return Vec3{};
    if (!block.bindings[slot].empty()) {
        const auto it = params.find(block.bindings[slot]);
        if (it != params.end()) return Vec3{it->second.x, it->second.y, it->second.z};
    }
    return Vec3{block.values[slot], block.values[slot + 1], block.values[slot + 2]};
}

Vec4 blockColor(const Block& block, int slot, const ParamValues& params) {
    if (slot < 0 || slot + 3 >= kBlockValues) return Vec4{};
    if (!block.bindings[slot].empty()) {
        const auto it = params.find(block.bindings[slot]);
        if (it != params.end()) return it->second;
    }
    return Vec4{block.values[slot], block.values[slot + 1], block.values[slot + 2], block.values[slot + 3]};
}

Vec4 sampleGradient(const std::vector<GradientKey>& keys, float t) {
    if (keys.empty()) return Vec4{1.0f, 1.0f, 1.0f, 1.0f};
    if (t <= keys.front().time) return keys.front().color;
    for (std::size_t i = 1; i < keys.size(); ++i) {
        if (t <= keys[i].time) {
            const float span = keys[i].time - keys[i - 1].time;
            const float u = span > 1e-6f ? (t - keys[i - 1].time) / span : 1.0f;
            return lerp4(keys[i - 1].color, keys[i].color, u);
        }
    }
    return keys.back().color;
}

float sampleCurve(const std::vector<CurveKey>& keys, float t) {
    if (keys.empty()) return 1.0f;
    if (t <= keys.front().time) return keys.front().value;
    for (std::size_t i = 1; i < keys.size(); ++i) {
        if (t <= keys[i].time) {
            const float span = keys[i].time - keys[i - 1].time;
            const float u = span > 1e-6f ? (t - keys[i - 1].time) / span : 1.0f;
            return keys[i - 1].value + (keys[i].value - keys[i - 1].value) * u;
        }
    }
    return keys.back().value;
}

void compileGraph(const VfxGraph& graph, const ParamValues& params, gfx::VfxInstanceDesc& out) {
    using T = gfx::VfxBlockType;
    out.initialize.clear();
    out.update.clear();
    out.ribbon_ids = false;  // lo pone el bloque RibbonAttributes
    const auto v = [&](const Block& b, int slot) { return blockValue(b, slot, params); };
    const auto v3 = [&](const Block& b, int slot) { return blockVec3(b, slot, params); };
    const auto c4 = [&](const Block& b, int slot) { return blockColor(b, slot, params); };

    for (const Block& b : graph.initialize) {
        if (!b.enabled) continue;
        gfx::VfxBlock g;
        switch (b.kind) {
            case BlockKind::PositionSphere:
                g = gpuBlock(T::PositionSphere);
                g.a = Vec4{v3(b, 0), v(b, 3)};
                g.b = Vec4{v(b, 4), v(b, 5), 0.0f, 0.0f};
                break;
            case BlockKind::PositionBox:
                g = gpuBlock(T::PositionBox);
                g.a = Vec4{v3(b, 0), 0.0f};
                g.b = Vec4{v3(b, 3), v(b, 6) > 0.5f ? 1.0f : 0.0f};
                break;
            case BlockKind::PositionCone:
                g = gpuBlock(T::PositionCone);
                g.a = Vec4{v3(b, 0), v(b, 3)};
                g.b = Vec4{v(b, 4), v(b, 5), v(b, 6), 0.0f};
                break;
            case BlockKind::PositionCircle:
                g = gpuBlock(T::PositionCircle);
                g.a = Vec4{v3(b, 0), v(b, 3)};
                g.b = Vec4{v(b, 4), 0.0f, 0.0f, 0.0f};
                break;
            case BlockKind::PositionLine:
                g = gpuBlock(T::PositionLine);
                g.a = Vec4{v3(b, 0), 0.0f};
                g.b = Vec4{v3(b, 3), 0.0f};
                break;
            case BlockKind::PositionMesh:
                g = gpuBlock(T::PositionMesh);  // a.x/a.y los pone el pase
                g.b = Vec4{v(b, 0), 0.0f, 0.0f, 0.0f};
                break;
            case BlockKind::VelocityRandom:
                g = gpuBlock(T::VelocityRandom);
                g.a = Vec4{v3(b, 0), 0.0f};
                g.b = Vec4{v3(b, 3), 0.0f};
                g.flags = v(b, 6) > 0.5f ? 1u : 0u;
                break;
            case BlockKind::VelocityFromShape:
                g = gpuBlock(T::VelocityFromShape);
                g.a = Vec4{v(b, 0), v(b, 1), v(b, 2), 0.0f};
                break;
            case BlockKind::Lifetime:
                g = gpuBlock(T::Lifetime);
                g.a = Vec4{v(b, 0), std::max(v(b, 1), v(b, 0)), 0.0f, 0.0f};
                break;
            case BlockKind::Size:
                g = gpuBlock(T::Size);
                g.a = Vec4{v(b, 0), std::max(v(b, 1), v(b, 0)), 0.0f, 0.0f};
                break;
            case BlockKind::Color:
                g = gpuBlock(T::Color);
                g.a = c4(b, 0);
                g.b = b.bindings[4].empty() && !b.bindings[0].empty() ? c4(b, 0) : c4(b, 4);
                break;
            case BlockKind::Rotation:
                g = gpuBlock(T::Rotation);
                g.a = Vec4{v(b, 0), v(b, 1), v(b, 2), v(b, 3)};
                break;
            case BlockKind::FlipbookFrame:
                g = gpuBlock(T::FlipbookFrame);
                g.a = Vec4{v(b, 0), std::max(v(b, 1), v(b, 0)), std::clamp(v(b, 2), 0.0f, 0.5f), 0.0f};
                break;
            case BlockKind::MeshIndex: {
                const float lo = std::clamp(std::round(v(b, 0)), 0.0f, static_cast<float>(gfx::kVfxMeshVariants - 1));
                const float hi = std::clamp(std::round(v(b, 1)), lo, static_cast<float>(gfx::kVfxMeshVariants - 1));
                g = gpuBlock(T::MeshIndex);
                g.a = Vec4{lo, hi, 0.0f, 0.0f};
                break;
            }
            case BlockKind::RibbonAttributes:
                g = gpuBlock(T::RibbonAttributes);
                g.a = Vec4{std::clamp(std::round(v(b, 0)), 0.0f, 255.0f), std::clamp(std::round(v(b, 1)), 1.0f, 16.0f),
                           std::max(v(b, 2), 0.0f), v(b, 3)};
                {
                    const Vec3 spacing = v3(b, 5);
                    g.b = Vec4{std::max(v(b, 4), v(b, 3)), spacing.x, spacing.y, spacing.z};
                }
                g.flags = v(b, 8) > 0.5f ? 1u : 0u;  // id del evento
                // Varias cintas: orden por (id, edad). Con un solo id basta la edad.
                out.ribbon_ids = out.ribbon_ids || g.a.x > 0.0f || g.a.y > 1.0f || g.flags != 0u;
                break;
            case BlockKind::InheritVelocity:
                g = gpuBlock(T::InheritVelocity);
                g.a = Vec4{v(b, 0), 0.0f, 0.0f, 0.0f};
                break;
            default: continue;
        }
        out.initialize.push_back(g);
    }

    // Canales de datos: su indice en eventChannels (vacio si no hay bloques).
    const std::vector<std::string> channel_names = eventChannels(graph);
    const auto channelSlot = [&](const std::string& name) {
        const auto found = std::find(channel_names.begin(), channel_names.end(), name);
        return static_cast<std::uint32_t>(found - channel_names.begin());
    };
    std::uint32_t trigger_index = 0;
    for (const Block& b : graph.update) {
        if (!b.enabled) continue;
        gfx::VfxBlock g;
        switch (b.kind) {
            case BlockKind::Gravity:
                g = gpuBlock(T::Gravity);
                g.a = Vec4{v3(b, 0), 0.0f};
                break;
            case BlockKind::Wind:
                g = gpuBlock(T::Wind);
                g.a = Vec4{v3(b, 0), v(b, 3)};
                break;
            case BlockKind::Drag:
                g = gpuBlock(T::Drag);
                g.a = Vec4{v(b, 0), v(b, 1), 0.0f, 0.0f};
                break;
            case BlockKind::Turbulence:
                g = gpuBlock(T::Turbulence);
                g.a = Vec4{v(b, 0), v(b, 1), std::clamp(v(b, 2), 1.0f, 4.0f), v(b, 3)};
                break;
            case BlockKind::Attractor:
                g = gpuBlock(T::Attractor);
                g.a = Vec4{v3(b, 0), v(b, 3)};
                g.b = Vec4{v(b, 4), v(b, 5), 0.0f, 0.0f};
                break;
            case BlockKind::Vortex: {
                g = gpuBlock(T::Vortex);
                Vec3 axis = v3(b, 4);
                axis = core::length(axis) > 1e-5f ? core::normalize(axis) : Vec3{0.0f, 1.0f, 0.0f};
                g.a = Vec4{v3(b, 0), v(b, 3)};
                g.b = Vec4{axis, v(b, 7)};
                break;
            }
            case BlockKind::CollideDepth:
                g = gpuBlock(T::CollideDepth);
                g.a = Vec4{v(b, 0), v(b, 1), v(b, 2), v(b, 3)};
                g.b = Vec4{v(b, 4), v(b, 5) > 0.5f ? 1.0f : 0.0f, 0.0f, 0.0f};
                // Suelo sin la vista: c.x la altura y c.y si hay suelo, los
                // pone applyGround con la posicion del emisor de cada frame.
                g.flags = v(b, 6) > 0.5f ? kGroundFlag : 0u;
                break;
            case BlockKind::CollidePlane: {
                g = gpuBlock(T::CollidePlane);
                Vec3 n = v3(b, 0);
                n = core::length(n) > 1e-5f ? core::normalize(n) : Vec3{0.0f, 1.0f, 0.0f};
                // Sobre el terreno: horizontal y en el mundo; applyGround le
                // suma la altura del suelo bajo el emisor a la distancia.
                const bool follow_ground = v(b, 8) > 0.5f;
                if (follow_ground) n = Vec3{0.0f, 1.0f, 0.0f};
                g.a = Vec4{n, v(b, 3)};
                g.b = Vec4{v(b, 4), v(b, 5), v(b, 6), 0.0f};
                g.flags = (v(b, 7) > 0.5f || follow_ground ? 1u : 0u) | (follow_ground ? kGroundFlag : 0u);
                break;
            }
            case BlockKind::CollideSphere:
                g = gpuBlock(T::CollideSphere);
                g.a = Vec4{v3(b, 0), v(b, 3)};
                g.b = Vec4{v(b, 4), v(b, 5), v(b, 6) > 0.5f ? 1.0f : 0.0f, 0.0f};
                break;
            case BlockKind::KillBox:
                g = gpuBlock(T::KillBox);
                g.a = Vec4{v3(b, 0), 0.0f};
                g.b = Vec4{v3(b, 3), 0.0f};
                g.flags = (v(b, 6) > 0.5f ? 1u : 0u) | (v(b, 7) > 0.5f ? 2u : 0u);  // 2: en el mundo
                break;
            case BlockKind::SpeedLimit:
                g = gpuBlock(T::SpeedLimit);
                g.a = Vec4{v(b, 0), v(b, 1), 0.0f, 0.0f};
                break;
            case BlockKind::ConformSphere:
                g = gpuBlock(T::ConformSphere);
                g.a = Vec4{v3(b, 0), v(b, 3)};
                g.b = Vec4{v(b, 4), v(b, 5), 0.0f, 0.0f};
                break;
            case BlockKind::AngularVelocity: {
                // Grados -> radianes (la particula guarda el giro en rad/s).
                constexpr float kRad = 0.0174532925f;
                g = gpuBlock(T::AngularVelocity);
                g.a = Vec4{v(b, 0) * kRad, std::max(v(b, 1), 0.0f), v(b, 2) * kRad, 0.0f};
                break;
            }
            case BlockKind::TriggerOnDie:
            case BlockKind::TriggerOnCollide: {
                if (!b.effect.valid()) continue;  // sin efecto que hacer nacer
                const bool die = b.kind == BlockKind::TriggerOnDie;
                g = gpuBlock(die ? T::TriggerOnDie : T::TriggerOnCollide);
                g.a = Vec4{std::clamp(v(b, 0), 0.0f, 64.0f), std::clamp(v(b, 1), 0.0f, 1.0f),
                           die ? 0.0f : std::max(v(b, 2), 0.0f), v(b, die ? 2 : 3)};
                g.pad0 = trigger_index++;  // indice en triggerTargets (el pase lo cambia por la ranura)
                break;
            }
            case BlockKind::EmitOnDeath:
            case BlockKind::EmitOnCollision:
            case BlockKind::EmitEvery: {
                if (b.text.empty()) continue;  // sin canal
                const T type = b.kind == BlockKind::EmitOnDeath       ? T::EmitOnDeath
                               : b.kind == BlockKind::EmitOnCollision ? T::EmitOnCollision
                                                                      : T::EmitEvery;
                g = gpuBlock(type);
                g.a = c4(b, 3);
                const float extra = b.kind == BlockKind::EmitOnDeath       ? 0.0f
                                    : b.kind == BlockKind::EmitOnCollision ? std::max(v(b, 8), 0.0f)
                                                                           : std::max(v(b, 8), 0.01f);
                g.b = Vec4{std::max(std::round(v(b, 0)), 0.0f), v(b, 1), std::clamp(v(b, 2), 0.0f, 1.0f), extra};
                g.flags = (v(b, 7) > 0.5f ? 1u : 0u) | (v(b, 9) > 0.5f ? 2u : 0u);  // 2: valor = id de la particula
                // pad1: indice en eventChannels; pad0: el canal, que pone
                // VfxSystem (sin resolver no apunta nada).
                g.pad0 = gfx::VfxPass::kNoChannel;
                g.pad1 = channelSlot(b.text);
                break;
            }
            default: continue;
        }
        out.update.push_back(g);
    }

    out.color_over_life = false;
    out.size_over_life = false;
    out.color_by_speed = false;
    out.light = false;
    for (const Block& b : graph.output_blocks) {
        if (!b.enabled) continue;
        if (b.kind == BlockKind::ColorOverLife) {
            out.color_over_life = true;
            for (std::uint32_t i = 0; i < gfx::kVfxLutSize; ++i) {
                out.color_lut[i] = sampleGradient(b.gradient, static_cast<float>(i) / static_cast<float>(gfx::kVfxLutSize - 1));
            }
        } else if (b.kind == BlockKind::SizeOverLife) {
            out.size_over_life = true;
            for (std::uint32_t i = 0; i < gfx::kVfxLutSize; ++i) {
                out.size_lut[i] = sampleCurve(b.curve, static_cast<float>(i) / static_cast<float>(gfx::kVfxLutSize - 1));
            }
        } else if (b.kind == BlockKind::ColorBySpeed) {
            out.color_by_speed = true;
            out.speed_color_slow = c4(b, 0);
            out.speed_color_fast = c4(b, 4);
            out.speed_min = v(b, 8);
            out.speed_max = v(b, 9);
        } else if (b.kind == BlockKind::ParticleLight && !out.light) {
            // Luz por particula (el primero activo): cada campo puede ir atado a
            // un parametro expuesto (setEffectFloat / setEffectBool la mueven).
            const float radius = v(b, 1);
            const float intensity = v(b, 2);
            if (v(b, 0) < 0.5f || radius <= 0.0f || intensity <= 0.0f) continue;
            out.light = true;
            out.light_radius = std::clamp(radius, 0.01f, 1000.0f);
            out.light_intensity = intensity;
            out.light_falloff = v(b, 3) >= 0.5f ? gfx::VfxLightFalloff::Linear : gfx::VfxLightFalloff::Physical;
            out.light_exponent = std::clamp(v(b, 4), 0.01f, 64.0f);
            out.light_max = static_cast<std::uint32_t>(std::clamp(v(b, 5), 1.0f, 256.0f) + 0.5f);
            out.light_priority =
                v(b, 6) >= 0.5f ? gfx::VfxLightPriority::CameraDistance : gfx::VfxLightPriority::IntensityRadius;
            out.light_translucent = v(b, 7) >= 0.5f;
        }
    }

    const OutputSettings& o = graph.output;
    out.blend = o.blend;
    out.orient = o.orient;
    out.soft_distance = o.soft_distance;
    out.intensity = o.intensity;
    out.stretch = o.stretch;
    out.alpha_clip = o.alpha_clip;
    out.lit = o.lit;
    out.flip_cols = static_cast<std::uint32_t>(std::max(o.flip_cols, 1));
    out.flip_rows = static_cast<std::uint32_t>(std::max(o.flip_rows, 1));
    out.flip_fps = o.flip_fps;
    out.flip_blend = o.flip_blend;
    out.world_space = graph.world_space;
    out.capacity = static_cast<std::uint32_t>(std::clamp(graph.capacity, 1, static_cast<int>(gfx::VfxPass::kMaxCapacity)));
    // Salida: cinta o malla (la ranura de la malla la pone VfxSystem).
    out.output = o.type;
    out.ribbon_facing = o.ribbon_facing;
    out.ribbon_uv = o.ribbon_uv;
    out.ribbon_tile = std::max(o.ribbon_tile, 0.001f);
    out.ribbon_gap = std::max(o.ribbon_gap, 0.0f);
    out.ribbon_vector = o.ribbon_vector;
    out.ribbon_tension = std::clamp(o.ribbon_tension, 0.0f, 1.0f);
    out.ribbon_subdivisions = std::clamp(o.ribbon_subdivisions, 1, 16);
    out.ribbon_adaptive = o.ribbon_adaptive;
    out.ribbon_atlas_cell = std::max(o.ribbon_atlas_cell, -1);
    out.ribbon_uv_transform = Vec4{o.ribbon_uv_scale.x, o.ribbon_uv_scale.y, o.ribbon_uv_offset.x, o.ribbon_uv_offset.y};
    out.mesh_orient = o.mesh_orient;
    out.mesh_scale = o.mesh_scale;
    // Malla: cuantas variantes (las ranuras las pone VfxSystem), LOD, sprite
    // lejano, giro de Fija (normalizado) y escala al azar.
    out.mesh_variant_count = static_cast<std::uint32_t>(
        std::min<std::size_t>(o.mesh_variants.size() + 1, gfx::kVfxMeshVariants));
    out.mesh_lod_distance = std::max(o.mesh_lod_distance, 0.0f);
    out.mesh_sprite_distance = std::max(o.mesh_sprite_distance, 0.0f);
    {
        const Vec4& q = o.mesh_rotation;
        const float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        out.mesh_rotation = len > 1e-6f ? Vec4{q.x / len, q.y / len, q.z / len, q.w / len} : Vec4{0.0f, 0.0f, 0.0f, 1.0f};
    }
    out.mesh_scale_jitter = Vec3{std::clamp(o.mesh_scale_jitter.x, 0.0f, 1.0f), std::clamp(o.mesh_scale_jitter.y, 0.0f, 1.0f),
                                 std::clamp(o.mesh_scale_jitter.z, 0.0f, 1.0f)};
    // Distorsion: px a 1080p (como mucho kVfxMaxDistortionPixels).
    out.distortion = std::clamp(o.distortion, 0.0f, gfx::kVfxMaxDistortionPixels);
    out.distortion_height = o.distortion_height;
    // Nacer donde estaba el emisor en su instante: las cintas (si no, van a
    // escalones) y lo que nace por distancia (estelas de quads).
    bool by_distance = false;
    for (const Block& b : graph.spawn) by_distance = by_distance || (b.enabled && b.kind == BlockKind::RateOverDistance);
    out.interpolate_spawn = graph.world_space && (o.type == gfx::VfxOutput::Ribbon || by_distance);

    // Canales de datos que escucha (Spawn): nacen en la GPU donde fue cada
    // evento, asi que el efecto simula en el mundo.
    out.listens.clear();
    for (const Block& b : graph.spawn) {
        if (!b.enabled || b.kind != BlockKind::OnGpuEvent || b.text.empty()) continue;
        if (out.listens.size() >= gfx::VfxPass::kMaxListens) break;
        gfx::VfxBlock g = gpuBlock(T::OnGpuEvent);
        const float type = std::round(v(b, 0));
        g.a = Vec4{type < 0.0f ? -1.0f : type, std::clamp(std::round(v(b, 1)), 0.0f, 64.0f), std::max(std::round(v(b, 2)), 0.0f),
                   v(b, 3)};
        g.flags = (v(b, 4) > 0.5f ? 1u : 0u) | (v(b, 5) > 0.5f ? 2u : 0u);
        g.pad0 = gfx::VfxPass::kNoChannel;  // como los Emit*: pad1 indice, pad0 lo pone VfxSystem
        g.pad1 = channelSlot(b.text);
        out.listens.push_back(g);
    }
    if (!out.listens.empty()) out.world_space = true;
}

std::vector<std::string> eventChannels(const VfxGraph& graph) {
    std::vector<std::string> names;
    const auto add = [&](const Block& b) {
        if (!b.enabled || !isChannelBlock(b.kind) || b.text.empty()) return;
        if (std::find(names.begin(), names.end(), b.text) == names.end()) names.push_back(b.text);
    };
    for (const Block& b : graph.spawn) add(b);
    for (const Block& b : graph.update) add(b);
    return names;
}

std::vector<Uuid> triggerTargets(const VfxGraph& graph) {
    std::vector<Uuid> targets;
    for (const Block& b : graph.update) {
        if (b.enabled && (b.kind == BlockKind::TriggerOnDie || b.kind == BlockKind::TriggerOnCollide) && b.effect.valid()) {
            targets.push_back(b.effect.uuid);
        }
    }
    return targets;
}

std::uint32_t advanceSpawn(const VfxGraph& graph, const ParamValues& params, SpawnState& state, float dt, float moved,
                           bool emitting, std::mt19937& /*random*/) {
    double count = 0.0;
    // Eventos: siempre (aunque no emita o el ciclo haya terminado).
    for (const std::string& event : state.pending_events) {
        for (const Block& b : graph.spawn) {
            if (b.enabled && b.kind == BlockKind::OnEvent && b.text == event) count += std::max(blockValue(b, 0, params), 0.0f);
        }
    }
    state.pending_events.clear();

    dt = std::max(dt, 0.0f);
    if (state.burst_cycles.size() != graph.spawn.size()) state.burst_cycles.assign(graph.spawn.size(), 0);
    if (!state.started) {
        state.started = true;
        state.time = 0.0f;
        state.total_time = 0.0f;
        state.rate_carry = 0.0f;
        state.distance_carry = 0.0f;
        state.finished = false;
        std::fill(state.burst_cycles.begin(), state.burst_cycles.end(), 0);
    }
    const float previous_total = state.total_time;
    state.total_time += dt;
    if (!emitting || state.finished || state.total_time <= graph.start_delay) {
        return static_cast<std::uint32_t>(std::min(count, 1.0e7));
    }
    // Tiempo de este frame dentro de los ciclos (sin el retraso inicial).
    float remaining = std::min(dt, state.total_time - std::max(previous_total, graph.start_delay));
    const float duration = std::max(graph.duration, 0.01f);
    float frame_rate_time = 0.0f;  // tiempo que emitio de verdad (caudal)
    while (remaining > 0.0f && !state.finished) {
        const float segment = std::min(remaining, duration - state.time);
        const float t0 = state.time;
        const float t1 = state.time + segment;
        for (std::size_t i = 0; i < graph.spawn.size(); ++i) {
            const Block& b = graph.spawn[i];
            if (!b.enabled) continue;
            if (b.kind == BlockKind::Burst) {
                const float at = std::max(blockValue(b, 1, params), 0.0f);
                if (state.burst_cycles[i] == 0 && at >= t0 && (at < t1 || (at == t0 && segment > 0.0f))) {
                    count += std::max(blockValue(b, 0, params), 0.0f);
                    state.burst_cycles[i] = 1;
                }
            } else if (b.kind == BlockKind::PeriodicBurst) {
                const float interval = std::max(blockValue(b, 1, params), 0.01f);
                const int cycles = static_cast<int>(blockValue(b, 2, params));
                int expected = static_cast<int>(std::floor(t1 / interval)) + 1;  // en 0, interval, 2*interval...
                if (t1 >= duration) expected = static_cast<int>(std::floor((duration - 1e-4f) / interval)) + 1;
                if (cycles > 0) expected = std::min(expected, cycles);
                if (expected > state.burst_cycles[i]) {
                    count += static_cast<double>(expected - state.burst_cycles[i]) * std::max(blockValue(b, 0, params), 0.0f);
                    state.burst_cycles[i] = expected;
                }
            }
        }
        frame_rate_time += segment;
        state.time = t1;
        remaining -= segment;
        if (state.time >= duration - 1e-6f) {
            if (graph.loop) {
                state.time = 0.0f;
                std::fill(state.burst_cycles.begin(), state.burst_cycles.end(), 0);
            } else {
                state.finished = true;
            }
        }
        if (segment <= 0.0f) break;
    }
    // Caudal y distancia: continuos (con la fraccion que sobra).
    double rate = 0.0;
    double per_meter = 0.0;
    for (const Block& b : graph.spawn) {
        if (!b.enabled) continue;
        if (b.kind == BlockKind::ConstantRate) rate += std::max(blockValue(b, 0, params), 0.0f);
        if (b.kind == BlockKind::RateOverDistance) per_meter += std::max(blockValue(b, 0, params), 0.0f);
    }
    const double from_rate = rate * frame_rate_time + state.rate_carry;
    const double whole_rate = std::floor(from_rate);
    state.rate_carry = static_cast<float>(from_rate - whole_rate);
    const double from_distance = per_meter * std::max(moved, 0.0f) + state.distance_carry;
    const double whole_distance = std::floor(from_distance);
    state.distance_carry = static_cast<float>(from_distance - whole_distance);
    count += whole_rate + whole_distance;
    return static_cast<std::uint32_t>(std::min(count, 1.0e7));
}

// --- Las cuentas de los shaders (tests) --------------------------------------------

float flipbookStartValue(int frame, float speed) {
    const float start = static_cast<float>(std::max(frame, 0));
    if (std::fabs(speed - 1.0f) < 1e-6f) return start;
    return start + std::clamp(speed - 0.5f, 0.001f, 0.999f);  // vfx_sim.comp B_FLIPBOOK_FRAME
}

FlipbookSample flipbookSample(float start_value, float age, float lifetime, int frames, float fps) {
    // vfx.vert (quads): indice continuo, cuadro actual y siguiente.
    FlipbookSample s;
    if (frames <= 1) return s;
    const float n = static_cast<float>(frames);
    const float start = std::floor(start_value);
    const float speed = start_value > start ? start_value - start + 0.5f : 1.0f;
    float f = 0.0f;
    float current = 0.0f;
    float next = 0.0f;
    if (fps > 0.0f) {
        f = start + age * fps * speed;
        current = std::fmod(std::floor(f), n);
        next = std::fmod(current + 1.0f, n);
    } else {
        const float t = lifetime > 0.0f ? std::clamp(age / lifetime, 0.0f, 1.0f) : 1.0f;
        f = start + t * n * speed;
        current = std::min(std::floor(f), n - 1.0f);
        next = std::min(current + 1.0f, n - 1.0f);
    }
    s.current = static_cast<int>(current + 0.5f) % frames;
    s.next = static_cast<int>(next + 0.5f) % frames;
    s.blend = next == current ? 0.0f : f - std::floor(f);
    return s;
}

namespace {

// vfx_common.glsl vfxEncodeDirection / vfxDecodeDirection (octaedrica).
void encodeOctahedral(Vec3 n, float& px, float& py) {
    const float sum = std::fabs(n.x) + std::fabs(n.y) + std::fabs(n.z) + 1e-8f;
    n = Vec3{n.x / sum, n.y / sum, n.z / sum};
    px = n.x;
    py = n.y;
    if (n.z < 0.0f) {
        px = (1.0f - std::fabs(n.y)) * (n.x >= 0.0f ? 1.0f : -1.0f);
        py = (1.0f - std::fabs(n.x)) * (n.y >= 0.0f ? 1.0f : -1.0f);
    }
}

Vec3 decodeOctahedral(float px, float py) {
    Vec3 n{px, py, 1.0f - std::fabs(px) - std::fabs(py)};
    const float t = std::max(-n.z, 0.0f);
    n.x += n.x >= 0.0f ? -t : t;
    n.y += n.y >= 0.0f ? -t : t;
    const float len = core::length(n);
    return len > 1e-12f ? Vec3{n.x / len, n.y / len, n.z / len} : Vec3{0.0f, 1.0f, 0.0f};
}

}  // namespace

float ribbonPack(std::uint32_t id, const Vec3& axis) {
    float px = 0.0f;
    float py = 0.0f;
    encodeOctahedral(axis, px, py);
    const auto q = [](float v) {
        return static_cast<std::uint32_t>(std::clamp(std::floor((v * 0.5f + 0.5f) * 255.0f + 0.5f), 0.0f, 255.0f));
    };
    return static_cast<float>((std::min(id, 255u) << 16u) | (q(px) << 8u) | q(py));
}

std::uint32_t ribbonPackedId(float packed) {
    return static_cast<std::uint32_t>(std::max(packed, 0.0f)) >> 16u;
}

Vec3 ribbonPackedAxis(float packed) {
    const std::uint32_t bits = static_cast<std::uint32_t>(std::max(packed, 0.0f));
    return decodeOctahedral(static_cast<float>((bits >> 8u) & 255u) * (2.0f / 255.0f) - 1.0f,
                            static_cast<float>(bits & 255u) * (2.0f / 255.0f) - 1.0f);
}

float ribbonSortKey(std::uint32_t id, float age) {
    return static_cast<float>(id) * 65536.0f + std::clamp(age * 1024.0f, 0.0f, 65535.0f);
}

// --- Componente ----------------------------------------------------------------------

void VisualEffect::reflect(ecs::PropertyVisitor& v) {
    v.asset({"graph", "Efecto (.crvfx)", "El asset del VFX Graph (Crear > Efecto visual)"}, graph, assets::AssetType::VisualEffect);
    v.field({"play_on_awake", "Reproducir al empezar"}, play_on_awake);
    v.field({"seed", "Semilla", "0 = al azar cada vez"}, seed, 0, 1000000);
    v.field({"simulation_speed", "Velocidad", "Multiplica el tiempo de la simulacion"}, simulation_speed,
            ecs::FloatRange{0.0f, 10.0f, 0.01f, "%.2fx"});
    v.field({"preview_in_editor", "Vista previa en el editor", "Simular en la escena si esta seleccionado"}, preview_in_editor);
    v.field({"grass_radius", "Apartar hierba (m)",
             "Aparta la hierba del terreno en este radio alrededor del efecto, sin fisica (circulos de suelo, "
             "impactos a ras de suelo). 0 = no"},
            grass_radius, ecs::FloatRange{0.0f, 20.0f, 0.05f, "%.2f m"});
    v.field({"local_player", "Del jugador local",
             "Nunca se descarta por el presupuesto (ni por distancia ni por tope) y usa los limites de Heroe"},
            local_player);
    v.field({"priority", "Prioridad", "Con el tope lleno se quedan antes los de mas prioridad"}, priority, -100, 100);
    ecs::listField(v, {"params", "Parámetros", "Valores de los parametros expuestos del efecto para este objeto"}, params,
                   [](ParamOverride& p, ecs::PropertyVisitor& pv) {
                       pv.field({"name", "Nombre"}, p.name);
                       pv.field({"value", "Valor", "Float = X; Color = RGB (y A abajo)"}, p.value, ecs::Vec3Kind::Position);
                       pv.field({"w", "Alfa / W"}, p.w, ecs::FloatRange{0.0f, 1.0f, 0.01f, "%.2f"});
                   });
}

void VfxGroup::reflect(ecs::PropertyVisitor& v) {
    v.field({"local_player", "Del jugador local",
             "El grupo entero nunca se descarta por el presupuesto (ni por distancia ni por tope)"},
            local_player);
    v.field({"priority", "Prioridad", "Con el tope lleno se quedan antes los grupos de mas prioridad"}, priority, -100,
            100);
}

void registerVfxComponents() {
    ecs::ComponentRegistry& registry = ecs::ComponentRegistry::instance();
    if (registry.find("VisualEffect") == nullptr) {
        registry.registerComponent<VisualEffect>("VisualEffect", "Visual Effect (VFX Graph)", "Efectos");
    }
    if (registry.find("VfxGroup") == nullptr) {
        registry.registerComponent<VfxGroup>("VfxGroup", "VFX Group (presupuesto)", "Efectos");
    }
}

VfxSystem* activeSystem() { return g_active; }
void setActiveSystem(VfxSystem* system) { g_active = system; }

// --- Escalabilidad -------------------------------------------------------------------

const char* effectTypeKey(EffectType type) { return kEffectTypeKeys[typeSlot(type)]; }
const char* effectTypeLabel(EffectType type) { return kEffectTypeLabels[typeSlot(type)]; }

EffectType effectTypeFromKey(const std::string& key, EffectType fallback) {
    for (int i = 0; i < kEffectTypeSlots; ++i) {
        if (key == kEffectTypeKeys[i]) return static_cast<EffectType>(i);
    }
    return fallback;
}

const char* overBudgetKey(OverBudget policy) { return kOverBudgetKeys[std::clamp(static_cast<int>(policy), 0, 2)]; }

void setQualityPreset(int level) { g_quality_preset.store(std::clamp(level, 0, 3)); }

int qualityLevel() {
    const int q = g_vfx_quality.get();
    return q >= 0 ? std::clamp(q, 0, 3) : g_quality_preset.load();
}

VfxPlatform currentPlatform() {
    const int forced = g_vfx_platform.get();
    if (forced >= 0) return static_cast<VfxPlatform>(std::clamp(forced, 0, 1));
#if defined(__ANDROID__)
    return VfxPlatform::Mobile;
#else
    return VfxPlatform::Desktop;
#endif
}

VfxScalability defaultScalability(int quality, VfxPlatform platform) {
    const int q = std::clamp(quality, 0, 3);
    constexpr std::uint32_t kAll = gfx::kVfxOutLight | gfx::kVfxOutDistortion | gfx::kVfxOutMesh;
    constexpr std::uint32_t kDist = gfx::kVfxOutDistortion;
    // Escritorio, de Baja a Ultra. Baja = Radeon Vega 7 con el VFX en ~2 ms:
    // pocas instancias por tipo, la mitad de particulas o menos y sin
    // salidas caras salvo en los del jugador.
    struct Row {
        float distance[4];
        int instances[4];
        float scale[4];
        std::uint32_t off[4];
        OverBudget policy;
    };
    static const Row kRows[kEffectTypes] = {
        // Heroe: siempre (sin distancia); si hay demasiados, los mas cercanos.
        {{0.0f, 0.0f, 0.0f, 0.0f}, {12, 16, 24, 32}, {0.6f, 0.8f, 1.0f, 1.0f}, {kDist, 0u, 0u, 0u}, OverBudget::RemoveFarthest},
        // Combate (los demas jugadores): los nuevos no entran si esta lleno.
        {{40.0f, 60.0f, 90.0f, 150.0f}, {24, 48, 72, 96}, {0.35f, 0.6f, 1.0f, 1.0f}, {kAll, kDist, 0u, 0u}, OverBudget::DontCreate},
        // Impacto: entran los nuevos cercanos, se quitan los lejanos.
        {{30.0f, 45.0f, 70.0f, 100.0f}, {24, 32, 48, 64}, {0.4f, 0.6f, 1.0f, 1.0f}, {kAll, kDist, 0u, 0u}, OverBudget::RemoveFarthest},
        // Ambiente: los lejanos se congelan (vuelven sin precalentar).
        {{25.0f, 40.0f, 70.0f, 120.0f}, {8, 16, 32, 48}, {0.3f, 0.5f, 0.8f, 1.0f}, {kAll, kAll, 0u, 0u}, OverBudget::Pause},
    };
    VfxScalability s;
    for (int t = 0; t < kEffectTypes; ++t) {
        EffectTypeBudget& b = s.types[static_cast<std::size_t>(t)];
        b.max_distance = kRows[t].distance[q];
        b.max_instances = kRows[t].instances[q];
        b.particle_scale = kRows[t].scale[q];
        b.disabled_outputs = kRows[t].off[q];
        b.policy = kRows[t].policy;
        if (platform == VfxPlatform::Mobile) {
            // Movil: menos distancia, menos instancias y particulas; sin luz,
            // distorsion ni mallas hasta Alta (en Alta, sin distorsion).
            b.max_distance *= 0.75f;
            b.max_instances = std::max(2, (b.max_instances * 3) / 5);
            b.particle_scale *= 0.6f;
            b.disabled_outputs |= q <= 1 ? kAll : (q == 2 ? kDist : 0u);
        }
    }
    static const int kPrewarmFrames[4] = {16, 12, 8, 6};
    s.prewarm_frames = platform == VfxPlatform::Mobile ? (q == 0 ? 0 : 16) : kPrewarmFrames[q];
    return s;
}

void applyBudget(std::vector<BudgetCandidate>& candidates, const VfxScalability& scalability) {
    using Decision = BudgetCandidate::Decision;
    std::vector<std::size_t> order;
    for (int t = 0; t < kEffectTypeSlots; ++t) {
        const bool typed = t < kEffectTypes;
        const EffectTypeBudget* budget = typed ? &scalability.types[static_cast<std::size_t>(t)] : nullptr;
        const Decision over = budget != nullptr && budget->policy == OverBudget::Pause ? Decision::Paused : Decision::Removed;
        order.clear();
        std::size_t essentials = 0;
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            BudgetCandidate& c = candidates[i];
            if (typeSlot(c.type) != t) continue;
            c.decision = Decision::Run;
            // Los del jugador local no se recortan por distancia.
            if (!c.essential && budget != nullptr && budget->max_distance > 0.0f && c.distance > budget->max_distance) {
                c.decision = over;
                continue;
            }
            if (c.essential) ++essentials;
            order.push_back(i);
        }
        if (budget != nullptr && budget->max_instances > 0 && order.size() > static_cast<std::size_t>(budget->max_instances)) {
            // Quienes se quedan: primero los del jugador local (siempre, aunque
            // se pasen del tope); despues, con DontCreate, los que ya corrian;
            // despues los de mas prioridad, los que se ven y los mas cercanos.
            const bool keep_running = budget->policy == OverBudget::DontCreate;
            std::stable_sort(order.begin(), order.end(), [&](std::size_t x, std::size_t y) {
                const BudgetCandidate& a = candidates[x];
                const BudgetCandidate& b = candidates[y];
                if (a.essential != b.essential) return a.essential;
                if (keep_running && a.running != b.running) return a.running;
                if (a.priority != b.priority) return a.priority > b.priority;
                if (a.in_view != b.in_view) return a.in_view;
                return a.distance < b.distance;
            });
            const std::size_t keep = std::max(static_cast<std::size_t>(budget->max_instances), essentials);
            for (std::size_t k = keep; k < order.size(); ++k) {
                candidates[order[k]].decision = over;
            }
            order.resize(keep);
        }
        for (const std::size_t i : order) {
            if (!candidates[i].in_view) candidates[i].decision = Decision::Hidden;
        }
    }
}

std::uint32_t takeSpread(std::uint32_t& backlog, std::uint32_t& rate, std::uint32_t added, int frames,
                         std::uint32_t min_per_frame) {
    std::uint32_t out = 0;
    if (frames <= 1) {
        out = added;  // este efecto no reparte: lo suyo sale ya
    } else if (added > 0) {
        backlog += added;
        const std::uint32_t even = (backlog + static_cast<std::uint32_t>(frames) - 1) / static_cast<std::uint32_t>(frames);
        rate = std::max({rate, even, min_per_frame});
    }
    if (backlog > 0) {
        const std::uint32_t n = std::min(backlog, std::max(rate, min_per_frame));
        backlog -= n;
        out += n;
    }
    if (backlog == 0) rate = 0;
    return out;
}

void queueSpread(std::uint32_t& backlog, std::uint32_t& rate, std::uint32_t count, int frames, std::uint32_t min_per_frame) {
    if (count == 0) return;
    backlog += count;
    const std::uint32_t f = static_cast<std::uint32_t>(std::max(frames, 1));
    rate = std::max({rate, (backlog + f - 1) / f, min_per_frame});
}

bool sphereInView(const core::Mat4& vp, const Vec3& center, float radius) {
    // Planos de Gribb-Hartmann: fila 3 +- fila 0 (izquierda, derecha) y +-
    // fila 1 (abajo, arriba). m[columna][fila].
    const auto plane = [&](int row, float sign) {
        return Vec4{vp.m[0][3] + sign * vp.m[0][row], vp.m[1][3] + sign * vp.m[1][row], vp.m[2][3] + sign * vp.m[2][row],
                    vp.m[3][3] + sign * vp.m[3][row]};
    };
    const Vec4 planes[4] = {plane(0, 1.0f), plane(0, -1.0f), plane(1, 1.0f), plane(1, -1.0f)};
    for (const Vec4& p : planes) {
        const float len = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
        if (len < 1e-8f) continue;
        if (p.x * center.x + p.y * center.y + p.z * center.z + p.w < -std::max(radius, 0.0f) * len) return false;
    }
    return true;
}

// --- Escalabilidad en VfxSystem ---------------------------------------------------------

void VfxSystem::setCamera(const Vec3& position, const core::Mat4& view_projection) {
    has_camera_ = true;
    camera_position_ = position;
    camera_view_projection_ = view_projection;
}

void VfxSystem::setScalability(const VfxScalability& table) {
    custom_scalability_ = table;
    has_custom_scalability_ = true;
}

void VfxSystem::resetScalability() { has_custom_scalability_ = false; }

float VfxSystem::timeScale() const { return time_scale_ * std::max(g_vfx_time_scale.get(), 0.0f); }

void VfxSystem::noteScalable(const VfxGraph& g) {
    if (g.effect_type != EffectType::None || g.fixed_bounds) any_scalable_ = true;
}

namespace {

// Bloques que leen o escriben canales de datos de la GPU: el efecto espera
// eventos que la CPU no ve (nunca se da por terminado).
bool usesGpuChannels(const VfxGraph& g) {
    for (const Block& b : g.spawn) {
        if (b.enabled && isChannelBlock(b.kind)) return true;
    }
    for (const Block& b : g.update) {
        if (b.enabled && isChannelBlock(b.kind)) return true;
    }
    return false;
}

float maxParticleLifetime(const VfxGraph& g) {
    float longest = 0.0f;
    for (const Block& b : g.initialize) {
        if (b.enabled && b.kind == BlockKind::Lifetime) longest = std::max({longest, b.values[0], b.values[1]});
    }
    return longest > 0.0f ? longest : 5.0f;
}

}  // namespace

bool effectFinished(const FinishedState& s) {
    if (s.emitting || s.pending) return false;
    // La lectura de las vivas llega un par de frames tarde: lo recien nacido
    // aun no sale en ella.
    constexpr float kReadbackDelay = 0.25f;
    if (s.quiet_seconds < kReadbackDelay) return false;
    if (s.quiet_seconds > s.max_lifetime + kReadbackDelay) return true;  // ya no puede quedar ninguna
    return s.alive_known && s.alive == 0;
}

bool VfxSystem::instanceFinished(const Instance& inst, const VfxGraph& g) const {
    // Sin emision pendiente: no emite (o su ciclo sin bucle acabo), sin
    // eventos ni rafagas repartidas, sin pausa y sin canales de la GPU.
    FinishedState s;
    s.emitting = inst.playing && !inst.spawn.finished;
    s.pending = inst.paused || !inst.spawn.pending_events.empty() || inst.spawn_backlog > 0;
    s.quiet_seconds = inst.quiet;
    if (s.emitting || s.pending || s.quiet_seconds < 0.25f) return false;
    if (usesGpuChannels(g)) return false;
    s.max_lifetime = maxParticleLifetime(g);
    if (renderer_ != nullptr) {
        s.alive_known = true;
        s.alive = renderer_->vfx().aliveCount(inst.id);
        for (const Instance::SubEmitter& child : inst.children) s.alive += renderer_->vfx().aliveCount(child.id);
    }
    return effectFinished(s);
}

EffectType VfxSystem::effectiveType(entt::entity handle, const VfxGraph& g) const {
    if (!type_overrides_.empty()) {
        const auto it = type_overrides_.find(handle);
        if (it != type_overrides_.end()) return it->second;
    }
    return g.effect_type;
}

void VfxSystem::planFrame(ecs::World& world, bool simulate, const std::vector<entt::entity>& preview) {
    decisions_.clear();
    type_overrides_.clear();
    planned_groups_.fill(-1);
    frame_scalability_ = has_custom_scalability_ ? custom_scalability_ : defaultScalability(qualityLevel(), currentPlatform());
    // Sin efectos con tipo ni caja (lo de antes) o fuera de Play (la vista
    // previa del editor se ve entera): nada que decidir.
    if (!any_scalable_ || !simulate || !g_vfx_scalability.get()) return;
    (void)preview;
    using Decision = BudgetCandidate::Decision;
    plan_scratch_.clear();
    plan_entities_.clear();
    plan_member_group_.clear();
    plan_members_.clear();
    plan_group_index_.clear();
    const bool group_prefabs = g_vfx_group_prefabs.get();
    for (const entt::entity handle : world.registry().view<VisualEffect>()) {
        const ecs::Entity e = world.wrap(handle);
        if (!e.activeInHierarchy()) continue;
        const VisualEffect& component = e.get<VisualEffect>();
        if (!component.graph.valid()) continue;
        const VfxGraph* g = graph(component.graph.uuid);
        if (g == nullptr || (g->effect_type == EffectType::None && !g->fixed_bounds)) continue;

        // Su grupo: el VfxGroup mas cercano hacia arriba; si no hay, la raiz
        // del prefab mas cercano (no para los de Ambiente: una celda entera
        // no es un grupo); si no, el efecto solo.
        entt::entity group_root = handle;
        const VfxGroup* group = nullptr;
        {
            entt::entity prefab_root = entt::null;
            const bool by_prefab = group_prefabs && g->effect_type != EffectType::Ambient && g->effect_type != EffectType::None;
            for (ecs::Entity p = e; p.valid(); p = p.parent()) {
                if (const VfxGroup* vg = p.tryGet<VfxGroup>()) {
                    group = vg;
                    group_root = p.handle();
                    break;
                }
                if (by_prefab && prefab_root == entt::null && p.has<ecs::PrefabInstance>()) prefab_root = p.handle();
            }
            if (group == nullptr && prefab_root != entt::null) group_root = prefab_root;
        }

        BudgetCandidate c;
        c.essential = component.local_player || (group != nullptr && group->local_player);
        c.priority = group != nullptr ? std::max(component.priority, group->priority) : component.priority;
        // Del jugador local: cuenta y se limita como Heroe.
        c.type = c.essential && g->effect_type != EffectType::None ? EffectType::Hero : g->effect_type;
        if (c.type != g->effect_type) type_overrides_[handle] = c.type;
        if (has_camera_) {
            const core::Mat4 m = e.worldMatrix();
            const Vec3 position{m.m[3][0], m.m[3][1], m.m[3][2]};
            c.distance = core::length(position - camera_position_);
            if (g->fixed_bounds) {
                const Vec4 center = m * Vec4{g->bounds_center, 1.0f};
                float scale = 0.0f;
                for (int col = 0; col < 3; ++col) {
                    scale = std::max(scale, core::length(Vec3{m.m[col][0], m.m[col][1], m.m[col][2]}));
                }
                c.in_view = sphereInView(camera_view_projection_, Vec3{center.x, center.y, center.z},
                                         0.5f * core::length(g->bounds_size) * scale);
            }
        }
        const auto it = instances_.find(handle);
        c.running = it != instances_.end() && it->second.budget_running;
        plan_entities_.push_back(handle);
        plan_members_.push_back(c);
        // Terminado (sin emision pendiente ni particulas vivas): no cuenta.
        if (it != instances_.end() && instanceFinished(it->second, *g)) {
            plan_member_group_.push_back(-1);
            continue;
        }
        // Al candidato del grupo: el tipo mas importante (Heroe, Combate,
        // Impacto, Ambiente; "sin tipo" no manda), la distancia menor, se ve
        // si se ve alguno, corria si corria alguno.
        const auto [slot, fresh] = plan_group_index_.try_emplace(group_root, static_cast<int>(plan_scratch_.size()));
        if (fresh) {
            plan_scratch_.push_back(c);
        } else {
            BudgetCandidate& gc = plan_scratch_[static_cast<std::size_t>(slot->second)];
            if (gc.type == EffectType::None || (c.type != EffectType::None && c.type < gc.type)) gc.type = c.type;
            gc.distance = std::min(gc.distance, c.distance);
            gc.in_view = gc.in_view || c.in_view;
            gc.running = gc.running || c.running;
            gc.essential = gc.essential || c.essential;
            gc.priority = std::max(gc.priority, c.priority);
        }
        plan_member_group_.push_back(slot->second);
    }
    applyBudget(plan_scratch_, frame_scalability_);
    // La decision del grupo, a cada efecto: entero o nada. Si el grupo corre,
    // un efecto con caja fija fuera de la vista se oculta (sin dibujar).
    planned_groups_.fill(0);
    for (const BudgetCandidate& gc : plan_scratch_) {
        const bool runs = gc.decision == Decision::Run || gc.decision == Decision::Hidden;
        if (runs && gc.type != EffectType::None) ++planned_groups_[static_cast<std::size_t>(typeSlot(gc.type))];
    }
    for (std::size_t i = 0; i < plan_entities_.size(); ++i) {
        const int group_index = plan_member_group_[i];
        Decision d = Decision::Finished;
        if (group_index >= 0) {
            d = plan_scratch_[static_cast<std::size_t>(group_index)].decision;
            if (d == Decision::Run && !plan_members_[i].in_view) d = Decision::Hidden;
        }
        if (d != Decision::Run) decisions_[plan_entities_[i]] = d;
    }
}

BudgetCandidate::Decision VfxSystem::decisionFor(entt::entity handle) const {
    if (decisions_.empty()) return BudgetCandidate::Decision::Run;
    const auto it = decisions_.find(handle);
    return it == decisions_.end() ? BudgetCandidate::Decision::Run : it->second;
}

void VfxSystem::applyTypeLimits(const VfxGraph& g, EffectType type, BudgetCandidate::Decision decision,
                                gfx::VfxInstanceDesc& desc) const {
    desc.sort_order = g.sort_order;
    if (decision == BudgetCandidate::Decision::Hidden || decision == BudgetCandidate::Decision::Paused) desc.visible = false;
    if (type == EffectType::None || !g_vfx_scalability.get()) return;
    const EffectTypeBudget& b = frame_scalability_.types[static_cast<std::size_t>(typeSlot(type))];
    desc.disabled_outputs = b.disabled_outputs;
    if (b.particle_scale < 0.999f) {
        const float scale = std::clamp(b.particle_scale, 0.05f, 1.0f);
        desc.capacity = std::max(std::min(16u, desc.capacity),
                                 static_cast<std::uint32_t>(std::ceil(static_cast<float>(desc.capacity) * scale)));
        desc.spawn_count = std::min(desc.spawn_count, desc.capacity);
    }
}

std::uint32_t VfxSystem::scaledSpawn(const VfxGraph& g, EffectType type, Instance& inst, std::uint32_t spawn, bool first,
                                     std::uint32_t warm) const {
    if (first) {
        inst.spawn_backlog = 0;
        inst.spread_rate = 0;
    }
    if (type != EffectType::None && g_vfx_scalability.get()) {
        const float scale = frame_scalability_.types[static_cast<std::size_t>(typeSlot(type))].particle_scale;
        spawn = scaleCount(spawn, scale, inst.scale_carry);
        warm = scaleCount(warm, scale, inst.scale_carry);
    }
    // Precalentado repartido en unos frames (de golpe daba un tiron).
    if (warm > 0) {
        const int forced = g_vfx_prewarm_frames.get();
        const int frames = forced >= 0 ? forced : frame_scalability_.prewarm_frames;
        if (frames == 1) {
            spawn += warm;  // de golpe, como antes
        } else if (frames > 1) {
            queueSpread(inst.spawn_backlog, inst.spread_rate, warm, frames);
        }  // 0: sin precalentado
    }
    return takeSpread(inst.spawn_backlog, inst.spread_rate, spawn, g.spawn_spread_frames);
}

bool VfxSystem::playAt(const Uuid& graph_uuid, const Vec3& position, const Vec3& forward) {
    const VfxGraph* g = graph(graph_uuid);
    if (g == nullptr) return false;
    // Su tipo: fuera de distancia o lleno (DontCreate) no entra.
    if (g->effect_type != EffectType::None && g_vfx_scalability.get()) {
        const int slot = typeSlot(g->effect_type);
        const EffectTypeBudget& b = frame_scalability_.types[static_cast<std::size_t>(slot)];
        if (has_camera_ && b.max_distance > 0.0f && core::length(position - camera_position_) > b.max_distance) return false;
        if (b.max_instances > 0) {
            int same = 0;
            for (const OneShot& s : one_shots_) {
                const VfxGraph* sg = graph(s.inst.graph);
                if (sg != nullptr && sg->effect_type == g->effect_type) ++same;
            }
            // Los de las entidades: los grupos que corrian en el ultimo
            // planFrame (una habilidad de 23 efectos cuenta 1); sin plan, los
            // efectos sueltos.
            const int planned = planned_groups_[static_cast<std::size_t>(slot)];
            const int entities = planned >= 0 ? planned : static_cast<int>(stats_.types[static_cast<std::size_t>(slot)].instances);
            if (same >= b.max_instances || (b.policy == OverBudget::DontCreate && same + entities >= b.max_instances)) {
                return false;
            }
        }
    }
    OneShot shot;
    shot.inst.id = next_id_++;
    shot.inst.graph = graph_uuid;
    shot.inst.seed = static_cast<std::uint32_t>(std::random_device{}() | 1u);
    shot.inst.random.seed(shot.inst.seed);
    shot.inst.started = true;
    shot.inst.playing = true;
    shot.inst.reset = true;
    shot.inst.seen = true;
    // Orientacion: +Z hacia `forward` (+Y arriba si se puede).
    Vec3 z = core::length(forward) > 1e-5f ? core::normalize(forward) : Vec3{0.0f, 0.0f, 1.0f};
    Vec3 up = std::fabs(z.y) < 0.99f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
    const Vec3 x = core::normalize(core::cross(up, z));
    const Vec3 y = core::cross(z, x);
    core::Mat4 m = core::Mat4::identity();
    m.m[0][0] = x.x; m.m[0][1] = x.y; m.m[0][2] = x.z;
    m.m[1][0] = y.x; m.m[1][1] = y.y; m.m[1][2] = y.z;
    m.m[2][0] = z.x; m.m[2][1] = z.y; m.m[2][2] = z.z;
    m.m[3][0] = position.x; m.m[3][1] = position.y; m.m[3][2] = position.z;
    shot.transform = m;
    // Cuanto vive: un ciclo de Spawn y la vida mas larga de sus particulas.
    float max_life = 0.0f;
    for (const Block& b : g->initialize) {
        if (b.enabled && b.kind == BlockKind::Lifetime) max_life = std::max({max_life, b.values[0], b.values[1]});
    }
    shot.lifetime = g->start_delay + g->duration + (max_life > 0.0f ? max_life : 5.0f) + 0.1f;
    one_shots_.push_back(std::move(shot));
    return true;
}

void VfxSystem::updateOneShots(float delta_seconds, std::vector<gfx::VfxInstanceDesc>& descs, VfxSystemStats& stats,
                               gfx::VulkanRenderer& renderer) {
    if (one_shots_.empty()) return;
    const float dt = std::max(delta_seconds, 0.0f) * timeScale();
    for (auto it = one_shots_.begin(); it != one_shots_.end();) {
        OneShot& s = *it;
        const VfxGraph* g = graph(s.inst.graph);
        if (g == nullptr || s.inst.time >= s.lifetime) {
            it = one_shots_.erase(it);  // su ranura de la GPU queda aparcada para el siguiente
            continue;
        }
        const int slot = typeSlot(g->effect_type);
        TypeTimer timer(stats.types[static_cast<std::size_t>(slot)].cpu_ms);
        ParamValues params;
        for (const ExposedParam& p : g->params) params[p.name] = p.value;
        // Un solo ciclo aunque el efecto tenga bucle.
        const bool emitting = s.inst.time < g->start_delay + g->duration;
        const bool first = s.inst.reset;
        std::uint32_t spawn = advanceSpawn(*g, params, s.inst.spawn, dt, 0.0f, emitting, s.inst.random);
        spawn = scaledSpawn(*g, g->effect_type, s.inst, spawn, first, 0);
        s.inst.time += dt;
        gfx::VfxInstanceDesc desc;
        compileGraph(*g, params, desc);
        desc.id = s.inst.id;
        desc.transform = s.transform;
        desc.reset = s.inst.reset;
        desc.spawn_count = std::min(spawn, desc.capacity);
        desc.delta_seconds = dt;
        desc.time = s.inst.time;
        desc.seed = s.inst.seed;
        desc.interpolate_spawn = false;
        desc.event_targets.clear();  // sin subemisores: sus Trigger no apuntan nada
        applyTypeLimits(*g, g->effect_type, BudgetCandidate::Decision::Run, desc);
        resolveResources(s.inst.graph, *g, s.inst.points, s.inst.points_model, desc, renderer);
        s.inst.reset = false;
        type_ids_.emplace_back(desc.id, slot);
        stats.capacity += desc.capacity;
        ++stats.effects;
        descs.push_back(std::move(desc));
        ++it;
    }
}

void VfxSystem::finishStats(VfxSystemStats& stats, gfx::VulkanRenderer& renderer) {
    stats.quality = qualityLevel();
    stats.parked = static_cast<std::uint32_t>(renderer.vfx().parkedSlots());
    stats.one_shots = static_cast<std::uint32_t>(one_shots_.size());
    for (const auto& [id, slot] : type_ids_) {
        VfxTypeStats& t = stats.types[static_cast<std::size_t>(slot)];
        ++t.instances;
        t.particles += renderer.vfx().aliveCount(id);
    }
    // GPU: el pase "VFX Graph" del perfilador de la GPU, repartido por lo que
    // cuesta cada tipo (particulas y un fijo por instancia: sus dispatch).
    float gpu_total = 0.0f;
    for (const gfx::GpuTiming& t : renderer.gpuProfiler().lastTimings()) {
        if (t.name == "VFX Graph") gpu_total += t.milliseconds;
    }
    std::array<double, kEffectTypeSlots> weight{};
    double weight_total = 0.0;
    for (int i = 0; i < kEffectTypeSlots; ++i) {
        const VfxTypeStats& t = stats.types[static_cast<std::size_t>(i)];
        weight[static_cast<std::size_t>(i)] = static_cast<double>(t.particles) + 256.0 * static_cast<double>(t.instances);
        weight_total += weight[static_cast<std::size_t>(i)];
    }
    if (weight_total > 0.0) {
        for (int i = 0; i < kEffectTypeSlots; ++i) {
            stats.types[static_cast<std::size_t>(i)].gpu_ms =
                static_cast<float>(gpu_total * weight[static_cast<std::size_t>(i)] / weight_total);
        }
    }
    if (prof::enabled()) {
        for (int i = 0; i < kEffectTypeSlots; ++i) {
            const VfxTypeStats& t = stats.types[static_cast<std::size_t>(i)];
            const VfxTypeStats& before = stats_.types[static_cast<std::size_t>(i)];
            if (t.instances == 0 && t.culled == 0 && before.instances == 0 && before.culled == 0) continue;  // tipo sin usar
            prof::counter(kCounterInstances[i], static_cast<double>(t.instances));
            prof::counter(kCounterParticles[i], static_cast<double>(t.particles));
            prof::counter(kCounterMs[i], static_cast<double>(t.cpu_ms + t.gpu_ms));
        }
    }
}

// --- Sistema -------------------------------------------------------------------------

void VfxSystem::setGraphOverride(const Uuid& uuid, const VfxGraph& graph) {
    overrides_[uuid] = graph;
    noteScalable(graph);
}

void VfxSystem::clearGraphOverride(const Uuid& uuid) { overrides_.erase(uuid); }

// --- Material de la salida Malla y distorsion ------------------------------------

VfxMaterial vfxMaterialFrom(const VfxMaterialSource& source) {
    VfxMaterial m;
    m.valid = true;
    m.tint = source.base_color;
    const float e = std::max(source.emissive_intensity, 0.0f);
    m.emissive = Vec3{source.emissive.x * e, source.emissive.y * e, source.emissive.z * e};
    m.albedo = source.albedo;
    m.lit = !source.unlit;
    // Opaco sin mezcla; transparente: aditivo o alfa (el vidrio, como alfa).
    m.blend = !source.transparent ? gfx::VfxBlend::Opaque
              : source.transparent_blend == 2 ? gfx::VfxBlend::Additive
                                              : gfx::VfxBlend::Alpha;
    return m;
}

void applyVfxMaterial(const VfxMaterial& material, gfx::VfxInstanceDesc& desc) {
    if (!material.valid) return;
    // La mezcla Distorsion de la salida manda (el material no pinta en ella).
    if (desc.blend != gfx::VfxBlend::Distortion) desc.blend = material.blend;
    desc.lit = material.lit;
    desc.tint = material.tint;
    desc.emissive = material.emissive;
}

bool vfxDistortionCvar() { return g_vfx_distortion.get(); }

VfxMaterial& VfxSystem::vfxMaterial(const Uuid& uuid) {
    auto it = materials_.find(uuid);
    if (it != materials_.end()) return it->second;
    VfxMaterial material;  // sin archivo o danado: no valido (la salida sigue como esta)
    const std::filesystem::path file = resolver_ ? resolver_(uuid) : std::filesystem::path{};
    assets::MaterialAsset asset;
    std::string error;
    if (!file.empty() && assets::loadMaterial(file, asset, &error)) {
        VfxMaterialSource source;
        source.transparent = asset.mode == assets::MaterialMode::Transparent;
        source.transparent_blend = static_cast<int>(asset.blend);
        source.unlit = asset.unlit;
        source.base_color = asset.base_color;
        source.emissive = asset.emissive;
        source.emissive_intensity = asset.emissive_intensity;
        source.albedo = asset.albedo;
        material = vfxMaterialFrom(source);
    } else if (!file.empty()) {
        std::cerr << "[VFX] Material de la malla: " << error << "\n";
    }
    return materials_.emplace(uuid, std::move(material)).first->second;
}

void VfxSystem::reloadGraphs() {
    graphs_.clear();
    materials_.clear();
    mesh_points_.clear();
    model_looks_.clear();
    output_mesh_failed_.clear();
    resource_warnings_.clear();
    for (auto& [handle, inst] : instances_) inst.points.reset();
}

void VfxSystem::reloadAsset(const Uuid& uuid) {
    if (!uuid.valid()) return;
    const bool graph_changed = graphs_.erase(uuid) > 0;
    materials_.erase(uuid);
    resource_warnings_.erase(uuid);
    if (!graph_changed) return;
    // Los que lo usan empiezan de nuevo con el nuevo (sus bloques pueden
    // ser otros: las particulas de antes no sirven).
    for (auto& [handle, inst] : instances_) {
        if (inst.graph != uuid) continue;
        inst.reset = true;
        inst.spawn = SpawnState{};
        inst.points.reset();
        inst.has_last_position = false;
    }
    std::cout << "[VFX] Efecto recargado: " << uuid.toString() << "\n";
}

int VfxSystem::reloadTextureFile(const std::string& relative) {
    if (renderer_ == nullptr || relative.empty()) return 0;
    const std::filesystem::path file = assets_root_ / std::filesystem::path(std::u8string(relative.begin(), relative.end()));
    // Se relee en su misma ranura: lo guardado sigue valiendo.
    return renderer_->vfx().reloadTexture(file) ? 1 : 0;
}

std::string VfxSystem::resourceWarning(const Uuid& graph) const {
    const auto it = resource_warnings_.find(graph);
    return it == resource_warnings_.end() ? std::string{} : it->second;
}

void VfxSystem::syncResourceEpoch(gfx::VulkanRenderer& renderer) {
    const std::uint64_t epoch = renderer.vfx().resourceEpoch();
    if (epoch == resource_epoch_) return;
    resource_epoch_ = epoch;
    // El pase solto (o dio a otra) alguna ranura: lo guardado se pide otra vez.
    for (auto& [uuid, cg] : graphs_) {
        cg.texture = -1;
        cg.texture_path.clear();
    }
    for (auto& [uuid, material] : materials_) material.texture = -2;
    for (auto& [uuid, look] : model_looks_) look.texture = -2;
    output_mesh_failed_.clear();
}

VfxSystem::ModelLook& VfxSystem::modelLook(const Uuid& model) {
    ModelLook& look = model_looks_[model];
    if (look.ready || !model_provider_) return look;
    const std::shared_ptr<const assets::ModelAsset> asset = model_provider_(model);
    if (!asset) return look;  // aun cargando: se vuelve a mirar
    look.ready = true;
    // El material de la primera pieza que tenga alguno (el de su primera submalla).
    for (const auto& part : asset->parts) {
        if (!part || part->materials.empty()) continue;
        const std::size_t index = part->submeshes.empty() ? 0u : std::min<std::size_t>(part->submeshes.front().material,
                                                                                         part->materials.size() - 1);
        const asset::MaterialData& m = part->materials[index];
        look.tint = m.base_color;
        if (m.albedo_texture >= 0 && static_cast<std::size_t>(m.albedo_texture) < part->textures.size()) {
            const asset::TextureData& t = part->textures[static_cast<std::size_t>(m.albedo_texture)];
            // (source_path sale de path::string() al importar: se vuelve igual.)
            const std::filesystem::path file = t.source_path.empty() ? std::filesystem::path{} : std::filesystem::path(t.source_path);
            std::error_code ec;
            if (!file.empty() && std::filesystem::is_regular_file(file, ec)) {
                look.file = file;
            } else {
                look.embedded = true;
            }
        }
        break;
    }
    return look;
}

VfxSystem::CachedGraph* VfxSystem::cached(const Uuid& uuid) {
    if (!uuid.valid()) return nullptr;
    auto it = graphs_.find(uuid);
    if (it == graphs_.end()) {
        CachedGraph entry;
        const std::filesystem::path file = resolver_ ? resolver_(uuid) : std::filesystem::path{};
        std::string error;
        if (!file.empty() && loadVfxGraph(file, entry.graph, &error)) {
            entry.valid = true;
            noteScalable(entry.graph);
        } else if (!file.empty()) {
            std::cerr << "[VFX] " << error << "\n";
        }
        it = graphs_.emplace(uuid, std::move(entry)).first;
    }
    return &it->second;
}

const VfxGraph* VfxSystem::graph(const Uuid& uuid) {
    const auto o = overrides_.find(uuid);
    if (o != overrides_.end()) return &o->second;
    CachedGraph* c = cached(uuid);
    return c != nullptr && c->valid ? &c->graph : nullptr;
}

VfxSystem::Instance& VfxSystem::instanceOf(ecs::Entity entity) {
    Instance& inst = instances_[entity.handle()];
    if (inst.id == 0) inst.id = next_id_++;
    return inst;
}

const VfxSystem::Instance* VfxSystem::findInstance(ecs::Entity entity) const {
    const auto it = instances_.find(entity.handle());
    return it == instances_.end() ? nullptr : &it->second;
}

ParamValues VfxSystem::paramsFor(const VfxGraph& graph, const VisualEffect& component, const Instance& instance) const {
    ParamValues values;
    for (const ExposedParam& p : graph.params) values[p.name] = p.value;
    for (const ParamOverride& o : component.params) {
        const ExposedParam* p = graph.findParam(o.name);
        if (p == nullptr) continue;
        values[o.name] = Vec4{o.value, o.w};
    }
    for (const auto& [name, value] : instance.script_params) values[name] = value;
    return values;
}

std::shared_ptr<const std::vector<Vec4>> VfxSystem::meshPoints(const Uuid& model) {
    const auto found = mesh_points_.find(model);
    if (found != mesh_points_.end()) return found->second;
    if (!model_provider_) return nullptr;
    const std::shared_ptr<const assets::ModelAsset> asset = model_provider_(model);
    if (!asset) return nullptr;  // aun cargando: se vuelve a intentar

    // Matriz de cada nodo (sus piezas estan en el espacio del nodo).
    std::vector<core::Mat4> world(asset->nodes.size(), core::Mat4::identity());
    for (std::size_t i = 0; i < asset->nodes.size(); ++i) {
        const assets::ModelNode& n = asset->nodes[i];
        world[i] = n.parent >= 0 && static_cast<std::size_t>(n.parent) < i ? world[static_cast<std::size_t>(n.parent)] * n.local : n.local;
    }
    struct Tri {
        Vec3 a, b, c, n;
        double area;
    };
    std::vector<Tri> tris;
    double total = 0.0;
    for (std::size_t i = 0; i < asset->nodes.size(); ++i) {
        const int part = asset->nodes[i].part;
        if (part < 0 || static_cast<std::size_t>(part) >= asset->parts.size() || !asset->parts[static_cast<std::size_t>(part)]) continue;
        const asset::ModelData& data = *asset->parts[static_cast<std::size_t>(part)];
        const core::Mat4& m = world[i];
        const auto tp = [&](const Vec3& p) {
            const Vec4 r = m * Vec4{p, 1.0f};
            return Vec3{r.x, r.y, r.z};
        };
        for (std::size_t t = 0; t + 2 < data.indices.size(); t += 3) {
            const std::uint32_t i0 = data.indices[t], i1 = data.indices[t + 1], i2 = data.indices[t + 2];
            if (i0 >= data.vertices.size() || i1 >= data.vertices.size() || i2 >= data.vertices.size()) continue;
            Tri tri;
            tri.a = tp(data.vertices[i0].position);
            tri.b = tp(data.vertices[i1].position);
            tri.c = tp(data.vertices[i2].position);
            const Vec3 cr = core::cross(tri.b - tri.a, tri.c - tri.a);
            const float len = core::length(cr);
            if (len < 1e-10f) continue;
            tri.n = cr * (1.0f / len);
            tri.area = 0.5 * len;
            total += tri.area;
            tris.push_back(tri);
        }
    }
    auto points = std::make_shared<std::vector<Vec4>>();
    if (!tris.empty() && total > 0.0) {
        constexpr int kPoints = 4096;
        std::vector<double> cumulative(tris.size());
        double acc = 0.0;
        for (std::size_t i = 0; i < tris.size(); ++i) {
            acc += tris[i].area;
            cumulative[i] = acc;
        }
        std::mt19937 rng(1234);
        std::uniform_real_distribution<double> pick(0.0, acc);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        points->reserve(kPoints * 2);
        for (int k = 0; k < kPoints; ++k) {
            const std::size_t t = static_cast<std::size_t>(
                std::lower_bound(cumulative.begin(), cumulative.end(), pick(rng)) - cumulative.begin());
            const Tri& tri = tris[std::min(t, tris.size() - 1)];
            float u = unit(rng), w = unit(rng);
            if (u + w > 1.0f) {
                u = 1.0f - u;
                w = 1.0f - w;
            }
            const Vec3 p = tri.a + (tri.b - tri.a) * u + (tri.c - tri.a) * w;
            points->push_back(Vec4{p, 1.0f});
            points->push_back(Vec4{tri.n, 0.0f});
        }
    }
    mesh_points_[model] = points;
    return points;
}

void VfxSystem::clear() {
    instances_.clear();
    ghosts_.clear();
    one_shots_.clear();
    decisions_.clear();
    for (std::vector<gfx::VfxEventRecord>& list : channel_events_) list.clear();
    // (Entrar o salir de Play, otra escena u otro proyecto.) Las texturas y
    // mallas que ya no use nadie salen de la GPU, y lo que no cabia o no se
    // pudo leer vuelve a probarse.
    output_mesh_failed_.clear();
    resource_warnings_.clear();
    if (renderer_ != nullptr) {
        renderer_->vfx().clear();
        renderer_->vfx().releaseUnused();
    }
}

void VfxSystem::shiftOrigin(const Vec3& offset) {
    if (renderer_ != nullptr) renderer_->vfx().shiftOrigin(offset);
    for (auto& [handle, inst] : instances_) {
        if (inst.has_last_position) inst.last_position = inst.last_position - offset;
    }
    // Las colas sin entidad en el espacio del emisor se mueven con el mundo.
    for (Ghost& ghost : ghosts_) {
        ghost.desc.transform.m[3][0] -= offset.x;
        ghost.desc.transform.m[3][1] -= offset.y;
        ghost.desc.transform.m[3][2] -= offset.z;
    }
    // Los efectos sueltos (playAt) y la camara, igual.
    for (OneShot& shot : one_shots_) {
        shot.transform.m[3][0] -= offset.x;
        shot.transform.m[3][1] -= offset.y;
        shot.transform.m[3][2] -= offset.z;
    }
    if (has_camera_) camera_position_ = camera_position_ - offset;
}

namespace {

std::shared_ptr<const gfx::VfxMeshData> buildOutputMesh(const assets::ModelAsset& asset) {
    // Todas las piezas juntas en el espacio del modelo (como meshPoints):
    // (posicion, u) (normal, v) por vertice.
    std::vector<core::Mat4> world(asset.nodes.size(), core::Mat4::identity());
    for (std::size_t i = 0; i < asset.nodes.size(); ++i) {
        const assets::ModelNode& n = asset.nodes[i];
        world[i] = n.parent >= 0 && static_cast<std::size_t>(n.parent) < i ? world[static_cast<std::size_t>(n.parent)] * n.local
                                                                            : n.local;
    }
    auto mesh = std::make_shared<gfx::VfxMeshData>();
    for (std::size_t i = 0; i < asset.nodes.size(); ++i) {
        const int part = asset.nodes[i].part;
        if (part < 0 || static_cast<std::size_t>(part) >= asset.parts.size() || !asset.parts[static_cast<std::size_t>(part)]) continue;
        const asset::ModelData& data = *asset.parts[static_cast<std::size_t>(part)];
        const core::Mat4& m = world[i];
        const auto first = static_cast<std::uint32_t>(mesh->vertices.size() / 2);
        if (first + data.vertices.size() > gfx::VfxPass::kMaxMeshVertices) break;  // el pase lo rechazaria entero
        for (const asset::SkinnedVertex& v : data.vertices) {
            const Vec4 p = m * Vec4{v.position, 1.0f};
            const Vec4 n4 = m * Vec4{v.normal, 0.0f};
            Vec3 n{n4.x, n4.y, n4.z};
            n = core::length(n) > 1e-8f ? core::normalize(n) : Vec3{0.0f, 1.0f, 0.0f};
            mesh->vertices.push_back(Vec4{p.x, p.y, p.z, v.uv.x});
            mesh->vertices.push_back(Vec4{n, v.uv.y});
        }
        for (const std::uint32_t index : data.indices) mesh->indices.push_back(first + index);
    }
    return mesh;
}

}  // namespace

int VfxSystem::outputMesh(const Uuid& model, gfx::VulkanRenderer& renderer) {
    if (!model.valid() || output_mesh_failed_.count(model) != 0) return -1;
    auto found = output_meshes_.find(model);
    if (found == output_meshes_.end()) {
        if (!model_provider_) return -1;
        const std::shared_ptr<const assets::ModelAsset> asset = model_provider_(model);
        if (!asset) return -1;  // aun cargando: se vuelve a intentar
        found = output_meshes_.emplace(model, buildOutputMesh(*asset)).first;
    }
    // El pase la sube la primera vez y despues solo la busca por la clave.
    const int slot = renderer.vfx().loadMesh(std::hash<Uuid>{}(model), *found->second);
    if (slot < 0) output_mesh_failed_[model] = true;  // no cabe: no se reintenta cada frame
    return slot;
}

void VfxSystem::resolveResources(const Uuid& graph_uuid, const VfxGraph& graph,
                                 std::shared_ptr<const std::vector<Vec4>>& points, Uuid& points_model,
                                 gfx::VfxInstanceDesc& desc, gfx::VulkanRenderer& renderer) {
    // Ranuras que el pase solto o dio a otra: se vuelven a pedir.
    syncResourceEpoch(renderer);
    std::string warning;
    // Textura (la carga el pase una vez por ruta). La guardada se marca en
    // uso: si no, el pase podria soltarla para otra.
    CachedGraph* cg = overrides_.count(graph_uuid) ? nullptr : cached(graph_uuid);
    const bool mesh_output = graph.output.type == gfx::VfxOutput::Mesh;
    const bool model_look = mesh_output && !graph.output.material.valid() && !graph.output.mesh_output_texture;
    if (!graph.output.texture.empty() && !model_look) {
        const std::filesystem::path file = assets_root_ / graph.output.texture;
        if (cg != nullptr && cg->texture_path == graph.output.texture && cg->texture >= 0) {
            desc.texture = cg->texture;
            renderer.vfx().touchTexture(desc.texture);
        } else {
            desc.texture = renderer.vfx().loadTexture(file);
            if (cg != nullptr) {
                cg->texture = desc.texture;
                cg->texture_path = graph.output.texture;
            }
        }
        if (desc.texture < 0) warning = "Sin textura: " + graph.output.texture + " (ver la consola)";
    }
    // Malla de la salida Malla, sus variantes y su material (.crmat).
    if (mesh_output) {
        desc.mesh = outputMesh(graph.output.mesh.uuid, renderer);
        if (desc.mesh < 0 && output_mesh_failed_.count(graph.output.mesh.uuid) != 0) {
            warning = "La malla no cabe en el pase: esas partículas no se dibujan (ver la consola)";
        }
        const std::size_t variants = std::min<std::size_t>(graph.output.mesh_variants.size(), desc.mesh_variants.size());
        for (std::size_t i = 0; i < variants; ++i) {
            const assets::AssetRef& v = graph.output.mesh_variants[i];
            desc.mesh_variants[i] = v.valid() ? outputMesh(v.uuid, renderer) : -1;  // -1: la principal
        }
        if (graph.output.material.valid()) {
            VfxMaterial& material = vfxMaterial(graph.output.material.uuid);
            applyVfxMaterial(material, desc);
            // Su textura base (una vez; sin ella, la de la salida).
            if (material.valid && !material.albedo.empty()) {
                if (material.texture == -2) {
                    material.texture = renderer.vfx().loadTexture(assets_root_ / material.albedo);
                } else {
                    renderer.vfx().touchTexture(material.texture);
                }
                if (material.texture >= 0) desc.texture = material.texture;
                else warning = "Sin la textura del material: " + material.albedo + " (ver la consola)";
            }
        } else if (model_look) {
            // Sin .crmat: el color y la textura base del propio modelo, con su
            // UV (antes, la textura de la salida: la de un sprite, con alfa ~0
            // en casi toda la hoja, y las mallas no se veian). Sin textura en
            // disco, solo su color (sin recorte por alfa).
            desc.texture = -1;
            ModelLook& look = modelLook(graph.output.mesh.uuid);
            if (look.ready) {
                desc.tint = look.tint;
                if (look.texture == -2) {
                    look.texture = look.file.empty() ? -1 : renderer.vfx().loadTexture(look.file);
                } else {
                    renderer.vfx().touchTexture(look.texture);
                }
                desc.texture = look.texture;
                if (look.embedded && warning.empty()) {
                    warning = "La textura del modelo va dentro del archivo: la malla usa solo su color (pon un .crmat)";
                }
            }
        }
    }
    if (warning.empty()) {
        if (!resource_warnings_.empty()) resource_warnings_.erase(graph_uuid);
    } else {
        resource_warnings_[graph_uuid] = std::move(warning);
    }
    // Superficie de malla.
    for (const Block& b : graph.initialize) {
        if (b.enabled && b.kind == BlockKind::PositionMesh && b.mesh.valid()) {
            if (!points || points_model != b.mesh.uuid) {
                points = meshPoints(b.mesh.uuid);
                points_model = b.mesh.uuid;
            }
            desc.points = points;
            break;
        }
    }
}

void VfxSystem::update(ecs::World& world, float delta_seconds, bool simulate, const std::vector<entt::entity>& preview,
                       gfx::VulkanRenderer& renderer) {
    renderer_ = &renderer;
    // Mezcla Distorsion: la cvar (la calidad la pone el preset de graficos).
    renderer.vfx().setDistortionEnabled(g_vfx_distortion.get());
    for (auto& [handle, inst] : instances_) inst.seen = false;
    std::vector<gfx::VfxInstanceDesc> descs;
    VfxSystemStats stats;
    // Escalabilidad: la tabla de la calidad y, con efectos con tipo o caja
    // fija en Play, que corre, que se oculta y que sobra (planFrame).
    type_ids_.clear();
    planFrame(world, simulate, preview);
    renderer.vfx().setBakedCurlNoise(g_vfx_curl_baked.get());
    for (const entt::entity handle : world.registry().view<VisualEffect>()) {
        const ecs::Entity e = world.wrap(handle);
        if (!e.activeInHierarchy()) continue;
        const VisualEffect& component = e.get<VisualEffect>();
        if (!simulate) {
            if (!component.preview_in_editor) continue;
            if (std::find(preview.begin(), preview.end(), handle) == preview.end()) continue;
        }
        if (!component.graph.valid()) continue;
        const VfxGraph* g = graph(component.graph.uuid);
        if (g == nullptr) continue;

        Instance& inst = instanceOf(e);
        inst.seen = true;
        if (inst.graph != component.graph.uuid) {
            inst.graph = component.graph.uuid;
            inst.reset = true;
            inst.spawn = SpawnState{};
            inst.points.reset();
            inst.has_last_position = false;
        }
        if (!inst.started) {
            inst.started = true;
            inst.playing = simulate ? component.play_on_awake : true;
            inst.seed = component.seed != 0 ? static_cast<std::uint32_t>(component.seed)
                                             : static_cast<std::uint32_t>(std::random_device{}() | 1u);
            inst.random.seed(inst.seed);
            inst.reset = true;
            inst.spawn = SpawnState{};
            inst.time = 0.0f;
        }
        // Escalabilidad: lo que decidio planFrame (Run si no hay nada que decidir).
        const BudgetCandidate::Decision decision = decisionFor(handle);
        const EffectType type = effectiveType(handle, *g);  // Heroe si es del jugador local
        const auto type_index = static_cast<std::size_t>(typeSlot(type));
        TypeTimer type_timer(stats.types[type_index].cpu_ms);
        inst.budget_running = decision == BudgetCandidate::Decision::Run || decision == BudgetCandidate::Decision::Hidden;
        if (decision == BudgetCandidate::Decision::Finished) {
            // Ya termino (sin emision pendiente ni particulas vivas): ni
            // cuenta ni se simula; su ranura de la GPU queda libre. play(),
            // un evento o un ciclo nuevo lo vuelven a empezar limpio.
            inst.reset = true;
            inst.has_last_position = false;
            continue;
        }
        if (decision != BudgetCandidate::Decision::Run) ++stats.types[type_index].culled;
        if (decision == BudgetCandidate::Decision::Removed) {
            // Fuera (distancia o presupuesto): sin ranura en la GPU; si vuelve,
            // sus particulas empiezan limpias.
            inst.reset = true;
            inst.has_last_position = false;
            continue;
        }
        // Congelado: en pausa por el presupuesto, o fuera de la vista con bucle
        // (sin bucle sigue hasta terminar, sin dibujarse).
        const bool frozen = decision == BudgetCandidate::Decision::Paused ||
                            (decision == BudgetCandidate::Decision::Hidden && g->loop);
        const ParamValues params = paramsFor(*g, component, inst);
        // El tiempo del juego (timeScale: pausa y camara lenta) va en dt: la
        // simulacion y el flipbook (por la edad) lo siguen.
        const float dt = inst.paused || frozen ? 0.0f
                                               : std::max(delta_seconds, 0.0f) * timeScale() *
                                                     std::max(component.simulation_speed, 0.0f);
        const Vec3 position = e.worldPosition();
        float moved = inst.has_last_position ? core::length(position - inst.last_position) : 0.0f;
        Vec3 velocity = inst.has_last_position && delta_seconds > 1e-5f
                            ? (position - inst.last_position) * (1.0f / delta_seconds)
                            : Vec3{};
        // Un salto (teletransporte, un objeto del pool que se recoloca): ni
        // caudal por distancia ni velocidad (las que nacen interpoladas se
        // repartirian por todo el salto).
        constexpr float kTeleportMeters = 20.0f;
        if (moved > kTeleportMeters) {
            moved = 0.0f;
            velocity = Vec3{};
        }
        inst.last_position = position;
        inst.has_last_position = true;
        inst.distance += moved;

        // Al empezar: el precalentado (aproximado), repartido en unos frames
        // (vfx.prewarmFrames; de golpe daba un tiron). Congelado no nace nada
        // (los eventos esperan).
        const bool first = inst.reset;
        std::uint32_t spawn = 0;
        if (!frozen) {
            spawn = advanceSpawn(*g, params, inst.spawn, dt, moved, inst.playing, inst.random);
            std::uint32_t warm = 0;
            if (first && g->prewarm > 0.0f && inst.playing) {
                SpawnState warm_state = inst.spawn;
                warm = advanceSpawn(*g, params, warm_state, g->prewarm, 0.0f, true, inst.random);
            }
            // Escala de particulas del tipo y rafagas repartidas.
            spawn = scaledSpawn(*g, type, inst, spawn, first, warm);
        }
        inst.time += dt;
        // Tiempo sin nacer nada (para saber si ya termino: planFrame).
        inst.quiet = first || spawn > 0 ? 0.0f : inst.quiet + dt;

        gfx::VfxInstanceDesc desc;
        compileGraph(*g, params, desc);
        resolveChannels(*g, desc);
        desc.id = inst.id;
        desc.transform = e.worldMatrix();
        desc.reset = inst.reset;
        desc.spawn_count = std::min(spawn, desc.capacity);
        desc.delta_seconds = dt;
        desc.time = inst.time;
        desc.seed = inst.seed;
        desc.emitter_velocity = velocity;
        // Metros del emisor en repeticiones de la textura de la cinta (en
        // double aqui; al pase le basta con la vuelta de 1024).
        desc.emitter_distance =
            static_cast<float>(std::fmod(inst.distance, static_cast<double>(desc.ribbon_tile) * 1024.0));
        // Orden de dibujo, salidas apagadas, capacidad y visible (tipo y calidad).
        applyTypeLimits(*g, type, decision, desc);
        type_ids_.emplace_back(inst.id, static_cast<int>(type_index));
        resolveResources(component.graph.uuid, *g, inst.points, inst.points_model, desc, renderer);
        // Cintas: lo justo para dejar la cola apagandose si la entidad
        // desaparece (los demas efectos no guardan nada).
        inst.ribbon = g->output.type == gfx::VfxOutput::Ribbon;
        if (inst.ribbon) {
            inst.last_transform = desc.transform;
            inst.last_params = params;
        }

        // --- Subemisores: un efecto por cada .crvfx de sus bloques Trigger ---
        // Solo nacen por los eventos de la GPU (sin Spawn propio), en el mundo
        // y con el emisor del padre (atractores y vortices del hijo, en su
        // espacio). Sin nietos: los Trigger de un subemisor no apuntan nada.
        trigger_scratch_.clear();
        for (const Block& b : g->update) {
            if (b.enabled && (b.kind == BlockKind::TriggerOnDie || b.kind == BlockKind::TriggerOnCollide) && b.effect.valid()) {
                trigger_scratch_.push_back(b.effect.uuid);  // el mismo orden que triggerTargets / compileGraph
            }
        }
        for (Instance::SubEmitter& child : inst.children) child.seen = false;
        desc.event_targets.clear();
        for (const Uuid& target : trigger_scratch_) {
            auto child = std::find_if(inst.children.begin(), inst.children.end(),
                                      [&](const Instance::SubEmitter& c) { return c.graph == target; });
            if (child == inst.children.end()) {
                Instance::SubEmitter fresh;
                fresh.graph = target;
                fresh.id = next_id_++;
                inst.children.push_back(fresh);
                child = std::prev(inst.children.end());
            }
            desc.event_targets.push_back(child->id);
            if (child->seen) continue;  // el mismo .crvfx en otro Trigger: una sola instancia
            child->seen = true;
            const VfxGraph* cg = target == component.graph.uuid ? nullptr : graph(target);
            if (cg == nullptr) continue;  // no existe (o es el mismo efecto): sus eventos no van a ningun sitio
            ParamValues child_params;
            for (const ExposedParam& p : cg->params) child_params[p.name] = p.value;
            gfx::VfxInstanceDesc cd;
            compileGraph(*cg, child_params, cd);
            resolveChannels(*cg, cd);
            cd.id = child->id;
            cd.transform = desc.transform;
            cd.world_space = true;
            cd.reset = child->reset || desc.reset;
            cd.spawn_count = 0;
            cd.delta_seconds = dt;
            cd.time = inst.time;
            cd.seed = inst.seed * 2654435761u + static_cast<std::uint32_t>(child->id);
            cd.interpolate_spawn = false;
            cd.event_target = true;
            // Como el padre: su orden, si se ve y lo que la calidad apaga.
            cd.sort_order = desc.sort_order;
            cd.visible = desc.visible;
            cd.disabled_outputs = desc.disabled_outputs;
            type_ids_.emplace_back(cd.id, static_cast<int>(type_index));
            resolveResources(target, *cg, child->points, child->points_model, cd, renderer);
            child->reset = false;
            stats.capacity += cd.capacity;
            ++stats.effects;
            descs.push_back(std::move(cd));
        }
        std::erase_if(inst.children, [](const Instance::SubEmitter& c) { return !c.seen; });

        inst.reset = false;
        stats.capacity += desc.capacity;
        ++stats.effects;
        descs.push_back(std::move(desc));
    }
    for (auto it = instances_.begin(); it != instances_.end();) {
        if (!it->second.seen) {
            // Una cinta que se queda sin entidad (el proyectil impacto): su
            // cola sigue sin nacer nada hasta que muere su ultimo punto.
            const Instance& gone = it->second;
            const VfxGraph* g = simulate && gone.ribbon && !gone.reset ? graph(gone.graph) : nullptr;
            if (g != nullptr && g->output.type == gfx::VfxOutput::Ribbon) {
                Ghost ghost;
                compileGraph(*g, gone.last_params, ghost.desc);
                resolveChannels(*g, ghost.desc);
                ghost.desc.listens.clear();  // la cola sigue apuntando, pero ya no escucha
                ghost.desc.id = gone.id;
                ghost.desc.transform = gone.last_transform;
                ghost.desc.seed = gone.seed;
                ghost.desc.time = gone.time;
                ghost.desc.interpolate_spawn = false;
                std::shared_ptr<const std::vector<Vec4>> points = gone.points;
                Uuid points_model = gone.points_model;
                resolveResources(gone.graph, *g, points, points_model, ghost.desc, renderer);
                // La vida mas larga de sus puntos (1 s sin bloque Vida).
                float longest = 0.0f;
                bool has_lifetime = false;
                for (const Block& b : g->initialize) {
                    if (!b.enabled || b.kind != BlockKind::Lifetime) continue;
                    has_lifetime = true;
                    longest = std::max({longest, blockValue(b, 0, gone.last_params), blockValue(b, 1, gone.last_params)});
                }
                ghost.remaining = (has_lifetime ? longest : 1.0f) + 0.1f;
                ghosts_.push_back(std::move(ghost));
            }
            it = instances_.erase(it);
        } else {
            ++it;
        }
    }
    // Las colas sin entidad: sin nacer, hasta que muere su ultimo punto.
    for (auto it = ghosts_.begin(); it != ghosts_.end();) {
        it->remaining -= std::max(delta_seconds, 0.0f);
        if (!simulate || it->remaining <= 0.0f) {
            it = ghosts_.erase(it);
            continue;
        }
        it->desc.time += std::max(delta_seconds, 0.0f);
        gfx::VfxInstanceDesc d = it->desc;
        d.reset = false;
        d.spawn_count = 0;
        d.delta_seconds = std::max(delta_seconds, 0.0f);
        d.event_targets.clear();  // sus subemisores ya no existen
        stats.capacity += d.capacity;
        ++stats.effects;
        descs.push_back(std::move(d));
        ++it;
    }
    // Eventos de los scripts a sus canales (antes de setInstances: si nadie
    // escucha, el pase los tira).
    for (std::size_t c = 0; c < channel_events_.size(); ++c) {
        std::vector<gfx::VfxEventRecord>& list = channel_events_[c];
        if (list.empty()) continue;
        renderer.vfx().pushEvents(static_cast<std::uint32_t>(c), list.data(), list.size());
        list.clear();
    }
    // Efectos sueltos (playAt): el pool de instancias.
    updateOneShots(delta_seconds, descs, stats, renderer);
    // Salida Luz: el tope de luces de particula (antes de setInstances: con
    // 0, ningun efecto apunta luces).
    {
        static constexpr std::uint32_t kLightCaps[] = {8u, 16u, 32u, 32u};
        renderer.vfx().setLightQualityCap(kLightCaps[std::clamp(qualityLevel(), 0, 3)]);
        renderer.vfx().setLightBudget(static_cast<std::uint32_t>(std::max(g_vfx_particle_lights.get(), 0)));
    }
    // Suelo bajo cada emisor para los bloques que lo piden (sin la vista).
    for (gfx::VfxInstanceDesc& d : descs) applyGround(d, ground_query_);
    renderer.vfx().setInstances(std::move(descs));
    const gfx::VfxStats gpu = renderer.vfx().stats();
    stats.alive = gpu.alive;
    stats.pool = gpu.pool;
    stats.memory_bytes = gpu.memory_bytes;
    finishStats(stats, renderer);
    stats_ = stats;
}

void VfxSystem::play(ecs::Entity entity) {
    if (!entity.valid()) return;
    Instance& inst = instanceOf(entity);
    inst.playing = true;
    inst.paused = false;
    inst.started = true;
    inst.reset = true;
    inst.spawn = SpawnState{};
    inst.time = 0.0f;
    // Reiniciar en otro sitio (pool) no es moverse: sin estela del salto.
    inst.has_last_position = false;
    if (inst.seed == 0) inst.seed = 1;
}

void VfxSystem::stop(ecs::Entity entity, bool clear_particles) {
    if (!entity.valid()) return;
    Instance& inst = instanceOf(entity);
    inst.started = true;
    inst.playing = false;
    if (clear_particles) {
        inst.reset = true;  // tambien los subemisores (cd.reset)
    }
}

void VfxSystem::pause(ecs::Entity entity, bool paused) {
    if (!entity.valid()) return;
    instanceOf(entity).paused = paused;
}

bool VfxSystem::isPlaying(ecs::Entity entity) const {
    const Instance* inst = findInstance(entity);
    return inst != nullptr && inst->playing && !inst->paused && !inst->spawn.finished;
}

void VfxSystem::sendEvent(ecs::Entity entity, const std::string& event) {
    if (!entity.valid()) return;
    instanceOf(entity).spawn.pending_events.push_back(event);
}

void VfxSystem::setFloat(ecs::Entity entity, const std::string& name, float value) {
    if (!entity.valid()) return;
    instanceOf(entity).script_params[name] = Vec4{value, 0.0f, 0.0f, 0.0f};
}

void VfxSystem::setVector(ecs::Entity entity, const std::string& name, const Vec3& value) {
    if (!entity.valid()) return;
    instanceOf(entity).script_params[name] = Vec4{value, 0.0f};
}

void VfxSystem::setColor(ecs::Entity entity, const std::string& name, const Vec4& value) {
    if (!entity.valid()) return;
    instanceOf(entity).script_params[name] = value;
}

void VfxSystem::setBool(ecs::Entity entity, const std::string& name, bool value) {
    if (!entity.valid()) return;
    instanceOf(entity).script_params[name] = Vec4{value ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
}

bool VfxSystem::getParam(ecs::Entity entity, const std::string& name, Vec4& value) {
    if (!entity.valid() || !entity.has<VisualEffect>()) return false;
    const VisualEffect& component = entity.get<VisualEffect>();
    const VfxGraph* g = graph(component.graph.uuid);
    if (g == nullptr || g->findParam(name) == nullptr) return false;
    const Instance* inst = findInstance(entity);
    const ParamValues values = paramsFor(*g, component, inst != nullptr ? *inst : Instance{});
    const auto it = values.find(name);
    if (it == values.end()) return false;
    value = it->second;
    return true;
}

std::uint32_t VfxSystem::aliveCount(ecs::Entity entity) const {
    const Instance* inst = findInstance(entity);
    if (inst == nullptr || renderer_ == nullptr) return 0;
    return renderer_->vfx().aliveCount(inst->id);
}

// --- Canales de datos ----------------------------------------------------------------

std::uint32_t VfxSystem::channelId(const std::string& channel) {
    if (channel.empty()) return gfx::VfxPass::kNoChannel;
    const auto found = channels_.find(channel);
    if (found != channels_.end()) return found->second;
    if (channels_.size() >= gfx::VfxPass::kMaxChannels) {
        static bool warned = false;
        if (!warned) {
            std::cerr << "[VFX] Demasiados canales de datos (maximo " << gfx::VfxPass::kMaxChannels << "): '" << channel
                      << "' no se usa\n";
            warned = true;
        }
        return gfx::VfxPass::kNoChannel;
    }
    const auto id = static_cast<std::uint32_t>(channels_.size());
    channels_.emplace(channel, id);
    return id;
}

void VfxSystem::resolveChannels(const VfxGraph& graph, gfx::VfxInstanceDesc& desc) {
    bool any = !desc.listens.empty();
    for (const gfx::VfxBlock& b : desc.update) any = any || gfx::vfxWritesChannel(b.type);
    if (!any) return;  // sin bloques de canales: nada que buscar
    const std::vector<std::string> names = eventChannels(graph);
    const auto resolve = [&](gfx::VfxBlock& b) {
        b.pad0 = b.pad1 < names.size() ? channelId(names[b.pad1]) : gfx::VfxPass::kNoChannel;
        b.pad1 = 0;
    };
    for (gfx::VfxBlock& b : desc.update) {
        if (gfx::vfxWritesChannel(b.type)) resolve(b);
    }
    for (gfx::VfxBlock& b : desc.listens) resolve(b);
}

bool VfxSystem::pushGpuEvent(const std::string& channel, const gfx::VfxEventRecord& event) {
    const std::uint32_t id = channelId(channel);
    if (id >= gfx::VfxPass::kMaxChannels) return false;
    if (channel_events_.size() < gfx::VfxPass::kMaxChannels) channel_events_.resize(gfx::VfxPass::kMaxChannels);
    std::vector<gfx::VfxEventRecord>& list = channel_events_[id];
    // El tope por frame del canal (los de mas se pierden, como en la GPU).
    if (list.size() < gfx::VfxPass::kMaxChannelEvents) list.push_back(event);
    return true;
}

std::size_t VfxSystem::pendingGpuEvents() const {
    std::size_t count = 0;
    for (const std::vector<gfx::VfxEventRecord>& list : channel_events_) count += list.size();
    return count;
}

}  // namespace cramion::vfx

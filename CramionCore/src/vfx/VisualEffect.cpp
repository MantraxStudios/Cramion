#include "CramionCore/vfx/VisualEffect.h"

#include "CramionCore/asset/AssetManager.h"
#include "CramionCore/ecs/Reflection.h"

#include <CramionFX/asset/Model.h>
#include <CramionFX/vk/VulkanRenderer.h>

#include <nlohmann/json.hpp>

#include <algorithm>
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
        {fInt("min", "Mínimo", 0, 0.0f, 0.0f, 1024.0f), fInt("max", "Máximo", 1, 0.0f, 0.0f, 1024.0f)});
    add(BlockKind::InheritVelocity, Context::Initialize, "InheritVelocity", "Heredar velocidad",
        "Parte de la velocidad del emisor", {f1("factor", "Factor", 0, 1.0f, -10.0f, 10.0f)});
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
         f1("thickness", "Grosor", 4, 0.5f, 0.01f, 100.0f), fBool("kill", "Morir al chocar", 5, false)});
    add(BlockKind::CollidePlane, Context::Update, "CollidePlane", "Colisión con plano", "Un plano infinito",
        {fVec("normal", "Normal", 0, Vec3{0.0f, 1.0f, 0.0f}), f1("distance", "Distancia", 3, 0.0f, -10000.0f, 10000.0f),
         f1("bounce", "Rebote", 4, 0.4f, 0.0f, 1.0f), f1("friction", "Fricción", 5, 0.2f, 0.0f, 1.0f),
         f1("life_loss", "Vida perdida", 6, 0.0f, 0.0f, 1.0f)});
    add(BlockKind::CollideSphere, Context::Update, "CollideSphere", "Colisión con esfera", "Una esfera solida o hueca",
        {fVec("center", "Centro", 0, Vec3{}), f1("radius", "Radio", 3, 1.0f, 0.0f, 10000.0f),
         f1("bounce", "Rebote", 4, 0.4f, 0.0f, 1.0f), f1("friction", "Fricción", 5, 0.2f, 0.0f, 1.0f),
         fBool("inside", "Por dentro", 6, false)});
    add(BlockKind::KillBox, Context::Update, "KillBox", "Matar en caja", "Mueren dentro (o fuera) de una caja",
        {fVec("center", "Centro", 0, Vec3{}), fVec("size", "Tamaño", 3, Vec3{10.0f, 10.0f, 10.0f}),
         fBool("outside", "Matar fuera", 6, true)});
    add(BlockKind::SpeedLimit, Context::Update, "SpeedLimit", "Velocidad máxima", "Limita la rapidez",
        {f1("max", "Máxima", 0, 10.0f, 0.0f, 10000.0f), f1("damping", "Amortiguación", 1, 1.0f, 0.0f, 1.0f)});
    add(BlockKind::ConformSphere, Context::Update, "ConformSphere", "Conformar a esfera", "Se pegan a una esfera",
        {fVec("center", "Centro", 0, Vec3{}), f1("radius", "Radio", 3, 1.0f, 0.0f, 10000.0f),
         f1("attraction", "Atracción", 4, 5.0f, 0.0f, 1000.0f), f1("stickiness", "Pegajosidad", 5, 0.5f, 0.0f, 1.0f)});
    // Output
    add(BlockKind::ColorOverLife, Context::Output, "ColorOverLife", "Color en la vida", "Multiplica el color a lo largo de la vida", {})
        .has_gradient = true;
    add(BlockKind::SizeOverLife, Context::Output, "SizeOverLife", "Tamaño en la vida", "Multiplica el tamano a lo largo de la vida", {})
        .has_curve = true;
    add(BlockKind::ColorBySpeed, Context::Output, "ColorBySpeed", "Color por velocidad", "Del color lento al rapido",
        {fColor("slow", "Lento", 0, Vec4{1.0f, 1.0f, 1.0f, 1.0f}), fColor("fast", "Rápido", 4, Vec4{1.0f, 0.4f, 0.1f, 1.0f}),
         f1("min", "Rapidez mín.", 8, 0.0f, 0.0f, 10000.0f), f1("max", "Rapidez máx.", 9, 10.0f, 0.0f, 10000.0f)});
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
const char* kBlendKeys[] = {"Additive", "Alpha", "Premultiplied"};
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
        if (info->has_text) b.text = "Event";
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
                      {"blend", kBlendKeys[std::clamp(static_cast<int>(o.blend), 0, 2)]},
                      {"intensity", o.intensity},
                      {"soft_distance", o.soft_distance},
                      {"stretch", o.stretch},
                      {"alpha_clip", o.alpha_clip},
                      {"lit", o.lit},
                      {"texture", o.texture},
                      {"flip_cols", o.flip_cols},
                      {"flip_rows", o.flip_rows},
                      {"flip_fps", o.flip_fps}};
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
                              "Magia", "Luciérnagas", "Nieve", "Lluvia", "Estela"};

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
                g.a = Vec4{v(b, 0), std::max(v(b, 1), v(b, 0)), 0.0f, 0.0f};
                break;
            case BlockKind::InheritVelocity:
                g = gpuBlock(T::InheritVelocity);
                g.a = Vec4{v(b, 0), 0.0f, 0.0f, 0.0f};
                break;
            default: continue;
        }
        out.initialize.push_back(g);
    }

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
                break;
            case BlockKind::CollidePlane: {
                g = gpuBlock(T::CollidePlane);
                Vec3 n = v3(b, 0);
                n = core::length(n) > 1e-5f ? core::normalize(n) : Vec3{0.0f, 1.0f, 0.0f};
                g.a = Vec4{n, v(b, 3)};
                g.b = Vec4{v(b, 4), v(b, 5), v(b, 6), 0.0f};
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
                g.flags = v(b, 6) > 0.5f ? 1u : 0u;
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
            default: continue;
        }
        out.update.push_back(g);
    }

    out.color_over_life = false;
    out.size_over_life = false;
    out.color_by_speed = false;
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
    out.world_space = graph.world_space;
    out.capacity = static_cast<std::uint32_t>(std::clamp(graph.capacity, 1, static_cast<int>(gfx::VfxPass::kMaxCapacity)));
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

// --- Componente ----------------------------------------------------------------------

void VisualEffect::reflect(ecs::PropertyVisitor& v) {
    v.asset({"graph", "Efecto (.crvfx)", "El asset del VFX Graph (Crear > Efecto visual)"}, graph, assets::AssetType::VisualEffect);
    v.field({"play_on_awake", "Reproducir al empezar"}, play_on_awake);
    v.field({"seed", "Semilla", "0 = al azar cada vez"}, seed, 0, 1000000);
    v.field({"simulation_speed", "Velocidad", "Multiplica el tiempo de la simulacion"}, simulation_speed,
            ecs::FloatRange{0.0f, 10.0f, 0.01f, "%.2fx"});
    v.field({"preview_in_editor", "Vista previa en el editor", "Simular en la escena si esta seleccionado"}, preview_in_editor);
    ecs::listField(v, {"params", "Parámetros", "Valores de los parametros expuestos del efecto para este objeto"}, params,
                   [](ParamOverride& p, ecs::PropertyVisitor& pv) {
                       pv.field({"name", "Nombre"}, p.name);
                       pv.field({"value", "Valor", "Float = X; Color = RGB (y A abajo)"}, p.value, ecs::Vec3Kind::Position);
                       pv.field({"w", "Alfa / W"}, p.w, ecs::FloatRange{0.0f, 1.0f, 0.01f, "%.2f"});
                   });
}

void registerVfxComponents() {
    ecs::ComponentRegistry& registry = ecs::ComponentRegistry::instance();
    if (registry.find("VisualEffect") == nullptr) {
        registry.registerComponent<VisualEffect>("VisualEffect", "Visual Effect (VFX Graph)", "Efectos");
    }
}

VfxSystem* activeSystem() { return g_active; }
void setActiveSystem(VfxSystem* system) { g_active = system; }

// --- Sistema -------------------------------------------------------------------------

void VfxSystem::setGraphOverride(const Uuid& uuid, const VfxGraph& graph) { overrides_[uuid] = graph; }

void VfxSystem::clearGraphOverride(const Uuid& uuid) { overrides_.erase(uuid); }

void VfxSystem::reloadGraphs() {
    graphs_.clear();
    mesh_points_.clear();
    for (auto& [handle, inst] : instances_) inst.points.reset();
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
    if (renderer_ != nullptr) renderer_->vfx().clear();
}

void VfxSystem::shiftOrigin(const Vec3& offset) {
    if (renderer_ != nullptr) renderer_->vfx().shiftOrigin(offset);
    for (auto& [handle, inst] : instances_) {
        if (inst.has_last_position) inst.last_position = inst.last_position - offset;
    }
}

void VfxSystem::update(ecs::World& world, float delta_seconds, bool simulate, const std::vector<entt::entity>& preview,
                       gfx::VulkanRenderer& renderer) {
    renderer_ = &renderer;
    for (auto& [handle, inst] : instances_) inst.seen = false;
    std::vector<gfx::VfxInstanceDesc> descs;
    VfxSystemStats stats;
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
        const ParamValues params = paramsFor(*g, component, inst);
        const float dt = inst.paused ? 0.0f : std::max(delta_seconds, 0.0f) * std::max(component.simulation_speed, 0.0f);
        const Vec3 position = e.worldPosition();
        const float moved = inst.has_last_position ? core::length(position - inst.last_position) : 0.0f;
        const Vec3 velocity = inst.has_last_position && delta_seconds > 1e-5f
                                  ? (position - inst.last_position) * (1.0f / delta_seconds)
                                  : Vec3{};
        inst.last_position = position;
        inst.has_last_position = true;

        // Al empezar: el precalentado (aproximado) nace de golpe.
        const bool first = inst.reset;
        std::uint32_t spawn = advanceSpawn(*g, params, inst.spawn, dt, moved, inst.playing, inst.random);
        if (first && g->prewarm > 0.0f && inst.playing) {
            SpawnState warm = inst.spawn;
            spawn += advanceSpawn(*g, params, warm, g->prewarm, 0.0f, true, inst.random);
        }
        inst.time += dt;

        gfx::VfxInstanceDesc desc;
        compileGraph(*g, params, desc);
        desc.id = inst.id;
        desc.transform = e.worldMatrix();
        desc.reset = inst.reset;
        desc.spawn_count = std::min(spawn, desc.capacity);
        desc.delta_seconds = dt;
        desc.time = inst.time;
        desc.seed = inst.seed;
        desc.emitter_velocity = velocity;
        // Textura (la carga el pase una vez por ruta).
        CachedGraph* cg = overrides_.count(component.graph.uuid) ? nullptr : cached(component.graph.uuid);
        if (!g->output.texture.empty()) {
            const std::filesystem::path file = assets_root_ / g->output.texture;
            if (cg != nullptr && cg->texture_path == g->output.texture && cg->texture >= 0) {
                desc.texture = cg->texture;
            } else {
                desc.texture = renderer.vfx().loadTexture(file);
                if (cg != nullptr) {
                    cg->texture = desc.texture;
                    cg->texture_path = g->output.texture;
                }
            }
        }
        // Superficie de malla.
        for (const Block& b : g->initialize) {
            if (b.enabled && b.kind == BlockKind::PositionMesh && b.mesh.valid()) {
                if (!inst.points || inst.points_model != b.mesh.uuid) {
                    inst.points = meshPoints(b.mesh.uuid);
                    inst.points_model = b.mesh.uuid;
                }
                desc.points = inst.points;
                break;
            }
        }
        inst.reset = false;
        stats.capacity += desc.capacity;
        ++stats.effects;
        descs.push_back(std::move(desc));
    }
    for (auto it = instances_.begin(); it != instances_.end();) {
        if (!it->second.seen) it = instances_.erase(it);
        else ++it;
    }
    renderer.vfx().setInstances(std::move(descs));
    const gfx::VfxStats gpu = renderer.vfx().stats();
    stats.alive = gpu.alive;
    stats.pool = gpu.pool;
    stats.memory_bytes = gpu.memory_bytes;
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
    if (inst.seed == 0) inst.seed = 1;
}

void VfxSystem::stop(ecs::Entity entity, bool clear_particles) {
    if (!entity.valid()) return;
    Instance& inst = instanceOf(entity);
    inst.started = true;
    inst.playing = false;
    if (clear_particles) inst.reset = true;
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

}  // namespace cramion::vfx

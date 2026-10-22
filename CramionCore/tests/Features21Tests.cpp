// Pruebas de los sistemas de la 2.1 (consola, sin GPU): VFX Graph (Spawn y
// JSON), fisica 2D y tilemaps, localizacion, dialogos, partidas guardadas,
// Behavior Trees, Visual Scripting (compilar), Shader Graph (generar),
// fractura, horneado de sondas y repeticiones. Devuelve 0 si todo va.

#include "CramionCore/ai/BehaviorTree.h"
#include "CramionCore/asset/AssetDatabase.h"
#include "CramionCore/asset/AssetManager.h"
#include "CramionCore/asset/Importer.h"
#include "CramionCore/asset/ShaderGraph.h"
#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/gameplay/Dialogue.h"
#include "CramionCore/gameplay/Localization.h"
#include "CramionCore/gameplay/SaveGame.h"
#include "CramionCore/asset/BakedLightingFile.h"
#include "CramionCore/asset/LightmapUv.h"
#include "CramionCore/ecs/StaticBatching.h"
#include "CramionCore/lighting/BakeLights.h"
#include "CramionCore/lighting/BakeTracer.h"
#include "CramionCore/lighting/Lightmapper.h"
#include "CramionCore/lighting/ProbeBaker.h"
#include "CramionCore/physics/Fracture.h"
#include "CramionCore/physics/PhysicsComponents.h"
#include "CramionCore/replay/Replay.h"
#include "CramionCore/scripting/VisualScript.h"
#include "CramionCore/terrain/Terrain.h"
#include "CramionCore/twod/System2D.h"
#include "CramionCore/vfx/VisualEffect.h"
#include "CramionCore/cvar/CVar.h"

#include <CramionFX/asset/TextureStats.h>
#include <CramionFX/scene/Camera.h>
#include <CramionFX/scene/Scene.h>
#include <CramionFX/scene/ShadowCascades.h>

// El filtro del lightmapper (interno de CramionCore) para probarlo suelto.
#include "../src/lighting/LightmapperInternal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <string>

using namespace cramion;
using core::Vec3;

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const char* what) {
    ++checks;
    std::printf("  %s %s\n", condition ? "OK   " : "FALLO", what);
    if (!condition) ++failures;
}

void testVfx() {
    std::printf("VFX Graph\n");
    vfx::VfxGraph g;
    g.loop = true;
    g.duration = 2.0f;
    g.spawn.push_back(vfx::makeBlock(vfx::BlockKind::ConstantRate));
    g.spawn.back().values[0] = 10.0f;
    vfx::Block burst = vfx::makeBlock(vfx::BlockKind::Burst);
    burst.values[0] = 50.0f;
    burst.values[1] = 0.5f;
    g.spawn.push_back(burst);
    vfx::SpawnState state;
    std::mt19937 rng(1);
    std::uint32_t total = 0;
    for (int i = 0; i < 60; ++i) total += vfx::advanceSpawn(g, {}, state, 1.0f / 60.0f, 0.0f, true, rng);
    check(total >= 59 && total <= 61, "1 s: 10 por caudal + 50 de la rafaga");
    // Otro ciclo: la rafaga vuelve.
    for (int i = 0; i < 120; ++i) total += vfx::advanceSpawn(g, {}, state, 1.0f / 60.0f, 0.0f, true, rng);
    std::printf("  (3 s: %u particulas)\n", total);
    check(total >= 128 && total <= 132, "3 s: 30 por caudal + 2 rafagas (el bucle la repite)");
    // Parametro atado.
    g.params.push_back(vfx::ExposedParam{"Rate", vfx::ParamType::Float, core::Vec4{100.0f, 0, 0, 0}, {}});
    g.spawn[0].bindings[0] = "Rate";
    vfx::ParamValues params;
    params["Rate"] = core::Vec4{100.0f, 0, 0, 0};
    check(std::fabs(vfx::blockValue(g.spawn[0], 0, params) - 100.0f) < 1e-4f, "campo atado a un parametro");
    // JSON de todas las plantillas.
    bool all = true;
    for (int i = 0; i < vfx::vfxPresetCount(); ++i) {
        const vfx::VfxGraph preset = vfx::vfxPreset(i);
        vfx::VfxGraph back;
        if (!vfx::vfxGraphFromText(vfx::vfxGraphToText(preset), back) || back.initialize.size() != preset.initialize.size() ||
            back.spawn.size() != preset.spawn.size() || back.update.size() != preset.update.size() ||
            back.output_blocks.size() != preset.output_blocks.size()) {
            std::printf("  (plantilla %d: spawn %zu/%zu init %zu/%zu update %zu/%zu output %zu/%zu)\n", i, back.spawn.size(),
                        preset.spawn.size(), back.initialize.size(), preset.initialize.size(), back.update.size(),
                        preset.update.size(), back.output_blocks.size(), preset.output_blocks.size());
            all = false;
        }
    }
    check(all, "las plantillas se guardan y se leen igual");
    gfx::VfxInstanceDesc desc;
    vfx::compileGraph(vfx::vfxPreset(1), {}, desc);
    check(!desc.initialize.empty() && !desc.update.empty() && desc.color_over_life, "el fuego compila a bloques de la GPU");
}

// Luz por particula (bloque ParticleLight): .crvfx de antes sin luz, JSON de
// ida y vuelta, compilar con un parametro atado (setEffectFloat) y la
// eleccion de luces (VfxLightSelector: maximo por efecto, tope global, sin
// parpadeos con el tope lleno y con fundidos al cambiar).
void testVfxParticleLight() {
    std::printf("VFX Graph: luz por particula\n");
    {
        const char* text =
            R"({"version":1,"capacity":64,"spawn":[],"initialize":[],"update":[],)"
            R"("output_blocks":[{"type":"ColorOverLife","enabled":true,"values":{},"gradient":[[0,1,1,1,1],[1,1,1,1,0]]}],)"
            R"("output":{"blend":"Additive"}})";
        vfx::VfxGraph old;
        const bool ok = vfx::vfxGraphFromText(text, old);
        gfx::VfxInstanceDesc d;
        vfx::compileGraph(old, {}, d);
        check(ok && old.output_blocks.size() == 1 && !d.light, "un .crvfx de antes (sin el bloque) carga igual y no da luz");
    }
    vfx::VfxGraph g = vfx::vfxPreset(0);
    vfx::Block light = vfx::makeBlock(vfx::BlockKind::ParticleLight);
    check(light.values[0] == 1.0f && light.values[5] == 4.0f && light.values[7] == 0.0f,
          "por defecto: encendida, 4 luces por efecto, sin lo transparente");
    light.values[1] = 0.5f;  // alcance
    light.values[3] = 1.0f;  // caida lineal
    light.values[4] = 3.0f;  // exponente
    light.values[5] = 12.0f;
    light.values[6] = 1.0f;  // por distancia a la camara
    light.bindings[2] = "LightIntensity";
    g.params.push_back(vfx::ExposedParam{"LightIntensity", vfx::ParamType::Float, core::Vec4{8.0f, 0, 0, 0}, {}});
    g.output_blocks.push_back(light);
    vfx::VfxGraph back;
    const bool ok = vfx::vfxGraphFromText(vfx::vfxGraphToText(g), back);
    const vfx::Block* saved = nullptr;
    for (const vfx::Block& b : back.output_blocks) {
        if (b.kind == vfx::BlockKind::ParticleLight) saved = &b;
    }
    check(ok && saved != nullptr && std::fabs(saved->values[1] - 0.5f) < 1e-5f && saved->values[5] == 12.0f &&
              saved->bindings[2] == "LightIntensity",
          "el bloque Luz por particula se guarda y se lee igual");
    vfx::ParamValues params;
    params["LightIntensity"] = core::Vec4{3.0f, 0, 0, 0};
    gfx::VfxInstanceDesc desc;
    vfx::compileGraph(back, params, desc);
    check(desc.light && std::fabs(desc.light_intensity - 3.0f) < 1e-5f && std::fabs(desc.light_radius - 0.5f) < 1e-5f &&
              desc.light_falloff == gfx::VfxLightFalloff::Linear && std::fabs(desc.light_exponent - 3.0f) < 1e-5f &&
              desc.light_max == 12 && desc.light_priority == gfx::VfxLightPriority::CameraDistance && !desc.light_translucent,
          "compila la luz con la intensidad del parametro atado");
    params["LightIntensity"] = core::Vec4{0.0f, 0, 0, 0};
    vfx::compileGraph(back, params, desc);
    check(!desc.light, "intensidad 0 (desde un script): sin luz");

    // --- Eleccion ---
    const auto candidate = [](std::uint64_t key, std::uint32_t emitter, float x, float brightness) {
        gfx::VfxLightCandidate c;
        c.key = key;
        c.emitter = emitter;
        c.position = Vec3{x, 0.0f, 10.0f};
        c.radius = 2.0f;
        c.color = Vec3{brightness, brightness, brightness};
        return c;
    };
    const float dt = 1.0f / 60.0f;
    std::vector<gfx::VfxLight> out;
    {
        // 12 impactos cortos de un efecto (maximo 12, tope 32): entran todos de golpe.
        gfx::VfxLightSelector selector;
        std::vector<gfx::VfxLightCandidate> list;
        for (std::uint64_t k = 0; k < 12; ++k) list.push_back(candidate(k, 0, static_cast<float>(k), 1.0f));
        selector.select(list, {{12, gfx::VfxLightPriority::IntensityRadius}}, Vec3{}, 32, dt, out);
        bool full = out.size() == 12;
        for (const gfx::VfxLight& l : out) full = full && std::fabs(l.color.x - 1.0f) < 1e-5f;
        check(full, "12 impactos: las 12 luces de golpe (sin fundido de entrada)");
        // 40 vivas con luz y maximo 12: 12.
        for (std::uint64_t k = 12; k < 40; ++k) list.push_back(candidate(k, 0, static_cast<float>(k), 1.0f));
        selector.select(list, {{12, gfx::VfxLightPriority::IntensityRadius}}, Vec3{}, 32, dt, out);
        check(out.size() == 12, "el maximo por efecto");
    }
    {
        // Tope global 4 con 8 iguales de dos efectos: siempre las mismas 4.
        gfx::VfxLightSelector selector;
        std::vector<gfx::VfxLightCandidate> list;
        for (std::uint64_t k = 0; k < 8; ++k) list.push_back(candidate(100 + k, static_cast<std::uint32_t>(k % 2), 0.0f, 1.0f));
        const std::vector<gfx::VfxLightSelector::Emitter> emitters{{8, gfx::VfxLightPriority::IntensityRadius},
                                                                   {8, gfx::VfxLightPriority::IntensityRadius}};
        selector.select(list, emitters, Vec3{}, 4, dt, out);
        std::vector<float> first;
        for (const gfx::VfxLight& l : out) first.push_back(l.position.x + l.color.x);
        bool stable = out.size() == 4;
        for (int frame = 0; frame < 30 && stable; ++frame) {
            // Pequenos cambios de brillo (el alfa en la vida) no cambian cuales lucen.
            for (std::size_t i = 0; i < list.size(); ++i) {
                const std::size_t step = (static_cast<std::size_t>(frame) + i) % 3;
                list[i].color = Vec3{1.0f, 1.0f, 1.0f} * (1.0f + 0.2f * static_cast<float>(step));
            }
            selector.select(list, emitters, Vec3{}, 4, dt, out);
            stable = out.size() == 4 && selector.tracked() == 4;
        }
        check(stable, "tope lleno: las mismas luces cada frame (sin parpadeo)");
        // Una mucho mas fuerte: la que sale baja poco a poco y la nueva entra
        // subiendo; nunca pasan del tope.
        list.push_back(candidate(999, 0, 5.0f, 50.0f));
        bool never_over = true;
        bool faded_out = false;
        bool faded_in = false;
        bool arrived = false;
        for (int frame = 0; frame < 30; ++frame) {
            selector.select(list, emitters, Vec3{}, 4, dt, out);
            never_over = never_over && out.size() <= 4;
            for (const gfx::VfxLight& l : out) {
                const float b = l.color.x;
                if (l.position.x == 0.0f && b > 0.0f && b < 0.99f) faded_out = true;
                if (l.position.x == 5.0f && b > 0.0f && b < 49.0f) faded_in = true;
                if (l.position.x == 5.0f && b > 49.9f) arrived = true;
            }
        }
        check(never_over && faded_out && faded_in && arrived,
              "con el tope lleno, una nueva sustituye a otra con fundidos (sin saltos)");
    }
}

// Cinta (Ribbon): atributos por particula, cara, UV, suavizado, .crvfx de
// antes, empaquetado del id y la clave de la ordenacion, la plantilla de
// proyectil y el caudal por distancia que la alimenta.
void testVfxRibbon() {
    std::printf("VFX: cinta\n");
    const char* old_json = R"({"capacity":256,"output":{"type":"Ribbon","ribbon_uv":"Tile","ribbon_facing":"Emitter",
        "ribbon_tile":2.0},"initialize":[{"type":"FlipbookFrame","values":{"min":1,"max":3}}]})";
    vfx::VfxGraph old;
    const bool loaded = vfx::vfxGraphFromText(old_json, old);
    check(loaded && old.output.type == gfx::VfxOutput::Ribbon && old.output.ribbon_uv == gfx::VfxRibbonUv::Tile &&
              old.output.ribbon_facing == gfx::VfxRibbonFacing::Emitter && old.output.ribbon_subdivisions == 1 &&
              !old.output.ribbon_adaptive && old.output.ribbon_atlas_cell == -1 && old.output.ribbon_uv_scale.x == 1.0f &&
              old.output.ribbon_uv_offset.x == 0.0f && !old.output.flip_blend && old.initialize.size() == 1 &&
              old.initialize[0].values[2] == 0.0f,
          "un .crvfx de antes carga con los valores de siempre");
    gfx::VfxInstanceDesc od;
    vfx::compileGraph(old, {}, od);
    check(!od.ribbon_ids && od.ribbon_subdivisions == 1 && !od.flip_blend && od.initialize.size() == 1 &&
              od.initialize[0].a.z == 0.0f,
          "sin los bloques nuevos: sin ids, un tramo, sin variacion (coste cero)");

    vfx::VfxGraph g;
    g.output.type = gfx::VfxOutput::Ribbon;
    g.output.ribbon_facing = gfx::VfxRibbonFacing::CustomSide;
    g.output.ribbon_vector = Vec3{1.0f, 0.0f, 0.0f};
    g.output.ribbon_uv = gfx::VfxRibbonUv::Age;
    g.output.ribbon_tension = 0.25f;
    g.output.ribbon_subdivisions = 8;
    g.output.ribbon_adaptive = true;
    g.output.ribbon_atlas_cell = 3;
    g.output.ribbon_uv_scale = core::Vec2{2.0f, 1.0f};
    g.output.ribbon_uv_offset = core::Vec2{0.5f, 0.0f};
    vfx::Block attributes = vfx::makeBlock(vfx::BlockKind::RibbonAttributes);
    check(attributes.values[0] == 0.0f && attributes.values[1] == 1.0f && std::fabs(attributes.values[2] - 0.1f) < 1e-6f,
          "RibbonAttributes por defecto: id 0, una tira, 0.1 m de ancho");
    attributes.values[0] = 2.0f;   // id
    attributes.values[1] = 3.0f;   // tiras
    attributes.values[2] = 0.2f;   // ancho
    attributes.values[3] = 10.0f;  // giro min
    attributes.values[4] = 30.0f;  // giro max
    attributes.values[5] = 0.1f;   // separacion x
    g.initialize.push_back(attributes);
    vfx::VfxGraph back;
    const bool round_trip = vfx::vfxGraphFromText(vfx::vfxGraphToText(g), back);
    check(round_trip && back.output.ribbon_facing == gfx::VfxRibbonFacing::CustomSide && back.output.ribbon_vector.x == 1.0f &&
              back.output.ribbon_uv == gfx::VfxRibbonUv::Age && std::fabs(back.output.ribbon_tension - 0.25f) < 1e-6f &&
              back.output.ribbon_subdivisions == 8 && back.output.ribbon_adaptive && back.output.ribbon_atlas_cell == 3 &&
              back.output.ribbon_uv_scale.x == 2.0f && back.output.ribbon_uv_offset.x == 0.5f &&
              back.initialize.size() == 1 && back.initialize[0].kind == vfx::BlockKind::RibbonAttributes &&
              back.initialize[0].values[1] == 3.0f,
          "la cinta (cara, UV, suavizado, atlas, atributos) se guarda y se lee igual");
    vfx::VfxGraph distance;
    check(vfx::vfxGraphFromText(R"({"output":{"type":"Ribbon","ribbon_uv":"Distance"}})", distance) &&
              distance.output.ribbon_uv == gfx::VfxRibbonUv::Tile,
          "ribbon_uv \"Distance\" se lee como Tile (por distancia)");
    gfx::VfxInstanceDesc d;
    vfx::compileGraph(back, {}, d);
    const gfx::VfxBlock* ribbon_block = nullptr;
    for (const gfx::VfxBlock& b : d.initialize) {
        if (b.type == static_cast<std::uint32_t>(gfx::VfxBlockType::RibbonAttributes)) ribbon_block = &b;
    }
    check(ribbon_block != nullptr && ribbon_block->a.x == 2.0f && ribbon_block->a.y == 3.0f &&
              std::fabs(ribbon_block->a.z - 0.2f) < 1e-6f && ribbon_block->a.w == 10.0f && ribbon_block->b.x == 30.0f &&
              std::fabs(ribbon_block->b.y - 0.1f) < 1e-6f && d.ribbon_ids && d.ribbon_subdivisions == 8 && d.ribbon_adaptive &&
              d.ribbon_atlas_cell == 3 && d.ribbon_uv_transform.x == 2.0f && d.ribbon_uv_transform.z == 0.5f,
          "la cinta compila a la GPU (bloque 48, ids, tramos, UV)");

    // Id y eje en un float (vfx_common.glsl vfxRibbonPack) y la clave (id, edad).
    const Vec3 axis = core::normalize(Vec3{1.0f, 2.0f, -3.0f});
    const float packed = vfx::ribbonPack(200, axis);
    const Vec3 unpacked = vfx::ribbonPackedAxis(packed);
    check(vfx::ribbonPackedId(packed) == 200 && core::dot(unpacked, axis) > 0.999f && packed < 16777216.0f,
          "id y eje de la cinta empaquetados (exacto, < 2^24)");
    const float up_packed = vfx::ribbonPack(0, Vec3{0.0f, 1.0f, 0.0f});
    check(vfx::ribbonPackedId(up_packed) == 0 && vfx::ribbonPackedAxis(up_packed).y > 0.999f, "id 0 y el eje Y");
    check(vfx::ribbonSortKey(0, 5.0f) < vfx::ribbonSortKey(1, 0.0f) && vfx::ribbonSortKey(3, 0.010f) < vfx::ribbonSortKey(3, 0.012f) &&
              vfx::ribbonSortKey(255, 63.8f) < vfx::ribbonSortKey(255, 63.9f) && vfx::ribbonSortKey(255, 100.0f) < 16777216.0f,
          "orden de las cintas por (id, edad), sin perder precision");

    // Plantilla "Estela de proyectil".
    int projectile = -1;
    for (int i = 0; i < vfx::vfxPresetCount(); ++i) {
        if (std::string(vfx::vfxPresetName(i)) == "Estela de proyectil") projectile = i;
    }
    bool by_distance = false;
    bool fades = false;
    bool has_attributes = false;
    if (projectile >= 0) {
        const vfx::VfxGraph p = vfx::vfxPreset(projectile);
        for (const vfx::Block& b : p.spawn) by_distance = by_distance || b.kind == vfx::BlockKind::RateOverDistance;
        for (const vfx::Block& b : p.initialize) has_attributes = has_attributes || b.kind == vfx::BlockKind::RibbonAttributes;
        for (const vfx::Block& b : p.output_blocks) {
            if (b.kind == vfx::BlockKind::ColorOverLife && !b.gradient.empty()) fades = b.gradient.back().color.w == 0.0f;
        }
        check(p.output.type == gfx::VfxOutput::Ribbon && p.output.ribbon_uv == gfx::VfxRibbonUv::Tile && by_distance && fades &&
                  has_attributes,
              "plantilla Estela de proyectil: cinta por distancia que se apaga en la cola");
    } else {
        check(false, "plantilla Estela de proyectil");
    }

    // RateOverDistance alimenta la cinta: un punto cada 1/8 m (20 m/s, 1 s).
    vfx::VfxGraph trail;
    trail.spawn.push_back(vfx::makeBlock(vfx::BlockKind::RateOverDistance));
    trail.spawn.back().values[0] = 8.0f;
    trail.output.type = gfx::VfxOutput::Ribbon;
    vfx::SpawnState state;
    std::mt19937 rng(7);
    std::uint32_t points = 0;
    for (int i = 0; i < 60; ++i) points += vfx::advanceSpawn(trail, {}, state, 1.0f / 60.0f, 20.0f / 60.0f, true, rng);
    gfx::VfxInstanceDesc td;
    vfx::compileGraph(trail, {}, td);
    check(points >= 159 && points <= 161 && td.interpolate_spawn, "caudal por distancia: 160 puntos en 20 m (repartidos en el frame)");
}

// Flipbook fundido: cuadro actual, siguiente y fraccion; cuadro inicial al
// azar y variacion de velocidad por particula (la misma cuenta que vfx.vert).
void testVfxFlipbookBlend() {
    std::printf("VFX: flipbook fundido\n");
    check(vfx::flipbookStartValue(3, 1.0f) == 3.0f, "sin variacion el cuadro inicial queda entero (como antes)");
    vfx::FlipbookSample s = vfx::flipbookSample(3.0f, 0.25f, 1.0f, 16, 0.0f);
    check(s.current == 7 && s.next == 8 && s.blend < 1e-4f, "a lo largo de la vida: cuadro 3 + 4");
    s = vfx::flipbookSample(3.0f, 0.26f, 1.0f, 16, 0.0f);
    check(s.current == 7 && s.next == 8 && std::fabs(s.blend - 0.16f) < 1e-3f, "fraccion para fundir con el siguiente");
    s = vfx::flipbookSample(0.0f, 0.55f, 1.0f, 16, 10.0f);
    check(s.current == 5 && s.next == 6 && std::fabs(s.blend - 0.5f) < 1e-3f, "por cuadros/s");
    s = vfx::flipbookSample(0.0f, 0.35f, 1.0f, 4, 10.0f);
    check(s.current == 3 && s.next == 0 && std::fabs(s.blend - 0.5f) < 1e-3f, "por cuadros/s da la vuelta al ultimo");
    s = vfx::flipbookSample(0.0f, 1.0f, 1.0f, 8, 0.0f);
    check(s.current == 7 && s.next == 7 && s.blend == 0.0f, "al final de la vida se queda en el ultimo sin fundir");
    const float fast = vfx::flipbookStartValue(0, 1.2f);
    const float slow = vfx::flipbookStartValue(2, 0.8f);
    check(vfx::flipbookSample(fast, 1.05f, 1.0f, 16, 10.0f).current == 12 && vfx::flipbookSample(slow, 1.05f, 1.0f, 16, 10.0f).current == 10,
          "variacion de velocidad por particula (+-20 %) y cuadro inicial");

    vfx::VfxGraph g;
    g.output.flip_cols = 4;
    g.output.flip_rows = 4;
    g.output.flip_blend = true;
    vfx::Block frame = vfx::makeBlock(vfx::BlockKind::FlipbookFrame);
    check(frame.values[2] == 0.0f, "FlipbookFrame: sin variacion por defecto");
    frame.values[0] = 0.0f;
    frame.values[1] = 15.0f;
    frame.values[2] = 0.2f;
    g.initialize.push_back(frame);
    vfx::VfxGraph back;
    const bool round_trip = vfx::vfxGraphFromText(vfx::vfxGraphToText(g), back);
    gfx::VfxInstanceDesc d;
    vfx::compileGraph(back, {}, d);
    check(round_trip && back.output.flip_blend && d.flip_blend && d.initialize.size() == 1 && d.initialize[0].a.y == 15.0f &&
              std::fabs(d.initialize[0].a.z - 0.2f) < 1e-6f,
          "flip_blend y la variacion se guardan y compilan");
}

// Canales de datos (eventos de la GPU con nombre): 12 flechas que mueren en el
// suelo apuntan "Impactos" (tipo 1) y un solo efecto escucha y hace nacer 5
// astillas por evento. Sin GPU: el catalogo, el .crvfx, la compilacion y el
// canal de los scripts.
void testVfxChannels() {
    std::printf("VFX Graph: canales de datos\n");
    vfx::VfxGraph arrows;
    arrows.world_space = true;
    arrows.capacity = 12;
    vfx::Block burst = vfx::makeBlock(vfx::BlockKind::Burst);
    burst.values[0] = 12.0f;
    arrows.spawn.push_back(burst);
    arrows.update.push_back(vfx::makeBlock(vfx::BlockKind::Gravity));
    arrows.update.push_back(vfx::makeBlock(vfx::BlockKind::CollidePlane));
    vfx::Block on_death = vfx::makeBlock(vfx::BlockKind::EmitOnDeath);
    on_death.text = "Impactos";
    on_death.values[0] = 1.0f;  // tipo
    on_death.values[1] = 2.5f;  // valor
    arrows.update.push_back(on_death);
    vfx::Block every = vfx::makeBlock(vfx::BlockKind::EmitEvery);
    every.text = "Rastro";
    every.values[8] = 0.1f;
    arrows.update.push_back(every);
    vfx::Block on_hit = vfx::makeBlock(vfx::BlockKind::EmitOnCollision);
    on_hit.text = "Impactos";
    arrows.update.push_back(on_hit);

    vfx::VfxGraph shards;
    shards.world_space = false;  // escuchar lo pasa al mundo
    vfx::Block listen = vfx::makeBlock(vfx::BlockKind::OnGpuEvent);
    listen.text = "Impactos";
    listen.values[0] = 1.0f;   // tipo
    listen.values[1] = 5.0f;   // por evento
    listen.values[2] = 60.0f;  // maximo por frame (12 x 5)
    listen.values[3] = 0.5f;   // velocidad heredada
    shards.spawn.push_back(listen);
    shards.initialize.push_back(vfx::makeBlock(vfx::BlockKind::Lifetime));

    const vfx::BlockInfo* info = vfx::blockInfo("OnGpuEvent");
    check(info != nullptr && info->context == vfx::Context::Spawn && info->has_text && info->text_label != nullptr &&
              vfx::blockInfo("EmitOnDeath") != nullptr && vfx::blockInfo("EmitOnCollision") != nullptr &&
              vfx::blockInfo("EmitEvery") != nullptr && vfx::isChannelBlock(vfx::BlockKind::EmitEvery),
          "bloques EmitOnDeath / EmitOnCollision / EmitEvery (Update) y OnGpuEvent (Spawn)");

    // El .crvfx: ida y vuelta con el canal.
    vfx::VfxGraph back;
    const bool round_trip = vfx::vfxGraphFromText(vfx::vfxGraphToText(arrows), back) && back.update.size() == 5 &&
                            back.update[2].kind == vfx::BlockKind::EmitOnDeath && back.update[2].text == "Impactos" &&
                            back.update[2].values[0] == 1.0f && back.update[3].values[8] == 0.1f;
    vfx::VfxGraph shards_back;
    const bool listen_trip = vfx::vfxGraphFromText(vfx::vfxGraphToText(shards), shards_back) && shards_back.spawn.size() == 1 &&
                             shards_back.spawn[0].kind == vfx::BlockKind::OnGpuEvent && shards_back.spawn[0].text == "Impactos";
    check(round_trip && listen_trip, "los bloques de canales se guardan y se leen igual");

    // Compilacion: los Emit* van al Update con el indice del canal en pad1 y
    // sin canal (pad0) hasta que VfxSystem lo pone.
    const std::vector<std::string> names = vfx::eventChannels(arrows);
    check(names.size() == 2 && names[0] == "Impactos" && names[1] == "Rastro", "eventChannels: sin repetir y en orden");
    gfx::VfxInstanceDesc d;
    vfx::compileGraph(arrows, {}, d);
    int emits = 0;
    bool death_ok = false;
    for (const gfx::VfxBlock& b : d.update) {
        if (!gfx::vfxWritesChannel(b.type)) continue;
        ++emits;
        if (b.type == static_cast<std::uint32_t>(gfx::VfxBlockType::EmitOnDeath)) {
            death_ok = b.pad0 == gfx::VfxPass::kNoChannel && b.pad1 == 0 && b.b.x == 1.0f && b.b.y == 2.5f && b.b.z == 1.0f &&
                       b.flags == 1u;
        }
        if (b.type == static_cast<std::uint32_t>(gfx::VfxBlockType::EmitEvery) && (b.pad1 != 1 || std::fabs(b.b.w - 0.1f) > 1e-6f)) {
            death_ok = false;
        }
    }
    check(emits == 3 && death_ok && d.listens.empty(), "los Emit* compilan al Update (tipo, valor, probabilidad, periodo)");
    gfx::VfxInstanceDesc ld;
    vfx::compileGraph(shards, {}, ld);
    check(ld.listens.size() == 1 && ld.listens[0].a.x == 1.0f && ld.listens[0].a.y == 5.0f && ld.listens[0].a.z == 60.0f &&
              ld.listens[0].a.w == 0.5f && ld.listens[0].flags == 1u && ld.listens[0].pad0 == gfx::VfxPass::kNoChannel &&
              ld.world_space,
          "OnGpuEvent compila a listens (tipo, 5 por evento, maximo, herencia) y simula en el mundo");
    // Escuchar no hace nacer nada en la CPU (es la GPU, con dispatch indirecto).
    vfx::SpawnState state;
    std::mt19937 rng(3);
    std::uint32_t cpu = 0;
    for (int i = 0; i < 30; ++i) cpu += vfx::advanceSpawn(shards, {}, state, 1.0f / 30.0f, 0.0f, true, rng);
    check(cpu == 0, "OnGpuEvent: ninguna nace en la CPU");
    // Desactivado o sin canal: ni escucha ni apunta.
    shards.spawn[0].enabled = false;
    arrows.update[2].text.clear();
    gfx::VfxInstanceDesc off;
    vfx::compileGraph(shards, {}, off);
    gfx::VfxInstanceDesc no_channel;
    vfx::compileGraph(arrows, {}, no_channel);
    int left = 0;
    for (const gfx::VfxBlock& b : no_channel.update) left += gfx::vfxWritesChannel(b.type) ? 1 : 0;
    check(off.listens.empty() && !off.world_space && left == 2, "desactivado o sin canal: fuera");

    // Un .crvfx de antes (sin canales) se lee y compila igual que siempre.
    vfx::VfxGraph old;
    const bool old_ok = vfx::vfxGraphFromText(
        R"({"capacity": 100, "spawn": [{"type": "ConstantRate", "values": {"rate": 10}}],
            "update": [{"type": "Gravity", "values": {"acceleration": [0, -9.81, 0]}}]})",
        old);
    gfx::VfxInstanceDesc od;
    if (old_ok) vfx::compileGraph(old, {}, od);
    check(old_ok && od.listens.empty() && od.update.size() == 1 && vfx::eventChannels(old).empty(), "un .crvfx de antes, igual");

    // El canal de los scripts: 100 impactos por segundo de varios jugadores
    // en el mismo canal (un frame de 30 Hz son ~4; aqui un segundo entero).
    vfx::VfxSystem system;
    int accepted = 0;
    for (int player = 0; player < 4; ++player) {
        for (int i = 0; i < 25; ++i) {
            gfx::VfxEventRecord e;
            e.type = 1.0f;
            e.position = Vec3{static_cast<float>(player), 0.0f, static_cast<float>(i)};
            accepted += system.pushGpuEvent("Impactos", e) ? 1 : 0;
        }
    }
    check(accepted == 100 && system.pendingGpuEvents() == 100 && system.channelId("Impactos") == 0,
          "100 impactos de 4 jugadores en un solo canal (sin GPU: pendientes)");
    // Tope de canales y de eventos por canal y frame.
    for (int c = 1; c < static_cast<int>(gfx::VfxPass::kMaxChannels); ++c) system.channelId("Canal" + std::to_string(c));
    check(system.channelId("Otro") == gfx::VfxPass::kNoChannel && !system.pushGpuEvent("Otro", gfx::VfxEventRecord{}) &&
              system.channelId("Impactos") == 0,
          "como mucho kMaxChannels canales (los de antes siguen)");
    for (std::uint32_t i = 0; i < gfx::VfxPass::kMaxChannelEvents; ++i) system.pushGpuEvent("Impactos", gfx::VfxEventRecord{});
    check(system.pendingGpuEvents() == gfx::VfxPass::kMaxChannelEvents, "tope de eventos por canal y frame");
    system.clear();
    check(system.pendingGpuEvents() == 0, "clear() los tira");
}

// Salida Malla: variantes / LOD, orientacion (cuaternion, camara), escala al
// azar, material, bloques MeshIndex y AngularVelocity (spec item 7).
void testVfxMeshParticles() {
    std::printf("VFX Graph: particulas de malla\n");
    // Un .crvfx de antes (salida Malla sin las claves nuevas) carga igual.
    vfx::VfxGraph old;
    const bool old_ok = vfx::vfxGraphFromText(
        R"({"capacity": 64, "output": {"type": "Mesh", "mesh_orient": "Fixed", "mesh_scale": [1, 2, 3]}})", old);
    check(old_ok && old.output.type == gfx::VfxOutput::Mesh && old.output.mesh_orient == gfx::VfxMeshOrient::Fixed &&
              old.output.mesh_variants.empty() && old.output.mesh_lod_distance == 0.0f &&
              old.output.mesh_sprite_distance == 0.0f && old.output.mesh_rotation.w == 1.0f &&
              old.output.mesh_scale_jitter.x == 0.0f && !old.output.material.valid(),
          "un .crvfx de malla de antes carga con los valores de siempre");
    gfx::VfxInstanceDesc old_desc;
    vfx::compileGraph(old, {}, old_desc);
    check(old_desc.mesh_variant_count == 1 && old_desc.mesh_sprite_distance == 0.0f && old_desc.tint.x == 1.0f &&
              old_desc.mesh_rotation.w == 1.0f,
          "sin variantes ni material: una malla, sin sprite, color sin tocar");

    // Todo lo nuevo, guardado y leido.
    vfx::VfxGraph g;
    g.output.type = gfx::VfxOutput::Mesh;
    g.output.mesh = assets::AssetRef{Uuid::generate(), assets::AssetType::Model};
    g.output.mesh_orient = gfx::VfxMeshOrient::Camera;
    g.output.mesh_variants.push_back(assets::AssetRef{Uuid::generate(), assets::AssetType::Model});
    g.output.mesh_variants.push_back(assets::AssetRef{{}, assets::AssetType::Model});  // hueco: la principal
    g.output.mesh_lod_distance = 12.0f;
    g.output.mesh_sprite_distance = 80.0f;
    g.output.mesh_rotation = core::Vec4{0.0f, 2.0f, 0.0f, 2.0f};  // sin normalizar
    g.output.mesh_scale_jitter = Vec3{0.25f, 0.0f, 0.5f};
    g.output.material = assets::AssetRef{Uuid::generate(), assets::AssetType::Material};
    vfx::Block index = vfx::makeBlock(vfx::BlockKind::MeshIndex);
    index.values[0] = 5.0f;  // fuera de rango: se acota a 0..3
    index.values[1] = 9.0f;
    g.initialize.push_back(index);
    vfx::Block spin = vfx::makeBlock(vfx::BlockKind::AngularVelocity);
    spin.values[0] = 180.0f;  // grados/s2
    spin.values[1] = 0.5f;
    spin.values[2] = 90.0f;   // grados por metro
    g.update.push_back(spin);
    vfx::VfxGraph back;
    const bool round = vfx::vfxGraphFromText(vfx::vfxGraphToText(g), back);
    check(round && back.output.mesh_orient == gfx::VfxMeshOrient::Camera && back.output.mesh_variants.size() == 2 &&
              back.output.mesh_variants[0].uuid == g.output.mesh_variants[0].uuid && !back.output.mesh_variants[1].valid() &&
              back.output.mesh_lod_distance == 12.0f && back.output.mesh_sprite_distance == 80.0f &&
              back.output.mesh_rotation.y == 2.0f && back.output.mesh_scale_jitter.z == 0.5f &&
              back.output.material.uuid == g.output.material.uuid && back.initialize.size() == 1 && back.update.size() == 1,
          "variantes, LOD, sprite, cuaternion, escala al azar y material se guardan y se leen");

    gfx::VfxInstanceDesc desc;
    vfx::compileGraph(back, {}, desc);
    const float qlen = std::sqrt(desc.mesh_rotation.x * desc.mesh_rotation.x + desc.mesh_rotation.y * desc.mesh_rotation.y +
                                 desc.mesh_rotation.z * desc.mesh_rotation.z + desc.mesh_rotation.w * desc.mesh_rotation.w);
    check(desc.mesh_variant_count == 3 && desc.mesh_lod_distance == 12.0f && desc.mesh_sprite_distance == 80.0f &&
              std::fabs(qlen - 1.0f) < 1e-4f && desc.mesh_scale_jitter.x == 0.25f,
          "compila: 3 mallas, LOD, sprite y cuaternion normalizado");
    bool index_ok = false;
    for (const gfx::VfxBlock& b : desc.initialize) {
        if (b.type == static_cast<std::uint32_t>(gfx::VfxBlockType::MeshIndex)) index_ok = b.a.x == 3.0f && b.a.y == 3.0f;
    }
    check(index_ok, "MeshIndex a la GPU, acotado a las 4 mallas");
    bool spin_ok = false;
    for (const gfx::VfxBlock& b : desc.update) {
        if (b.type == static_cast<std::uint32_t>(gfx::VfxBlockType::AngularVelocity)) {
            spin_ok = std::fabs(b.a.x - 3.14159265f) < 1e-3f && b.a.y == 0.5f && std::fabs(b.a.z - 1.5707963f) < 1e-3f;
        }
    }
    check(spin_ok, "AngularVelocity en Update (grados -> radianes)");

    // Material: opaco / transparente, color, emision y sin luz.
    vfx::VfxMaterialSource source;
    source.base_color = core::Vec4{0.5f, 0.25f, 1.0f, 1.0f};
    source.emissive = Vec3{1.0f, 0.5f, 0.0f};
    source.emissive_intensity = 2.0f;
    source.unlit = true;
    const vfx::VfxMaterial opaque = vfx::vfxMaterialFrom(source);
    source.transparent = true;
    source.transparent_blend = 2;  // aditivo
    const vfx::VfxMaterial additive = vfx::vfxMaterialFrom(source);
    source.transparent_blend = 0;  // vidrio -> alfa
    const vfx::VfxMaterial glass = vfx::vfxMaterialFrom(source);
    check(opaque.blend == gfx::VfxBlend::Opaque && additive.blend == gfx::VfxBlend::Additive &&
              glass.blend == gfx::VfxBlend::Alpha && !opaque.lit && opaque.emissive.x == 2.0f,
          "material .crmat: opaco, aditivo, vidrio como alfa, sin luz y emision x intensidad");
    gfx::VfxInstanceDesc lit_desc;
    lit_desc.lit = true;
    vfx::applyVfxMaterial(opaque, lit_desc);
    gfx::VfxInstanceDesc distortion_desc;
    distortion_desc.blend = gfx::VfxBlend::Distortion;
    vfx::applyVfxMaterial(opaque, distortion_desc);
    check(lit_desc.blend == gfx::VfxBlend::Opaque && !lit_desc.lit && lit_desc.tint.y == 0.25f &&
              distortion_desc.blend == gfx::VfxBlend::Distortion,
          "el material manda sobre la mezcla y la luz (no sobre Distorsion)");
}

// Mezcla Distorsion del VFX Graph (spec item 8).
void testVfxDistortion() {
    std::printf("VFX Graph: distorsion\n");
    vfx::VfxGraph old;
    vfx::vfxGraphFromText(R"({"output": {"blend": "Premultiplied"}})", old);
    check(old.output.blend == gfx::VfxBlend::Premultiplied && old.output.distortion == 3.0f && !old.output.distortion_height,
          "un .crvfx de antes: su mezcla, distorsion por defecto");
    vfx::VfxGraph unknown;
    vfx::vfxGraphFromText(R"({"output": {"blend": "Desconocida"}})", unknown);
    check(unknown.output.blend == gfx::VfxBlend::Additive, "mezcla desconocida: aditiva");

    vfx::VfxGraph g;
    g.output.blend = gfx::VfxBlend::Distortion;
    g.output.distortion = 9.0f;  // mas de 5 px: se acota
    g.output.distortion_height = true;
    g.output.texture = "Texturas/calor_normal.png";
    const std::string text = vfx::vfxGraphToText(g);
    vfx::VfxGraph back;
    const bool round = vfx::vfxGraphFromText(text, back);
    check(round && text.find("\"Distortion\"") != std::string::npos && back.output.blend == gfx::VfxBlend::Distortion &&
              back.output.distortion == gfx::kVfxMaxDistortionPixels && back.output.distortion_height,
          "Distorsion se guarda y se lee (como mucho 5 px a 1080p)");
    gfx::VfxInstanceDesc desc;
    vfx::compileGraph(g, {}, desc);
    check(desc.blend == gfx::VfxBlend::Distortion && desc.distortion == gfx::kVfxMaxDistortionPixels && desc.distortion_height,
          "compila a la mezcla Distorsion acotada");
    vfx::VfxGraph opaque;
    opaque.output.blend = gfx::VfxBlend::Opaque;
    vfx::VfxGraph opaque_back;
    vfx::vfxGraphFromText(vfx::vfxGraphToText(opaque), opaque_back);
    check(opaque_back.output.blend == gfx::VfxBlend::Opaque, "mezcla Opaca se guarda y se lee");
    check(!vfx::vfxDistortionForQuality(0) && vfx::vfxDistortionForQuality(1) && vfx::vfxDistortionForQuality(3),
          "calidad Baja: sin distorsion; Media en adelante, con ella");
    check(vfx::vfxDistortionCvar(), "render.vfx.Distortion encendida por defecto");
}

// Escalabilidad por tipo de efecto (MMO) y las mejoras pequenas: orden de
// dibujo, ruido rizado horneado, pool de instancias, texturas y tiempo.
void testVfxScalability() {
    std::printf("VFX: escalabilidad por tipo de efecto\n");
    using vfx::BudgetCandidate;
    using Decision = BudgetCandidate::Decision;

    // Un .crvfx de antes (sin las claves nuevas) se lee igual: sin tipo, sin
    // caja, orden 0, sin repartir; y al guardarlo no aparecen.
    {
        vfx::VfxGraph old;
        const bool ok = vfx::vfxGraphFromText(R"({"capacity": 300, "duration": 2, "loop": false})", old);
        check(ok && old.effect_type == vfx::EffectType::None && !old.fixed_bounds && old.sort_order == 0 &&
                  old.spawn_spread_frames == 1 && old.capacity == 300,
              "un .crvfx de antes: sin tipo ni caja (sin limites, como antes)");
        const std::string text = vfx::vfxGraphToText(old);
        check(text.find("effect_type") == std::string::npos && text.find("\"bounds\"") == std::string::npos &&
                  text.find("sort_order") == std::string::npos,
              "sin tipo se guarda igual que antes");
        vfx::VfxGraph typed = old;
        typed.effect_type = vfx::EffectType::Impact;
        typed.sort_order = 3;
        typed.fixed_bounds = true;
        typed.bounds_center = Vec3{0.0f, 1.0f, 0.0f};
        typed.bounds_size = Vec3{2.0f, 3.0f, 4.0f};
        typed.spawn_spread_frames = 4;
        vfx::VfxGraph back;
        check(vfx::vfxGraphFromText(vfx::vfxGraphToText(typed), back) && back.effect_type == vfx::EffectType::Impact &&
                  back.sort_order == 3 && back.fixed_bounds && std::fabs(back.bounds_size.z - 4.0f) < 1e-4f &&
                  std::fabs(back.bounds_center.y - 1.0f) < 1e-4f && back.spawn_spread_frames == 4,
              "tipo, orden, caja fija y rafagas repartidas se guardan y se leen");
        check(vfx::effectTypeFromKey("Ambient") == vfx::EffectType::Ambient &&
                  vfx::effectTypeFromKey("???") == vfx::EffectType::None,
              "tipos por su clave (desconocido = sin tipo)");
    }

    // Tabla por defecto: Baja mas estricta que Ultra; movil mas que escritorio.
    {
        const vfx::VfxScalability low = vfx::defaultScalability(0, vfx::VfxPlatform::Desktop);
        const vfx::VfxScalability ultra = vfx::defaultScalability(3, vfx::VfxPlatform::Desktop);
        const vfx::VfxScalability mobile = vfx::defaultScalability(0, vfx::VfxPlatform::Mobile);
        bool stricter = true;
        for (int t = 0; t < vfx::kEffectTypes; ++t) {
            const auto& l = low.types[static_cast<std::size_t>(t)];
            const auto& u = ultra.types[static_cast<std::size_t>(t)];
            const auto& m = mobile.types[static_cast<std::size_t>(t)];
            stricter = stricter && l.max_instances <= u.max_instances && l.particle_scale <= u.particle_scale &&
                       (u.max_distance == 0.0f || l.max_distance <= u.max_distance) && m.max_instances <= l.max_instances &&
                       m.particle_scale <= l.particle_scale && (l.disabled_outputs & ~m.disabled_outputs) == 0u;
        }
        check(stricter, "Baja <= Ultra y movil <= escritorio en distancia, instancias y particulas");
        const auto& hero = low.types[static_cast<std::size_t>(vfx::EffectType::Hero)];
        const auto& ambient = low.types[static_cast<std::size_t>(vfx::EffectType::Ambient)];
        check(hero.max_distance == 0.0f && (ambient.disabled_outputs & gfx::kVfxOutMesh) != 0u &&
                  ambient.policy == vfx::OverBudget::Pause && ultra.types[1].disabled_outputs == 0u,
              "Heroe sin distancia; en Baja el Ambiente sin mallas y en pausa al pasarse");
    }

    // Presupuesto: cada politica.
    {
        vfx::VfxScalability s;
        for (auto& b : s.types) b = vfx::EffectTypeBudget{};
        auto& impact = s.types[static_cast<std::size_t>(vfx::EffectType::Impact)];
        impact.max_instances = 2;
        impact.policy = vfx::OverBudget::RemoveFarthest;
        auto& combat = s.types[static_cast<std::size_t>(vfx::EffectType::Combat)];
        combat.max_instances = 2;
        combat.policy = vfx::OverBudget::DontCreate;
        auto& ambient = s.types[static_cast<std::size_t>(vfx::EffectType::Ambient)];
        ambient.max_distance = 30.0f;
        ambient.policy = vfx::OverBudget::Pause;

        const auto make = [](vfx::EffectType type, float distance, bool running = false, bool in_view = true) {
            BudgetCandidate c;
            c.type = type;
            c.distance = distance;
            c.running = running;
            c.in_view = in_view;
            return c;
        };
        std::vector<BudgetCandidate> c = {make(vfx::EffectType::Impact, 5.0f), make(vfx::EffectType::Impact, 1.0f),
                                          make(vfx::EffectType::Impact, 3.0f), make(vfx::EffectType::Impact, 8.0f)};
        vfx::applyBudget(c, s);
        check(c[1].decision == Decision::Run && c[2].decision == Decision::Run && c[0].decision == Decision::Removed &&
                  c[3].decision == Decision::Removed,
              "RemoveFarthest: entran los 2 mas cercanos, los lejanos se quitan");
        c = {make(vfx::EffectType::Combat, 50.0f, true), make(vfx::EffectType::Combat, 10.0f, true),
             make(vfx::EffectType::Combat, 1.0f, false)};
        vfx::applyBudget(c, s);
        check(c[0].decision == Decision::Run && c[1].decision == Decision::Run && c[2].decision == Decision::Removed,
              "DontCreate: los que ya corrian se quedan, el nuevo no entra aunque este cerca");
        c = {make(vfx::EffectType::Ambient, 10.0f), make(vfx::EffectType::Ambient, 45.0f),
             make(vfx::EffectType::Ambient, 5.0f, false, false), make(vfx::EffectType::None, 5000.0f, false, true)};
        vfx::applyBudget(c, s);
        check(c[0].decision == Decision::Run && c[1].decision == Decision::Paused && c[2].decision == Decision::Hidden &&
                  c[3].decision == Decision::Run,
              "Pause: el lejano se congela; fuera de la vista se oculta; sin tipo no se toca");
        // 300 impactos de un MMO en Baja: como mucho los de la tabla.
        const vfx::VfxScalability low = vfx::defaultScalability(0, vfx::VfxPlatform::Desktop);
        std::vector<BudgetCandidate> crowd;
        for (int i = 0; i < 300; ++i) crowd.push_back(make(vfx::EffectType::Impact, static_cast<float>(i % 40)));
        vfx::applyBudget(crowd, low);
        const auto running =
            std::count_if(crowd.begin(), crowd.end(), [](const BudgetCandidate& b) { return b.decision == Decision::Run; });
        check(running == low.types[static_cast<std::size_t>(vfx::EffectType::Impact)].max_instances,
              "300 impactos a la vez en Baja: solo corren los de la tabla");

        // Jugador local y prioridad (Keryan E21): con el tope lleno (DontCreate)
        // el esencial entra aunque sea nuevo y este lejos, y echa a los demas.
        c = {make(vfx::EffectType::Combat, 5.0f, true), make(vfx::EffectType::Combat, 6.0f, true),
             make(vfx::EffectType::Combat, 500.0f, false)};
        c[2].essential = true;
        vfx::applyBudget(c, s);
        check(c[2].decision == Decision::Run && c[0].decision == Decision::Run && c[1].decision == Decision::Removed,
              "jugador local: entra con el tope lleno (sin limite de distancia) y sale el de otro");
        c = {make(vfx::EffectType::Combat, 1.0f, true), make(vfx::EffectType::Combat, 2.0f, true),
             make(vfx::EffectType::Combat, 3.0f, true)};
        for (auto& x : c) x.essential = true;
        vfx::applyBudget(c, s);
        check(std::all_of(c.begin(), c.end(), [](const BudgetCandidate& b) { return b.decision == Decision::Run; }),
              "jugador local: nunca se descarta aunque se pase del tope");
        c = {make(vfx::EffectType::Impact, 1.0f), make(vfx::EffectType::Impact, 2.0f), make(vfx::EffectType::Impact, 30.0f)};
        c[2].priority = 5;
        vfx::applyBudget(c, s);
        check(c[2].decision == Decision::Run && c[0].decision == Decision::Run && c[1].decision == Decision::Removed,
              "prioridad: con el tope lleno se queda antes el de mas prioridad");
        c = {make(vfx::EffectType::Ambient, 45.0f)};
        c[0].essential = true;
        vfx::applyBudget(c, s);
        check(c[0].decision == Decision::Run, "jugador local: sin limite de distancia");
    }

    // Efectos terminados: no cuentan en el presupuesto.
    {
        vfx::FinishedState f;
        f.emitting = true;
        f.quiet_seconds = 100.0f;
        f.alive_known = true;
        check(!vfx::effectFinished(f), "emitiendo (o con bucle) no termina");
        f.emitting = false;
        f.pending = true;
        check(!vfx::effectFinished(f), "con eventos o rafagas pendientes no termina");
        f.pending = false;
        f.quiet_seconds = 0.1f;
        check(!vfx::effectFinished(f), "recien nacidas: la lectura de la GPU aun no las ve");
        f.quiet_seconds = 1.0f;
        f.max_lifetime = 3.0f;
        f.alive = 12;
        check(!vfx::effectFinished(f), "con particulas vivas no termina");
        f.alive = 0;
        check(vfx::effectFinished(f), "sin emision y sin vivas: terminado");
        f.alive = 12;
        f.alive_known = false;
        f.quiet_seconds = 3.5f;
        check(vfx::effectFinished(f), "pasada su vida maxima ya no puede quedar ninguna");
    }

    // Rafagas repartidas y precalentado en varios frames.
    {
        std::uint32_t backlog = 0, rate = 0;
        std::vector<std::uint32_t> frames;
        frames.push_back(vfx::takeSpread(backlog, rate, 400, 4));
        for (int i = 0; i < 4; ++i) frames.push_back(vfx::takeSpread(backlog, rate, 0, 4));
        check(frames[0] == 100 && frames[1] == 100 && frames[2] == 100 && frames[3] == 100 && frames[4] == 0 && backlog == 0,
              "una rafaga de 400 en 4 frames: 100 por frame");
        check(vfx::takeSpread(backlog, rate, 3, 4) == 3 && backlog == 0, "las pocas salen ya (sin esperar)");
        check(vfx::takeSpread(backlog, rate, 50, 1) == 50, "sin repartir (1 frame): todas a la vez, como antes");
        // Precalentado de 800 en 8 frames con un caudal de 2 por frame.
        vfx::queueSpread(backlog, rate, 800, 8);
        std::uint32_t total = 0, first = 0;
        for (int i = 0; i < 8; ++i) {
            const std::uint32_t n = vfx::takeSpread(backlog, rate, 2, 1);
            if (i == 0) first = n;
            total += n;
        }
        check(total == 816 && first == 102 && backlog == 0, "precalentado de 800 repartido en 8 frames (sin tiron)");
    }

    // Vista: esfera delante, detras y a un lado.
    {
        scene::Camera camera;
        camera.setPosition(Vec3{0.0f, 0.0f, 0.0f});
        camera.lookAt(Vec3{0.0f, 0.0f, -10.0f});
        camera.setAspectRatio(16.0f / 9.0f);
        const core::Mat4 vp = camera.projection() * camera.view();
        check(vfx::sphereInView(vp, Vec3{0.0f, 0.0f, -10.0f}, 1.0f), "delante de la camara: se ve");
        check(!vfx::sphereInView(vp, Vec3{0.0f, 0.0f, 10.0f}, 1.0f), "detras: no se ve");
        check(!vfx::sphereInView(vp, Vec3{200.0f, 0.0f, -10.0f}, 1.0f) && vfx::sphereInView(vp, Vec3{200.0f, 0.0f, -10.0f}, 400.0f),
              "a un lado: no se ve (salvo que la caja sea enorme)");
    }

    // Orden de dibujo entre efectos (sortOrder).
    {
        const gfx::VfxDrawKey smoke_near{0, false, 5.0f};
        const gfx::VfxDrawKey smoke_far{0, false, 50.0f};
        const gfx::VfxDrawKey fire{0, true, 1.0f};
        const gfx::VfxDrawKey on_top{1, false, 100.0f};
        check(gfx::vfxDrawBefore(smoke_far, smoke_near) && gfx::vfxDrawBefore(smoke_near, fire) &&
                  gfx::vfxDrawBefore(fire, on_top) && !gfx::vfxDrawBefore(on_top, smoke_far),
              "sort_order manda; a igual orden, transparentes de atras adelante y luego aditivos");
    }

    // Ruido rizado horneado: se repite, coincide con el calculado en sus
    // texeles y entre ellos va suave.
    {
        std::vector<core::Vec4> volume;
        gfx::bakeVfxCurlVolume(volume);
        const std::size_t n = gfx::kVfxCurlSize;
        check(volume.size() == n * n * n, "volumen de 32^3");
        const float period = static_cast<float>(gfx::kVfxCurlPeriod);
        float max_tile = 0.0f, max_texel = 0.0f, max_mid = 0.0f, mean = 0.0f;
        std::mt19937 rng(7);
        std::uniform_real_distribution<float> any(-20.0f, 20.0f);
        for (int i = 0; i < 200; ++i) {
            const Vec3 q{any(rng), any(rng), any(rng)};
            const Vec3 a = gfx::sampleVfxCurlVolume(volume, q);
            const Vec3 b = gfx::sampleVfxCurlVolume(volume, q + Vec3{period, -period, 2.0f * period});
            max_tile = std::max(max_tile, core::length(a - b));
            mean += core::length(a) / 200.0f;
            // En un texel: el valor exacto. Entre texeles: cerca del exacto.
            const Vec3 texel{std::floor(q.x * 4.0f) / 4.0f, std::floor(q.y * 4.0f) / 4.0f, std::floor(q.z * 4.0f) / 4.0f};
            max_texel = std::max(max_texel, core::length(gfx::sampleVfxCurlVolume(volume, texel) - gfx::vfxCurlReference(texel)));
            const Vec3 exact = gfx::vfxCurlReference(q);
            max_mid = std::max(max_mid, core::length(a - exact) / std::max(core::length(exact), 0.5f));
        }
        check(max_tile < 1e-3f, "se repite cada 8 celdas (tileable)");
        check(max_texel < 1e-3f, "en sus texeles es el campo calculado");
        check(mean > 0.2f && max_mid < 0.75f, "con fuerza y suave entre texeles (fallback: el calculado sigue en el shader)");
    }

    // Pool de instancias: aparcar y reutilizar ranuras.
    {
        gfx::VfxSlotRecycler<int> pool(3);
        check(pool.park(256, 1) && pool.park(512, 2) && pool.park(256, 3) && !pool.park(256, 4) && pool.size() == 3,
              "aparca hasta su tope");
        int token = 0;
        check(pool.take(256, token) && token == 3 && pool.take(256, token) && token == 1 && !pool.take(256, token),
              "reutiliza con la misma capacidad (la mas reciente primero)");
        check(!pool.take(1024, token) && pool.evictOldest(token) && token == 2 && pool.size() == 0,
              "otra capacidad no sirve; la mas antigua se puede liberar");
    }

    // Texturas: mas de 16 ranuras si el dispositivo puede.
    check(gfx::VfxPass::kMaxTextures >= 256 && gfx::VfxPass::kMinTextures == 16, "hasta 256 texturas (16 como poco)");

    // Tiempo del juego y calidad.
    {
        vfx::VfxSystem system;
        check(std::fabs(system.timeScale() - 1.0f) < 1e-6f, "tiempo normal por defecto");
        system.setTimeScale(0.5f);
        cvar::Registry::instance().set("vfx.timeScale", "0.5");
        check(std::fabs(system.timeScale() - 0.25f) < 1e-6f, "camara lenta: setTimeScale x vfx.timeScale");
        system.setTimeScale(0.0f);
        check(system.timeScale() == 0.0f, "pausa: 0");
        cvar::Registry::instance().set("vfx.timeScale", "1");
        vfx::setQualityPreset(0);
        check(vfx::qualityLevel() == 0, "el preset de graficos pone la calidad de los efectos");
        cvar::Registry::instance().set("vfx.quality", "3");
        check(vfx::qualityLevel() == 3, "vfx.quality manda sobre el preset");
        cvar::Registry::instance().set("vfx.quality", "-1");
        vfx::setQualityPreset(2);
    }
}

void testPhysics2D() {
    std::printf("Fisica 2D\n");
    twod::register2DComponents();
    ecs::World world;
    ecs::Entity ground = world.create("Suelo");
    ground.add<twod::BoxCollider2D>().size = core::Vec2{20.0f, 1.0f};
    ecs::Entity box = world.create("Caja");
    box.setWorldPosition(Vec3{0.0f, 5.0f, 0.0f});
    box.add<twod::BoxCollider2D>();
    box.add<twod::Rigidbody2D>();
    twod::System2D system;
    system.start(world);
    for (int i = 0; i < 240; ++i) system.update(world, 1.0f / 60.0f, twod::System2D::Mode::Play);
    const float y = box.worldPosition().y;
    check(y > 0.8f && y < 1.2f, "la caja cae y reposa sobre el suelo");
    twod::RaycastHit2D hit;
    const bool hit_ground = system.physics().raycast(core::Vec2{3.0f, 5.0f}, core::Vec2{0.0f, -1.0f}, 20.0f, hit);
    check(hit_ground && hit.entity == ground && std::fabs(hit.point.y - 0.5f) < 0.05f, "raycast 2D contra el suelo");
    bool entered = false;
    for (const twod::Physics2DEvent& e : system.takeEvents()) entered = entered || e.type == twod::Physics2DEventType::CollisionEnter;
    check(entered, "evento OnCollisionEnter2D");
    system.stop();

    // Tilemap: celdas, rectangulos solidos y Rule Tiles.
    twod::Tilemap map;
    for (int x = 0; x < 5; ++x) map.setTile(x, 0, 1);
    for (int x = 0; x < 5; ++x) map.setTile(x, 1, 1);
    check(map.getTile(3, 1) == 1 && map.getTile(9, 9) == 0, "poner y leer celdas");
    const std::vector<twod::TileRect> rects = twod::solidRects(map, nullptr);
    check(rects.size() == 1 && rects[0].w == 5 && rects[0].h == 2, "dos filas iguales: un rectangulo");
    twod::Tileset tileset;
    tileset.rule_tiles.push_back(twod::makeTerrainRuleTile("Terreno", 0, 3));
    twod::Tilemap terrain;
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 3; ++x) terrain.setTile(x, y, -1);
    }
    const int center = twod::resolveRuleTile(tileset, terrain.layers[0].data, 1, 1, 0);
    const int corner = twod::resolveRuleTile(tileset, terrain.layers[0].data, 0, 2, 0);
    check(center != corner, "la Rule Tile elige otro dibujo en la esquina");
}

void testLocalization() {
    std::printf("Localizacion\n");
    gameplay::Localization loc;
    loc.addLanguage("es", "Espanol");
    loc.addLanguage("en", "English");
    std::string error;
    const bool ok = loc.fromCsv("key,es,en\nhola,Hola {0},Hello {0}\nmonedas#one,{n} moneda,{n} coin\nmonedas#other,{n} monedas,{n} coins\n",
                                true, &error);
    check(ok, "leer un CSV");
    loc.setLanguage("es");
    check(loc.get("hola", {"Ana"}) == "Hola Ana", "argumentos");
    check(loc.plural("monedas", 1) == "1 moneda" && loc.plural("monedas", 3) == "3 monedas", "plurales");
    loc.setLanguage("en");
    check(loc.get("hola", {"Ana"}) == "Hello Ana", "cambiar de idioma");
    check(loc.get("no.existe") == "no.existe", "clave que falta: la propia clave");
}

void testDialogue() {
    std::printf("Dialogos\n");
    gameplay::DialogueAsset d;
    d.name = "Prueba";
    const int start = d.add(gameplay::DialogueNodeType::Start).id;
    const int line = d.add(gameplay::DialogueNodeType::Line).id;
    const int choice = d.add(gameplay::DialogueNodeType::Choice).id;
    const int set = d.add(gameplay::DialogueNodeType::SetVariable).id;
    const int end = d.add(gameplay::DialogueNodeType::End).id;
    d.find(start)->next = line;
    d.find(line)->speaker = "Mercader";
    d.find(line)->text = "Tienes {$oro} monedas";
    d.find(line)->next = choice;
    d.find(choice)->options = {gameplay::DialogueOption{"Comprar", {}, {}, true, set},
                               gameplay::DialogueOption{"Adios", {}, {}, true, end}};
    d.find(set)->variable = "oro";
    d.find(set)->op = "-=";
    d.find(set)->value = "3";
    d.find(set)->next = end;
    gameplay::DialogueSystem system;
    system.setVariable("oro", 10);
    check(system.start(d), "empieza");
    check(system.hasLine() && system.line().text == "Tienes 10 monedas", "linea con variable");
    // Una linea seguida de Opciones las ensena a la vez (con la linea encima).
    check(system.waitingChoice() && system.choices().size() == 2, "opciones junto a la linea");
    check(system.choose(0), "elegir la primera");
    std::printf("  (oro = %s, activo = %d)\n", system.variable("oro").dump().c_str(), system.active() ? 1 : 0);
    check(!system.active() && system.variable("oro").get<double>() == 7.0, "la opcion cambia la variable y termina");
}

void testSave() {
    std::printf("Partidas\n");
    gameplay::registerSaveComponents();
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "cramion_save_test";
    std::error_code ec;
    std::filesystem::remove_all(folder, ec);
    ecs::World world;
    ecs::Entity e = world.create("Cofre");
    e.add<gameplay::Saveable>();
    e.setWorldPosition(Vec3{1.0f, 2.0f, 3.0f});
    gameplay::SaveSystem saves;
    saves.setFolder(folder);
    saves.beginScene(world, "Test", "Test.crscene");
    saves.values()["oro"] = 42;
    std::string error;
    check(saves.save(world, "slot1", "Prueba", &error), "guardar");
    saves.flush();
    e.setWorldPosition(Vec3{9.0f, 9.0f, 9.0f});
    saves.values()["oro"] = 0;
    nlohmann::json snapshot;
    check(saves.read("slot1", snapshot, &error), "leer la ranura");
    check(saves.apply(world, snapshot, &error), "aplicar");
    check(std::fabs(e.worldPosition().x - 1.0f) < 1e-3f && saves.values().value("oro", 0) == 42, "posicion y valores de vuelta");
    check(saves.list().size() == 1, "listar ranuras");
    std::filesystem::remove_all(folder, ec);
}

void testBehaviorTree() {
    std::printf("Behavior Trees\n");
    ai::BehaviorTreeAsset tree;
    tree.ensureRoot();
    ai::Variable v;
    v.name = "listo";
    v.value.type = ai::VarType::Bool;
    tree.blackboard.push_back(v);
    const int sequence = tree.addNode(ai::BtNodeKind::Sequence, 0);
    const int set = tree.addNode(ai::BtNodeKind::SetBlackboard, sequence);
    tree.nodes[static_cast<std::size_t>(set)].params.key = "listo";
    tree.nodes[static_cast<std::size_t>(set)].params.value = "true";
    ai::BtRuntime rt;
    ai::resetBehaviorTree(tree, rt);
    ai::BtContext ctx;
    ctx.dt = 0.016f;
    const ai::BtStatus status = ai::tickBehaviorTree(tree, rt, ctx);
    const ai::Value* listo = rt.find("listo");
    check(listo != nullptr && listo->b, "Set Blackboard escribe la clave");
    check(status == ai::BtStatus::Success || status == ai::BtStatus::Running, "la secuencia termina bien");
    check(ai::behaviorTreeFromJson(ai::behaviorTreeToJson(ai::exampleGuardBehaviorTree()), tree), "el guardia de ejemplo se lee");
}

void testGraphs() {
    std::printf("Visual Scripting y Shader Graph\n");
    const vscript::CompileResult compiled = vscript::compileGraph(vscript::exampleGraph(), "Ejemplo.crgraph");
    check(compiled.ok && compiled.errors.empty(), "el grafo de ejemplo es valido");
    namespace sg = assets::shadergraph;
    sg::GenerateResult result;
    const sg::Graph graph = sg::makeDefault();
    check(sg::generate(graph, "Prueba", result) && result.errors.empty(), "el Shader Graph genera el .crshader");
    check(result.code.find("property") != std::string::npos, "con la propiedad del tinte");
    sg::Graph back;
    check(sg::parse(sg::serialize(graph), back) && back.nodes.size() == graph.nodes.size(), "el grafo se guarda y se lee");
}

void testFracture() {
    std::printf("Fractura\n");
    physics::FractureSettings settings;
    settings.pieces = 12;
    physics::FractureData data;
    std::string error;
    const bool ok = physics::fractureMesh(physics::boxSource(Vec3{2.0f, 1.0f, 1.0f}), settings, data, &error);
    check(ok && data.roots().size() >= 6, "la caja se parte en trozos");
    float volume = 0.0f;
    for (const int r : data.roots()) volume += data.pieces[static_cast<std::size_t>(r)].volume;
    check(std::fabs(volume - 2.0f) < 0.2f, "los trozos suman el volumen de la caja");
    physics::FractureData back;
    check(physics::fractureFromText(physics::fractureToText(data), back) && back.pieces.size() == data.pieces.size(), "el .crfracture se lee");
}

void testBake() {
    std::printf("Iluminacion horneada\n");
    lighting::BakeScene scene;
    // Suelo blanco de 20 x 20 m y un sol desde arriba.
    // Antihorarios vistos desde arriba (la cara de fuera mira a +Y).
    const lighting::BakeTriangle a{Vec3{-10, 0, -10}, Vec3{10, 0, 10}, Vec3{10, 0, -10}, Vec3{0.8f, 0.8f, 0.8f}, {}};
    const lighting::BakeTriangle b{Vec3{-10, 0, -10}, Vec3{-10, 0, 10}, Vec3{10, 0, 10}, Vec3{0.8f, 0.8f, 0.8f}, {}};
    scene.triangles = {a, b};
    scene.lights.sun.direction = Vec3{0.0f, -1.0f, 0.0f};
    scene.lights.sun.color = Vec3{1.0f, 1.0f, 1.0f};
    scene.lights.sun.intensity = 3.0f;
    lighting::BakeVolume volume;
    volume.min = Vec3{-2.0f, 0.5f, -2.0f};
    volume.size = Vec3{4.0f, 2.0f, 4.0f};
    volume.nx = volume.ny = volume.nz = 2;
    scene.volumes = {volume};
    scene.settings.rays = 128;
    scene.settings.bounces = 1;
    scene.settings.threads = 2;
    gfx::BakedLighting out;
    lighting::BakeStats stats;
    std::string error;
    check(lighting::bakeProbes(scene, out, nullptr, &stats, &error) && stats.probes == 8, "8 sondas horneadas");
    const core::Vec4* probe = out.probes.data();
    const Vec3 down = lighting::probeIrradiance(probe, Vec3{0.0f, -1.0f, 0.0f});
    const Vec3 up = lighting::probeIrradiance(probe, Vec3{0.0f, 1.0f, 0.0f});
    std::printf("  (abajo %.3f, arriba %.3f, cielo %.2f, invalidas %zu)\n", down.x, up.x,
                lighting::probeSkyVisibility(probe, Vec3{0.0f, 1.0f, 0.0f}), stats.invalid_probes);
    check(down.x > 0.1f && down.x > up.x, "el suelo al sol rebota luz hacia arriba");
    check(lighting::probeSkyVisibility(probe, Vec3{0.0f, 1.0f, 0.0f}) > 0.8f, "arriba se ve el cielo");
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "cramion_bake_test.crbake";
    gfx::BakedLighting back;
    gfx::LightingMode mode = gfx::LightingMode::Realtime;
    check(lighting::saveBakedLighting(file, out, gfx::LightingMode::Baked) && lighting::loadBakedLighting(file, back, mode) &&
              back.probes.size() == out.probes.size() && mode == gfx::LightingMode::Baked,
          "el .crbake se guarda y se lee");
    std::error_code ec;
    std::filesystem::remove(file, ec);
}

// Que entra en el horneado: Static propio y heredado (hijo de una instancia
// de prefab o de la raiz de un modelo), Rigidbody, desactivados y terreno.
void testBakeGather() {
    std::printf("Horneado: que entra\n");
    assets::AssetDatabase database;
    assets::AssetManager manager(database);
    const auto cube = [](ecs::World& world, const char* name, ecs::Entity parent = {}) {
        ecs::Entity e = world.create(name, parent);
        e.add<ecs::MeshRenderer>().mesh = ecs::Mesh::cube();
        return e;
    };
    {
        ecs::World world;
        ecs::Entity a = cube(world, "Estatico");
        a.get<ecs::EntityInfo>().is_static = true;
        cube(world, "Suelto");
        lighting::BakeScene scene;
        lighting::BakeGatherReport report;
        lighting::gatherBakeScene(world, manager, scene, {}, {}, &report);
        check(report.meshes_baked == 1 && scene.triangles.size() == 12, "un cubo Static: solo el (12 triangulos)");
        check(report.skipped[static_cast<int>(lighting::BakeSkip::NotStatic)] == 1, "el otro se omite por no ser Static");
    }
    {
        // La raiz de una instancia de prefab marcada Static (sin malla) y su
        // hijo con malla (sin marcar): el hijo hereda el Static.
        ecs::World world;
        ecs::Entity root = world.create("Prefab");
        root.get<ecs::EntityInfo>().is_static = true;
        ecs::Entity child = cube(world, "Hijo", root);
        child.setLocalPosition(Vec3{0.0f, 2.0f, 0.0f});
        cube(world, "Otro");
        lighting::BakeScene scene;
        lighting::BakeGatherReport report;
        lighting::gatherBakeScene(world, manager, scene, {}, {}, &report);
        check(lighting::staticInHierarchy(child), "el hijo de un Static es Static");
        check(report.meshes_baked == 1 && report.static_marked == 1 && scene.triangles.size() == 12,
              "el hijo de la instancia Static se hornea");
        check(!scene.triangles.empty() && scene.triangles.front().a.y > 1.0f, "en su sitio del mundo");
    }
    {
        // Nada Static: todo lo que no se mueve. Un Rigidbody Dynamic se omite;
        // Static con Rigidbody Dynamic entra (con aviso).
        ecs::World world;
        cube(world, "Quieto");
        cube(world, "Cae").add<physics::Rigidbody>();
        lighting::BakeScene scene;
        lighting::BakeGatherReport report;
        lighting::gatherBakeScene(world, manager, scene, {}, {}, &report);
        check(report.used_all_non_moving && report.meshes_baked == 1 &&
                  report.skipped[static_cast<int>(lighting::BakeSkip::DynamicBody)] == 1,
              "sin Static: entra lo que no se mueve, no el Rigidbody Dynamic");
        ecs::World world2;
        ecs::Entity s = cube(world2, "Static con fisica");
        s.get<ecs::EntityInfo>().is_static = true;
        s.add<physics::Rigidbody>();
        lighting::BakeScene scene2;
        lighting::BakeGatherReport report2;
        lighting::gatherBakeScene(world2, manager, scene2, {}, {}, &report2);
        check(report2.meshes_baked == 1 && report2.static_with_dynamic_rb == 1, "Static manda sobre el Rigidbody Dynamic");
    }
    {
        // Un Static desactivado no deja la escena vacia sin decir por que.
        ecs::World world;
        ecs::Entity a = cube(world, "Apagado");
        a.get<ecs::EntityInfo>().is_static = true;
        a.setActive(false);
        lighting::BakeScene scene;
        lighting::BakeGatherReport report;
        lighting::gatherBakeScene(world, manager, scene, {}, {}, &report);
        const std::string summary = report.summary();
        std::printf("  (%s)\n", summary.c_str());
        check(scene.triangles.empty() && summary.find("Nada que hornear") != std::string::npos &&
                  summary.find("desactivadas") != std::string::npos,
              "sin nada que hornear, el informe dice el motivo");
    }
    {
        // Terreno Static desde su mapa de alturas (sin collider ni fisica).
        ecs::World world;
        ecs::Entity t = world.create("Terreno");
        t.get<ecs::EntityInfo>().is_static = true;
        terrain::Terrain& comp = t.add<terrain::Terrain>();
        comp.size = 16.0f;
        comp.height = 4.0f;
        comp.collision = false;
        auto data = std::make_shared<terrain::TerrainData>();
        data->create(17, 16, 0.5f);
        lighting::BakeScene scene;
        lighting::BakeGatherReport report;
        lighting::gatherBakeScene(world, manager, scene, {},
                                  [&](ecs::Entity) -> std::shared_ptr<const terrain::TerrainData> { return data; }, &report);
        check(report.terrains == 1 && report.terrain_triangles == 16u * 16u * 2u && scene.triangles.size() == 512,
              "el terreno entra (16 x 16 celdas)");
        check(!scene.triangles.empty() && std::fabs(scene.triangles.front().a.y - 2.0f) < 1e-3f, "a su altura (0.5 x 4 m)");
    }
    {
        // Luces: la Horneada lleva su directa; la de solo tiempo real no entra.
        ecs::World world;
        ecs::Entity baked = world.create("Lampara");
        ecs::Light& l = baked.add<ecs::Light>();
        l.bake_mode = ecs::LightBakeMode::Baked;
        ecs::Entity realtime = world.create("Linterna");
        realtime.add<ecs::Light>().bake_mode = ecs::LightBakeMode::Realtime;
        world.create("Mixta").add<ecs::Light>();
        scene::LightSet sun;
        lighting::BakeScene scene;
        lighting::BakeGatherReport report;
        lighting::gatherBakeLights(world, sun, scene, ecs::LightBakeMode::Mixed, &report);
        int baked_count = 0;
        for (const std::uint8_t b : scene.point_baked) baked_count += b;
        check(scene.lights.points.size() == 2 && baked_count == 1 && scene.baked_light_ids.size() == 1 &&
                  scene.baked_light_ids[0][0] == baked.uuid().high && scene.baked_light_ids[0][1] == baked.uuid().low,
              "luces: 1 horneada y 1 mixta en el horneado, la de tiempo real fuera");
        check(!scene.sun_baked && report.lights_realtime == 1, "el sol mixto no se hornea entero");
    }
    {
        // Una pieza que el renderizador ya se llevo: al dibujarla, RenderSync
        // MUEVE su ModelData del AssetManager a scene::Scene y en el
        // AssetManager queda vacio. Con el editor abierto no entraba ningun
        // modelo ("0 de 105 mallas entran ... Marca los objetos como Static").
        database.refresh();  // con las primitivas en la base de datos (sin archivo), como en el editor
        const std::shared_ptr<const assets::ModelAsset> cube_asset = manager.loadModel(assets::builtin::kCube);
        const std::size_t cube_triangles = cube_asset ? cube_asset->parts[0]->indices.size() / 3 : 0;
        scene::Scene drawn;
        const std::uint32_t index = drawn.addModel(std::move(*cube_asset->parts[0]));  // como RenderSync::resolveModel
        check(cube_triangles > 0 && cube_asset->parts[0]->indices.empty(), "la pieza del AssetManager queda vacia al dibujarla");
        ecs::World world;
        ecs::Entity e = world.create("Cubo dibujado");
        e.get<ecs::EntityInfo>().is_static = true;
        e.add<ecs::MeshRenderer>().model = assets::AssetRef{assets::builtin::kCube, assets::AssetType::Model};
        lighting::BakeScene scene;
        lighting::BakeGatherReport report;
        lighting::gatherBakeScene(world, manager, scene, {}, {}, &report,
                                  [&](const Uuid& model, int part) -> const asset::ModelData* {
                                      return model == assets::builtin::kCube && part == 0 ? drawn.models()[index].get() : nullptr;
                                  });
        check(report.meshes_baked == 1 && scene.triangles.size() == cube_triangles && report.reread_models == 0,
              "la pieza movida se hornea con la malla de la escena del renderizador");
        lighting::BakeScene scene2;
        lighting::BakeGatherReport report2;
        lighting::gatherBakeScene(world, manager, scene2, {}, {}, &report2);
        check(report2.meshes_baked == 1 && scene2.triangles.size() == cube_triangles && report2.reread_models == 0 &&
                  report2.summary().find("del disco") == std::string::npos,
              "sin el renderizador, la primitiva movida se vuelve a generar (no cuenta como leida del disco)");
        manager.unload(assets::builtin::kCube);
    }
    {
        // Un modelo de verdad (.crdata) con un suelo y un cristal (material
        // transparente). Sus piezas se mueven como al dibujarlas: con la
        // escena del renderizador entran; sin ella se releen del disco; y si
        // el archivo ya no esta, el informe dice por que (sin culpar a Static).
        const std::filesystem::path folder = std::filesystem::temp_directory_path() / "cramion_bake_model_test";
        std::error_code ec;
        std::filesystem::remove_all(folder, ec);
        std::filesystem::create_directories(folder, ec);
        const auto quad = [](const char* name, float y, bool transparent) {
            asset::ModelData d;
            d.name = name;
            for (const Vec3& p : {Vec3{-1.0f, y, -1.0f}, Vec3{1.0f, y, -1.0f}, Vec3{1.0f, y, 1.0f}, Vec3{-1.0f, y, 1.0f}}) {
                asset::SkinnedVertex v{};
                v.position = p;
                v.normal = Vec3{0.0f, 1.0f, 0.0f};
                d.vertices.push_back(v);
            }
            d.indices = {0, 2, 1, 0, 3, 2};
            asset::SubMesh sm;
            sm.index_count = 6;
            d.submeshes.push_back(sm);
            asset::MaterialData material;
            material.name = name;
            material.transparent = transparent;
            d.materials.push_back(material);
            // Una textura incrustada (la relectura de la geometria la salta).
            asset::TextureData texture;
            texture.name = std::string(name) + "_albedo";
            texture.width = 2;
            texture.height = 2;
            texture.pixels.assign(16, 200);
            d.textures.push_back(texture);
            return d;
        };
        const Uuid model_uuid = Uuid::generate();
        const std::vector<assets::ModelNode> nodes = {{"Casa", -1, core::Mat4::identity(), -1},
                                                      {"Suelo", 0, core::Mat4::identity(), 0},
                                                      {"Cristal", 0, core::Mat4::identity(), 1}};
        const std::vector<asset::ModelData> pieces = {quad("Suelo", 0.0f, false), quad("Cristal", 1.0f, true)};
        const std::filesystem::path file = folder / "Casa.crdata";
        std::string error;
        check(assets::writeGeneratedModel(file, model_uuid, "Casa", nodes, pieces, &error), "un .crdata de prueba con dos piezas");
        assets::AssetDatabase models_db;
        models_db.open(folder);
        assets::AssetManager models(models_db);
        const std::shared_ptr<const assets::ModelAsset> casa = models.loadModel(model_uuid);
        check(casa && casa->parts.size() == 2, "se carga");
        if (casa && casa->parts.size() == 2) {
            ecs::World world;
            ecs::Entity root = world.create("Casa");
            root.get<ecs::EntityInfo>().is_static = true;
            world.create("Suelo", root).add<ecs::MeshRenderer>().model = assets::AssetRef{model_uuid, assets::AssetType::Model};
            ecs::MeshRenderer& glass = world.create("Cristal", root).add<ecs::MeshRenderer>();
            glass.model = assets::AssetRef{model_uuid, assets::AssetType::Model};
            glass.part = 1;
            scene::Scene drawn;
            const std::uint32_t floor_index = drawn.addModel(std::move(*casa->parts[0]));
            const std::uint32_t glass_index = drawn.addModel(std::move(*casa->parts[1]));
            const lighting::ModelPartProvider from_scene = [&](const Uuid& model, int part) -> const asset::ModelData* {
                if (model != model_uuid) return nullptr;
                return part == 0 ? drawn.models()[floor_index].get() : (part == 1 ? drawn.models()[glass_index].get() : nullptr);
            };
            const int no_geometry = static_cast<int>(lighting::BakeSkip::NoGeometry);
            const int transparent = static_cast<int>(lighting::BakeSkip::Transparent);
            lighting::BakeScene scene;
            lighting::BakeGatherReport report;
            lighting::gatherBakeScene(world, models, scene, {}, {}, &report, from_scene);
            std::printf("  (%s)\n", report.summary().c_str());
            check(report.meshes_baked == 1 && scene.triangles.size() == 2 && report.skipped[no_geometry] == 0,
                  "las piezas movidas de un .crdata entran desde la escena");
            check(report.skipped[transparent] == 1 && report.transparent_triangles == 2 &&
                      report.summary().find("transparentes") != std::string::npos,
                  "el cristal transparente no tapa la luz (y se dice)");
            lighting::BakeScene scene2;
            lighting::BakeGatherReport report2;
            lighting::gatherBakeScene(world, models, scene2, {}, {}, &report2);
            check(report2.meshes_baked == 1 && scene2.triangles.size() == 2 && report2.reread_models == 1,
                  "sin la escena, la geometria se relee del .crdata (una vez)");
            // La relectura salta los bytes de las texturas sin leerlos (y lee
            // bien lo que va detras) y solo trae las piezas pedidas.
            const std::shared_ptr<assets::ModelAsset> geometry =
                assets::AssetManager::readModelGeometry(model_uuid, file, "Casa", {false, true});
            check(geometry && geometry->parts.size() == 2 && geometry->parts[0]->indices.empty() &&
                      geometry->parts[1]->indices.size() == 6 && geometry->parts[1]->materials.size() == 1 &&
                      geometry->parts[1]->materials[0].transparent && geometry->parts[1]->textures.size() == 1 &&
                      geometry->parts[1]->textures[0].name == "Cristal_albedo" &&
                      geometry->parts[1]->textures[0].width == 2 && geometry->parts[1]->textures[0].pixels.empty(),
                  "la geometria se relee sin los pixeles de las texturas (y solo las piezas pedidas)");
            std::filesystem::remove(file, ec);
            lighting::BakeScene scene3;
            lighting::BakeGatherReport report3;
            lighting::gatherBakeScene(world, models, scene3, {}, {}, &report3);
            const std::string summary = report3.summary();
            std::printf("  (%s)\n", summary.c_str());
            check(scene3.triangles.empty() && report3.static_marked == 2 && report3.skipped[no_geometry] == 2 &&
                      summary.find("sin geometría") != std::string::npos && summary.find("Suelo") != std::string::npos,
                  "sin malla legible: el informe dice 'sin geometría' y la nombra");
            check(summary.find("como Static") == std::string::npos, "y no pide marcar Static (ya lo estan)");
        }
        std::filesystem::remove_all(folder, ec);
    }
    {
        // Una malla Static sin triangulos tampoco se pierde sin motivo (y no
        // se dice que no se pudo leer: es de codigo, no hay archivo).
        ecs::World world;
        cube(world, "Cubo").get<ecs::EntityInfo>().is_static = true;
        ecs::Entity empty = world.create("Malla vacia");
        empty.get<ecs::EntityInfo>().is_static = true;
        empty.add<ecs::MeshRenderer>().mesh = std::make_shared<ecs::Mesh>();
        lighting::BakeScene scene;
        lighting::BakeGatherReport report;
        lighting::gatherBakeScene(world, manager, scene, {}, {}, &report);
        const std::string summary = report.summary();
        const int empty_mesh = static_cast<int>(lighting::BakeSkip::EmptyMesh);
        std::printf("  (%s)\n", summary.c_str());
        check(report.meshes_baked == 1 && report.skipped[empty_mesh] == 1 &&
                  report.skipped[static_cast<int>(lighting::BakeSkip::NoGeometry)] == 0 &&
                  report.meshes_baked + report.skipped[empty_mesh] == report.meshes_seen &&
                  summary.find("Malla vacia") != std::string::npos && summary.find("vacía") != std::string::npos,
              "cada malla cuenta: horneada o con su motivo (la vacia, como vacia)");
    }
    {
        // Static solo en algo que no aporta (un cristal, una malla vacia): no
        // entra nada y el resto no es Static. Ahi marcar los demas si sirve, y
        // se dice.
        ecs::World world;
        cube(world, "Muro");
        ecs::Entity glass = world.create("Ventana");
        glass.get<ecs::EntityInfo>().is_static = true;
        glass.add<ecs::MeshRenderer>().mesh = std::make_shared<ecs::Mesh>();
        lighting::BakeScene scene;
        lighting::BakeGatherReport report;
        lighting::gatherBakeScene(world, manager, scene, {}, {}, &report);
        const std::string summary = report.summary();
        std::printf("  (%s)\n", summary.c_str());
        check(scene.triangles.empty() && report.skipped[static_cast<int>(lighting::BakeSkip::NotStatic)] == 1 &&
                  summary.find("marca también los demás") != std::string::npos,
              "si lo Static no aporta nada, pide marcar los demas");
    }
}

// Lee un texel del lightmap de superficie (la misma tabla que baked_gi.frag)
// y su irradiancia hacia `n`. -1 si no esta.
float surfaceIrradiance(const gfx::SurfaceLightmap& s, const Vec3& p, const Vec3& n) {
    if (s.capacity == 0) return -1.0f;
    const float inv = 1.0f / s.cell_size;
    const std::uint64_t key = gfx::surfaceCellKey(static_cast<int>(std::floor(p.x * inv)), static_cast<int>(std::floor(p.y * inv)),
                                                  static_cast<int>(std::floor(p.z * inv)));
    const std::uint32_t lo = static_cast<std::uint32_t>(key & 0xFFFFFFFFull);
    const std::uint32_t hi = static_cast<std::uint32_t>(key >> 32);
    std::uint32_t slot = gfx::surfaceCellHash(lo, hi) & (s.capacity - 1);
    for (std::uint32_t i = 0; i < s.capacity; ++i) {
        const std::uint32_t klo = s.keys[static_cast<std::size_t>(slot) * 2];
        const std::uint32_t khi = s.keys[static_cast<std::size_t>(slot) * 2 + 1];
        if (klo == gfx::kSurfaceEmptyKey && khi == gfx::kSurfaceEmptyKey) return -1.0f;
        if (klo == lo && khi == hi) {
            const core::Vec4* t = &s.texels[static_cast<std::size_t>(slot) * gfx::kSurfaceTexelVec4];
            // floats 0..11 = c0.rgb c1.rgb c2.rgb c3.rgb (rojo: 0, 3, 6, 9)
            const float c0 = t[0].x, c1 = t[0].w, c2 = t[1].z, c3 = t[2].y;
            return c0 * 0.282095f + c1 * 0.488603f * n.y + c2 * 0.488603f * n.z + c3 * 0.488603f * n.x;
        }
        slot = (slot + 1) & (s.capacity - 1);
    }
    return -1.0f;
}

// Un horneado diminuto con el sol Horneado: el lightmap lleva su luz directa
// y la sombra de un tejado.
void testBakedDirect() {
    std::printf("Luz directa y sombras horneadas\n");
    lighting::BakeScene scene;
    const Vec3 albedo{0.5f, 0.5f, 0.5f};
    // Suelo de 8 x 8 m (mira a +Y) y un tejado de 2 x 8 m a 1 m sobre x = -2.
    scene.triangles.push_back({Vec3{-4, 0, -4}, Vec3{4, 0, 4}, Vec3{4, 0, -4}, albedo, {}});
    scene.triangles.push_back({Vec3{-4, 0, -4}, Vec3{-4, 0, 4}, Vec3{4, 0, 4}, albedo, {}});
    scene.triangles.push_back({Vec3{-3, 1, -4}, Vec3{-1, 1, 4}, Vec3{-1, 1, -4}, albedo, {}});
    scene.triangles.push_back({Vec3{-3, 1, -4}, Vec3{-3, 1, 4}, Vec3{-1, 1, 4}, albedo, {}});
    scene.lights.sun.direction = Vec3{0.0f, -1.0f, 0.0f};
    scene.lights.sun.color = Vec3{1.0f, 1.0f, 1.0f};
    scene.lights.sun.intensity = 3.0f;
    scene.sun_baked = true;
    scene.baked_light_ids.push_back({0x1234ull, 0x5678ull});
    scene.settings.sky_intensity = 0.0f;
    scene.settings.bounces = 0;
    scene.settings.rays = 16;
    scene.settings.surface_texel = 0.25f;
    scene.settings.surface_rays = 16;
    scene.settings.direct_samples = 4;
    scene.settings.threads = 2;
    gfx::BakedLighting out;
    lighting::BakeStats stats;
    std::string error;
    check(lighting::bakeProbes(scene, out, nullptr, &stats, &error) && !out.surface.empty(), "horneado con lightmap de superficie");
    const Vec3 up{0.0f, 1.0f, 0.0f};
    const float lit = surfaceIrradiance(out.surface, Vec3{2.1f, 0.0f, 0.1f}, up);
    const float shadow = surfaceIrradiance(out.surface, Vec3{-1.9f, 0.0f, 0.1f}, up);
    std::printf("  (al sol %.3f, a la sombra %.3f, texeles %zu)\n", lit, shadow, stats.surface_texels);
    check(lit > 2.4f && lit < 3.6f, "el suelo al sol tiene la luz directa del sol (3)");
    check(shadow > -0.5f && shadow < lit * 0.2f, "bajo el tejado queda la sombra horneada");
    check(out.sun_baked && out.baked_lights.size() == 1, "lo horneado entero queda apuntado");
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "cramion_bake_direct_test.crbake";
    gfx::BakedLighting back;
    gfx::LightingMode mode = gfx::LightingMode::Realtime;
    check(lighting::saveBakedLighting(file, out, gfx::LightingMode::Baked) && lighting::loadBakedLighting(file, back, mode) &&
              back.sun_baked && back.baked_lights.size() == 1 && back.baked_lights[0][0] == 0x1234ull &&
              back.surface.count == out.surface.count && gfx::bakedSunMatches(back, Vec3{0.0f, -1.0f, 0.0f}) &&
              !gfx::bakedSunMatches(back, Vec3{0.3f, -1.0f, 0.0f}),
          "el .crbake (version 3) guarda las luces horneadas");
    std::error_code ec;
    std::filesystem::remove(file, ec);
}

// Escenas grandes (una ciudad): el volumen automatico cabe en el tope de
// sondas (y cubre el terreno si cabe), una caja enorme no tumba el horneado,
// el sol horneado llega tambien fuera de los volumenes (si cabe) y el texel
// automatico crece si lo de dentro no cabe.
void testBakeLargeScenes() {
    std::printf("Horneado de escenas grandes\n");
    const Vec3 albedo{0.5f, 0.5f, 0.5f};
    {
        // 400 x 400 m con sondas cada 2 m son 1.25 millones (tope 200000):
        // antes el horneado fallaba; ahora se separan mas.
        const std::vector<lighting::BakeTriangle> city = {{Vec3{0, 0, 0}, Vec3{400, 0, 0}, Vec3{0, 0, 400}, albedo, {}},
                                                          {Vec3{0, 0, 0}, Vec3{10, 120, 0}, Vec3{0, 0, 10}, albedo, {}}};
        const lighting::BakeVolume v = lighting::volumeAround(city, 2.0f, 200000.0f);
        const double probes = static_cast<double>(v.nx) * v.ny * v.nz;
        const float spacing = v.size.x / static_cast<float>(v.nx - 1);
        std::printf("  (%u x %u x %u = %.0f sondas, cada %.2f m, y de %.1f a %.1f m)\n", v.nx, v.ny, v.nz, probes,
                    static_cast<double>(spacing), static_cast<double>(v.min.y), static_cast<double>(v.min.y + v.size.y));
        check(probes <= 200000.0 && spacing > 2.0f, "el volumen automatico cabe en el tope de sondas");
        check(v.min.y <= 0.0f && v.min.y + v.size.y > 50.0f, "de alto empieza en el suelo (la torre de 120 m no lo levanta)");
        // Una caja de kilometros con sondas cada 10 cm: antes reservaba
        // gigas (std::bad_alloc en el hilo del horneado cerraba el editor).
        lighting::BakeScene scene;
        scene.triangles = city;
        lighting::BakeVolume too_big = v;
        too_big.nx = too_big.ny = too_big.nz = 100001;
        scene.volumes = {too_big};
        gfx::BakedLighting out;
        std::string error;
        const bool ok = lighting::bakeProbes(scene, out, nullptr, nullptr, &error);
        std::printf("  (%s)\n", error.c_str());
        check(!ok && error.find("demasiadas sondas") != std::string::npos && out.probes.empty(),
              "una caja enorme se rechaza antes de reservar memoria");
    }
    {
        // Una casa (las mallas, los 2 primeros triangulos) en un terreno.
        const auto with_terrain = [&](float half_side, const Vec3& house) {
            const Vec3 h0 = house, h1 = house + Vec3{10.0f, 6.0f, 0.0f}, h2 = house + Vec3{0.0f, 0.0f, 10.0f};
            return std::vector<lighting::BakeTriangle>{
                {h0, h1, h2, albedo, {}},
                {h0, h2, house + Vec3{10.0f, 0.0f, 10.0f}, albedo, {}},
                {Vec3{-half_side, 0, -half_side}, Vec3{half_side, 0, -half_side}, Vec3{-half_side, 0, half_side}, albedo, {}},
                {Vec3{half_side, 0, half_side}, Vec3{-half_side, 0, half_side}, Vec3{half_side, 0, -half_side}, albedo, {}}};
        };
        // Un terreno de 100 m: la caja lo cubre entero (lo dinamico va por el
        // terreno, no solo junto a la casa).
        const lighting::BakeVolume small = lighting::volumeAround(with_terrain(50.0f, Vec3{0, 0, 0}), 2.0f, 200000.0f, 2);
        std::printf("  (terreno de 100 m: caja de %.0f a %.0f m)\n", static_cast<double>(small.min.x),
                    static_cast<double>(small.min.x + small.size.x));
        check(small.min.x <= -50.0f && small.min.x + small.size.x >= 50.0f && small.min.z <= -50.0f &&
                  small.min.z + small.size.z >= 50.0f,
              "con terreno, el volumen automatico lo cubre si cabe");
        // Uno de 2 km con la casa en una esquina: 400 m alrededor de la casa.
        const lighting::BakeVolume big = lighting::volumeAround(with_terrain(1000.0f, Vec3{800, 0, 800}), 2.0f, 200000.0f, 2);
        std::printf("  (terreno de 2 km: caja de %.0f a %.0f m)\n", static_cast<double>(big.min.x),
                    static_cast<double>(big.min.x + big.size.x));
        check(big.size.x <= 400.0f && big.min.x <= 800.0f && big.min.x + big.size.x >= 810.0f && big.min.z <= 800.0f &&
                  big.min.z + big.size.z >= 810.0f && big.min.x + big.size.x <= 1001.5f,
              "con un terreno enorme, el volumen va alrededor de las mallas");
    }
    {
        // Sol horneado y un volumen que solo cubre la mitad del suelo: la
        // otra mitad tambien tiene texeles con el sol (en modo horneado no se
        // dibuja en tiempo real; antes se quedaba sin sol).
        lighting::BakeScene scene;
        scene.triangles.push_back({Vec3{-4, 0, -4}, Vec3{4, 0, 4}, Vec3{4, 0, -4}, albedo, {}});
        scene.triangles.push_back({Vec3{-4, 0, -4}, Vec3{-4, 0, 4}, Vec3{4, 0, 4}, albedo, {}});
        scene.lights.sun.direction = Vec3{0.0f, -1.0f, 0.0f};
        scene.lights.sun.color = Vec3{1.0f, 1.0f, 1.0f};
        scene.lights.sun.intensity = 3.0f;
        scene.sun_baked = true;
        lighting::BakeVolume half;
        half.min = Vec3{-4.0f, -1.0f, -4.0f};
        half.size = Vec3{4.0f, 2.0f, 8.0f};
        half.nx = half.ny = half.nz = 2;
        scene.volumes = {half};
        scene.settings.sky_intensity = 0.0f;
        scene.settings.bounces = 0;
        scene.settings.rays = 16;
        scene.settings.surface_rays = 16;
        scene.settings.direct_samples = 2;
        scene.settings.threads = 2;
        gfx::BakedLighting out;
        lighting::BakeStats stats;
        std::string error;
        const bool ok = lighting::bakeProbes(scene, out, nullptr, &stats, &error);
        const float outside = surfaceIrradiance(out.surface, Vec3{2.1f, 0.0f, 0.1f}, Vec3{0.0f, 1.0f, 0.0f});
        std::printf("  (fuera del volumen %.3f, texel %.2f m, %zu texeles)\n", outside, static_cast<double>(stats.surface_texel),
                    stats.surface_texels);
        check(ok && outside > 2.4f && outside < 3.6f && stats.outside_texels > 0 && !stats.outside_texels_dropped,
              "el sol horneado llega tambien fuera de los volumenes");

        // Los de fuera son un extra: si no caben se quedan fuera, sin
        // agrandar el texel de dentro ni hacer fallar un horneado que sin
        // ellos cabe (dentro: unos 153 texeles de 0.5 m; con los de fuera, ~290).
        scene.settings.max_surface_texels = 200;
        lighting::BakeStats kept;
        const bool auto_ok = lighting::bakeProbes(scene, out, nullptr, &kept, &error);
        std::printf("  (tope 200: texel %.2f m, %zu texeles, fuera %s)\n", static_cast<double>(kept.surface_texel),
                    kept.surface_texels, kept.outside_texels_dropped ? "no caben" : "caben");
        check(auto_ok && !kept.surface_texel_grown && kept.outside_texels_dropped && kept.outside_texels == 0 &&
                  kept.surface_texels > 100 && kept.surface_texels <= 200,
              "lo de fuera que no cabe no agranda el texel automatico");
        scene.settings.surface_texel = 0.5f;
        lighting::BakeStats manual;
        error.clear();
        check(lighting::bakeProbes(scene, out, nullptr, &manual, &error) && manual.outside_texels_dropped &&
                  manual.surface_texels == kept.surface_texels,
              "ni hace fallar un texel puesto a mano que dentro cabe");
        scene.settings.surface_texel = 0.0f;

        // Con un tope de texeles pequeno el texel automatico crece hasta caber...
        scene.settings.max_surface_texels = 100;
        lighting::BakeStats grown;
        const bool fits = lighting::bakeProbes(scene, out, nullptr, &grown, &error);
        std::printf("  (texel automatico %.2f m, %zu texeles)\n", static_cast<double>(grown.surface_texel), grown.surface_texels);
        check(fits && grown.surface_texel_grown && grown.surface_texel > 0.5f && grown.surface_texels <= 100,
              "el texel automatico se agranda si no cabe en el tope");
        // ...pero uno puesto a mano no cambia: falla al momento y dice que hacer.
        scene.settings.surface_texel = 0.5f;
        error.clear();
        check(!lighting::bakeProbes(scene, out, nullptr, nullptr, &error) &&
                  error.find("Tamaño del texel") != std::string::npos,
              "con el texel puesto a mano, el error nombra el ajuste");
    }
}

// Lightmaps por UV2 (PLAN-LIGHTMAPS.md, Fases 0 a 3).
#include "LightmapTests.inc"

void testReplay() {
    std::printf("Repeticiones\n");
    ecs::World world;
    ecs::Entity e = world.create("Pelota");
    replay::ReplayOptions options;
    options.rate = 30.0f;
    replay::ReplaySystem r;
    check(r.startRecording(world, options), "empieza a grabar");
    for (int i = 0; i <= 60; ++i) {
        e.setWorldPosition(Vec3{static_cast<float>(i) * 0.1f, 0.0f, 0.0f});
        r.update(world, 1.0f / 60.0f);
    }
    r.stopRecording();
    check(r.duration() > 0.9f, "un segundo grabado");
    e.setWorldPosition(Vec3{100.0f, 0.0f, 0.0f});
    check(r.play(world, 0.0f, 1.0f), "reproduce");
    r.setPaused(true);
    r.seek(0.5f);
    r.update(world, 0.0f);
    check(std::fabs(e.worldPosition().x - 3.0f) < 0.3f, "a los 0.5 s la pelota esta donde estaba");
    r.stop(world);
    check(std::fabs(e.worldPosition().x - 100.0f) < 1e-3f, "al parar todo vuelve como estaba");
}

}  // namespace

int main() {
    testVfx();
    testVfxParticleLight();
    testVfxRibbon();
    testVfxFlipbookBlend();
    testVfxChannels();
    testVfxScalability();
    testVfxMeshParticles();
    testVfxDistortion();
    testPhysics2D();
    testLocalization();
    testDialogue();
    testSave();
    testBehaviorTree();
    testGraphs();
    testFracture();
    testBake();
    testBakeGather();
    testBakedDirect();
    testBakeLargeScenes();
    // Lightmaps (PLAN-LIGHTMAPS.md)
    testSpotConeParity();
    testAverageTexture();
    testBakeSkySh();
    testBakeDeterminism();
    testLightmapUvUnwrap();
    testLightmapUvApplyRejects();
    testLightmapUvReadModel();
    testLightmapAtlasPacking();
    testLightmapWindowPatch();
    testLightmapThinWall();
    testLightmapValidity();
    testLightmapDeterminism();
    testCrbakeV4();
    testStaticBatchKeepOut();
    testLightmapSkyEnergy();
    testLightmapPointLightParity();
    testLightmapBounce();
    testLightmapDenoise();
    testStaticSunFit();
    testReplay();
    std::printf("\n%d de %d comprobaciones bien\n", checks - failures, checks);
    return failures == 0 ? 0 : 1;
}

// Pruebas sin GPU del bloque LUZ: sondas de reflexion colocadas
// (CramionFX/vk/ReflectionProbeSet.h, CramionCore/lighting/ReflectionProbes.h)
// y el empaquetado de lo que lee lighting.frag (GpuLights). Peso y fundido de
// la caja, proyeccion de caja, eleccion de las sondas de cada frame, mezcla de
// las 2 que mas pesan, empaquetado std140, el archivo .crrefl (ida y vuelta,
// danados, version futura) y el componente (registro y escena guardada y
// vuelta a leer). Devuelve 0 si todo va.

#include "CramionCore/ecs/SceneSerializer.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/lighting/ProbeBaker.h"
#include "CramionCore/lighting/ReflectionProbes.h"

#include <CramionFX/vk/GpuTypes.h>
#include <CramionFX/vk/ReflectionProbeSet.h>

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace cramion;
using core::Vec3;

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const char* what) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("  FALLO: %s\n", what);
    }
}

bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }
bool nearVec(const Vec3& a, const Vec3& b, float eps = 1e-4f) {
    return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps);
}

gfx::PlacedReflectionProbe box(std::uint64_t id, Vec3 center, Vec3 size, float blend = 1.0f, int priority = 0) {
    gfx::PlacedReflectionProbe p;
    p.id = gfx::PlacedProbeId{id, id * 7 + 1};
    p.position = center;
    p.box_min = center - size * 0.5f;
    p.box_max = center + size * 0.5f;
    p.blend_distance = blend;
    p.priority = priority;
    return p;
}

gfx::BakedReflectionCube cube(std::uint64_t id, std::uint32_t size, std::uint32_t mips, std::uint16_t seed) {
    gfx::BakedReflectionCube c;
    c.id = gfx::PlacedProbeId{id, id * 7 + 1};
    c.position = Vec3{1.0f, 2.0f, 3.0f};
    c.size = size;
    c.mips = mips;
    c.texels.resize(gfx::bakedCubeTexelCount(size, mips));
    for (std::size_t i = 0; i < c.texels.size(); ++i) c.texels[i] = static_cast<std::uint16_t>(seed + i * 31);
    return c;
}

void sizes() {
    std::printf("Tamanos del cubo horneado\n");
    // 128 px y 6 niveles (ReflectionProbe::kSize, kMips): 6 caras x 4 halfs.
    const std::size_t expected = 6u * 4u * (128u * 128u + 64u * 64u + 32u * 32u + 16u * 16u + 8u * 8u + 4u * 4u);
    check(gfx::bakedCubeTexelCount(128, 6) == expected, "halfs de un cubo de 128 px con 6 niveles");
    check(gfx::bakedCubeMipOffset(128, 0) == 0 && gfx::bakedCubeMipOffset(128, 1) == 6u * 4u * 128u * 128u &&
              gfx::bakedCubeMipOffset(128, 6) == expected,
          "donde empieza cada mip");
    gfx::BakedReflectionCube c = cube(1, 16, 3, 0);
    check(gfx::bakedCubeValid(c), "un cubo con su tamano vale");
    c.texels.pop_back();
    check(!gfx::bakedCubeValid(c), "con un half de menos, no");
    gfx::BakedReflectionCube empty;
    check(!gfx::bakedCubeValid(empty), "vacio, no");
}

void weights() {
    std::printf("Peso y fundido de la caja\n");
    const gfx::PlacedReflectionProbe p = box(1, Vec3{0.0f, 0.0f, 0.0f}, Vec3{10.0f, 4.0f, 10.0f}, 1.0f);
    check(near(gfx::placedProbeWeight(p, Vec3{0.0f, 0.0f, 0.0f}), 1.0f), "en el centro pesa 1");
    check(near(gfx::placedProbeWeight(p, Vec3{4.5f, 0.0f, 0.0f}), 0.5f), "a medio fundido de la cara pesa 0,5");
    check(near(gfx::placedProbeWeight(p, Vec3{0.0f, 1.75f, 0.0f}), 0.25f), "la cara mas cercana manda (techo)");
    check(gfx::placedProbeWeight(p, Vec3{5.01f, 0.0f, 0.0f}) == 0.0f, "fuera de la caja, 0");
    check(near(gfx::placedProbeWeight(p, Vec3{5.0f, 0.0f, 0.0f}), 0.0f), "en el borde, 0");
    const gfx::PlacedReflectionProbe hard = box(2, Vec3{0.0f, 0.0f, 0.0f}, Vec3{2.0f, 2.0f, 2.0f}, 0.0f);
    check(gfx::placedProbeWeight(hard, Vec3{0.99f, 0.0f, 0.0f}) == 1.0f && gfx::placedProbeWeight(hard, Vec3{1.01f, 0.0f, 0.0f}) == 0.0f,
          "sin fundido: 1 dentro, 0 fuera");
}

void projection() {
    std::printf("Proyeccion de caja (paralaje)\n");
    gfx::PlacedReflectionProbe p = box(1, Vec3{0.0f, 0.0f, 0.0f}, Vec3{10.0f, 10.0f, 10.0f});
    check(nearVec(gfx::boxProjectedDirection(p, Vec3{4.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}), Vec3{1.0f, 0.0f, 0.0f}),
          "hacia la pared de enfrente, la misma direccion");
    const Vec3 d = gfx::boxProjectedDirection(p, Vec3{0.0f, 0.0f, 4.0f}, Vec3{1.0f, 0.0f, 0.0f});
    check(nearVec(d, core::normalize(Vec3{5.0f, 0.0f, 4.0f})), "desde un lado: hacia el punto de la pared que ve");
    const Vec3 r = core::normalize(Vec3{1.0f, 1.0f, 0.0f});
    const Vec3 corner = gfx::boxProjectedDirection(p, Vec3{4.0f, 0.0f, 0.0f}, r);
    check(nearVec(corner, core::normalize(Vec3{5.0f, 1.0f, 0.0f})), "sale por la primera pared que cruza");
    // Desde el centro de la sonda la proyeccion no cambia nada.
    check(nearVec(gfx::boxProjectedDirection(p, Vec3{0.0f, 0.0f, 0.0f}, r), r), "desde el centro, sin cambio");
    // Componente casi nula: sin divisiones por cero.
    const Vec3 flat = gfx::boxProjectedDirection(p, Vec3{1.0f, 1.0f, 1.0f}, Vec3{0.0f, 0.0f, -1.0f});
    check(std::isfinite(flat.x) && std::isfinite(flat.y) && std::isfinite(flat.z) &&
              nearVec(flat, core::normalize(Vec3{1.0f, 1.0f, -5.0f})),
          "con componentes a cero, finita y correcta");
    p.box_projection = false;
    check(nearVec(gfx::boxProjectedDirection(p, Vec3{0.0f, 0.0f, 4.0f}, Vec3{1.0f, 0.0f, 0.0f}), Vec3{1.0f, 0.0f, 0.0f}),
          "sin proyeccion de caja, la direccion tal cual");
}

void selection() {
    std::printf("Eleccion de las sondas del frame\n");
    std::vector<gfx::PlacedReflectionProbe> probes;
    probes.push_back(box(1, Vec3{0.0f, 0.0f, 0.0f}, Vec3{40.0f, 10.0f, 40.0f}));         // edificio
    probes.push_back(box(2, Vec3{0.0f, 0.0f, 0.0f}, Vec3{6.0f, 3.0f, 6.0f}));            // habitacion
    probes.push_back(box(3, Vec3{100.0f, 0.0f, 0.0f}, Vec3{6.0f, 3.0f, 6.0f}));          // lejos
    probes.push_back(box(4, Vec3{0.0f, 0.0f, 10.0f}, Vec3{6.0f, 3.0f, 6.0f}, 1.0f, 5));  // prioritaria
    probes.push_back(box(5, Vec3{0.0f, 0.0f, 0.0f}, Vec3{2.0f, 2.0f, 2.0f}));            // sin hornear
    std::vector<gfx::PlacedProbeId> baked = {probes[3].id, probes[0].id, probes[1].id, probes[2].id};
    const auto all = gfx::selectPlacedProbes(probes, baked, Vec3{0.0f, 0.0f, 0.0f});
    check(all.size() == 4, "las horneadas entran; la que no tiene cubo, no");
    if (all.size() == 4) {
        check(all[0].probe == 3 && all[1].probe == 1 && all[2].probe == 2 && all[3].probe == 0,
              "orden de la GPU: prioridad, luego la caja mas pequena");
        check(all[0].layer == 0 && all[1].layer == 2 && all[3].layer == 1, "cada una con la capa de su horneado");
    }
    const auto two = gfx::selectPlacedProbes(probes, baked, Vec3{0.0f, 0.0f, 0.0f}, 2);
    check(two.size() == 2 && ((two[0].probe == 1 && two[1].probe == 0) || (two[0].probe == 0 && two[1].probe == 1)),
          "con tope, las mas cercanas a la camara (la lejana y la de al lado, fuera)");
    probes[1].intensity = 0.0f;
    probes[0].box_max.y = probes[0].box_min.y;  // caja plana
    const auto filtered = gfx::selectPlacedProbes(probes, baked, Vec3{0.0f, 0.0f, 0.0f});
    check(filtered.size() == 2, "sin intensidad o con la caja plana, fuera");
    check(gfx::selectPlacedProbes(probes, {}, Vec3{}).empty(), "sin horneados, ninguna");
    // Estable: mismo resultado en otro orden de entrada.
    std::vector<gfx::PlacedReflectionProbe> twins = {box(9, Vec3{}, Vec3{4.0f, 4.0f, 4.0f}), box(8, Vec3{}, Vec3{4.0f, 4.0f, 4.0f})};
    const auto ta = gfx::selectPlacedProbes(twins, {twins[0].id, twins[1].id}, Vec3{});
    std::swap(twins[0], twins[1]);
    const auto tb = gfx::selectPlacedProbes(twins, {twins[0].id, twins[1].id}, Vec3{});
    check(ta.size() == 2 && tb.size() == 2 && twins[tb[0].probe].id == twins[1 - ta[0].probe].id,
          "a igualdad, el orden lo decide el id (no baila entre frames)");
}

void blending() {
    std::printf("Mezcla de las 2 que mas pesan\n");
    // Ya en el orden de la GPU: habitacion (prioridad 1) y edificio.
    std::vector<gfx::PlacedReflectionProbe> ordered = {box(2, Vec3{}, Vec3{6.0f, 4.0f, 6.0f}, 1.0f, 1),
                                                       box(1, Vec3{}, Vec3{40.0f, 10.0f, 40.0f}, 2.0f, 0)};
    gfx::PlacedProbeBlend b = gfx::blendPlacedProbes(ordered, Vec3{0.0f, 0.0f, 0.0f});
    check(b.first == 0 && near(b.first_weight, 1.0f) && b.second_weight == 0.0f,
          "dentro de la habitacion manda ella sola (aunque el edificio tambien pese 1)");
    b = gfx::blendPlacedProbes(ordered, Vec3{2.5f, 0.0f, 0.0f});
    check(b.first == 0 && b.second == 1 && near(b.first_weight, 0.5f) && near(b.second_weight, 0.5f) &&
              near(b.total(), 1.0f),
          "en su borde de fundido, el edificio rellena lo que falta");
    b = gfx::blendPlacedProbes(ordered, Vec3{10.0f, 0.0f, 0.0f});
    check(b.first == 1 && b.second == -1 && near(b.first_weight, 1.0f), "fuera de la habitacion, solo el edificio");
    b = gfx::blendPlacedProbes(ordered, Vec3{19.5f, 0.0f, 0.0f});
    check(b.first == 1 && near(b.first_weight, 0.25f) && near(b.total(), 0.25f),
          "en el borde del edificio, el resto para la sonda de la camara");
    b = gfx::blendPlacedProbes(ordered, Vec3{30.0f, 0.0f, 0.0f});
    check(b.first == -1 && b.total() == 0.0f, "fuera de todas, nada");
    // Misma prioridad y una dentro de otra (habitacion en un edificio, la
    // pequena delante, como las ordena selectPlacedProbes): la habitacion
    // entra fundiendose en su borde. Antes mandaba la de mas peso y el
    // edificio (peso 1) tapaba todo el fundido: la habitacion salia de golpe.
    std::vector<gfx::PlacedReflectionProbe> nested = {box(5, Vec3{}, Vec3{6.0f, 4.0f, 6.0f}, 1.0f),
                                                      box(6, Vec3{}, Vec3{40.0f, 10.0f, 40.0f}, 2.0f)};
    b = gfx::blendPlacedProbes(nested, Vec3{2.75f, 0.0f, 0.0f});
    check(b.first == 0 && b.second == 1 && near(b.first_weight, 0.25f) && near(b.second_weight, 0.75f),
          "misma prioridad, una dentro de otra: la pequena se funde en su borde");
    float previous = 0.0f;
    bool continuous = true;
    for (int step = 0; step <= 40; ++step) {
        const float x = 3.0f - 0.05f * static_cast<float>(step);  // del borde de la habitacion hacia dentro
        const gfx::PlacedProbeBlend m = gfx::blendPlacedProbes(nested, Vec3{x, 0.0f, 0.0f});
        const float room = m.first == 0 ? m.first_weight : (m.second == 0 ? m.second_weight : 0.0f);
        if (room + 1e-4f < previous || room - previous > 0.06f || !near(m.total(), 1.0f)) continuous = false;
        previous = room;
    }
    check(continuous && near(previous, 1.0f), "el peso de la habitacion crece sin saltos y el total es 1");
    // Misma prioridad, dos que se solapan de lado: cada una con su peso.
    std::vector<gfx::PlacedReflectionProbe> same = {box(3, Vec3{-2.0f, 0.0f, 0.0f}, Vec3{6.0f, 4.0f, 6.0f}, 2.0f),
                                                    box(4, Vec3{2.0f, 0.0f, 0.0f}, Vec3{6.0f, 4.0f, 6.0f}, 2.0f)};
    b = gfx::blendPlacedProbes(same, Vec3{0.5f, 0.0f, 0.0f});
    check(b.first == 0 && b.second == 1 && near(b.first_weight, 0.25f) && near(b.second_weight, 0.75f),
          "a igual prioridad, de lado: cada una con su peso");
    b = gfx::blendPlacedProbes(same, Vec3{0.0f, 0.0f, 0.0f});
    check(b.first == 0 && near(b.first_weight, 0.5f) && near(b.second_weight, 0.5f), "en medio, mitad y mitad");
    // Tres: la primera pesa poco, de las otras rellena la que mas pesa.
    std::vector<gfx::PlacedReflectionProbe> three = {box(7, Vec3{-2.0f, 0.0f, 0.0f}, Vec3{6.0f, 4.0f, 6.0f}, 2.0f, 1),
                                                     box(8, Vec3{3.0f, 0.0f, 0.0f}, Vec3{6.0f, 4.0f, 6.0f}, 2.0f),
                                                     box(9, Vec3{}, Vec3{40.0f, 10.0f, 40.0f}, 2.0f)};
    b = gfx::blendPlacedProbes(three, Vec3{0.5f, 0.0f, 0.0f});
    check(b.first == 0 && b.second == 2 && near(b.first_weight, 0.25f) && near(b.second_weight, 0.75f),
          "la segunda es la que mas pesa de las demas (no la siguiente de la lista)");
}

void packing() {
    std::printf("Empaquetado para la GPU (std140)\n");
    gfx::PlacedReflectionProbe p = box(1, Vec3{1.0f, 2.0f, 3.0f}, Vec3{4.0f, 6.0f, 8.0f}, 0.5f, -3);
    p.intensity = 1.5f;
    p.box_projection = false;
    const gfx::GpuPlacedProbe g = gfx::packPlacedProbe(p, 7, Vec3{1.0f, 2.5f, 3.0f});
    check(g.center_intensity.x == 1.0f && g.center_intensity.y == 2.5f && g.center_intensity.w == 1.5f,
          "centro (el del horneado) e intensidad");
    check(g.box_min_blend.x == -1.0f && g.box_min_blend.y == -1.0f && g.box_min_blend.z == -1.0f &&
              g.box_min_blend.w == 0.5f,
          "esquina menor y fundido");
    check(g.box_max_priority.x == 3.0f && g.box_max_priority.y == 5.0f && g.box_max_priority.z == 7.0f &&
              g.box_max_priority.w == -3.0f,
          "esquina mayor y prioridad");
    check(g.params.x == 7.0f && g.params.y == 0.0f, "capa y proyeccion de caja");
    // GpuLights: lo nuevo va al final, alineado a 16 (uvec4 y PlacedProbeGpu
    // en std140), sin mover nada de lo de antes.
    check(sizeof(gfx::GpuPlacedProbe) == 64, "PlacedProbeGpu: 64 bytes");
    check(offsetof(gfx::GpuLights, placed_info) ==
              offsetof(gfx::GpuLights, point_tiles) + sizeof(std::uint32_t) * gfx::kMaxLightTiles,
          "placed_info justo detras de point_tiles");
    check(offsetof(gfx::GpuLights, placed_info) % 16 == 0 && offsetof(gfx::GpuLights, placed) % 16 == 0 &&
              offsetof(gfx::GpuLights, placed) == offsetof(gfx::GpuLights, placed_info) + 16,
          "alineados a 16 como en std140");
    check(sizeof(gfx::GpuLights) == offsetof(gfx::GpuLights, placed) + 64 * gfx::kMaxActivePlacedProbes,
          "GpuLights termina en las sondas");
    check(sizeof(gfx::GpuLights) <= 16384, "GpuLights cabe en el minimo de un uniform buffer (16 KB)");
    check(offsetof(gfx::GpuLights, probes) == 6 * 16 + 32 * 48 + 8 * 64, "lo de antes no se mueve (probes)");
    gfx::GpuLights fresh{};
    check(fresh.placed_info[0] == 0, "por defecto, ninguna sonda colocada (como antes)");
    check(sizeof(gfx::GpuBloomPush) == 16, "push del GTAO: 16 bytes (de 128)");
}

void file() {
    std::printf("Archivo .crrefl\n");
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "cramion_reflection_probe_tests";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    const std::filesystem::path scene = root / "Nivel.crscene";
    const std::filesystem::path path = lighting::reflectionProbesFile(scene);
    check(path.filename() == "Nivel.crrefl", "junto a la escena, con su nombre");

    std::vector<gfx::BakedReflectionCube> out = {cube(1, 16, 3, 100), cube(2, 8, 4, 7)};
    out[1].position = Vec3{-4.0f, 0.5f, 9.0f};
    std::string error;
    check(lighting::saveReflectionProbes(path, out, &error) && std::filesystem::exists(path), "guardar");
    check(!std::filesystem::exists(path.string() + ".tmp"), "sin el temporal");
    std::vector<gfx::BakedReflectionCube> back;
    check(lighting::loadReflectionProbes(path, back, &error), "leer");
    bool same = back.size() == out.size();
    for (std::size_t i = 0; same && i < back.size(); ++i) {
        same = back[i].id == out[i].id && back[i].size == out[i].size && back[i].mips == out[i].mips &&
               nearVec(back[i].position, out[i].position, 0.0f) && back[i].texels == out[i].texels;
    }
    check(same, "ida y vuelta: ids, posicion, tamano, mips y halfs iguales");

    // Danado: un byte de los halfs cambiado (FNV), cortado, version futura y magia mala.
    const auto bytes = [&] {
        std::ifstream in(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }();
    const auto write = [&](const std::string& data) {
        std::ofstream o(path, std::ios::binary | std::ios::trunc);
        o.write(data.data(), static_cast<std::streamsize>(data.size()));
    };
    std::string flipped = bytes;
    flipped[flipped.size() - 3] ^= 0x5A;
    write(flipped);
    check(!lighting::loadReflectionProbes(path, back, &error) && back.empty() &&
              error.find("danado") != std::string::npos,
          "un half cambiado: danado y sin cubos");
    write(bytes.substr(0, bytes.size() / 2));
    check(!lighting::loadReflectionProbes(path, back, &error) && back.empty(), "cortado: danado");
    std::string future = bytes;
    future[4] = 99;
    write(future);
    check(!lighting::loadReflectionProbes(path, back, &error), "version futura: se rechaza");
    std::string magic = bytes;
    magic[0] = 'X';
    write(magic);
    check(!lighting::loadReflectionProbes(path, back, &error), "magia mala: se rechaza");
    // Un recuento enorme no reserva memoria.
    std::string huge = bytes;
    huge[8] = '\xff';
    huge[9] = '\xff';
    huge[10] = '\xff';
    huge[11] = '\x7f';
    write(huge);
    check(!lighting::loadReflectionProbes(path, back, &error), "recuento absurdo: se rechaza");
    check(!lighting::loadReflectionProbes(root / "NoExiste.crrefl", back, &error), "sin archivo: false");

    // Los cubos que no valen no se guardan; sin ninguno, el archivo se borra.
    write(bytes);
    std::vector<gfx::BakedReflectionCube> bad = {cube(3, 8, 2, 1)};
    bad[0].texels.resize(5);
    bad.push_back(cube(0, 8, 2, 1));
    bad.back().id = gfx::PlacedProbeId{};
    check(lighting::saveReflectionProbes(path, bad, &error) && !std::filesystem::exists(path),
          "sin cubos validos, se borra el archivo");
    std::filesystem::remove_all(root, ec);
}

void component() {
    std::printf("Componente ReflectionProbeBox\n");
    lighting::registerLightingComponents();
    check(ecs::ComponentRegistry::instance().find("ReflectionProbeBox") != nullptr,
          "registrado con los de iluminacion");
    ecs::World world;
    ecs::Entity e = world.create("Sonda del salon");
    e.setWorldPosition(Vec3{3.0f, 1.5f, -2.0f});
    lighting::ReflectionProbeBox& p = e.add<lighting::ReflectionProbeBox>();
    p.size = Vec3{8.0f, 3.0f, 6.0f};
    p.blend_distance = 0.75f;
    p.priority = 4;
    p.intensity = 1.25f;
    p.box_projection = false;

    const gfx::PlacedReflectionProbe placed = lighting::placedProbeOf(p, e.uuid(), e.worldPosition());
    check(nearVec(placed.box_min, Vec3{-1.0f, 0.0f, -5.0f}) && nearVec(placed.box_max, Vec3{7.0f, 3.0f, 1.0f}),
          "la caja centrada en el objeto");
    check(placed.id == lighting::placedProbeId(e.uuid()) && placed.priority == 4 && !placed.box_projection,
          "id del objeto, prioridad y proyeccion");
    lighting::ReflectionProbeBox negative = p;
    negative.size = Vec3{-2.0f, 0.0f, 4.0f};
    const gfx::PlacedReflectionProbe fixed = lighting::placedProbeOf(negative, e.uuid(), Vec3{});
    check(fixed.box_max.x - fixed.box_min.x == 2.0f && fixed.box_max.y > fixed.box_min.y,
          "tamano negativo o cero: se arregla (nunca caja vacia)");

    const std::string json = ecs::serializeWorld(world);
    check(json.find("ReflectionProbeBox") != std::string::npos && json.find("blend_distance") != std::string::npos,
          "se guarda en la escena");
    ecs::World loaded;
    std::string error;
    check(ecs::deserializeWorld(loaded, json, &error), "y se vuelve a leer");
    ecs::Entity again = loaded.find(e.uuid());
    const lighting::ReflectionProbeBox* q = again.valid() ? again.tryGet<lighting::ReflectionProbeBox>() : nullptr;
    check(q != nullptr && nearVec(q->size, p.size) && near(q->blend_distance, 0.75f) && q->priority == 4 &&
              near(q->intensity, 1.25f) && !q->box_projection,
          "con todos sus campos");

    // Una escena de antes (sin el componente) sigue leyendose igual.
    ecs::World old_world;
    old_world.create("Suelo").setWorldPosition(Vec3{0.0f, -1.0f, 0.0f});
    ecs::World old_loaded;
    check(ecs::deserializeWorld(old_loaded, ecs::serializeWorld(old_world), &error) &&
              old_loaded.registry().view<lighting::ReflectionProbeBox>().size() == 0,
          "escena sin sondas: ninguna");
    // Campos que faltan (un archivo de otra version): los de por defecto.
    ecs::World partial;
    ecs::Entity bare = partial.create("Sonda");
    bare.add<lighting::ReflectionProbeBox>();
    std::string text = ecs::serializeWorld(partial);
    const std::size_t at = text.find("\"priority\"");
    if (at != std::string::npos) {
        const std::size_t end = text.find('\n', at);
        text.erase(at, end - at + 1);
    }
    ecs::World partial_loaded;
    check(ecs::deserializeWorld(partial_loaded, text, &error), "sin un campo se lee igual");
    ecs::Entity bare_again = partial_loaded.find(bare.uuid());
    const lighting::ReflectionProbeBox* r =
        bare_again.valid() ? bare_again.tryGet<lighting::ReflectionProbeBox>() : nullptr;
    check(r != nullptr && r->priority == 0 && r->box_projection && near(r->blend_distance, 1.0f),
          "y lo que falta queda por defecto");
}

}  // namespace

int main() {
    sizes();
    weights();
    projection();
    selection();
    blending();
    packing();
    file();
    component();
    std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

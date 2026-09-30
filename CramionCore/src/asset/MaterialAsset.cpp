#include "CramionCore/asset/MaterialAsset.h"

#include "CrData.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>

namespace cramion::assets {

namespace {

using json = nlohmann::json;

json vec(const core::Vec2& v) { return json::array({v.x, v.y}); }
json vec(const core::Vec3& v) { return json::array({v.x, v.y, v.z}); }
json vec(const core::Vec4& v) { return json::array({v.x, v.y, v.z, v.w}); }

float number(const json& j, const char* key, float fallback) {
    const auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->get<float>() : fallback;
}

template <std::size_t N>
std::array<float, N> numbers(const json& j, const char* key, std::array<float, N> fallback) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array() || it->size() != N) return fallback;
    for (std::size_t i = 0; i < N; ++i) {
        if ((*it)[i].is_number()) fallback[i] = (*it)[i].get<float>();
    }
    return fallback;
}

std::string text(const json& j, const char* key) {
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::uint64_t mix(std::uint64_t h, std::uint64_t v) {
    return (h ^ v) * 1099511628211ull;
}

std::uint64_t mixText(std::uint64_t h, const std::string& s) {
    for (unsigned char c : s) h = mix(h, c);
    return mix(h, 0xFF);
}

std::uint64_t mixFloat(std::uint64_t h, float f) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(f));
    std::memcpy(&bits, &f, sizeof(f));
    return mix(h, bits);
}

}  // namespace

bool loadMaterial(const std::filesystem::path& path, MaterialAsset& out, std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "no se pudo abrir " + crdata::utf8(path);
        return false;
    }
    const json j = json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) {
        if (error) *error = "JSON danado en " + crdata::utf8(path);
        return false;
    }
    MaterialAsset m;
    m.uuid = Uuid::parse(text(j, "uuid"));
    m.mode = text(j, "mode") == "transparent" ? MaterialMode::Transparent : MaterialMode::Opaque;
    const auto color = numbers<4>(j, "base_color", {1.0f, 1.0f, 1.0f, 1.0f});
    m.base_color = core::Vec4{color[0], color[1], color[2], color[3]};
    m.metallic = number(j, "metallic", m.metallic);
    m.roughness = number(j, "roughness", m.roughness);
    m.reflectance = number(j, "reflectance", m.reflectance);
    m.normal_strength = number(j, "normal_strength", m.normal_strength);
    m.normal_directx = j.value("normal_directx", false);
    m.occlusion_strength = number(j, "occlusion_strength", m.occlusion_strength);
    const auto emissive = numbers<3>(j, "emissive", {0.0f, 0.0f, 0.0f});
    m.emissive = core::Vec3{emissive[0], emissive[1], emissive[2]};
    m.emissive_intensity = number(j, "emissive_intensity", m.emissive_intensity);
    const auto tiling = numbers<2>(j, "tiling", {1.0f, 1.0f});
    m.tiling = core::Vec2{tiling[0], tiling[1]};
    const auto offset = numbers<2>(j, "offset", {0.0f, 0.0f});
    m.offset = core::Vec2{offset[0], offset[1]};
    m.albedo = text(j, "albedo");
    m.normal = text(j, "normal");
    m.shader = text(j, "shader");
    if (const auto values = j.find("shader_values"); values != j.end() && values->is_object()) {
        for (const auto& [name, v] : values->items()) {
            if (!v.is_array()) continue;
            core::Vec4 value{};
            float* out = &value.x;
            for (std::size_t i = 0; i < v.size() && i < 4; ++i) {
                if (v[i].is_number()) out[i] = v[i].get<float>();
            }
            m.shader_values[name] = value;
        }
    }
    if (const auto textures = j.find("shader_textures"); textures != j.end() && textures->is_object()) {
        for (const auto& [name, v] : textures->items()) {
            if (v.is_string()) m.shader_textures[name] = v.get<std::string>();
        }
    }
    m.metallic_map = text(j, "metallic_map");
    m.roughness_map = text(j, "roughness_map");
    m.occlusion = text(j, "occlusion");
    m.emissive_map = text(j, "emissive_map");
    m.height_map = text(j, "height_map");
    m.cavity_map = text(j, "cavity_map");
    m.specular_map = text(j, "specular_map");
    m.gloss_map = text(j, "gloss_map");
    m.bump_map = text(j, "bump_map");
    m.height_scale = number(j, "height_scale", m.height_scale);
    m.cavity_strength = number(j, "cavity_strength", m.cavity_strength);
    m.relief = text(j, "relief") == "tessellation" ? ReliefMode::Tessellation : ReliefMode::Parallax;
    m.tessellation_density = std::clamp(number(j, "tessellation_density", m.tessellation_density), 1.0f, 64.0f);
    if (const auto shadows = j.find("parallax_shadows"); shadows != j.end() && shadows->is_boolean()) {
        m.parallax_shadows = shadows->get<bool>();
    }
    m.shading = shadingModelFromKey(text(j, "shading"));
    m.specular_tint = std::clamp(number(j, "specular_tint", m.specular_tint), 0.0f, 1.0f);
    m.clearcoat = std::clamp(number(j, "clearcoat", m.clearcoat), 0.0f, 1.0f);
    m.clearcoat_roughness = std::clamp(number(j, "clearcoat_roughness", m.clearcoat_roughness), 0.0f, 1.0f);
    m.sheen = std::clamp(number(j, "sheen", m.sheen), 0.0f, 1.0f);
    m.sheen_tint = std::clamp(number(j, "sheen_tint", m.sheen_tint), 0.0f, 1.0f);
    m.subsurface = std::clamp(number(j, "subsurface", m.subsurface), 0.0f, 1.0f);
    m.translucency = std::clamp(number(j, "translucency", m.translucency), 0.0f, 1.0f);
    m.subsurface_thickness = std::clamp(number(j, "subsurface_thickness", m.subsurface_thickness), 0.01f, 0.3f);
    m.anisotropy = std::clamp(number(j, "anisotropy", m.anisotropy), 0.0f, 1.0f);
    m.anisotropy_rotation = std::clamp(number(j, "anisotropy_rotation", m.anisotropy_rotation), 0.0f, 180.0f);
    m.ior = std::clamp(number(j, "ior", m.ior), 1.0f, 2.5f);
    m.transmission_thickness = std::clamp(number(j, "transmission_thickness", m.transmission_thickness), 0.0f, 0.2f);
    out = std::move(m);
    return true;
}

bool saveMaterial(MaterialAsset& m, const std::filesystem::path& path, std::string* error) {
    if (!m.uuid.valid()) m.uuid = Uuid::generate();
    json j;
    j["uuid"] = m.uuid.toString();
    j["type"] = "material";
    j["version"] = 1;
    j["mode"] = m.mode == MaterialMode::Transparent ? "transparent" : "opaque";
    j["base_color"] = vec(m.base_color);
    j["metallic"] = m.metallic;
    j["roughness"] = m.roughness;
    j["reflectance"] = m.reflectance;
    j["normal_strength"] = m.normal_strength;
    j["normal_directx"] = m.normal_directx;
    j["occlusion_strength"] = m.occlusion_strength;
    j["emissive"] = vec(m.emissive);
    j["emissive_intensity"] = m.emissive_intensity;
    j["tiling"] = vec(m.tiling);
    j["offset"] = vec(m.offset);
    j["albedo"] = m.albedo;
    j["normal"] = m.normal;
    j["metallic_map"] = m.metallic_map;
    j["roughness_map"] = m.roughness_map;
    j["occlusion"] = m.occlusion;
    j["emissive_map"] = m.emissive_map;
    j["height_map"] = m.height_map;
    j["cavity_map"] = m.cavity_map;
    j["specular_map"] = m.specular_map;
    j["gloss_map"] = m.gloss_map;
    j["bump_map"] = m.bump_map;
    j["height_scale"] = m.height_scale;
    j["cavity_strength"] = m.cavity_strength;
    j["relief"] = m.relief == ReliefMode::Tessellation ? "tessellation" : "parallax";
    j["tessellation_density"] = m.tessellation_density;
    j["parallax_shadows"] = m.parallax_shadows;
    j["shading"] = shadingModelKey(m.shading);
    j["specular_tint"] = m.specular_tint;
    j["clearcoat"] = m.clearcoat;
    j["clearcoat_roughness"] = m.clearcoat_roughness;
    j["sheen"] = m.sheen;
    j["sheen_tint"] = m.sheen_tint;
    j["subsurface"] = m.subsurface;
    j["translucency"] = m.translucency;
    j["subsurface_thickness"] = m.subsurface_thickness;
    j["anisotropy"] = m.anisotropy;
    j["anisotropy_rotation"] = m.anisotropy_rotation;
    j["ior"] = m.ior;
    j["transmission_thickness"] = m.transmission_thickness;
    if (!m.shader.empty()) {
        j["shader"] = m.shader;
        json values = json::object();
        for (const auto& [name, v] : m.shader_values) values[name] = vec(v);
        j["shader_values"] = values;
        json textures = json::object();
        for (const auto& [name, path] : m.shader_textures) textures[name] = path;
        j["shader_textures"] = textures;
    }

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error) *error = "no se pudo escribir " + crdata::utf8(path);
        return false;
    }
    out << j.dump(2);
    return static_cast<bool>(out);
}

std::uint64_t materialStructureHash(const MaterialAsset& m) {
    std::uint64_t h = 1469598103934665603ull;
    h = mix(h, static_cast<std::uint64_t>(m.mode));
    for (const std::string* s : {&m.albedo, &m.normal, &m.metallic_map, &m.roughness_map, &m.occlusion, &m.emissive_map,
                                 &m.height_map, &m.cavity_map, &m.specular_map, &m.gloss_map, &m.bump_map}) {
        h = mixText(h, *s);
    }
    // La cavidad se hornea en la textura con su fuerza.
    h = mixFloat(h, m.cavity_map.empty() ? 0.0f : m.cavity_strength);
    // Teselado sin normal map ni bump: la altura hace de normal map.
    h = mix(h, m.relief == ReliefMode::Tessellation && m.normal.empty() && m.bump_map.empty() ? 1u : 0u);
    h = mixFloat(h, m.tiling.x);
    h = mixFloat(h, m.tiling.y);
    h = mixFloat(h, m.offset.x);
    h = mixFloat(h, m.offset.y);
    // Emision sin mapa: el shader usa blanco o negro segun si emite.
    const bool emits = m.emissive.x > 0.0f || m.emissive.y > 0.0f || m.emissive.z > 0.0f;
    h = mix(h, emits ? 1u : 0u);
    // Shader propio y sus texturas (los valores se cambian en vivo).
    h = mixText(h, m.shader);
    for (const auto& [name, path] : m.shader_textures) {
        h = mixText(h, name);
        h = mixText(h, path);
    }
    return h;
}

namespace {
constexpr const char* kShadingKeys[kShadingModelCount] = {"standard",   "clearcoat",   "cloth",
                                                          "subsurface", "anisotropic", "transmission"};
}  // namespace

const char* shadingModelKey(ShadingModel model) {
    const int index = static_cast<int>(model);
    return index >= 0 && index < kShadingModelCount ? kShadingKeys[index] : kShadingKeys[0];
}

ShadingModel shadingModelFromKey(const std::string& key) {
    for (int i = 0; i < kShadingModelCount; ++i) {
        if (key == kShadingKeys[i]) return static_cast<ShadingModel>(i);
    }
    return ShadingModel::Standard;
}

asset::MaterialData toMaterialData(const MaterialAsset& m, const std::string& name) {
    asset::MaterialData d;
    d.name = name;
    d.base_color = m.base_color;
    d.emissive = m.emissive * std::max(m.emissive_intensity, 0.0f);
    d.metallic = m.metallic;
    d.roughness = m.roughness;
    d.occlusion_strength = m.occlusion_strength;
    d.normal_scale = m.normal_strength;
    d.normal_map_directx = m.normal_directx;
    d.reflectance = m.reflectance;
    d.transparent = m.mode == MaterialMode::Transparent;
    d.height_scale = m.height_map.empty() ? 0.0f : std::max(m.height_scale, 0.0f);
    d.tessellation = m.relief == ReliefMode::Tessellation;
    d.tessellation_density = m.tessellation_density;
    d.parallax_shadows = m.parallax_shadows;
    d.shading_model = static_cast<int>(m.shading);
    d.specular_tint = m.specular_tint;
    d.clearcoat = m.clearcoat;
    d.clearcoat_roughness = m.clearcoat_roughness;
    d.sheen = m.sheen;
    d.sheen_tint = m.sheen_tint;
    d.subsurface = m.subsurface;
    d.translucency = m.translucency;
    d.subsurface_thickness = m.subsurface_thickness;
    d.anisotropy = m.anisotropy;
    d.anisotropy_rotation = m.anisotropy_rotation;
    d.ior = m.ior;
    d.transmission_thickness = m.transmission_thickness;
    d.specular_map = !m.specular_map.empty();
    return d;
}

MaterialAsset materialFromImage(const std::filesystem::path& assets_root, const std::string& image) {
    MaterialAsset m;
    m.albedo = image;
    const std::filesystem::path file = assets_root / crdata::fromUtf8(image);
    const std::filesystem::path folder = file.parent_path();

    // Nombre base: sin el sufijo de color (piedra_albedo -> piedra).
    std::string stem = lower(crdata::utf8(file.stem()));
    for (const char* suffix : {"_basecolor", "_base_color", "_albedo", "_diffuse", "_color", "_col", "_diff", "_d"}) {
        const std::string s(suffix);
        if (stem.size() > s.size() && stem.compare(stem.size() - s.size(), s.size(), s) == 0) {
            stem.resize(stem.size() - s.size());
            break;
        }
    }

    struct Slot {
        std::string* target;
        std::initializer_list<const char*> suffixes;
    };
    const Slot slots[] = {
        {&m.normal, {"_normal", "_normalgl", "_normal_gl", "_nor_gl", "_nrm", "_nor", "_norm", "_n"}},
        {&m.roughness_map, {"_roughness", "_rough", "_rgh", "_r"}},
        {&m.metallic_map, {"_metallic", "_metalness", "_metal", "_met", "_m"}},
        {&m.occlusion, {"_ao", "_occlusion", "_ambientocclusion", "_ambient_occlusion"}},
        {&m.emissive_map, {"_emissive", "_emission", "_emit", "_e"}},
        {&m.height_map, {"_displacement", "_height", "_disp", "_heightmap", "_parallax", "_h"}},
        {&m.cavity_map, {"_cavity", "_cav"}},
        {&m.specular_map, {"_specular", "_spec", "_specularlevel"}},
        {&m.gloss_map, {"_gloss", "_glossiness", "_smoothness"}},
        {&m.bump_map, {"_bump", "_bumpmap"}},
    };
    std::error_code ec;
    for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string ext = lower(it->path().extension().string());
        if (ext != ".png" && ext != ".jpg" && ext != ".jpeg" && ext != ".tga" && ext != ".bmp") continue;
        const std::string name = lower(crdata::utf8(it->path().stem()));
        if (name.size() <= stem.size() || name.compare(0, stem.size(), stem) != 0) continue;
        const std::string suffix = name.substr(stem.size());
        for (const Slot& slot : slots) {
            if (!slot.target->empty()) continue;
            if (std::find_if(slot.suffixes.begin(), slot.suffixes.end(),
                             [&](const char* s) { return suffix == s; }) != slot.suffixes.end()) {
                *slot.target = crdata::utf8(std::filesystem::relative(it->path(), assets_root, ec));
            }
        }
        if (m.normal.empty() && (suffix == "_normaldx" || suffix == "_normal_dx" || suffix == "_nor_dx")) {
            m.normal = crdata::utf8(std::filesystem::relative(it->path(), assets_root, ec));
            m.normal_directx = true;
        }
    }
    for (std::string* path : {&m.albedo, &m.normal, &m.roughness_map, &m.metallic_map, &m.occlusion, &m.emissive_map,
                              &m.height_map, &m.cavity_map, &m.specular_map, &m.gloss_map, &m.bump_map}) {
        std::replace(path->begin(), path->end(), '\\', '/');
    }
    if (!m.gloss_map.empty() && m.roughness_map.empty()) m.roughness = 1.0f;
    // Con mapa, el factor es el maximo (el mapa decide).
    if (!m.roughness_map.empty()) m.roughness = 1.0f;
    if (!m.metallic_map.empty()) m.metallic = 1.0f;
    if (!m.emissive_map.empty()) m.emissive = core::Vec3{1.0f, 1.0f, 1.0f};
    return m;
}

}  // namespace cramion::assets

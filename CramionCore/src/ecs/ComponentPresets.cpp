#include "CramionCore/ecs/ComponentPresets.h"

#include "CramionCore/ecs/Reflection.h"
#include "CramionCore/ecs/SceneSerializer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

namespace cramion::ecs {

using json = nlohmann::json;

namespace {

// Todas las secciones de un volumen: un preset tambien sirve en un volumen
// local (sin "sobrescribir" sus secciones saldrian de los globales).
const char* const kPostOverrides[] = {"override_exposure", "override_tonemapping", "override_bloom",
                                      "override_color",    "override_vignette",    "override_lens",
                                      "override_motion_blur", "override_depth_of_field", "override_light_shafts",
                                      "override_effects"};

ComponentPreset post(const char* name, const char* description, json fields) {
    for (const char* key : kPostOverrides) fields[key] = true;
    ComponentPreset p;
    p.name = name;
    p.component = "PostProcessing";
    p.description = description;
    p.fields = fields.dump();
    p.builtin = true;
    return p;
}

std::vector<ComponentPreset> postProcessingPresets() {
    std::vector<ComponentPreset> out;
    out.push_back(post("Por defecto", "Los valores de fabrica del motor", json::object()));
    out.push_back(post("Realista", "Neutro y fotografico: color fiel, poco efecto, motion blur de camara",
                       {{"tonemapper", "PBR Neutral"}, {"auto_exposure", true}, {"bloom", true},
                        {"bloom_intensity", 0.04}, {"bloom_threshold", 0.6}, {"contrast", 1.03},
                        {"saturation", 1.0}, {"vibrance", 0.1}, {"vignette", true}, {"vignette_intensity", 0.2},
                        {"film_grain", 0.04}, {"lens_flare", 0.25}, {"motion_blur", true},
                        {"motion_blur_intensity", 0.5}, {"volumetric_light", true}}));
    // Todo prendido con valores de camara real: cada efecto sutil para que
    // sume sin notarse como filtro. Es el mas caro de la lista.
    json ultra = {{"auto_exposure", true}, {"exposure_compensation", 0.0}, {"min_ev", -3.0}, {"max_ev", 2.5},
                  {"adaptation_speed_up", 2.5}, {"adaptation_speed_down", 0.8}, {"tonemapper", "ACES"},
                  {"bloom", true}, {"bloom_intensity", 0.05}, {"bloom_threshold", 0.8}, {"bloom_scatter", 1.2},
                  {"contrast", 1.05}, {"saturation", 1.0}, {"vibrance", 0.1}, {"vignette", true},
                  {"vignette_intensity", 0.22}, {"vignette_smoothness", 0.6}, {"chromatic_aberration", 0.05},
                  {"film_grain", 0.03}, {"lens_distortion", 0.02}, {"lens_flare", 0.3}, {"motion_blur", true},
                  {"motion_blur_intensity", 0.5}, {"motion_blur_max", 0.04}, {"depth_of_field", true},
                  {"dof_auto_focus", true}, {"dof_aperture", 4.0}, {"dof_focal_length", 35.0},
                  {"light_shafts", true}, {"light_shaft_intensity", 1.0}, {"fxaa", true},
                  {"ambient_occlusion", true}, {"global_illumination", true}, {"reflections", true},
                  {"contact_shadows", true}, {"contact_shadow_length", 1.0}, {"volumetric_light", true},
                  {"volumetric_density", 0.025}, {"volumetric_anisotropy", 0.7}, {"fog_density", 0.0008},
                  {"fog_height_falloff", 0.06}, {"lods", true}, {"lod_pixel_error", 0.5},
                  {"override_antialiasing", true}, {"override_performance", true}};
    out.push_back(post("Ultra realista",
                       "Todos los efectos prendidos con valores de camara real: GI, reflejos, SSAO, volumetrica, "
                       "bokeh, motion blur y LODs casi sin error. El mas pesado",
                       ultra));
    out.push_back(post("Cinematografico", "ACES, sombras verdeazuladas y luces calidas, grano, bokeh y destellos",
                       {{"tonemapper", "ACES"}, {"contrast", 1.15}, {"saturation", 0.95}, {"vibrance", 0.15},
                        {"temperature", 8.0}, {"lift", {0.0, 0.012, 0.025}}, {"gain", {1.03, 1.0, 0.95}},
                        {"bloom", true}, {"bloom_intensity", 0.08}, {"vignette", true}, {"vignette_intensity", 0.4},
                        {"film_grain", 0.18}, {"chromatic_aberration", 0.12}, {"lens_distortion", 0.06},
                        {"lens_flare", 0.6}, {"motion_blur", true}, {"motion_blur_intensity", 0.5},
                        {"depth_of_field", true}, {"dof_auto_focus", true}, {"dof_aperture", 2.8},
                        {"dof_focal_length", 50.0}}));
    out.push_back(post("Dia soleado", "Colores vivos, sol con rayos y destellos, contraste alegre",
                       {{"tonemapper", "PBR Neutral"}, {"temperature", 10.0}, {"saturation", 1.15},
                        {"vibrance", 0.3}, {"contrast", 1.1}, {"bloom", true}, {"bloom_intensity", 0.05},
                        {"light_shafts", true}, {"light_shaft_intensity", 1.3}, {"lens_flare", 0.8},
                        {"vignette", true}, {"vignette_intensity", 0.2}, {"fog_density", 0.0012}}));
    out.push_back(post("Atardecer", "Luz calida y dorada, rayos de sol y polvo en el aire",
                       {{"tonemapper", "ACES"}, {"temperature", 35.0}, {"tint", 5.0}, {"saturation", 1.1},
                        {"gain", {1.08, 1.0, 0.9}}, {"bloom", true}, {"bloom_intensity", 0.1},
                        {"light_shafts", true}, {"light_shaft_intensity", 1.6}, {"lens_flare", 1.0},
                        {"volumetric_light", true}, {"volumetric_density", 0.035}, {"vignette", true},
                        {"vignette_intensity", 0.3}}));
    out.push_back(post("Noche de luna", "Azul y apagada, luces que brillan, grano de camara",
                       {{"tonemapper", "ACES"}, {"temperature", -25.0}, {"tint", 5.0}, {"saturation", 0.6},
                        {"contrast", 1.1}, {"exposure_compensation", -0.7}, {"color_filter", {0.85, 0.92, 1.1}},
                        {"bloom", true}, {"bloom_intensity", 0.1}, {"vignette", true}, {"vignette_intensity", 0.45},
                        {"vignette_color", {0.0, 0.01, 0.03}}, {"film_grain", 0.15}}));
    out.push_back(post("Terror", "Desaturado y verdoso, mucha vineta, grano y aberracion, niebla densa",
                       {{"tonemapper", "ACES"}, {"saturation", 0.45}, {"contrast", 1.25}, {"temperature", -10.0},
                        {"tint", -8.0}, {"exposure_compensation", -0.8}, {"vignette", true},
                        {"vignette_intensity", 0.6}, {"vignette_smoothness", 0.35}, {"film_grain", 0.4},
                        {"chromatic_aberration", 0.35}, {"lens_distortion", 0.12}, {"motion_blur", true},
                        {"motion_blur_intensity", 0.7}, {"volumetric_light", true}, {"volumetric_density", 0.05},
                        {"fog_density", 0.01}}));
    out.push_back(post("Tormenta", "Gris y frio, contraste duro y aire cargado",
                       {{"tonemapper", "ACES"}, {"saturation", 0.7}, {"contrast", 1.15}, {"temperature", -12.0},
                        {"exposure_compensation", -0.5}, {"fog_density", 0.006}, {"volumetric_light", true},
                        {"volumetric_density", 0.05}, {"vignette", true}, {"vignette_intensity", 0.4},
                        {"film_grain", 0.1}}));
    out.push_back(post("Invierno", "Frio y limpio, sombras azuladas, algo de bruma",
                       {{"tonemapper", "PBR Neutral"}, {"temperature", -30.0}, {"saturation", 0.8},
                        {"contrast", 1.05}, {"lift", {0.02, 0.03, 0.05}}, {"fog_density", 0.004},
                        {"bloom", true}, {"bloom_intensity", 0.07}}));
    out.push_back(post("Retro (VHS / PSX)", "Sin suavizado, colores saturados, aberracion, grano y lente curvada",
                       {{"fxaa", false}, {"saturation", 1.2}, {"contrast", 1.05}, {"chromatic_aberration", 0.6},
                        {"film_grain", 0.5}, {"vignette", true}, {"vignette_intensity", 0.5}, {"bloom", true},
                        {"bloom_intensity", 0.15}, {"bloom_threshold", 0.5}, {"lens_distortion", 0.2}}));
    out.push_back(post("Blanco y negro", "Pelicula en blanco y negro con contraste y grano",
                       {{"saturation", 0.0}, {"vibrance", 0.0}, {"contrast", 1.3}, {"film_grain", 0.3},
                        {"vignette", true}, {"vignette_intensity", 0.45}}));
    return out;
}

std::string sanitizeName(const std::string& name) {
    std::string out;
    for (char c : name) {
        const bool bad = c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
                         c == '>' || c == '|';
        out += bad ? '_' : c;
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    return out;
}

std::filesystem::path presetFolder(const std::filesystem::path& assets, const std::string& component) {
    return assets / "Presets" / component;
}

}  // namespace

std::vector<ComponentPreset> builtinPresets(const std::string& component) {
    if (component == "PostProcessing") return postProcessingPresets();
    return {};
}

bool readPreset(const std::filesystem::path& file, ComponentPreset& out, std::string* error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        if (error) *error = "no se pudo abrir " + file.string();
        return false;
    }
    const json root = json::parse(in, nullptr, false);
    if (root.is_discarded() || !root.is_object() || !root.contains("fields") || !root["fields"].is_object()) {
        if (error) *error = "preset danado: " + file.string();
        return false;
    }
    out = ComponentPreset{};
    out.component = root.value("component", std::string{});
    out.name = root.value("name", file.stem().string());
    out.description = root.value("description", std::string{});
    out.fields = root["fields"].dump();
    out.file = file;
    return true;
}

std::vector<ComponentPreset> projectPresets(const std::filesystem::path& assets, const std::string& component) {
    std::vector<ComponentPreset> out;
    std::error_code ec;
    const std::filesystem::path folder = presetFolder(assets, component);
    if (!std::filesystem::is_directory(folder, ec)) return out;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        if (!entry.is_regular_file() || entry.path().extension() != kPresetExtension) continue;
        ComponentPreset p;
        if (readPreset(entry.path(), p) && (p.component.empty() || p.component == component)) {
            p.component = component;
            out.push_back(std::move(p));
        }
    }
    std::sort(out.begin(), out.end(), [](const ComponentPreset& a, const ComponentPreset& b) { return a.name < b.name; });
    return out;
}

std::vector<ComponentPreset> allPresets(const std::filesystem::path& assets, const std::string& component) {
    std::vector<ComponentPreset> out = builtinPresets(component);
    std::vector<ComponentPreset> mine = projectPresets(assets, component);
    out.insert(out.end(), std::make_move_iterator(mine.begin()), std::make_move_iterator(mine.end()));
    return out;
}

const ComponentPreset* findPreset(const std::vector<ComponentPreset>& presets, const std::string& name) {
    for (const ComponentPreset& p : presets) {
        if (p.name == name) return &p;
    }
    // Sin distinguir mayusculas (MCP, Lua).
    const auto lower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };
    for (const ComponentPreset& p : presets) {
        if (lower(p.name) == lower(name)) return &p;
    }
    return nullptr;
}

std::vector<std::string> presetExcludedFields(const std::string& component) {
    if (component == "PostProcessing") {
        return {"shape", "priority", "weight", "size", "radius", "blend_distance"};
    }
    if (component == "Light") return {};
    return {};
}

std::filesystem::path savePreset(World& world, Entity entity, const std::string& component, const std::string& name,
                                 const std::filesystem::path& assets, std::string* error) {
    const std::string clean = sanitizeName(name);
    if (clean.empty()) {
        if (error) *error = "el preset necesita un nombre";
        return {};
    }
    const std::string fields_text = componentToJson(world, entity, component);
    json fields = json::parse(fields_text, nullptr, false);
    if (fields.is_discarded() || !fields.is_object()) {
        if (error) *error = "la entidad no tiene " + component;
        return {};
    }
    for (const std::string& key : presetExcludedFields(component)) fields.erase(key);
    json root;
    root["format"] = "CramionPreset";
    root["component"] = component;
    root["name"] = name;
    root["fields"] = std::move(fields);
    const std::filesystem::path folder = presetFolder(assets, component);
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const std::filesystem::path file = folder / (clean + kPresetExtension);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error) *error = "no se pudo escribir " + file.string();
        return {};
    }
    out << root.dump(2);
    return file;
}

bool applyPreset(World& world, Entity entity, const ComponentPreset& preset, std::string* error) {
    const ComponentType* type = ComponentRegistry::instance().find(preset.component);
    if (type == nullptr) {
        if (error) *error = "componente desconocido: " + preset.component;
        return false;
    }
    if (!entity.valid()) {
        if (error) *error = "la entidad no existe";
        return false;
    }
    json fields = json::parse(preset.fields, nullptr, false);
    if (fields.is_discarded() || !fields.is_object()) {
        if (error) *error = "preset danado: " + preset.name;
        return false;
    }
    // Lo propio de la entidad se conserva; el resto vuelve a los valores por
    // defecto antes de aplicar el preset.
    json kept = json::object();
    if (type->has(world, entity.handle())) {
        const json current = json::parse(componentToJson(world, entity, preset.component), nullptr, false);
        if (current.is_object()) {
            for (const std::string& key : presetExcludedFields(preset.component)) {
                if (const auto it = current.find(key); it != current.end()) kept[key] = *it;
            }
        }
        type->remove(world, entity.handle());
    }
    type->add(world, entity.handle());
    for (const std::string& key : presetExcludedFields(preset.component)) fields.erase(key);
    fields.update(kept);
    return componentFromJson(world, entity, preset.component, fields.dump(), error);
}

}  // namespace cramion::ecs

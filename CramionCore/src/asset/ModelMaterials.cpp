#include "CramionCore/asset/ModelMaterials.h"

#include "CramionCore/asset/AssetManager.h"
#include "CramionCore/asset/MaterialAsset.h"
#include "CrData.h"

#include <CramionFX/asset/ImageFile.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <map>
#include <set>

namespace cramion::assets {

namespace {

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool isImageExtension(const std::string& extension) {
    const std::string e = lower(extension);
    return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".tga" || e == ".bmp";
}

// Solo letras y numeros, en minusculas: "Mat_Wood-01" -> "matwood01".
std::string normalized(const std::string& text) {
    std::string out;
    for (const unsigned char c : text) {
        if (std::isalnum(c) || c >= 0x80) out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

// Quita prefijos de material habituales ("M_Wood", "MI_Wood", "mat_wood").
std::string withoutMaterialPrefix(const std::string& name) {
    const std::string l = lower(name);
    for (const char* prefix : {"mat_", "mtl_", "mi_", "m_", "material_"}) {
        const std::string p(prefix);
        if (l.size() > p.size() + 1 && l.compare(0, p.size(), p) == 0) return name.substr(p.size());
    }
    return name;
}

// Nombre de archivo valido (sin \ / : * ? " < > |).
std::string safeFileName(const std::string& name) {
    std::string out;
    for (const char c : name) {
        const bool control = static_cast<unsigned char>(c) < 0x20;
        const bool reserved = c != '\0' && std::strchr("\\/:*?\"<>|", c) != nullptr;
        out.push_back(control || reserved ? '_' : c);
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    return out.empty() ? std::string("Material") : out;
}

// --- Clasificacion de imagenes por sufijo ---

enum class MapKind { Color, Other };

// Sufijos de mapas que NO son de color (los de materialFromImage y algunos mas).
constexpr std::array kOtherSuffixes = {
    "normal", "normalgl", "normaldx", "nrm", "nor", "norm", "n", "roughness", "rough", "rgh", "r",
    "metallic", "metalness", "metal", "met", "m", "ao", "occlusion", "ambientocclusion", "emissive",
    "emission", "emit", "e", "displacement", "height", "disp", "heightmap", "parallax", "h", "cavity",
    "cav", "specular", "spec", "specularlevel", "gloss", "glossiness", "smoothness", "bump", "bumpmap",
    "orm", "arm", "mra", "rma", "opacity", "alpha", "mask", "o"};
constexpr std::array kColorSuffixes = {"basecolor", "albedo", "albedotransparency", "diffuse", "diffusemap",
                                       "colormap", "color", "colour", "col", "diff", "d", "c", "bc", "basemap",
                                       "maintex", "tex"};

// Palabras que delatan un mapa que no es de color en cualquier parte del
// nombre: "Ambient Occlusion Map from Mesh x", "x_MetallicSmoothness_A" (un
// canal ya separado), "x_Normal_OpenGL"...
constexpr std::array kOtherWords = {"normal", "rough", "metallic", "metalness", "smoothness", "gloss", "occlusion",
                                    "ambientocclusion", "height", "displacement", "emissive", "emission", "specular",
                                    "cavity", "bump", "opacity", "maskmap", "curvature", "thickness"};

std::string squashedName(const std::string& text) {
    std::string out;
    for (const char c : lower(text)) {
        if (c != '_' && c != '-' && c != ' ' && c != '.') out.push_back(c);
    }
    return out;
}

// La imagen parece de color (no normal, rugosidad, AO, un canal separado...).
bool looksLikeColor(const std::filesystem::path& file) {
    const std::string name = squashedName(crdata::utf8(file.stem()));
    for (const char* word : kOtherWords) {
        if (name.find(word) != std::string::npos) return false;
    }
    return true;
}

// Parte final del nombre tras el ultimo separador ('_', '-', ' ', '.').
std::pair<std::string, std::string> splitSuffix(const std::string& stem) {
    const std::size_t at = stem.find_last_of("_- .");
    if (at == std::string::npos || at == 0 || at + 1 >= stem.size()) return {stem, {}};
    return {stem.substr(0, at), lower(stem.substr(at + 1))};
}

struct Candidate {
    std::filesystem::path file;
    std::string base;      // nombre normalizado sin el sufijo de color
    bool in_model_folder;  // en la carpeta del modelo (o debajo)
    bool external;         // fuera de Assets (junto al archivo original): se copia
};

// Una imagen es candidata a color si su sufijo es de color o no es de otro
// mapa ("madera.png").
bool colorCandidate(const std::filesystem::path& file, std::string& base) {
    if (!looksLikeColor(file)) return false;
    const std::string stem = crdata::utf8(file.stem());
    const auto [head, suffix] = splitSuffix(stem);
    if (!suffix.empty()) {
        if (std::find(kColorSuffixes.begin(), kColorSuffixes.end(), suffix) != kColorSuffixes.end()) {
            base = normalized(head);
            return !base.empty();
        }
        if (std::find(kOtherSuffixes.begin(), kOtherSuffixes.end(), suffix) != kOtherSuffixes.end()) return false;
    }
    base = normalized(stem);
    return !base.empty();
}

void collectImages(const std::filesystem::path& folder, int depth, bool external,
                   const std::filesystem::path& model_folder, std::vector<Candidate>& out) {
    std::error_code ec;
    if (!std::filesystem::is_directory(folder, ec)) return;
    for (std::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code ec2;
        if (it->is_directory(ec2)) {
            if (depth > 0) collectImages(it->path(), depth - 1, external, model_folder, out);
            continue;
        }
        if (!it->is_regular_file(ec2) || !isImageExtension(it->path().extension().string())) continue;
        std::string base;
        if (!colorCandidate(it->path(), base)) continue;
        const std::string rel = crdata::utf8(std::filesystem::relative(it->path(), model_folder, ec2));
        const bool inside = !external && !ec2 && !rel.empty() && rel.rfind("..", 0) != 0;
        out.push_back(Candidate{it->path(), base, inside, external});
    }
}

// Puntua lo bien que casa una imagen con un material. 0 = nada.
int matchScore(const Candidate& c, const std::vector<std::pair<std::string, int>>& keys) {
    int best = 0;
    for (const auto& [key, weight] : keys) {
        if (key.size() < 2) continue;
        int score = 0;
        if (c.base == key) score = weight;
        else if (key.size() >= 3 && c.base.size() >= 3 &&
                 (c.base.find(key) != std::string::npos || key.find(c.base) != std::string::npos)) {
            score = weight / 2;
        }
        best = std::max(best, score);
    }
    if (best > 0 && c.in_model_folder) best += 5;
    return best;
}

std::string relativeToAssets(const std::filesystem::path& file, const std::filesystem::path& assets_root) {
    std::error_code ec;
    std::string rel = crdata::utf8(std::filesystem::relative(file, assets_root, ec));
    std::replace(rel.begin(), rel.end(), '\\', '/');
    return rel;
}

bool writeBytes(const std::filesystem::path& file, const std::vector<std::uint8_t>& bytes) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

// Extension de un archivo de imagen por su firma; vacia si no se reconoce.
std::string imageExtension(const std::vector<std::uint8_t>& b) {
    if (b.size() >= 8 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') return ".png";
    if (b.size() >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return ".jpg";
    if (b.size() >= 2 && b[0] == 'B' && b[1] == 'M') return ".bmp";
    return {};
}

// Saca las texturas incrustadas de una pieza a imagenes de Assets.
class TextureWriter {
public:
    TextureWriter(const std::filesystem::path& folder, const std::filesystem::path& assets_root)
        : folder_(folder), assets_root_(assets_root) {}

    // Imagen completa. Vacia si no se pudo.
    std::string full(const asset::ModelData& part, std::int32_t index, const std::string& name) {
        const asset::TextureData* texture = get(part, index);
        if (texture == nullptr) return {};
        const std::string key = cacheKey(part, index, -1);
        if (const auto it = written_.find(key); it != written_.end()) return it->second;

        std::string result;
        if (!texture->encoded.empty()) {
            const std::string ext = imageExtension(texture->encoded);
            if (!ext.empty()) {
                const std::filesystem::path file = freeFile(name, ext);
                if (writeBytes(file, texture->encoded)) result = relativeToAssets(file, assets_root_);
            }
        } else if (!texture->source_path.empty() && isImageExtension(crdata::fromUtf8(texture->source_path).extension().string())) {
            const std::filesystem::path source = crdata::fromUtf8(texture->source_path);
            const std::filesystem::path file = freeFile(name, lower(source.extension().string()));
            std::error_code ec;
            std::filesystem::create_directories(file.parent_path(), ec);
            if (std::filesystem::copy_file(source, file, std::filesystem::copy_options::overwrite_existing, ec)) {
                result = relativeToAssets(file, assets_root_);
            }
        }
        if (result.empty()) {
            asset::ImageRgba8 image;
            if (decode(*texture, image)) {
                const std::filesystem::path file = freeFile(name, ".png");
                if (asset::saveImagePng(file, image)) result = relativeToAssets(file, assets_root_);
            }
        }
        if (!result.empty()) ++extracted_;
        written_[key] = result;
        return result;
    }

    // Un canal (0 R, 1 G, 2 B, 3 A) como mapa en grises. `skip_if_flat`: no
    // escribe nada si el canal es constante (un alfa todo a 255).
    std::string channel(const asset::ModelData& part, std::int32_t index, int c, const std::string& name,
                        bool skip_if_flat = false) {
        const asset::TextureData* texture = get(part, index);
        if (texture == nullptr) return {};
        const std::string key = cacheKey(part, index, c);
        if (const auto it = written_.find(key); it != written_.end()) return it->second;
        std::string result;
        asset::ImageRgba8 image;
        if (decode(*texture, image)) {
            const std::size_t count = static_cast<std::size_t>(image.width) * image.height;
            std::uint8_t lo = 255;
            std::uint8_t hi = 0;
            for (std::size_t i = 0; i < count; ++i) {
                const std::uint8_t v = image.pixels[i * 4 + static_cast<std::size_t>(c)];
                lo = std::min(lo, v);
                hi = std::max(hi, v);
                image.pixels[i * 4 + 0] = v;
                image.pixels[i * 4 + 1] = v;
                image.pixels[i * 4 + 2] = v;
                image.pixels[i * 4 + 3] = 255;
            }
            if (!(skip_if_flat && hi - lo < 3)) {
                const std::filesystem::path file = freeFile(name, ".png");
                if (asset::saveImagePng(file, image)) {
                    result = relativeToAssets(file, assets_root_);
                    ++extracted_;
                }
            }
        }
        written_[key] = result;
        return result;
    }

    int extracted() const { return extracted_; }

private:
    static const asset::TextureData* get(const asset::ModelData& part, std::int32_t index) {
        if (index < 0 || static_cast<std::size_t>(index) >= part.textures.size()) return nullptr;
        return &part.textures[static_cast<std::size_t>(index)];
    }

    // Misma imagen en varias piezas: por nombre y tamano de los datos.
    static std::string cacheKey(const asset::ModelData& part, std::int32_t index, int c) {
        const asset::TextureData& t = part.textures[static_cast<std::size_t>(index)];
        return t.name + "|" + std::to_string(t.encoded.size()) + "|" + t.source_path + "|" +
               std::to_string(t.width) + "x" + std::to_string(t.height) + "|" + std::to_string(c);
    }

    static bool decode(const asset::TextureData& t, asset::ImageRgba8& image) {
        if (!t.encoded.empty()) return asset::decodeImageRgba8(t.encoded.data(), t.encoded.size(), image);
        if (!t.source_path.empty()) return asset::loadImageRgba8(crdata::fromUtf8(t.source_path), image);
        if (t.format == asset::TextureFormat::Rgba8 && t.width > 0 &&
            t.pixels.size() >= static_cast<std::size_t>(t.width) * t.height * 4) {
            image.width = t.width;
            image.height = t.height;
            image.pixels.assign(t.pixels.begin(), t.pixels.begin() + static_cast<std::ptrdiff_t>(t.width) * t.height * 4);
            return true;
        }
        return false;  // BC comprimida: sin decodificador en CPU
    }

    std::filesystem::path freeFile(const std::string& name, const std::string& ext) {
        std::filesystem::path file = folder_ / crdata::fromUtf8(safeFileName(name) + ext);
        for (int i = 2; std::filesystem::exists(file) || used_.count(file.wstring()) != 0; ++i) {
            file = folder_ / crdata::fromUtf8(safeFileName(name) + " " + std::to_string(i) + ext);
        }
        used_.insert(file.wstring());
        return file;
    }

    std::filesystem::path folder_;
    std::filesystem::path assets_root_;
    std::map<std::string, std::string> written_;
    std::set<std::wstring> used_;
    int extracted_ = 0;
};

// Rellena los mapas vacios de `m` con los que acompanan a `image` (su
// imagen de color): los de Assets tal cual, los de fuera copiados a
// `texture_folder`. Devuelve cuantos puso.
int addCompanionMaps(MaterialAsset& m, const std::filesystem::path& image, const std::filesystem::path& assets_root,
                     const std::filesystem::path& texture_folder) {
    std::error_code ec;
    if (image.empty() || !std::filesystem::is_regular_file(image, ec)) return 0;
    const CompanionMaps maps = findCompanionMaps(image);
    const auto inside_assets = [&](const std::filesystem::path& p) {
        const std::string rel = crdata::utf8(p.lexically_relative(assets_root));
        return !rel.empty() && rel.rfind("..", 0) != 0;
    };
    int added = 0;
    const auto put = [&](std::string& target, const std::filesystem::path& found) {
        if (!target.empty() || found.empty()) return false;
        std::filesystem::path file = found;
        if (!inside_assets(file)) {
            std::error_code ec2;
            std::filesystem::create_directories(texture_folder, ec2);
            file = texture_folder / found.filename();
            std::filesystem::copy_file(found, file, std::filesystem::copy_options::skip_existing, ec2);
            if (!std::filesystem::exists(file, ec2)) return false;
        }
        target = relativeToAssets(file, assets_root);
        ++added;
        return true;
    };
    if (put(m.normal, maps.normal)) m.normal_directx = maps.normal_directx;
    // Con mapa, el factor es el maximo (el mapa decide).
    if (put(m.roughness_map, maps.roughness)) m.roughness = 1.0f;
    if (put(m.metallic_map, maps.metallic)) m.metallic = 1.0f;
    put(m.occlusion, maps.occlusion);
    if (put(m.emissive_map, maps.emissive) && std::max({m.emissive.x, m.emissive.y, m.emissive.z}) <= 0.0f) {
        m.emissive = core::Vec3{1.0f, 1.0f, 1.0f};
    }
    put(m.height_map, maps.height);
    put(m.cavity_map, maps.cavity);
    put(m.specular_map, maps.specular);
    if (put(m.gloss_map, maps.gloss) && m.roughness_map.empty()) m.roughness = 1.0f;
    put(m.bump_map, maps.bump);

    // Mapas empaquetados: cada canal a su propio mapa en grises, en
    // `texture_folder` (si ya estan de antes, se reutilizan).
    const auto unpack = [&](const std::filesystem::path& packed, std::initializer_list<std::pair<std::string*, int>> wanted) {
        if (packed.empty()) return;
        bool needed = false;
        for (const auto& [target, c] : wanted) needed = needed || target->empty();
        if (!needed) return;
        asset::ImageRgba8 image;
        bool loaded = false;
        const std::string stem = safeFileName(crdata::utf8(packed.stem()));
        static constexpr const char* kChannel[] = {"R", "G", "B", "A"};
        for (const auto& [target, c] : wanted) {
            if (!target->empty()) continue;
            const std::filesystem::path out = texture_folder / crdata::fromUtf8(stem + "_" + kChannel[c] + ".png");
            std::error_code ec2;
            if (!std::filesystem::exists(out, ec2)) {
                if (!loaded && !(loaded = asset::loadImageRgba8(packed, image))) return;
                asset::ImageRgba8 gray = image;
                const std::size_t count = static_cast<std::size_t>(gray.width) * gray.height;
                std::uint8_t lo = 255;
                std::uint8_t hi = 0;
                for (std::size_t i = 0; i < count; ++i) {
                    const std::uint8_t v = image.pixels[i * 4 + static_cast<std::size_t>(c)];
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                    gray.pixels[i * 4 + 0] = v;
                    gray.pixels[i * 4 + 1] = v;
                    gray.pixels[i * 4 + 2] = v;
                    gray.pixels[i * 4 + 3] = 255;
                }
                // Un canal constante (un alfa todo a 255): no es un mapa.
                if (hi - lo < 3) continue;
                std::filesystem::create_directories(texture_folder, ec2);
                if (!asset::saveImagePng(out, gray)) continue;
            }
            *target = relativeToAssets(out, assets_root);
            ++added;
        }
    };
    const bool had_metal = !m.metallic_map.empty();
    const bool had_rough = !m.roughness_map.empty() || !m.gloss_map.empty();
    unpack(maps.metallic_smoothness, {{&m.metallic_map, 0}, {&m.gloss_map, 3}});
    unpack(maps.mask_map, {{&m.metallic_map, 0}, {&m.occlusion, 1}, {&m.gloss_map, 3}});
    unpack(maps.orm, {{&m.occlusion, 0}, {&m.roughness_map, 1}, {&m.metallic_map, 2}});
    if (!had_metal && !m.metallic_map.empty()) m.metallic = 1.0f;
    if (!had_rough && (!m.roughness_map.empty() || !m.gloss_map.empty())) m.roughness = 1.0f;
    return added;
}

// Factores del material del modelo (sin texturas).
MaterialAsset factorsOf(const asset::MaterialData& d) {
    MaterialAsset m;
    m.mode = d.transparent ? MaterialMode::Transparent : MaterialMode::Opaque;
    m.base_color = d.base_color;
    m.metallic = d.metallic;
    m.roughness = d.roughness;
    m.reflectance = d.reflectance;
    m.normal_strength = d.normal_scale;
    m.normal_directx = d.normal_map_directx;
    m.occlusion_strength = d.occlusion_strength;
    const float peak = std::max({d.emissive.x, d.emissive.y, d.emissive.z});
    if (peak > 1.0f) {
        m.emissive = d.emissive * (1.0f / peak);
        m.emissive_intensity = peak;
    } else {
        m.emissive = d.emissive;
    }
    if (d.height_scale > 0.0f) m.height_scale = d.height_scale;
    return m;
}

}  // namespace

std::string modelMaterialKey(const std::string& material_name, std::size_t index) {
    return material_name.empty() ? "Material " + std::to_string(index) : material_name;
}

ExtractMaterialsResult extractModelMaterials(const AssetInfo& info, const std::filesystem::path& assets_root) {
    ExtractMaterialsResult result;
    const std::shared_ptr<ModelAsset> model = AssetManager::readModel(info.uuid, info.path, info.name, /*decode_textures=*/false);
    if (!model) {
        result.message = "no se pudo leer " + crdata::utf8(info.path.filename());
        return result;
    }

    const std::filesystem::path model_folder = info.path.parent_path();
    const std::string stem = safeFileName(crdata::utf8(info.path.stem()));
    const std::filesystem::path material_folder = model_folder / "Materials" / crdata::fromUtf8(stem);
    const std::filesystem::path texture_folder = model_folder / "Textures" / crdata::fromUtf8(stem);
    result.material_folder = material_folder;
    TextureWriter writer(texture_folder, assets_root);

    // Imagenes donde buscar las texturas que el modelo no trae: la carpeta
    // del modelo (y debajo), todo Assets y, fuera, junto al archivo original.
    std::vector<Candidate> candidates;
    bool searched = false;
    const auto search_pool = [&]() -> const std::vector<Candidate>& {
        if (!searched) {
            searched = true;
            collectImages(assets_root, 12, false, model_folder, candidates);
            if (!info.source.empty()) {
                const std::filesystem::path source_folder = info.source.parent_path();
                std::error_code ec;
                if (std::filesystem::is_directory(source_folder, ec) &&
                    source_folder.lexically_relative(assets_root).string().rfind("..", 0) == 0) {
                    collectImages(source_folder, 2, true, model_folder, candidates);
                    // Packs con las texturas en una carpeta hermana ("../Textures").
                    for (const char* sibling : {"Textures", "textures", "Texture", "Materials", "Tex"}) {
                        collectImages(source_folder.parent_path() / sibling, 1, true, model_folder, candidates);
                    }
                }
            }
        }
        return candidates;
    };

    // La imagen de color original de una textura del modelo (la ruta a la
    // que apunta el FBX o, si va incrustada, la del mismo nombre en el
    // proyecto o junto al archivo original): sus companeras estan ahi.
    const auto original_image = [&](const asset::ModelData& part, std::int32_t index) -> std::filesystem::path {
        if (index < 0 || static_cast<std::size_t>(index) >= part.textures.size()) return {};
        const asset::TextureData& t = part.textures[static_cast<std::size_t>(index)];
        std::error_code ec;
        if (!t.source_path.empty()) {
            const std::filesystem::path source = crdata::fromUtf8(t.source_path);
            if (std::filesystem::is_regular_file(source, ec)) return source;
        }
        const std::string wanted = lower(crdata::utf8(crdata::fromUtf8(t.source_path.empty() ? t.name : t.source_path).filename()));
        if (wanted.empty()) return {};
        for (const Candidate& c : search_pool()) {
            if (lower(crdata::utf8(c.file.filename())) == wanted) return c.file;
        }
        return {};
    };
    // Los mapas que le faltan a `m`: junto a su color original y junto a su
    // albedo de Assets.
    const auto complete = [&](MaterialAsset& m, const asset::ModelData& part, const asset::MaterialData& data) {
        int added = addCompanionMaps(m, original_image(part, data.albedo_texture), assets_root, texture_folder);
        if (!m.albedo.empty()) {
            added += addCompanionMaps(m, assets_root / crdata::fromUtf8(m.albedo), assets_root, texture_folder);
        }
        return added;
    };

    // Sin textura de color: se busca en el proyecto por el nombre del
    // material, de la pieza o del modelo. Conserva el UUID de `m`.
    const auto search_color = [&](MaterialAsset& m, const asset::ModelData& part, const asset::MaterialData& data) -> bool {
        const std::vector<std::pair<std::string, int>> keys = {
            {normalized(withoutMaterialPrefix(data.name)), 100},
            {normalized(data.name), 100},
            {normalized(part.name), 70},
            {normalized(model->name), 50},
            {normalized(stem), 50},
        };
        // Primero las de la carpeta del modelo (lo que trae su pack); si no
        // casa ninguna por nombre pero solo hay un juego, es el suyo. Solo
        // despues el resto del proyecto ("base" no debe coger la textura de
        // otro modelo que se llame T_X_Base_Color).
        const auto pick = [&](bool inside) {
            const Candidate* best = nullptr;
            int best_score = 0;
            for (const Candidate& c : search_pool()) {
                if (c.in_model_folder != inside) continue;
                const int score = matchScore(c, keys);
                if (score > best_score) {
                    best_score = score;
                    best = &c;
                }
            }
            return best;
        };
        const Candidate* best = pick(true);
        if (best == nullptr) {
            const Candidate* only = nullptr;
            int count = 0;
            for (const Candidate& c : search_pool()) {
                if (c.in_model_folder) {
                    only = &c;
                    ++count;
                }
            }
            if (count == 1) best = only;
        }
        if (best == nullptr) best = pick(false);
        if (best == nullptr) return false;
        std::filesystem::path image = best->file;
        if (best->external) {
            // Fuera de Assets: se copia el juego entero (los
            // mapas con el mismo nombre base) a Textures/<modelo>.
            std::error_code ec;
            std::filesystem::create_directories(texture_folder, ec);
            const std::string prefix = lower(splitSuffix(crdata::utf8(image.stem())).first);
            for (std::filesystem::directory_iterator it(image.parent_path(), ec), end; !ec && it != end; it.increment(ec)) {
                if (!isImageExtension(it->path().extension().string())) continue;
                const std::string s = lower(crdata::utf8(it->path().stem()));
                if (s.rfind(prefix, 0) != 0) continue;
                std::error_code ec2;
                std::filesystem::copy_file(it->path(), texture_folder / it->path().filename(),
                                           std::filesystem::copy_options::skip_existing, ec2);
            }
            image = texture_folder / image.filename();
        }
        MaterialAsset found = materialFromImage(assets_root, relativeToAssets(image, assets_root));
        // El color del archivo (un gris de 0.8 en muchos FBX) oscurecia la textura.
        found.uuid = m.uuid;
        found.base_color = core::Vec4{1.0f, 1.0f, 1.0f, m.base_color.w};
        found.mode = m.mode;
        found.normal_strength = m.normal_strength;
        if (found.roughness_map.empty()) found.roughness = m.roughness;
        if (found.metallic_map.empty()) found.metallic = m.metallic;
        if (found.normal.empty()) found.normal = m.normal;
        if (found.emissive_map.empty()) {
            found.emissive_map = m.emissive_map;
            found.emissive = m.emissive;
            found.emissive_intensity = m.emissive_intensity;
        }
        m = found;
        return true;
    };

    // Un material por nombre, aunque aparezca en varias piezas.
    std::map<std::string, Uuid> by_name;
    for (std::size_t p = 0; p < model->parts.size(); ++p) {
        const asset::ModelData& part = *model->parts[p];
        for (std::size_t i = 0; i < part.materials.size(); ++i) {
            const asset::MaterialData& data = part.materials[i];
            const std::string key = modelMaterialKey(data.name, i);
            if (by_name.count(key) != 0) continue;

            const std::filesystem::path file = material_folder / crdata::fromUtf8(safeFileName(key) + ".crmat");
            MaterialAsset existing;
            if (std::filesystem::exists(file) && loadMaterial(file, existing) && existing.uuid.valid()) {
                by_name[key] = existing.uuid;
                ++result.reused;
                // Un color que en realidad era otro mapa (AO, un canal de
                // MetallicSmoothness: versiones anteriores lo cogian): se busca otra vez.
                bool fixed = false;
                if (!existing.albedo.empty() && !looksLikeColor(crdata::fromUtf8(existing.albedo))) {
                    existing.albedo.clear();
                    fixed = search_color(existing, part, data);
                    if (fixed) ++result.textures_found;
                }
                // Solo sus huecos vacios: lo que el usuario puso se queda.
                const int added = complete(existing, part, data);
                if ((fixed || added > 0) && saveMaterial(existing, file)) {
                    result.maps_found += added;
                    ++result.completed;
                }
                continue;
            }

            MaterialAsset m = factorsOf(data);
            const std::string base = safeFileName(key);
            m.albedo = writer.full(part, data.albedo_texture, base + "_Albedo");
            m.normal = writer.full(part, data.normal_texture, base + "_Normal");
            m.emissive_map = writer.full(part, data.emissive_texture, base + "_Emission");
            if (!m.emissive_map.empty() && std::max({m.emissive.x, m.emissive.y, m.emissive.z}) <= 0.0f) {
                m.emissive = core::Vec3{1.0f, 1.0f, 1.0f};
            }
            // glTF: oclusion en R (el motor guarda la altura del parallax en G).
            if (data.occlusion_texture >= 0) {
                m.occlusion = writer.channel(part, data.occlusion_texture, 0, base + "_AO");
                if (data.height_scale > 0.0f) m.height_map = writer.channel(part, data.occlusion_texture, 1, base + "_Height");
            }
            // glTF: G = rugosidad, B = metal; R = reflectancia si el modelo la
            // trae; A = cavidad (1 = sin grietas).
            if (data.metallic_roughness_texture >= 0) {
                m.roughness_map = writer.channel(part, data.metallic_roughness_texture, 1, base + "_Roughness");
                m.metallic_map = writer.channel(part, data.metallic_roughness_texture, 2, base + "_Metallic", true);
                if (data.specular_map) m.specular_map = writer.channel(part, data.metallic_roughness_texture, 0, base + "_Specular");
                m.cavity_map = writer.channel(part, data.metallic_roughness_texture, 3, base + "_Cavity", true);
            }

            // Sin textura de color: se busca en el proyecto.
            if (m.albedo.empty()) {
                if (search_color(m, part, data)) ++result.textures_found;
                else result.without_color.push_back(key);
            }

            result.maps_found += complete(m, part, data);

            std::error_code ec;
            std::filesystem::create_directories(material_folder, ec);
            if (!saveMaterial(m, file)) {
                result.message = "no se pudo escribir " + crdata::utf8(file.filename());
                return result;
            }
            by_name[key] = m.uuid;
            ++result.created;
        }
    }

    result.map = std::move(by_name);
    result.textures_extracted = writer.extracted();
    result.ok = true;
    return result;
}

// --- Mapas por modelo ---

namespace {

std::filesystem::path mapFile(const std::filesystem::path& settings_folder) {
    return settings_folder / "ModelMaterials.json";
}

nlohmann::json readMaps(const std::filesystem::path& settings_folder) {
    std::ifstream in(mapFile(settings_folder), std::ios::binary);
    if (!in) return nlohmann::json::object();
    nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    return j.is_object() ? j : nlohmann::json::object();
}

}  // namespace

ModelMaterialMap loadModelMaterialMap(const std::filesystem::path& settings_folder, const Uuid& model) {
    ModelMaterialMap map;
    const nlohmann::json all = readMaps(settings_folder);
    const auto it = all.find(model.toString());
    if (it == all.end() || !it->is_object()) return map;
    for (const auto& [name, value] : it->items()) {
        if (!value.is_string()) continue;
        const Uuid uuid = Uuid::parse(value.get<std::string>());
        if (uuid.valid()) map[name] = uuid;
    }
    return map;
}

bool saveModelMaterialMap(const std::filesystem::path& settings_folder, const Uuid& model, const ModelMaterialMap& map) {
    nlohmann::json all = readMaps(settings_folder);
    nlohmann::json entry = nlohmann::json::object();
    for (const auto& [name, uuid] : map) entry[name] = uuid.toString();
    all[model.toString()] = entry;
    std::error_code ec;
    std::filesystem::create_directories(settings_folder, ec);
    std::ofstream out(mapFile(settings_folder), std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << all.dump(2);
    return static_cast<bool>(out);
}

}  // namespace cramion::assets

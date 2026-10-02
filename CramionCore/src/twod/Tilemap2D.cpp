#include "CramionCore/twod/Tilemap2D.h"

#include <CramionFX/asset/ImageFile.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <unordered_set>

namespace cramion::twod {

using json = nlohmann::json;

// -----------------------------------------------------------------------------
// Tileset
// -----------------------------------------------------------------------------

int Tileset::columns() const {
    if (tile_width <= 0 || image_width <= 0) return 0;
    return std::max(0, (image_width - 2 * margin + spacing) / (tile_width + spacing));
}

int Tileset::rows() const {
    if (tile_height <= 0 || image_height <= 0) return 0;
    return std::max(0, (image_height - 2 * margin + spacing) / (tile_height + spacing));
}

void Tileset::frameUv(int frame, core::Vec2& uv_min, core::Vec2& uv_max) const {
    const int cols = std::max(columns(), 1);
    const int col = frame % cols;
    const int row = frame / cols;
    const float px = static_cast<float>(margin + col * (tile_width + spacing));
    const float py = static_cast<float>(margin + row * (tile_height + spacing));
    const float w = static_cast<float>(std::max(image_width, 1));
    const float h = static_cast<float>(std::max(image_height, 1));
    // Un poco hacia dentro: sin lineas de la celda vecina al acercarse.
    constexpr float kInset = 0.01f;
    uv_min = core::Vec2{(px + kInset) / w, (py + kInset) / h};
    uv_max = core::Vec2{(px + static_cast<float>(tile_width) - kInset) / w, (py + static_cast<float>(tile_height) - kInset) / h};
}

bool Tileset::frameSolid(int frame) const {
    return std::find(no_collider.begin(), no_collider.end(), frame) == no_collider.end();
}

std::string tilesetToJson(const Tileset& t) {
    json j;
    j["version"] = 1;
    j["image"] = t.image;
    j["tile_width"] = t.tile_width;
    j["tile_height"] = t.tile_height;
    j["margin"] = t.margin;
    j["spacing"] = t.spacing;
    j["filter"] = t.filter == SpriteFilter::Point ? "point" : "bilinear";
    j["no_collider"] = t.no_collider;
    json rules = json::array();
    for (const RuleTile& r : t.rule_tiles) {
        json rj;
        rj["name"] = r.name;
        rj["default_frame"] = r.default_frame;
        rj["collider"] = r.collider;
        json list = json::array();
        for (const TileRule& rule : r.rules) {
            json n = json::array();
            for (TileNeighbor nb : rule.neighbors) n.push_back(static_cast<int>(nb));
            list.push_back(json{{"neighbors", n}, {"frames", rule.frames}});
        }
        rj["rules"] = list;
        rules.push_back(rj);
    }
    j["rule_tiles"] = rules;
    return j.dump(2);
}

bool tilesetFromJson(const std::string& text, Tileset& t) {
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;
    t.image = j.value("image", std::string());
    t.tile_width = std::max(1, j.value("tile_width", 16));
    t.tile_height = std::max(1, j.value("tile_height", 16));
    t.margin = std::max(0, j.value("margin", 0));
    t.spacing = std::max(0, j.value("spacing", 0));
    t.filter = j.value("filter", std::string("point")) == "bilinear" ? SpriteFilter::Bilinear : SpriteFilter::Point;
    t.no_collider.clear();
    if (j.contains("no_collider") && j["no_collider"].is_array()) {
        for (const json& n : j["no_collider"]) {
            if (n.is_number_integer()) t.no_collider.push_back(n.get<int>());
        }
    }
    t.rule_tiles.clear();
    if (j.contains("rule_tiles") && j["rule_tiles"].is_array()) {
        for (const json& rj : j["rule_tiles"]) {
            if (!rj.is_object()) continue;
            RuleTile r;
            r.name = rj.value("name", std::string("Rule Tile"));
            r.default_frame = rj.value("default_frame", 0);
            r.collider = rj.value("collider", true);
            if (rj.contains("rules") && rj["rules"].is_array()) {
                for (const json& rule_j : rj["rules"]) {
                    TileRule rule;
                    if (rule_j.contains("neighbors") && rule_j["neighbors"].is_array()) {
                        for (std::size_t i = 0; i < 8 && i < rule_j["neighbors"].size(); ++i) {
                            rule.neighbors[i] = static_cast<TileNeighbor>(std::clamp(rule_j["neighbors"][i].get<int>(), 0, 2));
                        }
                    }
                    if (rule_j.contains("frames") && rule_j["frames"].is_array()) {
                        for (const json& f : rule_j["frames"]) {
                            if (f.is_number_integer()) rule.frames.push_back(f.get<int>());
                        }
                    }
                    r.rules.push_back(rule);
                }
            }
            t.rule_tiles.push_back(std::move(r));
        }
    }
    return true;
}

bool loadTileset(const std::filesystem::path& file, Tileset& tileset) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    std::stringstream text;
    text << in.rdbuf();
    return tilesetFromJson(text.str(), tileset);
}

bool saveTileset(const std::filesystem::path& file, const Tileset& tileset, std::string* error) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << tilesetToJson(tileset);
    if (!out) {
        if (error != nullptr) *error = "no se pudo escribir " + file.string();
        return false;
    }
    return true;
}

RuleTile makeTerrainRuleTile(const std::string& name, int first_frame, int columns) {
    using N = TileNeighbor;
    const auto at = [&](int col, int row) { return first_frame + row * columns + col; };
    // Vecinos: 0 arriba-izq, 1 arriba, 2 arriba-der, 3 izq, 4 der, 5 abajo-izq, 6 abajo, 7 abajo-der.
    const auto rule = [](std::initializer_list<std::pair<int, N>> conditions, int frame) {
        TileRule r;
        for (const auto& [index, value] : conditions) r.neighbors[static_cast<std::size_t>(index)] = value;
        r.frames = {frame};
        return r;
    };
    RuleTile t;
    t.name = name;
    t.default_frame = at(1, 1);
    t.rules = {
        rule({{1, N::NotThis}, {3, N::NotThis}, {4, N::This}, {6, N::This}}, at(0, 0)),
        rule({{1, N::NotThis}, {4, N::NotThis}, {3, N::This}, {6, N::This}}, at(2, 0)),
        rule({{6, N::NotThis}, {3, N::NotThis}, {1, N::This}, {4, N::This}}, at(0, 2)),
        rule({{6, N::NotThis}, {4, N::NotThis}, {1, N::This}, {3, N::This}}, at(2, 2)),
        rule({{1, N::NotThis}}, at(1, 0)),
        rule({{6, N::NotThis}}, at(1, 2)),
        rule({{3, N::NotThis}}, at(0, 1)),
        rule({{4, N::NotThis}}, at(2, 1)),
    };
    return t;
}

const Tileset* TilesetLibrary::get(const std::string& relative) {
    if (relative.empty()) return nullptr;
    if (const auto it = sets_.find(relative); it != sets_.end()) return it->second.get();
    auto set = std::make_unique<Tileset>();
    const std::filesystem::path file = root_ / std::filesystem::path(std::u8string(relative.begin(), relative.end()));
    if (!loadTileset(file, *set)) {
        set.reset();
    } else {
        asset::ImageRgba8 image;
        const std::filesystem::path image_file = root_ / std::filesystem::path(std::u8string(set->image.begin(), set->image.end()));
        if (asset::loadImageRgba8(image_file, image)) {
            set->image_width = static_cast<int>(image.width);
            set->image_height = static_cast<int>(image.height);
        }
    }
    const Tileset* result = set.get();
    sets_[relative] = std::move(set);
    return result;
}

void TilesetLibrary::invalidate(const std::string& relative) {
    if (relative.empty()) {
        sets_.clear();
        return;
    }
    sets_.erase(relative);
}

// -----------------------------------------------------------------------------
// Datos de una capa
// -----------------------------------------------------------------------------

std::int32_t TileLayerData::get(int x, int y) const {
    const int cx = chunkCoord(x);
    const int cy = chunkCoord(y);
    const auto it = chunks_.find(key(cx, cy));
    if (it == chunks_.end()) return 0;
    const int lx = x - cx * kTileChunk;
    const int ly = y - cy * kTileChunk;
    return it->second.cells[static_cast<std::size_t>(ly * kTileChunk + lx)];
}

bool TileLayerData::set(int x, int y, std::int32_t id) {
    const int cx = chunkCoord(x);
    const int cy = chunkCoord(y);
    const std::int64_t k = key(cx, cy);
    auto it = chunks_.find(k);
    if (it == chunks_.end()) {
        if (id == 0) return false;
        it = chunks_.emplace(k, TileChunk{}).first;
    }
    const int lx = x - cx * kTileChunk;
    const int ly = y - cy * kTileChunk;
    std::int32_t& cell = it->second.cells[static_cast<std::size_t>(ly * kTileChunk + lx)];
    if (cell == id) return false;
    if (cell == 0) ++it->second.count;
    if (id == 0) --it->second.count;
    cell = id;
    if (it->second.count <= 0) chunks_.erase(it);
    return true;
}

std::size_t TileLayerData::cellCount() const {
    std::size_t n = 0;
    for (const auto& [k, chunk] : chunks_) n += static_cast<std::size_t>(chunk.count);
    return n;
}

bool TileLayerData::bounds(int& min_x, int& min_y, int& max_x, int& max_y) const {
    bool any = false;
    forEach([&](int x, int y, std::int32_t) {
        if (!any) {
            min_x = max_x = x;
            min_y = max_y = y;
            any = true;
            return;
        }
        min_x = std::min(min_x, x);
        min_y = std::min(min_y, y);
        max_x = std::max(max_x, x);
        max_y = std::max(max_y, y);
    });
    return any;
}

void TileLayerData::forEach(const std::function<void(int, int, std::int32_t)>& fn) const {
    for (const auto& [k, chunk] : chunks_) {
        const int cx = static_cast<int>(k >> 32);
        const int cy = static_cast<int>(static_cast<std::int32_t>(k & 0xFFFFFFFF));
        for (int ly = 0; ly < kTileChunk; ++ly) {
            for (int lx = 0; lx < kTileChunk; ++lx) {
                const std::int32_t id = chunk.cells[static_cast<std::size_t>(ly * kTileChunk + lx)];
                if (id != 0) fn(cx * kTileChunk + lx, cy * kTileChunk + ly, id);
            }
        }
    }
}

std::string TileLayerData::encode() const {
    // Orden estable (la escena no cambia si no cambian las celdas).
    std::vector<std::int64_t> keys;
    keys.reserve(chunks_.size());
    for (const auto& [k, chunk] : chunks_) keys.push_back(k);
    std::sort(keys.begin(), keys.end());
    std::string out = "v1;";
    for (const std::int64_t k : keys) {
        const TileChunk& chunk = chunks_.at(k);
        const int cx = static_cast<int>(k >> 32);
        const int cy = static_cast<int>(static_cast<std::int32_t>(k & 0xFFFFFFFF));
        out += std::to_string(cx) + "," + std::to_string(cy) + "=";
        std::size_t i = 0;
        bool first = true;
        while (i < chunk.cells.size()) {
            std::size_t run = 1;
            while (i + run < chunk.cells.size() && chunk.cells[i + run] == chunk.cells[i]) ++run;
            if (!first) out += ",";
            first = false;
            out += std::to_string(chunk.cells[i]);
            if (run > 1) out += "*" + std::to_string(run);
            i += run;
        }
        out += ";";
    }
    return out;
}

void TileLayerData::decode(const std::string& text) {
    chunks_.clear();
    if (text.rfind("v1;", 0) != 0) return;
    std::size_t pos = 3;
    while (pos < text.size()) {
        const std::size_t end = text.find(';', pos);
        const std::string part = text.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = end == std::string::npos ? text.size() : end + 1;
        const std::size_t eq = part.find('=');
        const std::size_t comma = part.find(',');
        if (eq == std::string::npos || comma == std::string::npos || comma > eq) continue;
        try {
            const int cx = std::stoi(part.substr(0, comma));
            const int cy = std::stoi(part.substr(comma + 1, eq - comma - 1));
            TileChunk chunk;
            std::size_t cell = 0;
            std::size_t p = eq + 1;
            while (p < part.size() && cell < chunk.cells.size()) {
                std::size_t next = part.find(',', p);
                if (next == std::string::npos) next = part.size();
                const std::string token = part.substr(p, next - p);
                p = next + 1;
                const std::size_t star = token.find('*');
                const std::int32_t id = static_cast<std::int32_t>(std::stol(token.substr(0, star)));
                std::size_t run = star == std::string::npos ? 1 : static_cast<std::size_t>(std::stoul(token.substr(star + 1)));
                for (; run > 0 && cell < chunk.cells.size(); --run) {
                    chunk.cells[cell++] = id;
                    if (id != 0) ++chunk.count;
                }
            }
            if (chunk.count > 0) chunks_[key(cx, cy)] = chunk;
        } catch (...) {
        }
    }
}

// -----------------------------------------------------------------------------
// Tilemap
// -----------------------------------------------------------------------------

std::int32_t Tilemap::getTile(int x, int y, int layer) const {
    if (layer < 0 || layer >= static_cast<int>(layers.size())) return 0;
    return layers[static_cast<std::size_t>(layer)].data.get(x, y);
}

bool Tilemap::setTile(int x, int y, std::int32_t id, int layer) {
    if (layer < 0 || layer >= static_cast<int>(layers.size())) return false;
    if (!layers[static_cast<std::size_t>(layer)].data.set(x, y, id)) return false;
    ++version;
    if (runtime.ptr) {
        TilemapRuntime& rt = *runtime.ptr;
        if (rt.layers.size() < layers.size()) rt.layers.resize(layers.size());
        // El trozo y los vecinos (las Rule Tiles de alrededor cambian de dibujo).
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const std::int64_t k = TileLayerData::key(TileLayerData::chunkCoord(x + dx), TileLayerData::chunkCoord(y + dy));
                rt.layers[static_cast<std::size_t>(layer)][k].dirty = true;
            }
        }
        ++rt.version;
    }
    return true;
}

void Tilemap::markAllDirty() {
    ++version;
    runtime.ptr.reset();
}

void Tilemap::clearLayer(int layer) {
    if (layer < 0 || layer >= static_cast<int>(layers.size())) return;
    layers[static_cast<std::size_t>(layer)].data.clear();
    markAllDirty();
}

void Tilemap::cellAt(const core::Vec3& local, int& x, int& y) const {
    x = static_cast<int>(std::floor(local.x / std::max(cell_size.x, 1e-4f)));
    y = static_cast<int>(std::floor(local.y / std::max(cell_size.y, 1e-4f)));
}

core::Vec3 Tilemap::cellCenter(int x, int y) const {
    return core::Vec3{(static_cast<float>(x) + 0.5f) * cell_size.x, (static_cast<float>(y) + 0.5f) * cell_size.y, 0.0f};
}

void Tilemap::reflect(ecs::PropertyVisitor& v) {
    v.field({"tileset", "Tileset", "Archivo .crtileset de Assets (la imagen cortada en celdas y sus Rule Tiles)"}, tileset);
    v.field({"cell_size", "Tamano de celda", "Unidades por celda"}, cell_size, 0.01f);
    v.field({"sorting_layer", "Capa de orden"}, sorting_layer);
    v.field({"order_in_layer", "Orden en la capa"}, order_in_layer, -32768, 32767);
    v.field({"lit", "Iluminado", "Le afectan las Light2D"}, lit);
    bool cells_changed = false;
    ecs::listField(v, {"layers", "Capas", "Fondo, suelo, detalles... (la paleta pinta en la elegida)"}, layers,
                   [&cells_changed](TilemapLayer& l, ecs::PropertyVisitor& item) {
                       item.field({"name", "Nombre"}, l.name);
                       item.field({"visible", "Visible"}, l.visible);
                       item.field({"collision", "Colision", "Sus celdas cuentan para el Tilemap Collider 2D"}, l.collision);
                       item.field({"order", "Orden", "Se suma al orden en la capa del Tilemap"}, l.order, -1000, 1000);
                       item.field({"tint", "Tinte"}, l.tint, ecs::Vec3Kind::Color);
                       item.field({"opacity", "Opacidad"}, l.opacity, ecs::FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
                       // Las celdas: solo al guardar/cargar (el Inspector no las ensena).
                       if (item.wantsAllFields()) {
                           std::string cells = l.data.encode();
                           if (item.field({"cells", "Celdas"}, cells)) {
                               l.data.decode(cells);
                               cells_changed = true;
                           }
                       }
                   });
    cell_size.x = std::max(cell_size.x, 0.001f);
    cell_size.y = std::max(cell_size.y, 0.001f);
    if (cells_changed) markAllDirty();
}

int floodFill(Tilemap& map, int layer, int x, int y, std::int32_t id, int min_x, int min_y, int max_x, int max_y,
              int limit) {
    if (layer < 0 || layer >= static_cast<int>(map.layers.size())) return 0;
    if (x < min_x || y < min_y || x > max_x || y > max_y) return 0;
    const std::int32_t target = map.getTile(x, y, layer);
    if (target == id) return 0;
    std::deque<std::pair<int, int>> queue;
    std::unordered_set<std::int64_t> visited;
    queue.emplace_back(x, y);
    visited.insert(TileLayerData::key(x, y));
    int changed = 0;
    while (!queue.empty() && changed < limit) {
        const auto [cx, cy] = queue.front();
        queue.pop_front();
        if (map.getTile(cx, cy, layer) != target) continue;
        if (map.setTile(cx, cy, id, layer)) ++changed;
        static constexpr int kDirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (const auto& d : kDirs) {
            const int nx = cx + d[0];
            const int ny = cy + d[1];
            if (nx < min_x || ny < min_y || nx > max_x || ny > max_y) continue;
            if (!visited.insert(TileLayerData::key(nx, ny)).second) continue;
            queue.emplace_back(nx, ny);
        }
    }
    return changed;
}

int resolveRuleTile(const Tileset& tileset, const TileLayerData& data, int x, int y, int rule_index) {
    if (rule_index < 0 || rule_index >= static_cast<int>(tileset.rule_tiles.size())) return 0;
    const RuleTile& rule_tile = tileset.rule_tiles[static_cast<std::size_t>(rule_index)];
    const std::int32_t self = -(rule_index + 1);
    static constexpr int kOffsets[8][2] = {{-1, 1}, {0, 1}, {1, 1}, {-1, 0}, {1, 0}, {-1, -1}, {0, -1}, {1, -1}};
    bool same[8];
    for (int i = 0; i < 8; ++i) same[i] = data.get(x + kOffsets[i][0], y + kOffsets[i][1]) == self;
    for (const TileRule& rule : rule_tile.rules) {
        bool ok = true;
        for (int i = 0; i < 8 && ok; ++i) {
            const TileNeighbor n = rule.neighbors[static_cast<std::size_t>(i)];
            if (n == TileNeighbor::This && !same[i]) ok = false;
            if (n == TileNeighbor::NotThis && same[i]) ok = false;
        }
        if (!ok || rule.frames.empty()) continue;
        if (rule.frames.size() == 1) return rule.frames.front();
        // Variacion estable por posicion (no parpadea al repintar).
        std::uint32_t h = static_cast<std::uint32_t>(x) * 73856093u ^ static_cast<std::uint32_t>(y) * 19349663u;
        h ^= h >> 13;
        h *= 0x5bd1e995u;
        return rule.frames[h % rule.frames.size()];
    }
    return rule_tile.default_frame;
}

namespace {

// Celda del tileset que dibuja `id` en (x, y), o -1.
int frameOf(const Tileset& tileset, const TileLayerData& data, int x, int y, std::int32_t id) {
    if (id > 0) return id - 1;
    if (id < 0) return resolveRuleTile(tileset, data, x, y, -id - 1);
    return -1;
}

bool cellSolid(const Tileset* tileset, std::int32_t id) {
    if (id == 0) return false;
    if (tileset == nullptr) return true;
    if (id < 0) {
        const int r = -id - 1;
        return r < static_cast<int>(tileset->rule_tiles.size()) ? tileset->rule_tiles[static_cast<std::size_t>(r)].collider : true;
    }
    return tileset->frameSolid(id - 1);
}

std::uint64_t tilesetFingerprint(const Tileset& tileset) {
    const std::string text = tilesetToJson(tileset) + std::to_string(tileset.image_width) + "x" +
                             std::to_string(tileset.image_height);
    return std::hash<std::string>{}(text);
}

}  // namespace

std::vector<TileRect> solidRects(const Tilemap& map, const Tileset* tileset, bool merge_vertical) {
    // Celdas solidas de todas las capas con colision, por filas.
    std::map<int, std::vector<int>> rows;
    for (const TilemapLayer& layer : map.layers) {
        if (!layer.collision) continue;
        layer.data.forEach([&](int x, int y, std::int32_t id) {
            if (cellSolid(tileset, id)) rows[y].push_back(x);
        });
    }
    std::vector<TileRect> rects;
    std::map<std::pair<int, int>, std::size_t> open;  // tramo (x0, x1) de la fila anterior -> rectangulo
    int previous_y = 0;
    bool has_previous = false;
    for (auto& [y, xs] : rows) {
        std::sort(xs.begin(), xs.end());
        xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
        std::map<std::pair<int, int>, std::size_t> now;
        std::size_t i = 0;
        while (i < xs.size()) {
            std::size_t j = i;
            while (j + 1 < xs.size() && xs[j + 1] == xs[j] + 1) ++j;
            const std::pair<int, int> run{xs[i], xs[j]};
            const auto it = open.find(run);
            if (merge_vertical && has_previous && previous_y == y - 1 && it != open.end()) {
                rects[it->second].h += 1;
                now[run] = it->second;
            } else {
                rects.push_back(TileRect{run.first, y, run.second - run.first + 1, 1});
                now[run] = rects.size() - 1;
            }
            i = j + 1;
        }
        open = std::move(now);
        previous_y = y;
        has_previous = true;
    }
    return rects;
}

void buildTilemapMeshes(Tilemap& map, const Tileset& tileset) {
    if (!map.runtime.ptr) map.runtime.ptr = std::make_shared<TilemapRuntime>();
    TilemapRuntime& rt = *map.runtime.ptr;
    const std::uint64_t fingerprint = tilesetFingerprint(tileset);
    if (rt.built_tileset != fingerprint) {
        rt.layers.clear();
        rt.built_tileset = fingerprint;
    }
    if (rt.layers.size() != map.layers.size()) rt.layers.resize(map.layers.size());
    const float cw = map.cell_size.x;
    const float ch = map.cell_size.y;
    for (std::size_t li = 0; li < map.layers.size(); ++li) {
        const TilemapLayer& layer = map.layers[li];
        auto& meshes = rt.layers[li];
        // Trozos que ya no existen fuera.
        for (auto it = meshes.begin(); it != meshes.end();) {
            if (!layer.data.chunks().contains(it->first)) {
                it = meshes.erase(it);
            } else {
                ++it;
            }
        }
        for (const auto& [k, chunk] : layer.data.chunks()) {
            auto found = meshes.find(k);
            if (found != meshes.end() && !found->second.dirty) continue;
            TilemapRuntime::ChunkMesh& mesh = meshes[k];
            mesh.quads.clear();
            mesh.dirty = false;
            const int cx = static_cast<int>(k >> 32);
            const int cy = static_cast<int>(static_cast<std::int32_t>(k & 0xFFFFFFFF));
            for (int ly = 0; ly < kTileChunk; ++ly) {
                for (int lx = 0; lx < kTileChunk; ++lx) {
                    const std::int32_t id = chunk.cells[static_cast<std::size_t>(ly * kTileChunk + lx)];
                    if (id == 0) continue;
                    const int x = cx * kTileChunk + lx;
                    const int y = cy * kTileChunk + ly;
                    const int frame = frameOf(tileset, layer.data, x, y, id);
                    if (frame < 0) continue;
                    core::Vec2 uv0{};
                    core::Vec2 uv1{};
                    tileset.frameUv(frame, uv0, uv1);
                    gfx::SpriteQuad q;
                    const float x0 = static_cast<float>(x) * cw;
                    const float y0 = static_cast<float>(y) * ch;
                    q.corners[0] = core::Vec3{x0, y0, 0.0f};
                    q.corners[1] = core::Vec3{x0 + cw, y0, 0.0f};
                    q.corners[2] = core::Vec3{x0 + cw, y0 + ch, 0.0f};
                    q.corners[3] = core::Vec3{x0, y0 + ch, 0.0f};
                    q.uvs[0] = core::Vec2{uv0.x, uv1.y};
                    q.uvs[1] = core::Vec2{uv1.x, uv1.y};
                    q.uvs[2] = core::Vec2{uv1.x, uv0.y};
                    q.uvs[3] = core::Vec2{uv0.x, uv0.y};
                    mesh.quads.push_back(q);
                }
            }
        }
    }
}

void TilemapCollider2D::reflect(ecs::PropertyVisitor& v) {
    v.field({"is_trigger", "Es trigger"}, is_trigger);
    v.field({"friction", "Friccion"}, friction, ecs::FloatRange{0.0f, 2.0f, 0.01f, "%.2f"});
    v.field({"bounciness", "Rebote"}, bounciness, ecs::FloatRange{0.0f, 1.0f, 0.01f, "%.2f", true});
    v.field({"one_way", "Un sentido", "Plataformas que se atraviesan saltando desde abajo"}, one_way);
    v.field({"used_by_composite", "Usado por el Composite", "Lo junta el Composite Collider 2D de la entidad"},
            used_by_composite);
}

}  // namespace cramion::twod

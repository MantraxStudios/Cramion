// Pintar prefabs (como la herramienta Foliage de Unreal, pero con prefabs de
// verdad: arboles con su script, rocas con su collider, cofres...):
//
//   - Un grupo de pintado (.crpaint en Assets/PaintGroups) es una lista de
//     prefabs con su peso (cuantos salen de cada uno), escala aleatoria,
//     alineado a la superficie, giro aleatorio y hundimiento.
//   - Se pinta el grupo entero (cada objeto elige un prefab por peso) o solo
//     el prefab elegido.
//   - Pincel: radio, densidad (objetos por 100 m2: repasar no amontona, se
//     rellena hasta la densidad, como en Unreal), separacion minima entre
//     objetos y pendiente maxima. Mayus + arrastrar borra.
//   - Cada trazo es un paso de deshacer. Los objetos quedan como instancias
//     normales de su prefab, bajo "Pintado - <grupo>" en la Jerarquia.
//
// La superficie se busca con la fisica (colliders, Mesh Collider del suelo)
// y los terrenos; lo ya pintado de la paleta no cuenta como suelo (no salen
// arboles encima de arboles).

#include "EditorApp.h"

#include "Dialogs.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <ImGuizmo.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>

namespace cramion::editor {

using core::Mat4;
using core::Quat;
using core::Vec3;
using json = nlohmann::json;

namespace {

constexpr const char* kPaintExtension = ".crpaint";
constexpr const char* kPaintFolder = "PaintGroups";
constexpr ImU32 kPaintColor = IM_COL32(90, 220, 120, 255);
constexpr ImU32 kEraseColor = IM_COL32(240, 80, 70, 255);

// Base ortonormal con `up` como eje Y, girada `yaw` radianes alrededor de el.
Mat4 basisMatrix(const Vec3& up, float yaw) {
    const Vec3 helper = std::abs(up.z) < 0.95f ? Vec3{0.0f, 0.0f, 1.0f} : Vec3{1.0f, 0.0f, 0.0f};
    Vec3 right = core::normalize(core::cross(up, helper));
    Vec3 forward = core::cross(right, up);
    const Vec3 r = right * std::cos(yaw) - forward * std::sin(yaw);
    const Vec3 f = forward * std::cos(yaw) + right * std::sin(yaw);
    Mat4 m = Mat4::identity();
    m.m[0][0] = r.x; m.m[0][1] = r.y; m.m[0][2] = r.z;
    m.m[1][0] = up.x; m.m[1][1] = up.y; m.m[1][2] = up.z;
    m.m[2][0] = f.x; m.m[2][1] = f.y; m.m[2][2] = f.z;
    return m;
}

// Dos ejes del plano perpendicular a `n`.
void planeAxes(const Vec3& n, Vec3& u, Vec3& v) {
    const Vec3 helper = std::abs(n.y) < 0.95f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
    u = core::normalize(core::cross(helper, n));
    v = core::cross(n, u);
}

}  // namespace

// -----------------------------------------------------------------------------
// Grupos (.crpaint)
// -----------------------------------------------------------------------------

std::vector<std::filesystem::path> EditorApp::paintGroupFiles() const {
    std::vector<std::filesystem::path> files;
    if (!has_project_) return files;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(project_.assetsFolder(), ec), end; !ec && it != end;
         it.increment(ec)) {
        if (it->is_regular_file(ec) && it->path().extension() == kPaintExtension) files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

bool EditorApp::loadPaintGroup(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    const json j = json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        std::cerr << "[Pintar] Grupo danado: " << dialogs::utf8(file) << "\n";
        return false;
    }
    PaintGroup group;
    group.file = file;
    group.name = j.value("name", dialogs::utf8(file.stem()));
    if (const auto items = j.find("items"); items != j.end() && items->is_array()) {
        for (const json& i : *items) {
            PaintItem item;
            item.prefab = Uuid::parse(i.value("prefab", std::string{}));
            if (!item.prefab.valid()) continue;
            item.enabled = i.value("enabled", true);
            item.weight = i.value("weight", 1.0f);
            item.scale_min = i.value("scale_min", 0.9f);
            item.scale_max = i.value("scale_max", 1.1f);
            item.align = i.value("align", 0.0f);
            item.random_yaw = i.value("random_yaw", true);
            item.sink = i.value("sink", 0.0f);
            group.items.push_back(item);
        }
    }
    const json& b = j.contains("brush") ? j["brush"] : json::object();
    paint_brush_.radius = b.value("radius", paint_brush_.radius);
    paint_brush_.density = b.value("density", paint_brush_.density);
    paint_brush_.spacing = b.value("spacing", paint_brush_.spacing);
    paint_brush_.max_slope = b.value("max_slope", paint_brush_.max_slope);
    paint_group_ = std::move(group);
    paint_selected_ = paint_group_.items.empty() ? -1 : 0;
    return true;
}

void EditorApp::savePaintGroup() {
    if (paint_group_.file.empty()) return;
    json items = json::array();
    for (const PaintItem& i : paint_group_.items) {
        items.push_back({{"prefab", i.prefab.toString()},
                         {"enabled", i.enabled},
                         {"weight", i.weight},
                         {"scale_min", i.scale_min},
                         {"scale_max", i.scale_max},
                         {"align", i.align},
                         {"random_yaw", i.random_yaw},
                         {"sink", i.sink}});
    }
    const json j = {{"format", "CramionPaintGroup"},
                    {"version", 1},
                    {"name", paint_group_.name},
                    {"items", items},
                    {"brush",
                     {{"radius", paint_brush_.radius},
                      {"density", paint_brush_.density},
                      {"spacing", paint_brush_.spacing},
                      {"max_slope", paint_brush_.max_slope}}}};
    std::error_code ec;
    std::filesystem::create_directories(paint_group_.file.parent_path(), ec);
    std::ofstream out(paint_group_.file, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "[Pintar] No se pudo guardar " << dialogs::utf8(paint_group_.file) << "\n";
        return;
    }
    out << j.dump(2);
}

void EditorApp::newPaintGroup() {
    const std::filesystem::path folder = project_.assetsFolder() / kPaintFolder;
    std::filesystem::path file = folder / (std::string("Grupo") + kPaintExtension);
    for (int i = 2; std::filesystem::exists(file); ++i) {
        file = folder / ("Grupo " + std::to_string(i) + kPaintExtension);
    }
    paint_group_ = PaintGroup{};
    paint_group_.file = file;
    paint_group_.name = dialogs::utf8(file.stem());
    paint_selected_ = -1;
    savePaintGroup();
    refreshDatabase();
}

void EditorApp::addPaintItem(const Uuid& prefab) {
    if (!prefab.valid()) return;
    for (std::size_t i = 0; i < paint_group_.items.size(); ++i) {
        if (paint_group_.items[i].prefab == prefab) {
            paint_selected_ = static_cast<int>(i);
            return;
        }
    }
    if (paint_group_.file.empty()) newPaintGroup();
    PaintItem item;
    item.prefab = prefab;
    paint_group_.items.push_back(item);
    paint_selected_ = static_cast<int>(paint_group_.items.size()) - 1;
    savePaintGroup();
}

// Prefabs que pinta el pincel ahora (todo el grupo o solo el elegido).
std::vector<int> EditorApp::activePaintItems() const {
    std::vector<int> active;
    for (int i = 0; i < static_cast<int>(paint_group_.items.size()); ++i) {
        const PaintItem& item = paint_group_.items[static_cast<std::size_t>(i)];
        if (!item.enabled || item.weight <= 0.0f) continue;
        if (paint_brush_.only_selected && i != paint_selected_) continue;
        active.push_back(i);
    }
    return active;
}

// -----------------------------------------------------------------------------
// Escena
// -----------------------------------------------------------------------------

// Instancias (raices) de los prefabs del grupo que hay en la escena.
std::vector<ecs::Entity> EditorApp::paintedInstances(bool active_only) const {
    std::vector<ecs::Entity> out;
    std::vector<Uuid> prefabs;
    if (active_only) {
        for (const int i : activePaintItems()) prefabs.push_back(paint_group_.items[static_cast<std::size_t>(i)].prefab);
    } else {
        for (const PaintItem& item : paint_group_.items) prefabs.push_back(item.prefab);
    }
    if (prefabs.empty()) return out;
    for (const entt::entity h : world_.registry().view<ecs::PrefabInstance>()) {
        const ecs::Entity e = world_.wrap(h);
        const Uuid& uuid = e.get<ecs::PrefabInstance>().prefab.uuid;
        if (std::any_of(prefabs.begin(), prefabs.end(), [&](const Uuid& p) { return p == uuid; })) out.push_back(e);
    }
    return out;
}

bool EditorApp::isPaintedEntity(ecs::Entity e) const {
    const ecs::Entity root = ecs::prefabRoot(e);
    if (!root.valid()) return false;
    const Uuid& uuid = root.get<ecs::PrefabInstance>().prefab.uuid;
    return std::any_of(paint_group_.items.begin(), paint_group_.items.end(),
                       [&](const PaintItem& item) { return item.prefab == uuid; });
}

// El objeto "Pintado - <grupo>" (en la raiz), creado si falta.
ecs::Entity EditorApp::paintContainer() {
    const std::string name = "Pintado - " + paint_group_.name;
    for (const entt::entity h : world_.roots()) {
        const ecs::Entity e = world_.wrap(h);
        if (e.name() == name) return e;
    }
    return world_.create(name);
}

// Superficie donde apoyar: colliders solidos (lo ya pintado no cuenta) y
// terrenos. `point`/`normal` del impacto mas cercano.
bool EditorApp::paintRaycast(const Vec3& origin, const Vec3& direction, float max_distance, Vec3& point,
                             Vec3& normal) const {
    float best = max_distance;
    bool found = false;
    if (physics_.running()) {
        physics::QueryFilter filter;
        filter.triggers = physics::QueryTriggers::Ignore;
        filter.record = false;
        for (const physics::RaycastHit& hit : physics_.raycastAll(origin, direction, max_distance, filter)) {
            if (hit.trigger || hit.distance >= best) continue;
            if (hit.entity.valid() && isPaintedEntity(hit.entity)) continue;
            best = hit.distance;
            point = hit.point;
            normal = core::normalize(hit.normal);
            found = true;
        }
    }
    for (const entt::entity h : world_.registry().view<terrain::Terrain>()) {
        const ecs::Entity e = world_.wrap(h);
        if (!e.activeInHierarchy()) continue;
        const terrain::Terrain& comp = e.get<terrain::Terrain>();
        const std::shared_ptr<terrain::TerrainData> data = terrain_store_.find(comp);
        if (!data) continue;
        Vec3 hit{};
        if (!terrain::raycast(*data, comp, e.worldPosition(), origin, direction, best, hit)) continue;
        const float distance = core::length(hit - origin);
        if (distance >= best) continue;
        best = distance;
        point = hit;
        normal = terrain::normalAt(*data, comp, e.worldPosition(), hit.x, hit.z);
        found = true;
    }
    return found;
}

// Un toque del pincel en `center` (sobre una superficie de normal `n`).
// Devuelve cuantos objetos se pusieron o quitaron.
int EditorApp::paintStamp(const Vec3& center, const Vec3& n, bool erase) {
    const PaintBrush& b = paint_brush_;
    const float radius = std::max(b.radius, 0.05f);
    const auto in_brush = [&](const Vec3& p) {
        Vec3 d = p - center;
        const float along = core::dot(d, n);
        d = d - n * along;
        return core::dot(d, d) <= radius * radius && std::abs(along) <= radius * 2.0f;
    };

    std::vector<ecs::Entity> existing = paintedInstances(true);
    if (erase) {
        int removed = 0;
        for (const ecs::Entity e : existing) {
            if (!e.valid() || !in_brush(e.worldPosition())) continue;
            world_.destroy(e);
            ++removed;
        }
        if (removed > 0) dirty_ = true;
        return removed;
    }

    const std::vector<int> active = activePaintItems();
    if (active.empty()) return 0;
    float total_weight = 0.0f;
    for (const int i : active) total_weight += paint_group_.items[static_cast<std::size_t>(i)].weight;

    // Lo que ya hay cerca (tambien de otros prefabs del grupo: la separacion
    // es entre todos).
    std::vector<Vec3> nearby;
    int inside = 0;
    const float reach = radius + b.spacing;
    for (const ecs::Entity e : paintedInstances(false)) {
        const Vec3 p = e.worldPosition();
        if (core::length(p - center) <= reach + radius) nearby.push_back(p);
        if (in_brush(p)) {
            for (const ecs::Entity a : existing) {
                if (a == e) {
                    ++inside;
                    break;
                }
            }
        }
    }

    // Cuantos faltan para la densidad pedida (redondeo al azar: con poca
    // densidad tambien sale alguno).
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    const float wanted = std::max(b.density, 0.0f) * core::kPi * radius * radius / 100.0f - static_cast<float>(inside);
    int to_add = static_cast<int>(std::floor(std::max(wanted, 0.0f) + unit(paint_rng_)));
    to_add = std::min(to_add, 500);
    if (to_add <= 0) return 0;

    Vec3 u{};
    Vec3 v{};
    planeAxes(n, u, v);
    const float min_up = std::cos(std::clamp(b.max_slope, 0.0f, 90.0f) * core::kPi / 180.0f);
    ecs::Entity container = b.group_under_parent ? paintContainer() : ecs::Entity{};
    int added = 0;
    for (int attempt = 0; attempt < to_add * 8 && added < to_add; ++attempt) {
        // Punto al azar en el disco (uniforme en area).
        const float rr = radius * std::sqrt(unit(paint_rng_));
        const float angle = unit(paint_rng_) * 2.0f * core::kPi;
        const Vec3 p = center + u * (std::cos(angle) * rr) + v * (std::sin(angle) * rr);
        Vec3 hit{};
        Vec3 hit_normal{};
        if (!paintRaycast(p + n * (radius * 1.5f + 2.0f), n * -1.0f, radius * 3.0f + 4.0f, hit, hit_normal)) continue;
        if (hit_normal.y < min_up) continue;  // demasiado inclinado
        bool crowded = false;
        for (const Vec3& q : nearby) {
            if (core::length(q - hit) < b.spacing) {
                crowded = true;
                break;
            }
        }
        if (crowded) continue;

        // Prefab por peso.
        float pick = unit(paint_rng_) * total_weight;
        int index = active.back();
        for (const int i : active) {
            pick -= paint_group_.items[static_cast<std::size_t>(i)].weight;
            if (pick <= 0.0f) {
                index = i;
                break;
            }
        }
        const PaintItem& item = paint_group_.items[static_cast<std::size_t>(index)];
        const std::string& text = prefabText(item.prefab);
        if (text.empty()) continue;
        ecs::Entity root = ecs::instantiatePrefab(world_, text, container);
        if (!root.valid()) continue;

        // Giro: vertical o inclinado con la superficie (align), mas el giro
        // aleatorio; encima del giro y la escala propios del prefab.
        const Vec3 up = core::normalize(Vec3{0.0f, 1.0f, 0.0f} * (1.0f - item.align) + hit_normal * item.align);
        const float yaw = item.random_yaw ? unit(paint_rng_) * 2.0f * core::kPi : 0.0f;
        const Quat paint_rotation = ecs::quatFromRotationMatrix(basisMatrix(up, yaw));
        const float lo = std::min(item.scale_min, item.scale_max);
        const float hi = std::max(item.scale_min, item.scale_max);
        const float s = std::max(lo + (hi - lo) * unit(paint_rng_), 0.001f);
        const Quat rotation = ecs::quatMultiply(paint_rotation, root.localRotation());
        const Vec3 scale = root.localScale() * s;
        root.setWorldMatrix(core::composeTrs(hit - up * item.sink, rotation, scale));
        nearby.push_back(hit);
        ++added;
    }
    if (added > 0) dirty_ = true;
    return added;
}

// -----------------------------------------------------------------------------
// Pincel en la vista Escena
// -----------------------------------------------------------------------------

void EditorApp::drawPaintToolbar() {
    ImGui::SameLine();
    const bool active = paint_mode_;
    if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button("B Pintar")) {
        paint_mode_ = !paint_mode_;
        if (paint_mode_) {
            stamp_mode_ = false;
            show_paint_window_ = true;
        }
    }
    if (active) ImGui::PopStyleColor();
    ImGui::SetItemTooltip("Pintar prefabs (árboles, rocas...) con un pincel (B). Mayús + arrastrar borra.");
}

bool EditorApp::drawPrefabPaintTool() {
    ImGuiIO& io = ImGui::GetIO();
    const auto end_stroke = [&]() {
        if (!paint_stroke_) return;
        paint_stroke_ = false;
        commit();  // un paso de deshacer por trazo
    };
    if (!paint_mode_ || flying_ || playing()) {
        end_stroke();
        return false;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && (view_hovered_ || view_focused_)) {
        paint_mode_ = false;
        end_stroke();
        return false;
    }
    if (!view_hovered_ && !paint_stroke_) return true;

    PaintBrush& b = paint_brush_;
    // Ctrl + rueda o [ ] = radio.
    if (view_hovered_ && io.MouseWheel != 0.0f && io.KeyCtrl) {
        b.radius = std::clamp(b.radius * (io.MouseWheel > 0 ? 1.1f : 1.0f / 1.1f), 0.2f, 200.0f);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) b.radius = std::max(b.radius / 1.1f, 0.2f);
    if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) b.radius = std::min(b.radius * 1.1f, 200.0f);

    Vec3 origin{};
    Vec3 direction{};
    Vec3 point{};
    Vec3 normal{0.0f, 1.0f, 0.0f};
    bool hit = mouseRay(io.MousePos.x, io.MousePos.y, origin, direction) &&
               paintRaycast(origin, direction, 5000.0f, point, normal);
    if (!hit) hit = surfaceHit(io.MousePos.x, io.MousePos.y, point, normal);
    if (!hit) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) end_stroke();
        return true;
    }

    const bool erase = io.KeyShift || b.erase;
    const ImU32 color = erase ? kEraseColor : kPaintColor;
    // Vista previa en 3D (con profundidad): el circulo del pincel, uno
    // interior y la normal.
    Vec3 u{};
    Vec3 v{};
    planeAxes(normal, u, v);
    const Vec3 lift = normal * 0.05f;
    overlayCircle(point + lift, u, v, b.radius, color, false, 72);
    overlayCircle(point + lift, u, v, b.radius * 0.97f, (color & 0x00FFFFFFu) | 0x60000000u, false, 72);
    overlayLine(point, point + normal * std::min(b.radius * 0.4f, 2.0f), color);

    const std::vector<int> active = activePaintItems();
    if (active.empty()) {
        ImGui::SetTooltip("Arrastra prefabs a la ventana Pintar prefabs");
    } else if (!paint_stroke_) {
        const std::string what = b.only_selected && paint_selected_ >= 0
                                     ? ecs::prefabName(prefabText(paint_group_.items[static_cast<std::size_t>(paint_selected_)].prefab))
                                     : paint_group_.name;
        ImGui::SetTooltip("%s %s   %.1f m", erase ? "Borrar" : "Pintar", what.c_str(), b.radius);
    }

    if (!paint_stroke_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && view_hovered_ && !ImGuizmo::IsOver() &&
        !io.KeyAlt && !active.empty()) {
        paint_stroke_ = true;
        paintStamp(point, normal, erase);
        paint_last_ = point;
    } else if (paint_stroke_ && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        // Un toque cada cierto trecho (no en cada frame: con el raton quieto
        // no se amontona).
        if (core::length(point - paint_last_) >= std::max(b.radius * 0.3f, 0.15f)) {
            paintStamp(point, normal, erase);
            paint_last_ = point;
        }
    }
    if (paint_stroke_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) end_stroke();
    return true;
}

// Nombre de un prefab del grupo (o su UUID si ya no existe).
std::string EditorApp::paintItemName(const PaintItem& item) {
    const std::string& text = prefabText(item.prefab);
    if (text.empty()) return "(falta) " + item.prefab.toString().substr(0, 8);
    return ecs::prefabName(text);
}

// -----------------------------------------------------------------------------
// Ventana
// -----------------------------------------------------------------------------

void EditorApp::drawPaintWindow() {
    if (!show_paint_window_) return;
    ImGui::SetNextWindowSize(ImVec2(360.0f, 620.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Pintar prefabs", &show_paint_window_)) {
        ImGui::End();
        return;
    }
    if (!has_project_) {
        ImGui::TextDisabled("Abre un proyecto.");
        ImGui::End();
        return;
    }
    bool changed = false;

    // --- Modo ---
    {
        const bool active = paint_mode_;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button(paint_mode_ ? "Pintando (B)" : "Pintar (B)", ImVec2(ImGui::GetContentRegionAvail().x * 0.5f, 0.0f))) {
            paint_mode_ = !paint_mode_;
            if (paint_mode_) stamp_mode_ = false;
        }
        if (active) ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::Checkbox("Borrar", &paint_brush_.erase);
        ImGui::SetItemTooltip("También con Mayús mientras pintas.");
    }

    // --- Grupo ---
    ImGui::SeparatorText("Grupo");
    const std::vector<std::filesystem::path> files = paintGroupFiles();
    if (paint_group_.file.empty() && !files.empty()) loadPaintGroup(files.front());
    ImGui::SetNextItemWidth(-70.0f);
    if (ImGui::BeginCombo("##grupo", paint_group_.file.empty() ? "(ninguno)" : paint_group_.name.c_str())) {
        for (const std::filesystem::path& file : files) {
            const std::string label = dialogs::utf8(std::filesystem::relative(file, project_.assetsFolder()));
            if (ImGui::Selectable(label.c_str(), file == paint_group_.file)) loadPaintGroup(file);
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Nuevo")) newPaintGroup();
    if (!paint_group_.file.empty()) {
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputText("##nombre_grupo", &paint_group_.name);
        if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
    }

    // --- Prefabs del grupo ---
    ImGui::SeparatorText("Prefabs");
    // Zona para soltar prefabs del panel Proyecto.
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.35f, 0.25f, 0.6f));
    ImGui::Button("Arrastra aquí prefabs del Proyecto", ImVec2(-1.0f, 34.0f));
    ImGui::PopStyleColor();
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAssetPayload)) {
            AssetPayload asset{};
            std::memcpy(&asset, payload->Data, sizeof(asset));
            if (asset.type == assets::AssetType::Prefab) addPaintItem(asset.uuid);
        }
        ImGui::EndDragDropTarget();
    }
    if (ImGui::Button("Añadir los prefabs seleccionados", ImVec2(-1.0f, 0.0f))) {
        for (const ecs::Entity e : selectedEntities()) {
            const ecs::Entity root = ecs::prefabRoot(e);
            if (root.valid()) addPaintItem(root.get<ecs::PrefabInstance>().prefab.uuid);
        }
    }
    ImGui::SetItemTooltip("Los prefabs de los objetos seleccionados en la Jerarquía.");

    int remove = -1;
    if (ImGui::BeginTable("##paint_items", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("on", ImGuiTableColumnFlags_WidthFixed, 24.0f);
        ImGui::TableSetupColumn("nombre", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("peso", ImGuiTableColumnFlags_WidthFixed, 92.0f);
        for (int i = 0; i < static_cast<int>(paint_group_.items.size()); ++i) {
            PaintItem& item = paint_group_.items[static_cast<std::size_t>(i)];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Checkbox("##on", &item.enabled)) changed = true;
            ImGui::TableNextColumn();
            if (ImGui::Selectable(paintItemName(item).c_str(), paint_selected_ == i)) paint_selected_ = i;
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("Quitar del grupo")) remove = i;
                ImGui::EndPopup();
            }
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::DragFloat("##peso", &item.weight, 0.05f, 0.0f, 100.0f, "peso %.2f")) changed = true;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (remove >= 0) {
        paint_group_.items.erase(paint_group_.items.begin() + remove);
        paint_selected_ = std::min(paint_selected_, static_cast<int>(paint_group_.items.size()) - 1);
        changed = true;
    }
    if (paint_group_.items.empty()) {
        ImGui::TextWrapped("Crea prefabs de tus árboles, rocas o arbustos (arrastrándolos de la Jerarquía al Proyecto) y arrástralos aquí.");
    }

    // Pintar todo el grupo o solo el elegido.
    int what = paint_brush_.only_selected ? 1 : 0;
    ImGui::RadioButton("Todo el grupo", &what, 0);
    ImGui::SameLine();
    ImGui::RadioButton("Solo el elegido", &what, 1);
    paint_brush_.only_selected = what == 1;

    // --- Ajustes del prefab elegido ---
    if (paint_selected_ >= 0 && paint_selected_ < static_cast<int>(paint_group_.items.size())) {
        PaintItem& item = paint_group_.items[static_cast<std::size_t>(paint_selected_)];
        ImGui::SeparatorText(("Elegido: " + paintItemName(item)).c_str());
        float scale[2] = {item.scale_min, item.scale_max};
        ImGui::SetNextItemWidth(-110.0f);
        if (ImGui::DragFloat2("Escala mín/máx", scale, 0.01f, 0.01f, 20.0f, "%.2f")) {
            item.scale_min = scale[0];
            item.scale_max = scale[1];
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
        ImGui::SetNextItemWidth(-110.0f);
        ImGui::SliderFloat("Alinear al suelo", &item.align, 0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
        ImGui::SetItemTooltip("0 = siempre vertical (árboles), 1 = sigue la inclinación del suelo (rocas, hierba).");
        ImGui::SetNextItemWidth(-110.0f);
        ImGui::DragFloat("Hundir (m)", &item.sink, 0.01f, -5.0f, 5.0f, "%.2f");
        if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
        ImGui::SetItemTooltip("Mete el objeto en el suelo (raíces, rocas medio enterradas).");
        if (ImGui::Checkbox("Giro aleatorio", &item.random_yaw)) changed = true;
    }

    // --- Pincel ---
    ImGui::SeparatorText("Pincel");
    PaintBrush& b = paint_brush_;
    ImGui::SetNextItemWidth(-110.0f);
    ImGui::DragFloat("Radio (m)", &b.radius, 0.1f, 0.2f, 200.0f, "%.1f");
    if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
    ImGui::SetItemTooltip("También Ctrl + rueda o [ y ].");
    ImGui::SetNextItemWidth(-110.0f);
    ImGui::DragFloat("Densidad", &b.density, 0.1f, 0.0f, 1000.0f, "%.1f / 100 m²");
    if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
    ImGui::SetItemTooltip("Objetos por cada 100 m². Repasar no amontona: rellena hasta esta densidad.");
    ImGui::SetNextItemWidth(-110.0f);
    ImGui::DragFloat("Separación (m)", &b.spacing, 0.05f, 0.0f, 100.0f, "%.2f");
    if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
    ImGui::SetItemTooltip("Distancia mínima entre dos objetos pintados.");
    ImGui::SetNextItemWidth(-110.0f);
    ImGui::SliderFloat("Pendiente máx.", &b.max_slope, 0.0f, 90.0f, "%.0f°");
    if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
    ImGui::SetItemTooltip("No pinta en suelo más inclinado (paredes, acantilados).");
    ImGui::Checkbox("Agrupar en la Jerarquía", &b.group_under_parent);
    ImGui::SetItemTooltip("Mete lo pintado bajo \"Pintado - <grupo>\".");

    // --- En la escena ---
    ImGui::SeparatorText("En la escena");
    const std::vector<ecs::Entity> painted = paintedInstances(false);
    ImGui::Text("%d objetos de este grupo", static_cast<int>(painted.size()));
    if (ImGui::Button("Seleccionarlos") && !painted.empty()) {
        selectOnly(painted.front().uuid());
        for (std::size_t i = 1; i < painted.size(); ++i) toggleSelection(painted[i].uuid());
    }
    ImGui::SameLine();
    if (ImGui::Button("Borrar todos") && !painted.empty()) ImGui::OpenPopup("paint_clear");
    if (ImGui::BeginPopup("paint_clear")) {
        ImGui::Text("¿Borrar los %d objetos de este grupo?", static_cast<int>(painted.size()));
        if (ImGui::Button("Borrar")) {
            clearSelection();
            for (const ecs::Entity e : painted) {
                if (e.valid()) world_.destroy(e);
            }
            dirty_ = true;
            commit();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancelar")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Clic/arrastrar: pintar · Mayús: borrar\nCtrl + rueda o [ ]: radio · Esc: salir");

    if (changed) savePaintGroup();
    ImGui::End();
}

}  // namespace cramion::editor

// Prefabs en el editor (como Unity): crear un prefab arrastrando un objeto
// al Proyecto (o con el menu de la Jerarquia), ponerlo en la escena
// arrastrando el .crprefab, y en el Inspector la barra de la instancia con
// Aplicar / Revertir / Desempaquetar. Los cambios propios de cada instancia
// se apuntan solos antes de cada paso de deshacer, y las instancias que se
// quedaron en una revision vieja se ponen al dia al abrir la escena o cuando
// el .crprefab cambia en disco.

#include "EditorApp.h"

#include "Dialogs.h"

#include <imgui.h>

#include <iostream>

namespace cramion::editor {

namespace {

constexpr ImU32 kPrefabBlue = IM_COL32(95, 170, 255, 255);

// Nombre de archivo valido en Windows a partir del nombre del objeto.
std::string fileSafe(std::string name) {
    for (char& c : name) {
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' ||
            c == '*' || static_cast<unsigned char>(c) < 32) {
            c = '_';
        }
    }
    while (!name.empty() && (name.back() == ' ' || name.back() == '.')) name.pop_back();
    return name.empty() ? std::string("Prefab") : name;
}

// La entidad de la instancia enlazada a `source` (para mostrar sus cambios).
ecs::Entity linkedEntity(ecs::World& world, ecs::Entity root, const std::string& source) {
    ecs::Entity found;
    std::vector<ecs::Entity> stack{root};
    while (!stack.empty() && !found.valid()) {
        const ecs::Entity e = stack.back();
        stack.pop_back();
        if (e != root && e.has<ecs::PrefabInstance>()) continue;
        if (const ecs::PrefabLink* link = e.tryGet<ecs::PrefabLink>(); link != nullptr && link->source == source) found = e;
        for (const entt::entity child : e.children()) stack.push_back(world.wrap(child));
    }
    return found;
}

// "entidad|Componente|campo" -> "Bombilla > Light.intensity" (para leerlo).
std::string describeOverride(ecs::World& world, ecs::Entity root, const std::string& key) {
    const std::size_t bar = key.find('|');
    if (bar == std::string::npos) return key;
    const ecs::Entity e = linkedEntity(world, root, key.substr(0, bar));
    const std::string who = e.valid() ? e.name() : std::string("(borrado)");
    std::string what = key.substr(bar + 1);
    if (what == "-") return who + ": borrado en la instancia";
    if (!what.empty() && what[0] == '#') return who + ": " + what.substr(1);
    if (!what.empty() && what[0] == '+') return who + ": componente anadido " + what.substr(1);
    if (!what.empty() && what[0] == '-') return who + ": componente quitado " + what.substr(1);
    if (const std::size_t field = what.find('|'); field != std::string::npos) what[field] = '.';
    return who + " > " + what;
}

}  // namespace

const std::string& EditorApp::prefabText(const Uuid& uuid) {
    static const std::string kEmpty;
    if (prefab_texts_version_ != database_version_) {
        prefab_texts_.clear();
        prefab_texts_version_ = database_version_;
    }
    const std::string key = uuid.toString();
    if (const auto it = prefab_texts_.find(key); it != prefab_texts_.end()) return it->second;
    const std::filesystem::path path = prefabPath(uuid);
    if (path.empty()) return kEmpty;
    return prefab_texts_.emplace(key, ecs::readPrefabFile(path)).first->second;
}

std::filesystem::path EditorApp::prefabPath(const Uuid& uuid) const {
    if (!database_ || !uuid.valid()) return {};
    const auto info = database_->find(uuid);
    return info && info->type == assets::AssetType::Prefab ? info->path : std::filesystem::path{};
}

void EditorApp::createPrefabsFromSelection(const std::filesystem::path& target_folder) {
    if (!has_project_) return;
    if (playing()) {
        std::cerr << "[Prefab] Sal del modo Play para crear prefabs\n";
        return;
    }
    const std::filesystem::path folder = target_folder.empty() ? project_.assetsFolder() / "Prefabs" : target_folder;
    int created = 0;
    for (ecs::Entity e : topLevelSelection()) {
        const std::string name = fileSafe(e.name());
        std::filesystem::path path = folder / dialogs::fromUtf8(name + ecs::kPrefabExtension);
        for (int i = 2; std::filesystem::exists(path); ++i) {
            path = folder / dialogs::fromUtf8(name + " " + std::to_string(i) + ecs::kPrefabExtension);
        }
        std::string error;
        if (ecs::createPrefab(world_, e, path, &error)) {
            ++created;
            std::cout << "[Prefab] Creado " << dialogs::utf8(path.filename()) << " (el objeto es ahora su instancia)\n";
        } else {
            std::cerr << "[Prefab] No se pudo crear " << dialogs::utf8(path.filename()) << ": " << error << "\n";
        }
    }
    if (created > 0) {
        refreshDatabase();
        commit();
    }
}

ecs::Entity EditorApp::instantiatePrefabAsset(const Uuid& uuid, ecs::Entity parent,
                                              const std::optional<core::Vec3>& world_position) {
    const std::string text = prefabText(uuid);
    if (text.empty()) {
        std::cerr << "[Prefab] No se pudo leer el prefab\n";
        return {};
    }
    ecs::Entity root = ecs::instantiatePrefab(world_, text, parent);
    if (!root.valid()) {
        std::cerr << "[Prefab] El prefab esta vacio o danado\n";
        return {};
    }
    if (world_position) {
        root.setWorldPosition(*world_position);
    } else if (!parent.valid()) {
        // Sin sitio: delante de la camara del editor, como Unity.
        const scene::Camera& camera = scene_.camera();
        root.setWorldPosition(camera.position() + camera.forward() * 6.0f);
    }
    selectOnly(root.uuid());
    revealInHierarchy(root.uuid());
    commit();
    return root;
}

void EditorApp::applyPrefab(ecs::Entity root) {
    if (!root.valid() || !root.has<ecs::PrefabInstance>()) return;
    if (playing()) {
        std::cerr << "[Prefab] Sal del modo Play para aplicar cambios al prefab\n";
        return;
    }
    const Uuid uuid = root.get<ecs::PrefabInstance>().prefab.uuid;
    const std::filesystem::path path = prefabPath(uuid);
    if (path.empty()) {
        std::cerr << "[Prefab] El asset del prefab ya no existe (desempaqueta la instancia o crea uno nuevo)\n";
        return;
    }
    std::string error;
    const std::string text = ecs::applyInstance(world_, root, path, &error);
    if (text.empty()) {
        std::cerr << "[Prefab] No se pudo guardar " << dialogs::utf8(path.filename()) << ": " << error << "\n";
        return;
    }
    prefab_texts_[uuid.toString()] = text;
    // Las demas instancias de la escena reciben la version nueva ya.
    const int updated = ecs::syncOutdatedInstances(world_, [&](const Uuid& id) { return prefabText(id); });
    std::cout << "[Prefab] " << dialogs::utf8(path.filename()) << " aplicado (revision " << ecs::prefabRevision(text)
              << "), " << updated << " instancia(s) actualizada(s)\n";
    commit();
}

void EditorApp::revertPrefab(ecs::Entity root) {
    if (!root.valid() || !root.has<ecs::PrefabInstance>()) return;
    const std::string text = prefabText(root.get<ecs::PrefabInstance>().prefab.uuid);
    if (text.empty()) {
        std::cerr << "[Prefab] El asset del prefab ya no existe\n";
        return;
    }
    ecs::revertInstance(world_, root, text);
    commit();
}

void EditorApp::unpackPrefab(ecs::Entity root) {
    if (!root.valid() || !root.has<ecs::PrefabInstance>()) return;
    ecs::unpackInstance(world_, root);
    commit();
}

void EditorApp::selectPrefabInstances(const Uuid& prefab) {
    selection_.clear();
    for (const ecs::Entity e : ecs::prefabInstances(world_, prefab)) {
        selection_.push_back(e.uuid());
        active_ = e.uuid();
    }
    if (active_.valid()) revealInHierarchy(active_);
}

void EditorApp::syncPrefabInstances() {
    if (!has_project_ || !database_ || playing()) return;
    const int updated = ecs::syncOutdatedInstances(world_, [&](const Uuid& id) { return prefabText(id); });
    if (updated > 0) {
        std::cout << "[Prefab] " << updated << " instancia(s) puesta(s) al dia con su prefab\n";
        commit();
    }
}

void EditorApp::recordPrefabOverrides() {
    if (!database_) return;
    for (const entt::entity handle : world_.registry().view<ecs::PrefabInstance>()) {
        const ecs::Entity root = world_.wrap(handle);
        const ecs::PrefabInstance& instance = root.get<ecs::PrefabInstance>();
        const std::string& text = prefabText(instance.prefab.uuid);
        // Solo con la misma revision: si no, todo lo nuevo pareceria propio.
        if (text.empty() || ecs::prefabRevision(text) != instance.revision) continue;
        ecs::recordOverrides(world_, root, text);
    }
}

// Barra de la instancia arriba del Inspector (azul, como Unity).
void EditorApp::drawPrefabInspectorBar(ecs::Entity entity) {
    const ecs::Entity root = ecs::prefabRoot(entity);
    if (!root.valid()) return;
    const ecs::PrefabInstance& instance = root.get<ecs::PrefabInstance>();
    const Uuid prefab = instance.prefab.uuid;
    const std::filesystem::path path = prefabPath(prefab);
    const bool missing = path.empty();
    const std::string name = missing ? std::string("(prefab perdido)") : dialogs::utf8(path.stem());

    ImGui::PushID("prefab_bar");
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->ChannelsSplit(2);
    draw->ChannelsSetCurrent(1);
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    ImGui::Indent(6.0f);
    const ImU32 accent = missing ? IM_COL32(235, 90, 90, 255) : kPrefabBlue;
    imgui_.drawIcon(draw, Icon::ColliderBox, ImGui::GetCursorScreenPos(), ImGui::GetTextLineHeight(), accent);
    ImGui::Dummy(ImVec2(ImGui::GetTextLineHeight(), ImGui::GetTextLineHeight()));
    ImGui::SameLine();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(accent), "Prefab");
    ImGui::SameLine();
    ImGui::TextUnformatted(name.c_str());
    if (root != entity) {
        ImGui::SameLine();
        ImGui::TextDisabled("(parte de %s)", root.name().c_str());
    }
    if (!missing && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("%s\nRevision %d", dialogs::utf8(path).c_str(), instance.revision);
    }

    const std::size_t overrides = instance.overrides.size();
    if (ImGui::SmallButton("Seleccionar")) {
        if (!missing) current_folder_ = path.parent_path();
        selectOnly(root.uuid());
    }
    ImGui::SetItemTooltip("Selecciona la raiz de la instancia y muestra el asset en el Proyecto");
    ImGui::SameLine();
    ImGui::BeginDisabled(missing || playing());
    if (ImGui::SmallButton("Revertir")) revertPrefab(root);
    ImGui::SetItemTooltip("Olvida los cambios propios: queda igual que el prefab");
    ImGui::SameLine();
    if (ImGui::SmallButton("Aplicar")) applyPrefab(root);
    ImGui::SetItemTooltip("Guarda esta instancia como la nueva version del prefab\ny actualiza las demas instancias");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::SmallButton("...")) ImGui::OpenPopup("prefab_more");
    if (ImGui::BeginPopup("prefab_more")) {
        if (ImGui::MenuItem("Seleccionar todas las instancias", nullptr, false, prefab.valid())) selectPrefabInstances(prefab);
        if (ImGui::MenuItem("Desempaquetar")) unpackPrefab(root);
        ImGui::SetItemTooltip("Quita el enlace: quedan objetos normales");
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (overrides == 0) {
        ImGui::TextDisabled("sin cambios propios");
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.35f, 1.0f), "%zu cambio(s) propio(s)", overrides);
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextDisabled("Lo que esta instancia tiene distinto del prefab\n(se respeta al actualizarla):");
            std::size_t shown = 0;
            for (const std::string& key : instance.overrides) {
                if (++shown > 24) {
                    ImGui::TextDisabled("... y %zu mas", overrides - 24);
                    break;
                }
                ImGui::BulletText("%s", describeOverride(world_, root, key).c_str());
            }
            ImGui::EndTooltip();
        }
    }
    ImGui::Unindent(6.0f);
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    draw->ChannelsSetCurrent(0);
    const ImVec2 end(start.x + width, ImGui::GetCursorScreenPos().y);
    draw->AddRectFilled(start, end, (accent & 0x00FFFFFFu) | 0x22000000u, 4.0f);
    draw->AddRect(start, end, (accent & 0x00FFFFFFu) | 0x80000000u, 4.0f);
    draw->ChannelsMerge();
    ImGui::PopID();
}

// Submenu "Prefab" del menu contextual de la Jerarquia.
void EditorApp::drawPrefabHierarchyMenu(ecs::Entity entity) {
    if (!ImGui::BeginMenu("Prefab")) return;
    const ecs::Entity root = ecs::prefabRoot(entity);
    const bool linked = root.valid();
    const bool missing = linked && prefabPath(root.get<ecs::PrefabInstance>().prefab.uuid).empty();
    if (ImGui::MenuItem("Crear prefab", nullptr, false, has_project_ && !playing())) createPrefabsFromSelection();
    ImGui::SetItemTooltip("Guarda el objeto (con sus hijos) en Assets/Prefabs.\nTambien: arrastrarlo al panel Proyecto.");
    ImGui::Separator();
    if (ImGui::MenuItem("Aplicar al prefab", nullptr, false, linked && !missing && !playing())) applyPrefab(root);
    if (ImGui::MenuItem("Revertir", nullptr, false, linked && !missing && !playing())) revertPrefab(root);
    if (ImGui::MenuItem("Desempaquetar", nullptr, false, linked)) unpackPrefab(root);
    if (ImGui::MenuItem("Seleccionar la raiz", nullptr, false, linked && root != entity)) selectOnly(root.uuid());
    if (ImGui::MenuItem("Mostrar el asset", nullptr, false, linked && !missing)) {
        current_folder_ = prefabPath(root.get<ecs::PrefabInstance>().prefab.uuid).parent_path();
        show_project_ = true;
    }
    ImGui::EndMenu();
}

}  // namespace cramion::editor

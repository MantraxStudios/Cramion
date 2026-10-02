// 2D en el editor (como el 2D de Unity):
//
//   Vista 2D        boton "2D" de la barra de la Escena: camara ortografica
//                   mirando el plano XY; rueda = zoom, boton central o derecho
//                   = desplazar. Los sprites y tilemaps se eligen con un clic.
//   Paleta de tiles pinta en el Tilemap seleccionado: pincel, rectangulo,
//                   relleno, borrar y cuentagotas, por capas; Mayus = borrar.
//   Tileset         (.crtileset) imagen cortada en celdas, celdas sin colision
//                   y Rule Tiles (autotiling con los 8 vecinos).
//   Sprite Editor   ajustes de una imagen como hoja de sprites: Single o
//                   Multiple, cortar por rejilla, por filas/columnas o
//                   automatico, pivote, pixeles por unidad y filtro.
//   Capas de orden  (Sorting Layers) del proyecto.

#include "EditorApp.h"

#include "Dialogs.h"

#include <CramionFX/asset/ImageFile.h>

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <iostream>

namespace cramion::editor {

using core::Vec3;

struct TwoDEditorState {
    // Vista 2D
    bool view_2d = false;
    Vec3 saved_position{};
    Vec3 saved_forward{0.0f, 0.0f, -1.0f};
    Vec3 saved_up{0.0f, 1.0f, 0.0f};
    float ortho_size = 6.0f;

    // Paleta
    bool show_palette = false;
    enum class Tool { Brush = 0, Rect, Fill, Erase, Picker } tool = Tool::Brush;
    std::int32_t tile = 1;  // 1..n celda (frame + 1), -k Rule Tile k-1
    int layer = 0;
    bool stroke = false;
    bool rect_dragging = false;
    int rect_x = 0, rect_y = 0;
    float palette_zoom = 2.0f;

    // Tileset abierto
    bool show_tileset = false;
    std::filesystem::path tileset_path;
    twod::Tileset tileset;
    bool tileset_dirty = false;
    int rule_selected = -1;
    int terrain_first = 0;

    // Sprite Editor
    bool show_sprite = false;
    std::string sprite_image;  // relativa a Assets
    twod::SpriteSheet sheet;
    bool sheet_loaded = false;
    bool sheet_dirty = false;
    int slice_mode = 0;  // 0 rejilla, 1 filas/columnas, 2 automatico
    int cell_w = 32, cell_h = 32, offset_x = 0, offset_y = 0, spacing_x = 0, spacing_y = 0;
    int columns = 4, rows = 4;
    int frame_selected = -1;
    float sprite_zoom = 2.0f;

    // Capas de orden
    bool show_layers = false;
    std::string new_layer;
};

namespace {

bool smallToggle(const char* label, bool active, const char* tooltip) {
    if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Header));
    const bool pressed = ImGui::Button(label);
    if (active) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tooltip);
    return pressed;
}

// Rectangulo de la celda de un tileset en la imagen (pixeles).
void tileRect(const twod::Tileset& t, int frame, int& x, int& y) {
    const int cols = std::max(t.columns(), 1);
    x = t.margin + (frame % cols) * (t.tile_width + t.spacing);
    y = t.margin + (frame / cols) * (t.tile_height + t.spacing);
}

}  // namespace

TwoDEditorState& EditorApp::twodEditor() {
    if (!twod_editor_) twod_editor_ = std::make_shared<TwoDEditorState>();
    return *twod_editor_;
}

// --- Vista 2D -----------------------------------------------------------------------------

void EditorApp::draw2DViewToggle() {
    TwoDEditorState& st = twodEditor();
    ImGui::SameLine();
    if (smallToggle("2D", st.view_2d, "Vista 2D: cámara ortográfica en el plano XY (rueda = zoom, botón central/derecho = desplazar)")) {
        scene::Camera& camera = scene_.camera();
        st.view_2d = !st.view_2d;
        if (st.view_2d) {
            st.saved_position = camera.position();
            st.saved_forward = camera.forward();
            st.saved_up = camera.up();
            camera.setPosition(Vec3{camera.position().x, camera.position().y, std::max(camera.position().z, 10.0f)});
            camera.setOrientation(Vec3{0.0f, 0.0f, -1.0f}, Vec3{0.0f, 1.0f, 0.0f});
            camera.setOrthographic(true, st.ortho_size);
        } else {
            camera.setOrthographic(false);
            camera.setPosition(st.saved_position);
            camera.setOrientation(st.saved_forward, st.saved_up);
        }
    }
    if (st.view_2d || st.show_palette) {
        ImGui::SameLine();
        if (smallToggle("Paleta", st.show_palette, "Paleta de tiles: pintar en el Tilemap seleccionado")) st.show_palette = !st.show_palette;
    }
}

bool EditorApp::handle2DCamera() {
    TwoDEditorState& st = twodEditor();
    scene::Camera& camera = scene_.camera();
    if (!st.view_2d) return false;
    ImGuiIO& io = ImGui::GetIO();
    flying_ = false;
    camera.setOrientation(Vec3{0.0f, 0.0f, -1.0f}, Vec3{0.0f, 1.0f, 0.0f});
    camera.setOrthographic(true, st.ortho_size);
    if (view_hovered_ && io.MouseWheel != 0.0f && !io.KeyCtrl) {
        // Zoom hacia el raton.
        const float u = (io.MousePos.x - view_x_) / view_w_ * 2.0f - 1.0f;
        const float v = 1.0f - (io.MousePos.y - view_y_) / view_h_ * 2.0f;
        const float aspect = view_w_ / std::max(view_h_, 1.0f);
        const float before_x = u * st.ortho_size * aspect;
        const float before_y = v * st.ortho_size;
        st.ortho_size = std::clamp(st.ortho_size * (io.MouseWheel > 0.0f ? 1.0f / 1.15f : 1.15f), 0.2f, 5000.0f);
        const float after_x = u * st.ortho_size * aspect;
        const float after_y = v * st.ortho_size;
        camera.setPosition(camera.position() + Vec3{before_x - after_x, before_y - after_y, 0.0f});
        camera.setOrthographic(true, st.ortho_size);
    }
    if (view_hovered_ && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f) || ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f))) {
        const float k = st.ortho_size * 2.0f / std::max(view_h_, 1.0f);
        camera.setPosition(camera.position() + Vec3{-io.MouseDelta.x * k, io.MouseDelta.y * k, 0.0f});
    }
    return true;
}

bool EditorApp::pick2DAt(float x, float y) {
    Vec3 origin, direction;
    if (!mouseRay(x, y, origin, direction)) return false;
    const ecs::Entity e = twod_.pick(world_, origin, direction);
    if (!e.valid()) return false;
    if (ImGui::GetIO().KeyCtrl) toggleSelection(e.uuid());
    else selectOnly(e.uuid());
    revealInHierarchy(e.uuid());
    return true;
}

// --- Pintar tiles ---------------------------------------------------------------------------

bool EditorApp::draw2DTileTool() {
    TwoDEditorState& st = twodEditor();
    if (!st.show_palette || playing() || flying_) {
        st.stroke = false;
        st.rect_dragging = false;
        return false;
    }
    ecs::Entity target = world_.find(active_);
    twod::Tilemap* map = target.valid() ? target.tryGet<twod::Tilemap>() : nullptr;
    if (map == nullptr) return false;
    ImGuiIO& io = ImGui::GetIO();
    if (!view_hovered_ && !st.stroke && !st.rect_dragging) return false;
    Vec3 origin, direction;
    if (!mouseRay(io.MousePos.x, io.MousePos.y, origin, direction)) return true;
    // Al plano del tilemap.
    const core::Mat4 inv = core::inverse(target.worldMatrix());
    const Vec3 o = ecs::transformPoint(inv, origin);
    const Vec3 d = ecs::transformDirection(inv, direction);
    if (std::fabs(d.z) < 1e-8f) return true;
    const float t = -o.z / d.z;
    if (t < 0.0f) return true;
    const Vec3 local = o + d * t;
    int cx = 0, cy = 0;
    map->cellAt(local, cx, cy);
    const int layer = std::clamp(st.layer, 0, std::max(static_cast<int>(map->layers.size()) - 1, 0));
    if (map->layers.empty()) map->layers.push_back(twod::TilemapLayer{});

    // Contorno de la celda (o del rectangulo).
    int x0 = cx, y0 = cy, x1 = cx, y1 = cy;
    if (st.rect_dragging) {
        x0 = std::min(st.rect_x, cx);
        y0 = std::min(st.rect_y, cy);
        x1 = std::max(st.rect_x, cx);
        y1 = std::max(st.rect_y, cy);
    }
    const core::Mat4& m = target.worldMatrix();
    const Vec3 a = ecs::transformPoint(m, Vec3{x0 * map->cell_size.x, y0 * map->cell_size.y, 0.0f});
    const Vec3 b = ecs::transformPoint(m, Vec3{(x1 + 1) * map->cell_size.x, y0 * map->cell_size.y, 0.0f});
    const Vec3 c = ecs::transformPoint(m, Vec3{(x1 + 1) * map->cell_size.x, (y1 + 1) * map->cell_size.y, 0.0f});
    const Vec3 e = ecs::transformPoint(m, Vec3{x0 * map->cell_size.x, (y1 + 1) * map->cell_size.y, 0.0f});
    const bool erase = io.KeyShift || st.tool == TwoDEditorState::Tool::Erase;
    const std::uint32_t color = erase ? 0xFF5050F0u : 0xFF40D0FFu;
    overlayLine(a, b, color);
    overlayLine(b, c, color);
    overlayLine(c, e, color);
    overlayLine(e, a, color);
    ImGui::SetTooltip("(%d, %d)  %s", cx, cy,
                      st.tool == TwoDEditorState::Tool::Picker ? "Cuentagotas"
                      : erase                                  ? "Borrar"
                      : st.tool == TwoDEditorState::Tool::Fill ? "Rellenar"
                      : st.tool == TwoDEditorState::Tool::Rect ? "Rectángulo"
                                                               : "Pincel");
    const std::int32_t id = erase ? 0 : st.tile;
    const bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left) && view_hovered_ && !io.KeyAlt;
    switch (st.tool) {
        case TwoDEditorState::Tool::Picker:
            if (clicked) {
                const std::int32_t picked = map->getTile(cx, cy, layer);
                if (picked != 0) st.tile = picked;
                st.tool = TwoDEditorState::Tool::Brush;
            }
            break;
        case TwoDEditorState::Tool::Fill:
            if (clicked) {
                int bx0 = cx - 64, by0 = cy - 64, bx1 = cx + 64, by1 = cy + 64;
                int mx0, my0, mx1, my1;
                if (map->layers[static_cast<std::size_t>(layer)].data.bounds(mx0, my0, mx1, my1)) {
                    bx0 = std::min(bx0, mx0 - 1);
                    by0 = std::min(by0, my0 - 1);
                    bx1 = std::max(bx1, mx1 + 1);
                    by1 = std::max(by1, my1 + 1);
                }
                twod::floodFill(*map, layer, cx, cy, id, bx0, by0, bx1, by1);
                ++map->version;
                commit();
            }
            break;
        case TwoDEditorState::Tool::Rect:
            if (clicked) {
                st.rect_dragging = true;
                st.rect_x = cx;
                st.rect_y = cy;
            } else if (st.rect_dragging && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                st.rect_dragging = false;
                for (int y = y0; y <= y1; ++y) {
                    for (int x = x0; x <= x1; ++x) map->setTile(x, y, id, layer);
                }
                ++map->version;
                commit();
            }
            break;
        default:
            if (clicked) st.stroke = true;
            if (st.stroke) {
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    if (map->setTile(cx, cy, id, layer)) ++map->version;
                } else {
                    st.stroke = false;
                    commit();
                }
            }
            break;
    }
    return true;
}

// --- Ventanas -------------------------------------------------------------------------------

void EditorApp::openTilesetEditor(const std::filesystem::path& file) {
    TwoDEditorState& st = twodEditor();
    twod::Tileset t;
    if (!twod::loadTileset(file, t)) {
        std::cerr << "[Editor] No se pudo abrir el tileset " << dialogs::utf8(file) << "\n";
        return;
    }
    st.tileset = std::move(t);
    st.tileset_path = file;
    st.tileset_dirty = false;
    st.rule_selected = -1;
    st.show_tileset = true;
}

void EditorApp::draw2DCreateMenu(const std::filesystem::path& folder) {
    if (ImGui::BeginMenu("2D")) {
        if (ImGui::MenuItem("Tileset")) {
            std::error_code ec;
            std::filesystem::create_directories(folder, ec);
            std::filesystem::path path = folder / dialogs::fromUtf8(std::string("Tileset") + twod::kTilesetExtension);
            for (int i = 2; std::filesystem::exists(path); ++i) {
                path = folder / dialogs::fromUtf8("Tileset " + std::to_string(i) + twod::kTilesetExtension);
            }
            twod::Tileset t;
            std::string error;
            if (twod::saveTileset(path, t, &error)) {
                refreshDatabase();
                openTilesetEditor(path);
            } else {
                std::cerr << "[Editor] " << error << "\n";
            }
        }
        ImGui::SetItemTooltip("Una imagen cortada en celdas para pintar Tilemaps (y sus Rule Tiles)");
        if (ImGui::MenuItem("Sprite Editor")) twodEditor().show_sprite = true;
        ImGui::EndMenu();
    }
}

void EditorApp::draw2DWindowMenu() {
    TwoDEditorState& st = twodEditor();
    if (ImGui::BeginMenu("2D")) {
        ImGui::MenuItem("Paleta de tiles", nullptr, &st.show_palette);
        ImGui::MenuItem("Sprite Editor", nullptr, &st.show_sprite);
        ImGui::MenuItem("Capas de orden (Sorting Layers)", nullptr, &st.show_layers);
        ImGui::EndMenu();
    }
}

void EditorApp::draw2DWindows() {
    TwoDEditorState& st = twodEditor();
    // La camara del editor no se queda ortografica al salir de Play.
    if (!playing() && !st.view_2d && scene_.camera().orthographic()) scene_.camera().setOrthographic(false);

    // --- Paleta de tiles ---
    if (st.show_palette) {
        ImGui::SetNextWindowSize(ImVec2(380.0f, 520.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Paleta de tiles###tile_palette", &st.show_palette)) {
            ecs::Entity target = world_.find(active_);
            twod::Tilemap* map = target.valid() ? target.tryGet<twod::Tilemap>() : nullptr;
            if (map == nullptr) {
                ImGui::TextWrapped("Selecciona un objeto con Tilemap para pintar.");
                if (ImGui::Button("Crear Tilemap")) {
                    ecs::Entity e = world_.create("Tilemap");
                    twod::Tilemap& tm = e.add<twod::Tilemap>();
                    tm.layers = {twod::TilemapLayer{}};
                    tm.layers[0].name = "Suelo";
                    e.add<twod::TilemapCollider2D>();
                    selectOnly(e.uuid());
                    commit();
                }
            } else {
                // Tileset del tilemap
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::BeginCombo("##tileset", map->tileset.empty() ? "(elige un tileset)" : map->tileset.c_str())) {
                    std::error_code e;
                    for (auto it = std::filesystem::recursive_directory_iterator(project_.assetsFolder(), e);
                         !e && it != std::filesystem::recursive_directory_iterator(); it.increment(e)) {
                        if (it->path().extension() != twod::kTilesetExtension) continue;
                        const std::string rel = assetRelative(it->path());
                        if (ImGui::Selectable(rel.c_str(), rel == map->tileset)) {
                            map->tileset = rel;
                            map->markAllDirty();
                            ++map->version;
                            commit();
                        }
                    }
                    ImGui::EndCombo();
                }
                // Herramientas
                const auto tool = [&](const char* label, TwoDEditorState::Tool t, const char* tip) {
                    if (smallToggle(label, st.tool == t, tip)) st.tool = t;
                    ImGui::SameLine();
                };
                tool("Pincel", TwoDEditorState::Tool::Brush, "Pintar celdas (arrastrar)");
                tool("Rect", TwoDEditorState::Tool::Rect, "Rellenar un rectángulo");
                tool("Relleno", TwoDEditorState::Tool::Fill, "Rellenar la zona igual");
                tool("Borrar", TwoDEditorState::Tool::Erase, "Borrar celdas (o Mayús con cualquier herramienta)");
                tool("Gotero", TwoDEditorState::Tool::Picker, "Coger el tile de una celda");
                ImGui::NewLine();
                // Capas
                ImGui::SetNextItemWidth(160.0f);
                st.layer = std::clamp(st.layer, 0, std::max(static_cast<int>(map->layers.size()) - 1, 0));
                if (ImGui::BeginCombo("Capa", map->layers.empty() ? "-" : map->layers[static_cast<std::size_t>(st.layer)].name.c_str())) {
                    for (std::size_t i = 0; i < map->layers.size(); ++i) {
                        if (ImGui::Selectable((map->layers[i].name + "##" + std::to_string(i)).c_str(), st.layer == static_cast<int>(i))) {
                            st.layer = static_cast<int>(i);
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("+ Capa")) {
                    twod::TilemapLayer layer;
                    layer.name = "Capa " + std::to_string(map->layers.size() + 1);
                    layer.order = static_cast<int>(map->layers.size());
                    map->layers.push_back(std::move(layer));
                    st.layer = static_cast<int>(map->layers.size()) - 1;
                    map->markAllDirty();
                    commit();
                }
                const twod::Tileset* tileset = map->tileset.empty() ? nullptr : twod_.tilesets().get(map->tileset);
                if (tileset == nullptr || tileset->image.empty()) {
                    ImGui::TextDisabled("El tileset no tiene imagen (ábrelo con doble clic).");
                } else {
                    // Rule Tiles
                    for (std::size_t r = 0; r < tileset->rule_tiles.size(); ++r) {
                        const std::int32_t id = -static_cast<std::int32_t>(r) - 1;
                        if (smallToggle((tileset->rule_tiles[r].name + "##rule" + std::to_string(r)).c_str(), st.tile == id,
                                        "Rule Tile: elige su dibujo según los vecinos")) {
                            st.tile = id;
                            if (st.tool == TwoDEditorState::Tool::Erase || st.tool == TwoDEditorState::Tool::Picker) st.tool = TwoDEditorState::Tool::Brush;
                        }
                        ImGui::SameLine();
                    }
                    ImGui::NewLine();
                    ImGui::SetNextItemWidth(120.0f);
                    ImGui::SliderFloat("Zoom", &st.palette_zoom, 0.5f, 6.0f, "%.1fx");
                    ImVec2 size{};
                    const ImTextureID texture = imgui_.image(twod_.sprites().absolute(tileset->image), &size);
                    ImGui::BeginChild("palette_image", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
                    if (texture != 0 && size.x > 0.0f) {
                        const ImVec2 p0 = ImGui::GetCursorScreenPos();
                        const float z = st.palette_zoom;
                        ImGui::Image(texture, ImVec2(size.x * z, size.y * z));
                        ImDrawList* draw = ImGui::GetWindowDrawList();
                        const int count = tileset->tileCount();
                        for (int f = 0; f < count; ++f) {
                            int px = 0, py = 0;
                            tileRect(*tileset, f, px, py);
                            const ImVec2 a(p0.x + px * z, p0.y + py * z);
                            const ImVec2 b(a.x + tileset->tile_width * z, a.y + tileset->tile_height * z);
                            const bool chosen = st.tile == f + 1;
                            draw->AddRect(a, b, chosen ? IM_COL32(255, 210, 60, 255) : IM_COL32(255, 255, 255, 40), 0.0f, 0, chosen ? 2.5f : 1.0f);
                            if (ImGui::IsMouseHoveringRect(a, b) && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsWindowHovered()) {
                                st.tile = f + 1;
                                if (st.tool == TwoDEditorState::Tool::Erase || st.tool == TwoDEditorState::Tool::Picker) st.tool = TwoDEditorState::Tool::Brush;
                            }
                        }
                    }
                    ImGui::EndChild();
                }
            }
        }
        ImGui::End();
    }

    // --- Tileset ---
    if (st.show_tileset) {
        ImGui::SetNextWindowSize(ImVec2(820.0f, 600.0f), ImGuiCond_FirstUseEver);
        const std::string title = "Tileset: " + dialogs::utf8(st.tileset_path.stem()) + (st.tileset_dirty ? " *" : "") + "###tileset_editor";
        if (ImGui::Begin(title.c_str(), &st.show_tileset)) {
            twod::Tileset& t = st.tileset;
            bool changed = false;
            ImGui::BeginChild("ts_left", ImVec2(330.0f, 0.0f), ImGuiChildFlags_ResizeX | ImGuiChildFlags_Borders);
            changed |= ImGui::InputTextWithHint("Imagen", "arrastra una imagen", &t.image);
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kImagePayload)) {
                    t.image = assetRelative(dialogs::fromUtf8(static_cast<const char*>(payload->Data)));
                    changed = true;
                }
                ImGui::EndDragDropTarget();
            }
            changed |= ImGui::DragInt("Ancho de celda", &t.tile_width, 0.2f, 1, 4096);
            changed |= ImGui::DragInt("Alto de celda", &t.tile_height, 0.2f, 1, 4096);
            changed |= ImGui::DragInt("Margen", &t.margin, 0.1f, 0, 512);
            changed |= ImGui::DragInt("Separación", &t.spacing, 0.1f, 0, 512);
            int filter = static_cast<int>(t.filter);
            static const char* kFilters[] = {"Point (pixel art)", "Bilinear"};
            if (ImGui::Combo("Filtro", &filter, kFilters, 2)) {
                t.filter = static_cast<twod::SpriteFilter>(filter);
                changed = true;
            }
            ImGui::TextDisabled("%d x %d celdas", t.columns(), t.rows());
            ImGui::SeparatorText("Rule Tiles");
            ImGui::TextWrapped("Eligen su dibujo según sus 8 vecinos (bordes y esquinas solos).");
            for (std::size_t r = 0; r < t.rule_tiles.size(); ++r) {
                if (ImGui::Selectable((t.rule_tiles[r].name + "##" + std::to_string(r)).c_str(), st.rule_selected == static_cast<int>(r))) {
                    st.rule_selected = static_cast<int>(r);
                }
            }
            ImGui::SetNextItemWidth(80.0f);
            ImGui::DragInt("##first", &st.terrain_first, 0.2f, 0, std::max(t.tileCount() - 1, 0));
            ImGui::SameLine();
            if (ImGui::Button("+ Terreno 3x3")) {
                t.rule_tiles.push_back(twod::makeTerrainRuleTile("Terreno " + std::to_string(t.rule_tiles.size() + 1), st.terrain_first,
                                                                 std::max(t.columns(), 1)));
                st.rule_selected = static_cast<int>(t.rule_tiles.size()) - 1;
                changed = true;
            }
            ImGui::SetItemTooltip("Desde la celda indicada: 3x3 celdas (esquinas, bordes y centro) de un terreno");
            ImGui::SameLine();
            if (ImGui::Button("+ Vacío")) {
                t.rule_tiles.push_back(twod::RuleTile{});
                st.rule_selected = static_cast<int>(t.rule_tiles.size()) - 1;
                changed = true;
            }
            if (st.rule_selected >= 0 && st.rule_selected < static_cast<int>(t.rule_tiles.size())) {
                twod::RuleTile& rt = t.rule_tiles[static_cast<std::size_t>(st.rule_selected)];
                ImGui::SeparatorText(rt.name.c_str());
                changed |= ImGui::InputText("Nombre", &rt.name);
                changed |= ImGui::DragInt("Por defecto", &rt.default_frame, 0.2f, 0, std::max(t.tileCount() - 1, 0));
                changed |= ImGui::Checkbox("Con colisión", &rt.collider);
                int remove_rule = -1;
                static const char* kNeighbor[] = {"·", "✔", "✖"};
                for (std::size_t i = 0; i < rt.rules.size(); ++i) {
                    twod::TileRule& rule = rt.rules[i];
                    ImGui::PushID(static_cast<int>(i));
                    ImGui::BeginGroup();
                    for (int k = 0; k < 9; ++k) {
                        if (k % 3 != 0) ImGui::SameLine(0.0f, 2.0f);
                        if (k == 4) {
                            ImGui::Button("■", ImVec2(22.0f, 22.0f));
                            continue;
                        }
                        const int n = k < 4 ? k : k - 1;
                        const int value = static_cast<int>(rule.neighbors[static_cast<std::size_t>(n)]);
                        ImGui::PushID(k);
                        if (ImGui::Button(kNeighbor[value], ImVec2(22.0f, 22.0f))) {
                            rule.neighbors[static_cast<std::size_t>(n)] = static_cast<twod::TileNeighbor>((value + 1) % 3);
                            changed = true;
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndGroup();
                    ImGui::SameLine();
                    ImGui::BeginGroup();
                    std::string frames;
                    for (const int f : rule.frames) frames += (frames.empty() ? "" : ",") + std::to_string(f);
                    ImGui::SetNextItemWidth(110.0f);
                    if (ImGui::InputText("Celdas", &frames)) {
                        rule.frames = twod::parseFrameList(frames);
                        changed = true;
                    }
                    if (ImGui::SmallButton("Quitar regla")) remove_rule = static_cast<int>(i);
                    ImGui::EndGroup();
                    ImGui::PopID();
                }
                if (remove_rule >= 0) {
                    rt.rules.erase(rt.rules.begin() + remove_rule);
                    changed = true;
                }
                if (ImGui::Button("+ Regla")) {
                    rt.rules.push_back(twod::TileRule{});
                    changed = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Borrar Rule Tile")) {
                    t.rule_tiles.erase(t.rule_tiles.begin() + st.rule_selected);
                    st.rule_selected = -1;
                    changed = true;
                }
                ImGui::TextDisabled("✔ es este tile · ✖ no lo es · · da igual");
            }
            ImGui::EndChild();
            ImGui::SameLine();
            ImGui::BeginChild("ts_image", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::TextDisabled("Clic en una celda: con o sin colisión (rojo = sin colisión)");
            if (!t.image.empty()) {
                ImVec2 size{};
                const ImTextureID texture = imgui_.image(twod_.sprites().absolute(t.image), &size);
                if (texture != 0 && size.x > 0.0f) {
                    t.image_width = static_cast<int>(size.x);
                    t.image_height = static_cast<int>(size.y);
                    const float z = 2.0f;
                    const ImVec2 p0 = ImGui::GetCursorScreenPos();
                    ImGui::Image(texture, ImVec2(size.x * z, size.y * z));
                    ImDrawList* draw = ImGui::GetWindowDrawList();
                    for (int f = 0; f < t.tileCount(); ++f) {
                        int px = 0, py = 0;
                        tileRect(t, f, px, py);
                        const ImVec2 a(p0.x + px * z, p0.y + py * z);
                        const ImVec2 b(a.x + t.tile_width * z, a.y + t.tile_height * z);
                        const bool solid = t.frameSolid(f);
                        if (!solid) draw->AddRectFilled(a, b, IM_COL32(220, 60, 50, 70));
                        draw->AddRect(a, b, IM_COL32(255, 255, 255, 50));
                        char label[8];
                        std::snprintf(label, sizeof(label), "%d", f);
                        draw->AddText(ImVec2(a.x + 2.0f, a.y + 1.0f), IM_COL32(255, 255, 255, 110), label);
                        if (ImGui::IsMouseHoveringRect(a, b) && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsWindowHovered()) {
                            auto it = std::find(t.no_collider.begin(), t.no_collider.end(), f);
                            if (it != t.no_collider.end()) t.no_collider.erase(it);
                            else t.no_collider.push_back(f);
                            changed = true;
                        }
                    }
                }
            }
            ImGui::EndChild();
            if (changed) st.tileset_dirty = true;
            if (st.tileset_dirty && !ImGui::IsAnyItemActive() && ImGui::GetIO().MouseDownDuration[0] < 0.0f) {
                std::string error;
                if (twod::saveTileset(st.tileset_path, t, &error)) {
                    st.tileset_dirty = false;
                    twod_.tilesets().invalidate(assetRelative(st.tileset_path));
                    for (const entt::entity h : world_.registry().view<twod::Tilemap>()) {
                        twod::Tilemap& m = world_.registry().get<twod::Tilemap>(h);
                        m.markAllDirty();
                        ++m.version;
                    }
                } else {
                    std::cerr << "[Editor] " << error << "\n";
                }
            }
        }
        ImGui::End();
    }

    // --- Sprite Editor ---
    if (st.show_sprite) {
        ImGui::SetNextWindowSize(ImVec2(900.0f, 600.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Sprite Editor###sprite_editor", &st.show_sprite)) {
            ImGui::SetNextItemWidth(320.0f);
            ImGui::InputTextWithHint("Imagen", "arrastra una imagen del Proyecto", &st.sprite_image, ImGuiInputTextFlags_ReadOnly);
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kImagePayload)) {
                    st.sprite_image = assetRelative(dialogs::fromUtf8(static_cast<const char*>(payload->Data)));
                    st.sheet = twod::SpriteSheet{};
                    st.sheet_loaded = twod::loadSpriteSheet(twod_.sprites().absolute(st.sprite_image), st.sheet);
                    st.sheet_dirty = false;
                    st.frame_selected = -1;
                }
                ImGui::EndDragDropTarget();
            }
            if (!st.sheet_loaded) {
                ImGui::TextDisabled("Suelta aquí una imagen para usarla como sprite (o hoja de sprites).");
            } else {
                twod::SpriteSheet& s = st.sheet;
                bool changed = false;
                ImGui::SameLine();
                ImGui::TextDisabled("%d x %d px", s.width, s.height);
                ImGui::BeginChild("sp_left", ImVec2(300.0f, 0.0f), ImGuiChildFlags_ResizeX | ImGuiChildFlags_Borders);
                int mode = static_cast<int>(s.mode);
                static const char* kModes[] = {"Single (la imagen entera)", "Multiple (hoja de sprites)"};
                if (ImGui::Combo("Modo", &mode, kModes, 2)) {
                    s.mode = static_cast<twod::SpriteMode>(mode);
                    changed = true;
                }
                changed |= ImGui::DragFloat("Píxeles por unidad", &s.pixels_per_unit, 0.5f, 0.01f, 10000.0f, "%.1f");
                int filter = static_cast<int>(s.filter);
                static const char* kFilters[] = {"Point (pixel art)", "Bilinear"};
                if (ImGui::Combo("Filtro", &filter, kFilters, 2)) {
                    s.filter = static_cast<twod::SpriteFilter>(filter);
                    changed = true;
                }
                if (s.mode == twod::SpriteMode::Single) {
                    changed |= ImGui::SliderFloat2("Pivote", &s.pivot.x, 0.0f, 1.0f, "%.2f");
                } else {
                    ImGui::SeparatorText("Cortar");
                    static const char* kSlice[] = {"Rejilla por tamaño", "Filas y columnas", "Automático"};
                    ImGui::Combo("##slice", &st.slice_mode, kSlice, 3);
                    if (st.slice_mode == 0) {
                        ImGui::DragInt("Ancho", &st.cell_w, 0.2f, 1, 4096);
                        ImGui::DragInt("Alto", &st.cell_h, 0.2f, 1, 4096);
                        ImGui::DragInt("Margen X", &st.offset_x, 0.2f, 0, 4096);
                        ImGui::DragInt("Margen Y", &st.offset_y, 0.2f, 0, 4096);
                        ImGui::DragInt("Separación X", &st.spacing_x, 0.2f, 0, 4096);
                        ImGui::DragInt("Separación Y", &st.spacing_y, 0.2f, 0, 4096);
                    } else if (st.slice_mode == 1) {
                        ImGui::DragInt("Columnas", &st.columns, 0.1f, 1, 256);
                        ImGui::DragInt("Filas", &st.rows, 0.1f, 1, 256);
                    }
                    if (ImGui::Button("Cortar", ImVec2(-1.0f, 0.0f))) {
                        asset::ImageRgba8 image;
                        const bool have_pixels = asset::loadImageRgba8(twod_.sprites().absolute(st.sprite_image), image);
                        const std::vector<std::uint8_t>* rgba = have_pixels ? &image.pixels : nullptr;
                        const std::string base = dialogs::utf8(dialogs::fromUtf8(st.sprite_image).stem());
                        if (st.slice_mode == 0) {
                            s.frames = twod::sliceGrid(s.width, s.height, st.cell_w, st.cell_h, st.offset_x, st.offset_y, st.spacing_x,
                                                       st.spacing_y, base, rgba);
                        } else if (st.slice_mode == 1) {
                            s.frames = twod::sliceCount(s.width, s.height, st.columns, st.rows, base, rgba);
                        } else if (have_pixels) {
                            s.frames = twod::sliceAutomatic(image.pixels, s.width, s.height, base);
                        }
                        changed = true;
                    }
                    ImGui::SeparatorText(("Cortes (" + std::to_string(s.frames.size()) + ")").c_str());
                    for (std::size_t i = 0; i < s.frames.size(); ++i) {
                        if (ImGui::Selectable((std::to_string(i) + ": " + s.frames[i].name).c_str(), st.frame_selected == static_cast<int>(i))) {
                            st.frame_selected = static_cast<int>(i);
                        }
                    }
                    if (st.frame_selected >= 0 && st.frame_selected < static_cast<int>(s.frames.size())) {
                        twod::SpriteFrame& f = s.frames[static_cast<std::size_t>(st.frame_selected)];
                        ImGui::SeparatorText("Corte");
                        changed |= ImGui::InputText("Nombre", &f.name);
                        int xywh[4] = {f.x, f.y, f.w, f.h};
                        if (ImGui::DragInt4("X Y An Al", xywh, 0.2f, 0, 16384)) {
                            f.x = xywh[0];
                            f.y = xywh[1];
                            f.w = std::max(xywh[2], 1);
                            f.h = std::max(xywh[3], 1);
                            changed = true;
                        }
                        changed |= ImGui::SliderFloat2("Pivote", &f.pivot.x, 0.0f, 1.0f, "%.2f");
                    }
                }
                ImGui::EndChild();
                ImGui::SameLine();
                ImGui::BeginChild("sp_image", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
                ImGui::SetNextItemWidth(120.0f);
                ImGui::SliderFloat("Zoom", &st.sprite_zoom, 0.25f, 8.0f, "%.2fx");
                ImVec2 size{};
                const ImTextureID texture = imgui_.image(twod_.sprites().absolute(st.sprite_image), &size);
                if (texture != 0) {
                    const float z = st.sprite_zoom;
                    const ImVec2 p0 = ImGui::GetCursorScreenPos();
                    ImGui::Image(texture, ImVec2(size.x * z, size.y * z));
                    ImDrawList* draw = ImGui::GetWindowDrawList();
                    if (s.mode == twod::SpriteMode::Multiple) {
                        for (std::size_t i = 0; i < s.frames.size(); ++i) {
                            const twod::SpriteFrame& f = s.frames[i];
                            const ImVec2 a(p0.x + f.x * z, p0.y + f.y * z);
                            const ImVec2 b(a.x + f.w * z, a.y + f.h * z);
                            const bool chosen = st.frame_selected == static_cast<int>(i);
                            draw->AddRect(a, b, chosen ? IM_COL32(255, 210, 60, 255) : IM_COL32(90, 200, 255, 200), 0.0f, 0, chosen ? 2.0f : 1.0f);
                            const ImVec2 pivot(a.x + f.pivot.x * f.w * z, b.y - f.pivot.y * f.h * z);
                            draw->AddCircle(pivot, 3.0f, IM_COL32(255, 80, 80, 255));
                            if (ImGui::IsMouseHoveringRect(a, b) && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsWindowHovered()) {
                                st.frame_selected = static_cast<int>(i);
                            }
                        }
                    } else {
                        const ImVec2 pivot(p0.x + s.pivot.x * size.x * z, p0.y + (1.0f - s.pivot.y) * size.y * z);
                        draw->AddCircle(pivot, 4.0f, IM_COL32(255, 80, 80, 255));
                    }
                }
                ImGui::EndChild();
                if (changed) st.sheet_dirty = true;
                if (st.sheet_dirty && !ImGui::IsAnyItemActive() && ImGui::GetIO().MouseDownDuration[0] < 0.0f) {
                    std::string error;
                    if (twod::saveSpriteSheet(twod_.sprites().absolute(st.sprite_image), s, &error)) {
                        st.sheet_dirty = false;
                        twod_.sprites().invalidate(st.sprite_image);
                    } else {
                        std::cerr << "[Editor] " << error << "\n";
                    }
                }
            }
        }
        ImGui::End();
    }

    // --- Capas de orden ---
    if (st.show_layers) {
        ImGui::SetNextWindowSize(ImVec2(320.0f, 360.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Capas de orden###sorting_layers", &st.show_layers)) {
            std::vector<std::string> layers = twod::sortingLayers();
            bool changed = false;
            ImGui::TextWrapped("De atrás adelante. Los Sprite Renderer y Tilemaps eligen una por nombre.");
            int remove = -1;
            int up = -1;
            for (std::size_t i = 0; i < layers.size(); ++i) {
                ImGui::PushID(static_cast<int>(i));
                ImGui::SetNextItemWidth(180.0f);
                changed |= ImGui::InputText("##n", &layers[i]);
                ImGui::SameLine();
                if (i > 0 && ImGui::SmallButton("^")) up = static_cast<int>(i);
                ImGui::SameLine();
                if (layers.size() > 1 && ImGui::SmallButton("x")) remove = static_cast<int>(i);
                ImGui::PopID();
            }
            if (up > 0) {
                std::swap(layers[static_cast<std::size_t>(up)], layers[static_cast<std::size_t>(up - 1)]);
                changed = true;
            }
            if (remove >= 0) {
                layers.erase(layers.begin() + remove);
                changed = true;
            }
            ImGui::SetNextItemWidth(180.0f);
            ImGui::InputTextWithHint("##new", "capa nueva", &st.new_layer);
            ImGui::SameLine();
            if (ImGui::Button("Añadir") && !st.new_layer.empty()) {
                layers.push_back(st.new_layer);
                st.new_layer.clear();
                changed = true;
            }
            if (changed) {
                twod::setSortingLayers(layers);
                twod::saveSortingLayers(project_.settingsFolder() / "SortingLayers.json", layers);
            }
        }
        ImGui::End();
    }
}

}  // namespace cramion::editor

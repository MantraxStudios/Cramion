// Decals en el editor (como los Decal Actors de Unreal):
//
//   Herramienta Estampar (T)  clic sobre cualquier superficie = un decal
//                             orientado a esa cara, con el pincel elegido
//                             (estampa con textura/color, charco o humedad);
//                             rueda + Ctrl = tamano, rueda + Mayus = girar
//   Gizmo                     la caja del decal y su direccion de proyeccion;
//                             se mueve/gira/escala con W/E/R como cualquier
//                             objeto (la escala es el tamano de la caja)
//   Imagenes de Assets/       arrastrarlas a la escena estampa esa imagen;
//                             doble clic = pincel con esa imagen
//
// Los charcos se suman a los de la lluvia global (Clima); con "Solo con
// lluvia" aparecen y crecen con ella.

#include "EditorApp.h"

#include "Dialogs.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <ImGuizmo.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <random>

namespace cramion::editor {

using core::Mat4;
using core::Vec3;

namespace {

const char* const kDecalTypeNames[] = {"Estampa", "Charco", "Humedad"};

// Rayo contra la caja [mn, mx] en su espacio: distancia y normal de la cara.
bool rayBoxFace(const Vec3& o, const Vec3& d, const Vec3& mn, const Vec3& mx, float& t_hit, Vec3& normal) {
    float t_min = 0.0f;
    float t_max = std::numeric_limits<float>::max();
    int axis = -1;
    float sign = 1.0f;
    const float oo[3] = {o.x, o.y, o.z};
    const float dd[3] = {d.x, d.y, d.z};
    const float lo[3] = {mn.x, mn.y, mn.z};
    const float hi[3] = {mx.x, mx.y, mx.z};
    for (int i = 0; i < 3; ++i) {
        if (std::abs(dd[i]) < 1e-9f) {
            if (oo[i] < lo[i] || oo[i] > hi[i]) return false;
            continue;
        }
        float t0 = (lo[i] - oo[i]) / dd[i];
        float t1 = (hi[i] - oo[i]) / dd[i];
        float s = -1.0f;  // entra por la cara de abajo (normal -eje)
        if (t0 > t1) {
            std::swap(t0, t1);
            s = 1.0f;
        }
        if (t0 > t_min) {
            t_min = t0;
            axis = i;
            sign = s;
        }
        t_max = std::min(t_max, t1);
        if (t_min > t_max) return false;
    }
    if (axis < 0) return false;  // el origen esta dentro
    t_hit = t_min;
    normal = Vec3{axis == 0 ? sign : 0.0f, axis == 1 ? sign : 0.0f, axis == 2 ? sign : 0.0f};
    return true;
}

Vec3 transformPoint(const Mat4& m, const Vec3& p) {
    return Vec3{m.m[0][0] * p.x + m.m[1][0] * p.y + m.m[2][0] * p.z + m.m[3][0],
                m.m[0][1] * p.x + m.m[1][1] * p.y + m.m[2][1] * p.z + m.m[3][1],
                m.m[0][2] * p.x + m.m[1][2] * p.y + m.m[2][2] * p.z + m.m[3][2]};
}

Vec3 transformVector(const Mat4& m, const Vec3& v) {
    return Vec3{m.m[0][0] * v.x + m.m[1][0] * v.y + m.m[2][0] * v.z,
                m.m[0][1] * v.x + m.m[1][1] * v.y + m.m[2][1] * v.z,
                m.m[0][2] * v.x + m.m[1][2] * v.y + m.m[2][2] * v.z};
}

bool isImageFile(const std::filesystem::path& path) {
    std::string e = path.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".tga" || e == ".bmp" || e == ".psd";
}

}  // namespace

bool EditorApp::isDecalImage(const std::filesystem::path& path) {
    return isImageFile(path);
}

// Superficie bajo el raton: rayo contra la caja de cada pieza en SU espacio
// (sigue la rotacion del objeto). Para el suelo, paredes y cajas es exacto.
bool EditorApp::surfaceHit(float x, float y, Vec3& point, Vec3& normal) const {
    Vec3 origin{};
    Vec3 direction{};
    if (!mouseRay(x, y, origin, direction) || !sync_) return false;
    float best = std::numeric_limits<float>::max();
    bool found = false;
    for (std::uint32_t i = 0; i < scene_.actors().size(); ++i) {
        const ecs::Entity owner = sync_->entityForActor(world_, i);
        if (!owner.valid() || !owner.activeInHierarchy()) continue;
        Vec3 mn{};
        Vec3 mx{};
        if (!sync_->actorLocalBounds(i, mn, mx)) continue;
        const Mat4 world = owner.worldMatrix();
        const Mat4 to_local = core::inverse(world);
        const Vec3 lo = transformPoint(to_local, origin);
        const Vec3 ld = transformVector(to_local, direction);
        float t = 0.0f;
        Vec3 local_normal{};
        if (!rayBoxFace(lo, ld, mn, mx, t, local_normal)) continue;
        const Vec3 hit = transformPoint(world, lo + ld * t);
        const float distance = core::length(hit - origin);
        if (distance < best) {
            best = distance;
            point = hit;
            // Normal al mundo con la inversa traspuesta (escalas no uniformes).
            normal = core::normalize(Vec3{to_local.m[0][0] * local_normal.x + to_local.m[0][1] * local_normal.y +
                                              to_local.m[0][2] * local_normal.z,
                                          to_local.m[1][0] * local_normal.x + to_local.m[1][1] * local_normal.y +
                                              to_local.m[1][2] * local_normal.z,
                                          to_local.m[2][0] * local_normal.x + to_local.m[2][1] * local_normal.y +
                                              to_local.m[2][2] * local_normal.z});
            found = true;
        }
    }
    // Sin nada debajo: el plano y = 0.
    if (!found && direction.y < -1e-3f) {
        const float t = -origin.y / direction.y;
        if (t > 0.0f && t < 500.0f) {
            point = origin + direction * t;
            normal = Vec3{0.0f, 1.0f, 0.0f};
            found = true;
        }
    }
    return found;
}

// Coloca la caja del decal apoyada en la superficie: su eje Y es la normal
// (proyecta hacia dentro), centrada en el punto, girada `spin` grados.
void EditorApp::placeDecal(ecs::Entity decal, const Vec3& point, const Vec3& normal, float size, float depth,
                           float spin_degrees) {
    const Vec3 up = core::normalize(normal);
    const Vec3 helper = std::abs(up.y) < 0.95f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{0.0f, 0.0f, 1.0f};
    Vec3 right = core::normalize(core::cross(helper, up));
    Vec3 forward = core::cross(right, up);
    const float a = spin_degrees * core::kPi / 180.0f;
    const Vec3 r = right * std::cos(a) + forward * std::sin(a);
    const Vec3 f = forward * std::cos(a) - right * std::sin(a);
    right = r;
    forward = f;
    Mat4 m = Mat4::identity();
    m.m[0][0] = right.x * size;
    m.m[0][1] = right.y * size;
    m.m[0][2] = right.z * size;
    m.m[1][0] = up.x * depth;
    m.m[1][1] = up.y * depth;
    m.m[1][2] = up.z * depth;
    m.m[2][0] = forward.x * size;
    m.m[2][1] = forward.y * size;
    m.m[2][2] = forward.z * size;
    m.m[3][0] = point.x;
    m.m[3][1] = point.y;
    m.m[3][2] = point.z;
    decal.setWorldMatrix(m);
}

ecs::Entity EditorApp::createDecal(int type, ecs::Entity parent) {
    static constexpr const char* kNames[] = {"Decal", "Charco", "Humedad"};
    ecs::Entity e = world_.create(kNames[std::clamp(type, 0, 2)], parent);
    ecs::Decal& d = e.add<ecs::Decal>();
    d.type = static_cast<ecs::DecalType>(std::clamp(type, 0, 2));
    if (d.type == ecs::DecalType::Stamp) {
        d.color = Vec3{0.85f, 0.25f, 0.2f};
    } else if (d.type == ecs::DecalType::Puddle) {
        d.edge_softness = 0.3f;
    }
    // Sobre la superficie en el centro de la vista (o delante de la camara).
    Vec3 point{};
    Vec3 normal{};
    if (!parent.valid()) {
        if (surfaceHit(view_x_ + view_w_ * 0.5f, view_y_ + view_h_ * 0.5f, point, normal)) {
            placeDecal(e, point, normal, d.type == ecs::DecalType::Stamp ? 1.0f : 2.5f, 0.5f, 0.0f);
        } else {
            const scene::Camera& camera = scene_.camera();
            e.setWorldPosition(camera.position() + camera.forward() * 6.0f);
            e.setLocalScale(Vec3{1.0f, 0.5f, 1.0f});
        }
    } else {
        e.setLocalScale(Vec3{1.0f, 0.5f, 1.0f});
    }
    return e;
}

ecs::Entity EditorApp::stampAt(const Vec3& point, const Vec3& normal, const std::string& texture) {
    static std::mt19937 random{1234u};
    StampBrush& b = stamp_brush_;
    float spin = b.spin;
    if (b.random_spin) spin += std::uniform_real_distribution<float>(0.0f, 360.0f)(random);
    ecs::Entity e = world_.create(b.type == 1 ? "Charco" : (b.type == 2 ? "Humedad" : "Decal"), {});
    ecs::Decal& d = e.add<ecs::Decal>();
    d.type = static_cast<ecs::DecalType>(b.type);
    d.texture = texture.empty() ? b.texture : texture;
    d.color = b.color;
    d.opacity = b.opacity;
    d.amount = b.amount;
    d.follow_rain = b.follow_rain;
    d.edge_softness = b.type == 0 && !d.texture.empty() ? 0.05f : 0.25f;
    placeDecal(e, point, normal, b.size, b.depth, spin);
    return e;
}

void EditorApp::drawStampToolbar() {
    ImGui::SameLine();
    const bool active = stamp_mode_;
    if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button("T Estampar")) stamp_mode_ = !stamp_mode_;
    if (active) ImGui::PopStyleColor();
    ImGui::SetItemTooltip("Estampar decals, charcos o humedad sobre las superficies (T)");
    ImGui::SameLine();
    if (ImGui::ArrowButton("##pincel", ImGuiDir_Down)) ImGui::OpenPopup("stamp_brush");
    ImGui::SetItemTooltip("Pincel de estampar");
    if (ImGui::BeginPopup("stamp_brush")) {
        StampBrush& b = stamp_brush_;
        ImGui::SeparatorText("Pincel");
        ImGui::SetNextItemWidth(160.0f);
        ImGui::Combo("Tipo", &b.type, kDecalTypeNames, 3);
        ImGui::SetNextItemWidth(160.0f);
        ImGui::DragFloat("Tamaño", &b.size, 0.01f, 0.05f, 50.0f, "%.2f m");
        ImGui::SetNextItemWidth(160.0f);
        ImGui::DragFloat("Profundidad", &b.depth, 0.01f, 0.02f, 10.0f, "%.2f m");
        ImGui::SetNextItemWidth(160.0f);
        ImGui::DragFloat("Giro", &b.spin, 1.0f, -360.0f, 360.0f, "%.0f°");
        ImGui::Checkbox("Giro aleatorio", &b.random_spin);
        ImGui::ColorEdit3("Color", &b.color.x, ImGuiColorEditFlags_NoInputs);
        ImGui::SetNextItemWidth(160.0f);
        ImGui::SliderFloat("Opacidad", &b.opacity, 0.0f, 1.0f, "%.2f");
        if (b.type != 0) {
            ImGui::SetNextItemWidth(160.0f);
            ImGui::SliderFloat(b.type == 1 ? "Nivel del agua" : "Humedad", &b.amount, 0.0f, 1.0f, "%.2f");
            ImGui::Checkbox("Solo con lluvia global", &b.follow_rain);
        }
        ImGui::TextUnformatted("Textura:");
        ImGui::SameLine();
        ImGui::TextDisabled("%s", b.texture.empty() ? "(ninguna: color / forma)" : b.texture.c_str());
        if (ImGui::Button("Elegir imagen...")) {
            const std::string texture = importDecalImage();
            if (!texture.empty()) b.texture = texture;
        }
        ImGui::SameLine();
        if (ImGui::Button("Quitar")) b.texture.clear();
        ImGui::Separator();
        ImGui::TextDisabled("Clic: estampar | Ctrl+rueda: tamaño | Mayús+rueda: girar | Esc: salir");
        ImGui::EndPopup();
    }
}

// Copia una imagen elegida en el dialogo dentro de Assets/Textures (si no
// esta ya en Assets/) y devuelve su ruta relativa a Assets/.
std::string EditorApp::importDecalImage() {
    const std::filesystem::path file = dialogs::openFile(
        window_.handle(), L"Imagenes (*.png;*.jpg;*.jpeg;*.tga;*.bmp)\0*.png;*.jpg;*.jpeg;*.tga;*.bmp\0Todos\0*.*\0",
        project_.assetsFolder());
    if (file.empty()) return {};
    return decalImageInAssets(file);
}

std::string EditorApp::decalImageInAssets(const std::filesystem::path& file) {
    std::error_code error;
    const std::filesystem::path assets = std::filesystem::weakly_canonical(project_.assetsFolder(), error);
    std::filesystem::path source = std::filesystem::weakly_canonical(file, error);
    std::filesystem::path relative = std::filesystem::relative(source, assets, error);
    if (error || relative.empty() || *relative.begin() == "..") {
        // Fuera del proyecto: se copia a Assets/Textures.
        const std::filesystem::path folder = project_.assetsFolder() / "Textures";
        std::filesystem::create_directories(folder, error);
        std::filesystem::path target = folder / file.filename();
        for (int i = 2; std::filesystem::exists(target); ++i) {
            target = folder / (file.stem().wstring() + L" " + std::to_wstring(i) + file.extension().wstring());
        }
        std::filesystem::copy_file(file, target, error);
        if (error) {
            std::cerr << "[Editor] No se pudo copiar la imagen: " << error.message() << "\n";
            return {};
        }
        std::cout << "[Editor] Imagen copiada a Assets/Textures: " << dialogs::utf8(target.filename()) << "\n";
        refreshDatabase();
        relative = std::filesystem::relative(target, project_.assetsFolder(), error);
    }
    const std::u8string text = relative.generic_u8string();
    return std::string(text.begin(), text.end());
}

// Herramienta de estampar: vista previa de la caja bajo el raton y clic para
// crear el decal. true si se come el clic (no selecciona).
bool EditorApp::drawStampTool() {
    if (!stamp_mode_ || flying_) return false;
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && (view_hovered_ || view_focused_)) {
        stamp_mode_ = false;
        return false;
    }
    if (!view_hovered_) return true;
    StampBrush& b = stamp_brush_;
    // Rueda con Ctrl = tamano, con Mayus = giro.
    if (io.MouseWheel != 0.0f && io.KeyCtrl) b.size = std::clamp(b.size * (io.MouseWheel > 0 ? 1.1f : 1.0f / 1.1f), 0.05f, 50.0f);
    if (io.MouseWheel != 0.0f && io.KeyShift) b.spin += io.MouseWheel * 15.0f;

    Vec3 point{};
    Vec3 normal{};
    if (!surfaceHit(io.MousePos.x, io.MousePos.y, point, normal)) return true;

    // Vista previa: el cuadrado del decal sobre la superficie y la normal.
    const Vec3 up = normal;
    const Vec3 helper = std::abs(up.y) < 0.95f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{0.0f, 0.0f, 1.0f};
    Vec3 right = core::normalize(core::cross(helper, up));
    Vec3 forward = core::cross(right, up);
    const float a = b.spin * core::kPi / 180.0f;
    const Vec3 r = (right * std::cos(a) + forward * std::sin(a)) * (b.size * 0.5f);
    const Vec3 f = (forward * std::cos(a) - right * std::sin(a)) * (b.size * 0.5f);
    const Vec3 lift = up * 0.01f;
    const Vec3 corners[4] = {point + r + f + lift, point - r + f + lift, point - r - f + lift, point + r - f + lift};
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(ImVec2(view_x_, view_y_), ImVec2(view_x_ + view_w_, view_y_ + view_h_), true);
    const ImU32 color = b.type == 1 ? IM_COL32(90, 170, 255, 230) : (b.type == 2 ? IM_COL32(120, 200, 230, 230)
                                                                                  : IM_COL32(255, 170, 60, 230));
    ImVec2 screen[4];
    bool ok = true;
    for (int i = 0; i < 4; ++i) ok = ok && worldToScreen(corners[i], screen[i].x, screen[i].y);
    if (ok) {
        draw->AddQuadFilled(screen[0], screen[1], screen[2], screen[3], (color & 0x00FFFFFF) | 0x30000000);
        draw->AddQuad(screen[0], screen[1], screen[2], screen[3], color, 2.0f);
    }
    float cx = 0.0f, cy = 0.0f, nx = 0.0f, ny = 0.0f;
    if (worldToScreen(point, cx, cy) && worldToScreen(point + up * (b.size * 0.4f), nx, ny)) {
        draw->AddLine(ImVec2(cx, cy), ImVec2(nx, ny), color, 2.0f);
        draw->AddCircleFilled(ImVec2(cx, cy), 3.0f, color);
    }
    draw->PopClipRect();
    ImGui::SetTooltip("%s  %.2f m", kDecalTypeNames[b.type], b.size);

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGuizmo::IsOver()) {
        const ecs::Entity e = stampAt(point, normal, {});
        selectOnly(e.uuid());
        revealInHierarchy(e.uuid());
        commit();
    }
    return true;
}

// Caja de los decals seleccionados y su direccion de proyeccion.
void EditorApp::drawDecalGizmos() {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect(ImVec2(view_x_, view_y_), ImVec2(view_x_ + view_w_, view_y_ + view_h_), true);
    for (const ecs::Entity e : selectedEntities()) {
        const ecs::Decal* d = e.tryGet<ecs::Decal>();
        if (d == nullptr) continue;
        const Mat4 m = e.worldMatrix();
        const ImU32 color = d->type == ecs::DecalType::Puddle
                                ? IM_COL32(90, 170, 255, 230)
                                : (d->type == ecs::DecalType::Wet ? IM_COL32(120, 200, 230, 230) : IM_COL32(255, 170, 60, 230));
        // Caja y flecha en 3D con profundidad: se ve por donde la caja corta
        // la superficie sobre la que proyecta.
        overlayBoxEdges(m, color);
        // Flecha: proyecta desde la cara de arriba (+Y) hacia abajo.
        const Vec3 top = transformPoint(m, Vec3{0.0f, 0.5f, 0.0f});
        const Vec3 bottom = transformPoint(m, Vec3{0.0f, -0.5f, 0.0f});
        const Vec3 down = bottom - top;
        const float span = core::length(down);
        if (span > 1e-5f) {
            overlayLine(top, bottom - down * 0.15f, color);
            overlayCone(bottom - down * 0.15f, down, span * 0.15f, span * 0.05f, color);
        }
    }
    draw->PopClipRect();
}

}  // namespace cramion::editor

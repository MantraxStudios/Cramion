// Modelado poligonal en la vista de escena (como ProBuilder de Unity): modos
// Objeto / Vertices / Aristas / Caras sobre una entidad con EditableMesh,
// seleccion con clic y caja (con o sin rayos X), el gizmo sobre lo elegido
// (Mayus + arrastrar extruye) y la ventana "Modelado" con las formas y las
// operaciones de modeling::ops.

#include "EditorApp.h"

#include "Dialogs.h"

#include <CramionCore/ecs/MathUtil.h>
#include <CramionCore/ecs/RuntimeMesh.h>
#include <CramionCore/physics/PhysicsComponents.h>

#include <imgui.h>
#include <ImGuizmo.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <set>

namespace cramion::editor {

using core::Mat4;
using core::Vec3;
using modeling::Edge;
using modeling::PolyMesh;
namespace ops = modeling::ops;
namespace shapes = modeling::shapes;

namespace {

constexpr ImU32 kWireColor = IM_COL32(110, 190, 255, 170);
constexpr ImU32 kWireObjectColor = IM_COL32(110, 190, 255, 70);
constexpr ImU32 kWireDimColor = IM_COL32(110, 190, 255, 80);
constexpr ImU32 kSelectedColor = IM_COL32(255, 150, 30, 255);
constexpr ImU32 kHoverColor = IM_COL32(255, 240, 110, 255);
constexpr ImU32 kVertexColor = IM_COL32(225, 235, 255, 255);
constexpr ImU32 kFaceFillColor = IM_COL32(255, 140, 20, 90);
constexpr ImU32 kFaceHoverColor = IM_COL32(255, 240, 110, 55);

struct WorldTriangle {
    Vec3 a, b, c;
    int face;
};

std::vector<WorldTriangle> worldTriangles(const PolyMesh& m, const Mat4& world) {
    std::vector<WorldTriangle> out;
    for (std::size_t fi = 0; fi < m.faces.size(); ++fi) {
        const modeling::Face& f = m.faces[fi];
        const std::vector<std::uint32_t> tris = m.triangulateFace(static_cast<int>(fi));
        for (std::size_t t = 0; t + 2 < tris.size(); t += 3) {
            out.push_back({ecs::transformPoint(world, m.positions[f.v[tris[t]]]),
                           ecs::transformPoint(world, m.positions[f.v[tris[t + 1]]]),
                           ecs::transformPoint(world, m.positions[f.v[tris[t + 2]]]), static_cast<int>(fi)});
        }
    }
    return out;
}

// Moller-Trumbore (las dos caras).
bool rayTriangle(const Vec3& origin, const Vec3& dir, const WorldTriangle& t, float& distance) {
    const Vec3 e1 = t.b - t.a, e2 = t.c - t.a;
    const Vec3 p = core::cross(dir, e2);
    const float det = core::dot(e1, p);
    if (std::abs(det) < 1e-12f) return false;
    const float inv = 1.0f / det;
    const Vec3 s = origin - t.a;
    const float u = core::dot(s, p) * inv;
    if (u < 0.0f || u > 1.0f) return false;
    const Vec3 q = core::cross(s, e1);
    const float v = core::dot(dir, q) * inv;
    if (v < 0.0f || u + v > 1.0f) return false;
    distance = core::dot(e2, q) * inv;
    return distance > 1e-5f;
}

float segmentDistance(const ImVec2& p, const ImVec2& a, const ImVec2& b) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len2 = dx * dx + dy * dy;
    float t = len2 > 1e-6f ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2 : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    return std::hypot(p.x - (a.x + dx * t), p.y - (a.y + dy * t));
}

bool modeButton(const char* label, bool active, const char* tooltip) {
    if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    const bool pressed = ImGui::Button(label);
    if (active) ImGui::PopStyleColor();
    if (tooltip != nullptr && *tooltip != 0) ImGui::SetItemTooltip("%s", tooltip);
    return pressed;
}

template <typename T>
void toggleIn(std::vector<T>& list, const T& value, bool add_only) {
    const auto it = std::find(list.begin(), list.end(), value);
    if (it == list.end()) {
        list.push_back(value);
    } else if (!add_only) {
        list.erase(it);
    }
}

}  // namespace

// -----------------------------------------------------------------------------
// Objetivo y seleccion
// -----------------------------------------------------------------------------

modeling::EditableMesh* EditorApp::modelingTarget(ecs::Entity* entity) {
    ecs::Entity e = world_.find(active_);
    modeling::EditableMesh* em = e.valid() ? e.tryGet<modeling::EditableMesh>() : nullptr;
    if (em == nullptr) {
        if (model_target_.valid()) modelingClearSelection();
        model_target_ = Uuid{};
        return nullptr;
    }
    if (!(e.uuid() == model_target_)) {
        model_target_ = e.uuid();
        modelingClearSelection();
    }
    // Deshacer/rehacer o un script cambiaron la malla (tras deshacer, el
    // componente es otro y su revision puede repetirse): lo elegido que ya no
    // existe, fuera.
    model_target_revision_ = em->revision;
    const auto nv = static_cast<std::uint32_t>(em->mesh.positions.size());
    const int nf = static_cast<int>(em->mesh.faces.size());
    std::erase_if(model_vertices_, [&](std::uint32_t v) { return v >= nv; });
    std::erase_if(model_faces_, [&](int f) { return f < 0 || f >= nf; });
    std::erase_if(model_edges_, [&](const Edge& edge) { return edge.a >= nv || edge.b >= nv; });
    if (entity != nullptr) *entity = e;
    return em;
}

bool EditorApp::modelingElementMode() {
    return model_mode_ != ModelMode::Object && !playing() && modelingTarget() != nullptr;
}

void EditorApp::modelingClearSelection() {
    model_vertices_.clear();
    model_edges_.clear();
    model_faces_.clear();
    model_hover_ = -1;
}

void EditorApp::modelingSelectionChanged() {
    std::sort(model_vertices_.begin(), model_vertices_.end());
    model_vertices_.erase(std::unique(model_vertices_.begin(), model_vertices_.end()), model_vertices_.end());
    std::sort(model_faces_.begin(), model_faces_.end());
    model_faces_.erase(std::unique(model_faces_.begin(), model_faces_.end()), model_faces_.end());
    std::sort(model_edges_.begin(), model_edges_.end());
    model_edges_.erase(std::unique(model_edges_.begin(), model_edges_.end()), model_edges_.end());
}

std::vector<std::uint32_t> EditorApp::modelingSelectedVertices(const PolyMesh& mesh) const {
    switch (model_mode_) {
        case ModelMode::Vertex: return model_vertices_;
        case ModelMode::Edge: return ops::verticesOfEdges(model_edges_);
        case ModelMode::Face: return ops::verticesOfFaces(mesh, model_faces_);
        case ModelMode::Object: break;
    }
    std::vector<std::uint32_t> all(mesh.positions.size());
    for (std::size_t i = 0; i < all.size(); ++i) all[i] = static_cast<std::uint32_t>(i);
    return all;
}

void EditorApp::modelingCommit(modeling::EditableMesh& em, const std::string& what) {
    em.markModified();
    model_target_revision_ = em.revision;
    modeling::updateEditableMeshes(world_);
    dirty_ = true;
    commit();
    if (!what.empty()) std::cout << "[Modelado] " << what << " (" << em.mesh.faces.size() << " caras, "
                                 << em.mesh.positions.size() << " vertices)" << std::endl;
}

bool EditorApp::modelingDeleteSelection() {
    if (!modelingElementMode()) return false;
    modeling::EditableMesh* em = modelingTarget();
    if (model_mode_ == ModelMode::Face && !model_faces_.empty()) {
        ops::deleteFaces(em->mesh, model_faces_);
    } else if (model_mode_ == ModelMode::Vertex && !model_vertices_.empty()) {
        // Las caras que usan esos vertices.
        const std::set<std::uint32_t> set(model_vertices_.begin(), model_vertices_.end());
        std::vector<int> faces;
        for (std::size_t fi = 0; fi < em->mesh.faces.size(); ++fi) {
            for (const std::uint32_t v : em->mesh.faces[fi].v) {
                if (set.contains(v)) {
                    faces.push_back(static_cast<int>(fi));
                    break;
                }
            }
        }
        ops::deleteFaces(em->mesh, faces);
    } else if (model_mode_ == ModelMode::Edge && !model_edges_.empty()) {
        std::vector<int> faces;
        for (const Edge& e : model_edges_) {
            for (const int f : em->mesh.facesOfEdge(e)) faces.push_back(f);
        }
        ops::deleteFaces(em->mesh, faces);
    } else {
        return false;
    }
    modelingClearSelection();
    modelingCommit(*em, "Borrar");
    return true;
}

// -----------------------------------------------------------------------------
// Vista: dibujo, gizmo y clics
// -----------------------------------------------------------------------------

void EditorApp::drawModelingOverlay() {
    ecs::Entity e;
    modeling::EditableMesh* em = modelingTarget(&e);
    if (em == nullptr || playing() || !e.activeInHierarchy()) return;
    const PolyMesh& m = em->mesh;
    const Mat4& world = e.worldMatrix();
    const bool element = model_mode_ != ModelMode::Object;
    std::vector<Vec3> p(m.positions.size());
    for (std::size_t i = 0; i < p.size(); ++i) p[i] = ecs::transformPoint(world, m.positions[i]);
    const std::set<Edge> selected_edges(model_edges_.begin(), model_edges_.end());
    const std::set<std::uint32_t> selected_vertices(model_vertices_.begin(), model_vertices_.end());
    const std::set<int> selected_faces(model_faces_.begin(), model_faces_.end());
    const Vec3 eye = scene_.camera().position();
    const auto lift = [&](const Vec3& q) { return q + (eye - q) * 0.004f; };  // sin pelearse con la superficie

    // Caras elegidas (y la de debajo del raton) rellenas.
    if (model_mode_ == ModelMode::Face) {
        const auto fill = [&](int fi, ImU32 color) {
            const modeling::Face& f = m.faces[static_cast<std::size_t>(fi)];
            const std::vector<std::uint32_t> tris = m.triangulateFace(fi);
            for (std::size_t t = 0; t + 2 < tris.size(); t += 3) {
                overlayTriangle(lift(p[f.v[tris[t]]]), lift(p[f.v[tris[t + 1]]]), lift(p[f.v[tris[t + 2]]]), color);
            }
        };
        for (const int fi : selected_faces) {
            if (fi >= 0 && static_cast<std::size_t>(fi) < m.faces.size()) fill(fi, kFaceFillColor);
        }
        if (model_hover_kind_ == static_cast<int>(ModelMode::Face) && model_hover_ >= 0 &&
            static_cast<std::size_t>(model_hover_) < m.faces.size() && !selected_faces.contains(static_cast<int>(model_hover_))) {
            fill(static_cast<int>(model_hover_), kFaceHoverColor);
        }
    }
    // Aristas.
    const bool hover_edge = model_mode_ == ModelMode::Edge && model_hover_kind_ == static_cast<int>(ModelMode::Edge) && model_hover_ >= 0;
    for (const Edge& edge : m.edges()) {
        ImU32 color = element ? kWireColor : kWireObjectColor;
        if (selected_edges.contains(edge)) color = kSelectedColor;
        else if (hover_edge && edge == model_hover_edge_) color = kHoverColor;
        else if (model_mode_ == ModelMode::Face) color = kWireDimColor;
        overlayLine(p[edge.a], p[edge.b], color);
        if (color == kSelectedColor) {
            // Mas gruesa: una segunda linea un poco desplazada en pantalla.
            const float k = gizmoWorldSize((p[edge.a] + p[edge.b]) * 0.5f) * 0.006f;
            const Mat4 view = scene_.camera().view();
            const Vec3 up{view.m[0][1], view.m[1][1], view.m[2][1]};
            overlayLine(p[edge.a] + up * k, p[edge.b] + up * k, color);
        }
    }
    // Vertices (como cuadraditos de cara a la camara).
    if (model_mode_ == ModelMode::Vertex && p.size() <= 60000) {
        const Mat4 view = scene_.camera().view();
        const Vec3 right = core::normalize(Vec3{view.m[0][0], view.m[1][0], view.m[2][0]});
        const Vec3 up = core::normalize(Vec3{view.m[0][1], view.m[1][1], view.m[2][1]});
        for (std::size_t i = 0; i < p.size(); ++i) {
            const bool selected = selected_vertices.contains(static_cast<std::uint32_t>(i));
            const bool hovered = model_hover_kind_ == static_cast<int>(ModelMode::Vertex) && model_hover_ == static_cast<std::int64_t>(i);
            const float s = gizmoWorldSize(p[i]) * (selected || hovered ? 0.045f : 0.03f);
            const Vec3 c = lift(p[i]);
            const ImU32 color = selected ? kSelectedColor : (hovered ? kHoverColor : kVertexColor);
            overlayQuad(c - right * s - up * s, c + right * s - up * s, c + right * s + up * s, c - right * s + up * s, color);
        }
    }
    // Centros de las caras elegidas (donde agarrar).
    if (model_mode_ == ModelMode::Face) {
        for (const int fi : selected_faces) {
            if (fi < 0 || static_cast<std::size_t>(fi) >= m.faces.size()) continue;
            const Vec3 c = ecs::transformPoint(world, m.faceCenter(fi));
            overlayScreenDisc(lift(c), gizmoWorldSize(c) * 0.025f, kSelectedColor);
        }
    }
}

bool EditorApp::drawModelingGizmo(const Mat4& view, const Mat4& projection) {
    if (!modelingElementMode()) {
        model_gizmo_using_ = false;
        return false;
    }
    ecs::Entity e;
    modeling::EditableMesh* em = modelingTarget(&e);
    const bool has_selection = !model_vertices_.empty() || !model_edges_.empty() || !model_faces_.empty();
    if (!has_selection && !model_gizmo_using_) return true;  // sin gizmo del objeto mientras se editan elementos
    const Mat4& world = e.worldMatrix();
    ImGuiIO& io = ImGui::GetIO();

    if (!model_gizmo_using_) {
        const std::vector<std::uint32_t> verts = modelingSelectedVertices(em->mesh);
        if (verts.empty()) return true;
        Vec3 pivot{};
        for (const std::uint32_t v : verts) pivot += ecs::transformPoint(world, em->mesh.positions[v]);
        pivot = pivot * (1.0f / static_cast<float>(verts.size()));
        Mat4 frame = core::translate(pivot);
        if (gizmo_local_) {
            // Ejes del objeto (sin su escala).
            Vec3 position{}, scale{};
            core::Quat rotation{};
            ecs::decomposeMatrix(world, position, rotation, scale);
            frame = core::composeTrs(pivot, rotation, Vec3{1.0f, 1.0f, 1.0f});
        }
        model_gizmo_matrix_ = frame;
        // Un clic puede empezar a arrastrar: la malla de partida.
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && view_hovered_) {
            model_drag_mesh_ = em->mesh;
            model_drag_start_ = frame;
        }
    }

    ImGuizmo::OPERATION operation = ImGuizmo::TRANSLATE;
    float snap[3] = {snap_translate_, snap_translate_, snap_translate_};
    if (gizmo_ == GizmoOperation::Rotate) {
        operation = static_cast<ImGuizmo::OPERATION>(ImGuizmo::ROTATE_X | ImGuizmo::ROTATE_Y | ImGuizmo::ROTATE_Z);
        snap[0] = snap[1] = snap[2] = snap_rotate_;
    } else if (gizmo_ == GizmoOperation::Scale) {
        operation = ImGuizmo::SCALE;
        snap[0] = snap[1] = snap[2] = snap_scale_;
    }
    const bool snapping = snap_enabled_ != io.KeyCtrl;
    ImGuizmo::PushID(0x30DE);
    ImGuizmo::Manipulate(&view.m[0][0], &projection.m[0][0], operation, gizmo_local_ ? ImGuizmo::LOCAL : ImGuizmo::WORLD,
                         &model_gizmo_matrix_.m[0][0], nullptr, snapping ? snap : nullptr);
    const bool using_now = ImGuizmo::IsUsing();
    ImGuizmo::PopID();

    if (using_now && !model_gizmo_using_) {
        // Empieza: con Mayus, primero se extruye lo elegido (y se mueve lo nuevo).
        if (model_drag_mesh_.faces.size() == 0 && model_drag_mesh_.positions.empty()) model_drag_mesh_ = em->mesh;
        if (io.KeyShift && model_mode_ == ModelMode::Face && !model_faces_.empty()) {
            model_faces_ = ops::extrudeFaces(model_drag_mesh_, model_faces_, 0.0f,
                                             model_extrude_individual_ ? ops::ExtrudeMode::Individual : ops::ExtrudeMode::Group);
        } else if (io.KeyShift && model_mode_ == ModelMode::Edge && !model_edges_.empty()) {
            const std::vector<Edge> moved = ops::extrudeEdges(model_drag_mesh_, model_edges_, 0.0f);
            if (!moved.empty()) model_edges_ = moved;
        }
        em->mesh = model_drag_mesh_;
    }
    if (using_now) {
        const Mat4 delta = model_gizmo_matrix_ * core::inverse(model_drag_start_);
        const Mat4 local = core::inverse(world) * delta * world;
        for (const std::uint32_t v : modelingSelectedVertices(model_drag_mesh_)) {
            if (v < em->mesh.positions.size() && v < model_drag_mesh_.positions.size()) {
                em->mesh.positions[v] = ecs::transformPoint(local, model_drag_mesh_.positions[v]);
            }
        }
        em->markModified();
        model_target_revision_ = em->revision;
    }
    if (model_gizmo_using_ && !using_now) {
        model_drag_mesh_.clear();
        modelingCommit(*em, "Mover elementos");
    }
    model_gizmo_using_ = using_now;
    if (gizmo_ != GizmoOperation::None) {
        drawGizmoGeometry(model_gizmo_matrix_, gizmo_local_,
                          gizmo_ == GizmoOperation::Rotate ? 1 : (gizmo_ == GizmoOperation::Scale ? 2 : 0));
    }
    return true;
}

bool EditorApp::handleModelingInput() {
    if (!modelingElementMode()) {
        model_box_ = false;
        model_hover_ = -1;
        return false;
    }
    ecs::Entity e;
    modeling::EditableMesh* em = modelingTarget(&e);
    const PolyMesh& m = em->mesh;
    const Mat4& world = e.worldMatrix();
    ImGuiIO& io = ImGui::GetIO();
    const Vec3 eye = scene_.camera().position();

    // Atajos: 1-4 modos, Esc limpia / sale, Ctrl+A todo.
    if ((view_hovered_ || view_focused_) && !io.WantTextInput && !flying_) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            if (!model_vertices_.empty() || !model_edges_.empty() || !model_faces_.empty()) {
                modelingClearSelection();
            } else {
                model_mode_ = ModelMode::Object;
                return false;
            }
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) {
            modelingClearSelection();
            if (model_mode_ == ModelMode::Vertex) {
                for (std::size_t i = 0; i < m.positions.size(); ++i) model_vertices_.push_back(static_cast<std::uint32_t>(i));
            } else if (model_mode_ == ModelMode::Edge) {
                model_edges_ = m.edges();
            } else {
                for (std::size_t i = 0; i < m.faces.size(); ++i) model_faces_.push_back(static_cast<int>(i));
            }
            modelingSelectionChanged();
        }
    }

    const std::vector<WorldTriangle> triangles = worldTriangles(m, world);
    // Visible: nada de la propia malla entre la camara y el punto.
    const auto visible = [&](const Vec3& point) {
        if (model_xray_) return true;
        const Vec3 to = point - eye;
        const float dist = core::length(to);
        if (dist < 1e-4f) return true;
        const Vec3 dir = to * (1.0f / dist);
        float t = 0.0f;
        for (const WorldTriangle& tri : triangles) {
            if (rayTriangle(eye, dir, tri, t) && t < dist * 0.995f - 1e-3f) return false;
        }
        return true;
    };
    std::vector<Vec3> p(m.positions.size());
    for (std::size_t i = 0; i < p.size(); ++i) p[i] = ecs::transformPoint(world, m.positions[i]);
    const auto screen = [&](const Vec3& q, ImVec2& s) {
        float x = 0.0f, y = 0.0f;
        if (!worldToScreen(q, x, y)) return false;
        s = ImVec2(x, y);
        return true;
    };

    // Lo que hay bajo el raton.
    const ImVec2 mouse = io.MousePos;
    model_hover_ = -1;
    model_hover_kind_ = static_cast<int>(model_mode_);
    if (view_hovered_ && !ImGuizmo::IsUsing() && !flying_ && !model_box_) {
        if (model_mode_ == ModelMode::Vertex) {
            // Los cercanos al raton, del mas cercano al mas lejano; el primero que se ve.
            std::vector<std::pair<float, std::uint32_t>> nearby;
            for (std::size_t i = 0; i < p.size(); ++i) {
                ImVec2 s;
                if (!screen(p[i], s)) continue;
                const float d = std::hypot(s.x - mouse.x, s.y - mouse.y);
                if (d < 12.0f) nearby.emplace_back(d, static_cast<std::uint32_t>(i));
            }
            std::sort(nearby.begin(), nearby.end());
            for (const auto& [d, i] : nearby) {
                if (visible(p[i] + (eye - p[i]) * 0.002f)) {
                    model_hover_ = static_cast<std::int64_t>(i);
                    break;
                }
            }
        } else if (model_mode_ == ModelMode::Edge) {
            std::vector<std::pair<float, Edge>> nearby;
            for (const Edge& edge : m.edges()) {
                ImVec2 a, b;
                if (!screen(p[edge.a], a) || !screen(p[edge.b], b)) continue;
                const float d = segmentDistance(mouse, a, b);
                if (d < 9.0f) nearby.emplace_back(d, edge);
            }
            std::sort(nearby.begin(), nearby.end());
            for (const auto& [d, edge] : nearby) {
                const Vec3 mid = (p[edge.a] + p[edge.b]) * 0.5f;
                if (!visible(mid + (eye - mid) * 0.002f)) continue;
                model_hover_ = 0;
                model_hover_edge_ = edge;
                break;
            }
        } else if (model_mode_ == ModelMode::Face) {
            Vec3 origin{}, dir{};
            if (mouseRay(mouse.x, mouse.y, origin, dir)) {
                float best = 1e30f, t = 0.0f;
                for (const WorldTriangle& tri : triangles) {
                    if (rayTriangle(origin, dir, tri, t) && t < best) {
                        best = t;
                        model_hover_ = tri.face;
                    }
                }
            }
        }
    }

    // Empieza un clic (fuera del gizmo): seleccion o caja.
    if (view_hovered_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt && !ImGuizmo::IsOver() && !ImGuizmo::IsUsing()) {
        model_box_ = true;
        model_box_start_ = mouse;
    }
    if (!model_box_) return view_hovered_;
    const ImVec2 lo{std::min(model_box_start_.x, mouse.x), std::min(model_box_start_.y, mouse.y)};
    const ImVec2 hi{std::max(model_box_start_.x, mouse.x), std::max(model_box_start_.y, mouse.y)};
    const bool dragging = hi.x - lo.x > 4.0f || hi.y - lo.y > 4.0f;
    if (dragging) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(lo, hi, IM_COL32(110, 170, 255, 40));
        draw->AddRect(lo, hi, IM_COL32(110, 170, 255, 200));
    }
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) return true;
    model_box_ = false;
    const bool add = io.KeyShift;
    const bool toggle = io.KeyCtrl;
    if (!add && !toggle) modelingClearSelection();
    if (!dragging) {
        // Clic: lo de debajo (si no hay nada, se deja elegir otro objeto).
        if (model_hover_ < 0) return false;
        if (model_mode_ == ModelMode::Vertex) toggleIn(model_vertices_, static_cast<std::uint32_t>(model_hover_), !toggle);
        else if (model_mode_ == ModelMode::Edge) toggleIn(model_edges_, model_hover_edge_, !toggle);
        else toggleIn(model_faces_, static_cast<int>(model_hover_), !toggle);
        modelingSelectionChanged();
        return true;
    }
    const auto inside = [&](const Vec3& q) {
        ImVec2 s;
        return screen(q, s) && s.x >= lo.x && s.x <= hi.x && s.y >= lo.y && s.y <= hi.y;
    };
    if (model_mode_ == ModelMode::Vertex) {
        for (std::size_t i = 0; i < p.size(); ++i) {
            if (inside(p[i]) && visible(p[i] + (eye - p[i]) * 0.002f)) toggleIn(model_vertices_, static_cast<std::uint32_t>(i), !toggle);
        }
    } else if (model_mode_ == ModelMode::Edge) {
        for (const Edge& edge : m.edges()) {
            const Vec3 mid = (p[edge.a] + p[edge.b]) * 0.5f;
            if (inside(p[edge.a]) && inside(p[edge.b]) && visible(mid + (eye - mid) * 0.002f)) toggleIn(model_edges_, edge, !toggle);
        }
    } else {
        for (std::size_t fi = 0; fi < m.faces.size(); ++fi) {
            const Vec3 c = ecs::transformPoint(world, m.faceCenter(static_cast<int>(fi)));
            const Vec3 n = core::normalize(ecs::transformDirection(world, m.faceNormal(static_cast<int>(fi))));
            const bool facing = model_xray_ || core::dot(n, eye - c) > 0.0f;
            if (inside(c) && facing && visible(c + (eye - c) * 0.002f)) toggleIn(model_faces_, static_cast<int>(fi), !toggle);
        }
    }
    modelingSelectionChanged();
    return true;
}

void EditorApp::drawModelingToolbar() {
    ImGui::SameLine();
    if (modeButton("Modelado", show_modeling_window_, "Modelado poligonal (como ProBuilder): formas, extruir, biselar, booleanas...")) {
        show_modeling_window_ = !show_modeling_window_;
    }
    if (modelingTarget() == nullptr || playing()) return;
    const auto mode = [&](const char* label, ModelMode m, const char* tip) {
        ImGui::SameLine(0.0f, 2.0f);
        if (modeButton(label, model_mode_ == m, tip)) {
            model_mode_ = m;
            model_hover_ = -1;
        }
    };
    mode("Obj##mm", ModelMode::Object, "Objeto (1): mover la pieza entera");
    mode("Vért##mm", ModelMode::Vertex, "Vertices (2)");
    mode("Arist##mm", ModelMode::Edge, "Aristas (3)");
    mode("Caras##mm", ModelMode::Face, "Caras (4). Mayus + arrastrar el gizmo extruye");
}

// -----------------------------------------------------------------------------
// Crear y convertir
// -----------------------------------------------------------------------------

ecs::Entity EditorApp::createModelingShape(shapes::Kind kind, const shapes::Params& params) {
    PolyMesh mesh = shapes::make(kind, params);
    ecs::Entity parent = prefabStageRoot();
    ecs::Entity e = modeling::createEditableEntity(world_, std::move(mesh), shapes::kindName(kind), parent);
    // En el suelo bajo el centro de la vista, o delante de la camara.
    Vec3 point{}, normal{};
    if (surfaceHit(view_x_ + view_w_ * 0.5f, view_y_ + view_h_ * 0.5f, point, normal) &&
        core::length(point - scene_.camera().position()) < 60.0f) {
        e.setWorldPosition(point);
    } else {
        e.setWorldPosition(scene_.camera().position() + scene_.camera().forward() * 6.0f);
    }
    if (snap_enabled_ && snap_translate_ > 0.0f) {
        Vec3 q = e.worldPosition();
        q = Vec3{std::round(q.x / snap_translate_) * snap_translate_, std::round(q.y / snap_translate_) * snap_translate_,
                 std::round(q.z / snap_translate_) * snap_translate_};
        e.setWorldPosition(q);
    }
    selectOnly(e.uuid());
    revealInHierarchy(e.uuid());
    dirty_ = true;
    commit();
    return e;
}

ecs::Entity EditorApp::convertToEditableMesh(ecs::Entity e, float quad_angle) {
    if (!e.valid() || e.has<modeling::EditableMesh>()) return e;
    ecs::MeshRenderer* renderer = e.tryGet<ecs::MeshRenderer>();
    if (renderer == nullptr) {
        std::cerr << "[Modelado] " << e.name() << " no tiene Mesh Renderer" << std::endl;
        return {};
    }
    std::vector<Vec3> positions;
    std::vector<std::uint32_t> indices;
    std::vector<int> materials;
    std::vector<modeling::SlotMaterial> slots;
    if (renderer->mesh) {
        positions = renderer->mesh->vertices;
        for (int s = 0; s < renderer->mesh->subMeshCount(); ++s) {
            const std::vector<std::uint32_t>& tris = renderer->mesh->triangles(s);
            indices.insert(indices.end(), tris.begin(), tris.end());
            materials.insert(materials.end(), tris.size() / 3, s);
        }
        for (const ecs::MeshMaterial& mm : renderer->mesh->materials) {
            modeling::SlotMaterial slot;
            slot.color = Vec3{mm.color.x, mm.color.y, mm.color.z};
            slot.metallic = mm.metallic;
            slot.roughness = mm.roughness;
            slots.push_back(slot);
        }
    } else if (const asset::ModelData* data = sync_->actorModelData(e, scene_)) {
        for (const asset::SkinnedVertex& v : data->vertices) positions.push_back(v.position);
        for (const asset::SubMesh& sub : data->submeshes) {
            for (std::uint32_t i = 0; i < sub.index_count; ++i) indices.push_back(data->indices[sub.first_index + i]);
            materials.insert(materials.end(), sub.index_count / 3, static_cast<int>(sub.material));
        }
        for (const asset::MaterialData& md : data->materials) {
            modeling::SlotMaterial slot;
            slot.color = Vec3{md.base_color.x, md.base_color.y, md.base_color.z};
            slot.metallic = md.metallic;
            slot.roughness = md.roughness;
            slots.push_back(slot);
        }
    } else {
        std::cerr << "[Modelado] " << e.name() << ": su malla aun no esta cargada" << std::endl;
        return {};
    }
    PolyMesh mesh = ops::fromTriangles(positions, indices, materials, quad_angle);
    if (mesh.faces.empty()) return {};
    modeling::EditableMesh& em = e.add<modeling::EditableMesh>();
    em.mesh = std::move(mesh);
    if (!slots.empty()) em.slots = slots;
    em.markModified();
    renderer->model = assets::AssetRef{{}, assets::AssetType::Model};  // la malla editable manda
    modeling::updateEditableMeshes(world_);
    dirty_ = true;
    commit();
    std::cout << "[Modelado] " << e.name() << " convertido: " << em.mesh.faces.size() << " caras" << std::endl;
    return e;
}

// -----------------------------------------------------------------------------
// Ventana
// -----------------------------------------------------------------------------

void EditorApp::drawModelingWindow() {
    if (!show_modeling_window_) return;
    ImGui::SetNextWindowSize(ImVec2(360.0f, 640.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Modelado", &show_modeling_window_)) {
        ImGui::End();
        return;
    }
    if (!has_project_) {
        ImGui::TextDisabled("Abre un proyecto para modelar.");
        ImGui::End();
        return;
    }
    const float full = ImGui::GetContentRegionAvail().x;

    // --- Crear ---
    if (ImGui::CollapsingHeader("Nueva forma", ImGuiTreeNodeFlags_DefaultOpen)) {
        const char* preview = shapes::kindName(static_cast<shapes::Kind>(model_shape_));
        if (ImGui::BeginCombo("Forma", preview)) {
            for (int k = 0; k < static_cast<int>(shapes::Kind::Count); ++k) {
                if (ImGui::Selectable(shapes::kindName(static_cast<shapes::Kind>(k)), k == model_shape_)) {
                    model_shape_ = k;
                    model_params_ = shapes::defaults(static_cast<shapes::Kind>(k));
                }
            }
            ImGui::EndCombo();
        }
        const auto kind = static_cast<shapes::Kind>(model_shape_);
        shapes::Params& p = model_params_;
        ImGui::DragFloat3("Tamano (X Y Z)", &p.size.x, 0.05f, 0.0f, 1000.0f, "%.2f m");
        const bool round = kind == shapes::Kind::Cylinder || kind == shapes::Kind::Cone || kind == shapes::Kind::Sphere ||
                           kind == shapes::Kind::Torus || kind == shapes::Kind::Pipe || kind == shapes::Kind::Arch ||
                           kind == shapes::Kind::Capsule;
        if (round) ImGui::SliderInt("Lados", &p.segments, 3, 128);
        if (kind == shapes::Kind::Sphere || kind == shapes::Kind::Torus || kind == shapes::Kind::Capsule) ImGui::SliderInt("Anillos", &p.rings, 2, 64);
        if (kind == shapes::Kind::Icosphere) ImGui::SliderInt("Subdivisiones", &p.rings, 0, 5);
        if (kind == shapes::Kind::Cube || kind == shapes::Kind::Plane || kind == shapes::Kind::Room || kind == shapes::Kind::Cylinder) {
            if (kind != shapes::Kind::Cylinder) ImGui::SliderInt("Cortes X", &p.subdivisions_x, 1, 32);
            if (kind != shapes::Kind::Plane) ImGui::SliderInt("Cortes Y", &p.subdivisions_y, 1, 32);
            if (kind != shapes::Kind::Cylinder) ImGui::SliderInt("Cortes Z", &p.subdivisions_z, 1, 32);
        }
        if (kind == shapes::Kind::Stairs || kind == shapes::Kind::CurvedStairs) {
            ImGui::SliderInt("Peldanos", &p.steps, 1, 64);
            ImGui::Checkbox("Maciza hasta el suelo", &p.sides);
        }
        if (kind == shapes::Kind::CurvedStairs) ImGui::DragFloat("Radio interior", &p.inner_radius, 0.05f, 0.0f, 100.0f, "%.2f m");
        if (kind == shapes::Kind::CurvedStairs || kind == shapes::Kind::Arch) ImGui::SliderFloat("Angulo", &p.angle, 10.0f, kind == shapes::Kind::Arch ? 359.0f : 1080.0f, "%.0f°");
        if (kind == shapes::Kind::Torus || kind == shapes::Kind::Pipe || kind == shapes::Kind::Arch ||
            (kind == shapes::Kind::Stairs && !p.sides) || (kind == shapes::Kind::CurvedStairs && !p.sides)) {
            ImGui::DragFloat(kind == shapes::Kind::Torus ? "Radio del tubo" : "Grosor", &p.thickness, 0.01f, 0.01f, 50.0f, "%.2f m");
        }
        if (kind == shapes::Kind::Door) ImGui::SliderFloat2("Hueco (ancho, alto)", &p.door.x, 0.05f, 0.95f, "%.2f");
        if (round) ImGui::Checkbox("Lados suaves", &p.smooth);
        if (kind == shapes::Kind::Cylinder) ImGui::Checkbox("Tapas", &p.caps);
        if (ImGui::Button("Crear", ImVec2(full * 0.5f, 0.0f))) {
            createModelingShape(kind, p);
            model_mode_ = ModelMode::Object;
        }
        ImGui::SameLine();
        if (ImGui::Button("Por defecto", ImVec2(-1.0f, 0.0f))) model_params_ = shapes::defaults(kind);
        ImGui::Separator();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::DragFloat("##quadangle", &model_quad_angle_, 0.1f, 0.0f, 45.0f, "quads < %.1f°");
        ImGui::SetItemTooltip("Al convertir: junta en quads los triangulos casi coplanares (0 = deja los triangulos)");
        ImGui::SameLine();
        if (ImGui::Button("Convertir seleccion en editable")) {
            for (ecs::Entity s : selectedEntities()) convertToEditableMesh(s, model_quad_angle_);
        }
        ImGui::SetItemTooltip("Modelos importados y mallas por codigo pasan a malla editable (como ProBuilderize)");
    }

    ecs::Entity e;
    modeling::EditableMesh* em = modelingTarget(&e);
    if (em == nullptr) {
        ImGui::Spacing();
        ImGui::TextWrapped("Elige un objeto con Malla editable (o crea una forma) para editar sus vertices, aristas y caras.");
        ImGui::End();
        return;
    }
    PolyMesh& mesh = em->mesh;

    // --- Modo y seleccion ---
    ImGui::SeparatorText(e.name().c_str());
    const auto mode = [&](const char* label, ModelMode m) {
        if (modeButton((std::string(label) + "##modo").c_str(), model_mode_ == m, "")) model_mode_ = m;
        ImGui::SameLine();
    };
    mode("Objeto", ModelMode::Object);
    mode("Vertices", ModelMode::Vertex);
    mode("Aristas", ModelMode::Edge);
    mode("Caras", ModelMode::Face);
    ImGui::NewLine();
    ImGui::Checkbox("Rayos X (elegir lo de detras)", &model_xray_);
    std::size_t triangles = 0;
    for (std::size_t fi = 0; fi < mesh.faces.size(); ++fi) triangles += mesh.faces[fi].v.size() >= 3 ? mesh.faces[fi].v.size() - 2 : 0;
    ImGui::TextDisabled("%zu vertices, %zu caras, %zu triangulos", mesh.positions.size(), mesh.faces.size(), triangles);
    const std::size_t selected = model_mode_ == ModelMode::Vertex ? model_vertices_.size()
                                 : model_mode_ == ModelMode::Edge ? model_edges_.size()
                                 : model_mode_ == ModelMode::Face ? model_faces_.size() : 0;
    if (model_mode_ != ModelMode::Object) ImGui::Text("Elegidos: %zu", selected);

    if (model_mode_ != ModelMode::Object && ImGui::CollapsingHeader("Seleccion", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Button("Todo")) {
            modelingClearSelection();
            if (model_mode_ == ModelMode::Vertex) {
                for (std::size_t i = 0; i < mesh.positions.size(); ++i) model_vertices_.push_back(static_cast<std::uint32_t>(i));
            } else if (model_mode_ == ModelMode::Edge) {
                model_edges_ = mesh.edges();
            } else {
                for (std::size_t i = 0; i < mesh.faces.size(); ++i) model_faces_.push_back(static_cast<int>(i));
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Nada")) modelingClearSelection();
        ImGui::SameLine();
        if (ImGui::Button("Invertir")) {
            if (model_mode_ == ModelMode::Vertex) {
                const std::set<std::uint32_t> was(model_vertices_.begin(), model_vertices_.end());
                model_vertices_.clear();
                for (std::size_t i = 0; i < mesh.positions.size(); ++i) {
                    if (!was.contains(static_cast<std::uint32_t>(i))) model_vertices_.push_back(static_cast<std::uint32_t>(i));
                }
            } else if (model_mode_ == ModelMode::Edge) {
                const std::set<Edge> was(model_edges_.begin(), model_edges_.end());
                model_edges_.clear();
                for (const Edge& edge : mesh.edges()) {
                    if (!was.contains(edge)) model_edges_.push_back(edge);
                }
            } else {
                const std::set<int> was(model_faces_.begin(), model_faces_.end());
                model_faces_.clear();
                for (std::size_t i = 0; i < mesh.faces.size(); ++i) {
                    if (!was.contains(static_cast<int>(i))) model_faces_.push_back(static_cast<int>(i));
                }
            }
        }
        if (model_mode_ == ModelMode::Face) {
            if (ImGui::Button("Crecer")) model_faces_ = ops::growFaces(mesh, model_faces_);
            ImGui::SameLine();
            if (ImGui::Button("Encoger")) model_faces_ = ops::shrinkFaces(mesh, model_faces_);
            ImGui::SameLine();
            if (ImGui::Button("Conectadas")) model_faces_ = ops::linkedFaces(mesh, model_faces_);
            if (ImGui::Button("Mismo plano") && !model_faces_.empty()) {
                std::vector<int> out;
                for (const int f : model_faces_) {
                    for (const int g : ops::facesByAngle(mesh, f, model_angle_)) out.push_back(g);
                }
                model_faces_ = out;
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90.0f);
            ImGui::DragFloat("##angle", &model_angle_, 0.5f, 0.0f, 180.0f, "< %.0f°");
            ImGui::SameLine();
            if (ImGui::Button("Mismo material") && !model_faces_.empty()) {
                model_faces_ = ops::facesWithMaterial(mesh, mesh.faces[static_cast<std::size_t>(model_faces_.front())].material);
            }
        }
        if (model_mode_ == ModelMode::Edge) {
            if (ImGui::Button("Bucle") && !model_edges_.empty()) {
                std::vector<Edge> out;
                for (const Edge& edge : model_edges_) {
                    for (const Edge& l : ops::edgeLoop(mesh, edge)) out.push_back(l);
                }
                model_edges_ = out;
            }
            ImGui::SetItemTooltip("Sigue las aristas por la rejilla de quads (como Alt+clic en Blender)");
            ImGui::SameLine();
            if (ImGui::Button("Anillo") && !model_edges_.empty()) {
                std::vector<Edge> out;
                for (const Edge& edge : model_edges_) {
                    for (const Edge& r : ops::edgeRing(mesh, edge)) out.push_back(r);
                }
                model_edges_ = out;
            }
            ImGui::SameLine();
            if (ImGui::Button("Bordes")) model_edges_ = mesh.borderEdges();
            ImGui::SetItemTooltip("Las aristas de los agujeros");
        }
        modelingSelectionChanged();
    }

    // --- Operaciones ---
    // Con su propio ID: "Caras" tambien es un boton de modo (mismo texto = mismo ID en ImGui).
    const char* header = model_mode_ == ModelMode::Face     ? "Operaciones: caras###model_ops"
                         : model_mode_ == ModelMode::Edge   ? "Operaciones: aristas###model_ops"
                         : model_mode_ == ModelMode::Vertex ? "Operaciones: vertices###model_ops"
                                                            : "Operaciones: objeto###model_ops";
    if (ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen)) {
        const float button = full * 0.42f;
        if (model_mode_ == ModelMode::Face) {
            const bool any = !model_faces_.empty();
            ImGui::BeginDisabled(!any);
            if (ImGui::Button("Extruir", ImVec2(button, 0.0f))) {
                model_faces_ = ops::extrudeFaces(mesh, model_faces_, model_extrude_,
                                                 model_extrude_individual_ ? ops::ExtrudeMode::Individual : ops::ExtrudeMode::Group);
                modelingCommit(*em, "Extruir");
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.0f);
            ImGui::DragFloat("##extrude", &model_extrude_, 0.01f, -100.0f, 100.0f, "%.2f m");
            ImGui::SameLine();
            ImGui::Checkbox("Separadas##ex", &model_extrude_individual_);
            if (ImGui::Button("Inset", ImVec2(button, 0.0f))) {
                model_faces_ = ops::insetFaces(mesh, model_faces_, model_inset_, model_inset_individual_);
                modelingCommit(*em, "Inset");
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80.0f);
            ImGui::DragFloat("##inset", &model_inset_, 0.005f, 0.001f, 100.0f, "%.3f m");
            ImGui::SameLine();
            ImGui::Checkbox("Separadas##in", &model_inset_individual_);
            if (ImGui::Button("Subdividir##caras", ImVec2(button, 0.0f))) {
                model_faces_ = ops::subdivideFaces(mesh, model_faces_);
                modelingCommit(*em, "Subdividir caras");
            }
            ImGui::SameLine();
            if (ImGui::Button("Unir en una", ImVec2(-1.0f, 0.0f))) {
                const int merged = ops::mergeFaces(mesh, model_faces_);
                if (merged >= 0) {
                    model_faces_ = {merged};
                    modelingCommit(*em, "Unir caras");
                } else {
                    std::cerr << "[Modelado] Las caras no forman una sola region sin agujeros" << std::endl;
                }
            }
            if (ImGui::Button("Triangular", ImVec2(button, 0.0f))) {
                model_faces_ = ops::triangulateFaces(mesh, model_faces_);
                modelingCommit(*em, "Triangular");
            }
            ImGui::SameLine();
            if (ImGui::Button("Voltear normales", ImVec2(-1.0f, 0.0f))) {
                ops::flipFaces(mesh, model_faces_);
                modelingCommit(*em, "Voltear");
            }
            if (ImGui::Button("Duplicar", ImVec2(button, 0.0f))) {
                model_faces_ = ops::duplicateFaces(mesh, model_faces_);
                modelingCommit(*em, "Duplicar caras");
            }
            ImGui::SameLine();
            if (ImGui::Button("Separar en objeto", ImVec2(-1.0f, 0.0f))) {
                PolyMesh piece = ops::detachFaces(mesh, model_faces_, true);
                modelingClearSelection();
                ecs::Entity copy = modeling::createEditableEntity(world_, std::move(piece), e.name() + " (parte)", e.parent());
                copy.setWorldMatrix(e.worldMatrix());
                copy.get<modeling::EditableMesh>().slots = em->slots;
                if (const ecs::MeshRenderer* r = e.tryGet<ecs::MeshRenderer>()) copy.get<ecs::MeshRenderer>().materials = r->materials;
                modelingCommit(*em, "Separar caras");
            }
            if (ImGui::Button("Borrar caras", ImVec2(button, 0.0f))) modelingDeleteSelection();
            ImGui::EndDisabled();
        } else if (model_mode_ == ModelMode::Edge) {
            const bool any = !model_edges_.empty();
            ImGui::BeginDisabled(!any);
            if (ImGui::Button("Biselar", ImVec2(button, 0.0f))) {
                const std::vector<int> faces = ops::bevelEdges(mesh, model_edges_, model_bevel_);
                modelingClearSelection();
                modelingCommit(*em, "Biselar (" + std::to_string(faces.size()) + " caras)");
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::DragFloat("##bevel", &model_bevel_, 0.005f, 0.001f, 100.0f, "%.3f m");
            if (ImGui::Button("Extruir borde", ImVec2(button, 0.0f))) {
                model_edges_ = ops::extrudeEdges(mesh, model_edges_, model_extrude_);
                modelingCommit(*em, "Extruir aristas");
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::DragFloat("##extrude_edges", &model_extrude_, 0.01f, -100.0f, 100.0f, "%.2f m");
            ImGui::SetItemTooltip("Solo aristas de borde (las de un agujero o de una superficie abierta)");
            if (ImGui::Button("Conectar##aristas", ImVec2(button, 0.0f))) {
                model_edges_ = ops::connectEdges(mesh, model_edges_);
                modelingCommit(*em, "Conectar aristas");
            }
            ImGui::SetItemTooltip("Une los puntos medios de las aristas elegidas dentro de cada cara");
            ImGui::SameLine();
            if (ImGui::Button("Insertar bucle", ImVec2(-1.0f, 0.0f))) {
                std::vector<Edge> out;
                for (const Edge& edge : std::vector<Edge>(model_edges_)) {
                    for (const Edge& n : ops::insertEdgeLoop(mesh, edge, model_loop_t_)) out.push_back(n);
                    break;  // un bucle por clic (las demas aristas cambiaron)
                }
                model_edges_ = out;
                modelingCommit(*em, "Insertar bucle");
            }
            ImGui::SetNextItemWidth(button);
            ImGui::SliderFloat("Donde corta el bucle", &model_loop_t_, 0.05f, 0.95f, "%.2f");
            if (ImGui::Button("Subdividir##aristas", ImVec2(button, 0.0f))) {
                const std::vector<std::uint32_t> points = ops::subdivideEdges(mesh, model_edges_, model_cuts_);
                modelingClearSelection();
                modelingCommit(*em, "Cortar aristas (" + std::to_string(points.size()) + " vertices)");
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::SliderInt("##cuts", &model_cuts_, 1, 16, "%d cortes");
            ImGui::BeginDisabled(model_edges_.size() != 2);
            if (ImGui::Button("Puente", ImVec2(button, 0.0f))) {
                const int face = ops::bridgeEdges(mesh, model_edges_[0], model_edges_[1]);
                if (face >= 0) modelingCommit(*em, "Puente");
                else std::cerr << "[Modelado] El puente necesita dos aristas de borde" << std::endl;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Rellenar agujero", ImVec2(-1.0f, 0.0f))) {
                const std::vector<int> faces = ops::fillHoles(mesh, model_edges_);
                modelingClearSelection();
                modelingCommit(*em, "Rellenar (" + std::to_string(faces.size()) + " caras)");
            }
            if (ImGui::Button("Borrar caras de las aristas", ImVec2(-1.0f, 0.0f))) modelingDeleteSelection();
            ImGui::EndDisabled();
        } else if (model_mode_ == ModelMode::Vertex) {
            const bool any = !model_vertices_.empty();
            ImGui::BeginDisabled(!any);
            if (ImGui::Button("Soldar", ImVec2(button, 0.0f))) {
                const int removed = ops::weldVertices(mesh, model_vertices_, model_weld_);
                modelingClearSelection();
                modelingCommit(*em, "Soldar (" + std::to_string(removed) + " menos)");
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::DragFloat("##weld", &model_weld_, 0.001f, 0.0001f, 10.0f, "a %.3f m");
            if (ImGui::Button("Colapsar", ImVec2(button, 0.0f))) {
                model_vertices_ = {ops::collapseVertices(mesh, model_vertices_)};
                modelingCommit(*em, "Colapsar");
            }
            ImGui::SetItemTooltip("Junta los vertices en su centro");
            ImGui::SameLine();
            if (ImGui::Button("Separar", ImVec2(-1.0f, 0.0f))) {
                ops::splitVertices(mesh, model_vertices_);
                modelingCommit(*em, "Separar vertices");
            }
            if (ImGui::Button("Conectar##vertices", ImVec2(button, 0.0f))) {
                ops::connectVertices(mesh, model_vertices_);
                modelingCommit(*em, "Conectar vertices");
            }
            ImGui::SetItemTooltip("Corta las caras entre los vertices elegidos");
            ImGui::SameLine();
            if (ImGui::Button("Ajustar a rejilla", ImVec2(-1.0f, 0.0f))) {
                ops::snapToGrid(mesh, model_vertices_, snap_translate_);
                modelingCommit(*em, "Ajustar a la rejilla");
            }
            ImGui::EndDisabled();
        }
        // Para todo el objeto (o lo elegido).
        ImGui::SeparatorText("Toda la malla");
        const std::vector<std::uint32_t> verts = modelingSelectedVertices(mesh);
        if (ImGui::Button("Relajar", ImVec2(button, 0.0f))) {
            ops::relax(mesh, model_mode_ == ModelMode::Object ? std::vector<std::uint32_t>{} : verts, model_relax_, 3);
            modelingCommit(*em, "Relajar");
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderFloat("##relax", &model_relax_, 0.05f, 1.0f, "fuerza %.2f");
        if (ImGui::Button("Ruido", ImVec2(button, 0.0f))) {
            ops::randomize(mesh, model_mode_ == ModelMode::Object ? std::vector<std::uint32_t>{} : verts, model_noise_,
                           static_cast<std::uint32_t>(ImGui::GetFrameCount()));
            modelingCommit(*em, "Ruido");
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::DragFloat("##noise", &model_noise_, 0.005f, 0.0f, 10.0f, "%.3f m");
        if (ImGui::Button("Suavizar (Catmull-Clark)", ImVec2(button * 1.6f, 0.0f))) {
            ops::subdivideSmooth(mesh, model_smooth_levels_);
            for (modeling::Face& f : mesh.faces) f.smoothing = std::max(f.smoothing, 1);
            modelingClearSelection();
            modelingCommit(*em, "Catmull-Clark");
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::SliderInt("##levels", &model_smooth_levels_, 1, 3, "x%d");
        if (ImGui::Button("Espejo", ImVec2(button * 0.7f, 0.0f))) {
            ops::mirror(mesh, model_mirror_axis_, true, 0.0f);
            modelingClearSelection();
            modelingCommit(*em, "Espejo (simetria)");
        }
        ImGui::SetItemTooltip("Copia reflejada por el plano del eje que pasa por el pivote, soldada en la costura");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f);
        ImGui::Combo("##axis", &model_mirror_axis_, "X\0Y\0Z\0");
        ImGui::SameLine();
        if (ImGui::Button("Reflejar")) {
            ops::mirror(mesh, model_mirror_axis_, false, 0.0f);
            modelingCommit(*em, "Reflejar");
        }
        if (ImGui::Button("Conformar normales", ImVec2(button, 0.0f))) {
            ops::conformNormals(mesh);
            modelingCommit(*em, "Conformar normales");
        }
        ImGui::SameLine();
        if (ImGui::Button("Soldar todo", ImVec2(-1.0f, 0.0f))) {
            const int removed = ops::weldVertices(mesh, {}, model_weld_);
            modelingClearSelection();
            modelingCommit(*em, "Soldar todo (" + std::to_string(removed) + " menos)");
        }
        if (ImGui::Button("Pivote al centro", ImVec2(button, 0.0f))) {
            const Vec3 offset = ops::centerPivot(mesh, false);
            e.setWorldPosition(ecs::transformPoint(e.worldMatrix(), offset));
            modelingCommit(*em, "Pivote al centro");
        }
        ImGui::SameLine();
        if (ImGui::Button("Pivote a la base", ImVec2(-1.0f, 0.0f))) {
            const Vec3 offset = ops::centerPivot(mesh, true);
            e.setWorldPosition(ecs::transformPoint(e.worldMatrix(), offset));
            modelingCommit(*em, "Pivote a la base");
        }
        if (ImGui::Button("Aplicar escala y giro", ImVec2(button, 0.0f))) {
            // La transformacion (sin la posicion) pasa a los vertices.
            Mat4 local = e.localMatrix();
            local.m[3][0] = local.m[3][1] = local.m[3][2] = 0.0f;
            for (Vec3& q : mesh.positions) q = ecs::transformPoint(local, q);
            const Vec3 x{local.m[0][0], local.m[0][1], local.m[0][2]};
            const Vec3 y{local.m[1][0], local.m[1][1], local.m[1][2]};
            const Vec3 z{local.m[2][0], local.m[2][1], local.m[2][2]};
            if (core::dot(core::cross(x, y), z) < 0.0f) {
                std::vector<int> all(mesh.faces.size());
                for (std::size_t i = 0; i < all.size(); ++i) all[i] = static_cast<int>(i);
                ops::flipFaces(mesh, all);
            }
            e.setLocalRotation(core::Quat{});
            e.setLocalScale(Vec3{1.0f, 1.0f, 1.0f});
            modelingCommit(*em, "Aplicar escala y giro");
        }
        ImGui::SameLine();
        if (ImGui::Button("Exportar OBJ", ImVec2(-1.0f, 0.0f))) {
            const std::filesystem::path folder = project_.assetsFolder() / "Modelos";
            std::error_code ec;
            std::filesystem::create_directories(folder, ec);
            std::string stem = e.name();
            for (char& c : stem) {
                if (std::string_view("\\/:*?\"<>|").find(c) != std::string_view::npos) c = '_';
            }
            const std::filesystem::path file = folder / dialogs::fromUtf8(stem + ".obj");
            std::ofstream out(file, std::ios::binary);
            out << ops::toObj(mesh, stem);
            std::cout << "[Modelado] Exportado " << dialogs::utf8(file) << std::endl;
        }
        // Objetos: combinar y booleanas con otro seleccionado.
        const std::vector<ecs::Entity> others = [&] {
            std::vector<ecs::Entity> out;
            for (ecs::Entity s : selectedEntities()) {
                if (!(s.uuid() == e.uuid()) && s.has<modeling::EditableMesh>()) out.push_back(s);
            }
            return out;
        }();
        ImGui::SeparatorText("Con otros objetos");
        ImGui::BeginDisabled(others.empty());
        if (ImGui::Button("Combinar", ImVec2(button, 0.0f))) {
            const Mat4 to_local = core::inverse(e.worldMatrix());
            for (ecs::Entity o : others) {
                ops::append(mesh, o.get<modeling::EditableMesh>().mesh, to_local * o.worldMatrix());
                world_.destroy(o);
            }
            selectOnly(e.uuid());
            modelingCommit(*em, "Combinar objetos");
        }
        ImGui::SetItemTooltip("Mete en este (el activo) las mallas de los otros seleccionados");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100.0f);
        ImGui::Combo("##boolop", &model_boolean_op_, "Union\0Resta\0Interseccion\0");
        ImGui::SameLine();
        if (ImGui::Button("Booleana")) {
            PolyMesh result = mesh;
            Mat4 result_world = e.worldMatrix();
            for (ecs::Entity o : others) {
                result = ops::boolean(result, result_world, o.get<modeling::EditableMesh>().mesh, o.worldMatrix(),
                                      static_cast<ops::BoolOp>(model_boolean_op_));
                o.setActive(false);  // se queda oculto por si se quiere volver a usar
            }
            mesh = std::move(result);
            modelingClearSelection();
            modelingCommit(*em, "Booleana");
        }
        ImGui::SetItemTooltip("El activo con los otros seleccionados (que quedan ocultos)");
        ImGui::EndDisabled();
        if (others.empty()) ImGui::TextDisabled("Selecciona mas objetos editables (Ctrl+clic) para combinar o booleanas.");
    }

    // --- Materiales, suavizado y UV de las caras elegidas ---
    if (ImGui::CollapsingHeader("Materiales y suavizado")) {
        const int slots = static_cast<int>(em->slots.size());
        ImGui::SetNextItemWidth(100.0f);
        ImGui::InputInt("Hueco", &model_material_);
        model_material_ = std::clamp(model_material_, 0, 63);
        if (model_material_ < slots) {
            ImGui::SameLine();
            ImGui::ColorEdit3("##slotcolor", &em->slots[static_cast<std::size_t>(model_material_)].color.x, ImGuiColorEditFlags_NoInputs);
            if (ImGui::IsItemDeactivatedAfterEdit()) modelingCommit(*em, "");
        }
        ImGui::BeginDisabled(model_faces_.empty() || model_mode_ != ModelMode::Face);
        if (ImGui::Button("Asignar a las caras", ImVec2(-1.0f, 0.0f))) {
            while (static_cast<int>(em->slots.size()) <= model_material_) em->slots.push_back(modeling::SlotMaterial{});
            for (const int f : model_faces_) mesh.faces[static_cast<std::size_t>(f)].material = model_material_;
            modelingCommit(*em, "Material de caras");
        }
        ImGui::SetNextItemWidth(100.0f);
        ImGui::InputInt("Grupo de suavizado", &model_smoothing_);
        model_smoothing_ = std::clamp(model_smoothing_, 0, 255);
        if (ImGui::Button("Suave", ImVec2(full * 0.42f, 0.0f))) {
            for (const int f : model_faces_) mesh.faces[static_cast<std::size_t>(f)].smoothing = std::max(model_smoothing_, 1);
            modelingCommit(*em, "Suavizado");
        }
        ImGui::SameLine();
        if (ImGui::Button("Duro", ImVec2(-1.0f, 0.0f))) {
            for (const int f : model_faces_) mesh.faces[static_cast<std::size_t>(f)].smoothing = 0;
            modelingCommit(*em, "Aristas duras");
        }
        ImGui::EndDisabled();
        ImGui::TextDisabled("Los .crmat de cada hueco se ponen en el Mesh Renderer (Materiales).");
    }
    if (ImGui::CollapsingHeader("UV de las caras")) {
        ImGui::BeginDisabled(model_faces_.empty() || model_mode_ != ModelMode::Face);
        if (!model_faces_.empty() && ImGui::Button("Leer de la primera")) {
            model_uv_ = mesh.faces[static_cast<std::size_t>(model_faces_.front())].uvs;
        }
        modeling::FaceUv& uv = model_uv_;
        int uv_mode = static_cast<int>(uv.mode);
        int uv_fill = static_cast<int>(uv.fill);
        ImGui::Combo("Proyeccion", &uv_mode, "Caja (bloques)\0Plana (la cara)\0Manual\0");
        ImGui::Combo("Relleno", &uv_fill, "Repetir (metros)\0Encajar\0Estirar\0");
        uv.mode = static_cast<modeling::UvMode>(uv_mode);
        uv.fill = static_cast<modeling::UvFill>(uv_fill);
        ImGui::DragFloat2("Escala", &uv.scale.x, 0.01f, 0.01f, 100.0f, "%.2f");
        ImGui::DragFloat2("Desplazamiento", &uv.offset.x, 0.01f, -100.0f, 100.0f, "%.2f");
        ImGui::SliderFloat("Giro", &uv.rotation, -180.0f, 180.0f, "%.0f°");
        ImGui::Checkbox("Voltear U", &uv.flip_u);
        ImGui::SameLine();
        ImGui::Checkbox("Voltear V", &uv.flip_v);
        ImGui::SameLine();
        ImGui::Checkbox("Cambiar U/V", &uv.swap_uv);
        ImGui::Checkbox("En el mundo (encajan entre piezas)", &uv.world_space);
        if (ImGui::Button("Aplicar a las caras", ImVec2(-1.0f, 0.0f))) {
            for (const int f : model_faces_) {
                modeling::Face& face = mesh.faces[static_cast<std::size_t>(f)];
                if (uv.mode == modeling::UvMode::Manual && face.uv.size() != face.v.size()) {
                    // Manual: parte de lo que se ve ahora (con la proyeccion de antes).
                    face.uv.resize(face.v.size());
                    for (std::size_t c = 0; c < face.v.size(); ++c) {
                        face.uv[c] = modeling::faceCornerUv(mesh, f, static_cast<int>(c), e.worldMatrix(), em->uv_scale);
                    }
                }
                const std::vector<core::Vec2> keep = face.uv;
                face.uvs = uv;
                face.uv = keep;
            }
            modelingCommit(*em, "UV");
        }
        if (ImGui::Button("Girar 90°", ImVec2(full * 0.42f, 0.0f))) {
            for (const int f : model_faces_) {
                float& r = mesh.faces[static_cast<std::size_t>(f)].uvs.rotation;
                r = std::fmod(r + 90.0f, 360.0f);
            }
            modelingCommit(*em, "Girar UV");
        }
        ImGui::SameLine();
        if (ImGui::Button("Restablecer", ImVec2(-1.0f, 0.0f))) {
            for (const int f : model_faces_) {
                mesh.faces[static_cast<std::size_t>(f)].uvs = modeling::FaceUv{};
                mesh.faces[static_cast<std::size_t>(f)].uv.clear();
            }
            modelingCommit(*em, "UV por defecto");
        }
        ImGui::EndDisabled();
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Atajos: 1-4 modo, Ctrl+A todo, Supr borrar, Esc soltar,");
    ImGui::TextDisabled("Mayus + gizmo extruir, W/E/R mover/girar/escalar.");
    ImGui::End();
}

}  // namespace cramion::editor

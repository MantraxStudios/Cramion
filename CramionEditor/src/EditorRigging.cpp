// Editor: esqueletos (ver y mover huesos, sockets), IK de animales, ragdoll y
// phys bones: su parte del Inspector y sus gizmos en la Escena.

#include "EditorApp.h"

#include <CramionCore/anim/Creature.h>
#include <CramionCore/anim/Humanoid.h>
#include <CramionCore/ecs/MathUtil.h>
#include <CramionCore/ecs/Rigging.h>
#include <CramionCore/physics/Ragdoll.h>

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iostream>
#include <unordered_map>

namespace cramion::editor {

using core::Quat;
using core::Vec3;

namespace {

std::string lowerText(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

}  // namespace

// El esqueleto que tiene que ver con `entity`: el suyo (o el de su pieza
// animada) o, para un socket, el de un antepasado.
bool EditorApp::rigPose(ecs::Entity entity, ecs::RenderSync::SkeletonPose& pose, bool search_up) {
    if (!sync_) return false;
    if (sync_->skeletonPose(world_, entity, pose)) return true;
    if (!search_up) return false;
    for (ecs::Entity a = entity.parent(); a.valid(); a = a.parent()) {
        if (sync_->skeletonPose(world_, a, pose)) return true;
    }
    return false;
}

void EditorApp::drawRigInspector(const std::string& type, ecs::Entity entity) {
    if (!sync_) return;
    ecs::RenderSync::SkeletonPose pose;

    if (type == "Skeleton") {
        ecs::Skeleton* sk = entity.tryGet<ecs::Skeleton>();
        if (sk == nullptr) return;
        if (!rigPose(entity, pose, false)) {
            ImGui::TextDisabled("Sin esqueleto: ponlo en un modelo animado o en su raiz.");
            return;
        }
        ImGui::SeparatorText("Huesos");
        if (ImGui::Button("Huesos como objetos", ImVec2(-1.0f, 0.0f))) createBoneObjects(entity);
        ImGui::SetItemTooltip("Un objeto por hueso en la Jerarquia: moverlo mueve esa parte del modelo\n"
                              "(una mano, un dedo) sin mover el resto.");
        ImGui::TextDisabled("%zu huesos. Clic para resaltar (tambien en la Escena).", pose.names.size());
        static char filter[64] = "";
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##bone_filter", "Buscar hueso...", filter, sizeof(filter));
        const std::string wanted = lowerText(filter);
        std::vector<int> depth(pose.names.size(), 0);
        for (std::size_t i = 0; i < pose.names.size(); ++i) {
            if (pose.parents[i] >= 0) depth[i] = depth[static_cast<std::size_t>(pose.parents[i])] + 1;
        }
        if (ImGui::BeginChild("bones", ImVec2(0.0f, 220.0f), ImGuiChildFlags_Borders)) {
            for (std::size_t i = 0; i < pose.names.size(); ++i) {
                const std::string& name = pose.names[i];
                if (!wanted.empty() && lowerText(name).find(wanted) == std::string::npos) continue;
                const bool moved = std::any_of(sk->bones.begin(), sk->bones.end(),
                                               [&](const ecs::BoneOverride& o) { return o.bone == name; });
                ImGui::PushID(static_cast<int>(i));
                if (wanted.empty()) ImGui::Indent(static_cast<float>(depth[i]) * 10.0f);
                if (moved) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.8f, 0.35f, 1.0f));
                if (ImGui::Selectable(name.c_str(), sk->selected == name)) {
                    sk->selected = name;
                    commit();
                }
                if (moved) ImGui::PopStyleColor();
                if (wanted.empty()) ImGui::Unindent(static_cast<float>(depth[i]) * 10.0f);
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        if (!sk->selected.empty()) {
            const bool moved = std::any_of(sk->bones.begin(), sk->bones.end(),
                                           [&](const ecs::BoneOverride& o) { return o.bone == sk->selected; });
            const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
            if (!moved && ImGui::Button("Mover este hueso", ImVec2(half, 0.0f))) {
                ecs::BoneOverride o;
                o.bone = sk->selected;
                sk->bones.push_back(o);
                commit();
            }
            if (moved && ImGui::Button("Dejar como la animacion", ImVec2(half, 0.0f))) {
                sk->bones.erase(std::remove_if(sk->bones.begin(), sk->bones.end(),
                                               [&](const ecs::BoneOverride& o) { return o.bone == sk->selected; }),
                                sk->bones.end());
                commit();
            }
            ImGui::SameLine();
            if (ImGui::Button("Socket aqui", ImVec2(half, 0.0f))) {
                ecs::Entity socket = world_.create(sk->selected + " (socket)", entity);
                socket.add<ecs::BoneSocket>().bone = sk->selected;
                selectOnly(socket.uuid());
                revealInHierarchy(socket.uuid());
                commit();
            }
            ImGui::SetItemTooltip("Crea una entidad enganchada a este hueso: pon dentro una espada, un sombrero o un collider.");
        }
        return;
    }

    if (type == "BoneSocket") {
        ecs::BoneSocket* socket = entity.tryGet<ecs::BoneSocket>();
        if (socket == nullptr) return;
        if (!rigPose(entity, pose, true)) {
            ImGui::TextDisabled("Pon esta entidad dentro de un modelo animado (o de su raiz).");
            return;
        }
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##socket_bone", socket->bone.empty() ? "Elegir hueso..." : socket->bone.c_str())) {
            for (const std::string& name : pose.names) {
                if (ImGui::Selectable(name.c_str(), name == socket->bone)) {
                    socket->bone = name;
                    commit();
                }
            }
            ImGui::EndCombo();
        }
        return;
    }

    if (type == "InverseKinematics") {
        ecs::InverseKinematics* ik = entity.tryGet<ecs::InverseKinematics>();
        if (ik == nullptr) return;
        float scale = 1.0f;
        const asset::ModelData* data = sync_->skeletonData(world_, entity, &scale);
        if (data == nullptr) return;
        if (ImGui::Button("Configurar automaticamente (animal o humano)", ImVec2(-1.0f, 0.0f))) {
            if (ecs::suggestCreatureIK(*data, *ik)) {
                commit();
            } else {
                std::cerr << "[IK] No se reconoce el esqueleto (sin patas ni cabeza)\n";
            }
        }
        ImGui::SetItemTooltip("Una cadena al suelo por cada pata (con 2 o 3 huesos segun la pata), la cabeza que mira\n"
                              "y su cuello. En humanos activa Pies en el suelo.");
        const creature::Rig rig = creature::detect(data->nodes, humanoid::restGlobals(data->nodes));
        if (rig.valid) {
            int front = 0;
            for (const creature::Leg& l : rig.legs) front += l.front ? 1 : 0;
            ImGui::TextDisabled("Detectado: %zu patas (%d delante), cabeza %s, cuello %zu, cola %zu", rig.legs.size(), front,
                                rig.head >= 0 ? data->nodes[static_cast<std::size_t>(rig.head)].name.c_str() : "-",
                                rig.neck.size(), rig.tail.size());
        }
        if (ImGui::Button("Crear objetivos (cadenas y mirar)", ImVec2(-1.0f, 0.0f))) {
            // Un objetivo en la punta de cada cadena sin objetivo (y uno para
            // mirar delante de la cabeza): moverlos con el gizmo mueve el hueso.
            const auto make_target = [&](const std::string& name, const Vec3& at) {
                ecs::Entity t = world_.create(name, entity);
                t.setWorldPosition(at);
                return t.uuid();
            };
            core::Mat4 m;
            for (ecs::IKChain& c : ik->chains) {
                if (c.ground || c.target.valid() || c.use_position) continue;
                if (sync_->boneWorld(world_, entity, c.bone, m)) {
                    c.target = make_target("IK " + c.bone, Vec3{m.m[3][0], m.m[3][1], m.m[3][2]});
                }
            }
            const std::string head = !ik->look_bone.empty() ? ik->look_bone
                                     : rig.head >= 0         ? data->nodes[static_cast<std::size_t>(rig.head)].name
                                                             : std::string();
            if (!ik->look_at.valid() && !head.empty() && sync_->boneWorld(world_, entity, head, m)) {
                const Vec3 forward = core::normalize(ecs::transformDirection(entity.worldMatrix(), rig.forward));
                ik->look_at = make_target("Mirar a", Vec3{m.m[3][0], m.m[3][1], m.m[3][2]} + forward * 2.0f);
            }
            commit();
        }
        return;
    }

    if (type == "Ragdoll") {
        ecs::Ragdoll* rag = entity.tryGet<ecs::Ragdoll>();
        if (rag == nullptr) return;
        float scale = 1.0f;
        const asset::ModelData* data = sync_->skeletonData(world_, entity, &scale);
        if (data == nullptr) {
            ImGui::TextDisabled("Ponlo en un modelo con esqueleto (o en su raiz).");
            return;
        }
        if (ImGui::Button("Generar huesos desde el esqueleto", ImVec2(-1.0f, 0.0f))) {
            rag->bones = ecs::suggestRagdollBones(*data, scale);
            commit();
        }
        ImGui::SetItemTooltip("Humanoides: cadera, columna, pecho, cabeza, brazos y piernas.\n"
                              "Animales: cuerpo, columna, cuello, cabeza, patas y cola.\nLuego ajusta radios y limites.");
        const ecs::RagdollRuntime* rt = rag->runtime.ptr.get();
        if (rt != nullptr && !rt->bones.empty()) {
            ImGui::TextDisabled("%zu cuerpos%s", rt->bones.size(), rt->simulating ? " (simulando)" : "");
        }
        if (playing()) {
            if (ImGui::Button(rag->active ? "Levantarse (volver a la animacion)" : "Caer ahora", ImVec2(-1.0f, 0.0f))) {
                rag->active = !rag->active;
            }
        } else {
            ImGui::TextDisabled("Pruebalo en Play: activalo aqui o con entity.ragdoll = true.");
        }
        return;
    }

    if (type == "PhysBones") {
        ecs::PhysBones* pb = entity.tryGet<ecs::PhysBones>();
        if (pb == nullptr) return;
        const asset::ModelData* data = sync_->skeletonData(world_, entity, nullptr);
        if (data == nullptr) {
            ImGui::TextDisabled("Ponlo en un modelo con esqueleto (o en su raiz).");
            return;
        }
        const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        if (ImGui::Button("Detectar pelo, colas, orejas...", ImVec2(half, 0.0f))) {
            const std::vector<ecs::PhysBoneChain> found = ecs::suggestPhysBones(*data);
            for (const ecs::PhysBoneChain& c : found) {
                if (std::none_of(pb->chains.begin(), pb->chains.end(),
                                 [&](const ecs::PhysBoneChain& o) { return o.bone == c.bone; })) {
                    pb->chains.push_back(c);
                }
            }
            if (found.empty()) std::cout << "[PhysBones] No hay huesos con nombre de pelo, cola, oreja, falda...\n";
            commit();
        }
        ImGui::SameLine();
        const ecs::Skeleton* sk = entity.tryGet<ecs::Skeleton>();
        ImGui::BeginDisabled(sk == nullptr || sk->selected.empty());
        if (ImGui::Button("Cadena en el hueso resaltado", ImVec2(half, 0.0f))) {
            ecs::PhysBoneChain c;
            c.bone = sk->selected;
            c.end_length = 0.5f;
            pb->chains.push_back(c);
            commit();
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Resalta un hueso en el componente Esqueleto (o con clic en la Escena).");
        // Collider en la cabeza (lo mas comun: que el pelo no la atraviese).
        const humanoid::Map map = humanoid::detect(data->nodes);
        const creature::Rig rig = creature::detect(data->nodes, humanoid::restGlobals(data->nodes));
        const int head = map.valid ? map[humanoid::Bone::Head] : rig.head;
        if (head >= 0 && ImGui::Button("Collider en la cabeza", ImVec2(-1.0f, 0.0f))) {
            ecs::Entity socket = world_.create("Collider cabeza", entity);
            socket.add<ecs::BoneSocket>().bone = data->nodes[static_cast<std::size_t>(head)].name;
            ecs::PhysBoneCollider& col = socket.add<ecs::PhysBoneCollider>();
            float scale = 1.0f;
            sync_->skeletonData(world_, entity, &scale);
            col.radius = std::max(rig.height * scale * 0.06f, 0.05f);
            col.offset = Vec3{0.0f, col.radius * 0.5f, 0.0f};
            selectOnly(socket.uuid());
            revealInHierarchy(socket.uuid());
            commit();
        }
        return;
    }
}

// --- Huesos como objetos ----------------------------------------------------------------

int EditorApp::createBoneObjects(ecs::Entity entity) {
    ecs::RenderSync::SkeletonPose pose;
    if (!sync_ || !rigPose(entity, pose, false) || !pose.source.valid()) {
        std::cerr << "[Esqueleto] " << entity.name() << " no tiene esqueleto\n";
        return 0;
    }
    ecs::Entity source = pose.source;
    // Los que ya van en un hueso de este modelo (no se repiten).
    std::unordered_map<std::string, ecs::Entity> existing;
    std::vector<ecs::Entity> stack{source};
    while (!stack.empty()) {
        const ecs::Entity e = stack.back();
        stack.pop_back();
        if (const ecs::BoneSocket* s = e.tryGet<ecs::BoneSocket>(); s != nullptr && s->bone == e.name()) {
            existing.emplace(s->bone, e);
        }
        for (const entt::entity child : e.children()) stack.push_back(world_.wrap(child));
    }

    // En el orden del esqueleto (los padres antes): cada hueso cuelga del
    // objeto de su hueso padre, asi mover la mano mueve tambien los dedos.
    std::vector<ecs::Entity> objects(pose.names.size());
    int created = 0;
    ecs::Entity first;
    for (std::size_t i = 0; i < pose.names.size(); ++i) {
        const std::string& bone = pose.names[i];
        if (const auto it = existing.find(bone); it != existing.end()) {
            objects[i] = it->second;
            continue;
        }
        core::Mat4 m;
        if (!sync_->boneWorld(world_, source, bone, m)) continue;
        const int parent_index = pose.parents[i];
        ecs::Entity parent = parent_index >= 0 ? objects[static_cast<std::size_t>(parent_index)] : ecs::Entity{};
        if (!parent.valid()) parent = source;
        ecs::Entity e = world_.create(bone, parent);
        // En el hueso, sin su escala (el socket no la usa).
        Vec3 position{};
        Quat rotation{};
        Vec3 scale{};
        ecs::decomposeMatrix(m, position, rotation, scale);
        e.setWorldMatrix(core::composeTrs(position, rotation, Vec3{1.0f, 1.0f, 1.0f}));
        // Siguen la animacion; el que se mueve con el gizmo pasa a mover su
        // hueso (drawGizmo).
        ecs::BoneSocket& socket = e.add<ecs::BoneSocket>();
        socket.bone = bone;
        socket.mode = ecs::SocketMode::Follow;
        socket.drive_position = true;
        objects[i] = e;
        if (!first.valid()) first = e;
        ++created;
    }
    if (created == 0) {
        std::cout << "[Esqueleto] " << source.name() << ": los huesos ya tienen objeto\n";
        return 0;
    }
    std::cout << "[Esqueleto] " << source.name() << ": " << created << " huesos como objetos\n";
    selectOnly(first.uuid());
    revealInHierarchy(first.uuid());
    commit();
    return created;
}

// --- Gizmos --------------------------------------------------------------------------

bool EditorApp::drawRigGizmos() {
    if (!sync_) return false;
    bool handle = false;
    const Vec3 cam_right = scene_.camera().right();
    const Vec3 cam_up = scene_.camera().up();
    const auto dot_at = [&](const Vec3& p, float r, std::uint32_t color) { overlayCircle(p, cam_right, cam_up, r, color, false, 12); };
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 mouse = ImGui::GetMousePos();

    // Esqueletos con "Ver huesos".
    for (const entt::entity h : world_.registry().view<ecs::Skeleton>()) {
        ecs::Entity e = world_.wrap(h);
        ecs::Skeleton& sk = e.get<ecs::Skeleton>();
        if (!sk.show_bones || !e.activeInHierarchy()) continue;
        ecs::RenderSync::SkeletonPose pose;
        if (!sync_->skeletonPose(world_, e, pose)) continue;
        float extent = 0.0f;
        for (std::size_t i = 1; i < pose.positions.size(); ++i) extent = std::max(extent, core::length(pose.positions[i] - pose.positions[0]));
        const float joint = std::max(extent * 0.012f, 0.004f) * sk.bone_size;
        int hovered = -1;
        float best = 8.0f;
        for (std::size_t i = 0; i < pose.positions.size(); ++i) {
            const bool selected = pose.names[i] == sk.selected;
            const std::uint32_t color = selected ? IM_COL32(255, 210, 60, 255) : IM_COL32(120, 200, 255, 230);
            if (pose.parents[i] >= 0) overlayLine(pose.positions[static_cast<std::size_t>(pose.parents[i])], pose.positions[i], color);
            dot_at(pose.positions[i], selected ? joint * 1.8f : joint, color);
            float x = 0.0f;
            float y = 0.0f;
            if (worldToScreen(pose.positions[i], x, y)) {
                const float d = std::hypot(x - mouse.x, y - mouse.y);
                if (view_hovered_ && d < best) {
                    best = d;
                    hovered = static_cast<int>(i);
                }
                if (sk.show_names || selected) {
                    draw->AddText(ImVec2(x + 6.0f, y - 7.0f), selected ? IM_COL32(255, 220, 90, 255) : IM_COL32(210, 225, 240, 200),
                                  pose.names[i].c_str());
                }
            }
        }
        // Clic en una articulacion: la resalta (y no selecciona otra cosa).
        if (hovered >= 0) {
            handle = true;
            float x = 0.0f;
            float y = 0.0f;
            worldToScreen(pose.positions[static_cast<std::size_t>(hovered)], x, y);
            ImGui::SetTooltip("%s", pose.names[static_cast<std::size_t>(hovered)].c_str());
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                sk.selected = pose.names[static_cast<std::size_t>(hovered)];
                selectOnly(e.uuid());
                commit();
            }
        }
    }

    for (const ecs::Entity e : selectedEntities()) {
        // Ragdoll: las capsulas (donde caen si esta simulando).
        if (const ecs::Ragdoll* rag = e.tryGet<ecs::Ragdoll>(); rag != nullptr && rag->runtime.ptr) {
            const ecs::RagdollRuntime& rt = *rag->runtime.ptr;
            for (const ecs::RagdollRuntime::Bone& b : rt.bones) {
                Vec3 origin{};
                Quat rotation{};
                Vec3 scale{};
                ecs::decomposeMatrix(b.world, origin, rotation, scale);
                Vec3 tip = b.tip;
                if (rt.simulating && rt.sim_ready) {
                    const Quat delta = ecs::quatMultiply(b.sim_rotation, ecs::quatConjugate(core::normalize(rotation)));
                    tip = b.sim_position + ecs::quatRotate(delta, b.tip - origin);
                    origin = b.sim_position;
                }
                const std::uint32_t color = rt.simulating ? IM_COL32(255, 120, 90, 230) : IM_COL32(255, 170, 90, 200);
                const Vec3 axis = tip - origin;
                const float len = core::length(axis);
                if (len < 1e-5f) continue;
                const Vec3 dir = axis * (1.0f / len);
                const Vec3 side = core::normalize(core::cross(dir, std::abs(dir.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0}));
                const Vec3 other = core::cross(dir, side);
                overlayCircle(origin, side, other, b.radius, color, false, 20);
                overlayCircle(tip, side, other, b.radius, color, false, 20);
                overlayLine(origin + side * b.radius, tip + side * b.radius, color);
                overlayLine(origin - side * b.radius, tip - side * b.radius, color);
                overlayLine(origin + other * b.radius, tip + other * b.radius, color);
                overlayLine(origin - other * b.radius, tip - other * b.radius, color);
            }
        }
        // Phys bones: las particulas y sus uniones.
        if (e.tryGet<ecs::PhysBones>() != nullptr) {
            if (const std::vector<physbone::Chain>* chains = sync_->physBoneChains(world_, e)) {
                for (const physbone::Chain& c : *chains) {
                    for (std::size_t i = 0; i < c.position.size(); ++i) {
                        dot_at(c.position[i], 0.012f, IM_COL32(120, 255, 150, 230));
                        if (c.parent[i] >= 0) overlayLine(c.position[static_cast<std::size_t>(c.parent[i])], c.position[i], IM_COL32(120, 255, 150, 200));
                    }
                }
            }
        }
        // IK: objetivos de las cadenas y de mirar.
        if (const ecs::InverseKinematics* ik = e.tryGet<ecs::InverseKinematics>()) {
            core::Mat4 m;
            for (const ecs::IKChain& c : ik->chains) {
                Vec3 goal{};
                bool has_goal = false;
                if (c.use_position) {
                    goal = c.position;
                    has_goal = true;
                } else if (const ecs::Entity t = world_.find(c.target); t.valid()) {
                    goal = t.worldPosition();
                    has_goal = true;
                }
                if (sync_->boneWorld(world_, e, c.bone, m)) {
                    const Vec3 end{m.m[3][0], m.m[3][1], m.m[3][2]};
                    dot_at(end, 0.02f, c.ground ? IM_COL32(200, 150, 255, 230) : IM_COL32(255, 120, 200, 230));
                    if (has_goal) overlayLine(end, goal, IM_COL32(255, 120, 200, 160));
                }
            }
        }
    }

    // Colliders de phys bones (siempre: son pequenos y ayudan a colocarlos).
    for (const entt::entity h : world_.registry().view<ecs::PhysBoneCollider>()) {
        const ecs::Entity e = world_.wrap(h);
        if (!e.activeInHierarchy()) continue;
        const ecs::PhysBoneCollider& c = e.get<ecs::PhysBoneCollider>();
        const core::Mat4& m = e.worldMatrix();
        const float r = c.radius * ecs::maxAxisScale(m);
        const std::uint32_t color = IM_COL32(90, 230, 255, 200);
        const Vec3 center = ecs::transformPoint(m, c.offset);
        const Vec3 up = core::normalize(ecs::transformDirection(m, Vec3{0, 1, 0}));
        const Vec3 side = core::normalize(ecs::transformDirection(m, Vec3{1, 0, 0}));
        const Vec3 front = core::normalize(ecs::transformDirection(m, Vec3{0, 0, 1}));
        switch (c.shape) {
            case ecs::PhysBoneColliderShape::Sphere:
                overlayCircle(center, side, up, r, color);
                overlayCircle(center, side, front, r, color);
                overlayCircle(center, up, front, r, color);
                break;
            case ecs::PhysBoneColliderShape::Capsule: {
                const float half = std::max(c.height * 0.5f * ecs::maxAxisScale(m) - r, 0.0f);
                const Vec3 a = center + up * half;
                const Vec3 b = center - up * half;
                overlayCircle(a, side, front, r, color);
                overlayCircle(b, side, front, r, color);
                overlayCircle(a, side, up, r, color, false, 32);
                overlayCircle(b, side, up, r, color, false, 32);
                overlayLine(a + side * r, b + side * r, color);
                overlayLine(a - side * r, b - side * r, color);
                overlayLine(a + front * r, b + front * r, color);
                overlayLine(a - front * r, b - front * r, color);
                break;
            }
            case ecs::PhysBoneColliderShape::Plane:
                for (int i = -2; i <= 2; ++i) {
                    const float t = static_cast<float>(i) * 0.25f;
                    overlayLine(center + side * t - front * 0.5f, center + side * t + front * 0.5f, color);
                    overlayLine(center + front * t - side * 0.5f, center + front * t + side * 0.5f, color);
                }
                overlayLine(center, center + up * 0.25f, color);
                break;
        }
    }
    return handle;
}

}  // namespace cramion::editor

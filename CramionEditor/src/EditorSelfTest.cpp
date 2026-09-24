// Prueba automatica del editor de extremo a extremo (--selftest). Cada paso
// se ejecuta en un frame distinto (con esperas) para que RenderSync suba los
// modelos y el renderizador dibuje de verdad entre medias: asi se prueba lo
// mismo que hace un usuario, con la capa de validacion de Vulkan mirando.

#include "EditorApp.h"

#include "Dialogs.h"

#include <cmath>
#include <iostream>

namespace cramion::editor {

using core::Vec3;

namespace {

void check(bool ok, const char* what, int& failures) {
    std::cout << "[SelfTest] " << (ok ? "OK    " : "FALLO ") << what << std::endl;
    if (!ok) ++failures;
}

}  // namespace

void EditorApp::startSelfTest(const std::filesystem::path& folder, const std::filesystem::path& model,
                              const std::filesystem::path& environment, const std::filesystem::path& image) {
    self_test_image_ = image;
    self_test_folder_ = folder;
    self_test_model_ = model;
    self_test_environment_ = environment;
    self_test_step_ = 1;
    self_test_wait_ = 0;
    self_test_failures_ = 0;
}

void EditorApp::runSelfTestStep() {
    if (self_test_step_ <= 0) return;
    if (self_test_wait_ > 0) {
        --self_test_wait_;
        return;
    }
    int& f = self_test_failures_;
    switch (self_test_step_) {
        case 1: {  // Proyecto nuevo (como el Hub) y abrirlo.
            std::error_code error;
            const std::filesystem::path root = self_test_folder_ / "SelfTest";
            std::filesystem::remove_all(root, error);
            std::filesystem::create_directories(self_test_folder_, error);
            project::ProjectInfo info = project::createProject(self_test_folder_, "SelfTest");
            ecs::World world;
            world.setSceneName("Main");
            ecs::populateDefaultScene(world);
            const std::filesystem::path scene = info.assetsFolder() / "Scenes" / "Main.crscene";
            std::filesystem::create_directories(scene.parent_path(), error);
            ecs::saveScene(world, scene);
            info.startup_scene = world.sceneUuid();
            project::saveProject(info);
            check(openProject(info.file), "crear y abrir proyecto", f);
            check(world_.entityCount() >= 3, "escena inicial con camara, luz y entorno", f);
            // La prueba no debe quedar en los recientes del usuario.
            project::removeRecentProject(info.file);
            self_test_wait_ = 5;
            break;
        }
        case 2: {  // Importar (sincrono en la prueba).
            const assets::ImportResult model = assets::importAny(self_test_model_, project_.assetsFolder());
            check(model.ok, "importar modelo a .crdata", f);
            const assets::ImportResult sky = assets::importAny(self_test_environment_, project_.assetsFolder());
            check(sky.ok, "importar cielo HDR a .crdata", f);
            refreshDatabase();
            check(database_->find(model.info.uuid).has_value(), "modelo en la base de datos", f);
            // Instanciar como al soltarlo en la escena y asignar el cielo.
            const ecs::Entity car = instantiateAsset(model.info.uuid, {}, Vec3{0.0f, 0.0f, 0.0f});
            check(car.valid() && (car.childCount() > 0 || car.has<ecs::MeshRenderer>()), "instanciar el modelo", f);
            self_test_car_ = car.uuid();
            assignEnvironment(sky.info.uuid);
            selectOnly(self_test_car_);
            self_test_wait_ = 90;  // subir a la GPU y dibujar
            break;
        }
        case 3: {  // Seleccion, outline y foco.
            ecs::Entity car = world_.find(self_test_car_);
            selectOnly(self_test_car_);
            focusSelection();
            check(!sync_->actorIndicesInSubtree(car).empty(), "actores del coche en el render", f);
            check(!renderer_.outlinedActors().empty(), "contorno de la seleccion", f);
            gizmo_ = GizmoOperation::Translate;
            self_test_wait_ = 30;
            break;
        }
        case 4: {  // Mover, deshacer, rehacer.
            ecs::Entity car = world_.find(self_test_car_);
            const Vec3 start = car.worldPosition();
            car.setWorldPosition(start + Vec3{2.0f, 0.0f, 0.0f});
            commit();
            undo();
            car = world_.find(self_test_car_);
            check(car.valid() && std::abs(car.worldPosition().x - start.x) < 1e-4f, "deshacer movimiento", f);
            redo();
            car = world_.find(self_test_car_);
            check(car.valid() && std::abs(car.worldPosition().x - (start.x + 2.0f)) < 1e-4f,
                  "rehacer movimiento", f);
            // Duplicar y emparentar.
            selectOnly(self_test_car_);
            duplicateSelection();
            ecs::Entity copy = world_.find(active_);
            check(copy.valid() && !(copy == car), "duplicar", f);
            check(copy.setParent(car, true, -1), "emparentar la copia bajo el original", f);
            commit();
            self_test_wait_ = 30;
            break;
        }
        case 5: {  // Guardar y reabrir.
            scene_path_ = project_.assetsFolder() / "Scenes" / "Main.crscene";
            self_test_entities_ = world_.entityCount();
            check(saveScene(), "guardar escena", f);
            check(openScene(scene_path_), "reabrir escena", f);
            check(world_.entityCount() == self_test_entities_, "mismas entidades tras reabrir", f);
            check(world_.find(self_test_car_).valid(), "el coche conserva su UUID", f);
            // Suelo y camara fija para ver el resultado.
            {
                ecs::Entity ground = createEntity(3, {});
                ground.setWorldPosition(Vec3{0.0f, 0.0f, 0.0f});
                ground.setLocalScale(Vec3{3.0f, 1.0f, 3.0f});
            }
            selectOnly(self_test_car_);
            scene_.placeCamera(Vec3{6.5f, 2.6f, 7.5f}, Vec3{0.0f, 0.6f, 0.0f});
            self_test_wait_ = 90;
            break;
        }
        case 6: {  // Estado final que se ve en la vista: seleccion, contorno y gizmo.
            check(isSelected(self_test_car_), "el coche sigue seleccionado", f);
            check(!renderer_.outlinedActors().empty(), "contorno activo al final", f);
            check(world_.find(active_).valid() && gizmo_ == GizmoOperation::Translate,
                  "gizmo de mover sobre la seleccion", f);
            break;
        }
        case 7: {  // Animator: crear con los clips del modelo, asignar y probar.
            const ecs::Entity car = world_.find(self_test_car_);
            const ecs::Entity animated = animatedEntityIn(car);
            if (!animated.valid()) {
                std::cout << "[SelfTest] (modelo sin animaciones: se salta el Animator)" << std::endl;
                self_test_step_ = 11;
                return;
            }
            selectOnly(self_test_car_);
            createAnimatorAsset(project_.assetsFolder());
            const ecs::Animator* animator = animated.tryGet<ecs::Animator>();
            check(animator != nullptr && animator->controller.valid() && animator->controller.uuid == animator_uuid_,
                  "Animator creado y asignado", f);
            check(!animator_.states.empty(), "un estado por clip del modelo", f);
            // Trigger "Go": estado 0 -> un estado de reposo nuevo.
            ecs::AnimatorParameter go;
            go.name = "Go";
            go.type = ecs::AnimatorParameterType::Trigger;
            animator_.parameters.push_back(go);
            ecs::AnimatorState rest;
            rest.name = "Reposo";
            rest.position = core::Vec2{240.0f, 0.0f};
            animator_.states.push_back(rest);
            ecs::AnimatorTransition t;
            t.from = 0;
            t.to = static_cast<int>(animator_.states.size()) - 1;
            t.conditions.push_back({"Go", ecs::AnimatorConditionMode::If, 0.0f});
            animator_.transitions.push_back(t);
            saveAnimatorEditor();
            self_test_wait_ = 20;
            break;
        }
        case 8: {
            const ecs::Entity animated = animatedEntityIn(world_.find(self_test_car_));
            ecs::Animator* animator = animated.valid() ? animated.tryGet<ecs::Animator>() : nullptr;
            check(animator != nullptr && animator->runtime.state == 0, "la maquina entra en el estado por defecto", f);
            check(animator != nullptr && animator->time > 0.0f, "el clip del estado suena", f);
            if (animator != nullptr) animator->setTrigger("Go");
            self_test_wait_ = 5;
            break;
        }
        case 9: {
            const ecs::Entity animated = animatedEntityIn(world_.find(self_test_car_));
            ecs::Animator* animator = animated.valid() ? animated.tryGet<ecs::Animator>() : nullptr;
            check(animator != nullptr && animator->runtime.state == static_cast<int>(animator_.states.size()) - 1,
                  "el trigger dispara la transicion", f);
            check(animator != nullptr && ecs::animatorParameterValue(animator_, animator->runtime, "Go") == 0.0f,
                  "el trigger se consume", f);
            // Extraer el primer clip (sin dialogo en la prueba) y usarlo como .cranim.
            const asset::ModelData* data = sync_->actorModelData(animated, scene_);
            const std::filesystem::path folder = project_.assetsFolder() / "Clips";
            std::filesystem::create_directories(folder);
            const std::filesystem::path clip_path = folder / "Extraido.cranim";
            check(data != nullptr && ecs::saveAnimationClip(*data, data->animations[0], clip_path),
                  "extraer clip a .cranim", f);
            refreshDatabase();
            Uuid clip_uuid{};
            for (const assets::AssetInfo& info : database_->all()) {
                if (info.type == assets::AssetType::AnimationClip) clip_uuid = info.uuid;
            }
            check(clip_uuid.valid(), "el .cranim entra en la base de datos", f);
            asset::AnimationClip loaded;
            check(data != nullptr && ecs::loadAnimationClip(clip_path, *data, loaded) &&
                      loaded.channels.size() == data->animations[0].channels.size(),
                  "el .cranim se relee con todas sus pistas", f);
            // Estado de reposo -> usa el clip extraido.
            animator_.states.back().clip = assets::AssetRef{clip_uuid, assets::AssetType::AnimationClip};
            saveAnimatorEditor();
            if (animator != nullptr) animator->time = 0.0f;
            self_test_wait_ = 20;
            break;
        }
        case 10: {
            const ecs::Entity animated = animatedEntityIn(world_.find(self_test_car_));
            const ecs::Animator* animator = animated.valid() ? animated.tryGet<ecs::Animator>() : nullptr;
            const asset::ModelData* data = sync_->actorModelData(animated, scene_);
            check(animator != nullptr && animator->time > 0.0f && data != nullptr && data->animations.size() >= 2,
                  "el estado reproduce el clip .cranim", f);
            show_animator_ = true;
            animator_focus_ = true;
            self_test_wait_ = 30;
            break;
        }
        case 11: {  // Decals: estampa con imagen, charco y humedad sobre el suelo.
            Vec3 point{};
            Vec3 normal{};
            check(surfaceHit(view_x_ + view_w_ * 0.5f, view_y_ + view_h_ * 0.75f, point, normal),
                  "la herramienta de estampar encuentra la superficie", f);
            std::string texture;
            if (!self_test_image_.empty()) {
                texture = decalImageInAssets(self_test_image_);
                check(!texture.empty() && std::filesystem::exists(project_.assetsFolder() / dialogs::fromUtf8(texture)),
                      "imagen copiada a Assets/Textures", f);
            }
            stamp_brush_ = StampBrush{};
            stamp_brush_.size = 1.6f;
            const ecs::Entity stamp = stampAt(Vec3{1.6f, 0.0f, 1.8f}, Vec3{0.0f, 1.0f, 0.0f}, texture);
            stamp_brush_.type = 1;
            stamp_brush_.size = 3.2f;
            stampAt(Vec3{-1.8f, 0.0f, 1.2f}, Vec3{0.0f, 1.0f, 0.0f}, {});
            stamp_brush_.type = 2;
            stamp_brush_.size = 2.2f;
            stampAt(Vec3{0.8f, 0.0f, -1.6f}, Vec3{0.0f, 1.0f, 0.0f}, {});
            stamp_brush_ = StampBrush{};
            // Foco para ver su gizmo (cono y asas).
            ecs::Entity spot = createEntity(8, {});
            spot.setWorldPosition(Vec3{-2.5f, 3.0f, -1.0f});
            selectOnly(spot.uuid());
            commit();
            check(stamp.valid() && stamp.has<ecs::Decal>(), "estampa creada", f);
            self_test_wait_ = 20;
            break;
        }
        case 12: {
            const auto& decals = renderer_.decals();
            check(decals.size() == 3, "los 3 decals llegan al renderizador", f);
            if (!self_test_image_.empty()) {
                bool textured = false;
                for (const auto& d : decals) textured = textured || (d.type == 0 && d.texture >= 0);
                check(textured, "la estampa tiene su textura cargada", f);
            }
            // Vista desde arriba para la captura.
            show_animator_ = false;
            scene_.placeCamera(Vec3{0.0f, 7.5f, 7.0f}, Vec3{0.0f, 0.0f, 0.5f});
            self_test_wait_ = 60;
            break;
        }
        default:
            std::cout << "[SelfTest] " << (self_test_failures_ == 0 ? "TODO OK" : "HAY FALLOS: ")
                      << (self_test_failures_ == 0 ? std::string() : std::to_string(self_test_failures_))
                      << std::endl;
            self_test_step_ = -1;
            return;
    }
    ++self_test_step_;
}

}  // namespace cramion::editor

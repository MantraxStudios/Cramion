// Prueba automatica del editor de extremo a extremo (--selftest). Cada paso
// se ejecuta en un frame distinto (con esperas) para que RenderSync suba los
// modelos y el renderizador dibuje de verdad entre medias: asi se prueba lo
// mismo que hace un usuario, con la capa de validacion de Vulkan mirando.

#include "EditorApp.h"

#include "Dialogs.h"

#include <CramionFX/asset/ImageFile.h>
#include <imgui.h>

#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>

namespace cramion::editor {

using core::Vec3;

namespace {

constexpr int kStressObjects = 300;

void check(bool ok, const char* what, int& failures) {
    std::cout << "[SelfTest] " << (ok ? "OK    " : "FALLO ") << what << std::endl;
    if (!ok) ++failures;
}

// La ventana tal como se ve (PrintWindow con el contenido de Vulkan), en
// RGBA8 y guardada como PNG para mirarla. Vacio si no se pudo.
std::vector<std::uint8_t> captureWindow(HWND hwnd, const std::filesystem::path& png) {
    RECT client{};
    GetClientRect(hwnd, &client);
    const int w = client.right - client.left, h = client.bottom - client.top;
    if (w <= 0 || h <= 0) return {};
    HDC window_dc = GetDC(hwnd);
    HDC memory_dc = CreateCompatibleDC(window_dc);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = -h;  // de arriba abajo
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(window_dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(memory_dc, bitmap);
    const bool ok = PrintWindow(hwnd, memory_dc, PW_CLIENTONLY | 0x2 /* PW_RENDERFULLCONTENT */) != FALSE;
    asset::ImageRgba8 image;
    if (ok && bits != nullptr) {
        image.width = static_cast<std::uint32_t>(w);
        image.height = static_cast<std::uint32_t>(h);
        image.pixels.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
        const auto* bgra = static_cast<const std::uint8_t*>(bits);
        for (std::size_t i = 0; i < image.pixels.size(); i += 4) {
            image.pixels[i] = bgra[i + 2];
            image.pixels[i + 1] = bgra[i + 1];
            image.pixels[i + 2] = bgra[i];
            image.pixels[i + 3] = 255;
        }
    }
    SelectObject(memory_dc, old);
    DeleteObject(bitmap);
    DeleteDC(memory_dc);
    ReleaseDC(hwnd, window_dc);
    if (!png.empty() && !image.pixels.empty()) asset::saveImagePng(png, image);
    return std::move(image.pixels);
}

// Diferencia media por canal (0..255) entre dos capturas del mismo tamano.
float captureDifference(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    if (a.empty() || a.size() != b.size()) return -1.0f;
    double sum = 0.0;
    for (std::size_t i = 0; i < a.size(); i += 4) {
        for (int c = 0; c < 3; ++c) sum += std::abs(static_cast<int>(a[i + c]) - static_cast<int>(b[i + c]));
    }
    return static_cast<float>(sum / (static_cast<double>(a.size() / 4) * 3.0));
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
    focus_scene_ = true;  // la prueba trabaja en la Escena (aunque el .ini dejara el Juego delante)
    self_test_failures_ = 0;
}

void EditorApp::printCpuTimings(const char* when) {
    static constexpr const char* kNames[kCpuSectionCount] = {"interfaz", "jerarquia", "inspector", "escena",
                                                             "paneles", "fisica", "sync", "render"};
    std::cout << "[SelfTest] CPU (" << when << "):";
    for (int i = 0; i < kCpuSectionCount; ++i) {
        std::cout << " " << kNames[i] << " " << cpu_ms_[static_cast<std::size_t>(i)] << " ms";
    }
    std::cout << "  | PhysicsSystem " << physics_.stats().step_milliseconds << " ms | " << ImGui::GetIO().Framerate
              << " FPS" << std::endl;
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
            focus_scene_ = true;  // ya con el acoplado creado
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
            // CRAMION_SELFTEST_FROM=N: con el proyecto ya abierto, salta al paso N
            // (para repetir solo una parte de la prueba).
            if (const char* from = std::getenv("CRAMION_SELFTEST_FROM"); from != nullptr && std::atoi(from) > 3) {
                self_test_step_ = std::atoi(from);
                std::cout << "[SelfTest] Saltando al paso " << self_test_step_ << std::endl;
                return;
            }
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
        case 13: {  // Fisica: suelo (MeshCollider del plano), caja, esfera, trigger y particulas.
            physics_settings_.layer_names[8] = "Jugador";
            applyPhysicsSettings();
            savePhysicsSettings();
            ecs::Entity cube = createEntity(13, {});
            cube.setWorldPosition(Vec3{2.0f, 4.0f, 1.5f});
            cube.setLocalEulerDegrees(Vec3{20.0f, 30.0f, 10.0f});
            cube.get<ecs::EntityInfo>().layer = 8;
            self_test_cube_ = cube.uuid();
            ecs::Entity ball = createEntity(14, {});
            ball.setWorldPosition(Vec3{-2.2f, 6.0f, 1.5f});
            ball.setLocalScale(Vec3{0.6f, 0.6f, 0.6f});
            ball.get<physics::Rigidbody>().initial_velocity = Vec3{0.0f, 0.0f, -0.5f};
            ecs::Entity zone = createEntity(15, {});
            zone.setWorldPosition(Vec3{-2.2f, 1.6f, 1.2f});
            self_test_zone_ = zone.uuid();
            ecs::Entity sparks = createEntity(16, {});
            sparks.setWorldPosition(Vec3{0.3f, 2.8f, -1.6f});
            physics::ParticleSystem& ps = sparks.get<physics::ParticleSystem>();
            ps.rate = 220.0f;
            ps.cone_angle = 35.0f;
            ps.speed_min = 2.0f;
            ps.speed_max = 4.0f;
            ps.bounce = 0.45f;
            check(cube.has<physics::BoxCollider>() && ball.has<physics::SphereCollider>(),
                  "las primitivas nuevas llevan su collider (como Unity)", f);
            const ecs::Entity ground = world_.findByName("Plano");
            check(ground.valid() && ground.has<physics::MeshCollider>(), "el plano tiene Mesh Collider", f);
            commit();
            enterPlay();
            check(playing(), "entrar en Play", f);
            self_test_wait_ = 180;
            break;
        }
        case 14: {
            const ecs::Entity cube = world_.find(self_test_cube_);
            const auto count = [&](physics::PhysicsEventType t) { return event_counts_[static_cast<std::size_t>(t)]; };
            check(cube.valid() && std::abs(cube.worldPosition().y - 0.5f) < 0.08f,
                  "la caja cae y reposa sobre el Mesh Collider del suelo", f);
            check(count(physics::PhysicsEventType::CollisionEnter) > 0 && count(physics::PhysicsEventType::CollisionStay) > 0,
                  "eventos CollisionEnter / CollisionStay", f);
            check(count(physics::PhysicsEventType::TriggerEnter) > 0, "la esfera dispara TriggerEnter en la zona", f);
            check(count(physics::PhysicsEventType::ParticleCollision) > 0 && particles_.particleCount() > 0,
                  "las particulas chocan con el suelo (ParticleCollision)", f);
            check(!renderer_.particles().empty(), "las particulas llegan al renderizador", f);
            // Raycast desde arriba con y sin la capa de la caja.
            physics::RaycastHit hit;
            physics::QueryFilter with_player;
            with_player.layer_mask = physics::kDefaultRaycastLayers;
            with_player.triggers = physics::QueryTriggers::Ignore;
            const Vec3 above = cube.worldPosition() + Vec3{0.0f, 5.0f, 0.0f};
            check(physics_.raycast(above, Vec3{0.0f, -1.0f, 0.0f}, 20.0f, hit, with_player) && hit.entity == cube,
                  "raycast toca la caja (capa Jugador en la mascara)", f);
            physics::QueryFilter without_player = with_player;
            without_player.layer_mask &= ~physics::layerBit(8);
            check(physics_.raycast(above, Vec3{0.0f, -1.0f, 0.0f}, 20.0f, hit, without_player) && !(hit.entity == cube) &&
                      std::abs(hit.point.y) < 0.02f,
                  "sin la capa Jugador el rayo la atraviesa y toca el suelo", f);
            exitPlay();
            const ecs::Entity restored = world_.find(self_test_cube_);
            check(!playing() && restored.valid() && std::abs(restored.worldPosition().y - 4.0f) < 1e-3f,
                  "al parar se restaura la escena", f);
            self_test_wait_ = 10;
            break;
        }
        case 15: {  // Estado final en Play para la captura: gizmos, particulas y rayo.
            enterPlay();
            focus_game_ = false;  // (al dar Play se ve el Juego; aqui interesa la Escena)
            focus_scene_ = true;
            selectOnly(self_test_cube_);
            gizmo_all_colliders_ = true;
            raycast_ = RaycastTester{};
            raycast_.enabled = true;
            raycast_.origin = 2;
            raycast_.query = 0;
            // Rayo hacia abajo sobre donde cae la caja (toca la caja o el suelo).
            raycast_.manual_origin = Vec3{2.0f, 6.0f, 1.5f};
            raycast_.manual_direction = Vec3{0.0f, -1.0f, 0.0f};
            scene_.placeCamera(Vec3{0.5f, 4.5f, 9.0f}, Vec3{0.0f, 0.8f, 0.0f});
            self_test_wait_ = 120;
            break;
        }
        case 16: {
            check(playing() && !raycast_.hits.empty(), "el probador de raycast toca algo en Play", f);
            // Picking por GPU: clic donde se ve el cubo.
            clearSelection();
            raycast_.enabled = false;
            float sx = 0.0f;
            float sy = 0.0f;
            const ecs::Entity cube = world_.find(self_test_cube_);
            check(cube.valid() && worldToScreen(cube.worldPosition(), sx, sy), "el cubo se ve en la vista", f);
            pickAt(sx, sy);
            self_test_wait_ = 8;  // la GPU tarda unos frames en devolverlo
            break;
        }
        case 17: {
            check(isSelected(self_test_cube_), "picking por GPU: el clic selecciona el cubo", f);
            // Estres: muchos objetos con fisica creados como lo haria el usuario.
            exitPlay();
            raycast_.enabled = false;
            gizmo_all_colliders_ = false;
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < kStressObjects; ++i) {
                ecs::Entity e = createEntity(13, {});
                e.setWorldPosition(Vec3{static_cast<float>(i % 10) * 1.2f - 6.0f, 1.0f + static_cast<float>(i / 100) * 1.2f,
                                        static_cast<float>((i / 10) % 10) * 1.2f - 6.0f});
            }
            const float total = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
            std::cout << "[SelfTest] Estres: " << kStressObjects << " objetos creados en " << total << " ms ("
                      << total / kStressObjects << " ms cada uno), " << world_.entityCount() << " entidades" << std::endl;
            commit();
            self_test_wait_ = 120;
            break;
        }
        case 18:
            printCpuTimings("edicion, con la seleccion");
            enterPlay();
            self_test_wait_ = 240;
            break;
        case 19:
            printCpuTimings("Play");
            check(physics_.stats().bodies >= static_cast<std::uint32_t>(kStressObjects), "todos los cuerpos en la fisica", f);
            break;
        case 20: {  // Cinematicas: camara que sigue al cubo, camara en riel y una secuencia.
            exitPlay();
            selectOnly(self_test_cube_);
            scene_.placeCamera(Vec3{-6.0f, 4.0f, 9.0f}, Vec3{0.0f, 1.0f, 0.0f});
            const ecs::Entity follow_cam = createCinematic(1);
            selectOnly(self_test_cube_);
            const ecs::Entity dolly_cam = createCinematic(3);
            const ecs::Entity sequence = createCinematic(5);
            check(follow_cam.valid() && follow_cam.get<cinema::VirtualCamera>().follow == self_test_cube_,
                  "camara virtual que sigue a la seleccion", f);
            check(dolly_cam.valid() && dolly_cam.get<cinema::VirtualCamera>().body == cinema::BodyMode::TrackedDolly &&
                      world_.find(dolly_cam.get<cinema::VirtualCamera>().track).valid(),
                  "camara en riel con su riel", f);
            check(ensureCameraBrain().has<cinema::CameraBrain>(), "la camara real tiene Camera Brain", f);
            cinema::CinematicSequence& seq = sequence.get<cinema::CinematicSequence>();
            // Solo las dos camaras nuevas, 2 s cada una con mezcla de 1 s.
            seq.shots.clear();
            seq.shots.push_back(cinema::CinematicShot{follow_cam.uuid(), 0.0f, 2.0f, 0.0f, cinema::BlendCurve::Cut});
            seq.shots.push_back(cinema::CinematicShot{dolly_cam.uuid(), 2.0f, 30.0f, 1.0f, cinema::BlendCurve::EaseInOut});
            self_test_sequence_ = sequence.uuid();
            commit();
            show_game_ = true;
            enterPlay();
            self_test_wait_ = 60;
            break;
        }
        case 21: {
            const ecs::Entity sequence = world_.find(self_test_sequence_);
            const ecs::Entity live = cinematics_.liveCamera();
            check(sequence.valid() && cinematics_.isPlaying(sequence), "la secuencia suena al dar Play", f);
            check(live.valid() && live.name() == "Camara de seguimiento", "primer plano: la camara de seguimiento", f);
            check(render_view_ == kGameSlot && game_view_visible_, "en Play se dibuja la vista Juego", f);
            const ecs::Entity brain = cinematics_.brain();
            check(brain.valid() && live.valid() && core::length(brain.worldPosition() - live.worldPosition()) < 0.05f,
                  "la camara real esta donde la virtual activa", f);
            self_test_wait_ = 150;  // pasa al segundo plano y mezcla
            break;
        }
        case 22: {
            // Esperar por tiempo de la cinematica (a muchos FPS 150 frames no llegan al 2.o plano).
            if (const ecs::Entity seq = world_.find(self_test_sequence_);
                seq.valid() && cinematics_.isPlaying(seq) && cinematics_.time(seq) < 3.5f) {
                self_test_wait_ = 5;
                return;
            }
            const ecs::Entity live = cinematics_.liveCamera();
            check(live.valid() && live.name() == "Camara en riel", "segundo plano: la camara en riel", f);
            // Vista final: el riel en la Escena en modo Bezier (asas y puntos),
            // la ventana Cinematica y las miniaturas de Assets/Textures.
            exitPlay();
            if (ecs::Entity track = world_.findByName("Riel"); track.valid()) {
                cinema::DollyTrack& t = track.get<cinema::DollyTrack>();
                t.mode = cinema::PathMode::Bezier;
                t.waypoints[1].tangent = Vec3{2.5f, 1.5f, 0.0f};
                selectOnly(track.uuid());
                waypoint_track_ = track.uuid();
                selected_waypoint_ = 1;
                scene_.placeCamera(track.worldPosition() + Vec3{0.0f, 6.0f, 9.0f}, track.worldPosition());
            }
            current_folder_ = project_.assetsFolder() / "Textures";
            focus_scene_ = true;
            show_cinematic_ = true;
            self_test_wait_ = 90;
            break;
        }
        case 23: {
            // Pedir las miniaturas (se decodifican en otro hilo) y esperar.
            std::error_code error;
            for (const auto& entry : std::filesystem::directory_iterator(project_.assetsFolder() / "Textures", error)) {
                if (isDecalImage(entry.path())) imgui_.thumbnail(entry.path());
            }
            self_test_wait_ = 30;
            break;
        }
        case 24: {
            // Miniaturas del navegador: la imagen de Assets/Textures se ve de verdad.
            bool any_image = false;
            bool loaded = false;
            std::error_code error;
            for (const auto& entry : std::filesystem::directory_iterator(project_.assetsFolder() / "Textures", error)) {
                if (!isDecalImage(entry.path())) continue;
                any_image = true;
                ImVec2 size{};
                loaded = loaded || (imgui_.thumbnail(entry.path(), &size) != 0 && size.x > 0.0f);
            }
            check(!any_image || loaded, "miniatura de la textura en el navegador de assets", f);
            check(imgui_.icon(Icon::FolderClosed) != 0 && imgui_.icon(Icon::Move) != 0, "iconos del editor cargados", f);
            const ecs::Entity track = world_.findByName("Riel");
            check(track.valid() && track.get<cinema::DollyTrack>().mode == cinema::PathMode::Bezier,
                  "riel en modo Bezier listo para editar", f);
            break;
        }
        case 25: {
            // Soltar una carpeta del Explorador: se copia con su estructura.
            std::error_code error;
            const std::filesystem::path source = self_test_folder_ / "CarpetaSoltada";
            std::filesystem::remove_all(source, error);
            std::filesystem::create_directories(source / "Texturas", error);
            if (!self_test_image_.empty()) {
                std::filesystem::copy_file(self_test_image_, source / "Texturas" / self_test_image_.filename(), error);
            }
            {
                std::ofstream obj(source / "Rampa.obj");
                obj << "v 0 0 0\nv 1 0 0\nv 1 1 1\nv 0 1 1\nvn 0 0.7 -0.7\nf 1//1 2//1 3//1\nf 1//1 3//1 4//1\n";
            }
            current_folder_ = project_.assetsFolder();
            onFilesDropped({source});
            self_test_wait_ = 60;  // el .obj se importa en otro hilo
            break;
        }
        case 26: {
            if (!imports_.empty()) {  // aun importando
                self_test_wait_ = 10;
                return;
            }
            const std::filesystem::path dropped = project_.assetsFolder() / "CarpetaSoltada";
            check(std::filesystem::is_directory(dropped / "Texturas"), "soltar una carpeta crea su estructura en Assets", f);
            check(self_test_image_.empty() || std::filesystem::exists(dropped / "Texturas" / self_test_image_.filename()),
                  "y copia sus archivos", f);
            bool model = false;
            for (const assets::AssetInfo& info : database_->inFolder(dropped)) {
                model = model || info.type == assets::AssetType::Model;
            }
            check(model, "y los modelos de dentro se importan", f);
            break;
        }
        case 27: {
            // Terreno: crear, generar relieve, esculpir una loma y pintar.
            ecs::Entity ground = world_.findByName("Plano");
            if (ground.valid()) ground.setActive(false);  // que no tape el terreno
            const ecs::Entity t = createTerrainEntity();
            check(t.valid() && t.has<terrain::Terrain>(), "crear un terreno", f);
            terrain::Terrain& comp = t.get<terrain::Terrain>();
            const std::shared_ptr<terrain::TerrainData> data = terrain_store_.get(comp);
            check(data != nullptr && std::filesystem::exists(project_.assetsFolder() / dialogs::fromUtf8(comp.data)),
                  "sus datos en Assets/Terrains", f);
            // Un cubo fisico encima de la loma. Se crea (y se vuelca al historial
            // con el terreno) antes del trazo: asi el trazo es lo ultimo que se
            // deshace en el paso siguiente.
            ecs::Entity cube = createEntity(13, {});
            cube.setWorldPosition(Vec3{0.0f, 20.0f, 0.0f});
            self_test_cube_ = cube.uuid();
            flushCommit();
            if (data) {
                terrain::generateRelief(*data, 11, 3.0f, 0.5f, 0.3f, 0.12f);
                terrain_brush_ = terrain::TerrainBrush{};
                terrain_brush_.tool = terrain::TerrainTool::Flatten;
                terrain_brush_.radius = 14.0f;
                terrain_brush_.target_height = 8.0f;
                auto before = std::make_shared<terrain::TerrainData>(*data);
                for (int i = 0; i < 120; ++i) {
                    terrain::applyBrush(*data, comp, t.worldPosition(), Vec3{0.0f, 0.0f, 0.0f}, terrain_brush_, 1.0f / 30.0f);
                }
                data->commitCollision();
                pushTerrainUndo(comp.data, std::move(before));
                terrain::paintByRules(*data, comp, 2, 30.0f, 90.0f, 0.0f, 1.0f);
                terrain::paintByRules(*data, comp, 1, 0.0f, 90.0f, 0.0f, 0.15f);
            }
            self_test_terrain_ = t.uuid();
            selectOnly(t.uuid());
            terrain_edit_ = true;
            scene_.placeCamera(Vec3{45.0f, 35.0f, 60.0f}, Vec3{0.0f, 5.0f, 0.0f});
            focus_scene_ = true;
            enterPlay();
            focus_game_ = false;
            focus_scene_ = true;
            self_test_wait_ = 150;
            break;
        }
        case 28: {
            check(renderer_.terrainChunkCount() > 0, "el terreno se dibuja (trozos con LOD)", f);
            const ecs::Entity cube = world_.find(self_test_cube_);
            check(cube.valid() && std::abs(cube.worldPosition().y - 8.5f) < 0.3f,
                  "un cubo fisico cae y se queda sobre la loma esculpida (8 m)", f);
            exitPlay();
            // Deshacer el trazo: la loma vuelve al relieve generado.
            const ecs::Entity t = world_.find(self_test_terrain_);
            const terrain::Terrain& comp = t.get<terrain::Terrain>();
            const auto data = terrain_store_.get(comp);
            const float sculpted = terrain::heightAt(*data, comp, t.worldPosition(), 0.0f, 0.0f);
            undo();
            const ecs::Entity t_undone = world_.find(self_test_terrain_);
            const float undone = terrain::heightAt(*data, t_undone.get<terrain::Terrain>(), t_undone.worldPosition(), 0.0f, 0.0f);
            check(std::abs(sculpted - 8.0f) < 0.2f && std::abs(undone - 8.0f) > 0.3f, "Ctrl+Z deshace el trazo de terreno", f);
            redo();
            const ecs::Entity t_redone = world_.find(self_test_terrain_);
            check(std::abs(terrain::heightAt(*data, t_redone.get<terrain::Terrain>(), t_redone.worldPosition(), 0.0f, 0.0f) - 8.0f) < 0.2f,
                  "y Ctrl+Y lo rehace", f);
            selectOnly(self_test_terrain_);
            self_test_wait_ = 60;
            break;
        }
        case 29: {  // Navegacion: volumen ajustado a la escena y un agente sobre la loma.
            const ecs::Entity volume = createNavigationEntity(0);
            check(volume.valid() && volume.has<navigation::NavMeshBounds>() &&
                      volume.get<navigation::NavMeshBounds>().size.x > 50.0f,
                  "Crear > Navegacion > Volumen se ajusta a la escena", f);
            ecs::Entity agent = world_.create("Agente de prueba");
            agent.setWorldPosition(Vec3{0.0f, 8.0f, 0.0f});
            agent.add<navigation::NavAgent>().speed = 5.0f;
            self_test_agent_ = agent.uuid();
            show_navigation_ = true;
            commit();
            self_test_wait_ = 30;
            break;
        }
        case 30: {
            if (nav_.building()) {  // las baldosas se generan en otros hilos
                self_test_wait_ = 10;
                return;
            }
            const navigation::NavStats stats = nav_.stats();
            std::cout << "[SelfTest] Navegacion: " << stats.tiles << " baldosas, " << stats.polygons << " poligonos"
                      << std::endl;
            check(nav_.ready() && !nav_.debugMesh().triangles.empty(), "la malla de navegacion se genera y se ve", f);
            // Tiempo real: mover el cubo de la loma rehace solo sus baldosas.
            ecs::Entity cube = world_.find(self_test_cube_);
            if (cube.valid()) cube.setWorldPosition(cube.worldPosition() + Vec3{3.0f, 0.0f, 0.0f});
            updateNavigation(0.016f);
            const int pending = nav_.stats().pending_tiles;
            check(pending > 0 && pending < std::max(stats.tiles, 1), "mover un objeto rehace solo sus baldosas", f);
            // Play: el agente camina hacia un punto de la malla.
            enterPlay();
            const ecs::Entity a = world_.find(self_test_agent_);
            Vec3 target{};
            check(nav_.randomPoint(a.worldPosition(), 12.0f, target), "punto al azar de la malla", f);
            self_test_nav_start_ = a.worldPosition();
            self_test_nav_target_ = target;
            nav_.moveTo(a, target);
            self_test_wait_ = 240;
            break;
        }
        case 31: {
            const ecs::Entity a = world_.find(self_test_agent_);
            const float before = core::length(self_test_nav_target_ - self_test_nav_start_);
            const float after = core::length(self_test_nav_target_ - a.worldPosition());
            std::cout << "[SelfTest] Agente: " << before << " m -> " << after << " m del destino" << std::endl;
            check(a.valid() && (after < 0.8f || after < before * 0.5f), "el agente camina por la malla en Play", f);
            exitPlay();
            check(nav_.ready(), "al salir de Play la malla sigue (sin regenerarla)", f);
            self_test_wait_ = 30;
            break;
        }
        case 32: {  // Mundo de bloques: se crea con su mar y se genera alrededor de la camara.
            const ecs::Entity voxel_world = createVoxelWorldEntity();
            bool has_sea = false;
            for (const entt::entity child : voxel_world.children()) has_sea = has_sea || world_.wrap(child).has<water::WaterBody>();
            check(voxel_world.has<voxel::VoxelWorld>() && has_sea && voxels_.active(),
                  "Crear > Mundo de bloques (con su mar) arranca la vista previa", f);
            self_test_wait_ = 30;
            break;
        }
        case 33: {
            // Chunks en hilos y texturas generadas por codigo: hasta ~20 s.
            static int tries = 0;
            const gfx::VoxelStats gpu = renderer_.voxelStats();
            if ((voxels_.stats().pending_meshes > 0 || !renderer_.hasVoxelTextures() || gpu.sections == 0) && ++tries < 200) {
                self_test_wait_ = 6;
                return;
            }
            const voxel::VoxelStats st = voxels_.stats();
            std::cout << "[SelfTest] Voxeles: " << st.chunks << " chunks, " << gpu.sections << " secciones en la GPU ("
                      << gpu.visible << " visibles, " << gpu.quads << " caras, " << (gpu.memory_bytes >> 20) << " MB)"
                      << std::endl;
            check(renderer_.hasVoxelTextures() && gpu.sections > 0 && gpu.visible > 0, "los bloques se mallan y se dibujan", f);
            // Play: un mundo nuevo; al parar vuelve la vista previa.
            enterPlay();
            check(voxels_.active(), "en Play hay mundo de bloques", f);
            exitPlay();
            updateVoxels(0.016f);
            check(voxels_.active(), "al salir de Play vuelve la vista previa", f);
            self_test_wait_ = 60;
            break;
        }
        case 34: {  // Sombras de luces locales: suelo, cubo y una luz puntual, sin sol.
            for (const entt::entity h : world_.registry().view<voxel::VoxelWorld>()) world_.destroy(world_.wrap(h));
            updateVoxels(0.0f);
            world_.forEachDepthFirst([&](ecs::Entity e) {
                const ecs::Light* l = e.tryGet<ecs::Light>();
                if (l != nullptr && l->type == ecs::LightType::Directional && e.activeSelf() && !self_test_sun_.valid()) {
                    self_test_sun_ = e.uuid();
                    e.setActive(false);
                }
            });
            const Vec3 base{400.0f, 0.0f, 400.0f};
            ecs::Entity floor = ecs::createPrimitive(world_, assets::builtin::kCube, "Suelo sombras");
            floor.setWorldPosition(base + Vec3{0.0f, -0.5f, 0.0f});
            floor.setLocalScale(Vec3{30.0f, 1.0f, 30.0f});
            ecs::Entity cube = ecs::createPrimitive(world_, assets::builtin::kCube, "Cubo sombras");
            cube.setWorldPosition(base + Vec3{0.0f, 1.0f, 0.0f});
            cube.setLocalScale(Vec3{2.0f, 2.0f, 2.0f});
            ecs::Entity light = ecs::createLight(world_, ecs::LightType::Point);
            light.setWorldPosition(base + Vec3{-3.0f, 4.0f, 0.0f});
            ecs::Light& l = light.get<ecs::Light>();
            l.intensity = 80.0f;
            l.range = 25.0f;
            l.color = Vec3{1.0f, 1.0f, 1.0f};
            self_test_light_ = light.uuid();
            clearSelection();
            preferred_view_ = kSceneSlot;
            focus_scene_ = true;
            scene_.placeCamera(base + Vec3{0.0f, 14.0f, 9.0f}, base);
            // Y la camara del juego igual (la vista Juego puede ser la que se dibuja).
            if (ecs::Entity cam = world_.findByName("Main Camera"); cam.valid()) {
                const Vec3 eye = base + Vec3{0.0f, 14.0f, 9.0f};
                const Vec3 d = core::normalize(base - eye);
                constexpr float kDeg = 57.2957795f;
                cam.setWorldPosition(eye);
                cam.setLocalEulerDegrees(Vec3{std::asin(d.y) * kDeg, std::atan2(-d.x, -d.z) * kDeg, 0.0f});
            }
            self_test_wait_ = 120;
            break;
        }
        case 35:
        case 37: {  // Captura con sombra; se apaga "Proyecta sombras".
            const bool spot = self_test_step_ == 37;
            const std::filesystem::path png = std::filesystem::temp_directory_path() /
                                              (spot ? "cramion_selftest_spot_on.png" : "cramion_selftest_point_on.png");
            self_test_capture_ = captureWindow(window_.handle(), png);
            std::cout << "[SelfTest] Captura: " << dialogs::utf8(png) << std::endl;
            ecs::Entity light = world_.find(self_test_light_);
            if (light.valid()) light.get<ecs::Light>().cast_shadows = false;
            self_test_wait_ = 30;
            break;
        }
        case 36:
        case 38: {
            const bool spot = self_test_step_ == 38;
            const std::filesystem::path png = std::filesystem::temp_directory_path() /
                                              (spot ? "cramion_selftest_spot_off.png" : "cramion_selftest_point_off.png");
            const std::vector<std::uint8_t> without = captureWindow(window_.handle(), png);
            const float difference = captureDifference(self_test_capture_, without);
            std::cout << "[SelfTest] " << (spot ? "Foco" : "Luz puntual") << ": diferencia con/sin sombra " << difference
                      << std::endl;
            check(difference > 0.5f, spot ? "el foco proyecta sombra" : "la luz puntual proyecta sombra", f);
            ecs::Entity light = world_.find(self_test_light_);
            if (!spot && light.valid()) {
                // Ahora un foco desde el mismo sitio, mirando al cubo.
                ecs::Light& l = light.get<ecs::Light>();
                l.cast_shadows = true;
                l.type = ecs::LightType::Spot;
                l.outer_angle = 50.0f;
                l.inner_angle = 35.0f;
                const Vec3 base{400.0f, 0.0f, 400.0f};
                const Vec3 d = core::normalize(base - light.worldPosition());
                constexpr float kDeg = 57.2957795f;
                light.setLocalEulerDegrees(Vec3{std::asin(d.y) * kDeg, std::atan2(-d.x, -d.z) * kDeg, 0.0f});
            } else {
                // Fuera la escena de cubos (el sol sigue apagado para la de bloques).
                for (const char* name : {"Suelo sombras", "Cubo sombras"}) {
                    if (ecs::Entity e = world_.findByName(name); e.valid()) world_.destroy(e);
                }
                if (light.valid()) world_.destroy(light);
            }
            self_test_wait_ = spot ? 30 : 60;
            break;
        }
        case 39: {  // Sombras locales de los bloques: un mundo de bloques.
            createVoxelWorldEntity();
            self_test_wait_ = 30;
            break;
        }
        case 40: {
            static int tries = 0;
            if ((voxels_.stats().pending_meshes > 0 || !renderer_.hasVoxelTextures()) && ++tries < 200) {
                self_test_wait_ = 6;
                return;
            }
            // Una columna de piedra y una luz puntual a su lado.
            const int h = voxels_.surfaceHeight(8, 8);
            for (int y = h + 1; y <= h + 12; ++y) {
                for (int x = 2; x <= 14; ++x) {
                    for (int z = 2; z <= 14; ++z) voxels_.setBlock(x, y, z, voxel::block::Air);
                }
            }
            for (int y = h + 1; y <= h + 3; ++y) voxels_.setBlock(8, y, 8, voxel::block::Stone);
            for (int x = 2; x <= 14; ++x) {
                for (int z = 2; z <= 14; ++z) voxels_.setBlock(x, h, z, voxel::block::Stone);
            }
            const float top = static_cast<float>(h) + 1.0f;
            ecs::Entity light = ecs::createLight(world_, ecs::LightType::Point);
            light.setWorldPosition(Vec3{5.5f, top + 4.0f, 8.5f});
            ecs::Light& l = light.get<ecs::Light>();
            l.intensity = 80.0f;
            l.range = 20.0f;
            l.color = Vec3{1.0f, 1.0f, 1.0f};
            self_test_light_ = light.uuid();
            const Vec3 target{8.5f, top, 8.5f};
            scene_.placeCamera(target + Vec3{0.0f, 12.0f, 7.0f}, target);
            focus_scene_ = true;
            preferred_view_ = kSceneSlot;
            self_test_wait_ = 90;
            break;
        }
        case 41: {
            self_test_capture_ = captureWindow(window_.handle(), std::filesystem::temp_directory_path() / "cramion_selftest_voxel_on.png");
            if (ecs::Entity light = world_.find(self_test_light_); light.valid()) light.get<ecs::Light>().cast_shadows = false;
            self_test_wait_ = 30;
            break;
        }
        case 42: {
            const std::vector<std::uint8_t> without =
                captureWindow(window_.handle(), std::filesystem::temp_directory_path() / "cramion_selftest_voxel_off.png");
            const float difference = captureDifference(self_test_capture_, without);
            std::cout << "[SelfTest] Bloques con luz puntual: diferencia con/sin sombra " << difference << std::endl;
            check(difference > 0.3f, "los bloques proyectan sombra de una luz puntual", f);
            // Fin: fuera la luz y el mundo de bloques; vuelve el sol.
            if (ecs::Entity light = world_.find(self_test_light_); light.valid()) world_.destroy(light);
            for (const entt::entity h : world_.registry().view<voxel::VoxelWorld>()) world_.destroy(world_.wrap(h));
            if (ecs::Entity sun = world_.find(self_test_sun_); sun.valid()) sun.setActive(true);
            self_test_wait_ = 30;
            break;
        }
        case 43: {  // Mallas creadas por codigo (como el Mesh de Unity).
            ecs::Entity e = world_.create("Malla por codigo");
            e.setWorldPosition(Vec3{600.0f, 1.0f, 600.0f});
            auto mesh = ecs::Mesh::sphere(1.0f, 32, 16);
            ecs::MeshMaterial red;
            red.color = core::Vec4{0.9f, 0.3f, 0.2f, 1.0f};
            red.roughness = 0.4f;
            mesh->materials.push_back(red);
            e.add<ecs::MeshRenderer>().mesh = mesh;
            ecs::Entity floor = world_.create("Suelo por codigo");
            floor.setWorldPosition(Vec3{600.0f, 0.0f, 600.0f});
            floor.add<ecs::MeshRenderer>().mesh = ecs::Mesh::plane(12.0f, 12.0f, 4, 4);
            self_test_light_ = e.uuid();
            scene_.placeCamera(Vec3{600.0f, 4.0f, 606.0f}, Vec3{600.0f, 1.0f, 600.0f});
            focus_scene_ = true;
            preferred_view_ = kSceneSlot;
            self_test_wait_ = 30;
            break;
        }
        case 44: {
            const ecs::Entity e = world_.find(self_test_light_);
            const std::size_t triangles = renderer_.triangleCount();
            check(e.valid() && !sync_->actorIndicesInSubtree(e).empty(), "una malla creada por codigo se dibuja", f);
            // Cambiarla: el mismo hueco, sin volver a subir la escena.
            ecs::Mesh& mesh = *e.get<ecs::MeshRenderer>().mesh;
            const ecs::Mesh cube = *ecs::Mesh::cube(Vec3{1.5f, 1.5f, 1.5f});
            mesh.vertices = cube.vertices;
            mesh.normals = cube.normals;
            mesh.uv = cube.uv;
            mesh.tangents = cube.tangents;
            mesh.setTriangles(cube.triangles(0));
            mesh.markModified();
            std::cout << "[SelfTest] Malla por codigo: " << triangles << " triangulos en la escena" << std::endl;
            self_test_wait_ = 30;
            break;
        }
        case 45: {
            const ecs::Entity e = world_.find(self_test_light_);
            captureWindow(window_.handle(), std::filesystem::temp_directory_path() / "cramion_selftest_mesh.png");
            check(e.valid() && !sync_->actorIndicesInSubtree(e).empty(), "al cambiarla se sigue dibujando (el cubo nuevo)", f);
            for (const char* name : {"Malla por codigo", "Suelo por codigo"}) {
                if (ecs::Entity x = world_.findByName(name); x.valid()) world_.destroy(x);
            }
            self_test_wait_ = 30;
            break;
        }
        case 46: {  // La plantilla "Mundo de bloques": crearla desde el Hub y jugarla.
            ProjectTemplate voxel_template;
            for (const ProjectTemplate& t : availableTemplates()) {
                if (t.id == "voxel") voxel_template = t;
            }
            std::error_code error;
            std::filesystem::remove_all(self_test_folder_ / "Bloques", error);
            const project::ProjectInfo created = createProjectFromTemplate(voxel_template, self_test_folder_, "Bloques");
            check(openProject(created.folder), "crear y abrir un proyecto con la plantilla Mundo de bloques", f);
            show_game_ = true;
            enterPlay();
            self_test_wait_ = 600;  // el mundo se genera y las texturas de los bloques se crean
            break;
        }
        case 47: {
            // Un inventario y un estado para ver la interfaz completa (sin mover el raton real).
            std::string out;
            const bool ok = scripts_.run(R"(
                local J = Scene.find('Main Camera'):getScript()
                J:dar('oak_log', 12); J:dar('cobblestone', 30); J:dar('pico_madera', 1); J:dar('torch', 16)
                J:dar('manzana', 3); J:dar('glass', 8); J:dar('palo', 5)
                J.vida = 13; J.hambre = 15; J.relojNombre = 2
                J:pintarTodo()
            )", &out);
            check(ok && scripts_.errors().empty(), "el juego de la plantilla corre en el editor sin errores", f);
            self_test_wait_ = 60;
            break;
        }
        case 48: {
            captureWindow(window_.handle(), std::filesystem::temp_directory_path() / "cramion_selftest_bloques_hud.png");
            std::string out;
            scripts_.run("local J = Scene.find('Main Camera'):getScript(); J.ui.inventario.active = true; J.mesaCerca = true; J:pintarTodo()",
                         &out);
            self_test_wait_ = 30;
            break;
        }
        case 49: {
            captureWindow(window_.handle(), std::filesystem::temp_directory_path() / "cramion_selftest_bloques_inventario.png");
            // Contorno, grietas, chispas y un objeto suelto sobre el bloque que se mira.
            std::string out;
            const bool ok = scripts_.run(R"(
                local J = Scene.find('Main Camera'):getScript()
                J.ui.inventario.active = false
                J.muerto = true  -- sin logica de juego: la escena se queda quieta para la foto
                J.pitch = -50
                J:colocarCamara()
                local g = Voxel.raycast(J.entity.position, J.entity.forward, 6)
                if not g then return "sin bloque" end
                J.contorno.position = g.block + Vec3(0.5, 0.5, 0.5); J.contorno.active = true
                J.grietas.mesh = J.etapas[6]; J.grietas.position = g.block + Vec3(0.5, 0.5, 0.5); J.grietas.active = true
                J:chispear(g.block + Vec3(1, 0, 0), Voxel.blockInfo(g.id))
                J:soltarObjeto('cobblestone', 1, g.block + Vec3(-0.5, 1.2, 0.5))
                return "ok"
            )", &out);
            check(ok && out == "ok", "contorno, grietas y chispas sobre un bloque", f);
            self_test_wait_ = 6;
            break;
        }
        case 50: {
            captureWindow(window_.handle(), std::filesystem::temp_directory_path() / "cramion_selftest_bloques_romper.png");
            exitPlay();
            self_test_wait_ = 30;
            break;
        }
        case 51: {  // Shader de superficie propio (.crshader) en un material.
            const std::filesystem::path shader = project_.assetsFolder() / "Shaders" / "SelfTest.crshader";
            std::error_code error;
            std::filesystem::create_directories(shader.parent_path(), error);
            std::ofstream(shader, std::ios::binary) << R"(property color tinte = 0.2, 0.8, 1.0
property range lineas = 30 (5, 120)
void surface(inout Surface s) {
    float linea = step(0.5, fract(s.worldPosition.y * lineas * 0.1 - TIME * 2.0));
    s.albedo = vec3(0.05);
    s.emission = tinte * (0.3 + linea * 0.7 + fresnel(s, 2.5) * 2.0);
}
void vertex(inout Vertex v) {
    v.position += v.normal * 0.05 * sin(TIME * 4.0);
}
)";
            assets::MaterialAsset material;
            material.shader = "Shaders/SelfTest.crshader";
            material.shader_values["tinte"] = core::Vec4{1.0f, 0.3f, 0.9f, 0.0f};
            const std::filesystem::path material_path = project_.assetsFolder() / "Materials" / "SelfTestShader.crmat";
            std::filesystem::create_directories(material_path.parent_path(), error);
            check(assets::saveMaterial(material, material_path), "material con shader propio", f);
            refreshDatabase();
            ecs::Entity cube = createEntity(1, {});
            cube.setWorldPosition(Vec3{0.0f, 160.0f, 0.0f});
            cube.setLocalScale(Vec3{2.0f, 2.0f, 2.0f});
            check(applyMaterial(cube, material.uuid, -1), "asignar el material al cubo", f);
            self_test_light_ = cube.uuid();
            scene_.placeCamera(Vec3{0.0f, 161.5f, 6.0f}, Vec3{0.0f, 160.0f, 0.0f});
            focus_scene_ = true;
            preferred_view_ = kSceneSlot;
            self_test_wait_ = 90;
            break;
        }
        case 52: {
            const ecs::Entity cube = world_.find(self_test_light_);
            captureWindow(window_.handle(), std::filesystem::temp_directory_path() / "cramion_selftest_shader.png");
            const std::string error = sync_->surfaceShaderError("Shaders/SelfTest.crshader");
            if (!error.empty()) std::cout << "[SelfTest] Error del shader: " << error << std::endl;
            check(error.empty() && sync_->surfaceShader("Shaders/SelfTest.crshader") >= 0, "el .crshader compila y tiene pipeline", f);
            check(cube.valid() && !sync_->actorIndicesInSubtree(cube).empty(), "el cubo con shader propio se dibuja", f);
            // Un error: se detecta con su linea y el material sigue dibujandose.
            const std::filesystem::path shader = project_.assetsFolder() / "Shaders" / "SelfTest.crshader";
            std::ofstream(shader, std::ios::binary) << "void surface(inout Surface s) {\n    s.albedo = noExiste;\n}\n";
            std::filesystem::last_write_time(shader, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(2));
            sync_->reloadSurfaceShaders();
            const std::string broken = sync_->surfaceShaderError("Shaders/SelfTest.crshader");
            check(broken.find("SelfTest.crshader:2:") != std::string::npos, "un error del shader dice su linea", f);
            // Arreglarlo: recompila en caliente.
            std::ofstream(shader, std::ios::binary) << "property color tinte = 1, 1, 1\nvoid surface(inout Surface s) {\n    s.emission = vec3(1.0, 0.4, 0.0) * 2.0;\n}\n";
            std::filesystem::last_write_time(shader, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(4));
            sync_->reloadSurfaceShaders();
            check(sync_->surfaceShaderError("Shaders/SelfTest.crshader").empty(), "al arreglarlo se recompila en caliente", f);
            self_test_wait_ = 60;
            break;
        }
        case 53: {
            captureWindow(window_.handle(), std::filesystem::temp_directory_path() / "cramion_selftest_shader_recompilado.png");
            const ecs::Entity cube = world_.find(self_test_light_);
            check(cube.valid() && !sync_->actorIndicesInSubtree(cube).empty(), "y se sigue dibujando", f);
            self_test_wait_ = 10;
            break;
        }
        case 54: {  // Navegador de contenido: carpetas y assets de cada tipo.
            const std::filesystem::path assets = project_.assetsFolder();
            std::error_code error;
            for (const char* folder : {"Personajes", "Niveles", "Texturas"}) std::filesystem::create_directories(assets / folder, error);
            createMaterialAsset(assets);
            std::ofstream(assets / "Scripts" / "Jugador.lua") << scripting::scriptTemplate("Jugador");
            std::filesystem::create_directories(assets / "Shaders", error);
            std::ofstream(assets / "Shaders" / "Brillo.crshader") << assets::surfaceShaderTemplate("Brillo");
            refreshDatabase();
            current_folder_ = assets;
            show_project_ = true;
            browser_list_view_ = false;
            icon_size_ = 96.0f;
            ImGui::SetWindowFocus("Proyecto");
            self_test_wait_ = 60;
            break;
        }
        case 55: {
            captureWindow(window_.handle(), std::filesystem::temp_directory_path() / "cramion_selftest_browser.png");
            check(!browser_items_.empty(), "el navegador muestra la carpeta Assets", f);
            browser_list_view_ = true;
            self_test_wait_ = 20;
            break;
        }
        case 56: {
            captureWindow(window_.handle(), std::filesystem::temp_directory_path() / "cramion_selftest_browser_lista.png");
            browser_list_view_ = false;
            project_filter_ = "mat";
            self_test_wait_ = 20;
            break;
        }
        case 57: {
            captureWindow(window_.handle(), std::filesystem::temp_directory_path() / "cramion_selftest_browser_busqueda.png");
            project_filter_.clear();
            self_test_wait_ = 10;
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

// Para el MCP (captura de pantalla para la IA).
std::vector<std::uint8_t> captureEditorWindow(HWND hwnd, const std::filesystem::path& png) {
    return captureWindow(hwnd, png);
}

}  // namespace cramion::editor

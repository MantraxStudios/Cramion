#ifndef CRAMION_EDITOR_EDITOR_APP_H
#define CRAMION_EDITOR_EDITOR_APP_H

// CramionEditor: el editor de escenas, al estilo de Unity.
//
// Sin proyecto abierto muestra el Hub (crear, abrir, recientes). Con proyecto:
//
//   Escena        la imagen del render; clic = seleccionar (clic otra vez en
//                 el mismo sitio baja por la jerarquia), gizmos W/E/R con
//                 snapping, boton derecho + WASD = volar, rueda = acercar,
//                 boton central = desplazar, F = enfocar
//   Jerarquia     entidades del mundo en orden, arrastrar para emparentar o
//                 reordenar, multiseleccion, renombrar, duplicar, copiar/pegar
//   Inspector     componentes de la seleccion (reflexion) + Add Component
//   Proyecto      carpetas y assets de Assets/, importar, arrastrar a la escena
//   Animator      editor visual de Animator Controllers (.cranimator):
//                 estados, transiciones, parametros; vista previa en vivo
//   Juego         la vista desde la camara real (Camera, movida por el
//                 Camera Brain), separada de la Escena
//   Cinematica    linea de tiempo de una Cinematic Sequence: planos de
//                 camaras virtuales, mezclas, objetos activos, cabezal
//   Fisica        capas y matriz de colisiones, ajustes, registro de eventos
//                 (colisiones, triggers, particulas), probador de raycast y
//                 estadisticas. Play / Pausa / Paso simulan con Jolt.
//   Estadisticas, Consola, Ajustes de render
//
// La fuente de verdad es el ecs::World; scene::Scene es solo lo que se le da
// al renderizador (RenderSync la rellena cada frame).

#include "ImGuiLayer.h"
#include "PropertyInspector.h"

#include <CramionCore/CramionCore.h>
#include <CramionDM/CramionDM.h>
#include <CramionFX/CramionFX.h>

#include <algorithm>
#include <array>
#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cramion::editor {

// std::find sobre Uuid: la STL de MSVC intenta vectorizar la comparacion de
// 16 bytes y con clang falla una static_assert; find_if la evita.
inline std::vector<Uuid>::const_iterator findUuid(const std::vector<Uuid>& list, const Uuid& uuid) {
    return std::find_if(list.begin(), list.end(), [&](const Uuid& u) { return u == uuid; });
}
inline std::vector<Uuid>::iterator findUuid(std::vector<Uuid>& list, const Uuid& uuid) {
    return std::find_if(list.begin(), list.end(), [&](const Uuid& u) { return u == uuid; });
}

// Imagen de Assets/ arrastrada desde el Proyecto (ruta UTF-8 terminada en 0).
inline constexpr const char* kImagePayload = "CRAMION_IMAGE";

class EditorApp {
public:
    EditorApp(dm::Window& window, gfx::VulkanRenderer& renderer, scene::Scene& scene,
              ImGuiLayer& imgui);
    ~EditorApp();

    // Abre un proyecto (.crproj o su carpeta). false si no es valido.
    bool openProject(const std::filesystem::path& path);

    // La interfaz de un frame (entre ImGuiLayer::beginFrame y endFrame).
    void drawUi(float delta_seconds);
    // Lleva el mundo al renderizador (despues de scene.update, antes de
    // drawFrame).
    void syncWorld(float delta_seconds);

    // La camara del editor solo recibe la entrada mientras se vuela.
    bool sceneWantsInput() const { return flying_; }
    // Despues de renderer.drawFrame: devuelve la camara del editor si este
    // frame se dibujo la vista Juego (con la camara real).
    void afterRender();
    bool quitRequested() const { return quit_; }
    // Cerrar la ventana: pregunta si hay cambios sin guardar.
    void requestQuit();

    // Archivos soltados sobre la ventana desde el Explorador: se importan.
    void onFilesDropped(const std::vector<std::filesystem::path>& files);

    // Prueba automatica de extremo a extremo (CramionEditor.exe --selftest
    // <carpeta> <modelo> <hdr>): crea un proyecto, importa, instancia,
    // selecciona, mueve, deshace, guarda y reabre, a lo largo de varios
    // frames para que el render trabaje de verdad. Escribe "[SelfTest]" en
    // la consola y deja la escena visible al terminar.
    void startSelfTest(const std::filesystem::path& folder, const std::filesystem::path& model,
                       const std::filesystem::path& environment, const std::filesystem::path& image = {});
    bool selfTestFinished() const { return self_test_step_ < 0; }

    // Tiempo de CPU del render del frame (lo mide main.cpp alrededor de
    // drawFrame) para el desglose de Estadisticas.
    void setRenderCpuTime(float milliseconds) { addCpuSample(kCpuRender, milliseconds); }

private:
    // Desglose del tiempo de CPU por frame (Estadisticas). Media movil.
    enum CpuSection {
        kCpuUi = 0,
        kCpuHierarchy,
        kCpuInspector,
        kCpuScene,
        kCpuPanels,
        kCpuPhysics,
        kCpuSync,
        kCpuRender,
        kCpuSectionCount
    };
    void addCpuSample(int section, float milliseconds) {
        float& value = cpu_ms_[static_cast<std::size_t>(section)];
        value = value * 0.9f + milliseconds * 0.1f;
    }
    std::array<float, kCpuSectionCount> cpu_ms_{};

    enum class GizmoOperation { None, Translate, Rotate, Scale };
    enum class PendingAction { None, NewScene, OpenScene, BackToHub, Quit };

    // --- Proyecto y escena (EditorApp.cpp) ---
    void closeProject();
    void newScene();
    bool openScene(const std::filesystem::path& path);
    bool saveScene();
    bool saveSceneAs();
    void runOrAskToSave(PendingAction action, const std::filesystem::path& scene = {});
    void performPending();
    void updateTitle();

    // --- Deshacer (EditorApp.cpp) ---
    void resetUndo();
    void commit();  // punto de deshacer tras una accion completa (se hace al final del frame)
    void flushCommit();
    bool commit_pending_ = false;
    void undo();
    void redo();

    // --- Seleccion ---
    bool isSelected(const Uuid& uuid) const;
    void selectOnly(const Uuid& uuid);
    void toggleSelection(const Uuid& uuid);
    void clearSelection();
    std::vector<ecs::Entity> selectedEntities() const;
    // Sin los que ya estan dentro de otro seleccionado (para mover, borrar,
    // duplicar sin hacerlo dos veces).
    std::vector<ecs::Entity> topLevelSelection() const;
    void revealInHierarchy(const Uuid& uuid);

    // --- Acciones sobre entidades ---
    ecs::Entity createEntity(int kind, ecs::Entity parent);
    void deleteSelection();
    void duplicateSelection();
    void copySelection();
    void pasteClipboard();
    void focusSelection();
    ecs::Entity instantiateAsset(const Uuid& uuid, ecs::Entity parent,
                                 const std::optional<core::Vec3>& world_position);
    void assignEnvironment(const Uuid& uuid);

    // --- Paneles ---
    void drawHub();
    void drawMenuBar();
    void drawToolbar();
    void drawHierarchy();
    // Jerarquia recortada: filas aplanadas (solo nodos abiertos) y se dibujan
    // solo las que se ven.
    struct HierarchyRow {
        entt::entity entity = entt::null;
        int depth = 0;
    };
    void buildHierarchyRows();
    void drawHierarchyRow(const HierarchyRow& row, bool scroll_to);
    static const void* hierarchyRowId(entt::entity entity);
    void drawInspector();
    void drawAddComponent(ecs::Entity entity);
    void drawProject();
    void drawSceneView();
    void drawSceneOverlays();
    void drawGizmo();

    // --- Geometria de ayuda en 3D con profundidad (EditorOverlay.cpp) ---
    // Se acumula durante el frame en overlay_ y flushOverlay() la da al
    // renderizador (setOverlayGeometry), que la dibuja con prueba de
    // profundidad: opaca donde se ve, atenuada donde queda tapada.
    void overlayLine(const core::Vec3& a, const core::Vec3& b, std::uint32_t color);
    void overlayTriangle(const core::Vec3& a, const core::Vec3& b, const core::Vec3& c, std::uint32_t color);
    void overlayQuad(const core::Vec3& a, const core::Vec3& b, const core::Vec3& c, const core::Vec3& d,
                     std::uint32_t color);
    void overlayCircle(const core::Vec3& center, const core::Vec3& u, const core::Vec3& v, float radius,
                       std::uint32_t color, bool front_only = false, int segments = 64);
    void overlayCone(const core::Vec3& base, const core::Vec3& direction, float length, float radius,
                     std::uint32_t color);
    void overlayCube(const core::Vec3& center, const core::Vec3& x, const core::Vec3& y, const core::Vec3& z,
                     float half, std::uint32_t color);
    void overlayBoxEdges(const core::Mat4& m, std::uint32_t color);
    void overlayScreenDisc(const core::Vec3& center, float radius, std::uint32_t color);
    float gizmoWorldSize(const core::Vec3& origin) const;
    // operation: 0 mover, 1 rotar, 2 escalar.
    void drawGizmoGeometry(const core::Mat4& matrix, bool local, int operation);
    void flushOverlay();
    // Gizmos de la luz seleccionada: alcance (esfera / cono) y angulos del
    // foco con asas que se arrastran. true si el raton esta sobre un asa.
    bool drawLightGizmos();

    // --- Fisica y modo Play (EditorPhysics.cpp) ---
    // Play: se guarda el mundo, la fisica simula y al parar se restaura
    // (como Unity). Fuera de Play la fisica solo refleja el mundo (raycast y
    // vista previa de particulas funcionan igual).
    void loadPhysicsSettings();
    void savePhysicsSettings();
    void applyPhysicsSettings();
    void enterPlay();
    void exitPlay();
    void togglePause();
    bool playing() const { return play_state_ != PlayState::Edit; }
    void updatePhysics(float delta_seconds);
    void onPhysicsEvent(const physics::PhysicsEvent& event);
    void drawPlayControls();
    void drawPhysicsWindow();
    void drawPhysicsGizmos();
    bool drawColliderHandles();
    void drawColliderGizmo(ecs::Entity entity, bool selected);
    void drawParticleEmitterGizmo(ecs::Entity entity);
    void drawRaycastTester();
    void runRaycastTester();
    bool layerCombo(const char* label, int& layer);
    bool layerMaskCombo(const char* label, std::uint32_t& mask);

    // --- Vista Juego y cinematicas (EditorCinematics.cpp) ---
    static constexpr std::uint32_t kSceneSlot = 0;
    static constexpr std::uint32_t kGameSlot = 1;
    void drawGameView();
    void chooseRenderView();
    void updateCinematics(float delta_seconds);
    bool drawCinematicGizmos();  // true si el raton esta sobre un asa
    void drawCameraFrustum(const core::Vec3& position, const core::Quat& rotation, float fov_degrees, float depth,
                           std::uint32_t color);
    bool drawWaypointGizmo(const core::Mat4& view, const core::Mat4& projection);
    void drawCinematicWindow();
    void drawCinematicInspector(const std::string& type_name, ecs::Entity entity);
    ecs::Entity ensureCameraBrain();
    ecs::Entity createCinematic(int kind);
    void alignWithView(ecs::Entity entity);

    // --- Decals (EditorDecals.cpp) ---
    bool surfaceHit(float x, float y, core::Vec3& point, core::Vec3& normal) const;
    void placeDecal(ecs::Entity decal, const core::Vec3& point, const core::Vec3& normal, float size, float depth,
                    float spin_degrees);
    ecs::Entity createDecal(int type, ecs::Entity parent);
    ecs::Entity stampAt(const core::Vec3& point, const core::Vec3& normal, const std::string& texture);
    void drawStampToolbar();
    bool drawStampTool();
    void drawDecalGizmos();
    std::string importDecalImage();
    std::string decalImageInAssets(const std::filesystem::path& file);
    static bool isDecalImage(const std::filesystem::path& path);
    // Icono (y su color) que representa a una entidad: Jerarquia, Escena.
    Icon entityIcon(const ecs::Entity& e, ImU32& tint) const;
    void drawStatistics(float delta_seconds);
    void drawConsole();
    void drawRenderSettings();
    void drawModals();
    void buildDefaultLayout(unsigned int dockspace_id);

    // --- Vista ---
    bool worldToScreen(const core::Vec3& world, float& x, float& y) const;
    bool mouseRay(float x, float y, core::Vec3& origin, core::Vec3& direction) const;
    void pickAt(float x, float y);
    void finishPick(const gfx::VulkanRenderer::PickResult& result);
    // Clic esperando el resultado del picking por GPU.
    struct PendingPick {
        bool active = false;
        float x = 0.0f;
        float y = 0.0f;
        bool additive = false;
    } pending_pick_;
    void handleCameraControls();

    void runSelfTestStep();
    void printCpuTimings(const char* when);

    // --- Importacion en segundo plano ---
    void startImport(const std::vector<std::filesystem::path>& files,
                     const std::filesystem::path& folder);
    void pollImports();
    // Vigila Assets/ (archivos nuevos, copiados, borrados o movidos desde
    // fuera) y refresca la base de datos sola.
    void watchAssets();
    void createSceneAsset(const std::filesystem::path& folder);

    // --- Animator (EditorAnimator.cpp) ---
    void createAnimatorAsset(const std::filesystem::path& folder);
    void openAnimatorEditor(const Uuid& uuid);
    void saveAnimatorEditor();
    void assignAnimatorToSelection(const Uuid& uuid);
    void drawAnimatorEditor();
    void drawAnimatorGraph(ecs::Entity preview);
    void drawAnimatorSidePanel(ecs::Entity preview);
    // Entidad seleccionada que usa el controlador abierto (vista previa).
    ecs::Entity animatorPreviewEntity();
    // Clips de la entidad (o de su subarbol) para el menu Extraer.
    ecs::Entity animatedEntityIn(ecs::Entity entity);
    void drawExtractAnimationsMenu(ecs::Entity entity);
    void extractAnimations(ecs::Entity entity, int clip);  // -1 = todas

    dm::Window& window_;
    gfx::VulkanRenderer& renderer_;
    scene::Scene& scene_;
    ImGuiLayer& imgui_;

    // Proyecto abierto.
    bool has_project_ = false;
    project::ProjectInfo project_{};
    std::unique_ptr<assets::AssetDatabase> database_;
    std::unique_ptr<assets::AssetManager> asset_manager_;
    std::unique_ptr<ecs::RenderSync> sync_;
    ecs::World world_;
    std::filesystem::path scene_path_;  // vacia = escena sin guardar
    bool dirty_ = false;

    // Seleccion (por UUID: sobrevive a deshacer, que recrea las entidades).
    std::vector<Uuid> selection_;
    Uuid active_{};
    Uuid reveal_{};
    std::vector<HierarchyRow> hierarchy_rows_;  // filas de la jerarquia este frame (Shift+clic)
    Uuid renaming_{};
    std::string rename_buffer_;
    bool rename_focus_ = false;
    std::string hierarchy_filter_;
    std::vector<std::string> clipboard_;

    // Deshacer: instantaneas del mundo en JSON.
    std::deque<std::string> undo_;
    std::deque<std::string> redo_;
    std::string current_state_;

    // Vista de escena.
    float view_x_ = 0.0f;
    float view_y_ = 0.0f;
    float view_w_ = 1.0f;
    float view_h_ = 1.0f;
    bool view_hovered_ = false;
    bool view_focused_ = false;
    bool flying_ = false;
    GizmoOperation gizmo_ = GizmoOperation::Translate;
    bool gizmo_local_ = false;
    bool gizmo_was_using_ = false;
    bool snap_enabled_ = false;
    float snap_translate_ = 0.25f;
    float snap_rotate_ = 15.0f;
    float snap_scale_ = 0.1f;
    float last_pick_x_ = -1.0f;
    gfx::OverlayGeometry overlay_;
    int light_handle_drag_ = 0;  // 0 nada, 1 alcance, 2 angulo exterior, 3 angulo interior
    // Herramienta de estampar decals.
    bool stamp_mode_ = false;
    struct StampBrush {
        int type = 0;  // 0 estampa, 1 charco, 2 humedad
        std::string texture;
        core::Vec3 color{1.0f, 1.0f, 1.0f};
        float opacity = 1.0f;
        float amount = 1.0f;
        bool follow_rain = false;
        float size = 1.0f;
        float depth = 0.5f;
        float spin = 0.0f;
        bool random_spin = false;
    } stamp_brush_;
    float last_pick_y_ = -1.0f;

    // Proyecto (navegador).
    std::filesystem::path current_folder_;
    float icon_size_ = 72.0f;
    std::string project_filter_;
    Uuid renaming_asset_{};
    std::string asset_rename_buffer_;
    Uuid pending_delete_asset_{};
    struct ImportJob {
        std::filesystem::path source;
        std::future<assets::ImportResult> result;
    };
    std::vector<ImportJob> imports_;
    // Vigilancia de Assets/ por notificacion del sistema (sin recorrer el
    // disco cada frame): HANDLE de FindFirstChangeNotificationW.
    void* assets_watch_ = nullptr;
    double refresh_at_ = -1.0;  // refresco pendiente (se agrupan rafagas de cambios)

    // Cache del navegador: el disco solo se lee cuando algo cambia.
    struct FolderNode {
        std::filesystem::path path;
        std::string name;
        std::vector<std::size_t> children;  // indices en folder_nodes_
    };
    std::vector<FolderNode> folder_nodes_;
    std::vector<std::filesystem::path> current_subfolders_;
    std::vector<assets::AssetInfo> current_assets_;
    std::vector<std::filesystem::path> current_images_;  // imagenes de la carpeta (texturas de decals)
    std::filesystem::path cached_folder_;
    std::uint64_t database_version_ = 0;  // sube con cada refresh de la base de datos
    std::uint64_t cached_version_ = ~0ull;
    void refreshDatabase();
    void rebuildBrowserCache();
    void drawFolderNode(std::size_t index);

    // Ventana Animator.
    bool show_animator_ = false;
    Uuid animator_uuid_{};
    std::filesystem::path animator_path_;
    ecs::AnimatorController animator_;
    bool animator_dirty_ = false;
    core::Vec2 animator_pan_{300.0f, 150.0f};
    float animator_zoom_ = 1.0f;
    int animator_selected_state_ = -1;       // -1 ninguno
    int animator_selected_transition_ = -1;
    int animator_link_from_ = -2;            // -2 = no se crea transicion; kAnyState = desde Any
    int animator_drag_ = -3;                 // estado arrastrado (-1 = Any, -2 = Entry, -3 = nada)
    bool animator_focus_ = false;
    unsigned int scene_dock_id_ = 0;  // nodo de la Escena (para acoplar el Animator)
    unsigned int console_dock_id_ = 0;  // nodo de la Consola (para acoplar la ventana Fisica)

    // Vistas y cinematicas.
    cinema::CinematicSystem cinematics_;
    bool show_game_ = true;
    bool show_cinematic_ = true;
    bool scene_view_visible_ = true;
    bool game_view_visible_ = false;
    std::uint32_t render_view_ = kSceneSlot;       // la vista que se dibuja este frame
    std::uint32_t last_render_view_ = kSceneSlot;
    std::uint32_t preferred_view_ = kSceneSlot;    // la ultima con la que se interactuo
    bool focus_game_ = false;                      // traer la vista Juego al frente
    bool focus_scene_ = false;                     // traer la Escena al frente
    std::optional<scene::Camera> saved_camera_;    // la del editor mientras se dibuja el Juego
    bool game_guides_ = false;                     // tercios en la vista Juego
    int selected_waypoint_ = -1;                   // punto del riel seleccionado (en la Escena)
    Uuid waypoint_track_{};
    bool waypoint_gizmo_using_ = false;
    int waypoint_drag_ = 0;            // 0 nada, 1 punto, 2 asa de salida, 3 asa de entrada
    int waypoint_drag_index_ = -1;
    float waypoint_drag_height_ = 0.0f;  // altura del plano al arrastrar un punto
    bool deleteSelectedWaypoint();
    // Ventana Cinematica.
    Uuid timeline_sequence_{};
    float timeline_time_ = 0.0f;
    bool timeline_preview_ = false;   // la secuencia manda en la camara (fuera de Play)
    bool timeline_playing_ = false;
    float timeline_zoom_ = 90.0f;     // pixeles por segundo
    int timeline_drag_ = 0;           // 0 nada, 1 cabezal, 2 mover plano, 3 duracion, 4 mezcla, 5 mover objeto, 6 fin objeto
    int timeline_item_ = -1;
    float timeline_drag_offset_ = 0.0f;
    int timeline_selected_shot_ = -1;

    // Fisica.
    enum class PlayState { Edit, Playing, Paused };
    physics::PhysicsSystem physics_;
    physics::ParticleWorld particles_;
    physics::PhysicsSettings physics_settings_;
    PlayState play_state_ = PlayState::Edit;
    std::string play_snapshot_;     // el mundo al darle a Play
    bool play_dirty_before_ = false;
    int step_requests_ = 0;         // "Paso" en pausa
    float play_time_ = 0.0f;
    bool show_physics_ = true;
    bool physics_settings_dirty_ = false;
    // Gizmos de fisica.
    bool gizmo_all_colliders_ = false;
    bool gizmo_contacts_ = true;
    bool gizmo_queries_ = true;
    bool gizmo_velocity_ = true;
    bool edit_collider_ = false;
    int collider_handle_drag_ = 0;  // 0 nada; asas de caja (1..6), radio (7), altura (8, 9)
    core::Vec3 collider_drag_anchor_{};  // cara fija de la caja al arrastrar
    // Registro de eventos.
    struct LoggedEvent {
        physics::PhysicsEventType type = physics::PhysicsEventType::CollisionEnter;
        std::string a;
        std::string b;
        Uuid a_uuid{};
        Uuid b_uuid{};
        core::Vec3 point{};
        std::uint64_t step = 0;
        float time = 0.0f;
    };
    std::deque<LoggedEvent> event_log_;
    std::array<std::uint64_t, physics::kPhysicsEventTypeCount> event_counts_{};
    std::array<bool, physics::kPhysicsEventTypeCount> event_filter_{true, false, true, true, false, true, false};
    bool event_log_console_ = true;
    static constexpr int kConsoleEventsPerFrame = 8;
    int console_events_this_frame_ = 0;
    int console_events_skipped_ = 0;
    bool event_log_paused_ = false;
    // Probador de raycast.
    struct RaycastTester {
        bool enabled = false;
        int query = 0;    // 0 Raycast, 1 RaycastAll, 2 SphereCast, 3 OverlapSphere, 4 OverlapBox
        int origin = 0;   // 0 camara, 1 seleccion (hacia delante), 2 manual, 3 raton (Alt + clic)
        core::Vec3 manual_origin{0.0f, 5.0f, 0.0f};
        core::Vec3 manual_direction{0.0f, -1.0f, 0.0f};
        float distance = 100.0f;
        float radius = 0.5f;
        std::uint32_t mask = physics::kDefaultRaycastLayers;
        int triggers = 0;  // QueryTriggers
        bool click_from_mouse = true;
        core::Vec3 mouse_origin{};
        core::Vec3 mouse_direction{};
        bool has_mouse_ray = false;
        // Resultado del ultimo lanzamiento.
        core::Vec3 from{};
        core::Vec3 to{};
        std::vector<physics::RaycastHit> hits;
        std::vector<ecs::Entity> overlaps;
    } raycast_;

    // Hub.
    bool show_new_project_ = false;
    std::string new_project_name_ = "Mi proyecto";
    std::string new_project_folder_;
    std::string hub_error_;

    // Paneles visibles y acciones pendientes.
    bool show_hierarchy_ = true;
    bool show_inspector_ = true;
    bool show_project_ = true;
    bool show_statistics_ = true;
    bool show_console_ = true;
    bool show_render_settings_ = true;
    bool reset_layout_ = false;
    PendingAction pending_ = PendingAction::None;
    std::filesystem::path pending_scene_;
    bool ask_save_ = false;
    bool quit_ = false;

    // Estadisticas / consola.
    std::array<float, 240> frame_history_{};
    std::size_t frame_history_head_ = 0;
    std::string console_filter_;
    bool console_autoscroll_ = true;

    // Auto-prueba (0 = sin prueba, < 0 = terminada).
    int self_test_step_ = 0;
    int self_test_wait_ = 0;
    int self_test_failures_ = 0;
    std::filesystem::path self_test_folder_;
    std::filesystem::path self_test_model_;
    std::filesystem::path self_test_environment_;
    std::filesystem::path self_test_image_;  // imagen para probar los decals (opcional)
    Uuid self_test_car_{};
    Uuid self_test_cube_{};
    Uuid self_test_zone_{};
    Uuid self_test_sequence_{};
    std::size_t self_test_entities_ = 0;
};

}  // namespace cramion::editor

#endif  // CRAMION_EDITOR_EDITOR_APP_H

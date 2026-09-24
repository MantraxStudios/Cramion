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

private:
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
    void commit();  // punto de deshacer tras una accion completa
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
    void drawHierarchyNode(ecs::Entity entity);
    void drawInspector();
    void drawAddComponent(ecs::Entity entity);
    void drawProject();
    void drawSceneView();
    void drawSceneOverlays();
    void drawGizmo();
    // Gizmos de la luz seleccionada: alcance (esfera / cono) y angulos del
    // foco con asas que se arrastran. true si el raton esta sobre un asa.
    bool drawLightGizmos();

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
    void drawStatistics(float delta_seconds);
    void drawConsole();
    void drawRenderSettings();
    void drawModals();
    void buildDefaultLayout(unsigned int dockspace_id);

    // --- Vista ---
    bool worldToScreen(const core::Vec3& world, float& x, float& y) const;
    bool mouseRay(float x, float y, core::Vec3& origin, core::Vec3& direction) const;
    void pickAt(float x, float y);
    void handleCameraControls();

    void runSelfTestStep();

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
    std::vector<Uuid> visible_order_;  // orden en la jerarquia (para Shift+clic)
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
    std::size_t self_test_entities_ = 0;
};

}  // namespace cramion::editor

#endif  // CRAMION_EDITOR_EDITOR_APP_H

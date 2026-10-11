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

#include "AndroidBuild.h"
#include "BuildConfig.h"

#include <CramionDM/TouchControls.h>
#include "Dialogs.h"
#include <CramionCore/world/WorldPartition.h>
#include <CramionCore/ai/Crowd.h>
#include <CramionCore/project/Pack.h>
#include <CramionCore/input/InputActions.h>
#include <CramionCore/asset/RenderTextureAsset.h>
#include "ImGuiLayer.h"
#include "Terminal.h"
#include "LuaCompletion.h"
#include "Clangd.h"
#include "McpServer.h"
#include "ModelPreviews.h"
#include "ProfilerOverlay.h"

#include <CramionCore/ecs/FloatingOrigin.h>
#include <CramionCore/xr/XrRig.h>
#include <CramionCore/fluid/Fluid.h>
#include <CramionCore/replay/Replay.h>
#include <CramionCore/twod/System2D.h>
#include <CramionCore/vfx/VisualEffect.h>
#include <CramionCore/modeling/EditableMesh.h>
#include <CramionCore/scripting/CppScripts.h>
#include <CramionCore/ai/StateMachine.h>
#include <functional>
#include <CramionCore/asset/ModelMaterials.h>
#include <CramionCore/terrain/TerrainGenerator.h>
#include <CramionCore/terrain/TerrainTools.h>
#include <CramionFX/asset/HouseGenerator.h>
#include <CramionFX/asset/MedievalBuildings.h>
#include <CramionFX/asset/SettlementGenerator.h>
#include <CramionUpdater/Update.h>
#include "PropertyInspector.h"
#include "ProjectTemplates.h"

#include <CramionCore/CramionCore.h>
#include <CramionDM/CramionDM.h>
#include <CramionFX/CramionFX.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace cramion::editor {

// Editar > Realidad virtual (EditorVR.ini del usuario; main.cpp lo lee antes
// de crear Vulkan): preparar Vulkan para el casco al abrir, y que runtime de
// OpenXR usar (xr::RuntimeChoice).
bool xrPlayPreference();
void setXrPlayPreference(bool on);
int xrRuntimePreference();
void setXrRuntimePreference(int choice);
bool xrStereoPreference();  // VR con una imagen por ojo (si no, una camara para los dos)
void setXrStereoPreference(bool on);

class RendererGraphicsHost;
struct PlatformState;  // EditorPlatform.cpp: Steam, Git, pruebas automaticas y avisos
struct GameplayEditorState;  // EditorGameplay.cpp: localizacion, partidas y dialogos
struct EffectsEditorState;   // EditorEffects.cpp: VFX Graph, 2D y repeticiones
struct GraphEditorState;     // EditorGraphs.cpp: Shader Graph y Visual Scripting
struct BtEditorState;        // EditorBehaviorTree.cpp
struct TwoDEditorState;      // Editor2D.cpp: tilesets, paleta y sprites

// Los editores que viven en un area al estilo de Blender (EditorAreas.cpp).
enum class AreaEditor {
    Scene,
    Game,
    Hierarchy,
    Inspector,
    Project,
    Console,
    Statistics,
    RenderSettings,
    Physics,
    Cinematic,
    Environment,
    Count
};
struct LightingEditorState;  // EditorLighting.cpp: horneado de la luz rebotada
struct MotionEditorState;    // EditorMotion.cpp: bases de Motion Matching
struct PhysicsToolsState;    // EditorDestruction.cpp: fracturar y asistente de vehiculo

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
// Carpeta del Proyecto arrastrada (ruta UTF-8): se mueve a otra.
inline constexpr const char* kFolderPayload = "CRAMION_FOLDER";
inline constexpr const char* kScriptPayload = "CRAMION_SCRIPT";  // ruta (utf8) de un .lua
inline constexpr const char* kAudioPayload = "CRAMION_AUDIO";    // ruta (utf8) de un audio

// Archivos de la 2.1 que se ven como los shaders en el Proyecto (extension en minusculas).
inline bool isGraphFileExtension(const std::string& ext) {
    return ext == ".crshadergraph" || ext == ".crgraph" || ext == ".crtileset";
}
inline const char* graphFileLabel(const std::string& ext) {
    if (ext == ".crshadergraph") return "Shader Graph";
    if (ext == ".crgraph") return "Visual Script";
    if (ext == ".crtileset") return "Tileset 2D";
    return "Shader";
}

class EditorApp {
public:
    EditorApp(dm::Window& window, gfx::VulkanRenderer& renderer, scene::Scene& scene,
              ImGuiLayer& imgui);
    ~EditorApp();

    // Abre un proyecto (.crproj o su carpeta). false si no es valido.
    // open_scene = false: sin la escena inicial (la abre la carga por
    // etapas, beginOpenProject).
    bool openProject(const std::filesystem::path& path, bool open_scene = true);
    // Abre el proyecto por etapas con el dialogo de carga (Hub): la ventana
    // no se congela y los modelos se leen en otro hilo.
    void beginOpenProject(const std::filesystem::path& path);
    // Otra escena del proyecto sin tapar el editor: se lee la escena y los
    // modelos llegan por streaming (hilos de fondo y subida en hilos)
    // mientras la interfaz sigue a la vista y responde; solo sale una barra
    // de carga en la barra de estado.
    void beginOpenScene(const std::filesystem::path& path);
    // Abriendo un proyecto (dialogo de carga a toda la ventana).
    bool projectLoading() const {
        return !project_load_.scene_only && project_load_.stage != ProjectLoad::Stage::Idle &&
               project_load_.stage != ProjectLoad::Stage::Done;
    }
    // Abriendo otra escena del proyecto (barra de carga, sin bloquear).
    bool sceneLoading() const { return project_load_.scene_only && project_load_.stage != ProjectLoad::Stage::Idle; }

    // La interfaz de un frame (entre ImGuiLayer::beginFrame y endFrame).
    void drawUi(float delta_seconds);
    // Lleva el mundo al renderizador (despues de scene.update, antes de
    // drawFrame).
    // `secondary`: la otra vista (Escena y Juego visibles a la vez: se dibujan
    // las dos cada frame, tambien en Play).
    void syncWorld(float delta_seconds, bool secondary = false);
    bool wantsSecondaryView() const { return has_project_ && scene_view_visible_ && game_view_visible_; }

    // La camara del editor solo recibe la entrada mientras se vuela.
    bool sceneWantsInput() const { return flying_; }
    // Despues de renderer.drawFrame: devuelve la camara del editor si este
    // frame se dibujo la vista Juego (con la camara real).
    void afterRender();
    bool quitRequested() const { return quit_; }
    // Cerrar la ventana: pregunta si hay cambios sin guardar.
    void requestQuit();

    // Archivos y carpetas soltados sobre la ventana desde el Explorador: se
    // importan (las carpetas con toda su estructura). `x`, `y` = donde se
    // soltaron (coordenadas de la ventana): encima de una carpeta del
    // navegador, van dentro de ella.
    void onFilesDropped(const std::vector<std::filesystem::path>& files, float x = -1.0f, float y = -1.0f);

    // Prueba automatica de extremo a extremo (CramionEditor.exe --selftest
    // <carpeta> <modelo> <hdr>): crea un proyecto, importa, instancia,
    // selecciona, mueve, deshace, guarda y reabre, a lo largo de varios
    // frames para que el render trabaje de verdad. Escribe "[SelfTest]" en
    // la consola y deja la escena visible al terminar.
    void startSelfTest(const std::filesystem::path& folder, const std::filesystem::path& model,
                       const std::filesystem::path& environment, const std::filesystem::path& image = {});
    bool selfTestFinished() const { return self_test_step_ < 0; }

    // Actualizaciones (EditorUpdates.cpp). El actualizador (CramionUpdater.exe)
    // manda update::kPrepareMessage: el editor guarda todo (escena, prefabs,
    // scripts, Animator), apunta su proyecto para volver a abrirlo y se cierra.
    void requestUpdateShutdown() { update_shutdown_requested_ = true; }

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
        frame_ms_[static_cast<std::size_t>(section)] += milliseconds;
    }
    std::array<float, kCpuSectionCount> cpu_ms_{};
    // Lo mismo sin suavizar, solo de este frame (para el informe de frames lentos).
    std::array<float, kCpuSectionCount> frame_ms_{};
    float frame_tasks_ms_ = 0.0f;    // importaciones, vigilar Assets, miniaturas, MCP
    float frame_refresh_ms_ = 0.0f;  // refrescos de la base de datos de assets
    int frame_refreshes_ = 0;
    double last_slow_report_ = -10.0;
public:
    // Al final de cada frame (main.cpp): si fue lento, escribe en la consola
    // en que se fue el tiempo, parte por parte.
    void reportFrame(float total_ms, float window_ms, float imgui_ms, float scene_update_ms);
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

    // --- MCP (EditorMcp.cpp): IA conectadas al editor ---
    friend class McpTools;
    void startMcp();
    void pollMcp();
    void drawMcpWindow();
    std::string handleMcp(const std::string& body);
    void mcpLog(const std::string& text);
    McpServer mcp_;
    bool show_mcp_ = false;
    std::string mcp_error_;
    std::deque<std::string> mcp_log_;

    // --- Terminal integrada (EditorTerminal.cpp, Terminal.h) ---
    enum class TerminalKind { PowerShell, Cmd, ClaudeCode, Codex, Gemini, InstallClaude };
    void openTerminal(TerminalKind kind);
    void drawTerminalWindow();
    void drawTerminalSession(TerminalSession& session);
    bool show_terminal_ = false;
    std::vector<std::unique_ptr<TerminalSession>> terminals_;
    int terminal_select_ = -1;
    int terminal_cols_ = 120;
    int terminal_rows_ = 30;
    std::string terminal_error_;

    // --- Prefabs (EditorPrefabs.cpp) ---
    // El texto de un .crprefab por su UUID (cache; se vacia al refrescar la base).
    const std::string& prefabText(const Uuid& uuid);
    std::filesystem::path prefabPath(const Uuid& uuid) const;
    // Guarda cada objeto de arriba de la seleccion como prefab en `folder`
    // (Assets/Prefabs si esta vacia); los objetos pasan a ser sus instancias.
    void createPrefabsFromSelection(const std::filesystem::path& folder = {});
    ecs::Entity instantiatePrefabAsset(const Uuid& uuid, ecs::Entity parent,
                                       const std::optional<core::Vec3>& world_position);
    void applyPrefab(ecs::Entity root);
    void revertPrefab(ecs::Entity root);
    void unpackPrefab(ecs::Entity root);
    void selectPrefabInstances(const Uuid& prefab);
    // Instancias que se quedaron en una revision vieja (al abrir la escena o
    // si cambio el .crprefab): se ponen al dia.
    void syncPrefabInstances();
    // Apunta en cada instancia lo que difiere de su prefab (antes de cada
    // instantanea de deshacer).
    void recordPrefabOverrides();
    void drawPrefabInspectorBar(ecs::Entity entity);
    void drawPrefabHierarchyMenu(ecs::Entity entity);

    // --- Espacios de trabajo (EditorWorkspaces.cpp) ---
    // Pestanas debajo del menu, como los editores de assets de Unreal: la
    // Escena (todo lo de siempre) y una pestana por cada prefab o script
    // abierto. Un prefab se edita en su propio escenario (Jerarquia a la
    // izquierda, vista en el centro, componentes a la derecha); al guardarlo
    // se actualizan todas sus instancias de la escena. Solo hay un ecs::World:
    // al cambiar de pestana el mundo que no se ve se guarda en JSON (con su
    // deshacer, seleccion y camara) y se carga el otro.
    enum class WorkspaceKind { Scene, Prefab, Script, StateMachine, Graph };
    // Editores de nodos que se abren en su pestana (una por tipo, a toda la
    // ventana, como los scripts): abrir otro archivo del mismo tipo la reusa.
    enum class GraphKind { Vfx, ShaderGraph, VisualScript, BehaviorTree, Dialogue, Animator, Count };
    struct GraphDoc {
        bool* show = nullptr;   // el editor esta abierto
        bool* focus = nullptr;  // se acaba de pedir (traer su pestana delante)
        bool dirty = false;
        std::string name;
        std::filesystem::path path;
    };
    GraphDoc graphDoc(GraphKind kind);
    void saveGraphDoc(GraphKind kind);
    GraphDoc graphDocVfx();
    GraphDoc graphDocShaderGraph();
    GraphDoc graphDocVisualScript();
    GraphDoc graphDocBehaviorTree();
    GraphDoc graphDocDialogue();
    void syncGraphWorkspaces();
    // Begin() del editor de nodos `kind` a toda la ventana si su pestana es la
    // activa; si no, no dibuja nada (y devuelve false sin Begin).
    bool beginGraphWorkspace(GraphKind kind, const char* title);
    WorkspaceKind activeWorkspaceKind() const;
    // Titulo de un panel: en la Escena tal cual (el diseno de siempre); en
    // otro espacio con un ID propio para acoplarse en su dockspace.
    std::string panelTitle(const char* name) const;
    void openPrefabWorkspace(const Uuid& prefab);
    void openScriptWorkspace(const std::filesystem::path& file);
    // Pestana de la maquina de estados abierta (a toda la ventana, como un script).
    void openStateMachineWorkspace();
    bool savePrefabWorkspace();
    void requestWorkspace(int id);
    void applyPendingWorkspace();
    void activateWorkspace(int id);
    void loadWorkspaceWorld(int id);
    void closeWorkspace(int id, bool save);
    void resetWorkspaces();
    // Deja la Escena delante (antes de abrir/crear escenas, salir, Play).
    void returnToSceneWorkspace();
    void syncScriptWorkspaces();
    void drawWorkspaceBar();
    void drawWorkspacePanels(float delta_seconds);
    void buildPrefabLayout(unsigned int dockspace_id);
    // Pestana de script: el codigo en el centro y a la derecha el arbol de
    // carpetas y assets (clic en un script lo abre).
    void buildScriptLayout(unsigned int dockspace_id);
    void drawScriptFileTree();
    struct FileTreeNode {
        std::filesystem::path path;
        std::string name;
        bool folder = false;
        std::vector<FileTreeNode> children;
    };
    void rebuildFileTree(FileTreeNode& node, int depth);
    bool drawFileTreeNode(const FileTreeNode& node, const std::filesystem::path& active, const std::string& filter);
    FileTreeNode file_tree_;
    std::uint64_t file_tree_version_ = ~0ull;
    double file_tree_time_ = -10.0;
    std::string file_tree_filter_;
    // Banda azul del escenario: que prefab se edita, Guardar y Volver.
    void drawPrefabStageBanner();
    // La raiz del prefab en el escenario activo (vacia fuera de un prefab).
    ecs::Entity prefabStageRoot();
    // Luz y cielo del escenario: no salen en la Jerarquia ni se seleccionan.
    bool isStageHelper(const Uuid& uuid) const;
    // Asset de la pestana de script activa (vacio si no es un script).
    std::filesystem::path activeScriptWorkspace() const;

    // --- Paneles ---
    void drawHub();
    // Hub (EditorHub.cpp).
    void drawHubSidebar();
    void drawHubProjects();
    void drawHubNewProject();
    void drawHubUpdates();
    void drawHubLearn();
    void drawHubUpdateBanner();
    void drawTemplateArt(ImDrawList* draw, ImVec2 a, ImVec2 b, const ProjectTemplate& t);
    void createProjectFromHub();
    void drawSaveTemplateDialog();
    void drawMenuBar();
    void drawAddMenuItems();
    void drawStatusBar();

    // --- Areas al estilo de Blender (EditorAreas.cpp) ---
    // Cada area muestra un solo editor (sin pestanas) con su cabecera: el
    // boton del tipo de editor y lo que el editor anada. Cambiar el tipo
    // trae ese editor al area (el de antes queda detras). Los
    // editores de Propiedades (Objeto, Render, Mundo, Fisica) comparten area
    // y se cambian con las pestanas verticales de la izquierda.
    std::string areaTitle(AreaEditor editor) const;
    // ImGui::Begin del editor en su area (con la cabecera). `header` dibuja lo
    // que el editor pone en su cabecera despues del tipo. Llamar siempre a
    // endArea (tambien si devuelve false, como ImGui::End).
    bool beginArea(AreaEditor editor, bool* p_open, ImGuiWindowFlags flags = 0,
                   const std::function<void()>& header = {});
    void endArea();
    // Pone `to` en el area de `from` (desde el boton del tipo de editor).
    void switchArea(AreaEditor from, AreaEditor to);
    // Trae un editor al frente de su area el proximo frame.
    void showAreaEditor(AreaEditor editor);
    struct AreaFrame {
        AreaEditor editor = AreaEditor::Scene;
        bool child = false;  // contenido en un hijo (pestanas de Propiedades)
    };
    std::vector<AreaFrame> area_stack_;
    std::unordered_map<ImGuiID, ImGuiID> area_front_;  // ventana -> area donde ponerla delante (0: la suya)
    void drawAreaTypeButton(AreaEditor editor);
    // Una ventana que recibe el foco (Ventana > ..., Play, soltada en un
    // area) pasa a ser la que se ve en su area.
    void promoteFocusedArea();
    ImGuiID last_focused_window_ = 0;
    void drawPropertiesTabs(AreaEditor editor);
    // Barra superior: las pestanas de los espacios de trabajo (Escena,
    // prefabs, scripts, grafos) junto a los menus, como en Blender.
    void drawWorkspaceTabs(float right_limit);
    // Barra de estado: lo que hacen los botones del raton aqui (Blender).
    void drawStatusHints();
    // Al dar Play se paso la vista a Juego: al parar vuelve a la Escena.
    bool play_switched_view_ = false;
    // Estante de herramientas, gizmo de navegacion y texto de la vista 3D.
    void drawViewportShelf();
    void drawNavigationGizmo();
    void drawViewportInfo();
    // La cabecera de la vista 3D (Vista, Seleccionar, Anadir, Objeto y las
    // opciones a la derecha) y la de la vista Juego.
    void drawSceneHeader();
    void drawGameHeader();
    // Orbita de la vista 3D (gizmo de navegacion): alrededor de la seleccion
    // o de un punto delante de la camara.
    core::Vec3 viewPivot();
    void alignView(int axis);
    void orbitView(float dx, float dy);
    // Donde va lo que se dibuja encima de la vista 3D (estante, gizmo de
    // navegacion y sus botones): con el raton encima no se selecciona ni se
    // mueve la camara.
    struct ViewportOverlays {
        ImVec2 shelf{};
        float tool = 0.0f;
        int tools = 0;
        ImVec2 gizmo{};
        float gizmo_radius = 0.0f;
        float button_radius = 0.0f;
        bool navigation = false;
    };
    ViewportOverlays viewport_overlays_;
    void layoutViewportOverlays(ImVec2 origin, ImVec2 size);
    bool viewportOverlayHit(ImVec2 point) const;
    // Ancho de lo alineado a la derecha en las cabeceras (medido el frame anterior).
    float scene_header_right_w_ = 0.0f;
    float game_header_right_w_ = 0.0f;
    // Vista 2D activa (Editor2D.cpp): sin gizmo de navegacion.
    bool view2DActive();
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
    // Presets del componente (de fabrica y del proyecto) para esas entidades.
    void drawComponentPresets(const std::string& component, const std::vector<ecs::Entity>& targets);
    bool preset_save_request_ = false;
    std::string preset_save_component_;
    std::string preset_save_name_;
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
    void overlaySolidFace(const core::Vec3& a, const core::Vec3& b, const core::Vec3& c,
                          const core::Vec3& outward, std::uint32_t color);
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
    // Navegacion (EditorNavigation.cpp).
    void loadNavigationSettings();
    void saveNavigationSettings();
    void updateNavigation(float delta_seconds);
    void drawNavigationGizmos();
    void drawNavigationWindow();
    void drawNavigationStats();
    void drawNavMeshBoundsInspector(ecs::Entity entity);
    void drawNavigationCreateMenu();
    ecs::Entity createNavigationEntity(int kind);  // 0 volumen, 1 modificador
    bool navigationSceneBounds(core::Vec3& lo, core::Vec3& hi) const;
    void fitNavBoundsToScene(ecs::Entity volume);
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

    // --- Terreno (EditorTerrain.cpp) ---
    ecs::Entity createTerrainEntity();
    // Vegetacion (foliage::Foliage): un bosque sobre el terreno que haya
    // (su mismo cuadrado) o de 2 km alrededor del origen.
    ecs::Entity createFoliageEntity();
    ecs::Entity terrainUnderMouse(float x, float y, core::Vec3* point = nullptr) const;
    bool drawTerrainTool(float delta_seconds);
    void drawTerrainInspector(ecs::Entity entity);

    // --- Scripting y audio (EditorScripting.cpp) ---
public:
    void setInput(const dm::Input* input) { input_ = input; }
    // --play-vr: al terminar de abrir el proyecto, Play en el casco.
    void requestVrPlayOnStart() { pending_vr_play_ = true; }
    // Al cerrar, antes de renderer.shutdown(): espera a la busqueda del casco
    // (si hay una) y lo suelta.
    void shutdownXr();
    // Realidad virtual en Play (EditorXr.cpp), desde el bucle de main.cpp:
    // beginXrFrame antes de la interfaz y la logica, renderXrEyes despues de
    // syncWorld y endXrFrame despues de drawFrame.
    void beginXrFrame(dm::Input& input);
    void renderXrEyes();
    void endXrFrame();
    // Todos los eventos de la ventana (main.cpp) y el fin de cada frame: la
    // simulacion tactil lleva su propia entrada.
    void onRawInputEvent(const dm::Event& e);
    void endInputFrame();
private:
    struct ScriptTab {
        std::filesystem::path path;
        std::string relative;  // dentro de Assets
        std::string text;
        std::string saved;
        bool select = false;
        bool focus = false;
        int goto_line = -1;
        int cursor = 0;
        int set_cursor = -1;  // mover el cursor del campo el proximo frame
        bool reload_text = false;  // el texto cambio por fuera (formatear) con el campo activo
        // C++: la ultima lista de clangd sin filtrar (se filtra al momento al teclear,
        // sin esperar a clangd: la lista no salta) y donde empezaba su palabra.
        std::vector<LuaCompletion> cpp_raw;
        std::size_t cpp_raw_start = std::string::npos;
        float popup_width = 0.0f;  // solo crece mientras esta abierta (no tiembla)
        int line = 1;
        int column = 1;
        // Autocompletado.
        bool completion_open = false;
        int completion_selected = 0;
        std::vector<LuaCompletion> completions;
        LuaCompletionContext completion_context;
        bool was_active = false;
        double last_edit = -10.0;  // el cursor no parpadea mientras se escribe
        // Codigo que no es un archivo (el de un estado de una maquina de
        // estados): Ctrl+S llama a esto en lugar de escribir `path`.
        std::function<void()> on_save;
        // C++ (.cpp, .h): IntelliSense de clangd.
        bool cpp = false;
        std::string clangd_synced;          // el texto que tiene clangd
        std::uint64_t completion_request = 0;
        std::vector<ClangdSignature> signatures;
        int signature_active = 0;
        bool signature_open = false;
        ImVec2 hover_mouse{};
        double hover_since = 0.0;
        int hover_offset = -1;              // donde se pidio la informacion del raton
        std::string hover_text;
    };
    // IntelliSense de C++ (Clangd.h): se arranca al abrir el primer .cpp.
    ClangdClient clangd_;
    void ensureClangd();
    void syncClangd(ScriptTab& tab);
    ScriptTab* scriptTabFor(const std::filesystem::path& file);
    void goToDefinition(ScriptTab& tab, int offset);
    // IntelliSense: la API del motor y lo del proyecto (EditorLuaSymbols.cpp).
    void refreshLuaSymbols();
    double lua_symbols_time_ = -1.0;
    std::string assetRelative(const std::filesystem::path& file) const;
    void createScriptAsset(const std::filesystem::path& folder, ecs::Entity attach_to);
    // --- Shaders de superficie (EditorShaders.cpp) ---
    std::filesystem::path createShaderAsset(const std::filesystem::path& folder);
    const std::vector<std::string>& projectShaderFiles();
    void drawMaterialShaderSection(assets::MaterialAsset& m, bool& changed, bool& structural);
    std::vector<std::string> shader_files_;  // .crshader de Assets (se rehace con la base de datos)
    std::uint64_t shader_files_version_ = ~0ull;
    void openScript(const std::filesystem::path& file);
    bool saveScript(ScriptTab& tab);
    void drawScriptEditor();
    void drawCodeEditor(ScriptTab& tab);
    void drawScriptInspector(ecs::Entity entity);
    void drawCppScriptInspector(ecs::Entity entity, bool header);  // EditorCppScripts.cpp (arriba / abajo)
    std::vector<std::string> cppClassesOfFile(const std::string& relative);
    std::string cppSourceFor(const std::string& relative);  // .h -> su .cpp
    // C++: formatear (clang-format), el .h/.cpp de al lado, su consola y la
    // ventana Configuracion del motor (EditorCppScripts.cpp).
    bool formatCppTab(ScriptTab& tab, bool quiet = false);
    static bool cppFormatOnSave();
    std::string cppFormatStyle() const;
    std::filesystem::path cppCounterpart(const std::filesystem::path& file) const;  // .h <-> .cpp
    void drawCppToolbar(ScriptTab& tab);
    void drawCppConsole(ScriptTab& tab);
    void drawEngineSettingsWindow();
    bool show_engine_settings_ = false;
    struct CppConsoleLine {
        int level = 0;  // 0 info, 1 aviso, 2 error
        std::string text;
        std::string file;  // dentro de Assets (para ir a la linea)
        int line = 0;
    };
    std::vector<CppConsoleLine> cpp_console_;
    std::uint64_t cpp_console_read_ = 0;  // entradas de EditorLog ya miradas
    std::string cpp_console_input_;
    bool cpp_console_scroll_ = false;
    float cpp_console_height_ = 150.0f;
    std::string mcp_type_queue_;  // MCP cpp_intellisense 'type': un caracter por frame (como si se tecleara)
    void drawAudioInspector(ecs::Entity entity);
    void updateScriptsAndAudio(float delta_seconds, int physics_steps);
    const dm::Input* input_ = nullptr;
    scripting::ScriptSystem scripts_;
    audio::AudioSystem audio_;
    std::vector<ScriptTab> script_tabs_;
    int active_script_tab_ = -1;
    bool show_script_editor_ = false;
    bool focus_script_editor_ = false;
    unsigned int script_dock_id_ = 0;  // donde se acoplan los scripts nuevos (junto a los abiertos)
    void drawScriptToolbar(ScriptTab* tab);
    void drawLuaConsole();
    std::string lua_console_;
    std::vector<std::filesystem::path> current_scripts_;  // .lua de la carpeta
    std::vector<std::filesystem::path> current_shaders_;  // .crshader de la carpeta
    std::vector<std::filesystem::path> current_audio_;    // audios de la carpeta

    // --- Exportar el juego (EditorExport.cpp): copia en otro hilo con progreso ---
    struct ExportJob {
        struct Copy {
            std::filesystem::path from;
            std::filesystem::path to;
        };
        std::vector<Copy> files;
        // Assets, ProjectSettings y el .crproj: comprimidos en Game/<Juego>.crpack.
        std::vector<project::PackInput> pack;
        std::filesystem::path pack_file;
        std::thread thread;
        std::atomic<std::uint64_t> done{0};
        std::atomic<std::uint64_t> total{0};
        std::atomic<bool> cancel{false};
        std::atomic<bool> finished{false};
        std::mutex mutex;
        std::string current;
        std::string error;
        std::filesystem::path target;
        std::filesystem::path exe;
        std::filesystem::path game_folder;
        std::string game_ini;
        std::string scene_name;
        bool run_after = false;
        // Static batching: cada .crscene del paquete se combina en el hilo
        // (copia de la escena + su lote en Library/ExportCache/StaticBatches).
        bool static_batching = true;
        std::filesystem::path assets_root;
        std::filesystem::path batch_cache;
        std::string batch_summary;
        std::filesystem::path icon;  // imagen o .ico de la configuracion (vacia = el del motor)
        // Texturas ya comprimidas (BC1/BC7) al paquete, en TextureCache/: el
        // juego no las comprime y sin ellas irian en RGBA8 (4 a 8 veces mas
        // VRAM). Las de los materiales; las de los modelos salen de sus .crdata.
        bool compress_textures = false;
        std::vector<asset::TextureData> material_textures;
        // Android: tras el paquete, el APK/AAB (AndroidBuild) y, si se pidio,
        // instalar y abrir en el dispositivo elegido.
        bool android = false;
        AndroidToolchain android_tools;
        AndroidPackageInput android_input;
        AndroidPackageResult android_result;
        std::string android_device;
        std::string android_notes;  // avisos para el mensaje final (no paran la exportacion)
        std::atomic<float> phase{-1.0f};  // >= 0: progreso del empaquetado (en vez de los bytes)
    };
    void exportGame(bool run_after);
    void drawExportProgress();
    void cancelExport();
    void startExport(const std::filesystem::path& parent);
    std::unique_ptr<ExportJob> export_job_;
    std::string export_message_;
    // Ventana previa: carpeta de destino (escrita o con Examinar) y ejecutar al terminar.
    bool export_setup_ = false;
    bool export_run_after_ = false;
    bool export_static_batching_ = true;
    // Componente Profiler: medidas por frame y su dibujo en la vista Juego.
    ProfilerOverlay profiler_overlay_;
    // Casilla Static del Inspector: valor a aplicar a los hijos si se acepta.
    bool static_children_value_ = false;
    std::string export_folder_;
    std::shared_ptr<dialogs::AsyncFolderPick> export_pick_;

    // --- DataPacks (EditorDataPack.cpp): escenas con todo lo que usan ---
    void openDataPackExport();
    void drawDataPackWindow();
    // Sin ventana (MCP): escribe `file` con `scenes` y sus dependencias.
    bool exportDataPack(const std::vector<std::filesystem::path>& scenes, const std::filesystem::path& file,
                        const std::string& name, std::string& message, std::size_t* file_count = nullptr);
    struct DataPackJob {
        std::thread thread;
        std::atomic<std::uint64_t> done{0};
        std::uint64_t total = 0;
        std::atomic<bool> finished{false};
        std::atomic<bool> cancel{false};
        bool ok = false;
        std::string message;
        std::filesystem::path file;
        std::mutex mutex;
        std::string current;  // archivo que se esta comprimiendo
    };
    bool show_datapack_ = false;
    std::string datapack_name_;
    std::string datapack_folder_;
    std::vector<std::filesystem::path> datapack_scenes_;  // escenas elegidas
    std::shared_ptr<dialogs::AsyncFolderPick> datapack_pick_;
    std::unique_ptr<DataPackJob> datapack_job_;
    std::string datapack_message_;
    bool datapack_done_ = false;  // la ventana muestra el resultado
    // Clic derecho en la Jerarquia > Empaquetar y exportar como DataPack.
    void openDataPackExportFor(ecs::Entity entity);
    std::filesystem::path prefabForDataPack(ecs::Entity entity, std::string& message);
    // Exporta en otro hilo con la ventana de progreso (boton Exportar y MCP async).
    void startDataPackJob(const std::vector<std::filesystem::path>& scenes, const std::filesystem::path& file,
                          const std::string& name);
    // Android: dispositivos de adb para "Exportar y jugar" y el elegido.
    std::vector<std::string> export_devices_;
    std::string export_device_;
    void refreshAndroidDevices();
    // Seccion Android de la ventana de configuraciones (EditorAndroidBuild.cpp).
    bool drawAndroidBuildSettings(BuildConfig& config);
    // --- Interfaz tactil del proyecto (EditorTouchInterface.cpp) ---
    void loadTouchInterface();
    void drawTouchInterfaceWindow();
    void drawTouchSimulation(const ImVec2& origin, const ImVec2& size);  // vista Juego en Play
    const dm::Input* touchSimulatedInput();  // la entrada del juego con la simulacion
    bool show_touch_interface_ = false;
    dm::TouchLayout touch_layout_;
    std::filesystem::path touch_layout_file_;
    bool touch_preview_portrait_ = false;
    bool touch_simulate_ = false;
    dm::TouchControls touch_game_;       // la del juego en Play (Lua la cambia)
    dm::Input touch_sim_input_;          // teclado real + lo que sale del tactil
    std::vector<dm::Event> touch_pending_;
    bool touch_sim_down_ = false;
    int touch_button_dragged_ = -1;
    // --- Entrada del proyecto: acciones y contextos (EditorInputActions.cpp) ---
    void loadInputActions();
    void drawInputActionsWindow();
    void saveInputActionsNow();
    bool show_input_actions_ = false;
    input::InputActionSettings input_actions_;
    std::filesystem::path input_actions_file_;
    int input_action_selected_ = 0;
    int input_context_selected_ = 0;
    int input_capture_mapping_ = -1;  // esperando una tecla para esa fila
    int input_tab_ = 0;               // 0 acciones, 1 contextos
    // --- Configuraciones de compilacion (EditorBuildConfigs.cpp) ---
    // Perfiles de exportacion: nombre del juego, version, icono del .exe,
    // escena inicial, ventana. ProjectSettings/BuildConfigs.json.
    void ensureBuildConfigs();
    void saveBuildConfigsNow();
    void drawBuildConfigsWindow();
    // Nombre del juego de la configuracion activa (o el del proyecto).
    std::string buildGameName();
    // Ruta absoluta del icono de una configuracion (vacia = el del motor).
    std::filesystem::path buildIconPath(const BuildConfig& config) const;
    BuildConfigs build_configs_;
    std::filesystem::path build_configs_file_;  // de que proyecto son
    bool show_build_configs_ = false;
    int build_config_selected_ = 0;

    // --- Interfaz del juego (EditorUI.cpp) ---
    void drawGameUi(ImVec2 origin, ImVec2 size);
    void updateWorldUi();     // Canvas en modo Mundo (VR): cada frame
    void dispatchUiEvents();  // eventos de la UI a los scripts
    ecs::Entity uiParentForNewElement();
    ecs::Entity createUiElement(int kind);  // 0 Canvas, 1 panel, 2 imagen, 3 texto, 4 boton, 5 slider, 6 campo, 7 casilla
    void drawUiCreateMenu();
    void drawRectTransformInspector(ecs::Entity entity);
    ui::UiSystem ui_;
    struct UiDrag {
        bool active = false;
        Uuid entity{};
        int mode = 0;  // 0 mover, 1..4 esquinas
        ImVec2 start_mouse{};
        core::Vec2 start_position{};
        core::Vec2 start_size{};
        float scale = 1.0f;
    } ui_drag_;

    // --- Mundo de bloques (EditorVoxel.cpp) ---
    ecs::Entity createVoxelWorldEntity();
    void updateVoxels(float delta_seconds);
    // --- Origen flotante (EditorOrigin.cpp) ---
    // Cada frame: si la camara se alejo, el mundo se desplaza.
    void updateFloatingOrigin();
    // Los sistemas y la camara del editor, -offset (y el mundo si move_world).
    void applyOriginShift(const core::Vec3& offset, bool move_world);
    // Tras cargar/deshacer/salir de Play: si el origen cambio, se alinea lo
    // demas (las entidades ya vienen en el origen nuevo).
    void alignOriginAfterLoad(const ecs::DVec3& before);
    ecs::FloatingOriginSettings floating_origin_{};
    void startVoxels();
    void stopVoxels();
    core::Vec3 voxelViewer();

    // --- Ambiente: clima, hora, estaciones y viento (EditorEnvironment.cpp) ---
    bool show_environment_window_ = false;
    ecs::Entity createEnvironmentEntity();
    void drawEnvironmentInspector(ecs::Entity entity);
    void drawEnvironmentWindow();

    // --- Fuego (EditorFire.cpp) ---
    ecs::Entity createFireEntity(const core::Vec3* position = nullptr);
    void drawFireInspector(ecs::Entity entity);
    bool drawFireTool();  // contorno de la zona y "Encender con clic"; true si se queda el clic
    bool fire_click_ignite_ = false;
    float fire_click_radius_ = 2.0f;

    // --- Agua (EditorWater.cpp) ---
    ecs::Entity createWaterEntity(int kind);  // 0 oceano, 1 lago, 2 rio
    void drawWaterInspector(ecs::Entity entity);
    bool drawWaterGizmos();  // true si el raton esta sobre un asa
    bool groundHeightAt(float x, float z, float& height) const;
    void snapRiverToTerrain(ecs::Entity entity);
    struct WaterDrag {
        bool active = false;
        Uuid entity{};
        int point = -1;
        float plane_y = 0.0f;
    } water_drag_;

    // --- Informe del cierre anterior (EditorCrashReport.cpp) ---
    void initCrashReporting();
    void drawCrashReportWindow();
    std::filesystem::path crash_report_path_;
    std::string crash_report_text_;
    bool crash_report_open_ = false;

    // --- Multitudes (CrowdSpawner) ---
    ai::CrowdSystem crowds_;

    // --- World Partition (EditorWorldPartition.cpp) ---
    worldpart::WorldPartitionSystem world_partition_;
    void drawWorldPartitionInspector(ecs::Entity entity);
    void drawWorldPartitionGrid();
    int buildHlods(const worldpart::WorldPartition& wp);  // celdas con HLOD creado

    // --- Splines (EditorSplines.cpp) ---
    // shape: 0 carretera, 1 camino, 2 rio, 3 muro, 4 valla, 5 tuberia,
    // 6 railes, 7 cinta, -1 solo la curva.
    ecs::Entity createSplineObject(int shape);
    ecs::Entity createFogVolumeEntity();  // niebla local (FogVolume)
    void drawSplineInspector(ecs::Entity entity);
    bool drawSplineGizmos();  // true si el raton esta sobre un asa
    struct SplineDrag {
        bool active = false;
        Uuid entity{};
        int point = -1;
        float plane_y = 0.0f;
    } spline_drag_;

    // --- Configuracion grafica (ProjectSettings/Graphics.ini) ---
    void loadGraphicsSettings();
    void saveGraphicsSettings() const;
    void drawGraphicsSettings();

    // --- Barra de titulo propia (sin la de Windows) ---
public:
    // Zona de arrastre de la ventana (la consulta dm::Window en WM_NCHITTEST).
    bool isCaptionDragArea(int x, int y) const;
private:
    void drawWindowControls();
    void drawHubTitleBar();
    void drawCaptionLogo();
    float caption_height_ = 0.0f;
    std::vector<ImVec4> caption_blockers_;  // min x, min y, max x, max y (no arrastran)

    // --- Materiales (EditorMaterials.cpp) ---
    void drawMaterialBall(ImDrawList* draw, ImVec2 center, float radius, const Uuid& material);
    // `image` (ruta en Assets): material a partir de esa textura y sus
    // companeras; `from`: con los valores de un material del modelo.
    void createMaterialAsset(const std::filesystem::path& folder, const std::string& image = {},
                             const asset::MaterialData* from = nullptr);
    int materialSlotCount(ecs::Entity entity) const;
    bool applyMaterial(ecs::Entity entity, const Uuid& material, int slot);
    // Clic derecho en uno o varios modelos del Proyecto: un .crmat por
    // material con sus texturas (EditorModelMaterials.cpp), guardado como
    // los materiales del modelo y puesto en sus instancias de la escena.
    // `replace_in` (Find And Build del Mesh Renderer): en esos objetos se
    // cambian todos los huecos, no solo los vacios.
    void createModelMaterials(const std::vector<Uuid>& models, const std::vector<ecs::Entity>& replace_in = {});
    // Los .crmat guardados del modelo en los huecos vacios de `root` y sus
    // hijos (al ponerlo en la escena). `replace`: solo `root`, y tambien los
    // huecos que ya tienen material. true si puso alguno.
    bool applyModelMaterials(ecs::Entity root, const Uuid& model, bool replace = false);
    bool materialTextureSlot(const char* label, std::string& path);
    void flushMaterialEdit(bool force_structure);
    void drawMaterialEditor(const Uuid& uuid);
    void drawMeshMaterials(ecs::Entity entity);
    // Render Texture (.crrt) elegida en el Proyecto: su Inspector.
    Uuid inspected_render_texture_{};
    Uuid render_texture_edit_uuid_{};
    assets::RenderTextureAsset render_texture_edit_{};
    std::filesystem::file_time_type render_texture_edit_stamp_{};
    void drawRenderTextureEditor(const Uuid& uuid);
    void createRenderTextureAsset(const std::filesystem::path& folder);
    Uuid inspected_material_{};     // material elegido en el Proyecto (el Inspector lo muestra)
    // Clic en un material del navegador: se abre al soltar si no se arrastro.
    Uuid pending_inspect_material_{};
    Uuid inline_material_{};        // el que se edita al pie del Mesh Renderer
    Uuid last_created_material_{};
    assets::MaterialAsset material_edit_{};
    Uuid material_edit_uuid_{};
    std::filesystem::path material_edit_path_;
    bool material_unsaved_ = false;
    std::uint64_t material_pushed_structure_ = 0;
    void pushTerrainUndo(const std::string& path, std::shared_ptr<terrain::TerrainData> before);
    void applyTerrainSnapshot(const std::string& path, const terrain::TerrainData& snapshot);

    // --- Vista Juego y cinematicas (EditorCinematics.cpp) ---
    static constexpr std::uint32_t kSceneSlot = 0;
    static constexpr std::uint32_t kGameSlot = 1;
    void drawGameView();
    void chooseRenderView();
    // --- Proporcion de las vistas (menu "Free Aspect" de Unity) ---
    // Rectangulo de la imagen de la vista `slot` dentro de `avail` (y el
    // tamano de render que quiere esa vista).
    ImVec2 layoutViewImage(std::uint32_t slot, ImVec2 avail, ImVec2& origin);
    void drawAspectMenu(std::uint32_t slot);
    // Pide al renderer el tamano de la vista que se dibuja (al final de la UI).
    void updateViewExtent(float delta_seconds);
    void loadViewSettings();
    void saveViewSettings() const;
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
        Uuid material{};  // soltar un material: se pone en el hueco bajo el raton
        std::string script;  // soltar un script: se engancha al objeto bajo el raton
    } pending_pick_;
    void handleCameraControls();

    void runSelfTestStep();
    void printCpuTimings(const char* when);

    // --- Importacion en segundo plano ---
    void startImport(const std::vector<std::filesystem::path>& files,
                     const std::filesystem::path& folder);
    void pollImports();
    // Ventana flotante con la barra de cada importacion (archivo y etapa).
    void drawImportProgress();
    // Miniatura del proyecto para el Hub (EditorThumbnail.cpp): captura de la
    // vista Escena en Library/thumbnail.png. Se hace al guardar la escena y
    // unos frames despues de abrir el proyecto (`thumbnail_countdown_`).
    void saveProjectThumbnail();
    int thumbnail_countdown_ = 0;
    struct ImportJob;  // mas abajo
    // --- Reimportar modelos (EditorReimport.cpp) ---
    // Vuelve a importar un modelo desde su archivo original, combinando sus
    // piezas por material si tiene muchas, y al terminar rehace sus
    // instancias en la escena abierta. Devuelve false si no se puede.
    // `settings`: los de importacion nuevos (el panel del Inspector); sin
    // ellos, los que tenia el modelo.
    bool startReimport(const Uuid& model, std::optional<assets::ModelImportSettings> settings = std::nullopt);
    // Ajustes de importacion de un modelo elegido en el Proyecto (como la
    // pestana Model de Unity): Scale Factor, Convert Units... y Aplicar.
    void drawModelImportSettings(const Uuid& model);
    Uuid inspected_model_{};
    Uuid inspected_model_active_{};            // la seleccion de la escena al elegirlo
    Uuid model_import_loaded_{};               // de que modelo son los ajustes en edicion
    assets::ModelImportSettings model_import_edit_{};
    assets::ModelImportSettings model_import_saved_{};
    float model_import_height_ = 0.0f;         // alto del modelo importado (m)
    std::string model_import_source_;
    void finishReimport(const ImportJob& job, const assets::ImportResult& result);
    // Cambia la jerarquia de cada instancia del modelo por la nueva (conserva
    // la raiz: su Transform, nombre, padre, componentes, Static y los .crmat
    // por nombre de material). Devuelve cuantas instancias rehizo.
    int rebuildModelInstances(const Uuid& model, const std::vector<std::vector<std::string>>& old_materials);
    // Vigila Assets/ (archivos nuevos, copiados, borrados o movidos desde
    // fuera) y refresca la base de datos sola.
    void watchAssets();
    void createSceneAsset(const std::filesystem::path& folder);
    // Copia/importa una carpeta del disco (y sus subcarpetas) dentro de Assets.
    void importFolder(const std::filesystem::path& source, const std::filesystem::path& target);
    std::filesystem::path createFolderIn(const std::filesystem::path& parent);
    // Carpetas dibujadas en el navegador (rectangulo en pantalla -> ruta),
    // para saber sobre cual se suelta algo desde el Explorador.
    struct FolderDropZone {
        float x0, y0, x1, y1;
        std::filesystem::path path;
    };
    std::vector<FolderDropZone> folder_drop_zones_;
    std::filesystem::path renaming_folder_;
    std::string folder_rename_buffer_;

    // --- Animator (EditorAnimator.cpp) ---
    void createAnimatorAsset(const std::filesystem::path& folder);
    void openAnimatorEditor(const Uuid& uuid);
    void saveAnimatorEditor();
    // Blend Tree del estado seleccionado (EditorAnimator.cpp).
    void drawBlendTreeEditor(ecs::AnimatorState& state, const asset::ModelData* data, ecs::Animator* live);
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
    void extractClips(const asset::ModelData& data, int clip);
    // Menu del asset de modelo en el Proyecto: extraer sus clips a .cranim
    // (se lee el modelo la primera vez que se abre el menu).
    void drawModelAssetAnimationsMenu(const assets::AssetInfo& info);
    std::shared_ptr<const assets::ModelAsset> clip_source_;
    Uuid clip_source_uuid_{};
    ModelPreviews model_previews_;

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
    // Importacion de texturas (como Unity): al abrir el proyecto, en segundo
    // plano, se comprimen a BC7 las imagenes y las texturas de los materiales
    // que no esten ya en Library/Cache/Textures. Al abrir una escena ya solo
    // se leen. (EditorProjectLoad.cpp)
    struct TextureImport {
        std::vector<asset::TextureData> items;
        std::vector<std::thread> threads;
        std::atomic<bool> stop{false};
        std::atomic<std::size_t> next{0};
        std::atomic<std::size_t> done{0};
        std::atomic<std::size_t> compressed{0};
        std::chrono::steady_clock::time_point started;
        bool reported = false;
        ~TextureImport() {
            stop = true;
            for (std::thread& thread : threads) {
                if (thread.joinable()) thread.join();
            }
        }
    };
    std::unique_ptr<TextureImport> texture_import_;
    void startTextureImport();
    void stopTextureImport() { texture_import_.reset(); }
    // Texto de la barra de estado mientras se importa ("" si no).
    std::string textureImportStatus();
    ecs::World world_;
    std::filesystem::path scene_path_;  // vacia = escena sin guardar
    bool dirty_ = false;

    // Seleccion (por UUID: sobrevive a deshacer, que recrea las entidades).
    std::vector<Uuid> selection_;
    // Clic sin Ctrl/Mayus sobre una fila ya seleccionada con varias: se
    // deja solo esa al soltar (si no se arrastro), como Unity; asi se puede
    // arrastrar toda la seleccion.
    Uuid pending_select_only_{};
    bool quit_play_requested_ = false;
    Uuid active_{};
    Uuid reveal_{};
    std::vector<HierarchyRow> hierarchy_rows_;  // filas de la jerarquia este frame (Shift+clic)
    Uuid renaming_{};
    std::string rename_buffer_;
    bool rename_focus_ = false;
    std::string hierarchy_filter_;
    std::vector<std::string> clipboard_;
    // Prefabs: texto de cada .crprefab leido (se vacia con database_version_).
    std::unordered_map<std::string, std::string> prefab_texts_;
    std::uint64_t prefab_texts_version_ = ~0ull;

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
    // Volando, el cursor se oculta y se fija en el centro de la vista (el giro
    // llega en bruto): no choca con el borde. Al soltar vuelve donde estaba.
    void syncFlyCursor();
    bool fly_cursor_captured_ = false;
    POINT fly_cursor_restore_{};
    GizmoOperation gizmo_ = GizmoOperation::Translate;
    bool gizmo_local_ = false;
    // Pivote (el origen del objeto activo) o Centro (el de la caja de toda la
    // seleccion, con sus hijos), como el boton Pivot/Center de Unity (Z).
    bool gizmo_center_ = false;
    core::Mat4 gizmo_handle_ = core::Mat4::identity();  // donde esta el gizmo en modo Centro (fijo al arrastrar)
    // Caja de lo seleccionado (sus actores y los de sus hijos; sin actores,
    // las posiciones). false si no hay seleccion. La usan F y el modo Centro.
    bool selectionBounds(core::Vec3& low, core::Vec3& high);
    bool gizmo_was_using_ = false;
    // Iconos y ayudas de la vista (luces, camaras, fisica...); el gizmo de
    // mover/rotar/escalar se queda siempre, como el boton Gizmos de Unity.
    bool show_gizmos_ = true;
    bool snap_enabled_ = false;
    float snap_translate_ = 0.25f;
    float snap_rotate_ = 15.0f;
    float snap_scale_ = 0.1f;
    float last_pick_x_ = -1.0f;
    gfx::OverlayGeometry overlay_;
    // true mientras se dibuja el gizmo de transformar: va a la parte "top"
    // del overlay (sin prueba de profundidad, siempre encima).
    bool overlay_on_top_ = false;
    // Tamano del gizmo en espacio de clip (ImGuizmo::SetGizmoSizeClipSpace).
    static constexpr float kGizmoClipSize = 0.13f;
    // Giro libre con la bola central del gizmo de rotar (radio en tamanos de gizmo).
    static constexpr float kFreeRotateRadius = 0.45f;
    bool free_rotate_hover_ = false;
    bool free_rotate_drag_ = false;
    bool drawFreeRotateHandle(ecs::Entity target, const core::Vec3& origin);
    // Triangulos de los Mesh Collider (caros de sacar de Jolt): se guardan
    // por entidad y se rehacen si se mueve o cada cierto tiempo.
    struct ColliderWire {
        core::Mat4 matrix{};
        int frame = 0;
        std::vector<core::Vec3> triangles;
    };
    std::unordered_map<std::uint32_t, ColliderWire> collider_wire_cache_;
    int light_handle_drag_ = 0;  // 0 nada, 1 alcance, 2 angulo exterior, 3 angulo interior
    // Navegador: modelo o clip humanoide (insignia) y su cache.
    std::unordered_map<std::string, bool> clip_humanoid_;
    // --- Carga del proyecto por etapas (EditorProjectLoad.cpp) ---
    struct ProjectLoad {
        // Proyecto: Open, Scene, Models, Upload, Physics. Escena (scene_only):
        // Scene y Stream (los modelos llegan con el editor funcionando).
        enum class Stage { Idle, Open, Scene, Models, Upload, Physics, Stream, Done };
        Stage stage = Stage::Idle;
        std::filesystem::path path;
        std::string project_name;
        bool scene_only = false;           // solo una escena (beginOpenScene)
        std::filesystem::path scene_path;  // la escena que se abre
        std::vector<Uuid> models;
        std::future<void> worker;
        std::atomic<std::size_t> models_done{0};
        std::mutex mutex;
        std::string current;  // modelo que lee el hilo
        int frames = 0;       // frames en la etapa (el texto se ve antes de bloquear)
        float shown = 0.0f;   // progreso dibujado (va suave hacia el real)
        float target = 0.0f;  // progreso real (no retrocede)
        float time = 0.0f;
        int captured_stage = -1;  // pruebas (CRAMION_CAPTURE_LOADING)
        std::chrono::steady_clock::time_point started{};
        void reset() {
            if (worker.valid()) worker.wait();
            stage = Stage::Idle;
            path.clear();
            project_name.clear();
            scene_only = false;
            scene_path.clear();
            models.clear();
            worker = {};
            models_done = 0;
            current.clear();
            frames = 0;
            shown = 0.0f;
            target = 0.0f;
            time = 0.0f;
            captured_stage = -1;
        }
    };
    ProjectLoad project_load_;
    void openStartupScene();
    void stepProjectLoad();
    void projectLoadProgress(float& fraction, std::string& text);
    void drawProjectLoading(float delta_seconds);
    // La barra de carga de otra escena (barra de estado): avanza `shown` y
    // termina la carga un momento despues de llegar al 100 %.
    void updateSceneLoading(float delta_seconds);
    void drawSceneLoadingBar(float width);
    // --- Volumenes de post-proceso (EditorPostVolumes.cpp) ---
    ecs::Entity createPostVolume(int shape, ecs::Entity parent);  // 0 global, 1 caja, 2 esfera
    // --- Realidad virtual (EditorXr.cpp) ---
    ecs::Entity createXrOrigin(ecs::Entity parent);
    ecs::Entity createXrPlayer(ecs::Entity parent);
    void drawXrControllerInspector(ecs::Entity entity);  // ver/quitar el mando de Quest 3  // XR Origin con CharacterController y XrPlayer
    void drawXrMenu();
    xr::XrRig xr_rig_;
    bool xr_frame_ = false;
    bool xr_play_preference_ = xrPlayPreference();
    int xr_runtime_preference_ = xrRuntimePreference();  // xr::RuntimeChoice
    bool xr_stereo_preference_ = xrStereoPreference();
    bool play_vr_ = false;          // este Play va al casco (boton Play on VR)
    bool pending_vr_play_ = false;  // reiniciado con --play-vr: Play en VR al cargar
    // Play on VR conecta el casco en otro hilo (SteamVR puede tardar en
    // abrir): mientras tanto nadie toca renderer_.xr().
    std::future<bool> xr_connect_;
    bool xr_connect_then_play_ = false;
    std::string xr_status_;                 // por que no se pudo conectar (ventana de Play on VR)
    bool xr_status_needs_restart_ = false;  // el casco pide otro Vulkan: reabrir
    bool xr_status_popup_ = false;          // abrir esa ventana en el proximo frame
    bool xrConnecting() const { return xr_connect_.valid(); }
    void startXrConnect(bool then_play);
    void finishXrConnect();
    void drawXrStatusPopup();
    void drawPlayVrButton(float height);
    void enterPlayVr();
    bool restartForVr();
    void drawPostVolumeGizmos();
    // Audio (EditorAudio.cpp): alcance, zonas de reverberacion y oclusion en Play.
    void drawAudioGizmos();
    // Esqueletos, IK de animales, ragdoll y phys bones (EditorRigging.cpp).
    bool rigPose(ecs::Entity entity, ecs::RenderSync::SkeletonPose& pose, bool search_up);
    void drawRigInspector(const std::string& type, ecs::Entity entity);
    // Huesos como objetos: una entidad por hueso (con su jerarquia) que mueve
    // el hueso (BoneSocket en modo Mover), para posar con el gizmo o desde la
    // Jerarquia (las manos de un modelo de RV). Devuelve cuantas creo.
    int createBoneObjects(ecs::Entity entity);
    bool drawRigGizmos();  // true si el raton esta sobre una articulacion
    // --- Scripts de C++ aislados y CVars (EditorCppScripts.cpp) ---
    scripting::CppScriptSystem cpp_scripts_;
    std::vector<scripting::ScriptError> cpp_compile_errors_;
    std::string cpp_status_;
    double cpp_last_check_ = 0.0;
    std::filesystem::file_time_type cpp_attempted_{};  // fuentes de la ultima compilacion intentada
    bool cpp_has_sources_ = false;
    bool show_cvars_window_ = false;
    std::string cvar_filter_;
    std::uint64_t cvar_saved_generation_ = 0;
    std::string cvar_saved_text_;
    double cvar_dirty_time_ = -1.0;
    void setupCppScripts();    // al abrir un proyecto
    void updateCppScripts();   // cada frame: compilar al guardar, guardar las CVars
    void startCppScripts();    // al dar Play
    void stopCppScripts();
    // Pide el nombre (ventana) y luego crea Nombre.h + Nombre.cpp.
    void createCppScriptAsset(const std::filesystem::path& folder, ecs::Entity attach_to);
    std::filesystem::path createCppScriptFiles(const std::filesystem::path& folder, ecs::Entity attach_to, const std::string& name);
    std::string cppScriptNameProblem(const std::filesystem::path& folder, const std::string& name) const;  // vacio = valido
    void drawNewCppScriptModal();
    struct NewCppScript {
        bool open = false;
        std::filesystem::path folder;
        Uuid entity{};
        std::string name;
        bool focus = false;
    } new_cpp_script_;
    // Un script (.lua o .cpp) arrastrado o asignado a un objeto.
    void attachScriptFile(ecs::Entity e, const std::string& relative);
    void drawCVarsWindow();
    // --- Modelado poligonal (EditorModeling.cpp), como ProBuilder ---
    // Con una entidad con EditableMesh activa: modo Objeto (el gizmo de
    // siempre) o Vertices / Aristas / Caras (seleccion de elementos con
    // clic, Ctrl/Mayus para sumar y arrastre para caja; el gizmo mueve, gira
    // o escala lo elegido; Mayus + arrastrar el gizmo extruye).
    enum class ModelMode : int { Object = 0, Vertex = 1, Edge = 2, Face = 3 };
    bool show_modeling_window_ = false;
    ModelMode model_mode_ = ModelMode::Object;
    Uuid model_target_{};
    std::uint64_t model_target_revision_ = 0;
    std::vector<std::uint32_t> model_vertices_;
    std::vector<modeling::Edge> model_edges_;
    std::vector<int> model_faces_;
    bool model_xray_ = false;          // elegir tambien lo de detras
    bool model_box_ = false;           // arrastrando una caja de seleccion
    ImVec2 model_box_start_{};
    bool model_gizmo_using_ = false;
    modeling::PolyMesh model_drag_mesh_;  // la malla al empezar a arrastrar el gizmo
    core::Mat4 model_drag_start_{};
    core::Mat4 model_gizmo_matrix_{};
    // Lo que hay bajo el raton (resaltado): tipo como ModelMode e indice.
    int model_hover_kind_ = 0;
    std::int64_t model_hover_ = -1;
    modeling::Edge model_hover_edge_{};
    // Parametros de la ventana.
    int model_shape_ = 0;
    modeling::shapes::Params model_params_ = modeling::shapes::defaults(modeling::shapes::Kind::Cube);
    float model_extrude_ = 0.5f;
    bool model_extrude_individual_ = false;
    float model_inset_ = 0.15f;
    bool model_inset_individual_ = false;
    float model_bevel_ = 0.1f;
    int model_cuts_ = 1;
    float model_loop_t_ = 0.5f;
    float model_weld_ = 0.01f;
    float model_relax_ = 0.5f;
    float model_noise_ = 0.05f;
    float model_angle_ = 15.0f;
    int model_smooth_levels_ = 1;
    int model_mirror_axis_ = 0;
    int model_boolean_op_ = 1;
    int model_material_ = 0;
    int model_smoothing_ = 1;
    modeling::FaceUv model_uv_{};
    float model_quad_angle_ = 2.0f;
    void drawModelingWindow();
    void drawModelingToolbar();
    // Contorno, vertices y seleccion en la vista (antes del gizmo).
    void drawModelingOverlay();
    // Gizmo de los elementos elegidos (dentro de drawGizmo). true si lo pinta.
    bool drawModelingGizmo(const core::Mat4& view, const core::Mat4& projection);
    // Clics y caja de seleccion (despues del gizmo). true si se queda el clic.
    bool handleModelingInput();
    // La malla editable de la entidad activa (o nullptr).
    modeling::EditableMesh* modelingTarget(ecs::Entity* entity = nullptr);
    bool modelingElementMode();  // vertices / aristas / caras sobre una malla editable
    void modelingSelectionChanged();
    void modelingClearSelection();
    std::vector<std::uint32_t> modelingSelectedVertices(const modeling::PolyMesh& mesh) const;
    // Tras una operacion: rehace la malla, guarda el paso de deshacer y avisa.
    void modelingCommit(modeling::EditableMesh& em, const std::string& what);
    ecs::Entity createModelingShape(modeling::shapes::Kind kind, const modeling::shapes::Params& params);
    ecs::Entity convertToEditableMesh(ecs::Entity entity, float quad_angle);
    bool modelingDeleteSelection();
    // --- Pintar prefabs (EditorPrefabPaint.cpp) ---
    struct PaintItem {
        Uuid prefab;
        bool enabled = true;
        float weight = 1.0f;      // cuantos salen de este frente a los demas
        float scale_min = 0.9f;   // escala aleatoria (multiplica la del prefab)
        float scale_max = 1.1f;
        float align = 0.0f;       // 0 = vertical, 1 = sigue la normal del suelo
        bool random_yaw = true;
        float sink = 0.0f;        // metros hundido en el suelo
    };
    struct PaintGroup {
        std::string name;
        std::filesystem::path file;  // .crpaint
        std::vector<PaintItem> items;
    };
    struct PaintBrush {
        float radius = 5.0f;
        float density = 8.0f;    // objetos por 100 m2
        float spacing = 1.5f;    // separacion minima (m)
        float max_slope = 40.0f; // grados
        bool erase = false;
        bool only_selected = false;
        bool group_under_parent = true;
    };
    bool paint_mode_ = false;
    bool show_paint_window_ = false;
    // --- Generador de terreno (EditorTerrainGenerator.cpp) ---
    struct TerrainGenJob {
        terrain::GenSettings settings;
        bool textures = true;
        std::string texture_folder;  // disco
        std::thread thread;
        std::atomic<float> progress{0.0f};
        std::atomic<bool> cancel{false};
        std::atomic<bool> done{false};
        bool ok = false;
        std::mutex mutex;
        std::string stage = "Empezando";
        std::unique_ptr<terrain::GenResult> result;
        ~TerrainGenJob() {
            cancel = true;
            if (thread.joinable()) thread.join();
        }
    };
    bool show_terrain_generator_ = false;
    terrain::GenSettings terrain_gen_;
    bool terrain_gen_textures_ = true;
    bool terrain_gen_trees_ = true;
    bool terrain_gen_grass_ = true;
    float terrain_gen_grass_density_ = 70.0f;
    float terrain_gen_tree_density_ = 120.0f;
    std::unique_ptr<TerrainGenJob> terrain_gen_job_;
    void drawTerrainGeneratorWindow();
    void startTerrainGeneration();
    void applyGeneratedTerrain(terrain::GenResult& result);
    int terrain_gen_houses_ = 8;  // casas de la aldea (0 = sin aldea)
    int terrain_gen_settlement_ = -1;  // -1 segun las casas, 0 aldea, 1 pueblo, 2 ciudad amurallada

    // --- Modo cine (EditorCinematics.cpp): grabar trailers por MCP ---
    // Solo la imagen de la escena, a toda la ventana y del tamano pedido; el
    // tiempo solo avanza cuando se pide un paso (cinema_step), `cinema_dt_`
    // por paso, para que cada frame capturado sea un frame exacto del video.
public:
    float cinemaDelta(float real_seconds);
    bool cinemaActive() const { return cinema_; }
private:
    bool cinema_ = false;
    float cinema_dt_ = 1.0f / 30.0f;
    int cinema_pending_ = 0;
    std::uint64_t cinema_frames_ = 0;
    void setCinema(bool enabled, int width, int height, float fps);
    // Cambios de la ventana pedidos por el modo cine: se aplican entre frames
    // (ShowWindow dentro del frame de ImGui colgaba el editor al salir).
    int cinema_window_request_ = 0;  // 0 nada, 1 entrar, 2 salir
    int cinema_window_width_ = 1920;
    int cinema_window_height_ = 1080;
public:
    void applyWindowRequests();
private:
    void drawCinemaView();

    // --- Generador de casas y pueblos medievales (EditorHouseGenerator.cpp) ---
    bool show_house_generator_ = false;
    asset::HouseSettings house_gen_ = asset::housePreset(asset::HouseStyle::LogCabin, 1);
    int house_gen_village_ = 8;
    asset::SettlementSettings settlement_gen_{};
    bool settlement_interiors_ = true;   // muebles en las casas del pueblo
    bool settlement_lights_ = true;      // luz en cada hogar y fragua
    bool settlement_here_ = false;       // delante de la camara (si no, busca el sitio)
    int building_gen_ = 0;               // edificio suelto (MedievalBuilding)
    void drawHouseGeneratorWindow();
    // Texturas y .crmat compartidos de las casas (se crean si faltan).
    bool ensureHouseMaterials(assets::ModelMaterialMap& map, std::string* error);
    // Modelo de una casa (se reutiliza si ya existe uno igual); uuid invalido si falla.
    // `model` (opcional) recibe la geometria (caja, fuegos) aunque se reutilice.
    Uuid writeHouseModel(const asset::HouseSettings& settings, std::string* error, asset::HouseModel* model = nullptr,
                         bool refresh = true);
    // Cualquier edificio procedural ya construido (iglesia, muralla...) como
    // .crdata en Casas/Modelos: `key` distingue variantes (mismo key = mismo
    // archivo). refresh = false en lotes (se refresca la base de datos al final).
    Uuid writeBuildingModel(const std::string& name, const std::string& key, const asset::HouseModel& model, std::string* error,
                            bool refresh = true);
    // Instancia con sus materiales, colisiones (MeshCollider + la puerta con un
    // BoxCollider ajustado a su malla), una luz en cada fuego y, con
    // `flatten`, el componente TerrainFlatten (aplana el terreno debajo).
    ecs::Entity placeHouse(const Uuid& model, const core::Vec3& position, float yaw_degrees, ecs::Entity parent,
                           bool flatten = true, const std::vector<core::Vec3>* lights = nullptr, bool commit_terrain = true);
    // Aldea/pueblo en el terreno de la escena; devuelve cuantas casas puso.
    // type: -1 segun el numero de casas, si no un asset::SettlementType.
    int placeVillage(int count, std::uint32_t seed, ecs::Entity parent, std::string* error, int type = -1);
    // Pueblo o ciudad medieval completo (calles, plaza, edificios con interior,
    // murallas, campos). search_radius > 0: busca el mejor sitio alrededor de
    // `center`. undo: guarda el terreno de antes para Ctrl+Z.
    ecs::Entity generateSettlement(const asset::SettlementSettings& settings, const core::Vec3& center, float search_radius,
                                   ecs::Entity parent, std::string* error, bool undo = true, int* houses = nullptr);
    bool groundAt(float x, float z, float& y);
    // El terreno bajo (x, z): su entidad y datos. false si no hay.
    bool terrainAt(float x, float z, ecs::Entity& entity, std::shared_ptr<terrain::TerrainData>& data);

    // --- Aplanar el terreno bajo los objetos (componente TerrainFlatten) ---
    struct FlattenState {
        core::Mat4 pose = core::Mat4::identity();
        ecs::DVec3 origin{};            // origen flotante del mundo al guardar la pose
        std::uint64_t settings = 0;     // firma de los campos del componente
        std::string terrain;            // ruta de los datos del terreno
        terrain::HeightPatch patch;     // alturas de antes (para devolverlas)
        terrain::Footprint footprint{};
        float blend = 0.0f;
        bool applied = false;
        std::uint64_t order = 0;        // orden de aplanado (se deshace al reves)
        double retry_at = 0.0;
    };
    std::unordered_map<Uuid, FlattenState> flatten_states_;
    bool flatten_prime_ = true;         // escena nueva: lo que hay ya esta aplanado
    Uuid flatten_scene_{};
    std::uint64_t flatten_order_ = 0;
    // Huella y altura del suelo de una entidad con TerrainFlatten.
    bool flattenFootprintOf(ecs::Entity entity, terrain::Footprint& footprint, float& ground);
    // Aplana ya bajo la entidad (anade TerrainFlatten si no lo tiene) y guarda
    // como estaba. false si no tiene malla o no hay terreno debajo.
    bool flattenUnder(ecs::Entity entity, bool commit_collision = true);
    // Cada frame (fuera de Play): rehace los que se movieron al soltarlos y
    // devuelve el terreno de los borrados.
    void updateTerrainFlatteners();
    PaintGroup paint_group_;
    int paint_selected_ = -1;
    PaintBrush paint_brush_;
    bool paint_stroke_ = false;
    core::Vec3 paint_last_{};
    std::mt19937 paint_rng_{20260926u};
    std::vector<std::filesystem::path> paintGroupFiles() const;
    bool loadPaintGroup(const std::filesystem::path& file);
    void savePaintGroup();
    void newPaintGroup();
    void addPaintItem(const Uuid& prefab);
    std::vector<int> activePaintItems() const;
    std::vector<ecs::Entity> paintedInstances(bool active_only) const;
    bool isPaintedEntity(ecs::Entity e) const;
    ecs::Entity paintContainer();
    bool paintRaycast(const core::Vec3& origin, const core::Vec3& direction, float max_distance, core::Vec3& point,
                      core::Vec3& normal) const;
    int paintStamp(const core::Vec3& center, const core::Vec3& normal, bool erase);
    std::string paintItemName(const PaintItem& item);
    void drawPaintToolbar();
    bool drawPrefabPaintTool();
    void drawPaintWindow();
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
    // Borrar (Proyecto y explorador de scripts): archivos, assets y carpetas,
    // a la Papelera de reciclaje (EditorProject.cpp).
    void requestDelete(std::vector<std::filesystem::path> paths);
    // Ir al Proyecto con este archivo (o carpeta) seleccionado y a la vista.
    void revealInProject(const std::filesystem::path& path);
    std::string browser_reveal_;  // BrowserItem::key() a enseñar en el siguiente dibujo
    void deletePaths(const std::vector<std::filesystem::path>& paths);
    void drawDeleteModal();
    std::vector<std::filesystem::path> pending_delete_paths_;
    bool delete_companions_ = true;  // el .h de un .cpp (y al reves)
    struct ImportJob {
        std::filesystem::path source;
        std::filesystem::path folder;
        // Compartido con el hilo que importa (lo sigue escribiendo aunque
        // el trabajo ya no este en la lista).
        std::shared_ptr<assets::ImportProgress> progress;
        // Vacio mientras espera en la cola.
        std::future<assets::ImportResult> result;
        // Reimportar un modelo (combinando sus piezas) en vez de importar un
        // archivo: su UUID y el nombre de los materiales de cada pieza de
        // antes (para conservar los .crmat asignados en la escena).
        Uuid reimport{};
        std::vector<std::vector<std::string>> old_materials;
        std::optional<assets::ModelImportSettings> settings;  // al reimportar (si no, los suyos)
    };
    // En cola y en curso, en orden de llegada. Solo kMaxParallelImports a la
    // vez: una carpeta con muchos FBX grandes agotaria la RAM.
    std::vector<ImportJob> imports_;
    static constexpr std::size_t kMaxParallelImports = 2;
    // Para "archivo N de M": se ponen a cero cuando la cola se vacia.
    std::size_t imports_total_ = 0;
    std::size_t imports_done_ = 0;
    std::size_t imports_failed_ = 0;
    // Fraccion total de la cola (terminados + parte de los que estan en curso).
    float importFraction() const;
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

    // Navegador de contenido (EditorProject.cpp), como el Content Browser de
    // Unreal: carpetas, assets y archivos sueltos en una sola lista, con
    // tarjetas o lista, filtros por tipo, busqueda en todo el proyecto,
    // historial, favoritos y seleccion multiple.
public:
    struct BrowserItem {
        enum class Kind { Folder, Asset, Image, Script, Audio, Shader } kind = Kind::Asset;
        std::filesystem::path path;
        std::string name;
        std::string type;
        assets::AssetInfo info;  // Kind::Asset
        std::uint64_t size = 0;
        std::string key() const { return kind == Kind::Asset && info.uuid.valid() ? info.uuid.toString() : path.string(); }
    };
private:
    std::vector<BrowserItem> browser_items_;      // lo que se ve (carpeta o busqueda)
    std::vector<BrowserItem> browser_all_loose_;  // archivos sueltos de todo Assets (para buscar)
    std::uint32_t browser_filter_ = 0;            // tipos marcados (0 = todos)
    bool browser_list_view_ = false;
    std::vector<std::string> browser_selection_;  // BrowserItem::key()
    std::vector<std::filesystem::path> browser_back_;
    std::vector<std::filesystem::path> browser_forward_;
    std::vector<std::filesystem::path> browser_favorites_;
    std::string browser_cached_filter_;
    std::uint32_t browser_cached_bits_ = ~0u;
    std::filesystem::path renaming_file_;         // archivo suelto que se renombra
    void navigateTo(const std::filesystem::path& folder);
    void buildBrowserItems();
    void openBrowserItem(const BrowserItem& item);
    void browserItemMenu(const BrowserItem& item);
    void browserDragSource(const BrowserItem& item);
    bool browserItemHumanoid(const BrowserItem& item);
    void browserDropTarget(const BrowserItem& item);
    // Lo soltado en una carpeta (arbol o contenido): se mueve alli. Si lo
    // arrastrado esta seleccionado, toda la seleccion (tambien con filtros o
    // busqueda, desde varias carpetas). true si se solto algo.
    bool acceptBrowserMove(const std::filesystem::path& folder);
    void moveBrowserItems(const std::filesystem::path& folder, const std::string& dragged_key);
    bool browserSelected(const BrowserItem& item) const;
    void browserClick(const BrowserItem& item, std::size_t index);
    void loadBrowserFavorites();
    void saveBrowserFavorites();

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

    // --- Maquinas de estados de IA (EditorStateMachine.cpp) ---
    // example: con los estados del enemigo de ejemplo (Patrullar, Perseguir...).
    Uuid createStateMachineAsset(const std::filesystem::path& folder, bool example = false);
    void openStateMachineEditor(const Uuid& uuid);   // la carga y abre su pestana
    bool loadStateMachineEditor(const Uuid& uuid);   // solo la carga (sin cambiar de pestana)
    void saveStateMachineEditor();
    void assignStateMachineToSelection(const Uuid& uuid);
    void drawStateMachineEditor();
    void drawStateMachineGraph(ecs::Entity live);
    void drawStateMachineVariables(ecs::Entity live);
    void drawStateMachineDetails(ecs::Entity live);
    void drawStateMachineInspector(ecs::Entity entity);
    // Objeto que usa la maquina abierta (la seleccion o el primero): depuracion en vivo.
    ecs::Entity stateMachineLiveEntity();
    // El asset de una maquina (la abierta o leida del disco, con cache).
    const ai::StateMachineAsset* stateMachineAsset(const Uuid& uuid);
    // MCP: herramientas de maquinas de estados (args y resultado en JSON).
    std::string stateMachineMcpTool(const std::string& name, const std::string& args_json, std::string& error);
    bool show_state_machine_ = false;
    bool fsm_focus_ = false;
    Uuid fsm_uuid_{};
    std::filesystem::path fsm_path_;
    ai::StateMachineAsset fsm_;
    bool fsm_dirty_ = false;
    core::Vec2 fsm_pan_{360.0f, 140.0f};
    float fsm_zoom_ = 1.0f;
    int fsm_selected_state_ = -3;       // -3 ninguno, -1 Cualquier estado, >= 0 estado
    int fsm_selected_transition_ = -1;
    int fsm_link_from_ = -3;            // -3 = no se crea transicion; -1 = desde Cualquier estado
    int fsm_drag_ = -4;                 // nodo arrastrado (-1 Any, -2 Entrada, -4 nada)
    float fsm_right_width_ = 470.0f;    // panel del codigo
    struct FsmAssetCache {
        ai::StateMachineAsset machine;
        std::filesystem::file_time_type time{};
    };
    std::unordered_map<Uuid, FsmAssetCache> fsm_cache_;

    // Terreno.
    terrain::TerrainStore terrain_store_;
    terrain::TerrainBrush terrain_brush_;
    bool terrain_edit_ = false;
    bool terrain_stroke_ = false;
    std::shared_ptr<terrain::TerrainData> terrain_stroke_before_;
    bool terrain_ramp_started_ = false;
    core::Vec3 terrain_ramp_start_{};
    struct {
        int seed = 1337;
        float frequency = 3.0f;
        float roughness = 0.5f;
        float ridges = 0.35f;
        float base = 0.1f;
    } terrain_generate_;
    struct {
        int layer = 2;
        float min_slope = 35.0f;
        float max_slope = 90.0f;
        float min_height = 0.0f;
        float max_height = 1.0f;
    } terrain_rules_;
    // Deshacer mezclado: 'W' = instantanea del mundo, 'T' = trazo de terreno.
    struct TerrainUndo {
        std::string path;
        std::shared_ptr<terrain::TerrainData> before;
        std::shared_ptr<terrain::TerrainData> after;
    };
    std::deque<TerrainUndo> terrain_undo_;
    std::deque<TerrainUndo> terrain_redo_;
    std::deque<char> undo_kinds_;
    std::deque<char> redo_kinds_;
    // Espacios de trabajo (EditorWorkspaces.cpp).
    struct Workspace {
        int id = 0;
        WorkspaceKind kind = WorkspaceKind::Scene;
        std::string name;
        Uuid prefab{};               // Prefab: el asset
        std::filesystem::path path;  // Script: el archivo
        GraphKind graph = GraphKind::Vfx;  // Graph: que editor de nodos
        Uuid root{};                 // Prefab: raiz de la instancia en el escenario
        std::vector<Uuid> helpers;   // Prefab: luz y cielo del escenario
        bool built = false;          // Prefab: escenario creado
        // El mundo mientras no esta cargado.
        bool stashed = false;
        std::string world;
        std::filesystem::path scene_path;
        bool dirty = false;
        std::deque<std::string> undo;
        std::deque<std::string> redo;
        std::string current_state;
        std::deque<char> undo_kinds;
        std::deque<char> redo_kinds;
        std::deque<TerrainUndo> terrain_undo;
        std::deque<TerrainUndo> terrain_redo;
        std::vector<Uuid> selection;
        Uuid active{};
        std::optional<scene::Camera> camera;
    };
    std::vector<Workspace> workspaces_;
    int next_workspace_id_ = 1;
    int active_workspace_ = 0;  // id; 0 = la Escena
    int world_workspace_ = 0;   // id del espacio cuyo mundo esta en world_
    int pending_workspace_ = -1;
    bool pending_play_ = false;
    int workspace_close_ask_ = -1;       // prefab con cambios: preguntar al cerrar
    bool workspace_close_popup_ = false;  // abrir esa pregunta (desde la barra superior)
    int workspace_focus_frames_ = 0;     // enfocar el prefab cuando ya tiene actores
    unsigned int script_workspace_dock_ = 0;  // dockspace de las pestanas de script
    Workspace* findWorkspace(int id);
    const Workspace* findWorkspace(int id) const;
    float frame_delta_ = 0.0f;

    // Vistas y cinematicas.
    cinema::CinematicSystem cinematics_;
    bool show_game_ = true;
    bool show_cinematic_ = true;
    bool scene_view_visible_ = true;
    bool game_view_visible_ = false;
    // La vista Juego tiene el foco (o un hijo suyo): solo entonces el juego
    // recibe teclado y raton en Play, como Unity. En la Escena, WASD y el
    // raton son de la camara del editor.
    bool game_view_focused_ = false;
    float game_image_rect_[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // x, y, ancho, alto (pixeles de la ventana)
    std::uint32_t render_view_ = kSceneSlot;       // la vista que se dibuja este frame
    std::uint32_t last_render_view_ = kSceneSlot;
    std::uint32_t preferred_view_ = kSceneSlot;    // la ultima con la que se interactuo
    bool focus_game_ = false;                      // traer la vista Juego al frente
    // Al dar Play, ir a la pestana Juego (Play Focused de Unity) o quedarse en
    // la vista actual (Play Unfocused). Se guarda en CramionEditor.ini.
    bool play_focus_game_ = true;
    void registerEditorSettings();
    bool focus_scene_ = false;                     // traer la Escena al frente
    bool focus_inspector_ = false;                 // traer el Inspector al frente (asset elegido)
    std::optional<scene::Camera> saved_camera_;    // la del editor mientras se dibuja el Juego
    bool game_guides_ = false;                     // tercios en la vista Juego
    // Proporcion de cada vista (Escena, Juego): preset de kAspectPresets
    // (0 = Free Aspect) o -1 = resolucion propia (custom_w x custom_h).
    struct ViewAspect {
        int preset = 0;
        int custom_w = 1920;
        int custom_h = 1080;
    };
    ViewAspect view_aspect_[2]{};
    // Modo de dibujo de la vista Escena (gfx::SceneDrawMode): Lit, Unlit,
    // Wireframe o Lit + Wireframe.
    int scene_draw_mode_ = 0;
    void drawSceneDrawModeMenu();
    void drawPathTracingButton();
    std::uint32_t view_desired_[2][2] = {{0, 0}, {0, 0}};  // tamano de render que pide cada vista este frame
    std::uint32_t pending_view_[2] = {0, 0};
    float pending_view_time_ = 0.0f;
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
    // Liquidos (fluid::FluidSystem, EditorFluid.cpp): en Play y con "Simular
    // en el editor".
    fluid::FluidSystem fluids_;
    // --- VFX Graph, 2D y repeticiones (EditorEffects.cpp) ---
    vfx::VfxSystem vfx_;
    twod::System2D twod_;
    replay::ReplaySystem replay_;
    std::shared_ptr<EffectsEditorState> effects_editor_;
    EffectsEditorState& effectsEditor();
    void setupEffects();              // al abrir un proyecto
    void updateEffects(float delta_seconds);  // cada frame, tras la fisica
    void effectsEnterPlay();
    void effectsExitPlay();
    Uuid createVfxAsset(const std::filesystem::path& folder, int preset);
    void openVfxEditor(const Uuid& uuid);
    void saveVfxEditor();
    void drawVfxEditor();
    void drawReplayWindow();
    void drawEffectsWindows();
    void drawEffectsWindowMenu();
    void drawEffectsCreateMenu(const std::filesystem::path& folder);  // Proyecto > Crear
    // Proyecto > Crear: los assets nuevos de la 2.1 (VFX, grafos, 2D, pruebas...).
    void drawCreateMenuExtras(const std::filesystem::path& folder);
    // Abre un .crshadergraph / .crgraph / .crtileset en su editor (false si no es de esos).
    bool openGraphFile(const std::filesystem::path& file);
    // --- Shader Graph y Visual Scripting (EditorGraphs.cpp) ---
    std::shared_ptr<GraphEditorState> graph_editors_;
    GraphEditorState& graphEditors();
    void openShaderGraphEditor(const std::filesystem::path& file);
    void saveShaderGraphEditor();
    void drawShaderGraphEditor();
    void openVisualScriptEditor(const std::filesystem::path& file);
    void saveVisualScriptEditor();
    void drawVisualScriptEditor();
    void drawGraphEditors();  // todos los de grafos y 2D (cada frame)
    // --- Behavior Trees (EditorBehaviorTree.cpp) ---
    std::shared_ptr<BtEditorState> bt_editor_;
    BtEditorState& btEditor();
    Uuid createBehaviorTreeAsset(const std::filesystem::path& folder, bool example);
    void openBehaviorTreeEditor(const Uuid& uuid);
    void saveBehaviorTreeEditor();
    void drawBehaviorTreeEditor();
    // --- 2D (Editor2D.cpp) ---
    std::shared_ptr<TwoDEditorState> twod_editor_;
    TwoDEditorState& twodEditor();
    void openTilesetEditor(const std::filesystem::path& file);
    void draw2DCreateMenu(const std::filesystem::path& folder);
    void draw2DWindows();
    void draw2DWindowMenu();
    // Vista de Escena: pintar tiles (true = se queda con el raton), la vista
    // 2D (ortografica en el plano XY) y elegir sprites con un clic.
    bool draw2DTileTool();
    bool handle2DCamera();
    bool pick2DAt(float x, float y);
    void draw2DViewToggle();
    // --- Iluminacion horneada (EditorLighting.cpp) ---
    std::shared_ptr<LightingEditorState> lighting_editor_;
    LightingEditorState& lightingEditor();
    void loadSceneLighting();  // <escena>.crbake al abrir la escena
    void startLightingBake();
    void drawLightingWindow();
    void configureLightingBake(int rays, int bounces, float spacing);  // < 0 = no cambiar
    std::string lightingStateJson();
    // MCP de la 2.1 (EditorMcp21.cpp): pruebas, horneado, VFX, grafos y arboles.
    std::string mcpTools21(const std::string& name, const std::string& args_json, std::string& error);
    // --- Motion Matching (EditorMotion.cpp) ---
    std::shared_ptr<MotionEditorState> motion_editor_;
    MotionEditorState& motionEditor();
    Uuid createMotionDatabaseAsset(const std::filesystem::path& folder);
    void openMotionDatabaseEditor(const Uuid& uuid);
    void drawMotionDatabaseEditor();
    void drawMotionMatchingDebug();
    // --- Fracturar y vehiculos (EditorDestruction.cpp) ---
    std::shared_ptr<PhysicsToolsState> physics_tools_;
    PhysicsToolsState& physicsTools();
    void drawFractureWindow();
    void drawVehicleWizard();
    void drawPhysicsToolWindows();  // y Motion Matching e Iluminacion
    void drawPhysicsToolMenu();
    bool fluid_preview_ = false;  // el liquido de la vista previa sigue ahi
    void updateFluids(float delta_seconds);
    // 0 grifo de agua, 1 bloque de agua, 2 chorro de miel, 3 chorro de lava,
    // 4 mundo de liquidos, 5 desague, 6 tanque de demostracion.
    ecs::Entity createFluidEntity(int kind);
    physics::PhysicsSettings physics_settings_;
    PlayState play_state_ = PlayState::Edit;
    std::string play_snapshot_;     // el mundo al darle a Play
    bool play_dirty_before_ = false;
    // Graphics (Lua) y lo grafico al darle a Play (se restaura al parar).
    std::unique_ptr<RendererGraphicsHost> graphics_host_;
    struct PlayGraphics {
        gfx::GraphicsSettings settings;
        bool shadows = true;
        bool ray_tracing = false;
        bool reflection_probe = true;
        bool occlusion_culling = true;
        bool cascade_debug = false;
    };
    std::optional<PlayGraphics> play_graphics_;
    int step_requests_ = 0;         // "Paso" en pausa
    float play_time_ = 0.0f;
    bool show_physics_ = true;
    bool physics_settings_dirty_ = false;
    // Mundo de bloques (vista previa editando, un mundo nuevo en Play).
    voxel::VoxelSystem voxels_;
    std::string voxel_signature_;
    // Navegacion.
    navigation::NavigationSystem nav_;
    navigation::NavigationSettings nav_settings_;
    bool show_navigation_ = true;          // la malla en la escena (P)
    bool show_navigation_window_ = false;
    std::uint64_t nav_draw_version_ = ~0ull;
    core::Vec3 nav_draw_offset_{};
    std::vector<gfx::OverlayVertex> nav_draw_triangles_;
    std::vector<gfx::OverlayVertex> nav_draw_edges_;
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
    int hub_page_ = 0;      // 0 proyectos, 1 nuevo, 2 actualizaciones, 3 aprender
    int hub_category_ = 0;  // 0 todas, 1 integradas, 2 del usuario
    int hub_template_ = 0;
    std::string hub_search_;
    std::vector<ProjectTemplate> hub_templates_;
    bool hub_templates_loaded_ = false;
    std::shared_ptr<dialogs::AsyncFolderPick> hub_open_pick_;
    std::shared_ptr<dialogs::AsyncFolderPick> hub_folder_pick_;
    // Tercera persona avanzada: donde esta el Locomotion Pack (zip o carpeta).
    std::string hub_pack_path_;
    std::shared_ptr<dialogs::AsyncFolderPick> hub_pack_pick_;
    int hub_sort_ = 0;         // 0 recientes, 1 nombre
    bool hub_list_view_ = false;

    // Actualizaciones (EditorUpdates.cpp).
    void pollUpdates();
    void startUpdateCheck(bool manual);
    // Hay una version nueva que el usuario no ha omitido.
    bool updateAvailable() const;
    const update::Release* latestRelease() const;
    // Guarda la escena, los prefabs abiertos, los scripts, el material y el
    // Animator. Las escenas sin guardar van a Assets/Scenes/ (sin dialogo).
    bool saveEverythingForUpdate(std::string* error);
    // Lo que se guardara (para el dialogo de confirmacion).
    std::vector<std::string> unsavedWorkSummary() const;
    // Guarda todo, lanza el actualizador y cierra el editor.
    // `reinstall`: la misma version otra vez (repara archivos que falten o
    // esten danados).
    void beginUpdateInstall(bool reinstall = false);
    enum class UpdaterMode { Open, Install, Reinstall };
    bool launchUpdater(UpdaterMode mode);
    bool update_ask_reinstall_ = false;
    void drawUpdateToast();
    void drawUpdateDialog();
    void drawUpdateStatusLine(bool compact);
    std::shared_ptr<update::CheckJob> update_check_;
    update::Release update_release_;  // la ultima consultada (valida si update_known_)
    bool update_known_ = false;
    std::string update_check_error_;
    bool update_check_manual_ = false;
    double update_check_after_ = 3.0;  // segundos desde el arranque
    bool update_check_started_ = false;
    bool update_toast_dismissed_ = false;
    bool show_update_dialog_ = false;
    bool update_confirm_ = false;
    std::string update_error_;
    bool update_shutdown_requested_ = false;
    std::vector<update::NoteLine> update_notes_;
    update::Settings update_settings_ = update::loadSettings();
    bool show_save_template_ = false;
    std::string template_name_;
    std::string template_description_;
    std::string template_error_;

    // --- Partidas, localizacion y dialogos (EditorGameplay.cpp) ---
    std::shared_ptr<GameplayEditorState> gameplay_editor_;
    GameplayEditorState& gameplayEditor();
    void loadLocalization();
    void saveLocalization();
    void drawLocalizationWindow();
    void drawSavesWindow();
    void createDialogueAsset(const std::filesystem::path& folder, bool example);
    void openDialogueEditor(const std::filesystem::path& file);
    void saveDialogueEditor();
    void drawDialogueWindow();
    void drawGameplayWindows();
    void drawGameplayWindowMenu();

    // --- Plataforma (EditorPlatform.cpp, EditorGit.cpp, EditorTests.cpp) ---
    // Steam (logros, marcadores... en Play), panel de Git, pruebas
    // automaticas (Test Runner, --run-tests) y avisos flotantes.
    std::shared_ptr<PlatformState> platform_;
    PlatformState& platform();
    void updatePlatform(float delta_seconds);  // cada frame, antes de la interfaz
    void drawPlatformWindows();                // Git, Pruebas y avisos
    void drawPlatformWindowMenu();             // entradas de Ventana
    void drawPlatformBuildSettings(BuildConfig& config, bool& changed);  // Steam y Linux en Configuraciones de compilacion
    // Archivos extra de la exportacion (steam_api64.dll, steam_appid.txt): de -> a.
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> platformExportFiles(const BuildConfig& config,
                                                                                             const std::filesystem::path& target);
    void platformEnterPlay();                  // Steam al dar Play
    void drawGitWindow();
    void drawTestRunnerWindow();
    void drawToasts();
    void updateTestRunner(float delta_seconds);
    void startTestRun(int mode);  // 0 todas, 1 edicion, 2 Play, 3 las que fallaron
    void stopTestRun();
    bool testRunActive() const;
    std::string testResultsJson() const;
    void gitRefresh();
public:
    // Un aviso flotante abajo a la derecha (kind: 0 info, 1 bien, 2 aviso, 3 error).
    void pushToast(const std::string& title, const std::string& text = {}, int kind = 0);
    // CramionEditor.exe --run-tests <proyecto> [--junit archivo]: abre el
    // proyecto, ejecuta todas las pruebas y cierra (codigo de salida 0 = todas bien).
    void startTestRunFromCli(const std::filesystem::path& project, const std::filesystem::path& junit);
    int exitCode() const { return exit_code_; }
private:
    int exit_code_ = 0;

    // Paneles visibles y acciones pendientes.
    bool show_hierarchy_ = true;
    bool show_inspector_ = true;
    bool show_project_ = true;
    bool show_statistics_ = true;
    // Ventana > Insights (perfilador de CPU, EditorInsights.cpp).
    bool show_insights_ = false;
    bool show_accessibility_ = false;  // Ventana > Accesibilidad (EditorAccessibility.cpp)
    void drawAccessibilityWindow();
    void drawInsightsWindow();
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
    Uuid self_test_agent_{};
    core::Vec3 self_test_nav_start_{};
    core::Vec3 self_test_nav_target_{};
    Uuid self_test_cube_{};
    Uuid self_test_zone_{};
    Uuid self_test_sequence_{};
    Uuid self_test_terrain_{};
    // Sombras de luces locales: la luz, el sol apagado mientras y la captura con sombra.
    Uuid self_test_light_{};
    Uuid self_test_prefab_{};  // prefab de la prueba de espacios de trabajo
    Uuid self_test_sun_{};
    std::vector<std::uint8_t> self_test_capture_;
    std::size_t self_test_entities_ = 0;
};

}  // namespace cramion::editor

#endif  // CRAMION_EDITOR_EDITOR_APP_H

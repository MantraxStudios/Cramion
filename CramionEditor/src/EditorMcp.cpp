// MCP (Model Context Protocol) del editor: las herramientas con las que una IA
// trabaja en el motor (escenas, objetos, componentes, scripts, shaders,
// materiales, modelos, prefabs, Play, Lua, consola y capturas). El transporte
// esta en McpServer; aqui el JSON-RPC y cada herramienta, siempre en el hilo
// principal y con deshacer como si lo hiciera el usuario.

#include "EditorApp.h"

#include "Dialogs.h"
#include "EditorLog.h"
#include "ProjectTemplates.h"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>

namespace cramion::editor {

// EditorSelfTest.cpp: la ventana tal como se ve, guardada como PNG.
std::vector<std::uint8_t> captureEditorWindow(HWND hwnd, const std::filesystem::path& png);

using json = nlohmann::json;
using core::Vec3;

namespace {

constexpr const char* kProtocolVersions[] = {"2025-06-18", "2025-03-26", "2024-11-05"};

struct ToolError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::string base64(const std::vector<std::uint8_t>& data) {
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < data.size(); i += 3) {
        const std::uint32_t n = (static_cast<std::uint32_t>(data[i]) << 16) |
                                (i + 1 < data.size() ? static_cast<std::uint32_t>(data[i + 1]) << 8 : 0u) |
                                (i + 2 < data.size() ? static_cast<std::uint32_t>(data[i + 2]) : 0u);
        out += table[(n >> 18) & 63];
        out += table[(n >> 12) & 63];
        out += i + 1 < data.size() ? table[(n >> 6) & 63] : '=';
        out += i + 2 < data.size() ? table[n & 63] : '=';
    }
    return out;
}

json vec(const Vec3& v) { return json::array({v.x, v.y, v.z}); }

Vec3 readVec(const json& j, const char* key, const Vec3& fallback) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array() || it->size() < 3) return fallback;
    return Vec3{(*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>()};
}

std::string readText(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

std::vector<std::uint8_t> readBytes(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), {});
}

// Un nombre de archivo seguro a partir de lo que pida la IA.
std::string safeName(std::string name) {
    for (char& c : name) {
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') c = '_';
    }
    return name.empty() ? std::string("Nuevo") : name;
}

// --- Definicion de las herramientas (tools/list) ------------------------------
json prop(const char* type, const char* description) { return json{{"type", type}, {"description", description}}; }
json vec3Prop(const char* description) {
    return json{{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3}, {"description", description}};
}

struct ToolDef {
    const char* name;
    const char* description;
    json properties;
    std::vector<std::string> required;
};

const std::vector<ToolDef>& toolDefs() {
    static const std::vector<ToolDef> defs = [] {
        const json entity = prop("string", "La entidad: su UUID, su nombre o una ruta Padre/Hijo");
        std::vector<ToolDef> d;
        d.push_back({"help", "Guia rapida del motor Cramion para la IA: convenciones, formatos (.lua, .crshader, .crmat) y flujo de trabajo recomendado. Llamala primero.", json::object(), {}});
        d.push_back({"editor_state", "Estado del editor: proyecto abierto, escena, si hay cambios sin guardar, modo Play, seleccion y camara.", json::object(), {}});
        d.push_back({"list_projects", "Proyectos recientes y plantillas disponibles para crear uno.", json::object(), {}});
        d.push_back({"open_project", "Abre un proyecto (.crproj o su carpeta).", {{"path", prop("string", "Ruta del .crproj o de la carpeta del proyecto")}, {"force", prop("boolean", "Descartar cambios sin guardar")}}, {"path"}});
        d.push_back({"create_project", "Crea un proyecto nuevo desde una plantilla y lo abre.", {{"name", prop("string", "Nombre del proyecto")}, {"folder", prop("string", "Carpeta donde crearlo (por defecto Documentos/Cramion Projects)")}, {"template", prop("string", "Id de la plantilla (ver list_projects); por defecto 'blank'")}}, {"name"}});
        d.push_back({"list_entities", "Arbol de la escena: cada entidad con su UUID, nombre, componentes e hijos.", {{"root", entity}, {"depth", prop("integer", "Profundidad maxima (por defecto 12)")}}, {}});
        d.push_back({"get_entity", "Todo de una entidad: transformacion y cada componente con sus campos (JSON).", {{"entity", entity}}, {"entity"}});
        d.push_back({"create_entity", "Crea una entidad. type: empty, cube, sphere, plane, cylinder, capsule, directional_light, point_light, spot_light, camera, decal, vehicle, terrain, voxel_world, ocean, lake, river. Opcional: posicion, giro (grados), escala, padre y componentes {\"Tipo\": {campos}}.",
                     {{"type", prop("string", "Que crear (por defecto empty)")}, {"name", prop("string", "Nombre")}, {"parent", entity},
                      {"position", vec3Prop("Posicion en el mundo")}, {"rotation", vec3Prop("Giro local en grados (X, Y, Z)")}, {"scale", vec3Prop("Escala local")},
                      {"components", prop("object", "Componentes a anadir/ajustar: {\"Light\": {\"intensity\": 5}, \"Rigidbody\": {}}")}}, {}});
        d.push_back({"modify_entity", "Cambia nombre, activo, tag, padre, posicion (mundo), posicion local, giro (grados) o escala de una entidad.",
                     {{"entity", entity}, {"name", prop("string", "Nombre nuevo")}, {"active", prop("boolean", "Activa")}, {"tag", prop("string", "Tag")},
                      {"parent", prop("string", "Nuevo padre (UUID/nombre; vacio = raiz)")}, {"position", vec3Prop("Posicion en el mundo")},
                      {"local_position", vec3Prop("Posicion local")}, {"rotation", vec3Prop("Giro local en grados")}, {"scale", vec3Prop("Escala local")}}, {"entity"}});
        d.push_back({"delete_entity", "Borra una entidad y sus hijos.", {{"entity", entity}}, {"entity"}});
        d.push_back({"duplicate_entity", "Duplica una entidad con sus hijos.", {{"entity", entity}}, {"entity"}});
        d.push_back({"list_component_types", "Todos los tipos de componente del motor con sus campos y valores por defecto (para set_component).", json::object(), {}});
        d.push_back({"set_component", "Anade un componente (si no lo tiene) y cambia sus campos. Los campos que no se pasen no cambian. Ver list_component_types.",
                     {{"entity", entity}, {"component", prop("string", "Nombre del tipo, p. ej. Light, Rigidbody, BoxCollider, Script, Camera")}, {"values", prop("object", "Campos a cambiar")}}, {"entity", "component"}});
        d.push_back({"remove_component", "Quita un componente de una entidad.", {{"entity", entity}, {"component", prop("string", "Nombre del tipo")}}, {"entity", "component"}});
        d.push_back({"select", "Selecciona una entidad en el editor y opcionalmente centra la camara en ella.", {{"entity", entity}, {"focus", prop("boolean", "Centrar la camara")}}, {"entity"}});
        d.push_back({"set_camera", "Coloca la camara del editor.", {{"position", vec3Prop("Posicion")}, {"target", vec3Prop("Punto al que mira")}}, {"position", "target"}});
        d.push_back({"list_assets", "Assets del proyecto (modelos, materiales, escenas, prefabs, cielos...) y archivos sueltos (scripts .lua, shaders .crshader, imagenes, audio).",
                     {{"folder", prop("string", "Subcarpeta de Assets (opcional)")}, {"type", prop("string", "Filtro: Model, Material, Scene, Prefab, Environment...")}}, {}});
        d.push_back({"read_file", "Lee un archivo de texto del proyecto (ruta relativa a Assets, p. ej. Scripts/Jugador.lua).", {{"path", prop("string", "Ruta dentro de Assets")}}, {"path"}});
        d.push_back({"write_file", "Escribe (crea o reemplaza) un archivo de texto en Assets: scripts, shaders, materiales, datos. Se recarga en caliente.",
                     {{"path", prop("string", "Ruta dentro de Assets")}, {"content", prop("string", "Contenido completo")}}, {"path", "content"}});
        d.push_back({"delete_file", "Borra un archivo o asset de Assets.", {{"path", prop("string", "Ruta dentro de Assets")}}, {"path"}});
        d.push_back({"create_script", "Crea un script Lua (con la plantilla o con el codigo dado) y opcionalmente lo engancha a una entidad.",
                     {{"name", prop("string", "Nombre (sera Scripts/<name>.lua)")}, {"code", prop("string", "Codigo Lua completo (debe terminar en return <tabla>)")}, {"attach_to", entity}}, {"name"}});
        d.push_back({"create_shader", "Crea un shader de superficie GLSL (.crshader) y lo compila. Devuelve los errores si los hay.",
                     {{"name", prop("string", "Nombre (sera Shaders/<name>.crshader)")}, {"code", prop("string", "Codigo del shader (ver help)")}}, {"name"}});
        d.push_back({"create_material", "Crea un material (.crmat): color, metalico, rugosidad, emision, texturas y opcionalmente un shader propio con sus valores.",
                     {{"name", prop("string", "Nombre (sera Materials/<name>.crmat)")}, {"color", prop("array", "Color RGBA lineal 0..1")},
                      {"metallic", prop("number", "0..1")}, {"roughness", prop("number", "0..1")}, {"emissive", vec3Prop("Color de emision")},
                      {"emissive_intensity", prop("number", "Intensidad de la emision")}, {"albedo_texture", prop("string", "Imagen de Assets para el color")},
                      {"normal_texture", prop("string", "Imagen de Assets para el normal map")}, {"transparent", prop("boolean", "Modo transparente (vidrio)")},
                      {"shader", prop("string", "Ruta del .crshader en Assets")}, {"shader_values", prop("object", "{propiedad: numero o [x,y,z]}")},
                      {"tiling", prop("array", "Repeticion UV [x, y]")}}, {"name"}});
        d.push_back({"assign_material", "Asigna un material a una entidad (y sus hijos si no tiene malla).",
                     {{"entity", entity}, {"material", prop("string", "Ruta, nombre o UUID del .crmat")}, {"slot", prop("integer", "Hueco de material (-1 = todos)")}}, {"entity", "material"}});
        d.push_back({"reimport_model", "Reimporta un modelo desde su archivo original combinando sus piezas por material (una palmera con cada hoja suelta pasa a tronco + hojas) y rehace sus instancias en la escena. En segundo plano: mira get_console.",
                     {{"asset", prop("string", "Ruta, nombre o UUID del modelo")}}, {"asset"}});
        d.push_back({"performance_stats", "Rendimiento del ultimo frame: FPS, ms de CPU y GPU, tiempo de GPU por pase, actores, triangulos, lotes y llamadas de sombras.",
                     json::object(), {}});
        d.push_back({"import_file", "Importa un archivo del disco al proyecto (modelo .fbx/.obj/.gltf/.glb, cielo .hdr).",
                     {{"path", prop("string", "Ruta absoluta del archivo")}, {"folder", prop("string", "Subcarpeta de Assets (por defecto Models)")}}, {"path"}});
        d.push_back({"create_model", "Crea un modelo 3D a partir de texto OBJ (y opcionalmente MTL) y lo importa como asset.",
                     {{"name", prop("string", "Nombre del modelo")}, {"obj", prop("string", "Contenido del .obj (v, vt, vn, f...)")}, {"mtl", prop("string", "Contenido del .mtl (opcional)")}}, {"name", "obj"}});
        d.push_back({"instantiate", "Pone en la escena un modelo, prefab o cielo del proyecto.",
                     {{"asset", prop("string", "Ruta, nombre o UUID del asset")}, {"position", vec3Prop("Posicion")}, {"parent", entity}, {"name", prop("string", "Nombre de la instancia")}}, {"asset"}});
        d.push_back({"create_prefab", "Guarda una entidad (con sus hijos) como prefab (.crprefab).", {{"entity", entity}, {"folder", prop("string", "Subcarpeta de Assets (por defecto Prefabs)")}}, {"entity"}});
        d.push_back({"list_scenes", "Escenas del proyecto.", json::object(), {}});
        d.push_back({"new_scene", "Escena nueva (camara, luz y cielo).", {{"force", prop("boolean", "Descartar cambios sin guardar")}}, {}});
        d.push_back({"open_scene", "Abre una escena.", {{"scene", prop("string", "Ruta o nombre de la escena")}, {"force", prop("boolean", "Descartar cambios sin guardar")}}, {"scene"}});
        d.push_back({"save_scene", "Guarda la escena (en su archivo o en el que se indique).", {{"path", prop("string", "Ruta dentro de Assets (opcional, p. ej. Scenes/Nivel1.crscene)")}}, {}});
        d.push_back({"play", "Entra en modo Play (el juego corre en el editor).", json::object(), {}});
        d.push_back({"stop", "Sale del modo Play (la escena vuelve a como estaba).", json::object(), {}});
        d.push_back({"pause", "Pausa o reanuda el modo Play.", json::object(), {}});
        d.push_back({"run_lua", "Ejecuta codigo Lua en el motor (en Play, dentro del juego). Devuelve lo que retorne el codigo.",
                     {{"code", prop("string", "Codigo Lua; usa return para obtener un valor")}}, {"code"}});
        d.push_back({"get_console", "Ultimas lineas de la consola del editor (logs, avisos, errores de scripts y shaders).",
                     {{"lines", prop("integer", "Cuantas (por defecto 60)")}, {"level", prop("string", "all, warnings o errors")}}, {}});
        d.push_back({"screenshot", "Captura del editor (imagen PNG) para ver el resultado.", json::object(), {}});
        d.push_back({"undo", "Deshace la ultima accion.", json::object(), {}});
        d.push_back({"redo", "Rehace.", json::object(), {}});
        return d;
    }();
    return defs;
}

const char* kHelp = R"(# Cramion para IA (MCP)

Motor 3D (Vulkan, Windows) con editor tipo Unity. Unidades: metros; Y arriba; "adelante" es -Z;
giros en grados (Euler, orden YXZ como Unity).

## Flujo recomendado
1. editor_state (hay proyecto abierto?). Si no: list_projects y open_project / create_project.
2. list_entities para ver la escena; get_entity para los detalles.
3. Crea/cambia con create_entity, modify_entity, set_component (list_component_types da los campos).
4. Logica: create_script (Lua) y engancharlo (attach_to). Aspecto: create_material / create_shader.
5. Prueba con play, get_console (errores), screenshot; stop. Guarda con save_scene.

## Scripts Lua (Assets/Scripts/*.lua)
local Jugador = { properties = { velocidad = 5.0 } }   -- se editan en el Inspector
function Jugador:Start() end
function Jugador:Update(dt)
    local x = Input.getAxis("Horizontal"); local z = Input.getAxis("Vertical")
    self.entity:translate(Vec3(x, 0, -z) * self.velocidad * dt)
end
function Jugador:OnCollisionEnter(other, contact) end
return Jugador   -- obligatorio
API: Vec3, Quat, Mathf, Random, Entity (position, rotation, scale, forward, velocity, addForce,
lookAt, find, getScript, destroy...), Scene (find, create, instantiate("Prefabs/X"), load), Input,
Time, Physics.raycast, Audio, UI, Prefs, Game, Debug.log, Navigation, Mesh, Voxel.
Manual completo: https://cramion.mantraxtools.store/manual/index.html

## Shaders de superficie (Assets/Shaders/*.crshader, GLSL)
property color tinte = 1, 0.5, 0.2
property range brillo = 2 (0, 10)
void surface(inout Surface s) {   // s.albedo, s.alpha, s.normal, s.metallic, s.roughness,
    s.emission = tinte * brillo;  // s.occlusion, s.emission; lectura: s.uv, s.worldPosition,
}                                 // s.viewDirection. TIME, noise(), fbm(), fresnel(s, p)
void vertex(inout Vertex v) { }   // opcional: v.position, v.normal, v.uv (mundo)
Se usan desde un material: create_material con shader = "Shaders/X.crshader".

## Componentes frecuentes (set_component)
MeshRenderer, Light (type Directional/Point/Spot, color, intensity, range), Camera, Rigidbody,
BoxCollider, SphereCollider, CapsuleCollider, MeshCollider, Script (file = "Scripts/X.lua"),
AudioSource, Animator, ParticleSystem, NavAgent, Terrain, WaterBody, Decal.
Nota: primitivas (cube, sphere...) ya traen su collider.
)";

}  // namespace

// Acceso a lo privado del editor para las herramientas.
class McpTools {
public:
    explicit McpTools(EditorApp& app) : a(app) {}

    json call(const std::string& name, const json& args, bool& image, std::string& image_data);

private:
    EditorApp& a;

    void needProject() const {
        if (!a.has_project_) throw ToolError("No hay proyecto abierto: usa list_projects y open_project (o create_project).");
    }
    std::string arg(const json& args, const char* key, const std::string& fallback = {}) const {
        const auto it = args.find(key);
        return it != args.end() && it->is_string() ? it->get<std::string>() : fallback;
    }

    ecs::Entity entity(const std::string& ref) const {
        if (ref.empty()) throw ToolError("falta la entidad");
        const Uuid uuid = Uuid::parse(ref);
        if (uuid.valid()) {
            if (ecs::Entity e = a.world_.find(uuid); e.valid()) return e;
        }
        if (ref.find('/') != std::string::npos) {
            ecs::Entity current;
            std::stringstream parts(ref);
            std::string part;
            bool first = true;
            while (std::getline(parts, part, '/')) {
                ecs::Entity next;
                if (first) {
                    for (const entt::entity root : a.world_.roots()) {
                        if (a.world_.wrap(root).name() == part) {
                            next = a.world_.wrap(root);
                            break;
                        }
                    }
                } else if (current.valid()) {
                    for (const entt::entity child : current.children()) {
                        if (a.world_.wrap(child).name() == part) {
                            next = a.world_.wrap(child);
                            break;
                        }
                    }
                }
                current = next;
                first = false;
                if (!current.valid()) break;
            }
            if (current.valid()) return current;
        }
        if (ecs::Entity e = a.world_.findByName(ref); e.valid()) return e;
        throw ToolError("no existe la entidad '" + ref + "' (usa list_entities)");
    }

    json summary(ecs::Entity e) const {
        json j;
        j["uuid"] = e.uuid().toString();
        j["name"] = e.name();
        j["active"] = e.activeSelf();
        if (!e.tag().empty() && e.tag() != "Untagged") j["tag"] = e.tag();
        j["position"] = vec(e.worldPosition());
        j["rotation"] = vec(e.localEulerDegrees());
        const Vec3 s = e.localScale();
        if (s.x != 1.0f || s.y != 1.0f || s.z != 1.0f) j["scale"] = vec(s);
        json components = json::array();
        for (const ecs::ComponentType& type : ecs::ComponentRegistry::instance().types()) {
            if (type.name == "EntityInfo" || type.name == "Transform") continue;
            if (type.has(a.world_, e.handle())) components.push_back(type.name);
        }
        j["components"] = components;
        return j;
    }

    json tree(ecs::Entity e, int depth, int& budget) const {
        json j = summary(e);
        if (e.childCount() > 0) {
            if (depth <= 0 || budget <= 0) {
                j["children_count"] = e.childCount();
            } else {
                json children = json::array();
                for (const entt::entity c : e.children()) {
                    if (--budget <= 0) break;
                    children.push_back(tree(a.world_.wrap(c), depth - 1, budget));
                }
                j["children"] = children;
            }
        }
        return j;
    }

    json fullEntity(ecs::Entity e) const {
        json j = summary(e);
        j["parent"] = e.parent().valid() ? json(e.parent().uuid().toString()) : json(nullptr);
        j["local_position"] = vec(e.localPosition());
        json components = json::object();
        for (const ecs::ComponentType& type : ecs::ComponentRegistry::instance().types()) {
            if (type.name == "EntityInfo" || !type.has(a.world_, e.handle())) continue;
            const std::string text = ecs::componentToJson(a.world_, e, type.name);
            components[type.name] = text.empty() ? json::object() : json::parse(text, nullptr, false);
        }
        j["components"] = components;
        json children = json::array();
        for (const entt::entity c : e.children()) {
            const ecs::Entity child = a.world_.wrap(c);
            children.push_back(json{{"uuid", child.uuid().toString()}, {"name", child.name()}});
        }
        j["children"] = children;
        return j;
    }

    void applyComponents(ecs::Entity e, const json& components) {
        if (!components.is_object()) return;
        for (const auto& [name, values] : components.items()) {
            std::string error;
            if (!ecs::componentFromJson(a.world_, e, name, values.is_object() ? values.dump() : std::string("{}"), &error)) {
                throw ToolError(error);
            }
        }
    }

    // Ruta dentro de Assets, sin salir de la carpeta del proyecto.
    std::filesystem::path assetPath(const std::string& relative) const {
        needProject();
        if (relative.empty()) throw ToolError("falta la ruta");
        std::string rel = relative;
        if (rel.rfind("Assets/", 0) == 0 || rel.rfind("Assets\\", 0) == 0) rel = rel.substr(7);
        const std::filesystem::path root = std::filesystem::weakly_canonical(a.project_.assetsFolder());
        const std::filesystem::path full = std::filesystem::weakly_canonical(root / dialogs::fromUtf8(rel));
        const auto mismatch = std::mismatch(root.begin(), root.end(), full.begin(), full.end());
        if (mismatch.first != root.end()) throw ToolError("la ruta sale de la carpeta Assets del proyecto");
        return full;
    }

    std::optional<assets::AssetInfo> findAsset(const std::string& ref) const {
        const Uuid uuid = Uuid::parse(ref);
        if (uuid.valid()) {
            if (auto info = a.database_->find(uuid)) return info;
        }
        std::filesystem::path path;
        try {
            path = assetPath(ref);
        } catch (const ToolError&) {
        }
        for (const assets::AssetInfo& info : a.database_->all()) {
            std::error_code ec;
            if (!path.empty() && !info.path.empty() && std::filesystem::equivalent(info.path, path, ec)) return info;
        }
        for (const assets::AssetInfo& info : a.database_->all()) {
            if (info.name == ref || dialogs::utf8(info.path.filename()) == ref || dialogs::utf8(info.path.stem()) == ref) return info;
        }
        return std::nullopt;
    }

    void afterWrite(const std::filesystem::path& file) {
        a.refreshDatabase();
        const std::string rel = a.assetRelative(file);
        const std::string ext = file.extension().string();
        if (ext == ".lua") a.scripts_.reloadFile(rel);
        if (ext == assets::kSurfaceShaderExtension && a.sync_) a.sync_->reloadSurfaceShaders();
        if (ext == ".crmat" && a.sync_) {
            for (const assets::AssetInfo& info : a.database_->all()) {
                std::error_code ec;
                if (std::filesystem::equivalent(info.path, file, ec)) a.sync_->reloadMaterial(info.uuid);
            }
            a.material_edit_uuid_ = {};  // el editor de materiales la vuelve a leer
        }
    }

    bool unsavedBlocks(const json& args) const {
        return a.dirty_ && !args.value("force", false);
    }
};

json McpTools::call(const std::string& name, const json& args, bool& image, std::string& image_data) {
    image = false;
    if (name == "help") return kHelp;

    if (name == "editor_state") {
        json j;
        j["engine"] = "Cramion";
        j["project_open"] = a.has_project_;
        if (a.has_project_) {
            j["project"] = a.project_.name;
            j["project_folder"] = dialogs::utf8(a.project_.folder);
            j["scene"] = a.world_.sceneName();
            j["scene_file"] = a.scene_path_.empty() ? json(nullptr) : json(a.assetRelative(a.scene_path_));
            j["unsaved_changes"] = a.dirty_;
            j["mode"] = a.play_state_ == EditorApp::PlayState::Edit ? "edit" : a.play_state_ == EditorApp::PlayState::Playing ? "play" : "paused";
            j["entities"] = a.world_.entityCount();
            json selection = json::array();
            for (const ecs::Entity e : a.selectedEntities()) selection.push_back(json{{"uuid", e.uuid().toString()}, {"name", e.name()}});
            j["selection"] = selection;
            j["camera_position"] = vec(a.scene_.camera().position());
            j["camera_forward"] = vec(a.scene_.camera().forward());
        }
        return j;
    }
    if (name == "list_projects") {
        json recent = json::array();
        for (const project::RecentProject& p : project::recentProjects()) {
            recent.push_back(json{{"name", p.name}, {"path", dialogs::utf8(p.file)}});
        }
        json templates = json::array();
        for (const ProjectTemplate& t : availableTemplates()) {
            templates.push_back(json{{"id", t.id}, {"name", t.name}, {"description", t.description}});
        }
        return json{{"recent", recent}, {"templates", templates}};
    }
    if (name == "open_project") {
        if (a.has_project_ && unsavedBlocks(args)) throw ToolError("hay cambios sin guardar: save_scene o force=true");
        if (!a.openProject(dialogs::fromUtf8(arg(args, "path")))) throw ToolError("no se pudo abrir el proyecto");
        return json{{"opened", a.project_.name}};
    }
    if (name == "create_project") {
        if (a.has_project_ && unsavedBlocks(args)) throw ToolError("hay cambios sin guardar: save_scene o force=true");
        const std::string template_id = arg(args, "template", "blank");
        ProjectTemplate chosen;
        bool found = false;
        for (const ProjectTemplate& t : availableTemplates()) {
            if (t.id == template_id) {
                chosen = t;
                found = true;
            }
        }
        if (!found) throw ToolError("plantilla desconocida: " + template_id);
        const std::filesystem::path folder = args.contains("folder") ? dialogs::fromUtf8(arg(args, "folder"))
                                                                     : dialogs::fromUtf8(a.new_project_folder_);
        std::filesystem::create_directories(folder);
        const project::ProjectInfo created = createProjectFromTemplate(chosen, folder, safeName(arg(args, "name")));
        if (!a.openProject(created.file)) throw ToolError("se creo pero no se pudo abrir");
        return json{{"project", created.name}, {"folder", dialogs::utf8(created.folder)}};
    }

    needProject();

    if (name == "list_entities") {
        const int depth = args.value("depth", 12);
        int budget = 3000;
        json out = json::array();
        if (args.contains("root") && !arg(args, "root").empty()) {
            out.push_back(tree(entity(arg(args, "root")), depth, budget));
        } else {
            for (const entt::entity root : a.world_.roots()) {
                if (--budget <= 0) break;
                out.push_back(tree(a.world_.wrap(root), depth, budget));
            }
        }
        return json{{"scene", a.world_.sceneName()}, {"entities", out}};
    }
    if (name == "get_entity") return fullEntity(entity(arg(args, "entity")));

    if (name == "create_entity") {
        static const std::map<std::string, int> kinds = {
            {"empty", 0}, {"cube", 1}, {"sphere", 2}, {"plane", 3}, {"cylinder", 4}, {"capsule", 5},
            {"directional_light", 6}, {"point_light", 7}, {"spot_light", 8}, {"camera", 9}, {"decal", 10}, {"vehicle", 17}};
        const std::string type = arg(args, "type", "empty");
        const ecs::Entity parent = args.contains("parent") && !arg(args, "parent").empty() ? entity(arg(args, "parent")) : ecs::Entity{};
        ecs::Entity e;
        if (const auto it = kinds.find(type); it != kinds.end()) e = a.createEntity(it->second, parent);
        else if (type == "terrain") e = a.createTerrainEntity();
        else if (type == "voxel_world") e = a.createVoxelWorldEntity();
        else if (type == "ocean") e = a.createWaterEntity(0);
        else if (type == "lake") e = a.createWaterEntity(1);
        else if (type == "river") e = a.createWaterEntity(2);
        else throw ToolError("tipo desconocido: " + type);
        if (!e.valid()) throw ToolError("no se pudo crear");
        if (args.contains("name")) e.setName(arg(args, "name"));
        if (parent.valid() && e.parent() != parent) e.setParent(parent, false);
        if (args.contains("position")) e.setWorldPosition(readVec(args, "position", e.worldPosition()));
        if (args.contains("rotation")) e.setLocalEulerDegrees(readVec(args, "rotation", {}));
        if (args.contains("scale")) e.setLocalScale(readVec(args, "scale", Vec3{1, 1, 1}));
        if (args.contains("components")) applyComponents(e, args["components"]);
        a.commit();
        return summary(e);
    }
    if (name == "modify_entity") {
        ecs::Entity e = entity(arg(args, "entity"));
        if (args.contains("name")) e.setName(arg(args, "name"));
        if (args.contains("active")) e.setActive(args["active"].get<bool>());
        if (args.contains("tag")) e.setTag(arg(args, "tag"));
        if (args.contains("parent")) {
            const std::string p = arg(args, "parent");
            if (!e.setParent(p.empty() ? ecs::Entity{} : entity(p), true)) throw ToolError("no se puede emparentar (crearia un ciclo)");
        }
        if (args.contains("position")) e.setWorldPosition(readVec(args, "position", e.worldPosition()));
        if (args.contains("local_position")) e.setLocalPosition(readVec(args, "local_position", e.localPosition()));
        if (args.contains("rotation")) e.setLocalEulerDegrees(readVec(args, "rotation", e.localEulerDegrees()));
        if (args.contains("scale")) e.setLocalScale(readVec(args, "scale", e.localScale()));
        a.commit();
        return summary(e);
    }
    if (name == "delete_entity") {
        const ecs::Entity e = entity(arg(args, "entity"));
        const std::string n = e.name();
        a.world_.destroy(e);
        a.clearSelection();
        a.commit();
        return json{{"deleted", n}};
    }
    if (name == "duplicate_entity") {
        const ecs::Entity e = entity(arg(args, "entity"));
        ecs::Entity copy = a.world_.duplicate(e);
        ecs::detachCopiedLinks(a.world_, copy);
        a.commit();
        return summary(copy);
    }
    if (name == "list_component_types") {
        json out = json::array();
        ecs::World scratch;
        for (const ecs::ComponentType& type : ecs::ComponentRegistry::instance().types()) {
            if (type.name == "EntityInfo" || type.name == "PrefabInstance" || type.name == "PrefabLink") continue;
            ecs::Entity x = scratch.create("x");
            if (!type.has(scratch, x.handle())) type.add(scratch, x.handle());
            const std::string defaults = ecs::componentToJson(scratch, x, type.name);
            out.push_back(json{{"name", type.name}, {"label", type.label}, {"category", type.category},
                               {"fields", defaults.empty() ? json::object() : json::parse(defaults, nullptr, false)}});
            scratch.destroy(x);
        }
        return out;
    }
    if (name == "set_component") {
        ecs::Entity e = entity(arg(args, "entity"));
        const std::string component = arg(args, "component");
        std::string error;
        if (!ecs::componentFromJson(a.world_, e, component, args.contains("values") ? args["values"].dump() : std::string("{}"), &error)) {
            throw ToolError(error);
        }
        a.commit();
        const std::string now = ecs::componentToJson(a.world_, e, component);
        return json{{"entity", e.name()}, {"component", component}, {"values", json::parse(now.empty() ? "{}" : now, nullptr, false)}};
    }
    if (name == "remove_component") {
        ecs::Entity e = entity(arg(args, "entity"));
        const ecs::ComponentType* type = ecs::ComponentRegistry::instance().find(arg(args, "component"));
        if (type == nullptr || !type->has(a.world_, e.handle())) throw ToolError("la entidad no tiene ese componente");
        if (!type->removable) throw ToolError("ese componente no se puede quitar");
        type->remove(a.world_, e.handle());
        a.commit();
        return summary(e);
    }
    if (name == "select") {
        const ecs::Entity e = entity(arg(args, "entity"));
        a.selectOnly(e.uuid());
        a.revealInHierarchy(e.uuid());
        if (args.value("focus", true)) a.focusSelection();
        return summary(e);
    }
    if (name == "set_camera") {
        a.scene_.placeCamera(readVec(args, "position", {}), readVec(args, "target", {}));
        return json{{"ok", true}};
    }

    if (name == "list_assets") {
        const std::string type_filter = arg(args, "type");
        std::filesystem::path folder = a.project_.assetsFolder();
        if (args.contains("folder") && !arg(args, "folder").empty()) folder = assetPath(arg(args, "folder"));
        json list = json::array();
        for (const assets::AssetInfo& info : a.database_->all()) {
            if (info.path.empty()) continue;
            const std::string type = assets::assetTypeName(info.type);
            if (!type_filter.empty()) {
                static const std::map<std::string, assets::AssetType> names = {
                    {"Model", assets::AssetType::Model}, {"Environment", assets::AssetType::Environment},
                    {"Scene", assets::AssetType::Scene}, {"Material", assets::AssetType::Material},
                    {"Prefab", assets::AssetType::Prefab}, {"AnimatorController", assets::AssetType::AnimatorController},
                    {"AnimationClip", assets::AssetType::AnimationClip}};
                const auto it = names.find(type_filter);
                if (it != names.end() && it->second != info.type) continue;
            }
            const std::string rel = a.assetRelative(info.path);
            const std::string folder_rel = a.assetRelative(folder);
            if (!folder_rel.empty() && rel.rfind(folder_rel, 0) != 0) continue;
            list.push_back(json{{"name", info.name}, {"type", type}, {"path", rel}, {"uuid", info.uuid.toString()}});
        }
        json files = json::array();
        std::error_code ec;
        int budget = 800;
        for (auto it = std::filesystem::recursive_directory_iterator(folder, ec);
             !ec && it != std::filesystem::recursive_directory_iterator() && budget > 0; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const std::string ext = it->path().extension().string();
            if (ext == ".crdata" || ext == ".crscene" || ext == ".crmat" || ext == ".crprefab" || ext == ".tmp") continue;
            files.push_back(a.assetRelative(it->path()));
            --budget;
        }
        return json{{"assets", list}, {"files", files}};
    }
    if (name == "read_file") {
        const std::filesystem::path file = assetPath(arg(args, "path"));
        std::error_code ec;
        if (!std::filesystem::is_regular_file(file, ec)) throw ToolError("no existe " + arg(args, "path"));
        if (std::filesystem::file_size(file, ec) > (2u << 20)) throw ToolError("archivo demasiado grande (o binario)");
        return readText(file);
    }
    if (name == "write_file") {
        const std::filesystem::path file = assetPath(arg(args, "path"));
        std::filesystem::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary | std::ios::trunc) << arg(args, "content");
        afterWrite(file);
        json out{{"written", a.assetRelative(file)}};
        if (file.extension() == assets::kSurfaceShaderExtension && a.sync_) {
            a.sync_->surfaceShader(a.assetRelative(file));
            const std::string error = a.sync_->surfaceShaderError(a.assetRelative(file));
            if (!error.empty()) out["shader_error"] = error;
        }
        return out;
    }
    if (name == "delete_file") {
        const std::filesystem::path file = assetPath(arg(args, "path"));
        if (const auto info = findAsset(arg(args, "path")); info && !info->path.empty()) {
            a.database_->remove(info->uuid);
        } else {
            std::error_code ec;
            if (!std::filesystem::remove(file, ec)) throw ToolError("no se pudo borrar " + arg(args, "path"));
        }
        a.refreshDatabase();
        return json{{"deleted", arg(args, "path")}};
    }
    if (name == "create_script") {
        const std::string script = safeName(arg(args, "name"));
        const std::filesystem::path file = a.project_.assetsFolder() / "Scripts" / dialogs::fromUtf8(script + ".lua");
        std::filesystem::create_directories(file.parent_path());
        const std::string code = args.contains("code") ? arg(args, "code") : scripting::scriptTemplate(script);
        std::ofstream(file, std::ios::binary | std::ios::trunc) << code;
        afterWrite(file);
        json out{{"path", a.assetRelative(file)}};
        if (args.contains("attach_to")) {
            ecs::Entity e = entity(arg(args, "attach_to"));
            scripting::Script& s = e.has<scripting::Script>() ? e.get<scripting::Script>() : e.add<scripting::Script>();
            s.file = a.assetRelative(file);
            a.commit();
            out["attached_to"] = e.name();
        }
        std::string error;
        a.scripts_.describe(a.assetRelative(file), &error);
        if (!error.empty()) out["error"] = error;
        return out;
    }
    if (name == "create_shader") {
        const std::string shader = safeName(arg(args, "name"));
        const std::filesystem::path file =
            a.project_.assetsFolder() / "Shaders" / dialogs::fromUtf8(shader + assets::kSurfaceShaderExtension);
        std::filesystem::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary | std::ios::trunc)
            << (args.contains("code") ? arg(args, "code") : assets::surfaceShaderTemplate(shader));
        afterWrite(file);
        const std::string rel = a.assetRelative(file);
        json out{{"path", rel}};
        if (a.sync_) {
            a.sync_->surfaceShader(rel);
            const std::string error = a.sync_->surfaceShaderError(rel);
            out["compiled"] = error.empty();
            if (!error.empty()) out["error"] = error;
        }
        return out;
    }
    if (name == "create_material") {
        assets::MaterialAsset m;
        if (args.contains("color") && args["color"].is_array() && args["color"].size() >= 3) {
            const json& c = args["color"];
            m.base_color = core::Vec4{c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c.size() > 3 ? c[3].get<float>() : 1.0f};
        }
        m.metallic = args.value("metallic", m.metallic);
        m.roughness = args.value("roughness", m.roughness);
        if (args.contains("emissive")) m.emissive = readVec(args, "emissive", {});
        m.emissive_intensity = args.value("emissive_intensity", m.emissive_intensity);
        m.albedo = arg(args, "albedo_texture");
        m.normal = arg(args, "normal_texture");
        if (args.value("transparent", false)) m.mode = assets::MaterialMode::Transparent;
        if (args.contains("tiling") && args["tiling"].is_array() && args["tiling"].size() >= 2) {
            m.tiling = core::Vec2{args["tiling"][0].get<float>(), args["tiling"][1].get<float>()};
        }
        m.shader = arg(args, "shader");
        if (m.shader.rfind("Assets/", 0) == 0) m.shader = m.shader.substr(7);
        if (args.contains("shader_values") && args["shader_values"].is_object()) {
            for (const auto& [key, v] : args["shader_values"].items()) {
                core::Vec4 value{};
                if (v.is_number()) value.x = v.get<float>();
                else if (v.is_array()) {
                    float* out = &value.x;
                    for (std::size_t i = 0; i < v.size() && i < 4; ++i) out[i] = v[i].get<float>();
                }
                m.shader_values[key] = value;
            }
        }
        const std::filesystem::path file = a.project_.assetsFolder() / "Materials" / dialogs::fromUtf8(safeName(arg(args, "name")) + ".crmat");
        std::string error;
        if (!assets::saveMaterial(m, file, &error)) throw ToolError(error);
        afterWrite(file);
        json out{{"path", a.assetRelative(file)}, {"uuid", m.uuid.toString()}};
        if (!m.shader.empty() && a.sync_) {
            a.sync_->surfaceShader(m.shader);
            const std::string shader_error = a.sync_->surfaceShaderError(m.shader);
            if (!shader_error.empty()) out["shader_error"] = shader_error;
        }
        return out;
    }
    if (name == "assign_material") {
        ecs::Entity e = entity(arg(args, "entity"));
        const auto info = findAsset(arg(args, "material"));
        if (!info || info->type != assets::AssetType::Material) throw ToolError("no existe el material " + arg(args, "material"));
        if (!a.applyMaterial(e, info->uuid, args.value("slot", -1))) throw ToolError("la entidad no tiene malla donde poner el material");
        a.commit();
        return json{{"entity", e.name()}, {"material", info->name}};
    }
    if (name == "reimport_model") {
        const auto info = findAsset(arg(args, "asset"));
        if (!info || info->type != assets::AssetType::Model) throw ToolError("no existe el modelo " + arg(args, "asset"));
        if (!a.startReimport(info->uuid)) throw ToolError("no se pudo empezar (ya se esta reimportando o no hay proyecto)");
        return json{{"started", true}, {"model", info->name}};
    }
    if (name == "performance_stats") {
        json passes = json::array();
        for (const gfx::GpuTiming& t : a.renderer_.gpuProfiler().timings()) {
            passes.push_back(json{{"pass", t.name}, {"ms", t.milliseconds}});
        }
        return json{{"fps", a.profiler_overlay_.fps()},
                    {"cpu_ms", a.profiler_overlay_.cpuMilliseconds()},
                    {"gpu_ms", a.renderer_.gpuProfiler().totalMilliseconds()},
                    {"gpu_passes", passes},
                    {"actors", a.scene_.actors().size()},
                    {"models", a.scene_.models().size()},
                    {"entities", a.world_.entityCount()},
                    {"triangles", a.renderer_.triangleCount()},
                    {"visible_submeshes", a.renderer_.visibleSubmeshes()},
                    {"material_batches", a.renderer_.batchCount()},
                    {"shadow_draw_calls", a.renderer_.shadowDrawCalls()},
                    {"lod_triangles", a.renderer_.lodTriangles()},
                    {"lod_actors", a.renderer_.lodActors()}};
    }
    if (name == "import_file") {
        const std::filesystem::path source = dialogs::fromUtf8(arg(args, "path"));
        std::error_code ec;
        if (!std::filesystem::is_regular_file(source, ec)) throw ToolError("no existe el archivo " + arg(args, "path"));
        const std::filesystem::path target = assetPath(arg(args, "folder", "Models"));
        std::filesystem::create_directories(target);
        const assets::ImportResult result = assets::importAny(source, target);
        if (!result.ok) throw ToolError("no se pudo importar: " + result.message);
        a.refreshDatabase();
        return json{{"name", result.info.name}, {"uuid", result.info.uuid.toString()}, {"type", assets::assetTypeName(result.info.type)},
                    {"path", a.assetRelative(result.info.path)}};
    }
    if (name == "create_model") {
        const std::string model = safeName(arg(args, "name"));
        const std::filesystem::path temp = a.project_.libraryFolder() / "McpModels" / dialogs::fromUtf8(model);
        std::filesystem::create_directories(temp);
        const std::filesystem::path obj = temp / dialogs::fromUtf8(model + ".obj");
        std::string obj_text = arg(args, "obj");
        if (args.contains("mtl")) {
            std::ofstream(temp / dialogs::fromUtf8(model + ".mtl"), std::ios::binary | std::ios::trunc) << arg(args, "mtl");
            if (obj_text.find("mtllib") == std::string::npos) obj_text = "mtllib " + model + ".mtl\n" + obj_text;
        }
        std::ofstream(obj, std::ios::binary | std::ios::trunc) << obj_text;
        const std::filesystem::path target = a.project_.assetsFolder() / "Models";
        std::filesystem::create_directories(target);
        const assets::ImportResult result = assets::importAny(obj, target);
        if (!result.ok) throw ToolError("el OBJ no se pudo importar: " + result.message);
        a.refreshDatabase();
        return json{{"name", result.info.name}, {"uuid", result.info.uuid.toString()}, {"path", a.assetRelative(result.info.path)},
                    {"hint", "usa instantiate con este uuid para ponerlo en la escena"}};
    }
    if (name == "instantiate") {
        const auto info = findAsset(arg(args, "asset"));
        if (!info) throw ToolError("no existe el asset " + arg(args, "asset") + " (usa list_assets)");
        const ecs::Entity parent = args.contains("parent") && !arg(args, "parent").empty() ? entity(arg(args, "parent")) : ecs::Entity{};
        const std::optional<Vec3> position = args.contains("position") ? std::optional<Vec3>(readVec(args, "position", {})) : std::nullopt;
        ecs::Entity e;
        if (info->type == assets::AssetType::Model) e = a.instantiateAsset(info->uuid, parent, position);
        else if (info->type == assets::AssetType::Prefab) e = a.instantiatePrefabAsset(info->uuid, parent, position);
        else if (info->type == assets::AssetType::Environment) {
            a.assignEnvironment(info->uuid);
            return json{{"sky", info->name}};
        } else {
            throw ToolError("ese asset no se instancia (tipo " + std::string(assets::assetTypeName(info->type)) + ")");
        }
        if (!e.valid()) throw ToolError("no se pudo instanciar");
        if (args.contains("name")) {
            e.setName(arg(args, "name"));
            a.commit();
        }
        return summary(e);
    }
    if (name == "create_prefab") {
        const ecs::Entity e = entity(arg(args, "entity"));
        a.selectOnly(e.uuid());
        a.createPrefabsFromSelection(args.contains("folder") ? assetPath(arg(args, "folder")) : std::filesystem::path{});
        if (!e.has<ecs::PrefabInstance>()) throw ToolError("no se pudo crear el prefab (mira get_console)");
        const std::filesystem::path path = a.prefabPath(e.get<ecs::PrefabInstance>().prefab.uuid);
        return json{{"prefab", a.assetRelative(path)}, {"entity", e.name()}};
    }
    if (name == "list_scenes") {
        json out = json::array();
        for (const assets::AssetInfo& info : a.database_->all()) {
            if (info.type == assets::AssetType::Scene) out.push_back(json{{"name", info.name}, {"path", a.assetRelative(info.path)}});
        }
        return json{{"current", a.scene_path_.empty() ? json(nullptr) : json(a.assetRelative(a.scene_path_))}, {"scenes", out}};
    }
    if (name == "new_scene") {
        if (a.playing()) a.exitPlay();
        if (unsavedBlocks(args)) throw ToolError("hay cambios sin guardar: save_scene o force=true");
        a.newScene();
        return json{{"scene", a.world_.sceneName()}};
    }
    if (name == "open_scene") {
        if (a.playing()) a.exitPlay();
        if (unsavedBlocks(args)) throw ToolError("hay cambios sin guardar: save_scene o force=true");
        const auto info = findAsset(arg(args, "scene"));
        if (!info || info->type != assets::AssetType::Scene) throw ToolError("no existe la escena " + arg(args, "scene"));
        if (!a.openScene(info->path)) throw ToolError("no se pudo abrir la escena");
        return json{{"scene", a.world_.sceneName()}, {"entities", a.world_.entityCount()}};
    }
    if (name == "save_scene") {
        if (a.playing()) throw ToolError("sal del modo Play (stop) antes de guardar");
        if (args.contains("path") && !arg(args, "path").empty()) {
            std::filesystem::path file = assetPath(arg(args, "path"));
            if (file.extension() != ".crscene") file += ".crscene";
            std::filesystem::create_directories(file.parent_path());
            a.scene_path_ = file;
            a.world_.setSceneName(dialogs::utf8(file.stem()));
        } else if (a.scene_path_.empty()) {
            const std::filesystem::path file = a.project_.assetsFolder() / "Scenes" /
                                               dialogs::fromUtf8(safeName(a.world_.sceneName()) + ".crscene");
            std::filesystem::create_directories(file.parent_path());
            a.scene_path_ = file;
        }
        if (!a.saveScene()) throw ToolError("no se pudo guardar (mira get_console)");
        return json{{"saved", a.assetRelative(a.scene_path_)}};
    }
    if (name == "play") {
        if (!a.playing()) a.enterPlay();
        return json{{"mode", "play"}};
    }
    if (name == "stop") {
        if (a.playing()) a.exitPlay();
        return json{{"mode", "edit"}};
    }
    if (name == "pause") {
        if (!a.playing()) throw ToolError("no esta en modo Play");
        a.togglePause();
        return json{{"mode", a.play_state_ == EditorApp::PlayState::Paused ? "paused" : "play"}};
    }
    if (name == "run_lua") {
        std::string out;
        const bool ok = a.scripts_.run(arg(args, "code"), &out);
        if (!ok) throw ToolError("error de Lua: " + out);
        return json{{"result", out}};
    }
    if (name == "get_console") {
        const int lines = std::max(1, args.value("lines", 60));
        const std::string level = arg(args, "level", "all");
        EditorLog& log = EditorLog::instance();
        std::lock_guard<std::mutex> lock(log.mutex());
        std::vector<std::string> picked;
        const auto& entries = log.entries();
        for (auto it = entries.rbegin(); it != entries.rend() && static_cast<int>(picked.size()) < lines; ++it) {
            if (level == "errors" && it->level != EditorLog::Level::Error) continue;
            if (level == "warnings" && it->level == EditorLog::Level::Info) continue;
            const char* tag = it->level == EditorLog::Level::Error ? "[ERROR] " : it->level == EditorLog::Level::Warning ? "[AVISO] " : "";
            picked.push_back(tag + it->text);
        }
        std::string text;
        for (auto it = picked.rbegin(); it != picked.rend(); ++it) text += *it + "\n";
        return text.empty() ? std::string("(consola vacia)") : text;
    }
    if (name == "screenshot") {
        const std::filesystem::path png = std::filesystem::temp_directory_path() / "cramion_mcp_screenshot.png";
        const std::vector<std::uint8_t> pixels = captureEditorWindow(a.window_.handle(), png);
        if (pixels.empty()) throw ToolError("no se pudo capturar la ventana (minimizada?)");
        image = true;
        image_data = base64(readBytes(png));
        return json{};
    }
    if (name == "undo") {
        a.undo();
        return json{{"ok", true}};
    }
    if (name == "redo") {
        a.redo();
        return json{{"ok", true}};
    }
    throw ToolError("herramienta desconocida: " + name);
}

// --- JSON-RPC ------------------------------------------------------------------

namespace {

json rpcError(const json& id, int code, const std::string& message) {
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

}  // namespace

std::string EditorApp::handleMcp(const std::string& body) {
    const json request = json::parse(body, nullptr, false);
    if (request.is_discarded()) return rpcError(nullptr, -32700, "JSON no valido").dump();

    McpTools tools(*this);
    const auto one = [&](const json& msg) -> json {
        if (!msg.is_object() || !msg.contains("method")) return nullptr;  // respuesta del cliente: nada
        const bool notification = !msg.contains("id");
        const json id = notification ? json(nullptr) : msg["id"];
        const std::string method = msg.value("method", std::string{});
        const json params = msg.contains("params") ? msg["params"] : json::object();
        if (notification) return nullptr;

        if (method == "initialize") {
            std::string version = kProtocolVersions[1];
            const std::string wanted = params.value("protocolVersion", std::string{});
            for (const char* v : kProtocolVersions) {
                if (wanted == v) version = v;
            }
            const std::string client = params.contains("clientInfo") ? params["clientInfo"].value("name", std::string("?")) : "?";
            mcpLog("conectado: " + client);
            return json{{"jsonrpc", "2.0"}, {"id", id},
                        {"result", {{"protocolVersion", version},
                                    {"capabilities", {{"tools", {{"listChanged", false}}}}},
                                    {"serverInfo", {{"name", "cramion-editor"}, {"version", "0.5.0"}}},
                                    {"instructions", "Editor del motor Cramion. Llama a la herramienta 'help' para la guia y a "
                                                     "'editor_state' para ver que hay abierto."}}}};
        }
        if (method == "ping") return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", json::object()}};
        if (method == "tools/list") {
            json list = json::array();
            for (const ToolDef& t : toolDefs()) {
                json schema{{"type", "object"}, {"properties", t.properties.is_object() ? t.properties : json::object()}};
                if (!t.required.empty()) schema["required"] = t.required;
                list.push_back(json{{"name", t.name}, {"description", t.description}, {"inputSchema", schema}});
            }
            return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"tools", list}}}};
        }
        if (method == "tools/call") {
            const std::string name = params.value("name", std::string{});
            const json args = params.contains("arguments") && params["arguments"].is_object() ? params["arguments"] : json::object();
            json content = json::array();
            bool is_error = false;
            try {
                bool image = false;
                std::string image_data;
                const json result = tools.call(name, args, image, image_data);
                if (image) {
                    content.push_back(json{{"type", "image"}, {"data", image_data}, {"mimeType", "image/png"}});
                } else {
                    content.push_back(json{{"type", "text"}, {"text", result.is_string() ? result.get<std::string>() : result.dump(2)}});
                }
                mcpLog(name);
            } catch (const std::exception& e) {
                is_error = true;
                content.push_back(json{{"type", "text"}, {"text", std::string("Error: ") + e.what()}});
                mcpLog(name + " -> error: " + e.what());
            }
            return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"content", content}, {"isError", is_error}}}};
        }
        if (method == "resources/list") return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"resources", json::array()}}}};
        if (method == "prompts/list") return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"prompts", json::array()}}}};
        return rpcError(id, -32601, "metodo no soportado: " + method);
    };

    if (request.is_array()) {
        json out = json::array();
        for (const json& msg : request) {
            json r = one(msg);
            if (!r.is_null()) out.push_back(std::move(r));
        }
        return out.empty() ? std::string{} : out.dump();
    }
    const json r = one(request);
    return r.is_null() ? std::string{} : r.dump();
}

// --- Servidor y ventana ------------------------------------------------------------

void EditorApp::mcpLog(const std::string& text) {
    const std::time_t now = std::time(nullptr);
    char stamp[16];
    std::strftime(stamp, sizeof(stamp), "%H:%M:%S", std::localtime(&now));
    mcp_log_.push_back(std::string(stamp) + "  " + text);
    while (mcp_log_.size() > 200) mcp_log_.pop_front();
    std::cout << "[MCP] " << text << "\n";
}

void EditorApp::startMcp() {
    int port = 7777;
    if (const char* env = std::getenv("CRAMION_MCP_PORT"); env != nullptr && std::atoi(env) > 0) port = std::atoi(env);
    mcp_error_.clear();
    // Si el puerto esta ocupado (otro editor abierto), los siguientes.
    for (int p = port; p < port + 10; ++p) {
        std::string error;
        if (mcp_.start(static_cast<std::uint16_t>(p), &error)) {
            std::cout << "[MCP] Servidor para IA en http://127.0.0.1:" << p << "/mcp\n";
            return;
        }
        mcp_error_ = error;
    }
    std::cerr << "[MCP] No se pudo abrir el servidor: " << mcp_error_ << "\n";
}

void EditorApp::pollMcp() {
    mcp_.poll([this](const std::string& body) { return handleMcp(body); });
}

void EditorApp::drawMcpWindow() {
    if (!show_mcp_) return;
    ImGui::SetNextWindowSize(ImVec2(620.0f, 520.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("MCP (IA)", &show_mcp_)) {
        ImGui::End();
        return;
    }
    ImGui::TextWrapped("Conecta una IA (Claude, Cursor, VS Code...) al editor: podrá crear escenas, objetos, scripts, "
                       "shaders, materiales y modelos, darle a Play y ver capturas.");
    ImGui::Spacing();
    const std::string url = "http://127.0.0.1:" + std::to_string(mcp_.port()) + "/mcp";
    if (mcp_.running()) {
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "Activo");
        ImGui::SameLine();
        ImGui::TextUnformatted(url.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(%llu peticiones)", static_cast<unsigned long long>(mcp_.requestCount()));
        if (ImGui::Button("Apagar")) mcp_.stop();
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "Apagado");
        if (!mcp_error_.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", mcp_error_.c_str());
        }
        if (ImGui::Button("Encender")) startMcp();
    }
    ImGui::TextDisabled("Solo acepta conexiones de este PC.");

    const auto snippet = [](const char* title, const std::string& text) {
        ImGui::SeparatorText(title);
        ImGui::PushID(title);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::SmallButton("Copiar")) ImGui::SetClipboardText(text.c_str());
        ImGui::PopID();
    };
    snippet("Claude Code (terminal)", "claude mcp add --transport http cramion " + url);
    snippet("Cursor / VS Code / Windsurf (mcp.json)", "{\n  \"mcpServers\": {\n    \"cramion\": { \"url\": \"" + url + "\" }\n  }\n}");
    std::string bridge = dialogs::utf8(gfx::shaders::directory().parent_path() / "CramionMcp.exe");
    std::string escaped;
    for (char c : bridge) {
        escaped += c;
        if (c == '\\') escaped += '\\';
    }
    snippet("Claude Desktop (claude_desktop_config.json, por stdio)",
            "{\n  \"mcpServers\": {\n    \"cramion\": {\n      \"command\": \"" + escaped + "\",\n      \"args\": [\"--port\", \"" +
                std::to_string(mcp_.port()) + "\"]\n    }\n  }\n}");

    ImGui::SeparatorText("Actividad");
    ImGui::BeginChild("##mcp_log", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    for (const std::string& line : mcp_log_) ImGui::TextUnformatted(line.c_str());
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::End();
}

}  // namespace cramion::editor

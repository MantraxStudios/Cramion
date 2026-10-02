// MCP (Model Context Protocol) del editor: las herramientas con las que una IA
// trabaja en el motor (escenas, objetos, componentes, scripts, shaders,
// materiales, modelos, prefabs, Play, Lua, consola y capturas). El transporte
// esta en McpServer; aqui el JSON-RPC y cada herramienta, siempre en el hilo
// principal y con deshacer como si lo hiciera el usuario.

#include "EditorApp.h"

#include <CramionCore/terrain/TerrainGenerator.h>
#include <CramionCore/environment/Environment.h>
#include <CramionCore/cvar/CVar.h>

#include "CramionCore/project/DataPack.h"

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

std::uint64_t vramMb(const gfx::VulkanRenderer& renderer, bool used) {
    std::uint64_t u = 0;
    std::uint64_t b = 0;
    if (!renderer.device().videoMemory(u, b)) return 0;
    return (used ? u : b) >> 20;
}

// Estado del presupuesto adaptativo (performance_stats, graphics_settings).
json budgetJson(const gfx::VulkanRenderer& renderer) {
    const gfx::FrameBudget& budget = renderer.frameBudget();
    const gfx::HardwareProfile& hw = renderer.hardwareProfile();
    json levers = json::object();
    for (std::size_t i = 0; i < gfx::kLeverCount; ++i) {
        const auto lever = static_cast<gfx::Lever>(i);
        levers[gfx::FrameBudget::leverName(lever)] =
            std::to_string(budget.level(lever)) + "/" + std::to_string(budget.maxLevel(lever));
    }
    return json{{"enabled", budget.enabled()},
                {"target_fps", budget.targetFps()},
                {"smoothed_gpu_ms", budget.smoothedGpuMs()},
                {"hardware_tier", gfx::tierName(hw.tier)},
                {"gpu", hw.gpu_name},
                {"vram_mb", hw.vram_mb},
                {"levers", levers},
                {"last_action", budget.lastAction()},
                {"texture_size_limit", renderer.textureSizeLimit()},
                {"vram_used_mb", vramMb(renderer, true)},
                {"vram_budget_mb", vramMb(renderer, false)}};
}

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
        d.push_back({"open_project", "Abre un proyecto (.crproj o su carpeta).", {{"path", prop("string", "Ruta del .crproj o de la carpeta del proyecto")}, {"force", prop("boolean", "Descartar cambios sin guardar")}, {"show_loading", prop("boolean", "Abrir como el Hub, con el dialogo de carga (vuelve enseguida)")}}, {"path"}});
        d.push_back({"create_project", "Crea un proyecto nuevo desde una plantilla y lo abre.", {{"name", prop("string", "Nombre del proyecto")}, {"folder", prop("string", "Carpeta donde crearlo (por defecto Documentos/Cramion Projects)")}, {"template", prop("string", "Id de la plantilla (ver list_projects); por defecto 'blank'")}}, {"name"}});
        d.push_back({"list_entities", "Arbol de la escena: cada entidad con su UUID, nombre, componentes e hijos.", {{"root", entity}, {"depth", prop("integer", "Profundidad maxima (por defecto 12)")}}, {}});
        d.push_back({"get_entity", "Todo de una entidad: transformacion y cada componente con sus campos (JSON).", {{"entity", entity}}, {"entity"}});
        d.push_back({"create_entity", "Crea una entidad. type: empty, cube, sphere, plane, cylinder, capsule, directional_light, point_light, spot_light, camera, decal, vehicle, character (Character Controller), terrain, voxel_world, ocean, lake, river. Opcional: posicion, giro (grados), escala, padre y componentes {\"Tipo\": {campos}}.",
                     {{"type", prop("string", "Que crear (por defecto empty)")}, {"name", prop("string", "Nombre")}, {"parent", entity},
                      {"position", vec3Prop("Posicion en el mundo")}, {"rotation", vec3Prop("Giro local en grados (X, Y, Z)")}, {"scale", vec3Prop("Escala local")},
                      {"components", prop("object", "Componentes a anadir/ajustar: {\"Light\": {\"intensity\": 5}, \"Rigidbody\": {}}")}}, {}});
        d.push_back({"modify_entity", "Cambia nombre, activo, tag, padre, posicion (mundo), posicion local, giro (grados) o escala de una entidad.",
                     {{"entity", entity}, {"name", prop("string", "Nombre nuevo")}, {"active", prop("boolean", "Activa")}, {"tag", prop("string", "Tag")},
                      {"parent", prop("string", "Nuevo padre (UUID/nombre; vacio = raiz)")}, {"position", vec3Prop("Posicion en el mundo")},
                      {"local_position", vec3Prop("Posicion local")}, {"rotation", vec3Prop("Giro local en grados")}, {"scale", vec3Prop("Escala local")}}, {"entity"}});
        d.push_back({"delete_entity", "Borra una entidad y sus hijos.", {{"entity", entity}}, {"entity"}});
        d.push_back({"cvar_list", "Variables de configuracion (CVars) del motor y del juego: nombre, valor, por defecto, tipo, limites y descripcion.",
                     {{"filter", prop("string", "Solo las que contengan este texto")}}, {}});
        d.push_back({"cvar_set", "Cambia una CVar (o la vuelve a su valor por defecto con reset). Las 'guardadas' se escriben en ProjectSettings/CVars.json.",
                     {{"name", prop("string", "Nombre (p. ej. script.cpp.TimeoutMs)")}, {"value", prop("string", "Valor nuevo")},
                      {"reset", prop("boolean", "Volver al valor por defecto")}}, {"name"}});
        d.push_back({"cpp_compile", "Scripts de C++ (Assets/**/*.cpp, aislados en un proceso aparte): compila ahora y devuelve los errores (archivo:linea), las clases y los fallos en Play. Con 'create' crea un script nuevo desde la plantilla (y con 'entity' se lo pone).",
                     {{"create", prop("string", "Nombre de la clase de un script nuevo (Assets/Scripts/<Nombre>.h y .cpp)")}, {"entity", entity},
                      {"ask_name", prop("boolean", "Abrir la ventana del editor que pide el nombre")}}, {}});
        d.push_back({"cpp_intellisense", "IntelliSense de C++ (clangd) en el editor: abre el archivo en el editor de codigo (opcionalmente con otro texto, sin guardar), y devuelve el estado de clangd, los errores/avisos y las sugerencias e informacion en una posicion (linea y columna desde 1).",
                     {{"file", prop("string", "Archivo dentro de Assets (p. ej. Scripts/Jugador.cpp)")}, {"text", prop("string", "Texto a poner en la pestana (sin guardar)")},
                      {"line", prop("integer", "Linea (desde 1) para sugerencias e informacion")}, {"column", prop("integer", "Columna (desde 1)")},
                      {"wait", prop("number", "Segundos maximos de espera (20)")},
                      {"cursor", prop("integer", "Poner el cursor en este caracter (-1 = al final) y activar el campo")},
                      {"type", prop("string", "Teclear este texto en el campo (un caracter por frame, con las sugerencias de verdad)")},
                      {"format", prop("boolean", "Formatear con clang-format (el estilo de Configuracion del motor)")},
                      {"settings", prop("boolean", "Mostrar u ocultar la ventana Configuracion del motor")}}, {"file"}});
        d.push_back({"modeling_create", "Modelado poligonal (como ProBuilder): crea una malla editable con una forma. shape: cube, plane, cylinder, cone, sphere, icosphere, torus, pipe, prism, wedge (rampa), stairs, curved_stairs, arch, door (pared con hueco), room (interior), capsule. La base queda en y = 0 del objeto, centrada en X/Z.",
                     {{"shape", prop("string", "Forma")}, {"name", prop("string", "Nombre")}, {"position", vec3Prop("Posicion de la base en el mundo")},
                      {"size", vec3Prop("Ancho (X), alto (Y), fondo (Z) en metros")}, {"segments", prop("integer", "Lados (cilindros, esferas, arcos...)")},
                      {"rings", prop("integer", "Anillos (esfera, toro, capsula) o subdivisiones de la icoesfera")},
                      {"subdivisions", vec3Prop("Cortes por eje (cubo, plano, habitacion)")}, {"steps", prop("integer", "Peldanos (escaleras)")},
                      {"thickness", prop("number", "Grosor (tubo, arco, losa de escalera) o radio del tubo del toro")},
                      {"inner_radius", prop("number", "Radio interior (escalera curva)")}, {"angle", prop("number", "Grados (arco, escalera curva)")},
                      {"solid", prop("boolean", "Escaleras macizas hasta el suelo (por defecto si)")}, {"smooth", prop("boolean", "Lados suaves")},
                      {"door", prop("array", "Puerta: hueco [fraccion del ancho, fraccion del alto]")}, {"parent", entity},
                      {"color", vec3Prop("Color del hueco 0 (0..1)")}}, {"shape"}});
        d.push_back({"modeling_info", "Malla editable de una entidad: vertices, aristas y caras con su indice, centro, normal (en el objeto) y material. Usalo para elegir que caras/aristas editar con modeling_edit.",
                     {{"entity", entity}, {"max_faces", prop("integer", "Maximo de caras listadas (por defecto 300)")}, {"vertices", prop("boolean", "Listar tambien las posiciones de los vertices")}}, {"entity"}});
        d.push_back({"modeling_edit", "Opera sobre una malla editable. operation: extrude, inset, bevel, subdivide, connect, insert_loop, subdivide_edges, bridge, fill_holes, merge, delete, flip, triangulate, duplicate, detach, weld, collapse, split, move, rotate, scale, relax, noise, smooth (Catmull-Clark), mirror, conform, center_pivot, material, smoothing, uv, boolean, combine. Caras: indices o un selector 'all', 'up', 'down', 'front' (+Z), 'back', 'left', 'right' (por la normal). Aristas: [[a,b],...] o 'border'. Vertices: indices o 'all'.",
                     {{"entity", entity}, {"operation", prop("string", "La operacion")},
                      {"faces", prop("array", "Caras (indices) o un selector en texto")}, {"edges", prop("array", "Aristas [[a,b],...] o 'border'")},
                      {"vertices", prop("array", "Vertices (indices) o 'all'")},
                      {"amount", prop("number", "Distancia / cantidad (extruir, inset, bisel, soldar, relajar, ruido)")},
                      {"individual", prop("boolean", "Extruir/inset cara a cara")}, {"cuts", prop("integer", "Cortes o niveles")},
                      {"t", prop("number", "Donde corta insert_loop (0..1)")}, {"vector", vec3Prop("move: desplazamiento; rotate: grados XYZ; scale: factores")},
                      {"axis", prop("string", "Espejo: x, y o z")}, {"keep", prop("boolean", "Espejo: conservar el original y soldar la costura (por defecto si)")},
                      {"other", entity}, {"op", prop("string", "Booleana: union, subtract, intersect")},
                      {"material", prop("integer", "Hueco de material")}, {"color", vec3Prop("Color del hueco (material)")},
                      {"group", prop("integer", "Grupo de suavizado (0 = duro)")},
                      {"uv", prop("object", "UV: {mode: box|planar, fill: tile|fit|stretch, scale:[u,v], offset:[u,v], rotation, world_space}")},
                      {"bottom", prop("boolean", "center_pivot: a la base")}}, {"entity", "operation"}});
        d.push_back({"modeling_convert", "Convierte un objeto con Mesh Renderer (modelo importado o primitiva) en malla editable (suelda vertices y junta triangulos coplanares en quads).",
                     {{"entity", entity}, {"quad_angle", prop("number", "Grados para juntar triangulos en quads (0 = no juntar; por defecto 2)")}}, {"entity"}});
        d.push_back({"duplicate_entity", "Duplica una entidad con sus hijos.", {{"entity", entity}}, {"entity"}});
        d.push_back({"list_component_types", "Todos los tipos de componente del motor con sus campos y valores por defecto (para set_component).", json::object(), {}});
        d.push_back({"set_component", "Anade un componente (si no lo tiene) y cambia sus campos. Los campos que no se pasen no cambian. Ver list_component_types.",
                     {{"entity", entity}, {"component", prop("string", "Nombre del tipo, p. ej. Light, Rigidbody, BoxCollider, Script, Camera")}, {"values", prop("object", "Campos a cambiar")}}, {"entity", "component"}});
        d.push_back({"remove_component", "Quita un componente de una entidad.", {{"entity", entity}, {"component", prop("string", "Nombre del tipo")}}, {"entity", "component"}});
        d.push_back({"select", "Selecciona una entidad en el editor y opcionalmente centra la camara en ella.", {{"entity", entity}, {"focus", prop("boolean", "Centrar la camara")}}, {"entity"}});
        d.push_back({"inspect_asset", "Muestra un asset en el Inspector (material, Render Texture o los ajustes de importacion de un modelo: Scale Factor...), como elegirlo en el Proyecto.", {{"asset", prop("string", "Ruta, nombre o UUID")}}, {"asset"}});
        d.push_back({"set_gizmo", "Cambia el gizmo de la vista de escena: none, move, rotate o scale, si va en ejes locales, y si se ven los iconos y guias.",
                     {{"mode", prop("string", "none | move | rotate | scale")}, {"local", prop("boolean", "Ejes locales (false = mundo)")},
                      {"show_gizmos", prop("boolean", "Mostrar iconos, contornos y volumenes en la vista de escena (false = captura limpia)")}}, {"mode"}});
        d.push_back({"paint_prefabs", "Pinta prefabs con el pincel del editor (arboles, rocas... como el Foliage de Unreal) sobre el suelo alrededor de un punto, o los borra. Usa un grupo .crpaint o una lista de prefabs (misma probabilidad).",
                     {{"center", vec3Prop("Centro del pincel (se busca el suelo debajo)")}, {"radius", prop("number", "Radio en metros (5)")},
                      {"density", prop("number", "Objetos por 100 m2 (8)")}, {"spacing", prop("number", "Separacion minima en metros (1.5)")},
                      {"max_slope", prop("number", "Pendiente maxima en grados (40)")}, {"group", prop("string", "Grupo .crpaint de Assets")},
                      {"prefabs", prop("array", "Nombres, rutas o UUID de prefabs (si no hay grupo)")}, {"align", prop("number", "0 vertical .. 1 sigue el suelo")},
                      {"scale_min", prop("number", "Escala minima (0.9)")}, {"scale_max", prop("number", "Escala maxima (1.1)")},
                      {"erase", prop("boolean", "Borrar en vez de pintar")}}, {"center"}});
        d.push_back({"set_camera", "Coloca la camara del editor.", {{"position", vec3Prop("Posicion")}, {"target", vec3Prop("Punto al que mira")}}, {"position", "target"}});
        d.push_back({"set_weather", "Ambiente (como Enviro): cambia el clima con una transicion suave. Crea el componente Environment (con cielo fisico) si la escena no tiene. Climas: Clear, Cloudy, Overcast, Foggy, LightRain, Rain, Storm, LightSnow, Snow, Blizzard, Sandstorm (tambien en espanol).",
                     {{"weather", prop("string", "Clima (p. ej. Storm, Nieve)")}, {"seconds", prop("number", "Segundos de la transicion (0 = al instante, tambien la humedad y la nieve acumulada)")},
                      {"random", prop("boolean", "Clima al azar segun la estacion")}, {"snow_cover", prop("number", "Nieve acumulada 0..1 al instante")},
                      {"wetness", prop("number", "Humedad de las superficies 0..1 al instante")}, {"puddles", prop("number", "Charcos 0..1 al instante")},
                      {"density", prop("number", "Densidad de gotas y copos 0..3")}}, {}});
        d.push_back({"set_time", "Ambiente: hora, fecha, latitud, estacion y velocidad del tiempo (mueve el sol y la luna por su recorrido real).",
                     {{"hours", prop("number", "Hora del dia 0..24")}, {"day", prop("integer", "Dia 1..31")}, {"month", prop("integer", "Mes 1..12")},
                      {"latitude", prop("number", "Latitud en grados (-89..89)")}, {"day_length", prop("number", "Minutos reales por dia (0 = el tiempo no avanza)")},
                      {"season", prop("string", "Spring, Summer, Autumn, Winter o auto (por la fecha)")}}, {}});
        d.push_back({"set_wind", "Ambiente: viento global (nubes, hierba, arboles, lluvia y nieve).",
                     {{"direction", prop("number", "Grados (0 = hacia +X)")}, {"strength", prop("number", "Multiplica el viento del clima (0..4)")},
                      {"wander", prop("boolean", "La direccion cambia despacio")}}, {}});
        d.push_back({"lightning_strike", "Ambiente: lanza un rayo ahora (destello, trazo y trueno con el retraso de la distancia).",
                     {{"distance", prop("number", "Distancia en metros (sin ella, al azar)")}}, {}});
        d.push_back({"get_environment", "Ambiente: estado actual (clima, transicion, hora, fecha, estacion, temperatura, viento, lluvia, nieve, humedad, particulas dibujadas).", json::object(), {}});
        d.push_back({"list_assets", "Assets del proyecto (modelos, materiales, escenas, prefabs, cielos...) y archivos sueltos (scripts .lua, shaders .crshader, imagenes, audio).",
                     {{"folder", prop("string", "Subcarpeta de Assets (opcional)")}, {"type", prop("string", "Filtro: Model, Material, Scene, Prefab, Environment...")}}, {}});
        d.push_back({"read_file", "Lee un archivo de texto del proyecto (ruta relativa a Assets, p. ej. Scripts/Jugador.lua).", {{"path", prop("string", "Ruta dentro de Assets")}}, {"path"}});
        d.push_back({"write_file", "Escribe (crea o reemplaza) un archivo de texto en Assets: scripts, shaders, materiales, datos. Se recarga en caliente.",
                     {{"path", prop("string", "Ruta dentro de Assets")}, {"content", prop("string", "Contenido completo")}}, {"path", "content"}});
        d.push_back({"delete_file", "Borra un archivo, asset o carpeta (con todo) de Assets: va a la Papelera de reciclaje y se cierran sus pestanas. Con 'ask' muestra la ventana de confirmar del editor.",
                     {{"path", prop("string", "Ruta dentro de Assets")}, {"ask", prop("boolean", "Preguntar en el editor en vez de borrar ya")}}, {"path"}});
        d.push_back({"create_script", "Crea un script Lua (con la plantilla o con el codigo dado) y opcionalmente lo engancha a una entidad.",
                     {{"name", prop("string", "Nombre (sera Scripts/<name>.lua)")}, {"code", prop("string", "Codigo Lua completo (debe terminar en return <tabla>)")}, {"attach_to", entity}}, {"name"}});
        d.push_back({"create_shader", "Crea un shader de superficie GLSL (.crshader) y lo compila. Devuelve los errores si los hay.",
                     {{"name", prop("string", "Nombre (sera Shaders/<name>.crshader)")}, {"code", prop("string", "Codigo del shader (ver help)")}}, {"name"}});
        d.push_back({"create_material", "Crea un material (.crmat): color, metalico, rugosidad, emision, texturas y opcionalmente un shader propio con sus valores.",
                     {{"name", prop("string", "Nombre (sera Materials/<name>.crmat)")}, {"color", prop("array", "Color RGBA lineal 0..1")},
                      {"metallic", prop("number", "0..1")}, {"roughness", prop("number", "0..1")}, {"emissive", vec3Prop("Color de emision")},
                      {"emissive_intensity", prop("number", "Intensidad de la emision")}, {"albedo_texture", prop("string", "Imagen de Assets para el color")},
                      {"normal_texture", prop("string", "Imagen de Assets para el normal map")},
                      {"from_image", prop("string", "Imagen de color de Assets: busca sus companeras por sufijo (_Normal, _Roughness, _AO, _Displacement, _Cavity, _Specular, _Gloss, _Bump) como un pack de Megascans")},
                      {"height_texture", prop("string", "Mapa de alturas / displacement (parallax)")}, {"height_scale", prop("number", "Profundidad del relieve en metros (0.03 por defecto)")},
                      {"relief", prop("string", "Uso del mapa de alturas: parallax (por defecto) o tessellation (la malla sube de verdad: silueta y sombras)")},
                      {"tessellation_density", prop("number", "Teselacion maxima por borde, 1..64 (16 por defecto)")},
                      {"parallax_shadows", prop("boolean", "Auto-sombra del relieve hacia el sol (true por defecto)")},
                      {"shading", prop("string", "Modelo de Disney: standard, clearcoat (barniz), cloth (tela), subsurface (piel/cera/hojas), anisotropic (metal cepillado), transmission (vidrio con refraccion; con transparent)")},
                      {"specular_tint", prop("number", "0..1: el brillo toma el tono del color")},
                      {"clearcoat", prop("number", "0..1 barniz")}, {"clearcoat_roughness", prop("number", "0..1 rugosidad del barniz")},
                      {"sheen", prop("number", "0..1 brillo de tela")}, {"sheen_tint", prop("number", "0..1 tono del sheen")},
                      {"subsurface", prop("number", "0..1")}, {"translucency", prop("number", "0..1 luz a contraluz")},
                      {"subsurface_thickness", prop("number", "Grosor en metros (0.01..0.3)")},
                      {"anisotropy", prop("number", "0..1")}, {"anisotropy_rotation", prop("number", "Grados 0..180")},
                      {"ior", prop("number", "Indice de refraccion 1..2.5 (transmission)")},
                      {"transmission_thickness", prop("number", "Grosor del vidrio en metros 0..0.2")},
                      {"roughness_texture", prop("string", "Mapa de rugosidad")}, {"occlusion_texture", prop("string", "Mapa de oclusion (AO)")},
                      {"cavity_texture", prop("string", "Mapa de cavidad")}, {"specular_texture", prop("string", "Mapa specular")}, {"gloss_texture", prop("string", "Mapa de brillo (gloss)")},
                      {"transparent", prop("boolean", "Modo transparente (vidrio)")},
                      {"shader", prop("string", "Ruta del .crshader en Assets")}, {"shader_values", prop("object", "{propiedad: numero o [x,y,z]}")},
                      {"tiling", prop("array", "Repeticion UV [x, y]")}}, {"name"}});
        d.push_back({"assign_material", "Asigna un material a una entidad (y sus hijos si no tiene malla).",
                     {{"entity", entity}, {"material", prop("string", "Ruta, nombre o UUID del .crmat")}, {"slot", prop("integer", "Hueco de material (-1 = todos)")}}, {"entity", "material"}});
        d.push_back({"reimport_model", "Reimporta un modelo desde su archivo original combinando sus piezas por material (una palmera con cada hoja suelta pasa a tronco + hojas) y rehace sus instancias en la escena. En segundo plano: mira get_console.",
                     {{"asset", prop("string", "Ruta, nombre o UUID del modelo")}}, {"asset"}});
        d.push_back({"graphics_settings", "Lee o cambia la configuracion grafica: presupuesto adaptativo (adaptive, target_fps) y resolucion del mapa de sombras (0 = segun el hardware). Devuelve el estado del presupuesto.",
                     {{"adaptive", prop("boolean", "Optimizacion adaptativa")}, {"target_fps", prop("number", "FPS objetivo")},
                      {"upscaler", prop("string", "Escalador: off, taa, fsr1, fsr3 (AMD FSR 3.1, INESTABLE) o dlss (NVIDIA DLSS 4, INESTABLE)")},
                      {"resolution", prop("string", "Resolucion interna: native (DLAA/Native AA), quality, balanced, performance, ultra_performance")},
                      {"sharpness", prop("number", "Nitidez RCAS tras el escalado (0..1)")},
                      {"shadow_resolution", prop("number", "Resolucion por cascada (0 = auto)")},
                      {"texture_max_size", prop("number", "Lado maximo de las texturas (0 = auto)")},
                      {"path_tracing", prop("boolean", "Path tracing en la vista Escena (necesita trazado de rayos)")},
                      {"path_tracing_bounces", prop("number", "Rebotes del path tracing (1-16)")},
                      {"path_tracing_samples", prop("number", "Muestras maximas por pixel del path tracing")}}, {}});
        d.push_back({"performance_stats", "Rendimiento del ultimo frame: FPS, ms de CPU y GPU, tiempo de GPU por pase, actores, triangulos, lotes y llamadas de sombras.",
                     json::object(), {}});
        d.push_back({"import_file", "Importa un archivo del disco al proyecto (modelo .fbx/.obj/.gltf/.glb, cielo .hdr).",
                     {{"path", prop("string", "Ruta absoluta del archivo")}, {"folder", prop("string", "Subcarpeta de Assets (por defecto Models)")}}, {"path"}});
        d.push_back({"ground_height", "Altura del suelo (terreno o lo que haya) en un punto x, z del mundo: para colocar camaras u objetos encima.",
                     {{"x", prop("number", "X del mundo")}, {"z", prop("number", "Z del mundo")}}, {"x", "z"}});
        d.push_back({"fluid_create", "Crea liquidos de particulas (GameObject > Efectos > Liquidos): faucet (grifo de agua), block (bloque de agua que cae), honey (chorro de miel), lava (chorro de lava), world (Mundo de liquidos: dominio y ajustes), drain (desague) o tank (tanque de demostracion con un cubo que flota). Crea un FluidWorld si no hay (con 'Simular en el editor').",
                     {{"kind", prop("string", "faucet, block, honey, lava, world, drain o tank")}, {"position", vec3Prop("Donde (sin: delante de la camara)")},
                      {"fluid", prop("string", "Tipo del emisor: water, oil, honey, lava, mud, blood, acid, custom")},
                      {"emitter_fields", prop("object", "Campos del FluidEmitter {\"speed\": 3, \"nozzle_radius\": 0.1, \"shape\": \"Caja (se llena)\", ...}")},
                      {"world_fields", prop("object", "Campos del FluidWorld {\"size\": [6,5,6], \"particle_radius\": 0.04, \"solid_walls\": true, ...}")}}, {}});
        d.push_back({"fluid_spawn", "Crea una bola de liquido (particulas) en un punto. Sin FluidWorld usa el dominio por defecto (8x6x8 m).",
                     {{"position", vec3Prop("Centro (mundo)")}, {"count", prop("integer", "Particulas (1000)")},
                      {"fluid", prop("string", "water, oil, honey, lava, mud, blood, acid, custom")}, {"velocity", vec3Prop("Velocidad inicial m/s")},
                      {"radius", prop("number", "Radio de la bola (0 = el justo)")}, {"lifetime", prop("number", "Segundos de vida (0 = siempre)")}}, {"position"}});
        d.push_back({"fluid_state", "Estado de los liquidos: particulas (total y por tipo), capacidad, pasos, colliders, emisores, cuerpos que flotan, memoria. Con 'position': densidad, velocidad y altura de la superficie ahi.",
                     {{"position", vec3Prop("Punto a consultar (opcional)")}, {"radius", prop("number", "Radio de la consulta")}}, {}});
        d.push_back({"fluid_clear", "Borra todas las particulas de liquido (los emisores de caja/esfera vuelven a llenarse).", json::object(), {}});
        d.push_back({"fire_create", "Crea una zona de fuego (componente Fire: incendio que se propaga con llamas y humo volumetricos, suelo quemado y luces). Sin position: delante de la camara sobre el suelo. Se simula en el editor; 'fields' ajusta el componente (size, cell_size, spread, limit_to_zone, spread_speed, burn_time, flame_height, smoke_amount, smoke_height, wind_speed, wind_direction, sky_wind...).",
                     {{"position", vec3Prop("Centro de la zona (y = suelo)")}, {"on_ground", prop("boolean", "Posarla en el terreno (true)")},
                      {"size", prop("number", "Lado de la zona en m (120)")}, {"ignite", prop("boolean", "Encender ya en el centro (true)")},
                      {"fields", prop("object", "Campos del componente Fire {\"spread_speed\": 1.0, ...}")}}, {}});
        d.push_back({"fire_ignite", "Enciende fuego en un circulo (mundo) dentro de las zonas de fuego que lo tocan. En el editor activa 'Simular en el editor' de esas zonas.",
                     {{"position", vec3Prop("Centro (mundo)")}, {"radius", prop("number", "Radio en m (2)")}}, {"position"}});
        d.push_back({"fire_extinguish", "Apaga el fuego en un circulo (mundo) o todo con all=true. reset=true ademas borra lo quemado (vuelve a empezar).",
                     {{"position", vec3Prop("Centro (mundo)")}, {"radius", prop("number", "Radio en m (10)")},
                      {"all", prop("boolean", "Todas las zonas")}, {"reset", prop("boolean", "Reiniciar las zonas (nada quemado)")}}, {}});
        d.push_back({"fire_state", "Estado de los incendios: por zona celdas en llamas, con brasas, quemadas, area en llamas, fraccion quemada, viento y segundos simulados; con position, calor y quemado en ese punto.",
                     {{"position", vec3Prop("Punto a consultar (opcional)")}}, {}});
        d.push_back({"generate_house", "Crea una casa o cabana procedural realista (Ventana > Generador de casas): 8 materiales PBR compartidos con relieve, MeshCollider y la puerta aparte (hijo 'Puerta', gira en Y). Con 'village' crea una aldea de N casas en el terreno (reemplaza 'Aldea').",
                     {{"style", prop("string", "log (troncos), timber (tablas), stone (piedra) o farm (casa de campo de 2 plantas)")},
                      {"seed", prop("integer", "Semilla: otras medidas y ventanas")}, {"width", prop("number", "Ancho en m")},
                      {"depth", prop("number", "Fondo en m")}, {"floors", prop("integer", "1 o 2")}, {"wall_height", prop("number", "Altura por planta")},
                      {"roof_pitch", prop("number", "Grados")}, {"roof_overhang", prop("number", "Alero en m")}, {"windows", prop("integer", "Ventanas delante (-1 auto)")},
                      {"porch", prop("boolean", "Porche")}, {"chimney", prop("boolean", "Chimenea")}, {"shutters", prop("boolean", "Contraventanas")},
                      {"position", vec3Prop("Donde (la puerta mira a +Z)")}, {"on_ground", prop("boolean", "Posarla en el terreno (true)")},
                      {"yaw", prop("number", "Giro en grados")}, {"village", prop("integer", "Aldea de N casas en el terreno")}}, {}});
        d.push_back({"generate_terrain", "Genera un mundo completo (como Ventana > Generador de terreno): relieve con erosion, rios, lagos, oceano, capas con texturas y arboles. Reemplaza el grupo 'Mundo generado'. Tarda unos segundos.",
                     {{"shape", prop("string", "island, archipelago, continent, mountains o canyons")}, {"seed", prop("integer", "Semilla")},
                      {"size", prop("number", "Metros por lado (2048)")}, {"height", prop("number", "Altura maxima en metros (420)")}, {"houses", prop("integer", "Casas de la aldea (8; 0 = sin aldea)")},
                      {"resolution", prop("integer", "Vertices por lado: 257, 513, 1025, 2049")}, {"sea_level", prop("number", "0..1 de la altura bajo el mar")},
                      {"mountains", prop("number", "0..1")}, {"ridges", prop("number", "0..1")}, {"hills", prop("number", "0..1")},
                      {"warp", prop("number", "0..1")}, {"plateaus", prop("number", "0..1")}, {"feature_scale", prop("number", "Escala de las formas (1)")},
                      {"erosion", prop("number", "0..2 lluvia")}, {"erosion_strength", prop("number", "0..1")}, {"thermal", prop("number", "0..1")},
                      {"ocean", prop("boolean", "Mar alrededor")}, {"rivers", prop("integer", "Rios principales")}, {"lakes", prop("boolean", "Lagos")},
                      {"beach_width", prop("number", "Metros de playa")}, {"snow_line", prop("number", "0..1 altura de la nieve")},
                      {"textures", prop("boolean", "Texturas procedurales de las capas (true)")},
                      {"trees", prop("boolean", "Arboles (true)")}, {"tree_density", prop("number", "Arboles por hectarea (120)")},
                      {"grass", prop("boolean", "Hierba en la GPU (true)")}, {"grass_density", prop("number", "Briznas por m2 (40)")}}, {}});
        d.push_back({"create_model", "Crea un modelo 3D a partir de texto OBJ (y opcionalmente MTL) y lo importa como asset.",
                     {{"name", prop("string", "Nombre del modelo")}, {"obj", prop("string", "Contenido del .obj (v, vt, vn, f...)")}, {"mtl", prop("string", "Contenido del .mtl (opcional)")}}, {"name", "obj"}});
        d.push_back({"instantiate", "Pone en la escena un modelo, prefab o cielo del proyecto.",
                     {{"asset", prop("string", "Ruta, nombre o UUID del asset")}, {"position", vec3Prop("Posicion")}, {"parent", entity}, {"name", prop("string", "Nombre de la instancia")}}, {"asset"}});
        d.push_back({"create_prefab", "Guarda una entidad (con sus hijos) como prefab (.crprefab).", {{"entity", entity}, {"folder", prop("string", "Subcarpeta de Assets (por defecto Prefabs)")}}, {"entity"}});
        d.push_back({"list_scenes", "Escenas del proyecto.", json::object(), {}});
        d.push_back({"new_scene", "Escena nueva (camara, luz y cielo).", {{"force", prop("boolean", "Descartar cambios sin guardar")}}, {}});
        d.push_back({"open_scene", "Abre una escena.", {{"scene", prop("string", "Ruta o nombre de la escena")}, {"force", prop("boolean", "Descartar cambios sin guardar")}}, {"scene"}});
        d.push_back({"export_datapack", "Exporta escenas con TODO lo que usan (modelos, materiales, texturas, prefabs, scripts, sonidos) a un .datapack, sin ejecutable (como un AssetBundle de Unity). El juego lo carga con DataPack.load / DataPack.loadScene.",
                     {{"file", prop("string", "Archivo .datapack de destino (ruta absoluta)")},
                      {"scenes", prop("array", "Escenas y/o prefabs (nombre, ruta o UUID); por defecto la escena abierta")},
                      {"entity", prop("string", "Objeto de la escena a empaquetar (se usa/crea su prefab en Assets/Prefabs)")},
                      {"name", prop("string", "Nombre del paquete (por defecto el de la primera escena)")},
                      {"async", prop("boolean", "true: exporta en segundo plano con la ventana de progreso del editor (el resultado sale en la Consola)")}}, {"file"}});
        d.push_back({"save_scene", "Guarda la escena (en su archivo o en el que se indique).", {{"path", prop("string", "Ruta dentro de Assets (opcional, p. ej. Scenes/Nivel1.crscene)")}}, {}});
        d.push_back({"play", "Entra en modo Play (el juego corre en el editor).", json::object(), {}});
        d.push_back({"export_game", "Exporta el juego con la configuracion de compilacion activa (en segundo plano: ver export_status). "
                                    "Con platform android sale un APK/AAB; install lo instala y abre en el dispositivo por adb.",
                     {{"folder", prop("string", "Carpeta de destino (fuera del proyecto)")},
                      {"platform", prop("string", "windows o android (cambia la de la configuracion activa)")},
                      {"install", prop("boolean", "Android: instalar y abrir en el dispositivo; Windows: ejecutar al terminar")},
                      {"device", prop("string", "Android: serie del dispositivo de adb (por defecto el primero)")}},
                     {"folder"}});
        d.push_back({"export_status", "Progreso de la exportacion en curso o el resultado de la ultima.", json::object(), {}});
        d.push_back({"stop", "Sale del modo Play (la escena vuelve a como estaba).", json::object(), {}});
        d.push_back({"pause", "Pausa o reanuda el modo Play.", json::object(), {}});
        d.push_back({"run_lua", "Ejecuta codigo Lua en el motor (en Play, dentro del juego). Devuelve lo que retorne el codigo.",
                     {{"code", prop("string", "Codigo Lua; usa return para obtener un valor")}}, {"code"}});
        d.push_back({"get_console", "Ultimas lineas de la consola del editor (logs, avisos, errores de scripts y shaders).",
                     {{"lines", prop("integer", "Cuantas (por defecto 60)")}, {"level", prop("string", "all, warnings o errors")}}, {}});
        d.push_back({"screenshot", "Captura del editor (imagen PNG) para ver el resultado. Con 'path' la guarda en ese archivo y no devuelve la imagen (para grabar frames).",
                     {{"path", prop("string", "Opcional: ruta absoluta del PNG")}}, {}});
        d.push_back({"cinema", "Modo cine para grabar trailers: la ventana pasa a ser SOLO la imagen de la escena (sin paneles ni gizmos) del tamano pedido, calidad maxima (sin presupuesto adaptativo) y el tiempo del motor (viento, agua, nubes, fisica) se para: solo avanza 1/fps por cada paso de cinema_step.",
                     {{"enabled", prop("boolean", "Activar o salir")}, {"width", prop("integer", "Ancho (1920)")}, {"height", prop("integer", "Alto (1080)")}, {"fps", prop("number", "Frames por segundo del video (30)")}}, {"enabled"}});
        d.push_back({"cinema_step", "En modo cine: coloca la camara (opcional) y avanza el tiempo 'frames' pasos. Devuelve el contador de frames; espera con cinema_state a que pase de ese valor + 3 antes de capturar.",
                     {{"position", vec3Prop("Posicion de la camara")}, {"target", vec3Prop("Punto al que mira")}, {"frames", prop("integer", "Pasos de tiempo (1)")}}, {}});
        d.push_back({"cinema_state", "Estado del modo cine: frames dibujados, pasos pendientes y tamano de la ventana.", json::object(), {}});
        // Maquinas de estados de IA (.crfsm, EditorStateMachine.cpp).
        {
            const json machine = prop("string", "La maquina: ruta dentro de Assets (IA/Enemigo.crfsm), nombre o UUID");
            d.push_back({"create_state_machine", "Crea (o reemplaza, conservando su UUID) una maquina de estados de IA (.crfsm) como los State Graphs de Bolt. 'machine' es el JSON del .crfsm: {variables:[{name,type(bool|int|float|string|entity|vec3),value}], states:[{name, code (Lua con OnEnter(self, sm), OnUpdate(self, dt), OnExit(self)...), position:[x,y], color:[r,g,b], script (opcional: .lua de Assets)}], entry:'Estado', any_code:'Lua de Cualquier estado (sensores, corre siempre)', transitions:[{from:'Estado'|'any', to:'Estado', priority, conditions:[{type:'variable', variable, compare(==,!=,>,<,>=,<=,true,false), value (o '$otraVariable')}, {type:'trigger', trigger}, {type:'timer', seconds}, {type:'lua', expression}]}]}. En el codigo: self.entity, self.vars.x, self.sm:go('Estado'), self.sm:trigger('t'), self.sm.stateTime. Sin 'machine' crea una vacia; example=true crea el enemigo de ejemplo (Patrullar/Perseguir/Atacar/Huir/Volver con NavAgent).",
                         {{"name", prop("string", "Nombre del archivo (sin extension)")}, {"folder", prop("string", "Carpeta dentro de Assets (por defecto IA)")},
                          {"machine", prop("object", "JSON del .crfsm (ver descripcion)")}, {"example", prop("boolean", "Crear el enemigo de ejemplo")},
                          {"attach_to", entity}, {"open", prop("boolean", "Abrirla en la ventana Maquina de estados")}}, {"name"}});
            d.push_back({"get_state_machine", "Lee una maquina de estados: su JSON completo (variables, estados con su codigo, transiciones) y los problemas que tenga.",
                         {{"machine", machine}}, {"machine"}});
            d.push_back({"update_state_machine", "Cambia una maquina de estados: 'data' la reemplaza entera (mismo formato que create_state_machine) y/o 'state_code' cambia el codigo de algunos estados {\"Patrullar\": \"function OnEnter(self, sm) ... end\"}, 'any_code' el de Cualquier estado y 'entry' el estado de entrada. En Play se recarga en caliente.",
                         {{"machine", machine}, {"data", prop("object", "JSON completo del .crfsm")}, {"state_code", prop("object", "{estado: codigo Lua}")},
                          {"any_code", prop("string", "Codigo de Cualquier estado")}, {"entry", prop("string", "Estado de entrada")}}, {"machine"}});
            d.push_back({"assign_state_machine", "Pone el componente StateMachine con esa maquina en una entidad. 'variables' cambia los valores iniciales de la pizarra solo para ese objeto.",
                         {{"entity", entity}, {"machine", machine}, {"variables", prop("object", "{variable: valor}")},
                          {"start_active", prop("boolean", "Empieza sola (por defecto si)")}, {"debug", prop("boolean", "Cada cambio de estado a la consola")}}, {"entity", "machine"}});
            d.push_back({"state_machine_debug", "Estado en vivo (en Play) de la maquina de una entidad: estado actual, anterior, tiempo, variables e historial de cambios. Opcional: go (ir a un estado), trigger (disparar uno) y set ({variable: valor}).",
                         {{"entity", entity}, {"go", prop("string", "Ir a este estado")}, {"trigger", prop("string", "Disparar este trigger")}, {"set", prop("object", "{variable: valor}")}}, {"entity"}});
        }
        // --- 2.1: pruebas, iluminacion horneada, VFX, Visual Script, Shader Graph, Behavior Trees (EditorMcp21.cpp) ---
        d.push_back({"run_tests", "Ejecuta las pruebas automaticas del proyecto (*.test.lua y Assets/Tests): entra en Play, corre las de edicion y las de Play y vuelve. Mira el resultado con test_results.",
                     {{"mode", prop("string", "all (por defecto), edit o play")}}, {}});
        d.push_back({"test_results", "Resultado de la ultima ejecucion de pruebas: running y cada prueba (file, name, mode, status passed/failed/skipped, message, seconds, assertions).", json::object(), {}});
        d.push_back({"bake_lighting", "Hornea la luz rebotada de la escena abierta en volumenes de sondas (Light Probe Volume; si no hay, uno automatico) y la activa. Corre en segundo plano: mira lighting_state.",
                     {{"rays", prop("integer", "Rayos por sonda (256)")}, {"bounces", prop("integer", "Rebotes extra (2)")}, {"spacing", prop("number", "Separacion del volumen automatico en metros (2)")}}, {}});
        d.push_back({"lighting_state", "Estado de la iluminacion horneada: baking, progress, mode (baked/realtime), volumes, probes.", json::object(), {}});
        d.push_back({"create_vfx", "Crea un efecto del VFX Graph (.crvfx) desde una plantilla (Chispas, Fuego, Humo, Chispas que rebotan, Explosion, Magia, Luciernagas, Nieve, Lluvia, Estela; o su indice 0..9) o desde 'graph' (el JSON del .crvfx: capacity, world_space, duration, loop, params, spawn/initialize/update/output_blocks [{type, values{campo: valor}, bind{campo: parametro}}], output{orient, blend, intensity, texture...}). attach_to: le pone el componente Visual Effect.",
                     {{"name", prop("string", "Nombre del archivo")}, {"folder", prop("string", "Carpeta dentro de Assets (Efectos)")}, {"preset", prop("string", "Plantilla")},
                      {"graph", prop("object", "JSON del .crvfx")}, {"attach_to", entity}}, {}});
        d.push_back({"vfx_control", "Controla el Visual Effect de una entidad (en Play o con vista previa): action play, stop (clear), event (event) o set (param, value: numero, bool, [x,y,z] o [r,g,b,a]).",
                     {{"entity", entity}, {"action", prop("string", "play, stop, event o set")}, {"event", prop("string", "Evento del bloque Spawn")},
                      {"param", prop("string", "Parametro expuesto")}, {"value", prop("string", "Valor")}, {"clear", prop("boolean", "Borrar las particulas al parar")}}, {"entity"}});
        d.push_back({"create_visual_script", "Crea un Visual Script (.crgraph, Blueprints). 'graph': {variables:[{name,type,value,exposed}], nodes:[{id, kind ('event.start', 'event.update', 'flow.branch', 'var.get', 'call'...), fn ('Debug.log', 'Entity:translate'...), values:{pin: valor}, position:[x,y]}], links:[{from, out (nombre o indice), to, in}]}. Devuelve si compila y los errores por nodo. attach_to: le pone el componente.",
                     {{"name", prop("string", "Nombre del archivo")}, {"folder", prop("string", "Carpeta (Scripts)")}, {"graph", prop("object", "JSON del grafo")}, {"attach_to", entity}}, {}});
        d.push_back({"create_shader_graph", "Crea un Shader Graph (.crshadergraph) y genera su .crshader (los materiales lo eligen como shader). 'graph' es el JSON del grafo (nodes [{id, type, x, y, inputs, name, value...}], links [{from_node, from_pin, to_node, to_pin}]); sin el, el de ejemplo. Devuelve el codigo generado y los errores.",
                     {{"name", prop("string", "Nombre del archivo")}, {"folder", prop("string", "Carpeta (Shaders)")}, {"graph", prop("object", "JSON del grafo")}}, {}});
        d.push_back({"create_behavior_tree", "Crea un Behavior Tree (.crbt): example=true el guardia de ejemplo; 'tree' el JSON (blackboard [{name,type,value}], nodes [{kind, name, children, decorators, services, params, position}]). attach_to: le pone el componente.",
                     {{"name", prop("string", "Nombre del archivo")}, {"folder", prop("string", "Carpeta (IA)")}, {"example", prop("boolean", "El guardia de ejemplo")},
                      {"tree", prop("object", "JSON del arbol")}, {"attach_to", entity}}, {}});
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
Time, Physics.raycast, Audio, UI, Prefs, Game, Debug.log, Navigation, Mesh, Voxel, Graphics (calidad, resolucion, sombras, texturas, ventana y Graphics.post para el post-procesado).
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
BoxCollider, SphereCollider, CapsuleCollider, MeshCollider, CharacterController, Script (file = "Scripts/X.lua"),
AudioSource, Animator, ParticleSystem, NavAgent, Terrain, WaterBody, Decal.
Nota: primitivas (cube, sphere...) ya traen su collider.

## Ambiente (clima y hora)
set_weather (Storm, Snow, Foggy...; seconds = transicion), set_time (hours, day, month, latitude,
day_length, season), set_wind, lightning_strike, get_environment. En Lua: Weather.set("Rain", 5).
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
            if (type.name == "EditableMesh" && components[type.name].is_object()) {
                const modeling::PolyMesh& m = e.get<modeling::EditableMesh>().mesh;
                components[type.name]["data"] = "(" + std::to_string(m.positions.size()) + " vertices, " +
                                                std::to_string(m.faces.size()) + " caras: usa modeling_info)";
            }
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
        if (ext == ai::kStateMachineExtension) {  // maquina de estados: Play y la ventana abierta
            a.fsm_cache_.clear();
            a.scripts_.reloadFile(rel);
        }
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
        if (args.value("show_loading", false)) {
            // Como el Hub: por etapas con el dialogo de carga (vuelve enseguida).
            a.beginOpenProject(dialogs::fromUtf8(arg(args, "path")));
            return json{{"loading", true}};
        }
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

    // Tambien en el Hub (sin proyecto).
    if (name == "screenshot") {
        const bool to_file = args.contains("path") && args["path"].is_string() && !arg(args, "path").empty();
        const std::filesystem::path png = to_file ? dialogs::fromUtf8(arg(args, "path"))
                                                  : std::filesystem::temp_directory_path() / "cramion_mcp_screenshot.png";
        const std::vector<std::uint8_t> pixels = captureEditorWindow(a.window_.handle(), png);
        if (pixels.empty()) throw ToolError("no se pudo capturar la ventana (minimizada?)");
        if (to_file) return json{{"path", arg(args, "path")}};
        image = true;
        image_data = base64(readBytes(png));
        return json{};
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
            {"directional_light", 6}, {"point_light", 7}, {"spot_light", 8}, {"camera", 9}, {"decal", 10}, {"vehicle", 17},
            {"character", 23}};
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
    if (name == "cvar_list") {
        const std::string filter = arg(args, "filter");
        json list = json::array();
        for (const cvar::CVarBase* c : cvar::Registry::instance().all()) {
            if (!filter.empty() && c->name().find(filter) == std::string::npos && c->description().find(filter) == std::string::npos) continue;
            json j{{"name", c->name()}, {"value", c->toString()}, {"default", c->defaultString()}, {"type", cvar::typeName(c->type())},
                   {"description", c->description()}, {"saved", (c->flags() & cvar::Saved) != 0}, {"read_only", (c->flags() & cvar::ReadOnly) != 0}};
            if (c->hasRange()) j["range"] = json::array({c->rangeMin(), c->rangeMax()});
            list.push_back(j);
        }
        return json{{"cvars", list}};
    }
    if (name == "cvar_set") {
        cvar::CVarBase* c = cvar::Registry::instance().find(arg(args, "name"));
        if (c == nullptr) throw ToolError("no existe la CVar '" + arg(args, "name") + "' (usa cvar_list)");
        if (args.value("reset", false)) {
            c->reset();
        } else {
            std::string error;
            const std::string value = args.contains("value") && !args["value"].is_string() ? args["value"].dump() : arg(args, "value");
            if (!cvar::Registry::instance().set(c->name(), value, &error)) throw ToolError(error);
        }
        return json{{"name", c->name()}, {"value", c->toString()}};
    }
    if (name == "cpp_compile") {
        needProject();
        if (args.value("ask_name", false)) {  // la ventana "Nuevo script C++" del editor
            a.createCppScriptAsset({}, {});
            return json{{"asking", true}};
        }
        if (args.contains("create") && !arg(args, "create").empty()) {
            ecs::Entity target = args.contains("entity") && !arg(args, "entity").empty() ? entity(arg(args, "entity")) : ecs::Entity{};
            const std::string cls = arg(args, "create");
            const std::filesystem::path file = a.project_.assetsFolder() / "Scripts" / dialogs::fromUtf8(cls + ".cpp");
            if (std::filesystem::exists(file)) throw ToolError("ya existe " + cls + ".cpp");
            std::filesystem::create_directories(file.parent_path());
            std::ofstream(file.parent_path() / dialogs::fromUtf8(cls + ".h"), std::ios::binary) << scripting::cppScriptHeaderTemplate(cls);
            std::ofstream(file, std::ios::binary) << scripting::cppScriptTemplate(cls);
            a.refreshDatabase();
            if (target.valid()) {
                a.attachScriptFile(target, a.assetRelative(file));
                a.commit();
            }
        }
        while (a.cpp_scripts_.compiling()) Sleep(20);
        a.cpp_scripts_.takeCompileResult();
        const scripting::CppCompileResult r = a.cpp_scripts_.compile();
        a.cpp_compile_errors_ = r.errors;
        a.cpp_attempted_ = a.cpp_scripts_.newestSourceTime();
        a.cpp_status_ = r.nothing_to_compile ? std::string()
                                             : (r.ok ? "Compilado (MCP)" : "Errores de compilacion: " + std::to_string(r.errors.size()));
        for (const scripting::ScriptError& e : r.errors) {
            std::cerr << "[C++] " << (e.file.empty() ? "" : e.file + ":" + std::to_string(e.line) + ": ") << e.message << std::endl;
        }
        if (r.ok && !r.dll.empty()) {
            a.cpp_scripts_.usePrebuilt(r.dll);
            a.cpp_scripts_.setBuildFolder(a.project_.libraryFolder() / "CppScripts");
            if (a.playing()) a.cpp_scripts_.reload();
        }
        json errors = json::array();
        for (const scripting::ScriptError& e : r.errors) errors.push_back(json{{"file", e.file}, {"line", e.line}, {"message", e.message}});
        json runtime = json::array();
        for (const scripting::ScriptError& e : a.cpp_scripts_.errors()) runtime.push_back(json{{"file", e.file}, {"line", e.line}, {"message", e.message}});
        return json{{"ok", r.ok}, {"nothing_to_compile", r.nothing_to_compile}, {"compiler", r.compiler}, {"seconds", r.seconds},
                    {"errors", errors}, {"runtime_errors", runtime}, {"classes", a.cpp_scripts_.classes()},
                    {"log", r.ok ? std::string() : r.log.substr(0, 4000)}};
    }
    if (name == "cpp_intellisense") {
        needProject();
        const std::filesystem::path file = a.project_.assetsFolder() / dialogs::fromUtf8(arg(args, "file"));
        if (!std::filesystem::exists(file)) throw ToolError("no existe " + arg(args, "file"));
        if (a.scriptTabFor(file) == nullptr) a.openScript(file);  // (abierto: no se vuelve a enfocar la ventana)
        EditorApp::ScriptTab* tab = a.scriptTabFor(file);
        if (tab == nullptr) throw ToolError("no se pudo abrir");
        if (args.contains("text") && args["text"].is_string()) tab->text = args["text"].get<std::string>();
        if (args.contains("cursor")) {  // -1 = al final; el campo queda activo para escribir
            const int c = args.value("cursor", -1);
            tab->set_cursor = c < 0 ? static_cast<int>(tab->text.size()) : std::min(c, static_cast<int>(tab->text.size()));
            tab->focus = true;
        }
        if (args.contains("type") && args["type"].is_string()) a.mcp_type_queue_ += args["type"].get<std::string>();
        if (args.value("format", false)) a.formatCppTab(*tab);
        if (args.contains("settings")) a.show_engine_settings_ = args.value("settings", false);
        a.ensureClangd();
        a.syncClangd(*tab);
        const double wait = args.value("wait", 20.0);
        const bool ask = args.contains("line");
        const int line = std::max(0, args.value("line", 1) - 1);
        const int column = std::max(0, args.value("column", 1) - 1);
        bool got_completion = !ask, got_hover = !ask;
        json completions = json::array();
        std::string hover;
        if (ask && a.clangd_.running()) {
            a.clangd_.completion(file, line, column, [&](std::vector<ClangdCompletion> items) {
                for (std::size_t i = 0; i < items.size() && i < 25; ++i) {
                    completions.push_back(json{{"label", items[i].label}, {"insert", items[i].insert}, {"detail", items[i].detail}, {"kind", items[i].kind}});
                }
                got_completion = true;
            });
            a.clangd_.hover(file, line, column, [&](std::string text) {
                hover = std::move(text);
                got_hover = true;
            });
        }
        const std::uint64_t start_version = a.clangd_.diagnosticsVersion();
        const auto t0 = std::chrono::steady_clock::now();
        while (a.clangd_.running() && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < wait) {
            a.clangd_.poll();
            if (got_completion && got_hover && a.clangd_.diagnosticsVersion() != start_version) break;
            Sleep(20);
        }
        json diagnostics = json::array();
        for (const ClangdDiagnostic& d : a.clangd_.diagnostics(file)) {
            diagnostics.push_back(json{{"line", d.line + 1}, {"column", d.column + 1}, {"severity", d.severity}, {"message", d.message}});
        }
        return json{{"running", a.clangd_.running()}, {"status", a.clangd_.status()},
                    {"clangd", dialogs::utf8(scripting::CppScriptSystem::findClangd())}, {"cpp_tab", tab->cpp},
                    {"diagnostics", diagnostics}, {"completions", completions}, {"hover", hover},
                    {"answered", json{{"completion", got_completion}, {"hover", got_hover}}},
                    {"tab", json{{"cursor", tab->cursor}, {"line", tab->line}, {"column", tab->column}, {"active", tab->was_active},
                                 {"completion_open", tab->completion_open}, {"completions", tab->completions.size()},
                                 {"first", tab->completions.empty() ? std::string() : tab->completions.front().label},
                                 {"signature_open", tab->signature_open}, {"text_size", tab->text.size()},
                                 {"text_tail", tab->text.substr(tab->text.size() > 400 ? tab->text.size() - 400 : 0)}}}};
    }
    if (name == "modeling_create") {
        needProject();
        modeling::shapes::Kind kind{};
        if (!modeling::shapes::kindFromKey(arg(args, "shape", "cube"), kind)) throw ToolError("forma desconocida: " + arg(args, "shape"));
        modeling::shapes::Params p = modeling::shapes::defaults(kind);
        p.size = readVec(args, "size", p.size);
        p.segments = args.value("segments", p.segments);
        p.rings = args.value("rings", p.rings);
        const Vec3 sub = readVec(args, "subdivisions", Vec3{static_cast<float>(p.subdivisions_x), static_cast<float>(p.subdivisions_y),
                                                          static_cast<float>(p.subdivisions_z)});
        p.subdivisions_x = std::max(1, static_cast<int>(sub.x));
        p.subdivisions_y = std::max(1, static_cast<int>(sub.y));
        p.subdivisions_z = std::max(1, static_cast<int>(sub.z));
        p.steps = args.value("steps", p.steps);
        p.thickness = args.value("thickness", p.thickness);
        p.inner_radius = args.value("inner_radius", p.inner_radius);
        p.angle = args.value("angle", p.angle);
        p.sides = args.value("solid", p.sides);
        p.smooth = args.value("smooth", p.smooth);
        if (args.contains("door") && args["door"].is_array() && args["door"].size() >= 2) {
            p.door = core::Vec2{args["door"][0].get<float>(), args["door"][1].get<float>()};
        }
        ecs::Entity e = a.createModelingShape(kind, p);
        if (args.contains("parent") && !arg(args, "parent").empty()) e.setParent(entity(arg(args, "parent")), true);
        if (args.contains("name")) e.setName(arg(args, "name"));
        if (args.contains("position")) e.setWorldPosition(readVec(args, "position", e.worldPosition()));
        if (args.contains("color")) e.get<modeling::EditableMesh>().slots[0].color = readVec(args, "color", Vec3{0.8f, 0.8f, 0.8f});
        e.get<modeling::EditableMesh>().markModified();
        a.commit();
        json j = summary(e);
        j["faces"] = e.get<modeling::EditableMesh>().mesh.faces.size();
        return j;
    }
    if (name == "modeling_convert") {
        needProject();
        ecs::Entity e = entity(arg(args, "entity"));
        if (!a.convertToEditableMesh(e, args.value("quad_angle", 2.0f)).valid()) throw ToolError("no se pudo convertir (sin Mesh Renderer o sin malla cargada)");
        json j = summary(e);
        j["faces"] = e.get<modeling::EditableMesh>().mesh.faces.size();
        return j;
    }
    if (name == "modeling_info") {
        ecs::Entity e = entity(arg(args, "entity"));
        const modeling::EditableMesh* em = e.tryGet<modeling::EditableMesh>();
        if (em == nullptr) throw ToolError("no es una malla editable (usa modeling_convert)");
        const modeling::PolyMesh& m = em->mesh;
        json faces = json::array();
        const int max_faces = args.value("max_faces", 300);
        for (std::size_t fi = 0; fi < m.faces.size() && static_cast<int>(fi) < max_faces; ++fi) {
            const modeling::Face& f = m.faces[fi];
            faces.push_back(json{{"index", fi}, {"vertices", f.v}, {"center", vec(m.faceCenter(static_cast<int>(fi)))},
                                 {"normal", vec(m.faceNormal(static_cast<int>(fi)))}, {"material", f.material}, {"smoothing", f.smoothing}});
        }
        Vec3 lo{}, hi{};
        m.bounds(lo, hi);
        json j = summary(e);
        j["vertex_count"] = m.positions.size();
        j["face_count"] = m.faces.size();
        j["edge_count"] = m.edges().size();
        j["border_edges"] = m.borderEdges().size();
        j["bounds_min"] = vec(lo);
        j["bounds_max"] = vec(hi);
        j["faces"] = faces;
        if (args.value("vertices", false)) {
            json verts = json::array();
            for (const Vec3& p : m.positions) verts.push_back(vec(p));
            j["positions"] = verts;
        }
        return j;
    }
    if (name == "modeling_edit") {
        needProject();
        ecs::Entity e = entity(arg(args, "entity"));
        modeling::EditableMesh* em = e.tryGet<modeling::EditableMesh>();
        if (em == nullptr) throw ToolError("no es una malla editable (usa modeling_convert)");
        modeling::PolyMesh& m = em->mesh;
        namespace mops = modeling::ops;
        const std::string op = arg(args, "operation");
        // Caras: indices o un selector por la normal.
        const auto faces = [&]() {
            std::vector<int> out;
            const auto it = args.find("faces");
            std::string selector = "all";
            if (it != args.end() && it->is_array()) {
                for (const json& v : *it) {
                    if (v.is_number_integer() && v.get<int>() >= 0 && static_cast<std::size_t>(v.get<int>()) < m.faces.size()) out.push_back(v.get<int>());
                }
                return out;
            }
            if (it != args.end() && it->is_string()) selector = it->get<std::string>();
            Vec3 dir{};
            if (selector == "up") dir = Vec3{0, 1, 0};
            else if (selector == "down") dir = Vec3{0, -1, 0};
            else if (selector == "front") dir = Vec3{0, 0, 1};
            else if (selector == "back") dir = Vec3{0, 0, -1};
            else if (selector == "right") dir = Vec3{1, 0, 0};
            else if (selector == "left") dir = Vec3{-1, 0, 0};
            else if (selector != "all") throw ToolError("selector de caras desconocido: " + selector);
            for (std::size_t fi = 0; fi < m.faces.size(); ++fi) {
                if (selector == "all" || core::dot(m.faceNormal(static_cast<int>(fi)), dir) > 0.9f) out.push_back(static_cast<int>(fi));
            }
            return out;
        };
        const auto edges = [&]() {
            std::vector<modeling::Edge> out;
            const auto it = args.find("edges");
            if (it != args.end() && it->is_string() && it->get<std::string>() == "border") return m.borderEdges();
            if (it != args.end() && it->is_array()) {
                for (const json& pair : *it) {
                    if (pair.is_array() && pair.size() == 2) out.push_back(modeling::Edge(pair[0].get<std::uint32_t>(), pair[1].get<std::uint32_t>()));
                }
            }
            if (out.empty()) throw ToolError("faltan las aristas ([[a,b],...] o 'border'; mira modeling_info)");
            return out;
        };
        const auto vertices = [&]() {
            std::vector<std::uint32_t> out;
            const auto it = args.find("vertices");
            if (it != args.end() && it->is_array()) {
                for (const json& v : *it) out.push_back(v.get<std::uint32_t>());
                return out;
            }
            if (args.contains("faces")) return mops::verticesOfFaces(m, faces());
            if (args.contains("edges")) return mops::verticesOfEdges(edges());
            out.resize(m.positions.size());
            for (std::size_t i = 0; i < out.size(); ++i) out[i] = static_cast<std::uint32_t>(i);
            return out;
        };
        const float amount = args.value("amount", 0.25f);
        json result;
        if (op == "extrude") {
            result["faces"] = mops::extrudeFaces(m, faces(), amount, args.value("individual", false) ? mops::ExtrudeMode::Individual : mops::ExtrudeMode::Group);
        } else if (op == "inset") {
            result["faces"] = mops::insetFaces(m, faces(), amount, args.value("individual", false));
        } else if (op == "bevel") {
            result["faces"] = mops::bevelEdges(m, edges(), amount);
        } else if (op == "subdivide") {
            result["faces"] = mops::subdivideFaces(m, faces());
        } else if (op == "connect") {
            result["edges"] = mops::connectEdges(m, edges()).size();
        } else if (op == "insert_loop") {
            result["edges"] = mops::insertEdgeLoop(m, edges().front(), args.value("t", 0.5f)).size();
        } else if (op == "subdivide_edges") {
            result["vertices"] = mops::subdivideEdges(m, edges(), args.value("cuts", 1));
        } else if (op == "bridge") {
            const std::vector<modeling::Edge> two = edges();
            if (two.size() != 2) throw ToolError("bridge necesita dos aristas de borde");
            result["face"] = mops::bridgeEdges(m, two[0], two[1]);
        } else if (op == "fill_holes") {
            result["faces"] = mops::fillHoles(m, args.contains("edges") ? edges() : std::vector<modeling::Edge>{});
        } else if (op == "merge") {
            result["face"] = mops::mergeFaces(m, faces());
        } else if (op == "delete") {
            mops::deleteFaces(m, faces());
        } else if (op == "flip") {
            mops::flipFaces(m, faces());
        } else if (op == "triangulate") {
            result["faces"] = mops::triangulateFaces(m, faces()).size();
        } else if (op == "duplicate") {
            result["faces"] = mops::duplicateFaces(m, faces());
        } else if (op == "detach") {
            modeling::PolyMesh piece = mops::detachFaces(m, faces(), true);
            ecs::Entity copy = modeling::createEditableEntity(a.world_, std::move(piece), e.name() + " (parte)", e.parent());
            copy.setWorldMatrix(e.worldMatrix());
            result["new_entity"] = summary(copy);
        } else if (op == "weld") {
            result["removed"] = mops::weldVertices(m, args.contains("vertices") || args.contains("faces") ? vertices() : std::vector<std::uint32_t>{}, args.value("amount", 0.01f));
        } else if (op == "collapse") {
            result["vertex"] = mops::collapseVertices(m, vertices());
        } else if (op == "split") {
            mops::splitVertices(m, vertices());
        } else if (op == "move" || op == "rotate" || op == "scale") {
            const std::vector<std::uint32_t> verts = vertices();
            const Vec3 v = readVec(args, "vector", op == "scale" ? Vec3{1, 1, 1} : Vec3{});
            Vec3 pivot{};
            for (const std::uint32_t i : verts) pivot += m.positions[i];
            if (!verts.empty()) pivot = pivot * (1.0f / static_cast<float>(verts.size()));
            core::Mat4 matrix = core::translate(v);
            if (op == "rotate") matrix = core::translate(pivot) * core::composeTrs(Vec3{}, ecs::quatFromEulerDegrees(v), Vec3{1, 1, 1}) * core::translate(-pivot);
            if (op == "scale") matrix = core::translate(pivot) * core::scale(v) * core::translate(-pivot);
            mops::transformVertices(m, verts, matrix);
        } else if (op == "relax") {
            mops::relax(m, args.contains("vertices") || args.contains("faces") ? vertices() : std::vector<std::uint32_t>{}, args.value("amount", 0.5f), args.value("cuts", 3));
        } else if (op == "noise") {
            mops::randomize(m, args.contains("vertices") || args.contains("faces") ? vertices() : std::vector<std::uint32_t>{}, amount, 7);
        } else if (op == "smooth") {
            mops::subdivideSmooth(m, args.value("cuts", 1));
            for (modeling::Face& f : m.faces) f.smoothing = std::max(f.smoothing, 1);
        } else if (op == "mirror") {
            const std::string axis = arg(args, "axis", "x");
            mops::mirror(m, axis == "y" ? 1 : (axis == "z" ? 2 : 0), args.value("keep", true));
        } else if (op == "conform") {
            mops::conformNormals(m);
        } else if (op == "center_pivot") {
            const Vec3 offset = mops::centerPivot(m, args.value("bottom", false));
            e.setWorldPosition(ecs::transformPoint(e.worldMatrix(), offset));
        } else if (op == "material") {
            const int slot = std::clamp(args.value("material", 0), 0, 63);
            while (static_cast<int>(em->slots.size()) <= slot) em->slots.push_back(modeling::SlotMaterial{});
            if (args.contains("color")) em->slots[static_cast<std::size_t>(slot)].color = readVec(args, "color", Vec3{0.8f, 0.8f, 0.8f});
            for (const int f : faces()) m.faces[static_cast<std::size_t>(f)].material = slot;
        } else if (op == "smoothing") {
            for (const int f : faces()) m.faces[static_cast<std::size_t>(f)].smoothing = std::max(args.value("group", 1), 0);
        } else if (op == "uv") {
            const json uv = args.value("uv", json::object());
            for (const int f : faces()) {
                modeling::FaceUv& u = m.faces[static_cast<std::size_t>(f)].uvs;
                const std::string mode = uv.value("mode", std::string(u.mode == modeling::UvMode::Planar ? "planar" : "box"));
                u.mode = mode == "planar" ? modeling::UvMode::Planar : modeling::UvMode::Box;
                const std::string fill = uv.value("fill", std::string("tile"));
                u.fill = fill == "fit" ? modeling::UvFill::Fit : (fill == "stretch" ? modeling::UvFill::Stretch : modeling::UvFill::Tile);
                if (uv.contains("scale") && uv["scale"].is_array() && uv["scale"].size() >= 2) u.scale = core::Vec2{uv["scale"][0].get<float>(), uv["scale"][1].get<float>()};
                if (uv.contains("offset") && uv["offset"].is_array() && uv["offset"].size() >= 2) u.offset = core::Vec2{uv["offset"][0].get<float>(), uv["offset"][1].get<float>()};
                u.rotation = uv.value("rotation", u.rotation);
                u.world_space = uv.value("world_space", u.world_space);
                m.faces[static_cast<std::size_t>(f)].uv.clear();
            }
        } else if (op == "boolean" || op == "combine") {
            ecs::Entity other = entity(arg(args, "other"));
            const modeling::EditableMesh* om = other.tryGet<modeling::EditableMesh>();
            if (om == nullptr) throw ToolError("'other' no es una malla editable");
            if (op == "combine") {
                mops::append(m, om->mesh, core::inverse(e.worldMatrix()) * other.worldMatrix());
                a.world_.destroy(other);
            } else {
                const std::string kind = arg(args, "op", "subtract");
                const mops::BoolOp bop = kind == "union" ? mops::BoolOp::Union : (kind == "intersect" ? mops::BoolOp::Intersect : mops::BoolOp::Subtract);
                m = mops::boolean(m, e.worldMatrix(), om->mesh, other.worldMatrix(), bop);
                other.setActive(false);
            }
        } else {
            throw ToolError("operacion desconocida: " + op);
        }
        const std::string problem = m.validate();
        if (!problem.empty()) throw ToolError("la malla quedo invalida: " + problem);
        em->markModified();
        a.modelingClearSelection();
        modeling::updateEditableMeshes(a.world_);
        a.dirty_ = true;
        a.commit();
        result["vertex_count"] = m.positions.size();
        result["face_count"] = m.faces.size();
        result["border_edges"] = m.borderEdges().size();
        return result;
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
    if (name == "set_gizmo") {
        const std::string mode = args.value("mode", std::string("move"));
        if (mode == "none") a.gizmo_ = EditorApp::GizmoOperation::None;
        else if (mode == "move") a.gizmo_ = EditorApp::GizmoOperation::Translate;
        else if (mode == "rotate") a.gizmo_ = EditorApp::GizmoOperation::Rotate;
        else if (mode == "scale") a.gizmo_ = EditorApp::GizmoOperation::Scale;
        else throw ToolError("mode debe ser none, move, rotate o scale");
        if (args.contains("local")) a.gizmo_local_ = args.value("local", false);
        // Iconos, contornos y volumenes de la vista de escena (capturas limpias).
        if (args.contains("show_gizmos")) a.show_gizmos_ = args.value("show_gizmos", true);
        return json{{"ok", true}, {"show_gizmos", a.show_gizmos_}};
    }
    if (name == "paint_prefabs") {
        if (arg(args, "group").empty() && !args.contains("prefabs")) throw ToolError("pasa group o prefabs");
        if (const std::string group = arg(args, "group"); !group.empty()) {
            if (!a.loadPaintGroup(assetPath(group))) throw ToolError("no se pudo leer el grupo " + group);
        } else {
            EditorApp::PaintGroup temp;
            temp.name = args.value("name", std::string("MCP"));
            for (const json& p : args["prefabs"]) {
                const auto info = findAsset(p.is_string() ? p.get<std::string>() : std::string{});
                if (!info || info->type != assets::AssetType::Prefab) throw ToolError("no es un prefab: " + p.dump());
                EditorApp::PaintItem item;
                item.prefab = info->uuid;
                item.align = args.value("align", item.align);
                item.scale_min = args.value("scale_min", item.scale_min);
                item.scale_max = args.value("scale_max", item.scale_max);
                temp.items.push_back(item);
            }
            a.paint_group_ = std::move(temp);
            a.paint_selected_ = 0;
        }
        EditorApp::PaintBrush& b = a.paint_brush_;
        b.radius = args.value("radius", b.radius);
        b.density = args.value("density", b.density);
        b.spacing = args.value("spacing", b.spacing);
        b.max_slope = args.value("max_slope", b.max_slope);
        b.only_selected = false;
        const Vec3 center = readVec(args, "center", {});
        Vec3 point{};
        Vec3 normal{};
        if (!a.paintRaycast(center + Vec3{0.0f, 200.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f}, 400.0f, point, normal)) {
            throw ToolError("no hay suelo con collider (o terreno) debajo del centro");
        }
        const int count = a.paintStamp(point, normal, args.value("erase", false));
        a.commit();
        return json{{args.value("erase", false) ? "removed" : "added", count},
                    {"total", static_cast<int>(a.paintedInstances(false).size())},
                    {"ground", {point.x, point.y, point.z}}};
    }
    if (name == "set_camera") {
        a.scene_.placeCamera(readVec(args, "position", {}), readVec(args, "target", {}));
        return json{{"ok", true}};
    }

    // --- Ambiente (environment::Environment) ---
    if (name == "set_weather" || name == "set_time" || name == "set_wind" || name == "lightning_strike" ||
        name == "get_environment") {
        namespace envns = cramion::environment;
        const auto state = [&](const envns::Environment& env) {
            const envns::EnvironmentRuntime& rt = env.runtime;
            return json{{"weather", envns::presetName(envns::currentPreset(env))},
                        {"target", envns::presetName(env.weather)},
                        {"label", envns::presetLabel(envns::currentPreset(env))},
                        {"transition", rt.initialized ? rt.transition : 1.0f},
                        {"random", env.random_weather},
                        {"hours", env.time_of_day},
                        {"day", env.day},
                        {"month", env.month},
                        {"latitude", env.latitude},
                        {"time_progress", env.time_progress},
                        {"day_length", env.day_length},
                        {"season", envns::seasonName(rt.initialized ? rt.season_now : env.season)},
                        {"temperature", rt.temperature},
                        {"wind", vec(rt.wind)},
                        {"wind_speed", rt.wind_speed},
                        {"wind_direction", rt.wind_direction},
                        {"rain", rt.current.rain},
                        {"snow", rt.current.snow},
                        {"fog", rt.current.fog * env.fog_strength},
                        {"wetness", rt.wetness},
                        {"puddles", rt.puddles},
                        {"snow_cover", rt.snow_cover},
                        {"to_sun", vec(rt.to_sun)},
                        {"particles", a.renderer_.precipitationParticles()}};
        };
        if (name == "get_environment") {
            const envns::Environment* env = envns::findEnvironment(a.world_);
            if (env == nullptr) return json{{"environment", false}};
            json j = state(*env);
            j["environment"] = true;
            return j;
        }
        envns::Environment& env = envns::ensureEnvironment(a.world_);
        if (name == "set_weather") {
            if (args.contains("weather")) {
                envns::WeatherPreset preset{};
                if (!envns::presetFromName(arg(args, "weather"), preset)) {
                    throw ToolError("clima desconocido: usa Clear, Cloudy, Overcast, Foggy, LightRain, Rain, Storm, LightSnow, Snow, Blizzard o Sandstorm");
                }
                envns::setWeather(env, preset, std::max(args.value("seconds", env.transition_time), 0.0f));
            }
            if (args.contains("random")) env.random_weather = args.value("random", false);
            if (args.contains("density")) env.precipitation_density = std::clamp(args.value("density", 1.0f), 0.0f, 3.0f);
            if (args.contains("snow_cover")) envns::setSnowCover(env, args.value("snow_cover", 0.0f));
            if (args.contains("wetness") || args.contains("puddles")) {
                envns::setWetness(env, args.value("wetness", env.runtime.wetness), args.value("puddles", env.runtime.puddles));
            }
        } else if (name == "set_time") {
            if (args.contains("hours")) env.time_of_day = std::clamp(args.value("hours", 12.0f), 0.0f, 24.0f);
            if (args.contains("month")) env.month = std::clamp(args.value("month", 6), 1, 12);
            if (args.contains("day")) env.day = std::clamp(args.value("day", 21), 1, 31);
            if (args.contains("latitude")) env.latitude = std::clamp(args.value("latitude", 40.0f), -89.0f, 89.0f);
            if (args.contains("day_length")) {
                const float minutes = args.value("day_length", 0.0f);
                env.time_progress = minutes > 0.0f;
                if (minutes > 0.0f) env.day_length = std::max(minutes, 0.05f);
            }
            if (args.contains("season")) {
                const std::string season_name = arg(args, "season");
                envns::Season season{};
                if (season_name.empty() || season_name == "auto") {
                    env.season_from_date = true;
                } else if (envns::seasonFromName(season_name, season)) {
                    env.season_from_date = false;
                    env.season = season;
                } else {
                    throw ToolError("estacion desconocida: Spring, Summer, Autumn, Winter o auto");
                }
            }
            env.control_time = true;
        } else if (name == "set_wind") {
            if (args.contains("direction")) env.wind_direction = args.value("direction", 30.0f);
            if (args.contains("strength")) env.wind_strength = std::clamp(args.value("strength", 1.0f), 0.0f, 4.0f);
            if (args.contains("wander")) env.wind_wander = args.value("wander", true);
        } else {
            envns::strikeLightning(env, args.value("distance", -1.0f));
        }
        if (!a.playing()) a.commit();
        return state(env);
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
                    {"AnimationClip", assets::AssetType::AnimationClip}, {"StateMachine", assets::AssetType::StateMachine}};
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
        std::filesystem::path file = assetPath(arg(args, "path"));
        if (const auto info = findAsset(arg(args, "path")); info && !info->path.empty()) file = info->path;
        std::error_code ec;
        if (!std::filesystem::exists(file, ec)) throw ToolError("no existe " + arg(args, "path"));
        if (args.value("ask", false)) {  // la ventana de confirmar del editor (como Supr)
            a.requestDelete({file});
            return json{{"asking", arg(args, "path")}};
        }
        // Lo mismo que Borrar en el editor: cierra sus pestanas, carpetas con todo, a la Papelera.
        a.deletePaths({file});
        if (std::filesystem::exists(file, ec)) throw ToolError("no se pudo borrar " + arg(args, "path"));
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
        if (const std::string image = arg(args, "from_image"); !image.empty()) {
            m = assets::materialFromImage(a.project_.assetsFolder(), image.rfind("Assets/", 0) == 0 ? image.substr(7) : image);
        }
        if (args.contains("color") && args["color"].is_array() && args["color"].size() >= 3) {
            const json& c = args["color"];
            m.base_color = core::Vec4{c[0].get<float>(), c[1].get<float>(), c[2].get<float>(), c.size() > 3 ? c[3].get<float>() : 1.0f};
        }
        m.metallic = args.value("metallic", m.metallic);
        m.roughness = args.value("roughness", m.roughness);
        if (args.contains("emissive")) m.emissive = readVec(args, "emissive", {});
        m.emissive_intensity = args.value("emissive_intensity", m.emissive_intensity);
        const auto texture = [&](const char* key, std::string& target) {
            if (std::string path = arg(args, key); !path.empty()) {
                if (path.rfind("Assets/", 0) == 0) path = path.substr(7);
                target = path;
            }
        };
        texture("albedo_texture", m.albedo);
        texture("normal_texture", m.normal);
        texture("roughness_texture", m.roughness_map);
        texture("occlusion_texture", m.occlusion);
        texture("height_texture", m.height_map);
        texture("cavity_texture", m.cavity_map);
        texture("specular_texture", m.specular_map);
        texture("gloss_texture", m.gloss_map);
        m.height_scale = args.value("height_scale", m.height_scale);
        if (arg(args, "relief") == "tessellation") m.relief = assets::ReliefMode::Tessellation;
        m.tessellation_density = std::clamp(args.value("tessellation_density", m.tessellation_density), 1.0f, 64.0f);
        m.parallax_shadows = args.value("parallax_shadows", m.parallax_shadows);
        if (args.contains("shading")) m.shading = assets::shadingModelFromKey(arg(args, "shading"));
        const auto unit = [&](const char* key, float& value, float lo, float hi) {
            if (args.contains(key) && args[key].is_number()) value = std::clamp(args[key].get<float>(), lo, hi);
        };
        unit("specular_tint", m.specular_tint, 0.0f, 1.0f);
        unit("clearcoat", m.clearcoat, 0.0f, 1.0f);
        unit("clearcoat_roughness", m.clearcoat_roughness, 0.0f, 1.0f);
        unit("sheen", m.sheen, 0.0f, 1.0f);
        unit("sheen_tint", m.sheen_tint, 0.0f, 1.0f);
        unit("subsurface", m.subsurface, 0.0f, 1.0f);
        unit("translucency", m.translucency, 0.0f, 1.0f);
        unit("subsurface_thickness", m.subsurface_thickness, 0.01f, 0.3f);
        unit("anisotropy", m.anisotropy, 0.0f, 1.0f);
        unit("anisotropy_rotation", m.anisotropy_rotation, 0.0f, 180.0f);
        unit("ior", m.ior, 1.0f, 2.5f);
        unit("transmission_thickness", m.transmission_thickness, 0.0f, 0.2f);
        if ((!m.roughness_map.empty() || !m.gloss_map.empty()) && !args.contains("roughness")) m.roughness = 1.0f;
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
    if (name == "inspect_asset") {
        const auto info = findAsset(arg(args, "asset"));
        if (!info) throw ToolError("no existe el asset " + arg(args, "asset"));
        a.inspected_material_ = {};
        a.inspected_render_texture_ = {};
        a.inspected_model_ = {};
        if (info->type == assets::AssetType::Material) a.inspected_material_ = info->uuid;
        else if (info->type == assets::AssetType::RenderTexture) a.inspected_render_texture_ = info->uuid;
        else if (info->type == assets::AssetType::Model) {
            a.inspected_model_ = info->uuid;  // sus ajustes de importacion
            a.inspected_model_active_ = a.active_;
        } else throw ToolError("el Inspector muestra materiales, Render Textures y modelos (importacion)");
        a.focus_inspector_ = true;
        return json{{"asset", info->name}, {"type", assets::assetTypeName(info->type)}};
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
    if (name == "graphics_settings") {
        gfx::GraphicsSettings g = a.renderer_.graphicsSettings();
        bool changed = false;
        if (args.contains("adaptive")) { g.adaptive = args["adaptive"].get<bool>(); changed = true; }
        if (args.contains("target_fps")) { g.target_fps = args["target_fps"].get<float>(); changed = true; }
        if (args.contains("upscaler")) {
            static constexpr const char* kNames[] = {"off", "taa", "fsr1", "fsr3", "dlss"};
            const std::string v = arg(args, "upscaler");
            bool found = false;
            for (int i = 0; i < 5; ++i) {
                if (v == kNames[i]) {
                    g.upscaler = static_cast<gfx::Upscaler>(i);
                    found = true;
                }
            }
            if (!found) throw ToolError("upscaler: off, taa, fsr1, fsr3 o dlss");
            changed = true;
        }
        if (args.contains("resolution")) {
            static constexpr const char* kNames[] = {"native", "quality", "balanced", "performance", "ultra_performance"};
            const std::string v = arg(args, "resolution");
            bool found = false;
            for (int i = 0; i < 5; ++i) {
                if (v == kNames[i]) {
                    g.quality = static_cast<gfx::UpscaleQuality>(i);
                    found = true;
                }
            }
            if (!found) throw ToolError("resolution: native, quality, balanced, performance o ultra_performance");
            changed = true;
        }
        if (args.contains("sharpness")) { g.sharpness = std::clamp(args["sharpness"].get<float>(), 0.0f, 1.0f); changed = true; }
        if (args.contains("shadow_resolution")) { g.shadow_resolution = args["shadow_resolution"].get<int>(); changed = true; }
        if (args.contains("texture_max_size")) { g.texture_max_size = args["texture_max_size"].get<int>(); changed = true; }
        if (changed) {
            a.renderer_.setGraphicsSettings(g);
            a.saveGraphicsSettings();
        }
        if (args.contains("path_tracing")) a.renderer_.setPathTracingEnabled(args["path_tracing"].get<bool>());
        if (args.contains("path_tracing_bounces")) {
            a.renderer_.setPathTracingBounces(static_cast<std::uint32_t>(std::max(args["path_tracing_bounces"].get<int>(), 1)));
        }
        if (args.contains("path_tracing_samples")) {
            a.renderer_.setPathTracingMaxSamples(static_cast<std::uint32_t>(std::max(args["path_tracing_samples"].get<int>(), 1)));
        }
        json j = budgetJson(a.renderer_);
        j["path_tracing"] = json{{"enabled", a.renderer_.pathTracingEnabled()},
                                 {"active", a.renderer_.pathTracingActive()},
                                 {"supported", a.renderer_.rayTracingSupported()},
                                 {"samples", a.renderer_.pathTracingSamples()},
                                 {"max_samples", a.renderer_.pathTracingMaxSamples()},
                                 {"bounces", a.renderer_.pathTracingBounces()}};
        j["shadow_resolution"] = a.renderer_.shadowResolution();
        j["shadow_resolution_setting"] = g.shadow_resolution;
        {
            static constexpr const char* kNames[] = {"off", "taa", "fsr1", "fsr3", "dlss"};
            const vk::Extent2D render = a.renderer_.renderExtent();
            const vk::Extent2D output = a.renderer_.sceneExtent();
            j["upscaler"] = kNames[static_cast<int>(a.renderer_.graphicsSettings().upscaler)];
            j["upscaler_active"] = kNames[static_cast<int>(a.renderer_.activeUpscaler())];
            j["upscaler_status"] = a.renderer_.upscalerStatus();
            j["fsr3_supported"] = a.renderer_.fsr3Supported();
            j["dlss_supported"] = a.renderer_.dlssSupported();
            j["render_size"] = json{render.width, render.height};
            j["output_size"] = json{output.width, output.height};
        }
        return j;
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
                    {"lod_actors", a.renderer_.lodActors()},
                    {"culled_small_actors", a.renderer_.culledSmallActors()},
                    {"shadow_resolution", a.renderer_.shadowResolution()},
                    {"render_extent", json::array({a.renderer_.renderExtent().width, a.renderer_.renderExtent().height})},
                    {"exposure", a.renderer_.currentExposure()},
                    {"scene_luminance", a.renderer_.measuredLuminance()},
                    {"budget", budgetJson(a.renderer_)}};
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
    if (name == "ground_height") {
        float y = 0.0f;
        const float x = args.value("x", 0.0f);
        const float z = args.value("z", 0.0f);
        if (!a.groundHeightAt(x, z, y)) return json{{"found", false}};
        return json{{"found", true}, {"y", y}};
    }
    if (name == "fluid_create") {
        static const std::map<std::string, int> kinds = {{"faucet", 0}, {"block", 1}, {"honey", 2}, {"lava", 3},
                                                         {"world", 4},  {"drain", 5}, {"tank", 6}};
        const auto it = kinds.find(arg(args, "kind", "faucet"));
        if (it == kinds.end()) throw ToolError("kind desconocido: " + arg(args, "kind"));
        bool had_world = false;
        for (const entt::entity h : a.world_.registry().view<fluid::FluidWorld>()) {
            (void)h;
            had_world = true;
            break;
        }
        ecs::Entity e = a.createFluidEntity(it->second);
        if (!e.valid()) throw ToolError("no se pudo crear");
        // Lo creado (y sus hijos) se mueve; si esta llamada creo tambien el
        // FluidWorld, el dominio va con ello (un mundo que ya habia, no).
        if (args.contains("position")) {
            const Vec3 before = e.worldPosition();
            const Vec3 p = readVec(args, "position", before);
            e.setWorldPosition(p);
            if (!had_world && !e.has<fluid::FluidWorld>()) {
                for (const entt::entity h : a.world_.registry().view<fluid::FluidWorld>()) {
                    ecs::Entity w = a.world_.wrap(h);
                    w.setWorldPosition(w.worldPosition() + (p - before));
                    break;
                }
            }
        }
        if (args.contains("fluid") && e.has<fluid::FluidEmitter>()) {
            const int t = fluid::fluidTypeFromName(arg(args, "fluid"));
            if (t < 0) throw ToolError("liquido desconocido: " + arg(args, "fluid"));
            e.get<fluid::FluidEmitter>().fluid = static_cast<fluid::FluidType>(t);
        }
        if (args.contains("emitter_fields") && e.has<fluid::FluidEmitter>()) {
            applyComponents(e, json{{"FluidEmitter", args["emitter_fields"]}});
        }
        if (args.contains("world_fields")) {
            ecs::Entity w = e.has<fluid::FluidWorld>() ? e : ecs::Entity{};
            if (!w.valid()) {
                for (const entt::entity h : a.world_.registry().view<fluid::FluidWorld>()) {
                    w = a.world_.wrap(h);
                    break;
                }
            }
            if (w.valid()) applyComponents(w, json{{"FluidWorld", args["world_fields"]}});
        }
        a.commit();
        json j = summary(e);
        for (const entt::entity h : a.world_.registry().view<fluid::FluidWorld>()) {
            const ecs::Entity w = a.world_.wrap(h);
            j["fluid_world"] = w.name();
            j["domain_center"] = vec(w.worldPosition());
            j["domain_size"] = vec(w.get<fluid::FluidWorld>().size);
            j["simulate_in_editor"] = w.get<fluid::FluidWorld>().simulate_in_editor;
            break;
        }
        return j;
    }
    if (name == "fluid_spawn") {
        fluid::FluidSystem& f = a.fluids_;
        const int t = fluid::fluidTypeFromName(arg(args, "fluid", "water"));
        if (t < 0) throw ToolError("liquido desconocido: " + arg(args, "fluid"));
        const int count = std::clamp(args.value("count", 1000), 1, static_cast<int>(gfx::FluidPass::kMaxParticles));
        f.spawn(readVec(args, "position", {}), count, static_cast<fluid::FluidType>(t), readVec(args, "velocity", {}),
                args.value("radius", 0.0f), args.value("lifetime", 0.0f));
        return json{{"queued", count},
                    {"note", a.playing() || fluid::FluidSystem::previewInEditor(a.world_)
                                 ? "se simula"
                                 : "fuera de Play y sin 'Simular en el editor' el liquido se queda quieto"}};
    }
    if (name == "fluid_state") {
        const fluid::FluidSystem& f = a.fluids_;
        const fluid::FluidSystemStats& s = f.stats();
        json types = json::object();
        for (std::size_t i = 0; i < static_cast<std::size_t>(fluid::FluidType::Count); ++i) {
            const std::uint32_t n = f.particleCount(static_cast<fluid::FluidType>(i));
            if (n > 0) types[fluid::fluidTypeKeys()[i]] = n;
        }
        json j{{"active", s.active},
               {"simulating", s.simulating},
               {"particles", s.particles},
               {"by_type", types},
               {"capacity", s.capacity},
               {"substeps_last_frame", s.substeps},
               {"colliders", s.shapes},
               {"emitters", s.emitters},
               {"floating_bodies", s.floating_bodies},
               {"particle_radius", f.particleRadius()},
               {"gpu_memory_mb", static_cast<double>(s.memory_bytes) / (1024.0 * 1024.0)}};
        if (args.contains("position")) {
            const Vec3 p = readVec(args, "position", {});
            const float r = args.value("radius", 0.0f);
            j["density"] = f.densityAt(p, r);
            j["velocity"] = vec(f.velocityAt(p, r));
            float height = 0.0f;
            j["surface_height"] = f.surfaceHeight(p.x, p.z, height, r) ? json(height) : json(nullptr);
        }
        return j;
    }
    if (name == "fluid_clear") {
        a.fluids_.clear();
        return json{{"cleared", true}};
    }
    if (name == "fire_create") {
        ecs::Entity e;
        if (args.contains("position")) {
            Vec3 p = readVec(args, "position", {});
            float ground = 0.0f;
            if (args.value("on_ground", true) && a.groundHeightAt(p.x, p.z, ground)) p.y = ground;
            e = a.createFireEntity(&p);
        } else {
            e = a.createFireEntity();
        }
        if (!e.valid()) throw ToolError("no se pudo crear");
        fire::Fire& f = e.get<fire::Fire>();
        if (args.contains("size") && args["size"].is_number()) f.size = std::clamp(args["size"].get<float>(), 4.0f, 4000.0f);
        if (args.contains("fields")) applyComponents(e, json{{"Fire", args["fields"]}});
        if (!args.value("ignite", true)) f.ignite_on_start = false;
        a.commit();
        json j = summary(e);
        j["size"] = f.size;
        j["simulate_in_editor"] = f.simulate_in_editor;
        return j;
    }
    if (name == "fire_ignite") {
        const Vec3 p = readVec(args, "position", {});
        const float radius = std::max(args.value("radius", 2.0f), 0.1f);
        if (!a.playing()) {
            // En el editor solo arden las zonas que se simulan.
            for (const entt::entity h : a.world_.registry().view<fire::Fire>()) {
                const ecs::Entity e = a.world_.wrap(h);
                fire::Fire& f = e.get<fire::Fire>();
                const Vec3 local = p - e.worldPosition();
                const float half = f.size * 0.5f + radius;
                if (std::abs(local.x) <= half && std::abs(local.z) <= half) f.simulate_in_editor = true;
            }
        }
        const int zones = fire::ignite(a.world_, p, radius);
        return json{{"zones", zones}, {"note", zones == 0 ? "el punto esta fuera de toda zona de fuego (fire_create)" : ""}};
    }
    if (name == "fire_extinguish") {
        if (args.value("reset", false)) {
            fire::resetAll(a.world_);
            return json{{"reset", true}};
        }
        if (args.value("all", false) || !args.contains("position")) {
            fire::extinguishAll(a.world_);
            return json{{"all", true}};
        }
        const int zones = fire::extinguish(a.world_, readVec(args, "position", {}), std::max(args.value("radius", 10.0f), 0.1f));
        return json{{"zones", zones}};
    }
    if (name == "fire_state") {
        json zones = json::array();
        for (const entt::entity h : a.world_.registry().view<fire::Fire>()) {
            const ecs::Entity e = a.world_.wrap(h);
            const fire::Fire& f = e.get<fire::Fire>();
            const fire::FireStats st = fire::stats(f);
            zones.push_back(json{{"entity", e.name()},
                                 {"uuid", e.uuid().toString()},
                                 {"position", vec(e.worldPosition())},
                                 {"enabled", f.enabled},
                                 {"simulating", st.active},
                                 {"simulate_in_editor", f.simulate_in_editor},
                                 {"size", st.active ? st.size : f.size},
                                 {"resolution", st.resolution},
                                 {"burning_cells", st.burning_cells},
                                 {"smoldering_cells", st.smoldering_cells},
                                 {"burned_cells", st.burned_cells},
                                 {"burnable_cells", st.burnable_cells},
                                 {"burning_area_m2", st.burning_area},
                                 {"burned_fraction", st.burned_fraction},
                                 {"wind", json::array({st.wind.x, st.wind.y})},
                                 {"seconds", st.simulated_seconds}});
        }
        json j{{"zones", zones}, {"playing", a.playing()}};
        if (args.contains("position")) {
            const Vec3 p = readVec(args, "position", {});
            j["heat"] = fire::heatAt(a.world_, p);
            j["burned"] = fire::charAt(a.world_, p);
        }
        return j;
    }
    if (name == "generate_terrain") {
        if (a.playing()) throw ToolError("para el modo Play antes de generar");
        if (a.terrain_gen_job_) throw ToolError("ya se esta generando un terreno");
        terrain::GenSettings gs = a.terrain_gen_;
        static const std::map<std::string, terrain::GenShape> shapes = {
            {"island", terrain::GenShape::Island},       {"archipelago", terrain::GenShape::Archipelago},
            {"continent", terrain::GenShape::Continent}, {"mountains", terrain::GenShape::Mountains},
            {"canyons", terrain::GenShape::Canyons}};
        if (args.contains("shape")) {
            const auto it = shapes.find(arg(args, "shape"));
            if (it == shapes.end()) throw ToolError("forma desconocida: " + arg(args, "shape"));
            gs.shape = it->second;
            if (gs.shape == terrain::GenShape::Mountains || gs.shape == terrain::GenShape::Canyons) gs.ocean = false;
        }
        const auto number = [&](const char* key, float& value, float lo, float hi) {
            if (args.contains(key) && args[key].is_number()) value = std::clamp(args[key].get<float>(), lo, hi);
        };
        if (args.contains("seed") && args["seed"].is_number_integer()) gs.seed = args["seed"].get<std::uint32_t>();
        number("size", gs.size, 256.0f, 16384.0f);
        number("height", gs.height, 20.0f, 3000.0f);
        if (args.contains("resolution") && args["resolution"].is_number_integer()) {
            gs.resolution = std::clamp(args["resolution"].get<int>(), 129, 4097);
            gs.splat_resolution = std::max(gs.resolution - 1, 256);
        }
        number("sea_level", gs.sea_level, 0.0f, 0.9f);
        number("mountains", gs.mountains, 0.0f, 1.0f);
        number("ridges", gs.ridges, 0.0f, 1.0f);
        number("hills", gs.hills, 0.0f, 1.0f);
        number("warp", gs.warp, 0.0f, 1.0f);
        number("plateaus", gs.plateaus, 0.0f, 1.0f);
        number("feature_scale", gs.feature_scale, 0.1f, 8.0f);
        number("erosion", gs.erosion, 0.0f, 2.0f);
        number("erosion_strength", gs.erosion_strength, 0.0f, 1.0f);
        number("thermal", gs.thermal, 0.0f, 1.0f);
        number("beach_width", gs.beach_width, 0.0f, 60.0f);
        number("snow_line", gs.snow_line, 0.1f, 1.0f);
        gs.ocean = args.value("ocean", gs.ocean);
        gs.lakes = args.value("lakes", gs.lakes);
        if (args.contains("rivers") && args["rivers"].is_number_integer()) gs.rivers = std::clamp(args["rivers"].get<int>(), 0, 20);
        a.terrain_gen_ = gs;
        a.terrain_gen_textures_ = args.value("textures", a.terrain_gen_textures_);
        a.terrain_gen_trees_ = args.value("trees", a.terrain_gen_trees_);
        number("tree_density", a.terrain_gen_tree_density_, 1.0f, 2000.0f);
        a.terrain_gen_grass_ = args.value("grass", a.terrain_gen_grass_);
        number("grass_density", a.terrain_gen_grass_density_, 1.0f, 400.0f);
        if (args.contains("houses") && args["houses"].is_number_integer()) {
            a.terrain_gen_houses_ = std::clamp(args["houses"].get<int>(), 0, 60);
        }
        terrain::GenResult result;
        if (!terrain::generateTerrain(gs, result)) throw ToolError("no se pudo generar");
        const std::filesystem::path textures = a.project_.assetsFolder() / "Terrains" / "Texturas";
        if (a.terrain_gen_textures_ && !terrain::generatorTexturesCurrent(dialogs::utf8(textures))) {
            terrain::writeGeneratorTextures(dialogs::utf8(textures), 1024, 7);
        }
        const double seconds = result.seconds;
        const std::size_t rivers = result.rivers.size();
        const std::size_t lakes = result.lakes.size();
        a.applyGeneratedTerrain(result);
        return json{{"seconds", seconds}, {"rivers", rivers}, {"lakes", lakes}, {"group", "Mundo generado"}};
    }
    if (name == "generate_house") {
        const std::string style_name = args.value("style", std::string("log"));
        asset::HouseStyle style = asset::HouseStyle::LogCabin;
        if (style_name == "timber" || style_name == "tablas") style = asset::HouseStyle::TimberCabin;
        else if (style_name == "stone" || style_name == "piedra") style = asset::HouseStyle::StoneCottage;
        else if (style_name == "farm" || style_name == "farmhouse" || style_name == "campo") style = asset::HouseStyle::Farmhouse;
        const std::uint32_t seed = args.contains("seed") && args["seed"].is_number_integer() ? args["seed"].get<std::uint32_t>() : 1U;
        if (args.contains("village") && args["village"].is_number_integer()) {
            if (ecs::Entity old = a.world_.findByName("Aldea"); old.valid()) a.world_.destroy(old);
            std::string error;
            const int made = a.placeVillage(std::clamp(args["village"].get<int>(), 1, 60), seed,
                                            a.world_.findByName("Mundo generado"), &error);
            if (made == 0) throw ToolError(error);
            a.commit();
            return json{{"houses", made}, {"group", "Aldea"}};
        }
        asset::HouseSettings s = asset::housePreset(style, seed);
        const auto number = [&](const char* key, float& value, float lo, float hi) {
            if (args.contains(key) && args[key].is_number()) value = std::clamp(args[key].get<float>(), lo, hi);
        };
        number("width", s.width, 3.0f, 30.0f);
        number("depth", s.depth, 3.0f, 30.0f);
        number("wall_height", s.wall_height, 2.2f, 4.0f);
        number("roof_pitch", s.roof_pitch, 10.0f, 60.0f);
        number("roof_overhang", s.roof_overhang, 0.1f, 2.0f);
        if (args.contains("floors") && args["floors"].is_number_integer()) s.floors = std::clamp(args["floors"].get<int>(), 1, 2);
        if (args.contains("windows") && args["windows"].is_number_integer()) s.windows = std::clamp(args["windows"].get<int>(), -1, 10);
        s.porch = args.value("porch", s.porch);
        s.chimney = args.value("chimney", s.chimney);
        s.shutters = args.value("shutters", s.shutters);
        std::string error;
        const Uuid uuid = a.writeHouseModel(s, &error);
        if (!uuid.valid()) throw ToolError(error);
        core::Vec3 position{};
        position = readVec(args, "position", position);
        if (!args.contains("position") || args.value("on_ground", true)) {
            float y = 0.0f;
            if (a.groundAt(position.x, position.z, y)) position.y = y;
        }
        ecs::Entity house = a.placeHouse(uuid, position, args.value("yaw", 0.0f), {});
        if (!house.valid()) throw ToolError("no se pudo instanciar la casa");
        a.commit();
        return json{{"entity", house.name()}, {"uuid", house.uuid().toString()}, {"model", uuid.toString()},
                    {"position", {position.x, position.y, position.z}}};
    }
    if (name == "cinema") {
        const bool on = args.value("enabled", true);
        a.setCinema(on, std::clamp(args.value("width", 1920), 320, 7680), std::clamp(args.value("height", 1080), 180, 4320),
                    args.value("fps", 30.0f));
        return json{{"enabled", on}, {"fps", 1.0f / a.cinema_dt_}};
    }
    if (name == "cinema_step") {
        if (args.contains("position") && args.contains("target")) {
            a.scene_.placeCamera(readVec(args, "position", {}), readVec(args, "target", {}));
        }
        if (a.cinema_) a.cinema_pending_ += std::clamp(args.value("frames", 1), 0, 100000);
        return json{{"frame", a.cinema_frames_}, {"pending", a.cinema_pending_}};
    }
    if (name == "cinema_state") {
        RECT client{};
        GetClientRect(a.window_.handle(), &client);
        return json{{"enabled", a.cinema_}, {"frame", a.cinema_frames_}, {"pending", a.cinema_pending_},
                    {"width", client.right - client.left}, {"height", client.bottom - client.top}};
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
    if (name == "export_datapack") {
        if (a.playing()) throw ToolError("sal del modo Play (stop) antes de exportar");
        std::vector<std::filesystem::path> scenes;
        if (args.contains("scenes") && args["scenes"].is_array()) {
            for (const json& item : args["scenes"]) {
                const std::string wanted = item.is_string() ? item.get<std::string>() : std::string();
                const auto info = findAsset(wanted);
                if (!info || (info->type != assets::AssetType::Scene && info->type != assets::AssetType::Prefab)) {
                    throw ToolError("no existe la escena o prefab " + wanted);
                }
                scenes.push_back(info->path);
            }
        }
        if (args.contains("entity")) {
            const ecs::Entity target = entity(arg(args, "entity"));
            std::string message;
            const std::filesystem::path prefab = a.prefabForDataPack(target, message);
            if (prefab.empty()) throw ToolError(message);
            scenes.push_back(prefab);
        }
        if (scenes.empty()) {
            if (a.scene_path_.empty()) throw ToolError("la escena abierta no esta guardada");
            if (a.dirty_ && !a.saveScene()) throw ToolError("no se pudo guardar la escena");
            scenes.push_back(a.scene_path_);
        }
        std::filesystem::path file = dialogs::fromUtf8(arg(args, "file"));
        if (file.extension() != project::kDataPackExtension) file += project::kDataPackExtension;
        const std::string pack_name = args.contains("name") ? arg(args, "name") : dialogs::utf8(scenes.front().stem());
        if (args.value("async", false)) {
            if (a.datapack_job_) throw ToolError("ya hay una exportacion de DataPack en marcha");
            a.startDataPackJob(scenes, file, pack_name);
            return json{{"file", dialogs::utf8(file)}, {"started", true}};
        }
        std::string message;
        std::size_t files = 0;
        if (!a.exportDataPack(scenes, file, pack_name, message, &files)) throw ToolError(message);
        return json{{"file", dialogs::utf8(file)}, {"files", files}, {"summary", message}};
    }
    if (name == "save_scene") {
        if (a.playing()) throw ToolError("sal del modo Play (stop) antes de guardar");
        if (args.contains("path") && !arg(args, "path").empty()) {
            std::filesystem::path file = assetPath(arg(args, "path"));
            if (file.extension() != ".crscene") file += ".crscene";
            std::filesystem::create_directories(file.parent_path());
            // Guardar como otra escena: UUID nuevo (si no, choca con la original).
            std::error_code ec;
            if (a.scene_path_.empty() || !std::filesystem::equivalent(file, a.scene_path_, ec)) a.world_.setSceneUuid(Uuid::generate());
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
    if (name == "export_game") {
        if (a.export_job_) throw ToolError("ya hay una exportacion en marcha (export_status)");
        a.ensureBuildConfigs();
        BuildConfig& config = a.build_configs_.current();
        if (args.contains("platform")) {
            const std::string platform = arg(args, "platform");
            if (platform != "windows" && platform != "android") throw ToolError("platform: windows o android");
            config.platform = platform == "android" ? BuildPlatform::Android : BuildPlatform::Windows;
            a.saveBuildConfigsNow();
        }
        const bool install = args.value("install", false);
        if (args.contains("device")) {
            a.refreshAndroidDevices();
            const std::string wanted = arg(args, "device");
            for (const std::string& d : a.export_devices_) {
                if (d.rfind(wanted, 0) == 0) a.export_device_ = d;
            }
        }
        a.export_message_.clear();
        a.exportGame(install);
        a.export_setup_ = false;  // sin la ventana: directo
        a.startExport(dialogs::fromUtf8(arg(args, "folder")));
        if (!a.export_job_) throw ToolError(a.export_message_.empty() ? std::string("no se pudo empezar") : a.export_message_);
        return json{{"started", true}, {"platform", config.platform == BuildPlatform::Android ? "android" : "windows"}};
    }
    if (name == "export_status") {
        if (a.export_job_) {
            EditorApp::ExportJob& j = *a.export_job_;
            std::string current;
            {
                std::lock_guard lock(j.mutex);
                current = j.current;
            }
            const float phase = j.phase.load();
            const double total = std::max<double>(static_cast<double>(j.total.load()), 1.0);
            return json{{"running", true},
                        {"step", phase >= 0.0f ? "android" : "copy"},
                        {"progress", phase >= 0.0f ? phase : static_cast<float>(static_cast<double>(j.done.load()) / total)},
                        {"current", current}};
        }
        return json{{"running", false}, {"message", a.export_message_}};
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
    if (name == "create_state_machine" || name == "get_state_machine" || name == "update_state_machine" ||
        name == "assign_state_machine" || name == "state_machine_debug") {
        needProject();
        // Las entidades llegan como UUID (aqui se aceptan tambien nombre y ruta).
        json in = args;
        if (args.contains("entity")) in["entity"] = entity(arg(args, "entity")).uuid().toString();
        if (args.contains("attach_to")) in["attach_to"] = entity(arg(args, "attach_to")).uuid().toString();
        std::string error;
        const std::string out = a.stateMachineMcpTool(name, in.dump(), error);
        if (!error.empty()) throw ToolError(error);
        return json::parse(out, nullptr, false);
    }
    if (name == "run_tests" || name == "test_results" || name == "bake_lighting" || name == "lighting_state" ||
        name == "create_vfx" || name == "vfx_control" || name == "create_visual_script" || name == "create_shader_graph" ||
        name == "create_behavior_tree") {
        needProject();
        json in = args;
        // Las entidades llegan como UUID (aqui se aceptan tambien nombre y ruta).
        if (args.contains("entity")) in["entity"] = entity(arg(args, "entity")).uuid().toString();
        if (args.contains("attach_to")) in["attach_to"] = entity(arg(args, "attach_to")).uuid().toString();
        std::string error;
        const std::string out = a.mcpTools21(name, in.dump(), error);
        if (!error.empty()) throw ToolError(error);
        return json::parse(out, nullptr, false);
    }
    if (name == "run_lua") {
        std::string out;
        const bool ok = a.scripts_.run(arg(args, "code"), &out, &a.world_);
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
                                    {"serverInfo", {{"name", "cramion-editor"}, {"version", "0.6.0"}}},
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

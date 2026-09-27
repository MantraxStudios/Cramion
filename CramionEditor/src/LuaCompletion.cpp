#include "LuaCompletion.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <set>
#include <unordered_map>

namespace cramion::editor {

namespace {

using List = std::vector<LuaCompletion>;

LuaCompletion fn(const char* name, const char* args, const char* doc) {
    return LuaCompletion{name, std::string(name) + "(", std::string(name) + "(" + args + ")  -  " + doc, 2};
}
LuaCompletion prop(const char* name, const char* doc) { return LuaCompletion{name, name, doc, 3}; }

const List& keywords() {
    static const List list = [] {
        List l;
        for (const char* k : {"and", "break", "do", "else", "elseif", "end", "false", "for", "function", "if", "in",
                              "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while"}) {
            l.push_back(LuaCompletion{k, k, "palabra clave", 0});
        }
        return l;
    }();
    return list;
}

const List& globals() {
    static const List list = {
        LuaCompletion{"self", "self", "la instancia del script (self.entity = su objeto)", 1},
        LuaCompletion{"Vec3", "Vec3", "vector 3D: Vec3(x, y, z), Vec3.up, a + b, v * 2, v:length()", 1},
        LuaCompletion{"Scene", "Scene", "buscar, crear y destruir objetos", 1},
        LuaCompletion{"Input", "Input", "teclado y raton", 1},
        LuaCompletion{"Time", "Time", "deltaTime, time, frameCount", 1},
        LuaCompletion{"Physics", "Physics", "consultas de fisica (raycast)", 1},
        LuaCompletion{"Audio", "Audio", "sonidos sueltos (playOneShot)", 1},
        LuaCompletion{"Navigation", "Navigation", "la malla de navegacion (findPath, randomPoint...)", 1},
        LuaCompletion{"Mesh", "Mesh", "mallas creadas por codigo (Mesh.new, Mesh.cube, entity.mesh)", 1},
        LuaCompletion{"Voxel", "Voxel", "el mundo de bloques (getBlock, setBlock, raycast, mundos)", 1},
        LuaCompletion{"Debug", "Debug", "mensajes en la Consola", 1},
        LuaCompletion{"Prefs", "Prefs", "datos guardados (como PlayerPrefs)", 1},
        LuaCompletion{"Game", "Game", "el juego (quit)", 1},
        LuaCompletion{"Graphics", "Graphics", "configuracion grafica: calidad, resolucion, sombras, texturas, ventana, post", 1},
        LuaCompletion{"Network", "Network", "multijugador: host, connect, send/on, spawn de objetos de red", 1},
        LuaCompletion{"Http", "Http", "peticiones HTTPS a servidores y webs (get, post, request)", 1},
        LuaCompletion{"Json", "Json", "texto JSON <-> tablas (encode, decode)", 1},
        LuaCompletion{"Mathf", "Mathf", "lerp, clamp, smoothDamp, angulos, ruido...", 1},
        LuaCompletion{"Quat", "Quat", "rotaciones: Quat.euler, Quat.lookRotation, q * v", 1},
        LuaCompletion{"Random", "Random", "aleatorios con semilla: range, int, pick, onUnitSphere...", 1},
        LuaCompletion{"math", "math", "biblioteca math de Lua", 1},
        LuaCompletion{"string", "string", "biblioteca string de Lua", 1},
        LuaCompletion{"table", "table", "biblioteca table de Lua", 1},
        fn("print", "...", "escribe en la Consola"),
        fn("pairs", "t", "recorre una tabla (clave, valor)"),
        fn("ipairs", "t", "recorre una lista (indice, valor)"),
        fn("tostring", "v", "a texto"),
        fn("tonumber", "v", "a numero"),
        fn("type", "v", "tipo del valor"),
        fn("setmetatable", "t, meta", "metatabla"),
    };
    return list;
}

const std::unordered_map<std::string, List>& tables() {
    static const std::unordered_map<std::string, List> map = {
        {"Input",
         {fn("getKey", "\"W\"", "tecla mantenida"), fn("getKeyDown", "\"Space\"", "tecla pulsada este frame"),
          fn("getKeyUp", "\"E\"", "tecla soltada este frame"),
          fn("getAxis", "\"Horizontal\"", "-1..1: Horizontal (A/D), Vertical (W/S), Mouse X, Mouse Y"),
          fn("getMouseButton", "0", "boton del raton mantenido (0 izq, 1 der, 2 medio)"),
          fn("getMouseButtonDown", "0", "boton pulsado este frame"), fn("getMouseButtonUp", "0", "boton soltado"),
          fn("mousePosition", "", "Vec3 con la posicion del raton"), fn("mouseDelta", "", "Vec3 con el movimiento"),
          fn("lockCursor", "true", "captura el raton (primera persona); false lo suelta"),
          fn("isCursorLocked", "", "esta capturado?")}},
        {"Scene",
         {fn("find", "\"nombre\"", "el primer objeto con ese nombre (o nil)"),
          fn("findWithTag", "\"tag\"", "el primer objeto con ese tag"),
          fn("findAllWithTag", "\"tag\"", "lista de objetos con ese tag"),
          fn("create", "\"nombre\", posicion", "objeto vacio nuevo"),
          fn("instantiate", "entity, posicion", "copia de un objeto (con hijos y componentes)"),
          fn("destroy", "entity", "lo destruye al final del frame"),
          fn("load", "\"Nivel2\"", "cambia de escena al terminar el frame (nombre o ruta del .crscene)"),
          fn("name", "", "nombre de la escena actual"),
          fn("origin", "", "x, y, z (doble precision) del origen flotante del mundo"),
          fn("toAbsolute", "posicion", "x, y, z absolutos (para guardar posiciones en una partida)"),
          fn("toLocal", "x, y, z", "Vec3 local de una posicion absoluta guardada")}},
        {"Prefs",
         {fn("setInt", "\"clave\", 3", "guarda un entero"), fn("getInt", "\"clave\", 0", "lee un entero (o el valor por defecto)"),
          fn("setFloat", "\"clave\", 0.5", "guarda un numero"), fn("getFloat", "\"clave\", 0.0", "lee un numero"),
          fn("setString", "\"clave\", \"texto\"", "guarda un texto"), fn("getString", "\"clave\", \"\"", "lee un texto"),
          fn("hasKey", "\"clave\"", "existe?"), fn("deleteKey", "\"clave\"", "la borra"), fn("deleteAll", "", "borra todo")}},
        {"Game", {fn("quit", "", "cierra el juego (en el editor, sale de Play)")}},
        {"Graphics",
         {fn("setQuality", "\"Alta\"", "calidad rapida: Baja, Media, Alta, Ultra (o 0..3)"),
          fn("getQuality", "", "la ultima calidad rapida o Personalizada"),
          fn("qualityLevels", "", "lista de calidades"),
          fn("set", "\"vsync\", true", "cambia una opcion; o Graphics.set{ clave = valor, ... }"),
          fn("get", "\"texture_quality\"", "valor de una opcion"), fn("getAll", "", "tabla clave -> valor"),
          fn("options", "", "lista {key, value, writable, description, choices} para un menu"),
          fn("resolutions", "", "resoluciones del monitor {width, height}"),
          fn("save", "", "guarda la configuracion del jugador (juego exportado)"),
          fn("getPost", "\"bloom\"", "campo del post-procesado global"),
          fn("setPost", "\"bloom\", false", "cambia el post-procesado global"),
          fn("postKeys", "", "claves del post-procesado"),
          prop("post", "post-procesado global: Graphics.post.bloom = false"),
          prop("upscaler", "off, taa o fsr1"), prop("resolution", "native, quality, balanced, performance, ultra_performance, custom"),
          prop("resolution_scale", "0.25..1"), prop("sharpness", "0..1"), prop("vsync", "true/false"),
          prop("adaptive", "presupuesto adaptativo"), prop("target_fps", "FPS objetivo"),
          prop("shadows", "true/false"), prop("shadow_quality", "auto, low, medium, high, ultra"),
          prop("shadow_resolution", "0 = auto, 512..8192"), prop("texture_quality", "auto, low, medium, high, ultra, max"),
          prop("texture_max_size", "0 = auto, 64..16384"), prop("ray_tracing", "true/false"),
          prop("reflection_probe", "true/false"), prop("occlusion_culling", "true/false"),
          prop("window_mode", "maximized, fullscreen o windowed"), prop("window_width", "ancho (windowed)"),
          prop("window_height", "alto (windowed)"), prop("gpu", "nombre de la GPU (lectura)"),
          prop("vram_mb", "VRAM (lectura)"), prop("screen_width", "ancho de salida (lectura)"),
          prop("screen_height", "alto de salida (lectura)"),
          prop("render_width", "ancho interno de render (lectura)"), prop("render_height", "alto interno de render (lectura)"),
          prop("gpu_ms", "ms de GPU por frame (lectura)"), prop("hardware_tier", "gama del equipo (lectura)"),
          prop("vram_used_mb", "VRAM usada (lectura)"), prop("ray_tracing_supported", "la GPU tiene trazado de rayos (lectura)"),
          prop("cascade_debug", "colorea las cascadas de sombra (depuracion)"),
          prop("foliage_trees", "arboles de la vegetacion en la GPU (lectura)"),
          prop("foliage_visible", "arboles dibujados el ultimo frame (lectura)"),
          prop("foliage_near", "arboles con todo el detalle (lectura)"),
          prop("foliage_triangles", "triangulos de la vegetacion (lectura)")}},
        // Tablas que llegan a funciones (por el nombre habitual del parametro).
        {"contact",
         {prop("point", "Vec3 punto del choque"), prop("normal", "Vec3 normal del choque"),
          prop("relativeVelocity", "Vec3 velocidad relativa del choque")}},
        {"res",
         {prop("ok", "true si el estado es 2xx"), prop("status", "estado HTTP (0 = sin respuesta)"),
          prop("body", "cuerpo como texto"), prop("data", "el JSON ya decodificado (tablas)"),
          prop("headers", "cabeceras (nombres en minusculas)"), prop("error", "por que fallo"),
          prop("time", "segundos que tardo")}},
        {"Network",
         {fn("host", "7777, 8", "crea la partida (eres el servidor y juegas); devuelve ok, error"),
          fn("connect", "\"127.0.0.1\", 7777", "se une a una partida (llega onConnected u onDisconnected)"),
          fn("disconnect", "", "sale de la partida (o la cierra si eres el servidor)"),
          fn("isServer", "", "eres el servidor?"), fn("isClient", "", "eres un cliente?"),
          fn("isConnected", "", "en partida (servidor abierto o cliente dentro)"),
          fn("isConnecting", "", "cliente esperando respuesta"), fn("isActive", "", "hay sesion de red"),
          fn("myId", "", "tu id de jugador (el servidor es 1)"), fn("players", "", "lista de ids de jugadores"),
          fn("playerCount", "", "cuantos jugadores"), fn("ping", "id", "ida y vuelta en ms"),
          fn("stats", "", "{sent, received, objects}"),
          fn("send", "\"chat\", datos, destino", "mensaje (destino: nil = todos, \"server\" o un id)"),
          fn("on", "\"chat\", function(datos, de) end", "recibe un mensaje"), fn("off", "\"chat\"", "deja de recibirlo"),
          fn("onPlayerJoined", "function(id) end", "entra un jugador"), fn("onPlayerLeft", "function(id) end", "sale un jugador"),
          fn("onConnected", "function(id) end", "cliente: ya estas dentro"),
          fn("onDisconnected", "function(motivo) end", "fuera de la partida"),
          fn("spawn", "\"Prefabs/Jugador\", posicion, dueno", "crea un objeto de red en todos (solo el servidor)"),
          fn("destroy", "entity", "lo borra en todos (solo el servidor)"),
          fn("objects", "", "todas las entidades de red"), fn("find", "netId", "entidad por su id de red"),
          fn("loadScene", "\"Nivel2\"", "todos cargan la escena (solo el servidor)"),
          prop("SERVER", "id del servidor (1)")}},
        {"Http",
         {fn("get", "\"https://...\", function(res) end, cabeceras", "GET; res = {ok, status, body, data, headers, error}"),
          fn("post", "\"https://...\", datos, function(res) end, cabeceras", "POST; datos texto o tabla (se manda como JSON)"),
          fn("request", "{ url = \"https://...\", method = \"PUT\", headers = {}, body = {}, timeout = 20 }, function(res) end",
             "cualquier metodo, con tiempo maximo y tamano maximo (maxSize)"),
          fn("urlEncode", "\"hola mundo\"", "texto seguro para una URL"),
          fn("query", "{ q = \"hola\", page = 2 }", "\"page=2&q=hola\" (codificado)"),
          fn("pending", "", "peticiones sin terminar"), fn("cancelAll", "", "cancela todas")}},
        {"Json",
         {fn("encode", "tabla, bonito", "tabla -> texto JSON (nil + error si no se puede)"),
          fn("decode", "texto", "texto JSON -> tabla (nil + error si no es JSON)")}},
        {"Time",
         {prop("deltaTime", "segundos desde el frame anterior"), prop("time", "segundos desde el Play"),
          prop("frameCount", "frames desde el Play"), prop("fixedDeltaTime", "paso fijo de la fisica")}},
        {"Physics", {fn("raycast", "origen, direccion, distancia", "nil o {entity, point, normal, distance}")}},
        {"Audio", {fn("playOneShot", "\"Audio/golpe.wav\", posicion, volumen", "sonido suelto (sin posicion = 2D)"),
                   fn("setOcclusion", "true", "paredes tapan los sonidos (Audio Listener)"),
                   fn("occlusion", "", "esta la oclusion activa?"),
                   fn("setLowPass", "true, 800", "todo apagado (bajo el agua, pausa)"),
                   fn("reverbLevel", "", "reverberacion que se oye ahora (zonas)")}},
        {"Navigation",
         {fn("findPath", "desde, hasta", "nil o lista de Vec3 (los giros del camino)"),
          fn("projectPoint", "Vec3, radio", "nil o el punto de la malla mas cercano"),
          fn("randomPoint", "centro, radio", "nil o un punto al azar de la malla"),
          fn("raycast", "desde, hasta", "llega?, punto del choque"), fn("isReady", "", "hay malla?")}},
        {"Mesh",
         {fn("new", "\"nombre\"", "malla vacia"), fn("cube", "tamano", "cubo (numero o Vec3)"),
          fn("quad", "ancho, alto", "cuadrado en XY"), fn("plane", "ancho, fondo, segX, segZ", "plano subdividido"),
          fn("sphere", "radio, segmentos, anillos", "esfera"), fn("cylinder", "radio, alto, segmentos", "cilindro"),
          fn("capsule", "radio, alto, segmentos", "capsula"), fn("wireCube", "tamano, grosor", "aristas de una caja (contornos)")}},
        {"Voxel",
         {fn("getBlock", "x, y, z", "numero del bloque (0 = aire)"), fn("setBlock", "x, y, z, \"stone\"", "pone o quita un bloque"),
          fn("getBlockAt", "Vec3", "bloque en ese punto"), fn("blockInfo", "\"stone\"", "{name, label, solid, hardness...}"),
          fn("blockId", "\"stone\"", "numero de un bloque"), fn("blockCount", "", "cuantos tipos de bloque hay"), fn("blockColor", "\"stone\"", "color medio del bloque (Vec3)"), fn("raycast", "origen, direccion, distancia", "nil o {block, normal, id, point, distance}"),
          fn("moveBox", "centro, semiejes, delta", "posicion, enSuelo, techo, pared"), fn("boxCollides", "centro, semiejes", "toca bloques?"),
          fn("surfaceHeight", "x, z", "altura del terreno"), fn("isReady", "Vec3", "ya esta generado?"), fn("inWater", "Vec3", "esta en el agua?"),
          fn("skyLight", "x, y, z", "luz del cielo 0..15"), fn("blockLight", "x, y, z", "luz de antorchas 0..15"),
          fn("newWorld", "\"nombre\", semilla", "mundo nuevo con nombre"), fn("loadWorld", "\"nombre\"", "carga un mundo guardado"),
          fn("saveWorld", "", "guarda"), fn("listWorlds", "", "lista de mundos"), fn("deleteWorld", "\"nombre\"", "lo borra"),
          fn("setMeta", "\"clave\", \"texto\"", "dato guardado con el mundo"), fn("getMeta", "\"clave\", \"\"", "lee un dato"),
          fn("worldName", "", "nombre del mundo"), fn("seed", "", "semilla"), fn("isActive", "", "hay mundo de bloques?")}},
        {"Debug",
         {fn("log", "...", "mensaje en la Consola"), fn("warn", "...", "aviso"), fn("error", "...", "error")}},
        {"Mathf",
         {prop("pi", "3.14159"), prop("tau", "2 pi"), prop("deg2rad", "grados a radianes"), prop("rad2deg", "radianes a grados"),
          prop("infinity", "infinito"), prop("epsilon", "numero muy pequeno"),
          fn("abs", "v", ""), fn("sign", "v", "-1 o 1"), fn("min", "a, b, ...", ""), fn("max", "a, b, ...", ""),
          fn("floor", "v", ""), fn("ceil", "v", ""), fn("round", "v, decimales", "redondea"), fn("sqrt", "v", ""),
          fn("pow", "a, b", ""), fn("exp", "v", ""), fn("log", "v, base", ""), fn("log10", "v", ""),
          fn("sin", "rad", ""), fn("cos", "rad", ""), fn("tan", "rad", ""), fn("asin", "v", ""), fn("acos", "v", ""),
          fn("atan", "v", ""), fn("atan2", "y, x", "angulo en radianes"),
          fn("clamp", "v, min, max", "limita"), fn("clamp01", "v", "0..1"), fn("lerp", "a, b, t", "interpola (t en 0..1)"),
          fn("lerpUnclamped", "a, b, t", "interpola sin limitar t"), fn("inverseLerp", "a, b, v", "t de v entre a y b"),
          fn("remap", "v, desde1, hasta1, desde2, hasta2", "pasa de un rango a otro"),
          fn("smoothstep", "a, b, v", "suave 0..1"), fn("smootherstep", "a, b, v", "mas suave 0..1"),
          fn("moveTowards", "actual, objetivo, paso", "se acerca"),
          fn("smoothDamp", "actual, objetivo, vel, tiempo, dt", "devuelve valor, vel"),
          fn("wrap", "t, largo", "repite 0..largo (Repeat)"), fn("pingPong", "t, largo", "va y vuelve"),
          fn("approximately", "a, b", "casi iguales?"), fn("deltaAngle", "a, b", "diferencia de angulos (grados)"),
          fn("lerpAngle", "a, b, t", "interpola angulos"), fn("moveTowardsAngle", "a, b, paso", "gira hacia"),
          fn("isPowerOfTwo", "n", ""), fn("nextPowerOfTwo", "n", ""),
          fn("perlinNoise", "x, y", "ruido 0..1"), fn("perlinNoise3", "x, y, z", "ruido -1..1"),
          fn("fractalNoise", "x, y, octavas", "ruido por capas 0..1"), fn("random", "min, max", "aleatorio")}},
        {"Random",
         {fn("seed", "n", "fija la semilla"), fn("value", "", "0..1"), fn("range", "min, max", "decimal"),
          fn("int", "min, max", "entero (incluye los dos)"), fn("chance", "0.25", "true con esa probabilidad"),
          fn("sign", "", "-1 o 1"), fn("onUnitSphere", "", "direccion al azar"),
          fn("insideUnitSphere", "", "punto dentro de la esfera"), fn("insideUnitCircle", "", "punto en el suelo (XZ)"),
          fn("rotation", "", "Quat al azar"), fn("pick", "lista", "un elemento al azar"), fn("shuffle", "lista", "baraja")}},
        {"Quat",
         {prop("identity", "sin giro"), fn("euler", "x, y, z", "de grados (como rotation)"),
          fn("angleAxis", "grados, eje", "giro sobre un eje"), fn("lookRotation", "direccion, arriba", "mira hacia"),
          fn("fromToRotation", "desde, hasta", "gira un vector a otro"), fn("slerp", "a, b, t", "interpola"),
          fn("lerp", "a, b, t", "interpola (rapido)"), fn("rotateTowards", "a, b, grados", "gira como mucho"),
          fn("angle", "a, b", "grados entre dos rotaciones"), fn("inverse", "q", "la contraria"), fn("dot", "a, b", "")}},
        {"Vec3",
         {prop("zero", "Vec3(0, 0, 0)"), prop("one", "Vec3(1, 1, 1)"), prop("up", "Vec3(0, 1, 0)"),
          prop("down", "Vec3(0, -1, 0)"), prop("right", "Vec3(1, 0, 0)"), prop("left", "Vec3(-1, 0, 0)"),
          prop("forward", "Vec3(0, 0, -1)"), prop("back", "Vec3(0, 0, 1)"),
          fn("lerp", "a, b, t", "interpola dos vectores"), fn("lerpUnclamped", "a, b, t", ""), fn("slerp", "a, b, t", "interpola girando"),
          fn("moveTowards", "a, b, paso", "se acerca"), fn("smoothDamp", "actual, objetivo, vel, tiempo, dt", "devuelve pos, vel"),
          fn("distance", "a, b", ""), fn("sqrDistance", "a, b", ""), fn("angle", "a, b", "grados"),
          fn("signedAngle", "a, b, eje", "grados con signo"), fn("dot", "a, b", ""), fn("cross", "a, b", ""),
          fn("project", "v, sobre", ""), fn("projectOnPlane", "v, normal", ""), fn("reflect", "dir, normal", "rebote"),
          fn("min", "a, b", ""), fn("max", "a, b", ""), fn("scale", "a, b", "por componentes")}},
        {"math",
         {fn("abs", "x", ""), fn("floor", "x", ""), fn("ceil", "x", ""), fn("sqrt", "x", ""), fn("sin", "x", ""),
          fn("cos", "x", ""), fn("atan", "y, x", ""), fn("min", "a, b", ""), fn("max", "a, b", ""),
          fn("random", "m, n", ""), prop("pi", ""), prop("huge", "")}},
        {"string",
         {fn("format", "\"%d\", ...", ""), fn("sub", "s, i, j", ""), fn("len", "s", ""), fn("upper", "s", ""),
          fn("lower", "s", ""), fn("find", "s, patron", ""), fn("rep", "s, n", "")}},
        {"table",
         {fn("insert", "t, v", ""), fn("remove", "t, i", ""), fn("concat", "t, sep", ""), fn("sort", "t, cmp", "")}},
    };
    return map;
}

// Miembros de un objeto: con '.' (propiedades) o ':' (metodos).
const List& entityProperties() {
    static const List list = {
        prop("name", "nombre"), prop("tag", "tag"), prop("active", "activo (true/false)"),
        prop("position", "Vec3 en el mundo"), prop("localPosition", "Vec3 respecto al padre"),
        prop("rotation", "Vec3 en grados"), prop("quaternion", "Quat de su giro"), prop("scale", "Vec3"), prop("forward", "Vec3 hacia delante"),
        prop("right", "Vec3 a la derecha"), prop("up", "Vec3 hacia arriba"), prop("parent", "el padre (o nil)"),
        prop("velocity", "Vec3 de su Rigidbody"), prop("angularVelocity", "Vec3 de giro (rad/s)"), prop("speed", "km/h de su Vehicle"),
        prop("rpm", "rpm del motor del Vehicle"), prop("gear", "marcha del Vehicle"),
        prop("isMoving", "su NavAgent va hacia un destino"), prop("remainingDistance", "metros que le quedan"),
        prop("navVelocity", "Vec3 de su NavAgent"), prop("mesh", "malla creada por codigo de su MeshRenderer"),
        prop("castShadows", "su MeshRenderer proyecta sombra"), prop("texture", "imagen de su UIImage"),
        prop("alpha", "transparencia de su UIImage/UIText"), prop("uiPosition", "Vec3 posicion de su RectTransform"),
        prop("uiSize", "Vec3 tamano de su RectTransform"), prop("text", "texto de su UIText o campo de texto"),
        prop("value", "valor de su Slider o Casilla"), prop("color", "Vec3 color de su UIImage/UIText"),
        prop("interactable", "su boton/slider/campo responde"),
        prop("ragdoll", "ragdoll activado (true/false)")};
    return list;
}
const List& entityMethods() {
    static const List list = {
        fn("translate", "Vec3", "mueve en el mundo"), fn("translateLocal", "Vec3", "mueve en sus ejes"),
        fn("rotate", "Vec3 grados", "gira"), fn("lookAt", "Vec3", "mira hacia un punto"),
        fn("distanceTo", "otra", "distancia a otro objeto"), fn("destroy", "", "lo destruye"),
        fn("addForce", "Vec3, \"impulse\"", "fuerza (force, impulse, acceleration, velocity)"),
        fn("addImpulse", "Vec3", "impulso"), fn("addTorque", "Vec3", "par de giro"),
        fn("setVehicleInput", "acelerador, direccion, freno, freno de mano", "conduce su Vehicle"),
        fn("playSound", "", "su Audio Source"), fn("stopSound", "", "para su sonido"),
        fn("isPlayingSound", "", "suena?"),
        fn("setSoundEffect", "\"lowpass\", true, 800", "lowpass/highpass/echo/reverb/occlusion"),
        prop("soundOcclusion", "paredes que tapan su sonido ahora (-1 = no suena)"),
        prop("audioOcclusion", "oclusion de su Audio Listener (true/false)"), fn("playAnimation", "\"Correr\", true", "clip del Animator"),
        fn("setAnimatorFloat", "\"velocidad\", 1.0", "parametro del Animator Controller"),
        fn("isMine", "", "red: este objeto lo controlas tu (sin red, siempre)"),
        prop("netId", "red: id del objeto (0 = no es de red)"), prop("netOwner", "red: id del jugador dueno"),
        fn("setNetVar", "\"vida\", 100", "red: variable sincronizada (dueno o servidor)"),
        fn("getNetVar", "\"vida\"", "red: lee una variable sincronizada"),
        fn("setAnimatorBool", "\"saltando\", true", ""), fn("setAnimatorTrigger", "\"atacar\"", ""),
        fn("hasComponent", "\"Rigidbody\"", "tiene ese componente?"), fn("getScript", "", "la instancia de su script"),
        fn("find", "\"hijo\"", "un hijo por nombre"), fn("valid", "", "sigue existiendo?"),
        fn("moveTo", "Vec3", "su NavAgent camina hasta alli por la malla"), fn("stopMoving", "", "se para"),
        fn("addComponent", "\"MeshCollider\"", "anade un componente por su nombre"),
        fn("removeComponent", "\"MeshCollider\"", "quita un componente"),
        fn("setMaterial", "0, \"Materials/Brillo.crmat\"", "material .crmat de un hueco de su MeshRenderer"),
        fn("getField", "\"Light\", \"intensity\"", "campo de cualquier componente"),
        fn("setField", "\"Light\", \"intensity\", 2", "cambia un campo (listas: \"chains[1].pull\")"),
        fn("getFields", "\"Ragdoll\"", "todos los campos de un componente"),
        fn("getBones", "", "nombres de los huesos"), fn("getBonePosition", "\"Head\"", "Vec3 del hueso en el mundo"),
        fn("getBoneRotation", "\"Head\"", "Quat del hueso"),
        fn("setBoneRotation", "\"Head\", Vec3 grados", "gira el hueso encima de la animacion"),
        fn("setBoneOffset", "\"Hips\", Vec3", "desplaza el hueso"), fn("setBoneScale", "\"Head\", 1.2", "escala el hueso"),
        fn("resetBone", "\"Head\"", "quita los cambios del hueso"), fn("resetBones", "", "quita los de todos"),
        fn("showBones", "true", "dibuja los huesos en la Escena"),
        fn("setIKTarget", "\"PieIzq\", objetivo", "objetivo de una cadena IK (entidad, Vec3 o nil)"),
        fn("setIKHint", "\"PieIzq\", objetivo", "pole (hacia donde dobla)"), fn("setIKWeight", "\"PieIzq\", 1", "peso 0..1"),
        fn("setLookAt", "objetivo, peso", "mirar con cabeza y cuello"), fn("setFootGrounding", "true", "patas al suelo"),
        fn("setupCreatureIK", "", "configura el IK solo (patas, cuello, cola)"),
        fn("setupRagdoll", "", "genera los huesos del ragdoll"), fn("setupPhysBones", "", "detecta pelo, colas, orejas..."),
        fn("addRagdollForce", "Vec3, \"Spine\"", "empujon al ragdoll (en un hueso)"),
        fn("attachToBone", "modelo, \"RightHand\", offset, giro", "sigue a un hueso (Bone Socket)"),
        fn("detachFromBone", "", "deja de seguir al hueso")};
    return list;
}
const List& vectorMembers(char accessor) {
    static const List fields = {prop("x", ""), prop("y", ""), prop("z", "")};
    static const List methods = {fn("length", "", "longitud"), fn("sqrLength", "", "longitud al cuadrado"),
                                 fn("normalized", "", "de longitud 1"), fn("clampLength", "max", "limita la longitud"),
                                 fn("dot", "otro", "producto escalar"), fn("cross", "otro", "producto vectorial"),
                                 fn("distance", "otro", "distancia"), fn("angle", "otro", "grados"),
                                 fn("lerp", "otro, t", "interpola"), fn("moveTowards", "otro, paso", "se acerca"),
                                 fn("abs", "", ""), fn("floor", "", ""), fn("round", "", ""),
                                 fn("copy", "", "copia independiente"), fn("unpack", "", "x, y, z"),
                                 fn("set", "x, y, z", "cambia sus valores")};
    return accessor == ':' ? methods : fields;
}
const List& meshMembers(char accessor) {
    static const List fields = {
        prop("name", "nombre"), prop("vertexCount", "vertices"), prop("triangleCount", "triangulos"),
        prop("subMeshCount", "submallas (materiales)"), prop("vertices", "lista de Vec3"), prop("normals", "lista de Vec3"),
        prop("uv", "lista de {x, y}"), prop("tangents", "lista de Vec3"), prop("triangles", "indices (submalla 0)"),
        prop("boundsMin", "Vec3 esquina minima"), prop("boundsMax", "Vec3 esquina maxima")};
    static const List methods = {
        fn("setTriangles", "indices, submalla", "triangulos de una submalla"), fn("getTriangles", "submalla", "sus indices"),
        fn("setVertex", "i, Vec3", "mueve un vertice (luego apply)"), fn("getVertex", "i", "Vec3 de un vertice"),
        fn("setMaterial", "submalla, {color, metallic, roughness...}", "material de una submalla"),
        fn("getMaterial", "submalla", "su material"), fn("recalculateNormals", "", "normales suaves"),
        fn("recalculateTangents", "", "tangentes"), fn("recalculateBounds", "", "caja"),
        fn("apply", "", "sube los cambios a la GPU"), fn("clear", "", "la vacia"),
        fn("validate", "", "\"\" si se puede dibujar; si no, el motivo"), fn("clone", "", "copia")};
    return accessor == ':' ? methods : fields;
}
const List& engineCallbacks() {
    static const List list = {
        LuaCompletion{"Awake", "Awake()", "al crearse (antes de Start)", 5},
        LuaCompletion{"Start", "Start()", "una vez, antes del primer Update", 5},
        LuaCompletion{"Update", "Update(dt)", "cada frame", 5},
        LuaCompletion{"LateUpdate", "LateUpdate(dt)", "cada frame, tras todos los Update", 5},
        LuaCompletion{"FixedUpdate", "FixedUpdate(dt)", "cada paso de la fisica", 5},
        LuaCompletion{"OnCollisionEnter", "OnCollisionEnter(other, contact)", "empieza un choque", 5},
        LuaCompletion{"OnOriginShift", "OnOriginShift(offset)", "el mundo se desplazo -offset (origen flotante): resta offset a las posiciones guardadas", 5},
        LuaCompletion{"OnCollisionStay", "OnCollisionStay(other, contact)", "sigue el choque", 5},
        LuaCompletion{"OnCollisionExit", "OnCollisionExit(other, contact)", "termina el choque", 5},
        LuaCompletion{"OnTriggerEnter", "OnTriggerEnter(other)", "entra en un trigger", 5},
        LuaCompletion{"OnTriggerStay", "OnTriggerStay(other)", "sigue dentro", 5},
        LuaCompletion{"OnTriggerExit", "OnTriggerExit(other)", "sale del trigger", 5},
        LuaCompletion{"OnDestroy", "OnDestroy()", "al destruirse o parar el Play", 5},
        LuaCompletion{"OnNetVar", "OnNetVar(clave, valor)", "red: cambio una variable sincronizada (setNetVar)", 5}};
    return list;
}

bool identChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Nombres del archivo: locales, funciones, propiedades y campos de self.
void fileSymbols(const std::string& text, List& locals, List& self_fields, List& self_methods) {
    std::set<std::string> seen;
    const auto add = [&](List& to, const std::string& name, const char* detail, int kind) {
        if (name.empty() || !seen.insert(std::to_string(kind) + name).second) return;
        to.push_back(LuaCompletion{name, name, detail, kind});
    };
    static const std::regex local_re(R"(local\s+(?:function\s+)?([A-Za-z_]\w*))");
    static const std::regex function_re(R"(function\s+[A-Za-z_]\w*[:.]([A-Za-z_]\w*))");
    static const std::regex self_re(R"(self\.([A-Za-z_]\w*)\s*=)");
    for (std::sregex_iterator it(text.begin(), text.end(), local_re), end; it != end; ++it) add(locals, (*it)[1], "local", 4);
    for (std::sregex_iterator it(text.begin(), text.end(), function_re), end; it != end; ++it) {
        add(self_methods, (*it)[1], "metodo del script", 2);
    }
    for (std::sregex_iterator it(text.begin(), text.end(), self_re), end; it != end; ++it) {
        add(self_fields, (*it)[1], "campo de self", 3);
    }
    // properties = { nombre = valor, ... }
    const std::size_t at = text.find("properties");
    if (at != std::string::npos) {
        const std::size_t open = text.find('{', at);
        if (open != std::string::npos) {
            int depth = 0;
            std::size_t close = open;
            for (; close < text.size(); ++close) {
                if (text[close] == '{') ++depth;
                if (text[close] == '}' && --depth == 0) break;
            }
            const std::string body = text.substr(open + 1, close - open - 1);
            static const std::regex prop_re(R"(([A-Za-z_]\w*)\s*=)");
            for (std::sregex_iterator it(body.begin(), body.end(), prop_re), end; it != end; ++it) {
                add(self_fields, (*it)[1], "propiedad (Inspector)", 3);
            }
        }
    }
    add(self_fields, "entity", "su objeto (Entity)", 3);
}

}  // namespace

bool luaCompletionContext(const std::string& text, std::size_t cursor, LuaCompletionContext& context) {
    cursor = std::min(cursor, text.size());
    // Dentro de un comentario o un texto no se completa.
    std::size_t line_start = text.rfind('\n', cursor == 0 ? 0 : cursor - 1);
    line_start = line_start == std::string::npos ? 0 : line_start + 1;
    const std::string line = text.substr(line_start, cursor - line_start);
    if (line.find("--") != std::string::npos) return false;
    if (std::count(line.begin(), line.end(), '"') % 2 == 1 || std::count(line.begin(), line.end(), '\'') % 2 == 1) {
        return false;
    }
    std::size_t start = cursor;
    while (start > 0 && identChar(text[start - 1])) --start;
    context = LuaCompletionContext{};
    context.prefix_start = start;
    context.prefix = text.substr(start, cursor - start);
    if (!context.prefix.empty() && std::isdigit(static_cast<unsigned char>(context.prefix[0]))) return false;
    if (start > 0 && (text[start - 1] == '.' || text[start - 1] == ':')) {
        context.accessor = text[start - 1];
        // Receptor: la cadena a.b.c antes del punto.
        std::size_t r = start - 1;
        while (r > 0 && (identChar(text[r - 1]) || text[r - 1] == '.')) --r;
        context.receiver = text.substr(r, start - 1 - r);
        // "function Clase:" -> metodos del motor.
        const std::string before = text.substr(line_start, r - line_start);
        std::string trimmed = before;
        while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) trimmed.pop_back();
        context.after_function = trimmed.size() >= 8 && trimmed.compare(trimmed.size() - 8, 8, "function") == 0;
        return true;
    }
    return !context.prefix.empty();
}

std::vector<LuaCompletion> luaCompletions(const std::string& text, const LuaCompletionContext& context) {
    List candidates;
    List locals;
    List self_fields;
    List self_methods;
    fileSymbols(text, locals, self_fields, self_methods);
    const auto append = [&](const List& list) { candidates.insert(candidates.end(), list.begin(), list.end()); };

    if (context.accessor != 0) {
        const std::string& r = context.receiver;
        const auto table = tables().find(r);
        if (context.after_function) {
            append(engineCallbacks());
        } else if (table != tables().end()) {
            append(table->second);
        } else if (r == "self") {
            append(context.accessor == ':' ? self_methods : self_fields);
            if (context.accessor == ':') append(engineCallbacks());
        } else {
            // Entidad (self.entity, other, lo que devuelve Scene.find...) o Vec3.
            const std::string low = lower(r);
            const bool looks_vector = low.find("pos") != std::string::npos || low.find("dir") != std::string::npos ||
                                      low.find("vel") != std::string::npos || low.find("vec") != std::string::npos ||
                                      low == "v" || low.find("point") != std::string::npos ||
                                      low.find("normal") != std::string::npos;
            const bool looks_mesh = low.find("mesh") != std::string::npos || low.find("malla") != std::string::npos;
            if (looks_mesh) {
                append(meshMembers(context.accessor));
            } else if (looks_vector) {
                append(vectorMembers(context.accessor));
            } else {
                append(context.accessor == ':' ? entityMethods() : entityProperties());
                append(vectorMembers(context.accessor));
            }
        }
    } else {
        append(locals);
        append(globals());
        append(keywords());
    }

    // Filtro: empieza igual (primero) o contiene el texto; sin repetir.
    const std::string needle = lower(context.prefix);
    List starts;
    List contains;
    std::set<std::string> seen;
    for (const LuaCompletion& c : candidates) {
        const std::string label = lower(c.label);
        if (label == needle && context.accessor == 0) continue;  // ya esta escrito entero
        if (!seen.insert(c.label).second) continue;
        if (label.rfind(needle, 0) == 0) {
            starts.push_back(c);
        } else if (needle.size() >= 2 && label.find(needle) != std::string::npos) {
            contains.push_back(c);
        }
    }
    const auto by_label = [](const LuaCompletion& a, const LuaCompletion& b) { return a.label < b.label; };
    if (context.accessor == 0) std::stable_sort(starts.begin(), starts.end(), by_label);
    starts.insert(starts.end(), contains.begin(), contains.end());
    return starts;
}

}  // namespace cramion::editor

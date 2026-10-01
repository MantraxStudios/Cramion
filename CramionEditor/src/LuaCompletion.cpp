#include "LuaCompletion.h"

#include <algorithm>
#include <cctype>
#include <map>
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
        LuaCompletion{"Fluid", "Fluid", "liquidos por particulas: spawn, density, emisores (agua, miel, lava...)", 1},
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
        LuaCompletion{"DataPack", "DataPack", "paquetes de escenas y objetos (como AssetBundles): load, loadScene, instantiate", 1},
        LuaCompletion{"Screen", "Screen", "tamano y orientacion de la pantalla", 1},
        LuaCompletion{"Weather", "Weather", "ambiente: clima (set \"Storm\"), hora, fecha, estacion, viento, rayos", 1},
        LuaCompletion{"Environment", "Environment", "lo mismo que Weather (clima, hora, viento)", 1},
        LuaCompletion{"Fire", "Fire", "incendios: ignite, extinguish, isBurning, burnedFraction", 1},
        LuaCompletion{"XR", "XR", "realidad virtual (OpenXR): cabeza, mandos, botones, vibracion", 1},
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
          fn("isCursorLocked", "", "esta capturado?"),
          fn("getAction", "\"Move\"", "valor de una accion: Bool -> true/false, Axis1D -> numero, Axis2D/3D -> Vec3"),
          fn("getActionValue", "\"Move\"", "Vec3 con el valor de la accion"),
          fn("getActionState", "\"Jump\"", "\"none\", \"ongoing\" o \"triggered\""),
          fn("isActionTriggered", "\"Fire\"", "disparada este frame (segun sus triggers)"),
          fn("wasActionStarted", "\"Jump\"", "empezo este frame"),
          fn("wasActionCompleted", "\"Jump\"", "termino este frame (se solto tras dispararse)"),
          fn("wasActionCanceled", "\"Jump\"", "se solto sin llegar a dispararse (Hold corto...)"),
          fn("getActionElapsed", "\"Fire\"", "segundos desde que empezo"),
          fn("bindAction", "\"Jump\", \"triggered\", function(valor, t) end", "llama a la funcion en ese evento; devuelve un id"),
          fn("unbindAction", "id", "quita un bindAction"),
          fn("addMappingContext", "\"Vuelo\", 1", "activa un contexto (prioridad opcional)"),
          fn("removeMappingContext", "\"Vuelo\"", "desactiva un contexto"),
          fn("hasMappingContext", "\"Vuelo\"", "esta activo?"),
          fn("getMappingContexts", "", "lista de contextos activos"),
          fn("getBindings", "\"Jump\"", "lista {context, key} de las teclas de una accion"),
          fn("rebind", "\"Default\", \"Jump\", 1, \"F\"", "cambia una tecla (1 = la primera de esa accion)"),
          fn("saveBindings", "", "guarda las teclas cambiadas (entre partidas)"),
          fn("resetBindings", "", "vuelve a las teclas del proyecto"),
          fn("anyKeyPressed", "", "tecla/boton pulsado este frame (\"W\", \"Gamepad A\") o nil")}},
        {"Scene",
         {fn("find", "\"nombre\"", "el primer objeto con ese nombre (o nil)"),
          fn("findWithTag", "\"tag\"", "el primer objeto con ese tag"),
          fn("findAllWithTag", "\"tag\"", "lista de objetos con ese tag"),
          fn("create", "\"nombre\", posicion", "objeto vacio nuevo"),
          fn("instantiate", "entity o \"Prefabs/Enemigo\", posicion, rotacion", "copia de un objeto (con hijos y componentes) o instancia de un prefab"),
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
          prop("upscaler", "off, taa, fsr1, fsr3 (AMD FSR 3.1, INESTABLE) o dlss (NVIDIA DLSS 4, INESTABLE)"),
          prop("upscaler_active", "el que escala de verdad (lectura)"), prop("fsr3_supported", "hay FSR 3.1 (lectura)"),
          prop("dlss_supported", "hay DLSS 4: GPU RTX (lectura)"), prop("resolution", "native, quality, balanced, performance, ultra_performance, custom"),
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
        {"Weather",
         {fn("set", "\"Storm\", 10", "cambia de clima en N segundos (Clear, Cloudy, Overcast, Foggy, LightRain, Rain, Storm, LightSnow, Snow, Blizzard, Sandstorm)"),
          fn("setWeather", "\"Rain\", 5", "lo mismo que set"), fn("get", "", "clima actual (\"Storm\")"),
          fn("getTarget", "", "clima al que va la transicion"), fn("getLabel", "", "nombre visible (\"Tormenta\")"),
          fn("presets", "", "lista de climas"), fn("transition", "", "0..1 lo que lleva la transicion"),
          fn("isTransitioning", "", "esta cambiando?"), fn("setRandom", "true, 120, 360", "clima al azar (segundos min y max)"),
          fn("setTime", "18.5", "hora del dia (0..24)"), fn("getTime", "", "hora del dia"),
          fn("setDate", "21, 12", "dia y mes (mueve el sol y la estacion)"), fn("getDate", "", "dia, mes"),
          fn("setLatitude", "40", "latitud en grados"), fn("getLatitude", "", "latitud"),
          fn("setDayLength", "24", "minutos reales por dia (nil = el tiempo se para)"),
          fn("setTimeScale", "60", "velocidad del tiempo (1 = real, 0 = parado)"), fn("getTimeScale", "", "velocidad del tiempo"),
          fn("setSeason", "\"Winter\"", "estacion fija (\"auto\" = por la fecha)"), fn("getSeason", "", "estacion actual"),
          fn("getTemperature", "", "grados C"), fn("setWind", "90, 1.5", "direccion (grados) y fuerza del viento"),
          fn("getWind", "", "Vec3 del viento (m/s)"), fn("getWindSpeed", "", "m/s con rachas"),
          fn("getWindDirection", "", "grados"), fn("getRain", "", "lluvia 0..1"), fn("getSnow", "", "nevada 0..1"),
          fn("getFog", "", "densidad de la niebla"), fn("getWetness", "", "humedad de las superficies 0..1"),
          fn("setWetness", "1, 0.6", "humedad y charcos al instante"), fn("getSnowCover", "", "nieve acumulada 0..1"),
          fn("setSnowCover", "1", "nieve acumulada al instante"), fn("setPrecipitationDensity", "0.5", "menos gotas (rendimiento)"),
          fn("lightning", "800", "un rayo ya (distancia en m; sin ella al azar)"),
          fn("setLightning", "true, 2", "rayos en las tormentas y su frecuencia"),
          fn("getSunDirection", "", "Vec3 hacia el sol"), fn("isNight", "", "el sol esta bajo el horizonte?"),
          fn("setAudio", "true, 0.8", "sonido de lluvia, viento y truenos")}},
        {"Audio", {fn("playOneShot", "\"Audio/golpe.wav\", posicion, volumen", "sonido suelto (sin posicion = 2D)"),
                   fn("setOcclusion", "true", "paredes tapan los sonidos (Audio Listener)"),
                   fn("occlusion", "", "esta la oclusion activa?"),
                   fn("setLowPass", "true, 800", "todo apagado (bajo el agua, pausa)"),
                   fn("reverbLevel", "", "reverberacion que se oye ahora (zonas)")}},
        {"Fire",
         {fn("ignite", "posicion, radio", "enciende fuego en las zonas Fuego que tocan el circulo (devuelve cuantas)"),
          fn("extinguish", "posicion, radio", "apaga el fuego en el circulo"), fn("extinguishAll", "", "apaga todo"),
          fn("reset", "", "vuelve a empezar: nada quemado (y se reenciende si 'Encender al empezar')"), fn("isBurning", "posicion", "hay llamas ahi?"),
          fn("heatAt", "posicion", "calor 0..1"), fn("burnedAt", "posicion", "quemado 0..1"),
          fn("charAt", "posicion", "lo mismo que burnedAt"), fn("burnedFraction", "", "0..1 de lo que podia arder"),
          fn("burningArea", "", "m2 en llamas"), fn("isActive", "", "hay algo ardiendo?"),
          fn("stats", "", "{burningCells, burnedFraction, burningArea, seconds...}")}},
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
        {"Fluid",
         {fn("spawn", "pos, cantidad, \"water\", vel, radio, vida", "crea liquido (bola de particulas)"),
          fn("clear", "", "borra todo el liquido"), fn("count", "\"honey\"", "particulas (todas o de un tipo)"),
          fn("density", "pos, radio", "0 = seco, ~1 = lleno"), fn("isInside", "pos", "hay liquido ahi?"),
          fn("velocity", "pos, radio", "velocidad media del liquido (Vec3)"),
          fn("surfaceHeight", "x, z", "nil o la altura de la superficie"),
          fn("start", "entidad", "el emisor empieza"), fn("stop", "entidad", "el emisor para"),
          fn("restart", "entidad", "vuelve a llenar una caja/esfera"), fn("setType", "entidad, \"lava\"", "cambia el liquido del emisor"),
          fn("types", "", "lista de tipos"), fn("stats", "", "{particles, capacity, emitters...}"),
          fn("isActive", "", "hay liquidos en la escena?")}},
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
        fn("detachFromBone", "", "deja de seguir al hueso"),
        fn("resetCloth", "", "su tela (Cloth) vuelve a la pose de reposo"),
        fn("addClothImpulse", "Vec3(0, 0, 3)", "empujon a su tela (m/s a cada particula libre)"),
        fn("resetSoftBody", "", "su cuerpo blando vuelve a su forma, quieto"),
        fn("addSoftBodyImpulse", "Vec3(0, 5, 0)", "empujon a su cuerpo blando (m/s a cada particula)")};
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
// Mas documentacion: lo que no estaba en las listas de arriba.
const std::unordered_map<std::string, List>& moreTables() {
    static const std::unordered_map<std::string, List> map = {
        {"XR",
         {fn("isAvailable", "", "hay casco y sesion de VR"), fn("isRunning", "", "el casco esta mostrando el juego"),
          fn("isFocused", "", "el juego tiene los mandos (sin el menu del sistema encima)"),
          fn("getSystemName", "", "nombre del casco"), fn("getRuntimeName", "", "runtime de OpenXR (SteamVR, Oculus...)"),
          fn("getHeadPosition", "", "Vec3 de la cabeza en el mundo (o nil)"),
          fn("getHeadRotation", "", "Quat de la cabeza en el mundo (o nil)"),
          fn("getHeadLocalPosition", "", "Vec3 de la cabeza dentro de la habitacion"),
          fn("isControllerActive", "\"right\"", "el mando esta encendido y se sigue"),
          fn("getControllerPosition", "\"right\", \"grip\"", "Vec3 de la mano (grip) o del puntero (aim), o nil"),
          fn("getControllerRotation", "\"right\", \"grip\"", "Quat de la mano o del puntero, o nil"),
          fn("getAimRay", "\"right\"", "origen, direccion del puntero (para Physics.raycast)"),
          fn("getTrigger", "\"right\"", "gatillo 0..1"), fn("getGrip", "\"right\"", "agarre 0..1"),
          fn("getThumbstick", "\"left\"", "Vec3(x, y, 0) del stick, -1..1"),
          fn("getButton", "\"right\", \"a\"", "boton mantenido: trigger, grip, thumbstick, primary (a/x), secondary (b/y), menu"),
          fn("getButtonDown", "\"right\", \"trigger\"", "boton pulsado este frame"),
          fn("getButtonUp", "\"right\", \"trigger\"", "boton soltado este frame"),
          fn("vibrate", "\"right\", 0.5, 0.1", "vibracion: intensidad 0..1, segundos, hz"),
          fn("setTrackingOrigin", "\"floor\"", "floor (de pie) o eyes (sentado); con XR Origin manda el componente"),
          fn("getTrackingOrigin", "", "\"floor\" o \"eyes\""),
          fn("getOriginPosition", "", "Vec3 del rig (XR Origin o la camara) en el mundo")}},
        {"Screen",
         {fn("width", "", "ancho en pixeles"), fn("height", "", "alto en pixeles"),
          fn("orientation", "", "\"landscape\" o \"portrait\" segun el tamano"),
          fn("setOrientation", "\"landscape\"", "auto, landscape, portrait, landscape_fixed, portrait_fixed (moviles)"),
          fn("orientationMode", "", "el ultimo modo pedido")}},
        {"Input",
         {fn("getGamepadButton", "\"a\"", "boton del mando mantenido: a, b, x, y, lb, rb, ls, rs, start, back, up, down, left, right"),
          fn("getGamepadButtonDown", "\"a\"", "pulsado este frame"), fn("getGamepadButtonUp", "\"a\"", "soltado este frame"),
          fn("getGamepadAxis", "\"leftx\"", "eje del mando: leftx, lefty, rightx, righty, lt, rt"),
          fn("isGamepadConnected", "", "hay un mando?"), fn("touchCount", "", "dedos en la pantalla"),
          fn("getTouch", "1", "{id, position, delta, start, phase} de un dedo (1..touchCount)"),
          fn("isMobile", "", "movil o pantalla tactil"), fn("vibrate", "60", "vibra el movil (ms)"),
          fn("setTouchControls", "true", "muestra u oculta los controles tactiles"),
          fn("touchControlsEnabled", "", "estan visibles?"), fn("setTouchJoystick", "true", "joystick tactil"),
          fn("setTouchLook", "true", "zona para mirar arrastrando"),
          fn("setTouchButton", "\"Saltar\", true", "muestra u oculta un boton tactil por su texto")}},
        {"Scene",
         {fn("findAll", "\"nombre\"", "todos los objetos con ese nombre"), fn("all", "", "todos los objetos de la escena")}},
        {"DataPack",
         {fn("load", "\"Nivel2\"", "monta un .datapack (junto al juego, en DataPacks/ o ruta); devuelve {name, scenes, objects, files} o nil"),
          fn("loadScene", "\"Nivel2\", \"escena\"", "monta el paquete y carga su escena (la primera si no se dice cual); true/false"),
          fn("instantiate", "\"Skins\", \"Coche\", posicion, rotacion", "monta el paquete y crea un objeto (prefab) suyo; devuelve la Entity o nil"),
          fn("unload", "\"Nivel2\"", "desmonta el paquete (borra lo que extrajo)"),
          fn("list", "", "nombres de los paquetes montados"),
          fn("isLoaded", "\"Nivel2\"", "esta montado?"),
          fn("info", "\"Nivel2\"", "{name, version, scenes, objects, files} sin montar nada (o nil)")}},
        {"Input",
         {fn("isActionOngoing", "\"Fire\"", "en curso (Hold todavia sin completar...)"),
          fn("getActions", "", "nombres de todas las acciones del proyecto"),
          fn("clearMappingContexts", "", "desactiva todos los contextos")}},
        {"Mathf", {prop("negativeInfinity", "-infinito")}},
    };
    return map;
}

const List& quatMembers(char accessor);

// La API que dio el motor (setLuaApiReference).
std::map<std::string, std::vector<LuaApiMember>>& apiReference() {
    static std::map<std::string, std::vector<LuaApiMember>> reference;
    return reference;
}
LuaProjectSymbols& project() {
    static LuaProjectSymbols symbols;
    return symbols;
}

// Miembros de un objeto sin documentar: los que da el motor.
void appendReference(List& to, const std::string& owner, char accessor) {
    const auto it = apiReference().find(owner);
    if (it == apiReference().end()) return;
    std::set<std::string> have;
    for (const LuaCompletion& c : to) have.insert(c.label);
    const bool object = !owner.empty() && owner.back() == ':';
    if (object) {
        // sol2 guarda en el objeto tambien las propiedades (como funciones) y
        // lo estatico del tipo (Vec3.up, Mesh.cube): esos no son metodos.
        const std::string type = owner.substr(0, owner.size() - 1);
        const List* documented[2] = {nullptr, nullptr};
        if (type == "Entity") documented[0] = &entityProperties(), documented[1] = &entityMethods();
        if (type == "Vec3") documented[0] = &vectorMembers('.'), documented[1] = &vectorMembers(':');
        if (type == "Quat") documented[0] = &quatMembers('.'), documented[1] = &quatMembers(':');
        if (type == "Mesh") documented[0] = &meshMembers('.'), documented[1] = &meshMembers(':');
        for (const List* list : documented) {
            if (list == nullptr) continue;
            for (const LuaCompletion& c : *list) have.insert(c.label);
        }
        if (const auto statics = apiReference().find(type); statics != apiReference().end()) {
            for (const LuaApiMember& m : statics->second) have.insert(m.name);
        }
        for (const char* name : {"new", "copy_from", "forward", "right", "up"}) {
            if (type != "Quat" || std::string_view(name) == "new") have.insert(name);
        }
    }
    for (const LuaApiMember& m : it->second) {
        if (have.contains(m.name)) continue;
        // Con ':' solo metodos; con '.' en un objeto, los campos (y en una tabla, todo).
        if (object && accessor == ':' && !m.function) continue;
        if (object && accessor == '.' && m.function) continue;
        to.push_back(m.function ? LuaCompletion{m.name, m.name + "(", m.name + "(...)  -  API del motor", 2}
                                : LuaCompletion{m.name, m.name, "API del motor", object ? 3 : 1});
    }
}

List tableMembers(const std::string& table) {
    List out;
    if (const auto it = tables().find(table); it != tables().end()) out = it->second;
    if (const auto it = moreTables().find(table); it != moreTables().end()) {
        std::set<std::string> have;
        for (const LuaCompletion& c : out) have.insert(c.label);
        for (const LuaCompletion& c : it->second) {
            if (!have.contains(c.label)) out.push_back(c);
        }
    }
    appendReference(out, table, '.');
    return out;
}

bool isGlobalTable(const std::string& name) {
    if (tables().contains(name) || moreTables().contains(name)) {
        return std::isupper(static_cast<unsigned char>(name[0])) || name == "math" || name == "string" || name == "table";
    }
    return apiReference().contains(name) && !name.empty() && name.back() != ':';
}

const List& quatMembers(char accessor) {
    static const List fields = {prop("x", ""), prop("y", ""), prop("z", ""), prop("w", "")};
    static const List methods = {fn("normalized", "", "de longitud 1"), fn("inverse", "", "el giro contrario"),
                                 fn("toEuler", "", "Vec3 en grados"), fn("dot", "otro", ""), fn("angle", "otro", "grados entre los dos"),
                                 fn("slerp", "otro, t", "interpola"), fn("lerp", "otro, t", "interpola (rapido)"),
                                 fn("rotateTowards", "otro, grados", "gira como mucho")};
    return accessor == ':' ? methods : fields;
}

const List& structFields(const std::string& type) {
    static const std::unordered_map<std::string, List> map = {
        {"hit", {prop("entity", "Entity que se toco"), prop("point", "Vec3 punto del choque"), prop("normal", "Vec3 normal"),
                 prop("distance", "metros")}},
        {"voxelhit", {prop("block", "Vec3 del bloque"), prop("normal", "Vec3 cara"), prop("id", "numero del bloque"),
                      prop("point", "Vec3 punto"), prop("distance", "metros")}},
        {"touch", {prop("id", "dedo"), prop("position", "Vec3 en pixeles"), prop("delta", "Vec3 movimiento"),
                   prop("start", "Vec3 donde empezo"), prop("phase", "began, moved, stationary, ended")}},
    };
    static const List empty;
    if (const auto it = map.find(type); it != map.end()) return it->second;
    if (const auto it = tables().find(type); it != tables().end()) return it->second;  // contact, res
    return empty;
}

// --- Tipos ---
// Lo que devuelve cada funcion ("Tabla.funcion" o "Tipo:metodo") o vale cada
// campo ("Tipo.campo"). "[Entity" = lista de entidades.
const std::unordered_map<std::string, std::string>& returnTypes() {
    static const std::unordered_map<std::string, std::string> map = [] {
        std::unordered_map<std::string, std::string> m;
        for (const char* f : {"Scene.find", "Scene.create", "Scene.instantiate", "Scene.findWithTag", "Network.spawn",
                              "Network.find", "Entity:find", "Entity.parent", "hit.entity"}) {
            m[f] = "Entity";
        }
        for (const char* f : {"Scene.findAllWithTag", "Scene.findAll", "Scene.all", "Network.objects"}) m[f] = "[Entity";
        for (const char* f : {"Vec3.zero", "Vec3.one", "Vec3.up", "Vec3.down", "Vec3.right", "Vec3.left", "Vec3.forward",
                              "Vec3.back", "Vec3.lerp", "Vec3.lerpUnclamped", "Vec3.slerp", "Vec3.moveTowards", "Vec3.cross",
                              "Vec3.project", "Vec3.projectOnPlane", "Vec3.reflect", "Vec3.min", "Vec3.max", "Vec3.scale",
                              "Vec3:normalized", "Vec3:clampLength", "Vec3:cross", "Vec3:lerp", "Vec3:moveTowards", "Vec3:abs",
                              "Vec3:floor", "Vec3:round", "Vec3:copy", "Quat:toEuler", "Input.mousePosition",
                              "Input.mouseDelta", "Input.getActionValue", "XR.getHeadPosition", "XR.getHeadLocalPosition",
                              "XR.getControllerPosition", "XR.getThumbstick", "XR.getOriginPosition",
                              "Navigation.projectPoint", "Navigation.randomPoint", "Random.onUnitSphere",
                              "Random.insideUnitSphere", "Random.insideUnitCircle", "Entity:getBonePosition", "Scene.toLocal",
                              "Entity.position", "Entity.localPosition", "Entity.rotation", "Entity.scale", "Entity.forward",
                              "Entity.right", "Entity.up", "Entity.velocity", "Entity.angularVelocity", "Entity.navVelocity",
                              "Entity.uiPosition", "Entity.uiSize", "Entity.color", "hit.point", "hit.normal",
                              "voxelhit.point", "voxelhit.normal", "voxelhit.block", "contact.point", "contact.normal",
                              "contact.relativeVelocity", "touch.position", "touch.delta", "touch.start", "Mesh.boundsMin",
                              "Mesh.boundsMax", "Mesh:getVertex"}) {
            m[f] = "Vec3";
        }
        for (const char* f : {"Quat.identity", "Quat.euler", "Quat.angleAxis", "Quat.lookRotation", "Quat.fromToRotation",
                              "Quat.slerp", "Quat.lerp", "Quat.rotateTowards", "Quat.inverse", "Quat:normalized",
                              "Quat:inverse", "Quat:slerp", "Quat:lerp", "Quat:rotateTowards", "Random.rotation",
                              "XR.getHeadRotation", "XR.getControllerRotation", "Entity:getBoneRotation", "Entity.quaternion"}) {
            m[f] = "Quat";
        }
        for (const char* f : {"Mesh.new", "Mesh.cube", "Mesh.quad", "Mesh.plane", "Mesh.sphere", "Mesh.cylinder",
                              "Mesh.capsule", "Mesh.wireCube", "Mesh:clone", "Entity.mesh"}) {
            m[f] = "Mesh";
        }
        m["Physics.raycast"] = "hit";
        m["Voxel.raycast"] = "voxelhit";
        m["Input.getTouch"] = "touch";
        return m;
    }();
    return map;
}

// Nombres de parametros habituales (callbacks del motor, funciones de Http...).
std::string parameterType(const std::string& name) {
    if (name == "other" || name == "entity" || name == "target" || name == "objetivo" || name == "jugador" ||
        name == "player" || name == "enemy" || name == "enemigo") {
        return "Entity";
    }
    if (name == "contact") return "contact";
    if (name == "sm") return "StateMachine";  // OnEnter(self, sm) de una maquina de estados
    if (name == "res" || name == "response") return "res";
    if (name == "hit") return "hit";
    if (name == "offset") return "Vec3";
    return {};
}

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

// Una cadena a.b:c(...).d partida en trozos.
struct Segment {
    char accessor = 0;  // 0 el primero, '.' o ':'
    std::string name;
    bool called = false;
};

// Lee una cadena desde el principio de `expr`; `end` = donde termina.
std::vector<Segment> parseChain(const std::string& expr, std::size_t* end = nullptr) {
    std::vector<Segment> out;
    std::size_t i = 0;
    while (i < expr.size() && std::isspace(static_cast<unsigned char>(expr[i]))) ++i;
    char accessor = 0;
    while (i < expr.size()) {
        std::size_t j = i;
        while (j < expr.size() && identChar(expr[j])) ++j;
        if (j == i) break;
        Segment s{accessor, expr.substr(i, j - i), false};
        i = j;
        // Argumentos (parentesis equilibrados; "Vec3 {..}" o f"texto" tambien).
        while (i < expr.size() && std::isspace(static_cast<unsigned char>(expr[i]))) ++i;
        if (i < expr.size() && (expr[i] == '(' || expr[i] == '{' || expr[i] == '"' || expr[i] == '\'')) {
            const char open = expr[i];
            if (open == '"' || open == '\'') {
                std::size_t k = i + 1;
                while (k < expr.size() && expr[k] != open) k += expr[k] == '\\' ? 2 : 1;
                i = std::min(k + 1, expr.size());
            } else {
                const char close = open == '(' ? ')' : '}';
                int depth = 0;
                char quote = 0;
                for (; i < expr.size(); ++i) {
                    const char c = expr[i];
                    if (quote != 0) {
                        if (c == '\\') ++i;
                        else if (c == quote) quote = 0;
                        continue;
                    }
                    if (c == '"' || c == '\'') quote = c;
                    else if (c == open) ++depth;
                    else if (c == close && --depth == 0) {
                        ++i;
                        break;
                    }
                }
            }
            s.called = true;
        }
        out.push_back(s);
        // [indice] -> sigue igual (no cambia el tipo que sepamos).
        while (i < expr.size() && expr[i] == '[') {
            int depth = 0;
            for (; i < expr.size(); ++i) {
                if (expr[i] == '[') ++depth;
                if (expr[i] == ']' && --depth == 0) {
                    ++i;
                    break;
                }
            }
            out.back().name += "[]";
        }
        if (i < expr.size() && (expr[i] == '.' || expr[i] == ':')) {
            accessor = expr[i];
            ++i;
            continue;
        }
        break;
    }
    if (end != nullptr) *end = i;
    return out;
}

// Asignaciones del archivo: variable -> expresion (la ultima gana) y los
// campos de self.
struct FileTypes {
    std::unordered_map<std::string, std::string> vars;        // nombre -> expresion
    std::unordered_map<std::string, std::string> self_fields;  // campo -> expresion
    std::unordered_map<std::string, std::string> loop_lists;   // variable de un for -> expresion de la lista
};

FileTypes scanTypes(const std::string& text) {
    FileTypes t;
    static const std::regex local_re(R"((?:^|[\s;])(?:local\s+)?([A-Za-z_]\w*)\s*=\s*([^=\n][^\n]*))");
    static const std::regex self_re(R"(self\.([A-Za-z_]\w*)\s*=\s*([^=\n][^\n]*))");
    static const std::regex for_re(R"(for\s+[A-Za-z_]\w*\s*,\s*([A-Za-z_]\w*)\s+in\s+i?pairs\s*\(([^\n]*)\)\s*do)");
    for (std::sregex_iterator it(text.begin(), text.end(), local_re), end; it != end; ++it) {
        t.vars[(*it)[1]] = (*it)[2];
    }
    for (std::sregex_iterator it(text.begin(), text.end(), self_re), end; it != end; ++it) {
        t.self_fields[(*it)[1]] = (*it)[2];
    }
    for (std::sregex_iterator it(text.begin(), text.end(), for_re), end; it != end; ++it) {
        t.loop_lists[(*it)[1]] = (*it)[2];
    }
    // properties = { velocidad = 5, objetivo = Vec3(...) }: tambien son de self.
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
            static const std::regex prop_re(R"(([A-Za-z_]\w*)\s*=\s*([^,\n}]*))");
            for (std::sregex_iterator it(body.begin(), body.end(), prop_re), end; it != end; ++it) {
                t.self_fields.emplace((*it)[1], (*it)[2]);
            }
        }
    }
    return t;
}

std::string typeOf(const std::string& expr, const FileTypes& file, int depth);

// Tipo de lo que devuelve un trozo sobre un tipo conocido.
std::string stepType(const std::string& type, const Segment& s) {
    if (type.empty()) return {};
    if (type[0] == '@') {  // tabla global
        const std::string table = type.substr(1);
        if (table == "Vec3" && s.called && s.accessor == 0) return "Vec3";
        const auto it = returnTypes().find(table + "." + s.name);
        return it != returnTypes().end() ? it->second : std::string();
    }
    if (type[0] == '[') return s.name.ends_with("[]") ? type.substr(1) : std::string();
    const std::string key = type + (s.accessor == ':' ? ":" : ".") + s.name;
    if (const auto it = returnTypes().find(key); it != returnTypes().end()) return it->second;
    return {};
}

std::string typeOfChain(const std::vector<Segment>& chain, const FileTypes& file, int depth) {
    if (chain.empty() || depth > 6) return {};
    const Segment& first = chain[0];
    std::string type;
    std::size_t next = 1;
    std::string name = first.name;
    bool indexed = false;
    if (name.ends_with("[]")) {
        name = name.substr(0, name.find('['));
        indexed = true;
    }
    if (name == "Vec3" && first.called) {
        type = "Vec3";
    } else if (name == "Quat" && first.called) {
        type = "Quat";
    } else if (name == "self") {
        if (chain.size() >= 2 && chain[1].accessor == '.') {
            next = 2;
            if (chain[1].name == "entity") {
                type = "Entity";
            } else if (chain[1].name == "sm") {
                type = "StateMachine";
            } else if (const auto it = file.self_fields.find(chain[1].name); it != file.self_fields.end()) {
                type = typeOf(it->second, file, depth + 1);
            }
        } else {
            type = "self";
        }
    } else if (isGlobalTable(name)) {
        type = "@" + name;
    } else if (const auto it = file.vars.find(name); it != file.vars.end()) {
        type = typeOf(it->second, file, depth + 1);
        if (type.empty()) type = parameterType(name);
    } else if (const auto loop = file.loop_lists.find(name); loop != file.loop_lists.end()) {
        const std::string list = typeOf(loop->second, file, depth + 1);
        type = !list.empty() && list[0] == '[' ? list.substr(1) : std::string();
    } else {
        type = parameterType(name);
    }
    if (indexed) type = !type.empty() && type[0] == '[' ? type.substr(1) : std::string();
    for (std::size_t i = next; i < chain.size() && !type.empty(); ++i) {
        Segment s = chain[i];
        const bool idx = s.name.ends_with("[]");
        if (idx) s.name = s.name.substr(0, s.name.find('['));
        type = stepType(type, s);
        if (idx) type = !type.empty() && type[0] == '[' ? type.substr(1) : std::string();
    }
    return type;
}

std::string typeOf(const std::string& raw, const FileTypes& file, int depth) {
    std::string expr = trim(raw);
    // "a or b": el de a.
    if (const std::size_t sep = expr.find(" or "); sep != std::string::npos) expr = expr.substr(0, sep);
    if (expr.rfind("not ", 0) == 0) return {};
    std::size_t end = 0;
    const std::vector<Segment> chain = parseChain(expr, &end);
    std::string type = typeOfChain(chain, file, depth);
    // "a + b", "v * 2": con un Vec3 delante (o un numero por un Vec3) sale Vec3.
    const std::string rest = trim(expr.substr(std::min(end, expr.size())));
    if (!rest.empty() && (rest[0] == '+' || rest[0] == '-' || rest[0] == '*' || rest[0] == '/')) {
        if (type == "Vec3") return "Vec3";
        if (type == "Quat" && rest[0] == '*') {
            const std::string right = typeOf(rest.substr(1), file, depth + 1);
            return right == "Vec3" ? "Vec3" : "Quat";
        }
        const std::string right = typeOf(rest.substr(1), file, depth + 1);
        return right == "Vec3" ? "Vec3" : std::string();
    }
    if (!rest.empty() && rest[0] != ')' && rest[0] != ',' && rest[0] != ';' && rest.rfind("--", 0) != 0) return {};
    if (chain.empty()) {
        if (!expr.empty() && (expr[0] == '"' || expr[0] == '\'')) return "string";
    }
    return type;
}

// Miembros de un tipo con '.' o ':'.
List typeMembers(const std::string& type, char accessor, const List& self_fields, const List& self_methods) {
    List out;
    const auto append = [&](const List& list) { out.insert(out.end(), list.begin(), list.end()); };
    if (type == "self") {
        append(accessor == ':' ? self_methods : self_fields);
        if (accessor == ':') append(engineCallbacks());
    } else if (type == "Entity") {
        append(accessor == ':' ? entityMethods() : entityProperties());
        appendReference(out, "Entity:", accessor);
    } else if (type == "Vec3") {
        append(vectorMembers(accessor));
        appendReference(out, "Vec3:", accessor);
    } else if (type == "Quat") {
        append(quatMembers(accessor));
        appendReference(out, "Quat:", accessor);
    } else if (type == "Mesh") {
        append(meshMembers(accessor));
        appendReference(out, "Mesh:", accessor);
    } else if (type == "StateMachine") {
        appendReference(out, "StateMachine:", accessor);
    } else if (!type.empty() && type[0] == '@') {
        out = tableMembers(type.substr(1));
    } else if (accessor == '.') {
        append(structFields(type));
    }
    return out;
}

// --- Textos: lo del proyecto segun la funcion y el argumento ---
List values(const std::vector<std::string>& names, const char* detail) {
    List out;
    out.reserve(names.size());
    for (const std::string& n : names) out.push_back(LuaCompletion{n, n, detail, 6});
    return out;
}

List stringValues(const LuaCompletionContext& c, const std::string& text) {
    const LuaProjectSymbols& p = project();
    const std::string& f = c.call;
    const int a = c.argument;
    const auto is = [&](std::initializer_list<const char*> names) {
        for (const char* n : names) {
            if (f == n) return true;
        }
        return false;
    };
    static const std::vector<std::string> kAxes = {"Horizontal", "Vertical", "Mouse X", "Mouse Y"};
    static const std::vector<std::string> kHands = {"left", "right"};
    static const std::vector<std::string> kXrButtons = {"trigger", "grip", "thumbstick", "primary", "secondary", "menu",
                                                        "a", "b", "x", "y"};
    static const std::vector<std::string> kPadButtons = {"a", "b", "x", "y", "lb", "rb", "ls", "rs", "start", "back",
                                                         "up", "down", "left", "right"};
    static const std::vector<std::string> kPadAxes = {"leftx", "lefty", "rightx", "righty", "lt", "rt"};
    static const std::vector<std::string> kEvents = {"started", "ongoing", "triggered", "completed", "canceled"};
    static const std::vector<std::string> kForces = {"force", "impulse", "acceleration", "velocity"};
    static const std::vector<std::string> kEffects = {"lowpass", "highpass", "echo", "reverb", "occlusion"};
    static const std::vector<std::string> kOrientations = {"auto", "landscape", "portrait", "landscape_fixed", "portrait_fixed"};
    static const std::vector<std::string> kQualities = {"Baja", "Media", "Alta", "Ultra"};

    if (is({"Input.getKey", "Input.getKeyDown", "Input.getKeyUp"})) return values(p.keys, "tecla");
    if (f == "Input.getAxis") {
        List out = values(kAxes, "eje");
        const List more = values(p.actions, "accion (Input Actions)");
        out.insert(out.end(), more.begin(), more.end());
        return out;
    }
    if (is({"Input.getAction", "Input.getActionValue", "Input.getActionState", "Input.isActionTriggered",
            "Input.wasActionStarted", "Input.wasActionCompleted", "Input.wasActionCanceled", "Input.getActionElapsed",
            "Input.getBindings"}) ||
        (f == "Input.bindAction" && a == 0) || (f == "Input.rebind" && a == 1)) {
        return values(p.actions, "accion (Archivo > Entrada del proyecto)");
    }
    if (f == "Input.bindAction" && a == 1) return values(kEvents, "evento");
    if (is({"Input.addMappingContext", "Input.removeMappingContext", "Input.hasMappingContext"}) ||
        (f == "Input.rebind" && a == 0)) {
        return values(p.contexts, "contexto de entrada");
    }
    if (f == "Input.rebind" && a == 3) return values(p.input_sources, "tecla, boton o eje");
    if (is({"Input.getGamepadButton", "Input.getGamepadButtonDown", "Input.getGamepadButtonUp"})) {
        return values(kPadButtons, "boton del mando");
    }
    if (f == "Input.getGamepadAxis") return values(kPadAxes, "eje del mando");
    if (is({"Scene.find", "Scene.findAll", "Entity:find"})) return values(p.entities, "objeto de la escena");
    if (is({"Scene.findWithTag", "Scene.findAllWithTag", "Entity:compareTag"})) return values(p.tags, "tag");
    if (is({"Scene.load", "Network.loadScene"})) return values(p.scenes, "escena");
    if ((f == "Scene.instantiate" && a == 0) || (f == "Network.spawn" && a == 0)) return values(p.prefabs, "prefab");
    if (f == "Audio.playOneShot" && a == 0) return values(p.audio, "sonido");
    if (is({"Entity:addComponent", "Entity:removeComponent", "Entity:hasComponent", "Entity:getFields"}) ||
        (is({"Entity:getField", "Entity:setField"}) && a == 0)) {
        List out;
        for (const LuaProjectSymbols::Component& comp : p.components) {
            out.push_back(LuaCompletion{comp.name, comp.name, comp.label, 6});
        }
        return out;
    }
    if (is({"Entity:getField", "Entity:setField"}) && a == 1) {
        List out;
        for (const LuaProjectSymbols::Component& comp : p.components) {
            if (comp.name != c.first_argument) continue;
            for (const auto& [key, detail] : comp.fields) out.push_back(LuaCompletion{key, key, detail, 6});
        }
        return out;
    }
    if (f == "Entity:setMaterial" && a == 1) return values(p.materials, "material");
    if (f == "Entity:addForce" && a == 1) return values(kForces, "modo de la fuerza");
    if (f == "Entity:setSoundEffect" && a == 0) return values(kEffects, "efecto de sonido");
    if (f.rfind("XR.", 0) == 0) {
        if (a == 0 && f != "XR.setTrackingOrigin") return values(kHands, "mano");
        if (a == 0) return values({"floor", "eyes"}, "origen del seguimiento");
        if (a == 1 && (f == "XR.getButton" || f == "XR.getButtonDown" || f == "XR.getButtonUp")) {
            return values(kXrButtons, "boton del mando VR");
        }
        if (a == 1 && (f == "XR.getControllerPosition" || f == "XR.getControllerRotation")) {
            return values({"grip", "aim"}, "grip = la mano, aim = el puntero");
        }
    }
    if (f == "Screen.setOrientation") return values(kOrientations, "orientacion");
    if (f == "Graphics.setQuality") return values(kQualities, "calidad");
    if ((f == "Graphics.set" || f == "Graphics.get") && a == 0) {
        List out;
        for (const LuaCompletion& item : tables().at("Graphics")) {
            if (item.kind == 3 && item.label != "post") out.push_back(LuaCompletion{item.label, item.label, item.detail, 6});
        }
        return out;
    }
    // Claves de Prefs y mensajes de red: las que ya usa el archivo.
    const auto used = [&](const char* pattern, const char* detail) {
        std::set<std::string> found;
        const std::regex re(pattern);
        for (std::sregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it) found.insert((*it)[1]);
        return values(std::vector<std::string>(found.begin(), found.end()), detail);
    };
    if (f.rfind("Prefs.", 0) == 0 && a == 0) return used(R"(Prefs\.\w+\(\s*["']([^"']+)["'])", "clave usada en el script");
    if (is({"Network.send", "Network.on", "Network.off"}) && a == 0) {
        return used(R"(Network\.(?:send|on|off)\(\s*["']([^"']+)["'])", "mensaje usado en el script");
    }
    return {};
}

// Receptor antes de un '.' o ':' en `end`: cadena con llamadas y corchetes.
std::size_t receiverStart(const std::string& text, std::size_t end) {
    std::size_t r = end;
    while (r > 0) {
        const char c = text[r - 1];
        if (identChar(c) || c == '.' || c == ':') {
            --r;
            continue;
        }
        if (c == ')' || c == ']') {
            const char open = c == ')' ? '(' : '[';
            int depth = 0;
            std::size_t k = r;
            while (k > 0) {
                --k;
                if (text[k] == c) ++depth;
                else if (text[k] == open && --depth == 0) break;
            }
            if (depth != 0) break;
            r = k;
            continue;
        }
        break;
    }
    // Un ':' suelto delante (a::b no es Lua) o un punto inicial no cuentan.
    while (r < end && (text[r] == '.' || text[r] == ':')) ++r;
    return r;
}

// La llamada que contiene `pos` (el '(' sin cerrar mas cercano): donde esta
// y en que argumento va.
bool enclosingCall(const std::string& text, std::size_t pos, std::size_t& open, int& argument) {
    int depth = 0;
    argument = 0;
    std::size_t line_breaks = 0;
    for (std::size_t i = pos; i > 0; --i) {
        const char c = text[i - 1];
        if (c == '\n' && ++line_breaks > 8) return false;
        if (c == '"' || c == '\'') {  // salta el texto hacia atras
            std::size_t k = i - 1;
            while (k > 0 && !(text[k - 1] == c && (k < 2 || text[k - 2] != '\\'))) --k;
            if (k == 0) return false;
            i = k;
            continue;
        }
        if (c == ')' || c == '}' || c == ']') ++depth;
        else if (c == '(' || c == '{' || c == '[') {
            if (depth == 0) {
                if (c != '(') return false;
                open = i - 1;
                return true;
            }
            --depth;
        } else if (c == ',' && depth == 0) {
            ++argument;
        }
    }
    return false;
}

// "Input.getKey" / "Entity:getField" de la llamada que abre en `open`.
std::string calleeName(const std::string& text, std::size_t open, const FileTypes& file) {
    std::size_t end = open;
    while (end > 0 && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    const std::size_t start = receiverStart(text, end);
    const std::string chain_text = text.substr(start, end - start);
    std::vector<Segment> chain = parseChain(chain_text);
    if (chain.empty()) return {};
    const Segment last = chain.back();
    if (chain.size() == 1) return last.name;
    chain.pop_back();
    std::string type = typeOfChain(chain, file, 0);
    if (!type.empty() && type[0] == '@') return type.substr(1) + "." + last.name;
    // Metodo de un objeto que no se sabe que es: si es de entidad, se toma como entidad.
    if (type.empty() && last.accessor == ':') type = "Entity";
    if (type.empty() && chain.size() == 1 && chain[0].name == "Vec3") type = "@Vec3";
    return type.empty() ? std::string() : type + (last.accessor == ':' ? ":" : ".") + last.name;
}

const LuaCompletion* findDoc(const std::string& callee) {
    const std::size_t sep = callee.find_first_of(".:");
    if (sep == std::string::npos) {
        for (const LuaCompletion& c : globals()) {
            if (c.label == callee) return &c;
        }
        return nullptr;
    }
    const std::string owner = callee.substr(0, sep);
    const std::string name = callee.substr(sep + 1);
    const auto search = [&](const List& list) -> const LuaCompletion* {
        for (const LuaCompletion& c : list) {
            if (c.label == name) return &c;
        }
        return nullptr;
    };
    if (owner == "Entity") return search(callee[sep] == ':' ? entityMethods() : entityProperties());
    if (owner == "Vec3" && callee[sep] == ':') return search(vectorMembers(':'));
    if (owner == "Quat" && callee[sep] == ':') return search(quatMembers(':'));
    if (owner == "Mesh" && callee[sep] == ':') return search(meshMembers(':'));
    if (const auto it = tables().find(owner); it != tables().end()) {
        if (const LuaCompletion* c = search(it->second)) return c;
    }
    if (const auto it = moreTables().find(owner); it != moreTables().end()) return search(it->second);
    return nullptr;
}

}  // namespace

void setLuaApiReference(const std::map<std::string, std::vector<LuaApiMember>>& reference) { apiReference() = reference; }

void setLuaProjectSymbols(LuaProjectSymbols symbols) { project() = std::move(symbols); }

bool luaIsApiName(std::string_view word) {
    if (word.empty()) return false;
    const std::string w(word);
    if (w == "self" || tables().contains(w) || moreTables().contains(w)) return true;
    for (const LuaCompletion& c : globals()) {
        if (c.label == w) return true;
    }
    const auto it = apiReference().find("");
    if (it == apiReference().end()) return false;
    return std::any_of(it->second.begin(), it->second.end(), [&](const LuaApiMember& m) { return m.name == w; });
}

bool luaCompletionContext(const std::string& text, std::size_t cursor, LuaCompletionContext& context) {
    cursor = std::min(cursor, text.size());
    context = LuaCompletionContext{};
    std::size_t line_start = text.rfind('\n', cursor == 0 ? 0 : cursor - 1);
    line_start = line_start == std::string::npos ? 0 : line_start + 1;
    if (cursor == 0) line_start = 0;
    const std::string line = text.substr(line_start, cursor - line_start);
    // Comentario: nada. (Un "--" dentro de un texto no cuenta.)
    char quote = 0;
    std::size_t quote_at = 0;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quote != 0) {
            if (c == '\\') ++i;
            else if (c == quote) quote = 0;
            continue;
        }
        if (c == '-' && i + 1 < line.size() && line[i + 1] == '-') return false;
        if (c == '"' || c == '\'') {
            quote = c;
            quote_at = i;
        }
    }
    if (quote != 0) {
        // Dentro de un texto: solo si es argumento de una llamada que sabemos completar.
        const std::size_t string_start = line_start + quote_at;
        std::size_t open = 0;
        int argument = 0;
        if (!enclosingCall(text, string_start, open, argument)) return false;
        const FileTypes file = scanTypes(text);
        context.call = calleeName(text, open, file);
        if (context.call.empty()) return false;
        context.in_string = true;
        context.argument = argument;
        context.prefix_start = string_start + 1;
        context.prefix = text.substr(context.prefix_start, cursor - context.prefix_start);
        // El primer argumento (para getField: el componente).
        std::size_t i = open + 1;
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        if (i < text.size() && (text[i] == '"' || text[i] == '\'')) {
            const std::size_t close = text.find(text[i], i + 1);
            if (close != std::string::npos) context.first_argument = text.substr(i + 1, close - i - 1);
        }
        return true;
    }
    std::size_t start = cursor;
    while (start > 0 && identChar(text[start - 1])) --start;
    context.prefix_start = start;
    context.prefix = text.substr(start, cursor - start);
    if (!context.prefix.empty() && std::isdigit(static_cast<unsigned char>(context.prefix[0]))) return false;
    if (start > 0 && (text[start - 1] == '.' || text[start - 1] == ':')) {
        // ".." (concatenar) no es un acceso.
        if (text[start - 1] == '.' && start > 1 && text[start - 2] == '.') return !context.prefix.empty();
        context.accessor = text[start - 1];
        const std::size_t r = receiverStart(text, start - 1);
        context.receiver = text.substr(r, start - 1 - r);
        if (context.receiver.empty()) return false;
        // "function Clase:" -> metodos del motor.
        std::string before = text.substr(line_start, r - line_start);
        while (!before.empty() && std::isspace(static_cast<unsigned char>(before.back()))) before.pop_back();
        context.after_function = before.size() >= 8 && before.compare(before.size() - 8, 8, "function") == 0;
        return true;
    }
    return !context.prefix.empty();
}

std::vector<LuaCompletion> luaCompletions(const std::string& text, const LuaCompletionContext& context) {
    List candidates;
    List locals;
    List self_fields;
    List self_methods;
    const auto append = [&](const List& list) { candidates.insert(candidates.end(), list.begin(), list.end()); };

    if (context.in_string) {
        append(stringValues(context, text));
    } else {
        fileSymbols(text, locals, self_fields, self_methods);
        if (context.accessor != 0) {
            const FileTypes file = scanTypes(text);
            const std::string type = typeOf(context.receiver, file, 0);
            if (context.after_function) {
                append(engineCallbacks());
            } else if (!type.empty()) {
                append(typeMembers(type, context.accessor, self_fields, self_methods));
            } else {
                // No se sabe que es: por el nombre (posiciones, mallas) o una entidad.
                const std::string low = lower(context.receiver);
                const bool looks_vector = low.find("pos") != std::string::npos || low.find("dir") != std::string::npos ||
                                          low.find("vel") != std::string::npos || low.find("vec") != std::string::npos ||
                                          low == "v" || low.find("point") != std::string::npos ||
                                          low.find("normal") != std::string::npos;
                const bool looks_mesh = low.find("mesh") != std::string::npos || low.find("malla") != std::string::npos;
                if (looks_mesh) {
                    append(typeMembers("Mesh", context.accessor, self_fields, self_methods));
                } else if (looks_vector) {
                    append(typeMembers("Vec3", context.accessor, self_fields, self_methods));
                } else {
                    append(typeMembers("Entity", context.accessor, self_fields, self_methods));
                    append(vectorMembers(context.accessor));
                }
            }
        } else {
            append(locals);
            append(globals());
            // Las tablas y funciones globales del motor sin documentar aqui.
            List more;
            for (const auto& [name, list] : moreTables()) {
                (void)list;
                more.push_back(LuaCompletion{name, name, "tabla del motor", 1});
            }
            appendReference(more, "", '.');
            append(more);
            append(keywords());
        }
    }

    // Filtro: empieza igual (primero) o contiene el texto; sin repetir.
    const std::string needle = lower(context.prefix);
    List starts;
    List contains;
    std::set<std::string> seen;
    for (const LuaCompletion& c : candidates) {
        const std::string label = lower(c.label);
        if (label == needle && context.accessor == 0 && !context.in_string) continue;  // ya esta escrito entero
        if (!seen.insert(c.label).second) continue;
        if (label.rfind(needle, 0) == 0) {
            starts.push_back(c);
        } else if (!needle.empty() && label.find(needle) != std::string::npos) {
            contains.push_back(c);
        }
    }
    const auto by_label = [](const LuaCompletion& a, const LuaCompletion& b) { return a.label < b.label; };
    if (context.accessor == 0 && !context.in_string) std::stable_sort(starts.begin(), starts.end(), by_label);
    starts.insert(starts.end(), contains.begin(), contains.end());
    return starts;
}

bool luaSignatureAt(const std::string& text, std::size_t cursor, LuaSignature& signature) {
    cursor = std::min(cursor, text.size());
    std::size_t open = 0;
    int argument = 0;
    if (!enclosingCall(text, cursor, open, argument)) return false;
    const FileTypes file = scanTypes(text);
    const std::string callee = calleeName(text, open, file);
    if (callee.empty()) return false;
    const LuaCompletion* doc = findDoc(callee);
    if (doc == nullptr || doc->kind != 2) return false;
    const std::size_t sep = doc->detail.find("  -  ");
    signature.label = callee.substr(0, callee.find_last_of(".:") + 1) + doc->detail.substr(0, sep);
    signature.detail = sep == std::string::npos ? std::string() : doc->detail.substr(sep + 5);
    signature.argument = argument;
    return true;
}

std::string luaExpressionType(const std::string& text, const std::string& expression) {
    return typeOf(expression, scanTypes(text), 0);
}

}  // namespace cramion::editor

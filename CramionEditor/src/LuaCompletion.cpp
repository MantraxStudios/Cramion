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
        {"Time",
         {prop("deltaTime", "segundos desde el frame anterior"), prop("time", "segundos desde el Play"),
          prop("frameCount", "frames desde el Play"), prop("fixedDeltaTime", "paso fijo de la fisica")}},
        {"Physics", {fn("raycast", "origen, direccion, distancia", "nil o {entity, point, normal, distance}")}},
        {"Audio", {fn("playOneShot", "\"Audio/golpe.wav\", posicion, volumen", "sonido suelto (sin posicion = 2D)")}},
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
          fn("blockId", "\"stone\"", "numero de un bloque"), fn("blockColor", "\"stone\"", "color medio del bloque (Vec3)"), fn("raycast", "origen, direccion, distancia", "nil o {block, normal, id, point, distance}"),
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
        prop("uiSize", "Vec3 tamano de su RectTransform")};
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
        fn("isPlayingSound", "", "suena?"), fn("playAnimation", "\"Correr\", true", "clip del Animator"),
        fn("setAnimatorFloat", "\"velocidad\", 1.0", "parametro del Animator Controller"),
        fn("setAnimatorBool", "\"saltando\", true", ""), fn("setAnimatorTrigger", "\"atacar\"", ""),
        fn("hasComponent", "\"Rigidbody\"", "tiene ese componente?"), fn("getScript", "", "la instancia de su script"),
        fn("find", "\"hijo\"", "un hijo por nombre"), fn("valid", "", "sigue existiendo?"),
        fn("moveTo", "Vec3", "su NavAgent camina hasta alli por la malla"), fn("stopMoving", "", "se para"),
        fn("addComponent", "\"MeshCollider\"", "anade un componente por su nombre"),
        fn("removeComponent", "\"MeshCollider\"", "quita un componente"),
        fn("setMaterial", "0, \"Materials/Brillo.crmat\"", "material .crmat de un hueco de su MeshRenderer")};
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
        LuaCompletion{"OnDestroy", "OnDestroy()", "al destruirse o parar el Play", 5}};
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
            if (looks_vector) {
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

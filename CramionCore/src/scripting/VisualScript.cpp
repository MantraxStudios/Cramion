#include "CramionCore/scripting/VisualScript.h"

// La definicion de ComponentRegistry::registerComponent<T>.
#include "CramionCore/ecs/World.h"
#include "CramionCore/ecs/Reflection.h"
#include "CramionCore/scripting/NativeApi.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace cramion::vscript {

using nlohmann::json;

namespace {

constexpr const char* kPinKeys[] = {"exec", "any", "bool", "int", "float", "string", "vec3", "entity"};
constexpr const char* kPinLabels[] = {"Ejecucion", "Cualquiera", "Booleano", "Entero", "Decimal", "Texto", "Vector", "Objeto"};

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s) {
    const std::size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const std::size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool parseNumber(const std::string& text, double& out) {
    const std::string t = trim(text);
    if (t.empty()) return false;
    char* end = nullptr;
    out = std::strtod(t.c_str(), &end);
    return end != nullptr && *end == '\0' && std::isfinite(out);
}

std::string formatNumber(double n) {
    char buffer[64];
    if (std::floor(n) == n && std::fabs(n) < 1e15) {
        std::snprintf(buffer, sizeof(buffer), "%.0f", n);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.9g", n);
    }
    return buffer;
}

// "1 2 3", "1, 2, 3", "Vec3(1, 2, 3)" -> los numeros que haya (como mucho 3).
std::vector<double> numbersIn(const std::string& text) {
    std::vector<double> out;
    const char* p = text.c_str();
    while (*p != '\0' && out.size() < 3) {
        if (std::isdigit(static_cast<unsigned char>(*p)) || ((*p == '-' || *p == '+' || *p == '.') && p[1] != '\0')) {
            char* end = nullptr;
            const double v = std::strtod(p, &end);
            if (end != p) {
                out.push_back(v);
                p = end;
                continue;
            }
        }
        ++p;
    }
    return out;
}

bool isIdentifier(const std::string& s) {
    if (s.empty() || std::isdigit(static_cast<unsigned char>(s[0]))) return false;
    return std::all_of(s.begin(), s.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
}

// --- Catalogo ---
Pin X(const char* name) { return Pin{name, PinType::Exec, {}}; }
Pin P(const char* name, PinType type, const char* value = "") { return Pin{name, type, value}; }
// Un pin con un desplegable de valores (no se guardan).
Pin O(Pin pin, std::vector<std::string> options) {
    pin.options = std::move(options);
    return pin;
}

constexpr PinType kAny = PinType::Any;
constexpr PinType kBool = PinType::Bool;
constexpr PinType kInt = PinType::Int;
constexpr PinType kFloat = PinType::Float;
constexpr PinType kString = PinType::String;
constexpr PinType kVector = PinType::Vector;
constexpr PinType kEntity = PinType::Entity;

std::vector<NodeInfo> buildCatalog() {
    std::vector<NodeInfo> c;
    const auto event = [&](const char* kind, const char* title, const char* description, const char* keywords,
                           std::vector<Pin> outputs, std::vector<Pin> inputs = {}) {
        NodeInfo n;
        n.kind = kind;
        n.title = title;
        n.category = "Eventos";
        n.description = description;
        n.keywords = keywords;
        n.event = true;
        n.inputs = std::move(inputs);
        n.outputs = std::move(outputs);
        c.push_back(std::move(n));
    };
    const auto node = [&](const char* category, const char* kind, const char* title, const char* description,
                          const char* keywords, std::vector<Pin> inputs, std::vector<Pin> outputs, bool latent = false) {
        NodeInfo n;
        n.kind = kind;
        n.title = title;
        n.category = category;
        n.description = description;
        n.keywords = keywords;
        n.latent = latent;
        n.inputs = std::move(inputs);
        n.outputs = std::move(outputs);
        c.push_back(std::move(n));
    };
    const auto pure = [&](const char* category, const char* kind, const char* title, const char* description,
                          const char* keywords, std::vector<Pin> inputs, std::vector<Pin> outputs) {
        NodeInfo n;
        n.kind = kind;
        n.title = title;
        n.category = category;
        n.description = description;
        n.keywords = keywords;
        n.pure = true;
        n.inputs = std::move(inputs);
        n.outputs = std::move(outputs);
        c.push_back(std::move(n));
    };

    // --- Eventos ---
    event("event.start", "Event Start", "Una vez, al empezar (antes del primer Update)", "begin play inicio empezar start", {X("")});
    event("event.update", "Event Update", "Cada frame", "tick frame update", {X(""), P("Delta", kFloat)});
    event("event.late_update", "Event Late Update", "Cada frame, despues de todos los Update", "late tick", {X(""), P("Delta", kFloat)});
    event("event.fixed_update", "Event Fixed Update", "Cada paso de la fisica", "fixed fisica physics", {X(""), P("Paso", kFloat)});
    event("event.destroy", "Event Destroy", "Cuando el objeto se destruye o se sale de Play", "destroy end play fin", {X("")});
    const std::vector<Pin> collision = {X(""), P("Otro", kEntity), P("Punto", kVector), P("Normal", kVector),
                                        P("Velocidad relativa", kVector)};
    event("event.collision_enter", "On Collision Enter", "Empieza un choque (el objeto necesita collider)", "hit choque colision",
          collision);
    event("event.collision_stay", "On Collision Stay", "Mientras dura un choque", "hit choque colision", collision);
    event("event.collision_exit", "On Collision Exit", "Termina un choque", "hit choque colision", collision);
    event("event.trigger_enter", "On Trigger Enter", "Algo entra en el trigger del objeto", "overlap trigger entrar zona",
          {X(""), P("Otro", kEntity)});
    event("event.trigger_stay", "On Trigger Stay", "Algo sigue dentro del trigger", "overlap trigger", {X(""), P("Otro", kEntity)});
    event("event.trigger_exit", "On Trigger Exit", "Algo sale del trigger", "overlap trigger salir", {X(""), P("Otro", kEntity)});
    event("event.custom", "Custom Event", "Evento propio: lo lanzan Call Event, Send Event, Set Timer, la interfaz (OnClick) o un script",
          "custom evento propio funcion", {X(""), P("Valor", kAny)}, {P("Nombre", kString, "MiEvento")});
    event("event.input_action", "Input Action", "Una accion de entrada (ProjectSettings/InputActions.json): triggered, started, completed, canceled, ongoing",
          "input accion action enhanced", {X(""), P("Valor", kAny)},
          {P("Accion", kString, "Jump"),
           O(P("Evento", kString, "triggered"), {"triggered", "started", "completed", "canceled", "ongoing"})});
    event("event.key", "Key Event", "Una tecla: pulsada, soltada o mantenida",
          "tecla key teclado keyboard pressed released pulsada soltada mantenida", {X("")},
          {P("Tecla", kString, "Space"), O(P("Cuando", kString, "pulsada"), {"pulsada", "soltada", "mantenida"})});
    event("event.websocket", "On WebSocket", "Evento de una conexion WebSocket por su nombre: message, open, close, error, reconnecting o any",
          "websocket ws servidor mensaje red online mmo", {X(""), P("Evento", kAny), P("Texto", kString), P("Datos", kAny)},
          {P("Conexion", kString, "main"), P("Tipo", kString, "message")});
    event("event.video", "On Video Event",
          "Evento del VideoPlayer del objeto: Prepared, Started, LoopPointReached, FrameReady, SeekCompleted, Error, "
          "FrameDropped, Buffering, Resumed o Any",
          "video reproductor player pelicula loop fin terminado preparado videoplayer",
          {X(""), P("Evento", kString), P("Tiempo", kFloat), P("Cantidad", kInt), P("Mensaje", kString)},
          {O(P("Evento", kString, "LoopPointReached"), {"Prepared", "Started", "LoopPointReached", "FrameReady", "SeekCompleted",
                                                         "Error", "FrameDropped", "Buffering", "Resumed", "Any"})});

    // --- Flujo ---
    node("Flujo", "flow.branch", "Branch", "Si la condicion es verdadera sigue por Verdadero; si no, por Falso", "if si condicion",
         {X(""), P("Condicion", kBool, "true")}, {X("Verdadero"), X("Falso")});
    node("Flujo", "flow.sequence", "Sequence", "Ejecuta sus salidas en orden", "secuencia orden then",
         {X("")}, {X("Then 0"), X("Then 1")});
    node("Flujo", "flow.for", "For Loop", "Repite el Cuerpo con Indice desde..hasta (incluido)", "bucle loop for repetir",
         {X(""), P("Desde", kInt, "0"), P("Hasta", kInt, "9")}, {X("Cuerpo"), P("Indice", kInt), X("Completado")});
    node("Flujo", "flow.foreach", "For Each", "Repite el Cuerpo con cada elemento de una lista", "bucle lista array each",
         {X(""), P("Lista", kAny)}, {X("Cuerpo"), P("Elemento", kAny), P("Indice", kInt), X("Completado")});
    node("Flujo", "flow.while", "While Loop", "Repite mientras la condicion sea verdadera (con un limite de vueltas)",
         "mientras bucle while", {X(""), P("Condicion", kBool, "false"), P("Max vueltas", kInt, "10000")},
         {X("Cuerpo"), X("Completado")});
    node("Flujo", "flow.delay", "Delay", "Espera unos segundos y sigue (el resto del grafo no se para)", "esperar wait delay retraso",
         {X(""), P("Segundos", kFloat, "1")}, {X("Completado")}, true);
    node("Flujo", "flow.do_once", "Do Once", "Solo pasa la primera vez (hasta Reset)", "una vez once",
         {X(""), X("Reset"), P("Empieza cerrado", kBool, "false")}, {X("Completado")});
    node("Flujo", "flow.do_n", "Do N", "Pasa N veces (hasta Reset)", "n veces contador",
         {X(""), X("Reset"), P("N", kInt, "3")}, {X("Salida"), P("Contador", kInt)});
    node("Flujo", "flow.flip_flop", "Flip Flop", "Alterna entre A y B cada vez", "alternar toggle flip",
         {X("")}, {X("A"), X("B"), P("Es A", kBool)});
    node("Flujo", "flow.gate", "Gate", "Una puerta: Entrar solo pasa si esta abierta", "puerta gate abrir cerrar",
         {X("Entrar"), X("Abrir"), X("Cerrar"), X("Alternar"), P("Empieza cerrada", kBool, "false")}, {X("Salida")});
    node("Flujo", "flow.set_timer", "Set Timer", "Lanza un Custom Event dentro de unos segundos (o cada N segundos)",
         "temporizador timer intervalo", {X(""), P("Evento", kString, "MiEvento"), P("Segundos", kFloat, "1"), P("Repetir", kBool, "false")},
         {X("")});
    node("Flujo", "flow.clear_timer", "Clear Timer", "Cancela un temporizador", "temporizador timer cancelar",
         {X(""), P("Evento", kString, "MiEvento")}, {X("")});
    node("Flujo", "flow.call_event", "Call Event", "Llama a un Custom Event de este grafo", "llamar evento call",
         {X(""), P("Evento", kString, "MiEvento"), P("Valor", kAny)}, {X("")});
    node("Flujo", "flow.send_event", "Send Event", "Llama a un Custom Event (o metodo) del Visual Script o script de otro objeto",
         "enviar mensaje evento otro objeto", {X(""), P("Objeto", kEntity), P("Evento", kString, "MiEvento"), P("Valor", kAny)}, {X("")});

    // --- Variables ---
    node("Variables", "var.set", "Set", "Cambia una variable del grafo", "variable asignar set", {X(""), P("Valor", kAny)},
         {X(""), P("Valor", kAny)});
    pure("Variables", "var.get", "Get", "Lee una variable del grafo", "variable leer get", {}, {P("Valor", kAny)});

    // --- Matematicas ---
    const auto binary = [&](const char* kind, const char* title, const char* keywords, const char* a, const char* b) {
        pure("Matematicas", kind, title, "Numeros o vectores", keywords, {P("A", kAny, a), P("B", kAny, b)}, {P("Resultado", kAny)});
    };
    binary("math.add", "+ Sumar", "add sumar mas plus", "0", "0");
    binary("math.sub", "- Restar", "sub restar menos minus", "0", "0");
    binary("math.mul", "* Multiplicar", "mul multiplicar por times", "1", "1");
    binary("math.div", "/ Dividir", "div dividir entre", "1", "1");
    pure("Matematicas", "math.mod", "% Resto", "Resto de dividir A entre B", "modulo resto mod", {P("A", kFloat, "0"), P("B", kFloat, "1")},
         {P("Resultado", kFloat)});
    pure("Matematicas", "math.pow", "Potencia", "A elevado a B", "pow potencia elevado", {P("A", kFloat, "2"), P("B", kFloat, "2")},
         {P("Resultado", kFloat)});
    pure("Matematicas", "math.min", "Min", "El menor", "minimo min", {P("A", kFloat, "0"), P("B", kFloat, "0")}, {P("Resultado", kFloat)});
    pure("Matematicas", "math.max", "Max", "El mayor", "maximo max", {P("A", kFloat, "0"), P("B", kFloat, "0")}, {P("Resultado", kFloat)});
    pure("Matematicas", "math.clamp", "Clamp", "Limita un valor entre Min y Max", "limitar clamp",
         {P("Valor", kFloat, "0"), P("Min", kFloat, "0"), P("Max", kFloat, "1")}, {P("Resultado", kFloat)});
    pure("Matematicas", "math.lerp", "Lerp", "Interpola de A a B (Alpha 0..1); numeros o vectores", "interpolar lerp mezclar",
         {P("A", kAny, "0"), P("B", kAny, "1"), P("Alpha", kFloat, "0.5")}, {P("Resultado", kAny)});
    pure("Matematicas", "math.map_range", "Map Range", "Pasa un valor de un rango a otro", "remap rango map",
         {P("Valor", kFloat, "0"), P("Desde min", kFloat, "0"), P("Desde max", kFloat, "1"), P("Hasta min", kFloat, "0"),
          P("Hasta max", kFloat, "100")},
         {P("Resultado", kFloat)});
    const auto unary = [&](const char* kind, const char* title, const char* keywords) {
        pure("Matematicas", kind, title, "", keywords, {P("Valor", kFloat, "0")}, {P("Resultado", kFloat)});
    };
    unary("math.abs", "Abs", "absoluto abs");
    unary("math.sqrt", "Raiz cuadrada", "sqrt raiz");
    unary("math.floor", "Floor", "redondear abajo floor");
    unary("math.ceil", "Ceil", "redondear arriba ceil");
    unary("math.round", "Round", "redondear round");
    unary("math.sin", "Sin", "seno sin (radianes)");
    unary("math.cos", "Cos", "coseno cos (radianes)");
    unary("math.tan", "Tan", "tangente tan (radianes)");
    unary("math.negate", "Negar", "negativo menos negate");
    pure("Matematicas", "math.atan2", "Atan2", "Angulo (radianes) de Y, X", "atan2 angulo", {P("Y", kFloat, "0"), P("X", kFloat, "1")},
         {P("Resultado", kFloat)});
    pure("Matematicas", "math.random_float", "Random Float", "Numero al azar entre Min y Max", "azar random aleatorio",
         {P("Min", kFloat, "0"), P("Max", kFloat, "1")}, {P("Resultado", kFloat)});
    pure("Matematicas", "math.random_int", "Random Int", "Entero al azar entre Min y Max (incluidos)", "azar random aleatorio dado",
         {P("Min", kInt, "1"), P("Max", kInt, "6")}, {P("Resultado", kInt)});
    pure("Matematicas", "math.pi", "Pi", "3.14159...", "pi", {}, {P("Pi", kFloat)});

    // --- Logica ---
    pure("Logica", "logic.and", "AND", "Las dos son verdaderas", "y and", {P("A", kBool, "false"), P("B", kBool, "false")}, {P("Resultado", kBool)});
    pure("Logica", "logic.or", "OR", "Alguna es verdadera", "o or", {P("A", kBool, "false"), P("B", kBool, "false")}, {P("Resultado", kBool)});
    pure("Logica", "logic.xor", "XOR", "Solo una es verdadera", "xor", {P("A", kBool, "false"), P("B", kBool, "false")}, {P("Resultado", kBool)});
    pure("Logica", "logic.not", "NOT", "Lo contrario", "no not negar", {P("A", kBool, "false")}, {P("Resultado", kBool)});
    const auto compare = [&](const char* kind, const char* title, const char* keywords) {
        pure("Logica", kind, title, "Compara A con B", keywords, {P("A", kAny, "0"), P("B", kAny, "0")}, {P("Resultado", kBool)});
    };
    compare("logic.equal", "== Igual", "igual equal ==");
    compare("logic.not_equal", "!= Distinto", "distinto not equal !=");
    compare("logic.greater", "> Mayor", "mayor greater >");
    compare("logic.less", "< Menor", "menor less <");
    compare("logic.greater_equal", ">= Mayor o igual", "mayor igual >=");
    compare("logic.less_equal", "<= Menor o igual", "menor igual <=");
    pure("Logica", "logic.select", "Select", "Si la condicion es verdadera da A; si no, B", "seleccionar elegir ternario select",
         {P("Condicion", kBool, "true"), P("A", kAny), P("B", kAny)}, {P("Resultado", kAny)});

    // --- Vectores ---
    pure("Vector", "vec.make", "Make Vector", "Vector de X, Y, Z", "vector crear make vec3",
         {P("X", kFloat, "0"), P("Y", kFloat, "0"), P("Z", kFloat, "0")}, {P("Vector", kVector)});
    pure("Vector", "vec.break", "Break Vector", "X, Y, Z de un vector", "vector separar break",
         {P("Vector", kVector, "0 0 0")}, {P("X", kFloat), P("Y", kFloat), P("Z", kFloat)});
    pure("Vector", "vec.length", "Length", "Longitud del vector", "longitud magnitud length", {P("Vector", kVector, "0 0 0")},
         {P("Longitud", kFloat)});
    pure("Vector", "vec.normalize", "Normalize", "El vector con longitud 1", "normalizar direccion", {P("Vector", kVector, "0 0 1")},
         {P("Resultado", kVector)});
    pure("Vector", "vec.distance", "Distance", "Distancia entre dos puntos", "distancia distance",
         {P("A", kVector, "0 0 0"), P("B", kVector, "0 0 0")}, {P("Distancia", kFloat)});
    pure("Vector", "vec.dot", "Dot", "Producto escalar", "dot escalar", {P("A", kVector, "0 0 0"), P("B", kVector, "0 0 0")},
         {P("Resultado", kFloat)});
    pure("Vector", "vec.cross", "Cross", "Producto vectorial", "cross vectorial", {P("A", kVector, "0 0 0"), P("B", kVector, "0 0 0")},
         {P("Resultado", kVector)});
    pure("Vector", "vec.lerp", "Vector Lerp", "Interpola dos vectores (Alpha 0..1)", "interpolar lerp vector",
         {P("A", kVector, "0 0 0"), P("B", kVector, "0 0 0"), P("Alpha", kFloat, "0.5")}, {P("Resultado", kVector)});

    // --- Texto ---
    pure("Texto", "str.append", "Append", "Une dos textos", "unir concatenar append", {P("A", kString, ""), P("B", kString, "")},
         {P("Texto", kString)});
    pure("Texto", "str.to_string", "To String", "Cualquier valor como texto", "texto string convertir", {P("Valor", kAny)},
         {P("Texto", kString)});
    pure("Texto", "str.to_number", "To Number", "Texto como numero (0 si no lo es)", "numero convertir parse",
         {P("Texto", kString, "0")}, {P("Numero", kFloat)});
    pure("Texto", "str.length", "Text Length", "Numero de caracteres", "longitud texto len", {P("Texto", kString, "")}, {P("Longitud", kInt)});
    pure("Texto", "str.contains", "Contains", "El texto contiene a otro?", "contiene buscar contains",
         {P("Texto", kString, ""), P("Buscar", kString, "")}, {P("Resultado", kBool)});

    // --- Objetos ---
    pure("Objeto", "entity.self", "Self", "Este objeto", "self yo este objeto", {}, {P("Objeto", kEntity)});
    // La ruta va en Node::fn (como la variable de un Get); sale al arrastrar un
    // objeto del panel Componentes al Event Graph.
    pure("Objeto", "entity.child", "Obtener objeto hijo (ruta)",
         "Un objeto de este Blueprint por su ruta desde la raiz (\"Cuerpo/Malla\"): cada instancia usa el suyo",
         "hijo child ruta path componente referencia blueprint objeto", {}, {P("Objeto", kEntity)});
    pure("Objeto", "entity.find", "Find Object", "El primer objeto con ese nombre", "buscar find nombre",
         {P("Nombre", kString, "Jugador")}, {P("Objeto", kEntity)});
    pure("Objeto", "entity.find_tag", "Find With Tag", "El primer objeto con ese tag", "buscar tag",
         {P("Tag", kString, "Player")}, {P("Objeto", kEntity)});
    pure("Objeto", "entity.get_position", "Get Position", "Posicion en el mundo", "posicion location transform",
         {P("Objeto", kEntity)}, {P("Posicion", kVector)});
    pure("Objeto", "entity.get_rotation", "Get Rotation", "Giro en grados", "rotacion giro transform", {P("Objeto", kEntity)},
         {P("Rotacion", kVector)});
    pure("Objeto", "entity.get_scale", "Get Scale", "Escala", "escala transform", {P("Objeto", kEntity)}, {P("Escala", kVector)});
    pure("Objeto", "entity.get_forward", "Get Forward", "Direccion hacia delante", "delante forward direccion",
         {P("Objeto", kEntity)}, {P("Delante", kVector)});
    pure("Objeto", "entity.get_name", "Get Name", "Nombre del objeto", "nombre name", {P("Objeto", kEntity)}, {P("Nombre", kString)});
    pure("Objeto", "entity.is_valid", "Is Valid", "El objeto existe?", "valido existe valid", {P("Objeto", kEntity)}, {P("Valido", kBool)});
    pure("Objeto", "entity.distance", "Distance To", "Distancia entre dos objetos", "distancia distance",
         {P("A", kEntity), P("B", kEntity)}, {P("Distancia", kFloat)});
    pure("Objeto", "entity.get_field", "Get Component Field", "Un campo de un componente (\"Light\", \"intensity\")",
         "componente campo field get", {P("Objeto", kEntity), P("Componente", kString, "Light"), P("Campo", kString, "intensity")},
         {P("Valor", kAny)});
    node("Objeto", "entity.set_position", "Set Position", "Mueve el objeto a una posicion", "posicion mover teleport",
         {X(""), P("Objeto", kEntity), P("Posicion", kVector, "0 0 0")}, {X("")});
    node("Objeto", "entity.set_rotation", "Set Rotation", "Giro en grados", "rotacion girar", {X(""), P("Objeto", kEntity),
         P("Rotacion", kVector, "0 0 0")}, {X("")});
    node("Objeto", "entity.set_scale", "Set Scale", "Escala", "escala", {X(""), P("Objeto", kEntity), P("Escala", kVector, "1 1 1")}, {X("")});
    node("Objeto", "entity.translate", "Translate", "Mueve en el mundo", "mover desplazar translate",
         {X(""), P("Objeto", kEntity), P("Desplazamiento", kVector, "0 0 0")}, {X("")});
    node("Objeto", "entity.rotate", "Rotate", "Gira unos grados", "girar rotar", {X(""), P("Objeto", kEntity), P("Grados", kVector, "0 90 0")},
         {X("")});
    node("Objeto", "entity.look_at", "Look At", "Mira hacia un punto", "mirar look", {X(""), P("Objeto", kEntity), P("Punto", kVector, "0 0 0")},
         {X("")});
    node("Objeto", "entity.add_force", "Add Force", "Fuerza a su Rigidbody (force, impulse, acceleration, velocity)", "fuerza impulso fisica",
         {X(""), P("Objeto", kEntity), P("Fuerza", kVector, "0 5 0"), P("Modo", kString, "impulse")}, {X("")});
    node("Objeto", "entity.set_active", "Set Active", "Activa o desactiva el objeto", "activar desactivar visible",
         {X(""), P("Objeto", kEntity), P("Activo", kBool, "true")}, {X("")});
    node("Objeto", "entity.spawn", "Spawn", "Crea una instancia de un prefab (\"Prefabs/Bala\") o copia un objeto", "crear spawn instanciar prefab",
         {X(""), P("Prefab", kAny, "Prefabs/MiPrefab"), P("Posicion", kVector, "0 0 0"), P("Rotacion", kVector, "0 0 0")},
         {X(""), P("Objeto", kEntity)});
    node("Objeto", "entity.destroy", "Destroy", "Destruye el objeto (al final del frame)", "destruir borrar destroy",
         {X(""), P("Objeto", kEntity)}, {X("")});
    node("Objeto", "entity.set_field", "Set Component Field", "Cambia un campo de un componente", "componente campo field set",
         {X(""), P("Objeto", kEntity), P("Componente", kString, "Light"), P("Campo", kString, "intensity"), P("Valor", kAny, "1")}, {X("")});

    // --- Entrada ---
    pure("Entrada", "input.key", "Get Key", "La tecla esta pulsada", "tecla key", {P("Tecla", kString, "W")}, {P("Pulsada", kBool)});
    pure("Entrada", "input.key_down", "Get Key Down", "La tecla se pulso este frame", "tecla key down", {P("Tecla", kString, "Space")},
         {P("Pulsada", kBool)});
    pure("Entrada", "input.key_up", "Get Key Up", "La tecla se solto este frame", "tecla key up", {P("Tecla", kString, "Space")},
         {P("Soltada", kBool)});
    pure("Entrada", "input.axis", "Get Axis", "-1..1: Horizontal, Vertical, Mouse X, Mouse Y", "eje axis", {P("Eje", kString, "Horizontal")},
         {P("Valor", kFloat)});
    pure("Entrada", "input.mouse_button", "Get Mouse Button", "Boton del raton (0 izq, 1 der, 2 medio)", "raton mouse boton",
         {P("Boton", kInt, "0")}, {P("Pulsado", kBool)});
    pure("Entrada", "input.action", "Get Action", "Valor de una accion de entrada", "accion action input", {P("Accion", kString, "Move")},
         {P("Valor", kAny)});

    // --- Tiempo ---
    pure("Tiempo", "time.delta", "Delta Time", "Segundos del ultimo frame", "delta tiempo dt", {}, {P("Delta", kFloat)});
    pure("Tiempo", "time.time", "Time", "Segundos desde que empezo el Play", "tiempo time", {}, {P("Tiempo", kFloat)});
    pure("Tiempo", "time.frame", "Frame Count", "Numero de frame", "frame", {}, {P("Frame", kInt)});

    // --- Depuracion ---
    node("Depuracion", "debug.print", "Print", "Escribe en la Consola", "print log imprimir consola", {X(""), P("Texto", kAny, "Hola")}, {X("")});
    node("Depuracion", "debug.warn", "Print Warning", "Aviso en la Consola", "warning aviso", {X(""), P("Texto", kAny, "Cuidado")}, {X("")});

    // --- Flujo (como Unreal) ---
    node("Flujo", "flow.switch_int", "Switch on Int", "Sigue por la salida con el numero del Selector (si no hay, por Default)",
         "switch caso case entero int", {X(""), P("Selector", kInt, "0")}, {X("Default"), X("0"), X("1")});
    c.back().dynamic_outputs = true;
    node("Flujo", "flow.switch_string", "Switch on String", "Sigue por la salida que se llama como el Selector (si no hay, por Default)",
         "switch caso case texto string", {X(""), P("Selector", kString, "")}, {X("Default"), X("A"), X("B")});
    c.back().dynamic_outputs = true;
    node("Flujo", "flow.for_break", "For Loop with Break", "Como For Loop; Break lo para y sigue por Completado",
         "bucle loop for repetir romper break", {X(""), P("Desde", kInt, "0"), P("Hasta", kInt, "9"), X("Break")},
         {X("Cuerpo"), P("Indice", kInt), X("Completado")});
    node("Flujo", "flow.foreach_break", "For Each with Break", "Como For Each; Break lo para y sigue por Completado",
         "bucle lista array each romper break", {X(""), P("Lista", kAny), X("Break")},
         {X("Cuerpo"), P("Elemento", kAny), P("Indice", kInt), X("Completado")});
    node("Flujo", "flow.multigate", "MultiGate", "Cada vez sigue por una salida distinta (en orden o al azar) hasta usarlas todas",
         "multigate puertas salidas orden azar",
         {X(""), X("Reset"), P("Aleatorio", kBool, "false"), P("Repetir", kBool, "false"), P("Indice inicial", kInt, "-1")},
         {X("Salida 0"), X("Salida 1"), X("Salida 2")});
    c.back().dynamic_outputs = true;
    node("Flujo", "flow.retrig_delay", "Retriggerable Delay", "Espera unos segundos; si vuelve a entrar antes, la cuenta empieza de nuevo",
         "esperar wait delay retraso reiniciar retriggerable", {X(""), P("Segundos", kFloat, "0.2")}, {X("Completado")}, true);
    node("Flujo", "flow.is_valid", "Is Valid", "Sigue por Valido si el objeto existe; si no, por No valido",
         "valido existe valid nil comprobar", {X(""), P("Objeto", kAny)}, {X("Valido"), X("No valido")});
    pure("Logica", "flow.select_int", "Select", "La opcion con el numero del Indice (desde 0)", "seleccionar elegir select indice opcion",
         {P("Indice", kInt, "0"), P("Opcion 0", kAny), P("Opcion 1", kAny)}, {P("Resultado", kAny)});
    c.back().dynamic_inputs = true;

    // --- Matematicas extra ---
    pure("Matematicas", "math.finterp_to", "FInterp To", "Acerca Actual a Objetivo con una velocidad (Delta vacio = el del frame)",
         "interpolar suavizar interp to", {P("Actual", kFloat, "0"), P("Objetivo", kFloat, "1"), P("Delta", kFloat, ""),
         P("Velocidad", kFloat, "5")}, {P("Resultado", kFloat)});
    pure("Matematicas", "math.vinterp_to", "VInterp To", "Acerca un vector a otro con una velocidad (Delta vacio = el del frame)",
         "interpolar suavizar interp to vector", {P("Actual", kVector, "0 0 0"), P("Objetivo", kVector, "0 0 0"), P("Delta", kFloat, ""),
         P("Velocidad", kFloat, "5")}, {P("Resultado", kVector)});
    pure("Matematicas", "math.nearly_equal", "Nearly Equal", "A y B se diferencian menos que la tolerancia", "casi igual nearly equal",
         {P("A", kFloat, "0"), P("B", kFloat, "0"), P("Tolerancia", kFloat, "0.0001")}, {P("Resultado", kBool)});
    pure("Matematicas", "math.in_range", "In Range", "El valor esta entre Min y Max", "rango entre range",
         {P("Valor", kFloat, "0"), P("Min", kFloat, "0"), P("Max", kFloat, "1"), P("Incluir min", kBool, "true"),
          P("Incluir max", kBool, "true")}, {P("Resultado", kBool)});
    pure("Matematicas", "math.wrap", "Wrap", "Da la vuelta al valor dentro de Min..Max", "envolver wrap ciclo",
         {P("Valor", kFloat, "0"), P("Min", kFloat, "0"), P("Max", kFloat, "1")}, {P("Resultado", kFloat)});
    unary("math.sign", "Sign", "signo sign");
    unary("math.frac", "Frac", "fraccion decimales frac");
    unary("math.asin", "Asin", "arcoseno asin (radianes)");
    unary("math.acos", "Acos", "arcocoseno acos (radianes)");
    unary("math.atan", "Atan", "arcotangente atan (radianes)");
    unary("math.exp", "Exp", "exponencial e exp");
    unary("math.log", "Log", "logaritmo natural ln log");
    unary("math.deg_to_rad", "Grados a radianes", "grados radianes degrees radians");
    unary("math.rad_to_deg", "Radianes a grados", "radianes grados radians degrees");
    pure("Matematicas", "math.random_bool", "Random Bool", "Verdadero o falso al azar", "azar random aleatorio moneda", {},
         {P("Resultado", kBool)});

    // --- Conversiones (nodos pequenos) ---
    pure("Conversion", "conv.int_to_float", "Entero a decimal", "", "convertir int float", {P("Valor", kInt, "0")}, {P("Resultado", kFloat)});
    pure("Conversion", "conv.float_to_int", "Decimal a entero", "Quita los decimales (hacia 0)", "convertir float int truncar",
         {P("Valor", kFloat, "0")}, {P("Resultado", kInt)});
    pure("Conversion", "conv.bool_to_int", "Booleano a entero", "Verdadero = 1, falso = 0", "convertir bool int",
         {P("Valor", kBool, "false")}, {P("Resultado", kInt)});

    // --- Texto extra ---
    pure("Texto", "str.format", "Format Text", "Cambia cada {nombre} del Formato por la entrada con ese nombre",
         "formato format texto interpolar", {P("Formato", kString, "Vida: {vida}"), P("vida", kAny)}, {P("Texto", kString)});
    c.back().dynamic_inputs = true;
    pure("Texto", "str.substring", "Substring", "Parte del texto (Inicio desde 0)", "subcadena substring cortar parte",
         {P("Texto", kString, ""), P("Inicio", kInt, "0"), P("Longitud", kInt, "1")}, {P("Texto", kString)});
    pure("Texto", "str.replace", "Replace", "Cambia cada aparicion de Buscar por Reemplazar", "reemplazar replace cambiar",
         {P("Texto", kString, ""), P("Buscar", kString, ""), P("Reemplazar", kString, "")}, {P("Texto", kString)});
    pure("Texto", "str.split", "Split", "Parte el texto por un separador (lista de textos)", "partir split separar lista",
         {P("Texto", kString, ""), P("Separador", kString, ",")}, {P("Lista", kAny)});
    pure("Texto", "str.upper", "To Upper", "En mayusculas", "mayusculas upper", {P("Texto", kString, "")}, {P("Texto", kString)});
    pure("Texto", "str.lower", "To Lower", "En minusculas", "minusculas lower", {P("Texto", kString, "")}, {P("Texto", kString)});
    pure("Texto", "str.trim", "Trim", "Sin espacios al principio ni al final", "trim espacios recortar", {P("Texto", kString, "")},
         {P("Texto", kString)});
    pure("Texto", "str.starts_with", "Starts With", "El texto empieza por el prefijo?", "empieza starts prefijo",
         {P("Texto", kString, ""), P("Prefijo", kString, "")}, {P("Resultado", kBool)});
    pure("Texto", "str.ends_with", "Ends With", "El texto acaba en el sufijo?", "acaba termina ends sufijo",
         {P("Texto", kString, ""), P("Sufijo", kString, "")}, {P("Resultado", kBool)});
    pure("Texto", "str.is_empty", "Is Empty", "El texto esta vacio?", "vacio empty", {P("Texto", kString, "")}, {P("Resultado", kBool)});

    // --- Listas (Array): indices desde 1, como la API y For Each ---
    pure("Array", "array.make", "Make Array", "Una lista con sus entradas", "lista array crear make", {P("1", kAny), P("2", kAny)},
         {P("Lista", kAny)});
    c.back().dynamic_inputs = true;
    node("Array", "array.add", "Add", "Anade un elemento al final (si la lista viene de una variable, la cambia)",
         "lista array anadir add push", {X(""), P("Lista", kAny), P("Elemento", kAny)}, {X(""), P("Lista", kAny), P("Indice", kInt)});
    pure("Array", "array.get", "Get", "El elemento de esa posicion (desde 1)", "lista array get elemento indice",
         {P("Lista", kAny), P("Indice", kInt, "1")}, {P("Elemento", kAny)});
    pure("Array", "array.length", "Length", "Numero de elementos", "lista array longitud length tamano", {P("Lista", kAny)},
         {P("Longitud", kInt)});
    pure("Array", "array.contains", "Contains", "La lista tiene ese elemento?", "lista array contiene contains",
         {P("Lista", kAny), P("Elemento", kAny)}, {P("Resultado", kBool)});
    pure("Array", "array.find", "Find", "Posicion del elemento (desde 1; 0 si no esta)", "lista array buscar find indice",
         {P("Lista", kAny), P("Elemento", kAny)}, {P("Indice", kInt)});
    node("Array", "array.remove_index", "Remove Index", "Quita el elemento de esa posicion (desde 1)", "lista array quitar remove",
         {X(""), P("Lista", kAny), P("Indice", kInt, "1")}, {X(""), P("Lista", kAny)});
    node("Array", "array.clear", "Clear", "Deja la lista vacia", "lista array vaciar clear", {X(""), P("Lista", kAny)},
         {X(""), P("Lista", kAny)});

    // --- Objeto: dueno y componentes ---
    std::vector<std::string> components;
    std::vector<std::string> addable;
    for (const ecs::ComponentType& type : ecs::ComponentRegistry::instance().types()) {
        components.push_back(type.name);
        if (type.addable && type.removable) addable.push_back(type.name);
    }
    pure("Objeto", "entity.owner", "Get Owner", "La raiz de su prefab o Blueprint (o el propio objeto)", "dueno owner actor raiz root",
         {P("Objeto", kEntity)}, {P("Dueno", kEntity)});
    pure("Objeto", "component.get", "Get Component", "El objeto (este o uno de sus hijos) que tiene ese componente",
         "componente component get buscar", {P("Objeto", kEntity), O(P("Componente", kString, "Light"), components)},
         {P("Objeto", kEntity), P("Encontrado", kBool)});
    pure("Objeto", "component.has", "Has Component", "El objeto tiene ese componente?", "componente component has tiene",
         {P("Objeto", kEntity), O(P("Componente", kString, "Rigidbody"), components)}, {P("Resultado", kBool)});
    node("Objeto", "component.add", "Add Component", "Anade un componente por su nombre", "componente component add anadir",
         {X(""), P("Objeto", kEntity), O(P("Componente", kString, "Rigidbody"), addable)}, {X(""), P("Hecho", kBool)});
    node("Objeto", "component.remove", "Remove Component", "Quita un componente por su nombre", "componente component remove quitar",
         {X(""), P("Objeto", kEntity), O(P("Componente", kString, "Rigidbody"), addable)}, {X(""), P("Hecho", kBool)});
    pure("Objeto", "component.ref", "Component", "Un objeto de este Blueprint o prefab (por su id dentro del prefab o su nombre)",
         "componente referencia hijo blueprint", {P("Componente", kString, "")}, {P("Objeto", kEntity)});

    // --- Fisica ---
    node("Fisica", "physics.line_trace", "Line Trace", "Un rayo de Inicio a Fin: el primer objeto que toca",
         "raycast rayo trazar line trace fisica hit",
         {X(""), P("Inicio", kVector, "0 0 0"), P("Fin", kVector, "0 0 10"), P("Ignorar este objeto", kBool, "true")},
         {X(""), P("Hit", kBool), P("Objeto", kEntity), P("Punto", kVector), P("Normal", kVector), P("Distancia", kFloat),
          P("Resultado", kAny)});
    pure("Fisica", "physics.break_hit", "Break Hit Result", "Los datos de un Resultado de Line Trace (o de Physics.raycast)",
         "hit resultado break raycast", {P("Resultado", kAny)},
         {P("Hit", kBool), P("Objeto", kEntity), P("Punto", kVector), P("Normal", kVector), P("Distancia", kFloat)});

    // --- Vector extra ---
    pure("Vector", "vec.rotate", "Rotate Vector", "Gira un vector unos grados (X, Y, Z)", "girar rotar vector",
         {P("Vector", kVector, "0 0 1"), P("Grados", kVector, "0 90 0")}, {P("Resultado", kVector)});
    pure("Vector", "vec.unit_direction", "Get Unit Direction", "Direccion (longitud 1) de Desde a Hasta", "direccion unit direction",
         {P("Desde", kVector, "0 0 0"), P("Hasta", kVector, "0 0 1")}, {P("Direccion", kVector)});
    pure("Vector", "vec.distance2d", "Distance 2D", "Distancia en el suelo (X, Z), sin la altura", "distancia 2d suelo plano",
         {P("A", kVector, "0 0 0"), P("B", kVector, "0 0 0")}, {P("Distancia", kFloat)});

    // --- Utilidades ---
    pure("Utilidades", "util.reroute", "Reroute", "Punto de paso para ordenar los cables", "reroute punto cable", {P("", kAny)}, {P("", kAny)});
    {
        NodeInfo n;
        n.kind = "comment";
        n.title = "Comentario";
        n.category = "Utilidades";
        n.description = "Caja con un texto para agrupar nodos";
        n.keywords = "comentario nota comment";
        n.pure = true;
        c.push_back(std::move(n));
    }
    return c;
}

bool isEventKind(const std::string& kind) {
    const NodeInfo* info = findNodeInfo(kind);
    return info != nullptr && info->event;
}

// Metodos que el motor llama en un script: un Custom Event no se puede llamar asi.
bool reservedMethod(const std::string& name) {
    static const std::set<std::string> kReserved = {
        "Awake", "Start", "Update", "LateUpdate", "FixedUpdate", "OnDestroy", "OnCollisionEnter", "OnCollisionStay",
        "OnCollisionExit", "OnTriggerEnter", "OnTriggerStay", "OnTriggerExit", "OnOriginShift", "OnNetVar", "properties", "entity"};
    return kReserved.contains(name) || name.rfind("__", 0) == 0;
}

}  // namespace

// --- Tipos ---------------------------------------------------------------------------

const char* pinTypeKey(PinType type) {
    const int i = static_cast<int>(type);
    return i >= 0 && i < kPinTypeCount ? kPinKeys[i] : "any";
}

const char* pinTypeLabel(PinType type) {
    const int i = static_cast<int>(type);
    return i >= 0 && i < kPinTypeCount ? kPinLabels[i] : "Cualquiera";
}

PinType pinTypeFromKey(const std::string& key, PinType fallback) {
    const std::string k = lower(key);
    for (int i = 0; i < kPinTypeCount; ++i) {
        if (k == kPinKeys[i]) return static_cast<PinType>(i);
    }
    if (k == "number" || k == "double") return PinType::Float;
    if (k == "vector" || k == "vec") return PinType::Vector;
    if (k == "text") return PinType::String;
    return fallback;
}

bool pinTypesCompatible(PinType from, PinType to) {
    if (from == PinType::Exec || to == PinType::Exec) return from == to;
    if (from == to || from == PinType::Any || to == PinType::Any) return true;
    if ((from == PinType::Int && to == PinType::Float) || (from == PinType::Float && to == PinType::Int)) return true;
    if (to == PinType::String) return true;  // todo se puede escribir
    if (to == PinType::Bool) return from != PinType::Vector;  // numero / texto / objeto "con valor"
    return false;
}

// --- Grafo --------------------------------------------------------------------------

int Node::input(const std::string& name) const {
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        if (inputs[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

int Node::output(const std::string& name) const {
    for (std::size_t i = 0; i < outputs.size(); ++i) {
        if (outputs[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

Node* Graph::find(int id) {
    for (Node& n : nodes) {
        if (n.id == id) return &n;
    }
    return nullptr;
}

const Node* Graph::find(int id) const {
    for (const Node& n : nodes) {
        if (n.id == id) return &n;
    }
    return nullptr;
}

const Variable* Graph::findVariable(const std::string& name) const {
    for (const Variable& v : variables) {
        if (v.name == name) return &v;
    }
    return nullptr;
}

Variable* Graph::findVariable(const std::string& name) {
    for (Variable& v : variables) {
        if (v.name == name) return &v;
    }
    return nullptr;
}

const Link* Graph::linkTo(int node, int pin) const {
    for (const Link& l : links) {
        if (l.to_node == node && l.to_pin == pin) return &l;
    }
    return nullptr;
}

void Graph::removeNode(int id) {
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(), [&](const Node& n) { return n.id == id; }), nodes.end());
    links.erase(std::remove_if(links.begin(), links.end(), [&](const Link& l) { return l.from_node == id || l.to_node == id; }),
                links.end());
}

bool Graph::connect(int from_node, int from_pin, int to_node, int to_pin, std::string* error) {
    const auto fail = [&](const char* message) {
        if (error) *error = message;
        return false;
    };
    const Node* a = find(from_node);
    const Node* b = find(to_node);
    if (a == nullptr || b == nullptr) return fail("El nodo no existe");
    if (a == b) return fail("No se puede enlazar un nodo consigo mismo");
    if (from_pin < 0 || from_pin >= static_cast<int>(a->outputs.size()) || to_pin < 0 || to_pin >= static_cast<int>(b->inputs.size())) {
        return fail("El pin no existe");
    }
    if (isEventKind(b->kind)) return fail("Los ajustes de un evento no aceptan enlaces");
    PinType from_type = a->outputs[from_pin].type;
    PinType to_type = b->inputs[to_pin].type;
    if ((a->kind == "var.get" || a->kind == "var.set") && from_type != PinType::Exec) {
        if (const Variable* v = findVariable(a->fn)) from_type = v->type;
    }
    if (b->kind == "var.set" && to_type != PinType::Exec) {
        if (const Variable* v = findVariable(b->fn)) to_type = v->type;
    }
    if ((from_type == PinType::Exec) != (to_type == PinType::Exec)) {
        return fail("Un pin de ejecucion solo se enlaza con otro de ejecucion");
    }
    if (!pinTypesCompatible(from_type, to_type)) return fail("Los tipos no son compatibles");
    if (from_type == PinType::Exec) {
        // Una salida exec va a un solo sitio (para varios: Sequence).
        links.erase(std::remove_if(links.begin(), links.end(),
                                   [&](const Link& l) { return l.from_node == from_node && l.from_pin == from_pin; }),
                    links.end());
    } else {
        // Una entrada de datos tiene un solo valor.
        links.erase(std::remove_if(links.begin(), links.end(),
                                   [&](const Link& l) { return l.to_node == to_node && l.to_pin == to_pin; }),
                    links.end());
    }
    links.push_back(Link{from_node, from_pin, to_node, to_pin});
    return true;
}

void Graph::renameVariable(const std::string& from, const std::string& to) {
    for (Variable& v : variables) {
        if (v.name == from) v.name = to;
    }
    for (Node& n : nodes) {
        if ((n.kind == "var.get" || n.kind == "var.set") && n.fn == from) n.fn = to;
    }
}

Node& Graph::add(Node node) {
    int max_id = 0;
    for (const Node& n : nodes) max_id = std::max(max_id, n.id);
    next_id = std::max(next_id, max_id + 1);
    node.id = next_id++;
    nodes.push_back(std::move(node));
    return nodes.back();
}

// --- Catalogo ------------------------------------------------------------------------

const std::vector<NodeInfo>& nodeCatalog() {
    static const std::vector<NodeInfo> catalog = buildCatalog();
    return catalog;
}

const NodeInfo* findNodeInfo(const std::string& kind) {
    // Indice por clave (el interprete lo pregunta mucho).
    static const std::unordered_map<std::string, const NodeInfo*> index = [] {
        std::unordered_map<std::string, const NodeInfo*> out;
        for (const NodeInfo& n : nodeCatalog()) out.emplace(n.kind, &n);
        return out;
    }();
    const auto it = index.find(kind);
    return it == index.end() ? nullptr : it->second;
}

Node makeNode(const std::string& kind, core::Vec2 position) {
    Node n;
    n.kind = kind;
    n.position = position;
    if (const NodeInfo* info = findNodeInfo(kind)) {
        n.inputs = info->inputs;
        n.outputs = info->outputs;
        n.pure = info->pure;
    }
    if (kind == "comment") n.size = core::Vec2{320.0f, 160.0f};
    return n;
}

std::string nodeTitle(const Node& node) {
    if (node.kind == "var.get") return node.fn.empty() ? std::string("Get ?") : node.fn;
    if (node.kind == "var.set") return "Set " + (node.fn.empty() ? std::string("?") : node.fn);
    if (node.kind == "call") return node.fn.empty() ? std::string("Llamada") : node.fn;
    if (node.kind == "call.get") return "Get " + node.fn;
    if (node.kind == "call.set") return "Set " + node.fn;
    if (node.kind == "event.custom" && !node.inputs.empty() && !node.inputs[0].value.empty()) {
        return "Evento: " + node.inputs[0].value;
    }
    if (node.kind == "event.input_action" && !node.inputs.empty() && !node.inputs[0].value.empty()) {
        return "Input Action: " + node.inputs[0].value;
    }
    if (node.kind == "event.key" && !node.inputs.empty() && !node.inputs[0].value.empty()) {
        return "Tecla " + node.inputs[0].value;
    }
    if (node.kind == "event.video" && !node.inputs.empty() && !node.inputs[0].value.empty()) {
        return "On Video " + node.inputs[0].value;
    }
    if (node.kind == "comment") return node.comment.empty() ? std::string("Comentario") : node.comment;
    if (node.kind == "entity.child") {
        const std::string name = entityChildPathName(node.fn);
        return name.empty() ? std::string("Objeto hijo ?") : name;
    }
    if (const NodeInfo* info = findNodeInfo(node.kind)) return info->title;
    return node.kind;
}

core::Vec3 nodeColor(const Node& node) {
    if (node.kind == "call" || node.kind == "call.get" || node.kind == "call.set") {
        return node.pure || node.kind == "call.get" ? core::Vec3{0.20f, 0.45f, 0.30f} : core::Vec3{0.18f, 0.35f, 0.62f};
    }
    const NodeInfo* info = findNodeInfo(node.kind);
    const std::string category = info != nullptr ? info->category : std::string();
    if (category == "Eventos") return core::Vec3{0.62f, 0.12f, 0.12f};
    if (category == "Flujo") return core::Vec3{0.38f, 0.38f, 0.40f};
    if (category == "Variables") return core::Vec3{0.45f, 0.30f, 0.55f};
    if (category == "Matematicas" || category == "Logica" || category == "Vector" || category == "Texto" ||
        category == "Conversion" || category == "Array") {
        return core::Vec3{0.20f, 0.45f, 0.30f};
    }
    if (category == "Objeto" || category == "Fisica") return core::Vec3{0.18f, 0.35f, 0.62f};
    if (category == "Entrada" || category == "Tiempo") return core::Vec3{0.55f, 0.40f, 0.12f};
    if (category == "Depuracion") return core::Vec3{0.30f, 0.30f, 0.32f};
    return core::Vec3{0.28f, 0.28f, 0.30f};
}

// --- Objeto hijo por ruta ------------------------------------------------------

namespace {

struct ChildPathPart {
    std::string name;
    int ordinal = 1;  // 1 = el primero con ese nombre entre sus hermanos
};

std::string escapePathName(const std::string& name) {
    std::string out;
    for (const char c : name) {
        if (c == '/' || c == '[' || c == ']' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

std::vector<ChildPathPart> parseChildPath(const std::string& path) {
    std::vector<ChildPathPart> parts;
    ChildPathPart part;
    bool any = false;
    for (std::size_t i = 0; i < path.size(); ++i) {
        const char c = path[i];
        if (c == '\\' && i + 1 < path.size()) {
            part.name += path[++i];
            any = true;
        } else if (c == '/') {
            if (any) parts.push_back(part);
            part = {};
            any = false;
        } else if (c == '[') {
            // "[n]" al final del tramo; si no es un numero, se toma como texto.
            const std::size_t close = path.find(']', i);
            const std::string digits = close == std::string::npos ? std::string() : path.substr(i + 1, close - i - 1);
            if (!digits.empty() && std::all_of(digits.begin(), digits.end(), [](unsigned char d) { return std::isdigit(d) != 0; })) {
                part.ordinal = std::max(1, std::atoi(digits.c_str()));
                i = close;
            } else {
                part.name += c;
            }
            any = true;
        } else {
            part.name += c;
            any = true;
        }
    }
    if (any) parts.push_back(part);
    return parts;
}

}  // namespace

std::string entityChildPath(const ecs::Entity& root, const ecs::Entity& e) {
    if (!root.valid() || !e.valid() || root.handle() == e.handle() || !root.isAncestorOf(e)) return {};
    std::vector<std::string> parts;
    for (ecs::Entity x = e; x.valid() && x.handle() != root.handle(); x = x.parent()) {
        const ecs::Entity parent = x.parent();
        int ordinal = 1;
        if (parent.valid() && parent.world() != nullptr) {
            for (const entt::entity child : parent.children()) {
                if (child == x.handle()) break;
                if (parent.world()->wrap(child).name() == x.name()) ++ordinal;
            }
        }
        std::string part = escapePathName(x.name());
        if (ordinal > 1) part += "[" + std::to_string(ordinal) + "]";
        parts.push_back(std::move(part));
    }
    std::string out;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        if (!out.empty()) out += '/';
        out += *it;
    }
    return out;
}

ecs::Entity resolveEntityChildPath(const ecs::Entity& root, const std::string& path) {
    if (!root.valid() || root.world() == nullptr) return {};
    const std::vector<ChildPathPart> parts = parseChildPath(path);
    if (parts.empty()) return {};
    ecs::Entity at = root;
    for (const ChildPathPart& part : parts) {
        ecs::Entity next;
        int seen = 0;
        for (const entt::entity child : at.children()) {
            const ecs::Entity c = at.world()->wrap(child);
            if (c.name() == part.name && ++seen == part.ordinal) {
                next = c;
                break;
            }
        }
        if (!next.valid()) return {};
        at = next;
    }
    return at;
}

std::string entityChildPathName(const std::string& path) {
    const std::vector<ChildPathPart> parts = parseChildPath(path);
    if (parts.empty()) return {};
    const ChildPathPart& last = parts.back();
    return last.ordinal > 1 ? last.name + " [" + std::to_string(last.ordinal) + "]" : last.name;
}

bool looksPure(const std::string& callee) {
    std::size_t sep = callee.find_last_of(".:");
    const std::string owner = sep == std::string::npos ? std::string() : callee.substr(0, sep);
    const std::string name = sep == std::string::npos ? callee : callee.substr(sep + 1);
    static const std::set<std::string> kPureOwners = {"math", "Mathf", "Vec3", "Vec2", "Quat", "Color", "string"};
    if (kPureOwners.contains(owner)) return true;
    static const char* kPrefixes[] = {"get", "is", "has", "was", "find", "distance", "sqr", "dot", "cross", "length",
                                      "normalized", "angle", "lerp", "to", "can", "any", "surface", "in", "blockInfo"};
    for (const char* p : kPrefixes) {
        const std::size_t n = std::char_traits<char>::length(p);
        if (name.size() >= n && name.compare(0, n, p) == 0 &&
            (name.size() == n || std::isupper(static_cast<unsigned char>(name[n])) || std::string(p) == name)) {
            return true;
        }
    }
    return false;
}

namespace {

// Tipo de un objeto de la API ("Entity:...", "Vec3:...") como pin.
PinType ownerPinType(const std::string& owner) {
    if (owner == "Entity") return PinType::Entity;
    if (owner == "Vec3") return PinType::Vector;
    return PinType::Any;
}

// Un argumento documentado ("\"W\"", "Vec3", "desde", "Vec3(0, 0, 3)") como pin.
Pin pinFromDoc(const std::string& raw) {
    const std::string p = trim(raw);
    Pin pin;
    pin.type = PinType::Any;
    double number = 0.0;
    if (p.empty()) {
        pin.name = "Valor";
    } else if (p[0] == '"') {
        // Un texto de ejemplo: el pin es de texto y el ejemplo su valor.
        const std::size_t close = p.find('"', 1);
        pin.name = "Texto";
        pin.type = PinType::String;
        pin.value = close != std::string::npos ? p.substr(1, close - 1) : p.substr(1);
        if (close != std::string::npos && close + 1 < p.size()) {
            pin.type = PinType::Any;  // "entity o \"Prefabs/...\""
        }
    } else if (p == "true" || p == "false") {
        pin.name = "Activar";
        pin.type = PinType::Bool;
        pin.value = p;
    } else if (parseNumber(p, number)) {
        pin.name = "Valor";
        pin.type = p.find('.') != std::string::npos ? PinType::Float : PinType::Int;
        pin.value = p;
    } else if (p.rfind("Vec3", 0) == 0) {
        pin.type = PinType::Vector;
        const std::vector<double> v = numbersIn(p.substr(4));
        if (v.size() == 3) pin.value = formatNumber(v[0]) + " " + formatNumber(v[1]) + " " + formatNumber(v[2]);
        // "Vec3 grados": el nombre es la palabra de despues.
        const std::size_t space = p.find(' ');
        pin.name = space != std::string::npos && p.find('(') == std::string::npos ? trim(p.substr(space + 1)) : std::string("Vector");
    } else if (p.rfind("function", 0) == 0) {
        pin.name = "Funcion";
    } else if (p[0] == '{') {
        pin.name = "Tabla";
    } else {
        // "desde", "freno de mano", "entity o ..."
        std::string name = p;
        if (const std::size_t q = name.find('"'); q != std::string::npos) name = trim(name.substr(0, q));
        if (name.size() > 2 && name.compare(name.size() - 2, 2, " o") == 0) name = name.substr(0, name.size() - 2);
        pin.name = name.empty() ? std::string("Valor") : name;
        const std::string l = lower(pin.name);
        static const std::set<std::string> kEntityNames = {"entity", "otra", "otro", "objetivo", "modelo", "target", "other", "objeto", "padre"};
        static const std::set<std::string> kVectorNames = {"posicion", "pos", "punto", "direccion", "destino", "vec", "offset", "giro", "velocidad"};
        static const std::set<std::string> kStringNames = {"nombre", "tag", "clave", "texto", "ruta", "accion", "evento", "tecla"};
        if (kEntityNames.contains(l)) pin.type = PinType::Entity;
        else if (kVectorNames.contains(l)) pin.type = PinType::Vector;
        else if (kStringNames.contains(l)) pin.type = PinType::String;
        else if (l == "x" || l == "y" || l == "z" || l == "t" || l == "segundos" || l == "radio" || l == "peso") pin.type = PinType::Float;
        if (pin.type == PinType::Entity) pin.value.clear();
        // Primera letra en mayuscula (como el resto de pines).
        pin.name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(pin.name[0])));
    }
    return pin;
}

std::vector<std::string> splitArgs(const std::string& args) {
    std::vector<std::string> parts;
    int depth = 0;
    bool quote = false;
    std::string cur;
    for (const char c : args) {
        if (c == '"') quote = !quote;
        if (!quote && (c == '(' || c == '{' || c == '[')) ++depth;
        if (!quote && (c == ')' || c == '}' || c == ']')) --depth;
        if (c == ',' && depth == 0 && !quote) {
            parts.push_back(trim(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!trim(cur).empty()) parts.push_back(trim(cur));
    return parts;
}

void uniquePinNames(std::vector<Pin>& pins) {
    std::map<std::string, int> seen;
    for (Pin& p : pins) {
        if (p.type == PinType::Exec) continue;
        const int n = ++seen[p.name];
        if (n > 1) p.name += " " + std::to_string(n);
    }
}

PinType returnTypeOf(const std::string& name, const std::string& description) {
    const std::string d = lower(trim(description));
    if (d.rfind("vec3", 0) == 0) return PinType::Vector;
    if (!d.empty() && d.back() == '?') return PinType::Bool;
    if (d.find("true/false") != std::string::npos) return PinType::Bool;
    for (const char* p : {"is", "has", "was", "can"}) {
        const std::size_t n = std::char_traits<char>::length(p);
        if (name.size() > n && name.compare(0, n, p) == 0 && std::isupper(static_cast<unsigned char>(name[n]))) return PinType::Bool;
    }
    if (d.find("objeto") != std::string::npos && (name.rfind("find", 0) == 0 || name == "instantiate" || name == "create")) {
        return PinType::Entity;
    }
    return PinType::Any;
}

}  // namespace

Node makeCallNode(const std::string& callee, const std::string& doc_args, const std::string& description, core::Vec2 position) {
    Node n;
    n.kind = "call";
    n.fn = callee;
    n.position = position;
    n.pure = looksPure(callee);
    n.comment = description;
    const std::size_t colon = callee.find(':');
    const std::size_t sep = callee.find_last_of(".:");
    const std::string member = sep == std::string::npos ? callee : callee.substr(sep + 1);
    if (!n.pure) {
        n.inputs.push_back(X(""));
        n.outputs.push_back(X(""));
    }
    if (colon != std::string::npos) {
        const std::string owner = callee.substr(0, colon);
        n.inputs.push_back(Pin{owner == "Entity" ? "Objeto" : owner, ownerPinType(owner), {}});
    }
    for (const std::string& part : splitArgs(doc_args)) {
        if (part.rfind("...", 0) == 0) {
            // Variable: dos huecos (los que sobren sin enlace no se envian).
            n.inputs.push_back(Pin{"Valor", PinType::Any, {}});
            n.inputs.push_back(Pin{"Valor", PinType::Any, {}});
            break;
        }
        n.inputs.push_back(pinFromDoc(part));
    }
    uniquePinNames(n.inputs);
    n.outputs.push_back(Pin{"Resultado", returnTypeOf(member, description), {}});
    return n;
}

Node makePropertyNode(const std::string& callee, bool set, const std::string& description, core::Vec2 position) {
    Node n;
    n.kind = set ? "call.set" : "call.get";
    n.fn = callee;
    n.position = position;
    n.pure = !set;
    n.comment = description;
    const std::size_t colon = callee.find(':');
    const std::size_t sep = callee.find_last_of(".:");
    const std::string member = sep == std::string::npos ? callee : callee.substr(sep + 1);
    PinType type = returnTypeOf(member, description);
    static const std::set<std::string> kVectors = {"position", "localPosition", "rotation", "scale", "forward", "right", "up",
                                                   "velocity", "angularVelocity", "navVelocity", "uiPosition", "uiSize", "color"};
    if (kVectors.contains(member)) type = PinType::Vector;
    if (member == "name" || member == "tag" || member == "text") type = PinType::String;
    if (member == "active" || member == "ragdoll" || member == "interactable" || member == "castShadows") type = PinType::Bool;
    if (set) n.inputs.push_back(X(""));
    if (colon != std::string::npos) {
        const std::string owner = callee.substr(0, colon);
        n.inputs.push_back(Pin{owner == "Entity" ? "Objeto" : owner, ownerPinType(owner), {}});
    }
    if (set) {
        n.inputs.push_back(Pin{"Valor", type, type == PinType::Vector ? "0 0 0" : ""});
        n.outputs.push_back(X(""));
    } else {
        std::string label = member;
        if (!label.empty()) label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
        n.outputs.push_back(Pin{label, type, {}});
    }
    return n;
}

// --- Aspecto y busqueda por contexto -------------------------------------------------

namespace {

// Operadores que se dibujan como un nodo pequeno con su simbolo.
const std::unordered_map<std::string, const char*>& compactSymbols() {
    static const std::unordered_map<std::string, const char*> symbols = {
        {"math.add", "+"},           {"math.sub", "-"},          {"math.mul", "\xC3\x97"},    {"math.div", "\xC3\xB7"},
        {"math.mod", "%"},           {"logic.and", "AND"},       {"logic.or", "OR"},          {"logic.xor", "XOR"},
        {"logic.not", "NOT"},        {"logic.equal", "=="},      {"logic.not_equal", "!="},   {"logic.greater", ">"},
        {"logic.less", "<"},         {"logic.greater_equal", ">="}, {"logic.less_equal", "<="}, {"conv.int_to_float", ""},
        {"conv.float_to_int", ""},   {"conv.bool_to_int", ""}};
    return symbols;
}

std::uint32_t typeMask(const std::vector<Pin>& pins) {
    std::uint32_t mask = 0;
    for (const Pin& p : pins) {
        const int i = static_cast<int>(p.type);
        if (i >= 0 && i < 32) mask |= std::uint32_t(1) << i;
    }
    return mask;
}

}  // namespace

NodeLook nodeLook(const Node& node) {
    if (node.kind == "comment") return NodeLook::Comment;
    if (node.kind == "util.reroute") return NodeLook::Reroute;
    if (node.kind == "var.get" || node.kind == "entity.child") return NodeLook::Variable;
    if (compactSymbols().contains(node.kind)) return NodeLook::Compact;
    const NodeInfo* info = findNodeInfo(node.kind);
    if (info != nullptr && info->event) return NodeLook::Event;
    if (info != nullptr && info->category == "Flujo" && !info->pure) return NodeLook::Flow;
    if (isPureNode(node)) return NodeLook::Pure;
    return NodeLook::Function;
}

const char* nodeSymbol(const Node& node) {
    const auto it = compactSymbols().find(node.kind);
    return it == compactSymbols().end() ? "" : it->second;
}

std::uint32_t inputTypeMask(const Node& node) { return typeMask(node.inputs); }
std::uint32_t outputTypeMask(const Node& node) { return typeMask(node.outputs); }

std::optional<std::string> conversionKind(PinType from, PinType to) {
    if (from == PinType::Exec || to == PinType::Exec || pinTypesCompatible(from, to)) return std::nullopt;
    if (from == PinType::Bool && (to == PinType::Int || to == PinType::Float)) return std::string("conv.bool_to_int");
    if (from == PinType::Entity && to == PinType::Vector) return std::string("entity.get_position");
    if (from == PinType::String && (to == PinType::Int || to == PinType::Float)) return std::string("str.to_number");
    if (from == PinType::String && to == PinType::Entity) return std::string("entity.find");
    return std::nullopt;
}

PinType typeFromDocText(const std::string& returns) {
    std::string t = lower(trim(returns));
    // "X (nota)" y "X o nil": cuenta X.
    if (const std::size_t paren = t.find(" ("); paren != std::string::npos) t = trim(t.substr(0, paren));
    if (t.size() > 6 && t.compare(t.size() - 6, 6, " o nil") == 0) t = trim(t.substr(0, t.size() - 6));
    if (t.empty()) return PinType::Any;
    // Listas, varias posibilidades y objetos de datos: cualquier cosa.
    if (t.rfind("lista", 0) == 0 || t.find(',') != std::string::npos || t.find(" o ") != std::string::npos || t[0] == '{') {
        return PinType::Any;
    }
    if (t == "bool" || t == "booleano" || t.rfind("true si", 0) == 0) return PinType::Bool;
    if (t == "numero" || t.rfind("numero ", 0) == 0 || t == "entero" || t == "id" || t.rfind("id ", 0) == 0) return PinType::Float;
    if (t == "texto" || t == "string") return PinType::String;
    if (t == "vec3" || t == "vector") return PinType::Vector;
    if (t == "entity") return PinType::Entity;
    // "objeto", "valor", "Quat", "Mesh", handles y texto libre.
    return PinType::Any;
}

namespace {

// Nodo de la API con el tipo de lo que devuelve (Doc.returns).
void retypeResult(Node& node, PinType type) {
    if (type == PinType::Any) return;
    for (Pin& p : node.outputs) {
        if (p.type != PinType::Exec) p.type = type;
    }
}

std::vector<ApiNodeTemplate> buildApiTemplates(const scripting::api::NativeApi& api) {
    using scripting::api::Entry;
    std::vector<ApiNodeTemplate> out;
    for (const Entry& e : api.entries()) {
        if (e.owner.empty() || e.name.empty() || e.name.rfind("__", 0) == 0) continue;
        const std::string category = "API: " + e.owner;
        const std::string keywords = e.name + " " + e.owner;
        const PinType returns = typeFromDocText(e.doc.returns);
        if (e.kind == Entry::Kind::Property) {
            // De un tipo ("Entity:position": con pin del objeto) o de una tabla ("Time.deltaTime").
            const std::string callee = e.owner + (e.member() ? ":" : ".") + e.name;
            const std::string shown = e.owner + "." + e.name;
            ApiNodeTemplate get{category, "Get " + shown, keywords, e.doc.description, makePropertyNode(callee, false, e.doc.description)};
            retypeResult(get.node, returns);
            out.push_back(std::move(get));
            if (e.assign) {
                ApiNodeTemplate set{category, "Set " + shown, keywords, e.doc.description, makePropertyNode(callee, true, e.doc.description)};
                if (returns != PinType::Any && !set.node.inputs.empty()) set.node.inputs.back().type = returns;
                out.push_back(std::move(set));
            }
            continue;
        }
        const std::string callee = e.owner + (e.member() ? ":" : ".") + e.name;
        ApiNodeTemplate call{category, callee, keywords, e.doc.description, makeCallNode(callee, e.doc.args, e.doc.description)};
        if (call.node.pure) {
            // Sin pines exec (como los que crea el editor para las funciones puras).
            std::erase_if(call.node.inputs, [](const Pin& p) { return p.type == PinType::Exec; });
            std::erase_if(call.node.outputs, [](const Pin& p) { return p.type == PinType::Exec; });
        }
        if (!e.doc.returns.empty()) {
            retypeResult(call.node, returns);
        } else if (!e.doc.description.empty() && !call.node.pure) {
            // Documentada y sin nada que devolver: sin pin Resultado.
            std::erase_if(call.node.outputs, [](const Pin& p) { return p.type != PinType::Exec; });
        }
        out.push_back(std::move(call));
    }
    std::stable_sort(out.begin(), out.end(), [](const ApiNodeTemplate& a, const ApiNodeTemplate& b) {
        return a.category != b.category ? a.category < b.category : a.title < b.title;
    });
    return out;
}

}  // namespace

const std::vector<ApiNodeTemplate>& apiNodeTemplates(const scripting::api::NativeApi& api) {
    static std::mutex mutex;
    static std::map<const scripting::api::NativeApi*, std::vector<ApiNodeTemplate>> cache;
    std::lock_guard<std::mutex> lock(mutex);
    auto it = cache.find(&api);
    if (it == cache.end()) it = cache.emplace(&api, buildApiTemplates(api)).first;
    return it->second;
}

std::vector<std::string> formatTextArguments(const std::string& format) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while ((at = format.find('{', at)) != std::string::npos) {
        const std::size_t close = format.find('}', at + 1);
        if (close == std::string::npos) break;
        const std::string name = trim(format.substr(at + 1, close - at - 1));
        if (!name.empty() && name.find('{') == std::string::npos &&
            std::find(out.begin(), out.end(), name) == out.end()) {
            out.push_back(name);
        }
        at = close + 1;
    }
    return out;
}

// --- Archivo --------------------------------------------------------------------------

namespace {

std::string jsonText(const json& j) {
    if (j.is_string()) return j.get<std::string>();
    if (j.is_boolean()) return j.get<bool>() ? "true" : "false";
    if (j.is_number_integer()) return std::to_string(j.get<long long>());
    if (j.is_number()) return formatNumber(j.get<double>());
    if (j.is_array()) {
        std::string out;
        for (const json& x : j) {
            if (!out.empty()) out += ' ';
            out += x.is_number() ? formatNumber(x.get<double>()) : std::string("0");
        }
        return out;
    }
    return {};
}

std::vector<Pin> readPins(const json& j) {
    std::vector<Pin> pins;
    if (!j.is_array()) return pins;
    for (const json& p : j) {
        if (!p.is_object()) continue;
        Pin pin;
        pin.name = p.value("name", std::string());
        pin.type = pinTypeFromKey(p.value("type", std::string("any")));
        if (p.contains("value")) pin.value = jsonText(p["value"]);
        pins.push_back(std::move(pin));
    }
    return pins;
}

json writePins(const std::vector<Pin>& pins) {
    json out = json::array();
    for (const Pin& p : pins) {
        json j{{"name", p.name}, {"type", pinTypeKey(p.type)}};
        if (!p.value.empty()) j["value"] = p.value;
        out.push_back(std::move(j));
    }
    return out;
}

// Tipos de los pines de las variables (Get/Set siguen a su variable).
void retypeVariableNodes(Graph& g) {
    for (Node& n : g.nodes) {
        if (n.kind != "var.get" && n.kind != "var.set") continue;
        const Variable* v = g.findVariable(n.fn);
        if (v == nullptr) continue;
        for (Pin& p : n.inputs) {
            if (p.type != PinType::Exec) p.type = v->type;
        }
        for (Pin& p : n.outputs) {
            if (p.type != PinType::Exec) p.type = v->type;
        }
    }
}

// "out": 2 o "out": "Verdadero" (o "exec" = la primera de ejecucion).
int pinIndex(const json& j, const std::vector<Pin>& pins) {
    if (j.is_number_integer()) return j.get<int>();
    if (!j.is_string()) return 0;
    const std::string name = j.get<std::string>();
    for (std::size_t i = 0; i < pins.size(); ++i) {
        if (pins[i].name == name) return static_cast<int>(i);
    }
    const std::string l = lower(name);
    if (l == "exec" || l == "then" || l == "out" || l == "in") {
        for (std::size_t i = 0; i < pins.size(); ++i) {
            if (pins[i].type == PinType::Exec) return static_cast<int>(i);
        }
    }
    for (std::size_t i = 0; i < pins.size(); ++i) {
        if (lower(pins[i].name) == l) return static_cast<int>(i);
    }
    return -1;
}

}  // namespace

bool graphFromJson(const std::string& text, Graph& out, std::string* error) {
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object()) {
        if (error) *error = "JSON no valido";
        return false;
    }
    Graph g;
    if (j.contains("uuid") && j["uuid"].is_string()) g.uuid = Uuid::parse(j["uuid"].get<std::string>());
    if (j.contains("variables") && j["variables"].is_array()) {
        for (const json& v : j["variables"]) {
            if (!v.is_object()) continue;
            Variable var;
            var.name = v.value("name", std::string());
            var.type = pinTypeFromKey(v.value("type", std::string("float")), PinType::Float);
            if (var.type == PinType::Exec) var.type = PinType::Any;
            if (v.contains("value")) var.value = jsonText(v["value"]);
            var.exposed = v.value("exposed", true);
            var.tooltip = v.value("tooltip", std::string());
            if (!var.name.empty()) g.variables.push_back(std::move(var));
        }
    }
    if (j.contains("nodes") && j["nodes"].is_array()) {
        for (const json& jn : j["nodes"]) {
            if (!jn.is_object()) continue;
            Node n;
            n.id = jn.value("id", 0);
            n.kind = jn.value("kind", std::string());
            n.position = core::Vec2{jn.value("x", 0.0f), jn.value("y", 0.0f)};
            n.fn = jn.value("fn", std::string());
            n.comment = jn.value("comment", std::string());
            n.size = core::Vec2{jn.value("w", 0.0f), jn.value("h", 0.0f)};
            n.breakpoint = jn.value("breakpoint", false);
            n.disabled = jn.value("disabled", false);
            if (jn.contains("comment_color") && jn["comment_color"].is_number_unsigned()) {
                n.comment_color = jn["comment_color"].get<std::uint32_t>();
            }
            const std::vector<Pin> stored_in = readPins(jn.contains("inputs") ? jn["inputs"] : json());
            const std::vector<Pin> stored_out = readPins(jn.contains("outputs") ? jn["outputs"] : json());
            if (const NodeInfo* info = findNodeInfo(n.kind)) {
                n.inputs = info->inputs;
                n.outputs = info->outputs;
                n.pure = info->pure;
                if (n.kind == "flow.sequence" && stored_out.size() > n.outputs.size()) {
                    for (std::size_t k = n.outputs.size(); k < stored_out.size(); ++k) {
                        n.outputs.push_back(Pin{"Then " + std::to_string(k), PinType::Exec, {}});
                    }
                }
                // Pines que anadio el usuario (Switch, MultiGate, Make Array, Format Text...):
                // los guardados mandan (nombres incluidos); las opciones salen del catalogo.
                const auto keepStored = [](std::vector<Pin>& pins, const std::vector<Pin>& stored) {
                    if (stored.empty()) return;
                    std::vector<Pin> merged = stored;
                    for (Pin& p : merged) {
                        for (const Pin& c : pins) {
                            if (c.name == p.name && c.type == p.type) p.options = c.options;
                        }
                    }
                    pins = std::move(merged);
                };
                if (info->dynamic_outputs) keepStored(n.outputs, stored_out);
                if (info->dynamic_inputs) keepStored(n.inputs, stored_in);
                // Valores guardados: por indice si el nombre coincide, si no por nombre.
                for (std::size_t k = 0; k < stored_in.size(); ++k) {
                    int index = k < n.inputs.size() && n.inputs[k].name == stored_in[k].name ? static_cast<int>(k) : n.input(stored_in[k].name);
                    if (index >= 0) n.inputs[index].value = stored_in[k].value;
                }
            } else {
                n.inputs = stored_in;
                n.outputs = stored_out;
                n.pure = jn.value("pure", n.kind == "call.get");
                // Una IA puede dar solo la funcion y su documentacion de argumentos.
                if ((n.kind == "call" || n.kind == "call.get" || n.kind == "call.set") && n.inputs.empty() && n.outputs.empty() &&
                    !n.fn.empty()) {
                    const Node made = n.kind == "call" ? makeCallNode(n.fn, jn.value("doc", std::string()), n.comment)
                                                       : makePropertyNode(n.fn, n.kind == "call.set", n.comment);
                    n.inputs = made.inputs;
                    n.outputs = made.outputs;
                    if (!jn.contains("pure")) n.pure = made.pure;
                    if (n.pure && n.kind == "call") {
                        // Sin pines exec.
                        n.inputs.erase(std::remove_if(n.inputs.begin(), n.inputs.end(), [](const Pin& p) { return p.type == PinType::Exec; }),
                                       n.inputs.end());
                        n.outputs.erase(std::remove_if(n.outputs.begin(), n.outputs.end(), [](const Pin& p) { return p.type == PinType::Exec; }),
                                        n.outputs.end());
                    }
                }
            }
            if (jn.contains("values") && jn["values"].is_object()) {
                for (const auto& [key, value] : jn["values"].items()) {
                    const int index = n.input(key);
                    if (index >= 0) n.inputs[index].value = jsonText(value);
                }
            }
            if (n.id <= 0) n.id = 0;
            g.nodes.push_back(std::move(n));
        }
    }
    // Ids que faltan o repetidos: nuevos.
    std::set<int> used;
    int max_id = 0;
    for (const Node& n : g.nodes) max_id = std::max(max_id, n.id);
    for (Node& n : g.nodes) {
        if (n.id <= 0 || !used.insert(n.id).second) {
            n.id = ++max_id;
            used.insert(n.id);
        }
    }
    g.next_id = std::max(j.value("next_id", 1), max_id + 1);
    retypeVariableNodes(g);
    if (j.contains("links") && j["links"].is_array()) {
        for (const json& jl : j["links"]) {
            if (!jl.is_object()) continue;
            Link l;
            l.from_node = jl.value("from", 0);
            l.to_node = jl.value("to", 0);
            const Node* a = g.find(l.from_node);
            const Node* b = g.find(l.to_node);
            if (a == nullptr || b == nullptr) continue;
            l.from_pin = jl.contains("out") ? pinIndex(jl["out"], a->outputs) : 0;
            l.to_pin = jl.contains("in") ? pinIndex(jl["in"], b->inputs) : 0;
            if (l.from_pin < 0 || l.to_pin < 0) continue;
            if (std::none_of(g.links.begin(), g.links.end(), [&](const Link& x) { return x == l; })) g.links.push_back(l);
        }
    }
    out = std::move(g);
    return true;
}

std::string graphToJson(const Graph& g) {
    json j;
    j["uuid"] = g.uuid.valid() ? g.uuid.toString() : Uuid::generate().toString();
    j["version"] = 1;
    j["next_id"] = g.next_id;
    json vars = json::array();
    for (const Variable& v : g.variables) {
        json jv{{"name", v.name}, {"type", pinTypeKey(v.type)}, {"value", v.value}, {"exposed", v.exposed}};
        if (!v.tooltip.empty()) jv["tooltip"] = v.tooltip;
        vars.push_back(std::move(jv));
    }
    j["variables"] = std::move(vars);
    json nodes = json::array();
    for (const Node& n : g.nodes) {
        json jn{{"id", n.id}, {"kind", n.kind}, {"x", n.position.x}, {"y", n.position.y}};
        if (!n.fn.empty()) jn["fn"] = n.fn;
        if (n.kind == "call") jn["pure"] = n.pure;
        if (!n.comment.empty()) jn["comment"] = n.comment;
        if (n.kind == "comment") {
            jn["w"] = n.size.x;
            jn["h"] = n.size.y;
        }
        if (n.breakpoint) jn["breakpoint"] = true;
        if (n.disabled) jn["disabled"] = true;
        if (n.comment_color != 0) jn["comment_color"] = n.comment_color;
        jn["inputs"] = writePins(n.inputs);
        jn["outputs"] = writePins(n.outputs);
        nodes.push_back(std::move(jn));
    }
    j["nodes"] = std::move(nodes);
    json links = json::array();
    for (const Link& l : g.links) links.push_back(json{{"from", l.from_node}, {"out", l.from_pin}, {"to", l.to_node}, {"in", l.to_pin}});
    j["links"] = std::move(links);
    return j.dump(2);
}

bool loadGraph(const std::filesystem::path& path, Graph& out, std::string* error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        if (error) *error = "no se pudo abrir " + path.string();
        return false;
    }
    std::stringstream ss;
    ss << file.rdbuf();
    std::string problem;
    if (!graphFromJson(ss.str(), out, &problem)) {
        if (error) *error = problem + " en " + path.string();
        return false;
    }
    return true;
}

bool saveGraph(const Graph& graph, const std::filesystem::path& path, std::string* error) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        if (error) *error = "no se pudo escribir " + path.string();
        return false;
    }
    file << graphToJson(graph);
    return static_cast<bool>(file);
}

Graph exampleGraph() {
    Graph g;
    g.uuid = Uuid::generate();
    g.variables.push_back(Variable{"velocidadGiro", PinType::Float, "45", true, "Grados por segundo"});
    Node& start = g.add(makeNode("event.start", core::Vec2{0.0f, 0.0f}));
    Node print = makeNode("debug.print", core::Vec2{260.0f, 0.0f});
    print.inputs[1].value = "Hola desde Visual Scripting";
    const int start_id = start.id;
    const int print_id = g.add(std::move(print)).id;
    g.connect(start_id, 0, print_id, 0);

    const int update_id = g.add(makeNode("event.update", core::Vec2{0.0f, 180.0f})).id;
    const int rotate_id = g.add(makeNode("entity.rotate", core::Vec2{520.0f, 180.0f})).id;
    Node get = makeNode("var.get", core::Vec2{0.0f, 320.0f});
    get.fn = "velocidadGiro";
    get.outputs[0].type = PinType::Float;
    const int get_id = g.add(std::move(get)).id;
    const int mul_id = g.add(makeNode("math.mul", core::Vec2{180.0f, 300.0f})).id;
    const int make_id = g.add(makeNode("vec.make", core::Vec2{340.0f, 300.0f})).id;
    g.connect(update_id, 0, rotate_id, 0);
    g.connect(get_id, 0, mul_id, 0);
    g.connect(update_id, 1, mul_id, 1);
    g.connect(mul_id, 0, make_id, 1);
    g.connect(make_id, 0, rotate_id, 2);
    Node note = makeNode("comment", core::Vec2{-30.0f, 130.0f});
    note.comment = "Gira el objeto cada frame";
    note.size = core::Vec2{720.0f, 290.0f};
    g.add(std::move(note));
    return g;
}

// --- Validar ----------------------------------------------------------------------------

PinType nodePinType(const Graph& graph, const Node& node, bool output, int index) {
    const std::vector<Pin>& pins = output ? node.outputs : node.inputs;
    if (index < 0 || index >= static_cast<int>(pins.size())) return PinType::Any;
    const PinType t = pins[static_cast<std::size_t>(index)].type;
    if ((node.kind == "var.get" || node.kind == "var.set") && t != PinType::Exec) {
        if (const Variable* v = graph.findVariable(node.fn)) return v->type;
    }
    return t;
}

bool isPureNode(const Node& node) {
    if (node.kind == "call") return node.pure;
    if (node.kind == "call.get" || node.kind == "comment") return true;
    if (node.kind == "call.set") return false;
    const NodeInfo* info = findNodeInfo(node.kind);
    return info != nullptr && info->pure;
}

Literal parseLiteral(const std::string& text, PinType type, bool* ok) {
    if (ok != nullptr) *ok = true;
    Literal out;
    const std::string v = trim(text);
    double number = 0.0;
    switch (type) {
        case PinType::Exec: return out;
        case PinType::Bool: {
            const std::string l = lower(v);
            out.kind = Literal::Kind::Bool;
            out.boolean = l == "true" || l == "1" || l == "si" || l == "yes";
            return out;
        }
        case PinType::Int:
        case PinType::Float:
            out.kind = Literal::Kind::Number;
            if (v.empty()) return out;
            if (!parseNumber(v, number)) {
                if (ok != nullptr) *ok = false;
                return out;
            }
            out.number = type == PinType::Int ? std::round(number) : number;
            return out;
        case PinType::String:
            out.kind = Literal::Kind::Text;
            out.text = text;
            return out;
        case PinType::Vector: {
            out.kind = Literal::Kind::Vector;
            const std::vector<double> c = numbersIn(v);
            if (c.size() == 3) out.vector = core::Vec3{static_cast<float>(c[0]), static_cast<float>(c[1]), static_cast<float>(c[2])};
            if (c.size() == 1) out.vector = core::Vec3{static_cast<float>(c[0])};
            return out;
        }
        case PinType::Entity:
            out.kind = v.empty() ? Literal::Kind::Self : Literal::Kind::Find;
            out.text = v;
            return out;
        case PinType::Any: {
            if (v.empty()) return out;
            if (parseNumber(v, number)) {
                out.kind = Literal::Kind::Number;
                out.number = number;
                return out;
            }
            const std::string l = lower(v);
            if (l == "true" || l == "false") {
                out.kind = Literal::Kind::Bool;
                out.boolean = l == "true";
                return out;
            }
            if (l == "nil") return out;
            const std::vector<double> c = numbersIn(v);
            if (c.size() == 3 && v.find_first_not_of("0123456789.-+eE ,()Vvec") == std::string::npos) {
                out.kind = Literal::Kind::Vector;
                out.vector = core::Vec3{static_cast<float>(c[0]), static_cast<float>(c[1]), static_cast<float>(c[2])};
                return out;
            }
            out.kind = Literal::Kind::Text;
            out.text = text;
            return out;
        }
    }
    return out;
}

namespace {

class Validator {
public:
    explicit Validator(const Graph& graph) : g_(graph) {}

    std::vector<NodeError> run() {
        for (const Node& n : g_.nodes) by_id_[n.id] = &n;
        for (const Link& l : g_.links) {
            in_links_[{l.to_node, l.to_pin}].push_back(&l);
            out_links_[{l.from_node, l.from_pin}].push_back(&l);
        }
        structure();
        literals();
        cycles();
        return std::move(errors_);
    }

private:
    const Graph& g_;
    std::vector<NodeError> errors_;
    std::map<int, const Node*> by_id_;
    std::map<std::pair<int, int>, std::vector<const Link*>> in_links_;   // (nodo, entrada)
    std::map<std::pair<int, int>, std::vector<const Link*>> out_links_;  // (nodo, salida)
    std::set<std::pair<int, std::string>> reported_;

    void error(int node, const std::string& message) {
        if (!reported_.insert({node, message}).second) return;
        errors_.push_back(NodeError{node, message});
    }
    void warning(int node, const std::string& message) {
        if (!reported_.insert({node, message}).second) return;
        errors_.push_back(NodeError{node, message, true});
    }
    static bool isCall(const Node& n) { return n.kind == "call" || n.kind == "call.get" || n.kind == "call.set"; }

    PinType pinType(const Node& n, bool output, int index) const { return nodePinType(g_, n, output, index); }

    static bool validCallee(const std::string& fn) {
        if (fn.empty()) return false;
        std::string part;
        int separators = 0;
        for (const char c : fn) {
            if (c == '.' || c == ':') {
                if (!isIdentifier(part)) return false;
                part.clear();
                ++separators;
            } else {
                part += c;
            }
        }
        return isIdentifier(part) && separators <= 2;
    }

    // Variables, nodos, eventos y enlaces.
    void structure() {
        std::set<std::string> names;
        for (const Variable& v : g_.variables) {
            if (v.name.empty()) error(0, "Una variable no tiene nombre");
            else if (!names.insert(v.name).second) error(0, "Hay dos variables \"" + v.name + "\"");
            else if (reservedMethod(v.name) || v.name == "entity") error(0, "La variable \"" + v.name + "\" usa un nombre reservado");
        }
        std::set<std::string> events;
        std::set<std::string> custom_names;
        for (const Node& n : g_.nodes) {
            if (n.kind == "event.custom" && !n.inputs.empty() && !n.disabled) custom_names.insert(trim(n.inputs[0].value));
        }
        for (const Node& n : g_.nodes) {
            const NodeInfo* info = findNodeInfo(n.kind);
            const bool call = n.kind == "call" || n.kind == "call.get" || n.kind == "call.set";
            if (n.disabled) {
                // Desactivado: no se comprueba (deja pasar la ejecucion); su valor no se puede usar.
                if (isPureNode(n) && n.kind != "comment") {
                    for (std::size_t i = 0; i < n.outputs.size(); ++i) {
                        if (out_links_.contains({n.id, static_cast<int>(i)})) {
                            error(n.id, "El nodo esta desactivado y se usa su valor");
                            break;
                        }
                    }
                }
                continue;
            }
            if (info == nullptr && !call) {
                error(n.id, "Tipo de nodo desconocido: " + n.kind);
                continue;
            }
            if ((n.kind == "var.get" || n.kind == "var.set") && g_.findVariable(n.fn) == nullptr) {
                error(n.id, n.fn.empty() ? std::string("Elige una variable") : "No existe la variable \"" + n.fn + "\"");
            }
            if (call && !validCallee(n.fn)) error(n.id, "Funcion no valida: \"" + n.fn + "\"");
            if (n.kind == "entity.child" && entityChildPathName(n.fn).empty()) {
                error(n.id, "Escribe la ruta del objeto (Detalles), p. ej. \"Cuerpo/Malla\"");
            }
            if (n.kind == "event.custom") {
                const std::string name = n.inputs.empty() ? std::string() : trim(n.inputs[0].value);
                if (name.empty()) error(n.id, "El evento necesita un nombre");
                else if (reservedMethod(name)) error(n.id, "\"" + name + "\" es un nombre reservado del motor");
                else if (!events.insert(name).second) error(n.id, "Hay dos Custom Event \"" + name + "\"");
            }
            // Eventos con nombre fijo que no existen (temporizadores y Call Event).
            if ((n.kind == "flow.set_timer" || n.kind == "flow.call_event") && n.inputs.size() > 1 &&
                g_.linkTo(n.id, 1) == nullptr) {
                const std::string name = trim(n.inputs[1].value);
                if (!custom_names.contains(name)) error(n.id, "No hay ningun Custom Event \"" + name + "\" en el grafo");
            }
            if (isEventKind(n.kind)) {
                for (std::size_t i = 0; i < n.inputs.size(); ++i) {
                    if (in_links_.contains({n.id, static_cast<int>(i)})) error(n.id, "Los ajustes de un evento no aceptan enlaces");
                }
            }
        }
        for (const Link& l : g_.links) {
            const auto a = by_id_.find(l.from_node);
            const auto b = by_id_.find(l.to_node);
            if (a == by_id_.end() || b == by_id_.end()) {
                error(0, "Un enlace va a un nodo que no existe");
                continue;
            }
            if (l.from_pin < 0 || l.from_pin >= static_cast<int>(a->second->outputs.size()) || l.to_pin < 0 ||
                l.to_pin >= static_cast<int>(b->second->inputs.size())) {
                error(l.to_node, "Un enlace va a un pin que no existe");
                continue;
            }
            const PinType from = pinType(*a->second, true, l.from_pin);
            const PinType to = pinType(*b->second, false, l.to_pin);
            if ((from == PinType::Exec) != (to == PinType::Exec)) {
                error(l.to_node, "Enlace de ejecucion con un pin de datos");
            } else if (!pinTypesCompatible(from, to)) {
                const std::string message = std::string("No se puede enlazar ") + pinTypeLabel(from) + " con " + pinTypeLabel(to) +
                                            " (" + b->second->inputs[static_cast<std::size_t>(l.to_pin)].name + ")";
                // Los nodos de la API comprueban en la llamada: un enlace viejo que su tipo
                // nuevo (Doc.returns) ya no admite es un aviso, no un error.
                if (isCall(*a->second) || isCall(*b->second)) warning(l.to_node, message);
                else error(l.to_node, message);
            }
        }
        for (const auto& [key, list] : out_links_) {
            const auto it = by_id_.find(key.first);
            if (it == by_id_.end() || list.size() < 2) continue;
            if (pinType(*it->second, true, key.second) == PinType::Exec) {
                error(key.first, "Una salida de ejecucion solo puede ir a un nodo (usa Sequence)");
            }
        }
        for (const auto& [key, list] : in_links_) {
            const auto it = by_id_.find(key.first);
            if (it == by_id_.end() || list.size() < 2) continue;
            if (pinType(*it->second, false, key.second) != PinType::Exec) {
                error(key.first, "Una entrada de datos solo puede tener un enlace");
            }
        }
    }

    // Numeros escritos en las entradas sin enlace y en las variables.
    void literals() {
        for (const Variable& v : g_.variables) {
            if (v.name.empty() || (v.type != PinType::Int && v.type != PinType::Float)) continue;
            bool ok = true;
            parseLiteral(v.value, v.type, &ok);
            if (!ok) error(0, "\"" + v.name + "\": \"" + trim(v.value) + "\" no es un numero");
        }
        for (const Node& n : g_.nodes) {
            if (n.kind == "comment" || n.disabled) continue;
            for (std::size_t i = 0; i < n.inputs.size(); ++i) {
                const PinType t = pinType(n, false, static_cast<int>(i));
                if ((t != PinType::Int && t != PinType::Float) || in_links_.contains({n.id, static_cast<int>(i)})) continue;
                bool ok = true;
                parseLiteral(n.inputs[i].value, t, &ok);
                if (!ok) error(n.id, "\"" + n.inputs[i].name + "\": \"" + trim(n.inputs[i].value) + "\" no es un numero");
            }
        }
    }

    // Un nodo de datos (puro) no puede depender de si mismo.
    void cycles() {
        std::map<int, int> mark;  // 1 visitando, 2 hecho
        for (const Node& n : g_.nodes) {
            if (isPureNode(n)) visit(n, mark);
        }
    }
    void visit(const Node& n, std::map<int, int>& mark) {
        int& m = mark[n.id];
        if (m == 2) return;
        if (m == 1) {
            error(n.id, "Ciclo entre nodos de datos (un valor depende de si mismo)");
            return;
        }
        m = 1;
        for (std::size_t i = 0; i < n.inputs.size(); ++i) {
            const auto it = in_links_.find({n.id, static_cast<int>(i)});
            if (it == in_links_.end() || it->second.empty()) continue;
            const auto src = by_id_.find(it->second.front()->from_node);
            if (src != by_id_.end() && isPureNode(*src->second)) visit(*src->second, mark);
        }
        mark[n.id] = 2;
    }
};

}  // namespace

CompileResult compileGraph(const Graph& graph, const std::string& /*chunk*/) {
    CompileResult r;
    r.errors = Validator(graph).run();
    r.ok = std::none_of(r.errors.begin(), r.errors.end(), [](const NodeError& e) { return !e.warning; });
    return r;
}

scripting::PropertyType propertyTypeOf(PinType type) {
    switch (type) {
        case PinType::Bool: return scripting::PropertyType::Bool;
        case PinType::Int:
        case PinType::Float: return scripting::PropertyType::Number;
        case PinType::Vector: return scripting::PropertyType::Vector;
        default: return scripting::PropertyType::Text;
    }
}

std::vector<scripting::ScriptProperty> exposedProperties(const Graph& graph) {
    std::vector<scripting::ScriptProperty> out;
    for (const Variable& v : graph.variables) {
        if (!v.exposed || v.name.empty() || v.type == PinType::Exec) continue;
        scripting::ScriptProperty p;
        p.name = v.name;
        p.type = propertyTypeOf(v.type);
        p.value = v.value;
        if (p.type == scripting::PropertyType::Bool) {
            const std::string l = lower(trim(v.value));
            p.value = l == "true" || l == "1" ? "true" : "false";
        }
        if (p.type == scripting::PropertyType::Vector && numbersIn(v.value).size() != 3) p.value = "0 0 0";
        if (p.type == scripting::PropertyType::Number && p.value.empty()) p.value = "0";
        out.push_back(std::move(p));
    }
    return out;
}

// --- Componente ---------------------------------------------------------------------

void VisualScript::reflect(ecs::PropertyVisitor& v) {
    v.field({"graph", "Visual Script", "Archivo .crgraph en Assets"}, graph);
    v.field({"enabled", "Activo"}, enabled);
    // El Inspector los dibuja aparte (con el control del tipo de cada variable).
    if (v.wantsAllFields()) {
        ecs::listField(v, {"properties", "Variables"}, properties, [](scripting::ScriptProperty& p, ecs::PropertyVisitor& item) {
            item.field({"name", "Nombre"}, p.name);
            int type = static_cast<int>(p.type);
            item.field({"type", "Tipo"}, type, 0, 3);
            p.type = static_cast<scripting::PropertyType>(std::clamp(type, 0, 3));
            item.field({"value", "Valor"}, p.value);
        });
    }
}

void registerVisualScriptComponents() {
    ecs::ComponentRegistry& registry = ecs::ComponentRegistry::instance();
    if (registry.find("VisualScript") == nullptr) {
        registry.registerComponent<VisualScript>("VisualScript", "Visual Script (Blueprints)", "Scripting");
    }
}

}  // namespace cramion::vscript

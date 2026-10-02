#include "CramionCore/scripting/VisualScript.h"

// La definicion de ComponentRegistry::registerComponent<T>.
#include "CramionCore/ecs/World.h"
#include "CramionCore/ecs/Reflection.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

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

// Texto como literal de Lua ("...").
std::string luaQuote(const std::string& s) {
    std::string out = "\"";
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 32 || c == 127) {
                    char b[8];
                    std::snprintf(b, sizeof(b), "\\%03d", static_cast<int>(c));
                    out += b;
                } else {
                    out += ch;
                }
        }
    }
    out += "\"";
    return out;
}

bool isIdentifier(const std::string& s) {
    if (s.empty() || std::isdigit(static_cast<unsigned char>(s[0]))) return false;
    return std::all_of(s.begin(), s.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; });
}

// --- Catalogo ---
Pin X(const char* name) { return Pin{name, PinType::Exec, {}}; }
Pin P(const char* name, PinType type, const char* value = "") { return Pin{name, type, value}; }

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
          "input accion action enhanced", {X(""), P("Valor", kAny)}, {P("Accion", kString, "Jump"), P("Evento", kString, "triggered")});
    event("event.key", "Key Event", "Una tecla: pulsada, soltada o mantenida", "tecla key teclado keyboard",
          {X("")}, {P("Tecla", kString, "Space"), P("Cuando", kString, "pulsada")});

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
    for (const NodeInfo& n : nodeCatalog()) {
        if (n.kind == kind) return &n;
    }
    return nullptr;
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
    if (node.kind == "comment") return node.comment.empty() ? std::string("Comentario") : node.comment;
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
    if (category == "Matematicas" || category == "Logica" || category == "Vector" || category == "Texto") {
        return core::Vec3{0.20f, 0.45f, 0.30f};
    }
    if (category == "Objeto") return core::Vec3{0.18f, 0.35f, 0.62f};
    if (category == "Entrada" || category == "Tiempo") return core::Vec3{0.55f, 0.40f, 0.12f};
    if (category == "Depuracion") return core::Vec3{0.30f, 0.30f, 0.32f};
    return core::Vec3{0.28f, 0.28f, 0.30f};
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

// --- Compilar ---------------------------------------------------------------------------

int CompileResult::nodeAtLine(int line) const {
    if (line <= 0 || line > static_cast<int>(line_nodes.size())) return 0;
    return line_nodes[line - 1];
}

namespace {

class Compiler {
public:
    Compiler(const Graph& graph, const std::string& chunk) : g_(graph), chunk_(chunk) {}

    CompileResult run() {
        index();
        validate();
        latent_ = false;
        timers_ = false;
        for (const Node& n : g_.nodes) {
            const NodeInfo* info = findNodeInfo(n.kind);
            if (info != nullptr && info->latent) latent_ = true;
            if (n.kind == "flow.set_timer") timers_ = true;
        }
        prelude();
        for (const Node& n : g_.nodes) {
            if (n.kind == "comment" || isPure(n)) continue;
            if (isEventKind(n.kind)) {
                eventFunction(n);
                continue;
            }
            for (std::size_t i = 0; i < n.inputs.size(); ++i) {
                if (n.inputs[i].type == PinType::Exec) execFunction(n, static_cast<int>(i));
            }
        }
        methods();
        current_ = 0;
        emit("return G");

        CompileResult r;
        r.errors = std::move(errors_);
        r.ok = r.errors.empty();
        std::string text;
        for (const std::string& line : lines_) {
            text += line;
            text += '\n';
        }
        r.lua = std::move(text);
        r.line_nodes = std::move(line_nodes_);
        return r;
    }

private:
    const Graph& g_;
    std::string chunk_;
    std::vector<std::string> lines_;
    std::vector<int> line_nodes_;
    std::vector<NodeError> errors_;
    std::map<int, const Node*> by_id_;
    std::map<std::pair<int, int>, std::vector<const Link*>> in_links_;   // (nodo, entrada)
    std::map<std::pair<int, int>, std::vector<const Link*>> out_links_;  // (nodo, salida)
    std::set<int> visiting_;
    std::set<std::pair<int, std::string>> reported_;
    int current_ = 0;
    int depth_ = 0;
    bool latent_ = false;
    bool timers_ = false;

    void emit(const std::string& line) {
        lines_.push_back(std::string(static_cast<std::size_t>(depth_) * 2, ' ') + line);
        line_nodes_.push_back(current_);
    }
    void emitBlock(const char* text) {
        std::istringstream in(text);
        for (std::string line; std::getline(in, line);) {
            lines_.push_back(line);
            line_nodes_.push_back(current_);
        }
    }
    void error(int node, const std::string& message) {
        if (!reported_.insert({node, message}).second) return;
        errors_.push_back(NodeError{node, message});
    }

    void index() {
        for (const Node& n : g_.nodes) by_id_[n.id] = &n;
        for (const Link& l : g_.links) {
            in_links_[{l.to_node, l.to_pin}].push_back(&l);
            out_links_[{l.from_node, l.from_pin}].push_back(&l);
        }
    }

    static bool isPure(const Node& n) {
        if (n.kind == "call") return n.pure;
        if (n.kind == "call.get" || n.kind == "comment") return true;
        if (n.kind == "call.set") return false;
        const NodeInfo* info = findNodeInfo(n.kind);
        return info != nullptr && info->pure;
    }

    PinType pinType(const Node& n, bool output, int index) const {
        const std::vector<Pin>& pins = output ? n.outputs : n.inputs;
        if (index < 0 || index >= static_cast<int>(pins.size())) return PinType::Any;
        const PinType t = pins[index].type;
        if ((n.kind == "var.get" || n.kind == "var.set") && t != PinType::Exec) {
            if (const Variable* v = g_.findVariable(n.fn)) return v->type;
        }
        return t;
    }

    void validate() {
        std::set<std::string> names;
        for (const Variable& v : g_.variables) {
            if (v.name.empty()) error(0, "Una variable no tiene nombre");
            else if (!names.insert(v.name).second) error(0, "Hay dos variables \"" + v.name + "\"");
            else if (reservedMethod(v.name) || v.name == "entity") error(0, "La variable \"" + v.name + "\" usa un nombre reservado");
        }
        std::set<std::string> events;
        std::set<std::string> custom_names;
        for (const Node& n : g_.nodes) {
            if (n.kind == "event.custom" && !n.inputs.empty()) custom_names.insert(trim(n.inputs[0].value));
        }
        for (const Node& n : g_.nodes) {
            const NodeInfo* info = findNodeInfo(n.kind);
            const bool call = n.kind == "call" || n.kind == "call.get" || n.kind == "call.set";
            if (info == nullptr && !call) {
                error(n.id, "Tipo de nodo desconocido: " + n.kind);
                continue;
            }
            if ((n.kind == "var.get" || n.kind == "var.set") && g_.findVariable(n.fn) == nullptr) {
                error(n.id, n.fn.empty() ? std::string("Elige una variable") : "No existe la variable \"" + n.fn + "\"");
            }
            if (call && !validCallee(n.fn)) error(n.id, "Funcion no valida: \"" + n.fn + "\"");
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
                error(l.to_node, std::string("No se puede enlazar ") + pinTypeLabel(from) + " con " + pinTypeLabel(to) + " (" +
                                     b->second->inputs[l.to_pin].name + ")");
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

    // --- Expresiones ---
    std::string slot(int node, int pin) const {
        return "self.__o[\"" + std::to_string(node) + ":" + std::to_string(pin) + "\"]";
    }

    std::string literal(const Node& n, const Pin& pin, PinType type) {
        const std::string v = trim(pin.value);
        double number = 0.0;
        switch (type) {
            case PinType::Exec: return "nil";
            case PinType::Bool: {
                const std::string l = lower(v);
                return l == "true" || l == "1" || l == "si" || l == "yes" ? "true" : "false";
            }
            case PinType::Int:
                if (v.empty()) return "0";
                if (!parseNumber(v, number)) {
                    error(n.id, "\"" + pin.name + "\": \"" + v + "\" no es un numero");
                    return "0";
                }
                return formatNumber(std::round(number));
            case PinType::Float:
                if (v.empty()) return "0";
                if (!parseNumber(v, number)) {
                    error(n.id, "\"" + pin.name + "\": \"" + v + "\" no es un numero");
                    return "0";
                }
                return formatNumber(number);
            case PinType::String: return luaQuote(pin.value);
            case PinType::Vector: {
                const std::vector<double> c = numbersIn(v);
                if (c.size() == 3) return "Vec3(" + formatNumber(c[0]) + ", " + formatNumber(c[1]) + ", " + formatNumber(c[2]) + ")";
                if (c.size() == 1) return "Vec3(" + formatNumber(c[0]) + ", " + formatNumber(c[0]) + ", " + formatNumber(c[0]) + ")";
                return "Vec3(0, 0, 0)";
            }
            case PinType::Entity: return v.empty() ? std::string("self.entity") : "Scene.find(" + luaQuote(v) + ")";
            case PinType::Any: {
                if (v.empty()) return "nil";
                if (parseNumber(v, number)) return formatNumber(number);
                const std::string l = lower(v);
                if (l == "true" || l == "false") return l;
                if (l == "nil") return "nil";
                const std::vector<double> c = numbersIn(v);
                if (c.size() == 3 && v.find_first_not_of("0123456789.-+eE ,()Vvec") == std::string::npos) {
                    return "Vec3(" + formatNumber(c[0]) + ", " + formatNumber(c[1]) + ", " + formatNumber(c[2]) + ")";
                }
                return luaQuote(pin.value);
            }
        }
        return "nil";
    }

    // Expresion de una entrada de datos de `n`.
    std::string in(const Node& n, int index) {
        if (index < 0 || index >= static_cast<int>(n.inputs.size())) return "nil";
        const PinType want = pinType(n, false, index);
        const auto it = in_links_.find({n.id, index});
        if (it == in_links_.end() || it->second.empty()) return literal(n, n.inputs[index], want);
        const Link* l = it->second.front();
        const auto src = by_id_.find(l->from_node);
        if (src == by_id_.end() || l->from_pin < 0 || l->from_pin >= static_cast<int>(src->second->outputs.size())) return "nil";
        std::string e = output(*src->second, l->from_pin);
        const PinType have = pinType(*src->second, true, l->from_pin);
        if (want == PinType::String && have != PinType::String && have != PinType::Any) e = "STR(" + e + ")";
        return e;
    }

    std::string output(const Node& n, int pin) {
        if (!isPure(n)) return slot(n.id, pin);
        if (visiting_.contains(n.id)) {
            error(n.id, "Ciclo entre nodos de datos (un valor depende de si mismo)");
            return "nil";
        }
        visiting_.insert(n.id);
        std::string e = pure(n, pin);
        visiting_.erase(n.id);
        return "W(self, \"" + std::to_string(n.id) + ":" + std::to_string(pin) + "\", " + e + ")";
    }

    std::string callArgs(const Node& n, const std::vector<int>& data, std::size_t first) {
        std::vector<std::string> args;
        for (std::size_t k = first; k < data.size(); ++k) args.push_back(in(n, data[k]));
        while (!args.empty() && args.back() == "nil") args.pop_back();
        std::string out;
        for (std::size_t k = 0; k < args.size(); ++k) out += (k ? ", " : "") + args[k];
        return out;
    }

    std::vector<int> dataInputs(const Node& n) const {
        std::vector<int> data;
        for (std::size_t i = 0; i < n.inputs.size(); ++i) {
            if (n.inputs[i].type != PinType::Exec) data.push_back(static_cast<int>(i));
        }
        return data;
    }

    std::string callExpr(const Node& n) {
        const std::vector<int> data = dataInputs(n);
        const std::size_t colon = n.fn.find(':');
        if (colon != std::string::npos) {
            const std::string target = data.empty() ? std::string("self.entity") : in(n, data[0]);
            return "(" + target + "):" + n.fn.substr(colon + 1) + "(" + callArgs(n, data, data.empty() ? 0 : 1) + ")";
        }
        return n.fn + "(" + callArgs(n, data, 0) + ")";
    }

    std::string propertyTarget(const Node& n, std::string& member) {
        const std::size_t colon = n.fn.find(':');
        if (colon == std::string::npos) {
            member.clear();
            return n.fn;
        }
        member = n.fn.substr(colon + 1);
        const std::vector<int> data = dataInputs(n);
        return data.empty() ? std::string("self.entity") : in(n, data[0]);
    }

    std::string pure(const Node& n, int pin) {
        const std::string& k = n.kind;
        const auto a = [&](int i) { return in(n, i); };
        if (k == "var.get") return "self[" + luaQuote(n.fn) + "]";
        if (k == "call") return callExpr(n);
        if (k == "call.get") {
            std::string member;
            const std::string target = propertyTarget(n, member);
            return member.empty() ? target : "(" + target + ")." + member;
        }
        if (k == "math.add") return "(" + a(0) + " + " + a(1) + ")";
        if (k == "math.sub") return "(" + a(0) + " - " + a(1) + ")";
        if (k == "math.mul") return "(" + a(0) + " * " + a(1) + ")";
        if (k == "math.div") return "(" + a(0) + " / " + a(1) + ")";
        if (k == "math.mod") return "(" + a(0) + " % " + a(1) + ")";
        if (k == "math.pow") return "(" + a(0) + " ^ " + a(1) + ")";
        if (k == "math.min") return "math.min(" + a(0) + ", " + a(1) + ")";
        if (k == "math.max") return "math.max(" + a(0) + ", " + a(1) + ")";
        if (k == "math.clamp") return "math.min(math.max(" + a(0) + ", " + a(1) + "), " + a(2) + ")";
        if (k == "math.lerp") return "LERP(" + a(0) + ", " + a(1) + ", " + a(2) + ")";
        if (k == "math.map_range") {
            return "MAP(" + a(0) + ", " + a(1) + ", " + a(2) + ", " + a(3) + ", " + a(4) + ")";
        }
        if (k == "math.abs") return "math.abs(" + a(0) + ")";
        if (k == "math.sqrt") return "math.sqrt(" + a(0) + ")";
        if (k == "math.floor") return "math.floor(" + a(0) + ")";
        if (k == "math.ceil") return "math.ceil(" + a(0) + ")";
        if (k == "math.round") return "math.floor(" + a(0) + " + 0.5)";
        if (k == "math.sin") return "math.sin(" + a(0) + ")";
        if (k == "math.cos") return "math.cos(" + a(0) + ")";
        if (k == "math.tan") return "math.tan(" + a(0) + ")";
        if (k == "math.negate") return "(-" + a(0) + ")";
        if (k == "math.atan2") return "math.atan(" + a(0) + ", " + a(1) + ")";
        if (k == "math.random_float") return "RANDF(" + a(0) + ", " + a(1) + ")";
        if (k == "math.random_int") return "RANDI(" + a(0) + ", " + a(1) + ")";
        if (k == "math.pi") return "math.pi";
        if (k == "logic.and") return "(" + a(0) + " and " + a(1) + ")";
        if (k == "logic.or") return "(" + a(0) + " or " + a(1) + ")";
        if (k == "logic.xor") return "((not " + a(0) + ") ~= (not " + a(1) + "))";
        if (k == "logic.not") return "(not " + a(0) + ")";
        if (k == "logic.equal") return "(" + a(0) + " == " + a(1) + ")";
        if (k == "logic.not_equal") return "(" + a(0) + " ~= " + a(1) + ")";
        if (k == "logic.greater") return "(" + a(0) + " > " + a(1) + ")";
        if (k == "logic.less") return "(" + a(0) + " < " + a(1) + ")";
        if (k == "logic.greater_equal") return "(" + a(0) + " >= " + a(1) + ")";
        if (k == "logic.less_equal") return "(" + a(0) + " <= " + a(1) + ")";
        if (k == "logic.select") return "SEL(" + a(0) + ", " + a(1) + ", " + a(2) + ")";
        if (k == "vec.make") return "Vec3(" + a(0) + ", " + a(1) + ", " + a(2) + ")";
        if (k == "vec.break") return "(" + a(0) + ")." + std::string(pin == 0 ? "x" : (pin == 1 ? "y" : "z"));
        if (k == "vec.length") return "(" + a(0) + "):length()";
        if (k == "vec.normalize") return "(" + a(0) + "):normalized()";
        if (k == "vec.distance") return "Vec3.distance(" + a(0) + ", " + a(1) + ")";
        if (k == "vec.dot") return "Vec3.dot(" + a(0) + ", " + a(1) + ")";
        if (k == "vec.cross") return "Vec3.cross(" + a(0) + ", " + a(1) + ")";
        if (k == "vec.lerp") return "Vec3.lerp(" + a(0) + ", " + a(1) + ", " + a(2) + ")";
        if (k == "str.append") return "(STR(" + a(0) + ") .. STR(" + a(1) + "))";
        if (k == "str.to_string") return "STR(" + a(0) + ")";
        if (k == "str.to_number") return "(tonumber(" + a(0) + ") or 0)";
        if (k == "str.length") return "#STR(" + a(0) + ")";
        if (k == "str.contains") return "(string.find(STR(" + a(0) + "), STR(" + a(1) + "), 1, true) ~= nil)";
        if (k == "entity.self") return "self.entity";
        if (k == "entity.find") return "Scene.find(" + a(0) + ")";
        if (k == "entity.find_tag") return "Scene.findWithTag(" + a(0) + ")";
        if (k == "entity.get_position") return "(" + a(0) + ").position";
        if (k == "entity.get_rotation") return "(" + a(0) + ").rotation";
        if (k == "entity.get_scale") return "(" + a(0) + ").scale";
        if (k == "entity.get_forward") return "(" + a(0) + ").forward";
        if (k == "entity.get_name") return "(" + a(0) + ").name";
        if (k == "entity.is_valid") return "VALID(" + a(0) + ")";
        if (k == "entity.distance") return "(" + a(0) + "):distanceTo(" + a(1) + ")";
        if (k == "entity.get_field") return "(" + a(0) + "):getField(" + a(1) + ", " + a(2) + ")";
        if (k == "input.key") return "Input.getKey(" + a(0) + ")";
        if (k == "input.key_down") return "Input.getKeyDown(" + a(0) + ")";
        if (k == "input.key_up") return "Input.getKeyUp(" + a(0) + ")";
        if (k == "input.axis") return "Input.getAxis(" + a(0) + ")";
        if (k == "input.mouse_button") return "Input.getMouseButton(" + a(0) + ")";
        if (k == "input.action") return "Input.getAction(" + a(0) + ")";
        if (k == "time.delta") return "(Time.deltaTime or 0)";
        if (k == "time.time") return "(Time.time or 0)";
        if (k == "time.frame") return "(Time.frameCount or 0)";
        if (k == "util.reroute") return a(0);
        error(n.id, "El nodo " + nodeTitle(n) + " no da valores");
        return "nil";
    }

    // --- Ejecucion ---
    // Sentencia que sigue por la salida exec `out` (vacia si no hay enlace).
    std::string next(const Node& n, int out, bool tail) {
        const auto it = out_links_.find({n.id, out});
        if (it == out_links_.end() || it->second.empty()) return {};
        const Link* l = it->second.front();
        const auto target = by_id_.find(l->to_node);
        if (target == by_id_.end()) return {};
        const std::string call = "N[\"" + std::to_string(l->to_node) + "_" + std::to_string(l->to_pin) + "\"](self)";
        return tail ? "return " + call : call;
    }
    void emitNext(const Node& n, int out, bool tail) {
        const std::string s = next(n, out, tail);
        if (!s.empty()) emit(s);
    }

    void eventFunction(const Node& n) {
        current_ = n.id;
        emit("N[\"" + std::to_string(n.id) + "_e\"] = function(self, a, b)");
        ++depth_;
        emit("T(self, " + std::to_string(n.id) + ")");
        const std::string& k = n.kind;
        if (n.outputs.size() > 1 && k.rfind("event.collision", 0) == 0) {
            emit(slot(n.id, 1) + " = a");
            emit(slot(n.id, 2) + " = b and b.point");
            emit(slot(n.id, 3) + " = b and b.normal");
            emit(slot(n.id, 4) + " = b and b.relativeVelocity");
        } else if (n.outputs.size() > 1) {
            emit(slot(n.id, 1) + " = a");
        }
        emitNext(n, 0, true);
        --depth_;
        emit("end");
    }

    void execFunction(const Node& n, int input) {
        current_ = n.id;
        const std::string id = std::to_string(n.id);
        emit("N[\"" + id + "_" + std::to_string(input) + "\"] = function(self)");
        ++depth_;
        emit("T(self, " + id + ")");
        const std::string& k = n.kind;
        const std::string state = "self.__s[" + id + "]";
        if (k == "flow.branch") {
            emit("if " + in(n, 1) + " then");
            ++depth_;
            emitNext(n, 0, true);
            --depth_;
            emit("else");
            ++depth_;
            emitNext(n, 1, true);
            --depth_;
            emit("end");
        } else if (k == "flow.sequence") {
            for (std::size_t o = 0; o < n.outputs.size(); ++o) emitNext(n, static_cast<int>(o), o + 1 == n.outputs.size());
        } else if (k == "flow.for") {
            emit("local first, last = math.floor(tonumber(" + in(n, 1) + ") or 0), math.floor(tonumber(" + in(n, 2) + ") or 0)");
            emit("for i = first, last do");
            ++depth_;
            emit(slot(n.id, 1) + " = i");
            emitNext(n, 0, false);
            --depth_;
            emit("end");
            emitNext(n, 2, true);
        } else if (k == "flow.foreach") {
            emit("local list = " + in(n, 1));
            emit("if list ~= nil then");
            ++depth_;
            emit("for i = 1, #list do");
            ++depth_;
            emit(slot(n.id, 1) + " = list[i]");
            emit(slot(n.id, 2) + " = i");
            emitNext(n, 0, false);
            --depth_;
            emit("end");
            --depth_;
            emit("end");
            emitNext(n, 3, true);
        } else if (k == "flow.while") {
            emit("local limit = tonumber(" + in(n, 2) + ") or 10000");
            emit("local count = 0");
            emit("while " + in(n, 1) + " do");
            ++depth_;
            emit("count = count + 1");
            emit("if count > limit then error(\"While Loop: mas de \" .. limit .. \" vueltas (bucle infinito?)\") end");
            emitNext(n, 0, false);
            --depth_;
            emit("end");
            emitNext(n, 1, true);
        } else if (k == "flow.delay") {
            emit("coroutine.yield(tonumber(" + in(n, 1) + ") or 0)");
            emitNext(n, 0, true);
        } else if (k == "flow.do_once") {
            if (input == 0) {
                emit("if " + state + " then return end");
                emit(state + " = true");
                emitNext(n, 0, true);
            } else {
                emit(state + " = nil");
            }
        } else if (k == "flow.do_n") {
            if (input == 0) {
                emit("local count = " + state + " or 0");
                emit("if count >= (tonumber(" + in(n, 2) + ") or 0) then return end");
                emit("count = count + 1");
                emit(state + " = count");
                emit(slot(n.id, 1) + " = count");
                emitNext(n, 0, true);
            } else {
                emit(state + " = 0");
            }
        } else if (k == "flow.flip_flop") {
            emit("local isA = not " + state);
            emit(state + " = isA");
            emit(slot(n.id, 2) + " = isA");
            emit("if isA then");
            ++depth_;
            emitNext(n, 0, true);
            --depth_;
            emit("else");
            ++depth_;
            emitNext(n, 1, true);
            --depth_;
            emit("end");
        } else if (k == "flow.gate") {
            if (input == 0) {
                emit("if " + state + " then");
                ++depth_;
                emitNext(n, 0, true);
                --depth_;
                emit("end");
            } else if (input == 1) {
                emit(state + " = true");
            } else if (input == 2) {
                emit(state + " = false");
            } else {
                emit(state + " = not " + state);
            }
        } else if (k == "flow.set_timer") {
            emit("local seconds = tonumber(" + in(n, 2) + ") or 0");
            emit("self.__tm[STR(" + in(n, 1) + ")] = {t = seconds, every = (" + in(n, 3) + ") and seconds or nil}");
            emitNext(n, 0, true);
        } else if (k == "flow.clear_timer") {
            emit("self.__tm[STR(" + in(n, 1) + ")] = nil");
            emitNext(n, 0, true);
        } else if (k == "flow.call_event") {
            emit("local name = STR(" + in(n, 1) + ")");
            emit("local f = G[name]");
            emit("if f == nil then error(\"Call Event: no existe el evento \" .. name) end");
            emit("f(self, " + in(n, 2) + ")");
            emitNext(n, 0, true);
        } else if (k == "flow.send_event") {
            emit("local target = " + in(n, 1));
            emit("if target ~= nil and __VS_SEND ~= nil then __VS_SEND(target, STR(" + in(n, 2) + "), " + in(n, 3) + ") end");
            emitNext(n, 0, true);
        } else if (k == "var.set") {
            const std::string key = "self[" + luaQuote(n.fn) + "]";
            emit(key + " = " + in(n, 1));
            emit(slot(n.id, 1) + " = " + key);
            emitNext(n, 0, true);
        } else if (k == "debug.print" || k == "debug.warn") {
            emit(std::string(k == "debug.print" ? "Debug.log" : "Debug.warn") + "(STR(" + in(n, 1) + "))");
            emitNext(n, 0, true);
        } else if (k == "entity.set_position" || k == "entity.set_rotation" || k == "entity.set_scale" || k == "entity.set_active") {
            const char* field = k == "entity.set_position" ? "position"
                                : k == "entity.set_rotation" ? "rotation"
                                : k == "entity.set_scale"    ? "scale"
                                                             : "active";
            emit("local o = " + in(n, 1));
            emit("if o ~= nil then o." + std::string(field) + " = " + in(n, 2) + " end");
            emitNext(n, 0, true);
        } else if (k == "entity.translate" || k == "entity.rotate" || k == "entity.look_at") {
            const char* method = k == "entity.translate" ? "translate" : (k == "entity.rotate" ? "rotate" : "lookAt");
            emit("local o = " + in(n, 1));
            emit("if o ~= nil then o:" + std::string(method) + "(" + in(n, 2) + ") end");
            emitNext(n, 0, true);
        } else if (k == "entity.add_force") {
            emit("local o = " + in(n, 1));
            emit("if o ~= nil then o:addForce(" + in(n, 2) + ", " + in(n, 3) + ") end");
            emitNext(n, 0, true);
        } else if (k == "entity.spawn") {
            emit(slot(n.id, 1) + " = Scene.instantiate(" + in(n, 1) + ", " + in(n, 2) + ", " + in(n, 3) + ")");
            emitNext(n, 0, true);
        } else if (k == "entity.destroy") {
            emit("local o = " + in(n, 1));
            emit("if o ~= nil then o:destroy() end");
            emitNext(n, 0, true);
        } else if (k == "entity.set_field") {
            emit("local o = " + in(n, 1));
            emit("if o ~= nil then o:setField(" + in(n, 2) + ", " + in(n, 3) + ", " + in(n, 4) + ") end");
            emitNext(n, 0, true);
        } else if (k == "call") {
            const int result = n.output("Resultado");
            emit((result >= 0 ? slot(n.id, result) : std::string("local _")) + " = " + callExpr(n));
            emitNext(n, 0, true);
        } else if (k == "call.set") {
            std::string member;
            const std::string target = propertyTarget(n, member);
            const std::vector<int> data = dataInputs(n);
            const std::string value = data.empty() ? std::string("nil") : in(n, data.back());
            if (member.empty()) {
                emit(target + " = " + value);
            } else {
                emit("local o = " + target);
                emit("if o ~= nil then o." + member + " = " + value + " end");
            }
            emitNext(n, 0, true);
        } else {
            error(n.id, "El nodo " + nodeTitle(n) + " no se puede ejecutar");
        }
        --depth_;
        emit("end");
    }

    std::vector<const Node*> eventsOf(const char* kind) const {
        std::vector<const Node*> out;
        for (const Node& n : g_.nodes) {
            if (n.kind == kind) out.push_back(&n);
        }
        return out;
    }

    std::string eventCall(const Node& n, const std::string& args) const {
        return "RUN(self, N[\"" + std::to_string(n.id) + "_e\"]" + (args.empty() ? std::string() : ", " + args) + ")";
    }

    void methods() {
        // __vsinit (Awake y al recargar el grafo en Play): estado, variables y
        // nodos con estado inicial. Solo rellena lo que falta: al recargar,
        // las instancias siguen con sus datos.
        current_ = 0;
        emit("G.__vsinit = function(self)");
        ++depth_;
        emit("self.__o = self.__o or {}");
        emit("self.__t = self.__t or {}");
        emit("self.__s = self.__s or {}");
        emit("self.__p = self.__p or {}");
        emit("self.__tm = self.__tm or {}");
        emit("self.__b = self.__b or {}");
        for (const Variable& v : g_.variables) {
            if (v.name.empty()) continue;
            const std::string key = "self[" + luaQuote(v.name) + "]";
            if (v.type == PinType::Entity) {
                if (!trim(v.value).empty()) emit("if " + key + " == nil then " + key + " = " + luaQuote(trim(v.value)) + " end");
                emit("if type(" + key + ") == \"string\" then");
                ++depth_;
                emit("local name = " + key);
                emit(key + " = name ~= \"\" and Scene.find(name) or nil");
                --depth_;
                emit("end");
            } else {
                Node dummy;
                Pin pin{v.name, v.type, v.value};
                emit("if " + key + " == nil then " + key + " = " + literal(dummy, pin, v.type) + " end");
            }
        }
        for (const Node& n : g_.nodes) {
            current_ = n.id;
            const std::string state = "self.__s[" + std::to_string(n.id) + "]";
            if (n.kind == "flow.do_once") emit("if " + state + " == nil and " + in(n, 2) + " then " + state + " = true end");
            if (n.kind == "flow.gate") emit("if " + state + " == nil then " + state + " = not (" + in(n, 4) + ") end");
        }
        current_ = 0;
        --depth_;
        emit("end");
        emit("G.Awake = G.__vsinit");

        // Start: acciones de entrada y los Event Start.
        const auto actions = eventsOf("event.input_action");
        const auto starts = eventsOf("event.start");
        if (!actions.empty() || !starts.empty()) {
            emit("G.Start = function(self)");
            ++depth_;
            for (const Node* n : actions) {
                current_ = n->id;
                const std::string action = n->inputs.size() > 0 ? n->inputs[0].value : std::string("Jump");
                const std::string when = n->inputs.size() > 1 && !trim(n->inputs[1].value).empty() ? trim(n->inputs[1].value) : "triggered";
                emit("self.__b[#self.__b + 1] = Input.bindAction(" + luaQuote(action) + ", " + luaQuote(when) +
                     ", function(value) " + eventCall(*n, "value") + " end)");
            }
            for (const Node* n : starts) {
                current_ = n->id;
                emit(eventCall(*n, {}));
            }
            current_ = 0;
            --depth_;
            emit("end");
        }

        // Update: esperas, temporizadores, teclas y los Event Update.
        const auto keys = eventsOf("event.key");
        const auto updates = eventsOf("event.update");
        if (latent_ || timers_ || !keys.empty() || !updates.empty()) {
            emit("G.Update = function(self, dt)");
            ++depth_;
            if (latent_ || timers_) emit("TICK(self, dt)");
            for (const Node* n : keys) {
                current_ = n->id;
                const std::string key = n->inputs.size() > 0 ? n->inputs[0].value : std::string("Space");
                const std::string when = n->inputs.size() > 1 ? lower(trim(n->inputs[1].value)) : std::string("pulsada");
                const char* fn = when == "soltada" || when == "released" || when == "up" ? "Input.getKeyUp"
                                 : when == "mantenida" || when == "held" || when == "down"  ? "Input.getKey"
                                                                                            : "Input.getKeyDown";
                emit("if " + std::string(fn) + "(" + luaQuote(key) + ") then " + eventCall(*n, {}) + " end");
            }
            for (const Node* n : updates) {
                current_ = n->id;
                emit(eventCall(*n, "dt"));
            }
            current_ = 0;
            --depth_;
            emit("end");
        }
        const auto simple = [&](const char* kind, const char* method, const char* params, const char* args) {
            const auto list = eventsOf(kind);
            if (list.empty()) return;
            emit(std::string("G.") + method + " = function(self" + params + ")");
            ++depth_;
            for (const Node* n : list) {
                current_ = n->id;
                emit(eventCall(*n, args));
            }
            current_ = 0;
            --depth_;
            emit("end");
        };
        simple("event.late_update", "LateUpdate", ", dt", "dt");
        simple("event.fixed_update", "FixedUpdate", ", step", "step");
        simple("event.collision_enter", "OnCollisionEnter", ", other, contact", "other, contact");
        simple("event.collision_stay", "OnCollisionStay", ", other, contact", "other, contact");
        simple("event.collision_exit", "OnCollisionExit", ", other, contact", "other, contact");
        simple("event.trigger_enter", "OnTriggerEnter", ", other, contact", "other, contact");
        simple("event.trigger_stay", "OnTriggerStay", ", other, contact", "other, contact");
        simple("event.trigger_exit", "OnTriggerExit", ", other, contact", "other, contact");

        // OnDestroy: suelta las acciones y los Event Destroy.
        const auto destroys = eventsOf("event.destroy");
        if (!actions.empty() || !destroys.empty()) {
            emit("G.OnDestroy = function(self)");
            ++depth_;
            if (!actions.empty()) emit("for _, id in ipairs(self.__b or {}) do Input.unbindAction(id) end");
            for (const Node* n : destroys) {
                current_ = n->id;
                emit(eventCall(*n, {}));
            }
            current_ = 0;
            --depth_;
            emit("end");
        }

        // Custom Events: metodos con su nombre (la interfaz, Send Event y los scripts los llaman).
        for (const Node* n : eventsOf("event.custom")) {
            current_ = n->id;
            const std::string name = n->inputs.empty() ? std::string() : trim(n->inputs[0].value);
            if (name.empty() || reservedMethod(name)) continue;
            emit("G[" + luaQuote(name) + "] = function(self, value) " + eventCall(*n, "value") + " end");
        }
        current_ = 0;
    }

    void prelude() {
        current_ = 0;
        emit("-- Generado por Cramion desde " + chunk_ + " (Visual Scripting). No editar: se rehace al guardar el grafo.");
        emit("local G = {}");
        emit("local N = {}");
        emit("local FILE = " + luaQuote(chunk_));
        emitBlock(R"(local function T(self, id)
  self.__t[id] = Time.time or 0
  local all = __VS_BP
  if all ~= nil then
    local bp = all[FILE]
    if bp ~= nil and bp[id] then __VS_BREAK(FILE, id, self.entity) end
  end
end
local function W(self, k, v)
  self.__o[k] = v
  return v
end
local function SEL(c, a, b)
  if c then return a end
  return b
end
local function STR(v)
  if v == nil then return "" end
  return tostring(v)
end
local function VALID(o)
  return o ~= nil and o:valid()
end
local function LERP(a, b, t)
  return a + (b - a) * t
end
local function MAP(v, a, b, c, d)
  if b == a then return c end
  return c + (v - a) / (b - a) * (d - c)
end
local function RANDF(a, b)
  return a + math.random() * (b - a)
end
local function RANDI(a, b)
  a, b = math.floor(a), math.floor(b)
  if b < a then a, b = b, a end
  return math.random(a, b)
end)");
        if (latent_) {
            emitBlock(R"(local function RUN(self, f, ...)
  local co = coroutine.create(f)
  local ok, wait = coroutine.resume(co, self, ...)
  if not ok then error(wait, 0) end
  if coroutine.status(co) ~= "dead" then
    local p = self.__p
    p[#p + 1] = {co = co, t = tonumber(wait) or 0}
  end
end)");
        } else {
            emitBlock(R"(local function RUN(self, f, ...)
  return f(self, ...)
end)");
        }
        emitBlock(R"(local function TICK(self, dt)
  local pending = self.__p
  if #pending > 0 then
    self.__p = {}
    for i = 1, #pending do
      local w = pending[i]
      w.t = w.t - dt
      if w.t > 0 then
        self.__p[#self.__p + 1] = w
      else
        local ok, wait = coroutine.resume(w.co)
        if not ok then error(wait, 0) end
        if coroutine.status(w.co) ~= "dead" then
          w.t = tonumber(wait) or 0
          self.__p[#self.__p + 1] = w
        end
      end
    end
  end
  local fired = nil
  for name, tm in pairs(self.__tm) do
    tm.t = tm.t - dt
    if tm.t <= 0 then
      fired = fired or {}
      fired[#fired + 1] = name
      if tm.every ~= nil and tm.every > 0 then
        tm.t = tm.t + tm.every
      else
        self.__tm[name] = nil
      end
    end
  end
  if fired ~= nil then
    for _, name in ipairs(fired) do
      local f = G[name]
      if f ~= nil then f(self) end
    end
  end
end)");
    }
};

}  // namespace

CompileResult compileGraph(const Graph& graph, const std::string& chunk) {
    Compiler compiler(graph, chunk);
    return compiler.run();
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

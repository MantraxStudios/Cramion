// Debug.log / warn / error y print: escriben en la Consola.
// Debug.drawLine / drawRay / drawSphere / drawBox / drawArrow / drawText:
// dibujo de depuracion en el mundo (scripting/DebugDraw.h).

#include "Modules.h"

#include "CramionCore/ecs/MathUtil.h"
#include "CramionCore/scripting/DebugDraw.h"

#include <cctype>
#include <cstdlib>

namespace cramion::scripting::native {
namespace {

// Los argumentos como un texto, separados por espacios (Debug.log("a", 1) -> "a 1").
std::string joinArgs(const api::Call& c) {
    std::string text;
    for (std::size_t i = 0; i < c.count(); ++i) {
        if (i > 0) text += " ";
        text += c.arg(i).asString();
    }
    return text;
}

// Un color de la API: nil (blanco), Vec3 (r, g, b), lista {r, g, b, a},
// objeto {r=, g=, b=, a=} (de 0 a 1), un nombre ("rojo", "red"...) o "#RRGGBB[AA]".
std::uint32_t colorArg(const api::Call& c, std::size_t i) {
    const api::Value& v = c.arg(i);
    const auto num = [](const api::Value& x, double fallback) { return static_cast<float>(x.isNumber() ? x.asNumber() : fallback); };
    if (v.isVec3()) {
        const core::Vec3 rgb = v.asVec3();
        return debugdraw::packColor(rgb.x, rgb.y, rgb.z, 1.0f);
    }
    if (v.isArray()) {
        return debugdraw::packColor(num(v[0], 1.0), num(v[1], 1.0), num(v[2], 1.0), num(v[3], 1.0));
    }
    if (v.isObject()) {
        return debugdraw::packColor(num(v["r"], 1.0), num(v["g"], 1.0), num(v["b"], 1.0), num(v["a"], 1.0));
    }
    if (v.isString()) {
        std::string name = v.asString();
        for (char& ch : name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (!name.empty() && name[0] == '#' && (name.size() == 7 || name.size() == 9)) {
            const unsigned long hex = std::strtoul(name.c_str() + 1, nullptr, 16);
            const bool alpha = name.size() == 9;
            const unsigned long rgb = alpha ? hex >> 8 : hex;
            const float a = alpha ? static_cast<float>(hex & 0xFFu) / 255.0f : 1.0f;
            return debugdraw::packColor(static_cast<float>((rgb >> 16) & 0xFFu) / 255.0f,
                                        static_cast<float>((rgb >> 8) & 0xFFu) / 255.0f, static_cast<float>(rgb & 0xFFu) / 255.0f, a);
        }
        struct Named {
            const char* es;
            const char* en;
            float r, g, b;
        };
        static constexpr Named kNames[] = {
            {"blanco", "white", 1, 1, 1},          {"negro", "black", 0, 0, 0},
            {"rojo", "red", 1, 0.2f, 0.2f},        {"verde", "green", 0.2f, 1, 0.2f},
            {"azul", "blue", 0.25f, 0.45f, 1},     {"amarillo", "yellow", 1, 0.9f, 0.1f},
            {"cian", "cyan", 0.1f, 0.95f, 1},      {"magenta", "magenta", 1, 0.2f, 1},
            {"naranja", "orange", 1, 0.55f, 0.1f}, {"gris", "gray", 0.55f, 0.55f, 0.55f},
            {"gris", "grey", 0.55f, 0.55f, 0.55f},
        };
        for (const Named& n : kNames) {
            if (name == n.es || name == n.en) return debugdraw::packColor(n.r, n.g, n.b, 1.0f);
        }
        throw api::Error("color desconocido: \"" + v.asString() + "\" (usa Vec3, {r, g, b, a}, \"#RRGGBB\" o un nombre)");
    }
    if (!v.isNil()) throw api::Error("el color debe ser Vec3, {r, g, b, a}, \"#RRGGBB\" o un nombre");
    return 0xFFFFFFFFu;
}

// color, duracion y si se prueba la profundidad, desde el argumento `first`.
debugdraw::Style styleArgs(const api::Call& c, std::size_t first, bool depth_default = true) {
    debugdraw::Style s;
    s.color = colorArg(c, first);
    s.duration = static_cast<float>(c.number(first + 1, 0.0));
    s.depth_test = c.boolean(first + 2, depth_default);
    return s;
}

// Un giro: Quat, Vec3 en grados (como el Inspector) o nada.
core::Quat rotationArg(const api::Call& c, std::size_t i) {
    const api::Value& v = c.arg(i);
    if (v.isQuat()) return v.asQuat();
    if (v.isVec3()) return ecs::quatFromEulerDegrees(v.asVec3());
    if (!v.isNil()) throw api::Error("el giro debe ser Quat o Vec3 (grados)");
    return core::Quat{};
}

void registerDebugDraw(Runtime& rt) {
    // Coste cero sin recoger (juego exportado sin debug.draw.InGame): vuelve ya.
    const auto off = [] { return !debugdraw::shared().enabled(); };
    rt.native.function("Debug.drawLine", [off](api::Call& c) {
        if (off()) return api::Value{};
        debugdraw::shared().line(c.vec3(0), c.vec3(1), styleArgs(c, 2));
        return api::Value{};
    }, {"desde, hasta, color, duracion, profundidad",
        "Linea de depuracion en el mundo (Play del editor; en el juego con debug.draw.InGame). duracion 0 = un frame; "
        "profundidad false = se ve a traves de todo"});
    rt.native.function("Debug.drawRay", [off](api::Call& c) {
        if (off()) return api::Value{};
        debugdraw::shared().ray(c.vec3(0), c.vec3(1), styleArgs(c, 2));
        return api::Value{};
    }, {"origen, direccion, color, duracion, profundidad", "Rayo de depuracion: de origen a origen + direccion"});
    rt.native.function("Debug.drawSphere", [off](api::Call& c) {
        if (off()) return api::Value{};
        debugdraw::shared().sphere(c.vec3(0), static_cast<float>(c.number(1, 0.5)), styleArgs(c, 2));
        return api::Value{};
    }, {"centro, radio, color, duracion, profundidad", "Esfera de alambre de depuracion (tres circulos)"});
    rt.native.function("Debug.drawBox", [off](api::Call& c) {
        if (off()) return api::Value{};
        debugdraw::shared().box(c.vec3(0), c.vec3(1, core::Vec3{1.0f, 1.0f, 1.0f}), rotationArg(c, 2), styleArgs(c, 3));
        return api::Value{};
    }, {"centro, tamano, giro, color, duracion, profundidad",
        "Caja de alambre de depuracion (tamano entero; giro Quat o Vec3 en grados)"});
    rt.native.function("Debug.drawArrow", [off](api::Call& c) {
        if (off()) return api::Value{};
        debugdraw::shared().arrow(c.vec3(0), c.vec3(1), styleArgs(c, 2), static_cast<float>(c.number(5, -1.0)));
        return api::Value{};
    }, {"desde, hasta, color, duracion, profundidad, punta", "Flecha de depuracion (punta: largo en metros)"});
    rt.native.function("Debug.drawText", [off](api::Call& c) {
        if (off()) return api::Value{};
        // El texto, por defecto, se lee a traves de todo (profundidad false).
        debugdraw::shared().text(c.vec3(0), c.arg(1).asString(), styleArgs(c, 2, false), static_cast<float>(c.number(5, 14.0)));
        return api::Value{};
    }, {"posicion, texto, color, duracion, profundidad, tamano",
        "Texto de depuracion en el mundo (tamano en pixeles; por defecto se ve a traves de todo)"});
    rt.native.function("Debug.drawClear", [](api::Call&) {
        debugdraw::shared().clear();
        return api::Value{};
    }, {"", "Borra todo el dibujo de depuracion (tambien lo que tenia duracion)"});
    rt.native.property("Debug", "drawEnabled", [](api::Call&) { return api::Value(debugdraw::shared().enabled()); }, {},
                       {"", "Si el dibujo de depuracion se ve (Play del editor, o la CVar debug.draw.InGame)", "booleano"});
    // Lo dibujado envejece al empezar cada frame y se borra al parar.
    rt.onFrame(Phase::Begin, [](float dt) { debugdraw::shared().advance(dt); });
    rt.onStop([](bool) { debugdraw::shared().clear(); });
}

}  // namespace

void registerDebugApi(Runtime& rt) {
    rt.native.function("Debug.log", [&rt](api::Call& c) { rt.write(0, joinArgs(c)); return api::Value{}; },
                       {"...", "Escribe en la consola"});
    rt.native.function("Debug.warn", [&rt](api::Call& c) { rt.write(1, joinArgs(c)); return api::Value{}; },
                       {"...", "Escribe un aviso en la consola"});
    rt.native.function("Debug.error", [&rt](api::Call& c) { rt.write(2, joinArgs(c)); return api::Value{}; },
                       {"...", "Escribe un error en la consola"});
    rt.native.function("print", [&rt](api::Call& c) { rt.write(0, joinArgs(c)); return api::Value{}; },
                       {"...", "Como Debug.log"});
    registerDebugDraw(rt);
}

}  // namespace cramion::scripting::native

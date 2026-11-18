// Pruebas de native/DebugApi.cpp: Debug.log / warn / error y print, y el
// dibujo de depuracion (Debug.drawLine... -> scripting/DebugDraw.h y la letra
// de trazos del texto, CramionFX/vk/StrokeFont.h).

#include "ApiTest.h"

#include "CramionCore/cvar/CVar.h"
#include "CramionCore/scripting/DebugDraw.h"

#include <CramionFX/vk/StrokeFont.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

using namespace cramion;
using namespace cramion::apitest;

namespace {

// Un canal (0 r, 1 g, 2 b, 3 a) de un color RGBA8.
std::uint32_t channel(std::uint32_t color, int i) { return (color >> (8 * i)) & 0xFFu; }

void testDrawDisabled() {
    std::printf("Debug.draw* sin recoger (juego exportado sin debug.draw.InGame)\n");
    debugdraw::DebugDraw& dd = debugdraw::shared();
    dd.setEditor(false);
    dd.clear();
    ApiFixture t;
    check(!t.get("Debug.drawEnabled").truthy(), "Debug.drawEnabled es false fuera del editor y sin la CVar");
    t.call("Debug.drawLine", {Value(core::Vec3{0, 0, 0}), Value(core::Vec3{1, 0, 0})});
    t.call("Debug.drawSphere", {Value(core::Vec3{0, 0, 0}), Value(2.0)});
    t.call("Debug.drawText", {Value(core::Vec3{0, 0, 0}), Value("hola")});
    check(dd.empty(), "sin recoger no guarda nada (coste cero)");
    // Con la CVar del juego si.
    cvar::Registry::instance().set("debug.draw.InGame", "true");
    check(t.get("Debug.drawEnabled").truthy(), "con debug.draw.InGame se recoge en el juego");
    t.call("Debug.drawLine", {Value(core::Vec3{0, 0, 0}), Value(core::Vec3{1, 0, 0})});
    check(dd.lineCount() == 1, "y la linea se guarda");
    cvar::Registry::instance().find("debug.draw.InGame")->reset();
    dd.clear();
}

void testDrawPrimitives() {
    std::printf("Debug.drawLine / drawRay / drawSphere / drawBox / drawArrow / drawText\n");
    debugdraw::DebugDraw& dd = debugdraw::shared();
    dd.setEditor(true);
    dd.clear();
    ApiFixture t;
    check(t.get("Debug.drawEnabled").truthy(), "en el editor se recoge siempre");

    t.call("Debug.drawLine", {Value(core::Vec3{0, 0, 0}), Value(core::Vec3{1, 2, 3}), Value("rojo")});
    check(dd.lineCount() == 1, "drawLine: un segmento");
    gfx::OverlayGeometry g;
    dd.build(g);
    check(g.lines.size() == 2 && g.top_lines.empty(), "con profundidad (por defecto) va a lines");
    check(std::abs(g.lines[1].position.z - 3.0f) < 1e-6f, "los extremos son los pedidos");
    check(channel(g.lines[0].color, 0) == 255 && channel(g.lines[0].color, 1) < 80 && channel(g.lines[0].color, 3) == 255,
          "color por nombre (rojo, opaco)");
    check(g.occluded_alpha == 0.0f, "lo tapado no se ve (occluded_alpha 0, como Unity)");

    // Duracion 0: se ve un frame (el que viene tras dibujarse) y se va.
    t.frame();
    check(dd.lineCount() == 0, "duracion 0: se quita al empezar el frame siguiente a dibujarse");

    // Sin dibujarse aun no envejece (lo de fixedUpdate, antes del inicio del frame).
    t.call("Debug.drawLine", {Value(core::Vec3{0, 0, 0}), Value(core::Vec3{1, 0, 0})});
    t.frame();
    check(dd.lineCount() == 1, "lo que aun no se dibujo sigue tras el inicio del frame");
    g.clear();
    dd.build(g);
    t.frame();
    check(dd.lineCount() == 0, "y se va tras dibujarse una vez");

    // Con duracion: segundos del juego.
    t.call("Debug.drawRay", {Value(core::Vec3{0, 1, 0}), Value(core::Vec3{0, 0, -2}), Value(core::Vec3{0, 1, 0}), Value(0.5),
                             Value(false)});
    g.clear();
    dd.build(g);
    check(g.top_lines.size() == 2 && g.lines.empty(), "profundidad false: va a top_lines (a traves de todo)");
    check(!g.top_lines.empty() && std::abs(g.top_lines[1].position.z + 2.0f) < 1e-6f &&
              std::abs(g.top_lines[1].position.y - 1.0f) < 1e-6f,
          "drawRay: de origen a origen + direccion");
    check(!g.top_lines.empty() && channel(g.top_lines[0].color, 1) == 255 && channel(g.top_lines[0].color, 0) == 0,
          "color como Vec3");
    t.frame(0.2f);
    check(dd.lineCount() == 1, "duracion 0,5: sigue a los 0,2 s");
    t.frame(0.2f);
    t.frame(0.2f);
    check(dd.lineCount() == 0, "y se va pasado el tiempo");

    dd.clear();
    t.call("Debug.drawSphere",
           {Value(core::Vec3{5, 0, 0}), Value(2.0), Value(Value::Array{Value(0.0), Value(0.0), Value(1.0), Value(0.5)})});
    check(dd.lineCount() == 72, "drawSphere: tres circulos de 24 lados");
    g.clear();
    dd.build(g);
    bool on_sphere = !g.lines.empty();
    for (const gfx::OverlayVertex& v : g.lines) {
        const float r = core::length(v.position - core::Vec3{5, 0, 0});
        on_sphere = on_sphere && std::abs(r - 2.0f) < 1e-3f;
    }
    check(on_sphere, "todos sus puntos estan a 2 m del centro");
    check(!g.lines.empty() && channel(g.lines[0].color, 2) == 255 && channel(g.lines[0].color, 3) == 128,
          "color como lista {r, g, b, a} (con alfa)");

    dd.clear();
    t.call("Debug.drawBox", {Value(core::Vec3{0, 0, 0}), Value(core::Vec3{2, 4, 6}), Value(core::Vec3{0, 90, 0})});
    check(dd.lineCount() == 12, "drawBox: 12 aristas");
    g.clear();
    dd.build(g);
    float max_x = 0.0f, max_z = 0.0f;
    for (const gfx::OverlayVertex& v : g.lines) {
        max_x = std::max(max_x, std::abs(v.position.x));
        max_z = std::max(max_z, std::abs(v.position.z));
    }
    check(std::abs(max_x - 3.0f) < 1e-3f && std::abs(max_z - 1.0f) < 1e-3f, "girada 90 grados en Y (Vec3 en grados): x y z cambian");
    dd.clear();
    t.call("Debug.drawBox", {Value(core::Vec3{0, 0, 0}), Value(core::Vec3{1, 1, 1}), Value(core::Quat{}), Value("#00FF0080")});
    g.clear();
    dd.build(g);
    check(!g.lines.empty() && channel(g.lines[0].color, 1) == 255 && channel(g.lines[0].color, 3) == 128,
          "giro Quat y color \"#RRGGBBAA\"");

    dd.clear();
    t.call("Debug.drawArrow", {Value(core::Vec3{0, 0, 0}), Value(core::Vec3{0, 0, 4})});
    check(dd.lineCount() == 5, "drawArrow: la linea y cuatro trazos de punta");
    g.clear();
    dd.build(g);
    bool head_ok = g.lines.size() == 10;
    for (std::size_t i = 2; head_ok && i + 1 < g.lines.size(); i += 2) {
        head_ok = std::abs(g.lines[i].position.z - 4.0f) < 1e-4f && std::abs(g.lines[i + 1].position.z - 3.5f) < 1e-3f;
    }
    check(head_ok, "la punta sale del extremo hacia atras (0,5 m)");

    dd.clear();
    t.call("Debug.drawText",
           {Value(core::Vec3{1, 2, 3}), Value("Vida: 10"), Value("amarillo"), Value(1.0), Value::nil(), Value(20.0)});
    check(dd.textCount() == 1, "drawText: un texto");
    g.clear();
    dd.build(g);
    check(g.texts.size() == 1 && g.texts[0].text == "Vida: 10" && g.texts[0].on_top && std::abs(g.texts[0].size - 20.0f) < 1e-6f,
          "el texto, por defecto a traves de todo, con su tamano");
    check(!g.empty(), "la geometria no esta vacia solo con texto");

    // Errores: un color que no existe vuelve al script como error.
    bool threw = false;
    try {
        t.call("Debug.drawLine", {Value(core::Vec3{0, 0, 0}), Value(core::Vec3{1, 0, 0}), Value("purpurina")});
    } catch (const scripting::api::Error&) {
        threw = true;
    }
    check(threw, "color desconocido: error para el script");
    // Por el puente de C++ (lo que usa el SDK): el color como lista con alfa.
    dd.clear();
    const json r = t.bridge({{"fn", "Debug.drawLine"},
                             {"args", {json{{"$v", {0, 0, 0}}}, json{{"$v", {0, 5, 0}}}, json::array({1, 1, 0, 1}), 0, true}}});
    check(r["ok"] == true && dd.lineCount() == 1, "Debug.drawLine por el puente de los scripts de C++");

    // Al parar se borra todo.
    t.call("Debug.drawSphere", {Value(core::Vec3{0, 0, 0}), Value(1.0), Value("azul"), Value(100.0)});
    t.scripts.stop();
    check(dd.empty(), "al parar el juego se borra lo dibujado");
    dd.setEditor(false);
}

void testDrawCap() {
    std::printf("Tope de primitivas (debug.draw.MaxLines / MaxTexts)\n");
    debugdraw::DebugDraw& dd = debugdraw::shared();
    dd.setEditor(true);
    dd.clear();
    cvar::Registry::instance().set("debug.draw.MaxLines", "100");
    cvar::Registry::instance().set("debug.draw.MaxTexts", "2");
    debugdraw::Style s;
    for (int i = 0; i < 150; ++i) dd.line(core::Vec3{0, 0, 0}, core::Vec3{1, 0, 0}, s);
    check(dd.lineCount() == 100 && dd.dropped() == 50, "pasado el tope se descarta (100 guardadas, 50 fuera)");
    dd.sphere(core::Vec3{0, 0, 0}, 1.0f, s);
    check(dd.lineCount() == 100, "una esfera que no cabe entera no se dibuja a medias");
    for (int i = 0; i < 5; ++i) dd.text(core::Vec3{0, 0, 0}, "x", s);
    check(dd.textCount() == 2, "y el de textos");
    cvar::Registry::instance().find("debug.draw.MaxLines")->reset();
    cvar::Registry::instance().find("debug.draw.MaxTexts")->reset();
    dd.clear();
    dd.line(core::Vec3{std::nanf(""), 0, 0}, core::Vec3{1, 0, 0}, s);
    check(dd.empty(), "una linea con NaN no se guarda");
    // Lo que nunca se dibuja (sin vista) no se queda para siempre.
    debugdraw::Style d;
    d.duration = 0.5f;
    dd.line(core::Vec3{0, 0, 0}, core::Vec3{1, 0, 0}, d);
    dd.advance(1.0f);
    check(dd.lineCount() == 1, "sin dibujarse sigue un rato");
    dd.advance(1.0f);
    check(dd.empty(), "pero se tira si ninguna vista lo pinta");
    dd.setEditor(false);
}

void testStrokeFont() {
    std::printf("Letra de trazos (Debug.drawText)\n");
    // Todas las letras estan bien escritas: puntos de dos cifras dentro de la rejilla 4 x 8.
    bool glyphs_ok = true;
    int known = 0;
    for (int c = 32; c < 127; ++c) {
        const char* g = gfx::strokeGlyph(static_cast<char>(c));
        if (g == nullptr) continue;
        ++known;
        std::size_t run = 0;
        for (const char* p = g;; ++p) {
            if (*p == ' ' || *p == '\0') {
                glyphs_ok = glyphs_ok && run % 2 == 0 && run >= 4;  // al menos un segmento por polilinea
                run = 0;
                if (*p == '\0') break;
                continue;
            }
            const bool digit = std::isdigit(static_cast<unsigned char>(*p)) != 0;
            const int v = *p - '0';
            glyphs_ok = glyphs_ok && digit && v >= 0 && v <= (run % 2 == 0 ? 4 : 8);
            ++run;
        }
    }
    check(glyphs_ok, "todas las letras: puntos de dos cifras dentro de la rejilla");
    check(known >= 66, "cifras, A-Z y la puntuacion comun");
    check(gfx::strokeGlyph('a') == gfx::strokeGlyph('A'), "las minusculas salen en mayusculas");

    std::vector<gfx::StrokeSegment> segs;
    gfx::strokeText("I", 16.0f, segs);
    // 'I': tres trazos (arriba, palo y abajo); centrado y de 16 px de alto.
    float min_x = 1e9f, max_x = -1e9f, min_y = 1e9f, max_y = -1e9f;
    for (const auto& s : segs) {
        min_x = std::min({min_x, s.x0, s.x1});
        max_x = std::max({max_x, s.x0, s.x1});
        min_y = std::min({min_y, s.y0, s.y1});
        max_y = std::max({max_y, s.y0, s.y1});
    }
    check(segs.size() == 3, "'I' son tres trazos");
    check(std::abs(min_y + 8.0f) < 1e-4f && std::abs(max_y - 8.0f) < 1e-4f, "de 16 px de alto centrado en el punto");
    check(std::abs(min_x + max_x) < 1e-4f, "y centrado en x");
    segs.clear();
    const std::size_t accents = gfx::strokeText("\xC3\x81rbol \xC3\xB1u", 14.0f, segs);  // "Arbol nu" con tilde y enie
    std::vector<gfx::StrokeSegment> plain;
    gfx::strokeText("ARBOL NU", 14.0f, plain);
    check(accents == plain.size(), "tildes y enie se dibujan como la letra base");
    segs.clear();
    gfx::strokeText("A\nB", 8.0f, segs);
    float top = -1e9f, bottom = 1e9f;
    for (const auto& s : segs) {
        top = std::max({top, s.y0, s.y1});
        bottom = std::min({bottom, s.y0, s.y1});
    }
    check(std::abs(top - 10.0f) < 1e-4f && std::abs(bottom + 10.0f) < 1e-4f, "dos lineas (\\n) una debajo de otra, centradas");
    segs.clear();
    check(gfx::strokeText("", 14.0f, segs) == 0 && gfx::strokeText("x", 0.0f, segs) == 0, "texto vacio o alto 0: nada");
    segs.clear();
    gfx::strokeText(std::string(1000, 'W'), 14.0f, segs, 10);
    check(segs.size() == 40, "tope de letras por texto (10 W de 4 trazos)");
}

}  // namespace

int main() {
    std::printf("Debug\n");
    {
        ApiFixture t;
        t.call("Debug.log", {Value("hola"), Value(3)});
        check(!t.log.empty() && t.log.back().first == 0 && t.log.back().second == "hola 3", "Debug.log une los argumentos");
        t.call("Debug.warn", {Value("cuidado")});
        check(t.log.back().first == 1, "Debug.warn es un aviso");
        const json r = t.bridge({{"fn", "Debug.error"}, {"args", {"mal"}}});
        check(r["ok"] == true && t.log.back().first == 2 && t.log.back().second == "mal", "Debug.error por el puente");
        t.call("print", {Value(1.5)});
        check(t.log.back().second == "1.5", "print");
    }
    testDrawDisabled();
    testDrawPrimitives();
    testDrawCap();
    testStrokeFont();
    return finish();
}

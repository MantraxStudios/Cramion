#ifndef CRAMION_CORE_SCRIPTING_DEBUG_DRAW_H
#define CRAMION_CORE_SCRIPTING_DEBUG_DRAW_H

// Dibujo de depuracion desde los scripts (como Debug.DrawLine de Unity y
// DrawDebugLine de Unreal): lineas, rayos, esferas, cajas giradas, flechas y
// texto en el mundo, con color, duracion (0 = un frame) y si se ven a traves
// de la geometria. La API (Debug.drawLine... en Visual Scripts, consola y el
// SDK de C++) los deja aqui; el editor (en Play: vistas de Escena y Juego) y
// CramionPlayer (con la CVar debug.draw.InGame) los pasan cada frame al
// renderizador como gfx::OverlayGeometry (VulkanRenderer::setDebugGeometry),
// el mismo pase de lineas con prueba de profundidad de los gizmos.
//
// Coste cero si no se usa: sin recoger (fuera del editor y sin la CVar) cada
// llamada vuelve sin hacer nada, y sin nada guardado el editor y el juego no
// tocan el renderizador. Tope de segmentos y textos vivos (debug.draw.MaxLines,
// debug.draw.MaxTexts): lo que pase se descarta (con un aviso por partida).
//
// Vida de lo dibujado: advance(dt) al empezar cada frame del juego (lo hace
// el ScriptSystem) descuenta la duracion de lo que ya se dibujo al menos una
// vez y quita lo que se acabo; build() lo pasa a la geometria y lo marca como
// dibujado. Asi, con duracion 0 se ve exactamente un frame, tambien si se
// dibuja en fixedUpdate (antes del inicio del frame).

#include <CramionFX/core/Math.h>
#include <CramionFX/vk/OverlayGeometry.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cramion::debugdraw {

// Color empaquetado RGBA8 (r | g<<8 | b<<16 | a<<24, como gfx::OverlayVertex).
std::uint32_t packColor(float r, float g, float b, float a = 1.0f);

struct Style {
    std::uint32_t color = 0xFFFFFFFFu;  // blanco
    float duration = 0.0f;              // segundos de juego; 0 = un frame
    bool depth_test = true;             // false: se ve a traves de la geometria
};

class DebugDraw {
public:
    // Si se recoge lo que piden los scripts: en el editor siempre (setEditor)
    // y en el juego con la CVar debug.draw.InGame.
    bool enabled() const;
    void setEditor(bool on) { editor_ = on; }

    void line(const core::Vec3& a, const core::Vec3& b, const Style& s);
    void ray(const core::Vec3& origin, const core::Vec3& direction, const Style& s);
    // Tres circulos (XY, XZ, YZ) de `segments` lados.
    void sphere(const core::Vec3& center, float radius, const Style& s, int segments = 24);
    // Caja de lado `size` (tamano entero, como Gizmos.DrawWireCube) girada por `rotation`.
    void box(const core::Vec3& center, const core::Vec3& size, const core::Quat& rotation, const Style& s);
    // Linea con punta (head: largo de la punta en metros; <= 0, un 25 % del largo hasta 0,5 m).
    void arrow(const core::Vec3& from, const core::Vec3& to, const Style& s, float head = -1.0f);
    void circle(const core::Vec3& center, const core::Vec3& normal, float radius, const Style& s, int segments = 24);
    // Texto en el mundo, centrado en `position`, de `size` pixeles de alto.
    void text(const core::Vec3& position, std::string_view text, const Style& s, float size = 14.0f);

    // Inicio de un frame del juego: descuenta `dt` a lo ya dibujado y quita lo acabado.
    void advance(float dt);
    // Lo vivo a la geometria del renderizador (lo marca como dibujado). Con
    // depth_test va a lines (lo tapado no se ve: occluded_alpha 0); sin el, a top_lines.
    void build(gfx::OverlayGeometry& out);
    void clear();

    bool empty() const { return segments_.empty() && texts_.empty(); }
    std::size_t lineCount() const { return segments_.size(); }
    std::size_t textCount() const { return texts_.size(); }
    // Lo descartado por el tope desde el ultimo clear().
    std::size_t dropped() const { return dropped_; }

private:
    struct Segment {
        core::Vec3 a, b;
        std::uint32_t color;
        float remaining;  // segundos que le quedan (se descuentan una vez dibujado)
        float age;        // segundos sin dibujarse (sin vista que lo pinte se tira)
        bool depth_test;
        bool drawn;
    };
    struct Text {
        core::Vec3 position;
        std::string text;
        std::uint32_t color;
        float size;
        float remaining;
        float age;
        bool depth_test;
        bool drawn;
    };
    bool roomFor(std::size_t segments);
    void push(const core::Vec3& a, const core::Vec3& b, const Style& s);

    std::vector<Segment> segments_;
    std::vector<Text> texts_;
    std::size_t dropped_ = 0;
    bool warned_ = false;
    bool editor_ = false;
};

// El de este proceso (lo comparten la API y quien dibuja).
DebugDraw& shared();

}  // namespace cramion::debugdraw

#endif  // CRAMION_CORE_SCRIPTING_DEBUG_DRAW_H

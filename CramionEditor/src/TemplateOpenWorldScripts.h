#ifndef CRAMION_EDITOR_TEMPLATE_OPENWORLD_SCRIPTS_H
#define CRAMION_EDITOR_TEMPLATE_OPENWORLD_SCRIPTS_H

// Scripts de C++ de la plantilla "Mundo abierto" (ProjectTemplates.cpp).

#include "TemplateScripts.h"

namespace cramion::editor::openworld {

inline constexpr TemplateFile kFiles[] = {
    {"Rendimiento.cpp",
R"CPP(// Rendimiento.cpp: prueba de rendimiento del mundo abierto.
// Muestra FPS (media, minimo y maximo de los ultimos segundos), ms de CPU y
// de GPU, arboles dibujados y triangulos. Teclas:
//   1 2 3 4   calidad Baja / Media / Alta / Ultra
//   N / M     menos / mas arboles (la densidad a la mitad o al doble)
//   J / L     distancia de dibujo de los arboles
//   K         sombras de los arboles si / no
//   V         viento si / no
//   T         volar alto (vista del bosque entero) o volver al suelo
//   H         ocultar / mostrar este panel
#include <cramion/Script.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <string>

using namespace cramion;

class Rendimiento : public Script {
public:
    void start() override {
        texto_ = Scene::find("UI_Rendimiento");
        panel_ = Scene::find("UI_Panel");
        bosque_ = Scene::find("Vegetacion");
        jugador_ = Scene::find("Jugador");
    }

    void update(float dt) override {
        teclas();
        // Ultimos 3 segundos de frames.
        muestras_.push_back(dt);
        suma_ += dt;
        while (suma_ > 3.0f && muestras_.size() > 1) {
            suma_ -= muestras_.front();
            muestras_.pop_front();
        }
        if (avisoTiempo_ > 0.0f) avisoTiempo_ -= dt;
        refresco_ -= dt;
        if (refresco_ > 0.0f) return;
        refresco_ = 0.25f;

        float peor = 0.0f, mejor = 1e9f;
        for (const float s : muestras_) {
            peor = std::max(peor, s);
            mejor = std::min(mejor, s);
        }
        const float media = suma_ / std::max<float>(static_cast<float>(muestras_.size()), 1.0f);
        const double arboles = Graphics::get("foliage_trees").asNumber();
        const double visibles = Graphics::get("foliage_visible").asNumber();
        const double cerca = Graphics::get("foliage_near").asNumber();
        const double triangulos = Graphics::get("foliage_triangles").asNumber();
        const double gpu = Graphics::get("gpu_ms").asNumber();
        char linea[256];
        std::string texto;
        if (avisoTiempo_ > 0.0f) texto = ">> " + aviso_ + "\n";
        std::snprintf(linea, sizeof(linea), "FPS %.0f   (min %.0f  max %.0f, ultimos 3 s)\n", 1.0 / media, 1.0 / peor, 1.0 / mejor);
        texto += linea;
        std::snprintf(linea, sizeof(linea), "Frame %.2f ms   GPU %.2f ms   calidad %s\n", media * 1000.0, gpu,
                      Graphics::getQuality().asString().c_str());
        texto += linea;
        texto += "Arboles " + miles(arboles) + " en la isla   dibujados " + miles(visibles) + " (" + miles(cerca) +
                 " con todo el detalle)\n";
        std::snprintf(linea, sizeof(linea), "Triangulos de los arboles %.1f M   densidad %.0f/ha   distancia %.0f m\n", triangulos / 1e6,
                      campo("density").asNumber(), campo("max_distance").asNumber());
        texto += linea;
        texto += "1-4 calidad  N/M arboles  J/L distancia  K sombras  V viento  T vista alta  H panel";
        texto_.set("text", texto);
    }

private:
    void avisar(const std::string& texto) {
        aviso_ = texto;
        avisoTiempo_ = 3.0f;
    }

    Value campo(const char* nombre) const { return bosque_.field("Foliage", nombre); }
    void poner(const char* nombre, const Value& valor) const { bosque_.setField("Foliage", nombre, valor); }

    void avisarDensidad() {
        char t[96];
        std::snprintf(t, sizeof(t), "Densidad %.0f arboles/ha (sembrando...)", campo("density").asNumber());
        avisar(t);
    }

    void avisarDistancia() {
        char t[96];
        std::snprintf(t, sizeof(t), "Distancia de los arboles %g m", campo("max_distance").asNumber());
        avisar(t);
    }

    void teclas() {
        static const char* calidades[] = {"Baja", "Media", "Alta", "Ultra"};
        for (int i = 0; i < 4; ++i) {
            if (Input::keyDown(std::to_string(i + 1))) {
                Graphics::setQuality(calidades[i]);
                avisar(std::string("Calidad ") + calidades[i]);
            }
        }
        if (Input::keyDown("n")) {
            poner("density", std::max(5.0, campo("density").asNumber() / 2.0));
            avisarDensidad();
        }
        if (Input::keyDown("m")) {
            poner("density", std::min(1200.0, campo("density").asNumber() * 2.0));
            avisarDensidad();
        }
        if (Input::keyDown("j")) {
            poner("max_distance", std::max(300.0, campo("max_distance").asNumber() - 500.0));
            avisarDistancia();
        }
        if (Input::keyDown("l")) {
            poner("max_distance", std::min(12000.0, campo("max_distance").asNumber() + 500.0));
            avisarDistancia();
        }
        if (Input::keyDown("k")) {
            poner("cast_shadows", !campo("cast_shadows").asBool());
            avisar(campo("cast_shadows").asBool() ? "Sombras de los arboles: si" : "Sombras de los arboles: no");
        }
        if (Input::keyDown("v")) {
            poner("wind", campo("wind").asNumber() > 0.0 ? 0 : 1);
            avisar(campo("wind").asNumber() > 0.0 ? "Viento: si" : "Viento: no");
        }
        if (Input::keyDown("t") && jugador_) {
            if (!alto_) {
                suelo_ = jugador_.position();
                jugador_.setPosition(suelo_ + Vec3{0, 350, 0});
                jugador_.setVelocity(Vec3{});
                alto_ = true;
                avisar("Vista desde 350 m (T para volver)");
            } else {
                jugador_.setPosition(suelo_);
                jugador_.setVelocity(Vec3{});
                alto_ = false;
            }
        }
        if (alto_ && jugador_) {
            // Arriba: sin caer (se queda flotando para mirar el bosque).
            const Vec3 v = jugador_.velocity();
            jugador_.setVelocity(Vec3{v.x, 0, v.z});
        }
        if (Input::keyDown("h")) panel_.setActive(!panel_.active());
    }

    // 1234567 -> "1.234.567"
    static std::string miles(double n) {
        const std::string s = std::to_string(static_cast<long long>(std::floor(n + 0.5)));
        std::string out;
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (i > 0 && (s.size() - i) % 3 == 0) out += '.';
            out += s[i];
        }
        return out;
    }

    Entity texto_, panel_, bosque_, jugador_;
    std::deque<float> muestras_;
    float suma_ = 0.0f;
    float refresco_ = 0.0f;
    std::string aviso_;
    float avisoTiempo_ = 0.0f;
    bool alto_ = false;
    Vec3 suelo_;
};

CRAMION_SCRIPT(Rendimiento)
)CPP"},
};

}  // namespace cramion::editor::openworld

#endif  // CRAMION_EDITOR_TEMPLATE_OPENWORLD_SCRIPTS_H

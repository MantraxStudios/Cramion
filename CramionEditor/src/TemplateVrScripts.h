#ifndef CRAMION_EDITOR_TEMPLATE_VR_SCRIPTS_H
#define CRAMION_EDITOR_TEMPLATE_VR_SCRIPTS_H

// Scripts de C++ de la plantilla de realidad virtual (ProjectTemplates.cpp).

namespace cramion::editor::template_vr {

// El panel de la plantilla: sus botones (Canvas en modo Mundo, se pulsan con
// el rayo y el gatillo) reinician los objetos y cambian el giro y la
// velocidad del jugador.
inline constexpr const char* kMenuHeader = R"CPP(// MenuVR.h: el panel de la plantilla de VR.
#pragma once

#include <cramion/Script.h>

#include <vector>

using namespace cramion;

// Va en el Canvas del panel (en modo Mundo). Sus controles llaman a estos
// metodos: Reiniciar objetos (OnReiniciar), Giro suave (OnGiroSuave) y
// Velocidad (OnVelocidad).
class MenuVR : public Script {
public:
    Property<Entity> jugador{this, "jugador", {}, Requires("XrPlayer"), Tooltip("El Jugador VR")};
    Property<std::vector<Entity>> objetos{this, "objetos", {}, Tooltip("Los que vuelven a su sitio al reiniciar")};
    Property<Entity> estado{this, "estado", {}, Requires("UIText"), Tooltip("Texto para los avisos del panel")};

    void start() override;

private:
    void reiniciar();
    void avisar(const std::string& texto);

    std::vector<Vec3> posiciones_;
    std::vector<Vec3> giros_;
};
)CPP";

inline constexpr const char* kMenuSource = R"CPP(// MenuVR.cpp: el panel de la plantilla de VR.
#include "MenuVR.h"

void MenuVR::start() {
    // Donde empieza cada objeto, para dejarlo ahi al reiniciar.
    for (const Entity& objeto : objetos.get()) {
        posiciones_.push_back(objeto.position());
        giros_.push_back(objeto.rotation());
    }
    on("OnReiniciar", [this](const Value&) { reiniciar(); });
    on("OnGiroSuave", [this](const Value& activo) {
        // XR Player: 0 = por pasos, 1 = suave.
        jugador.get().setField("XrPlayer", "turn", activo.asBool() ? 1 : 0);
        avisar(activo.asBool() ? "Giro suave" : "Giro por pasos");
    });
    on("OnVelocidad", [this](const Value& valor) {
        const float velocidad = valor.asFloat();
        jugador.get().setFloat("CharacterController", "walk_speed", velocidad);
        jugador.get().setFloat("CharacterController", "run_speed", velocidad * 1.8f);
        avisar("Velocidad: " + std::to_string(static_cast<int>(velocidad * 10.0f + 0.5f) / 10) + "." +
               std::to_string(static_cast<int>(velocidad * 10.0f + 0.5f) % 10) + " m/s");
    });
}

void MenuVR::reiniciar() {
    const std::vector<Entity>& lista = objetos.get();
    for (std::size_t i = 0; i < lista.size() && i < posiciones_.size(); ++i) {
        lista[i].setPosition(posiciones_[i]);
        lista[i].setRotation(giros_[i]);
        lista[i].setVelocity(Vec3{});
        lista[i].setAngularVelocity(Vec3{});
    }
    avisar("Objetos en su sitio");
}

void MenuVR::avisar(const std::string& texto) {
    if (estado.get().valid()) estado.get().setField("UIText", "text", texto);
}

CRAMION_SCRIPT(MenuVR)
)CPP";

}  // namespace cramion::editor::template_vr

#endif  // CRAMION_EDITOR_TEMPLATE_VR_SCRIPTS_H

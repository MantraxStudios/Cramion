// Nivel.cpp: el hoyo en juego.
#include "Nivel.h"

#include <algorithm>

namespace {

std::string resultado(int diferencia) {
    switch (diferencia) {
        case -3: return "¡ALBATROS!";
        case -2: return "¡EAGLE!";
        case -1: return "¡BIRDIE!";
        case 0: return "PAR";
        case 1: return "BOGEY";
        case 2: return "DOBLE BOGEY";
        default: return (diferencia > 0 ? "+" : "") + std::to_string(diferencia);
    }
}

}  // namespace

void Nivel::start() {
    golpes_ = 0;
    terminado_ = false;
    espera_ = 0.0f;
    mensajeTiempo_ = 0.0f;
    marcador_ = Scene::find("HUD_Golpes");
    total_ = Scene::find("HUD_Total");
    mensaje_ = Scene::find("HUD_Mensaje");
    barra_ = Scene::find("HUD_Potencia");
    etiquetaBarra_ = Scene::find("HUD_PotenciaTexto");
    if (Entity titulo = Scene::find("HUD_Titulo")) titulo.setText("HOYO " + std::to_string(numero) + "  ·  " + nombre.get());
    mostrar("HOYO " + std::to_string(numero), 2.0f);
    actualizar();
}

int Nivel::totalAnterior() const {
    int suma = 0;
    for (int i = 1; i < numero; ++i) suma += std::max(0, Prefs::getInt("golpes_" + std::to_string(i), 0).asInt());
    return suma;
}

void Nivel::actualizar() {
    if (marcador_) marcador_.setText("Par " + std::to_string(par) + "     Golpes " + std::to_string(golpes_));
    if (total_) total_.setText("Total " + std::to_string(totalAnterior() + golpes_));
}

void Nivel::mostrar(const std::string& texto, float segundos) {
    if (!mensaje_) return;
    mensaje_.setText(texto);
    mensaje_.setActive(true);
    mensajeTiempo_ = segundos;
}

void Nivel::mostrarPotencia(float valor) {
    if (barra_) {
        barra_.setValue(valor);
        barra_.setActive(valor > 0.0f);
    }
    if (etiquetaBarra_) etiquetaBarra_.setActive(valor > 0.0f);
}

void Nivel::golpe() {
    ++golpes_;
    actualizar();
}

void Nivel::penalizar() {
    ++golpes_;
    actualizar();
    mostrar("FUERA  (+1)", 1.5f);
}

void Nivel::hoyo() {
    if (terminado_) return;
    terminado_ = true;
    const std::string texto = golpes_ == 1 ? std::string("¡HOYO EN UNO!") : resultado(golpes_ - par);
    mostrar(texto + "\n" + std::to_string(golpes_) + (golpes_ == 1 ? " golpe" : " golpes"), 3.0f);
    Prefs::setInt("golpes_" + std::to_string(numero), golpes_);
    actualizar();
    espera_ = 3.0f;
}

void Nivel::update(float dt) {
    if (mensajeTiempo_ > 0.0f) {
        mensajeTiempo_ -= dt;
        if (mensajeTiempo_ <= 0.0f && mensaje_) mensaje_.setActive(false);
    }
    if (terminado_) {
        espera_ -= dt;
        if (espera_ <= 0.0f) {
            terminado_ = false;
            Scene::load(siguiente.get());
        }
    }
}

CRAMION_SCRIPT(Nivel)

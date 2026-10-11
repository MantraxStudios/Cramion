// Nivel.h: el hoyo en juego; golpes, par, marcador (HUD) y paso al siguiente.
// La pelota le avisa de cada golpe, del castigo y de cuando entra.
#pragma once

#include <cramion/Script.h>

#include <string>

using namespace cramion;

class Nivel : public Script {
public:
    Property<int> numero{this, "numero", 1};
    Property<std::string> nombre{this, "nombre", "Primer golpe"};
    Property<int> par{this, "par", 2};
    Property<std::string> siguiente{this, "siguiente", "Nivel2", Tooltip("Escena al terminar el hoyo")};

    void start() override;
    void update(float dt) override;

    void golpe();
    void penalizar();
    void hoyo();
    void mostrarPotencia(float valor);

private:
    int totalAnterior() const;
    void actualizar();
    void mostrar(const std::string& texto, float segundos);

    int golpes_ = 0;
    bool terminado_ = false;
    float espera_ = 0.0f;
    float mensajeTiempo_ = 0.0f;
    Entity marcador_, total_, mensaje_, barra_, etiquetaBarra_;
};

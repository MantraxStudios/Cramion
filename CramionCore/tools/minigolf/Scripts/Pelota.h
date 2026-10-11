// Pelota.h: la pelota; apuntar, cargar el golpe, rodar, caer al hoyo o salirse.
//
//   A / D o flechas       girar la direccion
//   clic derecho + raton  girar la direccion
//   Espacio o clic izq.   mantener para cargar, soltar para golpear
//   R                     volver al ultimo tiro (+1 golpe)
//   Esc                   menu
#pragma once

#include <cramion/Script.h>

#include <vector>

using namespace cramion;

class Nivel;

class Pelota : public Script {
public:
    Property<float> yawInicial{this, "yawInicial", 0.0f, Tooltip("Grados (0 = hacia -Z)")};
    Property<float> velocidadMaxima{this, "velocidadMaxima", 10.0f, Tooltip("m/s con la barra llena")};
    Property<float> velocidadGiro{this, "velocidadGiro", 90.0f, Tooltip("Grados por segundo con el teclado")};

    void start() override;
    void update(float dt) override;
    void onCollisionEnter(const Collision& c) override;

    // Hacia donde apunta (grados; la camara se pone detras).
    float yaw() const { return yaw_; }

private:
    enum class Estado { Apuntando, Cargando, Rodando, Hoyo };

    Nivel* nivel();
    void parar();
    void volverAlTiro(bool penalizar);
    void golpear();
    void actualizarMira();

    float yaw_ = 0.0f;
    Estado estado_ = Estado::Apuntando;
    float potencia_ = 0.0f;
    float carga_ = 0.0f;
    float quieto_ = 0.0f;
    float tiempoTiro_ = 0.0f;
    Vec3 ultimo_;
    float sueloY_ = 0.0f;
    Entity hoyo_, nivel_;
    std::vector<Entity> mira_;
};

#ifndef CRAMION_EDITOR_TEMPLATE21_SCRIPTS_H
#define CRAMION_EDITOR_TEMPLATE21_SCRIPTS_H

// Scripts de C++ de las plantillas de la 2.1: "Plataformas 2D" y "Coches".

#include "TemplateScripts.h"

namespace cramion::editor::template21 {

// Plataformas 2D: jugador (fisica 2D y animaciones) y camara.
inline constexpr TemplateFile kPlatformer2D[] = {
    {"Camara2D.cpp",
R"CPP(// Camara2D.cpp: camara 2D que sigue al jugador con suavidad (y un poco por
// encima).
#include <cramion/Script.h>

using namespace cramion;

class Camara2D : public Script {
public:
    Property<float> suavidad{this, "suavidad", 5.0f};
    Property<float> altura{this, "altura", 1.5f};

    void lateUpdate(float dt) override {
        const Entity jugador = Scene::find("Jugador");
        if (!jugador) return;
        const Vec3 p = entity().position();
        const Vec3 t = jugador.position();
        const float k = std::min(1.0f, dt * suavidad);
        entity().setPosition(Vec3{p.x + (t.x - p.x) * k, p.y + (t.y + altura - p.y) * k, p.z});
    }
};

CRAMION_SCRIPT(Camara2D)
)CPP"},
    {"Jugador2D.cpp",
R"CPP(// Jugador2D.cpp: jugador de plataformas. A/D o flechas para moverse, Espacio
// para saltar. Fisica 2D (Rigidbody2D + Box Collider 2D) y animaciones del
// Sprite Animator.
#include <cramion/Script.h>

#include <string>

using namespace cramion;

class Jugador2D : public Script {
public:
    Property<float> velocidad{this, "velocidad", 6.0f};
    Property<float> salto{this, "salto", 12.5f};

    void start() override {
        monedas_ = 0;
        inicio_ = entity().position();
        // Las monedas son triggers 2D: el mensaje trae {other, point, normal...}.
        on("OnTriggerEnter2D", [this](const Value& contacto) {
            const Entity otro = contacto["other"].asEntity();
            if (otro.tag() != "Moneda") return;
            otro.destroy();
            ++monedas_;
            if (const Entity marcador = Scene::find("Marcador")) marcador.set("text", "Monedas: " + std::to_string(monedas_));
        });
    }

    void update(float) override {
        Vec3 v = entity().velocity2D().asVec3();
        float eje = 0.0f;
        if (Input::key("a") || Input::key("left")) eje -= 1.0f;
        if (Input::key("d") || Input::key("right")) eje += 1.0f;
        v.x = eje * velocidad;

        // En el suelo: un rayo corto desde los pies.
        const Vec3 pies = entity().position() + Vec3{0, -0.5f, 0};
        const bool suelo = !Physics2D::raycast(pies, Vec3{0, -1, 0}, 0.12f).isNil();
        if (suelo && (Input::keyDown("space") || Input::keyDown("w") || Input::keyDown("up"))) v.y = salto;
        entity().setVelocity2D(v);

        if (eje != 0.0f) entity().setFlipX(eje < 0.0f);
        if (!suelo) entity().playSpriteAnimation("Saltar");
        else if (eje != 0.0f) entity().playSpriteAnimation("Correr");
        else entity().playSpriteAnimation("Quieto");

        // Se cae por un agujero: vuelve al principio.
        if (entity().position().y < -12.0f) {
            entity().movePosition2D(inicio_);
            entity().setVelocity2D(Vec3{});
        }
    }

private:
    int monedas_ = 0;
    Vec3 inicio_;
};

CRAMION_SCRIPT(Jugador2D)
)CPP"},
};

// Coches: camara de persecucion y velocimetro.
inline constexpr TemplateFile kCars[] = {
    {"CamaraCoche.cpp",
R"CPP(// CamaraCoche.cpp: camara de persecucion. Detras del coche y un poco por
// encima, con muelle.
#include <cramion/Script.h>

using namespace cramion;

class CamaraCoche : public Script {
public:
    Property<float> distancia{this, "distancia", 8.0f};
    Property<float> altura{this, "altura", 3.0f};
    Property<float> suavidad{this, "suavidad", 4.0f};

    void lateUpdate(float dt) override {
        const Entity coche = Scene::find("Coche");
        if (!coche) return;
        const Vec3 destino = coche.position() - coche.forward() * distancia + Vec3{0, altura, 0};
        const float k = std::min(1.0f, dt * suavidad);
        entity().setPosition(entity().position() + (destino - entity().position()) * k);
        entity().lookAt(coche.position() + Vec3{0, 1, 0});
    }
};

CRAMION_SCRIPT(CamaraCoche)
)CPP"},
    {"Velocimetro.cpp",
R"CPP(// Velocimetro.cpp: km/h, marcha y rpm del coche (Vehicle) en el HUD. R vuelve
// a la salida (si vuelca).
#include <cramion/Script.h>

#include <cmath>
#include <cstdio>
#include <string>

using namespace cramion;

class Velocimetro : public Script {
public:
    void update(float) override {
        const Entity coche = Scene::find("Coche");
        if (!coche) return;
        const Value estado = coche.vehicleState();
        if (estado.isNil()) return;
        const int gear = estado["gear"].asInt();
        const std::string marcha = gear == -1 ? "R" : (gear == 0 ? "N" : std::to_string(gear));
        char texto[96];
        std::snprintf(texto, sizeof(texto), "%3d km/h   marcha %s   %4d rpm", static_cast<int>(std::floor(estado["speed"].asFloat() + 0.5f)),
                      marcha.c_str(), static_cast<int>(std::floor(estado["rpm"].asFloat())));
        entity().set("text", texto);
        if (Input::keyDown("r")) {
            coche.setPosition(Vec3{0, 1.5f, 0});
            coche.setRotation(Vec3{});
            coche.setVelocity(Vec3{});
        }
    }
};

CRAMION_SCRIPT(Velocimetro)
)CPP"},
};

}  // namespace cramion::editor::template21

#endif  // CRAMION_EDITOR_TEMPLATE21_SCRIPTS_H

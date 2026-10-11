// Pelota.cpp: la pelota del minigolf.
#include "Pelota.h"

#include "Nivel.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace {

constexpr float kRadioHoyo = 0.19f;
constexpr float kProfundidad = 0.07f;

Vec3 direccion(float yaw) {
    const float r = yaw * Mathf::deg2rad;
    return Vec3{-std::sin(r), 0, -std::cos(r)};
}

}  // namespace

void Pelota::start() {
    yaw_ = yawInicial;
    estado_ = Estado::Apuntando;
    potencia_ = carga_ = quieto_ = tiempoTiro_ = 0.0f;
    ultimo_ = entity().position();
    sueloY_ = ultimo_.y;
    hoyo_ = Scene::find("Hoyo");
    nivel_ = Scene::find("Nivel");
    mira_.clear();
    for (int i = 1; i <= 8; ++i) {
        if (Entity punto = Scene::find("Mira" + std::to_string(i))) mira_.push_back(punto);
    }
}

Nivel* Pelota::nivel() { return nivel_ ? nivel_.script<Nivel>() : nullptr; }

void Pelota::parar() {
    entity().setVelocity(Vec3{});
    entity().setAngularVelocity(Vec3{});
}

void Pelota::volverAlTiro(bool penalizar) {
    entity().setPosition(ultimo_);
    parar();
    estado_ = Estado::Apuntando;
    quieto_ = 0.0f;
    Audio::playOneShot("Audio/fuera.wav");
    if (Nivel* n = nivel(); penalizar && n) n->penalizar();
}

void Pelota::golpear() {
    const float fuerza = 0.6f + std::pow(potencia_, 1.25f) * velocidadMaxima;
    ultimo_ = entity().position();
    entity().setVelocity(direccion(yaw_) * fuerza);
    estado_ = Estado::Rodando;
    quieto_ = tiempoTiro_ = potencia_ = 0.0f;
    Audio::playOneShot("Audio/golpe.wav", Value(), 0.9);
    if (Nivel* n = nivel()) n->golpe();
}

void Pelota::actualizarMira() {
    const bool visible = estado_ == Estado::Apuntando || estado_ == Estado::Cargando;
    const Vec3 p = entity().position();
    const Vec3 dir = direccion(yaw_);
    // Con la carga, mas puntos y mas separados.
    const int total = static_cast<int>(mira_.size());
    const int cuantos = estado_ == Estado::Cargando
                            ? std::max(2, static_cast<int>(std::ceil(total * (0.25f + 0.75f * potencia_))))
                            : std::min(4, total);
    const float paso = 0.28f + 0.12f * potencia_;
    for (int i = 0; i < total; ++i) {
        const bool activo = visible && i < cuantos;
        mira_[i].setActive(activo);
        if (activo) mira_[i].setPosition(p + dir * (0.2f + paso * static_cast<float>(i + 1)) + Vec3{0, -0.07f, 0});
    }
}

void Pelota::update(float dt) {
    if (Input::keyDown("Escape")) {
        Scene::load("Menu");
        return;
    }
    if (estado_ == Estado::Hoyo) {
        actualizarMira();
        return;
    }

    const Vec3 p = entity().position();
    // Fuera del campo (o cayo): al ultimo tiro con un golpe de castigo.
    if (p.y < sueloY_ - 0.12f) {
        volverAlTiro(true);
        return;
    }
    if (Input::keyDown("R") && estado_ == Estado::Rodando) {
        volverAlTiro(true);
        return;
    }

    // En el hoyo: dentro del circulo y por debajo del suelo.
    if (hoyo_) {
        const Vec3 h = hoyo_.position();
        const float dx = p.x - h.x, dz = p.z - h.z;
        const float d = std::sqrt(dx * dx + dz * dz);
        const Vec3 v = entity().velocity();
        if (d < kRadioHoyo && p.y < h.y + kProfundidad) {
            estado_ = Estado::Hoyo;
            parar();
            entity().setPosition(Vec3{h.x, h.y - 0.02f, h.z});
            Audio::playOneShot("Audio/hoyo.wav");
            if (Nivel* n = nivel()) n->hoyo();
            return;
        }
        // Cerca y despacio: el borde del hoyo la atrae un poco.
        if (estado_ == Estado::Rodando && d < 0.34f && v.length() < 1.6f && d > 0.001f) {
            entity().setVelocity(v + Vec3{-dx / d, 0, -dz / d} * (dt * 5.0f));
        }
    }

    if (estado_ == Estado::Rodando) {
        tiempoTiro_ += dt;
        if (tiempoTiro_ > 0.3f && entity().velocity().length() < 0.07f) {
            quieto_ += dt;
            if (quieto_ > 0.3f) {
                parar();
                estado_ = Estado::Apuntando;
            }
        } else {
            quieto_ = 0.0f;
        }
        // Rodando muy lento mucho rato: se da por parada.
        if (tiempoTiro_ > 12.0f) {
            parar();
            estado_ = Estado::Apuntando;
        }
    } else {
        // Apuntar.
        float giro = 0.0f;
        if (Input::key("A") || Input::key("Left")) giro += 1.0f;
        if (Input::key("D") || Input::key("Right")) giro -= 1.0f;
        const float lento = Input::key("Shift") ? 0.3f : 1.0f;
        yaw_ += giro * velocidadGiro * lento * dt;
        if (Input::mouseButton(1)) yaw_ -= Input::mouse().dx * 0.25f;
        // Cargar y golpear.
        if (Input::key("Space") || Input::mouseButton(0)) {
            if (estado_ != Estado::Cargando) {
                estado_ = Estado::Cargando;
                carga_ = 0.0f;
            }
            carga_ += dt;
            const float t = std::fmod(carga_ / 1.2f, 2.0f);
            potencia_ = t < 1.0f ? t : 2.0f - t;
        } else if (estado_ == Estado::Cargando) {
            golpear();
        }
    }
    if (Nivel* n = nivel()) n->mostrarPotencia(estado_ == Estado::Cargando ? potencia_ : 0.0f);
    actualizarMira();
}

// Rebotes: sonido segun la fuerza del choque.
void Pelota::onCollisionEnter(const Collision& c) {
    const float fuerza = c.relativeVelocity.length();
    if (fuerza > 1.2f && c.other.name() != "Suelo") Audio::playOneShot("Audio/rebote.wav", Value(), std::min(1.0f, fuerza / 6.0f));
}

CRAMION_SCRIPT(Pelota)

#ifndef CRAMION_EDITOR_TEMPLATE_LOCOMOTION_SCRIPTS_H
#define CRAMION_EDITOR_TEMPLATE_LOCOMOTION_SCRIPTS_H

// Scripts de C++ de la plantilla "Tercera persona avanzada"
// (ProjectTemplates.cpp): el personaje con el Locomotion Pack, la camara
// orbital, las palancas, los cristales y la interfaz.

#include "TemplateScripts.h"

namespace cramion::editor::locomotion_scripts {

inline constexpr TemplateFile kFiles[] = {
    {"CamaraOrbital.h",
R"CPP(// CamaraOrbital.h: camara orbital en tercera persona con brazo de muelle. El
// raton la gira (clic en la vista para capturarlo, Esc para soltarlo), la
// rueda la acerca y no atraviesa paredes (se acerca si algo tapa al
// personaje). Al apuntar (clic derecho) se pone sobre el hombro y cierra el
// campo de vision; al correr lo abre un poco.
#pragma once

#include <cramion/Script.h>

#include <string>

using namespace cramion;

class CamaraOrbital : public Script {
public:
    Property<std::string> objetivo{this, "objetivo", "Jugador"};
    Property<float> distancia{this, "distancia", 3.6f};
    Property<float> altura{this, "altura", 0.55f, Tooltip("Sobre el centro del personaje")};
    Property<float> hombro{this, "hombro", 0.55f, Tooltip("Desplazamiento lateral al apuntar")};
    Property<float> sensibilidad{this, "sensibilidad", 0.12f};
    Property<float> fov{this, "fov", 60.0f};

    void start() override;
    void lateUpdate(float dt) override;
    // Grados (el personaje se mueve relativo a la camara).
    float yaw() const { return yaw_; }

private:
    Entity target_;
    float yaw_ = 0.0f;
    float pitch_ = 12.0f;
    float actual_ = 0.0f;
    float lado_ = 0.0f;
    float campo_ = 60.0f;
    Vec3 foco_;
};
)CPP"},
    {"Palanca.h",
R"CPP(// Palanca.h: la acciona el personaje (E) con la mano. El brazo baja y abre la
// compuerta que diga `compuerta` (sube y deja pasar).
#pragma once

#include <cramion/Script.h>

#include <string>

using namespace cramion;

class Palanca : public Script {
public:
    Property<std::string> compuerta{this, "compuerta", "Compuerta 1"};
    Property<float> subir{this, "subir", 3.2f, Tooltip("Metros que sube la compuerta")};
    Property<float> duracion{this, "duracion", 1.6f};

    void start() override;
    void update(float dt) override;
    bool usada() const { return usada_; }
    void accionar();

private:
    Entity brazo_, puerta_;
    Vec3 base_;
    bool usada_ = false;
    float t_ = -1.0f;
};
)CPP"},
    {"Personaje.h",
R"CPP(// Personaje.h: personaje en tercera persona (Locomotion Pack de Mixamo).
//
//   WASD mover (relativo a la camara)  Shift correr  Espacio saltar
//   Clic derecho apuntar: el cuerpo mira con la camara y se mueve de lado
//   (Blend Tree 2D); quieto, gira en el sitio con las animaciones de girar
//   E accionar una palanca (la mano la agarra con IK)
//
// El Animator (Personaje.cranimator) recibe X e Y: la velocidad del cuerpo
// en m/s, de lado y hacia delante. Las velocidades de las propiedades son las
// que midio el importador en cada animacion: los pies no patinan.
#pragma once

#include <cramion/Script.h>

#include <optional>
#include <string>

using namespace cramion;

class Personaje : public Script {
public:
    Property<float> velAndar{this, "velAndar", 1.66f, Tooltip("m/s de cada animacion (las mide el importador)")};
    Property<float> velCorrer{this, "velCorrer", 4.35f};
    Property<float> velLateral{this, "velLateral", 1.72f};
    Property<float> velLateralCorrer{this, "velLateralCorrer", 4.49f};
    Property<float> aceleracion{this, "aceleracion", 9.0f};
    Property<float> giro{this, "giro", 600.0f, Tooltip("Grados/s al girar hacia donde se mueve")};
    Property<float> alturaSalto{this, "alturaSalto", 1.1f, Tooltip("m")};
    Property<float> despegue{this, "despegue", 0.8f, Tooltip("s de la animacion de saltar en que deja el suelo")};
    Property<float> aterrizaje{this, "aterrizaje", 1.27f, Tooltip("Y en que lo vuelve a tocar")};
    Property<float> duracionSalto{this, "duracionSalto", 2.17f};
    Property<float> durGiro90{this, "durGiro90", 0.93f, Tooltip("Duracion de las animaciones de girar 90 y 180")};
    Property<float> durGiro180{this, "durGiro180", 1.63f};
    Property<float> energiaMax{this, "energiaMax", 100.0f};
    Property<float> gastoCorrer{this, "gastoCorrer", 16.0f, Tooltip("Por segundo")};
    Property<float> recuperar{this, "recuperar", 22.0f};
    Property<float> escalon{this, "escalon", 0.45f, Tooltip("Lo mas alto que sube andando (m)")};
    Property<float> pendienteMax{this, "pendienteMax", 50.0f, Tooltip("Grados")};

    void start() override;
    void update(float dt) override;

    // Lo que leen la camara, los cristales y la interfaz.
    bool apuntando() const { return apuntando_; }
    bool enSuelo() const { return enSuelo_; }
    bool accionando() const { return mano_.has_value(); }
    float energia() const { return energia_; }
    int cristales() const { return cristales_; }
    float animX() const { return animX_; }
    float animY() const { return animY_; }
    void recoger() { ++cristales_; }
    void reaparecer();
    // La palanca sin usar mas cercana (no valida si no hay).
    Entity palancaCerca() const;
    // Estado para la interfaz y la depuracion (F1).
    std::string estado() const;

private:
    struct Salto {
        float anim = 0.0f;  // segundos de la animacion
        std::string fase;   // agachado, subiendo, cayendo, aterrizando
        bool moviendo = false;
        float t = 0.0f;
    };
    struct GiroSitio {
        float desde, grados, dur, t, vel;
    };
    struct Mano {
        Entity palanca;
        float t = 0.0f;
        bool hecho = false;
        Vec3 sitio;
        float yaw = 0.0f;
    };

    float yawCamara() const;
    bool sueloEn(const Vec3& p, float alto, float largo, RaycastHit* hit) const;
    bool pisable(const RaycastHit& hit) const;
    void saltar(float dt, Vec3& v);
    void girarEnSitio(float dt, float yawCam, bool quiere);
    void accionar(float dt);
    void mirar(float dt);

    Entity pivote_, modelo_, camara_;
    Vec3 inicio_;
    float yaw_ = 0.0f;
    float animX_ = 0.0f, animY_ = 0.0f;
    float energia_ = 0.0f;
    bool agotado_ = false;
    bool enSuelo_ = true;
    float aire_ = 0.0f;
    float escalonT_ = 0.0f;
    std::optional<Salto> salto_;
    std::optional<GiroSitio> giroSitio_;
    std::optional<Mano> mano_;
    bool apuntando_ = false;
    int cristales_ = 0;
    float pesoMirar_ = 0.0f;
    std::string mirando_;
};
)CPP"},
    {"CamaraOrbital.cpp",
R"CPP(// CamaraOrbital.cpp: la camara orbital de la plantilla avanzada.
#include "CamaraOrbital.h"

#include "Personaje.h"

#include <algorithm>
#include <cmath>

void CamaraOrbital::start() {
    target_ = Scene::find(objetivo.get());
    actual_ = distancia;
    campo_ = fov;
    if (target_) foco_ = target_.position();
    Input::lockCursor(true);
}

void CamaraOrbital::lateUpdate(float dt) {
    if (!target_) return;
    if (Input::keyDown("escape")) Input::lockCursor(false);
    if (Input::mouseButton(0, 1) && !Input::isCursorLocked().asBool()) Input::lockCursor(true);
    const Input::MouseState raton = Input::mouse();
    if (Input::isCursorLocked().asBool() || Input::mouseButton(1)) {
        yaw_ -= raton.dx * sensibilidad;
        pitch_ = Mathf::clamp(pitch_ + raton.dy * sensibilidad, -35.0f, 70.0f);
    }
    distancia = Mathf::clamp(distancia - raton.wheel * 0.5f, 1.5f, 9.0f);

    const Personaje* jugador = target_.script<Personaje>();
    const bool apunta = jugador != nullptr && jugador->apuntando();
    const bool corre = jugador != nullptr && target_.velocity().length() > 3.2f;
    lado_ = Mathf::lerp(lado_, apunta ? hombro.get() : 0.0f, Mathf::clamp01(8.0f * dt));
    const float dist = apunta ? std::min<float>(distancia, 2.0f) : distancia.get();
    campo_ = Mathf::lerp(campo_, apunta ? fov - 15.0f : (corre ? fov + 6.0f : fov.get()), Mathf::clamp01(5.0f * dt));
    entity().setField("Camera", "fov", campo_);

    // El foco sigue al personaje con algo de retraso en vertical (escalones).
    const Vec3 p = target_.position() + Vec3{0, altura, 0};
    foco_ = Vec3{p.x, Mathf::lerp(foco_.y, p.y, Mathf::clamp01(10.0f * dt)), p.z};
    const float yaw = yaw_ * Mathf::deg2rad;
    const float pitch = pitch_ * Mathf::deg2rad;
    const Vec3 atras{std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
    const Vec3 derecha{std::cos(yaw), 0, -std::sin(yaw)};
    const Vec3 foco = foco_ + derecha * lado_;

    // Brazo de muelle: si algo hay entre el foco y la camara, se acerca.
    float deseada = dist;
    const Vec3 inicio = foco + atras * 0.45f;  // fuera de la capsula del personaje
    RaycastHit hit;
    if (Physics::raycast(inicio, atras, dist, &hit)) deseada = std::max(0.6f, hit.distance + 0.45f - 0.25f);
    if (deseada < actual_) actual_ = deseada;
    else actual_ = Mathf::lerp(actual_, deseada, Mathf::clamp01(4.0f * dt));

    entity().setPosition(foco + atras * actual_);
    entity().lookAt(foco);
}

CRAMION_SCRIPT(CamaraOrbital)
)CPP"},
    {"Cristal.cpp",
R"CPP(// Cristal.cpp: gira, flota y se recoge al acercarse el personaje.
#include "Personaje.h"

#include <cmath>

class Cristal : public Script {
public:
    void start() override {
        base_ = entity().position();
        fase_ = base_.x * 0.7f + base_.z * 0.3f;
        jugador_ = Scene::find("Jugador");
    }

    void update(float dt) override {
        entity().rotate(Vec3{0, 90.0f * dt, 0});
        entity().setPosition(base_ + Vec3{0, std::sin(Time::time() * 2.0f + fase_) * 0.12f, 0});
        if (jugador_ && Vec3::distance(jugador_.position(), entity().position()) < 1.1f) {
            if (Personaje* p = jugador_.script<Personaje>()) p->recoger();
            entity().setActive(false);
        }
    }

private:
    Vec3 base_;
    float fase_ = 0.0f;
    Entity jugador_;
};

CRAMION_SCRIPT(Cristal)
)CPP"},
    {"Interfaz.cpp",
R"CPP(// Interfaz.cpp: cristales, energia, el aviso de la palanca y la depuracion
// (F1: estado, velocidades del Animator y del cuerpo).
#include "Personaje.h"

#include <cmath>
#include <cstdio>
#include <string>

class Interfaz : public Script {
public:
    void start() override {
        jugador_ = Scene::find("Jugador");
        marcador_ = entity().find("Marcador").asEntity();
        energia_ = entity().find("Energia").asEntity();
        aviso_ = entity().find("Aviso").asEntity();
        depuracion_ = entity().find("Depuracion").asEntity();
        total_ = static_cast<int>(Scene::findAllWithTag("Cristal").size());
        depuracion_.setActive(false);
    }

    void update(float) override {
        const Personaje* j = jugador_ ? jugador_.script<Personaje>() : nullptr;
        if (j == nullptr) return;
        const std::string cuenta = std::to_string(j->cristales()) + " / " + std::to_string(total_);
        marcador_.set("text", j->cristales() >= total_ && total_ > 0 ? "Todos los cristales!  " + cuenta : "Cristales  " + cuenta);
        energia_.set("value", j->energia() / j->energiaMax);
        aviso_.set("text", !j->accionando() && j->palancaCerca() ? "E  accionar la palanca" : "");

        if (Input::keyDown("f1")) {
            verDepuracion_ = !verDepuracion_;
            depuracion_.setActive(verDepuracion_);
        }
        if (verDepuracion_) {
            const Vec3 v = jugador_.velocity();
            char texto[256];
            std::snprintf(texto, sizeof(texto),
                          "Estado: %s\nAnimator  X %.2f  Y %.2f m/s\nVelocidad %.2f m/s  (vertical %.2f)\nEn el suelo: %s   Energia %d",
                          j->estado().c_str(), j->animX(), j->animY(), Vec3{v.x, 0, v.z}.length(), v.y, j->enSuelo() ? "true" : "false",
                          static_cast<int>(std::floor(j->energia())));
            depuracion_.set("text", texto);
        }
    }

private:
    Entity jugador_, marcador_, energia_, aviso_, depuracion_;
    int total_ = 0;
    bool verDepuracion_ = false;
};

CRAMION_SCRIPT(Interfaz)
)CPP"},
    {"Palanca.cpp",
R"CPP(// Palanca.cpp: la palanca y su compuerta.
#include "Palanca.h"

void Palanca::start() {
    brazo_ = entity().find("Brazo").asEntity();
    puerta_ = Scene::find(compuerta.get());
    if (puerta_) base_ = puerta_.position();
}

void Palanca::accionar() {
    if (usada_) return;
    usada_ = true;
    t_ = 0.0f;
}

void Palanca::update(float dt) {
    if (t_ < 0.0f) return;
    t_ += dt;
    // El brazo baja hacia el personaje en medio segundo (la mano va agarrada
    // a su pomo).
    const float a = Mathf::clamp01(t_ / 0.5f);
    brazo_.setRotation(Vec3{Mathf::lerp(10.0f, -35.0f, a * a * (3.0f - 2.0f * a)), 0, 0});
    if (puerta_) {
        const float s = Mathf::clamp01((t_ - 0.3f) / duracion);
        puerta_.setPosition(base_ + Vec3{0, subir * s * s * (3.0f - 2.0f * s), 0});
    }
    if (t_ > duracion + 0.5f) t_ = -1.0f;
}

CRAMION_SCRIPT(Palanca)
)CPP"},
    {"Personaje.cpp",
R"CPP(// Personaje.cpp: el controlador del personaje (escalones, pendientes, salto
// sincronizado con la animacion, giros en el sitio, modo apuntar con
// desplazamiento lateral) y su IK (pies en el suelo, mirar, la mano en las
// palancas).
#include "Personaje.h"

#include "CamaraOrbital.h"
#include "Palanca.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr float kMitad = 0.9f;  // media altura de la capsula
constexpr float kRadio = 0.3f;
constexpr float kG = 9.81f;

float suave(float t) {
    t = Mathf::clamp01(t);
    return t * t * (3.0f - 2.0f * t);
}
// Yaw (grados, 0 = mirar a -Z) <-> direccion horizontal.
Vec3 delante(float yaw) {
    const float r = yaw * Mathf::deg2rad;
    return Vec3{-std::sin(r), 0, -std::cos(r)};
}
Vec3 derecha(float yaw) {
    const float r = yaw * Mathf::deg2rad;
    return Vec3{std::cos(r), 0, -std::sin(r)};
}
float yawDe(const Vec3& d) { return std::atan2(-d.x, -d.z) * Mathf::rad2deg; }
Vec3 plano(const Vec3& v) { return Vec3{v.x, 0, v.z}; }
const Vec3 kAbajo{0, -1, 0};

}  // namespace

void Personaje::start() {
    pivote_ = entity().find("Pivote").asEntity();
    modelo_ = entity().find("Modelo").asEntity();
    camara_ = Scene::find("Main Camera");
    inicio_ = entity().position();
    yaw_ = pivote_.rotation().y;
    energia_ = energiaMax;
    modelo_.setFootGrounding(true);
    modelo_.setIKWeight("look", 0);
}

// Yaw de la camara (la orbital lo guarda en su script).
float Personaje::yawCamara() const {
    if (const CamaraOrbital* c = camara_ ? camara_.script<CamaraOrbital>() : nullptr) return c->yaw();
    return yawDe(plano(camara_ ? camara_.forward() : Vec3{0, 0, -1}));
}

// El suelo bajo un punto (rayo desde `alto` por encima de los pies).
bool Personaje::sueloEn(const Vec3& p, float alto, float largo, RaycastHit* hit) const {
    const float pies = entity().position().y - kMitad;
    return Physics::raycast(Vec3{p.x, pies + alto, p.z}, kAbajo, largo, hit);
}

bool Personaje::pisable(const RaycastHit& hit) const { return hit.normal.y >= std::cos(pendienteMax * Mathf::deg2rad); }

void Personaje::update(float dt) {
    if (dt <= 0.0f) return;
    Vec3 pos = entity().position();
    Vec3 v = entity().velocity();

    // Suelo: un rayo desde justo debajo de la capsula (desde dentro la tocaria
    // a ella) y cuatro alrededor para los bordes.
    RaycastHit bajo;
    const bool hayBajo = Physics::raycast(pos + Vec3{0, -kMitad - 0.02f, 0}, kAbajo, 0.5f, &bajo);
    bool tocando = hayBajo && bajo.distance < 0.08f;
    if (!tocando && v.y <= 0.5f) {
        for (const Vec3& o : {Vec3{kRadio, 0, 0}, Vec3{-kRadio, 0, 0}, Vec3{0, 0, kRadio}, Vec3{0, 0, -kRadio}}) {
            RaycastHit h;
            if (Physics::raycast(pos + o * 1.2f + Vec3{0, -kMitad + 0.1f, 0}, kAbajo, 0.2f, &h)) {
                tocando = true;
                break;
            }
        }
    }
    const bool antes = enSuelo_;
    enSuelo_ = tocando && (!salto_ || salto_->fase != "subiendo");
    aire_ = enSuelo_ ? 0.0f : aire_ + dt;

    const float yawCam = yawCamara();
    apuntando_ = Input::mouseButton(1) && !mano_;
    const float ix = Input::axis("Horizontal");
    const float iy = Input::axis("Vertical");
    Vec3 entrada = derecha(yawCam) * ix + delante(yawCam) * iy;
    if (entrada.length() > 1.0f) entrada = entrada.normalized();
    const bool quiere = entrada.length() > 0.1f && !mano_ && !giroSitio_;

    // Energia: correr la gasta; agotado no se corre hasta recuperar un 30 %.
    const bool corre = Input::key("shift") && quiere && !agotado_;
    if (corre && plano(v).length() > 2.5f) {
        energia_ = std::max(0.0f, energia_ - gastoCorrer * dt);
        if (energia_ <= 0.0f) agotado_ = true;
    } else {
        energia_ = std::min<float>(energiaMax, energia_ + recuperar * dt);
        if (agotado_ && energia_ > energiaMax * 0.3f) agotado_ = false;
    }

    // Velocidad deseada en el plano, segun el modo.
    Vec3 objetivo;
    if (quiere) {
        if (apuntando_) {
            // El cuerpo mira con la camara: delante, de lado o de espaldas
            // (andando: el clip de andar al reves).
            yaw_ = Mathf::moveTowardsAngle(yaw_, yawCam, giro * dt);
            const float lx = entrada.x * derecha(yaw_).x + entrada.z * derecha(yaw_).z;
            const float ly = entrada.x * delante(yaw_).x + entrada.z * delante(yaw_).z;
            const float vy = ly >= 0.0f ? (corre ? velCorrer : velAndar) : velAndar;
            const float vx = corre ? velLateralCorrer : velLateral;
            objetivo = derecha(yaw_) * (lx * vx) + delante(yaw_) * (ly * vy);
        } else {
            // Libre: gira hacia donde va (mas rapido parado) y avanza.
            const float deseado = yawDe(entrada);
            const float rapidez = plano(v).length();
            yaw_ = Mathf::moveTowardsAngle(yaw_, deseado, giro * (rapidez < 1.0f ? 1.4f : 1.0f) * dt);
            const float alineado = Mathf::clamp01(1.0f - std::abs(Mathf::deltaAngle(yaw_, deseado)) / 120.0f);
            objetivo = delante(yaw_) * (entrada.length() * (corre ? velCorrer : velAndar) * (0.35f + 0.65f * alineado));
        }
    }
    if (!enSuelo_) objetivo = Vec3::lerp(plano(v), objetivo, 0.35f);  // poco control en el aire

    const float k = Mathf::clamp01(aceleracion * dt * (enSuelo_ ? 1.0f : 0.3f));
    v.x = Mathf::lerp(v.x, objetivo.x, k);
    v.z = Mathf::lerp(v.z, objetivo.z, k);

    // Escalones: el suelo un poco por delante, si esta algo mas alto y no
    // mas que `escalon`, sube la capsula (no hace falta saltar).
    // (Mientras sube un escalon cuenta como en el suelo y no se pega al de abajo.)
    escalonT_ = std::max(0.0f, escalonT_ - dt);
    const Vec3 horizontal = plano(v);
    const bool saltando = salto_ && salto_->fase != "agachado";
    if ((enSuelo_ || escalonT_ > 0.0f) && !saltando && horizontal.length() > 0.2f) {
        const Vec3 d = horizontal.normalized();
        const Vec3 delanteP = pos + d * (kRadio + 0.18f);
        RaycastHit hit;
        if (sueloEn(delanteP, escalon + 0.05f, escalon + 0.1f, &hit) && pisable(hit)) {
            const float alto = hit.point.y - (pos.y - kMitad);
            if (alto > 0.03f && alto <= escalon) {
                escalonT_ = 0.25f;
                pos.y += std::min(alto, (3.0f + horizontal.length()) * dt);
                if (v.y < 0.0f) v.y = 0.0f;
                entity().setPosition(pos);
            }
        }
    }
    if (escalonT_ > 0.0f) enSuelo_ = true;
    // Bajando (escaleras, rampas): pegado al suelo si estaba en el.
    if (antes && !salto_ && escalonT_ <= 0.0f && v.y <= 0.1f && hayBajo && bajo.distance > 0.03f && bajo.distance < escalon &&
        pisable(bajo)) {
        pos.y -= bajo.distance;
        entity().setPosition(pos);
        v.y = 0.0f;
        enSuelo_ = true;
    }

    saltar(dt, v);
    girarEnSitio(dt, yawCam, quiere);
    accionar(dt);
    entity().setVelocity(v);
    pivote_.setRotation(Vec3{0, yaw_, 0});

    // Animator: la velocidad en los ejes del cuerpo, suavizada.
    const Vec3 real = plano(entity().velocity());
    const float ax = real.x * derecha(yaw_).x + real.z * derecha(yaw_).z;
    const float ay = real.x * delante(yaw_).x + real.z * delante(yaw_).z;
    const float s = Mathf::clamp01(10.0f * dt);
    animX_ = Mathf::lerp(animX_, ax, s);
    animY_ = Mathf::lerp(animY_, ay, s);
    modelo_.setAnimatorFloat("X", animX_);
    modelo_.setAnimatorFloat("Y", animY_);
    modelo_.setAnimatorBool("Moviendo", quiere);
    modelo_.setAnimatorBool("EnSuelo", enSuelo_);

    mirar(dt);
    if (pos.y < -20.0f) reaparecer();
}

// Salto sincronizado con la animacion: el agachado va rapido, despega justo
// en el fotograma en que la animacion deja el suelo y el vuelo dura lo que
// dure la fisica (la animacion se estira o se para) hasta tocar suelo.
void Personaje::saltar(float dt, Vec3& v) {
    if (Input::keyDown("space") && enSuelo_ && !salto_ && !mano_) {
        salto_ = Salto{0.0f, "agachado", plano(v).length() > 1.0f};
        giroSitio_.reset();
        modelo_.setAnimatorTrigger("Saltar");
    }
    if (!salto_) return;
    Salto& s = *salto_;
    const float vuelo = 2.0f * std::sqrt(2.0f * kG * alturaSalto) / kG;
    float velocidad = 1.0f;
    if (s.fase == "agachado") {
        velocidad = s.moviendo ? 3.2f : 1.8f;
        if (s.anim >= despegue) {
            v.y = std::sqrt(2.0f * kG * alturaSalto);
            s.fase = "subiendo";
            s.t = 0.0f;
        }
    } else if (s.fase == "subiendo") {
        s.t += dt;
        velocidad = (aterrizaje - despegue) / vuelo;
        if (s.anim >= aterrizaje - 0.06f) velocidad = 0.0f;  // sigue en el aire: se espera
        if (s.t > 0.15f && v.y <= 0.0f) s.fase = "cayendo";
    } else if (s.fase == "cayendo") {
        velocidad = s.anim >= aterrizaje - 0.06f ? 0.0f : (aterrizaje - despegue) / vuelo;
        if (enSuelo_) s.fase = "aterrizando";
    } else if (s.fase == "aterrizando") {
        s.anim = std::max<float>(s.anim, aterrizaje);
        velocidad = plano(v).length() > 0.5f ? 2.5f : 1.4f;
        if (s.anim >= duracionSalto * 0.98f || (plano(v).length() > 0.5f && s.anim > aterrizaje + 0.15f)) {
            salto_.reset();
            modelo_.setField("Animator", "speed", 1);
            return;
        }
    }
    s.anim += dt * velocidad;
    modelo_.setField("Animator", "speed", velocidad);
}

// Apuntando y quieto: si la camara se aleja del cuerpo, gira en el sitio con
// las animaciones de girar 90 o 180 grados (su giro se quito de la
// animacion al importarla: lo hace este script, a la vez).
void Personaje::girarEnSitio(float dt, float yawCam, bool quiere) {
    if (giroSitio_) {
        GiroSitio& g = *giroSitio_;
        g.t += dt * g.vel;
        yaw_ = g.desde + g.grados * suave(g.t / g.dur);
        if (g.t >= g.dur || quiere || salto_) {
            giroSitio_.reset();
            modelo_.setField("Animator", "speed", 1);
        }
        return;
    }
    if (!apuntando_ || quiere || salto_ || !enSuelo_ || mano_) return;
    const float diff = Mathf::deltaAngle(yaw_, yawCam);
    if (std::abs(diff) < 70.0f) return;
    const bool grande = std::abs(diff) > 135.0f;
    const bool izq = diff > 0.0f;
    const char* trigger = grande ? (izq ? "GirarIzq180" : "GirarDer180") : (izq ? "GirarIzq90" : "GirarDer90");
    const float dur = grande ? durGiro180 : durGiro90;
    // La animacion va algo mas rapida (girar 180 dura 1.6 s) y el giro real
    // es el que falta hasta la camara, con la forma de la animacion.
    const float vel = 1.5f;
    giroSitio_ = GiroSitio{yaw_, diff, dur, 0.0f, vel};
    modelo_.setField("Animator", "speed", vel);
    modelo_.setAnimatorTrigger(trigger);
}

// E cerca de una palanca: se pone delante, la mano derecha va al pomo (IK),
// la palanca baja con la mano agarrada y la mano vuelve.
void Personaje::accionar(float dt) {
    if (!mano_) {
        if (!Input::keyDown("e") || salto_) return;
        const Entity cerca = palancaCerca();
        if (!cerca) return;
        // Delante de la palanca y algo a su lado: la mano derecha queda frente al pomo.
        mano_ = Mano{cerca, 0.0f, false, cerca.position() + cerca.forward() * 0.55f + cerca.right() * 0.2f,
                     yawDe(plano(cerca.forward() * -1.0f))};
        modelo_.setIKTarget("right_hand", cerca.find("Pomo"));
        modelo_.setIKWeight("right_hand", 0);
        return;
    }
    Mano& m = *mano_;
    m.t += dt;
    // Colocarse (0..0.35 s), alcanzar (..0.6), bajar la palanca (..1.1), soltar (..1.45).
    const float ir = suave(m.t / 0.35f);
    const Vec3 p = entity().position();
    entity().setPosition(Vec3{Mathf::lerp(p.x, m.sitio.x, ir), p.y, Mathf::lerp(p.z, m.sitio.z, ir)});
    yaw_ = Mathf::lerpAngle(yaw_, m.yaw, ir);
    float peso = 0.0f;
    if (m.t < 0.6f) peso = suave((m.t - 0.15f) / 0.45f);
    else if (m.t < 1.1f) peso = 1.0f;
    else peso = 1.0f - suave((m.t - 1.1f) / 0.35f);
    modelo_.setIKWeight("right_hand", peso);
    if (m.t >= 0.6f && !m.hecho) {
)CPP"
R"CPP(        m.hecho = true;
        if (Palanca* palanca = m.palanca.script<Palanca>()) palanca->accionar();
    }
    if (m.t >= 1.45f) {
        modelo_.setIKTarget("right_hand", Value());
        mano_.reset();
    }
}

Entity Personaje::palancaCerca() const {
    Entity mejor;
    float dist = 1.8f;
    const Vec3 aqui = entity().position();
    for (const Value& item : Scene::findAllWithTag("Palanca").items()) {
        const Entity e = item.asEntity();
        const float d = Vec3::distance(aqui, e.position());
        const Palanca* p = e.script<Palanca>();
        if (d < dist && p != nullptr && !p->usada()) {
            mejor = e;
            dist = d;
        }
    }
    return mejor;
}

// Mirar (IK de la cabeza): apuntando, a donde apunta la camara; si no, a lo
// interesante que tenga delante (palancas, cristales, la estatua).
void Personaje::mirar(float dt) {
    std::optional<Vec3> objetivo;
    if (apuntando_ && camara_) {
        objetivo = camara_.position() + camara_.forward() * 30.0f;
    } else {
        // El mas cercano de delante; el que ya se mira sigue ganando salvo que
        // otro este 1 m mas cerca (con dos objetos juntos no se alterna).
        float mejor = 8.0f;
        Entity elegido;
        const Vec3 cabeza = entity().position() + Vec3{0, 0.7f, 0};
        for (const char* tag : {"Interes", "Cristal", "Palanca"}) {
            for (const Value& item : Scene::findAllWithTag(tag).items()) {
                const Entity e = item.asEntity();
                if (!e.active()) continue;
                const Vec3 a = e.position() - cabeza;
                float d = a.length();
                const float frente = (a.x * delante(yaw_).x + a.z * delante(yaw_).z) / std::max(d, 0.01f);
                if (frente <= 0.25f) continue;
                const std::string nombre = e.name();
                if (!mirando_.empty() && nombre == mirando_) d -= 1.0f;
                if (d < mejor) {
                    mejor = d;
                    elegido = e;
                }
            }
        }
        mirando_ = elegido ? elegido.name() : std::string();
        if (elegido) objetivo = elegido.position();
    }
    const float quiere = objetivo && !mano_ ? 1.0f : 0.0f;
    pesoMirar_ = Mathf::moveTowards(pesoMirar_, quiere, 2.5f * dt);
    if (objetivo) modelo_.setLookAt(*objetivo);
    modelo_.setIKWeight("look", pesoMirar_ * 0.85f);
}

void Personaje::reaparecer() {
    entity().setPosition(inicio_);
    entity().setVelocity(Vec3{});
    salto_.reset();
    modelo_.setField("Animator", "speed", 1);
}

std::string Personaje::estado() const {
    if (mano_) return "palanca";
    if (salto_) return "salto (" + salto_->fase + ")";
    if (giroSitio_) return "girando en el sitio";
    if (!enSuelo_) return "en el aire";
    return apuntando_ ? "apuntando" : "libre";
}

CRAMION_SCRIPT(Personaje)
)CPP"},
};

}  // namespace cramion::editor::locomotion_scripts

#endif  // CRAMION_EDITOR_TEMPLATE_LOCOMOTION_SCRIPTS_H

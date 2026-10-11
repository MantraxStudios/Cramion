#ifndef CRAMION_EDITOR_TEMPLATE_SCRIPTS_H
#define CRAMION_EDITOR_TEMPLATE_SCRIPTS_H

// Scripts de C++ de las plantillas basicas (ProjectTemplates.cpp). Cada
// plantilla escribe sus archivos en Assets/Scripts y los objetos de la
// escena llevan un CppScript con su clase. Las otras Template*Scripts.h
// tienen los de las plantillas grandes.

namespace cramion::editor {

// Un archivo de Assets/Scripts de una plantilla: su nombre y su codigo.
struct TemplateFile {
    const char* name;
    const char* code;
};

}  // namespace cramion::editor

namespace cramion::editor::template_scripts {

// Jugador en tercera persona (WASD, Shift, Espacio).
inline constexpr TemplateFile kPlayer[] = {
    {"Jugador.h",
R"CPP(// Jugador.h: jugador en tercera persona (WASD relativo a la camara, Shift
// para correr y Espacio para saltar).
#pragma once

#include <cramion/Script.h>

using namespace cramion;

// Va sobre un Rigidbody (la fisica lo empuja y lo hace caer); su hijo
// "Modelo" mira hacia donde camina.
class Jugador : public Script {
public:
    Property<float> velocidad{this, "velocidad", 6.0f, Range(0, 30)};
    Property<float> correr{this, "correr", 1.7f, Tooltip("Multiplicador con Shift")};
    Property<float> salto{this, "salto", 6.5f, Tooltip("m/s hacia arriba")};
    Property<float> aceleracion{this, "aceleracion", 12.0f};

    void start() override;
    void update(float dt) override;
    // Vuelve al punto de salida (los guardias lo llaman al atraparlo).
    void respawn();

private:
    bool enSuelo() const;

    Vec3 inicio_;
    Entity camara_;
    Entity modelo_;
};
)CPP"},
    {"Jugador.cpp",
R"CPP(// Jugador.cpp: jugador en tercera persona.
#include "Jugador.h"

void Jugador::start() {
    inicio_ = entity().position();
    camara_ = Scene::find("Main Camera");
    modelo_ = entity().find("Modelo").asEntity();
}

bool Jugador::enSuelo() const {
    // Desde justo debajo de la capsula (un rayo desde dentro la tocaria a ella).
    RaycastHit hit;
    return Physics::raycast(entity().position() + Vec3{0, -1.02f, 0}, Vec3{0, -1, 0}, 0.25f, &hit);
}

void Jugador::respawn() {
    entity().setPosition(inicio_);
    entity().setVelocity(Vec3{});
}

void Jugador::update(float dt) {
    Vec3 adelante{0, 0, -1};
    Vec3 derecha{1, 0, 0};
    if (camara_) {
        adelante = camara_.forward();
        adelante.y = 0;
        adelante = adelante.normalized();
        derecha = camara_.right();
        derecha.y = 0;
        derecha = derecha.normalized();
    }
    Vec3 direccion = derecha * Input::axis("Horizontal") + adelante * Input::axis("Vertical");
    if (direccion.length() > 1.0f) direccion = direccion.normalized();

    float rapidez = velocidad;
    if (Input::key("shift")) rapidez *= correr;
    const Vec3 objetivo = direccion * rapidez;
    Vec3 v = entity().velocity();
    const float t = Mathf::clamp01(aceleracion * dt);
    v.x = Mathf::lerp(v.x, objetivo.x, t);
    v.z = Mathf::lerp(v.z, objetivo.z, t);
    if (Input::keyDown("space") && enSuelo()) v.y = salto;
    entity().setVelocity(v);

    if (modelo_ && direccion.length() > 0.1f) modelo_.lookAt(modelo_.position() + direccion);
    if (entity().position().y < -20.0f) respawn();
}

CRAMION_SCRIPT(Jugador)
)CPP"},
};

// Camara en tercera persona que sigue a un objeto.
inline constexpr TemplateFile kThirdPersonCamera[] = {
    {"CamaraTercera.h",
R"CPP(// CamaraTercera.h: camara en tercera persona. Sigue al objetivo desde
// detras; clic derecho y raton para girarla, rueda para acercarla.
#pragma once

#include <cramion/Script.h>

#include <string>

using namespace cramion;

class CamaraTercera : public Script {
public:
    Property<std::string> objetivo{this, "objetivo", "Jugador", Tooltip("Nombre del objeto que sigue")};
    Property<float> distancia{this, "distancia", 7.0f, Range(3, 25)};
    Property<float> altura{this, "altura", 1.2f, Tooltip("Punto al que mira, sobre el objetivo")};
    Property<float> inclinacion{this, "inclinacion", 18.0f, Tooltip("Grados")};
    Property<float> sensibilidad{this, "sensibilidad", 0.25f};
    Property<float> suavizado{this, "suavizado", 12.0f};

    void start() override;
    void lateUpdate(float dt) override;
    // A quien sigue (otro script puede cambiarlo; sin objetivo se queda quieta).
    void setTarget(Entity e) { target_ = e; }

private:
    Entity target_;
    float yaw_ = 0.0f;
    float pitch_ = 0.0f;
};
)CPP"},
    {"CamaraTercera.cpp",
R"CPP(// CamaraTercera.cpp: camara en tercera persona.
#include "CamaraTercera.h"

#include <cmath>

void CamaraTercera::start() {
    target_ = Scene::find(objetivo.get());
    yaw_ = 0.0f;
    pitch_ = inclinacion;
}

void CamaraTercera::lateUpdate(float dt) {
    if (!target_) return;
    const Input::MouseState raton = Input::mouse();
    if (Input::mouseButton(1)) {
        yaw_ -= raton.dx * sensibilidad;
        pitch_ = Mathf::clamp(pitch_ + raton.dy * sensibilidad, -5.0f, 75.0f);
    }
    distancia = Mathf::clamp(distancia - raton.wheel, 3.0f, 25.0f);

    const float yaw = yaw_ * Mathf::deg2rad;
    const float pitch = pitch_ * Mathf::deg2rad;
    const Vec3 foco = target_.position() + Vec3{0, altura, 0};
    const Vec3 detras{std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
    const Vec3 deseada = foco + detras * distancia;
    entity().setPosition(Vec3::lerpClamped(entity().position(), deseada, suavizado * dt));
    entity().lookAt(foco);
}

CRAMION_SCRIPT(CamaraTercera)
)CPP"},
};

// Tercera persona: monedas y marcador.
inline constexpr TemplateFile kThirdPerson[] = {
    {"Marcador.h",
R"CPP(// Marcador.h: el marcador del HUD (monedas recogidas y record).
#pragma once

#include <cramion/Script.h>

using namespace cramion;

class Marcador : public Script {
public:
    void start() override;
    // Las monedas llaman aqui al recogerse.
    void sumar(int n);

private:
    void pintar();

    int puntos_ = 0;
    int total_ = 0;
};
)CPP"},
    {"Marcador.cpp",
R"CPP(// Marcador.cpp: cuenta las monedas recogidas y guarda el record.
#include "Marcador.h"

#include <algorithm>
#include <string>

void Marcador::start() {
    puntos_ = 0;
    total_ = static_cast<int>(Scene::findAllWithTag("Moneda").size());
    pintar();
}

void Marcador::pintar() {
    entity().set("text", "Monedas  " + std::to_string(puntos_) + " / " + std::to_string(total_));
}

void Marcador::sumar(int n) {
    puntos_ += n;
    pintar();
    if (puntos_ >= total_) {
        entity().set("text", "¡Todas las monedas!");
        entity().set("color", Vec3{1.0f, 0.85f, 0.25f});
        Prefs::setInt("record", std::max(Prefs::getInt("record", 0).asInt(), puntos_));
    }
}

CRAMION_SCRIPT(Marcador)
)CPP"},
    {"Moneda.cpp",
R"CPP(// Moneda.cpp: flota, gira y se recoge al tocarla (su collider es un trigger).
#include "Marcador.h"

#include <cmath>

class Moneda : public Script {
public:
    Property<int> valor{this, "valor", 1};

    void start() override {
        base_ = entity().position();
        fase_ = base_.x + base_.z;
    }

    void update(float dt) override {
        entity().rotate(Vec3{0, 120.0f * dt, 0});
        entity().setPosition(base_ + Vec3{0, std::sin(Time::time() * 2.5f + fase_) * 0.15f, 0});
    }

    void onTriggerEnter(Entity otro) override {
        if (otro.name() != "Jugador") return;
        if (Marcador* marcador = Scene::find("Marcador").script<Marcador>()) marcador->sumar(valor);
        entity().destroy();
    }

private:
    Vec3 base_;
    float fase_ = 0.0f;
};

CRAMION_SCRIPT(Moneda)
)CPP"},
};

// Escapa de los guardias (navegacion).
inline constexpr TemplateFile kNavigation[] = {
    {"Marcador.h",
R"CPP(// Marcador.h: el HUD del modo escapa (veces atrapado y el mensaje final).
#pragma once

#include <cramion/Script.h>

using namespace cramion;

class Marcador : public Script {
public:
    void start() override;
    // Los guardias llaman aqui al atrapar al jugador.
    void atrapado();
    // La meta llama aqui al llegar.
    void ganar();

private:
    void pintar();

    int atrapado_ = 0;
};
)CPP"},
    {"Guardia.cpp",
R"CPP(// Guardia.cpp: guardia con NavAgent. Patrulla por puntos al azar de la malla
// de navegacion y persigue al jugador si lo ve (sin paredes en medio). Si lo
// alcanza, el jugador vuelve al inicio.
#include "Jugador.h"
#include "Marcador.h"

class Guardia : public Script {
public:
    Property<float> radioPatrulla{this, "radioPatrulla", 10.0f};
    Property<float> vision{this, "vision", 11.0f};
    Property<float> alcance{this, "alcance", 1.3f};

    void start() override {
        casa_ = entity().position();
        jugador_ = Scene::find("Jugador");
        hud_ = Scene::find("Marcador");
    }

    void update(float dt) override {
        if (jugador_ && entity().distanceTo(jugador_).asFloat() < alcance) {
            if (Jugador* j = jugador_.script<Jugador>()) j->respawn();
            if (Marcador* m = hud_.script<Marcador>()) m->atrapado();
            entity().stopMoving();
            persigue_ = false;
            return;
        }

        if (veAlJugador()) {
            persigue_ = true;
            entity().moveTo(jugador_.position());
            return;
        }

        if (persigue_) {
            // Lo perdio de vista: va a donde lo vio por ultima vez.
            if (!entity().isMoving().asBool()) persigue_ = false;
            return;
        }

        if (!entity().isMoving().asBool()) {
            espera_ -= dt;
            if (espera_ <= 0.0f) {
                const Value punto = Navigation::randomPoint(casa_, radioPatrulla.get());
                if (!punto.isNil()) entity().moveTo(punto);
                espera_ = Random::range(0.5f, 2.5f).asFloat();
            }
        }
    }

private:
    bool veAlJugador() const {
        if (!jugador_) return false;
        if (entity().distanceTo(jugador_).asFloat() > vision) return false;
        // [llega, punto]: sin paredes de la malla en medio.
        return Navigation::raycast(entity().position(), jugador_.position())[0].asBool();
    }

    Vec3 casa_;
    Entity jugador_;
    Entity hud_;
    bool persigue_ = false;
    float espera_ = 0.0f;
};

CRAMION_SCRIPT(Guardia)
)CPP"},
    {"Marcador.cpp",
R"CPP(// Marcador.cpp: el HUD del modo escapa.
#include "Marcador.h"

#include <string>

void Marcador::start() {
    atrapado_ = 0;
    pintar();
}

void Marcador::pintar() {
    entity().set("text", "Llega a la meta sin que te atrapen  ·  Atrapado: " + std::to_string(atrapado_));
}

void Marcador::atrapado() {
    ++atrapado_;
    pintar();
}

void Marcador::ganar() {
    entity().set("text", "¡Lo lograste!");
    entity().set("color", Vec3{0.4f, 1.0f, 0.5f});
}

CRAMION_SCRIPT(Marcador)
)CPP"},
    {"Meta.cpp",
R"CPP(// Meta.cpp: al llegar se gana y el nivel empieza de nuevo a los 3 segundos.
#include "Marcador.h"

class Meta : public Script {
public:
    void update(float dt) override {
        entity().rotate(Vec3{0, 45.0f * dt, 0});
        if (reinicio_ > 0.0f) {
            reinicio_ -= dt;
            if (reinicio_ <= 0.0f) Scene::load(Scene::name());
        }
    }

    void onTriggerEnter(Entity otro) override {
        if (otro.name() != "Jugador" || reinicio_ > 0.0f) return;
        if (Marcador* hud = Scene::find("Marcador").script<Marcador>()) hud->ganar();
        reinicio_ = 3.0f;
    }

private:
    float reinicio_ = 0.0f;
};

CRAMION_SCRIPT(Meta)
)CPP"},
};

// Maquinas de estados: golpe y el texto del estado.
inline constexpr TemplateFile kStateMachines[] = {
    {"Enemigo.cpp",
R"CPP(// Enemigo.cpp: hace cada estado de su maquina (IA/Enemigo.crfsm). La maquina
// decide cuando cambiar de estado con sus variables y transiciones; este
// script le pone `distancia` cada frame y recibe los mensajes:
//   OnStateEnter   al entrar en un estado (empezar a moverse, pararse...)
//   OnStateUpdate  cada frame del estado (perseguir, golpear, curarse...)
// Patrullar recorre los objetos con el tag "Waypoint" (en orden de nombre).
#include <cramion/Script.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace cramion;

class Enemigo : public Script {
public:
    Property<float> cadencia{this, "cadencia", 1.0f, Tooltip("Segundos entre golpes")};
    Property<float> empujon{this, "empujon", 6.0f, Tooltip("Lo que empuja al jugador al golpearlo")};
    Property<float> curacion{this, "curacion", 12.0f, Tooltip("Vida por segundo mientras huye")};

    void start() override {
        sm_ = entity().getStateMachine();
        casa_ = entity().position();
        set("casa", casa_);
        const Entity objetivo = get("objetivo").asEntity();
        jugador_ = objetivo.id() != 0 ? objetivo : Scene::find("Jugador");
        for (const Value& w : Scene::findAllWithTag("Waypoint").items()) ruta_.push_back(w.asEntity());
        std::sort(ruta_.begin(), ruta_.end(), [](const Entity& a, const Entity& b) { return a.name() < b.name(); });

        on("OnStateEnter", [this](const Value& v) { entrar(v["state"].asString()); });
        on("OnStateUpdate", [this](const Value& v) { cadaFrame(v["state"].asString(), v["dt"].asFloat()); });
    }

    void update(float) override {
        if (sm_.isNil() || jugador_.id() == 0) return;
        set("distancia", Vec3::distance(entity().position(), jugador_.position()));
    }

private:
    Value get(const char* variable) const { return sm_.isNil() ? Value{} : sm_.call("get", {variable}); }
    void set(const char* variable, const Value& valor) const {
        if (!sm_.isNil()) sm_.call("set", {variable, valor});
    }

    void entrar(const std::string& estado) {
        if (estado == "Patrullar") {
            siguientePunto();
        } else if (estado == "Atacar") {
            entity().stopMoving();
            recarga_ = 0.3f;
        } else if (estado == "Huir") {
            huir();
        } else if (estado == "Volver") {
            entity().moveTo(casa_);
        }
        repensar_ = 0.0f;
    }

    void cadaFrame(const std::string& estado, float dt) {
        repensar_ -= dt;
        if (estado == "Patrullar") {
            if (!entity().isMoving().truthy()) siguientePunto();
        } else if (estado == "Perseguir") {
            // El destino cambia: se recalcula el camino cada poco.
            if (repensar_ <= 0.0f && jugador_.id() != 0) {
                repensar_ = 0.25f;
                entity().moveTo(jugador_.position());
            }
        } else if (estado == "Atacar") {
            atacar(dt);
        } else if (estado == "Huir") {
            set("vida", std::min(100.0, get("vida").asNumber() + curacion * dt));
            if (!entity().isMoving().truthy()) huir();
        } else if (estado == "Volver") {
            // En casa: otra vez a patrullar.
            if (Vec3::distance(entity().position(), casa_) < 1.5f || !entity().isMoving().truthy()) sm_.call("go", {"Patrullar"});
        }
    }

    void siguientePunto() {
        if (ruta_.empty()) {
            // Sin ruta: un punto al azar cerca de casa.
            const Value p = Navigation::randomPoint(casa_, get("radioPatrulla").asFloat(8.0f));
            if (!p.isNil()) entity().moveTo(p.asVec3());
            return;
        }
        punto_ = (punto_ + 1) % ruta_.size();
        entity().moveTo(ruta_[punto_].position());
    }

    void huir() {
        if (jugador_.id() == 0) return;
        const Vec3 yo = entity().position();
        Vec3 lejos = yo - jugador_.position();
        lejos.y = 0.0f;
        const Value p = Navigation::randomPoint(yo + lejos.normalized() * 10.0f, 3.0f);
        entity().moveTo(p.isNil() ? casa_ : p.asVec3());
    }

    void atacar(float dt) {
        if (jugador_.id() == 0) return;
        const Vec3 yo = entity().position();
        const Vec3 suyo = jugador_.position();
        entity().lookAt(Vec3{suyo.x, yo.y, suyo.z});
        recarga_ -= dt;
        if (recarga_ > 0.0f) return;
        recarga_ = cadencia;
        Vec3 direccion = suyo - yo;
        direccion.y = 0.0f;
        jugador_.addForce(direccion.normalized() * empujon + Vec3{0.0f, empujon * 0.4f, 0.0f}, ForceMode::VelocityChange);
        Debug::log(entity().name() + " te golpea (" + std::to_string(get("danio").asInt()) + ")");
    }

    Value sm_;
    Entity jugador_;
    Vec3 casa_;
    std::vector<Entity> ruta_;
    std::size_t punto_ = static_cast<std::size_t>(-1);  // el primero es el 0
    float recarga_ = 0.0f;
    float repensar_ = 0.0f;
};

CRAMION_SCRIPT(Enemigo)
)CPP"},
    {"Estados.cpp",
R"CPP(// Estados.cpp: el HUD con el estado de la maquina de cada enemigo y su vida.
#include <cramion/Script.h>

#include <cmath>
#include <string>

using namespace cramion;

class Estados : public Script {
public:
    void update(float) override {
        std::string lineas = "WASD moverse  ·  F golpear (con poca vida huyen)";
        for (const Value& item : Scene::findAllWithTag("Enemigo").items()) {
            const Entity e = item.asEntity();
            const Value sm = e.getStateMachine();
            if (sm.isNil()) continue;
            const Value estado = sm.get("state");
            lineas += "\n" + e.name() + ":  " + (estado.isNil() ? std::string("-") : estado.asString()) + "   (vida " +
                      std::to_string(static_cast<int>(std::floor(sm.call("get", {"vida"}).asNumber()))) + ")";
        }
        entity().set("text", lineas);
    }
};

CRAMION_SCRIPT(Estados)
)CPP"},
    {"Golpe.cpp",
R"CPP(// Golpe.cpp: golpear (F) resta vida a los enemigos cercanos. Su maquina de
// estados lo ve en la variable "vida": con menos de 30 pasan a Huir
// (transicion desde Cualquier estado) y, curados, vuelven a casa y a patrullar.
#include <cramion/Script.h>

#include <cmath>
#include <string>

using namespace cramion;

class Golpe : public Script {
public:
    Property<float> alcance{this, "alcance", 3.5f};
    Property<float> danio{this, "danio", 35.0f};

    void update(float) override {
        if (!Input::keyDown("f")) return;
        for (const Value& item : Scene::findAllWithTag("Enemigo").items()) {
            const Entity e = item.asEntity();
            if (entity().distanceTo(e).asFloat() > alcance) continue;
            const Value sm = e.getStateMachine();
            if (sm.isNil()) continue;
            const Value antes = sm.call("get", {"vida"});
            const double vida = (antes.isNil() ? 100.0 : antes.asNumber()) - danio;
            sm.call("set", {"vida", vida});
            Debug::log("Golpe a " + e.name() + ": vida " + std::to_string(static_cast<int>(std::floor(vida))));
        }
    }
};

CRAMION_SCRIPT(Golpe)
)CPP"},
};

// Mundo de bloques: el jugador y todo el juego.
inline constexpr TemplateFile kVoxel[] = {
    {"JugadorBloques.cpp",
R"CPP(// JugadorBloques.cpp: jugador de un mundo de bloques en primera persona (como
// Minecraft).
//
// Clic en la vista para jugar (captura el raton). WASD andar, Espacio saltar
// o nadar, Ctrl correr. Clic izquierdo (mantener) rompe, derecho pone el
// bloque elegido o come, central copia (creativo). Rueda o 1-9 eligen. E abre
// el inventario y el crafteo, Escape la pausa. En creativo, F vuela.
//
// Supervivencia: vida, hambre y aire; dano por caida y al ahogarse; los
// bloques sueltan objetos que se recogen; la piedra y las menas piden un
// pico. El mundo, la posicion, el inventario y la vida se guardan solos.
#include <cramion/Script.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace cramion;

namespace {

const Vec3 kMitad{0.3f, 0.9f, 0.3f};  // la caja del jugador (0.6 x 1.8 m)
constexpr float kOjos = 0.72f;        // del centro de la caja a los ojos (1.62 m del suelo)
constexpr int kHuecos = 36;           // 1..9 la barra, 10..36 la mochila
constexpr int kPila = 64;
const std::string kIconos = "Voxel/Iconos/";

// Objetos que no son bloques.
struct Objeto {
    const char* item;
    const char* label;
    int comida = 0;
    float pico = 0.0f;  // multiplica la velocidad al romper piedra
    int max = 0;        // 0 = una pila normal
    bool fuerte = false;
    Vec3 color;
};
const std::vector<Objeto> kObjetos = {
    {"palo", "Palo", 0, 0.0f, 0, false, {0.55f, 0.38f, 0.2f}},
    {"carbon", "Carbón", 0, 0.0f, 0, false, {0.12f, 0.12f, 0.13f}},
    {"manzana", "Manzana", 4, 0.0f, 0, false, {0.85f, 0.12f, 0.1f}},
    {"pico_madera", "Pico de madera", 0, 2.5f, 1, false, {0.6f, 0.45f, 0.25f}},
    {"pico_piedra", "Pico de piedra", 0, 5.0f, 1, true, {0.55f, 0.55f, 0.57f}},
};

// Crafteo. `mesa`: hace falta una mesa de trabajo a menos de 4 bloques.
struct Receta {
    std::string da;
    int n;
    std::vector<std::pair<std::string, int>> pide;
    bool mesa = false;
};
const std::vector<Receta> kRecetas = {
    {"oak_planks", 4, {{"oak_log", 1}}},
    {"oak_planks", 4, {{"birch_log", 1}}},
    {"oak_planks", 4, {{"spruce_log", 1}}},
    {"palo", 4, {{"oak_planks", 2}}},
    {"crafting_table", 1, {{"oak_planks", 4}}},
    {"torch", 4, {{"palo", 1}, {"carbon", 1}}},
    {"pico_madera", 1, {{"oak_planks", 3}, {"palo", 2}}, true},
    {"pico_piedra", 1, {{"cobblestone", 3}, {"palo", 2}}, true},
    {"stone_bricks", 4, {{"cobblestone", 4}}, true},
    {"sandstone", 1, {{"sand", 4}}},
    {"glass", 1, {{"sand", 2}, {"carbon", 1}}, true},
    {"bricks", 1, {{"clay", 4}}, true},
    {"glowstone", 1, {{"torch", 4}, {"glass", 1}}, true},
};

// Bloques de la barra en creativo.
const std::vector<std::string> kCreativo = {"grass", "dirt", "stone", "cobblestone", "oak_planks", "oak_log", "glass", "torch", "bricks"};

const Objeto* objeto(const std::string& item) {
    for (const Objeto& o : kObjetos) {
        if (item == o.item) return &o;
    }
    return nullptr;
}

bool esBloque(const std::string& item) { return objeto(item) == nullptr && Voxel::blockId(item).asInt() != 0; }

std::string nombre(const std::string& item) {
    if (const Objeto* o = objeto(item)) return o->label;
    const Value info = Voxel::blockInfo(item);
    return info.isNil() ? item : info["label"].asString();
}

std::string icono(const std::string& item) { return kIconos + item + ".png"; }

int maximo(const std::string& item) {
    const Objeto* o = objeto(item);
    return o != nullptr && o->max > 0 ? o->max : kPila;
}

bool contiene(const std::string& texto, const char* parte) { return texto.find(parte) != std::string::npos; }

// Bloques de piedra: sin pico tardan mucho y no sueltan nada.
bool dePiedra(const Value& info) {
    const std::string n = info["name"].asString();
    return contiene(n, "stone") || contiene(n, "_ore") || contiene(n, "brick");
}

Vec3 colorObjeto(const std::string& item) {
    if (esBloque(item)) return Voxel::blockColor(item).asVec3();
    const Objeto* o = objeto(item);
    return o != nullptr ? o->color : Vec3{1, 1, 1};
}

float azar(float a, float b) { return Random::range(a, b).asFloat(); }

// Un objeto suelto en el suelo y un trocito de bloque al romperlo.
struct Suelto {
    Entity e;
    std::string item;
    int n = 0;
    Vec3 pos, vel;
    float t = 0.0f;
};
struct Chispa {
    Entity e;
    Vec3 pos, vel;
    float vida = 0.0f;
};

struct Hueco {
    std::string item;  // vacio = libre
    int n = 0;
};

}  // namespace

class JugadorBloques : public Script {
public:
    Property<std::string> mundo{this, "mundo", "Mi mundo"};
    Property<int> semilla{this, "semilla", 0, Tooltip("0 = al azar al crear el mundo")};
    Property<bool> creativo{this, "creativo", false, Tooltip("Romper al instante, volar, bloques sin fin")};
    Property<float> velocidad{this, "velocidad", 4.3f};
    Property<float> correr{this, "correr", 1.5f};
    Property<float> salto{this, "salto", 8.4f};
    Property<float> gravedad{this, "gravedad", 28.0f};
    Property<float> sensibilidad{this, "sensibilidad", 0.12f};
    Property<float> alcance{this, "alcance", 5.0f};

    void start() override;
    void update(float dt) override;
    void onDestroy() override { guardar(); }

private:
    // --- UI ---
    struct Ui {
        Entity info, aviso, nombre, seleccion, dano, inventario, muerte, pausa;
        std::array<Entity, 10> barra, corazones, comida, burbujas;
        std::array<Entity, kHuecos + 1> huecos;
        std::vector<Entity> recetas;
    };

    void buscarInterfaz();
    bool panelAbierto() const { return muerto_ || abierto(ui_.inventario) || abierto(ui_.pausa); }
    static bool abierto(const Entity& e) { return e && e.active(); }
    void mover(float dt);
    void colocarCamara();
    bool ocupa(const Vec3& p) const;
    void apuntar(const Value& golpe);
    const Objeto* herramienta() const;
    float tiempoRomper(const Value& info) const;
    std::string suelta(const Value& info) const;
    void romper(const Vec3& b, const Value& info);
    void bloques(float dt);
    void elegir();
    int dar(const std::string& item, int n);
    void quitar(int hueco, int n);
    int cuenta(const std::string& item) const;
    void gastar(const std::string& item, int n);
    bool cercaDeMesa() const;
    bool puedeCraftearSinMesa(const Receta& receta) const;
    bool puedeCraftear(const Receta& receta) const;
    bool craftear(int i);
    void abrirInventario(bool abrir);
    void pausar(bool pausa);
    void reaparecer();
    void cansar(float cantidad);
    void herir(int puntos, const std::string& causa);
    void estadisticas(float dt);
    Value mallaDe(const std::string& item, float tamano);
    void soltarObjeto(const std::string& item, int n, const Vec3& pos);
    void chispear(const Vec3& b, const Value& info);
    void efectos(float dt);
    void recoger(float dt);
    void mostrarAviso(const std::string& texto);
    void pintarHueco(const Entity& hueco, const Hueco& dato) const;
    void pintarBarra();
    void pintarEstado();
    void pintarInventario();
    void pintarTodo();
    void pintarInfo(const std::string& aviso = {});
    void cargarInventario();
    void guardar();

    float yaw_ = 0.0f, pitch_ = -10.0f;
    Vec3 vel_;
    Vec3 centro_, inicio_;
    bool volando_ = false, enSuelo_ = false, corriendo_ = false, listo_ = false, muerto_ = false, mesaCerca_ = false;
    int elegido_ = 1;
    float progreso_ = 0.0f;
    std::string rompiendo_;
    float guardado_ = 0.0f;
    int vida_ = 20, hambre_ = 20;
    float aire_ = 10.0f;
    float relojHambre_ = 0.0f, relojVida_ = 0.0f, relojAire_ = 0.0f;
    float cansancio_ = 0.0f;
    float caidaDesde_ = 0.0f;
    float flash_ = 0.0f;
    float relojAviso_ = 0.0f, relojNombre_ = 0.0f;
    std::array<Hueco, kHuecos + 1> inv_;
    std::vector<Suelto> objetos_;
    std::vector<Chispa> chispas_;
    std::map<std::string, Value> mallas_;
    Value apuntado_;
    Entity contorno_, grietas_;
    std::array<Value, 10> etapas_;
    Ui ui_;
};

// ------------------------------------------------------------------ inicio

void JugadorBloques::start() {
    on("OnReceta", [this](const Value& v) {
        // El boton se llama "Receta N".
        const std::string n = v.asEntity().name();
        const std::size_t at = n.find_first_of("0123456789");
        if (at == std::string::npos) return;
        craftear(std::atoi(n.c_str() + at));
        mesaCerca_ = cercaDeMesa();
        pintarTodo();
    });
    // Clic en un hueco de la mochila: lo cambia por el elegido de la barra.
    on("OnHueco", [this](const Value& v) {
        const std::string n = v.asEntity().name();
        const std::size_t at = n.find_first_of("0123456789");
        if (at == std::string::npos) return;
        const int i = std::atoi(n.c_str() + at);
        if (i < 1 || i > kHuecos || i == elegido_) return;
        std::swap(inv_[i], inv_[elegido_]);
        pintarTodo();
    });
    on("OnReaparecer", [this](const Value&) { reaparecer(); });
    on("OnSeguir", [this](const Value&) { pausar(false); });
    on("OnGuardar", [this](const Value&) {
        guardar();
        mostrarAviso("Mundo guardado");
    });
    on("OnSalir", [this](const Value&) {
        guardar();
        Game::quit();
    });
    buscarInterfaz();

    // El mundo guardado con ese nombre, o uno nuevo.
    if (!Voxel::loadWorld(mundo.get()).asBool()) {
        int s = semilla;
        if (s == 0) s = Random::int_(1, 2000000000).asInt();
        Voxel::newWorld(mundo.get(), s);
        Voxel::setMeta("mode", creativo ? "creativo" : "supervivencia");
    }
    inicio_ = Vec3{0.5f, Voxel::surfaceHeight(0, 0).asFloat() + 1.0f + kMitad.y + 0.1f, 0.5f};
    float x, y, z, yaw, pitch;
    if (std::sscanf(Voxel::getMeta("player", "").asString().c_str(), "%f %f %f %f %f", &x, &y, &z, &yaw, &pitch) == 5) {
        centro_ = Vec3{x, y, z};
        yaw_ = yaw;
        pitch_ = pitch;
    } else {
        centro_ = inicio_;
    }
    vida_ = std::atoi(Voxel::getMeta("vida", "20").asString().c_str());
    hambre_ = std::atoi(Voxel::getMeta("hambre", "20").asString().c_str());
    cargarInventario();
    caidaDesde_ = centro_.y;

    // Contorno del bloque apuntado y grietas al romper (mallas por codigo).
    contorno_ = Scene::create("Contorno");
    const Value marco = Mesh::wireCube(1.002f, 0.014f);
    marco.call("setMaterial", {0, Value{{"color", Vec3{0.02f, 0.02f, 0.02f}}, {"roughness", 1}}});
    contorno_.set("mesh", marco);
    contorno_.set("castShadows", false);
    contorno_.setActive(false);
    grietas_ = Scene::create("Grietas");
    grietas_.set("castShadows", false);
    grietas_.setActive(false);
    for (int i = 0; i <= 9; ++i) {
        const Value m = Mesh::cube(1.006f);
        m.call("setMaterial", {0, Value{{"texture", "Voxel/Grietas/grieta_" + std::to_string(i) + ".png"}, {"roughness", 1}}});
        etapas_[i] = m;
    }

    colocarCamara();
    pintarTodo();
}

void JugadorBloques::buscarInterfaz() {
    ui_.info = Scene::find("Info");
    ui_.aviso = Scene::find("Aviso");
    ui_.nombre = Scene::find("NombreObjeto");
    ui_.seleccion = Scene::find("Seleccion");
    ui_.dano = Scene::find("Dano");
    ui_.inventario = Scene::find("Inventario");
    ui_.muerte = Scene::find("Muerte");
    ui_.pausa = Scene::find("Pausa");
    for (int i = 1; i <= 9; ++i) ui_.barra[i] = Scene::find("Hueco " + std::to_string(i));
    for (int i = 1; i <= 10; ++i) {
        ui_.corazones[i - 1] = Scene::find("Corazon " + std::to_string(i));
        ui_.comida[i - 1] = Scene::find("Comida " + std::to_string(i));
        ui_.burbujas[i - 1] = Scene::find("Aire " + std::to_string(i));
    }
    for (int i = 1; i <= kHuecos; ++i) ui_.huecos[i] = Scene::find("Inv " + std::to_string(i));
    for (std::size_t i = 1; i <= kRecetas.size(); ++i) ui_.recetas.push_back(Scene::find("Receta " + std::to_string(i)));
}

)CPP"
R"CPP(// ------------------------------------------------------------------ cada frame

void JugadorBloques::update(float dt) {
    dt = std::min(dt, 0.05f);
    efectos(dt);
    const bool panel = panelAbierto();
    // Teclas de los paneles.
    if (Input::keyDown("escape")) {
        if (abierto(ui_.inventario)) abrirInventario(false);
        else if (!muerto_) pausar(!abierto(ui_.pausa));
        return;
    }
    if (Input::keyDown("e") && !muerto_ && !abierto(ui_.pausa)) {
        abrirInventario(!abierto(ui_.inventario));
        return;
    }
    if (panel) return;

    // Raton: clic en la vista lo captura.
    const bool mirando = Input::isCursorLocked().asBool();
    if (!mirando) {
        if (Input::mouseButton(0, 1)) Input::lockCursor(true);
    } else {
        const Input::MouseState d = Input::mouse();
        yaw_ -= d.dx * sensibilidad;
        pitch_ = Mathf::clamp(pitch_ - d.dy * sensibilidad, -89.0f, 89.0f);
    }
    // Mientras se genera el suelo, quieto (no se cae del mundo).
    if (!Voxel::isReady(centro_).asBool()) {
        colocarCamara();
        pintarInfo("Generando el mundo...");
        return;
    }
    if (!listo_) {
        // Si aparece dentro de un arbol o de una colina, sube hasta el aire.
        while (Voxel::boxCollides(centro_, kMitad).asBool() && centro_.y < 250.0f) centro_ = centro_ + Vec3{0, 1, 0};
        caidaDesde_ = centro_.y;
        listo_ = true;
    }
    mover(dt);
    colocarCamara();
    if (mirando) bloques(dt);
    else apuntar(Value());
    elegir();
    estadisticas(dt);
    recoger(dt);
    pintarInfo();
    // Guardado automatico cada minuto.
    guardado_ += dt;
    if (guardado_ > 60.0f) {
        guardado_ = 0.0f;
        guardar();
    }
}

void JugadorBloques::mover(float dt) {
    const float yaw = yaw_ * Mathf::deg2rad;
    const Vec3 adelante{-std::sin(yaw), 0, -std::cos(yaw)};
    const Vec3 derecha{std::cos(yaw), 0, -std::sin(yaw)};
    Vec3 dir = adelante * Input::axis("Vertical") + derecha * Input::axis("Horizontal");
    if (dir.length() > 1.0f) dir = dir.normalized();
    float rapidez = velocidad;
    corriendo_ = Input::key("ctrl") && dir.length() > 0.1f && hambre_ > 6;
    if (corriendo_) rapidez *= correr;
    if (creativo && Input::keyDown("f")) {
        volando_ = !volando_;
        vel_ = Vec3{};
    }
    const bool agua = Voxel::inWater(centro_).asBool();
    Vec3 v = vel_;
    if (volando_) {
        rapidez *= 2.5f;
        v = dir * rapidez;
        if (Input::key("space")) v.y = rapidez;
        else if (Input::key("shift")) v.y = -rapidez;
        else v.y = 0.0f;
    } else {
        if (agua) rapidez *= 0.5f;
        const float t = Mathf::clamp01((enSuelo_ ? 14.0f : 3.0f) * dt);
        v.x = Mathf::lerp(v.x, dir.x * rapidez, t);
        v.z = Mathf::lerp(v.z, dir.z * rapidez, t);
        if (agua) {
            const float objetivo = Input::key("space") ? 3.5f : -2.0f;
            v.y = Mathf::lerp(v.y, objetivo, Mathf::clamp01(4.0f * dt));
        } else {
            v.y = std::max(v.y - gravedad * dt, -50.0f);
            if (enSuelo_ && Input::key("space")) {
                v.y = salto;
                cansar(0.05f);
            }
        }
    }
    const Vec3 antes = centro_;
    // [posicion, enSuelo, techo, pared]
    const Value r = Voxel::moveBox(centro_, kMitad, v * dt);
    const Vec3 pos = r[0].asVec3();
    const bool suelo = r[1].asBool();
    const bool techo = r[2].asBool();
    if (suelo && v.y < 0.0f) v.y = 0.0f;
    if (techo && v.y > 0.0f) v.y = 0.0f;
    // Dano por caida: desde el punto mas alto del salto, 1 por metro pasados 3.
    if (volando_ || agua) {
        caidaDesde_ = pos.y;
    } else if (!suelo) {
        caidaDesde_ = std::max(caidaDesde_, pos.y);
    } else if (!enSuelo_) {
        const float metros = caidaDesde_ - pos.y;
        if (metros > 3.5f && !creativo) herir(static_cast<int>(std::floor(metros - 3.0f)), "Caíste desde muy alto");
        caidaDesde_ = pos.y;
    } else {
        caidaDesde_ = pos.y;
    }
    enSuelo_ = suelo;
    centro_ = pos;
    vel_ = v;
    const float paso = Vec3{pos.x - antes.x, 0, pos.z - antes.z}.length();
    cansar(paso * (corriendo_ ? 0.1f : 0.01f));
    if (centro_.y < -20.0f) herir(40, "Te caíste del mundo");
}

void JugadorBloques::colocarCamara() {
    entity().setPosition(centro_ + Vec3{0, kOjos, 0});
    entity().setRotation(Vec3{pitch_, yaw_, 0});
}

// El bloque en `p` (esquina) se solapa con el jugador.
bool JugadorBloques::ocupa(const Vec3& p) const {
    const Vec3 c = centro_;
    return p.x < c.x + kMitad.x && p.x + 1 > c.x - kMitad.x && p.y < c.y + kMitad.y && p.y + 1 > c.y - kMitad.y &&
           p.z < c.z + kMitad.z && p.z + 1 > c.z - kMitad.z;
}

// ------------------------------------------------------------------ bloques

void JugadorBloques::apuntar(const Value& golpe) {
    apuntado_ = golpe;
    if (!golpe.isNil()) {
        contorno_.setPosition(golpe["block"].asVec3() + Vec3{0.5f, 0.5f, 0.5f});
        contorno_.setActive(true);
    } else {
        contorno_.setActive(false);
        grietas_.setActive(false);
    }
}

const Objeto* JugadorBloques::herramienta() const {
    const Objeto* o = objeto(inv_[elegido_].item);
    return o != nullptr && o->pico > 0.0f ? o : nullptr;
}

// Segundos para romper un bloque con lo que se lleva en la mano.
float JugadorBloques::tiempoRomper(const Value& info) const {
    float t = info["hardness"].asFloat();
    if (dePiedra(info)) {
        if (const Objeto* pico = herramienta()) t /= pico->pico;
        else t *= 5.0f;
    }
    return std::max(t, 0.05f);
}

// Lo que suelta un bloque (vacio = nada).
std::string JugadorBloques::suelta(const Value& info) const {
    const std::string n = info["name"].asString();
    if (contiene(n, "leaves")) return Random::value().asFloat() < 0.08f ? "manzana" : "";
    if (n == "tall_grass" || n == "red_flower" || n == "yellow_flower" || n == "dead_bush" || n == "glass" || n == "ice") return "";
    if (dePiedra(info)) {
        const Objeto* pico = herramienta();
        if (pico == nullptr) return "";
        if ((n == "iron_ore" || n == "gold_ore" || n == "diamond_ore") && !pico->fuerte) return "";
    }
    if (n == "coal_ore") return "carbon";
    const Value drop = Voxel::blockInfo(info["drop"]);
    return drop.isNil() ? n : drop["name"].asString();
}

void JugadorBloques::romper(const Vec3& b, const Value& info) {
    Voxel::setBlock(b.x, b.y, b.z, "air");
    chispear(b, info);
    cansar(0.005f);
    if (!creativo) {
        const std::string item = suelta(info);
        if (!item.empty()) soltarObjeto(item, 1, b + Vec3{0.5f, 0.5f, 0.5f});
    }
}

void JugadorBloques::bloques(float dt) {
    const Value golpe = Voxel::raycast(entity().position(), entity().forward(), alcance.get());
    apuntar(golpe);
    // Romper: al instante en creativo; si no, manteniendo segun la dureza y la herramienta.
    if (!golpe.isNil() && Input::mouseButton(0)) {
        const Value info = Voxel::blockInfo(golpe["id"]);
        const Vec3 b = golpe["block"].asVec3();
        const std::string clave = std::to_string(static_cast<int>(b.x)) + "," + std::to_string(static_cast<int>(b.y)) + "," +
                                  std::to_string(static_cast<int>(b.z));
        if (rompiendo_ != clave) {
            rompiendo_ = clave;
            progreso_ = 0.0f;
        }
        if (info["hardness"].asFloat() >= 0.0f) {
            if (creativo) {
                if (Input::mouseButton(0, 1)) romper(b, info);
            } else {
                progreso_ += dt / tiempoRomper(info);
                if (progreso_ >= 1.0f) {
                    romper(b, info);
                    progreso_ = 0.0f;
                    rompiendo_.clear();
                }
            }
        }
    } else {
        rompiendo_.clear();
        progreso_ = 0.0f;
    }
    // Grietas sobre el bloque segun lo que falta para romperlo.
    if (!golpe.isNil() && progreso_ > 0.0f) {
        grietas_.set("mesh", etapas_[std::min(9, static_cast<int>(std::floor(progreso_ * 10.0f)))]);
        grietas_.setPosition(golpe["block"].asVec3() + Vec3{0.5f, 0.5f, 0.5f});
        grietas_.setActive(true);
    } else {
        grietas_.setActive(false);
    }
    // Clic derecho: comer lo que se lleva o poner el bloque junto a la cara apuntada.
    if (Input::mouseButton(1, 1)) {
        const Hueco& hueco = inv_[elegido_];
        const Objeto* o = objeto(hueco.item);
        if (o != nullptr && o->comida > 0) {
            if (hambre_ < 20) {
                hambre_ = std::min(20, hambre_ + o->comida);
                quitar(elegido_, 1);
                mostrarAviso("Ñam");
            }
        } else if (!golpe.isNil() && !hueco.item.empty() && esBloque(hueco.item)) {
            const Vec3 p = golpe["block"].asVec3() + golpe["normal"].asVec3();
            const Value info = Voxel::blockInfo(hueco.item);
            if (!(info["solid"].asBool() && ocupa(p)) && Voxel::setBlock(p.x, p.y, p.z, hueco.item).asBool()) {
                if (!creativo) quitar(elegido_, 1);
            }
        }
    }
    // Copiar el bloque apuntado a la barra (creativo).
    if (creativo && !golpe.isNil() && Input::mouseButton(2, 1)) {
        inv_[elegido_] = Hueco{Voxel::blockInfo(golpe["id"])["name"].asString(), kPila};
        pintarTodo();
    }
}

void JugadorBloques::elegir() {
    const int antes = elegido_;
    for (int i = 1; i <= 9; ++i) {
        if (Input::keyDown(std::to_string(i))) elegido_ = i;
    }
    const float rueda = Input::mouse().wheel;
    if (rueda > 0.0f) --elegido_;
    else if (rueda < 0.0f) ++elegido_;
    if (elegido_ < 1) elegido_ = 9;
    else if (elegido_ > 9) elegido_ = 1;
    if (antes != elegido_) {
        relojNombre_ = 2.0f;
        pintarBarra();
    }
}

// ------------------------------------------------------------------ inventario

// Mete `n` de `item`; devuelve los que no caben.
int JugadorBloques::dar(const std::string& item, int n) {
    const int max = maximo(item);
    for (int i = 1; i <= kHuecos && n > 0; ++i) {
        Hueco& h = inv_[i];
        if (h.item == item && h.n < max) {
            const int cabe = std::min(n, max - h.n);
            h.n += cabe;
            n -= cabe;
        }
    }
    for (int i = 1; i <= kHuecos && n > 0; ++i) {
        if (inv_[i].item.empty()) {
            const int cabe = std::min(n, max);
            inv_[i] = Hueco{item, cabe};
            n -= cabe;
        }
    }
    pintarTodo();
    return n;
}

void JugadorBloques::quitar(int hueco, int n) {
    Hueco& h = inv_[hueco];
    if (h.item.empty()) return;
    h.n -= n;
    if (h.n <= 0) h = Hueco{};
    pintarTodo();
}

int JugadorBloques::cuenta(const std::string& item) const {
    int total = 0;
    for (int i = 1; i <= kHuecos; ++i) {
        if (inv_[i].item == item) total += inv_[i].n;
    }
    return total;
}

void JugadorBloques::gastar(const std::string& item, int n) {
    for (int i = kHuecos; i >= 1 && n > 0; --i) {
        Hueco& h = inv_[i];
        if (h.item != item) continue;
        const int toma = std::min(n, h.n);
        h.n -= toma;
        n -= toma;
        if (h.n <= 0) h = Hueco{};
    }
}

// Hay una mesa de trabajo a menos de 4 bloques.
bool JugadorBloques::cercaDeMesa() const {
    const int cx = static_cast<int>(std::floor(centro_.x));
    const int cy = static_cast<int>(std::floor(centro_.y));
    const int cz = static_cast<int>(std::floor(centro_.z));
    const int mesa = Voxel::blockId("crafting_table").asInt();
    for (int x = cx - 4; x <= cx + 4; ++x) {
        for (int y = cy - 2; y <= cy + 2; ++y) {
            for (int z = cz - 4; z <= cz + 4; ++z) {
                if (Voxel::getBlock(x, y, z).asInt() == mesa) return true;
            }
        }
    }
    return false;
}

bool JugadorBloques::puedeCraftearSinMesa(const Receta& receta) const {
    for (const auto& [item, n] : receta.pide) {
        if (cuenta(item) < n) return false;
    }
    return true;
}

)CPP"
R"CPP(bool JugadorBloques::puedeCraftear(const Receta& receta) const {
    if (creativo) return true;
    return puedeCraftearSinMesa(receta) && (!receta.mesa || cercaDeMesa());
}

bool JugadorBloques::craftear(int i) {
    if (i < 1 || i > static_cast<int>(kRecetas.size())) return false;
    const Receta& receta = kRecetas[i - 1];
    if (receta.mesa && !creativo && !cercaDeMesa()) {
        mostrarAviso("Necesitas una mesa de trabajo cerca");
        return false;
    }
    if (!puedeCraftear(receta)) {
        mostrarAviso("Te faltan materiales");
        return false;
    }
    if (!creativo) {
        for (const auto& [item, n] : receta.pide) gastar(item, n);
    }
    const int sobra = dar(receta.da, receta.n);
    if (sobra > 0) soltarObjeto(receta.da, sobra, centro_ + Vec3{0, 0.5f, 0});
    mostrarAviso("+" + std::to_string(receta.n) + " " + nombre(receta.da));
    return true;
}

void JugadorBloques::abrirInventario(bool abrir) {
    if (!ui_.inventario) return;
    ui_.inventario.setActive(abrir);
    Input::lockCursor(!abrir);
    if (abrir) {
        mesaCerca_ = cercaDeMesa();
        pintarTodo();
    }
}

void JugadorBloques::pausar(bool pausa) {
    if (!ui_.pausa) return;
    ui_.pausa.setActive(pausa);
    Input::lockCursor(!pausa);
}

void JugadorBloques::reaparecer() {
    muerto_ = false;
    vida_ = 20;
    hambre_ = 20;
    aire_ = 10.0f;
    centro_ = inicio_;
    vel_ = Vec3{};
    caidaDesde_ = centro_.y;
    listo_ = false;
    if (ui_.muerte) ui_.muerte.setActive(false);
    Input::lockCursor(true);
    pintarTodo();
}

// ------------------------------------------------------------------ vida y hambre

void JugadorBloques::cansar(float cantidad) {
    if (creativo) return;
    cansancio_ += cantidad;
    if (cansancio_ >= 4.0f) {
        cansancio_ -= 4.0f;
        hambre_ = std::max(0, hambre_ - 1);
    }
}

void JugadorBloques::herir(int puntos, const std::string& causa) {
    if (creativo || muerto_ || puntos <= 0) return;
    vida_ = std::max(0, vida_ - puntos);
    flash_ = 0.45f;
    if (vida_ <= 0) {
        muerto_ = true;
        if (ui_.muerte) {
            ui_.muerte.setActive(true);
            const Entity texto = ui_.muerte.find("Causa").asEntity();
            if (texto) texto.set("text", causa);
        }
        Input::lockCursor(false);
    }
    pintarEstado();
}

void JugadorBloques::estadisticas(float dt) {
    if (creativo) return;
    // Aire: con la cabeza en el agua se gasta; sin aire, dano.
    if (Voxel::inWater(entity().position()).asBool()) {
        aire_ -= dt;
        if (aire_ <= 0.0f) {
            relojAire_ += dt;
            if (relojAire_ >= 1.0f) {
                relojAire_ = 0.0f;
                herir(2, "Te ahogaste");
            }
        }
    } else {
        aire_ = std::min(10.0f, aire_ + dt * 5.0f);
        relojAire_ = 0.0f;
    }
    // Con hambre llena se cura; sin comida se pierde vida (hasta medio corazon).
    relojVida_ += dt;
    if (relojVida_ >= 4.0f) {
        relojVida_ = 0.0f;
        if (hambre_ >= 18 && vida_ < 20) {
            ++vida_;
            cansar(1.5f);
        } else if (hambre_ <= 0 && vida_ > 1) {
            herir(1, "Te moriste de hambre");
        }
    }
    // El hambre baja despacio aunque no se haga nada.
    relojHambre_ += dt;
    if (relojHambre_ >= 40.0f) {
        relojHambre_ = 0.0f;
        cansar(4.0f);
    }
    pintarEstado();
}

// ------------------------------------------------------------------ objetos sueltos y chispas

Value JugadorBloques::mallaDe(const std::string& item, float tamano) {
    const std::string clave = item + std::to_string(tamano);
    auto it = mallas_.find(clave);
    if (it == mallas_.end()) {
        const Value m = Mesh::cube(tamano);
        m.call("setMaterial", {0, Value{{"color", colorObjeto(item)}, {"roughness", 0.8f}}});
        it = mallas_.emplace(clave, m).first;
    }
    return it->second;
}

void JugadorBloques::soltarObjeto(const std::string& item, int n, const Vec3& pos) {
    Entity e = Scene::create("Objeto");
    e.setPosition(pos);
    e.set("mesh", mallaDe(item, 0.25f));
    objetos_.push_back(Suelto{e, item, n, pos, Vec3{azar(-1.5f, 1.5f), 3.5f, azar(-1.5f, 1.5f)}, 0.0f});
}

// Trocitos del bloque que saltan al romperlo.
void JugadorBloques::chispear(const Vec3& b, const Value& info) {
    const Value malla = mallaDe(info["name"].asString(), 0.09f);
    for (int i = 0; i < 10; ++i) {
        const Vec3 p = b + Vec3{azar(0.2f, 0.8f), azar(0.2f, 0.8f), azar(0.2f, 0.8f)};
        Entity e = Scene::create("Chispa");
        e.setPosition(p);
        e.set("mesh", malla);
        e.set("castShadows", false);
        chispas_.push_back(Chispa{e, p, Vec3{azar(-2, 2), azar(1, 4), azar(-2, 2)}, azar(0.5f, 0.9f)});
    }
}

void JugadorBloques::efectos(float dt) {
    for (int i = static_cast<int>(chispas_.size()) - 1; i >= 0; --i) {
        Chispa& c = chispas_[i];
        c.vida -= dt;
        if (c.vida <= 0.0f || !c.e.valid()) {
            if (c.e.valid()) c.e.destroy();
            chispas_.erase(chispas_.begin() + i);
            continue;
        }
        c.vel.y -= 20.0f * dt;
        const Value r = Voxel::moveBox(c.pos, Vec3{0.045f, 0.045f, 0.045f}, c.vel * dt);
        if (r[1].asBool()) {
            c.vel = c.vel * 0.5f;
            c.vel.y = 0.0f;
        }
        c.pos = r[0].asVec3();
        c.e.setPosition(c.pos);
        const float s = std::min(1.0f, c.vida * 2.0f);
        c.e.setScale(Vec3{s, s, s});
    }
    // Mensajes y golpes en pantalla.
    flash_ = std::max(0.0f, flash_ - dt);
    if (ui_.dano) ui_.dano.set("alpha", flash_ * 0.8f);
    relojAviso_ = std::max(0.0f, relojAviso_ - dt);
    if (ui_.aviso) ui_.aviso.set("alpha", std::min(1.0f, relojAviso_));
    relojNombre_ = std::max(0.0f, relojNombre_ - dt);
    if (ui_.nombre) ui_.nombre.set("alpha", std::min(1.0f, relojNombre_));
}

// Los objetos sueltos caen, flotan girando y se recogen al acercarse.
void JugadorBloques::recoger(float dt) {
    for (int i = static_cast<int>(objetos_.size()) - 1; i >= 0; --i) {
        Suelto& o = objetos_[i];
        o.t += dt;
        const Vec3 hacia = centro_ - o.pos;
        const float lejos = hacia.length();
        if (o.t > 0.5f && lejos < 1.4f) {
            const int sobra = dar(o.item, o.n);
            if (sobra <= 0) {
                o.e.destroy();
                objetos_.erase(objetos_.begin() + i);
                continue;
            }
            o.n = sobra;
        }
        if (o.t > 0.5f && lejos < 3.0f) {
            o.vel = Vec3::lerpClamped(o.vel, hacia.normalized() * 6.0f, 8.0f * dt);  // iman
        } else {
            o.vel.y -= 20.0f * dt;
            o.vel.x *= 0.96f;
            o.vel.z *= 0.96f;
        }
        const Value r = Voxel::moveBox(o.pos, Vec3{0.125f, 0.125f, 0.125f}, o.vel * dt);
        if (r[1].asBool()) o.vel = Vec3{};
        o.pos = r[0].asVec3();
        o.e.setPosition(o.pos + Vec3{0, 0.1f + std::sin(o.t * 3.0f) * 0.06f, 0});
        o.e.setRotation(Vec3{0, o.t * 90.0f, 0});
        if (o.t > 300.0f) {
            o.e.destroy();
            objetos_.erase(objetos_.begin() + i);
        }
    }
}

// ------------------------------------------------------------------ interfaz

void JugadorBloques::mostrarAviso(const std::string& texto) {
    if (ui_.aviso) ui_.aviso.set("text", texto);
    relojAviso_ = 2.5f;
}

void JugadorBloques::pintarHueco(const Entity& hueco, const Hueco& dato) const {
    if (!hueco) return;
    const Entity ic = hueco.find("Icono").asEntity();
    const Entity num = hueco.find("Cantidad").asEntity();
    if (!dato.item.empty()) {
        if (ic) {
            ic.set("texture", icono(dato.item));
            ic.set("alpha", 1);
        }
        if (num) num.set("text", dato.n > 1 && !creativo ? std::to_string(dato.n) : std::string());
    } else {
        if (ic) ic.set("alpha", 0);
        if (num) num.set("text", "");
    }
}

void JugadorBloques::pintarBarra() {
    for (int i = 1; i <= 9; ++i) pintarHueco(ui_.barra[i], inv_[i]);
    if (ui_.seleccion) ui_.seleccion.set("uiPosition", Vec3{(elegido_ - 5) * 84.0f, -20.0f, 0});
    const Hueco& dato = inv_[elegido_];
    if (ui_.nombre) ui_.nombre.set("text", dato.item.empty() ? std::string() : nombre(dato.item));
}

void JugadorBloques::pintarEstado() {
    for (int i = 1; i <= 10; ++i) {
        const Entity& c = ui_.corazones[i - 1];
        const Entity& f = ui_.comida[i - 1];
        const Entity& b = ui_.burbujas[i - 1];
        const int v = vida_ - (i - 1) * 2;
        const int h = hambre_ - (i - 1) * 2;
        if (c) {
            c.setActive(!creativo);
            c.set("texture", kIconos + (v >= 2 ? "corazon.png" : (v == 1 ? "corazon_medio.png" : "corazon_vacio.png")));
        }
        if (f) {
            f.setActive(!creativo);
            f.set("texture", kIconos + (h >= 2 ? "comida.png" : (h == 1 ? "comida_media.png" : "comida_vacia.png")));
        }
        if (b) b.setActive(!creativo && aire_ < 10.0f && i <= static_cast<int>(std::ceil(aire_)));
    }
}

void JugadorBloques::pintarInventario() {
    if (!abierto(ui_.inventario)) return;
    for (int i = 1; i <= kHuecos; ++i) pintarHueco(ui_.huecos[i], inv_[i]);
    for (std::size_t i = 0; i < kRecetas.size() && i < ui_.recetas.size(); ++i) {
        const Receta& receta = kRecetas[i];
        const Entity& boton = ui_.recetas[i];
        if (!boton) continue;
        std::string partes;
        for (const auto& [item, n] : receta.pide) partes += (partes.empty() ? "" : " + ") + std::to_string(n) + " " + nombre(item);
        std::string texto = std::to_string(receta.n) + " " + nombre(receta.da) + "   ←   " + partes;
        if (receta.mesa) texto += "   (mesa)";
        if (const Entity t = boton.find("Texto").asEntity()) t.set("text", texto);
        if (const Entity ic = boton.find("Icono").asEntity()) ic.set("texture", icono(receta.da));
        boton.set("interactable", creativo || (puedeCraftearSinMesa(receta) && (!receta.mesa || mesaCerca_)));
    }
}

void JugadorBloques::pintarTodo() {
    pintarBarra();
    pintarEstado();
    pintarInventario();
}

void JugadorBloques::pintarInfo(const std::string& aviso) {
    if (!ui_.info) return;
    const Vec3 c = centro_;
    std::string texto = Voxel::worldName().asString() + "  ·  " + (creativo ? "creativo" : "supervivencia") + "  ·  " +
                        std::to_string(static_cast<int>(std::floor(c.x))) + ", " +
                        std::to_string(static_cast<int>(std::floor(c.y - kMitad.y))) + ", " +
                        std::to_string(static_cast<int>(std::floor(c.z)));
    if (volando_) texto += "  ·  volando (F)";
    if (!aviso.empty()) texto += "\n" + aviso;
    if (!apuntado_.isNil()) {
        texto += "\n" + Voxel::blockInfo(apuntado_["id"])["label"].asString();
        if (progreso_ > 0.0f) texto += "  " + std::to_string(static_cast<int>(std::floor(progreso_ * 100.0f))) + "%";
    }
    if (!Input::isCursorLocked().asBool() && !panelAbierto()) texto += "\nClic para jugar  ·  E inventario  ·  Escape pausa";
    ui_.info.set("text", texto);
}

// ------------------------------------------------------------------ guardar

void JugadorBloques::cargarInventario() {
    inv_.fill(Hueco{});
    if (creativo) {
        for (std::size_t i = 0; i < kCreativo.size(); ++i) inv_[i + 1] = Hueco{kCreativo[i], kPila};
        // La mochila con todos los bloques que se pueden poner.
        int i = 10;
        const int total = Voxel::blockCount().asInt();
        for (int id = 1; id < total && i <= kHuecos; ++id) {
            const Value info = Voxel::blockInfo(id);
            if (info["placeable"].asBool()) inv_[i++] = Hueco{info["name"].asString(), kPila};
        }
        return;
    }
    // "hueco=item:n,hueco=item:n..."
    const std::string texto = Voxel::getMeta("inventario", "").asString();
    std::size_t at = 0;
    while (at < texto.size()) {
        std::size_t fin = texto.find(',', at);
)CPP"
R"CPP(        if (fin == std::string::npos) fin = texto.size();
        const std::string parte = texto.substr(at, fin - at);
        const std::size_t igual = parte.find('=');
        const std::size_t dos = parte.find(':');
        if (igual != std::string::npos && dos != std::string::npos && dos > igual) {
            const int hueco = std::atoi(parte.c_str());
            if (hueco >= 1 && hueco <= kHuecos) inv_[hueco] = Hueco{parte.substr(igual + 1, dos - igual - 1), std::atoi(parte.c_str() + dos + 1)};
        }
        at = fin + 1;
    }
}

void JugadorBloques::guardar() {
    char posicion[128];
    std::snprintf(posicion, sizeof(posicion), "%.2f %.2f %.2f %.1f %.1f", centro_.x, centro_.y, centro_.z, yaw_, pitch_);
    Voxel::setMeta("player", posicion);
    if (!creativo) {
        std::string partes;
        for (int i = 1; i <= kHuecos; ++i) {
            const Hueco& h = inv_[i];
            if (h.item.empty()) continue;
            if (!partes.empty()) partes += ",";
            partes += std::to_string(i) + "=" + h.item + ":" + std::to_string(h.n);
        }
        Voxel::setMeta("inventario", partes);
        Voxel::setMeta("vida", std::to_string(vida_));
        Voxel::setMeta("hambre", std::to_string(hambre_));
    }
    Voxel::saveWorld();
}

CRAMION_SCRIPT(JugadorBloques)
)CPP"},
};

}  // namespace cramion::editor::template_scripts

#endif  // CRAMION_EDITOR_TEMPLATE_SCRIPTS_H

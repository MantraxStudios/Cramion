#ifndef CRAMION_EDITOR_TEMPLATE_CREATURE_SCRIPTS_H
#define CRAMION_EDITOR_TEMPLATE_CREATURE_SCRIPTS_H

// Scripts de C++ de la plantilla "Criaturas" (ProjectTemplates.cpp): el perro,
// el maniqui, la pelota que siguen con la cabeza y los controles.

#include "TemplateScripts.h"

namespace cramion::editor::creature_scripts {

inline constexpr TemplateFile kFiles[] = {
    {"Controles.cpp",
R"CPP(// Controles.cpp: controles de la demo y el texto de ayuda.
//   B: ver los huesos del perro      I: IK del perro (pies y mirada) on/off
//   P: phys bones (cola, orejas, coleta) on/off
#include <cramion/Script.h>

#include <string>

using namespace cramion;

class Controles : public Script {
public:
    void start() override {
        perro_ = Scene::find("Perro");
        maniqui_ = Scene::find("Maniqui");
        pintar();
    }

    void update(float) override {
        if (Input::keyDown("b")) {
            huesos_ = !huesos_;
            perro_.showBones(huesos_);
            pintar();
        }
        if (Input::keyDown("i")) {
            ik_ = !ik_;
            perro_.setField("InverseKinematics", "enabled", ik_);
            maniqui_.setField("InverseKinematics", "enabled", ik_);
            pintar();
        }
        if (Input::keyDown("p")) {
            fisica_ = !fisica_;
            perro_.setField("PhysBones", "enabled", fisica_);
            maniqui_.setField("PhysBones", "enabled", fisica_);
            pintar();
        }
    }

private:
    void pintar() {
        const auto si = [](bool v) { return std::string(v ? "si" : "no"); };
        entity().set("text", "R: ragdoll del perro (Espacio: empujar)   F: tirar al maniqui   B: huesos (" + si(huesos_) +
                                 ")   I: IK (" + si(ik_) + ")   P: phys bones (" + si(fisica_) + ")   Clic derecho y rueda: camara");
    }

    Entity perro_, maniqui_;
    bool huesos_ = false;
    bool ik_ = true;
    bool fisica_ = true;
};

CRAMION_SCRIPT(Controles)
)CPP"},
    {"Maniqui.cpp",
R"CPP(// Maniqui.cpp: maniqui humanoide. Sus pies se apoyan en el escalon (Pies en
// el suelo), la coleta es un Phys Bone y la cabeza sigue a la pelota.
//   F: cae (ragdoll) empujado desde la camara; se levanta solo a los 4 s (o
//      con G), mezclando de vuelta a la animacion.
#include <cramion/Script.h>

using namespace cramion;

class Maniqui : public Script {
public:
    Property<float> levantarse{this, "levantarse", 4.0f, Tooltip("Segundos en el suelo")};

    void start() override {
        camara_ = Scene::find("Main Camera");
        entity().setLookAt(Scene::find("Pelota"), 0.9f);
        entity().playAnimation("Reposo", true);
    }

    void update(float dt) override {
        if (Input::keyDown("f") && !entity().ragdoll().asBool()) {
            entity().setRagdoll(true);
            Vec3 desde = entity().position() - camara_.position();
            desde = Vec3{desde.x, 0, desde.z}.normalized();
            entity().addRagdollForce(desde * 140.0f + Vec3{0, 25, 0}, "Spine1");
        }
        if (entity().ragdoll().asBool()) {
            // Empezo a caer (con F o desde otro script): cuenta hasta levantarse.
            if (!cayendo_) {
                cayendo_ = true;
                tiempo_ = levantarse;
            }
            tiempo_ -= dt;
            if (tiempo_ <= 0.0f || Input::keyDown("g")) entity().setRagdoll(false);
        } else {
            cayendo_ = false;
        }
    }

private:
    Entity camara_;
    bool cayendo_ = false;
    float tiempo_ = 0.0f;
};

CRAMION_SCRIPT(Maniqui)
)CPP"},
    {"Pelota.cpp",
R"CPP(// Pelota.cpp: pelota que da vueltas; el perro y el maniqui la siguen con la
// cabeza (IK).
#include <cramion/Script.h>

#include <cmath>

using namespace cramion;

class Pelota : public Script {
public:
    Property<float> radio{this, "radio", 4.0f};
    Property<float> altura{this, "altura", 1.3f};
    Property<float> velocidad{this, "velocidad", 0.5f};

    void start() override { centro_ = entity().position(); }

    void update(float dt) override {
        t_ += dt * velocidad;
        const Vec3 p{std::cos(t_) * radio, altura + std::sin(t_ * 2.3f) * 0.6f, std::sin(t_) * radio};
        entity().setPosition(centro_ + p);
    }

private:
    Vec3 centro_;
    float t_ = 0.0f;
};

CRAMION_SCRIPT(Pelota)
)CPP"},
    {"Perro.cpp",
R"CPP(// Perro.cpp: recorre un circuito con escaleras, una rampa y piedras. Sus patas
// se apoyan en el suelo con el IK (cadenas de 3 huesos con "Al suelo") y el
// cuerpo se inclina en la rampa. La cola y las orejas son Phys Bones y la
// cabeza (con el cuello) sigue a la pelota.
//   R: ragdoll (se cae con un empujon); otra vez R: se levanta.
//   Espacio con el ragdoll: otro empujon.
#include <cramion/Script.h>

#include <array>

using namespace cramion;

class Perro : public Script {
public:
    Property<float> velocidad{this, "velocidad", 1.4f, Tooltip("m/s")};
    Property<float> giro{this, "giro", 3.0f, Tooltip("Rapidez al girar")};

    void start() override {
        i_ = 0;
        entity().setLookAt(Scene::find("Pelota"), 0.7f);
        entity().playAnimation("Caminar", true);
    }

    void update(float dt) override {
        if (Input::keyDown("r")) {
            const bool ragdoll = !entity().ragdoll().asBool();
            entity().setRagdoll(ragdoll);
            if (ragdoll) {
                // Se cae de lado con la velocidad que llevaba y un empujon.
                const Vec3 f = entity().forward();
                const Vec3 lado{f.z, 0, -f.x};
                entity().addRagdollForce(lado * 70.0f + Vec3{0, 25, 0}, "Chest");
            }
        }
        if (entity().ragdoll().asBool()) {
            if (Input::keyDown("space")) {
                entity().addRagdollForce(Vec3{Random::range(-30, 30).asFloat(), 55, Random::range(-30, 30).asFloat()});
            }
            return;
        }

        // Andar hacia el siguiente punto, girando suave.
        Vec3 pos = entity().position();
        const Vec3 meta = kPuntos[i_];
        const Vec3 hacia{meta.x - pos.x, 0, meta.z - pos.z};
        if (hacia.length() < 0.6f) {
            i_ = (i_ + 1) % kPuntos.size();
            return;
        }
        Vec3 delante = entity().forward();
        delante = Vec3{delante.x, 0, delante.z}.normalized();
        const Vec3 nuevo = Vec3::lerpClamped(delante, hacia.normalized(), giro * dt).normalized();
        pos = pos + nuevo * (velocidad * dt);
        pos.y = suelo(pos);
        entity().setPosition(pos);
        entity().lookAt(pos + nuevo);
    }

private:
    // Altura del suelo bajo un punto (escalones, rampa, piedras).
    static float suelo(const Vec3& p) {
        RaycastHit hit;
        if (Physics::raycast(p + Vec3{0, 2, 0}, Vec3{0, -1, 0}, 6.0f, &hit)) return hit.point.y;
        return p.y;
    }

    static constexpr std::array<Vec3, 8> kPuntos = {Vec3{-7, 0, 7}, Vec3{-1, 0, 7}, Vec3{6, 0, 7},  Vec3{7, 0, 1},
                                                    Vec3{7, 0, -6}, Vec3{0, 0, -7}, Vec3{-7, 0, -6}, Vec3{-7, 0, 1}};
    std::size_t i_ = 0;
};

CRAMION_SCRIPT(Perro)
)CPP"},
};

}  // namespace cramion::editor::creature_scripts

#endif  // CRAMION_EDITOR_TEMPLATE_CREATURE_SCRIPTS_H

# Los ejemplos del manual en C++ (cada uno .h + .cpp, como la plantilla del editor).
# gen: python docs-src/examples_cpp.py [carpeta]  -> escribe las paginas
# docs-src/pages/ejemplo-*.html y, con carpeta, los archivos para compilarlos
# (las pruebas los compilan con el SDK: un ejemplo que no compila no se publica).
import html
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

EXAMPLES = {
    "ejemplo-girar": ("Aspas de molino, monedas, plataformas... El eje y la velocidad se ajustan en el Inspector.", [
        ("Girar.h", """#pragma once
#include <cramion/Script.h>
using namespace cramion;

class Girar : public Script {
public:
    Property<Vec3> eje{this, "eje", Vec3{0, 1, 0}, Tooltip("Eje local")};
    Property<float> velocidad{this, "velocidad", 90.0f, Range(0, 720), Tooltip("Grados por segundo")};

    void update(float dt) override;
};
"""),
        ("Girar.cpp", """#include "Girar.h"

void Girar::update(float dt) {
    entity().rotate(eje.get() * (velocidad * dt));
}

CRAMION_SCRIPT(Girar)
""")]),
    "ejemplo-camara": ("Va en lateUpdate para moverse después de que el jugador ya se movió en su update. "
                       "El objetivo se arrastra al Inspector.", [
        ("CamaraSeguir.h", """#pragma once
#include <cramion/Script.h>
using namespace cramion;

class CamaraSeguir : public Script {
public:
    Property<Entity> objetivo{this, "objetivo"};                   // arrastra el jugador
    Property<Vec3> distancia{this, "distancia", Vec3{0, 4, 8}};     // detras y por encima
    Property<float> suavizado{this, "suavizado", 6.0f, Range(0, 20)};

    void start() override;
    void lateUpdate(float dt) override;
};
"""),
        ("CamaraSeguir.cpp", """#include "CamaraSeguir.h"

void CamaraSeguir::start() {
    if (!objetivo.get()) Debug::warning("CamaraSeguir: arrastra un objetivo en el Inspector");
}

void CamaraSeguir::lateUpdate(float dt) {
    const Entity jugador = objetivo.get();
    if (!jugador.valid()) return;
    const Vec3 destino = jugador.position() + distancia.get();
    const float t = Mathf::clamp01(suavizado * dt);
    entity().setPosition(Vec3::lerp(entity().position(), destino, t));
    entity().lookAt(jugador.position() + Vec3::up());
}

CRAMION_SCRIPT(CamaraSeguir)
""")]),
    "ejemplo-jugador": ("Movimiento con WASD y salto con Espacio sobre un Rigidbody con CapsuleCollider. "
                        "Detecta el suelo con un raycast y fija la velocidad horizontal para que el control sea preciso. "
                        "(Con un Character Controller es aún más simple: <code>entity().setMoveInput(dir)</code> y <code>entity().jump()</code>.)", [
        ("Jugador.h", """#pragma once
#include <cramion/Script.h>
using namespace cramion;

class Jugador : public Script {
public:
    Property<float> velocidad{this, "velocidad", 6.0f};
    Property<float> correr{this, "correr", 1.6f, Tooltip("Multiplicador con Shift")};
    Property<float> salto{this, "salto", 5.5f, Tooltip("m/s hacia arriba")};
    Property<float> altura{this, "altura", 1.0f, Tooltip("Del centro a los pies")};
    Property<AudioClip> sonidoSalto{this, "sonidoSalto"};

    void update(float dt) override;

private:
    bool enSuelo() const;
};
"""),
        ("Jugador.cpp", """#include "Jugador.h"

bool Jugador::enSuelo() const {
    // El rayo empieza justo fuera de la capsula para no detectarla.
    const Vec3 pies = entity().position() + Vec3::down() * (altura + 0.02f);
    RaycastHit hit;
    return Physics::raycast(pies, Vec3::down(), 0.15f, &hit, entity());
}

void Jugador::update(float) {
    // Direccion en el plano (W = hacia -Z, el "delante" del motor).
    Vec3 entrada{Input::axis("Horizontal"), 0.0f, -Input::axis("Vertical")};
    if (entrada.length() > 1.0f) entrada = entrada.normalized();

    float rapidez = velocidad;
    if (Input::key("LeftShift")) rapidez *= correr;

    Vec3 v = entity().velocity();
    v.x = entrada.x * rapidez;
    v.z = entrada.z * rapidez;
    if (Input::keyDown("Space") && enSuelo()) {
        v.y = salto;
        Audio::playOneShot(sonidoSalto);
    }
    entity().setVelocity(v);

    // Mirar hacia donde camina.
    if (entrada.length() > 0.1f) entity().lookAt(entity().position() + entrada);
}

CRAMION_SCRIPT(Jugador)
""")]),
    "ejemplo-disparo": ("Al hacer clic, lanza un rayo hacia delante y empuja lo que toque; si es un Enemigo (el del "
                        "<a href=\"ejemplo-spawner.html\">generador</a>), le quita vida llamando a su script.", [
        ("Disparo.h", """#pragma once
#include <cramion/Script.h>
using namespace cramion;

class Disparo : public Script {
public:
    Property<float> alcance{this, "alcance", 100.0f};
    Property<float> fuerza{this, "fuerza", 12.0f};
    Property<float> cadencia{this, "cadencia", 0.15f, Tooltip("Segundos entre disparos")};
    Property<AudioClip> sonido{this, "sonido"};

    void update(float dt) override;

private:
    float espera = 0.0f;
};
"""),
        ("Disparo.cpp", """#include "Disparo.h"
#include "Enemigo.h"

#include <cstdio>

void Disparo::update(float dt) {
    espera -= dt;
    if (!Input::mouseButton(0) || espera > 0.0f) return;
    espera = cadencia;

    const Vec3 origen = entity().position();
    const Vec3 dir = entity().forward();
    Audio::playOneShot(sonido, origen, 0.7f);

    RaycastHit hit;
    if (!Physics::raycast(origen, dir, alcance, &hit, entity())) return;
    char texto[128];
    std::snprintf(texto, sizeof(texto), "Impacto en %s a %.1f m", hit.entity.name().c_str(), hit.distance);
    Debug::log(texto);
    if (hit.entity.has("Rigidbody")) hit.entity.addForce(dir * fuerza.get(), ForceMode::Impulse);
    if (Enemigo* enemigo = hit.entity.script<Enemigo>()) enemigo->dano(25);
}

CRAMION_SCRIPT(Disparo)
""")]),
    "ejemplo-monedas": ("Cada moneda tiene un SphereCollider marcado como trigger y la etiqueta Moneda. "
                        "El marcador es una entidad con un UIText llamada Marcador; la moneda llama a su script con <code>script&lt;Marcador&gt;()</code>.", [
        ("Moneda.h", """#pragma once
#include <cramion/Script.h>
using namespace cramion;

class Moneda : public Script {
public:
    Property<int> valor{this, "valor", 1};
    Property<AudioClip> sonido{this, "sonido"};

    void start() override;
    void update(float dt) override;
    void onTriggerEnter(Entity other) override;

private:
    Vec3 base;
};
"""),
        ("Moneda.cpp", """#include "Moneda.h"
#include "Marcador.h"

void Moneda::start() { base = entity().position(); }

void Moneda::update(float dt) {
    // Flota y gira.
    entity().rotate(Vec3{0.0f, 180.0f * dt, 0.0f});
    entity().setPosition(base + Vec3{0.0f, Mathf::sin(Time::time() * 3.0f) * 0.15f, 0.0f});
}

void Moneda::onTriggerEnter(Entity other) {
    if (other.name() != "Jugador") return;
    if (Marcador* marcador = Scene::find("Marcador").script<Marcador>()) marcador->sumar(valor);
    Audio::playOneShot(sonido, entity().position());
    entity().destroy();
}

CRAMION_SCRIPT(Moneda)
"""),
        ("Marcador.h", """#pragma once
#include <cramion/Script.h>
using namespace cramion;

class Marcador : public Script {
public:
    void start() override;
    void sumar(int n);

private:
    void pintar();
    int puntos = 0;
    int total = 0;
};
"""),
        ("Marcador.cpp", """#include "Marcador.h"

#include <algorithm>

void Marcador::start() {
    total = static_cast<int>(Scene::findAllWithTag("Moneda").size());
    pintar();
}

void Marcador::pintar() {
    entity().setText("Monedas: " + std::to_string(puntos) + " / " + std::to_string(total));
}

void Marcador::sumar(int n) {
    puntos += n;
    pintar();
    if (puntos < total) return;
    entity().setColor(Vec3{1.0f, 0.85f, 0.2f});
    Prefs::setInt("record", std::max(Prefs::getInt("record", 0).asInt(), puntos));
    Scene::load("Victoria");
}

CRAMION_SCRIPT(Marcador)
""")]),
    "ejemplo-spawner": ("Copia un objeto plantilla (desactivado en la escena, arrastrado al Inspector) cada cierto "
                        "tiempo en un punto aleatorio. Los enemigos persiguen al jugador y tienen vida.", [
        ("Generador.h", """#pragma once
#include <cramion/Script.h>
using namespace cramion;

class Generador : public Script {
public:
    Property<Entity> plantilla{this, "plantilla", {}, Tooltip("Un enemigo desactivado de la escena")};
    Property<float> intervalo{this, "intervalo", 2.0f};
    Property<float> radio{this, "radio", 10.0f};
    Property<int> maximo{this, "maximo", 12};

    void update(float dt) override;

private:
    float reloj = 0.0f;
};
"""),
        ("Generador.cpp", """#include "Generador.h"

void Generador::update(float dt) {
    if (!plantilla.get()) return;
    reloj += dt;
    if (reloj < intervalo) return;
    reloj = 0.0f;
    if (static_cast<int>(Scene::findAllWithTag("Enemigo").size()) >= maximo) return;

    const float angulo = Random::range(0.0f, Mathf::tau);
    const Vec3 punto = entity().position() + Vec3{Mathf::cos(angulo), 0.0f, Mathf::sin(angulo)} * radio.get();
    const Entity enemigo = Scene::instantiate(plantilla.get(), punto);
    enemigo.setName("Enemigo");
    enemigo.set("tag", "Enemigo");
    enemigo.setActive(true);
}

CRAMION_SCRIPT(Generador)
"""),
        ("Enemigo.h", """#pragma once
#include <cramion/Script.h>
using namespace cramion;

class Enemigo : public Script {
public:
    Property<float> velocidad{this, "velocidad", 2.5f};
    Property<int> vidaInicial{this, "vida", 50};
    Property<AudioClip> explosion{this, "explosion"};

    void start() override;
    void update(float dt) override;
    void dano(int cantidad);  // la llama Disparo

private:
    Entity jugador;
    int vida = 0;
};
"""),
        ("Enemigo.cpp", """#include "Enemigo.h"

void Enemigo::start() {
    jugador = Scene::find("Jugador");
    vida = vidaInicial;
}

void Enemigo::update(float dt) {
    if (!jugador.valid()) return;
    Vec3 hacia = jugador.position() - entity().position();
    hacia.y = 0.0f;
    if (hacia.length() > 1.2f) {
        entity().translate(hacia.normalized() * (velocidad * dt));
        entity().lookAt(jugador.position());
    }
}

void Enemigo::dano(int cantidad) {
    vida -= cantidad;
    if (vida > 0) return;
    Audio::playOneShot(explosion, entity().position());
    entity().destroy();
}

CRAMION_SCRIPT(Enemigo)
""")]),
    "ejemplo-vehiculo": ("En una entidad con Rigidbody, Vehicle y ruedas con WheelCollider. "
                         "Muestra la velocidad en un UIText (arrástralo al Inspector).", [
        ("Coche.h", """#pragma once
#include <cramion/Script.h>
using namespace cramion;

class Coche : public Script {
public:
    Property<float> respuesta{this, "respuesta", 3.0f, Tooltip("Lo rapido que gira el volante")};
    Property<Entity> velocimetro{this, "velocimetro", {}, Requires("UIText")};

    void update(float dt) override;

private:
    float giro = 0.0f;
};
"""),
        ("Coche.cpp", """#include "Coche.h"

#include <cstdio>

void Coche::update(float dt) {
    const float acelerador = Input::axis("Vertical");
    // Volante progresivo: no gira de golpe.
    giro = Mathf::moveTowards(giro, Input::axis("Horizontal"), respuesta * dt);
    const float velocidad = entity().speed();
    const float freno = Input::key("S") && velocidad > 5.0f ? 1.0f : 0.0f;
    const float mano = Input::key("Space") ? 1.0f : 0.0f;
    entity().setVehicleInput(acelerador, giro, freno, mano);

    if (velocimetro.get()) {
        char texto[96];
        std::snprintf(texto, sizeof(texto), "%3d km/h   %4d rpm   marcha %d", static_cast<int>(velocidad),
                      static_cast<int>(entity().rpm().asFloat()), entity().gear().asInt());
        velocimetro->setText(texto);
    }
    if (Input::keyDown("R")) {  // enderezar
        entity().setRotation(Vec3{0.0f, entity().rotation().y, 0.0f});
        entity().translate(Vec3::up());
    }
}

CRAMION_SCRIPT(Coche)
""")]),
    "ejemplo-menu": ("Los botones y el slider de la UI envían mensajes por su nombre (<em>Al hacer clic</em>: "
                     "<code>OnJugar</code>) al script: se reciben con <code>on(\"OnJugar\", ...)</code>. "
                     "El volumen y el récord se guardan con Prefs.", [
        ("Menu.h", """#pragma once
#include <cramion/Script.h>
using namespace cramion;

class Menu : public Script {
public:
    Property<SceneAsset> primerNivel{this, "primerNivel"};
    Property<AudioClip> clic{this, "clic"};

    void awake() override;
    void start() override;
    void update(float dt) override;

private:
    void jugar();
};
"""),
        ("Menu.cpp", """#include "Menu.h"

void Menu::awake() {
    // Los mensajes de la UI: el nombre es el que se puso en el boton o el slider.
    on("OnJugar", [this](const Value&) { jugar(); });
    on("OnVolumen", [](const Value& valor) { Prefs::setFloat("volumen", valor); });
    on("OnSalir", [](const Value&) { Game::quit(); });
}

void Menu::start() {
    const int record = Prefs::getInt("record", 0);
    if (Entity texto = Scene::find("Menu_Record"); texto.valid()) {
        texto.setText(record > 0 ? "Record: " + std::to_string(record) : std::string("Juega tu primera partida!"));
    }
    if (Entity slider = Scene::find("Menu_Volumen"); slider.valid()) slider.setValue(Prefs::getFloat("volumen", 1.0f));
}

void Menu::jugar() {
    Audio::playOneShot(clic);
    Scene::load(primerNivel);
}

void Menu::update(float) {
    if (Input::keyDown("Enter")) jugar();
}

CRAMION_SCRIPT(Menu)
""")]),
    "ejemplo-animacion": ("Alimenta los parámetros del controlador de animación a partir de la velocidad del Rigidbody.", [
        ("AnimarPersonaje.h", """#pragma once
#include <cramion/Script.h>
using namespace cramion;

class AnimarPersonaje : public Script {
public:
    void update(float dt) override;
};
"""),
        ("AnimarPersonaje.cpp", """#include "AnimarPersonaje.h"

void AnimarPersonaje::update(float) {
    const Vec3 v = entity().velocity();
    const float plano = Vec3{v.x, 0.0f, v.z}.length();
    entity().setAnimatorFloat("Velocidad", plano);
    entity().setAnimatorBool("EnAire", Mathf::abs(v.y) > 0.5f);
    if (Input::keyDown("E")) entity().setAnimatorTrigger("Saludar");
}

CRAMION_SCRIPT(AnimarPersonaje)
""")]),
    "ejemplo-comunicacion": ("<code>entity.script&lt;T&gt;()</code> devuelve el script de C++ de otro objeto (como "
                             "<code>GetComponent&lt;T&gt;()</code> de Unity): lees sus datos y llamas a sus métodos. "
                             "Aquí una palanca abre una puerta.", [
        ("Puerta.h", """#pragma once
#include <cramion/Script.h>
using namespace cramion;

class Puerta : public Script {
public:
    Property<float> altura{this, "altura", 3.0f};
    Property<float> velocidad{this, "velocidad", 2.0f};
    Property<AudioClip> sonido{this, "sonido"};

    void start() override;
    void update(float dt) override;
    void abrir();  // la llama la Palanca

private:
    Vec3 cerrada, abierta;
    bool abriendo = false;
};
"""),
        ("Puerta.cpp", """#include "Puerta.h"

void Puerta::start() {
    cerrada = entity().position();
    abierta = cerrada + Vec3{0.0f, altura, 0.0f};
}

void Puerta::abrir() {
    if (!abriendo) Audio::playOneShot(sonido, cerrada);
    abriendo = true;
}

void Puerta::update(float dt) {
    const Vec3 destino = abriendo ? abierta : cerrada;
    entity().setPosition(Vec3::moveTowards(entity().position(), destino, velocidad * dt));
}

CRAMION_SCRIPT(Puerta)
"""),
        ("Palanca.h", """#pragma once
#include <cramion/Script.h>
using namespace cramion;

class Palanca : public Script {
public:
    Property<Entity> puerta{this, "puerta", {}, Tooltip("El objeto con el script Puerta")};

    void onTriggerStay(Entity other) override;
};
"""),
        ("Palanca.cpp", """#include "Palanca.h"
#include "Puerta.h"

void Palanca::onTriggerStay(Entity other) {
    if (other.name() != "Jugador" || !Input::keyDown("E")) return;
    if (Puerta* p = puerta->script<Puerta>()) p->abrir();
}

CRAMION_SCRIPT(Palanca)
""")]),
    "ejemplo-ia": ("Un guardia con NavAgent que patrulla por puntos al azar y persigue al jugador cuando lo ve "
                   "(el raycast de la malla de navegación comprueba que no haya una pared en medio). Si lo pierde, vuelve a patrullar.", [
        ("Guardia.h", """#pragma once
#include <cramion/Script.h>
using namespace cramion;

class Guardia : public Script {
public:
    Property<float> radioPatrulla{this, "radioPatrulla", 12.0f};
    Property<float> vision{this, "vision", 10.0f};

    void start() override;
    void update(float dt) override;

private:
    bool veAlJugador() const;
    enum class Estado { Patrulla, Persigue } estado = Estado::Patrulla;
    Vec3 casa;
    Entity jugador;
    float espera = 0.0f;
};
"""),
        ("Guardia.cpp", """#include "Guardia.h"

void Guardia::start() {
    casa = entity().position();
    jugador = Scene::find("Jugador");
}

bool Guardia::veAlJugador() const {
    if (!jugador.valid()) return false;
    if (Vec3::distance(entity().position(), jugador.position()) > vision) return false;
    return Navigation::raycast(entity().position(), jugador.position());  // sin paredes en medio
}

void Guardia::update(float dt) {
    if (veAlJugador()) {
        estado = Estado::Persigue;
        entity().moveTo(jugador.position());
        return;
    }
    const bool moviendose = entity().isMoving();
    if (estado == Estado::Persigue) {
        // Lo perdio: va al ultimo sitio donde lo vio y luego patrulla.
        if (!moviendose) estado = Estado::Patrulla;
        return;
    }
    // Patrulla: un punto al azar alrededor de casa, espera y otro.
    if (moviendose) return;
    espera -= dt;
    if (espera > 0.0f) return;
    if (const Value punto = Navigation::randomPoint(casa, radioPatrulla.get()); !punto.isNil()) entity().moveTo(punto);
    espera = Random::range(1.0f, 3.0f);
}

CRAMION_SCRIPT(Guardia)
""")]),
    "ejemplo-terreno": ("Una malla creada por código con su material y un MeshCollider para que los Rigidbody choquen con ella.", [
        ("Terreno.h", """#pragma once
#include <cramion/Script.h>
using namespace cramion;

class Terreno : public Script {
public:
    Property<float> tamano{this, "tamano", 60.0f};
    Property<float> altura{this, "altura", 4.0f};

    void start() override;
};
"""),
        ("Terreno.cpp", """#include "Terreno.h"

void Terreno::start() {
    Mesh malla = Mesh::plane(tamano.get(), tamano.get(), 80, 80);
    const Value vertices = malla.vertices();
    Values nuevos;
    nuevos.reserve(vertices.size());
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        Vec3 p = vertices[i];
        p.y = (Mathf::sin(p.x * 0.15f) + Mathf::cos(p.z * 0.12f)) * altura * 0.5f;
        nuevos.push_back(p);
    }
    malla.setVertices(nuevos);  // sube la malla
    malla.recalculateNormals();
    malla.recalculateTangents();
    malla.setMaterial(0, Value{{"color", Vec3{0.35f, 0.6f, 0.25f}}, {"roughness", 0.9f}});
    entity().setMesh(malla);
    entity().add("MeshCollider");  // los Rigidbody chocan con ella
}

CRAMION_SCRIPT(Terreno)
""")]),
    "ejemplo-efectos": ("Una malla con dos submallas y dos materiales: piedra con textura y un cristal que brilla y late.", [
        ("Cristal.h", """#pragma once
#include <cramion/Script.h>
using namespace cramion;

class Cristal : public Script {
public:
    void start() override;
    void update(float dt) override;

private:
    Mesh malla;
};
"""),
        ("Cristal.cpp", """#include "Cristal.h"

void Cristal::start() {
    malla = Mesh::cube(1.0f);
    // Dos submallas: las caras de arriba y abajo brillan, el resto es piedra.
    const Value tris = malla.triangles();
    Values lados, tapas;
    for (std::size_t i = 0; i + 2 < tris.size(); i += 3) {
        Values& destino = (i >= 12 && i < 24) ? tapas : lados;  // caras 3 y 4 del cubo: +Y y -Y
        destino.push_back(tris[i]);
        destino.push_back(tris[i + 1]);
        destino.push_back(tris[i + 2]);
    }
    malla.setTriangles(lados, 0);
    malla.setTriangles(tapas, 1);
    malla.setMaterial(0, Value{{"texture", "Textures/piedra.png"}, {"normalMap", "Textures/piedra_n.png"}, {"tiling", Vec3{2, 2, 0}}});
    malla.setMaterial(1, Value{{"color", Vec3{0.2f, 0.6f, 1.0f}}, {"emission", Vec3{0.2f, 0.6f, 1.0f}}, {"emissionIntensity", 4.0f}});
    entity().setMesh(malla);
}

void Cristal::update(float) {
    // El brillo late: solo cambia el material (no se vuelve a subir la malla).
    malla.setMaterial(1, Value{{"emissionIntensity", 3.0f + Mathf::sin(Time::time() * 4.0f) * 2.0f}});
}

CRAMION_SCRIPT(Cristal)
""")]),
    "ejemplo-olas": ("Cambiar los vértices de una malla cada frame.", [
        ("Olas.h", """#pragma once
#include <cramion/Script.h>
#include <vector>
using namespace cramion;

class Olas : public Script {
public:
    void start() override;
    void update(float dt) override;

private:
    Mesh malla;
    std::vector<Vec3> base;
};
"""),
        ("Olas.cpp", """#include "Olas.h"

void Olas::start() {
    malla = Mesh::plane(20.0f, 20.0f, 60, 60);
    const Value v = malla.vertices();
    for (std::size_t i = 0; i < v.size(); ++i) base.push_back(v[i]);
    entity().setMesh(malla);
}

void Olas::update(float) {
    Values v;
    v.reserve(base.size());
    for (const Vec3& p : base) v.push_back(Vec3{p.x, Mathf::sin(p.x * 0.8f + Time::time() * 2.0f) * 0.3f, p.z});
    malla.setVertices(v);
    malla.recalculateNormals();
}

CRAMION_SCRIPT(Olas)
""")]),
}


def page(slug, intro, files):
    out = [f"<p>{intro}</p>"]
    for name, code in files:
        out.append(f'<p class="file">Assets/Scripts/{name}</p>')
        out.append(f'<pre><code class="language-cpp">{html.escape(code.rstrip())}</code></pre>')
    return "\n".join(out) + "\n"


def main():
    folder = sys.argv[1] if len(sys.argv) > 1 else None
    for slug, (intro, files) in EXAMPLES.items():
        open(os.path.join(HERE, "pages", slug + ".html"), "w", encoding="utf-8").write(page(slug, intro, files))
        if folder:
            for name, code in files:
                open(os.path.join(folder, name), "w", encoding="utf-8").write(code)
    print(f"{len(EXAMPLES)} ejemplos de C++")


if __name__ == "__main__":
    main()

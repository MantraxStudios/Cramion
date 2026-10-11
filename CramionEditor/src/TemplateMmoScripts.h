#ifndef CRAMION_EDITOR_TEMPLATE_MMO_SCRIPTS_H
#define CRAMION_EDITOR_TEMPLATE_MMO_SCRIPTS_H

// Scripts de C++ de la plantilla "MMO RPG" (ProjectTemplates.cpp). Todo el
// juego va aqui: el motor solo pone la escena y la interfaz.
//
//   Heroe      el jugador y todos los sistemas: movimiento, estadisticas,
//              niveles, objetivo (Tab / clic), habilidades con mana,
//              lanzamiento y enfriamientos, inventario, equipo, tienda,
//              misiones, dialogos, botin, chat, minimapa, muerte y guardado
//   Enemigo    IA de los enemigos (patrulla, aggro, leash, jefe con area),
//              muerte, botin y reaparicion
//   Bot        otros "jugadores" del mundo: pasean, cazan y hablan por el chat
//   NPC        personajes del pueblo: miran al jugador y su marca de mision
//   Combate    lo comun del heroe y los bots (los enemigos los atacan)

#include "TemplateScripts.h"

namespace cramion::editor::mmo {

inline constexpr TemplateFile kFiles[] = {
    {"Bot.h",
R"CPP(// Bot.h: otro "jugador" del mundo (simulado). Pasea entre las zonas, caza los
// enemigos que encuentra, a veces habla por el chat y reaparece en el pueblo
// si muere. Le da vida al mundo como en un MMO.
#pragma once

#include "Combate.h"

#include <string>
#include <vector>

class Bot : public Combatiente {
public:
    Property<std::string> nombre{this, "nombre", "Jugador"};
    Property<std::string> clase{this, "clase", "guerrero", Tooltip("guerrero (cuerpo a cuerpo) o mago (a distancia)")};
    Property<int> nivel{this, "nivel", 3, Range(1, 10)};

    void start() override;
    void update(float dt) override;
    bool estaVivo() const override { return !muerto_; }
    void recibirDano(float cantidad, Entity fuente, const std::string& nombre) override;

private:
    bool iniciado_ = false;
    Vec3 casa_;
    float vidaMax_ = 1.0f;
    float vida_ = 1.0f;
    bool muerto_ = false;
    bool descansando_ = false;
    Entity heroe_, modelo_, objetivo_;
    std::vector<Entity> enemigos_, zonas_;
    float espera_ = 0.0f;
    float charla_ = 0.0f;
    float recarga_ = 0.0f;
    float repensar_ = 0.0f;
    float reaparecer_ = 0.0f;
    float alcance_ = 2.4f;
};
)CPP"},
    {"Combate.h",
R"CPP(// Combate.h: lo que tienen en comun el heroe y los otros jugadores (Bot): los
// enemigos los buscan, miran si siguen vivos y les hacen dano.
#pragma once

#include <cramion/Script.h>

#include <string>

using namespace cramion;

class Combatiente : public Script {
public:
    virtual bool estaVivo() const = 0;
    // Lo llaman los enemigos al golpear (nombre: quien o que golpea).
    virtual void recibirDano(float cantidad, Entity fuente, const std::string& nombre) = 0;
};

// El Combatiente de un objeto (nullptr si no tiene).
inline Combatiente* combatiente(Entity e) { return e.id() != 0 ? e.script<Combatiente>() : nullptr; }
)CPP"},
    {"Enemigo.h",
R"CPP(// Enemigo.h: enemigo del MMO. Patrulla cerca de su sitio, ataca al heroe o a
// los otros jugadores que se acercan, vuelve a casa curandose si lo alejan
// demasiado (leash), muere, suelta botin y reaparece. El Rey Goblin da
// pisotones en area (avisa antes: alejate).
#pragma once

#include <cramion/Script.h>

#include <string>
#include <vector>

using namespace cramion;

struct TipoEnemigo;

class Enemigo : public Script {
public:
    Property<std::string> tipo{this, "tipo", "lobo", Tooltip("lobo, goblin, chaman o rey")};
    Property<int> nivel{this, "nivel", 1, Range(1, 10)};

    void start() override { iniciar(); }
    void update(float dt) override;

    // Lo que el heroe y los bots preguntan.
    bool estaVivo() { iniciar(); return !muerto_; }
    std::string nombre();
    int nivelReal() { iniciar(); return nivelReal_; }
    float xp();
    bool esJefe();
    int vida() { iniciar(); return vida_ > 0.0f ? static_cast<int>(vida_) : 0; }
    int vidaMax() { iniciar(); return vidaMax_; }
    // Lo llaman el heroe y los bots. Devuelve el dano hecho.
    int danar(float cantidad, Entity atacante, bool esHeroe);

private:
    void iniciar();
    std::vector<Entity> candidatos() const;
    void mirar(const Vec3& posicion);
    void combatir(float dt);
    void habilidadJefe(float dt);
    void patrullar(float dt);
    void morir();
    void revivir();
    void pintarBarra();

    bool listo_ = false;
    const TipoEnemigo* datos_ = nullptr;
    int nivelReal_ = 1;
    int vidaMax_ = 1;
    float vida_ = 1.0f;
    float dano_ = 0.0f;
    Vec3 casa_;
    Entity modelo_, barra_, relleno_, heroe_, objetivo_;
    std::vector<Entity> bots_;
    bool muerto_ = false;
    bool volviendo_ = false;
    bool golpeadoPorHeroe_ = false;
    bool avisado_ = false;
    float espera_ = 0.0f;
    float recarga_ = 0.0f;
    float repensar_ = 0.0f;
    float pisoton_ = 9.0f;
    float reaparecer_ = 0.0f;
    bool barraVisible_ = true;
};
)CPP"},
    {"Heroe.h",
R"CPP(// Heroe.h: MMO RPG, el heroe y TODOS los sistemas del juego.
//
// Controles: WASD mover, Shift correr, Espacio saltar, clic derecho + raton
// girar la camara, rueda acercar. Tab o clic izquierdo (mirando a un
// enemigo) = objetivo. 1-6 habilidades y pociones. E hablar con un NPC.
// I inventario, C personaje, L diario de misiones, H ayuda, Escape cerrar.
// F9 borra la partida guardada.
#pragma once

#include "Combate.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

class Enemigo;
struct Mision;

class Heroe : public Combatiente {
public:
    Property<std::string> nombre{this, "nombre", "Heroe"};
    Property<float> velocidad{this, "velocidad", 6.0f, Range(0, 30)};
    Property<float> correr{this, "correr", 1.6f, Tooltip("Multiplicador con Shift")};
    Property<float> salto{this, "salto", 6.0f, Tooltip("m/s hacia arriba")};

    void start() override;
    void update(float dt) override;
    void onDestroy() override { guardar(); }

    bool estaVivo() const override { return !muerto_; }
    void recibirDano(float cantidad, Entity fuente, const std::string& quien) override;
    // Lo que usan los demas scripts: el chat, el aviso del centro y la muerte de un enemigo.
    void chat(const std::string& texto);
    void aviso(const std::string& texto, const Vec3& color = Vec3{1.0f, 1.0f, 1.0f});
    void enemigoMuerto(Enemigo& enemigo, bool credito, const std::vector<std::string>& botin, int oro);

private:
    struct Ranura {
        std::string id;
        int n = 0;
    };
    struct EstadoMision {
        std::string estado;  // activa, lista o hecha
        int progreso = 0;
    };
    struct Opcion {
        std::string texto;
        std::function<void()> accion;
    };
    struct BolsaBotin {
        Entity e;
        std::vector<std::string> items;
        int oro = 0;
        float vida = 90.0f;
    };
    struct Lanzamiento {
        int i = 0;
        float t = 0.0f;
        float dur = 0.0f;
    };
    struct Zona {
        Entity entidad;
        std::string nombre;
        float radio = 0.0f;
    };
    struct Interfaz {
        Entity jugador, objetivo, xpBarra, xpTexto, lanzamiento, chat, seguimiento, mapa, zona, aviso, ayuda;
        Entity inventario, personaje, diario, dialogo, muerte;
        std::vector<Entity> habilidades, flotantes, puntos, ranuras, opciones;
    };

    void buscarInterfaz();
    bool panelAbierto();
    void panel(const std::string& nombre, bool alternar = false);
    void teclas();
    void mover(float dt);
    bool enSuelo();
    void recalcular();
    void ganarXp(float cantidad);
    void regenerar(float dt);
    Enemigo* enemigoDe(Entity e) const;
    bool objetivoValido();
    std::vector<std::pair<Entity, float>> vivos(float maximo);
    void siguienteObjetivo();
    Entity apuntado();
    void objetivoCercano();
    bool usarHabilidad(int i);
    void lanzar(float dt);
    void danar(Entity enemigo, int cantidad, bool critico, const std::string& que);
    void aplicarHabilidad(int i);
    void atacarSolo(float dt);
    void morir(const std::string& causa);
    void reaparecerEn(float fraccion);
    void recogerBotin(float dt);
    int cuenta(const std::string& id) const;
    int dar(const std::string& id, int n, bool callado = false);
    void quitar(const std::string& id, int n);
    bool usarObjeto(const std::string& id);
    bool equipar(const std::string& id);
    void desequipar(Entity boton);
    void pulsarRanura(Entity boton);
    void comprar(const std::string& id);
    bool disponible(const Mision& m) const;
    bool aceptarMision(int id);
    void revisarRecoger();
    bool entregarMision(int id);
    Entity npcCercano(float maximo);
    void hablar();
    void dialogo(const std::string& titulo, const std::string& cuerpo, std::vector<Opcion> opciones);
    void abrirDialogo(Entity npc);
    std::string recompensa(const Mision& m) const;
    void ofrecerMision(Entity npc, const Mision& m);
    void ofrecerEntrega(Entity npc, const Mision& m);
    void pintarDialogoTienda();
    void cerrarDialogo();
    void pulsarCerrar(Entity boton);
    void alejarseDelDialogo();
    void marcasNpc();
    void animarAviso(float dt);
    void flotante(const std::string& texto, const Vec3& color);
    void animarFlotantes(float dt);
    void pintarTodo();
    void pintarHud();
    void pintarHabilidades();
    void pintarSeguimiento();
    void pintarMapa();
    void pintarPaneles();
    void guardar();
    bool cargar();

    Entity camara_, modelo_, spawn_, plantillaBotin_;
    std::vector<Entity> enemigos_, npcs_, bots_;
    std::vector<Zona> zonas_;
    Interfaz ui_;
    bool iniciado_ = false;

    int nivel_ = 1;
    int xp_ = 0;
    int oro_ = 10;
    std::vector<Ranura> inventario_;
    std::string arma_ = "espada_madera";
    std::string armadura_ = "tunica";
    std::map<int, EstadoMision> misiones_;
    std::map<int, float> enfriamientos_;
    float gcd_ = 0.0f;
    std::vector<std::string> lineas_;
    std::map<int, float> flotantes_;
    int siguienteFlotante_ = 0;
    std::vector<BolsaBotin> botines_;
    float combate_ = 0.0f;  // segundos que quedan "en combate"
    float autoataque_ = 0.0f;
    float guardado_ = 30.0f;
    float avisoTiempo_ = 0.01f;  // > 0: el primer frame esconde el aviso
    bool muerto_ = false;
    bool noGuardar_ = false;
    bool moviendose_ = false;
    bool vendiendo_ = false;
    bool hayLanzamiento_ = false;
    Lanzamiento lanzando_;
    Entity objetivo_, npcHablando_, tiendaNpc_;
    std::vector<Opcion> opciones_;

    float vida_ = 0.0f, mana_ = 0.0f;
    int vidaMax_ = 100, manaMax_ = 80, ataque_ = 0, defensa_ = 0, poder_ = 0;
};
)CPP"},
    {"NPC.h",
R"CPP(// NPC.h: personaje del pueblo. Mira al jugador cuando esta cerca y muestra su
// marca (amarilla = mision nueva, azul = mision para entregar). El heroe habla
// con el con E (Heroe.cpp lleva los dialogos, las misiones y la tienda).
#pragma once

#include <cramion/Script.h>

#include <string>

using namespace cramion;

class NPC : public Script {
public:
    Property<std::string> nombre{this, "nombre", "Aldeano"};
    Property<std::string> saludo{this, "saludo", "Buenos dias, viajero."};
    Property<bool> comerciante{this, "comerciante", false};

    void start() override;
    void update(float dt) override;
    // "" (sin marca), "mision" o "entregar".
    void marca(const std::string& tipo);

private:
    Entity jugador_, modelo_, marca_;
    std::string estadoMarca_ = "?";
    bool marcaActiva_ = false;
    float fase_ = 0.0f;
};
)CPP"},
    {"Bot.cpp",
R"CPP(// Bot.cpp: otro "jugador" del mundo (ver Bot.h).
#include "Bot.h"

#include "Enemigo.h"
#include "Heroe.h"

#include <algorithm>
#include <iterator>

namespace {

const char* const kFrases[] = {
    "¿Alguien para el Rey Goblin?", "lf grupo campamento goblin", "vendo pieles de lobo baratas",
    "¿donde se entrega la mision de los lobos?", "gg", "cuidado con el pisoton del Rey",
    "¿alguien tiene pociones de mana?", "este servidor va genial hoy", "jajaja casi me mata un lobo",
    "subi de nivel!", "¿donde esta el mercader?", "wts espada de acero", "brb", "alguien sabe de algun evento?",
};

float azar(float a, float b) { return Random::range(a, b).asFloat(); }

}  // namespace

void Bot::start() {
    casa_ = entity().position();
    vidaMax_ = 80.0f + nivel * 25.0f;
    vida_ = vidaMax_;
    muerto_ = false;
    heroe_ = Scene::find("Jugador");
    for (const Value& e : Scene::findAllWithTag("Enemigo").items()) enemigos_.push_back(e.asEntity());
    modelo_ = entity().find("Modelo").asEntity();
    for (const char* n : {"Zona Pueblo", "Zona Bosque", "Zona Campamento"}) {
        const Entity z = Scene::find(n);
        if (z.id() != 0) zonas_.push_back(z);
    }
    espera_ = azar(1, 4);
    charla_ = azar(15, 50);
    alcance_ = clase.get() == "mago" ? 12.0f : 2.4f;
    iniciado_ = true;
}

void Bot::update(float dt) {
    if (!iniciado_) return;
    if (muerto_) {
        reaparecer_ -= dt;
        if (reaparecer_ <= 0.0f) {
            muerto_ = false;
            vida_ = vidaMax_;
            entity().setPosition(casa_);
            if (modelo_.id() != 0) modelo_.setActive(true);
        }
        return;
    }
    recarga_ = std::max(0.0f, recarga_ - dt);
    repensar_ -= dt;
    charla_ -= dt;
    if (charla_ <= 0.0f) {
        charla_ = azar(25, 70);
        if (Heroe* h = heroe_.script<Heroe>()) {
            const int i = Random::int_(0, static_cast<int>(std::size(kFrases)) - 1).asInt();
            h->chat("[General] " + nombre.get() + ": " + kFrases[i]);
        }
    }

    // Poca vida: se retira a descansar.
    if (vida_ < vidaMax_ * 0.3f && !descansando_) {
        descansando_ = true;
        objetivo_ = Entity{};
        entity().moveTo(casa_);
    }
    if (descansando_) {
        vida_ = std::min(vidaMax_, vida_ + vidaMax_ * 0.08f * dt);
        if (vida_ >= vidaMax_ * 0.95f) descansando_ = false;
        return;
    }

    const Vec3 yo = entity().position();
    if (objetivo_.id() != 0) {
        Enemigo* s = objetivo_.script<Enemigo>();
        if (s == nullptr || !s->estaVivo() || Vec3::distance(yo, objetivo_.position()) > 35.0f) objetivo_ = Entity{};
    }
    if (objetivo_.id() == 0 && repensar_ <= 0.0f) {
        repensar_ = 0.5f;
        Entity mejor;
        float dist = 16.0f;
        for (const Entity& e : enemigos_) {
            Enemigo* s = e.script<Enemigo>();
            if (s != nullptr && s->estaVivo() && !s->esJefe()) {
                const float d = Vec3::distance(yo, e.position());
                if (d < dist) {
                    mejor = e;
                    dist = d;
                }
            }
        }
        objetivo_ = mejor;
    }

    if (objetivo_.id() != 0) {
        const Vec3 p = objetivo_.position();
        if (Vec3::distance(yo, p) > alcance_) {
            if (repensar_ <= 0.0f) {
                repensar_ = 0.3f;
                entity().moveTo(p);
            }
        } else {
            if (entity().isMoving().truthy()) entity().stopMoving();
            if (recarga_ <= 0.0f) {
                const bool mago = clase.get() == "mago";
                recarga_ = mago ? 2.2f : 1.7f;
                const float golpe = (mago ? 14.0f : 10.0f) + nivel * 2.0f;
                if (Enemigo* s = objetivo_.script<Enemigo>()) s->danar(golpe * azar(0.85f, 1.15f), entity(), false);
            }
        }
        return;
    }

    // Pasear: a un punto al azar de alguna zona.
    if (!entity().isMoving().truthy()) {
        espera_ -= dt;
        if (espera_ <= 0.0f && !zonas_.empty()) {
            espera_ = azar(4, 12);
            const Entity& z = zonas_[static_cast<std::size_t>(Random::int_(0, static_cast<int>(zonas_.size()) - 1).asInt())];
            const Value p = Navigation::randomPoint(z.position(), 20);
            if (!p.isNil()) entity().moveTo(p.asVec3());
        }
    }
}

void Bot::recibirDano(float cantidad, Entity fuente, const std::string& quien) {
    if (muerto_ || !iniciado_) return;
    vida_ -= cantidad;
    if (fuente.id() != 0 && objetivo_.id() == 0 && !descansando_) objetivo_ = fuente;
    if (vida_ <= 0.0f) {
        muerto_ = true;
        objetivo_ = Entity{};
        descansando_ = false;
        entity().stopMoving();
        if (modelo_.id() != 0) modelo_.setActive(false);
        reaparecer_ = 12.0f;
        if (Heroe* h = heroe_.script<Heroe>()) h->chat("[Sistema] " + nombre.get() + " ha muerto (" + (quien.empty() ? "?" : quien) + ").");
    }
}

CRAMION_SCRIPT(Bot)
)CPP"},
    {"Enemigo.cpp",
R"CPP(// Enemigo.cpp: enemigo del MMO (ver Enemigo.h).
#include "Enemigo.h"

#include "Combate.h"
#include "Heroe.h"

#include <algorithm>
#include <cmath>

struct Botin {
    const char* item;
    float probabilidad;
};

struct TipoEnemigo {
    const char* id;
    const char* nombre;
    float vida, dano, cadencia, alcance, aggro, xp;
    int oroMin, oroMax;
    std::vector<Botin> botin;
    float reaparece;
    bool distancia;
    bool jefe;
};

namespace {

const TipoEnemigo kTipos[] = {
    {"lobo", "Lobo", 60, 6, 1.6f, 2.3f, 11, 30, 1, 3, {{"piel_lobo", 0.6f}, {"colmillo", 0.45f}, {"pocion_vida", 0.12f}}, 25, false,
     false},
    {"goblin", "Goblin", 95, 9, 1.8f, 2.4f, 13, 50, 3, 7, {{"oreja_goblin", 0.55f}, {"pocion_mana", 0.15f}, {"espada_acero", 0.04f}}, 30,
     false, false},
    {"chaman", "Chaman goblin", 70, 12, 2.6f, 14, 16, 60, 4, 9, {{"oreja_goblin", 0.5f}, {"pocion_mana", 0.3f}, {"cota_malla", 0.03f}}, 35,
     true, false},
    {"rey", "Rey Goblin", 900, 20, 2.2f, 3.6f, 16, 450, 60, 90, {{"hacha_rey", 1.0f}, {"pocion_vida", 1.0f}}, 120, false, true},
};
constexpr float kLeash = 30.0f;

float azar(float a, float b) { return Random::range(a, b).asFloat(); }

bool vivo(Entity e) {
    const Combatiente* c = combatiente(e);
    return c != nullptr && c->estaVivo();
}

}  // namespace

void Enemigo::iniciar() {
    if (listo_) return;
    listo_ = true;
    datos_ = &kTipos[0];
    for (const TipoEnemigo& t : kTipos) {
        if (tipo.get() == t.id) datos_ = &t;
    }
    nivelReal_ = std::max(1, nivel.get());
    const float escala = 1.0f + 0.22f * static_cast<float>(nivelReal_ - 1);
    vidaMax_ = static_cast<int>(std::floor(datos_->vida * escala));
    vida_ = static_cast<float>(vidaMax_);
    dano_ = datos_->dano * (1.0f + 0.18f * static_cast<float>(nivelReal_ - 1));
    casa_ = entity().position();
    modelo_ = entity().find("Modelo").asEntity();
    barra_ = entity().find("Barra").asEntity();
    if (barra_.id() != 0) relleno_ = barra_.find("Relleno").asEntity();
    muerto_ = false;
    espera_ = azar(1, 5);
    heroe_ = Scene::find("Jugador");
    for (const Value& b : Scene::findAllWithTag("Bot").items()) bots_.push_back(b.asEntity());
}

std::string Enemigo::nombre() {
    iniciar();
    return datos_->nombre;
}

float Enemigo::xp() {
    iniciar();
    return datos_->xp * (1.0f + 0.25f * static_cast<float>(nivelReal_ - 1));
}

bool Enemigo::esJefe() {
    iniciar();
    return datos_->jefe;
}

std::vector<Entity> Enemigo::candidatos() const {
    std::vector<Entity> lista;
    if (heroe_.id() != 0) lista.push_back(heroe_);
    lista.insert(lista.end(), bots_.begin(), bots_.end());
    return lista;
}

void Enemigo::update(float dt) {
    iniciar();
    if (muerto_) {
        reaparecer_ -= dt;
        if (reaparecer_ <= 0.0f) revivir();
        return;
    }
    recarga_ = std::max(0.0f, recarga_ - dt);
    repensar_ -= dt;

    // Lejos de casa o sin objetivo vivo: vuelve curandose.
    if (objetivo_.id() != 0 && (!vivo(objetivo_) || Vec3::distance(entity().position(), casa_) > kLeash)) {
        objetivo_ = Entity{};
        golpeadoPorHeroe_ = false;
        volviendo_ = true;
        entity().moveTo(casa_);
    }
    if (volviendo_) {
        vida_ = std::min(static_cast<float>(vidaMax_), vida_ + vidaMax_ * 0.5f * dt);
        if (Vec3::distance(entity().position(), casa_) < 2.5f || !entity().isMoving().truthy()) volviendo_ = false;
        pintarBarra();
        return;
    }

    if (objetivo_.id() == 0) {
        const Vec3 yo = entity().position();
        Entity mejor;
        float distancia = datos_->aggro;
        for (const Entity& c : candidatos()) {
            if (vivo(c)) {
                const float d = Vec3::distance(yo, c.position());
                if (d < distancia) {
                    mejor = c;
                    distancia = d;
                }
            }
        }
        objetivo_ = mejor;
    }

    if (objetivo_.id() != 0) {
        combatir(dt);
    } else {
        patrullar(dt);
        vida_ = std::min(static_cast<float>(vidaMax_), vida_ + vidaMax_ * 0.05f * dt);
    }
    pintarBarra();
}

void Enemigo::mirar(const Vec3& posicion) {
    const Vec3 p = entity().position();
    const Vec3 destino{posicion.x, p.y, posicion.z};
    if ((destino - p).length() > 0.1f) entity().lookAt(destino);
}

void Enemigo::combatir(float dt) {
    const Entity objetivo = objetivo_;
    const Vec3 yo = entity().position();
    const Vec3 suyo = objetivo.position();
    const float d = Vec3::distance(yo, suyo);
    const float alcance = datos_->alcance;
    if (d > alcance) {
        if (repensar_ <= 0.0f) {
            repensar_ = 0.25f;
            Vec3 destino = suyo;
            if (datos_->distancia) destino = Vec3::lerp(yo, suyo, 1.0f - (alcance * 0.8f) / d);
            entity().moveTo(destino);
        }
    } else {
        if (entity().isMoving().truthy()) entity().stopMoving();
        mirar(suyo);
        if (recarga_ <= 0.0f) {
            recarga_ = datos_->cadencia;
            const float golpe = std::floor(dano_ * azar(0.85f, 1.15f) + 0.5f);
            std::string quien = datos_->nombre;
            if (datos_->distancia) quien += " (descarga)";
            if (Combatiente* c = combatiente(objetivo)) c->recibirDano(golpe, entity(), quien);
        }
    }
    if (datos_->jefe) habilidadJefe(dt);
}

// Pisoton: 1.5 s de aviso y dano a todos los que esten a menos de 6.5 m.
void Enemigo::habilidadJefe(float dt) {
    pisoton_ -= dt;
    const Vec3 yo = entity().position();
    if (pisoton_ <= 1.5f && !avisado_) {
        avisado_ = true;
        if (Heroe* h = heroe_.id() != 0 ? heroe_.script<Heroe>() : nullptr) {
            if (Vec3::distance(yo, heroe_.position()) < 30.0f) h->aviso("¡El Rey Goblin va a dar un pisoton! ¡Alejate!", Vec3{1.0f, 0.45f, 0.3f});
        }
    }
    if (pisoton_ <= 0.0f) {
        pisoton_ = 9.0f;
        avisado_ = false;
        for (const Entity& c : candidatos()) {
            Combatiente* s = combatiente(c);
            if (s != nullptr && s->estaVivo() && Vec3::distance(yo, c.position()) < 6.5f) {
                s->recibirDano(std::floor(dano_ * 1.6f), entity(), "Pisoton del Rey");
            }
        }
    }
}

void Enemigo::patrullar(float dt) {
    if (entity().isMoving().truthy()) return;
    espera_ -= dt;
    if (espera_ > 0.0f) return;
    espera_ = azar(3, 8);
    const Value p = Navigation::randomPoint(casa_, datos_->jefe ? 3 : 7);
    if (!p.isNil()) entity().moveTo(p.asVec3());
}

int Enemigo::danar(float cantidad, Entity atacante, bool esHeroe) {
    iniciar();
    if (muerto_) return 0;
    const int hecho = std::max(1, static_cast<int>(std::floor(cantidad)));
    vida_ -= static_cast<float>(hecho);
    if (esHeroe) golpeadoPorHeroe_ = true;
    if (atacante.id() != 0) {
        volviendo_ = false;
        objetivo_ = atacante;
    }
    if (vida_ <= 0.0f) morir();
    pintarBarra();
    return hecho;
}

void Enemigo::morir() {
    vida_ = 0.0f;
    muerto_ = true;
    objetivo_ = Entity{};
    entity().stopMoving();
    if (modelo_.id() != 0) modelo_.setActive(false);
    reaparecer_ = datos_->reaparece;
    std::vector<std::string> botin;
    for (const Botin& b : datos_->botin) {
        if (Random::chance(b.probabilidad).truthy()) botin.push_back(b.item);
    }
    const int oro = Random::int_(datos_->oroMin, datos_->oroMax).asInt();
    if (Heroe* h = heroe_.id() != 0 ? heroe_.script<Heroe>() : nullptr) h->enemigoMuerto(*this, golpeadoPorHeroe_, botin, oro);
    golpeadoPorHeroe_ = false;
    pintarBarra();
}

void Enemigo::revivir() {
    muerto_ = false;
    entity().setPosition(casa_);
    vida_ = static_cast<float>(vidaMax_);
    objetivo_ = Entity{};
    volviendo_ = false;
    if (modelo_.id() != 0) modelo_.setActive(true);
    pintarBarra();
}

// La barra de vida sobre la cabeza (solo si esta herido).
void Enemigo::pintarBarra() {
    if (barra_.id() == 0) return;
    const float frac = vida_ / static_cast<float>(vidaMax_);
    const bool mostrar = !muerto_ && frac < 0.999f;
    if (mostrar != barraVisible_) {
        barraVisible_ = mostrar;
        barra_.setActive(mostrar);
    }
    if (mostrar && relleno_.id() != 0) {
        relleno_.setScale(Vec3{std::max(0.02f, frac), 1.0f, 1.0f});
        relleno_.setLocalPosition(Vec3{(frac - 1.0f) * 0.5f, 0.0f, 0.0f});
    }
}

CRAMION_SCRIPT(Enemigo)
)CPP"},
    {"Heroe.cpp",
R"CPP(// Heroe.cpp: el heroe y todos los sistemas del MMO: movimiento, estadisticas,
// niveles, objetivo (Tab / clic), habilidades con mana, lanzamiento y
// enfriamientos, inventario, equipo, tienda, misiones, dialogos, botin, chat,
// minimapa, muerte y guardado (Prefs).
#include "Heroe.h"

#include "Bot.h"
#include "Enemigo.h"
#include "NPC.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

// =========================================================================
// Datos del juego
// =========================================================================

struct Item {
    const char* id;
    const char* nombre;
    const char* tipo;  // consumible, botin, arma o armadura
    int cura, mana, ataque, defensa, nivel, precio;
    Vec3 color;
};

struct Mision {
    int id;
    const char* nombre;
    const char* npc;
    const char* tipo;  // matar o recoger
    const char* objetivo;
    int cantidad, nivel, requiere;
    const char* texto;
    const char* fin;
    int xp, oro;
    const char* premio;
    int premioCantidad;
};

namespace {

//                id               nombre                  tipo          cura mana ataque defensa nivel precio
const Item kItems[] = {
    {"pocion_vida", "Pocion de vida", "consumible", 80, 0, 0, 0, 0, 8, {0.9f, 0.2f, 0.2f}},
    {"pocion_mana", "Pocion de mana", "consumible", 0, 60, 0, 0, 0, 8, {0.25f, 0.45f, 1.0f}},
    {"piel_lobo", "Piel de lobo", "botin", 0, 0, 0, 0, 0, 4, {0.6f, 0.45f, 0.3f}},
    {"colmillo", "Colmillo", "botin", 0, 0, 0, 0, 0, 3, {0.92f, 0.9f, 0.8f}},
    {"oreja_goblin", "Oreja de goblin", "botin", 0, 0, 0, 0, 0, 5, {0.45f, 0.75f, 0.3f}},
    {"espada_madera", "Espada de practica", "arma", 0, 0, 3, 0, 0, 4, {0.7f, 0.55f, 0.35f}},
    {"espada_hierro", "Espada de hierro", "arma", 0, 0, 8, 0, 2, 40, {0.75f, 0.78f, 0.82f}},
    {"espada_acero", "Espada de acero", "arma", 0, 0, 14, 0, 4, 110, {0.45f, 0.7f, 1.0f}},
    {"hacha_rey", "Hacha del Rey Goblin", "arma", 0, 0, 24, 0, 5, 300, {1.0f, 0.55f, 0.1f}},
    {"tunica", "Tunica de aprendiz", "armadura", 0, 0, 0, 2, 0, 4, {0.7f, 0.65f, 0.55f}},
    {"armadura_cuero", "Armadura de cuero", "armadura", 0, 0, 0, 5, 2, 45, {0.6f, 0.4f, 0.25f}},
    {"cota_malla", "Cota de malla", "armadura", 0, 0, 0, 10, 4, 120, {0.45f, 0.7f, 1.0f}},
};
const char* const kTienda[] = {"pocion_vida", "pocion_mana", "espada_hierro", "armadura_cuero", "espada_acero", "cota_malla"};
constexpr int kPila = 20;  // objetos iguales por ranura
constexpr int kRanuras = 20;

const Mision kMisiones[] = {
    {1, "Lobos hambrientos", "Capitana Elena", "matar", "lobo", 5, 1, 0,
     "Los lobos del Bosque Gris atacan a los viajeros del camino del oeste. Acaba con 5 lobos y vuelve a verme.",
     "Buen trabajo. El camino vuelve a ser seguro. Toma esta espada, te hara falta.", 140, 20, "espada_hierro", 1},
    {2, "Pieles para el invierno", "Mercader Tomas", "recoger", "piel_lobo", 4, 1, 0,
     "El invierno llega y no me quedan pieles. Traeme 4 pieles de lobo y te pagare bien.",
     "¡Perfectas! Con esto abrigare a medio pueblo.", 110, 35, "armadura_cuero", 1},
    {3, "El campamento goblin", "Capitana Elena", "matar", "goblin", 6, 3, 1,
     "Los goblins han levantado un campamento al este. Elimina a 6 goblins antes de que ataquen el pueblo.",
     "Eso les enseñara. Pero su jefe sigue ahi fuera...", 300, 45, "pocion_vida", 3},
    {4, "Pruebas del mal", "Hermano Anselmo", "recoger", "oreja_goblin", 5, 3, 0,
     "Necesito 5 orejas de goblin para estudiar su magia oscura. No preguntes.",
     "Interesante... muy interesante. Toma, para tu magia.", 240, 30, "pocion_mana", 4},
    {5, "El Rey Goblin", "Hermano Anselmo", "matar", "rey", 1, 5, 3,
     "El Rey Goblin se esconde en su guarida al norte. Es fuerte: lleva pociones y cuidado con su pisoton.",
     "¡Lo has logrado! Villa Alba te lo agradecera siempre, heroe.", 700, 150, "cota_malla", 1},
};

struct Habilidad {
    const char* nombre;
    float mana, cd, alcance, lanzar;
    int nivel;  // 0 = siempre
    bool objetivo;
    const char* item;  // las pociones
};
const Habilidad kHabilidades[] = {
    {"Golpe", 0, 0, 3.6f, 0, 1, true, nullptr},
    {"Bola de fuego", 15, 0, 26, 1.4f, 1, true, nullptr},
    {"Curar", 22, 8, 0, 1.6f, 2, false, nullptr},
    {"Torbellino", 28, 10, 5.5f, 0, 3, false, nullptr},
    {"Pocion de vida", 0, 12, 0, 0, 0, false, "pocion_vida"},
    {"Pocion de mana", 0, 12, 0, 0, 0, false, "pocion_mana"},
};
constexpr int kNumHabilidades = static_cast<int>(std::size(kHabilidades));
constexpr float kGcd = 1.0f;  // enfriamiento global entre habilidades
constexpr int kNivelMax = 10;
constexpr float kAutoataque = 2.0f;  // segundos entre golpes automaticos
int xpNivel(int n) { return static_cast<int>(std::floor(100.0 * std::pow(n, 1.5))); }

// Colores de los textos flotantes.
const Vec3 kDano{1.0f, 0.9f, 0.3f};
const Vec3 kCritico{1.0f, 0.55f, 0.1f};
const Vec3 kCura{0.4f, 1.0f, 0.45f};
const Vec3 kRecibido{1.0f, 0.3f, 0.25f};
const Vec3 kMana{0.45f, 0.65f, 1.0f};
const Vec3 kDorado{1.0f, 0.85f, 0.3f};
const Vec3 kBlanco{1.0f, 1.0f, 1.0f};

const Item* itemDe(const std::string& id) {
    for (const Item& it : kItems) {
        if (id == it.id) return &it;
    }
    return nullptr;
}

const Mision* misionDe(int id) {
    for (const Mision& m : kMisiones) {
        if (m.id == id) return &m;
    }
    return nullptr;
}

std::string fmt(const char* formato, ...) {
    char texto[1024];
    va_list args;
    va_start(args, formato);
    std::vsnprintf(texto, sizeof(texto), formato, args);
    va_end(args);
    return texto;
}

int redondear(float v) { return static_cast<int>(std::floor(v + 0.5f)); }
float azar(float a, float b) { return Random::range(a, b).asFloat(); }

std::vector<std::string> dividir(const std::string& texto, char sep) {
    std::vector<std::string> partes;
    if (texto.empty()) return partes;
    std::size_t a = 0;
    while (true) {
        const std::size_t b = texto.find(sep, a);
        partes.push_back(texto.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return partes;
}

// El numero del final del nombre ("Ranura 12" = 12).
int numeroDe(const std::string& nombre) {
    std::size_t i = nombre.size();
    while (i > 0 && nombre[i - 1] >= '0' && nombre[i - 1] <= '9') --i;
    return i < nombre.size() ? std::atoi(nombre.c_str() + i) : 0;
}

bool hay(const Entity& e) { return e.id() != 0; }
Entity hijo(const Entity& padre, const std::string& nombre) { return hay(padre) ? padre.find(nombre).asEntity() : Entity{}; }
bool activo(const Entity& e) { return hay(e) && e.active(); }
void activar(const Entity& e, bool on) {
    if (hay(e)) e.setActive(on);
}
void texto(const Entity& e, const std::string& t) {
    if (hay(e)) e.set("text", t);
}
void color(const Entity& e, const Vec3& c) {
    if (hay(e)) e.set("color", c);
}

// Una barra de la interfaz: el ancho del RectTransform es la fraccion.
void barra(const Entity& e, float fraccion, float ancho) {
    if (!hay(e)) return;
    const Vec3 tam = e.uiSize().asVec3();
    e.setUiSize(Vec3{std::max(0.0f, ancho * Mathf::clamp01(fraccion)), tam.y, 0.0f});
}

std::string unir(const std::vector<std::string>& lineas, const char* sep) {
    std::string t;
    for (std::size_t i = 0; i < lineas.size(); ++i) {
        if (i > 0) t += sep;
        t += lineas[i];
    }
    return t;
}

}  // namespace

// =========================================================================
// Inicio
// =========================================================================

void Heroe::start() {
    camara_ = Scene::find("Main Camera");
    modelo_ = entity().find("Modelo").asEntity();
    spawn_ = Scene::find("Punto de reaparicion");
    plantillaBotin_ = Scene::find("Plantilla Botin");
    for (const Value& e : Scene::findAllWithTag("Enemigo").items()) enemigos_.push_back(e.asEntity());
    for (const Value& e : Scene::findAllWithTag("NPC").items()) npcs_.push_back(e.asEntity());
    for (const Value& e : Scene::findAllWithTag("Bot").items()) bots_.push_back(e.asEntity());
    const Zona zonas[] = {{{}, "Villa Alba", 28}, {{}, "Bosque Gris", 40}, {{}, "Campamento Goblin", 38}, {{}, "Guarida del Rey", 26}};
    const char* const objetos[] = {"Zona Pueblo", "Zona Bosque", "Zona Campamento", "Zona Guarida"};
    for (std::size_t i = 0; i < std::size(zonas); ++i) {
        const Entity e = Scene::find(objetos[i]);
        if (hay(e)) zonas_.push_back({e, zonas[i].nombre, zonas[i].radio});
    }
    buscarInterfaz();

    // Los botones de la interfaz.
    on("OnReaparecer", [this](const Value&) { reaparecerEn(0.5f); });
    on("OnDesequipar", [this](const Value& boton) { desequipar(boton.asEntity()); });
    on("OnRanura", [this](const Value& boton) { pulsarRanura(boton.asEntity()); });
    on("OnOpcion", [this](const Value& boton) {
        const int i = numeroDe(boton.asEntity().name());
        if (i < 1 || i > static_cast<int>(opciones_.size())) return;
        const std::function<void()> accion = opciones_[static_cast<std::size_t>(i - 1)].accion;  // copia: la opcion puede cambiar las opciones
        if (accion) accion();
    });
    on("OnCerrar", [this](const Value& boton) { pulsarCerrar(boton.asEntity()); });
    on("OnHabilidad", [this](const Value& boton) { usarHabilidad(numeroDe(boton.asEntity().name())); });

    const bool nueva = !cargar();
    recalcular();
    vida_ = static_cast<float>(vidaMax_);
    mana_ = static_cast<float>(manaMax_);
    iniciado_ = true;
    if (nueva) {
        dar("pocion_vida", 3, true);
        dar("pocion_mana", 2, true);
    }
    chat("[Sistema] Bienvenido a Villa Alba, " + nombre.get() + ". Pulsa H para ver los controles.");
    chat("[Sistema] Habla con los personajes con una ! amarilla (E) para conseguir misiones.");
    panel("");
    pintarTodo();
}

void Heroe::buscarInterfaz() {
    Interfaz& u = ui_;
    u.jugador = Scene::find("UI_Jugador");
    u.objetivo = Scene::find("UI_Objetivo");
    u.xpBarra = Scene::find("UI_XPBarra");
    u.xpTexto = Scene::find("UI_XPTexto");
    u.lanzamiento = Scene::find("UI_Lanzamiento");
    u.chat = Scene::find("UI_Chat");
    u.seguimiento = Scene::find("UI_Seguimiento");
    u.mapa = Scene::find("UI_Mapa");
    u.zona = Scene::find("UI_Zona");
    u.aviso = Scene::find("UI_Aviso");
    u.ayuda = Scene::find("UI_Ayuda");
    u.inventario = Scene::find("UI_Inventario");
    u.personaje = Scene::find("UI_Personaje");
    u.diario = Scene::find("UI_Diario");
    u.dialogo = Scene::find("UI_Dialogo");
    u.muerte = Scene::find("UI_Muerte");
    for (int i = 1; i <= kNumHabilidades; ++i) u.habilidades.push_back(Scene::find("UI_Hab " + std::to_string(i)));
    for (int i = 1; i <= 8; ++i) u.flotantes.push_back(Scene::find("UI_Flotante " + std::to_string(i)));
    if (hay(u.mapa)) {
        for (int i = 1; i <= 40; ++i) u.puntos.push_back(hijo(u.mapa, "Punto " + std::to_string(i)));
    }
    if (hay(u.inventario)) {
        for (int i = 1; i <= kRanuras; ++i) u.ranuras.push_back(hijo(u.inventario, "Ranura " + std::to_string(i)));
    }
    if (hay(u.dialogo)) {
        for (int i = 1; i <= 6; ++i) u.opciones.push_back(hijo(u.dialogo, "Opcion " + std::to_string(i)));
    }
}

// =========================================================================
// Cada frame
// =========================================================================

void Heroe::update(float dt) {
    gcd_ = std::max(0.0f, gcd_ - dt);
    for (auto& [i, t] : enfriamientos_) t = std::max(0.0f, t - dt);
    combate_ = std::max(0.0f, combate_ - dt);
    animarFlotantes(dt);
    animarAviso(dt);
    if (!muerto_) {
        teclas();
        mover(dt);
        lanzar(dt);
        atacarSolo(dt);
        regenerar(dt);
        recogerBotin(dt);
        alejarseDelDialogo();
    }
    marcasNpc();
    pintarHud();
    guardado_ -= dt;
    if (guardado_ <= 0.0f) {
        guardado_ = 30.0f;
)CPP"
R"CPP(        guardar();
    }
}

bool Heroe::panelAbierto() {
    const Interfaz& u = ui_;
    return activo(u.inventario) || activo(u.personaje) || activo(u.diario) || activo(u.dialogo) || activo(u.ayuda);
}

// Abre un panel ("inventario", "personaje", "diario", "ayuda") o los cierra ("").
void Heroe::panel(const std::string& nombre, bool alternar) {
    const Interfaz& u = ui_;
    const std::pair<const char*, Entity> paneles[] = {
        {"inventario", u.inventario}, {"personaje", u.personaje}, {"diario", u.diario}, {"ayuda", u.ayuda}};
    std::string abrir = nombre;
    if (alternar) {
        for (const auto& [n, p] : paneles) {
            if (nombre == n && activo(p)) abrir.clear();
        }
    }
    for (const auto& [n, p] : paneles) activar(p, abrir == n);
    if (abrir != "inventario") vendiendo_ = false;
    if (!abrir.empty()) activar(u.dialogo, false);
    pintarPaneles();
}

void Heroe::teclas() {
    if (Input::keyDown("i")) panel("inventario", true);
    if (Input::keyDown("c")) panel("personaje", true);
    if (Input::keyDown("l")) panel("diario", true);
    if (Input::keyDown("h")) panel("ayuda", true);
    if (Input::keyDown("escape")) {
        if (panelAbierto()) {
            panel("");
            activar(ui_.dialogo, false);
        } else {
            objetivo_ = Entity{};
        }
    }
    if (Input::keyDown("tab")) siguienteObjetivo();
    if (Input::mouseButton(0, 1) && !panelAbierto()) {
        const Entity e = apuntado();
        if (hay(e)) objetivo_ = e;
    }
    if (Input::keyDown("e")) hablar();
    for (int i = 1; i <= kNumHabilidades; ++i) {
        if (Input::keyDown(std::to_string(i))) usarHabilidad(i);
    }
    if (Input::keyDown("f9")) {
        Prefs::deleteKey("mmo_partida");
        chat("[Sistema] Partida borrada: la proxima vez empezaras de cero.");
        noGuardar_ = true;
    }
}

void Heroe::mover(float dt) {
    Vec3 adelante{0.0f, 0.0f, -1.0f};
    Vec3 derecha{1.0f, 0.0f, 0.0f};
    if (hay(camara_)) {
        adelante = camara_.forward();
        adelante.y = 0.0f;
        adelante = adelante.normalized();
        derecha = camara_.right();
        derecha.y = 0.0f;
        derecha = derecha.normalized();
    }
    Vec3 direccion = derecha * Input::axis("Horizontal") + adelante * Input::axis("Vertical");
    if (direccion.length() > 1.0f) direccion = direccion.normalized();
    float rapidez = velocidad;
    if (Input::key("shift")) rapidez *= correr;
    const Vec3 objetivo = direccion * rapidez;
    Vec3 v = entity().velocity();
    const float t = Mathf::clamp01(12.0f * dt);
    v.x = Mathf::lerp(v.x, objetivo.x, t);
    v.z = Mathf::lerp(v.z, objetivo.z, t);
    if (Input::keyDown("space") && enSuelo()) v.y = salto;
    entity().setVelocity(v);
    moviendose_ = direccion.length() > 0.1f;
    if (hay(modelo_)) {
        if (moviendose_) {
            modelo_.lookAt(modelo_.position() + direccion);
        } else if (objetivoValido()) {
            const Vec3 p = objetivo_.position();
            modelo_.lookAt(Vec3{p.x, modelo_.position().y, p.z});
        }
    }
    if (entity().position().y < -30.0f) reaparecerEn(1.0f);
}

bool Heroe::enSuelo() {
    RaycastHit hit;
    return Physics::raycast(entity().position() + Vec3{0.0f, -1.02f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f}, 0.25f, &hit, entity());
}

// =========================================================================
// Estadisticas y niveles
// =========================================================================

void Heroe::recalcular() {
    const Item* arma = itemDe(arma_);
    const Item* armadura = itemDe(armadura_);
    vidaMax_ = 100 + (nivel_ - 1) * 28;
    manaMax_ = 80 + (nivel_ - 1) * 16;
    ataque_ = 4 + nivel_ * 2 + (arma != nullptr ? arma->ataque : 0);
    defensa_ = nivel_ + (armadura != nullptr ? armadura->defensa : 0);
    poder_ = 10 + nivel_ * 4;
    vida_ = std::min(vida_, static_cast<float>(vidaMax_));
    mana_ = std::min(mana_, static_cast<float>(manaMax_));
}

void Heroe::ganarXp(float cantidad) {
    if (nivel_ >= kNivelMax) return;
    const int ganada = static_cast<int>(std::floor(cantidad));
    xp_ += ganada;
    chat(fmt("[Combate] Ganas %d de experiencia.", ganada));
    while (nivel_ < kNivelMax && xp_ >= xpNivel(nivel_)) {
        xp_ -= xpNivel(nivel_);
        ++nivel_;
        recalcular();
        vida_ = static_cast<float>(vidaMax_);
        mana_ = static_cast<float>(manaMax_);
        aviso("¡Nivel " + std::to_string(nivel_) + "!", Vec3{1.0f, 0.85f, 0.3f});
        chat("[Sistema] ¡Has subido al nivel " + std::to_string(nivel_) + "! Vida y mana al maximo.");
        for (const Habilidad& h : kHabilidades) {
            if (h.nivel == nivel_) chat(std::string("[Sistema] Nueva habilidad: ") + h.nombre + ".");
        }
    }
    if (nivel_ >= kNivelMax) xp_ = 0;
    guardar();
}

void Heroe::regenerar(float dt) {
    const bool fuera = combate_ <= 0.0f;
    vida_ = std::min(static_cast<float>(vidaMax_), vida_ + vidaMax_ * (fuera ? 0.04f : 0.004f) * dt);
    mana_ = std::min(static_cast<float>(manaMax_), mana_ + manaMax_ * (fuera ? 0.05f : 0.012f) * dt);
}

// =========================================================================
// Objetivo
// =========================================================================

Enemigo* Heroe::enemigoDe(Entity e) const { return hay(e) ? e.script<Enemigo>() : nullptr; }

bool Heroe::objetivoValido() {
    Enemigo* s = enemigoDe(objetivo_);
    return s != nullptr && s->estaVivo();
}

std::vector<std::pair<Entity, float>> Heroe::vivos(float maximo) {
    std::vector<std::pair<Entity, float>> lista;
    const Vec3 yo = entity().position();
    for (const Entity& e : enemigos_) {
        Enemigo* s = enemigoDe(e);
        if (s != nullptr && s->estaVivo()) {
            const float d = Vec3::distance(yo, e.position());
            if (d <= maximo) lista.emplace_back(e, d);
        }
    }
    std::sort(lista.begin(), lista.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    return lista;
}

// Tab: el siguiente mas cercano (va rotando entre los de alrededor).
void Heroe::siguienteObjetivo() {
    const auto lista = vivos(35.0f);
    if (lista.empty()) {
        objetivo_ = Entity{};
        return;
    }
    std::size_t siguiente = 0;
    for (std::size_t i = 0; i < lista.size(); ++i) {
        if (lista[i].first == objetivo_) siguiente = (i + 1) % lista.size();
    }
    objetivo_ = lista[siguiente].first;
}

// Clic: el enemigo mas cerca del centro de la camara.
Entity Heroe::apuntado() {
    if (!hay(camara_)) return {};
    const Vec3 origen = camara_.position();
    const Vec3 dir = camara_.forward();
    Entity mejor;
    float mejorCos = 0.965f;
    for (const Entity& e : enemigos_) {
        Enemigo* s = enemigoDe(e);
        if (s != nullptr && s->estaVivo()) {
            const Vec3 v = e.position() - origen;
            const float d = v.length();
            if (d > 0.5f && d < 50.0f) {
                const float c = Vec3::dot(v, dir) / d;
                if (c > mejorCos) {
                    mejor = e;
                    mejorCos = c;
                }
            }
        }
    }
    return mejor;
}

void Heroe::objetivoCercano() {
    const auto lista = vivos(8.0f);
    if (!lista.empty()) objetivo_ = lista.front().first;
}

// =========================================================================
// Combate
// =========================================================================

bool Heroe::usarHabilidad(int i) {
    if (i < 1 || i > kNumHabilidades || muerto_) return false;
    const Habilidad& h = kHabilidades[i - 1];
    if (h.nivel > 0 && nivel_ < h.nivel) {
        aviso(std::string(h.nombre) + ": necesitas nivel " + std::to_string(h.nivel));
        return false;
    }
    if (enfriamientos_[i] > 0.0f) {
        aviso(std::string(h.nombre) + " no esta lista");
        return false;
    }
    if (h.item != nullptr) {
        if (cuenta(h.item) <= 0) {
            aviso(std::string("No te quedan: ") + itemDe(h.item)->nombre);
            return false;
        }
        usarObjeto(h.item);
        enfriamientos_[i] = h.cd;
        return true;
    }
    if (gcd_ > 0.0f || hayLanzamiento_) return false;
    if (h.mana > mana_) {
        aviso("No tienes mana suficiente");
        return false;
    }
    if (h.objetivo) {
        if (!objetivoValido()) objetivoCercano();
        if (!objetivoValido()) {
            aviso("No tienes objetivo (Tab)");
            return false;
        }
        if (Vec3::distance(entity().position(), objetivo_.position()) > h.alcance) {
            aviso("Demasiado lejos");
            return false;
        }
    }
    gcd_ = kGcd;
    if (h.lanzar > 0.0f) {
        hayLanzamiento_ = true;
        lanzando_ = {i, 0.0f, h.lanzar};
        return true;
    }
    aplicarHabilidad(i);
    return true;
}

// La barra de lanzamiento: moverse la interrumpe.
void Heroe::lanzar(float dt) {
    if (!hayLanzamiento_) return;
    if (moviendose_) {
        hayLanzamiento_ = false;
        aviso("Interrumpido");
        return;
    }
    lanzando_.t += dt;
    if (lanzando_.t >= lanzando_.dur) {
        hayLanzamiento_ = false;
        const Habilidad& h = kHabilidades[lanzando_.i - 1];
        if (h.objetivo && (!objetivoValido() || Vec3::distance(entity().position(), objetivo_.position()) > h.alcance + 2.0f)) {
            aviso("El objetivo ya no esta al alcance");
            return;
        }
        aplicarHabilidad(lanzando_.i);
    }
}

namespace {
struct Golpe {
    int dano;
    bool critico;
};
Golpe golpe(float base) {
    const bool critico = Random::chance(0.12).truthy();
    float d = base * azar(0.9f, 1.1f);
    if (critico) d *= 1.8f;
    return {redondear(d), critico};
}
}  // namespace

void Heroe::danar(Entity enemigo, int cantidad, bool critico, const std::string& que) {
    Enemigo* s = enemigoDe(enemigo);
    if (s == nullptr || !s->estaVivo()) return;
    const std::string quien = s->nombre();
    const int hecho = s->danar(static_cast<float>(cantidad), entity(), true);
    combate_ = 6.0f;
    flotante((critico ? "¡" : "") + std::to_string(hecho) + (critico ? "!" : ""), critico ? kCritico : kDano);
    chat(fmt("[Combate] %s a %s: %d%s", que.c_str(), quien.c_str(), hecho, critico ? " (critico)" : ""));
}

void Heroe::aplicarHabilidad(int i) {
    const Habilidad& h = kHabilidades[i - 1];
    mana_ -= h.mana;
    if (h.cd > 0.0f) enfriamientos_[i] = h.cd;
    if (i == 1) {
        const Golpe g = golpe(static_cast<float>(ataque_ + 5));
        danar(objetivo_, g.dano, g.critico, "Golpe");
        autoataque_ = kAutoataque;
    } else if (i == 2) {
        const Golpe g = golpe(poder_ * 1.4f + 8.0f);
        danar(objetivo_, g.dano, g.critico, "Bola de fuego");
    } else if (i == 3) {
        const int cura = poder_ * 2 + 20;
        vida_ = std::min(static_cast<float>(vidaMax_), vida_ + cura);
        flotante("+" + std::to_string(cura), kCura);
        chat("[Combate] Curar: +" + std::to_string(cura) + " de vida.");
    } else if (i == 4) {
        int tocados = 0;
        for (const auto& [e, d] : vivos(h.alcance)) {
            const Golpe g = golpe(ataque_ * 0.9f + 6.0f);
            danar(e, g.dano, g.critico, "Torbellino");
            ++tocados;
        }
        if (tocados == 0) chat("[Combate] Torbellino: no habia nadie cerca.");
    }
}

// Con un objetivo al lado y en combate, se golpea solo cada 2 s.
void Heroe::atacarSolo(float dt) {
    autoataque_ = std::max(0.0f, autoataque_ - dt);
    if (hayLanzamiento_ || autoataque_ > 0.0f || combate_ <= 0.0f || !objetivoValido()) return;
    if (Vec3::distance(entity().position(), objetivo_.position()) > kHabilidades[0].alcance) return;
    autoataque_ = kAutoataque;
    const Golpe g = golpe(ataque_ * 0.7f);
    danar(objetivo_, g.dano, g.critico, "Ataque");
}

// Lo llaman los enemigos al golpear.
void Heroe::recibirDano(float cantidad, Entity, const std::string& quien) {
    if (muerto_) return;
)CPP"
R"CPP(    const int d = std::max(1, static_cast<int>(std::floor(cantidad - defensa_ * 0.5f + 0.5f)));
    vida_ -= static_cast<float>(d);
    combate_ = 6.0f;
    flotante("-" + std::to_string(d), kRecibido);
    chat(fmt("[Combate] %s te golpea: %d", quien.empty() ? "Algo" : quien.c_str(), d));
    if (hayLanzamiento_ && Random::chance(0.25).truthy()) {
        hayLanzamiento_ = false;
        aviso("¡Te han interrumpido!");
    }
    if (vida_ <= 0.0f) morir(quien);
}

void Heroe::morir(const std::string& causa) {
    vida_ = 0.0f;
    muerto_ = true;
    hayLanzamiento_ = false;
    objetivo_ = Entity{};
    entity().setVelocity(Vec3{});
    panel("");
    activar(ui_.dialogo, false);
    if (hay(ui_.muerte)) {
        ui_.muerte.setActive(true);
        texto(hijo(ui_.muerte, "Causa"), "Te ha matado: " + (causa.empty() ? std::string("algo") : causa));
    }
    chat("[Sistema] Has muerto. Reaparecer te lleva al pueblo.");
}

void Heroe::reaparecerEn(float fraccion) {
    const Vec3 p = hay(spawn_) ? spawn_.position() : Vec3{0.0f, 2.0f, 6.0f};
    entity().setPosition(p);
    entity().setVelocity(Vec3{});
    vida_ = std::max(1.0f, std::floor(vidaMax_ * fraccion));
    mana_ = std::floor(manaMax_ * fraccion);
    muerto_ = false;
    combate_ = 0.0f;
    activar(ui_.muerte, false);
}

// Un enemigo murio (Enemigo.cpp): experiencia, misiones, oro y botin.
void Heroe::enemigoMuerto(Enemigo& enemigo, bool credito, const std::vector<std::string>& botin, int oro) {
    if (enemigo.entity() == objetivo_) objetivo_ = Entity{};
    if (!credito) return;
    const int diferencia = nivel_ - enemigo.nivelReal();
    const float factor = Mathf::clamp(1.0f - 0.15f * diferencia, 0.2f, 1.5f);
    ganarXp(enemigo.xp() * factor);
    chat("[Combate] " + enemigo.nombre() + " ha muerto.");
    for (const Mision& m : kMisiones) {
        auto it = misiones_.find(m.id);
        if (it == misiones_.end()) continue;
        EstadoMision& estado = it->second;
        if (estado.estado == "activa" && std::string(m.tipo) == "matar" && enemigo.tipo.get() == m.objetivo) {
            estado.progreso = std::min(m.cantidad, estado.progreso + 1);
            if (estado.progreso >= m.cantidad) {
                estado.estado = "lista";
                aviso(std::string("Mision completada: ") + m.nombre, kDorado);
                chat(std::string("[Mision] ") + m.nombre + ": completada. Vuelve con " + m.npc + ".");
            } else {
                chat(fmt("[Mision] %s: %d/%d", m.nombre, estado.progreso, m.cantidad));
            }
        }
    }
    // Bolsa de botin en el suelo (se recoge al pasar por encima).
    if ((!botin.empty() || oro > 0) && hay(plantillaBotin_)) {
        const Entity bolsa = Scene::instantiate(plantillaBotin_, enemigo.entity().position() + Vec3{0.0f, -0.6f, 0.0f}).asEntity();
        if (hay(bolsa)) {
            bolsa.setActive(true);
            botines_.push_back({bolsa, botin, oro, 90.0f});
        }
    }
}

void Heroe::recogerBotin(float dt) {
    const Vec3 yo = entity().position();
    for (std::size_t i = botines_.size(); i-- > 0;) {
        BolsaBotin& b = botines_[i];
        b.vida -= dt;
        const bool existe = b.e.valid();
        if (existe) b.e.rotate(Vec3{0.0f, 90.0f * dt, 0.0f});
        if (existe && Vec3::distance(yo, b.e.position()) < 2.4f) {
            if (b.oro > 0) {
                oro_ += b.oro;
                chat("[Botin] Recoges " + std::to_string(b.oro) + " de oro.");
                b.oro = 0;
            }
            std::vector<std::string> quedan;
            for (const std::string& id : b.items) {
                if (dar(id, 1) > 0) quedan.push_back(id);
            }
            b.items = quedan;
            if (!quedan.empty()) aviso("Inventario lleno");
        }
        if (b.vida <= 0.0f || (b.items.empty() && b.oro == 0)) {
            if (existe) b.e.destroy();
            botines_.erase(botines_.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
}

// =========================================================================
// Inventario y equipo
// =========================================================================

int Heroe::cuenta(const std::string& id) const {
    int n = 0;
    for (const Ranura& r : inventario_) {
        if (r.id == id) n += r.n;
    }
    return n;
}

// Da `n` objetos; devuelve los que no caben.
int Heroe::dar(const std::string& id, int n, bool callado) {
    const Item* item = itemDe(id);
    if (item == nullptr) return n;
    const std::string tipo = item->tipo;
    const bool apila = tipo != "arma" && tipo != "armadura";
    int quedan = n;
    if (apila) {
        for (Ranura& r : inventario_) {
            if (quedan <= 0) break;
            if (r.id == id && r.n < kPila) {
                const int pon = std::min(kPila - r.n, quedan);
                r.n += pon;
                quedan -= pon;
            }
        }
    }
    while (quedan > 0 && static_cast<int>(inventario_.size()) < kRanuras) {
        const int pon = apila ? std::min(kPila, quedan) : 1;
        inventario_.push_back({id, pon});
        quedan -= pon;
    }
    const int dados = n - quedan;
    if (dados > 0 && !callado) chat(std::string("[Botin] Recibes ") + item->nombre + (dados > 1 ? " x" + std::to_string(dados) : "") + ".");
    revisarRecoger();
    pintarPaneles();
    return quedan;
}

void Heroe::quitar(const std::string& id, int n) {
    for (std::size_t i = inventario_.size(); i-- > 0;) {
        if (n <= 0) break;
        Ranura& r = inventario_[i];
        if (r.id == id) {
            const int q = std::min(r.n, n);
            r.n -= q;
            n -= q;
            if (r.n <= 0) inventario_.erase(inventario_.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
    revisarRecoger();
    pintarPaneles();
}

bool Heroe::usarObjeto(const std::string& id) {
    const Item* item = itemDe(id);
    if (item == nullptr || cuenta(id) <= 0) return false;
    const std::string tipo = item->tipo;
    if (tipo == "consumible") {
        quitar(id, 1);
        if (item->cura > 0) {
            vida_ = std::min(static_cast<float>(vidaMax_), vida_ + item->cura);
            flotante("+" + std::to_string(item->cura), kCura);
        }
        if (item->mana > 0) {
            mana_ = std::min(static_cast<float>(manaMax_), mana_ + item->mana);
            flotante("+" + std::to_string(item->mana), kMana);
        }
        chat(std::string("[Sistema] Usas ") + item->nombre + ".");
        return true;
    }
    if (tipo == "arma" || tipo == "armadura") return equipar(id);
    return false;
}

bool Heroe::equipar(const std::string& id) {
    const Item* item = itemDe(id);
    if (item->nivel > 0 && nivel_ < item->nivel) {
        aviso(std::string(item->nombre) + ": necesitas nivel " + std::to_string(item->nivel));
        return false;
    }
    std::string& hueco = std::string(item->tipo) == "arma" ? arma_ : armadura_;
    quitar(id, 1);
    const std::string anterior = hueco;
    hueco = id;
    if (!anterior.empty()) dar(anterior, 1, true);
    recalcular();
    chat(std::string("[Sistema] Te equipas: ") + item->nombre + ".");
    pintarPaneles();
    return true;
}

void Heroe::desequipar(Entity boton) {
    std::string& hueco = boton.name() == "Arma" ? arma_ : armadura_;
    if (hueco.empty()) return;
    if (static_cast<int>(inventario_.size()) >= kRanuras) {
        aviso("Inventario lleno");
        return;
    }
    const std::string id = hueco;
    hueco.clear();
    dar(id, 1, true);
    recalcular();
}

// Clic en una ranura: usar/equipar, o vender si la tienda esta abierta.
void Heroe::pulsarRanura(Entity boton) {
    const int i = numeroDe(boton.name());
    if (i < 1 || i > static_cast<int>(inventario_.size())) return;
    const std::string id = inventario_[static_cast<std::size_t>(i - 1)].id;
    const Item* item = itemDe(id);
    if (vendiendo_) {
        const int precio = std::max(1, item->precio / 2);
        quitar(id, 1);
        oro_ += precio;
        chat(fmt("[Tienda] Vendes %s por %d de oro.", item->nombre, precio));
        pintarDialogoTienda();
        return;
    }
    usarObjeto(id);
}

void Heroe::comprar(const std::string& id) {
    const Item* item = itemDe(id);
    if (oro_ < item->precio) {
        aviso("No tienes oro suficiente");
        return;
    }
    if (dar(id, 1, true) > 0) {
        aviso("Inventario lleno");
        return;
    }
    oro_ -= item->precio;
    chat(fmt("[Tienda] Compras %s por %d de oro.", item->nombre, item->precio));
    pintarDialogoTienda();
}

// =========================================================================
// Misiones y dialogos
// =========================================================================

bool Heroe::disponible(const Mision& m) const {
    if (misiones_.count(m.id) != 0) return false;
    if (nivel_ < m.nivel) return false;
    if (m.requiere != 0) {
        auto r = misiones_.find(m.requiere);
        if (r == misiones_.end() || r->second.estado != "hecha") return false;
    }
    return true;
}

bool Heroe::aceptarMision(int id) {
    const Mision* m = misionDe(id);
    if (m == nullptr || !disponible(*m)) return false;
    misiones_[id] = {"activa", 0};
    revisarRecoger();
    aviso(std::string("Nueva mision: ") + m->nombre, kDorado);
    chat(std::string("[Mision] Aceptada: ") + m->nombre + ".");
    guardar();
    return true;
}

// Misiones de recoger: el progreso es lo que llevas en el inventario.
void Heroe::revisarRecoger() {
    for (const Mision& m : kMisiones) {
        auto it = misiones_.find(m.id);
        if (it == misiones_.end() || std::string(m.tipo) != "recoger") continue;
        EstadoMision& e = it->second;
        if (e.estado != "activa" && e.estado != "lista") continue;
        const std::string antes = e.estado;
        e.progreso = std::min(m.cantidad, cuenta(m.objetivo));
        e.estado = e.progreso >= m.cantidad ? "lista" : "activa";
        if (antes == "activa" && e.estado == "lista") {
            aviso(std::string("Mision completada: ") + m.nombre, kDorado);
            chat(std::string("[Mision] ") + m.nombre + ": completada. Vuelve con " + m.npc + ".");
        }
    }
}

bool Heroe::entregarMision(int id) {
    const Mision* m = misionDe(id);
    auto it = misiones_.find(id);
    if (m == nullptr || it == misiones_.end() || it->second.estado != "lista") return false;
    if (std::string(m->tipo) == "recoger") quitar(m->objetivo, m->cantidad);
    misiones_[id].estado = "hecha";
    oro_ += m->oro;
    chat(fmt("[Mision] %s entregada: %d de oro.", m->nombre, m->oro));
    if (m->premio != nullptr) dar(m->premio, m->premioCantidad);
    aviso(std::string("Mision entregada: ") + m->nombre, Vec3{0.5f, 1.0f, 0.5f});
    ganarXp(static_cast<float>(m->xp));
    return true;
}

Entity Heroe::npcCercano(float maximo) {
    Entity mejor;
    float dist = maximo;
    const Vec3 yo = entity().position();
    for (const Entity& n : npcs_) {
        const float d = Vec3::distance(yo, n.position());
        if (d < dist) {
            mejor = n;
            dist = d;
        }
    }
    return mejor;
}

void Heroe::hablar() {
    const Entity npc = npcCercano(4.5f);
    if (!hay(npc)) {
        aviso("No hay nadie cerca con quien hablar");
        return;
    }
    abrirDialogo(npc);
}

void Heroe::dialogo(const std::string& titulo, const std::string& cuerpo, std::vector<Opcion> opciones) {
    const Entity d = ui_.dialogo;
    if (!hay(d)) return;
    panel("");
    d.setActive(true);
    texto(hijo(d, "Titulo"), titulo);
    texto(hijo(d, "Cuerpo"), cuerpo);
    opciones_ = std::move(opciones);
    for (std::size_t i = 0; i < ui_.opciones.size(); ++i) {
        const Entity& b = ui_.opciones[i];
        const bool hayOpcion = i < opciones_.size();
        activar(b, hayOpcion);
        if (hayOpcion) texto(hijo(b, "Texto"), opciones_[i].texto);
    }
}

void Heroe::abrirDialogo(Entity npc) {
    NPC* s = npc.script<NPC>();
    if (s == nullptr) return;
    npcHablando_ = npc;
)CPP"
R"CPP(    vendiendo_ = false;
    const std::string quien = s->nombre;
    std::vector<Opcion> opciones;
    std::string cuerpo = s->saludo;
    for (const Mision& m : kMisiones) {
        if (quien != m.npc) continue;
        auto it = misiones_.find(m.id);
        const Mision* mp = &m;
        if (it != misiones_.end() && it->second.estado == "lista") {
            opciones.push_back({std::string("Entregar: ") + m.nombre, [this, npc, mp]() { ofrecerEntrega(npc, *mp); }});
        } else if (disponible(m)) {
            opciones.push_back({std::string("Mision: ") + m.nombre, [this, npc, mp]() { ofrecerMision(npc, *mp); }});
        } else if (it != misiones_.end() && it->second.estado == "activa") {
            cuerpo += fmt("\n\n(%s: %d/%d)", m.nombre, it->second.progreso, m.cantidad);
        }
    }
    if (s->comerciante) {
        opciones.push_back({"Comerciar", [this, npc]() {
                                tiendaNpc_ = npc;
                                pintarDialogoTienda();
                            }});
    }
    opciones.push_back({"Adios", [this]() { cerrarDialogo(); }});
    dialogo(quien, cuerpo, std::move(opciones));
}

std::string Heroe::recompensa(const Mision& m) const {
    std::string t = fmt("Recompensa: %d XP, %d de oro", m.xp, m.oro);
    if (m.premio != nullptr) {
        t += std::string(", ") + itemDe(m.premio)->nombre + (m.premioCantidad > 1 ? " x" + std::to_string(m.premioCantidad) : "");
    }
    return t;
}

void Heroe::ofrecerMision(Entity npc, const Mision& m) {
    const Mision* mp = &m;
    dialogo(m.nombre, std::string(m.texto) + "\n\n" + recompensa(m),
            {{"Aceptar",
              [this, npc, mp]() {
                  aceptarMision(mp->id);
                  abrirDialogo(npc);
              }},
             {"Ahora no", [this, npc]() { abrirDialogo(npc); }}});
}

void Heroe::ofrecerEntrega(Entity npc, const Mision& m) {
    const Mision* mp = &m;
    dialogo(m.nombre, std::string(m.fin) + "\n\n" + recompensa(m),
            {{"Entregar",
              [this, npc, mp]() {
                  entregarMision(mp->id);
                  abrirDialogo(npc);
              }},
             {"Volver", [this, npc]() { abrirDialogo(npc); }}});
}

void Heroe::pintarDialogoTienda() {
    NPC* s = hay(tiendaNpc_) ? tiendaNpc_.script<NPC>() : nullptr;
    if (s == nullptr) return;
    std::vector<Opcion> opciones;
    for (const char* id : kTienda) {
        const Item* it = itemDe(id);
        if (opciones.size() < 5) {
            const std::string nivel = it->nivel > 0 ? " · Nv " + std::to_string(it->nivel) : "";
            const std::string compra = id;
            opciones.push_back({fmt("%s  (%d oro%s)", it->nombre, it->precio, nivel.c_str()), [this, compra]() { comprar(compra); }});
        }
    }
    opciones.push_back({"Adios", [this]() { cerrarDialogo(); }});
    dialogo(s->nombre.get() + " · Tienda",
            "Tu oro: " + std::to_string(oro_) +
                "\nClic en un objeto para comprarlo.\nClic en tu inventario (a la izquierda) para vender a mitad de precio.",
            std::move(opciones));
    // El inventario a la vez, para vender.
    activar(ui_.inventario, true);
    vendiendo_ = true;
    pintarPaneles();
}

void Heroe::cerrarDialogo() {
    activar(ui_.dialogo, false);
    if (vendiendo_) activar(ui_.inventario, false);
    vendiendo_ = false;
    tiendaNpc_ = Entity{};
    npcHablando_ = Entity{};
}

void Heroe::pulsarCerrar(Entity boton) {
    const Entity p = boton.parent();
    if (!hay(p)) return;
    p.setActive(false);
    if (p == ui_.dialogo) cerrarDialogo();
    if (p == ui_.inventario) vendiendo_ = false;
}

void Heroe::alejarseDelDialogo() {
    if (hay(npcHablando_) && Vec3::distance(entity().position(), npcHablando_.position()) > 7.0f) cerrarDialogo();
}

// La marca de cada NPC: ! (mision nueva) o ? (para entregar).
void Heroe::marcasNpc() {
    for (const Entity& npc : npcs_) {
        NPC* s = npc.script<NPC>();
        if (s == nullptr) continue;
        const std::string quien = s->nombre;
        std::string marca;
        for (const Mision& m : kMisiones) {
            if (quien != m.npc) continue;
            auto it = misiones_.find(m.id);
            if (it != misiones_.end() && it->second.estado == "lista") {
                marca = "entregar";
            } else if (marca.empty() && disponible(m)) {
                marca = "mision";
            }
        }
        s->marca(marca);
    }
}

// =========================================================================
// Chat, avisos y textos flotantes
// =========================================================================

void Heroe::chat(const std::string& linea) {
    lineas_.push_back(linea);
    while (lineas_.size() > 9) lineas_.erase(lineas_.begin());
    texto(ui_.chat, unir(lineas_, "\n"));
}

void Heroe::aviso(const std::string& linea, const Vec3& c) {
    const Entity a = ui_.aviso;
    if (!hay(a)) return;
    a.set("text", linea);
    a.set("color", c);
    a.setAlpha(1.0f);
    avisoTiempo_ = 2.5f;
}

void Heroe::animarAviso(float dt) {
    if (!hay(ui_.aviso)) return;
    const float antes = avisoTiempo_;
    avisoTiempo_ = std::max(0.0f, avisoTiempo_ - dt);
    if (antes > 0.0f) ui_.aviso.setAlpha(Mathf::clamp01(avisoTiempo_ / 0.8f));
}

void Heroe::flotante(const std::string& linea, const Vec3& c) {
    const std::vector<Entity>& lista = ui_.flotantes;
    if (lista.empty()) return;
    siguienteFlotante_ = siguienteFlotante_ % static_cast<int>(lista.size()) + 1;
    const Entity& t = lista[static_cast<std::size_t>(siguienteFlotante_ - 1)];
    if (!hay(t)) return;
    t.set("text", linea);
    t.set("color", c);
    t.setAlpha(1.0f);
    t.setUiPosition(Vec3{azar(-90, 90), azar(-170, -120), 0.0f});
    flotantes_[siguienteFlotante_] = 1.2f;
}

void Heroe::animarFlotantes(float dt) {
    const std::vector<Entity>& lista = ui_.flotantes;
    for (std::size_t i = 0; i < lista.size(); ++i) {
        const Entity& t = lista[i];
        auto it = flotantes_.find(static_cast<int>(i) + 1);
        if (!hay(t) || it == flotantes_.end()) continue;
        float& vida = it->second;
        if (vida > 0.0f) {
            vida -= dt;
            t.setUiPosition(t.uiPosition().asVec3() + Vec3{0.0f, -70.0f * dt, 0.0f});
            t.setAlpha(Mathf::clamp01(vida / 0.6f));
        } else {
            t.setAlpha(0.0f);
            flotantes_.erase(it);
        }
    }
}

// =========================================================================
// Interfaz
// =========================================================================

void Heroe::pintarTodo() {
    pintarHud();
    pintarPaneles();
}

void Heroe::pintarHud() {
    const Interfaz& u = ui_;
    if (hay(u.jugador)) {
        texto(hijo(u.jugador, "Nombre"), fmt("%s  ·  Nivel %d", nombre.get().c_str(), nivel_));
        barra(hijo(u.jugador, "VidaBarra"), vida_ / vidaMax_, 320);
        barra(hijo(u.jugador, "ManaBarra"), mana_ / manaMax_, 320);
        texto(hijo(u.jugador, "VidaTexto"), fmt("%d / %d", redondear(vida_), vidaMax_));
        texto(hijo(u.jugador, "ManaTexto"), fmt("%d / %d", redondear(mana_), manaMax_));
    }
    if (hay(u.objetivo)) {
        const bool valido = objetivoValido();
        u.objetivo.setActive(valido);
        if (valido) {
            Enemigo* s = enemigoDe(objetivo_);
            const int vida = s->vida();
            const int maximo = s->vidaMax();
            const Entity n = hijo(u.objetivo, "Nombre");
            texto(n, fmt("%s  ·  Nv %d", s->nombre().c_str(), s->nivelReal()));
            color(n, s->esJefe() ? Vec3{1.0f, 0.55f, 0.2f} : Vec3{1.0f, 0.85f, 0.85f});
            barra(hijo(u.objetivo, "VidaBarra"), static_cast<float>(vida) / maximo, 320);
            texto(hijo(u.objetivo, "VidaTexto"),
                  fmt("%d / %d  ·  %.0f m", vida, maximo, Vec3::distance(entity().position(), objetivo_.position())));
        }
    }
    if (hay(u.xpBarra)) {
        const int necesita = xpNivel(nivel_);
        barra(u.xpBarra, nivel_ >= kNivelMax ? 1.0f : static_cast<float>(xp_) / necesita, 900);
        texto(u.xpTexto, nivel_ >= kNivelMax ? "Nivel " + std::to_string(nivel_) + " (maximo)"
                                             : fmt("Nivel %d  ·  %d / %d XP", nivel_, xp_, necesita));
    }
    if (hay(u.lanzamiento)) {
        u.lanzamiento.setActive(hayLanzamiento_);
        if (hayLanzamiento_) {
            barra(hijo(u.lanzamiento, "Barra"), lanzando_.t / lanzando_.dur, 400);
            texto(hijo(u.lanzamiento, "Texto"), kHabilidades[lanzando_.i - 1].nombre);
        }
    }
    pintarHabilidades();
    pintarSeguimiento();
    pintarMapa();
}

void Heroe::pintarHabilidades() {
    for (std::size_t k = 0; k < ui_.habilidades.size(); ++k) {
        const Entity& b = ui_.habilidades[k];
        if (!hay(b)) continue;
        const int i = static_cast<int>(k) + 1;
        const Habilidad& h = kHabilidades[k];
        float cd = enfriamientos_[i];
        float total = h.cd;
        if (h.item == nullptr && gcd_ > cd) {
            cd = gcd_;
            total = kGcd;
        }
        const bool bloqueada = h.nivel > 0 && nivel_ < h.nivel;
        const Entity sombra = hijo(b, "Enfriamiento");
        float frac = total > 0.0f ? cd / total : 0.0f;
        if (bloqueada) frac = 1.0f;
        if (hay(sombra)) {
            const Vec3 tam = sombra.uiSize().asVec3();
            sombra.setUiSize(Vec3{tam.x, 72.0f * Mathf::clamp01(frac), 0.0f});
        }
        texto(hijo(b, "Tiempo"), bloqueada ? "Nv " + std::to_string(h.nivel)
                                           : (cd > 0.05f && total > kGcd ? std::to_string(static_cast<int>(std::ceil(cd))) : ""));
        const Entity n = hijo(b, "Nombre");
        if (h.item != nullptr) {
            std::string corto = h.nombre;
            const std::string quita = "Pocion de ";
            if (corto.rfind(quita, 0) == 0) corto = corto.substr(quita.size());
            texto(n, corto + " x" + std::to_string(cuenta(h.item)));
        } else {
            texto(n, h.nombre);
        }
        color(n, h.mana > mana_ ? Vec3{0.55f, 0.6f, 1.0f} : kBlanco);
    }
}

void Heroe::pintarSeguimiento() {
    if (!hay(ui_.seguimiento)) return;
    std::vector<std::string> lineas{"Misiones (L)"};
    for (const Mision& m : kMisiones) {
        auto it = misiones_.find(m.id);
        if (it == misiones_.end() || it->second.estado == "hecha") continue;
        if (it->second.estado == "lista") {
            lineas.push_back(std::string("· ") + m.nombre + ": ¡lista! (" + m.npc + ")");
        } else {
            lineas.push_back(fmt("· %s: %d/%d", m.nombre, it->second.progreso, m.cantidad));
        }
    }
    if (lineas.size() == 1) lineas.push_back("· Habla con los ! del pueblo");
    texto(ui_.seguimiento, unir(lineas, "\n"));
}

// Minimapa: 60 m alrededor, norte arriba. Rojo enemigos, verde otros
// jugadores, amarillo NPC, dorado botin.
void Heroe::pintarMapa() {
    const Interfaz& u = ui_;
    if (!hay(u.mapa)) return;
    const Vec3 p = entity().position();
    const float escala = 110.0f / 60.0f;
    std::size_t usados = 0;
    const auto punto = [&](const Vec3& pos, const Vec3& c, float tam) {
        const float dx = (pos.x - p.x) * escala;
        const float dz = (pos.z - p.z) * escala;
        if (dx * dx + dz * dz > 105.0f * 105.0f || usados >= u.puntos.size()) return;
        const Entity& d = u.puntos[usados++];
        if (!hay(d)) return;
        d.setActive(true);
        d.setUiPosition(Vec3{dx, dz, 0.0f});
        d.setUiSize(Vec3{tam, tam, 0.0f});
        d.set("color", c);
    };
    for (const Entity& n : npcs_) punto(n.position(), Vec3{1.0f, 0.85f, 0.2f}, 9);
    for (const Entity& b : bots_) {
        const Bot* s = b.script<Bot>();
        if (s != nullptr && s->estaVivo()) punto(b.position(), Vec3{0.35f, 1.0f, 0.45f}, 8);
    }
    for (const Entity& e : enemigos_) {
        Enemigo* s = enemigoDe(e);
        if (s != nullptr && s->estaVivo()) {
)CPP"
R"CPP(            const bool jefe = s->esJefe();
            punto(e.position(), jefe ? Vec3{1.0f, 0.4f, 0.05f} : Vec3{1.0f, 0.2f, 0.2f}, jefe ? 12 : 7);
        }
    }
    for (const BolsaBotin& b : botines_) {
        if (b.e.valid()) punto(b.e.position(), Vec3{1.0f, 0.75f, 0.2f}, 6);
    }
    for (std::size_t i = usados; i < u.puntos.size(); ++i) activar(u.puntos[i], false);
    if (hay(u.zona)) {
        std::string nombreZona = "Tierras salvajes";
        for (const Zona& z : zonas_) {
            if ((z.entidad.position() - p).length() < z.radio) nombreZona = z.nombre;
        }
        texto(u.zona, nombreZona);
    }
}

void Heroe::pintarPaneles() {
    const Interfaz& u = ui_;
    if (!iniciado_) return;
    if (activo(u.inventario)) {
        texto(hijo(u.inventario, "Oro"), "Oro: " + std::to_string(oro_));
        texto(hijo(u.inventario, "Pista"), vendiendo_ ? "Clic: vender (mitad de precio)" : "Clic: usar o equipar");
        for (std::size_t i = 0; i < u.ranuras.size(); ++i) {
            const Entity& b = u.ranuras[i];
            if (!hay(b)) continue;
            const Entity n = hijo(b, "Nombre");
            const Entity cantidad = hijo(b, "Cantidad");
            const Entity c = hijo(b, "Color");
            if (i < inventario_.size()) {
                const Ranura& r = inventario_[i];
                const Item* it = itemDe(r.id);
                texto(n, it->nombre);
                texto(cantidad, r.n > 1 ? "x" + std::to_string(r.n) : "");
                if (hay(c)) {
                    c.setAlpha(1.0f);
                    c.set("color", it->color);
                }
            } else {
                texto(n, "");
                texto(cantidad, "");
                if (hay(c)) c.setAlpha(0.0f);
            }
        }
    }
    if (activo(u.personaje)) {
        const int necesita = xpNivel(nivel_);
        texto(hijo(u.personaje, "Stats"),
              fmt("%s\nNivel %d  (%d / %d XP)\n\nVida  %d / %d\nMana  %d / %d\nAtaque  %d\nPoder magico  %d\nDefensa  %d\nOro  %d",
                  nombre.get().c_str(), nivel_, xp_, necesita, static_cast<int>(std::floor(vida_)), vidaMax_,
                  static_cast<int>(std::floor(mana_)), manaMax_, ataque_, poder_, defensa_, oro_));
        const Item* arma = itemDe(arma_);
        const Item* armadura = itemDe(armadura_);
        texto(hijo(hijo(u.personaje, "Arma"), "Texto"),
              "Arma: " + (arma != nullptr ? fmt("%s (+%d ataque)", arma->nombre, arma->ataque) : std::string("(nada)")));
        texto(hijo(hijo(u.personaje, "Armadura"), "Texto"),
              "Armadura: " + (armadura != nullptr ? fmt("%s (+%d defensa)", armadura->nombre, armadura->defensa) : std::string("(nada)")));
    }
    if (activo(u.diario)) {
        std::vector<std::string> lineas;
        for (const Mision& m : kMisiones) {
            auto it = misiones_.find(m.id);
            std::string estado;
            if (it == misiones_.end()) {
                estado = disponible(m) ? std::string("disponible: habla con ") + m.npc
                                       : "nivel " + std::to_string(m.nivel) + (m.requiere != 0 ? ", tras otra mision" : "");
            } else if (it->second.estado == "activa") {
                estado = fmt("en curso %d/%d", it->second.progreso, m.cantidad);
            } else if (it->second.estado == "lista") {
                estado = std::string("¡lista! Vuelve con ") + m.npc;
            } else {
                estado = "completada";
            }
            lineas.push_back(std::string(m.nombre) + "  —  " + estado);
            if (it != misiones_.end() && it->second.estado != "hecha") lineas.push_back(std::string("    ") + m.texto);
        }
        texto(hijo(u.diario, "Texto"), unir(lineas, "\n"));
    }
}

// =========================================================================
// Guardar y cargar (Prefs)
// =========================================================================

void Heroe::guardar() {
    if (noGuardar_ || !iniciado_) return;
    std::vector<std::string> inv;
    for (const Ranura& r : inventario_) inv.push_back(r.id + ":" + std::to_string(r.n));
    std::vector<std::string> mis;
    for (const auto& [id, m] : misiones_) mis.push_back(std::to_string(id) + ":" + m.estado + ":" + std::to_string(m.progreso));
    const std::vector<std::string> datos{"1",
                                         std::to_string(nivel_),
                                         std::to_string(xp_),
                                         std::to_string(oro_),
                                         unir(inv, ","),
                                         arma_.empty() ? "-" : arma_,
                                         armadura_.empty() ? "-" : armadura_,
                                         unir(mis, ",")};
    Prefs::setString("mmo_partida", unir(datos, "|"));
}

bool Heroe::cargar() {
    const std::vector<std::string> d = dividir(Prefs::getString("mmo_partida", "").asString(), '|');
    if (d.size() < 8 || d[0] != "1") return false;
    nivel_ = Mathf::clamp(std::atoi(d[1].c_str()), 1, kNivelMax);
    xp_ = std::atoi(d[2].c_str());
    oro_ = std::atoi(d[3].c_str());
    inventario_.clear();
    for (const std::string& par : dividir(d[4], ',')) {
        const std::vector<std::string> p = dividir(par, ':');
        if (p.size() == 2 && itemDe(p[0]) != nullptr) inventario_.push_back({p[0], std::atoi(p[1].c_str())});
    }
    arma_ = itemDe(d[5]) != nullptr ? d[5] : "";
    armadura_ = itemDe(d[6]) != nullptr ? d[6] : "";
    for (const std::string& trozo : dividir(d[7], ',')) {
        const std::vector<std::string> p = dividir(trozo, ':');
        if (p.size() == 3) misiones_[std::atoi(p[0].c_str())] = {p[1], std::atoi(p[2].c_str())};
    }
    return true;
}

CRAMION_SCRIPT(Heroe)
)CPP"},
    {"NPC.cpp",
R"CPP(// NPC.cpp: personaje del pueblo (ver NPC.h).
#include "NPC.h"

#include <cmath>

void NPC::start() {
    jugador_ = Scene::find("Jugador");
    modelo_ = entity().find("Modelo").asEntity();
    marca_ = entity().find("Marca").asEntity();
    fase_ = entity().position().x;
}

void NPC::marca(const std::string& tipo) {
    if (marca_.id() == 0 || tipo == estadoMarca_) return;
    estadoMarca_ = tipo;
    marcaActiva_ = !tipo.empty();
    marca_.setActive(marcaActiva_);
    if (tipo == "mision") {
        marca_.setMaterial(0, "Materials/Marca mision");
    } else if (tipo == "entregar") {
        marca_.setMaterial(0, "Materials/Marca entregar");
    }
}

void NPC::update(float dt) {
    if (marca_.id() != 0 && marcaActiva_) {
        marca_.rotate(Vec3{0.0f, 90.0f * dt, 0.0f});
        marca_.setLocalPosition(Vec3{0.0f, 1.75f + std::sin(Time::time() * 2.2f + fase_) * 0.12f, 0.0f});
    }
    if (jugador_.id() != 0 && modelo_.id() != 0 && Vec3::distance(entity().position(), jugador_.position()) < 8.0f) {
        const Vec3 p = jugador_.position();
        const Vec3 yo = modelo_.position();
        modelo_.lookAt(Vec3{p.x, yo.y, p.z});
    }
}

CRAMION_SCRIPT(NPC)
)CPP"},
};

}  // namespace cramion::editor::mmo

#endif  // CRAMION_EDITOR_TEMPLATE_MMO_SCRIPTS_H

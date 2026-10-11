#ifndef CRAMION_EDITOR_TEMPLATE_ONLINE_SCRIPTS_H
#define CRAMION_EDITOR_TEMPLATE_ONLINE_SCRIPTS_H

// Scripts de C++ de la plantilla "Online" (ProjectTemplates.cpp): Red.cpp lleva
// el juego (menu, chat, marcador, objetos y eventos), JugadorRed.cpp el
// personaje de cada jugador y MonedaRed.cpp las monedas.

#include "TemplateScripts.h"

namespace cramion::editor::online {

inline constexpr TemplateFile kFiles[] = {
    {"JugadorRed.cpp",
R"CPP(// JugadorRed.cpp: el personaje de un jugador. Existe en todos los ordenadores
// (lo crea el servidor con Network::spawn), pero solo lo mueve su dueno
// (entity().isMine()). Los demas lo ven moverse con la posicion que llega por
// la red (el motor la suaviza y pone su Rigidbody cinematico).
#include "CamaraTercera.h"

#include <string>

class JugadorRed : public Script {
public:
    Property<float> velocidad{this, "velocidad", 6.0f};
    Property<float> correr{this, "correr", 1.7f};
    Property<float> salto{this, "salto", 6.5f};
    Property<float> aceleracion{this, "aceleracion", 12.0f};

    void start() override {
        static const char* colores[] = {"Azul", "Rojo", "Verde", "Amarillo", "Morado", "Naranja", "Cian", "Rosa"};
        modelo_ = entity().find("Modelo").asEntity();
        ultima_ = entity().position();
        // Color por jugador (el mismo en todos: sale de su id).
        const int n = ((entity().netOwner().asInt() - 1) % 8 + 8) % 8;
        if (const Entity cuerpo = entity().find("Cuerpo").asEntity()) cuerpo.setMaterial(0, std::string("Materials/Jugador ") + colores[n]);
        if (entity().isMine().asBool()) {
            inicio_ = entity().position();
            camara_ = Scene::find("Main Camera");
            if (CamaraTercera* cam = camara_.script<CamaraTercera>()) cam->setTarget(entity());
        }
    }

    void update(float dt) override {
        if (!entity().isMine().asBool()) {
            // De otro jugador: mirar hacia donde se mueve.
            const Vec3 p = entity().position();
            Vec3 d = p - ultima_;
            d.y = 0;
            if (modelo_ && d.length() > 0.01f) modelo_.lookAt(modelo_.position() + d);
            ultima_ = p;
            return;
        }
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
        Vec3 v = entity().velocity();
        const float t = Mathf::clamp01(aceleracion * dt);
        v.x = Mathf::lerp(v.x, direccion.x * rapidez, t);
        v.z = Mathf::lerp(v.z, direccion.z * rapidez, t);
        if (Input::keyDown("space") && enSuelo()) v.y = salto;
        entity().setVelocity(v);
        if (modelo_ && direccion.length() > 0.1f) modelo_.lookAt(modelo_.position() + direccion);
        if (entity().position().y < -20.0f) {
            entity().setPosition(inicio_);
            entity().setVelocity(Vec3{});
        }
    }

private:
    bool enSuelo() const {
        RaycastHit hit;
        return Physics::raycast(entity().position() + Vec3{0, -1.02f, 0}, Vec3{0, -1, 0}, 0.25f, &hit);
    }

    Entity modelo_, camara_;
    Vec3 ultima_, inicio_;
};

CRAMION_SCRIPT(JugadorRed)
)CPP"},
    {"MonedaRed.cpp",
R"CPP(// MonedaRed.cpp: solo se ve girar (cada uno la gira en su pantalla). Quien la
// recoge lo decide el servidor en Red.cpp (distancia a cada jugador).
#include <cramion/Script.h>

#include <cmath>

using namespace cramion;

class MonedaRed : public Script {
public:
    void start() override {
        base_ = entity().position();
        fase_ = Random::value().asFloat() * 6.28f;
    }

    void update(float dt) override {
        fase_ += dt * 2.5f;
        entity().rotate(Vec3{0, 160.0f * dt, 0});
        entity().setPosition(base_ + Vec3{0, std::sin(fase_) * 0.15f, 0});
    }

private:
    Vec3 base_;
    float fase_ = 0.0f;
};

CRAMION_SCRIPT(MonedaRed)
)CPP"},
    {"Red.cpp",
R"CPP(// Red.cpp: el juego online. Todo pasa por aqui:
//   Menu        nombre, IP, "Crear partida" (eres el servidor y juegas) o "Unirse".
//   Jugadores   cada uno tiene su personaje (Prefabs/Jugador) creado por el
//               servidor con Network::spawn: lo mueve su dueno y los demas lo
//               ven sincronizado.
//   Chat        el cliente lo manda al servidor ("decir"), el servidor le pone
//               el nombre y lo reparte a todos ("chat").
//   Marcador    puntos y ping de cada jugador; gana la ronda quien llega antes.
//   Objetos     monedas, un balon y cajas del servidor (su fisica la simula el
//               servidor); F patea lo que tengas cerca. Goles en las porterias.
//   Eventos     cada cierto tiempo: lluvia de monedas o monedas dobles.
// El servidor decide los puntos (un cliente no puede darse puntos a si mismo).
#include "CamaraTercera.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace {

const char* const kColores[] = {"Azul", "Rojo", "Verde", "Amarillo", "Morado", "Naranja", "Cian", "Rosa"};
constexpr std::size_t kLineasChat = 9;
constexpr float kMitadArena = 24.0f;  // la arena va de -24 a 24 (m)
constexpr float kPorteriaX = 26.0f;   // el balon cruza esta linea = gol
constexpr float kPorteriaZ = 4.0f;    // ancho de la porteria (a cada lado)
constexpr int kServidor = 1;          // Network::SERVER

std::string colorDe(int id) { return kColores[((id - 1) % 8 + 8) % 8]; }

// Texto de un jugador: sin saltos de linea, sin espacios en los extremos y con un largo maximo.
std::string limpiar(const std::string& entrada, std::size_t largo) {
    std::string texto = entrada;
    for (char& c : texto) {
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    }
    const std::size_t a = texto.find_first_not_of(' ');
    if (a == std::string::npos) return {};
    texto = texto.substr(a, texto.find_last_not_of(' ') - a + 1);
    if (texto.size() > largo) texto.resize(largo);
    return texto;
}

Vec3 puntoAlAzar(float margen = 3.0f) {
    const float m = kMitadArena - margen;
    return Vec3{Random::range(-m, m).asFloat(), 1.2f, Random::range(-m, m).asFloat()};
}

}  // namespace

class Red : public Script {
public:
    Property<int> puerto{this, "puerto", 7777};
    Property<int> maxJugadores{this, "maxJugadores", 8};
    Property<int> puntosParaGanar{this, "puntosParaGanar", 30};
    Property<int> monedas{this, "monedas", 8, Tooltip("Monedas a la vez en la arena")};
    Property<float> segundosEvento{this, "segundosEvento", 45.0f};

    void start() override {
        menu_ = Scene::find("UI_Menu");
        estado_ = Scene::find("UI_Estado");
        campoNombre_ = Scene::find("UI_Nombre");
        campoIP_ = Scene::find("UI_IP");
        juego_ = Scene::find("UI_Juego");
        chat_ = Scene::find("UI_Chat");
        campoChat_ = Scene::find("UI_EscribirChat");
        marcador_ = Scene::find("UI_Marcador");
        aviso_ = Scene::find("UI_Aviso");
        info_ = Scene::find("UI_Info");
        camara_ = Scene::find("Main Camera");
        if (camara_) inicioCamara_ = camara_.position();
        campoNombre_.set("text", Prefs::getString("online_nombre", "Jugador" + std::to_string(Random::int_(10, 99).asInt())));
        campoIP_.set("text", Prefs::getString("online_ip", "127.0.0.1"));
        mostrarMenu(true, "Crea una partida o unete a la de un amigo (su IP; en el mismo PC, 127.0.0.1).");
        escuchar();
    }

    // Recarga en caliente: los callbacks de la red son del proceso anterior.
    void onAfterReload(const Value&) override { escuchar(); }

    void update(float dt) override {
        if (avisoTiempo_ > 0.0f) {
            avisoTiempo_ -= dt;
            aviso_.set("alpha", Mathf::clamp01(avisoTiempo_));
        }
        if (!enPartida_) return;
        if (Input::keyDown("f")) {
            if (Network::isServer().asBool()) patear(kServidor);
            else Network::send("patear", Value::object(), "server");
        }
        if (Network::isServer().asBool() && servidor_) actualizarServidor(dt);

        infoTiempo_ -= dt;
        if (infoTiempo_ <= 0.0f) {
            infoTiempo_ = 0.5f;
            std::string estado;
            if (Network::isServer().asBool()) {
                estado = "Servidor (puerto " + std::to_string(puerto.get()) + ") - " + std::to_string(Network::playerCount().asInt()) +
                         " jugador(es)";
            } else {
                estado = "Jugador " + std::to_string(Network::myId().asInt()) + " - ping " + std::to_string(Network::ping().asInt()) + " ms";
            }
            const Value s = Network::stats();
            char linea[96];
            std::snprintf(linea, sizeof(linea), "  -  %d objetos  -  %.1f KB enviados", s["objects"].asInt(), s["sent"].asNumber() / 1024.0);
            info_.set("text", estado + linea + "\nWASD mover - Shift correr - Espacio saltar - F patear - clic en el chat para escribir");
        }
    }

private:
    struct Jugador {
        std::string nombre;
        int puntos = 0;
        Entity avatar;
    };
    struct Objeto {
        Entity entidad;
        float fuerza = 0.0f;
        bool balon = false;
        int ultimo = 0;  // quien lo pateo por ultima vez (0 = nadie)
    };

    void escuchar() {
        on("OnCrear", [this](const Value&) { crear(); });
        on("OnUnirse", [this](const Value&) { unirse(); });
        on("OnSalir", [this](const Value&) {
            Network::disconnect();
            alSalir("Has salido de la partida.");
        });
        // El campo de texto del chat (Enter).
        on("OnChat", [this](const Value& texto) { escribirChat(texto.asString()); });

        // Eventos de la red (valen para toda la partida).
        Network::onConnected([this](const Values& a) { alEntrar(a.empty() ? 0 : a[0].asInt()); });
        Network::onDisconnected([this](const Values& a) { alSalir(a.empty() ? std::string() : a[0].asString()); });
        Network::onPlayerJoined([this](const Values& a) { jugadorEntra(a.empty() ? 0 : a[0].asInt()); });
        Network::onPlayerLeft([this](const Values& a) { jugadorSale(a.empty() ? 0 : a[0].asInt()); });
        // Mensajes: "hola", "decir" y "patear" van al servidor; "chat", "tabla" y "aviso" a todos.
        // Cada uno llega con (datos, de).
        Network::on("hola", [this](const Values& a) { hola(arg(a, 1).asInt(), arg(a, 0)); });
        Network::on("decir", [this](const Values& a) { difundirChat(arg(a, 1).asInt(), arg(a, 0)["texto"].asString()); });
        Network::on("patear", [this](const Values& a) { patear(arg(a, 1).asInt()); });
        Network::on("chat", [this](const Values& a) { anadirLinea(arg(a, 0)["texto"].asString()); });
        Network::on("tabla", [this](const Values& a) {
            tabla_ = arg(a, 0);
            pintarMarcador();
        });
        Network::on("aviso", [this](const Values& a) { mostrarAviso(arg(a, 0)["texto"].asString()); });
    }

    static Value arg(const Values& a, std::size_t i) { return i < a.size() ? a[i] : Value(); }

    // --- Menu ---

    void mostrarMenu(bool visible, const std::string& texto = {}) {
        menu_.setActive(visible);
        juego_.setActive(!visible);
        if (!texto.empty()) estado_.set("text", texto);
        Input::lockCursor(false);
    }

    std::string nombre() const { return limpiar(campoNombre_.text().asString(), 16); }

    void crear() {
        if (Network::isActive().asBool()) return;
        Prefs::setString("online_nombre", nombre());
        const Value r = Network::host(puerto.get(), maxJugadores.get());
        if (!r[0].asBool()) {
            estado_.set("text", "No se pudo crear la partida: " + r[1].asString());
            return;
        }
        // El servidor tambien juega: es el jugador 1.
        jugadores_.clear();
        monedasVivas_.clear();
        objetos_.clear();
        servidor_ = true;
        eventoTiempo_ = segundosEvento;
        multiplicador_ = 1;
        multiplicadorTiempo_ = 0.0f;
        tablaTiempo_ = 0.0f;
        jugadores_[kServidor] = Jugador{nombre(), 0, {}};
        crearAvatar(kServidor);
        crearObjetos();
        empezar();
        sistema(nombre() + " ha creado la partida (puerto " + std::to_string(puerto.get()) + ").");
        enviarTabla();
    }

    void unirse() {
        if (Network::isActive().asBool()) return;
        std::string ip = limpiar(campoIP_.text().asString(), 64);
        if (ip.empty()) ip = "127.0.0.1";
        Prefs::setString("online_nombre", nombre());
        Prefs::setString("online_ip", ip);
        const Value r = Network::connect(ip, puerto.get());
        estado_.set("text", r[0].asBool() ? "Conectando con " + ip + "..." : "No se pudo conectar: " + r[1].asString());
    }

    // Cliente: ya dentro. Se presenta al servidor con su nombre.
    void alEntrar(int id) {
        empezar();
        Network::send("hola", Value{{"nombre", nombre()}}, "server");
        anadirLinea("Conectado como jugador " + std::to_string(id) + " (" + colorDe(id) + ").");
    }

    void empezar() {
        enPartida_ = true;
        lineas_.clear();
        chat_.set("text", "");
        mostrarMenu(false);
    }

    void alSalir(const std::string& motivo) {
        enPartida_ = false;
        tabla_ = Value::array();
        servidor_ = false;
        jugadores_.clear();
        if (camara_) {
            if (CamaraTercera* cam = camara_.script<CamaraTercera>()) cam->setTarget(Entity());
            if (inicioCamara_) camara_.setPosition(*inicioCamara_);
        }
        mostrarMenu(true, motivo);
    }

    // --- Servidor: jugadores ---

    void crearAvatar(int id) {
        const Entity avatar = Network::spawn("Prefabs/Jugador", puntoAlAzar(6.0f), id).asEntity();
        if (avatar) jugadores_[id].avatar = avatar;
    }

    void jugadorEntra(int id) {
        if (!Network::isServer().asBool() || !servidor_) return;
        jugadores_[id] = Jugador{"Jugador " + std::to_string(id), 0, {}};
        crearAvatar(id);
        enviarTabla();
    }

    void hola(int de, const Value& datos) {
        if (!Network::isServer().asBool() || !servidor_ || !jugadores_.count(de)) return;
        const std::string n = limpiar(datos["nombre"].asString(), 16);
        if (!n.empty()) jugadores_[de].nombre = n;
        sistema(jugadores_[de].nombre + " se ha unido (" + colorDe(de) + ").");
        enviarTabla();
    }

    void jugadorSale(int id) {
        if (!Network::isServer().asBool() || !servidor_ || !jugadores_.count(id)) return;
        // Su personaje desaparece solo (sus objetos de red se borran al irse).
        sistema(jugadores_[id].nombre + " ha salido.");
        jugadores_.erase(id);
        enviarTabla();
    }

    // --- Chat ---

    void escribirChat(const std::string& entrada) {
        campoChat_.set("text", "");
        const std::string texto = limpiar(entrada, 120);
        if (texto.empty() || !enPartida_) return;
        if (Network::isServer().asBool()) difundirChat(kServidor, texto);
        else Network::send("decir", Value{{"texto", texto}}, "server");
    }

    // Servidor: pone el nombre y lo manda a todos.
    void difundirChat(int id, const std::string& entrada) {
        if (!Network::isServer().asBool() || !servidor_ || !jugadores_.count(id)) return;
        const std::string texto = limpiar(entrada, 120);
        if (texto.empty()) return;
        const std::string linea = jugadores_[id].nombre + ": " + texto;
        Network::send("chat", Value{{"texto", linea}});
        anadirLinea(linea);
    }

    // Servidor: mensaje del sistema en el chat de todos.
    void sistema(const std::string& texto) {
        Network::send("chat", Value{{"texto", "* " + texto}});
        anadirLinea("* " + texto);
    }

    void anadirLinea(const std::string& texto) {
        lineas_.push_back(texto);
        while (lineas_.size() > kLineasChat) lineas_.pop_front();
        std::string todo;
)CPP"
R"CPP(        for (const std::string& l : lineas_) todo += (todo.empty() ? "" : "\n") + l;
        chat_.set("text", todo);
    }

    // --- Avisos y marcador ---

    void anunciar(const std::string& texto) {
        Network::send("aviso", Value{{"texto", texto}});
        mostrarAviso(texto);
    }

    void mostrarAviso(const std::string& texto) {
        aviso_.set("text", texto);
        aviso_.set("alpha", 1);
        avisoTiempo_ = 4.0f;
    }

    void enviarTabla() {
        if (!servidor_) return;
        std::vector<std::pair<int, const Jugador*>> orden;
        for (const auto& [id, j] : jugadores_) orden.emplace_back(id, &j);
        std::stable_sort(orden.begin(), orden.end(), [](const auto& a, const auto& b) { return a.second->puntos > b.second->puntos; });
        Value lista = Value::array();
        for (const auto& [id, j] : orden) {
            lista.push(Value{{"id", id}, {"nombre", j->nombre}, {"puntos", j->puntos}, {"ping", Network::ping(id)}});
        }
        Network::send("tabla", lista);
        tabla_ = lista;
        pintarMarcador();
    }

    void pintarMarcador() {
        std::string texto = "JUGADORES   (a " + std::to_string(puntosParaGanar.get()) + " puntos)";
        const int yo = Network::myId().asInt();
        for (const Value& j : tabla_.items()) {
            const int id = j["id"].asInt();
            const std::string ping = id == kServidor ? "servidor" : std::to_string(j["ping"].asInt()) + " ms";
            texto += "\n" + colorDe(id) + "  " + j["nombre"].asString() + "  " + std::to_string(j["puntos"].asInt()) + " pts  (" + ping + ")" +
                     (id == yo ? "  < tu" : "");
        }
        marcador_.set("text", texto);
    }

    // --- Servidor: objetos, puntos y eventos ---

    void crearObjetos() {
        // Balon y cajas: los simula el servidor y los ven todos.
        if (const Entity balon = Network::spawn("Prefabs/Balon", Vec3{0, 2, 0}).asEntity()) objetos_.push_back(Objeto{balon, 9.0f, true});
        for (int i = 1; i <= 4; ++i) {
            const Entity caja = Network::spawn("Prefabs/Caja", Vec3{-12.0f + i * 5.0f, 1, -10}).asEntity();
            if (caja) objetos_.push_back(Objeto{caja, 45.0f, false});
        }
        for (int i = 0; i < monedas; ++i) crearMoneda();
    }

    void crearMoneda() {
        const Entity moneda = Network::spawn("Prefabs/Moneda", puntoAlAzar(3.0f)).asEntity();
        if (moneda) monedasVivas_.push_back(moneda);
    }

    void sumar(int id, int puntos) {
        const auto it = jugadores_.find(id);
        if (it == jugadores_.end()) return;
        Jugador& j = it->second;
        j.puntos += puntos * multiplicador_;
        if (j.puntos >= puntosParaGanar) {
            anunciar(j.nombre + " gana la ronda!");
            sistema(j.nombre + " gana la ronda con " + std::to_string(j.puntos) + " puntos. Nueva ronda!");
            for (auto& [otro_id, otro] : jugadores_) otro.puntos = 0;
        }
        enviarTabla();
    }

    // F: patea lo que el jugador tenga cerca (el servidor aplica el golpe).
    void patear(int id) {
        if (!Network::isServer().asBool() || !servidor_) return;
        const auto it = jugadores_.find(id);
        if (it == jugadores_.end() || !it->second.avatar) return;
        const Entity avatar = it->second.avatar;
        for (Objeto& o : objetos_) {
            if (o.entidad.distanceTo(avatar).asFloat() >= 2.4f) continue;
            Vec3 dir = o.entidad.position() - avatar.position();
            dir.y = 0;
            dir = dir.normalized();
            dir.y = 0.45f;
            o.entidad.addForce(dir * o.fuerza, ForceMode::Impulse);
            o.ultimo = id;
        }
    }

    void actualizarServidor(float dt) {
        // Monedas: se recogen al pasar cerca (lo comprueba el servidor).
        for (int i = static_cast<int>(monedasVivas_.size()) - 1; i >= 0; --i) {
            const Entity moneda = monedasVivas_[i];
            for (const auto& [id, j] : jugadores_) {
                if (j.avatar && moneda.distanceTo(j.avatar).asFloat() < 1.4f) {
                    Network::destroy(moneda);
                    monedasVivas_.erase(monedasVivas_.begin() + i);
                    sumar(id, 1);
                    break;
                }
            }
        }
        while (static_cast<int>(monedasVivas_.size()) < monedas) crearMoneda();

        // Goles: el balon cruza una porteria.
        for (Objeto& o : objetos_) {
            const Vec3 p = o.entidad.position();
            if (o.balon) {
                if (std::abs(p.x) > kPorteriaX && std::abs(p.z) < kPorteriaZ) {
                    const auto quien = jugadores_.find(o.ultimo);
                    if (o.ultimo != 0 && quien != jugadores_.end()) {
                        anunciar("GOL de " + quien->second.nombre + "! +5");
                        sumar(o.ultimo, 5);
                    } else {
                        anunciar("Gol!");
                    }
                    o.entidad.setPosition(Vec3{0, 3, 0});
                    o.entidad.setVelocity(Vec3{});
                    o.ultimo = 0;
                } else if (p.y < -10.0f) {
                    o.entidad.setPosition(Vec3{0, 3, 0});
                    o.entidad.setVelocity(Vec3{});
                }
            } else if (p.y < -10.0f) {
                o.entidad.setPosition(puntoAlAzar(5.0f) + Vec3{0, 2, 0});
                o.entidad.setVelocity(Vec3{});
            }
        }

        // Eventos cada cierto tiempo.
        eventoTiempo_ -= dt;
        if (eventoTiempo_ <= 0.0f) {
            eventoTiempo_ = segundosEvento;
            if (Random::value().asFloat() < 0.5f) {
                anunciar("Lluvia de monedas!");
                for (int i = 0; i < 12; ++i) crearMoneda();
            } else {
                anunciar("Monedas dobles durante 20 segundos!");
                multiplicador_ = 2;
                multiplicadorTiempo_ = 20.0f;
            }
        }
        if (multiplicadorTiempo_ > 0.0f) {
            multiplicadorTiempo_ -= dt;
            if (multiplicadorTiempo_ <= 0.0f) {
                multiplicador_ = 1;
                anunciar("Se acabaron las monedas dobles");
            }
        }

        // El ping cambia: el marcador se refresca cada 2 segundos.
        tablaTiempo_ -= dt;
        if (tablaTiempo_ <= 0.0f) {
            tablaTiempo_ = 2.0f;
            enviarTabla();
        }
    }

    Entity menu_, estado_, campoNombre_, campoIP_, juego_, chat_, campoChat_, marcador_, aviso_, info_, camara_;
    std::optional<Vec3> inicioCamara_;
    std::deque<std::string> lineas_;
    Value tabla_ = Value::array();
    float avisoTiempo_ = 0.0f;
    float infoTiempo_ = 0.0f;
    bool enPartida_ = false;
    // Solo en el servidor.
    bool servidor_ = false;
    std::map<int, Jugador> jugadores_;
    std::vector<Entity> monedasVivas_;
    std::vector<Objeto> objetos_;
    float eventoTiempo_ = 0.0f;
    int multiplicador_ = 1;
    float multiplicadorTiempo_ = 0.0f;
    float tablaTiempo_ = 0.0f;
};

CRAMION_SCRIPT(Red)
)CPP"},
};

}  // namespace cramion::editor::online

#endif  // CRAMION_EDITOR_TEMPLATE_ONLINE_SCRIPTS_H

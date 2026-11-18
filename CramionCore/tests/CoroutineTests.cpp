// Pruebas de las corrutinas y los temporizadores de los scripts de C++
// (sdk/cramion/Coroutine.h y su planificador en ScriptMain.cpp), sin motor ni
// proceso de scripts: se compila el SDK aqui y se simulan los frames (el
// evento Time que manda el motor tras los update). La API no responde (sin
// motor): Debug::error no hace nada. Devuelve 0 si todo va.

#include <cramion/Script.h>

#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace cramion;

// Las exportaciones de ScriptMain.cpp (compilado en esta prueba).
extern "C" int cramion_call(void* instance, std::uint32_t event, const cppproto::EventArgs* args, char* error, int error_size);
extern "C" void cramion_destroy(void* instance);
extern "C" int cramion_class_count();
extern "C" const char* cramion_class_name(int i);
extern "C" int cramion_class_order(int i);
extern "C" std::uint32_t cramion_class_events(int i);

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const std::string& what) {
    ++checks;
    std::printf("  %s %s\n", condition ? "OK   " : "FALLO", what.c_str());
    if (!condition) ++failures;
}

// El reloj del "motor": cada frame, sus update y despues el evento Time.
std::uint64_t g_frame = 0;
cppproto::EventArgs frameArgs(float dt) {
    cppproto::EventArgs a{};
    a.delta_time = dt;
    a.frame = g_frame;
    a.time = static_cast<float>(g_frame) * dt;
    return a;
}
void send(Script* s, cppproto::Event e, float dt = 1.0f / 60.0f) {
    const cppproto::EventArgs a = frameArgs(dt);
    char error[256] = {};
    cramion_call(s, static_cast<std::uint32_t>(e), &a, error, sizeof(error));
}
// Un frame: update de los scripts (los que se pasan) y el Time de despues.
void frame(const std::vector<Script*>& scripts, float dt = 1.0f / 60.0f) {
    ++g_frame;
    for (Script* s : scripts) send(s, cppproto::Event::Update, dt);
    if (!scripts.empty()) send(scripts.front(), cppproto::Event::Time, dt);
}

class Pasos : public Script {
public:
    std::vector<std::string> log;
    int contado = -1;
    Coroutine principal;
    void start() override { principal = startCoroutine(correr()); }
    Task<int> contar(int veces) {
        for (int i = 0; i < veces; ++i) co_await nextFrame();
        co_return veces * 10;
    }
    Task<> correr() {
        log.push_back("empieza");
        co_await nextFrame();
        log.push_back("frame");
        co_await waitFrames(2);
        log.push_back("dos frames");
        co_await waitSeconds(0.5f);
        log.push_back("medio segundo");
        contado = co_await contar(3);
        log.push_back("contado");
        co_await waitUntil([this] { return listo; });
        log.push_back("listo");
        co_await waitWhile([this] { return listo; });
        log.push_back("ya no");
    }
    bool listo = false;
};

class Lanzador : public Script {
public:
    int vueltas = 0;
    bool despues = false;
    void start() override {
        startCoroutine(fallar());
        startCoroutine(seguir());
    }
    Task<> fallar() {
        co_await nextFrame();
        throw std::runtime_error("roto");
    }
    Task<> seguir() {
        for (;;) {
            ++vueltas;
            co_await nextFrame();
        }
    }
};

class Temporizado : public Script {
public:
    int una = 0, repetida = 0;
    Timer t;
    void start() override {
        invoke([this] { ++una; }, 0.1f);
        t = invokeRepeating([this] { ++repetida; }, 0.0f, 0.05f);
    }
};

class Esperador : public Script {
public:
    std::vector<std::string> log;
    Task<> hija() {
        co_await waitFrames(3);
        log.push_back("hija");
    }
    Task<> madre() {
        Coroutine c = startCoroutine(hija());
        co_await c;
        log.push_back("madre");
    }
    void start() override { startCoroutine(madre()); }
};

class Parada : public Script {
public:
    int pasos = 0;
    Coroutine c;
    Task<> bucle() {
        for (;;) {
            ++pasos;
            if (pasos == 3) stopCoroutine(c);  // se para a si misma: sigue hasta la espera
            co_await nextFrame();
        }
    }
    void start() override { c = startCoroutine(bucle()); }
};

class Prioridad : public Script {
public:
    static constexpr int kExecutionOrder = -50;
};

class Real : public Script {
public:
    bool hecho = false;
    Task<> esperar() {
        co_await waitSecondsRealtime(0.05f);
        hecho = true;
    }
    void start() override { startCoroutine(esperar()); }
};

}  // namespace

CRAMION_SCRIPT(Pasos)
CRAMION_SCRIPT(Prioridad)

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const float dt = 1.0f / 60.0f;

    std::printf("Corrutinas (startCoroutine, esperas, Task<T>)\n");
    {
        auto* s = new Pasos();
        send(s, cppproto::Event::Start);
        check(s->log.size() == 1 && s->log[0] == "empieza", "startCoroutine corre ya hasta la primera espera");
        check(s->principal.running(), "Coroutine::running mientras espera");
        // El Time del mismo frame en que se puso a esperar no la reanuda.
        send(s, cppproto::Event::Time);
        check(s->log.size() == 1, "nextFrame: no sigue en el mismo frame");
        frame({s});
        check(s->log.size() == 2 && s->log[1] == "frame", "nextFrame: sigue en el siguiente");
        frame({s});
        check(s->log.size() == 2, "waitFrames(2): tras un frame aun espera");
        frame({s});
        check(s->log.size() == 3 && s->log[2] == "dos frames", "waitFrames(2): sigue a los dos");
        for (int i = 0; i < 20; ++i) frame({s}, 0.02f);  // 0,4 s
        check(s->log.size() == 3, "waitSeconds(0,5): a los 0,4 s aun espera");
        for (int i = 0; i < 6; ++i) frame({s}, 0.02f);  // 0,52 s
        check(s->log.size() == 4 && s->log[3] == "medio segundo", "waitSeconds(0,5): sigue al pasar 0,5 s");
        for (int i = 0; i < 3; ++i) frame({s});
        check(s->contado == 30 && s->log.size() == 5, "co_await de una Task<int>: espera sus frames y da su resultado");
        frame({s});
        frame({s});
        check(s->log.size() == 5, "waitUntil: espera mientras sea falso");
        s->listo = true;
        frame({s});
        check(s->log.size() == 6 && s->log[5] == "listo", "waitUntil: sigue al cumplirse");
        frame({s});
        check(s->log.size() == 6, "waitWhile: espera mientras sea verdad");
        s->listo = false;
        frame({s});
        check(s->log.size() == 7 && !s->principal.running(), "waitWhile: sigue y la corrutina termina");
        check(detail::coCount(s) == 0, "al terminar no queda nada en el planificador");
        cramion_destroy(s);
    }

    std::printf("Excepciones y parar\n");
    {
        auto* s = new Lanzador();
        send(s, cppproto::Event::Start);
        check(detail::coCount(s) == 2, "dos corrutinas en marcha");
        frame({s});
        check(detail::coCount(s) == 1, "la que lanza una excepcion se para (sale en la Consola)");
        frame({s});
        frame({s});
        check(s->vueltas == 4, "la otra sigue (" + std::to_string(s->vueltas) + " vueltas)");
        send(s, cppproto::Event::Disable);
        check(detail::coCount(s) == 0, "onDisable para las corrutinas del script");
        frame({s});
        check(s->vueltas == 4, "y ya no siguen");
        cramion_destroy(s);

        auto* p = new Parada();
        send(p, cppproto::Event::Start);
        for (int i = 0; i < 6; ++i) frame({p});
        check(p->pasos == 3 && !p->c.running(), "stopCoroutine sobre si misma: para en su siguiente espera");
        cramion_destroy(p);

        auto* q = new Pasos();
        send(q, cppproto::Event::Start);
        check(detail::coCount(q) == 1, "otra en marcha");
        cramion_destroy(q);  // destruir el script (onDestroy) con la corrutina a medias
        check(detail::coCount(nullptr) == 0, "destruir el script libera su corrutina (y la Task anidada)");
    }

    std::printf("co_await de otra corrutina\n");
    {
        auto* s = new Esperador();
        send(s, cppproto::Event::Start);
        for (int i = 0; i < 3; ++i) frame({s});
        check(s->log.size() == 1 && s->log[0] == "hija", "la hija termina a los 3 frames");
        frame({s});
        check(s->log.size() == 2 && s->log[1] == "madre", "la madre sigue cuando acaba la hija");
        cramion_destroy(s);
    }

    std::printf("invoke / invokeRepeating\n");
    {
        auto* s = new Temporizado();
        send(s, cppproto::Event::Start);
        check(s->isInvoking() && s->t.active(), "isInvoking y Timer::active");
        for (int i = 0; i < 3; ++i) frame({s}, 0.025f);  // 0,075 s
        check(s->una == 0, "invoke(fn, 0,1): a los 0,075 s aun no");
        for (int i = 0; i < 2; ++i) frame({s}, 0.025f);  // 0,125 s
        check(s->una == 1, "invoke(fn, 0,1) salta una vez pasados los 0,1 s");
        check(s->repetida >= 2 && s->repetida <= 3, "invokeRepeating(0, 0,05): al empezar y cada 0,05 s (" + std::to_string(s->repetida) +
                                    " veces en 0,125 s)");
        for (int i = 0; i < 10; ++i) frame({s}, 0.02f);
        check(s->una == 1, "invoke no se repite");
        const int antes = s->repetida;
        s->cancelInvoke(s->t);
        frame({s}, 0.1f);
        check(s->repetida == antes && !s->isInvoking(), "cancelInvoke lo para");
        s->invoke([s] { ++s->una; }, 1.0f);
        send(s, cppproto::Event::Disable);
        check(!s->isInvoking(), "onDisable cancela los invoke");
        cramion_destroy(s);
    }

    std::printf("Tiempo real\n");
    {
        auto* s = new Real();
        send(s, cppproto::Event::Start);
        frame({s}, 1.0f);  // un frame "largo" del juego no cuenta
        check(!s->hecho, "waitSecondsRealtime: no sigue por el tiempo del juego");
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        frame({s}, 0.0f);  // pausa: sin tiempo del juego
        check(s->hecho, "waitSecondsRealtime: sigue por el reloj real (tambien en pausa)");
        cramion_destroy(s);
    }

    std::printf("Prioridad de las clases (orden de ejecucion)\n");
    {
        int pasos = -1, prioridad = -1;
        for (int i = 0; i < cramion_class_count(); ++i) {
            const std::string name = cramion_class_name(i);
            if (name == "Pasos") pasos = cramion_class_order(i);
            if (name == "Prioridad") prioridad = cramion_class_order(i);
        }
        check(pasos == 0 && prioridad == -50, "kExecutionOrder de la clase llega al motor (cramion_class_order)");
        bool disable_always = true;
        for (int i = 0; i < cramion_class_count(); ++i) {
            disable_always = disable_always && ((cramion_class_events(i) >> static_cast<int>(cppproto::Event::Disable)) & 1u) != 0;
        }
        check(disable_always, "todas reciben onDisable (para parar sus corrutinas)");
    }

    std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

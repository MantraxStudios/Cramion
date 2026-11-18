// Cramion C++ scripting: corrutinas y temporizadores del juego (como
// StartCoroutine e Invoke de Unity), con corrutinas de C++20.
//
//   class Puerta : public Script {
//   public:
//       void start() override { startCoroutine(abrir()); }
//       Task<> abrir() {
//           co_await waitSeconds(1.0f);                       // tiempo del juego
//           for (int i = 0; i < 30; ++i) {
//               entity().translate(Vec3{0.0f, 0.1f, 0.0f});
//               co_await nextFrame();                          // el siguiente frame
//           }
//           co_await waitUntil([this] { return abierta(); });  // cada frame hasta que se cumpla
//           int n = co_await contar(3);                        // otra Task (con resultado)
//           co_await waitSecondsRealtime(0.5f);                // tiempo real (sin pausa ni escala)
//       }
//       Task<int> contar(int veces) { for (int i = 0; i < veces; ++i) co_await nextFrame(); co_return veces; }
//   };
//
// startCoroutine la empieza ya (corre hasta su primera espera) y devuelve un
// Coroutine: stopCoroutine(c), c.running() y co_await c (esperar a que acabe).
// Las corrutinas y los temporizadores de un script se paran solos en su
// onDisable (desactivar el script o su objeto) y en onDestroy.
//
//   invoke([this] { explotar(); }, 2.0f);                 // una vez, dentro de 2 s
//   Timer t = invokeRepeating([this] { disparar(); }, 0.0f, 0.5f);  // cada 0,5 s
//   cancelInvoke(t);  /  cancelInvoke();  /  isInvoking()
//
// Se reanudan una vez por frame, despues de todos los update() y antes de los
// lateUpdate() (como en Unity). Una espera puesta en un frame se mira desde el
// siguiente: nextFrame() siempre espera un frame. Una excepcion dentro de una
// corrutina sale en la Consola y para esa corrutina (el script sigue).
// Para salir antes: co_return (no return). La recarga en caliente no conserva
// las corrutinas: vuelve a lanzarlas en onAfterReload().
//
// Las pruebas automaticas (cramion/Test.h) tienen su propio Test::Task.
#pragma once

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

namespace cramion {

class Script;

namespace detail {

// Lo que espera ahora una corrutina (la guarda su raiz).
struct CoWait {
    enum class Kind : std::uint8_t { None, Frames, Seconds, Realtime, Until, While, Coroutine };
    Kind kind = Kind::None;
    int frames = 0;
    float seconds = 0.0f;                              // Seconds: lo que falta (tiempo del juego)
    std::chrono::steady_clock::time_point deadline{};  // Realtime
    std::function<bool()> condition;                   // Until / While
    std::uint64_t coroutine = 0;                       // Coroutine: a cual
};

// Lo comun de las promesas de las Task (la raiz la empieza startCoroutine).
struct CoPromiseBase {
    CoPromiseBase* root = this;       // la de la corrutina empezada con startCoroutine
    std::coroutine_handle<> leaf;     // (raiz) la corrutina mas interna, la que espera
    std::coroutine_handle<> parent;   // (anidada) quien la espera con co_await
    std::exception_ptr error;
    CoWait wait;                      // (raiz) lo que se espera
    void unhandled_exception() noexcept { error = std::current_exception(); }
};

template <class P>
concept CoPromise = std::is_base_of_v<CoPromiseBase, P>;

// El planificador (ScriptMain.cpp, uno por DLL de scripts).
std::uint64_t coStart(Script* owner, std::coroutine_handle<> handle, CoPromiseBase* promise);
void coStop(std::uint64_t id);
bool coRunning(std::uint64_t id);
// Para las corrutinas (coroutines) y/o los temporizadores (timers) de un script.
void coStopOwned(const Script* owner, bool coroutines, bool timers);
std::uint64_t timerStart(Script* owner, std::function<void()> fn, float delay, float interval);  // interval < 0: una vez
bool timerAny(const Script* owner);
// Vivas (corrutinas + temporizadores) de un script, o de todos con nullptr.
std::size_t coCount(const Script* owner);
// Un frame del juego: reanuda lo que toca (evento Time, tras los update).
void coTick();

// La espera de waitSeconds, nextFrame, waitUntil...
struct CoWaiter {
    CoWait wait;
    bool await_ready() {
        switch (wait.kind) {
            case CoWait::Kind::Frames: return wait.frames <= 0;
            case CoWait::Kind::Seconds: return !(wait.seconds > 0.0f);
            case CoWait::Kind::Realtime: return std::chrono::steady_clock::now() >= wait.deadline;
            case CoWait::Kind::Until: return !wait.condition || wait.condition();
            case CoWait::Kind::While: return !wait.condition || !wait.condition();
            case CoWait::Kind::Coroutine: return !coRunning(wait.coroutine);
            default: return true;
        }
    }
    template <CoPromise P>
    void await_suspend(std::coroutine_handle<P> h) {
        CoPromiseBase* root = h.promise().root;
        root->wait = std::move(wait);
        root->leaf = h;
    }
    void await_resume() noexcept {}
};

// El resultado de una Task (co_return valor; o nada con Task<>).
template <class T>
struct TaskResult {
    std::optional<T> value;
    template <class U>
    void return_value(U&& v) {
        value.emplace(std::forward<U>(v));
    }
    T take() { return std::move(*value); }
};
template <>
struct TaskResult<void> {
    void return_void() noexcept {}
    void take() {}
};

}  // namespace detail

/// Una corrutina del juego: startCoroutine(tarea()) o, desde otra, co_await tarea().
/// Task<int> devuelve un valor con co_return (int n = co_await contar(3)).
template <class T = void>
class [[nodiscard]] Task {
public:
    struct promise_type : detail::CoPromiseBase, detail::TaskResult<T> {
        Task get_return_object() noexcept { return Task(std::coroutine_handle<promise_type>::from_promise(*this)); }
        std::suspend_always initial_suspend() noexcept { return {}; }
        struct Final {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
                promise_type& p = h.promise();
                if (!p.parent) return std::noop_coroutine();
                p.root->leaf = p.parent;  // sigue quien la esperaba
                return p.parent;
            }
            void await_resume() noexcept {}
        };
        Final final_suspend() noexcept { return {}; }
    };
    using Handle = std::coroutine_handle<promise_type>;

    Task(Task&& o) noexcept : handle_(std::exchange(o.handle_, {})) {}
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    Task& operator=(Task&& o) noexcept {
        if (this != &o) {
            if (handle_) handle_.destroy();
            handle_ = std::exchange(o.handle_, {});
        }
        return *this;
    }
    ~Task() {
        if (handle_) handle_.destroy();
    }

    // co_await tarea: la empieza y sigue cuando termina (con su excepcion, si fallo).
    bool await_ready() const noexcept { return !handle_ || handle_.done(); }
    template <detail::CoPromise P>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<P> parent) noexcept {
        promise_type& p = handle_.promise();
        p.parent = parent;
        p.root = parent.promise().root;
        p.root->leaf = handle_;
        return handle_;
    }
    T await_resume() {
        if (handle_ && handle_.promise().error) std::rethrow_exception(handle_.promise().error);
        if constexpr (!std::is_void_v<T>) return handle_.promise().take();
    }

    // (interno) El planificador se queda con la corrutina.
    Handle release() noexcept { return std::exchange(handle_, {}); }

private:
    explicit Task(Handle h) : handle_(h) {}
    Handle handle_;
};

/// Una corrutina en marcha (lo que devuelve startCoroutine).
class Coroutine {
public:
    Coroutine() = default;
    explicit Coroutine(std::uint64_t id) : id_(id) {}
    std::uint64_t id() const { return id_; }
    /// Sigue en marcha (no ha terminado ni se paro).
    bool running() const { return id_ != 0 && detail::coRunning(id_); }
    explicit operator bool() const { return running(); }
    // co_await c: espera a que termine (yield return StartCoroutine(...) de Unity).
    bool await_ready() const { return !running(); }
    template <detail::CoPromise P>
    void await_suspend(std::coroutine_handle<P> h) const {
        detail::CoPromiseBase* root = h.promise().root;
        root->wait = {};
        root->wait.kind = detail::CoWait::Kind::Coroutine;
        root->wait.coroutine = id_;
        root->leaf = h;
    }
    void await_resume() const noexcept {}

private:
    std::uint64_t id_ = 0;
};

/// Un temporizador (lo que devuelven invoke e invokeRepeating).
class Timer {
public:
    Timer() = default;
    explicit Timer(std::uint64_t id) : id_(id) {}
    std::uint64_t id() const { return id_; }
    /// Sigue pendiente (no salto aun, o es repetido y no se cancelo).
    bool active() const { return id_ != 0 && detail::coRunning(id_); }
    explicit operator bool() const { return active(); }
    void cancel() const {
        if (id_ != 0) detail::coStop(id_);
    }

private:
    std::uint64_t id_ = 0;
};

// Las esperas (tambien como Script::waitSeconds... dentro de los scripts).
namespace co {
/// co_await waitSeconds(1.5f): segundos del juego (Time::deltaTime: se para con la pausa).
inline detail::CoWaiter waitSeconds(float seconds) {
    detail::CoWaiter w;
    w.wait.kind = detail::CoWait::Kind::Seconds;
    w.wait.seconds = seconds;
    return w;
}
/// co_await waitSecondsRealtime(1.5f): segundos reales (sigue con la pausa).
inline detail::CoWaiter waitSecondsRealtime(float seconds) {
    detail::CoWaiter w;
    w.wait.kind = detail::CoWait::Kind::Realtime;
    const auto span = std::chrono::duration<double>(seconds > 0.0f ? seconds : 0.0f);
    w.wait.deadline = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(span);
    return w;
}
/// co_await waitFrames(3): frames del juego.
inline detail::CoWaiter waitFrames(int frames) {
    detail::CoWaiter w;
    w.wait.kind = detail::CoWait::Kind::Frames;
    w.wait.frames = frames;
    return w;
}
/// co_await nextFrame(): el siguiente frame (yield return null de Unity).
inline detail::CoWaiter nextFrame() { return waitFrames(1); }
/// co_await waitUntil([&] { return listo; }): cada frame hasta que sea verdad.
template <class F>
detail::CoWaiter waitUntil(F condition) {
    detail::CoWaiter w;
    w.wait.kind = detail::CoWait::Kind::Until;
    w.wait.condition = [fn = std::move(condition)]() mutable { return static_cast<bool>(fn()); };
    return w;
}
/// co_await waitWhile([&] { return cargando; }): cada frame mientras sea verdad.
template <class F>
detail::CoWaiter waitWhile(F condition) {
    detail::CoWaiter w;
    w.wait.kind = detail::CoWait::Kind::While;
    w.wait.condition = [fn = std::move(condition)]() mutable { return static_cast<bool>(fn()); };
    return w;
}
}  // namespace co

}  // namespace cramion

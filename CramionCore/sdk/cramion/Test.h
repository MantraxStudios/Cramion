// Cramion C++ scripting: pruebas automaticas del juego, con corrutinas de C++20.
//
//   // Assets/Scripts/Pruebas.cpp
//   #include <cramion/Test.h>
//   using namespace cramion;
//
//   CRAMION_TEST(Suma) {
//       Assert::equal(1 + 1, 2, "la suma");
//   }
//   CRAMION_TEST(Salta) {
//       Entity jugador = Scene::find("Jugador");
//       Assert::notNull(jugador, "hay jugador");
//       const float antes = jugador.position().y;
//       jugador.jump(2.0f);
//       co_await Test::waitSeconds(0.3f);                  // tiempo del juego
//       Assert::greater(jugador.position().y, antes, "subio");
//       co_await Test::waitUntil([=] { return jugador.isGrounded(); }, 5.0f);
//       co_await Test::waitFrames(3);
//   }
//   CRAMION_TEST_TIMEOUT(Carrera, 120.0f) { ... }          // limite propio (por defecto 30 s)
//
// Pon el script CramionTests en un objeto de la escena: al empezar el juego
// corre todas las pruebas (CRAMION_TEST), una detras de otra, y cuenta el
// resultado al motor (Test.begin / check / finish / done de la API: la
// ventana de pruebas). Un Assert que falla termina esa prueba (las demas
// siguen); una excepcion o pasar del limite de tiempo, tambien.
//
// Una prueba que espera (co_await) es una corrutina: para salir antes usa
// co_return (no return). Las funciones de ayuda que esperan devuelven
// Test::Task y se esperan con co_await:
//   Test::Task andar(Entity e, float segundos) {
//       e.setMoveInput(Vec3(0, 0, 1));
//       co_await Test::waitSeconds(segundos);
//   }
//   CRAMION_TEST(Anda) { co_await andar(Scene::find("Jugador"), 1.0f); ... }
#pragma once

#include "Script.h"

#include <cmath>
#include <coroutine>
#include <exception>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// <windows.h> define near y far como nada: Assert::near no se podria declarar.
#pragma push_macro("near")
#undef near

namespace cramion {
namespace Test {

class Task;

namespace detail {

// Lo que lanza un Assert que falla: termina la prueba (el fallo ya se conto).
struct Failure : std::runtime_error {
    explicit Failure(const std::string& text) : std::runtime_error("Assert: " + text) {}
};

// Lo que espera la prueba ahora.
struct Wait {
    enum class Kind { None, Frames, Seconds, Until };
    Kind kind = Kind::None;
    int frames = 0;
    float seconds = 0.0f;          // Seconds: lo que falta; Until: el limite que queda
    std::function<bool()> until;   // Until: la condicion
    bool timed_out = false;        // Until: se paso del limite
    std::string message;           // Until: el mensaje si falla
};

// Lo comun de las promesas de las pruebas y de las Task de ayuda.
struct PromiseBase {
    PromiseBase* root = this;        // la de la prueba
    std::coroutine_handle<> leaf;    // (root) la corrutina mas interna, la que espera
    std::coroutine_handle<> parent;  // (Task) quien la espera
    std::exception_ptr error;
    Wait wait;                       // (root) lo que se espera
    void unhandled_exception() noexcept { error = std::current_exception(); }
};

template <class P>
concept TestPromise = std::is_base_of_v<PromiseBase, P>;

// Donde deja la prueba su corrutina al empezar (si espera).
struct Slot {
    std::coroutine_handle<> handle;
    PromiseBase* promise = nullptr;
};
// El parametro oculto de CRAMION_TEST (con el, una prueba con co_await es una corrutina).
struct Context {
    Slot* slot = nullptr;
};

struct RootPromise : PromiseBase {
    Slot* slot;
    explicit RootPromise(Context c) : slot(c.slot) {}
    void get_return_object() noexcept {
        const auto h = std::coroutine_handle<RootPromise>::from_promise(*this);
        leaf = h;
        if (slot != nullptr) {
            slot->handle = h;
            slot->promise = this;
        }
    }
    std::suspend_always initial_suspend() noexcept { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }
    void return_void() noexcept {}
};

// Las pruebas registradas (en el orden en que se declaran).
struct Case {
    const char* name;
    void (*fn)(Context);
    float timeout;
};
inline std::vector<Case>& cases() {
    static std::vector<Case> list;
    return list;
}
struct Registrar {
    Registrar(const char* name, void (*fn)(Context), float timeout) { cases().push_back({name, fn, timeout}); }
};

// Hay una prueba en marcha (los Assert cuentan al motor solo entonces).
inline bool& running() {
    static bool on = false;
    return on;
}

inline std::string number(double v) {
    std::ostringstream o;
    o << v;
    return o.str();
}

// Una comprobacion: al motor (Test.check) y, si falla, termina la prueba.
//   what: "Assert.isTrue" (el texto si pasa sin mensaje); why: por que fallo.
inline void check(bool ok, std::string_view what, const std::string& why, std::string_view message) {
    std::string text;
    if (ok) text = message.empty() ? std::string(what) : std::string(message);
    else text = message.empty() ? why : std::string(message) + " (" + why + ")";
    if (running()) Api::call("Test.check", {ok, text});
    if (!ok) throw Failure(text);
}

template <class T>
std::string describe(const T& v) {
    if constexpr (std::is_same_v<T, bool>) {
        return v ? "true" : "false";
    } else if constexpr (std::is_same_v<T, Value>) {
        return v.isNil() ? "nil" : (v.isString() ? "\"" + v.asString() + "\"" : v.asString());
    } else if constexpr (std::is_convertible_v<const T&, std::string_view>) {
        return "\"" + std::string(std::string_view(v)) + "\"";
    } else if constexpr (std::is_arithmetic_v<T>) {
        return number(static_cast<double>(v));
    } else if constexpr (std::is_same_v<T, Vec2>) {
        return "Vec2(" + number(v.x) + ", " + number(v.y) + ")";
    } else if constexpr (std::is_same_v<T, Vec3>) {
        return "Vec3(" + number(v.x) + ", " + number(v.y) + ", " + number(v.z) + ")";
    } else if constexpr (std::is_same_v<T, Quat>) {
        return "Quat(" + number(v.x) + ", " + number(v.y) + ", " + number(v.z) + ", " + number(v.w) + ")";
    } else if constexpr (std::is_same_v<T, Color>) {
        return "Color(" + number(v.r) + ", " + number(v.g) + ", " + number(v.b) + ", " + number(v.a) + ")";
    } else if constexpr (std::is_same_v<T, Entity>) {
        return v.id() != 0 ? "Entity(" + std::to_string(v.id()) + ")" : "Entity(nula)";
    } else if constexpr (std::is_pointer_v<T> || std::is_null_pointer_v<T>) {
        return v != nullptr ? "puntero" : "nullptr";
    } else if constexpr (requires(std::ostream& o) { o << v; }) {
        std::ostringstream o;
        o << v;
        return o.str();
    } else if constexpr (std::is_constructible_v<Value, const T&>) {
        return describe(Value(v));
    } else {
        return "(valor)";
    }
}

template <class A, class B>
bool same(const A& a, const B& b) {
    if constexpr (std::is_convertible_v<const A&, std::string_view> && std::is_convertible_v<const B&, std::string_view>) {
        return std::string_view(a) == std::string_view(b);
    } else if constexpr (std::is_integral_v<A> && std::is_integral_v<B> && !std::is_same_v<A, bool> && !std::is_same_v<B, bool>) {
        return std::cmp_equal(a, b);
    } else if constexpr (std::is_arithmetic_v<A> && std::is_arithmetic_v<B>) {
        return static_cast<double>(a) == static_cast<double>(b);
    } else if constexpr (std::is_same_v<A, Value> || std::is_same_v<B, Value>) {
        return Value(a).toJson() == Value(b).toJson();
    } else if constexpr (requires { static_cast<bool>(a == b); }) {
        return static_cast<bool>(a == b);
    } else {
        static_assert(std::is_constructible_v<Value, const A&> && std::is_constructible_v<Value, const B&>,
                      "Assert::equal: estos tipos no se pueden comparar (sin ==)");
        return Value(a).toJson() == Value(b).toJson();
    }
}

// Numeros, Vec2/Vec3/Quat/Color o un Value con uno de ellos: sus componentes.
template <class T>
std::vector<double> components(const T& v) {
    if constexpr (std::is_arithmetic_v<T>) return {static_cast<double>(v)};
    else if constexpr (std::is_same_v<T, Vec2>) return {v.x, v.y};
    else if constexpr (std::is_same_v<T, Vec3>) return {v.x, v.y, v.z};
    else if constexpr (std::is_same_v<T, Quat>) return {v.x, v.y, v.z, v.w};
    else if constexpr (std::is_same_v<T, Color>) return {v.r, v.g, v.b, v.a};
    else if constexpr (std::is_same_v<T, Value>) {
        if (v.isNumber()) return {v.asNumber()};
        if (v.type() == Value::Type::Quat) return components(v.asQuat());
        if (v.type() == Value::Type::Vec3) return components(v.asVec3());
        return {};
    } else {
        return {};
    }
}

// Un numero para greater/less (los Value, por su numero).
template <class T>
double scalar(const T& v) {
    if constexpr (std::is_same_v<T, Value>) return v.asNumber();
    else return static_cast<double>(v);
}

template <class A, class B>
int compare(const A& a, const B& b) {
    if constexpr (std::is_integral_v<A> && std::is_integral_v<B>) {
        return std::cmp_less(a, b) ? -1 : (std::cmp_equal(a, b) ? 0 : 1);
    } else {
        const double x = scalar(a), y = scalar(b);
        return x < y ? -1 : (x > y ? 1 : (x == y ? 0 : 2));  // 2: NaN
    }
}

template <class T>
bool isNullValue(const T& v) {
    if constexpr (std::is_same_v<T, Value>) return v.isNil();
    else if constexpr (std::is_same_v<T, Entity>) return !v.valid();  // nula o ya destruida
    else if constexpr (std::is_null_pointer_v<T>) return true;
    else if constexpr (std::is_pointer_v<T>) return v == nullptr;
    else if constexpr (requires { v == nullptr; }) return static_cast<bool>(v == nullptr);  // shared_ptr, function...
    else if constexpr (requires { v.has_value(); }) return !v.has_value();             // optional
    else if constexpr (requires { v.valid(); }) return !v.valid();                     // Prefab, AudioClip...
    else return !static_cast<bool>(v);                                                  // Mesh...
}

// La espera de waitFrames / waitSeconds / waitUntil.
struct Waiter {
    Wait wait;
    PromiseBase* root = nullptr;
    bool await_ready() {
        switch (wait.kind) {
            case Wait::Kind::Frames: return wait.frames <= 0;
            case Wait::Kind::Seconds: return wait.seconds <= 0.0f;
            case Wait::Kind::Until: return !wait.until || wait.until();
            default: return true;
        }
    }
    template <TestPromise P>
    void await_suspend(std::coroutine_handle<P> h) {
        root = h.promise().root;
        root->wait = std::move(wait);
        root->leaf = h;
    }
    void await_resume() {
        if (root == nullptr) return;
        const bool timed_out = root->wait.timed_out;
        const std::string message = std::move(root->wait.message);
        root->wait = {};
        if (timed_out) check(false, "Test.waitUntil", message, {});
    }
};

}  // namespace detail

/// Una funcion de ayuda que espera (co_await Test::waitSeconds...): se espera con co_await.
class [[nodiscard]] Task {
public:
    struct promise_type : detail::PromiseBase {
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
        void return_void() noexcept {}
    };

    Task(Task&& o) noexcept : handle_(std::exchange(o.handle_, {})) {}
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    Task& operator=(Task&&) = delete;
    ~Task() {
        if (handle_) handle_.destroy();
    }

    // co_await tarea: la empieza y sigue cuando termina (con su excepcion, si fallo).
    bool await_ready() const noexcept { return !handle_ || handle_.done(); }
    template <detail::TestPromise P>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<P> parent) noexcept {
        promise_type& p = handle_.promise();
        p.parent = parent;
        p.root = parent.promise().root;
        p.root->leaf = handle_;
        return handle_;
    }
    void await_resume() const {
        if (handle_ && handle_.promise().error) std::rethrow_exception(handle_.promise().error);
    }

private:
    explicit Task(std::coroutine_handle<promise_type> h) : handle_(h) {}
    std::coroutine_handle<promise_type> handle_;
};

/// co_await Test::waitFrames(3): espera frames (updates del juego).
inline detail::Waiter waitFrames(int frames = 1) {
    detail::Waiter w;
    w.wait.kind = detail::Wait::Kind::Frames;
    w.wait.frames = frames;
    return w;
}
/// co_await Test::waitSeconds(1.5f): espera segundos del juego.
inline detail::Waiter waitSeconds(float seconds) {
    detail::Waiter w;
    w.wait.kind = detail::Wait::Kind::Seconds;
    w.wait.seconds = seconds;
    return w;
}
/// co_await Test::waitUntil([&] { return x; }, 5.0f): espera a que se cumpla (cada frame);
/// si pasa el limite (segundos), la prueba falla.
template <class F>
detail::Waiter waitUntil(F condition, float timeout = 5.0f, std::string_view message = {}) {
    detail::Waiter w;
    w.wait.kind = detail::Wait::Kind::Until;
    w.wait.until = [fn = std::move(condition)]() mutable { return static_cast<bool>(fn()); };
    w.wait.seconds = timeout;
    const std::string why = "Test.waitUntil: no se cumplio en " + detail::number(timeout) + " s";
    w.wait.message = message.empty() ? why : std::string(message) + " (" + why + ")";
    return w;
}

}  // namespace Test

// Comprobaciones de las pruebas: si falla, la prueba termina (y cuenta como fallida).
// El mensaje es opcional; sin el, se dice que se esperaba.
namespace Assert {
/// Es verdadero (bool, Value, Entity valida, puntero...).
template <class T>
void isTrue(const T& v, std::string_view message = {}) {
    Test::detail::check(static_cast<bool>(v), "Assert.isTrue", "se esperaba verdadero, es " + Test::detail::describe(v), message);
}
/// Es falso.
template <class T>
void isFalse(const T& v, std::string_view message = {}) {
    Test::detail::check(!static_cast<bool>(v), "Assert.isFalse", "se esperaba falso, es " + Test::detail::describe(v), message);
}
/// actual == expected (numeros, texto, Vec3, Value, entidades...).
template <class A, class E>
void equal(const A& actual, const E& expected, std::string_view message = {}) {
    Test::detail::check(Test::detail::same(actual, expected), "Assert.equal",
                        "se esperaba " + Test::detail::describe(expected) + ", es " + Test::detail::describe(actual), message);
}
/// actual != other.
template <class A, class E>
void notEqual(const A& actual, const E& other, std::string_view message = {}) {
    Test::detail::check(!Test::detail::same(actual, other), "Assert.notEqual", "no deberia ser " + Test::detail::describe(other), message);
}
/// |actual - expected| <= tolerance (numeros, Vec2, Vec3, Quat, Color: cada componente).
template <class A, class E>
void near(const A& actual, const E& expected, double tolerance = 1e-4, std::string_view message = {}) {
    const std::vector<double> a = Test::detail::components(actual), e = Test::detail::components(expected);
    bool ok = !a.empty() && a.size() == e.size();
    for (std::size_t i = 0; ok && i < a.size(); ++i) ok = std::fabs(a[i] - e[i]) <= tolerance;
    Test::detail::check(ok, "Assert.near",
                        "se esperaba " + Test::detail::describe(expected) + " (+-" + Test::detail::number(tolerance) + "), es " +
                            Test::detail::describe(actual),
                        message);
}
/// Como near (el nombre de antes; sirve tambien con <windows.h>, que define near).
template <class A, class E>
void approx(const A& actual, const E& expected, double tolerance = 1e-4, std::string_view message = {}) {
    near(actual, expected, tolerance, message);
}
/// a > b.
template <class A, class B>
void greater(const A& a, const B& b, std::string_view message = {}) {
    Test::detail::check(Test::detail::compare(a, b) == 1, "Assert.greater",
                        Test::detail::describe(a) + " no es mayor que " + Test::detail::describe(b), message);
}
/// a < b.
template <class A, class B>
void less(const A& a, const B& b, std::string_view message = {}) {
    Test::detail::check(Test::detail::compare(a, b) == -1, "Assert.less",
                        Test::detail::describe(a) + " no es menor que " + Test::detail::describe(b), message);
}
/// a >= b.
template <class A, class B>
void atLeast(const A& a, const B& b, std::string_view message = {}) {
    const int c = Test::detail::compare(a, b);
    Test::detail::check(c == 0 || c == 1, "Assert.atLeast", Test::detail::describe(a) + " es menor que " + Test::detail::describe(b),
                        message);
}
/// a <= b.
template <class A, class B>
void atMost(const A& a, const B& b, std::string_view message = {}) {
    const int c = Test::detail::compare(a, b);
    Test::detail::check(c == 0 || c == -1, "Assert.atMost", Test::detail::describe(a) + " es mayor que " + Test::detail::describe(b),
                        message);
}
/// Es nulo: Value nil, Entity nula o destruida, nullptr, optional vacio, asset sin poner.
template <class T>
void isNull(const T& v, std::string_view message = {}) {
    Test::detail::check(Test::detail::isNullValue(v), "Assert.isNull", "se esperaba nulo, es " + Test::detail::describe(v), message);
}
/// No es nulo.
template <class T>
void notNull(const T& v, std::string_view message = {}) {
    Test::detail::check(!Test::detail::isNullValue(v), "Assert.notNull", "no deberia ser nulo", message);
}
/// fn() lanza una excepcion.
template <class F>
void throws(F&& fn, std::string_view message = {}) {
    bool threw = false;
    try {
        fn();
    } catch (...) {
        threw = true;
    }
    Test::detail::check(threw, "Assert.throws", "se esperaba una excepcion", message);
}
/// Falla siempre (y termina la prueba).
inline void fail(std::string_view message = {}) { Test::detail::check(false, "Assert.fail", "Assert.fail", message); }
}  // namespace Assert

/// Corre las pruebas (CRAMION_TEST) al empezar el juego: ponlo en un objeto de la escena.
/// Una detras de otra; cada una con su limite de tiempo (30 s si no se dice).
class CramionTests : public Script {
public:
    ~CramionTests() override { stop(); }
    void start() override {
        next_ = 0;
        finished_ = false;
        advance();
    }
    void update(float dt) override { tick(dt); }
    void onDestroy() override { stop(); }

private:
    using Wait = Test::detail::Wait;

    // Empieza las pruebas que quedan hasta una que espera (o hasta el final).
    void advance() {
        const auto& list = Test::detail::cases();
        while (!current_.handle && next_ < list.size()) begin(list[next_++]);
        if (!current_.handle && !finished_) {
            finished_ = true;
            Api::call("Test.done");
        }
    }

    void begin(const Test::detail::Case& c) {
        Api::call("Test.begin", {c.name});
        Test::detail::running() = true;
        elapsed_ = 0.0f;
        timeout_ = c.timeout;
        Test::detail::Slot slot;
        try {
            c.fn(Test::detail::Context{&slot});  // sin co_await corre entera aqui
        } catch (...) {
            if (slot.handle) slot.handle.destroy();
            end(std::current_exception());
            return;
        }
        if (!slot.handle) {
            end(nullptr);
            return;
        }
        current_ = slot;
        resume();
    }

    // Sigue la corrutina que espera; si termino, cierra la prueba.
    void resume() {
        current_.promise->leaf.resume();
        if (!current_.handle.done()) return;
        const std::exception_ptr error = current_.promise->error;
        stop();
        end(error);
    }

    void tick(float dt) {
        if (!current_.handle) return;
        elapsed_ += dt;
        Wait& w = current_.promise->wait;
        bool ready = false;
        try {
            switch (w.kind) {
                case Wait::Kind::Frames: ready = --w.frames <= 0; break;
                case Wait::Kind::Seconds:
                    w.seconds -= dt;
                    ready = w.seconds <= 0.0f;
                    break;
                case Wait::Kind::Until:
                    if (!w.until || w.until()) {
                        ready = true;
                    } else if ((w.seconds -= dt) <= 0.0f) {
                        w.timed_out = true;
                        ready = true;
                    }
                    break;
                default: ready = true; break;
            }
        } catch (...) {  // la condicion de waitUntil fallo
            stop();
            end(std::current_exception());
            advance();
            return;
        }
        if (ready) {
            resume();
        } else if (elapsed_ >= timeout_) {
            stop();
            failNow("tiempo agotado: la prueba paso de " + Test::detail::number(timeout_) + " s");
            finish();
        }
        if (!current_.handle) advance();
    }

    // Cierra la prueba (con el error que la termino, si lo hubo).
    void end(std::exception_ptr error) {
        if (error) {
            try {
                std::rethrow_exception(error);
            } catch (const Test::detail::Failure&) {
                // Un Assert: ya se conto.
            } catch (const std::exception& e) {
                failNow(std::string("excepcion: ") + e.what());
            } catch (...) {
                failNow("excepcion desconocida");
            }
        }
        finish();
    }
    void failNow(const std::string& text) { Api::call("Test.check", {false, text}); }
    void finish() {
        Test::detail::running() = false;
        Api::call("Test.finish");
    }
    void stop() {
        if (current_.handle) current_.handle.destroy();
        current_ = {};
        Test::detail::running() = false;
    }

    Test::detail::Slot current_;
    std::size_t next_ = 0;
    bool finished_ = false;
    float elapsed_ = 0.0f;
    float timeout_ = 30.0f;
};

// Una vez aunque se incluya en varios .cpp (variable inline).
inline const detail::Registrar<CramionTests> cramion_registrar_CramionTests{"CramionTests", __FILE__};

}  // namespace cramion

// La prueba es una corrutina que devuelve void con el parametro oculto Context.
namespace std {
template <>
struct coroutine_traits<void, ::cramion::Test::detail::Context> {
    using promise_type = ::cramion::Test::detail::RootPromise;
};
}  // namespace std

#pragma pop_macro("near")

// Una prueba: CRAMION_TEST(Nombre) { ...Assert::...; co_await Test::waitSeconds(1.0f); ... }
#define CRAMION_TEST_TIMEOUT(Name, Seconds)                                                                                         \
    static void cramion_test_##Name(::cramion::Test::detail::Context);                                                             \
    static const ::cramion::Test::detail::Registrar cramion_test_registrar_##Name(#Name, &cramion_test_##Name, (Seconds));          \
    static void cramion_test_##Name(::cramion::Test::detail::Context)
#define CRAMION_TEST(Name) CRAMION_TEST_TIMEOUT(Name, 30.0f)

// Libreria matematica de Lua (como Vector3, Quaternion, Mathf y Random de
// Unity): vectores, rotaciones con cuaterniones, funciones de interpolacion,
// angulos, amortiguado, numeros aleatorios con semilla y ruido Perlin.
//
// Convenciones del motor: grados en la API, Euler en orden YXZ (el de
// Entity.rotation) y "adelante" es -Z (Vec3.forward).

#include "LuaMath.h"

#include "CramionCore/ecs/MathUtil.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <random>
#include <cstdio>
#include <sstream>

namespace cramion::scripting {

using core::Quat;
using core::Vec3;

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDeg = 180.0f / kPi;
constexpr float kRad = kPi / 180.0f;

// Mismo formato de siempre: (1.000, 2.000, 3.000).
std::string vecText(const Vec3& v) {
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "(%.3f, %.3f, %.3f)", v.x, v.y, v.z);
    return buffer;
}

float len(const Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
Vec3 unit(const Vec3& v) {
    const float l = len(v);
    return l > 1e-6f ? v * (1.0f / l) : Vec3{};
}
float dot3(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross3(const Vec3& a, const Vec3& b) { return core::cross(a, b); }

// Angulo sin signo entre dos vectores, en grados.
float angleBetween(const Vec3& a, const Vec3& b) {
    const float d = len(a) * len(b);
    if (d < 1e-12f) return 0.0f;
    return std::acos(std::clamp(dot3(a, b) / d, -1.0f, 1.0f)) * kDeg;
}

// --- Mathf -------------------------------------------------------------------
float repeatF(float t, float length) {
    if (length == 0.0f) return 0.0f;
    return std::clamp(t - std::floor(t / length) * length, 0.0f, length);
}
float deltaAngle(float current, float target) {
    float d = repeatF(target - current, 360.0f);
    if (d > 180.0f) d -= 360.0f;
    return d;
}
float moveTowardsF(float current, float target, float step) {
    return std::abs(target - current) <= step ? target : current + (target > current ? step : -step);
}

// SmoothDamp (Game Programming Gems 4): muelle critico, sin pasarse.
std::pair<float, float> smoothDampF(float current, float target, float velocity, float smooth_time, float dt,
                                    float max_speed) {
    smooth_time = std::max(0.0001f, smooth_time);
    const float omega = 2.0f / smooth_time;
    const float x = omega * dt;
    const float e = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
    float change = current - target;
    const float original = target;
    const float max_change = max_speed * smooth_time;
    change = std::clamp(change, -max_change, max_change);
    target = current - change;
    const float temp = (velocity + omega * change) * dt;
    velocity = (velocity - omega * temp) * e;
    float out = target + (change + temp) * e;
    if ((original - current > 0.0f) == (out > original)) {
        out = original;
        velocity = dt > 0.0f ? (out - original) / dt : 0.0f;
    }
    return {out, velocity};
}

// --- Ruido Perlin mejorado (Ken Perlin, 2002) --------------------------------
const int* permutation() {
    static const int base[256] = {
        151, 160, 137, 91,  90,  15,  131, 13,  201, 95,  96,  53,  194, 233, 7,   225, 140, 36,  103, 30,  69,
        142, 8,   99,  37,  240, 21,  10,  23,  190, 6,   148, 247, 120, 234, 75,  0,   26,  197, 62,  94,  252,
        219, 203, 117, 35,  11,  32,  57,  177, 33,  88,  237, 149, 56,  87,  174, 20,  125, 136, 171, 168, 68,
        175, 74,  165, 71,  134, 139, 48,  27,  166, 77,  146, 158, 231, 83,  111, 229, 122, 60,  211, 133, 230,
        220, 105, 92,  41,  55,  46,  245, 40,  244, 102, 143, 54,  65,  25,  63,  161, 1,   216, 80,  73,  209,
        76,  132, 187, 208, 89,  18,  169, 200, 196, 135, 130, 116, 188, 159, 86,  164, 100, 109, 198, 173, 186,
        3,   64,  52,  217, 226, 250, 124, 123, 5,   202, 38,  147, 118, 126, 255, 82,  85,  212, 207, 206, 59,
        227, 47,  16,  58,  17,  182, 189, 28,  42,  223, 183, 170, 213, 119, 248, 152, 2,   44,  154, 163, 70,
        221, 153, 101, 155, 167, 43,  172, 9,   129, 22,  39,  253, 19,  98,  108, 110, 79,  113, 224, 232, 178,
        185, 112, 104, 218, 246, 97,  228, 251, 34,  242, 193, 238, 210, 144, 12,  191, 179, 162, 241, 81,  51,
        145, 235, 249, 14,  239, 107, 49,  192, 214, 31,  181, 199, 106, 157, 184, 84,  204, 176, 115, 121, 50,
        45,  127, 4,   150, 254, 138, 236, 205, 93,  222, 114, 67,  29,  24,  72,  243, 141, 128, 195, 78,  66,
        215, 61,  156, 180};
    static int p[512] = {};
    static bool ready = false;
    if (!ready) {
        for (int i = 0; i < 512; ++i) p[i] = base[i & 255];
        ready = true;
    }
    return p;
}
float fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }
float grad(int hash, float x, float y, float z) {
    const int h = hash & 15;
    const float u = h < 8 ? x : y;
    const float v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
    return ((h & 1) == 0 ? u : -u) + ((h & 2) == 0 ? v : -v);
}
float lerpF(float a, float b, float t) { return a + (b - a) * t; }

// Ruido en [-1, 1] (aprox.).
float perlin3(float x, float y, float z) {
    const int* p = permutation();
    const int X = static_cast<int>(std::floor(x)) & 255;
    const int Y = static_cast<int>(std::floor(y)) & 255;
    const int Z = static_cast<int>(std::floor(z)) & 255;
    x -= std::floor(x);
    y -= std::floor(y);
    z -= std::floor(z);
    const float u = fade(x), v = fade(y), w = fade(z);
    const int A = p[X] + Y, AA = p[A] + Z, AB = p[A + 1] + Z;
    const int B = p[X + 1] + Y, BA = p[B] + Z, BB = p[B + 1] + Z;
    return lerpF(lerpF(lerpF(grad(p[AA], x, y, z), grad(p[BA], x - 1, y, z), u),
                       lerpF(grad(p[AB], x, y - 1, z), grad(p[BB], x - 1, y - 1, z), u), v),
                 lerpF(lerpF(grad(p[AA + 1], x, y, z - 1), grad(p[BA + 1], x - 1, y, z - 1), u),
                       lerpF(grad(p[AB + 1], x, y - 1, z - 1), grad(p[BB + 1], x - 1, y - 1, z - 1), u), v),
                 w);
}

// --- Quat -------------------------------------------------------------------
Quat angleAxis(float degrees, const Vec3& axis) {
    const Vec3 a = unit(axis);
    const float h = degrees * kRad * 0.5f;
    const float s = std::sin(h);
    return Quat{a.x * s, a.y * s, a.z * s, std::cos(h)};
}
float quatDot(const Quat& a, const Quat& b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
float quatAngle(const Quat& a, const Quat& b) {
    const float d = std::min(std::abs(quatDot(core::normalize(a), core::normalize(b))), 1.0f);
    return d > 1.0f - 1e-6f ? 0.0f : 2.0f * std::acos(d) * kDeg;
}

// Cuaternion de una base ortonormal (columnas x, y, z).
Quat fromBasis(const Vec3& x, const Vec3& y, const Vec3& z) {
    const float m00 = x.x, m01 = y.x, m02 = z.x;
    const float m10 = x.y, m11 = y.y, m12 = z.y;
    const float m20 = x.z, m21 = y.z, m22 = z.z;
    const float trace = m00 + m11 + m22;
    Quat q;
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q = Quat{(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s};
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q = Quat{0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q = Quat{(m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s};
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q = Quat{(m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s};
    }
    return core::normalize(q);
}

// La rotacion que mira hacia `forward` (el -Z local apunta alli) con `up` arriba.
Quat lookRotation(const Vec3& forward, const Vec3& up) {
    const Vec3 f = unit(forward);
    if (len(f) < 0.5f) return Quat{};
    const Vec3 z = f * -1.0f;
    Vec3 x = cross3(up, z);
    if (len(x) < 1e-5f) x = cross3(std::abs(z.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0}, z);  // up paralelo
    x = unit(x);
    const Vec3 y = cross3(z, x);
    return fromBasis(x, y, z);
}

Quat fromToRotation(const Vec3& from, const Vec3& to) {
    const Vec3 a = unit(from);
    const Vec3 b = unit(to);
    const float d = dot3(a, b);
    if (d > 1.0f - 1e-6f) return Quat{};
    if (d < -1.0f + 1e-6f) {
        Vec3 axis = cross3(Vec3{1, 0, 0}, a);
        if (len(axis) < 1e-5f) axis = cross3(Vec3{0, 1, 0}, a);
        return angleAxis(180.0f, axis);
    }
    const Vec3 c = cross3(a, b);
    return core::normalize(Quat{c.x, c.y, c.z, 1.0f + d});
}

std::string quatText(const Quat& q) {
    std::ostringstream s;
    s << "Quat(" << q.x << ", " << q.y << ", " << q.z << ", " << q.w << ")";
    return s.str();
}

// Numeros aleatorios: un generador por estado de Lua, con semilla opcional.
struct Rng {
    std::mt19937 engine{std::random_device{}()};
    float value() { return std::uniform_real_distribution<float>(0.0f, 1.0f)(engine); }
    float range(float a, float b) { return a + (b - a) * value(); }
    int integer(int a, int b) {
        if (a > b) std::swap(a, b);
        return std::uniform_int_distribution<int>(a, b)(engine);
    }
    Vec3 onSphere() {
        std::normal_distribution<float> n(0.0f, 1.0f);
        for (int i = 0; i < 8; ++i) {
            const Vec3 v{n(engine), n(engine), n(engine)};
            if (len(v) > 1e-4f) return unit(v);
        }
        return Vec3{0, 1, 0};
    }
};

}  // namespace

void bindMath(sol::state& L) {
    // --- Vec3 -------------------------------------------------------------------
    auto vec = L.new_usertype<Vec3>(
        "Vec3", sol::call_constructor,
        sol::factories([]() { return Vec3{}; }, [](float x, float y, float z) { return Vec3{x, y, z}; },
                       [](float x, float y) { return Vec3{x, y, 0.0f}; }, [](const Vec3& v) { return v; }),
        "x", &Vec3::x, "y", &Vec3::y, "z", &Vec3::z,
        sol::meta_function::addition, [](const Vec3& a, const Vec3& b) { return a + b; },
        sol::meta_function::subtraction, [](const Vec3& a, const Vec3& b) { return a - b; },
        sol::meta_function::multiplication,
        sol::overload([](const Vec3& a, float s) { return a * s; }, [](float s, const Vec3& a) { return a * s; },
                      [](const Vec3& a, const Vec3& b) { return Vec3{a.x * b.x, a.y * b.y, a.z * b.z}; }),
        sol::meta_function::division,
        sol::overload([](const Vec3& a, float s) { return a * (1.0f / s); },
                      [](const Vec3& a, const Vec3& b) { return Vec3{a.x / b.x, a.y / b.y, a.z / b.z}; }),
        sol::meta_function::unary_minus, [](const Vec3& a) { return a * -1.0f; },
        sol::meta_function::equal_to, [](const Vec3& a, const Vec3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; },
        sol::meta_function::to_string, [](const Vec3& v) { return vecText(v); },
        sol::meta_function::concatenation,
        sol::overload([](const std::string& s, const Vec3& v) { return s + vecText(v); },
                      [](const Vec3& v, const std::string& s) { return vecText(v) + s; }),
        // Longitud y direccion.
        "length", [](const Vec3& v) { return len(v); },
        "sqrLength", [](const Vec3& v) { return dot3(v, v); },
        "normalized", [](const Vec3& v) { return unit(v); },
        "clampLength", [](const Vec3& v, float max_length) {
            const float l = len(v);
            return l > max_length && l > 1e-6f ? v * (max_length / l) : v;
        },
        // Productos, distancias y angulos.
        "dot", [](const Vec3& a, const Vec3& b) { return dot3(a, b); },
        "cross", [](const Vec3& a, const Vec3& b) { return cross3(a, b); },
        "distance", [](const Vec3& a, const Vec3& b) { return len(a - b); },
        "sqrDistance", [](const Vec3& a, const Vec3& b) { const Vec3 d = a - b; return dot3(d, d); },
        "angle", [](const Vec3& a, const Vec3& b) { return angleBetween(a, b); },
        "signedAngle", [](const Vec3& a, const Vec3& b, const Vec3& axis) {
            const float angle = angleBetween(a, b);
            return dot3(axis, cross3(a, b)) < 0.0f ? -angle : angle;
        },
        // Interpolacion y movimiento.
        "lerp", [](const Vec3& a, const Vec3& b, float t) { return a + (b - a) * std::clamp(t, 0.0f, 1.0f); },
        "lerpUnclamped", [](const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; },
        "slerp", [](const Vec3& a, const Vec3& b, float t) {
            // Gira la direccion y mezcla la longitud.
            t = std::clamp(t, 0.0f, 1.0f);
            const float la = len(a), lb = len(b);
            if (la < 1e-6f || lb < 1e-6f) return a + (b - a) * t;
            const Quat q = core::slerp(Quat{}, fromToRotation(a, b), t);
            return unit(ecs::quatRotate(q, a)) * (la + (lb - la) * t);
        },
        "moveTowards", [](const Vec3& current, const Vec3& target, float max_delta) {
            const Vec3 d = target - current;
            const float l = len(d);
            return l <= max_delta || l < 1e-6f ? target : current + d * (max_delta / l);
        },
        "smoothDamp", [](const Vec3& current, const Vec3& target, const Vec3& velocity, float smooth_time, float dt,
                         sol::optional<float> max_speed) {
            const float ms = max_speed.value_or(std::numeric_limits<float>::infinity());
            const auto x = smoothDampF(current.x, target.x, velocity.x, smooth_time, dt, ms);
            const auto y = smoothDampF(current.y, target.y, velocity.y, smooth_time, dt, ms);
            const auto z = smoothDampF(current.z, target.z, velocity.z, smooth_time, dt, ms);
            return std::make_tuple(Vec3{x.first, y.first, z.first}, Vec3{x.second, y.second, z.second});
        },
        // Proyecciones y reflejos.
        "project", [](const Vec3& v, const Vec3& on) {
            const float d = dot3(on, on);
            return d < 1e-12f ? Vec3{} : on * (dot3(v, on) / d);
        },
        "projectOnPlane", [](const Vec3& v, const Vec3& normal) {
            const float d = dot3(normal, normal);
            return d < 1e-12f ? v : v - normal * (dot3(v, normal) / d);
        },
        "reflect", [](const Vec3& direction, const Vec3& normal) {
            const Vec3 n = unit(normal);
            return direction - n * (2.0f * dot3(direction, n));
        },
        // Por componentes.
        "scale", [](const Vec3& a, const Vec3& b) { return Vec3{a.x * b.x, a.y * b.y, a.z * b.z}; },
        "min", [](const Vec3& a, const Vec3& b) { return Vec3{std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; },
        "max", [](const Vec3& a, const Vec3& b) { return Vec3{std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; },
        "abs", [](const Vec3& v) { return Vec3{std::abs(v.x), std::abs(v.y), std::abs(v.z)}; },
        "floor", [](const Vec3& v) { return Vec3{std::floor(v.x), std::floor(v.y), std::floor(v.z)}; },
        "ceil", [](const Vec3& v) { return Vec3{std::ceil(v.x), std::ceil(v.y), std::ceil(v.z)}; },
        "round", [](const Vec3& v) { return Vec3{std::round(v.x), std::round(v.y), std::round(v.z)}; },
        "approximately", [](const Vec3& a, const Vec3& b, sol::optional<float> epsilon) {
            return len(a - b) <= epsilon.value_or(1e-4f);
        },
        // Copias y componentes sueltos (un Vec3 de Lua se comparte como una tabla).
        "copy", [](const Vec3& v) { return v; },
        "unpack", [](const Vec3& v) { return std::make_tuple(v.x, v.y, v.z); },
        "set", [](Vec3& v, float x, float y, float z) { v = Vec3{x, y, z}; });
    // Constantes: cada acceso da una copia nueva (modificar lo que devuelven no
    // cambia la constante, p. ej. `self.vel = Vec3.zero; self.vel.y = 5`).
    vec["zero"] = sol::property([]() { return Vec3{0.0f, 0.0f, 0.0f}; });
    vec["one"] = sol::property([]() { return Vec3{1.0f, 1.0f, 1.0f}; });
    vec["up"] = sol::property([]() { return Vec3{0.0f, 1.0f, 0.0f}; });
    vec["down"] = sol::property([]() { return Vec3{0.0f, -1.0f, 0.0f}; });
    vec["right"] = sol::property([]() { return Vec3{1.0f, 0.0f, 0.0f}; });
    vec["left"] = sol::property([]() { return Vec3{-1.0f, 0.0f, 0.0f}; });
    vec["forward"] = sol::property([]() { return Vec3{0.0f, 0.0f, -1.0f}; });
    vec["back"] = sol::property([]() { return Vec3{0.0f, 0.0f, 1.0f}; });

    // --- Quat -------------------------------------------------------------------
    auto quat = L.new_usertype<Quat>(
        "Quat", sol::call_constructor,
        sol::factories([]() { return Quat{}; }, [](float x, float y, float z, float w) { return Quat{x, y, z, w}; }),
        "x", &Quat::x, "y", &Quat::y, "z", &Quat::z, "w", &Quat::w,
        // q1 * q2 = primero q2 y luego q1; q * v gira el vector.
        sol::meta_function::multiplication,
        sol::overload([](const Quat& a, const Quat& b) { return ecs::quatMultiply(a, b); },
                      [](const Quat& q, const Vec3& v) { return ecs::quatRotate(q, v); }),
        sol::meta_function::equal_to, [](const Quat& a, const Quat& b) { return quatAngle(a, b) < 1e-3f; },
        sol::meta_function::to_string, [](const Quat& q) { return quatText(q); },
        "euler", sol::overload([](float x, float y, float z) { return ecs::quatFromEulerDegrees(Vec3{x, y, z}); },
                               [](const Vec3& degrees) { return ecs::quatFromEulerDegrees(degrees); }),
        "angleAxis", [](float degrees, const Vec3& axis) { return angleAxis(degrees, axis); },
        "lookRotation", [](const Vec3& forward, sol::optional<Vec3> up) { return lookRotation(forward, up.value_or(Vec3{0, 1, 0})); },
        "fromToRotation", [](const Vec3& from, const Vec3& to) { return fromToRotation(from, to); },
        "toEuler", [](const Quat& q) { return ecs::quatToEulerDegrees(core::normalize(q)); },
        "inverse", [](const Quat& q) { return ecs::quatConjugate(core::normalize(q)); },
        "normalized", [](const Quat& q) { return core::normalize(q); },
        "dot", [](const Quat& a, const Quat& b) { return quatDot(a, b); },
        "angle", [](const Quat& a, const Quat& b) { return quatAngle(a, b); },
        "slerp", [](const Quat& a, const Quat& b, float t) { return core::slerp(core::normalize(a), core::normalize(b), std::clamp(t, 0.0f, 1.0f)); },
        "lerp", [](const Quat& a, Quat b, float t) {
            t = std::clamp(t, 0.0f, 1.0f);
            if (quatDot(a, b) < 0.0f) b = Quat{-b.x, -b.y, -b.z, -b.w};
            return core::normalize(Quat{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t});
        },
        "rotateTowards", [](const Quat& from, const Quat& to, float max_degrees) {
            const float angle = quatAngle(from, to);
            if (angle < 1e-4f) return core::normalize(to);
            return core::slerp(core::normalize(from), core::normalize(to), std::min(1.0f, max_degrees / angle));
        },
        "forward", [](const Quat& q) { return ecs::quatRotate(q, Vec3{0, 0, -1}); },
        "right", [](const Quat& q) { return ecs::quatRotate(q, Vec3{1, 0, 0}); },
        "up", [](const Quat& q) { return ecs::quatRotate(q, Vec3{0, 1, 0}); },
        "copy", [](const Quat& q) { return q; });
    quat["identity"] = sol::property([]() { return Quat{}; });

    // --- Mathf ------------------------------------------------------------------
    sol::table m = L.create_named_table("Mathf");
    m["pi"] = static_cast<double>(kPi);
    m["tau"] = static_cast<double>(kPi) * 2.0;
    m["deg2rad"] = static_cast<double>(kRad);
    m["rad2deg"] = static_cast<double>(kDeg);
    m["infinity"] = std::numeric_limits<double>::infinity();
    m["negativeInfinity"] = -std::numeric_limits<double>::infinity();
    m["epsilon"] = 1e-6;
    // Basicas.
    m["abs"] = [](double v) { return std::abs(v); };
    m["sign"] = [](double v) { return v < 0.0 ? -1.0 : 1.0; };
    m["min"] = [](double a, double b, sol::variadic_args rest) {
        double r = std::min(a, b);
        for (auto v : rest) r = std::min(r, v.as<double>());
        return r;
    };
    m["max"] = [](double a, double b, sol::variadic_args rest) {
        double r = std::max(a, b);
        for (auto v : rest) r = std::max(r, v.as<double>());
        return r;
    };
    m["floor"] = [](double v) { return std::floor(v); };
    m["ceil"] = [](double v) { return std::ceil(v); };
    m["round"] = [](double v, sol::optional<int> digits) {
        const double f = std::pow(10.0, digits.value_or(0));
        return std::round(v * f) / f;
    };
    m["sqrt"] = [](double v) { return std::sqrt(v); };
    m["pow"] = [](double a, double b) { return std::pow(a, b); };
    m["exp"] = [](double v) { return std::exp(v); };
    m["log"] = [](double v, sol::optional<double> base) { return base ? std::log(v) / std::log(*base) : std::log(v); };
    m["log10"] = [](double v) { return std::log10(v); };
    // Trigonometria (radianes, como math.*).
    m["sin"] = [](double v) { return std::sin(v); };
    m["cos"] = [](double v) { return std::cos(v); };
    m["tan"] = [](double v) { return std::tan(v); };
    m["asin"] = [](double v) { return std::asin(std::clamp(v, -1.0, 1.0)); };
    m["acos"] = [](double v) { return std::acos(std::clamp(v, -1.0, 1.0)); };
    m["atan"] = [](double v) { return std::atan(v); };
    m["atan2"] = [](double y, double x) { return std::atan2(y, x); };
    // Rangos e interpolacion.
    m["clamp"] = [](float v, float lo, float hi) { return std::clamp(v, std::min(lo, hi), std::max(lo, hi)); };
    m["clamp01"] = [](float v) { return std::clamp(v, 0.0f, 1.0f); };
    m["lerp"] = [](float a, float b, float t) { return a + (b - a) * std::clamp(t, 0.0f, 1.0f); };
    m["lerpUnclamped"] = [](float a, float b, float t) { return a + (b - a) * t; };
    m["inverseLerp"] = [](float a, float b, float v) { return a == b ? 0.0f : std::clamp((v - a) / (b - a), 0.0f, 1.0f); };
    m["remap"] = [](float v, float in_min, float in_max, float out_min, float out_max) {
        if (in_min == in_max) return out_min;
        return out_min + (v - in_min) / (in_max - in_min) * (out_max - out_min);
    };
    m["smoothstep"] = [](float a, float b, float v) {
        const float t = a == b ? (v >= b ? 1.0f : 0.0f) : std::clamp((v - a) / (b - a), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    };
    m["smootherstep"] = [](float a, float b, float v) {
        const float t = a == b ? (v >= b ? 1.0f : 0.0f) : std::clamp((v - a) / (b - a), 0.0f, 1.0f);
        return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
    };
    m["moveTowards"] = [](float current, float target, float step) { return moveTowardsF(current, target, step); };
    m["smoothDamp"] = [](float current, float target, float velocity, float smooth_time, float dt, sol::optional<float> max_speed) {
        const auto r = smoothDampF(current, target, velocity, smooth_time, dt,
                                   max_speed.value_or(std::numeric_limits<float>::infinity()));
        return std::make_tuple(r.first, r.second);
    };
    // "repeat" es palabra reservada de Lua: Mathf.wrap (Mathf["repeat"] tambien vale).
    m["wrap"] = [](float t, float length) { return repeatF(t, length); };
    m["repeat"] = m["wrap"];
    m["pingPong"] = [](float t, float length) {
        const float r = repeatF(t, length * 2.0f);
        return length - std::abs(r - length);
    };
    m["approximately"] = [](float a, float b, sol::optional<float> epsilon) {
        return std::abs(a - b) <= epsilon.value_or(std::max(1e-6f * std::max(std::abs(a), std::abs(b)), 1e-5f));
    };
    // Angulos (grados).
    m["deltaAngle"] = [](float current, float target) { return deltaAngle(current, target); };
    m["lerpAngle"] = [](float a, float b, float t) { return a + deltaAngle(a, b) * std::clamp(t, 0.0f, 1.0f); };
    m["moveTowardsAngle"] = [](float current, float target, float step) {
        const float d = deltaAngle(current, target);
        if (-step < d && d < step) return target;
        return moveTowardsF(current, current + d, step);
    };
    // Potencias de dos.
    m["isPowerOfTwo"] = [](int v) { return v > 0 && (v & (v - 1)) == 0; };
    m["nextPowerOfTwo"] = [](int v) {
        int p = 1;
        while (p < v && p < (1 << 30)) p <<= 1;
        return p;
    };
    // Ruido.
    m["perlinNoise"] = [](float x, float y) { return std::clamp(perlin3(x, y, 0.5f) * 0.5f + 0.5f, 0.0f, 1.0f); };
    m["perlinNoise3"] = [](float x, float y, float z) { return perlin3(x, y, z); };
    m["fractalNoise"] = [](float x, float y, sol::optional<int> octaves, sol::optional<float> persistence,
                           sol::optional<float> lacunarity) {
        const int n = std::clamp(octaves.value_or(4), 1, 12);
        const float g = persistence.value_or(0.5f);
        const float l = lacunarity.value_or(2.0f);
        float sum = 0.0f, amplitude = 1.0f, frequency = 1.0f, norm = 0.0f;
        for (int i = 0; i < n; ++i) {
            sum += perlin3(x * frequency, y * frequency, 0.5f + static_cast<float>(i) * 17.0f) * amplitude;
            norm += amplitude;
            amplitude *= g;
            frequency *= l;
        }
        return std::clamp((sum / norm) * 0.5f + 0.5f, 0.0f, 1.0f);
    };

    // --- Random -----------------------------------------------------------------
    auto rng = std::make_shared<Rng>();
    // Compatible con lo anterior: Mathf.random(min, max).
    m["random"] = [rng](float lo, float hi) { return rng->range(lo, hi); };
    sol::table r = L.create_named_table("Random");
    r["seed"] = [rng](int seed) { rng->engine.seed(static_cast<unsigned>(seed)); };
    r["value"] = [rng]() { return rng->value(); };
    r["range"] = [rng](float a, float b) { return rng->range(a, b); };
    r["int"] = [rng](int a, int b) { return rng->integer(a, b); };
    r["chance"] = [rng](float probability) { return rng->value() < probability; };
    r["sign"] = [rng]() { return rng->value() < 0.5f ? -1 : 1; };
    r["onUnitSphere"] = [rng]() { return rng->onSphere(); };
    r["insideUnitSphere"] = [rng]() { return rng->onSphere() * std::cbrt(rng->value()); };
    r["insideUnitCircle"] = [rng]() {  // en el plano del suelo (XZ)
        const float a = rng->value() * 2.0f * kPi;
        const float d = std::sqrt(rng->value());
        return Vec3{std::cos(a) * d, 0.0f, std::sin(a) * d};
    };
    r["rotation"] = [rng]() {  // rotacion uniforme al azar
        const float u1 = rng->value(), u2 = rng->value() * 2.0f * kPi, u3 = rng->value() * 2.0f * kPi;
        const float a = std::sqrt(1.0f - u1), b = std::sqrt(u1);
        return Quat{a * std::sin(u2), a * std::cos(u2), b * std::sin(u3), b * std::cos(u3)};
    };
    r["pick"] = [rng](const sol::table& list) -> sol::object {
        const std::size_t n = list.size();
        if (n == 0) return sol::lua_nil;
        return list[rng->integer(1, static_cast<int>(n))];
    };
    r["shuffle"] = [rng](sol::table list) {
        const int n = static_cast<int>(list.size());
        for (int i = n; i > 1; --i) {
            const int j = rng->integer(1, i);
            sol::object a = list[i];
            sol::object b = list[j];
            list[i] = b;
            list[j] = a;
        }
        return list;
    };
}

}  // namespace cramion::scripting

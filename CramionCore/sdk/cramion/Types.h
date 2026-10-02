// Cramion C++ scripting: tipos matematicos del SDK (Vec2, Vec3, Quat, Color)
// y Mathf, como los de Unity. Todo se calcula en el script (sin llamar al motor).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace cramion {

/// Funciones matematicas (como Mathf de Unity). Angulos en grados salvo sin/cos/tan.
namespace Mathf {
inline constexpr float pi = 3.14159265358979f;
inline constexpr float tau = 6.28318530717959f;
/// Multiplica grados para pasarlos a radianes.
inline constexpr float deg2rad = pi / 180.0f;
/// Multiplica radianes para pasarlos a grados.
inline constexpr float rad2deg = 180.0f / pi;
inline constexpr float epsilon = 1e-6f;
inline constexpr float infinity = std::numeric_limits<float>::infinity();
inline constexpr float negativeInfinity = -std::numeric_limits<float>::infinity();

inline float abs(float v) { return std::fabs(v); }
/// 1 si v >= 0, si no -1.
inline float sign(float v) { return v >= 0.0f ? 1.0f : -1.0f; }
inline float min(float a, float b) { return a < b ? a : b; }
inline float max(float a, float b) { return a > b ? a : b; }
inline float clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline int clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
/// v entre 0 y 1.
inline float clamp01(float v) { return clamp(v, 0.0f, 1.0f); }
/// De a a b segun t (0..1, recortado).
inline float lerp(float a, float b, float t) { return a + (b - a) * clamp01(t); }
/// De a a b segun t sin recortar (t puede salir de 0..1).
inline float lerpUnclamped(float a, float b, float t) { return a + (b - a) * t; }
/// El t que da v entre a y b (0..1).
inline float inverseLerp(float a, float b, float v) { return a != b ? clamp01((v - a) / (b - a)) : 0.0f; }
/// Lleva v del rango [a1, b1] al rango [a2, b2].
inline float remap(float v, float a1, float b1, float a2, float b2) {
    return a1 != b1 ? a2 + (v - a1) * (b2 - a2) / (b1 - a1) : a2;
}
/// t repetido entre 0 y length (como el modulo, tambien con negativos).
inline float repeat(float t, float length) { return length != 0.0f ? clamp(t - std::floor(t / length) * length, 0.0f, length) : 0.0f; }
/// v dentro de [lo, hi) dando la vuelta.
inline float wrap(float v, float lo, float hi) { return lo + repeat(v - lo, hi - lo); }
/// Va y vuelve entre 0 y length.
inline float pingPong(float t, float length) {
    t = repeat(t, length * 2.0f);
    return length - std::fabs(t - length);
}
/// Diferencia mas corta entre dos angulos (grados, -180..180).
inline float deltaAngle(float current, float target) {
    float d = repeat(target - current, 360.0f);
    if (d > 180.0f) d -= 360.0f;
    return d;
}
/// Como lerp pero por el camino corto entre angulos (grados).
inline float lerpAngle(float a, float b, float t) { return a + deltaAngle(a, b) * clamp01(t); }
/// Acerca current a target como mucho maxDelta.
inline float moveTowards(float current, float target, float maxDelta) {
    if (std::fabs(target - current) <= maxDelta) return target;
    return current + sign(target - current) * maxDelta;
}
/// moveTowards entre angulos (grados).
inline float moveTowardsAngle(float current, float target, float maxDelta) {
    const float d = deltaAngle(current, target);
    if (-maxDelta < d && d < maxDelta) return target;
    return moveTowards(current, current + d, maxDelta);
}
/// Curva suave 0..1 (3t^2 - 2t^3) entre from y to.
inline float smoothstep(float from, float to, float t) {
    t = clamp01(t);
    t = t * t * (3.0f - 2.0f * t);
    return from + (to - from) * t;
}
/// Curva aun mas suave (6t^5 - 15t^4 + 10t^3).
inline float smootherstep(float from, float to, float t) {
    t = clamp01(t);
    t = t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
    return from + (to - from) * t;
}
/// Sigue a target con suavidad (muelle amortiguado). velocity se guarda entre frames.
inline float smoothDamp(float current, float target, float& velocity, float smoothTime, float deltaTime,
                        float maxSpeed = infinity) {
    smoothTime = max(0.0001f, smoothTime);
    const float omega = 2.0f / smoothTime;
    const float x = omega * deltaTime;
    const float e = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
    float change = current - target;
    const float original = target;
    const float maxChange = maxSpeed * smoothTime;
    change = clamp(change, -maxChange, maxChange);
    target = current - change;
    const float temp = (velocity + omega * change) * deltaTime;
    velocity = (velocity - omega * temp) * e;
    float out = target + (change + temp) * e;
    if ((original - current > 0.0f) == (out > original)) {
        out = original;
        velocity = (out - original) / deltaTime;
    }
    return out;
}
/// a y b casi iguales.
inline bool approximately(float a, float b) { return std::fabs(a - b) < max(1e-6f * max(std::fabs(a), std::fabs(b)), 1e-6f); }
inline float round(float v) { return std::round(v); }
inline float floor(float v) { return std::floor(v); }
inline float ceil(float v) { return std::ceil(v); }
inline int roundToInt(float v) { return static_cast<int>(std::lround(v)); }
inline int floorToInt(float v) { return static_cast<int>(std::floor(v)); }
inline int ceilToInt(float v) { return static_cast<int>(std::ceil(v)); }
inline float sqrt(float v) { return std::sqrt(v); }
inline float pow(float v, float p) { return std::pow(v, p); }
inline float exp(float v) { return std::exp(v); }
inline float log(float v) { return std::log(v); }
inline float log10(float v) { return std::log10(v); }
/// Seno (radianes).
inline float sin(float r) { return std::sin(r); }
/// Coseno (radianes).
inline float cos(float r) { return std::cos(r); }
inline float tan(float r) { return std::tan(r); }
inline float asin(float v) { return std::asin(clamp(v, -1.0f, 1.0f)); }
inline float acos(float v) { return std::acos(clamp(v, -1.0f, 1.0f)); }
inline float atan(float v) { return std::atan(v); }
/// Angulo (radianes) del punto (x, y).
inline float atan2(float y, float x) { return std::atan2(y, x); }
inline bool isPowerOfTwo(int v) { return v > 0 && (v & (v - 1)) == 0; }
inline int nextPowerOfTwo(int v) {
    int p = 1;
    while (p < v) p <<= 1;
    return p;
}


}  // namespace Mathf

namespace math_detail {
inline const std::uint8_t* perm() {
    struct Table {
        std::uint8_t v[512];
        Table() {
            std::uint32_t s = 1337u;
            std::uint8_t base[256];
            for (int i = 0; i < 256; ++i) base[i] = static_cast<std::uint8_t>(i);
            for (int i = 255; i > 0; --i) {
                s = s * 1664525u + 1013904223u;
                const int j = static_cast<int>((s >> 8) % static_cast<std::uint32_t>(i + 1));
                std::swap(base[i], base[j]);
            }
            for (int i = 0; i < 512; ++i) v[i] = base[i & 255];
        }
    };
    static const Table table;
    return table.v;
}
inline float fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }
inline float grad(int h, float x, float y, float z) {
    h &= 15;
    const float u = h < 8 ? x : y;
    const float v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
    return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
}
}  // namespace math_detail

namespace Mathf {
/// Ruido de Perlin 3D (-1..1 aprox.).
inline float perlinNoise3(float x, float y, float z) {
    const std::uint8_t* p = math_detail::perm();
    const int X = static_cast<int>(std::floor(x)) & 255, Y = static_cast<int>(std::floor(y)) & 255,
              Z = static_cast<int>(std::floor(z)) & 255;
    x -= std::floor(x);
    y -= std::floor(y);
    z -= std::floor(z);
    const float u = math_detail::fade(x), v = math_detail::fade(y), w = math_detail::fade(z);
    const int A = p[X] + Y, AA = p[A] + Z, AB = p[A + 1] + Z, B = p[X + 1] + Y, BA = p[B] + Z, BB = p[B + 1] + Z;
    const auto L = [](float t, float a, float b) { return a + t * (b - a); };
    return L(w,
             L(v, L(u, math_detail::grad(p[AA], x, y, z), math_detail::grad(p[BA], x - 1, y, z)),
               L(u, math_detail::grad(p[AB], x, y - 1, z), math_detail::grad(p[BB], x - 1, y - 1, z))),
             L(v, L(u, math_detail::grad(p[AA + 1], x, y, z - 1), math_detail::grad(p[BA + 1], x - 1, y, z - 1)),
               L(u, math_detail::grad(p[AB + 1], x, y - 1, z - 1), math_detail::grad(p[BB + 1], x - 1, y - 1, z - 1))));
}
/// Ruido de Perlin 2D en 0..1 (como Mathf.PerlinNoise de Unity).
inline float perlinNoise(float x, float y) { return clamp01(perlinNoise3(x, y, 0.5f) * 0.5f + 0.5f); }
/// Suma de varias capas de ruido (montanas, nubes...), en 0..1.
inline float fractalNoise(float x, float y, int octaves = 4, float persistence = 0.5f, float lacunarity = 2.0f) {
    float sum = 0.0f, amp = 1.0f, freq = 1.0f, norm = 0.0f;
    for (int i = 0; i < octaves; ++i) {
        sum += perlinNoise(x * freq, y * freq) * amp;
        norm += amp;
        amp *= persistence;
        freq *= lacunarity;
    }
    return norm > 0.0f ? sum / norm : 0.0f;
}
}  // namespace Mathf

/// Vector 2D (UV, pantalla, entradas de un stick...).
struct Vec2 {
    float x = 0.0f, y = 0.0f;
    constexpr Vec2() = default;
    constexpr Vec2(float x_, float y_) : x(x_), y(y_) {}
    constexpr Vec2 operator+(const Vec2& o) const { return {x + o.x, y + o.y}; }
    constexpr Vec2 operator-(const Vec2& o) const { return {x - o.x, y - o.y}; }
    constexpr Vec2 operator-() const { return {-x, -y}; }
    constexpr Vec2 operator*(float s) const { return {x * s, y * s}; }
    constexpr Vec2 operator/(float s) const { return {x / s, y / s}; }
    constexpr bool operator==(const Vec2& o) const { return x == o.x && y == o.y; }
    Vec2& operator+=(const Vec2& o) { return *this = *this + o; }
    Vec2& operator-=(const Vec2& o) { return *this = *this - o; }
    Vec2& operator*=(float s) { return *this = *this * s; }
    float length() const { return std::sqrt(x * x + y * y); }
    constexpr float sqrLength() const { return x * x + y * y; }
    Vec2 normalized() const {
        const float l = length();
        return l > 1e-8f ? *this / l : Vec2{};
    }
    static constexpr float dot(const Vec2& a, const Vec2& b) { return a.x * b.x + a.y * b.y; }
    static float distance(const Vec2& a, const Vec2& b) { return (a - b).length(); }
    static Vec2 lerp(const Vec2& a, const Vec2& b, float t) { return a + (b - a) * Mathf::clamp01(t); }
    static constexpr Vec2 zero() { return {}; }
    static constexpr Vec2 one() { return {1, 1}; }
    static constexpr Vec2 up() { return {0, 1}; }
    static constexpr Vec2 right() { return {1, 0}; }
};
inline constexpr Vec2 operator*(float s, const Vec2& v) { return v * s; }

/// Vector 3D (posiciones, direcciones, escalas...). El "delante" del motor es -Z.
struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    constexpr Vec3() = default;
    constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    constexpr Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    constexpr Vec3 operator-() const { return {-x, -y, -z}; }
    constexpr Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    constexpr Vec3 operator/(float s) const { return {x / s, y / s, z / s}; }
    constexpr bool operator==(const Vec3& o) const { return x == o.x && y == o.y && z == o.z; }
    constexpr bool operator!=(const Vec3& o) const { return !(*this == o); }
    Vec3& operator+=(const Vec3& o) { return *this = *this + o; }
    Vec3& operator-=(const Vec3& o) { return *this = *this - o; }
    Vec3& operator*=(float s) { return *this = *this * s; }
    Vec3& operator/=(float s) { return *this = *this / s; }
    /// Largo (magnitud).
    float length() const { return std::sqrt(x * x + y * y + z * z); }
    /// Largo al cuadrado (mas rapido para comparar distancias).
    constexpr float sqrLength() const { return x * x + y * y + z * z; }
    /// El mismo vector con largo 1 (o cero si es cero).
    Vec3 normalized() const {
        const float l = length();
        return l > 1e-8f ? *this / l : Vec3{};
    }
    static constexpr float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    static constexpr Vec3 cross(const Vec3& a, const Vec3& b) {
        return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
    }
    /// De a a b segun t (sin recortar, como antes).
    static Vec3 lerp(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }
    static Vec3 lerpClamped(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * Mathf::clamp01(t); }
    static float distance(const Vec3& a, const Vec3& b) { return (a - b).length(); }
    static constexpr float sqrDistance(const Vec3& a, const Vec3& b) { return (a - b).sqrLength(); }
    /// Componente a componente (escala no uniforme).
    static constexpr Vec3 scale(const Vec3& a, const Vec3& b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
    static constexpr Vec3 min(const Vec3& a, const Vec3& b) {
        return {a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z};
    }
    static constexpr Vec3 max(const Vec3& a, const Vec3& b) {
        return {a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z};
    }
    /// Angulo entre dos direcciones (grados, 0..180).
    static float angle(const Vec3& a, const Vec3& b) {
        const float d = std::sqrt(a.sqrLength() * b.sqrLength());
        return d < 1e-12f ? 0.0f : Mathf::acos(dot(a, b) / d) * Mathf::rad2deg;
    }
    /// Angulo con signo alrededor de axis (grados, -180..180).
    static float signedAngle(const Vec3& from, const Vec3& to, const Vec3& axis) {
        const float a = angle(from, to);
        return dot(axis, cross(from, to)) < 0.0f ? -a : a;
    }
    /// Acerca current a target como mucho maxDistance.
    static Vec3 moveTowards(const Vec3& current, const Vec3& target, float maxDistance) {
        const Vec3 d = target - current;
        const float l = d.length();
        return l <= maxDistance || l < 1e-8f ? target : current + d / l * maxDistance;
    }
    /// Proyeccion de v sobre una direccion.
    static Vec3 project(const Vec3& v, const Vec3& onNormal) {
        const float s = onNormal.sqrLength();
        return s < 1e-12f ? Vec3{} : onNormal * (dot(v, onNormal) / s);
    }
    /// v sin la parte de la normal del plano (deslizar por una pared).
    static Vec3 projectOnPlane(const Vec3& v, const Vec3& planeNormal) { return v - project(v, planeNormal); }
    /// Rebote de una direccion en una superficie.
    static Vec3 reflect(const Vec3& dir, const Vec3& normal) { return dir - normal * (2.0f * dot(dir, normal)); }
    /// El mismo vector con un largo maximo.
    static Vec3 clampLength(const Vec3& v, float maxLength) {
        const float l = v.length();
        return l > maxLength && l > 1e-8f ? v * (maxLength / l) : v;
    }
    /// Interpolacion esferica entre direcciones (gira en vez de cortar camino).
    static Vec3 slerp(const Vec3& a, const Vec3& b, float t) {
        t = Mathf::clamp01(t);
        const float la = a.length(), lb = b.length();
        if (la < 1e-8f || lb < 1e-8f) return lerp(a, b, t);
        const Vec3 na = a / la, nb = b / lb;
        const float d = Mathf::clamp(dot(na, nb), -1.0f, 1.0f);
        const float theta = std::acos(d) * t;
        Vec3 rel = (nb - na * d).normalized();
        if (rel.sqrLength() < 1e-12f) return lerp(a, b, t);
        return (na * std::cos(theta) + rel * std::sin(theta)) * Mathf::lerpUnclamped(la, lb, t);
    }
    /// Sigue a target con suavidad. velocity se guarda entre frames.
    static Vec3 smoothDamp(const Vec3& current, const Vec3& target, Vec3& velocity, float smoothTime, float deltaTime,
                           float maxSpeed = Mathf::infinity) {
        return {Mathf::smoothDamp(current.x, target.x, velocity.x, smoothTime, deltaTime, maxSpeed),
                Mathf::smoothDamp(current.y, target.y, velocity.y, smoothTime, deltaTime, maxSpeed),
                Mathf::smoothDamp(current.z, target.z, velocity.z, smoothTime, deltaTime, maxSpeed)};
    }
    static bool approximately(const Vec3& a, const Vec3& b) { return (a - b).sqrLength() < 1e-10f; }
    Vec3 abs() const { return {std::fabs(x), std::fabs(y), std::fabs(z)}; }
    Vec3 floor() const { return {std::floor(x), std::floor(y), std::floor(z)}; }
    Vec3 ceil() const { return {std::ceil(x), std::ceil(y), std::ceil(z)}; }
    Vec3 round() const { return {std::round(x), std::round(y), std::round(z)}; }
    static constexpr Vec3 zero() { return {}; }
    static constexpr Vec3 one() { return {1, 1, 1}; }
    static constexpr Vec3 up() { return {0, 1, 0}; }
    static constexpr Vec3 down() { return {0, -1, 0}; }
    static constexpr Vec3 forward() { return {0, 0, -1}; }  // el "delante" del motor
    static constexpr Vec3 back() { return {0, 0, 1}; }
    static constexpr Vec3 right() { return {1, 0, 0}; }
    static constexpr Vec3 left() { return {-1, 0, 0}; }
};
inline constexpr Vec3 operator*(float s, const Vec3& v) { return v * s; }

/// Rotacion (cuaternion). Angulos en grados, en el mismo orden que el Inspector.
struct Quat {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
    constexpr Quat() = default;
    constexpr Quat(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    /// Sin rotacion.
    static constexpr Quat identity() { return {}; }
    /// Desde grados (X, Y, Z), el mismo orden que el Inspector.
    static Quat euler(const Vec3& degrees) {
        const float k = 3.14159265f / 360.0f;
        const float cx = std::cos(degrees.x * k), sx = std::sin(degrees.x * k);
        const float cy = std::cos(degrees.y * k), sy = std::sin(degrees.y * k);
        const float cz = std::cos(degrees.z * k), sz = std::sin(degrees.z * k);
        return {sx * cy * cz - cx * sy * sz, cx * sy * cz + sx * cy * sz, cx * cy * sz - sx * sy * cz, cx * cy * cz + sx * sy * sz};
    }
    static Quat euler(float x, float y, float z) { return euler(Vec3{x, y, z}); }
    /// Los grados (X, Y, Z) de esta rotacion (lo contrario de euler).
    Vec3 toEuler() const {
        const float sinp = Mathf::clamp(2.0f * (w * y - z * x), -1.0f, 1.0f);
        return Vec3{std::atan2(2.0f * (w * x + y * z), 1.0f - 2.0f * (x * x + y * y)), std::asin(sinp),
                    std::atan2(2.0f * (w * z + x * y), 1.0f - 2.0f * (y * y + z * z))} *
               Mathf::rad2deg;
    }
    /// Girar degrees alrededor de un eje.
    static Quat angleAxis(float degrees, const Vec3& axis) {
        const Vec3 n = axis.normalized();
        const float h = degrees * Mathf::deg2rad * 0.5f;
        const float s = std::sin(h);
        return {n.x * s, n.y * s, n.z * s, std::cos(h)};
    }
    /// Mirar hacia forward (el delante del motor es -Z) con up hacia arriba.
    static Quat lookRotation(const Vec3& forward, const Vec3& up = Vec3::up()) {
        const Vec3 zb = (-forward).normalized();  // el eje +Z apunta hacia atras
        if (zb.sqrLength() < 1e-12f) return {};
        Vec3 xb = Vec3::cross(up, zb).normalized();
        if (xb.sqrLength() < 1e-12f) xb = Vec3::cross(Vec3::right(), zb).normalized();
        const Vec3 yb = Vec3::cross(zb, xb);
        const float m00 = xb.x, m11 = yb.y, m22 = zb.z;
        const float tr = m00 + m11 + m22;
        Quat q;
        if (tr > 0.0f) {
            const float s = std::sqrt(tr + 1.0f) * 2.0f;
            q = {(yb.z - zb.y) / s, (zb.x - xb.z) / s, (xb.y - yb.x) / s, 0.25f * s};
        } else if (m00 > m11 && m00 > m22) {
            const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
            q = {0.25f * s, (yb.x + xb.y) / s, (zb.x + xb.z) / s, (yb.z - zb.y) / s};
        } else if (m11 > m22) {
            const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
            q = {(yb.x + xb.y) / s, 0.25f * s, (zb.y + yb.z) / s, (zb.x - xb.z) / s};
        } else {
            const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
            q = {(zb.x + xb.z) / s, (zb.y + yb.z) / s, 0.25f * s, (xb.y - yb.x) / s};
        }
        return q.normalized();
    }
    /// La rotacion que lleva la direccion from a la direccion to.
    static Quat fromToRotation(const Vec3& from, const Vec3& to) {
        const Vec3 a = from.normalized(), b = to.normalized();
        const float d = Vec3::dot(a, b);
        if (d < -0.999999f) {
            Vec3 axis = Vec3::cross(Vec3::right(), a);
            if (axis.sqrLength() < 1e-12f) axis = Vec3::cross(Vec3::up(), a);
            return angleAxis(180.0f, axis);
        }
        const Vec3 c = Vec3::cross(a, b);
        return Quat{c.x, c.y, c.z, 1.0f + d}.normalized();
    }
    static constexpr float dot(const Quat& a, const Quat& b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
    /// Angulo entre dos rotaciones (grados).
    static float angle(const Quat& a, const Quat& b) {
        const float d = std::fabs(dot(a.normalized(), b.normalized()));
        return d > 0.999999f ? 0.0f : 2.0f * std::acos(Mathf::clamp(d, -1.0f, 1.0f)) * Mathf::rad2deg;
    }
    Quat normalized() const {
        const float l = std::sqrt(x * x + y * y + z * z + w * w);
        return l > 1e-8f ? Quat{x / l, y / l, z / l, w / l} : Quat{};
    }
    /// La rotacion contraria.
    Quat inverse() const {
        const float n = x * x + y * y + z * z + w * w;
        return n > 1e-12f ? Quat{-x / n, -y / n, -z / n, w / n} : Quat{};
    }
    /// Interpolacion esferica (suave y a velocidad constante).
    static Quat slerp(const Quat& a, Quat b, float t) {
        t = Mathf::clamp01(t);
        float d = dot(a, b);
        if (d < 0.0f) {
            b = {-b.x, -b.y, -b.z, -b.w};
            d = -d;
        }
        if (d > 0.9995f) return lerp(a, b, t);
        const float theta = std::acos(d), s = std::sin(theta);
        const float wa = std::sin((1.0f - t) * theta) / s, wb = std::sin(t * theta) / s;
        return Quat{a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb}.normalized();
    }
    /// Interpolacion lineal normalizada (mas rapida que slerp).
    static Quat lerp(const Quat& a, const Quat& b, float t) {
        t = Mathf::clamp01(t);
        const float s = dot(a, b) < 0.0f ? -1.0f : 1.0f;
        return Quat{a.x + (b.x * s - a.x) * t, a.y + (b.y * s - a.y) * t, a.z + (b.z * s - a.z) * t, a.w + (b.w * s - a.w) * t}
            .normalized();
    }
    /// Gira from hacia to como mucho maxDegrees.
    static Quat rotateTowards(const Quat& from, const Quat& to, float maxDegrees) {
        const float a = angle(from, to);
        return a < 1e-4f ? to : slerp(from, to, Mathf::min(1.0f, maxDegrees / a));
    }
    Quat operator*(const Quat& b) const {
        return {w * b.x + x * b.w + y * b.z - z * b.y, w * b.y - x * b.z + y * b.w + z * b.x,
                w * b.z + x * b.y - y * b.x + z * b.w, w * b.w - x * b.x - y * b.y - z * b.z};
    }
    /// Gira un vector.
    Vec3 operator*(const Vec3& v) const {
        const Vec3 q{x, y, z};
        const Vec3 t = Vec3::cross(q, v) * 2.0f;
        return v + t * w + Vec3::cross(q, t);
    }
    /// Su delante (-Z girado), derecha y arriba.
    Vec3 forward() const { return *this * Vec3::forward(); }
    Vec3 right() const { return *this * Vec3::right(); }
    Vec3 up() const { return *this * Vec3::up(); }
};

/// Color RGBA (0..1).
struct Color {
    float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
    constexpr Color() = default;
    constexpr Color(float r_, float g_, float b_, float a_ = 1.0f) : r(r_), g(g_), b(b_), a(a_) {}
    constexpr Vec3 rgb() const { return {r, g, b}; }
    constexpr Color operator*(float s) const { return {r * s, g * s, b * s, a}; }
    static Color lerp(const Color& x, const Color& y, float t) {
        t = Mathf::clamp01(t);
        return {x.r + (y.r - x.r) * t, x.g + (y.g - x.g) * t, x.b + (y.b - x.b) * t, x.a + (y.a - x.a) * t};
    }
    /// Desde tono (0..1), saturacion y valor.
    static Color hsv(float h, float s, float v) {
        h = Mathf::repeat(h, 1.0f) * 6.0f;
        const int i = static_cast<int>(h);
        const float f = h - static_cast<float>(i), p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
        switch (i % 6) {
            case 0: return {v, t, p};
            case 1: return {q, v, p};
            case 2: return {p, v, t};
            case 3: return {p, q, v};
            case 4: return {t, p, v};
            default: return {v, p, q};
        }
    }
    static constexpr Color white() { return {1, 1, 1}; }
    static constexpr Color black() { return {0, 0, 0}; }
    static constexpr Color red() { return {1, 0, 0}; }
    static constexpr Color green() { return {0, 1, 0}; }
    static constexpr Color blue() { return {0, 0, 1}; }
    static constexpr Color yellow() { return {1, 0.92f, 0.016f}; }
    static constexpr Color clear() { return {0, 0, 0, 0}; }
};

}  // namespace cramion

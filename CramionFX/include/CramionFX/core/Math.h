#ifndef CRAMION_CORE_MATH_H
#define CRAMION_CORE_MATH_H

#include <cmath>

namespace cramion::core {

inline constexpr float kPi = 3.14159265358979323846f;

inline constexpr float radians(float degrees) {
    return degrees * (kPi / 180.0f);
}

// -----------------------------------------------------------------------------
// Vectores
// -----------------------------------------------------------------------------

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    constexpr Vec3() = default;
    constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    explicit constexpr Vec3(float s) : x(s), y(s), z(s) {}
};

struct Vec4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;

    constexpr Vec4() = default;
    constexpr Vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    constexpr Vec4(const Vec3& v, float w_) : x(v.x), y(v.y), z(v.z), w(w_) {}
};

constexpr Vec3 operator+(const Vec3& a, const Vec3& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
constexpr Vec3 operator-(const Vec3& a, const Vec3& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
constexpr Vec3 operator-(const Vec3& v) {
    return {-v.x, -v.y, -v.z};
}
constexpr Vec3 operator*(const Vec3& v, float s) {
    return {v.x * s, v.y * s, v.z * s};
}
constexpr Vec3 operator*(float s, const Vec3& v) {
    return v * s;
}
constexpr Vec3 operator*(const Vec3& a, const Vec3& b) {
    return {a.x * b.x, a.y * b.y, a.z * b.z};
}
constexpr Vec3& operator+=(Vec3& a, const Vec3& b) {
    a = a + b;
    return a;
}
constexpr Vec3& operator-=(Vec3& a, const Vec3& b) {
    a = a - b;
    return a;
}
constexpr Vec3& operator*=(Vec3& v, float s) {
    v = v * s;
    return v;
}

constexpr float dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

constexpr Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline float length(const Vec3& v) {
    return std::sqrt(dot(v, v));
}

inline Vec3 normalize(const Vec3& v) {
    const float len = length(v);
    return (len > 1e-6f) ? v * (1.0f / len) : Vec3{};
}

constexpr Vec3 lerp(const Vec3& a, const Vec3& b, float t) {
    return a + (b - a) * t;
}

// -----------------------------------------------------------------------------
// Matriz 4x4, almacenada por columnas igual que GLSL: m[columna][fila].
// -----------------------------------------------------------------------------

struct Mat4 {
    float m[4][4] = {};

    static constexpr Mat4 identity() {
        Mat4 result{};
        result.m[0][0] = 1.0f;
        result.m[1][1] = 1.0f;
        result.m[2][2] = 1.0f;
        result.m[3][3] = 1.0f;
        return result;
    }
};

constexpr Mat4 operator*(const Mat4& a, const Mat4& b) {
    Mat4 result{};
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a.m[k][row] * b.m[col][k];
            }
            result.m[col][row] = sum;
        }
    }
    return result;
}

constexpr Vec4 operator*(const Mat4& a, const Vec4& v) {
    return {a.m[0][0] * v.x + a.m[1][0] * v.y + a.m[2][0] * v.z + a.m[3][0] * v.w,
            a.m[0][1] * v.x + a.m[1][1] * v.y + a.m[2][1] * v.z + a.m[3][1] * v.w,
            a.m[0][2] * v.x + a.m[1][2] * v.y + a.m[2][2] * v.z + a.m[3][2] * v.w,
            a.m[0][3] * v.x + a.m[1][3] * v.y + a.m[2][3] * v.z + a.m[3][3] * v.w};
}

// Inversa general de una matriz 4x4 por cofactores. Se calcula una vez por
// frame en la CPU (la camara) para no tener que invertir nada por pixel en el
// shader de iluminacion.
inline Mat4 inverse(const Mat4& matrix) {
    // Vista plana en orden por columnas, que es como se almacena Mat4.
    const float* m = &matrix.m[0][0];
    float inv[16];

    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] +
             m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] -
             m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] +
             m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] -
              m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] -
             m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] +
             m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] -
             m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] +
              m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] +
             m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] -
             m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] +
              m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] -
              m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] -
             m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] +
             m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] -
              m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] +
              m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];

    const float determinant = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (std::abs(determinant) < 1e-12f) {
        return Mat4::identity();  // Matriz singular: no hay inversa.
    }

    const float inv_determinant = 1.0f / determinant;

    Mat4 result{};
    float* out = &result.m[0][0];
    for (int i = 0; i < 16; ++i) {
        out[i] = inv[i] * inv_determinant;
    }
    return result;
}

constexpr Mat4 translate(const Vec3& t) {
    Mat4 result = Mat4::identity();
    result.m[3][0] = t.x;
    result.m[3][1] = t.y;
    result.m[3][2] = t.z;
    return result;
}

constexpr Mat4 scale(const Vec3& s) {
    Mat4 result = Mat4::identity();
    result.m[0][0] = s.x;
    result.m[1][1] = s.y;
    result.m[2][2] = s.z;
    return result;
}

// Proyeccion en perspectiva para Vulkan: espacio de vista diestro (mira hacia
// -Z) y profundidad de clip en el rango [0, 1]. El eje Y se invierte aqui
// porque en Vulkan las Y de la pantalla crecen hacia abajo.
inline Mat4 perspective(float fov_y_radians, float aspect, float near_plane, float far_plane) {
    const float f = 1.0f / std::tan(fov_y_radians * 0.5f);

    Mat4 result{};
    result.m[0][0] = f / aspect;
    result.m[1][1] = -f;
    result.m[2][2] = far_plane / (near_plane - far_plane);
    result.m[2][3] = -1.0f;
    result.m[3][2] = (far_plane * near_plane) / (near_plane - far_plane);
    return result;
}

// Proyeccion ortografica para Vulkan, con la misma convencion que
// perspective(): profundidad en [0, 1] y eje Y invertido. La usan las cascadas
// de sombras, donde la luz direccional proyecta en paralelo.
inline Mat4 orthographic(float left, float right, float bottom, float top, float near_plane,
                         float far_plane) {
    Mat4 result = Mat4::identity();
    result.m[0][0] = 2.0f / (right - left);
    result.m[1][1] = -2.0f / (top - bottom);
    result.m[2][2] = -1.0f / (far_plane - near_plane);
    result.m[3][0] = -(right + left) / (right - left);
    result.m[3][1] = (top + bottom) / (top - bottom);
    result.m[3][2] = -near_plane / (far_plane - near_plane);
    return result;
}

// -----------------------------------------------------------------------------
// Cuaterniones (rotaciones de las animaciones esqueleticas)
// -----------------------------------------------------------------------------

struct Quat {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

inline Quat normalize(const Quat& q) {
    const float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (len < 1e-8f) {
        return Quat{};
    }
    const float inv = 1.0f / len;
    return {q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

// Interpolacion esferica: velocidad angular constante entre dos rotaciones.
// Si los cuaterniones apuntan a hemisferios opuestos se invierte uno para
// tomar el camino corto; si estan casi alineados basta una interpolacion
// lineal (slerp dividiria por un seno casi nulo).
inline Quat slerp(const Quat& a, Quat b, float t) {
    float cosine = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (cosine < 0.0f) {
        b = {-b.x, -b.y, -b.z, -b.w};
        cosine = -cosine;
    }

    float wa = 1.0f - t;
    float wb = t;
    if (cosine < 0.9995f) {
        const float angle = std::acos(cosine);
        const float inv_sin = 1.0f / std::sin(angle);
        wa = std::sin((1.0f - t) * angle) * inv_sin;
        wb = std::sin(t * angle) * inv_sin;
    }

    return normalize(Quat{a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb,
                          a.w * wa + b.w * wb});
}

// Traslacion * rotacion * escala en una sola matriz (el orden habitual de
// las transformaciones locales de un nodo animado).
inline Mat4 composeTrs(const Vec3& translation, const Quat& rotation, const Vec3& scaling) {
    const Quat q = normalize(rotation);
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;

    Mat4 result{};
    result.m[0][0] = (1.0f - 2.0f * (yy + zz)) * scaling.x;
    result.m[0][1] = (2.0f * (xy + wz)) * scaling.x;
    result.m[0][2] = (2.0f * (xz - wy)) * scaling.x;
    result.m[1][0] = (2.0f * (xy - wz)) * scaling.y;
    result.m[1][1] = (1.0f - 2.0f * (xx + zz)) * scaling.y;
    result.m[1][2] = (2.0f * (yz + wx)) * scaling.y;
    result.m[2][0] = (2.0f * (xz + wy)) * scaling.z;
    result.m[2][1] = (2.0f * (yz - wx)) * scaling.z;
    result.m[2][2] = (1.0f - 2.0f * (xx + yy)) * scaling.z;
    result.m[3][0] = translation.x;
    result.m[3][1] = translation.y;
    result.m[3][2] = translation.z;
    result.m[3][3] = 1.0f;
    return result;
}

// Matriz de vista diestra.
inline Mat4 lookAt(const Vec3& eye, const Vec3& center, const Vec3& up) {
    const Vec3 f = normalize(center - eye);
    const Vec3 s = normalize(cross(f, up));
    const Vec3 u = cross(s, f);

    Mat4 result = Mat4::identity();
    result.m[0][0] = s.x;
    result.m[1][0] = s.y;
    result.m[2][0] = s.z;
    result.m[0][1] = u.x;
    result.m[1][1] = u.y;
    result.m[2][1] = u.z;
    result.m[0][2] = -f.x;
    result.m[1][2] = -f.y;
    result.m[2][2] = -f.z;
    result.m[3][0] = -dot(s, eye);
    result.m[3][1] = -dot(u, eye);
    result.m[3][2] = dot(f, eye);
    return result;
}

}  // namespace cramion::core

#endif  // CRAMION_CORE_MATH_H

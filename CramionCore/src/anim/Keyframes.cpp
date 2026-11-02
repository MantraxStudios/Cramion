// Animacion por fotogramas clave (anim/Keyframes.h): curvas Bezier como las
// F-Curves de Blender, acceso a propiedades por ruta con la reflexion de los
// componentes y el sistema que las hace sonar en Play.

#include "CramionCore/anim/Keyframes.h"

#include "CramionCore/ecs/Reflection.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>

namespace cramion::anim {

using core::Vec2;
using core::Vec3;

namespace {

constexpr float kEpsilon = 1e-6f;

// --- Bezier -------------------------------------------------------------------

float cubic(float p0, float p1, float p2, float p3, float u) {
    const float iu = 1.0f - u;
    return iu * iu * iu * p0 + 3.0f * iu * iu * u * p1 + 3.0f * iu * u * u * p2 + u * u * u * p3;
}

float cubicDerivative(float p0, float p1, float p2, float p3, float u) {
    const float iu = 1.0f - u;
    return 3.0f * iu * iu * (p1 - p0) + 6.0f * iu * u * (p2 - p1) + 3.0f * u * u * (p3 - p2);
}

// Tramo Bezier entre dos claves en `time`. Las asas se recortan al tramo (como
// hace Blender) para que la curva no vuelva atras en el tiempo.
float evaluateBezier(const Keyframe& a, const Keyframe& b, float time) {
    const float span = b.time - a.time;
    if (span <= kEpsilon) return b.value;
    Vec2 h1 = a.right;
    Vec2 h2 = b.left;
    const float len1 = h1.x - a.time;
    const float len2 = b.time - h2.x;
    if (len1 < 0.0f || len2 < 0.0f || len1 + len2 > span) {
        // Asas que se cruzan: se escalan para que quepan.
        const float l1 = std::max(len1, 0.0f);
        const float l2 = std::max(len2, 0.0f);
        const float scale = l1 + l2 > span ? span / (l1 + l2) : 1.0f;
        const float k1 = len1 > kEpsilon ? l1 * scale / len1 : 0.0f;
        const float k2 = len2 > kEpsilon ? l2 * scale / len2 : 0.0f;
        h1 = Vec2{a.time + (h1.x - a.time) * k1, a.value + (h1.y - a.value) * k1};
        h2 = Vec2{b.time - (b.time - h2.x) * k2, b.value + (h2.y - b.value) * k2};
    }
    // u tal que x(u) = time: Newton con caida a biseccion.
    float lo = 0.0f;
    float hi = 1.0f;
    float u = (time - a.time) / span;
    for (int i = 0; i < 24; ++i) {
        const float x = cubic(a.time, h1.x, h2.x, b.time, u) - time;
        if (std::abs(x) < 1e-6f * std::max(1.0f, span)) break;
        if (x > 0.0f) hi = u;
        else lo = u;
        const float d = cubicDerivative(a.time, h1.x, h2.x, b.time, u);
        float next = std::abs(d) > kEpsilon ? u - x / d : -1.0f;
        if (next <= lo || next >= hi) next = 0.5f * (lo + hi);
        u = next;
    }
    return cubic(a.value, h1.y, h2.y, b.value, u);
}

// Pendiente con la que sale la curva de una clave extrema (extrapolacion lineal).
float endSlope(const std::vector<Keyframe>& keys, bool first) {
    if (keys.size() < 2) return 0.0f;
    const Keyframe& k = first ? keys.front() : keys.back();
    const Keyframe& n = first ? keys[1] : keys[keys.size() - 2];
    const Keyframe& seg = first ? keys.front() : keys[keys.size() - 2];
    if (seg.interpolation == Interpolation::Constant) return 0.0f;
    if (seg.interpolation == Interpolation::Bezier) {
        const Vec2 h = first ? k.right : k.left;
        const float dx = first ? h.x - k.time : k.time - h.x;
        if (std::abs(dx) > kEpsilon) return first ? (h.y - k.value) / dx : (k.value - h.y) / dx;
    }
    const float dt = n.time - k.time;
    return std::abs(dt) > kEpsilon ? (n.value - k.value) / dt : 0.0f;
}

bool isAuto(HandleType t) { return t == HandleType::Auto || t == HandleType::AutoClamped; }

// --- Ruta de un campo -----------------------------------------------------------

// Como FieldPath del editor: grupos por etiqueta, listas por clave e indice.
class PathVisitor : public ecs::PropertyVisitor {
public:
    bool beginGroup(const char* label, bool) override {
        stack_.emplace_back(label != nullptr ? label : "");
        labels_.emplace_back(label != nullptr ? label : "");
        return true;
    }
    void endGroup() override { pop(); }
    bool field(const ecs::Meta&, std::string&) override { return false; }
    bool asset(const ecs::Meta&, assets::AssetRef&, assets::AssetType) override { return false; }
    bool entity(const ecs::Meta&, Uuid&) override { return false; }
    bool layerMask(const ecs::Meta&, std::uint32_t&) override { return false; }
    bool beginList(const ecs::Meta& meta, std::size_t&) override {
        stack_.emplace_back(meta.key);
        labels_.emplace_back(meta.label);
        return true;
    }
    bool beginListItem(std::size_t index) override {
        stack_.push_back(std::to_string(index));
        labels_.push_back(std::to_string(index));
        return true;
    }
    void endListItem() override { pop(); }
    int endList() override {
        pop();
        return -1;
    }

protected:
    std::string pathOf(const char* key) const {
        std::string path;
        for (const std::string& part : stack_) {
            path += part;
            path += '/';
        }
        path += key;
        return path;
    }
    std::string labelOf(const char* label) const {
        std::string text;
        for (const std::string& part : labels_) {
            text += part;
            text += " / ";
        }
        text += label;
        return text;
    }
    void pop() {
        if (!stack_.empty()) stack_.pop_back();
        if (!labels_.empty()) labels_.pop_back();
    }

private:
    std::vector<std::string> stack_;
    std::vector<std::string> labels_;
};

// Lista los campos animables (los que ve el Inspector).
class ListVisitor final : public PathVisitor {
public:
    explicit ListVisitor(std::vector<AnimatableProperty>& out) : out_(out) {}
    bool wantsAllFields() const override { return false; }
    bool field(const ecs::Meta& m, float&, const ecs::FloatRange&) override { return add(m, PropertyKind::Float, 1); }
    bool field(const ecs::Meta& m, int&, int, int) override { return add(m, PropertyKind::Int, 1); }
    bool field(const ecs::Meta& m, bool&) override { return add(m, PropertyKind::Bool, 1); }
    bool field(const ecs::Meta& m, std::string& s) override { return PathVisitor::field(m, s); }
    bool field(const ecs::Meta& m, Vec3&, ecs::Vec3Kind) override { return add(m, PropertyKind::Vec3, 3); }
    bool field(const ecs::Meta& m, Vec2&, float) override { return add(m, PropertyKind::Vec2, 2); }
    bool enumeration(const ecs::Meta& m, int&, std::span<const char* const>) override {
        return add(m, PropertyKind::Enum, 1);
    }

private:
    bool add(const ecs::Meta& meta, PropertyKind kind, int channels) {
        out_.push_back({pathOf(meta.key), labelOf(meta.label), kind, channels});
        return false;
    }
    std::vector<AnimatableProperty>& out_;
};

// Lee o escribe canales concretos.
class AccessVisitor final : public PathVisitor {
public:
    struct Slot {
        std::string_view path;
        int channel = 0;
        float value = 0.0f;
        int channels = 0;  // al leer: los del campo (0 = no encontrado)
    };
    AccessVisitor(std::vector<Slot>& slots, bool write) : slots_(slots), write_(write) {}
    // Como ListVisitor: solo lo animable. Con true, cada frame entraban los
    // campos ocultos (EditableMesh se serializaba entera, Tilemap2D, ...).
    bool wantsAllFields() const override { return false; }

    bool field(const ecs::Meta& m, float& value, const ecs::FloatRange& range) override {
        return scalar(m, 1, [&](float& f, bool set) {
            if (!set) {
                f = value;
                return false;
            }
            float v = f;
            if (range.max > range.min) v = std::clamp(v, range.min, range.max);
            if (v == value) return false;
            value = v;
            return true;
        });
    }
    bool field(const ecs::Meta& m, int& value, int min, int max) override {
        return scalar(m, 1, [&](float& f, bool set) {
            if (!set) {
                f = static_cast<float>(value);
                return false;
            }
            int v = static_cast<int>(std::lround(f));
            if (max > min) v = std::clamp(v, min, max);
            if (v == value) return false;
            value = v;
            return true;
        });
    }
    bool field(const ecs::Meta& m, bool& value) override {
        return scalar(m, 1, [&](float& f, bool set) {
            if (!set) {
                f = value ? 1.0f : 0.0f;
                return false;
            }
            const bool v = f >= 0.5f;
            if (v == value) return false;
            value = v;
            return true;
        });
    }
    bool field(const ecs::Meta& m, std::string& s) override { return PathVisitor::field(m, s); }
    bool enumeration(const ecs::Meta& m, int& value, std::span<const char* const> names) override {
        return scalar(m, 1, [&](float& f, bool set) {
            if (!set) {
                f = static_cast<float>(value);
                return false;
            }
            const int v = std::clamp(static_cast<int>(std::lround(f)), 0, std::max(0, static_cast<int>(names.size()) - 1));
            if (v == value) return false;
            value = v;
            return true;
        });
    }
    bool field(const ecs::Meta& m, Vec3& value, ecs::Vec3Kind) override {
        return vector(m, 3, [&](int axis) -> float& { return axis == 0 ? value.x : (axis == 1 ? value.y : value.z); });
    }
    bool field(const ecs::Meta& m, Vec2& value, float) override {
        return vector(m, 2, [&](int axis) -> float& { return axis == 0 ? value.x : value.y; });
    }

private:
    template <typename Fn>
    bool scalar(const ecs::Meta& meta, int channels, Fn&& access) {
        bool changed = false;
        const std::string path = pathOf(meta.key);
        for (Slot& slot : slots_) {
            if (slot.path != path) continue;
            slot.channels = channels;
            if (slot.channel != 0) continue;
            changed |= access(slot.value, write_);
        }
        return changed;
    }
    template <typename Fn>
    bool vector(const ecs::Meta& meta, int channels, Fn&& axis) {
        bool changed = false;
        const std::string path = pathOf(meta.key);
        for (Slot& slot : slots_) {
            if (slot.path != path) continue;
            slot.channels = channels;
            if (slot.channel < 0 || slot.channel >= channels) continue;
            float& v = axis(slot.channel);
            if (!write_) {
                slot.value = v;
            } else if (v != slot.value) {
                v = slot.value;
                changed = true;
            }
        }
        return changed;
    }

    std::vector<Slot>& slots_;
    bool write_;
};

// Resuelve una vez a que campo escribe cada curva: su desplazamiento en bytes
// dentro del componente, su tipo y sus limites. Un campo que no vive dentro
// del componente (elemento de una lista, copia en una variable de reflect)
// queda para la via lenta.
class BindVisitor final : public PathVisitor {
public:
    enum class Status { NotFound, Bound, Slow, Ignored };
    struct Slot {
        std::string_view path;
        int channel = 0;
        Status status = Status::NotFound;
        std::uint32_t offset = 0;
        PropertyKind kind = PropertyKind::Float;
        float fmin = 0.0f, fmax = 0.0f;
        int imin = 0, imax = 0;
    };
    BindVisitor(std::vector<Slot>& slots, const void* base, std::size_t size)
        : slots_(slots), base_(static_cast<const char*>(base)), size_(size) {}
    bool wantsAllFields() const override { return false; }

    bool field(const ecs::Meta& m, float& value, const ecs::FloatRange& range) override {
        scalar(m, &value, sizeof(float), PropertyKind::Float, [&](Slot& s) {
            s.fmin = range.min;
            s.fmax = range.max;
        });
        return false;
    }
    bool field(const ecs::Meta& m, int& value, int min, int max) override {
        scalar(m, &value, sizeof(int), PropertyKind::Int, [&](Slot& s) {
            s.imin = min;
            s.imax = max;
        });
        return false;
    }
    bool field(const ecs::Meta& m, bool& value) override {
        scalar(m, &value, sizeof(bool), PropertyKind::Bool, [](Slot&) {});
        return false;
    }
    bool field(const ecs::Meta& m, std::string& s) override { return PathVisitor::field(m, s); }
    bool enumeration(const ecs::Meta& m, int& value, std::span<const char* const> names) override {
        scalar(m, &value, sizeof(int), PropertyKind::Enum, [&](Slot& s) {
            s.imin = 0;
            s.imax = std::max(0, static_cast<int>(names.size()) - 1);
        });
        return false;
    }
    bool field(const ecs::Meta& m, Vec3& value, ecs::Vec3Kind) override {
        float* axes[3] = {&value.x, &value.y, &value.z};
        vector(m, 3, axes, PropertyKind::Vec3);
        return false;
    }
    bool field(const ecs::Meta& m, Vec2& value, float) override {
        float* axes[3] = {&value.x, &value.y, nullptr};
        vector(m, 2, axes, PropertyKind::Vec2);
        return false;
    }

private:
    // Desplazamiento de `p` (de `bytes`) dentro del componente, o -1.
    long long offsetOf(const void* p, std::size_t bytes) const {
        const std::uintptr_t c = reinterpret_cast<std::uintptr_t>(p);
        const std::uintptr_t b = reinterpret_cast<std::uintptr_t>(base_);
        if (c < b || c + bytes > b + size_) return -1;
        return static_cast<long long>(c - b);
    }
    void place(Slot& slot, const void* p, std::size_t bytes, PropertyKind kind) {
        const long long offset = offsetOf(p, bytes);
        slot.kind = kind;
        if (offset < 0) {
            slot.status = Status::Slow;
        } else {
            slot.status = Status::Bound;
            slot.offset = static_cast<std::uint32_t>(offset);
        }
    }
    template <typename Fn>
    void scalar(const ecs::Meta& meta, const void* p, std::size_t bytes, PropertyKind kind, Fn&& limits) {
        const std::string path = pathOf(meta.key);
        for (Slot& slot : slots_) {
            if (slot.path != path) continue;
            // Como AccessVisitor: un escalar solo tiene el canal 0.
            if (slot.channel != 0) {
                slot.status = Status::Ignored;
                continue;
            }
            place(slot, p, bytes, kind);
            limits(slot);
        }
    }
    void vector(const ecs::Meta& meta, int channels, float* const* axes, PropertyKind kind) {
        const std::string path = pathOf(meta.key);
        for (Slot& slot : slots_) {
            if (slot.path != path) continue;
            if (slot.channel < 0 || slot.channel >= channels) {
                slot.status = Status::Ignored;
                continue;
            }
            place(slot, axes[slot.channel], sizeof(float), kind);
        }
    }

    std::vector<Slot>& slots_;
    const char* base_;
    std::size_t size_;
};

// Tras escribir directamente en el componente: un reflect() que dice "cambio"
// en los campos escritos (por su direccion; sin rutas ni textos). Asi corre lo
// mismo que con la via lenta: lo que reflect hace al cambiar un campo
// (markModified de una spline...) y el onChanged del componente.
class MarkVisitor final : public ecs::PropertyVisitor {
public:
    explicit MarkVisitor(const std::vector<const void*>& fields) : fields_(fields) {}
    bool wantsAllFields() const override { return false; }
    bool field(const ecs::Meta&, float& v, const ecs::FloatRange&) override { return has(&v); }
    bool field(const ecs::Meta&, int& v, int, int) override { return has(&v); }
    bool field(const ecs::Meta&, bool& v) override { return has(&v); }
    bool field(const ecs::Meta&, std::string&) override { return false; }
    bool field(const ecs::Meta&, Vec3& v, ecs::Vec3Kind) override { return has(&v.x) || has(&v.y) || has(&v.z); }
    bool field(const ecs::Meta&, Vec2& v, float) override { return has(&v.x) || has(&v.y); }
    bool enumeration(const ecs::Meta&, int& v, std::span<const char* const>) override { return has(&v); }
    bool asset(const ecs::Meta&, assets::AssetRef&, assets::AssetType) override { return false; }
    bool entity(const ecs::Meta&, Uuid&) override { return false; }
    bool layerMask(const ecs::Meta&, std::uint32_t&) override { return false; }

private:
    bool has(const void* p) const { return std::find(fields_.begin(), fields_.end(), p) != fields_.end(); }
    const std::vector<const void*>& fields_;
};

std::atomic<std::uint64_t> g_curves_serial{0};

bool access(ecs::World& world, ecs::Entity entity, std::string_view component, std::vector<AccessVisitor::Slot>& slots,
            bool write) {
    if (!entity.valid()) return false;
    const ecs::ComponentType* type = ecs::ComponentRegistry::instance().find(component);
    if (type == nullptr || !type->has(world, entity.handle())) return false;
    AccessVisitor visitor(slots, write);
    // Al leer no cambia nada: reflect no avisa al componente.
    return type->reflect(world, entity.handle(), visitor);
}

// --- Texto de las claves ----------------------------------------------------------

void appendFloat(std::string& out, float value) {
    char buffer[32];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    out.append(buffer, result.ptr);
}

bool parseFloat(std::string_view& text, float& value) {
    while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{}) return false;
    text.remove_prefix(static_cast<std::size_t>(result.ptr - text.data()));
    return true;
}

}  // namespace

// -----------------------------------------------------------------------------
// AnimationCurve
// -----------------------------------------------------------------------------

float AnimationCurve::evaluate(float time) const {
    if (keys.empty()) return 0.0f;
    if (keys.size() == 1) return keys.front().value;
    const Keyframe& first = keys.front();
    const Keyframe& last = keys.back();
    if (time < first.time || time > last.time) {
        switch (extrapolation) {
            case Extrapolation::Constant: return time < first.time ? first.value : last.value;
            case Extrapolation::Linear:
                return time < first.time ? first.value + endSlope(keys, true) * (time - first.time)
                                         : last.value + endSlope(keys, false) * (time - last.time);
            case Extrapolation::Cycle: {
                const float span = last.time - first.time;
                if (span <= kEpsilon) return first.value;
                time = first.time + std::fmod(std::fmod(time - first.time, span) + span, span);
                break;
            }
        }
    }
    // Tramo: la ultima clave con tiempo <= time.
    const auto it = std::upper_bound(keys.begin(), keys.end(), time,
                                     [](float t, const Keyframe& k) { return t < k.time; });
    if (it == keys.begin()) return first.value;
    if (it == keys.end()) return last.value;
    const Keyframe& a = *(it - 1);
    const Keyframe& b = *it;
    switch (a.interpolation) {
        case Interpolation::Constant: return a.value;
        case Interpolation::Linear: {
            const float span = b.time - a.time;
            return span > kEpsilon ? a.value + (b.value - a.value) * (time - a.time) / span : b.value;
        }
        case Interpolation::Bezier: return evaluateBezier(a, b, time);
    }
    return a.value;
}

int AnimationCurve::find(float time, float epsilon) const {
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (std::abs(keys[i].time - time) <= epsilon) return static_cast<int>(i);
    }
    return -1;
}

int AnimationCurve::insert(float time, float value, Interpolation interpolation) {
    int index = find(time);
    if (index >= 0) {
        Keyframe& k = keys[static_cast<std::size_t>(index)];
        const float dv = value - k.value;
        k.value = value;
        // Las asas libres se mueven con la clave.
        k.left.y += dv;
        k.right.y += dv;
    } else {
        Keyframe k;
        k.time = time;
        k.value = value;
        k.interpolation = interpolation;
        // Como la clave anterior (si es constante o lineal, la nueva tambien).
        const auto it = std::upper_bound(keys.begin(), keys.end(), time,
                                         [](float t, const Keyframe& key) { return t < key.time; });
        if (it != keys.begin()) k.interpolation = (it - 1)->interpolation;
        index = static_cast<int>(keys.insert(it, k) - keys.begin());
    }
    recalcHandles();
    return index;
}

void AnimationCurve::sort() {
    std::stable_sort(keys.begin(), keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
    recalcHandles();
}

void AnimationCurve::recalcHandles() {
    const std::size_t n = keys.size();
    for (std::size_t i = 0; i < n; ++i) {
        Keyframe& k = keys[i];
        const Keyframe* prev = i > 0 ? &keys[i - 1] : nullptr;
        const Keyframe* next = i + 1 < n ? &keys[i + 1] : nullptr;
        // Longitud de cada asa: un tercio del tramo (o del otro si es un extremo).
        float dl = prev != nullptr ? k.time - prev->time : (next != nullptr ? next->time - k.time : 1.0f);
        float dr = next != nullptr ? next->time - k.time : dl;
        dl = std::max(dl, kEpsilon) / 3.0f;
        dr = std::max(dr, kEpsilon) / 3.0f;

        // Pendiente automatica.
        float slope = 0.0f;
        if (prev != nullptr && next != nullptr) {
            slope = (next->value - prev->value) / std::max(next->time - prev->time, kEpsilon);
            const bool clamped = k.left_type == HandleType::AutoClamped || k.right_type == HandleType::AutoClamped;
            if (clamped) {
                const float a = k.value - prev->value;
                const float b = next->value - k.value;
                if (a * b <= 0.0f) {
                    slope = 0.0f;  // maximo o minimo: plana
                } else {
                    // Sin pasarse de las claves vecinas (como mucho 3 veces
                    // la pendiente de cada tramo; dl y dr ya son un tercio).
                    const float limit = std::min(std::abs(a) / dl, std::abs(b) / dr);
                    slope = std::clamp(slope, -limit, limit);
                }
            }
        } else if (extrapolation == Extrapolation::Linear && (prev != nullptr || next != nullptr)) {
            const Keyframe& o = prev != nullptr ? *prev : *next;
            slope = (o.value - k.value) / (o.time - k.time == 0.0f ? kEpsilon : o.time - k.time);
        }

        auto left_vector = [&] {
            return prev != nullptr ? Vec2{k.time + (prev->time - k.time) / 3.0f, k.value + (prev->value - k.value) / 3.0f}
                                   : Vec2{k.time - dl, k.value};
        };
        auto right_vector = [&] {
            return next != nullptr ? Vec2{k.time + (next->time - k.time) / 3.0f, k.value + (next->value - k.value) / 3.0f}
                                   : Vec2{k.time + dr, k.value};
        };
        const Vec2 auto_left{k.time - dl, k.value - slope * dl};
        const Vec2 auto_right{k.time + dr, k.value + slope * dr};

        // Asas sin inicializar (clave nueva con asas libres): como las automaticas.
        const bool unset = k.left.x == 0.0f && k.left.y == 0.0f && k.right.x == 0.0f && k.right.y == 0.0f;

        if (isAuto(k.left_type) || (unset && k.left_type != HandleType::Vector)) {
            k.left = auto_left;
        } else if (k.left_type == HandleType::Vector) {
            k.left = left_vector();
        }
        if (isAuto(k.right_type) || (unset && k.right_type != HandleType::Vector)) {
            k.right = auto_right;
        } else if (k.right_type == HandleType::Vector) {
            k.right = right_vector();
        }
        // Alineadas: una sigue la direccion de la otra conservando su longitud
        // (manda la de la derecha salvo que la de la derecha sea vector/auto).
        if (k.left_type == HandleType::Aligned && k.right_type != HandleType::Free) {
            const Vec2 d{k.right.x - k.time, k.right.y - k.value};
            const float ld = std::sqrt(d.x * d.x + d.y * d.y);
            const Vec2 l{k.left.x - k.time, k.left.y - k.value};
            const float ll = std::sqrt(l.x * l.x + l.y * l.y);
            if (ld > kEpsilon) k.left = Vec2{k.time - d.x / ld * ll, k.value - d.y / ld * ll};
        } else if (k.right_type == HandleType::Aligned && k.left_type != HandleType::Free) {
            const Vec2 d{k.time - k.left.x, k.value - k.left.y};
            const float ld = std::sqrt(d.x * d.x + d.y * d.y);
            const Vec2 r{k.right.x - k.time, k.right.y - k.value};
            const float lr = std::sqrt(r.x * r.x + r.y * r.y);
            if (ld > kEpsilon) k.right = Vec2{k.time + d.x / ld * lr, k.value + d.y / ld * lr};
        }
        // Nunca hacia el otro lado en el tiempo.
        k.left.x = std::min(k.left.x, k.time);
        k.right.x = std::max(k.right.x, k.time);
    }
}

// -----------------------------------------------------------------------------
// KeyframeAnimation
// -----------------------------------------------------------------------------

AnimationCurve* KeyframeAnimation::findCurve(const Uuid& target, std::string_view component, std::string_view property,
                                             int channel) {
    for (AnimationCurve& c : curves) {
        if (c.target == target && c.channel == channel && c.component == component && c.property == property) return &c;
    }
    return nullptr;
}

AnimationCurve& KeyframeAnimation::curve(const Uuid& target, std::string_view component, std::string_view property,
                                         int channel) {
    if (AnimationCurve* c = findCurve(target, component, property, channel)) return *c;
    AnimationCurve c;
    c.target = target;
    c.component = std::string(component);
    c.property = std::string(property);
    c.channel = channel;
    // Junto a los otros canales de la misma propiedad, por orden de canal.
    std::size_t at = curves.size();
    for (std::size_t i = 0; i < curves.size(); ++i) {
        const AnimationCurve& o = curves[i];
        if (!(o.target == target) || o.component != component || o.property != property) continue;
        if (o.channel < channel) {
            at = i + 1;
        } else if (at == curves.size()) {
            at = i;
            break;
        }
    }
    touchCurves();
    return *curves.insert(curves.begin() + static_cast<std::ptrdiff_t>(at), std::move(c));
}

void KeyframeAnimation::touchCurves() {
    curves_version = ++g_curves_serial;
}

float KeyframeAnimation::timeAt(float elapsed) const {
    const float length = end - start;
    if (length <= kEpsilon) return start;
    switch (loop) {
        case LoopMode::Once: return start + std::clamp(elapsed, 0.0f, length);
        case LoopMode::Loop: return start + std::fmod(std::fmod(elapsed, length) + length, length);
        case LoopMode::PingPong: {
            const float t = std::fmod(std::fmod(elapsed, 2.0f * length) + 2.0f * length, 2.0f * length);
            return start + (t <= length ? t : 2.0f * length - t);
        }
    }
    return start;
}

void KeyframeAnimation::reflect(ecs::PropertyVisitor& v) {
    static constexpr std::array<const char*, 3> kLoops = {"Una vez", "En bucle", "Ida y vuelta"};
    static constexpr std::array<const char*, 3> kExtrapolations = {"Constante", "Lineal", "En ciclo"};
    v.field({"play_on_start", "Empezar al dar Play"}, play_on_start);
    ecs::enumField(v, {"loop", "Repeticion"}, loop, kLoops);
    v.field({"speed", "Velocidad"}, speed, ecs::FloatRange{-10.0f, 10.0f, 0.01f, "%.2fx"});
    v.field({"start", "Inicio", "Segundos"}, start, ecs::FloatRange{-100000.0f, 100000.0f, 0.01f, "%.2f s"});
    v.field({"end", "Fin", "Segundos"}, end, ecs::FloatRange{-100000.0f, 100000.0f, 0.01f, "%.2f s"});
    v.field({"fps", "Fotogramas por segundo", "Los del editor: las claves se ajustan a ellos"}, fps,
            ecs::FloatRange{1.0f, 240.0f, 0.1f, "%.0f"});
    // Las curvas se editan en la ventana Animacion (Dope Sheet / Graficas):
    // en el Inspector no se ven, solo se guardan.
    if (!v.wantsAllFields()) return;
    // Leer la escena (o deshacer) puede cambiar que anima cada curva: la
    // cache del sistema se rehace (touchCurves).
    bool retargeted = false;
    retargeted |= ecs::listField(v, {"curves", "Curvas"}, curves, [&](AnimationCurve& c, ecs::PropertyVisitor& item) {
        retargeted |= item.entity({"target", "Objeto"}, c.target);
        retargeted |= item.field({"component", "Componente"}, c.component);
        retargeted |= item.field({"property", "Propiedad"}, c.property);
        retargeted |= item.field({"channel", "Canal"}, c.channel, 0, 3);
        ecs::enumField(item, {"extrapolation", "Extrapolacion"}, c.extrapolation, kExtrapolations);
        item.field({"muted", "Silenciada"}, c.muted);
        std::string text = encodeKeys(c.keys);
        if (item.field({"keys", "Claves"}, text)) c.keys = decodeKeys(text);
    });
    if (retargeted) touchCurves();
}

// -----------------------------------------------------------------------------
// Propiedades
// -----------------------------------------------------------------------------

std::vector<AnimatableProperty> animatableProperties(ecs::World& world, ecs::Entity entity,
                                                     std::string_view component) {
    std::vector<AnimatableProperty> out;
    if (!entity.valid()) return out;
    const ecs::ComponentType* type = ecs::ComponentRegistry::instance().find(component);
    if (type == nullptr || !type->has(world, entity.handle())) return out;
    ListVisitor visitor(out);
    type->reflect(world, entity.handle(), visitor);
    return out;
}

bool readProperty(ecs::World& world, ecs::Entity entity, std::string_view component, std::string_view path,
                  int channel, float& value) {
    std::vector<AccessVisitor::Slot> slots{{path, channel, 0.0f, 0}};
    access(world, entity, component, slots, false);
    if (slots[0].channels == 0 || channel >= slots[0].channels) return false;
    value = slots[0].value;
    return true;
}

int propertyChannels(ecs::World& world, ecs::Entity entity, std::string_view component, std::string_view path) {
    std::vector<AccessVisitor::Slot> slots{{path, 0, 0.0f, 0}};
    access(world, entity, component, slots, false);
    return slots[0].channels;
}

bool writeProperties(ecs::World& world, ecs::Entity entity, std::string_view component,
                     const std::vector<ChannelValue>& values) {
    if (values.empty()) return false;
    std::vector<AccessVisitor::Slot> slots;
    slots.reserve(values.size());
    for (const ChannelValue& v : values) slots.push_back({v.path, v.channel, v.value, 0});
    return access(world, entity, component, slots, true);
}

// "tiempo valor interpolacion asa_izq asa_der ix iy dx dy;..."
std::string encodeKeys(const std::vector<Keyframe>& keys) {
    std::string out;
    out.reserve(keys.size() * 48);
    for (const Keyframe& k : keys) {
        if (!out.empty()) out += ';';
        appendFloat(out, k.time);
        out += ' ';
        appendFloat(out, k.value);
        out += ' ';
        out += std::to_string(static_cast<int>(k.interpolation));
        out += ' ';
        out += std::to_string(static_cast<int>(k.left_type));
        out += ' ';
        out += std::to_string(static_cast<int>(k.right_type));
        for (const float f : {k.left.x, k.left.y, k.right.x, k.right.y}) {
            out += ' ';
            appendFloat(out, f);
        }
    }
    return out;
}

std::vector<Keyframe> decodeKeys(std::string_view text) {
    std::vector<Keyframe> keys;
    while (!text.empty()) {
        const std::size_t end = text.find(';');
        std::string_view item = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        std::array<float, 9> f{};
        int count = 0;
        while (count < 9 && parseFloat(item, f[static_cast<std::size_t>(count)])) ++count;
        if (count < 2) continue;
        Keyframe k;
        k.time = f[0];
        k.value = f[1];
        if (count >= 5) {
            k.interpolation = static_cast<Interpolation>(std::clamp(static_cast<int>(f[2]), 0, 2));
            k.left_type = static_cast<HandleType>(std::clamp(static_cast<int>(f[3]), 0, 4));
            k.right_type = static_cast<HandleType>(std::clamp(static_cast<int>(f[4]), 0, 4));
        }
        if (count >= 9) {
            k.left = Vec2{f[5], f[6]};
            k.right = Vec2{f[7], f[8]};
        }
        keys.push_back(k);
    }
    std::stable_sort(keys.begin(), keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
    return keys;
}

// -----------------------------------------------------------------------------
// Sistema
// -----------------------------------------------------------------------------

void KeyframeSystem::applyAt(ecs::World& world, ecs::Entity entity, const KeyframeAnimation& animation, float time) {
    // Una pasada de reflect() por objeto y componente con todos sus canales.
    struct Batch {
        const Uuid* target;
        const std::string* component;
        std::vector<ChannelValue> values;
    };
    std::vector<Batch> batches;
    for (const AnimationCurve& c : animation.curves) {
        if (c.muted || c.keys.empty()) continue;
        Batch* batch = nullptr;
        for (Batch& b : batches) {
            if (*b.target == c.target && *b.component == c.component) {
                batch = &b;
                break;
            }
        }
        if (batch == nullptr) batch = &batches.emplace_back(Batch{&c.target, &c.component, {}});
        batch->values.push_back({c.property, c.channel, c.evaluate(time)});
    }
    for (const Batch& b : batches) {
        const ecs::Entity target = b.target->valid() ? world.find(*b.target) : entity;
        writeProperties(world, target, *b.component, b.values);
    }
}

void KeyframeSystem::applyAllAt(ecs::World& world, float time) {
    for (const entt::entity handle : world.registry().view<KeyframeAnimation>()) {
        const KeyframeAnimation& animation = world.registry().get<KeyframeAnimation>(handle);
        applyAt(world, world.wrap(handle), animation, time);
    }
}

void KeyframeSystem::bind(ecs::World& world, ecs::Entity entity, const KeyframeAnimation& animation, Cache& cache) {
    ++bind_count_;
    cache.groups.clear();
    cache.valid = true;
    cache.complete = true;
    cache.curves_version = animation.curves_version;
    cache.curve_count = animation.curves.size();
    cache.structure = world.structureVersion();
    const ecs::ComponentRegistry& registry = ecs::ComponentRegistry::instance();
    // Como applyAt: un grupo por objeto y componente, en el orden de las curvas.
    std::vector<std::uint32_t> first;  // primera curva de cada grupo
    std::vector<std::vector<std::uint32_t>> members;
    for (std::uint32_t i = 0; i < animation.curves.size(); ++i) {
        const AnimationCurve& c = animation.curves[i];
        std::size_t g = 0;
        for (; g < first.size(); ++g) {
            const AnimationCurve& f = animation.curves[first[g]];
            if (f.target == c.target && f.component == c.component) break;
        }
        if (g == first.size()) {
            first.push_back(i);
            members.emplace_back();
        }
        members[g].push_back(i);
    }
    std::vector<BindVisitor::Slot> slots;
    for (std::size_t g = 0; g < first.size(); ++g) {
        const AnimationCurve& f = animation.curves[first[g]];
        const int type_index = registry.indexOf(f.component);
        const ecs::Entity target = f.target.valid() ? world.find(f.target) : entity;
        if (type_index < 0 || !target.valid()) {
            cache.complete = false;  // se reintenta al cambiar el mundo
            continue;
        }
        const ecs::ComponentType& type = registry.types()[static_cast<std::size_t>(type_index)];
        void* base = type.get != nullptr ? type.get(world, target.handle()) : nullptr;
        if (base == nullptr) {
            cache.complete = false;
            continue;
        }
        Group group;
        group.target = f.target;
        group.handle = target.handle();
        group.type = static_cast<std::size_t>(type_index);
        if (type.name == "Transform") {
            const ecs::Transform* t = static_cast<const ecs::Transform*>(base);
            group.transform = true;
            group.euler_offset = static_cast<std::uint32_t>(reinterpret_cast<const char*>(&t->euler) -
                                                            reinterpret_cast<const char*>(t));
        }
        slots.clear();
        for (const std::uint32_t i : members[g]) {
            const AnimationCurve& c = animation.curves[i];
            BindVisitor::Slot slot;
            slot.path = c.property;
            slot.channel = c.channel;
            slots.push_back(slot);
        }
        BindVisitor visitor(slots, base, type.size);
        type.reflect(world, target.handle(), visitor);  // no cambia nada: no avisa
        for (std::size_t k = 0; k < slots.size(); ++k) {
            const BindVisitor::Slot& s = slots[k];
            const std::uint32_t curve = members[g][k];
            switch (s.status) {
                case BindVisitor::Status::Bound: {
                    Binding b;
                    b.curve = curve;
                    b.offset = s.offset;
                    b.kind = s.kind;
                    b.fmin = s.fmin;
                    b.fmax = s.fmax;
                    b.imin = s.imin;
                    b.imax = s.imax;
                    group.bindings.push_back(b);
                    break;
                }
                case BindVisitor::Status::Slow:
                case BindVisitor::Status::NotFound: group.slow.push_back(curve); break;
                case BindVisitor::Status::Ignored: break;  // canal que el campo no tiene: no se escribe
            }
        }
        cache.groups.push_back(std::move(group));
    }
}

void KeyframeSystem::applyCached(ecs::World& world, ecs::Entity entity, const KeyframeAnimation& animation,
                                 Cache& cache, float time) {
    if (!cache.valid || cache.curves_version != animation.curves_version ||
        cache.curve_count != animation.curves.size()) {
        bind(world, entity, animation, cache);
    } else if (cache.structure != world.structureVersion()) {
        // Algo se creo, destruyo o cambio de componentes: los objetivos por
        // UUID pueden ser otra entidad (deshacer, recargar una celda).
        bool same = cache.complete;
        for (const Group& g : cache.groups) {
            if (!same) break;
            const ecs::Entity target = g.target.valid() ? world.find(g.target) : entity;
            same = target.valid() && target.handle() == g.handle;
        }
        if (same) cache.structure = world.structureVersion();
        else bind(world, entity, animation, cache);
    }
    const ecs::ComponentRegistry& registry = ecs::ComponentRegistry::instance();
    for (const Group& g : cache.groups) {
        if (!world.valid(g.handle) || g.type >= registry.types().size()) continue;
        const ecs::ComponentType& type = registry.types()[g.type];
        // El puntero se pide cada frame: el pool mueve sus componentes al
        // quitar alguno.
        char* base = static_cast<char*>(type.get(world, g.handle));
        if (base == nullptr) {
            cache.complete = false;  // le quitaron el componente: se rehace con el mundo
            continue;
        }
        changed_fields_.clear();
        bool euler_changed = false;
        for (const Binding& b : g.bindings) {
            const AnimationCurve& c = animation.curves[b.curve];
            if (c.muted || c.keys.empty()) continue;
            const float f = c.evaluate(time);
            char* p = base + b.offset;
            switch (b.kind) {
                case PropertyKind::Float:
                case PropertyKind::Vec2:
                case PropertyKind::Vec3: {
                    float v = f;
                    if (b.kind == PropertyKind::Float && b.fmax > b.fmin) v = std::clamp(v, b.fmin, b.fmax);
                    float& dst = *reinterpret_cast<float*>(p);
                    if (dst == v) continue;
                    dst = v;
                    break;
                }
                case PropertyKind::Int: {
                    int v = static_cast<int>(std::lround(f));
                    if (b.imax > b.imin) v = std::clamp(v, b.imin, b.imax);
                    int& dst = *reinterpret_cast<int*>(p);
                    if (dst == v) continue;
                    dst = v;
                    break;
                }
                case PropertyKind::Enum: {
                    const int v = std::clamp(static_cast<int>(std::lround(f)), 0, b.imax);
                    int& dst = *reinterpret_cast<int*>(p);
                    if (dst == v) continue;
                    dst = v;
                    break;
                }
                case PropertyKind::Bool: {
                    const bool v = f >= 0.5f;
                    bool& dst = *reinterpret_cast<bool*>(p);
                    if (dst == v) continue;
                    dst = v;
                    break;
                }
            }
            changed_fields_.push_back(p);
            if (g.transform && b.offset >= g.euler_offset && b.offset < g.euler_offset + sizeof(Vec3)) {
                euler_changed = true;
            }
        }
        if (!changed_fields_.empty()) {
            if (g.transform && !euler_changed) {
                // Solo posicion o escala: el giro no se rehace con los grados
                // (Transform::onChanged); asi no se pisa el que dejo la fisica o
                // un script (setWorldMatrix no siempre rehace los grados).
                world.markTransformDirty(g.handle);
            } else {
                MarkVisitor visitor(changed_fields_);
                type.reflect(world, g.handle, visitor);
            }
        }
        if (!g.slow.empty()) {
            slow_values_.clear();
            for (const std::uint32_t i : g.slow) {
                const AnimationCurve& c = animation.curves[i];
                if (c.muted || c.keys.empty()) continue;
                slow_values_.push_back({c.property, c.channel, c.evaluate(time)});
            }
            if (!slow_values_.empty()) writeProperties(world, world.wrap(g.handle), type.name, slow_values_);
        }
    }
}

void KeyframeSystem::update(ecs::World& world, float delta_seconds) {
    // Estados de entidades que ya no existen (o con su handle reutilizado por
    // otra): fuera, solo cuando cambia la estructura del mundo.
    if (pruned_structure_ != world.structureVersion()) {
        pruned_structure_ = world.structureVersion();
        for (auto it = states_.begin(); it != states_.end();) {
            const ecs::Entity e = world.wrap(it->first);
            if (!e.valid() || !(e.uuid() == it->second.uuid)) {
                it = states_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const entt::entity handle : world.registry().view<KeyframeAnimation>()) {
        const ecs::Entity entity = world.wrap(handle);
        if (!entity.activeInHierarchy()) continue;
        const KeyframeAnimation& animation = world.registry().get<KeyframeAnimation>(handle);
        State& state = stateOf(entity);
        if (!state.started) {
            state.started = true;
            state.playing = animation.play_on_start;
            state.elapsed = 0.0f;
        }
        if (!state.playing) continue;
        state.elapsed += delta_seconds * animation.speed;
        const float length = animation.end - animation.start;
        if (animation.loop == LoopMode::Once && (state.elapsed >= length || state.elapsed <= 0.0f) &&
            delta_seconds > 0.0f) {
            state.elapsed = std::clamp(state.elapsed, 0.0f, std::max(length, 0.0f));
            state.playing = false;  // se queda en el ultimo fotograma
        }
        applyCached(world, entity, animation, state.cache, animation.timeAt(state.elapsed));
    }
}

KeyframeSystem::State& KeyframeSystem::stateOf(ecs::Entity entity) {
    State& state = states_[entity.handle()];
    if (!(state.uuid == entity.uuid())) state = State{entity.uuid()};
    return state;
}

void KeyframeSystem::play(ecs::Entity entity, float from) {
    if (!entity.valid()) return;
    const KeyframeAnimation* animation = entity.tryGet<KeyframeAnimation>();
    if (animation == nullptr) return;
    State& state = stateOf(entity);
    state.started = true;
    state.playing = true;
    state.elapsed = from < 0.0f ? 0.0f : from - animation->start;
}

void KeyframeSystem::stop(ecs::Entity entity) {
    if (!entity.valid()) return;
    State& state = stateOf(entity);
    state.started = true;
    state.playing = false;
}

bool KeyframeSystem::isPlaying(ecs::Entity entity) const {
    const auto it = states_.find(entity.handle());
    return it != states_.end() && it->second.uuid == entity.uuid() && it->second.playing;
}

float KeyframeSystem::time(ecs::Entity entity) const {
    const auto it = states_.find(entity.handle());
    if (it == states_.end() || !entity.valid() || !(it->second.uuid == entity.uuid())) return 0.0f;
    const KeyframeAnimation* animation = entity.tryGet<KeyframeAnimation>();
    return animation != nullptr ? animation->timeAt(it->second.elapsed) : 0.0f;
}

}  // namespace cramion::anim

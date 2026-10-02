#ifndef CRAMION_CORE_CVAR_H
#define CRAMION_CORE_CVAR_H

// Variables de configuracion con nombre (CVars, como las console variables
// de Unreal o de id Tech): se declaran donde se usan, se cambian desde la
// consola, la ventana Variables, Lua, los scripts de C++ o el MCP, y las
// marcadas Saved se guardan en ProjectSettings/CVars.json.
//
//   static cvar::CVar<int> g_host_memory("script.cpp.MemoryLimitMB", 1024,
//       "Memoria maxima del proceso de los scripts de C++ (MB)", cvar::Saved, 64, 65536);
//   if (g_host_memory > 512) ...           // lectura barata (atomica)
//   g_host_memory = 2048;                  // avisa a los oyentes
//
// Tipos: bool, int, int64, float, double y std::string. Los numeros pueden
// tener limites (se recortan al cambiar). Se pueden leer desde cualquier hilo.
// Las que crean los scripts (Registry::createDynamic) viven en el registro.
//
// Comandos de consola (Registry::execute): "nombre" muestra el valor,
// "nombre valor" lo cambia, "reset nombre", "cvars [filtro]" lista.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

namespace cramion::cvar {

enum Flags : std::uint32_t {
    None = 0,
    ReadOnly = 1 << 0,         // solo se cambia desde el codigo (o con force)
    Saved = 1 << 1,            // se guarda en ProjectSettings/CVars.json
    Cheat = 1 << 2,            // solo con trucos permitidos (editor / desarrollo)
    RequiresRestart = 1 << 3,  // tiene efecto al reiniciar (Play o el juego)
    Script = 1 << 4,           // la creo un script (Lua o C++)
};

enum class Type : int { Bool = 0, Int = 1, Float = 2, String = 3 };
const char* typeName(Type type);

class CVarBase {
public:
    CVarBase(const CVarBase&) = delete;
    CVarBase& operator=(const CVarBase&) = delete;
    virtual ~CVarBase();

    const std::string& name() const { return name_; }
    const std::string& description() const { return description_; }
    std::uint32_t flags() const { return flags_; }
    Type type() const { return type_; }
    // Categoria = lo que hay antes del primer punto ("script" en "script.cpp.X").
    std::string category() const;

    virtual std::string toString() const = 0;
    virtual std::string defaultString() const = 0;
    // false (y `error`) si no es un valor valido para el tipo.
    virtual bool fromString(const std::string& text, std::string* error = nullptr) = 0;
    virtual void reset() = 0;
    bool isDefault() const { return toString() == defaultString(); }

    bool hasRange() const { return has_range_; }
    double rangeMin() const { return min_; }
    double rangeMax() const { return max_; }
    // Sube con cada cambio (para ver si algo cambio sin oyentes).
    std::uint64_t version() const { return version_.load(std::memory_order_relaxed); }

    // Oyentes: se llaman en el hilo que cambia el valor.
    int onChanged(std::function<void(CVarBase&)> callback);
    void removeCallback(int id);

protected:
    CVarBase(std::string name, std::string description, std::uint32_t flags, Type type);
    void setRange(double min, double max);
    void notifyChanged();
    double clampValue(double v) const { return has_range_ ? (v < min_ ? min_ : (v > max_ ? max_ : v)) : v; }

private:
    std::string name_;
    std::string description_;
    std::uint32_t flags_ = 0;
    Type type_ = Type::Int;
    bool has_range_ = false;
    double min_ = 0.0, max_ = 0.0;
    std::atomic<std::uint64_t> version_{0};
    std::mutex callbacks_mutex_;
    std::vector<std::pair<int, std::shared_ptr<std::function<void(CVarBase&)>>>> callbacks_;
    int next_callback_ = 1;
};

namespace detail {
std::string formatNumber(double v, bool integer);
bool parseNumber(const std::string& text, double& out);
bool parseBool(const std::string& text, bool& out);
}  // namespace detail

template <typename T>
class CVar final : public CVarBase {
    static_assert(std::is_same_v<T, bool> || std::is_same_v<T, int> || std::is_same_v<T, std::int64_t> ||
                      std::is_same_v<T, float> || std::is_same_v<T, double> || std::is_same_v<T, std::string>,
                  "CVar<T>: bool, int, int64_t, float, double o std::string");

public:
    static constexpr Type kType = std::is_same_v<T, bool>          ? Type::Bool
                                  : std::is_same_v<T, std::string> ? Type::String
                                  : std::is_integral_v<T>          ? Type::Int
                                                                   : Type::Float;

    CVar(const char* name, T default_value, const char* description, std::uint32_t flags = None)
        : CVarBase(name, description != nullptr ? description : "", flags, kType), default_(default_value) {
        store(default_value);
        registerSelf();
    }
    // Con limites (solo numeros).
    template <typename U = T, typename = std::enable_if_t<std::is_arithmetic_v<U> && !std::is_same_v<U, bool>>>
    CVar(const char* name, T default_value, const char* description, std::uint32_t flags, T min, T max)
        : CVarBase(name, description != nullptr ? description : "", flags, kType), default_(default_value) {
        setRange(static_cast<double>(min), static_cast<double>(max));
        store(clamp(default_value));
        registerSelf();
    }
    ~CVar() override { unregisterSelf(); }

    T get() const {
        if constexpr (std::is_same_v<T, std::string>) {
            std::lock_guard lock(string_mutex_);
            return string_;
        } else {
            return value_.load(std::memory_order_relaxed);
        }
    }
    operator T() const { return get(); }
    const T& defaultValue() const { return default_; }

    // Devuelve si cambio.
    bool set(T value) {
        value = clamp(value);
        if constexpr (std::is_same_v<T, std::string>) {
            {
                std::lock_guard lock(string_mutex_);
                if (string_ == value) return false;
                string_ = std::move(value);
            }
        } else {
            if (value_.exchange(value, std::memory_order_relaxed) == value) return false;
        }
        notifyChanged();
        return true;
    }
    CVar& operator=(T value) {
        set(std::move(value));
        return *this;
    }

    std::string toString() const override { return format(get()); }
    std::string defaultString() const override { return format(default_); }
    bool fromString(const std::string& text, std::string* error) override {
        if constexpr (std::is_same_v<T, std::string>) {
            set(text);
            return true;
        } else if constexpr (std::is_same_v<T, bool>) {
            bool b = false;
            if (!detail::parseBool(text, b)) {
                if (error != nullptr) *error = "se esperaba true/false (o 1/0)";
                return false;
            }
            set(b);
            return true;
        } else {
            double d = 0.0;
            if (!detail::parseNumber(text, d)) {
                if (error != nullptr) *error = "se esperaba un numero";
                return false;
            }
            set(static_cast<T>(clampValue(d)));
            return true;
        }
    }
    void reset() override { set(default_); }

private:
    T clamp(T value) const {
        if constexpr (std::is_arithmetic_v<T> && !std::is_same_v<T, bool>) {
            return static_cast<T>(clampValue(static_cast<double>(value)));
        } else {
            return value;
        }
    }
    static std::string format(const T& v) {
        if constexpr (std::is_same_v<T, std::string>) return v;
        else if constexpr (std::is_same_v<T, bool>) return v ? "true" : "false";
        else return detail::formatNumber(static_cast<double>(v), std::is_integral_v<T>);
    }
    void store(const T& v) {
        if constexpr (std::is_same_v<T, std::string>) string_ = v;
        else value_.store(v, std::memory_order_relaxed);
    }
    void registerSelf();
    void unregisterSelf();

    T default_;
    std::conditional_t<std::is_same_v<T, std::string>, char, std::atomic<T>> value_{};
    std::conditional_t<std::is_same_v<T, std::string>, std::string, char> string_{};
    mutable std::mutex string_mutex_;
};

class Registry {
public:
    static Registry& instance();

    CVarBase* find(const std::string& name) const;  // sin distinguir mayusculas
    // Ordenadas por nombre.
    std::vector<CVarBase*> all() const;

    // Cambia por texto. ReadOnly y Cheat (sin trucos) fallan salvo con force.
    bool set(const std::string& name, const std::string& value, std::string* error = nullptr, bool force = false);
    void setCheatsAllowed(bool allowed) { cheats_ = allowed; }
    bool cheatsAllowed() const { return cheats_; }

    // Las de los scripts: si ya existe una con ese nombre y tipo, la devuelve
    // (no cambia su valor). nullptr si existe con otro tipo.
    CVarBase* createDynamic(const std::string& name, Type type, const std::string& default_value,
                            const std::string& description, std::uint32_t flags = Script);
    // Quita las creadas por scripts (al parar Play).
    void clearDynamic();

    // ProjectSettings/CVars.json: las Saved que no tienen su valor por
    // defecto. load() aplica las que existan (y recuerda las desconocidas: una
    // que declare despues un script recibe su valor guardado).
    bool load(const std::filesystem::path& file, std::string* error = nullptr);
    bool save(const std::filesystem::path& file, std::string* error = nullptr) const;
    // Lo mismo desde/hacia texto JSON.
    void loadJson(const std::string& text);
    std::string saveJson() const;

    // Una linea de consola. `handled` = false si no era un comando de CVars
    // (para pasarla a Lua).
    std::string execute(const std::string& line, bool* handled = nullptr);

    // Cambia cada vez que se registra, se quita o cambia alguna (para la UI).
    std::uint64_t generation() const { return generation_.load(std::memory_order_relaxed); }

    // Internos (CVar<T>).
    void add(CVarBase* cvar);
    void remove(CVarBase* cvar);
    void touch() { generation_.fetch_add(1, std::memory_order_relaxed); }

private:
    Registry() = default;
    ~Registry() { dynamic_.clear(); }  // las de los scripts se quitan mientras el registro existe
    mutable std::recursive_mutex mutex_;
    std::vector<CVarBase*> cvars_;
    std::vector<std::unique_ptr<CVarBase>> dynamic_;
    std::vector<std::pair<std::string, std::string>> pending_;  // guardadas que aun no existen
    std::atomic<std::uint64_t> generation_{0};
    bool cheats_ = true;
};

template <typename T>
void CVar<T>::registerSelf() {
    Registry::instance().add(this);
}
template <typename T>
void CVar<T>::unregisterSelf() {
    Registry::instance().remove(this);
}

}  // namespace cramion::cvar

#endif  // CRAMION_CORE_CVAR_H

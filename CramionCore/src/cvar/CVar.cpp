#include "CramionCore/cvar/CVar.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace cramion::cvar {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string describe(const CVarBase& c) {
    std::string out = c.name() + " = " + c.toString() + "  (" + typeName(c.type());
    if (!c.isDefault()) out += ", por defecto " + c.defaultString();
    if (c.hasRange()) {
        out += ", " + detail::formatNumber(c.rangeMin(), c.type() == Type::Int) + " a " +
               detail::formatNumber(c.rangeMax(), c.type() == Type::Int);
    }
    if ((c.flags() & ReadOnly) != 0) out += ", solo lectura";
    if ((c.flags() & Saved) != 0) out += ", se guarda";
    if ((c.flags() & Cheat) != 0) out += ", truco";
    if ((c.flags() & RequiresRestart) != 0) out += ", al reiniciar";
    out += ")";
    if (!c.description().empty()) out += "  " + c.description();
    return out;
}

}  // namespace

const char* typeName(Type type) {
    switch (type) {
        case Type::Bool: return "bool";
        case Type::Int: return "int";
        case Type::Float: return "float";
        case Type::String: return "texto";
    }
    return "?";
}

// --- CVarBase -------------------------------------------------------------------

CVarBase::CVarBase(std::string name, std::string description, std::uint32_t flags, Type type)
    : name_(std::move(name)), description_(std::move(description)), flags_(flags), type_(type) {}

CVarBase::~CVarBase() = default;

std::string CVarBase::category() const {
    const std::size_t dot = name_.find('.');
    return dot == std::string::npos ? std::string() : name_.substr(0, dot);
}

void CVarBase::setRange(double min, double max) {
    has_range_ = true;
    min_ = std::min(min, max);
    max_ = std::max(min, max);
}

int CVarBase::onChanged(std::function<void(CVarBase&)> callback) {
    std::lock_guard lock(callbacks_mutex_);
    const int id = next_callback_++;
    callbacks_.emplace_back(id, std::make_shared<std::function<void(CVarBase&)>>(std::move(callback)));
    return id;
}

void CVarBase::removeCallback(int id) {
    std::lock_guard lock(callbacks_mutex_);
    std::erase_if(callbacks_, [&](const auto& c) { return c.first == id; });
}

void CVarBase::notifyChanged() {
    version_.fetch_add(1, std::memory_order_relaxed);
    Registry::instance().touch();
    std::vector<std::shared_ptr<std::function<void(CVarBase&)>>> copy;
    {
        std::lock_guard lock(callbacks_mutex_);
        for (const auto& c : callbacks_) copy.push_back(c.second);
    }
    for (const auto& c : copy) (*c)(*this);  // un oyente puede quitarse dentro
}

namespace detail {

std::string formatNumber(double v, bool integer) {
    char buffer[64];
    if (integer) {
        std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(std::llround(v)));
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.6g", v);
    }
    std::string s(buffer);
    std::replace(s.begin(), s.end(), ',', '.');  // sin depender del locale
    return s;
}

bool parseNumber(const std::string& text_in, double& out) {
    const std::string text = trim(text_in);
    if (text.empty()) return false;
    // Sin strtod (depende del locale): signo, digitos, punto o coma, exponente.
    std::size_t i = 0;
    bool neg = false;
    if (text[i] == '+' || text[i] == '-') neg = text[i++] == '-';
    double v = 0.0;
    bool digits = false;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
        v = v * 10.0 + (text[i++] - '0');
        digits = true;
    }
    if (i < text.size() && (text[i] == '.' || text[i] == ',')) {
        ++i;
        double scale = 0.1;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
            v += (text[i++] - '0') * scale;
            scale *= 0.1;
            digits = true;
        }
    }
    if (digits && i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        ++i;
        bool eneg = false;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) eneg = text[i++] == '-';
        int e = 0;
        bool edigits = false;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
            e = e * 10 + (text[i++] - '0');
            edigits = true;
        }
        if (!edigits) return false;
        v *= std::pow(10.0, eneg ? -e : e);
    }
    if (!digits || i != text.size()) {
        // true/false tambien valen como 1/0.
        bool b = false;
        if (parseBool(text, b)) {
            out = b ? 1.0 : 0.0;
            return true;
        }
        return false;
    }
    out = neg ? -v : v;
    return std::isfinite(out);
}

bool parseBool(const std::string& text_in, bool& out) {
    const std::string t = lower(trim(text_in));
    if (t == "1" || t == "true" || t == "si" || t == "on" || t == "yes") {
        out = true;
        return true;
    }
    if (t == "0" || t == "false" || t == "no" || t == "off") {
        out = false;
        return true;
    }
    return false;
}

}  // namespace detail

// --- Registry --------------------------------------------------------------------

Registry& Registry::instance() {
    static Registry registry;
    return registry;
}

void Registry::add(CVarBase* cvar) {
    std::lock_guard lock(mutex_);
    cvars_.push_back(cvar);
    // Un valor guardado que esperaba a que se declarara.
    const std::string key = lower(cvar->name());
    for (auto it = pending_.begin(); it != pending_.end(); ++it) {
        if (lower(it->first) != key) continue;
        cvar->fromString(it->second);
        pending_.erase(it);
        break;
    }
    touch();
}

void Registry::remove(CVarBase* cvar) {
    std::lock_guard lock(mutex_);
    std::erase(cvars_, cvar);
    touch();
}

CVarBase* Registry::find(const std::string& name) const {
    std::lock_guard lock(mutex_);
    const std::string key = lower(trim(name));
    for (CVarBase* c : cvars_) {
        if (lower(c->name()) == key) return c;
    }
    return nullptr;
}

std::vector<CVarBase*> Registry::all() const {
    std::lock_guard lock(mutex_);
    std::vector<CVarBase*> out = cvars_;
    std::sort(out.begin(), out.end(), [](const CVarBase* a, const CVarBase* b) { return lower(a->name()) < lower(b->name()); });
    return out;
}

bool Registry::set(const std::string& name, const std::string& value, std::string* error, bool force) {
    CVarBase* c = find(name);
    if (c == nullptr) {
        if (error != nullptr) *error = "no existe la variable '" + name + "'";
        return false;
    }
    if (!force && (c->flags() & ReadOnly) != 0) {
        if (error != nullptr) *error = c->name() + " es de solo lectura";
        return false;
    }
    if (!force && (c->flags() & Cheat) != 0 && !cheats_) {
        if (error != nullptr) *error = c->name() + " es un truco (no permitido aqui)";
        return false;
    }
    return c->fromString(value, error);
}

CVarBase* Registry::createDynamic(const std::string& name, Type type, const std::string& default_value,
                                  const std::string& description, std::uint32_t flags) {
    if (trim(name).empty()) return nullptr;
    std::lock_guard lock(mutex_);
    if (CVarBase* existing = find(name)) return existing->type() == type ? existing : nullptr;
    flags |= Script;
    std::unique_ptr<CVarBase> created;
    const std::string clean = trim(name);
    switch (type) {
        case Type::Bool: {
            bool b = false;
            detail::parseBool(default_value, b);
            created = std::make_unique<CVar<bool>>(clean.c_str(), b, description.c_str(), flags);
            break;
        }
        case Type::Int: {
            double d = 0.0;
            detail::parseNumber(default_value, d);
            created = std::make_unique<CVar<std::int64_t>>(clean.c_str(), static_cast<std::int64_t>(std::llround(d)), description.c_str(), flags);
            break;
        }
        case Type::Float: {
            double d = 0.0;
            detail::parseNumber(default_value, d);
            created = std::make_unique<CVar<double>>(clean.c_str(), d, description.c_str(), flags);
            break;
        }
        case Type::String: created = std::make_unique<CVar<std::string>>(clean.c_str(), default_value, description.c_str(), flags); break;
    }
    CVarBase* raw = created.get();
    dynamic_.push_back(std::move(created));
    return raw;
}

void Registry::clearDynamic() {
    std::lock_guard lock(mutex_);
    // Su valor (si se guarda) espera por si vuelven a declararse.
    for (const auto& c : dynamic_) {
        if ((c->flags() & Saved) != 0 && !c->isDefault()) pending_.emplace_back(c->name(), c->toString());
    }
    dynamic_.clear();
}

void Registry::loadJson(const std::string& text) {
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object()) return;
    std::lock_guard lock(mutex_);
    for (const auto& [name, value] : j.items()) {
        const std::string v = value.is_string() ? value.get<std::string>() : value.dump();
        if (CVarBase* c = find(name)) {
            c->fromString(v);
        } else {
            std::erase_if(pending_, [&](const auto& p) { return lower(p.first) == lower(name); });
            pending_.emplace_back(name, v);
        }
    }
}

std::string Registry::saveJson() const {
    nlohmann::ordered_json j = nlohmann::ordered_json::object();
    std::lock_guard lock(mutex_);
    for (CVarBase* c : all()) {
        if ((c->flags() & Saved) == 0 || c->isDefault()) continue;
        switch (c->type()) {
            case Type::Bool: j[c->name()] = c->toString() == "true"; break;
            case Type::Int:
            case Type::Float: {
                double d = 0.0;
                detail::parseNumber(c->toString(), d);
                if (c->type() == Type::Int) j[c->name()] = static_cast<long long>(std::llround(d));
                else j[c->name()] = d;
                break;
            }
            case Type::String: j[c->name()] = c->toString(); break;
        }
    }
    // Las guardadas que no se han declarado en esta sesion no se pierden.
    for (const auto& [name, value] : pending_) {
        if (!j.contains(name)) j[name] = value;
    }
    return j.dump(2);
}

bool Registry::load(const std::filesystem::path& file, std::string* error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        if (error != nullptr) *error = "no se pudo abrir";
        return false;
    }
    std::stringstream s;
    s << in.rdbuf();
    loadJson(s.str());
    return true;
}

bool Registry::save(const std::filesystem::path& file, std::string* error) const {
    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary);
    if (!out) {
        if (error != nullptr) *error = "no se pudo escribir";
        return false;
    }
    out << saveJson() << "\n";
    return static_cast<bool>(out);
}

std::string Registry::execute(const std::string& line_in, bool* handled) {
    const std::string line = trim(line_in);
    if (handled != nullptr) *handled = true;
    std::istringstream words(line);
    std::string first;
    words >> first;
    std::string rest;
    std::getline(words, rest);
    rest = trim(rest);
    if (lower(first) == "cvars") {
        std::string out;
        int count = 0;
        for (CVarBase* c : all()) {
            if (!rest.empty() && lower(c->name()).find(lower(rest)) == std::string::npos) continue;
            out += describe(*c) + "\n";
            ++count;
        }
        return out + std::to_string(count) + " variables";
    }
    if (lower(first) == "reset" && !rest.empty()) {
        CVarBase* c = find(rest);
        if (c == nullptr) return "no existe la variable '" + rest + "'";
        c->reset();
        return describe(*c);
    }
    CVarBase* c = find(first);
    if (c == nullptr) {
        if (handled != nullptr) *handled = false;
        return {};
    }
    if (rest.empty()) return describe(*c);
    // Texto entre comillas: sin ellas.
    if (rest.size() >= 2 && rest.front() == '"' && rest.back() == '"') rest = rest.substr(1, rest.size() - 2);
    std::string error;
    if (!set(c->name(), rest, &error)) return "error: " + error;
    return describe(*c);
}

}  // namespace cramion::cvar

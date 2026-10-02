#include "CramionCore/gameplay/Localization.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace cramion::gameplay {

namespace {

constexpr const char* kSettingsFile = "settings.json";

bool readText(const std::filesystem::path& file, std::string& out) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    // BOM de UTF-8 (Excel lo pone al guardar CSV).
    if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF && static_cast<unsigned char>(out[1]) == 0xBB &&
        static_cast<unsigned char>(out[2]) == 0xBF) {
        out.erase(0, 3);
    }
    return true;
}

bool writeText(const std::filesystem::path& file, const std::string& text) {
    std::error_code e;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), e);
    // Escritura segura: a un temporal y luego se renombra.
    std::filesystem::path temp = file;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << text;
        if (!out) return false;
    }
    std::filesystem::rename(temp, file, e);
    if (e) {
        std::filesystem::remove(file, e);
        e.clear();
        std::filesystem::rename(temp, file, e);
    }
    return !e;
}

std::string lowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// "es-ES" / "es_ES.UTF-8" -> "es"
std::string baseLanguage(const std::string& code) {
    const std::size_t cut = code.find_first_of("-_.@");
    return lowerAscii(cut == std::string::npos ? code : code.substr(0, cut));
}

std::string numberText(double value) {
    if (std::isfinite(value) && std::floor(value) == value && std::fabs(value) < 1e15) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%.0f", value);
        return buffer;
    }
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%g", value);
    return buffer;
}

// --- CSV (RFC 4180: comillas dobles, saltos de linea dentro de comillas) ---

std::vector<std::vector<std::string>> parseCsv(const std::string& text) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string cell;
    bool quoted = false;
    bool any = false;
    // Separador: coma, o punto y coma si la cabecera lo usa (Excel en espanol).
    char sep = ',';
    {
        const std::size_t line_end = text.find('\n');
        const std::string head = text.substr(0, line_end);
        if (std::count(head.begin(), head.end(), ';') > std::count(head.begin(), head.end(), ',')) sep = ';';
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quoted) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') {
                    cell += '"';
                    ++i;
                } else {
                    quoted = false;
                }
            } else {
                cell += c;
            }
            continue;
        }
        if (c == '"') {
            quoted = true;
            any = true;
        } else if (c == sep) {
            row.push_back(std::move(cell));
            cell.clear();
            any = true;
        } else if (c == '\r') {
            // se ignora (CRLF)
        } else if (c == '\n') {
            row.push_back(std::move(cell));
            cell.clear();
            if (any || row.size() > 1 || !row[0].empty()) rows.push_back(std::move(row));
            row.clear();
            any = false;
        } else {
            cell += c;
            any = true;
        }
    }
    if (any || !cell.empty() || !row.empty()) {
        row.push_back(std::move(cell));
        rows.push_back(std::move(row));
    }
    return rows;
}

std::string csvCell(const std::string& value) {
    if (value.find_first_of(",;\"\n\r") == std::string::npos) return value;
    std::string out = "\"";
    for (const char c : value) {
        if (c == '"') out += '"';
        out += c;
    }
    out += '"';
    return out;
}

std::string trim(const std::string& s) {
    std::size_t a = 0;
    std::size_t b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

}  // namespace

// --- Archivos ---

bool Localization::load(const std::filesystem::path& folder, std::string* error) {
    languages.clear();
    table.clear();
    default_language = "es";
    fallback_language.clear();
    use_system_language = true;

    std::string text;
    if (!readText(folder / kSettingsFile, text)) {
        // Proyecto sin localizacion: espanol e ingles vacios.
        languages = {{"es", "Español"}, {"en", "English"}};
        fallback_language = "en";
        language_ = default_language;
        touch();
        return true;
    }
    const nlohmann::json settings = nlohmann::json::parse(text, nullptr, false);
    if (settings.is_discarded() || !settings.is_object()) {
        if (error) *error = "settings.json no es JSON valido";
        languages = {{"es", "Español"}, {"en", "English"}};
        language_ = default_language;
        touch();
        return false;
    }
    if (const auto it = settings.find("languages"); it != settings.end() && it->is_array()) {
        for (const auto& l : *it) {
            LocalizationLanguage lang;
            if (l.is_string()) {
                lang.code = l.get<std::string>();
                lang.name = lang.code;
            } else if (l.is_object()) {
                lang.code = l.value("code", std::string());
                lang.name = l.value("name", lang.code);
            }
            if (!lang.code.empty() && !hasLanguage(lang.code)) languages.push_back(lang);
        }
    }
    default_language = settings.value("default", languages.empty() ? std::string("es") : languages.front().code);
    fallback_language = settings.value("fallback", std::string());
    use_system_language = settings.value("use_system_language", true);
    if (languages.empty()) languages.push_back({default_language, default_language});

    bool ok = true;
    for (const LocalizationLanguage& lang : languages) {
        std::string body;
        if (!readText(folder / (lang.code + ".json"), body)) continue;
        const nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
        if (j.is_discarded() || !j.is_object()) {
            if (error) *error = lang.code + ".json no es JSON valido";
            ok = false;
            continue;
        }
        const nlohmann::json& strings = j.contains("strings") ? j["strings"] : j;
        if (!strings.is_object()) continue;
        for (const auto& [key, value] : strings.items()) {
            if (key == "language" || key == "name") continue;
            if (value.is_string()) {
                table[key][lang.code] = value.get<std::string>();
            } else if (value.is_object()) {
                // Plurales en forma de objeto: {"one": "...", "other": "..."}
                for (const auto& [form, v] : value.items()) {
                    if (v.is_string()) table[key + "#" + form][lang.code] = v.get<std::string>();
                }
            }
        }
    }

    // Idioma inicial.
    std::string chosen = default_language;
    if (use_system_language) {
        const std::string system = systemLanguage();
        for (const LocalizationLanguage& l : languages) {
            if (l.code == system || baseLanguage(l.code) == system) {
                chosen = l.code;
                break;
            }
        }
    }
    language_ = hasLanguage(chosen) ? chosen : languages.front().code;
    touch();
    return ok;
}

bool Localization::save(const std::filesystem::path& folder, std::string* error) const {
    nlohmann::json settings;
    settings["languages"] = nlohmann::json::array();
    for (const LocalizationLanguage& l : languages) settings["languages"].push_back({{"code", l.code}, {"name", l.name}});
    settings["default"] = default_language;
    settings["fallback"] = fallback_language;
    settings["use_system_language"] = use_system_language;
    if (!writeText(folder / kSettingsFile, settings.dump(2))) {
        if (error) *error = "no se pudo escribir " + (folder / kSettingsFile).string();
        return false;
    }
    for (const LocalizationLanguage& l : languages) {
        nlohmann::json j;
        j["language"] = l.code;
        j["name"] = l.name;
        nlohmann::json strings = nlohmann::json::object();
        for (const auto& [key, values] : table) {
            const auto it = values.find(l.code);
            if (it != values.end()) strings[key] = it->second;
        }
        j["strings"] = std::move(strings);
        if (!writeText(folder / (l.code + ".json"), j.dump(2, ' ', false, nlohmann::json::error_handler_t::replace))) {
            if (error) *error = "no se pudo escribir " + l.code + ".json";
            return false;
        }
    }
    return true;
}

std::string Localization::toCsv() const {
    std::string out = "key";
    for (const LocalizationLanguage& l : languages) out += "," + csvCell(l.code);
    out += "\n";
    for (const auto& [key, values] : table) {
        out += csvCell(key);
        for (const LocalizationLanguage& l : languages) {
            const auto it = values.find(l.code);
            out += ",";
            if (it != values.end()) out += csvCell(it->second);
        }
        out += "\n";
    }
    return out;
}

bool Localization::fromCsv(const std::string& text, bool merge, std::string* error) {
    const auto rows = parseCsv(text);
    if (rows.empty() || rows[0].size() < 2) {
        if (error) *error = "el CSV necesita una cabecera: key,es,en,...";
        return false;
    }
    std::vector<std::string> codes;
    for (std::size_t c = 1; c < rows[0].size(); ++c) {
        const std::string code = trim(rows[0][c]);
        codes.push_back(code);
        if (!code.empty() && !hasLanguage(code)) addLanguage(code, code);
    }
    if (!merge) table.clear();
    for (std::size_t r = 1; r < rows.size(); ++r) {
        const auto& row = rows[r];
        if (row.empty()) continue;
        const std::string key = trim(row[0]);
        if (key.empty() || key[0] == '#') continue;  // comentario
        auto& values = table[key];
        for (std::size_t c = 1; c < row.size() && c - 1 < codes.size(); ++c) {
            if (codes[c - 1].empty()) continue;
            if (row[c].empty()) {
                values.erase(codes[c - 1]);
            } else {
                values[codes[c - 1]] = row[c];
            }
        }
    }
    touch();
    return true;
}

bool Localization::importCsv(const std::filesystem::path& file, bool merge, std::string* error) {
    std::string text;
    if (!readText(file, text)) {
        if (error) *error = "no se pudo leer el archivo";
        return false;
    }
    return fromCsv(text, merge, error);
}

bool Localization::exportCsv(const std::filesystem::path& file, std::string* error) const {
    // Con BOM: Excel lo abre como UTF-8 (acentos bien).
    if (!writeText(file, "\xEF\xBB\xBF" + toCsv())) {
        if (error) *error = "no se pudo escribir el archivo";
        return false;
    }
    return true;
}

// --- Edicion ---

bool Localization::hasLanguage(const std::string& code) const {
    return std::any_of(languages.begin(), languages.end(), [&](const LocalizationLanguage& l) { return l.code == code; });
}

void Localization::addLanguage(const std::string& code, const std::string& name) {
    if (code.empty() || hasLanguage(code)) return;
    languages.push_back({code, name.empty() ? code : name});
    touch();
}

void Localization::removeLanguage(const std::string& code) {
    languages.erase(std::remove_if(languages.begin(), languages.end(), [&](const LocalizationLanguage& l) { return l.code == code; }),
                    languages.end());
    for (auto& [key, values] : table) values.erase(code);
    if (language_ == code) language_ = languages.empty() ? default_language : languages.front().code;
    if (default_language == code && !languages.empty()) default_language = languages.front().code;
    if (fallback_language == code) fallback_language.clear();
    touch();
}

void Localization::setText(const std::string& key, const std::string& language, const std::string& text) {
    if (key.empty()) return;
    if (text.empty()) {
        // La clave se queda (aunque quede vacia) para que no desaparezca de la tabla.
        table[key].erase(language);
    } else {
        table[key][language] = text;
    }
    touch();
}

void Localization::removeKey(const std::string& key) {
    table.erase(key);
    touch();
}

void Localization::renameKey(const std::string& from, const std::string& to) {
    if (from == to || to.empty()) return;
    const auto it = table.find(from);
    if (it == table.end()) return;
    auto values = std::move(it->second);
    table.erase(it);
    auto& target = table[to];
    for (auto& [lang, text] : values) target[lang] = std::move(text);
    touch();
}

std::vector<std::string> Localization::missing(const std::string& language) const {
    std::vector<std::string> out;
    for (const auto& [key, values] : table) {
        const auto it = values.find(language);
        if (it == values.end() || it->second.empty()) out.push_back(key);
    }
    return out;
}

// --- Uso ---

bool Localization::setLanguage(const std::string& code) {
    std::string wanted = code;
    if (!hasLanguage(wanted)) {
        // "es-MX" -> "es" si solo hay "es"
        const std::string base = baseLanguage(code);
        bool found = false;
        for (const LocalizationLanguage& l : languages) {
            if (baseLanguage(l.code) == base) {
                wanted = l.code;
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    if (wanted == language_) return true;
    language_ = wanted;
    touch();
    // Copia: un oyente puede quitarse a si mismo.
    const auto listeners = listeners_;
    for (const auto& [id, fn] : listeners) {
        if (fn) fn(language_);
    }
    return true;
}

const std::string* Localization::lookup(const std::string& key, const std::string& language) const {
    const auto it = table.find(key);
    if (it == table.end()) return nullptr;
    const auto v = it->second.find(language);
    if (v == it->second.end() || v->second.empty()) return nullptr;
    return &v->second;
}

bool Localization::has(const std::string& key) const { return table.contains(key); }

bool Localization::has(const std::string& key, const std::string& language) const {
    return lookup(key, language) != nullptr;
}

std::string Localization::get(const std::string& key) const { return get(key, {}, {}); }

std::string Localization::get(const std::string& key, const std::vector<std::string>& args,
                              const std::map<std::string, std::string>& named) const {
    const std::string* text = lookup(key, language_);
    if (text == nullptr && !fallback_language.empty()) text = lookup(key, fallback_language);
    if (text == nullptr && default_language != language_) text = lookup(key, default_language);
    if (text == nullptr) {
        // Sin traduccion: la clave tal cual (se ve en el juego y se arregla).
        return args.empty() && named.empty() ? key : format(key, args, named);
    }
    return args.empty() && named.empty() ? *text : format(*text, args, named);
}

std::string Localization::plural(const std::string& key, double count, const std::vector<std::string>& args,
                                 const std::map<std::string, std::string>& named) const {
    std::map<std::string, std::string> all = named;
    all.try_emplace("n", numberText(count));
    std::vector<std::string> forms;
    if (count == 0.0) forms.push_back("zero");  // "#zero" opcional en cualquier idioma
    forms.push_back(pluralCategory(language_, count));
    forms.push_back("other");
    for (const std::string& form : forms) {
        const std::string candidate = key + "#" + form;
        if (lookup(candidate, language_) != nullptr ||
            (!fallback_language.empty() && lookup(candidate, fallback_language) != nullptr)) {
            return get(candidate, args, all);
        }
    }
    return get(key, args, all);
}

std::string Localization::systemLanguage() {
#if defined(_WIN32)
    wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
    if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) > 0) {
        std::string ascii;
        for (const wchar_t* p = name; *p != 0; ++p) ascii += static_cast<char>(*p < 128 ? *p : '?');
        return baseLanguage(ascii);
    }
    return "en";
#else
    for (const char* var : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        if (const char* v = std::getenv(var); v != nullptr && *v != '\0' && std::string(v) != "C" && std::string(v) != "POSIX") {
            return baseLanguage(v);
        }
    }
    return "en";
#endif
}

std::string Localization::pluralCategory(const std::string& language, double count) {
    const std::string lang = baseLanguage(language);
    const double n = std::fabs(count);
    const bool integer = std::floor(n) == n;
    const long long i = static_cast<long long>(n);
    // Reglas de CLDR simplificadas para los idiomas mas comunes.
    if (lang == "ja" || lang == "zh" || lang == "ko" || lang == "th" || lang == "vi" || lang == "id") return "other";
    const bool portuguese_br = lang == "pt" && lowerAscii(language).find("br") != std::string::npos;
    if (lang == "fr" || portuguese_br) {
        return n < 2.0 ? "one" : (integer && i != 0 && i % 1000000 == 0 ? "many" : "other");
    }
    if (lang == "ru" || lang == "uk" || lang == "be" || lang == "sr" || lang == "hr" || lang == "bs") {
        if (!integer) return "other";
        if (i % 10 == 1 && i % 100 != 11) return "one";
        if (i % 10 >= 2 && i % 10 <= 4 && (i % 100 < 12 || i % 100 > 14)) return "few";
        return "many";
    }
    if (lang == "pl") {
        if (!integer) return "other";
        if (i == 1) return "one";
        if (i % 10 >= 2 && i % 10 <= 4 && (i % 100 < 12 || i % 100 > 14)) return "few";
        return "many";
    }
    if (lang == "cs" || lang == "sk") {
        if (!integer) return "many";
        if (i == 1) return "one";
        if (i >= 2 && i <= 4) return "few";
        return "other";
    }
    if (lang == "ar") {
        if (!integer) return "other";
        if (i == 0) return "zero";
        if (i == 1) return "one";
        if (i == 2) return "two";
        if (i % 100 >= 3 && i % 100 <= 10) return "few";
        if (i % 100 >= 11) return "many";
        return "other";
    }
    // es, en, de, it, pt, nl, sv...: 1 = "one"
    if (lang == "es" || lang == "it" || lang == "pt") {
        if (integer && i == 1) return "one";
        if (integer && i != 0 && i % 1000000 == 0) return "many";
        return "other";
    }
    return integer && i == 1 ? "one" : "other";
}

std::string Localization::format(const std::string& text, const std::vector<std::string>& args,
                                 const std::map<std::string, std::string>& named) {
    std::string out;
    out.reserve(text.size() + 16);
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '{' && i + 1 < text.size() && text[i + 1] == '{') {
            out += '{';
            ++i;
            continue;
        }
        if (c == '}' && i + 1 < text.size() && text[i + 1] == '}') {
            out += '}';
            ++i;
            continue;
        }
        if (c != '{') {
            out += c;
            continue;
        }
        const std::size_t close = text.find('}', i + 1);
        if (close == std::string::npos) {
            out += text.substr(i);
            break;
        }
        const std::string name = text.substr(i + 1, close - i - 1);
        bool replaced = false;
        if (!name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
            const std::size_t index = static_cast<std::size_t>(std::strtoul(name.c_str(), nullptr, 10));
            if (index < args.size()) {
                out += args[index];
                replaced = true;
            }
        } else if (const auto it = named.find(name); it != named.end()) {
            out += it->second;
            replaced = true;
        }
        if (!replaced) out += text.substr(i, close - i + 1);  // se deja tal cual
        i = close;
    }
    return out;
}

int Localization::addListener(std::function<void(const std::string& language)> fn) {
    const int id = next_listener_++;
    listeners_[id] = std::move(fn);
    return id;
}

void Localization::removeListener(int id) { listeners_.erase(id); }

Localization& localization() {
    static Localization instance;
    return instance;
}

}  // namespace cramion::gameplay

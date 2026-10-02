#ifndef CRAMION_CORE_GAMEPLAY_LOCALIZATION_H
#define CRAMION_CORE_GAMEPLAY_LOCALIZATION_H

// Localizacion (textos del juego en varios idiomas), como el paquete
// Localization de Unity o las String Tables de Unreal.
//
// Datos del proyecto en ProjectSettings/Localization/:
//   settings.json   {"languages":[{"code":"es","name":"Espanol"}, ...],
//                    "default":"es", "fallback":"en", "use_system_language":true}
//   <codigo>.json   {"language":"es", "strings":{"menu.jugar":"Jugar", ...}}
// Se importa/exporta a CSV (clave,es,en,...) para pasarlo a traductores.
//
// Textos con argumentos: "Hola {0}, tienes {1} monedas" o con nombre
// "{jugador}". Plurales: claves con sufijo "#zero", "#one", "#few", "#many" y
// "#other" (monedas#one = "{n} moneda", monedas#other = "{n} monedas"); la
// regla de cada idioma elige la forma.
//
// Hay una tabla global (localization()) que usan la UI (Texto con clave), los
// dialogos y Lua (Text.get).

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace cramion::gameplay {

struct LocalizationLanguage {
    std::string code;  // "es", "en", "pt-BR"...
    std::string name;  // "Espanol"
};

class Localization {
public:
    // --- Datos ---
    std::vector<LocalizationLanguage> languages;
    std::string default_language = "es";
    std::string fallback_language;  // vacio = el idioma por defecto
    bool use_system_language = true;
    // clave -> idioma -> texto
    std::map<std::string, std::map<std::string, std::string>> table;

    // Carpeta ProjectSettings/Localization (o la de un juego exportado).
    // Sin archivos deja una tabla vacia con espanol e ingles. Elige idioma:
    // el del sistema si esta y use_system_language, si no el de por defecto.
    bool load(const std::filesystem::path& folder, std::string* error = nullptr);
    bool save(const std::filesystem::path& folder, std::string* error = nullptr) const;
    // CSV: primera fila "key,es,en,..." (las columnas de idiomas nuevos crean el
    // idioma). `merge`: conserva las claves que no vengan en el CSV.
    bool importCsv(const std::filesystem::path& file, bool merge = true, std::string* error = nullptr);
    bool exportCsv(const std::filesystem::path& file, std::string* error = nullptr) const;
    std::string toCsv() const;
    bool fromCsv(const std::string& text, bool merge = true, std::string* error = nullptr);

    // --- Edicion ---
    bool hasLanguage(const std::string& code) const;
    void addLanguage(const std::string& code, const std::string& name);
    void removeLanguage(const std::string& code);
    void setText(const std::string& key, const std::string& language, const std::string& text);
    void removeKey(const std::string& key);
    void renameKey(const std::string& from, const std::string& to);
    // Claves sin texto en ese idioma.
    std::vector<std::string> missing(const std::string& language) const;

    // --- Uso ---
    const std::string& language() const { return language_; }
    // false si el idioma no existe (no cambia). Avisa a los oyentes.
    bool setLanguage(const std::string& code);
    // Texto de la clave en el idioma actual (o el de respaldo, o la propia
    // clave si no existe). Los argumentos sustituyen {0}, {1}...
    std::string get(const std::string& key) const;
    std::string get(const std::string& key, const std::vector<std::string>& args,
                    const std::map<std::string, std::string>& named = {}) const;
    // Forma plural para `count` ({n} = el numero).
    std::string plural(const std::string& key, double count, const std::vector<std::string>& args = {},
                       const std::map<std::string, std::string>& named = {}) const;
    bool has(const std::string& key) const;
    bool has(const std::string& key, const std::string& language) const;
    // Idioma del sistema operativo ("es", "en"...).
    static std::string systemLanguage();
    // Categoria plural de un numero en un idioma: "zero", "one", "few", "many" u "other".
    static std::string pluralCategory(const std::string& language, double count);
    // Sustituye {0}, {1}, {nombre} en un texto ({{ y }} = llaves literales).
    static std::string format(const std::string& text, const std::vector<std::string>& args,
                              const std::map<std::string, std::string>& named = {});

    // Sube con cada cambio de idioma o de textos (la UI cachea por esto).
    std::uint64_t revision() const { return revision_; }
    void touch() { ++revision_; }

    // Oyentes del cambio de idioma (Lua: Text.onLanguageChanged).
    int addListener(std::function<void(const std::string& language)> fn);
    void removeListener(int id);

private:
    const std::string* lookup(const std::string& key, const std::string& language) const;
    std::string language_ = "es";
    std::uint64_t revision_ = 1;
    int next_listener_ = 1;
    std::map<int, std::function<void(const std::string&)>> listeners_;
};

// La tabla del juego (una por programa).
Localization& localization();

}  // namespace cramion::gameplay

#endif  // CRAMION_CORE_GAMEPLAY_LOCALIZATION_H

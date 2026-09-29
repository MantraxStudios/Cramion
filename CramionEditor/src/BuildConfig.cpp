#include "BuildConfig.h"

#include <CramionFX/asset/ImageFile.h>

#include <nlohmann/json.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>

namespace cramion::editor {

using json = nlohmann::json;

BuildConfig& BuildConfigs::current() {
    if (configs.empty()) configs.emplace_back();
    active = std::clamp(active, 0, static_cast<int>(configs.size()) - 1);
    return configs[static_cast<std::size_t>(active)];
}

BuildConfigs loadBuildConfigs(const std::filesystem::path& file) {
    BuildConfigs out;
    std::ifstream in(file, std::ios::binary);
    if (in) {
        try {
            const json root = json::parse(in);
            out.active = root.value("active", 0);
            for (const json& c : root.value("configs", json::array())) {
                BuildConfig b;
                b.name = c.value("name", b.name);
                b.game_name = c.value("game_name", b.game_name);
                b.version = c.value("version", b.version);
                b.version_in_title = c.value("version_in_title", b.version_in_title);
                b.icon = c.value("icon", b.icon);
                b.startup_scene = c.value("startup_scene", b.startup_scene);
                b.window_mode = std::clamp(c.value("window_mode", b.window_mode), 0, 2);
                b.width = std::clamp(c.value("width", b.width), 320, 16384);
                b.height = std::clamp(c.value("height", b.height), 240, 16384);
                b.static_batching = c.value("static_batching", b.static_batching);
                b.show_fps = c.value("show_fps", b.show_fps);
                b.vr = c.value("vr", b.vr);
                b.platform = c.value("platform", std::string("windows")) == "android" ? BuildPlatform::Android
                                                                                       : BuildPlatform::Windows;
                if (const auto a = c.find("android"); a != c.end() && a->is_object()) {
                    AndroidBuildSettings& s = b.android;
                    s.package = a->value("package", s.package);
                    s.version_code = std::max(1, a->value("version_code", s.version_code));
                    s.min_sdk = std::clamp(a->value("min_sdk", s.min_sdk), 26, 40);
                    s.target_sdk = std::clamp(a->value("target_sdk", s.target_sdk), s.min_sdk, 40);
                    s.orientation = std::clamp(a->value("orientation", s.orientation), 0, 4);
                    s.make_apk = a->value("apk", s.make_apk);
                    s.make_aab = a->value("aab", s.make_aab);
                    if (!s.make_apk && !s.make_aab) s.make_apk = true;
                    s.split_obb = a->value("obb", s.split_obb);
                    s.x86_64 = a->value("x86_64", s.x86_64);
                    s.internet = a->value("internet", s.internet);
                    s.vibrate = a->value("vibrate", s.vibrate);
                    s.record_audio = a->value("record_audio", s.record_audio);
                    s.icon = a->value("icon", s.icon);
                    s.keystore = a->value("keystore", s.keystore);
                    s.key_alias = a->value("key_alias", s.key_alias);
                    s.quality = std::clamp(a->value("quality", s.quality), 0, 3);
                    s.target_fps = std::clamp(a->value("target_fps", s.target_fps), 15, 240);
                }
                out.configs.push_back(std::move(b));
            }
        } catch (const std::exception&) {
            out.configs.clear();
        }
    }
    if (out.configs.empty()) out.configs.emplace_back();
    out.current();  // active dentro del rango
    return out;
}

bool saveBuildConfigs(const std::filesystem::path& file, const BuildConfigs& configs) {
    json list = json::array();
    for (const BuildConfig& b : configs.configs) {
        list.push_back({{"name", b.name},
                        {"game_name", b.game_name},
                        {"version", b.version},
                        {"version_in_title", b.version_in_title},
                        {"icon", b.icon},
                        {"startup_scene", b.startup_scene},
                        {"window_mode", b.window_mode},
                        {"width", b.width},
                        {"height", b.height},
                        {"static_batching", b.static_batching},
                        {"show_fps", b.show_fps},
                        {"vr", b.vr},
                        {"platform", b.platform == BuildPlatform::Android ? "android" : "windows"}});
        const AndroidBuildSettings& s = b.android;
        list.back()["android"] = {{"package", s.package},
                                  {"version_code", s.version_code},
                                  {"min_sdk", s.min_sdk},
                                  {"target_sdk", s.target_sdk},
                                  {"orientation", s.orientation},
                                  {"apk", s.make_apk},
                                  {"aab", s.make_aab},
                                  {"obb", s.split_obb},
                                  {"x86_64", s.x86_64},
                                  {"internet", s.internet},
                                  {"vibrate", s.vibrate},
                                  {"record_audio", s.record_audio},
                                  {"icon", s.icon},
                                  {"keystore", s.keystore},
                                  {"key_alias", s.key_alias},
                                  {"quality", s.quality},
                                  {"target_fps", s.target_fps}};
    }
    const json root = {{"format", "CramionBuildConfigs"}, {"version", 1}, {"active", configs.active}, {"configs", list}};
    std::error_code e;
    std::filesystem::create_directories(file.parent_path(), e);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << root.dump(2);
    return static_cast<bool>(out);
}

std::string buildConfigIni(const BuildConfig& config, const std::string& game_name) {
    std::string title = game_name;
    if (config.version_in_title && !config.version.empty()) title += " " + config.version;
    std::ostringstream ini;
    ini << "title=" << title << "\n";
    ini << "version=" << config.version << "\n";
    ini << "window=" << config.window_mode << "\n";
    ini << "width=" << config.width << "\n";
    ini << "height=" << config.height << "\n";
    ini << "show_fps=" << (config.show_fps ? 1 : 0) << "\n";
    if (config.vr && config.platform == BuildPlatform::Windows) ini << "vr=1\n";
    if (config.platform == BuildPlatform::Android) {
        ini << "platform=android\n";
        ini << "android_quality=" << config.android.quality << "\n";
        ini << "android_fps=" << config.android.target_fps << "\n";
    }
    return ini.str();
}

// -----------------------------------------------------------------------------
// Icono del .exe
// -----------------------------------------------------------------------------

namespace {

std::string lowerExtension(const std::filesystem::path& file) {
    std::string e = file.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e;
}

struct IconImage {
    int width = 0;   // 256 se escribe 0 en el directorio
    int height = 0;
    int colors = 0;
    int planes = 1;
    int bits = 32;
    std::vector<std::uint8_t> data;  // DIB (o PNG si venia asi en el .ico)
};

#pragma pack(push, 2)
struct GroupHeader {
    WORD reserved;
    WORD type;
    WORD count;
};
struct GroupEntry {
    BYTE width;
    BYTE height;
    BYTE colors;
    BYTE reserved;
    WORD planes;
    WORD bits;
    DWORD bytes;
    WORD id;
};
#pragma pack(pop)

template <typename T>
T readAt(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    T value{};
    if (offset + sizeof(T) <= bytes.size()) std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

// Un .ico: sus imagenes tal cual (DIB o PNG).
bool readIco(const std::filesystem::path& file, std::vector<IconImage>& images, std::string* error) {
    std::ifstream in(file, std::ios::binary);
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() < 6 || readAt<WORD>(bytes, 0) != 0 || readAt<WORD>(bytes, 2) != 1) {
        if (error) *error = "no es un .ico valido";
        return false;
    }
    const int count = readAt<WORD>(bytes, 4);
    for (int i = 0; i < count; ++i) {
        const std::size_t entry = 6 + static_cast<std::size_t>(i) * 16;
        if (entry + 16 > bytes.size()) break;
        IconImage image;
        image.width = bytes[entry] == 0 ? 256 : bytes[entry];
        image.height = bytes[entry + 1] == 0 ? 256 : bytes[entry + 1];
        image.colors = bytes[entry + 2];
        image.planes = readAt<WORD>(bytes, entry + 4);
        image.bits = readAt<WORD>(bytes, entry + 6);
        const DWORD size = readAt<DWORD>(bytes, entry + 8);
        const DWORD offset = readAt<DWORD>(bytes, entry + 12);
        if (static_cast<std::size_t>(offset) + size > bytes.size() || size == 0) continue;
        image.data.assign(bytes.begin() + offset, bytes.begin() + offset + size);
        images.push_back(std::move(image));
    }
    if (images.empty() && error) *error = "el .ico no tiene imagenes";
    return !images.empty();
}

// Imagen cuadrada de `size` px (centrada, fondo transparente) como DIB de
// 32 bits para un icono.
IconImage dibIcon(const asset::ImageRgba8& source, int size) {
    IconImage icon;
    icon.width = icon.height = size;
    const std::size_t row_mask = static_cast<std::size_t>((size + 31) / 32) * 4;
    BITMAPINFOHEADER header{};
    header.biSize = sizeof(BITMAPINFOHEADER);
    header.biWidth = size;
    header.biHeight = size * 2;  // color + mascara
    header.biPlanes = 1;
    header.biBitCount = 32;
    header.biCompression = BI_RGB;
    header.biSizeImage = static_cast<DWORD>(static_cast<std::size_t>(size) * size * 4 + row_mask * size);
    icon.data.resize(sizeof(header) + header.biSizeImage, 0);
    std::memcpy(icon.data.data(), &header, sizeof(header));
    std::uint8_t* pixels = icon.data.data() + sizeof(header);
    const int ox = (size - static_cast<int>(source.width)) / 2;
    const int oy = (size - static_cast<int>(source.height)) / 2;
    for (int y = 0; y < static_cast<int>(source.height); ++y) {
        const int ty = oy + y;
        if (ty < 0 || ty >= size) continue;
        const int row = size - 1 - ty;  // DIB: de abajo arriba
        for (int x = 0; x < static_cast<int>(source.width); ++x) {
            const int tx = ox + x;
            if (tx < 0 || tx >= size) continue;
            const std::uint8_t* s = &source.pixels[(static_cast<std::size_t>(y) * source.width + x) * 4];
            std::uint8_t* d = pixels + (static_cast<std::size_t>(row) * size + tx) * 4;
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0];
            d[3] = s[3];
        }
    }
    return icon;  // la mascara AND queda a cero: manda el alfa
}

bool makeIconImages(const std::filesystem::path& file, std::vector<IconImage>& images, std::string* error) {
    if (lowerExtension(file) == ".ico") return readIco(file, images, error);
    asset::ImageRgba8 full;
    if (!asset::loadImageRgba8(file, full) || full.width == 0 || full.height == 0) {
        if (error) *error = "no se pudo leer la imagen";
        return false;
    }
    const int largest = static_cast<int>(std::max(full.width, full.height));
    for (const int size : {256, 128, 64, 48, 32, 24, 16}) {
        if (size > largest && size != 16) continue;  // sin agrandar (16 siempre)
        asset::ImageRgba8 image;
        if (size >= largest) {
            image = full;
        } else if (!asset::loadImageRgba8(file, image, static_cast<std::uint32_t>(size))) {
            continue;
        }
        images.push_back(dibIcon(image, size));
    }
    if (images.empty() && error) *error = "no se pudo hacer el icono";
    return !images.empty();
}

struct ResourceId {
    WORD id;
    WORD language;
};

BOOL CALLBACK collectLanguages(HMODULE, LPCWSTR, LPCWSTR, WORD language, LONG_PTR param) {
    reinterpret_cast<std::vector<WORD>*>(param)->push_back(language);
    return TRUE;
}

BOOL CALLBACK collectIcons(HMODULE module, LPCWSTR type, LPWSTR name, LONG_PTR param) {
    if (!IS_INTRESOURCE(name)) return TRUE;
    std::vector<WORD> languages;
    EnumResourceLanguagesW(module, type, name, collectLanguages, reinterpret_cast<LONG_PTR>(&languages));
    auto* out = reinterpret_cast<std::vector<ResourceId>*>(param);
    for (const WORD language : languages) out->push_back(ResourceId{static_cast<WORD>(reinterpret_cast<ULONG_PTR>(name)), language});
    return TRUE;
}

}  // namespace

bool isIconSource(const std::filesystem::path& file) {
    const std::string e = lowerExtension(file);
    return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".tga" || e == ".bmp" || e == ".ico";
}

bool setExeIcon(const std::filesystem::path& exe, const std::filesystem::path& image, std::string* error) {
    std::vector<IconImage> images;
    if (!makeIconImages(image, images, error)) return false;

    // Lo que ya tiene: el grupo 1 (idioma) y sus imagenes (se quitan las viejas).
    std::vector<WORD> group_languages;
    std::vector<ResourceId> old_icons;
    if (HMODULE module = LoadLibraryExW(exe.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE)) {
        EnumResourceLanguagesW(module, RT_GROUP_ICON, MAKEINTRESOURCEW(1), collectLanguages,
                               reinterpret_cast<LONG_PTR>(&group_languages));
        EnumResourceNamesW(module, RT_ICON, collectIcons, reinterpret_cast<LONG_PTR>(&old_icons));
        FreeLibrary(module);
    }
    const WORD language = group_languages.empty() ? MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL) : group_languages.front();

    std::vector<std::uint8_t> group(sizeof(GroupHeader) + sizeof(GroupEntry) * images.size());
    GroupHeader header{0, 1, static_cast<WORD>(images.size())};
    std::memcpy(group.data(), &header, sizeof(header));
    for (std::size_t i = 0; i < images.size(); ++i) {
        const IconImage& img = images[i];
        GroupEntry entry{};
        entry.width = static_cast<BYTE>(img.width >= 256 ? 0 : img.width);
        entry.height = static_cast<BYTE>(img.height >= 256 ? 0 : img.height);
        entry.colors = static_cast<BYTE>(img.colors);
        entry.planes = static_cast<WORD>(img.planes);
        entry.bits = static_cast<WORD>(img.bits);
        entry.bytes = static_cast<DWORD>(img.data.size());
        entry.id = static_cast<WORD>(i + 1);
        std::memcpy(group.data() + sizeof(GroupHeader) + i * sizeof(GroupEntry), &entry, sizeof(entry));
    }

    // El antivirus puede tener el .exe recien copiado abierto un momento.
    for (int attempt = 0; attempt < 6; ++attempt) {
        HANDLE update = BeginUpdateResourceW(exe.c_str(), FALSE);
        if (update == nullptr) {
            Sleep(250);
            continue;
        }
        bool ok = true;
        for (const ResourceId& old : old_icons) {
            const bool replaced = old.language == language && old.id >= 1 && old.id <= images.size();
            if (!replaced) UpdateResourceW(update, RT_ICON, MAKEINTRESOURCEW(old.id), old.language, nullptr, 0);
        }
        for (const WORD other : group_languages) {
            if (other != language) UpdateResourceW(update, RT_GROUP_ICON, MAKEINTRESOURCEW(1), other, nullptr, 0);
        }
        for (std::size_t i = 0; i < images.size() && ok; ++i) {
            ok = UpdateResourceW(update, RT_ICON, MAKEINTRESOURCEW(static_cast<WORD>(i + 1)), language,
                                 images[i].data.data(), static_cast<DWORD>(images[i].data.size())) != FALSE;
        }
        ok = ok && UpdateResourceW(update, RT_GROUP_ICON, MAKEINTRESOURCEW(1), language, group.data(),
                                   static_cast<DWORD>(group.size())) != FALSE;
        if (EndUpdateResourceW(update, ok ? FALSE : TRUE) && ok) return true;
        Sleep(250);
    }
    if (error) *error = "Windows no dejo cambiar el icono del .exe (error " + std::to_string(GetLastError()) + ")";
    return false;
}

}  // namespace cramion::editor

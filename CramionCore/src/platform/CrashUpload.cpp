#include "CramionCore/platform/CrashUpload.h"

#include "CramionCore/cvar/CVar.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

namespace cramion::crash {

namespace {

cvar::CVar<std::string> g_editor_url("crash.ReportUrl", std::string(),
                                     "Direccion (https) a la que el editor puede enviar el informe de un cierre "
                                     "(boton Enviar de la ventana del informe); vacia = no se envia nada",
                                     cvar::Saved);

// La cola la tocan el hilo de envio y el principal. Sin destructor (se
// pierde a proposito): el hilo de envio va suelto y puede seguir vivo
// mientras el proceso destruye sus estaticos al salir.
std::mutex& g_queue_mutex = *new std::mutex;
// Un envio en segundo plano a la vez: dos (el boton del editor pulsado dos
// veces, o el arranque y el boton) leerian la misma cola y mandarian dos
// veces el mismo informe.
std::atomic<bool> g_uploading{false};

std::filesystem::path queueFile(const std::filesystem::path& folder, const std::string& app) {
    return folder / (app + ".upload");
}

struct Entry {
    int attempts = 0;
    std::filesystem::path report;
};

std::vector<Entry> readQueue(const std::filesystem::path& file) {
    std::vector<Entry> entries;
    std::ifstream in(file, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t tab = line.find('\t');
        if (tab == std::string::npos || tab + 1 >= line.size()) continue;
        Entry e;
        e.attempts = std::atoi(line.substr(0, tab).c_str());
        const std::string path = line.substr(tab + 1);
        e.report = std::filesystem::path(std::u8string(path.begin(), path.end()));
        entries.push_back(std::move(e));
    }
    return entries;
}

void writeQueue(const std::filesystem::path& file, const std::vector<Entry>& entries) {
    std::error_code ec;
    if (entries.empty()) {
        std::filesystem::remove(file, ec);
        return;
    }
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    for (const Entry& e : entries) {
        const std::u8string path = e.report.u8string();
        out << e.attempts << '\t' << std::string(path.begin(), path.end()) << '\n';
    }
}

std::string tailOf(const std::filesystem::path& file, std::size_t max_bytes, bool& ok) {
    std::ifstream in(file, std::ios::binary);
    ok = static_cast<bool>(in);
    if (!in) return {};
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    const std::streamoff from = size > static_cast<std::streamoff>(max_bytes) ? size - static_cast<std::streamoff>(max_bytes) : 0;
    in.seekg(from);
    std::string out(static_cast<std::size_t>(size - from), '\0');
    in.read(out.data(), static_cast<std::streamsize>(out.size()));
    return out;
}

// El texto como UTF-8 valido (el JSON lo exige): los bytes sueltos que no lo
// son pasan a '?'.
std::string validUtf8(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        std::size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        bool good = n > 0 && i + n <= text.size();
        for (std::size_t k = 1; good && k < n; ++k) good = (static_cast<unsigned char>(text[i + k]) >> 6) == 2;
        if (!good) {
            out += '?';
            ++i;
            continue;
        }
        out.append(text, i, n);
        i += n;
    }
    return out;
}

}  // namespace

const char* platformName() {
#if defined(__ANDROID__)
    return "android";
#elif defined(_WIN32)
    return "windows";
#elif defined(__linux__)
    return "linux";
#else
    return "otro";
#endif
}

std::string editorReportUrl() { return g_editor_url.get(); }

std::string buildUploadBody(const UploadConfig& config, const std::string& report_text, const std::string& report_name) {
    std::string text = report_text;
    if (text.size() > config.max_bytes) text = text.substr(text.size() - config.max_bytes);
    nlohmann::json body = {{"format", "cramion-crash-1"},
                           {"app", validUtf8(config.app)},
                           {"version", validUtf8(config.version)},
                           {"platform", config.platform.empty() ? std::string(platformName()) : config.platform},
                           {"report_name", validUtf8(report_name)},
                           {"report", validUtf8(text)}};
    return body.dump();
}

bool buildUploadRequest(const UploadConfig& config, const std::filesystem::path& report, net::HttpRequest& out,
                        std::string* error) {
    if (config.url.empty()) {
        if (error != nullptr) *error = "sin direccion";
        return false;
    }
    if (!net::checkUrl(config.url, error)) return false;
    bool ok = false;
    const std::string text = tailOf(report, config.max_bytes, ok);
    if (!ok) {
        if (error != nullptr) *error = "no se pudo leer el informe";
        return false;
    }
    const std::u8string name = report.filename().u8string();
    out = net::HttpRequest{};
    out.method = "POST";
    out.url = config.url;
    out.headers = {{"Content-Type", "application/json; charset=utf-8"}, {"X-Cramion-Crash", "1"}};
    out.body = buildUploadBody(config, text, std::string(name.begin(), name.end()));
    out.timeout_ms = config.timeout_ms;
    out.max_response_bytes = 64 * 1024;
    return true;
}

void queueReport(const std::filesystem::path& crash_folder, const std::string& app, const std::filesystem::path& report) {
    if (report.empty()) return;
    const std::lock_guard<std::mutex> lock(g_queue_mutex);
    const std::filesystem::path file = queueFile(crash_folder, app);
    std::vector<Entry> entries = readQueue(file);
    for (const Entry& e : entries) {
        if (e.report == report) return;
    }
    entries.push_back(Entry{0, report});
    // Como mucho los 8 mas recientes (un juego que se cierra en bucle no
    // llena la cola).
    if (entries.size() > 8) entries.erase(entries.begin(), entries.end() - 8);
    writeQueue(file, entries);
}

std::vector<std::filesystem::path> queuedReports(const std::filesystem::path& crash_folder, const std::string& app) {
    const std::lock_guard<std::mutex> lock(g_queue_mutex);
    std::vector<std::filesystem::path> out;
    for (const Entry& e : readQueue(queueFile(crash_folder, app))) out.push_back(e.report);
    return out;
}

int uploadQueued(const UploadConfig& config, const std::filesystem::path& crash_folder, const std::string& app,
                 const Sender& send, int max_attempts) {
    if (config.url.empty() || !send) return 0;
    std::vector<Entry> entries;
    {
        const std::lock_guard<std::mutex> lock(g_queue_mutex);
        entries = readQueue(queueFile(crash_folder, app));
    }
    int sent = 0;
    std::vector<Entry> remaining;
    std::vector<std::filesystem::path> done;
    for (Entry& e : entries) {
        net::HttpRequest request;
        std::string error;
        if (!buildUploadRequest(config, e.report, request, &error)) {
            // Direccion mala: se queda para cuando se corrija; informe borrado: fuera.
            std::error_code ec;
            if (std::filesystem::exists(e.report, ec)) remaining.push_back(e);
            std::cerr << "[Crash] No se envio el informe " << e.report.filename().string() << ": " << error << "\n";
            continue;
        }
        const net::HttpResponse response = send(request);
        if (response.ok) {
            ++sent;
            done.push_back(e.report);
            std::cout << "[Crash] Informe " << e.report.filename().string() << " enviado\n";
            continue;
        }
        ++e.attempts;
        std::cerr << "[Crash] No se pudo enviar el informe " << e.report.filename().string() << " ("
                  << (response.status != 0 ? "estado " + std::to_string(response.status) : response.error) << ", intento "
                  << e.attempts << " de " << max_attempts << ")\n";
        if (e.attempts < max_attempts) remaining.push_back(e);
    }
    {
        // Lo que se anadio mientras tanto se conserva.
        const std::lock_guard<std::mutex> lock(g_queue_mutex);
        const std::filesystem::path file = queueFile(crash_folder, app);
        std::vector<Entry> now = readQueue(file);
        std::vector<Entry> merged = remaining;
        for (const Entry& e : now) {
            const bool known = std::any_of(entries.begin(), entries.end(), [&](const Entry& o) { return o.report == e.report; });
            if (!known) merged.push_back(e);
        }
        writeQueue(file, merged);
    }
    return sent;
}

bool uploadQueuedAsync(const UploadConfig& config, const std::filesystem::path& crash_folder, const std::string& app) {
    if (config.url.empty()) return false;
    // Ya hay uno en marcha: lo que se acaba de poner en la cola sale en el
    // siguiente (o en el proximo arranque).
    if (g_uploading.exchange(true)) return false;
    try {
        std::thread([config, crash_folder, app] {
            try {
                uploadQueued(config, crash_folder, app, [](const net::HttpRequest& r) { return net::httpRequest(r); });
            } catch (const std::exception& e) {
                std::cerr << "[Crash] Envio de informes: " << e.what() << "\n";
            }
            g_uploading = false;
        }).detach();
    } catch (const std::exception& e) {
        g_uploading = false;
        std::cerr << "[Crash] No se pudo lanzar el envio de informes: " << e.what() << "\n";
        return false;
    }
    return true;
}

}  // namespace cramion::crash

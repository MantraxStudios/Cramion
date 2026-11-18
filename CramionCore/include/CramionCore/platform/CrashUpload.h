#ifndef CRAMION_CORE_PLATFORM_CRASH_UPLOAD_H
#define CRAMION_CORE_PLATFORM_CRASH_UPLOAD_H

// Envio de los informes de cierre (los .txt de Crashes/, LoadingScreen.h y
// AndroidSupport.cpp) a un servidor del proyecto. Apagado por defecto: sin
// direccion no se envia nada.
//
//   - El juego exportado: `crash_report_url=` en game.ini (Configuracion de
//     compilacion > Informes de cierre). Al arrancar, el informe del cierre
//     anterior se pone en la cola y la cola se envia en segundo plano.
//   - El editor: CVar crash.ReportUrl; la ventana del informe tiene un boton
//     para enviarlo (solo si la hay).
//
// Peticion: POST <url> con JSON
//   {"format": "cramion-crash-1", "app", "version", "platform", "report_name",
//    "report": <texto, como mucho los ultimos max_bytes>}
// y la cabecera X-Cramion-Crash: 1. Solo https:// (o http:// a este PC para
// probar), como todo net::httpRequest. 2xx = recibido.
//
// Cola: Crashes/<app>.upload (una linea por informe: intentos<TAB>ruta). Lo
// que no se pudo enviar se reintenta en los arranques siguientes, como mucho
// `max_attempts` veces.

#include "CramionCore/net/Http.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace cramion::crash {

struct UploadConfig {
    std::string url;       // vacio = apagado
    std::string app;       // nombre del juego o "CramionEditor"
    std::string version;
    std::string platform;  // "windows", "linux", "android" (vacio = el de este binario)
    std::size_t max_bytes = 256 * 1024;
    int timeout_ms = 15000;
};

// La plataforma de este binario.
const char* platformName();
// La direccion del editor (CVar crash.ReportUrl).
std::string editorReportUrl();

std::string buildUploadBody(const UploadConfig& config, const std::string& report_text, const std::string& report_name);
// La peticion para un informe (sin enviarla). false si la direccion no vale
// o el informe no se puede leer.
bool buildUploadRequest(const UploadConfig& config, const std::filesystem::path& report, net::HttpRequest& out,
                        std::string* error = nullptr);

// Cola de envio.
void queueReport(const std::filesystem::path& crash_folder, const std::string& app, const std::filesystem::path& report);
std::vector<std::filesystem::path> queuedReports(const std::filesystem::path& crash_folder, const std::string& app);

// Envia la cola con `send` (las pruebas pasan uno que no sale a la red).
// Quita lo enviado y lo que llega a max_attempts; devuelve cuantos se
// enviaron. Sin direccion no hace nada.
using Sender = std::function<net::HttpResponse(const net::HttpRequest&)>;
int uploadQueued(const UploadConfig& config, const std::filesystem::path& crash_folder, const std::string& app,
                 const Sender& send, int max_attempts = 3);
// Lo mismo en un hilo aparte con net::httpRequest (no espera). Sin
// direccion no hace nada; con un envio ya en marcha tampoco (lo nuevo de la
// cola sale en el siguiente). true si lanzo el envio.
bool uploadQueuedAsync(const UploadConfig& config, const std::filesystem::path& crash_folder, const std::string& app);

}  // namespace cramion::crash

#endif  // CRAMION_CORE_PLATFORM_CRASH_UPLOAD_H

#ifndef CRAMION_CORE_NET_HTTP_H
#define CRAMION_CORE_NET_HTTP_H

// Peticiones HTTPS para hablar con servidores y webs (APIs REST, marcadores,
// cuentas, telemetria) con WinHTTP, la pila TLS de Windows.
//
// Seguridad:
//   - Solo https:// (TLS 1.2 o 1.3, certificado comprobado por Windows: si no
//     es valido o no es de ese dominio, falla). http:// solo a localhost /
//     127.0.0.1 / ::1, para probar con un servidor propio en el mismo PC.
//   - Las redirecciones nunca pasan de https a http.
//   - Cabeceras sin saltos de linea (no se pueden inyectar otras).
//   - Tiempo maximo y tamano maximo de la respuesta (una respuesta enorme no
//     llena la memoria).
//
// HttpClient las hace en hilos aparte (el juego no se para) y entrega las
// respuestas con poll(), en el hilo que llama.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cramion::net {

using HttpHeaders = std::vector<std::pair<std::string, std::string>>;

struct HttpRequest {
    std::string method = "GET";
    std::string url;
    HttpHeaders headers;
    std::string body;
    int timeout_ms = 20000;                       // total aproximado (conectar, enviar, recibir)
    std::size_t max_response_bytes = 32u << 20;   // 32 MB
};

struct HttpResponse {
    bool ok = false;          // llego respuesta y el estado es 2xx
    int status = 0;           // 0 = no hubo respuesta (ver error)
    std::string body;
    HttpHeaders headers;      // nombres en minusculas
    std::string error;        // vacio si hubo respuesta
    double seconds = 0.0;     // lo que tardo
};

// La direccion se puede usar (https, o http a este PC). false + motivo si no.
bool checkUrl(const std::string& url, std::string* error);
// Nombre y valor de cabecera validos (sin saltos de linea ni caracteres de control).
bool validHeader(const std::string& name, const std::string& value);
// Codifica para una URL (RFC 3986: letras, cifras y -._~ quedan igual).
std::string urlEncode(const std::string& text);

// Hace la peticion y espera la respuesta (bloquea: usar desde un hilo aparte
// o con HttpClient). `cancel` (opcional) la corta entre bloques.
HttpResponse httpRequest(const HttpRequest& request, const std::atomic<bool>* cancel = nullptr);

class HttpClient {
public:
    explicit HttpClient(int max_parallel = 6);
    ~HttpClient();  // cancela lo pendiente (no espera a los hilos)
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    // Encola la peticion; devuelve su id (> 0).
    std::uint32_t send(HttpRequest request);
    // Respuestas terminadas desde la ultima llamada (id, respuesta).
    std::vector<std::pair<std::uint32_t, HttpResponse>> poll();
    // Olvida todo lo pendiente (las respuestas que lleguen se tiran).
    void cancelAll();
    // En cola o en marcha.
    int pending() const;

private:
    struct Shared;
    std::shared_ptr<Shared> shared_;
    void startQueued();
};

}  // namespace cramion::net

#endif  // CRAMION_CORE_NET_HTTP_H

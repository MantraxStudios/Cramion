#ifndef CRAMION_CORE_NET_COMMON_H
#define CRAMION_CORE_NET_COMMON_H

// Comun de red (Http y WebSocket): direcciones locales, JavaVM y textos de error.

#include <string>
#if defined(__ANDROID__)
#include <jni.h>
#endif

namespace cramion::net::detail {

bool isLoopbackHost(const std::string& host);  // "localhost", 127.0.0.0/8, ::1 (literales, sin DNS)
bool isLoopbackIp(const std::string& ip);      // texto numerico (inet_ntop)
bool isPrivateIp(const std::string& ip);       // loopback + 10/8, 172.16/12, 192.168/16, 169.254/16, fc00::/7, fe80::/10
void* javaVm();                                // JavaVM* (Android; lo pone setJavaVM) o nullptr

#if defined(__ANDROID__)
// Engancha el hilo a la JavaVM si no lo estaba (y lo suelta al destruirse).
class JniThread {
public:
    JniThread();
    ~JniThread();
    JniThread(const JniThread&) = delete;
    JniThread& operator=(const JniThread&) = delete;
    JNIEnv* env() const { return env_; }  // nullptr si no hay JavaVM

private:
    JNIEnv* env_ = nullptr;
    bool attached_ = false;
};
// toString() de la excepcion pendiente (y la limpia); "" si no habia.
std::string javaExceptionText(JNIEnv* env);
#endif

// Textos (sin acentos, como Http.cpp).
inline constexpr const char* kErrNoHost = "no se encuentra el servidor (sin Internet o nombre mal escrito)";
inline constexpr const char* kErrRefused = "el servidor rechazo la conexion (no hay nada escuchando en ese puerto)";
inline constexpr const char* kErrTimeout = "el servidor no responde (tiempo agotado)";
inline constexpr const char* kErrCert = "conexion segura rechazada (el certificado del servidor no es valido)";
inline constexpr const char* kErrCut = "se corto la conexion";
inline constexpr const char* kErrNoTls = "wss:// no disponible en esta compilacion: en Linux compila con OpenSSL (apt install libssl-dev)";
inline constexpr const char* kErrPermission = "falta el permiso INTERNET (activa Internet al exportar para Android)";
inline constexpr const char* kErrNotLocal = "ws:// sin cifrar solo a la red local (esta direccion es publica)";

}  // namespace cramion::net::detail

#endif

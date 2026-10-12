#ifndef CRAMION_CORE_NET_WINHTTP_COMMON_H
#define CRAMION_CORE_NET_WINHTTP_COMMON_H

// Comun de WinHTTP (Http.cpp y WsEngineWinHttp.cpp): UTF-8 <-> UTF-16, handle
// que se cierra solo y el texto de los errores. Solo Windows.

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <string>

namespace cramion::net::detail {

inline std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

inline std::string narrow(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

struct Handle {
    HINTERNET h = nullptr;
    explicit Handle(HINTERNET handle = nullptr) : h(handle) {}
    ~Handle() {
        if (h != nullptr) WinHttpCloseHandle(h);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit operator bool() const { return h != nullptr; }
};

inline std::string winError(const char* what) {
    const DWORD code = GetLastError();
    std::string text = what;
    switch (code) {
        case ERROR_WINHTTP_NAME_NOT_RESOLVED: text += ": no se encuentra el servidor (sin Internet o nombre mal escrito)"; break;
        case ERROR_WINHTTP_TIMEOUT: text += ": el servidor no responde (tiempo agotado)"; break;
        case ERROR_WINHTTP_CANNOT_CONNECT: text += ": no se pudo conectar con el servidor"; break;
        case ERROR_WINHTTP_CONNECTION_ERROR: text += ": se corto la conexion"; break;
        case ERROR_WINHTTP_SECURE_FAILURE:
        case ERROR_WINHTTP_SECURE_INVALID_CERT:
        case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
        case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
        case ERROR_WINHTTP_SECURE_INVALID_CA:
            text += ": conexion segura rechazada (el certificado del servidor no es valido)";
            break;
        case ERROR_WINHTTP_REDIRECT_FAILED: text += ": redireccion no permitida (de https a http)"; break;
        case ERROR_WINHTTP_OPERATION_CANCELLED: text += ": cancelada"; break;
        default: text += " (error " + std::to_string(code) + ")"; break;
    }
    return text;
}

}  // namespace cramion::net::detail

#endif  // _WIN32

#endif  // CRAMION_CORE_NET_WINHTTP_COMMON_H

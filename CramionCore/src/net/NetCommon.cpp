// Comun de red (ver NetCommon.h): direcciones locales, JavaVM y JNI.

#include "NetCommon.h"

#include "CramionCore/net/Http.h"  // setJavaVM

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>

namespace cramion::net {

namespace {

std::atomic<void*> g_java_vm{nullptr};

std::string lowerText(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// "a.b.c.d" estricto: 4 partes decimales de 0-255, sin ceros a la izquierda
// (algunos resolutores leen "0177" en octal).
bool parseIpv4(const std::string& text, std::array<std::uint8_t, 4>& out) {
    std::size_t part = 0;
    std::size_t i = 0;
    while (part < 4) {
        if (i >= text.size() || std::isdigit(static_cast<unsigned char>(text[i])) == 0) return false;
        const std::size_t start = i;
        int value = 0;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])) != 0) {
            value = value * 10 + (text[i] - '0');
            if (i - start >= 3 || value > 255) return false;
            ++i;
        }
        if (i - start > 1 && text[start] == '0') return false;
        out[part++] = static_cast<std::uint8_t>(value);
        if (part < 4) {
            if (i >= text.size() || text[i] != '.') return false;
            ++i;
        }
    }
    return i == text.size();
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// IPv6 en texto (RFC 4291: "::", grupos de 1-4 hex, IPv4 al final). Sin zona.
bool parseIpv6(const std::string& text, std::array<std::uint8_t, 16>& out) {
    out.fill(0);
    if (text.empty()) return false;
    std::array<std::uint16_t, 8> groups{};
    int count = 0;
    int gap = -1;  // donde va "::"
    std::size_t i = 0;
    if (text.compare(0, 2, "::") == 0) {
        gap = 0;
        i = 2;
        if (i == text.size()) {
            out.fill(0);
            return true;
        }
    } else if (text[0] == ':') {
        return false;
    }
    while (i < text.size()) {
        // IPv4 al final ("::ffff:1.2.3.4").
        const std::size_t next_colon = text.find(':', i);
        const std::string piece = text.substr(i, next_colon == std::string::npos ? std::string::npos : next_colon - i);
        if (next_colon == std::string::npos && piece.find('.') != std::string::npos) {
            std::array<std::uint8_t, 4> v4{};
            if (count > 6 || !parseIpv4(piece, v4)) return false;
            groups[static_cast<std::size_t>(count++)] = static_cast<std::uint16_t>((v4[0] << 8) | v4[1]);
            groups[static_cast<std::size_t>(count++)] = static_cast<std::uint16_t>((v4[2] << 8) | v4[3]);
            i = text.size();
            break;
        }
        if (piece.empty() || piece.size() > 4 || count >= 8) return false;
        unsigned value = 0;
        for (const char c : piece) {
            const int h = hexValue(c);
            if (h < 0) return false;
            value = (value << 4) | static_cast<unsigned>(h);
        }
        groups[static_cast<std::size_t>(count++)] = static_cast<std::uint16_t>(value);
        if (next_colon == std::string::npos) {
            i = text.size();
            break;
        }
        i = next_colon + 1;
        if (i < text.size() && text[i] == ':') {
            if (gap >= 0) return false;  // dos "::"
            gap = count;
            ++i;
        } else if (i == text.size()) {
            return false;  // ':' suelto al final
        }
    }
    if (gap < 0) {
        if (count != 8) return false;
    } else if (count > 7) {
        return false;
    }
    std::array<std::uint16_t, 8> full{};
    if (gap < 0) {
        full = groups;
    } else {
        const int tail = count - gap;
        for (int k = 0; k < gap; ++k) full[static_cast<std::size_t>(k)] = groups[static_cast<std::size_t>(k)];
        for (int k = 0; k < tail; ++k) {
            full[static_cast<std::size_t>(8 - tail + k)] = groups[static_cast<std::size_t>(gap + k)];
        }
    }
    for (std::size_t k = 0; k < 8; ++k) {
        out[k * 2] = static_cast<std::uint8_t>(full[k] >> 8);
        out[k * 2 + 1] = static_cast<std::uint8_t>(full[k] & 0xFF);
    }
    return true;
}

// Quita corchetes y la zona ("fe80::1%eth0").
std::string bareIp(const std::string& ip) {
    std::string s = ip;
    if (s.size() >= 2 && s.front() == '[' && s.back() == ']') s = s.substr(1, s.size() - 2);
    if (const std::size_t pct = s.find('%'); pct != std::string::npos) s = s.substr(0, pct);
    return s;
}

bool loopbackV4(const std::array<std::uint8_t, 4>& a) { return a[0] == 127; }

bool privateV4(const std::array<std::uint8_t, 4>& a) {
    return a[0] == 127 || a[0] == 10 || (a[0] == 172 && (a[1] & 0xF0) == 16) || (a[0] == 192 && a[1] == 168) ||
           (a[0] == 169 && a[1] == 254);
}

// ::ffff:a.b.c.d -> a.b.c.d
bool mappedV4(const std::array<std::uint8_t, 16>& b, std::array<std::uint8_t, 4>& v4) {
    for (std::size_t k = 0; k < 10; ++k) {
        if (b[k] != 0) return false;
    }
    if (b[10] != 0xFF || b[11] != 0xFF) return false;
    v4 = {b[12], b[13], b[14], b[15]};
    return true;
}

bool loopbackV6(const std::array<std::uint8_t, 16>& b) {
    for (std::size_t k = 0; k < 15; ++k) {
        if (b[k] != 0) return false;
    }
    return b[15] == 1;
}

}  // namespace

// Unica definicion (todas las plataformas). Android: la JavaVM de la actividad.
void setJavaVM(void* vm) { g_java_vm.store(vm); }

namespace detail {

void* javaVm() { return g_java_vm.load(); }

bool isLoopbackIp(const std::string& ip) {
    const std::string s = bareIp(ip);
    std::array<std::uint8_t, 4> v4{};
    if (parseIpv4(s, v4)) return loopbackV4(v4);
    std::array<std::uint8_t, 16> v6{};
    if (!parseIpv6(s, v6)) return false;
    if (mappedV4(v6, v4)) return loopbackV4(v4);
    return loopbackV6(v6);
}

bool isLoopbackHost(const std::string& host) {
    const std::string h = lowerText(host);
    if (h == "localhost") return true;
    return isLoopbackIp(h);
}

bool isPrivateIp(const std::string& ip) {
    const std::string s = bareIp(ip);
    std::array<std::uint8_t, 4> v4{};
    if (parseIpv4(s, v4)) return privateV4(v4);
    std::array<std::uint8_t, 16> v6{};
    if (!parseIpv6(s, v6)) return false;
    if (mappedV4(v6, v4)) return privateV4(v4);
    if (loopbackV6(v6)) return true;
    if ((v6[0] & 0xFE) == 0xFC) return true;                    // fc00::/7
    if (v6[0] == 0xFE && (v6[1] & 0xC0) == 0x80) return true;   // fe80::/10
    return false;
}

#if defined(__ANDROID__)
JniThread::JniThread() {
    auto* vm = static_cast<JavaVM*>(javaVm());
    if (vm == nullptr) return;
    JNIEnv* env = nullptr;
    const jint got = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (got == JNI_OK) {
        env_ = env;
    } else if (got == JNI_EDETACHED) {
        if (vm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
            env_ = env;
            attached_ = true;
        }
    }
}

JniThread::~JniThread() {
    if (!attached_) return;
    auto* vm = static_cast<JavaVM*>(javaVm());
    if (vm != nullptr) vm->DetachCurrentThread();
}

std::string javaExceptionText(JNIEnv* env) {
    if (env == nullptr || env->ExceptionCheck() != JNI_TRUE) return {};
    jthrowable ex = env->ExceptionOccurred();
    env->ExceptionClear();
    if (ex == nullptr) return {};
    std::string text;
    jclass throwable = env->FindClass("java/lang/Throwable");
    if (throwable != nullptr) {
        jmethodID to_string = env->GetMethodID(throwable, "toString", "()Ljava/lang/String;");
        if (to_string != nullptr) {
            auto message = static_cast<jstring>(env->CallObjectMethod(ex, to_string));
            env->ExceptionClear();
            if (message != nullptr) {
                const char* chars = env->GetStringUTFChars(message, nullptr);
                text = chars != nullptr ? chars : "";
                if (chars != nullptr) env->ReleaseStringUTFChars(message, chars);
                env->DeleteLocalRef(message);
            }
        }
        env->DeleteLocalRef(throwable);
    }
    env->ExceptionClear();
    env->DeleteLocalRef(ex);
    if (text.empty()) text = "java.lang.Throwable";
    return text;
}
#endif

}  // namespace detail
}  // namespace cramion::net

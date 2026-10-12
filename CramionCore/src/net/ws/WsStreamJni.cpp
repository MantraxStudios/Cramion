// wss:// del motor propio en Android: javax.net.ssl.SSLSocket por JNI (la pila
// TLS y los certificados del sistema, con la configuracion de red de la app).
// Sin codigo Java propio: solo clases del sistema (el APK sigue con hasCode=false).
// ws:// en Android va por WsStreamSocket.cpp (POSIX).

#include "WsStream.h"

#if defined(__ANDROID__)

#include "../NetCommon.h"
#include "WsSocket.h"

#include <jni.h>

#include <algorithm>
#include <atomic>
#include <mutex>

namespace cramion::net::ws {

namespace {

constexpr jint kChunk = 64 * 1024;

struct Jni {
    bool ok = false;
    std::string error;
    jclass socket = nullptr;
    jmethodID socket_ctor = nullptr, socket_connect = nullptr, socket_set_so_timeout = nullptr,
              socket_set_tcp_no_delay = nullptr, socket_set_keep_alive = nullptr, socket_close = nullptr;
    jclass inet_addr = nullptr;
    jmethodID inet_addr_ctor = nullptr;
    jclass factory = nullptr;
    jmethodID factory_get_default = nullptr, factory_create_socket = nullptr;
    jclass ssl_socket = nullptr;
    jmethodID ssl_get_params = nullptr, ssl_set_params = nullptr, ssl_start_handshake = nullptr,
              ssl_get_input = nullptr, ssl_get_output = nullptr, ssl_close = nullptr;
    jclass ssl_params = nullptr;
    jmethodID params_set_endpoint = nullptr;
    jclass input = nullptr;
    jmethodID input_read = nullptr;
    jclass output = nullptr;
    jmethodID output_write = nullptr, output_flush = nullptr;
};

Jni g_jni;
std::once_flag g_jni_once;

jclass globalClass(JNIEnv* env, const char* name) {
    jclass local = env->FindClass(name);
    if (local == nullptr || env->ExceptionCheck()) {
        env->ExceptionClear();
        return nullptr;
    }
    auto* global = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    return global;
}

jmethodID method(JNIEnv* env, jclass cls, const char* name, const char* sig, bool is_static = false) {
    if (cls == nullptr) return nullptr;
    jmethodID id = is_static ? env->GetStaticMethodID(cls, name, sig) : env->GetMethodID(cls, name, sig);
    if (id == nullptr || env->ExceptionCheck()) {
        env->ExceptionClear();
        return nullptr;
    }
    return id;
}

const Jni& jni(JNIEnv* env) {
    std::call_once(g_jni_once, [env] {
        Jni& j = g_jni;
        if (env == nullptr) {
            j.error = "Android: falta la JavaVM (setJavaVM)";
            return;
        }
        j.socket = globalClass(env, "java/net/Socket");
        j.socket_ctor = method(env, j.socket, "<init>", "()V");
        j.socket_connect = method(env, j.socket, "connect", "(Ljava/net/SocketAddress;I)V");
        j.socket_set_so_timeout = method(env, j.socket, "setSoTimeout", "(I)V");
        j.socket_set_tcp_no_delay = method(env, j.socket, "setTcpNoDelay", "(Z)V");
        j.socket_set_keep_alive = method(env, j.socket, "setKeepAlive", "(Z)V");
        j.socket_close = method(env, j.socket, "close", "()V");
        j.inet_addr = globalClass(env, "java/net/InetSocketAddress");
        j.inet_addr_ctor = method(env, j.inet_addr, "<init>", "(Ljava/lang/String;I)V");
        j.factory = globalClass(env, "javax/net/ssl/SSLSocketFactory");
        j.factory_get_default = method(env, j.factory, "getDefault", "()Ljavax/net/SocketFactory;", true);
        j.factory_create_socket =
            method(env, j.factory, "createSocket", "(Ljava/net/Socket;Ljava/lang/String;IZ)Ljava/net/Socket;");
        j.ssl_socket = globalClass(env, "javax/net/ssl/SSLSocket");
        j.ssl_get_params = method(env, j.ssl_socket, "getSSLParameters", "()Ljavax/net/ssl/SSLParameters;");
        j.ssl_set_params = method(env, j.ssl_socket, "setSSLParameters", "(Ljavax/net/ssl/SSLParameters;)V");
        j.ssl_start_handshake = method(env, j.ssl_socket, "startHandshake", "()V");
        j.ssl_get_input = method(env, j.ssl_socket, "getInputStream", "()Ljava/io/InputStream;");
        j.ssl_get_output = method(env, j.ssl_socket, "getOutputStream", "()Ljava/io/OutputStream;");
        j.ssl_close = method(env, j.ssl_socket, "close", "()V");
        j.ssl_params = globalClass(env, "javax/net/ssl/SSLParameters");
        j.params_set_endpoint =
            method(env, j.ssl_params, "setEndpointIdentificationAlgorithm", "(Ljava/lang/String;)V");
        j.input = globalClass(env, "java/io/InputStream");
        j.input_read = method(env, j.input, "read", "([BII)I");
        j.output = globalClass(env, "java/io/OutputStream");
        j.output_write = method(env, j.output, "write", "([BII)V");
        j.output_flush = method(env, j.output, "flush", "()V");
        j.ok = j.socket_ctor && j.socket_connect && j.socket_set_so_timeout && j.socket_set_tcp_no_delay &&
               j.socket_set_keep_alive && j.socket_close && j.inet_addr_ctor && j.factory_get_default &&
               j.factory_create_socket && j.ssl_get_params && j.ssl_set_params && j.ssl_start_handshake &&
               j.ssl_get_input && j.ssl_get_output && j.ssl_close && j.params_set_endpoint && j.input_read &&
               j.output_write && j.output_flush;
        if (!j.ok) j.error = "Android: no se encontraron las clases de red del sistema (javax.net.ssl)";
    });
    return g_jni;
}

bool contains(const std::string& text, const char* what) { return text.find(what) != std::string::npos; }

// Excepcion de Java -> texto del motor (y si es fatal).
void mapException(const std::string& text, std::string& error, bool& fatal) {
    fatal = false;
    if (contains(text, "SecurityException") || contains(text, "EACCES") || contains(text, "Permission denied")) {
        error = detail::kErrPermission;
        fatal = true;
    } else if (contains(text, "UnknownHost")) {
        error = detail::kErrNoHost;
    } else if (contains(text, "SocketTimeout")) {
        error = detail::kErrTimeout;
    } else if (contains(text, "SSLHandshake") || contains(text, "CertPath") || contains(text, "SSLPeerUnverified") ||
               contains(text, "Certificate")) {
        error = detail::kErrCert;
        fatal = true;
    } else if (contains(text, "ConnectException") || contains(text, "ECONNREFUSED")) {
        error = detail::kErrRefused;
    } else {
        error = text.empty() ? std::string(detail::kErrCut) : std::string(detail::kErrCut) + " (" + text + ")";
    }
}

// PushLocalFrame / PopLocalFrame en un bloque.
struct LocalFrame {
    JNIEnv* env;
    bool pushed;
    LocalFrame(JNIEnv* e, jint n) : env(e), pushed(e->PushLocalFrame(n) == 0) {
        if (!pushed) env->ExceptionClear();
    }
    ~LocalFrame() {
        if (pushed) env->PopLocalFrame(nullptr);
    }
    LocalFrame(const LocalFrame&) = delete;
    LocalFrame& operator=(const LocalFrame&) = delete;
};

class JniTlsStream final : public ByteStream {
public:
    ~JniTlsStream() override {
        detail::JniThread thread;
        JNIEnv* env = thread.env();
        if (env == nullptr) return;
        std::lock_guard<std::mutex> lock(mu_);
        for (jobject* ref : {&plain_, &ssl_, &in_, &out_}) {
            if (*ref != nullptr) env->DeleteGlobalRef(*ref);
            *ref = nullptr;
        }
        for (jbyteArray* ref : {&rbuf_, &wbuf_}) {
            if (*ref != nullptr) env->DeleteGlobalRef(*ref);
            *ref = nullptr;
        }
    }

    bool connect(const std::string& host, int port, int timeout_ms, AddrPolicy policy, std::string& error,
                 bool& fatal) override {
        fatal = false;
        if (timeout_ms <= 0) timeout_ms = 10000;
        // La politica (red local) con todas las direcciones, antes de Java.
        if (policy != AddrPolicy::Any && !sock::checkPolicy(host, port, policy, error, fatal)) return false;

        detail::JniThread thread;
        JNIEnv* env = thread.env();
        if (env == nullptr) {
            // Sin JavaVM no se llama a jni(): su call_once quedaria fallido para siempre.
            error = "Android: falta la JavaVM (setJavaVM)";
            fatal = true;
            return false;
        }
        const Jni& j = jni(env);
        if (!j.ok) {
            error = j.error;
            fatal = true;
            return false;
        }
        LocalFrame frame(env, 32);

        auto failed = [&](const char* what) -> bool {
            if (!env->ExceptionCheck()) return false;
            const std::string text = detail::javaExceptionText(env);
            if (aborted_.load()) {
                error = "conexion cancelada";
                fatal = false;
            } else {
                mapException(text, error, fatal);
                if (error.empty()) error = what;
            }
            return true;
        };

        // 1. Socket normal y conectar (InetSocketAddress resuelve el nombre).
        jobject plain = env->NewObject(j.socket, j.socket_ctor);
        if (failed("Socket") || plain == nullptr) return fail(error, "no se pudo crear el socket");
        {
            std::lock_guard<std::mutex> lock(mu_);
            plain_ = env->NewGlobalRef(plain);
        }
        if (aborted_.load()) return cancelled(error, fatal);
        jstring jhost = env->NewStringUTF(host.c_str());
        jobject addr = env->NewObject(j.inet_addr, j.inet_addr_ctor, jhost, static_cast<jint>(port));
        if (failed("InetSocketAddress") || addr == nullptr) return fail(error, detail::kErrNoHost);
        env->CallVoidMethod(plain, j.socket_connect, addr, static_cast<jint>(timeout_ms));
        if (failed("connect")) return false;
        if (aborted_.load()) return cancelled(error, fatal);

        // 2. Tiempo del saludo TLS; sin Nagle; keepalive.
        env->CallVoidMethod(plain, j.socket_set_so_timeout, static_cast<jint>(timeout_ms));
        if (failed("setSoTimeout")) return false;
        env->CallVoidMethod(plain, j.socket_set_tcp_no_delay, JNI_TRUE);
        if (failed("setTcpNoDelay")) return false;
        env->CallVoidMethod(plain, j.socket_set_keep_alive, JNI_TRUE);
        if (failed("setKeepAlive")) return false;

        // 3. SSLSocket por encima (con SNI: el nombre del servidor).
        jobject factory = env->CallStaticObjectMethod(j.factory, j.factory_get_default);
        if (failed("SSLSocketFactory") || factory == nullptr) return fail(error, "no hay TLS en el sistema");
        jobject ssl = env->CallObjectMethod(factory, j.factory_create_socket, plain, jhost, static_cast<jint>(port),
                                            JNI_TRUE);
        if (failed("createSocket") || ssl == nullptr) return fail(error, "no se pudo crear el socket TLS");
        {
            std::lock_guard<std::mutex> lock(mu_);
            ssl_ = env->NewGlobalRef(ssl);
        }
        if (aborted_.load()) return cancelled(error, fatal);

        // 4. Comprobar el nombre del certificado (HTTPS) y saludo.
        jobject params = env->CallObjectMethod(ssl, j.ssl_get_params);
        if (failed("getSSLParameters") || params == nullptr) return fail(error, "TLS sin parametros");
        jstring https = env->NewStringUTF("HTTPS");
        env->CallVoidMethod(params, j.params_set_endpoint, https);
        if (failed("setEndpointIdentificationAlgorithm")) return false;
        env->CallVoidMethod(ssl, j.ssl_set_params, params);
        if (failed("setSSLParameters")) return false;
        env->CallVoidMethod(ssl, j.ssl_start_handshake);
        if (failed("startHandshake")) return false;

        // 5. Ya conectado: lecturas sin tiempo (las vigila el motor).
        env->CallVoidMethod(ssl, j.socket_set_so_timeout, static_cast<jint>(0));
        if (failed("setSoTimeout")) return false;

        // 6. Flujos y bufferes de 64 KB reutilizados.
        jobject in = env->CallObjectMethod(ssl, j.ssl_get_input);
        if (failed("getInputStream") || in == nullptr) return fail(error, detail::kErrCut);
        jobject out = env->CallObjectMethod(ssl, j.ssl_get_output);
        if (failed("getOutputStream") || out == nullptr) return fail(error, detail::kErrCut);
        jbyteArray rbuf = env->NewByteArray(kChunk);
        jbyteArray wbuf = env->NewByteArray(kChunk);
        if (failed("NewByteArray") || rbuf == nullptr || wbuf == nullptr) return fail(error, "sin memoria");
        {
            std::lock_guard<std::mutex> lock(mu_);
            in_ = env->NewGlobalRef(in);
            out_ = env->NewGlobalRef(out);
            rbuf_ = static_cast<jbyteArray>(env->NewGlobalRef(rbuf));
            wbuf_ = static_cast<jbyteArray>(env->NewGlobalRef(wbuf));
        }
        if (aborted_.load()) return cancelled(error, fatal);
        return true;
    }

    long read(char* buffer, std::size_t n, std::string& error) override {
        if (aborted_.load()) {
            error.clear();
            return -1;
        }
        detail::JniThread thread;
        JNIEnv* env = thread.env();
        const Jni& j = jni(env);
        if (env == nullptr || !j.ok || in_ == nullptr) {
            error = detail::kErrCut;
            return -1;
        }
        const jint want = static_cast<jint>(std::min<std::size_t>(n, static_cast<std::size_t>(kChunk)));
        for (;;) {
            LocalFrame frame(env, 8);
            const jint r = env->CallIntMethod(in_, j.input_read, rbuf_, static_cast<jint>(0), want);
            if (env->ExceptionCheck()) {
                const std::string text = detail::javaExceptionText(env);
                if (aborted_.load()) {
                    error.clear();
                } else {
                    bool fatal = false;
                    mapException(text, error, fatal);
                }
                return -1;
            }
            if (r < 0) return aborted_.load() ? -1 : 0;  // -1 de Java = fin
            if (r == 0) continue;
            env->GetByteArrayRegion(rbuf_, 0, r, reinterpret_cast<jbyte*>(buffer));
            return r;
        }
    }

    bool write(const char* data, std::size_t n, std::string& error) override {
        if (aborted_.load()) {
            error.clear();
            return false;
        }
        detail::JniThread thread;
        JNIEnv* env = thread.env();
        const Jni& j = jni(env);
        if (env == nullptr || !j.ok || out_ == nullptr) {
            error = detail::kErrCut;
            return false;
        }
        LocalFrame frame(env, 8);
        auto check = [&]() -> bool {
            if (!env->ExceptionCheck()) return true;
            const std::string text = detail::javaExceptionText(env);
            if (aborted_.load()) {
                error.clear();
            } else {
                bool fatal = false;
                mapException(text, error, fatal);
            }
            return false;
        };
        while (n > 0) {
            const jint len = static_cast<jint>(std::min<std::size_t>(n, static_cast<std::size_t>(kChunk)));
            env->SetByteArrayRegion(wbuf_, 0, len, reinterpret_cast<const jbyte*>(data));
            env->CallVoidMethod(out_, j.output_write, wbuf_, static_cast<jint>(0), len);
            if (!check()) return false;
            data += len;
            n -= static_cast<std::size_t>(len);
        }
        env->CallVoidMethod(out_, j.output_flush);
        return check();
    }

    void abort() override {
        if (aborted_.exchange(true)) return;
        detail::JniThread thread;
        JNIEnv* env = thread.env();
        if (env == nullptr) return;
        const Jni& j = jni(env);
        if (!j.ok) return;
        std::lock_guard<std::mutex> lock(mu_);
        // Primero el socket de abajo: despierta lecturas y escrituras al momento
        // sin que el close() del SSLSocket tenga que esperar a nadie.
        if (plain_ != nullptr) {
            env->CallVoidMethod(plain_, j.socket_close);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        if (ssl_ != nullptr) {
            env->CallVoidMethod(ssl_, j.ssl_close);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
    }

private:
    static bool fail(std::string& error, const char* fallback) {
        if (error.empty()) error = fallback;
        return false;
    }
    static bool cancelled(std::string& error, bool& fatal) {
        error = "conexion cancelada";
        fatal = false;
        return false;
    }

    std::mutex mu_;
    std::atomic<bool> aborted_{false};
    jobject plain_ = nullptr;
    jobject ssl_ = nullptr;
    jobject in_ = nullptr;
    jobject out_ = nullptr;
    jbyteArray rbuf_ = nullptr;
    jbyteArray wbuf_ = nullptr;
};

}  // namespace

std::unique_ptr<ByteStream> makeTlsStream(std::string* error) {
    if (detail::javaVm() == nullptr) {
        if (error != nullptr) *error = "Android: falta la JavaVM (setJavaVM)";
        return nullptr;
    }
    return std::make_unique<JniTlsStream>();
}

}  // namespace cramion::net::ws

#endif  // __ANDROID__

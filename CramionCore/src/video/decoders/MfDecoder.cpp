// Decodificador de video de Windows con Media Foundation (IMFSourceReader).
//
// - Lector asincrono (MF_SOURCE_READER_ASYNC_CALLBACK): next() espera en una
//   variable de condicion que interrupt() despierta; nunca queda colgado en un
//   ReadSample sincrono.
// - Decodificacion por hardware (Auto/Hardware): un ID3D11Device con
//   VIDEO_SUPPORT y un IMFDXGIDeviceManager, mas
//   MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS. Salida NV12 (el compute de la GPU
//   convierte); si no se puede, RGB32 por el procesador avanzado (Bgra8).
// - Audio en float con el formato nativo de cada pista (AudioChunk dice la
//   frecuencia y los canales reales); una pista por cada flujo elegido.
// - http(s): byte stream propio sobre net::httpRequest con peticiones Range,
//   asi se aplica la politica de Http.h (solo https; http solo a este PC; nada
//   de https -> http) y interrupt() corta la red. El HTTP interno de MF no se usa.
// - mfplat.dll y mfreadwrite.dll se cargan con LoadLibrary (Windows N/KN sin
//   Media Feature Pack: error claro en vez de no arrancar).

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// D3D antes que MfCommon.h (que define las GUID de MF): las de D3D quedan
// como declaraciones normales.
#include <windows.h>
#include <d3d10_1.h>
#include <d3d11.h>

#include "MfCommon.h"

#include "DecoderInternal.h"

#include "CramionCore/net/Http.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <iostream>
#include <iterator>
#include <list>
#include <thread>
#include <utility>
#include <vector>

namespace cramion::video::mf {

// --- Carga de Media Foundation, COM y MFStartup ---------------------------------

namespace {

constexpr const char* kMissingMf =
    "Falta Media Foundation (mfplat.dll). En Windows N/KN instala el Media Feature Pack: "
    "Configuracion > Aplicaciones > Caracteristicas opcionales > Agregar una caracteristica > "
    "Media Feature Pack, y reinicia.";

struct Loaded {
    Api api;
    bool ok = false;
    std::string why;
};

// Nunca se destruye (new sin delete): un hilo de reproductor soltado con
// detach puede seguir usando la Api mientras el proceso sale.
const Loaded& loaded() {
    static const Loaded& instance = *new Loaded([] {
        Loaded l;
        HMODULE plat = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        HMODULE rw = LoadLibraryExW(L"mfreadwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (plat == nullptr || rw == nullptr) {
            l.why = kMissingMf;
            return l;
        }
        const auto get = [](HMODULE m, const char* name, auto& fn) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(reinterpret_cast<void*>(GetProcAddress(m, name)));
            return fn != nullptr;
        };
        bool all = true;
        all &= get(plat, "MFStartup", l.api.Startup);
        all &= get(plat, "MFShutdown", l.api.Shutdown);
        all &= get(plat, "MFCreateAttributes", l.api.CreateAttributes);
        all &= get(plat, "MFCreateMediaType", l.api.CreateMediaType);
        all &= get(plat, "MFCreateDXGIDeviceManager", l.api.CreateDXGIDeviceManager);
        all &= get(plat, "MFCreateAsyncResult", l.api.CreateAsyncResult);
        all &= get(plat, "MFInvokeCallback", l.api.InvokeCallback);
        all &= get(rw, "MFCreateSourceReaderFromURL", l.api.CreateSourceReaderFromURL);
        all &= get(rw, "MFCreateSourceReaderFromByteStream", l.api.CreateSourceReaderFromByteStream);
        all &= get(rw, "MFCreateSinkWriterFromURL", l.api.CreateSinkWriterFromURL);
        all &= get(plat, "MFCreateSample", l.api.CreateSample);
        all &= get(plat, "MFCreateMemoryBuffer", l.api.CreateMemoryBuffer);
        if (!all) {
            l.why = std::string("Media Foundation esta incompleto en este Windows. ") + kMissingMf;
            return l;
        }
        // Las DLL no se liberan nunca (un hilo soltado puede seguir dentro).
        l.ok = true;
        return l;
    }());
    return instance;
}

// COM en modo MTA, una vez por hilo. Si el hilo ya es STA (el principal del
// editor), Media Foundation funciona igual: no se toca.
struct ThreadCom {
    HRESULT hr = E_FAIL;
    ThreadCom() { hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    ~ThreadCom() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

void ensureThreadCom() {
    thread_local ThreadCom com;
    (void)com;
}

// Igual que loaded(): sin destructor estatico (un hilo soltado puede llegar a
// Session::release despues de que main termine).
std::mutex& startupMutex() {
    static std::mutex* m = new std::mutex();
    return *m;
}
int g_startup_count = 0;

}  // namespace

const Api* api(std::string* why) {
    const Loaded& l = loaded();
    if (!l.ok) {
        if (why != nullptr) *why = l.why;
        return nullptr;
    }
    return &l.api;
}

bool Session::acquire(std::string* why) {
    if (active_) return true;
    ensureThreadCom();
    const Api* a = api(why);
    if (a == nullptr) return false;
    std::lock_guard<std::mutex> lock(startupMutex());
    if (g_startup_count == 0) {
        const HRESULT hr = a->Startup(MF_VERSION, MFSTARTUP_FULL);
        if (FAILED(hr)) {
            if (why != nullptr) {
                char code[16];
                std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(hr));
                *why = std::string("Media Foundation no arranca (MFStartup ") + code + "). " + kMissingMf;
            }
            return false;
        }
    }
    ++g_startup_count;
    active_ = true;
    return true;
}

void Session::release() {
    if (!active_) return;
    active_ = false;
    const Api* a = api();
    std::lock_guard<std::mutex> lock(startupMutex());
    // MFShutdown solo cuando nadie (ningun hilo de reproductor vivo) lo usa.
    if (--g_startup_count == 0 && a != nullptr) a->Shutdown();
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string narrow(const std::wstring& s) {
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::string hresultText(HRESULT hr) {
    char code[16];
    std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(hr));
    switch (hr) {
        case E_ABORT: return "Interrumpido";
        case MF_E_UNSUPPORTED_BYTESTREAM_TYPE:
        case MF_E_UNSUPPORTED_FORMAT:
            return std::string("Formato de video no soportado por Media Foundation (") + code +
                   "). Recomendado: MP4 con H.264 + AAC";
        case MF_E_UNSUPPORTED_SCHEME: return std::string("Tipo de direccion no soportado (") + code + ")";
        case MF_E_NET_READ: return std::string("Se corto la conexion al leer el video (") + code + ")";
        case MF_E_DRM_UNSUPPORTED: return "El video tiene DRM y no se puede reproducir";
        case HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND):
        case HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND): return "No existe el archivo de video";
        case HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED): return "Sin permiso para leer el archivo de video";
        case HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION): return "El archivo de video esta abierto por otro programa";
        default: break;
    }
    std::string text = std::string("Media Foundation ") + code;
    wchar_t* message = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, static_cast<DWORD>(hr), 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    if (n > 0 && message != nullptr) {
        std::string m = narrow(std::wstring(message, n));
        while (!m.empty() && (m.back() == '\n' || m.back() == '\r' || m.back() == ' ' || m.back() == '.')) m.pop_back();
        if (!m.empty()) text += ": " + m;
    }
    if (message != nullptr) LocalFree(message);
    return text;
}

// --- ByteStreamBase --------------------------------------------------------------

namespace {

// Objeto del IMFAsyncResult de BeginRead: los bytes leidos.
class ReadCount final : public IUnknown {
public:
    explicit ReadCount(ULONG bytes) : bytes(bytes) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr) return E_POINTER;
        if (riid == __uuidof(IUnknown)) {
            *ppv = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG r = --refs_;
        if (r == 0) delete this;
        return r;
    }
    const ULONG bytes;

private:
    std::atomic<ULONG> refs_{1};
};

}  // namespace

ByteStreamBase::ByteStreamBase() = default;
ByteStreamBase::~ByteStreamBase() = default;

bool ByteStreamBase::initAttributes() {
    const Api* a = api();
    return a != nullptr && SUCCEEDED(a->CreateAttributes(&attrs_, 4));
}

STDMETHODIMP ByteStreamBase::QueryInterface(REFIID riid, void** ppv) {
    if (ppv == nullptr) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFByteStream)) {
        *ppv = static_cast<IMFByteStream*>(this);
    } else if (riid == __uuidof(IMFAttributes) && attrs_) {
        *ppv = static_cast<IMFAttributes*>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) ByteStreamBase::AddRef() { return ++refs_; }

STDMETHODIMP_(ULONG) ByteStreamBase::Release() {
    const ULONG r = --refs_;
    if (r == 0) delete this;
    return r;
}

STDMETHODIMP ByteStreamBase::GetCapabilities(DWORD* caps) {
    if (caps == nullptr) return E_POINTER;
    *caps = capabilities();
    return S_OK;
}

STDMETHODIMP ByteStreamBase::GetLength(QWORD* len) {
    if (len == nullptr) return E_POINTER;
    *len = length();
    return S_OK;
}

STDMETHODIMP ByteStreamBase::GetCurrentPosition(QWORD* position) {
    if (position == nullptr) return E_POINTER;
    std::lock_guard<std::mutex> lock(position_mutex_);
    *position = position_;
    return S_OK;
}

STDMETHODIMP ByteStreamBase::SetCurrentPosition(QWORD position) {
    // canSeekTo lee el estado de readAt (p. ej. los trozos de HLS): bajo el mismo mutex.
    std::lock_guard<std::mutex> lock(position_mutex_);
    if (!canSeekTo(position)) return E_INVALIDARG;
    position_ = position;
    end_ = false;
    return S_OK;
}

STDMETHODIMP ByteStreamBase::IsEndOfStream(BOOL* end) {
    if (end == nullptr) return E_POINTER;
    std::lock_guard<std::mutex> lock(position_mutex_);
    const QWORD len = length();
    *end = (end_ || (len != static_cast<QWORD>(-1) && position_ >= len)) ? TRUE : FALSE;
    return S_OK;
}

STDMETHODIMP ByteStreamBase::Read(BYTE* pb, ULONG cb, ULONG* read) {
    if (pb == nullptr || read == nullptr) return E_POINTER;
    *read = 0;
    if (cancel_.load()) return E_ABORT;
    // Una lectura a la vez: la posicion avanza de forma coherente.
    std::lock_guard<std::mutex> lock(position_mutex_);
    ULONG n = 0;
    const HRESULT hr = readAt(position_, pb, cb, &n);
    if (FAILED(hr)) return hr;
    position_ += n;
    if (n == 0 && cb > 0) end_ = true;
    *read = n;
    return S_OK;
}

STDMETHODIMP ByteStreamBase::BeginRead(BYTE* pb, ULONG cb, IMFAsyncCallback* callback, IUnknown* state) {
    if (callback == nullptr) return E_POINTER;
    const Api* a = api();
    if (a == nullptr) return E_FAIL;
    ULONG n = 0;
    const HRESULT status = Read(pb, cb, &n);
    ComPtr<IUnknown> object;
    object.Attach(new ReadCount(n));
    ComPtr<IMFAsyncResult> result;
    const HRESULT hr = a->CreateAsyncResult(object.Get(), callback, state, &result);
    if (FAILED(hr)) return hr;
    result->SetStatus(status);
    return a->InvokeCallback(result.Get());
}

STDMETHODIMP ByteStreamBase::EndRead(IMFAsyncResult* result, ULONG* read) {
    if (result == nullptr || read == nullptr) return E_POINTER;
    *read = 0;
    ComPtr<IUnknown> object;
    if (SUCCEEDED(result->GetObject(&object)) && object) *read = static_cast<ReadCount*>(object.Get())->bytes;
    return result->GetStatus();
}

STDMETHODIMP ByteStreamBase::Seek(MFBYTESTREAM_SEEK_ORIGIN origin, LONGLONG offset, DWORD, QWORD* position) {
    QWORD target = 0;
    if (origin == msoBegin) {
        if (offset < 0) return E_INVALIDARG;
        target = static_cast<QWORD>(offset);
    } else {
        QWORD current = 0;
        GetCurrentPosition(&current);
        if (offset < 0 && static_cast<QWORD>(-offset) > current) return E_INVALIDARG;
        target = current + offset;
    }
    const HRESULT hr = SetCurrentPosition(target);
    if (SUCCEEDED(hr) && position != nullptr) *position = target;
    return hr;
}

STDMETHODIMP ByteStreamBase::Close() {
    abort();
    return S_OK;
}

// --- Byte stream http(s) con peticiones Range ------------------------------------

namespace {

constexpr QWORD kUnknownLength = static_cast<QWORD>(-1);

class HttpByteStream final : public ByteStreamBase {
public:
    explicit HttpByteStream(std::string url) : url_(std::move(url)) {}

    // Primera peticion: longitud, si admite rangos y el tipo de contenido.
    bool start(std::string& error) {
        if (!initAttributes()) {
            error = "Media Foundation: no se pudo crear el almacen de atributos";
            return false;
        }
        net::HttpResponse r;
        if (!fetch(0, kChunk - 1, r, error)) return false;
        if (r.status == 206) {
            length_ = parseTotal(r);
            insert(0, std::make_shared<const std::string>(std::move(r.body)));
        } else {
            // El servidor no admite Range: el archivo entero (ya cabia en un trozo).
            length_ = r.body.size();
            whole_ = std::make_shared<const std::string>(std::move(r.body));
        }
        std::string type = header(r, "content-type");
        if (const auto semi = type.find(';'); semi != std::string::npos) type.resize(semi);
        if (!type.empty() && type != "application/octet-stream" && type != "binary/octet-stream") {
            SetString(MF_BYTESTREAM_CONTENT_TYPE, widen(type).c_str());
        }
        SetString(MF_BYTESTREAM_ORIGIN_NAME, widen(url_).c_str());
        return true;
    }

protected:
    HRESULT readAt(QWORD position, BYTE* pb, ULONG cb, ULONG* read) override {
        *read = 0;
        if (length_ != kUnknownLength && position >= length_) return S_OK;
        ULONG done = 0;
        while (done < cb) {
            if (cancel_.load()) return E_ABORT;
            const QWORD p = position + done;
            std::shared_ptr<const std::string> data;
            QWORD base = 0;
            if (whole_) {
                data = whole_;
            } else {
                const QWORD index = p / kChunk;
                base = index * kChunk;
                std::string error;
                data = chunk(index, error);
                if (!data) {
                    if (cancel_.load()) return E_ABORT;
                    if (done > 0) break;
                    std::cerr << "[Video] " << error << "\n";
                    return MF_E_NET_READ;
                }
            }
            const QWORD offset = p - base;
            if (offset >= data->size()) break;  // fin
            const ULONG n = static_cast<ULONG>(std::min<QWORD>(cb - done, data->size() - offset));
            std::memcpy(pb + done, data->data() + offset, n);
            done += n;
            if (!whole_ && data->size() < kChunk && offset + n >= data->size()) break;  // ultimo trozo
        }
        *read = done;
        return S_OK;
    }
    DWORD capabilities() const override {
        DWORD caps = MFBYTESTREAM_IS_READABLE | MFBYTESTREAM_IS_REMOTE;
        if (length_ != kUnknownLength) caps |= MFBYTESTREAM_IS_SEEKABLE;
        return caps;
    }
    QWORD length() const override { return length_; }
    bool canSeekTo(QWORD) const override { return length_ != kUnknownLength; }

private:
    static constexpr QWORD kChunk = 2ull << 20;  // 2 MB por peticion
    static constexpr std::size_t kMaxChunks = 16;   // 32 MB de cache como mucho

    static std::string header(const net::HttpResponse& r, const char* name) {
        for (const auto& [k, v] : r.headers) {
            if (k == name) return v;
        }
        return {};
    }

    static QWORD parseTotal(const net::HttpResponse& r) {
        // "bytes 0-2097151/123456789" ("*" = desconocida).
        const std::string range = header(r, "content-range");
        const auto slash = range.rfind('/');
        if (slash == std::string::npos) return kUnknownLength;
        const std::string total = range.substr(slash + 1);
        if (total.empty() || total[0] == '*') return kUnknownLength;
        try {
            return static_cast<QWORD>(std::stoull(total));
        } catch (...) {
            return kUnknownLength;
        }
    }

    bool fetch(QWORD first, QWORD last, net::HttpResponse& out, std::string& error) {
        net::HttpRequest request;
        request.url = url_;
        request.headers.emplace_back("Range", "bytes=" + std::to_string(first) + "-" + std::to_string(last));
        request.max_response_bytes = static_cast<std::size_t>(kChunk + (64u << 10));
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (cancel_.load()) {
                error = "Interrumpido";
                return false;
            }
            out = net::httpRequest(request, &cancel_);
            // httpRequest deja status = 200 aunque corte el cuerpo por el maximo
            // (servidor que ignora Range): solo vale si no hubo error.
            const bool too_big = !out.error.empty() && out.error.find("maximo") != std::string::npos;
            if ((out.status == 206 || out.status == 200) && out.error.empty()) return true;
            if (out.status == 416) {  // pasado el final
                out.body.clear();
                return true;
            }
            if (too_big && first != 0) break;  // Range ignorado a mitad: no se arregla reintentando
            if (out.status >= 400 && out.status < 500) break;  // no se arregla reintentando
            if (too_big && first == 0) {
                // Sin Range y mas grande que un trozo: se baja entero (con limite).
                request.headers.clear();
                request.max_response_bytes = 256u << 20;
                out = net::httpRequest(request, &cancel_);
                if (out.ok && out.error.empty()) return true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(150 * (attempt + 1)));
        }
        if (out.status != 0) error = "HTTP " + std::to_string(out.status) + " al leer " + url_;
        else error = out.error.empty() ? "No se pudo leer " + url_ : out.error;
        return false;
    }

    std::shared_ptr<const std::string> chunk(QWORD index, std::string& error) {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        for (auto it = cache_.begin(); it != cache_.end(); ++it) {
            if (it->first == index) {
                cache_.splice(cache_.begin(), cache_, it);
                return cache_.front().second;
            }
        }
        const QWORD first = index * kChunk;
        QWORD last = first + kChunk - 1;
        if (length_ != kUnknownLength) {
            if (first >= length_) return std::make_shared<const std::string>();
            last = std::min(last, length_ - 1);
        }
        net::HttpResponse r;
        if (!fetch(first, last, r, error)) return nullptr;
        if (r.status == 200) {
            // Ignoro el Range a mitad: el cuerpo es el archivo entero.
            whole_ = std::make_shared<const std::string>(std::move(r.body));
            const std::string& all = *whole_;
            if (first >= all.size()) return std::make_shared<const std::string>();
            return std::make_shared<const std::string>(all.substr(first, static_cast<std::size_t>(last - first + 1)));
        }
        auto data = std::make_shared<const std::string>(std::move(r.body));
        cache_.emplace_front(index, data);
        if (cache_.size() > kMaxChunks) cache_.pop_back();
        return data;
    }

    void insert(QWORD index, std::shared_ptr<const std::string> data) {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        cache_.emplace_front(index, std::move(data));
    }

    std::string url_;
    QWORD length_ = kUnknownLength;
    std::shared_ptr<const std::string> whole_;
    std::mutex cache_mutex_;
    std::list<std::pair<QWORD, std::shared_ptr<const std::string>>> cache_;
};

// --- Callback del lector asincrono ------------------------------------------------

struct ReadItem {
    HRESULT hr = S_OK;
    DWORD stream = 0;
    DWORD flags = 0;
    LONGLONG timestamp = 0;
    ComPtr<IMFSample> sample;
};

class ReaderCallback final : public IMFSourceReaderCallback {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (ppv == nullptr) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFSourceReaderCallback)) {
            *ppv = static_cast<IMFSourceReaderCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG r = --refs_;
        if (r == 0) delete this;
        return r;
    }

    STDMETHODIMP OnReadSample(HRESULT hr, DWORD stream, DWORD flags, LONGLONG timestamp, IMFSample* sample) override {
        {
            std::lock_guard<std::mutex> lock(mutex);
            ReadItem item;
            item.hr = hr;
            item.stream = stream;
            item.flags = flags;
            item.timestamp = timestamp;
            item.sample = sample;
            results.push_back(std::move(item));
        }
        cv.notify_all();
        return S_OK;
    }
    STDMETHODIMP OnFlush(DWORD) override {
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++flushes;
        }
        cv.notify_all();
        return S_OK;
    }
    STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }

    void interrupt() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            interrupted = true;
        }
        cv.notify_all();
    }

    std::mutex mutex;
    std::condition_variable cv;
    std::deque<ReadItem> results;
    std::uint64_t flushes = 0;
    bool interrupted = false;

private:
    std::atomic<ULONG> refs_{1};
};

// --- Pool de buferes de los frames ------------------------------------------------

class FramePool : public std::enable_shared_from_this<FramePool> {
public:
    std::shared_ptr<std::vector<std::uint8_t>> get(std::size_t size) {
        std::unique_ptr<std::vector<std::uint8_t>> buffer;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto it = free_.begin(); it != free_.end(); ++it) {
                if ((*it)->capacity() >= size) {
                    buffer = std::move(*it);
                    free_.erase(it);
                    break;
                }
            }
        }
        if (!buffer) buffer = std::make_unique<std::vector<std::uint8_t>>();
        buffer->resize(size);
        std::weak_ptr<FramePool> weak = weak_from_this();
        return std::shared_ptr<std::vector<std::uint8_t>>(buffer.release(), [weak](std::vector<std::uint8_t>* b) {
            if (auto pool = weak.lock()) {
                std::lock_guard<std::mutex> lock(pool->mutex_);
                if (pool->free_.size() < kKeep) {
                    pool->free_.emplace_back(b);
                    return;
                }
            }
            delete b;
        });
    }

private:
    static constexpr std::size_t kKeep = 6;
    std::mutex mutex_;
    std::vector<std::unique_ptr<std::vector<std::uint8_t>>> free_;
};

std::string fourcc(DWORD v) {
    std::string s;
    for (int i = 0; i < 4; ++i) {
        const char c = static_cast<char>((v >> (8 * i)) & 0xFF);
        s += (c >= 32 && c < 127) ? c : '?';
    }
    return s;
}

constexpr DWORD fcc(const char (&s)[5]) {
    return static_cast<DWORD>(static_cast<unsigned char>(s[0])) | (static_cast<DWORD>(static_cast<unsigned char>(s[1])) << 8) |
           (static_cast<DWORD>(static_cast<unsigned char>(s[2])) << 16) |
           (static_cast<DWORD>(static_cast<unsigned char>(s[3])) << 24);
}

std::string codecName(const GUID& subtype) {
    if (subtype == MFVideoFormat_MPEG2) return "MPEG-2";
    switch (subtype.Data1) {
        case fcc("H264"): case fcc("h264"): case fcc("AVC1"): case fcc("avc1"): return "H.264";
        case fcc("HEVC"): case fcc("H265"): case fcc("hvc1"): case fcc("hev1"): return "HEVC";
        case fcc("AV01"): return "AV1";
        case fcc("VP90"): return "VP9";
        case fcc("VP80"): return "VP8";
        case fcc("MP4V"): case fcc("M4S2"): case fcc("MP4S"): return "MPEG-4";
        case fcc("MP43"): return "MPEG-4 v3";
        case fcc("WMV1"): case fcc("WMV2"): case fcc("WMV3"): return "WMV";
        case fcc("WVC1"): return "VC-1";
        case fcc("MPG1"): return "MPEG-1";
        case fcc("MJPG"): return "MJPEG";
        default: return fourcc(subtype.Data1);
    }
}

// Que falta para decodificar ese codec (critico #23).
std::string codecHint(const std::string& codec) {
    if (codec == "HEVC") {
        return "No hay decodificador HEVC en este equipo: instala \"Extensiones de video HEVC\" desde Microsoft Store "
               "o convierte el video a H.264";
    }
    if (codec == "AV1") return "No hay decodificador AV1: instala \"Extension de video AV1\" desde Microsoft Store";
    if (codec == "VP9" || codec == "VP8") {
        return "No hay decodificador " + codec + ": instala \"Extensiones de multimedia web\" desde Microsoft Store";
    }
    return "No hay decodificador para el codec " + codec + " en este equipo. Recomendado: MP4 con H.264 + AAC";
}

std::string propString(IMFSourceReader* reader, DWORD stream, REFGUID key) {
    PROPVARIANT v;
    PropVariantInit(&v);
    std::string out;
    if (SUCCEEDED(reader->GetPresentationAttribute(stream, key, &v)) && v.vt == VT_LPWSTR && v.pwszVal != nullptr) {
        out = narrow(v.pwszVal);
    }
    PropVariantClear(&v);
    return out;
}

}  // namespace

// --- MfDecoder --------------------------------------------------------------------

struct MfDecoder::State {
    struct Slot {
        DWORD stream = 0;
        int track = -1;  // -1 = video; si no, indice en StreamInfo::audio
        bool pending = false, ended = false, end_after = false;
        bool has_ready = false;
        ReadItem ready;
        std::chrono::steady_clock::time_point ready_since;
        std::uint32_t rate = 0, channels = 0;
        bool pcm16 = false;
    };

    Session session;
    DecodeOptions options;
    ComPtr<IMFSourceReader> reader;
    ComPtr<ReaderCallback> callback;
    ComPtr<ID3D11Device> device;
    ComPtr<IMFDXGIDeviceManager> manager;
    UINT reset_token = 0;

    std::mutex abort_mutex;
    ComPtr<ByteStreamBase> abortable;
    std::atomic<bool> interrupted{false};

    static constexpr DWORD kNone = 0xFFFFFFFFu;
    DWORD video_stream = kNone;
    std::vector<DWORD> audio_streams;  // indice de pista -> flujo del lector
    std::vector<Slot> slots;

    // Salida de video actual.
    PixelLayout layout = PixelLayout::Nv12;
    std::uint32_t buf_w = 0, buf_h = 0;   // tamano del bufer (p. ej. 1088)
    std::uint32_t vis_x = 0, vis_y = 0, vis_w = 0, vis_h = 0;  // apertura visible
    LONG default_stride = 0;
    ColorInfo color;
    std::string codec;

    mutable std::mutex info_mutex;
    StreamInfo info;

    std::shared_ptr<FramePool> pool = std::make_shared<FramePool>();
    bool warned_frame = false;

    ~State() { shutdown(); }

    void shutdown() {
        if (reader) {
            bool pending = false;
            for (const Slot& s : slots) pending |= s.pending;
            if (pending) flushAndWait(std::chrono::milliseconds(500));
            reader.Reset();
        }
        callback.Reset();
        manager.Reset();
        device.Reset();
        {
            std::lock_guard<std::mutex> lock(abort_mutex);
            abortable.Reset();
        }
        session.release();
    }

    // interruptible: interrupt() corta la espera (busqueda: parar no espera
    // hasta 3 s). Al cerrar (shutdown) no: se espera el OnFlush antes de soltar
    // el lector aunque ya se haya interrumpido.
    bool flushAndWait(std::chrono::milliseconds timeout, bool interruptible = false) {
        if (!reader || !callback) return false;
        std::uint64_t before = 0;
        {
            std::lock_guard<std::mutex> lock(callback->mutex);
            before = callback->flushes;
        }
        if (FAILED(reader->Flush(MF_SOURCE_READER_ALL_STREAMS))) return false;
        std::unique_lock<std::mutex> lock(callback->mutex);
        const bool done = callback->cv.wait_for(lock, timeout, [&] {
            return callback->flushes > before || (interruptible && callback->interrupted);
        });
        callback->results.clear();
        for (Slot& s : slots) {
            // Cortada por interrupt() sin OnFlush: sigue pendiente, para que
            // shutdown() vuelva a vaciar antes de soltar el lector.
            if (done || !interruptible) s.pending = false;
            s.has_ready = false;
            s.ready = ReadItem{};
        }
        return done;
    }

    bool createD3D() {
        static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                                   D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        const UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
                                       static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION, &device, nullptr, nullptr);
        if (hr == E_INVALIDARG) {  // sin 11_1 (runtime antiguo)
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels + 1,
                                   static_cast<UINT>(std::size(levels) - 1), D3D11_SDK_VERSION, &device, nullptr, nullptr);
        }
        if (FAILED(hr)) return false;
        ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(device.As(&mt))) mt->SetMultithreadProtected(TRUE);
        const Api* a = api();
        if (a == nullptr || FAILED(a->CreateDXGIDeviceManager(&reset_token, &manager)) ||
            FAILED(manager->ResetDevice(device.Get(), reset_token))) {
            manager.Reset();
            device.Reset();
            return false;
        }
        return true;
    }

    void readVideoOutputType() {
        ComPtr<IMFMediaType> type;
        if (FAILED(reader->GetCurrentMediaType(video_stream, &type))) return;
        GUID subtype{};
        type->GetGUID(MF_MT_SUBTYPE, &subtype);
        layout = subtype == MFVideoFormat_NV12 ? PixelLayout::Nv12 : PixelLayout::Bgra8;
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h);
        buf_w = w;
        buf_h = h;
        vis_x = vis_y = 0;
        vis_w = w;
        vis_h = h;
        MFVideoArea area{};
        UINT32 size = 0;
        if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area), &size)) &&
            size == sizeof(area) && area.Area.cx > 0 && area.Area.cy > 0) {
            vis_x = static_cast<std::uint32_t>(std::max<short>(0, area.OffsetX.value));
            vis_y = static_cast<std::uint32_t>(std::max<short>(0, area.OffsetY.value));
            vis_w = static_cast<std::uint32_t>(area.Area.cx);
            vis_h = static_cast<std::uint32_t>(area.Area.cy);
        }
        if (vis_x >= buf_w) vis_x = 0;
        if (vis_y >= buf_h) vis_y = 0;
        vis_w = std::min(vis_w, buf_w - vis_x);
        vis_h = std::min(vis_h, buf_h - vis_y);
        UINT32 stride = 0;
        if (SUCCEEDED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride))) {
            default_stride = static_cast<LONG>(static_cast<INT32>(stride));
        } else {
            // Sin el atributo: NV12 de arriba abajo; RGB de abajo arriba (convenio de MF).
            default_stride = layout == PixelLayout::Nv12 ? static_cast<LONG>(w) : -static_cast<LONG>(w * 4);
        }
        UINT32 matrix = 0, range = 0;
        if (SUCCEEDED(type->GetUINT32(MF_MT_YUV_MATRIX, &matrix))) {
            if (matrix == MFVideoTransferMatrix_BT601) color.matrix = 601;
            else if (matrix == MFVideoTransferMatrix_BT709) color.matrix = 709;
        }
        if (SUCCEEDED(type->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &range))) {
            color.full_range = range == MFNominalRange_0_255;
        }
        std::lock_guard<std::mutex> lock(info_mutex);
        info.width = vis_w;
        info.height = vis_h;
        info.color = color;
    }

    bool trySetVideo(const GUID& subtype, std::uint32_t w, std::uint32_t h) {
        const Api* a = api();
        ComPtr<IMFMediaType> type;
        if (a == nullptr || FAILED(a->CreateMediaType(&type))) return false;
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        type->SetGUID(MF_MT_SUBTYPE, subtype);
        if (w != 0 && h != 0) MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, w, h);
        return SUCCEEDED(reader->SetCurrentMediaType(video_stream, nullptr, type.Get()));
    }

    // NV12 (con escala si hay resolucion maxima) o RGB32 por el procesador avanzado.
    bool configureVideoOutput(std::string& error) {
        ComPtr<IMFMediaType> native;
        if (FAILED(reader->GetNativeMediaType(video_stream, MF_SOURCE_READER_CURRENT_TYPE_INDEX, &native)) &&
            FAILED(reader->GetNativeMediaType(video_stream, 0, &native))) {
            error = "Media Foundation no da el formato del video";
            return false;
        }
        GUID subtype{};
        native->GetGUID(MF_MT_SUBTYPE, &subtype);
        codec = codecName(subtype);
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &w, &h);
        UINT32 matrix = 0, range = 0;
        color.matrix = h >= 720 ? 709 : 601;
        color.full_range = false;
        if (SUCCEEDED(native->GetUINT32(MF_MT_YUV_MATRIX, &matrix))) {
            if (matrix == MFVideoTransferMatrix_BT601) color.matrix = 601;
            else if (matrix == MFVideoTransferMatrix_BT709) color.matrix = 709;
        }
        if (SUCCEEDED(native->GetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, &range))) color.full_range = range == MFNominalRange_0_255;

        std::uint32_t sw = 0, sh = 0;  // 0 = nativo
        if (w != 0 && h != 0 &&
            ((options.max_width != 0 && w > options.max_width) || (options.max_height != 0 && h > options.max_height))) {
            double s = 1.0;
            if (options.max_width != 0) s = std::min(s, static_cast<double>(options.max_width) / w);
            if (options.max_height != 0) s = std::min(s, static_cast<double>(options.max_height) / h);
            sw = std::max<std::uint32_t>(2, static_cast<std::uint32_t>(w * s)) & ~1u;
            sh = std::max<std::uint32_t>(2, static_cast<std::uint32_t>(h * s)) & ~1u;
        }
        const bool ok = (sw != 0 && trySetVideo(MFVideoFormat_NV12, sw, sh)) || trySetVideo(MFVideoFormat_NV12, 0, 0) ||
                        (sw != 0 && trySetVideo(MFVideoFormat_RGB32, sw, sh)) || trySetVideo(MFVideoFormat_RGB32, 0, 0);
        if (!ok) {
            error = codecHint(codec);
            return false;
        }
        readVideoOutputType();
        return true;
    }

    void readAudioOutputType(Slot& slot) {
        ComPtr<IMFMediaType> type;
        if (FAILED(reader->GetCurrentMediaType(slot.stream, &type))) return;
        UINT32 rate = 0, channels = 0, bits = 32;
        type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
        type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
        type->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
        GUID subtype{};
        type->GetGUID(MF_MT_SUBTYPE, &subtype);
        slot.rate = rate;
        slot.channels = std::max<UINT32>(1, channels);
        slot.pcm16 = subtype == MFAudioFormat_PCM && bits == 16;
    }

    bool configureAudio(Slot& slot) {
        const Api* a = api();
        const auto attempt = [&](bool hints) {
            ComPtr<IMFMediaType> type;
            if (a == nullptr || FAILED(a->CreateMediaType(&type))) return false;
            type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
            if (hints && options.audio_rate != 0) type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, options.audio_rate);
            if (hints && options.audio_channels != 0) type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, options.audio_channels);
            return SUCCEEDED(reader->SetCurrentMediaType(slot.stream, nullptr, type.Get()));
        };
        const bool hints = options.audio_rate != 0 || options.audio_channels != 0;
        if (!(hints && attempt(true)) && !attempt(false)) return false;
        readAudioOutputType(slot);
        return slot.rate != 0;
    }

    bool openReader(const std::function<HRESULT(IMFAttributes*, IMFSourceReader**)>& create, std::string& error) {
        const Api* a = api(&error);
        if (a == nullptr) return false;
        ComPtr<IMFAttributes> attrs;
        if (FAILED(a->CreateAttributes(&attrs, 8))) {
            error = "Media Foundation: no se pudieron crear los atributos del lector";
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(abort_mutex);
            callback.Attach(new ReaderCallback());
            if (interrupted.load()) callback->interrupt();
        }
        attrs->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, callback.Get());
        bool hw = false;
        if (options.mode != DecodeMode::Software) {
            hw = createD3D();
            if (!hw && options.mode == DecodeMode::Hardware) {
                error = "No se pudo crear el dispositivo D3D11 de video para decodificar por hardware "
                        "(elige Auto o Software)";
                return false;
            }
            if (hw) {
                attrs->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, manager.Get());
                attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
            }
        }
        attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        if (options.low_latency) attrs->SetUINT32(MF_LOW_LATENCY, TRUE);

        const HRESULT hr = create(attrs.Get(), &reader);
        if (interrupted.load()) {
            error = "Apertura interrumpida";
            return false;
        }
        if (FAILED(hr)) {
            error = hresultText(hr);
            return false;
        }

        // Flujos: el primer video y todas las pistas de audio.
        StreamInfo si;
        si.backend = "MediaFoundation";
        for (DWORD i = 0; i < 64; ++i) {
            ComPtr<IMFMediaType> type;
            const HRESULT th = reader->GetNativeMediaType(i, 0, &type);
            if (th == MF_E_INVALIDSTREAMNUMBER) break;
            if (FAILED(th)) continue;
            GUID major{};
            type->GetMajorType(&major);
            if (major == MFMediaType_Video && video_stream == kNone) {
                video_stream = i;
                UINT32 num = 0, den = 0;
                if (SUCCEEDED(MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &num, &den)) && den != 0) {
                    si.fps = static_cast<double>(num) / den;
                }
            } else if (major == MFMediaType_Audio) {
                AudioTrackInfo t;
                UINT32 channels = 0, rate = 0;
                type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
                type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
                t.channels = channels;
                t.rate = rate;
                t.language = propString(reader.Get(), i, MF_SD_LANGUAGE);
                t.name = propString(reader.Get(), i, MF_SD_STREAM_NAME);
                if (t.name.empty()) t.name = "Pista " + std::to_string(si.audio.size() + 1);
                si.audio.push_back(t);
                audio_streams.push_back(i);
            }
        }
        if (video_stream == kNone) {
            error = "El archivo no tiene pista de video";
            return false;
        }

        reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
        reader->SetStreamSelection(video_stream, TRUE);
        if (!configureVideoOutput(error)) return false;
        si.codec = codec;

        Slot video;
        video.stream = video_stream;
        slots.push_back(video);
        if (!options.probe_only) {
            for (const int t : options.audio_tracks) {
                if (t < 0 || t >= static_cast<int>(audio_streams.size())) continue;
                const DWORD stream = audio_streams[static_cast<std::size_t>(t)];
                if (std::any_of(slots.begin(), slots.end(), [&](const Slot& s) { return s.stream == stream; })) continue;
                Slot s;
                s.stream = stream;
                s.track = t;
                reader->SetStreamSelection(stream, TRUE);
                if (!configureAudio(s)) {
                    reader->SetStreamSelection(stream, FALSE);
                    std::cerr << "[Video] La pista de audio " << (t + 1) << " no se puede decodificar a float; se omite\n";
                    continue;
                }
                slots.push_back(s);
            }
        }

        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &v)) &&
            v.vt == VT_UI8 && v.uhVal.QuadPart > 0) {
            si.duration = static_cast<double>(v.uhVal.QuadPart) / 1e7;
        }
        PropVariantClear(&v);
        PropVariantInit(&v);
        if (SUCCEEDED(reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE,
                                                       MF_SOURCE_READER_MEDIASOURCE_CHARACTERISTICS, &v)) &&
            v.vt == VT_UI4) {
            si.seekable = (v.ulVal & MFMEDIASOURCE_CAN_SEEK) != 0;
            si.is_live = (v.ulVal & MFMEDIASOURCE_IS_LIVE) != 0;
        }
        PropVariantClear(&v);
        if (si.is_live) si.duration = -1.0;

        std::lock_guard<std::mutex> lock(info_mutex);
        si.width = vis_w;
        si.height = vis_h;
        si.color = color;
        si.hardware = false;  // se confirma con el primer frame en memoria de GPU
        info = si;
        return true;
    }

    bool convertVideo(const ReadItem& item, VideoFrame& frame) {
        ComPtr<IMFMediaBuffer> buffer;
        DWORD count = 0;
        item.sample->GetBufferCount(&count);
        const HRESULT bh = count == 1 ? item.sample->GetBufferByIndex(0, &buffer) : item.sample->ConvertToContiguousBuffer(&buffer);
        if (FAILED(bh) || !buffer) return false;
        {
            ComPtr<IMFDXGIBuffer> dxgi;
            if (SUCCEEDED(buffer.As(&dxgi))) {
                std::lock_guard<std::mutex> lock(info_mutex);
                info.hardware = true;
            }
        }

        BYTE* scan0 = nullptr;
        LONG pitch = 0;
        DWORD length = 0;  // 0 = no se sabe
        ComPtr<IMF2DBuffer2> b2;
        ComPtr<IMF2DBuffer> b1;
        bool locked2 = false, locked1 = false, locked0 = false;
        if (SUCCEEDED(buffer.As(&b2))) {
            BYTE* start = nullptr;
            locked2 = SUCCEEDED(b2->Lock2DSize(MF2DBuffer_LockFlags_Read, &scan0, &pitch, &start, &length));
        }
        if (!locked2 && SUCCEEDED(buffer.As(&b1))) {
            locked1 = SUCCEEDED(b1->Lock2D(&scan0, &pitch));
        }
        if (!locked2 && !locked1) {
            BYTE* raw = nullptr;
            DWORD max_length = 0;
            if (FAILED(buffer->Lock(&raw, &max_length, &length))) return false;
            locked0 = true;
            pitch = default_stride;
            scan0 = pitch < 0 ? raw + static_cast<std::size_t>(-pitch) * (buf_h - 1) : raw;
        }
        const auto unlock = [&] {
            if (locked2) b2->Unlock2D();
            else if (locked1) b1->Unlock2D();
            else if (locked0) buffer->Unlock();
        };
        if (scan0 == nullptr || pitch == 0) {
            unlock();
            return false;
        }

        const std::size_t abs_pitch = static_cast<std::size_t>(pitch < 0 ? -pitch : pitch);
        bool ok = true;
        frame.pts = static_cast<double>(item.timestamp) / 1e7;
        frame.layout = layout;
        frame.color = color;
        if (layout == PixelLayout::Nv12) {
            const std::uint32_t w = vis_w & ~1u, h = vis_h & ~1u;
            // El plano UV empieza tras las filas reales de la superficie, que
            // pueden ser mas que el alto del tipo (texturas D3D11 alineadas):
            // si la longitud de Lock2DSize es pitch * filas * 3/2, se usan esas filas.
            std::uint32_t plane_h = buf_h;
            if (length != 0) {
                const std::size_t rows = length / abs_pitch;
                const auto from_length = static_cast<std::uint32_t>(rows * 2 / 3);
                if (abs_pitch * plane_h * 3 / 2 > length) plane_h = from_length;
                else if (locked2 && length % abs_pitch == 0 && rows % 3 == 0 && from_length > plane_h) plane_h = from_length;
            }
            if (w == 0 || h == 0 || vis_y + h > plane_h || vis_x + w > abs_pitch) {
                ok = false;
            } else {
                const std::size_t y_size = static_cast<std::size_t>(w) * h;
                auto data = pool->get(y_size + y_size / 2);
                std::uint8_t* dst = data->data();
                const BYTE* uv0 = scan0 + static_cast<std::ptrdiff_t>(pitch) * plane_h;
                for (std::uint32_t r = 0; r < h; ++r) {
                    std::memcpy(dst + static_cast<std::size_t>(r) * w,
                                scan0 + static_cast<std::ptrdiff_t>(pitch) * (vis_y + r) + vis_x, w);
                }
                std::uint8_t* dst_uv = dst + y_size;
                for (std::uint32_t r = 0; r < h / 2; ++r) {
                    std::memcpy(dst_uv + static_cast<std::size_t>(r) * w,
                                uv0 + static_cast<std::ptrdiff_t>(pitch) * (vis_y / 2 + r) + (vis_x & ~1u), w);
                }
                frame.width = w;
                frame.height = h;
                frame.planes = {dst, dst_uv};
                frame.strides = {w, w};
                frame.owner = std::shared_ptr<const void>(data, data->data());
            }
        } else {
            const std::uint32_t w = vis_w, h = vis_h;
            if (w == 0 || h == 0 || static_cast<std::size_t>(vis_x + w) * 4 > abs_pitch || vis_y + h > buf_h) {
                ok = false;
            } else {
                auto data = pool->get(static_cast<std::size_t>(w) * h * 4);
                std::uint8_t* dst = data->data();
                for (std::uint32_t r = 0; r < h; ++r) {
                    std::uint8_t* row = dst + static_cast<std::size_t>(r) * w * 4;
                    std::memcpy(row, scan0 + static_cast<std::ptrdiff_t>(pitch) * (vis_y + r) + vis_x * 4,
                                static_cast<std::size_t>(w) * 4);
                    // RGB32 trae X en el cuarto byte: alfa opaco.
                    for (std::uint32_t x = 0; x < w; ++x) row[x * 4 + 3] = 0xFF;
                }
                frame.width = w;
                frame.height = h;
                frame.planes = {dst, nullptr};
                frame.strides = {w * 4, 0};
                frame.owner = std::shared_ptr<const void>(data, data->data());
            }
        }
        unlock();
        return ok;
    }

    bool convertAudio(const ReadItem& item, const Slot& slot, AudioChunk& chunk) {
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(item.sample->ConvertToContiguousBuffer(&buffer))) return false;
        BYTE* data = nullptr;
        DWORD length = 0;
        if (FAILED(buffer->Lock(&data, nullptr, &length))) return false;
        chunk.track = slot.track;
        chunk.pts = static_cast<double>(item.timestamp) / 1e7;
        chunk.rate = slot.rate;
        chunk.channels = slot.channels;
        if (slot.pcm16) {
            const std::size_t n = (length / 2) / slot.channels * slot.channels;
            chunk.samples.resize(n);
            const auto* src = reinterpret_cast<const std::int16_t*>(data);
            for (std::size_t i = 0; i < n; ++i) chunk.samples[i] = static_cast<float>(src[i]) / 32768.0f;
        } else {
            const std::size_t n = (length / sizeof(float)) / slot.channels * slot.channels;
            chunk.samples.resize(n);
            if (n > 0) std::memcpy(chunk.samples.data(), data, n * sizeof(float));
        }
        buffer->Unlock();
        return true;
    }

    Slot* slotFor(DWORD stream) {
        for (Slot& s : slots) {
            if (s.stream == stream) return &s;
        }
        return nullptr;
    }
};

MfDecoder::MfDecoder() : s_(std::make_unique<State>()) {}

MfDecoder::~MfDecoder() = default;

bool MfDecoder::open(const std::string& uri, const DecodeOptions& options, std::string& error) {
    s_->options = options;
    if (!s_->session.acquire(&error)) return false;
    if (detail::isNetworkUri(uri)) {
        std::string why;
        if (!net::checkUrl(uri, &why)) {
            error = why;
            return false;
        }
        ComPtr<HttpByteStream> stream;
        stream.Attach(new HttpByteStream(uri));
        {
            std::lock_guard<std::mutex> lock(s_->abort_mutex);
            s_->abortable = stream;
            if (s_->interrupted.load()) stream->abort();
        }
        if (!stream->start(error)) return false;
        const Api* a = api(&error);
        if (a == nullptr) return false;
        return s_->openReader(
            [&](IMFAttributes* attrs, IMFSourceReader** reader) {
                return a->CreateSourceReaderFromByteStream(stream.Get(), attrs, reader);
            },
            error);
    }

    std::error_code ec;
    const std::filesystem::path path(widen(uri));
    if (!std::filesystem::is_regular_file(path, ec)) {
        error = "No existe el archivo de video: " + uri;
        return false;
    }
    const std::wstring wide = std::filesystem::path(path).make_preferred().wstring();
    const Api* a = api(&error);
    if (a == nullptr) return false;
    return s_->openReader(
        [&](IMFAttributes* attrs, IMFSourceReader** reader) {
            return a->CreateSourceReaderFromURL(wide.c_str(), attrs, reader);
        },
        error);
}

bool MfDecoder::openStream(IMFByteStream* stream, ByteStreamBase* abortable, const DecodeOptions& options,
                           std::string& error) {
    s_->options = options;
    if (stream == nullptr) {
        error = "Sin datos de video";
        return false;
    }
    if (!s_->session.acquire(&error)) return false;
    {
        std::lock_guard<std::mutex> lock(s_->abort_mutex);
        s_->abortable = abortable;
        if (abortable != nullptr && s_->interrupted.load()) abortable->abort();
    }
    const Api* a = api(&error);
    if (a == nullptr) return false;
    return s_->openReader(
        [&](IMFAttributes* attrs, IMFSourceReader** reader) {
            return a->CreateSourceReaderFromByteStream(stream, attrs, reader);
        },
        error);
}

StreamInfo MfDecoder::info() const {
    std::lock_guard<std::mutex> lock(s_->info_mutex);
    return s_->info;
}

DecodeEvent MfDecoder::next(VideoFrame& video, AudioChunk& audio, std::string& error) {
    State& s = *s_;
    if (!s.reader || !s.callback) {
        error = "El decodificador no esta abierto";
        return DecodeEvent::Error;
    }
    using clock = std::chrono::steady_clock;
    // Sin nada que entregar en este tiempo: NeedMore (el hilo mira si debe
    // parar y marca Buffering).
    const auto deadline = clock::now() + std::chrono::milliseconds(100);
    // Otro flujo puede tardar (red); pasado esto se entrega lo que hay.
    constexpr auto kMaxSkew = std::chrono::milliseconds(300);

    for (;;) {
        if (s.interrupted.load()) return DecodeEvent::NeedMore;

        // 1. Una peticion pendiente por flujo activo.
        for (State::Slot& slot : s.slots) {
            if (slot.ended || slot.pending || slot.has_ready) continue;
            const HRESULT hr = s.reader->ReadSample(slot.stream, 0, nullptr, nullptr, nullptr, nullptr);
            if (FAILED(hr)) {
                error = hresultText(hr);
                return DecodeEvent::Error;
            }
            slot.pending = true;
        }

        // 2. Recoger lo que llego.
        std::deque<ReadItem> items;
        {
            std::lock_guard<std::mutex> lock(s.callback->mutex);
            items.swap(s.callback->results);
        }
        const auto now = clock::now();
        for (ReadItem& item : items) {
            State::Slot* slot = s.slotFor(item.stream);
            if (slot == nullptr) continue;
            slot->pending = false;
            if (FAILED(item.hr)) {
                error = hresultText(item.hr);
                return DecodeEvent::Error;
            }
            if ((item.flags & MF_SOURCE_READERF_ERROR) != 0) {
                error = "Media Foundation dio un error al leer el video";
                return DecodeEvent::Error;
            }
            if (slot->track < 0 && (item.flags & MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED) != 0) {
                // Cambio de resolucion (otra variante): se vuelve a pedir la salida.
                if (!s.configureVideoOutput(error)) return DecodeEvent::Error;
            } else if ((item.flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0) {
                if (slot->track < 0) s.readVideoOutputType();
                else s.readAudioOutputType(*slot);
            }
            if (item.sample) {
                slot->has_ready = true;
                slot->ready = std::move(item);
                slot->ready_since = now;
                if ((slot->ready.flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) slot->end_after = true;
            } else if ((item.flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
                slot->ended = true;
            }
            // STREAMTICK u otro sin muestra: se vuelve a pedir en la siguiente vuelta.
        }

        // 3. Elegir por pts: el menor de los que tienen muestra, esperando a
        // que todos los flujos activos tengan una (o pase kMaxSkew).
        State::Slot* best = nullptr;
        bool waiting = false, any_active = false;
        auto oldest = now;
        for (State::Slot& slot : s.slots) {
            if (slot.has_ready) {
                any_active = true;
                if (best == nullptr || slot.ready.timestamp < best->ready.timestamp) best = &slot;
                oldest = std::min(oldest, slot.ready_since);
            } else if (!slot.ended) {
                any_active = true;
                waiting = true;
            }
        }
        if (!any_active) return DecodeEvent::EndOfStream;
        if (best != nullptr && (!waiting || now - oldest >= kMaxSkew)) {
            ReadItem item = std::move(best->ready);
            best->has_ready = false;
            best->ready = ReadItem{};
            if (best->end_after) best->ended = true;
            if (best->track < 0) {
                if (!s.convertVideo(item, video)) {
                    if (!s.warned_frame) {
                        std::cerr << "[Video] Un frame no se pudo leer de la memoria de video; se salta\n";
                        s.warned_frame = true;
                    }
                    continue;
                }
                return DecodeEvent::Video;
            }
            if (!s.convertAudio(item, *best, audio)) continue;
            return DecodeEvent::Audio;
        }
        if (clock::now() >= deadline) return DecodeEvent::NeedMore;

        // 4. Esperar a Media Foundation (o a interrupt()).
        std::unique_lock<std::mutex> lock(s.callback->mutex);
        s.callback->cv.wait_for(lock, std::chrono::milliseconds(10),
                                [&] { return !s.callback->results.empty() || s.callback->interrupted; });
    }
}

bool MfDecoder::seek(double seconds) {
    State& s = *s_;
    if (!s.reader) return false;
    {
        std::lock_guard<std::mutex> lock(s.info_mutex);
        if (!s.info.seekable) return false;
    }
    s.flushAndWait(std::chrono::milliseconds(3000), true);
    if (s.interrupted.load()) return false;  // parando: sin buscar
    PROPVARIANT position;
    PropVariantInit(&position);
    position.vt = VT_I8;
    position.hVal.QuadPart = static_cast<LONGLONG>(std::llround(std::max(0.0, seconds) * 1e7));
    const HRESULT hr = s.reader->SetCurrentPosition(GUID{}, position);
    for (State::Slot& slot : s.slots) {
        slot.ended = false;
        slot.end_after = false;
    }
    return SUCCEEDED(hr);
}

void MfDecoder::interrupt() {
    // abort_mutex: open() crea el callback y el byte stream en otro hilo.
    std::lock_guard<std::mutex> lock(s_->abort_mutex);
    s_->interrupted.store(true);
    if (s_->callback) s_->callback->interrupt();
    if (s_->abortable) s_->abortable->abort();
}

}  // namespace cramion::video::mf

namespace cramion::video::detail {

std::unique_ptr<IVideoDecoder> makeMfDecoder() { return std::make_unique<mf::MfDecoder>(); }

bool mediaFoundationAvailable(std::string* why) { return mf::api(why) != nullptr; }

}  // namespace cramion::video::detail

#endif  // _WIN32

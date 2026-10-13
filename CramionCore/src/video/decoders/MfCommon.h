#ifndef CRAMION_VIDEO_MF_COMMON_H
#define CRAMION_VIDEO_MF_COMMON_H

// Comun de Media Foundation (MfDecoder.cpp y MfEngineDecoder.cpp). Solo Windows.
//
// - mfplat.dll y mfreadwrite.dll se cargan con LoadLibrary al usarlas (nunca
//   se enlazan): en Windows N/KN sin Media Feature Pack el editor y el juego
//   arrancan igual y el video da un error claro.
// - COM (MTA) y MFStartup se inician en el hilo que abre el decodificador
//   (el del reproductor o el de la importacion), nunca en el hilo principal.
//   MFStartup lleva un contador global; MFShutdown solo cuando llega a 0.
// - ByteStreamBase: IMFByteStream + IMFAttributes propios para leer por
//   net::httpRequest (politica de Http.h, interrupt() de verdad) y para HLS.

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// Las GUID de Media Foundation (DEFINE_GUID de mfapi.h) se definen aqui como
// selectany, despues de windows.h para no redefinir las de COM: no hace falta
// enlazar mfplat.lib, mfreadwrite.lib ni mfuuid.lib (EXTERN_GUID ya es selectany).
#include <initguid.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include "CramionCore/video/VideoDecoder.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace cramion::video::mf {

using Microsoft::WRL::ComPtr;

// Funciones de mfplat.dll / mfreadwrite.dll (cargadas en tiempo de ejecucion).
struct Api {
    decltype(&::MFStartup) Startup = nullptr;
    decltype(&::MFShutdown) Shutdown = nullptr;
    decltype(&::MFCreateAttributes) CreateAttributes = nullptr;
    decltype(&::MFCreateMediaType) CreateMediaType = nullptr;
    decltype(&::MFCreateDXGIDeviceManager) CreateDXGIDeviceManager = nullptr;
    decltype(&::MFCreateAsyncResult) CreateAsyncResult = nullptr;
    decltype(&::MFInvokeCallback) InvokeCallback = nullptr;
    decltype(&::MFCreateSourceReaderFromURL) CreateSourceReaderFromURL = nullptr;
    decltype(&::MFCreateSourceReaderFromByteStream) CreateSourceReaderFromByteStream = nullptr;
    // Para las pruebas (CramionVideoTests genera un MP4 con IMFSinkWriter).
    decltype(&::MFCreateSinkWriterFromURL) CreateSinkWriterFromURL = nullptr;
    decltype(&::MFCreateSample) CreateSample = nullptr;
    decltype(&::MFCreateMemoryBuffer) CreateMemoryBuffer = nullptr;
};

// nullptr si falta Media Foundation (why = que instalar). Se carga una vez y
// las DLL no se descargan nunca (un hilo soltado con detach puede seguir en ellas).
const Api* api(std::string* why = nullptr);

// Inicia COM (MTA) en este hilo (una vez por hilo) y MFStartup (contador global).
class Session {
public:
    Session() = default;
    ~Session() { release(); }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    bool acquire(std::string* why);
    void release();
    bool active() const { return active_; }

private:
    bool active_ = false;
};

std::wstring widen(const std::string& s);
std::string narrow(const std::wstring& s);
// "Media Foundation 0xC00D36C4: ..." con un texto claro para los errores comunes.
std::string hresultText(HRESULT hr);

// IMFByteStream de solo lectura con su almacen de atributos
// (MF_BYTESTREAM_CONTENT_TYPE, MF_BYTESTREAM_ORIGIN_NAME). BeginRead lee en
// el acto y entrega el resultado con MFInvokeCallback. abort() hace que las
// lecturas pendientes y las siguientes devuelvan E_ABORT.
class ByteStreamBase : public IMFByteStream, public IMFAttributes {
public:
    ByteStreamBase();
    virtual ~ByteStreamBase();
    bool initAttributes();  // false si MFCreateAttributes falla
    void abort() { cancel_.store(true); }
    bool aborted() const { return cancel_.load(); }
    std::atomic<bool>* cancelFlag() { return &cancel_; }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IMFByteStream
    STDMETHODIMP GetCapabilities(DWORD* caps) override;
    STDMETHODIMP GetLength(QWORD* length) override;
    STDMETHODIMP SetLength(QWORD) override { return E_NOTIMPL; }
    STDMETHODIMP GetCurrentPosition(QWORD* position) override;
    STDMETHODIMP SetCurrentPosition(QWORD position) override;
    STDMETHODIMP IsEndOfStream(BOOL* end) override;
    STDMETHODIMP Read(BYTE* pb, ULONG cb, ULONG* read) override;
    STDMETHODIMP BeginRead(BYTE* pb, ULONG cb, IMFAsyncCallback* callback, IUnknown* state) override;
    STDMETHODIMP EndRead(IMFAsyncResult* result, ULONG* read) override;
    STDMETHODIMP Write(const BYTE*, ULONG, ULONG*) override { return E_ACCESSDENIED; }
    STDMETHODIMP BeginWrite(const BYTE*, ULONG, IMFAsyncCallback*, IUnknown*) override { return E_ACCESSDENIED; }
    STDMETHODIMP EndWrite(IMFAsyncResult*, ULONG*) override { return E_ACCESSDENIED; }
    STDMETHODIMP Seek(MFBYTESTREAM_SEEK_ORIGIN origin, LONGLONG offset, DWORD flags, QWORD* position) override;
    STDMETHODIMP Flush() override { return S_OK; }
    STDMETHODIMP Close() override;

    // IMFAttributes (todo se reenvia a attrs_)
    STDMETHODIMP GetItem(REFGUID key, PROPVARIANT* value) override { return attrs_->GetItem(key, value); }
    STDMETHODIMP GetItemType(REFGUID key, MF_ATTRIBUTE_TYPE* type) override { return attrs_->GetItemType(key, type); }
    STDMETHODIMP CompareItem(REFGUID key, REFPROPVARIANT value, BOOL* result) override {
        return attrs_->CompareItem(key, value, result);
    }
    STDMETHODIMP Compare(IMFAttributes* theirs, MF_ATTRIBUTES_MATCH_TYPE type, BOOL* result) override {
        return attrs_->Compare(theirs, type, result);
    }
    STDMETHODIMP GetUINT32(REFGUID key, UINT32* value) override { return attrs_->GetUINT32(key, value); }
    STDMETHODIMP GetUINT64(REFGUID key, UINT64* value) override { return attrs_->GetUINT64(key, value); }
    STDMETHODIMP GetDouble(REFGUID key, double* value) override { return attrs_->GetDouble(key, value); }
    STDMETHODIMP GetGUID(REFGUID key, GUID* value) override { return attrs_->GetGUID(key, value); }
    STDMETHODIMP GetStringLength(REFGUID key, UINT32* length) override { return attrs_->GetStringLength(key, length); }
    STDMETHODIMP GetString(REFGUID key, LPWSTR value, UINT32 size, UINT32* length) override {
        return attrs_->GetString(key, value, size, length);
    }
    STDMETHODIMP GetAllocatedString(REFGUID key, LPWSTR* value, UINT32* length) override {
        return attrs_->GetAllocatedString(key, value, length);
    }
    STDMETHODIMP GetBlobSize(REFGUID key, UINT32* size) override { return attrs_->GetBlobSize(key, size); }
    STDMETHODIMP GetBlob(REFGUID key, UINT8* buf, UINT32 size, UINT32* blob_size) override {
        return attrs_->GetBlob(key, buf, size, blob_size);
    }
    STDMETHODIMP GetAllocatedBlob(REFGUID key, UINT8** buf, UINT32* size) override {
        return attrs_->GetAllocatedBlob(key, buf, size);
    }
    STDMETHODIMP GetUnknown(REFGUID key, REFIID riid, LPVOID* ppv) override { return attrs_->GetUnknown(key, riid, ppv); }
    STDMETHODIMP SetItem(REFGUID key, REFPROPVARIANT value) override { return attrs_->SetItem(key, value); }
    STDMETHODIMP DeleteItem(REFGUID key) override { return attrs_->DeleteItem(key); }
    STDMETHODIMP DeleteAllItems() override { return attrs_->DeleteAllItems(); }
    STDMETHODIMP SetUINT32(REFGUID key, UINT32 value) override { return attrs_->SetUINT32(key, value); }
    STDMETHODIMP SetUINT64(REFGUID key, UINT64 value) override { return attrs_->SetUINT64(key, value); }
    STDMETHODIMP SetDouble(REFGUID key, double value) override { return attrs_->SetDouble(key, value); }
    STDMETHODIMP SetGUID(REFGUID key, REFGUID value) override { return attrs_->SetGUID(key, value); }
    STDMETHODIMP SetString(REFGUID key, LPCWSTR value) override { return attrs_->SetString(key, value); }
    STDMETHODIMP SetBlob(REFGUID key, const UINT8* buf, UINT32 size) override { return attrs_->SetBlob(key, buf, size); }
    STDMETHODIMP SetUnknown(REFGUID key, IUnknown* value) override { return attrs_->SetUnknown(key, value); }
    STDMETHODIMP LockStore() override { return attrs_->LockStore(); }
    STDMETHODIMP UnlockStore() override { return attrs_->UnlockStore(); }
    STDMETHODIMP GetCount(UINT32* items) override { return attrs_->GetCount(items); }
    STDMETHODIMP GetItemByIndex(UINT32 index, GUID* key, PROPVARIANT* value) override {
        return attrs_->GetItemByIndex(index, key, value);
    }
    STDMETHODIMP CopyAllItems(IMFAttributes* dest) override { return attrs_->CopyAllItems(dest); }

protected:
    // Lee hasta `cb` bytes desde `position` (puede bloquear: red). *read = 0 al final.
    virtual HRESULT readAt(QWORD position, BYTE* pb, ULONG cb, ULONG* read) = 0;
    virtual DWORD capabilities() const = 0;
    virtual QWORD length() const = 0;  // static_cast<QWORD>(-1) si no se sabe
    virtual bool canSeekTo(QWORD position) const = 0;

    std::atomic<bool> cancel_{false};

private:
    std::atomic<ULONG> refs_{1};
    ComPtr<IMFAttributes> attrs_;
    std::mutex position_mutex_;
    QWORD position_ = 0;
    bool end_ = false;
};

// Decodificador de Media Foundation con IMFSourceReader asincrono (interrupt()
// despierta next() de verdad). Abre un archivo, una URL http(s) (por
// net::httpRequest con peticiones Range) o un IMFByteStream ya hecho (HLS).
class MfDecoder final : public IVideoDecoder {
public:
    MfDecoder();
    ~MfDecoder() override;
    bool open(const std::string& uri, const DecodeOptions& options, std::string& error) override;
    // `stream` ya preparado (atributos de tipo de contenido puestos). Si es un
    // ByteStreamBase, interrupt() lo aborta.
    bool openStream(IMFByteStream* stream, ByteStreamBase* abortable, const DecodeOptions& options,
                    std::string& error);
    StreamInfo info() const override;
    DecodeEvent next(VideoFrame& video, AudioChunk& audio, std::string& error) override;
    bool seek(double seconds) override;
    void interrupt() override;

private:
    struct State;
    std::unique_ptr<State> s_;
};

}  // namespace cramion::video::mf

#endif  // _WIN32
#endif  // CRAMION_VIDEO_MF_COMMON_H

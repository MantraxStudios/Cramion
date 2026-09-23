#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

namespace cramion::dm {

using Microsoft::WRL::ComPtr;

// Encapsula la inicialización del dispositivo DirectX 12:
// fábrica DXGI, selección de adaptador, dispositivo y cola de comandos.
class Device {
public:
    Device() = default;
    ~Device() = default;

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    // Inicializa DX12. Con enableDebugLayer se activa la capa de depuración
    // (requiere el "Graphics Tools" de Windows). Devuelve false si falla.
    bool initialize(bool enableDebugLayer = true);

    // Nombre del adaptador seleccionado (GPU).
    const std::wstring& adapterName() const { return adapterName_; }

    // VRAM dedicada del adaptador en bytes.
    uint64_t dedicatedVideoMemory() const { return dedicatedVideoMemory_; }

    ID3D12Device* device() const { return device_.Get(); }
    ID3D12CommandQueue* graphicsQueue() const { return graphicsQueue_.Get(); }
    IDXGIFactory6* factory() const { return factory_.Get(); }

    bool isValid() const { return device_ != nullptr; }

private:
    bool enableDebugLayer();
    bool createFactory();
    bool selectAdapter();
    bool createDevice();
    bool createCommandQueue();

    ComPtr<IDXGIFactory6> factory_;
    ComPtr<IDXGIAdapter1> adapter_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> graphicsQueue_;

    std::wstring adapterName_;
    uint64_t dedicatedVideoMemory_ = 0;
    bool debugLayerEnabled_ = false;
};

}  // namespace cramion::dm

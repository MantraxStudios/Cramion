#include "CramionDM/Device.h"

#include <d3d12sdklayers.h>

#include <iostream>

namespace cramion::dm {

namespace {
constexpr D3D_FEATURE_LEVEL kMinFeatureLevel = D3D_FEATURE_LEVEL_11_0;
}

bool Device::initialize(bool wantDebugLayer) {
    if (wantDebugLayer) {
        debugLayerEnabled_ = enableDebugLayer();
    }
    if (!createFactory()) {
        return false;
    }
    if (!selectAdapter()) {
        return false;
    }
    if (!createDevice()) {
        return false;
    }
    if (!createCommandQueue()) {
        return false;
    }
    return true;
}

bool Device::enableDebugLayer() {
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
        debugController->EnableDebugLayer();
        return true;
    }
    std::wcerr << L"[CramionDM] No se pudo habilitar la capa de depuración (Graphics Tools?).\n";
    return false;
}

bool Device::createFactory() {
    UINT flags = 0;
    if (debugLayerEnabled_) {
        flags |= DXGI_CREATE_FACTORY_DEBUG;
    }
    if (FAILED(CreateDXGIFactory2(flags, IID_PPV_ARGS(&factory_)))) {
        std::wcerr << L"[CramionDM] CreateDXGIFactory2 falló.\n";
        return false;
    }
    return true;
}

bool Device::selectAdapter() {
    // Preferir la GPU de mayor rendimiento con soporte DX12.
    for (UINT i = 0;
         factory_->EnumAdapterByGpuPreference(
             i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter_)) != DXGI_ERROR_NOT_FOUND;
         ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter_->GetDesc1(&desc);

        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            continue;  // Ignorar el adaptador software (WARP).
        }

        // Comprobar que el adaptador soporta crear un dispositivo DX12.
        if (SUCCEEDED(D3D12CreateDevice(adapter_.Get(), kMinFeatureLevel, __uuidof(ID3D12Device),
                                        nullptr))) {
            adapterName_ = desc.Description;
            dedicatedVideoMemory_ = desc.DedicatedVideoMemory;
            return true;
        }
    }
    std::wcerr << L"[CramionDM] No se encontró un adaptador compatible con DX12.\n";
    return false;
}

bool Device::createDevice() {
    if (FAILED(D3D12CreateDevice(adapter_.Get(), kMinFeatureLevel, IID_PPV_ARGS(&device_)))) {
        std::wcerr << L"[CramionDM] D3D12CreateDevice falló.\n";
        return false;
    }
    device_->SetName(L"CramionDM Device");
    return true;
}

bool Device::createCommandQueue() {
    D3D12_COMMAND_QUEUE_DESC desc{};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    desc.NodeMask = 0;

    if (FAILED(device_->CreateCommandQueue(&desc, IID_PPV_ARGS(&graphicsQueue_)))) {
        std::wcerr << L"[CramionDM] CreateCommandQueue falló.\n";
        return false;
    }
    graphicsQueue_->SetName(L"CramionDM Graphics Queue");
    return true;
}

}  // namespace cramion::dm

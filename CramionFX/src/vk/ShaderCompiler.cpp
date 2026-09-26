// GLSL -> SPIR-V en tiempo de ejecucion con la API en C de shaderc
// (shaderc_shared.dll). Se enlaza a mano con LoadLibrary: sin la DLL el motor
// arranca igual y solo los shaders del usuario dan un error claro.

#include "CramionFX/vk/ShaderCompiler.h"

#include <windows.h>

#include <cstring>
#include <mutex>

namespace cramion::gfx::shaders {

namespace {

// Lo minimo de shaderc.h (los valores son los de la cabecera del SDK).
using Compiler = void*;
using Options = void*;
using Result = void*;
constexpr int kVertexShader = 0;             // shaderc_vertex_shader
constexpr int kFragmentShader = 1;           // shaderc_fragment_shader
constexpr int kTargetEnvVulkan = 0;          // shaderc_target_env_vulkan
constexpr unsigned kVulkan13 = (1u << 22) | (3u << 12);  // shaderc_env_version_vulkan_1_3
constexpr int kOptimizePerformance = 2;      // shaderc_optimization_level_performance
constexpr int kStatusSuccess = 0;            // shaderc_compilation_status_success

struct Api {
    HMODULE dll = nullptr;
    Compiler (*compiler_initialize)() = nullptr;
    Options (*options_initialize)() = nullptr;
    void (*options_release)(Options) = nullptr;
    void (*options_set_optimization_level)(Options, int) = nullptr;
    void (*options_set_target_env)(Options, int, unsigned) = nullptr;
    Result (*compile_into_spv)(Compiler, const char*, size_t, int, const char*, const char*, Options) = nullptr;
    int (*result_get_compilation_status)(Result) = nullptr;
    size_t (*result_get_length)(Result) = nullptr;
    const char* (*result_get_bytes)(Result) = nullptr;
    const char* (*result_get_error_message)(Result) = nullptr;
    void (*result_release)(Result) = nullptr;
    Compiler compiler = nullptr;
    std::string failure;
};

template <typename T>
bool bind(HMODULE dll, const char* name, T& out) {
    out = reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(dll, name)));
    return out != nullptr;
}

Api& api() {
    static Api a;
    static std::once_flag once;
    std::call_once(once, [] {
        // Primero junto al ejecutable (lo copia CMake y lo lleva el juego exportado).
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring path(exe);
        path = path.substr(0, path.find_last_of(L"\\/") + 1) + L"shaderc_shared.dll";
        a.dll = LoadLibraryW(path.c_str());
        if (a.dll == nullptr) a.dll = LoadLibraryW(L"shaderc_shared.dll");
        if (a.dll == nullptr) {
            a.failure = "no se encontro shaderc_shared.dll junto al ejecutable (viene con el Vulkan SDK)";
            return;
        }
        const bool ok = bind(a.dll, "shaderc_compiler_initialize", a.compiler_initialize) &&
                        bind(a.dll, "shaderc_compile_options_initialize", a.options_initialize) &&
                        bind(a.dll, "shaderc_compile_options_release", a.options_release) &&
                        bind(a.dll, "shaderc_compile_options_set_optimization_level", a.options_set_optimization_level) &&
                        bind(a.dll, "shaderc_compile_options_set_target_env", a.options_set_target_env) &&
                        bind(a.dll, "shaderc_compile_into_spv", a.compile_into_spv) &&
                        bind(a.dll, "shaderc_result_get_compilation_status", a.result_get_compilation_status) &&
                        bind(a.dll, "shaderc_result_get_length", a.result_get_length) &&
                        bind(a.dll, "shaderc_result_get_bytes", a.result_get_bytes) &&
                        bind(a.dll, "shaderc_result_get_error_message", a.result_get_error_message) &&
                        bind(a.dll, "shaderc_result_release", a.result_release);
        if (!ok) {
            a.failure = "shaderc_shared.dll no tiene las funciones esperadas (version distinta)";
            return;
        }
        a.compiler = a.compiler_initialize();
        if (a.compiler == nullptr) a.failure = "shaderc no pudo iniciarse";
    });
    return a;
}

std::mutex& compileMutex() {
    static std::mutex m;
    return m;
}

}  // namespace

bool compilerAvailable() { return api().compiler != nullptr; }

bool compile(const std::string& source, Stage stage, const std::string& name, std::vector<std::uint32_t>& spirv,
             std::string& error) {
    Api& a = api();
    if (a.compiler == nullptr) {
        error = a.failure;
        return false;
    }
    std::lock_guard<std::mutex> lock(compileMutex());
    Options options = a.options_initialize();
    a.options_set_optimization_level(options, kOptimizePerformance);
    a.options_set_target_env(options, kTargetEnvVulkan, kVulkan13);
    Result result = a.compile_into_spv(a.compiler, source.data(), source.size(),
                                       stage == Stage::Vertex ? kVertexShader : kFragmentShader, name.c_str(), "main",
                                       options);
    a.options_release(options);
    if (result == nullptr) {
        error = "shaderc no devolvio resultado";
        return false;
    }
    const bool ok = a.result_get_compilation_status(result) == kStatusSuccess;
    if (ok) {
        const size_t bytes = a.result_get_length(result);
        spirv.resize(bytes / 4);
        std::memcpy(spirv.data(), a.result_get_bytes(result), spirv.size() * 4);
        error.clear();
    } else {
        const char* message = a.result_get_error_message(result);
        error = message != nullptr ? message : "error desconocido";
    }
    a.result_release(result);
    return ok;
}

}  // namespace cramion::gfx::shaders

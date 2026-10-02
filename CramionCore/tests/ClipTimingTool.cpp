// Herramienta (no es una prueba): cuanto tarda cargar y convertir un .cranim
// sobre un modelo. ClipTimingTool <modelo.crdata> <clip.cranim>
#include "CramionCore/asset/AssetManager.h"
#include "CramionCore/ecs/AnimatorController.h"
#include "CramionCore/profiling/Profiler.h"

#include <chrono>
#include <cstdio>
#include <string>

using namespace cramion;

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    const auto t0 = std::chrono::steady_clock::now();
    auto content = assets::AssetManager::readModel(Uuid::generate(), argv[1], "modelo");
    const auto t1 = std::chrono::steady_clock::now();
    if (!content || content->parts.empty()) { std::printf("no se pudo leer el modelo\n"); return 1; }
    const asset::ModelData& model = *content->parts.front();
    std::printf("modelo: %zu nodos, %.0f ms\n", model.nodes.size(), std::chrono::duration<double, std::milli>(t1 - t0).count());
    for (int i = 2; i < argc; ++i) {
        const auto a = std::chrono::steady_clock::now();
        asset::AnimationClip clip;
        std::string error;
        const bool ok = ecs::loadAnimationClip(argv[i], model, clip, &error);
        const auto b = std::chrono::steady_clock::now();
        std::printf("%s: %s %.0f ms, %zu canales %s\n", argv[i], ok ? "ok" : "FALLO", std::chrono::duration<double, std::milli>(b - a).count(),
                    clip.channels.size(), error.c_str());
    }
    return 0;
}

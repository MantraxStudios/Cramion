#include "CramionCore/platform/MemoryPressure.h"

#include "CramionCore/cvar/CVar.h"
#include "CramionCore/profiling/Profiler.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#else
#include <unistd.h>
#if defined(__ANDROID__)
#include <malloc.h>
#endif
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <mutex>

namespace cramion::memory {

namespace {

cvar::CVar<int> g_budget_mb("memory.AssetRamBudgetMB", 0,
                            "Presupuesto de RAM de los modelos en memoria (MB): por encima se sueltan los que nadie "
                            "usa, del usado hace mas tiempo al mas reciente; 0 = sin limite (como antes)",
                            cvar::Saved, 0, 1 << 20);
cvar::CVar<bool> g_budget_auto("memory.AssetRamBudgetAuto", false,
                               "Presupuesto de RAM de los modelos segun la RAM del aparato (gama baja, media, alta); "
                               "manda sobre memory.AssetRamBudgetMB",
                               cvar::Saved);
cvar::CVar<bool> g_trim_on_pressure("memory.TrimOnPressure", true,
                                    "Atender los avisos de memoria baja del sistema (Android onTrimMemory/onLowMemory) "
                                    "soltando los modelos que nadie usa",
                                    cvar::Saved);

std::atomic<int> g_pending{0};
std::mutex g_stats_mutex;
assets::AssetRamStats g_last{};
std::atomic<std::uint64_t> g_last_rss{0};

std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

double toMb(std::uint64_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }

}  // namespace

void notifyTrimMemory(int level) {
    int current = g_pending.load();
    while (level > current && !g_pending.compare_exchange_weak(current, level)) {
    }
}

void notifyLowMemory() { notifyTrimMemory(kLowMemory); }

int takePendingTrim() { return g_pending.exchange(0); }

std::optional<assets::TrimLevel> trimLevelFor(int level) {
    if (level >= kTrimComplete || level == kTrimRunningCritical) return assets::TrimLevel::Critical;
    if (level >= kTrimBackground || level == kTrimRunningLow) return assets::TrimLevel::Low;
    if (level >= kTrimRunningModerate) return assets::TrimLevel::Moderate;  // 5 y UI_HIDDEN (20)
    return std::nullopt;
}

std::uint64_t processResidentBytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) return counters.WorkingSetSize;
    return 0;
#else
    // /proc/self/statm: paginas totales y residentes.
    std::FILE* f = std::fopen("/proc/self/statm", "r");
    if (f == nullptr) return 0;
    unsigned long long size = 0;
    unsigned long long resident = 0;
    const int read = std::fscanf(f, "%llu %llu", &size, &resident);
    std::fclose(f);
    if (read != 2) return 0;
    return resident * static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
#endif
}

std::uint64_t physicalMemoryBytes() {
#if defined(_WIN32)
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) return status.ullTotalPhys;
    return 0;
#else
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long page = sysconf(_SC_PAGESIZE);
    if (pages <= 0 || page <= 0) return 0;
    return static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(page);
#endif
}

std::uint64_t autoAssetRamBudget(std::uint64_t physical_bytes) {
    // Por gama, como en los moviles: el sistema reserva parte y el render
    // (VRAM compartida en movil) se lleva otra. Lo que no se sabe, el de 4 GB.
    constexpr std::uint64_t kGb = 1ull << 30;
    constexpr std::uint64_t kMb = 1ull << 20;
    if (physical_bytes == 0) return 384 * kMb;
    if (physical_bytes <= 3 * kGb + kGb / 2) return 256 * kMb;   // 2-3 GB: gama baja
    if (physical_bytes <= 4 * kGb + kGb / 2) return 384 * kMb;   // 4 GB
    if (physical_bytes <= 6 * kGb + kGb / 2) return 512 * kMb;   // 6 GB
    if (physical_bytes <= 8 * kGb + kGb / 2) return 768 * kMb;   // 8 GB
    return 1024 * kMb;                                          // 12 GB o mas (y PC)
}

std::uint64_t configuredAssetRamBudget() {
    if (g_budget_auto.get()) return autoAssetRamBudget(physicalMemoryBytes());
    const int mb = g_budget_mb.get();
    return mb > 0 ? static_cast<std::uint64_t>(mb) << 20 : 0;
}

std::uint64_t tickAssetRam(assets::AssetManager& manager, bool force) {
    static std::uint64_t last_check = 0;
    std::uint64_t freed = 0;
    // Avisos del sistema: en cuanto llegan.
    if (const int pending = takePendingTrim(); pending > 0) {
        const std::optional<assets::TrimLevel> level = trimLevelFor(pending);
        if (level && g_trim_on_pressure.get()) {
            const std::uint64_t released = manager.trim(*level);
            freed += released;
#if defined(__ANDROID__) && defined(M_PURGE)
            // Que el asignador devuelva al sistema lo que se acaba de soltar.
            if (*level != assets::TrimLevel::Moderate) mallopt(M_PURGE, 0);
#endif
            std::cout << "[Memoria] Aviso del sistema (nivel " << pending << "): soltados " << (released >> 20)
                      << " MB de modelos sin usar; RAM del proceso " << (processResidentBytes() >> 20) << " MB\n";
        }
        force = true;
    }
    // Medir recorre todos los modelos: una vez por segundo (las cifras del
    // perfilador, las de la ultima medida en cada frame).
    const std::uint64_t now = nowMs();
    if (force || now - last_check >= 1000) {
        last_check = now;
        manager.setRamBudget(configuredAssetRamBudget());
        freed += manager.enforceRamBudget();
        const assets::AssetRamStats stats = manager.ramStats();
        {
            const std::lock_guard<std::mutex> lock(g_stats_mutex);
            g_last = stats;
        }
        g_last_rss = processResidentBytes();
    }
    prof::counter("RAM modelos (MB)", toMb(lastAssetRamStats().bytes));
    prof::counter("RAM proceso (MB)", toMb(g_last_rss.load()));
    return freed;
}

assets::AssetRamStats lastAssetRamStats() {
    const std::lock_guard<std::mutex> lock(g_stats_mutex);
    return g_last;
}

std::uint64_t lastProcessResidentBytes() { return g_last_rss.load(); }

}  // namespace cramion::memory

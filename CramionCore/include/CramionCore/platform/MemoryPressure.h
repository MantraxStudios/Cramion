#ifndef CRAMION_CORE_PLATFORM_MEMORY_PRESSURE_H
#define CRAMION_CORE_PLATFORM_MEMORY_PRESSURE_H

// RAM del juego: presupuesto de los modelos en memoria (AssetManager, LRU) y
// avisos de memoria baja del sistema (Android: onTrimMemory/onLowMemory).
//
// CVars (apagado por defecto: sin presupuesto, nada se suelta salvo un aviso
// del sistema, que en escritorio no llega nunca):
//   memory.AssetRamBudgetMB   presupuesto de los modelos en MB (0 = sin limite)
//   memory.AssetRamBudgetAuto presupuesto segun la RAM del aparato (gama)
//   memory.TrimOnPressure     atender los avisos de memoria baja (encendido)
//
// tickAssetRam() va una vez por frame en el hilo principal (el player): pone
// el presupuesto, suelta lo que sobre, atiende los avisos pendientes y deja
// las cifras en el perfilador ("RAM modelos (MB)", "RAM proceso (MB)") y en
// lastAssetRamStats() (la linea de [Rendimiento]).

#include "CramionCore/asset/AssetManager.h"

#include <cstdint>
#include <optional>

namespace cramion::memory {

// Niveles de ComponentCallbacks2.onTrimMemory (Android).
inline constexpr int kTrimRunningModerate = 5;
inline constexpr int kTrimRunningLow = 10;
inline constexpr int kTrimRunningCritical = 15;
inline constexpr int kTrimUiHidden = 20;
inline constexpr int kTrimBackground = 40;
inline constexpr int kTrimModerate = 60;
inline constexpr int kTrimComplete = 80;
// onLowMemory (no es un nivel de Android: por encima de todos).
inline constexpr int kLowMemory = 100;

// Avisos del sistema (cualquier hilo: el de Java, el de la glue). Se guarda
// el mayor hasta que el hilo principal lo recoge.
void notifyTrimMemory(int level);
void notifyLowMemory();
// El mayor aviso desde la ultima llamada (0 = ninguno).
int takePendingTrim();
// Que se suelta con cada nivel (nullopt: nada).
std::optional<assets::TrimLevel> trimLevelFor(int level);

// RAM del proceso (residente) y del aparato. 0 si no se sabe.
std::uint64_t processResidentBytes();
std::uint64_t physicalMemoryBytes();
// Presupuesto de los modelos segun la RAM del aparato (memory.AssetRamBudgetAuto).
std::uint64_t autoAssetRamBudget(std::uint64_t physical_bytes);
// El presupuesto que piden las CVars (0 = sin limite).
std::uint64_t configuredAssetRamBudget();

// Una vez por frame (hilo principal). `force`: medir ya (sin esperar al
// segundo). Devuelve los bytes soltados en esta llamada.
std::uint64_t tickAssetRam(assets::AssetManager& manager, bool force = false);
// Lo ultimo medido por tickAssetRam.
assets::AssetRamStats lastAssetRamStats();
std::uint64_t lastProcessResidentBytes();

}  // namespace cramion::memory

#endif  // CRAMION_CORE_PLATFORM_MEMORY_PRESSURE_H

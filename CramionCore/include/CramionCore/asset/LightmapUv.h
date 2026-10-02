#ifndef CRAMION_CORE_ASSET_LIGHTMAP_UV_H
#define CRAMION_CORE_ASSET_LIGHTMAP_UV_H

// UV de lightmap (UV2) generadas por el motor, como el "Generate Lightmap UVs"
// de Unity. Implementacion: CramionCore/src/asset/LightmapUv.cpp (target
// CramionAssets: la usa AssetManager::readModel).
//
// La malla se parte en cartas (zonas casi planas y conectadas), cada carta se
// proyecta en su plano, se gira para ocupar la menor caja posible y todas se
// empaquetan en el cuadrado [0, 1]^2 sin solaparse y con un hueco entre ellas.
// Un vertice que esta en el borde de dos cartas necesita dos UV distintas: se
// duplica (remap = de que vertice original sale cada vertice nuevo). El
// resultado se guarda junto a la iluminacion horneada de la escena y se aplica
// al cargar el modelo: el lightmap horneado y el que se dibuja usan siempre la
// MISMA disposicion.

#include "CramionCore/Uuid.h"

#include <CramionFX/asset/Model.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace cramion::lighting {

struct LightmapUvSettings {
    // Caras que se doblan mas que esto respecto a su vecina van en cartas
    // distintas (grados).
    float hard_angle = 66.0f;
    // Hueco entre cartas en el espacio UV (0..1). Con el lightmap de un objeto
    // de R texeles de lado, son padding * R texeles: el horneado lo calcula
    // con la resolucion del objeto mas pequeno que usa la malla.
    float padding = 2.5f / 64.0f;
};

// Disposicion de las UV de lightmap de una malla (una pieza de un modelo).
struct LightmapUvLayout {
    // Para saber si corresponde a la malla que se carga.
    std::uint32_t source_vertices = 0;
    std::uint32_t source_indices = 0;
    std::uint64_t source_hash = 0;

    // remap[i] = vertice original del vertice nuevo i.
    std::vector<std::uint32_t> remap;
    // Indices nuevos: mismos triangulos, en el mismo orden (las submallas no
    // cambian), con los vertices partidos.
    std::vector<std::uint32_t> indices;
    // UV2 de cada vertice nuevo, en [0, 1].
    std::vector<core::Vec2> uvs;

    std::uint32_t charts = 0;
    // Parte del cuadrado que ocupan las cartas (0..1).
    float uv_area = 0.0f;
    // Superficie total de la malla (unidades del modelo al cuadrado).
    float surface_area = 0.0f;
};

// Huella de la geometria (posiciones e indices) para invalidar disposiciones viejas.
std::uint64_t lightmapMeshHash(const asset::ModelData& model);

// Superficie de la malla en unidades del modelo (al cuadrado).
float meshSurfaceArea(const asset::ModelData& model);

// Genera la disposicion. false si la malla esta vacia.
bool generateLightmapUvs(const asset::ModelData& model, LightmapUvLayout& out,
                         const LightmapUvSettings& settings = {});

// Parte los vertices, cambia los indices (tambien los de los LODs si ya
// existen) y rellena ModelData::lightmap_uvs. false (sin tocar nada) si la
// disposicion no es de esta malla.
bool applyLightmapUvs(asset::ModelData& model, const LightmapUvLayout& layout);

// Archivo .cruv (binario).
bool saveLightmapUvLayout(const std::filesystem::path& file, const LightmapUvLayout& layout);
bool loadLightmapUvLayout(const std::filesystem::path& file, LightmapUvLayout& out);

// --- Registro ---
// Que piezas de que modelos llevan UV de lightmap en la escena abierta (lo
// rellena la iluminacion horneada al abrir la escena o al hornear).
// AssetManager::readModel lo consulta al leer cada pieza. Seguro entre hilos.
void registerLightmapUvs(const Uuid& model, int part, std::shared_ptr<const LightmapUvLayout> layout);
void clearLightmapUvRegistry();
std::shared_ptr<const LightmapUvLayout> registeredLightmapUvs(const Uuid& model, int part);
// Aplica la disposicion registrada (si la hay). Devuelve si se aplico.
bool applyRegisteredLightmapUvs(const Uuid& model, int part, asset::ModelData& data);

}  // namespace cramion::lighting

#endif  // CRAMION_CORE_ASSET_LIGHTMAP_UV_H

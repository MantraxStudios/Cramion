#ifndef CRAMION_CORE_VOXEL_FAR_TERRAIN_H
#define CRAMION_CORE_VOXEL_FAR_TERRAIN_H

// Terreno lejano de los mundos de bloques (estilo "Distant Horizons"): mas
// alla de los chunks de verdad (VoxelWorld::render_distance) se ve el mundo a
// varios kilometros con mallas simplificadas.
//
//   - Teselas en un arbol cuaternario alrededor de quien mira: nivel L =
//     16 x 16 columnas separadas 2^L bloques (cada tesela cubre 16 * 2^L m).
//     Cuanto mas lejos, mas grandes y mas simples (anillos 2x, 4x, 8x...).
//   - Cada columna sale directamente del generador (Generator::farColumn):
//     altura, bloque de arriba, agua y copas de arboles; sin generar chunks.
//   - Malla de columnas coloreadas (cara de arriba + taludes con los vecinos +
//     faldon en los bordes para que no se vean grietas entre niveles), en el
//     formato de vertice "lejano" del VoxelPass (VoxelPass::setFarSection):
//     color por vertice (la media de la textura del bloque), sin texturas.
//   - Todo en los hilos del sistema de jobs (prioridad Background), de cerca
//     a lejos y con un limite en vuelo; el hilo principal solo decide que
//     teselas hacen falta (cuando quien mira se mueve) y pasa las mallas al
//     renderizador con un presupuesto de bytes por frame.
//   - Cache en disco de las columnas por tesela (%LOCALAPPDATA%/Cramion/
//     FarTerrain/<generador, semilla, vegetacion>): la segunda vez carga en
//     milisegundos. Solo depende del generador (no de lo que cambia el juego).
//   - Las teselas viejas no se quitan hasta que estan listas las nuevas que
//     cubren su zona: sin agujeros al moverse.
//   - El VoxelPass descarta lo lejano dentro del circulo de chunks ya
//     dibujados (setFarHole) y lo empuja un poco hacia atras en profundidad:
//     los chunks de verdad siempre ganan; donde aun no han cargado se ve el
//     terreno lejano (sin agujeros al entrar al mundo o al teletransportarse).
//
// CVars: voxel.far.Distance (m, 0 = apagado), voxel.far.Detail, voxel.far.DiskCache, voxel.far.Sea.

#include "VoxelInternal.h"

#include <CramionFX/core/Math.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace cramion::gfx {
struct VoxelTextureLayer;
}

namespace cramion::voxel {

// Color de cada bloque visto de lejos (sRGB 0..255, la media de su textura
// sin los huecos) y rugosidad 0..15.
struct FarPalette {
    struct Entry {
        std::uint8_t r = 200, g = 200, b = 200, roughness = 13;
    };
    std::array<Entry, block::Count> top{};
    std::array<Entry, block::Count> side{};
};
// A partir de las texturas de los bloques (en un hilo de fondo: recorre todas).
std::shared_ptr<FarPalette> buildFarPalette(const std::vector<gfx::VoxelTextureLayer>& layers);

class FarTerrain {
public:
    struct Settings {
        float distance = 0.0f;  // m (0 = apagado)
        float detail = 1.0f;    // 1 = normal; 2 = el doble de columnas a la misma distancia
        bool disk_cache = true;
        bool sea = true;        // dibujar la superficie del mar (si no, el fondo)
    };
    struct Update {
        std::uint64_t key = 0;
        core::Vec3 origin{};  // absoluto (bloques)
        float scale = 1.0f;   // bloques por columna
        std::vector<std::uint32_t> vertices;  // vacio = quitar
    };
    struct Stats {
        int tiles = 0;     // las que hacen falta
        int ready = 0;     // con malla en el renderizador
        int pending = 0;   // esperando hilo o subida
        std::uint64_t quads = 0;
        float last_tile_ms = 0.0f;
        int from_cache = 0;  // teselas leidas del disco (total)
        int generated = 0;   // teselas generadas (total)
    };

    // Los ajustes de las CVars voxel.far.* (se pueden leer desde cualquier hilo).
    static Settings currentSettings();

    FarTerrain();
    ~FarTerrain();
    FarTerrain(const FarTerrain&) = delete;
    FarTerrain& operator=(const FarTerrain&) = delete;

    // Otro mundo, dimension o renderizador vaciado: olvida todo (sin mandar
    // quitas: el renderizador ya no las tiene). Nulo = apagado.
    void reset(std::shared_ptr<const Generator> generator);
    const Generator* generator() const;
    void setPalette(std::shared_ptr<const FarPalette> palette);
    bool hasPalette() const;
    // Cada frame (hilo principal). `viewer` absoluto; `inner_radius`: radio
    // (bloques) de los chunks de verdad: ahi dentro solo teselas gruesas
    // (relleno mientras cargan).
    void update(const core::Vec3& viewer, float inner_radius, const Settings& settings);
    // Lo que hay que mandar al renderizador: las quitas y las mallas nuevas
    // hasta `byte_budget` bytes (el resto en los frames siguientes).
    void takeUpdates(std::vector<Update>& out, std::size_t byte_budget);
    Stats stats() const;

    // --- Piezas sueltas (pruebas) ---
    static constexpr int kSamples = 16;               // columnas por lado de una tesela
    static constexpr int kBorder = kSamples + 2;      // con el borde de 1
    // Columnas de una tesela (con borde): (x + 1) + (z + 1) * kBorder.
    static void sampleTile(const Generator& generator, int level, int tx, int tz, std::vector<Generator::FarColumn>& out);
    // Malla de una tesela (formato lejano del VoxelPass).
    static void meshTile(const std::vector<Generator::FarColumn>& columns, int level, const FarPalette& palette, bool sea,
                         std::vector<std::uint32_t>& out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cramion::voxel

#endif  // CRAMION_CORE_VOXEL_FAR_TERRAIN_H

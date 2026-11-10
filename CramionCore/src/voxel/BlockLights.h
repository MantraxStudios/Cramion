#ifndef CRAMION_CORE_VOXEL_BLOCK_LIGHTS_H
#define CRAMION_CORE_VOXEL_BLOCK_LIGHTS_H

// Luces reales de los bloques que dan luz (VoxelSystem::appendBlockLights).
//
// La luz de antorchas de los bloques (0..15, propagada por celdas) solo llega
// a las mallas de voxeles (voxel.frag la pinta como emision). Los personajes,
// criaturas y objetos (actores normales) no la leen: de noche solo les llega
// la luna y la GI y se veian negros junto a una farola. Aqui se buscan los
// bloques que dan luz cerca de la camara y se convierten en luces puntuales
// del motor (sin sombra; por clusteres), que iluminan a todo.
//
// Cache por seccion (16^3) de cada columna: los bloques que dan luz (los
// cercanos del mismo tipo juntos). Solo se rehace al cambiar una luz en la
// columna (setBlock con un bloque que da luz, antes o despues) o al cargarse
// otra vez; las secciones nuevas se exploran con un presupuesto por frame,
// las mas cercanas primero. CVars voxel.lights.*.

#include "VoxelInternal.h"

#include <CramionFX/scene/Light.h>

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace cramion::voxel {

class BlockLightCache {
public:
    // Lo que la cache necesita de una columna cargada.
    struct Column {
        const ChunkData* data = nullptr;
        std::uint64_t serial = 0;  // cambia al cargar la columna otra vez
        std::uint32_t edits = 0;   // sube al poner o quitar un bloque que da luz
        int top = kWorldHeight - 1;
    };
    // false si la columna (cx, cz) no esta cargada.
    using ColumnLookup = std::function<bool(int cx, int cz, Column& out)>;

    // Anade a `out` las luces cercanas a `eye_abs` (coordenadas absolutas de
    // bloque); `origin` = desplazamiento del origen flotante (local = absoluto
    // - origin). `seconds`: reloj del titileo.
    void collect(const core::Vec3& eye_abs, const core::Vec3& origin, const ColumnLookup& lookup, float seconds,
                 std::vector<scene::PointLight>& out);
    void clear() { entries_.clear(); }

private:
    struct Source {
        float x = 0.0f, y = 0.0f, z = 0.0f;  // centro (absoluto, ya con el desplazamiento del tipo)
        std::uint8_t level = 0;
        BlockId id = 0;
        std::uint8_t count = 1;  // bloques juntados en esta luz
    };
    struct Entry {
        std::uint64_t serial = 0;
        std::uint32_t edits = 0;
        std::uint32_t scanned = 0;  // bit por seccion ya explorada
        std::uint32_t last_frame = 0;
        std::array<std::vector<Source>, kSectionCount> sections;
    };
    void scanSection(const ChunkData& data, int sy, std::vector<Source>& out) const;

    std::unordered_map<std::int64_t, Entry> entries_;
    std::uint32_t frame_ = 0;
    // Reutilizados entre frames (sin reservar memoria).
    struct Candidate {
        float score = 0.0f;
        float distance = 0.0f;
        const Source* source = nullptr;
    };
    std::vector<Candidate> candidates_;
    std::vector<std::pair<float, std::int64_t>> columns_;
};

}  // namespace cramion::voxel

#endif  // CRAMION_CORE_VOXEL_BLOCK_LIGHTS_H

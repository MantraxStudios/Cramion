// Rieles y senales de arcanita (bloques 211..230 de Voxel.h).
//
// VIAS. No hay datos por celda: la forma de cada via sale de sus vecinas (como
// las vallas o las puertas), y la usan igual el Mesher (dibujo), VoxelSystem::
// railAt (el juego: vagonetas) y las pruebas:
//   - Se une a las vias de los 4 lados: a la misma altura, un bloque mas arriba
//     (entonces es una RAMPA que sube hacia alli) o un bloque mas abajo (la de
//     abajo es la rampa que sube hasta esta).
//   - Una rampa gana a todo (la primera direccion por orden +X, -X, +Z, -Z).
//   - Sin vecinas: recta en X. Con una: recta hacia ella. Con dos: recta o curva.
//     Con tres: un cruce en T (la recta y un ramal). Con cuatro: cruce en cruz.
//   - El tope (BufferStop) solo se une a una (la primera).
//   Al pasar por una T (railExit): por la recta se sigue recto y desde el ramal
//   se gira en sentido horario; una via de CAMBIO con senal desvia: desde la
//   recta entra en el ramal y desde el ramal gira al otro lado.
//
// SENALES (SignalSim, en VoxelSystem a 10 Hz, solo donde algo cambio):
//   - Fuentes (15): palanca encendida, boton pulsado, placa de presion o via
//     detectora con algo encima (setSignalInput, lo dice el juego).
//   - El polvo de arcanita (traza) lleva la senal perdiendo 1 por bloque (hasta
//     15): se une a los nodos de los 4 lados, sube y baja escalones de traza y
//     alimenta el nodo de debajo (una lampara bajo la traza).
//   - Runa nula (inversor) y repetidor de eco (retardo de 0.3 s, senal a 15):
//     leen del lado mas cercano a una fuente (distancia en la red, solo con la
//     forma: no cambia al encenderse) y mandan a los demas. Asi no hay bucles.
//   - Consumidores: lamparas, vias de impulso, freno y cambio. Las de impulso y
//     freno pasan la senal a las suyas seguidas (hasta 8).
//   - Lo encendido se ve cambiando el bloque por su version "Lit"/"Dim" (solo
//     en local; cada cliente calcula lo mismo con los mismos bloques y entradas).
#pragma once

#include "CramionCore/voxel/Voxel.h"

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cramion::voxel::rail {

// Direcciones horizontales: 0 +X, 1 -X, 2 +Z, 3 -Z (las caras 0, 1, 4, 5 de kFaces).
inline constexpr int kDx[4] = {1, -1, 0, 0};
inline constexpr int kDz[4] = {0, 0, 1, -1};
inline constexpr int opposite(int d) { return d ^ 1; }
// Giro horario visto desde arriba: +X -> +Z -> -X -> -Z.
inline constexpr int clockwise(int d) {
    constexpr int t[4] = {2, 3, 1, 0};
    return t[d & 3];
}
inline constexpr int counterClockwise(int d) {
    constexpr int t[4] = {3, 2, 0, 1};
    return t[d & 3];
}
inline int firstBit(int mask) {
    for (int d = 0; d < 4; ++d)
        if ((mask & (1 << d)) != 0) return d;
    return -1;
}
inline int bitCount(int mask) {
    int n = 0;
    for (int d = 0; d < 4; ++d) n += (mask >> d) & 1;
    return n;
}

struct RailShape {
    int mask = 0;  // conexiones (bit d)
    int up = -1;   // rampa: sube hacia d (-1 llana)
};

template <class BlockAt>
RailShape railShape(const BlockAt& at, int x, int y, int z) {
    RailShape r;
    const BlockId self = at(x, y, z);
    if (!block::isRail(self)) return r;
    int flat = 0, up = 0, down = 0;
    for (int d = 0; d < 4; ++d) {
        const int nx = x + kDx[d], nz = z + kDz[d];
        if (block::isRail(at(nx, y, nz))) flat |= 1 << d;
        else if (block::isRail(at(nx, y + 1, nz))) up |= 1 << d;
        else if (block::isRail(at(nx, y - 1, nz))) down |= 1 << d;
    }
    if (self == block::BufferStop) {
        const int d = firstBit(flat | down);
        r.mask = 1 << (d < 0 ? 0 : d);
        return r;
    }
    if (up != 0) {
        const int d = firstBit(up);
        r.up = d;
        r.mask = (1 << d) | (1 << opposite(d));
        return r;
    }
    const int c = flat | down;
    const int n = bitCount(c);
    if (n == 0) r.mask = 3;
    else if (n == 1) r.mask = c | (1 << opposite(firstBit(c)));
    else r.mask = c;
    return r;
}

// Por donde sale una vagoneta que entra por el lado `from` (la direccion, desde
// esta celda, hacia la que venia). powered: senal en una via de cambio.
inline int railExit(BlockId id, int mask, int from, bool powered) {
    const int n = bitCount(mask);
    const bool in = (mask & (1 << from)) != 0;
    if (n <= 1) return opposite(from);  // tope o suelta: sigue (el juego la para)
    if (n == 2) {
        if (!in) return (mask & (1 << opposite(from))) != 0 ? opposite(from) : firstBit(mask);
        return firstBit(mask & ~(1 << from));
    }
    if (n == 4) return opposite(from);
    // T: la recta es el par opuesto, el ramal el que sobra.
    int stem = -1;
    for (int d = 0; d < 4; ++d)
        if ((mask & (1 << d)) != 0 && (mask & (1 << opposite(d))) == 0) stem = d;
    const bool divert = id == block::SwitchRail && powered;
    if (from == stem) {
        const int a = divert ? counterClockwise(stem) : clockwise(stem);
        return (mask & (1 << a)) != 0 ? a : opposite(a);
    }
    if (!in) return opposite(from);
    return divert ? stem : opposite(from);
}

// --- Polvo de arcanita ---
inline constexpr bool isTrace(BlockId id) { return id == block::ArcaniteTrace || id == block::ArcaniteTraceLit; }

struct TraceShape {
    int flat = 0;  // nodo de senal al lado (misma altura)
    int up = 0;    // traza un bloque mas arriba a ese lado (sube por la pared)
    int down = 0;  // traza un bloque mas abajo a ese lado
};

template <class BlockAt>
TraceShape traceShape(const BlockAt& at, int x, int y, int z) {
    TraceShape t;
    const bool open_above = !blockDef(at(x, y + 1, z)).opaque;
    for (int d = 0; d < 4; ++d) {
        const int nx = x + kDx[d], nz = z + kDz[d];
        const BlockId n = at(nx, y, nz);
        if (block::isSignal(n)) {
            t.flat |= 1 << d;
            continue;
        }
        if (open_above && isTrace(at(nx, y + 1, nz))) t.up |= 1 << d;
        else if (!blockDef(n).opaque && isTrace(at(nx, y - 1, nz))) t.down |= 1 << d;
    }
    return t;
}

// --- Simulacion de senales ---

enum class NodeKind : std::uint8_t { None, Trace, Source, Processor, Sink };
NodeKind nodeKind(BlockId id);
// La version encendida / apagada de un bloque de senal (el mismo si no tiene).
BlockId litVariant(BlockId id, bool on);
// Bloque "logico" (sin el encendido): el apagado de cada pareja.
BlockId baseVariant(BlockId id);

class SignalSim {
public:
    // Acceso al mundo (coordenadas absolutas). set cambia el bloque sin avisar a blockChanged.
    struct World {
        std::function<BlockId(int, int, int)> get;
        std::function<void(int, int, int, BlockId)> set;
    };

    static constexpr float kTick = 0.1f;   // 10 Hz
    static constexpr int kRelayDelay = 3;  // ticks de retardo del repetidor
    static constexpr float kInputLife = 0.35f;
    static constexpr int kMaxCellsPerTick = 8192;
    static constexpr int kMaxCircuit = 4096;

    // Algo cambio en la celda (antes / despues): se revisa su circuito en el siguiente tick.
    void blockChanged(const World& w, int x, int y, int z, BlockId before, BlockId after);
    // Columna cargada: revisa sus bloques de senal (blocks en el orden de cellIndex).
    void chunkLoaded(int cx, int cz, const std::vector<BlockId>& blocks);
    void setInput(int x, int y, int z, bool on);
    int power(int x, int y, int z) const;
    // Avanza; true si cambio algun bloque.
    bool update(float dt, const World& w);
    // Evalua ya lo pendiente (pruebas): `ticks` pasos.
    void step(const World& w, int ticks = 1);
    void clear();
    std::size_t pending() const { return seeds_.size(); }

private:
    struct Cell {
        int x, y, z;
    };
    struct Proc {
        bool out = false;
        std::uint8_t hist = 0;
    };
    struct Input {
        Cell c;
        float expires = 0;
    };
    static std::int64_t key(int x, int y, int z) {
        return (static_cast<std::int64_t>(x & 0x3FFFFF) << 42) | (static_cast<std::int64_t>(z & 0x3FFFFF) << 20) |
               static_cast<std::int64_t>(y & 0xFFFFF);
    }
    void seed(int x, int y, int z);
    void tick(const World& w);
    bool evaluate(const World& w, const Cell& start, std::unordered_set<std::int64_t>& done, int& budget);

    float time_ = 0, accum_ = 0;
    std::vector<Cell> seeds_;
    std::unordered_set<std::int64_t> seed_keys_;
    std::unordered_map<std::int64_t, std::uint8_t> power_;
    std::unordered_map<std::int64_t, Proc> procs_;
    std::unordered_map<std::int64_t, Input> inputs_;
    bool changed_ = false;
};

}  // namespace cramion::voxel::rail

// Simulacion de senales de arcanita (ver Rails.h): circuitos evaluados por
// sucesos (solo los que tocan una celda que cambio, una entrada o un retardo
// en curso), a 10 Hz, deterministas (solo dependen de los bloques, de las
// entradas y del estado de las runas y repetidores).

#include "Rails.h"

#include <algorithm>
#include <array>
#include <deque>

namespace cramion::voxel::rail {

NodeKind nodeKind(BlockId id) {
    switch (id) {
        case block::ArcaniteTrace:
        case block::ArcaniteTraceLit: return NodeKind::Trace;
        case block::RuneLever:
        case block::RuneLeverOn:
        case block::RuneButton:
        case block::RuneButtonOn:
        case block::TriggerPlate:
        case block::SensorRail: return NodeKind::Source;
        case block::NullRune:
        case block::NullRuneDim:
        case block::EchoRelay:
        case block::EchoRelayLit: return NodeKind::Processor;
        case block::SignalLamp:
        case block::SignalLampLit:
        case block::SurgeRail:
        case block::DragRail:
        case block::SwitchRail: return NodeKind::Sink;
        default: return NodeKind::None;
    }
}

BlockId litVariant(BlockId id, bool on) {
    switch (id) {
        case block::ArcaniteTrace:
        case block::ArcaniteTraceLit: return on ? block::ArcaniteTraceLit : block::ArcaniteTrace;
        case block::SignalLamp:
        case block::SignalLampLit: return on ? block::SignalLampLit : block::SignalLamp;
        case block::NullRune:
        case block::NullRuneDim: return on ? block::NullRune : block::NullRuneDim;
        case block::EchoRelay:
        case block::EchoRelayLit: return on ? block::EchoRelayLit : block::EchoRelay;
        default: return id;
    }
}

BlockId baseVariant(BlockId id) {
    switch (id) {
        case block::ArcaniteTraceLit: return block::ArcaniteTrace;
        case block::SignalLampLit: return block::SignalLamp;
        case block::NullRuneDim: return block::NullRune;
        case block::EchoRelayLit: return block::EchoRelay;
        case block::RuneLeverOn: return block::RuneLever;
        case block::RuneButtonOn: return block::RuneButton;
        default: return id;
    }
}

namespace {

struct P3 {
    int x, y, z;
};

// Vecinos enlazados de un nodo (ver Rails.h): la traza con sus conexiones y el
// nodo de debajo; el resto, con los 6 vecinos (una traza solo de lado o encima)
// y las vias de impulso / freno tambien con las suyas en diagonal (rampas).
template <class Get>
void links(const Get& get, const P3& c, BlockId id, std::vector<P3>& out) {
    out.clear();
    const NodeKind kind = nodeKind(id);
    if (kind == NodeKind::None) return;
    if (kind == NodeKind::Trace) {
        const TraceShape t = traceShape(get, c.x, c.y, c.z);
        for (int d = 0; d < 4; ++d) {
            const int nx = c.x + kDx[d], nz = c.z + kDz[d];
            if ((t.flat & (1 << d)) != 0) out.push_back({nx, c.y, nz});
            if ((t.up & (1 << d)) != 0) out.push_back({nx, c.y + 1, nz});
            if ((t.down & (1 << d)) != 0) out.push_back({nx, c.y - 1, nz});
        }
        const NodeKind below = nodeKind(get(c.x, c.y - 1, c.z));
        if (below != NodeKind::None && below != NodeKind::Trace) out.push_back({c.x, c.y - 1, c.z});
        return;
    }
    static constexpr int kSix[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}, {0, 1, 0}, {0, -1, 0}};
    for (const auto& o : kSix) {
        const P3 n{c.x + o[0], c.y + o[1], c.z + o[2]};
        const NodeKind k = nodeKind(get(n.x, n.y, n.z));
        if (k == NodeKind::None) continue;
        if (k == NodeKind::Trace && o[1] < 0) continue;  // traza debajo: no la alimenta
        if (k == NodeKind::Trace && o[1] > 0) {
            out.push_back(n);  // traza encima: lo alimenta (la traza tiene a este como "nodo de debajo")
            continue;
        }
        out.push_back(n);
    }
    if (id == block::SurgeRail || id == block::DragRail) {
        for (int d = 0; d < 4; ++d) {
            for (const int dy : {1, -1}) {
                const P3 n{c.x + kDx[d], c.y + dy, c.z + kDz[d]};
                if (get(n.x, n.y, n.z) == id) out.push_back(n);
            }
        }
    }
}

bool isSourceOn(BlockId id, bool input) {
    switch (id) {
        case block::RuneLeverOn:
        case block::RuneButtonOn: return true;
        case block::TriggerPlate:
        case block::SensorRail: return input;
        default: return false;
    }
}

}  // namespace

void SignalSim::seed(int x, int y, int z) {
    if (y < 0 || y >= kWorldHeight) return;
    if (seed_keys_.insert(key(x, y, z)).second) seeds_.push_back({x, y, z});
}

void SignalSim::blockChanged(const World& w, int x, int y, int z, BlockId before, BlockId after) {
    bool relevant = block::isSignal(before) || block::isSignal(after);
    // Un bloque normal al lado de una traza cambia por donde sube o baja.
    for (int dy = -1; dy <= 1 && !relevant; ++dy)
        for (int dz = -1; dz <= 1 && !relevant; ++dz)
            for (int dx = -1; dx <= 1 && !relevant; ++dx) relevant = block::isSignal(w.get(x + dx, y + dy, z + dz));
    if (!relevant) return;
    const std::int64_t k = key(x, y, z);
    if (baseVariant(before) != baseVariant(after)) {
        procs_.erase(k);
        if (after != block::TriggerPlate && after != block::SensorRail) inputs_.erase(k);
        if (nodeKind(after) == NodeKind::None) power_.erase(k);
    }
    seed(x, y, z);
    for (int dy = -1; dy <= 1; ++dy)
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) {
                if ((dx | dy | dz) == 0) continue;
                if (block::isSignal(w.get(x + dx, y + dy, z + dz))) seed(x + dx, y + dy, z + dz);
            }
}

void SignalSim::chunkLoaded(int cx, int cz, const std::vector<BlockId>& blocks) {
    // IDS16: el chunk puede venir sin el aire de arriba (VoxelInternal.h ChunkData::trimAir).
    const std::size_t cells = std::min(blocks.size(), static_cast<std::size_t>(kChunkSize) * kChunkSize * kWorldHeight);
    for (std::size_t i = 0; i < cells; ++i) {
        const BlockId id = blocks[i];
        if (!block::isSignal(id)) continue;
        // cellIndex: x + z * 16 + y * 256
        const int lx = static_cast<int>(i & 15), lz = static_cast<int>((i >> 4) & 15), y = static_cast<int>(i >> 8);
        seed(cx * kChunkSize + lx, y, cz * kChunkSize + lz);
    }
}

void SignalSim::setInput(int x, int y, int z, bool on) {
    const std::int64_t k = key(x, y, z);
    if (on) {
        const auto it = inputs_.find(k);
        if (it == inputs_.end()) {
            inputs_.emplace(k, Input{{x, y, z}, time_ + kInputLife});
            seed(x, y, z);
        } else {
            it->second.expires = time_ + kInputLife;
        }
    } else if (inputs_.erase(k) != 0) {
        seed(x, y, z);
    }
}

int SignalSim::power(int x, int y, int z) const {
    const auto it = power_.find(key(x, y, z));
    return it == power_.end() ? 0 : it->second;
}

void SignalSim::clear() {
    seeds_.clear();
    seed_keys_.clear();
    power_.clear();
    procs_.clear();
    inputs_.clear();
    accum_ = 0;
}

bool SignalSim::update(float dt, const World& w) {
    changed_ = false;
    time_ += dt;
    accum_ += dt;
    int steps = 0;
    while (accum_ >= kTick && steps < 3) {
        accum_ -= kTick;
        ++steps;
        tick(w);
    }
    if (accum_ > kTick) accum_ = 0;  // muy atrasado: no se acumula
    return changed_;
}

void SignalSim::step(const World& w, int ticks) {
    for (int i = 0; i < ticks; ++i) {
        time_ += kTick;
        tick(w);
    }
}

void SignalSim::tick(const World& w) {
    // Entradas que nadie refresco: fuera.
    for (auto it = inputs_.begin(); it != inputs_.end();) {
        if (it->second.expires < time_) {
            seed(it->second.c.x, it->second.c.y, it->second.c.z);
            it = inputs_.erase(it);
        } else {
            ++it;
        }
    }
    if (seeds_.empty()) return;
    std::vector<Cell> now;
    now.swap(seeds_);
    seed_keys_.clear();
    // Orden fijo (el resultado no depende del orden, pero el presupuesto si).
    std::sort(now.begin(), now.end(), [](const Cell& a, const Cell& b) {
        return a.y != b.y ? a.y < b.y : (a.z != b.z ? a.z < b.z : a.x < b.x);
    });
    std::unordered_set<std::int64_t> done;
    int budget = kMaxCellsPerTick;
    for (const Cell& c : now) {
        const std::int64_t k = key(c.x, c.y, c.z);
        if (done.count(k) != 0) continue;
        if (budget <= 0) {
            seed(c.x, c.y, c.z);  // al siguiente tick
            continue;
        }
        if (nodeKind(w.get(c.x, c.y, c.z)) == NodeKind::None) {
            power_.erase(k);
            procs_.erase(k);
            continue;
        }
        evaluate(w, c, done, budget);
    }
}

bool SignalSim::evaluate(const World& w, const Cell& start, std::unordered_set<std::int64_t>& done, int& budget) {
    const auto get = [&](int x, int y, int z) { return w.get(x, y, z); };
    // --- El circuito: todo lo enlazado ---
    std::vector<P3> cells;
    std::vector<BlockId> ids;
    std::unordered_map<std::int64_t, int> index;
    std::deque<P3> queue;
    queue.push_back({start.x, start.y, start.z});
    index[key(start.x, start.y, start.z)] = 0;
    cells.push_back({start.x, start.y, start.z});
    ids.push_back(w.get(start.x, start.y, start.z));
    std::vector<std::vector<int>> adj(1);
    std::vector<P3> tmp;
    std::size_t head = 0;
    while (head < cells.size()) {
        const P3 c = cells[head];
        const int ci = static_cast<int>(head);
        ++head;
        links(get, c, ids[static_cast<std::size_t>(ci)], tmp);
        for (const P3& n : tmp) {
            if (n.y < 0 || n.y >= kWorldHeight) continue;
            const std::int64_t nk = key(n.x, n.y, n.z);
            auto it = index.find(nk);
            int ni;
            if (it == index.end()) {
                if (static_cast<int>(cells.size()) >= kMaxCircuit) continue;
                ni = static_cast<int>(cells.size());
                index.emplace(nk, ni);
                cells.push_back(n);
                ids.push_back(w.get(n.x, n.y, n.z));
                adj.emplace_back();
            } else {
                ni = it->second;
            }
            auto& a = adj[static_cast<std::size_t>(ci)];
            if (std::find(a.begin(), a.end(), ni) == a.end()) a.push_back(ni);
            auto& b = adj[static_cast<std::size_t>(ni)];
            if (std::find(b.begin(), b.end(), ci) == b.end()) b.push_back(ci);
        }
    }
    const int n = static_cast<int>(cells.size());
    budget -= n;
    for (const P3& c : cells) done.insert(key(c.x, c.y, c.z));
    std::vector<NodeKind> kind(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) kind[static_cast<std::size_t>(i)] = nodeKind(ids[static_cast<std::size_t>(i)]);

    // --- Unidades: cada red de trazas es una; el resto, una por celda ---
    std::vector<int> unit(static_cast<std::size_t>(n), -1);
    int units = 0;
    for (int i = 0; i < n; ++i) {
        if (unit[static_cast<std::size_t>(i)] >= 0) continue;
        const int u = units++;
        unit[static_cast<std::size_t>(i)] = u;
        if (kind[static_cast<std::size_t>(i)] != NodeKind::Trace) continue;
        std::vector<int> stack{i};
        while (!stack.empty()) {
            const int c = stack.back();
            stack.pop_back();
            for (const int m : adj[static_cast<std::size_t>(c)]) {
                if (kind[static_cast<std::size_t>(m)] != NodeKind::Trace || unit[static_cast<std::size_t>(m)] >= 0) continue;
                unit[static_cast<std::size_t>(m)] = u;
                stack.push_back(m);
            }
        }
    }
    std::vector<NodeKind> ukind(static_cast<std::size_t>(units));
    std::vector<std::vector<int>> uadj(static_cast<std::size_t>(units));
    for (int i = 0; i < n; ++i) {
        const int u = unit[static_cast<std::size_t>(i)];
        ukind[static_cast<std::size_t>(u)] = kind[static_cast<std::size_t>(i)];
        for (const int m : adj[static_cast<std::size_t>(i)]) {
            const int v = unit[static_cast<std::size_t>(m)];
            if (v == u) continue;
            auto& a = uadj[static_cast<std::size_t>(u)];
            if (std::find(a.begin(), a.end(), v) == a.end()) a.push_back(v);
        }
    }
    // --- Distancia a las fuentes (solo la forma del circuito) ---
    constexpr int kInf = 1 << 28;
    std::vector<int> dist(static_cast<std::size_t>(units), kInf);
    std::deque<int> bfs;
    for (int u = 0; u < units; ++u)
        if (ukind[static_cast<std::size_t>(u)] == NodeKind::Source) {
            dist[static_cast<std::size_t>(u)] = 0;
            bfs.push_back(u);
        }
    while (!bfs.empty()) {
        const int u = bfs.front();
        bfs.pop_front();
        if (ukind[static_cast<std::size_t>(u)] == NodeKind::Sink) continue;
        for (const int v : uadj[static_cast<std::size_t>(u)]) {
            if (ukind[static_cast<std::size_t>(v)] == NodeKind::Source || dist[static_cast<std::size_t>(v)] != kInf) continue;
            dist[static_cast<std::size_t>(v)] = dist[static_cast<std::size_t>(u)] + 1;
            bfs.push_back(v);
        }
    }
    const auto feeds = [&](int p, int v) {  // la unidad procesador p manda a la unidad v
        const NodeKind kv = ukind[static_cast<std::size_t>(v)];
        if (kv == NodeKind::Source) return false;
        if (kv == NodeKind::Sink) return true;
        const int dp = dist[static_cast<std::size_t>(p)], dv = dist[static_cast<std::size_t>(v)];
        return dv > dp || (dp == kInf && dv == kInf);
    };
    const auto reads = [&](int p, int v) {  // p lee de v
        const NodeKind kv = ukind[static_cast<std::size_t>(v)];
        if (kv == NodeKind::Sink) return false;
        return dist[static_cast<std::size_t>(v)] < dist[static_cast<std::size_t>(p)];
    };

    // --- Estado de las fuentes y de los procesadores (el del tick anterior) ---
    std::vector<char> on(static_cast<std::size_t>(n), 0);  // fuente encendida / procesador emitiendo
    for (int i = 0; i < n; ++i) {
        const P3& c = cells[static_cast<std::size_t>(i)];
        const BlockId id = ids[static_cast<std::size_t>(i)];
        if (kind[static_cast<std::size_t>(i)] == NodeKind::Source) {
            on[static_cast<std::size_t>(i)] = isSourceOn(id, inputs_.count(key(c.x, c.y, c.z)) != 0) ? 1 : 0;
        } else if (kind[static_cast<std::size_t>(i)] == NodeKind::Processor) {
            auto it = procs_.find(key(c.x, c.y, c.z));
            if (it == procs_.end()) {
                Proc p;
                p.out = id == block::NullRune || id == block::EchoRelayLit;
                p.hist = p.out && (id == block::EchoRelay || id == block::EchoRelayLit) ? 0xFF : 0;
                it = procs_.emplace(key(c.x, c.y, c.z), p).first;
            }
            on[static_cast<std::size_t>(i)] = it->second.out ? 1 : 0;
        }
    }
    // --- Potencia de las trazas: desde las fuentes y salidas, -1 por bloque ---
    std::vector<int> pw(static_cast<std::size_t>(n), 0);
    std::array<std::vector<int>, 16> level;
    for (int i = 0; i < n; ++i) {
        if (kind[static_cast<std::size_t>(i)] != NodeKind::Trace) continue;
        int best = 0;
        for (const int m : adj[static_cast<std::size_t>(i)]) {
            if (!on[static_cast<std::size_t>(m)]) continue;
            if (kind[static_cast<std::size_t>(m)] == NodeKind::Source) best = 15;
            else if (kind[static_cast<std::size_t>(m)] == NodeKind::Processor &&
                     feeds(unit[static_cast<std::size_t>(m)], unit[static_cast<std::size_t>(i)]))
                best = 15;
        }
        if (best > 0) {
            pw[static_cast<std::size_t>(i)] = best;
            level[static_cast<std::size_t>(best)].push_back(i);
        }
    }
    for (int lv = 15; lv >= 2; --lv) {
        for (std::size_t q = 0; q < level[static_cast<std::size_t>(lv)].size(); ++q) {
            const int i = level[static_cast<std::size_t>(lv)][q];
            if (pw[static_cast<std::size_t>(i)] != lv) continue;
            for (const int m : adj[static_cast<std::size_t>(i)]) {
                if (kind[static_cast<std::size_t>(m)] != NodeKind::Trace || pw[static_cast<std::size_t>(m)] >= lv - 1) continue;
                pw[static_cast<std::size_t>(m)] = lv - 1;
                level[static_cast<std::size_t>(lv - 1)].push_back(m);
            }
        }
    }
    // --- Consumidores ---
    for (int i = 0; i < n; ++i) {
        if (kind[static_cast<std::size_t>(i)] != NodeKind::Sink) continue;
        int best = 0;
        for (const int m : adj[static_cast<std::size_t>(i)]) {
            const NodeKind km = kind[static_cast<std::size_t>(m)];
            if (km == NodeKind::Trace) best = std::max(best, pw[static_cast<std::size_t>(m)]);
            else if (km == NodeKind::Source && on[static_cast<std::size_t>(m)]) best = 15;
            else if (km == NodeKind::Processor && on[static_cast<std::size_t>(m)] &&
                     feeds(unit[static_cast<std::size_t>(m)], unit[static_cast<std::size_t>(i)]))
                best = 15;
        }
        pw[static_cast<std::size_t>(i)] = best;
    }
    // Vias de impulso y de freno: pasan la senal a las suyas seguidas (hasta 8).
    {
        std::vector<int> chain(static_cast<std::size_t>(n), 0);
        std::deque<int> q;
        for (int i = 0; i < n; ++i) {
            const BlockId id = ids[static_cast<std::size_t>(i)];
            if ((id == block::SurgeRail || id == block::DragRail) && pw[static_cast<std::size_t>(i)] > 0) {
                chain[static_cast<std::size_t>(i)] = 9;
                q.push_back(i);
            }
        }
        while (!q.empty()) {
            const int i = q.front();
            q.pop_front();
            const int next = chain[static_cast<std::size_t>(i)] - 1;
            if (next <= 0) continue;
            for (const int m : adj[static_cast<std::size_t>(i)]) {
                if (ids[static_cast<std::size_t>(m)] != ids[static_cast<std::size_t>(i)] || chain[static_cast<std::size_t>(m)] >= next) continue;
                chain[static_cast<std::size_t>(m)] = next;
                pw[static_cast<std::size_t>(m)] = std::max(pw[static_cast<std::size_t>(m)], next);
                q.push_back(m);
            }
        }
    }
    // --- Procesadores: leen ahora y emiten en el tick siguiente ---
    bool again = false;
    for (int i = 0; i < n; ++i) {
        if (kind[static_cast<std::size_t>(i)] != NodeKind::Processor) continue;
        const int pu = unit[static_cast<std::size_t>(i)];
        bool input = false;
        for (const int m : adj[static_cast<std::size_t>(i)]) {
            const int mu = unit[static_cast<std::size_t>(m)];
            if (!reads(pu, mu)) continue;
            const NodeKind km = kind[static_cast<std::size_t>(m)];
            if (km == NodeKind::Trace) input = input || pw[static_cast<std::size_t>(m)] > 0;
            else input = input || on[static_cast<std::size_t>(m)] != 0;
        }
        const P3& c = cells[static_cast<std::size_t>(i)];
        Proc& p = procs_[key(c.x, c.y, c.z)];
        const Proc before = p;
        const BlockId base = baseVariant(ids[static_cast<std::size_t>(i)]);
        if (base == block::NullRune) {
            p.out = !input;
        } else {
            p.hist = static_cast<std::uint8_t>((p.hist << 1) | (input ? 1 : 0));
            p.out = ((p.hist >> (kRelayDelay - 1)) & 1) != 0;
        }
        const bool settling = base == block::EchoRelay && (p.hist & 0x7) != 0 && (p.hist & 0x7) != 0x7;
        if (p.out != before.out || settling) again = true;
    }
    // --- Lo que se ve y la potencia para consultas ---
    for (int i = 0; i < n; ++i) {
        const P3& c = cells[static_cast<std::size_t>(i)];
        const std::int64_t k = key(c.x, c.y, c.z);
        const BlockId id = ids[static_cast<std::size_t>(i)];
        const NodeKind kd = kind[static_cast<std::size_t>(i)];
        bool lit = false;
        if (kd == NodeKind::Trace || kd == NodeKind::Sink) {
            lit = pw[static_cast<std::size_t>(i)] > 0;
            if (pw[static_cast<std::size_t>(i)] > 0) power_[k] = static_cast<std::uint8_t>(pw[static_cast<std::size_t>(i)]);
            else power_.erase(k);
        } else if (kd == NodeKind::Processor) {
            lit = on[static_cast<std::size_t>(i)] != 0;
            if (lit) power_[k] = 15;
            else power_.erase(k);
        } else if (kd == NodeKind::Source) {
            if (on[static_cast<std::size_t>(i)]) power_[k] = 15;
            else power_.erase(k);
            continue;
        }
        const BlockId want = litVariant(id, lit);
        if (want != id) {
            w.set(c.x, c.y, c.z, want);
            changed_ = true;
        }
    }
    if (again) seed(start.x, start.y, start.z);
    return true;
}

}  // namespace cramion::voxel::rail

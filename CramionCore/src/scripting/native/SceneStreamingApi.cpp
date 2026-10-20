// Scene.instantiate / Scene.instantiateAsync / Scene.destroyDeferred y el
// pool (Scene.pool / Scene.acquire / Scene.release): instanciar y destruir
// prefabs, grandes (celdas de un mapa, un valle entero) o muchos (balas,
// minions), sin congelar el frame.
//
// Antes, Scene.instantiate lo hacia todo en el frame que lo pedia: leer el
// archivo (y un stat en cada llamada), parsear el JSON, copiar todos los
// registros, crear las entidades y remapear sus referencias. Medido con
// Overthrow Arena: 9-43 ms por celda de ~1000 entidades y 265-491 ms por
// Valley.crprefab (9900), en un solo frame.
//
// Ahora:
//   - leer, parsear y compilar la plantilla (ecs::PrefabTemplate: registros
//     con el indice del padre, componentes ya leidos en cintas binarias y
//     referencias como indices): en los hilos de IO (jobs::scheduleIo), con
//     una cache de plantillas por ruta (LRU, scene.PrefabCacheMB), asi que
//     volver a instanciar el mismo prefab no lee, ni parsea, ni busca claves;
//     Scene.instantiate usa la misma cache (la primera vez compila en el
//     hilo principal; despues solo crea);
//   - crear: ecs::PrefabSpawn, por tandas con scene.SpawnBudgetMs por frame
//     (entre todas las peticiones, en orden), bajo una raiz desactivada que
//     se activa una sola vez al terminar;
//   - destruir: la raiz se desactiva al momento (deja de verse y de
//     simularse) y se borra de las hojas hacia arriba con
//     scene.DestroyBudgetMs por frame, con los mismos avisos que
//     Scene.destroy (OnDestroy de los scripts...);
//   - pool: Scene.pool(ruta, n) deja n copias creadas y desactivadas (por
//     tandas, con scene.SpawnBudgetMs); Scene.acquire las activa (onEnable de
//     sus scripts, sin awake ni start la segunda vez) y Scene.release las
//     desactiva (onDisable) y las devuelve, sin crear ni borrar entidades.
//
// Uso (C++):
//   const int id = Scene::instantiateAsync("Prefabs/Maps/Ashbound/3_4", pos).asInt();
//   ...cada frame: if (Scene::spawnProgress(id).asNumber() >= 1) celda = Scene::spawnResult(id);
//   Scene::pool("Prefabs/Bala", 64);                       // al cargar el nivel
//   Entity bala = Scene::acquire("Prefabs/Bala", boca, giro);  // al disparar
//   Scene::release(bala);                                  // al chocar (en vez de destroy)

#include "Modules.h"

#include "CramionCore/cvar/CVar.h"
#include "CramionCore/ecs/Prefab.h"
#include "CramionCore/ecs/PrefabSpawn.h"
#include "CramionCore/jobs/JobSystem.h"
#include "CramionCore/profiling/Profiler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace cramion::scripting::native {
namespace {

cvar::CVar<float> g_spawn_budget_ms("scene.SpawnBudgetMs", 2.0f,
                                    "Milisegundos por frame para crear las entidades de Scene.instantiateAsync y "
                                    "llenar los pools de Scene.pool (entre todas las peticiones; siempre avanza al "
                                    "menos una entidad o una copia)",
                                    cvar::Saved, 0.0f, 100.0f);
cvar::CVar<float> g_destroy_budget_ms("scene.DestroyBudgetMs", 1.0f,
                                      "Milisegundos por frame para borrar lo pedido con Scene.destroyDeferred",
                                      cvar::Saved, 0.0f, 100.0f);
cvar::CVar<int> g_prefab_cache_mb("scene.PrefabCacheMB", 192,
                                  "Tope de la cache de prefabs compilados de Scene.instantiate e instantiateAsync (MB "
                                  "en memoria, estimados como 8 veces el texto del archivo mas la plantilla)",
                                  cvar::Saved, 0, 4096);
// Lo que ocupa un documento nlohmann parseado frente a su texto (medido con
// las celdas de Ashbound: 8-12x). Antes la cache contaba el texto, asi que 48
// MB de tope eran ~0,5 GB de RAM real.
constexpr std::size_t kParsedBytesPerTextByte = 8;
// Cada cuanto se mira la fecha del archivo de un prefab de la cache (en
// OneDrive un stat cuesta): un .crprefab editado en Play se ve a los 2 s.
constexpr auto kStampCheck = std::chrono::seconds(2);

using Template = std::shared_ptr<const ecs::PrefabTemplate>;

// Lectura, parseo y compilacion en un hilo de IO.
struct ParseJob {
    std::filesystem::path file;
    std::filesystem::file_time_type known_stamp{};  // el de la cache (si habia)
    Template known;                                 // el de la cache (si habia)
    std::size_t known_bytes = 0;
    // Lo escribe la tarea:
    Template prefab;
    std::filesystem::file_time_type stamp{};
    std::size_t bytes = 0;
    std::atomic<int> state{0};  // 0 en marcha, 1 listo, 2 no existe o no es un prefab
};

struct Request {
    int id = 0;
    std::string key;  // ruta del archivo (cache)
    std::shared_ptr<ParseJob> parse;
    std::unique_ptr<ecs::PrefabSpawn> spawn;
    bool has_position = false;
    bool has_rotation = false;
    core::Vec3 position{};
    core::Vec3 rotation{};
    entt::entity parent = entt::null;
    int status = 0;  // 0 en marcha, 1 terminada, 2 fallo
    entt::entity result = entt::null;
    std::chrono::steady_clock::time_point finished{};
};

struct CachedPrefab {
    Template prefab;
    std::filesystem::file_time_type stamp{};
    std::size_t bytes = 0;
    std::uint64_t last_use = 0;
    std::chrono::steady_clock::time_point checked{};  // ultima comprobacion de la fecha
};

// Un pool por prefab (Scene.pool / acquire / release).
struct Pool {
    std::string name;  // como lo pidio el script (para los avisos)
    std::string key;   // ruta del archivo (cache)
    std::filesystem::path file;
    Template prefab;
    std::shared_ptr<ParseJob> parse;  // la plantilla, si se esta leyendo
    std::vector<entt::entity> free;   // desactivadas y listas (la ultima, la primera en salir)
    std::size_t target = 0;           // Scene.pool(ruta, n): cuantas tener creadas
    std::size_t alive = 0;            // creadas y vivas (libres + en uso)
    bool failed = false;              // no existe o no es un prefab (ya se aviso)
    // El Transform de la raiz del prefab: lo recupera cada copia al salir.
    bool has_base = false;
    core::Vec3 base_position{};
    core::Quat base_rotation{};
    core::Vec3 base_scale{1.0f, 1.0f, 1.0f};
};

struct Pooled {
    std::uint32_t pool = 0;
    bool in_use = false;
};

struct State {
    ecs::World* world = nullptr;
    std::uint64_t generation = 0;
    int next_id = 1;
    std::map<int, Request> requests;  // en orden de llegada (id creciente)
    std::unordered_map<std::string, CachedPrefab> cache;
    std::size_t cache_bytes = 0;
    std::uint64_t use_clock = 0;
    std::deque<entt::entity> destroys;  // raices pendientes de Scene.destroyDeferred
    std::vector<Pool> pools;
    std::unordered_map<std::string, std::uint32_t> pool_index;  // por ruta del archivo
    std::unordered_map<entt::entity, Pooled> pooled;            // las raices de los pools

    // Lo de un mundo (no la cache: los prefabs valen para el siguiente).
    void clearWorld() {
        requests.clear();
        destroys.clear();
        pools.clear();
        pool_index.clear();
        pooled.clear();
    }
    void clear() {
        clearWorld();
        world = nullptr;
    }

    void trimCache() {
        const std::size_t limit = static_cast<std::size_t>(std::max(0, g_prefab_cache_mb.get())) * 1024u * 1024u;
        while (cache_bytes > limit && !cache.empty()) {
            auto oldest = cache.begin();
            for (auto it = cache.begin(); it != cache.end(); ++it) {
                if (it->second.last_use < oldest->second.last_use) oldest = it;
            }
            cache_bytes -= std::min(cache_bytes, oldest->second.bytes);
            cache.erase(oldest);
        }
    }
};

// El texto de un prefab a su plantilla (cualquier hilo). bytes: memoria estimada.
Template compileText(const std::string& text, std::size_t& bytes) {
    Template prefab = text.empty() ? nullptr : ecs::compilePrefabTemplate(text);
    bytes = text.size() * kParsedBytesPerTextByte + (prefab ? ecs::prefabTemplateBytes(*prefab) : 0);
    return prefab;
}

std::shared_ptr<ParseJob> launchParse(State& s, const std::filesystem::path& file, const std::string& key) {
    auto job = std::make_shared<ParseJob>();
    job->file = file;
    if (const auto it = s.cache.find(key); it != s.cache.end()) {
        it->second.last_use = ++s.use_clock;
        // Comprobada hace poco: sin tocar el disco (antes, un stat por
        // instanciacion; en OneDrive cuesta).
        if (std::chrono::steady_clock::now() - it->second.checked < kStampCheck) {
            job->prefab = it->second.prefab;
            job->stamp = it->second.stamp;
            job->bytes = it->second.bytes;
            job->state.store(1, std::memory_order_release);
            return job;
        }
        job->known = it->second.prefab;
        job->known_stamp = it->second.stamp;
        job->known_bytes = it->second.bytes;
    }
    jobs::scheduleIo([job]() {
        std::error_code ec;
        const auto stamp = std::filesystem::last_write_time(job->file, ec);
        if (ec) {
            job->state.store(2, std::memory_order_release);
            return;
        }
        if (job->known && stamp == job->known_stamp) {
            job->prefab = job->known;
            job->bytes = job->known_bytes;
        } else {
            job->prefab = compileText(ecs::readPrefabFile(job->file), job->bytes);
        }
        job->known.reset();
        job->stamp = stamp;
        job->state.store(job->prefab ? 1 : 2, std::memory_order_release);
    });
    return job;
}

void remember(State& s, const std::string& key, const Template& prefab, std::filesystem::file_time_type stamp,
              std::size_t bytes) {
    CachedPrefab& entry = s.cache[key];
    if (entry.prefab != prefab) {
        s.cache_bytes -= std::min(s.cache_bytes, entry.bytes);
        entry.bytes = bytes > 0 ? bytes : entry.bytes;
        s.cache_bytes += entry.bytes;
    }
    entry.prefab = prefab;
    entry.stamp = stamp;
    entry.last_use = ++s.use_clock;
    entry.checked = std::chrono::steady_clock::now();
    s.trimCache();
}

void remember(State& s, const std::string& key, const ParseJob& job) { remember(s, key, job.prefab, job.stamp, job.bytes); }

// La plantilla ya, en este hilo (Scene.instantiate, Scene.acquire sin pool
// lleno): de la cache si esta al dia; si no, se lee, se parsea y se compila
// aqui (la primera vez cuesta lo de antes; las siguientes, solo crear).
Template templateNow(State& s, const std::filesystem::path& file, const std::string& key) {
    const auto now = std::chrono::steady_clock::now();
    const auto it = s.cache.find(key);
    if (it != s.cache.end() && now - it->second.checked < kStampCheck) {
        it->second.last_use = ++s.use_clock;
        return it->second.prefab;
    }
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(file, ec);
    if (ec) return nullptr;
    if (it != s.cache.end() && it->second.stamp == stamp) {
        it->second.checked = now;
        it->second.last_use = ++s.use_clock;
        return it->second.prefab;
    }
    std::size_t bytes = 0;
    Template prefab = compileText(ecs::readPrefabFile(file), bytes);
    if (!prefab) return nullptr;
    remember(s, key, prefab, stamp, bytes);
    return prefab;
}

std::filesystem::path prefabFile(const Runtime& rt, const std::string& name) {
    std::filesystem::path file = rt.root / pathFromUtf8(name);
    if (file.extension() != ecs::kPrefabExtension) file += ecs::kPrefabExtension;
    return file;
}

// Cambio de escena o de mundo: lo pendiente era del anterior.
void syncWorld(Runtime& rt, State& s) {
    const std::uint64_t generation = rt.world != nullptr ? rt.world->sceneGeneration() : 0;
    if (s.world != rt.world || s.generation != generation) {
        s.clearWorld();
        s.world = rt.world;
        s.generation = generation;
    }
}

void pumpSpawns(Runtime& rt, State& s, const std::chrono::steady_clock::time_point started, double budget, bool& advanced) {
    if (rt.world == nullptr || s.requests.empty()) return;
    CR_PROFILE_SCOPE("Scene.instantiateAsync");
    ecs::World& world = *rt.world;
    for (auto& [id, r] : s.requests) {
        if (id < 0 || r.status != 0) continue;  // precargas: las resuelve el frame
        if (!r.spawn) {
            const int state = r.parse ? r.parse->state.load(std::memory_order_acquire) : 2;
            if (state == 0) continue;  // aun leyendose (no frena a las de detras)
            if (state == 2) {
                rt.write(2, "Scene.instantiateAsync: no existe el prefab o no se pudo leer: " + r.key);
                r.status = 2;
                r.finished = std::chrono::steady_clock::now();
                continue;
            }
            remember(s, r.key, *r.parse);
            ecs::Entity parent = rt.entity(r.parent);
            r.spawn = std::make_unique<ecs::PrefabSpawn>(r.parse->prefab, parent);
            if (r.has_position) r.spawn->setPosition(r.position);
            if (r.has_rotation) r.spawn->setEulerDegrees(r.rotation);
            r.parse.reset();
        }
        const double used = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        if (advanced && used >= budget) break;
        // Siempre avanza la primera (aunque el presupuesto sea 0).
        const double left = std::max(budget - used, 0.0);
        advanced = true;
        if (r.spawn->step(world, left)) {
            r.status = r.spawn->failed() ? 2 : 1;
            r.result = r.status == 1 ? r.spawn->root().handle() : entt::entity{entt::null};
            r.finished = std::chrono::steady_clock::now();
            r.spawn.reset();
        } else {
            break;  // presupuesto gastado: las de detras esperan su turno
        }
    }
    // Terminadas que nadie recoge: se olvidan a los 30 s (la entidad sigue).
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(s.requests, [&](const auto& item) {
        return item.second.status != 0 && now - item.second.finished > std::chrono::seconds(30);
    });
}

void pumpDestroys(Runtime& rt, State& s) {
    if (rt.world == nullptr || s.destroys.empty()) return;
    CR_PROFILE_SCOPE("Scene.destroyDeferred");
    ecs::World& world = *rt.world;
    auto& registry = world.registry();
    const auto started = std::chrono::steady_clock::now();
    const double budget = std::max(0.0f, g_destroy_budget_ms.get());
    bool first = true;
    while (!s.destroys.empty()) {
        if (!first &&
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count() >= budget) {
            break;
        }
        const entt::entity root = s.destroys.front();
        if (!registry.valid(root)) {
            s.destroys.pop_front();
            continue;
        }
        // Una hoja (bajando por el ultimo hijo): de abajo arriba, borrar es
        // barato (sin recorrer subarboles) y el padre queda intacto.
        entt::entity leaf = root;
        for (;;) {
            const auto& children = world.wrap(leaf).children();
            if (children.empty()) break;
            leaf = children.back();
        }
        first = false;
        for (const auto& fn : rt.hooks.destroying) fn(leaf);
        if (rt.destroy_listener && registry.valid(leaf)) rt.destroy_listener(world.wrap(leaf));
        // El aviso pudo crear hijos (o borrarlo): se reintenta en la vuelta siguiente.
        if (!registry.valid(leaf)) continue;
        if (!world.wrap(leaf).children().empty()) continue;
        world.destroy(world.wrap(leaf));
        if (leaf == root) s.destroys.pop_front();
    }
}

// --- Pool --------------------------------------------------------------------------

Pool& poolFor(Runtime& rt, State& s, const std::string& name) {
    const std::filesystem::path file = prefabFile(rt, name);
    const std::string key = file.string();
    if (const auto it = s.pool_index.find(key); it != s.pool_index.end()) return s.pools[it->second];
    s.pool_index.emplace(key, static_cast<std::uint32_t>(s.pools.size()));
    Pool& pool = s.pools.emplace_back();
    pool.name = name;
    pool.key = key;
    pool.file = file;
    return pool;
}

// La plantilla del pool si ya esta (la lectura en otro hilo, si la hay, se recoge aqui).
bool poolReady(Runtime& rt, State& s, Pool& pool) {
    if (pool.prefab) return true;
    if (pool.failed || !pool.parse) return false;
    const int state = pool.parse->state.load(std::memory_order_acquire);
    if (state == 0) return false;
    if (state == 1) {
        remember(s, pool.key, *pool.parse);
        pool.prefab = pool.parse->prefab;
    } else {
        pool.failed = true;
        rt.write(2, "Scene.pool: no existe el prefab o no se pudo leer: " + pool.name);
    }
    pool.parse.reset();
    return pool.prefab != nullptr;
}

// Una copia nueva para el pool: desactivada (sus scripts no reciben onEnable
// hasta Scene.acquire) y en la raiz de la escena.
entt::entity createPooled(State& s, ecs::World& world, std::uint32_t index) {
    Pool& pool = s.pools[index];
    ecs::Entity e = ecs::instantiatePrefabTemplate(world, *pool.prefab, {}, /*active=*/false);
    if (!e.valid()) return entt::null;
    if (!pool.has_base) {
        pool.has_base = true;
        pool.base_position = e.localPosition();
        pool.base_rotation = e.localRotation();
        pool.base_scale = e.localScale();
    }
    ++pool.alive;
    s.pooled[e.handle()] = Pooled{index, false};
    return e.handle();
}

// Llena los pools hasta su tamano: copias enteras con el presupuesto que dejo
// instantiateAsync, y al menos una por frame si hace falta (asi un flujo
// continuo de instantiateAsync no los deja sin llenar).
void pumpPools(Runtime& rt, State& s, const std::chrono::steady_clock::time_point started, double budget) {
    bool advanced = false;
    if (rt.world == nullptr || s.pools.empty()) return;
    CR_PROFILE_SCOPE("Scene.pool");
    for (std::uint32_t i = 0; i < s.pools.size(); ++i) {
        Pool& pool = s.pools[i];
        if (pool.alive >= pool.target || !poolReady(rt, s, pool)) continue;
        while (s.pools[i].alive < s.pools[i].target) {
            const double used = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            if (advanced && used >= budget) return;
            advanced = true;
            const entt::entity h = createPooled(s, *rt.world, i);
            if (h == entt::null) break;
            s.pools[i].free.push_back(h);
        }
    }
}

}  // namespace

void registerSceneStreamingApi(Runtime& rt) {
    auto state = std::make_shared<State>();

    // Copia de una entidad, o una instancia de un prefab por su ruta en
    // Assets ("Prefabs/Enemigo" o "Prefabs/Enemigo.crprefab"), de una vez.
    rt.native.function("Scene.instantiate", [&rt, state](api::Call& c) {
        if (rt.world == nullptr) return api::Value{};
        syncWorld(rt, *state);
        ecs::Entity copy;
        if (c.arg(0).isString()) {
            const std::string name = c.string(0);
            const std::filesystem::path file = prefabFile(rt, name);
            const Template prefab = templateNow(*state, file, file.string());
            if (!prefab) {
                rt.write(2, "Scene.instantiate: no existe el prefab \"" + name + "\"");
                return api::Value{};
            }
            CR_PROFILE_SCOPE("Scene.instantiate");
            copy = ecs::instantiatePrefabTemplate(*rt.world, *prefab);
        } else {
            // Un objeto que ya no existe (o nil) no se copia: nil, como antes.
            const ecs::Entity source = rt.entity(c.entity(0));
            if (!source.valid()) return api::Value{};
            copy = rt.world->duplicate(source);
            ecs::detachCopiedLinks(*rt.world, copy);
        }
        if (!copy.valid()) return api::Value{};
        if (c.has(1)) copy.setWorldPosition(c.vec3(1));
        if (c.has(2)) copy.setLocalEulerDegrees(c.vec3(2));
        return rt.entityValue(copy);
    }, {"entity o \"Prefabs/Enemigo\", posicion, rotacion",
        "Copia de un objeto (con hijos y componentes) o instancia de un prefab", "Entity"});

    rt.native.function("Scene.instantiateAsync", [&rt, state](api::Call& c) {
        if (rt.world == nullptr) return api::Value(0.0);
        syncWorld(rt, *state);
        const std::string name = c.string(0);
        const std::filesystem::path file = prefabFile(rt, name);
        Request r;
        r.id = state->next_id++;
        r.key = file.string();
        r.parse = launchParse(*state, file, r.key);
        if (c.has(1)) {
            r.has_position = true;
            r.position = c.vec3(1);
        }
        if (c.has(2)) {
            r.has_rotation = true;
            r.rotation = c.vec3(2);
        }
        if (c.has(3)) r.parent = c.entity(3);
        const int id = r.id;
        state->requests.emplace(id, std::move(r));
        return api::Value(static_cast<double>(id));
    }, {"\"Prefabs/Celda\", posicion, rotacion, padre",
        "Instancia un prefab sin tirones: lee y parsea en otro hilo y crea las entidades por tandas (scene.SpawnBudgetMs "
        "por frame) bajo una raiz desactivada que se activa al terminar. Devuelve un id para Scene.spawnProgress / "
        "Scene.spawnResult",
        "numero"});
    rt.native.function("Scene.spawnProgress", [state](api::Call& c) {
        const auto it = state->requests.find(static_cast<int>(c.number(0)));
        if (it == state->requests.end() || it->second.status == 2) return api::Value(-1.0);
        if (it->second.status == 1) return api::Value(1.0);
        const float p = it->second.spawn ? it->second.spawn->progress() : 0.0f;
        return api::Value(static_cast<double>(std::min(p, 0.999f)));
    }, {"id", "Progreso de Scene.instantiateAsync: 0..1 (1 = lista), -1 si fallo o no existe", "numero"});
    rt.native.function("Scene.spawnResult", [&rt, state](api::Call& c) {
        const auto it = state->requests.find(static_cast<int>(c.number(0)));
        if (it == state->requests.end() || it->second.status == 0) return api::Value{};
        const entt::entity result = it->second.result;
        state->requests.erase(it);  // recogida: se olvida
        const ecs::Entity e = rt.entity(result);
        return e.valid() ? rt.entityValue(e) : api::Value{};
    }, {"id", "La raiz instanciada por Scene.instantiateAsync cuando ya esta (nil si aun no o si fallo); se recoge una vez",
        "Entity"});
    rt.native.function("Scene.cancelSpawn", [&rt, state](api::Call& c) {
        const auto it = state->requests.find(static_cast<int>(c.number(0)));
        if (it == state->requests.end()) return api::Value(false);
        // Lo ya creado se borra como Scene.destroy (al final del frame, con
        // los avisos a los scripts y modulos), no a pelo con World::destroy.
        if (it->second.spawn && rt.world != nullptr && !it->second.spawn->done()) {
            const ecs::Entity root = it->second.spawn->root();
            if (root.valid()) rt.destroyLater(root.handle());
        }
        state->requests.erase(it);
        return api::Value(true);
    }, {"id", "Cancela un Scene.instantiateAsync (borra lo que ya se habia creado)", "bool"});
    rt.native.function("Scene.preloadPrefab", [&rt, state](api::Call& c) {
        if (rt.root.empty()) return api::Value(false);
        const std::filesystem::path file = prefabFile(rt, c.string(0));
        // Una peticion sin crear nada (id negativo): al terminar de parsear
        // entra en la cache y se olvida (lo resuelve el frame).
        syncWorld(rt, *state);
        Request r;
        r.id = -(state->next_id++);
        r.key = file.string();
        r.parse = launchParse(*state, file, r.key);
        const int id = r.id;
        state->requests.emplace(id, std::move(r));
        return api::Value(true);
    }, {"\"Prefabs/Celda\"", "Lee y parsea el prefab en otro hilo y lo guarda en la cache (sin instanciarlo)", "bool"});
    rt.native.function("Scene.destroyDeferred", [&rt, state](api::Call& c) {
        const ecs::Entity e = rt.entity(c.entity(0));
        if (!e.valid() || rt.world == nullptr) return api::Value{};
        syncWorld(rt, *state);
        ecs::Entity target = e;
        target.setActive(false);  // deja de verse y de simularse ya
        state->destroys.push_back(target.handle());
        return api::Value{};
    }, {"entity", "Lo desactiva ya y lo destruye (con sus hijos) repartido en varios frames (scene.DestroyBudgetMs)"});

    // --- Pool ---
    rt.native.function("Scene.pool", [&rt, state](api::Call& c) {
        if (rt.world == nullptr || rt.root.empty()) return api::Value(false);
        syncWorld(rt, *state);
        const std::string name = c.string(0);
        const long long wanted = c.integer(1, 0);
        Pool& pool = poolFor(rt, *state, name);
        if (pool.failed) return api::Value(false);
        pool.target = std::max(pool.target, static_cast<std::size_t>(std::clamp<long long>(wanted, 0, 1 << 20)));
        // La plantilla en otro hilo; las copias, por tandas en los frames siguientes.
        if (!pool.prefab && !pool.parse) pool.parse = launchParse(*state, pool.file, pool.key);
        return api::Value(true);
    }, {"\"Prefabs/Bala\", n",
        "Prepara un pool: deja n copias del prefab creadas y desactivadas (por tandas, con scene.SpawnBudgetMs) para "
        "Scene.acquire",
        "bool"});
    rt.native.function("Scene.acquire", [&rt, state](api::Call& c) -> api::Value {
        if (rt.world == nullptr || rt.root.empty()) return {};
        syncWorld(rt, *state);
        State& s = *state;
        ecs::World& world = *rt.world;
        const std::string name = c.string(0);
        Pool& pool = poolFor(rt, s, name);
        const auto index = static_cast<std::uint32_t>(&pool - s.pools.data());
        entt::entity handle = entt::null;
        while (!pool.free.empty() && handle == entt::null) {
            const entt::entity h = pool.free.back();
            pool.free.pop_back();
            if (world.registry().valid(h)) handle = h;  // las que se borraron por fuera se saltan
        }
        if (handle == entt::null) {
            // Vacio: una copia nueva ahora (con la plantilla de la cache o leida aqui).
            if (!poolReady(rt, s, pool)) {
                if (pool.failed) return {};
                pool.parse.reset();
                pool.prefab = templateNow(s, pool.file, pool.key);
                if (!pool.prefab) {
                    pool.failed = true;
                    rt.write(2, "Scene.acquire: no existe el prefab \"" + name + "\"");
                    return {};
                }
            }
            handle = createPooled(s, world, index);
            if (handle == entt::null) return {};
        }
        s.pooled[handle].in_use = true;
        ecs::Entity e = world.wrap(handle);
        const ecs::Entity parent = c.has(3) ? rt.entityArg(c, 3) : ecs::Entity{};
        if (e.parent() != parent) e.setParent(parent, /*keep_world=*/false);
        // Como recien instanciada: el Transform del prefab y lo que se pida encima.
        e.setLocalTrs(pool.base_position, pool.base_rotation, pool.base_scale);
        if (c.has(1)) e.setWorldPosition(c.vec3(1));
        if (c.has(2)) {
            if (c.arg(2).isQuat()) e.setLocalRotation(c.quat(2));
            else e.setLocalEulerDegrees(c.vec3(2));
        }
        e.setActive(true);  // onEnable de sus scripts (awake y start solo la primera vez)
        return rt.entityValue(e);
    }, {"\"Prefabs/Bala\", posicion, rotacion, padre",
        "Saca una copia del pool del prefab (o crea una si esta vacio), la pone donde se pide (rotacion en grados o Quat) "
        "y la activa",
        "Entity"});
    rt.native.function("Scene.release", [&rt, state](api::Call& c) -> api::Value {
        ecs::Entity e = rt.entity(c.entity(0));
        if (!e.valid() || rt.world == nullptr) return api::Value(false);
        syncWorld(rt, *state);
        State& s = *state;
        const auto it = s.pooled.find(e.handle());
        if (it == s.pooled.end()) {
            // No es de un pool: se destruye como Scene.destroy.
            rt.destroyLater(e.handle());
            return api::Value(false);
        }
        if (!it->second.in_use) return api::Value(true);  // ya estaba devuelta
        it->second.in_use = false;
        e.setActive(false);  // onDisable de sus scripts (la instancia sigue viva)
        // A la raiz: si su padre se borra, la copia sigue en el pool.
        if (e.parent().valid()) e.setParent({}, /*keep_world=*/true);
        s.pools[it->second.pool].free.push_back(e.handle());
        return api::Value(true);
    }, {"entity",
        "Devuelve una copia de Scene.acquire a su pool (la desactiva: onDisable). Si no es de un pool, la destruye "
        "como Scene.destroy",
        "bool"});
    rt.native.function("Scene.poolCount", [&rt, state](api::Call& c) -> api::Value {
        if (rt.world == nullptr || rt.root.empty()) return api::Value(0.0);
        syncWorld(rt, *state);
        const std::string key = prefabFile(rt, c.string(0)).string();
        const auto it = state->pool_index.find(key);
        if (it == state->pool_index.end()) return api::Value(0.0);
        const Pool& pool = state->pools[it->second];
        return api::Value(static_cast<double>(pool.free.size()));
    }, {"\"Prefabs/Bala\"", "Copias libres (creadas y desactivadas) en el pool del prefab", "numero"});
    // Una copia de un pool que se destruye por fuera (Scene.destroy, cambiar de escena...).
    rt.onDestroying([state](entt::entity h) {
        State& s = *state;
        if (s.pooled.empty()) return;
        const auto it = s.pooled.find(h);
        if (it == s.pooled.end()) return;
        Pool& pool = s.pools[it->second.pool];
        if (pool.alive > 0) --pool.alive;
        if (!it->second.in_use) std::erase(pool.free, h);
        s.pooled.erase(it);
    });

    rt.onFrame(Phase::Begin, [&rt, state](float) {
        syncWorld(rt, *state);
        // Precargas (ids negativos): al terminar de parsear, a la cache.
        for (auto it = state->requests.begin(); it != state->requests.end();) {
            if (it->first >= 0) break;  // el map va ordenado: las negativas primero
            Request& r = it->second;
            const int st = r.parse ? r.parse->state.load(std::memory_order_acquire) : 2;
            if (st == 0) {
                ++it;
                continue;
            }
            if (st == 1) remember(*state, r.key, *r.parse);
            it = state->requests.erase(it);
        }
        pumpDestroys(rt, *state);
        // Un solo presupuesto para crear: primero las instancias pedidas, luego los pools.
        const auto started = std::chrono::steady_clock::now();
        const double budget = std::max(0.0f, g_spawn_budget_ms.get());
        bool advanced = false;
        pumpSpawns(rt, *state, started, budget, advanced);
        pumpPools(rt, *state, started, budget);
    });
    rt.onStop([state](bool) { state->clear(); });
}

}  // namespace cramion::scripting::native

#include "CramionCore/ecs/SceneLoading.h"

#include "CramionCore/cvar/CVar.h"
#include "CramionCore/ecs/Components.h"
#include "CramionCore/jobs/JobSystem.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <unordered_set>

namespace cramion::ecs {

namespace {

cvar::CVar<float> g_load_budget_ms("scene.LoadBudgetMs", 4.0f,
                                   "Milisegundos por frame para crear las entidades de Scene.loadAsync (la escena "
                                   "actual sigue jugando y dibujandose mientras tanto)",
                                   cvar::Saved, 0.1f, 100.0f);

// Cuanto se guardan las operaciones terminadas (para Scene.loadProgress / isDone).
constexpr auto kKeepFinished = std::chrono::seconds(120);

std::string stemOf(const std::filesystem::path& file) {
    const std::u8string s = file.stem().u8string();
    return std::string(s.begin(), s.end());
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

Entity rootOf(Entity e) {
    while (e.valid() && e.parent().valid()) e = e.parent();
    return e;
}

// Lectura y parseo en un hilo de IO.
struct ReadJob {
    std::filesystem::path file;
    std::shared_ptr<const ParsedEntities> parsed;
    SceneHeader header;
    std::vector<Uuid> models;
    std::string error;
    std::atomic<int> state{0};  // 0 en marcha, 1 lista, 2 fallo
};

}  // namespace

void setDontDestroyOnLoad(Entity entity, bool keep) {
    const Entity root = rootOf(entity);
    if (!root.valid()) return;
    root.get<EntityInfo>().dont_destroy_on_load = keep;
}

bool isDontDestroyOnLoad(Entity entity) {
    const Entity root = rootOf(entity);
    return root.valid() && root.get<EntityInfo>().dont_destroy_on_load;
}

struct AsyncSceneLoader::Impl {
    enum class Stage { Reading, Models, Building, Ready, Done, Failed };
    struct Op {
        int id = 0;
        bool unload = false;
        SceneLoadMode mode = SceneLoadMode::Single;
        std::filesystem::path file;
        std::string scene;
        entt::entity notify = entt::null;
        Stage stage = Stage::Reading;
        float progress = 0.0f;
        float reported = -1.0f;
        bool allow = true;
        std::string error;
        std::shared_ptr<ReadJob> job;
        std::vector<Uuid> models;
        std::size_t models_ready = 0;
        std::unique_ptr<SceneEntityBuilder> builder;
        std::chrono::steady_clock::time_point finished{};
    };
    struct Additive {
        std::string scene;
        std::filesystem::path file;
        std::vector<Uuid> roots;
    };

    int next_id = 1;
    std::map<int, Op> ops;  // por id (orden de peticion)
    std::vector<Event> events;
    std::filesystem::path active_file;
    std::string active_scene;
    std::vector<Additive> additive;

    void finish(Op& op, Stage stage) {
        op.stage = stage;
        op.finished = std::chrono::steady_clock::now();
        op.job.reset();
        op.builder.reset();
        op.models.clear();
    }

    void emit(const Op& op, Event::Kind kind) {
        Event e;
        e.kind = kind;
        e.id = op.id;
        e.scene = op.scene;
        e.mode = op.mode;
        e.progress = op.progress;
        e.notify = op.notify;
        e.error = op.error;
        events.push_back(std::move(e));
    }

    void destroyRoot(World& world, const Host& host, Entity e) {
        if (!e.valid()) return;
        if (host.destroy) {
            host.destroy(e);
        } else {
            world.destroy(e);
        }
    }

    void flush(const Host& host) {
        if (host.flush_destroys) host.flush_destroys();
    }

    bool pending(entt::entity h) const {
        for (const auto& [id, op] : ops) {
            if (!op.builder) continue;
            const auto& roots = op.builder->roots();
            if (std::find(roots.begin(), roots.end(), h) != roots.end()) return true;
        }
        return false;
    }

    bool inAdditive(World& world, entt::entity h) const {
        const Uuid& id = world.wrap(h).uuid();
        for (const Additive& a : additive) {
            if (std::find(a.roots.begin(), a.roots.end(), id) != a.roots.end()) return true;
        }
        return false;
    }

    void activate(World& world, const Host& host, Op& op) {
        const std::vector<entt::entity> new_roots = op.builder->roots();
        if (op.mode == SceneLoadMode::Single) {
            if (host.before_single) host.before_single(op.file);
            // La escena vieja (y las aditivas) fuera, salvo lo persistente, lo
            // que se esta creando para otras operaciones y lo del motor
            // (teselas de terreno: las quita before_single).
            std::vector<entt::entity> old;
            for (const entt::entity r : world.roots()) {
                if (std::find(new_roots.begin(), new_roots.end(), r) != new_roots.end()) continue;
                const Entity e = world.wrap(r);
                const EntityInfo& info = e.get<EntityInfo>();
                if (info.dont_destroy_on_load || info.transient || pending(r)) continue;
                old.push_back(r);
            }
            for (const entt::entity r : old) {
                if (world.valid(r)) destroyRoot(world, host, world.wrap(r));
            }
            flush(host);
            additive.clear();
            active_file = op.file;
            active_scene = op.scene;
            if (op.job) {
                world.setSceneUuid(op.job->header.uuid.valid() ? op.job->header.uuid : Uuid::generate());
                world.setSceneName(op.job->header.name);
            }
            op.builder->activate(world);
            if (host.after_single) host.after_single(op.file);
        } else {
            op.builder->activate(world);
            Additive a;
            a.scene = op.scene;
            a.file = op.file;
            for (const entt::entity r : new_roots) {
                if (world.valid(r)) a.roots.push_back(world.wrap(r).uuid());
            }
            additive.push_back(std::move(a));
            if (host.after_additive) host.after_additive(op.file);
        }
        op.progress = 1.0f;
        emit(op, Event::Kind::Activated);
        finish(op, Stage::Done);
    }

    void doUnload(World& world, const Host& host, Op& op) {
        const std::string wanted = lower(op.scene);
        for (auto it = additive.begin(); it != additive.end(); ++it) {
            if (lower(it->scene) != wanted) continue;
            for (const Uuid& id : it->roots) {
                if (const Entity e = world.find(id); e.valid() && !isDontDestroyOnLoad(e)) destroyRoot(world, host, e);
            }
            additive.erase(it);
            flush(host);
            op.progress = 1.0f;
            emit(op, Event::Kind::Unloaded);
            finish(op, Stage::Done);
            return;
        }
        if (!active_scene.empty() && lower(active_scene) == wanted) {
            std::vector<entt::entity> old;
            for (const entt::entity r : world.roots()) {
                const EntityInfo& info = world.wrap(r).get<EntityInfo>();
                if (info.dont_destroy_on_load || info.transient || pending(r) || inAdditive(world, r)) continue;
                old.push_back(r);
            }
            for (const entt::entity r : old) {
                if (world.valid(r)) destroyRoot(world, host, world.wrap(r));
            }
            flush(host);
            active_scene.clear();
            op.progress = 1.0f;
            emit(op, Event::Kind::Unloaded);
            finish(op, Stage::Done);
            return;
        }
        op.error = "la escena \"" + op.scene + "\" no esta cargada";
        op.progress = -1.0f;
        emit(op, Event::Kind::Failed);
        finish(op, Stage::Failed);
    }
};

AsyncSceneLoader::AsyncSceneLoader() : impl_(std::make_unique<Impl>()) {}
AsyncSceneLoader::~AsyncSceneLoader() = default;

int AsyncSceneLoader::load(const std::filesystem::path& file, SceneLoadMode mode, entt::entity notify) {
    Impl& d = *impl_;
    Impl::Op op;
    op.id = d.next_id++;
    op.mode = mode;
    op.file = file;
    op.scene = stemOf(file);
    op.notify = notify;
    auto job = std::make_shared<ReadJob>();
    job->file = file;
    op.job = job;
    jobs::scheduleIo(
        [job]() {
            std::ifstream in(job->file, std::ios::binary);
            if (!in) {
                job->error = "no se pudo abrir " + job->file.string();
                job->state.store(2, std::memory_order_release);
                return;
            }
            std::stringstream text;
            text << in.rdbuf();
            job->parsed = parseSceneText(text.str(), &job->header, &job->error);
            if (job->parsed) job->models = parsedSceneModels(*job->parsed);
            job->state.store(job->parsed ? 1 : 2, std::memory_order_release);
        },
        jobs::Priority::Normal, "Scene.loadAsync");
    const int id = op.id;
    d.ops.emplace(id, std::move(op));
    return id;
}

int AsyncSceneLoader::unload(const std::string& scene, entt::entity notify) {
    Impl& d = *impl_;
    std::string name = scene;
    // "Escenas/Nivel2.crscene" -> "Nivel2"
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos || name.find('.') != std::string::npos) {
        name = stemOf(std::filesystem::path(std::u8string(scene.begin(), scene.end())));
    }
    const std::string wanted = lower(name);
    bool loaded = !d.active_scene.empty() && lower(d.active_scene) == wanted;
    for (const Impl::Additive& a : d.additive) loaded = loaded || lower(a.scene) == wanted;
    if (!loaded) return 0;
    Impl::Op op;
    op.id = d.next_id++;
    op.unload = true;
    op.scene = name;
    op.notify = notify;
    op.stage = Impl::Stage::Ready;
    const int id = op.id;
    d.ops.emplace(id, std::move(op));
    return id;
}

bool AsyncSceneLoader::setAllowActivation(int id, bool allow) {
    const auto it = impl_->ops.find(id);
    if (it == impl_->ops.end()) return false;
    it->second.allow = allow;
    return true;
}

SceneLoadInfo AsyncSceneLoader::info(int id) const {
    SceneLoadInfo out;
    const auto it = impl_->ops.find(id);
    if (it == impl_->ops.end()) return out;
    const Impl::Op& op = it->second;
    out.id = op.id;
    out.exists = true;
    out.unload = op.unload;
    out.mode = op.mode;
    out.scene = op.scene;
    out.file = op.file;
    out.progress = op.progress;
    out.done = op.stage == Impl::Stage::Done || op.stage == Impl::Stage::Failed;
    out.failed = op.stage == Impl::Stage::Failed;
    out.allow_activation = op.allow;
    out.error = op.error;
    return out;
}

void AsyncSceneLoader::update(World& world, const Host& host, double budget_ms) {
    Impl& d = *impl_;
    if (budget_ms < 0.0) budget_ms = static_cast<double>(g_load_budget_ms.get());
    const auto now = std::chrono::steady_clock::now();
    // Las terminadas hace mucho, fuera.
    for (auto it = d.ops.begin(); it != d.ops.end();) {
        const bool finished = it->second.stage == Impl::Stage::Done || it->second.stage == Impl::Stage::Failed;
        if (finished && now - it->second.finished > kKeepFinished) {
            it = d.ops.erase(it);
        } else {
            ++it;
        }
    }
    bool built_this_frame = false;  // una escena creandose a la vez (el presupuesto es de todas)
    bool activated_this_frame = false;
    for (auto& [id, op] : d.ops) {
        if (op.stage == Impl::Stage::Done || op.stage == Impl::Stage::Failed) continue;
        // Las descargas, en su orden (despues de lo que se pidio antes).
        if (op.unload) {
            if (!activated_this_frame) {
                d.doUnload(world, host, op);
                activated_this_frame = true;
            }
            continue;
        }
        if (op.stage == Impl::Stage::Reading) {
            const int state = op.job->state.load(std::memory_order_acquire);
            if (state == 0) {
                op.progress = 0.05f;
            } else if (state == 2) {
                op.error = op.job->error.empty() ? "no se pudo leer la escena" : op.job->error;
                op.progress = -1.0f;
                std::cerr << "[Scene.loadAsync] " << op.file.string() << ": " << op.error << "\n";
                d.emit(op, Event::Kind::Failed);
                d.finish(op, Impl::Stage::Failed);
                continue;
            } else {
                op.models = host.model_ready ? op.job->models : std::vector<Uuid>{};
                op.models_ready = 0;
                op.stage = Impl::Stage::Models;
                op.progress = 0.1f;
            }
        }
        if (op.stage == Impl::Stage::Models) {
            // Se piden todas (no esperan: los hilos del AssetManager las leen)
            // y se cuenta cuantas hay ya.
            std::size_t ready = 0;
            for (const Uuid& model : op.models) {
                if (host.model_ready(model)) ++ready;
            }
            op.models_ready = ready;
            const float fraction =
                op.models.empty() ? 1.0f : static_cast<float>(ready) / static_cast<float>(op.models.size());
            op.progress = std::max(op.progress, 0.1f + 0.5f * fraction);
            if (ready >= op.models.size()) {
                // Origen: las raices se ponen donde estaban en su escena
                // (relativas al origen del mundo de ahora).
                const DVec3 scene_origin = op.job->header.origin;
                const DVec3 world_origin = world.origin();
                const core::Vec3 offset{static_cast<float>(scene_origin.x - world_origin.x),
                                        static_cast<float>(scene_origin.y - world_origin.y),
                                        static_cast<float>(scene_origin.z - world_origin.z)};
                op.builder = std::make_unique<SceneEntityBuilder>(op.job->parsed, /*hidden=*/true, offset);
                op.stage = Impl::Stage::Building;
                op.models.clear();
            }
        }
        if (op.stage == Impl::Stage::Building && !built_this_frame) {
            built_this_frame = true;
            if (op.builder->step(world, budget_ms)) {
                op.stage = Impl::Stage::Ready;
            }
            op.progress = std::max(op.progress, 0.6f + 0.3f * op.builder->progress());
        }
        if (op.stage == Impl::Stage::Ready) {
            op.progress = 0.9f;
            // Se activan en el orden en que se pidieron (como Unity) y una por frame.
            bool earlier_pending = false;
            for (const auto& [other_id, other] : d.ops) {
                if (other_id >= id) break;
                if (other.stage != Impl::Stage::Done && other.stage != Impl::Stage::Failed) earlier_pending = true;
            }
            if (op.allow && !earlier_pending && !activated_this_frame) {
                activated_this_frame = true;
                d.activate(world, host, op);
                continue;
            }
        }
        if (op.progress != op.reported) {
            op.reported = op.progress;
            d.emit(op, Event::Kind::Progress);
        }
    }
}

std::vector<AsyncSceneLoader::Event> AsyncSceneLoader::takeEvents() {
    std::vector<Event> out;
    out.swap(impl_->events);
    return out;
}

bool AsyncSceneLoader::busy() const {
    for (const auto& [id, op] : impl_->ops) {
        if (op.stage != Impl::Stage::Done && op.stage != Impl::Stage::Failed) return true;
    }
    return false;
}

void AsyncSceneLoader::reset() {
    Impl& d = *impl_;
    d.ops.clear();
    d.events.clear();
    d.additive.clear();
}

void AsyncSceneLoader::cancel(World& world) {
    for (auto& [id, op] : impl_->ops) {
        if (op.builder) op.builder->cancel(world);
    }
    reset();
}

void AsyncSceneLoader::setActiveSceneName(const std::string& scene) {
    if (scene == impl_->active_scene) return;
    impl_->active_scene = scene;
    impl_->additive.clear();
}

std::vector<std::string> AsyncSceneLoader::loadedScenes() const {
    std::vector<std::string> out;
    if (!impl_->active_scene.empty()) out.push_back(impl_->active_scene);
    for (const Impl::Additive& a : impl_->additive) out.push_back(a.scene);
    return out;
}

bool AsyncSceneLoader::isPendingRoot(entt::entity root) const { return impl_->pending(root); }

}  // namespace cramion::ecs

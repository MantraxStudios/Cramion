#ifndef CRAMION_EDITOR_EDITOR_HISTORY_H
#define CRAMION_EDITOR_EDITOR_HISTORY_H

// Historial de deshacer del editor por diferencias (ROADMAP 2 #10).
//
// Antes cada accion serializaba el mundo entero a JSON y cada Ctrl+Z
// reconstruia todas las entidades. Ahora la instantanea se guarda UNA vez y
// por entidad (su registro JSON, el mismo de las escenas, y una huella de 64
// bits), y cada paso lleva solo lo que cambio: el antes y el despues de las
// entidades tocadas y el orden de los hijos de los padres cuyo orden cambio.
//
// Como se sabe lo que cambio: en cada paso se calcula la huella de todas las
// entidades leyendo sus componentes con un visitante que no crea JSON ni
// reserva memoria, por pools de EnTT (solo las entidades que tienen cada
// componente) y repartido entre los hilos del JobSystem. Asi entra todo lo que
// cambie el mundo, venga de donde venga (Inspector, gizmos, MCP, herramientas,
// Construction Script), sin que cada sitio tenga que avisar. Transform::version
// no basta: hay escrituras directas que no la suben y no cubre los demas
// componentes. Solo las entidades con otra huella se pasan a JSON y se
// comparan con su registro. La lista de entidades (en profundidad) y el orden
// de los hijos solo se rehacen si cambio World::structureVersion.
//
// Deshacer aplica solo esas entidades (ecs::applyEntityRecord: dentro, solo
// los componentes que difieren), recrea las borradas con su UUID, quita las
// creadas y recoloca padres y orden. Los registros se comparten (shared_ptr)
// entre la instantanea y los pasos: un paso no copia texto.
//
// El texto de la escena entera (autoguardado, el mundo de una pestana que se
// guarda, Play) se arma pegando los registros, sin volver a serializar nada;
// el del autoguardado, en otro hilo.

#include <CramionCore/Uuid.h>
#include <CramionCore/ecs/SceneSerializer.h>
#include <CramionCore/ecs/World.h>
#include <CramionCore/jobs/JobSystem.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace cramion::editor {

class UndoHistory {
public:
    using RecordPtr = std::shared_ptr<const std::string>;

    // El mundo tal como esta en la instantanea, listo para pasarlo a texto
    // (en cualquier hilo: los registros no cambian nunca).
    struct Frozen {
        ecs::SceneHeader header;
        std::vector<RecordPtr> records;  // en profundidad
        // Version de la instantanea al congelar: si sigue igual (snapshotVersion),
        // la instantanea no se toco desde entonces y revert() vuelve a ella.
        std::uint64_t version = 0;
        std::string text() const;
        bool empty() const { return records.empty(); }
        void clear() {
            records.clear();
            records.shrink_to_fit();
        }
    };

    // Antes de guardar un paso, con las entidades que cambiaron (y los padres
    // de las borradas): el editor puede tocarlas (cambios propios de los
    // prefabs) y deja en `touched` las que cambio, que se vuelven a mirar.
    using BeforeStep = std::function<void(std::span<const ecs::Entity> changed, std::vector<ecs::Entity>& touched)>;

    UndoHistory();
    ~UndoHistory();
    UndoHistory(UndoHistory&&) noexcept;
    UndoHistory& operator=(UndoHistory&&) noexcept;
    UndoHistory(const UndoHistory&) = delete;
    UndoHistory& operator=(const UndoHistory&) = delete;

    // Instantanea nueva del mundo, sin pasos (abrir o crear una escena).
    void reset(ecs::World& world);
    // Un paso con lo que cambio desde la instantanea. false = nada cambio.
    bool commit(ecs::World& world, const BeforeStep& before = {});
    // La instantanea al dia sin paso (origen flotante desplazado, enlaces
    // nuevos de un prefab guardado): lo cambiado no se puede deshacer.
    void sync(ecs::World& world);
    bool undo(ecs::World& world);
    bool redo(ecs::World& world);

    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    std::size_t undoCount() const { return undo_.size(); }
    void clearRedo();
    // Quita los pasos mas viejos hasta `max_steps` y `max_bytes` (instantanea
    // incluida). Devuelve cuantos quito.
    std::size_t trim(std::size_t max_steps, std::size_t max_bytes);
    std::size_t memoryBytes() const { return steps_bytes_ + snapshot_bytes_; }

    // La escena entera (sync antes): Play, guardar el mundo de una pestana.
    Frozen freeze(ecs::World& world);
    std::string sceneText(ecs::World& world) { return freeze(world).text(); }
    // Sube con cada cambio de la instantanea (paso, sync, deshacer, reset).
    std::uint64_t snapshotVersion() const { return version_; }
    // Parar Play sin rehacer el mundo: devuelve el mundo a la instantanea
    // tocando solo las entidades que cambiaron (las creadas fuera, las
    // borradas vuelven con su UUID; las cambiadas toman su registro), sin
    // anadir pasos ni tocar deshacer/rehacer. Solo vale si la instantanea no
    // cambio desde el freeze (Frozen::version == snapshotVersion) y el origen
    // flotante es el mismo; si no, false y no toca nada.
    bool revert(ecs::World& world, std::uint64_t frozen_version);
    // Texto de la instantanea para el autoguardado, armado en otro hilo como
    // mucho cada `min_seconds` mientras `want` (va unos segundos por detras
    // del ultimo paso). nullptr hasta que haya uno de esta escena.
    const std::string* backgroundText(bool want, double min_seconds = 3.0);

    // Lo que costo el ultimo paso (para el perfilador y los tests).
    struct Stats {
        double ms = 0.0;
        std::size_t entities = 0;  // entidades comparadas
        std::size_t changed = 0;   // con otra huella
        std::size_t recorded = 0;  // que entraron en el paso
    };
    const Stats& lastStats() const { return stats_; }

private:
    struct Entry {
        RecordPtr record;
        std::uint64_t hash = 0;
        Uuid parent{};
        std::vector<Uuid> children;  // sin las transitorias, en orden
        std::uint32_t seen = 0;
    };
    struct Slot {
        entt::entity handle = entt::null;
        Uuid uuid{};
        Entry* entry = nullptr;
    };
    struct Change {
        Uuid uuid{};
        RecordPtr before;  // nullptr = no existia
        RecordPtr after;   // nullptr = se borro
    };
    struct Order {
        Uuid parent{};  // invalido = las raices
        std::vector<Uuid> before;
        std::vector<Uuid> after;
    };
    struct Step {
        std::vector<Change> changes;
        std::vector<Order> orders;
        ecs::SceneHeader scene_before;
        ecs::SceneHeader scene_after;
        bool scene_changed = false;
        ecs::DVec3 origin{};  // el origen flotante con el que se tomaron los registros
        std::size_t bytes = 0;
    };
    struct TextJob {
        Frozen frozen;
        std::string text;
        std::uint64_t version = 0;
        bool discard = false;  // otra escena: el resultado no vale (solo el hilo principal)
    };

    // Rehace la lista en profundidad si cambio la estructura. true = rehecha.
    bool refreshSlots(ecs::World& world);
    void hashAll(ecs::World& world);
    std::uint64_t hashEntity(ecs::World& world, entt::entity e) const;
    int slotOf(entt::entity e) const;
    // Compara con la instantanea y la pone al dia; con `step`, apunta el paso.
    bool diff(ecs::World& world, const BeforeStep* before, Step* step);
    void apply(ecs::World& world, const Step& step, bool forward);
    Frozen frozenNow() const;
    void dropText();
    static std::size_t stepBytes(const Step& step);

    std::unordered_map<Uuid, Entry> entries_;
    std::vector<Uuid> roots_;
    ecs::SceneHeader header_;
    bool valid_ = false;

    std::vector<Slot> slots_;
    std::vector<std::uint32_t> slot_of_;  // indice de entidad de EnTT -> slot + 1 (0 = ninguno)
    std::uint64_t slots_version_ = ~0ull;
    const ecs::World* slots_world_ = nullptr;
    std::uint32_t epoch_ = 0;
    // Buffers que se reutilizan entre pasos (sin reservas por paso).
    std::vector<std::uint64_t> hashes_;
    std::vector<std::uint32_t> changed_;
    std::vector<Uuid> destroyed_;
    std::vector<std::string> texts_;
    std::vector<Uuid> children_tmp_;

    std::deque<Step> undo_;
    std::deque<Step> redo_;
    std::size_t steps_bytes_ = 0;
    std::size_t snapshot_bytes_ = 0;
    std::uint64_t version_ = 0;  // sube con cada cambio de la instantanea

    std::shared_ptr<TextJob> job_;
    jobs::JobHandle job_handle_;
    std::string text_;
    std::uint64_t text_version_ = ~0ull;
    std::chrono::steady_clock::time_point text_at_{};

    Stats stats_;
};

}  // namespace cramion::editor

#endif  // CRAMION_EDITOR_EDITOR_HISTORY_H

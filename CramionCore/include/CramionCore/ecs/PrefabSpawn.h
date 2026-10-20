#ifndef CRAMION_CORE_ECS_PREFAB_SPAWN_H
#define CRAMION_CORE_ECS_PREFAB_SPAWN_H

// Instanciar un prefab sin tirones (Scene.instantiate, Scene.instantiateAsync
// y el pool de Scene.acquire):
//   1. parsePrefabDocument: el JSON se lee y se parsea en otro hilo (IO), una
//      sola vez; el documento es inmutable y se puede compartir entre varias
//      instancias.
//   2. compilePrefabTemplate: tambien en otro hilo, el documento pasa a una
//      plantilla compacta: los registros en orden con el indice de su padre,
//      cada componente ya leido del JSON en una cinta binaria (sus valores en
//      el orden en que los visita su reflect) y las referencias entre las
//      entidades del prefab como indices de registro. Instanciar ya no busca
//      claves en el JSON, ni convierte numeros, ni hace la pasada de remapeo
//      de referencias (los UUID nuevos se sacan antes de crear nada).
//   3. instantiatePrefabTemplate (de una vez) o PrefabSpawn::step (por tandas
//      con un presupuesto de milisegundos por frame, bajo una raiz desactivada
//      que se activa una sola vez al terminar).
// El resultado es el mismo que ecs::instantiatePrefab (PrefabInstance en la
// raiz, PrefabLink en cada entidad, UUID nuevos y referencias internas a las
// copias); solo cambia cuando y cuanto se paga. Una cinta que no casa con el
// reflect del componente (no deberia pasar: es el mismo codigo con los mismos
// valores) se lee del JSON como antes y esa entidad se remapea.

#include "CramionCore/ecs/World.h"

#include <cstddef>
#include <memory>
#include <string>

namespace cramion::ecs {

// Un .crprefab ya parseado (inmutable).
struct PrefabDocument;
// Un .crprefab compilado (inmutable; se comparte entre instancias y entre hilos).
struct PrefabTemplate;

// Cualquier hilo. nullptr si el texto no es un prefab con entidades.
std::shared_ptr<const PrefabDocument> parsePrefabDocument(const std::string& text);
// Numero de entidades del documento.
std::size_t prefabDocumentSize(const PrefabDocument& document);

// Cualquier hilo (lee los componentes en un mundo propio). Guarda el
// documento (lo necesita si una cinta no casa). nullptr si no hay documento.
std::shared_ptr<const PrefabTemplate> compilePrefabTemplate(std::shared_ptr<const PrefabDocument> document);
std::shared_ptr<const PrefabTemplate> compilePrefabTemplate(const std::string& text);
std::size_t prefabTemplateSize(const PrefabTemplate& prefab);
// Memoria estimada de la plantilla sin contar su documento (cintas y registros).
std::size_t prefabTemplateBytes(const PrefabTemplate& prefab);
// Referencias entre entidades del prefab ya resueltas como indices (estadistica).
std::size_t prefabTemplateInternalRefs(const PrefabTemplate& prefab);

// De una vez, igual que instantiatePrefab(texto). Los registros sin padre en
// el prefab (salvo la raiz) cuelgan de `parent`, como alli. active = false: la
// raiz se crea desactivada (el pool la activa al pedirla; sus scripts no
// reciben onEnable hasta entonces).
Entity instantiatePrefabTemplate(World& world, const PrefabTemplate& prefab, Entity parent = {}, bool active = true);

class PrefabSpawn {
public:
    PrefabSpawn(std::shared_ptr<const PrefabTemplate> prefab, Entity parent = {});
    // Con el documento: se compila en el primer step (en este hilo).
    PrefabSpawn(std::shared_ptr<const PrefabDocument> document, Entity parent = {});
    ~PrefabSpawn();
    PrefabSpawn(const PrefabSpawn&) = delete;
    PrefabSpawn& operator=(const PrefabSpawn&) = delete;

    // Donde poner la raiz (antes del primer step).
    void setPosition(const core::Vec3& position) {
        position_ = position;
        has_position_ = true;
    }
    void setEulerDegrees(const core::Vec3& euler) {
        euler_ = euler;
        has_euler_ = true;
    }

    // Avanza hasta gastar `budget_ms` (siempre al menos una entidad). true
    // cuando ha terminado (bien o mal: ver failed()).
    bool step(World& world, double budget_ms);
    bool done() const { return stage_ == Stage::Done || stage_ == Stage::Failed; }
    bool failed() const { return stage_ == Stage::Failed; }
    // La raiz (valida desde el primer step; activa solo al terminar).
    Entity root() const { return root_; }
    // 0..1 (creacion y, si hizo falta, remapeo).
    float progress() const;
    // Destruye lo creado si aun no habia terminado (cambio de escena, cancelar).
    void cancel(World& world);

private:
    enum class Stage { Create, Remap, Done, Failed };
    struct State;
    std::shared_ptr<const PrefabDocument> document_;
    std::shared_ptr<const PrefabTemplate> template_;
    std::unique_ptr<State> state_;
    Entity parent_;
    Entity root_;
    bool root_active_ = true;
    Stage stage_ = Stage::Create;
    core::Vec3 position_{};
    core::Vec3 euler_{};
    bool has_position_ = false;
    bool has_euler_ = false;
};

}  // namespace cramion::ecs

#endif  // CRAMION_CORE_ECS_PREFAB_SPAWN_H

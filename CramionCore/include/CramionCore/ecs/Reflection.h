#ifndef CRAMION_CORE_ECS_REFLECTION_H
#define CRAMION_CORE_ECS_REFLECTION_H

// Reflexion de componentes: cada componente describe sus propiedades UNA vez
// en reflect(PropertyVisitor&) y ese mismo codigo sirve para
//   - el Inspector del editor (un visitante que dibuja controles ImGui),
//   - guardar la escena (visitante que escribe JSON),
//   - cargarla (visitante que lee JSON),
// como los campos serializados de Unity. Anadir una propiedad es una linea.

#include "CramionCore/Uuid.h"
#include "CramionCore/asset/AssetTypes.h"

#include <CramionFX/core/Math.h>

#include <entt/entity/entity.hpp>

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace cramion::ecs {

class World;

// Nombre de una propiedad: `key` es la clave estable del archivo (no cambiar
// nunca: las escenas guardadas la usan); `label` lo que ve el usuario.
struct Meta {
    const char* key;
    const char* label;
    const char* tooltip = nullptr;
};

// Como mostrar/editar un Vec3.
enum class Vec3Kind {
    Position,   // metros
    Direction,  // se normaliza
    Scale,
    Euler,      // grados
    Color,      // 0..1
    ColorHdr,   // puede pasar de 1
};

struct FloatRange {
    float min = 0.0f;  // min == max: sin limites
    float max = 0.0f;
    float speed = 0.01f;       // arrastre en el Inspector
    const char* format = "%.3f";
    bool slider = false;       // barra en lugar de arrastre
};

// Visitante de propiedades. Cada metodo devuelve true si el visitante cambio
// el valor (el Inspector al editar, el lector JSON al cargar).
class PropertyVisitor {
public:
    virtual ~PropertyVisitor() = default;

    // Los serializadores necesitan todos los campos; la UI puede ocultar los
    // que no aplican (p. ej. los angulos de un foco en una luz puntual).
    virtual bool wantsAllFields() const { return true; }

    // Grupo con cabecera (Bloom, Vineta...). Devuelve si hay que visitar su
    // contenido (la UI puede tenerlo plegado). Siempre emparejado con
    // endGroup() si devuelve true.
    virtual bool beginGroup(const char* /*label*/, bool /*default_open*/ = true) { return true; }
    virtual void endGroup() {}

    virtual bool field(const Meta& meta, float& value, const FloatRange& range = {}) = 0;
    virtual bool field(const Meta& meta, int& value, int min = 0, int max = 0) = 0;
    virtual bool field(const Meta& meta, bool& value) = 0;
    virtual bool field(const Meta& meta, std::string& value) = 0;
    virtual bool field(const Meta& meta, core::Vec3& value, Vec3Kind kind) = 0;
    virtual bool field(const Meta& meta, core::Vec2& value, float speed = 0.05f) = 0;
    // Enum como indice en `names` (se guarda por nombre: reordenar el enum no
    // rompe las escenas).
    virtual bool enumeration(const Meta& meta, int& value, std::span<const char* const> names) = 0;
    // Referencia a un asset (el Inspector acepta arrastrar desde el navegador).
    virtual bool asset(const Meta& meta, assets::AssetRef& ref, assets::AssetType type) = 0;
    // Mascara de capas de fisica (bit i = capa i), como el LayerMask de
    // Unity. Por defecto se guarda como entero; el Inspector la muestra como
    // una lista desplegable con los nombres de las capas.
    // Referencia a otra entidad del mundo por su UUID (el objetivo de una
    // camara, el riel de un carro...). Invalida = ninguna. Por defecto se
    // guarda como texto; el Inspector acepta arrastrarla desde la Jerarquia.
    virtual bool entity(const Meta& meta, Uuid& ref) {
        std::string text = ref.valid() ? ref.toString() : std::string();
        const bool changed = field(meta, text);
        if (changed) {
            ref = text.empty() ? Uuid{} : Uuid::parse(text);
        }
        return changed;
    }

    // Lista de elementos con sus propias propiedades (puntos de un riel,
    // planos de una secuencia). Uso: listField() de abajo. El visitante puede
    // cambiar `count` (el lector JSON, el boton Anadir del Inspector);
    // endList() devuelve el indice de un elemento a quitar, o -1. Los
    // visitantes que no saben de listas las saltan.
    virtual bool beginList(const Meta& /*meta*/, std::size_t& /*count*/) { return false; }
    virtual bool beginListItem(std::size_t /*index*/) { return true; }
    virtual void endListItem() {}
    virtual int endList() { return -1; }

    virtual bool layerMask(const Meta& meta, std::uint32_t& mask) {
        int value = static_cast<int>(mask);
        const bool changed = field(meta, value);
        if (changed) {
            mask = static_cast<std::uint32_t>(value);
        }
        return changed;
    }
};

// Enum tipado sobre enumeration().
template <typename E>
bool enumField(PropertyVisitor& v, const Meta& meta, E& value, std::span<const char* const> names) {
    int index = static_cast<int>(value);
    const bool changed = v.enumeration(meta, index, names);
    if (changed) {
        value = static_cast<E>(index);
    }
    return changed;
}

// Lista de structs: `reflect_item(T&, PropertyVisitor&)` describe cada uno.
template <typename T, typename Fn>
bool listField(PropertyVisitor& v, const Meta& meta, std::vector<T>& items, Fn&& reflect_item) {
    std::size_t count = items.size();
    if (!v.beginList(meta, count)) {
        return false;
    }
    bool changed = false;
    if (count != items.size()) {
        items.resize(count);
        changed = true;
    }
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (v.beginListItem(i)) {
            reflect_item(items[i], v);
            v.endListItem();
        }
    }
    const int remove = v.endList();
    if (remove >= 0 && static_cast<std::size_t>(remove) < items.size()) {
        items.erase(items.begin() + remove);
        changed = true;
    }
    return changed;
}

// Descripcion de un tipo de componente con borrado de tipo, para el editor
// ("Add Component", Inspector, quitar) y la serializacion.
struct ComponentType {
    std::string name;       // clave estable en los .crscene ("Light")
    std::string label;      // nombre visible ("Luz")
    std::string category;   // menu "Add Component" ("Renderizado")
    bool removable = true;  // Transform no se puede quitar
    bool addable = true;    // aparece en "Add Component"

    std::function<bool(const World&, entt::entity)> has;
    std::function<void(World&, entt::entity)> add;
    std::function<void(World&, entt::entity)> remove;
    // Visita las propiedades; si alguna cambia, avisa al mundo (p. ej. el
    // Transform recalcula su matriz). Devuelve si hubo cambios.
    std::function<bool(World&, entt::entity, PropertyVisitor&)> reflect;
    // Copia el componente de una entidad a otra (duplicar).
    std::function<void(World&, entt::entity from, entt::entity to)> copy;
};

// Todos los tipos de componente registrados, en orden de registro (el del
// Inspector). Los del motor se registran solos la primera vez que se usa.
class ComponentRegistry {
public:
    static ComponentRegistry& instance();

    // T necesita: default-constructible, copiable y
    //   void reflect(PropertyVisitor&)
    // Opcional: `static void onChanged(World&, entt::entity)` tras editarlo.
    template <typename T>
    void registerComponent(std::string name, std::string label, std::string category,
                           bool removable = true, bool addable = true);

    const std::vector<ComponentType>& types() const { return types_; }
    const ComponentType* find(std::string_view name) const;

private:
    ComponentRegistry() = default;
    std::vector<ComponentType> types_;
};

// Registra los componentes del motor (idempotente). Lo llama instance().
void registerBuiltinComponents(ComponentRegistry& registry);

}  // namespace cramion::ecs

#endif  // CRAMION_CORE_ECS_REFLECTION_H

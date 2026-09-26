#ifndef CRAMION_CORE_SERIALIZER_INTERNAL_H
#define CRAMION_CORE_SERIALIZER_INTERNAL_H

// Piezas del serializador de escenas que comparten las escenas y los prefabs:
// componentes a JSON y de JSON (por reflexion) y registros de entidades.

#include "CramionCore/ecs/Reflection.h"
#include "CramionCore/ecs/World.h"

#include <nlohmann/json.hpp>

#include <vector>

namespace cramion::ecs::detail {

using nlohmann::json;

inline json vec3(const core::Vec3& v) {
    return json::array({v.x, v.y, v.z});
}

inline bool readFloat(const json& j, float& out) {
    if (!j.is_number()) {
        return false;
    }
    out = j.get<float>();
    return true;
}

// Escribe cada propiedad en un objeto JSON por su clave.
class JsonWriter final : public PropertyVisitor {
public:
    explicit JsonWriter(json& out) : stack_{&out} {}

    bool field(const Meta& m, float& v, const FloatRange&) override {
        top()[m.key] = v;
        return false;
    }
    bool field(const Meta& m, int& v, int, int) override {
        top()[m.key] = v;
        return false;
    }
    bool field(const Meta& m, bool& v) override {
        top()[m.key] = v;
        return false;
    }
    bool field(const Meta& m, std::string& v) override {
        top()[m.key] = v;
        return false;
    }
    bool field(const Meta& m, core::Vec3& v, Vec3Kind) override {
        top()[m.key] = vec3(v);
        return false;
    }
    bool field(const Meta& m, core::Vec2& v, float) override {
        top()[m.key] = json::array({v.x, v.y});
        return false;
    }
    bool enumeration(const Meta& m, int& v, std::span<const char* const> names) override {
        // Por nombre: reordenar el enum no cambia el significado del archivo.
        if (v >= 0 && v < static_cast<int>(names.size())) {
            top()[m.key] = names[static_cast<std::size_t>(v)];
        } else {
            top()[m.key] = v;
        }
        return false;
    }
    bool asset(const Meta& m, assets::AssetRef& ref, assets::AssetType) override {
        top()[m.key] = ref.valid() ? json(ref.uuid.toString()) : json(nullptr);
        return false;
    }

    bool beginList(const Meta& m, std::size_t& count) override {
        json& list = top()[m.key];
        list = json::array();
        for (std::size_t i = 0; i < count; ++i) list.push_back(json::object());
        lists_.push_back(&list);
        return true;
    }
    bool beginListItem(std::size_t index) override {
        stack_.push_back(&(*lists_.back())[index]);
        return true;
    }
    void endListItem() override { stack_.pop_back(); }
    int endList() override {
        lists_.pop_back();
        return -1;
    }

private:
    json& top() { return *stack_.back(); }
    std::vector<json*> stack_;
    std::vector<json*> lists_;
};

// Lee las propiedades que esten; las que falten no se tocan.
class JsonReader final : public PropertyVisitor {
public:
    explicit JsonReader(const json& in) : stack_{&in} {}

    bool field(const Meta& m, float& v, const FloatRange&) override {
        const json* j = find(m);
        return j != nullptr && readFloat(*j, v);
    }
    bool field(const Meta& m, int& v, int, int) override {
        const json* j = find(m);
        if (j == nullptr || !j->is_number()) {
            return false;
        }
        v = j->get<int>();
        return true;
    }
    bool field(const Meta& m, bool& v) override {
        const json* j = find(m);
        if (j == nullptr || !j->is_boolean()) {
            return false;
        }
        v = j->get<bool>();
        return true;
    }
    bool field(const Meta& m, std::string& v) override {
        const json* j = find(m);
        if (j == nullptr || !j->is_string()) {
            return false;
        }
        v = j->get<std::string>();
        return true;
    }
    bool field(const Meta& m, core::Vec3& v, Vec3Kind) override {
        const json* j = find(m);
        if (j == nullptr || !j->is_array() || j->size() < 3) {
            return false;
        }
        core::Vec3 r = v;
        if (!readFloat((*j)[0], r.x) || !readFloat((*j)[1], r.y) || !readFloat((*j)[2], r.z)) {
            return false;
        }
        v = r;
        return true;
    }
    bool field(const Meta& m, core::Vec2& v, float) override {
        const json* j = find(m);
        if (j == nullptr || !j->is_array() || j->size() < 2) {
            return false;
        }
        core::Vec2 r = v;
        if (!readFloat((*j)[0], r.x) || !readFloat((*j)[1], r.y)) {
            return false;
        }
        v = r;
        return true;
    }
    bool enumeration(const Meta& m, int& v, std::span<const char* const> names) override {
        const json* j = find(m);
        if (j == nullptr) {
            return false;
        }
        if (j->is_string()) {
            const std::string name = j->get<std::string>();
            for (std::size_t i = 0; i < names.size(); ++i) {
                if (name == names[i]) {
                    v = static_cast<int>(i);
                    return true;
                }
            }
            return false;
        }
        if (j->is_number_integer()) {
            v = j->get<int>();
            return true;
        }
        return false;
    }
    bool asset(const Meta& m, assets::AssetRef& ref, assets::AssetType type) override {
        const json* j = find(m);
        if (j == nullptr) {
            return false;
        }
        ref.type = type;
        ref.uuid = j->is_string() ? Uuid::parse(j->get<std::string>()) : Uuid{};
        return true;
    }

    bool beginList(const Meta& m, std::size_t& count) override {
        const json* list = find(m);
        if (list == nullptr || !list->is_array()) {
            return false;  // falta: la lista se queda como esta
        }
        count = list->size();
        lists_.push_back(list);
        return true;
    }
    bool beginListItem(std::size_t index) override {
        stack_.push_back(&(*lists_.back())[index]);
        return true;
    }
    void endListItem() override { stack_.pop_back(); }
    int endList() override {
        lists_.pop_back();
        return -1;
    }

private:
    const json* find(const Meta& m) const {
        const json& in = *stack_.back();
        if (!in.is_object()) return nullptr;
        const auto it = in.find(m.key);
        return it == in.end() ? nullptr : &*it;
    }
    std::vector<const json*> stack_;
    std::vector<const json*> lists_;
};

// Registro JSON de una entidad (nombre, padre, activa, tag, capa y componentes).
json entityRecord(World& world, Entity e, bool is_root_of_copy);
// Crea las entidades de una lista de registros. `remap`: UUIDs nuevos (pegar)
// en lugar de los del archivo. `root_parent`: padre de los registros sin padre.
// `created` (opcional): las entidades creadas, en el orden de los registros.
// Devuelve la primera entidad creada.
Entity buildEntities(World& world, const json& entities, bool remap, Entity root_parent,
                     std::vector<Entity>* created = nullptr);

}  // namespace cramion::ecs::detail

#endif  // CRAMION_CORE_SERIALIZER_INTERNAL_H

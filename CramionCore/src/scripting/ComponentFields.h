#ifndef CRAMION_CORE_SCRIPTING_COMPONENT_FIELDS_H
#define CRAMION_CORE_SCRIPTING_COMPONENT_FIELDS_H

// Campos de componentes por su clave (Graphics.post, Entity:getField y
// setField de la API de scripting). Privada de src/scripting.

#include "CramionCore/ecs/Reflection.h"

#include <CramionFX/core/Math.h>

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace cramion::scripting {

using core::Vec3;

namespace fields_detail {
inline std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return s;
}
}  // namespace fields_detail


// --- Campos de componentes por su clave (Graphics.post, entity:getField) ---
// Recorre la reflexion de un componente (la misma del Inspector y de los
// .crscene) para leer o cambiar un campo por su clave ("bloom",
// "look_weight", "chains[2].pull"...). Las listas se nombran con [indice]
// desde 1 (como en las versiones anteriores); escribir en el indice siguiente al ultimo anade un
// elemento. Con `post_only` (Graphics.post) se saltan las opciones del
// volumen (forma, prioridad...) y las casillas de sobrescribir: es el aspecto
// de la escena, no el volumen.
struct PostValue {
    enum class Type { None, Bool, Number, Text, Vector } type = Type::None;
    bool flag = false;
    double number = 0.0;
    std::string text;
    Vec3 vector{};
    std::vector<std::string> choices;  // enumeraciones
};

class PostFieldVisitor final : public ecs::PropertyVisitor {
    static PostValue numberValue(double n) {
        PostValue v;
        v.type = PostValue::Type::Number;
        v.number = n;
        return v;
    }

public:
    enum class Mode { Collect, Get, Set };

    PostFieldVisitor(Mode mode, std::string key = {}, PostValue value = {}, bool post_only = true)
        : mode_(mode), key_(std::move(key)), value_(std::move(value)), post_only_(post_only) {}

    bool beginGroup(const char* label, bool) override {
        skipping_ = post_only_ && std::string(label) == "Volumen";
        return true;
    }
    void endGroup() override { skipping_ = false; }

    // Listas: "clave[i].campo".
    bool beginList(const ecs::Meta& meta, std::size_t& count) override {
        if (skipping_ || meta.key == nullptr) return false;
        const std::string base = prefix_ + meta.key;
        ListState list{base, count, 0};
        if (mode_ == Mode::Collect) {
            fields_.emplace_back(base + "#", numberValue(static_cast<double>(count)));
        } else {
            const std::string open = base + "[";
            if (key_.rfind(open, 0) != 0) {
                // La lista entera: su numero de elementos.
                if (key_ == base + "#" || key_ == base) {
                    found_ = true;
                    if (mode_ == Mode::Get) {
                        value_ = numberValue(static_cast<double>(count));
                    }
                }
                return false;
            }
            const std::size_t close = key_.find(']', open.size());
            if (close == std::string::npos) return false;
            const int index = std::atoi(key_.substr(open.size(), close - open.size()).c_str());
            if (index < 1) return false;
            if (mode_ == Mode::Set && static_cast<std::size_t>(index) == count + 1) ++count;  // anadir
            if (static_cast<std::size_t>(index) > count) return false;
            list.wanted = static_cast<std::size_t>(index);
        }
        lists_.push_back(list);
        return true;
    }
    bool beginListItem(std::size_t index) override {
        if (lists_.empty()) return false;
        const ListState& list = lists_.back();
        if (mode_ != Mode::Collect && index + 1 != list.wanted) return false;
        saved_prefix_.push_back(prefix_);
        prefix_ = list.base + "[" + std::to_string(index + 1) + "].";
        return true;
    }
    void endListItem() override {
        if (saved_prefix_.empty()) return;
        prefix_ = saved_prefix_.back();
        saved_prefix_.pop_back();
    }
    int endList() override {
        if (!lists_.empty()) lists_.pop_back();
        return -1;
    }

    bool field(const ecs::Meta& meta, float& v, const ecs::FloatRange& range) override {
        PostValue current;
        current.type = PostValue::Type::Number;
        current.number = v;
        if (!visit(meta, current)) return false;
        float next = static_cast<float>(value_.number);
        if (range.min != range.max) next = std::clamp(next, range.min, range.max);
        if (next == v) return false;
        v = next;
        return true;
    }
    bool field(const ecs::Meta& meta, int& v, int min, int max) override {
        PostValue current;
        current.type = PostValue::Type::Number;
        current.number = v;
        if (!visit(meta, current)) return false;
        int next = static_cast<int>(std::lround(value_.number));
        if (min != max) next = std::clamp(next, min, max);
        if (next == v) return false;
        v = next;
        return true;
    }
    bool field(const ecs::Meta& meta, bool& v) override {
        PostValue current;
        current.type = PostValue::Type::Bool;
        current.flag = v;
        if (!visit(meta, current) || value_.flag == v) return false;
        v = value_.flag;
        return true;
    }
    bool field(const ecs::Meta& meta, std::string& v) override {
        PostValue current;
        current.type = PostValue::Type::Text;
        current.text = v;
        if (!visit(meta, current) || value_.text == v) return false;
        v = value_.text;
        return true;
    }
    bool field(const ecs::Meta& meta, Vec3& v, ecs::Vec3Kind) override {
        PostValue current;
        current.type = PostValue::Type::Vector;
        current.vector = v;
        if (!visit(meta, current)) return false;
        v = value_.vector;
        return true;
    }
    bool field(const ecs::Meta& meta, core::Vec2& v, float) override {
        PostValue current;
        current.type = PostValue::Type::Vector;
        current.vector = Vec3{v.x, v.y, 0.0f};
        if (!visit(meta, current)) return false;
        v = core::Vec2{value_.vector.x, value_.vector.y};
        return true;
    }
    bool enumeration(const ecs::Meta& meta, int& v, std::span<const char* const> names) override {
        PostValue current;
        current.type = PostValue::Type::Text;
        current.text = v >= 0 && v < static_cast<int>(names.size()) ? names[static_cast<std::size_t>(v)] : "";
        for (const char* name : names) current.choices.emplace_back(name);
        if (!visit(meta, current)) return false;
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (fields_detail::lower(names[i]) == fields_detail::lower(value_.text)) {
                if (static_cast<int>(i) == v) return false;
                v = static_cast<int>(i);
                return true;
            }
        }
        error_ = "'" + value_.text + "' no es un valor de " + key_;
        return false;
    }
    // Referencias a assets (material, Target Texture...): su UUID como texto
    // ("" = ninguno).
    bool asset(const ecs::Meta& meta, assets::AssetRef& ref, assets::AssetType) override {
        PostValue current;
        current.type = PostValue::Type::Text;
        current.text = ref.valid() ? ref.uuid.toString() : std::string();
        if (!visit(meta, current) || value_.text == current.text) return false;
        if (value_.text.empty()) {
            ref.uuid = {};
            return true;
        }
        const Uuid uuid = Uuid::parse(value_.text);
        if (!uuid.valid()) {
            error_ = "'" + value_.text + "' no es un UUID de asset";
            return false;
        }
        ref.uuid = uuid;
        return true;
    }

    // Collect: clave -> valor de todos los campos, en orden.
    const std::vector<std::pair<std::string, PostValue>>& fields() const { return fields_; }
    bool found() const { return found_; }
    const PostValue& value() const { return value_; }
    const std::string& error() const { return error_; }

private:
    // true si hay que escribir `value_` en el campo (modo Set y es su clave).
    bool visit(const ecs::Meta& meta, PostValue& current) {
        if (skipping_ || meta.key == nullptr) return false;
        const std::string key = prefix_ + meta.key;
        if (post_only_ && key.rfind("override_", 0) == 0) return false;
        switch (mode_) {
            case Mode::Collect:
                fields_.emplace_back(key, current);
                return false;
            case Mode::Get:
                if (key == key_) {
                    found_ = true;
                    value_ = current;
                }
                return false;
            case Mode::Set:
                if (key != key_) return false;
                found_ = true;
                if (!convert(current)) return false;
                return true;
        }
        return false;
    }

    // Adapta el valor del script al tipo del campo (un numero vale para una
    // casilla, true/false para un numero...).
    bool convert(const PostValue& target) {
        using T = PostValue::Type;
        if (target.type == value_.type) return true;
        if (target.type == T::Bool && value_.type == T::Number) {
            value_.flag = value_.number != 0.0;
            return true;
        }
        if (target.type == T::Number && value_.type == T::Bool) {
            value_.number = value_.flag ? 1.0 : 0.0;
            return true;
        }
        if (target.type == T::Vector && value_.type == T::Number) {
            const float n = static_cast<float>(value_.number);
            value_.vector = Vec3{n, n, n};
            return true;
        }
        if (target.type == T::Text && value_.type == T::Number && !target.choices.empty()) {
            const int index = static_cast<int>(value_.number);
            if (index >= 0 && index < static_cast<int>(target.choices.size())) {
                value_.text = target.choices[static_cast<std::size_t>(index)];
                return true;
            }
        }
        error_ = "tipo de valor equivocado para " + key_;
        return false;
    }

    struct ListState {
        std::string base;
        std::size_t count = 0;
        std::size_t wanted = 0;  // 1..count (Get/Set)
    };
    Mode mode_;
    std::string key_;
    PostValue value_;
    bool post_only_ = true;
    std::string prefix_;
    std::vector<std::string> saved_prefix_;
    std::vector<ListState> lists_;
    bool skipping_ = false;
    bool found_ = false;
    std::string error_;
    std::vector<std::pair<std::string, PostValue>> fields_;
};

}  // namespace cramion::scripting

#endif  // CRAMION_CORE_SCRIPTING_COMPONENT_FIELDS_H

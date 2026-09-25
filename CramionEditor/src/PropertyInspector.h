#ifndef CRAMION_EDITOR_PROPERTY_INSPECTOR_H
#define CRAMION_EDITOR_PROPERTY_INSPECTOR_H

#include <CramionCore/CramionCore.h>

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace cramion::editor {

// Payload de arrastrar y soltar un asset (navegador de proyecto -> escena,
// jerarquia o un campo del Inspector).
inline constexpr const char* kAssetPayload = "CRAMION_ASSET";
// Payload de una entidad arrastrada desde la Jerarquia (su Uuid).
inline constexpr const char* kEntityPayload = "CRAMION_ENTITY";
struct AssetPayload {
    Uuid uuid{};
    assets::AssetType type = assets::AssetType::Unknown;
};

// --- Edicion multiple (como Unity) -------------------------------------------
// Cada campo se identifica por su ruta ("grupo/lista/3/clave"). Con varios
// objetos seleccionados: CollectFieldsVisitor lee los valores de cada uno, los
// campos distintos se muestran con "—", y el campo que se edita en el activo
// se copia a los demas con ApplyFieldVisitor (solo ese campo; en los Vec3,
// solo los ejes tocados).
struct FieldValue {
    enum class Kind { Float, Int, Bool, String, Vec3, Vec2, Enum, Asset, Mask, Entity, ListCount, ListRemove };
    Kind kind = Kind::Float;
    float f = 0.0f;
    int i = 0;  // Int, Enum, ListRemove
    bool b = false;
    std::string s;
    core::Vec3 v3{};
    core::Vec2 v2{};
    assets::AssetRef asset{};
    std::uint32_t mask = 0;
    Uuid uuid{};
    std::size_t count = 0;
    int axes = 0x7;  // Vec3/Vec2: ejes cambiados (bit 0 = x)
};
using FieldValues = std::unordered_map<std::string, FieldValue>;
// Ruta -> ejes distintos (1 para campos simples).
using MixedFields = std::unordered_map<std::string, int>;

// Ruta del campo que se esta visitando.
class FieldPath {
protected:
    std::string pathOf(const char* key) const;
    void push(std::string part) { stack_.push_back(std::move(part)); }
    void pop() {
        if (!stack_.empty()) stack_.pop_back();
    }
    std::vector<std::string> stack_;
};

class CollectFieldsVisitor final : public ecs::PropertyVisitor, FieldPath {
public:
    explicit CollectFieldsVisitor(FieldValues& out) : out_(out) {}
    bool wantsAllFields() const override { return false; }
    bool beginGroup(const char* label, bool default_open) override;
    void endGroup() override { pop(); }
    bool field(const ecs::Meta& meta, float& value, const ecs::FloatRange& range) override;
    bool field(const ecs::Meta& meta, int& value, int min, int max) override;
    bool field(const ecs::Meta& meta, bool& value) override;
    bool field(const ecs::Meta& meta, std::string& value) override;
    bool field(const ecs::Meta& meta, core::Vec3& value, ecs::Vec3Kind kind) override;
    bool field(const ecs::Meta& meta, core::Vec2& value, float speed) override;
    bool enumeration(const ecs::Meta& meta, int& value, std::span<const char* const> names) override;
    bool asset(const ecs::Meta& meta, assets::AssetRef& ref, assets::AssetType type) override;
    bool layerMask(const ecs::Meta& meta, std::uint32_t& mask) override;
    bool entity(const ecs::Meta& meta, Uuid& ref) override;
    bool beginList(const ecs::Meta& meta, std::size_t& count) override;
    bool beginListItem(std::size_t index) override;
    void endListItem() override { pop(); }
    int endList() override {
        pop();
        return -1;
    }

private:
    FieldValues& out_;
};

class ApplyFieldVisitor final : public ecs::PropertyVisitor, FieldPath {
public:
    ApplyFieldVisitor(std::string path, FieldValue value) : path_(std::move(path)), value_(std::move(value)) {}
    bool wantsAllFields() const override { return true; }
    bool beginGroup(const char* label, bool default_open) override;
    void endGroup() override { pop(); }
    bool field(const ecs::Meta& meta, float& value, const ecs::FloatRange& range) override;
    bool field(const ecs::Meta& meta, int& value, int min, int max) override;
    bool field(const ecs::Meta& meta, bool& value) override;
    bool field(const ecs::Meta& meta, std::string& value) override;
    bool field(const ecs::Meta& meta, core::Vec3& value, ecs::Vec3Kind kind) override;
    bool field(const ecs::Meta& meta, core::Vec2& value, float speed) override;
    bool enumeration(const ecs::Meta& meta, int& value, std::span<const char* const> names) override;
    bool asset(const ecs::Meta& meta, assets::AssetRef& ref, assets::AssetType type) override;
    bool layerMask(const ecs::Meta& meta, std::uint32_t& mask) override;
    bool entity(const ecs::Meta& meta, Uuid& ref) override;
    bool beginList(const ecs::Meta& meta, std::size_t& count) override;
    bool beginListItem(std::size_t index) override;
    void endListItem() override { pop(); }
    int endList() override;

private:
    bool matches(const char* key, FieldValue::Kind kind) const { return value_.kind == kind && pathOf(key) == path_; }
    std::string path_;
    FieldValue value_;
    std::vector<std::string> lists_;
};

// Campos con valores distintos entre varias entidades (values[0] = el activo).
MixedFields mixedFields(const std::vector<FieldValues>& values);

// Dibuja las propiedades de un componente con ImGui a partir de su reflect():
// el mismo codigo que lo guarda en la escena describe su Inspector.
class ImGuiPropertyVisitor final : public ecs::PropertyVisitor, FieldPath {
public:
    explicit ImGuiPropertyVisitor(const assets::AssetDatabase* database, const ecs::World* world = nullptr)
        : database_(database), world_(world) {}

    bool wantsAllFields() const override { return false; }
    bool beginGroup(const char* label, bool default_open) override;
    void endGroup() override;

    bool field(const ecs::Meta& meta, float& value, const ecs::FloatRange& range) override;
    bool field(const ecs::Meta& meta, int& value, int min, int max) override;
    bool field(const ecs::Meta& meta, bool& value) override;
    bool field(const ecs::Meta& meta, std::string& value) override;
    bool field(const ecs::Meta& meta, core::Vec3& value, ecs::Vec3Kind kind) override;
    bool field(const ecs::Meta& meta, core::Vec2& value, float speed) override;
    bool enumeration(const ecs::Meta& meta, int& value, std::span<const char* const> names) override;
    bool asset(const ecs::Meta& meta, assets::AssetRef& ref, assets::AssetType type) override;
    // Mascara de capas: lista con los nombres de PhysicsSettings (Nada / Todo).
    bool layerMask(const ecs::Meta& meta, std::uint32_t& mask) override;
    // Referencia a entidad: su nombre, soltar desde la Jerarquia o elegir de
    // una lista con busqueda.
    bool entity(const ecs::Meta& meta, Uuid& ref) override;
    // Listas: cabecera plegable con "+", cada elemento con su "x".
    bool beginList(const ecs::Meta& meta, std::size_t& count) override;
    bool beginListItem(std::size_t index) override;
    void endListItem() override;
    int endList() override;

    // Hubo una edicion terminada (se solto el raton, Enter...): punto de
    // deshacer.
    bool editFinished() const { return edit_finished_; }

    // Edicion multiple: antes de cada componente, los campos distintos (o
    // nullptr con un solo objeto). Tras dibujarlo, el campo que cambio.
    void beginComponent(const MixedFields* mixed) {
        mixed_ = mixed;
        stack_.clear();
        change_.reset();
    }
    const std::optional<std::pair<std::string, FieldValue>>& lastChange() const { return change_; }

private:
    bool label(const ecs::Meta& meta);
    void finishItem();
    int mixed(const char* key) const;
    void record(const char* key, FieldValue value) { change_ = std::make_pair(pathOf(key), std::move(value)); }
    void recordPath(std::string path, FieldValue value) { change_ = std::make_pair(std::move(path), std::move(value)); }
    bool dragAxes(float* values, int count, float speed, float min, float max, const char* format, int mixed_axes,
                  int& changed_axes);
    const MixedFields* mixed_ = nullptr;
    std::optional<std::pair<std::string, FieldValue>> change_;
    std::vector<std::string> list_paths_;

    const assets::AssetDatabase* database_;
    const ecs::World* world_ = nullptr;
    struct ListState {
        bool open = false;
        int remove = -1;
    };
    std::vector<ListState> lists_;
    int depth_ = 0;
    bool edit_finished_ = false;
};

}  // namespace cramion::editor

#endif  // CRAMION_EDITOR_PROPERTY_INSPECTOR_H

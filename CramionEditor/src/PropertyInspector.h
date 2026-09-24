#ifndef CRAMION_EDITOR_PROPERTY_INSPECTOR_H
#define CRAMION_EDITOR_PROPERTY_INSPECTOR_H

#include <CramionCore/CramionCore.h>

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

// Dibuja las propiedades de un componente con ImGui a partir de su reflect():
// el mismo codigo que lo guarda en la escena describe su Inspector.
class ImGuiPropertyVisitor final : public ecs::PropertyVisitor {
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

private:
    bool label(const ecs::Meta& meta);
    void finishItem();

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

#ifndef CRAMION_EDITOR_PROPERTY_INSPECTOR_H
#define CRAMION_EDITOR_PROPERTY_INSPECTOR_H

#include <CramionCore/CramionCore.h>

namespace cramion::editor {

// Payload de arrastrar y soltar un asset (navegador de proyecto -> escena,
// jerarquia o un campo del Inspector).
inline constexpr const char* kAssetPayload = "CRAMION_ASSET";
struct AssetPayload {
    Uuid uuid{};
    assets::AssetType type = assets::AssetType::Unknown;
};

// Dibuja las propiedades de un componente con ImGui a partir de su reflect():
// el mismo codigo que lo guarda en la escena describe su Inspector.
class ImGuiPropertyVisitor final : public ecs::PropertyVisitor {
public:
    explicit ImGuiPropertyVisitor(const assets::AssetDatabase* database) : database_(database) {}

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

    // Hubo una edicion terminada (se solto el raton, Enter...): punto de
    // deshacer.
    bool editFinished() const { return edit_finished_; }

private:
    bool label(const ecs::Meta& meta);
    void finishItem();

    const assets::AssetDatabase* database_;
    int depth_ = 0;
    bool edit_finished_ = false;
};

}  // namespace cramion::editor

#endif  // CRAMION_EDITOR_PROPERTY_INSPECTOR_H

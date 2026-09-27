// Gizmos del audio en la Escena: alcance de los Audio Source (min/max),
// esferas de las Audio Reverb Zone y, en Play, la linea al oyente de cada
// sonido seleccionado (roja si una pared lo tapa).

#include "EditorApp.h"

#include <algorithm>

namespace cramion::editor {

using core::Vec3;

void EditorApp::drawAudioGizmos() {
    const Vec3 axes[3] = {Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}};
    const auto sphere = [&](const Vec3& c, float r, std::uint32_t color) {
        for (int i = 0; i < 3; ++i) overlayCircle(c, axes[(i + 1) % 3], axes[(i + 2) % 3], r, color);
    };
    // El oyente: el AudioListener activo o la camara del editor.
    Vec3 listener = scene_.camera().position();
    for (const entt::entity h : world_.registry().view<audio::AudioListener>()) {
        const ecs::Entity e = world_.wrap(h);
        if (e.activeInHierarchy()) {
            listener = e.worldPosition();
            break;
        }
    }
    for (const ecs::Entity e : selectedEntities()) {
        const Vec3 c = e.worldPosition();
        if (const audio::AudioReverbZone* zone = e.tryGet<audio::AudioReverbZone>()) {
            sphere(c, std::max(zone->min_distance, 0.0f), IM_COL32(120, 200, 255, 230));
            sphere(c, std::max(zone->max_distance, zone->min_distance), IM_COL32(120, 200, 255, 110));
        }
        if (const audio::AudioSource* source = e.tryGet<audio::AudioSource>(); source != nullptr && source->spatial) {
            sphere(c, source->min_distance, IM_COL32(240, 170, 70, 220));
            sphere(c, source->max_distance, IM_COL32(240, 170, 70, 90));
            const float walls = audio_.occlusionOf(e);
            if (walls >= 0.0f) {
                overlayLine(listener, c, walls > 0.05f ? IM_COL32(255, 80, 70, 255) : IM_COL32(110, 230, 120, 255));
            }
        }
    }
}

}  // namespace cramion::editor

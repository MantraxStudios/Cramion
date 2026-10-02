#ifndef CRAMION_CORE_TWOD_SYSTEM2D_H
#define CRAMION_CORE_TWOD_SYSTEM2D_H

// El 2D de un World: anima los sprites, simula la fisica 2D en Play y da al
// renderizador los sprites y tilemaps ordenados (gfx::SpriteDrawList). Lo
// lleva el editor y el juego:
//
//   twod.setAssetsRoot(proyecto / "Assets");
//   twod.start(world);                      // al dar Play (fisica 2D)
//   twod.update(world, dt, Mode::Play);     // cada frame
//   renderer.setSprites(twod.drawList(world, camara));
//   twod.stop();                            // al parar
//
// Los scripts lo encuentran con System2D::forWorld(world) (Physics2D.*,
// eventos OnCollisionEnter2D...).

#include "CramionCore/ecs/World.h"
#include "CramionCore/twod/Physics2D.h"
#include "CramionCore/twod/Sprite2D.h"
#include "CramionCore/twod/Tilemap2D.h"

#include <CramionFX/scene/Camera.h>
#include <CramionFX/vk/SpriteGeometry.h>

#include <filesystem>
#include <memory>
#include <vector>

namespace cramion::twod {

// Registra los componentes 2D en ComponentRegistry (idempotente).
void register2DComponents();

class System2D {
public:
    enum class Mode { Edit, Play, Paused };

    System2D();
    ~System2D();
    System2D(const System2D&) = delete;
    System2D& operator=(const System2D&) = delete;

    void setAssetsRoot(const std::filesystem::path& assets_root);
    SpriteLibrary& sprites() { return sprites_; }
    TilesetLibrary& tilesets() { return tilesets_; }

    // Play: fisica 2D nueva y animaciones desde el principio.
    void start(ecs::World& world);
    void stop();
    bool running() const { return running_; }
    // Animaciones (en Edit solo las de "vista previa") y la fisica en Play.
    // Devuelve los pasos de fisica 2D dados.
    int update(ecs::World& world, float dt, Mode mode);
    // Pausa > Paso.
    void step(ecs::World& world);

    Physics2DWorld& physics() { return *physics_; }
    const Physics2DWorld& physics() const { return *physics_; }
    std::vector<Physics2DEvent> takeEvents();

    // Lo que dibuja el renderizador. `camera_position`/`camera_forward`
    // deciden el orden a igual capa y orden (lo mas lejos primero).
    gfx::SpriteDrawList drawList(ecs::World& world, const core::Vec3& camera_position,
                                 const core::Vec3& camera_forward);

    // Tamano en unidades de un SpriteRenderer (su corte, sin escala): para el
    // gizmo y el picking del editor.
    bool spriteBounds(ecs::Entity e, core::Vec2& min, core::Vec2& max);
    // El sprite o tilemap mas al frente bajo un punto del mundo (plano de cada
    // uno). Invalida si no hay.
    ecs::Entity pick(ecs::World& world, const core::Vec3& ray_origin, const core::Vec3& ray_direction);

    // El sistema del mundo en Play (o nullptr): los scripts lo usan.
    static System2D* forWorld(const ecs::World* world);

private:
    void advanceAnimators(ecs::World& world, float dt, bool play);

    SpriteLibrary sprites_;
    TilesetLibrary tilesets_;
    std::unique_ptr<Physics2DWorld> physics_;
    ecs::World* world_ = nullptr;
    bool running_ = false;
};

}  // namespace cramion::twod

#endif  // CRAMION_CORE_TWOD_SYSTEM2D_H

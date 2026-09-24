#ifndef CRAMION_SCENE_SCENE_H
#define CRAMION_SCENE_SCENE_H

#include "anim/Animator.h"
#include "asset/Model.h"
#include "scene/Camera.h"
#include "scene/Light.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace cramion::dm {
class Input;
}

namespace cramion::scene {

// Instancia animada de un modelo: donde esta y en que punto de su animacion.
// El modelo (malla, esqueleto, clips) se comparte entre instancias.
struct Actor {
    std::uint32_t model = 0;  // Indice en Scene::models().
    core::Mat4 transform = core::Mat4::identity();
    anim::Animator animator;

    // Esfera que envuelve al actor en cualquier pose, en el mundo. Sirve para
    // saber que sombras de luces locales hay que redibujar cuando se mueve.
    core::Vec3 bounds_center{};
    float bounds_radius = 0.0f;
};

// Escena: los modelos cargados, la camara y una unica luz direccional (el
// sol de dia, la luna de noche) con su cielo.
//
// Es la capa que el renderizador consulta cada frame; no conoce Vulkan.
class Scene {
public:
    // Coloca la luz y deja la camara en el origen (ver placeCamera()).
    void initialize();

    // Avanza el ciclo del sol y las animaciones y aplica la entrada a la
    // camara.
    void update(const dm::Input& input, float delta_seconds);

    const LightSet& lights() const { return lights_; }

    // Carga un modelo con assimp y devuelve su indice. Lanza si falla.
    std::uint32_t loadModel(const std::filesystem::path& path);

    // Coloca una instancia del modelo de pie sobre el terreno, cerca de (x, z)
    // y mirando en la direccion `yaw` (radianes, 0 = +Z). Se escala para que
    // mida `height` unidades y reproduce en bucle su primera animacion.
    void spawnActor(std::uint32_t model, float x, float z, float height, float yaw);

    // Coloca un modelo tal cual viene (escenarios: San Miguel, Sponza...),
    // con la transformacion indicada y sin escalarlo.
    void spawnStatic(std::uint32_t model,
                     const core::Mat4& transform = core::Mat4::identity());

    // Situa la camara en `position` mirando hacia `target`.
    void placeCamera(const core::Vec3& position, const core::Vec3& target);

    const std::vector<std::unique_ptr<asset::ModelData>>& models() const { return models_; }
    const std::vector<Actor>& actors() const { return actors_; }

    Camera& camera() { return camera_; }
    const Camera& camera() const { return camera_; }

    // Estado de los interruptores de la demo (para mostrarlo en el titulo).
    bool dayCycleEnabled() const { return day_cycle_enabled_; }

    // Hora del dia en [0, 24): 6 = amanecer, 12 = mediodia, 18 = ocaso.
    float timeOfDayHours() const;

private:
    void createLights();
    void updateSun(float delta_seconds);

    Camera camera_;
    LightSet lights_;

    // unique_ptr: los Animator guardan un puntero a su modelo, que no debe
    // moverse al crecer el vector.
    std::vector<std::unique_ptr<asset::ModelData>> models_;
    std::vector<Actor> actors_;

    float time_seconds_ = 0.0f;
    // Angulo del sol en su arco: 0 = amanecer, pi/2 = mediodia, pi = ocaso,
    // de pi a 2*pi es de noche.
    float sun_angle_ = core::radians(55.0f);

    bool day_cycle_enabled_ = false;
};

}  // namespace cramion::scene

#endif  // CRAMION_SCENE_SCENE_H

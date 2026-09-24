#ifndef CRAMION_SCENE_SCENE_H
#define CRAMION_SCENE_SCENE_H

#include "CramionFX/anim/Animator.h"
#include "CramionFX/asset/Model.h"
#include "CramionFX/scene/Camera.h"
#include "CramionFX/scene/Light.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
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
    // `force_static`: escenario, ver asset::loadModel.
    std::uint32_t loadModel(const std::filesystem::path& path, bool force_static = false);

    // Coloca una instancia del modelo de pie sobre el terreno, cerca de (x, z)
    // y mirando en la direccion `yaw` (radianes, 0 = +Z). Se escala para que
    // mida `height` unidades y reproduce en bucle su primera animacion.
    void spawnActor(std::uint32_t model, float x, float z, float height, float yaw);

    // Coloca un modelo tal cual viene (escenarios: San Miguel, Sponza...),
    // con la transformacion indicada y sin escalarlo.
    void spawnStatic(std::uint32_t model,
                     const core::Mat4& transform = core::Mat4::identity());

    // Ajusta a mano los materiales llamados `name` de un modelo cargado. Los
    // OBJ no guardan PBR y a veces el exportador pierde propiedades (el suelo
    // de marmol pulido de Sibenik llega como "mate"). Hay que llamarlo antes
    // de subir los modelos al renderizador. Devuelve cuantos cambio.
    // `reflectance` es el F0 de la parte no metalica (0.04 por defecto).
    // `albedo_scale` multiplica el color base (lineal): el vidrio opaco de
    // una ventana casi no tiene difuso (la luz se pierde en el interior).
    // `base_color` (lineal), si tiene alguna componente >= 0, sustituye al
    // color base antes de escalarlo: los MTL de Phong dejan los metales con
    // Kd casi negro (su color va en Ks) y en PBR el color de un metal ES su
    // reflejo.
    std::uint32_t overrideMaterial(std::uint32_t model, const std::string& name, float roughness,
                                   float metallic = 0.0f, float reflectance = 0.04f,
                                   float albedo_scale = 1.0f,
                                   const core::Vec3& base_color = core::Vec3{-1.0f, -1.0f, -1.0f});

    // Marca los normal maps de un modelo como de convenio DirectX (+Y hacia
    // abajo). Para los assets de Unreal o Lumberyard (Bistro), cuyo archivo
    // no lo indica. Antes de subir los modelos.
    void setDirectXNormalMaps(std::uint32_t model, bool directx);

    // Situa la camara en `position` mirando hacia `target`.
    void placeCamera(const core::Vec3& position, const core::Vec3& target);

    const std::vector<std::unique_ptr<asset::ModelData>>& models() const { return models_; }
    const std::vector<Actor>& actors() const { return actors_; }

    Camera& camera() { return camera_; }
    const Camera& camera() const { return camera_; }

    // Estado de los interruptores de la demo (para mostrarlo en el titulo).
    bool dayCycleEnabled() const { return day_cycle_enabled_; }

    // Sol fijo en una direccion (hacia el sol): el del cielo fotografiado de
    // un mapa de entorno HDR. Mientras este fijado no hay ciclo de dia ni
    // salto a la noche (la foto es de una hora concreta). nullopt lo suelta.
    void setFixedSun(const std::optional<core::Vec3>& to_sun) { fixed_sun_ = to_sun; }
    bool sunFixed() const { return fixed_sun_.has_value(); }

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
    std::optional<core::Vec3> fixed_sun_;
};

}  // namespace cramion::scene

#endif  // CRAMION_SCENE_SCENE_H

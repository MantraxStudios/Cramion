// Pruebas de las capas de render, el arma en primera persona (viewmodel) y los
// contornos de juego (RM-capas). Consola, sin GPU: la serializacion (y que las
// escenas de antes cargan igual), el culling por capas, la matriz del
// viewmodel, el empaquetado de los estilos del contorno, la recogida de los
// Outline del mundo y las sombras de las luces locales por capas.
// Devuelve 0 si todo va.

#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/Outline.h"
#include "CramionCore/ecs/RenderLayers.h"
#include "CramionCore/ecs/SceneSerializer.h"
#include "CramionCore/ecs/World.h"
#include "CramionCore/physics/PhysicsSettings.h"

#include <CramionFX/scene/Camera.h>
#include <CramionFX/scene/Light.h>
#include <CramionFX/scene/LocalLightShadows.h>
#include <CramionFX/scene/RenderLayers.h>
#include <CramionFX/scene/Scene.h>

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <string>

using namespace cramion;
using core::Vec3;
using core::Vec4;

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const char* what) {
    ++checks;
    std::printf("  %s %s\n", condition ? "OK   " : "FALLO", what);
    if (!condition) ++failures;
}

bool near(float a, float b, float eps = 1e-3f) { return std::abs(a - b) < eps; }

// Quita una clave de todos los objetos del JSON (una escena "de antes").
void stripKey(nlohmann::json& j, const std::string& key) {
    if (j.is_object()) {
        j.erase(key);
        for (auto& [k, v] : j.items()) stripKey(v, key);
    } else if (j.is_array()) {
        for (auto& v : j) stripKey(v, key);
    }
}

void testDefaults() {
    std::printf("Valores por defecto (sin cambios)\n");
    ecs::MeshRenderer mr;
    ecs::Camera cam;
    ecs::Light light;
    check(mr.render_layers == 1u, "Mesh Renderer en la capa 0");
    check(cam.culling_mask == 0xFFFFFFFFu, "la camara ve todas las capas");
    check(cam.viewmodel_layers == 0u, "sin viewmodel");
    check(light.shadow_layers == 0xFFFFFFFFu, "la luz da sombra a todas las capas");
    scene::Actor actor;
    scene::Camera view;
    check(actor.layers == scene::kDefaultRenderLayers && view.cullingMask() == scene::kAllRenderLayers,
          "actor y camara del render igual");
    check(!view.viewmodel().active(), "camara del render sin viewmodel");
    check(scene::DirectionalLight{}.shadow_layers == scene::kAllRenderLayers &&
              scene::PointLight{}.shadow_layers == scene::kAllRenderLayers &&
              scene::SpotLight{}.shadow_layers == scene::kAllRenderLayers,
          "luces del render con sombra en todas las capas");
    check(scene::layersVisible(actor.layers, view.cullingMask()), "por defecto todo se ve");
}

void testSerialization() {
    std::printf("Guardar y leer\n");
    ecs::World world;
    ecs::Entity arma = world.create("Arma");
    ecs::MeshRenderer& mr = arma.add<ecs::MeshRenderer>();
    mr.render_layers = physics::layerBit(8) | physics::layerBit(31);
    ecs::Entity camara = world.create("Camara");
    ecs::Camera& cam = camara.add<ecs::Camera>();
    cam.culling_mask = ~physics::layerBit(8);
    cam.viewmodel_layers = physics::layerBit(8);
    cam.viewmodel_fov = 55.0f;
    cam.viewmodel_near = 0.02f;
    cam.viewmodel_depth = 0.05f;
    ecs::Entity sol = world.create("Sol");
    ecs::Light& light = sol.add<ecs::Light>();
    light.type = ecs::LightType::Directional;
    light.shadow_layers = ~physics::layerBit(8);
    ecs::Entity heroe = world.create("Heroe");
    ecs::Outline& outline = heroe.add<ecs::Outline>();
    outline.color = Vec3{0.1f, 0.9f, 0.2f};
    outline.mode = ecs::OutlineMode::Occluded;
    outline.width = 3.5f;
    outline.occluded_fill = 0.4f;
    outline.pulse_speed = 1.5f;
    outline.include_children = false;

    const std::string text = ecs::serializeWorld(world);
    ecs::World copy;
    check(ecs::deserializeWorld(copy, text), "la escena se lee");
    const ecs::MeshRenderer* m2 = copy.findByName("Arma").tryGet<ecs::MeshRenderer>();
    check(m2 != nullptr && m2->render_layers == (physics::layerBit(8) | physics::layerBit(31)),
          "capas del Mesh Renderer (incluida la 31: bit alto)");
    const ecs::Camera* c2 = copy.findByName("Camara").tryGet<ecs::Camera>();
    check(c2 != nullptr && c2->culling_mask == ~physics::layerBit(8) && c2->viewmodel_layers == physics::layerBit(8),
          "culling mask y capas del viewmodel");
    check(c2 != nullptr && near(c2->viewmodel_fov, 55.0f) && near(c2->viewmodel_near, 0.02f) &&
              near(c2->viewmodel_depth, 0.05f),
          "campo de vision, plano cercano y profundidad del viewmodel");
    const ecs::Light* l2 = copy.findByName("Sol").tryGet<ecs::Light>();
    check(l2 != nullptr && l2->shadow_layers == ~physics::layerBit(8), "capas con sombra de la luz");
    const ecs::Outline* o2 = copy.findByName("Heroe").tryGet<ecs::Outline>();
    check(o2 != nullptr && o2->mode == ecs::OutlineMode::Occluded && near(o2->width, 3.5f) &&
              near(o2->occluded_fill, 0.4f) && near(o2->pulse_speed, 1.5f) && !o2->include_children &&
              near(o2->color.y, 0.9f),
          "componente Outline");

    // Una escena de antes (sin los campos nuevos): todo como siempre.
    nlohmann::json old = nlohmann::json::parse(text);
    for (const char* key : {"render_layers", "culling_mask", "viewmodel_layers", "viewmodel_fov", "viewmodel_near",
                            "viewmodel_depth", "shadow_layers"}) {
        stripKey(old, key);
    }
    ecs::World legacy;
    check(ecs::deserializeWorld(legacy, old.dump()), "la escena de antes se lee");
    const ecs::MeshRenderer* m3 = legacy.findByName("Arma").tryGet<ecs::MeshRenderer>();
    const ecs::Camera* c3 = legacy.findByName("Camara").tryGet<ecs::Camera>();
    const ecs::Light* l3 = legacy.findByName("Sol").tryGet<ecs::Light>();
    check(m3 != nullptr && m3->render_layers == 1u, "sin capas: la capa 0");
    check(c3 != nullptr && c3->culling_mask == 0xFFFFFFFFu && c3->viewmodel_layers == 0u,
          "sin culling mask: ve todo, sin viewmodel");
    check(l3 != nullptr && l3->shadow_layers == 0xFFFFFFFFu, "sin capas con sombra: todas");
}

void testCameraAndCulling() {
    std::printf("Culling por capas\n");
    ecs::Camera cam;
    cam.culling_mask = ~physics::layerBit(8);
    cam.viewmodel_layers = physics::layerBit(9);
    cam.viewmodel_fov = 50.0f;
    scene::Camera view;
    ecs::applyCameraLayers(cam, view);
    check(view.cullingMask() == ~physics::layerBit(8), "la culling mask llega a la camara del render");
    check(view.viewmodel().active() && view.viewmodel().layers == physics::layerBit(9) &&
              near(view.viewmodel().fov_y, core::radians(50.0f)),
          "y el viewmodel (en radianes)");
    check(!scene::layersVisible(physics::layerBit(8), view.cullingMask()), "la capa 8 no se ve");
    check(scene::layersVisible(physics::layerBit(8) | 1u, view.cullingMask()), "con otra capa visible, si");
    check(!scene::layersVisible(0u, scene::kAllRenderLayers), "sin capas: no lo ve nadie");
    ecs::Camera bad;
    bad.viewmodel_fov = 500.0f;
    bad.viewmodel_depth = 0.0f;
    bad.viewmodel_layers = 1u;
    ecs::applyCameraLayers(bad, view);
    check(view.viewmodel().fov_y <= core::radians(150.0f) + 1e-4f && view.viewmodel().depth_range >= 0.0005f,
          "valores fuera de rango acotados");

    // Nombres de las capas (las del proyecto, como Physics.layerMask).
    physics::PhysicsSettings settings;
    settings.layer_names[8] = "Arma";
    physics::setProjectPhysicsSettings(settings);
    check(ecs::renderLayerMask({"Default", "Arma", "NoExiste"}) == (physics::layerBit(0) | physics::layerBit(8)),
          "mascara por nombres");
    physics::setProjectPhysicsSettings(physics::PhysicsSettings{});
}

// Proyecta un punto (mundo) con la camara: NDC (x, y) y profundidad [0, 1].
Vec3 project(const scene::Camera& camera, const Vec3& p) {
    const Vec4 clip = camera.projection() * (camera.view() * Vec4{p.x, p.y, p.z, 1.0f});
    return Vec3{clip.x / clip.w, clip.y / clip.w, clip.z / clip.w};
}

void testViewmodel() {
    std::printf("Viewmodel\n");
    scene::Camera camera;
    camera.setPosition(Vec3{3.0f, 2.0f, -1.0f});
    camera.setOrientation(core::normalize(Vec3{0.3f, -0.1f, -1.0f}), Vec3{0.0f, 1.0f, 0.0f});
    camera.setAspectRatio(16.0f / 9.0f);
    camera.setFovY(core::radians(90.0f));
    camera.setClipPlanes(0.1f, 1000.0f);
    check(core::Mat4::identity().m[0][0] == scene::viewmodelMatrix(camera).m[0][0] &&
              scene::viewmodelMatrix(camera).m[3][0] == 0.0f,
          "sin viewmodel: identidad");

    scene::Camera::Viewmodel vm{};
    vm.layers = 1u << 9;
    vm.fov_y = core::radians(60.0f);
    vm.near_plane = 0.02f;
    camera.setViewmodel(vm);
    // El arma dibujada con una camara de 60 grados (el resto igual)...
    scene::Camera reference = camera;
    reference.setFovY(core::radians(60.0f));
    const core::Mat4 m = scene::viewmodelMatrix(camera);
    bool same_screen = true;
    bool in_front = true;
    const Vec3 eye = camera.position();
    for (const Vec3& local : {Vec3{0.2f, -0.15f, -0.4f}, Vec3{-0.05f, -0.1f, -0.03f}, Vec3{0.3f, 0.1f, -0.9f}}) {
        // Un punto delante de la camara (en su espacio de vista -> mundo).
        const core::Mat4 to_world = core::inverse(camera.view());
        const Vec4 w = to_world * Vec4{local.x, local.y, local.z, 1.0f};
        const Vec3 world{w.x, w.y, w.z};
        const Vec4 moved4 = m * Vec4{world.x, world.y, world.z, 1.0f};
        const Vec3 moved{moved4.x, moved4.y, moved4.z};
        const Vec3 a = project(camera, moved);
        const Vec3 b = project(reference, world);
        same_screen = same_screen && near(a.x, b.x, 1e-3f) && near(a.y, b.y, 1e-3f);
        // ...queda mas alla del plano cercano de la camara (no se corta).
        const float distance = -(camera.view() * Vec4{moved.x, moved.y, moved.z, 1.0f}).z;
        in_front = in_front && distance >= camera.nearPlane() - 1e-4f;
        (void)eye;
    }
    check(same_screen, "en pantalla, como con el campo de vision del viewmodel");
    check(in_front, "lo mas cercano (viewmodel_near) queda tras el plano cercano");
    check(near(scene::viewmodelDepthScale(0.1f, 0.02f), 5.0f) && near(scene::viewmodelDepthScale(0.1f, 0.5f), 1.0f),
          "escala de profundidad: solo aleja");
    check(near(scene::viewmodelFovScale(core::radians(90.0f), core::radians(90.0f)), 1.0f),
          "mismo campo de vision: sin escala");
    // Sigue a la camara: misma posicion relativa al moverla (vectores de
    // movimiento ~0 para un arma quieta respecto a la camara).
    const Vec3 local_point{0.2f, -0.15f, -0.4f};
    const auto screen_of = [&](const scene::Camera& c) {
        const Vec4 w = core::inverse(c.view()) * Vec4{local_point.x, local_point.y, local_point.z, 1.0f};
        const Vec4 moved = scene::viewmodelMatrix(c) * w;
        return project(c, Vec3{moved.x, moved.y, moved.z});
    };
    const Vec3 before = screen_of(camera);
    camera.setPosition(Vec3{10.0f, 4.0f, 7.0f});
    camera.setOrientation(core::normalize(Vec3{-1.0f, 0.2f, 0.4f}), Vec3{0.0f, 1.0f, 0.0f});
    const Vec3 after = screen_of(camera);
    check(near(before.x, after.x, 1e-3f) && near(before.y, after.y, 1e-3f) && near(before.z, after.z, 1e-4f),
          "pegado a la camara al moverla y girarla");
    scene::Camera ortho = camera;
    ortho.setOrthographic(true);
    check(scene::viewmodelMatrix(ortho).m[3][0] == 0.0f && scene::viewmodelMatrix(ortho).m[0][0] == 1.0f,
          "ortografica: sin viewmodel");
    check(near(scene::viewmodelDepthRange(vm), 0.02f), "rango de profundidad por defecto 0.02");
}

void testOutlineStyles() {
    std::printf("Estilos de contorno\n");
    scene::OutlineStyle s{};
    s.color = Vec3{1.0f, 0.5f, 0.0f};
    s.alpha = 0.5f;
    s.width = 3.25f;
    s.mode = scene::OutlineMode::Occluded;
    s.occluded_fill = 1.0f;
    const auto p = scene::packOutlineStyle(s);
    check((p[0] & 0xFFu) == 255u && ((p[0] >> 8) & 0xFFu) == 128u && ((p[0] >> 24) & 0xFFu) == 128u,
          "color RGBA8");
    check(near(scene::unpackOutlineWidth(p[1]), 3.25f, 1e-3f), "grosor en 1/16 px");
    check(scene::unpackOutlineMode(p[1]) == scene::OutlineMode::Occluded, "modo");
    check(((p[1] >> 16) & 0xFFu) == 255u, "relleno de lo tapado");
    s.width = 100.0f;
    check(scene::unpackOutlineWidth(scene::packOutlineStyle(s)[1]) <= 16.0f, "grosor acotado");

    scene::GameOutlines outlines;
    scene::OutlineStyle a{};
    scene::OutlineStyle b{};
    b.color = Vec3{0.0f, 1.0f, 0.0f};
    check(outlines.addStyle(a) == 0 && outlines.addStyle(b) == 1 && outlines.addStyle(a) == 0,
          "estilos iguales se comparten");
    for (int i = 0; i < 40; ++i) {
        scene::OutlineStyle c{};
        c.color = Vec3{static_cast<float>(i) / 64.0f, 0.3f, 0.3f};
        outlines.addStyle(c);
    }
    check(outlines.styles.size() == scene::kMaxOutlineStyles, "como mucho 15 estilos");
    const auto packed = outlines.pack();
    check(packed[30] == scene::kMaxOutlineStyles && packed[31] == 2u, "cuantos y radio de busqueda");
    check(sizeof(packed) == 128, "caben en 128 bytes de push constants");

    check(near(scene::outlinePulse(0.0f, 0.0f, 0.2f), 1.0f), "sin pulso: 1");
    check(near(scene::outlinePulse(0.0f, 1.0f, 0.2f), 1.0f) && near(scene::outlinePulse(0.5f, 1.0f, 0.2f), 0.2f),
          "pulso de minimo a 1");
}

void testCollectOutlines() {
    std::printf("Outline en el mundo\n");
    ecs::World world;
    ecs::Entity heroe = world.create("Heroe");
    ecs::Entity casco = world.create("Casco", heroe);
    ecs::Entity enemigo = world.create("Enemigo");
    ecs::Entity apagado = world.create("Apagado");
    ecs::Entity inactivo = world.create("Inactivo");
    ecs::Outline& oh = heroe.add<ecs::Outline>();
    oh.mode = ecs::OutlineMode::Occluded;
    oh.occluded_fill = 0.5f;
    ecs::Outline& oe = enemigo.add<ecs::Outline>();
    oe.color = Vec3{1.0f, 0.0f, 0.0f};
    oe.include_children = false;
    oe.pulse_speed = 2.0f;
    oe.pulse_min = 0.0f;
    apagado.add<ecs::Outline>().enabled = false;
    inactivo.add<ecs::Outline>();
    inactivo.setActive(false);
    // Actores falsos: Heroe = 0, Casco = 1, Enemigo = 2, Apagado = 3, Inactivo = 4.
    const auto actors_of = [&](ecs::Entity e, bool children) {
        std::vector<std::uint32_t> out;
        if (e == heroe) {
            out.push_back(0);
            if (children) out.push_back(1);
        } else if (e == enemigo) {
            out.push_back(2);
        } else if (e == apagado) {
            out.push_back(3);
        } else if (e == inactivo) {
            out.push_back(4);
        }
        (void)casco;
        return out;
    };
    const scene::GameOutlines o = ecs::collectOutlines(world, 0.25f, actors_of);
    check(o.actors.size() == 3, "heroe (con su hijo) y enemigo; ni apagado ni inactivo");
    check(o.styles.size() == 2, "dos estilos");
    bool hero_occluded = false;
    bool enemy_red = false;
    for (const scene::OutlinedActor& a : o.actors) {
        const scene::OutlineStyle& st = o.styles[a.style];
        if (a.actor <= 1) hero_occluded = st.mode == scene::OutlineMode::Occluded && near(st.occluded_fill, 0.5f);
        if (a.actor == 2) enemy_red = near(st.color.x, 1.0f) && near(st.color.y, 0.0f) && near(st.alpha, 0.0f, 1e-3f);
    }
    check(hero_occluded, "heroe: solo tapado, con relleno");
    check(enemy_red, "enemigo: rojo, con el pulso en su minimo a 0.25 s (2 Hz)");
    ecs::World empty;
    check(ecs::collectOutlines(empty, 0.0f, actors_of).empty(), "sin Outline: vacio");
}

void testLocalShadowLayers() {
    std::printf("Sombras de luces locales por capas\n");
    scene::LocalLightShadows shadows;
    scene::LightSet lights;
    scene::SpotLight spot{};
    spot.position = Vec3{0.0f, 5.0f, 0.0f};
    spot.shadow_layers = ~(1u << 8);
    lights.spots.push_back(spot);
    scene::PointLight point{};
    point.position = Vec3{2.0f, 3.0f, 0.0f};
    point.shadow_layers = 1u;
    lights.points.push_back(point);
    scene::Camera camera;
    shadows.update(camera, lights, 1024, 512);
    check(shadows.spots()[0].active && shadows.spots()[0].layers == ~(1u << 8), "el foco lleva sus capas");
    check(shadows.points()[0].active() && shadows.points()[0].layers == 1u, "la puntual tambien");
    shadows.update(camera, lights, 1024, 512);
    check(!shadows.spots()[0].dirty && !shadows.points()[0].dirty, "igual: el mapa sigue en cache");
    lights.spots[0].shadow_layers = scene::kAllRenderLayers;
    lights.points[0].shadow_layers = scene::kAllRenderLayers;
    shadows.update(camera, lights, 1024, 512);
    check(shadows.spots()[0].dirty && shadows.points()[0].dirty, "otras capas: se redibuja");
}

}  // namespace

int main() {
    testDefaults();
    testSerialization();
    testCameraAndCulling();
    testViewmodel();
    testOutlineStyles();
    testCollectOutlines();
    testLocalShadowLayers();
    std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

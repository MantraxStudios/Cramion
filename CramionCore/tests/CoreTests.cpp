// Pruebas del ECS de CramionCore (consola): jerarquia, transformaciones,
// duplicar, reflexion y serializacion de ida y vuelta. Devuelve 0 si todo va.

#include "CramionCore/ecs/ComponentPresets.h"
#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/MathUtil.h"
#include "CramionCore/ecs/ModelInstantiation.h"
#include "CramionCore/ecs/SceneSerializer.h"
#include "CramionCore/ecs/World.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace cramion;
using namespace cramion::ecs;

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const char* what) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("  FALLO: %s\n", what);
    }
}

bool approx(float a, float b, float eps = 1e-3f) {
    return std::abs(a - b) <= eps;
}

bool approx(const core::Vec3& a, const core::Vec3& b, float eps = 1e-3f) {
    return approx(a.x, b.x, eps) && approx(a.y, b.y, eps) && approx(a.z, b.z, eps);
}

void testHierarchy() {
    std::printf("Jerarquia y transformaciones\n");
    World world;
    Entity root = world.create("Raiz");
    Entity a = world.create("A", root);
    Entity b = world.create("B", root);
    Entity c = world.create("C", a);
    check(world.roots().size() == 1, "una raiz");
    check(root.childCount() == 2 && root.child(0) == a && root.child(1) == b, "hijos en orden");
    check(c.parent() == a && a.parent() == root, "padres");

    root.setLocalPosition({10.0f, 0.0f, 0.0f});
    a.setLocalPosition({0.0f, 5.0f, 0.0f});
    c.setLocalPosition({0.0f, 0.0f, 1.0f});
    check(approx(c.worldPosition(), {10.0f, 5.0f, 1.0f}), "posicion en el mundo compuesta");

    // La cache se invalida al mover un padre.
    root.setLocalPosition({0.0f, 0.0f, 0.0f});
    check(approx(c.worldPosition(), {0.0f, 5.0f, 1.0f}), "cache invalidada al mover el padre");

    // Rotacion: 90 grados en Y lleva +X a -Z.
    root.setLocalEulerDegrees({0.0f, 90.0f, 0.0f});
    b.setLocalPosition({1.0f, 0.0f, 0.0f});
    check(approx(b.worldPosition(), {0.0f, 0.0f, -1.0f}), "rotacion del padre aplicada");
    check(approx(root.localEulerDegrees(), {0.0f, 90.0f, 0.0f}), "euler guardados");

    // Euler <-> cuaternion ida y vuelta (orden YXZ).
    const core::Vec3 euler{30.0f, -45.0f, 60.0f};
    check(approx(quatToEulerDegrees(quatFromEulerDegrees(euler)), euler, 1e-2f), "euler ida y vuelta");

    // Reparentar conservando la posicion en el mundo.
    const core::Vec3 before = c.worldPosition();
    check(c.setParent(b, /*keep_world=*/true), "reparentar");
    check(c.parent() == b && a.childCount() == 0 && b.childCount() == 1, "hijos tras reparentar");
    check(approx(c.worldPosition(), before, 1e-3f), "posicion conservada al reparentar");

    // Ciclo prohibido.
    check(!root.setParent(c), "no se puede emparentar con un descendiente");

    // Orden de hermanos.
    Entity d = world.create("D", root);
    d.setSiblingIndex(0);
    check(root.child(0) == d && d.siblingIndex() == 0, "reordenar hermanos");

    // setWorldMatrix (gizmos).
    core::Mat4 target = core::translate(core::Vec3{3.0f, 4.0f, 5.0f});
    c.setWorldMatrix(target);
    check(approx(c.worldPosition(), {3.0f, 4.0f, 5.0f}), "setWorldMatrix");

    // Activo en la jerarquia.
    a.setActive(false);
    Entity e = world.create("E", a);
    check(!e.activeInHierarchy() && e.activeSelf(), "activo heredado");

    // Destruir es recursivo y limpia el mapa de UUID.
    const Uuid id_e = e.uuid();
    world.destroy(a);
    check(!e.valid() && !world.find(id_e).valid(), "destruccion recursiva");
    check(root.childCount() == 2, "hijos tras destruir");
}

void testDuplicateAndReflection() {
    std::printf("Duplicar y reflexion\n");
    World world;
    Entity lamp = world.create("Farola");
    Light& light = lamp.add<Light>();
    light.type = LightType::Spot;
    light.intensity = 42.0f;
    Entity bulb = world.create("Bombilla", lamp);
    bulb.setLocalPosition({0.0f, 3.0f, 0.0f});

    Entity copy = world.duplicate(lamp);
    check(copy.valid() && copy.uuid() != lamp.uuid(), "copia con uuid nuevo");
    check(copy.has<Light>() && copy.get<Light>().intensity == 42.0f, "componentes copiados");
    check(copy.childCount() == 1 && copy.child(0).uuid() != bulb.uuid(), "hijos copiados");
    check(approx(copy.child(0).worldPosition(), {0.0f, 3.0f, 0.0f}), "transform del hijo copiado");
    check(world.roots().size() == 2 && world.roots()[1] == copy.handle(), "copia junto al original");

    const ComponentType* type = ComponentRegistry::instance().find("Light");
    check(type != nullptr && type->has(world, lamp.handle()), "registro por nombre");
    int count = 0;
    for (const ComponentType& t : ComponentRegistry::instance().types()) {
        count += t.addable ? 1 : 0;
    }
    check(count >= 7, "componentes en Add Component");
}

void testSerialization() {
    std::printf("Serializacion de escenas\n");
    World world;
    world.setSceneName("Prueba");
    populateDefaultScene(world);
    Entity car = world.create("Coche");
    car.setLocalPosition({1.0f, 2.0f, 3.0f});
    car.setLocalEulerDegrees({0.0f, 45.0f, 0.0f});
    MeshRenderer& mr = car.add<MeshRenderer>();
    mr.model = assets::AssetRef{assets::builtin::kCube, assets::AssetType::Model};
    Entity wheel = createPrimitive(world, assets::builtin::kSphere, "Rueda", car);
    wheel.setLocalPosition({0.5f, 0.0f, 0.0f});
    Entity post_entity = world.findByName("Entorno");
    PostProcessing& post = post_entity.get<PostProcessing>();
    post.settings.tonemapper = gfx::Tonemapper::Aces;
    post.settings.vignette_intensity = 0.77f;

    const std::string text = serializeWorld(world);
    World loaded;
    std::string error;
    check(deserializeWorld(loaded, text, &error), "leer la escena");
    check(loaded.sceneName() == "Prueba" && loaded.sceneUuid() == world.sceneUuid(), "cabecera");
    check(loaded.entityCount() == world.entityCount(), "mismo numero de entidades");

    Entity car2 = loaded.find(car.uuid());
    check(car2.valid() && car2.name() == "Coche", "entidad por uuid");
    check(approx(car2.worldPosition(), {1.0f, 2.0f, 3.0f}), "posicion");
    check(approx(car2.localEulerDegrees(), {0.0f, 45.0f, 0.0f}), "rotacion");
    check(car2.has<MeshRenderer>() && car2.get<MeshRenderer>().model.uuid == assets::builtin::kCube,
          "referencia a asset");
    Entity wheel2 = loaded.find(wheel.uuid());
    check(wheel2.valid() && wheel2.parent() == car2, "jerarquia");
    check(approx(wheel2.worldPosition(), wheel.worldPosition(), 1e-3f), "transform del hijo");
    Entity post2 = loaded.find(post_entity.uuid());
    check(post2.valid() && post2.get<PostProcessing>().settings.tonemapper == gfx::Tonemapper::Aces &&
              approx(post2.get<PostProcessing>().settings.vignette_intensity, 0.77f),
          "post-procesado");
    check(serializeWorld(loaded) == text, "ida y vuelta identica");

    // Copiar y pegar (UUIDs nuevos).
    const std::string clip = serializeEntity(world, car);
    Entity pasted = pasteEntities(world, clip);
    check(pasted.valid() && pasted.uuid() != car.uuid() && pasted.childCount() == 1, "pegar");

    // Campos desconocidos y que faltan.
    const std::string odd = R"({"format":"CramionScene","version":1,"name":"X","entities":[
        {"uuid":"12345678-1234-4234-8234-123456789abc","name":"Raro","parent":null,
         "components":{"Transform":{"position":[1,2,3],"campo_nuevo":5},"NoExiste":{}}}]})";
    World odd_world;
    check(deserializeWorld(odd_world, odd), "escena con campos raros");
    Entity raro = odd_world.findByName("Raro");
    check(raro.valid() && approx(raro.worldPosition(), {1.0f, 2.0f, 3.0f}), "campos parciales");
}

void testModelInstantiation() {
    std::printf("Instanciar modelos\n");
    World world;
    assets::ModelAsset model;
    model.uuid = Uuid::generate();
    model.name = "Coche";
    model.nodes.push_back(assets::ModelNode{"root", -1, core::Mat4::identity(), -1});
    model.nodes.push_back(assets::ModelNode{"Carroceria", 0, core::translate(core::Vec3{0.0f, 1.0f, 0.0f}), 0});
    model.nodes.push_back(assets::ModelNode{"Rueda", 1, core::translate(core::Vec3{1.0f, -0.5f, 0.0f}), 1});
    model.parts.resize(2);
    Entity root = instantiateModel(world, model);
    check(root.name() == "Coche" && root.childCount() == 1, "raiz del modelo");
    Entity body = root.child(0);
    check(body.has<MeshRenderer>() && body.get<MeshRenderer>().part == 0, "pieza 0");
    Entity wheel = body.child(0);
    check(wheel.valid() && wheel.get<MeshRenderer>().part == 1, "pieza 1");
    check(approx(wheel.worldPosition(), {1.0f, 0.5f, 0.0f}), "transforms de los nodos");
}

void testPostVolumes() {
    std::printf("Volumenes de post-proceso\n");
    World world;
    Entity global = world.create("Global");
    PostProcessing& g = global.add<PostProcessing>();
    g.settings.saturation = 1.0f;
    g.settings.vignette_intensity = 0.2f;

    Entity box = world.create("Caja");
    box.setWorldPosition(core::Vec3{10.0f, 0.0f, 0.0f});
    box.setLocalScale(core::Vec3{2.0f, 2.0f, 2.0f});  // caja de 4 x 4 x 4 m
    PostProcessing& b = box.add<PostProcessing>();
    b.shape = PostVolumeShape::Box;
    b.size = core::Vec3{2.0f, 2.0f, 2.0f};
    b.blend_distance = 2.0f;
    b.priority = 1;
    b.overrides = kPostColor;
    b.settings.saturation = 0.0f;
    b.settings.vignette_intensity = 1.0f;  // no la sobrescribe

    const core::Mat4& m = box.worldMatrix();
    check(std::abs(b.influence(m, core::Vec3{10.0f, 0.0f, 0.0f}) - 1.0f) < 1e-4f, "dentro de la caja: 1");
    check(std::abs(b.influence(m, core::Vec3{11.9f, 0.0f, 0.0f}) - 1.0f) < 1e-4f, "caja escalada: el borde a 2 m");
    const float half = b.influence(m, core::Vec3{13.0f, 0.0f, 0.0f});
    check(half > 0.4f && half < 0.6f, "a mitad de la mezcla: ~0.5");
    check(b.influence(m, core::Vec3{14.5f, 0.0f, 0.0f}) == 0.0f, "fuera de la mezcla: 0");

    Entity sphere = world.create("Esfera");
    PostProcessing& sp = sphere.add<PostProcessing>();
    sp.shape = PostVolumeShape::Sphere;
    sp.radius = 3.0f;
    sp.blend_distance = 0.0f;
    check(sp.influence(sphere.worldMatrix(), core::Vec3{0.0f, 2.9f, 0.0f}) == 1.0f &&
              sp.influence(sphere.worldMatrix(), core::Vec3{0.0f, 3.1f, 0.0f}) == 0.0f,
          "esfera sin mezcla: de golpe");

    // Mezcla: el global lo pone todo; la caja solo el color.
    gfx::PostProcessSettings out{};
    blendPostProcess(out, g.settings, 1.0f, kPostAll);
    blendPostProcess(out, b.settings, 0.5f, b.overrides);
    check(std::abs(out.saturation - 0.5f) < 1e-4f, "saturacion a medias entre global y caja");
    check(std::abs(out.vignette_intensity - 0.2f) < 1e-4f, "lo no sobrescrito sale del global");

    // Se guarda y se lee con la escena.
    const std::string text = serializeWorld(world);
    World loaded;
    check(deserializeWorld(loaded, text), "escena con volumenes");
    const Entity again = loaded.findByName("Caja");
    const PostProcessing* read = again.valid() ? again.tryGet<PostProcessing>() : nullptr;
    check(read != nullptr && read->shape == PostVolumeShape::Box && read->overrides == kPostColor &&
              std::abs(read->blend_distance - 2.0f) < 1e-4f,
          "forma, mezcla y sobrescrituras guardadas");
    const Entity old = loaded.findByName("Global");
    check(old.valid() && old.get<PostProcessing>().isGlobal(), "el global sigue global");
}

void testUltraRealisticPreset() {
    std::printf("Preset Ultra realista\n");
    const std::vector<ComponentPreset> presets = builtinPresets("PostProcessing");
    const ComponentPreset* ultra = nullptr;
    for (const ComponentPreset& p : presets) {
        if (p.name == "Ultra realista") ultra = &p;
    }
    check(ultra != nullptr, "el preset existe");
    if (ultra == nullptr) return;
    World world;
    Entity e = world.create("Post");
    std::string error;
    check(applyPreset(world, e, *ultra, &error), "se aplica");
    const gfx::PostProcessSettings& s = e.get<PostProcessing>().settings;
    check(s.auto_exposure && s.bloom && s.vignette && s.motion_blur && s.depth_of_field && s.dof_auto_focus &&
              s.light_shafts && s.fxaa && s.ambient_occlusion && s.global_illumination && s.reflections &&
              s.contact_shadows && s.volumetric_light && s.lods,
          "todos los efectos prendidos");
    check(s.film_grain > 0.0f && s.chromatic_aberration > 0.0f && s.lens_flare > 0.0f && s.fog_density > 0.0f,
          "lente y niebla activas");
    check(s.tonemapper == gfx::Tonemapper::Agx && s.lod_pixel_error < 1.0f, "AgX y LODs finos");
}

}  // namespace

int main() {
    testHierarchy();
    testDuplicateAndReflection();
    testSerialization();
    testModelInstantiation();
    testPostVolumes();
    testUltraRealisticPreset();
    std::printf("%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

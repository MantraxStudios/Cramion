// Pruebas de native/MeshApi.cpp: Mesh (crear, listas, submallas, materiales,
// clone), los handles (el mismo para la misma malla; se sueltan al parar) y
// Entity.mesh.

#include "ApiTest.h"

#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/RuntimeMesh.h"

#include <cmath>
#include <functional>
#include <memory>

using namespace cramion;
using namespace cramion::apitest;
using core::Vec3;

namespace {

bool near(const Vec3& a, const Vec3& b, float eps = 1e-4f) { return core::length(a - b) < eps; }

Value vec3List(std::initializer_list<Vec3> list) {
    Value::Array out;
    for (const Vec3& v : list) out.emplace_back(v);
    return Value(std::move(out));
}

Value numbers(std::initializer_list<double> list) {
    Value::Array out;
    for (const double d : list) out.emplace_back(d);
    return Value(std::move(out));
}

bool throws(const std::function<void()>& fn, const std::string& part) {
    try {
        fn();
    } catch (const scripting::api::Error& e) {
        return std::string(e.what()).find(part) != std::string::npos;
    }
    return false;
}

void testCreate(ApiFixture& t) {
    std::printf("Crear y editar\n");
    const Value m = t.call("Mesh.new", {Value("Rampa")});
    check(m.isHandle() && m.asHandle()->typeName() == "Mesh", "Mesh.new da un handle Mesh");
    check(t.get("name", m).asString() == "Rampa", "name");
    t.set("vertices", vec3List({{-5, 0, -5}, {5, 0, -5}, {5, 0, 5}, {-5, 0, 5}}), m);
    t.set("uv", vec3List({{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}}), m);
    t.set("triangles", numbers({0, 2, 1, 0, 3, 2}), m);
    t.call("recalculateNormals", {}, m);
    check(t.get("vertexCount", m).asNumber() == 4 && t.get("triangleCount", m).asNumber() == 2, "vertexCount y triangleCount");
    const Value normals = t.get("normals", m);
    check(normals.size() == 4 && near(normals[0].asVec3(), Vec3{0, 1, 0}), "recalculateNormals: el suelo mira a +Y");
    const Value uv = t.get("uv", m);
    check(uv.size() == 4 && near(uv[2].asVec3(), Vec3{1, 1, 0}), "uv como Vec3 (x, y)");
    const Value tris = t.get("triangles", m);
    check(tris.size() == 6 && tris[1].asNumber() == 2 && tris[5].asNumber() == 2, "triangles (desde 0)");
    check(t.call("validate", {}, m).asString().empty(), "validate: se puede dibujar");
    check(near(t.get("boundsMin", m).asVec3(), Vec3{-5, 0, -5}) && near(t.get("boundsMax", m).asVec3(), Vec3{5, 0, 5}),
          "boundsMin y boundsMax");

    // Un vertice suelto (desde 0) y fuera de rango.
    t.call("setVertex", {Value(1), Value(Vec3{6, 0, -5})}, m);
    t.call("setVertex", {Value(99), Value(Vec3{1, 1, 1})}, m);
    check(near(t.call("getVertex", {Value(1)}, m).asVec3(), Vec3{6, 0, -5}) &&
              near(t.call("getVertex", {Value(99)}, m).asVec3(), Vec3{}),
          "setVertex / getVertex (fuera de rango = cero)");
    t.call("recalculateBounds", {}, m);
    check(std::abs(t.get("boundsMax", m).asVec3().x - 6.0f) < 1e-5f, "recalculateBounds");

    // Submallas.
    t.call("setTriangles", {numbers({0, 2, 1}), Value(0)}, m);
    t.call("setTriangles", {numbers({0, 3, 2}), Value(1)}, m);
    check(t.get("subMeshCount", m).asNumber() == 2 && t.call("getTriangles", {Value(1)}, m).size() == 3 &&
              t.call("getTriangles", {}, m).size() == 3,
          "setTriangles / getTriangles por submalla");
    t.set("subMeshCount", Value(1), m);
    check(t.get("subMeshCount", m).asNumber() == 1, "subMeshCount se puede cambiar");

    // Tangentes (Vec3; w = +1).
    t.call("recalculateTangents", {}, m);
    check(t.get("tangents", m).size() == 4, "recalculateTangents");
    t.set("tangents", vec3List({{1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}}), m);
    check(near(t.get("tangents", m)[3].asVec3(), Vec3{1, 0, 0}), "tangents como lista de Vec3");

    // Errores claros.
    check(throws([&] { t.set("vertices", numbers({1, 2, 3}), m); }, "no es un Vec3"), "vertices con numeros: error");
    check(throws([&] { t.set("triangles", vec3List({{0, 0, 0}}), m); }, "no es un numero"), "triangles con Vec3: error");
    const Value bad = t.call("Mesh.new");
    t.set("vertices", vec3List({{0, 0, 0}}), bad);
    t.set("triangles", numbers({0, 1, 2}), bad);
    check(t.call("validate", {}, bad).asString().find("vertice 1") != std::string::npos, "validate explica que falla");
    t.call("clear", {}, bad);
    check(t.get("vertexCount", bad).asNumber() == 0, "clear la vacia");

    // clone: otra malla (otro handle) con lo mismo.
    const Value copy = t.call("clone", {}, m);
    t.call("setVertex", {Value(0), Value(Vec3{-7, 0, -5})}, copy);
    check(copy.isHandle() && copy.asHandle() != m.asHandle() && t.get("vertexCount", copy).asNumber() == 4 &&
              near(t.call("getVertex", {Value(0)}, m).asVec3(), Vec3{-5, 0, -5}),
          "clone es una copia independiente");
}

void testPrimitives(ApiFixture& t) {
    std::printf("Primitivas\n");
    const Value cube = t.call("Mesh.cube", {Value(Vec3{2, 1, 1})});
    check(t.get("vertexCount", cube).asNumber() == 24 && t.get("triangleCount", cube).asNumber() == 12 &&
              std::abs(t.get("boundsMin", cube).asVec3().x + 1.0f) < 1e-5f,
          "Mesh.cube(Vec3)");
    const Value unit = t.call("Mesh.cube", {Value(3)});
    check(std::abs(t.get("boundsMax", unit).asVec3().y - 1.5f) < 1e-5f, "Mesh.cube(numero)");
    check(t.get("vertexCount", t.call("Mesh.quad")).asNumber() == 4, "Mesh.quad()");
    check(t.get("vertexCount", t.call("Mesh.plane", {Value(4), Value(4), Value(3), Value(3)})).asNumber() == 16,
          "Mesh.plane con segmentos");
    check(t.get("vertexCount", t.call("Mesh.plane", {Value(60), Value(60), Value(80), Value(80)})).asNumber() == 6561 &&
              t.get("triangleCount", t.call("Mesh.plane", {Value(60), Value(60), Value(80), Value(80)})).asNumber() == 12800,
          "Mesh.plane del ejemplo de terreno (6561 vertices, 12800 triangulos)");
    const Value sphere = t.call("Mesh.sphere", {Value(1), Value(16), Value(8)});
    bool on_sphere = true;
    for (const Value& p : t.get("vertices", sphere).items()) on_sphere = on_sphere && std::abs(core::length(p.asVec3()) - 1.0f) < 1e-4f;
    check(on_sphere, "Mesh.sphere: los vertices a su radio");
    check(t.call("validate", {}, t.call("Mesh.cylinder")).asString().empty() &&
              t.call("validate", {}, t.call("Mesh.capsule", {Value(0.5), Value(2)})).asString().empty(),
          "Mesh.cylinder y Mesh.capsule");
    check(t.get("triangleCount", t.call("Mesh.wireCube", {Value(1), Value(0.02)})).asNumber() == 144, "Mesh.wireCube: 12 barras");
}

void testMaterials(ApiFixture& t) {
    std::printf("Materiales\n");
    const Value m = t.call("Mesh.cube", {Value(1)});
    ecs::Entity e = t.world.create("Efectos");
    t.set("mesh", m, ApiFixture::entity(e));
    const std::shared_ptr<ecs::Mesh> mesh = e.has<ecs::MeshRenderer>() ? e.get<ecs::MeshRenderer>().mesh : nullptr;
    check(mesh != nullptr, "la malla en el MeshRenderer (entity.mesh)");
    if (!mesh) return;
    Value mat = Value::object();
    mat.set("color", Vec3{1, 0, 0});
    mat.set("emission", Vec3{1, 1, 0});
    mat.set("emissionIntensity", 3);
    t.call("setMaterial", {Value(0), mat}, m);
    const std::uint64_t v1 = mesh->version(), mv1 = mesh->materialVersion();
    Value glow = Value::object();
    glow.set("emissionIntensity", 5);
    glow.set("alpha", 0.5);
    t.call("setMaterial", {Value(0), glow}, m);  // mismo hueco ya creado
    check(mesh->version() == v1 && mesh->materialVersion() > mv1 && mesh->materials[0].emission_intensity == 5.0f &&
              mesh->materials[0].color.x == 1.0f && mesh->materials[0].color.w == 0.5f,
          "cambiar el brillo o el color no vuelve a subir la malla");
    Value tex = Value::object();
    tex.set("texture", "Textures/rejilla.png");
    tex.set("tiling", Vec3{4, 4, 0});
    t.call("setMaterial", {Value(0), tex}, m);
    check(mesh->version() > v1 && mesh->materials[0].texture == "Textures/rejilla.png" && mesh->materials[0].tiling.x == 4.0f,
          "cambiar la textura o la repeticion si la vuelve a subir");
    const Value got = t.call("getMaterial", {Value(0)}, m);
    check(got["texture"].asString() == "Textures/rejilla.png" && got["emissionIntensity"].asNumber() == 5.0 &&
              near(got["color"].asVec3(), Vec3{1, 0, 0}) && near(got["tiling"].asVec3(), Vec3{4, 4, 0}),
          "getMaterial devuelve el material");
    Value off = Value::object();
    off.set("texture", false);
    t.call("setMaterial", {Value(0), off}, m);
    check(mesh->materials[0].texture.empty(), "texture = false la quita");
    check(t.call("getMaterial", {Value(-1)}, m).isNil() && t.call("getMaterial", {Value(3)}, m)["roughness"].asNumber() > 0.5,
          "getMaterial de un hueco sin material: el de por defecto (negativo = nil)");
    t.call("setMaterial", {Value(2), Value::object()}, m);
    check(mesh->materials.size() == 3, "setMaterial crea el hueco");
}

void testHandles() {
    std::printf("Handles y Entity.mesh\n");
    ApiFixture t;
    ecs::Entity e = t.world.create("Suelo");
    const Value m = t.call("Mesh.sphere", {Value(0.5)});
    check(t.api().find("Entity:mesh") != nullptr, "Entity.mesh registrada");
    t.set("mesh", m, ApiFixture::entity(e));
    check(e.has<ecs::MeshRenderer>() && e.get<ecs::MeshRenderer>().mesh &&
              e.get<ecs::MeshRenderer>().mesh->vertices.size() == static_cast<std::size_t>(t.get("vertexCount", m).asNumber()),
          "entity.mesh anade el MeshRenderer con esa malla");
    const Value back = t.get("mesh", ApiFixture::entity(e));
    check(back.isHandle() && back.asHandle() == m.asHandle(), "la misma malla da el mismo handle");
    // Por el puente: el mismo id cada vez.
    const json a = t.bridge({{"op", "get"}, {"self", {{"$e", entt::to_integral(e.handle()) + 1}}}, {"key", "mesh"}});
    const json b = t.bridge({{"op", "get"}, {"self", {{"$e", entt::to_integral(e.handle()) + 1}}}, {"key", "mesh"}});
    check(a["ok"] == true && a["result"].contains("$h") && a["result"] == b["result"], "por el puente: el mismo $h");
    const json count = t.bridge({{"op", "get"}, {"self", a["result"]}, {"key", "vertexCount"}});
    check(count["ok"] == true && count["result"].get<double>() > 100, "metodos del handle por el puente");
    const json created = t.bridge({{"fn", "Mesh.cube"}, {"args", {2}}});
    const json set = t.bridge({{"op", "set"},
                               {"self", created["result"]},
                               {"key", "vertices"},
                               {"value", {{{"$v", {0, 0, 0}}}, {{"$v", {1, 0, 0}}}, {{"$v", {0, 1, 0}}}}}});
    const json tris = t.bridge({{"op", "call"}, {"fn", "setTriangles"}, {"self", created["result"]}, {"args", {{0, 1, 2}}}});
    const json valid = t.bridge({{"fn", "validate"}, {"self", created["result"]}});
    check(set["ok"] == true && tris["ok"] == true && valid["ok"] == true && valid["result"] == "",
          "crear y editar por el puente (JSON del SDK)");
    const json wrong = t.bridge({{"op", "set"}, {"self", created["result"]}, {"key", "vertices"}, {"value", {1, 2}}});
    check(wrong["ok"] == false && wrong["error"].get<std::string>().find("no es un Vec3") != std::string::npos,
          "un error por el puente");
    const json typeErr = t.bridge({{"op", "set"}, {"self", {{"$e", entt::to_integral(e.handle()) + 1}}}, {"key", "mesh"}, {"value", 3}});
    check(typeErr["ok"] == false, "entity.mesh = 3: error");

    // Quitarla: nil la quita; en un objeto sin MeshRenderer no anade nada.
    t.set("mesh", Value(), ApiFixture::entity(e));
    check(e.has<ecs::MeshRenderer>() && !e.get<ecs::MeshRenderer>().mesh, "entity.mesh = nil la quita");
    ecs::Entity other = t.world.create("Vacio");
    t.set("mesh", Value(), ApiFixture::entity(other));
    check(!other.has<ecs::MeshRenderer>() && t.get("mesh", ApiFixture::entity(other)).isNil(),
          "sin MeshRenderer: nil y no se anade");

    // Propiedad compartida: la malla vive mientras la tenga un handle (hasta
    // parar el juego) o un MeshRenderer. Por el puente, sin Values de prueba.
    const json self = {{"$e", entt::to_integral(other.handle()) + 1}};
    const json loose = t.bridge({{"fn", "Mesh.cube"}, {"args", {1}}});
    const json kept = t.bridge({{"fn", "Mesh.quad"}});
    t.bridge({{"op", "set"}, {"self", self}, {"key", "mesh"}, {"value", loose["result"]}});
    const std::weak_ptr<ecs::Mesh> loose_weak = other.get<ecs::MeshRenderer>().mesh;
    t.bridge({{"op", "set"}, {"self", self}, {"key", "mesh"}, {"value", kept["result"]}});
    const std::weak_ptr<ecs::Mesh> kept_weak = other.get<ecs::MeshRenderer>().mesh;
    check(!loose_weak.expired() && loose_weak.lock() != kept_weak.lock(), "quitada del objeto, la sigue teniendo su handle");
    t.scripts.stop();
    t.scripts.start(t.world);
    check(loose_weak.expired(), "al parar el juego se suelta la que solo tenia el script");
    check(!kept_weak.expired() && other.get<ecs::MeshRenderer>().mesh == kept_weak.lock(),
          "la del MeshRenderer sigue viva (propiedad compartida)");
    const json gone = t.bridge({{"op", "get"}, {"self", kept["result"]}, {"key", "vertexCount"}});
    check(gone["ok"] == false && gone["error"].get<std::string>().find("ya no existe") != std::string::npos,
          "un handle de antes de parar ya no vale");
    const json again = t.bridge({{"op", "get"}, {"self", self}, {"key", "mesh"}});
    const json quad = t.bridge({{"op", "get"}, {"self", again["result"]}, {"key", "vertexCount"}});
    check(quad["ok"] == true && quad["result"] == 4, "entity.mesh da un handle nuevo de la misma malla");
    // Destruida la entidad: error claro.
    t.world.destroy(other);
    const json dead = t.bridge({{"op", "get"}, {"self", {{"$e", entt::to_integral(other.handle()) + 1}}}, {"key", "mesh"}});
    check(dead["ok"] == false, "entity.mesh de una entidad destruida: error");
}

}  // namespace

int main() {
    std::printf("Mesh\n");
    {
        ApiFixture t;
        testCreate(t);
        testPrimitives(t);
        testMaterials(t);
    }
    testHandles();
    return finish();
}

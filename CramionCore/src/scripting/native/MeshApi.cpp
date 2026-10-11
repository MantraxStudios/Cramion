// Mesh: mallas creadas por codigo, como el Mesh de Unity. Los triangulos usan
// indices de vertice desde 0 (como Unity). Las UV van en Vec3 (x, y). Cambiar
// una lista entera (mesh.vertices = {...}) o llamar a mesh:apply() la sube a
// la GPU en el siguiente frame.
//
// Cada malla es un handle "Mesh" que comparte la ecs::Mesh (shared_ptr) con
// el MeshRenderer que la dibuja: la misma malla da siempre el mismo handle.

#include "Modules.h"

#include "CramionCore/ecs/RuntimeMesh.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace cramion::scripting::native {

namespace {

using core::Vec3;

struct MeshHandle final : api::Handle {
    explicit MeshHandle(std::shared_ptr<ecs::Mesh> m) : mesh(std::move(m)) {}
    std::string_view typeName() const override { return "Mesh"; }
    std::shared_ptr<ecs::Mesh> mesh;
};

// El `self` de un metodo o propiedad de Mesh (lo mantiene vivo su handle).
ecs::Mesh& selfMesh(const api::Call& c) {
    const std::shared_ptr<MeshHandle> h = c.self().as<MeshHandle>();
    if (!h || !h->mesh) throw api::Error("se esperaba un Mesh");
    return *h->mesh;
}

std::size_t indexOf(int i) { return static_cast<std::size_t>(i); }

api::Value vec3List(const std::vector<Vec3>& in) {
    api::Value::Array out;
    out.reserve(in.size());
    for (const Vec3& v : in) out.emplace_back(v);
    return api::Value(std::move(out));
}

api::Value indexList(const std::vector<std::uint32_t>& in) {
    api::Value::Array out;
    out.reserve(in.size());
    for (const std::uint32_t i : in) out.emplace_back(i);
    return api::Value(std::move(out));
}

// Una lista de Vec3 (el argumento i; nil = vacia).
std::vector<Vec3> readVec3(const api::Call& c, std::size_t i) {
    const api::Value::Array& items = c.list(i).items();
    std::vector<Vec3> out;
    out.reserve(items.size());
    for (std::size_t k = 0; k < items.size(); ++k) {
        if (!items[k].isVec3()) throw api::Error("el elemento " + std::to_string(k + 1) + " de la lista no es un Vec3");
        out.push_back(items[k].asVec3());
    }
    return out;
}

// Indices de vertice (desde 0); los negativos, 0xFFFFFFFF (invalidos).
std::vector<std::uint32_t> readIndices(const api::Call& c, std::size_t i) {
    const api::Value::Array& items = c.list(i).items();
    std::vector<std::uint32_t> out;
    out.reserve(items.size());
    for (std::size_t k = 0; k < items.size(); ++k) {
        if (!items[k].isNumber()) throw api::Error("el elemento " + std::to_string(k + 1) + " de la lista no es un numero");
        const double v = items[k].asNumber();
        out.push_back(v < 0.0 ? 0xFFFFFFFFu : static_cast<std::uint32_t>(v));
    }
    return out;
}

// Mesh.cube(1) o Mesh.cube(Vec3(2, 1, 1)); nil = 1 x 1 x 1.
Vec3 sizeArg(const api::Call& c, std::size_t i) {
    const api::Value& v = c.arg(i);
    if (v.isVec3()) return v.asVec3();
    if (v.isNumber()) return Vec3{1.0f, 1.0f, 1.0f} * static_cast<float>(v.asNumber());
    return Vec3{1.0f, 1.0f, 1.0f};
}

// Material de una submalla (sin .crmat): {color, alpha, metallic, roughness,
// emission, emissionIntensity, normalStrength, texture, normalMap,
// emissionMap, tiling, offset}. Solo cambia lo que venga.
void setMaterial(ecs::Mesh& m, int submesh, const api::Value& t) {
    if (submesh < 0) return;
    bool layout = false;  // texturas o huecos nuevos: hay que volver a subirla
    if (m.materials.size() <= indexOf(submesh)) {
        m.materials.resize(indexOf(submesh) + 1);
        layout = true;
    }
    ecs::MeshMaterial& mat = m.materials[indexOf(submesh)];
    const auto number = [&](const char* key, float& field) {
        if (t[key].isNumber()) field = static_cast<float>(t[key].asNumber());
    };
    if (t["color"].isVec3()) {
        const Vec3 c = t["color"].asVec3();
        mat.color = core::Vec4{c.x, c.y, c.z, mat.color.w};
    }
    number("alpha", mat.color.w);
    number("metallic", mat.metallic);
    number("roughness", mat.roughness);
    if (t["emission"].isVec3()) mat.emission = t["emission"].asVec3();
    number("emissionIntensity", mat.emission_intensity);
    number("normalStrength", mat.normal_strength);
    const auto text = [&](const char* key, std::string& field) {
        const api::Value& o = t[key];
        if (o.isString() && o.asString() != field) {
            field = o.asString();
            layout = true;
        } else if (o.isBool() && !o.truthy() && !field.empty()) {
            field.clear();  // false = quitarla
            layout = true;
        }
    };
    text("texture", mat.texture);
    text("normalMap", mat.normal_map);
    text("emissionMap", mat.emission_map);
    if (t["tiling"].isVec3()) {
        const Vec3 v = t["tiling"].asVec3();
        mat.tiling = core::Vec2{v.x, v.y};
        layout = true;
    }
    if (t["offset"].isVec3()) {
        const Vec3 v = t["offset"].asVec3();
        mat.offset = core::Vec2{v.x, v.y};
        layout = true;
    }
    if (layout) m.markModified();
    else m.markMaterialsModified();  // efectos por frame: sin volver a subir la malla
}

api::Value getMaterial(const ecs::Mesh& m, int submesh) {
    if (submesh < 0) return {};
    const ecs::MeshMaterial mat = indexOf(submesh) < m.materials.size() ? m.materials[indexOf(submesh)] : ecs::MeshMaterial{};
    api::Value t = api::Value::object();
    t.set("color", Vec3{mat.color.x, mat.color.y, mat.color.z});
    t.set("alpha", mat.color.w);
    t.set("metallic", mat.metallic);
    t.set("roughness", mat.roughness);
    t.set("emission", mat.emission);
    t.set("emissionIntensity", mat.emission_intensity);
    t.set("normalStrength", mat.normal_strength);
    t.set("texture", mat.texture);
    t.set("normalMap", mat.normal_map);
    t.set("emissionMap", mat.emission_map);
    t.set("tiling", Vec3{mat.tiling.x, mat.tiling.y, 0.0f});
    t.set("offset", Vec3{mat.offset.x, mat.offset.y, 0.0f});
    return t;
}

}  // namespace

api::Value meshValue(Runtime& rt, std::shared_ptr<ecs::Mesh> mesh) {
    if (!mesh) return {};
    const void* key = mesh.get();
    return api::Value::handle(rt.native.intern<MeshHandle>(key, [&] { return std::make_shared<MeshHandle>(std::move(mesh)); }));
}

std::shared_ptr<ecs::Mesh> meshOf(const api::Value& v) {
    const std::shared_ptr<MeshHandle> h = v.as<MeshHandle>();
    return h ? h->mesh : nullptr;
}

void registerMeshApi(Runtime& rt) {
    api::NativeApi& api = rt.native;
    const auto prop = [&api](const char* name, api::Function get, api::Function set, api::Doc doc) {
        api.property("Mesh", name, std::move(get), std::move(set), std::move(doc), true);
    };

    // --- Crear ---
    api.function("Mesh.new", [&rt](api::Call& c) {
        auto m = std::make_shared<ecs::Mesh>();
        if (c.has(0)) m->name = c.string(0);
        return meshValue(rt, m);
    }, {"\"nombre\"", "malla vacia", "Mesh"});
    api.function("Mesh.cube", [&rt](api::Call& c) { return meshValue(rt, ecs::Mesh::cube(sizeArg(c, 0))); },
                 {"tamano", "cubo (numero o Vec3)", "Mesh"});
    api.function("Mesh.quad", [&rt](api::Call& c) {
        const float w = static_cast<float>(c.number(0, 1.0));
        return meshValue(rt, ecs::Mesh::quad(w, static_cast<float>(c.number(1, w))));
    }, {"ancho, alto", "cuadrado en XY", "Mesh"});
    api.function("Mesh.plane", [&rt](api::Call& c) {
        const float w = static_cast<float>(c.number(0, 10.0));
        const int sx = static_cast<int>(c.integer(2, 10));
        return meshValue(rt, ecs::Mesh::plane(w, static_cast<float>(c.number(1, w)), sx, static_cast<int>(c.integer(3, sx))));
    }, {"ancho, fondo, segX, segZ", "plano subdividido", "Mesh"});
    api.function("Mesh.sphere", [&rt](api::Call& c) {
        return meshValue(rt, ecs::Mesh::sphere(static_cast<float>(c.number(0, 0.5)), static_cast<int>(c.integer(1, 32)),
                                               static_cast<int>(c.integer(2, 16))));
    }, {"radio, segmentos, anillos", "esfera", "Mesh"});
    api.function("Mesh.cylinder", [&rt](api::Call& c) {
        return meshValue(rt, ecs::Mesh::cylinder(static_cast<float>(c.number(0, 0.5)), static_cast<float>(c.number(1, 2.0)),
                                                 static_cast<int>(c.integer(2, 32))));
    }, {"radio, alto, segmentos", "cilindro", "Mesh"});
    api.function("Mesh.wireCube", [&rt](api::Call& c) {
        return meshValue(rt, ecs::Mesh::wireCube(sizeArg(c, 0), static_cast<float>(c.number(1, 0.02))));
    }, {"tamano, grosor", "aristas de una caja (contornos)", "Mesh"});
    api.function("Mesh.capsule", [&rt](api::Call& c) {
        return meshValue(rt, ecs::Mesh::capsule(static_cast<float>(c.number(0, 0.5)), static_cast<float>(c.number(1, 2.0)),
                                                static_cast<int>(c.integer(2, 24))));
    }, {"radio, alto, segmentos", "capsula", "Mesh"});

    // --- Propiedades ---
    prop("name", [](api::Call& c) { return api::Value(selfMesh(c).name); },
         [](api::Call& c) { selfMesh(c).name = c.string(0); return api::Value{}; }, {"", "nombre", "texto"});
    prop("vertexCount", [](api::Call& c) { return api::Value(static_cast<int>(selfMesh(c).vertices.size())); }, {},
         {"", "vertices", "numero"});
    prop("triangleCount", [](api::Call& c) { return api::Value(static_cast<int>(selfMesh(c).triangleCount())); }, {},
         {"", "triangulos", "numero"});
    prop("subMeshCount", [](api::Call& c) { return api::Value(selfMesh(c).subMeshCount()); },
         [](api::Call& c) { selfMesh(c).setSubMeshCount(static_cast<int>(c.integer(0))); return api::Value{}; },
         {"", "submallas (materiales)", "numero"});
    prop("vertices", [](api::Call& c) { return vec3List(selfMesh(c).vertices); },
         [](api::Call& c) {
             ecs::Mesh& m = selfMesh(c);
             m.vertices = readVec3(c, 0);
             m.markModified();
             return api::Value{};
         },
         {"", "lista de Vec3", "lista de Vec3"});
    prop("normals", [](api::Call& c) { return vec3List(selfMesh(c).normals); },
         [](api::Call& c) {
             ecs::Mesh& m = selfMesh(c);
             m.normals = readVec3(c, 0);
             m.markModified();
             return api::Value{};
         },
         {"", "lista de Vec3", "lista de Vec3"});
    prop("uv",
         [](api::Call& c) {
             const ecs::Mesh& m = selfMesh(c);
             api::Value::Array out;
             out.reserve(m.uv.size());
             for (const core::Vec2& v : m.uv) out.emplace_back(Vec3{v.x, v.y, 0.0f});
             return api::Value(std::move(out));
         },
         [](api::Call& c) {
             ecs::Mesh& m = selfMesh(c);
             const std::vector<Vec3> list = readVec3(c, 0);
             m.uv.clear();
             for (const Vec3& v : list) m.uv.push_back(core::Vec2{v.x, v.y});
             m.markModified();
             return api::Value{};
         },
         {"", "lista de {x, y}", "lista de Vec3"});
    // Tangentes: Vec3 (+U); el signo de la bitangente, +1.
    prop("tangents",
         [](api::Call& c) {
             const ecs::Mesh& m = selfMesh(c);
             api::Value::Array out;
             out.reserve(m.tangents.size());
             for (const core::Vec4& t : m.tangents) out.emplace_back(Vec3{t.x, t.y, t.z});
             return api::Value(std::move(out));
         },
         [](api::Call& c) {
             ecs::Mesh& m = selfMesh(c);
             const std::vector<Vec3> list = readVec3(c, 0);
             m.tangents.clear();
             for (const Vec3& v : list) m.tangents.push_back(core::Vec4{v.x, v.y, v.z, 1.0f});
             m.markModified();
             return api::Value{};
         },
         {"", "lista de Vec3", "lista de Vec3"});
    // Los de la submalla 0 (como mesh.triangles de Unity).
    prop("triangles", [](api::Call& c) { return indexList(selfMesh(c).triangles(0)); },
         [](api::Call& c) {
             ecs::Mesh& m = selfMesh(c);
             m.setTriangles(readIndices(c, 0), 0);
             m.markModified();
             return api::Value{};
         },
         {"", "indices (submalla 0)", "lista de numeros"});
    prop("boundsMin", [](api::Call& c) { return api::Value(selfMesh(c).boundsMin()); }, {},
         {"", "Vec3 esquina minima", "Vec3"});
    prop("boundsMax", [](api::Call& c) { return api::Value(selfMesh(c).boundsMax()); }, {},
         {"", "Vec3 esquina maxima", "Vec3"});

    // --- Metodos ---
    api.method("Mesh", "setTriangles", [](api::Call& c) {
        ecs::Mesh& m = selfMesh(c);
        m.setTriangles(readIndices(c, 0), static_cast<int>(c.integer(1, 0)));
        m.markModified();
        return api::Value{};
    }, {"indices, submalla", "triangulos de una submalla"});
    api.method("Mesh", "getTriangles", [](api::Call& c) {
        return indexList(selfMesh(c).triangles(static_cast<int>(c.integer(0, 0))));
    }, {"submalla", "sus indices", "lista de numeros"});
    // Un vertice suelto (indice desde 0): despues, mesh:apply().
    api.method("Mesh", "setVertex", [](api::Call& c) {
        ecs::Mesh& m = selfMesh(c);
        const long long index = c.integer(0);
        const Vec3 p = c.vec3(1);
        if (index >= 0 && static_cast<std::size_t>(index) < m.vertices.size()) m.vertices[static_cast<std::size_t>(index)] = p;
        return api::Value{};
    }, {"i, Vec3", "mueve un vertice (luego apply)"});
    api.method("Mesh", "getVertex", [](api::Call& c) {
        const ecs::Mesh& m = selfMesh(c);
        const long long index = c.integer(0);
        return api::Value(index >= 0 && static_cast<std::size_t>(index) < m.vertices.size()
                              ? m.vertices[static_cast<std::size_t>(index)]
                              : Vec3{});
    }, {"i", "Vec3 de un vertice", "Vec3"});
    api.method("Mesh", "setMaterial", [](api::Call& c) {
        ecs::Mesh& m = selfMesh(c);
        setMaterial(m, static_cast<int>(c.integer(0)), c.object(1));
        return api::Value{};
    }, {"submalla, {color, metallic, roughness...}", "material de una submalla"});
    api.method("Mesh", "getMaterial", [](api::Call& c) { return getMaterial(selfMesh(c), static_cast<int>(c.integer(0))); },
               {"submalla", "su material", "objeto"});
    api.method("Mesh", "recalculateNormals", [](api::Call& c) {
        ecs::Mesh& m = selfMesh(c);
        m.recalculateNormals();
        m.markModified();
        return api::Value{};
    }, {"", "normales suaves"});
    api.method("Mesh", "recalculateTangents", [](api::Call& c) {
        ecs::Mesh& m = selfMesh(c);
        m.recalculateTangents();
        m.markModified();
        return api::Value{};
    }, {"", "tangentes"});
    api.method("Mesh", "recalculateBounds", [](api::Call& c) { selfMesh(c).recalculateBounds(); return api::Value{}; },
               {"", "caja"});
    api.method("Mesh", "apply", [](api::Call& c) { selfMesh(c).markModified(); return api::Value{}; },
               {"", "sube los cambios a la GPU"});
    api.method("Mesh", "clear", [](api::Call& c) { selfMesh(c).clear(); return api::Value{}; }, {"", "la vacia"});
    // "" si se puede dibujar; si no, que le pasa.
    api.method("Mesh", "validate", [](api::Call& c) { return api::Value(selfMesh(c).validate()); },
               {"", "\"\" si se puede dibujar; si no, el motivo", "texto"});
    api.method("Mesh", "clone", [&rt](api::Call& c) {
        auto copy = std::make_shared<ecs::Mesh>(selfMesh(c));
        copy->markModified();
        return meshValue(rt, copy);
    }, {"", "copia", "Mesh"});
    // Entity.mesh (la malla de su MeshRenderer) la registra EntityApi.cpp con
    // meshValue / meshOf.
}

}  // namespace cramion::scripting::native

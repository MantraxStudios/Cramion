#ifndef CRAMION_CORE_RUNTIME_MESH_H
#define CRAMION_CORE_RUNTIME_MESH_H

// Mallas creadas en tiempo de ejecucion, como el Mesh de Unity:
//
//   auto mesh = std::make_shared<ecs::Mesh>();
//   mesh->vertices = {...};  mesh->uv = {...};
//   mesh->setTriangles({0, 1, 2, 0, 2, 3});
//   mesh->recalculateNormals();   // y recalculateTangents() para normal maps
//   mesh->markModified();         // la GPU la recoge en el siguiente frame
//   entity.get<ecs::MeshRenderer>().mesh = mesh;
//
// El MeshRenderer que la lleva la dibuja en lugar de su modelo (y un
// MeshCollider de la misma entidad choca con ella). Cada submalla es un hueco
// de material: los .crmat de MeshRenderer::materials mandan y, si no hay, se
// usa su MeshMaterial (color, metal, rugosidad, emision, texturas con recorte
// por alfa y repeticion). Cambiar solo los factores de un material
// (markMaterialsModified) no vuelve a subir la malla: sirve para efectos que
// cambian cada frame (brillos que laten, fundidos de color). No se guardan en
// la escena (como las de Unity creadas por codigo).
//
// En Lua: Mesh.new(), Mesh.cube/plane/sphere/cylinder/capsule/quad y
// entity.mesh (ver docs/scripting.html).

#include <CramionFX/core/Math.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace cramion::asset {
struct ModelData;
}

namespace cramion::ecs {

// Material de una submalla cuando el MeshRenderer no le pone un .crmat.
struct MeshMaterial {
    core::Vec4 color{1.0f, 1.0f, 1.0f, 1.0f};  // sRGB (como el Inspector)
    float metallic = 0.0f;
    float roughness = 0.6f;
    core::Vec3 emission{0.0f, 0.0f, 0.0f};  // sRGB
    float emission_intensity = 1.0f;
    // Texturas (rutas en Assets; vacia = ninguna). El alfa del color recorta
    // (hojas, rejas, grietas).
    std::string texture;
    std::string normal_map;
    std::string emission_map;
    float normal_strength = 1.0f;
    core::Vec2 tiling{1.0f, 1.0f};  // repeticiones de la textura
    core::Vec2 offset{0.0f, 0.0f};
};

class Mesh {
public:
    std::string name = "Malla";

    // Por vertice (todas las listas con el mismo tamano que `vertices`, o
    // vacias: se rellenan al subirla).
    std::vector<core::Vec3> vertices;
    std::vector<core::Vec3> normals;
    std::vector<core::Vec4> tangents;  // xyz = +U, w = +-1 (bitangente)
    std::vector<core::Vec2> uv;

    // Material por submalla (el indice es el de la submalla).
    std::vector<MeshMaterial> materials;

    // --- Triangulos (3 indices de vertice por triangulo, antihorarios vistos
    // desde fuera) ---
    int subMeshCount() const { return static_cast<int>(submeshes_.size()); }
    void setSubMeshCount(int count);
    void setTriangles(std::vector<std::uint32_t> triangles, int submesh = 0);
    const std::vector<std::uint32_t>& triangles(int submesh = 0) const;
    // Todos los triangulos de todas las submallas seguidos.
    std::vector<std::uint32_t> allTriangles() const;
    std::size_t triangleCount() const;
    void clear();

    // --- Calculos ---
    // Normales suaves por vertice (media de las caras que lo usan, pesada por
    // su area). Para aristas duras, vertices separados en cada cara.
    void recalculateNormals();
    // Tangentes a partir de las UV y las normales (para los normal maps).
    void recalculateTangents();
    void recalculateBounds();
    const core::Vec3& boundsMin() const { return bounds_min_; }
    const core::Vec3& boundsMax() const { return bounds_max_; }

    // Tras cambiar los datos: sube la version y el renderizador (y el
    // MeshCollider) la vuelven a leer. Tambien recalcula los limites.
    void markModified();
    std::uint64_t version() const { return version_; }
    // Solo cambiaron factores de los materiales (color, metal, rugosidad,
    // emision): se actualizan en la GPU sin volver a subir la malla. Si
    // cambian las texturas o la repeticion, markModified().
    void markMaterialsModified() { ++material_version_; }
    std::uint64_t materialVersion() const { return material_version_; }
    // Firma de lo que obliga a volver a subirla desde los materiales.
    std::string materialLayout() const;

    // Vacio si se puede dibujar; si no, el motivo (indices fuera de rango,
    // listas de distinto tamano...).
    std::string validate() const;

    // Datos del renderizador: normales, tangentes y UV que falten se calculan
    // o se ponen a cero; un material por submalla. Las texturas se buscan en
    // `assets_root` (quedan por leer: asset::finalizeModel las decodifica).
    asset::ModelData toModelData(const std::filesystem::path& assets_root = {}) const;
    // Material del renderizador de una submalla (factores, sin texturas).
    static void applyFactors(const MeshMaterial& m, void* material_data);

    // --- Primitivas (centradas en el origen, UV de 0 a 1 por cara) ---
    static std::shared_ptr<Mesh> cube(const core::Vec3& size = {1.0f, 1.0f, 1.0f});
    static std::shared_ptr<Mesh> quad(float width = 1.0f, float height = 1.0f);  // en XY, mirando a +Z
    static std::shared_ptr<Mesh> plane(float width = 10.0f, float depth = 10.0f, int segments_x = 10, int segments_z = 10);
    static std::shared_ptr<Mesh> sphere(float radius = 0.5f, int segments = 32, int rings = 16);
    static std::shared_ptr<Mesh> cylinder(float radius = 0.5f, float height = 2.0f, int segments = 32);
    static std::shared_ptr<Mesh> capsule(float radius = 0.5f, float height = 2.0f, int segments = 24, int rings = 8);
    // Las 12 aristas de una caja como barras de `thickness` (contornos).
    static std::shared_ptr<Mesh> wireCube(const core::Vec3& size = {1.0f, 1.0f, 1.0f}, float thickness = 0.02f);

private:
    std::vector<std::vector<std::uint32_t>> submeshes_{1};
    core::Vec3 bounds_min_{};
    core::Vec3 bounds_max_{};
    std::uint64_t version_ = 1;
    std::uint64_t material_version_ = 1;
};

}  // namespace cramion::ecs

#endif  // CRAMION_CORE_RUNTIME_MESH_H

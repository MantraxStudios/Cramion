#ifndef CRAMIONFX_ASSET_PROXY_MESH_H
#define CRAMIONFX_ASSET_PROXY_MESH_H

// Mallas proxy para HLOD (como los Hierarchical LOD de Unreal): muchas piezas
// estaticas de una zona juntadas en una sola malla muy simplificada, con un
// color por grupo de materiales (el color medio de cada uno, textura
// incluida). Se ve de lejos en lugar de las piezas (World Partition: cuando
// la celda esta descargada).

#include "CramionFX/asset/Model.h"
#include "CramionFX/core/Math.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cramion::asset {

struct ProxyPart {
    const ModelData* model = nullptr;
    core::Mat4 transform = core::Mat4::identity();  // modelo -> espacio del proxy
};

struct ProxyMesh {
    std::vector<core::Vec3> positions;
    std::vector<core::Vec3> normals;
    struct Group {
        core::Vec3 color{0.5f, 0.5f, 0.5f};  // sRGB
        float roughness = 0.8f;
        std::vector<std::uint32_t> indices;
    };
    std::vector<Group> groups;
    std::size_t sourceTriangles = 0;
    std::size_t triangles() const;
};

// Junta las piezas y simplifica hasta `ratio` de los triangulos (y como mucho
// `max_triangles`). Hasta `max_colors` grupos de color.
bool buildProxyMesh(const std::vector<ProxyPart>& parts, float ratio, std::size_t max_triangles, int max_colors,
                    ProxyMesh& out);

// Escribe la malla como .obj + .mtl (un material por grupo).
bool writeProxyObj(const ProxyMesh& mesh, const std::filesystem::path& obj_file, const std::string& name,
                   std::string* error = nullptr);

}  // namespace cramion::asset

#endif  // CRAMIONFX_ASSET_PROXY_MESH_H

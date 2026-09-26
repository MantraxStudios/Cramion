#ifndef CRAMION_CORE_ASSET_MATERIAL_ASSET_H
#define CRAMION_CORE_ASSET_MATERIAL_ASSET_H

// Material del proyecto (.crmat, JSON), como el Material de Unity: color,
// texturas sueltas de Assets (color, normal, metal, rugosidad, oclusion,
// emision), factores y tiling. Se asigna a los huecos de material de un
// MeshRenderer (arrastrandolo al objeto); los objetos con el mismo modelo y
// los mismos materiales se dibujan juntos (una llamada por material).

#include "CramionCore/Uuid.h"

#include <CramionFX/asset/Model.h>
#include <CramionFX/core/Math.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>

namespace cramion::assets {

enum class MaterialMode : int {
    Opaque = 0,       // opaco (con recorte si el color tiene alfa: hojas, rejas)
    Transparent = 1,  // vidrio, agua: semitransparente
};

struct MaterialAsset {
    Uuid uuid;
    MaterialMode mode = MaterialMode::Opaque;

    core::Vec4 base_color{1.0f, 1.0f, 1.0f, 1.0f};  // lineal; a = opacidad
    float metallic = 0.0f;
    float roughness = 0.5f;
    float reflectance = 0.04f;
    float normal_strength = 1.0f;
    bool normal_directx = false;  // normal map de Unreal/DirectX (+Y abajo)
    float occlusion_strength = 1.0f;
    core::Vec3 emissive{0.0f, 0.0f, 0.0f};
    float emissive_intensity = 1.0f;
    core::Vec2 tiling{1.0f, 1.0f};
    core::Vec2 offset{0.0f, 0.0f};
    // Relieve del mapa de alturas (parallax occlusion mapping) en metros:
    // lo que hay del negro al blanco del mapa (0.03 = 3 cm).
    float height_scale = 0.03f;
    float cavity_strength = 1.0f;  // cuanto oscurecen las grietas (mapa de cavidad)

    // Texturas: rutas dentro de Assets (vacias = solo el factor).
    std::string albedo;
    std::string normal;
    std::string metallic_map;   // gris: metalicidad (multiplica al factor)
    std::string roughness_map;  // gris: rugosidad
    std::string occlusion;
    std::string emissive_map;
    // Mapas de los packs de escaneos (Megascans, Poly Haven...):
    std::string height_map;    // gris: altura / displacement (blanco = alto) -> parallax
    std::string cavity_map;    // gris: grietas (negro = hueco): quita brillo y luz
    std::string specular_map;  // gris: reflectancia (0.5 = F0 0.04, como Unreal)
    std::string gloss_map;     // gris: brillo = 1 - rugosidad (si no hay rugosidad)
    std::string bump_map;      // gris: relieve fino; se usa como normal si no hay normal map

    // Shader de superficie propio (.crshader dentro de Assets; vacio = el
    // estandar) y los valores de sus propiedades por nombre (las que falten
    // usan el valor por defecto del shader). Las texturas son imagenes de Assets.
    std::string shader;
    std::map<std::string, core::Vec4> shader_values;
    std::map<std::string, std::string> shader_textures;
};

bool loadMaterial(const std::filesystem::path& path, MaterialAsset& out, std::string* error = nullptr);
// Sin UUID se le da uno nuevo (y se escribe en `material`).
bool saveMaterial(MaterialAsset& material, const std::filesystem::path& path, std::string* error = nullptr);

// Lo que obliga a volver a subir el modelo a la GPU si cambia (texturas,
// tiling, modo). Lo demas (colores y factores) se cambia en vivo.
std::uint64_t materialStructureHash(const MaterialAsset& material);

// Factores del material para el renderizador (sin las texturas).
asset::MaterialData toMaterialData(const MaterialAsset& material, const std::string& name);

// Material nuevo a partir de una imagen de color: busca junto a ella sus
// companeras por el sufijo del nombre (piedra_normal.png, piedra_rough.png,
// piedra_ao.png...), como los packs de texturas PBR. `image` y las rutas del
// resultado son relativas a `assets_root`.
MaterialAsset materialFromImage(const std::filesystem::path& assets_root, const std::string& image);

}  // namespace cramion::assets

#endif  // CRAMION_CORE_ASSET_MATERIAL_ASSET_H

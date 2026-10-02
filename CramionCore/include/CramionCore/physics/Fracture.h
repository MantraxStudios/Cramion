#ifndef CRAMION_CORE_PHYSICS_FRACTURE_H
#define CRAMION_CORE_PHYSICS_FRACTURE_H

// Fractura de Voronoi precalculada (como el Fracture Mode de Unreal o los
// paquetes de destruccion de Unity): la malla de un objeto se parte en trozos
// convexos con semillas aleatorias (celdas de Voronoi) y se guarda en un
// asset .crfracture. Cada trozo lleva sus caras de fuera (con las UV y
// normales del modelo) y las caras de dentro (el corte), que van a otro
// material. Opcionalmente cada trozo se vuelve a partir (varios niveles:
// los trozos grandes se rompen otra vez al recibir golpes).
//
//   FractureSettings s;  s.pieces = 24;  s.seed = 7;
//   FractureData data;
//   fractureMesh(sourceFromModel(model_part), s, data);
//   saveFracture("Assets/Fracturas/Muro.crfracture", data);
//
// Archivo .crfracture: 1a linea = cabecera JSON {"uuid", "type":"fracture",
// "pieces", "source_model"...}; 2a linea = los trozos (JSON). Lo usa el
// componente Destructible (Destruction.h).

#include "CramionCore/Uuid.h"

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
class Mesh;
struct MeshMaterial;
}

namespace cramion::physics {

inline constexpr const char* kFractureExtension = ".crfracture";

// Malla de entrada (en el espacio local del objeto).
struct FractureSourceMesh {
    std::vector<core::Vec3> positions;
    std::vector<core::Vec3> normals;  // vacio: se calculan por cara
    std::vector<core::Vec2> uvs;      // vacio: (0, 0)
    std::vector<std::uint32_t> indices;
};

struct FractureSettings {
    int pieces = 16;                // trozos del primer nivel
    std::uint32_t seed = 1;
    int levels = 1;                 // 1 = un nivel; 2 o 3 = los trozos se rompen otra vez
    int sub_pieces = 4;             // trozos de cada trozo en los niveles siguientes
    float interior_uv_scale = 1.0f; // repeticion de la textura en las caras del corte (por metro)
    // Concentrar los trozos (pequenos) alrededor de un punto: impactos.
    bool cluster = false;
    core::Vec3 cluster_point{};
    float cluster_radius = 0.5f;    // m
    float min_piece_fraction = 0.002f;  // trozos con menos volumen (del total) se descartan
};

struct FracturePiece {
    core::Vec3 center{};            // centro del trozo en el espacio del objeto
    // Vertices relativos a `center`.
    std::vector<core::Vec3> positions;
    std::vector<core::Vec3> normals;
    std::vector<core::Vec2> uvs;
    std::vector<std::uint32_t> exterior;  // triangulos de fuera (material del objeto)
    std::vector<std::uint32_t> interior;  // triangulos del corte (material de dentro)
    float volume = 0.0f;            // m3 (sin la escala del objeto)
    int level = 0;                  // 0 = primer nivel
    int parent = -1;                // indice del trozo que lo contiene
    std::vector<int> children;      // trozos en que se rompe (siguiente nivel)
};

struct FractureData {
    Uuid uuid{};
    Uuid source_model{};            // informativo
    int source_part = 0;
    FractureSettings settings;
    core::Vec3 bounds_min{};
    core::Vec3 bounds_max{};
    float total_volume = 0.0f;
    std::vector<FracturePiece> pieces;

    // Los trozos del primer nivel.
    std::vector<int> roots() const;
    std::size_t triangleCount() const;
};

// Malla de entrada desde una pieza de modelo o una malla creada por codigo.
FractureSourceMesh sourceFromModel(const asset::ModelData& model);
FractureSourceMesh sourceFromMesh(const ecs::Mesh& mesh);

// Parte la malla. Devuelve false (y el motivo) si no sale ningun trozo.
// La malla deberia ser cerrada; si tiene agujeros, las tapas del corte
// pueden faltar en esa zona.
bool fractureMesh(const FractureSourceMesh& source, const FractureSettings& settings, FractureData& out,
                  std::string* error = nullptr);

// Lo mismo con cajas sin malla: un cubo de `size` partido (pruebas y muros).
FractureSourceMesh boxSource(const core::Vec3& size);

// .crfracture
std::string fractureToText(const FractureData& data);
bool fractureFromText(const std::string& text, FractureData& out, std::string* error = nullptr);
bool saveFracture(const std::filesystem::path& file, const FractureData& data, std::string* error = nullptr);
bool loadFracture(const std::filesystem::path& file, FractureData& out, std::string* error = nullptr);

// Malla del renderizador de un trozo: submalla 0 = fuera, 1 = dentro.
std::shared_ptr<ecs::Mesh> pieceMesh(const FracturePiece& piece, const ecs::MeshMaterial& exterior,
                                     const ecs::MeshMaterial& interior);

}  // namespace cramion::physics

#endif  // CRAMION_CORE_PHYSICS_FRACTURE_H

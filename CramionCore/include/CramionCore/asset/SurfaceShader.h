#ifndef CRAMION_CORE_ASSET_SURFACE_SHADER_H
#define CRAMION_CORE_ASSET_SURFACE_SHADER_H

// Shaders de superficie del usuario (.crshader), como los "surface shaders"
// de Unity: GLSL que cambia la superficie (color, normal, brillo, emision...)
// y opcionalmente mueve los vertices; el motor lo mete en su shader del
// G-buffer (CramionFX/shaders/surface.vert/.frag), asi recibe la luz, las
// sombras, los reflejos y la lluvia como cualquier material.
//
//   // Holograma
//   property color tinte = 0.2, 0.8, 1.0
//   property range velocidad = 2.0 (0, 10)
//   property texture ruido
//
//   void surface(inout Surface s) {
//       float bandas = sin(s.worldPosition.y * 40.0 - TIME * velocidad);
//       s.emission = tinte * (0.5 + 0.5 * bandas);
//   }
//
//   void vertex(inout Vertex v) { ... }   // opcional
//
// Propiedades: float, range (min, max), color, vector (hasta 8 en total) y
// texture (hasta 4). Se leen por su nombre en el codigo y se editan en el
// material (.crmat) que usa el shader.

#include <CramionFX/core/Math.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cramion::assets {

inline constexpr const char* kSurfaceShaderExtension = ".crshader";
inline constexpr int kMaxShaderValues = 8;    // float, range, color, vector
inline constexpr int kMaxShaderTextures = 4;  // texture

enum class ShaderPropertyType { Float, Range, Color, Vector, Texture };

struct ShaderProperty {
    std::string name;
    ShaderPropertyType type = ShaderPropertyType::Float;
    core::Vec4 value{0.0f, 0.0f, 0.0f, 0.0f};  // por defecto
    float min = 0.0f;                           // range
    float max = 1.0f;
    int slot = 0;  // vec4 de las propiedades o textura (0..3)
    int line = 0;
};

struct SurfaceShaderSource {
    std::string name;  // del archivo (sale en los errores)
    std::vector<ShaderProperty> properties;
    std::string code;  // el codigo, sin las lineas "property" (mismas lineas)
    bool has_vertex = false;
};

// Lee un .crshader. Los errores llevan "archivo:linea: ...".
bool parseSurfaceShader(const std::string& text, const std::string& name, SurfaceShaderSource& out,
                        std::string* error = nullptr);
bool loadSurfaceShader(const std::filesystem::path& file, SurfaceShaderSource& out, std::string* error = nullptr);

// El GLSL completo de un stage: la plantilla (surface.vert/.frag de
// `template_dir`, que es shaders/source junto al ejecutable) con el codigo del
// usuario y las propiedades como #define.
bool generateSurfaceGlsl(const SurfaceShaderSource& shader, bool vertex_stage, const std::filesystem::path& template_dir,
                         std::string& glsl, std::string* error = nullptr);

// Genera y compila los dos stages a SPIR-V (shaderc). Los errores de
// compilacion apuntan a las lineas del .crshader.
bool compileSurfaceShader(const SurfaceShaderSource& shader, const std::filesystem::path& template_dir,
                          std::vector<std::uint32_t>& vertex_spirv, std::vector<std::uint32_t>& fragment_spirv,
                          std::string* error = nullptr);

// shaders/source junto al ejecutable.
std::filesystem::path surfaceTemplateDirectory();

// Contenido de un .crshader nuevo (Proyecto > Crear > Shader).
std::string surfaceShaderTemplate(const std::string& name);

}  // namespace cramion::assets

#endif  // CRAMION_CORE_ASSET_SURFACE_SHADER_H

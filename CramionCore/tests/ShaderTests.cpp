// Pruebas de los shaders de superficie del usuario (consola, sin GPU): leer
// un .crshader (propiedades y errores), montarlo con las plantillas del motor,
// compilarlo con shaderc y que los errores apunten a su linea. Tambien el
// shader nuevo por defecto y el .crmat con shader. Devuelve 0 si todo va.

#include "CramionCore/asset/MaterialAsset.h"
#include "CramionCore/asset/SurfaceShader.h"

#include <CramionFX/vk/ShaderCompiler.h>

#include <cstdio>
#include <filesystem>
#include <string>

using namespace cramion;

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const char* what, const std::string& detail = {}) {
    ++checks;
    std::printf("  %s %s%s%s\n", condition ? "OK   " : "FALLO", what, condition || detail.empty() ? "" : "  -> ",
                condition ? "" : detail.c_str());
    if (!condition) ++failures;
}

const char* kHologram = R"(// Holograma
property color tinte = 0.2, 0.8, 1.0
property range velocidad = 2.0 (0, 10)
property float bandas = 40
property vector viento = 1, 0, 0
property texture ruido

void surface(inout Surface s) {
    float linea = 0.5 + 0.5 * sin(s.worldPosition.y * bandas - TIME * velocidad);
    s.albedo = mix(s.albedo, tinte, 0.5);
    s.emission = tinte * linea * texture(ruido, s.uv).r + tinte * fresnel(s, 3.0);
    s.alpha = linea > 0.1 ? 1.0 : 0.0;
    float d = dFdx(s.uv.x);  // solo en fragmentos: no debe romper los vertices
    s.roughness += d * 0.0;
}

void vertex(inout Vertex v) {
    v.position += viento * sin(TIME + v.position.y) * 0.05 * noise(v.position);
}
)";

}  // namespace

int main(int argc, char** argv) {
    // Con archivos .crshader: solo los compila (los ejemplos del manual).
    if (argc > 1 && std::filesystem::path(argv[1]).extension() == assets::kSurfaceShaderExtension) {
        int bad = 0;
        for (int i = 1; i < argc; ++i) {
            assets::SurfaceShaderSource s;
            std::vector<std::uint32_t> vertex, fragment;
            std::string e;
            if (!assets::loadSurfaceShader(argv[i], s, &e) ||
                !assets::compileSurfaceShader(s, assets::surfaceTemplateDirectory(), vertex, fragment, &e)) {
                ++bad;
                std::printf("  FALLO %s\n%s\n", argv[i], e.c_str());
            }
        }
        std::printf("%d shaders, %d no compilan\n", argc - 1, bad);
        return bad == 0 ? 0 : 1;
    }
    // Plantillas: shaders/source junto al motor (el primer argumento puede
    // dar otra carpeta, p. ej. build/CramionFX/shaders/source).
    const std::filesystem::path templates =
        argc > 1 ? std::filesystem::path(argv[1]) : assets::surfaceTemplateDirectory();

    std::printf("Leer .crshader\n");
    assets::SurfaceShaderSource shader;
    std::string error;
    const bool parsed = assets::parseSurfaceShader(kHologram, "Holograma.crshader", shader, &error);
    check(parsed && shader.properties.size() == 5 && shader.has_vertex, "propiedades y funcion vertex", error);
    if (parsed) {
        const auto& p = shader.properties;
        check(p[0].type == assets::ShaderPropertyType::Color && p[0].slot == 0 && p[0].value.y == 0.8f,
              "color con su valor por defecto");
        check(p[1].type == assets::ShaderPropertyType::Range && p[1].min == 0.0f && p[1].max == 10.0f &&
                  p[1].value.x == 2.0f && p[1].slot == 1,
              "range con sus limites");
        check(p[4].type == assets::ShaderPropertyType::Texture && p[4].slot == 0, "texture en su propia ranura");
        check(shader.code.find("property") == std::string::npos &&
                  std::count(shader.code.begin(), shader.code.end(), '\n') ==
                      std::count(kHologram, kHologram + std::char_traits<char>::length(kHologram), '\n'),
              "el codigo conserva las lineas (errores en su sitio)");
    }
    const auto parse_error = [](const char* text) {
        assets::SurfaceShaderSource s;
        std::string e;
        return assets::parseSurfaceShader(text, "Malo.crshader", s, &e) ? std::string{} : e;
    };
    check(parse_error("property colour x = 1\nvoid surface(inout Surface s) {}").find("Malo.crshader:1:") == 0,
          "tipo desconocido: error con archivo y linea", parse_error("property colour x = 1\nvoid surface(inout Surface s) {}"));
    check(parse_error("property range x = 1\nvoid surface(inout Surface s) {}").find("limites") != std::string::npos,
          "range sin limites");
    check(parse_error("void vertex(inout Vertex v) {}").find("surface") != std::string::npos, "falta surface()");
    check(parse_error("property float a\nproperty float a\nvoid surface(inout Surface s) {}").find(":2:") != std::string::npos,
          "propiedad repetida");

    std::printf("Montar y compilar (shaderc)\n");
    std::string vertex_glsl, fragment_glsl;
    const bool generated = assets::generateSurfaceGlsl(shader, true, templates, vertex_glsl, &error) &&
                           assets::generateSurfaceGlsl(shader, false, templates, fragment_glsl, &error);
    check(generated && fragment_glsl.find("#define tinte (CRAMION_PARAM(0).xyz)") != std::string::npos &&
              fragment_glsl.find("#define ruido cramion_texture0") != std::string::npos &&
              vertex_glsl.find("dFdx") == std::string::npos && vertex_glsl.find("CRAMION_HAS_VERTEX 1") != std::string::npos,
          "plantillas con las propiedades; los vertices sin surface()", error);
    if (!gfx::shaders::compilerAvailable()) {
        std::string why;
        std::vector<std::uint32_t> none;
        gfx::shaders::compile("", gfx::shaders::Stage::Vertex, "x", none, why);
        check(false, "shaderc_shared.dll disponible", why);
    } else {
        std::vector<std::uint32_t> vertex, fragment;
        const bool compiled = assets::compileSurfaceShader(shader, templates, vertex, fragment, &error);
        check(compiled && vertex.size() > 100 && fragment.size() > 100 && fragment[0] == 0x07230203u,
              "compila a SPIR-V los dos stages", error);

        assets::SurfaceShaderSource broken;
        assets::parseSurfaceShader("property float fuerza = 1\n\nvoid surface(inout Surface s) {\n    s.albedo = colorQueNoExiste;\n}\n",
                                   "Roto.crshader", broken, nullptr);
        std::string compile_error;
        const bool bad = assets::compileSurfaceShader(broken, templates, vertex, fragment, &compile_error);
        check(!bad && compile_error.find("Roto.crshader:4:") != std::string::npos &&
                  compile_error.find("colorQueNoExiste") != std::string::npos,
              "un error de GLSL dice su archivo y su linea", compile_error);

        assets::SurfaceShaderSource fresh;
        const bool fresh_ok = assets::parseSurfaceShader(assets::surfaceShaderTemplate("Nuevo"), "Nuevo.crshader", fresh, &error) &&
                              assets::compileSurfaceShader(fresh, templates, vertex, fragment, &error);
        check(fresh_ok && fresh.properties.size() == 3 && !fresh.has_vertex, "el shader nuevo por defecto compila", error);

        // Con vertex() de ejemplo descomentado.
        std::string with_vertex = assets::surfaceShaderTemplate("Olas");
        for (const char* line : {"// void vertex", "//     v.position", "// }"}) {
            const std::size_t at = with_vertex.find(line);
            if (at != std::string::npos) with_vertex.erase(at, 3);
        }
        const bool olas_ok = assets::parseSurfaceShader(with_vertex, "Olas.crshader", fresh, &error) &&
                             fresh.has_vertex && assets::compileSurfaceShader(fresh, templates, vertex, fragment, &error);
        check(olas_ok, "y con su vertex() de ejemplo", error);
    }

    std::printf("Material con shader\n");
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "cramion_shader_tests";
    std::filesystem::create_directories(dir);
    assets::MaterialAsset m;
    m.shader = "Shaders/Holograma.crshader";
    m.shader_values["tinte"] = core::Vec4{1.0f, 0.0f, 0.5f, 0.0f};
    m.shader_textures["ruido"] = "Texturas/ruido.png";
    const std::uint64_t before = assets::materialStructureHash(m);
    bool saved = assets::saveMaterial(m, dir / "Holo.crmat", &error);
    assets::MaterialAsset loaded;
    saved = saved && assets::loadMaterial(dir / "Holo.crmat", loaded, &error);
    check(saved && loaded.shader == m.shader && loaded.shader_values["tinte"].z == 0.5f &&
              loaded.shader_textures["ruido"] == "Texturas/ruido.png",
          "el .crmat guarda el shader, sus valores y sus texturas", error);
    loaded.shader_values["tinte"].x = 0.25f;
    check(assets::materialStructureHash(loaded) == before, "cambiar un valor no obliga a volver a subir el modelo");
    loaded.shader_textures["ruido"] = "otra.png";
    check(assets::materialStructureHash(loaded) != before, "cambiar una textura si");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

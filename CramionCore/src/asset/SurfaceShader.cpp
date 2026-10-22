// Shaders de superficie del usuario (ver SurfaceShader.h): lectura del
// .crshader, propiedades como #define y montaje con las plantillas del motor.

#include "CramionCore/asset/SurfaceShader.h"

#include "CrData.h"

#include <CramionFX/vk/ShaderCompiler.h>
#include <CramionFX/vk/VulkanShader.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

namespace cramion::assets {

namespace {

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

bool validName(const std::string& n) {
    if (n.empty() || !(std::isalpha(static_cast<unsigned char>(n[0])) || n[0] == '_')) return false;
    for (char c : n) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
    }
    // Nombres que usa la plantilla.
    static const std::set<std::string> reserved = {
        "s", "v", "surface", "vertex", "main", "push", "camera", "weather", "TIME", "DELTA_TIME", "CAMERA_POSITION",
        "Surface", "Vertex", "SCREEN_UV", "INSTANCE_DATA", "instanceData",
        // Librerias de surface_common.glsl (un #define con su nombre las rompe).
        "hash", "noise", "fbm", "fresnel", "remap", "toLinear", "toSrgb", "random", "pcgHash", "pcgHash2", "pcgHash3",
        "gradientNoise", "gradientNoiseD", "simplexNoise", "fbmNoise", "ridgedNoise", "voronoi", "curlNoise",
        "sceneColor", "sceneDepth", "sceneWorldPosition", "sceneLinearDepth", "sceneEmission", "depthFade"};
    return reserved.count(n) == 0;
}

// "1, 0.5, 0.2" -> hasta 4 numeros.
bool readNumbers(const std::string& text, std::vector<float>& out) {
    std::string t = text;
    for (char& c : t) {
        if (c == ',' || c == '(' || c == ')') c = ' ';
    }
    std::istringstream in(t);
    float v = 0.0f;
    while (in >> v) out.push_back(v);
    in.clear();
    std::string rest;
    in >> rest;
    return rest.empty();
}

std::string readFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

// El codigo con los comentarios en blanco (mismas posiciones y lineas): para
// buscar funciones sin confundirse con las comentadas.
std::string blankComments(const std::string& code) {
    std::string out = code;
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '/' && i + 1 < out.size() && out[i + 1] == '/') {
            while (i < out.size() && out[i] != '\n') out[i++] = ' ';
        } else if (out[i] == '/' && i + 1 < out.size() && out[i + 1] == '*') {
            while (i < out.size() && !(out[i] == '*' && i + 1 < out.size() && out[i + 1] == '/')) {
                if (out[i] != '\n') out[i] = ' ';
                ++i;
            }
            if (i + 1 < out.size()) out[i] = out[i + 1] = ' ';
            ++i;
        }
    }
    return out;
}

bool hasFunction(const std::string& code, const std::string& name) {
    const std::regex head("\\bvoid\\s+" + name + "\\s*\\(");
    const std::string clean = blankComments(code);
    return std::regex_search(clean, head);
}

// Quita (sin cambiar las lineas) la funcion `void name(` con su cuerpo.
std::string removeFunction(const std::string& code, const std::string& name) {
    const std::regex head("\\bvoid\\s+" + name + "\\s*\\(");
    const std::string clean = blankComments(code);
    std::smatch m;
    if (!std::regex_search(clean, m, head)) return code;
    const std::size_t start = static_cast<std::size_t>(m.position(0));
    std::size_t open = clean.find('{', start);
    if (open == std::string::npos) return code;
    int depth = 0;
    std::size_t end = open;
    for (; end < clean.size(); ++end) {
        if (clean[end] == '{') ++depth;
        if (clean[end] == '}' && --depth == 0) break;
    }
    if (end >= code.size()) return code;
    std::string out = code;
    for (std::size_t i = start; i <= end; ++i) {
        if (out[i] != '\n') out[i] = ' ';
    }
    return out;
}

std::string swizzle(ShaderPropertyType type) {
    switch (type) {
        case ShaderPropertyType::Color:
        case ShaderPropertyType::Vector: return ".xyz";
        default: return ".x";
    }
}

// Expande los #include de `file` (dentro de `dir`). El de surface_user.glsl
// se sustituye por `user`. Pone #line para que los errores digan de donde.
bool expand(const std::filesystem::path& dir, const std::string& file, const std::string& user,
            std::ostringstream& out, int depth, std::string* error) {
    if (depth > 8) {
        if (error) *error = "includes demasiado anidados en " + file;
        return false;
    }
    const std::string text = readFile(dir / file);
    if (text.empty()) {
        if (error) *error = "falta la plantilla " + crdata::utf8(dir / file) + " (compila el motor: shaders/source)";
        return false;
    }
    std::istringstream in(text);
    std::string line;
    int number = 0;
    static const std::regex include_line("^\\s*#\\s*include\\s+\"([^\"]+)\"");
    while (std::getline(in, line)) {
        ++number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::smatch m;
        if (std::regex_search(line, m, include_line)) {
            const std::string name = m[1].str();
            if (name == "surface_user.glsl") {
                out << user << "\n";
            } else {
                out << "#line 1 \"" << name << "\"\n";
                if (!expand(dir, name, user, out, depth + 1, error)) return false;
            }
            out << "#line " << (number + 1) << " \"" << file << "\"\n";
            continue;
        }
        out << line << "\n";
    }
    return true;
}

}  // namespace

bool parseSurfaceShader(const std::string& text, const std::string& name, SurfaceShaderSource& out,
                        std::string* error) {
    out = SurfaceShaderSource{};
    out.name = name;
    std::istringstream in(text);
    std::string line;
    std::ostringstream code;
    int number = 0;
    int values = 0;
    int textures = 0;
    std::set<std::string> seen;
    const auto fail = [&](const std::string& message) {
        if (error) *error = name + ":" + std::to_string(number) + ": " + message;
        return false;
    };
    while (std::getline(in, line)) {
        ++number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string t = trim(line);
        // "domain opaque|transparent": donde se dibuja (en su propia linea; una
        // variable que se llame domain, "domain = 1.0;", sigue siendo codigo).
        if ((t.rfind("domain ", 0) == 0 || t.rfind("domain\t", 0) == 0) &&
            t.find_first_of("=;(") == std::string::npos) {
            code << "\n";
            if (const std::size_t comment = t.find("//"); comment != std::string::npos) t = trim(t.substr(0, comment));
            // "domain opaque", "domain unlit" (opaco sin luz), "domain
            // transparent" (iluminado) o "domain transparent unlit".
            std::istringstream domain_words(trim(t.substr(7)));
            std::string value, modifier, extra;
            domain_words >> value >> modifier >> extra;
            out.unlit = modifier == "unlit";
            if (value == "transparent") {
                out.domain = SurfaceDomain::Transparent;
            } else if (value == "opaque") {
                out.domain = SurfaceDomain::Opaque;
            } else if (value == "unlit" && modifier.empty()) {
                out.domain = SurfaceDomain::Opaque;
                out.unlit = true;
            } else {
                return fail("domain desconocido '" + value + "' (opaque, unlit, transparent, transparent unlit)");
            }
            if ((!modifier.empty() && modifier != "unlit") || !extra.empty()) {
                return fail("domain: tras '" + value + "' solo puede ir 'unlit'");
            }
            continue;
        }
        if (t.rfind("property ", 0) != 0 && t.rfind("property\t", 0) != 0) {
            code << line << "\n";
            continue;
        }
        code << "\n";  // la linea queda vacia: los errores siguen en su linea
        if (const std::size_t comment = t.find("//"); comment != std::string::npos) t = trim(t.substr(0, comment));
        std::istringstream words(t.substr(9));
        std::string type_word, prop_name;
        words >> type_word >> prop_name;
        std::string rest;
        std::getline(words, rest);
        rest = trim(rest);
        ShaderProperty p;
        p.name = prop_name;
        p.line = number;
        if (!validName(prop_name)) return fail("nombre de propiedad no valido: '" + prop_name + "'");
        if (!seen.insert(prop_name).second) return fail("propiedad repetida: " + prop_name);

        if (type_word == "texture") {
            p.type = ShaderPropertyType::Texture;
            if (textures >= kMaxShaderTextures) {
                return fail("demasiadas texturas en " + name + ": '" + prop_name + "' seria la " +
                            std::to_string(textures + 1) + ".a y el limite es " + std::to_string(kMaxShaderTextures) +
                            " propiedades texture por shader");
            }
            p.slot = textures++;
            out.properties.push_back(p);
            continue;
        }
        if (type_word == "float") p.type = ShaderPropertyType::Float;
        else if (type_word == "range") p.type = ShaderPropertyType::Range;
        else if (type_word == "color") p.type = ShaderPropertyType::Color;
        else if (type_word == "vector") p.type = ShaderPropertyType::Vector;
        else return fail("tipo de propiedad desconocido '" + type_word + "' (float, range, color, vector, texture)");
        if (values >= kMaxShaderValues) {
            return fail("demasiadas propiedades de numeros en " + name + ": '" + prop_name + "' seria la " +
                        std::to_string(values + 1) + ".a y el limite es " + std::to_string(kMaxShaderValues) +
                        " float/range/color/vector por shader (junta varias en un vector o color)");
        }
        p.slot = values++;

        // "= valor [ (min, max) ]"
        std::string value_text = rest;
        std::string range_text;
        if (!value_text.empty() && value_text[0] == '=') value_text = trim(value_text.substr(1));
        if (p.type == ShaderPropertyType::Range) {
            const std::size_t paren = value_text.find('(');
            if (paren == std::string::npos) return fail("range necesita sus limites: range nombre = 0.5 (0, 1)");
            range_text = value_text.substr(paren);
            value_text = trim(value_text.substr(0, paren));
        }
        std::vector<float> nums;
        if (!value_text.empty() && !readNumbers(value_text, nums)) return fail("valor no valido: " + value_text);
        const std::size_t want = p.type == ShaderPropertyType::Color || p.type == ShaderPropertyType::Vector ? 3 : 1;
        if (!nums.empty() && nums.size() != want && !(want == 3 && nums.size() == 1)) {
            return fail("se esperaban " + std::to_string(want) + " numeros para " + prop_name);
        }
        if (want == 3) {
            if (nums.empty()) nums = {p.type == ShaderPropertyType::Color ? 1.0f : 0.0f};
            if (nums.size() == 1) nums = {nums[0], nums[0], nums[0]};
            p.value = core::Vec4{nums[0], nums[1], nums[2], 0.0f};
        } else {
            p.value = core::Vec4{nums.empty() ? 0.0f : nums[0], 0.0f, 0.0f, 0.0f};
        }
        if (p.type == ShaderPropertyType::Range) {
            std::vector<float> limits;
            if (!readNumbers(range_text, limits) || limits.size() != 2) return fail("limites no validos: " + range_text);
            p.min = std::min(limits[0], limits[1]);
            p.max = std::max(limits[0], limits[1]);
        }
        out.properties.push_back(p);
    }
    out.code = code.str();
    if (!hasFunction(out.code, "surface")) {
        number = 1;
        return fail("falta la funcion void surface(inout Surface s)");
    }
    out.has_vertex = hasFunction(out.code, "vertex");
    // Color y profundidad de la escena: solo en la pasada forward (el G-buffer
    // se escribe antes de que haya imagen que leer). Mejor un error claro que
    // el "no matching overloaded function" de glslang.
    if (out.domain == SurfaceDomain::Opaque) {
        static const std::regex scene_use(
            "\\b(sceneColor|sceneDepth|sceneWorldPosition|sceneLinearDepth|sceneEmission|depthFade|SCREEN_UV)\\b");
        const std::string clean = blankComments(out.code);
        std::smatch m;
        if (std::regex_search(clean, m, scene_use)) {
            number = 1 + static_cast<int>(std::count(clean.begin(), clean.begin() + m.position(0), '\n'));
            return fail(m[1].str() + " solo existe con 'domain transparent' (se dibuja sobre la escena ya iluminada)");
        }
    }
    return true;
}

bool loadSurfaceShader(const std::filesystem::path& file, SurfaceShaderSource& out, std::string* error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        if (error) *error = "no se pudo abrir " + crdata::utf8(file);
        return false;
    }
    std::stringstream s;
    s << in.rdbuf();
    return parseSurfaceShader(s.str(), crdata::utf8(file.filename()), out, error);
}

bool generateSurfaceGlsl(const SurfaceShaderSource& shader, bool vertex_stage, const std::filesystem::path& template_dir,
                         std::string& glsl, std::string* error) {
    return generateSurfaceGlsl(shader, vertex_stage ? SurfaceStage::Vertex : SurfaceStage::Fragment, template_dir, glsl,
                               error);
}

bool generateSurfaceGlsl(const SurfaceShaderSource& shader, SurfaceStage stage, const std::filesystem::path& template_dir,
                         std::string& glsl, std::string* error) {
    const bool vertex_stage = stage != SurfaceStage::Fragment;
    std::ostringstream user;
    // Funciones propias que leen la escena (domain transparent) tambien se
    // compilan en los vertices: alli no hay escena, sceneColor() y compania
    // devuelven nada. Antes de las propiedades: sus #define no las tocan.
    if (vertex_stage && shader.domain == SurfaceDomain::Transparent) {
        user << "#define SCREEN_UV vec2(0.0)\n"
                "vec3 sceneColor(vec2 cramion_uv) { return vec3(0.0); }\n"
                "float sceneDepth(vec2 cramion_uv) { return 1.0; }\n"
                "vec3 sceneWorldPosition(vec2 cramion_uv) { return vec3(0.0); }\n"
                "float sceneLinearDepth(vec2 cramion_uv) { return 1.0e6; }\n"
                "vec3 sceneEmission(vec2 cramion_uv) { return vec3(0.0); }\n"
                "float depthFade(Surface cramion_s, float cramion_distance) { return 1.0; }\n";
    }
    // Sin luz: surface.frag escribe el modelo 8 (Unlit) y surface_forward.frag
    // se salta el sol, las luces y la sonda.
    if (shader.unlit) user << "#define CRAMION_UNLIT 1\n";
    user << "// --- " << shader.name << " (propiedades) ---\n";
    for (const ShaderProperty& p : shader.properties) {
        if (p.type == ShaderPropertyType::Texture) {
            user << "#define " << p.name << " cramion_texture" << p.slot << "\n";
        } else {
            user << "#define " << p.name << " (CRAMION_PARAM(" << p.slot << ")" << swizzle(p.type) << ")\n";
        }
    }
    if (shader.has_vertex) user << "#define CRAMION_HAS_VERTEX 1\n";
    // En los vertices no se compila surface() (puede usar dFdx, discard...).
    const std::string code = vertex_stage ? removeFunction(shader.code, "surface") : shader.code;
    user << "#line 1 \"" << shader.name << "\"\n" << code << "\n";
    // Fuera del codigo del usuario los nombres de sus propiedades vuelven a
    // ser libres (la plantilla puede tener variables que se llamen igual).
    for (const ShaderProperty& p : shader.properties) user << "#undef " << p.name << "\n";
    std::ostringstream out;
    const char* file = stage == SurfaceStage::Vertex                   ? "surface.vert"
                       : stage == SurfaceStage::ShadowVertex           ? "surface_shadow.vert"
                       : shader.domain == SurfaceDomain::Transparent ? "surface_forward.frag"
                                                                     : "surface.frag";
    if (!expand(template_dir, file, user.str(), out, 0, error)) return false;
    glsl = out.str();
    return true;
}

bool compileSurfaceShader(const SurfaceShaderSource& shader, const std::filesystem::path& template_dir,
                          std::vector<std::uint32_t>& vertex_spirv, std::vector<std::uint32_t>& fragment_spirv,
                          std::string* error) {
    std::string vertex_glsl, fragment_glsl, message;
    if (!generateSurfaceGlsl(shader, true, template_dir, vertex_glsl, error)) return false;
    if (!generateSurfaceGlsl(shader, false, template_dir, fragment_glsl, error)) return false;
    if (!gfx::shaders::compile(fragment_glsl, gfx::shaders::Stage::Fragment, shader.name, fragment_spirv, message)) {
        if (error) *error = message;
        return false;
    }
    if (!gfx::shaders::compile(vertex_glsl, gfx::shaders::Stage::Vertex, shader.name, vertex_spirv, message)) {
        if (error) *error = message;
        return false;
    }
    return true;
}

bool compileSurfaceShader(const SurfaceShaderSource& shader, const std::filesystem::path& template_dir,
                          SurfaceShaderPrograms& programs, std::string* error) {
    programs.shadow_vertex.clear();
    if (!compileSurfaceShader(shader, template_dir, programs.vertex, programs.fragment, error)) return false;
    // La sombra solo si vertex() mueve algo (sin el, la estandar) y si va al
    // G-buffer (lo transparente no proyecta sombra).
    if (!shader.has_vertex || shader.domain == SurfaceDomain::Transparent) return true;
    std::string shadow_glsl, message;
    if (!generateSurfaceGlsl(shader, SurfaceStage::ShadowVertex, template_dir, shadow_glsl, error)) return false;
    if (!gfx::shaders::compile(shadow_glsl, gfx::shaders::Stage::Vertex, shader.name, programs.shadow_vertex, message)) {
        if (error) *error = message;
        return false;
    }
    return true;
}

namespace {

bool readWords(const std::filesystem::path& file, std::vector<std::uint32_t>& words) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size <= 0 || size % 4 != 0) return false;
    words.resize(static_cast<std::size_t>(size) / 4);
    in.seekg(0);
    in.read(reinterpret_cast<char*>(words.data()), size);
    return static_cast<bool>(in) && !words.empty() && words[0] == 0x07230203u;  // numero magico de SPIR-V
}

bool writeWords(const std::filesystem::path& file, const std::vector<std::uint32_t>& words) {
    std::error_code e;
    std::filesystem::create_directories(file.parent_path(), e);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(words.data()), static_cast<std::streamsize>(words.size() * 4));
    return static_cast<bool>(out);
}

}  // namespace

bool loadPrecompiledSurfaceShader(const std::filesystem::path& crshader, std::vector<std::uint32_t>& vertex_spirv,
                                  std::vector<std::uint32_t>& fragment_spirv) {
    std::filesystem::path vertex = crshader;
    vertex += ".vert.spv";
    std::filesystem::path fragment = crshader;
    fragment += ".frag.spv";
    return readWords(vertex, vertex_spirv) && readWords(fragment, fragment_spirv);
}

bool loadPrecompiledSurfaceShader(const std::filesystem::path& crshader, SurfaceShaderPrograms& programs) {
    if (!loadPrecompiledSurfaceShader(crshader, programs.vertex, programs.fragment)) return false;
    std::filesystem::path shadow = crshader;
    shadow += ".shadow.vert.spv";
    if (!readWords(shadow, programs.shadow_vertex)) programs.shadow_vertex.clear();
    return true;
}

bool writePrecompiledSurfaceShader(const std::filesystem::path& vertex_file, const std::filesystem::path& fragment_file,
                                   const std::vector<std::uint32_t>& vertex_spirv,
                                   const std::vector<std::uint32_t>& fragment_spirv) {
    return writeWords(vertex_file, vertex_spirv) && writeWords(fragment_file, fragment_spirv);
}

bool writePrecompiledSurfaceShadow(const std::filesystem::path& shadow_file,
                                   const std::vector<std::uint32_t>& shadow_spirv) {
    return !shadow_spirv.empty() && writeWords(shadow_file, shadow_spirv);
}

std::filesystem::path surfaceTemplateDirectory() { return gfx::shaders::directory() / "source"; }

std::string surfaceShaderTemplate(const std::string& name) {
    return "// " + name + R"( - shader de superficie de Cramion (GLSL).
// Se usa desde un material (.crmat): en su Inspector, Shader > este archivo.
// Guardar (Ctrl+S) lo recompila y se ve al momento. Referencia: docs/manual/shaders.html

// Propiedades: se editan en el material y se leen aqui por su nombre.
property color tinte = 1.0, 1.0, 1.0
property range brillo = 0.0 (0, 5)
property float velocidad = 1.0

// Lo que ve la superficie: s.albedo, s.alpha, s.normal, s.metallic,
// s.roughness, s.occlusion, s.emission (se pueden cambiar) y s.uv,
// s.worldPosition, s.vertexNormal, s.viewDirection (solo lectura).
// Tambien TIME (segundos), DELTA_TIME, CAMERA_POSITION, fresnel(s, p) y ruido:
// noise(), fbm(), gradientNoise(), simplexNoise(), fbmNoise(p, octavas),
// ridgedNoise(), voronoi(), curlNoise(), random()...
// Por objeto, sin otro material: INSTANCE_DATA(0) e INSTANCE_DATA(1) (vec4 del
// MeshRenderer; desde un script, entity().setMaterialParam(0, x, y, z, w)).
void surface(inout Surface s) {
    s.albedo *= tinte;
    // Un pulso de luz con el tiempo.
    s.emission += tinte * brillo * (0.5 + 0.5 * sin(TIME * velocidad));
}

// Opcional: mover los vertices (en el mundo) antes de dibujarlos. La sombra
// y los vectores de movimiento (TAA, FSR, DLSS) siguen el movimiento.
// void vertex(inout Vertex v) {
//     v.position += v.normal * sin(TIME * 3.0 + v.position.y * 4.0) * 0.05;
// }

// Opcional: "domain unlit" se ve sin luz (color tal cual mas emision).
// "domain transparent" lo dibuja sobre la escena ya iluminada (con sol, luces,
// sonda y niebla; "domain transparent unlit": albedo * alpha + emision) y deja
// leerla: sceneColor(uv), sceneDepth(uv), SCREEN_UV, depthFade(s, metros).
// Refraccion (vidrio, agua): s.refraction = 0.05; desvia lo de detras por
// s.normal y lo deja ver por (1 - s.alpha).
// Distorsion de calor:
// domain transparent unlit
// void surface(inout Surface s) {
//     vec2 onda = gradientNoiseD(s.worldPosition.xz * 3.0 + TIME).yz * 0.01;
//     s.albedo = vec3(0.0);
//     s.alpha = 1.0;
//     s.emission = sceneEmission(SCREEN_UV + onda);
// }
)";
}

}  // namespace cramion::assets

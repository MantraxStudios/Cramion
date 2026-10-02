// Shader Graph (ver ShaderGraph.h): catalogo de nodos, lectura y escritura
// del .crshadergraph, generacion del .crshader y evaluacion en CPU para las
// vistas previas del editor.

#include "CramionCore/asset/ShaderGraph.h"

#include "CramionCore/asset/SurfaceShader.h"
#include "CrData.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <unordered_set>

namespace cramion::assets::shadergraph {

using json = nlohmann::json;

namespace {

enum class K {
    Master,
    // Entrada
    UV, Time, Position, Normal, ViewDir, Camera, Object, VertexColor, MaterialBase, MainTexture,
    // Constantes
    Float, Vector2, Vector3, Vector4, Color,
    // Propiedades
    PropFloat, PropRange, PropColor, PropVector, PropTexture,
    // Texturas
    SampleTexture, SampleNormal, Triplanar,
    // Matematicas
    Add, Subtract, Multiply, Divide, Lerp, Clamp, Saturate, Power, Dot, Cross, Normalize, OneMinus, Smoothstep,
    Step, Sin, Cos, Frac, Remap, Abs, Negate, Min, Max, Floor, Sqrt, Length, Distance, Modulo,
    // Ruido
    ValueNoise, PerlinNoise, Fbm, Voronoi,
    // UV
    TilingOffset, Panner, RotateUV,
    // Superficie
    Fresnel, NormalBlend, NormalStrength,
    // Canales y utilidad
    Split, Combine, Swizzle, Custom,
};

// Entradas del nodo Salida.
enum MasterPin {
    kBaseColor = 0,
    kAlpha,
    kAlphaClip,
    kMetallic,
    kRoughness,
    kOcclusion,
    kEmission,
    kNormalTangent,
    kNormalWorld,
    kVertexOffset,
    kVertexNormal,
};

struct Entry {
    NodeDef def;
    K kind = K::Float;
};

constexpr PinType F = PinType::Float;
constexpr PinType V2 = PinType::Vec2;
constexpr PinType V3 = PinType::Vec3;
constexpr PinType V4 = PinType::Vec4;
constexpr PinType TX = PinType::Texture;
constexpr PinType DY = PinType::Dynamic;

PinDef pin(const char* name, PinType type, core::Vec4 value = {}, PinDefault binding = PinDefault::Value) {
    PinDef p;
    p.name = name;
    p.type = type;
    p.value = value;
    p.binding = binding;
    return p;
}

core::Vec4 s4(float x) { return {x, x, x, x}; }

const std::vector<Entry>& entries() {
    static const std::vector<Entry> list = [] {
        std::vector<Entry> e;
        e.reserve(80);
        const auto add = [&e](K kind, const char* type, const char* title, const char* category, const char* description,
                              const char* keywords, std::vector<PinDef> inputs, std::vector<PinDef> outputs) -> NodeDef& {
            Entry x;
            x.kind = kind;
            x.def.type = type;
            x.def.title = title;
            x.def.category = category;
            x.def.description = description;
            x.def.keywords = keywords;
            x.def.inputs = std::move(inputs);
            x.def.outputs = std::move(outputs);
            e.push_back(std::move(x));
            return e.back().def;
        };
        const PinDefault UVB = PinDefault::UV;

        // --- Salida ---
        add(K::Master, "master", "Salida", "Salida",
            "La superficie final. Lo que no se conecta conserva el valor del material (sus texturas y factores).",
            "master output surface fragment vertex",
            {pin("Color base", V3, {0.8f, 0.8f, 0.8f, 0.0f}), pin("Alfa", F, s4(1.0f)), pin("Recorte alfa", F, s4(0.5f)),
             pin("Metal", F, s4(0.0f)), pin("Rugosidad", F, s4(0.5f)), pin("Oclusión", F, s4(1.0f)),
             pin("Emisión", V3, s4(0.0f)), pin("Normal (tangente)", V3, {0.0f, 0.0f, 1.0f, 0.0f}),
             pin("Normal (mundo)", V3, {0.0f, 1.0f, 0.0f, 0.0f}), pin("Desplazamiento (vértice)", V3, s4(0.0f)),
             pin("Normal (vértice)", V3, {0.0f, 1.0f, 0.0f, 0.0f})},
            {})
            .unique = true;

        // --- Entrada ---
        add(K::UV, "uv", "UV", "Entrada", "Coordenadas de textura de la malla.", "uv texcoord coordinates", {},
            {pin("UV", V2)});
        add(K::Time, "time", "Tiempo", "Entrada", "Segundos desde el inicio y su seno y coseno.", "time sine cosine",
            {}, {pin("Tiempo", F), pin("Seno", F), pin("Coseno", F)});
        add(K::Position, "position", "Posición (mundo)", "Entrada", "Posición del punto en el mundo.",
            "position world worldposition", {}, {pin("Posición", V3)});
        add(K::Normal, "normal", "Normal", "Entrada",
            "Normal en el mundo: la del material (con su normal map) y la de la malla.", "normal vector world",
            {}, {pin("Normal", V3), pin("Normal malla", V3)});
        add(K::ViewDir, "view_dir", "Dirección de vista", "Entrada", "Del punto hacia la cámara (normalizada).",
            "view direction camera eye", {}, {pin("Dirección", V3)});
        add(K::Camera, "camera", "Cámara", "Entrada", "Posición de la cámara y su distancia al punto.",
            "camera position distance", {}, {pin("Posición", V3), pin("Distancia", F)});
        add(K::Object, "object", "Objeto", "Entrada", "Posición y escala del objeto que se dibuja.",
            "object position scale transform", {}, {pin("Posición", V3), pin("Escala", V3)});
        add(K::VertexColor, "vertex_color", "Color de vértice", "Entrada",
            "Color por vértice. Las mallas del motor no lo guardan: devuelve blanco.", "vertex color",
            {}, {pin("RGBA", V4), pin("RGB", V3)});
        add(K::MaterialBase, "material", "Material (base)", "Entrada",
            "La superficie que calcula el material (texturas y factores del .crmat) antes del grafo.",
            "material base surface albedo",
            {}, {pin("Color", V3), pin("Alfa", F), pin("Metal", F), pin("Rugosidad", F), pin("Oclusión", F),
                 pin("Emisión", V3), pin("Normal", V3)})
            .fragment_only = true;
        add(K::MainTexture, "main_texture", "Textura del material", "Entrada",
            "La textura de color (Albedo) del material, para leerla con otras UV.", "main texture albedo",
            {}, {pin("Textura", TX)})
            .fragment_only = true;

        // --- Constantes ---
        {
            NodeDef& d = add(K::Float, "float", "Float", "Constantes", "Un número.", "float constant number scalar",
                             {}, {pin("Valor", F)});
            d.fields = NodeField::Value;
            d.value_components = 1;
        }
        {
            NodeDef& d = add(K::Vector2, "vector2", "Vector 2", "Constantes", "Dos números.", "vector2 vec2 constant",
                             {}, {pin("Vector", V2)});
            d.fields = NodeField::Value;
            d.value_components = 2;
        }
        {
            NodeDef& d = add(K::Vector3, "vector3", "Vector 3", "Constantes", "Tres números.", "vector3 vec3 constant",
                             {}, {pin("Vector", V3)});
            d.fields = NodeField::Value;
            d.value_components = 3;
        }
        {
            NodeDef& d = add(K::Vector4, "vector4", "Vector 4", "Constantes", "Cuatro números.", "vector4 vec4 constant",
                             {}, {pin("Vector", V4)});
            d.fields = NodeField::Value;
            d.value_components = 4;
        }
        {
            NodeDef& d = add(K::Color, "color", "Color", "Constantes", "Un color fijo (con alfa).", "color constant rgb",
                             {}, {pin("RGBA", V4), pin("RGB", V3), pin("Alfa", F)});
            d.fields = NodeField::Value;
            d.value_components = 4;
            d.color_value = true;
        }

        // --- Propiedades (salen en el material) ---
        {
            NodeDef& d = add(K::PropFloat, "prop_float", "Propiedad Float", "Propiedades",
                             "Número que se edita en el material.", "property float exposed parameter", {},
                             {pin("Valor", F)});
            d.fields = NodeField::Name | NodeField::Value;
            d.value_components = 1;
        }
        {
            NodeDef& d = add(K::PropRange, "prop_range", "Propiedad Range", "Propiedades",
                             "Número con límites (deslizador en el material).", "property range slider exposed parameter",
                             {}, {pin("Valor", F)});
            d.fields = NodeField::Name | NodeField::Value | NodeField::Range;
            d.value_components = 1;
        }
        {
            NodeDef& d = add(K::PropColor, "prop_color", "Propiedad Color", "Propiedades",
                             "Color que se edita en el material.", "property color tint exposed parameter", {},
                             {pin("Color", V3)});
            d.fields = NodeField::Name | NodeField::Value;
            d.value_components = 3;
            d.color_value = true;
        }
        {
            NodeDef& d = add(K::PropVector, "prop_vector", "Propiedad Vector", "Propiedades",
                             "Vector (x, y, z) que se edita en el material.", "property vector exposed parameter", {},
                             {pin("Vector", V3)});
            d.fields = NodeField::Name | NodeField::Value;
            d.value_components = 3;
        }
        {
            NodeDef& d = add(K::PropTexture, "prop_texture", "Propiedad Textura", "Propiedades",
                             "Textura que se asigna en el material (blanca si no se asigna).",
                             "property texture texture2d exposed parameter", {}, {pin("Textura", TX)});
            d.fields = NodeField::Name | NodeField::Texture;
        }

        // --- Texturas ---
        add(K::SampleTexture, "sample_texture", "Sample Texture 2D", "Texturas", "Lee una textura en unas UV.",
            "sample texture 2d read tex2d",
            {pin("Textura", TX), pin("UV", V2, {}, UVB)},
            {pin("RGBA", V4), pin("RGB", V3), pin("R", F), pin("G", F), pin("B", F), pin("A", F)});
        add(K::SampleNormal, "sample_normal", "Sample Normal Map", "Texturas",
            "Lee un normal map (estilo OpenGL/glTF) y da la normal en espacio tangente.", "sample normal map bump",
            {pin("Textura", TX), pin("UV", V2, {}, UVB), pin("Fuerza", F, s4(1.0f))}, {pin("Normal", V3)});
        add(K::Triplanar, "triplanar", "Triplanar", "Texturas",
            "Proyecta la textura desde los tres ejes del mundo: sin UV ni costuras (rocas, terreno).",
            "triplanar projection world",
            {pin("Textura", TX), pin("Posición", V3, {}, PinDefault::Position),
             pin("Normal", V3, {}, PinDefault::VertexNormal), pin("Escala", F, s4(1.0f)), pin("Mezcla", F, s4(4.0f))},
            {pin("RGBA", V4), pin("RGB", V3)});

        // --- Matematicas ---
        const char* M = "Matemáticas";
        add(K::Add, "add", "Sumar", M, "A + B", "add sum plus", {pin("A", DY), pin("B", DY)}, {pin("Resultado", DY)});
        add(K::Subtract, "subtract", "Restar", M, "A - B", "subtract minus sub", {pin("A", DY), pin("B", DY)},
            {pin("Resultado", DY)});
        add(K::Multiply, "multiply", "Multiplicar", M, "A × B", "multiply mul times",
            {pin("A", DY, s4(1.0f)), pin("B", DY, s4(1.0f))}, {pin("Resultado", DY)});
        add(K::Divide, "divide", "Dividir", M, "A / B", "divide div",
            {pin("A", DY, s4(1.0f)), pin("B", DY, s4(1.0f))}, {pin("Resultado", DY)});
        add(K::Lerp, "lerp", "Lerp (mezclar)", M, "Mezcla A y B según T (0 = A, 1 = B).", "lerp mix blend interpolate",
            {pin("A", DY), pin("B", DY, s4(1.0f)), pin("T", DY, s4(0.5f))}, {pin("Resultado", DY)});
        add(K::Clamp, "clamp", "Clamp", M, "Limita el valor entre Mín y Máx.", "clamp limit",
            {pin("Entrada", DY), pin("Mín", DY), pin("Máx", DY, s4(1.0f))}, {pin("Resultado", DY)});
        add(K::Saturate, "saturate", "Saturate", M, "Limita entre 0 y 1.", "saturate clamp01", {pin("Entrada", DY)},
            {pin("Resultado", DY)});
        add(K::Power, "power", "Potencia", M, "Base elevada a Exp.", "power pow exponent",
            {pin("Base", DY), pin("Exp", DY, s4(2.0f))}, {pin("Resultado", DY)});
        add(K::Dot, "dot", "Producto escalar", M, "dot(A, B)", "dot product", {pin("A", DY), pin("B", DY)},
            {pin("Resultado", F)});
        add(K::Cross, "cross", "Producto vectorial", M, "cross(A, B)", "cross product",
            {pin("A", V3), pin("B", V3, {0.0f, 1.0f, 0.0f, 0.0f})}, {pin("Resultado", V3)});
        add(K::Normalize, "normalize", "Normalizar", M, "El vector con longitud 1.", "normalize unit",
            {pin("Entrada", DY)}, {pin("Resultado", DY)});
        add(K::OneMinus, "one_minus", "Uno menos", M, "1 - Entrada", "one minus invert", {pin("Entrada", DY)},
            {pin("Resultado", DY)});
        add(K::Smoothstep, "smoothstep", "Smoothstep", M, "Paso suave de 0 a 1 entre Borde 0 y Borde 1.",
            "smoothstep smooth step",
            {pin("Borde 0", DY), pin("Borde 1", DY, s4(1.0f)), pin("Entrada", DY)}, {pin("Resultado", DY)});
        add(K::Step, "step", "Step", M, "0 si Entrada < Borde, 1 si no.", "step threshold",
            {pin("Borde", DY, s4(0.5f)), pin("Entrada", DY)}, {pin("Resultado", DY)});
        add(K::Sin, "sin", "Seno", M, "sin(Entrada)", "sin sine wave", {pin("Entrada", DY)}, {pin("Resultado", DY)});
        add(K::Cos, "cos", "Coseno", M, "cos(Entrada)", "cos cosine wave", {pin("Entrada", DY)},
            {pin("Resultado", DY)});
        add(K::Frac, "frac", "Fracción", M, "La parte decimal (fract).", "frac fract fraction",
            {pin("Entrada", DY)}, {pin("Resultado", DY)});
        add(K::Remap, "remap", "Remapear", M, "Pasa Entrada del rango [Ent mín, Ent máx] a [Sal mín, Sal máx].",
            "remap range map",
            {pin("Entrada", DY), pin("Ent mín", DY), pin("Ent máx", DY, s4(1.0f)), pin("Sal mín", DY),
             pin("Sal máx", DY, s4(1.0f))},
            {pin("Resultado", DY)});
        add(K::Abs, "abs", "Absoluto", M, "|Entrada|", "abs absolute", {pin("Entrada", DY)}, {pin("Resultado", DY)});
        add(K::Negate, "negate", "Negar", M, "-Entrada", "negate negative", {pin("Entrada", DY)},
            {pin("Resultado", DY)});
        add(K::Min, "min", "Mínimo", M, "El menor de A y B.", "min minimum", {pin("A", DY), pin("B", DY)},
            {pin("Resultado", DY)});
        add(K::Max, "max", "Máximo", M, "El mayor de A y B.", "max maximum", {pin("A", DY), pin("B", DY)},
            {pin("Resultado", DY)});
        add(K::Floor, "floor", "Floor", M, "Redondea hacia abajo.", "floor round", {pin("Entrada", DY)},
            {pin("Resultado", DY)});
        add(K::Sqrt, "sqrt", "Raíz cuadrada", M, "sqrt(Entrada) (0 si es negativa).", "sqrt square root",
            {pin("Entrada", DY, s4(1.0f))}, {pin("Resultado", DY)});
        add(K::Length, "length", "Longitud", M, "Longitud del vector.", "length magnitude", {pin("Entrada", DY)},
            {pin("Resultado", F)});
        add(K::Distance, "distance", "Distancia", M, "Distancia entre A y B.", "distance",
            {pin("A", DY), pin("B", DY)}, {pin("Resultado", F)});
        add(K::Modulo, "modulo", "Módulo", M, "Resto de A / B (mod).", "modulo mod fmod remainder",
            {pin("A", DY), pin("B", DY, s4(1.0f))}, {pin("Resultado", DY)});

        // --- Ruido ---
        add(K::ValueNoise, "value_noise", "Ruido simple", "Ruido", "Ruido de valor suave (0..1).",
            "value noise simple", {pin("UV", V2, {}, UVB), pin("Escala", F, s4(10.0f))}, {pin("Ruido", F)});
        add(K::PerlinNoise, "perlin_noise", "Ruido Perlin", "Ruido", "Ruido de gradiente (Perlin), 0..1.",
            "perlin gradient noise", {pin("UV", V2, {}, UVB), pin("Escala", F, s4(10.0f))}, {pin("Ruido", F)});
        add(K::Fbm, "fbm", "Ruido fractal", "Ruido", "Cinco capas de ruido (nubes, roca, suciedad), 0..1.",
            "fbm fractal noise clouds", {pin("UV", V2, {}, UVB), pin("Escala", F, s4(4.0f))}, {pin("Ruido", F)});
        add(K::Voronoi, "voronoi", "Voronoi", "Ruido", "Celdas: distancia al centro mas cercano y un valor por celda.",
            "voronoi cellular worley cells",
            {pin("UV", V2, {}, UVB), pin("Escala", F, s4(5.0f)), pin("Aleatorio", F, s4(1.0f))},
            {pin("Distancia", F), pin("Celdas", F)});

        // --- UV ---
        add(K::TilingOffset, "tiling_offset", "Repetir y desplazar", "UV", "UV × Repetición + Desplazamiento.",
            "tiling offset tile scale uv",
            {pin("UV", V2, {}, UVB), pin("Repetición", V2, {1.0f, 1.0f, 0.0f, 0.0f}), pin("Desplazamiento", V2)},
            {pin("UV", V2)});
        add(K::Panner, "panner", "Panner", "UV", "Mueve las UV con el tiempo (agua, lava, cintas).",
            "panner scroll move uv",
            {pin("UV", V2, {}, UVB), pin("Velocidad", V2, {0.1f, 0.0f, 0.0f, 0.0f}),
             pin("Tiempo", F, {}, PinDefault::Time)},
            {pin("UV", V2)});
        add(K::RotateUV, "rotate_uv", "Girar UV", "UV", "Gira las UV alrededor de un centro (radianes).",
            "rotate rotation uv",
            {pin("UV", V2, {}, UVB), pin("Centro", V2, {0.5f, 0.5f, 0.0f, 0.0f}), pin("Ángulo", F)},
            {pin("UV", V2)});

        // --- Superficie ---
        add(K::Fresnel, "fresnel", "Fresnel", "Superficie", "Brillo en el contorno: 0 de frente, 1 en los bordes.",
            "fresnel rim edge",
            {pin("Normal", V3, {}, PinDefault::Normal), pin("Vista", V3, {}, PinDefault::ViewDirection),
             pin("Potencia", F, s4(5.0f))},
            {pin("Resultado", F)});
        add(K::NormalBlend, "normal_blend", "Mezclar normales", "Superficie",
            "Combina dos normales en espacio tangente (Reoriented Normal Mapping).", "normal blend combine detail",
            {pin("A", V3, {0.0f, 0.0f, 1.0f, 0.0f}), pin("B", V3, {0.0f, 0.0f, 1.0f, 0.0f})}, {pin("Normal", V3)});
        add(K::NormalStrength, "normal_strength", "Fuerza de normal", "Superficie",
            "Más o menos relieve en una normal en espacio tangente.", "normal strength intensity",
            {pin("Normal", V3, {0.0f, 0.0f, 1.0f, 0.0f}), pin("Fuerza", F, s4(1.0f))}, {pin("Normal", V3)});

        // --- Canales ---
        add(K::Split, "split", "Separar", "Canales", "Separa los componentes.", "split channels separate",
            {pin("Entrada", DY)}, {pin("R", F), pin("G", F), pin("B", F), pin("A", F)});
        add(K::Combine, "combine", "Combinar", "Canales", "Junta componentes en un vector.",
            "combine merge append channels",
            {pin("R", F), pin("G", F), pin("B", F), pin("A", F, s4(1.0f))},
            {pin("RGBA", V4), pin("RGB", V3), pin("RG", V2)});
        {
            NodeDef& d = add(K::Swizzle, "swizzle", "Swizzle", "Canales",
                             "Reordena componentes con una máscara (xyz, yx, rrr, wzyx...).", "swizzle mask channels",
                             {pin("Entrada", DY)}, {pin("Salida", DY)});
            d.fields = NodeField::Name;
        }
        {
            NodeDef& d = add(K::Custom, "custom", "Código GLSL", "Utilidad",
                             "Una expresión GLSL propia con A, B, C y D (vec4). Ej.: A.xyz * B.x + sin(TIME)",
                             "custom code expression glsl hlsl function",
                             {pin("A", V4), pin("B", V4), pin("C", V4), pin("D", V4)}, {pin("Resultado", DY)});
            d.fields = NodeField::Name | NodeField::Width;
        }
        return e;
    }();
    return list;
}

const Entry* entry(const std::string& type) {
    for (const Entry& e : entries()) {
        if (e.def.type == type) return &e;
    }
    return nullptr;
}

bool isProperty(K k) {
    return k == K::PropFloat || k == K::PropRange || k == K::PropColor || k == K::PropVector || k == K::PropTexture;
}

// "Emisión" -> "emision" (sin mayusculas ni tildes, para buscar pines).
std::string fold(const std::string& text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == 0xC3 && i + 1 < text.size()) {
            const unsigned char d = static_cast<unsigned char>(static_cast<unsigned char>(text[i + 1]) | 0x20u);  // minuscula
            char plain = 0;
            switch (d) {
                case 0xA1: plain = 'a'; break;
                case 0xA9: plain = 'e'; break;
                case 0xAD: plain = 'i'; break;
                case 0xB3: plain = 'o'; break;
                case 0xBA: plain = 'u'; break;
                case 0xBC: plain = 'u'; break;
                case 0xB1: plain = 'n'; break;
                default: break;
            }
            if (plain != 0) {
                out.push_back(plain);
                ++i;
                continue;
            }
        }
        if (c == ' ' || c == '_' || c == '-' || c == '(' || c == ')') continue;
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

const std::vector<std::pair<std::string, std::string>>& helperCode();

// --- Nombres de propiedades ---
// Las propiedades son #define en el .crshader: no pueden llamarse como nada
// que use el codigo generado (campos de Surface, funciones de GLSL, swizzles).
bool reservedName(const std::string& n) {
    static const std::set<std::string> words = {
        // plantilla y Surface/Vertex
        "s", "v", "surface", "vertex", "main", "push", "camera", "weather", "TIME", "CAMERA_POSITION", "Surface",
        "Vertex", "albedo", "alpha", "normal", "metallic", "roughness", "occlusion", "emission", "uv", "worldPosition",
        "vertexNormal", "viewDirection", "position", "model", "flags", "params", "pick_id", "bone_offset", "material",
        "base_color", "emissive", "reflectance", "bones", "surface_params", "albedo_map", "metallic_roughness_map",
        "normal_map", "occlusion_map", "emissive_map", "hash", "noise", "fbm", "fresnel", "remap", "toLinear",
        "SG_SAMPLE", "A", "B", "C", "D",
        // GLSL
        "float", "int", "uint", "bool", "double", "vec2", "vec3", "vec4", "ivec2", "ivec3", "ivec4", "uvec2", "uvec3",
        "uvec4", "bvec2", "bvec3", "bvec4", "mat2", "mat3", "mat4", "sampler2D", "void", "return", "if", "else", "for",
        "while", "do", "break", "continue", "discard", "const", "in", "out", "inout", "uniform", "layout", "true",
        "false", "struct", "switch", "case", "default", "radians", "degrees", "sin", "cos", "tan", "asin", "acos",
        "atan", "sinh", "cosh", "tanh", "pow", "exp", "log", "exp2", "log2", "sqrt", "inversesqrt", "abs", "sign",
        "floor", "trunc", "round", "ceil", "fract", "mod", "modf", "min", "max", "clamp", "mix", "step", "smoothstep",
        "isnan", "isinf", "length", "distance", "dot", "cross", "normalize", "reflect", "refract", "faceforward",
        "transpose", "inverse", "determinant", "texture", "textureLod", "textureGrad", "texelFetch", "textureSize",
        "dFdx", "dFdy", "fwidth", "lessThan", "greaterThan", "equal", "not", "any", "all"};
    if (words.count(n) != 0) return true;
    // Y los nombres que usan las funciones de apoyo (best, cell, angle...).
    static const std::set<std::string> helper_words = [] {
        std::set<std::string> out;
        for (const auto& [key, code] : helperCode()) {
            std::string word;
            for (char c : code + " ") {
                if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
                    word.push_back(c);
                } else {
                    if (!word.empty() && !std::isdigit(static_cast<unsigned char>(word[0]))) out.insert(word);
                    word.clear();
                }
            }
        }
        return out;
    }();
    if (helper_words.count(n) != 0) return true;
    if (n.rfind("sg_", 0) == 0 || n.rfind("gl_", 0) == 0 || n.rfind("cramion", 0) == 0 || n.rfind("CRAMION", 0) == 0) {
        return true;
    }
    // Swizzles (x, xy, rgb, stpq...): romperian los .xyz del codigo.
    if (n.size() <= 4) {
        bool swizzle = true;
        for (char c : n) {
            if (std::string("xyzwrgbastpq").find(c) == std::string::npos) swizzle = false;
        }
        if (swizzle) return true;
    }
    return false;
}

bool identifier(const std::string& n) {
    if (n.empty() || n.size() > 48 || !(std::isalpha(static_cast<unsigned char>(n[0])))) return false;
    for (char c : n) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
    }
    return true;
}

// --- GLSL ---
struct Expr {
    std::string code;
    int width = 1;  // 0 = textura ("" = sin conectar: blanca)
};

std::string num(float v) {
    if (!std::isfinite(v)) v = 0.0f;
    std::ostringstream o;
    o.imbue(std::locale::classic());
    o << std::setprecision(7) << v;
    std::string s = o.str();
    if (s.find_first_of(".eE") == std::string::npos) s += ".0";
    return s;
}

const char* typeName(int w) {
    switch (w) {
        case 1: return "float";
        case 2: return "vec2";
        case 3: return "vec3";
        default: return "vec4";
    }
}

std::string literal(const core::Vec4& v, int w) {
    if (w <= 1) return num(v.x);
    std::string s = std::string(typeName(w)) + "(" + num(v.x) + ", " + num(v.y);
    if (w >= 3) s += ", " + num(v.z);
    if (w >= 4) s += ", " + num(v.w);
    return s + ")";
}

Expr convert(const Expr& e, int w) {
    if (e.width == w || e.width <= 0 || w <= 0) return e;
    if (e.width == 1) return {std::string(typeName(w)) + "(" + e.code + ")", w};
    if (w == 1) return {"(" + e.code + ").x", 1};
    if (w < e.width) return {"(" + e.code + ")." + std::string("xyzw").substr(0, static_cast<std::size_t>(w)), w};
    if (e.width == 2 && w == 3) return {"vec3(" + e.code + ", 0.0)", 3};
    if (e.width == 2 && w == 4) return {"vec4(" + e.code + ", 0.0, 1.0)", 4};
    return {"vec4(" + e.code + ", 1.0)", 4};
}

// Una expresion que se puede repetir sin variable (s.uv, TIME, nombres).
bool simple(const std::string& code) {
    if (code.empty()) return true;
    for (char c : code) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.')) return false;
    }
    return true;
}

// Funciones de apoyo (se anaden solo las que se usan).
const std::vector<std::pair<std::string, std::string>>& helperCode() {
    static const std::vector<std::pair<std::string, std::string>> helpers = {
        {"perlin", R"(vec2 sg_gradient(vec2 p) {
    float a = hash(p) * 6.2831853;
    return vec2(cos(a), sin(a));
}
float sg_perlin(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float a = dot(sg_gradient(i), f);
    float b = dot(sg_gradient(i + vec2(1.0, 0.0)), f - vec2(1.0, 0.0));
    float c = dot(sg_gradient(i + vec2(0.0, 1.0)), f - vec2(0.0, 1.0));
    float d = dot(sg_gradient(i + vec2(1.0, 1.0)), f - vec2(1.0, 1.0));
    return clamp(mix(mix(a, b, u.x), mix(c, d, u.x), u.y) * 0.7071 + 0.5, 0.0, 1.0);
})"},
        {"voronoi", R"(vec2 sg_voronoi(vec2 p, float jitter) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    float best = 8.0;
    float cell = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 o = vec2(float(x), float(y));
            vec2 h = vec2(hash(i + o), hash(i + o + vec2(17.31, 9.73)));
            vec2 r = o + h * jitter - f;
            float d = dot(r, r);
            if (d < best) {
                best = d;
                cell = h.x;
            }
        }
    }
    return vec2(sqrt(best), cell);
})"},
        {"triplanar", R"(vec4 sg_triplanar(sampler2D t, vec3 p, vec3 n, float tiling, float blend) {
    vec3 w = pow(abs(normalize(n)), vec3(max(blend, 0.01)));
    w /= max(w.x + w.y + w.z, 1e-5);
    return SG_SAMPLE(t, p.zy * tiling) * w.x + SG_SAMPLE(t, p.xz * tiling) * w.y + SG_SAMPLE(t, p.xy * tiling) * w.z;
})"},
        {"normal", R"(vec3 sg_unpackNormal(vec4 c, float strength) {
    vec2 xy = c.xy * 2.0 - 1.0;
    float z = sqrt(max(1.0 - dot(xy, xy), 0.0));
    return normalize(vec3(xy * strength, z));
})"},
        {"blend", R"(vec3 sg_blendNormals(vec3 a, vec3 b) {
    vec3 t = a + vec3(0.0, 0.0, 1.0);
    vec3 u = b * vec3(-1.0, -1.0, 1.0);
    return normalize(t * dot(t, u) / max(t.z, 1e-5) - u);
})"},
        {"rotate", R"(vec2 sg_rotate(vec2 uv, vec2 center, float angle) {
    float sa = sin(angle);
    float ca = cos(angle);
    uv -= center;
    return vec2(uv.x * ca - uv.y * sa, uv.x * sa + uv.y * ca) + center;
})"},
        // Normal en espacio tangente -> mundo con la base de las derivadas
        // (sin tangentes de la malla). Solo en fragmentos (dFdx).
        {"tangent", R"(#ifdef CRAMION_FRAGMENT_STAGE
vec3 sg_tangentToWorld(vec3 tn, vec3 n, vec3 p, vec2 uv) {
    vec3 dp1 = dFdx(p);
    vec3 dp2 = dFdy(p);
    vec2 duv1 = dFdx(uv);
    vec2 duv2 = dFdy(uv);
    vec3 dp2perp = cross(dp2, n);
    vec3 dp1perp = cross(n, dp1);
    vec3 t = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 b = dp2perp * duv1.y + dp1perp * duv2.y;
    float scale = inversesqrt(max(max(dot(t, t), dot(b, b)), 1e-20));
    return normalize(t * scale * tn.x - b * scale * tn.y + n * tn.z);
}
#endif)"},
    };
    return helpers;
}

class Gen {
public:
    Gen(const Graph& graph, bool vertex, GenerateResult& result, std::set<std::string>& helpers)
        : graph_(graph), vertex_(vertex), result_(result), helpers_(helpers) {}

    std::vector<std::pair<std::string, int>> lines;  // cuerpo de la funcion (texto, nodo)

    void master(const Node& m, const NodeDef& d);

private:
    const Graph& graph_;
    bool vertex_;
    GenerateResult& result_;
    std::set<std::string>& helpers_;
    std::unordered_map<int, std::vector<Expr>> done_;
    std::unordered_set<int> visiting_;

    void line(int node, const std::string& text) { lines.emplace_back(text, node); }
    void error(int node, const std::string& message) {
        if (result_.error_node == 0) result_.error_node = node;
        result_.errors.push_back(message);
    }
    std::string var(int node, int index) const {
        return "sg_" + std::string(vertex_ ? "v" : "f") + std::to_string(node) + "_" + std::to_string(index);
    }

    std::string uv() const { return vertex_ ? "v.uv" : "s.uv"; }
    std::string position() const { return vertex_ ? "v.position" : "s.worldPosition"; }
    std::string normal() const { return vertex_ ? "v.normal" : "s.normal"; }
    std::string vertexNormal() const { return vertex_ ? "v.normal" : "s.vertexNormal"; }
    std::string view() const { return vertex_ ? "normalize(CAMERA_POSITION - v.position)" : "s.viewDirection"; }

    Expr fallback(const Node& n, int pin, const PinDef& p);
    Expr input(const Node& n, const NodeDef& d, int pin);
    const std::vector<Expr>& node(int id);
    std::vector<Expr> emit(const Node& n, const Entry& e, const std::vector<Expr>& in, int dyn);
};

Expr Gen::fallback(const Node& n, int pin, const PinDef& p) {
    switch (p.binding) {
        case PinDefault::UV: return {uv(), 2};
        case PinDefault::Position: return {position(), 3};
        case PinDefault::Normal: return {normal(), 3};
        case PinDefault::VertexNormal: return {vertexNormal(), 3};
        case PinDefault::ViewDirection: return {view(), 3};
        case PinDefault::Time: result_.uses_time = true; return {"TIME", 1};
        default: break;
    }
    if (p.type == PinType::Texture) return {"", 0};
    const int w = std::max(1, pinWidth(p.type));
    const core::Vec4 value = pin < static_cast<int>(n.inputs.size()) ? n.inputs[static_cast<std::size_t>(pin)] : p.value;
    return {literal(value, w), w};
}

Expr Gen::input(const Node& n, const NodeDef& d, int pin) {
    const PinDef& p = d.inputs[static_cast<std::size_t>(pin)];
    const int w = pinWidth(p.type);
    const Link* link = graph_.linkTo(n.id, pin);
    if (link == nullptr) return fallback(n, pin, p);
    const std::vector<Expr>& source = node(link->from_node);
    if (link->from_pin < 0 || link->from_pin >= static_cast<int>(source.size())) {
        if (!source.empty()) error(n.id, "enlace roto en '" + d.title + "'");
        return fallback(n, pin, p);
    }
    const Expr e = source[static_cast<std::size_t>(link->from_pin)];
    if (p.type == PinType::Texture) {
        if (e.width != 0) {
            error(n.id, "'" + p.name + "' de '" + d.title + "' necesita una textura");
            return {"", 0};
        }
        return e;
    }
    if (e.width == 0) {
        error(n.id, "una textura no es un número: en '" + p.name + "' de '" + d.title +
                        "' conecta un Sample Texture 2D");
        return fallback(n, pin, p);
    }
    return w > 0 ? convert(e, w) : e;
}

const std::vector<Expr>& Gen::node(int id) {
    static const std::vector<Expr> none;
    if (const auto it = done_.find(id); it != done_.end()) return it->second;
    const Node* n = graph_.find(id);
    if (n == nullptr) {
        error(0, "enlace a un nodo que no existe (" + std::to_string(id) + ")");
        return none;
    }
    const Entry* e = entry(n->type);
    if (e == nullptr) {
        error(id, "tipo de nodo desconocido: " + n->type);
        return none;
    }
    if (e->kind == K::Master) {
        error(id, "la Salida no tiene salidas");
        return none;
    }
    if (visiting_.count(id) != 0) {
        error(id, "el grafo tiene un ciclo en '" + e->def.title + "'");
        return none;
    }
    if (e->def.fragment_only && vertex_) {
        error(id, "'" + e->def.title + "' solo funciona en la superficie (no en las entradas de vértice de la Salida)");
        return none;
    }
    visiting_.insert(id);
    std::vector<Expr> in;
    in.reserve(e->def.inputs.size());
    for (std::size_t i = 0; i < e->def.inputs.size(); ++i) in.push_back(input(*n, e->def, static_cast<int>(i)));
    int dyn = 1;
    for (std::size_t i = 0; i < e->def.inputs.size(); ++i) {
        if (e->def.inputs[i].type == PinType::Dynamic) dyn = std::max(dyn, in[i].width);
    }
    for (std::size_t i = 0; i < e->def.inputs.size(); ++i) {
        if (e->def.inputs[i].type == PinType::Dynamic) in[i] = convert(in[i], dyn);
    }
    const std::vector<Expr> outs = emit(*n, *e, in, dyn);
    visiting_.erase(id);
    std::vector<Expr> named;
    named.reserve(outs.size());
    for (std::size_t o = 0; o < outs.size(); ++o) {
        if (outs[o].width <= 0 || simple(outs[o].code)) {
            named.push_back(outs[o]);
            continue;
        }
        const std::string name = var(id, static_cast<int>(o));
        line(id, std::string(typeName(outs[o].width)) + " " + name + " = " + outs[o].code + ";");
        named.push_back({name, outs[o].width});
    }
    return done_[id] = std::move(named);
}

std::vector<Expr> Gen::emit(const Node& n, const Entry& e, const std::vector<Expr>& in, int dyn) {
    const auto c = [&in](std::size_t i) -> const std::string& { return in[i].code; };
    const auto call1 = [&](const char* f) { return std::vector<Expr>{{std::string(f) + "(" + c(0) + ")", dyn}}; };
    const auto call2 = [&](const char* f) {
        return std::vector<Expr>{{std::string(f) + "(" + c(0) + ", " + c(1) + ")", dyn}};
    };
    const auto op = [&](const char* o) { return std::vector<Expr>{{"(" + c(0) + " " + o + " " + c(1) + ")", dyn}}; };
    const std::string temp = var(n.id, 9);  // temporal propio del nodo
    const auto sample = [&](const Expr& tex, const std::string& at) {
        return tex.code.empty() ? std::string("vec4(1.0)") : "SG_SAMPLE(" + tex.code + ", " + at + ")";
    };
    switch (e.kind) {
        case K::Master: return {};
        case K::UV: return {{uv(), 2}};
        case K::Time:
            result_.uses_time = true;
            return {{"TIME", 1}, {"sin(TIME)", 1}, {"cos(TIME)", 1}};
        case K::Position: return {{position(), 3}};
        case K::Normal: return {{normal(), 3}, {vertexNormal(), 3}};
        case K::ViewDir: return {{view(), 3}};
        case K::Camera: return {{"CAMERA_POSITION", 3}, {"distance(CAMERA_POSITION, " + position() + ")", 1}};
        case K::Object:
            return {{"push.model[3].xyz", 3},
                    {"vec3(length(push.model[0].xyz), length(push.model[1].xyz), length(push.model[2].xyz))", 3}};
        case K::VertexColor: return {{"vec4(1.0)", 4}, {"vec3(1.0)", 3}};
        case K::MaterialBase:
            return {{"s.albedo", 3},   {"s.alpha", 1},    {"s.metallic", 1}, {"s.roughness", 1},
                    {"s.occlusion", 1}, {"s.emission", 3}, {"s.normal", 3}};
        case K::MainTexture: return {{"albedo_map", 0}};
        case K::Float: return {{num(n.value.x), 1}};
        case K::Vector2: return {{literal(n.value, 2), 2}};
        case K::Vector3: return {{literal(n.value, 3), 3}};
        case K::Vector4: return {{literal(n.value, 4), 4}};
        case K::Color: return {{literal(n.value, 4), 4}, {literal(n.value, 3), 3}, {num(n.value.w), 1}};
        case K::PropFloat:
        case K::PropRange: return {{n.name, 1}};
        case K::PropColor:
        case K::PropVector: return {{n.name, 3}};
        case K::PropTexture: return {{n.name, 0}};
        case K::SampleTexture:
            line(n.id, "vec4 " + temp + " = " + sample(in[0], c(1)) + ";");
            return {{temp, 4}, {temp + ".rgb", 3}, {temp + ".r", 1}, {temp + ".g", 1}, {temp + ".b", 1}, {temp + ".a", 1}};
        case K::SampleNormal:
            if (in[0].code.empty()) return {{"vec3(0.0, 0.0, 1.0)", 3}};
            helpers_.insert("normal");
            return {{"sg_unpackNormal(" + sample(in[0], c(1)) + ", " + c(2) + ")", 3}};
        case K::Triplanar:
            if (in[0].code.empty()) return {{"vec4(1.0)", 4}, {"vec3(1.0)", 3}};
            helpers_.insert("triplanar");
            line(n.id, "vec4 " + temp + " = sg_triplanar(" + c(0) + ", " + c(1) + ", " + c(2) + ", " + c(3) + ", " +
                           c(4) + ");");
            return {{temp, 4}, {temp + ".rgb", 3}};
        case K::Add: return op("+");
        case K::Subtract: return op("-");
        case K::Multiply: return op("*");
        case K::Divide: return op("/");
        case K::Lerp: return {{"mix(" + c(0) + ", " + c(1) + ", " + c(2) + ")", dyn}};
        case K::Clamp: return {{"clamp(" + c(0) + ", " + c(1) + ", " + c(2) + ")", dyn}};
        case K::Saturate: return {{"clamp(" + c(0) + ", 0.0, 1.0)", dyn}};
        case K::Power: return call2("pow");
        case K::Dot: return {{"dot(" + c(0) + ", " + c(1) + ")", 1}};
        case K::Cross: return {{"cross(" + c(0) + ", " + c(1) + ")", 3}};
        case K::Normalize: return call1("normalize");
        case K::OneMinus: return {{"(1.0 - " + c(0) + ")", dyn}};
        case K::Smoothstep: return {{"smoothstep(" + c(0) + ", " + c(1) + ", " + c(2) + ")", dyn}};
        case K::Step: return call2("step");
        case K::Sin: return call1("sin");
        case K::Cos: return call1("cos");
        case K::Frac: return call1("fract");
        case K::Remap:
            return {{"(" + c(3) + " + (" + c(0) + " - " + c(1) + ") / (" + c(2) + " - " + c(1) + ") * (" + c(4) + " - " +
                         c(3) + "))",
                     dyn}};
        case K::Abs: return call1("abs");
        case K::Negate: return {{"(-" + c(0) + ")", dyn}};
        case K::Min: return call2("min");
        case K::Max: return call2("max");
        case K::Floor: return call1("floor");
        case K::Sqrt: return {{"sqrt(max(" + c(0) + ", 0.0))", dyn}};
        case K::Length: return {{"length(" + c(0) + ")", 1}};
        case K::Distance: return {{"distance(" + c(0) + ", " + c(1) + ")", 1}};
        case K::Modulo: return call2("mod");
        case K::ValueNoise: return {{"noise(" + c(0) + " * " + c(1) + ")", 1}};
        case K::PerlinNoise:
            helpers_.insert("perlin");
            return {{"sg_perlin(" + c(0) + " * " + c(1) + ")", 1}};
        case K::Fbm: return {{"fbm(" + c(0) + " * " + c(1) + ")", 1}};
        case K::Voronoi:
            helpers_.insert("voronoi");
            line(n.id, "vec2 " + temp + " = sg_voronoi(" + c(0) + " * " + c(1) + ", " + c(2) + ");");
            return {{temp + ".x", 1}, {temp + ".y", 1}};
        case K::TilingOffset: return {{"(" + c(0) + " * " + c(1) + " + " + c(2) + ")", 2}};
        case K::Panner: return {{"(" + c(0) + " + " + c(1) + " * " + c(2) + ")", 2}};
        case K::RotateUV:
            helpers_.insert("rotate");
            return {{"sg_rotate(" + c(0) + ", " + c(1) + ", " + c(2) + ")", 2}};
        case K::Fresnel:
            return {{"pow(1.0 - clamp(dot(normalize(" + c(0) + "), normalize(" + c(1) + ")), 0.0, 1.0), max(" + c(2) +
                         ", 0.0))",
                     1}};
        case K::NormalBlend:
            helpers_.insert("blend");
            return {{"sg_blendNormals(" + c(0) + ", " + c(1) + ")", 3}};
        case K::NormalStrength:
            return {{"normalize(vec3((" + c(0) + ").xy * " + c(1) + ", mix(1.0, (" + c(0) + ").z, clamp(" + c(1) +
                         ", 0.0, 1.0))))",
                     3}};
        case K::Split: {
            line(n.id, "vec4 " + temp + " = " + convert(in[0], 4).code + ";");
            return {{temp + ".x", 1}, {temp + ".y", 1}, {temp + ".z", 1}, {temp + ".w", 1}};
        }
        case K::Combine:
            return {{"vec4(" + c(0) + ", " + c(1) + ", " + c(2) + ", " + c(3) + ")", 4},
                    {"vec3(" + c(0) + ", " + c(1) + ", " + c(2) + ")", 3},
                    {"vec2(" + c(0) + ", " + c(1) + ")", 2}};
        case K::Swizzle: {
            std::string mask;
            for (char ch : n.name) {
                const char l = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                const std::size_t rgba = std::string("rgba").find(l);
                mask.push_back(rgba != std::string::npos ? "xyzw"[rgba] : l);
            }
            bool ok = !mask.empty() && mask.size() <= 4;
            for (char ch : mask) {
                const std::size_t index = std::string("xyzw").find(ch);
                if (index == std::string::npos || (dyn > 1 && static_cast<int>(index) >= dyn)) ok = false;
            }
            if (!ok) {
                error(n.id, "máscara de Swizzle no válida: '" + n.name + "' (usa x, y, z, w o r, g, b, a; hasta 4)");
                return {{"0.0", 1}};
            }
            const Expr source = dyn == 1 ? convert(in[0], 4) : in[0];
            return {{"(" + source.code + ")." + mask, static_cast<int>(mask.size())}};
        }
        case K::Custom: {
            const int width = std::clamp(static_cast<int>(std::lround(n.value.x)), 1, 4);
            std::string expression = n.name.empty() ? std::string("A") : n.name;
            std::replace(expression.begin(), expression.end(), '\n', ' ');
            std::replace(expression.begin(), expression.end(), '\r', ' ');
            const std::string type = typeName(width);
            line(n.id, type + " " + temp + ";");
            line(n.id, "{ vec4 A = " + c(0) + "; vec4 B = " + c(1) + "; vec4 C = " + c(2) + "; vec4 D = " + c(3) + "; " +
                           temp + " = " + type + "(" + expression + "); }");
            return {{temp, width}};
        }
    }
    return {};
}

void Gen::master(const Node& m, const NodeDef& d) {
    const auto linked = [&](int pin) { return graph_.linkTo(m.id, pin) != nullptr; };
    // Primero todos los valores en variables y luego se escriben: asi lo que
    // lee la superficie (Material, Normal) ve la de antes del grafo.
    std::vector<std::string> assign;
    const auto value = [&](int pin, const char* name) {
        const Expr e = input(m, d, pin);
        const std::string v = std::string("sg_") + name;
        line(m.id, std::string(typeName(e.width)) + " " + v + " = " + e.code + ";");
        return v;
    };
    if (vertex_) {
        if (linked(kVertexOffset)) assign.push_back("v.position += " + value(kVertexOffset, "offset") + ";");
        if (linked(kVertexNormal)) assign.push_back("v.normal = normalize(" + value(kVertexNormal, "vertex_normal") + ");");
    } else {
        if (linked(kBaseColor)) assign.push_back("s.albedo = " + value(kBaseColor, "albedo") + ";");
        const bool clip_set = linked(kAlphaClip) ||
                              (static_cast<std::size_t>(kAlphaClip) < m.inputs.size() &&
                               std::abs(m.inputs[static_cast<std::size_t>(kAlphaClip)].x - 0.5f) > 1e-4f);
        if (linked(kAlpha) || clip_set) {
            const std::string alpha = linked(kAlpha) ? value(kAlpha, "alpha") : std::string("s.alpha");
            const std::string clip = value(kAlphaClip, "alpha_clip");
            // El motor recorta por debajo de 0.5: se mueve el umbral.
            assign.push_back("s.alpha = " + alpha + " + (0.5 - " + clip + ");");
        }
        if (linked(kMetallic)) assign.push_back("s.metallic = " + value(kMetallic, "metallic") + ";");
        if (linked(kRoughness)) assign.push_back("s.roughness = " + value(kRoughness, "roughness") + ";");
        if (linked(kOcclusion)) assign.push_back("s.occlusion = " + value(kOcclusion, "occlusion") + ";");
        if (linked(kEmission)) assign.push_back("s.emission = " + value(kEmission, "emission") + ";");
        if (linked(kNormalTangent)) {
            helpers_.insert("tangent");
            assign.push_back("s.normal = sg_tangentToWorld(" + value(kNormalTangent, "normal_ts") +
                             ", s.vertexNormal, s.worldPosition, s.uv);");
        }
        if (linked(kNormalWorld)) assign.push_back("s.normal = normalize(" + value(kNormalWorld, "normal_ws") + ");");
    }
    for (const std::string& a : assign) line(m.id, a);
}

// --- CPU ---
using Value = Evaluator::Value;

float fractf(float x) { return x - std::floor(x); }

Value scalar(float x) {
    Value v;
    v.v[0] = v.v[1] = v.v[2] = v.v[3] = x;
    v.width = 1;
    return v;
}

Value vec(float x, float y, float z, float w, int width) {
    Value v;
    v.v[0] = x;
    v.v[1] = y;
    v.v[2] = z;
    v.v[3] = w;
    v.width = width;
    return v;
}

Value vec3v(const core::Vec3& p) { return vec(p.x, p.y, p.z, 0.0f, 3); }

Value conv(const Value& a, int w) {
    if (a.width == w || a.width <= 0 || w <= 0) return a;
    Value r = a;
    r.width = w;
    if (a.width == 1) {
        for (float& x : r.v) x = a.v[0];
        return r;
    }
    if (w > a.width) {
        if (a.width == 2) {
            r.v[2] = 0.0f;
            r.v[3] = 1.0f;
        } else if (a.width == 3) {
            r.v[3] = 1.0f;
        }
    }
    return r;
}

template <class Fn>
Value map1(const Value& a, Fn f) {
    Value r;
    r.width = a.width;
    for (int i = 0; i < 4; ++i) r.v[i] = f(a.v[i]);
    return r;
}
template <class Fn>
Value map2(const Value& a, const Value& b, Fn f) {
    Value r;
    r.width = a.width;
    for (int i = 0; i < 4; ++i) r.v[i] = f(a.v[i], b.v[i]);
    return r;
}
template <class Fn>
Value map3(const Value& a, const Value& b, const Value& c, Fn f) {
    Value r;
    r.width = a.width;
    for (int i = 0; i < 4; ++i) r.v[i] = f(a.v[i], b.v[i], c.v[i]);
    return r;
}

float dotv(const Value& a, const Value& b) {
    float s = 0.0f;
    for (int i = 0; i < std::max(1, a.width); ++i) s += a.v[i] * b.v[i];
    return s;
}

// Igual que hash()/noise()/fbm() de surface_common.glsl.
float hash2(float x, float y) {
    x = fractf(x * 123.34f);
    y = fractf(y * 456.21f);
    const float d = x * (x + 45.32f) + y * (y + 45.32f);
    x += d;
    y += d;
    return fractf(x * y);
}

float smoothstepf(float a, float b, float x) {
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float noise2(float px, float py) {
    const float ix = std::floor(px), iy = std::floor(py);
    const float fx = px - ix, fy = py - iy;
    const float ux = fx * fx * (3.0f - 2.0f * fx), uy = fy * fy * (3.0f - 2.0f * fy);
    const float a = hash2(ix, iy), b = hash2(ix + 1.0f, iy);
    const float c = hash2(ix, iy + 1.0f), d = hash2(ix + 1.0f, iy + 1.0f);
    const float ab = a + (b - a) * ux;
    const float cd = c + (d - c) * ux;
    return ab + (cd - ab) * uy;
}

float fbm2(float px, float py) {
    float sum = 0.0f;
    float amplitude = 0.5f;
    for (int i = 0; i < 5; ++i) {
        sum += noise2(px, py) * amplitude;
        px *= 2.03f;
        py *= 2.03f;
        amplitude *= 0.5f;
    }
    return sum / 0.96875f;
}

float perlin2(float px, float py) {
    const float ix = std::floor(px), iy = std::floor(py);
    const float fx = px - ix, fy = py - iy;
    const float ux = fx * fx * fx * (fx * (fx * 6.0f - 15.0f) + 10.0f);
    const float uy = fy * fy * fy * (fy * (fy * 6.0f - 15.0f) + 10.0f);
    const auto grad = [](float x, float y, float dx, float dy) {
        const float a = hash2(x, y) * 6.2831853f;
        return std::cos(a) * dx + std::sin(a) * dy;
    };
    const float a = grad(ix, iy, fx, fy);
    const float b = grad(ix + 1.0f, iy, fx - 1.0f, fy);
    const float c = grad(ix, iy + 1.0f, fx, fy - 1.0f);
    const float d = grad(ix + 1.0f, iy + 1.0f, fx - 1.0f, fy - 1.0f);
    const float ab = a + (b - a) * ux;
    const float cd = c + (d - c) * ux;
    return std::clamp((ab + (cd - ab) * uy) * 0.7071f + 0.5f, 0.0f, 1.0f);
}

void voronoi2(float px, float py, float jitter, float& distance, float& cell) {
    const float ix = std::floor(px), iy = std::floor(py);
    const float fx = px - ix, fy = py - iy;
    float best = 8.0f;
    cell = 0.0f;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            const float ox = static_cast<float>(x), oy = static_cast<float>(y);
            const float hx = hash2(ix + ox, iy + oy);
            const float hy = hash2(ix + ox + 17.31f, iy + oy + 9.73f);
            const float rx = ox + hx * jitter - fx;
            const float ry = oy + hy * jitter - fy;
            const float d = rx * rx + ry * ry;
            if (d < best) {
                best = d;
                cell = hx;
            }
        }
    }
    distance = std::sqrt(best);
}

Value normalizeV(const Value& a) {
    const float len = std::sqrt(std::max(dotv(a, a), 0.0f));
    if (len <= 1e-12f) return map1(a, [](float) { return 0.0f; });
    return map1(a, [len](float x) { return x / len; });
}

Value cross3(const Value& a, const Value& b) {
    return vec(a.v[1] * b.v[2] - a.v[2] * b.v[1], a.v[2] * b.v[0] - a.v[0] * b.v[2], a.v[0] * b.v[1] - a.v[1] * b.v[0],
               0.0f, 3);
}

const char* kWhite = "*blanca";

}  // namespace

// ---------------------------------------------------------------------------

int pinWidth(PinType type) {
    switch (type) {
        case PinType::Float: return 1;
        case PinType::Vec2: return 2;
        case PinType::Vec3: return 3;
        case PinType::Vec4: return 4;
        case PinType::Texture: return 0;
        default: return -1;
    }
}

const std::vector<NodeDef>& nodeDefinitions() {
    static const std::vector<NodeDef> defs = [] {
        std::vector<NodeDef> d;
        for (const Entry& e : entries()) d.push_back(e.def);
        return d;
    }();
    return defs;
}

const NodeDef* findNodeDef(const std::string& type) {
    const Entry* e = entry(type);
    return e != nullptr ? &e->def : nullptr;
}

int findPin(const NodeDef& def, const std::string& name, bool output) {
    const std::vector<PinDef>& pins = output ? def.outputs : def.inputs;
    if (!name.empty() && std::all_of(name.begin(), name.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
        const int index = std::atoi(name.c_str());
        return index >= 0 && index < static_cast<int>(pins.size()) ? index : -1;
    }
    const std::string wanted = fold(name);
    for (std::size_t i = 0; i < pins.size(); ++i) {
        if (fold(pins[i].name) == wanted) return static_cast<int>(i);
    }
    return -1;
}

bool compatible(PinType from, PinType to) {
    const bool from_texture = from == PinType::Texture;
    const bool to_texture = to == PinType::Texture;
    return from_texture == to_texture;
}

// --- Graph ------------------------------------------------------------------

Node* Graph::find(int id) {
    for (Node& n : nodes) {
        if (n.id == id) return &n;
    }
    return nullptr;
}

const Node* Graph::find(int id) const {
    for (const Node& n : nodes) {
        if (n.id == id) return &n;
    }
    return nullptr;
}

const Node* Graph::master() const {
    for (const Node& n : nodes) {
        if (n.type == "master") return &n;
    }
    return nullptr;
}

int Graph::add(const std::string& type, float x, float y) {
    const Entry* e = entry(type);
    if (e == nullptr) return 0;
    if (e->def.unique) {
        for (const Node& n : nodes) {
            if (n.type == type) return 0;
        }
    }
    Node n;
    n.id = next_id++;
    n.type = type;
    n.x = x;
    n.y = y;
    for (const PinDef& p : e->def.inputs) n.inputs.push_back(p.value);
    switch (e->kind) {
        case K::Float: n.value = {1.0f, 0.0f, 0.0f, 0.0f}; break;
        case K::Color: n.value = {1.0f, 1.0f, 1.0f, 1.0f}; break;
        case K::PropFloat: n.value = {1.0f, 0.0f, 0.0f, 0.0f}; break;
        case K::PropRange: n.value = {0.5f, 0.0f, 0.0f, 0.0f}; break;
        case K::PropColor: n.value = {1.0f, 1.0f, 1.0f, 1.0f}; break;
        case K::PropVector:
        case K::Vector2:
        case K::Vector3:
        case K::Vector4: n.value = {0.0f, 0.0f, 0.0f, 0.0f}; break;
        case K::Swizzle: n.name = "xyz"; break;
        case K::Custom:
            n.name = "A.xyz * B.x";
            n.value = {3.0f, 0.0f, 0.0f, 0.0f};
            break;
        default: break;
    }
    if (isProperty(e->kind)) {
        // Nombre libre: Valor, Valor2...
        const char* base = e->kind == K::PropColor     ? "Color"
                           : e->kind == K::PropVector  ? "Vector"
                           : e->kind == K::PropTexture ? "Textura"
                                                       : "Valor";
        std::string name = base;
        for (int i = 2;; ++i) {
            const bool used = std::any_of(nodes.begin(), nodes.end(), [&](const Node& o) {
                const Entry* oe = entry(o.type);
                return oe != nullptr && isProperty(oe->kind) && o.name == name;
            });
            if (!used) break;
            name = std::string(base) + std::to_string(i);
        }
        n.name = name;
    }
    nodes.push_back(std::move(n));
    return nodes.back().id;
}

void Graph::remove(int id) {
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(), [id](const Node& n) { return n.id == id; }), nodes.end());
    links.erase(std::remove_if(links.begin(), links.end(),
                               [id](const Link& l) { return l.from_node == id || l.to_node == id; }),
                links.end());
}

const Link* Graph::linkTo(int node, int pin) const {
    for (const Link& l : links) {
        if (l.to_node == node && l.to_pin == pin) return &l;
    }
    return nullptr;
}

void Graph::disconnectInput(int node, int pin) {
    links.erase(std::remove_if(links.begin(), links.end(),
                               [node, pin](const Link& l) { return l.to_node == node && l.to_pin == pin; }),
                links.end());
}

bool Graph::connect(int from_node, int from_pin, int to_node, int to_pin, std::string* error) {
    const auto fail = [error](const std::string& m) {
        if (error) *error = m;
        return false;
    };
    const Node* from = find(from_node);
    const Node* to = find(to_node);
    if (from == nullptr || to == nullptr) return fail("el nodo no existe");
    if (from_node == to_node) return fail("un nodo no se puede conectar consigo mismo");
    const NodeDef* fd = findNodeDef(from->type);
    const NodeDef* td = findNodeDef(to->type);
    if (fd == nullptr || td == nullptr) return fail("tipo de nodo desconocido");
    if (from_pin < 0 || from_pin >= static_cast<int>(fd->outputs.size())) return fail("esa salida no existe");
    if (to_pin < 0 || to_pin >= static_cast<int>(td->inputs.size())) return fail("esa entrada no existe");
    if (!compatible(fd->outputs[static_cast<std::size_t>(from_pin)].type, td->inputs[static_cast<std::size_t>(to_pin)].type)) {
        return fail("tipos incompatibles: una textura solo se conecta a una entrada de textura");
    }
    // Ciclo: ¿`from` depende ya de `to`?
    std::vector<int> stack = {from_node};
    std::unordered_set<int> seen;
    while (!stack.empty()) {
        const int current = stack.back();
        stack.pop_back();
        if (current == to_node) return fail("esa conexión crearía un ciclo");
        if (!seen.insert(current).second) continue;
        for (const Link& l : links) {
            if (l.to_node == current) stack.push_back(l.from_node);
        }
    }
    disconnectInput(to_node, to_pin);
    links.push_back({from_node, from_pin, to_node, to_pin});
    return true;
}

void Graph::normalize() {
    int highest = 0;
    for (Node& n : nodes) {
        highest = std::max(highest, n.id);
        const NodeDef* d = findNodeDef(n.type);
        if (d == nullptr) continue;
        while (n.inputs.size() < d->inputs.size()) n.inputs.push_back(d->inputs[n.inputs.size()].value);
        n.inputs.resize(d->inputs.size());
    }
    next_id = std::max(next_id, highest + 1);
    links.erase(std::remove_if(links.begin(), links.end(),
                               [this](const Link& l) {
                                   const Node* a = find(l.from_node);
                                   const Node* b = find(l.to_node);
                                   if (a == nullptr || b == nullptr) return true;
                                   const NodeDef* da = findNodeDef(a->type);
                                   const NodeDef* db = findNodeDef(b->type);
                                   if (da == nullptr || db == nullptr) return true;
                                   return l.from_pin < 0 || l.from_pin >= static_cast<int>(da->outputs.size()) ||
                                          l.to_pin < 0 || l.to_pin >= static_cast<int>(db->inputs.size());
                               }),
                links.end());
    // Una sola entrada por pin (la ultima gana).
    std::set<std::pair<int, int>> taken;
    for (auto it = links.rbegin(); it != links.rend(); ++it) {
        if (!taken.insert({it->to_node, it->to_pin}).second) it->to_node = -1;
    }
    links.erase(std::remove_if(links.begin(), links.end(), [](const Link& l) { return l.to_node == -1; }), links.end());
}

Graph makeDefault() {
    Graph g;
    const int master = g.add("master", 420.0f, -40.0f);
    const int base = g.add("material", -40.0f, -60.0f);
    const int tint = g.add("prop_color", -40.0f, 170.0f);
    if (Node* n = g.find(tint)) n->name = "Tinte";
    const int mul = g.add("multiply", 200.0f, 20.0f);
    g.connect(base, 0, mul, 0);
    g.connect(tint, 0, mul, 1);
    g.connect(mul, 0, master, kBaseColor);
    return g;
}

// --- JSON -------------------------------------------------------------------

namespace {

json vec4j(const core::Vec4& v) { return json::array({v.x, v.y, v.z, v.w}); }

core::Vec4 readVec4(const json& j, core::Vec4 fallback) {
    if (j.is_number()) {
        const float x = j.get<float>();
        return {x, x, x, x};
    }
    if (!j.is_array()) return fallback;
    core::Vec4 v = fallback;
    float* out[4] = {&v.x, &v.y, &v.z, &v.w};
    for (std::size_t i = 0; i < 4 && i < j.size(); ++i) {
        if (j[i].is_number()) *out[i] = j[i].get<float>();
    }
    return v;
}

}  // namespace

std::string serialize(const Graph& graph) {
    json root;
    root["version"] = 1;
    root["description"] = graph.description;
    json nodes = json::array();
    for (const Node& n : graph.nodes) {
        json j;
        j["id"] = n.id;
        j["type"] = n.type;
        j["position"] = json::array({n.x, n.y});
        json inputs = json::array();
        for (const core::Vec4& v : n.inputs) inputs.push_back(vec4j(v));
        j["inputs"] = inputs;
        if (!n.name.empty()) j["name"] = n.name;
        j["value"] = vec4j(n.value);
        const Entry* e = entry(n.type);
        if (e != nullptr && e->kind == K::PropRange) {
            j["min"] = n.min;
            j["max"] = n.max;
        }
        if (!n.texture.empty()) j["texture"] = n.texture;
        if (!n.preview) j["preview"] = false;
        nodes.push_back(j);
    }
    root["nodes"] = nodes;
    json links = json::array();
    for (const Link& l : graph.links) links.push_back(json::array({l.from_node, l.from_pin, l.to_node, l.to_pin}));
    root["links"] = links;
    json groups = json::array();
    for (const Group& g : graph.groups) {
        groups.push_back({{"title", g.title}, {"rect", json::array({g.x, g.y, g.w, g.h})}, {"color", vec4j(g.color)}});
    }
    root["groups"] = groups;
    return root.dump(2);
}

bool parse(const std::string& text, Graph& out, std::string* error) {
    const json root = json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        if (error) *error = "el .crshadergraph no es JSON valido";
        return false;
    }
    Graph g;
    g.description = root.value("description", std::string{});
    if (const auto it = root.find("nodes"); it != root.end() && it->is_array()) {
        for (const json& j : *it) {
            if (!j.is_object()) continue;
            Node n;
            n.id = j.value("id", 0);
            n.type = j.value("type", std::string{});
            if (n.id <= 0 || n.type.empty()) continue;
            if (const auto p = j.find("position"); p != j.end() && p->is_array() && p->size() >= 2) {
                n.x = (*p)[0].is_number() ? (*p)[0].get<float>() : 0.0f;
                n.y = (*p)[1].is_number() ? (*p)[1].get<float>() : 0.0f;
            }
            const NodeDef* d = findNodeDef(n.type);
            if (const auto in = j.find("inputs"); in != j.end()) {
                if (in->is_array()) {
                    for (std::size_t i = 0; i < in->size(); ++i) {
                        const core::Vec4 fallback = d != nullptr && i < d->inputs.size() ? d->inputs[i].value : core::Vec4{};
                        n.inputs.push_back(readVec4((*in)[i], fallback));
                    }
                } else if (in->is_object() && d != nullptr) {
                    // {"B": 2} por nombre (comodo para la IA)
                    for (const PinDef& p : d->inputs) n.inputs.push_back(p.value);
                    for (const auto& [key, value] : in->items()) {
                        const int index = findPin(*d, key, false);
                        if (index >= 0) n.inputs[static_cast<std::size_t>(index)] = readVec4(value, n.inputs[static_cast<std::size_t>(index)]);
                    }
                }
            }
            n.name = j.value("name", std::string{});
            if (const auto v = j.find("value"); v != j.end()) n.value = readVec4(*v, n.value);
            n.min = j.value("min", 0.0f);
            n.max = j.value("max", 1.0f);
            n.texture = j.value("texture", std::string{});
            n.preview = j.value("preview", true);
            if (g.find(n.id) != nullptr) continue;
            g.nodes.push_back(std::move(n));
        }
    }
    if (const auto it = root.find("links"); it != root.end() && it->is_array()) {
        for (const json& j : *it) {
            Link l;
            if (j.is_array() && j.size() >= 4) {
                l = {j[0].get<int>(), j[1].get<int>(), j[2].get<int>(), j[3].get<int>()};
            } else if (j.is_object() && j.contains("from") && j.contains("to") && j["from"].is_array() &&
                       j["to"].is_array() && j["from"].size() >= 2 && j["to"].size() >= 2) {
                l = {j["from"][0].get<int>(), j["from"][1].get<int>(), j["to"][0].get<int>(), j["to"][1].get<int>()};
            } else {
                continue;
            }
            g.links.push_back(l);
        }
    }
    if (const auto it = root.find("groups"); it != root.end() && it->is_array()) {
        for (const json& j : *it) {
            if (!j.is_object()) continue;
            Group gr;
            gr.title = j.value("title", std::string("Grupo"));
            if (const auto r = j.find("rect"); r != j.end() && r->is_array() && r->size() >= 4) {
                gr.x = (*r)[0].get<float>();
                gr.y = (*r)[1].get<float>();
                gr.w = std::max(60.0f, (*r)[2].get<float>());
                gr.h = std::max(40.0f, (*r)[3].get<float>());
            }
            if (const auto c = j.find("color"); c != j.end()) gr.color = readVec4(*c, gr.color);
            g.groups.push_back(gr);
        }
    }
    g.normalize();
    out = std::move(g);
    return true;
}

bool load(const std::filesystem::path& file, Graph& out, std::string* error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        if (error) *error = "no se pudo abrir " + crdata::utf8(file);
        return false;
    }
    std::stringstream s;
    s << in.rdbuf();
    return parse(s.str(), out, error);
}

bool save(const Graph& graph, const std::filesystem::path& file) {
    std::error_code e;
    std::filesystem::create_directories(file.parent_path(), e);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << serialize(graph);
    return static_cast<bool>(out);
}

// --- Generacion -------------------------------------------------------------

bool generate(const Graph& graph, const std::string& name, GenerateResult& out) {
    out = GenerateResult{};
    std::vector<std::pair<std::string, int>> lines;
    const auto add = [&lines](const std::string& text, int node = 0) { lines.emplace_back(text, node); };
    add("// Cramion Shader Graph: generado desde " + name + kShaderGraphExtension);
    add("// No lo edites a mano: se rehace al guardar el grafo (doble clic en el .crshadergraph).");
    if (!graph.description.empty()) {
        std::string first = graph.description.substr(0, graph.description.find('\n'));
        add("// " + first);
    }
    add("");

    // Propiedades: una linea `property` por nombre.
    std::map<std::string, K> declared;
    int values = 0;
    int textures = 0;
    for (const Node& n : graph.nodes) {
        const Entry* e = entry(n.type);
        if (e == nullptr || !isProperty(e->kind)) continue;
        if (!identifier(n.name)) {
            out.errors.push_back("nombre de propiedad no válido: '" + n.name + "' (letras, números y _; empieza por letra)");
            if (out.error_node == 0) out.error_node = n.id;
            continue;
        }
        if (reservedName(n.name)) {
            out.errors.push_back("'" + n.name + "' es un nombre reservado del shader: elige otro para la propiedad");
            if (out.error_node == 0) out.error_node = n.id;
            continue;
        }
        if (const auto it = declared.find(n.name); it != declared.end()) {
            if (it->second != e->kind) {
                out.errors.push_back("la propiedad '" + n.name + "' está repetida con otro tipo");
                if (out.error_node == 0) out.error_node = n.id;
            }
            continue;  // misma propiedad usada en varios sitios
        }
        declared[n.name] = e->kind;
        std::string line;
        switch (e->kind) {
            case K::PropFloat: line = "property float " + n.name + " = " + num(n.value.x); break;
            case K::PropRange: {
                const float lo = std::min(n.min, n.max);
                const float hi = std::max(n.min, n.max);
                line = "property range " + n.name + " = " + num(std::clamp(n.value.x, lo, hi)) + " (" + num(lo) + ", " +
                       num(hi) + ")";
                break;
            }
            case K::PropColor:
                line = "property color " + n.name + " = " + num(n.value.x) + ", " + num(n.value.y) + ", " + num(n.value.z);
                break;
            case K::PropVector:
                line = "property vector " + n.name + " = " + num(n.value.x) + ", " + num(n.value.y) + ", " + num(n.value.z);
                break;
            default: line = "property texture " + n.name; break;
        }
        if (e->kind == K::PropTexture) {
            ++textures;
        } else {
            ++values;
        }
        add(line, n.id);
    }
    if (values > kMaxShaderValues) {
        out.errors.push_back("demasiadas propiedades numéricas (" + std::to_string(values) + "): como mucho " +
                             std::to_string(kMaxShaderValues) + " float/range/color/vector");
    }
    if (textures > kMaxShaderTextures) {
        out.errors.push_back("demasiadas propiedades de textura (" + std::to_string(textures) + "): como mucho " +
                             std::to_string(kMaxShaderTextures));
    }

    std::set<std::string> helpers;
    Gen fragment(graph, false, out, helpers);
    Gen vertex(graph, true, out, helpers);
    const Node* m = graph.master();
    if (m == nullptr) {
        out.errors.push_back("el grafo no tiene nodo Salida");
    } else if (const NodeDef* d = findNodeDef(m->type)) {
        fragment.master(*m, *d);
        vertex.master(*m, *d);
    }

    add("");
    add("// Lectura de texturas: en los vertices sin derivadas (nivel 0).");
    add("#ifdef CRAMION_VERTEX_STAGE");
    add("#define SG_SAMPLE(t, uv) textureLod(t, uv, 0.0)");
    add("#else");
    add("#define SG_SAMPLE(t, uv) texture(t, uv)");
    add("#endif");
    for (const auto& [key, code] : helperCode()) {
        if (helpers.count(key) == 0) continue;
        add("");
        std::istringstream in(code);
        std::string l;
        while (std::getline(in, l)) add(l);
    }
    add("");
    add("void surface(inout Surface s) {");
    for (const auto& [text, node] : fragment.lines) add("    " + text, node);
    add("}");
    if (!vertex.lines.empty()) {
        out.has_vertex = true;
        add("");
        add("void vertex(inout Vertex v) {");
        for (const auto& [text, node] : vertex.lines) add("    " + text, node);
        add("}");
    }

    std::ostringstream code;
    out.line_nodes.assign(1, 0);  // las lineas empiezan en 1
    for (const auto& [text, node] : lines) {
        code << text << "\n";
        out.line_nodes.push_back(node);
    }
    out.code = code.str();
    return out.errors.empty();
}

std::filesystem::path generatedShaderPath(const std::filesystem::path& graph_file) {
    std::filesystem::path p = graph_file;
    p.replace_extension(kSurfaceShaderExtension);
    return p;
}

std::filesystem::path graphForShader(const std::filesystem::path& shader_file) {
    std::filesystem::path p = shader_file;
    p.replace_extension(kShaderGraphExtension);
    std::error_code e;
    return std::filesystem::exists(p, e) ? p : std::filesystem::path{};
}

bool isGeneratedShader(const std::string& text) {
    return text.substr(0, 400).find("Cramion Shader Graph: generado") != std::string::npos;
}

bool writeGeneratedShader(const Graph& graph, const std::filesystem::path& graph_file, GenerateResult& result,
                          std::string* error) {
    const std::filesystem::path target = generatedShaderPath(graph_file);
    std::string existing;
    {
        std::ifstream in(target, std::ios::binary);
        if (in) {
            std::stringstream s;
            s << in.rdbuf();
            existing = s.str();
        }
    }
    if (!existing.empty() && !isGeneratedShader(existing)) {
        if (error) {
            *error = "ya existe " + crdata::utf8(target.filename()) +
                     " escrito a mano: renombra el grafo o ese shader para no perderlo";
        }
        return false;
    }
    if (!generate(graph, crdata::utf8(graph_file.stem()), result)) {
        if (error) *error = result.errors.empty() ? std::string("error al generar") : result.errors.front();
        return false;
    }
    if (existing == result.code) return true;  // igual: no recompilar
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    out << result.code;
    if (!out) {
        if (error) *error = "no se pudo escribir " + crdata::utf8(target);
        return false;
    }
    return true;
}

int nodeForErrorLine(const GenerateResult& result, const std::string& compile_error) {
    const std::string marker = std::string(kSurfaceShaderExtension) + ":";
    const std::size_t at = compile_error.find(marker);
    if (at == std::string::npos) return 0;
    const int line = std::atoi(compile_error.c_str() + at + marker.size());
    if (line <= 0 || line >= static_cast<int>(result.line_nodes.size())) return 0;
    return result.line_nodes[static_cast<std::size_t>(line)];
}

// --- Evaluador en CPU -------------------------------------------------------

Evaluator::Evaluator(const Graph& graph, TextureSampler sampler) : graph_(graph), sampler_(std::move(sampler)) {}

Evaluator::Value Evaluator::input(const Node& node, int pin, int depth) {
    const NodeDef* d = findNodeDef(node.type);
    if (d == nullptr || pin < 0 || pin >= static_cast<int>(d->inputs.size())) return scalar(0.0f);
    const PinDef& p = d->inputs[static_cast<std::size_t>(pin)];
    const int w = pinWidth(p.type);
    if (const Link* link = graph_.linkTo(node.id, pin)) {
        const std::vector<Value>& source = outputs(link->from_node, depth + 1);
        if (link->from_pin >= 0 && link->from_pin < static_cast<int>(source.size())) {
            const Value& v = source[static_cast<std::size_t>(link->from_pin)];
            if (p.type == PinType::Texture) {
                if (v.width == 0) return v;
            } else if (v.width > 0) {
                return w > 0 ? conv(v, w) : v;
            }
        }
    }
    switch (p.binding) {
        case PinDefault::UV: return vec(point_.uv.x, point_.uv.y, 0.0f, 0.0f, 2);
        case PinDefault::Position: return vec3v(point_.position);
        case PinDefault::Normal:
        case PinDefault::VertexNormal: return vec3v(point_.normal);
        case PinDefault::ViewDirection: return vec3v(point_.view);
        case PinDefault::Time: return scalar(point_.time);
        default: break;
    }
    if (p.type == PinType::Texture) {
        Value t;
        t.width = 0;
        t.texture = kWhite;
        return t;
    }
    const core::Vec4 v = pin < static_cast<int>(node.inputs.size()) ? node.inputs[static_cast<std::size_t>(pin)] : p.value;
    return w <= 1 ? scalar(v.x) : vec(v.x, v.y, v.z, v.w, w);
}

const std::vector<Evaluator::Value>& Evaluator::outputs(int id, int depth) {
    static const std::vector<Value> none;
    if (const auto it = memo_.find(id); it != memo_.end()) return it->second;
    const Node* n = graph_.find(id);
    const Entry* e = n != nullptr ? entry(n->type) : nullptr;
    if (e == nullptr || depth > 64 || e->kind == K::Master) return none;
    std::vector<Value> in;
    for (std::size_t i = 0; i < e->def.inputs.size(); ++i) in.push_back(input(*n, static_cast<int>(i), depth));
    int dyn = 1;
    for (std::size_t i = 0; i < e->def.inputs.size(); ++i) {
        if (e->def.inputs[i].type == PinType::Dynamic) dyn = std::max(dyn, in[i].width);
    }
    for (std::size_t i = 0; i < e->def.inputs.size(); ++i) {
        if (e->def.inputs[i].type == PinType::Dynamic) in[i] = conv(in[i], dyn);
    }
    const auto sample = [this](const Value& tex, float u, float v) -> Value {
        if (tex.texture == kWhite) return vec(1.0f, 1.0f, 1.0f, 1.0f, 4);
        core::Vec4 c{1.0f, 1.0f, 1.0f, 1.0f};
        if (sampler_) c = sampler_(tex.texture, core::Vec2{u, v});
        return vec(c.x, c.y, c.z, c.w, 4);
    };
    const core::Vec3 camera{0.0f, 0.0f, 3.0f};
    std::vector<Value> o;
    const auto a = [&in](std::size_t i) -> const Value& { return in[i]; };
    switch (e->kind) {
        case K::Master: break;
        case K::UV: o = {vec(point_.uv.x, point_.uv.y, 0.0f, 0.0f, 2)}; break;
        case K::Time: o = {scalar(point_.time), scalar(std::sin(point_.time)), scalar(std::cos(point_.time))}; break;
        case K::Position: o = {vec3v(point_.position)}; break;
        case K::Normal: o = {vec3v(point_.normal), vec3v(point_.normal)}; break;
        case K::ViewDir: o = {vec3v(point_.view)}; break;
        case K::Camera: {
            const core::Vec3 d = camera - point_.position;
            o = {vec3v(camera), scalar(core::length(d))};
            break;
        }
        case K::Object: o = {vec(0.0f, 0.0f, 0.0f, 0.0f, 3), vec(1.0f, 1.0f, 1.0f, 0.0f, 3)}; break;
        case K::VertexColor: o = {vec(1.0f, 1.0f, 1.0f, 1.0f, 4), vec(1.0f, 1.0f, 1.0f, 0.0f, 3)}; break;
        case K::MaterialBase: {
            const PreviewSurface base;
            o = {vec(base.albedo.x, base.albedo.y, base.albedo.z, 0.0f, 3), scalar(base.alpha), scalar(base.metallic),
                 scalar(base.roughness), scalar(base.occlusion), vec(0.0f, 0.0f, 0.0f, 0.0f, 3), vec3v(point_.normal)};
            break;
        }
        case K::MainTexture: {
            Value t;
            t.width = 0;
            o = {t};
            break;
        }
        case K::Float: o = {scalar(n->value.x)}; break;
        case K::Vector2: o = {vec(n->value.x, n->value.y, 0.0f, 0.0f, 2)}; break;
        case K::Vector3: o = {vec(n->value.x, n->value.y, n->value.z, 0.0f, 3)}; break;
        case K::Vector4: o = {vec(n->value.x, n->value.y, n->value.z, n->value.w, 4)}; break;
        case K::Color:
            o = {vec(n->value.x, n->value.y, n->value.z, n->value.w, 4), vec(n->value.x, n->value.y, n->value.z, 0.0f, 3),
                 scalar(n->value.w)};
            break;
        case K::PropFloat: o = {scalar(n->value.x)}; break;
        case K::PropRange: o = {scalar(std::clamp(n->value.x, std::min(n->min, n->max), std::max(n->min, n->max)))}; break;
        case K::PropColor:
        case K::PropVector: o = {vec(n->value.x, n->value.y, n->value.z, 0.0f, 3)}; break;
        case K::PropTexture: {
            Value t;
            t.width = 0;
            t.texture = n->texture.empty() ? std::string(kWhite) : n->texture;
            o = {t};
            break;
        }
        case K::SampleTexture: {
            const Value c = sample(a(0), a(1).v[0], a(1).v[1]);
            o = {c, vec(c.v[0], c.v[1], c.v[2], 0.0f, 3), scalar(c.v[0]), scalar(c.v[1]), scalar(c.v[2]), scalar(c.v[3])};
            break;
        }
        case K::SampleNormal: {
            if (a(0).texture == kWhite) {
                o = {vec(0.0f, 0.0f, 1.0f, 0.0f, 3)};
                break;
            }
            const Value c = sample(a(0), a(1).v[0], a(1).v[1]);
            const float x = c.v[0] * 2.0f - 1.0f, y = c.v[1] * 2.0f - 1.0f;
            const float z = std::sqrt(std::max(1.0f - x * x - y * y, 0.0f));
            o = {normalizeV(vec(x * a(2).v[0], y * a(2).v[0], z, 0.0f, 3))};
            break;
        }
        case K::Triplanar: {
            const Value& p = a(1);
            const Value nn = normalizeV(a(2));
            const float blend = std::max(a(4).v[0], 0.01f);
            float w[3] = {std::pow(std::abs(nn.v[0]), blend), std::pow(std::abs(nn.v[1]), blend),
                          std::pow(std::abs(nn.v[2]), blend)};
            const float sum = std::max(w[0] + w[1] + w[2], 1e-5f);
            const float t = a(3).v[0];
            const Value x = sample(a(0), p.v[2] * t, p.v[1] * t);
            const Value y = sample(a(0), p.v[0] * t, p.v[2] * t);
            const Value z = sample(a(0), p.v[0] * t, p.v[1] * t);
            Value r;
            r.width = 4;
            for (int i = 0; i < 4; ++i) r.v[i] = (x.v[i] * w[0] + y.v[i] * w[1] + z.v[i] * w[2]) / sum;
            o = {r, vec(r.v[0], r.v[1], r.v[2], 0.0f, 3)};
            break;
        }
        case K::Add: o = {map2(a(0), a(1), [](float x, float y) { return x + y; })}; break;
        case K::Subtract: o = {map2(a(0), a(1), [](float x, float y) { return x - y; })}; break;
        case K::Multiply: o = {map2(a(0), a(1), [](float x, float y) { return x * y; })}; break;
        case K::Divide: o = {map2(a(0), a(1), [](float x, float y) { return x / y; })}; break;
        case K::Lerp: o = {map3(a(0), a(1), a(2), [](float x, float y, float t) { return x + (y - x) * t; })}; break;
        case K::Clamp:
            o = {map3(a(0), a(1), a(2), [](float x, float lo, float hi) { return std::min(std::max(x, lo), hi); })};
            break;
        case K::Saturate: o = {map1(a(0), [](float x) { return std::clamp(x, 0.0f, 1.0f); })}; break;
        case K::Power: o = {map2(a(0), a(1), [](float x, float y) { return std::pow(x, y); })}; break;
        case K::Dot: o = {scalar(dotv(a(0), a(1)))}; break;
        case K::Cross: o = {cross3(a(0), a(1))}; break;
        case K::Normalize: o = {normalizeV(a(0))}; break;
        case K::OneMinus: o = {map1(a(0), [](float x) { return 1.0f - x; })}; break;
        case K::Smoothstep: o = {map3(a(0), a(1), a(2), smoothstepf)}; break;
        case K::Step: o = {map2(a(0), a(1), [](float edge, float x) { return x < edge ? 0.0f : 1.0f; })}; break;
        case K::Sin: o = {map1(a(0), [](float x) { return std::sin(x); })}; break;
        case K::Cos: o = {map1(a(0), [](float x) { return std::cos(x); })}; break;
        case K::Frac: o = {map1(a(0), fractf)}; break;
        case K::Remap: {
            Value r;
            r.width = a(0).width;
            for (int i = 0; i < 4; ++i) {
                r.v[i] = a(3).v[i] + (a(0).v[i] - a(1).v[i]) / (a(2).v[i] - a(1).v[i]) * (a(4).v[i] - a(3).v[i]);
            }
            o = {r};
            break;
        }
        case K::Abs: o = {map1(a(0), [](float x) { return std::abs(x); })}; break;
        case K::Negate: o = {map1(a(0), [](float x) { return -x; })}; break;
        case K::Min: o = {map2(a(0), a(1), [](float x, float y) { return std::min(x, y); })}; break;
        case K::Max: o = {map2(a(0), a(1), [](float x, float y) { return std::max(x, y); })}; break;
        case K::Floor: o = {map1(a(0), [](float x) { return std::floor(x); })}; break;
        case K::Sqrt: o = {map1(a(0), [](float x) { return std::sqrt(std::max(x, 0.0f)); })}; break;
        case K::Length: o = {scalar(std::sqrt(std::max(dotv(a(0), a(0)), 0.0f)))}; break;
        case K::Distance: {
            const Value d = map2(a(0), a(1), [](float x, float y) { return x - y; });
            o = {scalar(std::sqrt(std::max(dotv(d, d), 0.0f)))};
            break;
        }
        case K::Modulo:
            o = {map2(a(0), a(1), [](float x, float y) { return x - y * std::floor(x / y); })};
            break;
        case K::ValueNoise: o = {scalar(noise2(a(0).v[0] * a(1).v[0], a(0).v[1] * a(1).v[0]))}; break;
        case K::PerlinNoise: o = {scalar(perlin2(a(0).v[0] * a(1).v[0], a(0).v[1] * a(1).v[0]))}; break;
        case K::Fbm: o = {scalar(fbm2(a(0).v[0] * a(1).v[0], a(0).v[1] * a(1).v[0]))}; break;
        case K::Voronoi: {
            float distance = 0.0f, cell = 0.0f;
            voronoi2(a(0).v[0] * a(1).v[0], a(0).v[1] * a(1).v[0], a(2).v[0], distance, cell);
            o = {scalar(distance), scalar(cell)};
            break;
        }
        case K::TilingOffset:
            o = {vec(a(0).v[0] * a(1).v[0] + a(2).v[0], a(0).v[1] * a(1).v[1] + a(2).v[1], 0.0f, 0.0f, 2)};
            break;
        case K::Panner:
            o = {vec(a(0).v[0] + a(1).v[0] * a(2).v[0], a(0).v[1] + a(1).v[1] * a(2).v[0], 0.0f, 0.0f, 2)};
            break;
        case K::RotateUV: {
            const float sa = std::sin(a(2).v[0]), ca = std::cos(a(2).v[0]);
            const float u = a(0).v[0] - a(1).v[0], v = a(0).v[1] - a(1).v[1];
            o = {vec(u * ca - v * sa + a(1).v[0], u * sa + v * ca + a(1).v[1], 0.0f, 0.0f, 2)};
            break;
        }
        case K::Fresnel: {
            const float d = std::clamp(dotv(normalizeV(a(0)), normalizeV(a(1))), 0.0f, 1.0f);
            o = {scalar(std::pow(1.0f - d, std::max(a(2).v[0], 0.0f)))};
            break;
        }
        case K::NormalBlend: {
            const Value t = vec(a(0).v[0], a(0).v[1], a(0).v[2] + 1.0f, 0.0f, 3);
            const Value u = vec(-a(1).v[0], -a(1).v[1], a(1).v[2], 0.0f, 3);
            const float k = dotv(t, u) / std::max(t.v[2], 1e-5f);
            o = {normalizeV(vec(t.v[0] * k - u.v[0], t.v[1] * k - u.v[1], t.v[2] * k - u.v[2], 0.0f, 3))};
            break;
        }
        case K::NormalStrength: {
            const float s = a(1).v[0];
            const float z = 1.0f + (a(0).v[2] - 1.0f) * std::clamp(s, 0.0f, 1.0f);
            o = {normalizeV(vec(a(0).v[0] * s, a(0).v[1] * s, z, 0.0f, 3))};
            break;
        }
        case K::Split: {
            const Value v = conv(a(0), 4);
            o = {scalar(v.v[0]), scalar(v.v[1]), scalar(v.v[2]), scalar(v.v[3])};
            break;
        }
        case K::Combine:
            o = {vec(a(0).v[0], a(1).v[0], a(2).v[0], a(3).v[0], 4), vec(a(0).v[0], a(1).v[0], a(2).v[0], 0.0f, 3),
                 vec(a(0).v[0], a(1).v[0], 0.0f, 0.0f, 2)};
            break;
        case K::Swizzle: {
            const Value source = conv(a(0), 4);
            Value r;
            r.width = 0;
            for (char ch : n->name) {
                const char l = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                std::size_t index = std::string("xyzw").find(l);
                if (index == std::string::npos) index = std::string("rgba").find(l);
                if (index == std::string::npos || r.width >= 4) {
                    r.width = 0;
                    break;
                }
                r.v[r.width++] = source.v[index];
            }
            if (r.width == 0) r = scalar(0.0f);
            o = {r};
            break;
        }
        case K::Custom: {
            // No se interpreta GLSL en CPU: gris en la vista previa.
            const int width = std::clamp(static_cast<int>(std::lround(n->value.x)), 1, 4);
            o = {conv(scalar(0.5f), width)};
            break;
        }
    }
    for (Value& v : o) {
        for (float& x : v.v) {
            if (!std::isfinite(x)) x = 0.0f;
        }
    }
    return memo_[id] = std::move(o);
}

core::Vec4 Evaluator::output(const PreviewPoint& point, int node, int pin, int* width) {
    point_ = point;
    memo_.clear();
    const std::vector<Value>& o = outputs(node, 0);
    if (pin < 0 || pin >= static_cast<int>(o.size())) {
        if (width) *width = 1;
        return {};
    }
    const Value& v = o[static_cast<std::size_t>(pin)];
    if (width) *width = v.width;
    if (v.width == 0) {
        // Una textura: se ve leida en las UV.
        if (v.texture == kWhite) return {1.0f, 1.0f, 1.0f, 1.0f};
        return sampler_ ? sampler_(v.texture, point.uv) : core::Vec4{1.0f, 1.0f, 1.0f, 1.0f};
    }
    const Value full = conv(v, 4);
    if (v.width == 1) return {v.v[0], v.v[0], v.v[0], 1.0f};
    return {full.v[0], full.v[1], v.width >= 3 ? full.v[2] : 0.0f, 1.0f};
}

PreviewSurface Evaluator::surface(const PreviewPoint& point) {
    point_ = point;
    memo_.clear();
    PreviewSurface s;
    s.normal = point.normal;
    const Node* m = graph_.master();
    if (m == nullptr) return s;
    const auto linked = [&](int pin) { return graph_.linkTo(m->id, pin) != nullptr; };
    const auto get = [&](int pin) { return input(*m, pin, 0); };
    if (linked(kBaseColor)) {
        const Value v = get(kBaseColor);
        s.albedo = {v.v[0], v.v[1], v.v[2]};
    }
    const float clip = get(kAlphaClip).v[0];
    if (linked(kAlpha)) s.alpha = get(kAlpha).v[0];
    s.alpha += 0.5f - clip;
    if (linked(kMetallic)) s.metallic = get(kMetallic).v[0];
    if (linked(kRoughness)) s.roughness = get(kRoughness).v[0];
    if (linked(kOcclusion)) s.occlusion = get(kOcclusion).v[0];
    if (linked(kEmission)) {
        const Value v = get(kEmission);
        s.emission = {v.v[0], v.v[1], v.v[2]};
    }
    if (linked(kNormalTangent)) {
        // Base tangente aproximada de la esfera: T hacia +x, B hacia +y.
        const core::Vec3 n = point.normal;
        core::Vec3 t = core::cross(core::Vec3{0.0f, 1.0f, 0.0f}, n);
        if (core::length(t) < 1e-4f) t = core::Vec3{1.0f, 0.0f, 0.0f};
        t = core::normalize(t);
        const core::Vec3 b = core::cross(n, t);
        const Value tn = get(kNormalTangent);
        s.normal = core::normalize(t * tn.v[0] + b * tn.v[1] + n * tn.v[2]);
    }
    if (linked(kNormalWorld)) {
        const Value v = get(kNormalWorld);
        s.normal = core::normalize(core::Vec3{v.v[0], v.v[1], v.v[2]});
    }
    s.metallic = std::clamp(s.metallic, 0.0f, 1.0f);
    s.roughness = std::clamp(s.roughness, 0.04f, 1.0f);
    s.occlusion = std::clamp(s.occlusion, 0.0f, 1.0f);
    return s;
}

}  // namespace cramion::assets::shadergraph

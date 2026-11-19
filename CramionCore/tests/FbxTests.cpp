// Pruebas del cargador de FBX con ufbx (consola, sin GPU) y de las formas
// de mezcla (morph targets).
//
//   CramionFbxTests                 FBX escritos aqui mismo (siempre)
//   CramionFbxTests <carpeta>...    ademas, paridad con assimp de cada .fbx
//                                   de esas carpetas (recursivo)
//
// La variable CRAMION_FBX_PARITY_DIRS (separadas por ';') anade carpetas. La
// paridad carga cada FBX con los dos cargadores (pieza animada y partida en
// piezas) y compara triangulos, vertices, huesos, nodos, tomas, duraciones y
// la pose global de los nodos en varios tiempos, con tolerancia relativa al
// tamano del modelo. Escribe una linea por diferencia.
// Devuelve 0 si las pruebas fijas pasan (las diferencias de paridad de
// carpetas externas se informan pero no cuentan como fallo salvo con
// --strict).

#include "CramionCore/asset/Importer.h"
#include "CramionCore/cvar/CVar.h"

#include <CramionFX/asset/Model.h>
#include <CramionFX/asset/ModelCache.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace cramion;
using core::Mat4;
using core::Quat;
using core::Vec3;

namespace {

int failures = 0;
int checks = 0;

void check(bool condition, const char* what) {
    ++checks;
    std::printf("  %s %s\n", condition ? "OK   " : "FALLO", what);
    if (!condition) ++failures;
}

std::filesystem::path fromUtf8(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

bool approx(float a, float b, float tolerance) { return std::abs(a - b) <= tolerance; }
bool approx(const Vec3& a, const Vec3& b, float tolerance) {
    return approx(a.x, b.x, tolerance) && approx(a.y, b.y, tolerance) && approx(a.z, b.z, tolerance);
}

// --- FBX ASCII de prueba --------------------------------------------------------

// Una caja de 1 m (en cm: UnitScaleFactor 1) de y=0 a y=1 con dos huesos
// (Root abajo, Tip arriba), una forma "Bulge" que sube la cara de arriba
// 0.5, un material rojo y una toma "Wave" de 1 s: Tip gira 90 grados en Z y
// la forma va de 0 a 100 %.
std::string cubeGeometry(long long id, const char* name) {
    std::ostringstream s;
    s << "\tGeometry: " << id << ", \"Geometry::" << name << "\", \"Mesh\" {\n"
      << "\t\tVertices: *24 {\n\t\t\ta: -0.5,0,-0.5,0.5,0,-0.5,0.5,0,0.5,-0.5,0,0.5,-0.5,1,-0.5,0.5,1,-0.5,0.5,1,0.5,-0.5,1,0.5\n\t\t}\n"
      << "\t\tPolygonVertexIndex: *24 {\n\t\t\ta: 0,1,2,-4,4,7,6,-6,3,2,6,-8,1,0,4,-6,2,1,5,-7,0,3,7,-5\n\t\t}\n"
      << "\t\tGeometryVersion: 124\n"
      << "\t\tLayerElementNormal: 0 {\n\t\t\tVersion: 101\n\t\t\tName: \"\"\n"
      << "\t\t\tMappingInformationType: \"ByPolygonVertex\"\n\t\t\tReferenceInformationType: \"Direct\"\n"
      << "\t\t\tNormals: *72 {\n\t\t\t\ta: ";
    const char* normals[6] = {"0,-1,0", "0,1,0", "0,0,1", "0,0,-1", "1,0,0", "-1,0,0"};
    for (int f = 0; f < 6; ++f) {
        for (int k = 0; k < 4; ++k) s << (f + k > 0 ? "," : "") << normals[f];
    }
    s << "\n\t\t\t}\n\t\t}\n"
      << "\t\tLayerElementUV: 0 {\n\t\t\tVersion: 101\n\t\t\tName: \"map1\"\n"
      << "\t\t\tMappingInformationType: \"ByPolygonVertex\"\n\t\t\tReferenceInformationType: \"IndexToDirect\"\n"
      << "\t\t\tUV: *8 {\n\t\t\t\ta: 0,0,1,0,1,1,0,1\n\t\t\t}\n\t\t\tUVIndex: *24 {\n\t\t\t\ta: ";
    for (int f = 0; f < 6; ++f) s << (f > 0 ? "," : "") << "0,1,2,3";
    s << "\n\t\t\t}\n\t\t}\n"
      << "\t\tLayerElementMaterial: 0 {\n\t\t\tVersion: 101\n\t\t\tName: \"\"\n"
      << "\t\t\tMappingInformationType: \"AllSame\"\n\t\t\tReferenceInformationType: \"IndexToDirect\"\n"
      << "\t\t\tMaterials: *1 {\n\t\t\t\ta: 0\n\t\t\t}\n\t\t}\n"
      << "\t\tLayer: 0 {\n\t\t\tVersion: 100\n"
      << "\t\t\tLayerElement:  {\n\t\t\t\tType: \"LayerElementNormal\"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n"
      << "\t\t\tLayerElement:  {\n\t\t\t\tType: \"LayerElementMaterial\"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n"
      << "\t\t\tLayerElement:  {\n\t\t\t\tType: \"LayerElementUV\"\n\t\t\t\tTypedIndex: 0\n\t\t\t}\n"
      << "\t\t}\n\t}\n";
    return s.str();
}

std::string model(long long id, const char* name, const char* type, const char* translation) {
    std::ostringstream s;
    s << "\tModel: " << id << ", \"Model::" << name << "\", \"" << type << "\" {\n\t\tVersion: 232\n"
      << "\t\tProperties70:  {\n\t\t\tP: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\"," << translation
      << "\n\t\t}\n\t\tShading: Y\n\t\tCulling: \"CullingOff\"\n\t}\n";
    return s.str();
}

const char* kHeader =
    "; FBX 7.4.0 project file\n"
    "FBXHeaderExtension:  {\n\tFBXHeaderVersion: 1003\n\tFBXVersion: 7400\n\tCreator: \"Cramion FbxTests\"\n}\n"
    "GlobalSettings:  {\n\tVersion: 1000\n\tProperties70:  {\n"
    "\t\tP: \"UpAxis\", \"int\", \"Integer\", \"\",1\n\t\tP: \"UpAxisSign\", \"int\", \"Integer\", \"\",1\n"
    "\t\tP: \"FrontAxis\", \"int\", \"Integer\", \"\",2\n\t\tP: \"FrontAxisSign\", \"int\", \"Integer\", \"\",1\n"
    "\t\tP: \"CoordAxis\", \"int\", \"Integer\", \"\",0\n\t\tP: \"CoordAxisSign\", \"int\", \"Integer\", \"\",1\n"
    "\t\tP: \"UnitScaleFactor\", \"double\", \"Number\", \"\",1\n"
    "\t\tP: \"TimeMode\", \"enum\", \"\", \"\",6\n\t}\n}\n";

constexpr long long kSecond = 46186158000ll;  // ticks de FBX por segundo

std::string animatedCubeFbx() {
    std::ostringstream s;
    s << kHeader << "Objects:  {\n" << cubeGeometry(1000, "Cube")
      << model(2000, "Cube", "Mesh", "0,0,0") << model(2001, "Root", "LimbNode", "0,0,0")
      << model(2002, "Tip", "LimbNode", "0,1,0")
      << "\tNodeAttribute: 3001, \"NodeAttribute::Root\", \"LimbNode\" {\n\t\tTypeFlags: \"Skeleton\"\n\t}\n"
      << "\tNodeAttribute: 3002, \"NodeAttribute::Tip\", \"LimbNode\" {\n\t\tTypeFlags: \"Skeleton\"\n\t}\n"
      << "\tMaterial: 4000, \"Material::Red\", \"\" {\n\t\tVersion: 102\n\t\tShadingModel: \"phong\"\n\t\tMultiLayer: 0\n"
      << "\t\tProperties70:  {\n\t\t\tP: \"DiffuseColor\", \"Color\", \"\", \"A\",1,0,0\n"
      << "\t\t\tP: \"ShininessExponent\", \"Number\", \"\", \"A\",25\n\t\t}\n\t}\n"
      << "\tDeformer: 5000, \"Deformer::Skin\", \"Skin\" {\n\t\tVersion: 101\n\t\tLink_DeformAcuracy: 50\n\t}\n"
      << "\tDeformer: 5001, \"SubDeformer::Root\", \"Cluster\" {\n\t\tVersion: 100\n\t\tUserData: \"\", \"\"\n"
      << "\t\tIndexes: *4 {\n\t\t\ta: 0,1,2,3\n\t\t}\n\t\tWeights: *4 {\n\t\t\ta: 1,1,1,1\n\t\t}\n"
      << "\t\tTransform: *16 {\n\t\t\ta: 1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1\n\t\t}\n"
      << "\t\tTransformLink: *16 {\n\t\t\ta: 1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1\n\t\t}\n\t}\n"
      << "\tDeformer: 5002, \"SubDeformer::Tip\", \"Cluster\" {\n\t\tVersion: 100\n\t\tUserData: \"\", \"\"\n"
      << "\t\tIndexes: *4 {\n\t\t\ta: 4,5,6,7\n\t\t}\n\t\tWeights: *4 {\n\t\t\ta: 1,1,1,1\n\t\t}\n"
      // Transform = malla -> hueso en el enlace (TransformLink^-1 * malla), como
      // lo escriben el FBX SDK y Blender; TransformLink = el hueso en el mundo.
      << "\t\tTransform: *16 {\n\t\t\ta: 1,0,0,0,0,1,0,0,0,0,1,0,0,-1,0,1\n\t\t}\n"
      << "\t\tTransformLink: *16 {\n\t\t\ta: 1,0,0,0,0,1,0,0,0,0,1,0,0,1,0,1\n\t\t}\n\t}\n"
      << "\tDeformer: 6000, \"Deformer::Morph\", \"BlendShape\" {\n\t\tVersion: 100\n\t}\n"
      << "\tDeformer: 6001, \"SubDeformer::Bulge\", \"BlendShapeChannel\" {\n\t\tVersion: 100\n\t\tDeformPercent: 0\n"
      << "\t\tFullWeights: *1 {\n\t\t\ta: 100\n\t\t}\n\t}\n"
      << "\tGeometry: 7000, \"Geometry::Bulge\", \"Shape\" {\n\t\tVersion: 100\n"
      << "\t\tIndexes: *4 {\n\t\t\ta: 4,5,6,7\n\t\t}\n"
      << "\t\tVertices: *12 {\n\t\t\ta: 0,0.5,0,0,0.5,0,0,0.5,0,0,0.5,0\n\t\t}\n"
      << "\t\tNormals: *12 {\n\t\t\ta: 0,0,0,0,0,0,0,0,0,0,0,0\n\t\t}\n\t}\n"
      << "\tAnimationStack: 8000, \"AnimStack::Wave\", \"\" {\n\t\tProperties70:  {\n"
      << "\t\t\tP: \"LocalStart\", \"KTime\", \"Time\", \"\",0\n"
      << "\t\t\tP: \"LocalStop\", \"KTime\", \"Time\", \"\"," << kSecond << "\n\t\t}\n\t}\n"
      << "\tAnimationLayer: 8001, \"AnimLayer::BaseLayer\", \"\" {\n\t}\n"
      << "\tAnimationCurveNode: 8002, \"AnimCurveNode::R\", \"\" {\n\t\tProperties70:  {\n"
      << "\t\t\tP: \"d|X\", \"Number\", \"\", \"A\",0\n\t\t\tP: \"d|Y\", \"Number\", \"\", \"A\",0\n"
      << "\t\t\tP: \"d|Z\", \"Number\", \"\", \"A\",0\n\t\t}\n\t}\n";
    const auto curve = [&](long long id, const char* values) {
        s << "\tAnimationCurve: " << id << ", \"AnimCurve::\", \"\" {\n\t\tDefault: 0\n\t\tKeyVer: 4009\n"
          << "\t\tKeyTime: *2 {\n\t\t\ta: 0," << kSecond << "\n\t\t}\n"
          << "\t\tKeyValueFloat: *2 {\n\t\t\ta: " << values << "\n\t\t}\n"
          << "\t\tKeyAttrFlags: *1 {\n\t\t\ta: 4\n\t\t}\n"
          << "\t\tKeyAttrDataFloat: *4 {\n\t\t\ta: 0,0,0,0\n\t\t}\n"
          << "\t\tKeyAttrRefCount: *1 {\n\t\t\ta: 2\n\t\t}\n\t}\n";
    };
    curve(8003, "0,90");
    s << "\tAnimationCurveNode: 8004, \"AnimCurveNode::DeformPercent\", \"\" {\n\t\tProperties70:  {\n"
      << "\t\t\tP: \"d|DeformPercent\", \"Number\", \"\", \"A\",0\n\t\t}\n\t}\n";
    curve(8005, "0,100");
    s << "}\n"
      << "Connections:  {\n"
      << "\tC: \"OO\",2000,0\n\tC: \"OO\",2001,0\n\tC: \"OO\",2002,2001\n\tC: \"OO\",3001,2001\n\tC: \"OO\",3002,2002\n"
      << "\tC: \"OO\",1000,2000\n\tC: \"OO\",4000,2000\n\tC: \"OO\",5000,1000\n\tC: \"OO\",5001,5000\n"
      << "\tC: \"OO\",5002,5000\n\tC: \"OO\",2001,5001\n\tC: \"OO\",2002,5002\n\tC: \"OO\",6000,1000\n"
      << "\tC: \"OO\",6001,6000\n\tC: \"OO\",7000,6001\n\tC: \"OO\",8001,8000\n\tC: \"OO\",8002,8001\n"
      << "\tC: \"OP\",8002,2002, \"Lcl Rotation\"\n\tC: \"OP\",8003,8002, \"d|Z\"\n\tC: \"OO\",8004,8001\n"
      << "\tC: \"OP\",8004,6001, \"DeformPercent\"\n\tC: \"OP\",8005,8004, \"d|DeformPercent\"\n"
      << "}\n"
      << "Takes:  {\n\tCurrent: \"Wave\"\n\tTake: \"Wave\" {\n\t\tFileName: \"Wave.tak\"\n"
      << "\t\tLocalTime: 0," << kSecond << "\n\t\tReferenceTime: 0," << kSecond << "\n\t}\n}\n";
    return s.str();
}

// Escenario estatico: dos rocas (LOD0 y LOD1, la segunda desplazada) sin
// piel ni tomas.
std::string staticLodFbx() {
    std::ostringstream s;
    s << kHeader << "Objects:  {\n" << cubeGeometry(1000, "RockA") << cubeGeometry(1001, "RockB")
      << model(2000, "Rock_LOD0", "Mesh", "0,0,0") << model(2001, "Rock_LOD1", "Mesh", "3,0,0")
      << "\tMaterial: 4000, \"Material::Stone\", \"\" {\n\t\tVersion: 102\n\t\tShadingModel: \"lambert\"\n"
      << "\t\tMultiLayer: 0\n\t\tProperties70:  {\n\t\t\tP: \"DiffuseColor\", \"Color\", \"\", \"A\",0.5,0.5,0.5\n"
      << "\t\t}\n\t}\n}\n"
      << "Connections:  {\n\tC: \"OO\",2000,0\n\tC: \"OO\",2001,0\n\tC: \"OO\",1000,2000\n\tC: \"OO\",1001,2001\n"
      << "\tC: \"OO\",4000,2000\n\tC: \"OO\",4000,2001\n}\n";
    return s.str();
}

// --- Pose de un clip (como el Animator: lo que falta, de la pose de reposo) ---

void decompose(const Mat4& m, Vec3& t, Quat& r, Vec3& s) {
    t = Vec3{m.m[3][0], m.m[3][1], m.m[3][2]};
    Vec3 c[3];
    for (int i = 0; i < 3; ++i) c[i] = Vec3{m.m[i][0], m.m[i][1], m.m[i][2]};
    s = Vec3{core::length(c[0]), core::length(c[1]), core::length(c[2])};
    for (int i = 0; i < 3; ++i) {
        const float l = i == 0 ? s.x : (i == 1 ? s.y : s.z);
        if (l > 1e-12f) c[i] = c[i] * (1.0f / l);
    }
    // Matriz de giro -> cuaternion.
    const float m00 = c[0].x, m11 = c[1].y, m22 = c[2].z;
    const float trace = m00 + m11 + m22;
    Quat q{};
    if (trace > 0.0f) {
        const float k = 0.5f / std::sqrt(trace + 1.0f);
        q.w = 0.25f / k;
        q.x = (c[1].z - c[2].y) * k;
        q.y = (c[2].x - c[0].z) * k;
        q.z = (c[0].y - c[1].x) * k;
    } else if (m00 > m11 && m00 > m22) {
        const float k = 2.0f * std::sqrt(1.0f + m00 - m11 - m22);
        q.w = (c[1].z - c[2].y) / k;
        q.x = 0.25f * k;
        q.y = (c[1].x + c[0].y) / k;
        q.z = (c[2].x + c[0].z) / k;
    } else if (m11 > m22) {
        const float k = 2.0f * std::sqrt(1.0f + m11 - m00 - m22);
        q.w = (c[2].x - c[0].z) / k;
        q.x = (c[1].x + c[0].y) / k;
        q.y = 0.25f * k;
        q.z = (c[2].y + c[1].z) / k;
    } else {
        const float k = 2.0f * std::sqrt(1.0f + m22 - m00 - m11);
        q.w = (c[0].y - c[1].x) / k;
        q.x = (c[2].x + c[0].z) / k;
        q.y = (c[2].y + c[1].z) / k;
        q.z = 0.25f * k;
    }
    r = core::normalize(q);
}

template <typename Key, typename Value, typename Lerp>
Value sampleKeys(const std::vector<Key>& keys, float time, Value rest, Lerp lerp) {
    if (keys.empty()) return rest;
    if (time <= keys.front().time) return keys.front().value;
    if (time >= keys.back().time) return keys.back().value;
    for (std::size_t i = 1; i < keys.size(); ++i) {
        if (time <= keys[i].time) {
            const float span = keys[i].time - keys[i - 1].time;
            const float f = span > 0.0f ? (time - keys[i - 1].time) / span : 0.0f;
            return lerp(keys[i - 1].value, keys[i].value, f);
        }
    }
    return keys.back().value;
}

// Matrices globales de los nodos con `clip` en `time` (nullptr = reposo).
std::vector<Mat4> globalPose(const asset::ModelData& m, const asset::AnimationClip* clip, float time) {
    std::vector<Mat4> local(m.nodes.size());
    for (std::size_t i = 0; i < m.nodes.size(); ++i) local[i] = m.nodes[i].local;
    if (clip != nullptr) {
        for (const asset::AnimationChannel& c : clip->channels) {
            if (c.node < 0 || static_cast<std::size_t>(c.node) >= local.size()) continue;
            Vec3 t, s;
            Quat r;
            decompose(m.nodes[static_cast<std::size_t>(c.node)].local, t, r, s);
            const auto vlerp = [](const Vec3& a, const Vec3& b, float f) { return a + (b - a) * f; };
            t = sampleKeys(c.positions, time, t, vlerp);
            s = sampleKeys(c.scales, time, s, vlerp);
            r = sampleKeys(c.rotations, time, r, [](const Quat& a, const Quat& b, float f) { return core::slerp(a, b, f); });
            local[static_cast<std::size_t>(c.node)] = core::composeTrs(t, r, s);
        }
    }
    std::vector<Mat4> global(m.nodes.size());
    for (std::size_t i = 0; i < m.nodes.size(); ++i) {
        const std::int32_t p = m.nodes[i].parent;
        global[i] = p >= 0 ? global[static_cast<std::size_t>(p)] * local[i] : local[i];
    }
    return global;
}

Vec3 transformPoint(const Mat4& m, const Vec3& p) {
    return Vec3{m.m[0][0] * p.x + m.m[1][0] * p.y + m.m[2][0] * p.z + m.m[3][0],
                m.m[0][1] * p.x + m.m[1][1] * p.y + m.m[2][1] * p.z + m.m[3][1],
                m.m[0][2] * p.x + m.m[1][2] * p.y + m.m[2][2] * p.z + m.m[3][2]};
}

// Vertices deformados por la piel en una pose (el skinning del shader).
std::vector<Vec3> skinnedPositions(const asset::ModelData& m, const std::vector<Mat4>& global) {
    std::vector<Mat4> palette(m.bones.size());
    for (std::size_t b = 0; b < m.bones.size(); ++b) {
        const std::int32_t n = m.bones[b].node;
        palette[b] = (n >= 0 && static_cast<std::size_t>(n) < global.size() ? global[static_cast<std::size_t>(n)]
                                                                           : Mat4::identity()) *
                     m.bones[b].offset;
    }
    std::vector<Vec3> out;
    out.reserve(m.vertices.size());
    for (const asset::SkinnedVertex& v : m.vertices.get()) {
        Vec3 p{0.0f, 0.0f, 0.0f};
        for (std::size_t i = 0; i < asset::kMaxBoneInfluences; ++i) {
            if (v.weights[i] <= 0.0f || v.joints[i] >= palette.size()) continue;
            p = p + transformPoint(palette[v.joints[i]], v.position) * v.weights[i];
        }
        out.push_back(p);
    }
    return out;
}

struct Box {
    Vec3 low{1e30f, 1e30f, 1e30f};
    Vec3 high{-1e30f, -1e30f, -1e30f};
    void add(const Vec3& p) {
        low = Vec3{std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
        high = Vec3{std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
    }
    float size() const { return high.x >= low.x ? core::length(high - low) : 0.0f; }
};

// --- Paridad entre cargadores -------------------------------------------------

struct Parity {
    std::vector<std::string> differences;
    void add(const std::string& text) { differences.push_back(text); }
};

std::string fmt(float v) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.4g", v);
    return text;
}

void compareModels(const asset::ModelData& a, const asset::ModelData& b, bool animated, bool baked, Parity& parity) {
    const std::size_t ta = a.indices.size() / 3, tb = b.indices.size() / 3;
    if (ta != tb) parity.add("triangulos " + std::to_string(ta) + " / " + std::to_string(tb));
    const std::size_t va = a.vertices.size(), vb = b.vertices.size();
    if (va != vb) {
        const float ratio = static_cast<float>(std::max(va, vb)) / static_cast<float>(std::max<std::size_t>(1, std::min(va, vb)));
        if (ratio > 1.05f) parity.add("vertices " + std::to_string(va) + " / " + std::to_string(vb));
    }
    // Materiales por nombre (los que usan las submallas).
    std::set<std::string> ma, mb;
    for (const asset::SubMesh& s : a.submeshes) ma.insert(a.materials[s.material].name);
    for (const asset::SubMesh& s : b.submeshes) mb.insert(b.materials[s.material].name);
    if (ma != mb) parity.add("materiales usados distintos (" + std::to_string(ma.size()) + " / " + std::to_string(mb.size()) + ")");
    std::size_t texa = 0, texb = 0;
    for (const asset::MaterialData& m : a.materials) texa += m.albedo_texture >= 0;
    for (const asset::MaterialData& m : b.materials) texb += m.albedo_texture >= 0;
    if (texa != texb) parity.add("materiales con textura de color " + std::to_string(texa) + " / " + std::to_string(texb));
    if (a.vertex_colors.empty() != b.vertex_colors.empty()) parity.add("color de vertice solo en uno");

    Box box_a, box_b;
    for (const asset::SkinnedVertex& v : a.vertices.get()) box_a.add(v.position);
    for (const asset::SkinnedVertex& v : b.vertices.get()) box_b.add(v.position);
    const float size = std::max(box_a.size(), 1e-3f);
    const float tolerance = 0.01f * size;
    if (!approx(box_a.low, box_b.low, tolerance) || !approx(box_a.high, box_b.high, tolerance)) {
        parity.add("caja de vertices distinta (tam " + fmt(box_a.size()) + " / " + fmt(box_b.size()) + ")");
    }

    // Escenario horneado: los nodos son los de PreTransformVertices (uno por
    // malla de salida, luces y camaras), no los del archivo: no se comparan.
    if (baked) return;

    // Nodos por nombre, y su posicion global en reposo.
    std::map<std::string, std::size_t> nodes_b;
    for (std::size_t i = 0; i < b.nodes.size(); ++i) nodes_b.emplace(b.nodes[i].name, i);
    std::size_t missing = 0;
    std::string first_missing;
    for (const asset::Node& n : a.nodes) {
        if (nodes_b.count(n.name) == 0 && missing++ == 0) first_missing = n.name;
    }
    if (missing > 0) {
        parity.add(std::to_string(missing) + " nodos de assimp sin pareja en ufbx (\"" + first_missing + "\"...)");
    }
    {
        const std::vector<Mat4> ga = globalPose(a, nullptr, 0.0f);
        const std::vector<Mat4> gb = globalPose(b, nullptr, 0.0f);
        float worst = 0.0f;
        std::string worst_node;
        for (std::size_t i = 0; i < a.nodes.size(); ++i) {
            const auto nb = nodes_b.find(a.nodes[i].name);
            if (nb == nodes_b.end()) continue;
            for (int c = 0; c < 4; ++c) {
                for (int r = 0; r < 3; ++r) {
                    const float d = std::abs(ga[i].m[c][r] - gb[nb->second].m[c][r]) / (c == 3 ? size : 1.0f);
                    if (d > worst) {
                        worst = d;
                        worst_node = a.nodes[i].name;
                    }
                }
            }
        }
        if (worst > 0.01f) parity.add("nodos en reposo distintos (" + fmt(worst) + " en \"" + worst_node + "\")");
    }

    if (!animated) return;

    std::set<std::string> bones_a, bones_b;
    for (const asset::Bone& bone : a.bones) bones_a.insert(bone.name);
    for (const asset::Bone& bone : b.bones) bones_b.insert(bone.name);
    if (bones_a != bones_b) {
        parity.add("huesos " + std::to_string(bones_a.size()) + " / " + std::to_string(bones_b.size()));
    }

    // Piel en reposo: la caja de los vertices deformados.
    {
        Box sa, sb;
        for (const Vec3& p : skinnedPositions(a, globalPose(a, nullptr, 0.0f))) sa.add(p);
        for (const Vec3& p : skinnedPositions(b, globalPose(b, nullptr, 0.0f))) sb.add(p);
        if (!approx(sa.low, sb.low, tolerance) || !approx(sa.high, sb.high, tolerance)) {
            parity.add("piel en reposo distinta (caja " + fmt(sa.size()) + " / " + fmt(sb.size()) + ")");
        }
    }

    if (a.animations.size() != b.animations.size()) {
        parity.add("tomas " + std::to_string(a.animations.size()) + " / " + std::to_string(b.animations.size()));
    }
    for (const asset::AnimationClip& clip_a : a.animations) {
        const auto it = std::find_if(b.animations.begin(), b.animations.end(),
                                     [&](const asset::AnimationClip& c) { return c.name == clip_a.name; });
        if (it == b.animations.end()) {
            parity.add("toma \"" + clip_a.name + "\" falta en ufbx");
            continue;
        }
        const asset::AnimationClip& clip_b = *it;
        if (!approx(clip_a.duration, clip_b.duration, std::max(0.02f, 0.01f * clip_a.duration))) {
            parity.add("toma \"" + clip_a.name + "\": duracion " + fmt(clip_a.duration) + " / " + fmt(clip_b.duration));
        }
        // Pose global de los nodos con pareja en 5 tiempos.
        float worst = 0.0f;
        std::string worst_node;
        float worst_time = 0.0f;
        for (int k = 0; k <= 4; ++k) {
            const float time = clip_a.duration * static_cast<float>(k) / 4.0f;
            const std::vector<Mat4> ga = globalPose(a, &clip_a, time);
            const std::vector<Mat4> gb = globalPose(b, &clip_b, time);
            for (std::size_t i = 0; i < a.nodes.size(); ++i) {
                const auto nb = nodes_b.find(a.nodes[i].name);
                if (nb == nodes_b.end()) continue;
                const Vec3 pa{ga[i].m[3][0], ga[i].m[3][1], ga[i].m[3][2]};
                const Vec3 pb{gb[nb->second].m[3][0], gb[nb->second].m[3][1], gb[nb->second].m[3][2]};
                const float d = core::length(pa - pb);
                if (d > worst) {
                    worst = d;
                    worst_node = a.nodes[i].name;
                    worst_time = time;
                }
            }
        }
        if (worst > 0.02f * size) {
            parity.add("toma \"" + clip_a.name + "\": pose distinta, " + fmt(worst) + " (tam " + fmt(size) + ") en \"" +
                       worst_node + "\" a " + fmt(worst_time) + " s");
        }
    }
}

struct LoadResult {
    bool ok = false;
    std::string error;
    asset::ModelData model;
};

LoadResult load(const std::filesystem::path& file, asset::FbxLoader loader, bool hierarchy) {
    asset::setFbxLoader(loader);
    LoadResult r;
    try {
        r.model = hierarchy ? asset::importModelHierarchy(file) : asset::importModelSource(file);
        r.ok = true;
    } catch (const std::exception& e) {
        r.error = e.what();
    }
    asset::setFbxLoader(asset::FbxLoader::Assimp);
    return r;
}

// Paridad de un archivo: lo que encuentra va a `report`. Devuelve si salio igual.
bool parityOf(const std::filesystem::path& file, std::vector<std::string>& report) {
    bool same = true;
    for (const bool hierarchy : {false, true}) {
        const LoadResult a = load(file, asset::FbxLoader::Assimp, hierarchy);
        const LoadResult b = load(file, asset::FbxLoader::Ufbx, hierarchy);
        const std::string label = file.filename().string() + (hierarchy ? " [piezas]" : " [animado]");
        if (!a.ok && !b.ok) {
            report.push_back(label + ": ninguno lo lee");
            continue;
        }
        if (a.ok != b.ok) {
            report.push_back(label + ": solo lo lee " + (a.ok ? std::string("assimp (ufbx: ") + b.error + ")"
                                                              : std::string("ufbx (assimp: ") + a.error + ")"));
            same = false;
            continue;
        }
        const bool animated = !hierarchy && (a.model.bones.size() > 1 || !a.model.animations.empty());
        Parity parity;
        compareModels(a.model, b.model, animated, !hierarchy && !animated, parity);
        for (const std::string& d : parity.differences) report.push_back(label + ": " + d);
        same = same && parity.differences.empty();
    }
    return same;
}

// --- Pruebas fijas --------------------------------------------------------------

std::filesystem::path writeTemp(const std::string& name, const std::string& text) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "CramionFbxTests";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path file = dir / name;
    std::ofstream(file, std::ios::binary) << text;
    return file;
}

void testAnimatedCube(asset::FbxLoader loader) {
    const bool ufbx = loader == asset::FbxLoader::Ufbx;
    std::printf("\n[Caja animada, %s]\n", ufbx ? "ufbx" : "assimp");
    const std::filesystem::path file = writeTemp("cube_anim.fbx", animatedCubeFbx());
    const LoadResult r = load(file, loader, false);
    check(r.ok, "se lee");
    if (!r.ok) {
        std::printf("    %s\n", r.error.c_str());
        return;
    }
    const asset::ModelData& m = r.model;
    check(m.indices.size() == 36, "12 triangulos");
    check(m.vertices.size() == 24, "24 vertices (una normal por cara)");
    bool root = false, tip = false;
    for (const asset::Bone& b : m.bones) {
        root = root || b.name == "Root";
        tip = tip || b.name == "Tip";
    }
    check(root && tip, "huesos Root y Tip");
    check(m.materials.size() >= 1 && m.materials[m.submeshes[0].material].name == "Red", "material Red");
    const asset::MaterialData& red = m.materials[m.submeshes[0].material];
    check(approx(red.base_color.x, 1.0f, 1e-4f) && approx(red.base_color.y, 0.0f, 1e-4f), "color difuso rojo");
    check(approx(red.roughness, 0.5f, 1e-3f), "rugosidad de ShininessExponent 25 (1 - 5/10)");
    check(m.animations.size() == 1 && m.animations[0].name == "Wave", "toma Wave");
    if (m.animations.empty()) return;
    const asset::AnimationClip& clip = m.animations[0];
    check(approx(clip.duration, 1.0f, 1e-3f), "dura 1 s");
    // A 1 s, Tip girado 90 grados en Z: el vertice (0.5, 1, 0.5) va a (0, 1.5, 0.5).
    const std::vector<Mat4> pose = globalPose(m, &clip, 1.0f);
    const std::vector<Vec3> skinned = skinnedPositions(m, pose);
    bool found = false;
    for (std::size_t v = 0; v < skinned.size(); ++v) {
        if (approx(m.vertices[v].position, Vec3{0.5f, 1.0f, 0.5f}, 1e-4f)) {
            found = approx(skinned[v], Vec3{0.0f, 1.5f, 0.5f}, 2e-3f);
            if (!found) std::printf("    (%.3f, %.3f, %.3f)\n", skinned[v].x, skinned[v].y, skinned[v].z);
            break;
        }
    }
    check(found, "piel: la cara de arriba gira con Tip");
    const std::vector<Vec3> half = skinnedPositions(m, globalPose(m, &clip, 0.0f));
    bool rest = true;
    for (std::size_t v = 0; v < half.size(); ++v) rest = rest && approx(half[v], m.vertices[v].position, 1e-4f);
    check(rest, "a 0 s, la pose de reposo");
    check(approx(asset::fileUnitScale(file), 0.01f, 1e-6f), "unidades: centimetros (0.01)");

    if (ufbx) {
        check(m.blend_shapes.size() == 1 && m.blend_shapes[0].name == "Bulge", "forma Bulge");
        if (!m.blend_shapes.empty()) {
            const asset::BlendShape& bulge = m.blend_shapes[0];
            check(bulge.vertices.size() == 12, "12 vertices (4 logicos x 3 caras)");
            bool up = !bulge.position_deltas.empty();
            for (const Vec3& d : bulge.position_deltas) up = up && approx(d, Vec3{0.0f, 0.5f, 0.0f}, 1e-5f);
            check(up, "delta (0, 0.5, 0)");
            const std::vector<asset::SkinnedVertex> moved = asset::applyBlendShapes(m, {1.0f});
            bool top = true;
            for (const std::uint32_t v : bulge.vertices) top = top && approx(moved[v].position.y, 1.5f, 1e-5f);
            check(top, "aplicada al 100 %: la cara de arriba sube a 1.5");
        }
        check(clip.blend_tracks.size() == 1, "la toma anima el peso de Bulge");
        if (!clip.blend_tracks.empty()) {
            check(approx(asset::sampleBlendShapeTrack(clip.blend_tracks[0], 0.5f), 0.5f, 0.02f), "peso 0.5 a mitad");
            check(approx(asset::sampleBlendShapeTrack(clip.blend_tracks[0], 1.0f), 1.0f, 1e-3f), "peso 1 al final");
        }
        // Solo tomas, asignadas por nombre (retargeting de LocomotionPack).
        asset::setFbxLoader(loader);
        std::string error;
        const std::vector<asset::AnimationClip> clips = asset::loadAnimationClips(file, m.nodes, &error, 2.0f, false);
        asset::setFbxLoader(asset::FbxLoader::Assimp);
        check(clips.size() == 1 && !clips[0].channels.empty(), "loadAnimationClips con ufbx");
    } else {
        std::printf("  (assimp: %zu formas)\n", m.blend_shapes.size());
    }

    // Paridad con el otro cargador en este archivo.
    if (ufbx) {
        std::vector<std::string> report;
        const bool same = parityOf(file, report);
        for (const std::string& line : report) std::printf("    %s\n", line.c_str());
        check(same, "paridad con assimp");
    }
}

void testHierarchy() {
    std::printf("\n[Piezas y LODs por nombre, ufbx]\n");
    const std::filesystem::path file = writeTemp("rock_lods.fbx", staticLodFbx());
    const LoadResult r = load(file, asset::FbxLoader::Ufbx, true);
    check(r.ok, "se lee");
    if (!r.ok) return;
    const asset::ModelData& m = r.model;
    bool lod0 = false, lod1 = false;
    std::int32_t lod1_index = -1;
    for (std::size_t i = 0; i < m.nodes.size(); ++i) {
        lod0 = lod0 || m.nodes[i].name == "Rock_LOD0";
        if (m.nodes[i].name == "Rock_LOD1") {
            lod1 = true;
            lod1_index = static_cast<std::int32_t>(i);
        }
    }
    check(lod0 && lod1, "nodos Rock_LOD0 y Rock_LOD1");
    check(m.submeshes.size() == 2, "una submalla por nodo");
    bool local = false;
    for (const asset::SubMesh& s : m.submeshes) {
        if (s.node == lod1_index) local = approx(s.bounds_min.x, -0.5f, 1e-4f);  // en el espacio de SU nodo
    }
    check(local, "cada malla en el espacio de su nodo");
    check(approx(m.nodes[static_cast<std::size_t>(std::max(lod1_index, 0))].local.m[3][0], 3.0f, 1e-4f),
          "el nodo lleva su traslacion");
    check(m.animations.empty() && m.bones.size() == 1, "sin tomas, un hueso");

    // Importacion de assets entera (partida en piezas, LODs a mano).
    asset::setFbxLoader(asset::FbxLoader::Ufbx);
    const std::filesystem::path out = std::filesystem::temp_directory_path() / "CramionFbxTests" / "Assets";
    std::error_code ec;
    std::filesystem::remove_all(out, ec);
    const assets::ImportResult imported = assets::importAny(file, out);
    asset::setFbxLoader(asset::FbxLoader::Assimp);
    check(imported.ok, "importAny con ufbx");
    std::printf("    %s\n", imported.message.c_str());

    std::vector<std::string> report;
    const bool same = parityOf(file, report);
    for (const std::string& line : report) std::printf("    %s\n", line.c_str());
    check(same, "paridad con assimp");

    // Escenario horneado (loadModel / force_static).
    asset::setFbxLoader(asset::FbxLoader::Ufbx);
    asset::ModelData flat = asset::importModelSource(file, /*force_static=*/true);
    asset::setFbxLoader(asset::FbxLoader::Assimp);
    Box box;
    for (const asset::SkinnedVertex& v : flat.vertices.get()) box.add(v.position);
    check(approx(box.high.x, 3.5f, 1e-4f) && approx(box.low.x, -0.5f, 1e-4f), "horneado: la segunda roca en x = 3");
}

void testCVar() {
    std::printf("\n[CVar asset.fbx.Loader]\n");
    cvar::CVarBase* loader = cvar::Registry::instance().find("asset.fbx.Loader");
    check(loader != nullptr, "existe");
    if (loader == nullptr) return;
    check(loader->defaultString() == "anterior", "por defecto el anterior (assimp)");
    check(asset::fbxLoader() == asset::FbxLoader::Assimp, "arranca con assimp");
    (void)loader->fromString("ufbx");
    check(asset::fbxLoader() == asset::FbxLoader::Ufbx, "\"ufbx\" cambia el cargador");
    (void)loader->fromString("anterior");
    check(asset::fbxLoader() == asset::FbxLoader::Assimp, "\"anterior\" lo devuelve");
    cvar::CVarBase* shapes = cvar::Registry::instance().find("asset.import.BlendShapes");
    check(shapes != nullptr && shapes->toString() == "true", "asset.import.BlendShapes activa");
}

void testBrokenTake() {
    std::printf("\n[Toma rota, ufbx]\n");
    // La toma tiene una curva sin claves y otra con un NaN: se lee igual.
    std::string text = animatedCubeFbx();
    const std::string from = "KeyValueFloat: *2 {\n\t\t\ta: 0,90";
    const std::size_t at = text.find(from);
    if (at != std::string::npos) text.replace(at, from.size(), "KeyValueFloat: *2 {\n\t\t\ta: 0,nan");
    const std::filesystem::path file = writeTemp("cube_broken.fbx", text);
    const LoadResult r = load(file, asset::FbxLoader::Ufbx, false);
    check(r.ok, "se lee el modelo");
    check(r.ok && r.model.indices.size() == 36, "con su malla");
}

}  // namespace

int main(int argc, char** argv) {
    // Paridad de carpetas (argumentos y CRAMION_FBX_PARITY_DIRS).
    // --solo-paridad: sin las pruebas fijas.
    bool strict = false;
    bool only_parity = false;
    std::vector<std::filesystem::path> dirs;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--strict") strict = true;
        else if (arg == "--solo-paridad") only_parity = true;
        else if (arg == "--volcar" && i + 1 < argc) {
            // Lo que sale de cada cargador (para investigar una diferencia).
            const std::filesystem::path file = fromUtf8(argv[++i]);
            for (const bool hierarchy : {false, true}) {
                for (const asset::FbxLoader loader : {asset::FbxLoader::Assimp, asset::FbxLoader::Ufbx}) {
                    const LoadResult r = load(file, loader, hierarchy);
                    std::printf("--- %s %s: %s\n", loader == asset::FbxLoader::Ufbx ? "ufbx" : "assimp",
                                hierarchy ? "piezas" : "animado", r.ok ? "" : r.error.c_str());
                    if (!r.ok) continue;
                    const asset::ModelData& m = r.model;
                    const std::vector<Mat4> g = globalPose(m, nullptr, 0.0f);
                    for (std::size_t n = 0; n < m.nodes.size(); ++n) {
                        std::printf("  nodo %zu \"%s\" padre %d  global t=(%.3f %.3f %.3f) x=(%.3f %.3f %.3f)\n", n,
                                    m.nodes[n].name.c_str(), m.nodes[n].parent, g[n].m[3][0], g[n].m[3][1], g[n].m[3][2],
                                    g[n].m[0][0], g[n].m[0][1], g[n].m[0][2]);
                    }
                    for (const asset::Bone& b : m.bones) std::printf("  hueso \"%s\" nodo %d\n", b.name.c_str(), b.node);
                    for (const asset::SubMesh& s : m.submeshes) {
                        std::printf("  submalla %u tri, material \"%s\", nodo %d\n", s.index_count / 3,
                                    m.materials[s.material].name.c_str(), s.node);
                    }
                    for (const asset::AnimationClip& c : m.animations) {
                        std::printf("  toma \"%s\" %.3f s, %zu pistas\n", c.name.c_str(), c.duration, c.channels.size());
                        for (std::size_t p = 0; p < c.channels.size() && p < 4; ++p) {
                            const asset::AnimationChannel& ch = c.channels[p];
                            std::printf("    pista nodo %d: %zu pos, %zu rot, %zu esc\n", ch.node, ch.positions.size(),
                                        ch.rotations.size(), ch.scales.size());
                            for (std::size_t k = 0; k < ch.positions.size() && k < 6; ++k) {
                                std::printf("      pos %.4f (%.3f %.3f %.3f)\n", ch.positions[k].time, ch.positions[k].value.x,
                                            ch.positions[k].value.y, ch.positions[k].value.z);
                            }
                            for (std::size_t k = 0; k < ch.rotations.size() && k < 6; ++k) {
                                std::printf("      rot %.4f (%.3f %.3f %.3f %.3f)\n", ch.rotations[k].time,
                                            ch.rotations[k].value.x, ch.rotations[k].value.y, ch.rotations[k].value.z,
                                            ch.rotations[k].value.w);
                            }
                        }
                    }
                    std::printf("  %zu vertices, %zu formas\n", m.vertices.size(), m.blend_shapes.size());
                }
            }
            return 0;
        } else dirs.emplace_back(fromUtf8(arg));
    }

    asset::setImportBlendShapes(true);
    if (!only_parity) {
        testCVar();
        testAnimatedCube(asset::FbxLoader::Assimp);
        testAnimatedCube(asset::FbxLoader::Ufbx);
        testHierarchy();
        testBrokenTake();
    }
    std::string env;
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, "CRAMION_FBX_PARITY_DIRS") == 0 && value != nullptr) env = value;
    std::free(value);
#else
    if (const char* value = std::getenv("CRAMION_FBX_PARITY_DIRS")) env = value;
#endif
    if (!env.empty()) {
        std::stringstream list(env);
        std::string item;
        while (std::getline(list, item, ';')) {
            if (!item.empty()) dirs.emplace_back(fromUtf8(item));
        }
    }
    if (!dirs.empty()) {
        std::printf("\n[Paridad assimp / ufbx]\n");
        std::size_t files = 0, same = 0;
        std::vector<std::string> report;
        for (const std::filesystem::path& dir : dirs) {
            std::error_code ec;
            if (std::filesystem::is_regular_file(dir, ec)) {
                // Un archivo suelto (el guion de la tanda los pasa de uno en
                // uno: un FBX que tumbe a assimp no se lleva a los demas).
                ++files;
                std::vector<std::string> lines;
                std::printf("  %s\n", dir.filename().string().c_str());
                std::fflush(stdout);
                if (parityOf(dir, lines)) ++same;
                for (const std::string& line : lines) {
                    std::printf("    %s\n", line.c_str());
                    report.push_back(line);
                }
                continue;
            }
            for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
                 !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                if (!it->is_regular_file()) continue;
                std::string ext = it->path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (ext != ".fbx") continue;
                ++files;
                std::vector<std::string> lines;
                std::printf("  %s\n", it->path().filename().string().c_str());
                std::fflush(stdout);
                if (parityOf(it->path(), lines)) ++same;
                for (const std::string& line : lines) {
                    std::printf("    %s\n", line.c_str());
                    report.push_back(line);
                }
            }
        }
        std::printf("\n  %zu FBX, %zu iguales, %zu con diferencias\n", files, same, files - same);
        if (strict) check(same == files, "paridad en todas las carpetas");
    }

    std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

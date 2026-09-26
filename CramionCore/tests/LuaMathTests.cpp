// Pruebas de la libreria matematica de Lua (consola, sin GPU): Vec3, Quat,
// Mathf, Random y ruido, con valores conocidos. Devuelve 0 si todo va.

#include "CramionCore/ecs/World.h"
#include "CramionCore/scripting/Scripting.h"

#include <cstdio>
#include <string>

using namespace cramion;

namespace {

int failures = 0;
int checks = 0;
scripting::ScriptSystem* scripts = nullptr;

// Ejecuta `code` (que devuelve true si todo cuadra) y lo cuenta.
void check(const char* what, const char* code) {
    ++checks;
    std::string out;
    const bool ok = scripts->run(std::string("local function near(a, b, e) return math.abs(a - b) <= (e or 1e-3) end\n"
                                             "local function vnear(a, b, e) return (a - b):length() <= (e or 1e-3) end\n") +
                                     code,
                                 &out);
    const bool passed = ok && out == "true";
    std::printf("  %s %s%s%s\n", passed ? "OK   " : "FALLO", what, passed ? "" : "  -> ", passed ? "" : out.c_str());
    if (!passed) ++failures;
}

}  // namespace

int main(int argc, char** argv) {
    scripting::registerScriptComponents();
    ecs::World world;
    scripting::ScriptSystem system;
    scripts = &system;
    system.start(world);

    // Con archivos: solo comprueba que cada uno compila (los ejemplos del manual).
    if (argc > 1) {
        int bad = 0;
        for (int i = 1; i < argc; ++i) {
            std::string out;
            FILE* f = std::fopen(argv[i], "rb");
            if (f == nullptr) continue;
            std::string text;
            char buffer[4096];
            for (std::size_t n; (n = std::fread(buffer, 1, sizeof(buffer), f)) > 0;) text.append(buffer, n);
            std::fclose(f);
            const bool ok = system.run("local fn, err = load([==[" + text + "]==]); return err or 'ok'", &out);
            if (!ok || out != "ok") {
                ++bad;
                std::printf("  FALLO %s: %s\n", argv[i], out.c_str());
            }
        }
        std::printf("%d archivos, %d no compilan\n", argc - 1, bad);
        return bad == 0 ? 0 : 1;
    }

    std::printf("Vec3\n");
    check("constructores (vacio, x y z, x y, copia) y copy() independiente",
          "local a = Vec3(1, 2, 3); local b = a:copy(); b.x = 9\n"
          "return Vec3() == Vec3.zero and Vec3(1, 2) == Vec3(1, 2, 0) and Vec3(a) == a and a.x == 1");
    check("las constantes (Vec3.zero...) no cambian al modificar lo que devuelven",
          "local a = Vec3.zero; a.x = 5; local u = Vec3.up; u.y = 7\n"
          "local q = Quat.identity; q.w = 0\n"
          "return Vec3.zero.x == 0 and Vec3.up.y == 1 and Quat.identity.w == 1");
    check("operaciones, division por vector y concatenar con texto",
          "local v = Vec3(2, 4, 6)\n"
          "return (v / 2) == Vec3(1, 2, 3) and (v / Vec3(2, 4, 3)) == Vec3(1, 1, 2) and -v == Vec3(-2, -4, -6)\n"
          "  and ('p=' .. Vec3(1, 2, 3)) == 'p=(1.000, 2.000, 3.000)'");
    check("length, sqrLength, normalized, clampLength",
          "local v = Vec3(3, 4, 0)\n"
          "return v:length() == 5 and v:sqrLength() == 25 and vnear(v:normalized(), Vec3(0.6, 0.8, 0))\n"
          "  and near(v:clampLength(2):length(), 2) and Vec3.zero:normalized() == Vec3.zero");
    check("angle y signedAngle",
          "return near(Vec3.angle(Vec3.right, Vec3.forward), 90)\n"
          "  and near(Vec3.signedAngle(Vec3.right, Vec3.back, Vec3.up), -90)\n"
          "  and near(Vec3.signedAngle(Vec3.right, Vec3.forward, Vec3.up), 90)");
    check("lerp (limitado), lerpUnclamped, moveTowards",
          "return Vec3.lerp(Vec3.zero, Vec3(10, 0, 0), 2) == Vec3(10, 0, 0)\n"
          "  and Vec3.lerpUnclamped(Vec3.zero, Vec3(10, 0, 0), 2) == Vec3(20, 0, 0)\n"
          "  and Vec3.moveTowards(Vec3.zero, Vec3(10, 0, 0), 3) == Vec3(3, 0, 0)\n"
          "  and Vec3.moveTowards(Vec3.zero, Vec3(1, 0, 0), 3) == Vec3(1, 0, 0)");
    check("slerp gira la direccion y mezcla la longitud",
          "local v = Vec3.slerp(Vec3(2, 0, 0), Vec3(0, 0, -4), 0.5)\n"
          "return near(v:length(), 3) and near(Vec3.angle(v, Vec3.right), 45)");
    check("project, projectOnPlane, reflect",
          "return Vec3.project(Vec3(3, 4, 0), Vec3.right) == Vec3(3, 0, 0)\n"
          "  and Vec3.projectOnPlane(Vec3(3, 4, 0), Vec3.up) == Vec3(3, 0, 0)\n"
          "  and vnear(Vec3.reflect(Vec3(1, -1, 0), Vec3.up), Vec3(1, 1, 0))");
    check("min, max, abs, floor, round, scale, unpack",
          "local x, y, z = Vec3(1, 2, 3):unpack()\n"
          "return Vec3.min(Vec3(1, 5, 3), Vec3(4, 2, 6)) == Vec3(1, 2, 3) and Vec3.max(Vec3(1, 5, 3), Vec3(4, 2, 6)) == Vec3(4, 5, 6)\n"
          "  and Vec3(-1, 2, -3):abs() == Vec3(1, 2, 3) and Vec3(1.7, -1.2, 0.5):floor() == Vec3(1, -2, 0)\n"
          "  and Vec3(1.6, 2.4, -0.6):round() == Vec3(2, 2, -1) and Vec3.scale(Vec3(1, 2, 3), Vec3(2, 2, 2)) == Vec3(2, 4, 6)\n"
          "  and x == 1 and y == 2 and z == 3");
    check("smoothDamp llega al objetivo sin pasarse",
          "local p, v = Vec3.zero, Vec3.zero\n"
          "local over = false\n"
          "for i = 1, 300 do p, v = Vec3.smoothDamp(p, Vec3(10, 0, 0), v, 0.3, 1 / 60); if p.x > 10.0001 then over = true end end\n"
          "return not over and near(p.x, 10, 1e-2)");

    std::printf("Quat\n");
    check("identidad, euler <-> toEuler (orden YXZ)",
          "local q = Quat.euler(30, 45, 10)\n"
          "return vnear(q:toEuler(), Vec3(30, 45, 10)) and Quat.identity * Vec3(1, 2, 3) == Vec3(1, 2, 3)\n"
          "  and vnear(Quat.euler(Vec3(0, 90, 0)) * Vec3.forward, Vec3.left)");
    check("angleAxis y multiplicar (q1 * q2)",
          "local a = Quat.angleAxis(90, Vec3.up)\n"
          "return vnear(a * Vec3.right, Vec3.forward) and vnear((a * a) * Vec3.right, Vec3.left)");
    check("lookRotation: su forward apunta al objetivo",
          "local d = Vec3(1, 2, -3):normalized()\n"
          "local q = Quat.lookRotation(d)\n"
          "return vnear(q:forward(), d) and near(q:up():dot(d), 0) and vnear(Quat.lookRotation(Vec3.up):forward(), Vec3.up)");
    check("fromToRotation (tambien opuestos)",
          "return vnear(Quat.fromToRotation(Vec3.right, Vec3.up) * Vec3.right, Vec3.up)\n"
          "  and vnear(Quat.fromToRotation(Vec3.right, Vec3.left) * Vec3.right, Vec3.left)");
    check("inverse, angle, slerp, rotateTowards",
          "local q = Quat.euler(0, 70, 0)\n"
          "return vnear((q:inverse() * q) * Vec3.forward, Vec3.forward) and near(Quat.angle(Quat.identity, q), 70)\n"
          "  and near(Quat.angle(Quat.identity, Quat.slerp(Quat.identity, q, 0.5)), 35)\n"
          "  and near(Quat.angle(Quat.identity, Quat.rotateTowards(Quat.identity, q, 20)), 20)");

    std::printf("Mathf\n");
    check("constantes y basicas (min/max con varios, round con decimales, log con base)",
          "return near(Mathf.tau, 2 * Mathf.pi) and Mathf.infinity > 1e300 and Mathf.min(3, 1, 2) == 1\n"
          "  and Mathf.max(3, 1, 7, 2) == 7 and Mathf.round(3.14159, 2) == 3.14 and near(Mathf.log(8, 2), 3)\n"
          "  and Mathf.sign(-4) == -1 and Mathf.sign(0) == 1");
    check("lerp, inverseLerp, remap, clamp",
          "return Mathf.lerp(0, 10, 1.5) == 10 and Mathf.lerpUnclamped(0, 10, 1.5) == 15\n"
          "  and Mathf.inverseLerp(10, 20, 15) == 0.5 and Mathf.remap(5, 0, 10, 100, 200) == 150\n"
          "  and Mathf.clamp(5, 10, 0) == 5 and Mathf.clamp01(-2) == 0");
    check("wrap (Repeat), pingPong",
          "return near(Mathf.wrap(7.5, 3), 1.5) and near(Mathf.wrap(-1, 3), 2) and Mathf['repeat'](4, 3) == 1\n"
          "  and near(Mathf.pingPong(4, 3), 2) and near(Mathf.pingPong(1, 3), 1)");
    check("angulos: deltaAngle, lerpAngle, moveTowardsAngle",
          "return near(Mathf.deltaAngle(350, 10), 20) and near(Mathf.deltaAngle(10, 350), -20)\n"
          "  and near(Mathf.lerpAngle(350, 10, 0.5), 360) and near(Mathf.moveTowardsAngle(350, 10, 5), 355)");
    check("smoothstep, smootherstep, smoothDamp, approximately",
          "local v, vel = 0, 0\n"
          "for i = 1, 300 do v, vel = Mathf.smoothDamp(v, 5, vel, 0.2, 1 / 60) end\n"
          "return Mathf.smoothstep(0, 1, 0.5) == 0.5 and Mathf.smootherstep(0, 1, 0) == 0 and near(v, 5, 1e-2)\n"
          "  and Mathf.approximately(0.1 + 0.2, 0.3)");
    check("potencias de dos",
          "return Mathf.isPowerOfTwo(64) and not Mathf.isPowerOfTwo(48) and Mathf.nextPowerOfTwo(33) == 64");
    check("ruido: perlinNoise en [0,1], continuo y repetible",
          "local ok = true\n"
          "for i = 0, 200 do local n = Mathf.perlinNoise(i * 0.37, i * 0.11); if n < 0 or n > 1 then ok = false end end\n"
          "local a, b = Mathf.perlinNoise(3.2, 7.7), Mathf.perlinNoise(3.201, 7.7)\n"
          "local f = Mathf.fractalNoise(1.3, 2.1, 5)\n"
          "return ok and near(a, b, 0.01) and a == Mathf.perlinNoise(3.2, 7.7) and f >= 0 and f <= 1\n"
          "  and near(Mathf.perlinNoise3(1, 2, 3), 0, 1e-6)");

    std::printf("Random\n");
    check("seed repite la misma secuencia",
          "Random.seed(42); local a = { Random.value(), Random.int(1, 100), Random.range(-5, 5) }\n"
          "Random.seed(42); local b = { Random.value(), Random.int(1, 100), Random.range(-5, 5) }\n"
          "return a[1] == b[1] and a[2] == b[2] and a[3] == b[3]");
    check("rangos: int incluye los extremos, range y value dentro",
          "local lo, hi, ok = false, false, true\n"
          "for i = 1, 2000 do\n"
          "  local n = Random.int(1, 3); if n == 1 then lo = true end; if n == 3 then hi = true end\n"
          "  if n < 1 or n > 3 then ok = false end\n"
          "  local r = Random.range(2, 4); if r < 2 or r > 4 then ok = false end\n"
          "end\n"
          "return ok and lo and hi");
    check("esferas, circulo, rotacion",
          "local ok = true\n"
          "for i = 1, 200 do\n"
          "  if not near(Random.onUnitSphere():length(), 1) then ok = false end\n"
          "  if Random.insideUnitSphere():length() > 1.0001 then ok = false end\n"
          "  local c = Random.insideUnitCircle(); if c.y ~= 0 or c:length() > 1.0001 then ok = false end\n"
          "  if not near((Random.rotation() * Vec3.up):length(), 1) then ok = false end\n"
          "end\n"
          "return ok");
    check("pick, shuffle y chance",
          "local t = { 'a', 'b', 'c', 'd', 'e' }\n"
          "local p = Random.pick(t)\n"
          "local s = Random.shuffle({ 1, 2, 3, 4, 5, 6 }); local sum = 0; for _, v in ipairs(s) do sum = sum + v end\n"
          "return (p == 'a' or p == 'b' or p == 'c' or p == 'd' or p == 'e') and #s == 6 and sum == 21\n"
          "  and Random.pick({}) == nil and Random.chance(1) and not Random.chance(0)");

    std::printf("Entity\n");
    check("quaternion de una entidad (y coincide con rotation)",
          "local e = Scene.create('Giro')\n"
          "e.quaternion = Quat.euler(0, 90, 0)\n"
          "return near(e.rotation.y, 90) and vnear(e.forward, Vec3.left)\n"
          "  and vnear(e.quaternion * Vec3.forward, e.forward)");

    system.stop();
    std::printf("\n%d comprobaciones, %d fallos\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

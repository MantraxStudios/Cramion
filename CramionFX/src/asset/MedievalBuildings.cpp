#include "CramionFX/asset/MedievalBuildings.h"

#include "HouseGeo.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace cramion::asset {

namespace {

using namespace housegeo;

HouseModel finishModel(const Geo& g, const Geo& door, const std::string& name, const Vec3& hinge, const Vec3& door_size) {
    HouseModel out;
    g.finish(out.house, name);
    out.triangles = g.triangles();
    if (door.triangles() > 0) {
        door.finish(out.door, "Puerta");
        out.triangles += door.triangles();
        out.door_hinge = hinge;
        out.door_size = door_size;
    }
    modelBounds(out.house, out.bounds_min, out.bounds_max);
    out.lights = g.lights;
    return out;
}

// Paredes de un rectangulo W x D centrado en (cx, cz).
std::array<Wall, 4> rectWallsAt(float W, float D, float cx, float cz) {
    std::array<Wall, 4> w = rectWalls(W, D);
    for (Wall& wall : w) wall.center = wall.center + Vec3{cx, 0.0f, cz};
    return w;
}

// Marco de puerta de piedra/vigas con la hoja (pieza aparte) y escalones.
void doorway(Geo& g, Geo& door, const Wall& w, const Opening& o, float thick, Vec3& hinge, Vec3& size, bool leaf = true) {
    const float h = thick * 0.5f;
    const float fw = 0.1f;
    wallBox(g, kHouseBeams, w, o.s0, o.s0 + fw, o.y0, o.y1, -h - 0.01f, h + 0.02f);
    wallBox(g, kHouseBeams, w, o.s1 - fw, o.s1, o.y0, o.y1, -h - 0.01f, h + 0.02f);
    wallBox(g, kHouseBeams, w, o.s0, o.s1, o.y1 - fw, o.y1, -h - 0.01f, h + 0.02f);
    wallBox(g, kHouseStone, w, o.s0 - 0.3f, o.s1 + 0.3f, o.y1, o.y1 + 0.35f, h - 0.05f, h + 0.08f);
    wallBox(g, kHouseStone, w, o.s0, o.s1, o.y0 - 0.02f, o.y0 + 0.04f, -h - 0.01f, h + 0.15f, true);
    if (!leaf) return;
    const float dw = (o.s1 - o.s0) - 2.0f * fw - 0.01f;
    const float dh = (o.y1 - o.y0) - fw - 0.04f;
    hinge = at(w, o.s0 + fw + 0.005f, o.y0 + 0.035f, h - 0.0475f);
    size = Vec3{dw, dh, 0.055f};
    doorLeaf(door, dw, dh);
}

// Escalones de piedra delante de una pared (bajan de y_top al suelo, hacia fuera).
void frontStepsAt(Geo& g, const Wall& w, float s, float y_top, float width, float face) {
    const int count = std::max(1, static_cast<int>(std::ceil(y_top / 0.18f)) - 1);
    const float rise = y_top / static_cast<float>(count + 1);
    for (int i = 0; i < count; ++i) {
        const float y1 = y_top - rise * static_cast<float>(i + 1);
        const float d0 = face + 0.32f * static_cast<float>(i);
        wallBox(g, kHouseStone, w, s - width * 0.5f, s + width * 0.5f, -0.3f, y1, d0 - 0.02f, d0 + 0.32f, true);
    }
}

// Torre redonda maciza con almenas y, si `roof`, tejado conico.
void roundTower(Geo& g, const Vec3& base, float radius, float y0, float y1, bool roof, int roof_m, int sides = 16) {
    g.frustum(kHouseStone, base + kUp * y0, base + kUp * y1, radius * 1.06f, radius, sides, false, false);
    // Plataforma y parapeto con merlones.
    g.cylinder(kHouseStone, base + kUp * (y1 - 0.01f), base + kUp * (y1 + 0.02f), radius + 0.02f, sides, false, true);
    g.frustum(kHouseStone, base + kUp * (y1 - 0.35f), base + kUp * (y1 + 0.05f), radius + 0.15f, radius + 0.15f, sides, true, true);
    const int merlons = sides / 2;
    for (int k = 0; k < merlons; ++k) {
        const float a = 2.0f * kPi * (static_cast<float>(k) + 0.25f) / static_cast<float>(merlons);
        const Vec3 dir{std::cos(a), 0.0f, std::sin(a)};
        const Vec3 tangent{-std::sin(a), 0.0f, std::cos(a)};
        const float w = 2.0f * kPi * radius / static_cast<float>(merlons) * 0.55f;
        g.box(kHouseStone, base + dir * (radius - 0.05f) + kUp * (y1 + 0.55f), Vec3{w * 0.5f, 0.5f, 0.28f}, tangent, kUp, dir);
    }
    if (roof) {
        g.frustum(roof_m, base + kUp * (y1 + 1.0f), base + kUp * (y1 + 1.0f + radius * 1.5f), radius + 0.35f, 0.0f, sides, true, false,
                  false, 0.0f, kHousePlanks);
        g.cylinder(kHouseIron, base + kUp * (y1 + 1.0f + radius * 1.5f - 0.1f), base + kUp * (y1 + 1.6f + radius * 1.5f), 0.03f, 6, false,
                   true);
    }
}

// ---------------------------------------------------------------------------
// Iglesia
// ---------------------------------------------------------------------------

HouseModel church(const MedievalSettings& s) {
    Rng rng(s.seed * 17U + 3U);
    const float k = std::clamp(s.scale, 0.7f, 2.0f);
    const float W = 8.5f * k;
    const float L = 15.0f * k;
    const float H = 6.5f * k;
    const float t = 0.8f;
    const float plinth = 0.25f;
    const float TW = 4.8f * k;
    const float TH = 16.0f * k + 2.0f;
    const float tanr = std::tan(50.0f * kPi / 180.0f);
    const float ridge = H + (W * 0.5f + t * 0.5f) * tanr;
    Geo g;
    Geo door;
    Vec3 hinge{};
    Vec3 dsize{};

    // Cimientos y suelo de losas.
    g.boxMinMax(kHouseStone, Vec3{-W * 0.5f - 0.3f, -0.8f, -L * 0.5f - 0.3f}, Vec3{W * 0.5f + 0.3f, plinth, L * 0.5f + 0.3f}, false, 4);
    const float zb = L * 0.5f + t;  // eje de la pared de atras de la torre
    const float zc = zb + TW * 0.5f - t * 0.5f;
    g.boxMinMax(kHouseStone, Vec3{-TW * 0.5f - 0.3f, -0.8f, zb - t * 0.5f}, Vec3{TW * 0.5f + 0.3f, plinth, zc + TW * 0.5f + 0.3f}, false, 4);

    // --- Nave ---
    const std::array<Wall, 4> walls = rectWalls(W, L);
    std::vector<Opening> ops;
    ops.push_back(Opening{0, -0.9f * k, 0.9f * k, plinth, plinth + 3.4f * k, true});
    const int windows = std::max(3, static_cast<int>(std::round(L / 4.2f)));
    for (int side = 2; side <= 3; ++side) {
        for (int i = 0; i < windows; ++i) {
            const float sc = -L * 0.5f + (static_cast<float>(i) + 0.5f) * L / static_cast<float>(windows);
            ops.push_back(Opening{side, sc - 0.45f * k, sc + 0.45f * k, plinth + 2.4f * k, plinth + 5.0f * k, false});
        }
    }
    ops.push_back(Opening{1, -0.75f * k, 0.75f * k, H - 2.4f * k, H - 0.9f * k, false});
    for (int w = 0; w < 4; ++w) {
        const float half = w < 2 ? W * 0.5f + t * 0.5f : L * 0.5f - t * 0.5f;
        wallPanel(g, kHouseStone, walls[static_cast<std::size_t>(w)], -half, half, plinth, H, t, ops, w, true, true, kHousePlaster);
    }
    for (int w = 0; w < 2; ++w) gableWall(g, kHouseStone, walls[static_cast<std::size_t>(w)], W * 0.5f + t * 0.5f, H, ridge, t, kHousePlaster);
    for (const Opening& o : ops) {
        if (!o.door) windowFrame(g, walls[static_cast<std::size_t>(o.wall)], o, t, t * 0.5f, true, false, kHouseBeams);
    }
    // Contrafuertes entre las ventanas.
    for (int side = 2; side <= 3; ++side) {
        const Wall& wall = walls[static_cast<std::size_t>(side)];
        for (int i = 0; i <= windows; ++i) {
            const float sc = std::clamp(-L * 0.5f + static_cast<float>(i) * L / static_cast<float>(windows), -L * 0.5f + 0.4f, L * 0.5f - 0.4f);
            wallBox(g, kHouseStone, wall, sc - 0.35f, sc + 0.35f, -0.4f, H * 0.62f, t * 0.5f - 0.02f, t * 0.5f + 0.85f);
            wallBox(g, kHouseStone, wall, sc - 0.3f, sc + 0.3f, H * 0.62f, H * 0.92f, t * 0.5f - 0.02f, t * 0.5f + 0.45f);
        }
    }
    // Tejado de teja (sin vuelo delante: alli esta la torre).
    GableRoof r;
    r.center = Vec3{0.0f, 0.0f, -0.225f};
    r.along = kZ;
    r.half_len = L * 0.5f + t * 0.5f + 0.225f;
    r.half_span = W * 0.5f + t * 0.5f + 0.6f;
    r.ridge_y = ridge;
    r.tan = tanr;
    r.th = 0.15f;
    r.top_m = kHouseTile;
    r.under_m = kHousePlanks;
    r.trim_m = kHouseBeams;
    gableRoof(g, r);

    // --- Campanario ---
    const std::array<Wall, 4> tw = rectWallsAt(TW, TW, 0.0f, zc);
    std::vector<Opening> tops;
    tops.push_back(Opening{0, -0.85f * k, 0.85f * k, plinth, plinth + 3.2f * k, true});
    tops.push_back(Opening{1, -0.9f * k, 0.9f * k, plinth, plinth + 3.4f * k, true});
    for (int side = 2; side <= 3; ++side) {
        for (float y : {6.0f * k, 10.0f * k}) tops.push_back(Opening{side, -0.13f, 0.13f, y, y + 1.1f, false});
    }
    const float bel0 = TH - 3.8f * k;
    const float bel1 = TH - 1.3f * k;
    for (int w = 0; w < 4; ++w) {
        for (float sc : {-TW * 0.2f, TW * 0.2f}) tops.push_back(Opening{w, sc - 0.4f * k, sc + 0.4f * k, bel0, bel1, false});
    }
    for (int w = 0; w < 4; ++w) {
        const float half = w < 2 ? TW * 0.5f + t * 0.5f : TW * 0.5f - t * 0.5f;
        wallPanel(g, kHouseStone, tw[static_cast<std::size_t>(w)], -half, half, plinth, TH, t, tops, w);
    }
    for (const Opening& o : tops) {
        if (!o.door && o.y1 - o.y0 < 1.5f) slitFrame(g, tw[static_cast<std::size_t>(o.wall)], o, t);
        if (!o.door && o.y0 >= bel0 - 0.01f) {
            // Arco de las troneras (dovelas en el dintel) y antepecho.
            wallBox(g, kHouseStone, tw[static_cast<std::size_t>(o.wall)], o.s0 - 0.15f, o.s1 + 0.15f, o.y1, o.y1 + 0.3f, t * 0.5f - 0.03f,
                    t * 0.5f + 0.06f);
            wallBox(g, kHouseStone, tw[static_cast<std::size_t>(o.wall)], o.s0 - 0.1f, o.s1 + 0.1f, o.y0 - 0.12f, o.y0, t * 0.5f - 0.03f,
                    t * 0.5f + 0.08f);
        }
    }
    doorway(g, door, tw[0], tops[0], t, hinge, dsize);
    frontStepsAt(g, tw[0], 0.0f, plinth, 2.4f * k, t * 0.5f);
    // Impostas (molduras) en el fuste y la cornisa.
    for (float y : {H, bel0 - 0.4f}) {
        g.boxMinMax(kHouseStone, Vec3{-TW * 0.5f - t * 0.5f - 0.08f, y - 0.12f, zc - TW * 0.5f - t * 0.5f - 0.08f},
                    Vec3{TW * 0.5f + t * 0.5f + 0.08f, y + 0.06f, zc + TW * 0.5f + t * 0.5f + 0.08f}, false, 0);
    }
    g.boxMinMax(kHouseStone, Vec3{-TW * 0.5f - t * 0.5f - 0.2f, TH - 0.05f, zc - TW * 0.5f - t * 0.5f - 0.2f},
                Vec3{TW * 0.5f + t * 0.5f + 0.2f, TH + 0.3f, zc + TW * 0.5f + t * 0.5f + 0.2f});
    // Chapitel.
    pyramidRoof(g, kHouseRoof, Vec3{0.0f, 0.0f, zc}, TW * 0.5f + t * 0.5f + 0.35f, TW * 0.5f + t * 0.5f + 0.35f, TH + 0.3f,
                TH + 0.3f + 7.0f * k);
    // Cruz de hierro arriba.
    const float apex = TH + 0.3f + 7.0f * k;
    g.boxMinMax(kHouseIron, Vec3{-0.04f, apex - 0.1f, zc - 0.04f}, Vec3{0.04f, apex + 1.3f, zc + 0.04f});
    g.boxMinMax(kHouseIron, Vec3{-0.35f, apex + 0.75f, zc - 0.04f}, Vec3{0.35f, apex + 0.83f, zc + 0.04f});
    // Campana con su yugo y el suelo del campanario.
    g.boxMinMax(kHousePlanks, Vec3{-TW * 0.5f + t * 0.5f, bel0 - 0.6f, zc - TW * 0.5f + t * 0.5f},
                Vec3{TW * 0.5f - t * 0.5f, bel0 - 0.45f, zc + TW * 0.5f - t * 0.5f}, true);
    const Vec3 bell{0.0f, bel1 - 0.5f, zc};
    g.beam(kHouseBeams, Vec3{-TW * 0.5f + t * 0.5f, bel1 - 0.25f, zc}, Vec3{TW * 0.5f - t * 0.5f, bel1 - 0.25f, zc}, 0.22f, 0.22f);
    g.frustum(kHouseIron, bell - kUp * (1.0f * k), bell, 0.62f * k, 0.28f * k, 18, true, true);
    g.frustum(kHouseIron, bell - kUp * (1.12f * k), bell - kUp * (1.0f * k), 0.66f * k, 0.62f * k, 18, false, false);

    // --- Interior ---
    if (s.interior) {
        const float iz0 = -L * 0.5f + t * 0.5f;
        const float iz1 = L * 0.5f - t * 0.5f;
        const float ix = W * 0.5f - t * 0.5f;
        // Presbiterio elevado con el altar y la cruz.
        g.boxMinMax(kHouseStone, Vec3{-ix, plinth, iz0}, Vec3{ix, plinth + 0.3f, iz0 + 3.2f * k}, false, 4);
        const float ay = plinth + 0.3f;
        g.boxMinMax(kHouseStone, Vec3{-0.95f, ay, iz0 + 1.1f * k}, Vec3{0.95f, ay + 0.95f, iz0 + 1.1f * k + 1.0f});
        g.boxMinMax(kHouseCloth, Vec3{-1.02f, ay + 0.95f, iz0 + 1.1f * k - 0.05f}, Vec3{1.02f, ay + 0.98f, iz0 + 1.1f * k + 1.05f});
        g.boxMinMax(kHouseCloth, Vec3{-0.5f, ay + 0.4f, iz0 + 1.1f * k + 1.0f}, Vec3{0.5f, ay + 0.98f, iz0 + 1.1f * k + 1.03f});
        for (float x : {-0.7f, 0.7f}) {
            g.cylinder(kHouseIron, Vec3{x, ay + 0.98f, iz0 + 1.1f * k + 0.5f}, Vec3{x, ay + 1.4f, iz0 + 1.1f * k + 0.5f}, 0.025f, 6, false, true);
            g.cylinder(kHousePlaster, Vec3{x, ay + 1.4f, iz0 + 1.1f * k + 0.5f}, Vec3{x, ay + 1.6f, iz0 + 1.1f * k + 0.5f}, 0.03f, 6, false, true);
        }
        g.boxMinMax(kHouseBeams, Vec3{-0.09f, ay + 1.2f, iz0 + 0.02f}, Vec3{0.09f, ay + 3.6f, iz0 + 0.16f});
        g.boxMinMax(kHouseBeams, Vec3{-0.7f, ay + 2.75f, iz0 + 0.02f}, Vec3{0.7f, ay + 2.93f, iz0 + 0.16f});
        g.lights.push_back(Vec3{0.0f, ay + 2.2f, iz0 + 2.4f * k});
        // Bancos a los dos lados del pasillo, mirando al altar.
        const float aisle = 1.6f;
        const float pew_len = ix - aisle * 0.5f - 0.35f;
        for (float z = iz1 - 1.6f; z > iz0 + 3.2f * k + 1.0f; z -= 1.05f) {
            for (float side : {-1.0f, 1.0f}) {
                const float x = side * (aisle * 0.5f + pew_len * 0.5f + 0.05f);
                pew(g, frameAt(Vec3{x, plinth, z}, 2), pew_len);
            }
        }
        // Pulpito y pila.
        g.boxMinMax(kHouseBeams, Vec3{ix - 1.2f, ay, iz0 + 2.0f * k}, Vec3{ix - 0.3f, ay + 1.15f, iz0 + 2.0f * k + 0.9f});
        g.frustum(kHouseStone, Vec3{-1.6f, plinth, iz1 - 0.8f}, Vec3{-1.6f, plinth + 0.9f, iz1 - 0.8f}, 0.2f, 0.42f, 10, true, true);
    }
    return finishModel(g, door, "Iglesia", hinge, dsize);
}

// ---------------------------------------------------------------------------
// Granero / establo
// ---------------------------------------------------------------------------

HouseModel barn(const MedievalSettings& s) {
    Rng rng(s.seed * 29U + 7U);
    const float k = std::clamp(s.scale, 0.7f, 1.6f);
    const float W = 9.0f * k;
    const float L = 13.0f * k;
    const float H = 4.4f;
    const float t = 0.18f;
    const float plinth = 0.3f;
    const float tanr = std::tan(42.0f * kPi / 180.0f);
    const float ridge = H + (W * 0.5f + t * 0.5f) * tanr;
    Geo g;
    Geo door;

    g.boxMinMax(kHouseStone, Vec3{-W * 0.5f - 0.1f, -0.6f, -L * 0.5f - 0.1f}, Vec3{W * 0.5f + 0.1f, plinth, L * 0.5f + 0.1f}, false, 4);
    g.boxMinMax(kHousePlanks, Vec3{-W * 0.5f + t, plinth - 0.02f, -L * 0.5f + t}, Vec3{W * 0.5f - t, plinth + 0.03f, L * 0.5f - t}, true, 4);
    const std::array<Wall, 4> walls = rectWalls(W, L);
    std::vector<Opening> ops;
    ops.push_back(Opening{0, -1.9f, 1.9f, plinth, plinth + 3.7f, true});
    ops.push_back(Opening{1, -0.55f, 0.55f, plinth, plinth + 2.1f, true});
    for (int side = 2; side <= 3; ++side) {
        for (float sc = -L * 0.5f + 2.0f; sc < L * 0.5f - 1.5f; sc += 3.2f) ops.push_back(Opening{side, sc - 0.4f, sc + 0.4f, 2.4f, 3.0f, false});
    }
    for (int w = 0; w < 4; ++w) {
        const float half = w < 2 ? W * 0.5f + t * 0.5f : L * 0.5f - t * 0.5f;
        wallPanel(g, kHousePlanks, walls[static_cast<std::size_t>(w)], -half, half, plinth, H, t, ops, w);
    }
    for (int w = 0; w < 2; ++w) gableWall(g, kHousePlanks, walls[static_cast<std::size_t>(w)], W * 0.5f + t * 0.5f, H, ridge, t);
    // Entramado de vigas por fuera: soleras, carreras, postes y marcos.
    for (int w = 0; w < 4; ++w) {
        const Wall& wall = walls[static_cast<std::size_t>(w)];
        const float half = w < 2 ? W * 0.5f + t * 0.5f + 0.06f : L * 0.5f + t * 0.5f;
        wallBox(g, kHouseBeams, wall, -half, half, plinth, plinth + 0.2f, t * 0.5f - 0.01f, t * 0.5f + 0.06f);
        wallBox(g, kHouseBeams, wall, -half, half, H - 0.2f, H, t * 0.5f - 0.01f, t * 0.5f + 0.06f);
        for (float sc = -half + 0.11f; sc <= half; sc += std::max((2.0f * half - 0.22f) / std::max(1.0f, std::round((2.0f * half) / 2.6f)), 0.5f)) {
            bool hole = false;
            for (const Opening& o : ops) hole = hole || (o.wall == w && sc > o.s0 - 0.15f && sc < o.s1 + 0.15f && o.door);
            if (!hole) wallBox(g, kHouseBeams, wall, sc - 0.11f, sc + 0.11f, plinth + 0.2f, H - 0.2f, t * 0.5f - 0.01f, t * 0.5f + 0.06f);
        }
    }
    for (const Opening& o : ops) {
        const Wall& wall = walls[static_cast<std::size_t>(o.wall)];
        wallBox(g, kHouseBeams, wall, o.s0 - 0.16f, o.s0, o.y0, o.y1 + 0.16f, -t * 0.5f - 0.02f, t * 0.5f + 0.07f);
        wallBox(g, kHouseBeams, wall, o.s1, o.s1 + 0.16f, o.y0, o.y1 + 0.16f, -t * 0.5f - 0.02f, t * 0.5f + 0.07f);
        wallBox(g, kHouseBeams, wall, o.s0 - 0.16f, o.s1 + 0.16f, o.y1, o.y1 + 0.18f, -t * 0.5f - 0.02f, t * 0.5f + 0.07f);
        if (!o.door) {
            // Contraventana entornada.
            wallBox(g, kHousePlanks, wall, o.s1 + 0.17f, o.s1 + 0.17f + (o.s1 - o.s0), o.y0, o.y1, t * 0.5f + 0.07f, t * 0.5f + 0.11f);
        }
    }
    // Portalon: dos hojas abiertas contra la fachada (con sus pernios).
    for (float side : {-1.0f, 1.0f}) {
        const float a = side * 2.06f;
        const float b = side * (2.06f + 1.9f);
        wallBox(g, kHousePlanks, walls[0], std::min(a, b), std::max(a, b), plinth + 0.05f, plinth + 3.65f, t * 0.5f + 0.08f, t * 0.5f + 0.16f);
        for (float y : {plinth + 0.6f, plinth + 1.85f, plinth + 3.1f}) {
            wallBox(g, kHouseIron, walls[0], std::min(a, b) + 0.05f, std::max(a, b) - 0.05f, y - 0.04f, y + 0.04f, t * 0.5f + 0.16f,
                    t * 0.5f + 0.18f);
        }
        wallBox(g, kHouseBeams, walls[0], std::min(a, b), std::max(a, b), plinth + 1.75f, plinth + 1.95f, t * 0.5f + 0.16f, t * 0.5f + 0.2f);
    }
    // Puerta del pajar en el hastial y la viga del polipasto.
    wallBox(g, kHousePlanks, walls[0], -0.8f, 0.8f, H + 0.5f, H + 2.0f, t * 0.5f, t * 0.5f + 0.06f);
    wallBox(g, kHouseBeams, walls[0], -0.9f, 0.9f, H + 2.0f, H + 2.14f, t * 0.5f, t * 0.5f + 0.1f);
    g.beam(kHouseBeams, Vec3{0.0f, ridge - 0.5f, L * 0.5f - 0.6f}, Vec3{0.0f, ridge - 0.5f, L * 0.5f + 1.2f}, 0.2f, 0.22f);
    g.cylinder(kHouseIron, Vec3{-0.06f, ridge - 0.78f, L * 0.5f + 1.0f}, Vec3{0.06f, ridge - 0.78f, L * 0.5f + 1.0f}, 0.12f, 10, true, true);
    g.boxMinMax(kHouseThatch, Vec3{-0.015f, H + 1.0f, L * 0.5f + 0.985f}, Vec3{0.015f, ridge - 0.8f, L * 0.5f + 1.015f});
    // Tejado: tablillas o paja.
    GableRoof r;
    r.along = kZ;
    r.half_len = L * 0.5f + t * 0.5f + 0.5f;
    r.half_span = W * 0.5f + t * 0.5f + 0.55f;
    r.ridge_y = ridge;
    r.tan = tanr;
    const bool thatch = rng.chance(0.45f);
    r.top_m = thatch ? kHouseThatch : kHouseRoof;
    r.th = thatch ? 0.35f : 0.16f;
    r.trim_m = thatch ? kHouseThatch : kHouseBeams;
    r.barge = !thatch;
    gableRoof(g, r);

    if (s.interior) {
        const float ix = W * 0.5f - t;
        const float iz0 = -L * 0.5f + t;
        const float iz1 = L * 0.5f - t;
        // Pajar sobre la mitad de atras, con postes, vigas y escalera de mano.
        const float yl = plinth + 2.85f;
        const float edge = -0.6f;
        g.boxMinMax(kHousePlanks, Vec3{-ix, yl - 0.12f, iz0}, Vec3{ix, yl, edge}, true);
        for (float x : {-ix + 1.2f, 0.0f, ix - 1.2f}) {
            g.boxMinMax(kHouseBeams, Vec3{x - 0.11f, plinth, edge - 0.22f}, Vec3{x + 0.11f, yl - 0.12f, edge});
        }
        g.boxMinMax(kHouseBeams, Vec3{-ix, yl - 0.36f, edge - 0.22f}, Vec3{ix, yl - 0.12f, edge});
        for (float x = -ix + 0.6f; x < ix; x += 1.2f) g.boxMinMax(kHouseBeams, Vec3{x - 0.07f, yl - 0.3f, iz0}, Vec3{x + 0.07f, yl - 0.12f, edge});
        const float lx = ix - 2.3f;
        for (float side : {-0.25f, 0.25f}) {
            g.beam(kHouseBeams, Vec3{lx + side, plinth, edge + 1.3f}, Vec3{lx + side, yl + 0.9f, edge - 0.05f}, 0.06f, 0.08f);
        }
        for (float f = 0.1f; f < 1.0f; f += 0.09f) {
            const Vec3 p{lx, plinth + (yl + 0.9f - plinth) * f, edge + 1.3f - 1.35f * f};
            g.beam(kHouseBeams, p - kX * 0.27f, p + kX * 0.27f, 0.04f, 0.04f);
        }
        // Heno en el pajar y abajo.
        for (int i = 0; i < 6; ++i) {
            const float w = rng.range(1.2f, 2.4f);
            const float d = rng.range(1.0f, 2.0f);
            const float x = rng.range(-ix + w * 0.5f, ix - w * 0.5f - 2.6f);
            const float z = rng.range(iz0 + d * 0.5f, edge - d * 0.5f - 0.3f);
            hay(g, frameAt(Vec3{x, yl, z}, rng.pick(4)), w, rng.range(0.6f, 1.3f), d, 0.0f);
        }
        hay(g, frameAt(Vec3{ix - 1.0f, plinth, iz0 + 1.2f}, 0), 1.6f, 1.0f, 2.0f);
        // Cuadras a la izquierda: tabiques bajos y pesebres.
        for (float z = iz0 + 0.2f; z < iz1 - 2.0f; z += 2.6f) {
            g.boxMinMax(kHousePlanks, Vec3{-ix, plinth, z - 0.04f}, Vec3{-ix + 2.4f, plinth + 1.35f, z + 0.04f}, true);
            g.boxMinMax(kHouseBeams, Vec3{-ix + 2.3f, plinth, z - 0.07f}, Vec3{-ix + 2.44f, plinth + 1.6f, z + 0.07f});
            g.boxMinMax(kHousePlanks, Vec3{-ix, plinth + 0.5f, z + 0.5f}, Vec3{-ix + 0.5f, plinth + 0.9f, z + 2.1f});
            hay(g, frameAt(Vec3{-ix + 0.25f, plinth + 0.9f, z + 1.3f}, 1), 1.4f, 0.12f, 0.4f, 0.0f);
        }
        // Carro y barriles.
        cart(g, frameAt(Vec3{ix - 1.4f, plinth, iz1 - 3.2f}, 0));
        barrel(g, Vec3{ix - 0.5f, plinth, 1.0f}, 0.3f, 0.85f, 0.2f);
        barrel(g, Vec3{ix - 0.55f, plinth, 0.3f}, 0.3f, 0.85f, 0.8f);
    }
    return finishModel(g, door, "Granero", Vec3{}, Vec3{});
}

// ---------------------------------------------------------------------------
// Pozo
// ---------------------------------------------------------------------------

HouseModel well(const MedievalSettings& s) {
    Rng rng(s.seed * 41U + 1U);
    Geo g;
    Geo door;
    const float ro = 0.95f;
    const float ri = 0.68f;
    const float h = 0.85f;
    g.frustum(kHouseStone, Vec3{0.0f, -0.5f, 0.0f}, Vec3{0.0f, h, 0.0f}, ro + 0.05f, ro, 16, false, false);
    g.frustum(kHouseStone, Vec3{0.0f, -1.2f, 0.0f}, Vec3{0.0f, h, 0.0f}, ri, ri, 16, false, false, true);
    // Brocal: anillo de arriba (16 trapecios).
    for (int k = 0; k < 16; ++k) {
        const float a0 = 2.0f * kPi * static_cast<float>(k) / 16.0f;
        const float a1 = 2.0f * kPi * static_cast<float>(k + 1) / 16.0f;
        const Vec3 d0{std::cos(a0), 0.0f, std::sin(a0)};
        const Vec3 d1{std::cos(a1), 0.0f, std::sin(a1)};
        g.flat(kHouseStone, {d0 * ri + kUp * h, d1 * ri + kUp * h, d1 * ro + kUp * h, d0 * ro + kUp * h}, kUp, kX, kZ);
    }
    // Agua oscura al fondo.
    g.cylinder(kHouseGlass, Vec3{0.0f, -0.75f, 0.0f}, Vec3{0.0f, -0.7f, 0.0f}, ri, 16, false, true);
    // Postes, travesano con el torno, cuerda y cubo.
    for (float x : {-1.05f, 1.05f}) g.boxMinMax(kHouseBeams, Vec3{x - 0.08f, -0.3f, -0.08f}, Vec3{x + 0.08f, 2.35f, 0.08f});
    g.cylinder(kHouseBeams, Vec3{-1.2f, 1.75f, 0.0f}, Vec3{1.2f, 1.75f, 0.0f}, 0.07f, 10, true, true);
    g.boxMinMax(kHouseIron, Vec3{1.2f, 1.71f, -0.03f}, Vec3{1.25f, 1.79f, 0.25f});
    g.boxMinMax(kHouseIron, Vec3{1.25f, 1.71f, 0.22f}, Vec3{1.4f, 1.79f, 0.28f});
    g.boxMinMax(kHouseThatch, Vec3{-0.015f, 1.0f, -0.015f}, Vec3{0.015f, 1.72f, 0.015f});
    g.frustum(kHousePlanks, Vec3{0.0f, 0.7f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, 0.14f, 0.17f, 10, true, false, false, 0.0f, kHouseLogEnds);
    g.frustum(kHouseIron, Vec3{0.0f, 0.97f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f}, 0.175f, 0.175f, 10, false, false);
    // Tejadillo a dos aguas.
    GableRoof r;
    r.along = kX;
    r.half_len = 1.35f;
    r.half_span = 0.95f;
    r.ridge_y = 2.95f;
    r.tan = 0.75f;
    r.th = 0.08f;
    r.top_m = rng.chance(0.5f) ? kHouseRoof : kHouseTile;
    r.trim_m = kHouseBeams;
    gableRoof(g, r);
    for (float x : {-1.05f, 1.05f}) {
        g.beam(kHouseBeams, Vec3{x, 2.35f, 0.0f}, Vec3{x, 2.95f, 0.0f}, 0.08f, 0.08f, kX);
        for (float z : {-0.8f, 0.8f}) g.beam(kHouseBeams, Vec3{x, 2.95f, 0.0f}, Vec3{x, 2.95f - 0.75f * 0.8f, z}, 0.07f, 0.07f, kX);
    }
    // Banco de piedra alrededor (una losa).
    g.frustum(kHouseStone, Vec3{0.0f, -0.2f, 0.0f}, Vec3{0.0f, 0.12f, 0.0f}, ro + 0.55f, ro + 0.5f, 16, false, true);
    return finishModel(g, door, "Pozo", Vec3{}, Vec3{});
}

// ---------------------------------------------------------------------------
// Puesto de mercado
// ---------------------------------------------------------------------------

HouseModel marketStall(const MedievalSettings& s) {
    Rng rng(s.seed * 53U + 11U);
    Geo g;
    Geo door;
    const float w = rng.range(2.4f, 3.0f);
    const float d = 1.0f;
    const float hz = 0.9f;
    // Mostrador.
    g.boxMinMax(kHousePlanks, Vec3{-w * 0.5f, hz - 0.05f, -d * 0.5f}, Vec3{w * 0.5f, hz, d * 0.5f}, true);
    g.boxMinMax(kHousePlanks, Vec3{-w * 0.5f, 0.05f, d * 0.5f - 0.04f}, Vec3{w * 0.5f, hz - 0.05f, d * 0.5f}, false);
    for (float sx : {-1.0f, 1.0f}) g.boxMinMax(kHousePlanks, Vec3{sx * w * 0.5f - 0.03f, 0.0f, -d * 0.5f}, Vec3{sx * w * 0.5f + 0.03f, hz, d * 0.5f});
    // Postes (los de atras mas altos) y el toldo a rayas.
    const float front_h = 2.3f;
    const float back_h = 2.75f;
    for (float sx : {-1.0f, 1.0f}) {
        g.boxMinMax(kHouseBeams, Vec3{sx * w * 0.5f - 0.05f, 0.0f, d * 0.5f + 0.05f}, Vec3{sx * w * 0.5f + 0.05f, front_h, d * 0.5f + 0.15f});
        g.boxMinMax(kHouseBeams, Vec3{sx * w * 0.5f - 0.05f, 0.0f, -d * 0.5f - 0.75f}, Vec3{sx * w * 0.5f + 0.05f, back_h, -d * 0.5f - 0.65f});
    }
    const Vec3 a{0.0f, back_h, -d * 0.5f - 0.75f};
    const Vec3 b{0.0f, front_h - 0.1f, d * 0.5f + 0.55f};
    const Vec3 dir = core::normalize(b - a);
    const Vec3 n = core::normalize(core::cross(dir, kX)) * -1.0f;
    const Vec3 nn = n.y < 0.0f ? n * -1.0f : n;
    const float len = core::length(b - a);
    g.box(kHouseCloth, (a + b) * 0.5f + nn * 0.02f, Vec3{w * 0.5f + 0.2f, 0.015f, len * 0.5f}, kX, nn, dir, false);
    // Faldon del toldo por delante.
    g.boxMinMax(kHouseCloth, Vec3{-w * 0.5f - 0.2f, front_h - 0.45f, d * 0.5f + 0.52f}, Vec3{w * 0.5f + 0.2f, front_h - 0.08f, d * 0.5f + 0.56f});
    // Genero: cestas, cajas, tarros, quesos y telas.
    float x = -w * 0.5f + 0.25f;
    while (x < w * 0.5f - 0.25f) {
        const float kind = rng.range(0.0f, 1.0f);
        const float size = rng.range(0.16f, 0.26f);
        const float z = rng.range(-0.2f, 0.15f);
        if (kind < 0.3f) {
            g.frustum(kHouseThatch, Vec3{x, hz, z}, Vec3{x, hz + size * 1.1f, z}, size * 0.8f, size, 10, true, false);
            g.cylinder(rng.chance(0.5f) ? kHouseCloth : kHouseTile, Vec3{x, hz + size * 0.95f, z}, Vec3{x, hz + size * 1.05f, z}, size * 0.9f, 10,
                       false, true);
        } else if (kind < 0.55f) {
            crate(g, frameAt(Vec3{x, hz, z}, rng.pick(4)), size * 1.6f);
        } else if (kind < 0.75f) {
            for (int i = 0; i < 3; ++i) {
                g.frustum(kHouseStone, Vec3{x + i * 0.1f - 0.1f, hz, z}, Vec3{x + i * 0.1f - 0.1f, hz + 0.22f, z}, 0.05f, 0.035f, 8, false, true);
            }
        } else if (kind < 0.88f) {
            g.cylinder(kHousePlaster, Vec3{x, hz, z}, Vec3{x, hz + 0.12f, z}, size, 12, false, true);
        } else {
            g.boxMinMax(kHouseCloth, Vec3{x - size, hz, z - 0.2f}, Vec3{x + size, hz + 0.18f, z + 0.2f});
        }
        x += size * 2.0f + rng.range(0.05f, 0.2f);
    }
    // Detras: barril, cajas apiladas y un taburete.
    barrel(g, Vec3{w * 0.5f - 0.35f, 0.0f, -d * 0.5f - 0.6f}, 0.28f, 0.8f, rng.range(0.0f, 1.0f));
    crate(g, frameAt(Vec3{-w * 0.5f + 0.4f, 0.0f, -d * 0.5f - 0.5f}, 0), 0.55f);
    crate(g, frameAt(Vec3{-w * 0.5f + 0.4f, 0.0f, -d * 0.5f - 0.5f}, 1), 0.42f, 0.55f);
    stool(g, frameAt(Vec3{0.0f, 0.0f, -d * 0.5f - 0.5f}, 0));
    return finishModel(g, door, "Puesto de mercado", Vec3{}, Vec3{});
}

// ---------------------------------------------------------------------------
// Torre del homenaje
// ---------------------------------------------------------------------------

HouseModel keep(const MedievalSettings& s) {
    Rng rng(s.seed * 61U + 5U);
    const float k = std::clamp(s.scale, 0.7f, 1.8f);
    const float S = 10.0f * k;
    const float H = 15.0f * k;
    const float t = 1.6f;
    const float yh = 3.0f;          // suelo del salon (puerta en alto)
    const float hall_h = 4.2f;
    Geo g;
    Geo door;
    Vec3 hinge{};
    Vec3 dsize{};
    // Zarpa (base ataludada) y cimientos.
    g.frustum(kHouseStone, Vec3{0.0f, -1.0f, 0.0f}, Vec3{0.0f, 1.4f, 0.0f}, (S * 0.5f + t * 0.5f + 0.6f) * 1.4142f,
              (S * 0.5f + t * 0.5f) * 1.4142f, 4, false, true, false, 0.0f, -1, kPi * 0.25f);
    const std::array<Wall, 4> walls = rectWalls(S, S);
    std::vector<Opening> ops;
    ops.push_back(Opening{0, -0.75f, 0.75f, yh, yh + 2.5f, true});
    for (int w = 0; w < 4; ++w) {
        for (float sc : {-S * 0.25f, S * 0.25f}) {
            ops.push_back(Opening{w, sc - 0.13f, sc + 0.13f, yh + 1.3f, yh + 2.5f, false});
            ops.push_back(Opening{w, sc - 0.13f, sc + 0.13f, yh + hall_h + 1.4f, yh + hall_h + 2.6f, false});
            if (H - 3.0f > yh + hall_h + 3.2f) ops.push_back(Opening{w, sc - 0.13f, sc + 0.13f, H - 3.0f, H - 1.9f, false});
        }
    }
    for (int w = 0; w < 4; ++w) {
        const float half = w < 2 ? S * 0.5f + t * 0.5f : S * 0.5f - t * 0.5f;
        wallPanel(g, kHouseStone, walls[static_cast<std::size_t>(w)], -half, half, 1.4f, H, t, ops, w, false, true, kHousePlaster);
    }
    for (const Opening& o : ops) {
        if (!o.door) slitFrame(g, walls[static_cast<std::size_t>(o.wall)], o, t);
    }
    doorway(g, door, walls[0], ops[0], t, hinge, dsize);
    // Adarve: suelo de piedra arriba y almenas en los cuatro lados.
    const float inner = S * 0.5f - t * 0.5f;
    g.boxMinMax(kHouseStone, Vec3{-inner, H - 0.35f, -inner}, Vec3{inner, H, inner}, false, 0);
    for (int w = 0; w < 4; ++w) {
        const Wall& wall = walls[static_cast<std::size_t>(w)];
        const float half = S * 0.5f + t * 0.5f + 0.15f;
        wallBox(g, kHouseStone, wall, -half, half, H - 0.4f, H, -t * 0.5f, t * 0.5f + 0.15f);  // cornisa
        battlements(g, wall, -half, half, H, 1.5f, t * 0.5f - 0.45f, 0.6f, 0.75f, 0.55f);
    }
    // Torrecillas en las esquinas (sobre canecillos) con tejado conico.
    for (float sx : {-1.0f, 1.0f}) {
        for (float sz : {-1.0f, 1.0f}) {
            const Vec3 c{sx * (S * 0.5f + t * 0.5f - 0.2f), 0.0f, sz * (S * 0.5f + t * 0.5f - 0.2f)};
            g.frustum(kHouseStone, c + kUp * (H * 0.62f - 1.6f), c + kUp * (H * 0.62f), 0.3f, 1.45f, 12, false, false);
            roundTower(g, c, 1.4f, H * 0.62f, H + 1.6f, true, kHouseTile, 12);
        }
    }
    // Escalera de piedra por la fachada hasta la puerta, con rellano.
    const float face = t * 0.5f;
    g.boxMinMax(kHouseStone, Vec3{-1.2f, -0.5f, S * 0.5f + face - 0.05f}, Vec3{1.4f, yh, S * 0.5f + face + 1.5f});
    const int steps = static_cast<int>(std::ceil(yh / 0.2f));
    const float rise = yh / static_cast<float>(steps);
    for (int i = 1; i < steps; ++i) {
        const float x0 = 1.4f + 0.3f * static_cast<float>(i - 1);
        g.boxMinMax(kHouseStone, Vec3{x0, -0.5f, S * 0.5f + face - 0.05f}, Vec3{x0 + 0.32f, yh - rise * static_cast<float>(i), S * 0.5f + face + 1.5f},
                    true);
    }
    // Pretil del rellano y la escalera.
    g.boxMinMax(kHouseStone, Vec3{-1.2f, yh, S * 0.5f + face + 1.25f}, Vec3{1.4f, yh + 0.9f, S * 0.5f + face + 1.5f});

    if (s.interior) {
        const float ix = S * 0.5f - t * 0.5f;
        // Salon: suelo de tablas, techo con vigas, hogar, mesa larga y estandartes.
        g.boxMinMax(kHousePlanks, Vec3{-ix, 1.4f, -ix}, Vec3{ix, yh, ix}, true, 4);
        g.boxMinMax(kHousePlanks, Vec3{-ix, yh + hall_h, -ix}, Vec3{ix, yh + hall_h + 0.15f, ix}, true);
        for (float x = -ix + 0.8f; x < ix; x += 1.6f) g.boxMinMax(kHouseBeams, Vec3{x - 0.12f, yh + hall_h - 0.25f, -ix}, Vec3{x + 0.12f, yh + hall_h, ix}, true);
        fireplace(g, frameAt(Vec3{0.0f, yh, -ix + 0.33f}, 0), 1.9f, yh + hall_h - 0.25f);
        table(g, frameAt(Vec3{0.0f, yh, 0.2f}, 1), 3.4f, 0.95f);
        for (float side : {-1.0f, 1.0f}) bench(g, frameAt(Vec3{side * 0.75f, yh, 0.2f}, side < 0.0f ? 1 : 3), 3.2f);
        chair(g, frameAt(Vec3{0.0f, yh, -1.9f}, 0));
        for (float side : {-1.0f, 1.0f}) {
            for (float z : {-ix * 0.45f, ix * 0.45f}) {
                const float x = side * (ix - 0.03f);
                g.boxMinMax(kHouseCloth, Vec3{std::min(x, x - side * 0.04f), yh + 1.3f, z - 0.5f}, Vec3{std::max(x, x - side * 0.04f), yh + 3.7f, z + 0.5f});
                g.boxMinMax(kHouseBeams, Vec3{std::min(x, x - side * 0.1f), yh + 3.7f, z - 0.6f}, Vec3{std::max(x, x - side * 0.1f), yh + 3.8f, z + 0.6f});
            }
        }
        chest(g, frameAt(Vec3{ix - 0.5f, yh, -ix + 0.8f}, 3));
        chest(g, frameAt(Vec3{-ix + 0.5f, yh, -ix + 0.8f}, 1));
        barrel(g, Vec3{ix - 0.5f, yh, ix - 1.0f}, 0.3f, 0.85f, 0.4f);
    }
    return finishModel(g, door, "Torre del homenaje", hinge, dsize);
}

// ---------------------------------------------------------------------------
// Puerta de la muralla
// ---------------------------------------------------------------------------

HouseModel gatehouse(const MedievalSettings& s) {
    const float W = 10.0f;
    const float D = 8.0f;
    const float wall_h = std::max(s.wall_height, 4.0f);
    const float H = wall_h + 3.5f;
    const float pw = 2.1f;   // media anchura del paso
    const float ph = 5.0f;   // alto del paso
    Geo g;
    Geo door;
    // Pilas, puente sobre el paso y cimientos (todo de piedra).
    g.boxMinMax(kHouseStone, Vec3{-W * 0.5f, -2.0f, -D * 0.5f}, Vec3{-pw, H, D * 0.5f}, false, 0);
    g.boxMinMax(kHouseStone, Vec3{pw, -2.0f, -D * 0.5f}, Vec3{W * 0.5f, H, D * 0.5f}, false, 0);
    g.boxMinMax(kHouseStone, Vec3{-pw, ph, -D * 0.5f}, Vec3{pw, H, D * 0.5f}, false, 0);
    // Suelo empedrado del paso.
    g.boxMinMax(kHouseStone, Vec3{-pw, -0.4f, -D * 0.5f - 0.5f}, Vec3{pw, 0.04f, D * 0.5f + 0.5f}, true, 4);
    // Recercado de sillares en las dos bocas (jambas y dintel salientes).
    for (float side : {-1.0f, 1.0f}) {
        const float z0 = side * (D * 0.5f - 0.02f);
        const float z1 = side * (D * 0.5f + 0.16f);
        for (float sx : {-1.0f, 1.0f}) {
            g.boxMinMax(kHouseStone, Vec3{std::min(sx * pw, sx * (pw + 0.45f)), 0.0f, std::min(z0, z1)},
                        Vec3{std::max(sx * pw, sx * (pw + 0.45f)), ph, std::max(z0, z1)});
        }
        g.boxMinMax(kHouseStone, Vec3{-pw - 0.45f, ph, std::min(z0, z1)}, Vec3{pw + 0.45f, ph + 0.6f, std::max(z0, z1)});
        g.boxMinMax(kHouseStone, Vec3{-0.35f, ph - 0.05f, std::min(z0, z1) - 0.02f}, Vec3{0.35f, ph + 0.65f, std::max(z0, z1) + 0.02f});
    }
    // Rastrillo levantado (rejilla de hierro) en la boca de fuera.
    const float rz = D * 0.5f - 0.7f;
    for (float x = -pw + 0.15f; x < pw - 0.1f; x += 0.32f) g.boxMinMax(kHouseIron, Vec3{x - 0.035f, 3.4f, rz - 0.035f}, Vec3{x + 0.035f, ph, rz + 0.035f});
    for (float y = 3.5f; y < ph; y += 0.4f) g.boxMinMax(kHouseIron, Vec3{-pw, y - 0.03f, rz - 0.04f}, Vec3{pw, y + 0.03f, rz + 0.04f});
    for (float x = -pw + 0.15f; x < pw - 0.1f; x += 0.32f) {
        g.frustum(kHouseIron, Vec3{x, 3.4f, rz}, Vec3{x, 3.2f, rz}, 0.035f, 0.0f, 4, false, false);
    }
    // Hojas del porton abiertas contra las paredes del paso (por dentro).
    for (float side : {-1.0f, 1.0f}) {
        const float x = side * (pw - 0.09f);
        g.boxMinMax(kHousePlanks, Vec3{std::min(x, x + side * 0.12f), 0.05f, -D * 0.5f + 0.9f}, Vec3{std::max(x, x + side * 0.12f), 4.6f, -D * 0.5f + 3.0f}, true);
        for (float y : {0.8f, 2.3f, 3.8f}) {
            g.boxMinMax(kHouseIron, Vec3{std::min(x, x - side * 0.02f), y - 0.05f, -D * 0.5f + 0.95f},
                        Vec3{std::max(x, x - side * 0.02f), y + 0.05f, -D * 0.5f + 2.95f});
        }
    }
    // Adarve y almenas alrededor.
    const std::array<Wall, 4> walls = rectWalls(W, D);
    for (int w = 0; w < 4; ++w) {
        const Wall& wall = walls[static_cast<std::size_t>(w)];
        const float half = w < 2 ? W * 0.5f + 0.15f : D * 0.5f + 0.15f;
        wallBox(g, kHouseStone, wall, -half, half, H - 0.35f, H, -0.3f, 0.15f);
        battlements(g, wall, -half, half, H, 1.5f, -0.45f, 0.6f, 0.75f, 0.55f);
    }
    // Saeteras (falsas) y estandarte sobre el arco de fuera.
    for (float x : {-3.6f, 3.6f}) {
        for (float y : {2.5f, wall_h}) g.boxMinMax(kHouseGlass, Vec3{x - 0.07f, y, D * 0.5f}, Vec3{x + 0.07f, y + 1.1f, D * 0.5f + 0.01f});
    }
    g.boxMinMax(kHouseCloth, Vec3{-0.6f, ph + 0.5f, D * 0.5f + 0.02f}, Vec3{0.6f, H - 0.6f, D * 0.5f + 0.05f});
    g.beam(kHouseBeams, Vec3{-0.75f, H - 0.55f, D * 0.5f + 0.08f}, Vec3{0.75f, H - 0.55f, D * 0.5f + 0.08f}, 0.06f, 0.06f);
    // Torreones redondos en las esquinas de fuera.
    for (float sx : {-1.0f, 1.0f}) roundTower(g, Vec3{sx * (W * 0.5f - 0.6f), 0.0f, D * 0.5f - 0.6f}, 2.2f, -2.0f, H + 2.0f, true, kHouseTile);
    return finishModel(g, door, "Puerta de la muralla", Vec3{}, Vec3{});
}

// ---------------------------------------------------------------------------
// Molino de viento
// ---------------------------------------------------------------------------

HouseModel windmill(const MedievalSettings& s) {
    Rng rng(s.seed * 71U + 13U);
    Geo g;
    Geo door;
    const float r0 = 3.4f;
    const float r1 = 2.7f;
    const float h = 9.0f;
    const auto radius_at = [&](float y) { return r0 + (r1 - r0) * (y + 0.5f) / (h + 0.5f); };
    g.frustum(kHousePlaster, Vec3{0.0f, -0.5f, 0.0f}, Vec3{0.0f, h, 0.0f}, r0, r1, 18, false, false);
    g.frustum(kHouseStone, Vec3{0.0f, -0.8f, 0.0f}, Vec3{0.0f, 0.6f, 0.0f}, r0 + 0.25f, r0 + 0.12f, 18, false, true);
    // Puerta y ventanucos (sobre la pared) y escalones.
    const float zd = radius_at(1.2f);
    g.boxMinMax(kHouseBeams, Vec3{-0.62f, 0.6f, zd - 0.12f}, Vec3{0.62f, 2.85f, zd + 0.06f});
    g.boxMinMax(kHousePlanks, Vec3{-0.5f, 0.62f, zd + 0.02f}, Vec3{0.5f, 2.7f, zd + 0.09f}, true);
    g.boxMinMax(kHouseIron, Vec3{0.3f, 1.6f, zd + 0.09f}, Vec3{0.38f, 1.7f, zd + 0.13f});
    g.boxMinMax(kHouseStone, Vec3{-0.8f, -0.3f, zd + 0.1f}, Vec3{0.8f, 0.6f, zd + 0.7f}, true);
    for (const auto& [y, a] : std::array<std::pair<float, float>, 3>{{{4.0f, 0.8f}, {6.3f, 3.3f}, {5.2f, -1.8f}}}) {
        const float rr = radius_at(y);
        const Vec3 dir{std::sin(a), 0.0f, std::cos(a)};
        const Vec3 tangent{std::cos(a), 0.0f, -std::sin(a)};
        g.box(kHouseBeams, dir * (rr - 0.02f) + kUp * y, Vec3{0.4f, 0.5f, 0.1f}, tangent, kUp, dir);
        g.box(kHouseGlass, dir * (rr + 0.06f) + kUp * y, Vec3{0.3f, 0.4f, 0.02f}, tangent, kUp, dir);
    }
    // Caperuza conica de tablillas con su anillo.
    g.frustum(kHouseBeams, Vec3{0.0f, h - 0.1f, 0.0f}, Vec3{0.0f, h + 0.3f, 0.0f}, r1 + 0.25f, r1 + 0.25f, 18, true, true);
    g.frustum(kHouseRoof, Vec3{0.0f, h + 0.3f, 0.0f}, Vec3{0.0f, h + 3.6f, 0.0f}, r1 + 0.3f, 0.0f, 18, true, false, false, 0.0f, kHousePlanks);
    // Eje y cubo de las aspas (a +Z).
    const Vec3 hub{0.0f, h + 1.1f, r1 + 1.0f};
    g.beam(kHouseBeams, Vec3{0.0f, h + 0.9f, 0.4f}, hub, 0.3f, 0.3f);
    g.lights.clear();

    // Aspas: pieza aparte con el eje en su origen (giran en su Z local).
    Geo sails;
    sails.cylinder(kHouseBeams, Vec3{0.0f, 0.0f, -0.2f}, Vec3{0.0f, 0.0f, 0.35f}, 0.32f, 12, true, true);
    for (int arm = 0; arm < 4; ++arm) {
        const float a = kPi * 0.25f + kPi * 0.5f * static_cast<float>(arm);
        const Vec3 dir{std::cos(a), std::sin(a), 0.0f};
        const Vec3 side{-std::sin(a), std::cos(a), 0.0f};
        sails.beam(kHouseBeams, dir * 0.2f + kZ * 0.15f, dir * 7.0f + kZ * 0.15f, 0.2f, 0.2f, kZ);
        // Enrejado: largueros y travesanos, con la lona (enlucido claro) encima.
        for (float off : {0.25f, 0.9f, 1.55f}) {
            sails.beam(kHouseBeams, dir * 1.3f + side * off + kZ * 0.2f, dir * 6.8f + side * off + kZ * 0.2f, 0.06f, 0.06f, kZ);
        }
        for (float r = 1.3f; r <= 6.85f; r += 0.5f) {
            sails.beam(kHouseBeams, dir * r + side * 0.12f + kZ * 0.22f, dir * r + side * 1.62f + kZ * 0.22f, 0.05f, 0.05f, kZ);
        }
        const Vec3 c = dir * 4.05f + side * 0.9f + kZ * 0.26f;
        sails.box(kHousePlaster, c, Vec3{0.62f, 2.6f, 0.012f}, side, dir, kZ);
    }
    HouseModel out = finishModel(g, door, "Molino", Vec3{}, Vec3{});
    HousePart part;
    part.name = "Aspas";
    part.origin = hub;
    sails.finish(part.model, "Aspas");
    out.triangles += sails.triangles();
    out.parts.push_back(std::move(part));
    // La caja incluye las aspas (para el aplanado y la colision de la huella).
    out.bounds_min = Vec3{std::min(out.bounds_min.x, -7.2f), out.bounds_min.y, out.bounds_min.z};
    out.bounds_max = Vec3{std::max(out.bounds_max.x, 7.2f), std::max(out.bounds_max.y, hub.y + 7.2f), std::max(out.bounds_max.z, hub.z + 0.6f)};
    (void)rng;
    return out;
}

// ---------------------------------------------------------------------------
// Objetos
// ---------------------------------------------------------------------------

HouseModel prop(const MedievalSettings& s) {
    Rng rng(s.seed * 83U + static_cast<std::uint32_t>(s.type) * 7U);
    Geo g;
    Geo door;
    std::string name = medievalBuildingName(s.type);
    switch (s.type) {
        case MedievalBuilding::Barrels: {
            const int n = 3 + rng.pick(3);
            for (int i = 0; i < n; ++i) {
                const float a = 2.0f * kPi * static_cast<float>(i) / static_cast<float>(n) + rng.range(-0.3f, 0.3f);
                const float r = i == 0 ? 0.0f : 0.62f;
                barrel(g, Vec3{std::cos(a) * r, 0.0f, std::sin(a) * r}, rng.range(0.27f, 0.32f), rng.range(0.78f, 0.9f), rng.range(0.0f, 1.0f));
            }
            barrelLying(g, Vec3{1.0f, 0.3f, 0.6f}, Vec3{0.3f, 0.0f, 1.0f}, 0.28f, 0.8f);
            break;
        }
        case MedievalBuilding::Crates: {
            const int n = 3 + rng.pick(3);
            for (int i = 0; i < n; ++i) {
                const float sz = rng.range(0.5f, 0.75f);
                const float x = static_cast<float>(i % 3) * 0.8f - 0.8f + rng.range(-0.08f, 0.08f);
                const float y = i >= 3 ? 0.7f : 0.0f;
                crate(g, frameAt(Vec3{i >= 3 ? x * 0.6f : x, 0.0f, rng.range(-0.1f, 0.1f)}, rng.pick(4)), sz, y);
            }
            // Sacos.
            for (int i = 0; i < 2; ++i) {
                g.frustum(kHousePlaster, Vec3{-1.4f + i * 0.5f, 0.0f, 0.5f}, Vec3{-1.4f + i * 0.5f, 0.55f, 0.5f}, 0.24f, 0.17f, 10, true, true);
            }
            break;
        }
        case MedievalBuilding::Cart:
            cart(g, frameAt(Vec3{}, 0));
            hay(g, frameAt(Vec3{0.0f, 0.0f, -0.2f}, 0), 1.1f, 0.5f, 1.7f, 0.82f);
            break;
        case MedievalBuilding::HayBales: {
            const int n = 3 + rng.pick(4);
            for (int i = 0; i < n; ++i) {
                const bool top = i >= 3;
                const float x = static_cast<float>(i % 3) * 1.25f - 1.25f + (top ? 0.6f : 0.0f);
                hay(g, frameAt(Vec3{x, 0.0f, rng.range(-0.1f, 0.1f)}, 0), 1.2f, 0.55f, 0.65f, top ? 0.55f : 0.0f);
                // Cuerdas.
                for (float o : {-0.3f, 0.3f}) {
                    g.boxMinMax(kHouseBeams, Vec3{x + o - 0.015f, (top ? 0.55f : 0.0f) - 0.003f, -0.335f},
                                Vec3{x + o + 0.015f, (top ? 0.55f : 0.0f) + 0.553f, 0.335f});
                }
            }
            break;
        }
        case MedievalBuilding::Woodpile:
            woodpile(g, frameAt(Vec3{}, 0), 2.0f, 1.1f, rng);
            g.log(Vec3{1.6f, 0.0f, 0.6f}, Vec3{1.6f, 0.5f, 0.6f}, 0.26f, 10, false, true, 0.3f, 0.0f);
            g.beam(kHouseBeams, Vec3{1.55f, 0.5f, 0.6f}, Vec3{1.85f, 1.15f, 0.7f}, 0.04f, 0.04f);
            g.box(kHouseIron, Vec3{1.6f, 0.58f, 0.62f}, Vec3{0.05f, 0.08f, 0.012f}, kX, kUp, kZ);
            break;
        case MedievalBuilding::Bench:
        default:
            bench(g, frameAt(Vec3{}, 0), 1.8f);
            fbox(g, kHousePlanks, frameAt(Vec3{}, 0), Vec3{0.0f, 0.75f, -0.15f}, Vec3{0.9f, 0.14f, 0.02f});
            for (float x : {-0.75f, 0.75f}) fbox(g, kHouseBeams, frameAt(Vec3{}, 0), Vec3{x, 0.45f, -0.15f}, Vec3{0.03f, 0.45f, 0.03f}, true, 4);
            break;
    }
    return finishModel(g, door, name, Vec3{}, Vec3{});
}

}  // namespace

const char* medievalBuildingName(MedievalBuilding building) {
    switch (building) {
        case MedievalBuilding::Church: return "Iglesia";
        case MedievalBuilding::Barn: return "Granero";
        case MedievalBuilding::Well: return "Pozo";
        case MedievalBuilding::MarketStall: return "Puesto de mercado";
        case MedievalBuilding::Keep: return "Torre del homenaje";
        case MedievalBuilding::Gatehouse: return "Puerta de la muralla";
        case MedievalBuilding::Windmill: return "Molino";
        case MedievalBuilding::Barrels: return "Barriles";
        case MedievalBuilding::Crates: return "Cajas";
        case MedievalBuilding::Cart: return "Carro";
        case MedievalBuilding::HayBales: return "Pacas de paja";
        case MedievalBuilding::Woodpile: return "Lena";
        case MedievalBuilding::Bench: return "Banco";
    }
    return "Edificio";
}

HouseModel buildMedieval(const MedievalSettings& settings) {
    switch (settings.type) {
        case MedievalBuilding::Church: return church(settings);
        case MedievalBuilding::Barn: return barn(settings);
        case MedievalBuilding::Well: return well(settings);
        case MedievalBuilding::MarketStall: return marketStall(settings);
        case MedievalBuilding::Keep: return keep(settings);
        case MedievalBuilding::Gatehouse: return gatehouse(settings);
        case MedievalBuilding::Windmill: return windmill(settings);
        default: return prop(settings);
    }
}

HouseModel buildCityWall(const CityWallSettings& s) {
    Geo g;
    const std::size_t n = s.points.size();
    if (n < 3) {
        HouseModel empty;
        return empty;
    }
    Vec3 centroid{};
    for (const Vec3& p : s.points) centroid = centroid + p;
    centroid = centroid * (1.0f / static_cast<float>(n));
    const float H = std::max(s.height, 3.0f);
    const float T = std::max(s.thickness, 1.0f);
    const float hT = T * 0.5f;
    const auto tower_at = [&](std::size_t i) { return i < s.towers.size() && s.towers[i]; };
    const auto gap_at = [&](std::size_t i) { return i < s.gaps.size() && s.gaps[i]; };
    for (std::size_t i = 0; i < n; ++i) {
        if (gap_at(i)) continue;
        const std::size_t j = (i + 1) % n;
        Vec3 a = s.points[i];
        Vec3 b = s.points[j];
        Vec3 d{b.x - a.x, 0.0f, b.z - a.z};
        float len = core::length(d);
        if (len < 0.5f) continue;
        d = d * (1.0f / len);
        Vec3 out{d.z, 0.0f, -d.x};
        const Vec3 mid = (a + b) * 0.5f;
        if (core::dot(out, Vec3{mid.x - centroid.x, 0.0f, mid.z - centroid.z}) < 0.0f) out = out * -1.0f;
        // Recortar los extremos que entran en las torres.
        const float trim = s.tower_radius * 0.6f;
        if (tower_at(i)) {
            a = a + d * trim;
            a.y = s.points[i].y + (s.points[j].y - s.points[i].y) * (trim / len);
        }
        if (tower_at(j)) {
            b = b - d * trim;
            b.y = s.points[j].y - (s.points[j].y - s.points[i].y) * (trim / len);
        }
        len = core::length(Vec3{b.x - a.x, 0.0f, b.z - a.z});
        if (len < 0.3f) continue;
        const float bottom = std::min(a.y, b.y) - 2.5f;
        const float ya = a.y + H;
        const float yb = b.y + H;
        const Vec3 ao = a + out * hT;
        const Vec3 bo = b + out * hT;
        const Vec3 ai = a - out * hT;
        const Vec3 bi = b - out * hT;
        const auto P = [](const Vec3& p, float y) { return Vec3{p.x, y, p.z}; };
        const Vec3 down = kUp * -1.0f;
        // Caras de fuera, de dentro y adarve (inclinado si el suelo sube).
        g.flat(kHouseStone, {P(ao, bottom), P(bo, bottom), P(bo, yb), P(ao, ya)}, out, d, down);
        g.flat(kHouseStone, {P(bi, bottom), P(ai, bottom), P(ai, ya), P(bi, yb)}, out * -1.0f, d, down);
        const Vec3 walk_n = core::normalize(core::cross(P(bo, yb) - P(ao, ya), P(ai, ya) - P(ao, ya)));
        g.flat(kHouseStone, {P(ao, ya), P(bo, yb), P(bi, yb), P(ai, ya)}, walk_n.y < 0.0f ? walk_n * -1.0f : walk_n, d, out);
        // Testeros donde no hay torre (junto a las puertas los tapa la puerta).
        if (!tower_at(i)) g.flat(kHouseStone, {P(ai, bottom), P(ao, bottom), P(ao, ya), P(ai, ya)}, d * -1.0f, out, down);
        if (!tower_at(j)) g.flat(kHouseStone, {P(bo, bottom), P(bi, bottom), P(bi, yb), P(bo, yb)}, d, out, down);
        // Parapeto de fuera con merlones (siguiendo la pendiente) y pretil de dentro.
        const float pt = 0.55f;
        const float lower = 0.65f;
        const float merlon = 0.75f;
        const float gap = 0.55f;
        const int pieces = std::max(1, static_cast<int>(std::floor(len / (merlon + gap))));
        const float step = len / static_cast<float>(pieces);
        for (int p = 0; p < pieces; ++p) {
            const float t0 = static_cast<float>(p) * step / len;
            const float t1 = static_cast<float>(p + 1) * step / len;
            const Vec3 q0 = a + (b - a) * t0;
            const Vec3 q1 = a + (b - a) * t1;
            const float y0 = q0.y + H;
            const float y1 = q1.y + H;
            const Vec3 c0 = q0 + out * (hT - pt * 0.5f);
            const Vec3 c1 = q1 + out * (hT - pt * 0.5f);
            // Antepecho continuo.
            const Vec3 mid_p = (c0 + c1) * 0.5f;
            const float seg = core::length(Vec3{c1.x - c0.x, 0.0f, c1.z - c0.z});
            const Vec3 axis = core::normalize(Vec3{c1.x - c0.x, y1 - y0, c1.z - c0.z});
            const Vec3 ay = core::normalize(core::cross(out, axis)) * (core::cross(out, axis).y < 0.0f ? -1.0f : 1.0f);
            const float slope_len = std::sqrt(seg * seg + (y1 - y0) * (y1 - y0));
            g.box(kHouseStone, Vec3{mid_p.x, (y0 + y1) * 0.5f + lower * 0.5f, mid_p.z}, Vec3{pt * 0.5f, lower * 0.5f, slope_len * 0.5f + 0.01f}, out, ay, axis,
                  false);
            // Merlon.
            const Vec3 m = q0 + (q1 - q0) * (merlon * 0.5f / step) + out * (hT - pt * 0.5f);
            const float ym = q0.y + (q1.y - q0.y) * (merlon * 0.5f / step) + H + lower;
            g.box(kHouseStone, Vec3{m.x, ym + 0.42f, m.z}, Vec3{pt * 0.5f, 0.42f, merlon * 0.5f}, out, kUp, d, false);
            // Pretil bajo por dentro.
            const Vec3 ci = (q0 + q1) * 0.5f - out * (hT - 0.15f);
            g.box(kHouseStone, Vec3{ci.x, (y0 + y1) * 0.5f + 0.25f, ci.z}, Vec3{0.15f, 0.25f, slope_len * 0.5f + 0.01f}, out, ay, axis, false);
        }
    }
    // Torres redondas.
    Rng rng(s.seed * 97U + 1U);
    for (std::size_t i = 0; i < n; ++i) {
        if (!tower_at(i)) continue;
        const Vec3 p = s.points[i];
        const bool roof = rng.chance(0.5f);
        roundTower(g, Vec3{p.x, 0.0f, p.z}, s.tower_radius, p.y - 2.5f, p.y + H + 2.5f, roof, rng.chance(0.5f) ? kHouseTile : kHouseRoof);
    }
    Geo door;
    return finishModel(g, door, "Muralla", Vec3{}, Vec3{});
}

HouseModel buildFences(const std::vector<std::pair<core::Vec3, core::Vec3>>& segments) {
    Geo g;
    for (const auto& [a, b] : segments) fence(g, a, b);
    Geo door;
    return finishModel(g, door, "Vallas", Vec3{}, Vec3{});
}

}  // namespace cramion::asset

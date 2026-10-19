// Pruebas de los lightmaps por UV2 (PLAN-LIGHTMAPS.md, Fases 0 a 3). Se
// incluye dentro del espacio anonimo de Features21Tests.cpp (usa check()).

// --- Geometria de prueba ------------------------------------------------------

// Triangulos con sus vertices (cada cara los suyos: aristas duras, como un
// modelo importado) y una normal por cara.
struct TestMesh {
    std::vector<Vec3> p;
    std::vector<Vec3> n;
    std::vector<std::uint32_t> idx;

    // Cuadrilatero a, b, c, d; la cara mira hacia `facing` (se da la vuelta si hace falta).
    void quad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, const Vec3& facing) {
        if (core::dot(core::cross(b - a, c - a), facing) < 0.0f) std::swap(b, d);
        const Vec3 normal = core::normalize(core::cross(b - a, c - a));
        const auto base = static_cast<std::uint32_t>(p.size());
        for (const Vec3& v : {a, b, c, d}) {
            p.push_back(v);
            n.push_back(normal);
        }
        idx.insert(idx.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    // Suelo horizontal (y) de x0..x1, z0..z1 mirando hacia arriba.
    void floor(float x0, float x1, float z0, float z1, float y = 0.0f) {
        quad(Vec3{x0, y, z0}, Vec3{x0, y, z1}, Vec3{x1, y, z1}, Vec3{x1, y, z0}, Vec3{0.0f, 1.0f, 0.0f});
    }
    // Rectangulo en el plano x = value (y0..y1, z0..z1), mirando hacia `sign` x.
    void wallX(float x, float y0, float y1, float z0, float z1, float sign) {
        quad(Vec3{x, y0, z0}, Vec3{x, y1, z0}, Vec3{x, y1, z1}, Vec3{x, y0, z1}, Vec3{sign, 0.0f, 0.0f});
    }
    void wallZ(float z, float x0, float x1, float y0, float y1, float sign) {
        quad(Vec3{x0, y0, z}, Vec3{x1, y0, z}, Vec3{x1, y1, z}, Vec3{x0, y1, z}, Vec3{0.0f, 0.0f, sign});
    }
    // Caja cerrada con las caras hacia fuera (inward = hacia dentro: una habitacion).
    void box(const Vec3& lo, const Vec3& hi, bool inward = false) {
        const float s = inward ? -1.0f : 1.0f;
        wallX(lo.x, lo.y, hi.y, lo.z, hi.z, -s);
        wallX(hi.x, lo.y, hi.y, lo.z, hi.z, s);
        quad(Vec3{lo.x, lo.y, lo.z}, Vec3{hi.x, lo.y, lo.z}, Vec3{hi.x, lo.y, hi.z}, Vec3{lo.x, lo.y, hi.z}, Vec3{0.0f, -s, 0.0f});
        quad(Vec3{lo.x, hi.y, lo.z}, Vec3{hi.x, hi.y, lo.z}, Vec3{hi.x, hi.y, hi.z}, Vec3{lo.x, hi.y, hi.z}, Vec3{0.0f, s, 0.0f});
        wallZ(lo.z, lo.x, hi.x, lo.y, hi.y, -s);
        wallZ(hi.z, lo.x, hi.x, lo.y, hi.y, s);
    }
    void triangle(const Vec3& a, const Vec3& b, const Vec3& c) {
        const auto base = static_cast<std::uint32_t>(p.size());
        Vec3 normal = core::normalize(core::cross(b - a, c - a));
        if (core::dot(normal, normal) < 0.5f) normal = Vec3{0.0f, 1.0f, 0.0f};
        for (const Vec3& v : {a, b, c}) {
            p.push_back(v);
            n.push_back(normal);
        }
        idx.insert(idx.end(), {base, base + 1, base + 2});
    }
    // Esfera UV (vertices compartidos: una sola componente suave).
    void sphere(const Vec3& center, float radius, int segments, int rings) {
        const auto base = static_cast<std::uint32_t>(p.size());
        for (int r = 0; r <= rings; ++r) {
            const float theta = 3.14159265f * static_cast<float>(r) / static_cast<float>(rings);
            for (int s = 0; s <= segments; ++s) {
                const float phi = 2.0f * 3.14159265f * static_cast<float>(s) / static_cast<float>(segments);
                const Vec3 d{std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi)};
                p.push_back(center + d * radius);
                n.push_back(d);
            }
        }
        const auto row = static_cast<std::uint32_t>(segments + 1);
        for (int r = 0; r < rings; ++r) {
            for (int s = 0; s < segments; ++s) {
                const std::uint32_t a = base + static_cast<std::uint32_t>(r) * row + static_cast<std::uint32_t>(s);
                const std::uint32_t b = a + row;
                if (r > 0) idx.insert(idx.end(), {a, a + 1, b});
                if (r + 1 < rings) idx.insert(idx.end(), {a + 1, b + 1, b});
            }
        }
    }
    asset::ModelData model(const char* name = "prueba") const {
        asset::ModelData m;
        m.name = name;
        std::vector<asset::SkinnedVertex> vertices(p.size());
        for (std::size_t i = 0; i < p.size(); ++i) {
            vertices[i].position = p[i];
            vertices[i].normal = n[i];
            vertices[i].weights[0] = 1.0f;
        }
        m.vertices = std::move(vertices);
        m.indices = idx;
        asset::SubMesh sm;
        sm.first_index = 0;
        sm.index_count = static_cast<std::uint32_t>(idx.size());
        sm.material = 0;
        m.submeshes = {sm};
        m.materials = {asset::MaterialData{}};
        asset::computeSubmeshBounds(m);
        return m;
    }
    void toScene(lighting::BakeScene& scene, const Vec3& albedo = Vec3{0.5f, 0.5f, 0.5f}) const {
        for (std::size_t t = 0; t + 2 < idx.size(); t += 3) {
            scene.triangles.push_back(lighting::BakeTriangle{p[idx[t]], p[idx[t + 1]], p[idx[t + 2]], albedo, {}});
        }
    }
};

// Un horneado de lightmaps de prueba: los receptores (mallas en el mundo,
// matriz identidad) sobre la geometria de `scene`.
struct LightmapBakeResult {
    bool ok = false;
    std::string error;
    gfx::BakedLighting out;
    lighting::LightmapBakeStats stats;
    std::vector<asset::ModelData> applied;  // cada receptor con su UV2
};

lighting::LightmapBakeSettings testLightmapSettings(int threads = 2) {
    lighting::LightmapBakeSettings s;
    s.texels_per_meter = 20.0f;
    s.sun_samples = 64;
    s.indirect_samples = 0;
    s.bounces = 0;
    s.denoise = false;
    s.threads = threads;
    s.probes = false;
    return s;
}

LightmapBakeResult bakeReceivers(const std::vector<asset::ModelData>& receivers, const lighting::BakeScene& scene,
                                 const lighting::LightmapBakeSettings& settings,
                                 const std::vector<std::uint32_t>& material = {}) {
    LightmapBakeResult r;
    lighting::LightmapBakeInput input;
    input.scene = scene;
    input.lightmaps = settings;
    for (std::size_t i = 0; i < receivers.size(); ++i) {
        lighting::LightmapReceiver rec;
        rec.entity = Uuid{0x10ull, 0x100ull + i};
        rec.model = Uuid{0x20ull, 0x200ull + i};
        rec.part = 0;
        rec.name = "receptor " + std::to_string(i);
        rec.materials = {i < material.size() ? material[i] : lighting::kNoBakeMaterial};
        rec.transparent = {0};
        input.receivers.push_back(rec);
        input.receiver_meshes.push_back(std::make_shared<asset::ModelData>(receivers[i]));
    }
    std::vector<std::pair<lighting::LightmapUvPartKey, std::shared_ptr<const lighting::LightmapUvLayout>>> layouts;
    r.ok = lighting::bakeLighting(input, r.out, layouts, nullptr, &r.stats, &r.error);
    for (std::size_t i = 0; i < receivers.size(); ++i) {
        asset::ModelData copy = receivers[i];
        for (const auto& [key, layout] : layouts) {
            if (key.model == input.receivers[i].model && layout) lighting::applyLightmapUvs(copy, *layout);
        }
        r.applied.push_back(std::move(copy));
    }
    return r;
}

// Lo que se ve en el lightmap del receptor `receiver` en el punto del mundo
// `point` (sobre su superficie), con bilineal como la GPU. -1 si no cae en el.
core::Vec4 sampleLightmapAt(const LightmapBakeResult& r, std::size_t receiver, const Vec3& point) {
    if (receiver >= r.applied.size() || receiver >= r.out.lightmaps.records.size()) return core::Vec4{-1.0f, -1.0f, -1.0f, -1.0f};
    const asset::ModelData& m = r.applied[receiver];
    const std::vector<asset::SkinnedVertex>& v = m.vertices.get();
    const std::vector<std::uint32_t>& idx = m.indices.get();
    for (std::size_t t = 0; t + 2 < idx.size(); t += 3) {
        const Vec3& a = v[idx[t]].position;
        const Vec3& b = v[idx[t + 1]].position;
        const Vec3& c = v[idx[t + 2]].position;
        // Baricentricas en el plano del triangulo.
        const Vec3 n = core::cross(b - a, c - a);
        const float area = core::dot(n, n);
        if (area < 1e-12f) continue;
        const Vec3 q = point - n * (core::dot(point - a, n) / area);
        const float w1 = core::dot(core::cross(q - a, c - a), n) / area;
        const float w2 = core::dot(core::cross(b - a, q - a), n) / area;
        const float w0 = 1.0f - w1 - w2;
        if (w0 < -1e-4f || w1 < -1e-4f || w2 < -1e-4f) continue;
        const core::Vec2& ua = m.lightmap_uvs[idx[t]];
        const core::Vec2& ub = m.lightmap_uvs[idx[t + 1]];
        const core::Vec2& uc = m.lightmap_uvs[idx[t + 2]];
        if (ua.x < 0.0f || ub.x < 0.0f || uc.x < 0.0f) return core::Vec4{-1.0f, -1.0f, -1.0f, -1.0f};
        const float u = ua.x * w0 + ub.x * w1 + uc.x * w2;
        const float uv = ua.y * w0 + ub.y * w1 + uc.y * w2;
        const gfx::LightmapRecord& record = r.out.lightmaps.records[receiver];
        const gfx::LightmapPage& page = r.out.lightmaps.pages[record.page];
        const float au = u * record.scale_offset.x + record.scale_offset.z;
        const float av = uv * record.scale_offset.y + record.scale_offset.w;
        return lighting::sampleLightmapPage(page, au * static_cast<float>(r.out.lightmaps.width) / static_cast<float>(page.width),
                                            av * static_cast<float>(r.out.lightmaps.height) / static_cast<float>(page.height));
    }
    return core::Vec4{-1.0f, -1.0f, -1.0f, -1.0f};
}

// --- Fase 0 ---------------------------------------------------------------------

void testSpotConeParity() {
    std::printf("Cono de los focos: horneado = render\n");
    const float cos_inner = std::cos(0.35f), cos_outer = std::cos(0.6f);
    float worst = 0.0f;
    for (int i = 0; i < 20; ++i) {
        const float angle = 0.7f * static_cast<float>(i) / 19.0f;
        const float c = std::cos(angle);
        // lighting.frag: cone = clamp((c - outer) / max(inner - outer, 0.0001), 0, 1); cone * cone
        const float x = std::clamp((c - cos_outer) / std::max(cos_inner - cos_outer, 0.0001f), 0.0f, 1.0f);
        worst = std::max(worst, std::fabs(lighting::spotCone(c, cos_inner, cos_outer) - x * x));
    }
    check(worst < 1e-6f, "spotCone coincide con lighting.frag en 20 angulos");
}

void testAverageTexture() {
    std::printf("Color medio de una textura\n");
    asset::TextureData t;
    t.width = 2;
    t.height = 2;
    t.format = asset::TextureFormat::Rgba8;
    t.pixels = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 0};
    Vec3 rgb;
    float opaque = 0.0f;
    const bool ok = asset::averageTextureLinear(t, rgb, &opaque);
    std::printf("  (%.3f %.3f %.3f, opacos %.2f)\n", static_cast<double>(rgb.x), static_cast<double>(rgb.y),
                static_cast<double>(rgb.z), static_cast<double>(opaque));
    check(ok && std::fabs(rgb.x - 1.0f / 3.0f) < 1e-3f && std::fabs(rgb.y - 1.0f / 3.0f) < 1e-3f &&
              std::fabs(rgb.z - 1.0f / 3.0f) < 1e-3f,
          "media lineal de los texeles opacos");
    check(std::fabs(opaque - 0.75f) < 1e-6f, "opaque_fraction cuenta el texel de alfa 0");
}

void testBakeSkySh() {
    std::printf("Horneado con el cielo del render\n");
    lighting::BakeScene scene;
    const Vec3 albedo{0.5f, 0.5f, 0.5f};
    scene.triangles.push_back({Vec3{-50, 0, -50}, Vec3{50, 0, 50}, Vec3{50, 0, -50}, albedo, {}});
    scene.triangles.push_back({Vec3{-50, 0, -50}, Vec3{-50, 0, 50}, Vec3{50, 0, 50}, albedo, {}});
    scene.lights.sun.intensity = 0.0f;
    // Cielo constante L = 1: c0 = 1 / Y00 * ... = 3.5449 (los demas, 0).
    scene.sky_sh.fill(Vec3{});
    scene.sky_sh[0] = Vec3{3.5449f, 3.5449f, 3.5449f};
    scene.has_sky_sh = true;
    check(std::fabs(lighting::irradianceFromSh(scene.sky_sh, Vec3{0.0f, 1.0f, 0.0f}).x - 1.0f) < 1e-3f,
          "con c0 = 3.5449 la irradiancia del cielo es 1");
    lighting::BakeVolume volume;
    volume.min = Vec3{-0.5f, 1.0f, -0.5f};
    volume.size = Vec3{1.0f, 0.1f, 1.0f};
    volume.nx = volume.ny = volume.nz = 2;
    scene.volumes = {volume};
    scene.settings.rays = 2048;
    scene.settings.bounces = 1;
    scene.settings.threads = 2;
    gfx::BakedLighting out;
    std::string error;
    check(lighting::bakeProbes(scene, out, nullptr, nullptr, &error), "horneado con cielo constante");
    const Vec3 down = out.probes.empty() ? Vec3{} : lighting::probeIrradiance(out.probes.data(), Vec3{0.0f, -1.0f, 0.0f});
    std::printf("  (hacia abajo %.3f)\n", static_cast<double>(down.x));
    check(down.x > 0.45f && down.x < 0.55f, "la sonda a 1 m ve el suelo (albedo 0.5) con luz 0.5 +-10%");
}

void testBakeDeterminism() {
    std::printf("Horneado determinista\n");
    lighting::BakeScene scene;
    TestMesh mesh;
    mesh.floor(-5.0f, 5.0f, -5.0f, 5.0f);
    mesh.box(Vec3{-1.0f, 0.0f, -1.0f}, Vec3{1.0f, 2.0f, 1.0f});
    mesh.toScene(scene);
    scene.lights.sun.direction = core::normalize(Vec3{0.3f, -1.0f, 0.2f});
    scene.lights.sun.color = Vec3{1.0f, 1.0f, 1.0f};
    scene.lights.sun.intensity = 3.0f;
    scene.sun_baked = true;
    lighting::BakeVolume volume;
    volume.min = Vec3{-4.0f, 0.5f, -4.0f};
    volume.size = Vec3{8.0f, 3.0f, 8.0f};
    volume.nx = volume.ny = volume.nz = 4;
    scene.volumes = {volume};
    scene.settings.rays = 64;
    scene.settings.bounces = 2;
    scene.settings.direct_samples = 8;
    gfx::BakedLighting one, three;
    scene.settings.threads = 1;
    const bool a = lighting::bakeProbes(scene, one);
    scene.settings.threads = 3;
    const bool b = lighting::bakeProbes(scene, three);
    check(a && b && one.probes.size() == three.probes.size() && !one.probes.empty() &&
              std::memcmp(one.probes.data(), three.probes.data(), one.probes.size() * sizeof(core::Vec4)) == 0,
          "1 y 3 hilos dan las mismas sondas byte a byte");
}

// --- Fase 1 ---------------------------------------------------------------------

// Cobertura conservadora: el triangulo (en texeles) toca el cuadrado del texel?
bool testTouchesTexel(const core::Vec2& a, const core::Vec2& b, const core::Vec2& c, float x, float y) {
    if (std::max({a.x, b.x, c.x}) <= x || std::min({a.x, b.x, c.x}) >= x + 1.0f) return false;
    if (std::max({a.y, b.y, c.y}) <= y || std::min({a.y, b.y, c.y}) >= y + 1.0f) return false;
    const float orient = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    const core::Vec2 corners[4] = {{x, y}, {x + 1.0f, y}, {x, y + 1.0f}, {x + 1.0f, y + 1.0f}};
    const core::Vec2 edges[3][2] = {{a, b}, {b, c}, {c, a}};
    for (const auto& e : edges) {
        bool outside = true;
        for (const core::Vec2& q : corners) {
            const float side = (e[1].x - e[0].x) * (q.y - e[0].y) - (e[1].y - e[0].y) * (q.x - e[0].x);
            if (side * orient > 0.0f) {
                outside = false;
                break;
            }
        }
        if (outside) return false;
    }
    return true;
}

void testLightmapUvUnwrap() {
    std::printf("UV de lightmap (xatlas)\n");
#if CRAMION_HAS_LIGHTMAP_UNWRAP
    TestMesh mesh;
    mesh.box(Vec3{0.0f, 0.0f, 0.0f}, Vec3{4.0f, 3.0f, 4.0f}, true);  // habitacion: 24 vertices
    const std::size_t room_triangles = mesh.idx.size() / 3;
    mesh.sphere(Vec3{10.0f, 1.0f, 0.0f}, 1.0f, 32, 16);
    const std::size_t lit_triangles = mesh.idx.size() / 3;
    const std::size_t first_tiny = mesh.p.size();
    for (int i = 0; i < 200; ++i) {
        const Vec3 lo{20.0f + static_cast<float>(i % 20) * 0.1f, 0.0f, static_cast<float>(i / 20) * 0.1f};
        mesh.box(lo, lo + Vec3{0.03f, 0.03f, 0.03f});
    }
    mesh.triangle(Vec3{30.0f, 0.0f, 0.0f}, Vec3{31.0f, 0.0f, 0.0f}, Vec3{32.0f, 0.0f, 0.0f});  // degenerado
    const asset::ModelData original = mesh.model();
    lighting::LightmapUvSettings s;
    s.texels_per_unit = 20.0f;
    s.padding = 2;
    lighting::LightmapUvLayout layout;
    std::string error;
    const bool ok = lighting::unwrapLightmapUvs(original, s, layout, {}, &error);
    std::printf("  (%u x %u, %u cartas, %u ignorados, ocupacion %.2f%s%s)\n", layout.width, layout.height, layout.charts,
                layout.ignored_triangles, static_cast<double>(layout.utilization), error.empty() ? "" : ", ", error.c_str());
    check(ok && layout.width > 0 && layout.height > 0, "desenvuelve la malla");
    asset::ModelData applied = original;
    check(lighting::applyLightmapUvs(applied, layout), "(i) applyLightmapUvs acepta la disposicion");
    check(applied.lightmap_uvs.size() == applied.vertices.size(), "(i) una UV2 por vertice");
    const std::vector<asset::SkinnedVertex>& v = applied.vertices.get();
    const std::vector<std::uint32_t>& idx = applied.indices.get();
    // (g) mismo orden de caras: las posiciones de cada triangulo no cambian.
    bool same = idx.size() == original.indices.size();
    for (std::size_t i = 0; same && i < idx.size(); ++i) {
        const Vec3& pa = v[idx[i]].position;
        const Vec3& pb = original.vertices.get()[original.indices.get()[i]].position;
        same = pa.x == pb.x && pa.y == pb.y && pa.z == pb.z;
    }
    check(same, "(g) los triangulos siguen en su orden y con sus posiciones");
    // (a) y (e)
    bool lit_ok = true, tiny_ok = true;
    for (std::size_t t = 0; t < idx.size() / 3; ++t) {
        for (int k = 0; k < 3; ++k) {
            const core::Vec2& uv = applied.lightmap_uvs[idx[t * 3 + static_cast<std::size_t>(k)]];
            if (t < lit_triangles) lit_ok = lit_ok && uv.x >= 0.0f && uv.x <= 1.0f && uv.y >= 0.0f && uv.y <= 1.0f;
            else tiny_ok = tiny_ok && uv.x < 0.0f;
        }
    }
    (void)first_tiny;
    (void)room_triangles;
    check(lit_ok, "(a) la habitacion y la esfera tienen sus UV2 en [0, 1]");
    check(tiny_ok, "(e) los cubos de 3 cm y el triangulo degenerado van sin lightmap (centinela)");
    // Cartas: componentes en el espacio UV (triangulos que comparten un
    // vertice con la misma posicion y la misma UV2).
    std::vector<std::uint32_t> chart(idx.size() / 3);
    std::vector<std::uint32_t> parent(v.size());
    for (std::uint32_t i = 0; i < parent.size(); ++i) parent[i] = i;
    const std::function<std::uint32_t(std::uint32_t)> find = [&](std::uint32_t x) {
        while (parent[x] != x) x = parent[x] = parent[parent[x]];
        return x;
    };
    {
        std::map<std::array<float, 5>, std::uint32_t> same;
        for (std::uint32_t i = 0; i < v.size(); ++i) {
            const core::Vec2& uv = applied.lightmap_uvs[i];
            if (uv.x < 0.0f) continue;
            const std::array<float, 5> key = {v[i].position.x, v[i].position.y, v[i].position.z, uv.x, uv.y};
            const auto [it, inserted] = same.try_emplace(key, i);
            if (!inserted) parent[find(i)] = find(it->second);
        }
    }
    for (std::size_t t = 0; t < lit_triangles; ++t) {
        const std::uint32_t a = find(idx[t * 3]);
        parent[find(idx[t * 3 + 1])] = a;
        parent[find(idx[t * 3 + 2])] = find(idx[t * 3]);
    }
    for (std::size_t t = 0; t < lit_triangles; ++t) chart[t] = find(idx[t * 3]);
    // (b) y (c): raster conservador; ningun texel de dos cartas, ni dilatando 1.
    const auto w = static_cast<int>(layout.width), h = static_cast<int>(layout.height);
    std::vector<std::int64_t> owner(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), -1);
    bool overlap = false;
    std::map<std::uint32_t, double> chart_uv_area, chart_world_area;
    for (std::size_t t = 0; t < lit_triangles; ++t) {
        core::Vec2 c[3];
        for (int k = 0; k < 3; ++k) {
            const core::Vec2& uv = applied.lightmap_uvs[idx[t * 3 + static_cast<std::size_t>(k)]];
            c[k] = core::Vec2{uv.x * static_cast<float>(w), uv.y * static_cast<float>(h)};
        }
        chart_uv_area[chart[t]] += 0.5 * std::fabs(static_cast<double>((c[1].x - c[0].x) * (c[2].y - c[0].y) - (c[1].y - c[0].y) * (c[2].x - c[0].x)));
        const Vec3& pa = v[idx[t * 3]].position;
        chart_world_area[chart[t]] += 0.5 * static_cast<double>(core::length(core::cross(v[idx[t * 3 + 1]].position - pa, v[idx[t * 3 + 2]].position - pa)));
        const int x0 = std::max(0, static_cast<int>(std::floor(std::min({c[0].x, c[1].x, c[2].x}))));
        const int x1 = std::min(w - 1, static_cast<int>(std::floor(std::max({c[0].x, c[1].x, c[2].x}))));
        const int y0 = std::max(0, static_cast<int>(std::floor(std::min({c[0].y, c[1].y, c[2].y}))));
        const int y1 = std::min(h - 1, static_cast<int>(std::floor(std::max({c[0].y, c[1].y, c[2].y}))));
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                if (!testTouchesTexel(c[0], c[1], c[2], static_cast<float>(x), static_cast<float>(y))) continue;
                std::int64_t& o = owner[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)];
                if (o >= 0 && o != chart[t]) overlap = true;
                o = chart[t];
            }
        }
    }
    check(!overlap, "(b) sin solapes entre cartas");
    bool touching = false;
    for (int y = 0; y < h && !touching; ++y) {
        for (int x = 0; x < w && !touching; ++x) {
            const std::int64_t o = owner[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)];
            if (o < 0) continue;
            for (int dy = -1; dy <= 1 && !touching; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    const std::int64_t q = owner[static_cast<std::size_t>(ny) * static_cast<std::size_t>(w) + static_cast<std::size_t>(nx)];
                    if (q >= 0 && q != o) touching = true;
                }
            }
        }
    }
    check(!touching, "(c) separadas: dilatando 1 texel tampoco se tocan");
    bool density = true;
    for (const auto& [id, uv_area] : chart_uv_area) {
        if (uv_area <= 4.0) continue;
        const double ratio = uv_area / (chart_world_area[id] * 400.0);
        if (ratio < 0.8 || ratio > 1.25) {
            density = false;
            std::printf("  (carta con densidad %.2f)\n", ratio);
        }
    }
    check(density, "(d) cada carta con su densidad (0.8..1.25 de 20 texeles/m)");
    check(layout.utilization > 0.5f, "(f) ocupacion > 0.5");
    lighting::LightmapUvLayout again;
    check(lighting::unwrapLightmapUvs(original, s, again, {}, nullptr) && again.layout_hash == layout.layout_hash,
          "(h) dos desenvueltas dan la misma disposicion");
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "cramion_lightmap_test.cruv";
    lighting::LightmapUvLayout back;
    check(lighting::saveLightmapUvLayout(file, layout) && lighting::loadLightmapUvLayout(file, back) &&
              lighting::encodeLightmapUvLayout(back) == lighting::encodeLightmapUvLayout(layout),
          "(j) el .cruv se guarda y se lee igual");
    std::error_code ec;
    std::filesystem::remove(file, ec);
#else
    check(true, "(sin xatlas en este binario)");
#endif
}

void testLightmapUvApplyRejects() {
    std::printf("UV de lightmap de otra version de la malla\n");
#if CRAMION_HAS_LIGHTMAP_UNWRAP
    TestMesh mesh;
    mesh.box(Vec3{0.0f, 0.0f, 0.0f}, Vec3{2.0f, 2.0f, 2.0f});
    const asset::ModelData original = mesh.model();
    lighting::LightmapUvLayout layout;
    lighting::LightmapUvSettings s;
    check(lighting::unwrapLightmapUvs(original, s, layout, {}, nullptr), "desenvuelve la caja");
    asset::ModelData moved = original;
    moved.vertices[3].position.y += 0.25f;
    const std::size_t vertices = moved.vertices.size();
    check(!lighting::applyLightmapUvs(moved, layout) && moved.vertices.size() == vertices && moved.lightmap_uvs.empty(),
          "con una posicion movida se rechaza sin tocar la malla");
#else
    check(true, "(sin xatlas en este binario)");
#endif
}

void testLightmapUvReadModel() {
    std::printf("Modelo leido con sus UV de lightmap\n");
#if CRAMION_HAS_LIGHTMAP_UNWRAP
    TestMesh mesh;
    mesh.box(Vec3{0.0f, 0.0f, 0.0f}, Vec3{4.0f, 3.0f, 4.0f}, true);
    mesh.sphere(Vec3{2.0f, 1.0f, 2.0f}, 0.8f, 48, 24);
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / "cramion_lightmap_readmodel";
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    const std::filesystem::path file = folder / "modelo.crdata";
    const Uuid uuid{0x1111ull, 0x2222ull};
    const std::vector<assets::ModelNode> nodes = {assets::ModelNode{"raiz", -1, core::Mat4::identity(), 0}};
    std::string error;
    check(assets::writeGeneratedModel(file, uuid, "modelo", nodes, {mesh.model()}, &error), "escribe el .crdata");
    // La disposicion se hace sobre la malla tal como sale del .crdata.
    const auto raw = assets::AssetManager::readModelGeometry(uuid, file, "modelo");
    lighting::LightmapUvLayout layout;
    const bool unwrapped = raw && !raw->parts.empty() &&
                           lighting::unwrapLightmapUvs(*raw->parts[0], lighting::LightmapUvSettings{}, layout, {}, nullptr);
    const std::filesystem::path cruv = folder / "modelo.cruv";
    check(unwrapped && lighting::saveLightmapUvLayout(cruv, layout), "disposicion en un .cruv");
    lighting::LightmapUvSet set;
    lighting::LightmapUvSet::Source source;
    source.file = cruv;
    set.parts[lighting::LightmapUvPartKey{uuid, 0}] = source;
    const auto model = assets::AssetManager::readModel(uuid, file, "modelo", true, &set);
    const asset::ModelData* part = model && !model->parts.empty() ? model->parts[0].get() : nullptr;
    check(part != nullptr && part->lightmap_uvs.size() == part->vertices.size() && part->lightmap_layout_hash == layout.layout_hash,
          "readModel devuelve la pieza partida con su UV2");
    bool lods_ok = part != nullptr;
    if (part != nullptr) {
        for (const std::uint32_t i : part->lod_indices.get()) lods_ok = lods_ok && i < part->vertices.size();
    }
    check(lods_ok, "los LODs (si los hay) son de la malla partida");
    const auto plain = assets::AssetManager::readModel(uuid, file, "modelo");
    check(plain && !plain->parts.empty() && plain->parts[0]->lightmap_uvs.empty(), "sin el conjunto, sin UV2");
    std::filesystem::remove_all(folder, ec);
#else
    check(true, "(sin xatlas en este binario)");
#endif
}

// --- Fase 2 ---------------------------------------------------------------------

void testLightmapAtlasPacking() {
    std::printf("Paginas de los lightmaps\n");
    std::mt19937 rng(7);
    std::uniform_int_distribution<int> size(1, 300);
    std::vector<lighting::LightmapRect> rects(300);
    for (lighting::LightmapRect& r : rects) {
        r.width = static_cast<std::uint32_t>(size(rng));
        r.height = static_cast<std::uint32_t>(size(rng));
    }
    rects.push_back(lighting::LightmapRect{3000, 200});  // no cabe: se escala
    const lighting::LightmapPacking packing = lighting::packLightmapRects(rects, 1024, 2);
    std::printf("  (%u paginas, array %u x %u)\n", packing.pages, packing.width, packing.height);
    bool inside = true, apart = true, offsets = true;
    for (std::size_t i = 0; i < rects.size(); ++i) {
        const lighting::LightmapRect& a = rects[i];
        inside = inside && a.page < packing.pages && a.x >= 1 && a.y >= 1 && a.x + a.placed_width <= packing.page_width[a.page] &&
                 a.y + a.placed_height <= packing.page_height[a.page];
        for (std::size_t j = i + 1; j < rects.size(); ++j) {
            const lighting::LightmapRect& b = rects[j];
            if (a.page != b.page) continue;
            const bool separated = a.x + a.placed_width + 2 <= b.x || b.x + b.placed_width + 2 <= a.x ||
                                   a.y + a.placed_height + 2 <= b.y || b.y + b.placed_height + 2 <= a.y;
            apart = apart && separated;
        }
        const core::Vec4 so = lighting::lightmapScaleOffset(a, packing);
        const float u0 = so.z * static_cast<float>(packing.width), v0 = so.w * static_cast<float>(packing.height);
        const float u1 = (so.x + so.z) * static_cast<float>(packing.width), v1 = (so.y + so.w) * static_cast<float>(packing.height);
        offsets = offsets && std::fabs(u0 - static_cast<float>(a.x)) < 1e-2f && std::fabs(v0 - static_cast<float>(a.y)) < 1e-2f &&
                  std::fabs(u1 - static_cast<float>(a.x + a.placed_width)) < 1e-2f &&
                  std::fabs(v1 - static_cast<float>(a.y + a.placed_height)) < 1e-2f;
    }
    check(inside, "todo dentro de sus paginas");
    check(apart, "sin solapes y con 2 texeles de hueco");
    check(offsets, "scale_offset lleva (0,0) y (1,1) a las esquinas del rect");
    check(rects.back().scaled && rects.back().placed_width <= 1022, "el que no cabe en una pagina se escala");
}

// Habitacion de 4 x 3 x 4 m con una ventana de 1 x 1 m en la pared x = 4;
// el sol entra a 45 grados y hace un parche en el suelo (x 2..3, z 1.5..2.5).
lighting::BakeScene windowRoom() {
    lighting::BakeScene scene;
    TestMesh walls;
    walls.wallX(0.0f, 0.0f, 3.0f, 0.0f, 4.0f, 1.0f);
    walls.wallZ(0.0f, 0.0f, 4.0f, 0.0f, 3.0f, 1.0f);
    walls.wallZ(4.0f, 0.0f, 4.0f, 0.0f, 3.0f, -1.0f);
    walls.quad(Vec3{0, 3, 0}, Vec3{4, 3, 0}, Vec3{4, 3, 4}, Vec3{0, 3, 4}, Vec3{0.0f, -1.0f, 0.0f});  // techo
    // Pared x = 4 con el hueco (z 1.5..2.5, y 1..2).
    walls.wallX(4.0f, 0.0f, 1.0f, 0.0f, 4.0f, -1.0f);
    walls.wallX(4.0f, 2.0f, 3.0f, 0.0f, 4.0f, -1.0f);
    walls.wallX(4.0f, 1.0f, 2.0f, 0.0f, 1.5f, -1.0f);
    walls.wallX(4.0f, 1.0f, 2.0f, 2.5f, 4.0f, -1.0f);
    walls.toScene(scene);
    scene.lights.sun.direction = core::normalize(Vec3{-1.0f, -1.0f, 0.0f});
    scene.lights.sun.color = Vec3{1.0f, 1.0f, 1.0f};
    scene.lights.sun.intensity = 3.0f;
    scene.settings.sky_intensity = 0.0f;
    return scene;
}

void testLightmapWindowPatch() {
    std::printf("Lightmap: parche de sol por una ventana\n");
    TestMesh floor;
    floor.floor(0.0f, 4.0f, 0.0f, 4.0f);
    const LightmapBakeResult r = bakeReceivers({floor.model("suelo")}, windowRoom(), testLightmapSettings());
    check(r.ok && r.out.lightmaps.sun_visibility && !r.out.lightmaps.pages.empty(), "horneado del suelo");
    if (!r.ok || r.out.lightmaps.pages.empty()) return;
    const auto vis = [&](float x) { return sampleLightmapAt(r, 0, Vec3{x, 0.0f, 2.0f}).w; };
    std::printf("  (vis 1.7: %.3f, 2.5: %.3f, 3.3: %.3f)\n", static_cast<double>(vis(1.7f)), static_cast<double>(vis(2.5f)),
                static_cast<double>(vis(3.3f)));
    check(vis(2.5f) > 0.98f, "(a) el centro del parche al sol");
    check(vis(1.7f) < 0.02f && vis(3.3f) < 0.02f, "(b) fuera del parche, sombra");
    // (c) Bordes: del 10% al 90% en menos de 2 texeles (10 cm), centrados en x = 2 y x = 3.
    const auto edge = [&](float from, float to, float& center) {
        float x10 = -1.0f, x90 = -1.0f;
        const float step = 0.002f * (to > from ? 1.0f : -1.0f);
        for (float x = from; (to > from) ? x <= to : x >= to; x += step) {
            const float v = vis(x);
            if (x10 < 0.0f && v >= 0.1f) x10 = x;
            if (x90 < 0.0f && v >= 0.9f) x90 = x;
        }
        center = 0.5f * (x10 + x90);
        return std::fabs(x90 - x10);
    };
    float c2 = 0.0f, c3 = 0.0f;
    const float w2 = edge(1.8f, 2.4f, c2);
    const float w3 = edge(3.2f, 2.6f, c3);
    std::printf("  (borde x=2: ancho %.3f m en %.3f; borde x=3: ancho %.3f m en %.3f)\n", static_cast<double>(w2),
                static_cast<double>(c2), static_cast<double>(w3), static_cast<double>(c3));
    check(w2 < 0.1f && w3 < 0.1f, "(c) la transicion mide menos de 2 texeles");
    check(std::fabs(c2 - 2.0f) <= 0.05f && std::fabs(c3 - 3.0f) <= 0.05f, "(c) centrada a +-1 texel del borde analitico");
}

void testLightmapThinWall() {
    std::printf("Lightmap: pared fina\n");
    lighting::BakeScene scene;
    TestMesh wall;
    wall.box(Vec3{-0.01f, 0.0f, -1.5f}, Vec3{0.01f, 2.0f, 1.5f});
    wall.toScene(scene);
    // Sol rasante (30 grados) desde -x.
    scene.lights.sun.direction = core::normalize(Vec3{std::cos(0.5236f), -std::sin(0.5236f), 0.0f});
    scene.lights.sun.color = Vec3{1.0f, 1.0f, 1.0f};
    scene.lights.sun.intensity = 3.0f;
    scene.settings.sky_intensity = 0.0f;
    TestMesh floor;
    floor.floor(-1.0f, 1.0f, -1.0f, 1.0f);
    const LightmapBakeResult r = bakeReceivers({floor.model("suelo")}, scene, testLightmapSettings());
    const float lit = sampleLightmapAt(r, 0, Vec3{-0.1f, 0.0f, 0.0f}).w;
    const float shadow = sampleLightmapAt(r, 0, Vec3{0.1f, 0.0f, 0.0f}).w;
    std::printf("  (x=-0.1: %.3f, x=+0.1: %.3f)\n", static_cast<double>(lit), static_cast<double>(shadow));
    check(r.ok && lit > 0.95f, "del lado del sol, luz");
    check(r.ok && shadow >= 0.0f && shadow < 0.05f, "detras de la pared de 2 cm, sombra (no se mezclan las caras)");
}

void testLightmapValidity() {
    std::printf("Lightmap: texeles dentro de la geometria\n");
    lighting::BakeScene scene;
    TestMesh box;
    box.box(Vec3{-0.3f, 0.0f, -0.3f}, Vec3{0.3f, 0.6f, 0.3f});
    box.toScene(scene);
    scene.lights.sun.direction = Vec3{0.0f, -1.0f, 0.0f};
    scene.lights.sun.color = Vec3{1.0f, 1.0f, 1.0f};
    scene.lights.sun.intensity = 3.0f;
    scene.settings.sky_intensity = 0.0f;
    TestMesh floor;
    floor.floor(-1.5f, 1.5f, -1.5f, 1.5f);
    const LightmapBakeResult r = bakeReceivers({floor.model("suelo")}, scene, testLightmapSettings());
    const float under = sampleLightmapAt(r, 0, Vec3{0.0f, 0.0f, 0.0f}).w;
    const float near_edge = sampleLightmapAt(r, 0, Vec3{0.3f + 1.5f * 0.05f, 0.0f, 0.0f}).w;
    std::printf("  (bajo la caja %.3f, a 1.5 texeles %.3f, %zu no validos)\n", static_cast<double>(under),
                static_cast<double>(near_edge), r.stats.invalid_texels);
    check(r.ok && under >= 0.0f && under < 0.05f, "bajo la caja: no valido o sin sol");
    check(r.ok && near_edge > 0.9f, "a 1.5 texeles del borde, sin halo oscuro");
}

void testLightmapDeterminism() {
    std::printf("Lightmap determinista\n");
    TestMesh floor;
    floor.floor(0.0f, 4.0f, 0.0f, 4.0f);
    lighting::LightmapBakeSettings one = testLightmapSettings(1);
    lighting::LightmapBakeSettings four = testLightmapSettings(4);
    // Con la luz rebotada tambien (Fase 3).
    for (lighting::LightmapBakeSettings* s : {&one, &four}) {
        s->indirect_samples = 32;
        s->bounces = 2;
        s->denoise = true;
    }
    lighting::BakeScene scene = windowRoom();
    scene.settings.sky_intensity = 1.0f;
    const LightmapBakeResult a = bakeReceivers({floor.model("suelo")}, scene, one);
    const LightmapBakeResult b = bakeReceivers({floor.model("suelo")}, scene, four);
    bool same = a.ok && b.ok && a.out.lightmaps.pages.size() == b.out.lightmaps.pages.size() && !a.out.lightmaps.pages.empty();
    for (std::size_t p = 0; same && p < a.out.lightmaps.pages.size(); ++p) {
        same = a.out.lightmaps.pages[p].texels == b.out.lightmaps.pages[p].texels;
    }
    check(same, "1 y 4 hilos dan las mismas paginas byte a byte");
}

void testCrbakeV4() {
    std::printf(".crbake v4\n");
    gfx::BakedLighting data;
    data.version = 4;
    data.sun_in_probes = false;
    data.volumes.push_back(gfx::BakedProbeVolume{Vec3{1, 2, 3}, Vec3{4, 5, 6}, 2, 2, 2, 0, 1.5f});
    data.probes.assign(8 * gfx::kBakedProbeVec4, core::Vec4{0.25f, 0.5f, 0.75f, 1.0f});
    data.sun_baked = true;
    data.sun_direction = Vec3{0.0f, -1.0f, 0.0f};
    data.baked_lights.push_back({0xAAull, 0xBBull});
    data.world_origin = {1024.0, 0.0, -2048.0};
    data.scene_uuid = {0x5ull, 0x6ull};
    gfx::LightmapSet& lm = data.lightmaps;
    lm.width = 256;
    lm.height = 256;
    lm.texels_per_meter = 20.0f;
    lm.sun_visibility = true;
    lm.bounds_min = Vec3{-1, -2, -3};
    lm.bounds_max = Vec3{4, 5, 6};
    gfx::LightmapPage page;
    page.width = 256;
    page.height = 256;
    page.texels.resize(256 * 256 * 4);
    for (std::size_t i = 0; i < page.texels.size(); ++i) page.texels[i] = static_cast<std::uint16_t>(i * 2654435761u >> 16);
    lm.pages.push_back(page);
    gfx::LightmapRecord record;
    record.entity = {1, 2};
    record.model = {3, 4};
    record.part = 2;
    record.page = 0;
    record.scale_offset = core::Vec4{0.5f, 0.25f, 0.125f, 0.0625f};
    record.layout_hash = 0x123456789ull;
    record.world[9] = 7.0f;
    lm.records.push_back(record);
    auto layout = std::make_shared<lighting::LightmapUvLayout>();
    layout->source_vertices = 3;
    layout->source_indices = 3;
    layout->duplicates = {1};
    layout->patch_corner = {2};
    layout->patch_vertex = {3};
    layout->uv = {0x00010002u, 0x00030004u, lighting::kLightmapUvNone, 0x7fff7fffu};
    layout->layout_hash = 0x99ull;
    lighting::BakedLightingExtras extras;
    extras.engine_version = "prueba";
    extras.layouts.push_back(lighting::BakedLightingExtras::Layout{Uuid{3, 4}, 2, layout});
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "cramion_v4_test.crbake";
    std::string error;
    check(lighting::saveBakedLighting(file, data, gfx::LightingMode::Baked, &error, &extras), "se guarda");
    gfx::BakedLighting back;
    gfx::LightingMode mode = gfx::LightingMode::Realtime;
    lighting::LightmapUvSet uvs;
    const bool loaded = lighting::loadBakedLighting(file, back, mode, &error, &uvs);
    check(loaded && back.version == 4 && !back.sun_in_probes && mode == gfx::LightingMode::Baked, "se lee como v4");
    check(back.probes.size() == data.probes.size() && back.volumes.size() == 1 && back.volumes[0].intensity == 1.5f &&
              back.baked_lights.size() == 1 && back.world_origin[2] == -2048.0 && back.scene_uuid[1] == 0x6ull,
          "sondas, volumenes, luces, origen y escena");
    check(back.lightmaps.pages.size() == 1 && back.lightmaps.pages[0].texels == page.texels && back.lightmaps.sun_visibility &&
              back.lightmaps.records.size() == 1 && back.lightmaps.records[0].layout_hash == record.layout_hash &&
              back.lightmaps.records[0].scale_offset.w == 0.0625f && back.lightmaps.records[0].world[9] == 7.0f,
          "paginas y registros iguales");
    const auto got = uvs.load(Uuid{3, 4}, 2);
    check(got && lighting::encodeLightmapUvLayout(*got) == lighting::encodeLightmapUvLayout(*layout),
          "LightmapUvSet::load devuelve la disposicion guardada (sin leerla al abrir)");
    std::vector<gfx::LightmapRecord> records;
    check(lighting::loadLightmapRecords(file, records) && records.size() == 1 && records[0].entity[1] == 2,
          "solo los registros (lote estatico al exportar)");
    check(lighting::setBakedLightingMode(file, gfx::LightingMode::Realtime) && lighting::loadBakedLighting(file, back, mode) &&
              mode == gfx::LightingMode::Realtime && !back.lightmaps.pages.empty() && back.lightmaps.pages[0].texels == page.texels,
          "setBakedLightingMode cambia el modo sin tocar las paginas");
    // Una version 5 se rechaza.
    {
        std::fstream io(file, std::ios::binary | std::ios::in | std::ios::out);
        const std::uint32_t five = 5;
        io.seekp(4);
        io.write(reinterpret_cast<const char*>(&five), sizeof(five));
    }
    check(!lighting::loadBakedLighting(file, back, mode, &error), "una version 5 se rechaza");
    // Un v3 escrito byte a byte se sigue leyendo (con el sol en las sondas).
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        const char magic[4] = {'C', 'R', 'B', 'K'};
        out.write(magic, 4);
        const std::uint32_t header[3] = {3, 1, 1};
        out.write(reinterpret_cast<const char*>(header), sizeof(header));
        const float f[7] = {0, 0, 0, 2, 2, 2, 1};
        const std::uint32_t u[4] = {2, 2, 2, 0};
        out.write(reinterpret_cast<const char*>(f), sizeof(f));
        out.write(reinterpret_cast<const char*>(u), sizeof(u));
        const std::uint64_t count = 8 * gfx::kBakedProbeVec4;
        out.write(reinterpret_cast<const char*>(&count), sizeof(count));
        const std::vector<core::Vec4> probes(count, core::Vec4{0.1f, 0.2f, 0.3f, 1.0f});
        out.write(reinterpret_cast<const char*>(probes.data()), static_cast<std::streamsize>(count * sizeof(core::Vec4)));
        const float cell = 0.5f;
        const std::uint32_t surface[3] = {0, 0, 0};
        out.write(reinterpret_cast<const char*>(&cell), sizeof(cell));
        out.write(reinterpret_cast<const char*>(surface), sizeof(surface));
        const std::uint32_t lights[2] = {1, 0};
        const float dir[3] = {0.0f, -1.0f, 0.0f};
        out.write(reinterpret_cast<const char*>(lights), sizeof(lights));
        out.write(reinterpret_cast<const char*>(dir), sizeof(dir));
    }
    gfx::BakedLighting old;
    check(lighting::loadBakedLighting(file, old, mode, &error) && old.version == 3 && old.sun_in_probes && old.sun_baked &&
              old.probes.size() == 8 * gfx::kBakedProbeVec4 && old.lightmaps.empty(),
          "un .crbake v3 se sigue leyendo (sol en las sondas)");
    std::error_code ec;
    std::filesystem::remove(file, ec);
}

void testStaticBatchKeepOut() {
    std::printf("Lote estatico sin lo que lleva lightmap\n");
    physics::registerPhysicsComponents();
    std::error_code ec;
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "CramionKeepOutTest";
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / "Assets", ec);
    assets::AssetDatabase database;
    database.open(root / "Assets");
    ecs::World world;
    const auto cube = [&](const char* name, const Vec3& position) {
        ecs::Entity e = world.create(name);
        e.setLocalPosition(position);
        e.get<ecs::EntityInfo>().is_static = true;
        e.add<ecs::MeshRenderer>().model = assets::AssetRef{assets::builtin::kCube, assets::AssetType::Model};
        return e;
    };
    ecs::Entity a = cube("Con lightmap", Vec3{0.0f, 0.0f, 0.0f});
    ecs::Entity b = cube("Sin lightmap", Vec3{3.0f, 0.0f, 0.0f});
    ecs::Entity c = cube("Otro", Vec3{6.0f, 0.0f, 0.0f});
    ecs::StaticBatchOptions options;
    const Uuid kept = a.uuid();
    options.keep_out = [kept](const Uuid& entity) { return entity == kept; };
    ecs::StaticBatchReport report;
    const bool ok = ecs::buildStaticBatch(world, database, root / "Assets" / "lote.crdata", options, report);
    std::printf("  %s\n", report.message.c_str());
    check(ok && report.combined == 2 && report.kept_lightmapped == 1, "un lote con dos; el del lightmap se queda");
    check(!a.get<ecs::EntityInfo>().static_batched && b.get<ecs::EntityInfo>().static_batched &&
              c.get<ecs::EntityInfo>().static_batched,
          "solo se marcan los del lote");
    std::filesystem::remove_all(root, ec);
}

// --- Fase 3 ---------------------------------------------------------------------

void testLightmapSkyEnergy() {
    std::printf("Lightmap: energia del cielo\n");
    lighting::BakeScene scene;
    scene.lights.sun.intensity = 0.0f;
    scene.sky_sh.fill(Vec3{});
    scene.sky_sh[0] = Vec3{3.5449f, 3.5449f, 3.5449f};
    scene.has_sky_sh = true;
    TestMesh plane;
    plane.floor(-5.0f, 5.0f, -5.0f, 5.0f);
    lighting::LightmapBakeSettings s = testLightmapSettings();
    s.indirect_samples = 256;
    s.bounces = 1;
    const LightmapBakeResult r = bakeReceivers({plane.model("plano")}, scene, s);
    const core::Vec4 e = sampleLightmapAt(r, 0, Vec3{0.0f, 0.0f, 0.0f});
    const float expected = lighting::irradianceFromSh(scene.sky_sh, Vec3{0.0f, 1.0f, 0.0f}).x;
    std::printf("  (irradiancia %.4f, la del render %.4f)\n", static_cast<double>(e.x), static_cast<double>(expected));
    check(r.ok && r.out.lightmaps.has_irradiance && std::fabs(e.x - expected) < 0.02f * expected,
          "un plano a la intemperie recibe la irradiancia del cielo del render (+-2%)");
}

void testLightmapPointLightParity() {
    std::printf("Lightmap: luz puntual horneada = tiempo real\n");
    lighting::BakeScene scene;
    scene.lights.sun.intensity = 0.0f;
    scene.settings.sky_intensity = 0.0f;
    scene::PointLight light{};
    light.position = Vec3{0.0f, 2.0f, 0.0f};
    light.color = Vec3{1.0f, 1.0f, 1.0f};
    light.intensity = 10.0f;
    light.range = 10.0f;
    light.cast_shadows = true;
    light.shadow_strength = 1.0f;
    light.source_radius = 0.0f;
    scene.lights.points.push_back(light);
    scene.point_baked.push_back(1);
    TestMesh floor;
    floor.floor(-4.0f, 4.0f, -4.0f, 4.0f);
    lighting::LightmapBakeSettings s = testLightmapSettings();
    s.direct_samples = 4;
    const LightmapBakeResult r = bakeReceivers({floor.model("suelo")}, scene, s);
    bool ok = r.ok && r.out.lightmaps.has_irradiance;
    for (const float x : {0.0f, 1.0f, 2.0f}) {
        const float d = std::sqrt(x * x + 4.0f);
        const float expected = 10.0f * lighting::lightAttenuation(d, 10.0f) * (2.0f / d);
        const float got = sampleLightmapAt(r, 0, Vec3{x, 0.0f, 0.0f}).x;
        std::printf("  (x = %.0f: %.4f, lighting.frag %.4f)\n", static_cast<double>(x), static_cast<double>(got),
                    static_cast<double>(expected));
        ok = ok && std::fabs(got - expected) < 0.03f * expected;
    }
    check(ok, "E(x) = I * atenuacion * cos de lighting.frag (+-3%)");
}

void testLightmapBounce() {
    std::printf("Lightmap: luz rebotada\n");
    lighting::BakeScene scene;
    scene.lights.sun.direction = Vec3{0.0f, -1.0f, 0.0f};
    scene.lights.sun.color = Vec3{1.0f, 1.0f, 1.0f};
    scene.lights.sun.intensity = 3.0f;
    scene.settings.sky_intensity = 0.0f;
    lighting::BakeMaterial white;
    white.albedo = Vec3{0.8f, 0.8f, 0.8f};
    white.resolved = true;
    scene.materials.push_back(white);
    TestMesh floor;
    floor.floor(0.0f, 4.0f, -2.0f, 2.0f);
    TestMesh wall;  // en x = 0 mirando a +x: el sol cenital no le da
    wall.wallX(0.0f, 0.0f, 3.0f, -2.0f, 2.0f, 1.0f);
    const auto average = [&](int bounces) {
        lighting::LightmapBakeSettings s = testLightmapSettings();
        s.indirect_samples = 1024;
        s.bounces = bounces;
        const LightmapBakeResult r = bakeReceivers({floor.model("suelo"), wall.model("pared")}, scene, s, {0, 0});
        float sum = 0.0f;
        int n = 0;
        for (int y = 0; y < 5; ++y) {
            for (int z = 0; z < 5; ++z) {
                const float e = sampleLightmapAt(r, 1, Vec3{0.0f, 0.5f + 0.4f * static_cast<float>(y), -1.0f + 0.5f * static_cast<float>(z)}).x;
                if (e >= 0.0f) {
                    sum += e;
                    ++n;
                }
            }
        }
        return n > 0 ? sum / static_cast<float>(n) : -1.0f;
    };
    const float one = average(1);
    const float two = average(2);
    std::printf("  (pared: 1 rebote %.3f, 2 rebotes %.3f; cota %.2f)\n", static_cast<double>(one), static_cast<double>(two),
                0.8 * 3.0);
    check(one > 0.0f && one < 0.8f * 3.0f, "la pared en sombra recibe rebote, por debajo de la cota fisica");
    check(two > one, "con un rebote mas, aumenta");
}

void testLightmapDenoise() {
    std::printf("Lightmap: filtro de la luz rebotada\n");
    lighting::lm::Context ctx;
    ctx.threads = 2;
    lighting::lm::Page page;
    page.width = 64;
    page.height = 64;
    const std::size_t pixels = 64 * 64;
    page.texel_of.resize(pixels);
    page.state.assign(pixels, 1);
    page.value.assign(pixels, core::Vec4{});
    std::vector<Vec3> irradiance(pixels);
    std::vector<float> variance(pixels, 0.03f);
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> noise(-0.3f, 0.3f);
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * 64 + static_cast<std::size_t>(x);
            lighting::lm::Texel t;
            const bool wall = x >= 32;  // arista: suelo a la izquierda, pared a la derecha
            t.center = wall ? Vec3{1.6f, static_cast<float>(x - 32) * 0.05f, static_cast<float>(y) * 0.05f}
                            : Vec3{static_cast<float>(x) * 0.05f, 0.0f, static_cast<float>(y) * 0.05f};
            t.normal = wall ? Vec3{-1.0f, 0.0f, 0.0f} : Vec3{0.0f, 1.0f, 0.0f};
            t.geo_normal = t.normal;
            t.side = 0.05f;
            t.instance = 0;
            page.texel_of[i] = static_cast<std::uint32_t>(page.texels.size());
            page.texels.push_back(t);
            const float base = wall ? 0.3f : 1.0f;
            irradiance[i] = Vec3{base + noise(rng) * base, base, base};
        }
    }
    const auto stats = [&](const std::vector<Vec3>& image, int x0, int x1, float& mean) {
        double sum = 0.0, sum2 = 0.0;
        int n = 0;
        for (int y = 8; y < 56; ++y) {
            for (int x = x0; x < x1; ++x) {
                const double v = image[static_cast<std::size_t>(y) * 64 + static_cast<std::size_t>(x)].x;
                sum += v;
                sum2 += v * v;
                ++n;
            }
        }
        mean = static_cast<float>(sum / n);
        return static_cast<float>(std::sqrt(std::max(0.0, sum2 / n - (sum / n) * (sum / n))));
    };
    float mean_floor = 0.0f, mean_wall = 0.0f;
    const float before = stats(irradiance, 4, 28, mean_floor);
    stats(irradiance, 36, 60, mean_wall);
    const float before_gap = mean_floor - mean_wall;
    lighting::lm::denoisePage(ctx, page, irradiance, variance, 4);
    const float after = stats(irradiance, 4, 28, mean_floor);
    stats(irradiance, 36, 60, mean_wall);
    const float after_gap = mean_floor - mean_wall;
    std::printf("  (desviacion %.3f -> %.3f; diferencia entre lados %.3f -> %.3f)\n", static_cast<double>(before),
                static_cast<double>(after), static_cast<double>(before_gap), static_cast<double>(after_gap));
    check(after < 0.3f * before, "en lo plano el ruido baja a menos del 30%");
    check(after_gap >= 0.8f * before_gap, "la arista suelo-pared se conserva (>= 80%)");
}

// --- Fase 4 ---------------------------------------------------------------------

void testStaticSunFit() {
    std::printf("Mapa de sombra estatico del sol horneado\n");
    scene::ShadowCascades cascades;
    const core::Aabb box{Vec3{-10.0f, 0.0f, -6.0f}, Vec3{14.0f, 8.0f, 6.0f}};
    scene::DirectionalLight sun;
    sun.direction = Vec3{0.0f, -1.0f, 0.0f};
    constexpr std::uint32_t kResolution = 2048;
    cascades.fitStatic(box, sun, kResolution);
    const scene::ShadowCascade& c = cascades.cascade(0);
    bool inside = true;
    for (int i = 0; i < 8; ++i) {
        const core::Vec4 p = c.light_view_projection * core::Vec4{(i & 1) ? box.max.x : box.min.x, (i & 2) ? box.max.y : box.min.y,
                                                                  (i & 4) ? box.max.z : box.min.z, 1.0f};
        inside = inside && std::fabs(p.x / p.w) <= 1.0001f && std::fabs(p.y / p.w) <= 1.0001f && p.z / p.w >= 0.0f &&
                 p.z / p.w <= 1.0f;
    }
    const float side = c.texel_world_size * static_cast<float>(kResolution);
    std::printf("  (lado %.3f m, texel %.4f m)\n", static_cast<double>(side), static_cast<double>(c.texel_world_size));
    check(inside, "el encuadre contiene los 8 vertices de la caja");
    check(std::fabs(side - 24.0f) <= c.texel_world_size * 1.01f, "con el sol cenital el lado es el de la caja (+-1 texel)");
    check(cascades.cascade(3).split_distance > 1e6f && cascades.cascade(3).texel_world_size == c.texel_world_size,
          "las cuatro cascadas con el mismo encuadre (siempre la 0)");
}

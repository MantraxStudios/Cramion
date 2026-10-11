// Splines y el resto de lo nuevo de la 2.4 desde Lua.
//
//   local p = self.entity:splinePoint(0.5)          -- mitad de la curva (mundo)
//   local d = self.entity:closestSplineDistance(pos)
//   local s = Spline.create({Vec3(0,0,0), Vec3(0,0,20), Vec3(10,0,30)}, "road")
//   seguidor:playSplineFollower()  seguidor.splineDistance = 0
//
// Se incluye al final de Scripting.cpp (necesita ScriptSystem::Impl).

namespace {

spline::SplinePath luaSplinePath(const ecs::Entity& e) {
    spline::SplinePath p;
    if (e.valid() && e.has<spline::Spline>()) p.build(e.get<spline::Spline>(), e.worldMatrix());
    return p;
}

int splineShapeFromName(const std::string& raw) {
    const std::string n = lower(raw);
    if (n == "road" || n == "carretera") return 0;
    if (n == "path" || n == "camino") return 1;
    if (n == "river" || n == "rio") return 2;
    if (n == "wall" || n == "muro") return 3;
    if (n == "fence" || n == "valla") return 4;
    if (n == "pipe" || n == "tuberia") return 5;
    if (n == "rails" || n == "railes") return 6;
    if (n == "ribbon" || n == "cinta") return 7;
    return -1;
}

}  // namespace

void ScriptSystem::Impl::bindFeatures24(sol::state& L, sol::usertype<LuaEntity>& entity) {
    // --- Splines ---
    entity["splinePoint"] = [](const LuaEntity& e, float t) { return luaSplinePath(e.get()).atNormalized(t).position; };
    entity["splinePointAt"] = [](const LuaEntity& e, float d) { return luaSplinePath(e.get()).atDistance(d).position; };
    entity["splineTangent"] = [](const LuaEntity& e, float t) { return luaSplinePath(e.get()).atNormalized(t).tangent; };
    entity["splineRight"] = [](const LuaEntity& e, float t) { return luaSplinePath(e.get()).atNormalized(t).right; };
    entity["splineLength"] = sol::property([](const LuaEntity& e) { return luaSplinePath(e.get()).length(); });
    entity["closestSplineDistance"] = [](const LuaEntity& e, const Vec3& p) {
        return luaSplinePath(e.get()).closestDistance(p);
    };
    entity["closestSplinePoint"] = [](const LuaEntity& e, const Vec3& p) {
        Vec3 c{};
        luaSplinePath(e.get()).closestDistance(p, &c);
        return c;
    };
    entity["splinePointCount"] = sol::property([](const LuaEntity& e) {
        const ecs::Entity x = e.get();
        return x.valid() && x.has<spline::Spline>() ? static_cast<int>(x.get<spline::Spline>().points.size()) : 0;
    });
    // Puntos de control en el mundo.
    entity["getSplineControlPoint"] = [](const LuaEntity& e, int index) -> sol::optional<Vec3> {
        const ecs::Entity x = e.get();
        const spline::Spline* s = x.valid() ? x.tryGet<spline::Spline>() : nullptr;
        if (s == nullptr || index < 1 || index > static_cast<int>(s->points.size())) return sol::nullopt;
        return ecs::transformPoint(x.worldMatrix(), s->points[static_cast<std::size_t>(index - 1)].position);
    };
    entity["setSplineControlPoint"] = [](LuaEntity& e, int index, const Vec3& world_pos) {
        ecs::Entity x = e.get();
        spline::Spline* s = x.valid() ? x.tryGet<spline::Spline>() : nullptr;
        if (s == nullptr || index < 1 || index > static_cast<int>(s->points.size())) return false;
        s->points[static_cast<std::size_t>(index - 1)].position = ecs::transformPoint(core::inverse(x.worldMatrix()), world_pos);
        s->markModified();
        return true;
    };
    entity["addSplinePoint"] = [](LuaEntity& e, const Vec3& world_pos, sol::optional<float> width) {
        ecs::Entity x = e.get();
        if (!x.valid()) return false;
        spline::Spline& s = x.has<spline::Spline>() ? x.get<spline::Spline>() : x.add<spline::Spline>();
        spline::SplinePoint p;
        p.position = ecs::transformPoint(core::inverse(x.worldMatrix()), world_pos);
        p.width = width.value_or(s.points.empty() ? 1.0f : s.points.back().width);
        s.points.push_back(p);
        s.markModified();
        return true;
    };
    entity["clearSplinePoints"] = [](LuaEntity& e) {
        ecs::Entity x = e.get();
        if (spline::Spline* s = x.valid() ? x.tryGet<spline::Spline>() : nullptr) {
            s->points.clear();
            s->markModified();
        }
    };
    // Seguidor.
    const auto follower = [](const LuaEntity& e) -> spline::SplineFollower* {
        ecs::Entity x = e.get();
        return x.valid() ? x.tryGet<spline::SplineFollower>() : nullptr;
    };
    entity["playSplineFollower"] = [follower](LuaEntity& e) {
        if (spline::SplineFollower* f = follower(e)) {
            f->started = true;
            f->playing = true;
        }
    };
    entity["stopSplineFollower"] = [follower](LuaEntity& e) {
        if (spline::SplineFollower* f = follower(e)) {
            f->started = true;
            f->playing = false;
        }
    };
    entity["splineDistance"] = sol::property(
        [follower](const LuaEntity& e) {
            const spline::SplineFollower* f = follower(e);
            return f != nullptr ? f->distance : 0.0f;
        },
        [follower](LuaEntity& e, float d) {
            if (spline::SplineFollower* f = follower(e)) f->distance = d;
        });
    entity["splineSpeed"] = sol::property(
        [follower](const LuaEntity& e) {
            const spline::SplineFollower* f = follower(e);
            return f != nullptr ? f->speed : 0.0f;
        },
        [follower](LuaEntity& e, float v) {
            if (spline::SplineFollower* f = follower(e)) f->speed = v;
        });

    // --- UI: Dropdown, ScrollView, texto enriquecido ---
    const auto dropdown = [](const LuaEntity& e) -> ui::Dropdown* {
        ecs::Entity x = e.get();
        return x.valid() ? x.tryGet<ui::Dropdown>() : nullptr;
    };
    entity["dropdownValue"] = sol::property(
        [dropdown](const LuaEntity& e) {
            const ui::Dropdown* d = dropdown(e);
            return d != nullptr ? d->value + 1 : 0;
        },
        [dropdown](LuaEntity& e, int v) {
            if (ui::Dropdown* d = dropdown(e)) {
                d->value = std::clamp(v - 1, 0, std::max(static_cast<int>(d->options.size()) - 1, 0));
            }
        });
    entity["dropdownText"] = sol::property([dropdown](const LuaEntity& e) {
        const ui::Dropdown* d = dropdown(e);
        if (d == nullptr || d->value < 0 || d->value >= static_cast<int>(d->options.size())) return std::string();
        return d->options[static_cast<std::size_t>(d->value)];
    });
    entity["setDropdownOptions"] = [dropdown](LuaEntity& e, sol::table options) {
        ui::Dropdown* d = dropdown(e);
        if (d == nullptr) return;
        d->options.clear();
        for (std::size_t i = 1; i <= options.size(); ++i) {
            sol::optional<std::string> text = options[i];
            if (text) d->options.push_back(*text);
        }
        d->value = std::clamp(d->value, 0, std::max(static_cast<int>(d->options.size()) - 1, 0));
    };
    entity["getDropdownOptions"] = [this, dropdown](const LuaEntity& e) {
        sol::table t = lua->create_table();
        if (const ui::Dropdown* d = dropdown(e)) {
            for (std::size_t i = 0; i < d->options.size(); ++i) t[i + 1] = d->options[i];
        }
        return t;
    };
    const auto scrollView = [](const LuaEntity& e) -> ui::ScrollView* {
        ecs::Entity x = e.get();
        return x.valid() ? x.tryGet<ui::ScrollView>() : nullptr;
    };
    entity["scrollY"] = sol::property(
        [scrollView](const LuaEntity& e) {
            const ui::ScrollView* s = scrollView(e);
            return s != nullptr ? s->scroll.y : 0.0f;
        },
        [scrollView](LuaEntity& e, float v) {
            if (ui::ScrollView* s = scrollView(e)) {
                s->scroll.y = v;
                s->velocity = core::Vec2{};
            }
        });
    entity["scrollX"] = sol::property(
        [scrollView](const LuaEntity& e) {
            const ui::ScrollView* s = scrollView(e);
            return s != nullptr ? s->scroll.x : 0.0f;
        },
        [scrollView](LuaEntity& e, float v) {
            if (ui::ScrollView* s = scrollView(e)) {
                s->scroll.x = v;
                s->velocity = core::Vec2{};
            }
        });
    // 0..1 del recorrido (0 = arriba, 1 = abajo del todo).
    entity["scrollToFraction"] = [scrollView](LuaEntity& e, float t) {
        if (ui::ScrollView* s = scrollView(e)) {
            s->velocity = core::Vec2{};
            s->scroll.y = std::max(s->content.y, 0.0f) * std::clamp(t, 0.0f, 1.0f);  // se recorta al maquetar
        }
    };
    entity["scrollContentHeight"] = sol::property([scrollView](const LuaEntity& e) {
        const ui::ScrollView* s = scrollView(e);
        return s != nullptr ? s->content.y : 0.0f;
    });
    sol::table text_api = L["Text"];  // la crea bindGameplay (Text.get / Text.language...)
    text_api["strip"] = [](const std::string& rich) { return ui::stripRichText(rich); };

    // --- World Partition ---
    sol::table wp = L.create_named_table("WorldPartition");
    wp["active"] = []() {
        const worldpart::WorldPartitionSystem* p = worldpart::activePartition();
        return p != nullptr && p->active();
    };
    wp["loadAll"] = [this]() {
        if (worldpart::WorldPartitionSystem* p = worldpart::activePartition(); p != nullptr && world != nullptr) p->loadAll(*world);
    };
    wp["isLoaded"] = [](const Vec3& position) {
        const worldpart::WorldPartitionSystem* p = worldpart::activePartition();
        return p == nullptr || !p->active() || p->cellLoaded(p->cellOf(position));
    };
    wp["stats"] = [this]() {
        sol::table t = lua->create_table();
        if (const worldpart::WorldPartitionSystem* p = worldpart::activePartition()) {
            const worldpart::PartitionStats& s = p->stats();
            t["active"] = s.active;
            t["cells"] = s.cells;
            t["loadedCells"] = s.loaded_cells;
            t["objects"] = s.streamed_entities;
            t["unloadedObjects"] = s.unloaded_entities;
            t["storedBytes"] = static_cast<double>(s.stored_bytes);
        }
        return t;
    };

    // --- Netcode: compensacion de lag, estadisticas y simulador ---
    sol::table nw = L["Network"];
    // true en CramionServer (servidor dedicado sin ventana).
    nw["isDedicated"] = [](sol::this_state s) {
        sol::state_view lua_view(s);
        sol::optional<bool> flag = lua_view["Network"]["dedicated"];
        return flag.value_or(false);
    };
    // Donde estaba un objeto de red hace `segundos` (historia de 1,5 s).
    nw["positionAt"] = [this](lua_Integer net_id, float seconds_ago) -> sol::optional<Vec3> {
        if (!network) return sol::nullopt;
        Vec3 p{};
        core::Quat q{};
        if (!network->positionAt(static_cast<std::uint32_t>(net_id), seconds_ago, p, q)) return sol::nullopt;
        return p;
    };
    // Servidor: el disparo de `jugador` contra donde el veia a los demas
    // (su ping / 2 + el retraso de interpolacion). Devuelve id de red, punto
    // y distancia del primero que toca (esferas de `radio` a `altura`).
    nw["lagCompensatedRaycast"] = [this](const Vec3& origin, const Vec3& direction, float max_distance,
                                         lua_Integer shooter, sol::optional<float> radius,
                                         sol::optional<float> height) -> sol::object {
        if (!network) return sol::lua_nil;
        const Vec3 dir = core::normalize(direction);
        const float r = radius.value_or(0.5f);
        const float up = height.value_or(1.0f);
        const float ping_s = static_cast<float>(network->ping(static_cast<std::uint32_t>(shooter))) / 2000.0f;
        std::uint32_t best_id = 0;
        float best_t = max_distance;
        for (const auto& [id, obj] : network->objects()) {
            if (obj.owner == static_cast<std::uint32_t>(shooter)) continue;
            float delay = 0.1f;
            if (const ecs::Entity e = netEntity(id); e.valid()) {
                if (const net::NetworkObject* n = e.tryGet<net::NetworkObject>()) delay = n->interpolation_delay;
            }
            Vec3 p{};
            core::Quat q{};
            if (!network->positionAt(id, ping_s + delay, p, q)) continue;
            const Vec3 c = p + Vec3{0.0f, up, 0.0f};
            const Vec3 oc = origin - c;
            const float b = core::dot(oc, dir);
            const float cc = core::dot(oc, oc) - r * r;
            const float disc = b * b - cc;
            if (disc < 0.0f) continue;
            const float t = -b - std::sqrt(disc);
            if (t >= 0.0f && t < best_t) {
                best_t = t;
                best_id = id;
            }
        }
        if (best_id == 0) return sol::lua_nil;
        sol::table hit = lua->create_table();
        hit["netId"] = static_cast<lua_Integer>(best_id);
        hit["point"] = origin + dir * best_t;
        hit["distance"] = best_t;
        return hit;
    };
    nw["stats"] = [this]() {
        sol::table t = lua->create_table();
        if (!network) return t;
        t["sendRate"] = network->sendRate();
        t["receiveRate"] = network->receiveRate();
        t["bytesSent"] = static_cast<double>(network->bytesSent());
        t["bytesReceived"] = static_cast<double>(network->bytesReceived());
        t["objects"] = static_cast<int>(network->objects().size());
        const std::uint32_t other = network->isClient() ? net::kServerId : 0;
        if (other != 0) {
            t["ping"] = network->ping(other);
            t["packetLoss"] = network->packetLoss(other);
        }
        return t;
    };
    // Network.simulate{latency = 120, jitter = 30, loss = 5}  (todo a 0 = red normal)
    nw["simulate"] = [](sol::table o) {
        cvar::Registry& reg = cvar::Registry::instance();
        reg.execute("net.SimLatencyMs " + std::to_string(o.get_or("latency", 0)));
        reg.execute("net.SimJitterMs " + std::to_string(o.get_or("jitter", 0)));
        reg.execute("net.SimLossPercent " + std::to_string(o.get_or("loss", 0)));
    };

    // --- Accesibilidad ---
    sol::table acc = L.create_named_table("Accessibility");
    acc["get"] = [this]() {
        const gameplay::AccessibilitySettings& a = gameplay::accessibility();
        sol::table t = lua->create_table();
        t["colorblind"] = a.colorblind_mode;
        t["colorblindStrength"] = a.colorblind_strength;
        t["colorblindCorrect"] = a.colorblind_correct;
        t["textScale"] = a.text_scale;
        t["subtitleScale"] = a.subtitle_scale;
        t["subtitleBackground"] = a.subtitle_background;
        t["reduceMotion"] = a.reduce_motion;
        t["cameraShake"] = a.camera_shake;
        t["highContrast"] = a.high_contrast_ui;
        return t;
    };
    // Accessibility.set{ colorblind = 2, textScale = 1.25, reduceMotion = true }
    acc["set"] = [](sol::table t) {
        gameplay::AccessibilitySettings& a = gameplay::accessibility();
        a.colorblind_mode = std::clamp(t.get_or("colorblind", a.colorblind_mode), 0, 4);
        a.colorblind_strength = std::clamp(t.get_or("colorblindStrength", a.colorblind_strength), 0.0f, 1.0f);
        a.colorblind_correct = t.get_or("colorblindCorrect", a.colorblind_correct);
        a.text_scale = std::clamp(t.get_or("textScale", a.text_scale), 0.5f, 3.0f);
        a.subtitle_scale = std::clamp(t.get_or("subtitleScale", a.subtitle_scale), 0.5f, 3.0f);
        a.subtitle_background = t.get_or("subtitleBackground", a.subtitle_background);
        a.reduce_motion = t.get_or("reduceMotion", a.reduce_motion);
        a.camera_shake = std::clamp(t.get_or("cameraShake", a.camera_shake), 0.0f, 2.0f);
        a.high_contrast_ui = t.get_or("highContrast", a.high_contrast_ui);
    };
    acc["save"] = []() { return gameplay::saveAccessibility(); };
    acc["load"] = []() { return gameplay::loadAccessibility(); };
    acc["reset"] = []() { gameplay::accessibility() = gameplay::AccessibilitySettings{}; };
    acc["colorblindName"] = [](int mode) { return std::string(gameplay::colorblindModeName(mode)); };

    sol::table cam = L.create_named_table("Camera");
    // Camera.shake(intensidad 0..1, duracion s, frecuencia Hz)
    cam["shake"] = [](float intensity, sol::optional<float> duration, sol::optional<float> frequency) {
        gameplay::addCameraShake(intensity, duration.value_or(0.5f), frequency.value_or(18.0f));
    };
    cam["stopShake"] = []() { gameplay::clearCameraShake(); };

    // --- Chat de voz ---
    sol::table vc = L.create_named_table("Voice");
    const auto voice = []() -> audio::VoiceChat* {
        if (audio::activeVoiceChat() == nullptr) {
            static audio::VoiceChat shared;  // la crea el primer uso
            audio::setActiveVoiceChat(&shared);
        }
        return audio::activeVoiceChat();
    };
    vc["start"] = [voice]() { return voice()->start(); };
    vc["stop"] = [voice]() { voice()->stop(); };
    vc["setMode"] = [voice](const std::string& mode) {
        const std::string m = lower(mode);
        voice()->setMode(m == "open" || m == "voz" ? audio::VoiceMode::Open
                         : m == "off"              ? audio::VoiceMode::Off
                                                   : audio::VoiceMode::PushToTalk);
    };
    vc["setTalking"] = [voice](bool talking) { voice()->setTalking(talking); };
    vc["setThreshold"] = [voice](float rms) { voice()->setVoiceThreshold(rms); };
    vc["setVolume"] = [voice](float v) { voice()->setVolume(v); };
    vc["setMicGain"] = [voice](float g) { voice()->setMicGain(g); };
    vc["setProximity"] = [voice](float meters) { voice()->setProximity(meters); };
    vc["setMuted"] = [voice](lua_Integer player, bool muted) { voice()->setMuted(static_cast<std::uint32_t>(player), muted); };
    vc["isMuted"] = [voice](lua_Integer player) { return voice()->muted(static_cast<std::uint32_t>(player)); };
    vc["isSpeaking"] = [voice, this](sol::optional<lua_Integer> player) {
        if (!player || (network && static_cast<std::uint32_t>(*player) == network->localId())) return voice()->localSpeaking();
        return voice()->isSpeaking(static_cast<std::uint32_t>(*player));
    };
    vc["micLevel"] = [voice]() { return voice()->micLevel(); };

    // --- Multitudes ---
    entity["spawnCrowd"] = [this](LuaEntity& e) {
        ai::CrowdSystem* c = ai::activeCrowds();
        return c != nullptr && world != nullptr ? c->spawn(*world, e.get()) : 0;
    };
    entity["despawnCrowd"] = [this](LuaEntity& e) {
        if (ai::CrowdSystem* c = ai::activeCrowds(); c != nullptr && world != nullptr) c->despawn(*world, e.get());
    };
    sol::table cr = L.create_named_table("Crowd");
    cr["stats"] = [this]() {
        sol::table t = lua->create_table();
        if (const ai::CrowdSystem* c = ai::activeCrowds()) {
            t["agents"] = c->stats().agents;
            t["visible"] = c->stats().visible;
            t["spawners"] = c->stats().spawners;
        }
        return t;
    };

    // --- Mods ---
    sol::table md = L.create_named_table("Mods");
    md["enabled"] = []() { return project::activeMods() != nullptr; };
    md["list"] = [this]() {
        sol::table t = lua->create_table();
        if (const project::ModManager* m = project::activeMods()) {
            int i = 1;
            for (const project::ModInfo& info : m->mods()) {
                sol::table row = lua->create_table();
                row["id"] = info.id;
                row["name"] = info.name;
                row["version"] = info.version;
                row["author"] = info.author;
                row["description"] = info.description;
                row["enabled"] = info.enabled;
                bool loaded = false;
                for (const project::ModMount& mm : m->mounted()) loaded = loaded || mm.id == info.id;
                row["loaded"] = loaded;
                t[i++] = row;
            }
        }
        return t;
    };
    // Se aplica al volver a abrir el juego.
    md["setEnabled"] = [](const std::string& id, bool enabled) {
        project::ModManager* m = project::activeMods();
        if (m == nullptr || !m->setEnabled(id, enabled)) return false;
        return m->saveState();
    };
    md["isLoaded"] = [](const std::string& id) {
        const project::ModManager* m = project::activeMods();
        if (m == nullptr) return false;
        for (const project::ModMount& mm : m->mounted()) {
            if (mm.id == id) return true;
        }
        return false;
    };

    // --- Job system ---
    sol::table jb = L.create_named_table("Jobs");
    jb["workers"] = []() { return jobs::workerCount(); };
    jb["executed"] = []() { return static_cast<double>(jobs::stats().executed); };

    sol::table sp = L.create_named_table("Spline");
    // Spline.create(puntos, forma, nombre, cerrada) -> entidad
    sp["create"] = [this](sol::table points, sol::optional<std::string> shape, sol::optional<std::string> name,
                          sol::optional<bool> closed) -> sol::object {
        if (world == nullptr) return sol::lua_nil;
        std::vector<Vec3> pts;
        for (std::size_t i = 1; i <= points.size(); ++i) {
            sol::optional<Vec3> v = points[i];
            if (v) pts.push_back(*v);
        }
        const int s = splineShapeFromName(shape.value_or(""));
        ecs::Entity e = spline::createSplineEntity(*world, pts, s, name.value_or("Spline"), closed.value_or(false));
        return sol::make_object(*lua, LuaEntity{e.handle(), world});
    };
}

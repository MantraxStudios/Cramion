// VFX Graph, 2D, destruccion, repeticiones, Motion Matching y vehiculos desde Lua.
//
//   self.entity:playEffect()                    -- VisualEffect
//   self.entity:sendEffectEvent("Explode")
//   self.entity:setEffectFloat("Rate", 200)
//
//   self.entity.velocity2D = Vec3(3, 0, 0)       -- Rigidbody2D
//   local hit = Physics2D.raycast(self.entity.position, Vec3(0, -1, 0), 1.2)
//   self.entity:playSpriteAnimation("Correr")
//   function OnCollisionEnter2D(self, otro, contacto) ... end
//
//   self.entity:fracture(punto, 6)              -- Destructible
//   function OnBreak(self, trozos) ... end
//
//   Replay.start()  Replay.play(-5)  Replay.save("gol")
//   self.entity:setMotionVelocity(dir * 4)      -- Motion Matching (modo Script)
//
// Se incluye al final de Scripting.cpp (necesita ScriptSystem::Impl).

namespace {

core::Vec2 xy(const Vec3& v) { return core::Vec2{v.x, v.y}; }

twod::ForceMode2D forceMode2D(const sol::optional<std::string>& mode) {
    const std::string name = lower(mode.value_or("force"));
    if (name == "impulse") return twod::ForceMode2D::Impulse;
    if (name == "velocity" || name == "velocitychange") return twod::ForceMode2D::VelocityChange;
    return twod::ForceMode2D::Force;
}

}  // namespace

void ScriptSystem::Impl::bindEffects(sol::state& L, sol::usertype<LuaEntity>& entity) {
    // --- VFX Graph (componente VisualEffect) ---
    entity["playEffect"] = [](LuaEntity& e) {
        if (vfx::VfxSystem* s = vfx::activeSystem()) s->play(e.get());
    };
    entity["stopEffect"] = [](LuaEntity& e, sol::optional<bool> clear) {
        if (vfx::VfxSystem* s = vfx::activeSystem()) s->stop(e.get(), clear.value_or(false));
    };
    entity["pauseEffect"] = [](LuaEntity& e, sol::optional<bool> paused) {
        if (vfx::VfxSystem* s = vfx::activeSystem()) s->pause(e.get(), paused.value_or(true));
    };
    entity["isEffectPlaying"] = [](const LuaEntity& e) {
        const vfx::VfxSystem* s = vfx::activeSystem();
        return s != nullptr && s->isPlaying(e.get());
    };
    entity["sendEffectEvent"] = [](LuaEntity& e, const std::string& name) {
        if (vfx::VfxSystem* s = vfx::activeSystem()) s->sendEvent(e.get(), name);
    };
    entity["setEffectFloat"] = [](LuaEntity& e, const std::string& name, float value) {
        if (vfx::VfxSystem* s = vfx::activeSystem()) s->setFloat(e.get(), name, value);
    };
    entity["setEffectVector"] = [](LuaEntity& e, const std::string& name, const Vec3& value) {
        if (vfx::VfxSystem* s = vfx::activeSystem()) s->setVector(e.get(), name, value);
    };
    entity["setEffectColor"] = [](LuaEntity& e, const std::string& name, const Vec3& rgb, sol::optional<float> alpha) {
        if (vfx::VfxSystem* s = vfx::activeSystem()) s->setColor(e.get(), name, core::Vec4{rgb, alpha.value_or(1.0f)});
    };
    entity["setEffectBool"] = [](LuaEntity& e, const std::string& name, bool value) {
        if (vfx::VfxSystem* s = vfx::activeSystem()) s->setBool(e.get(), name, value);
    };
    entity["getEffectFloat"] = [](LuaEntity& e, const std::string& name) -> sol::optional<float> {
        vfx::VfxSystem* s = vfx::activeSystem();
        core::Vec4 v{};
        if (s == nullptr || !s->getParam(e.get(), name, v)) return sol::nullopt;
        return v.x;
    };
    entity["effectParticles"] = sol::property([](const LuaEntity& e) {
        const vfx::VfxSystem* s = vfx::activeSystem();
        return s != nullptr ? static_cast<int>(s->aliveCount(e.get())) : 0;
    });

    // --- 2D ---
    const auto system2D = [this]() -> twod::System2D* { return twod::System2D::forWorld(world); };
    entity["velocity2D"] = sol::property(
        [system2D](const LuaEntity& e) {
            twod::System2D* s = system2D();
            const core::Vec2 v = s != nullptr ? s->physics().velocity(e.get()) : core::Vec2{};
            return Vec3{v.x, v.y, 0.0f};
        },
        [system2D](LuaEntity& e, const Vec3& v) {
            if (twod::System2D* s = system2D()) s->physics().setVelocity(e.get(), xy(v));
        });
    entity["angularVelocity2D"] = sol::property(
        [system2D](const LuaEntity& e) {
            twod::System2D* s = system2D();
            return s != nullptr ? s->physics().angularVelocity(e.get()) * 57.2957795f : 0.0f;
        },
        [system2D](LuaEntity& e, float degrees) {
            if (twod::System2D* s = system2D()) s->physics().setAngularVelocity(e.get(), degrees / 57.2957795f);
        });
    entity["addForce2D"] = [system2D](LuaEntity& e, const Vec3& f, sol::optional<std::string> mode) {
        if (twod::System2D* s = system2D()) s->physics().addForce(e.get(), xy(f), forceMode2D(mode));
    };
    entity["addForceAtPosition2D"] = [system2D](LuaEntity& e, const Vec3& f, const Vec3& p, sol::optional<std::string> mode) {
        if (twod::System2D* s = system2D()) s->physics().addForceAtPosition(e.get(), xy(f), xy(p), forceMode2D(mode));
    };
    entity["addTorque2D"] = [system2D](LuaEntity& e, float torque, sol::optional<std::string> mode) {
        if (twod::System2D* s = system2D()) s->physics().addTorque(e.get(), torque, forceMode2D(mode));
    };
    entity["movePosition2D"] = [system2D](LuaEntity& e, const Vec3& p) {
        if (twod::System2D* s = system2D()) s->physics().setPosition(e.get(), xy(p));
    };
    entity["isSleeping2D"] = [system2D](const LuaEntity& e) {
        twod::System2D* s = system2D();
        return s != nullptr && s->physics().isSleeping(e.get());
    };
    // Sprites
    entity["playSpriteAnimation"] = [](LuaEntity& e, const std::string& clip, sol::optional<bool> restart) {
        ecs::Entity x = e.get();
        twod::SpriteAnimator* a = x.valid() ? x.tryGet<twod::SpriteAnimator>() : nullptr;
        if (a == nullptr || a->findClip(clip) == nullptr) return false;
        a->play(clip, restart.value_or(false));
        return true;
    };
    entity["spriteAnimation"] = sol::property([](const LuaEntity& e) {
        const ecs::Entity x = e.get();
        const twod::SpriteAnimator* a = x.valid() ? x.tryGet<twod::SpriteAnimator>() : nullptr;
        return a != nullptr ? a->state.clip : std::string();
    });
    entity["spriteAnimationFinished"] = sol::property([](const LuaEntity& e) {
        const ecs::Entity x = e.get();
        const twod::SpriteAnimator* a = x.valid() ? x.tryGet<twod::SpriteAnimator>() : nullptr;
        return a != nullptr && a->state.finished;
    });
    entity["spriteFrame"] = sol::property(
        [](const LuaEntity& e) {
            const ecs::Entity x = e.get();
            const twod::SpriteRenderer* r = x.valid() ? x.tryGet<twod::SpriteRenderer>() : nullptr;
            return r != nullptr ? r->frame : 0;
        },
        [](LuaEntity& e, int frame) {
            ecs::Entity x = e.get();
            if (twod::SpriteRenderer* r = x.valid() ? x.tryGet<twod::SpriteRenderer>() : nullptr) r->frame = std::max(frame, 0);
        });
    entity["flipX"] = sol::property(
        [](const LuaEntity& e) {
            const ecs::Entity x = e.get();
            const twod::SpriteRenderer* r = x.valid() ? x.tryGet<twod::SpriteRenderer>() : nullptr;
            return r != nullptr && r->flip_x;
        },
        [](LuaEntity& e, bool flip) {
            ecs::Entity x = e.get();
            if (twod::SpriteRenderer* r = x.valid() ? x.tryGet<twod::SpriteRenderer>() : nullptr) r->flip_x = flip;
        });
    entity["spriteColor"] = sol::property(
        [](const LuaEntity& e) {
            const ecs::Entity x = e.get();
            const twod::SpriteRenderer* r = x.valid() ? x.tryGet<twod::SpriteRenderer>() : nullptr;
            return r != nullptr ? r->color : Vec3{1.0f, 1.0f, 1.0f};
        },
        [](LuaEntity& e, const Vec3& c) {
            ecs::Entity x = e.get();
            if (twod::SpriteRenderer* r = x.valid() ? x.tryGet<twod::SpriteRenderer>() : nullptr) r->color = c;
        });
    // Tilemap: celdas en coordenadas de la rejilla (x, y).
    entity["setTile"] = [](LuaEntity& e, int cx, int cy, int id, sol::optional<int> layer) {
        ecs::Entity x = e.get();
        twod::Tilemap* map = x.valid() ? x.tryGet<twod::Tilemap>() : nullptr;
        if (map == nullptr) return false;
        return map->setTile(cx, cy, id, std::max(layer.value_or(1) - 1, 0));
    };
    entity["getTile"] = [](const LuaEntity& e, int cx, int cy, sol::optional<int> layer) {
        const ecs::Entity x = e.get();
        const twod::Tilemap* map = x.valid() ? x.tryGet<twod::Tilemap>() : nullptr;
        return map != nullptr ? static_cast<int>(map->getTile(cx, cy, std::max(layer.value_or(1) - 1, 0))) : 0;
    };
    entity["worldToCell"] = [](const LuaEntity& e, const Vec3& p) {
        const ecs::Entity x = e.get();
        const twod::Tilemap* map = x.valid() ? x.tryGet<twod::Tilemap>() : nullptr;
        int cx = 0, cy = 0;
        if (map != nullptr) map->cellAt(ecs::transformPoint(core::inverse(x.worldMatrix()), p), cx, cy);
        return std::make_tuple(cx, cy);
    };
    entity["cellToWorld"] = [](const LuaEntity& e, int cx, int cy) {
        const ecs::Entity x = e.get();
        const twod::Tilemap* map = x.valid() ? x.tryGet<twod::Tilemap>() : nullptr;
        return map != nullptr ? ecs::transformPoint(x.worldMatrix(), map->cellCenter(cx, cy)) : Vec3{};
    };

    sol::table p2 = L.create_named_table("Physics2D");
    const auto hitTable = [this](const twod::RaycastHit2D& h) {
        sol::table t = lua->create_table();
        t["entity"] = LuaEntity{h.entity.handle(), world};
        t["point"] = Vec3{h.point.x, h.point.y, h.entity.valid() ? h.entity.worldPosition().z : 0.0f};
        t["normal"] = Vec3{h.normal.x, h.normal.y, 0.0f};
        t["distance"] = h.distance;
        t["fraction"] = h.fraction;
        return t;
    };
    p2["raycast"] = [this, system2D, hitTable](const Vec3& origin, const Vec3& direction, sol::optional<float> distance,
                                               sol::optional<int> mask) -> sol::object {
        twod::System2D* s = system2D();
        if (s == nullptr) return sol::lua_nil;
        twod::RaycastHit2D hit;
        if (!s->physics().raycast(xy(origin), xy(direction), distance.value_or(1000.0f), hit,
                                  mask ? static_cast<std::uint32_t>(*mask) : 0xFFFFFFFFu)) {
            return sol::lua_nil;
        }
        return hitTable(hit);
    };
    p2["raycastAll"] = [this, system2D, hitTable](const Vec3& origin, const Vec3& direction, sol::optional<float> distance,
                                                  sol::optional<int> mask) {
        sol::table out = lua->create_table();
        twod::System2D* s = system2D();
        if (s == nullptr) return out;
        int i = 1;
        for (const twod::RaycastHit2D& h : s->physics().raycastAll(xy(origin), xy(direction), distance.value_or(1000.0f),
                                                                    mask ? static_cast<std::uint32_t>(*mask) : 0xFFFFFFFFu)) {
            out[i++] = hitTable(h);
        }
        return out;
    };
    const auto entityList = [this](const std::vector<ecs::Entity>& list) {
        sol::table out = lua->create_table();
        int i = 1;
        for (const ecs::Entity& e : list) out[i++] = LuaEntity{e.handle(), world};
        return out;
    };
    p2["overlapCircle"] = [system2D, entityList](const Vec3& center, float radius, sol::optional<int> mask) {
        twod::System2D* s = system2D();
        return entityList(s != nullptr ? s->physics().overlapCircle(xy(center), radius, mask ? static_cast<std::uint32_t>(*mask) : 0xFFFFFFFFu)
                                       : std::vector<ecs::Entity>{});
    };
    p2["overlapBox"] = [system2D, entityList](const Vec3& center, const Vec3& size, sol::optional<float> angle, sol::optional<int> mask) {
        twod::System2D* s = system2D();
        return entityList(s != nullptr ? s->physics().overlapBox(xy(center), xy(size), angle.value_or(0.0f),
                                                                 mask ? static_cast<std::uint32_t>(*mask) : 0xFFFFFFFFu)
                                       : std::vector<ecs::Entity>{});
    };
    p2["overlapPoint"] = [system2D, entityList](const Vec3& point, sol::optional<int> mask) {
        twod::System2D* s = system2D();
        return entityList(s != nullptr ? s->physics().overlapPoint(xy(point), mask ? static_cast<std::uint32_t>(*mask) : 0xFFFFFFFFu)
                                       : std::vector<ecs::Entity>{});
    };
    p2["getGravity"] = [system2D]() {
        twod::System2D* s = system2D();
        return s != nullptr ? Vec3{s->physics().gravity.x, s->physics().gravity.y, 0.0f} : Vec3{0.0f, -9.81f, 0.0f};
    };
    p2["setGravity"] = [system2D](const Vec3& g) {
        if (twod::System2D* s = system2D()) s->physics().gravity = xy(g);
    };

    // --- Destruccion (Destructible) ---
    entity["fracture"] = [this](LuaEntity& e, sol::optional<Vec3> point, sol::optional<float> force) {
        ecs::Entity x = e.get();
        if (!x.valid() || physics == nullptr) return false;
        return physics->destruction().fracture(x, point.value_or(x.worldPosition()), force.value_or(-1.0f));
    };
    entity["damage"] = [this](LuaEntity& e, float amount, sol::optional<Vec3> point, sol::optional<float> force) {
        ecs::Entity x = e.get();
        if (!x.valid() || physics == nullptr) return;
        physics->destruction().applyDamage(x, amount, point.value_or(x.worldPosition()), force.value_or(-1.0f));
    };
    entity["health"] = sol::property([this](const LuaEntity& e) {
        const ecs::Entity x = e.get();
        return x.valid() && physics != nullptr ? physics->destruction().health(x) : 0.0f;
    });
    entity["isBroken"] = sol::property([this](const LuaEntity& e) {
        const ecs::Entity x = e.get();
        return x.valid() && physics != nullptr && physics->destruction().isBroken(x);
    });
    if (physics != nullptr && break_listener < 0) {
        break_listener = physics->destruction().addBreakListener([this](const physics::BreakEvent& event) { breaks.push_back(event); });
    }

    // --- Vehiculos: cambio manual y estado completo ---
    entity["setGear"] = [this](LuaEntity& e, int gear) {
        if (ecs::Entity x = e.get(); x.valid() && physics != nullptr) physics->setVehicleGear(x, gear);
    };
    entity["shiftGear"] = [this](LuaEntity& e, int delta) {
        if (ecs::Entity x = e.get(); x.valid() && physics != nullptr) physics->shiftVehicleGear(x, delta);
    };
    entity["vehicleState"] = [this](const LuaEntity& e) -> sol::object {
        const ecs::Entity x = e.get();
        if (!x.valid() || physics == nullptr) return sol::lua_nil;
        const physics::PhysicsSystem::VehicleState s = physics->vehicleState(x);
        if (!s.valid) return sol::lua_nil;
        sol::table t = lua->create_table();
        t["speed"] = s.speed_kmh;
        t["forwardSpeed"] = s.forward_speed_kmh;
        t["rpm"] = s.rpm;
        t["rpmFraction"] = s.rpm_fraction;
        t["gear"] = s.gear;
        t["gearCount"] = s.gear_count;
        t["automatic"] = s.automatic;
        return t;
    };

    // --- Motion Matching (modo Script) ---
    entity["setMotionVelocity"] = [](LuaEntity& e, const Vec3& v) {
        ecs::Entity x = e.get();
        if (anim::MotionMatching* mm = x.valid() ? x.tryGet<anim::MotionMatching>() : nullptr) mm->runtime.desired_velocity = v;
    };
    entity["setMotionFacing"] = [](LuaEntity& e, const Vec3& v) {
        ecs::Entity x = e.get();
        if (anim::MotionMatching* mm = x.valid() ? x.tryGet<anim::MotionMatching>() : nullptr) mm->runtime.desired_facing = v;
    };
    entity["setMotionTags"] = [](LuaEntity& e, const std::string& tags) {
        ecs::Entity x = e.get();
        if (anim::MotionMatching* mm = x.valid() ? x.tryGet<anim::MotionMatching>() : nullptr) mm->runtime.extra_tags = tags;
    };
    entity["motionClip"] = sol::property([](const LuaEntity& e) {
        const ecs::Entity x = e.get();
        const anim::MotionMatching* mm = x.valid() ? x.tryGet<anim::MotionMatching>() : nullptr;
        return mm != nullptr ? mm->runtime.clip_label : std::string();
    });

    // --- Repeticiones ---
    sol::table rp = L.create_named_table("Replay");
    rp["start"] = [this](sol::optional<sol::table> options) {
        replay::ReplaySystem* r = replay::activeSystem();
        if (r == nullptr || world == nullptr) return false;
        replay::ReplayOptions o;
        if (options) {
            o.rate = options->get_or("rate", o.rate);
            o.max_seconds = options->get_or("maxSeconds", o.max_seconds);
            o.tag = options->get_or("tag", o.tag);
            o.record_audio = options->get_or("audio", o.record_audio);
            o.record_animation = options->get_or("animation", o.record_animation);
        }
        return r->startRecording(*world, o);
    };
    rp["stop"] = []() {
        if (replay::ReplaySystem* r = replay::activeSystem()) r->stopRecording();
    };
    rp["play"] = [this](sol::optional<float> from, sol::optional<float> speed) {
        replay::ReplaySystem* r = replay::activeSystem();
        return r != nullptr && world != nullptr && r->play(*world, from.value_or(0.0f), speed.value_or(1.0f));
    };
    rp["stopPlayback"] = [this]() {
        replay::ReplaySystem* r = replay::activeSystem();
        if (r != nullptr && world != nullptr) r->stop(*world);
    };
    rp["seek"] = [](float t) {
        if (replay::ReplaySystem* r = replay::activeSystem()) r->seek(t);
    };
    rp["pause"] = [](sol::optional<bool> paused) {
        if (replay::ReplaySystem* r = replay::activeSystem()) r->setPaused(paused.value_or(true));
    };
    rp["setSpeed"] = [](float speed) {
        if (replay::ReplaySystem* r = replay::activeSystem()) r->setSpeed(speed);
    };
    rp["setLoop"] = [](bool loop) {
        if (replay::ReplaySystem* r = replay::activeSystem()) r->setLoop(loop);
    };
    rp["setFreeCamera"] = [](bool free) {
        if (replay::ReplaySystem* r = replay::activeSystem()) r->setFreeCamera(free);
    };
    rp["mark"] = [](const std::string& name, sol::optional<std::string> data) {
        if (replay::ReplaySystem* r = replay::activeSystem()) r->mark(name, data.value_or(std::string()));
    };
    rp["save"] = [](const std::string& name) {
        replay::ReplaySystem* r = replay::activeSystem();
        return r != nullptr && r->save(r->fileFor(name));
    };
    rp["load"] = [](const std::string& name) {
        replay::ReplaySystem* r = replay::activeSystem();
        return r != nullptr && r->load(r->fileFor(name));
    };
    rp["list"] = [this]() {
        sol::table out = lua->create_table();
        if (replay::ReplaySystem* r = replay::activeSystem()) {
            int i = 1;
            for (const std::string& name : r->list()) out[i++] = name;
        }
        return out;
    };
    rp["isRecording"] = []() {
        const replay::ReplaySystem* r = replay::activeSystem();
        return r != nullptr && r->recording();
    };
    rp["isPlaying"] = []() {
        const replay::ReplaySystem* r = replay::activeSystem();
        return r != nullptr && r->playing();
    };
    rp["time"] = []() {
        const replay::ReplaySystem* r = replay::activeSystem();
        return r != nullptr ? r->time() : 0.0f;
    };
    rp["duration"] = []() {
        const replay::ReplaySystem* r = replay::activeSystem();
        return r != nullptr ? r->duration() : 0.0f;
    };
}

void ScriptSystem::Impl::dispatch2DEvents() {
    twod::System2D* s = twod::System2D::forWorld(world);
    if (s == nullptr || lua == nullptr) return;
    for (const twod::Physics2DEvent& event : s->takeEvents()) {
        const char* method = nullptr;
        switch (event.type) {
            case twod::Physics2DEventType::CollisionEnter: method = "OnCollisionEnter2D"; break;
            case twod::Physics2DEventType::CollisionStay: method = "OnCollisionStay2D"; break;
            case twod::Physics2DEventType::CollisionExit: method = "OnCollisionExit2D"; break;
            case twod::Physics2DEventType::TriggerEnter: method = "OnTriggerEnter2D"; break;
            case twod::Physics2DEventType::TriggerStay: method = "OnTriggerStay2D"; break;
            case twod::Physics2DEventType::TriggerExit: method = "OnTriggerExit2D"; break;
        }
        const twod::Physics2DEvent sides[2] = {event, event.flipped()};
        for (const twod::Physics2DEvent& side : sides) {
            if (!side.a.valid() || !side.b.valid()) continue;
            const auto it = instances.find(side.a.handle());
            Instance* visual = vsInstance(side.a.handle());
            if (it == instances.end() && visual == nullptr) continue;
            sol::table contact = lua->create_table();
            contact["point"] = side.point;
            contact["normal"] = side.normal;
            contact["relativeVelocity"] = side.relative_velocity;
            if (it != instances.end()) call(it->second, method, LuaEntity{side.b.handle(), world}, contact);
            if (visual != nullptr) call(*visual, method, LuaEntity{side.b.handle(), world}, contact);
        }
    }
}

void ScriptSystem::Impl::dispatchBreakEvents() {
    if (breaks.empty() || lua == nullptr) return;
    std::vector<physics::BreakEvent> queue;
    queue.swap(breaks);
    for (const physics::BreakEvent& event : queue) {
        if (!event.entity.valid()) continue;
        sol::table pieces = lua->create_table();
        int i = 1;
        for (const ecs::Entity& p : event.pieces) pieces[i++] = LuaEntity{p.handle(), world};
        const auto it = instances.find(event.entity.handle());
        if (it != instances.end()) call(it->second, "OnBreak", pieces, event.point);
        if (Instance* visual = vsInstance(event.entity.handle())) call(*visual, "OnBreak", pieces, event.point);
    }
}

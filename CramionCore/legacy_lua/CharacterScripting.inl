// Character Controller desde Lua (componente physics::CharacterController).
//
//   local flags = self.entity:move(Vec3.new(0, 0, -speed * dt))  -- Move de Unity
//   if self.entity:isGrounded() then ... end
//   self.entity:setMoveInput(dir, corriendo)   -- movimiento integrado
//   self.entity:jump()  /  self.entity:setCrouch(true)
//   local s = self.entity:characterState()     -- grounded, velocity, ground...
//
// CharacterController.Sides / Above / Below: bits de lo que devuelve move()
// (y de characterState().flags).
//
// Se incluye al final de Scripting.cpp (necesita ScriptSystem::Impl).

void ScriptSystem::Impl::bindCharacter(sol::state& L, sol::usertype<LuaEntity>& entity) {
    sol::table cc = L.create_named_table("CharacterController");
    cc["Sides"] = static_cast<lua_Integer>(physics::PhysicsSystem::kCollidedSides);
    cc["Above"] = static_cast<lua_Integer>(physics::PhysicsSystem::kCollidedAbove);
    cc["Below"] = static_cast<lua_Integer>(physics::PhysicsSystem::kCollidedBelow);

    entity["move"] = [this](LuaEntity& e, const Vec3& displacement) -> lua_Integer {
        ecs::Entity x = e.get();
        if (!x.valid() || physics == nullptr || world == nullptr) return 0;
        return static_cast<lua_Integer>(physics->moveCharacter(*world, x, displacement));
    };
    entity["isGrounded"] = [this](const LuaEntity& e) {
        const ecs::Entity x = e.get();
        return x.valid() && physics != nullptr && physics->characterState(x).grounded;
    };
    entity["isCrouching"] = [this](const LuaEntity& e) {
        const ecs::Entity x = e.get();
        return x.valid() && physics != nullptr && physics->characterState(x).crouching;
    };
    entity["setMoveInput"] = [this](LuaEntity& e, const Vec3& direction, sol::optional<bool> run) {
        if (auto x = e.get(); x.valid() && physics != nullptr) physics->setCharacterInput(x, direction, run.value_or(false));
    };
    entity["jump"] = [this](LuaEntity& e, sol::optional<float> height) {
        ecs::Entity x = e.get();
        return x.valid() && physics != nullptr && physics->characterJump(x, height.value_or(-1.0f));
    };
    entity["setCrouch"] = [this](LuaEntity& e, bool crouch) {
        if (auto x = e.get(); x.valid() && physics != nullptr) physics->setCharacterCrouch(x, crouch);
    };
    entity["addCharacterVelocity"] = [this](LuaEntity& e, const Vec3& velocity) {
        if (auto x = e.get(); x.valid() && physics != nullptr) physics->addCharacterVelocity(x, velocity);
    };
    entity["characterState"] = [this](const LuaEntity& e) -> sol::object {
        const ecs::Entity x = e.get();
        if (!x.valid() || physics == nullptr) return sol::lua_nil;
        const physics::PhysicsSystem::CharacterState s = physics->characterState(x);
        if (!s.valid) return sol::lua_nil;
        static constexpr const char* kGround[] = {"OnGround", "OnSteepGround", "NotSupported", "InAir"};
        sol::table t = lua->create_table();
        t["grounded"] = s.grounded;
        t["groundState"] = kGround[static_cast<int>(s.ground_state)];
        t["crouching"] = s.crouching;
        t["velocity"] = s.velocity;
        t["groundNormal"] = s.ground_normal;
        t["groundPoint"] = s.ground_point;
        t["groundVelocity"] = s.ground_velocity;
        if (s.ground.valid()) t["ground"] = LuaEntity{s.ground.handle(), world};
        t["flags"] = static_cast<lua_Integer>(s.collision_flags);
        t["jumps"] = s.jumps_used;
        return t;
    };
}

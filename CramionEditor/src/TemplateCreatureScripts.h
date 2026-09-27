#ifndef CRAMION_EDITOR_TEMPLATE_CREATURE_SCRIPTS_H
#define CRAMION_EDITOR_TEMPLATE_CREATURE_SCRIPTS_H

// Scripts de la plantilla "Criaturas": IK de animales, phys bones, ragdoll
// y huesos, todo manejado desde Lua.

namespace cramion::editor::creature_scripts {

constexpr const char* kDog = R"lua(-- Perro: recorre un circuito con escaleras, una rampa y piedras. Sus patas se
-- apoyan en el suelo con el IK (cadenas de 3 huesos con "Al suelo") y el
-- cuerpo se inclina en la rampa. La cola y las orejas son Phys Bones y la
-- cabeza (con el cuello) sigue a la pelota.
--   R: ragdoll (se cae con un empujon); otra vez R: se levanta.
--   Espacio con el ragdoll: otro empujon.
local Perro = {
    properties = {
        velocidad = 1.4,  -- m/s
        giro = 3.0,       -- rapidez al girar
    }
}

local PUNTOS = {
    Vec3(-7, 0, 7), Vec3(-1, 0, 7), Vec3(6, 0, 7), Vec3(7, 0, 1),
    Vec3(7, 0, -6), Vec3(0, 0, -7), Vec3(-7, 0, -6), Vec3(-7, 0, 1),
}

function Perro:Start()
    self.i = 1
    self.pelota = Scene.find("Pelota")
    self.entity:setLookAt(self.pelota, 0.7)
    self.entity:playAnimation("Caminar", true)
end

-- Altura del suelo bajo un punto (escalones, rampa, piedras).
function Perro:suelo(p)
    local hit = Physics.raycast(p + Vec3(0, 2, 0), Vec3(0, -1, 0), 6)
    if hit then return hit.point.y end
    return p.y
end

function Perro:Update(dt)
    if Input.getKeyDown("r") then
        self.entity.ragdoll = not self.entity.ragdoll
        if self.entity.ragdoll then
            -- Se cae de lado con la velocidad que llevaba y un empujon.
            local f = self.entity.forward
            local lado = Vec3(f.z, 0, -f.x)
            self.entity:addRagdollForce(lado * 70 + Vec3(0, 25, 0), "Chest")
        end
    end
    if self.entity.ragdoll then
        if Input.getKeyDown("space") then
            self.entity:addRagdollForce(Vec3(Random.range(-30, 30), 55, Random.range(-30, 30)))
        end
        return
    end

    -- Andar hacia el siguiente punto, girando suave.
    local pos = self.entity.position
    local meta = PUNTOS[self.i]
    local hacia = Vec3(meta.x - pos.x, 0, meta.z - pos.z)
    if hacia:length() < 0.6 then
        self.i = self.i % #PUNTOS + 1
        return
    end
    local delante = self.entity.forward
    delante = Vec3(delante.x, 0, delante.z):normalized()
    local nuevo = delante:lerp(hacia:normalized(), Mathf.clamp01(self.giro * dt)):normalized()
    pos = pos + nuevo * (self.velocidad * dt)
    pos.y = self:suelo(pos)
    self.entity.position = pos
    self.entity:lookAt(pos + nuevo)
end

return Perro
)lua";

constexpr const char* kDummy = R"lua(-- Maniqui humanoide: sus pies se apoyan en el escalon (Pies en el suelo),
-- la coleta es un Phys Bone y la cabeza sigue a la pelota.
--   F: cae (ragdoll) empujado desde la camara; se levanta solo a los 4 s
--      (o con G), mezclando de vuelta a la animacion.
local Maniqui = {
    properties = { levantarse = 4.0 }
}

function Maniqui:Start()
    self.pelota = Scene.find("Pelota")
    self.camara = Scene.find("Main Camera")
    self.entity:setLookAt(self.pelota, 0.9)
    self.entity:playAnimation("Reposo", true)
    self.tiempo = 0
end

function Maniqui:Update(dt)
    if Input.getKeyDown("f") and not self.entity.ragdoll then
        self.entity.ragdoll = true
        local desde = (self.entity.position - self.camara.position)
        desde = Vec3(desde.x, 0, desde.z):normalized()
        self.entity:addRagdollForce(desde * 140 + Vec3(0, 25, 0), "Spine1")
    end
    if self.entity.ragdoll then
        -- Empezo a caer (con F o desde otro script): cuenta hasta levantarse.
        if not self.cayendo then
            self.cayendo = true
            self.tiempo = self.levantarse
        end
        self.tiempo = self.tiempo - dt
        if self.tiempo <= 0 or Input.getKeyDown("g") then
            self.entity.ragdoll = false
        end
    else
        self.cayendo = false
    end
end

return Maniqui
)lua";

constexpr const char* kBall = R"lua(-- Pelota que da vueltas: el perro y el maniqui la siguen con la cabeza (IK).
local Pelota = {
    properties = { radio = 4.0, altura = 1.3, velocidad = 0.5 }
}

function Pelota:Start()
    self.centro = self.entity.position
    self.t = 0
end

function Pelota:Update(dt)
    self.t = self.t + dt * self.velocidad
    local p = Vec3(math.cos(self.t) * self.radio, self.altura + math.sin(self.t * 2.3) * 0.6, math.sin(self.t) * self.radio)
    self.entity.position = self.centro + p
end

return Pelota
)lua";

constexpr const char* kControls = R"lua(-- Controles de la demo y el texto de ayuda.
--   B: ver los huesos del perro      I: IK del perro (pies y mirada) on/off
--   P: phys bones (cola, orejas, coleta) on/off
local Controles = {}

function Controles:Start()
    self.perro = Scene.find("Perro")
    self.maniqui = Scene.find("Maniqui")
    self.huesos = false
    self.ik = true
    self.fisica = true
    self:pintar()
end

function Controles:pintar()
    local function si(v) return v and "si" or "no" end
    self.entity.text = "R: ragdoll del perro (Espacio: empujar)   F: tirar al maniqui   B: huesos (" .. si(self.huesos) ..
        ")   I: IK (" .. si(self.ik) .. ")   P: phys bones (" .. si(self.fisica) .. ")   Clic derecho y rueda: camara"
end

function Controles:Update(dt)
    if Input.getKeyDown("b") then
        self.huesos = not self.huesos
        self.perro:showBones(self.huesos)
        self:pintar()
    end
    if Input.getKeyDown("i") then
        self.ik = not self.ik
        self.perro:setField("InverseKinematics", "enabled", self.ik)
        self.maniqui:setField("InverseKinematics", "enabled", self.ik)
        self:pintar()
    end
    if Input.getKeyDown("p") then
        self.fisica = not self.fisica
        self.perro:setField("PhysBones", "enabled", self.fisica)
        self.maniqui:setField("PhysBones", "enabled", self.fisica)
        self:pintar()
    end
end

return Controles
)lua";

}  // namespace cramion::editor::creature_scripts

#endif  // CRAMION_EDITOR_TEMPLATE_CREATURE_SCRIPTS_H

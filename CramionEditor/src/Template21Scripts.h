#ifndef CRAMION_EDITOR_TEMPLATE21_SCRIPTS_H
#define CRAMION_EDITOR_TEMPLATE21_SCRIPTS_H

// Scripts de las plantillas de la 2.1: "Plataformas 2D" y "Coches".

namespace cramion::editor::template21 {

// --- Plataformas 2D ---------------------------------------------------------------

inline constexpr const char* kPlayer2D = R"lua(-- Jugador de plataformas: A/D o flechas para moverse, Espacio para saltar.
-- Fisica 2D (Rigidbody2D + Box Collider 2D) y animaciones del Sprite Animator.
local Jugador = {
    properties = { velocidad = 6, salto = 12.5 },
}

function Jugador:Start()
    self.monedas = 0
    self.inicio = self.entity.position
end

function Jugador:Update(dt)
    local v = self.entity.velocity2D
    local eje = 0
    if Input.getKey("a") or Input.getKey("left") then eje = eje - 1 end
    if Input.getKey("d") or Input.getKey("right") then eje = eje + 1 end
    v.x = eje * self.velocidad

    -- En el suelo: un rayo corto desde los pies.
    local pies = self.entity.position + Vec3(0, -0.5, 0)
    local suelo = Physics2D.raycast(pies, Vec3(0, -1, 0), 0.12) ~= nil
    if suelo and (Input.getKeyDown("space") or Input.getKeyDown("w") or Input.getKeyDown("up")) then
        v.y = self.salto
    end
    self.entity.velocity2D = v

    if eje ~= 0 then self.entity.flipX = eje < 0 end
    if not suelo then
        self.entity:playSpriteAnimation("Saltar")
    elseif eje ~= 0 then
        self.entity:playSpriteAnimation("Correr")
    else
        self.entity:playSpriteAnimation("Quieto")
    end

    -- Se cae por un agujero: vuelve al principio.
    if self.entity.position.y < -12 then
        self.entity:movePosition2D(self.inicio)
        self.entity.velocity2D = Vec3(0, 0, 0)
    end
end

function Jugador:OnTriggerEnter2D(otro, contacto)
    if otro.tag == "Moneda" then
        otro:destroy()
        self.monedas = self.monedas + 1
        local marcador = Scene.find("Marcador")
        if marcador then marcador.text = "Monedas: " .. self.monedas end
    end
end

return Jugador
)lua";

inline constexpr const char* kCamera2D = R"lua(-- Camara 2D que sigue al jugador con suavidad (y un poco por delante).
local Camara = {
    properties = { suavidad = 5, altura = 1.5 },
}

function Camara:LateUpdate(dt)
    local jugador = Scene.find("Jugador")
    if not jugador then return end
    local p = self.entity.position
    local t = jugador.position
    local k = math.min(1, dt * self.suavidad)
    self.entity.position = Vec3(p.x + (t.x - p.x) * k, p.y + (t.y + self.altura - p.y) * k, p.z)
end

return Camara
)lua";

// --- Coches ---------------------------------------------------------------------------

inline constexpr const char* kChaseCamera = R"lua(-- Camara de persecucion: detras del coche y un poco por encima, con muelle.
local Camara = {
    properties = { distancia = 8, altura = 3, suavidad = 4 },
}

function Camara:LateUpdate(dt)
    local coche = Scene.find("Coche")
    if not coche then return end
    local destino = coche.position - coche.forward * self.distancia + Vec3(0, self.altura, 0)
    local k = math.min(1, dt * self.suavidad)
    self.entity.position = self.entity.position + (destino - self.entity.position) * k
    self.entity:lookAt(coche.position + Vec3(0, 1, 0))
end

return Camara
)lua";

inline constexpr const char* kSpeedometer = R"lua(-- Velocimetro: km/h, marcha y rpm del coche (Vehicle) en el HUD.
local Velocimetro = {}

function Velocimetro:Update(dt)
    local coche = Scene.find("Coche")
    if not coche then return end
    local estado = coche:vehicleState()
    if not estado then return end
    local marcha = estado.gear == -1 and "R" or (estado.gear == 0 and "N" or tostring(estado.gear))
    self.entity.text = string.format("%3d km/h   marcha %s   %4d rpm", math.floor(estado.speed + 0.5), marcha,
                                     math.floor(estado.rpm))
    -- R: volver a la salida (si vuelca).
    if Input.getKeyDown("r") then
        coche.position = Vec3(0, 1.5, 0)
        coche.rotation = Vec3(0, 0, 0)
        coche.velocity = Vec3(0, 0, 0)
    end
end

return Velocimetro
)lua";

}  // namespace cramion::editor::template21

#endif  // CRAMION_EDITOR_TEMPLATE21_SCRIPTS_H

-- Camara del menu: da vueltas lentas alrededor del campo.
local Orbita = {
    properties = {
        centro = Vec3(0, 0, -4),
        radio = 9.0,
        altura = 5.0,
        velocidad = 6.0,  -- grados por segundo
    }
}

function Orbita:Start() self.angulo = 30 end

function Orbita:Update(dt)
    self.angulo = self.angulo + self.velocidad * dt
    local r = math.rad(self.angulo)
    self.entity.position = self.centro + Vec3(math.sin(r) * self.radio, self.altura, math.cos(r) * self.radio)
    self.entity:lookAt(self.centro)
end

return Orbita

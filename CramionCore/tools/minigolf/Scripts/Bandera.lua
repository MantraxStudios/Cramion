-- La bandera sube cuando la pelota se acerca (para que pueda entrar).
local Bandera = { properties = { subida = 0.9 } }

function Bandera:Start()
    self.base = self.entity.position
    self.pelota = Scene.find("Pelota")
    self.altura = 0
end

function Bandera:Update(dt)
    if not self.pelota then return end
    local d = self.pelota.position - self.base
    local cerca = math.sqrt(d.x * d.x + d.z * d.z) < 1.4
    local objetivo = cerca and self.subida or 0
    self.altura = self.altura + (objetivo - self.altura) * (1 - math.exp(-dt * 6))
    self.entity.position = self.base + Vec3(0, self.altura, 0)
end

return Bandera

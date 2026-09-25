-- Gira sin parar (las aspas del molino, la camara del menu...).
local Girar = {
    properties = {
        eje = Vec3(0, 0, 1),     -- eje local
        velocidad = 60.0,        -- grados por segundo
    }
}

function Girar:Update(dt)
    self.entity:rotate(self.eje * (self.velocidad * dt))
end

return Girar

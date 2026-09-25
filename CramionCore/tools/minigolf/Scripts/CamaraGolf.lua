-- Camara que sigue a la pelota por detras de la direccion de tiro.
-- Rueda del raton: acercar / alejar.
local CamaraGolf = {
    properties = {
        distancia = 3.2,
        altura = 1.7,
        suavidad = 5.0,
    }
}

function CamaraGolf:Start()
    self.pelota = Scene.find("Pelota")
    self.zoom = 1.0
    self.foco = nil
    self.pos = nil
end

function CamaraGolf:LateUpdate(dt)
    if not self.pelota then return end
    local s = self.pelota:getScript()
    local yaw = (s and s.yaw) or 0
    local r = math.rad(yaw)
    local dir = Vec3(-math.sin(r), 0, -math.cos(r))

    local rueda = Input.getAxis("mouse scrollwheel")
    if rueda ~= 0 then
        self.zoom = Mathf.clamp(self.zoom - rueda * 0.12, 0.55, 2.4)
    end

    local objetivo = self.pelota.position
    local k = 1 - math.exp(-dt * self.suavidad * 1.4)
    if self.foco == nil then self.foco = objetivo else self.foco = self.foco:lerp(objetivo, k) end
    local deseada = self.foco - dir * (self.distancia * self.zoom) + Vec3(0, self.altura * self.zoom, 0)
    local kp = 1 - math.exp(-dt * self.suavidad)
    if self.pos == nil then self.pos = deseada else self.pos = self.pos:lerp(deseada, kp) end
    self.entity.position = self.pos
    self.entity:lookAt(self.foco + dir * 0.8 + Vec3(0, 0.15, 0))
end

return CamaraGolf

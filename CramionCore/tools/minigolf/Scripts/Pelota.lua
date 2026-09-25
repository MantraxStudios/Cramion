-- La pelota: apuntar, cargar el golpe, rodar, caer al hoyo o salirse.
--
--   A / D o flechas       girar la direccion
--   clic derecho + raton  girar la direccion
--   Espacio o clic izq.   mantener para cargar, soltar para golpear
--   R                     volver al ultimo tiro (+1 golpe)
--   Esc                   menu
local Pelota = {
    properties = {
        yawInicial = 0.0,        -- grados (0 = hacia -Z)
        velocidadMaxima = 10.0,  -- m/s con la barra llena
        velocidadGiro = 90.0,    -- grados por segundo con el teclado
    }
}

local RADIO_HOYO = 0.19
local PROFUNDIDAD = 0.07

local function direccion(yaw)
    local r = math.rad(yaw)
    return Vec3(-math.sin(r), 0, -math.cos(r))
end

function Pelota:Start()
    self.yaw = self.yawInicial
    self.estado = "apuntando"  -- apuntando | cargando | rodando | hoyo
    self.potencia = 0
    self.carga = 0
    self.quieto = 0
    self.tiempoTiro = 0
    self.ultimo = self.entity.position
    self.sueloY = self.entity.position.y
    self.hoyo = Scene.find("Hoyo")
    self.nivel = Scene.find("Nivel")
    self.mira = {}
    for i = 1, 8 do
        local punto = Scene.find("Mira" .. i)
        if punto then self.mira[#self.mira + 1] = punto end
    end
end

function Pelota:nivelScript()
    return self.nivel and self.nivel:getScript() or nil
end

function Pelota:parar()
    self.entity.velocity = Vec3(0, 0, 0)
    self.entity.angularVelocity = Vec3(0, 0, 0)
end

function Pelota:volverAlTiro(penalizar)
    self.entity.position = self.ultimo
    self:parar()
    self.estado = "apuntando"
    self.quieto = 0
    Audio.playOneShot("Audio/fuera.wav")
    local nivel = self:nivelScript()
    if penalizar and nivel then nivel:Penalizar() end
end

function Pelota:golpear()
    local fuerza = 0.6 + (self.potencia ^ 1.25) * self.velocidadMaxima
    self.ultimo = self.entity.position
    self.entity.velocity = direccion(self.yaw) * fuerza
    self.estado = "rodando"
    self.quieto = 0
    self.tiempoTiro = 0
    self.potencia = 0
    Audio.playOneShot("Audio/golpe.wav", nil, 0.9)
    local nivel = self:nivelScript()
    if nivel then nivel:OnGolpe() end
end

function Pelota:actualizarMira()
    local visible = self.estado == "apuntando" or self.estado == "cargando"
    local p = self.entity.position
    local dir = direccion(self.yaw)
    -- Con la carga, mas puntos y mas separados.
    local cuantos = #self.mira
    if self.estado == "cargando" then
        cuantos = math.max(2, math.ceil(#self.mira * (0.25 + 0.75 * self.potencia)))
    else
        cuantos = math.min(4, #self.mira)
    end
    local paso = 0.28 + 0.12 * self.potencia
    for i, punto in ipairs(self.mira) do
        punto.active = visible and i <= cuantos
        if punto.active then
            punto.position = p + dir * (0.2 + paso * i) + Vec3(0, -0.07, 0)
        end
    end
end

function Pelota:Update(dt)
    if Input.getKeyDown("escape") then
        Scene.load("Menu")
        return
    end
    if self.estado == "hoyo" then
        self:actualizarMira()
        return
    end

    local p = self.entity.position
    -- Fuera del campo (o cayo): al ultimo tiro con un golpe de castigo.
    if p.y < self.sueloY - 0.12 then
        self:volverAlTiro(true)
        return
    end
    if Input.getKeyDown("r") and self.estado == "rodando" then
        self:volverAlTiro(true)
        return
    end

    -- En el hoyo: dentro del circulo y por debajo del suelo.
    if self.hoyo then
        local h = self.hoyo.position
        local dx, dz = p.x - h.x, p.z - h.z
        local d = math.sqrt(dx * dx + dz * dz)
        local v = self.entity.velocity
        local rapidez = v:length()
        if d < RADIO_HOYO and p.y < h.y + PROFUNDIDAD then
            self.estado = "hoyo"
            self:parar()
            self.entity.position = Vec3(h.x, h.y - 0.02, h.z)
            Audio.playOneShot("Audio/hoyo.wav")
            local nivel = self:nivelScript()
            if nivel then nivel:OnHoyo() end
            return
        end
        -- Cerca y despacio: el borde del hoyo la atrae un poco.
        if self.estado == "rodando" and d < 0.34 and rapidez < 1.6 and d > 0.001 then
            local hacia = Vec3(-dx / d, 0, -dz / d)
            self.entity.velocity = v + hacia * (dt * 5.0)
        end
    end

    if self.estado == "rodando" then
        self.tiempoTiro = self.tiempoTiro + dt
        local rapidez = self.entity.velocity:length()
        if self.tiempoTiro > 0.3 and rapidez < 0.07 then
            self.quieto = self.quieto + dt
            if self.quieto > 0.3 then
                self:parar()
                self.estado = "apuntando"
            end
        else
            self.quieto = 0
        end
        -- Rodando muy lento mucho rato: se da por parada.
        if self.tiempoTiro > 12 then
            self:parar()
            self.estado = "apuntando"
        end
    else
        -- Apuntar.
        local giro = 0
        if Input.getKey("a") or Input.getKey("left") then giro = giro + 1 end
        if Input.getKey("d") or Input.getKey("right") then giro = giro - 1 end
        local lento = (Input.getKey("shift") and 0.3) or 1.0
        self.yaw = self.yaw + giro * self.velocidadGiro * lento * dt
        if Input.getMouseButton(1) then
            self.yaw = self.yaw - Input.mouseDelta().x * 0.25
        end
        -- Cargar y golpear.
        local cargando = Input.getKey("space") or Input.getMouseButton(0)
        if cargando then
            if self.estado ~= "cargando" then
                self.estado = "cargando"
                self.carga = 0
            end
            self.carga = self.carga + dt
            local t = (self.carga / 1.2) % 2
            self.potencia = t < 1 and t or 2 - t
        elseif self.estado == "cargando" then
            self:golpear()
        end
    end
    local nivel = self:nivelScript()
    if nivel then nivel:MostrarPotencia(self.estado == "cargando" and self.potencia or 0) end
    self:actualizarMira()
end

-- Rebotes: sonido segun la fuerza del choque.
function Pelota:OnCollisionEnter(other, contact)
    local fuerza = contact.relativeVelocity:length()
    if fuerza > 1.2 and other.name ~= "Suelo" then
        Audio.playOneShot("Audio/rebote.wav", nil, math.min(1.0, fuerza / 6))
    end
end

return Pelota

#ifndef CRAMION_EDITOR_TEMPLATE_LOCOMOTION_SCRIPTS_H
#define CRAMION_EDITOR_TEMPLATE_LOCOMOTION_SCRIPTS_H

// Scripts de la plantilla "Tercera persona avanzada" (el personaje y las
// animaciones del Locomotion Pack de Mixamo). Todo el juego va en Lua: el
// controlador del personaje (escalones, pendientes, salto sincronizado con la
// animacion, giros en el sitio, modo apuntar con desplazamiento lateral), el
// IK (pies en el suelo, mirar, la mano en las palancas), la camara con muelle
// y colision, las palancas, los cristales y la interfaz.

namespace cramion::editor::locomotion_scripts {

constexpr const char* kCharacter = R"lua(-- Personaje en tercera persona (Locomotion Pack de Mixamo). Todo en Lua.
--
--   WASD mover (relativo a la camara)  Shift correr  Espacio saltar
--   Clic derecho apuntar: el cuerpo mira con la camara y se mueve de lado
--   (Blend Tree 2D); quieto, gira en el sitio con las animaciones de girar
--   E accionar una palanca (la mano la agarra con IK)
--
-- El Animator (Personaje.cranimator) recibe X e Y: la velocidad del cuerpo
-- en m/s, de lado y hacia delante. Las velocidades de las propiedades son las
-- que medio el importador en cada animacion: los pies no patinan.
local Personaje = {
    properties = {
        velAndar = 1.66,         -- m/s de cada animacion (las mide el importador)
        velCorrer = 4.35,
        velLateral = 1.72,
        velLateralCorrer = 4.49,
        aceleracion = 9.0,
        giro = 600.0,            -- grados/s al girar hacia donde se mueve
        alturaSalto = 1.1,       -- m
        despegue = 0.8,          -- s de la animacion de saltar en que deja el suelo
        aterrizaje = 1.27,       -- y en que lo vuelve a tocar
        duracionSalto = 2.17,
        durGiro90 = 0.93,        -- duracion de las animaciones de girar 90 y 180
        durGiro180 = 1.63,
        energiaMax = 100,
        gastoCorrer = 16,        -- por segundo
        recuperar = 22,
        escalon = 0.45,          -- lo mas alto que sube andando (m)
        pendienteMax = 50,       -- grados
    }
}

local MITAD = 0.9    -- media altura de la capsula
local RADIO = 0.3
local G = 9.81

local function suave(t) t = Mathf.clamp01(t); return t * t * (3 - 2 * t) end
-- Yaw (grados, 0 = mirar a -Z) <-> direccion horizontal.
local function delante(yaw) local r = math.rad(yaw); return Vec3(-math.sin(r), 0, -math.cos(r)) end
local function derecha(yaw) local r = math.rad(yaw); return Vec3(math.cos(r), 0, -math.sin(r)) end
local function yawDe(d) return math.deg(math.atan(-d.x, -d.z)) end
local function plano(v) return Vec3(v.x, 0, v.z) end

function Personaje:Start()
    self.pivote = self.entity:find("Pivote")
    self.modelo = self.entity:find("Modelo")
    self.camara = Scene.find("Main Camera")
    self.inicio = self.entity.position
    self.yaw = self.pivote.rotation.y
    self.animX, self.animY = 0, 0
    self.energia = self.energiaMax
    self.agotado = false
    self.enSuelo = true
    self.aire = 0
    self.suelo = nil        -- lo que pisa (tabla de Physics.raycast)
    self.salto = nil        -- en curso: { anim = segundos de la animacion, fase }
    self.giroSitio = nil    -- girar en el sitio: { desde, grados, dur, t }
    self.mano = nil         -- accionando una palanca
    self.apuntando = false
    self.cristales = 0
    self.pesoMirar = 0
    self.modelo:setFootGrounding(true)
    self.modelo:setIKWeight("look", 0)
end

-- Yaw de la camara (la orbital lo guarda en su script).
function Personaje:yawCamara()
    local c = self.camara and self.camara:getScript()
    if c and c.yaw then return c.yaw end
    return yawDe(plano(self.camara and self.camara.forward or Vec3(0, 0, -1)))
end

-- El suelo bajo un punto (rayo desde `alto` por encima de los pies).
function Personaje:sueloEn(p, alto, largo)
    local pies = self.entity.position.y - MITAD
    return Physics.raycast(Vec3(p.x, pies + alto, p.z), Vec3.down, largo)
end

function Personaje:pisable(hit)
    return hit ~= nil and hit.normal.y >= math.cos(math.rad(self.pendienteMax))
end

function Personaje:Update(dt)
    if dt <= 0 then return end
    local pos = self.entity.position
    local v = self.entity.velocity

    -- Suelo: un rayo desde justo debajo de la capsula (desde dentro la tocaria
    -- a ella) y cuatro alrededor para los bordes.
    local bajo = Physics.raycast(pos + Vec3(0, -MITAD - 0.02, 0), Vec3.down, 0.5)
    self.suelo = bajo
    local tocando = bajo ~= nil and bajo.distance < 0.08
    if not tocando and v.y <= 0.5 then
        for _, o in ipairs({ Vec3(RADIO, 0, 0), Vec3(-RADIO, 0, 0), Vec3(0, 0, RADIO), Vec3(0, 0, -RADIO) }) do
            local h = Physics.raycast(pos + o * 1.2 + Vec3(0, -MITAD + 0.1, 0), Vec3.down, 0.2)
            if h then tocando = true break end
        end
    end
    local antes = self.enSuelo
    self.enSuelo = tocando and (self.salto == nil or self.salto.fase ~= "subiendo")
    self.aire = self.enSuelo and 0 or self.aire + dt

    local yawCam = self:yawCamara()
    self.apuntando = Input.getMouseButton(1) and self.mano == nil
    local ix, iy = Input.getAxis("Horizontal"), Input.getAxis("Vertical")
    local entrada = derecha(yawCam) * ix + delante(yawCam) * iy
    if entrada:length() > 1 then entrada = entrada:normalized() end
    local quiere = entrada:length() > 0.1 and self.mano == nil and self.giroSitio == nil

    -- Energia: correr la gasta; agotado no se corre hasta recuperar un 30 %.
    local corre = Input.getKey("shift") and quiere and not self.agotado
    if corre and plano(v):length() > 2.5 then
        self.energia = math.max(0, self.energia - self.gastoCorrer * dt)
        if self.energia <= 0 then self.agotado = true end
    else
        self.energia = math.min(self.energiaMax, self.energia + self.recuperar * dt)
        if self.agotado and self.energia > self.energiaMax * 0.3 then self.agotado = false end
    end

    -- Velocidad deseada en el plano, segun el modo.
    local objetivo = Vec3.zero
    if quiere then
        if self.apuntando then
            -- El cuerpo mira con la camara: delante, de lado o de espaldas
            -- (andando: el clip de andar al reves).
            self.yaw = Mathf.moveTowardsAngle(self.yaw, yawCam, self.giro * dt)
            local lx = entrada.x * derecha(self.yaw).x + entrada.z * derecha(self.yaw).z
            local ly = entrada.x * delante(self.yaw).x + entrada.z * delante(self.yaw).z
            local vy = ly >= 0 and (corre and self.velCorrer or self.velAndar) or self.velAndar
            local vx = corre and self.velLateralCorrer or self.velLateral
            objetivo = derecha(self.yaw) * (lx * vx) + delante(self.yaw) * (ly * vy)
        else
            -- Libre: gira hacia donde va (mas rapido parado) y avanza.
            local deseado = yawDe(entrada)
            local rapidez = plano(v):length()
            self.yaw = Mathf.moveTowardsAngle(self.yaw, deseado, self.giro * (rapidez < 1 and 1.4 or 1) * dt)
            local alineado = Mathf.clamp01(1 - math.abs(Mathf.deltaAngle(self.yaw, deseado)) / 120)
            objetivo = delante(self.yaw) * (entrada:length() * (corre and self.velCorrer or self.velAndar) * (0.35 + 0.65 * alineado))
        end
    end
    if not self.enSuelo then objetivo = plano(v):lerp(objetivo, 0.35) end  -- poco control en el aire

    local k = Mathf.clamp01(self.aceleracion * dt * (self.enSuelo and 1 or 0.3))
    v.x = Mathf.lerp(v.x, objetivo.x, k)
    v.z = Mathf.lerp(v.z, objetivo.z, k)

    -- Escalones: el suelo un poco por delante, si esta algo mas alto y no
    -- mas que `escalon`, sube la capsula (no hace falta saltar).
    -- (Mientras sube un escalon cuenta como en el suelo y no se pega al de abajo.)
    self.escalonT = math.max(0, (self.escalonT or 0) - dt)
    local horizontal = plano(v)
    local saltando = self.salto ~= nil and self.salto.fase ~= "agachado"
    if (self.enSuelo or self.escalonT > 0) and not saltando and horizontal:length() > 0.2 then
        local d = horizontal:normalized()
        local delanteP = pos + d * (RADIO + 0.18)
        local hit = self:sueloEn(delanteP, self.escalon + 0.05, self.escalon + 0.1)
        if self:pisable(hit) then
            local alto = hit.point.y - (pos.y - MITAD)
            if alto > 0.03 and alto <= self.escalon then
                self.escalonT = 0.25
                pos.y = pos.y + math.min(alto, (3.0 + horizontal:length()) * dt)
                if v.y < 0 then v.y = 0 end
                self.entity.position = pos
            end
        end
    end
    if self.escalonT > 0 then self.enSuelo = true end
    -- Bajando (escaleras, rampas): pegado al suelo si estaba en el.
    if antes and not self.salto and self.escalonT <= 0 and v.y <= 0.1 and bajo and bajo.distance > 0.03 and bajo.distance < self.escalon
        and self:pisable(bajo) then
        pos.y = pos.y - bajo.distance
        self.entity.position = pos
        v.y = 0
        self.enSuelo = true
    end

    self:saltar(dt, v)
    self:girarEnSitio(dt, yawCam, quiere)
    self:accionar(dt)
    self.entity.velocity = v
    self.pivote.rotation = Vec3(0, self.yaw, 0)

    -- Animator: la velocidad en los ejes del cuerpo, suavizada.
    local real = plano(self.entity.velocity)
    local ax = real.x * derecha(self.yaw).x + real.z * derecha(self.yaw).z
    local ay = real.x * delante(self.yaw).x + real.z * delante(self.yaw).z
    local s = Mathf.clamp01(10 * dt)
    self.animX = Mathf.lerp(self.animX, ax, s)
    self.animY = Mathf.lerp(self.animY, ay, s)
    self.modelo:setAnimatorFloat("X", self.animX)
    self.modelo:setAnimatorFloat("Y", self.animY)
    self.modelo:setAnimatorBool("Moviendo", quiere)
    self.modelo:setAnimatorBool("EnSuelo", self.enSuelo)

    self:mirar(dt)
    if pos.y < -20 then self:Reaparecer() end
end

-- Salto sincronizado con la animacion: el agachado va rapido, despega justo
-- en el fotograma en que la animacion deja el suelo y el vuelo dura lo que
-- dure la fisica (la animacion se estira o se para) hasta tocar suelo.
function Personaje:saltar(dt, v)
    if Input.getKeyDown("space") and self.enSuelo and self.salto == nil and self.mano == nil then
        self.salto = { anim = 0, fase = "agachado", moviendo = plano(v):length() > 1 }
        self.giroSitio = nil
        self.modelo:setAnimatorTrigger("Saltar")
    end
    local s = self.salto
    if s == nil then return end
    local vuelo = 2 * math.sqrt(2 * G * self.alturaSalto) / G
    local velocidad = 1
    if s.fase == "agachado" then
        velocidad = s.moviendo and 3.2 or 1.8
        if s.anim >= self.despegue then
            v.y = math.sqrt(2 * G * self.alturaSalto)
            s.fase = "subiendo"
            s.t = 0
        end
    elseif s.fase == "subiendo" then
        s.t = s.t + dt
        velocidad = (self.aterrizaje - self.despegue) / vuelo
        if s.anim >= self.aterrizaje - 0.06 then velocidad = 0 end  -- sigue en el aire: se espera
        if s.t > 0.15 and v.y <= 0 then s.fase = "cayendo" end
    elseif s.fase == "cayendo" then
        velocidad = s.anim >= self.aterrizaje - 0.06 and 0 or (self.aterrizaje - self.despegue) / vuelo
        if self.enSuelo then s.fase = "aterrizando" end
    elseif s.fase == "aterrizando" then
        s.anim = math.max(s.anim, self.aterrizaje)
        velocidad = plano(v):length() > 0.5 and 2.5 or 1.4
        if s.anim >= self.duracionSalto * 0.98 or (plano(v):length() > 0.5 and s.anim > self.aterrizaje + 0.15) then
            self.salto = nil
            self.modelo:setField("Animator", "speed", 1)
            return
        end
    end
    s.anim = s.anim + dt * velocidad
    self.modelo:setField("Animator", "speed", velocidad)
end

-- Apuntando y quieto: si la camara se aleja del cuerpo, gira en el sitio con
-- las animaciones de girar 90 o 180 grados (su giro se quito de la
-- animacion al importarla: lo hace este script, a la vez).
function Personaje:girarEnSitio(dt, yawCam, quiere)
    local g = self.giroSitio
    if g then
        g.t = g.t + dt * g.vel
        self.yaw = g.desde + g.grados * suave(g.t / g.dur)
        if g.t >= g.dur or quiere or self.salto then
            if g.t < g.dur then self.yaw = g.desde + g.grados * suave(g.t / g.dur) end
            self.giroSitio = nil
            self.modelo:setField("Animator", "speed", 1)
        end
        return
    end
    if not self.apuntando or quiere or self.salto or not self.enSuelo or self.mano then return end
    local diff = Mathf.deltaAngle(self.yaw, yawCam)
    if math.abs(diff) < 70 then return end
    local grande = math.abs(diff) > 135
    local izq = diff > 0
    local trigger = grande and (izq and "GirarIzq180" or "GirarDer180") or (izq and "GirarIzq90" or "GirarDer90")
    local dur = grande and self.durGiro180 or self.durGiro90
    -- La animacion va algo mas rapida (girar 180 dura 1.6 s) y el giro real
    -- es el que falta hasta la camara, con la forma de la animacion.
    local vel = 1.5
    self.giroSitio = { desde = self.yaw, grados = diff, dur = dur, t = 0, vel = vel }
    self.modelo:setField("Animator", "speed", vel)
    self.modelo:setAnimatorTrigger(trigger)
end

-- E cerca de una palanca: se pone delante, la mano derecha va al pomo (IK),
-- la palanca baja con la mano agarrada y la mano vuelve.
function Personaje:accionar(dt)
    if self.mano == nil then
        if not Input.getKeyDown("e") or self.salto then return end
        local cerca = self:palancaCerca()
        if cerca == nil then return end
        local p = cerca:getScript()
        if p.usada then return end
        -- Delante de la palanca y algo a su lado: la mano derecha queda frente al pomo.
        self.mano = { palanca = cerca, t = 0, hecho = false,
                      sitio = cerca.position + cerca.forward * 0.55 + cerca.right * 0.2,
                      yaw = yawDe(plano(cerca.forward * -1)) }
        self.modelo:setIKTarget("right_hand", cerca:find("Pomo"))
        self.modelo:setIKWeight("right_hand", 0)
        return
    end
    local m = self.mano
    m.t = m.t + dt
    -- Colocarse (0..0.35 s), alcanzar (..0.6), bajar la palanca (..1.1), soltar (..1.45).
    local ir = suave(m.t / 0.35)
    local destino = Vec3(m.sitio.x, self.entity.position.y, m.sitio.z)
    local p = self.entity.position
    self.entity.position = Vec3(Mathf.lerp(p.x, destino.x, ir), p.y, Mathf.lerp(p.z, destino.z, ir))
    self.yaw = Mathf.lerpAngle(self.yaw, m.yaw, ir)
    local peso = 0
    if m.t < 0.6 then peso = suave((m.t - 0.15) / 0.45)
    elseif m.t < 1.1 then peso = 1
    else peso = 1 - suave((m.t - 1.1) / 0.35) end
    self.modelo:setIKWeight("right_hand", peso)
    if m.t >= 0.6 and not m.hecho then
        m.hecho = true
        m.palanca:getScript():Accionar()
    end
    if m.t >= 1.45 then
        self.modelo:setIKTarget("right_hand", nil)
        self.mano = nil
    end
end

function Personaje:palancaCerca()
    local mejor, dist = nil, 1.8
    for _, e in ipairs(Scene.findAllWithTag("Palanca")) do
        local d = self.entity.position:distance(e.position)
        if d < dist and not e:getScript().usada then mejor, dist = e, d end
    end
    return mejor
end

-- Mirar (IK de la cabeza): apuntando, a donde apunta la camara; si no, a lo
-- interesante que tenga delante (palancas, cristales, la estatua).
function Personaje:mirar(dt)
    local objetivo = nil
    if self.apuntando and self.camara then
        objetivo = self.camara.position + self.camara.forward * 30
    else
        -- El mas cercano de delante; el que ya se mira sigue ganando salvo que
        -- otro este 1 m mas cerca (con dos objetos juntos no se alterna).
        local mejor, elegido = 8, nil
        local cabeza = self.entity.position + Vec3(0, 0.7, 0)
        for _, tag in ipairs({ "Interes", "Cristal", "Palanca" }) do
            for _, e in ipairs(Scene.findAllWithTag(tag)) do
                if e.active then
                    local a = e.position - cabeza
                    local d = a:length()
                    local frente = (a.x * delante(self.yaw).x + a.z * delante(self.yaw).z) / math.max(d, 0.01)
                    if frente > 0.25 then
                        if self.mirando and e.name == self.mirando then d = d - 1.0 end
                        if d < mejor then mejor, elegido = d, e end
                    end
                end
            end
        end
        self.mirando = elegido and elegido.name or nil
        objetivo = elegido and elegido.position or nil
    end
    local quiere = objetivo ~= nil and self.mano == nil and 1 or 0
    self.pesoMirar = Mathf.moveTowards(self.pesoMirar, quiere, 2.5 * dt)
    if objetivo then self.modelo:setLookAt(objetivo) end
    self.modelo:setIKWeight("look", self.pesoMirar * 0.85)
end

function Personaje:Reaparecer()
    self.entity.position = self.inicio
    self.entity.velocity = Vec3.zero
    self.salto = nil
    self.modelo:setField("Animator", "speed", 1)
end

function Personaje:Recoger()
    self.cristales = self.cristales + 1
end

-- Estado para la interfaz y la depuracion (F1).
function Personaje:estado()
    if self.mano then return "palanca" end
    if self.salto then return "salto (" .. self.salto.fase .. ")" end
    if self.giroSitio then return "girando en el sitio" end
    if not self.enSuelo then return "en el aire" end
    return self.apuntando and "apuntando" or "libre"
end

return Personaje
)lua";

constexpr const char* kOrbitCamera = R"lua(-- Camara orbital en tercera persona con brazo de muelle: el raton la gira
-- (clic en la vista para capturarlo, Esc para soltarlo), la rueda la acerca y
-- no atraviesa paredes (se acerca si algo tapa al personaje). Al apuntar
-- (clic derecho) se pone sobre el hombro y cierra el campo de vision; al
-- correr lo abre un poco.
local CamaraOrbital = {
    properties = {
        objetivo = "Jugador",
        distancia = 3.6,
        altura = 0.55,        -- sobre el centro del personaje
        hombro = 0.55,        -- desplazamiento lateral al apuntar
        sensibilidad = 0.12,
        fov = 60,
    }
}

function CamaraOrbital:Start()
    self.target = Scene.find(self.objetivo)
    self.yaw = 0
    self.pitch = 12
    self.actual = self.distancia
    self.lado = 0
    self.campo = self.fov
    self.foco = self.target and self.target.position or Vec3.zero
    Input.lockCursor(true)
end

function CamaraOrbital:LateUpdate(dt)
    if not self.target then return end
    if Input.getKeyDown("escape") then Input.lockCursor(false) end
    if Input.getMouseButtonDown(0) and not Input.isCursorLocked() then Input.lockCursor(true) end
    if Input.isCursorLocked() or Input.getMouseButton(1) then
        local d = Input.mouseDelta()
        self.yaw = self.yaw - d.x * self.sensibilidad
        self.pitch = Mathf.clamp(self.pitch + d.y * self.sensibilidad, -35, 70)
    end
    self.distancia = Mathf.clamp(self.distancia - Input.getAxis("Mouse ScrollWheel") * 0.5, 1.5, 9)

    local jugador = self.target:getScript()
    local apunta = jugador and jugador.apuntando
    local corre = jugador and self.target.velocity:length() > 3.2
    self.lado = Mathf.lerp(self.lado, apunta and self.hombro or 0, Mathf.clamp01(8 * dt))
    local distancia = apunta and math.min(self.distancia, 2.0) or self.distancia
    self.campo = Mathf.lerp(self.campo, apunta and self.fov - 15 or (corre and self.fov + 6 or self.fov), Mathf.clamp01(5 * dt))
    self.entity:setField("Camera", "fov", self.campo)

    -- El foco sigue al personaje con algo de retraso en vertical (escalones).
    local p = self.target.position + Vec3(0, self.altura, 0)
    self.foco = Vec3(p.x, Mathf.lerp(self.foco.y, p.y, Mathf.clamp01(10 * dt)), p.z)
    local yaw, pitch = math.rad(self.yaw), math.rad(self.pitch)
    local atras = Vec3(math.sin(yaw) * math.cos(pitch), math.sin(pitch), math.cos(yaw) * math.cos(pitch))
    local derecha = Vec3(math.cos(yaw), 0, -math.sin(yaw))
    local foco = self.foco + derecha * self.lado

    -- Brazo de muelle: si algo hay entre el foco y la camara, se acerca.
    local deseada = distancia
    local inicio = foco + atras * 0.45   -- fuera de la capsula del personaje
    local hit = Physics.raycast(inicio, atras, distancia)
    if hit then deseada = math.max(0.6, hit.distance + 0.45 - 0.25) end
    if deseada < self.actual then self.actual = deseada
    else self.actual = Mathf.lerp(self.actual, deseada, Mathf.clamp01(4 * dt)) end

    self.entity.position = foco + atras * self.actual
    self.entity:lookAt(foco)
end

return CamaraOrbital
)lua";

constexpr const char* kLever = R"lua(-- Palanca: la acciona el personaje (E) con la mano. El brazo baja y abre la
-- compuerta que diga `compuerta` (sube y deja pasar).
local Palanca = {
    properties = {
        compuerta = "Compuerta 1",
        subir = 3.2,       -- metros que sube la compuerta
        duracion = 1.6,
    }
}

function Palanca:Start()
    self.brazo = self.entity:find("Brazo")
    self.puerta = Scene.find(self.compuerta)
    self.usada = false
    self.t = -1
    if self.puerta then self.base = self.puerta.position end
end

function Palanca:Accionar()
    if self.usada then return end
    self.usada = true
    self.t = 0
end

function Palanca:Update(dt)
    if self.t < 0 then return end
    self.t = self.t + dt
    -- El brazo baja hacia el personaje en medio segundo (la mano va agarrada
    -- a su pomo).
    local a = Mathf.clamp01(self.t / 0.5)
    self.brazo.rotation = Vec3(Mathf.lerp(10, -35, a * a * (3 - 2 * a)), 0, 0)
    if self.puerta then
        local s = Mathf.clamp01((self.t - 0.3) / self.duracion)
        self.puerta.position = self.base + Vec3(0, self.subir * s * s * (3 - 2 * s), 0)
    end
    if self.t > self.duracion + 0.5 then self.t = -1 end
end

return Palanca
)lua";

constexpr const char* kCrystal = R"lua(-- Cristal: gira, flota y se recoge al acercarse el personaje.
local Cristal = {}

function Cristal:Start()
    self.base = self.entity.position
    self.fase = self.base.x * 0.7 + self.base.z * 0.3
    self.jugador = Scene.find("Jugador")
end

function Cristal:Update(dt)
    self.entity:rotate(Vec3(0, 90 * dt, 0))
    self.entity.position = self.base + Vec3(0, math.sin(Time.time * 2 + self.fase) * 0.12, 0)
    if self.jugador and self.jugador.position:distance(self.entity.position) < 1.1 then
        self.jugador:getScript():Recoger()
        self.entity.active = false
    end
end

return Cristal
)lua";

constexpr const char* kInterface = R"lua(-- Interfaz: cristales, energia, el aviso de la palanca y la depuracion (F1:
-- estado, velocidades del Animator y del cuerpo).
local Interfaz = {}

function Interfaz:Start()
    self.jugador = Scene.find("Jugador")
    self.marcador = self.entity:find("Marcador")
    self.energia = self.entity:find("Energia")
    self.aviso = self.entity:find("Aviso")
    self.depuracion = self.entity:find("Depuracion")
    self.total = #Scene.findAllWithTag("Cristal")
    self.verDepuracion = false
    self.depuracion.active = false
end

function Interfaz:Update(dt)
    local j = self.jugador and self.jugador:getScript()
    if not j or j.cristales == nil then return end  -- (su Start aun no ha corrido)
    self.marcador.text = "Cristales  " .. j.cristales .. " / " .. self.total
    if j.cristales >= self.total and self.total > 0 then
        self.marcador.text = "Todos los cristales!  " .. j.cristales .. " / " .. self.total
    end
    self.energia.value = j.energia / j.energiaMax

    local cerca = j.mano == nil and j:palancaCerca()
    self.aviso.text = cerca and "E  accionar la palanca" or ""

    if Input.getKeyDown("f1") then
        self.verDepuracion = not self.verDepuracion
        self.depuracion.active = self.verDepuracion
    end
    if self.verDepuracion then
        local v = self.jugador.velocity
        self.depuracion.text = string.format(
            "Estado: %s\nAnimator  X %.2f  Y %.2f m/s\nVelocidad %.2f m/s  (vertical %.2f)\nEn el suelo: %s   Energia %d",
            j:estado(), j.animX, j.animY, Vec3(v.x, 0, v.z):length(), v.y, tostring(j.enSuelo), math.floor(j.energia))
    end
end

return Interfaz
)lua";

}  // namespace cramion::editor::locomotion_scripts

#endif  // CRAMION_EDITOR_TEMPLATE_LOCOMOTION_SCRIPTS_H

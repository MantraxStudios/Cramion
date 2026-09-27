#ifndef CRAMION_EDITOR_TEMPLATE_OPEN_WORLD_SCRIPTS_H
#define CRAMION_EDITOR_TEMPLATE_OPEN_WORLD_SCRIPTS_H

// Scripts de la plantilla Mundo abierto (prueba de rendimiento).

namespace cramion::editor::openworld {

// HUD de rendimiento y teclas para forzar el motor.
constexpr const char* kPerformanceScript = R"lua(-- Rendimiento.lua: prueba de rendimiento del mundo abierto.
-- Muestra FPS (media, minimo y maximo de los ultimos segundos), ms de CPU y
-- de GPU, arboles dibujados y triangulos. Teclas:
--   1 2 3 4   calidad Baja / Media / Alta / Ultra
--   N / M     menos / mas arboles (la densidad a la mitad o al doble)
--   J / L     distancia de dibujo de los arboles
--   K         sombras de los arboles si / no
--   V         viento si / no
--   T         volar alto (vista del bosque entero) o volver al suelo
--   H         ocultar / mostrar este panel
local Rendimiento = {}

local CALIDADES = { "Baja", "Media", "Alta", "Ultra" }

function Rendimiento:Start()
    self.texto = Scene.find("UI_Rendimiento")
    self.panel = Scene.find("UI_Panel")
    self.bosque = Scene.find("Vegetacion")
    self.jugador = Scene.find("Jugador")
    self.muestras = {}
    self.refresco = 0
    self.suma = 0
    self.aviso = ""
    self.avisoTiempo = 0
end

function Rendimiento:avisar(texto)
    self.aviso = texto
    self.avisoTiempo = 3
end

function Rendimiento:campo(nombre) return self.bosque:getField("Foliage", nombre) end
function Rendimiento:poner(nombre, valor) self.bosque:setField("Foliage", nombre, valor) end

function Rendimiento:teclas()
    for i, calidad in ipairs(CALIDADES) do
        if Input.getKeyDown(tostring(i)) then
            Graphics.setQuality(calidad)
            self:avisar("Calidad " .. calidad)
        end
    end
    if Input.getKeyDown("n") then
        self:poner("density", math.max(5, self:campo("density") / 2))
        self:avisar("Densidad " .. string.format("%.0f", self:campo("density")) .. " arboles/ha (sembrando...)")
    end
    if Input.getKeyDown("m") then
        self:poner("density", math.min(1200, self:campo("density") * 2))
        self:avisar("Densidad " .. string.format("%.0f", self:campo("density")) .. " arboles/ha (sembrando...)")
    end
    if Input.getKeyDown("j") then
        self:poner("max_distance", math.max(300, self:campo("max_distance") - 500))
        self:avisar("Distancia de los arboles " .. self:campo("max_distance") .. " m")
    end
    if Input.getKeyDown("l") then
        self:poner("max_distance", math.min(12000, self:campo("max_distance") + 500))
        self:avisar("Distancia de los arboles " .. self:campo("max_distance") .. " m")
    end
    if Input.getKeyDown("k") then
        self:poner("cast_shadows", not self:campo("cast_shadows"))
        self:avisar(self:campo("cast_shadows") and "Sombras de los arboles: si" or "Sombras de los arboles: no")
    end
    if Input.getKeyDown("v") then
        self:poner("wind", self:campo("wind") > 0 and 0 or 1)
        self:avisar(self:campo("wind") > 0 and "Viento: si" or "Viento: no")
    end
    if Input.getKeyDown("t") and self.jugador then
        local j = self.jugador
        if not self.alto then
            self.suelo = j.position
            j.position = j.position + Vec3(0, 350, 0)
            j.velocity = Vec3.zero
            self.alto = true
            self:avisar("Vista desde 350 m (T para volver)")
        else
            j.position = self.suelo
            j.velocity = Vec3.zero
            self.alto = false
        end
    end
    if self.alto and self.jugador then
        -- Arriba: sin caer (se queda flotando para mirar el bosque).
        self.jugador.velocity = Vec3(self.jugador.velocity.x, 0, self.jugador.velocity.z)
    end
    if Input.getKeyDown("h") then self.panel.active = not self.panel.active end
end

function Rendimiento:Update(dt)
    self:teclas()
    -- Ultimos 3 segundos de frames.
    table.insert(self.muestras, dt)
    self.suma = self.suma + dt
    while self.suma > 3 and #self.muestras > 1 do
        self.suma = self.suma - table.remove(self.muestras, 1)
    end
    if self.avisoTiempo > 0 then self.avisoTiempo = self.avisoTiempo - dt end
    self.refresco = self.refresco - dt
    if self.refresco > 0 then return end
    self.refresco = 0.25

    local peor, mejor = 0, 1e9
    for _, s in ipairs(self.muestras) do
        peor = math.max(peor, s)
        mejor = math.min(mejor, s)
    end
    local media = self.suma / math.max(#self.muestras, 1)
    local arboles = Graphics.get("foliage_trees") or 0
    local visibles = Graphics.get("foliage_visible") or 0
    local cerca = Graphics.get("foliage_near") or 0
    local triangulos = Graphics.get("foliage_triangles") or 0
    local gpu = Graphics.get("gpu_ms") or 0
    local lineas = {
        string.format("FPS %.0f   (min %.0f  max %.0f, ultimos 3 s)", 1 / media, 1 / peor, 1 / mejor),
        string.format("Frame %.2f ms   GPU %.2f ms   calidad %s", media * 1000, gpu, tostring(Graphics.getQuality())),
        string.format("Arboles %s en la isla   dibujados %s (%s con todo el detalle)",
            self:miles(arboles), self:miles(visibles), self:miles(cerca)),
        string.format("Triangulos de los arboles %.1f M   densidad %.0f/ha   distancia %.0f m",
            triangulos / 1e6, self:campo("density"), self:campo("max_distance")),
        "1-4 calidad  N/M arboles  J/L distancia  K sombras  V viento  T vista alta  H panel",
    }
    if self.avisoTiempo > 0 then table.insert(lineas, 1, ">> " .. self.aviso) end
    self.texto.text = table.concat(lineas, "\n")
end

function Rendimiento:miles(n)
    n = math.floor(n + 0.5)
    local s = tostring(n)
    local fuera = s:reverse():gsub("(%d%d%d)", "%1."):reverse()
    return (fuera:gsub("^%.", ""))
end

return Rendimiento
)lua";

}  // namespace cramion::editor::openworld

#endif  // CRAMION_EDITOR_TEMPLATE_OPEN_WORLD_SCRIPTS_H

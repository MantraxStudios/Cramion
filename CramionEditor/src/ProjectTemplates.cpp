// Plantillas de proyecto (ver ProjectTemplates.h). Las integradas construyen
// su escena con el ECS: primitivas con colliders y materiales propios,
// scripts de Lua de verdad (jugador, camara, monedas, guardias) y una
// interfaz sencilla, para que el proyecto se pueda jugar con Play nada mas
// crearlo.

#include "ProjectTemplates.h"
#include "TemplateMmoScripts.h"
#include "TemplateCreatureScripts.h"
#include "TemplateOnlineScripts.h"
#include "TemplateOpenWorldScripts.h"
#include "CreatureModels.h"
#include "LocomotionPack.h"
#include "TemplateLocomotionScripts.h"

#include <CramionCore/ecs/AnimatorController.h>
#include <CramionCore/ai/StateMachine.h>
#include <CramionCore/ecs/Rigging.h>
#include <CramionCore/net/NetworkObject.h>

#include <CramionCore/CramionCore.h>

#include <nlohmann/json.hpp>

#include <CramionFX/asset/ImageFile.h>

#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>

namespace cramion::editor {

using core::Vec2;
using core::Vec3;

namespace {

constexpr std::uint32_t rgba(int r, int g, int b) {
    return static_cast<std::uint32_t>(r) | (static_cast<std::uint32_t>(g) << 8) |
           (static_cast<std::uint32_t>(b) << 16) | 0xFF000000u;
}

// --- Scripts de las plantillas ---------------------------------------------------

constexpr const char* kPlayerScript = R"(-- Jugador en tercera persona: WASD relativo a la camara, Shift para correr
-- y Espacio para saltar. Va sobre un Rigidbody (la fisica lo empuja y lo
-- hace caer); su hijo "Modelo" mira hacia donde camina.
local Jugador = {
    properties = {
        velocidad = 6.0,
        correr = 1.7,       -- multiplicador con Shift
        salto = 6.5,        -- m/s hacia arriba
        aceleracion = 12.0,
    }
}

function Jugador:Start()
    self.inicio = self.entity.position
    self.camara = Scene.find("Main Camera")
    self.modelo = self.entity:find("Modelo")
end

function Jugador:enSuelo()
    -- Desde justo debajo de la capsula (un rayo desde dentro la tocaria a ella).
    return Physics.raycast(self.entity.position + Vec3(0, -1.02, 0), Vec3.down, 0.25) ~= nil
end

function Jugador:Respawn()
    self.entity.position = self.inicio
    self.entity.velocity = Vec3.zero
end

function Jugador:Update(dt)
    local adelante = Vec3(0, 0, -1)
    local derecha = Vec3(1, 0, 0)
    if self.camara then
        adelante = self.camara.forward
        adelante.y = 0
        adelante = adelante:normalized()
        derecha = self.camara.right
        derecha.y = 0
        derecha = derecha:normalized()
    end
    local direccion = derecha * Input.getAxis("Horizontal") + adelante * Input.getAxis("Vertical")
    if direccion:length() > 1 then direccion = direccion:normalized() end

    local rapidez = self.velocidad
    if Input.getKey("shift") then rapidez = rapidez * self.correr end
    local objetivo = direccion * rapidez
    local v = self.entity.velocity
    local t = Mathf.clamp01(self.aceleracion * dt)
    v.x = Mathf.lerp(v.x, objetivo.x, t)
    v.z = Mathf.lerp(v.z, objetivo.z, t)
    if Input.getKeyDown("space") and self:enSuelo() then
        v.y = self.salto
    end
    self.entity.velocity = v

    if self.modelo and direccion:length() > 0.1 then
        self.modelo:lookAt(self.modelo.position + direccion)
    end
    if self.entity.position.y < -20 then self:Respawn() end
end

return Jugador
)";

constexpr const char* kCameraScript = R"(-- Camara en tercera persona: sigue al objetivo desde detras. Clic derecho y
-- raton para girarla, rueda para acercarla.
local CamaraTercera = {
    properties = {
        objetivo = "Jugador",
        distancia = 7.0,
        altura = 1.2,        -- punto al que mira, sobre el objetivo
        inclinacion = 18.0,  -- grados
        sensibilidad = 0.25,
        suavizado = 12.0,
    }
}

function CamaraTercera:Start()
    self.target = Scene.find(self.objetivo)
    self.yaw = 0
    self.pitch = self.inclinacion
end

function CamaraTercera:LateUpdate(dt)
    if not self.target then return end
    if Input.getMouseButton(1) then
        local d = Input.mouseDelta()
        self.yaw = self.yaw - d.x * self.sensibilidad
        self.pitch = Mathf.clamp(self.pitch + d.y * self.sensibilidad, -5, 75)
    end
    self.distancia = Mathf.clamp(self.distancia - Input.getAxis("Mouse ScrollWheel"), 3, 25)

    local yaw = math.rad(self.yaw)
    local pitch = math.rad(self.pitch)
    local foco = self.target.position + Vec3(0, self.altura, 0)
    local detras = Vec3(math.sin(yaw) * math.cos(pitch), math.sin(pitch), math.cos(yaw) * math.cos(pitch))
    local deseada = foco + detras * self.distancia
    self.entity.position = self.entity.position:lerp(deseada, Mathf.clamp01(self.suavizado * dt))
    self.entity:lookAt(foco)
end

return CamaraTercera
)";

constexpr const char* kCoinScript = R"(-- Moneda: flota, gira y se recoge al tocarla (su collider es un trigger).
local Moneda = { properties = { valor = 1 } }

function Moneda:Start()
    self.base = self.entity.position
    self.fase = self.base.x + self.base.z
end

function Moneda:Update(dt)
    self.entity:rotate(Vec3(0, 120 * dt, 0))
    self.entity.position = self.base + Vec3(0, math.sin(Time.time * 2.5 + self.fase) * 0.15, 0)
end

function Moneda:OnTriggerEnter(other)
    if other.name ~= "Jugador" then return end
    local marcador = Scene.find("Marcador")
    if marcador then marcador:getScript():Sumar(self.valor) end
    self.entity:destroy()
end

return Moneda
)";

constexpr const char* kScoreScript = R"(-- Marcador del HUD: cuenta las monedas recogidas y guarda el record.
local Marcador = { properties = {} }

function Marcador:Start()
    self.puntos = 0
    self.total = #Scene.findAllWithTag("Moneda")
    self:pintar()
end

function Marcador:pintar()
    self.entity.text = string.format("Monedas  %d / %d", self.puntos, self.total)
end

function Marcador:Sumar(n)
    self.puntos = self.puntos + n
    self:pintar()
    if self.puntos >= self.total then
        self.entity.text = "¡Todas las monedas!"
        self.entity.color = Vec3(1.0, 0.85, 0.25)
        Prefs.setInt("record", math.max(Prefs.getInt("record", 0), self.puntos))
    end
end

return Marcador
)";

constexpr const char* kGuardScript = R"(-- Guardia con NavAgent: patrulla por puntos al azar de la malla de
-- navegacion y persigue al jugador si lo ve (sin paredes en medio). Si lo
-- alcanza, el jugador vuelve al inicio.
local Guardia = {
    properties = {
        radioPatrulla = 10.0,
        vision = 11.0,
        alcance = 1.3,
    }
}

function Guardia:Start()
    self.casa = self.entity.position
    self.jugador = Scene.find("Jugador")
    self.hud = Scene.find("Marcador")
    self.estado = "patrulla"
    self.espera = 0
end

function Guardia:veAlJugador()
    if not self.jugador then return false end
    if self.entity:distanceTo(self.jugador) > self.vision then return false end
    local libre = Navigation.raycast(self.entity.position, self.jugador.position)
    return libre
end

function Guardia:Update(dt)
    if self.jugador and self.entity:distanceTo(self.jugador) < self.alcance then
        self.jugador:getScript():Respawn()
        if self.hud then self.hud:getScript():Atrapado() end
        self.entity:stopMoving()
        self.estado = "patrulla"
        return
    end

    if self:veAlJugador() then
        self.estado = "persigue"
        self.entity:moveTo(self.jugador.position)
        return
    end

    if self.estado == "persigue" then
        -- Lo perdio de vista: va a donde lo vio por ultima vez.
        if not self.entity.isMoving then self.estado = "patrulla" end
        return
    end

    if not self.entity.isMoving then
        self.espera = self.espera - dt
        if self.espera <= 0 then
            local punto = Navigation.randomPoint(self.casa, self.radioPatrulla)
            if punto then self.entity:moveTo(punto) end
            self.espera = Mathf.random(0.5, 2.5)
        end
    end
end

return Guardia
)";

constexpr const char* kGoalScript = R"(-- La meta: al llegar se gana y el nivel empieza de nuevo a los 3 segundos.
local Meta = { properties = {} }

function Meta:Update(dt)
    self.entity:rotate(Vec3(0, 45 * dt, 0))
    if self.reinicio then
        self.reinicio = self.reinicio - dt
        if self.reinicio <= 0 then Scene.load(Scene.name()) end
    end
end

function Meta:OnTriggerEnter(other)
    if other.name ~= "Jugador" or self.reinicio then return end
    local hud = Scene.find("Marcador")
    if hud then hud:getScript():Ganar() end
    self.reinicio = 3
end

return Meta
)";

constexpr const char* kEscapeHudScript = R"(-- HUD del modo escapa: veces atrapado y el mensaje final.
local Marcador = { properties = {} }

function Marcador:Start()
    self.atrapado = 0
    self:pintar()
end

function Marcador:pintar()
    self.entity.text = string.format("Llega a la meta sin que te atrapen  ·  Atrapado: %d", self.atrapado)
end

function Marcador:Atrapado()
    self.atrapado = self.atrapado + 1
    self:pintar()
end

function Marcador:Ganar()
    self.entity.text = "¡Lo lograste!"
    self.entity.color = Vec3(0.4, 1.0, 0.5)
end

return Marcador
)";

constexpr const char* kVoxelPlayerScript = R"lua(-- Jugador de un mundo de bloques en primera persona (como Minecraft).
--
-- Clic en la vista para jugar (captura el raton). WASD andar, Espacio saltar
-- o nadar, Ctrl correr. Clic izquierdo (mantener) rompe, derecho pone el
-- bloque elegido o come, central copia (creativo). Rueda o 1-9 eligen. E abre
-- el inventario y el crafteo, Escape la pausa. En creativo, F vuela.
--
-- Supervivencia: vida, hambre y aire; dano por caida y al ahogarse; los
-- bloques sueltan objetos que se recogen; la piedra y las menas piden un
-- pico. El mundo, la posicion, el inventario y la vida se guardan solos.
local Jugador = {
    properties = {
        mundo = "Mi mundo",
        semilla = 0,          -- 0 = al azar al crear el mundo
        creativo = false,     -- romper al instante, volar, bloques sin fin
        velocidad = 4.3,
        correr = 1.5,
        salto = 8.4,
        gravedad = 28.0,
        sensibilidad = 0.12,
        alcance = 5.0,
    }
}

local MITAD = Vec3(0.3, 0.9, 0.3)  -- la caja del jugador (0.6 x 1.8 m)
local OJOS = 0.72                  -- del centro de la caja a los ojos (1.62 m del suelo)
local HUECOS = 36                  -- 1..9 la barra, 10..36 la mochila
local PILA = 64
local ICONOS = "Voxel/Iconos/"

-- Objetos que no son bloques.
local OBJETOS = {
    palo = { label = "Palo" },
    carbon = { label = "Carbón" },
    manzana = { label = "Manzana", comida = 4 },
    pico_madera = { label = "Pico de madera", pico = 2.5, max = 1 },
    pico_piedra = { label = "Pico de piedra", pico = 5.0, max = 1, fuerte = true },
}

-- Crafteo. `mesa`: hace falta una mesa de trabajo a menos de 4 bloques.
local RECETAS = {
    { da = "oak_planks", n = 4, pide = { { "oak_log", 1 } } },
    { da = "oak_planks", n = 4, pide = { { "birch_log", 1 } } },
    { da = "oak_planks", n = 4, pide = { { "spruce_log", 1 } } },
    { da = "palo", n = 4, pide = { { "oak_planks", 2 } } },
    { da = "crafting_table", n = 1, pide = { { "oak_planks", 4 } } },
    { da = "torch", n = 4, pide = { { "palo", 1 }, { "carbon", 1 } } },
    { da = "pico_madera", n = 1, pide = { { "oak_planks", 3 }, { "palo", 2 } }, mesa = true },
    { da = "pico_piedra", n = 1, pide = { { "cobblestone", 3 }, { "palo", 2 } }, mesa = true },
    { da = "stone_bricks", n = 4, pide = { { "cobblestone", 4 } }, mesa = true },
    { da = "sandstone", n = 1, pide = { { "sand", 4 } } },
    { da = "glass", n = 1, pide = { { "sand", 2 }, { "carbon", 1 } }, mesa = true },
    { da = "bricks", n = 1, pide = { { "clay", 4 } }, mesa = true },
    { da = "glowstone", n = 1, pide = { { "torch", 4 }, { "glass", 1 } }, mesa = true },
}

-- Bloques de la barra en creativo.
local CREATIVO = { "grass", "dirt", "stone", "cobblestone", "oak_planks", "oak_log", "glass", "torch", "bricks" }

-- ------------------------------------------------------------------ objetos

local function esBloque(item) return OBJETOS[item] == nil and Voxel.blockId(item) ~= 0 end

local function nombre(item)
    if OBJETOS[item] then return OBJETOS[item].label end
    local info = Voxel.blockInfo(item)
    return info and info.label or item
end

local function icono(item) return ICONOS .. item .. ".png" end

local function maximo(item) return (OBJETOS[item] and OBJETOS[item].max) or PILA end

-- Bloques de piedra: sin pico tardan mucho y no sueltan nada.
local function dePiedra(info)
    local n = info.name
    return string.find(n, "stone") ~= nil or string.find(n, "_ore") ~= nil or string.find(n, "brick") ~= nil
end

local function colorObjeto(item)
    if esBloque(item) then return Voxel.blockColor(item) end
    local colores = { palo = Vec3(0.55, 0.38, 0.2), carbon = Vec3(0.12, 0.12, 0.13), manzana = Vec3(0.85, 0.12, 0.1),
                      pico_madera = Vec3(0.6, 0.45, 0.25), pico_piedra = Vec3(0.55, 0.55, 0.57) }
    return colores[item] or Vec3(1, 1, 1)
end

-- ------------------------------------------------------------------ inicio

function Jugador:Start()
    self.yaw, self.pitch = 0, -10
    self.vel = Vec3.zero
    self.volando = false
    self.elegido = 1
    self.progreso = 0
    self.guardado = 0
    self.vida, self.hambre, self.aire = 20, 20, 10
    self.relojHambre, self.relojVida, self.relojAire = 0, 0, 0
    self.flash = 0
    self.aviso, self.relojAviso = "", 0
    self.relojNombre = 0
    self.inv = {}
    self.objetos, self.chispas = {}, {}
    self.mallas = {}
    self:buscarInterfaz()

    -- El mundo guardado con ese nombre, o uno nuevo.
    if not Voxel.loadWorld(self.mundo) then
        local semilla = self.semilla
        if semilla == 0 then semilla = math.random(1, 2000000000) end
        Voxel.newWorld(self.mundo, semilla)
        Voxel.setMeta("mode", self.creativo and "creativo" or "supervivencia")
    end
    local x, y, z, yaw, pitch = string.match(Voxel.getMeta("player", ""), "(%S+) (%S+) (%S+) (%S+) (%S+)")
    if x then
        self.centro = Vec3(tonumber(x), tonumber(y), tonumber(z))
        self.yaw, self.pitch = tonumber(yaw), tonumber(pitch)
    else
        self.centro = Vec3(0.5, Voxel.surfaceHeight(0, 0) + 1 + MITAD.y + 0.1, 0.5)
    end
    self.inicio = Vec3(0.5, Voxel.surfaceHeight(0, 0) + 1 + MITAD.y + 0.1, 0.5)
    self.vida = tonumber(Voxel.getMeta("vida", "20")) or 20
    self.hambre = tonumber(Voxel.getMeta("hambre", "20")) or 20
    self:cargarInventario()
    self.caidaDesde = self.centro.y

    -- Contorno del bloque apuntado y grietas al romper (mallas por codigo).
    self.contorno = Scene.create("Contorno")
    local marco = Mesh.wireCube(1.002, 0.014)
    marco:setMaterial(0, { color = Vec3(0.02, 0.02, 0.02), roughness = 1 })
    self.contorno.mesh = marco
    self.contorno.castShadows = false
    self.contorno.active = false
    self.grietas = Scene.create("Grietas")
    self.grietas.castShadows = false
    self.grietas.active = false
    self.etapas = {}
    for i = 0, 9 do
        local m = Mesh.cube(1.006)
        m:setMaterial(0, { texture = "Voxel/Grietas/grieta_" .. i .. ".png", roughness = 1 })
        self.etapas[i] = m
    end

    self:colocarCamara()
    self:pintarTodo()
end

function Jugador:buscarInterfaz()
    self.ui = {
        info = Scene.find("Info"), aviso = Scene.find("Aviso"), nombre = Scene.find("NombreObjeto"),
        seleccion = Scene.find("Seleccion"), dano = Scene.find("Dano"), inventario = Scene.find("Inventario"),
        muerte = Scene.find("Muerte"), pausa = Scene.find("Pausa"),
    }
    self.ui.barra, self.ui.corazones, self.ui.comida, self.ui.burbujas, self.ui.huecos, self.ui.recetas = {}, {}, {}, {}, {}, {}
    for i = 1, 9 do self.ui.barra[i] = Scene.find("Hueco " .. i) end
    for i = 1, 10 do
        self.ui.corazones[i] = Scene.find("Corazon " .. i)
        self.ui.comida[i] = Scene.find("Comida " .. i)
        self.ui.burbujas[i] = Scene.find("Aire " .. i)
    end
    for i = 1, HUECOS do self.ui.huecos[i] = Scene.find("Inv " .. i) end
    for i = 1, #RECETAS do self.ui.recetas[i] = Scene.find("Receta " .. i) end
end

-- ------------------------------------------------------------------ cada frame

function Jugador:Update(dt)
    dt = math.min(dt, 0.05)
    self:efectos(dt)
    local panel = self:panelAbierto()
    -- Teclas de los paneles.
    if Input.getKeyDown("escape") then
        if self.ui.inventario and self.ui.inventario.active then self:abrirInventario(false)
        elseif not self.muerto then self:pausar(not (self.ui.pausa and self.ui.pausa.active)) end
        return
    end
    if Input.getKeyDown("e") and not self.muerto and not (self.ui.pausa and self.ui.pausa.active) then
        self:abrirInventario(not (self.ui.inventario and self.ui.inventario.active))
        return
    end
    if panel then return end

    -- Raton: clic en la vista lo captura.
    local mirando = Input.isCursorLocked()
    if not mirando then
        if Input.getMouseButtonDown(0) then Input.lockCursor(true) end
    else
        local d = Input.mouseDelta()
        self.yaw = self.yaw - d.x * self.sensibilidad
        self.pitch = Mathf.clamp(self.pitch - d.y * self.sensibilidad, -89, 89)
    end
    -- Mientras se genera el suelo, quieto (no se cae del mundo).
    if not Voxel.isReady(self.centro) then
        self:colocarCamara()
        self:pintarInfo("Generando el mundo...")
        return
    end
    if not self.listo then
        -- Si aparece dentro de un arbol o de una colina, sube hasta el aire.
        while Voxel.boxCollides(self.centro, MITAD) and self.centro.y < 250 do
            self.centro = self.centro + Vec3(0, 1, 0)
        end
        self.caidaDesde = self.centro.y
        self.listo = true
    end
    self:mover(dt)
    self:colocarCamara()
    if mirando then self:bloques(dt) else self:apuntar(nil) end
    self:elegir()
    self:estadisticas(dt)
    self:recoger(dt)
    self:pintarInfo()
    -- Guardado automatico cada minuto.
    self.guardado = self.guardado + dt
    if self.guardado > 60 then
        self.guardado = 0
        self:guardar()
    end
end

function Jugador:panelAbierto()
    return self.muerto or (self.ui.inventario and self.ui.inventario.active) or (self.ui.pausa and self.ui.pausa.active)
end

function Jugador:mover(dt)
    local yaw = math.rad(self.yaw)
    local adelante = Vec3(-math.sin(yaw), 0, -math.cos(yaw))
    local derecha = Vec3(math.cos(yaw), 0, -math.sin(yaw))
    local dir = adelante * Input.getAxis("Vertical") + derecha * Input.getAxis("Horizontal")
    if dir:length() > 1 then dir = dir:normalized() end
    local rapidez = self.velocidad
    self.corriendo = Input.getKey("ctrl") and dir:length() > 0.1 and self.hambre > 6
    if self.corriendo then rapidez = rapidez * self.correr end
    if self.creativo and Input.getKeyDown("f") then
        self.volando = not self.volando
        self.vel = Vec3.zero
    end
    local agua = Voxel.inWater(self.centro)
    local v = self.vel
    if self.volando then
        rapidez = rapidez * 2.5
        v = dir * rapidez
        if Input.getKey("space") then v.y = rapidez
        elseif Input.getKey("shift") then v.y = -rapidez
        else v.y = 0 end
    else
        if agua then rapidez = rapidez * 0.5 end
        local t = Mathf.clamp01((self.enSuelo and 14 or 3) * dt)
        v.x = Mathf.lerp(v.x, dir.x * rapidez, t)
        v.z = Mathf.lerp(v.z, dir.z * rapidez, t)
        if agua then
            local objetivo = Input.getKey("space") and 3.5 or -2.0
            v.y = Mathf.lerp(v.y, objetivo, Mathf.clamp01(4 * dt))
        else
            v.y = math.max(v.y - self.gravedad * dt, -50)
            if self.enSuelo and Input.getKey("space") then
                v.y = self.salto
                self:cansar(0.05)
            end
        end
    end
    local antes = self.centro
    local pos, suelo, techo = Voxel.moveBox(self.centro, MITAD, v * dt)
    if suelo and v.y < 0 then v.y = 0 end
    if techo and v.y > 0 then v.y = 0 end
    -- Dano por caida: desde el punto mas alto del salto, 1 por metro pasados 3.
    if self.volando or agua then self.caidaDesde = pos.y
    elseif not suelo then self.caidaDesde = math.max(self.caidaDesde, pos.y)
    elseif not self.enSuelo then
        local metros = self.caidaDesde - pos.y
        if metros > 3.5 and not self.creativo then self:herir(math.floor(metros - 3), "Caíste desde muy alto") end
        self.caidaDesde = pos.y
    else
        self.caidaDesde = pos.y
    end
    self.enSuelo = suelo
    self.centro = pos
    self.vel = v
    local paso = Vec3(pos.x - antes.x, 0, pos.z - antes.z):length()
    self:cansar(paso * (self.corriendo and 0.1 or 0.01))
    if self.centro.y < -20 then self:herir(40, "Te caíste del mundo") end
end

function Jugador:colocarCamara()
    self.entity.position = self.centro + Vec3(0, OJOS, 0)
    self.entity.rotation = Vec3(self.pitch, self.yaw, 0)
end

-- El bloque en `p` (esquina) se solapa con el jugador.
function Jugador:ocupa(p)
    local c = self.centro
    return p.x < c.x + MITAD.x and p.x + 1 > c.x - MITAD.x and p.y < c.y + MITAD.y and p.y + 1 > c.y - MITAD.y
        and p.z < c.z + MITAD.z and p.z + 1 > c.z - MITAD.z
end

-- ------------------------------------------------------------------ bloques

function Jugador:apuntar(golpe)
    self.apuntado = golpe
    if golpe then
        self.contorno.position = golpe.block + Vec3(0.5, 0.5, 0.5)
        self.contorno.active = true
    else
        self.contorno.active = false
        self.grietas.active = false
    end
end

function Jugador:herramienta()
    local hueco = self.inv[self.elegido]
    return hueco and OBJETOS[hueco.item] and OBJETOS[hueco.item].pico and OBJETOS[hueco.item] or nil
end

-- Segundos para romper un bloque con lo que se lleva en la mano.
function Jugador:tiempoRomper(info)
    local t = info.hardness
    if dePiedra(info) then
        local pico = self:herramienta()
        if pico then t = t / pico.pico else t = t * 5 end
    end
    return math.max(t, 0.05)
end

-- Lo que suelta un bloque (nil = nada).
function Jugador:suelta(info)
    local n = info.name
    if string.find(n, "leaves") then return math.random() < 0.08 and "manzana" or nil end
    if n == "tall_grass" or n == "red_flower" or n == "yellow_flower" or n == "dead_bush" or n == "glass" or n == "ice" then
        return nil
    end
    if dePiedra(info) then
        local pico = self:herramienta()
        if not pico then return nil end
        if (n == "iron_ore" or n == "gold_ore" or n == "diamond_ore") and not pico.fuerte then return nil end
    end
    if n == "coal_ore" then return "carbon" end
    local drop = Voxel.blockInfo(info.drop)
    return drop and drop.name or n
end

function Jugador:romper(b, info)
    Voxel.setBlock(b.x, b.y, b.z, "air")
    self:chispear(b, info)
    self:cansar(0.005)
    if not self.creativo then
        local item = self:suelta(info)
        if item then self:soltarObjeto(item, 1, b + Vec3(0.5, 0.5, 0.5)) end
    end
end

function Jugador:bloques(dt)
    local golpe = Voxel.raycast(self.entity.position, self.entity.forward, self.alcance)
    self:apuntar(golpe)
    -- Romper: al instante en creativo; si no, manteniendo segun la dureza y la herramienta.
    if golpe and Input.getMouseButton(0) then
        local info = Voxel.blockInfo(golpe.id)
        local b = golpe.block
        local clave = b.x .. "," .. b.y .. "," .. b.z
        if self.rompiendo ~= clave then
            self.rompiendo = clave
            self.progreso = 0
        end
        if info.hardness >= 0 then
            if self.creativo then
                if Input.getMouseButtonDown(0) then self:romper(b, info) end
            else
                self.progreso = self.progreso + dt / self:tiempoRomper(info)
                if self.progreso >= 1 then
                    self:romper(b, info)
                    self.progreso = 0
                    self.rompiendo = nil
                end
            end
        end
    else
        self.rompiendo = nil
        self.progreso = 0
    end
    -- Grietas sobre el bloque segun lo que falta para romperlo.
    if golpe and self.progreso > 0 then
        self.grietas.mesh = self.etapas[math.min(9, math.floor(self.progreso * 10))]
        self.grietas.position = golpe.block + Vec3(0.5, 0.5, 0.5)
        self.grietas.active = true
    else
        self.grietas.active = false
    end
    -- Clic derecho: comer lo que se lleva o poner el bloque junto a la cara apuntada.
    if Input.getMouseButtonDown(1) then
        local hueco = self.inv[self.elegido]
        local comida = hueco and OBJETOS[hueco.item] and OBJETOS[hueco.item].comida
        if comida then
            if self.hambre < 20 then
                self.hambre = math.min(20, self.hambre + comida)
                self:quitar(self.elegido, 1)
                self:mostrarAviso("Ñam")
            end
        elseif golpe and hueco and esBloque(hueco.item) then
            local p = golpe.block + golpe.normal
            local info = Voxel.blockInfo(hueco.item)
            if not (info.solid and self:ocupa(p)) and Voxel.setBlock(p.x, p.y, p.z, hueco.item) then
                if not self.creativo then self:quitar(self.elegido, 1) end
            end
        end
    end
    -- Copiar el bloque apuntado a la barra (creativo).
    if self.creativo and golpe and Input.getMouseButtonDown(2) then
        self.inv[self.elegido] = { item = Voxel.blockInfo(golpe.id).name, n = PILA }
        self:pintarTodo()
    end
end

function Jugador:elegir()
    local antes = self.elegido
    for i = 1, 9 do
        if Input.getKeyDown(tostring(i)) then self.elegido = i end
    end
    local rueda = Input.getAxis("Mouse ScrollWheel")
    if rueda > 0 then self.elegido = self.elegido - 1 elseif rueda < 0 then self.elegido = self.elegido + 1 end
    if self.elegido < 1 then self.elegido = 9 elseif self.elegido > 9 then self.elegido = 1 end
    if antes ~= self.elegido then
        self.relojNombre = 2
        self:pintarBarra()
    end
end

-- ------------------------------------------------------------------ inventario

-- Mete `n` de `item`; devuelve los que no caben.
function Jugador:dar(item, n)
    local max = maximo(item)
    for i = 1, HUECOS do
        local h = self.inv[i]
        if n <= 0 then break end
        if h and h.item == item and h.n < max then
            local cabe = math.min(n, max - h.n)
            h.n = h.n + cabe
            n = n - cabe
        end
    end
    for i = 1, HUECOS do
        if n <= 0 then break end
        if not self.inv[i] then
            local cabe = math.min(n, max)
            self.inv[i] = { item = item, n = cabe }
            n = n - cabe
        end
    end
    self:pintarTodo()
    return n
end

function Jugador:quitar(hueco, n)
    local h = self.inv[hueco]
    if not h then return end
    h.n = h.n - n
    if h.n <= 0 then self.inv[hueco] = nil end
    self:pintarTodo()
end

function Jugador:cuenta(item)
    local total = 0
    for i = 1, HUECOS do
        local h = self.inv[i]
        if h and h.item == item then total = total + h.n end
    end
    return total
end

function Jugador:gastar(item, n)
    for i = HUECOS, 1, -1 do
        local h = self.inv[i]
        if n <= 0 then break end
        if h and h.item == item then
            local toma = math.min(n, h.n)
            h.n = h.n - toma
            n = n - toma
            if h.n <= 0 then self.inv[i] = nil end
        end
    end
end

-- Hay una mesa de trabajo a menos de 4 bloques.
function Jugador:cercaDeMesa()
    local c = self.centro
    local cx, cy, cz = math.floor(c.x), math.floor(c.y), math.floor(c.z)
    local mesa = Voxel.blockId("crafting_table")
    for x = cx - 4, cx + 4 do
        for y = cy - 2, cy + 2 do
            for z = cz - 4, cz + 4 do
                if Voxel.getBlock(x, y, z) == mesa then return true end
            end
        end
    end
    return false
end

function Jugador:puedeCraftear(receta)
    if self.creativo then return true end
    for _, p in ipairs(receta.pide) do
        if self:cuenta(p[1]) < p[2] then return false end
    end
    return not receta.mesa or self:cercaDeMesa()
end

function Jugador:craftear(i)
    local receta = RECETAS[i]
    if not receta then return false end
    if receta.mesa and not self.creativo and not self:cercaDeMesa() then
        self:mostrarAviso("Necesitas una mesa de trabajo cerca")
        return false
    end
    if not self:puedeCraftear(receta) then
        self:mostrarAviso("Te faltan materiales")
        return false
    end
    if not self.creativo then
        for _, p in ipairs(receta.pide) do self:gastar(p[1], p[2]) end
    end
    local sobra = self:dar(receta.da, receta.n)
    if sobra > 0 then self:soltarObjeto(receta.da, sobra, self.centro + Vec3(0, 0.5, 0)) end
    self:mostrarAviso("+" .. receta.n .. " " .. nombre(receta.da))
    return true
end

function Jugador:abrirInventario(abrir)
    if not self.ui.inventario then return end
    self.ui.inventario.active = abrir
    Input.lockCursor(not abrir)
    if abrir then
        self.mesaCerca = self:cercaDeMesa()
        self:pintarTodo()
    end
end

function Jugador:pausar(pausa)
    if not self.ui.pausa then return end
    self.ui.pausa.active = pausa
    Input.lockCursor(not pausa)
end

-- Botones de la interfaz (UIButton -> metodo del script).
function Jugador:OnReceta(boton)
    local i = tonumber(string.match(boton.name, "(%d+)"))
    if i then
        self:craftear(i)
        self.mesaCerca = self:cercaDeMesa()
        self:pintarTodo()
    end
end

-- Clic en un hueco de la mochila: lo cambia por el elegido de la barra.
function Jugador:OnHueco(boton)
    local i = tonumber(string.match(boton.name, "(%d+)"))
    if not i or i == self.elegido then return end
    self.inv[i], self.inv[self.elegido] = self.inv[self.elegido], self.inv[i]
    self:pintarTodo()
end

function Jugador:OnReaparecer()
    self.muerto = false
    self.vida, self.hambre, self.aire = 20, 20, 10
    self.centro = self.inicio
    self.vel = Vec3.zero
    self.caidaDesde = self.centro.y
    self.listo = false
    if self.ui.muerte then self.ui.muerte.active = false end
    Input.lockCursor(true)
    self:pintarTodo()
end

function Jugador:OnSeguir() self:pausar(false) end

function Jugador:OnGuardar()
    self:guardar()
    self:mostrarAviso("Mundo guardado")
end

function Jugador:OnSalir()
    self:guardar()
    Game.quit()
end

-- ------------------------------------------------------------------ vida y hambre

function Jugador:cansar(cantidad)
    if self.creativo then return end
    self.cansancio = (self.cansancio or 0) + cantidad
    if self.cansancio >= 4 then
        self.cansancio = self.cansancio - 4
        self.hambre = math.max(0, self.hambre - 1)
    end
end

function Jugador:herir(puntos, causa)
    if self.creativo or self.muerto or puntos <= 0 then return end
    self.vida = math.max(0, self.vida - puntos)
    self.flash = 0.45
    if self.vida <= 0 then
        self.muerto = true
        if self.ui.muerte then
            self.ui.muerte.active = true
            local texto = self.ui.muerte:find("Causa")
            if texto then texto.text = causa or "" end
        end
        Input.lockCursor(false)
    end
    self:pintarEstado()
end

function Jugador:estadisticas(dt)
    if self.creativo then return end
    -- Aire: con la cabeza en el agua se gasta; sin aire, dano.
    if Voxel.inWater(self.entity.position) then
        self.aire = self.aire - dt
        if self.aire <= 0 then
            self.relojAire = self.relojAire + dt
            if self.relojAire >= 1 then
                self.relojAire = 0
                self:herir(2, "Te ahogaste")
            end
        end
    else
        self.aire = math.min(10, self.aire + dt * 5)
        self.relojAire = 0
    end
    -- Con hambre llena se cura; sin comida se pierde vida (hasta medio corazon).
    self.relojVida = self.relojVida + dt
    if self.relojVida >= 4 then
        self.relojVida = 0
        if self.hambre >= 18 and self.vida < 20 then
            self.vida = self.vida + 1
            self:cansar(1.5)
        elseif self.hambre <= 0 and self.vida > 1 then
            self:herir(1, "Te moriste de hambre")
        end
    end
    -- El hambre baja despacio aunque no se haga nada.
    self.relojHambre = self.relojHambre + dt
    if self.relojHambre >= 40 then
        self.relojHambre = 0
        self:cansar(4)
    end
    self:pintarEstado()
end

-- ------------------------------------------------------------------ objetos sueltos y chispas

function Jugador:mallaDe(item, tamano)
    local clave = item .. tamano
    if not self.mallas[clave] then
        local m = Mesh.cube(tamano)
        m:setMaterial(0, { color = colorObjeto(item), roughness = 0.8 })
        self.mallas[clave] = m
    end
    return self.mallas[clave]
end

function Jugador:soltarObjeto(item, n, pos)
    local e = Scene.create("Objeto", pos)
    e.mesh = self:mallaDe(item, 0.25)
    table.insert(self.objetos, { e = e, item = item, n = n, pos = pos, vel = Vec3(Mathf.random(-1.5, 1.5), 3.5, Mathf.random(-1.5, 1.5)), t = 0 })
end

-- Trocitos del bloque que saltan al romperlo.
function Jugador:chispear(b, info)
    local malla = self:mallaDe(info.name, 0.09)
    for _ = 1, 10 do
        local p = b + Vec3(Mathf.random(0.2, 0.8), Mathf.random(0.2, 0.8), Mathf.random(0.2, 0.8))
        local e = Scene.create("Chispa", p)
        e.mesh = malla
        e.castShadows = false
        table.insert(self.chispas, { e = e, pos = p, vel = Vec3(Mathf.random(-2, 2), Mathf.random(1, 4), Mathf.random(-2, 2)),
                                     vida = Mathf.random(0.5, 0.9) })
    end
end

function Jugador:efectos(dt)
    for i = #self.chispas, 1, -1 do
        local c = self.chispas[i]
        c.vida = c.vida - dt
        if c.vida <= 0 or not c.e:valid() then
            if c.e:valid() then c.e:destroy() end
            table.remove(self.chispas, i)
        else
            c.vel.y = c.vel.y - 20 * dt
            local pos, suelo = Voxel.moveBox(c.pos, Vec3(0.045, 0.045, 0.045), c.vel * dt)
            if suelo then c.vel = c.vel * 0.5; c.vel.y = 0 end
            c.pos = pos
            c.e.position = pos
            local s = math.min(1, c.vida * 2)
            c.e.scale = Vec3(s, s, s)
        end
    end
    -- Mensajes y golpes en pantalla.
    self.flash = math.max(0, self.flash - dt)
    if self.ui.dano then self.ui.dano.alpha = self.flash * 0.8 end
    self.relojAviso = math.max(0, self.relojAviso - dt)
    if self.ui.aviso then self.ui.aviso.alpha = math.min(1, self.relojAviso) end
    self.relojNombre = math.max(0, self.relojNombre - dt)
    if self.ui.nombre then self.ui.nombre.alpha = math.min(1, self.relojNombre) end
end

-- Los objetos sueltos caen, flotan girando y se recogen al acercarse.
function Jugador:recoger(dt)
    for i = #self.objetos, 1, -1 do
        local o = self.objetos[i]
        o.t = o.t + dt
        local hacia = self.centro - o.pos
        local lejos = hacia:length()
        if o.t > 0.5 and lejos < 1.4 then
            local sobra = self:dar(o.item, o.n)
            if sobra <= 0 then
                o.e:destroy()
                table.remove(self.objetos, i)
                goto siguiente
            end
            o.n = sobra
        end
        if o.t > 0.5 and lejos < 3 then
            o.vel = o.vel:lerp(hacia:normalized() * 6, Mathf.clamp01(8 * dt))  -- iman
        else
            o.vel.y = o.vel.y - 20 * dt
            o.vel.x, o.vel.z = o.vel.x * 0.96, o.vel.z * 0.96
        end
        do
            local pos, suelo = Voxel.moveBox(o.pos, Vec3(0.125, 0.125, 0.125), o.vel * dt)
            if suelo then o.vel = Vec3(0, 0, 0) end
            o.pos = pos
            o.e.position = pos + Vec3(0, 0.1 + math.sin(o.t * 3) * 0.06, 0)
            o.e.rotation = Vec3(0, o.t * 90, 0)
        end
        if o.t > 300 then
            o.e:destroy()
            table.remove(self.objetos, i)
        end
        ::siguiente::
    end
end

-- ------------------------------------------------------------------ interfaz

function Jugador:mostrarAviso(texto)
    if self.ui.aviso then self.ui.aviso.text = texto end
    self.relojAviso = 2.5
end

local function pintarHueco(hueco, dato, infinito)
    if not hueco then return end
    local ic, num = hueco:find("Icono"), hueco:find("Cantidad")
    if dato then
        if ic then ic.texture = icono(dato.item); ic.alpha = 1 end
        if num then num.text = (dato.n > 1 and not infinito) and tostring(dato.n) or "" end
    else
        if ic then ic.alpha = 0 end
        if num then num.text = "" end
    end
end

function Jugador:pintarBarra()
    for i = 1, 9 do pintarHueco(self.ui.barra[i], self.inv[i], self.creativo) end
    if self.ui.seleccion then self.ui.seleccion.uiPosition = Vec3((self.elegido - 5) * 84, -20, 0) end
    local dato = self.inv[self.elegido]
    if self.ui.nombre then self.ui.nombre.text = dato and nombre(dato.item) or "" end
end

function Jugador:pintarEstado()
    local creativo = self.creativo
    for i = 1, 10 do
        local c, f, b = self.ui.corazones[i], self.ui.comida[i], self.ui.burbujas[i]
        local v, h = self.vida - (i - 1) * 2, self.hambre - (i - 1) * 2
        if c then
            c.active = not creativo
            c.texture = ICONOS .. (v >= 2 and "corazon.png" or (v == 1 and "corazon_medio.png" or "corazon_vacio.png"))
        end
        if f then
            f.active = not creativo
            f.texture = ICONOS .. (h >= 2 and "comida.png" or (h == 1 and "comida_media.png" or "comida_vacia.png"))
        end
        if b then b.active = not creativo and self.aire < 10 and i <= math.ceil(self.aire) end
    end
end

function Jugador:pintarInventario()
    if not (self.ui.inventario and self.ui.inventario.active) then return end
    for i = 1, HUECOS do pintarHueco(self.ui.huecos[i], self.inv[i], self.creativo) end
    for i, receta in ipairs(RECETAS) do
        local boton = self.ui.recetas[i]
        if boton then
            local partes = {}
            for _, p in ipairs(receta.pide) do partes[#partes + 1] = p[2] .. " " .. nombre(p[1]) end
            local texto = receta.n .. " " .. nombre(receta.da) .. "   ←   " .. table.concat(partes, " + ")
            if receta.mesa then texto = texto .. "   (mesa)" end
            local t = boton:find("Texto")
            if t then t.text = texto end
            local ic = boton:find("Icono")
            if ic then ic.texture = icono(receta.da) end
            boton.interactable = self.creativo or (self:puedeCraftearSinMesa(receta) and (not receta.mesa or self.mesaCerca))
        end
    end
end

function Jugador:puedeCraftearSinMesa(receta)
    for _, p in ipairs(receta.pide) do
        if self:cuenta(p[1]) < p[2] then return false end
    end
    return true
end

function Jugador:pintarTodo()
    self:pintarBarra()
    self:pintarEstado()
    self:pintarInventario()
end

function Jugador:pintarInfo(aviso)
    if not self.ui.info then return end
    local c = self.centro
    local texto = string.format("%s  ·  %s  ·  %d, %d, %d", Voxel.worldName(), self.creativo and "creativo" or "supervivencia",
                                math.floor(c.x), math.floor(c.y - MITAD.y), math.floor(c.z))
    if self.volando then texto = texto .. "  ·  volando (F)" end
    if aviso then texto = texto .. "\n" .. aviso end
    if self.apuntado then
        texto = texto .. "\n" .. Voxel.blockInfo(self.apuntado.id).label
        if self.progreso > 0 then texto = texto .. string.format("  %d%%", math.floor(self.progreso * 100)) end
    end
    if not Input.isCursorLocked() and not self:panelAbierto() then
        texto = texto .. "\nClic para jugar  ·  E inventario  ·  Escape pausa"
    end
    self.ui.info.text = texto
end

-- ------------------------------------------------------------------ guardar

function Jugador:cargarInventario()
    self.inv = {}
    if self.creativo then
        for i, b in ipairs(CREATIVO) do self.inv[i] = { item = b, n = PILA } end
        -- La mochila con todos los bloques que se pueden poner.
        local i = 10
        for id = 1, Voxel.blockCount() - 1 do
            local info = Voxel.blockInfo(id)
            if info.placeable and i <= HUECOS then
                self.inv[i] = { item = info.name, n = PILA }
                i = i + 1
            end
        end
        return
    end
    for hueco, item, n in string.gmatch(Voxel.getMeta("inventario", ""), "(%d+)=([%w_]+):(%d+)") do
        self.inv[tonumber(hueco)] = { item = item, n = tonumber(n) }
    end
end

function Jugador:guardar()
    local c = self.centro
    if c then
        Voxel.setMeta("player", string.format("%.2f %.2f %.2f %.1f %.1f", c.x, c.y, c.z, self.yaw, self.pitch))
    end
    if not self.creativo then
        local partes = {}
        for i = 1, HUECOS do
            local h = self.inv[i]
            if h then partes[#partes + 1] = i .. "=" .. h.item .. ":" .. h.n end
        end
        Voxel.setMeta("inventario", table.concat(partes, ","))
        Voxel.setMeta("vida", tostring(self.vida))
        Voxel.setMeta("hambre", tostring(self.hambre))
    end
    Voxel.saveWorld()
end

function Jugador:OnDestroy()
    self:guardar()
end

return Jugador
)lua";

// --- Construccion de escenas -------------------------------------------------------

struct Builder {
    project::ProjectInfo& project;
    ecs::World world;

    explicit Builder(project::ProjectInfo& p) : project(p) {}

    assets::AssetRef material(const std::string& name, const Vec3& color, float roughness, float metallic = 0.0f,
                              const Vec3& emissive = Vec3{}, float emissive_intensity = 1.0f) {
        assets::MaterialAsset m;
        // El color del Inspector es sRGB; el del material, lineal.
        m.base_color = core::Vec4{std::pow(color.x, 2.2f), std::pow(color.y, 2.2f), std::pow(color.z, 2.2f), 1.0f};
        m.roughness = roughness;
        m.metallic = metallic;
        m.emissive = emissive;
        m.emissive_intensity = emissive_intensity;
        const std::filesystem::path path = project.assetsFolder() / "Materials" / (name + ".crmat");
        std::filesystem::create_directories(path.parent_path());
        std::string error;
        if (!assets::saveMaterial(m, path, &error)) throw std::runtime_error("material " + name + ": " + error);
        return assets::AssetRef{m.uuid, assets::AssetType::Material};
    }

    void script(const std::string& file, const char* code) {
        const std::filesystem::path path = project.assetsFolder() / "Scripts" / file;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        out << code;
        if (!out) throw std::runtime_error("no se pudo escribir " + file);
    }

    static void attach(ecs::Entity e, const std::string& file,
                       std::vector<scripting::ScriptProperty> properties = {}) {
        scripting::Script& s = e.add<scripting::Script>();
        s.file = "Scripts/" + file;
        s.properties = std::move(properties);
    }

    ecs::Entity box(const std::string& name, const Vec3& position, const Vec3& size, const assets::AssetRef& mat,
                    const Vec3& euler = Vec3{}, ecs::Entity parent = {}) {
        ecs::Entity e = ecs::createPrimitive(world, assets::builtin::kCube, name, parent);
        e.setLocalPosition(position);
        e.setLocalEulerDegrees(euler);
        e.setLocalScale(size);
        e.get<ecs::MeshRenderer>().materials = {mat};
        e.add<physics::BoxCollider>();  // la caja unidad escalada por el Transform
        return e;
    }

    ecs::Entity player(const Vec3& spawn, const assets::AssetRef& body, const assets::AssetRef& visor) {
        ecs::Entity p = world.create("Jugador");
        p.setWorldPosition(spawn);
        physics::CapsuleCollider& capsule = p.add<physics::CapsuleCollider>();
        capsule.radius = 0.45f;
        capsule.height = 1.9f;
        capsule.material.friction = 0.0f;  // no se engancha en las paredes
        physics::Rigidbody& rb = p.add<physics::Rigidbody>();
        rb.mass = 70.0f;
        rb.lock_rotation_x = rb.lock_rotation_y = rb.lock_rotation_z = true;
        rb.continuous = true;
        attach(p, "Jugador.lua");
        ecs::Entity model = world.create("Modelo", p);
        ecs::Entity mesh = ecs::createPrimitive(world, assets::builtin::kCapsule, "Cuerpo", model);
        mesh.setLocalScale(Vec3{0.9f, 0.95f, 0.9f});
        mesh.get<ecs::MeshRenderer>().materials = {body};
        ecs::Entity eyes = ecs::createPrimitive(world, assets::builtin::kCube, "Visor", model);
        eyes.setLocalPosition(Vec3{0.0f, 0.45f, -0.36f});
        eyes.setLocalScale(Vec3{0.62f, 0.22f, 0.22f});
        eyes.get<ecs::MeshRenderer>().materials = {visor};
        return p;
    }

    void camera(float distance, float pitch) {
        ecs::Entity cam = world.findByName("Main Camera");
        cam.setWorldPosition(Vec3{0.0f, 5.0f, 12.0f});
        attach(cam, "CamaraTercera.lua",
               {{"distancia", scripting::PropertyType::Number, std::to_string(distance)},
                {"inclinacion", scripting::PropertyType::Number, std::to_string(pitch)}});
    }

    ecs::Entity hud(const std::string& script_file) {
        ecs::Entity canvas = world.create("HUD");
        canvas.add<ui::Canvas>();
        ecs::Entity text = world.create("Marcador", canvas);
        ui::RectTransform& rt = text.add<ui::RectTransform>();
        rt.anchor_min = Vec2{0.0f, 0.0f};
        rt.anchor_max = Vec2{0.0f, 0.0f};
        rt.pivot = Vec2{0.0f, 0.0f};
        rt.position = Vec2{40.0f, 32.0f};
        rt.size = Vec2{1100.0f, 60.0f};
        ui::Text& t = text.add<ui::Text>();
        t.text = "";
        t.font_size = 38.0f;
        t.h_align = ui::HAlign::Left;
        t.shadow = true;
        attach(text, script_file);
        return text;
    }

    void ring(float half, float height, float thickness, const assets::AssetRef& mat) {
        box("Muro Norte", Vec3{0.0f, height * 0.5f, -half}, Vec3{half * 2.0f + thickness, height, thickness}, mat);
        box("Muro Sur", Vec3{0.0f, height * 0.5f, half}, Vec3{half * 2.0f + thickness, height, thickness}, mat);
        box("Muro Este", Vec3{half, height * 0.5f, 0.0f}, Vec3{thickness, height, half * 2.0f}, mat);
        box("Muro Oeste", Vec3{-half, height * 0.5f, 0.0f}, Vec3{thickness, height, half * 2.0f}, mat);
    }

    void save(const std::string& scene_name) {
        world.setSceneName(scene_name);
        const std::filesystem::path path = project.assetsFolder() / "Scenes" / (scene_name + ".crscene");
        std::filesystem::create_directories(path.parent_path());
        std::string error;
        if (!ecs::saveScene(world, path, &error)) throw std::runtime_error("escena: " + error);
        project.startup_scene = world.sceneUuid();
    }
};

void buildBlank(project::ProjectInfo& project) {
    Builder b(project);
    b.world.setSceneUuid(Uuid::generate());
    ecs::populateDefaultScene(b.world);
    const assets::AssetRef floor = b.material("Suelo", Vec3{0.62f, 0.63f, 0.65f}, 0.85f);
    b.box("Suelo", Vec3{0.0f, -0.5f, 0.0f}, Vec3{40.0f, 1.0f, 40.0f}, floor);
    b.save("Main");
}

void buildThirdPerson(project::ProjectInfo& project) {
    Builder b(project);
    b.world.setSceneUuid(Uuid::generate());
    ecs::populateDefaultScene(b.world);
    b.script("Jugador.lua", kPlayerScript);
    b.script("CamaraTercera.lua", kCameraScript);
    b.script("Moneda.lua", kCoinScript);
    b.script("Marcador.lua", kScoreScript);

    const assets::AssetRef floor = b.material("Suelo", Vec3{0.58f, 0.60f, 0.63f}, 0.9f);
    const assets::AssetRef wall = b.material("Muro", Vec3{0.34f, 0.37f, 0.42f}, 0.8f);
    const assets::AssetRef orange = b.material("Plataforma naranja", Vec3{0.95f, 0.52f, 0.18f}, 0.55f);
    const assets::AssetRef blue = b.material("Plataforma azul", Vec3{0.16f, 0.50f, 0.92f}, 0.45f);
    const assets::AssetRef body = b.material("Jugador", Vec3{0.10f, 0.72f, 0.95f}, 0.35f);
    const assets::AssetRef visor = b.material("Visor", Vec3{0.05f, 0.06f, 0.08f}, 0.15f, 0.6f);
    const assets::AssetRef gold = b.material("Moneda", Vec3{1.0f, 0.78f, 0.22f}, 0.25f, 1.0f,
                                             Vec3{1.0f, 0.65f, 0.1f}, 0.6f);

    b.box("Suelo", Vec3{0.0f, -0.5f, 0.0f}, Vec3{60.0f, 1.0f, 60.0f}, floor);
    b.ring(30.0f, 2.5f, 1.0f, wall);
    // Escalera de bloques y una rampa hasta una plataforma alta.
    for (int i = 0; i < 5; ++i) {
        const float h = 0.5f * static_cast<float>(i + 1);
        b.box("Escalon " + std::to_string(i + 1), Vec3{8.0f + 2.5f * static_cast<float>(i), h * 0.5f, -6.0f},
              Vec3{2.5f, h, 3.0f}, i % 2 == 0 ? orange : blue);
    }
    b.box("Plataforma alta", Vec3{-10.0f, 2.5f, -12.0f}, Vec3{8.0f, 0.6f, 8.0f}, blue);
    b.box("Rampa", Vec3{-10.0f, 1.2f, -3.2f}, Vec3{3.0f, 0.4f, 10.0f}, orange, Vec3{-14.5f, 0.0f, 0.0f});
    b.box("Columna 1", Vec3{-14.0f, 2.0f, 10.0f}, Vec3{1.5f, 4.0f, 1.5f}, wall);
    b.box("Columna 2", Vec3{-6.0f, 2.0f, 14.0f}, Vec3{1.5f, 4.0f, 1.5f}, wall);
    b.box("Bloque", Vec3{12.0f, 1.0f, 12.0f}, Vec3{4.0f, 2.0f, 4.0f}, blue, Vec3{0.0f, 30.0f, 0.0f});

    const Vec3 coins[] = {{8.0f, 1.4f, -6.0f},   {13.0f, 2.4f, -6.0f}, {18.0f, 3.4f, -6.0f}, {-10.0f, 3.6f, -12.0f},
                          {-12.0f, 3.6f, -14.0f}, {12.0f, 3.0f, 12.0f}, {-10.0f, 1.0f, 10.0f}, {4.0f, 1.0f, 18.0f}};
    int n = 0;
    for (const Vec3& p : coins) {
        ecs::Entity c = ecs::createPrimitive(b.world, assets::builtin::kCylinder, "Moneda " + std::to_string(++n));
        c.setWorldPosition(p);
        c.setLocalEulerDegrees(Vec3{90.0f, 0.0f, 0.0f});
        c.setLocalScale(Vec3{0.6f, 0.08f, 0.6f});
        c.get<ecs::MeshRenderer>().materials = {gold};
        c.setTag("Moneda");
        physics::SphereCollider& trigger = c.add<physics::SphereCollider>();
        trigger.radius = 0.9f;
        trigger.material.is_trigger = true;
        Builder::attach(c, "Moneda.lua");
    }

    b.player(Vec3{0.0f, 1.1f, 8.0f}, body, visor);
    b.camera(7.0f, 18.0f);
    b.hud("Marcador.lua");
    b.save("Main");

    std::vector<std::string> tags = ecs::defaultTags();
    tags.push_back("Moneda");
    ecs::saveTags(project.settingsFolder() / "Tags.json", tags);
}

void buildNavigation(project::ProjectInfo& project) {
    Builder b(project);
    b.world.setSceneUuid(Uuid::generate());
    ecs::populateDefaultScene(b.world);
    b.script("Jugador.lua", kPlayerScript);
    b.script("CamaraTercera.lua", kCameraScript);
    b.script("Guardia.lua", kGuardScript);
    b.script("Meta.lua", kGoalScript);
    b.script("Marcador.lua", kEscapeHudScript);

    const assets::AssetRef floor = b.material("Suelo", Vec3{0.46f, 0.50f, 0.44f}, 0.9f);
    const assets::AssetRef wall = b.material("Muro", Vec3{0.72f, 0.70f, 0.66f}, 0.8f);
    const assets::AssetRef crate = b.material("Caja", Vec3{0.62f, 0.42f, 0.24f}, 0.75f);
    const assets::AssetRef body = b.material("Jugador", Vec3{0.10f, 0.72f, 0.95f}, 0.35f);
    const assets::AssetRef visor = b.material("Visor", Vec3{0.05f, 0.06f, 0.08f}, 0.15f, 0.6f);
    const assets::AssetRef guard = b.material("Guardia", Vec3{0.88f, 0.18f, 0.16f}, 0.4f);
    const assets::AssetRef eye = b.material("Ojo", Vec3{1.0f, 0.9f, 0.3f}, 0.3f, 0.0f, Vec3{1.0f, 0.8f, 0.2f}, 3.0f);
    const assets::AssetRef goal = b.material("Meta", Vec3{0.3f, 1.0f, 0.45f}, 0.2f, 0.0f, Vec3{0.3f, 1.0f, 0.45f}, 4.0f);

    b.box("Suelo", Vec3{0.0f, -0.5f, 0.0f}, Vec3{52.0f, 1.0f, 52.0f}, floor);
    b.ring(25.0f, 3.0f, 1.0f, wall);
    // Un laberinto sencillo: muros con huecos y cajas para esconderse.
    b.box("Muro A", Vec3{-8.0f, 1.5f, 12.0f}, Vec3{26.0f, 3.0f, 1.0f}, wall);
    b.box("Muro B", Vec3{10.0f, 1.5f, 2.0f}, Vec3{22.0f, 3.0f, 1.0f}, wall);
    b.box("Muro C", Vec3{-12.0f, 1.5f, -6.0f}, Vec3{18.0f, 3.0f, 1.0f}, wall);
    b.box("Muro D", Vec3{4.0f, 1.5f, -14.0f}, Vec3{1.0f, 3.0f, 18.0f}, wall);
    b.box("Muro E", Vec3{-3.0f, 1.5f, 4.0f}, Vec3{1.0f, 3.0f, 10.0f}, wall);
    const Vec3 crates[] = {{16.0f, 0.75f, 16.0f}, {-18.0f, 0.75f, 3.0f}, {14.0f, 0.75f, -8.0f}, {-6.0f, 0.75f, -16.0f}};
    int n = 0;
    for (const Vec3& p : crates) {
        ++n;
        b.box("Caja " + std::to_string(n), p, Vec3{1.5f, 1.5f, 1.5f}, crate, Vec3{0.0f, 20.0f * static_cast<float>(n), 0.0f});
    }

    // La malla de navegacion: todo el nivel.
    ecs::Entity volume = b.world.create("NavMeshBoundsVolume");
    volume.setWorldPosition(Vec3{0.0f, 2.0f, 0.0f});
    volume.add<navigation::NavMeshBounds>().size = Vec3{52.0f, 8.0f, 52.0f};

    const Vec3 guards[] = {{8.0f, 1.0f, 8.0f}, {-14.0f, 1.0f, 0.0f}, {14.0f, 1.0f, -16.0f}};
    n = 0;
    for (const Vec3& p : guards) {
        ecs::Entity g = b.world.create("Guardia " + std::to_string(++n));
        g.setWorldPosition(p);
        navigation::NavAgent& agent = g.add<navigation::NavAgent>();
        agent.speed = 3.6f;
        agent.base_offset = 1.0f;  // pivote en el centro de la capsula
        Builder::attach(g, "Guardia.lua");
        ecs::Entity mesh = ecs::createPrimitive(b.world, assets::builtin::kCapsule, "Cuerpo", g);
        mesh.get<ecs::MeshRenderer>().materials = {guard};
        ecs::Entity e = ecs::createPrimitive(b.world, assets::builtin::kCube, "Ojo", g);
        e.setLocalPosition(Vec3{0.0f, 0.5f, -0.4f});
        e.setLocalScale(Vec3{0.55f, 0.16f, 0.2f});
        e.get<ecs::MeshRenderer>().materials = {eye};
        // Luz roja para verlos venir.
        ecs::Entity light = ecs::createLight(b.world, ecs::LightType::Point, g);
        light.setLocalPosition(Vec3{0.0f, 1.4f, 0.0f});
        if (auto* l = light.tryGet<ecs::Light>()) {
            l->color = Vec3{1.0f, 0.25f, 0.2f};
            l->intensity = 6.0f;
            l->range = 6.0f;
            l->cast_shadows = false;  // se mueven: sin sombras (mas barato)
        }
    }

    ecs::Entity meta = ecs::createPrimitive(b.world, assets::builtin::kCube, "Meta");
    meta.setWorldPosition(Vec3{19.0f, 1.0f, -20.0f});
    meta.setLocalScale(Vec3{1.4f, 1.4f, 1.4f});
    meta.setLocalEulerDegrees(Vec3{45.0f, 0.0f, 45.0f});
    meta.get<ecs::MeshRenderer>().materials = {goal};
    physics::SphereCollider& trigger = meta.add<physics::SphereCollider>();
    trigger.radius = 1.4f;
    trigger.material.is_trigger = true;
    Builder::attach(meta, "Meta.lua");

    b.player(Vec3{-20.0f, 1.1f, 20.0f}, body, visor);
    b.camera(13.0f, 50.0f);
    b.hud("Marcador.lua");
    b.save("Main");

    navigation::NavigationSettings nav;
    navigation::saveNavigationSettings(project.settingsFolder() / "Navigation.json", nav);
}

// --- IA con maquinas de estados ----------------------------------------------------

constexpr const char* kStrikeScript = R"lua(-- Golpear (F): resta vida a los enemigos cercanos. Su maquina de estados
-- lo ve en la variable "vida": con menos de 30 pasan a Huir (transicion
-- desde Cualquier estado) y, curados, vuelven a casa y a patrullar.
local Golpe = {
    properties = {
        alcance = 3.5,
        danio = 35,
    }
}

function Golpe:Update(dt)
    if not Input.getKeyDown("f") then return end
    for _, e in ipairs(Scene.findAllWithTag("Enemigo")) do
        if self.entity:distanceTo(e) <= self.alcance then
            local sm = e:getStateMachine()
            if sm then
                sm:set("vida", (sm:get("vida") or 100) - self.danio)
                Debug.log("Golpe a " .. e.name .. ": vida " .. sm:get("vida"))
            end
        end
    end
end

return Golpe
)lua";

constexpr const char* kStateHudScript = R"lua(-- HUD: el estado de la maquina de cada enemigo (sm.state) y su vida.
local Estados = { properties = {} }

function Estados:Update(dt)
    local lineas = { "WASD moverse  ·  F golpear (con poca vida huyen)" }
    for _, e in ipairs(Scene.findAllWithTag("Enemigo")) do
        local sm = e:getStateMachine()
        if sm then
            lineas[#lineas + 1] = string.format("%s:  %s   (vida %d)", e.name, sm.state or "-", math.floor(sm:get("vida") or 0))
        end
    end
    self.entity.text = table.concat(lineas, "\n")
end

return Estados
)lua";

void buildStateMachines(project::ProjectInfo& project) {
    Builder b(project);
    b.world.setSceneUuid(Uuid::generate());
    ecs::populateDefaultScene(b.world);
    b.script("Jugador.lua", kPlayerScript);
    b.script("CamaraTercera.lua", kCameraScript);
    b.script("Golpe.lua", kStrikeScript);
    b.script("Estados.lua", kStateHudScript);

    // La maquina de estados del enemigo (la de ejemplo del motor).
    ai::StateMachineAsset machine = ai::exampleEnemyStateMachine();
    machine.uuid = Uuid::generate();
    const std::filesystem::path machine_path = project.assetsFolder() / "IA" / "Enemigo.crfsm";
    std::filesystem::create_directories(machine_path.parent_path());
    std::string error;
    if (!ai::saveStateMachine(machine, machine_path, &error)) throw std::runtime_error("maquina de estados: " + error);

    const assets::AssetRef floor = b.material("Suelo", Vec3{0.44f, 0.48f, 0.46f}, 0.9f);
    const assets::AssetRef wall = b.material("Muro", Vec3{0.70f, 0.68f, 0.64f}, 0.8f);
    const assets::AssetRef body = b.material("Jugador", Vec3{0.10f, 0.72f, 0.95f}, 0.35f);
    const assets::AssetRef visor = b.material("Visor", Vec3{0.05f, 0.06f, 0.08f}, 0.15f, 0.6f);
    const assets::AssetRef enemy = b.material("Enemigo", Vec3{0.85f, 0.30f, 0.12f}, 0.4f);
    const assets::AssetRef eye = b.material("Ojo", Vec3{1.0f, 0.9f, 0.3f}, 0.3f, 0.0f, Vec3{1.0f, 0.8f, 0.2f}, 3.0f);
    const assets::AssetRef marker = b.material("Waypoint", Vec3{0.3f, 0.6f, 1.0f}, 0.3f, 0.0f, Vec3{0.3f, 0.6f, 1.0f}, 2.0f);

    b.box("Suelo", Vec3{0.0f, -0.5f, 0.0f}, Vec3{44.0f, 1.0f, 44.0f}, floor);
    b.ring(21.0f, 2.5f, 1.0f, wall);
    b.box("Muro A", Vec3{-6.0f, 1.25f, 6.0f}, Vec3{14.0f, 2.5f, 1.0f}, wall);
    b.box("Muro B", Vec3{8.0f, 1.25f, -6.0f}, Vec3{1.0f, 2.5f, 12.0f}, wall);

    ecs::Entity volume = b.world.create("NavMeshBoundsVolume");
    volume.setWorldPosition(Vec3{0.0f, 2.0f, 0.0f});
    volume.add<navigation::NavMeshBounds>().size = Vec3{44.0f, 8.0f, 44.0f};

    // Ruta de patrulla: objetos con el tag "Waypoint" (en orden de nombre).
    const Vec3 points[] = {{-14.0f, 0.1f, -14.0f}, {14.0f, 0.1f, -14.0f}, {14.0f, 0.1f, 14.0f}, {-14.0f, 0.1f, 14.0f}};
    int n = 0;
    for (const Vec3& p : points) {
        ecs::Entity w = ecs::createPrimitive(b.world, assets::builtin::kCylinder, "Punto " + std::to_string(++n));
        w.setWorldPosition(p);
        w.setLocalScale(Vec3{0.6f, 0.05f, 0.6f});
        w.get<ecs::MeshRenderer>().materials = {marker};
        w.setTag("Waypoint");
    }

    // Dos enemigos con NavAgent y la misma maquina (el segundo ve mas lejos).
    const Vec3 spawns[] = {{-10.0f, 1.0f, -10.0f}, {10.0f, 1.0f, 10.0f}};
    n = 0;
    for (const Vec3& p : spawns) {
        ecs::Entity g = b.world.create("Enemigo " + std::to_string(++n));
        g.setWorldPosition(p);
        g.setTag("Enemigo");
        navigation::NavAgent& agent = g.add<navigation::NavAgent>();
        agent.speed = 3.4f;
        agent.base_offset = 1.0f;  // pivote en el centro de la capsula
        ai::StateMachine& sm = g.add<ai::StateMachine>();
        sm.machine = assets::AssetRef{machine.uuid, assets::AssetType::StateMachine};
        sm.debug = n == 1;  // sus cambios de estado salen en la Consola
        if (n == 2) sm.variables.push_back(ai::VariableOverride{"rangoVision", static_cast<int>(ai::VarType::Float), "14"});
        ecs::Entity mesh = ecs::createPrimitive(b.world, assets::builtin::kCapsule, "Cuerpo", g);
        mesh.get<ecs::MeshRenderer>().materials = {enemy};
        ecs::Entity e = ecs::createPrimitive(b.world, assets::builtin::kCube, "Ojo", g);
        e.setLocalPosition(Vec3{0.0f, 0.5f, -0.4f});
        e.setLocalScale(Vec3{0.55f, 0.16f, 0.2f});
        e.get<ecs::MeshRenderer>().materials = {eye};
    }

    ecs::Entity player = b.player(Vec3{0.0f, 1.1f, 16.0f}, body, visor);
    ecs::Entity strike = b.world.create("Golpe", player);
    Builder::attach(strike, "Golpe.lua");
    b.camera(12.0f, 45.0f);
    ecs::Entity hud = b.hud("Estados.lua");
    if (auto* rt = hud.tryGet<ui::RectTransform>()) rt->size = Vec2{1000.0f, 200.0f};
    if (auto* t = hud.tryGet<ui::Text>()) t->font_size = 28.0f;
    b.save("Main");

    navigation::NavigationSettings nav;
    navigation::saveNavigationSettings(project.settingsFolder() / "Navigation.json", nav);
}

// --- Recursos del mundo de bloques (iconos pixel art, grietas) ---------------------

using Rgba = std::array<std::uint8_t, 4>;

// Un icono de 8 x 8 "pixeles" ampliado a `size` (sin suavizar, como Minecraft).
void pixelIcon(const std::filesystem::path& file, const std::vector<std::string>& rows,
               const std::map<char, Rgba>& palette, std::uint32_t size = 64) {
    asset::ImageRgba8 image;
    image.width = image.height = size;
    image.pixels.assign(static_cast<std::size_t>(size) * size * 4, 0);
    const std::size_t n = rows.size();
    for (std::uint32_t y = 0; y < size; ++y) {
        for (std::uint32_t x = 0; x < size; ++x) {
            const std::size_t r = y * n / size, c = x * n / size;
            const char key = c < rows[r].size() ? rows[r][c] : '.';
            const auto it = palette.find(key);
            if (it == palette.end()) continue;
            std::memcpy(&image.pixels[(static_cast<std::size_t>(y) * size + x) * 4], it->second.data(), 4);
        }
    }
    std::filesystem::create_directories(file.parent_path());
    if (!asset::saveImagePng(file, image)) throw std::runtime_error("no se pudo escribir " + file.string());
}

void writeVoxelIcons(const std::filesystem::path& folder) {
    const Rgba red{214, 32, 40, 255}, light{255, 170, 170, 255}, dark{60, 20, 24, 255}, empty{70, 70, 76, 200};
    const std::vector<std::string> heart = {".RR..RR.", "RWRRRRRR", "RRRRRRRR", "RRRRRRRR",
                                            ".RRRRRR.", "..RRRR..", "...RR...", "........"};
    pixelIcon(folder / "corazon.png", heart, {{'R', red}, {'W', light}});
    pixelIcon(folder / "corazon_medio.png", {".RR..EE.", "RWRREEEE", "RRRREEEE", "RRRREEEE", ".RRREEE.", "..RREE..", "...RE...", "........"},
              {{'R', red}, {'W', light}, {'E', empty}});
    pixelIcon(folder / "corazon_vacio.png", heart, {{'R', empty}, {'W', empty}});
    (void)dark;

    const Rgba meat{190, 90, 40, 255}, bone{235, 225, 205, 255}, meat_dark{120, 50, 20, 255};
    const std::vector<std::string> food = {"......BB", ".....BWB", "....BBB.", "..MMMB..",
                                           ".MMMMM..", ".MMMMD..", ".MMMD...", "..MM...."};
    pixelIcon(folder / "comida.png", food, {{'M', meat}, {'B', bone}, {'W', bone}, {'D', meat_dark}});
    pixelIcon(folder / "comida_media.png", {"......EE", ".....EEE", "....EEE.", "..MMEE..", ".MMMEE..", ".MMMEE..", ".MMEE...", "..ME...."},
              {{'M', meat}, {'E', empty}});
    pixelIcon(folder / "comida_vacia.png", food, {{'M', empty}, {'B', empty}, {'W', empty}, {'D', empty}});

    const Rgba bubble{150, 210, 255, 230}, shine{255, 255, 255, 255};
    pixelIcon(folder / "aire.png", {"..BBBB..", ".B....B.", "B.W....B", "B......B", "B......B", "B......B", ".B....B.", "..BBBB.."},
              {{'B', bubble}, {'W', shine}});

    const Rgba wood{176, 128, 72, 255}, wood_dark{110, 76, 40, 255};
    pixelIcon(folder / "palo.png", {".......W", "......WB", ".....WB.", "....WB..", "...WB...", "..WB....", ".WB.....", "WB......"},
              {{'W', wood}, {'B', wood_dark}});
    const Rgba coal{30, 30, 34, 255}, coal_shine{90, 90, 100, 255};
    pixelIcon(folder / "carbon.png", {"........", "..KKK...", ".KKGKK..", ".KKKKKK.", ".KGKKKK.", "..KKKGK.", "...KKK..", "........"},
              {{'K', coal}, {'G', coal_shine}});
    const Rgba apple{210, 30, 30, 255}, apple_shine{255, 140, 140, 255}, leaf{70, 160, 50, 255}, stem{100, 70, 40, 255};
    pixelIcon(folder / "manzana.png", {"....L...", "...LS...", ".RRRSRR.", "RRWRRRRR", "RWRRRRRR", "RRRRRRRR", ".RRRRRR.", "..RR.RR."},
              {{'R', apple}, {'W', apple_shine}, {'L', leaf}, {'S', stem}});
    const std::vector<std::string> pick = {".HHHHHH.", "H..WB..H", "...WB...", "...WB...", "...WB...", "...WB...", "...WB...", "...WB..."};
    pixelIcon(folder / "pico_madera.png", pick, {{'H', {150, 108, 60, 255}}, {'W', wood}, {'B', wood_dark}});
    pixelIcon(folder / "pico_piedra.png", pick, {{'H', {128, 128, 132, 255}}, {'W', wood}, {'B', wood_dark}});
}

// Diez etapas de grietas (64 x 64 con alfa): mas lineas y mas largas cada vez.
void writeCrackTextures(const std::filesystem::path& folder) {
    std::filesystem::create_directories(folder);
    constexpr std::uint32_t kSize = 64;
    constexpr int kCell = 2;  // lineas de 2 pixeles
    constexpr int kGrid = static_cast<int>(kSize) / kCell;
    std::vector<std::uint8_t> cracked(kGrid * kGrid, 0);
    std::uint32_t seed = 1234567u;
    const auto rnd = [&]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / static_cast<float>(1u << 24);
    };
    for (int stage = 0; stage < 10; ++stage) {
        // Cada etapa anade grietas a las de la anterior (se van extendiendo).
        // Una o dos grietas nuevas desde cerca del centro hacia fuera.
        const int lines = 1 + (stage % 2);
        for (int l = 0; l < lines; ++l) {
            float x = kGrid * 0.5f + (rnd() - 0.5f) * 6.0f, y = kGrid * 0.5f + (rnd() - 0.5f) * 6.0f;
            float angle = rnd() * 6.2831853f;
            const int steps = 6 + stage * 2;
            for (int s = 0; s < steps; ++s) {
                const int cx = static_cast<int>(x), cy = static_cast<int>(y);
                if (cx >= 0 && cy >= 0 && cx < kGrid && cy < kGrid) cracked[static_cast<std::size_t>(cy * kGrid + cx)] = 1;
                angle += (rnd() - 0.5f) * 1.1f;
                x += std::cos(angle);
                y += std::sin(angle);
            }
        }
        asset::ImageRgba8 image;
        image.width = image.height = kSize;
        image.pixels.assign(static_cast<std::size_t>(kSize) * kSize * 4, 0);
        for (std::uint32_t py = 0; py < kSize; ++py) {
            for (std::uint32_t px = 0; px < kSize; ++px) {
                if (!cracked[static_cast<std::size_t>((py / kCell) * kGrid + px / kCell)]) continue;
                std::uint8_t* p = &image.pixels[(static_cast<std::size_t>(py) * kSize + px) * 4];
                p[0] = 24;
                p[1] = 22;
                p[2] = 20;
                p[3] = 235;
            }
        }
        if (!asset::saveImagePng(folder / ("grieta_" + std::to_string(stage) + ".png"), image)) {
            throw std::runtime_error("no se pudo escribir las grietas");
        }
    }
}

void buildVoxel(project::ProjectInfo& project) {
    voxel::registerVoxelComponents();
    Builder b(project);
    b.world.setSceneUuid(Uuid::generate());
    ecs::populateDefaultScene(b.world);
    b.script("JugadorBloques.lua", kVoxelPlayerScript);

    // Iconos de todos los bloques, de los objetos y de la interfaz; grietas.
    const std::filesystem::path icons = project.assetsFolder() / "Voxel" / "Iconos";
    if (!voxel::writeBlockIcons(icons, 64)) throw std::runtime_error("no se pudieron escribir los iconos de los bloques");
    writeVoxelIcons(icons);
    writeCrackTextures(project.assetsFolder() / "Voxel" / "Grietas");

    // El mundo infinito y su mar (el oceano del motor en la superficie del agua).
    ecs::Entity world = b.world.create("Mundo de bloques");
    const voxel::VoxelWorld& settings = world.add<voxel::VoxelWorld>();
    ecs::Entity sea = b.world.create("Mar", world);
    sea.add<water::WaterBody>() = water::oceanPreset();
    sea.setWorldPosition(Vec3{0.0f, std::round(settings.sea_level) - 0.1f, 0.0f});

    // La camara es el jugador (primera persona); sus botones llaman a su script.
    ecs::Entity cam = b.world.findByName("Main Camera");
    cam.setWorldPosition(Vec3{0.5f, 100.0f, 0.5f});
    Builder::attach(cam, "JugadorBloques.lua");
    const Uuid player = cam.uuid();

    // --- Interfaz ---
    ecs::Entity canvas = b.world.create("HUD");
    canvas.add<ui::Canvas>();
    const auto rect = [&](ecs::Entity e, Vec2 anchor, Vec2 pivot, Vec2 position, Vec2 size) {
        ui::RectTransform& rt = e.add<ui::RectTransform>();
        rt.anchor_min = anchor;
        rt.anchor_max = anchor;
        rt.pivot = pivot;
        rt.position = position;
        rt.size = size;
        return e;
    };
    const auto stretch = [&](ecs::Entity e) {
        ui::RectTransform& rt = e.add<ui::RectTransform>();
        rt.anchor_min = Vec2{0.0f, 0.0f};
        rt.anchor_max = Vec2{1.0f, 1.0f};
        rt.size = Vec2{0.0f, 0.0f};
        return e;
    };
    const auto image = [&](ecs::Entity e, Vec3 color, float alpha, float radius = 0.0f, const std::string& texture = {}) {
        ui::Image& im = e.add<ui::Image>();
        im.color = color;
        im.alpha = alpha;
        im.corner_radius = radius;
        im.texture = texture;
        return e;
    };
    const auto text = [&](ecs::Entity e, const std::string& value, float font, ui::HAlign align, Vec3 color = Vec3{1, 1, 1}) {
        ui::Text& t = e.add<ui::Text>();
        t.text = value;
        t.font_size = font;
        t.h_align = align;
        t.color = color;
        t.shadow = true;
        return e;
    };
    const auto button = [&](ecs::Entity e, const std::string& method, Vec3 color) {
        ui::Button& bt = e.add<ui::Button>();
        bt.normal = color;
        bt.hover = color * 1.35f;
        bt.pressed = color * 0.7f;
        bt.disabled = Vec3{0.16f, 0.16f, 0.17f};
        bt.corner_radius = 6.0f;
        bt.target = player;
        bt.on_click = method;
        return e;
    };
    // Un hueco de objeto: fondo, icono y cantidad.
    const auto slot = [&](ecs::Entity e, float icon_size) {
        ecs::Entity icon = b.world.create("Icono", e);
        rect(icon, Vec2{0.5f, 0.5f}, Vec2{0.5f, 0.5f}, Vec2{0.0f, 0.0f}, Vec2{icon_size, icon_size});
        image(icon, Vec3{1, 1, 1}, 0.0f);
        ecs::Entity count = b.world.create("Cantidad", e);
        rect(count, Vec2{1.0f, 1.0f}, Vec2{1.0f, 1.0f}, Vec2{-4.0f, -1.0f}, Vec2{60.0f, 28.0f});
        text(count, "", 22.0f, ui::HAlign::Right);
    };

    // Mira, avisos y datos.
    text(rect(b.world.create("Mira", canvas), Vec2{0.5f, 0.5f}, Vec2{0.5f, 0.5f}, Vec2{0, 0}, Vec2{60, 60}), "+", 40.0f, ui::HAlign::Center);
    text(rect(b.world.create("Info", canvas), Vec2{0, 0}, Vec2{0, 0}, Vec2{32, 24}, Vec2{1300, 140}), "", 24.0f, ui::HAlign::Left);
    ecs::Entity aviso = text(rect(b.world.create("Aviso", canvas), Vec2{0.5f, 0.0f}, Vec2{0.5f, 0.0f}, Vec2{0, 150}, Vec2{1200, 50}), "",
                             30.0f, ui::HAlign::Center, Vec3{1.0f, 0.92f, 0.55f});
    aviso.get<ui::Text>().alpha = 0.0f;
    // Destello rojo al recibir dano (toda la pantalla).
    image(stretch(b.world.create("Dano", canvas)), Vec3{0.8f, 0.05f, 0.05f}, 0.0f);

    // Barra de 9 huecos, el marco del elegido, vida, hambre y aire.
    image(rect(b.world.create("BarraFondo", canvas), Vec2{0.5f, 1.0f}, Vec2{0.5f, 1.0f}, Vec2{0, -16}, Vec2{9 * 84 + 16, 96}),
          Vec3{0.05f, 0.05f, 0.06f}, 0.55f, 10.0f);
    image(rect(b.world.create("Seleccion", canvas), Vec2{0.5f, 1.0f}, Vec2{0.5f, 1.0f}, Vec2{-4 * 84, -20}, Vec2{84, 84}),
          Vec3{0.95f, 0.95f, 0.9f}, 0.95f, 8.0f);
    for (int i = 1; i <= 9; ++i) {
        ecs::Entity s = b.world.create("Hueco " + std::to_string(i), canvas);
        rect(s, Vec2{0.5f, 1.0f}, Vec2{0.5f, 1.0f}, Vec2{static_cast<float>((i - 5) * 84), -24}, Vec2{76, 76});
        image(s, Vec3{0.14f, 0.14f, 0.16f}, 0.9f, 6.0f);
        slot(s, 56.0f);
    }
    for (int i = 1; i <= 10; ++i) {
        const float step = static_cast<float>(i - 1) * 32.0f;
        image(rect(b.world.create("Corazon " + std::to_string(i), canvas), Vec2{0.5f, 1.0f}, Vec2{0.5f, 1.0f},
                   Vec2{-374.0f + 15.0f + step, -118}, Vec2{30, 30}),
              Vec3{1, 1, 1}, 1.0f, 0.0f, "Voxel/Iconos/corazon.png");
        image(rect(b.world.create("Comida " + std::to_string(i), canvas), Vec2{0.5f, 1.0f}, Vec2{0.5f, 1.0f},
                   Vec2{374.0f - 15.0f - step, -118}, Vec2{30, 30}),
              Vec3{1, 1, 1}, 1.0f, 0.0f, "Voxel/Iconos/comida.png");
        ecs::Entity bubble = image(rect(b.world.create("Aire " + std::to_string(i), canvas), Vec2{0.5f, 1.0f}, Vec2{0.5f, 1.0f},
                                        Vec2{374.0f - 15.0f - step, -152}, Vec2{28, 28}),
                                   Vec3{1, 1, 1}, 1.0f, 0.0f, "Voxel/Iconos/aire.png");
        bubble.setActive(false);
    }
    ecs::Entity item_name = text(rect(b.world.create("NombreObjeto", canvas), Vec2{0.5f, 1.0f}, Vec2{0.5f, 1.0f}, Vec2{0, -190},
                                      Vec2{800, 40}),
                                 "", 28.0f, ui::HAlign::Center);
    item_name.get<ui::Text>().alpha = 0.0f;

    // Inventario y crafteo (E).
    ecs::Entity inventory = b.world.create("Inventario", canvas);
    rect(inventory, Vec2{0.5f, 0.5f}, Vec2{0.5f, 0.5f}, Vec2{0, -20}, Vec2{1400, 780});
    image(inventory, Vec3{0.07f, 0.08f, 0.1f}, 0.94f, 14.0f);
    text(rect(b.world.create("Titulo", inventory), Vec2{0, 0}, Vec2{0, 0}, Vec2{40, 22}, Vec2{600, 48}), "Inventario", 34.0f,
         ui::HAlign::Left);
    text(rect(b.world.create("TituloBarra", inventory), Vec2{0, 0}, Vec2{0, 0}, Vec2{40, 342}, Vec2{600, 30}), "Barra (1-9)", 20.0f,
         ui::HAlign::Left, Vec3{0.7f, 0.72f, 0.78f});
    for (int i = 1; i <= 36; ++i) {
        const int index = i <= 9 ? i - 1 : i - 10;
        const float x = 40.0f + static_cast<float>(index % 9) * 80.0f;
        const float y = i <= 9 ? 376.0f : 86.0f + static_cast<float>(index / 9) * 80.0f;
        ecs::Entity s = b.world.create("Inv " + std::to_string(i), inventory);
        rect(s, Vec2{0, 0}, Vec2{0, 0}, Vec2{x, y}, Vec2{72, 72});
        button(s, "OnHueco", Vec3{0.18f, 0.19f, 0.22f});
        slot(s, 52.0f);
    }
    text(rect(b.world.create("Ayuda", inventory), Vec2{0, 1}, Vec2{0, 1}, Vec2{40, -24}, Vec2{720, 90}),
         "Clic en un hueco: lo cambia por el elegido de la barra.\nE o Escape: cerrar.", 20.0f, ui::HAlign::Left,
         Vec3{0.7f, 0.72f, 0.78f});
    text(rect(b.world.create("TituloCrafteo", inventory), Vec2{0, 0}, Vec2{0, 0}, Vec2{790, 22}, Vec2{570, 48}), "Crafteo", 34.0f,
         ui::HAlign::Left);
    for (int i = 1; i <= 13; ++i) {
        ecs::Entity r = b.world.create("Receta " + std::to_string(i), inventory);
        rect(r, Vec2{0, 0}, Vec2{0, 0}, Vec2{790, 86.0f + static_cast<float>(i - 1) * 50.0f}, Vec2{580, 44});
        button(r, "OnReceta", Vec3{0.16f, 0.36f, 0.24f});
        image(rect(b.world.create("Icono", r), Vec2{0.0f, 0.5f}, Vec2{0.0f, 0.5f}, Vec2{8, 0}, Vec2{34, 34}), Vec3{1, 1, 1}, 1.0f);
        text(rect(b.world.create("Texto", r), Vec2{0.0f, 0.5f}, Vec2{0.0f, 0.5f}, Vec2{52, 0}, Vec2{520, 40}), "", 19.0f,
             ui::HAlign::Left);
    }
    inventory.setActive(false);

    // Muerte y pausa.
    ecs::Entity death = b.world.create("Muerte", canvas);
    image(stretch(death), Vec3{0.35f, 0.02f, 0.02f}, 0.6f);
    text(rect(b.world.create("Titulo", death), Vec2{0.5f, 0.5f}, Vec2{0.5f, 0.5f}, Vec2{0, -110}, Vec2{900, 90}), "¡Has muerto!",
         72.0f, ui::HAlign::Center);
    text(rect(b.world.create("Causa", death), Vec2{0.5f, 0.5f}, Vec2{0.5f, 0.5f}, Vec2{0, -30}, Vec2{900, 50}), "", 30.0f,
         ui::HAlign::Center, Vec3{1.0f, 0.8f, 0.8f});
    ecs::Entity respawn = b.world.create("Reaparecer", death);
    rect(respawn, Vec2{0.5f, 0.5f}, Vec2{0.5f, 0.5f}, Vec2{0, 70}, Vec2{340, 66});
    button(respawn, "OnReaparecer", Vec3{0.5f, 0.12f, 0.12f});
    text(stretch(b.world.create("Texto", respawn)), "Reaparecer", 30.0f, ui::HAlign::Center);
    death.setActive(false);

    ecs::Entity pause = b.world.create("Pausa", canvas);
    image(stretch(pause), Vec3{0.02f, 0.03f, 0.05f}, 0.6f);
    text(rect(b.world.create("Titulo", pause), Vec2{0.5f, 0.5f}, Vec2{0.5f, 0.5f}, Vec2{0, -170}, Vec2{600, 90}), "Pausa", 64.0f,
         ui::HAlign::Center);
    const char* pause_buttons[][2] = {{"Seguir", "OnSeguir"}, {"Guardar", "OnGuardar"}, {"Salir", "OnSalir"}};
    for (int i = 0; i < 3; ++i) {
        ecs::Entity bt = b.world.create(pause_buttons[i][0], pause);
        rect(bt, Vec2{0.5f, 0.5f}, Vec2{0.5f, 0.5f}, Vec2{0, -40.0f + static_cast<float>(i) * 84.0f}, Vec2{360, 66});
        button(bt, pause_buttons[i][1], Vec3{0.16f, 0.36f, 0.72f});
        const std::string label = i == 1 ? "Guardar el mundo" : (i == 2 ? "Guardar y salir" : "Seguir jugando");
        text(stretch(b.world.create("Texto", bt)), label, 28.0f, ui::HAlign::Center);
    }
    pause.setActive(false);
    b.save("Main");
}

// --- Plantilla MMO RPG ----------------------------------------------------------
// El motor solo coloca el mundo (pueblo, bosque, campamento, guarida), los
// personajes y la interfaz; todo lo que pasa lo hacen los scripts de Lua
// (TemplateMmoScripts.h).

// Numeros al azar repetibles (el mismo mundo en cada proyecto nuevo).
struct Lcg {
    std::uint32_t state;
    float next() {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>((state >> 8) & 0xFFFFFF) / 16777216.0f;
    }
    float range(float lo, float hi) { return lo + (hi - lo) * next(); }
};

void buildMmo(project::ProjectInfo& project) {
    Builder b(project);
    b.world.setSceneUuid(Uuid::generate());
    ecs::populateDefaultScene(b.world);
    b.script("Heroe.lua", mmo::kHeroScript);
    b.script("Enemigo.lua", mmo::kEnemyScript);
    b.script("Bot.lua", mmo::kBotScript);
    b.script("NPC.lua", mmo::kNpcScript);
    b.script("CamaraTercera.lua", kCameraScript);

    const assets::AssetRef grass = b.material("Hierba", Vec3{0.36f, 0.52f, 0.27f}, 0.95f);
    const assets::AssetRef road = b.material("Camino", Vec3{0.55f, 0.46f, 0.34f}, 0.95f);
    const assets::AssetRef stone = b.material("Piedra", Vec3{0.58f, 0.57f, 0.55f}, 0.85f);
    const assets::AssetRef plaster = b.material("Pared", Vec3{0.86f, 0.80f, 0.68f}, 0.9f);
    const assets::AssetRef roof = b.material("Tejado", Vec3{0.62f, 0.24f, 0.17f}, 0.8f);
    const assets::AssetRef wood = b.material("Madera", Vec3{0.45f, 0.31f, 0.19f}, 0.85f);
    const assets::AssetRef dark_wood = b.material("Madera oscura", Vec3{0.25f, 0.17f, 0.11f}, 0.85f);
    const assets::AssetRef leaves = b.material("Hojas", Vec3{0.20f, 0.42f, 0.18f}, 0.9f);
    const assets::AssetRef rock = b.material("Roca", Vec3{0.34f, 0.33f, 0.32f}, 0.9f);
    const assets::AssetRef tent = b.material("Lona", Vec3{0.52f, 0.44f, 0.30f}, 0.95f);
    const assets::AssetRef fire = b.material("Fuego", Vec3{1.0f, 0.5f, 0.1f}, 0.5f, 0.0f, Vec3{1.0f, 0.45f, 0.1f}, 6.0f);
    const assets::AssetRef body = b.material("Jugador", Vec3{0.10f, 0.55f, 0.95f}, 0.35f);
    const assets::AssetRef visor = b.material("Visor", Vec3{0.05f, 0.06f, 0.08f}, 0.15f, 0.6f);
    const assets::AssetRef wolf = b.material("Lobo", Vec3{0.42f, 0.42f, 0.45f}, 0.8f);
    const assets::AssetRef goblin = b.material("Goblin", Vec3{0.35f, 0.62f, 0.25f}, 0.7f);
    const assets::AssetRef shaman = b.material("Chaman", Vec3{0.50f, 0.28f, 0.65f}, 0.6f);
    const assets::AssetRef king = b.material("Rey Goblin", Vec3{0.22f, 0.40f, 0.16f}, 0.6f);
    const assets::AssetRef gold = b.material("Oro", Vec3{1.0f, 0.78f, 0.25f}, 0.25f, 1.0f);
    const assets::AssetRef eye = b.material("Ojos", Vec3{1.0f, 0.2f, 0.1f}, 0.3f, 0.0f, Vec3{1.0f, 0.2f, 0.1f}, 4.0f);
    const assets::AssetRef bar_back = b.material("Barra fondo", Vec3{0.08f, 0.05f, 0.05f}, 0.9f);
    const assets::AssetRef bar_fill = b.material("Barra vida", Vec3{0.9f, 0.12f, 0.1f}, 0.5f, 0.0f, Vec3{0.9f, 0.1f, 0.08f}, 1.5f);
    b.material("Marca mision", Vec3{1.0f, 0.82f, 0.1f}, 0.3f, 0.0f, Vec3{1.0f, 0.78f, 0.1f}, 5.0f);
    b.material("Marca entregar", Vec3{0.3f, 0.65f, 1.0f}, 0.3f, 0.0f, Vec3{0.3f, 0.6f, 1.0f}, 5.0f);
    const assets::AssetRef quest_mark = assets::AssetRef{};  // lo pone NPC.lua

    // Una caja sin collider (decoracion plana: caminos, plaza).
    const auto flat = [&](const std::string& name, const Vec3& p, const Vec3& s, const assets::AssetRef& m, float yaw = 0.0f) {
        ecs::Entity e = b.box(name, p, s, m, Vec3{0.0f, yaw, 0.0f});
        e.remove<physics::BoxCollider>();
        return e;
    };
    const auto primitive = [&](const Uuid& mesh, const std::string& name, ecs::Entity parent, const Vec3& p, const Vec3& s,
                               const assets::AssetRef& m, const Vec3& euler = Vec3{}) {
        ecs::Entity e = ecs::createPrimitive(b.world, mesh, name, parent);
        e.setLocalPosition(p);
        e.setLocalScale(s);
        e.setLocalEulerDegrees(euler);
        e.get<ecs::MeshRenderer>().materials = {m};
        return e;
    };
    const auto point_light = [&](const std::string& name, const Vec3& p, const Vec3& color, float intensity, float range) {
        ecs::Entity l = ecs::createLight(b.world, ecs::LightType::Point);
        l.setName(name);
        l.setWorldPosition(p);
        if (auto* light = l.tryGet<ecs::Light>()) {
            light->color = color;
            light->intensity = intensity;
            light->range = range;
            light->cast_shadows = false;
        }
        return l;
    };
    // Casa: paredes, tejado a dos aguas (una caja girada 45 grados) y puerta.
    const auto house = [&](const std::string& name, const Vec3& p, const Vec3& size, float yaw) {
        ecs::Entity h = b.box(name, p + Vec3{0.0f, size.y * 0.5f, 0.0f}, size, plaster, Vec3{0.0f, yaw, 0.0f});
        const float r = size.z * 0.72f;
        ecs::Entity t = primitive(assets::builtin::kCube, "Tejado", h, Vec3{0.0f, 0.5f, 0.0f},
                                  Vec3{1.08f, r / size.y, r / size.z}, roof, Vec3{45.0f, 0.0f, 0.0f});
        (void)t;
        primitive(assets::builtin::kCube, "Puerta", h, Vec3{0.0f, -0.5f + 1.1f / size.y, 0.505f},
                  Vec3{1.4f / size.x, 2.2f / size.y, 0.02f}, dark_wood);
        return h;
    };

    // --- Terreno y caminos ---
    b.box("Suelo", Vec3{0.0f, -0.5f, 0.0f}, Vec3{280.0f, 1.0f, 280.0f}, grass);
    b.ring(138.0f, 5.0f, 3.0f, rock);
    flat("Plaza", Vec3{0.0f, 0.01f, 0.0f}, Vec3{34.0f, 0.04f, 34.0f}, stone);
    flat("Camino oeste", Vec3{-38.0f, 0.02f, -20.0f}, Vec3{5.0f, 0.04f, 48.0f}, road, 62.0f);
    flat("Camino este", Vec3{38.0f, 0.02f, -32.0f}, Vec3{5.0f, 0.04f, 62.0f}, road, -48.0f);
    flat("Camino norte", Vec3{0.0f, 0.02f, -60.0f}, Vec3{5.0f, 0.04f, 88.0f}, road);

    // --- Villa Alba (el pueblo) ---
    ecs::Entity zone_town = b.world.create("Zona Pueblo");
    zone_town.setWorldPosition(Vec3{0.0f, 0.0f, 0.0f});
    house("Casa 1", Vec3{-22.0f, 0.0f, -14.0f}, Vec3{8.0f, 5.0f, 7.0f}, 90.0f);
    house("Casa 2", Vec3{-22.0f, 0.0f, 6.0f}, Vec3{8.0f, 5.0f, 7.0f}, 90.0f);
    house("Casa 3", Vec3{22.0f, 0.0f, -14.0f}, Vec3{8.0f, 5.0f, 7.0f}, -90.0f);
    house("Casa 4", Vec3{22.0f, 0.0f, 10.0f}, Vec3{9.0f, 5.5f, 7.0f}, -90.0f);
    house("Casa 5", Vec3{-8.0f, 0.0f, 24.0f}, Vec3{9.0f, 5.0f, 7.0f}, 180.0f);
    house("Posada", Vec3{10.0f, 0.0f, 25.0f}, Vec3{12.0f, 6.5f, 8.0f}, 180.0f);
    ecs::Entity chapel = house("Capilla", Vec3{-30.0f, 0.0f, 28.0f}, Vec3{10.0f, 7.0f, 14.0f}, 135.0f);
    primitive(assets::builtin::kCube, "Torre", chapel, Vec3{0.0f, 0.9f, -0.3f}, Vec3{0.3f, 0.9f, 0.22f}, plaster);
    b.box("Pozo", Vec3{0.0f, 0.5f, 0.0f}, Vec3{2.2f, 1.0f, 2.2f}, stone);
    primitive(assets::builtin::kCylinder, "Tejadillo del pozo", b.world.findByName("Pozo"), Vec3{0.0f, 2.2f, 0.0f},
              Vec3{0.8f, 0.08f, 0.8f}, wood);
    // Puestos del mercado.
    for (int i = 0; i < 3; ++i) {
        const float x = 7.0f + static_cast<float>(i) * 4.0f;
        b.box("Puesto " + std::to_string(i + 1), Vec3{x, 0.5f, 8.0f}, Vec3{3.0f, 1.0f, 1.4f}, wood);
        flat("Toldo " + std::to_string(i + 1), Vec3{x, 2.4f, 8.0f}, Vec3{3.4f, 0.1f, 2.0f}, i % 2 == 0 ? roof : tent);
    }
    // Farolas.
    const Vec3 lamps[] = {{-12, 0, -12}, {12, 0, -12}, {-12, 0, 12}, {12, 0, 12}};
    int lamp = 0;
    for (const Vec3& p : lamps) {
        ++lamp;
        b.box("Farola " + std::to_string(lamp), p + Vec3{0.0f, 1.6f, 0.0f}, Vec3{0.25f, 3.2f, 0.25f}, dark_wood);
        point_light("Luz farola " + std::to_string(lamp), p + Vec3{0.0f, 3.5f, 0.0f}, Vec3{1.0f, 0.78f, 0.45f}, 6.0f, 12.0f);
    }
    ecs::Entity spawn = b.world.create("Punto de reaparicion");
    spawn.setWorldPosition(Vec3{0.0f, 1.2f, 8.0f});

    // --- Bosque Gris (oeste): arboles y lobos ---
    const Vec3 forest{-68.0f, 0.0f, -40.0f};
    ecs::Entity zone_forest = b.world.create("Zona Bosque");
    zone_forest.setWorldPosition(forest);
    Lcg rng{12345u};
    for (int i = 0; i < 46; ++i) {
        const float angle = rng.range(0.0f, 6.2832f);
        const float dist = rng.range(4.0f, 36.0f);
        const Vec3 p = forest + Vec3{std::cos(angle) * dist, 0.0f, std::sin(angle) * dist};
        const float height = rng.range(4.0f, 7.0f);
        ecs::Entity tree = b.world.create("Arbol " + std::to_string(i + 1));
        tree.setWorldPosition(p);
        ecs::Entity trunk = primitive(assets::builtin::kCylinder, "Tronco", tree, Vec3{0.0f, height * 0.5f, 0.0f},
                                      Vec3{0.5f, height * 0.5f, 0.5f}, wood);
        trunk.add<physics::BoxCollider>();
        const float crown = rng.range(2.6f, 4.0f);
        primitive(assets::builtin::kSphere, "Copa", tree, Vec3{0.0f, height + crown * 0.3f, 0.0f},
                  Vec3{crown, crown * 1.1f, crown}, leaves);
    }

    // --- Campamento goblin (este): empalizada, tiendas y hoguera ---
    const Vec3 camp{66.0f, 0.0f, -66.0f};
    ecs::Entity zone_camp = b.world.create("Zona Campamento");
    zone_camp.setWorldPosition(camp);
    b.box("Empalizada N", camp + Vec3{0.0f, 1.5f, -22.0f}, Vec3{44.0f, 3.0f, 0.6f}, dark_wood);
    b.box("Empalizada S1", camp + Vec3{-14.0f, 1.5f, 22.0f}, Vec3{16.0f, 3.0f, 0.6f}, dark_wood);
    b.box("Empalizada S2", camp + Vec3{14.0f, 1.5f, 22.0f}, Vec3{16.0f, 3.0f, 0.6f}, dark_wood);
    b.box("Empalizada E", camp + Vec3{22.0f, 1.5f, 0.0f}, Vec3{0.6f, 3.0f, 44.0f}, dark_wood);
    b.box("Empalizada O1", camp + Vec3{-22.0f, 1.5f, -12.0f}, Vec3{0.6f, 3.0f, 20.0f}, dark_wood);
    b.box("Empalizada O2", camp + Vec3{-22.0f, 1.5f, 15.0f}, Vec3{0.6f, 3.0f, 14.0f}, dark_wood);
    const Vec3 tents[] = {{-12, 0, -12}, {10, 0, -14}, {12, 0, 8}, {-10, 0, 10}};
    int tent_n = 0;
    for (const Vec3& t : tents) {
        ++tent_n;
        ecs::Entity e = b.box("Tienda " + std::to_string(tent_n), camp + t + Vec3{0.0f, 1.2f, 0.0f}, Vec3{4.0f, 2.8f, 2.8f}, tent,
                              Vec3{45.0f, static_cast<float>(tent_n) * 40.0f, 0.0f});
        (void)e;
    }
    b.box("Hoguera", camp + Vec3{0.0f, 0.2f, 0.0f}, Vec3{1.6f, 0.4f, 1.6f}, rock);
    primitive(assets::builtin::kSphere, "Llamas", b.world.findByName("Hoguera"), Vec3{0.0f, 1.2f, 0.0f}, Vec3{0.5f, 1.4f, 0.5f}, fire);
    point_light("Luz hoguera", camp + Vec3{0.0f, 1.6f, 0.0f}, Vec3{1.0f, 0.55f, 0.2f}, 10.0f, 16.0f);

    // --- Guarida del Rey (norte): circulo de rocas y trono ---
    const Vec3 lair{0.0f, 0.0f, -118.0f};
    ecs::Entity zone_lair = b.world.create("Zona Guarida");
    zone_lair.setWorldPosition(lair);
    for (int i = 0; i < 12; ++i) {
        if (i == 3) continue;  // la entrada (hacia el camino, al sur)
        const float angle = static_cast<float>(i) * 0.5236f + 0.26f;
        const Vec3 p = lair + Vec3{std::cos(angle) * 17.0f, 2.2f, std::sin(angle) * 17.0f};
        b.box("Roca " + std::to_string(i + 1), p, Vec3{5.0f, 4.4f + static_cast<float>(i % 3), 3.5f}, rock,
              Vec3{static_cast<float>(i * 7 % 15), static_cast<float>(i * 37), 0.0f});
    }
    b.box("Trono", lair + Vec3{0.0f, 1.2f, -9.0f}, Vec3{3.0f, 2.4f, 2.0f}, gold);
    point_light("Luz guarida", lair + Vec3{0.0f, 4.0f, -6.0f}, Vec3{0.6f, 1.0f, 0.4f}, 8.0f, 20.0f);

    // Navegacion: todo el mapa.
    ecs::Entity volume = b.world.create("NavMeshBoundsVolume");
    volume.setWorldPosition(Vec3{0.0f, 4.0f, 0.0f});
    volume.add<navigation::NavMeshBounds>().size = Vec3{276.0f, 14.0f, 276.0f};

    // --- Jugador y camara ---
    ecs::Entity player = b.player(Vec3{0.0f, 1.1f, 8.0f}, body, visor);
    player.get<scripting::Script>().file = "Scripts/Heroe.lua";
    player.get<scripting::Script>().properties = {{"nombre", scripting::PropertyType::Text, "Heroe"}};
    b.camera(9.0f, 24.0f);
    const Uuid hero = player.uuid();

    // --- Personajes del pueblo ---
    const auto npc = [&](const std::string& name, const Vec3& p, const Vec3& color, const std::string& greeting, bool merchant) {
        ecs::Entity n = b.world.create(name);
        n.setWorldPosition(p + Vec3{0.0f, 1.0f, 0.0f});
        n.setTag("NPC");
        physics::CapsuleCollider& c = n.add<physics::CapsuleCollider>();
        c.radius = 0.45f;
        c.height = 1.9f;
        Builder::attach(n, "NPC.lua",
                        {{"nombre", scripting::PropertyType::Text, name},
                         {"saludo", scripting::PropertyType::Text, greeting},
                         {"comerciante", scripting::PropertyType::Bool, merchant ? "true" : "false"}});
        const assets::AssetRef cloth = b.material("NPC " + name, color, 0.6f);
        ecs::Entity model = b.world.create("Modelo", n);
        primitive(assets::builtin::kCapsule, "Cuerpo", model, Vec3{}, Vec3{0.9f, 0.95f, 0.9f}, cloth);
        primitive(assets::builtin::kCube, "Cara", model, Vec3{0.0f, 0.45f, -0.36f}, Vec3{0.5f, 0.2f, 0.2f}, plaster);
        primitive(assets::builtin::kCube, "Marca", n, Vec3{0.0f, 1.75f, 0.0f}, Vec3{0.32f, 0.32f, 0.32f}, gold,
                  Vec3{45.0f, 0.0f, 45.0f});
        return n;
    };
    npc("Capitana Elena", Vec3{-6.0f, 0.0f, -8.0f}, Vec3{0.75f, 0.2f, 0.2f},
        "Soy la capitana de la guardia de Villa Alba. Estos dias no damos abasto: lobos al oeste, goblins al este...", false);
    npc("Mercader Tomas", Vec3{11.0f, 0.0f, 6.2f}, Vec3{0.3f, 0.55f, 0.3f},
        "¡Bienvenido! Pociones, armas y armaduras al mejor precio de toda la comarca.", true);
    npc("Hermano Anselmo", Vec3{-24.0f, 0.0f, 20.0f}, Vec3{0.85f, 0.85f, 0.8f},
        "Que la luz te guie. Algo oscuro se mueve en la guarida del norte...", false);
    (void)quest_mark;

    // --- Enemigos ---
    int enemy_n = 0;
    const auto health_bar = [&](ecs::Entity e, float height) {
        ecs::Entity bar = b.world.create("Barra", e);
        bar.setLocalPosition(Vec3{0.0f, height, 0.0f});
        bar.setLocalScale(Vec3{1.2f, 0.14f, 0.14f});
        primitive(assets::builtin::kCube, "Fondo", bar, Vec3{}, Vec3{1.0f, 0.8f, 0.8f}, bar_back).get<ecs::MeshRenderer>().cast_shadows =
            ecs::ShadowCasting::Off;
        primitive(assets::builtin::kCube, "Relleno", bar, Vec3{}, Vec3{1.0f, 1.0f, 1.0f}, bar_fill).get<ecs::MeshRenderer>().cast_shadows =
            ecs::ShadowCasting::Off;
        bar.setActive(false);
    };
    const auto enemy = [&](const std::string& type, int level, const Vec3& p) {
        ++enemy_n;
        const bool is_wolf = type == "lobo";
        const bool is_king = type == "rey";
        const std::string label = is_wolf ? "Lobo" : is_king ? "Rey Goblin" : type == "chaman" ? "Chaman" : "Goblin";
        const float half = is_wolf ? 0.45f : is_king ? 1.6f : 0.7f;  // del suelo al pivote
        ecs::Entity e = b.world.create(label + " " + std::to_string(enemy_n));
        e.setWorldPosition(p + Vec3{0.0f, half, 0.0f});
        e.setTag("Enemigo");
        navigation::NavAgent& agent = e.add<navigation::NavAgent>();
        agent.speed = is_wolf ? 4.8f : is_king ? 4.0f : 3.8f;
        agent.base_offset = half;
        agent.radius = is_king ? 0.9f : 0.45f;
        agent.height = half * 2.0f;
        Builder::attach(e, "Enemigo.lua",
                        {{"tipo", scripting::PropertyType::Text, type},
                         {"nivel", scripting::PropertyType::Number, std::to_string(level)}});
        ecs::Entity model = b.world.create("Modelo", e);
        if (is_wolf) {
            primitive(assets::builtin::kCube, "Cuerpo", model, Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.55f, 0.55f, 1.3f}, wolf);
            primitive(assets::builtin::kCube, "Cabeza", model, Vec3{0.0f, 0.2f, -0.8f}, Vec3{0.4f, 0.4f, 0.5f}, wolf);
            primitive(assets::builtin::kCube, "Ojos", model, Vec3{0.0f, 0.28f, -1.06f}, Vec3{0.3f, 0.08f, 0.02f}, eye);
            primitive(assets::builtin::kCube, "Cola", model, Vec3{0.0f, 0.15f, 0.8f}, Vec3{0.14f, 0.14f, 0.5f}, wolf,
                      Vec3{-30.0f, 0.0f, 0.0f});
        } else {
            const assets::AssetRef skin = is_king ? king : type == "chaman" ? shaman : goblin;
            const float s = half / 1.0f;
            primitive(assets::builtin::kCapsule, "Cuerpo", model, Vec3{}, Vec3{s, s, s}, skin);
            primitive(assets::builtin::kCube, "Ojos", model, Vec3{0.0f, 0.45f * s, -0.42f * s}, Vec3{0.45f * s, 0.1f * s, 0.05f}, eye);
            if (is_king) {
                primitive(assets::builtin::kCylinder, "Corona", model, Vec3{0.0f, 1.05f * s, 0.0f}, Vec3{0.6f, 0.25f, 0.6f}, gold);
                primitive(assets::builtin::kCube, "Maza", model, Vec3{0.9f, 0.1f, -0.3f}, Vec3{0.3f, 1.6f, 0.3f}, dark_wood);
            } else if (type == "chaman") {
                primitive(assets::builtin::kCylinder, "Baston", model, Vec3{0.55f, 0.1f, 0.0f}, Vec3{0.08f, 0.9f, 0.08f}, dark_wood);
                primitive(assets::builtin::kSphere, "Orbe", model, Vec3{0.55f, 1.05f, 0.0f}, Vec3{0.22f, 0.22f, 0.22f}, fire);
            } else {
                primitive(assets::builtin::kCube, "Garrote", model, Vec3{0.55f, 0.0f, -0.2f}, Vec3{0.14f, 0.9f, 0.14f}, wood,
                          Vec3{20.0f, 0.0f, 0.0f});
            }
        }
        health_bar(e, is_wolf ? 0.9f : half + 0.55f);
        return e;
    };
    Lcg spots{777u};
    for (int i = 0; i < 8; ++i) {
        const float a = spots.range(0.0f, 6.2832f);
        const float d = spots.range(6.0f, 26.0f);
        enemy("lobo", i < 5 ? 1 : 2, forest + Vec3{std::cos(a) * d, 0.0f, std::sin(a) * d});
    }
    for (int i = 0; i < 8; ++i) {
        const float a = spots.range(0.0f, 6.2832f);
        const float d = spots.range(4.0f, 16.0f);
        enemy(i >= 6 ? "chaman" : "goblin", i < 4 ? 3 : 4, camp + Vec3{std::cos(a) * d, 0.0f, std::sin(a) * d});
    }
    enemy("rey", 6, lair + Vec3{0.0f, 0.0f, -4.0f});
    enemy("goblin", 5, lair + Vec3{-5.0f, 0.0f, 0.0f});
    enemy("goblin", 5, lair + Vec3{5.0f, 0.0f, 0.0f});

    // --- Otros jugadores (simulados) ---
    const struct {
        const char* name;
        const char* role;
        int level;
        Vec3 color;
        Vec3 pos;
    } bots[] = {{"Aria_Luz", "mago", 4, {0.8f, 0.3f, 0.9f}, {4.0f, 0.0f, -4.0f}},
                {"Kraven", "guerrero", 5, {0.85f, 0.45f, 0.1f}, {-4.0f, 0.0f, 4.0f}},
                {"Nube42", "mago", 3, {0.9f, 0.9f, 0.95f}, {6.0f, 0.0f, 3.0f}},
                {"Torvald", "guerrero", 4, {0.35f, 0.35f, 0.4f}, {-6.0f, 0.0f, -3.0f}}};
    for (const auto& info : bots) {
        ecs::Entity bot = b.world.create(info.name);
        bot.setWorldPosition(info.pos + Vec3{0.0f, 1.0f, 0.0f});
        bot.setTag("Bot");
        navigation::NavAgent& agent = bot.add<navigation::NavAgent>();
        agent.speed = 4.6f;
        agent.base_offset = 1.0f;
        Builder::attach(bot, "Bot.lua",
                        {{"nombre", scripting::PropertyType::Text, info.name},
                         {"clase", scripting::PropertyType::Text, info.role},
                         {"nivel", scripting::PropertyType::Number, std::to_string(info.level)}});
        ecs::Entity model = b.world.create("Modelo", bot);
        const assets::AssetRef cloth = b.material(std::string("Bot ") + info.name, info.color, 0.45f);
        primitive(assets::builtin::kCapsule, "Cuerpo", model, Vec3{}, Vec3{0.9f, 0.95f, 0.9f}, cloth);
        primitive(assets::builtin::kCube, "Visor", model, Vec3{0.0f, 0.45f, -0.36f}, Vec3{0.62f, 0.22f, 0.22f}, visor);
        const bool mage = std::string(info.role) == "mago";
        primitive(assets::builtin::kCube, mage ? "Baston" : "Espada", model, Vec3{0.55f, 0.1f, -0.2f},
                  mage ? Vec3{0.08f, 1.6f, 0.08f} : Vec3{0.1f, 1.1f, 0.2f}, mage ? dark_wood : stone);
    }

    // Bolsa de botin: Heroe.lua hace copias donde muere un enemigo.
    ecs::Entity loot = primitive(assets::builtin::kCube, "Plantilla Botin", {}, Vec3{0.0f, -20.0f, 0.0f}, Vec3{0.6f, 0.45f, 0.45f}, gold);
    primitive(assets::builtin::kCube, "Tapa", loot, Vec3{0.0f, 0.6f, 0.0f}, Vec3{1.05f, 0.25f, 1.05f}, wood);
    loot.setActive(false);

    // --- Interfaz ---
    ecs::Entity canvas = b.world.create("HUD");
    canvas.add<ui::Canvas>();
    const auto rect = [&](ecs::Entity e, Vec2 anchor, Vec2 pivot, Vec2 position, Vec2 size) {
        ui::RectTransform& rt = e.add<ui::RectTransform>();
        rt.anchor_min = anchor;
        rt.anchor_max = anchor;
        rt.pivot = pivot;
        rt.position = position;
        rt.size = size;
        return e;
    };
    const auto stretch = [&](ecs::Entity e, float margin = 0.0f) {
        ui::RectTransform& rt = e.add<ui::RectTransform>();
        rt.anchor_min = Vec2{0.0f, 0.0f};
        rt.anchor_max = Vec2{1.0f, 1.0f};
        rt.size = Vec2{-2.0f * margin, -2.0f * margin};
        return e;
    };
    const auto image = [&](ecs::Entity e, Vec3 color, float alpha, float radius = 0.0f) {
        ui::Image& im = e.add<ui::Image>();
        im.color = color;
        im.alpha = alpha;
        im.corner_radius = radius;
        return e;
    };
    const auto text = [&](ecs::Entity e, const std::string& value, float font, ui::HAlign align, Vec3 color = Vec3{1, 1, 1},
                          ui::VAlign valign = ui::VAlign::Middle, bool wrap = false) {
        ui::Text& t = e.add<ui::Text>();
        t.text = value;
        t.font_size = font;
        t.h_align = align;
        t.v_align = valign;
        t.color = color;
        t.shadow = true;
        t.wrap = wrap;
        return e;
    };
    const auto button = [&](ecs::Entity e, const std::string& method, Vec3 color) {
        ui::Button& bt = e.add<ui::Button>();
        bt.normal = color;
        bt.hover = color * 1.35f;
        bt.pressed = color * 0.7f;
        bt.disabled = Vec3{0.16f, 0.16f, 0.17f};
        bt.corner_radius = 6.0f;
        bt.target = hero;
        bt.on_click = method;
        return e;
    };
    const auto child = [&](const std::string& name, ecs::Entity parent) { return b.world.create(name, parent); };
    const Vec2 top_left{0.0f, 0.0f};
    const Vec2 top_right{1.0f, 0.0f};
    const Vec2 top{0.5f, 0.0f};
    const Vec2 bottom{0.5f, 1.0f};
    const Vec2 bottom_left{0.0f, 1.0f};
    const Vec2 center{0.5f, 0.5f};
    // Barra con fondo, relleno (se acorta desde Lua) y texto.
    const auto bar = [&](ecs::Entity parent, const std::string& prefix, Vec2 position, Vec2 size, Vec3 back, Vec3 fill, float font) {
        image(rect(child(prefix + "Fondo", parent), top_left, top_left, position, size), back, 0.9f, 4.0f);
        image(rect(child(prefix + "Barra", parent), top_left, top_left, position, size), fill, 1.0f, 4.0f);
        text(rect(child(prefix + "Texto", parent), top_left, top_left, position, size), "", font, ui::HAlign::Center);
    };
    const Vec3 panel_color{0.06f, 0.07f, 0.09f};
    const auto close_button = [&](ecs::Entity panel) {
        ecs::Entity c = rect(child("Cerrar", panel), top_right, top_right, Vec2{-12.0f, 12.0f}, Vec2{40.0f, 36.0f});
        button(c, "OnCerrar", Vec3{0.45f, 0.14f, 0.14f});
        text(stretch(child("Texto", c)), "X", 22.0f, ui::HAlign::Center);
    };

    // Marco del jugador (arriba a la izquierda) y del objetivo (arriba al centro).
    ecs::Entity frame = image(rect(child("UI_Jugador", canvas), top_left, top_left, Vec2{24, 24}, Vec2{360, 112}), panel_color, 0.65f, 10.0f);
    text(rect(child("Nombre", frame), top_left, top_left, Vec2{20, 8}, Vec2{330, 32}), "", 24.0f, ui::HAlign::Left);
    bar(frame, "Vida", Vec2{20, 46}, Vec2{320, 26}, Vec3{0.25f, 0.04f, 0.04f}, Vec3{0.85f, 0.15f, 0.12f}, 18.0f);
    bar(frame, "Mana", Vec2{20, 78}, Vec2{320, 22}, Vec3{0.04f, 0.07f, 0.25f}, Vec3{0.2f, 0.4f, 0.95f}, 16.0f);
    ecs::Entity target = image(rect(child("UI_Objetivo", canvas), top, top, Vec2{0, 24}, Vec2{360, 84}), panel_color, 0.65f, 10.0f);
    text(rect(child("Nombre", target), top_left, top_left, Vec2{20, 8}, Vec2{330, 32}), "", 24.0f, ui::HAlign::Left);
    bar(target, "Vida", Vec2{20, 46}, Vec2{320, 26}, Vec3{0.25f, 0.04f, 0.04f}, Vec3{0.85f, 0.15f, 0.12f}, 18.0f);
    target.setActive(false);

    // Experiencia (abajo del todo).
    image(rect(child("UI_XPFondo", canvas), bottom, bottom, Vec2{0, -8}, Vec2{900, 14}), Vec3{0.08f, 0.05f, 0.12f}, 0.85f, 4.0f);
    image(rect(child("UI_XPBarra", canvas), bottom, Vec2{0.0f, 1.0f}, Vec2{-450, -8}, Vec2{900, 14}), Vec3{0.62f, 0.3f, 0.95f}, 1.0f, 4.0f);
    text(rect(child("UI_XPTexto", canvas), bottom, bottom, Vec2{0, -24}, Vec2{900, 24}), "", 17.0f, ui::HAlign::Center);

    // Barra de habilidades (1-6).
    static const char* kAbilities[] = {"Golpe", "Bola de fuego", "Curar", "Torbellino", "Vida", "Mana"};
    static const Vec3 kAbilityColors[] = {{0.45f, 0.3f, 0.2f}, {0.7f, 0.28f, 0.08f}, {0.2f, 0.55f, 0.25f},
                                          {0.5f, 0.2f, 0.45f}, {0.55f, 0.12f, 0.12f}, {0.14f, 0.25f, 0.6f}};
    for (int i = 0; i < 6; ++i) {
        ecs::Entity slot = rect(child("UI_Hab " + std::to_string(i + 1), canvas), bottom, bottom,
                                Vec2{(static_cast<float>(i) - 2.5f) * 86.0f, -44.0f}, Vec2{78, 72});
        button(slot, "OnHabilidad", kAbilityColors[i]);
        text(rect(child("Nombre", slot), center, center, Vec2{0, 6}, Vec2{74, 44}), kAbilities[i], 14.0f, ui::HAlign::Center,
             Vec3{1, 1, 1}, ui::VAlign::Middle, true);
        image(rect(child("Enfriamiento", slot), bottom, bottom, Vec2{0, 0}, Vec2{78, 0}), Vec3{0, 0, 0}, 0.62f, 6.0f);
        text(rect(child("Tecla", slot), top_left, top_left, Vec2{6, 2}, Vec2{20, 20}), std::to_string(i + 1), 16.0f, ui::HAlign::Left,
             Vec3{1.0f, 0.9f, 0.5f});
        text(rect(child("Tiempo", slot), center, center, Vec2{0, 0}, Vec2{74, 34}), "", 26.0f, ui::HAlign::Center);
    }
    // Barra de lanzamiento.
    ecs::Entity cast = image(rect(child("UI_Lanzamiento", canvas), bottom, bottom, Vec2{0, -150}, Vec2{404, 30}), panel_color, 0.8f, 6.0f);
    image(rect(child("Barra", cast), top_left, top_left, Vec2{2, 2}, Vec2{400, 26}), Vec3{0.95f, 0.6f, 0.15f}, 1.0f, 5.0f);
    text(stretch(child("Texto", cast)), "", 18.0f, ui::HAlign::Center);
    cast.setActive(false);

    // Chat (abajo a la izquierda), misiones (derecha) y minimapa (arriba a la derecha).
    ecs::Entity chat = image(rect(child("UI_ChatFondo", canvas), bottom_left, bottom_left, Vec2{24, -24}, Vec2{660, 250}), panel_color,
                             0.4f, 8.0f);
    text(stretch(child("UI_Chat", chat), 12.0f), "", 17.0f, ui::HAlign::Left, Vec3{0.92f, 0.92f, 0.95f}, ui::VAlign::Bottom, true);
    text(rect(child("UI_Seguimiento", canvas), top_right, top_right, Vec2{-24, 300}, Vec2{430, 260}), "", 19.0f, ui::HAlign::Left,
         Vec3{1.0f, 0.93f, 0.7f}, ui::VAlign::Top, true);
    ecs::Entity map = image(rect(child("UI_Mapa", canvas), top_right, top_right, Vec2{-24, 24}, Vec2{230, 230}), Vec3{0.1f, 0.14f, 0.1f},
                            0.7f, 115.0f);
    for (int i = 1; i <= 40; ++i) {
        ecs::Entity dot = image(rect(child("Punto " + std::to_string(i), map), center, center, Vec2{0, 0}, Vec2{7, 7}), Vec3{1, 0, 0},
                                1.0f, 4.0f);
        dot.setActive(false);
    }
    image(rect(child("Yo", map), center, center, Vec2{0, 0}, Vec2{11, 11}), Vec3{1, 1, 1}, 1.0f, 6.0f);
    text(rect(child("Norte", map), top, top, Vec2{0, 4}, Vec2{30, 24}), "N", 18.0f, ui::HAlign::Center);
    text(rect(child("UI_Zona", canvas), top_right, top_right, Vec2{-24, 258}, Vec2{230, 30}), "", 19.0f, ui::HAlign::Center,
         Vec3{0.85f, 1.0f, 0.8f});

    // Avisos grandes y numeros flotantes.
    ecs::Entity notice = text(rect(child("UI_Aviso", canvas), top, top, Vec2{0, 130}, Vec2{1300, 50}), "", 34.0f, ui::HAlign::Center);
    notice.get<ui::Text>().alpha = 0.0f;
    for (int i = 1; i <= 8; ++i) {
        ecs::Entity f = text(rect(child("UI_Flotante " + std::to_string(i), canvas), center, center, Vec2{0, -120}, Vec2{300, 50}), "",
                             36.0f, ui::HAlign::Center);
        f.get<ui::Text>().alpha = 0.0f;
    }

    // Inventario (I): 20 ranuras.
    ecs::Entity inventory = image(rect(child("UI_Inventario", canvas), center, center, Vec2{-430, -10}, Vec2{530, 600}), panel_color,
                                  0.94f, 12.0f);
    text(rect(child("Titulo", inventory), top_left, top_left, Vec2{22, 14}, Vec2{400, 40}), "Inventario (I)", 30.0f, ui::HAlign::Left);
    text(rect(child("Oro", inventory), top_left, top_left, Vec2{22, 56}, Vec2{400, 30}), "", 21.0f, ui::HAlign::Left,
         Vec3{1.0f, 0.85f, 0.35f});
    for (int i = 0; i < 20; ++i) {
        ecs::Entity s = rect(child("Ranura " + std::to_string(i + 1), inventory), top_left, top_left,
                             Vec2{22.0f + static_cast<float>(i % 4) * 124.0f, 96.0f + static_cast<float>(i / 4) * 96.0f}, Vec2{116, 88});
        button(s, "OnRanura", Vec3{0.15f, 0.16f, 0.2f});
        image(rect(child("Color", s), top_left, top_left, Vec2{6, 6}, Vec2{16, 16}), Vec3{1, 1, 1}, 0.0f, 3.0f);
        text(rect(child("Nombre", s), center, center, Vec2{0, 4}, Vec2{108, 58}), "", 15.0f, ui::HAlign::Center, Vec3{1, 1, 1},
             ui::VAlign::Middle, true);
        text(rect(child("Cantidad", s), Vec2{1.0f, 1.0f}, Vec2{1.0f, 1.0f}, Vec2{-6, -2}, Vec2{60, 24}), "", 16.0f, ui::HAlign::Right);
    }
    text(rect(child("Pista", inventory), bottom_left, bottom_left, Vec2{22, -14}, Vec2{480, 28}), "", 17.0f, ui::HAlign::Left,
         Vec3{0.7f, 0.72f, 0.78f});
    close_button(inventory);
    inventory.setActive(false);

    // Personaje (C): estadisticas y equipo.
    ecs::Entity sheet = image(rect(child("UI_Personaje", canvas), center, center, Vec2{430, -10}, Vec2{470, 560}), panel_color, 0.94f, 12.0f);
    text(rect(child("Titulo", sheet), top_left, top_left, Vec2{22, 14}, Vec2{400, 40}), "Personaje (C)", 30.0f, ui::HAlign::Left);
    text(rect(child("Stats", sheet), top_left, top_left, Vec2{22, 64}, Vec2{420, 330}), "", 20.0f, ui::HAlign::Left, Vec3{1, 1, 1},
         ui::VAlign::Top, true);
    for (int i = 0; i < 2; ++i) {
        ecs::Entity slot = rect(child(i == 0 ? "Arma" : "Armadura", sheet), top_left, top_left, Vec2{22.0f, 410.0f + 62.0f * static_cast<float>(i)},
                                Vec2{426, 52});
        button(slot, "OnDesequipar", Vec3{0.2f, 0.22f, 0.3f});
        text(stretch(child("Texto", slot), 10.0f), "", 18.0f, ui::HAlign::Left);
    }
    close_button(sheet);
    sheet.setActive(false);

    // Diario de misiones (L).
    ecs::Entity journal = image(rect(child("UI_Diario", canvas), center, center, Vec2{0, -10}, Vec2{980, 640}), panel_color, 0.95f, 12.0f);
    text(rect(child("Titulo", journal), top_left, top_left, Vec2{24, 14}, Vec2{600, 40}), "Diario de misiones (L)", 30.0f, ui::HAlign::Left);
    text(rect(child("Texto", journal), top_left, top_left, Vec2{24, 70}, Vec2{930, 550}), "", 19.0f, ui::HAlign::Left, Vec3{1, 1, 1},
         ui::VAlign::Top, true);
    close_button(journal);
    journal.setActive(false);

    // Dialogo con un NPC (misiones y tienda).
    ecs::Entity dialog = image(rect(child("UI_Dialogo", canvas), center, center, Vec2{320, -10}, Vec2{640, 640}), panel_color, 0.95f, 12.0f);
    text(rect(child("Titulo", dialog), top_left, top_left, Vec2{24, 16}, Vec2{540, 44}), "", 30.0f, ui::HAlign::Left,
         Vec3{1.0f, 0.85f, 0.4f});
    text(rect(child("Cuerpo", dialog), top_left, top_left, Vec2{24, 70}, Vec2{592, 250}), "", 20.0f, ui::HAlign::Left, Vec3{1, 1, 1},
         ui::VAlign::Top, true);
    for (int i = 0; i < 6; ++i) {
        ecs::Entity o = rect(child("Opcion " + std::to_string(i + 1), dialog), top_left, top_left,
                             Vec2{24.0f, 336.0f + 48.0f * static_cast<float>(i)}, Vec2{592, 42});
        button(o, "OnOpcion", Vec3{0.16f, 0.3f, 0.5f});
        text(stretch(child("Texto", o), 8.0f), "", 19.0f, ui::HAlign::Left);
    }
    close_button(dialog);
    dialog.setActive(false);

    // Muerte y ayuda.
    ecs::Entity death = image(stretch(child("UI_Muerte", canvas)), Vec3{0.35f, 0.02f, 0.02f}, 0.55f);
    text(rect(child("Titulo", death), center, center, Vec2{0, -100}, Vec2{900, 90}), "Has muerto", 72.0f, ui::HAlign::Center);
    text(rect(child("Causa", death), center, center, Vec2{0, -20}, Vec2{900, 50}), "", 30.0f, ui::HAlign::Center, Vec3{1.0f, 0.8f, 0.8f});
    ecs::Entity respawn = rect(child("Reaparecer", death), center, center, Vec2{0, 80}, Vec2{380, 66});
    button(respawn, "OnReaparecer", Vec3{0.5f, 0.12f, 0.12f});
    text(stretch(child("Texto", respawn)), "Reaparecer en el pueblo", 26.0f, ui::HAlign::Center);
    death.setActive(false);

    ecs::Entity help = image(rect(child("UI_Ayuda", canvas), center, center, Vec2{0, -20}, Vec2{820, 560}), panel_color, 0.95f, 12.0f);
    text(rect(child("Titulo", help), top_left, top_left, Vec2{24, 14}, Vec2{600, 40}), "Controles (H)", 30.0f, ui::HAlign::Left);
    text(rect(child("Texto", help), top_left, top_left, Vec2{24, 70}, Vec2{770, 470}),
         "WASD  moverse          Shift  correr          Espacio  saltar\n"
         "Clic derecho + raton  girar la camara          Rueda  acercar\n\n"
         "Tab  siguiente enemigo          Clic izquierdo  el enemigo al que miras\n"
         "1  Golpe (cuerpo a cuerpo)          2  Bola de fuego (a distancia)\n"
         "3  Curar (nivel 2)          4  Torbellino en area (nivel 3)\n"
         "5 / 6  pociones de vida y de mana\n\n"
         "E  hablar con un personaje (! = mision nueva, azul = para entregar)\n"
         "I  inventario (clic: usar, equipar o vender)          C  personaje\n"
         "L  diario de misiones          Escape  cerrar / quitar objetivo\n"
         "F9  borrar la partida guardada\n\n"
         "La partida se guarda sola. Los otros jugadores del mundo tambien cazan.",
         20.0f, ui::HAlign::Left, Vec3{1, 1, 1}, ui::VAlign::Top, true);
    close_button(help);
    help.setActive(false);

    b.save("Main");

    std::vector<std::string> tags = ecs::defaultTags();
    for (const char* t : {"Enemigo", "NPC", "Bot"}) tags.push_back(t);
    ecs::saveTags(project.settingsFolder() / "Tags.json", tags);
    navigation::NavigationSettings nav;
    navigation::saveNavigationSettings(project.settingsFolder() / "Navigation.json", nav);
}

// --- Criaturas: IK de animales, phys bones, ragdoll y huesos ------------------------

void buildCreatures(project::ProjectInfo& project) {
    Builder b(project);
    b.world.setSceneUuid(Uuid::generate());
    ecs::populateDefaultScene(b.world);
    b.script("Perro.lua", creature_scripts::kDog);
    b.script("Maniqui.lua", creature_scripts::kDummy);
    b.script("Pelota.lua", creature_scripts::kBall);
    b.script("Controles.lua", creature_scripts::kControls);
    b.script("CamaraTercera.lua", kCameraScript);

    // Los dos modelos con esqueleto, generados (como si se hubieran importado).
    const asset::ModelData dog_model = makeDogModel();
    const asset::ModelData dummy_model = makeDummyModel();
    const Uuid dog_uuid = Uuid::generate();
    const Uuid dummy_uuid = Uuid::generate();
    std::string error;
    if (!writeCreatureModel(project.assetsFolder() / "Models" / "Perro.crdata", dog_uuid, "Perro", dog_model, &error) ||
        !writeCreatureModel(project.assetsFolder() / "Models" / "Maniqui.crdata", dummy_uuid, "Maniqui", dummy_model, &error)) {
        throw std::runtime_error(error);
    }

    const assets::AssetRef floor = b.material("Cesped", Vec3{0.42f, 0.58f, 0.3f}, 0.95f);
    const assets::AssetRef stone = b.material("Piedra", Vec3{0.55f, 0.55f, 0.58f}, 0.85f);
    const assets::AssetRef wood = b.material("Madera", Vec3{0.62f, 0.44f, 0.26f}, 0.8f);
    const assets::AssetRef red = b.material("Sombrero", Vec3{0.85f, 0.15f, 0.12f}, 0.5f);
    const assets::AssetRef ball = b.material("Pelota", Vec3{1.0f, 0.82f, 0.2f}, 0.35f, 0.0f, Vec3{1.0f, 0.7f, 0.1f}, 0.4f);

    b.box("Suelo", Vec3{0.0f, -0.5f, 0.0f}, Vec3{30.0f, 1.0f, 30.0f}, floor);
    // Escalera: sube tres escalones, un rellano y baja (en el lado de delante).
    const float step = 0.12f;
    for (int i = 0; i < 3; ++i) {
        const float h = step * static_cast<float>(i + 1);
        b.box("Escalon subida " + std::to_string(i + 1), Vec3{0.0f + 0.6f * static_cast<float>(i), h * 0.5f, 7.0f},
              Vec3{0.6f, h, 2.0f}, wood);
        b.box("Escalon bajada " + std::to_string(i + 1), Vec3{4.8f - 0.6f * static_cast<float>(i), h * 0.5f, 7.0f},
              Vec3{0.6f, h, 2.0f}, wood);
    }
    b.box("Rellano", Vec3{2.4f, step * 1.5f, 7.0f}, Vec3{1.8f, step * 3.0f, 2.0f}, wood);
    // Rampa y bajada (lado derecho): el perro se inclina con la pendiente.
    b.box("Rampa", Vec3{7.0f, 0.2f, -0.5f}, Vec3{2.4f, 0.2f, 4.2f}, stone, Vec3{10.0f, 0.0f, 0.0f});
    b.box("Meseta", Vec3{7.0f, 0.46f, -3.4f}, Vec3{2.4f, 0.2f, 1.8f}, stone);
    b.box("Bajada", Vec3{7.0f, 0.2f, -6.0f}, Vec3{2.4f, 0.2f, 3.6f}, stone, Vec3{-14.0f, 0.0f, 0.0f});
    // Piedras sueltas (lado de atras): cada pata pisa a una altura.
    const Vec3 rocks[] = {{-1.5f, 0.06f, -7.0f}, {-2.6f, 0.1f, -6.6f}, {-3.8f, 0.05f, -7.2f},
                          {-4.9f, 0.12f, -6.8f}, {-6.0f, 0.07f, -6.3f}};
    int r = 0;
    for (const Vec3& p : rocks) {
        const float h = p.y * 2.0f;
        b.box("Piedra " + std::to_string(++r), p, Vec3{0.7f, h, 0.6f}, stone, Vec3{0.0f, 25.0f * static_cast<float>(r), 4.0f});
    }

    // Pelota que siguen con la cabeza.
    ecs::Entity pelota = ecs::createPrimitive(b.world, assets::builtin::kSphere, "Pelota");
    pelota.setLocalScale(Vec3{0.3f, 0.3f, 0.3f});
    pelota.get<ecs::MeshRenderer>().materials = {ball};
    Builder::attach(pelota, "Pelota.lua");

    // --- El perro ---
    ecs::Entity dog = b.world.create("Perro");
    dog.setWorldPosition(Vec3{-7.0f, 0.0f, 7.0f});
    dog.add<ecs::MeshRenderer>().model = assets::AssetRef{dog_uuid, assets::AssetType::Model};
    dog.add<ecs::Animator>().clip_name = "Caminar";
    ecs::InverseKinematics& dog_ik = dog.add<ecs::InverseKinematics>();
    ecs::suggestCreatureIK(dog_model, dog_ik);  // 4 patas al suelo, cabeza y cuello
    dog_ik.look_weight = 0.7f;
    dog_ik.look_max_angle = 80.0f;
    dog_ik.max_step = 0.4f;
    dog.add<ecs::PhysBones>().chains = ecs::suggestPhysBones(dog_model);  // cola y orejas
    ecs::Ragdoll& dog_rag = dog.add<ecs::Ragdoll>();
    dog_rag.mass = 25.0f;
    dog_rag.bones = ecs::suggestRagdollBones(dog_model, 1.0f);
    ecs::Skeleton& dog_skeleton = dog.add<ecs::Skeleton>();
    dog_skeleton.show_bones = false;
    Builder::attach(dog, "Perro.lua");
    // Sombrero enganchado a la cabeza (Bone Socket) y un collider en el
    // cuerpo para que la cola no lo atraviese.
    ecs::Entity hat_socket = b.world.create("Sombrero", dog);
    ecs::BoneSocket& hat = hat_socket.add<ecs::BoneSocket>();
    hat.bone = "Head";
    hat.position = Vec3{0.0f, 0.1f, -0.02f};
    ecs::Entity hat_mesh = ecs::createPrimitive(b.world, assets::builtin::kCylinder, "Copa", hat_socket);
    hat_mesh.setLocalScale(Vec3{0.1f, 0.05f, 0.1f});
    hat_mesh.get<ecs::MeshRenderer>().materials = {red};
    ecs::Entity brim = ecs::createPrimitive(b.world, assets::builtin::kCylinder, "Ala", hat_socket);
    brim.setLocalPosition(Vec3{0.0f, -0.045f, 0.0f});
    brim.setLocalScale(Vec3{0.2f, 0.01f, 0.2f});
    brim.get<ecs::MeshRenderer>().materials = {red};
    ecs::Entity body_collider = b.world.create("Collider cuerpo", dog);
    body_collider.add<ecs::BoneSocket>().bone = "Body";
    ecs::PhysBoneCollider& body_sphere = body_collider.add<ecs::PhysBoneCollider>();
    body_sphere.radius = 0.14f;

    // --- El maniqui (un pie en un escalon) ---
    b.box("Escalon del maniqui", Vec3{-3.1f, 0.08f, -1.5f}, Vec3{0.3f, 0.16f, 0.5f}, wood);
    ecs::Entity dummy = b.world.create("Maniqui");
    dummy.setWorldPosition(Vec3{-3.0f, 0.0f, -1.5f});
    dummy.add<ecs::MeshRenderer>().model = assets::AssetRef{dummy_uuid, assets::AssetType::Model};
    dummy.add<ecs::Animator>().clip_name = "Reposo";
    ecs::InverseKinematics& dummy_ik = dummy.add<ecs::InverseKinematics>();
    ecs::suggestCreatureIK(dummy_model, dummy_ik);  // pies en el suelo
    dummy_ik.look_weight = 0.9f;
    dummy.add<ecs::PhysBones>().chains = ecs::suggestPhysBones(dummy_model);  // la coleta
    ecs::Ragdoll& dummy_rag = dummy.add<ecs::Ragdoll>();
    dummy_rag.mass = 70.0f;
    dummy_rag.blend_out = 0.6f;
    dummy.add<ecs::Skeleton>().show_bones = false;
    Builder::attach(dummy, "Maniqui.lua");
    ecs::Entity head_collider = b.world.create("Collider cabeza", dummy);
    head_collider.add<ecs::BoneSocket>().bone = "Head";
    ecs::PhysBoneCollider& head_sphere = head_collider.add<ecs::PhysBoneCollider>();
    head_sphere.radius = 0.11f;
    head_sphere.offset = Vec3{0.0f, 0.09f, 0.0f};

    // Camara que sigue al perro y el texto de ayuda.
    ecs::Entity cam = b.world.findByName("Main Camera");
    cam.setWorldPosition(Vec3{0.0f, 4.0f, 14.0f});
    Builder::attach(cam, "CamaraTercera.lua",
                    {{"objetivo", scripting::PropertyType::Text, "Perro"},
                     {"distancia", scripting::PropertyType::Number, "5.5"},
                     {"altura", scripting::PropertyType::Number, "0.4"},
                     {"inclinacion", scripting::PropertyType::Number, "22"}});
    ecs::Entity help = b.hud("Controles.lua");
    help.get<ui::Text>().font_size = 24.0f;
    help.get<ui::RectTransform>().size = Vec2{1800.0f, 40.0f};
    b.save("Main");
}

// Online: todo el juego en Lua (Red.lua). Arena con porterias, balon, cajas
// y monedas (del servidor), jugadores sincronizados, chat, marcador y eventos.
void buildOnline(project::ProjectInfo& project) {
    Builder b(project);
    b.world.setSceneUuid(Uuid::generate());
    ecs::populateDefaultScene(b.world);
    b.script("Red.lua", online::kNetGameScript);
    b.script("JugadorRed.lua", online::kNetPlayerScript);
    b.script("MonedaRed.lua", online::kNetCoinScript);
    b.script("CamaraTercera.lua", kCameraScript);

    const assets::AssetRef grass = b.material("Cesped", Vec3{0.30f, 0.55f, 0.28f}, 0.95f);
    const assets::AssetRef line = b.material("Linea", Vec3{0.95f, 0.95f, 0.95f}, 0.8f);
    const assets::AssetRef wall = b.material("Muro", Vec3{0.62f, 0.64f, 0.68f}, 0.85f);
    const assets::AssetRef post = b.material("Porteria", Vec3{0.95f, 0.95f, 0.97f}, 0.4f, 0.2f);
    const assets::AssetRef net_blue = b.material("Red azul", Vec3{0.15f, 0.35f, 0.85f}, 0.9f);
    const assets::AssetRef net_red = b.material("Red roja", Vec3{0.85f, 0.18f, 0.15f}, 0.9f);
    const assets::AssetRef wood = b.material("Madera", Vec3{0.55f, 0.38f, 0.22f}, 0.85f);
    const assets::AssetRef ball = b.material("Balon", Vec3{0.96f, 0.96f, 0.96f}, 0.45f);
    const assets::AssetRef gold = b.material("Oro", Vec3{1.0f, 0.78f, 0.25f}, 0.25f, 1.0f, Vec3{1.0f, 0.7f, 0.2f}, 0.6f);
    const assets::AssetRef visor = b.material("Visor", Vec3{0.05f, 0.06f, 0.08f}, 0.15f, 0.6f);
    // Un color por jugador (JugadorRed.lua elige el suyo por su id).
    const std::pair<const char*, Vec3> kColors[] = {
        {"Azul", {0.10f, 0.45f, 0.95f}},  {"Rojo", {0.90f, 0.15f, 0.12f}},    {"Verde", {0.20f, 0.75f, 0.25f}},
        {"Amarillo", {0.98f, 0.82f, 0.10f}}, {"Morado", {0.55f, 0.25f, 0.85f}}, {"Naranja", {0.98f, 0.50f, 0.10f}},
        {"Cian", {0.10f, 0.80f, 0.85f}},  {"Rosa", {0.95f, 0.40f, 0.70f}},
    };
    assets::AssetRef first_color{};
    for (const auto& [name, color] : kColors) {
        const assets::AssetRef m = b.material(std::string("Jugador ") + name, color, 0.35f);
        if (!first_color.valid()) first_color = m;
    }

    // --- Arena: 52 x 52 m con muros y una porteria a cada lado (x = +-26) ---
    constexpr float kHalf = 26.0f;
    constexpr float kGoal = 4.0f;  // media anchura de la porteria
    b.box("Suelo", Vec3{0.0f, -0.5f, 0.0f}, Vec3{kHalf * 2.0f + 8.0f, 1.0f, kHalf * 2.0f}, grass);
    const auto flat = [&](const std::string& name, const Vec3& p, const Vec3& s) {
        ecs::Entity e = b.box(name, p, s, line);
        e.remove<physics::BoxCollider>();
        return e;
    };
    flat("Linea central", Vec3{0.0f, 0.005f, 0.0f}, Vec3{0.25f, 0.01f, kHalf * 2.0f});
    flat("Area azul", Vec3{-kHalf + 3.0f, 0.005f, 0.0f}, Vec3{0.25f, 0.01f, kGoal * 2.0f + 4.0f});
    flat("Area roja", Vec3{kHalf - 3.0f, 0.005f, 0.0f}, Vec3{0.25f, 0.01f, kGoal * 2.0f + 4.0f});
    b.box("Muro Norte", Vec3{0.0f, 1.0f, -kHalf}, Vec3{kHalf * 2.0f + 0.6f, 2.0f, 0.6f}, wall);
    b.box("Muro Sur", Vec3{0.0f, 1.0f, kHalf}, Vec3{kHalf * 2.0f + 0.6f, 2.0f, 0.6f}, wall);
    const float side = (kHalf - kGoal);
    for (const float sx : {-1.0f, 1.0f}) {
        const std::string tag = sx < 0.0f ? " Oeste" : " Este";
        const assets::AssetRef net = sx < 0.0f ? net_blue : net_red;
        // Muro con el hueco de la porteria.
        b.box("Muro" + tag + " 1", Vec3{sx * kHalf, 1.0f, -(kGoal + side * 0.5f)}, Vec3{0.6f, 2.0f, side}, wall);
        b.box("Muro" + tag + " 2", Vec3{sx * kHalf, 1.0f, kGoal + side * 0.5f}, Vec3{0.6f, 2.0f, side}, wall);
        // Porteria: fondo, lados, larguero y postes.
        b.box("Porteria" + tag + " fondo", Vec3{sx * (kHalf + 3.0f), 1.25f, 0.0f}, Vec3{0.3f, 2.5f, kGoal * 2.0f}, net);
        b.box("Porteria" + tag + " lado 1", Vec3{sx * (kHalf + 1.5f), 1.25f, -kGoal}, Vec3{3.0f, 2.5f, 0.2f}, net);
        b.box("Porteria" + tag + " lado 2", Vec3{sx * (kHalf + 1.5f), 1.25f, kGoal}, Vec3{3.0f, 2.5f, 0.2f}, net);
        b.box("Porteria" + tag + " techo", Vec3{sx * (kHalf + 1.5f), 2.55f, 0.0f}, Vec3{3.0f, 0.1f, kGoal * 2.0f}, net);
        b.box("Larguero" + tag, Vec3{sx * kHalf, 2.5f, 0.0f}, Vec3{0.3f, 0.3f, kGoal * 2.0f + 0.3f}, post);
        b.box("Poste" + tag + " 1", Vec3{sx * kHalf, 1.25f, -kGoal}, Vec3{0.3f, 2.5f, 0.3f}, post);
        b.box("Poste" + tag + " 2", Vec3{sx * kHalf, 1.25f, kGoal}, Vec3{0.3f, 2.5f, 0.3f}, post);
    }
    // Rampas y plataformas en las esquinas (para saltar y esconder monedas).
    b.box("Rampa 1", Vec3{-15.0f, 0.9f, -17.0f}, Vec3{4.0f, 0.4f, 8.0f}, wall, Vec3{-13.0f, 0.0f, 0.0f});
    b.box("Plataforma 1", Vec3{-15.0f, 1.8f, -22.5f}, Vec3{6.0f, 0.4f, 4.0f}, wall);
    b.box("Rampa 2", Vec3{15.0f, 0.9f, 17.0f}, Vec3{4.0f, 0.4f, 8.0f}, wall, Vec3{13.0f, 0.0f, 0.0f});
    b.box("Plataforma 2", Vec3{15.0f, 1.8f, 22.5f}, Vec3{6.0f, 0.4f, 4.0f}, wall);

    // Camara: vista general en el menu; en la partida sigue a tu jugador.
    ecs::Entity cam = b.world.findByName("Main Camera");
    cam.setWorldPosition(Vec3{0.0f, 30.0f, 40.0f});
    cam.setLocalEulerDegrees(Vec3{-36.0f, 0.0f, 0.0f});
    Builder::attach(cam, "CamaraTercera.lua",
                    {{"objetivo", scripting::PropertyType::Text, ""},
                     {"distancia", scripting::PropertyType::Number, "9"},
                     {"inclinacion", scripting::PropertyType::Number, "22"}});

    // El juego.
    ecs::Entity game = b.world.create("Red");
    Builder::attach(game, "Red.lua");
    const Uuid game_id = game.uuid();

    // --- Prefabs de red (se crean en otro mundo: no van en la escena) ---
    {
        ecs::World pw;
        const std::filesystem::path folder = project.assetsFolder() / "Prefabs";
        std::filesystem::create_directories(folder);
        const auto make = [&](ecs::Entity root, const char* file) {
            std::string error;
            if (!ecs::createPrefab(pw, root, folder / file, &error)) throw std::runtime_error(std::string(file) + ": " + error);
        };
        const auto mesh = [&](const Uuid& shape, const std::string& name, ecs::Entity parent, const Vec3& p, const Vec3& s,
                              const assets::AssetRef& m, const Vec3& euler = Vec3{}) {
            ecs::Entity e = ecs::createPrimitive(pw, shape, name, parent);
            e.setLocalPosition(p);
            e.setLocalScale(s);
            e.setLocalEulerDegrees(euler);
            e.get<ecs::MeshRenderer>().materials = {m};
            return e;
        };

        // Jugador: capsula con Rigidbody (como la plantilla Tercera persona) y su script.
        ecs::Entity player = pw.create("Jugador");
        physics::CapsuleCollider& capsule = player.add<physics::CapsuleCollider>();
        capsule.radius = 0.45f;
        capsule.height = 1.9f;
        capsule.material.friction = 0.0f;
        physics::Rigidbody& rb = player.add<physics::Rigidbody>();
        rb.mass = 70.0f;
        rb.lock_rotation_x = rb.lock_rotation_y = rb.lock_rotation_z = true;
        rb.continuous = true;
        net::NetworkObject& pn = player.add<net::NetworkObject>();
        pn.send_rate = 20.0f;
        pn.smoothing = 14.0f;
        Builder::attach(player, "JugadorRed.lua");
        ecs::Entity model = pw.create("Modelo", player);
        mesh(assets::builtin::kCapsule, "Cuerpo", model, Vec3{}, Vec3{0.9f, 0.95f, 0.9f}, first_color);
        mesh(assets::builtin::kCube, "Visor", model, Vec3{0.0f, 0.45f, -0.36f}, Vec3{0.62f, 0.22f, 0.22f}, visor);
        make(player, "Jugador.crprefab");

        // Moneda: no manda su posicion (esta quieta; cada uno la gira en su pantalla).
        ecs::Entity coin = pw.create("Moneda");
        mesh(assets::builtin::kCylinder, "Disco", coin, Vec3{}, Vec3{0.8f, 0.08f, 0.8f}, gold, Vec3{90.0f, 0.0f, 0.0f});
        coin.add<net::NetworkObject>().sync_transform = false;
        Builder::attach(coin, "MonedaRed.lua");
        make(coin, "Moneda.crprefab");

        // Balon y caja: los simula el servidor y, con fisica local, tambien
        // cada cliente (asi cualquiera los empuja al pasar; el servidor corrige).
        ecs::Entity ball_e = mesh(assets::builtin::kSphere, "Balon", {}, Vec3{}, Vec3{0.7f, 0.7f, 0.7f}, ball);
        physics::SphereCollider& sphere = ball_e.add<physics::SphereCollider>();
        sphere.material.bounciness = 0.55f;
        sphere.material.friction = 0.4f;
        physics::Rigidbody& brb = ball_e.add<physics::Rigidbody>();
        brb.mass = 0.8f;
        brb.linear_damping = 0.25f;
        brb.angular_damping = 0.4f;
        brb.continuous = true;
        net::NetworkObject& bn = ball_e.add<net::NetworkObject>();
        bn.send_rate = 30.0f;
        bn.smoothing = 18.0f;
        bn.local_physics = true;
        make(ball_e, "Balon.crprefab");

        ecs::Entity crate = mesh(assets::builtin::kCube, "Caja", {}, Vec3{}, Vec3{1.3f, 1.3f, 1.3f}, wood);
        crate.add<physics::BoxCollider>();
        crate.add<physics::Rigidbody>().mass = 8.0f;
        net::NetworkObject& cn = crate.add<net::NetworkObject>();
        cn.send_rate = 20.0f;
        cn.local_physics = true;
        make(crate, "Caja.crprefab");
    }

    // --- Interfaz ---
    ecs::Entity canvas = b.world.create("HUD");
    canvas.add<ui::Canvas>();
    const auto rect = [&](ecs::Entity e, Vec2 anchor, Vec2 pivot, Vec2 position, Vec2 size) {
        ui::RectTransform& rt = e.add<ui::RectTransform>();
        rt.anchor_min = anchor;
        rt.anchor_max = anchor;
        rt.pivot = pivot;
        rt.position = position;
        rt.size = size;
        return e;
    };
    const auto stretch = [&](ecs::Entity e, float margin = 0.0f) {
        ui::RectTransform& rt = e.add<ui::RectTransform>();
        rt.anchor_min = Vec2{0.0f, 0.0f};
        rt.anchor_max = Vec2{1.0f, 1.0f};
        rt.size = Vec2{-2.0f * margin, -2.0f * margin};
        return e;
    };
    const auto image = [&](ecs::Entity e, Vec3 color, float alpha, float radius = 0.0f) {
        ui::Image& im = e.add<ui::Image>();
        im.color = color;
        im.alpha = alpha;
        im.corner_radius = radius;
        return e;
    };
    const auto text = [&](ecs::Entity e, const std::string& value, float font, ui::HAlign align, Vec3 color = Vec3{1, 1, 1},
                          ui::VAlign valign = ui::VAlign::Middle, bool wrap = false) {
        ui::Text& t = e.add<ui::Text>();
        t.text = value;
        t.font_size = font;
        t.h_align = align;
        t.v_align = valign;
        t.color = color;
        t.shadow = true;
        t.wrap = wrap;
        return e;
    };
    const auto button = [&](ecs::Entity e, const std::string& label, const std::string& method, Vec3 color) {
        ui::Button& bt = e.add<ui::Button>();
        bt.normal = color;
        bt.hover = color * 1.3f;
        bt.pressed = color * 0.7f;
        bt.corner_radius = 8.0f;
        bt.target = game_id;
        bt.on_click = method;
        ecs::Entity t = b.world.create("Texto", e);
        ui::RectTransform& rt = t.add<ui::RectTransform>();
        rt.anchor_min = Vec2{0.0f, 0.0f};
        rt.anchor_max = Vec2{1.0f, 1.0f};
        ui::Text& tx = t.add<ui::Text>();
        tx.text = label;
        tx.font_size = 26.0f;
        tx.h_align = ui::HAlign::Center;
        tx.shadow = true;
        return e;
    };
    const auto field = [&](ecs::Entity e, const std::string& value, const std::string& placeholder, int max_length,
                           const std::string& on_submit = {}) {
        ui::InputField& f = e.add<ui::InputField>();
        f.text = value;
        f.placeholder = placeholder;
        f.font_size = 24.0f;
        f.max_length = max_length;
        f.target = game_id;
        f.on_submit = on_submit;
        return e;
    };
    const auto child = [&](const std::string& name, ecs::Entity parent) { return b.world.create(name, parent); };
    const Vec2 top_left{0.0f, 0.0f}, top_right{1.0f, 0.0f}, top{0.5f, 0.0f}, bottom_left{0.0f, 1.0f}, center{0.5f, 0.5f};
    const Vec3 panel{0.06f, 0.07f, 0.09f};

    // Menu: nombre, IP y crear / unirse.
    ecs::Entity menu = image(rect(child("UI_Menu", canvas), center, center, Vec2{0, 0}, Vec2{620, 470}), panel, 0.88f, 14.0f);
    text(rect(child("Titulo", menu), top_left, top_left, Vec2{30, 22}, Vec2{560, 50}), "Cramion Online", 44.0f, ui::HAlign::Left,
         Vec3{0.45f, 0.75f, 1.0f});
    text(rect(child("Sub", menu), top_left, top_left, Vec2{30, 74}, Vec2{560, 30}), "Arena con balon, monedas, chat y eventos", 20.0f,
         ui::HAlign::Left, Vec3{0.7f, 0.72f, 0.78f});
    text(rect(child("Etiqueta nombre", menu), top_left, top_left, Vec2{30, 118}, Vec2{250, 30}), "Tu nombre", 22.0f, ui::HAlign::Left);
    field(rect(child("UI_Nombre", menu), top_left, top_left, Vec2{30, 150}, Vec2{560, 46}), "Jugador", "Tu nombre", 16);
    text(rect(child("Etiqueta IP", menu), top_left, top_left, Vec2{30, 206}, Vec2{560, 30}), "IP del servidor (para unirse)", 22.0f,
         ui::HAlign::Left);
    field(rect(child("UI_IP", menu), top_left, top_left, Vec2{30, 238}, Vec2{560, 46}), "127.0.0.1", "127.0.0.1", 64);
    button(rect(child("Crear", menu), top_left, top_left, Vec2{30, 304}, Vec2{270, 58}), "Crear partida", "OnCrear",
           Vec3{0.10f, 0.45f, 0.85f});
    button(rect(child("Unirse", menu), top_left, top_left, Vec2{320, 304}, Vec2{270, 58}), "Unirse", "OnUnirse",
           Vec3{0.15f, 0.55f, 0.30f});
    text(rect(child("UI_Estado", menu), top_left, top_left, Vec2{30, 376}, Vec2{560, 80}), "", 19.0f, ui::HAlign::Left,
         Vec3{1.0f, 0.88f, 0.55f}, ui::VAlign::Top, true);

    // En partida: chat, marcador, avisos, informacion y salir.
    ecs::Entity hud = stretch(child("UI_Juego", canvas));
    ecs::Entity chat = image(rect(child("Chat", hud), bottom_left, bottom_left, Vec2{24, -84}, Vec2{620, 250}), panel, 0.55f, 10.0f);
    text(stretch(child("UI_Chat", chat), 12.0f), "", 19.0f, ui::HAlign::Left, Vec3{0.93f, 0.94f, 0.97f}, ui::VAlign::Bottom, true);
    field(rect(child("UI_EscribirChat", hud), bottom_left, bottom_left, Vec2{24, -24}, Vec2{620, 50}), "",
          "Clic aqui, escribe y pulsa Enter...", 120, "OnChat");
    image(rect(child("Marcador fondo", hud), top_right, top_right, Vec2{-24, 24}, Vec2{520, 290}), panel, 0.55f, 10.0f);
    text(rect(child("UI_Marcador", hud), top_right, top_right, Vec2{-36, 34}, Vec2{496, 270}), "", 20.0f, ui::HAlign::Left,
         Vec3{1, 1, 1}, ui::VAlign::Top, true);
    ecs::Entity notice = text(rect(child("UI_Aviso", hud), top, top, Vec2{0, 150}, Vec2{1300, 70}), "", 46.0f, ui::HAlign::Center,
                              Vec3{1.0f, 0.85f, 0.3f});
    notice.get<ui::Text>().alpha = 0.0f;
    text(rect(child("UI_Info", hud), top_left, top_left, Vec2{150, 24}, Vec2{1100, 64}), "", 18.0f, ui::HAlign::Left,
         Vec3{0.85f, 0.88f, 0.92f}, ui::VAlign::Top, true);
    button(rect(child("Salir", hud), top_left, top_left, Vec2{24, 24}, Vec2{110, 46}), "Salir", "OnSalir", Vec3{0.55f, 0.16f, 0.16f});

    b.save("Main");
}

// Mundo abierto: una isla de 8 x 8 km con relieve, playas, rocas y nieve,
// rodeada de oceano, con unos 2 millones de arboles instanciados (componente
// Vegetacion) y un jugador en tercera persona. Prueba de rendimiento: el HUD
// dice los FPS, los ms de CPU y GPU y cuantos arboles se dibujan.
void buildOpenWorld(project::ProjectInfo& project) {
    Builder b(project);
    b.world.setSceneUuid(Uuid::generate());
    ecs::populateDefaultScene(b.world);
    b.script("Jugador.lua", kPlayerScript);
    b.script("CamaraTercera.lua", kCameraScript);
    b.script("Rendimiento.lua", openworld::kPerformanceScript);

    // --- Terreno: relieve fBm con una mascara de isla ---
    constexpr float kSize = 8192.0f;
    constexpr float kHeight = 450.0f;
    constexpr float kSea = 0.15f;  // el mar (y = 0) a este 0..1 del terreno
    terrain::Terrain comp;
    comp.data = "Terrains/Isla.crterrain";
    comp.size = kSize;
    comp.height = kHeight;
    comp.resolution = 1025;
    comp.splat_resolution = 1024;
    comp.lod_distance = 2.0f;
    terrain::TerrainData data;
    data.create(1025, 1024, 0.0f);
    terrain::generateRelief(data, 2027, 3.2f, 0.52f, 0.45f, 0.0f);
    {
        const auto res = static_cast<int>(data.resolution());
        std::vector<float>& h = data.heights();
        for (int y = 0; y < res; ++y) {
            for (int x = 0; x < res; ++x) {
                const float u = static_cast<float>(x) / static_cast<float>(res - 1) * 2.0f - 1.0f;
                const float v = static_cast<float>(y) / static_cast<float>(res - 1) * 2.0f - 1.0f;
                // Costa algo irregular: la distancia al centro deformada.
                const float angle = std::atan2(v, u);
                const float d = std::sqrt(u * u + v * v) * (1.0f + 0.08f * std::sin(angle * 5.0f) + 0.05f * std::cos(angle * 3.0f));
                const float t = std::clamp((0.92f - d) / 0.42f, 0.0f, 1.0f);
                const float mask = t * t * (3.0f - 2.0f * t);
                float& value = h[static_cast<std::size_t>(y) * static_cast<std::size_t>(res) + static_cast<std::size_t>(x)];
                const float land = kSea + 0.02f + value * 0.78f;
                value = 0.02f + (land - 0.02f) * mask;
            }
        }
    }
    // Capas: hierba (0) de base, arena (3) en la costa, roca (2) en los taludes y
    // tierra (1) en lo mas alto.
    terrain::paintByRules(data, comp, 3, 0.0f, 90.0f, 0.0f, kSea + 5.0f / kHeight);
    terrain::paintByRules(data, comp, 2, 34.0f, 90.0f, 0.0f, 1.0f);
    terrain::paintByRules(data, comp, 1, 0.0f, 90.0f, 0.72f, 1.0f);
    {
        const std::filesystem::path path = project.assetsFolder() / "Terrains" / "Isla.crterrain";
        std::filesystem::create_directories(path.parent_path());
        if (!data.save(path)) throw std::runtime_error("no se pudo guardar el terreno");
    }
    const Vec3 origin{-kSize * 0.5f, -kSea * kHeight, -kSize * 0.5f};
    ecs::Entity island = b.world.create("Isla");
    island.setWorldPosition(origin);
    island.add<terrain::Terrain>() = comp;

    // Niebla de mundo abierto: fina y alta (se ve la isla entera desde lo alto).
    if (ecs::Entity env = b.world.findByName("Entorno"); env.valid() && env.has<ecs::PostProcessing>()) {
        env.get<ecs::PostProcessing>().settings.fog_density = 0.00022f;
        env.get<ecs::PostProcessing>().settings.fog_height_falloff = 0.0035f;
    }

    // --- Oceano ---
    ecs::Entity ocean = b.world.create("Oceano");
    ocean.setWorldPosition(Vec3{0.0f, 0.0f, 0.0f});
    ocean.add<water::WaterBody>() = water::oceanPreset();

    // --- Bosque: ~2 millones de arboles en la isla ---
    ecs::Entity forest = b.world.create("Vegetacion");
    forest.setWorldPosition(Vec3{0.0f, 0.0f, 0.0f});
    foliage::Foliage& f = forest.add<foliage::Foliage>();
    f.area = kSize;
    f.density = 700.0f;
    f.seed = 7;
    f.pine = 0.55f;
    f.oak = 0.3f;
    f.birch = 0.15f;
    f.min_height = 4.0f;                          // sobre la playa
    f.max_height = 0.7f * kHeight - kSea * kHeight;  // sin arboles en las cumbres
    f.max_slope = 30.0f;
    f.max_instances = 6000000;
    f.lod1_distance = 110.0f;
    f.lod2_distance = 520.0f;
    f.max_distance = 3000.0f;
    f.shadow_distance = 130.0f;

    // --- Jugador en un claro cerca de la costa sur ---
    const auto ground_at = [&](float x, float z) { return terrain::heightAt(data, comp, origin, x, z); };
    Vec3 spawn{0.0f, 40.0f, 0.0f};
    for (float z = kSize * 0.35f; z > -kSize * 0.35f; z -= 16.0f) {
        const float y = ground_at(0.0f, z);
        const Vec3 n = terrain::normalAt(data, comp, origin, 0.0f, z);
        if (y > 12.0f && y < 60.0f && n.y > 0.95f) {
            spawn = Vec3{0.0f, y + 2.0f, z};
            break;
        }
    }
    // Un claro donde empieza el jugador (sin arboles encima).
    forest.get<foliage::Foliage>().clearings.push_back({spawn, 30.0f});
    const assets::AssetRef body = b.material("Jugador", Vec3{0.95f, 0.45f, 0.10f}, 0.4f);
    const assets::AssetRef visor = b.material("Visor", Vec3{0.05f, 0.06f, 0.08f}, 0.15f, 0.6f);
    ecs::Entity player = b.player(spawn, body, visor);
    // Mas rapido: la isla es grande.
    player.get<scripting::Script>().properties = {{"velocidad", scripting::PropertyType::Number, "9"},
                                                  {"correr", scripting::PropertyType::Number, "2.6"}};
    ecs::Entity cam = b.world.findByName("Main Camera");
    cam.setWorldPosition(spawn + Vec3{0.0f, 4.0f, 9.0f});
    Builder::attach(cam, "CamaraTercera.lua",
                    {{"distancia", scripting::PropertyType::Number, "8"}, {"inclinacion", scripting::PropertyType::Number, "14"}});

    // --- HUD de rendimiento ---
    ecs::Entity canvas = b.world.create("HUD");
    canvas.add<ui::Canvas>();
    ecs::Entity panel = b.world.create("UI_Panel", canvas);
    {
        ui::RectTransform& rt = panel.add<ui::RectTransform>();
        rt.anchor_min = rt.anchor_max = Vec2{0.0f, 0.0f};
        rt.pivot = Vec2{0.0f, 0.0f};
        rt.position = Vec2{20.0f, 20.0f};
        rt.size = Vec2{980.0f, 196.0f};
        ui::Image& im = panel.add<ui::Image>();
        im.color = Vec3{0.05f, 0.06f, 0.08f};
        im.alpha = 0.62f;
        im.corner_radius = 10.0f;
    }
    ecs::Entity text = b.world.create("UI_Rendimiento", panel);
    {
        ui::RectTransform& rt = text.add<ui::RectTransform>();
        rt.anchor_min = Vec2{0.0f, 0.0f};
        rt.anchor_max = Vec2{1.0f, 1.0f};
        rt.size = Vec2{-28.0f, -24.0f};
        ui::Text& t = text.add<ui::Text>();
        t.text = "Midiendo...";
        t.font_size = 22.0f;
        t.h_align = ui::HAlign::Left;
        t.v_align = ui::VAlign::Top;
        t.shadow = true;
        t.wrap = true;
    }
    ecs::Entity perf = b.world.create("Rendimiento");
    Builder::attach(perf, "Rendimiento.lua");

    b.save("Main");
}

// Tercera persona avanzada: el personaje y las animaciones del Locomotion
// Pack de Mixamo (el del usuario: el motor no puede repartirlo) con un
// Animator Controller (Blend Tree 2D, salto, giros en el sitio), IK de pies,
// mirada y manos, y un circuito para lucirlo. Todo el juego en Lua.
void buildThirdPersonPro(project::ProjectInfo& project, std::filesystem::path pack) {
    if (pack.empty()) pack = locomotion::findDownloadedPack();
    if (pack.empty()) {
        throw std::runtime_error(
            "Falta el Locomotion Pack de Mixamo: en mixamo.com elige el pack \"Locomotion Pack\" (FBX, con skin) y "
            "dejalo en Descargas (el .zip o la carpeta descomprimida), o indica donde esta.");
    }
    const locomotion::PackResult pr = locomotion::importPack(pack, project.assetsFolder());
    for (const std::string& line : pr.log) std::cout << "[Plantilla] " << line << "\n";
    if (!pr.ok) throw std::runtime_error(pr.error);

    // --- El Animator Controller ---
    ecs::AnimatorController c;
    c.uuid = Uuid::generate();
    const auto param = [&](const char* name, ecs::AnimatorParameterType type, float value = 0.0f) {
        c.parameters.push_back({name, type, value});
    };
    param("X", ecs::AnimatorParameterType::Float);
    param("Y", ecs::AnimatorParameterType::Float);
    param("Moviendo", ecs::AnimatorParameterType::Bool);
    param("EnSuelo", ecs::AnimatorParameterType::Bool, 1.0f);
    param("Saltar", ecs::AnimatorParameterType::Trigger);
    for (const char* t : {"GirarIzq90", "GirarDer90", "GirarIzq180", "GirarDer180"}) {
        param(t, ecs::AnimatorParameterType::Trigger);
    }
    const auto clip_ref = [&](const std::string& key) {
        const locomotion::ClipInfo* info = pr.clip(key);
        return assets::AssetRef{info != nullptr ? info->uuid : Uuid{}, assets::AssetType::AnimationClip};
    };
    const auto speed_of = [&](const std::string& key, float fallback) {
        const locomotion::ClipInfo* info = pr.clip(key);
        return info != nullptr && info->speed > 0.1f ? info->speed : fallback;
    };
    const float walk = speed_of("walk", 1.6f);
    const float run = speed_of("run", 4.3f);
    const float side_walk = speed_of("strafe_walk_left", 1.7f);
    const float side_run = speed_of("strafe_run_left", 4.4f);

    ecs::AnimatorState moving;
    moving.name = "Locomocion";
    moving.motion = ecs::AnimatorMotion::BlendTree2D;
    moving.blend_parameter = "X";
    moving.blend_parameter_y = "Y";
    moving.position = core::Vec2{0.0f, 0.0f};
    const auto child = [&](const std::string& key, float x, float y, float speed = 1.0f) {
        if (pr.clip(key) == nullptr) return;
        ecs::BlendTreeChild ch;
        ch.clip = clip_ref(key);
        ch.position = core::Vec2{x, y};
        ch.speed = speed;
        moving.children.push_back(ch);
    };
    child("idle", 0.0f, 0.0f);
    child("walk", 0.0f, walk);
    child("run", 0.0f, run);
    child("walk", 0.0f, -walk, -1.0f);  // de espaldas: andar al reves
    child("strafe_walk_left", -side_walk, 0.0f);
    child("strafe_walk_right", side_walk, 0.0f);
    child("strafe_run_left", -side_run, 0.0f);
    child("strafe_run_right", side_run, 0.0f);
    c.states.push_back(moving);

    const auto clip_state = [&](const char* name, const std::string& key, core::Vec2 at) {
        ecs::AnimatorState s;
        s.name = name;
        s.clip = clip_ref(key);
        s.loop = false;
        s.position = at;
        c.states.push_back(s);
        return static_cast<int>(c.states.size()) - 1;
    };
    const int jump = clip_state("Saltar", "jump", core::Vec2{280.0f, -160.0f});
    const locomotion::ClipInfo* jump_info = pr.clip("jump");
    const float jump_duration = jump_info != nullptr ? std::max(jump_info->duration, 0.1f) : 2.0f;
    const float takeoff = jump_info != nullptr && jump_info->landing > jump_info->takeoff ? jump_info->takeoff : 0.8f;
    const float landing = jump_info != nullptr && jump_info->landing > jump_info->takeoff ? jump_info->landing : 1.25f;
    const auto transition = [&](int from, int to, float duration, std::vector<ecs::AnimatorCondition> conditions,
                                float exit_time = -1.0f) {
        ecs::AnimatorTransition t;
        t.from = from;
        t.to = to;
        t.duration = duration;
        t.conditions = std::move(conditions);
        if (exit_time >= 0.0f) {
            t.has_exit_time = true;
            t.exit_time = exit_time;
        }
        c.transitions.push_back(t);
    };
    using Mode = ecs::AnimatorConditionMode;
    transition(ecs::kAnyState, jump, 0.12f, {{"Saltar", Mode::If, 0.0f}});
    transition(jump, 0, 0.25f, {}, 0.97f);
    // Aterrizando y moviendose: vuelve antes a andar o correr.
    transition(jump, 0, 0.2f, {{"Moviendo", Mode::If, 0.0f}, {"EnSuelo", Mode::If, 0.0f}},
               std::min(0.95f, (landing + 0.15f) / jump_duration));
    struct Turn {
        const char* state;
        const char* key;
        const char* trigger;
        core::Vec2 at;
    };
    const Turn turns[] = {{"Girar 90 izq", "turn90_left", "GirarIzq90", {280.0f, 60.0f}},
                          {"Girar 90 der", "turn90_right", "GirarDer90", {280.0f, 140.0f}},
                          {"Girar 180 izq", "turn_left", "GirarIzq180", {280.0f, 220.0f}},
                          {"Girar 180 der", "turn_right", "GirarDer180", {280.0f, 300.0f}}};
    for (const Turn& t : turns) {
        if (pr.clip(t.key) == nullptr) continue;
        const int s = clip_state(t.state, t.key, t.at);
        transition(0, s, 0.15f, {{t.trigger, Mode::If, 0.0f}});
        transition(s, 0, 0.2f, {}, 0.9f);
        transition(s, 0, 0.2f, {{"Moviendo", Mode::If, 0.0f}});  // echa a andar: se corta
    }
    const std::filesystem::path controller_path = project.assetsFolder() / "Animations" / "Personaje.cranimator";
    std::string error;
    if (!ecs::saveAnimatorController(c, controller_path, &error)) throw std::runtime_error("controlador: " + error);

    // --- Escena ---
    Builder b(project);
    b.world.setSceneUuid(Uuid::generate());
    ecs::populateDefaultScene(b.world);
    b.script("Personaje.lua", locomotion_scripts::kCharacter);
    b.script("CamaraOrbital.lua", locomotion_scripts::kOrbitCamera);
    b.script("Palanca.lua", locomotion_scripts::kLever);
    b.script("Cristal.lua", locomotion_scripts::kCrystal);
    b.script("Interfaz.lua", locomotion_scripts::kInterface);

    const assets::AssetRef floor = b.material("Suelo", Vec3{0.52f, 0.54f, 0.56f}, 0.9f);
    const assets::AssetRef stone = b.material("Piedra", Vec3{0.46f, 0.45f, 0.43f}, 0.85f);
    const assets::AssetRef wood = b.material("Madera", Vec3{0.60f, 0.42f, 0.25f}, 0.75f);
    const assets::AssetRef wall = b.material("Muro", Vec3{0.36f, 0.38f, 0.42f}, 0.8f);
    const assets::AssetRef orange = b.material("Naranja", Vec3{0.95f, 0.52f, 0.18f}, 0.55f);
    const assets::AssetRef blue = b.material("Azul", Vec3{0.16f, 0.50f, 0.92f}, 0.45f);
    const assets::AssetRef metal = b.material("Metal", Vec3{0.62f, 0.63f, 0.66f}, 0.35f, 1.0f);
    const assets::AssetRef red = b.material("Pomo", Vec3{0.85f, 0.12f, 0.10f}, 0.4f);
    const assets::AssetRef crystal = b.material("Cristal", Vec3{0.3f, 0.9f, 1.0f}, 0.1f, 0.0f, Vec3{0.2f, 0.8f, 1.0f}, 2.5f);

    b.box("Suelo", Vec3{0.0f, -0.5f, 0.0f}, Vec3{70.0f, 1.0f, 70.0f}, floor);
    b.ring(35.0f, 2.5f, 1.0f, wall);

    // Escalera de 8 escalones (18 cm), rellano y bajada: los pies se apoyan en
    // cada escalon (IK) y el personaje sube sin saltar.
    const float rise = 0.18f;
    const float tread = 0.4f;
    const float top = rise * 8.0f;
    for (int i = 0; i < 8; ++i) {
        const float h = rise * static_cast<float>(i + 1);
        b.box("Escalon " + std::to_string(i + 1), Vec3{-8.0f, h * 0.5f, 4.0f - tread * static_cast<float>(i) - tread * 0.5f},
              Vec3{3.0f, h, tread}, i % 2 == 0 ? wood : orange);
    }
    b.box("Rellano", Vec3{-8.0f, top * 0.5f, -1.2f}, Vec3{3.0f, top, 4.0f}, wood);
    for (int i = 0; i < 7; ++i) {
        const float h = top - rise * static_cast<float>(i + 1);
        b.box("Bajada " + std::to_string(i + 1), Vec3{-8.0f, h * 0.5f, -3.2f - tread * static_cast<float>(i) - tread * 0.5f},
              Vec3{3.0f, h, tread}, i % 2 == 0 ? orange : wood);
    }
    // Rampa (15 grados) hasta una plataforma.
    const float slope = 15.0f;
    const float ramp_len = 6.0f;
    const float ramp_top = ramp_len * std::sin(slope * 3.14159265f / 180.0f);
    b.box("Rampa", Vec3{8.0f, ramp_top * 0.5f - 0.1f, 4.0f - ramp_len * 0.5f * std::cos(slope * 3.14159265f / 180.0f)},
          Vec3{3.0f, 0.3f, ramp_len}, stone, Vec3{slope, 0.0f, 0.0f});
    b.box("Plataforma", Vec3{8.0f, ramp_top * 0.5f, -3.8f}, Vec3{3.0f, ramp_top, 4.0f}, stone);
    // Piedras de alturas distintas: cada pie pisa a una altura.
    std::mt19937 rng(7);
    const auto range = [&](float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng); };
    for (int i = 0; i < 14; ++i) {
        const float h = range(0.08f, 0.32f);
        const Vec3 p{range(-3.0f, 3.0f), h * 0.5f, range(-12.0f, -6.0f)};
        b.box("Roca " + std::to_string(i + 1), p, Vec3{range(0.6f, 1.2f), h, range(0.6f, 1.2f)}, stone,
              Vec3{range(-4.0f, 4.0f), range(0.0f, 90.0f), range(-4.0f, 4.0f)});
    }
    // Bloques para saltar (hay que saltar: mas altos que un escalon).
    const float blocks[] = {0.7f, 1.3f, 1.9f};
    for (int i = 0; i < 3; ++i) {
        const float h = blocks[i];
        b.box("Bloque " + std::to_string(i + 1), Vec3{17.0f, h * 0.5f, 4.0f - 3.2f * static_cast<float>(i)},
              Vec3{2.2f, h, 2.2f}, i % 2 == 0 ? blue : orange);
    }
    // Cajas que se empujan.
    for (int i = 0; i < 4; ++i) {
        ecs::Entity crate = b.box("Caja " + std::to_string(i + 1),
                                  Vec3{3.0f + 1.0f * static_cast<float>(i % 2), 0.4f + 0.8f * static_cast<float>(i / 2), 7.0f},
                                  Vec3{0.8f, 0.8f, 0.8f}, wood);
        physics::Rigidbody& rb = crate.add<physics::Rigidbody>();
        rb.mass = 15.0f;
    }

    // Patio con compuerta (la abre la palanca 1) y camara con otra (palanca 2).
    b.box("Patio muro norte", Vec3{0.0f, 1.5f, -27.0f}, Vec3{12.0f, 3.0f, 0.6f}, wall);
    b.box("Patio muro oeste", Vec3{-6.0f, 1.5f, -22.5f}, Vec3{0.6f, 3.0f, 9.0f}, wall);
    b.box("Patio muro este", Vec3{6.0f, 1.5f, -22.5f}, Vec3{0.6f, 3.0f, 9.0f}, wall);
    b.box("Patio muro sur izq", Vec3{-4.0f, 1.5f, -18.0f}, Vec3{4.0f, 3.0f, 0.6f}, wall);
    b.box("Patio muro sur der", Vec3{4.0f, 1.5f, -18.0f}, Vec3{4.0f, 3.0f, 0.6f}, wall);
    b.box("Camara muro oeste", Vec3{-22.0f, 1.5f, -8.0f}, Vec3{0.6f, 3.0f, 6.0f}, wall);
    b.box("Camara muro norte", Vec3{-19.0f, 1.5f, -11.0f}, Vec3{6.6f, 3.0f, 0.6f}, wall);
    b.box("Camara muro sur", Vec3{-19.0f, 1.5f, -5.0f}, Vec3{6.6f, 3.0f, 0.6f}, wall);
    b.box("Camara techo", Vec3{-19.0f, 3.15f, -8.0f}, Vec3{6.6f, 0.3f, 6.6f}, wall);
    const auto gate = [&](const std::string& name, const Vec3& p, const Vec3& size) {
        ecs::Entity g = b.box(name, p, size, metal);
        g.add<physics::Rigidbody>().type = physics::BodyType::Kinematic;
    };
    gate("Compuerta 1", Vec3{0.0f, 1.5f, -18.0f}, Vec3{4.0f, 3.0f, 0.4f});
    gate("Compuerta 2", Vec3{-16.0f, 1.5f, -8.0f}, Vec3{0.4f, 3.0f, 5.4f});

    const auto lever = [&](const std::string& name, const Vec3& p, float yaw, const std::string& target) {
        ecs::Entity root = b.world.create(name);
        root.setWorldPosition(p);
        root.setLocalEulerDegrees(Vec3{0.0f, yaw, 0.0f});
        root.setTag("Palanca");
        ecs::Entity base = ecs::createPrimitive(b.world, assets::builtin::kCube, "Base", root);
        base.setLocalPosition(Vec3{0.0f, 0.45f, 0.0f});
        base.setLocalScale(Vec3{0.4f, 0.9f, 0.3f});
        base.get<ecs::MeshRenderer>().materials = {metal};
        base.add<physics::BoxCollider>();
        ecs::Entity arm = b.world.create("Brazo", root);
        arm.setLocalPosition(Vec3{0.0f, 0.85f, -0.15f});
        arm.setLocalEulerDegrees(Vec3{10.0f, 0.0f, 0.0f});
        ecs::Entity rod = ecs::createPrimitive(b.world, assets::builtin::kCylinder, "Vara", arm);
        rod.setLocalPosition(Vec3{0.0f, 0.22f, 0.0f});
        rod.setLocalScale(Vec3{0.05f, 0.22f, 0.05f});
        rod.get<ecs::MeshRenderer>().materials = {metal};
        ecs::Entity knob = ecs::createPrimitive(b.world, assets::builtin::kSphere, "Pomo", arm);
        knob.setLocalPosition(Vec3{0.0f, 0.46f, 0.0f});
        knob.setLocalScale(Vec3{0.1f, 0.1f, 0.1f});
        knob.get<ecs::MeshRenderer>().materials = {red};
        Builder::attach(root, "Palanca.lua", {{"compuerta", scripting::PropertyType::Text, target}});
    };
    // (La palanca mira a -Z: yaw 180 = hacia +Z, por donde llega el jugador.)
    lever("Palanca 1", Vec3{3.8f, 0.0f, -16.8f}, 180.0f, "Compuerta 1");
    lever("Palanca 2", Vec3{-14.5f, 0.0f, -4.2f}, 270.0f, "Compuerta 2");

    // Estatua (el personaje la mira al pasar).
    ecs::Entity statue = ecs::createPrimitive(b.world, assets::builtin::kCylinder, "Pedestal");
    statue.setWorldPosition(Vec3{-3.0f, 0.5f, 2.0f});
    statue.setLocalScale(Vec3{0.7f, 0.5f, 0.7f});
    statue.get<ecs::MeshRenderer>().materials = {stone};
    statue.add<physics::CapsuleCollider>();
    ecs::Entity bust = ecs::createPrimitive(b.world, assets::builtin::kSphere, "Estatua");
    bust.setWorldPosition(Vec3{-3.0f, 1.45f, 2.0f});
    bust.setLocalScale(Vec3{0.45f, 0.45f, 0.45f});
    bust.get<ecs::MeshRenderer>().materials = {metal};
    bust.setTag("Interes");

    const Vec3 gems[] = {{-8.0f, top + 1.0f, -1.2f},   {8.0f, ramp_top + 1.0f, -3.8f}, {17.0f, 1.9f + 1.0f, -2.4f},
                         {0.0f, 1.0f, -9.0f},          {-2.0f, 1.0f, -23.0f},          {3.0f, 1.0f, -25.0f},
                         {-19.5f, 1.0f, -8.0f}};
    int g = 0;
    for (const Vec3& p : gems) {
        ecs::Entity e = ecs::createPrimitive(b.world, assets::builtin::kSphere, "Cristal " + std::to_string(++g));
        e.setWorldPosition(p);
        e.setLocalScale(Vec3{0.28f, 0.45f, 0.28f});
        e.get<ecs::MeshRenderer>().materials = {crystal};
        e.setTag("Cristal");
        Builder::attach(e, "Cristal.lua");
    }

    // --- El personaje: capsula con Rigidbody, un pivote que gira y el modelo ---
    ecs::Entity player = b.world.create("Jugador");
    player.setWorldPosition(Vec3{0.0f, 0.95f, 12.0f});
    physics::CapsuleCollider& capsule = player.add<physics::CapsuleCollider>();
    capsule.radius = 0.3f;
    capsule.height = 1.8f;
    capsule.material.friction = 0.0f;
    physics::Rigidbody& rb = player.add<physics::Rigidbody>();
    rb.mass = 70.0f;
    rb.lock_rotation_x = rb.lock_rotation_y = rb.lock_rotation_z = true;
    rb.continuous = true;
    const auto num = [](float v) { return std::to_string(v); };
    const auto duration_of = [&](const char* key, float fallback) {
        const locomotion::ClipInfo* info = pr.clip(key);
        return info != nullptr ? info->duration : fallback;
    };
    using scripting::PropertyType;
    Builder::attach(player, "Personaje.lua",
                    {{"velAndar", PropertyType::Number, num(walk)},
                     {"velCorrer", PropertyType::Number, num(run)},
                     {"velLateral", PropertyType::Number, num(side_walk)},
                     {"velLateralCorrer", PropertyType::Number, num(side_run)},
                     {"despegue", PropertyType::Number, num(takeoff)},
                     {"aterrizaje", PropertyType::Number, num(landing)},
                     {"duracionSalto", PropertyType::Number, num(jump_duration)},
                     {"durGiro90", PropertyType::Number, num(duration_of("turn90_left", 0.93f))},
                     {"durGiro180", PropertyType::Number, num(duration_of("turn_left", 1.63f))}});
    ecs::Entity pivot = b.world.create("Pivote", player);
    ecs::Entity model = b.world.create("Modelo", pivot);
    model.setLocalPosition(Vec3{0.0f, -0.9f, 0.0f});
    model.setLocalEulerDegrees(Vec3{0.0f, 180.0f, 0.0f});  // Mixamo mira a +Z; el motor, a -Z
    model.add<ecs::MeshRenderer>().model = assets::AssetRef{pr.character, assets::AssetType::Model};
    ecs::Animator& animator = model.add<ecs::Animator>();
    animator.controller = assets::AssetRef{c.uuid, assets::AssetType::AnimatorController};
    ecs::InverseKinematics& ik = model.add<ecs::InverseKinematics>();
    ik.foot_grounding = true;
    ik.foot_locking = true;  // los pies apoyados no patinan
    ik.max_step = 0.4f;
    ik.look_weight = 0.0f;
    ik.look_max_angle = 75.0f;
    ik.right_hand.weight = 0.0f;
    // Se inclina al arrancar, frenar y girar (la inercia del cuerpo).
    ecs::ProceduralAnimation& body = model.add<ecs::ProceduralAnimation>();
    body.lean = true;
    body.lean_amount = 7.0f;
    model.add<ecs::Skeleton>().show_bones = false;

    // Camara orbital y la interfaz.
    ecs::Entity cam = b.world.findByName("Main Camera");
    cam.setWorldPosition(Vec3{0.0f, 2.5f, 16.0f});
    Builder::attach(cam, "CamaraOrbital.lua");

    ecs::Entity canvas = b.world.create("HUD");
    canvas.add<ui::Canvas>();
    const auto rect = [&](const std::string& name, Vec2 anchor, Vec2 pivot_at, Vec2 position, Vec2 size) {
        ecs::Entity e = b.world.create(name, canvas);
        ui::RectTransform& rt = e.add<ui::RectTransform>();
        rt.anchor_min = anchor;
        rt.anchor_max = anchor;
        rt.pivot = pivot_at;
        rt.position = position;
        rt.size = size;
        return e;
    };
    const auto label = [&](ecs::Entity e, const std::string& value, float size, ui::HAlign align,
                           ui::VAlign valign = ui::VAlign::Middle) {
        ui::Text& t = e.add<ui::Text>();
        t.text = value;
        t.font_size = size;
        t.h_align = align;
        t.v_align = valign;
        t.shadow = true;
        return e;
    };
    label(rect("Marcador", Vec2{0.0f, 0.0f}, Vec2{0.0f, 0.0f}, Vec2{40.0f, 30.0f}, Vec2{900.0f, 50.0f}), "", 36.0f,
          ui::HAlign::Left);
    label(rect("Ayuda", Vec2{0.0f, 1.0f}, Vec2{0.0f, 1.0f}, Vec2{40.0f, -24.0f}, Vec2{1500.0f, 40.0f}),
          "WASD mover   Shift correr   Espacio saltar   Clic derecho apuntar   E palanca   Rueda distancia   "
          "F1 depuracion   Esc soltar el raton",
          22.0f, ui::HAlign::Left);
    ecs::Entity energy = rect("Energia", Vec2{0.5f, 1.0f}, Vec2{0.5f, 1.0f}, Vec2{0.0f, -80.0f}, Vec2{420.0f, 16.0f});
    ui::Slider& bar = energy.add<ui::Slider>();
    bar.interactable = false;
    bar.value = 1.0f;
    bar.fill = Vec3{0.35f, 0.85f, 0.35f};
    bar.handle = bar.fill;
    label(rect("Aviso", Vec2{0.5f, 1.0f}, Vec2{0.5f, 1.0f}, Vec2{0.0f, -140.0f}, Vec2{900.0f, 50.0f}), "", 30.0f,
          ui::HAlign::Center);
    label(rect("Depuracion", Vec2{0.0f, 0.0f}, Vec2{0.0f, 0.0f}, Vec2{40.0f, 100.0f}, Vec2{900.0f, 180.0f}), "", 24.0f,
          ui::HAlign::Left, ui::VAlign::Top);
    Builder::attach(canvas, "Interfaz.lua");
    b.save("Main");

    std::vector<std::string> tags = ecs::defaultTags();
    for (const char* t : {"Palanca", "Cristal", "Interes"}) tags.push_back(t);
    ecs::saveTags(project.settingsFolder() / "Tags.json", tags);
}

void copyFolder(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code error;
    if (!std::filesystem::is_directory(from, error)) return;
    std::filesystem::create_directories(to, error);
    std::filesystem::copy(from, to,
                          std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing,
                          error);
    if (error) throw std::runtime_error("no se pudo copiar " + from.string() + ": " + error.message());
}

std::string safeFolderName(const std::string& name) {
    std::string out;
    for (const char c : name) {
        out += (std::string("<>:\"/\\|?*").find(c) != std::string::npos || static_cast<unsigned char>(c) < 32) ? '_' : c;
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    return out.empty() ? "Plantilla" : out;
}

std::filesystem::path fromUtf8(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }

}  // namespace

// ================================================================================

std::filesystem::path userTemplatesFolder() {
    std::filesystem::path base;
    char* local = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&local, &length, "LOCALAPPDATA") == 0 && local != nullptr && *local != '\0') {
        base = local;
        std::free(local);
    } else {
        std::free(local);
        std::error_code error;
        base = std::filesystem::temp_directory_path(error);
    }
    return base / "Cramion" / "Templates";
}

std::vector<ProjectTemplate> availableTemplates() {
    std::vector<ProjectTemplate> list;
    list.push_back(ProjectTemplate{
        "blank", "Vacío", "Integradas",
        "Un proyecto limpio: cielo, sol, cámara y un suelo. Para empezar desde cero.",
        {"Cielo físico y sol", "Suelo con collider", "Post-proceso global"}, rgba(0, 143, 242), TemplateArt::Blank, {}});
    list.push_back(ProjectTemplate{
        "third_person", "Tercera persona", "Integradas",
        "Un personaje que corre y salta con cámara orbital, plataformas, rampas y monedas que recoger.",
        {"Jugador con Rigidbody (WASD, Shift, Espacio)", "Cámara orbital (clic derecho y rueda)",
         "Monedas con trigger y marcador en el HUD", "Escalera, rampa y plataformas"},
        rgba(242, 140, 40), TemplateArt::ThirdPerson, {}});
    list.push_back(ProjectTemplate{
        "third_person_pro", "Tercera persona avanzada", "Integradas",
        "Un personaje de Mixamo con todas las animaciones del Locomotion Pack: anda, corre, se mueve de lado al "
        "apuntar, gira en el sitio y salta sincronizado con la fisica. Pies en el suelo, mirada y manos con IK. "
        "Todo el juego en Lua. Usa tu Locomotion Pack (mixamo.com) desde Descargas.",
        {"Blend Tree 2D con las velocidades medidas de cada animacion (los pies no patinan)",
         "Escalones, rampas y rocas: sube sin saltar y cada pie se apoya con IK",
         "Apuntar (clic derecho): desplazamiento lateral, de espaldas y giros de 90 y 180 grados en el sitio",
         "Salto con el despegue y el aterrizaje de la animacion; palancas con la mano (IK) y camara con muelle"},
        rgba(250, 110, 60), TemplateArt::ThirdPersonPro, locomotion::findDownloadedPack()});
    list.push_back(ProjectTemplate{
        "navigation", "IA y navegación", "Integradas",
        "Escapa de los guardias: patrullan un laberinto con NavMesh y te persiguen si te ven. Llega a la meta.",
        {"NavMesh generado en tiempo real", "Guardias con NavAgent (patrullar / perseguir)",
         "Visión con Navigation.raycast", "Meta, HUD y reinicio del nivel"},
        rgba(60, 200, 110), TemplateArt::Navigation, {}});
    list.push_back(ProjectTemplate{
        "state_machines", "IA con máquinas de estados", "Integradas",
        "Enemigos con una máquina de estados como las de Bolt: patrullan una ruta, te persiguen si te ven, te atacan, "
        "huyen con poca vida y vuelven a casa. Cada estado tiene su código Lua; ábrela en la ventana Máquina de estados "
        "y mira en Play cómo cambia de estado.",
        {"Máquina Enemigo.crfsm: Patrullar, Perseguir, Atacar, Huir y Volver", "Variables (pizarra) con valores por enemigo",
         "Transiciones con variables, temporizador y expresión Lua", "F para golpear y HUD con el estado de cada enemigo"},
        rgba(240, 120, 60), TemplateArt::Navigation, {}});
    list.push_back(ProjectTemplate{
        "voxel", "Mundo de bloques", "Integradas",
        "Supervivencia en un mundo infinito de bloques como Minecraft: rompe, recoge, craftea herramientas y construye. "
        "Vida, hambre y aire. Se guarda solo.",
        {"Mundo infinito con biomas, cuevas, arboles y mar", "Inventario de 36 huecos con iconos y 13 recetas de crafteo",
         "Vida, hambre, aire, dano por caida, muerte y reaparicion", "Contorno del bloque, grietas al romper y objetos que se recogen"},
        rgba(110, 190, 70), TemplateArt::Voxel, {}});
    list.push_back(ProjectTemplate{
        "mmo", "MMO RPG", "Integradas",
        "Un mundo de rol online en miniatura: pueblo con misiones y tienda, bosque de lobos, campamento goblin y un jefe. "
        "Otros jugadores cazan a tu lado. Todo el juego esta en Lua (Heroe, Enemigo, Bot, NPC).",
        {"Combate con objetivo (Tab), 4 habilidades con mana, enfriamientos y pociones",
         "5 misiones, dialogos, tienda, inventario de 20 huecos y equipo",
         "Enemigos con IA (aggro, leash, reaparicion), botin y un jefe con ataque en area",
         "Interfaz completa: marcos, barra de habilidades, chat, minimapa y niveles"},
        rgba(210, 160, 60), TemplateArt::Mmo, {}});
    list.push_back(ProjectTemplate{
        "creatures", "Criaturas: IK, ragdoll y phys bones", "Integradas",
        "Un perro que recorre escaleras, una rampa y piedras apoyando cada pata con IK, con la cola y las orejas "
        "fisicas (Phys Bones) y un sombrero enganchado a la cabeza; y un maniqui que cae como un ragdoll. Todo desde Lua.",
        {"IK de animales: 4 patas de 3 huesos al suelo, el cuerpo se inclina y la cabeza sigue a la pelota",
         "Phys Bones en la cola, las orejas y la coleta, con colliders en el cuerpo y la cabeza",
         "Ragdoll con Jolt (R y F) que hereda la velocidad y vuelve a la animacion",
         "Esqueleto visible (B), Bone Sockets y los dos modelos con esqueleto generados"},
        rgba(230, 120, 90), TemplateArt::Creatures, {}});
    list.push_back(ProjectTemplate{
        "online", "Online (multijugador)", "Integradas",
        "Una arena multijugador por red: crea una partida o unete con la IP de un amigo. Jugadores sincronizados, chat, "
        "marcador, monedas, un balon con porterias, cajas que se patean y eventos. Todo el juego en Lua (Red.lua).",
        {"Crear partida o unirse por IP (servidor que tambien juega)", "Jugadores sincronizados con Network.spawn y isMine",
         "Chat con nombres, avisos y marcador con puntos y ping", "Balon, cajas y monedas del servidor; goles y eventos"},
        rgba(80, 170, 255), TemplateArt::Online, {}});
    list.push_back(ProjectTemplate{
        "open_world", "Mundo abierto (rendimiento)", "Integradas",
        "Una isla de 8 x 8 km con relieve, playas y montanas, rodeada de oceano y con unos 2 millones de arboles "
        "instanciados. Recorrela en tercera persona y mide el rendimiento: FPS, ms de GPU y arboles dibujados.",
        {"Terreno de 8 km (1025 x 1025) con capas por altura y pendiente", "Oceano con oleaje y playas",
         "~2 millones de arboles (Vegetacion): recorte y niveles de detalle en la GPU",
         "HUD de rendimiento y teclas para forzar el motor (calidad, densidad, distancia, sombras)"},
        rgba(70, 180, 120), TemplateArt::OpenWorld, {}});

    // Del usuario.
    std::error_code error;
    const std::filesystem::path root = userTemplatesFolder();
    for (std::filesystem::directory_iterator it(root, error); !error && it != std::filesystem::directory_iterator();
         it.increment(error)) {
        const std::filesystem::path json_file = it->path() / "template.json";
        std::ifstream in(json_file, std::ios::binary);
        if (!in) continue;
        const nlohmann::json json = nlohmann::json::parse(in, nullptr, false);
        if (json.is_discarded() || !json.is_object()) continue;
        ProjectTemplate t;
        const std::u8string folder_name = it->path().filename().u8string();
        t.id = "user:" + std::string(folder_name.begin(), folder_name.end());
        t.name = json.value("name", std::string(folder_name.begin(), folder_name.end()));
        t.description = json.value("description", std::string("Plantilla guardada desde un proyecto."));
        t.category = "Mis plantillas";
        if (json.contains("features") && json["features"].is_array()) {
            for (const auto& f : json["features"]) {
                if (f.is_string()) t.features.push_back(f.get<std::string>());
            }
        }
        t.accent = json.value("accent", rgba(170, 110, 240));
        t.art = TemplateArt::User;
        t.folder = it->path();
        list.push_back(std::move(t));
    }
    return list;
}

project::ProjectInfo createProjectFromTemplate(const ProjectTemplate& t, const std::filesystem::path& parent,
                                               const std::string& name) {
    std::filesystem::create_directories(parent);
    project::ProjectInfo info = project::createProject(parent, name);
    try {
        if (t.art == TemplateArt::User) {
            copyFolder(t.folder / "Assets", info.assetsFolder());
            copyFolder(t.folder / "ProjectSettings", info.settingsFolder());
            std::ifstream in(t.folder / "template.json", std::ios::binary);
            const nlohmann::json json = nlohmann::json::parse(in, nullptr, false);
            if (!json.is_discarded() && json.contains("startup_scene") && json["startup_scene"].is_string()) {
                info.startup_scene = Uuid::parse(json["startup_scene"].get<std::string>());
            }
        } else if (t.id == "third_person") {
            buildThirdPerson(info);
        } else if (t.id == "third_person_pro") {
            buildThirdPersonPro(info, t.folder);
        } else if (t.id == "navigation") {
            buildNavigation(info);
        } else if (t.id == "state_machines") {
            buildStateMachines(info);
        } else if (t.id == "voxel") {
            buildVoxel(info);
        } else if (t.id == "mmo") {
            buildMmo(info);
        } else if (t.id == "open_world") {
            buildOpenWorld(info);
        } else if (t.id == "online") {
            buildOnline(info);
        } else if (t.id == "creatures") {
            buildCreatures(info);
        } else {
            buildBlank(info);
        }
        project::saveProject(info);
    } catch (...) {
        // Nada a medias: la carpeta la acaba de crear createProject.
        std::error_code error;
        std::filesystem::remove_all(info.folder, error);
        throw;
    }
    return info;
}

bool saveProjectAsTemplate(const project::ProjectInfo& project, const std::string& name,
                           const std::string& description, std::string* error) {
    try {
        const std::filesystem::path folder = userTemplatesFolder() / fromUtf8(safeFolderName(name));
        std::error_code ec;
        std::filesystem::remove_all(folder, ec);
        std::filesystem::create_directories(folder);
        copyFolder(project.assetsFolder(), folder / "Assets");
        copyFolder(project.settingsFolder(), folder / "ProjectSettings");
        nlohmann::json json;
        json["name"] = name;
        json["description"] = description.empty() ? "Plantilla creada desde \"" + project.name + "\"." : description;
        json["startup_scene"] = project.startup_scene.valid() ? project.startup_scene.toString() : std::string();
        json["accent"] = rgba(170, 110, 240);
        std::ofstream out(folder / "template.json", std::ios::binary | std::ios::trunc);
        out << json.dump(2);
        if (!out) throw std::runtime_error("no se pudo escribir template.json");
        return true;
    } catch (const std::exception& e) {
        if (error != nullptr) *error = e.what();
        return false;
    }
}

}  // namespace cramion::editor

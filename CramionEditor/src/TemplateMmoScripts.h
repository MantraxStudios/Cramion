#ifndef CRAMION_EDITOR_TEMPLATE_MMO_SCRIPTS_H
#define CRAMION_EDITOR_TEMPLATE_MMO_SCRIPTS_H

// Scripts de Lua de la plantilla "MMO RPG" (ProjectTemplates.cpp). Todo el
// juego va aqui, en Lua: el motor solo pone la escena y la interfaz.
//
//   Heroe.lua    el jugador y todos los sistemas: movimiento, estadisticas,
//                niveles, objetivo (Tab / clic), habilidades con mana,
//                lanzamiento y enfriamientos, inventario, equipo, tienda,
//                misiones, dialogos, botin, chat, minimapa, muerte y guardado
//   Enemigo.lua  IA de los enemigos (patrulla, aggro, leash, jefe con area),
//                muerte, botin y reaparicion
//   Bot.lua      otros "jugadores" del mundo: pasean, cazan y hablan por el chat
//   NPC.lua      personajes del pueblo: miran al jugador y su marca de mision

namespace cramion::editor::mmo {

constexpr const char* kHeroScript = R"lua(-- MMO RPG: el heroe y TODOS los sistemas del juego (en Lua).
--
-- Controles: WASD mover, Shift correr, Espacio saltar, clic derecho + raton
-- girar la camara, rueda acercar. Tab o clic izquierdo (mirando a un
-- enemigo) = objetivo. 1-6 habilidades y pociones. E hablar con un NPC.
-- I inventario, C personaje, L diario de misiones, H ayuda, Escape cerrar.
-- F9 borra la partida guardada.
local Heroe = {
    properties = {
        nombre = "Heroe",
        velocidad = 6.0,
        correr = 1.6,
        salto = 6.0,
    }
}

-- =========================================================================
-- Datos del juego
-- =========================================================================

local ITEMS = {
    pocion_vida   = { nombre = "Pocion de vida", tipo = "consumible", cura = 80, precio = 8, color = Vec3(0.9, 0.2, 0.2) },
    pocion_mana   = { nombre = "Pocion de mana", tipo = "consumible", mana = 60, precio = 8, color = Vec3(0.25, 0.45, 1.0) },
    piel_lobo     = { nombre = "Piel de lobo", tipo = "botin", precio = 4, color = Vec3(0.6, 0.45, 0.3) },
    colmillo      = { nombre = "Colmillo", tipo = "botin", precio = 3, color = Vec3(0.92, 0.9, 0.8) },
    oreja_goblin  = { nombre = "Oreja de goblin", tipo = "botin", precio = 5, color = Vec3(0.45, 0.75, 0.3) },
    espada_madera = { nombre = "Espada de practica", tipo = "arma", ataque = 3, precio = 4, color = Vec3(0.7, 0.55, 0.35) },
    espada_hierro = { nombre = "Espada de hierro", tipo = "arma", ataque = 8, nivel = 2, precio = 40, color = Vec3(0.75, 0.78, 0.82) },
    espada_acero  = { nombre = "Espada de acero", tipo = "arma", ataque = 14, nivel = 4, precio = 110, color = Vec3(0.45, 0.7, 1.0) },
    hacha_rey     = { nombre = "Hacha del Rey Goblin", tipo = "arma", ataque = 24, nivel = 5, precio = 300, color = Vec3(1.0, 0.55, 0.1) },
    tunica        = { nombre = "Tunica de aprendiz", tipo = "armadura", defensa = 2, precio = 4, color = Vec3(0.7, 0.65, 0.55) },
    armadura_cuero = { nombre = "Armadura de cuero", tipo = "armadura", defensa = 5, nivel = 2, precio = 45, color = Vec3(0.6, 0.4, 0.25) },
    cota_malla    = { nombre = "Cota de malla", tipo = "armadura", defensa = 10, nivel = 4, precio = 120, color = Vec3(0.45, 0.7, 1.0) },
}
local TIENDA = { "pocion_vida", "pocion_mana", "espada_hierro", "armadura_cuero", "espada_acero", "cota_malla" }
local PILA = 20        -- objetos iguales por ranura
local RANURAS = 20

local MISIONES = {
    { id = 1, nombre = "Lobos hambrientos", npc = "Capitana Elena", tipo = "matar", objetivo = "lobo", cantidad = 5, nivel = 1,
      texto = "Los lobos del Bosque Gris atacan a los viajeros del camino del oeste. Acaba con 5 lobos y vuelve a verme.",
      fin = "Buen trabajo. El camino vuelve a ser seguro. Toma esta espada, te hara falta.",
      xp = 140, oro = 20, premio = "espada_hierro" },
    { id = 2, nombre = "Pieles para el invierno", npc = "Mercader Tomas", tipo = "recoger", objetivo = "piel_lobo", cantidad = 4, nivel = 1,
      texto = "El invierno llega y no me quedan pieles. Traeme 4 pieles de lobo y te pagare bien.",
      fin = "¡Perfectas! Con esto abrigare a medio pueblo.", xp = 110, oro = 35, premio = "armadura_cuero" },
    { id = 3, nombre = "El campamento goblin", npc = "Capitana Elena", tipo = "matar", objetivo = "goblin", cantidad = 6, nivel = 3, requiere = 1,
      texto = "Los goblins han levantado un campamento al este. Elimina a 6 goblins antes de que ataquen el pueblo.",
      fin = "Eso les enseñara. Pero su jefe sigue ahi fuera...", xp = 300, oro = 45, premio = "pocion_vida", premioCantidad = 3 },
    { id = 4, nombre = "Pruebas del mal", npc = "Hermano Anselmo", tipo = "recoger", objetivo = "oreja_goblin", cantidad = 5, nivel = 3,
      texto = "Necesito 5 orejas de goblin para estudiar su magia oscura. No preguntes.",
      fin = "Interesante... muy interesante. Toma, para tu magia.", xp = 240, oro = 30, premio = "pocion_mana", premioCantidad = 4 },
    { id = 5, nombre = "El Rey Goblin", npc = "Hermano Anselmo", tipo = "matar", objetivo = "rey", cantidad = 1, nivel = 5, requiere = 3,
      texto = "El Rey Goblin se esconde en su guarida al norte. Es fuerte: lleva pociones y cuidado con su pisoton.",
      fin = "¡Lo has logrado! Villa Alba te lo agradecera siempre, heroe.", xp = 700, oro = 150, premio = "cota_malla" },
}

local HABILIDADES = {
    { nombre = "Golpe", mana = 0, cd = 0, alcance = 3.6, lanzar = 0, nivel = 1, objetivo = true },
    { nombre = "Bola de fuego", mana = 15, cd = 0, alcance = 26, lanzar = 1.4, nivel = 1, objetivo = true },
    { nombre = "Curar", mana = 22, cd = 8, alcance = 0, lanzar = 1.6, nivel = 2 },
    { nombre = "Torbellino", mana = 28, cd = 10, alcance = 5.5, lanzar = 0, nivel = 3 },
    { nombre = "Pocion de vida", item = "pocion_vida", cd = 12 },
    { nombre = "Pocion de mana", item = "pocion_mana", cd = 12 },
}
local GCD = 1.0            -- enfriamiento global entre habilidades
local NIVEL_MAX = 10
local AUTOATAQUE = 2.0     -- segundos entre golpes automaticos
local function xpNivel(n) return math.floor(100 * n ^ 1.5) end

-- Colores de los textos flotantes.
local C_DANO = Vec3(1.0, 0.9, 0.3)
local C_CRITICO = Vec3(1.0, 0.55, 0.1)
local C_CURA = Vec3(0.4, 1.0, 0.45)
local C_RECIBIDO = Vec3(1.0, 0.3, 0.25)
local C_MANA = Vec3(0.45, 0.65, 1.0)

local function dividir(texto, sep)
    local partes = {}
    if texto == nil or texto == "" then return partes end
    for trozo in string.gmatch(texto .. sep, "(.-)" .. sep:gsub("%p", "%%%0")) do partes[#partes + 1] = trozo end
    return partes
end

local function numeroDe(nombre)
    return tonumber(string.match(nombre or "", "(%d+)$")) or 0
end

-- =========================================================================
-- Inicio
-- =========================================================================

function Heroe:Start()
    self.camara = Scene.find("Main Camera")
    self.modelo = self.entity:find("Modelo")
    self.spawn = Scene.find("Punto de reaparicion")
    self.plantillaBotin = Scene.find("Plantilla Botin")
    self.enemigos = Scene.findAllWithTag("Enemigo")
    self.npcs = Scene.findAllWithTag("NPC")
    self.bots = Scene.findAllWithTag("Bot")
    self.zonas = {}
    for _, z in ipairs({ { "Zona Pueblo", "Villa Alba", 28 }, { "Zona Bosque", "Bosque Gris", 40 },
                         { "Zona Campamento", "Campamento Goblin", 38 }, { "Zona Guarida", "Guarida del Rey", 26 } }) do
        local e = Scene.find(z[1])
        if e then self.zonas[#self.zonas + 1] = { entidad = e, nombre = z[2], radio = z[3] } end
    end
    self:buscarInterfaz()

    self.nivel = 1
    self.xp = 0
    self.oro = 10
    self.inventario = {}
    self.equipo = { arma = "espada_madera", armadura = "tunica" }
    self.misiones = {}
    self.enfriamientos = {}
    self.gcd = 0
    self.lineas = {}
    self.flotantes = {}
    self.botines = {}
    self.combate = 0          -- segundos que quedan "en combate"
    self.autoataque = 0
    self.guardado = 30
    self.avisoTiempo = 0
    self.muerto = false
    local nueva = not self:cargar()
    self:recalcular()
    self.vida = self.vidaMax
    self.mana = self.manaMax
    if nueva then
        self:dar("pocion_vida", 3, true)
        self:dar("pocion_mana", 2, true)
    end
    self:Chat("[Sistema] Bienvenido a Villa Alba, " .. self.nombre .. ". Pulsa H para ver los controles.")
    self:Chat("[Sistema] Habla con los personajes con una ! amarilla (E) para conseguir misiones.")
    self:panel(nil)
    self:pintarTodo()
end

function Heroe:buscarInterfaz()
    local u = {}
    local function f(n) return Scene.find(n) end
    u.jugador = f("UI_Jugador")
    u.objetivo = f("UI_Objetivo")
    u.xpBarra = f("UI_XPBarra")
    u.xpTexto = f("UI_XPTexto")
    u.lanzamiento = f("UI_Lanzamiento")
    u.chat = f("UI_Chat")
    u.seguimiento = f("UI_Seguimiento")
    u.mapa = f("UI_Mapa")
    u.zona = f("UI_Zona")
    u.aviso = f("UI_Aviso")
    u.ayuda = f("UI_Ayuda")
    u.inventario = f("UI_Inventario")
    u.personaje = f("UI_Personaje")
    u.diario = f("UI_Diario")
    u.dialogo = f("UI_Dialogo")
    u.muerte = f("UI_Muerte")
    u.habilidades = {}
    for i = 1, #HABILIDADES do u.habilidades[i] = f("UI_Hab " .. i) end
    u.flotantes = {}
    for i = 1, 8 do u.flotantes[i] = f("UI_Flotante " .. i) end
    u.puntos = {}
    if u.mapa then
        for i = 1, 40 do u.puntos[i] = u.mapa:find("Punto " .. i) end
    end
    u.ranuras = {}
    if u.inventario then
        for i = 1, RANURAS do u.ranuras[i] = u.inventario:find("Ranura " .. i) end
    end
    u.opciones = {}
    if u.dialogo then
        for i = 1, 6 do u.opciones[i] = u.dialogo:find("Opcion " .. i) end
    end
    self.ui = u
end

-- =========================================================================
-- Cada frame
-- =========================================================================

function Heroe:Update(dt)
    self.gcd = math.max(0, self.gcd - dt)
    for i, t in pairs(self.enfriamientos) do self.enfriamientos[i] = math.max(0, t - dt) end
    self.combate = math.max(0, self.combate - dt)
    self:animarFlotantes(dt)
    self:animarAviso(dt)
    if not self.muerto then
        self:teclas()
        self:mover(dt)
        self:lanzar(dt)
        self:atacarSolo(dt)
        self:regenerar(dt)
        self:recogerBotin(dt)
        self:alejarseDelDialogo()
    end
    self:marcasNpc()
    self:pintarHud()
    self.guardado = self.guardado - dt
    if self.guardado <= 0 then
        self.guardado = 30
        self:guardar()
    end
end

function Heroe:panelAbierto()
    local u = self.ui
    return (u.inventario and u.inventario.active) or (u.personaje and u.personaje.active) or
           (u.diario and u.diario.active) or (u.dialogo and u.dialogo.active) or (u.ayuda and u.ayuda.active)
end

-- Abre un panel ("inventario", "personaje", "diario", "ayuda") o los cierra (nil).
function Heroe:panel(nombre, alternar)
    local u = self.ui
    local paneles = { inventario = u.inventario, personaje = u.personaje, diario = u.diario, ayuda = u.ayuda }
    local abrir = nombre
    if alternar and paneles[nombre] and paneles[nombre].active then abrir = nil end
    for n, p in pairs(paneles) do
        if p then p.active = (n == abrir) end
    end
    if abrir ~= "inventario" then self.vendiendo = false end
    if u.dialogo and abrir ~= nil then u.dialogo.active = false end
    self:pintarPaneles()
end

function Heroe:teclas()
    if Input.getKeyDown("i") then self:panel("inventario", true) end
    if Input.getKeyDown("c") then self:panel("personaje", true) end
    if Input.getKeyDown("l") then self:panel("diario", true) end
    if Input.getKeyDown("h") then self:panel("ayuda", true) end
    if Input.getKeyDown("escape") then
        if self:panelAbierto() then
            self:panel(nil)
            if self.ui.dialogo then self.ui.dialogo.active = false end
        else
            self.objetivo = nil
        end
    end
    if Input.getKeyDown("tab") then self:siguienteObjetivo() end
    if Input.getMouseButtonDown(0) and not self:panelAbierto() then
        local e = self:apuntado()
        if e then self.objetivo = e end
    end
    if Input.getKeyDown("e") then self:hablar() end
    for i = 1, #HABILIDADES do
        if Input.getKeyDown(tostring(i)) then self:usarHabilidad(i) end
    end
    if Input.getKeyDown("f9") then
        Prefs.deleteKey("mmo_partida")
        self:Chat("[Sistema] Partida borrada: la proxima vez empezaras de cero.")
        self.noGuardar = true
    end
end

function Heroe:mover(dt)
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
    local t = Mathf.clamp01(12 * dt)
    v.x = Mathf.lerp(v.x, objetivo.x, t)
    v.z = Mathf.lerp(v.z, objetivo.z, t)
    if Input.getKeyDown("space") and self:enSuelo() then v.y = self.salto end
    self.entity.velocity = v
    self.moviendose = direccion:length() > 0.1
    if self.modelo then
        if self.moviendose then
            self.modelo:lookAt(self.modelo.position + direccion)
        elseif self:objetivoValido() then
            local p = self.objetivo.position
            self.modelo:lookAt(Vec3(p.x, self.modelo.position.y, p.z))
        end
    end
    if self.entity.position.y < -30 then self:reaparecerEn(1.0) end
end

function Heroe:enSuelo()
    return Physics.raycast(self.entity.position + Vec3(0, -1.02, 0), Vec3.down, 0.25) ~= nil
end

-- =========================================================================
-- Estadisticas y niveles
-- =========================================================================

function Heroe:recalcular()
    local arma = ITEMS[self.equipo.arma or ""] or {}
    local armadura = ITEMS[self.equipo.armadura or ""] or {}
    self.vidaMax = 100 + (self.nivel - 1) * 28
    self.manaMax = 80 + (self.nivel - 1) * 16
    self.ataque = 4 + self.nivel * 2 + (arma.ataque or 0)
    self.defensa = self.nivel + (armadura.defensa or 0)
    self.poder = 10 + self.nivel * 4
    if self.vida then self.vida = math.min(self.vida, self.vidaMax) end
    if self.mana then self.mana = math.min(self.mana, self.manaMax) end
end

function Heroe:ganarXp(cantidad)
    if self.nivel >= NIVEL_MAX then return end
    cantidad = math.floor(cantidad)
    self.xp = self.xp + cantidad
    self:Chat(string.format("[Combate] Ganas %d de experiencia.", cantidad))
    while self.nivel < NIVEL_MAX and self.xp >= xpNivel(self.nivel) do
        self.xp = self.xp - xpNivel(self.nivel)
        self.nivel = self.nivel + 1
        self:recalcular()
        self.vida = self.vidaMax
        self.mana = self.manaMax
        self:Aviso("¡Nivel " .. self.nivel .. "!", Vec3(1.0, 0.85, 0.3))
        self:Chat("[Sistema] ¡Has subido al nivel " .. self.nivel .. "! Vida y mana al maximo.")
        for _, h in ipairs(HABILIDADES) do
            if h.nivel == self.nivel then self:Chat("[Sistema] Nueva habilidad: " .. h.nombre .. ".") end
        end
    end
    if self.nivel >= NIVEL_MAX then self.xp = 0 end
    self:guardar()
end

function Heroe:regenerar(dt)
    local fuera = self.combate <= 0
    self.vida = math.min(self.vidaMax, self.vida + self.vidaMax * (fuera and 0.04 or 0.004) * dt)
    self.mana = math.min(self.manaMax, self.mana + self.manaMax * (fuera and 0.05 or 0.012) * dt)
end

-- =========================================================================
-- Objetivo
-- =========================================================================

function Heroe:objetivoValido()
    if not self.objetivo then return false end
    local s = self.objetivo:getScript()
    return s ~= nil and s:EstaVivo()
end

function Heroe:vivos(maximo)
    local lista = {}
    for _, e in ipairs(self.enemigos) do
        local s = e:getScript()
        if s and s:EstaVivo() then
            local d = self.entity:distanceTo(e)
            if d <= maximo then lista[#lista + 1] = { e = e, d = d } end
        end
    end
    table.sort(lista, function(a, b) return a.d < b.d end)
    return lista
end

-- Tab: el siguiente mas cercano (va rotando entre los de alrededor).
function Heroe:siguienteObjetivo()
    local lista = self:vivos(35)
    if #lista == 0 then
        self.objetivo = nil
        return
    end
    local siguiente = 1
    for i, v in ipairs(lista) do
        if v.e == self.objetivo then siguiente = (i % #lista) + 1 end
    end
    self.objetivo = lista[siguiente].e
end

-- Clic: el enemigo mas cerca del centro de la camara.
function Heroe:apuntado()
    if not self.camara then return nil end
    local origen = self.camara.position
    local dir = self.camara.forward
    local mejor, mejorCos = nil, 0.965
    for _, e in ipairs(self.enemigos) do
        local s = e:getScript()
        if s and s:EstaVivo() then
            local v = e.position - origen
            local d = v:length()
            if d > 0.5 and d < 50 then
                local c = v:dot(dir) / d
                if c > mejorCos then mejor, mejorCos = e, c end
            end
        end
    end
    return mejor
end

function Heroe:objetivoCercano()
    local lista = self:vivos(8)
    if #lista > 0 then self.objetivo = lista[1].e end
end

-- =========================================================================
-- Combate
-- =========================================================================

function Heroe:EstaVivo() return not self.muerto end

function Heroe:usarHabilidad(i)
    local h = HABILIDADES[i]
    if not h or self.muerto then return false end
    if h.nivel and self.nivel < h.nivel then
        self:Aviso(h.nombre .. ": necesitas nivel " .. h.nivel)
        return false
    end
    if (self.enfriamientos[i] or 0) > 0 then
        self:Aviso(h.nombre .. " no esta lista")
        return false
    end
    if h.item then
        if self:cuenta(h.item) <= 0 then
            self:Aviso("No te quedan: " .. ITEMS[h.item].nombre)
            return false
        end
        self:usarObjeto(h.item)
        self.enfriamientos[i] = h.cd
        return true
    end
    if self.gcd > 0 or self.lanzando then return false end
    if (h.mana or 0) > self.mana then
        self:Aviso("No tienes mana suficiente")
        return false
    end
    if h.objetivo then
        if not self:objetivoValido() then self:objetivoCercano() end
        if not self:objetivoValido() then
            self:Aviso("No tienes objetivo (Tab)")
            return false
        end
        if self.entity:distanceTo(self.objetivo) > h.alcance then
            self:Aviso("Demasiado lejos")
            return false
        end
    end
    self.gcd = GCD
    if h.lanzar > 0 then
        self.lanzando = { i = i, t = 0, dur = h.lanzar }
        return true
    end
    self:aplicarHabilidad(i)
    return true
end

-- La barra de lanzamiento: moverse la interrumpe.
function Heroe:lanzar(dt)
    local l = self.lanzando
    if not l then return end
    if self.moviendose then
        self.lanzando = nil
        self:Aviso("Interrumpido")
        return
    end
    l.t = l.t + dt
    if l.t >= l.dur then
        self.lanzando = nil
        local h = HABILIDADES[l.i]
        if h.objetivo and (not self:objetivoValido() or self.entity:distanceTo(self.objetivo) > h.alcance + 2) then
            self:Aviso("El objetivo ya no esta al alcance")
            return
        end
        self:aplicarHabilidad(l.i)
    end
end

function Heroe:golpe(base)
    local critico = Random.chance(0.12)
    local d = base * Mathf.random(0.9, 1.1)
    if critico then d = d * 1.8 end
    return math.floor(d + 0.5), critico
end

function Heroe:danar(enemigo, cantidad, critico, que)
    local s = enemigo:getScript()
    if not s or not s:EstaVivo() then return end
    local hecho = s:Danar(cantidad, self.entity, true)
    self.combate = 6
    self:Flotante((critico and "¡" or "") .. hecho .. (critico and "!" or ""), critico and C_CRITICO or C_DANO)
    self:Chat(string.format("[Combate] %s a %s: %d%s", que, s:Nombre(), hecho, critico and " (critico)" or ""))
end

function Heroe:aplicarHabilidad(i)
    local h = HABILIDADES[i]
    self.mana = self.mana - (h.mana or 0)
    if h.cd and h.cd > 0 then self.enfriamientos[i] = h.cd end
    if i == 1 then
        local d, c = self:golpe(self.ataque + 5)
        self:danar(self.objetivo, d, c, "Golpe")
        self.autoataque = AUTOATAQUE
    elseif i == 2 then
        local d, c = self:golpe(self.poder * 1.4 + 8)
        self:danar(self.objetivo, d, c, "Bola de fuego")
    elseif i == 3 then
        local cura = math.floor(self.poder * 2 + 20)
        self.vida = math.min(self.vidaMax, self.vida + cura)
        self:Flotante("+" .. cura, C_CURA)
        self:Chat("[Combate] Curar: +" .. cura .. " de vida.")
    elseif i == 4 then
        local tocados = 0
        for _, v in ipairs(self:vivos(h.alcance)) do
            local d, c = self:golpe(self.ataque * 0.9 + 6)
            self:danar(v.e, d, c, "Torbellino")
            tocados = tocados + 1
        end
        if tocados == 0 then self:Chat("[Combate] Torbellino: no habia nadie cerca.") end
    end
end

-- Con un objetivo al lado y en combate, se golpea solo cada 2 s.
function Heroe:atacarSolo(dt)
    self.autoataque = math.max(0, self.autoataque - dt)
    if self.lanzando or self.autoataque > 0 or self.combate <= 0 or not self:objetivoValido() then return end
    if self.entity:distanceTo(self.objetivo) > HABILIDADES[1].alcance then return end
    self.autoataque = AUTOATAQUE
    local d, c = self:golpe(self.ataque * 0.7)
    self:danar(self.objetivo, d, c, "Ataque")
end

-- Lo llaman los enemigos al golpear.
function Heroe:RecibirDano(cantidad, fuente, nombre)
    if self.muerto then return end
    local d = math.max(1, math.floor(cantidad - self.defensa * 0.5 + 0.5))
    self.vida = self.vida - d
    self.combate = 6
    self:Flotante("-" .. d, C_RECIBIDO)
    self:Chat(string.format("[Combate] %s te golpea: %d", nombre or "Algo", d))
    if self.lanzando and Random.chance(0.25) then
        self.lanzando = nil
        self:Aviso("¡Te han interrumpido!")
    end
    if self.vida <= 0 then self:morir(nombre) end
end

function Heroe:morir(causa)
    self.vida = 0
    self.muerto = true
    self.lanzando = nil
    self.objetivo = nil
    self.entity.velocity = Vec3.zero
    self:panel(nil)
    if self.ui.dialogo then self.ui.dialogo.active = false end
    if self.ui.muerte then
        self.ui.muerte.active = true
        local t = self.ui.muerte:find("Causa")
        if t then t.text = "Te ha matado: " .. (causa or "algo") end
    end
    self:Chat("[Sistema] Has muerto. Reaparecer te lleva al pueblo.")
end

function Heroe:reaparecerEn(fraccion)
    local p = self.spawn and self.spawn.position or Vec3(0, 2, 6)
    self.entity.position = p
    self.entity.velocity = Vec3.zero
    self.vida = math.max(1, math.floor(self.vidaMax * fraccion))
    self.mana = math.floor(self.manaMax * fraccion)
    self.muerto = false
    self.combate = 0
    if self.ui.muerte then self.ui.muerte.active = false end
end

function Heroe:OnReaparecer() self:reaparecerEn(0.5) end

-- Un enemigo murio (Enemigo.lua): experiencia, misiones, oro y botin.
function Heroe:EnemigoMuerto(enemigo, credito, botin, oro)
    if enemigo.entity == self.objetivo then self.objetivo = nil end
    if not credito then return end
    local diferencia = self.nivel - enemigo:Nivel()
    local factor = Mathf.clamp(1 - 0.15 * diferencia, 0.2, 1.5)
    self:ganarXp(enemigo:Xp() * factor)
    self:Chat("[Combate] " .. enemigo:Nombre() .. " ha muerto.")
    for _, m in ipairs(MISIONES) do
        local estado = self.misiones[m.id]
        if estado and estado.estado == "activa" and m.tipo == "matar" and m.objetivo == enemigo.tipo then
            estado.progreso = math.min(m.cantidad, estado.progreso + 1)
            if estado.progreso >= m.cantidad then
                estado.estado = "lista"
                self:Aviso("Mision completada: " .. m.nombre, Vec3(1, 0.85, 0.3))
                self:Chat("[Mision] " .. m.nombre .. ": completada. Vuelve con " .. m.npc .. ".")
            else
                self:Chat(string.format("[Mision] %s: %d/%d", m.nombre, estado.progreso, m.cantidad))
            end
        end
    end
    -- Bolsa de botin en el suelo (se recoge al pasar por encima).
    if (#botin > 0 or oro > 0) and self.plantillaBotin then
        local bolsa = Scene.instantiate(self.plantillaBotin, enemigo.entity.position + Vec3(0, -0.6, 0))
        if bolsa then
            bolsa.active = true
            self.botines[#self.botines + 1] = { e = bolsa, items = botin, oro = oro, vida = 90 }
        end
    end
end

function Heroe:recogerBotin(dt)
    for i = #self.botines, 1, -1 do
        local b = self.botines[i]
        b.vida = b.vida - dt
        if b.e and b.e:valid() then b.e:rotate(Vec3(0, 90 * dt, 0)) end
        local cerca = b.e and b.e:valid() and self.entity:distanceTo(b.e) < 2.4
        if cerca then
            if b.oro > 0 then
                self.oro = self.oro + b.oro
                self:Chat("[Botin] Recoges " .. b.oro .. " de oro.")
                b.oro = 0
            end
            local quedan = {}
            for _, id in ipairs(b.items) do
                if self:dar(id, 1) > 0 then quedan[#quedan + 1] = id end
            end
            b.items = quedan
            if #quedan > 0 then self:Aviso("Inventario lleno") end
        end
        if b.vida <= 0 or (#b.items == 0 and b.oro == 0) then
            if b.e and b.e:valid() then b.e:destroy() end
            table.remove(self.botines, i)
        end
    end
end

-- =========================================================================
-- Inventario y equipo
-- =========================================================================

function Heroe:cuenta(id)
    local n = 0
    for _, r in ipairs(self.inventario) do
        if r.id == id then n = n + r.n end
    end
    return n
end

-- Da `n` objetos; devuelve los que no caben.
function Heroe:dar(id, n, callado)
    local item = ITEMS[id]
    if not item then return n end
    local apila = item.tipo ~= "arma" and item.tipo ~= "armadura"
    local quedan = n
    if apila then
        for _, r in ipairs(self.inventario) do
            if quedan <= 0 then break end
            if r.id == id and r.n < PILA then
                local pon = math.min(PILA - r.n, quedan)
                r.n = r.n + pon
                quedan = quedan - pon
            end
        end
    end
    while quedan > 0 and #self.inventario < RANURAS do
        local pon = apila and math.min(PILA, quedan) or 1
        self.inventario[#self.inventario + 1] = { id = id, n = pon }
        quedan = quedan - pon
    end
    local dados = n - quedan
    if dados > 0 and not callado then
        self:Chat("[Botin] Recibes " .. item.nombre .. (dados > 1 and (" x" .. dados) or "") .. ".")
    end
    self:revisarRecoger()
    self:pintarPaneles()
    return quedan
end

function Heroe:quitar(id, n)
    for i = #self.inventario, 1, -1 do
        local r = self.inventario[i]
        if n <= 0 then break end
        if r.id == id then
            local q = math.min(r.n, n)
            r.n = r.n - q
            n = n - q
            if r.n <= 0 then table.remove(self.inventario, i) end
        end
    end
    self:revisarRecoger()
    self:pintarPaneles()
end

function Heroe:usarObjeto(id)
    local item = ITEMS[id]
    if not item or self:cuenta(id) <= 0 then return false end
    if item.tipo == "consumible" then
        self:quitar(id, 1)
        if item.cura then
            self.vida = math.min(self.vidaMax, self.vida + item.cura)
            self:Flotante("+" .. item.cura, C_CURA)
        end
        if item.mana then
            self.mana = math.min(self.manaMax, self.mana + item.mana)
            self:Flotante("+" .. item.mana, C_MANA)
        end
        self:Chat("[Sistema] Usas " .. item.nombre .. ".")
        return true
    end
    if item.tipo == "arma" or item.tipo == "armadura" then return self:equipar(id) end
    return false
end

function Heroe:equipar(id)
    local item = ITEMS[id]
    if item.nivel and self.nivel < item.nivel then
        self:Aviso(item.nombre .. ": necesitas nivel " .. item.nivel)
        return false
    end
    local hueco = item.tipo
    self:quitar(id, 1)
    local anterior = self.equipo[hueco]
    self.equipo[hueco] = id
    if anterior then self:dar(anterior, 1, true) end
    self:recalcular()
    self:Chat("[Sistema] Te equipas: " .. item.nombre .. ".")
    self:pintarPaneles()
    return true
end

function Heroe:OnDesequipar(boton)
    local hueco = boton.name == "Arma" and "arma" or "armadura"
    local id = self.equipo[hueco]
    if not id then return end
    if #self.inventario >= RANURAS then
        self:Aviso("Inventario lleno")
        return
    end
    self.equipo[hueco] = nil
    self:dar(id, 1, true)
    self:recalcular()
end

-- Clic en una ranura: usar/equipar, o vender si la tienda esta abierta.
function Heroe:OnRanura(boton)
    local r = self.inventario[numeroDe(boton.name)]
    if not r then return end
    local item = ITEMS[r.id]
    if self.vendiendo then
        local precio = math.max(1, math.floor(item.precio / 2))
        self:quitar(r.id, 1)
        self.oro = self.oro + precio
        self:Chat(string.format("[Tienda] Vendes %s por %d de oro.", item.nombre, precio))
        self:pintarDialogoTienda()
        return
    end
    self:usarObjeto(r.id)
end

function Heroe:comprar(id)
    local item = ITEMS[id]
    if self.oro < item.precio then
        self:Aviso("No tienes oro suficiente")
        return
    end
    if self:dar(id, 1, true) > 0 then
        self:Aviso("Inventario lleno")
        return
    end
    self.oro = self.oro - item.precio
    self:Chat(string.format("[Tienda] Compras %s por %d de oro.", item.nombre, item.precio))
    self:pintarDialogoTienda()
end

-- =========================================================================
-- Misiones y dialogos
-- =========================================================================

function Heroe:mision(id)
    for _, m in ipairs(MISIONES) do
        if m.id == id then return m end
    end
    return nil
end

function Heroe:disponible(m)
    if self.misiones[m.id] then return false end
    if self.nivel < m.nivel then return false end
    if m.requiere then
        local r = self.misiones[m.requiere]
        if not r or r.estado ~= "hecha" then return false end
    end
    return true
end

function Heroe:aceptarMision(id)
    local m = self:mision(id)
    if not m or not self:disponible(m) then return false end
    self.misiones[id] = { estado = "activa", progreso = 0 }
    self:revisarRecoger()
    self:Aviso("Nueva mision: " .. m.nombre, Vec3(1, 0.85, 0.3))
    self:Chat("[Mision] Aceptada: " .. m.nombre .. ".")
    self:guardar()
    return true
end

-- Misiones de recoger: el progreso es lo que llevas en el inventario.
function Heroe:revisarRecoger()
    if not self.misiones then return end
    for _, m in ipairs(MISIONES) do
        local e = self.misiones[m.id]
        if e and m.tipo == "recoger" and (e.estado == "activa" or e.estado == "lista") then
            local antes = e.estado
            e.progreso = math.min(m.cantidad, self:cuenta(m.objetivo))
            e.estado = e.progreso >= m.cantidad and "lista" or "activa"
            if antes == "activa" and e.estado == "lista" then
                self:Aviso("Mision completada: " .. m.nombre, Vec3(1, 0.85, 0.3))
                self:Chat("[Mision] " .. m.nombre .. ": completada. Vuelve con " .. m.npc .. ".")
            end
        end
    end
end

function Heroe:entregarMision(id)
    local m = self:mision(id)
    local e = self.misiones[id]
    if not m or not e or e.estado ~= "lista" then return false end
    if m.tipo == "recoger" then self:quitar(m.objetivo, m.cantidad) end
    e.estado = "hecha"
    self.oro = self.oro + m.oro
    self:Chat(string.format("[Mision] %s entregada: %d de oro.", m.nombre, m.oro))
    if m.premio then self:dar(m.premio, m.premioCantidad or 1) end
    self:Aviso("Mision entregada: " .. m.nombre, Vec3(0.5, 1, 0.5))
    self:ganarXp(m.xp)
    return true
end

function Heroe:npcCercano(maximo)
    local mejor, dist = nil, maximo
    for _, n in ipairs(self.npcs) do
        local d = self.entity:distanceTo(n)
        if d < dist then mejor, dist = n, d end
    end
    return mejor
end

function Heroe:hablar()
    local npc = self:npcCercano(4.5)
    if not npc then
        self:Aviso("No hay nadie cerca con quien hablar")
        return
    end
    self:abrirDialogo(npc)
end

function Heroe:dialogo(titulo, cuerpo, opciones)
    local d = self.ui.dialogo
    if not d then return end
    self:panel(nil)
    d.active = true
    d:find("Titulo").text = titulo
    d:find("Cuerpo").text = cuerpo
    self.opciones = opciones
    for i, b in ipairs(self.ui.opciones) do
        local o = opciones[i]
        b.active = o ~= nil
        if o then b:find("Texto").text = o.texto end
    end
end

function Heroe:abrirDialogo(npc)
    self.npcHablando = npc
    self.vendiendo = false
    local s = npc:getScript()
    local nombre = s:Nombre()
    local opciones = {}
    local cuerpo = s:Saludo()
    for _, m in ipairs(MISIONES) do
        if m.npc == nombre then
            local e = self.misiones[m.id]
            if e and e.estado == "lista" then
                opciones[#opciones + 1] = { texto = "Entregar: " .. m.nombre, accion = function() self:ofrecerEntrega(npc, m) end }
            elseif self:disponible(m) then
                opciones[#opciones + 1] = { texto = "Mision: " .. m.nombre, accion = function() self:ofrecerMision(npc, m) end }
            elseif e and e.estado == "activa" then
                cuerpo = cuerpo .. string.format("\n\n(%s: %d/%d)", m.nombre, e.progreso, m.cantidad)
            end
        end
    end
    if s:EsComerciante() then
        opciones[#opciones + 1] = { texto = "Comerciar", accion = function() self:abrirTienda(npc) end }
    end
    opciones[#opciones + 1] = { texto = "Adios", accion = function() self:cerrarDialogo() end }
    self:dialogo(nombre, cuerpo, opciones)
end

function Heroe:recompensa(m)
    local t = string.format("Recompensa: %d XP, %d de oro", m.xp, m.oro)
    if m.premio then
        t = t .. ", " .. ITEMS[m.premio].nombre .. ((m.premioCantidad or 1) > 1 and (" x" .. m.premioCantidad) or "")
    end
    return t
end

function Heroe:ofrecerMision(npc, m)
    self:dialogo(m.nombre, m.texto .. "\n\n" .. self:recompensa(m), {
        { texto = "Aceptar", accion = function() self:aceptarMision(m.id); self:abrirDialogo(npc) end },
        { texto = "Ahora no", accion = function() self:abrirDialogo(npc) end },
    })
end

function Heroe:ofrecerEntrega(npc, m)
    self:dialogo(m.nombre, m.fin .. "\n\n" .. self:recompensa(m), {
        { texto = "Entregar", accion = function() self:entregarMision(m.id); self:abrirDialogo(npc) end },
        { texto = "Volver", accion = function() self:abrirDialogo(npc) end },
    })
end

function Heroe:abrirTienda(npc)
    self.tiendaNpc = npc
    self:pintarDialogoTienda()
end

function Heroe:pintarDialogoTienda()
    if not self.tiendaNpc then return end
    local opciones = {}
    for _, id in ipairs(TIENDA) do
        local it = ITEMS[id]
        if #opciones < 5 then
            local nivel = it.nivel and (" · Nv " .. it.nivel) or ""
            opciones[#opciones + 1] = { texto = string.format("%s  (%d oro%s)", it.nombre, it.precio, nivel),
                                        accion = function() self:comprar(id) end }
        end
    end
    opciones[#opciones + 1] = { texto = "Adios", accion = function() self:cerrarDialogo() end }
    self:dialogo(self.tiendaNpc:getScript():Nombre() .. " · Tienda",
                 "Tu oro: " .. self.oro .. "\nClic en un objeto para comprarlo.\nClic en tu inventario (a la izquierda) para vender a mitad de precio.",
                 opciones)
    -- El inventario a la vez, para vender.
    if self.ui.inventario then self.ui.inventario.active = true end
    self.vendiendo = true
    self:pintarPaneles()
end

function Heroe:cerrarDialogo()
    if self.ui.dialogo then self.ui.dialogo.active = false end
    if self.vendiendo and self.ui.inventario then self.ui.inventario.active = false end
    self.vendiendo = false
    self.tiendaNpc = nil
    self.npcHablando = nil
end

function Heroe:OnOpcion(boton)
    local o = self.opciones and self.opciones[numeroDe(boton.name)]
    if o then o.accion() end
end

function Heroe:OnCerrar(boton)
    local p = boton.parent
    if p then p.active = false end
    if p == self.ui.dialogo then self:cerrarDialogo() end
    if p == self.ui.inventario then self.vendiendo = false end
end

function Heroe:alejarseDelDialogo()
    if self.npcHablando and self.entity:distanceTo(self.npcHablando) > 7 then self:cerrarDialogo() end
end

-- La marca de cada NPC: ! (mision nueva) o ? (para entregar).
function Heroe:marcasNpc()
    for _, npc in ipairs(self.npcs) do
        local s = npc:getScript()
        if s and s.Nombre then
            local nombre = s:Nombre()
            local marca = nil
            for _, m in ipairs(MISIONES) do
                if m.npc == nombre then
                    local e = self.misiones[m.id]
                    if e and e.estado == "lista" then
                        marca = "entregar"
                    elseif marca == nil and self:disponible(m) then
                        marca = "mision"
                    end
                end
            end
            s:Marca(marca)
        end
    end
end

-- =========================================================================
-- Chat, avisos y textos flotantes
-- =========================================================================

function Heroe:Chat(texto)
    if not self.lineas then self.lineas = {} end
    self.lineas[#self.lineas + 1] = texto
    while #self.lineas > 9 do table.remove(self.lineas, 1) end
    if self.ui and self.ui.chat then self.ui.chat.text = table.concat(self.lineas, "\n") end
end

function Heroe:Aviso(texto, color)
    local a = self.ui and self.ui.aviso
    if not a then return end
    a.text = texto
    a.color = color or Vec3(1, 1, 1)
    a.alpha = 1
    self.avisoTiempo = 2.5
end

function Heroe:animarAviso(dt)
    local a = self.ui and self.ui.aviso
    if not a then return end
    self.avisoTiempo = math.max(0, (self.avisoTiempo or 0) - dt)
    a.alpha = Mathf.clamp01(self.avisoTiempo / 0.8)
end

function Heroe:Flotante(texto, color)
    local lista = self.ui and self.ui.flotantes
    if not lista then return end
    self.siguienteFlotante = ((self.siguienteFlotante or 0) % #lista) + 1
    local t = lista[self.siguienteFlotante]
    if not t then return end
    t.text = texto
    t.color = color
    t.alpha = 1
    t.uiPosition = Vec3(Mathf.random(-90, 90), Mathf.random(-170, -120), 0)
    self.flotantes[self.siguienteFlotante] = 1.2
end

function Heroe:animarFlotantes(dt)
    local lista = self.ui and self.ui.flotantes
    if not lista then return end
    for i, t in ipairs(lista) do
        local vida = self.flotantes[i]
        if vida and vida > 0 then
            vida = vida - dt
            self.flotantes[i] = vida
            t.uiPosition = t.uiPosition + Vec3(0, -70 * dt, 0)
            t.alpha = Mathf.clamp01(vida / 0.6)
        elseif t.alpha > 0 then
            t.alpha = 0
        end
    end
end

-- =========================================================================
-- Interfaz
-- =========================================================================

local function barra(e, fraccion, ancho)
    if e then e.uiSize = Vec3(math.max(0, ancho * Mathf.clamp01(fraccion)), e.uiSize.y, 0) end
end

function Heroe:pintarTodo()
    self:pintarHud()
    self:pintarPaneles()
end

function Heroe:pintarHud()
    local u = self.ui
    if u.jugador then
        u.jugador:find("Nombre").text = string.format("%s  ·  Nivel %d", self.nombre, self.nivel)
        barra(u.jugador:find("VidaBarra"), self.vida / self.vidaMax, 320)
        barra(u.jugador:find("ManaBarra"), self.mana / self.manaMax, 320)
        u.jugador:find("VidaTexto").text = string.format("%d / %d", math.floor(self.vida + 0.5), self.vidaMax)
        u.jugador:find("ManaTexto").text = string.format("%d / %d", math.floor(self.mana + 0.5), self.manaMax)
    end
    if u.objetivo then
        local valido = self:objetivoValido()
        u.objetivo.active = valido
        if valido then
            local s = self.objetivo:getScript()
            local vida, maximo = s:Vida()
            local n = u.objetivo:find("Nombre")
            n.text = string.format("%s  ·  Nv %d", s:Nombre(), s:Nivel())
            n.color = s:EsJefe() and Vec3(1.0, 0.55, 0.2) or Vec3(1, 0.85, 0.85)
            barra(u.objetivo:find("VidaBarra"), vida / maximo, 320)
            u.objetivo:find("VidaTexto").text = string.format("%d / %d  ·  %.0f m", vida, maximo, self.entity:distanceTo(self.objetivo))
        end
    end
    if u.xpBarra then
        local necesita = xpNivel(self.nivel)
        barra(u.xpBarra, self.nivel >= NIVEL_MAX and 1 or self.xp / necesita, 900)
        u.xpTexto.text = self.nivel >= NIVEL_MAX and ("Nivel " .. self.nivel .. " (maximo)") or
                         string.format("Nivel %d  ·  %d / %d XP", self.nivel, self.xp, necesita)
    end
    if u.lanzamiento then
        u.lanzamiento.active = self.lanzando ~= nil
        if self.lanzando then
            barra(u.lanzamiento:find("Barra"), self.lanzando.t / self.lanzando.dur, 400)
            u.lanzamiento:find("Texto").text = HABILIDADES[self.lanzando.i].nombre
        end
    end
    self:pintarHabilidades()
    self:pintarSeguimiento()
    self:pintarMapa()
end

function Heroe:pintarHabilidades()
    for i, b in ipairs(self.ui.habilidades) do
        local h = HABILIDADES[i]
        local cd = self.enfriamientos[i] or 0
        local total = h.cd or 0
        if not h.item and self.gcd > cd then cd, total = self.gcd, GCD end
        local bloqueada = h.nivel and self.nivel < h.nivel
        local sombra = b:find("Enfriamiento")
        local frac = total > 0 and cd / total or 0
        if bloqueada then frac = 1 end
        sombra.uiSize = Vec3(sombra.uiSize.x, 72 * Mathf.clamp01(frac), 0)
        local tiempo = b:find("Tiempo")
        tiempo.text = bloqueada and ("Nv " .. h.nivel) or (cd > 0.05 and total > GCD and tostring(math.ceil(cd)) or "")
        local nombre = b:find("Nombre")
        if h.item then
            nombre.text = h.nombre:gsub("Pocion de ", "") .. " x" .. self:cuenta(h.item)
        else
            nombre.text = h.nombre
        end
        local falta = (h.mana or 0) > self.mana
        nombre.color = falta and Vec3(0.55, 0.6, 1.0) or Vec3(1, 1, 1)
    end
end

function Heroe:pintarSeguimiento()
    local t = self.ui.seguimiento
    if not t then return end
    local lineas = { "Misiones (L)" }
    for _, m in ipairs(MISIONES) do
        local e = self.misiones[m.id]
        if e and e.estado ~= "hecha" then
            if e.estado == "lista" then
                lineas[#lineas + 1] = "· " .. m.nombre .. ": ¡lista! (" .. m.npc .. ")"
            else
                lineas[#lineas + 1] = string.format("· %s: %d/%d", m.nombre, e.progreso, m.cantidad)
            end
        end
    end
    if #lineas == 1 then lineas[2] = "· Habla con los ! del pueblo" end
    t.text = table.concat(lineas, "\n")
end

-- Minimapa: 60 m alrededor, norte arriba. Rojo enemigos, verde otros
-- jugadores, amarillo NPC, dorado botin.
function Heroe:pintarMapa()
    local u = self.ui
    if not u.mapa then return end
    local p = self.entity.position
    local escala = 110 / 60
    local usados = 0
    local function punto(pos, color, tam)
        local dx, dz = (pos.x - p.x) * escala, (pos.z - p.z) * escala
        if dx * dx + dz * dz > 105 * 105 or usados >= #u.puntos then return end
        usados = usados + 1
        local d = u.puntos[usados]
        d.active = true
        d.uiPosition = Vec3(dx, dz, 0)
        d.uiSize = Vec3(tam, tam, 0)
        d.color = color
    end
    for _, n in ipairs(self.npcs) do punto(n.position, Vec3(1.0, 0.85, 0.2), 9) end
    for _, b in ipairs(self.bots) do
        local s = b:getScript()
        if s and s:EstaVivo() then punto(b.position, Vec3(0.35, 1.0, 0.45), 8) end
    end
    for _, e in ipairs(self.enemigos) do
        local s = e:getScript()
        if s and s:EstaVivo() then
            local jefe = s:EsJefe()
            punto(e.position, jefe and Vec3(1.0, 0.4, 0.05) or Vec3(1.0, 0.2, 0.2), jefe and 12 or 7)
        end
    end
    for _, b in ipairs(self.botines) do
        if b.e and b.e:valid() then punto(b.e.position, Vec3(1.0, 0.75, 0.2), 6) end
    end
    for i = usados + 1, #u.puntos do u.puntos[i].active = false end
    if u.zona then
        local nombre = "Tierras salvajes"
        for _, z in ipairs(self.zonas) do
            if (z.entidad.position - p):length() < z.radio then nombre = z.nombre end
        end
        u.zona.text = nombre
    end
end

function Heroe:pintarPaneles()
    local u = self.ui
    if not u then return end
    if u.inventario and u.inventario.active then
        u.inventario:find("Oro").text = "Oro: " .. self.oro
        u.inventario:find("Pista").text = self.vendiendo and "Clic: vender (mitad de precio)" or "Clic: usar o equipar"
        for i, b in ipairs(u.ranuras) do
            local r = self.inventario[i]
            local nombre = b:find("Nombre")
            local cantidad = b:find("Cantidad")
            local color = b:find("Color")
            if r then
                local it = ITEMS[r.id]
                nombre.text = it.nombre
                cantidad.text = r.n > 1 and ("x" .. r.n) or ""
                color.alpha = 1
                color.color = it.color
            else
                nombre.text = ""
                cantidad.text = ""
                color.alpha = 0
            end
        end
    end
    if u.personaje and u.personaje.active then
        local necesita = xpNivel(self.nivel)
        u.personaje:find("Stats").text = string.format(
            "%s\nNivel %d  (%d / %d XP)\n\nVida  %d / %d\nMana  %d / %d\nAtaque  %d\nPoder magico  %d\nDefensa  %d\nOro  %d",
            self.nombre, self.nivel, self.xp, necesita, math.floor(self.vida), self.vidaMax, math.floor(self.mana),
            self.manaMax, self.ataque, self.poder, self.defensa, self.oro)
        local arma = ITEMS[self.equipo.arma or ""]
        local armadura = ITEMS[self.equipo.armadura or ""]
        u.personaje:find("Arma"):find("Texto").text = "Arma: " .. (arma and (arma.nombre .. " (+" .. arma.ataque .. " ataque)") or "(nada)")
        u.personaje:find("Armadura"):find("Texto").text = "Armadura: " ..
            (armadura and (armadura.nombre .. " (+" .. armadura.defensa .. " defensa)") or "(nada)")
    end
    if u.diario and u.diario.active then
        local lineas = {}
        for _, m in ipairs(MISIONES) do
            local e = self.misiones[m.id]
            local estado
            if not e then
                estado = self:disponible(m) and ("disponible: habla con " .. m.npc) or ("nivel " .. m.nivel .. (m.requiere and ", tras otra mision" or ""))
            elseif e.estado == "activa" then
                estado = string.format("en curso %d/%d", e.progreso, m.cantidad)
            elseif e.estado == "lista" then
                estado = "¡lista! Vuelve con " .. m.npc
            else
                estado = "completada"
            end
            lineas[#lineas + 1] = m.nombre .. "  —  " .. estado
            if e and e.estado ~= "hecha" then lineas[#lineas + 1] = "    " .. m.texto end
        end
        u.diario:find("Texto").text = table.concat(lineas, "\n")
    end
end

function Heroe:OnHabilidad(boton) self:usarHabilidad(numeroDe(boton.name)) end

-- =========================================================================
-- Guardar y cargar (Prefs)
-- =========================================================================

function Heroe:guardar()
    if self.noGuardar or not self.inventario then return end
    local inv = {}
    for _, r in ipairs(self.inventario) do inv[#inv + 1] = r.id .. ":" .. r.n end
    local mis = {}
    for id, m in pairs(self.misiones) do mis[#mis + 1] = id .. ":" .. m.estado .. ":" .. m.progreso end
    local datos = { "1", tostring(self.nivel), tostring(math.floor(self.xp)), tostring(self.oro), table.concat(inv, ","),
                    self.equipo.arma or "-", self.equipo.armadura or "-", table.concat(mis, ",") }
    Prefs.setString("mmo_partida", table.concat(datos, "|"))
end

function Heroe:cargar()
    local texto = Prefs.getString("mmo_partida", "")
    local d = dividir(texto, "|")
    if #d < 8 or d[1] ~= "1" then return false end
    self.nivel = math.floor(Mathf.clamp(tonumber(d[2]) or 1, 1, NIVEL_MAX))
    self.xp = tonumber(d[3]) or 0
    self.oro = tonumber(d[4]) or 0
    self.inventario = {}
    for _, par in ipairs(dividir(d[5], ",")) do
        local id, n = string.match(par, "([%w_]+):(%d+)")
        if id and ITEMS[id] then self.inventario[#self.inventario + 1] = { id = id, n = tonumber(n) } end
    end
    self.equipo.arma = ITEMS[d[6]] and d[6] or nil
    self.equipo.armadura = ITEMS[d[7]] and d[7] or nil
    for _, trozo in ipairs(dividir(d[8], ",")) do
        local id, estado, progreso = string.match(trozo, "(%d+):(%a+):(%d+)")
        if id then self.misiones[tonumber(id)] = { estado = estado, progreso = tonumber(progreso) } end
    end
    return true
end

function Heroe:OnDestroy() self:guardar() end

return Heroe
)lua";

constexpr const char* kEnemyScript = R"lua(-- Enemigo del MMO: patrulla cerca de su sitio, ataca al heroe o a los otros
-- jugadores que se acercan, vuelve a casa curandose si lo alejan demasiado
-- (leash), muere, suelta botin y reaparece. El Rey Goblin da pisotones en
-- area (avisa antes: alejate).
local Enemigo = {
    properties = {
        tipo = "lobo",   -- lobo, goblin, chaman, rey
        nivel = 1,
    }
}

local TIPOS = {
    lobo   = { nombre = "Lobo", vida = 60, dano = 6, cadencia = 1.6, alcance = 2.3, aggro = 11, xp = 30, oro = { 1, 3 },
               botin = { { "piel_lobo", 0.6 }, { "colmillo", 0.45 }, { "pocion_vida", 0.12 } }, reaparece = 25 },
    goblin = { nombre = "Goblin", vida = 95, dano = 9, cadencia = 1.8, alcance = 2.4, aggro = 13, xp = 50, oro = { 3, 7 },
               botin = { { "oreja_goblin", 0.55 }, { "pocion_mana", 0.15 }, { "espada_acero", 0.04 } }, reaparece = 30 },
    chaman = { nombre = "Chaman goblin", vida = 70, dano = 12, cadencia = 2.6, alcance = 14, aggro = 16, xp = 60, oro = { 4, 9 },
               botin = { { "oreja_goblin", 0.5 }, { "pocion_mana", 0.3 }, { "cota_malla", 0.03 } }, reaparece = 35, distancia = true },
    rey    = { nombre = "Rey Goblin", vida = 900, dano = 20, cadencia = 2.2, alcance = 3.6, aggro = 16, xp = 450, oro = { 60, 90 },
               botin = { { "hacha_rey", 1.0 }, { "pocion_vida", 1.0 } }, reaparece = 120, jefe = true },
}
local LEASH = 30

function Enemigo:iniciar()
    if self.listo then return end
    self.listo = true
    local t = TIPOS[self.tipo] or TIPOS.lobo
    self.datos = t
    self.nivelReal = math.max(1, math.floor(self.nivel))
    local escala = 1 + 0.22 * (self.nivelReal - 1)
    self.vidaMax = math.floor(t.vida * escala)
    self.vida = self.vidaMax
    self.dano = t.dano * (1 + 0.18 * (self.nivelReal - 1))
    self.casa = self.entity.position
    self.modelo = self.entity:find("Modelo")
    self.barra = self.entity:find("Barra")
    self.muerto = false
    self.espera = Mathf.random(1, 5)
    self.recarga = 0
    self.repensar = 0
    self.pisoton = 9
    self.heroe = Scene.find("Jugador")
    self.bots = Scene.findAllWithTag("Bot")
end

function Enemigo:Start() self:iniciar() end
function Enemigo:EstaVivo() self:iniciar(); return not self.muerto end
function Enemigo:Nombre() self:iniciar(); return self.datos.nombre end
function Enemigo:Nivel() self:iniciar(); return self.nivelReal end
function Enemigo:Xp() self:iniciar(); return self.datos.xp * (1 + 0.25 * (self.nivelReal - 1)) end
function Enemigo:EsJefe() self:iniciar(); return self.datos.jefe == true end
function Enemigo:Vida() self:iniciar(); return math.max(0, math.floor(self.vida)), self.vidaMax end

local function vivo(e)
    if not e then return false end
    local s = e:getScript()
    return s ~= nil and s:EstaVivo()
end

function Enemigo:candidatos()
    local lista = {}
    if self.heroe then lista[#lista + 1] = self.heroe end
    for _, b in ipairs(self.bots) do lista[#lista + 1] = b end
    return lista
end

function Enemigo:Update(dt)
    self:iniciar()
    if self.muerto then
        self.reaparecer = self.reaparecer - dt
        if self.reaparecer <= 0 then self:revivir() end
        return
    end
    self.recarga = math.max(0, self.recarga - dt)
    self.repensar = self.repensar - dt

    -- Lejos de casa o sin objetivo vivo: vuelve curandose.
    if self.objetivo and (not vivo(self.objetivo) or (self.entity.position - self.casa):length() > LEASH) then
        self.objetivo = nil
        self.golpeadoPorHeroe = false
        self.volviendo = true
        self.entity:moveTo(self.casa)
    end
    if self.volviendo then
        self.vida = math.min(self.vidaMax, self.vida + self.vidaMax * 0.5 * dt)
        if (self.entity.position - self.casa):length() < 2.5 or not self.entity.isMoving then self.volviendo = false end
        self:pintarBarra()
        return
    end

    if not self.objetivo then
        local mejor, distancia = nil, self.datos.aggro
        for _, c in ipairs(self:candidatos()) do
            if vivo(c) then
                local d = self.entity:distanceTo(c)
                if d < distancia then mejor, distancia = c, d end
            end
        end
        self.objetivo = mejor
    end

    if self.objetivo then
        self:combatir(dt)
    else
        self:patrullar(dt)
        self.vida = math.min(self.vidaMax, self.vida + self.vidaMax * 0.05 * dt)
    end
    self:pintarBarra()
end

function Enemigo:mirar(posicion)
    local p = self.entity.position
    if (Vec3(posicion.x, p.y, posicion.z) - p):length() > 0.1 then self.entity:lookAt(Vec3(posicion.x, p.y, posicion.z)) end
end

function Enemigo:combatir(dt)
    local objetivo = self.objetivo
    local d = self.entity:distanceTo(objetivo)
    local alcance = self.datos.alcance
    if d > alcance then
        if self.repensar <= 0 then
            self.repensar = 0.25
            local destino = objetivo.position
            if self.datos.distancia then destino = self.entity.position:lerp(objetivo.position, 1 - (alcance * 0.8) / d) end
            self.entity:moveTo(destino)
        end
    else
        if self.entity.isMoving then self.entity:stopMoving() end
        self:mirar(objetivo.position)
        if self.recarga <= 0 then
            self.recarga = self.datos.cadencia
            local golpe = math.floor(self.dano * Mathf.random(0.85, 1.15) + 0.5)
            local nombre = self.datos.nombre
            if self.datos.distancia then nombre = nombre .. " (descarga)" end
            objetivo:getScript():RecibirDano(golpe, self.entity, nombre)
        end
    end
    if self.datos.jefe then self:habilidadJefe(dt) end
end

-- Pisoton: 1.5 s de aviso y dano a todos los que esten a menos de 6.5 m.
function Enemigo:habilidadJefe(dt)
    self.pisoton = self.pisoton - dt
    if self.pisoton <= 1.5 and not self.avisado then
        self.avisado = true
        if self.heroe and self.entity:distanceTo(self.heroe) < 30 then
            self.heroe:getScript():Aviso("¡El Rey Goblin va a dar un pisoton! ¡Alejate!", Vec3(1, 0.45, 0.3))
        end
    end
    if self.pisoton <= 0 then
        self.pisoton = 9
        self.avisado = false
        for _, c in ipairs(self:candidatos()) do
            if vivo(c) and self.entity:distanceTo(c) < 6.5 then
                c:getScript():RecibirDano(math.floor(self.dano * 1.6), self.entity, "Pisoton del Rey")
            end
        end
    end
end

function Enemigo:patrullar(dt)
    if self.entity.isMoving then return end
    self.espera = self.espera - dt
    if self.espera > 0 then return end
    self.espera = Mathf.random(3, 8)
    local p = Navigation.randomPoint(self.casa, self.datos.jefe and 3 or 7)
    if p then self.entity:moveTo(p) end
end

-- Lo llaman el heroe y los bots. Devuelve el dano hecho.
function Enemigo:Danar(cantidad, atacante, esHeroe)
    self:iniciar()
    if self.muerto then return 0 end
    cantidad = math.max(1, math.floor(cantidad))
    self.vida = self.vida - cantidad
    if esHeroe then self.golpeadoPorHeroe = true end
    if atacante then
        self.volviendo = false
        self.objetivo = atacante
    end
    if self.vida <= 0 then self:morir() end
    self:pintarBarra()
    return cantidad
end

function Enemigo:morir()
    self.vida = 0
    self.muerto = true
    self.objetivo = nil
    self.entity:stopMoving()
    if self.modelo then self.modelo.active = false end
    self.reaparecer = self.datos.reaparece
    local botin = {}
    for _, b in ipairs(self.datos.botin) do
        if Random.chance(b[2]) then botin[#botin + 1] = b[1] end
    end
    local oro = Random.int(self.datos.oro[1], self.datos.oro[2])
    if self.heroe then self.heroe:getScript():EnemigoMuerto(self, self.golpeadoPorHeroe, botin, oro) end
    self.golpeadoPorHeroe = false
    self:pintarBarra()
end

function Enemigo:revivir()
    self.muerto = false
    self.entity.position = self.casa
    self.vida = self.vidaMax
    self.objetivo = nil
    self.volviendo = false
    if self.modelo then self.modelo.active = true end
    self:pintarBarra()
end

-- La barra de vida sobre la cabeza (solo si esta herido).
function Enemigo:pintarBarra()
    if not self.barra then return end
    local frac = self.vida / self.vidaMax
    local mostrar = not self.muerto and frac < 0.999
    self.barra.active = mostrar
    if mostrar then
        local r = self.barra:find("Relleno")
        if r then
            r.scale = Vec3(math.max(0.02, frac), 1, 1)
            r.localPosition = Vec3((frac - 1) * 0.5, 0, 0)
        end
    end
end

return Enemigo
)lua";

constexpr const char* kBotScript = R"lua(-- Otro "jugador" del mundo (simulado): pasea entre las zonas, caza los
-- enemigos que encuentra, a veces habla por el chat y reaparece en el pueblo
-- si muere. Le da vida al mundo como en un MMO.
local Bot = {
    properties = {
        nombre = "Jugador",
        clase = "guerrero",   -- guerrero (cuerpo a cuerpo) o mago (a distancia)
        nivel = 3,
    }
}

local FRASES = {
    "¿Alguien para el Rey Goblin?", "lf grupo campamento goblin", "vendo pieles de lobo baratas",
    "¿donde se entrega la mision de los lobos?", "gg", "cuidado con el pisoton del Rey",
    "¿alguien tiene pociones de mana?", "este servidor va genial hoy", "jajaja casi me mata un lobo",
    "subi de nivel!", "¿donde esta el mercader?", "wts espada de acero", "brb", "alguien sabe de algun evento?",
}

function Bot:Start()
    self.casa = self.entity.position
    self.vidaMax = 80 + self.nivel * 25
    self.vida = self.vidaMax
    self.muerto = false
    self.heroe = Scene.find("Jugador")
    self.enemigos = Scene.findAllWithTag("Enemigo")
    self.modelo = self.entity:find("Modelo")
    self.zonas = {}
    for _, n in ipairs({ "Zona Pueblo", "Zona Bosque", "Zona Campamento" }) do
        local z = Scene.find(n)
        if z then self.zonas[#self.zonas + 1] = z end
    end
    self.espera = Mathf.random(1, 4)
    self.charla = Mathf.random(15, 50)
    self.recarga = 0
    self.repensar = 0
    self.alcance = self.clase == "mago" and 12 or 2.4
end

function Bot:EstaVivo() return self.muerto ~= true end

function Bot:Update(dt)
    if not self.vida then return end
    if self.muerto then
        self.reaparecer = self.reaparecer - dt
        if self.reaparecer <= 0 then
            self.muerto = false
            self.vida = self.vidaMax
            self.entity.position = self.casa
            if self.modelo then self.modelo.active = true end
        end
        return
    end
    self.recarga = math.max(0, self.recarga - dt)
    self.repensar = self.repensar - dt
    self.charla = self.charla - dt
    if self.charla <= 0 then
        self.charla = Mathf.random(25, 70)
        if self.heroe then self.heroe:getScript():Chat("[General] " .. self.nombre .. ": " .. Random.pick(FRASES)) end
    end

    -- Poca vida: se retira a descansar.
    if self.vida < self.vidaMax * 0.3 and not self.descansando then
        self.descansando = true
        self.objetivo = nil
        self.entity:moveTo(self.casa)
    end
    if self.descansando then
        self.vida = math.min(self.vidaMax, self.vida + self.vidaMax * 0.08 * dt)
        if self.vida >= self.vidaMax * 0.95 then self.descansando = false end
        return
    end

    if self.objetivo then
        local s = self.objetivo:getScript()
        if not s or not s:EstaVivo() or self.entity:distanceTo(self.objetivo) > 35 then self.objetivo = nil end
    end
    if not self.objetivo and self.repensar <= 0 then
        self.repensar = 0.5
        local mejor, dist = nil, 16
        for _, e in ipairs(self.enemigos) do
            local s = e:getScript()
            if s and s:EstaVivo() and not s:EsJefe() then
                local d = self.entity:distanceTo(e)
                if d < dist then mejor, dist = e, d end
            end
        end
        self.objetivo = mejor
    end

    if self.objetivo then
        local d = self.entity:distanceTo(self.objetivo)
        if d > self.alcance then
            if self.repensar <= 0 then
                self.repensar = 0.3
                self.entity:moveTo(self.objetivo.position)
            end
        else
            if self.entity.isMoving then self.entity:stopMoving() end
            if self.recarga <= 0 then
                self.recarga = self.clase == "mago" and 2.2 or 1.7
                local golpe = (self.clase == "mago" and 14 or 10) + self.nivel * 2
                self.objetivo:getScript():Danar(golpe * Mathf.random(0.85, 1.15), self.entity, false)
            end
        end
        return
    end

    -- Pasear: a un punto al azar de alguna zona.
    if not self.entity.isMoving then
        self.espera = self.espera - dt
        if self.espera <= 0 and #self.zonas > 0 then
            self.espera = Mathf.random(4, 12)
            local z = Random.pick(self.zonas)
            local p = Navigation.randomPoint(z.position, 20)
            if p then self.entity:moveTo(p) end
        end
    end
end

function Bot:RecibirDano(cantidad, fuente, nombre)
    if self.muerto or not self.vida then return end
    self.vida = self.vida - cantidad
    if fuente and not self.objetivo and not self.descansando then self.objetivo = fuente end
    if self.vida <= 0 then
        self.muerto = true
        self.objetivo = nil
        self.descansando = false
        self.entity:stopMoving()
        if self.modelo then self.modelo.active = false end
        self.reaparecer = 12
        if self.heroe then self.heroe:getScript():Chat("[Sistema] " .. self.nombre .. " ha muerto (" .. (nombre or "?") .. ").") end
    end
end

return Bot
)lua";

constexpr const char* kNpcScript = R"lua(-- Personaje del pueblo: mira al jugador cuando esta cerca y muestra su marca
-- (amarilla = mision nueva, azul = mision para entregar). El heroe habla con
-- el con E (Heroe.lua lleva los dialogos, las misiones y la tienda).
local NPC = {
    properties = {
        nombre = "Aldeano",
        saludo = "Buenos dias, viajero.",
        comerciante = false,
    }
}

function NPC:Start()
    self.jugador = Scene.find("Jugador")
    self.modelo = self.entity:find("Modelo")
    self.marca = self.entity:find("Marca")
    self.estadoMarca = "?"
    self.fase = self.entity.position.x
end

function NPC:Nombre() return self.nombre end
function NPC:Saludo() return self.saludo end
function NPC:EsComerciante() return self.comerciante == true end

-- nil, "mision" o "entregar"
function NPC:Marca(tipo)
    if not self.marca or tipo == self.estadoMarca then return end
    self.estadoMarca = tipo
    self.marca.active = tipo ~= nil
    if tipo == "mision" then
        self.marca:setMaterial(0, "Materials/Marca mision")
    elseif tipo == "entregar" then
        self.marca:setMaterial(0, "Materials/Marca entregar")
    end
end

function NPC:Update(dt)
    if self.marca and self.marca.active then
        self.marca:rotate(Vec3(0, 90 * dt, 0))
        self.marca.localPosition = Vec3(0, 1.75 + math.sin(Time.time * 2.2 + (self.fase or 0)) * 0.12, 0)
    end
    if self.jugador and self.modelo and self.entity:distanceTo(self.jugador) < 8 then
        local p = self.jugador.position
        local yo = self.modelo.position
        self.modelo:lookAt(Vec3(p.x, yo.y, p.z))
    end
end

return NPC
)lua";

}  // namespace cramion::editor::mmo

#endif  // CRAMION_EDITOR_TEMPLATE_MMO_SCRIPTS_H

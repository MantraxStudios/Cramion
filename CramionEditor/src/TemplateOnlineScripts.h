#ifndef CRAMION_EDITOR_TEMPLATE_ONLINE_SCRIPTS_H
#define CRAMION_EDITOR_TEMPLATE_ONLINE_SCRIPTS_H

// Scripts de la plantilla Online (todo el juego en Lua): menu para crear o
// unirse a una partida, jugadores sincronizados, chat, marcador, monedas,
// balon y cajas que se patean (fisica del servidor), porterias y eventos.

namespace cramion::editor::online {

// El juego: menu, partida, chat, marcador y lo que decide el servidor.
constexpr const char* kNetGameScript = R"lua(-- Red.lua: el juego online. Todo pasa por aqui:
--   Menu        nombre, IP, "Crear partida" (eres el servidor y juegas) o "Unirse".
--   Jugadores   cada uno tiene su personaje (Prefabs/Jugador) creado por el
--               servidor con Network.spawn: lo mueve su dueno y los demas lo
--               ven sincronizado.
--   Chat        el cliente lo manda al servidor ("decir"), el servidor le pone
--               el nombre y lo reparte a todos ("chat").
--   Marcador    puntos y ping de cada jugador; gana la ronda quien llega antes.
--   Objetos     monedas, un balon y cajas del servidor (su fisica la simula el
--               servidor); F patea lo que tengas cerca. Goles en las porterias.
--   Eventos     cada cierto tiempo: lluvia de monedas o monedas dobles.
-- El servidor decide los puntos (un cliente no puede darse puntos a si mismo).
local Red = {
    properties = {
        puerto = 7777,
        maxJugadores = 8,
        puntosParaGanar = 30,
        monedas = 8,            -- monedas a la vez en la arena
        segundosEvento = 45,
    }
}

local COLORES = { "Azul", "Rojo", "Verde", "Amarillo", "Morado", "Naranja", "Cian", "Rosa" }
local LINEAS_CHAT = 9
local MITAD_ARENA = 24      -- la arena va de -24 a 24 (m)
local PORTERIA_X = 26       -- el balon cruza esta linea = gol
local PORTERIA_Z = 4        -- ancho de la porteria (a cada lado)

local function colorDe(id) return COLORES[((id - 1) % #COLORES) + 1] end

-- Texto de un jugador: sin saltos de linea y con un largo maximo.
local function limpiar(texto, largo)
    texto = tostring(texto or ""):gsub("[\r\n\t]", " ")
    texto = texto:gsub("^%s+", ""):gsub("%s+$", "")
    if #texto > largo then texto = texto:sub(1, largo) end
    return texto
end

local function puntoAlAzar(margen)
    local m = MITAD_ARENA - (margen or 3)
    return Vec3(math.random() * 2 * m - m, 1.2, math.random() * 2 * m - m)
end

function Red:Start()
    self.menu = Scene.find("UI_Menu")
    self.estado = Scene.find("UI_Estado")
    self.campoNombre = Scene.find("UI_Nombre")
    self.campoIP = Scene.find("UI_IP")
    self.juego = Scene.find("UI_Juego")
    self.chat = Scene.find("UI_Chat")
    self.campoChat = Scene.find("UI_EscribirChat")
    self.marcador = Scene.find("UI_Marcador")
    self.aviso = Scene.find("UI_Aviso")
    self.info = Scene.find("UI_Info")
    self.camara = Scene.find("Main Camera")
    self.inicioCamara = self.camara and self.camara.position
    self.lineas = {}
    self.tabla = {}
    self.avisoTiempo = 0
    self.infoTiempo = 0
    self.enPartida = false
    self.campoNombre.text = Prefs.getString("online_nombre", "Jugador" .. math.random(10, 99))
    self.campoIP.text = Prefs.getString("online_ip", "127.0.0.1")
    self:mostrarMenu(true, "Crea una partida o unete a la de un amigo (su IP; en el mismo PC, 127.0.0.1).")

    -- Eventos de la red (valen para toda la partida).
    Network.onConnected(function(id) self:alEntrar(id) end)
    Network.onDisconnected(function(motivo) self:alSalir(motivo) end)
    Network.onPlayerJoined(function(id) self:jugadorEntra(id) end)
    Network.onPlayerLeft(function(id) self:jugadorSale(id) end)
    -- Mensajes: "hola", "decir" y "patear" van al servidor; "chat", "tabla" y "aviso" a todos.
    Network.on("hola", function(d, de) self:hola(de, d) end)
    Network.on("decir", function(d, de) self:difundirChat(de, d.texto) end)
    Network.on("patear", function(d, de) self:patear(de) end)
    Network.on("chat", function(d) self:anadirLinea(d.texto) end)
    Network.on("tabla", function(d) self.tabla = d; self:pintarMarcador() end)
    Network.on("aviso", function(d) self:mostrarAviso(d.texto) end)
end

-- --- Menu -------------------------------------------------------------------

function Red:mostrarMenu(visible, texto)
    self.menu.active = visible
    self.juego.active = not visible
    if texto then self.estado.text = texto end
    Input.lockCursor(false)
end

function Red:nombre() return limpiar(self.campoNombre.text, 16) end

function Red:OnCrear()
    if Network.isActive() then return end
    Prefs.setString("online_nombre", self:nombre())
    local ok, err = Network.host(self.puerto, self.maxJugadores)
    if not ok then
        self.estado.text = "No se pudo crear la partida: " .. tostring(err)
        return
    end
    -- El servidor tambien juega: es el jugador 1.
    self.jugadores = {}
    self.monedasVivas = {}
    self.objetos = {}
    self.eventoTiempo = self.segundosEvento
    self.multiplicador = 1
    self.multiplicadorTiempo = 0
    self.tablaTiempo = 0
    self.jugadores[Network.SERVER] = { nombre = self:nombre(), puntos = 0 }
    self:crearAvatar(Network.SERVER)
    self:crearObjetos()
    self:empezar()
    self:sistema(self:nombre() .. " ha creado la partida (puerto " .. self.puerto .. ").")
    self:enviarTabla()
end

function Red:OnUnirse()
    if Network.isActive() then return end
    local ip = limpiar(self.campoIP.text, 64)
    if ip == "" then ip = "127.0.0.1" end
    Prefs.setString("online_nombre", self:nombre())
    Prefs.setString("online_ip", ip)
    local ok, err = Network.connect(ip, self.puerto)
    self.estado.text = ok and ("Conectando con " .. ip .. "...") or ("No se pudo conectar: " .. tostring(err))
end

function Red:OnSalir()
    Network.disconnect()
    self:alSalir("Has salido de la partida.")
end

-- Cliente: ya dentro. Se presenta al servidor con su nombre.
function Red:alEntrar(id)
    self:empezar()
    Network.send("hola", { nombre = self:nombre() }, "server")
    self:anadirLinea("Conectado como jugador " .. id .. " (" .. colorDe(id) .. ").")
end

function Red:empezar()
    self.enPartida = true
    self.lineas = {}
    self.chat.text = ""
    self:mostrarMenu(false)
end

function Red:alSalir(motivo)
    self.enPartida = false
    self.tabla = {}
    self.jugadores = nil
    if self.camara then
        local cam = self.camara:getScript()
        if cam then cam.target = nil end
        if self.inicioCamara then self.camara.position = self.inicioCamara end
    end
    self:mostrarMenu(true, motivo)
end

-- --- Servidor: jugadores -------------------------------------------------------

function Red:crearAvatar(id)
    local avatar = Network.spawn("Prefabs/Jugador", puntoAlAzar(6), id)
    if avatar then self.jugadores[id].avatar = avatar end
end

function Red:jugadorEntra(id)
    if not Network.isServer() or not self.jugadores then return end
    self.jugadores[id] = { nombre = "Jugador " .. id, puntos = 0 }
    self:crearAvatar(id)
    self:enviarTabla()
end

function Red:hola(de, datos)
    if not Network.isServer() or not self.jugadores or not self.jugadores[de] then return end
    local nombre = limpiar(datos and datos.nombre, 16)
    if nombre ~= "" then self.jugadores[de].nombre = nombre end
    self:sistema(self.jugadores[de].nombre .. " se ha unido (" .. colorDe(de) .. ").")
    self:enviarTabla()
end

function Red:jugadorSale(id)
    if not Network.isServer() or not self.jugadores or not self.jugadores[id] then return end
    -- Su personaje desaparece solo (sus objetos de red se borran al irse).
    self:sistema(self.jugadores[id].nombre .. " ha salido.")
    self.jugadores[id] = nil
    self:enviarTabla()
end

-- --- Chat ------------------------------------------------------------------------

-- El campo de texto del chat (Enter).
function Red:OnChat(texto)
    self.campoChat.text = ""
    texto = limpiar(texto, 120)
    if texto == "" or not self.enPartida then return end
    if Network.isServer() then
        self:difundirChat(Network.SERVER, texto)
    else
        Network.send("decir", { texto = texto }, "server")
    end
end

-- Servidor: pone el nombre y lo manda a todos.
function Red:difundirChat(id, texto)
    if not Network.isServer() or not self.jugadores or not self.jugadores[id] then return end
    texto = limpiar(texto, 120)
    if texto == "" then return end
    local linea = self.jugadores[id].nombre .. ": " .. texto
    Network.send("chat", { texto = linea })
    self:anadirLinea(linea)
end

-- Servidor: mensaje del sistema en el chat de todos.
function Red:sistema(texto)
    Network.send("chat", { texto = "* " .. texto })
    self:anadirLinea("* " .. texto)
end

function Red:anadirLinea(texto)
    table.insert(self.lineas, texto)
    while #self.lineas > LINEAS_CHAT do table.remove(self.lineas, 1) end
    self.chat.text = table.concat(self.lineas, "\n")
end

-- --- Avisos y marcador --------------------------------------------------------------

function Red:anunciar(texto)
    Network.send("aviso", { texto = texto })
    self:mostrarAviso(texto)
end

function Red:mostrarAviso(texto)
    self.aviso.text = texto
    self.aviso.alpha = 1
    self.avisoTiempo = 4
end

function Red:enviarTabla()
    if not self.jugadores then return end
    local lista = {}
    for id, j in pairs(self.jugadores) do
        lista[#lista + 1] = { id = id, nombre = j.nombre, puntos = j.puntos, ping = Network.ping(id) }
    end
    table.sort(lista, function(a, b) return a.puntos > b.puntos end)
    Network.send("tabla", lista)
    self.tabla = lista
    self:pintarMarcador()
end

function Red:pintarMarcador()
    local lineas = { "JUGADORES   (a " .. self.puntosParaGanar .. " puntos)" }
    for _, j in ipairs(self.tabla) do
        local yo = (j.id == Network.myId()) and "  < tu" or ""
        local ping = (j.id == Network.SERVER) and "servidor" or (tostring(j.ping) .. " ms")
        lineas[#lineas + 1] = string.format("%s  %s  %d pts  (%s)%s", colorDe(j.id), j.nombre, j.puntos, ping, yo)
    end
    self.marcador.text = table.concat(lineas, "\n")
end

-- --- Servidor: objetos, puntos y eventos -----------------------------------------------

function Red:crearObjetos()
    -- Balon y cajas: los simula el servidor y los ven todos.
    local balon = Network.spawn("Prefabs/Balon", Vec3(0, 2, 0))
    if balon then self.objetos[#self.objetos + 1] = { entidad = balon, fuerza = 9, balon = true } end
    for i = 1, 4 do
        local caja = Network.spawn("Prefabs/Caja", Vec3(-12 + i * 5, 1, -10))
        if caja then self.objetos[#self.objetos + 1] = { entidad = caja, fuerza = 45 } end
    end
    for i = 1, self.monedas do self:crearMoneda() end
end

function Red:crearMoneda()
    local moneda = Network.spawn("Prefabs/Moneda", puntoAlAzar(3))
    if moneda then self.monedasVivas[#self.monedasVivas + 1] = moneda end
end

function Red:sumar(id, puntos)
    local j = self.jugadores[id]
    if not j then return end
    j.puntos = j.puntos + puntos * self.multiplicador
    if j.puntos >= self.puntosParaGanar then
        self:anunciar(j.nombre .. " gana la ronda!")
        self:sistema(j.nombre .. " gana la ronda con " .. j.puntos .. " puntos. Nueva ronda!")
        for _, otro in pairs(self.jugadores) do otro.puntos = 0 end
    end
    self:enviarTabla()
end

-- F: patea lo que el jugador tenga cerca (el servidor aplica el golpe).
function Red:patear(id)
    if not Network.isServer() or not self.jugadores or not self.jugadores[id] then return end
    local avatar = self.jugadores[id].avatar
    if not avatar then return end
    for _, o in ipairs(self.objetos) do
        if o.entidad:distanceTo(avatar) < 2.4 then
            local dir = o.entidad.position - avatar.position
            dir.y = 0
            dir = dir:normalized()
            dir.y = 0.45
            o.entidad:addForce(dir * o.fuerza, "impulse")
            o.ultimo = id
        end
    end
end

function Red:actualizarServidor(dt)
    -- Monedas: se recogen al pasar cerca (lo comprueba el servidor).
    for i = #self.monedasVivas, 1, -1 do
        local moneda = self.monedasVivas[i]
        for id, j in pairs(self.jugadores) do
            if j.avatar and moneda:distanceTo(j.avatar) < 1.4 then
                Network.destroy(moneda)
                table.remove(self.monedasVivas, i)
                self:sumar(id, 1)
                break
            end
        end
    end
    while #self.monedasVivas < self.monedas do self:crearMoneda() end

    -- Goles: el balon cruza una porteria.
    for _, o in ipairs(self.objetos) do
        if o.balon then
            local p = o.entidad.position
            if math.abs(p.x) > PORTERIA_X and math.abs(p.z) < PORTERIA_Z then
                local quien = o.ultimo and self.jugadores[o.ultimo]
                if quien then
                    self:anunciar("GOL de " .. quien.nombre .. "! +5")
                    self:sumar(o.ultimo, 5)
                else
                    self:anunciar("Gol!")
                end
                o.entidad.position = Vec3(0, 3, 0)
                o.entidad.velocity = Vec3.zero
                o.ultimo = nil
            elseif p.y < -10 then
                o.entidad.position = Vec3(0, 3, 0)
                o.entidad.velocity = Vec3.zero
            end
        elseif o.entidad.position.y < -10 then
            o.entidad.position = puntoAlAzar(5) + Vec3(0, 2, 0)
            o.entidad.velocity = Vec3.zero
        end
    end

    -- Eventos cada cierto tiempo.
    self.eventoTiempo = self.eventoTiempo - dt
    if self.eventoTiempo <= 0 then
        self.eventoTiempo = self.segundosEvento
        if math.random() < 0.5 then
            self:anunciar("Lluvia de monedas!")
            for i = 1, 12 do self:crearMoneda() end
        else
            self:anunciar("Monedas dobles durante 20 segundos!")
            self.multiplicador = 2
            self.multiplicadorTiempo = 20
        end
    end
    if self.multiplicadorTiempo > 0 then
        self.multiplicadorTiempo = self.multiplicadorTiempo - dt
        if self.multiplicadorTiempo <= 0 then
            self.multiplicador = 1
            self:anunciar("Se acabaron las monedas dobles")
        end
    end

    -- El ping cambia: el marcador se refresca cada 2 segundos.
    self.tablaTiempo = self.tablaTiempo - dt
    if self.tablaTiempo <= 0 then
        self.tablaTiempo = 2
        self:enviarTabla()
    end
end

function Red:Update(dt)
    if self.avisoTiempo > 0 then
        self.avisoTiempo = self.avisoTiempo - dt
        self.aviso.alpha = Mathf.clamp01(self.avisoTiempo)
    end
    if not self.enPartida then return end
    if Input.getKeyDown("f") then
        if Network.isServer() then self:patear(Network.SERVER) else Network.send("patear", {}, "server") end
    end
    if Network.isServer() and self.jugadores then self:actualizarServidor(dt) end

    self.infoTiempo = self.infoTiempo - dt
    if self.infoTiempo <= 0 then
        self.infoTiempo = 0.5
        local estado
        if Network.isServer() then
            estado = "Servidor (puerto " .. self.puerto .. ") - " .. Network.playerCount() .. " jugador(es)"
        else
            estado = "Jugador " .. Network.myId() .. " - ping " .. Network.ping() .. " ms"
        end
        local s = Network.stats()
        self.info.text = estado .. string.format("  -  %d objetos  -  %.1f KB enviados", s.objects, s.sent / 1024) ..
            "\nWASD mover - Shift correr - Espacio saltar - F patear - clic en el chat para escribir"
    end
end

return Red
)lua";

// El personaje de cada jugador (Prefabs/Jugador).
constexpr const char* kNetPlayerScript = R"lua(-- JugadorRed.lua: el personaje de un jugador. Existe en todos los ordenadores
-- (lo crea el servidor con Network.spawn), pero solo lo mueve su dueno:
-- self.entity:isMine(). Los demas lo ven moverse con la posicion que llega
-- por la red (el motor la suaviza y pone su Rigidbody cinematico).
local JugadorRed = {
    properties = {
        velocidad = 6.0,
        correr = 1.7,
        salto = 6.5,
        aceleracion = 12.0,
    }
}

local COLORES = { "Azul", "Rojo", "Verde", "Amarillo", "Morado", "Naranja", "Cian", "Rosa" }

function JugadorRed:Start()
    self.modelo = self.entity:find("Modelo")
    self.ultima = self.entity.position
    -- Color por jugador (el mismo en todos: sale de su id).
    local n = ((self.entity.netOwner - 1) % #COLORES) + 1
    local cuerpo = self.entity:find("Cuerpo")
    if cuerpo then cuerpo:setMaterial(0, "Materials/Jugador " .. COLORES[n]) end
    if self.entity:isMine() then
        self.inicio = self.entity.position
        self.camara = Scene.find("Main Camera")
        local cam = self.camara and self.camara:getScript()
        if cam then cam.target = self.entity end
    end
end

function JugadorRed:enSuelo()
    return Physics.raycast(self.entity.position + Vec3(0, -1.02, 0), Vec3.down, 0.25) ~= nil
end

function JugadorRed:Update(dt)
    if not self.entity:isMine() then
        -- De otro jugador: mirar hacia donde se mueve.
        local p = self.entity.position
        local d = p - self.ultima
        d.y = 0
        if self.modelo and d:length() > 0.01 then self.modelo:lookAt(self.modelo.position + d) end
        self.ultima = p
        return
    end
    local adelante, derecha = Vec3(0, 0, -1), Vec3(1, 0, 0)
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
    local v = self.entity.velocity
    local t = Mathf.clamp01(self.aceleracion * dt)
    v.x = Mathf.lerp(v.x, direccion.x * rapidez, t)
    v.z = Mathf.lerp(v.z, direccion.z * rapidez, t)
    if Input.getKeyDown("space") and self:enSuelo() then v.y = self.salto end
    self.entity.velocity = v
    if self.modelo and direccion:length() > 0.1 then self.modelo:lookAt(self.modelo.position + direccion) end
    if self.entity.position.y < -20 then
        self.entity.position = self.inicio
        self.entity.velocity = Vec3.zero
    end
end

return JugadorRed
)lua";

// Moneda (Prefabs/Moneda): gira en cada ordenador; la recoge el servidor.
constexpr const char* kNetCoinScript = R"lua(-- MonedaRed.lua: solo se ve girar (cada uno la gira en su pantalla). Quien la
-- recoge lo decide el servidor en Red.lua (distancia a cada jugador).
local Moneda = {}

function Moneda:Start()
    self.base = self.entity.position
    self.fase = math.random() * 6.28
end

function Moneda:Update(dt)
    self.fase = self.fase + dt * 2.5
    self.entity:rotate(Vec3(0, 160 * dt, 0))
    self.entity.position = self.base + Vec3(0, math.sin(self.fase) * 0.15, 0)
end

return Moneda
)lua";

}  // namespace cramion::editor::online

#endif  // CRAMION_EDITOR_TEMPLATE_ONLINE_SCRIPTS_H

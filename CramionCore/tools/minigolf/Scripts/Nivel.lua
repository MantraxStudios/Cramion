-- El hoyo en juego: golpes, par, marcador (HUD) y paso al siguiente.
local Nivel = {
    properties = {
        numero = 1,
        nombre = "Primer golpe",
        par = 2,
        siguiente = "Nivel2",
    }
}

local RESULTADOS = {
    [-3] = "¡ALBATROS!", [-2] = "¡EAGLE!", [-1] = "¡BIRDIE!", [0] = "PAR", [1] = "BOGEY", [2] = "DOBLE BOGEY",
}

function Nivel:Start()
    -- Las propiedades numericas llegan como decimales (1.0): a entero.
    self.numero = math.floor(self.numero + 0.5)
    self.par = math.floor(self.par + 0.5)
    self.golpes = 0
    self.terminado = false
    self.espera = 0
    self.mensajeTiempo = 0
    self.titulo = Scene.find("HUD_Titulo")
    self.marcador = Scene.find("HUD_Golpes")
    self.total = Scene.find("HUD_Total")
    self.mensaje = Scene.find("HUD_Mensaje")
    self.barra = Scene.find("HUD_Potencia")
    self.etiquetaBarra = Scene.find("HUD_PotenciaTexto")
    if self.titulo then self.titulo.text = "HOYO " .. self.numero .. "  ·  " .. self.nombre end
    self:mostrar("HOYO " .. self.numero, 2.0)
    self:actualizar()
end

function Nivel:totalAnterior()
    local suma = 0
    for i = 1, self.numero - 1 do
        suma = suma + math.max(0, Prefs.getInt("golpes_" .. i, 0))
    end
    return suma
end

function Nivel:actualizar()
    if self.marcador then self.marcador.text = "Par " .. self.par .. "     Golpes " .. self.golpes end
    if self.total then self.total.text = "Total " .. (self:totalAnterior() + self.golpes) end
end

function Nivel:mostrar(texto, segundos)
    if not self.mensaje then return end
    self.mensaje.text = texto
    self.mensaje.active = true
    self.mensajeTiempo = segundos
end

function Nivel:MostrarPotencia(valor)
    if self.barra then
        self.barra.value = valor
        self.barra.active = valor > 0
    end
    if self.etiquetaBarra then self.etiquetaBarra.active = valor > 0 end
end

function Nivel:OnGolpe()
    self.golpes = self.golpes + 1
    self:actualizar()
end

function Nivel:Penalizar()
    self.golpes = self.golpes + 1
    self:actualizar()
    self:mostrar("FUERA  (+1)", 1.5)
end

function Nivel:OnHoyo()
    if self.terminado then return end
    self.terminado = true
    local diferencia = self.golpes - self.par
    local texto
    if self.golpes == 1 then
        texto = "¡HOYO EN UNO!"
    else
        texto = RESULTADOS[diferencia] or ("+" .. diferencia)
    end
    self:mostrar(texto .. "\n" .. self.golpes .. (self.golpes == 1 and " golpe" or " golpes"), 3.0)
    Prefs.setInt("golpes_" .. self.numero, self.golpes)
    self:actualizar()
    self.espera = 3.0
end

function Nivel:Update(dt)
    if self.mensajeTiempo > 0 then
        self.mensajeTiempo = self.mensajeTiempo - dt
        if self.mensajeTiempo <= 0 and self.mensaje then self.mensaje.active = false end
    end
    if self.terminado then
        self.espera = self.espera - dt
        if self.espera <= 0 then
            self.terminado = false
            Scene.load(self.siguiente)
        end
    end
end

return Nivel

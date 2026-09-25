-- Tarjeta final: golpes por hoyo, total contra el par y record.
local Resultados = {
    properties = {
        pares = "2,3,3,3,4",
        nombres = "Primer golpe,La curva,El tunel,El castillo,El molino",
    }
}

local function separar(texto)
    local lista = {}
    for parte in string.gmatch(texto, "([^,]+)") do lista[#lista + 1] = parte end
    return lista
end

function Resultados:Start()
    local pares = separar(self.pares)
    local nombres = separar(self.nombres)
    local lineas = {}
    local total, totalPar = 0, 0
    for i = 1, #pares do
        local golpes = Prefs.getInt("golpes_" .. i, 0)
        local par = tonumber(pares[i]) or 3
        total = total + golpes
        totalPar = totalPar + par
        local diferencia = golpes - par
        local signo = diferencia > 0 and ("+" .. diferencia) or (diferencia == 0 and "E" or tostring(diferencia))
        lineas[#lineas + 1] = string.format("%d.  %-14s  par %d   %2d golpes   %s", i, nombres[i] or "", par, golpes, signo)
    end
    local tarjeta = Scene.find("Res_Tarjeta")
    if tarjeta then tarjeta.text = table.concat(lineas, "\n") end

    local diferencia = total - totalPar
    local resumen = Scene.find("Res_Total")
    if resumen then
        local signo = diferencia > 0 and ("+" .. diferencia) or (diferencia == 0 and "al par" or tostring(diferencia))
        resumen.text = "TOTAL  " .. total .. " golpes  (" .. signo .. ")"
    end
    local record = Prefs.getInt("record", 0)
    local aviso = Scene.find("Res_Record")
    if total > 0 and (record == 0 or total < record) then
        Prefs.setInt("record", total)
        if aviso then aviso.text = "¡NUEVO RECORD!" end
    elseif aviso then
        aviso.text = "Record: " .. record .. " golpes"
    end
    Audio.playOneShot("Audio/fanfarria.wav")
end

function Resultados:OnOtraVez(boton)
    Audio.playOneShot("Audio/click.wav")
    for i = 1, 5 do Prefs.deleteKey("golpes_" .. i) end
    Scene.load("Nivel1")
end

function Resultados:OnMenu(boton)
    Audio.playOneShot("Audio/click.wav")
    Scene.load("Menu")
end

return Resultados

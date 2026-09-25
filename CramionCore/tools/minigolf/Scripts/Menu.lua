-- Menu de inicio: jugar, salir y el record guardado.
local Menu = { properties = {} }

function Menu:Start()
    local record = Prefs.getInt("record", 0)
    local texto = Scene.find("Menu_Record")
    if texto then
        texto.text = record > 0 and ("Mejor partida: " .. record .. " golpes") or "¡Juega tu primera partida!"
    end
end

function Menu:OnJugar(boton)
    Audio.playOneShot("Audio/click.wav")
    for i = 1, 5 do Prefs.deleteKey("golpes_" .. i) end
    Scene.load("Nivel1")
end

function Menu:OnSalir(boton)
    Audio.playOneShot("Audio/click.wav")
    Game.quit()
end

function Menu:Update(dt)
    if Input.getKeyDown("enter") or Input.getKeyDown("space") then self:OnJugar(nil) end
end

return Menu

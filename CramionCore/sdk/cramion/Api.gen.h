// GENERADO por cramion_sdkgen desde la API de Lua del motor: no lo edites.
// Cada funcion de Lua (Tabla.funcion) es una funcion de C++ con los mismos
// argumentos (Value: numeros, texto, Vec3, entidades, listas, objetos,
// funciones...) que devuelve un Value. Ver la referencia de Lua en el manual.
#pragma once

namespace cramion {

namespace Accessibility {
/// Accessibility.colorblindName(2)
/// nombre del tipo de daltonismo
template <typename T0 = Value>
inline Value colorblindName(const T0& valor = {}) {
    return detail::lua("Accessibility.colorblindName", Values{Value(valor)});
}
/// Accessibility.get()
/// {colorblind, colorblindStrength, colorblindCorrect, textScale, subtitleScale, ...}
template <typename... Mas>
inline Value get(Mas&&... mas) {
    return detail::lua("Accessibility.get", Values{Value(std::forward<Mas>(mas))...});
}
/// Accessibility.load()
/// las vuelve a leer
template <typename... Mas>
inline Value load(Mas&&... mas) {
    return detail::lua("Accessibility.load", Values{Value(std::forward<Mas>(mas))...});
}
/// Accessibility.reset()
/// por defecto
template <typename... Mas>
inline Value reset(Mas&&... mas) {
    return detail::lua("Accessibility.reset", Values{Value(std::forward<Mas>(mas))...});
}
/// Accessibility.save()
/// las guarda (por jugador)
template <typename... Mas>
inline Value save(Mas&&... mas) {
    return detail::lua("Accessibility.save", Values{Value(std::forward<Mas>(mas))...});
}
/// Accessibility.set({colorblind = 2, textScale = 1.3, reduceMotion = true})
/// cambia opciones (las que vengan)
template <typename T0 = Value>
inline Value set(const T0& tabla = {}) {
    return detail::lua("Accessibility.set", Values{Value(tabla)});
}
}  // namespace Accessibility

namespace Assert {
/// Assert.approx(real, esperado, tolerancia, mensaje)
/// numeros o Vec3
template <typename T0 = Value, typename T1 = Value, typename T2 = Value, typename T3 = Value>
inline Value approx(const T0& real = {}, const T1& esperado = {}, const T2& tolerancia = {}, const T3& mensaje = {}) {
    return detail::lua("Assert.approx", Values{Value(real), Value(esperado), Value(tolerancia), Value(mensaje)});
}
/// Assert.atLeast(a, b, mensaje)
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value atLeast(const T0& a = {}, const T1& b = {}, const T2& mensaje = {}) {
    return detail::lua("Assert.atLeast", Values{Value(a), Value(b), Value(mensaje)});
}
/// Assert.atMost(a, b, mensaje)
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value atMost(const T0& a = {}, const T1& b = {}, const T2& mensaje = {}) {
    return detail::lua("Assert.atMost", Values{Value(a), Value(b), Value(mensaje)});
}
/// Assert.contains(lista o texto, valor, mensaje)
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value contains(const T0& listaOTexto = {}, const T1& valor = {}, const T2& mensaje = {}) {
    return detail::lua("Assert.contains", Values{Value(listaOTexto), Value(valor), Value(mensaje)});
}
/// Assert.equal(real, esperado, mensaje)
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value equal(const T0& real = {}, const T1& esperado = {}, const T2& mensaje = {}) {
    return detail::lua("Assert.equal", Values{Value(real), Value(esperado), Value(mensaje)});
}
/// Assert.fail(mensaje)
/// falla siempre
template <typename T0 = Value>
inline Value fail(const T0& mensaje = {}) {
    return detail::lua("Assert.fail", Values{Value(mensaje)});
}
/// Assert.greater(a, b, mensaje)
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value greater(const T0& a = {}, const T1& b = {}, const T2& mensaje = {}) {
    return detail::lua("Assert.greater", Values{Value(a), Value(b), Value(mensaje)});
}
/// Assert.isFalse(valor, mensaje)
template <typename T0 = Value, typename T1 = Value>
inline Value isFalse(const T0& valor = {}, const T1& mensaje = {}) {
    return detail::lua("Assert.isFalse", Values{Value(valor), Value(mensaje)});
}
/// Assert.isNil(valor, mensaje)
template <typename T0 = Value, typename T1 = Value>
inline Value isNil(const T0& valor = {}, const T1& mensaje = {}) {
    return detail::lua("Assert.isNil", Values{Value(valor), Value(mensaje)});
}
/// Assert.isTrue(valor, mensaje)
template <typename T0 = Value, typename T1 = Value>
inline Value isTrue(const T0& valor = {}, const T1& mensaje = {}) {
    return detail::lua("Assert.isTrue", Values{Value(valor), Value(mensaje)});
}
/// Assert.less(a, b, mensaje)
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value less(const T0& a = {}, const T1& b = {}, const T2& mensaje = {}) {
    return detail::lua("Assert.less", Values{Value(a), Value(b), Value(mensaje)});
}
/// Assert.noError(funcion, mensaje)
/// no tiene que fallar
template <typename T0 = Value, typename T1 = Value>
inline Value noError(const T0& funcion = {}, const T1& mensaje = {}) {
    return detail::lua("Assert.noError", Values{Value(funcion), Value(mensaje)});
}
/// Assert.notEqual(a, b, mensaje)
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value notEqual(const T0& a = {}, const T1& b = {}, const T2& mensaje = {}) {
    return detail::lua("Assert.notEqual", Values{Value(a), Value(b), Value(mensaje)});
}
/// Assert.notNil(valor, mensaje)
template <typename T0 = Value, typename T1 = Value>
inline Value notNil(const T0& valor = {}, const T1& mensaje = {}) {
    return detail::lua("Assert.notNil", Values{Value(valor), Value(mensaje)});
}
/// Assert.throws(funcion, mensaje)
/// la funcion tiene que fallar
template <typename T0 = Value, typename T1 = Value>
inline Value throws(const T0& funcion = {}, const T1& mensaje = {}) {
    return detail::lua("Assert.throws", Values{Value(funcion), Value(mensaje)});
}
}  // namespace Assert

namespace Audio {
/// Audio.occlusion()
/// esta la oclusion activa?
template <typename... Mas>
inline Value occlusion(Mas&&... mas) {
    return detail::lua("Audio.occlusion", Values{Value(std::forward<Mas>(mas))...});
}
/// Audio.playOneShot("Audio/golpe.wav", posicion, volumen)
/// sonido suelto (sin posicion = 2D)
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value playOneShot(const T0& texto = {}, const T1& posicion = {}, const T2& volumen = {}) {
    return detail::lua("Audio.playOneShot", Values{Value(texto), Value(posicion), Value(volumen)});
}
/// Audio.reverbLevel()
/// reverberacion que se oye ahora (zonas)
template <typename... Mas>
inline Value reverbLevel(Mas&&... mas) {
    return detail::lua("Audio.reverbLevel", Values{Value(std::forward<Mas>(mas))...});
}
/// Audio.setLowPass(true, 800)
/// todo apagado (bajo el agua, pausa)
template <typename T0 = Value, typename T1 = Value>
inline Value setLowPass(const T0& activar = {}, const T1& valor = {}) {
    return detail::lua("Audio.setLowPass", Values{Value(activar), Value(valor)});
}
/// Audio.setOcclusion(true)
/// paredes tapan los sonidos (Audio Listener)
template <typename T0 = Value>
inline Value setOcclusion(const T0& activar = {}) {
    return detail::lua("Audio.setOcclusion", Values{Value(activar)});
}
}  // namespace Audio

namespace Camera {
/// Camera.shake(0.6, 0.4, 18)
/// temblor de camara (intensidad 0..1, segundos, Hz)
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value shake(const T0& valor = {}, const T1& valor2 = {}, const T2& valor3 = {}) {
    return detail::lua("Camera.shake", Values{Value(valor), Value(valor2), Value(valor3)});
}
/// Camera.stopShake()
/// lo para
template <typename... Mas>
inline Value stopShake(Mas&&... mas) {
    return detail::lua("Camera.stopShake", Values{Value(std::forward<Mas>(mas))...});
}
}  // namespace Camera

namespace CharacterController {
/// CharacterController.Above: campo
inline Value Above() { return detail::luaGet("CharacterController.Above"); }
/// CharacterController.Below: campo
inline Value Below() { return detail::luaGet("CharacterController.Below"); }
/// CharacterController.Sides: campo
inline Value Sides() { return detail::luaGet("CharacterController.Sides"); }
}  // namespace CharacterController

namespace Crowd {
/// Crowd.stats()
/// {agents, visible, spawners}
template <typename... Mas>
inline Value stats(Mas&&... mas) {
    return detail::lua("Crowd.stats", Values{Value(std::forward<Mas>(mas))...});
}
}  // namespace Crowd

namespace DataPack {
/// DataPack.info("Nivel2")
/// {name, version, scenes, objects, files} sin montar nada (o nil)
template <typename T0 = Value>
inline Value info(const T0& texto = {}) {
    return detail::lua("DataPack.info", Values{Value(texto)});
}
/// DataPack.instantiate("Skins", "Coche", posicion, rotacion)
/// monta el paquete y crea un objeto (prefab) suyo; devuelve la Entity o nil
template <typename T0 = Value, typename T1 = Value, typename T2 = Value, typename T3 = Value>
inline Value instantiate(const T0& texto = {}, const T1& texto2 = {}, const T2& posicion = {}, const T3& rotacion = {}) {
    return detail::lua("DataPack.instantiate", Values{Value(texto), Value(texto2), Value(posicion), Value(rotacion)});
}
/// DataPack.isLoaded("Nivel2")
/// esta montado?
template <typename T0 = Value>
inline Value isLoaded(const T0& texto = {}) {
    return detail::lua("DataPack.isLoaded", Values{Value(texto)});
}
/// DataPack.list()
/// nombres de los paquetes montados
template <typename... Mas>
inline Value list(Mas&&... mas) {
    return detail::lua("DataPack.list", Values{Value(std::forward<Mas>(mas))...});
}
/// DataPack.load("Nivel2")
/// monta un .datapack (junto al juego, en DataPacks/ o ruta); devuelve {name, scenes, objects, files} o nil
template <typename T0 = Value>
inline Value load(const T0& texto = {}) {
    return detail::lua("DataPack.load", Values{Value(texto)});
}
/// DataPack.loadScene("Nivel2", "escena")
/// monta el paquete y carga su escena (la primera si no se dice cual); true/false
template <typename T0 = Value, typename T1 = Value>
inline Value loadScene(const T0& texto = {}, const T1& texto2 = {}) {
    return detail::lua("DataPack.loadScene", Values{Value(texto), Value(texto2)});
}
/// DataPack.unload("Nivel2")
/// desmonta el paquete (borra lo que extrajo)
template <typename T0 = Value>
inline Value unload(const T0& texto = {}) {
    return detail::lua("DataPack.unload", Values{Value(texto)});
}
}  // namespace DataPack

namespace Debug {
/// Debug.warn(...)
/// aviso
template <typename... Mas>
inline Value warn(Mas&&... mas) {
    return detail::lua("Debug.warn", Values{Value(std::forward<Mas>(mas))...});
}
}  // namespace Debug

namespace Dialogue {
/// Dialogue.advance()
/// lo mismo que next
template <typename... Mas>
inline Value advance(Mas&&... mas) {
    return detail::lua("Dialogue.advance", Values{Value(std::forward<Mas>(mas))...});
}
/// Dialogue.choices()
/// lista de {index, text, enabled}
template <typename... Mas>
inline Value choices(Mas&&... mas) {
    return detail::lua("Dialogue.choices", Values{Value(std::forward<Mas>(mas))...});
}
/// Dialogue.choose(1)
/// elige la opcion 1..n
template <typename T0 = Value>
inline Value choose(const T0& valor = {}) {
    return detail::lua("Dialogue.choose", Values{Value(valor)});
}
/// Dialogue.currentLine()
/// {speaker, text, audio, node, autoAdvance} o nil
template <typename... Mas>
inline Value currentLine(Mas&&... mas) {
    return detail::lua("Dialogue.currentLine", Values{Value(std::forward<Mas>(mas))...});
}
/// Dialogue.getVar("oro")
/// lee una variable
template <typename T0 = Value>
inline Value getVar(const T0& texto = {}) {
    return detail::lua("Dialogue.getVar", Values{Value(texto)});
}
/// Dialogue.isActive()
/// hay un dialogo en marcha?
template <typename... Mas>
inline Value isActive(Mas&&... mas) {
    return detail::lua("Dialogue.isActive", Values{Value(std::forward<Mas>(mas))...});
}
/// Dialogue.isWaitingChoice()
/// esta esperando que se elija?
template <typename... Mas>
inline Value isWaitingChoice(Mas&&... mas) {
    return detail::lua("Dialogue.isWaitingChoice", Values{Value(std::forward<Mas>(mas))...});
}
/// Dialogue.name()
/// dialogo que corre
template <typename... Mas>
inline Value name(Mas&&... mas) {
    return detail::lua("Dialogue.name", Values{Value(std::forward<Mas>(mas))...});
}
/// Dialogue.next()
/// sigue tras una linea
template <typename... Mas>
inline Value next(Mas&&... mas) {
    return detail::lua("Dialogue.next", Values{Value(std::forward<Mas>(mas))...});
}
/// Dialogue.onChoices(function(opciones) end)
/// opciones para elegir
template <typename T0 = Value>
inline Value onChoices(const T0& callback = {}) {
    return detail::lua("Dialogue.onChoices", Values{Value(callback)});
}
/// Dialogue.onEnd(function(nombre) end)
/// al terminar
template <typename T0 = Value>
inline Value onEnd(const T0& callback = {}) {
    return detail::lua("Dialogue.onEnd", Values{Value(callback)});
}
/// Dialogue.onEvent(function(nombre, argumento) end)
/// nodo Evento
template <typename T0 = Value>
inline Value onEvent(const T0& callback = {}) {
    return detail::lua("Dialogue.onEvent", Values{Value(callback)});
}
/// Dialogue.onLine(function(linea) end)
/// cada linea
template <typename T0 = Value>
inline Value onLine(const T0& callback = {}) {
    return detail::lua("Dialogue.onLine", Values{Value(callback)});
}
/// Dialogue.onStart(function(nombre) end)
/// al empezar
template <typename T0 = Value>
inline Value onStart(const T0& callback = {}) {
    return detail::lua("Dialogue.onStart", Values{Value(callback)});
}
/// Dialogue.setAutoAudio(true)
/// reproduce solo el audio de cada linea
template <typename T0 = Value>
inline Value setAutoAudio(const T0& activar = {}) {
    return detail::lua("Dialogue.setAutoAudio", Values{Value(activar)});
}
/// Dialogue.setVar("oro", 10)
/// variable de los dialogos
template <typename T0 = Value, typename T1 = Value>
inline Value setVar(const T0& texto = {}, const T1& valor = {}) {
    return detail::lua("Dialogue.setVar", Values{Value(texto), Value(valor)});
}
/// Dialogue.start("Mercader")
/// empieza un .crdialog (nombre o ruta)
template <typename T0 = Value>
inline Value start(const T0& texto = {}) {
    return detail::lua("Dialogue.start", Values{Value(texto)});
}
/// Dialogue.stop()
/// lo corta
template <typename... Mas>
inline Value stop(Mas&&... mas) {
    return detail::lua("Dialogue.stop", Values{Value(std::forward<Mas>(mas))...});
}
}  // namespace Dialogue

namespace Environment {
/// Environment.get(...)
template <typename... Mas>
inline Value get(Mas&&... mas) {
    return detail::lua("Environment.get", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.getDate(...)
template <typename... Mas>
inline Value getDate(Mas&&... mas) {
    return detail::lua("Environment.getDate", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.getFog(...)
template <typename... Mas>
inline Value getFog(Mas&&... mas) {
    return detail::lua("Environment.getFog", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.getLabel(...)
template <typename... Mas>
inline Value getLabel(Mas&&... mas) {
    return detail::lua("Environment.getLabel", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.getLatitude(...)
template <typename... Mas>
inline Value getLatitude(Mas&&... mas) {
    return detail::lua("Environment.getLatitude", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.getRain(...)
template <typename... Mas>
inline Value getRain(Mas&&... mas) {
    return detail::lua("Environment.getRain", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.getSeason(...)
template <typename... Mas>
inline Value getSeason(Mas&&... mas) {
    return detail::lua("Environment.getSeason", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.getSnow(...)
template <typename... Mas>
inline Value getSnow(Mas&&... mas) {
    return detail::lua("Environment.getSnow", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.getSnowCover(...)
template <typename... Mas>
inline Value getSnowCover(Mas&&... mas) {
    return detail::lua("Environment.getSnowCover", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.getSunDirection(...)
template <typename... Mas>
inline Value getSunDirection(Mas&&... mas) {
    return detail::lua("Environment.getSunDirection", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.getTarget(...)
template <typename... Mas>
inline Value getTarget(Mas&&... mas) {
    return detail::lua("Environment.getTarget", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.getTemperature(...)
template <typename... Mas>
inline Value getTemperature(Mas&&... mas) {
    return detail::lua("Environment.getTemperature", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.getTime(...)
template <typename... Mas>
inline Value getTime(Mas&&... mas) {
    return detail::lua("Environment.getTime", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.getTimeScale(...)
template <typename... Mas>
inline Value getTimeScale(Mas&&... mas) {
    return detail::lua("Environment.getTimeScale", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.getWeather(...)
template <typename... Mas>
inline Value getWeather(Mas&&... mas) {
    return detail::lua("Environment.getWeather", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.getWetness(...)
template <typename... Mas>
inline Value getWetness(Mas&&... mas) {
    return detail::lua("Environment.getWetness", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.getWind(...)
template <typename... Mas>
inline Value getWind(Mas&&... mas) {
    return detail::lua("Environment.getWind", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.getWindDirection(...)
template <typename... Mas>
inline Value getWindDirection(Mas&&... mas) {
    return detail::lua("Environment.getWindDirection", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.getWindSpeed(...)
template <typename... Mas>
inline Value getWindSpeed(Mas&&... mas) {
    return detail::lua("Environment.getWindSpeed", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.isNight(...)
template <typename... Mas>
inline Value isNight(Mas&&... mas) {
    return detail::lua("Environment.isNight", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.isTransitioning(...)
template <typename... Mas>
inline Value isTransitioning(Mas&&... mas) {
    return detail::lua("Environment.isTransitioning", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.lightning(...)
template <typename... Mas>
inline Value lightning(Mas&&... mas) {
    return detail::lua("Environment.lightning", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.presets(...)
template <typename... Mas>
inline Value presets(Mas&&... mas) {
    return detail::lua("Environment.presets", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.set(...)
template <typename... Mas>
inline Value set(Mas&&... mas) {
    return detail::lua("Environment.set", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.setAudio(...)
template <typename... Mas>
inline Value setAudio(Mas&&... mas) {
    return detail::lua("Environment.setAudio", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.setDate(...)
template <typename... Mas>
inline Value setDate(Mas&&... mas) {
    return detail::lua("Environment.setDate", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.setDayLength(...)
template <typename... Mas>
inline Value setDayLength(Mas&&... mas) {
    return detail::lua("Environment.setDayLength", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.setLatitude(...)
template <typename... Mas>
inline Value setLatitude(Mas&&... mas) {
    return detail::lua("Environment.setLatitude", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.setLightning(...)
template <typename... Mas>
inline Value setLightning(Mas&&... mas) {
    return detail::lua("Environment.setLightning", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.setPrecipitationDensity(...)
template <typename... Mas>
inline Value setPrecipitationDensity(Mas&&... mas) {
    return detail::lua("Environment.setPrecipitationDensity", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.setRandom(...)
template <typename... Mas>
inline Value setRandom(Mas&&... mas) {
    return detail::lua("Environment.setRandom", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.setSeason(...)
template <typename... Mas>
inline Value setSeason(Mas&&... mas) {
    return detail::lua("Environment.setSeason", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.setSnowCover(...)
template <typename... Mas>
inline Value setSnowCover(Mas&&... mas) {
    return detail::lua("Environment.setSnowCover", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.setTime(...)
template <typename... Mas>
inline Value setTime(Mas&&... mas) {
    return detail::lua("Environment.setTime", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.setTimeScale(...)
template <typename... Mas>
inline Value setTimeScale(Mas&&... mas) {
    return detail::lua("Environment.setTimeScale", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.setWeather(...)
template <typename... Mas>
inline Value setWeather(Mas&&... mas) {
    return detail::lua("Environment.setWeather", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.setWetness(...)
template <typename... Mas>
inline Value setWetness(Mas&&... mas) {
    return detail::lua("Environment.setWetness", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.setWind(...)
template <typename... Mas>
inline Value setWind(Mas&&... mas) {
    return detail::lua("Environment.setWind", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.transition(...)
template <typename... Mas>
inline Value transition(Mas&&... mas) {
    return detail::lua("Environment.transition", Values{Value(std::forward<Mas>(mas))...});
}
}  // namespace Environment

namespace Fire {
/// Fire.burnedAt(posicion)
/// quemado 0..1
template <typename T0 = Value>
inline Value burnedAt(const T0& posicion = {}) {
    return detail::lua("Fire.burnedAt", Values{Value(posicion)});
}
/// Fire.burnedFraction()
/// 0..1 de lo que podia arder
template <typename... Mas>
inline Value burnedFraction(Mas&&... mas) {
    return detail::lua("Fire.burnedFraction", Values{Value(std::forward<Mas>(mas))...});
}
/// Fire.burningArea()
/// m2 en llamas
template <typename... Mas>
inline Value burningArea(Mas&&... mas) {
    return detail::lua("Fire.burningArea", Values{Value(std::forward<Mas>(mas))...});
}
/// Fire.charAt(posicion)
/// lo mismo que burnedAt
template <typename T0 = Value>
inline Value charAt(const T0& posicion = {}) {
    return detail::lua("Fire.charAt", Values{Value(posicion)});
}
/// Fire.extinguish(posicion, radio)
/// apaga el fuego en el circulo
template <typename T0 = Value, typename T1 = Value>
inline Value extinguish(const T0& posicion = {}, const T1& radio = {}) {
    return detail::lua("Fire.extinguish", Values{Value(posicion), Value(radio)});
}
/// Fire.extinguishAll()
/// apaga todo
template <typename... Mas>
inline Value extinguishAll(Mas&&... mas) {
    return detail::lua("Fire.extinguishAll", Values{Value(std::forward<Mas>(mas))...});
}
/// Fire.heatAt(posicion)
/// calor 0..1
template <typename T0 = Value>
inline Value heatAt(const T0& posicion = {}) {
    return detail::lua("Fire.heatAt", Values{Value(posicion)});
}
/// Fire.ignite(posicion, radio)
/// enciende fuego en las zonas Fuego que tocan el circulo (devuelve cuantas)
template <typename T0 = Value, typename T1 = Value>
inline Value ignite(const T0& posicion = {}, const T1& radio = {}) {
    return detail::lua("Fire.ignite", Values{Value(posicion), Value(radio)});
}
/// Fire.isActive()
/// hay algo ardiendo?
template <typename... Mas>
inline Value isActive(Mas&&... mas) {
    return detail::lua("Fire.isActive", Values{Value(std::forward<Mas>(mas))...});
}
/// Fire.isBurning(posicion)
/// hay llamas ahi?
template <typename T0 = Value>
inline Value isBurning(const T0& posicion = {}) {
    return detail::lua("Fire.isBurning", Values{Value(posicion)});
}
/// Fire.reset()
/// vuelve a empezar: nada quemado (y se reenciende si 'Encender al empezar')
template <typename... Mas>
inline Value reset(Mas&&... mas) {
    return detail::lua("Fire.reset", Values{Value(std::forward<Mas>(mas))...});
}
/// Fire.stats()
/// {burningCells, burnedFraction, burningArea, seconds...}
template <typename... Mas>
inline Value stats(Mas&&... mas) {
    return detail::lua("Fire.stats", Values{Value(std::forward<Mas>(mas))...});
}
}  // namespace Fire

namespace Fluid {
/// Fluid.clear()
/// borra todo el liquido
template <typename... Mas>
inline Value clear(Mas&&... mas) {
    return detail::lua("Fluid.clear", Values{Value(std::forward<Mas>(mas))...});
}
/// Fluid.count("honey")
/// particulas (todas o de un tipo)
template <typename T0 = Value>
inline Value count(const T0& texto = {}) {
    return detail::lua("Fluid.count", Values{Value(texto)});
}
/// Fluid.density(pos, radio)
/// 0 = seco, ~1 = lleno
template <typename T0 = Value, typename T1 = Value>
inline Value density(const T0& pos = {}, const T1& radio = {}) {
    return detail::lua("Fluid.density", Values{Value(pos), Value(radio)});
}
/// Fluid.isActive()
/// hay liquidos en la escena?
template <typename... Mas>
inline Value isActive(Mas&&... mas) {
    return detail::lua("Fluid.isActive", Values{Value(std::forward<Mas>(mas))...});
}
/// Fluid.isInside(pos)
/// hay liquido ahi?
template <typename T0 = Value>
inline Value isInside(const T0& pos = {}) {
    return detail::lua("Fluid.isInside", Values{Value(pos)});
}
/// Fluid.restart(entidad)
/// vuelve a llenar una caja/esfera
template <typename T0 = Value>
inline Value restart(const T0& entidad = {}) {
    return detail::lua("Fluid.restart", Values{Value(entidad)});
}
/// Fluid.setType(entidad, "lava")
/// cambia el liquido del emisor
template <typename T0 = Value, typename T1 = Value>
inline Value setType(const T0& entidad = {}, const T1& texto = {}) {
    return detail::lua("Fluid.setType", Values{Value(entidad), Value(texto)});
}
/// Fluid.spawn(pos, cantidad, "water", vel, radio, vida)
/// crea liquido (bola de particulas)
template <typename T0 = Value, typename T1 = Value, typename T2 = Value, typename T3 = Value, typename T4 = Value, typename T5 = Value>
inline Value spawn(const T0& pos = {}, const T1& cantidad = {}, const T2& texto = {}, const T3& vel = {}, const T4& radio = {}, const T5& vida = {}) {
    return detail::lua("Fluid.spawn", Values{Value(pos), Value(cantidad), Value(texto), Value(vel), Value(radio), Value(vida)});
}
/// Fluid.start(entidad)
/// el emisor empieza
template <typename T0 = Value>
inline Value start(const T0& entidad = {}) {
    return detail::lua("Fluid.start", Values{Value(entidad)});
}
/// Fluid.stats()
/// {particles, capacity, emitters...}
template <typename... Mas>
inline Value stats(Mas&&... mas) {
    return detail::lua("Fluid.stats", Values{Value(std::forward<Mas>(mas))...});
}
/// Fluid.stop(entidad)
/// el emisor para
template <typename T0 = Value>
inline Value stop(const T0& entidad = {}) {
    return detail::lua("Fluid.stop", Values{Value(entidad)});
}
/// Fluid.surfaceHeight(x, z)
/// nil o la altura de la superficie
template <typename T0 = Value, typename T1 = Value>
inline Value surfaceHeight(const T0& x = {}, const T1& z = {}) {
    return detail::lua("Fluid.surfaceHeight", Values{Value(x), Value(z)});
}
/// Fluid.types()
/// lista de tipos
template <typename... Mas>
inline Value types(Mas&&... mas) {
    return detail::lua("Fluid.types", Values{Value(std::forward<Mas>(mas))...});
}
/// Fluid.velocity(pos, radio)
/// velocidad media del liquido (Vec3)
template <typename T0 = Value, typename T1 = Value>
inline Value velocity(const T0& pos = {}, const T1& radio = {}) {
    return detail::lua("Fluid.velocity", Values{Value(pos), Value(radio)});
}
}  // namespace Fluid

namespace Game {
/// Game.quit()
/// cierra el juego (en el editor, sale de Play)
template <typename... Mas>
inline Value quit(Mas&&... mas) {
    return detail::lua("Game.quit", Values{Value(std::forward<Mas>(mas))...});
}
}  // namespace Game

namespace Graphics {
/// Graphics.get("texture_quality")
/// valor de una opcion
template <typename T0 = Value>
inline Value get(const T0& texto = {}) {
    return detail::lua("Graphics.get", Values{Value(texto)});
}
/// Graphics.getAll()
/// tabla clave -> valor
template <typename... Mas>
inline Value getAll(Mas&&... mas) {
    return detail::lua("Graphics.getAll", Values{Value(std::forward<Mas>(mas))...});
}
/// Graphics.getPost("bloom")
/// campo del post-procesado global
template <typename T0 = Value>
inline Value getPost(const T0& texto = {}) {
    return detail::lua("Graphics.getPost", Values{Value(texto)});
}
/// Graphics.getQuality()
/// la ultima calidad rapida o Personalizada
template <typename... Mas>
inline Value getQuality(Mas&&... mas) {
    return detail::lua("Graphics.getQuality", Values{Value(std::forward<Mas>(mas))...});
}
/// Graphics.options()
/// lista {key, value, writable, description, choices} para un menu
template <typename... Mas>
inline Value options(Mas&&... mas) {
    return detail::lua("Graphics.options", Values{Value(std::forward<Mas>(mas))...});
}
/// Graphics.post: post-procesado global: Graphics.post.bloom = false
inline Value post() { return detail::luaGet("Graphics.post"); }
/// Graphics.postKeys()
/// claves del post-procesado
template <typename... Mas>
inline Value postKeys(Mas&&... mas) {
    return detail::lua("Graphics.postKeys", Values{Value(std::forward<Mas>(mas))...});
}
/// Graphics.qualityLevels()
/// lista de calidades
template <typename... Mas>
inline Value qualityLevels(Mas&&... mas) {
    return detail::lua("Graphics.qualityLevels", Values{Value(std::forward<Mas>(mas))...});
}
/// Graphics.resolutions()
/// resoluciones del monitor {width, height}
template <typename... Mas>
inline Value resolutions(Mas&&... mas) {
    return detail::lua("Graphics.resolutions", Values{Value(std::forward<Mas>(mas))...});
}
/// Graphics.save()
/// guarda la configuracion del jugador (juego exportado)
template <typename... Mas>
inline Value save(Mas&&... mas) {
    return detail::lua("Graphics.save", Values{Value(std::forward<Mas>(mas))...});
}
/// Graphics.set("vsync", true)
/// cambia una opcion; o Graphics.set{ clave = valor, ... }
template <typename T0 = Value, typename T1 = Value>
inline Value set(const T0& texto = {}, const T1& activar = {}) {
    return detail::lua("Graphics.set", Values{Value(texto), Value(activar)});
}
/// Graphics.setPost("bloom", false)
/// cambia el post-procesado global
template <typename T0 = Value, typename T1 = Value>
inline Value setPost(const T0& texto = {}, const T1& activar = {}) {
    return detail::lua("Graphics.setPost", Values{Value(texto), Value(activar)});
}
/// Graphics.setQuality("Alta")
/// calidad rapida: Baja, Media, Alta, Ultra (o 0..3)
template <typename T0 = Value>
inline Value setQuality(const T0& texto = {}) {
    return detail::lua("Graphics.setQuality", Values{Value(texto)});
}
}  // namespace Graphics

namespace Http {
/// Http.cancelAll()
/// cancela todas
template <typename... Mas>
inline Value cancelAll(Mas&&... mas) {
    return detail::lua("Http.cancelAll", Values{Value(std::forward<Mas>(mas))...});
}
/// Http.get("https://...", function(res) end, cabeceras)
/// GET; res = {ok, status, body, data, headers, error}
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value get(const T0& texto = {}, const T1& callback = {}, const T2& cabeceras = {}) {
    return detail::lua("Http.get", Values{Value(texto), Value(callback), Value(cabeceras)});
}
/// Http.pending()
/// peticiones sin terminar
template <typename... Mas>
inline Value pending(Mas&&... mas) {
    return detail::lua("Http.pending", Values{Value(std::forward<Mas>(mas))...});
}
/// Http.post("https://...", datos, function(res) end, cabeceras)
/// POST; datos texto o tabla (se manda como JSON)
template <typename T0 = Value, typename T1 = Value, typename T2 = Value, typename T3 = Value>
inline Value post(const T0& texto = {}, const T1& datos = {}, const T2& callback = {}, const T3& cabeceras = {}) {
    return detail::lua("Http.post", Values{Value(texto), Value(datos), Value(callback), Value(cabeceras)});
}
/// Http.query({ q = "hola", page = 2 })
/// "page=2&q=hola" (codificado)
template <typename T0 = Value>
inline Value query(const T0& tabla = {}) {
    return detail::lua("Http.query", Values{Value(tabla)});
}
/// Http.request({ url = "https://...", method = "PUT", headers = {}, body = {}, timeout = 20 }, function(res) end)
/// cualquier metodo, con tiempo maximo y tamano maximo (maxSize)
template <typename T0 = Value, typename T1 = Value>
inline Value request(const T0& tabla = {}, const T1& callback = {}) {
    return detail::lua("Http.request", Values{Value(tabla), Value(callback)});
}
/// Http.urlEncode("hola mundo")
/// texto seguro para una URL
template <typename T0 = Value>
inline Value urlEncode(const T0& texto = {}) {
    return detail::lua("Http.urlEncode", Values{Value(texto)});
}
}  // namespace Http

namespace Input {
/// Input.addMappingContext("Vuelo", 1)
/// activa un contexto (prioridad opcional)
template <typename T0 = Value, typename T1 = Value>
inline Value addMappingContext(const T0& texto = {}, const T1& valor = {}) {
    return detail::lua("Input.addMappingContext", Values{Value(texto), Value(valor)});
}
/// Input.anyKeyPressed()
/// tecla/boton pulsado este frame ("W", "Gamepad A") o nil
template <typename... Mas>
inline Value anyKeyPressed(Mas&&... mas) {
    return detail::lua("Input.anyKeyPressed", Values{Value(std::forward<Mas>(mas))...});
}
/// Input.bindAction("Jump", "triggered", function(valor, t) end)
/// llama a la funcion en ese evento; devuelve un id
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value bindAction(const T0& texto = {}, const T1& texto2 = {}, const T2& callback = {}) {
    return detail::lua("Input.bindAction", Values{Value(texto), Value(texto2), Value(callback)});
}
/// Input.clearMappingContexts(...)
template <typename... Mas>
inline Value clearMappingContexts(Mas&&... mas) {
    return detail::lua("Input.clearMappingContexts", Values{Value(std::forward<Mas>(mas))...});
}
/// Input.getAction("Move")
/// valor de una accion: Bool -> true/false, Axis1D -> numero, Axis2D/3D -> Vec3
template <typename T0 = Value>
inline Value getAction(const T0& texto = {}) {
    return detail::lua("Input.getAction", Values{Value(texto)});
}
/// Input.getActionElapsed("Fire")
/// segundos desde que empezo
template <typename T0 = Value>
inline Value getActionElapsed(const T0& texto = {}) {
    return detail::lua("Input.getActionElapsed", Values{Value(texto)});
}
/// Input.getActionState("Jump")
/// "none", "ongoing" o "triggered"
template <typename T0 = Value>
inline Value getActionState(const T0& texto = {}) {
    return detail::lua("Input.getActionState", Values{Value(texto)});
}
/// Input.getActionValue("Move")
/// Vec3 con el valor de la accion
template <typename T0 = Value>
inline Value getActionValue(const T0& texto = {}) {
    return detail::lua("Input.getActionValue", Values{Value(texto)});
}
/// Input.getActions(...)
template <typename... Mas>
inline Value getActions(Mas&&... mas) {
    return detail::lua("Input.getActions", Values{Value(std::forward<Mas>(mas))...});
}
/// Input.getAxis("Horizontal")
/// -1..1: Horizontal (A/D), Vertical (W/S), Mouse X, Mouse Y
template <typename T0 = Value>
inline Value getAxis(const T0& texto = {}) {
    return detail::lua("Input.getAxis", Values{Value(texto)});
}
/// Input.getBindings("Jump")
/// lista {context, key} de las teclas de una accion
template <typename T0 = Value>
inline Value getBindings(const T0& texto = {}) {
    return detail::lua("Input.getBindings", Values{Value(texto)});
}
/// Input.getGamepadAxis("leftx")
/// eje del mando: leftx, lefty, rightx, righty, lt, rt
template <typename T0 = Value>
inline Value getGamepadAxis(const T0& texto = {}) {
    return detail::lua("Input.getGamepadAxis", Values{Value(texto)});
}
/// Input.getGamepadButton("a")
/// boton del mando mantenido: a, b, x, y, lb, rb, ls, rs, start, back, up, down, left, right
template <typename T0 = Value>
inline Value getGamepadButton(const T0& texto = {}) {
    return detail::lua("Input.getGamepadButton", Values{Value(texto)});
}
/// Input.getGamepadButtonDown("a")
/// pulsado este frame
template <typename T0 = Value>
inline Value getGamepadButtonDown(const T0& texto = {}) {
    return detail::lua("Input.getGamepadButtonDown", Values{Value(texto)});
}
/// Input.getGamepadButtonUp("a")
/// soltado este frame
template <typename T0 = Value>
inline Value getGamepadButtonUp(const T0& texto = {}) {
    return detail::lua("Input.getGamepadButtonUp", Values{Value(texto)});
}
/// Input.getKey("W")
/// tecla mantenida
template <typename T0 = Value>
inline Value getKey(const T0& texto = {}) {
    return detail::lua("Input.getKey", Values{Value(texto)});
}
/// Input.getKeyDown("Space")
/// tecla pulsada este frame
template <typename T0 = Value>
inline Value getKeyDown(const T0& texto = {}) {
    return detail::lua("Input.getKeyDown", Values{Value(texto)});
}
/// Input.getKeyUp("E")
/// tecla soltada este frame
template <typename T0 = Value>
inline Value getKeyUp(const T0& texto = {}) {
    return detail::lua("Input.getKeyUp", Values{Value(texto)});
}
/// Input.getMappingContexts()
/// lista de contextos activos
template <typename... Mas>
inline Value getMappingContexts(Mas&&... mas) {
    return detail::lua("Input.getMappingContexts", Values{Value(std::forward<Mas>(mas))...});
}
/// Input.getMouseButton(0)
/// boton del raton mantenido (0 izq, 1 der, 2 medio)
template <typename T0 = Value>
inline Value getMouseButton(const T0& valor = {}) {
    return detail::lua("Input.getMouseButton", Values{Value(valor)});
}
/// Input.getMouseButtonDown(0)
/// boton pulsado este frame
template <typename T0 = Value>
inline Value getMouseButtonDown(const T0& valor = {}) {
    return detail::lua("Input.getMouseButtonDown", Values{Value(valor)});
}
/// Input.getMouseButtonUp(0)
/// boton soltado
template <typename T0 = Value>
inline Value getMouseButtonUp(const T0& valor = {}) {
    return detail::lua("Input.getMouseButtonUp", Values{Value(valor)});
}
/// Input.getTouch(1)
/// {id, position, delta, start, phase} de un dedo (1..touchCount)
template <typename T0 = Value>
inline Value getTouch(const T0& valor = {}) {
    return detail::lua("Input.getTouch", Values{Value(valor)});
}
/// Input.hasMappingContext("Vuelo")
/// esta activo?
template <typename T0 = Value>
inline Value hasMappingContext(const T0& texto = {}) {
    return detail::lua("Input.hasMappingContext", Values{Value(texto)});
}
/// Input.isActionOngoing(...)
template <typename... Mas>
inline Value isActionOngoing(Mas&&... mas) {
    return detail::lua("Input.isActionOngoing", Values{Value(std::forward<Mas>(mas))...});
}
/// Input.isActionTriggered("Fire")
/// disparada este frame (segun sus triggers)
template <typename T0 = Value>
inline Value isActionTriggered(const T0& texto = {}) {
    return detail::lua("Input.isActionTriggered", Values{Value(texto)});
}
/// Input.isCursorLocked()
/// esta capturado?
template <typename... Mas>
inline Value isCursorLocked(Mas&&... mas) {
    return detail::lua("Input.isCursorLocked", Values{Value(std::forward<Mas>(mas))...});
}
/// Input.isGamepadConnected()
/// hay un mando?
template <typename... Mas>
inline Value isGamepadConnected(Mas&&... mas) {
    return detail::lua("Input.isGamepadConnected", Values{Value(std::forward<Mas>(mas))...});
}
/// Input.isMobile()
/// movil o pantalla tactil
template <typename... Mas>
inline Value isMobile(Mas&&... mas) {
    return detail::lua("Input.isMobile", Values{Value(std::forward<Mas>(mas))...});
}
/// Input.lockCursor(true)
/// captura el raton (primera persona); false lo suelta
template <typename T0 = Value>
inline Value lockCursor(const T0& activar = {}) {
    return detail::lua("Input.lockCursor", Values{Value(activar)});
}
/// Input.mouseDelta()
/// Vec3 con el movimiento
template <typename... Mas>
inline Value mouseDelta(Mas&&... mas) {
    return detail::lua("Input.mouseDelta", Values{Value(std::forward<Mas>(mas))...});
}
/// Input.mousePosition()
/// Vec3 con la posicion del raton
template <typename... Mas>
inline Value mousePosition(Mas&&... mas) {
    return detail::lua("Input.mousePosition", Values{Value(std::forward<Mas>(mas))...});
}
/// Input.rebind("Default", "Jump", 1, "F")
/// cambia una tecla (1 = la primera de esa accion)
template <typename T0 = Value, typename T1 = Value, typename T2 = Value, typename T3 = Value>
inline Value rebind(const T0& texto = {}, const T1& texto2 = {}, const T2& valor = {}, const T3& texto3 = {}) {
    return detail::lua("Input.rebind", Values{Value(texto), Value(texto2), Value(valor), Value(texto3)});
}
/// Input.removeMappingContext("Vuelo")
/// desactiva un contexto
template <typename T0 = Value>
inline Value removeMappingContext(const T0& texto = {}) {
    return detail::lua("Input.removeMappingContext", Values{Value(texto)});
}
/// Input.resetBindings()
/// vuelve a las teclas del proyecto
template <typename... Mas>
inline Value resetBindings(Mas&&... mas) {
    return detail::lua("Input.resetBindings", Values{Value(std::forward<Mas>(mas))...});
}
/// Input.saveBindings()
/// guarda las teclas cambiadas (entre partidas)
template <typename... Mas>
inline Value saveBindings(Mas&&... mas) {
    return detail::lua("Input.saveBindings", Values{Value(std::forward<Mas>(mas))...});
}
/// Input.setTouchButton("Saltar", true)
/// muestra u oculta un boton tactil por su texto
template <typename T0 = Value, typename T1 = Value>
inline Value setTouchButton(const T0& texto = {}, const T1& activar = {}) {
    return detail::lua("Input.setTouchButton", Values{Value(texto), Value(activar)});
}
/// Input.setTouchControls(true)
/// muestra u oculta los controles tactiles
template <typename T0 = Value>
inline Value setTouchControls(const T0& activar = {}) {
    return detail::lua("Input.setTouchControls", Values{Value(activar)});
}
/// Input.setTouchJoystick(true)
/// joystick tactil
template <typename T0 = Value>
inline Value setTouchJoystick(const T0& activar = {}) {
    return detail::lua("Input.setTouchJoystick", Values{Value(activar)});
}
/// Input.setTouchLook(true)
/// zona para mirar arrastrando
template <typename T0 = Value>
inline Value setTouchLook(const T0& activar = {}) {
    return detail::lua("Input.setTouchLook", Values{Value(activar)});
}
/// Input.touchControlsEnabled()
/// estan visibles?
template <typename... Mas>
inline Value touchControlsEnabled(Mas&&... mas) {
    return detail::lua("Input.touchControlsEnabled", Values{Value(std::forward<Mas>(mas))...});
}
/// Input.touchCount()
/// dedos en la pantalla
template <typename... Mas>
inline Value touchCount(Mas&&... mas) {
    return detail::lua("Input.touchCount", Values{Value(std::forward<Mas>(mas))...});
}
/// Input.unbindAction(id)
/// quita un bindAction
template <typename T0 = Value>
inline Value unbindAction(const T0& id = {}) {
    return detail::lua("Input.unbindAction", Values{Value(id)});
}
/// Input.vibrate(60)
/// vibra el movil (ms)
template <typename T0 = Value>
inline Value vibrate(const T0& valor = {}) {
    return detail::lua("Input.vibrate", Values{Value(valor)});
}
/// Input.wasActionCanceled("Jump")
/// se solto sin llegar a dispararse (Hold corto...)
template <typename T0 = Value>
inline Value wasActionCanceled(const T0& texto = {}) {
    return detail::lua("Input.wasActionCanceled", Values{Value(texto)});
}
/// Input.wasActionCompleted("Jump")
/// termino este frame (se solto tras dispararse)
template <typename T0 = Value>
inline Value wasActionCompleted(const T0& texto = {}) {
    return detail::lua("Input.wasActionCompleted", Values{Value(texto)});
}
/// Input.wasActionStarted("Jump")
/// empezo este frame
template <typename T0 = Value>
inline Value wasActionStarted(const T0& texto = {}) {
    return detail::lua("Input.wasActionStarted", Values{Value(texto)});
}
}  // namespace Input

namespace Jobs {
/// Jobs.executed()
/// tareas ejecutadas
template <typename... Mas>
inline Value executed(Mas&&... mas) {
    return detail::lua("Jobs.executed", Values{Value(std::forward<Mas>(mas))...});
}
/// Jobs.workers()
/// hilos del job system
template <typename... Mas>
inline Value workers(Mas&&... mas) {
    return detail::lua("Jobs.workers", Values{Value(std::forward<Mas>(mas))...});
}
}  // namespace Jobs

namespace Json {
/// Json.decode(texto)
/// texto JSON -> tabla (nil + error si no es JSON)
template <typename T0 = Value>
inline Value decode(const T0& texto = {}) {
    return detail::lua("Json.decode", Values{Value(texto)});
}
/// Json.encode(tabla, bonito)
/// tabla -> texto JSON (nil + error si no se puede)
template <typename T0 = Value, typename T1 = Value>
inline Value encode(const T0& tabla = {}, const T1& bonito = {}) {
    return detail::lua("Json.encode", Values{Value(tabla), Value(bonito)});
}
}  // namespace Json

namespace Mods {
/// Mods.enabled()
/// el juego carga mods?
template <typename... Mas>
inline Value enabled(Mas&&... mas) {
    return detail::lua("Mods.enabled", Values{Value(std::forward<Mas>(mas))...});
}
/// Mods.isLoaded(id)
template <typename T0 = Value>
inline Value isLoaded(const T0& id = {}) {
    return detail::lua("Mods.isLoaded", Values{Value(id)});
}
/// Mods.list()
/// {id, name, version, author, description, enabled, loaded}
template <typename... Mas>
inline Value list(Mas&&... mas) {
    return detail::lua("Mods.list", Values{Value(std::forward<Mas>(mas))...});
}
/// Mods.setEnabled(id, false)
/// activa o desactiva (al volver a abrir)
template <typename T0 = Value, typename T1 = Value>
inline Value setEnabled(const T0& id = {}, const T1& activar = {}) {
    return detail::lua("Mods.setEnabled", Values{Value(id), Value(activar)});
}
}  // namespace Mods

namespace Navigation {
/// Navigation.findPath(desde, hasta)
/// nil o lista de Vec3 (los giros del camino)
template <typename T0 = Value, typename T1 = Value>
inline Value findPath(const T0& desde = {}, const T1& hasta = {}) {
    return detail::lua("Navigation.findPath", Values{Value(desde), Value(hasta)});
}
/// Navigation.isReady()
/// hay malla?
template <typename... Mas>
inline Value isReady(Mas&&... mas) {
    return detail::lua("Navigation.isReady", Values{Value(std::forward<Mas>(mas))...});
}
/// Navigation.projectPoint(Vec3, radio)
/// nil o el punto de la malla mas cercano
template <typename T0 = Value, typename T1 = Value>
inline Value projectPoint(const T0& vec = {}, const T1& radio = {}) {
    return detail::lua("Navigation.projectPoint", Values{Value(vec), Value(radio)});
}
/// Navigation.randomPoint(centro, radio)
/// nil o un punto al azar de la malla
template <typename T0 = Value, typename T1 = Value>
inline Value randomPoint(const T0& centro = {}, const T1& radio = {}) {
    return detail::lua("Navigation.randomPoint", Values{Value(centro), Value(radio)});
}
/// Navigation.raycast(desde, hasta)
/// llega?, punto del choque
template <typename T0 = Value, typename T1 = Value>
inline Value raycast(const T0& desde = {}, const T1& hasta = {}) {
    return detail::lua("Navigation.raycast", Values{Value(desde), Value(hasta)});
}
}  // namespace Navigation

namespace Network {
/// Network.SERVER: id del servidor (1)
inline Value SERVER() { return detail::luaGet("Network.SERVER"); }
/// Network.connect("127.0.0.1", 7777)
/// se une a una partida (llega onConnected u onDisconnected)
template <typename T0 = Value, typename T1 = Value>
inline Value connect(const T0& texto = {}, const T1& valor = {}) {
    return detail::lua("Network.connect", Values{Value(texto), Value(valor)});
}
/// Network.destroy(entity)
/// lo borra en todos (solo el servidor)
template <typename T0 = Value>
inline Value destroy(const T0& entity = {}) {
    return detail::lua("Network.destroy", Values{Value(entity)});
}
/// Network.disconnect()
/// sale de la partida (o la cierra si eres el servidor)
template <typename... Mas>
inline Value disconnect(Mas&&... mas) {
    return detail::lua("Network.disconnect", Values{Value(std::forward<Mas>(mas))...});
}
/// Network.find(netId)
/// entidad por su id de red
template <typename T0 = Value>
inline Value find(const T0& netId = {}) {
    return detail::lua("Network.find", Values{Value(netId)});
}
/// Network.host(7777, 8)
/// crea la partida (eres el servidor y juegas); devuelve ok, error
template <typename T0 = Value, typename T1 = Value>
inline Value host(const T0& valor = {}, const T1& valor2 = {}) {
    return detail::lua("Network.host", Values{Value(valor), Value(valor2)});
}
/// Network.isActive()
/// hay sesion de red
template <typename... Mas>
inline Value isActive(Mas&&... mas) {
    return detail::lua("Network.isActive", Values{Value(std::forward<Mas>(mas))...});
}
/// Network.isClient()
/// eres un cliente?
template <typename... Mas>
inline Value isClient(Mas&&... mas) {
    return detail::lua("Network.isClient", Values{Value(std::forward<Mas>(mas))...});
}
/// Network.isConnected()
/// en partida (servidor abierto o cliente dentro)
template <typename... Mas>
inline Value isConnected(Mas&&... mas) {
    return detail::lua("Network.isConnected", Values{Value(std::forward<Mas>(mas))...});
}
/// Network.isConnecting()
/// cliente esperando respuesta
template <typename... Mas>
inline Value isConnecting(Mas&&... mas) {
    return detail::lua("Network.isConnecting", Values{Value(std::forward<Mas>(mas))...});
}
/// Network.isDedicated(...)
template <typename... Mas>
inline Value isDedicated(Mas&&... mas) {
    return detail::lua("Network.isDedicated", Values{Value(std::forward<Mas>(mas))...});
}
/// Network.isServer()
/// eres el servidor?
template <typename... Mas>
inline Value isServer(Mas&&... mas) {
    return detail::lua("Network.isServer", Values{Value(std::forward<Mas>(mas))...});
}
/// Network.lagCompensatedRaycast(...)
template <typename... Mas>
inline Value lagCompensatedRaycast(Mas&&... mas) {
    return detail::lua("Network.lagCompensatedRaycast", Values{Value(std::forward<Mas>(mas))...});
}
/// Network.loadScene("Nivel2")
/// todos cargan la escena (solo el servidor)
template <typename T0 = Value>
inline Value loadScene(const T0& texto = {}) {
    return detail::lua("Network.loadScene", Values{Value(texto)});
}
/// Network.myId()
/// tu id de jugador (el servidor es 1)
template <typename... Mas>
inline Value myId(Mas&&... mas) {
    return detail::lua("Network.myId", Values{Value(std::forward<Mas>(mas))...});
}
/// Network.objects()
/// todas las entidades de red
template <typename... Mas>
inline Value objects(Mas&&... mas) {
    return detail::lua("Network.objects", Values{Value(std::forward<Mas>(mas))...});
}
/// Network.off("chat")
/// deja de recibirlo
template <typename T0 = Value>
inline Value off(const T0& texto = {}) {
    return detail::lua("Network.off", Values{Value(texto)});
}
/// Network.on("chat", function(datos, de) end)
/// recibe un mensaje
template <typename T0 = Value, typename T1 = Value>
inline Value on(const T0& texto = {}, const T1& callback = {}) {
    return detail::lua("Network.on", Values{Value(texto), Value(callback)});
}
/// Network.onConnected(function(id) end)
/// cliente: ya estas dentro
template <typename T0 = Value>
inline Value onConnected(const T0& callback = {}) {
    return detail::lua("Network.onConnected", Values{Value(callback)});
}
/// Network.onDisconnected(function(motivo) end)
/// fuera de la partida
template <typename T0 = Value>
inline Value onDisconnected(const T0& callback = {}) {
    return detail::lua("Network.onDisconnected", Values{Value(callback)});
}
/// Network.onPlayerJoined(function(id) end)
/// entra un jugador
template <typename T0 = Value>
inline Value onPlayerJoined(const T0& callback = {}) {
    return detail::lua("Network.onPlayerJoined", Values{Value(callback)});
}
/// Network.onPlayerLeft(function(id) end)
/// sale un jugador
template <typename T0 = Value>
inline Value onPlayerLeft(const T0& callback = {}) {
    return detail::lua("Network.onPlayerLeft", Values{Value(callback)});
}
/// Network.ping(id)
/// ida y vuelta en ms
template <typename T0 = Value>
inline Value ping(const T0& id = {}) {
    return detail::lua("Network.ping", Values{Value(id)});
}
/// Network.playerCount()
/// cuantos jugadores
template <typename... Mas>
inline Value playerCount(Mas&&... mas) {
    return detail::lua("Network.playerCount", Values{Value(std::forward<Mas>(mas))...});
}
/// Network.players()
/// lista de ids de jugadores
template <typename... Mas>
inline Value players(Mas&&... mas) {
    return detail::lua("Network.players", Values{Value(std::forward<Mas>(mas))...});
}
/// Network.positionAt(...)
template <typename... Mas>
inline Value positionAt(Mas&&... mas) {
    return detail::lua("Network.positionAt", Values{Value(std::forward<Mas>(mas))...});
}
/// Network.send("chat", datos, destino)
/// mensaje (destino: nil = todos, "server" o un id)
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value send(const T0& texto = {}, const T1& datos = {}, const T2& destino = {}) {
    return detail::lua("Network.send", Values{Value(texto), Value(datos), Value(destino)});
}
/// Network.simulate(...)
template <typename... Mas>
inline Value simulate(Mas&&... mas) {
    return detail::lua("Network.simulate", Values{Value(std::forward<Mas>(mas))...});
}
/// Network.spawn("Prefabs/Jugador", posicion, dueno)
/// crea un objeto de red en todos (solo el servidor)
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value spawn(const T0& texto = {}, const T1& posicion = {}, const T2& dueno = {}) {
    return detail::lua("Network.spawn", Values{Value(texto), Value(posicion), Value(dueno)});
}
/// Network.stats()
/// {sent, received, objects}
template <typename... Mas>
inline Value stats(Mas&&... mas) {
    return detail::lua("Network.stats", Values{Value(std::forward<Mas>(mas))...});
}
}  // namespace Network

namespace Physics {
/// Physics.ignoreCollision(a, b, true)
/// a y b no chocan entre si (false lo deshace)
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value ignoreCollision(const T0& a = {}, const T1& b = {}, const T2& activar = {}) {
    return detail::lua("Physics.ignoreCollision", Values{Value(a), Value(b), Value(activar)});
}
}  // namespace Physics

namespace Physics2D {
/// Physics2D.getGravity()
/// Vec3 gravedad 2D
template <typename... Mas>
inline Value getGravity(Mas&&... mas) {
    return detail::lua("Physics2D.getGravity", Values{Value(std::forward<Mas>(mas))...});
}
/// Physics2D.overlapBox(centro, tamano, angulo, mascara)
/// objetos dentro de la caja
template <typename T0 = Value, typename T1 = Value, typename T2 = Value, typename T3 = Value>
inline Value overlapBox(const T0& centro = {}, const T1& tamano = {}, const T2& angulo = {}, const T3& mascara = {}) {
    return detail::lua("Physics2D.overlapBox", Values{Value(centro), Value(tamano), Value(angulo), Value(mascara)});
}
/// Physics2D.overlapCircle(centro, radio, mascara)
/// objetos con collider 2D dentro del circulo
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value overlapCircle(const T0& centro = {}, const T1& radio = {}, const T2& mascara = {}) {
    return detail::lua("Physics2D.overlapCircle", Values{Value(centro), Value(radio), Value(mascara)});
}
/// Physics2D.overlapPoint(punto, mascara)
/// objetos que tocan el punto
template <typename T0 = Value, typename T1 = Value>
inline Value overlapPoint(const T0& punto = {}, const T1& mascara = {}) {
    return detail::lua("Physics2D.overlapPoint", Values{Value(punto), Value(mascara)});
}
/// Physics2D.raycast(origen, direccion, distancia, mascara)
/// nil o {entity, point, normal, distance, fraction}
template <typename T0 = Value, typename T1 = Value, typename T2 = Value, typename T3 = Value>
inline Value raycast(const T0& origen = {}, const T1& direccion = {}, const T2& distancia = {}, const T3& mascara = {}) {
    return detail::lua("Physics2D.raycast", Values{Value(origen), Value(direccion), Value(distancia), Value(mascara)});
}
/// Physics2D.raycastAll(origen, direccion, distancia, mascara)
/// lista de choques, del mas cercano al mas lejano
template <typename T0 = Value, typename T1 = Value, typename T2 = Value, typename T3 = Value>
inline Value raycastAll(const T0& origen = {}, const T1& direccion = {}, const T2& distancia = {}, const T3& mascara = {}) {
    return detail::lua("Physics2D.raycastAll", Values{Value(origen), Value(direccion), Value(distancia), Value(mascara)});
}
/// Physics2D.setGravity(Vec3(0, -9.81, 0))
/// cambia la gravedad 2D
template <typename T0 = Value>
inline Value setGravity(const T0& arg = {}) {
    return detail::lua("Physics2D.setGravity", Values{Value(arg)});
}
}  // namespace Physics2D

namespace Prefs {
/// Prefs.deleteAll()
/// borra todo
template <typename... Mas>
inline Value deleteAll(Mas&&... mas) {
    return detail::lua("Prefs.deleteAll", Values{Value(std::forward<Mas>(mas))...});
}
/// Prefs.deleteKey("clave")
/// la borra
template <typename T0 = Value>
inline Value deleteKey(const T0& texto = {}) {
    return detail::lua("Prefs.deleteKey", Values{Value(texto)});
}
/// Prefs.getFloat("clave", 0.0)
/// lee un numero
template <typename T0 = Value, typename T1 = Value>
inline Value getFloat(const T0& texto = {}, const T1& valor = {}) {
    return detail::lua("Prefs.getFloat", Values{Value(texto), Value(valor)});
}
/// Prefs.getInt("clave", 0)
/// lee un entero (o el valor por defecto)
template <typename T0 = Value, typename T1 = Value>
inline Value getInt(const T0& texto = {}, const T1& valor = {}) {
    return detail::lua("Prefs.getInt", Values{Value(texto), Value(valor)});
}
/// Prefs.getString("clave", "")
/// lee un texto
template <typename T0 = Value, typename T1 = Value>
inline Value getString(const T0& texto = {}, const T1& texto2 = {}) {
    return detail::lua("Prefs.getString", Values{Value(texto), Value(texto2)});
}
/// Prefs.hasKey("clave")
/// existe?
template <typename T0 = Value>
inline Value hasKey(const T0& texto = {}) {
    return detail::lua("Prefs.hasKey", Values{Value(texto)});
}
/// Prefs.setFloat("clave", 0.5)
/// guarda un numero
template <typename T0 = Value, typename T1 = Value>
inline Value setFloat(const T0& texto = {}, const T1& valor = {}) {
    return detail::lua("Prefs.setFloat", Values{Value(texto), Value(valor)});
}
/// Prefs.setInt("clave", 3)
/// guarda un entero
template <typename T0 = Value, typename T1 = Value>
inline Value setInt(const T0& texto = {}, const T1& valor = {}) {
    return detail::lua("Prefs.setInt", Values{Value(texto), Value(valor)});
}
/// Prefs.setString("clave", "texto")
/// guarda un texto
template <typename T0 = Value, typename T1 = Value>
inline Value setString(const T0& texto = {}, const T1& texto2 = {}) {
    return detail::lua("Prefs.setString", Values{Value(texto), Value(texto2)});
}
}  // namespace Prefs

namespace Profiler {
/// Profiler.begin(...)
template <typename... Mas>
inline Value begin(Mas&&... mas) {
    return detail::lua("Profiler.begin", Values{Value(std::forward<Mas>(mas))...});
}
/// Profiler.capture(...)
template <typename... Mas>
inline Value capture(Mas&&... mas) {
    return detail::lua("Profiler.capture", Values{Value(std::forward<Mas>(mas))...});
}
/// Profiler.counter(...)
template <typename... Mas>
inline Value counter(Mas&&... mas) {
    return detail::lua("Profiler.counter", Values{Value(std::forward<Mas>(mas))...});
}
/// Profiler.finish(...)
template <typename... Mas>
inline Value finish(Mas&&... mas) {
    return detail::lua("Profiler.finish", Values{Value(std::forward<Mas>(mas))...});
}
/// Profiler.frameMs(...)
template <typename... Mas>
inline Value frameMs(Mas&&... mas) {
    return detail::lua("Profiler.frameMs", Values{Value(std::forward<Mas>(mas))...});
}
/// Profiler.zones(...)
template <typename... Mas>
inline Value zones(Mas&&... mas) {
    return detail::lua("Profiler.zones", Values{Value(std::forward<Mas>(mas))...});
}
}  // namespace Profiler

namespace Random {
/// Random.chance(0.25)
/// true con esa probabilidad
template <typename T0 = Value>
inline Value chance(const T0& valor = {}) {
    return detail::lua("Random.chance", Values{Value(valor)});
}
/// Random.insideUnitCircle()
/// punto en el suelo (XZ)
template <typename... Mas>
inline Value insideUnitCircle(Mas&&... mas) {
    return detail::lua("Random.insideUnitCircle", Values{Value(std::forward<Mas>(mas))...});
}
/// Random.insideUnitSphere()
/// punto dentro de la esfera
template <typename... Mas>
inline Value insideUnitSphere(Mas&&... mas) {
    return detail::lua("Random.insideUnitSphere", Values{Value(std::forward<Mas>(mas))...});
}
/// Random.int(min, max)
/// entero (incluye los dos)
template <typename T0 = Value, typename T1 = Value>
inline Value int_(const T0& min_ = {}, const T1& max_ = {}) {
    return detail::lua("Random.int", Values{Value(min_), Value(max_)});
}
/// Random.onUnitSphere()
/// direccion al azar
template <typename... Mas>
inline Value onUnitSphere(Mas&&... mas) {
    return detail::lua("Random.onUnitSphere", Values{Value(std::forward<Mas>(mas))...});
}
/// Random.pick(lista)
/// un elemento al azar
template <typename T0 = Value>
inline Value pick(const T0& lista = {}) {
    return detail::lua("Random.pick", Values{Value(lista)});
}
/// Random.range(min, max)
/// decimal
template <typename T0 = Value, typename T1 = Value>
inline Value range(const T0& min_ = {}, const T1& max_ = {}) {
    return detail::lua("Random.range", Values{Value(min_), Value(max_)});
}
/// Random.rotation()
/// Quat al azar
template <typename... Mas>
inline Value rotation(Mas&&... mas) {
    return detail::lua("Random.rotation", Values{Value(std::forward<Mas>(mas))...});
}
/// Random.seed(n)
/// fija la semilla
template <typename T0 = Value>
inline Value seed(const T0& n = {}) {
    return detail::lua("Random.seed", Values{Value(n)});
}
/// Random.shuffle(lista)
/// baraja
template <typename T0 = Value>
inline Value shuffle(const T0& lista = {}) {
    return detail::lua("Random.shuffle", Values{Value(lista)});
}
/// Random.sign()
/// -1 o 1
template <typename... Mas>
inline Value sign(Mas&&... mas) {
    return detail::lua("Random.sign", Values{Value(std::forward<Mas>(mas))...});
}
/// Random.value()
/// 0..1
template <typename... Mas>
inline Value value(Mas&&... mas) {
    return detail::lua("Random.value", Values{Value(std::forward<Mas>(mas))...});
}
}  // namespace Random

namespace Replay {
/// Replay.duration()
/// segundos grabados
template <typename... Mas>
inline Value duration(Mas&&... mas) {
    return detail::lua("Replay.duration", Values{Value(std::forward<Mas>(mas))...});
}
/// Replay.isPlaying()
/// esta reproduciendo?
template <typename... Mas>
inline Value isPlaying(Mas&&... mas) {
    return detail::lua("Replay.isPlaying", Values{Value(std::forward<Mas>(mas))...});
}
/// Replay.isRecording()
/// esta grabando?
template <typename... Mas>
inline Value isRecording(Mas&&... mas) {
    return detail::lua("Replay.isRecording", Values{Value(std::forward<Mas>(mas))...});
}
/// Replay.list()
/// repeticiones guardadas
template <typename... Mas>
inline Value list(Mas&&... mas) {
    return detail::lua("Replay.list", Values{Value(std::forward<Mas>(mas))...});
}
/// Replay.load("gol")
/// carga un .crreplay
template <typename T0 = Value>
inline Value load(const T0& texto = {}) {
    return detail::lua("Replay.load", Values{Value(texto)});
}
/// Replay.mark("Gol", datos)
/// marcador en la linea de tiempo
template <typename T0 = Value, typename T1 = Value>
inline Value mark(const T0& texto = {}, const T1& datos = {}) {
    return detail::lua("Replay.mark", Values{Value(texto), Value(datos)});
}
/// Replay.pause(true)
/// pausa la reproduccion
template <typename T0 = Value>
inline Value pause(const T0& activar = {}) {
    return detail::lua("Replay.pause", Values{Value(activar)});
}
/// Replay.play(desde, velocidad)
/// reproduce (desde < 0 = los ultimos N segundos)
template <typename T0 = Value, typename T1 = Value>
inline Value play(const T0& desde = {}, const T1& velocidad = {}) {
    return detail::lua("Replay.play", Values{Value(desde), Value(velocidad)});
}
/// Replay.save("gol")
/// guarda un .crreplay
template <typename T0 = Value>
inline Value save(const T0& texto = {}) {
    return detail::lua("Replay.save", Values{Value(texto)});
}
/// Replay.seek(segundos)
/// salta a ese momento
template <typename T0 = Value>
inline Value seek(const T0& segundos = {}) {
    return detail::lua("Replay.seek", Values{Value(segundos)});
}
/// Replay.setFreeCamera(true)
/// camara libre (WASD + raton)
template <typename T0 = Value>
inline Value setFreeCamera(const T0& activar = {}) {
    return detail::lua("Replay.setFreeCamera", Values{Value(activar)});
}
/// Replay.setLoop(true)
/// en bucle
template <typename T0 = Value>
inline Value setLoop(const T0& activar = {}) {
    return detail::lua("Replay.setLoop", Values{Value(activar)});
}
/// Replay.setSpeed(0.25)
/// camara lenta / rapida
template <typename T0 = Value>
inline Value setSpeed(const T0& valor = {}) {
    return detail::lua("Replay.setSpeed", Values{Value(valor)});
}
/// Replay.start({rate = 30, maxSeconds = 10, tag = "Coche"})
/// empieza a grabar
template <typename T0 = Value>
inline Value start(const T0& tabla = {}) {
    return detail::lua("Replay.start", Values{Value(tabla)});
}
/// Replay.stop()
/// deja de grabar
template <typename... Mas>
inline Value stop(Mas&&... mas) {
    return detail::lua("Replay.stop", Values{Value(std::forward<Mas>(mas))...});
}
/// Replay.stopPlayback()
/// vuelve al juego
template <typename... Mas>
inline Value stopPlayback(Mas&&... mas) {
    return detail::lua("Replay.stopPlayback", Values{Value(std::forward<Mas>(mas))...});
}
/// Replay.time()
/// segundo de la reproduccion
template <typename... Mas>
inline Value time(Mas&&... mas) {
    return detail::lua("Replay.time", Values{Value(std::forward<Mas>(mas))...});
}
}  // namespace Replay

namespace Save {
/// Save.clearValues()
/// borra todos
template <typename... Mas>
inline Value clearValues(Mas&&... mas) {
    return detail::lua("Save.clearValues", Values{Value(std::forward<Mas>(mas))...});
}
/// Save.delete("slot1")
/// borra una ranura
template <typename T0 = Value>
inline Value delete_(const T0& texto = {}) {
    return detail::lua("Save.delete", Values{Value(texto)});
}
/// Save.deleteValue("oro")
/// lo borra
template <typename T0 = Value>
inline Value deleteValue(const T0& texto = {}) {
    return detail::lua("Save.deleteValue", Values{Value(texto)});
}
/// Save.exists("slot1")
/// existe?
template <typename T0 = Value>
inline Value exists(const T0& texto = {}) {
    return detail::lua("Save.exists", Values{Value(texto)});
}
/// Save.folder()
/// carpeta de las partidas
template <typename... Mas>
inline Value folder(Mas&&... mas) {
    return detail::lua("Save.folder", Values{Value(std::forward<Mas>(mas))...});
}
/// Save.getValue("oro", 0)
/// lee un valor suelto
template <typename T0 = Value, typename T1 = Value>
inline Value getValue(const T0& texto = {}, const T1& valor = {}) {
    return detail::lua("Save.getValue", Values{Value(texto), Value(valor)});
}
/// Save.hasValue("oro")
/// existe?
template <typename T0 = Value>
inline Value hasValue(const T0& texto = {}) {
    return detail::lua("Save.hasValue", Values{Value(texto)});
}
/// Save.info("slot1")
/// datos de una ranura (o nil)
template <typename T0 = Value>
inline Value info(const T0& texto = {}) {
    return detail::lua("Save.info", Values{Value(texto)});
}
/// Save.isWriting()
/// esta escribiendo en segundo plano?
template <typename... Mas>
inline Value isWriting(Mas&&... mas) {
    return detail::lua("Save.isWriting", Values{Value(std::forward<Mas>(mas))...});
}
/// Save.list()
/// {slot, label, scene, date, playtime, size} de cada ranura
template <typename... Mas>
inline Value list(Mas&&... mas) {
    return detail::lua("Save.list", Values{Value(std::forward<Mas>(mas))...});
}
/// Save.load("slot1")
/// carga una partida (cambia de escena si hace falta)
template <typename T0 = Value>
inline Value load(const T0& texto = {}) {
    return detail::lua("Save.load", Values{Value(texto)});
}
/// Save.onLoaded(function(slot) end)
/// despues de cargar una partida
template <typename T0 = Value>
inline Value onLoaded(const T0& callback = {}) {
    return detail::lua("Save.onLoaded", Values{Value(callback)});
}
/// Save.playtime()
/// segundos jugados
template <typename... Mas>
inline Value playtime(Mas&&... mas) {
    return detail::lua("Save.playtime", Values{Value(std::forward<Mas>(mas))...});
}
/// Save.remove(...)
template <typename... Mas>
inline Value remove(Mas&&... mas) {
    return detail::lua("Save.remove", Values{Value(std::forward<Mas>(mas))...});
}
/// Save.save("slot1", "Etiqueta")
/// guarda la partida (objetos Saveable, valores, dialogos)
template <typename T0 = Value, typename T1 = Value>
inline Value save(const T0& texto = {}, const T1& texto2 = {}) {
    return detail::lua("Save.save", Values{Value(texto), Value(texto2)});
}
/// Save.setAutosave(60, "autosave")
/// autoguardado cada N segundos (0 = no)
template <typename T0 = Value, typename T1 = Value>
inline Value setAutosave(const T0& valor = {}, const T1& texto = {}) {
    return detail::lua("Save.setAutosave", Values{Value(valor), Value(texto)});
}
/// Save.setCompression(true)
/// partidas comprimidas
template <typename T0 = Value>
inline Value setCompression(const T0& activar = {}) {
    return detail::lua("Save.setCompression", Values{Value(activar)});
}
/// Save.setValue("oro", 120)
/// valor suelto (va en cada partida)
template <typename T0 = Value, typename T1 = Value>
inline Value setValue(const T0& texto = {}, const T1& valor = {}) {
    return detail::lua("Save.setValue", Values{Value(texto), Value(valor)});
}
}  // namespace Save

namespace Scene {
/// Scene.destroy(entity)
/// lo destruye al final del frame
template <typename T0 = Value>
inline Value destroy(const T0& entity = {}) {
    return detail::lua("Scene.destroy", Values{Value(entity)});
}
/// Scene.findAllWithTag("tag")
/// lista de objetos con ese tag
template <typename T0 = Value>
inline Value findAllWithTag(const T0& texto = {}) {
    return detail::lua("Scene.findAllWithTag", Values{Value(texto)});
}
/// Scene.instantiate(entity o "Prefabs/Enemigo", posicion, rotacion)
/// copia de un objeto (con hijos y componentes) o instancia de un prefab
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value instantiate(const T0& arg = {}, const T1& posicion = {}, const T2& rotacion = {}) {
    return detail::lua("Scene.instantiate", Values{Value(arg), Value(posicion), Value(rotacion)});
}
/// Scene.load("Nivel2")
/// cambia de escena al terminar el frame (nombre o ruta del .crscene)
template <typename T0 = Value>
inline Value load(const T0& texto = {}) {
    return detail::lua("Scene.load", Values{Value(texto)});
}
/// Scene.name()
/// nombre de la escena actual
template <typename... Mas>
inline Value name(Mas&&... mas) {
    return detail::lua("Scene.name", Values{Value(std::forward<Mas>(mas))...});
}
/// Scene.origin()
/// x, y, z (doble precision) del origen flotante del mundo
template <typename... Mas>
inline Value origin(Mas&&... mas) {
    return detail::lua("Scene.origin", Values{Value(std::forward<Mas>(mas))...});
}
/// Scene.toAbsolute(posicion)
/// x, y, z absolutos (para guardar posiciones en una partida)
template <typename T0 = Value>
inline Value toAbsolute(const T0& posicion = {}) {
    return detail::lua("Scene.toAbsolute", Values{Value(posicion)});
}
/// Scene.toLocal(x, y, z)
/// Vec3 local de una posicion absoluta guardada
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value toLocal(const T0& x = {}, const T1& y = {}, const T2& z = {}) {
    return detail::lua("Scene.toLocal", Values{Value(x), Value(y), Value(z)});
}
}  // namespace Scene

namespace Screen {
/// Screen.height()
/// alto en pixeles
template <typename... Mas>
inline Value height(Mas&&... mas) {
    return detail::lua("Screen.height", Values{Value(std::forward<Mas>(mas))...});
}
/// Screen.orientation()
/// "landscape" o "portrait" segun el tamano
template <typename... Mas>
inline Value orientation(Mas&&... mas) {
    return detail::lua("Screen.orientation", Values{Value(std::forward<Mas>(mas))...});
}
/// Screen.orientationMode()
/// el ultimo modo pedido
template <typename... Mas>
inline Value orientationMode(Mas&&... mas) {
    return detail::lua("Screen.orientationMode", Values{Value(std::forward<Mas>(mas))...});
}
/// Screen.setOrientation("landscape")
/// auto, landscape, portrait, landscape_fixed, portrait_fixed (moviles)
template <typename T0 = Value>
inline Value setOrientation(const T0& texto = {}) {
    return detail::lua("Screen.setOrientation", Values{Value(texto)});
}
/// Screen.width()
/// ancho en pixeles
template <typename... Mas>
inline Value width(Mas&&... mas) {
    return detail::lua("Screen.width", Values{Value(std::forward<Mas>(mas))...});
}
}  // namespace Screen

namespace Spline {
/// Spline.create({Vec3(0,0,0), Vec3(0,0,20)}, "road", "Camino", cerrada)
/// crea una spline (road, path, river, wall, fence, pipe, rails, ribbon o nada)
template <typename T0 = Value, typename T1 = Value, typename T2 = Value, typename T3 = Value>
inline Value create(const T0& tabla = {}, const T1& texto = {}, const T2& texto2 = {}, const T3& cerrada = {}) {
    return detail::lua("Spline.create", Values{Value(tabla), Value(texto), Value(texto2), Value(cerrada)});
}
}  // namespace Spline

namespace Steam {
/// Steam.achievementProgress("COLECCIONISTA", 5, 10)
/// muestra el progreso
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value achievementProgress(const T0& texto = {}, const T1& valor = {}, const T2& valor2 = {}) {
    return detail::lua("Steam.achievementProgress", Values{Value(texto), Value(valor), Value(valor2)});
}
/// Steam.appId()
/// AppID
template <typename... Mas>
inline Value appId(Mas&&... mas) {
    return detail::lua("Steam.appId", Values{Value(std::forward<Mas>(mas))...});
}
/// Steam.available()
/// Steam esta abierto y la DLL cargada
template <typename... Mas>
inline Value available(Mas&&... mas) {
    return detail::lua("Steam.available", Values{Value(std::forward<Mas>(mas))...});
}
/// Steam.clearAchievement("PRIMERA_SANGRE")
/// lo vuelve a bloquear (pruebas)
template <typename T0 = Value>
inline Value clearAchievement(const T0& texto = {}) {
    return detail::lua("Steam.clearAchievement", Values{Value(texto)});
}
/// Steam.clearRichPresence()
/// lo borra
template <typename... Mas>
inline Value clearRichPresence(Mas&&... mas) {
    return detail::lua("Steam.clearRichPresence", Values{Value(std::forward<Mas>(mas))...});
}
/// Steam.cloudDelete("partida.json")
/// lo borra
template <typename T0 = Value>
inline Value cloudDelete(const T0& texto = {}) {
    return detail::lua("Steam.cloudDelete", Values{Value(texto)});
}
/// Steam.cloudEnabled()
/// Steam Cloud activo?
template <typename... Mas>
inline Value cloudEnabled(Mas&&... mas) {
    return detail::lua("Steam.cloudEnabled", Values{Value(std::forward<Mas>(mas))...});
}
/// Steam.cloudExists("partida.json")
/// existe?
template <typename T0 = Value>
inline Value cloudExists(const T0& texto = {}) {
    return detail::lua("Steam.cloudExists", Values{Value(texto)});
}
/// Steam.cloudFiles()
/// {name, size} de cada archivo
template <typename... Mas>
inline Value cloudFiles(Mas&&... mas) {
    return detail::lua("Steam.cloudFiles", Values{Value(std::forward<Mas>(mas))...});
}
/// Steam.cloudRead("partida.json")
/// lee de la nube (o nil)
template <typename T0 = Value>
inline Value cloudRead(const T0& texto = {}) {
    return detail::lua("Steam.cloudRead", Values{Value(texto)});
}
/// Steam.cloudWrite("partida.json", texto)
/// guarda en la nube
template <typename T0 = Value, typename T1 = Value>
inline Value cloudWrite(const T0& texto = {}, const T1& texto2 = {}) {
    return detail::lua("Steam.cloudWrite", Values{Value(texto), Value(texto2)});
}
/// Steam.createLobby("public", 4, function(ok, sala) end)
/// crea una sala
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value createLobby(const T0& texto = {}, const T1& valor = {}, const T2& callback = {}) {
    return detail::lua("Steam.createLobby", Values{Value(texto), Value(valor), Value(callback)});
}
/// Steam.downloadScores("Puntos", "global", 1, 10, function(ok, filas) end)
/// lee el marcador
template <typename T0 = Value, typename T1 = Value, typename T2 = Value, typename T3 = Value, typename T4 = Value>
inline Value downloadScores(const T0& texto = {}, const T1& texto2 = {}, const T2& valor = {}, const T3& valor2 = {}, const T4& callback = {}) {
    return detail::lua("Steam.downloadScores", Values{Value(texto), Value(texto2), Value(valor), Value(valor2), Value(callback)});
}
/// Steam.error()
/// por que no lo esta
template <typename... Mas>
inline Value error(Mas&&... mas) {
    return detail::lua("Steam.error", Values{Value(std::forward<Mas>(mas))...});
}
/// Steam.findLobbies({modo = "coop"}, 20, function(ok, salas) end)
/// busca salas
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value findLobbies(const T0& tabla = {}, const T1& valor = {}, const T2& callback = {}) {
    return detail::lua("Steam.findLobbies", Values{Value(tabla), Value(valor), Value(callback)});
}
/// Steam.friendName(id)
/// nombre de un amigo
template <typename T0 = Value>
inline Value friendName(const T0& id = {}) {
    return detail::lua("Steam.friendName", Values{Value(id)});
}
/// Steam.getLobbyData(sala, "ip")
/// lo lee
template <typename T0 = Value, typename T1 = Value>
inline Value getLobbyData(const T0& sala = {}, const T1& texto = {}) {
    return detail::lua("Steam.getLobbyData", Values{Value(sala), Value(texto)});
}
/// Steam.getStatFloat("km")
/// lee una estadistica
template <typename T0 = Value>
inline Value getStatFloat(const T0& texto = {}) {
    return detail::lua("Steam.getStatFloat", Values{Value(texto)});
}
/// Steam.getStatInt("partidas")
/// lee una estadistica
template <typename T0 = Value>
inline Value getStatInt(const T0& texto = {}) {
    return detail::lua("Steam.getStatInt", Values{Value(texto)});
}
/// Steam.inviteToLobby(sala)
/// dialogo de invitar del overlay
template <typename T0 = Value>
inline Value inviteToLobby(const T0& sala = {}) {
    return detail::lua("Steam.inviteToLobby", Values{Value(sala)});
}
/// Steam.isAchievementUnlocked("PRIMERA_SANGRE")
/// esta desbloqueado?
template <typename T0 = Value>
inline Value isAchievementUnlocked(const T0& texto = {}) {
    return detail::lua("Steam.isAchievementUnlocked", Values{Value(texto)});
}
/// Steam.isDlcInstalled(appId)
/// tiene el DLC?
template <typename T0 = Value>
inline Value isDlcInstalled(const T0& appId = {}) {
    return detail::lua("Steam.isDlcInstalled", Values{Value(appId)});
}
/// Steam.isSteamDeck()
/// corre en una Steam Deck?
template <typename... Mas>
inline Value isSteamDeck(Mas&&... mas) {
    return detail::lua("Steam.isSteamDeck", Values{Value(std::forward<Mas>(mas))...});
}
/// Steam.joinLobby(sala, function(ok, sala) end)
/// entra en una sala
template <typename T0 = Value, typename T1 = Value>
inline Value joinLobby(const T0& sala = {}, const T1& callback = {}) {
    return detail::lua("Steam.joinLobby", Values{Value(sala), Value(callback)});
}
/// Steam.language()
/// idioma de Steam
template <typename... Mas>
inline Value language(Mas&&... mas) {
    return detail::lua("Steam.language", Values{Value(std::forward<Mas>(mas))...});
}
/// Steam.leaveLobby(sala)
/// sale
template <typename T0 = Value>
inline Value leaveLobby(const T0& sala = {}) {
    return detail::lua("Steam.leaveLobby", Values{Value(sala)});
}
/// Steam.lobbyMembers(sala)
/// {id, name} de cada jugador
template <typename T0 = Value>
inline Value lobbyMembers(const T0& sala = {}) {
    return detail::lua("Steam.lobbyMembers", Values{Value(sala)});
}
/// Steam.lobbyOwner(sala)
/// SteamID del dueno
template <typename T0 = Value>
inline Value lobbyOwner(const T0& sala = {}) {
    return detail::lua("Steam.lobbyOwner", Values{Value(sala)});
}
/// Steam.onLobbyJoinRequested(function(sala) end)
/// un amigo invito y el jugador acepto
template <typename T0 = Value>
inline Value onLobbyJoinRequested(const T0& callback = {}) {
    return detail::lua("Steam.onLobbyJoinRequested", Values{Value(callback)});
}
/// Steam.onOverlay(function(abierto) end)
/// se abrio o cerro el overlay (pausar)
template <typename T0 = Value>
inline Value onOverlay(const T0& callback = {}) {
    return detail::lua("Steam.onOverlay", Values{Value(callback)});
}
/// Steam.openOverlay("friends")
/// abre el overlay
template <typename T0 = Value>
inline Value openOverlay(const T0& texto = {}) {
    return detail::lua("Steam.openOverlay", Values{Value(texto)});
}
/// Steam.openOverlayUrl("https://...")
/// web en el overlay
template <typename T0 = Value>
inline Value openOverlayUrl(const T0& texto = {}) {
    return detail::lua("Steam.openOverlayUrl", Values{Value(texto)});
}
/// Steam.openStore()
/// la pagina de la tienda
template <typename... Mas>
inline Value openStore(Mas&&... mas) {
    return detail::lua("Steam.openStore", Values{Value(std::forward<Mas>(mas))...});
}
/// Steam.overlayActive()
/// esta abierto ahora?
template <typename... Mas>
inline Value overlayActive(Mas&&... mas) {
    return detail::lua("Steam.overlayActive", Values{Value(std::forward<Mas>(mas))...});
}
/// Steam.overlayEnabled()
/// el overlay funciona?
template <typename... Mas>
inline Value overlayEnabled(Mas&&... mas) {
    return detail::lua("Steam.overlayEnabled", Values{Value(std::forward<Mas>(mas))...});
}
/// Steam.setLobbyData(sala, "ip", "1.2.3.4:7777")
/// dato de la sala
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value setLobbyData(const T0& sala = {}, const T1& texto = {}, const T2& texto2 = {}) {
    return detail::lua("Steam.setLobbyData", Values{Value(sala), Value(texto), Value(texto2)});
}
/// Steam.setRichPresence("steam_display", "#Jugando")
/// estado que ven los amigos
template <typename T0 = Value, typename T1 = Value>
inline Value setRichPresence(const T0& texto = {}, const T1& texto2 = {}) {
    return detail::lua("Steam.setRichPresence", Values{Value(texto), Value(texto2)});
}
/// Steam.setStatFloat("km", 4.5)
/// estadistica decimal
template <typename T0 = Value, typename T1 = Value>
inline Value setStatFloat(const T0& texto = {}, const T1& valor = {}) {
    return detail::lua("Steam.setStatFloat", Values{Value(texto), Value(valor)});
}
/// Steam.setStatInt("partidas", 3)
/// estadistica entera
template <typename T0 = Value, typename T1 = Value>
inline Value setStatInt(const T0& texto = {}, const T1& valor = {}) {
    return detail::lua("Steam.setStatInt", Values{Value(texto), Value(valor)});
}
/// Steam.storeStats()
/// envia logros y estadisticas a Steam
template <typename... Mas>
inline Value storeStats(Mas&&... mas) {
    return detail::lua("Steam.storeStats", Values{Value(std::forward<Mas>(mas))...});
}
/// Steam.unlockAchievement("PRIMERA_SANGRE")
/// desbloquea un logro
template <typename T0 = Value>
inline Value unlockAchievement(const T0& texto = {}) {
    return detail::lua("Steam.unlockAchievement", Values{Value(texto)});
}
/// Steam.uploadScore("Puntos", 1200, function(ok, puesto) end)
/// sube una puntuacion al marcador
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value uploadScore(const T0& texto = {}, const T1& valor = {}, const T2& callback = {}) {
    return detail::lua("Steam.uploadScore", Values{Value(texto), Value(valor), Value(callback)});
}
/// Steam.userId()
/// SteamID del jugador
template <typename... Mas>
inline Value userId(Mas&&... mas) {
    return detail::lua("Steam.userId", Values{Value(std::forward<Mas>(mas))...});
}
/// Steam.userName()
/// nombre del jugador
template <typename... Mas>
inline Value userName(Mas&&... mas) {
    return detail::lua("Steam.userName", Values{Value(std::forward<Mas>(mas))...});
}
/// Steam.workshopItems()
/// objetos del Workshop suscritos
template <typename... Mas>
inline Value workshopItems(Mas&&... mas) {
    return detail::lua("Steam.workshopItems", Values{Value(std::forward<Mas>(mas))...});
}
/// Steam.workshopProgress()
/// 0..1 de la subida
template <typename... Mas>
inline Value workshopProgress(Mas&&... mas) {
    return detail::lua("Steam.workshopProgress", Values{Value(std::forward<Mas>(mas))...});
}
/// Steam.workshopUpload({title, description, folder, preview, tags}, function(ok, id) end)
/// sube al Workshop
template <typename T0 = Value, typename T1 = Value>
inline Value workshopUpload(const T0& tabla = {}, const T1& callback = {}) {
    return detail::lua("Steam.workshopUpload", Values{Value(tabla), Value(callback)});
}
}  // namespace Steam

namespace Test {
/// Test.case("nombre", {play = true, timeout = 10}, function() end)
/// una prueba (de edicion o de Play)
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value case_(const T0& texto = {}, const T1& tabla = {}, const T2& callback = {}) {
    return detail::lua("Test.case", Values{Value(texto), Value(tabla), Value(callback)});
}
/// Test.elapsed()
/// segundos que lleva
template <typename... Mas>
inline Value elapsed(Mas&&... mas) {
    return detail::lua("Test.elapsed", Values{Value(std::forward<Mas>(mas))...});
}
/// Test.fail("mensaje")
/// la da por mala
template <typename T0 = Value>
inline Value fail(const T0& texto = {}) {
    return detail::lua("Test.fail", Values{Value(texto)});
}
/// Test.isRunning()
/// hay una prueba en marcha?
template <typename... Mas>
inline Value isRunning(Mas&&... mas) {
    return detail::lua("Test.isRunning", Values{Value(std::forward<Mas>(mas))...});
}
/// Test.log("texto")
/// texto en el resultado
template <typename T0 = Value>
inline Value log(const T0& texto = {}) {
    return detail::lua("Test.log", Values{Value(texto)});
}
/// Test.name()
/// nombre de la prueba
template <typename... Mas>
inline Value name(Mas&&... mas) {
    return detail::lua("Test.name", Values{Value(std::forward<Mas>(mas))...});
}
/// Test.pass("mensaje")
/// da la prueba por buena
template <typename T0 = Value>
inline Value pass(const T0& texto = {}) {
    return detail::lua("Test.pass", Values{Value(texto)});
}
/// Test.setFrames(...)
template <typename... Mas>
inline Value setFrames(Mas&&... mas) {
    return detail::lua("Test.setFrames", Values{Value(std::forward<Mas>(mas))...});
}
/// Test.setTimeout(segundos)
/// tiempo maximo
template <typename T0 = Value>
inline Value setTimeout(const T0& segundos = {}) {
    return detail::lua("Test.setTimeout", Values{Value(segundos)});
}
/// Test.wait(frames)
/// espera frames (pruebas de Play)
template <typename T0 = Value>
inline Value wait(const T0& frames = {}) {
    return detail::lua("Test.wait", Values{Value(frames)});
}
/// Test.waitSeconds(1.5)
/// espera segundos
template <typename T0 = Value>
inline Value waitSeconds(const T0& valor = {}) {
    return detail::lua("Test.waitSeconds", Values{Value(valor)});
}
/// Test.waitUntil(function() return x end, limite)
/// espera a que se cumpla (falla al pasar el limite)
template <typename T0 = Value, typename T1 = Value>
inline Value waitUntil(const T0& callback = {}, const T1& limite = {}) {
    return detail::lua("Test.waitUntil", Values{Value(callback), Value(limite)});
}
}  // namespace Test

namespace Text {
/// Text.format("Hola {0}", nombre)
/// sustituye {0}, {1}, {nombre}
template <typename T0 = Value, typename T1 = Value>
inline Value format(const T0& texto = {}, const T1& nombre = {}) {
    return detail::lua("Text.format", Values{Value(texto), Value(nombre)});
}
/// Text.get("menu.jugar", ...)
/// texto en el idioma actual ({0}, {1}... con los argumentos)
template <typename T0 = Value, typename... Mas>
inline Value get(const T0& texto = {}, Mas&&... mas) {
    return detail::lua("Text.get", Values{Value(texto), Value(std::forward<Mas>(mas))...});
}
/// Text.has("clave")
/// existe la clave?
template <typename T0 = Value>
inline Value has(const T0& texto = {}) {
    return detail::lua("Text.has", Values{Value(texto)});
}
/// Text.language()
/// idioma actual ("es")
template <typename... Mas>
inline Value language(Mas&&... mas) {
    return detail::lua("Text.language", Values{Value(std::forward<Mas>(mas))...});
}
/// Text.languages()
/// {code, name} de cada idioma
template <typename... Mas>
inline Value languages(Mas&&... mas) {
    return detail::lua("Text.languages", Values{Value(std::forward<Mas>(mas))...});
}
/// Text.onLanguageChanged(function(codigo) end)
/// aviso al cambiar de idioma (devuelve un id)
template <typename T0 = Value>
inline Value onLanguageChanged(const T0& callback = {}) {
    return detail::lua("Text.onLanguageChanged", Values{Value(callback)});
}
/// Text.plural("monedas", n, ...)
/// forma plural (clave#one / clave#other) con {n}
template <typename T0 = Value, typename T1 = Value, typename... Mas>
inline Value plural(const T0& texto = {}, const T1& n = {}, Mas&&... mas) {
    return detail::lua("Text.plural", Values{Value(texto), Value(n), Value(std::forward<Mas>(mas))...});
}
/// Text.removeListener(id)
/// quita un aviso
template <typename T0 = Value>
inline Value removeListener(const T0& id = {}) {
    return detail::lua("Text.removeListener", Values{Value(id)});
}
/// Text.setLanguage("en")
/// cambia el idioma (la UI se actualiza sola)
template <typename T0 = Value>
inline Value setLanguage(const T0& texto = {}) {
    return detail::lua("Text.setLanguage", Values{Value(texto)});
}
/// Text.strip("<b>Hola</b>")
/// el texto sin las etiquetas del texto enriquecido
template <typename T0 = Value>
inline Value strip(const T0& texto = {}) {
    return detail::lua("Text.strip", Values{Value(texto)});
}
/// Text.systemLanguage()
/// idioma del sistema
template <typename... Mas>
inline Value systemLanguage(Mas&&... mas) {
    return detail::lua("Text.systemLanguage", Values{Value(std::forward<Mas>(mas))...});
}
}  // namespace Text

namespace Voice {
/// Voice.isMuted(id)
template <typename T0 = Value>
inline Value isMuted(const T0& id = {}) {
    return detail::lua("Voice.isMuted", Values{Value(id)});
}
/// Voice.isSpeaking(id)
/// habla ahora? (sin id = yo)
template <typename T0 = Value>
inline Value isSpeaking(const T0& id = {}) {
    return detail::lua("Voice.isSpeaking", Values{Value(id)});
}
/// Voice.micLevel()
/// 0..1 del microfono
template <typename... Mas>
inline Value micLevel(Mas&&... mas) {
    return detail::lua("Voice.micLevel", Values{Value(std::forward<Mas>(mas))...});
}
/// Voice.setMicGain(1)
/// ganancia del microfono
template <typename T0 = Value>
inline Value setMicGain(const T0& valor = {}) {
    return detail::lua("Voice.setMicGain", Values{Value(valor)});
}
/// Voice.setMode("push")
/// push (pulsar para hablar), open (por voz) u off
template <typename T0 = Value>
inline Value setMode(const T0& texto = {}) {
    return detail::lua("Voice.setMode", Values{Value(texto)});
}
/// Voice.setMuted(id, true)
/// silenciar a un jugador
template <typename T0 = Value, typename T1 = Value>
inline Value setMuted(const T0& id = {}, const T1& activar = {}) {
    return detail::lua("Voice.setMuted", Values{Value(id), Value(activar)});
}
/// Voice.setProximity(30)
/// volumen por distancia (0 = todos igual)
template <typename T0 = Value>
inline Value setProximity(const T0& valor = {}) {
    return detail::lua("Voice.setProximity", Values{Value(valor)});
}
/// Voice.setTalking(true)
/// modo push: hablando
template <typename T0 = Value>
inline Value setTalking(const T0& activar = {}) {
    return detail::lua("Voice.setTalking", Values{Value(activar)});
}
/// Voice.setThreshold(0.02)
/// modo open: volumen minimo
template <typename T0 = Value>
inline Value setThreshold(const T0& valor = {}) {
    return detail::lua("Voice.setThreshold", Values{Value(valor)});
}
/// Voice.setVolume(1)
/// volumen de los demas
template <typename T0 = Value>
inline Value setVolume(const T0& valor = {}) {
    return detail::lua("Voice.setVolume", Values{Value(valor)});
}
/// Voice.start()
/// abre el microfono y la salida
template <typename... Mas>
inline Value start(Mas&&... mas) {
    return detail::lua("Voice.start", Values{Value(std::forward<Mas>(mas))...});
}
/// Voice.stop()
/// los cierra
template <typename... Mas>
inline Value stop(Mas&&... mas) {
    return detail::lua("Voice.stop", Values{Value(std::forward<Mas>(mas))...});
}
}  // namespace Voice

namespace Voxel {
/// Voxel.blockColor("stone")
/// color medio del bloque (Vec3)
template <typename T0 = Value>
inline Value blockColor(const T0& texto = {}) {
    return detail::lua("Voxel.blockColor", Values{Value(texto)});
}
/// Voxel.blockCount()
/// cuantos tipos de bloque hay
template <typename... Mas>
inline Value blockCount(Mas&&... mas) {
    return detail::lua("Voxel.blockCount", Values{Value(std::forward<Mas>(mas))...});
}
/// Voxel.blockId("stone")
/// numero de un bloque
template <typename T0 = Value>
inline Value blockId(const T0& texto = {}) {
    return detail::lua("Voxel.blockId", Values{Value(texto)});
}
/// Voxel.blockInfo("stone")
/// {name, label, solid, hardness...}
template <typename T0 = Value>
inline Value blockInfo(const T0& texto = {}) {
    return detail::lua("Voxel.blockInfo", Values{Value(texto)});
}
/// Voxel.blockLight(x, y, z)
/// luz de antorchas 0..15
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value blockLight(const T0& x = {}, const T1& y = {}, const T2& z = {}) {
    return detail::lua("Voxel.blockLight", Values{Value(x), Value(y), Value(z)});
}
/// Voxel.boxCollides(centro, semiejes)
/// toca bloques?
template <typename T0 = Value, typename T1 = Value>
inline Value boxCollides(const T0& centro = {}, const T1& semiejes = {}) {
    return detail::lua("Voxel.boxCollides", Values{Value(centro), Value(semiejes)});
}
/// Voxel.deleteWorld("nombre")
/// lo borra
template <typename T0 = Value>
inline Value deleteWorld(const T0& texto = {}) {
    return detail::lua("Voxel.deleteWorld", Values{Value(texto)});
}
/// Voxel.getBlock(x, y, z)
/// numero del bloque (0 = aire)
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value getBlock(const T0& x = {}, const T1& y = {}, const T2& z = {}) {
    return detail::lua("Voxel.getBlock", Values{Value(x), Value(y), Value(z)});
}
/// Voxel.getBlockAt(Vec3)
/// bloque en ese punto
template <typename T0 = Value>
inline Value getBlockAt(const T0& vec = {}) {
    return detail::lua("Voxel.getBlockAt", Values{Value(vec)});
}
/// Voxel.getMeta("clave", "")
/// lee un dato
template <typename T0 = Value, typename T1 = Value>
inline Value getMeta(const T0& texto = {}, const T1& texto2 = {}) {
    return detail::lua("Voxel.getMeta", Values{Value(texto), Value(texto2)});
}
/// Voxel.inWater(Vec3)
/// esta en el agua?
template <typename T0 = Value>
inline Value inWater(const T0& vec = {}) {
    return detail::lua("Voxel.inWater", Values{Value(vec)});
}
/// Voxel.isActive()
/// hay mundo de bloques?
template <typename... Mas>
inline Value isActive(Mas&&... mas) {
    return detail::lua("Voxel.isActive", Values{Value(std::forward<Mas>(mas))...});
}
/// Voxel.isReady(Vec3)
/// ya esta generado?
template <typename T0 = Value>
inline Value isReady(const T0& vec = {}) {
    return detail::lua("Voxel.isReady", Values{Value(vec)});
}
/// Voxel.listWorlds()
/// lista de mundos
template <typename... Mas>
inline Value listWorlds(Mas&&... mas) {
    return detail::lua("Voxel.listWorlds", Values{Value(std::forward<Mas>(mas))...});
}
/// Voxel.loadWorld("nombre")
/// carga un mundo guardado
template <typename T0 = Value>
inline Value loadWorld(const T0& texto = {}) {
    return detail::lua("Voxel.loadWorld", Values{Value(texto)});
}
/// Voxel.moveBox(centro, semiejes, delta)
/// posicion, enSuelo, techo, pared
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value moveBox(const T0& centro = {}, const T1& semiejes = {}, const T2& delta = {}) {
    return detail::lua("Voxel.moveBox", Values{Value(centro), Value(semiejes), Value(delta)});
}
/// Voxel.newWorld("nombre", semilla)
/// mundo nuevo con nombre
template <typename T0 = Value, typename T1 = Value>
inline Value newWorld(const T0& texto = {}, const T1& semilla = {}) {
    return detail::lua("Voxel.newWorld", Values{Value(texto), Value(semilla)});
}
/// Voxel.raycast(origen, direccion, distancia)
/// nil o {block, normal, id, point, distance}
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value raycast(const T0& origen = {}, const T1& direccion = {}, const T2& distancia = {}) {
    return detail::lua("Voxel.raycast", Values{Value(origen), Value(direccion), Value(distancia)});
}
/// Voxel.saveWorld()
/// guarda
template <typename... Mas>
inline Value saveWorld(Mas&&... mas) {
    return detail::lua("Voxel.saveWorld", Values{Value(std::forward<Mas>(mas))...});
}
/// Voxel.seed()
/// semilla
template <typename... Mas>
inline Value seed(Mas&&... mas) {
    return detail::lua("Voxel.seed", Values{Value(std::forward<Mas>(mas))...});
}
/// Voxel.setBlock(x, y, z, "stone")
/// pone o quita un bloque
template <typename T0 = Value, typename T1 = Value, typename T2 = Value, typename T3 = Value>
inline Value setBlock(const T0& x = {}, const T1& y = {}, const T2& z = {}, const T3& texto = {}) {
    return detail::lua("Voxel.setBlock", Values{Value(x), Value(y), Value(z), Value(texto)});
}
/// Voxel.setMeta("clave", "texto")
/// dato guardado con el mundo
template <typename T0 = Value, typename T1 = Value>
inline Value setMeta(const T0& texto = {}, const T1& texto2 = {}) {
    return detail::lua("Voxel.setMeta", Values{Value(texto), Value(texto2)});
}
/// Voxel.skyLight(x, y, z)
/// luz del cielo 0..15
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value skyLight(const T0& x = {}, const T1& y = {}, const T2& z = {}) {
    return detail::lua("Voxel.skyLight", Values{Value(x), Value(y), Value(z)});
}
/// Voxel.surfaceHeight(x, z)
/// altura del terreno
template <typename T0 = Value, typename T1 = Value>
inline Value surfaceHeight(const T0& x = {}, const T1& z = {}) {
    return detail::lua("Voxel.surfaceHeight", Values{Value(x), Value(z)});
}
/// Voxel.worldName()
/// nombre del mundo
template <typename... Mas>
inline Value worldName(Mas&&... mas) {
    return detail::lua("Voxel.worldName", Values{Value(std::forward<Mas>(mas))...});
}
}  // namespace Voxel

namespace Weather {
/// Weather.get()
/// clima actual ("Storm")
template <typename... Mas>
inline Value get(Mas&&... mas) {
    return detail::lua("Weather.get", Values{Value(std::forward<Mas>(mas))...});
}
/// Weather.getDate()
/// dia, mes
template <typename... Mas>
inline Value getDate(Mas&&... mas) {
    return detail::lua("Weather.getDate", Values{Value(std::forward<Mas>(mas))...});
}
/// Weather.getFog()
/// densidad de la niebla
template <typename... Mas>
inline Value getFog(Mas&&... mas) {
    return detail::lua("Weather.getFog", Values{Value(std::forward<Mas>(mas))...});
}
/// Weather.getLabel()
/// nombre visible ("Tormenta")
template <typename... Mas>
inline Value getLabel(Mas&&... mas) {
    return detail::lua("Weather.getLabel", Values{Value(std::forward<Mas>(mas))...});
}
/// Weather.getLatitude()
/// latitud
template <typename... Mas>
inline Value getLatitude(Mas&&... mas) {
    return detail::lua("Weather.getLatitude", Values{Value(std::forward<Mas>(mas))...});
}
/// Weather.getRain()
/// lluvia 0..1
template <typename... Mas>
inline Value getRain(Mas&&... mas) {
    return detail::lua("Weather.getRain", Values{Value(std::forward<Mas>(mas))...});
}
/// Weather.getSeason()
/// estacion actual
template <typename... Mas>
inline Value getSeason(Mas&&... mas) {
    return detail::lua("Weather.getSeason", Values{Value(std::forward<Mas>(mas))...});
}
/// Weather.getSnow()
/// nevada 0..1
template <typename... Mas>
inline Value getSnow(Mas&&... mas) {
    return detail::lua("Weather.getSnow", Values{Value(std::forward<Mas>(mas))...});
}
/// Weather.getSnowCover()
/// nieve acumulada 0..1
template <typename... Mas>
inline Value getSnowCover(Mas&&... mas) {
    return detail::lua("Weather.getSnowCover", Values{Value(std::forward<Mas>(mas))...});
}
/// Weather.getSunDirection()
/// Vec3 hacia el sol
template <typename... Mas>
inline Value getSunDirection(Mas&&... mas) {
    return detail::lua("Weather.getSunDirection", Values{Value(std::forward<Mas>(mas))...});
}
/// Weather.getTarget()
/// clima al que va la transicion
template <typename... Mas>
inline Value getTarget(Mas&&... mas) {
    return detail::lua("Weather.getTarget", Values{Value(std::forward<Mas>(mas))...});
}
/// Weather.getTemperature()
/// grados C
template <typename... Mas>
inline Value getTemperature(Mas&&... mas) {
    return detail::lua("Weather.getTemperature", Values{Value(std::forward<Mas>(mas))...});
}
/// Weather.getTime()
/// hora del dia
template <typename... Mas>
inline Value getTime(Mas&&... mas) {
    return detail::lua("Weather.getTime", Values{Value(std::forward<Mas>(mas))...});
}
/// Weather.getTimeScale()
/// velocidad del tiempo
template <typename... Mas>
inline Value getTimeScale(Mas&&... mas) {
    return detail::lua("Weather.getTimeScale", Values{Value(std::forward<Mas>(mas))...});
}
/// Weather.getWeather(...)
template <typename... Mas>
inline Value getWeather(Mas&&... mas) {
    return detail::lua("Weather.getWeather", Values{Value(std::forward<Mas>(mas))...});
}
/// Weather.getWetness()
/// humedad de las superficies 0..1
template <typename... Mas>
inline Value getWetness(Mas&&... mas) {
    return detail::lua("Weather.getWetness", Values{Value(std::forward<Mas>(mas))...});
}
/// Weather.getWind()
/// Vec3 del viento (m/s)
template <typename... Mas>
inline Value getWind(Mas&&... mas) {
    return detail::lua("Weather.getWind", Values{Value(std::forward<Mas>(mas))...});
}
/// Weather.getWindDirection()
/// grados
template <typename... Mas>
inline Value getWindDirection(Mas&&... mas) {
    return detail::lua("Weather.getWindDirection", Values{Value(std::forward<Mas>(mas))...});
}
/// Weather.getWindSpeed()
/// m/s con rachas
template <typename... Mas>
inline Value getWindSpeed(Mas&&... mas) {
    return detail::lua("Weather.getWindSpeed", Values{Value(std::forward<Mas>(mas))...});
}
/// Weather.isNight()
/// el sol esta bajo el horizonte?
template <typename... Mas>
inline Value isNight(Mas&&... mas) {
    return detail::lua("Weather.isNight", Values{Value(std::forward<Mas>(mas))...});
}
/// Weather.isTransitioning()
/// esta cambiando?
template <typename... Mas>
inline Value isTransitioning(Mas&&... mas) {
    return detail::lua("Weather.isTransitioning", Values{Value(std::forward<Mas>(mas))...});
}
/// Weather.lightning(800)
/// un rayo ya (distancia en m; sin ella al azar)
template <typename T0 = Value>
inline Value lightning(const T0& valor = {}) {
    return detail::lua("Weather.lightning", Values{Value(valor)});
}
/// Weather.presets()
/// lista de climas
template <typename... Mas>
inline Value presets(Mas&&... mas) {
    return detail::lua("Weather.presets", Values{Value(std::forward<Mas>(mas))...});
}
/// Weather.set("Storm", 10)
/// cambia de clima en N segundos (Clear, Cloudy, Overcast, Foggy, LightRain, Rain, Storm, LightSnow, Snow, Blizzard, Sandstorm)
template <typename T0 = Value, typename T1 = Value>
inline Value set(const T0& texto = {}, const T1& valor = {}) {
    return detail::lua("Weather.set", Values{Value(texto), Value(valor)});
}
/// Weather.setAudio(true, 0.8)
/// sonido de lluvia, viento y truenos
template <typename T0 = Value, typename T1 = Value>
inline Value setAudio(const T0& activar = {}, const T1& valor = {}) {
    return detail::lua("Weather.setAudio", Values{Value(activar), Value(valor)});
}
/// Weather.setDate(21, 12)
/// dia y mes (mueve el sol y la estacion)
template <typename T0 = Value, typename T1 = Value>
inline Value setDate(const T0& valor = {}, const T1& valor2 = {}) {
    return detail::lua("Weather.setDate", Values{Value(valor), Value(valor2)});
}
/// Weather.setDayLength(24)
/// minutos reales por dia (nil = el tiempo se para)
template <typename T0 = Value>
inline Value setDayLength(const T0& valor = {}) {
    return detail::lua("Weather.setDayLength", Values{Value(valor)});
}
/// Weather.setLatitude(40)
/// latitud en grados
template <typename T0 = Value>
inline Value setLatitude(const T0& valor = {}) {
    return detail::lua("Weather.setLatitude", Values{Value(valor)});
}
/// Weather.setLightning(true, 2)
/// rayos en las tormentas y su frecuencia
template <typename T0 = Value, typename T1 = Value>
inline Value setLightning(const T0& activar = {}, const T1& valor = {}) {
    return detail::lua("Weather.setLightning", Values{Value(activar), Value(valor)});
}
/// Weather.setPrecipitationDensity(0.5)
/// menos gotas (rendimiento)
template <typename T0 = Value>
inline Value setPrecipitationDensity(const T0& valor = {}) {
    return detail::lua("Weather.setPrecipitationDensity", Values{Value(valor)});
}
/// Weather.setRandom(true, 120, 360)
/// clima al azar (segundos min y max)
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value setRandom(const T0& activar = {}, const T1& valor = {}, const T2& valor2 = {}) {
    return detail::lua("Weather.setRandom", Values{Value(activar), Value(valor), Value(valor2)});
}
/// Weather.setSeason("Winter")
/// estacion fija ("auto" = por la fecha)
template <typename T0 = Value>
inline Value setSeason(const T0& texto = {}) {
    return detail::lua("Weather.setSeason", Values{Value(texto)});
}
/// Weather.setSnowCover(1)
/// nieve acumulada al instante
template <typename T0 = Value>
inline Value setSnowCover(const T0& valor = {}) {
    return detail::lua("Weather.setSnowCover", Values{Value(valor)});
}
/// Weather.setTime(18.5)
/// hora del dia (0..24)
template <typename T0 = Value>
inline Value setTime(const T0& valor = {}) {
    return detail::lua("Weather.setTime", Values{Value(valor)});
}
/// Weather.setTimeScale(60)
/// velocidad del tiempo (1 = real, 0 = parado)
template <typename T0 = Value>
inline Value setTimeScale(const T0& valor = {}) {
    return detail::lua("Weather.setTimeScale", Values{Value(valor)});
}
/// Weather.setWeather("Rain", 5)
/// lo mismo que set
template <typename T0 = Value, typename T1 = Value>
inline Value setWeather(const T0& texto = {}, const T1& valor = {}) {
    return detail::lua("Weather.setWeather", Values{Value(texto), Value(valor)});
}
/// Weather.setWetness(1, 0.6)
/// humedad y charcos al instante
template <typename T0 = Value, typename T1 = Value>
inline Value setWetness(const T0& valor = {}, const T1& valor2 = {}) {
    return detail::lua("Weather.setWetness", Values{Value(valor), Value(valor2)});
}
/// Weather.setWind(90, 1.5)
/// direccion (grados) y fuerza del viento
template <typename T0 = Value, typename T1 = Value>
inline Value setWind(const T0& valor = {}, const T1& valor2 = {}) {
    return detail::lua("Weather.setWind", Values{Value(valor), Value(valor2)});
}
/// Weather.transition()
/// 0..1 lo que lleva la transicion
template <typename... Mas>
inline Value transition(Mas&&... mas) {
    return detail::lua("Weather.transition", Values{Value(std::forward<Mas>(mas))...});
}
}  // namespace Weather

namespace WorldPartition {
/// WorldPartition.active()
/// esta repartiendo el mundo en celdas?
template <typename... Mas>
inline Value active(Mas&&... mas) {
    return detail::lua("WorldPartition.active", Values{Value(std::forward<Mas>(mas))...});
}
/// WorldPartition.isLoaded(posicion)
/// la celda de ese punto esta cargada?
template <typename T0 = Value>
inline Value isLoaded(const T0& posicion = {}) {
    return detail::lua("WorldPartition.isLoaded", Values{Value(posicion)});
}
/// WorldPartition.loadAll()
/// carga todas las celdas ya
template <typename... Mas>
inline Value loadAll(Mas&&... mas) {
    return detail::lua("WorldPartition.loadAll", Values{Value(std::forward<Mas>(mas))...});
}
/// WorldPartition.stats()
/// {cells, loadedCells, objects, unloadedObjects, storedBytes}
template <typename... Mas>
inline Value stats(Mas&&... mas) {
    return detail::lua("WorldPartition.stats", Values{Value(std::forward<Mas>(mas))...});
}
}  // namespace WorldPartition

namespace XR {
/// XR.getAimRay("right")
/// origen, direccion del puntero (para Physics.raycast)
template <typename T0 = Value>
inline Value getAimRay(const T0& texto = {}) {
    return detail::lua("XR.getAimRay", Values{Value(texto)});
}
/// XR.getButton("right", "a")
/// boton mantenido: trigger, grip, thumbstick, primary (a/x), secondary (b/y), menu
template <typename T0 = Value, typename T1 = Value>
inline Value getButton(const T0& texto = {}, const T1& texto2 = {}) {
    return detail::lua("XR.getButton", Values{Value(texto), Value(texto2)});
}
/// XR.getButtonDown("right", "trigger")
/// boton pulsado este frame
template <typename T0 = Value, typename T1 = Value>
inline Value getButtonDown(const T0& texto = {}, const T1& texto2 = {}) {
    return detail::lua("XR.getButtonDown", Values{Value(texto), Value(texto2)});
}
/// XR.getButtonUp("right", "trigger")
/// boton soltado este frame
template <typename T0 = Value, typename T1 = Value>
inline Value getButtonUp(const T0& texto = {}, const T1& texto2 = {}) {
    return detail::lua("XR.getButtonUp", Values{Value(texto), Value(texto2)});
}
/// XR.getControllerPosition("right", "grip")
/// Vec3 de la mano (grip) o del puntero (aim), o nil
template <typename T0 = Value, typename T1 = Value>
inline Value getControllerPosition(const T0& texto = {}, const T1& texto2 = {}) {
    return detail::lua("XR.getControllerPosition", Values{Value(texto), Value(texto2)});
}
/// XR.getControllerRotation("right", "grip")
/// Quat de la mano o del puntero, o nil
template <typename T0 = Value, typename T1 = Value>
inline Value getControllerRotation(const T0& texto = {}, const T1& texto2 = {}) {
    return detail::lua("XR.getControllerRotation", Values{Value(texto), Value(texto2)});
}
/// XR.getGrip("right")
/// agarre 0..1
template <typename T0 = Value>
inline Value getGrip(const T0& texto = {}) {
    return detail::lua("XR.getGrip", Values{Value(texto)});
}
/// XR.getHeadLocalPosition()
/// Vec3 de la cabeza dentro de la habitacion
template <typename... Mas>
inline Value getHeadLocalPosition(Mas&&... mas) {
    return detail::lua("XR.getHeadLocalPosition", Values{Value(std::forward<Mas>(mas))...});
}
/// XR.getHeadPosition()
/// Vec3 de la cabeza en el mundo (o nil)
template <typename... Mas>
inline Value getHeadPosition(Mas&&... mas) {
    return detail::lua("XR.getHeadPosition", Values{Value(std::forward<Mas>(mas))...});
}
/// XR.getHeadRotation()
/// Quat de la cabeza en el mundo (o nil)
template <typename... Mas>
inline Value getHeadRotation(Mas&&... mas) {
    return detail::lua("XR.getHeadRotation", Values{Value(std::forward<Mas>(mas))...});
}
/// XR.getOriginPosition()
/// Vec3 del rig (XR Origin o la camara) en el mundo
template <typename... Mas>
inline Value getOriginPosition(Mas&&... mas) {
    return detail::lua("XR.getOriginPosition", Values{Value(std::forward<Mas>(mas))...});
}
/// XR.getRuntimeName()
/// runtime de OpenXR (SteamVR, Oculus...)
template <typename... Mas>
inline Value getRuntimeName(Mas&&... mas) {
    return detail::lua("XR.getRuntimeName", Values{Value(std::forward<Mas>(mas))...});
}
/// XR.getSystemName()
/// nombre del casco
template <typename... Mas>
inline Value getSystemName(Mas&&... mas) {
    return detail::lua("XR.getSystemName", Values{Value(std::forward<Mas>(mas))...});
}
/// XR.getThumbstick("left")
/// Vec3(x, y, 0) del stick, -1..1
template <typename T0 = Value>
inline Value getThumbstick(const T0& texto = {}) {
    return detail::lua("XR.getThumbstick", Values{Value(texto)});
}
/// XR.getTrackingOrigin()
/// "floor" o "eyes"
template <typename... Mas>
inline Value getTrackingOrigin(Mas&&... mas) {
    return detail::lua("XR.getTrackingOrigin", Values{Value(std::forward<Mas>(mas))...});
}
/// XR.getTrigger("right")
/// gatillo 0..1
template <typename T0 = Value>
inline Value getTrigger(const T0& texto = {}) {
    return detail::lua("XR.getTrigger", Values{Value(texto)});
}
/// XR.isAvailable()
/// hay casco y sesion de VR
template <typename... Mas>
inline Value isAvailable(Mas&&... mas) {
    return detail::lua("XR.isAvailable", Values{Value(std::forward<Mas>(mas))...});
}
/// XR.isControllerActive("right")
/// el mando esta encendido y se sigue
template <typename T0 = Value>
inline Value isControllerActive(const T0& texto = {}) {
    return detail::lua("XR.isControllerActive", Values{Value(texto)});
}
/// XR.isFocused()
/// el juego tiene los mandos (sin el menu del sistema encima)
template <typename... Mas>
inline Value isFocused(Mas&&... mas) {
    return detail::lua("XR.isFocused", Values{Value(std::forward<Mas>(mas))...});
}
/// XR.isRunning()
/// el casco esta mostrando el juego
template <typename... Mas>
inline Value isRunning(Mas&&... mas) {
    return detail::lua("XR.isRunning", Values{Value(std::forward<Mas>(mas))...});
}
/// XR.setTrackingOrigin("floor")
/// floor (de pie) o eyes (sentado); con XR Origin manda el componente
template <typename T0 = Value>
inline Value setTrackingOrigin(const T0& texto = {}) {
    return detail::lua("XR.setTrackingOrigin", Values{Value(texto)});
}
/// XR.vibrate("right", 0.5, 0.1)
/// vibracion: intensidad 0..1, segundos, hz
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value vibrate(const T0& texto = {}, const T1& valor = {}, const T2& valor2 = {}) {
    return detail::lua("XR.vibrate", Values{Value(texto), Value(valor), Value(valor2)});
}
}  // namespace XR

/// Malla creada por codigo (Mesh::cube, Mesh::create, entity().mesh()). Cambiala y llama a apply().
class Mesh {
public:
    Value handle;
    Mesh() = default;
    Mesh(Value v) : handle(std::move(v)) {}
    operator Value() const { return handle; }
    explicit operator bool() const { return handle.truthy(); }
    /// Mesh:apply()
    /// sube los cambios a la GPU
    template <typename... Mas>
    Value apply(Mas&&... mas) const {
        return handle.call("apply", Values{Value(std::forward<Mas>(mas))...});
    }
    /// Mesh.boundsMax: Vec3 esquina maxima
    Value boundsMax() const { return handle.get("boundsMax"); }
    /// Cambia Mesh.boundsMax
    void setBoundsMax(const Value& v) const { handle.setField("boundsMax", v); }
    /// Mesh.boundsMin: Vec3 esquina minima
    Value boundsMin() const { return handle.get("boundsMin"); }
    /// Cambia Mesh.boundsMin
    void setBoundsMin(const Value& v) const { handle.setField("boundsMin", v); }
    /// Mesh.capsule(radio, alto, segmentos)
    /// capsula
    template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
    static Mesh capsule(const T0& radio = {}, const T1& alto = {}, const T2& segmentos = {}) {
        return Mesh(detail::lua("Mesh.capsule", Values{Value(radio), Value(alto), Value(segmentos)}));
    }
    /// Mesh:clear()
    /// la vacia
    template <typename... Mas>
    Value clear(Mas&&... mas) const {
        return handle.call("clear", Values{Value(std::forward<Mas>(mas))...});
    }
    /// Mesh:clone()
    /// copia
    template <typename... Mas>
    Value clone(Mas&&... mas) const {
        return handle.call("clone", Values{Value(std::forward<Mas>(mas))...});
    }
    /// Mesh.cube(tamano)
    /// cubo (numero o Vec3)
    template <typename T0 = Value>
    static Mesh cube(const T0& tamano = {}) {
        return Mesh(detail::lua("Mesh.cube", Values{Value(tamano)}));
    }
    /// Mesh.cylinder(radio, alto, segmentos)
    /// cilindro
    template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
    static Mesh cylinder(const T0& radio = {}, const T1& alto = {}, const T2& segmentos = {}) {
        return Mesh(detail::lua("Mesh.cylinder", Values{Value(radio), Value(alto), Value(segmentos)}));
    }
    /// Mesh:getMaterial(submalla)
    /// su material
    template <typename T0 = Value>
    Value getMaterial(const T0& submalla = {}) const {
        return handle.call("getMaterial", Values{Value(submalla)});
    }
    /// Mesh:getTriangles(submalla)
    /// sus indices
    template <typename T0 = Value>
    Value getTriangles(const T0& submalla = {}) const {
        return handle.call("getTriangles", Values{Value(submalla)});
    }
    /// Mesh:getVertex(i)
    /// Vec3 de un vertice
    template <typename T0 = Value>
    Value getVertex(const T0& i = {}) const {
        return handle.call("getVertex", Values{Value(i)});
    }
    /// Mesh.name: nombre
    Value name() const { return handle.get("name"); }
    /// Cambia Mesh.name
    void setName(const Value& v) const { handle.setField("name", v); }
    /// Mesh.new("nombre")
    /// malla vacia
    template <typename T0 = Value>
    static Mesh create(const T0& texto = {}) {
        return Mesh(detail::lua("Mesh.new", Values{Value(texto)}));
    }
    /// Mesh.normals: lista de Vec3
    Value normals() const { return handle.get("normals"); }
    /// Cambia Mesh.normals
    void setNormals(const Value& v) const { handle.setField("normals", v); }
    /// Mesh.plane(ancho, fondo, segX, segZ)
    /// plano subdividido
    template <typename T0 = Value, typename T1 = Value, typename T2 = Value, typename T3 = Value>
    static Mesh plane(const T0& ancho = {}, const T1& fondo = {}, const T2& segX = {}, const T3& segZ = {}) {
        return Mesh(detail::lua("Mesh.plane", Values{Value(ancho), Value(fondo), Value(segX), Value(segZ)}));
    }
    /// Mesh.quad(ancho, alto)
    /// cuadrado en XY
    template <typename T0 = Value, typename T1 = Value>
    static Mesh quad(const T0& ancho = {}, const T1& alto = {}) {
        return Mesh(detail::lua("Mesh.quad", Values{Value(ancho), Value(alto)}));
    }
    /// Mesh:recalculateBounds()
    /// caja
    template <typename... Mas>
    Value recalculateBounds(Mas&&... mas) const {
        return handle.call("recalculateBounds", Values{Value(std::forward<Mas>(mas))...});
    }
    /// Mesh:recalculateNormals()
    /// normales suaves
    template <typename... Mas>
    Value recalculateNormals(Mas&&... mas) const {
        return handle.call("recalculateNormals", Values{Value(std::forward<Mas>(mas))...});
    }
    /// Mesh:recalculateTangents()
    /// tangentes
    template <typename... Mas>
    Value recalculateTangents(Mas&&... mas) const {
        return handle.call("recalculateTangents", Values{Value(std::forward<Mas>(mas))...});
    }
    /// Mesh:setMaterial(submalla, {color, metallic, roughness...})
    /// material de una submalla
    template <typename T0 = Value, typename T1 = Value>
    Value setMaterial(const T0& submalla = {}, const T1& tabla = {}) const {
        return handle.call("setMaterial", Values{Value(submalla), Value(tabla)});
    }
    /// Mesh:setTriangles(indices, submalla)
    /// triangulos de una submalla
    template <typename T0 = Value, typename T1 = Value>
    Value setTriangles(const T0& indices = {}, const T1& submalla = {}) const {
        return handle.call("setTriangles", Values{Value(indices), Value(submalla)});
    }
    /// Mesh:setVertex(i, Vec3)
    /// mueve un vertice (luego apply)
    template <typename T0 = Value, typename T1 = Value>
    Value setVertex(const T0& i = {}, const T1& vec = {}) const {
        return handle.call("setVertex", Values{Value(i), Value(vec)});
    }
    /// Mesh.sphere(radio, segmentos, anillos)
    /// esfera
    template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
    static Mesh sphere(const T0& radio = {}, const T1& segmentos = {}, const T2& anillos = {}) {
        return Mesh(detail::lua("Mesh.sphere", Values{Value(radio), Value(segmentos), Value(anillos)}));
    }
    /// Mesh.subMeshCount: submallas (materiales)
    Value subMeshCount() const { return handle.get("subMeshCount"); }
    /// Cambia Mesh.subMeshCount
    void setSubMeshCount(const Value& v) const { handle.setField("subMeshCount", v); }
    /// Mesh.tangents: lista de Vec3
    Value tangents() const { return handle.get("tangents"); }
    /// Cambia Mesh.tangents
    void setTangents(const Value& v) const { handle.setField("tangents", v); }
    /// Mesh.triangleCount: triangulos
    Value triangleCount() const { return handle.get("triangleCount"); }
    /// Cambia Mesh.triangleCount
    void setTriangleCount(const Value& v) const { handle.setField("triangleCount", v); }
    /// Mesh.triangles: indices (submalla 0)
    Value triangles() const { return handle.get("triangles"); }
    /// Mesh.uv: lista de {x, y}
    Value uv() const { return handle.get("uv"); }
    /// Cambia Mesh.uv
    void setUv(const Value& v) const { handle.setField("uv", v); }
    /// Mesh:validate()
    /// "" si se puede dibujar; si no, el motivo
    template <typename... Mas>
    Value validate(Mas&&... mas) const {
        return handle.call("validate", Values{Value(std::forward<Mas>(mas))...});
    }
    /// Mesh.vertexCount: vertices
    Value vertexCount() const { return handle.get("vertexCount"); }
    /// Cambia Mesh.vertexCount
    void setVertexCount(const Value& v) const { handle.setField("vertexCount", v); }
    /// Mesh.vertices: lista de Vec3
    Value vertices() const { return handle.get("vertices"); }
    /// Cambia Mesh.vertices
    void setVertices(const Value& v) const { handle.setField("vertices", v); }
    /// Mesh.wireCube(tamano, grosor)
    /// aristas de una caja (contornos)
    template <typename T0 = Value, typename T1 = Value>
    static Mesh wireCube(const T0& tamano = {}, const T1& grosor = {}) {
        return Mesh(detail::lua("Mesh.wireCube", Values{Value(tamano), Value(grosor)}));
    }
};

/// Maquina de estados de un objeto (entity().getStateMachine()): go, trigger, set/get...
class StateMachine {
public:
    Value handle;
    StateMachine() = default;
    StateMachine(Value v) : handle(std::move(v)) {}
    operator Value() const { return handle; }
    explicit operator bool() const { return handle.truthy(); }
    /// StateMachine:broadcast(...)
    template <typename... Mas>
    Value broadcast(Mas&&... mas) const {
        return handle.call("broadcast", Values{Value(std::forward<Mas>(mas))...});
    }
    /// StateMachine:changeState(...)
    template <typename... Mas>
    Value changeState(Mas&&... mas) const {
        return handle.call("changeState", Values{Value(std::forward<Mas>(mas))...});
    }
    /// StateMachine:changes(...)
    template <typename... Mas>
    Value changes(Mas&&... mas) const {
        return handle.call("changes", Values{Value(std::forward<Mas>(mas))...});
    }
    /// StateMachine:entity(...)
    template <typename... Mas>
    Value entity(Mas&&... mas) const {
        return handle.call("entity", Values{Value(std::forward<Mas>(mas))...});
    }
    /// StateMachine:get(...)
    template <typename... Mas>
    Value get(Mas&&... mas) const {
        return handle.call("get", Values{Value(std::forward<Mas>(mas))...});
    }
    /// StateMachine:go(...)
    template <typename... Mas>
    Value go(Mas&&... mas) const {
        return handle.call("go", Values{Value(std::forward<Mas>(mas))...});
    }
    /// StateMachine:has(...)
    template <typename... Mas>
    Value has(Mas&&... mas) const {
        return handle.call("has", Values{Value(std::forward<Mas>(mas))...});
    }
    /// StateMachine:isIn(...)
    template <typename... Mas>
    Value isIn(Mas&&... mas) const {
        return handle.call("isIn", Values{Value(std::forward<Mas>(mas))...});
    }
    /// StateMachine.new(...)
    template <typename... Mas>
    static StateMachine create(Mas&&... mas) {
        return StateMachine(detail::lua("StateMachine.new", Values{Value(std::forward<Mas>(mas))...}));
    }
    /// StateMachine.of(...)
    template <typename... Mas>
    static StateMachine of(Mas&&... mas) {
        return StateMachine(detail::lua("StateMachine.of", Values{Value(std::forward<Mas>(mas))...}));
    }
    /// StateMachine:previous(...)
    template <typename... Mas>
    Value previous(Mas&&... mas) const {
        return handle.call("previous", Values{Value(std::forward<Mas>(mas))...});
    }
    /// StateMachine:restart(...)
    template <typename... Mas>
    Value restart(Mas&&... mas) const {
        return handle.call("restart", Values{Value(std::forward<Mas>(mas))...});
    }
    /// StateMachine:running(...)
    template <typename... Mas>
    Value running(Mas&&... mas) const {
        return handle.call("running", Values{Value(std::forward<Mas>(mas))...});
    }
    /// StateMachine:set(...)
    template <typename... Mas>
    Value set(Mas&&... mas) const {
        return handle.call("set", Values{Value(std::forward<Mas>(mas))...});
    }
    /// StateMachine:start(...)
    template <typename... Mas>
    Value start(Mas&&... mas) const {
        return handle.call("start", Values{Value(std::forward<Mas>(mas))...});
    }
    /// StateMachine:state(...)
    template <typename... Mas>
    Value state(Mas&&... mas) const {
        return handle.call("state", Values{Value(std::forward<Mas>(mas))...});
    }
    /// StateMachine:stateTime(...)
    template <typename... Mas>
    Value stateTime(Mas&&... mas) const {
        return handle.call("stateTime", Values{Value(std::forward<Mas>(mas))...});
    }
    /// StateMachine:states(...)
    template <typename... Mas>
    Value states(Mas&&... mas) const {
        return handle.call("states", Values{Value(std::forward<Mas>(mas))...});
    }
    /// StateMachine:stop(...)
    template <typename... Mas>
    Value stop(Mas&&... mas) const {
        return handle.call("stop", Values{Value(std::forward<Mas>(mas))...});
    }
    /// StateMachine:time(...)
    template <typename... Mas>
    Value time(Mas&&... mas) const {
        return handle.call("time", Values{Value(std::forward<Mas>(mas))...});
    }
    /// StateMachine:trigger(...)
    template <typename... Mas>
    Value trigger(Mas&&... mas) const {
        return handle.call("trigger", Values{Value(std::forward<Mas>(mas))...});
    }
    /// StateMachine:vars(...)
    template <typename... Mas>
    Value vars(Mas&&... mas) const {
        return handle.call("vars", Values{Value(std::forward<Mas>(mas))...});
    }
};

/// Behavior Tree de un objeto (entity().getBehaviorTree()): get/set de la pizarra, start/stop, finishTask...
class BehaviorTree {
public:
    Value handle;
    BehaviorTree() = default;
    BehaviorTree(Value v) : handle(std::move(v)) {}
    operator Value() const { return handle; }
    explicit operator bool() const { return handle.truthy(); }
    /// BehaviorTree:activeTask(...)
    template <typename... Mas>
    Value activeTask(Mas&&... mas) const {
        return handle.call("activeTask", Values{Value(std::forward<Mas>(mas))...});
    }
    /// BehaviorTree.broadcast("clave", valor)
    /// cambia una clave en todos los arboles
    template <typename T0 = Value, typename T1 = Value>
    static BehaviorTree broadcast(const T0& texto = {}, const T1& valor = {}) {
        return BehaviorTree(detail::lua("BehaviorTree.broadcast", Values{Value(texto), Value(valor)}));
    }
    /// BehaviorTree:clear(...)
    template <typename... Mas>
    Value clear(Mas&&... mas) const {
        return handle.call("clear", Values{Value(std::forward<Mas>(mas))...});
    }
    /// BehaviorTree:cycles(...)
    template <typename... Mas>
    Value cycles(Mas&&... mas) const {
        return handle.call("cycles", Values{Value(std::forward<Mas>(mas))...});
    }
    /// BehaviorTree:entity(...)
    template <typename... Mas>
    Value entity(Mas&&... mas) const {
        return handle.call("entity", Values{Value(std::forward<Mas>(mas))...});
    }
    /// BehaviorTree:finishTask(...)
    template <typename... Mas>
    Value finishTask(Mas&&... mas) const {
        return handle.call("finishTask", Values{Value(std::forward<Mas>(mas))...});
    }
    /// BehaviorTree:get(...)
    template <typename... Mas>
    Value get(Mas&&... mas) const {
        return handle.call("get", Values{Value(std::forward<Mas>(mas))...});
    }
    /// BehaviorTree:has(...)
    template <typename... Mas>
    Value has(Mas&&... mas) const {
        return handle.call("has", Values{Value(std::forward<Mas>(mas))...});
    }
    /// BehaviorTree:isActive(...)
    template <typename... Mas>
    Value isActive(Mas&&... mas) const {
        return handle.call("isActive", Values{Value(std::forward<Mas>(mas))...});
    }
    /// BehaviorTree:new(...)
    template <typename... Mas>
    Value create(Mas&&... mas) const {
        return handle.call("new", Values{Value(std::forward<Mas>(mas))...});
    }
    /// BehaviorTree.of(entity)
    /// el arbol de un objeto (o nil)
    template <typename T0 = Value>
    static BehaviorTree of(const T0& entity = {}) {
        return BehaviorTree(detail::lua("BehaviorTree.of", Values{Value(entity)}));
    }
    /// BehaviorTree.registerTask("Atacar", function(self, bt, primera) return "running" end)
    /// tarea Run Script en Lua
    template <typename T0 = Value, typename T1 = Value>
    static BehaviorTree registerTask(const T0& texto = {}, const T1& callback = {}) {
        return BehaviorTree(detail::lua("BehaviorTree.registerTask", Values{Value(texto), Value(callback)}));
    }
    /// BehaviorTree.reportNoise(posicion, radio, quien)
    /// ruido que oye el servicio Hearing
    template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
    static BehaviorTree reportNoise(const T0& posicion = {}, const T1& radio = {}, const T2& quien = {}) {
        return BehaviorTree(detail::lua("BehaviorTree.reportNoise", Values{Value(posicion), Value(radio), Value(quien)}));
    }
    /// BehaviorTree:restart(...)
    template <typename... Mas>
    Value restart(Mas&&... mas) const {
        return handle.call("restart", Values{Value(std::forward<Mas>(mas))...});
    }
    /// BehaviorTree:running(...)
    template <typename... Mas>
    Value running(Mas&&... mas) const {
        return handle.call("running", Values{Value(std::forward<Mas>(mas))...});
    }
    /// BehaviorTree:set(...)
    template <typename... Mas>
    Value set(Mas&&... mas) const {
        return handle.call("set", Values{Value(std::forward<Mas>(mas))...});
    }
    /// BehaviorTree:start(...)
    template <typename... Mas>
    Value start(Mas&&... mas) const {
        return handle.call("start", Values{Value(std::forward<Mas>(mas))...});
    }
    /// BehaviorTree:stop(...)
    template <typename... Mas>
    Value stop(Mas&&... mas) const {
        return handle.call("stop", Values{Value(std::forward<Mas>(mas))...});
    }
    /// BehaviorTree:time(...)
    template <typename... Mas>
    Value time(Mas&&... mas) const {
        return handle.call("time", Values{Value(std::forward<Mas>(mas))...});
    }
    /// BehaviorTree:vars(...)
    template <typename... Mas>
    Value vars(Mas&&... mas) const {
        return handle.call("vars", Values{Value(std::forward<Mas>(mas))...});
    }
};

}  // namespace cramion

// GENERADO por cramion_sdkgen desde el registro de la API del motor: no lo edites.
// Cada funcion de la API (Tabla.funcion) es una funcion de C++ con los mismos
// argumentos (Value: numeros, texto, Vec3, entidades, listas, objetos,
// funciones...) que devuelve un Value. Ver la referencia de la API en el manual.
#pragma once

namespace cramion {

namespace Accessibility {
/// Accessibility.colorblindName(2)
/// nombre del tipo de daltonismo
/// Devuelve: texto
template <typename T0 = Value>
inline Value colorblindName(const T0& valor = {}) {
    return detail::api("Accessibility.colorblindName", Values{Value(valor)});
}
/// Accessibility.get()
/// {colorblind, colorblindStrength, colorblindCorrect, textScale, subtitleScale, ...}
/// Devuelve: objeto
template <typename... Mas>
inline Value get(Mas&&... mas) {
    return detail::api("Accessibility.get", Values{Value(std::forward<Mas>(mas))...});
}
/// Accessibility.load()
/// las vuelve a leer
/// Devuelve: bool
template <typename... Mas>
inline Value load(Mas&&... mas) {
    return detail::api("Accessibility.load", Values{Value(std::forward<Mas>(mas))...});
}
/// Accessibility.reset()
/// por defecto
template <typename... Mas>
inline Value reset(Mas&&... mas) {
    return detail::api("Accessibility.reset", Values{Value(std::forward<Mas>(mas))...});
}
/// Accessibility.save()
/// las guarda (por jugador)
/// Devuelve: bool
template <typename... Mas>
inline Value save(Mas&&... mas) {
    return detail::api("Accessibility.save", Values{Value(std::forward<Mas>(mas))...});
}
/// Accessibility.set({colorblind = 2, textScale = 1.3, reduceMotion = true})
/// cambia opciones (las que vengan)
template <typename T0 = Value>
inline Value set(const T0& tabla = {}) {
    return detail::api("Accessibility.set", Values{Value(tabla)});
}
}  // namespace Accessibility

namespace Audio {
/// Audio.occlusion()
/// Esta la oclusion activa?
/// Devuelve: bool
template <typename... Mas>
inline Value occlusion(Mas&&... mas) {
    return detail::api("Audio.occlusion", Values{Value(std::forward<Mas>(mas))...});
}
/// Audio.playOneShot("Audio/golpe.wav", posicion, volumen)
/// Sonido suelto (sin posicion = 2D)
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value playOneShot(const T0& texto = {}, const T1& posicion = {}, const T2& volumen = {}) {
    return detail::api("Audio.playOneShot", Values{Value(texto), Value(posicion), Value(volumen)});
}
/// Audio.reverbLevel()
/// Reverberacion que se oye ahora (zonas)
/// Devuelve: numero
template <typename... Mas>
inline Value reverbLevel(Mas&&... mas) {
    return detail::api("Audio.reverbLevel", Values{Value(std::forward<Mas>(mas))...});
}
/// Audio.setLowPass(true, 800)
/// Todo apagado (bajo el agua, pausa)
template <typename T0 = Value, typename T1 = Value>
inline Value setLowPass(const T0& activar = {}, const T1& valor = {}) {
    return detail::api("Audio.setLowPass", Values{Value(activar), Value(valor)});
}
/// Audio.setOcclusion(true)
/// Las paredes tapan los sonidos (Audio Listener)
template <typename T0 = Value>
inline Value setOcclusion(const T0& activar = {}) {
    return detail::api("Audio.setOcclusion", Values{Value(activar)});
}
}  // namespace Audio

namespace Camera {
/// Camera.shake(0.6, 0.4, 18)
/// temblor de camara (intensidad 0..1, segundos, Hz)
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value shake(const T0& valor = {}, const T1& valor2 = {}, const T2& valor3 = {}) {
    return detail::api("Camera.shake", Values{Value(valor), Value(valor2), Value(valor3)});
}
/// Camera.stopShake()
/// lo para
template <typename... Mas>
inline Value stopShake(Mas&&... mas) {
    return detail::api("Camera.stopShake", Values{Value(std::forward<Mas>(mas))...});
}
}  // namespace Camera

namespace CharacterController {
/// CharacterController.Above: Bit de move(): choco por arriba (numero)
inline Value Above() { return detail::apiGet("CharacterController.Above"); }
/// CharacterController.Below: Bit de move(): toca el suelo (numero)
inline Value Below() { return detail::apiGet("CharacterController.Below"); }
/// CharacterController.Sides: Bit de move(): choco por los lados (numero)
inline Value Sides() { return detail::apiGet("CharacterController.Sides"); }
}  // namespace CharacterController

namespace Crowd {
/// Crowd.stats()
/// {agents, visible, spawners}
/// Devuelve: objeto
template <typename... Mas>
inline Value stats(Mas&&... mas) {
    return detail::api("Crowd.stats", Values{Value(std::forward<Mas>(mas))...});
}
}  // namespace Crowd

namespace DataPack {
/// DataPack.info("Nivel2")
/// {name, version, scenes, objects, files} sin montar nada (o nil)
/// Devuelve: objeto o nil
template <typename T0 = Value>
inline Value info(const T0& texto = {}) {
    return detail::api("DataPack.info", Values{Value(texto)});
}
/// DataPack.instantiate("Skins", "Coche", posicion, rotacion)
/// monta el paquete y crea un objeto (prefab) suyo; devuelve la Entity o nil
/// Devuelve: Entity o nil
template <typename T0 = Value, typename T1 = Value, typename T2 = Value, typename T3 = Value>
inline Value instantiate(const T0& texto = {}, const T1& texto2 = {}, const T2& posicion = {}, const T3& rotacion = {}) {
    return detail::api("DataPack.instantiate", Values{Value(texto), Value(texto2), Value(posicion), Value(rotacion)});
}
/// DataPack.isLoaded("Nivel2")
/// esta montado?
/// Devuelve: booleano
template <typename T0 = Value>
inline Value isLoaded(const T0& texto = {}) {
    return detail::api("DataPack.isLoaded", Values{Value(texto)});
}
/// DataPack.list()
/// nombres de los paquetes montados
/// Devuelve: lista de textos
template <typename... Mas>
inline Value list(Mas&&... mas) {
    return detail::api("DataPack.list", Values{Value(std::forward<Mas>(mas))...});
}
/// DataPack.load("Nivel2")
/// monta un .datapack (junto al juego, en DataPacks/ o ruta); devuelve {name, scenes, objects, files} o nil
/// Devuelve: objeto o nil
template <typename T0 = Value>
inline Value load(const T0& texto = {}) {
    return detail::api("DataPack.load", Values{Value(texto)});
}
/// DataPack.loadScene("Nivel2", "escena")
/// monta el paquete y carga su escena (la primera si no se dice cual); true/false
/// Devuelve: booleano
template <typename T0 = Value, typename T1 = Value>
inline Value loadScene(const T0& texto = {}, const T1& texto2 = {}) {
    return detail::api("DataPack.loadScene", Values{Value(texto), Value(texto2)});
}
/// DataPack.unload("Nivel2")
/// desmonta el paquete (borra lo que extrajo)
/// Devuelve: booleano
template <typename T0 = Value>
inline Value unload(const T0& texto = {}) {
    return detail::api("DataPack.unload", Values{Value(texto)});
}
}  // namespace DataPack

namespace Debug {
/// Debug.warn(...)
/// Escribe un aviso en la consola
template <typename... Mas>
inline Value warn(Mas&&... mas) {
    return detail::api("Debug.warn", Values{Value(std::forward<Mas>(mas))...});
}
}  // namespace Debug

namespace Dialogue {
/// Dialogue.advance()
/// lo mismo que next
template <typename... Mas>
inline Value advance(Mas&&... mas) {
    return detail::api("Dialogue.advance", Values{Value(std::forward<Mas>(mas))...});
}
/// Dialogue.choices()
/// lista de {index, text, enabled}
/// Devuelve: lista de objetos
template <typename... Mas>
inline Value choices(Mas&&... mas) {
    return detail::api("Dialogue.choices", Values{Value(std::forward<Mas>(mas))...});
}
/// Dialogue.choose(1)
/// elige la opcion 1..n
/// Devuelve: booleano
template <typename T0 = Value>
inline Value choose(const T0& valor = {}) {
    return detail::api("Dialogue.choose", Values{Value(valor)});
}
/// Dialogue.currentLine()
/// {speaker, text, audio, node, autoAdvance} o nil
/// Devuelve: objeto o nil
template <typename... Mas>
inline Value currentLine(Mas&&... mas) {
    return detail::api("Dialogue.currentLine", Values{Value(std::forward<Mas>(mas))...});
}
/// Dialogue.getVar("oro")
/// lee una variable
/// Devuelve: valor
template <typename T0 = Value>
inline Value getVar(const T0& texto = {}) {
    return detail::api("Dialogue.getVar", Values{Value(texto)});
}
/// Dialogue.isActive()
/// hay un dialogo en marcha?
/// Devuelve: booleano
template <typename... Mas>
inline Value isActive(Mas&&... mas) {
    return detail::api("Dialogue.isActive", Values{Value(std::forward<Mas>(mas))...});
}
/// Dialogue.isWaitingChoice()
/// esta esperando que se elija?
/// Devuelve: booleano
template <typename... Mas>
inline Value isWaitingChoice(Mas&&... mas) {
    return detail::api("Dialogue.isWaitingChoice", Values{Value(std::forward<Mas>(mas))...});
}
/// Dialogue.name()
/// dialogo que corre
/// Devuelve: texto
template <typename... Mas>
inline Value name(Mas&&... mas) {
    return detail::api("Dialogue.name", Values{Value(std::forward<Mas>(mas))...});
}
/// Dialogue.next()
/// sigue tras una linea
template <typename... Mas>
inline Value next(Mas&&... mas) {
    return detail::api("Dialogue.next", Values{Value(std::forward<Mas>(mas))...});
}
/// Dialogue.onChoices(function(opciones) end)
/// opciones para elegir
template <typename T0 = Value>
inline Value onChoices(const T0& callback = {}) {
    return detail::api("Dialogue.onChoices", Values{Value(callback)});
}
/// Dialogue.onEnd(function(nombre) end)
/// al terminar
template <typename T0 = Value>
inline Value onEnd(const T0& callback = {}) {
    return detail::api("Dialogue.onEnd", Values{Value(callback)});
}
/// Dialogue.onEvent(function(nombre, argumento) end)
/// nodo Evento
template <typename T0 = Value>
inline Value onEvent(const T0& callback = {}) {
    return detail::api("Dialogue.onEvent", Values{Value(callback)});
}
/// Dialogue.onLine(function(linea) end)
/// cada linea
template <typename T0 = Value>
inline Value onLine(const T0& callback = {}) {
    return detail::api("Dialogue.onLine", Values{Value(callback)});
}
/// Dialogue.onStart(function(nombre) end)
/// al empezar
template <typename T0 = Value>
inline Value onStart(const T0& callback = {}) {
    return detail::api("Dialogue.onStart", Values{Value(callback)});
}
/// Dialogue.setAutoAudio(true)
/// reproduce solo el audio de cada linea
template <typename T0 = Value>
inline Value setAutoAudio(const T0& activar = {}) {
    return detail::api("Dialogue.setAutoAudio", Values{Value(activar)});
}
/// Dialogue.setVar("oro", 10)
/// variable de los dialogos
template <typename T0 = Value, typename T1 = Value>
inline Value setVar(const T0& texto = {}, const T1& valor = {}) {
    return detail::api("Dialogue.setVar", Values{Value(texto), Value(valor)});
}
/// Dialogue.start("Mercader")
/// empieza un .crdialog (nombre o ruta)
/// Devuelve: booleano
template <typename T0 = Value>
inline Value start(const T0& texto = {}) {
    return detail::api("Dialogue.start", Values{Value(texto)});
}
/// Dialogue.stop()
/// lo corta
template <typename... Mas>
inline Value stop(Mas&&... mas) {
    return detail::api("Dialogue.stop", Values{Value(std::forward<Mas>(mas))...});
}
}  // namespace Dialogue

namespace Environment {
/// Environment.get()
/// clima actual ("Storm")
/// Devuelve: texto
template <typename... Mas>
inline Value get(Mas&&... mas) {
    return detail::api("Environment.get", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.getDate()
/// dia, mes
/// Devuelve: lista {dia, mes}
template <typename... Mas>
inline Value getDate(Mas&&... mas) {
    return detail::api("Environment.getDate", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.getFog()
/// densidad de la niebla
/// Devuelve: numero
template <typename... Mas>
inline Value getFog(Mas&&... mas) {
    return detail::api("Environment.getFog", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.getLabel()
/// nombre visible ("Tormenta")
/// Devuelve: texto
template <typename... Mas>
inline Value getLabel(Mas&&... mas) {
    return detail::api("Environment.getLabel", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.getLatitude()
/// latitud
/// Devuelve: numero
template <typename... Mas>
inline Value getLatitude(Mas&&... mas) {
    return detail::api("Environment.getLatitude", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.getRain()
/// lluvia 0..1
/// Devuelve: numero
template <typename... Mas>
inline Value getRain(Mas&&... mas) {
    return detail::api("Environment.getRain", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.getSeason()
/// estacion actual
/// Devuelve: texto
template <typename... Mas>
inline Value getSeason(Mas&&... mas) {
    return detail::api("Environment.getSeason", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.getSnow()
/// nevada 0..1
/// Devuelve: numero
template <typename... Mas>
inline Value getSnow(Mas&&... mas) {
    return detail::api("Environment.getSnow", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.getSnowCover()
/// nieve acumulada 0..1
/// Devuelve: numero
template <typename... Mas>
inline Value getSnowCover(Mas&&... mas) {
    return detail::api("Environment.getSnowCover", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.getSunDirection()
/// Vec3 hacia el sol
/// Devuelve: Vec3
template <typename... Mas>
inline Value getSunDirection(Mas&&... mas) {
    return detail::api("Environment.getSunDirection", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.getTarget()
/// clima al que va la transicion
/// Devuelve: texto
template <typename... Mas>
inline Value getTarget(Mas&&... mas) {
    return detail::api("Environment.getTarget", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.getTemperature()
/// grados C
/// Devuelve: numero
template <typename... Mas>
inline Value getTemperature(Mas&&... mas) {
    return detail::api("Environment.getTemperature", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.getTime()
/// hora del dia
/// Devuelve: numero
template <typename... Mas>
inline Value getTime(Mas&&... mas) {
    return detail::api("Environment.getTime", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.getTimeScale()
/// velocidad del tiempo
/// Devuelve: numero
template <typename... Mas>
inline Value getTimeScale(Mas&&... mas) {
    return detail::api("Environment.getTimeScale", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.getWeather()
/// clima actual ("Storm")
/// Devuelve: texto
template <typename... Mas>
inline Value getWeather(Mas&&... mas) {
    return detail::api("Environment.getWeather", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.getWetness()
/// humedad de las superficies 0..1
/// Devuelve: numero
template <typename... Mas>
inline Value getWetness(Mas&&... mas) {
    return detail::api("Environment.getWetness", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.getWind()
/// Vec3 del viento (m/s)
/// Devuelve: Vec3
template <typename... Mas>
inline Value getWind(Mas&&... mas) {
    return detail::api("Environment.getWind", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.getWindDirection()
/// grados
/// Devuelve: numero
template <typename... Mas>
inline Value getWindDirection(Mas&&... mas) {
    return detail::api("Environment.getWindDirection", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.getWindSpeed()
/// m/s con rachas
/// Devuelve: numero
template <typename... Mas>
inline Value getWindSpeed(Mas&&... mas) {
    return detail::api("Environment.getWindSpeed", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.isNight()
/// el sol esta bajo el horizonte?
/// Devuelve: bool
template <typename... Mas>
inline Value isNight(Mas&&... mas) {
    return detail::api("Environment.isNight", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.isTransitioning()
/// esta cambiando?
/// Devuelve: bool
template <typename... Mas>
inline Value isTransitioning(Mas&&... mas) {
    return detail::api("Environment.isTransitioning", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.lightning(800)
/// un rayo ya (distancia en m; sin ella al azar)
template <typename T0 = Value>
inline Value lightning(const T0& valor = {}) {
    return detail::api("Environment.lightning", Values{Value(valor)});
}
/// Environment.presets()
/// lista de climas
/// Devuelve: lista de texto
template <typename... Mas>
inline Value presets(Mas&&... mas) {
    return detail::api("Environment.presets", Values{Value(std::forward<Mas>(mas))...});
}
/// Environment.set("Storm", 10)
/// cambia de clima en N segundos (Clear, Cloudy, Overcast, Foggy, LightRain, Rain, Storm, LightSnow, Snow, Blizzard, Sandstorm)
/// Devuelve: bool
template <typename T0 = Value, typename T1 = Value>
inline Value set(const T0& texto = {}, const T1& valor = {}) {
    return detail::api("Environment.set", Values{Value(texto), Value(valor)});
}
/// Environment.setAudio(true, 0.8)
/// sonido de lluvia, viento y truenos
template <typename T0 = Value, typename T1 = Value>
inline Value setAudio(const T0& activar = {}, const T1& valor = {}) {
    return detail::api("Environment.setAudio", Values{Value(activar), Value(valor)});
}
/// Environment.setDate(21, 12)
/// dia y mes (mueve el sol y la estacion)
template <typename T0 = Value, typename T1 = Value>
inline Value setDate(const T0& valor = {}, const T1& valor2 = {}) {
    return detail::api("Environment.setDate", Values{Value(valor), Value(valor2)});
}
/// Environment.setDayLength(24)
/// minutos reales por dia (nil = el tiempo se para)
template <typename T0 = Value>
inline Value setDayLength(const T0& valor = {}) {
    return detail::api("Environment.setDayLength", Values{Value(valor)});
}
/// Environment.setLatitude(40)
/// latitud en grados
template <typename T0 = Value>
inline Value setLatitude(const T0& valor = {}) {
    return detail::api("Environment.setLatitude", Values{Value(valor)});
}
/// Environment.setLightning(true, 2)
/// rayos en las tormentas y su frecuencia
template <typename T0 = Value, typename T1 = Value>
inline Value setLightning(const T0& activar = {}, const T1& valor = {}) {
    return detail::api("Environment.setLightning", Values{Value(activar), Value(valor)});
}
/// Environment.setPrecipitationDensity(0.5)
/// menos gotas (rendimiento)
template <typename T0 = Value>
inline Value setPrecipitationDensity(const T0& valor = {}) {
    return detail::api("Environment.setPrecipitationDensity", Values{Value(valor)});
}
/// Environment.setRandom(true, 120, 360)
/// clima al azar (segundos min y max)
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value setRandom(const T0& activar = {}, const T1& valor = {}, const T2& valor2 = {}) {
    return detail::api("Environment.setRandom", Values{Value(activar), Value(valor), Value(valor2)});
}
/// Environment.setSeason("Winter")
/// estacion fija ("auto" = por la fecha)
template <typename T0 = Value>
inline Value setSeason(const T0& texto = {}) {
    return detail::api("Environment.setSeason", Values{Value(texto)});
}
/// Environment.setSnowCover(1)
/// nieve acumulada al instante
template <typename T0 = Value>
inline Value setSnowCover(const T0& valor = {}) {
    return detail::api("Environment.setSnowCover", Values{Value(valor)});
}
/// Environment.setTime(18.5)
/// hora del dia (0..24)
template <typename T0 = Value>
inline Value setTime(const T0& valor = {}) {
    return detail::api("Environment.setTime", Values{Value(valor)});
}
/// Environment.setTimeScale(60)
/// velocidad del tiempo (1 = real, 0 = parado)
template <typename T0 = Value>
inline Value setTimeScale(const T0& valor = {}) {
    return detail::api("Environment.setTimeScale", Values{Value(valor)});
}
/// Environment.setWeather("Rain", 5)
/// lo mismo que set
/// Devuelve: bool
template <typename T0 = Value, typename T1 = Value>
inline Value setWeather(const T0& texto = {}, const T1& valor = {}) {
    return detail::api("Environment.setWeather", Values{Value(texto), Value(valor)});
}
/// Environment.setWetness(1, 0.6)
/// humedad y charcos al instante
template <typename T0 = Value, typename T1 = Value>
inline Value setWetness(const T0& valor = {}, const T1& valor2 = {}) {
    return detail::api("Environment.setWetness", Values{Value(valor), Value(valor2)});
}
/// Environment.setWind(90, 1.5)
/// direccion (grados) y fuerza del viento
template <typename T0 = Value, typename T1 = Value>
inline Value setWind(const T0& valor = {}, const T1& valor2 = {}) {
    return detail::api("Environment.setWind", Values{Value(valor), Value(valor2)});
}
/// Environment.transition()
/// 0..1 lo que lleva la transicion
/// Devuelve: numero
template <typename... Mas>
inline Value transition(Mas&&... mas) {
    return detail::api("Environment.transition", Values{Value(std::forward<Mas>(mas))...});
}
}  // namespace Environment

namespace Fire {
/// Fire.burnedAt(posicion)
/// quemado 0..1
/// Devuelve: numero
template <typename T0 = Value>
inline Value burnedAt(const T0& posicion = {}) {
    return detail::api("Fire.burnedAt", Values{Value(posicion)});
}
/// Fire.burnedFraction()
/// 0..1 de lo que podia arder
/// Devuelve: numero
template <typename... Mas>
inline Value burnedFraction(Mas&&... mas) {
    return detail::api("Fire.burnedFraction", Values{Value(std::forward<Mas>(mas))...});
}
/// Fire.burningArea()
/// m2 en llamas
/// Devuelve: numero
template <typename... Mas>
inline Value burningArea(Mas&&... mas) {
    return detail::api("Fire.burningArea", Values{Value(std::forward<Mas>(mas))...});
}
/// Fire.charAt(posicion)
/// lo mismo que burnedAt
/// Devuelve: numero
template <typename T0 = Value>
inline Value charAt(const T0& posicion = {}) {
    return detail::api("Fire.charAt", Values{Value(posicion)});
}
/// Fire.extinguish(posicion, radio)
/// apaga el fuego en el circulo
/// Devuelve: numero
template <typename T0 = Value, typename T1 = Value>
inline Value extinguish(const T0& posicion = {}, const T1& radio = {}) {
    return detail::api("Fire.extinguish", Values{Value(posicion), Value(radio)});
}
/// Fire.extinguishAll()
/// apaga todo
template <typename... Mas>
inline Value extinguishAll(Mas&&... mas) {
    return detail::api("Fire.extinguishAll", Values{Value(std::forward<Mas>(mas))...});
}
/// Fire.heatAt(posicion)
/// calor 0..1
/// Devuelve: numero
template <typename T0 = Value>
inline Value heatAt(const T0& posicion = {}) {
    return detail::api("Fire.heatAt", Values{Value(posicion)});
}
/// Fire.ignite(posicion, radio)
/// enciende fuego en las zonas Fuego que tocan el circulo (devuelve cuantas)
/// Devuelve: numero
template <typename T0 = Value, typename T1 = Value>
inline Value ignite(const T0& posicion = {}, const T1& radio = {}) {
    return detail::api("Fire.ignite", Values{Value(posicion), Value(radio)});
}
/// Fire.isActive()
/// hay algo ardiendo?
/// Devuelve: bool
template <typename... Mas>
inline Value isActive(Mas&&... mas) {
    return detail::api("Fire.isActive", Values{Value(std::forward<Mas>(mas))...});
}
/// Fire.isBurning(posicion)
/// hay llamas ahi?
/// Devuelve: bool
template <typename T0 = Value>
inline Value isBurning(const T0& posicion = {}) {
    return detail::api("Fire.isBurning", Values{Value(posicion)});
}
/// Fire.reset()
/// vuelve a empezar: nada quemado (y se reenciende si 'Encender al empezar')
template <typename... Mas>
inline Value reset(Mas&&... mas) {
    return detail::api("Fire.reset", Values{Value(std::forward<Mas>(mas))...});
}
/// Fire.stats()
/// {burningCells, burnedFraction, burningArea, seconds...}
/// Devuelve: objeto
template <typename... Mas>
inline Value stats(Mas&&... mas) {
    return detail::api("Fire.stats", Values{Value(std::forward<Mas>(mas))...});
}
}  // namespace Fire

namespace Fluid {
/// Fluid.clear()
/// borra todo el liquido
template <typename... Mas>
inline Value clear(Mas&&... mas) {
    return detail::api("Fluid.clear", Values{Value(std::forward<Mas>(mas))...});
}
/// Fluid.count("honey")
/// particulas (todas o de un tipo)
/// Devuelve: numero
template <typename T0 = Value>
inline Value count(const T0& texto = {}) {
    return detail::api("Fluid.count", Values{Value(texto)});
}
/// Fluid.density(pos, radio)
/// 0 = seco, ~1 = lleno
/// Devuelve: numero
template <typename T0 = Value, typename T1 = Value>
inline Value density(const T0& pos = {}, const T1& radio = {}) {
    return detail::api("Fluid.density", Values{Value(pos), Value(radio)});
}
/// Fluid.isActive()
/// hay liquidos en la escena?
/// Devuelve: bool
template <typename... Mas>
inline Value isActive(Mas&&... mas) {
    return detail::api("Fluid.isActive", Values{Value(std::forward<Mas>(mas))...});
}
/// Fluid.isInside(pos)
/// hay liquido ahi?
/// Devuelve: bool
template <typename T0 = Value>
inline Value isInside(const T0& pos = {}) {
    return detail::api("Fluid.isInside", Values{Value(pos)});
}
/// Fluid.restart(entidad)
/// vuelve a llenar una caja/esfera
template <typename T0 = Value>
inline Value restart(const T0& entidad = {}) {
    return detail::api("Fluid.restart", Values{Value(entidad)});
}
/// Fluid.setType(entidad, "lava")
/// cambia el liquido del emisor
template <typename T0 = Value, typename T1 = Value>
inline Value setType(const T0& entidad = {}, const T1& texto = {}) {
    return detail::api("Fluid.setType", Values{Value(entidad), Value(texto)});
}
/// Fluid.spawn(pos, cantidad, "water", vel, radio, vida)
/// crea liquido (bola de particulas)
template <typename T0 = Value, typename T1 = Value, typename T2 = Value, typename T3 = Value, typename T4 = Value, typename T5 = Value>
inline Value spawn(const T0& pos = {}, const T1& cantidad = {}, const T2& texto = {}, const T3& vel = {}, const T4& radio = {}, const T5& vida = {}) {
    return detail::api("Fluid.spawn", Values{Value(pos), Value(cantidad), Value(texto), Value(vel), Value(radio), Value(vida)});
}
/// Fluid.start(entidad)
/// el emisor empieza
template <typename T0 = Value>
inline Value start(const T0& entidad = {}) {
    return detail::api("Fluid.start", Values{Value(entidad)});
}
/// Fluid.stats()
/// {particles, capacity, emitters...}
/// Devuelve: objeto
template <typename... Mas>
inline Value stats(Mas&&... mas) {
    return detail::api("Fluid.stats", Values{Value(std::forward<Mas>(mas))...});
}
/// Fluid.stop(entidad)
/// el emisor para
template <typename T0 = Value>
inline Value stop(const T0& entidad = {}) {
    return detail::api("Fluid.stop", Values{Value(entidad)});
}
/// Fluid.surfaceHeight(x, z)
/// nil o la altura de la superficie
/// Devuelve: numero o nil
template <typename T0 = Value, typename T1 = Value>
inline Value surfaceHeight(const T0& x = {}, const T1& z = {}) {
    return detail::api("Fluid.surfaceHeight", Values{Value(x), Value(z)});
}
/// Fluid.types()
/// lista de tipos
/// Devuelve: lista de texto
template <typename... Mas>
inline Value types(Mas&&... mas) {
    return detail::api("Fluid.types", Values{Value(std::forward<Mas>(mas))...});
}
/// Fluid.velocity(pos, radio)
/// velocidad media del liquido (Vec3)
/// Devuelve: Vec3
template <typename T0 = Value, typename T1 = Value>
inline Value velocity(const T0& pos = {}, const T1& radio = {}) {
    return detail::api("Fluid.velocity", Values{Value(pos), Value(radio)});
}
}  // namespace Fluid

namespace Game {
/// Game.quit()
/// Cierra el juego (en el editor, sale de Play)
template <typename... Mas>
inline Value quit(Mas&&... mas) {
    return detail::api("Game.quit", Values{Value(std::forward<Mas>(mas))...});
}
}  // namespace Game

namespace Graphics {
/// Graphics.get("texture_quality")
/// valor de una opcion
/// Devuelve: bool, numero o texto
template <typename T0 = Value>
inline Value get(const T0& texto = {}) {
    return detail::api("Graphics.get", Values{Value(texto)});
}
/// Graphics.getAll()
/// tabla clave -> valor
/// Devuelve: objeto
template <typename... Mas>
inline Value getAll(Mas&&... mas) {
    return detail::api("Graphics.getAll", Values{Value(std::forward<Mas>(mas))...});
}
/// Graphics.getPost("bloom")
/// campo del post-procesado global
/// Devuelve: bool, numero, texto o Vec3
template <typename T0 = Value>
inline Value getPost(const T0& texto = {}) {
    return detail::api("Graphics.getPost", Values{Value(texto)});
}
/// Graphics.getQuality()
/// la ultima calidad rapida o Personalizada
/// Devuelve: texto
template <typename... Mas>
inline Value getQuality(Mas&&... mas) {
    return detail::api("Graphics.getQuality", Values{Value(std::forward<Mas>(mas))...});
}
/// Graphics.options()
/// lista {key, value, writable, description, choices} para un menu
/// Devuelve: lista de objetos
template <typename... Mas>
inline Value options(Mas&&... mas) {
    return detail::api("Graphics.options", Values{Value(std::forward<Mas>(mas))...});
}
/// Graphics.post: post-procesado global: Graphics.post.bloom = false (objeto)
inline Value post() { return detail::apiGet("Graphics.post"); }
/// Graphics.postKeys()
/// claves del post-procesado
/// Devuelve: lista de texto
template <typename... Mas>
inline Value postKeys(Mas&&... mas) {
    return detail::api("Graphics.postKeys", Values{Value(std::forward<Mas>(mas))...});
}
/// Graphics.qualityLevels()
/// lista de calidades
/// Devuelve: lista de texto
template <typename... Mas>
inline Value qualityLevels(Mas&&... mas) {
    return detail::api("Graphics.qualityLevels", Values{Value(std::forward<Mas>(mas))...});
}
/// Graphics.resolutions()
/// resoluciones del monitor {width, height}
/// Devuelve: lista de {width, height}
template <typename... Mas>
inline Value resolutions(Mas&&... mas) {
    return detail::api("Graphics.resolutions", Values{Value(std::forward<Mas>(mas))...});
}
/// Graphics.save()
/// guarda la configuracion del jugador (juego exportado)
/// Devuelve: bool
template <typename... Mas>
inline Value save(Mas&&... mas) {
    return detail::api("Graphics.save", Values{Value(std::forward<Mas>(mas))...});
}
/// Graphics.set("vsync", true)
/// cambia una opcion; o Graphics.set{ clave = valor, ... }
/// Devuelve: bool
template <typename T0 = Value, typename T1 = Value>
inline Value set(const T0& texto = {}, const T1& activar = {}) {
    return detail::api("Graphics.set", Values{Value(texto), Value(activar)});
}
/// Graphics.setPost("bloom", false)
/// cambia el post-procesado global
/// Devuelve: bool
template <typename T0 = Value, typename T1 = Value>
inline Value setPost(const T0& texto = {}, const T1& activar = {}) {
    return detail::api("Graphics.setPost", Values{Value(texto), Value(activar)});
}
/// Graphics.setQuality("Alta")
/// calidad rapida: Baja, Media, Alta, Ultra (o 0..3)
/// Devuelve: bool
template <typename T0 = Value>
inline Value setQuality(const T0& texto = {}) {
    return detail::api("Graphics.setQuality", Values{Value(texto)});
}
}  // namespace Graphics

namespace Http {
/// Http.cancelAll()
/// cancela todas
template <typename... Mas>
inline Value cancelAll(Mas&&... mas) {
    return detail::api("Http.cancelAll", Values{Value(std::forward<Mas>(mas))...});
}
/// Http.get("https://...", function(res) end, cabeceras)
/// GET; res = {ok, status, body, data, headers, error}
/// Devuelve: id de la peticion (nil si no vale)
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value get(const T0& texto = {}, const T1& callback = {}, const T2& cabeceras = {}) {
    return detail::api("Http.get", Values{Value(texto), Value(callback), Value(cabeceras)});
}
/// Http.pending()
/// peticiones sin terminar
/// Devuelve: numero
template <typename... Mas>
inline Value pending(Mas&&... mas) {
    return detail::api("Http.pending", Values{Value(std::forward<Mas>(mas))...});
}
/// Http.post("https://...", datos, function(res) end, cabeceras)
/// POST; datos texto o tabla (se manda como JSON)
/// Devuelve: id de la peticion (nil si no vale)
template <typename T0 = Value, typename T1 = Value, typename T2 = Value, typename T3 = Value>
inline Value post(const T0& texto = {}, const T1& datos = {}, const T2& callback = {}, const T3& cabeceras = {}) {
    return detail::api("Http.post", Values{Value(texto), Value(datos), Value(callback), Value(cabeceras)});
}
/// Http.query({ q = "hola", page = 2 })
/// "page=2&q=hola" (codificado)
/// Devuelve: texto
template <typename T0 = Value>
inline Value query(const T0& tabla = {}) {
    return detail::api("Http.query", Values{Value(tabla)});
}
/// Http.request({ url = "https://...", method = "PUT", headers = {}, body = {}, timeout = 20 }, function(res) end)
/// cualquier metodo, con tiempo maximo y tamano maximo (maxSize)
/// Devuelve: id de la peticion (nil si no vale)
template <typename T0 = Value, typename T1 = Value>
inline Value request(const T0& tabla = {}, const T1& callback = {}) {
    return detail::api("Http.request", Values{Value(tabla), Value(callback)});
}
/// Http.urlEncode("hola mundo")
/// texto seguro para una URL
/// Devuelve: texto
template <typename T0 = Value>
inline Value urlEncode(const T0& texto = {}) {
    return detail::api("Http.urlEncode", Values{Value(texto)});
}
}  // namespace Http

namespace Input {
/// Input.addMappingContext("Vuelo", 1)
/// activa un contexto (prioridad opcional)
/// Devuelve: bool
template <typename T0 = Value, typename T1 = Value>
inline Value addMappingContext(const T0& texto = {}, const T1& valor = {}) {
    return detail::api("Input.addMappingContext", Values{Value(texto), Value(valor)});
}
/// Input.anyKeyPressed()
/// tecla/boton pulsado este frame ("W", "Gamepad A") o nil
/// Devuelve: texto o nil
template <typename... Mas>
inline Value anyKeyPressed(Mas&&... mas) {
    return detail::api("Input.anyKeyPressed", Values{Value(std::forward<Mas>(mas))...});
}
/// Input.bindAction("Jump", "triggered", function(valor, t) end)
/// llama a la funcion en ese evento; devuelve un id
/// Devuelve: numero
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value bindAction(const T0& texto = {}, const T1& texto2 = {}, const T2& callback = {}) {
    return detail::api("Input.bindAction", Values{Value(texto), Value(texto2), Value(callback)});
}
/// Input.clearMappingContexts()
/// desactiva todos los contextos
template <typename... Mas>
inline Value clearMappingContexts(Mas&&... mas) {
    return detail::api("Input.clearMappingContexts", Values{Value(std::forward<Mas>(mas))...});
}
/// Input.getAction("Move")
/// valor de una accion: Bool -> true/false, Axis1D -> numero, Axis2D/3D -> Vec3
/// Devuelve: bool, numero o Vec3
template <typename T0 = Value>
inline Value getAction(const T0& texto = {}) {
    return detail::api("Input.getAction", Values{Value(texto)});
}
/// Input.getActionElapsed("Fire")
/// segundos desde que empezo
/// Devuelve: numero
template <typename T0 = Value>
inline Value getActionElapsed(const T0& texto = {}) {
    return detail::api("Input.getActionElapsed", Values{Value(texto)});
}
/// Input.getActionState("Jump")
/// "none", "ongoing" o "triggered"
/// Devuelve: texto
template <typename T0 = Value>
inline Value getActionState(const T0& texto = {}) {
    return detail::api("Input.getActionState", Values{Value(texto)});
}
/// Input.getActionValue("Move")
/// Vec3 con el valor de la accion
/// Devuelve: Vec3
template <typename T0 = Value>
inline Value getActionValue(const T0& texto = {}) {
    return detail::api("Input.getActionValue", Values{Value(texto)});
}
/// Input.getActions()
/// nombres de todas las acciones del proyecto
/// Devuelve: lista de texto
template <typename... Mas>
inline Value getActions(Mas&&... mas) {
    return detail::api("Input.getActions", Values{Value(std::forward<Mas>(mas))...});
}
/// Input.getAxis("Horizontal")
/// -1..1: Horizontal (A/D), Vertical (W/S), Mouse X, Mouse Y
/// Devuelve: numero
template <typename T0 = Value>
inline Value getAxis(const T0& texto = {}) {
    return detail::api("Input.getAxis", Values{Value(texto)});
}
/// Input.getBindings("Jump")
/// lista {context, key} de las teclas de una accion
/// Devuelve: lista de {context, key}
template <typename T0 = Value>
inline Value getBindings(const T0& texto = {}) {
    return detail::api("Input.getBindings", Values{Value(texto)});
}
/// Input.getGamepadAxis("leftx")
/// eje del mando: leftx, lefty, rightx, righty, lt, rt
/// Devuelve: numero
template <typename T0 = Value>
inline Value getGamepadAxis(const T0& texto = {}) {
    return detail::api("Input.getGamepadAxis", Values{Value(texto)});
}
/// Input.getGamepadButton("a")
/// boton del mando mantenido: a, b, x, y, lb, rb, ls, rs, start, back, up, down, left, right
/// Devuelve: bool
template <typename T0 = Value>
inline Value getGamepadButton(const T0& texto = {}) {
    return detail::api("Input.getGamepadButton", Values{Value(texto)});
}
/// Input.getGamepadButtonDown("a")
/// pulsado este frame
/// Devuelve: bool
template <typename T0 = Value>
inline Value getGamepadButtonDown(const T0& texto = {}) {
    return detail::api("Input.getGamepadButtonDown", Values{Value(texto)});
}
/// Input.getGamepadButtonUp("a")
/// soltado este frame
/// Devuelve: bool
template <typename T0 = Value>
inline Value getGamepadButtonUp(const T0& texto = {}) {
    return detail::api("Input.getGamepadButtonUp", Values{Value(texto)});
}
/// Input.getKey("W")
/// tecla mantenida
/// Devuelve: bool
template <typename T0 = Value>
inline Value getKey(const T0& texto = {}) {
    return detail::api("Input.getKey", Values{Value(texto)});
}
/// Input.getKeyDown("Space")
/// tecla pulsada este frame
/// Devuelve: bool
template <typename T0 = Value>
inline Value getKeyDown(const T0& texto = {}) {
    return detail::api("Input.getKeyDown", Values{Value(texto)});
}
/// Input.getKeyUp("E")
/// tecla soltada este frame
/// Devuelve: bool
template <typename T0 = Value>
inline Value getKeyUp(const T0& texto = {}) {
    return detail::api("Input.getKeyUp", Values{Value(texto)});
}
/// Input.getMappingContexts()
/// lista de contextos activos
/// Devuelve: lista de texto
template <typename... Mas>
inline Value getMappingContexts(Mas&&... mas) {
    return detail::api("Input.getMappingContexts", Values{Value(std::forward<Mas>(mas))...});
}
/// Input.getMouseButton(0)
/// boton del raton mantenido (0 izq, 1 der, 2 medio)
/// Devuelve: bool
template <typename T0 = Value>
inline Value getMouseButton(const T0& valor = {}) {
    return detail::api("Input.getMouseButton", Values{Value(valor)});
}
/// Input.getMouseButtonDown(0)
/// boton pulsado este frame
/// Devuelve: bool
template <typename T0 = Value>
inline Value getMouseButtonDown(const T0& valor = {}) {
    return detail::api("Input.getMouseButtonDown", Values{Value(valor)});
}
/// Input.getMouseButtonUp(0)
/// boton soltado
/// Devuelve: bool
template <typename T0 = Value>
inline Value getMouseButtonUp(const T0& valor = {}) {
    return detail::api("Input.getMouseButtonUp", Values{Value(valor)});
}
/// Input.getTouch(1)
/// {id, position, delta, start, phase} de un dedo (1..touchCount)
/// Devuelve: touch o nil
template <typename T0 = Value>
inline Value getTouch(const T0& valor = {}) {
    return detail::api("Input.getTouch", Values{Value(valor)});
}
/// Input.hasMappingContext("Vuelo")
/// esta activo?
/// Devuelve: bool
template <typename T0 = Value>
inline Value hasMappingContext(const T0& texto = {}) {
    return detail::api("Input.hasMappingContext", Values{Value(texto)});
}
/// Input.isActionOngoing("Fire")
/// en curso (Hold todavia sin completar...)
/// Devuelve: bool
template <typename T0 = Value>
inline Value isActionOngoing(const T0& texto = {}) {
    return detail::api("Input.isActionOngoing", Values{Value(texto)});
}
/// Input.isActionTriggered("Fire")
/// disparada este frame (segun sus triggers)
/// Devuelve: bool
template <typename T0 = Value>
inline Value isActionTriggered(const T0& texto = {}) {
    return detail::api("Input.isActionTriggered", Values{Value(texto)});
}
/// Input.isCursorLocked()
/// esta capturado?
/// Devuelve: bool
template <typename... Mas>
inline Value isCursorLocked(Mas&&... mas) {
    return detail::api("Input.isCursorLocked", Values{Value(std::forward<Mas>(mas))...});
}
/// Input.isGamepadConnected()
/// hay un mando?
/// Devuelve: bool
template <typename... Mas>
inline Value isGamepadConnected(Mas&&... mas) {
    return detail::api("Input.isGamepadConnected", Values{Value(std::forward<Mas>(mas))...});
}
/// Input.isMobile()
/// movil o pantalla tactil
/// Devuelve: bool
template <typename... Mas>
inline Value isMobile(Mas&&... mas) {
    return detail::api("Input.isMobile", Values{Value(std::forward<Mas>(mas))...});
}
/// Input.lockCursor(true)
/// captura el raton (primera persona); false lo suelta
template <typename T0 = Value>
inline Value lockCursor(const T0& activar = {}) {
    return detail::api("Input.lockCursor", Values{Value(activar)});
}
/// Input.mouseDelta()
/// Vec3 con el movimiento
/// Devuelve: Vec3
template <typename... Mas>
inline Value mouseDelta(Mas&&... mas) {
    return detail::api("Input.mouseDelta", Values{Value(std::forward<Mas>(mas))...});
}
/// Input.mousePosition()
/// Vec3 con la posicion del raton
/// Devuelve: Vec3
template <typename... Mas>
inline Value mousePosition(Mas&&... mas) {
    return detail::api("Input.mousePosition", Values{Value(std::forward<Mas>(mas))...});
}
/// Input.rebind("Default", "Jump", 1, "F")
/// cambia una tecla (1 = la primera de esa accion)
/// Devuelve: bool
template <typename T0 = Value, typename T1 = Value, typename T2 = Value, typename T3 = Value>
inline Value rebind(const T0& texto = {}, const T1& texto2 = {}, const T2& valor = {}, const T3& texto3 = {}) {
    return detail::api("Input.rebind", Values{Value(texto), Value(texto2), Value(valor), Value(texto3)});
}
/// Input.removeMappingContext("Vuelo")
/// desactiva un contexto
template <typename T0 = Value>
inline Value removeMappingContext(const T0& texto = {}) {
    return detail::api("Input.removeMappingContext", Values{Value(texto)});
}
/// Input.resetBindings()
/// vuelve a las teclas del proyecto
template <typename... Mas>
inline Value resetBindings(Mas&&... mas) {
    return detail::api("Input.resetBindings", Values{Value(std::forward<Mas>(mas))...});
}
/// Input.saveBindings()
/// guarda las teclas cambiadas (entre partidas)
template <typename... Mas>
inline Value saveBindings(Mas&&... mas) {
    return detail::api("Input.saveBindings", Values{Value(std::forward<Mas>(mas))...});
}
/// Input.setTouchButton("Saltar", true)
/// muestra u oculta un boton tactil por su texto
/// Devuelve: bool
template <typename T0 = Value, typename T1 = Value>
inline Value setTouchButton(const T0& texto = {}, const T1& activar = {}) {
    return detail::api("Input.setTouchButton", Values{Value(texto), Value(activar)});
}
/// Input.setTouchControls(true)
/// muestra u oculta los controles tactiles
template <typename T0 = Value>
inline Value setTouchControls(const T0& activar = {}) {
    return detail::api("Input.setTouchControls", Values{Value(activar)});
}
/// Input.setTouchJoystick(true)
/// joystick tactil
template <typename T0 = Value>
inline Value setTouchJoystick(const T0& activar = {}) {
    return detail::api("Input.setTouchJoystick", Values{Value(activar)});
}
/// Input.setTouchLook(true)
/// zona para mirar arrastrando
template <typename T0 = Value>
inline Value setTouchLook(const T0& activar = {}) {
    return detail::api("Input.setTouchLook", Values{Value(activar)});
}
/// Input.touchControlsEnabled()
/// estan visibles?
/// Devuelve: bool
template <typename... Mas>
inline Value touchControlsEnabled(Mas&&... mas) {
    return detail::api("Input.touchControlsEnabled", Values{Value(std::forward<Mas>(mas))...});
}
/// Input.touchCount()
/// dedos en la pantalla
/// Devuelve: numero
template <typename... Mas>
inline Value touchCount(Mas&&... mas) {
    return detail::api("Input.touchCount", Values{Value(std::forward<Mas>(mas))...});
}
/// Input.unbindAction(id)
/// quita un bindAction
template <typename T0 = Value>
inline Value unbindAction(const T0& id = {}) {
    return detail::api("Input.unbindAction", Values{Value(id)});
}
/// Input.vibrate(60)
/// vibra el movil (ms)
template <typename T0 = Value>
inline Value vibrate(const T0& valor = {}) {
    return detail::api("Input.vibrate", Values{Value(valor)});
}
/// Input.wasActionCanceled("Jump")
/// se solto sin llegar a dispararse (Hold corto...)
/// Devuelve: bool
template <typename T0 = Value>
inline Value wasActionCanceled(const T0& texto = {}) {
    return detail::api("Input.wasActionCanceled", Values{Value(texto)});
}
/// Input.wasActionCompleted("Jump")
/// termino este frame (se solto tras dispararse)
/// Devuelve: bool
template <typename T0 = Value>
inline Value wasActionCompleted(const T0& texto = {}) {
    return detail::api("Input.wasActionCompleted", Values{Value(texto)});
}
/// Input.wasActionStarted("Jump")
/// empezo este frame
/// Devuelve: bool
template <typename T0 = Value>
inline Value wasActionStarted(const T0& texto = {}) {
    return detail::api("Input.wasActionStarted", Values{Value(texto)});
}
}  // namespace Input

namespace Jobs {
/// Jobs.executed()
/// tareas ejecutadas
/// Devuelve: numero
template <typename... Mas>
inline Value executed(Mas&&... mas) {
    return detail::api("Jobs.executed", Values{Value(std::forward<Mas>(mas))...});
}
/// Jobs.workers()
/// hilos del job system
/// Devuelve: numero
template <typename... Mas>
inline Value workers(Mas&&... mas) {
    return detail::api("Jobs.workers", Values{Value(std::forward<Mas>(mas))...});
}
}  // namespace Jobs

namespace Json {
/// Json.decode(texto)
/// texto JSON -> tabla (nil y un aviso si no es JSON)
/// Devuelve: valor
template <typename T0 = Value>
inline Value decode(const T0& texto = {}) {
    return detail::api("Json.decode", Values{Value(texto)});
}
/// Json.encode(tabla, bonito)
/// tabla -> texto JSON (nil y un error en la consola si no se puede)
/// Devuelve: texto
template <typename T0 = Value, typename T1 = Value>
inline Value encode(const T0& tabla = {}, const T1& bonito = {}) {
    return detail::api("Json.encode", Values{Value(tabla), Value(bonito)});
}
}  // namespace Json

namespace Mods {
/// Mods.enabled()
/// el juego carga mods?
/// Devuelve: bool
template <typename... Mas>
inline Value enabled(Mas&&... mas) {
    return detail::api("Mods.enabled", Values{Value(std::forward<Mas>(mas))...});
}
/// Mods.isLoaded(id)
/// Devuelve: bool
template <typename T0 = Value>
inline Value isLoaded(const T0& id = {}) {
    return detail::api("Mods.isLoaded", Values{Value(id)});
}
/// Mods.list()
/// {id, name, version, author, description, enabled, loaded}
/// Devuelve: lista de objetos
template <typename... Mas>
inline Value list(Mas&&... mas) {
    return detail::api("Mods.list", Values{Value(std::forward<Mas>(mas))...});
}
/// Mods.setEnabled(id, false)
/// activa o desactiva (al volver a abrir)
/// Devuelve: bool
template <typename T0 = Value, typename T1 = Value>
inline Value setEnabled(const T0& id = {}, const T1& activar = {}) {
    return detail::api("Mods.setEnabled", Values{Value(id), Value(activar)});
}
}  // namespace Mods

namespace Navigation {
/// Navigation.findPath(desde, hasta)
/// Camino por la malla (los puntos de giro)
/// Devuelve: lista de Vec3 o nil
template <typename T0 = Value, typename T1 = Value>
inline Value findPath(const T0& desde = {}, const T1& hasta = {}) {
    return detail::api("Navigation.findPath", Values{Value(desde), Value(hasta)});
}
/// Navigation.isReady()
/// Hay malla de navegacion?
/// Devuelve: booleano
template <typename... Mas>
inline Value isReady(Mas&&... mas) {
    return detail::api("Navigation.isReady", Values{Value(std::forward<Mas>(mas))...});
}
/// Navigation.projectPoint(Vec3, radio)
/// El punto de la malla mas cercano
/// Devuelve: Vec3 o nil
template <typename T0 = Value, typename T1 = Value>
inline Value projectPoint(const T0& vec = {}, const T1& radio = {}) {
    return detail::api("Navigation.projectPoint", Values{Value(vec), Value(radio)});
}
/// Navigation.randomPoint(centro, radio)
/// Un punto al azar de la malla
/// Devuelve: Vec3 o nil
template <typename T0 = Value, typename T1 = Value>
inline Value randomPoint(const T0& centro = {}, const T1& radio = {}) {
    return detail::api("Navigation.randomPoint", Values{Value(centro), Value(radio)});
}
/// Navigation.raycast(desde, hasta)
/// Linea recta por la malla: llega?, punto del choque
/// Devuelve: lista [booleano, Vec3]
template <typename T0 = Value, typename T1 = Value>
inline Value raycast(const T0& desde = {}, const T1& hasta = {}) {
    return detail::api("Navigation.raycast", Values{Value(desde), Value(hasta)});
}
}  // namespace Navigation

namespace Network {
/// Network.SERVER: id del servidor (1) (numero)
inline Value SERVER() { return detail::apiGet("Network.SERVER"); }
/// Network.connect("127.0.0.1", 7777)
/// se une a una partida (llega onConnected u onDisconnected)
/// Devuelve: lista {ok, error}
template <typename T0 = Value, typename T1 = Value>
inline Value connect(const T0& texto = {}, const T1& valor = {}) {
    return detail::api("Network.connect", Values{Value(texto), Value(valor)});
}
/// Network.dedicated: true en un servidor dedicado (lo pone CramionServer) (bool)
inline Value dedicated() { return detail::apiGet("Network.dedicated"); }
/// Cambia Network.dedicated
inline void setDedicated(const Value& v) { detail::apiSet("Network.dedicated", v); }
/// Network.destroy(entity)
/// lo borra en todos (solo el servidor)
template <typename T0 = Value>
inline Value destroy(const T0& entity = {}) {
    return detail::api("Network.destroy", Values{Value(entity)});
}
/// Network.disconnect()
/// sale de la partida (o la cierra si eres el servidor)
template <typename... Mas>
inline Value disconnect(Mas&&... mas) {
    return detail::api("Network.disconnect", Values{Value(std::forward<Mas>(mas))...});
}
/// Network.find(netId)
/// entidad por su id de red
/// Devuelve: Entity
template <typename T0 = Value>
inline Value find(const T0& netId = {}) {
    return detail::api("Network.find", Values{Value(netId)});
}
/// Network.host(7777, 8)
/// crea la partida (eres el servidor y juegas); devuelve ok, error
/// Devuelve: lista {ok, error}
template <typename T0 = Value, typename T1 = Value>
inline Value host(const T0& valor = {}, const T1& valor2 = {}) {
    return detail::api("Network.host", Values{Value(valor), Value(valor2)});
}
/// Network.isActive()
/// hay sesion de red
/// Devuelve: bool
template <typename... Mas>
inline Value isActive(Mas&&... mas) {
    return detail::api("Network.isActive", Values{Value(std::forward<Mas>(mas))...});
}
/// Network.isClient()
/// eres un cliente?
/// Devuelve: bool
template <typename... Mas>
inline Value isClient(Mas&&... mas) {
    return detail::api("Network.isClient", Values{Value(std::forward<Mas>(mas))...});
}
/// Network.isConnected()
/// en partida (servidor abierto o cliente dentro)
/// Devuelve: bool
template <typename... Mas>
inline Value isConnected(Mas&&... mas) {
    return detail::api("Network.isConnected", Values{Value(std::forward<Mas>(mas))...});
}
/// Network.isConnecting()
/// cliente esperando respuesta
/// Devuelve: bool
template <typename... Mas>
inline Value isConnecting(Mas&&... mas) {
    return detail::api("Network.isConnecting", Values{Value(std::forward<Mas>(mas))...});
}
/// Network.isDedicated()
/// true en CramionServer (servidor dedicado sin ventana)
/// Devuelve: bool
template <typename... Mas>
inline Value isDedicated(Mas&&... mas) {
    return detail::api("Network.isDedicated", Values{Value(std::forward<Mas>(mas))...});
}
/// Network.isServer()
/// eres el servidor?
/// Devuelve: bool
template <typename... Mas>
inline Value isServer(Mas&&... mas) {
    return detail::api("Network.isServer", Values{Value(std::forward<Mas>(mas))...});
}
/// Network.lagCompensatedRaycast(origen, direccion, distancia, jugador, radio, altura)
/// servidor: disparo de un jugador contra donde el veia a los demas (compensacion de lag)
/// Devuelve: {netId, point, distance} o nil
template <typename T0 = Value, typename T1 = Value, typename T2 = Value, typename T3 = Value, typename T4 = Value, typename T5 = Value>
inline Value lagCompensatedRaycast(const T0& origen = {}, const T1& direccion = {}, const T2& distancia = {}, const T3& jugador = {}, const T4& radio = {}, const T5& altura = {}) {
    return detail::api("Network.lagCompensatedRaycast", Values{Value(origen), Value(direccion), Value(distancia), Value(jugador), Value(radio), Value(altura)});
}
/// Network.loadScene("Nivel2")
/// todos cargan la escena (solo el servidor)
template <typename T0 = Value>
inline Value loadScene(const T0& texto = {}) {
    return detail::api("Network.loadScene", Values{Value(texto)});
}
/// Network.myId()
/// tu id de jugador (el servidor es 1)
/// Devuelve: numero
template <typename... Mas>
inline Value myId(Mas&&... mas) {
    return detail::api("Network.myId", Values{Value(std::forward<Mas>(mas))...});
}
/// Network.objects()
/// todas las entidades de red
/// Devuelve: lista de Entity
template <typename... Mas>
inline Value objects(Mas&&... mas) {
    return detail::api("Network.objects", Values{Value(std::forward<Mas>(mas))...});
}
/// Network.off("chat")
/// deja de recibirlo
template <typename T0 = Value>
inline Value off(const T0& texto = {}) {
    return detail::api("Network.off", Values{Value(texto)});
}
/// Network.on("chat", function(datos, de) end)
/// recibe un mensaje
template <typename T0 = Value, typename T1 = Value>
inline Value on(const T0& texto = {}, const T1& callback = {}) {
    return detail::api("Network.on", Values{Value(texto), Value(callback)});
}
/// Network.onConnected(function(id) end)
/// cliente: ya estas dentro
template <typename T0 = Value>
inline Value onConnected(const T0& callback = {}) {
    return detail::api("Network.onConnected", Values{Value(callback)});
}
/// Network.onDisconnected(function(motivo) end)
/// fuera de la partida
template <typename T0 = Value>
inline Value onDisconnected(const T0& callback = {}) {
    return detail::api("Network.onDisconnected", Values{Value(callback)});
}
/// Network.onPlayerJoined(function(id) end)
/// entra un jugador
template <typename T0 = Value>
inline Value onPlayerJoined(const T0& callback = {}) {
    return detail::api("Network.onPlayerJoined", Values{Value(callback)});
}
/// Network.onPlayerLeft(function(id) end)
/// sale un jugador
template <typename T0 = Value>
inline Value onPlayerLeft(const T0& callback = {}) {
    return detail::api("Network.onPlayerLeft", Values{Value(callback)});
}
/// Network.ping(id)
/// ida y vuelta en ms
/// Devuelve: numero
template <typename T0 = Value>
inline Value ping(const T0& id = {}) {
    return detail::api("Network.ping", Values{Value(id)});
}
/// Network.playerCount()
/// cuantos jugadores
/// Devuelve: numero
template <typename... Mas>
inline Value playerCount(Mas&&... mas) {
    return detail::api("Network.playerCount", Values{Value(std::forward<Mas>(mas))...});
}
/// Network.players()
/// lista de ids de jugadores
/// Devuelve: lista de numeros
template <typename... Mas>
inline Value players(Mas&&... mas) {
    return detail::api("Network.players", Values{Value(std::forward<Mas>(mas))...});
}
/// Network.positionAt(netId, segundos)
/// donde estaba un objeto de red hace unos segundos (historia de 1,5 s)
/// Devuelve: Vec3
template <typename T0 = Value, typename T1 = Value>
inline Value positionAt(const T0& netId = {}, const T1& segundos = {}) {
    return detail::api("Network.positionAt", Values{Value(netId), Value(segundos)});
}
/// Network.send("chat", datos, destino)
/// mensaje (destino: nil = todos, "server" o un id)
/// Devuelve: bool
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value send(const T0& texto = {}, const T1& datos = {}, const T2& destino = {}) {
    return detail::api("Network.send", Values{Value(texto), Value(datos), Value(destino)});
}
/// Network.simulate({ latency = 120, jitter = 30, loss = 5 })
/// simula una red mala (ms de retraso, variacion y % de perdida; 0 = normal)
template <typename T0 = Value>
inline Value simulate(const T0& tabla = {}) {
    return detail::api("Network.simulate", Values{Value(tabla)});
}
/// Network.spawn("Prefabs/Jugador", posicion, dueno, giro)
/// crea un objeto de red en todos (solo el servidor)
/// Devuelve: Entity
template <typename T0 = Value, typename T1 = Value, typename T2 = Value, typename T3 = Value>
inline Value spawn(const T0& texto = {}, const T1& posicion = {}, const T2& dueno = {}, const T3& giro = {}) {
    return detail::api("Network.spawn", Values{Value(texto), Value(posicion), Value(dueno), Value(giro)});
}
/// Network.stats()
/// {sent, received, objects, sendRate, receiveRate, bytesSent, bytesReceived, ping, packetLoss}
/// Devuelve: objeto
template <typename... Mas>
inline Value stats(Mas&&... mas) {
    return detail::api("Network.stats", Values{Value(std::forward<Mas>(mas))...});
}
}  // namespace Network

namespace Physics {
/// Physics.ignoreCollision(a, b, true)
/// a y b no chocan entre si (false lo deshace)
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value ignoreCollision(const T0& a = {}, const T1& b = {}, const T2& activar = {}) {
    return detail::api("Physics.ignoreCollision", Values{Value(a), Value(b), Value(activar)});
}
}  // namespace Physics

namespace Physics2D {
/// Physics2D.getGravity()
/// Vec3 gravedad 2D
/// Devuelve: Vec3
template <typename... Mas>
inline Value getGravity(Mas&&... mas) {
    return detail::api("Physics2D.getGravity", Values{Value(std::forward<Mas>(mas))...});
}
/// Physics2D.overlapBox(centro, tamano, angulo, mascara)
/// objetos dentro de la caja
/// Devuelve: lista de Entity
template <typename T0 = Value, typename T1 = Value, typename T2 = Value, typename T3 = Value>
inline Value overlapBox(const T0& centro = {}, const T1& tamano = {}, const T2& angulo = {}, const T3& mascara = {}) {
    return detail::api("Physics2D.overlapBox", Values{Value(centro), Value(tamano), Value(angulo), Value(mascara)});
}
/// Physics2D.overlapCircle(centro, radio, mascara)
/// objetos con collider 2D dentro del circulo
/// Devuelve: lista de Entity
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value overlapCircle(const T0& centro = {}, const T1& radio = {}, const T2& mascara = {}) {
    return detail::api("Physics2D.overlapCircle", Values{Value(centro), Value(radio), Value(mascara)});
}
/// Physics2D.overlapPoint(punto, mascara)
/// objetos que tocan el punto
/// Devuelve: lista de Entity
template <typename T0 = Value, typename T1 = Value>
inline Value overlapPoint(const T0& punto = {}, const T1& mascara = {}) {
    return detail::api("Physics2D.overlapPoint", Values{Value(punto), Value(mascara)});
}
/// Physics2D.raycast(origen, direccion, distancia, mascara)
/// nil o {entity, point, normal, distance, fraction}
/// Devuelve: objeto
template <typename T0 = Value, typename T1 = Value, typename T2 = Value, typename T3 = Value>
inline Value raycast(const T0& origen = {}, const T1& direccion = {}, const T2& distancia = {}, const T3& mascara = {}) {
    return detail::api("Physics2D.raycast", Values{Value(origen), Value(direccion), Value(distancia), Value(mascara)});
}
/// Physics2D.raycastAll(origen, direccion, distancia, mascara)
/// lista de choques, del mas cercano al mas lejano
/// Devuelve: lista de objetos
template <typename T0 = Value, typename T1 = Value, typename T2 = Value, typename T3 = Value>
inline Value raycastAll(const T0& origen = {}, const T1& direccion = {}, const T2& distancia = {}, const T3& mascara = {}) {
    return detail::api("Physics2D.raycastAll", Values{Value(origen), Value(direccion), Value(distancia), Value(mascara)});
}
/// Physics2D.setGravity(Vec3(0, -9.81, 0))
/// cambia la gravedad 2D
template <typename T0 = Value>
inline Value setGravity(const T0& arg = {}) {
    return detail::api("Physics2D.setGravity", Values{Value(arg)});
}
}  // namespace Physics2D

namespace Prefs {
/// Prefs.deleteAll()
/// Borra todo
template <typename... Mas>
inline Value deleteAll(Mas&&... mas) {
    return detail::api("Prefs.deleteAll", Values{Value(std::forward<Mas>(mas))...});
}
/// Prefs.deleteKey("clave")
/// La borra
template <typename T0 = Value>
inline Value deleteKey(const T0& texto = {}) {
    return detail::api("Prefs.deleteKey", Values{Value(texto)});
}
/// Prefs.getFloat("clave", 0.0)
/// Lee un numero
/// Devuelve: numero
template <typename T0 = Value, typename T1 = Value>
inline Value getFloat(const T0& texto = {}, const T1& valor = {}) {
    return detail::api("Prefs.getFloat", Values{Value(texto), Value(valor)});
}
/// Prefs.getInt("clave", 0)
/// Lee un entero (o el valor por defecto)
/// Devuelve: numero
template <typename T0 = Value, typename T1 = Value>
inline Value getInt(const T0& texto = {}, const T1& valor = {}) {
    return detail::api("Prefs.getInt", Values{Value(texto), Value(valor)});
}
/// Prefs.getString("clave", "")
/// Lee un texto
/// Devuelve: texto
template <typename T0 = Value, typename T1 = Value>
inline Value getString(const T0& texto = {}, const T1& texto2 = {}) {
    return detail::api("Prefs.getString", Values{Value(texto), Value(texto2)});
}
/// Prefs.hasKey("clave")
/// Existe?
/// Devuelve: bool
template <typename T0 = Value>
inline Value hasKey(const T0& texto = {}) {
    return detail::api("Prefs.hasKey", Values{Value(texto)});
}
/// Prefs.setFloat("clave", 0.5)
/// Guarda un numero
template <typename T0 = Value, typename T1 = Value>
inline Value setFloat(const T0& texto = {}, const T1& valor = {}) {
    return detail::api("Prefs.setFloat", Values{Value(texto), Value(valor)});
}
/// Prefs.setInt("clave", 3)
/// Guarda un entero
template <typename T0 = Value, typename T1 = Value>
inline Value setInt(const T0& texto = {}, const T1& valor = {}) {
    return detail::api("Prefs.setInt", Values{Value(texto), Value(valor)});
}
/// Prefs.setString("clave", "texto")
/// Guarda un texto
template <typename T0 = Value, typename T1 = Value>
inline Value setString(const T0& texto = {}, const T1& texto2 = {}) {
    return detail::api("Prefs.setString", Values{Value(texto), Value(texto2)});
}
}  // namespace Prefs

namespace Profiler {
/// Profiler.begin("nombre")
/// Empieza una zona propia del perfilador (Insights)
template <typename T0 = Value>
inline Value begin(const T0& texto = {}) {
    return detail::api("Profiler.begin", Values{Value(texto)});
}
/// Profiler.capture(frames)
/// Guarda una captura .crtrace de los ultimos frames (300) y devuelve su ruta
/// Devuelve: texto
template <typename T0 = Value>
inline Value capture(const T0& frames = {}) {
    return detail::api("Profiler.capture", Values{Value(frames)});
}
/// Profiler.counter("nombre", valor)
/// Apunta el valor de un contador en este frame
template <typename T0 = Value, typename T1 = Value>
inline Value counter(const T0& texto = {}, const T1& valor = {}) {
    return detail::api("Profiler.counter", Values{Value(texto), Value(valor)});
}
/// Profiler.finish()
/// Termina la ultima zona empezada con Profiler.begin
template <typename... Mas>
inline Value finish(Mas&&... mas) {
    return detail::api("Profiler.finish", Values{Value(std::forward<Mas>(mas))...});
}
/// Profiler.frameMs()
/// Duracion del ultimo frame (ms)
/// Devuelve: numero
template <typename... Mas>
inline Value frameMs(Mas&&... mas) {
    return detail::api("Profiler.frameMs", Values{Value(std::forward<Mas>(mas))...});
}
/// Profiler.zones(cuantas)
/// Las zonas que mas cuestan (10): lista de {name, ms, self, max}
/// Devuelve: lista de objetos
template <typename T0 = Value>
inline Value zones(const T0& cuantas = {}) {
    return detail::api("Profiler.zones", Values{Value(cuantas)});
}
}  // namespace Profiler

namespace Random {
/// Random.chance(0.25)
/// true con esa probabilidad
/// Devuelve: bool
template <typename T0 = Value>
inline Value chance(const T0& valor = {}) {
    return detail::api("Random.chance", Values{Value(valor)});
}
/// Random.insideUnitCircle()
/// Punto en el suelo (XZ)
/// Devuelve: Vec3
template <typename... Mas>
inline Value insideUnitCircle(Mas&&... mas) {
    return detail::api("Random.insideUnitCircle", Values{Value(std::forward<Mas>(mas))...});
}
/// Random.insideUnitSphere()
/// Punto dentro de la esfera
/// Devuelve: Vec3
template <typename... Mas>
inline Value insideUnitSphere(Mas&&... mas) {
    return detail::api("Random.insideUnitSphere", Values{Value(std::forward<Mas>(mas))...});
}
/// Random.int(min, max)
/// Entero (incluye los dos)
/// Devuelve: numero
template <typename T0 = Value, typename T1 = Value>
inline Value int_(const T0& min_ = {}, const T1& max_ = {}) {
    return detail::api("Random.int", Values{Value(min_), Value(max_)});
}
/// Random.onUnitSphere()
/// Direccion al azar
/// Devuelve: Vec3
template <typename... Mas>
inline Value onUnitSphere(Mas&&... mas) {
    return detail::api("Random.onUnitSphere", Values{Value(std::forward<Mas>(mas))...});
}
/// Random.pick(lista)
/// Un elemento al azar
/// Devuelve: valor
template <typename T0 = Value>
inline Value pick(const T0& lista = {}) {
    return detail::api("Random.pick", Values{Value(lista)});
}
/// Random.range(min, max)
/// Decimal entre min y max
/// Devuelve: numero
template <typename T0 = Value, typename T1 = Value>
inline Value range(const T0& min_ = {}, const T1& max_ = {}) {
    return detail::api("Random.range", Values{Value(min_), Value(max_)});
}
/// Random.rotation()
/// Quat al azar
/// Devuelve: Quat
template <typename... Mas>
inline Value rotation(Mas&&... mas) {
    return detail::api("Random.rotation", Values{Value(std::forward<Mas>(mas))...});
}
/// Random.seed(n)
/// Fija la semilla
template <typename T0 = Value>
inline Value seed(const T0& n = {}) {
    return detail::api("Random.seed", Values{Value(n)});
}
/// Random.shuffle(lista)
/// Baraja
/// Devuelve: lista
template <typename T0 = Value>
inline Value shuffle(const T0& lista = {}) {
    return detail::api("Random.shuffle", Values{Value(lista)});
}
/// Random.sign()
/// -1 o 1
/// Devuelve: numero
template <typename... Mas>
inline Value sign(Mas&&... mas) {
    return detail::api("Random.sign", Values{Value(std::forward<Mas>(mas))...});
}
/// Random.value()
/// 0..1
/// Devuelve: numero
template <typename... Mas>
inline Value value(Mas&&... mas) {
    return detail::api("Random.value", Values{Value(std::forward<Mas>(mas))...});
}
}  // namespace Random

namespace Replay {
/// Replay.duration()
/// segundos grabados
/// Devuelve: numero
template <typename... Mas>
inline Value duration(Mas&&... mas) {
    return detail::api("Replay.duration", Values{Value(std::forward<Mas>(mas))...});
}
/// Replay.isPlaying()
/// esta reproduciendo?
/// Devuelve: bool
template <typename... Mas>
inline Value isPlaying(Mas&&... mas) {
    return detail::api("Replay.isPlaying", Values{Value(std::forward<Mas>(mas))...});
}
/// Replay.isRecording()
/// esta grabando?
/// Devuelve: bool
template <typename... Mas>
inline Value isRecording(Mas&&... mas) {
    return detail::api("Replay.isRecording", Values{Value(std::forward<Mas>(mas))...});
}
/// Replay.list()
/// repeticiones guardadas
/// Devuelve: lista de textos
template <typename... Mas>
inline Value list(Mas&&... mas) {
    return detail::api("Replay.list", Values{Value(std::forward<Mas>(mas))...});
}
/// Replay.load("gol")
/// carga un .crreplay
/// Devuelve: bool
template <typename T0 = Value>
inline Value load(const T0& texto = {}) {
    return detail::api("Replay.load", Values{Value(texto)});
}
/// Replay.mark("Gol", datos)
/// marcador en la linea de tiempo
template <typename T0 = Value, typename T1 = Value>
inline Value mark(const T0& texto = {}, const T1& datos = {}) {
    return detail::api("Replay.mark", Values{Value(texto), Value(datos)});
}
/// Replay.pause(true)
/// pausa la reproduccion
template <typename T0 = Value>
inline Value pause(const T0& activar = {}) {
    return detail::api("Replay.pause", Values{Value(activar)});
}
/// Replay.play(desde, velocidad)
/// reproduce (desde < 0 = los ultimos N segundos)
/// Devuelve: bool
template <typename T0 = Value, typename T1 = Value>
inline Value play(const T0& desde = {}, const T1& velocidad = {}) {
    return detail::api("Replay.play", Values{Value(desde), Value(velocidad)});
}
/// Replay.save("gol")
/// guarda un .crreplay
/// Devuelve: bool
template <typename T0 = Value>
inline Value save(const T0& texto = {}) {
    return detail::api("Replay.save", Values{Value(texto)});
}
/// Replay.seek(segundos)
/// salta a ese momento
template <typename T0 = Value>
inline Value seek(const T0& segundos = {}) {
    return detail::api("Replay.seek", Values{Value(segundos)});
}
/// Replay.setFreeCamera(true)
/// camara libre (WASD + raton)
template <typename T0 = Value>
inline Value setFreeCamera(const T0& activar = {}) {
    return detail::api("Replay.setFreeCamera", Values{Value(activar)});
}
/// Replay.setLoop(true)
/// en bucle
template <typename T0 = Value>
inline Value setLoop(const T0& activar = {}) {
    return detail::api("Replay.setLoop", Values{Value(activar)});
}
/// Replay.setSpeed(0.25)
/// camara lenta / rapida
template <typename T0 = Value>
inline Value setSpeed(const T0& valor = {}) {
    return detail::api("Replay.setSpeed", Values{Value(valor)});
}
/// Replay.start({rate = 30, maxSeconds = 10, tag = "Coche"})
/// empieza a grabar
/// Devuelve: bool
template <typename T0 = Value>
inline Value start(const T0& tabla = {}) {
    return detail::api("Replay.start", Values{Value(tabla)});
}
/// Replay.stop()
/// deja de grabar
template <typename... Mas>
inline Value stop(Mas&&... mas) {
    return detail::api("Replay.stop", Values{Value(std::forward<Mas>(mas))...});
}
/// Replay.stopPlayback()
/// vuelve al juego
template <typename... Mas>
inline Value stopPlayback(Mas&&... mas) {
    return detail::api("Replay.stopPlayback", Values{Value(std::forward<Mas>(mas))...});
}
/// Replay.time()
/// segundo de la reproduccion
/// Devuelve: numero
template <typename... Mas>
inline Value time(Mas&&... mas) {
    return detail::api("Replay.time", Values{Value(std::forward<Mas>(mas))...});
}
}  // namespace Replay

namespace Save {
/// Save.clearValues()
/// borra todos
template <typename... Mas>
inline Value clearValues(Mas&&... mas) {
    return detail::api("Save.clearValues", Values{Value(std::forward<Mas>(mas))...});
}
/// Save.delete("slot1")
/// borra una ranura
/// Devuelve: booleano
template <typename T0 = Value>
inline Value delete_(const T0& texto = {}) {
    return detail::api("Save.delete", Values{Value(texto)});
}
/// Save.deleteValue("oro")
/// lo borra
template <typename T0 = Value>
inline Value deleteValue(const T0& texto = {}) {
    return detail::api("Save.deleteValue", Values{Value(texto)});
}
/// Save.exists("slot1")
/// existe?
/// Devuelve: booleano
template <typename T0 = Value>
inline Value exists(const T0& texto = {}) {
    return detail::api("Save.exists", Values{Value(texto)});
}
/// Save.folder()
/// carpeta de las partidas
/// Devuelve: texto
template <typename... Mas>
inline Value folder(Mas&&... mas) {
    return detail::api("Save.folder", Values{Value(std::forward<Mas>(mas))...});
}
/// Save.getValue("oro", 0)
/// lee un valor suelto
/// Devuelve: valor
template <typename T0 = Value, typename T1 = Value>
inline Value getValue(const T0& texto = {}, const T1& valor = {}) {
    return detail::api("Save.getValue", Values{Value(texto), Value(valor)});
}
/// Save.hasValue("oro")
/// existe?
/// Devuelve: booleano
template <typename T0 = Value>
inline Value hasValue(const T0& texto = {}) {
    return detail::api("Save.hasValue", Values{Value(texto)});
}
/// Save.info("slot1")
/// datos de una ranura (o nil)
/// Devuelve: objeto o nil
template <typename T0 = Value>
inline Value info(const T0& texto = {}) {
    return detail::api("Save.info", Values{Value(texto)});
}
/// Save.isWriting()
/// esta escribiendo en segundo plano?
/// Devuelve: booleano
template <typename... Mas>
inline Value isWriting(Mas&&... mas) {
    return detail::api("Save.isWriting", Values{Value(std::forward<Mas>(mas))...});
}
/// Save.list()
/// {slot, label, scene, date, playtime, size} de cada ranura
/// Devuelve: lista de objetos
template <typename... Mas>
inline Value list(Mas&&... mas) {
    return detail::api("Save.list", Values{Value(std::forward<Mas>(mas))...});
}
/// Save.load("slot1")
/// carga una partida (cambia de escena si hace falta)
/// Devuelve: booleano
template <typename T0 = Value>
inline Value load(const T0& texto = {}) {
    return detail::api("Save.load", Values{Value(texto)});
}
/// Save.onLoaded(function(slot) end)
/// despues de cargar una partida
template <typename T0 = Value>
inline Value onLoaded(const T0& callback = {}) {
    return detail::api("Save.onLoaded", Values{Value(callback)});
}
/// Save.playtime()
/// segundos jugados
/// Devuelve: numero
template <typename... Mas>
inline Value playtime(Mas&&... mas) {
    return detail::api("Save.playtime", Values{Value(std::forward<Mas>(mas))...});
}
/// Save.remove("slot1")
/// borra una ranura (lo mismo que delete)
/// Devuelve: booleano
template <typename T0 = Value>
inline Value remove(const T0& texto = {}) {
    return detail::api("Save.remove", Values{Value(texto)});
}
/// Save.save("slot1", "Etiqueta")
/// guarda la partida (objetos Saveable, valores, dialogos)
/// Devuelve: booleano
template <typename T0 = Value, typename T1 = Value>
inline Value save(const T0& texto = {}, const T1& texto2 = {}) {
    return detail::api("Save.save", Values{Value(texto), Value(texto2)});
}
/// Save.setAutosave(60, "autosave")
/// autoguardado cada N segundos (0 = no)
template <typename T0 = Value, typename T1 = Value>
inline Value setAutosave(const T0& valor = {}, const T1& texto = {}) {
    return detail::api("Save.setAutosave", Values{Value(valor), Value(texto)});
}
/// Save.setCompression(true)
/// partidas comprimidas
template <typename T0 = Value>
inline Value setCompression(const T0& activar = {}) {
    return detail::api("Save.setCompression", Values{Value(activar)});
}
/// Save.setValue("oro", 120)
/// valor suelto (va en cada partida)
template <typename T0 = Value, typename T1 = Value>
inline Value setValue(const T0& texto = {}, const T1& valor = {}) {
    return detail::api("Save.setValue", Values{Value(texto), Value(valor)});
}
}  // namespace Save

namespace Scene {
/// Scene.destroy(entity)
/// Lo destruye al final del frame
template <typename T0 = Value>
inline Value destroy(const T0& entity = {}) {
    return detail::api("Scene.destroy", Values{Value(entity)});
}
/// Scene.findAllWithTag("tag")
/// Lista de objetos con ese tag
/// Devuelve: lista de Entity
template <typename T0 = Value>
inline Value findAllWithTag(const T0& texto = {}) {
    return detail::api("Scene.findAllWithTag", Values{Value(texto)});
}
/// Scene.instantiate(entity o "Prefabs/Enemigo", posicion, rotacion)
/// Copia de un objeto (con hijos y componentes) o instancia de un prefab
/// Devuelve: Entity
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value instantiate(const T0& arg = {}, const T1& posicion = {}, const T2& rotacion = {}) {
    return detail::api("Scene.instantiate", Values{Value(arg), Value(posicion), Value(rotacion)});
}
/// Scene.load("Nivel2")
/// Cambia de escena al terminar el frame (nombre o ruta del .crscene)
/// Devuelve: bool
template <typename T0 = Value>
inline Value load(const T0& texto = {}) {
    return detail::api("Scene.load", Values{Value(texto)});
}
/// Scene.name()
/// Nombre de la escena actual
/// Devuelve: texto
template <typename... Mas>
inline Value name(Mas&&... mas) {
    return detail::api("Scene.name", Values{Value(std::forward<Mas>(mas))...});
}
/// Scene.origin()
/// x, y, z (doble precision) del origen flotante del mundo
/// Devuelve: lista de 3 numeros
template <typename... Mas>
inline Value origin(Mas&&... mas) {
    return detail::api("Scene.origin", Values{Value(std::forward<Mas>(mas))...});
}
/// Scene.toAbsolute(posicion)
/// x, y, z absolutos (para guardar posiciones en una partida)
/// Devuelve: lista de 3 numeros
template <typename T0 = Value>
inline Value toAbsolute(const T0& posicion = {}) {
    return detail::api("Scene.toAbsolute", Values{Value(posicion)});
}
/// Scene.toLocal(x, y, z)
/// Vec3 local de una posicion absoluta guardada
/// Devuelve: Vec3
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value toLocal(const T0& x = {}, const T1& y = {}, const T2& z = {}) {
    return detail::api("Scene.toLocal", Values{Value(x), Value(y), Value(z)});
}
}  // namespace Scene

namespace Screen {
/// Screen.height()
/// alto en pixeles
/// Devuelve: numero
template <typename... Mas>
inline Value height(Mas&&... mas) {
    return detail::api("Screen.height", Values{Value(std::forward<Mas>(mas))...});
}
/// Screen.orientation()
/// "landscape" o "portrait" segun el tamano
/// Devuelve: texto
template <typename... Mas>
inline Value orientation(Mas&&... mas) {
    return detail::api("Screen.orientation", Values{Value(std::forward<Mas>(mas))...});
}
/// Screen.orientationMode()
/// el ultimo modo pedido
/// Devuelve: texto
template <typename... Mas>
inline Value orientationMode(Mas&&... mas) {
    return detail::api("Screen.orientationMode", Values{Value(std::forward<Mas>(mas))...});
}
/// Screen.setOrientation("landscape")
/// auto, landscape, portrait, landscape_fixed, portrait_fixed (moviles)
/// Devuelve: bool
template <typename T0 = Value>
inline Value setOrientation(const T0& texto = {}) {
    return detail::api("Screen.setOrientation", Values{Value(texto)});
}
/// Screen.width()
/// ancho en pixeles
/// Devuelve: numero
template <typename... Mas>
inline Value width(Mas&&... mas) {
    return detail::api("Screen.width", Values{Value(std::forward<Mas>(mas))...});
}
}  // namespace Screen

namespace Spline {
/// Spline.create({Vec3(0,0,0), Vec3(0,0,20)}, "road", "Camino", cerrada)
/// crea una spline (road, path, river, wall, fence, pipe, rails, ribbon o nada)
/// Devuelve: Entity
template <typename T0 = Value, typename T1 = Value, typename T2 = Value, typename T3 = Value>
inline Value create(const T0& tabla = {}, const T1& texto = {}, const T2& texto2 = {}, const T3& cerrada = {}) {
    return detail::api("Spline.create", Values{Value(tabla), Value(texto), Value(texto2), Value(cerrada)});
}
}  // namespace Spline

namespace Steam {
/// Steam.achievementProgress("COLECCIONISTA", 5, 10)
/// muestra el progreso
/// Devuelve: bool
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value achievementProgress(const T0& texto = {}, const T1& valor = {}, const T2& valor2 = {}) {
    return detail::api("Steam.achievementProgress", Values{Value(texto), Value(valor), Value(valor2)});
}
/// Steam.appId()
/// AppID
/// Devuelve: numero
template <typename... Mas>
inline Value appId(Mas&&... mas) {
    return detail::api("Steam.appId", Values{Value(std::forward<Mas>(mas))...});
}
/// Steam.available()
/// Steam esta abierto y la DLL cargada
/// Devuelve: bool
template <typename... Mas>
inline Value available(Mas&&... mas) {
    return detail::api("Steam.available", Values{Value(std::forward<Mas>(mas))...});
}
/// Steam.clearAchievement("PRIMERA_SANGRE")
/// lo vuelve a bloquear (pruebas)
/// Devuelve: bool
template <typename T0 = Value>
inline Value clearAchievement(const T0& texto = {}) {
    return detail::api("Steam.clearAchievement", Values{Value(texto)});
}
/// Steam.clearRichPresence()
/// lo borra
template <typename... Mas>
inline Value clearRichPresence(Mas&&... mas) {
    return detail::api("Steam.clearRichPresence", Values{Value(std::forward<Mas>(mas))...});
}
/// Steam.cloudDelete("partida.json")
/// lo borra
/// Devuelve: bool
template <typename T0 = Value>
inline Value cloudDelete(const T0& texto = {}) {
    return detail::api("Steam.cloudDelete", Values{Value(texto)});
}
/// Steam.cloudEnabled()
/// Steam Cloud activo?
/// Devuelve: bool
template <typename... Mas>
inline Value cloudEnabled(Mas&&... mas) {
    return detail::api("Steam.cloudEnabled", Values{Value(std::forward<Mas>(mas))...});
}
/// Steam.cloudExists("partida.json")
/// existe?
/// Devuelve: bool
template <typename T0 = Value>
inline Value cloudExists(const T0& texto = {}) {
    return detail::api("Steam.cloudExists", Values{Value(texto)});
}
/// Steam.cloudFiles()
/// {name, size} de cada archivo
/// Devuelve: lista de objetos
template <typename... Mas>
inline Value cloudFiles(Mas&&... mas) {
    return detail::api("Steam.cloudFiles", Values{Value(std::forward<Mas>(mas))...});
}
/// Steam.cloudRead("partida.json")
/// lee de la nube (o nil)
/// Devuelve: texto
template <typename T0 = Value>
inline Value cloudRead(const T0& texto = {}) {
    return detail::api("Steam.cloudRead", Values{Value(texto)});
}
/// Steam.cloudWrite("partida.json", texto)
/// guarda en la nube
/// Devuelve: bool
template <typename T0 = Value, typename T1 = Value>
inline Value cloudWrite(const T0& texto = {}, const T1& texto2 = {}) {
    return detail::api("Steam.cloudWrite", Values{Value(texto), Value(texto2)});
}
/// Steam.createLobby("public", 4, function(ok, sala) end)
/// crea una sala
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value createLobby(const T0& texto = {}, const T1& valor = {}, const T2& callback = {}) {
    return detail::api("Steam.createLobby", Values{Value(texto), Value(valor), Value(callback)});
}
/// Steam.downloadScores("Puntos", "global", 1, 10, function(ok, filas) end)
/// lee el marcador
template <typename T0 = Value, typename T1 = Value, typename T2 = Value, typename T3 = Value, typename T4 = Value>
inline Value downloadScores(const T0& texto = {}, const T1& texto2 = {}, const T2& valor = {}, const T3& valor2 = {}, const T4& callback = {}) {
    return detail::api("Steam.downloadScores", Values{Value(texto), Value(texto2), Value(valor), Value(valor2), Value(callback)});
}
/// Steam.error()
/// por que no lo esta
/// Devuelve: texto
template <typename... Mas>
inline Value error(Mas&&... mas) {
    return detail::api("Steam.error", Values{Value(std::forward<Mas>(mas))...});
}
/// Steam.findLobbies({modo = "coop"}, 20, function(ok, salas) end)
/// busca salas
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value findLobbies(const T0& tabla = {}, const T1& valor = {}, const T2& callback = {}) {
    return detail::api("Steam.findLobbies", Values{Value(tabla), Value(valor), Value(callback)});
}
/// Steam.friendName(id)
/// nombre de un amigo
/// Devuelve: texto
template <typename T0 = Value>
inline Value friendName(const T0& id = {}) {
    return detail::api("Steam.friendName", Values{Value(id)});
}
/// Steam.getLobbyData(sala, "ip")
/// lo lee
/// Devuelve: texto
template <typename T0 = Value, typename T1 = Value>
inline Value getLobbyData(const T0& sala = {}, const T1& texto = {}) {
    return detail::api("Steam.getLobbyData", Values{Value(sala), Value(texto)});
}
/// Steam.getStatFloat("km")
/// lee una estadistica (nil si no hay)
/// Devuelve: numero
template <typename T0 = Value>
inline Value getStatFloat(const T0& texto = {}) {
    return detail::api("Steam.getStatFloat", Values{Value(texto)});
}
/// Steam.getStatInt("partidas")
/// lee una estadistica (nil si no hay)
/// Devuelve: numero
template <typename T0 = Value>
inline Value getStatInt(const T0& texto = {}) {
    return detail::api("Steam.getStatInt", Values{Value(texto)});
}
/// Steam.inviteToLobby(sala)
/// dialogo de invitar del overlay
template <typename T0 = Value>
inline Value inviteToLobby(const T0& sala = {}) {
    return detail::api("Steam.inviteToLobby", Values{Value(sala)});
}
/// Steam.isAchievementUnlocked("PRIMERA_SANGRE")
/// esta desbloqueado?
/// Devuelve: bool
template <typename T0 = Value>
inline Value isAchievementUnlocked(const T0& texto = {}) {
    return detail::api("Steam.isAchievementUnlocked", Values{Value(texto)});
}
/// Steam.isDlcInstalled(appId)
/// tiene el DLC?
/// Devuelve: bool
template <typename T0 = Value>
inline Value isDlcInstalled(const T0& appId = {}) {
    return detail::api("Steam.isDlcInstalled", Values{Value(appId)});
}
/// Steam.isSteamDeck()
/// corre en una Steam Deck?
/// Devuelve: bool
template <typename... Mas>
inline Value isSteamDeck(Mas&&... mas) {
    return detail::api("Steam.isSteamDeck", Values{Value(std::forward<Mas>(mas))...});
}
/// Steam.joinLobby(sala, function(ok, sala) end)
/// entra en una sala
template <typename T0 = Value, typename T1 = Value>
inline Value joinLobby(const T0& sala = {}, const T1& callback = {}) {
    return detail::api("Steam.joinLobby", Values{Value(sala), Value(callback)});
}
/// Steam.language()
/// idioma de Steam
/// Devuelve: texto
template <typename... Mas>
inline Value language(Mas&&... mas) {
    return detail::api("Steam.language", Values{Value(std::forward<Mas>(mas))...});
}
/// Steam.leaveLobby(sala)
/// sale
template <typename T0 = Value>
inline Value leaveLobby(const T0& sala = {}) {
    return detail::api("Steam.leaveLobby", Values{Value(sala)});
}
/// Steam.lobbyMembers(sala)
/// {id, name} de cada jugador
/// Devuelve: lista de objetos
template <typename T0 = Value>
inline Value lobbyMembers(const T0& sala = {}) {
    return detail::api("Steam.lobbyMembers", Values{Value(sala)});
}
/// Steam.lobbyOwner(sala)
/// SteamID del dueno
/// Devuelve: texto
template <typename T0 = Value>
inline Value lobbyOwner(const T0& sala = {}) {
    return detail::api("Steam.lobbyOwner", Values{Value(sala)});
}
/// Steam.onLobbyJoinRequested(function(sala) end)
/// un amigo invito y el jugador acepto
template <typename T0 = Value>
inline Value onLobbyJoinRequested(const T0& callback = {}) {
    return detail::api("Steam.onLobbyJoinRequested", Values{Value(callback)});
}
/// Steam.onOverlay(function(abierto) end)
/// se abrio o cerro el overlay (pausar)
template <typename T0 = Value>
inline Value onOverlay(const T0& callback = {}) {
    return detail::api("Steam.onOverlay", Values{Value(callback)});
}
/// Steam.openOverlay("friends")
/// abre el overlay
template <typename T0 = Value>
inline Value openOverlay(const T0& texto = {}) {
    return detail::api("Steam.openOverlay", Values{Value(texto)});
}
/// Steam.openOverlayUrl("https://...")
/// web en el overlay
template <typename T0 = Value>
inline Value openOverlayUrl(const T0& texto = {}) {
    return detail::api("Steam.openOverlayUrl", Values{Value(texto)});
}
/// Steam.openStore()
/// la pagina de la tienda
template <typename... Mas>
inline Value openStore(Mas&&... mas) {
    return detail::api("Steam.openStore", Values{Value(std::forward<Mas>(mas))...});
}
/// Steam.overlayActive()
/// esta abierto ahora?
/// Devuelve: bool
template <typename... Mas>
inline Value overlayActive(Mas&&... mas) {
    return detail::api("Steam.overlayActive", Values{Value(std::forward<Mas>(mas))...});
}
/// Steam.overlayEnabled()
/// el overlay funciona?
/// Devuelve: bool
template <typename... Mas>
inline Value overlayEnabled(Mas&&... mas) {
    return detail::api("Steam.overlayEnabled", Values{Value(std::forward<Mas>(mas))...});
}
/// Steam.setLobbyData(sala, "ip", "1.2.3.4:7777")
/// dato de la sala
/// Devuelve: bool
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value setLobbyData(const T0& sala = {}, const T1& texto = {}, const T2& texto2 = {}) {
    return detail::api("Steam.setLobbyData", Values{Value(sala), Value(texto), Value(texto2)});
}
/// Steam.setRichPresence("steam_display", "#Jugando")
/// estado que ven los amigos
/// Devuelve: bool
template <typename T0 = Value, typename T1 = Value>
inline Value setRichPresence(const T0& texto = {}, const T1& texto2 = {}) {
    return detail::api("Steam.setRichPresence", Values{Value(texto), Value(texto2)});
}
/// Steam.setStatFloat("km", 4.5)
/// estadistica decimal
/// Devuelve: bool
template <typename T0 = Value, typename T1 = Value>
inline Value setStatFloat(const T0& texto = {}, const T1& valor = {}) {
    return detail::api("Steam.setStatFloat", Values{Value(texto), Value(valor)});
}
/// Steam.setStatInt("partidas", 3)
/// estadistica entera
/// Devuelve: bool
template <typename T0 = Value, typename T1 = Value>
inline Value setStatInt(const T0& texto = {}, const T1& valor = {}) {
    return detail::api("Steam.setStatInt", Values{Value(texto), Value(valor)});
}
/// Steam.storeStats()
/// envia logros y estadisticas a Steam
/// Devuelve: bool
template <typename... Mas>
inline Value storeStats(Mas&&... mas) {
    return detail::api("Steam.storeStats", Values{Value(std::forward<Mas>(mas))...});
}
/// Steam.unlockAchievement("PRIMERA_SANGRE")
/// desbloquea un logro
/// Devuelve: bool
template <typename T0 = Value>
inline Value unlockAchievement(const T0& texto = {}) {
    return detail::api("Steam.unlockAchievement", Values{Value(texto)});
}
/// Steam.uploadScore("Puntos", 1200, function(ok, puesto) end)
/// sube una puntuacion al marcador
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value uploadScore(const T0& texto = {}, const T1& valor = {}, const T2& callback = {}) {
    return detail::api("Steam.uploadScore", Values{Value(texto), Value(valor), Value(callback)});
}
/// Steam.userId()
/// SteamID del jugador
/// Devuelve: texto
template <typename... Mas>
inline Value userId(Mas&&... mas) {
    return detail::api("Steam.userId", Values{Value(std::forward<Mas>(mas))...});
}
/// Steam.userName()
/// nombre del jugador
/// Devuelve: texto
template <typename... Mas>
inline Value userName(Mas&&... mas) {
    return detail::api("Steam.userName", Values{Value(std::forward<Mas>(mas))...});
}
/// Steam.workshopItems()
/// objetos del Workshop suscritos
/// Devuelve: lista de objetos
template <typename... Mas>
inline Value workshopItems(Mas&&... mas) {
    return detail::api("Steam.workshopItems", Values{Value(std::forward<Mas>(mas))...});
}
/// Steam.workshopProgress()
/// 0..1 de la subida (-1 si no hay)
/// Devuelve: numero
template <typename... Mas>
inline Value workshopProgress(Mas&&... mas) {
    return detail::api("Steam.workshopProgress", Values{Value(std::forward<Mas>(mas))...});
}
/// Steam.workshopUpload({title, description, folder, preview, tags}, function(ok, id) end)
/// sube al Workshop
template <typename T0 = Value, typename T1 = Value>
inline Value workshopUpload(const T0& tabla = {}, const T1& callback = {}) {
    return detail::api("Steam.workshopUpload", Values{Value(tabla), Value(callback)});
}
}  // namespace Steam

namespace Test {
/// Test.begin("nombre", archivo)
/// empieza un caso de prueba (termina el anterior)
template <typename T0 = Value, typename T1 = Value>
inline Value begin(const T0& texto = {}, const T1& archivo = {}) {
    return detail::api("Test.begin", Values{Value(texto), Value(archivo)});
}
/// Test.check(ok, "mensaje")
/// una comprobacion del caso (false = fallo con ese mensaje)
/// Devuelve: bool
template <typename T0 = Value, typename T1 = Value>
inline Value check(const T0& ok = {}, const T1& texto = {}) {
    return detail::api("Test.check", Values{Value(ok), Value(texto)});
}
/// Test.done()
/// ya terminaron todos los casos
template <typename... Mas>
inline Value done(Mas&&... mas) {
    return detail::api("Test.done", Values{Value(std::forward<Mas>(mas))...});
}
/// Test.finish()
/// termina el caso (true si fue bien)
/// Devuelve: bool
template <typename... Mas>
inline Value finish(Mas&&... mas) {
    return detail::api("Test.finish", Values{Value(std::forward<Mas>(mas))...});
}
/// Test.log("texto")
/// una linea en la salida del caso
template <typename T0 = Value>
inline Value log(const T0& texto = {}) {
    return detail::api("Test.log", Values{Value(texto)});
}
/// Test.results()
/// {done, passed, failed, cases = {name, passed, assertions, failures, messages, seconds}}
/// Devuelve: objeto
template <typename... Mas>
inline Value results(Mas&&... mas) {
    return detail::api("Test.results", Values{Value(std::forward<Mas>(mas))...});
}
}  // namespace Test

namespace Text {
/// Text.format("Hola {0}", nombre)
/// sustituye {0}, {1}, {nombre}
/// Devuelve: texto
template <typename T0 = Value, typename T1 = Value>
inline Value format(const T0& texto = {}, const T1& nombre = {}) {
    return detail::api("Text.format", Values{Value(texto), Value(nombre)});
}
/// Text.get("menu.jugar", ...)
/// texto en el idioma actual ({0}, {1}... con los argumentos)
/// Devuelve: texto
template <typename T0 = Value, typename... Mas>
inline Value get(const T0& texto = {}, Mas&&... mas) {
    return detail::api("Text.get", Values{Value(texto), Value(std::forward<Mas>(mas))...});
}
/// Text.has("clave")
/// existe la clave?
/// Devuelve: booleano
template <typename T0 = Value>
inline Value has(const T0& texto = {}) {
    return detail::api("Text.has", Values{Value(texto)});
}
/// Text.language()
/// idioma actual ("es")
/// Devuelve: texto
template <typename... Mas>
inline Value language(Mas&&... mas) {
    return detail::api("Text.language", Values{Value(std::forward<Mas>(mas))...});
}
/// Text.languages()
/// {code, name} de cada idioma
/// Devuelve: lista de objetos
template <typename... Mas>
inline Value languages(Mas&&... mas) {
    return detail::api("Text.languages", Values{Value(std::forward<Mas>(mas))...});
}
/// Text.onLanguageChanged(function(codigo) end)
/// aviso al cambiar de idioma (devuelve un id)
/// Devuelve: numero
template <typename T0 = Value>
inline Value onLanguageChanged(const T0& callback = {}) {
    return detail::api("Text.onLanguageChanged", Values{Value(callback)});
}
/// Text.plural("monedas", n, ...)
/// forma plural (clave#one / clave#other) con {n}
/// Devuelve: texto
template <typename T0 = Value, typename T1 = Value, typename... Mas>
inline Value plural(const T0& texto = {}, const T1& n = {}, Mas&&... mas) {
    return detail::api("Text.plural", Values{Value(texto), Value(n), Value(std::forward<Mas>(mas))...});
}
/// Text.removeListener(id)
/// quita un aviso
template <typename T0 = Value>
inline Value removeListener(const T0& id = {}) {
    return detail::api("Text.removeListener", Values{Value(id)});
}
/// Text.setLanguage("en")
/// cambia el idioma (la UI se actualiza sola)
/// Devuelve: booleano
template <typename T0 = Value>
inline Value setLanguage(const T0& texto = {}) {
    return detail::api("Text.setLanguage", Values{Value(texto)});
}
/// Text.strip("<b>Hola</b>")
/// el texto sin las etiquetas del texto enriquecido
/// Devuelve: texto
template <typename T0 = Value>
inline Value strip(const T0& texto = {}) {
    return detail::api("Text.strip", Values{Value(texto)});
}
/// Text.systemLanguage()
/// idioma del sistema
/// Devuelve: texto
template <typename... Mas>
inline Value systemLanguage(Mas&&... mas) {
    return detail::api("Text.systemLanguage", Values{Value(std::forward<Mas>(mas))...});
}
}  // namespace Text

namespace Voice {
/// Voice.isMuted(id)
/// Devuelve: bool
template <typename T0 = Value>
inline Value isMuted(const T0& id = {}) {
    return detail::api("Voice.isMuted", Values{Value(id)});
}
/// Voice.isSpeaking(id)
/// habla ahora? (sin id = yo)
/// Devuelve: bool
template <typename T0 = Value>
inline Value isSpeaking(const T0& id = {}) {
    return detail::api("Voice.isSpeaking", Values{Value(id)});
}
/// Voice.micLevel()
/// 0..1 del microfono
/// Devuelve: numero
template <typename... Mas>
inline Value micLevel(Mas&&... mas) {
    return detail::api("Voice.micLevel", Values{Value(std::forward<Mas>(mas))...});
}
/// Voice.setMicGain(1)
/// ganancia del microfono
template <typename T0 = Value>
inline Value setMicGain(const T0& valor = {}) {
    return detail::api("Voice.setMicGain", Values{Value(valor)});
}
/// Voice.setMode("push")
/// push (pulsar para hablar), open (por voz) u off
template <typename T0 = Value>
inline Value setMode(const T0& texto = {}) {
    return detail::api("Voice.setMode", Values{Value(texto)});
}
/// Voice.setMuted(id, true)
/// silenciar a un jugador
template <typename T0 = Value, typename T1 = Value>
inline Value setMuted(const T0& id = {}, const T1& activar = {}) {
    return detail::api("Voice.setMuted", Values{Value(id), Value(activar)});
}
/// Voice.setProximity(30)
/// volumen por distancia (0 = todos igual)
template <typename T0 = Value>
inline Value setProximity(const T0& valor = {}) {
    return detail::api("Voice.setProximity", Values{Value(valor)});
}
/// Voice.setTalking(true)
/// modo push: hablando
template <typename T0 = Value>
inline Value setTalking(const T0& activar = {}) {
    return detail::api("Voice.setTalking", Values{Value(activar)});
}
/// Voice.setThreshold(0.02)
/// modo open: volumen minimo
template <typename T0 = Value>
inline Value setThreshold(const T0& valor = {}) {
    return detail::api("Voice.setThreshold", Values{Value(valor)});
}
/// Voice.setVolume(1)
/// volumen de los demas
template <typename T0 = Value>
inline Value setVolume(const T0& valor = {}) {
    return detail::api("Voice.setVolume", Values{Value(valor)});
}
/// Voice.start()
/// abre el microfono y la salida
/// Devuelve: bool
template <typename... Mas>
inline Value start(Mas&&... mas) {
    return detail::api("Voice.start", Values{Value(std::forward<Mas>(mas))...});
}
/// Voice.stop()
/// los cierra
template <typename... Mas>
inline Value stop(Mas&&... mas) {
    return detail::api("Voice.stop", Values{Value(std::forward<Mas>(mas))...});
}
}  // namespace Voice

namespace Voxel {
/// Voxel.blockColor("stone")
/// color medio del bloque (Vec3)
/// Devuelve: Vec3
template <typename T0 = Value>
inline Value blockColor(const T0& texto = {}) {
    return detail::api("Voxel.blockColor", Values{Value(texto)});
}
/// Voxel.blockCount()
/// cuantos tipos de bloque hay
/// Devuelve: numero
template <typename... Mas>
inline Value blockCount(Mas&&... mas) {
    return detail::api("Voxel.blockCount", Values{Value(std::forward<Mas>(mas))...});
}
/// Voxel.blockId("stone")
/// numero de un bloque
/// Devuelve: numero
template <typename T0 = Value>
inline Value blockId(const T0& texto = {}) {
    return detail::api("Voxel.blockId", Values{Value(texto)});
}
/// Voxel.blockInfo("stone")
/// {name, label, solid, hardness...}
/// Devuelve: objeto
template <typename T0 = Value>
inline Value blockInfo(const T0& texto = {}) {
    return detail::api("Voxel.blockInfo", Values{Value(texto)});
}
/// Voxel.blockLight(x, y, z)
/// luz de antorchas 0..15
/// Devuelve: numero
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value blockLight(const T0& x = {}, const T1& y = {}, const T2& z = {}) {
    return detail::api("Voxel.blockLight", Values{Value(x), Value(y), Value(z)});
}
/// Voxel.boxCollides(centro, semiejes)
/// toca bloques?
/// Devuelve: bool
template <typename T0 = Value, typename T1 = Value>
inline Value boxCollides(const T0& centro = {}, const T1& semiejes = {}) {
    return detail::api("Voxel.boxCollides", Values{Value(centro), Value(semiejes)});
}
/// Voxel.deleteWorld("nombre")
/// lo borra
/// Devuelve: bool
template <typename T0 = Value>
inline Value deleteWorld(const T0& texto = {}) {
    return detail::api("Voxel.deleteWorld", Values{Value(texto)});
}
/// Voxel.getBlock(x, y, z)
/// numero del bloque (0 = aire)
/// Devuelve: numero
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value getBlock(const T0& x = {}, const T1& y = {}, const T2& z = {}) {
    return detail::api("Voxel.getBlock", Values{Value(x), Value(y), Value(z)});
}
/// Voxel.getBlockAt(Vec3)
/// bloque en ese punto
/// Devuelve: numero
template <typename T0 = Value>
inline Value getBlockAt(const T0& vec = {}) {
    return detail::api("Voxel.getBlockAt", Values{Value(vec)});
}
/// Voxel.getMeta("clave", "")
/// lee un dato
/// Devuelve: texto
template <typename T0 = Value, typename T1 = Value>
inline Value getMeta(const T0& texto = {}, const T1& texto2 = {}) {
    return detail::api("Voxel.getMeta", Values{Value(texto), Value(texto2)});
}
/// Voxel.inWater(Vec3)
/// esta en el agua?
/// Devuelve: bool
template <typename T0 = Value>
inline Value inWater(const T0& vec = {}) {
    return detail::api("Voxel.inWater", Values{Value(vec)});
}
/// Voxel.isActive()
/// hay mundo de bloques?
/// Devuelve: bool
template <typename... Mas>
inline Value isActive(Mas&&... mas) {
    return detail::api("Voxel.isActive", Values{Value(std::forward<Mas>(mas))...});
}
/// Voxel.isReady(Vec3)
/// ya esta generado?
/// Devuelve: bool
template <typename T0 = Value>
inline Value isReady(const T0& vec = {}) {
    return detail::api("Voxel.isReady", Values{Value(vec)});
}
/// Voxel.listWorlds()
/// lista de mundos
/// Devuelve: lista de {name, seed, lastPlayed, mode}
template <typename... Mas>
inline Value listWorlds(Mas&&... mas) {
    return detail::api("Voxel.listWorlds", Values{Value(std::forward<Mas>(mas))...});
}
/// Voxel.loadWorld("nombre")
/// carga un mundo guardado
/// Devuelve: bool
template <typename T0 = Value>
inline Value loadWorld(const T0& texto = {}) {
    return detail::api("Voxel.loadWorld", Values{Value(texto)});
}
/// Voxel.moveBox(centro, semiejes, delta)
/// posicion, enSuelo, techo, pared
/// Devuelve: lista {Vec3, bool, bool, bool}
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value moveBox(const T0& centro = {}, const T1& semiejes = {}, const T2& delta = {}) {
    return detail::api("Voxel.moveBox", Values{Value(centro), Value(semiejes), Value(delta)});
}
/// Voxel.newWorld("nombre", semilla)
/// mundo nuevo con nombre
/// Devuelve: bool
template <typename T0 = Value, typename T1 = Value>
inline Value newWorld(const T0& texto = {}, const T1& semilla = {}) {
    return detail::api("Voxel.newWorld", Values{Value(texto), Value(semilla)});
}
/// Voxel.raycast(origen, direccion, distancia)
/// nil o {block, normal, id, point, distance}
/// Devuelve: objeto o nil
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value raycast(const T0& origen = {}, const T1& direccion = {}, const T2& distancia = {}) {
    return detail::api("Voxel.raycast", Values{Value(origen), Value(direccion), Value(distancia)});
}
/// Voxel.saveWorld()
/// guarda
/// Devuelve: bool
template <typename... Mas>
inline Value saveWorld(Mas&&... mas) {
    return detail::api("Voxel.saveWorld", Values{Value(std::forward<Mas>(mas))...});
}
/// Voxel.seed()
/// semilla
/// Devuelve: numero
template <typename... Mas>
inline Value seed(Mas&&... mas) {
    return detail::api("Voxel.seed", Values{Value(std::forward<Mas>(mas))...});
}
/// Voxel.setBlock(x, y, z, "stone")
/// pone o quita un bloque
/// Devuelve: bool
template <typename T0 = Value, typename T1 = Value, typename T2 = Value, typename T3 = Value>
inline Value setBlock(const T0& x = {}, const T1& y = {}, const T2& z = {}, const T3& texto = {}) {
    return detail::api("Voxel.setBlock", Values{Value(x), Value(y), Value(z), Value(texto)});
}
/// Voxel.setMeta("clave", "texto")
/// dato guardado con el mundo
template <typename T0 = Value, typename T1 = Value>
inline Value setMeta(const T0& texto = {}, const T1& texto2 = {}) {
    return detail::api("Voxel.setMeta", Values{Value(texto), Value(texto2)});
}
/// Voxel.skyLight(x, y, z)
/// luz del cielo 0..15
/// Devuelve: numero
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value skyLight(const T0& x = {}, const T1& y = {}, const T2& z = {}) {
    return detail::api("Voxel.skyLight", Values{Value(x), Value(y), Value(z)});
}
/// Voxel.surfaceHeight(x, z)
/// altura del terreno
/// Devuelve: numero
template <typename T0 = Value, typename T1 = Value>
inline Value surfaceHeight(const T0& x = {}, const T1& z = {}) {
    return detail::api("Voxel.surfaceHeight", Values{Value(x), Value(z)});
}
/// Voxel.worldName()
/// nombre del mundo
/// Devuelve: texto
template <typename... Mas>
inline Value worldName(Mas&&... mas) {
    return detail::api("Voxel.worldName", Values{Value(std::forward<Mas>(mas))...});
}
}  // namespace Voxel

namespace Weather {
/// Weather.get()
/// clima actual ("Storm")
/// Devuelve: texto
template <typename... Mas>
inline Value get(Mas&&... mas) {
    return detail::api("Weather.get", Values{Value(std::forward<Mas>(mas))...});
}
/// Weather.getDate()
/// dia, mes
/// Devuelve: lista {dia, mes}
template <typename... Mas>
inline Value getDate(Mas&&... mas) {
    return detail::api("Weather.getDate", Values{Value(std::forward<Mas>(mas))...});
}
/// Weather.getFog()
/// densidad de la niebla
/// Devuelve: numero
template <typename... Mas>
inline Value getFog(Mas&&... mas) {
    return detail::api("Weather.getFog", Values{Value(std::forward<Mas>(mas))...});
}
/// Weather.getLabel()
/// nombre visible ("Tormenta")
/// Devuelve: texto
template <typename... Mas>
inline Value getLabel(Mas&&... mas) {
    return detail::api("Weather.getLabel", Values{Value(std::forward<Mas>(mas))...});
}
/// Weather.getLatitude()
/// latitud
/// Devuelve: numero
template <typename... Mas>
inline Value getLatitude(Mas&&... mas) {
    return detail::api("Weather.getLatitude", Values{Value(std::forward<Mas>(mas))...});
}
/// Weather.getRain()
/// lluvia 0..1
/// Devuelve: numero
template <typename... Mas>
inline Value getRain(Mas&&... mas) {
    return detail::api("Weather.getRain", Values{Value(std::forward<Mas>(mas))...});
}
/// Weather.getSeason()
/// estacion actual
/// Devuelve: texto
template <typename... Mas>
inline Value getSeason(Mas&&... mas) {
    return detail::api("Weather.getSeason", Values{Value(std::forward<Mas>(mas))...});
}
/// Weather.getSnow()
/// nevada 0..1
/// Devuelve: numero
template <typename... Mas>
inline Value getSnow(Mas&&... mas) {
    return detail::api("Weather.getSnow", Values{Value(std::forward<Mas>(mas))...});
}
/// Weather.getSnowCover()
/// nieve acumulada 0..1
/// Devuelve: numero
template <typename... Mas>
inline Value getSnowCover(Mas&&... mas) {
    return detail::api("Weather.getSnowCover", Values{Value(std::forward<Mas>(mas))...});
}
/// Weather.getSunDirection()
/// Vec3 hacia el sol
/// Devuelve: Vec3
template <typename... Mas>
inline Value getSunDirection(Mas&&... mas) {
    return detail::api("Weather.getSunDirection", Values{Value(std::forward<Mas>(mas))...});
}
/// Weather.getTarget()
/// clima al que va la transicion
/// Devuelve: texto
template <typename... Mas>
inline Value getTarget(Mas&&... mas) {
    return detail::api("Weather.getTarget", Values{Value(std::forward<Mas>(mas))...});
}
/// Weather.getTemperature()
/// grados C
/// Devuelve: numero
template <typename... Mas>
inline Value getTemperature(Mas&&... mas) {
    return detail::api("Weather.getTemperature", Values{Value(std::forward<Mas>(mas))...});
}
/// Weather.getTime()
/// hora del dia
/// Devuelve: numero
template <typename... Mas>
inline Value getTime(Mas&&... mas) {
    return detail::api("Weather.getTime", Values{Value(std::forward<Mas>(mas))...});
}
/// Weather.getTimeScale()
/// velocidad del tiempo
/// Devuelve: numero
template <typename... Mas>
inline Value getTimeScale(Mas&&... mas) {
    return detail::api("Weather.getTimeScale", Values{Value(std::forward<Mas>(mas))...});
}
/// Weather.getWeather()
/// clima actual ("Storm")
/// Devuelve: texto
template <typename... Mas>
inline Value getWeather(Mas&&... mas) {
    return detail::api("Weather.getWeather", Values{Value(std::forward<Mas>(mas))...});
}
/// Weather.getWetness()
/// humedad de las superficies 0..1
/// Devuelve: numero
template <typename... Mas>
inline Value getWetness(Mas&&... mas) {
    return detail::api("Weather.getWetness", Values{Value(std::forward<Mas>(mas))...});
}
/// Weather.getWind()
/// Vec3 del viento (m/s)
/// Devuelve: Vec3
template <typename... Mas>
inline Value getWind(Mas&&... mas) {
    return detail::api("Weather.getWind", Values{Value(std::forward<Mas>(mas))...});
}
/// Weather.getWindDirection()
/// grados
/// Devuelve: numero
template <typename... Mas>
inline Value getWindDirection(Mas&&... mas) {
    return detail::api("Weather.getWindDirection", Values{Value(std::forward<Mas>(mas))...});
}
/// Weather.getWindSpeed()
/// m/s con rachas
/// Devuelve: numero
template <typename... Mas>
inline Value getWindSpeed(Mas&&... mas) {
    return detail::api("Weather.getWindSpeed", Values{Value(std::forward<Mas>(mas))...});
}
/// Weather.isNight()
/// el sol esta bajo el horizonte?
/// Devuelve: bool
template <typename... Mas>
inline Value isNight(Mas&&... mas) {
    return detail::api("Weather.isNight", Values{Value(std::forward<Mas>(mas))...});
}
/// Weather.isTransitioning()
/// esta cambiando?
/// Devuelve: bool
template <typename... Mas>
inline Value isTransitioning(Mas&&... mas) {
    return detail::api("Weather.isTransitioning", Values{Value(std::forward<Mas>(mas))...});
}
/// Weather.lightning(800)
/// un rayo ya (distancia en m; sin ella al azar)
template <typename T0 = Value>
inline Value lightning(const T0& valor = {}) {
    return detail::api("Weather.lightning", Values{Value(valor)});
}
/// Weather.presets()
/// lista de climas
/// Devuelve: lista de texto
template <typename... Mas>
inline Value presets(Mas&&... mas) {
    return detail::api("Weather.presets", Values{Value(std::forward<Mas>(mas))...});
}
/// Weather.set("Storm", 10)
/// cambia de clima en N segundos (Clear, Cloudy, Overcast, Foggy, LightRain, Rain, Storm, LightSnow, Snow, Blizzard, Sandstorm)
/// Devuelve: bool
template <typename T0 = Value, typename T1 = Value>
inline Value set(const T0& texto = {}, const T1& valor = {}) {
    return detail::api("Weather.set", Values{Value(texto), Value(valor)});
}
/// Weather.setAudio(true, 0.8)
/// sonido de lluvia, viento y truenos
template <typename T0 = Value, typename T1 = Value>
inline Value setAudio(const T0& activar = {}, const T1& valor = {}) {
    return detail::api("Weather.setAudio", Values{Value(activar), Value(valor)});
}
/// Weather.setDate(21, 12)
/// dia y mes (mueve el sol y la estacion)
template <typename T0 = Value, typename T1 = Value>
inline Value setDate(const T0& valor = {}, const T1& valor2 = {}) {
    return detail::api("Weather.setDate", Values{Value(valor), Value(valor2)});
}
/// Weather.setDayLength(24)
/// minutos reales por dia (nil = el tiempo se para)
template <typename T0 = Value>
inline Value setDayLength(const T0& valor = {}) {
    return detail::api("Weather.setDayLength", Values{Value(valor)});
}
/// Weather.setLatitude(40)
/// latitud en grados
template <typename T0 = Value>
inline Value setLatitude(const T0& valor = {}) {
    return detail::api("Weather.setLatitude", Values{Value(valor)});
}
/// Weather.setLightning(true, 2)
/// rayos en las tormentas y su frecuencia
template <typename T0 = Value, typename T1 = Value>
inline Value setLightning(const T0& activar = {}, const T1& valor = {}) {
    return detail::api("Weather.setLightning", Values{Value(activar), Value(valor)});
}
/// Weather.setPrecipitationDensity(0.5)
/// menos gotas (rendimiento)
template <typename T0 = Value>
inline Value setPrecipitationDensity(const T0& valor = {}) {
    return detail::api("Weather.setPrecipitationDensity", Values{Value(valor)});
}
/// Weather.setRandom(true, 120, 360)
/// clima al azar (segundos min y max)
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value setRandom(const T0& activar = {}, const T1& valor = {}, const T2& valor2 = {}) {
    return detail::api("Weather.setRandom", Values{Value(activar), Value(valor), Value(valor2)});
}
/// Weather.setSeason("Winter")
/// estacion fija ("auto" = por la fecha)
template <typename T0 = Value>
inline Value setSeason(const T0& texto = {}) {
    return detail::api("Weather.setSeason", Values{Value(texto)});
}
/// Weather.setSnowCover(1)
/// nieve acumulada al instante
template <typename T0 = Value>
inline Value setSnowCover(const T0& valor = {}) {
    return detail::api("Weather.setSnowCover", Values{Value(valor)});
}
/// Weather.setTime(18.5)
/// hora del dia (0..24)
template <typename T0 = Value>
inline Value setTime(const T0& valor = {}) {
    return detail::api("Weather.setTime", Values{Value(valor)});
}
/// Weather.setTimeScale(60)
/// velocidad del tiempo (1 = real, 0 = parado)
template <typename T0 = Value>
inline Value setTimeScale(const T0& valor = {}) {
    return detail::api("Weather.setTimeScale", Values{Value(valor)});
}
/// Weather.setWeather("Rain", 5)
/// lo mismo que set
/// Devuelve: bool
template <typename T0 = Value, typename T1 = Value>
inline Value setWeather(const T0& texto = {}, const T1& valor = {}) {
    return detail::api("Weather.setWeather", Values{Value(texto), Value(valor)});
}
/// Weather.setWetness(1, 0.6)
/// humedad y charcos al instante
template <typename T0 = Value, typename T1 = Value>
inline Value setWetness(const T0& valor = {}, const T1& valor2 = {}) {
    return detail::api("Weather.setWetness", Values{Value(valor), Value(valor2)});
}
/// Weather.setWind(90, 1.5)
/// direccion (grados) y fuerza del viento
template <typename T0 = Value, typename T1 = Value>
inline Value setWind(const T0& valor = {}, const T1& valor2 = {}) {
    return detail::api("Weather.setWind", Values{Value(valor), Value(valor2)});
}
/// Weather.transition()
/// 0..1 lo que lleva la transicion
/// Devuelve: numero
template <typename... Mas>
inline Value transition(Mas&&... mas) {
    return detail::api("Weather.transition", Values{Value(std::forward<Mas>(mas))...});
}
}  // namespace Weather

namespace WorldPartition {
/// WorldPartition.active()
/// esta repartiendo el mundo en celdas?
/// Devuelve: bool
template <typename... Mas>
inline Value active(Mas&&... mas) {
    return detail::api("WorldPartition.active", Values{Value(std::forward<Mas>(mas))...});
}
/// WorldPartition.isLoaded(posicion)
/// la celda de ese punto esta cargada?
/// Devuelve: bool
template <typename T0 = Value>
inline Value isLoaded(const T0& posicion = {}) {
    return detail::api("WorldPartition.isLoaded", Values{Value(posicion)});
}
/// WorldPartition.loadAll()
/// carga todas las celdas ya
template <typename... Mas>
inline Value loadAll(Mas&&... mas) {
    return detail::api("WorldPartition.loadAll", Values{Value(std::forward<Mas>(mas))...});
}
/// WorldPartition.stats()
/// {cells, loadedCells, objects, unloadedObjects, storedBytes}
/// Devuelve: objeto
template <typename... Mas>
inline Value stats(Mas&&... mas) {
    return detail::api("WorldPartition.stats", Values{Value(std::forward<Mas>(mas))...});
}
}  // namespace WorldPartition

namespace XR {
/// XR.getAimRay("right")
/// origen, direccion del puntero (para Physics.raycast)
/// Devuelve: lista [Vec3, Vec3]
template <typename T0 = Value>
inline Value getAimRay(const T0& texto = {}) {
    return detail::api("XR.getAimRay", Values{Value(texto)});
}
/// XR.getButton("right", "a")
/// boton mantenido: trigger, grip, thumbstick, primary (a/x), secondary (b/y), menu
/// Devuelve: bool
template <typename T0 = Value, typename T1 = Value>
inline Value getButton(const T0& texto = {}, const T1& texto2 = {}) {
    return detail::api("XR.getButton", Values{Value(texto), Value(texto2)});
}
/// XR.getButtonDown("right", "trigger")
/// boton pulsado este frame
/// Devuelve: bool
template <typename T0 = Value, typename T1 = Value>
inline Value getButtonDown(const T0& texto = {}, const T1& texto2 = {}) {
    return detail::api("XR.getButtonDown", Values{Value(texto), Value(texto2)});
}
/// XR.getButtonUp("right", "trigger")
/// boton soltado este frame
/// Devuelve: bool
template <typename T0 = Value, typename T1 = Value>
inline Value getButtonUp(const T0& texto = {}, const T1& texto2 = {}) {
    return detail::api("XR.getButtonUp", Values{Value(texto), Value(texto2)});
}
/// XR.getControllerPosition("right", "grip")
/// Vec3 de la mano (grip) o del puntero (aim), o nil
/// Devuelve: Vec3 o nil
template <typename T0 = Value, typename T1 = Value>
inline Value getControllerPosition(const T0& texto = {}, const T1& texto2 = {}) {
    return detail::api("XR.getControllerPosition", Values{Value(texto), Value(texto2)});
}
/// XR.getControllerRotation("right", "grip")
/// Quat de la mano o del puntero, o nil
/// Devuelve: Quat o nil
template <typename T0 = Value, typename T1 = Value>
inline Value getControllerRotation(const T0& texto = {}, const T1& texto2 = {}) {
    return detail::api("XR.getControllerRotation", Values{Value(texto), Value(texto2)});
}
/// XR.getGrip("right")
/// agarre 0..1
/// Devuelve: numero
template <typename T0 = Value>
inline Value getGrip(const T0& texto = {}) {
    return detail::api("XR.getGrip", Values{Value(texto)});
}
/// XR.getHeadLocalPosition()
/// Vec3 de la cabeza dentro de la habitacion
/// Devuelve: Vec3 o nil
template <typename... Mas>
inline Value getHeadLocalPosition(Mas&&... mas) {
    return detail::api("XR.getHeadLocalPosition", Values{Value(std::forward<Mas>(mas))...});
}
/// XR.getHeadPosition()
/// Vec3 de la cabeza en el mundo (o nil)
/// Devuelve: Vec3 o nil
template <typename... Mas>
inline Value getHeadPosition(Mas&&... mas) {
    return detail::api("XR.getHeadPosition", Values{Value(std::forward<Mas>(mas))...});
}
/// XR.getHeadRotation()
/// Quat de la cabeza en el mundo (o nil)
/// Devuelve: Quat o nil
template <typename... Mas>
inline Value getHeadRotation(Mas&&... mas) {
    return detail::api("XR.getHeadRotation", Values{Value(std::forward<Mas>(mas))...});
}
/// XR.getOriginPosition()
/// Vec3 del rig (XR Origin o la camara) en el mundo
/// Devuelve: Vec3
template <typename... Mas>
inline Value getOriginPosition(Mas&&... mas) {
    return detail::api("XR.getOriginPosition", Values{Value(std::forward<Mas>(mas))...});
}
/// XR.getRuntimeName()
/// runtime de OpenXR (SteamVR, Oculus...)
/// Devuelve: texto
template <typename... Mas>
inline Value getRuntimeName(Mas&&... mas) {
    return detail::api("XR.getRuntimeName", Values{Value(std::forward<Mas>(mas))...});
}
/// XR.getSystemName()
/// nombre del casco
/// Devuelve: texto
template <typename... Mas>
inline Value getSystemName(Mas&&... mas) {
    return detail::api("XR.getSystemName", Values{Value(std::forward<Mas>(mas))...});
}
/// XR.getThumbstick("left")
/// Vec3(x, y, 0) del stick, -1..1
/// Devuelve: Vec3
template <typename T0 = Value>
inline Value getThumbstick(const T0& texto = {}) {
    return detail::api("XR.getThumbstick", Values{Value(texto)});
}
/// XR.getTrackingOrigin()
/// "floor" o "eyes"
/// Devuelve: texto
template <typename... Mas>
inline Value getTrackingOrigin(Mas&&... mas) {
    return detail::api("XR.getTrackingOrigin", Values{Value(std::forward<Mas>(mas))...});
}
/// XR.getTrigger("right")
/// gatillo 0..1
/// Devuelve: numero
template <typename T0 = Value>
inline Value getTrigger(const T0& texto = {}) {
    return detail::api("XR.getTrigger", Values{Value(texto)});
}
/// XR.isAvailable()
/// hay casco y sesion de VR
/// Devuelve: bool
template <typename... Mas>
inline Value isAvailable(Mas&&... mas) {
    return detail::api("XR.isAvailable", Values{Value(std::forward<Mas>(mas))...});
}
/// XR.isControllerActive("right")
/// el mando esta encendido y se sigue
/// Devuelve: bool
template <typename T0 = Value>
inline Value isControllerActive(const T0& texto = {}) {
    return detail::api("XR.isControllerActive", Values{Value(texto)});
}
/// XR.isFocused()
/// el juego tiene los mandos (sin el menu del sistema encima)
/// Devuelve: bool
template <typename... Mas>
inline Value isFocused(Mas&&... mas) {
    return detail::api("XR.isFocused", Values{Value(std::forward<Mas>(mas))...});
}
/// XR.isRunning()
/// el casco esta mostrando el juego
/// Devuelve: bool
template <typename... Mas>
inline Value isRunning(Mas&&... mas) {
    return detail::api("XR.isRunning", Values{Value(std::forward<Mas>(mas))...});
}
/// XR.setTrackingOrigin("floor")
/// floor (de pie) o eyes (sentado); con XR Origin manda el componente
/// Devuelve: bool
template <typename T0 = Value>
inline Value setTrackingOrigin(const T0& texto = {}) {
    return detail::api("XR.setTrackingOrigin", Values{Value(texto)});
}
/// XR.vibrate("right", 0.5, 0.1)
/// vibracion: intensidad 0..1, segundos, hz
template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
inline Value vibrate(const T0& texto = {}, const T1& valor = {}, const T2& valor2 = {}) {
    return detail::api("XR.vibrate", Values{Value(texto), Value(valor), Value(valor2)});
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
    /// Mesh.boundsMax: Vec3 esquina maxima (Vec3)
    Value boundsMax() const { return handle.get("boundsMax"); }
    /// Mesh.boundsMin: Vec3 esquina minima (Vec3)
    Value boundsMin() const { return handle.get("boundsMin"); }
    /// Mesh.capsule(radio, alto, segmentos)
    /// capsula
    /// Devuelve: Mesh
    template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
    static Mesh capsule(const T0& radio = {}, const T1& alto = {}, const T2& segmentos = {}) {
        return Mesh(detail::api("Mesh.capsule", Values{Value(radio), Value(alto), Value(segmentos)}));
    }
    /// Mesh:clear()
    /// la vacia
    template <typename... Mas>
    Value clear(Mas&&... mas) const {
        return handle.call("clear", Values{Value(std::forward<Mas>(mas))...});
    }
    /// Mesh:clone()
    /// copia
    /// Devuelve: Mesh
    template <typename... Mas>
    Value clone(Mas&&... mas) const {
        return handle.call("clone", Values{Value(std::forward<Mas>(mas))...});
    }
    /// Mesh.cube(tamano)
    /// cubo (numero o Vec3)
    /// Devuelve: Mesh
    template <typename T0 = Value>
    static Mesh cube(const T0& tamano = {}) {
        return Mesh(detail::api("Mesh.cube", Values{Value(tamano)}));
    }
    /// Mesh.cylinder(radio, alto, segmentos)
    /// cilindro
    /// Devuelve: Mesh
    template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
    static Mesh cylinder(const T0& radio = {}, const T1& alto = {}, const T2& segmentos = {}) {
        return Mesh(detail::api("Mesh.cylinder", Values{Value(radio), Value(alto), Value(segmentos)}));
    }
    /// Mesh:getMaterial(submalla)
    /// su material
    /// Devuelve: objeto
    template <typename T0 = Value>
    Value getMaterial(const T0& submalla = {}) const {
        return handle.call("getMaterial", Values{Value(submalla)});
    }
    /// Mesh:getTriangles(submalla)
    /// sus indices
    /// Devuelve: lista de numeros
    template <typename T0 = Value>
    Value getTriangles(const T0& submalla = {}) const {
        return handle.call("getTriangles", Values{Value(submalla)});
    }
    /// Mesh:getVertex(i)
    /// Vec3 de un vertice
    /// Devuelve: Vec3
    template <typename T0 = Value>
    Value getVertex(const T0& i = {}) const {
        return handle.call("getVertex", Values{Value(i)});
    }
    /// Mesh.name: nombre (texto)
    Value name() const { return handle.get("name"); }
    /// Cambia Mesh.name
    void setName(const Value& v) const { handle.setField("name", v); }
    /// Mesh.new("nombre")
    /// malla vacia
    /// Devuelve: Mesh
    template <typename T0 = Value>
    static Mesh create(const T0& texto = {}) {
        return Mesh(detail::api("Mesh.new", Values{Value(texto)}));
    }
    /// Mesh.normals: lista de Vec3 (lista de Vec3)
    Value normals() const { return handle.get("normals"); }
    /// Cambia Mesh.normals
    void setNormals(const Value& v) const { handle.setField("normals", v); }
    /// Mesh.plane(ancho, fondo, segX, segZ)
    /// plano subdividido
    /// Devuelve: Mesh
    template <typename T0 = Value, typename T1 = Value, typename T2 = Value, typename T3 = Value>
    static Mesh plane(const T0& ancho = {}, const T1& fondo = {}, const T2& segX = {}, const T3& segZ = {}) {
        return Mesh(detail::api("Mesh.plane", Values{Value(ancho), Value(fondo), Value(segX), Value(segZ)}));
    }
    /// Mesh.quad(ancho, alto)
    /// cuadrado en XY
    /// Devuelve: Mesh
    template <typename T0 = Value, typename T1 = Value>
    static Mesh quad(const T0& ancho = {}, const T1& alto = {}) {
        return Mesh(detail::api("Mesh.quad", Values{Value(ancho), Value(alto)}));
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
    /// Devuelve: Mesh
    template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
    static Mesh sphere(const T0& radio = {}, const T1& segmentos = {}, const T2& anillos = {}) {
        return Mesh(detail::api("Mesh.sphere", Values{Value(radio), Value(segmentos), Value(anillos)}));
    }
    /// Mesh.subMeshCount: submallas (materiales) (numero)
    Value subMeshCount() const { return handle.get("subMeshCount"); }
    /// Cambia Mesh.subMeshCount
    void setSubMeshCount(const Value& v) const { handle.setField("subMeshCount", v); }
    /// Mesh.tangents: lista de Vec3 (lista de Vec3)
    Value tangents() const { return handle.get("tangents"); }
    /// Cambia Mesh.tangents
    void setTangents(const Value& v) const { handle.setField("tangents", v); }
    /// Mesh.triangleCount: triangulos (numero)
    Value triangleCount() const { return handle.get("triangleCount"); }
    /// Mesh.triangles: indices (submalla 0) (lista de numeros)
    Value triangles() const { return handle.get("triangles"); }
    /// Mesh.uv: lista de {x, y} (lista de Vec3)
    Value uv() const { return handle.get("uv"); }
    /// Cambia Mesh.uv
    void setUv(const Value& v) const { handle.setField("uv", v); }
    /// Mesh:validate()
    /// "" si se puede dibujar; si no, el motivo
    /// Devuelve: texto
    template <typename... Mas>
    Value validate(Mas&&... mas) const {
        return handle.call("validate", Values{Value(std::forward<Mas>(mas))...});
    }
    /// Mesh.vertexCount: vertices (numero)
    Value vertexCount() const { return handle.get("vertexCount"); }
    /// Mesh.vertices: lista de Vec3 (lista de Vec3)
    Value vertices() const { return handle.get("vertices"); }
    /// Cambia Mesh.vertices
    void setVertices(const Value& v) const { handle.setField("vertices", v); }
    /// Mesh.wireCube(tamano, grosor)
    /// aristas de una caja (contornos)
    /// Devuelve: Mesh
    template <typename T0 = Value, typename T1 = Value>
    static Mesh wireCube(const T0& tamano = {}, const T1& grosor = {}) {
        return Mesh(detail::api("Mesh.wireCube", Values{Value(tamano), Value(grosor)}));
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
    /// StateMachine.broadcast("trigger")
    /// Activa un trigger en todas las maquinas
    /// Devuelve: numero
    template <typename T0 = Value>
    static Value broadcast(const T0& texto = {}) {
        return detail::api("StateMachine.broadcast", Values{Value(texto)});
    }
    /// StateMachine:changeState("Estado")
    /// Como go
    /// Devuelve: bool
    template <typename T0 = Value>
    Value changeState(const T0& texto = {}) const {
        return handle.call("changeState", Values{Value(texto)});
    }
    /// StateMachine.changes: Cambios de estado desde que empezo (numero)
    Value changes() const { return handle.get("changes"); }
    /// StateMachine.entity: El objeto de la maquina (Entity)
    Value entity() const { return handle.get("entity"); }
    /// StateMachine:get("variable")
    /// Valor de una variable (o nil)
    /// Devuelve: valor
    template <typename T0 = Value>
    Value get(const T0& texto = {}) const {
        return handle.call("get", Values{Value(texto)});
    }
    /// StateMachine:go("Estado")
    /// Cambia a ese estado (antes que las transiciones)
    /// Devuelve: bool
    template <typename T0 = Value>
    Value go(const T0& texto = {}) const {
        return handle.call("go", Values{Value(texto)});
    }
    /// StateMachine:has("variable")
    /// Existe esa variable?
    /// Devuelve: bool
    template <typename T0 = Value>
    Value has(const T0& texto = {}) const {
        return handle.call("has", Values{Value(texto)});
    }
    /// StateMachine:isIn("Estado")
    /// Esta en ese estado?
    /// Devuelve: bool
    template <typename T0 = Value>
    Value isIn(const T0& texto = {}) const {
        return handle.call("isIn", Values{Value(texto)});
    }
    /// StateMachine.of(entity)
    /// La maquina de estados de un objeto (o nil)
    /// Devuelve: StateMachine
    template <typename T0 = Value>
    static StateMachine of(const T0& entity = {}) {
        return StateMachine(detail::api("StateMachine.of", Values{Value(entity)}));
    }
    /// StateMachine.previous: Nombre del estado anterior (o nil) (texto)
    Value previous() const { return handle.get("previous"); }
    /// StateMachine:restart()
    /// Vuelve a la entrada con las variables iniciales
    template <typename... Mas>
    Value restart(Mas&&... mas) const {
        return handle.call("restart", Values{Value(std::forward<Mas>(mas))...});
    }
    /// StateMachine.running: Esta en marcha (no parada con stop) (bool)
    Value running() const { return handle.get("running"); }
    /// StateMachine:set("variable", valor)
    /// Cambia una variable (o la crea con el tipo del valor)
    template <typename T0 = Value, typename T1 = Value>
    Value set(const T0& texto = {}, const T1& valor = {}) const {
        return handle.call("set", Values{Value(texto), Value(valor)});
    }
    /// StateMachine:start()
    /// La pone en marcha (o sigue)
    template <typename... Mas>
    Value start(Mas&&... mas) const {
        return handle.call("start", Values{Value(std::forward<Mas>(mas))...});
    }
    /// StateMachine.state: Nombre del estado actual (nil si no ha empezado) (texto)
    Value state() const { return handle.get("state"); }
    /// StateMachine.stateTime: Segundos en el estado actual (numero)
    Value stateTime() const { return handle.get("stateTime"); }
    /// StateMachine:states()
    /// Nombres de los estados
    /// Devuelve: lista de texto
    template <typename... Mas>
    Value states(Mas&&... mas) const {
        return handle.call("states", Values{Value(std::forward<Mas>(mas))...});
    }
    /// StateMachine:stop()
    /// La para (se queda en su estado)
    template <typename... Mas>
    Value stop(Mas&&... mas) const {
        return handle.call("stop", Values{Value(std::forward<Mas>(mas))...});
    }
    /// StateMachine.time: Segundos desde que empezo la maquina (numero)
    Value time() const { return handle.get("time"); }
    /// StateMachine:trigger("nombre")
    /// Activa un trigger (vale hasta que se miran las transiciones)
    template <typename T0 = Value>
    Value trigger(const T0& texto = {}) const {
        return handle.call("trigger", Values{Value(texto)});
    }
    /// StateMachine.vars: Las variables (copia: {nombre = valor}); para cambiarlas, set (objeto)
    Value vars() const { return handle.get("vars"); }
};

/// Behavior Tree de un objeto (entity().getBehaviorTree()): get/set de la pizarra, start/stop, finishTask...
class BehaviorTree {
public:
    Value handle;
    BehaviorTree() = default;
    BehaviorTree(Value v) : handle(std::move(v)) {}
    operator Value() const { return handle; }
    explicit operator bool() const { return handle.truthy(); }
    /// BehaviorTree.activeTask: Nombre de la tarea que corre (o nil) (texto)
    Value activeTask() const { return handle.get("activeTask"); }
    /// BehaviorTree.broadcast("clave", valor)
    /// cambia una clave en todos los arboles
    /// Devuelve: numero
    template <typename T0 = Value, typename T1 = Value>
    static Value broadcast(const T0& texto = {}, const T1& valor = {}) {
        return detail::api("BehaviorTree.broadcast", Values{Value(texto), Value(valor)});
    }
    /// BehaviorTree:clear("clave")
    /// Vacia una clave (false, 0, "", sin objeto)
    template <typename T0 = Value>
    Value clear(const T0& texto = {}) const {
        return handle.call("clear", Values{Value(texto)});
    }
    /// BehaviorTree.cycles: Veces que la raiz termino (numero)
    Value cycles() const { return handle.get("cycles"); }
    /// BehaviorTree.entity: El objeto del arbol (Entity)
    Value entity() const { return handle.get("entity"); }
    /// BehaviorTree.finishTask(entity, true)
    /// Termina la tarea Run Script del objeto (true = bien, false = fallo)
    template <typename T0 = Value, typename T1 = Value>
    static BehaviorTree finishTask(const T0& entity = {}, const T1& activar = {}) {
        return BehaviorTree(detail::api("BehaviorTree.finishTask", Values{Value(entity), Value(activar)}));
    }
    /// BehaviorTree:get("clave")
    /// Valor de una clave de la pizarra (o nil)
    /// Devuelve: valor
    template <typename T0 = Value>
    Value get(const T0& texto = {}) const {
        return handle.call("get", Values{Value(texto)});
    }
    /// BehaviorTree:has("clave")
    /// Existe esa clave?
    /// Devuelve: bool
    template <typename T0 = Value>
    Value has(const T0& texto = {}) const {
        return handle.call("has", Values{Value(texto)});
    }
    /// BehaviorTree:isActive("Nodo")
    /// Ese nodo esta en la rama que corre?
    /// Devuelve: bool
    template <typename T0 = Value>
    Value isActive(const T0& texto = {}) const {
        return handle.call("isActive", Values{Value(texto)});
    }
    /// BehaviorTree.of(entity)
    /// el arbol de un objeto (o nil)
    /// Devuelve: BehaviorTree
    template <typename T0 = Value>
    static BehaviorTree of(const T0& entity = {}) {
        return BehaviorTree(detail::api("BehaviorTree.of", Values{Value(entity)}));
    }
    /// BehaviorTree.registerTask("Atacar", funcion(bt, primera) o {start, update, abort})
    /// tarea Run Script: se llama cada tick mientras corre; termina con bt.finishTask(true/false)
    /// Devuelve: bool
    template <typename T0 = Value, typename T1 = Value>
    static Value registerTask(const T0& texto = {}, const T1& arg = {}) {
        return detail::api("BehaviorTree.registerTask", Values{Value(texto), Value(arg)});
    }
    /// BehaviorTree.reportNoise(posicion, radio, quien)
    /// ruido que oye el servicio Hearing
    template <typename T0 = Value, typename T1 = Value, typename T2 = Value>
    static BehaviorTree reportNoise(const T0& posicion = {}, const T1& radio = {}, const T2& quien = {}) {
        return BehaviorTree(detail::api("BehaviorTree.reportNoise", Values{Value(posicion), Value(radio), Value(quien)}));
    }
    /// BehaviorTree:restart()
    /// Corta todo y empieza con la pizarra inicial
    template <typename... Mas>
    Value restart(Mas&&... mas) const {
        return handle.call("restart", Values{Value(std::forward<Mas>(mas))...});
    }
    /// BehaviorTree.running: Esta en marcha (bool)
    Value running() const { return handle.get("running"); }
    /// BehaviorTree:set("clave", valor)
    /// Cambia una clave (o la crea con el tipo del valor)
    template <typename T0 = Value, typename T1 = Value>
    Value set(const T0& texto = {}, const T1& valor = {}) const {
        return handle.call("set", Values{Value(texto), Value(valor)});
    }
    /// BehaviorTree:start()
    /// Lo pone en marcha
    template <typename... Mas>
    Value start(Mas&&... mas) const {
        return handle.call("start", Values{Value(std::forward<Mas>(mas))...});
    }
    /// BehaviorTree:stop()
    /// Corta lo que corre y lo para
    template <typename... Mas>
    Value stop(Mas&&... mas) const {
        return handle.call("stop", Values{Value(std::forward<Mas>(mas))...});
    }
    /// BehaviorTree.time: Segundos desde que empezo (numero)
    Value time() const { return handle.get("time"); }
    /// BehaviorTree.vars: La pizarra (copia: {clave = valor}); para cambiarla, set (objeto)
    Value vars() const { return handle.get("vars"); }
};

}  // namespace cramion

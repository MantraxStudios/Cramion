# Notas del bloque LUZ (rama rm-luz)

Base: horizonz-1031 (102bd30). Dos piezas del ROADMAP: **sondas de reflexion colocadas con caja** y **GTAO con normales dobladas y oclusion especular**. Todo compila (CramionFX, CramionCore, CramionEditor, CramionPlayer) y pasan `CramionReflectionProbeTests` (nueva, 68 comprobaciones) y `CramionShaderTests`. Sin probar en GPU (ver abajo).

## Entrada para el CHANGELOG (estilo de CHANGELOG.md)

### Sondas de reflexion con caja (como las Reflection Probe de Unity)
- **Componente `ReflectionProbeBox`** (*Reflection Probe*, categoria Iluminacion; `CramionCore/lighting/ReflectionProbes.h`): caja de influencia centrada en el objeto (sin giro), `blend_distance` (metros hacia dentro en que entra fundiendose), `priority`, `intensity` y `box_projection` (paralaje de caja). Boton *+ Reflection Probe* en la ventana Iluminacion.
- **Horneado**: en el Inspector (*Hornear esta sonda* / *Hornear todas*) o por MCP (`bake_reflection_probes`, con `entity` opcional). Cada sonda se captura como la sonda de la camara (las seis caras con la escena completa: sombras, SSAO/GTAO, SSR, GI de pantalla, nubes, terreno y vegetacion), se prefiltra por rugosidad (`probe_prefilter.comp`, los mismos 6 niveles que el IBL) y se lee a la CPU. Se guarda en **`<escena>.crrefl`** junto a la escena (cubos RGBA16F con sus mips, con FNV por cubo) y lo cargan el editor y el juego al abrir la escena. Hornear una sola sonda conserva el horneado de las demas y quita el de las que ya no existen.
- **Sombreado** (`lighting.frag`): las 8 sondas horneadas mas cercanas a la camara van al final de `GpuLights` (ordenadas por prioridad y, a igual prioridad, por la caja mas pequena); cada pixel mezcla **2 sondas**: manda la primera de ese orden que le llega, con el peso de su fundido, y de las demas la que mas pesa rellena lo que deja la primera (una habitacion dentro de un edificio entra fundiendose en su borde) y lo que queda sin sonda colocada se queda con la **sonda de la camara** o el cielo, como antes. Con la proyeccion de caja el reflejo se lee hacia el punto de la pared de la caja que ve el rayo (interiores creibles). Donde las colocadas cubren del todo, la sonda de la camara ni se calcula.
- Array de cubos en el binding 31 de la iluminacion (hasta 64 cubos en memoria, ~1 MB cada uno). Sin `.crrefl` no cambia nada: las escenas de antes se ven igual y no se espera a la GPU al abrirlas.
- CVar **`render.probe.Placed`** (1): 0 = solo la sonda de la camara (como antes).

### GTAO (oclusion ambiental por horizontes)
- **`gtao.frag`** (XeGTAO de Intel, MIT; `THIRD_PARTY_NOTICES.md`): 2 cortes x 4 pasos a cada lado con el patron de 4x4 del SSAO (el mismo desenfoque de `lighting.frag` quita el ruido) y, con TAA/FSR 2+/DLSS, el patron cambia cada frame y la historia lo acumula. Escribe la visibilidad, la profundidad y la **normal doblada** (octaedro, mundo).
- En `lighting.frag`: la irradiancia del cielo se mira con la normal doblada y el respaldo del reflejo (sonda/cielo) lleva ademas **oclusion especular** de cono contra cono (GTSO, Jimenez 2016) con la normal doblada. Lo que encontro el SSR no se ocluye (como antes).
- CVar **`render.ao.Gtao`** (0 por defecto): GTAO en lugar del SSAO desde el nivel Medio; en Bajo y en el modo compatible (movil) sigue el SSAO de siempre. Con GI por rayos no se calcula ninguno de los dos (como antes). Misma imagen, mismos sets y mismo pase que el SSAO (sin pases ni palancas nuevos); push constant de 16 bytes.
- La imagen del SSAO pasa a RGBA16F desde el nivel Medio en PC (antes RG16F) para guardar la normal doblada; en gama baja y en el modo compatible sigue en RG16F (alli no hay GTAO).

Pruebas: `CramionReflectionProbeTests` (peso y fundido de la caja, proyeccion de caja, eleccion de las sondas del frame, mezcla de las 2 que mas pesan, empaquetado std140 de `GpuLights`, archivo `.crrefl` de ida y vuelta, danado/cortado/version futura, y el componente guardado y vuelto a leer con escenas de antes).

## Archivos tocados

Nuevos:
- `CramionFX/include/CramionFX/vk/ReflectionProbeSet.h`, `CramionFX/src/vk/ReflectionProbeSet.cpp`: datos y cuentas sin Vulkan (peso, proyeccion, eleccion, mezcla, empaquetado, tamanos del cubo).
- `CramionFX/src/vk/VulkanRendererReflectionProbes.cpp`: `fillPlacedProbes`, `writePlacedProbeDescriptors`, `setBakedReflectionCubes`, `bakeReflectionCube`, `gtaoActive`.
- `CramionFX/shaders/gtao.frag`.
- `CramionCore/include/CramionCore/lighting/ReflectionProbes.h`, `CramionCore/src/lighting/ReflectionProbes.cpp`: componente, `.crrefl`, sync, carga y horneado.
- `CramionCore/tests/ReflectionProbeTests.cpp`.
- `CramionEditor/src/EditorReflectionProbes.cpp`: Inspector y MCP.

Modificados (bloques localizados):
- `CramionFX/include/CramionFX/vk/ReflectionProbe.h`, `src/vk/ReflectionProbe.cpp`: cubo de horneado + lectura, array de cubos de las colocadas (`uploadPlacedCubes`), `recordPrefilter` sobre `recordPrefilterInto`.
- `CramionFX/include/CramionFX/vk/GpuTypes.h`: `GpuLights` + `placed_info` y `placed[8]` **al final** (nada se mueve; sigue < 16 KB).
- `CramionFX/include/CramionFX/vk/LightingPass.h`, `src/vk/LightingPass.cpp`: binding 31 (no en el modo compatible).
- `CramionFX/include/CramionFX/vk/VulkanRenderer.h`: API nueva y miembros (un bloque publico y otro privado), `kGtaoFormat`, `ssaoFormat()`.
- `CramionFX/src/vk/VulkanRenderer.cpp` (compartido): creacion/destruccion de `gtao_pass_`, formato de la imagen del SSAO, +1 sampler en el pool de la iluminacion, `writePlacedProbeDescriptors()` al final de `updateLightingDescriptors`, `ssao_enabled` 1/2, `fillPlacedProbes` antes de escribir las luces, rama GTAO en `recordSsaoPass`.
- `CramionFX/shaders/lighting_body.glsl`: `PlacedProbeGpu` y su sitio al final del bloque de luces, binding 31, funciones de las sondas y del GTAO, AO/irradiancia/oclusion especular/respaldo del reflejo.
- `CramionCore/src/ecs/RenderSync.cpp` (compartido): CVars `render.probe.Placed` y `render.ao.Gtao`, `syncReflectionProbes`.
- `CramionCore/src/lighting/ProbeBaker.cpp`: `registerLightingComponents` registra tambien `ReflectionProbeBox`.
- `CramionCore/CMakeLists.txt` (compartido): ejecutable `CramionReflectionProbeTests`.
- `CramionEditor/src/EditorApp.h`, `EditorInspector.cpp`, `EditorLighting.cpp` (carga del `.crrefl` y boton *+ Reflection Probe*), `EditorMcp.cpp`, `EditorMcp21.cpp`.
- `CramionEditor/player/PlayerMain.cpp`: carga del `.crrefl` con la escena.
- `THIRD_PARTY_NOTICES.md`: XeGTAO (MIT).

No se tocan Components.h, CoreApi.cpp, Api.gen.h ni Script.h (sin API de scripts nueva: no hace falta regenerar el SDK).

## CVars
- `render.probe.Placed` (bool, 1, guardada): sondas colocadas.
- `render.ao.Gtao` (bool, 0, guardada): GTAO en lugar del SSAO (desde Medio; nunca en el movil).

## API nueva
- `gfx::VulkanRenderer`: `setPlacedReflectionProbes`, `placedReflectionProbes`, `setPlacedProbesEnabled`, `placedProbesEnabled`, `placedProbesSupported`, `setBakedReflectionCubes`, `bakedReflectionCubes`, `bakeReflectionCube`, `activePlacedProbes`, `setGtaoEnabled`, `gtaoEnabled`, `gtaoActive`.
- `gfx::ReflectionProbe`: `recordBakePrefilter`, `readBake`, `uploadPlacedCubes`, `placedSupported`, `placedView`, `placedLayers`.
- `CramionFX/vk/ReflectionProbeSet.h`: `PlacedReflectionProbe`, `BakedReflectionCube`, `GpuPlacedProbe`, `placedProbeWeight`, `boxProjectedDirection`, `selectPlacedProbes`, `blendPlacedProbes`, `packPlacedProbe`, `bakedCubeTexelCount`, `bakedCubeMipOffset`, `bakedCubeValid`.
- `CramionCore/lighting/ReflectionProbes.h`: `ReflectionProbeBox`, `reflectionProbesFile`, `saveReflectionProbes`, `loadReflectionProbes`, `syncReflectionProbes`, `loadSceneReflectionProbes`, `bakeReflectionProbes`.
- MCP: `bake_reflection_probes {entity?}` (nombre o UUID) -> `baked, failed, total, file, error, supported, enabled, active_last_frame, probes[]`.

## Pruebas y resultado
- `CramionReflectionProbeTests`: 68 comprobaciones, 0 fallos (65 antes de la revision).
- `CramionShaderTests`: 19 comprobaciones, 0 fallos.
- `CramionCoreTests` (registro y serializacion de componentes): 102 comprobaciones, 0 fallos. Los shaders tocados (`lighting.frag`, `lighting_mobile.frag`, `lighting_unlit.frag`, `gtao.frag`) compilan con glslc en Vulkan 1.3 y en el modo compatible (Vulkan 1.0, `CRAMION_COMPAT`).
- Compilan `CramionFX`, `CramionCore`, `CramionEditor` y `CramionPlayer` en `build-rm` (Release, clang).

## Lo que queda (pendiente)
- **Modo compatible (movil)**: sin sondas colocadas ni GTAO. No es directo: el modo compatible no tiene la sonda de la camara, el shader esta en el tope de 16 texturas y el array de cubos pide `imageCubeArray`. Habria que meter el cubo de la sonda mas cercana en el hueco de `reflection_probe_0` (una sola, sin mezcla).
- **Agua y vidrio**: siguen con la sonda de la camara. No es directo: usan sus propios sets (set 2 de `glass.frag`/`water.frag`) y habria que anadir el array y repetir la eleccion por pixel alli.
- **Por teselas/clusteres**: con 8 sondas por frame el bucle por pixel es corto (cajas en el UBO, ningun muestreo hasta elegir 2); si se suben a mas, repartirlas en las teselas de las luces puntuales (`light_tiles`).
- **Trazado de rayos**: con RT activo no se hornea (la captura de la sonda de la camara no existe con RT). Se puede hornear con RT apagado y las sondas se usan igual.
- **Gizmo** de la caja en la vista Escena (hoy solo los numeros del Inspector) y aviso automatico de "desactualizada" al mover la sonda (el Inspector ya lo dice).
- **Compresion** del `.crrefl` (zstd, como el `.crbake`): hoy ~1 MB por sonda sin comprimir.
- **API de scripts** para hornear en el juego (p. ej. al cambiar la hora del dia): no se ha anadido (no tocaba `CoreApi.cpp`/`Api.gen.h`).
- **GTAO**: sin cadena de mips de profundidad (XeGTAO la usa para lo lejano) ni historia temporal propia (se apoya en el desenfoque de 4x4 y en el TAA/upscaler). Un denoiser propio con historia daria menos ruido sin TAA.
- **Manual**: regenerar la referencia de componentes con `cramion_docgen docs-src/pages` al integrar (sale *Reflection Probe* en Iluminacion); no se ha hecho aqui para no chocar con los demas bloques.
- Exportar: el `.crrefl` va junto a la escena como el `.crbake`; comprobar en una exportacion real que entra en el paquete.

## Riesgos
- **`GpuLights` crece 528 bytes** (al final; 12,1 KB en total, por debajo de los 16 KB minimos). Lo leen tambien vidrio, agua, fluidos, volumetrica y `rt_common.glsl` con declaraciones mas cortas: no se mueve nada de lo de antes (comprobado en la prueba de empaquetado).
- **Imagen del SSAO en RGBA16F** en PC desde el nivel Medio (antes RG16F), tambien con el GTAO apagado: el doble de memoria y ancho de banda para esa imagen (a 1440p, ~15 MB mas). En gama baja y en el movil sigue en RG16F. `ssao.frag` sigue escribiendo `vec2` (los otros dos canales no se leen con el SSAO).
- **`samplerCubeArray`** en `lighting.frag` (no compat) pide `imageCubeArray`: todas las GPU de PC lo tienen; si alguna no, el pipeline de iluminacion no se crearia. El modo compatible no lo declara.
- El horneado reutiliza la imagen de captura de la sonda de la camara: si habia una captura a medias se empieza de nuevo (un parpadeo de confianza, nada mas).
- La oclusion especular con normal doblada es una aproximacion (cono contra cono, solo donde hay oclusion: `mix(1, so, (1 - ao) * 4)`); revisar a ojo en metales en esquinas.
- Las sondas usan la posicion actual del objeto como centro del paralaje (sigue al origen flotante); si se mueve despues de hornear, el Inspector avisa y hay que volver a hornear.

## Probar a mano (GPU / editor)
1. Escena interior (habitacion con suelo pulido): *+ Reflection Probe*, ajustar la caja a la habitacion, *Hornear esta sonda*. Ver el suelo y un metal con rugosidad media: el reflejo de las paredes debe quedarse "pegado" a ellas al moverse (proyeccion de caja). Comparar con `render.probe.Placed 0`.
2. Dos sondas solapadas (habitacion dentro de un pasillo, con la misma prioridad y con prioridad 1): transicion suave en el borde de fundido de la habitacion, sin saltos ni costuras; fuera de ambas, la sonda de la camara.
3. `bake_reflection_probes` por MCP (con y sin `entity`), cerrar y abrir la escena: el `.crrefl` se carga y se ve igual; en CramionPlayer tambien.
4. Escena sin sondas (Horizonz): misma imagen y mismo tiempo de frame que antes; abrir la escena no espera a la GPU.
5. `render.ao.Gtao 1` en nivel Medio/Alto: esquinas y contactos sin el oscurecimiento plano del SSAO en suelos de refilon; sin ruido visible con TAA; con TAA apagado, el patron fijo sin parpadeo. Metales en rincones: el reflejo del cielo se apaga dentro, no en lo abierto. En Bajo y en el movil, el SSAO de siempre. Con GI por rayos, ni uno ni otro.
6. Capas de validacion (`CRAMION_VALIDATION`): hornear varias sondas, cambiar de escena y volver; sin errores (layouts del array de cubos y del cubo de horneado).
7. Medir el coste: GTAO frente a SSAO (pase SSAO del perfilador) y la iluminacion con 2 sondas colocadas solapadas.

## Revision (segundo agente)

Revisado el diff `horizonz-1031...rm-luz` con escepticismo (logica, retrocompatibilidad, sincronizacion de Vulkan, coste con la funcion sin usar, pruebas). Lo confirmado y arreglado:

- **Mezcla de sondas anidadas (error de logica, CPU y shader)**: la clave `prioridad * 2 + peso` hacia que, con la misma prioridad, el edificio (peso 1) mandase sobre la habitacion durante todo su borde de fundido y que la habitacion apareciese de golpe al final del fundido (el caso tipico: una sonda por habitacion dentro de una grande). Ahora manda la primera que pesa algo en el orden de la lista (prioridad y luego caja mas pequena, el de `selectPlacedProbes`) con su propio peso, y de las demas la que mas pesa rellena el resto (`blendPlacedProbes` y `placedProbeReflection`, misma regla; ademas, si la primera cubre del todo no se mira ninguna mas). Pruebas nuevas: habitacion y edificio con la misma prioridad (fundido, peso continuo y total 1 en 41 puntos del borde) y tres sondas (la segunda es la que mas pesa, no la siguiente de la lista). Las pruebas de antes usaban prioridad 1 para la habitacion y no veian el fallo.
- **Coste con el GTAO apagado en gama baja**: la imagen del SSAO pasaba a RGBA16F en todo PC, tambien en nivel Bajo, donde el GTAO nunca se usa (el doble de ancho de banda justo en las GPU mas flojas). Ahora `ssaoFormat()` es RG16F en Bajo y en el modo compatible (el nivel se decide antes de crear la imagen y no cambia), `gtao_pass_` no se crea alli y `gtaoActive()` exige ademas el formato RGBA16F.
- **MCP `bake_reflection_probes`**: la descripcion decia "nombre, ruta o UUID" pero solo aceptaba UUID; ahora acepta UUID o nombre, y una entidad sin Reflection Probe da error en vez de "0 horneadas" sin explicacion.

Revisado sin cambios: retrocompatibilidad (componente nuevo con valores por defecto, escenas sin el se leen igual; sin `.crrefl` no se sube nada ni se espera a la GPU al abrir; `render.probe.Placed` sin cubos sale al principio de `fillPlacedProbes` y el shader sale con `placed_info.x = 0`; `GpuLights` solo crece al final), sincronizacion del horneado (espera a la GPU, recoge la captura de la camara terminada, usa el hueco de la sonda y su fence, restaura las matrices; la interfaz se construye entre frames, asi que los botones del Inspector y el MCP hornean fuera de un frame), cambio del array de cubos (`waitIdle` antes de soltar el viejo y reescritura del binding 31 en todos los sets, tambien el de la sonda), formulas de XeGTAO (horizontes, integral y normal doblada coinciden con la referencia) y el `.crrefl` (escritura atomica, FNV, topes antes de reservar).

Tras la revision: compilan CramionFX, CramionCore, CramionEditor y CramionPlayer; `CramionReflectionProbeTests` 68/0, `CramionShaderTests` 19/0, `CramionCoreTests` 102/0; `lighting.frag`, `lighting_mobile.frag`, `lighting_unlit.frag` y `gtao.frag` compilan con glslc.

Sigue pendiente lo de la lista de arriba (movil, agua/vidrio, gizmo, compresion, API de scripts, mips de profundidad del GTAO) y probar en GPU.

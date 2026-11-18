# NOTAS rm-salida — Salida de imagen (baja latencia, HDR10/scRGB, LUT .cube)

Rama `rm-salida` (desde `horizonz-1031`). Todo apagado por defecto: sin tocar los ajustes, la imagen, la
presentacion y las escenas son las de antes.

## Entrada para el CHANGELOG (estilo del proyecto)

- **Baja latencia** (`GraphicsSettings::low_latency`: apagada / activada / activada + boost): con NVIDIA Reflex
  (`VK_NV_low_latency2`: `vkLatencySleepNV` antes de leer la entrada, marcas de simulacion, envio y presentacion,
  modo boost), AMD Anti-Lag 2 (`VK_AMD_anti_lag`: marca de entrada y de presentacion) o, con cualquier GPU que tenga
  `VK_KHR_present_id` + `VK_KHR_present_wait`, esperando a que se presente el frame anterior (boost) o el de antes
  (activada) en vez de acumular frames. Elige solo, en ese orden. Sin ninguna extension, igual que antes. El juego
  exportado espera al principio del bucle, antes de leer la entrada (`VulkanRenderer::beginLatencyFrame`); si nadie la
  llama, `drawFrame` lo hace al empezar. Esperas con tope de 100 ms.
- **Latencia en `[Rendimiento]`**: al final de la linea, `| latencia X ms (modo, metodo) | salida SDR/HDR10/scRGB`.
  Con Reflex, sus tiempos (de la entrada al final del render en la GPU); con present_wait, de empezar el frame a
  presentarse; sin ellos, estimada por las fences (`~X ms`, una cota).
- **Salida HDR10 (PQ, BT.2020) y scRGB** (`GraphicsSettings::hdr_output`: SDR / automatica / HDR10 / scRGB, con
  `hdr_paper_white` 80..500 nits y `hdr_max_nits` hasta 10000) cuando la pantalla y el driver la ofrecen
  (`VK_EXT_swapchain_colorspace`; metadatos con `VK_EXT_hdr_metadata`). Si no se puede, SDR (y lo dice el log).
  La escena terminada (composicion, FXAA, UI en el mundo) va al brillo de papel; la UI de pantalla (ImGui, la UI del
  juego) se dibuja aparte sobre transparente y se compone encima, siempre al brillo de papel; las luces de la
  escena suben por encima del papel con un mapa de ganancia: AgX con el hombro llevado hasta los nits maximos
  contra el AgX de SDR, solo donde el SDR ya se satura (los medios tonos no cambian). `hdr_output.frag`.
- **LUT 3D importable (.cube)** en la gradacion de color del componente PostProcessing (`color_lut`, ruta relativa al
  proyecto, y `color_lut_intensity`): la de DaVinci Resolve, Photoshop o Lightroom, sobre la imagen ya con el tono en
  sRGB (`composite.frag`, binding 7, textura 3D RGBA16F). Lee `LUT_3D_SIZE` (2..256), `LUT_1D_SIZE` (se convierte
  a un cubo de 33), `DOMAIN_MIN/MAX`, `LUT_3D_INPUT_RANGE`, `TITLE`, comentarios, BOM y CRLF; los errores dicen la
  linea. Los volumenes la mezclan: la intensidad sube o baja desde 0 y entre dos LUT la ruta cambia a mitad.

## Archivos

Nuevos:
- `CramionFX/include/CramionFX/vk/DisplayOutput.h`, `DisplayOutputVk.h`, `src/vk/DisplayOutput.cpp`: enums
  (`HdrOutput`, `OutputEncoding`, `LowLatencyMode`, `LatencyBackend`), eleccion del formato de la swapchain
  (`chooseOutputFormat`), PQ, eleccion del metodo de latencia, `presentWaitTarget`, `LatencyMeter`. Sin GPU.
- `CramionFX/include/CramionFX/vk/CubeLut.h`, `src/vk/CubeLut.cpp`: lector .cube, muestreo trilineal en CPU,
  conversion a half.
- `CramionFX/src/vk/VulkanRendererOutput.cpp`: todo lo del renderizador (latencia, HDR, LUT).
- `CramionFX/shaders/hdr_output.frag`.
- `CramionCore/tests/OutputTests.cpp` (+ objetivo `CramionOutputTests` en `CramionCore/CMakeLists.txt`).

Tocados (bloques localizados):
- `VulkanSwapchain.h/.cpp`: formato por `chooseOutputFormat` (el SDR es el mismo de antes), `uiFormat()`,
  `encoding()`, `setOutputRequest`, `VkSwapchainLatencyCreateInfoNV`, `setHdrMetadata`.
- `VulkanDevice.h/.cpp`: extensiones opcionales de salida (`outputExtensions()`); `CRAMION_OUTPUT_EXTENSIONS=0` no
  activa ninguna. Solo en el camino de escritorio (el modo compatible no cambia).
- `VulkanInstance.cpp`: `VK_EXT_swapchain_colorspace` si esta.
- `VulkanRenderer.h/.cpp` (shared): ganchos en `drawFrame` (espera, fence, envio, presentacion), `recordPostProcessPass`
  (desvio a la salida HDR), `recreateSwapchain`/`initialize` (`createOutputTargets`), `updatePostDescriptors`
  (binding 7 y sets HDR), binding 7 de la composicion y +1 textura por composicion en los dos pools, el FXAA y los
  pipelines de herramientas con `uiFormat()` (igual a `imageFormat()` en SDR), `postProcessPush()` sacado de
  `recordPostProcessPass` (mismo codigo).
- `GraphicsSettings.h` (4 campos al final), `PostProcessSettings.h` (2 campos), `GpuTypes.h` (`lut_min`/`lut_max` al
  final de `GpuCompositeSettings`, 13 -> 15 vec4; `GpuHdrOutputPush` de 32 bytes).
- `composite.frag`: binding 7 y la LUT tras saturacion/viveza (antes del daltonismo y la vineta).
- `CramionCore/src/ecs/Components.cpp`: reflect (`color_lut`, `color_lut_intensity`) y `blendPostProcess`.
- `CramionCore/src/ecs/RenderSync.cpp`: CVars y ruta absoluta de la LUT.
- `CramionEditor/src/GraphicsConfig.cpp`: opciones, Graphics.ini, `Graphics.set`.
- `CramionEditor/src/EditorApp.cpp`: seccion "Salida" en los ajustes graficos.
- `CramionEditor/player/PlayerMain.cpp`: `beginLatencyFrame()` antes de leer la entrada y campos al final de
  `[Rendimiento]`.

## CVars

- `render.output.LowLatency` (-1 = el ajuste grafico, 0 apagada, 1 activada, 2 boost). No se guarda.
- `render.output.Hdr` (-1 = el ajuste grafico, 0 SDR, 1 automatica, 2 HDR10, 3 scRGB). No se guarda.
- Variable de entorno `CRAMION_OUTPUT_EXTENSIONS=0`: no activa present_id/present_wait/Reflex/Anti-Lag/metadatos HDR.

## API (sin cambios en el SDK: van por las opciones de Graphics, como el resto)

- `Graphics.set("low_latency", "off" | "on" | "boost" | true/false)`, `Graphics.set("hdr_output", "off" | "auto" |
  "hdr10" | "scrgb" | true/false)`, `Graphics.set("hdr_paper_white", n)`, `Graphics.set("hdr_max_nits", n)`.
- Lectura: `low_latency_active` (Reflex / Anti-Lag 2 / present_wait / ninguno), `latency_ms`, `hdr_active`.
- Se guardan con `Graphics.save()` (Graphics.ini: `low_latency`, `hdr_output`, `hdr_paper_white`, `hdr_max_nits`;
  los ini de antes no las traen = apagadas).
- `Graphics.post.color_lut` / `color_lut_intensity` (por reflexion del componente).
- C++: `VulkanRenderer::beginLatencyFrame()`, `setOutputOverrides()`, `outputStatus()`.

## Pruebas

`CramionOutputTests` (nueva, consola, sin GPU): lector .cube (cubo invertido, orden de Adobe, trilineal, identidad
con BOM/CRLF, dominio, `LUT_3D_INPUT_RANGE`, 1D -> 3D, 8 errores con su linea, half float, archivo), eleccion del
formato (SDR igual que antes, HDR10, scRGB, vueltas a SDR, sin la extension de la instancia, lista vacia, formato de
UI estable), PQ (valores de ST 2084 e ida y vuelta), limites de nits, metodo de latencia, objetivos de present_wait,
medidor de latencia, tamanos de `GpuCompositeSettings` / `GpuHdrOutputPush`, guardar/leer la LUT en una escena,
escena de antes sin las claves, y la mezcla de volumenes.

Resultado (build-rm, Release, clang): `CramionOutputTests` 64 comprobaciones, 0 fallos; `CramionCoreTests` 102, 0
fallos; `CramionApiGraphicsApiTests` 69, 0 fallos. Compilan `CramionFX`, `CramionCore`, `CramionEditor` y
`CramionPlayer`. Los shaders compilan con glslc (vulkan1.3 y el modo compatible). No se probo en GPU.

## Pendiente

- Probar en GPU (no se abrio ninguna ventana): Reflex en una NVIDIA, Anti-Lag 2 en una AMD con driver 24.x+,
  present_wait en otra; HDR10 y scRGB en una pantalla HDR con Windows en HDR.
- Mapa de ganancia: aproximacion por luminancia con AgX aunque la escena use otro tono (Neutral, ACES); la UI en el
  mundo (WorldUiPass) y los videos encima de un cielo muy brillante tambien suben (no hay mascara de UI en el mundo).
  Lo correcto seria un ldr_color_ de 16 bits en HDR: muchos pases lo escriben con `kLdrFormat` (capturas, VR, video).
- En el editor la vista de escena es una imagen de ImGui: con HDR se ve al brillo de papel, sin luces por encima.
- Reflex: sin `vkQueueNotifyOutOfBandNV` ni marcas de los hilos; `PC Latency` de GeForce Experience sin probar.
- Anti-Lag 2: solo `maxFPS = 0` (sin tope propio).
- Android / modo compatible: HDR y baja latencia se fuerzan a apagados.
- Selector de archivo .cube en el Inspector (hoy es un campo de texto con la ruta).

## Riesgos

- `GpuCompositeSettings` crece 32 bytes (UBO, no push constants) y la composicion tiene un binding mas (dos pools con
  +1 textura por set): si otro bloque toca esos pools, revisar las cuentas al fusionar.
- La ruta de la LUT se carga en el hilo de render con `waitIdle` al cambiar (raro: al abrir la escena o al editar).
- Un driver que anuncie present_wait y no avise: la espera tiene tope de 100 ms; un fallo de la llamada desactiva el
  metodo y sigue sin el.
- En HDR los pipelines de ImGui siguen con el formato SDR (`nativeHandles().swapchain_format` = `uiFormat()`): si un
  juego crea pipelines propios con `swapchain_.imageFormat()`, tendria que usar `uiFormat()`.

## Probar a mano (GPU / editor)

1. Juego exportado sin tocar nada: misma imagen y mismo `[Rendimiento]` (con los campos nuevos al final).
2. `Graphics.set("low_latency", "on")` y `"boost"`: el log dice el metodo; `latency_ms` debe bajar respecto a "off".
3. `render.output.Hdr 1` con Windows en HDR: el log dice "salida HDR10"; la UI no deslumbra, el sol y las luces
   brillan mas que el blanco de la UI; `hdr_paper_white` cambia el brillo general; con Windows en SDR vuelve a SDR.
4. Un `.cube` en el PostProcessing (p. ej. una LUT de Resolve): se ve igual que en Resolve; intensidad 0..1;
   un volumen local con otra LUT funde al entrar.

# Plan de optimización de Cramion para PCs de gama baja

Base: el código de la 2.4.13 sin commit. Este análisis es de solo lectura: no se ha compilado ni editado nada.

Equipo de referencia:
- GPU integrada (Intel UHD 620 o Iris Xe, AMD Vega 8) o una GTX 1050.
- Pantalla de 1080p, 8 GB de RAM y CPU de 4 núcleos.

## Resumen

**El presupuesto adaptativo falla justo en los PCs lentos.** Esto está confirmado en el código, en los logs y por simulación.
- En el 2.º frame el perfilador de GPU todavía no tiene datos, y el presupuesto se siembra con 0 ms.
- A partir de ahí, el filtro de tirones descarta para siempre cualquier muestra de 8 ms o más.
- En una GPU lenta todos los frames superan los 8 ms. La «media» se queda en 0, el gobernador cree que le sobra tiempo y sube todas las palancas hasta la calidad máxima en unos 35 s.
- Es exactamente el «media 0, palancas 0» del log del jugador. Que se imprima «palancas» demuestra que el presupuesto estaba encendido.

**El juego exportado se lleva los ajustes del visor del editor** (`ProjectSettings/Graphics.ini`):
- 4 de 13 juegos van con el presupuesto apagado.
- 3 juegos piden 240 FPS.
- `test` lleva sombras de 6144 y texturas de 16K.
- 10 de 13 juegos activan el trazado de rayos.
- Esos valores pisan los que corresponden a la gama del PC.

**Lo más caro no tiene palanca.**
- El DOF y el motion blur se dibujan después del escalado, a la resolución de salida. Son 6,3 de los 18 ms de BlackHouse y no bajan al bajar la resolución.
- Las nubes y el agua no se pueden bajar. La volumétrica solo baja a la mitad de pasos.
- La luz horneada sigue pagando el filtro SVGF de la GI y las cascadas del sol.

**Mundos grandes.**
- Los LODs se generan, pero se pierden en las variantes con `.crmat`.
- El hilo principal repite en cada frame trabajo que no cambia: ríos, vegetación, ondas.
- Keryan está limitado por la CPU: 15-16 ms de frame con 9 ms de GPU en el PC del autor.

**La Fase 1** va en la próxima versión: la 2.4.14, o la 2.4.13 si aún no se publica. Casi todo es esfuerzo S. Contiene:
- arreglar el gobernador y encenderlo en los juegos exportados, con un objetivo de FPS sensato;
- palancas nuevas y un arranque de gama baja que cubran DOF/motion blur, nubes y volumétrica;
- luz horneada más barata;
- LODs en las variantes;
- arreglos rápidos de CPU;
- un log que permita medir en el PC del jugador.

**Ganancia estimada.** Son estimaciones, no medidas. Supuestos:
- Una GTX 1050 o una Iris Xe es entre 6 y 8 veces más lenta que la RTX 4060 Ti del autor en estos pases (FP32 de unos 2 frente a 22 TFLOPS; 112 GB/s, o 50-68 GB/s compartidos, frente a 288 GB/s más 32 MB de L2).
- Una CPU de 4 núcleos es entre 2 y 3 veces más lenta en el hilo principal.
- La Vega 8 tarda entre 1,5 y 2 veces lo de la tabla; la UHD 620, entre 3 y 3,5 veces.

| Escena (1080p, GTX 1050 / Iris Xe) | Hoy | Tras la Fase 1 | Tras la Fase 2 |
|---|---|---|---|
| Interior tipo BlackHouse (DOF+MB, 30 luces, GI, SSR) | 110-145 ms (7-9 FPS) | 18-30 ms de GPU (35-55 FPS) | 13-20 ms |
| Exterior tipo test (océano, nubes, agua, DOF+MB) | 105-140 ms | 20-35 ms | 14-22 ms |
| PortalLab (38 luces, 2 Render Textures) | 200-270 ms (4-5 FPS) | 35-55 ms | 25-40 ms |
| Mundo grande tipo Keryan | GPU 55-70+ y CPU 30-45 ms (≈15 FPS) | GPU 30-45 y CPU 20-35 ms (≈22-30 FPS) | GPU 25-35 y CPU 12-20 ms |

**Sobre los 4 puntos del análisis pegado:**

1. **Presupuesto en el juego exportado.** Confirmado; la causa está en C1. Afecta también al visor del editor.
2. **Horneado («0 mallas Static»).** Está arreglado en el código de la 2.4.13, que no está publicada; no lo he comprobado en un juego exportado.
   - Aun así, hornear no quita todavía las sombras en tiempo real: las luces son Mixtas por defecto.
   - Con el sol en Horneada, las cascadas se siguen dibujando (C4). Lo cubren F1-6, F1-8 y F3-2.
3. **LODs.** El umbral de 4.096 triángulos y «solo rígidos» es cierto.
   - Lo de «solo si se activan en la escena» no: `lods` está encendido por defecto (`PostProcessSettings.h:138`).
   - El «0 con LOD» viene de que las variantes con `.crmat` pierden la cadena de LODs (C8). Lo cubre F1-10.
4. **2.4.10 y 2.4.11.** Confirmado. La 2.4.10 solo toca el presupuesto en VR y la 2.4.11 la iluminación y el SSR (`CHANGELOG.md`).

## Causas confirmadas

Leyenda: (código) = leído en el código; (log) = visto en los logs del autor; (sim) = reproducido con un port 1:1 en Python; (inv.) = dato de la investigación que no he vuelto a comprobar.

**C1. La media del presupuesto se bloquea en 0 y el gobernador sube todo** (código, log, sim)
- `GpuProfiler.cpp:38` empieza con un total de 0. `collect()` devuelve false mientras el hueco del frame no se ha grabado (`:83-85`, con 2 frames en vuelo). En el primer dato, el total suavizado se siembra con el valor crudo (`:123`).
- `VulkanRenderer.cpp:4597-4624` llama a `budget_.update(dt, gpu_profiler_.totalMilliseconds(), timings())` en todos los frames, haya medida o no.
- Frame 1: dt es 0, así que `update()` sale en `FrameBudget.cpp:304`. Frame 2: llega `gpu_ms = 0`, se siembra `ema_ms_ = 0` y `has_sample_ = true` (`FrameBudget.cpp:351-353`).
- Desde entonces, `gpu_ms < ema_ms_ * 4 + 8` (`:354`) descarta toda muestra de 8 ms o más. Solo `reset()` lo desbloquea (`:148-159`).
- Con la media a 0, `under_seconds_` no para de crecer (`:374`). `raiseOne()` acepta los pasos sin medir con un coste de 1 ms (`:285-286`) y aprende 0 (`:368`).
  - `governor_sim.py`, arranque Bajo a 70 ms: queda «media 0.00, palancas [0,…,0]», con las 9 subidas entre t = 3 s y t = 34 s.
  - `ema_sim.py`: con un primer dato de 10 ms y la escena a 18 ms, la media queda en 0 (BlackHouse.log:283/285, «GPU 17.99 ms (media 0»). Con un primer dato de 3 ms funciona.
  - En una GPU lenta el primer dato siempre pasa de 8 ms.
- `test.log:107` sí muestra una media real (17,46). La escena `test/Main` tiene `lod_pixel_error` 0,5, por debajo del 1,0 por defecto. Eso dispara `postQualityRaised` (`FrameBudget.h:173-179`), luego `reset()` (`VulkanRenderer.h:337`), y la media se vuelve a sembrar con un valor real. BlackwoodHouse tiene 8,0, así que no hay reset y la media se queda en 0.
- Los tests no lo ven: `testFrameBudget` (`MeshTests.cpp:800+`) da un GPU válido desde el primer frame.

**C2. El juego exportado hereda los ajustes del visor del editor** (código, archivos)
- La casilla «Optimización adaptativa» y el combo «Objetivo» (`EditorApp.cpp:2460-2480`, con hasta 240 FPS en `:2466`) guardan al momento en `ProjectSettings/Graphics.ini` (`:2666-2676`).
- La exportación empaqueta `ProjectSettings` (`EditorExport.cpp:336`). El jugador de Windows lo carga como base (`PlayerMain.cpp:427`) y encima el suyo (`:428`).
- Esos valores mandan:
  - `setEnabled(adaptive)` (`VulkanRenderer.cpp:3158`) deja `levels_ = {}` (`FrameBudget.cpp:112`), sin el arranque Bajo.
  - Un tamaño de sombras o texturas explícito anula la gama (`VulkanRenderer.cpp:3132-3152`).
- La configuración de compilación de escritorio no escribe ninguna clave de calidad (`BuildConfig.cpp:157-180`). Android sí fuerza `adaptive = true` (`PlayerMain.cpp:642`).
- Archivos exportados reales:
  - `adaptive=0`: BlackHouse, PortalLab, STREET KINGS y test.
  - `target_fps=240`: Night, Reinos y WhatIs. Keryan pide 30.
  - test: `shadow_resolution=6144` (576 MiB de cascadas) y `texture_max_size=16384`.
  - test y BlackHouse: DLSS en modo DLAA. Sin DLSS cae a TAA con la misma escala, es decir, al 100 % (`VulkanRenderer.cpp:8588-8594`).
  - `ray_tracing=1` en 10 de 13.

**C3. Los pases más caros no tienen palanca** (código, log)
- Las palancas (`FrameBudget.h:57-68`) y los pases que lee el gobernador (`FrameBudget.cpp:309-321`) no incluyen el DOF/motion blur, las nubes ni el agua.
- DOF y motion blur:
  - Se graban después del escalado (`VulkanRenderer.cpp:5435`) sobre `postSource()` a resolución de salida (`:9031`), con una copia completa de la imagen por efecto (`:9058-9091`).
  - Usan 64 muestras de DOF (`camera_fx.frag:79`) y 24 de motion blur (`:159`).
  - Cuestan 6,27 de 17,99 ms en BlackHouse.log:283.
  - En WhatIs.log:68→76 pasan de 5,6 a 4,4 ms aunque la resolución interna baja de 1286x724 a 960x540. Con todas las palancas al tope, son el 53 % del frame.
- Nubes:
  - Se graban antes del G-buffer (`:5371`), así que se recorre también el cielo tapado por la geometría. Se dibujan aunque la cobertura sea 0 (`:9017`).
  - Hacen hasta 128 pasos con 6 de luz (`clouds.frag:59-60`) y cuestan 2,9 ms en test.log.
  - `RenderSync.cpp:3568` las vuelve a encender en cada frame. Por eso el «sin nubes» del móvil en Baja/Media (`PlayerMain.cpp:638`) no dura.
- La palanca de volumétrica solo baja a 16 pasos (`FrameBudget.cpp:401-404`); el pase cuesta 3,75-4,69 ms en PortalLab.log:190-192.
- La estimación de RenderScale cuenta como dependiente de la resolución todo lo que no son sombras, incluido lo que va después del escalado (`FrameBudget.cpp:203`).

**C4. La luz horneada sigue pagando lo caro** (código)
- **GI.** Con GI horneada se dibuja `baked_gi_pass_` (`VulkanRenderer.cpp:6558`) y después el SVGF completo: temporal, 5 pasadas à-trous y copia de momentos (`:6565-6632`). Pero `baked_gi.frag` no tiene ruido por frame: su «hash» es una tabla espacial. Además, la palanca GI no hace nada con GI horneada (`:6452`).
- **Sol horneado.** `lighting.frag:1447-1450/1616` ignora las cascadas, pero:
  - Las cascadas y las sombras del sol por rayos solo miran `sunShadows()` (`VulkanRenderer.cpp:4077`, `4343-4350`, `5539`).
  - Volumétrica, agua, vidrio y fluidos no miran el flag del sol horneado.
  - Las luces son Mixtas por defecto (`Components.h:393`).
- La documentación dice que una luz Horneada «no cuesta nada en el juego» (`docs-src/pages/iluminacion-horneada.html:53`), y para el sol es falso.

**C5. La resolución dinámica existe, pero es tosca** (código)
- Tiene 6 escalas fijas (`FrameBudget.cpp:22`).
- Cada paso marca `targets_dirty_` (`VulkanRenderer.cpp:3309`), y `recreateRenderTargets()` hace `device_.waitIdle()`, rehace el G-buffer y todos los destinos e invalida los historiales (`:3101-3119`).
- Cambia además el sesgo de mips, lo que rehace los samplers de todos los modelos con otro `waitIdle`. Ver `computeRenderExtent` y `applyTextureFiltering`, y la nota de la 2.4.10.
- La escala del presupuesto multiplica la del preset sin suelo (`:3265`). Keryan (FSR 1 Calidad, 0,67) arrancaría en un PC Bajo al 0,45: 862x453.

**C6. El modelo de costes aprende muy poco a FPS bajos** (código, sim)
- Hay doble suavizado: el perfilador al 3 % por frame (`GpuProfiler.cpp:12/120/123`) y el EMA del presupuesto (`FrameBudget.cpp:355`). A eso se suman 2 frames de latencia y una sola medida al asentarse (`:232-238`, `:362-369`).
- En `learned_raw_sim.py` (scratchpad), un ahorro real de 10 ms se aprende hoy como:

  | FPS | Aprendido hoy |
  |---|---|
  | 60 | 3,7 ms |
  | 30 | 0,6 ms |
  | 15 | 0,2 ms |
  | 10 | 0,1 ms |

- Con muestras crudas y un mínimo de 6 muestras se aprenden 5,5-8 ms.
- Por eso sube pasos caros demasiado pronto, tiene que volver a bajarlos y bloquea esas subidas entre 15 y 240 s (`:381-387`): oscila.

**C7. La gama se decide solo por la VRAM, y el arranque Bajo deja fuera lo caro** (código, log)
- `tierFor` (`FrameBudget.cpp:31-36`) clasifica así:
  - Integrada o menos de 3 GB: Bajo.
  - Menos de 6 GB: Medio.
  - Menos de 11 GB: Alto.
- La 4060 Ti sale como «Alto» (BlackHouse.log:19). Por la misma regla, una GTX 1070 o una RX 580 de 8 GB también son Alto, y una GTX 1050 Ti o 1650 de 4 GB son Medio.
- El arranque Bajo (`:122-140`) no toca SSAO, SSR, DOF/MB, nubes ni agua.
- El preset «Baja» (`GraphicsConfig.cpp:228-242`) solo cambia el escalador, la nitidez, la sonda, el RT y el culling de oclusión.
- La distancia de sombras está fija en 100 m: `setShadowDistance` nunca se llama (`ShadowCascades.h:77`).

**C8. Hay LODs, pero casi no se usan** (código, log)
- `RenderSync::buildVariant` no copia `lods`, `lod_indices` ni `lightmap_uvs` (`RenderSync.cpp:717-721`).
- Cada modelo se sube dos veces: la base «494 meshlets en 6 LODs», que nadie dibuja, y la variante «257 meshlets en 1 LODs» (Keryan.log:459 frente a 822). Lo mismo pasa con el lote estático de test (test.log:53-56).
- En Keryan, el 100 % de los 3.540 renderers dibujables llevan `.crmat` (inv.).
- El lote estático contiene la cámara en su esfera, así que `chooseLod` devuelve 0 (`VulkanRenderer.cpp:1963`).
- El umbral es de 4.096 triángulos y solo para rígidos (`MeshLod.cpp:21`, `32-35`). El de objetos diminutos es de 0,5 px (`FrameBudget.cpp:416-419`).
- Fallo latente: `drawCpuActors` recorre los índices de `lodSubmeshes(lod)` (`:1983`) pero lee `model.submeshes()[i]` (`:6032`). En modo compatible (Android, `CRAMION_VK_COMPAT`) dibuja rangos equivocados en cuanto hay un LOD mayor que 0.

**C9. Los mundos grandes están limitados por la CPU, y la CPU repite trabajo** (log, código; el reparto en ms es inv.)
- Keryan.log:1495-1548: frame p50 de 14,9-16,6 ms con 8,8-9,5 ms de GPU.
- La línea central de cada río se rehace en cada muestreo (`Water.cpp:296`, llamado desde `RenderSync.cpp:1896` y `2106`).
- `syncFoliage` rehace las líneas de los ríos y copia el agua a cada bosque en cada frame (`:2439`, `:2461`).
- La flotación rehace las líneas en cada paso fijo (`PhysicsSystem.cpp:3105`).
- Los subpasos de `RenderSync::sync` no tienen zona de perfilado (`:2183-2204`).

**C10. Diagnóstico engañoso que además frena** (código, log)
- En el juego, si un frame pasa de 50 ms, se escribe una captura de 120 frames de forma síncrona, con el mutex tomado, como mucho cada 10 s (`Profiler.cpp:25`, `256-276`). En un PC lento eso pasa todo el rato.
- La primera línea de `[Rendimiento]` incluye los frames de carga: p95 de 251 ms en BlackHouse.log:283 frente a 20 ms en `:285`.
- El compute del principio (terreno, TLAS, hierba, FFT del océano, fluidos) se cobra a «Sombras (cascadas)», y el vidrio a «Agua» (`VulkanRenderer.cpp:5332-5397`).
- Cada cámara con Render Texture repite un `drawFrame` entero (`:7512`) y alimenta sola al presupuesto. PortalLab tiene 2 (CamAzul y CamNaranja). Que sus 28 FPS con «GPU 11,4 ms» equivalgan a unos 34 ms reales es una inferencia.

**C11. La mezcla de volúmenes de post-proceso puede reiniciar el presupuesto en cada frame** (código; no medido en la práctica)
- `RenderSync.cpp:3677` llama a `setPostProcess(blended)` en cada frame.
- Cualquier bajada de más del 1 % en `lod_pixel_error`, o un efecto que se encienda, reinicia el presupuesto: vuelven los niveles de salida y se borra lo aprendido.

**C12. El PC del autor no recorre el camino de gama baja** (log, código)
- La 4060 Ti usa mesh shaders para la geometría y las cascadas (BlackHouse.log:10/25) y casi siempre RT.
- Las GTX 10xx, Intel Xe-LP/UHD y Vega no tienen `VK_EXT_mesh_shader`, y usan sonda, SSGI y mapa de cielo sin RT.
- Con `CRAMION_NO_MESH=1` y `ray_tracing=0` se puede medir ese camino.

## Plan por fases

Esfuerzo: **S** es 1 día o menos; **M**, de 2 a 5 días; **L**, más de una semana. Hay que contar con compilar de uno en uno con `-j 2`. «Defaults» significa que el cambio lo nota el usuario y debe ir en las notas de versión.

| # | Mejora | Esf. | Riesgo | Ganancia en gama baja (estimada) | Coste visual | Defaults |
|---|---|---|---|---|---|---|
| F1-1 | Arreglar la «media 0» y alimentar el presupuesto con muestras frescas | S | Bajo | Condición para todo; tipo BlackHouse con presupuesto: ~125 → 55-65 ms solo con las palancas actuales | Ninguno | Sí |
| F1-2 | Juego exportado: presupuesto siempre activo, objetivo hasta los Hz de pantalla (máx. 144), ajustes del proyecto como topes por gama, RT fuera por debajo de Alto | S-M | Medio-bajo | Gobernador en 4/13 juegos más; sin 576 MiB de cascadas ni texturas de 16K en GPUs de 2-4 GB | Sombras y texturas de su gama; sin RT ni sonda en Bajo | Sí |
| F1-3 | Palanca «Desenfoque de cámara» (DOF + motion blur); RenderScale sin contar lo de después del escalado | S-M | Bajo-medio | 40-50 ms menos en GTX 1050 con DOF/MB; con F1-1 y F1-2, tipo BlackHouse ~18-30 ms | Solo sin margen: bokeh con ruido, luego sin DOF/MB | Sí |
| F1-4 | Arranque Bajo y Medio que cubra SSAO, SSR y DOF/MB; sin mapa de cielo con iluminación ligera | S | Bajo | Primeros segundos ~40-50 % más baratos en Bajo | Arranque más sobrio; vuelve si hay margen | Sí |
| F1-5 | Log del jugador útil, marcas de GPU fieles, capturas de tirones que no paren el juego | S | Nulo | Quita una escritura síncrona cada 10 s; permite medir | Ninguno | Sí (devs) |
| F1-6 | GI horneada sin filtro SVGF | S | Bajo | GI de ~20 a 2-3 ms (GTX 1050, nativa) | Ninguno esperado | No |
| F1-7 | Resolución interna sin doble reducción, con suelo e histéresis | S | Bajo | Nitidez y menos tirones (Keryan en Bajo: 862x453 → 1280x673) | Mejor | Sí |
| F1-8 | Sol horneado sin cascadas ni sombras por rayos si nadie las lee | S | Bajo-medio | Quita las cascadas (~7-20 ms) en escenas con el sol horneado | Ninguno en opacos | Sí |
| F1-9 | Palancas más hondas: volumétrica en 3 niveles, nubes por pasos; nubes del móvil apagadas de verdad | S-M | Bajo-medio | Nubes ~20 → 6-9 ms; volumétrica con muchas luces ~30 → 4-6 ms | Nubes y niebla más suaves | Sí |
| F1-10 | LODs en las variantes `.crmat` y los 2 fallos latentes | S | Bajo-medio | Keryan: 40-50 % menos triángulos en la vista de la ciudad | Algún cambio de LOD lejano | Sí |
| F1-11 | Objetos diminutos: umbral por gama con histéresis | S | Bajo | Keryan: ~880 instancias menos (hoy 16-35) | Lo de menos de 3-4 px desaparece lejos | Sí |
| F1-12 | CPU: ríos, vegetación, ondas y flotación sin trabajo repetido | S-M | Bajo | Keryan: 3-7 ms menos (autor); 8-18 ms menos en 4 núcleos | Ninguno | No |
| F1-13 | Render Textures a un ritmo según la gama | S | Bajo-medio | PortalLab: de 3 a ~1,7 vistas por frame | Portales a 20-30 Hz | Sí |
| F2-1 | Aprendizaje por tiempo y niveles guardados por PC | M | Bajo-medio | No oscila a 10-30 FPS; el 2.º arranque va directo a sus niveles | Ninguno | No |
| F2-2 | Gama por rendimiento real; RT solo con hardware de RT | M | Bajo-medio | GTX 1060-1070 y RX 580 (hoy Alto) arrancan 2-4 veces más baratas | Arranque más suave | Sí |
| F2-3 | Nubes solo donde se ve cielo, y calidad Baja | M | Medio | Interiores ~0; exteriores −40-60 % (en Bajo, ~20 → 2 ms) | Ninguno o bordes suaves | Sí |
| F2-4 | DOF/MB por tiles, a media resolución en Bajo/Medio, sin copias | M | Medio | Con DOF/MB encendidos: ~44 → 3-6 ms | Casi ninguno | No |
| F2-5 | Sonda de reflexión a 256×256 sin parar la CPU | M | Medio | Quita tirones de ~1 frame por cara (camino sin RT) | Ninguno | Sí |
| F2-6 | SSGI/SVGF en calidad Baja | M | Medio | GI ~25 → ~8 ms (nativa) | GI con más ruido | Sí |
| F2-7 | Iluminación y agua en calidad Baja | S-M | Bajo-medio | 35-50 % menos en cada pase | Reflejos y AO menos finos | Sí |
| F2-8 | Distancia de sombras y sesgo de LOD por gama | S | Bajo | Menos casters por cascada; 10-20 % menos triángulos | Las sombras acaban antes | Sí |
| F2-9 | LODs en mundos grandes: lote estático, umbral ~1.000, caché de LODs | S+S+M | Bajo-medio | 1,5 M triángulos menos en Keryan; 4-8 s menos de carga | Ninguno | Sí |
| F2-10 | Variantes que comparten la geometría | M | Medio | 300-400 MB menos de RAM; ~700 MB menos de pico de VRAM | Ninguno | No |
| F2-11 | CPU de mundos grandes, segunda tanda | 5×M | Medio | 12-20 ms menos en 4 núcleos (Keryan) | Ninguno | No |
| F2-12 | Ajustes del editor separados del juego; preset de primera ejecución; menú de opciones integrado | M | Medio | El jugador puede bajar la calidad en juegos sin menú | Ninguno | Sí |
| F2-13 | Gama de CPU y palancas de CPU | M | Medio | 3-10 ms en escenas pesadas | Animación lejana más tosca | Sí |
| F3-1 | Resolución dinámica real, sin recrear destinos | L | Alto | Sin tirones por paso; escala continua | Ninguno | No |
| F3-2 | Sol Mixto con shadowmask horneado | L | Medio-alto | 80-95 % menos de cascadas con la luz por defecto | Sombras estáticas más suaves | Sí (rehornear) |
| F3-3 | LOD por clúster / HLOD para piezas gigantes | L | Medio | 30-50 % menos triángulos en interiores enormes | Posible pop | No |
| F3-4 | LODs para personajes con esqueleto | M | Medio | 0,5-3 ms con multitudes | Deformación sutil | No |
| F3-5 | G-buffer más delgado | L | Medio | 1-2 ms en UHD 620 y Vega 8 | Posible banding | No |
| F3-6 | Camino de vértices rígido sin mezcla de matrices (por verificar) | M | Medio | Por medir | Ninguno | No |

**Núcleo de la Fase 1, que debe salir junto:** F1-1, F1-2, F1-3, F1-4, F1-5 y F1-7. Si F1-1 sale sin el tope de objetivo de F1-2, Night, Reinos y WhatIs bajan al mínimo en cualquier PC. Si sale sin F1-3, el DOF/MB sigue mandando. Lo demás de la Fase 1 puede pasar a la versión siguiente si no da tiempo.

**Orden de trabajo:**
1. F1-5, para medir la línea base.
2. F1-1 con sus tests.
3. F1-2 y F1-7, en el jugador.
4. F1-3 y F1-4, en el gobernador y el shader.
5. F1-6 y F1-8.
6. F1-9, F1-10 y F1-11.
7. F1-12 y F1-13.

### Fase 1 — detalle

**F1-1. Arreglar la «media 0» y alimentar el presupuesto con muestras frescas** (S, riesgo bajo)

Qué cambiar en `CramionFX/src/vk/FrameBudget.cpp`, en `update()` (303-356):
- **(a)** Justo después de `clock_ += dt`, añadir `if (!(gpu_ms > 0.0f) || !std::isfinite(gpu_ms)) return;`, antes de mezclar los pases y de sembrar la media.
- **(b)** Cambiar el filtro absoluto de la línea 354 por uno de picos sostenidos:
  - Una muestra mayor que max(3 × media, media + 20 ms) se ignora mientras dure menos de unos 0,75 s (miembro nuevo `spike_seconds_`).
  - Si dura más, es la nueva normalidad y se vuelve a sembrar `ema_ms_ = gpu_ms`.
- **(c)** Calentamiento: tras sembrar, no bajar ni subir nada hasta pasados 0,5 s y 4 muestras.
- **(d)** Medir cada cambio con al menos 6 muestras, además de `settleSeconds`.

En `VulkanRenderer.cpp:4597-4625`:
- Llamar a `budget_.update` solo si `profiled` (o si `!gpu_profiler_.supported()`), con el `dt` acumulado desde la última muestra entregada.
- Pasar los valores crudos `lastTotalMilliseconds()` y `lastTimings()`, que ya existen (`GpuProfiler.h:48/52`). `totalMilliseconds()` sigue para el log y el editor.
- Saltar las vistas de Render Texture (`render_texture_target_ >= 0`) y las secundarias del editor, y sumar su GPU a la siguiente muestra de la vista principal.

En `VulkanRenderer.h:334-339` (`setPostProcess`):
- Reiniciar el presupuesto solo si se enciende un efecto o si `lod_pixel_error` baja más de un 25 %.
- Como mucho, un reinicio cada 10 s.
- `setGraphicsSettings` y `resetAdaptiveBudget` siguen reiniciando al momento.

Ganancia:
- El gobernador empieza a funcionar en las GPUs lentas. Conserva el arranque Bajo (67 %, iluminación ligera, volumétrica a 16 pasos, cascadas espaciadas) y sigue bajando: resolución al 50 % y sin SSAO, SSR ni GI.
- Escena tipo BlackHouse en un juego con el presupuesto activo, en GTX 1050 o Iris Xe: de ~125 ms a ~55-65 ms. El resto lo frena el DOF/MB (F1-3).
- Con muestras crudas aprende entre el 55 y el 80 % del ahorro real a 10-60 FPS (hoy entre el 1 y el 37 %), así que oscila mucho menos.

Coste visual: ninguno por sí mismo.

Riesgo: bajo.
- Los tests actuales no envían 0, así que siguen valiendo.
- La ventana de persistencia es lo que impide que una carga se tome como coste.

Prueba, en `CramionCore/tests/MeshTests.cpp`, con un `testFrameBudgetStartup()` nuevo llamado desde `main` (996):
- `setStartLevels(Low)`, objetivo 60, `update(1/60, 0, {})` y luego 20 s de `update(1/15, 70, pases)`. Debe dar `smoothedGpuMs() > 50`, `level(RenderScale) >= 3` y una suma de niveles no menor que la de salida. Hoy falla.
- 2 s a 3 ms y luego 2 s a 30 ms: la media debe pasar de 25.
- 10 ms estables con un frame de 250 ms: la media debe quedar por debajo de 12.
- En el juego, «GPU X ms (media Y» con Y ≈ X antes de 5 s.

Notas de versión: sí. «El presupuesto adaptativo vuelve a funcionar en PCs lentos (antes se quedaba en "media 0" y subía la calidad al máximo).»

**F1-2. Juego exportado: presupuesto siempre activo, objetivo sensato y ajustes del proyecto como topes** (S-M, riesgo medio-bajo)

Función pura nueva, comprobable en un test, junto a `tierFor` (`FrameBudget.h/.cpp`): `shippedGraphics(GraphicsSettings proyecto, HardwareTier, float hz_pantalla, bool bloqueado)`. Reglas:
- `adaptive = true`, salvo con el bloqueo.
- `target_fps` limitado a entre 30 y el mínimo de (Hz de la pantalla, 144). Si no se conocen los Hz, 60.
- `shadow_resolution`, `texture_max_size` y `texture_filtering`: si el proyecto trae un valor, pasa a ser tope: `min(valor, …For(tier))`.
  - BlackHouse se queda en 1024.
  - test pasa a 1536 y 2048 en Bajo.
- `texture_lod_bias` no baja de 0 en Bajo y Medio.

En `PlayerMain.cpp:427-428`:
- Aplicarla entre el ini del proyecto y el del jugador.
- Llamar a `setRayTracingEnabled(false)` por debajo de Alto.
- Llamar a `setReflectionProbeEnabled(false)` en Bajo, como ya hace el preset «Baja», hasta F2-5.
- Leer los Hz con `MonitorFromWindow` y `EnumDisplaySettings`.

En la configuración de compilación:
- Casilla nueva «Respetar los ajustes gráficos del proyecto (no adaptar a cada PC)», que escribe `graphics_lock=1` en game.ini (`BuildConfig.h:74-92`, `BuildConfig.cpp:157-180`).

En el ini del jugador:
- `saveGraphicsIni` (`GraphicsConfig.cpp:172-196`) escribe `version=2`.
- Solo se respeta `adaptive=0` en archivos con `version=2`. `Graphics.save()` guarda también lo heredado del proyecto, así que en los archivos viejos ese valor puede no ser una elección del jugador.

En el editor:
- Los tooltips de «Optimización adaptativa» y «Objetivo» deben explicar que el juego exportado se adapta a cada PC, salvo con la casilla de bloqueo.

Ganancia:
- BlackHouse, PortalLab, STREET KINGS y test pasan a tener el gobernador encendido.
- Night, Reinos y WhatIs dejan de perseguir 240 FPS. WhatIs acabó a 960x540 con todo apagado en la 4060 Ti (WhatIs.log:76).
- Las GPUs de 2-4 GB y las integradas dejan de paginar VRAM por 576 MiB de cascadas (más otro tanto de caché) y texturas de 16K.

Coste visual:
- En PCs Bajo y Medio: sombras y texturas de su gama, sin RT, y sin sonda en Bajo.
- En Alto y Ultra: igual que ahora, salvo el tope del objetivo.

Riesgo: medio-bajo. Cambia lo que entrega el desarrollador; hay que documentarlo y ofrecer el bloqueo.

Prueba:
- `testShippedGraphics()`: {0, 240, 6144, 16384, 16, −1} con Bajo, 60 Hz y sin bloqueo debe dar {1, 60, 1536, 2048, 4, 0}. Con bloqueo, `adaptive` sigue en 0.
- Re-exportar BlackHouse y test: la línea de rendimiento imprime «palancas:».
- test con `CRAMION_HARDWARE_TIER=low`: «sombras 1536».

Notas de versión: sí. Hay que re-exportar los juegos.

**F1-3. Palanca «Desenfoque de cámara» (DOF + motion blur)** (S-M, riesgo bajo-medio)

Qué cambiar:
- **Palanca.** Añadir `Lever::CameraFx` después de `RenderScale` (`FrameBudget.h:57-68`), para no mover los índices del log. `kMaxLevel` 2.
- **Gobernador:**
  - Leer «Motion blur + profundidad de campo» en `update()` (309-321).
  - Ahorro: 0,5 de su coste para el primer paso y 0,9 del resto para el segundo.
  - Penalización: 1,8 y 3,0, por debajo de la GI (4).
  - En la línea 203, sacar de `pixel_ms` lo que va después del escalado: desenfoque, Bloom, Rayos de luz, el escalador y Composición.
- **apply():**
  - Nivel 1: 32 muestras de DOF y 12 de motion blur.
  - Nivel 2: sin DOF ni motion blur.
  - Las muestras van como campos internos de `PostProcessSettings`, igual que `volumetric_steps`, que tampoco se guarda en la escena (`Components.cpp:325` solo visita los campos listados).
- **Renderizador:** pasar las muestras en `GpuCameraFxPush::projection_w.zw`, que hoy valen 0. El struct ya ocupa exactamente 128 B, el mínimo garantizado para las push constants: no se le pueden añadir campos.
- **Shader:** en `camera_fx.frag:79/159`, bucles con el tope fijo actual y `break` al llegar a las muestras pedidas.

Ganancia:
- Es el pase más caro de los logs y no baja con la resolución.
- En gama baja es ~40-50 ms del frame.
- Con F1-1 y F1-2, una escena tipo BlackHouse queda en ~18-30 ms, unos 35-55 FPS limitados por GPU.

Coste visual: solo si no llega al objetivo. Primero un bokeh con más ruido; después, sin DOF ni motion blur.

Riesgo: bajo-medio. Hay que revisar el DOF a 32 muestras con TAA.

Prueba:
- Ampliar `testFrameBudget` (`MeshTests.cpp:800-948`) con un pase de 6 ms × nivel. En un caso tipo WhatIs, CameraFx debe bajar antes que la GI y el objetivo alcanzarse en menos de 10 s.
- `apply()` nivel 1 → 32/12.
- Una escena sin DOF ni motion blur nunca baja CameraFx.

Notas de versión: sí.

**F1-4. Arranque Bajo y Medio que cubra los pases caros** (S, riesgo bajo)

Qué cambiar:
- En `setStartLevels` (`FrameBudget.cpp:122-140`):

  | Palanca | Bajo | Medio |
  |---|---|---|
  | Lod | 1 | 1 |
  | ShadowDetail | 2 | 1 |
  | Volumetric | 1 | 1 |
  | ContactShadows | 1 | — |
  | Lighting | 1 | — |
  | Ssao | 1 | — |
  | Reflections | 1 | — |
  | CameraFx | 2 | 1 |
  | RenderScale | 3 (0 si el preset ya escala; F1-7) | — |
  | Clouds | 1 (con F1-9) | — |

- `skyMapWanted()` (`VulkanRendererSkyMap.cpp:63-67`) debe devolver false con `liteLighting()`. Hoy el mapa se dibuja aunque `lighting.frag:523` no lo lea en modo ligero; solo pasa en el camino sin RT, que es el de gama baja.

Ganancia: los primeros segundos en un PC Bajo cuestan un 40-50 % menos que con el arranque Bajo de hoy en una escena con SSAO, SSR y DOF/MB. Estimado sobre los pases de BlackHouse.

Coste visual: el arranque es más sobrio, pero lo que sobre lo devuelve el gobernador.

Riesgo: bajo.

Prueba:
- Test de los niveles de salida.
- Con `CRAMION_HARDWARE_TIER=low`, la primera línea de «palancas» debe mostrar esos niveles.

Notas de versión: sí.

**F1-5. Diagnóstico útil en el log del jugador** (S, riesgo nulo)

Qué cambiar:
- **Línea `[Rendimiento]`** (`PlayerMain.cpp:1598-1624`): añadir al final la gama, el escalador activo, los ms de CPU del hilo principal (frame menos espera de GPU, como `ProfilerOverlay.cpp:41-49`) y las 3-5 zonas más caras. No tocar los campos que ya existen.
- **Frames de carga:** excluirlos de `prof::summary(300)` (1621), marcando el momento de «Escena lista» (1193).
- **Marcas de GPU:**
  - «Preparacion (compute)» antes de las sombras (`VulkanRenderer.cpp:5354`).
  - «Vidrio» separado de «Agua» (5392).
  - El mapa de cielo fuera de «Geometria» (5374).
  - Mapearlas en `FrameBudget::update`; hoy el gobernador baja el detalle de sombras por culpa del compute.
- **Zonas de CPU:** `CR_PROFILE_SCOPE` en los subpasos de `RenderSync::sync` (`RenderSync.cpp:2183-2204`).
- **Tirones en el juego:** umbral relativo, mayor que max(50 ms, 3 × p50), y escritura fuera del mutex (`Profiler.cpp:256-276`).

Ganancia:
- Quita una escritura síncrona de captura cada 10 s en PCs cuyos frames pasan siempre de 50 ms. Estimado en 10-100 ms cada vez.
- Convierte las estimaciones de este plan en medidas del PC del jugador.

Coste visual: ninguno.

Riesgo: nulo.

Prueba:
- `CramionOptimizationTests`: el resumen ignora la carga, y un tirón forzado no bloquea `endFrame` más de 1 ms.
- En Keryan, las zonas nuevas suman lo que hoy es tiempo propio de RenderSync, con 0,5 ms de margen.

Notas de versión: sí, para desarrolladores (log y capturas).

**F1-6. GI horneada sin el filtro SVGF** (S, riesgo bajo)

Qué cambiar:
- En `recordSsgiPass` (`VulkanRenderer.cpp:6545-6632`), con GI horneada y sin sondas dinámicas:
  - Después de `baked_gi_pass_` (6558), copiar `gi_raw_` a `gi_image_`, o enlazar `gi_raw_` en la iluminación.
  - Salir antes del temporal, las 5 pasadas à-trous y la copia de momentos.
  - Poner `gi_filter_history_valid_ = false`.
  - No he verificado que `gi_raw_` y `gi_image_` tengan el mismo formato y tamaño.
- Con GI horneada, `budget_.setLevelCap(Lever::Gi, 0)`. Hoy esa palanca no hace nada en ese modo (6452) y el gobernador malgastaría un paso.

Ganancia:
- El filtro es ~85-90 % del pase (estimado por número de lecturas).
- La GI baja de ~3 a 0,3-0,5 ms en la 4060 Ti, que en una GTX 1050 a resolución nativa es de ~20 a 2-3 ms.
- Es lo que hace rentable «hornear para gama baja» ahora que la 2.4.13 arregla el horneado.

Coste visual: ninguno esperado.

Riesgo: bajo. Si aparecen costuras, se deja 1 pasada à-trous.

Prueba:
- En una escena horneada exportada, «GI» baja al menos un 70 %.
- Captura antes y después sin diferencias visibles.

Notas de versión: no.

**F1-7. Resolución interna sin doble reducción, con suelo y con menos saltos** (S, riesgo bajo)

Qué cambiar, en `applyEffectiveGraphics` (`VulkanRenderer.cpp:3261-3269`):
- Si la escala del preset ya es menor que 0,9, la palanca RenderScale arranca en 0, como en el móvil (`VulkanRenderer.h:351`).
- Suelo: el alto interno no baja de max(540, 0,5 × la salida), salvo si el frame lleva más de 3 s por encima de 1,5 veces el objetivo.
- Como mucho un paso de RenderScale cada 3 s. Cada paso cuesta dos `waitIdle`, rehacer los destinos y los samplers, y un flash de TAA (C5).

Ganancia: sobre todo nitidez y menos tirones. La GPU trabaja un 10-20 % más que con el 0,45 de hoy, y eso lo pagan F1-3 y F1-4.

Coste visual: mejora.

Riesgo: bajo.

Prueba:
- Test: Bajo con un preset de 0,67 da una escala de 0,58 o más al arrancar.
- Keryan con `CRAMION_HARDWARE_TIER=low`: «1280x673 -> 1920x1009».

Notas de versión: sí.

**F1-8. Sol horneado sin cascadas ni sombras por rayos cuando nadie las lee** (S, riesgo bajo-medio)

Qué cambiar:
- Función nueva `sunCascadesNeeded()`:

  ```
  sunShadows() && !(bakedSunActive(dir) && !post_.volumetric_light
                    && water_pass_.empty() && !fluid_pass_.active()
                    && no hay vidrio)
  ```

- Usarla en la programación de cascadas (4343-4350, que ya tiene el camino `kept_clear`), en `recordShadowPass` (5539) y en las sombras del sol por rayos (4077).
- Añadir un consejo en la ventana de iluminación y corregir `iluminacion-horneada.html:53`.

Ganancia:
- Desaparecen las cascadas en las escenas con el sol horneado. Cuestan 1,0-2,8 ms en la 4060 Ti, unos 7-20 ms en gama baja, lo que encaja con los ~9 ms del análisis.
- La CPU se ahorra grabar 300-800 llamadas de sombra.

Coste visual: ninguno en superficies opacas. Los objetos dinámicos ya no tienen sombra del sol cuando el sol está horneado.

Riesgo: bajo-medio. Si se olvida a algún lector de las cascadas, habrá fugas de luz en el agua o el vidrio.

Prueba:
- En una escena con el sol horneado, el log dice «sombras N (0 llamadas)».
- Al añadir un Water, las cascadas vuelven.

Notas de versión: sí.

**F1-9. Palancas más hondas: volumétrica y nubes** (S-M, riesgo bajo-medio)

Qué cambiar:
- **Volumétrica**, en 3 niveles:
  - 1: 16 pasos y sin rayos de luz (como hoy).
  - 2: 8 pasos. `volumetric.frag:422` ya admite de 8 a 32.
  - 3: sin luces locales en la volumétrica, solo el sol.
- **Nubes**, palanca nueva `Lever::Clouds`:
  - 1: 64 pasos y 4 de luz.
  - 2: 32 pasos y 2 de luz, con la sombra de nubes cada 4 frames.
  - Los pasos van en `GpuCloudPush::layer.w`, que hoy vale 0.
  - No dibujar nubes con cobertura 0 (9017).
- Un tope de nubes propio del renderizador, que la escena no pueda pisar. Eso arregla el móvil (`PlayerMain.cpp:638` frente a `RenderSync.cpp:3568`).
- Leer «Nubes» y «Agua» en `update()`.

Ganancia:
- Nubes: 2,9 ms en test, unos 20 ms en una GTX 1050, que bajan a 6-9 ms.
- Volumétrica: 3,8-4,7 ms en PortalLab, unos 27-33 ms en gama baja, que bajan a 4-6 ms.

Coste visual: nubes más suaves, niebla más fina y, en el nivel 3, farolas sin halo.

Riesgo: bajo-medio.

Prueba:
- `testFrameBudget` con los pases «Nubes» y «Volumetrica».
- test y PortalLab con objetivo 240: las palancas se mueven y los pases bajan.

Notas de versión: sí. En Android, las nubes de Baja y Media se apagan de verdad.

**F1-10. LODs en las variantes `.crmat`** (S, riesgo bajo-medio)

Qué cambiar:
- En `buildVariant` (`RenderSync.cpp:717-721`), copiar `lod_indices`, `lods` y `lightmap_uvs`. Los LODs son índices sobre los mismos vértices, así que el tiling (722-734) sigue valiendo.
- Obligatorio en el mismo cambio: `drawCpuActors` (`VulkanRenderer.cpp:6032`) debe leer `model.lodSubmeshes(draw.lod)[i]`. Hoy ya falla en modo compatible con los modelos que tienen LOD y no llevan `.crmat`.
- En las sombras con un LOD distinto al de la cámara (2127-2137), probar la caja de cada submalla.

Ganancia:
- En Keryan, ~55 % de los triángulos activos pasan a tener LOD: 40-50 % menos triángulos enviados al mirar la ciudad (inv.).
- La palanca Lod empieza a ahorrar de verdad.
- En escenas pequeñas apenas cambia nada.

Coste visual: algún cambio de LOD visible a lo lejos.

Riesgo: bajo-medio. Cada variante ocupa más RAM con sus índices hasta F2-10.

Prueba:
- Test: base de 4.096 triángulos o más → `generateLods` → `buildVariant` con `.crmat` da los mismos LODs.
- Keryan: «N con LOD» en cientos, y sin el gemelo «en 1 LODs».
- Con `CRAMION_VK_COMPAT=1`, no faltan trozos a distancia.

Notas de versión: sí.

**F1-11. Objetos diminutos: umbral por gama con histéresis** (S, riesgo bajo)

Qué cambiar:
- Umbral base: Bajo 1,5 px, Medio 1, Alto 0,75 y Ultra 0,5.
- En `VulkanRenderer.cpp:1622`, usar max(base, `cullPixels()`).
- Histéresis por actor: se oculta por debajo de t y vuelve por encima de 1,25 t.

Ganancia: desde el spawn de Keryan, con 1 px a escala 0,67, ~880 instancias ocultas (23 %, 1,66 M triángulos). Hoy son 16-35 (inv.).

Coste visual: lo que mide menos de 3-4 px desaparece a lo lejos.

Riesgo: bajo.

Prueba: Keryan con Bajo da «N diminutos» en cientos, sin parpadeos al acercarse.

Notas de versión: sí.

**F1-12. CPU: ríos, vegetación, ondas y flotación sin trabajo repetido** (S-M, riesgo bajo)

Qué cambiar:
- **Ríos:**
  - Guardar la línea central junto al hash de la cinta y pasarla a `sampleWater(…, line)` (`Water.h:117`) en `RenderSync.cpp:1896` y `2106`.
  - Descartar por AABB antes de muestrear.
  - Saltar el océano en la prueba de cámara bajo el agua si la cámara está muy por encima.
  - Guardar las cintas en un `shared_ptr<const>`. Hoy se copian ~8 MB por frame.
- **syncFoliage** (2433-2461): rehacer la lista de agua solo si cambia su hash, y compartirla entre bosques. Hoy se copian ~7 MB por frame.
- **updateRipples** (2005-2160): salir enseguida si no hay agua a unos 30 m, y usar contenedores miembro en vez de crearlos en cada frame.
- **Flotación** (`PhysicsSystem.cpp:3105`): guardar las líneas en caché y no hacer nada si no hay cuerpos dinámicos.

Ganancia: Keryan, 3-7 ms menos en el PC del autor (inv., de los 7,4-11,2 ms sin zona). Son 8-18 ms en un portátil de 4 núcleos (hipótesis ×2,5).

Coste visual: ninguno.

Riesgo: bajo.

Prueba:
- `CramionWaterTests`: el muestreo con y sin la línea en caché da lo mismo en 1.000 puntos.
- En Keryan, las zonas «Agua», «Ondas» y «Vegetacion» (F1-5) bajan de 0,3 ms.

Notas de versión: no.

**F1-13. Render Textures a un ritmo según la gama** (S, riesgo bajo-medio)

Qué cambiar:
- Cvar `render.RenderTextureInterval`: 1 en Alto y Ultra, 2 en Medio, 3 en Bajo. Escalonado por cámara (`RenderSync.cpp:2317-2339`).
- El presupuesto suma el GPU de esas vistas (F1-1).

Ganancia: PortalLab en Bajo pasa de 3 a ~1,7 vistas dibujadas por frame, un 40-45 % menos de GPU.

Coste visual: portales a 20 Hz en Bajo y 30 Hz en Medio.

Riesgo: bajo-medio.

Prueba:
- FPS de PortalLab antes y después con Bajo.
- Test del intervalo.

Notas de versión: sí.

### Fase 2 — detalle

**F2-1. Aprendizaje por tiempo y niveles guardados por PC** (M)
- Qué cambiar:
  - `GpuProfiler::collect(frame, dt)` con un alfa de 1 − e^(−dt/0,5 s) (`GpuProfiler.cpp:12/120/123`).
  - Ahorro aprendido = media de antes − media de las muestras crudas en una ventana tras asentarse (`FrameBudget.cpp:362-369`).
  - Guardar `levels_` y `learned_` por GPU y driver en `Saves/<juego>.budget.ini`.
  - Opcional: si todo está al mínimo y el frame lleva 10 s por encima de 1,25 veces el objetivo, pasar a 30 FPS con vsync cada 2.
- Prueba: a 12 FPS con 2 frames de latencia, el ahorro aprendido de la GI queda entre 7 y 13 ms, con como mucho 1 cambio en los últimos 20 s.

**F2-2. Gama por rendimiento real y RT solo con hardware** (M)
- Qué cambiar:
  - Además de la VRAM (`FrameBudget.cpp:31-36`), un micro-benchmark de 20-40 ms con timestamps, o una tabla por vendor y device ID.
  - `CRAMION_HARDWARE_TIER` sigue mandando.
  - RT solo en Alto y Ultra, y con RT por hardware.
- Prueba: tabla de casos de `tierFor`.

**F2-3. Nubes solo donde se ve cielo y calidad Baja** (M)
- Qué cambiar:
  - Grabar las nubes después del G-buffer y salir en los píxeles cuya huella de profundidad está tapada entera. La iluminación solo lee nubes donde la profundidad es 1 o más (`lighting.frag:1385-1392`).
  - Las capturas de sonda siguen recorriendo todo.
  - Bajo: 48 pasos y 3 de luz a un cuarto de resolución.
  - Sombra de nubes cada 4 frames.
  - Valorar no poner nubes cuando la escena no tiene Sky.
- Prueba: en un interior, «Nubes» por debajo de 0,05 ms; diferencia de 1/255 o menos.

**F2-4. DOF y motion blur por tiles** (M)
- Qué cambiar:
  - Tiles de 16×16 con el CoC y la velocidad máximos.
  - Muestras según el tile, con salida temprana por debajo de 0,5 px.
  - A media resolución en Bajo y Medio.
  - Ping-pong en lugar de las 2 copias completas (9058-9086).
  - Sin motion blur con la cámara y los actores quietos.
- Prueba: añadir `--dof` y `--motion-blur` a RenderBench. Debe tardar como mucho el 25 % de hoy, con un SSIM de 0,98 o más.

**F2-5. Sonda de reflexión a 256×256** (M)
- Qué cambiar:
  - Hoy cada cara se dibuja a la resolución interna completa (5147) y espera una fence en la CPU (5231).
  - Pasar a caras de 256×256, sin SSGI, SSAO, SSR ni volumétrica, grabadas en el frame, y sin invalidar las cascadas.
  - Con esto vuelve la sonda en Bajo.
- Prueba: con `ray_tracing=0` y Bajo, p99 de 1,3 veces el p50 o menos.

**F2-6. SSGI y SVGF en calidad Baja** (M)
- Qué cambiar:
  - Rayos y pasos de `ssgi.frag:59-60` por constante de especialización: 3×6 en Bajo.
  - 2 pasadas à-trous en Bajo.
  - Varianza en R16F.
  - Guía de profundidad y normal a media resolución.
- Prueba: «GI» al 35 % o menos con `--tier low --freeze-budget`.

**F2-7. Iluminación y agua en calidad Baja** (S-M)
- Qué cambiar:
  - `probeDirection` de 24 a 8 pasos.
  - Blur de SSAO sin las 16 lecturas de normal.
  - Sombras de contacto de 48 a 16 pasos en Medio.
  - Tope de luces locales en Bajo: 12 puntuales y 4 focos por importancia, con fundido.
  - SSR del agua de 32+6 a 12+3 pasos.
  - Reutilizar la copia de la escena entre el vidrio y el agua.
- Prueba: «Iluminacion» al 65 % o menos y «Agua» al 60 % o menos; SSIM de 0,96 o más.

**F2-8. Distancia de sombras y sesgo de LOD por gama** (S)
- Qué cambiar:
  - Llamar a `setShadowDistance`: 60 m en Bajo y 80 m en Medio.
  - `lodScaleFor(tier)` de 2 en Bajo y 1,5 en Medio, aplicado al componer `post_` (4626).
  - Quitar el Lod 1 del arranque para no contarlo dos veces.
- Prueba: `lod_triangles` (MCP) en la misma vista con Bajo y con Alto.

**F2-9. LODs en mundos grandes** (S+S+M)
- Qué cambiar:
  - **(a)** Las piezas de 16.384 triángulos o más se quedan fuera del lote estático (`StaticBatching.cpp:378-394`). En Keryan, 17 objetos son el 83 % del lote (inv.).
  - **(b)** Umbral de LOD de unos 1.000 triángulos (`MeshLod.cpp:21`) cuando exista la caché.
  - **(c)** Caché de LODs en `Library/Cache/Lods`, que se genera al abrir el proyecto y se copia al paquete.
  - **(d)** La física usa `readModelGeometry`, sin LODs ni texturas.
- Ganancia de carga en el PC del autor: 1,3 s, 2,9 s y 3,4 s menos (inv.).

**F2-10. Variantes que comparten la geometría** (M)
- Qué cambiar:
  - Vértices, índices y LODs en un `shared_ptr<const>`.
  - No subir bases que solo usan variantes.
  - El streaming expulsa esos modelos en vez de volver a subirlos.
- Ganancia: unos 410 MB menos de RAM en Keryan, y el pico de buffers de VRAM de test baja de 1.105 a unos 380 MB (test.log:103-108).

**F2-11. CPU de mundos grandes, segunda tanda** (M cada uno; inv.)
- `activeInHierarchy` en O(1) y recorridos solo de lo activo.
- Navegación con `runtime_generation` incremental (0,9-1,4 ms).
- Sincronización de física incremental, y `max_substeps` 2 con CPU baja.
- Scripts C++ con una petición IPC por fase (0,9-1,3 ms).
- `syncActors` con lista persistente.
- Clústeres estáticos persistentes (14.196 en Keryan).

**F2-12. Ajustes del editor separados de los del juego** (M)
- Los ajustes del visor del editor van a un archivo propio, y la configuración de compilación tiene una sección «Calidad del juego». Esto cambia archivos del proyecto: hay que pedir permiso y migrar con copia.
- Primera ejecución: un preset por gama, guardado en el ini del jugador.
- Menú de opciones integrado opcional (F10; ImGui ya está, `PlayerMain.cpp:463`) que guarde `quality=` y los efectos.
- `RenderBench --preset` debe usar `applyQualityPreset`.

**F2-13. Gama de CPU y palancas de CPU** (M)
- Una rama del gobernador con el tiempo de frame menos la espera de GPU. Hoy Keryan, a 15 ms de frame con 9 de GPU, no activa nada.
- Palancas: distancia de animación, subpasos de física, ritmo de Render Textures y de navegación, ondas.

### Fase 3 — detalle

**F3-1. Resolución dinámica real** (L, riesgo alto)
- Destinos al tamaño máximo creados una sola vez, y dibujo con viewport y scissor a salida × escala.
- `uv_scale` en los pases a pantalla completa. TAA y FSR 1 leen el sub-rectángulo, FSR 3 recibe `renderSize` en cada llamada y DLSS usa `InRenderSubrectDimensions`.
- La escala pasa a ser continua (de 0,5 a 1) con un controlador PI.
- Paso intermedio a valorar: destrucción diferida de los destinos viejos, sin `waitIdle`.
- Prueba: `recreateRenderTargets` no se llama ni una vez al mover la escala 1 → 0,6 → 1.

**F3-2. Sol Mixto con shadowmask horneado** (L)
- Visibilidad del sol horneada por texel y por sonda (nueva versión de `.crbake`).
- Las cascadas solo dibujan lo animado (el camino existe en 5685-5687).
- Hay que rehornear.

**F3-3. LOD por clúster / HLOD** (L)
- Para la casa de test: 2,99 M triángulos con la cámara dentro de una pieza de 12,8 m de radio.

**F3-4. LODs para personajes con esqueleto** (M)
- `generateLods` acepta huesos, y se elige `draw.lod` también en la rama no rígida.

**F3-5. G-buffer más delgado** (L)
- De 32 a 20-24 B por píxel, para las integradas.

**F3-6. Camino de vértices rígido** (M; dato de la investigación sin verificar)
- `skinned.vert` mezclaría 8 matrices por vértice incluso en modelos rígidos.

## Riesgos y qué no tocar

**Proyectos del usuario:**
- No modificar sin permiso `ProjectSettings/Graphics.ini`, las escenas, los `.crmat` ni `Navigation.json`.
- La política del juego exportado (F1-2) se aplica al arrancar el jugador y no reescribe nada.
- Separar los ajustes del editor (F2-12) cambia archivos del proyecto: necesita consentimiento y una migración con copia.

**Ini viejo del jugador:** no respetarlo a ciegas, porque `Graphics.save()` guarda también lo heredado del proyecto. De ahí el `version=2`.

**Filtro de tirones:** no quitarlo. Las cargas no deben aprenderse como coste.

**Push constants:** no ampliarlas.
- `GpuCameraFxPush` ya ocupa 128 B, el mínimo garantizado (muchas integradas no admiten más): usar `projection_w.zw`.
- En las nubes, usar `layer.w`.

**VR y móvil:** no tocar la VR (escala por ojo, tope de VRAM, TAA por ojo) ni el perfil móvil, salvo el arreglo de las nubes.

**Nombres que leen otros:** no cambiar el orden ni los nombres de las palancas y pases existentes, ni los campos actuales de `[Rendimiento]`, que leen el editor, el MCP y las personas. Lo nuevo se añade al final.

**Reinicios del presupuesto:** al limitarlos (F1-1), un efecto que el usuario enciende a mano debe seguir viéndose al momento.

**Modo cine del editor:** sigue sin presupuesto. Ojo: hoy, en un PC Bajo, conserva los niveles de salida.

**F1-10:** el arreglo de `drawCpuActors` va en el mismo cambio que las variantes con LOD. Sin él, Android y el modo compatible pierden trozos.

**F1-8:** no saltar las cascadas si las leen el agua, el vidrio, la niebla volumétrica o los fluidos.

**Resultado esperado tras F1-1:** en PCs medios y altos, algunos juegos con objetivo 60 que hoy van a 17-18 ms (test) empezarán a bajar algo de calidad. Es lo correcto, pero hay que decirlo en las notas de versión, junto con la casilla de bloqueo.

**Estilo de código:**
- Comentarios en español sin tildes.
- Textos nuevos de la interfaz y notas de versión en español con tildes, por ejemplo «Respetar los ajustes gráficos del proyecto» o «Desenfoque de cámara». Los nombres de palanca actuales no llevan tildes; hay que decidir si se corrigen a la vez.

**Compilación y publicación:**
- Preguntar antes de compilar.
- Una sola compilación a la vez, con `-j 2`: primero los objetivos de Windows (`CramionEditor` y el jugador), y Android aparte.
- Tests de uno en uno.
- Sin commits ni publicación sin pedirlo.

**Re-exportar:** para que los jugadores reciban los arreglos hay que volver a exportar los juegos, porque el `CramionPlayer` va dentro de cada juego.

## Cómo medirlo (antes/después)

**1. Línea base, antes de tocar nada.** En el PC del autor, 60 s con el mismo recorrido de cámara en estos juegos:
- BlackHouse: interior con DOF y motion blur.
- test: océano, nubes y agua.
- PortalLab: luces, volumétrica y Render Textures.
- XR: camino sin RT.
- Keryan: CPU y LODs.

Para cada uno, estas variantes:

| Variante | Qué se ve |
|---|---|
| a. Tal cual | Lo que juega hoy la gente en un PC como el del autor |
| b. `CRAMION_HARDWARE_TIER=low` | Arranque y tamaños de la gama Baja |
| c. b + `CRAMION_NO_MESH=1` | Camino de vértices, sin mesh shaders, como las GPUs de gama baja |
| d. c + `ray_tracing=0` en `%LOCALAPPDATA%\Cramion\Saves\<Juego>.graphics.ini` | Camino sin RT: sonda, SSGI y mapa de cielo (borrar el archivo después) |
| e. `start "" /affinity F <Juego>.exe` + `CRAMION_JOB_THREADS=2` | CPU de 4 hilos |

De cada línea `[Rendimiento]`, apuntar: FPS, GPU, media, palancas, resolución interna, pases de 0,3 ms o más, sombras y llamadas, VRAM, p50, p95, p99 y tirones.

**2. Ver bajar al gobernador en un PC rápido.**
- `CRAMION_HARDWARE_TIER=low` en un PC rápido solo cambia el arranque: el gobernador volverá a subir.
- Para verlo bajar, poner `target_fps=240` en el ini del jugador, o bajar los relojes de la GPU con `nvidia-smi -lgc 300,800` (como administrador; restaurar con `nvidia-smi -rgc`). No he comprobado que el driver de GeForce lo permita.

**3. Tests unitarios**, de uno en uno, en `CramionMeshTests`:
- `testFrameBudgetStartup`. Hoy debe fallar.
- Picos y saltos sostenidos.
- `testShippedGraphics`.
- CameraFx, nubes y volumétrica en `testFrameBudget`.
- Niveles de salida Bajo y Medio.
- Escala con suelo.

Más los tests de variantes con LOD (`CramionMeshTests` o `CramionMaterialTests`), la línea en caché en `CramionWaterTests`, y el resumen y los tirones en `CramionOptimizationTests`.

**4. Criterios de aceptación de la Fase 1,** comparando con la línea base:
- En los 5 juegos, la media queda a menos de un 10 % de «GPU» a los 5 s. Hoy BlackHouse da 0 frente a 18.
- «palancas:» aparece en todos los juegos sin bloqueo. Hoy falta en BlackHouse, test, PortalLab y STREET KINGS.
- Con `target_fps=240`, Bajo y `CRAMION_NO_MESH=1`:
  - BlackHouse, con las palancas al mínimo, queda en 4-5 ms o menos en la 4060 Ti. WhatIs se quedaba hoy en 8,3 ms con DOF/MB.
  - Se baja CameraFx antes que la GI.
  - Como mucho 1 cambio de palanca en los últimos 20 s.
- test con Bajo: «sombras 1536» y al menos 500 MB menos de VRAM.
- Escena horneada: «GI» al menos un 70 % más baja. Con el sol horneado: «(0 llamadas)».
- Keryan: «con LOD» y «diminutos» en cientos; zonas de RenderSync visibles; CPU ms en el log; resolución en Bajo de 1280x673.
- Ningún tirón de más de 100 ms después de «Escena lista», salvo los pasos de resolución, que se cuentan aparte.

**5. Hardware real antes de publicar:**
- Probar al menos en una integrada (Iris Xe o Vega 8) y en una GTX 1050 o similar.
- Pedir al jugador original el log completo de `%LOCALAPPDATA%\Cramion\Logs\<Juego>.log` (la línea «Perfil de hardware» y las de `[Rendimiento]`), antes y después.

## Preguntas abiertas

1. **El log del jugador.** No está en esta máquina. Hace falta saber qué juego es (imprimió «palancas», así que llevaba `adaptive=1`), su GPU y la gama detectada. Los ~9 ms de sombras no los he podido comprobar.
2. **Factores de lentitud.** Los ×6-8 (GTX 1050 / Iris Xe), ×10-14 (Vega 8), ×20-25 (UHD 620) y ×2-3 de CPU salen de especificaciones públicas. Todas las ganancias en ms de este plan son estimaciones hasta que haya una ejecución real.
3. **Política para el desarrollador.** ¿Debe poder entregar `adaptive=0`? La propuesta es que solo con la casilla de bloqueo, y que el jugador siempre gane. ¿Puede la gama Baja quitar el DOF/MB, las nubes o la volumétrica que el desarrollador encendió? La propuesta es que sí, como nivel de salida que el gobernador devuelve si sobra margen.
4. **Objetivo en integradas.** ¿60 FPS, o 30 estables con vsync cada 2 (F2-1)?
5. **Versión.** ¿La Fase 1 va dentro de la 2.4.13 (sin publicar) o en la 2.4.14?
6. **RT en GTX 10xx/16xx.** ¿Expone el driver de NVIDIA el trazado de rayos de Vulkan en esas tarjetas? Si es así, una GTX 1070 (Alto) activaría RT con el ini del proyecto. F1-2 solo lo corta por debajo de Alto; F2-2 lo cubre del todo.
7. **Modo compatible en gama baja.** ¿Caen en él las integradas antiguas (drivers anteriores a Vulkan 1.3; `VulkanDevice.cpp:57-90`)? Eso decide si les afecta el fallo de `drawCpuActors` y si tienen culling en GPU.
8. **Camino sin mesh shaders.** Su coste en las GPUs de gama baja no está medido. «Geometria» y «Sombras» del PC del autor no lo representan.
9. **Formatos de la GI.** No he verificado que `gi_raw_` y `gi_image_` compartan formato y tamaño (F1-6), ni si sin las pasadas à-trous aparecen costuras en el lightmap de superficie.
10. **Volúmenes de Keryan.** ¿Reinician de verdad el presupuesto en cada frame al moverse entre sus 83 volúmenes (C11)? El mecanismo está comprobado; la frecuencia real no.
11. **Datos de la investigación sin re-verificar:**
    - El reparto en ms del tiempo de RenderSync en Keryan (sus trazas se han sobrescrito).
    - Los recuentos de entidades, colliders y renderers.
    - Los cálculos de LOD y de objetos diminutos con los bounds reales.
    - La RAM duplicada por las variantes.
    - La sobresuscripción de hilos en CPUs de 4 hilos.
    - El coste por vértice de `skinned.vert` en modelos rígidos.
    - El coste por paso de los shaders de nubes, volumétrica y SSGI.
12. **Atajos de diseño en Keryan.** ¿Necesita generar la navmesh en tiempo real y la flotación en ríos decorativos? Apagarlos sería una ganancia inmediata, pero toca el proyecto del usuario: solo con su permiso.
13. **Actores dibujados en Keryan.** Dibuja 920-1.198 actores de 3.540 renderers dibujables. ¿Es carga asíncrona, scripts o modelos que no se resuelven? Las ganancias de LOD y CPU escalan con lo que esté cargado.
14. **Modo cine del editor en PCs Bajo.** No da la calidad máxima porque conserva los niveles de salida. Es solo del editor y menor.
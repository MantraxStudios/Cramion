# Mejora de rendimiento de Cramion: auditoría

Fecha: 2026-10-19. Base: `main` (31eaff6). Limpiado el 2026-10-31: se quitó lo ya hecho y compilado (el trabajo del 2026-10-19 y los parches de Keryan); falta probarlo en los proyectos (`PENDIENTES.md`).

Es el único documento de rendimiento: sustituye a `PLAN-OPTIMIZACION.md` (borrado el 2026-10-19; está en git, commit 31eaff6). Lo que quedaba pendiente de aquel plan está en la sección 6 con sus mismos códigos (F1-8, F2-1...).

Leyenda:
- **(código)**: leído en el código, con archivo y línea.
- **(est.)**: ganancia estimada, no medida.
- Esfuerzo: S (horas), M (1-3 días), L (una semana o más).

## Resumen

Lo que queda de las tres familias de problemas:

1. **Trabajo repetido en cada frame que no cambia.** Falta el buffer persistente de clústeres en GPU (A4/B4).
2. **Memoria de GPU fuera de VMA:** `LightmapAtlas`, `FoliagePass` y `TerrainPass` siguen con `findMemoryType` propio (A1).
3. **Duplicados por las variantes `.crmat`.** Queda el tiling de `RuntimeMesh` (sección 1).

Técnicas que faltan (sección 3): vértices comprimidos, buffer persistente de lo estático en GPU, sombras de lo estático por GPU, materiales bindless, grabación de comandos en varios hilos, impostores y HLOD.

## 1. Variantes `.crmat`: lo que queda

Queda de esta parte (lo demás está hecho y compilado; el arreglo de `drawCpuActors` sigue sin probar en modo compatible): `RuntimeMesh.cpp` sigue horneando su propio tiling (no es una variante); cambiar el tiling en el inspector rehace la variante (no va en vivo).

## 2. Mal programado: hallazgos nuevos

### A1. Memoria de Vulkan fuera de VMA (código)
VMA y el anillo de staging están hechos. Queda fuera: `LightmapAtlas`, `FoliagePass` y `TerrainPass` siguen con `findMemoryType` propio.

### A4. Lo estático se recalcula entero en cada frame (código) — alta en mundos grandes
Las cachés en CPU están hechas. **Falta:** el buffer persistente de clústeres en GPU (`GpuCluster` se sigue subiendo cada frame).
- **VulkanRenderer, `updateActors`** (`VulkanRenderer.cpp:1636-1810`): vuelve a crear cada `GpuCluster` y lo sube todo de nuevo en cada frame.
- Arreglo: para lo rígido que no se mueve (versión de `Transform` igual), reutilizar cajas y clústeres de un **buffer persistente de estáticos**. Hoy `EntityInfo::is_static` no se usa en tiempo de ejecución.
- Esfuerzo: M.

### A5. Identidad por posición en la lista (código)
Hecho salvo `tiny_actors_`, que sigue por índice.

### A6. Sombras de lo estático dibujadas por la CPU, objeto a objeto (código) — media-alta
**Parcial:** rejilla XZ de actores (celdas de 8 m) para las luces locales en `recordActorShadows`, y la sombra con otro LOD prueba `world_box` en vez de la esfera. De Keryan: proyectores fuera del trozo de vista barrido hacia el sol (0013), recorte por clúster en cascadas (0015) y terreno por cascada (0016). **Falta:** el camino por GPU (culling por clúster en compute y `drawIndexedIndirectCount` por cascada o cara): necesita compute nuevo, vertex shader de sombra con la matriz por `gl_InstanceIndex`, pipelines y sets, y encajarlo con la caché estática y lo recortado.
- `recordActorShadows` (`VulkanRenderer.cpp:2086-2200`) recorre todos los actores por cada cascada y luz, con culling en la CPU y un `drawIndexed` por tramo.
- Con un LOD de sombra distinto al de la cámara, solo prueba la esfera del actor (`:2145-2155`), sin cajas por submalla.
- `actorsTouch` compara cada luz con todas las esferas (luces × N).
- Arreglo:
  - Mover lo estático al mismo camino que la vista principal: culling por clúster en compute y `drawIndexedIndirectCount`, una vez por cascada o cara.
  - Un BVH o rejilla de actores para las luces.
- Ganancia (est.): 2-6 ms de CPU en escenas con muchas luces con sombra. Esfuerzo: M-L.

### A9. Vértice gordo de 80 bytes también en lo rígido (código) — media
- `SkinnedVertex` (`Model.h:24-35`, con `static_assert` en `SkinnedModel.cpp:23`) lleva 4 huesos y 4 pesos en `uint32` y `float`: 32 de los 80 bytes. Los modelos rígidos (casi todo un escenario) tienen siempre un hueso.
- Arreglo (técnica B3): formato rígido de 32-40 bytes, con un vertex shader propio o un `#define`.
  - Posición en float, normal y tangente en octaedro de 16 bits.
  - UV en half, o en float si hay tiling grande.
  - Sin huesos.
- Ganancia (est.): un 50-60 % menos de ancho de banda de vértices. Notable en sombras (4 cascadas) y en GPUs integradas. Esfuerzo: M-L. Requiere tocar meshlets (`meshlet_common.glsl` lee 20 floats).

## 3. Técnicas de optimización que faltan

Orden aproximado por relación entre ganancia y esfuerzo. «Ya existe» significa que no hace falta añadirla.

| # | Técnica | Estado en Cramion | Qué daría | Esfuerzo |
|---|---|---|---|---|
| B2 | Async compute | No (la cola de transferencia ya está) | Async compute para culling, SSAO o nubes en GPUs que lo solapan | L |
| B3 | Compresión de vértices (formato rígido cuantizado) | No (A9) | −50 % de ancho de banda de vértices | M-L |
| B4 | Buffer persistente de estáticos | Parcial: cachés en CPU hechas (A4); los clústeres se suben cada frame | CPU por frame casi constante con lo estático | M |
| B5 | Sombras por GPU (indirect + culling por clúster en cascadas y locales) | Solo en la vista principal; rejilla de luces en CPU hecha (A6) | CPU de sombras ~0 con lo estático | M-L |
| B7 | Materiales bindless (descriptor indexing) | El dispositivo ya pide `runtimeDescriptorArray` (`VulkanDevice.cpp:475`), pero hay un descriptor set por material y modelo | Menos cambios de set, lotes entre modelos con distinto material, y variantes `.crmat` que serían solo un índice de material | L |
| B8 | Instancia con material propio (sin variante) | No: cada combinación de `.crmat` es otro modelo | Elimina las variantes de raíz (GPU, RAM, RT y tiling). Depende de B7 | L |
| B9 | Grabación de comandos en varios hilos (secondary command buffers) | No | 1-3 ms de CPU en escenas con muchos pases | M-L |
| B11 | Impostores / billboards para lo lejano | No | Árboles y edificios lejanos a 2 triángulos | M |
| B12 | HLOD (fusionar grupos lejanos en una malla) | No (F3-3). El parche 0032 de Keryan (LOD por parches) está pospuesto | Menos draws y clústeres en ciudades | L |
| B14 | Static batching en el editor y en Play | Solo al exportar (`StaticBatching.cpp`) | El editor y Play van como el juego | M |
| B16 | Meshlets / mesh shaders | **Ya existe** (solo rígidos cerrados) | Ampliar a mallas abiertas con culling por cono desactivado | S |
| B17 | Compresión de texturas BC1/BC7 con mips | **Ya existe**, también en las imágenes de la UI, los árboles y las capas del terreno | Falta ASTC/ETC2 para Android: hoy se descomprime a RGBA8 en la CPU (`SkinnedModel.cpp:383-389`), con 4-8 veces más VRAM en el móvil | M |
| B19 | Streaming de texturas por mips | **Ya existe** (`texture_lod`) | Solo de 2 en 2 niveles. Falta un sampler feedback o una prioridad por pantalla más fina | M |

## 4. Orden recomendado

1. **Ya:** probar en los proyectos todo lo del 2026-10-19 (ya compilado). Riesgos a revisar en `PENDIENTES.md`.
2. **M, mundos grandes:** lo que falta de A4/B4 (clústeres persistentes en GPU) y A6/B5 (sombras por GPU).
3. **Gama baja:** lo que falta de F2-7 y el resto de la fase 2 (sección 6).
4. **L:** B7 y B8 (bindless e instancia con material: quita las variantes de raíz), B3 (vértices), B11-B12 (y con ellos el 0032 de Keryan).

## 5. Cómo medir antes y después

- **CPU:** perfilador del editor, zonas «Recorrer entidades», «Actores y animación», «Subir modelos nuevos» y las de sombras (`CRAMION_SHADOW_MARKS=1`). En la línea `[Rendimiento]` del jugador, los ms de CPU del hilo principal.
- **Memoria:** VRAM de buffers y texturas en el log (`GpuMemoryStats`) y RAM del proceso (Administrador de tareas, memoria privada). Para el trazado de rayos agrupado por geometría, comparar `test` y Keryan con el commit 5f7a7d3 (la línea «Trazado de rayos listo» da ahora BLAS, modelos que comparten geometría y modelos sin instancias).
- **Carga:** tiempo de «Escena lista» en el log, desde abrir la escena.
- **Escenas:** Keryan (mundo grande, todo con `.crmat`), `test` (exterior y lote estático), BlackHouse (interior) y una ciudad de objetos Static (A4-A5).
- **Tests:** uno a uno, como siempre: `CramionMaterialTests`, `CramionCoreTests`, `CramionOptimizationTests` y `CramionPhysicsTests`.

## 6. Pendiente del plan de gama baja (antes en `PLAN-OPTIMIZACION.md`)

**Fase 1: hecha y compilada.** Solo queda de F1-8 (sol horneado sin cascadas cuando nadie las lee) el consejo en la ventana de iluminación del editor.

**Fases 2 y 3** (F2-7 en parte):

| Ítem | Qué es | Esfuerzo | Estado |
|---|---|---|---|
| F2-1 | Aprendizaje del gobernador por tiempo y niveles guardados por PC (hoy oscila a 10-30 FPS) | M | Pendiente |
| F2-2 | Gama por rendimiento real, no solo por VRAM; RT solo con hardware de RT | M | Pendiente |
| F2-3 | Nubes solo donde se ve cielo y calidad Baja | M | Pendiente |
| F2-4 | DOF y motion blur por tiles, a media resolución en Bajo/Medio, sin copias | M | Pendiente |
| F2-5 | Sonda de reflexión a 256×256 sin parar la CPU | M | Pendiente |
| F2-6 | SSGI/SVGF en calidad Baja | M | Pendiente |
| F2-7 | Iluminación y agua en calidad Baja | S-M | Parcial: SSR del agua a 12 pasos y 3 de afinado con iluminación ligera; `lighting_quality` auto/full/lite (0031). Faltan `probeDirection` a 8 pasos, blur del SSAO, sombras de contacto en Medio, tope de luces locales y compartir la copia de escena entre vidrio y agua |
| F2-9 | LODs en mundos grandes: lote estático, umbral ~1.000 triángulos, caché de LODs | S+S+M | Pendiente; el 0032 de Keryan (LOD por parches) va aquí, pospuesto |
| F2-11 | CPU de mundos grandes, segunda tanda | 5×M | Pendiente |
| F2-12 | Ajustes del editor separados del juego; preset de primera ejecución; menú de opciones integrado | M | Pendiente |
| F2-13 | Gama de CPU y palancas de CPU | M | Pendiente |
| F3-1 | Resolución dinámica real, sin recrear destinos ni `waitIdle` | L | Pendiente (el sesgo de mips ya no depende del presupuesto: sin tirón al cambiar de escala) |
| F3-2 | Sol Mixto con shadowmask horneado (ver `PLAN-LIGHTMAPS.md`) | L | Pendiente |
| F3-3 | LOD por clúster / HLOD (= B12) | L | Pendiente; 0032 pospuesto |
| F3-5 | G-buffer más delgado | L | Pendiente |
| F3-6 | Camino de vértices rígido sin mezcla de matrices (va con B3/A9) | M | Pendiente |

**Reglas que siguen valiendo:**
- No ampliar las push constants (128 B).
- No cambiar el orden ni los nombres de las palancas, de los pases ni de los campos de `[Rendimiento]`: los leen el editor, el MCP y las personas. Lo nuevo va al final.
- No quitar el filtro de tirones del presupuesto: las cargas no deben aprenderse como coste.
- No tocar la VR ni el perfil móvil sin probarlos.
- Para medir gama baja en el PC del autor: `CRAMION_HARDWARE_TIER=low` y `CRAMION_NO_MESH=1`, en copias de los juegos (no tocar `%LOCALAPPDATA%\Cramion\Saves` ni `Games`).
- Los jugadores solo reciben los arreglos si se vuelven a exportar los juegos.

**Preguntas abiertas:**
- ¿Puede un desarrollador entregar su juego con `adaptive=0`? Hoy solo con la casilla «Respetar los ajustes gráficos del proyecto».
- En gráficas integradas, ¿objetivo 60 FPS o 30 estables?

## 7. Parches de Keryan (44, revisados el 2026-10-19)

Todos aplicados, adaptados o descartados (y compilados) salvo el 0032.

| Nº | Qué hace | Estado |
|---|---|---|
| 0032 | LOD por parches para modelos enormes | **Pospuesto** (~400 líneas, riesgo medio-alto; choca con SharedVector, lightmaps y el bucle nuevo de clústeres). Va con F2-9/F3-3 |

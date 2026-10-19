# Mejora de rendimiento de Cramion: auditoría

Fecha: 2026-10-19. Base: `main` (31eaff6).

Es el único documento de rendimiento: sustituye a `PLAN-OPTIMIZACION.md` (borrado el 2026-10-19; está en git, commit 31eaff6). Lo que quedaba pendiente de aquel plan está en la sección 6 con sus mismos códigos (F1-6, F2-1...).

Leyenda:
- **(código)**: leído en el código, con archivo y línea.
- **(est.)**: ganancia estimada, no medida.
- Esfuerzo: S (horas), M (1-3 días), L (una semana o más).

## Resumen

Hay tres familias de problemas:

1. **Trabajo repetido en cada frame que no cambia.** Lo estático se trata como si se moviera: RenderSync y `updateActors` rehacen cajas, clústeres, lotes y animadores de todos los objetos en cada frame. Además, varias comprobaciones van por posición en la lista en vez de por identidad, y un objeto que entra o sale «mueve» a todos los de detrás.
2. **Gestión de memoria de GPU ingenua.**
   - Una asignación de Vulkan (`vkAllocateMemory`) por cada buffer y cada textura, sin suballocador.
   - Una espera síncrona (fence) por cada subida.
   - Todo va por la cola gráfica.
   - Escala mal con miles de texturas y roza el límite de asignaciones del driver (4096 en muchos drivers de Windows).
3. **Duplicados por las variantes `.crmat`.** Ya está arreglado en GPU, RAM y LODs. Quedan el trazado de rayos y el tiling (sección 1).

Técnicas que faltan (sección 3): suballocador de memoria (VMA), cola de transferencia, vértices comprimidos, buffer persistente de lo estático, sombras de lo estático por GPU, materiales bindless, grabación de comandos en varios hilos, impostores y HLOD, y un pool de hilos único para el streaming.

## 1. Ya hecho (commit 31eaff6)

| Cambio | Archivos | Estado |
|---|---|---|
| Las variantes `.crmat` comparten vértices e índices en la GPU (caché por contenido, como las texturas) | `CramionFX/src/vk/SkinnedModel.cpp`, `SkinnedModel.h` | Compila; tests OK |
| Las variantes comparten la geometría en RAM: `core::SharedVector`, con copia al escribir, en `vertices`, `indices` y `lod_indices` | `CramionFX/include/CramionFX/core/SharedVector.h`, `asset/Model.h`, `ModelCache.cpp`, `Cloth.cpp` | Compila; tests OK |
| Las variantes conservan LODs y UV de lightmap (F1-10) | `CramionCore/src/ecs/RenderSync.cpp` (`buildVariant`) | Compila; tests OK |
| **Bug:** `drawCpuActors` leía las submallas de LOD0 con índices del LOD elegido. En modo compatible y Android se dibujaban otros tramos o se salía del buffer | `CramionFX/src/vk/VulkanRenderer.cpp:6168` | Arreglado; compila (sin probar en modo compatible) |

Pendiente de esta parte (pedido por el usuario, va lo siguiente):
- **Trazado de rayos.** `RayTracing::build` (`RayTracing.cpp:946-956`) copia los vértices de **todos** los modelos a un buffer único. Eso incluye el modelo base que nadie dibuja y cada variante con la misma malla, y construye una BLAS por modelo (`:1188-1234`).
  - Arreglo: agrupar los modelos por geometría compartida (`SharedVector::sharesWith` o la clave del buffer) y dar una sola BLAS al grupo. Los materiales van aparte, por instancia (`instanceCustomIndex`).
  - Saltar los modelos sin instancias.
  - (est.) Con todo con `.crmat`, la mitad de memoria de BLAS y de tiempo de construcción.
- **Tiling.** `buildVariant` hornea el tiling en las UV (`RenderSync.cpp:731-746`), y eso separa los vértices de esa variante.
  - Arreglo: pasar escala y desplazamiento de UV por material. Las push constants están llenas (128 B), así que irían en el buffer de materiales o en `surface_params`.

## 2. Mal programado: hallazgos nuevos

### A1. Una asignación de memoria de Vulkan por recurso, sin suballocador (código) — prioridad alta
- `VulkanBuffer::create` hace un `vk::raii::DeviceMemory` por buffer (`VulkanBuffer.cpp:31`). Las texturas, uno por imagen (`VulkanTexture.cpp:75`, `:190`).
- `createDeviceLocal` crea además un staging propio con su asignación y lo destruye (`VulkanBuffer.cpp:46-66`).
- Problemas:
  - `vkAllocateMemory` es lento: microsegundos o milisegundos, con bloqueo del driver.
  - Muchos drivers limitan `maxMemoryAllocationCount` a 4096. Una escena con 2.000 texturas más buffers y stagings se acerca o lo pasa, y entonces falla la subida.
  - Hay fragmentación y no se respeta `bufferImageGranularity` agrupando.
- Arreglo: integrar **VMA** (Vulkan Memory Allocator, MIT, de un solo archivo) en `VulkanBuffer` y `VulkanTexture`, más un anillo de staging persistente.
- Ganancia (est.): cargas un 20-40 % más rápidas y sin fallos por límite de asignaciones. Esfuerzo: M.

### A2. Cada subida espera a la GPU, de una en una, en la cola gráfica (código) — alta
- `VulkanDevice::submitOneTime` (`VulkanDevice.cpp:743-785`) crea una fence, envía a `graphics_queue_` y espera con `waitForFences(UINT64_MAX)`.
- `SkinnedModel::create` lo llama por cada buffer y cada textura. Un modelo con 30 texturas son unas 32 esperas completas, que compiten con el frame en la misma cola y el mismo `queue_mutex_`.
- Arreglo:
  - Agrupar todas las copias de un modelo en un solo command buffer y una sola fence.
  - Usar una **cola de transferencia dedicada** si la GPU la tiene, con transferencia de propiedad entre colas.
  - No bloquear el hilo: comprobar la fence en `uploadNewModels`.
- Ganancia (est.): subidas 3-10 veces más rápidas y menos tirones al hacer streaming. Esfuerzo: M.

### A3. Un hilo del sistema por cada modelo que se sube (código) — media
- `launchModelJob` usa `std::async(std::launch::async, ...)` (`VulkanRendererSkyMap.cpp:235`). Con libc++ (Android) eso crea y destruye un hilo del SO por modelo. Ya existe `CramionCore/jobs/JobSystem` (lo usan el follaje y el terreno), pero el renderizador no lo usa.
- Lo mismo con los clips (`RenderSync.cpp:1102`) y los vóxeles (`VoxelSystem.cpp:1105-1110`).
- Arreglo: un pool de hilos de streaming en CramionFX, o inyectar el `JobSystem` con prioridad Background. Esfuerzo: S-M.

### A4. Lo estático se recalcula entero en cada frame (código) — alta en mundos grandes
Es la causa de los bajones de FPS de la ciudad gigante de objetos Static (análisis del 15-10-2026). Aquí va el detalle de dónde se gasta:
- **RenderSync, `syncActors`** (`RenderSync.cpp:2955-3420`), por objeto y frame:
  - `resolveModel` y `resolveVariant`. Este construye la clave y llama a `material(uuid)` por hueco (`:772`), que copia un `shared_ptr` (incremento atómico) y busca en un hash map.
  - Busca en `animations_`, también un hash map, y crea un `Animator` incluso para un edificio (`:2996-3003`).
  - Copia la transformación y recalcula `bounds_center` y `bounds_radius`.
- **VulkanRenderer, `updateActors`** (`VulkanRenderer.cpp:1636-1810`), por objeto y frame:
  - Transforma la caja de **todas** las submallas de LOD0 (`:1692-1696`), aunque se dibuje otro LOD. En ese caso transforma además las del LOD (`:1722-1726`).
  - Vuelve a crear cada `GpuCluster`, con una búsqueda en `batch_lookup_` (hash) por clúster (`:1753`).
  - Copia los huesos a `bone_staging_` y sube todo de nuevo.
- Arreglo:
  - Caché por entidad de «modelo resuelto + variante» que solo se invalida si cambian `MeshRenderer` o `materials` (versión del componente).
  - Para lo rígido que no se mueve (versión de `Transform` igual), reutilizar cajas y clústeres de un **buffer persistente de estáticos**. Hoy `EntityInfo::is_static` no se usa en tiempo de ejecución.
  - No calcular las cajas de LOD0 cuando el LOD elegido es otro.
- Ganancia (est.): con unos 3.500 objetos (Keryan), 4-8 ms de CPU menos en el PC del autor y el doble en 4 núcleos. Esfuerzo: M.

### A5. Identidad por posición en la lista (código) — alta
- **Sombras:** el movimiento se compara con `previous_actor_motion_[index]` (`VulkanRenderer.cpp:1790-1797`). Si cambia el número de actores, `actor_set_changed_` (`:1932`) redibuja las 4 cascadas y todas las luces locales. El streaming de residencia cambia ese número sin parar.
- **Animadores:** en `syncActors`, `same_slot` también va por posición (`RenderSync.cpp:3389-3396`). Un objeto que entra al principio hace que **todos** copien su `Animator` entero (`actor.animator = state.animator`).
- Arreglo: comparar por `scene_actor` o por entidad. Un actor nuevo o que sale solo marca su propia esfera. Esfuerzo: S. Es lo primero que hay que hacer para la ciudad Static. Para confirmarlo antes: desactivar `render.streaming.GpuResidency`; si los picos se van, es esto.

### A6. Sombras de lo estático dibujadas por la CPU, objeto a objeto (código) — media-alta
- `recordActorShadows` (`VulkanRenderer.cpp:2086-2200`) recorre todos los actores por cada cascada y luz, con culling en la CPU y un `drawIndexed` por tramo.
- Con un LOD de sombra distinto al de la cámara, solo prueba la esfera del actor (`:2145-2155`), sin cajas por submalla.
- `actorsTouch` compara cada luz con todas las esferas (luces × N).
- Arreglo:
  - Mover lo estático al mismo camino que la vista principal: culling por clúster en compute y `drawIndexedIndirectCount`, una vez por cascada o cara.
  - Un BVH o rejilla de actores para las luces.
- Ganancia (est.): 2-6 ms de CPU en escenas con muchas luces con sombra. Esfuerzo: M-L.

### A7. Log por cada modelo subido (código) — baja, pero gratis
- Hay `std::cout` en cada `SkinnedModel::create` (`SkinnedModel.cpp:565`) y en cada `buildMeshlets` (`:673`), además de unas 16 líneas más en `VulkanRenderer.cpp`.
- `std::cout` sincroniza con `stdio`. Con streaming son cientos de líneas por minuto desde hilos de subida, y la consola de Windows es lenta.
- Arreglo: pasar al nivel debug del logger, o resumir una línea por lote. Esfuerzo: S.

### A8. El modelo base de una variante se sube y se mantiene aunque nadie lo dibuje (código) — media
- `uploadNewModels` sube **todos** los modelos de la escena en orden (`VulkanRenderer.cpp:3768-3772`).
- Con la geometría ya compartida, el coste que queda es:
  - las texturas propias del modelo base que todos sus `.crmat` sustituyen (VRAM real);
  - sus descriptor sets;
  - meter el base en la escena de rayos.
- Arreglo: una marca «solo base de variantes» en la escena. No se sube hasta que lo use alguna instancia, y el streaming lo expulsa (la parte que faltaba de F2-10). Esfuerzo: S-M.

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
| B1 | Suballocador de memoria (VMA) y anillo de staging | No: una asignación por recurso (A1) | Cargas más rápidas, sin el límite de 4096, menos fragmentación | M |
| B2 | Cola de transferencia / async compute | No: todo en la cola gráfica (A2) | Subidas sin parar el frame. Async compute para culling, SSAO o nubes en GPUs que lo solapan | M / L |
| B3 | Compresión de vértices (formato rígido cuantizado) | No (A9) | −50 % de ancho de banda de vértices | M-L |
| B4 | Buffer persistente de estáticos | No: se rehace cada frame (A4) | CPU por frame casi constante con lo estático | M |
| B5 | Sombras por GPU (indirect + culling por clúster en cascadas y locales) | Solo en la vista principal (A6) | CPU de sombras ~0 con lo estático | M-L |
| B6 | Caché de sombras estáticas por identidad | Hay caché, pero se invalida por índice (A5) | Sin redibujar 4 cascadas por streaming | S |
| B7 | Materiales bindless (descriptor indexing) | El dispositivo ya pide `runtimeDescriptorArray` (`VulkanDevice.cpp:475`), pero hay un descriptor set por material y modelo | Menos cambios de set, lotes entre modelos con distinto material, y variantes `.crmat` que serían solo un índice de material | L |
| B8 | Instancia con material propio (sin variante) | No: cada combinación de `.crmat` es otro modelo | Elimina las variantes de raíz (GPU, RAM, RT y tiling). Depende de B7 | L |
| B9 | Grabación de comandos en varios hilos (secondary command buffers) | No | 1-3 ms de CPU en escenas con muchos pases | M-L |
| B10 | Pool de hilos único para el streaming | Parcial: hay `JobSystem`, pero el renderizador usa `std::async` (A3) | Menos hilos del SO y prioridades | S-M |
| B11 | Impostores / billboards para lo lejano | No | Árboles y edificios lejanos a 2 triángulos | M |
| B12 | HLOD (fusionar grupos lejanos en una malla) | No (F3-3) | Menos draws y clústeres en ciudades | L |
| B13 | LOD de personajes con esqueleto | No (F3-4) | Multitudes | M |
| B14 | Static batching en el editor y en Play | Solo al exportar (`StaticBatching.cpp`) | El editor y Play van como el juego | M |
| B15 | Oclusión Hi-Z | **Ya existe** (`GpuCulling.cpp`) | — | — |
| B16 | Meshlets / mesh shaders | **Ya existe** (solo rígidos cerrados) | Ampliar a mallas abiertas con culling por cono desactivado | S |
| B17 | Compresión de texturas BC1/BC7 con mips | **Ya existe** | Falta ASTC/ETC2 para Android: hoy se descomprime a RGBA8 en la CPU (`SkinnedModel.cpp:383-389`), con 4-8 veces más VRAM en el móvil | M |
| B18 | Caché de pipelines en disco | **Ya existe** (`VulkanDevice.cpp:999-1053`) | — | — |
| B19 | Streaming de texturas por mips | **Ya existe** (`texture_lod`) | Solo de 2 en 2 niveles. Falta un sampler feedback o una prioridad por pantalla más fina | M |

## 4. Orden recomendado

Va en paralelo a lo pendiente de la sección 6, porque toca otras zonas.

1. **Ya:** compilar y probar lo de la sección 1 (variantes y `drawCpuActors`). Prueba con `CRAMION_VK_COMPAT=1` y una escena con LODs y `.crmat`.
2. **S, mucho impacto:** A5/B6 (identidad estable en la caché de sombras y en los animadores), A7 (logs) y la parte S de A8.
3. **Trazado de rayos y tiling** (sección 1, pendiente pedido).
4. **M, base para todo lo demás:** A1/B1 (VMA) y A2/B2 (subidas por lotes y cola de transferencia), A3/B10.
5. **M, mundos grandes:** A4/B4 (persistente de estáticos) y luego A6/B5 (sombras por GPU).
6. **L:** B7 y B8 (bindless e instancia con material: quita las variantes de raíz), B3 (vértices), B11-B13.

## 5. Cómo medir antes y después

- **CPU:** perfilador del editor, zonas «Recorrer entidades», «Actores y animación», «Subir modelos nuevos» y las de sombras (`CRAMION_SHADOW_MARKS=1`). En la línea `[Rendimiento]` del jugador, los ms de CPU del hilo principal.
- **Memoria:** VRAM de buffers y texturas en el log (`GpuMemoryStats`) y RAM del proceso (Administrador de tareas, memoria privada). Para la sección 1, comparar `test` y Keryan con el commit 5f7a7d3.
- **Carga:** tiempo de «Escena lista» en el log, desde abrir la escena.
- **Escenas:** Keryan (mundo grande, todo con `.crmat`), `test` (exterior y lote estático), BlackHouse (interior) y una ciudad de objetos Static (A4-A5).
- **Tests:** uno a uno, como siempre: `CramionMaterialTests`, `CramionCoreTests`, `CramionOptimizationTests` y `CramionPhysicsTests`. Falta un test de `SharedVector` y otro de que una variante sin tiling comparte los vértices con su base (`sharesWith`).

## 6. Pendiente del plan de gama baja (antes en `PLAN-OPTIMIZACION.md`)

**Fase 1: hechos 10 de 13.** F1-1, F1-2, F1-3, F1-4, F1-5, F1-7, F1-9, F1-11 y F1-13 están en `main` (2fd1af3 y 5f7a7d3), y F1-10 está hecho en esta sesión. Faltan tres:

**F1-6. GI horneada sin el filtro SVGF** (S, riesgo bajo)
- En `recordSsgiPass` (`VulkanRenderer.cpp`, ~6545-6632), con GI horneada y sin sondas dinámicas: después de `baked_gi_pass_`, copiar `gi_raw_` a `gi_image_` (o enlazar `gi_raw_` en la iluminación) y saltarse el temporal, las 5 pasadas à-trous y la copia de momentos. Poner `gi_filter_history_valid_ = false`. Falta comprobar que `gi_raw_` y `gi_image_` tienen el mismo formato y tamaño.
- Con GI horneada, `budget_.setLevelCap(Lever::Gi, 0)`: hoy esa palanca no hace nada en ese modo.
- Ganancia (est.): la GI de ~20 a 2-3 ms en una GTX 1050 a resolución nativa. Si salen costuras, dejar una pasada à-trous.

**F1-8. Sol horneado sin cascadas ni sombras por rayos cuando nadie las lee** (S, riesgo bajo-medio)
- Función nueva `sunCascadesNeeded()`: `sunShadows() && !(bakedSunActive(dir) && !post_.volumetric_light && water_pass_.empty() && !fluid_pass_.active() && no hay vidrio)`.
- Usarla en la programación de cascadas (~4343-4350), en `recordShadowPass` (~5539) y en las sombras del sol por rayos (~4077). Corregir `docs-src/pages/iluminacion-horneada.html:53`, que dice que una luz Horneada «no cuesta nada».
- Ganancia (est.): 7-20 ms de cascadas en gama baja. Riesgo: fugas de luz si se olvida algún lector de las cascadas (agua, vidrio, volumétrica, fluidos).

**F1-12. CPU: ríos, vegetación, ondas y flotación sin trabajo repetido** (S-M, riesgo bajo)
- Ríos: guardar la línea central con el hash de la cinta y pasarla a `sampleWater(…, line)` (`Water.h:117`). Descartar por AABB. Guardar las cintas en un `shared_ptr<const>`: hoy se copian ~8 MB por frame.
- `syncFoliage`: rehacer la lista de agua solo si cambia su hash y compartirla entre bosques (~7 MB por frame hoy).
- `updateRipples`: salir enseguida si no hay agua a unos 30 m; contenedores miembro en vez de crearlos cada frame.
- Flotación (`PhysicsSystem.cpp:3105`): líneas en caché, y nada si no hay cuerpos dinámicos.
- Ganancia (est.): Keryan, 3-7 ms menos en el PC del autor y 8-18 ms en 4 núcleos.

**Fases 2 y 3** (sin empezar; F2-10 está casi hecho, falta A8):

| Ítem | Qué es | Esfuerzo |
|---|---|---|
| F2-1 | Aprendizaje del gobernador por tiempo y niveles guardados por PC (hoy oscila a 10-30 FPS) | M |
| F2-2 | Gama por rendimiento real, no solo por VRAM; RT solo con hardware de RT | M |
| F2-3 | Nubes solo donde se ve cielo y calidad Baja | M |
| F2-4 | DOF y motion blur por tiles, a media resolución en Bajo/Medio, sin copias | M |
| F2-5 | Sonda de reflexión a 256×256 sin parar la CPU | M |
| F2-6 | SSGI/SVGF en calidad Baja | M |
| F2-7 | Iluminación y agua en calidad Baja | S-M |
| F2-8 | Distancia de sombras y sesgo de LOD por gama (hoy fija en 100 m) | S |
| F2-9 | LODs en mundos grandes: lote estático, umbral ~1.000 triángulos, caché de LODs | S+S+M |
| F2-11 | CPU de mundos grandes, segunda tanda | 5×M |
| F2-12 | Ajustes del editor separados del juego; preset de primera ejecución; menú de opciones integrado | M |
| F2-13 | Gama de CPU y palancas de CPU | M |
| F3-1 | Resolución dinámica real, sin recrear destinos ni `waitIdle` | L |
| F3-2 | Sol Mixto con shadowmask horneado (ver `PLAN-LIGHTMAPS.md`) | L |
| F3-3 | LOD por clúster / HLOD (= B12) | L |
| F3-4 | LODs para personajes con esqueleto (= B13) | M |
| F3-5 | G-buffer más delgado | L |
| F3-6 | Camino de vértices rígido sin mezcla de matrices (va con B3/A9) | M |

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

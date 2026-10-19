# Mejora de rendimiento de Cramion: auditoría

Fecha: 2026-10-19. Base: `main` (5f7a7d3) más los cambios sin commit de esta sesión.

Este documento completa `PLAN-OPTIMIZACION.md` (gobernador, palancas, gama baja; sus causas C1-C12 y fases F1-F3) y no lo repite. Cuando un punto ya está allí, solo se enlaza.

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

## 1. Ya hecho en esta sesión (sin commit)

| Cambio | Archivos | Estado |
|---|---|---|
| Las variantes `.crmat` comparten vértices e índices en la GPU (caché por contenido, como las texturas) | `CramionFX/src/vk/SkinnedModel.cpp`, `SkinnedModel.h` | Compila; tests OK |
| Las variantes comparten la geometría en RAM: `core::SharedVector`, con copia al escribir, en `vertices`, `indices` y `lod_indices` | `CramionFX/include/CramionFX/core/SharedVector.h`, `asset/Model.h`, `ModelCache.cpp`, `Cloth.cpp` | Compila; tests OK |
| Las variantes conservan LODs y UV de lightmap (F1-10 del plan) | `CramionCore/src/ecs/RenderSync.cpp` (`buildVariant`) | Compila; tests OK |
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
Coincide con `PENDIENTES-MANANA.md` §6b. Aquí va el detalle de dónde se gasta:
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
- Arreglo: comparar por `scene_actor` o por entidad. Un actor nuevo o que sale solo marca su propia esfera. Esfuerzo: S. Es el arreglo 1 de §6b.

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
- Arreglo: una marca «solo base de variantes» en la escena. No se sube hasta que lo use alguna instancia, y el streaming lo expulsa (F2-10 del plan, la parte que falta). Esfuerzo: S-M.

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
| B12 | HLOD (fusionar grupos lejanos en una malla) | No (F3-3 del plan) | Menos draws y clústeres en ciudades | L |
| B13 | LOD de personajes con esqueleto | No (F3-4 del plan) | Multitudes | M |
| B14 | Static batching en el editor y en Play | Solo al exportar (`StaticBatching.cpp`) | El editor y Play van como el juego | M |
| B15 | Oclusión Hi-Z | **Ya existe** (`GpuCulling.cpp`) | — | — |
| B16 | Meshlets / mesh shaders | **Ya existe** (solo rígidos cerrados) | Ampliar a mallas abiertas con culling por cono desactivado | S |
| B17 | Compresión de texturas BC1/BC7 con mips | **Ya existe** | Falta ASTC/ETC2 para Android: hoy se descomprime a RGBA8 en la CPU (`SkinnedModel.cpp:383-389`), con 4-8 veces más VRAM en el móvil | M |
| B18 | Caché de pipelines en disco | **Ya existe** (`VulkanDevice.cpp:999-1053`) | — | — |
| B19 | Streaming de texturas por mips | **Ya existe** (`texture_lod`) | Solo de 2 en 2 niveles. Falta un sampler feedback o una prioridad por pantalla más fina | M |

## 4. Orden recomendado

Va después de la Fase 1 de `PLAN-OPTIMIZACION.md` o en paralelo, porque toca otras zonas.

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
- **Escenas:** Keryan (mundo grande, todo con `.crmat`), `test` (exterior y lote estático), BlackHouse (interior) y una ciudad de objetos Static (§6b).
- **Tests:** uno a uno, como siempre: `CramionMaterialTests`, `CramionCoreTests`, `CramionOptimizationTests` y `CramionPhysicsTests`. Falta un test de `SharedVector` y otro de que una variante sin tiling comparte los vértices con su base (`sharesWith`).

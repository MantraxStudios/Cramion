# Mejora de rendimiento de Cramion: auditoría

Fecha: 2026-10-19. Base: `main` (31eaff6). Actualizado el 2026-10-19 con el trabajo sin commitear de ese día (9 tareas en paralelo y los parches de Keryan): **nada de eso está compilado ni probado**; «hecho» quiere decir escrito y revisado a mano.

Es el único documento de rendimiento: sustituye a `PLAN-OPTIMIZACION.md` (borrado el 2026-10-19; está en git, commit 31eaff6). Lo que quedaba pendiente de aquel plan está en la sección 6 con sus mismos códigos (F1-6, F2-1...).

Leyenda:
- **(código)**: leído en el código, con archivo y línea.
- **(est.)**: ganancia estimada, no medida.
- Esfuerzo: S (horas), M (1-3 días), L (una semana o más).
- **Hecho (sin compilar)**: escrito el 2026-10-19, falta compilar y probar (lista de riesgos en `PENDIENTES.md`).

## Resumen

Hay tres familias de problemas:

1. **Trabajo repetido en cada frame que no cambia.** Lo estático se trata como si se moviera: RenderSync y `updateActors` rehacen cajas, clústeres, lotes y animadores de todos los objetos en cada frame. Además, varias comprobaciones van por posición en la lista en vez de por identidad, y un objeto que entra o sale «mueve» a todos los de detrás. *Hecho (sin compilar): identidad estable (A5), caché de resolución y cajas (A4); falta el buffer persistente en GPU.*
2. **Gestión de memoria de GPU ingenua.**
   - Una asignación de Vulkan (`vkAllocateMemory`) por cada buffer y cada textura, sin suballocador.
   - Una espera síncrona (fence) por cada subida.
   - Todo va por la cola gráfica.
   - Escala mal con miles de texturas y roza el límite de asignaciones del driver (4096 en muchos drivers de Windows).
   - *Hecho (sin compilar): VMA, anillo de staging, lotes por modelo y cola de transferencia (A1-A2).*
3. **Duplicados por las variantes `.crmat`.** Arreglado en GPU, RAM y LODs (31eaff6); trazado de rayos y tiling hechos (sin compilar, sección 1).

Técnicas que faltan (sección 3): vértices comprimidos, buffer persistente de lo estático en GPU, sombras de lo estático por GPU, materiales bindless, grabación de comandos en varios hilos, impostores y HLOD.

## 1. Ya hecho (commit 31eaff6 y trabajo del 2026-10-19)

| Cambio | Archivos | Estado |
|---|---|---|
| Las variantes `.crmat` comparten vértices e índices en la GPU (caché por contenido, como las texturas) | `CramionFX/src/vk/SkinnedModel.cpp`, `SkinnedModel.h` | Compila; tests OK |
| Las variantes comparten la geometría en RAM: `core::SharedVector`, con copia al escribir, en `vertices`, `indices` y `lod_indices` | `CramionFX/include/CramionFX/core/SharedVector.h`, `asset/Model.h`, `ModelCache.cpp`, `Cloth.cpp` | Compila; tests OK |
| Las variantes conservan LODs y UV de lightmap (F1-10) | `CramionCore/src/ecs/RenderSync.cpp` (`buildVariant`) | Compila; tests OK |
| **Bug:** `drawCpuActors` leía las submallas de LOD0 con índices del LOD elegido. En modo compatible y Android se dibujaban otros tramos o se salía del buffer | `CramionFX/src/vk/VulkanRenderer.cpp:6168` | Arreglado; compila (sin probar en modo compatible) |
| **Trazado de rayos:** modelos agrupados por geometría (buffers compartidos, submallas y tipo de alfa; en las recortadas también textura, alfa y tiling) con una sola copia de triángulos y una sola BLAS por grupo (`blas_owner`); cada textura entra una vez; los modelos sin instancias se saltan (si aparecen luego, reconstrucción con la espera de 1 s). `RtMaterial` lleva el tiling (80 B) | `RayTracing.cpp/.h`, `VulkanRendererRayScene.cpp`, `rt_common.glsl`, `path_trace.rahit/.rgen`, `rt_omm_bake.comp` | Hecho (sin compilar) |
| **Tiling:** `buildVariant` ya no toca los vértices; `uv_scale`/`uv_offset` van en `MaterialData` y en un uniform del set 1, binding 9 (`kMaterialUvBinding`), compartido por contenido. Se aplica en el fragment shader (y en `surface.vert` para el gancho `vertex()`); entra en el hash del lote estático | `RenderSync.cpp`, `SkinnedModel.*`, `SkinnedPass.*`, `skinned.frag/.tese`, `skinned_shadow.*`, `glass.frag`, `surface.vert`, `StaticBatching.cpp` | Hecho (sin compilar) |
| Tests: `testSharedVector` (`CoreTests.cpp`) y `testVariantSharesMesh` (`MaterialTests.cpp`) | `CramionCore/tests` | Hecho (sin ejecutar) |

Queda de esta parte: `RuntimeMesh.cpp` sigue horneando su propio tiling (no es una variante); cambiar el tiling en el inspector rehace la variante (no va en vivo).

## 2. Mal programado: hallazgos nuevos

### A1. Una asignación de memoria de Vulkan por recurso, sin suballocador (código) — prioridad alta
**Hecho (sin compilar):** VMA 3.3.0 en `CramionFX/vendor/vma` (`VulkanDeviceAllocator.cpp`); `VulkanBuffer` y `VulkanTexture` usan VMA y se liberan en su destructor; anillo de staging persistente de 64 MB (16 MB en modo compatible) en `VulkanDeviceUpload.*`. Queda fuera: `LightmapAtlas`, `FoliagePass` y `TerrainPass` siguen con `findMemoryType` propio.
- `VulkanBuffer::create` hace un `vk::raii::DeviceMemory` por buffer (`VulkanBuffer.cpp:31`). Las texturas, uno por imagen (`VulkanTexture.cpp:75`, `:190`).
- `createDeviceLocal` crea además un staging propio con su asignación y lo destruye (`VulkanBuffer.cpp:46-66`).
- Problemas:
  - `vkAllocateMemory` es lento: microsegundos o milisegundos, con bloqueo del driver.
  - Muchos drivers limitan `maxMemoryAllocationCount` a 4096. Una escena con 2.000 texturas más buffers y stagings se acerca o lo pasa, y entonces falla la subida.
  - Hay fragmentación y no se respeta `bufferImageGranularity` agrupando.
- Arreglo: integrar **VMA** (Vulkan Memory Allocator, MIT, de un solo archivo) en `VulkanBuffer` y `VulkanTexture`, más un anillo de staging persistente.
- Ganancia (est.): cargas un 20-40 % más rápidas y sin fallos por límite de asignaciones. Esfuerzo: M.

### A2. Cada subida espera a la GPU, de una en una, en la cola gráfica (código) — alta
**Hecho (sin compilar):** un `UploadBatch` (un command buffer y una fence) por modelo; cola de transferencia dedicada si existe, con paso de propiedad por semáforo (`CRAMION_VK_NO_TRANSFER_QUEUE=1` la apaga); `uploadNewModels` y `streamModels` instalan el modelo cuando su fence acaba, sin bloquear; las cachés compartidas recuerdan el lote que las sube.
- `VulkanDevice::submitOneTime` (`VulkanDevice.cpp:743-785`) crea una fence, envía a `graphics_queue_` y espera con `waitForFences(UINT64_MAX)`.
- `SkinnedModel::create` lo llama por cada buffer y cada textura. Un modelo con 30 texturas son unas 32 esperas completas, que compiten con el frame en la misma cola y el mismo `queue_mutex_`.
- Arreglo:
  - Agrupar todas las copias de un modelo en un solo command buffer y una sola fence.
  - Usar una **cola de transferencia dedicada** si la GPU la tiene, con transferencia de propiedad entre colas.
  - No bloquear el hilo: comprobar la fence en `uploadNewModels`.
- Ganancia (est.): subidas 3-10 veces más rápidas y menos tirones al hacer streaming. Esfuerzo: M.

### A3. Un hilo del sistema por cada modelo que se sube (código) — media
**Hecho (sin compilar):** modelos en `VulkanDevice::runStreamingTask` (3 hilos, `CRAMION_STREAM_THREADS`; lo que está en cola se cancela sin esperar); clips con `jobs::scheduleIo`; vóxeles con `jobs::schedule` (Background) y `parallelFor`.
- `launchModelJob` usa `std::async(std::launch::async, ...)` (`VulkanRendererSkyMap.cpp:235`). Con libc++ (Android) eso crea y destruye un hilo del SO por modelo. Ya existe `CramionCore/jobs/JobSystem` (lo usan el follaje y el terreno), pero el renderizador no lo usa.
- Lo mismo con los clips (`RenderSync.cpp:1102`) y los vóxeles (`VoxelSystem.cpp:1105-1110`).
- Arreglo: un pool de hilos de streaming en CramionFX, o inyectar el `JobSystem` con prioridad Background. Esfuerzo: S-M.

### A4. Lo estático se recalcula entero en cada frame (código) — alta en mundos grandes
**Casi hecho (sin compilar):** caché por entidad de «modelo resuelto + variante» en `AnimationState` (invalida `resolve_epoch_`; sin vídeo ni tela), esfera del mundo por `Transform::version`, pose de reposo compartida sin `Animator` para lo que no se anima, cajas copiadas del frame anterior si no cambian modelo/revisión/matriz, sin cajas de LOD0 si el LOD es otro, lotes por índice (fuera `batch_lookup_`). **Falta:** el buffer persistente de clústeres en GPU (`GpuCluster` se sigue subiendo cada frame).

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
**Hecho (sin compilar):** `Actor::id` estable (entidad + 1); `previous_actor_motion_` se busca por identidad (cursor y tabla si cambia el orden); un actor que entra o sale solo marca su esfera; `actor_set_reset_` (más de 2048 cambios o la mitad) conserva el redibujo total; `cascade_stale_` para cambios pequeños en las cascadas 2 y 3; solo la vista principal (y los ojos de VR) actualiza el registro; `same_slot` ya no depende de la posición y solo copia huesos (`copyBonesFrom`). Los vectores de movimiento de los huesos también van por identidad (`last_bone_lookup_`, del parche 0010 de Keryan). Queda por índice `tiny_actors_`.
- **Sombras:** el movimiento se compara con `previous_actor_motion_[index]` (`VulkanRenderer.cpp:1790-1797`). Si cambia el número de actores, `actor_set_changed_` (`:1932`) redibuja las 4 cascadas y todas las luces locales. El streaming de residencia cambia ese número sin parar.
- **Animadores:** en `syncActors`, `same_slot` también va por posición (`RenderSync.cpp:3389-3396`). Un objeto que entra al principio hace que **todos** copien su `Animator` entero (`actor.animator = state.animator`).
- Arreglo: comparar por `scene_actor` o por entidad. Un actor nuevo o que sale solo marca su propia esfera. Esfuerzo: S. Es lo primero que hay que hacer para la ciudad Static. Para confirmarlo antes: desactivar `render.streaming.GpuResidency`; si los picos se van, es esto.

### A6. Sombras de lo estático dibujadas por la CPU, objeto a objeto (código) — media-alta
**Parcial (sin compilar):** rejilla XZ de actores (celdas de 8 m) para las luces locales en `recordActorShadows`, y la sombra con otro LOD prueba `world_box` en vez de la esfera. De Keryan: proyectores fuera del trozo de vista barrido hacia el sol (0013), recorte por clúster en cascadas (0015) y terreno por cascada (0016). **Falta:** el camino por GPU (culling por clúster en compute y `drawIndexedIndirectCount` por cascada o cara): necesita compute nuevo, vertex shader de sombra con la matriz por `gl_InstanceIndex`, pipelines y sets, y encajarlo con la caché estática y lo recortado.
- `recordActorShadows` (`VulkanRenderer.cpp:2086-2200`) recorre todos los actores por cada cascada y luz, con culling en la CPU y un `drawIndexed` por tramo.
- Con un LOD de sombra distinto al de la cámara, solo prueba la esfera del actor (`:2145-2155`), sin cajas por submalla.
- `actorsTouch` compara cada luz con todas las esferas (luces × N).
- Arreglo:
  - Mover lo estático al mismo camino que la vista principal: culling por clúster en compute y `drawIndexedIndirectCount`, una vez por cascada o cara.
  - Un BVH o rejilla de actores para las luces.
- Ganancia (est.): 2-6 ms de CPU en escenas con muchas luces con sombra. Esfuerzo: M-L.

### A7. Log por cada modelo subido (código) — baja, pero gratis
**Hecho (sin compilar):** `VulkanDevice::debugLog()`, que solo escribe con `CRAMION_LOG_LEVEL=debug` o `CRAMION_VK_VERBOSE=1`; fuera por defecto los `std::cout` de `SkinnedModel.cpp` (modelo y meshlets) y el del decal.
- Hay `std::cout` en cada `SkinnedModel::create` (`SkinnedModel.cpp:565`) y en cada `buildMeshlets` (`:673`), además de unas 16 líneas más en `VulkanRenderer.cpp`.
- `std::cout` sincroniza con `stdio`. Con streaming son cientos de líneas por minuto desde hilos de subida, y la consola de Windows es lenta.
- Arreglo: pasar al nivel debug del logger, o resumir una línea por lote. Esfuerzo: S.

### A8. El modelo base de una variante se sube y se mantiene aunque nadie lo dibuje (código) — media
**Hecho (sin compilar):** `Scene::setVariantBaseOnly`; RenderSync marca «solo base» el modelo del que sale una variante mientras nadie lo dibuje tal cual; `uploadNewModels`/`uploadModel` lo instalan vacío y fuera de la GPU; `streamModels` lo sube solo cuando una instancia lo dibuja y lo expulsa tras el tiempo de inactividad (también sin streaming). Con esto F2-10 queda completo.
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
| B1 | Suballocador de memoria (VMA) y anillo de staging | **Hecho (sin compilar)** (A1) | Cargas más rápidas, sin el límite de 4096, menos fragmentación | M |
| B2 | Cola de transferencia / async compute | Cola de transferencia **hecha (sin compilar)** (A2); async compute no | Subidas sin parar el frame. Async compute para culling, SSAO o nubes en GPUs que lo solapan | M / L |
| B3 | Compresión de vértices (formato rígido cuantizado) | No (A9) | −50 % de ancho de banda de vértices | M-L |
| B4 | Buffer persistente de estáticos | Parcial: cachés en CPU hechas (A4); los clústeres se suben cada frame | CPU por frame casi constante con lo estático | M |
| B5 | Sombras por GPU (indirect + culling por clúster en cascadas y locales) | Solo en la vista principal; rejilla de luces en CPU hecha (A6) | CPU de sombras ~0 con lo estático | M-L |
| B6 | Caché de sombras estáticas por identidad | **Hecho (sin compilar)** (A5) | Sin redibujar 4 cascadas por streaming | S |
| B7 | Materiales bindless (descriptor indexing) | El dispositivo ya pide `runtimeDescriptorArray` (`VulkanDevice.cpp:475`), pero hay un descriptor set por material y modelo | Menos cambios de set, lotes entre modelos con distinto material, y variantes `.crmat` que serían solo un índice de material | L |
| B8 | Instancia con material propio (sin variante) | No: cada combinación de `.crmat` es otro modelo | Elimina las variantes de raíz (GPU, RAM, RT y tiling). Depende de B7 | L |
| B9 | Grabación de comandos en varios hilos (secondary command buffers) | No | 1-3 ms de CPU en escenas con muchos pases | M-L |
| B10 | Pool de hilos único para el streaming | **Hecho (sin compilar)** (A3) | Menos hilos del SO y prioridades | S-M |
| B11 | Impostores / billboards para lo lejano | No | Árboles y edificios lejanos a 2 triángulos | M |
| B12 | HLOD (fusionar grupos lejanos en una malla) | No (F3-3). El parche 0032 de Keryan (LOD por parches) está pospuesto | Menos draws y clústeres en ciudades | L |
| B13 | LOD de personajes con esqueleto | **Hecho (sin compilar)**: parches 0006, 0040 y 0041 de Keryan (sección 7) | Multitudes | M |
| B14 | Static batching en el editor y en Play | Solo al exportar (`StaticBatching.cpp`) | El editor y Play van como el juego | M |
| B15 | Oclusión Hi-Z | **Ya existe** (`GpuCulling.cpp`); el vidrio también, con `glass_cull.comp` (0034, sin compilar) | — | — |
| B16 | Meshlets / mesh shaders | **Ya existe** (solo rígidos cerrados) | Ampliar a mallas abiertas con culling por cono desactivado | S |
| B17 | Compresión de texturas BC1/BC7 con mips | **Ya existe**; imágenes de la UI, árboles y capas del terreno en BC7 (0028, 0036, sin compilar) | Falta ASTC/ETC2 para Android: hoy se descomprime a RGBA8 en la CPU (`SkinnedModel.cpp:383-389`), con 4-8 veces más VRAM en el móvil | M |
| B18 | Caché de pipelines en disco | **Ya existe** (`VulkanDevice.cpp:999-1053`) | — | — |
| B19 | Streaming de texturas por mips | **Ya existe** (`texture_lod`) | Solo de 2 en 2 niveles. Falta un sampler feedback o una prioridad por pantalla más fina | M |

## 4. Orden recomendado

1. **Ya:** compilar y probar todo lo del 2026-10-19 (secciones 1, 2, 6 y 7), una compilación con `-j 4` y los tests de uno en uno. Riesgos a revisar en `PENDIENTES.md`.
2. **M, mundos grandes:** lo que falta de A4/B4 (clústeres persistentes en GPU) y A6/B5 (sombras por GPU).
3. **Gama baja:** lo que falta de F2-7 y el resto de la fase 2 (sección 6).
4. **L:** B7 y B8 (bindless e instancia con material: quita las variantes de raíz), B3 (vértices), B11-B12 (y con ellos el 0032 de Keryan).

## 5. Cómo medir antes y después

- **CPU:** perfilador del editor, zonas «Recorrer entidades», «Actores y animación», «Subir modelos nuevos» y las de sombras (`CRAMION_SHADOW_MARKS=1`). En la línea `[Rendimiento]` del jugador, los ms de CPU del hilo principal.
- **Memoria:** VRAM de buffers y texturas en el log (`GpuMemoryStats`) y RAM del proceso (Administrador de tareas, memoria privada). Para la sección 1, comparar `test` y Keryan con el commit 5f7a7d3 (la línea «Trazado de rayos listo» da ahora BLAS, modelos que comparten geometría y modelos sin instancias).
- **Carga:** tiempo de «Escena lista» en el log, desde abrir la escena.
- **Escenas:** Keryan (mundo grande, todo con `.crmat`), `test` (exterior y lote estático), BlackHouse (interior) y una ciudad de objetos Static (A4-A5).
- **Tests:** uno a uno, como siempre: `CramionMaterialTests`, `CramionCoreTests`, `CramionOptimizationTests` y `CramionPhysicsTests`. Los tests de `SharedVector` y de que una variante sin tiling comparte los vértices con su base ya están escritos (sin ejecutar).

## 6. Pendiente del plan de gama baja (antes en `PLAN-OPTIMIZACION.md`)

**Fase 1: los 13 escritos.** F1-1, F1-2, F1-3, F1-4, F1-5, F1-7, F1-9, F1-11 y F1-13 están en `main` (2fd1af3 y 5f7a7d3), F1-10 en 31eaff6, y F1-6, F1-8 y F1-12 hechos el 2026-10-19 (sin compilar):

**F1-6. GI horneada sin el filtro SVGF** (S, riesgo bajo) — **hecho (sin compilar)**
- En `recordSsgiPass`, con GI horneada y sin sondas dinámicas (`!dynamicProbesActive()`), `gi_raw_` se copia a `gi_image_` (mismo tamaño y formato; `gi_raw_` lleva `eTransferSrc`) y se saltan el temporal, las 5 pasadas à-trous y la copia de momentos; `gi_filter_history_valid_ = false`. Es la misma rama que usa la 0.1 de `PLAN-LIGHTMAPS.md`.
- Con GI horneada, la palanca Gi tiene tope 0, y vuelve a 255 al dejar de estarlo.
- Ganancia (est.): la GI de ~20 a 2-3 ms en una GTX 1050 a resolución nativa. Si salen costuras, dejar una pasada à-trous.

**F1-8. Sol horneado sin cascadas ni sombras por rayos cuando nadie las lee** (S, riesgo bajo-medio) — **hecho (sin compilar)**
- `sunCascadesNeeded()`: con el sol horneado solo hay cascadas si las lee la volumétrica (con algo que dibujar), el agua, los fluidos o el vidrio (`sceneHasGlass()`). Se calcula una vez por frame (en VR, en el primer ojo) y se usa en la programación de cascadas, `recordShadowPass` y la sombra del sol por rayos. Doc corregida en `iluminacion-horneada.html` (solo aplica a los `.crbake` v1-3: con lightmaps v4 las cascadas del sol se dibujan siempre).
- Falta: el consejo en la ventana de iluminación del editor.
- Ganancia (est.): 7-20 ms de cascadas en gama baja. Riesgo: fugas de luz si se olvida algún lector de las cascadas.

**F1-12. CPU: ríos, vegetación, ondas y flotación sin trabajo repetido** (S-M, riesgo bajo) — **hecho (sin compilar)**
- `water::RiverLine` con caja en xz y `sampleWater` que descarta por la caja; la cinta del río en `shared_ptr<const>`; océano sin muestrear si la cámara está muy por encima.
- `syncFoliage`: la lista de agua solo se rehace si cambia su firma y la comparten todos los bosques (`Foliage::water` es un puntero compartido).
- `updateRipples`: sale si no hay agua a unos 29 m (con el 0012 de Keryan, el océano cuenta por altura), contenedores miembro y repetidos fuera con sort + unique.
- Flotación: nada sin cuerpos dinámicos; línea de cada río en caché por entidad. Test nuevo en `WaterTests` (con y sin caché da lo mismo).
- Ganancia (est.): Keryan, 3-7 ms menos en el PC del autor y 8-18 ms en 4 núcleos.

**Fases 2 y 3** (F2-10 completo con A8; F2-8 hecho; F2-7 y F3-4 en parte o hechos):

| Ítem | Qué es | Esfuerzo | Estado |
|---|---|---|---|
| F2-1 | Aprendizaje del gobernador por tiempo y niveles guardados por PC (hoy oscila a 10-30 FPS) | M | Pendiente |
| F2-2 | Gama por rendimiento real, no solo por VRAM; RT solo con hardware de RT | M | Pendiente |
| F2-3 | Nubes solo donde se ve cielo y calidad Baja | M | Pendiente |
| F2-4 | DOF y motion blur por tiles, a media resolución en Bajo/Medio, sin copias | M | Pendiente |
| F2-5 | Sonda de reflexión a 256×256 sin parar la CPU | M | Pendiente |
| F2-6 | SSGI/SVGF en calidad Baja | M | Pendiente |
| F2-7 | Iluminación y agua en calidad Baja | S-M | Parcial (sin compilar): SSR del agua a 12 pasos y 3 de afinado con iluminación ligera; `lighting_quality` auto/full/lite (0031). Faltan `probeDirection` a 8 pasos, blur del SSAO, sombras de contacto en Medio, tope de luces locales y compartir la copia de escena entre vidrio y agua |
| F2-8 | Distancia de sombras y sesgo de LOD por gama | S | **Hecho (sin compilar)**: `shadowDistanceFor` (60/80/100 m) y `lodScaleFor` (×2 Bajo, ×1,5 Medio), solo en escritorio con presupuesto adaptativo; Bajo y Medio ya no arrancan en Lod 1 |
| F2-9 | LODs en mundos grandes: lote estático, umbral ~1.000 triángulos, caché de LODs | S+S+M | Pendiente; el 0032 de Keryan (LOD por parches) va aquí, pospuesto |
| F2-11 | CPU de mundos grandes, segunda tanda | 5×M | Pendiente |
| F2-12 | Ajustes del editor separados del juego; preset de primera ejecución; menú de opciones integrado | M | Pendiente |
| F2-13 | Gama de CPU y palancas de CPU | M | Pendiente |
| F3-1 | Resolución dinámica real, sin recrear destinos ni `waitIdle` | L | Pendiente (el sesgo de mips ya no depende del presupuesto: sin tirón al cambiar de escala) |
| F3-2 | Sol Mixto con shadowmask horneado (ver `PLAN-LIGHTMAPS.md`) | L | Pendiente |
| F3-3 | LOD por clúster / HLOD (= B12) | L | Pendiente; 0032 pospuesto |
| F3-4 | LODs para personajes con esqueleto (= B13) | M | **Hecho (sin compilar)** (0006, 0040, 0041) |
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

Todos sin compilar. «Adaptado» = reescrito sobre el árbol actual, con los fallos que se vieron al revisarlo corregidos.

| Nº | Qué hace | Estado |
|---|---|---|
| 0001 | Variante `.crmat` con LODs y UV de lightmap | Descartado: ya en 31eaff6 |
| 0002 | Swapchain en Immediate sin vsync ni Mailbox | Aplicado |
| 0003 | Sin bloom ni auto-exposición si están apagados | Adaptado: `composite.frag` solo lee el bloom si está encendido o hay destello; nivel 0 pasado a textura una vez |
| 0004 | Ríos con `shared_ptr` y línea en caché | Descartado: cubierto mejor por F1-12 |
| 0005 | Static batch en celdas | Descartado: no gana nada (CPU +1,4 ms) |
| 0006 | LODs con esqueleto y sombras de animados por cascada | Adaptado: sin «sloppy» con más de 1 hueso; margen de cascada más conservador |
| 0007 | G-buffer: escenario antes que terreno | Aplicado |
| 0008 | Copia de vista solo si alguien la pide; el terreno no fuerza cascadas | Adaptado: firma `TerrainPass::shadowSignature()` (id, alturas, sombra, sitio y trozos) tomada de la vista principal |
| 0009 | MeshCollider grandes en un hilo de fondo | Adaptado: generación de escena, job fallido, urgente si hay cuerpos encima y `flushPendingTerrain` |
| 0010 | Identidad de actores | Solo los huesos de los vectores de movimiento (el resto ya estaba) |
| 0011 | Navegación sin Nav Mesh Bounds no escanea fuentes | Aplicado |
| 0012 | Ondas sin colliders sin agua cerca | Adaptado sobre F1-12 (océano por altura, solo los cercanos, sort + unique) |
| 0013 | Sombras y luces: fuera lo que no se ve | Adaptado: luces completas con RT/path tracing, redibujo forzado al girar (0,22·fovY), barrido con el alcance de la cascada |
| 0014 | Parallax con capas limitadas por píxel | Aplicado |
| 0015 | Sombra barrida por clúster | Reescrito con `submeshBox`/`lod0Box` |
| 0016 | Terreno: cada cascada solo sus trozos | Aplicado |
| 0017 | FSR 1 con cadena LDR corta | Adaptado: transición única de bloom y rayos de luz |
| 0018 | Ritmo de cascadas en gama baja sin presupuesto | Descartado: contradice F2-8 |
| 0019 | Océano sin FFT si no se ve | Aplicado |
| 0020-0022 | El clic sobre la UI no llega al juego (`UI.pointerOverUi`); `UIEventTrigger` y `Entity:uiScreenRect`; `UIInputField` con cursor, selección y portapapeles | Aplicados (falta regenerar el SDK) |
| 0023-0024 | Fecha de las imágenes de la UI cada 1,5 s; canvas raíz con `view<Canvas>` | Aplicados |
| 0025 | `Physics`: `layerMask`, `sphereCast`, `overlapSphere` | Aplicado |
| 0026-0027 | `UIImage` con `uv_rect` y 9-slice; rutas de la UI en caché y zonas del perfilador | Aplicados |
| 0028 | Imágenes de la UI en BC7 si el PSNR ≥ 40 dB | Adaptado: exportación sin `std::regex`, sin BC si la GPU no lo tiene, vuelta a RGBA8 si falla, nombres no ANSI |
| 0029 + 0038 | G-buffer de los opacos sin `discard` (early-Z) | Adaptado: `opaque_albedo` sale de `texture_alpha`; pipeline liberada en `destroy` |
| 0030 | Orden de triángulos para la caché de vértices | Aplicado |
| 0031 | `lighting_quality` auto/full/lite | Aplicado |
| 0032 | LOD por parches para modelos enormes | **Pospuesto** (~400 líneas, riesgo medio-alto; choca con SharedVector, lightmaps y el bucle nuevo de clústeres). Va con F2-9/F3-3 |
| 0033 | Editor en segundo plano sin dibujar la escena | Adaptado: tampoco mientras carga una escena |
| 0034 | Oclusión Hi-Z del vidrio | Adaptado: cajas con `lod0Box`, solo con pirámide del frame (`CRAMION_GLASS_OCCLUSION=0` lo apaga) |
| 0035 | Luces puntuales por teselas | Adaptado: no en los ojos de VR (`CRAMION_LIGHT_TILES=0` lo apaga) |
| 0036 | Árboles y capas del terreno en BC7 con caché en disco | Adaptado: hilo del terreno con join, caché validada, un solo `once_flag` de bc7enc |
| 0037 | Ventana exacta y modo «sin bordes» | Aplicado |
| 0039 | Tope de FPS (`fps_limit`) | Adaptado (encaja con `--run-tests`) |
| 0040 + 0041 | LOD de personajes con atributos y tope de error de 0,5 px | Adaptado: pesos de los atributos corregidos (`wb = 0.5/extent`); respeta la escala de F2-8 |
| 0042 | Sin FXAA encima de TAA, FSR 3 o DLSS | Aplicado |
| 0043 | Escala del Bone Socket bajo un padre escalado | Adaptado (restaura la escala local) |
| 0044 | El cursor vuelve a su sitio al soltarlo | Aplicado |

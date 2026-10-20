# Cramion: hoja de ruta (qué reparar, añadir y mejorar)

Análisis del 2026-10-20 sobre el código de `D:/Trabajo/NewProyects/Cramion` (2.5.1, con lo de 2.5.2 sin compilar). Solo lectura: cada punto se comprobó con grep o leyendo el archivo que se cita. El contexto es portar Overthrow Arena (MOBA) con calidad AAA y dejar el motor listo también para shooters.

No se repite aquí lo que ya está en [`MEJORA.md`](MEJORA.md) (VMA, cola de transferencia, bindless B7, instancia con material B8, vértices comprimidos B3, sombras por GPU B5, impostores B11, HLOD por clúster B12, static batching en editor B14, ASTC B17, F2-12), en [`PENDIENTES.md`](PENDIENTES.md) (compilar y probar lo del 2026-10-19, restos menores) ni en [`PLAN-LIGHTMAPS.md`](PLAN-LIGHTMAPS.md). Tampoco lo que otros agentes están reparando ahora (streaming de GPU, memoria, duplicados, sombras de contacto); solo se señala lo que queda de esos temas.

Tamaños: **S** (horas a 2 días), **M** (1-2 semanas), **L** (3-6 semanas), **XL** (meses). Prioridades: **P0** cierre o corrupción, **P1** bloquea un juego AAA o el rendimiento, **P2** calidad o productividad, **P3** deseable.

---

## 1. Resumen

El motor ya está muy por encima de lo habitual en un motor propio: diferido Vulkan 1.3 con modo compatible, PBR de Disney completo, PCSS, RT (sombras, reflejos, DDGI, path tracing), FSR 3.1 y DLSS 4, océano FFT, follaje en GPU, Hi-Z, meshlets, VFX Graph en GPU, World Partition con HLOD, Jolt (ragdoll, vehículos, tela, fractura), Recast/Detour con multitudes, servidor dedicado con compensación de lag, Motion Matching, Enhanced Input y un editor completo con MCP. Lo que falta no son técnicas sueltas, sino **robustez** y **arquitectura para escenas grandes**:

1. **Cierres sin red de seguridad (P0):** `ErrorDeviceLost` y las excepciones en el JobSystem acaban en `std::terminate` sin diagnóstico de GPU y sin guardar la escena. Falta autoguardado y una Release con símbolos.
2. **El editor no escala:** deshacer, Play/Stop y abrir un prefab serializan y reconstruyen el mundo entero, y varios recorridos por frame visitan las 30-40 mil entidades. El editor además carga todas las celdas de World Partition. Esto explica gran parte de los 200-300 ms/frame.
3. **Flujo de assets:** lo editado en disco no se recarga, mover o renombrar rompe referencias sin avisar, `create_model_materials` empareja mal los nombres genéricos y el importador de FBX falla con piel sin tomas o con una toma rota.
4. **Prefabs y spawn:** no hay prefabs anidados ni variantes, instanciar parsea JSON cada vez, no hay pool y desactivar una entidad destruye su script C++.
5. **Coste de los scripts C++:** un viaje IPC por instancia y evento, y la API generada viaja por JSON y espera respuesta incluso en las funciones `void` (`setQuaternion`).
6. **Render para juego:** tope de 32 luces puntuales y 8 focos, transparencia solo de vidrio, `.crshader` sin color ni profundidad de escena, sin color de vértice ni parámetros por instancia, sin máscara reactiva para FSR/DLSS, VFX sin ribbons ni mallas.
7. **Animación de gameplay:** no hay capas con máscara, aditivas, root motion, eventos (notifies) ni montages.
8. **Plataforma y seguridad:** no hay CI ni CTest, el MCP no pide token (cualquier proceso local puede compilar y ejecutar C++), el mando no funciona en PC y el `.crpack` se descomprime entero al primer arranque.

---

## 2. Reparar primero (P0 / P1)

Lo que más rinde por esfuerzo, en orden de ataque. Lista completa por área en la sección 3.

| # | Título | Área | Dónde | Tam. | Por qué |
|---|---|---|---|---|---|
| 1 | ✅ `ErrorDeviceLost` sin captura, diagnóstico ni guardado (**P0**) | Render | `VulkanRenderer.cpp` drawFrame (:5280, :5646, :5660); `CramionEditor/src/main.cpp:264`; `VulkanDevice.cpp:893-913` | M | Solo `waitIdle` lo captura; en `drawFrame` sube al catch final, que imprime «Error fatal» y sale. Sin `VK_EXT_device_fault` ni marcadores no se sabe qué pase colgó la GPU. **✅ 2026-10-20 (R1):** causa del `--rt 0` hallada con `VK_EXT_device_fault` (la sonda dibujaba la vegetación sin recortarla) y arreglada; informe en `%LOCALAPPDATA%/Cramion/Crashes/DeviceLost-*.txt`, `setDeviceLostHandler` (autoguardado del editor) y esperas con tope. |
| 2 | ✅ Excepciones en las tareas del JobSystem = `std::terminate` (**P0**) | Plataforma | `CramionCore/src/jobs/JobSystem.cpp` run() y workerLoop() | S | No hay ningún `catch`; además el contador del grupo no baja y quien espera en `waitFor` se cuelga. `set_terminate` no está puesto en los workers. **✅ 2026-10-20 (R1):** `try/catch` con guarda RAII del contador, `wait`/`parallelFor` relanzan, nombre de tarea y `set_terminate` por trabajador. |
| 3 | ✅ Autoguardado y recuperación de escena | Editor | `CramionEditor/src` (no hay nada: grep `autosave` = 0) | S | Con los cierres de los puntos 1 y 2 se pierde todo desde el último Ctrl+S. La instantánea `current_state_` ya está en memoria. **✅ 2026-10-20 (R1):** `Autosave.cpp`: cada 2 min en un hilo, de emergencia (SEH, terminate, abort, DeviceLost) y modal de recuperación. |
| 4 | ✅ Release sin símbolos (sin PDB) | Plataforma | `CMakeLists.txt`, `build-release` (`-O3 -DNDEBUG`), `cmake/PackageRelease.cmake` | S | Los minidumps solo dan módulo y desplazamiento. Solución: `-gcodeview` + `/DEBUG:FULL` y PDB archivados por versión. **✅ 2026-10-20 (R1):** `CRAMION_RELEASE_PDB=ON`; `cramion_package` archiva los PDB en `symbols/<versión>/` y en su zip. |
| 5 | ✅ MCP sin token: ejecución de código desde cualquier proceso local | Plataforma | `CramionEditor/src/McpServer.cpp:45-56, 164` | S | Acepta peticiones sin Origin; `write_file` + `cpp_compile` + `play` = código nativo. Token por sesión en el perfil, enviado por McpBridge. **✅ 2026-10-20 (R1):** token por sesión en `%LOCALAPPDATA%/Cramion/mcp-token-<puerto>`, Host local obligatorio y casilla «Exigir token MCP». |
| 6 | ✅ FBX con piel pero sin tomas se importa como estático | Núcleo | `CramionCore/src/asset/Importer.cpp:658-676` | S | `animated` depende solo de `mNumAnimations`; debe contar también `aiMesh::HasBones()`. Además se lee el FBX dos veces. **✅ 2026-10-20 (R1):** animado con 2 o más huesos; la lectura de comprobación ya no lee materiales, texturas, luces ni cámaras (sigue siendo doble). |
| 7 | 🟡 FBX con una toma rota: se rechaza entero | Núcleo | `CramionFX/src/asset/ModelLoader.cpp:1059-1074` | S→L | Reintentar sin `ValidateDataStructure` y quitar las pistas inválidas (S); ufbx como cargador de FBX (L), como hizo Godot 4.3. **🟡 2026-10-20 (R1):** hecha la parte S (relectura sin validar + `repairAnimations`, también en `loadAnimationClips`). Falta la L: ufbx. |
| 8 | ✅ `create_model_materials` asigna texturas equivocadas | Núcleo | `CramionCore/src/asset/ModelMaterials.cpp:166-180, 509-547` | S | Coincidencia por subcadena de 3 caracteres y búsqueda en todo el proyecto. Faltan una lista de nombres genéricos (`Material`, `lambert1`, `standardSurface1`, `Default`) y no salir de la carpeta del modelo. **✅ 2026-10-20 (R1):** lista de genéricos, palabras completas y búsqueda en la carpeta del modelo + las `Textures` de sus padres (no todo `Assets/`). |
| 9 | 🟡 Recarga de assets editados en disco + rescan incremental | Núcleo/Editor | `EditorProject.cpp:91-133`; `AssetDatabase.cpp:70-150`; `RenderSync.cpp:269` | L | Solo se recargan prefabs y `.crshader`; `.crmat` solo por MCP; texturas, `.crdata`, `.cranim` y FBX nunca. Cada cambio recorre todo `Assets/`. **🟡 2026-10-20 (R2):** `assets::FileWatcher` (ReadDirectoryChangesW / inotify), rescan solo de las rutas avisadas en un hilo (`scanPaths`/`applyScan`) y recarga de `.crdata`, `.crmat`, texturas de los `.crmat`, `.cranim`, controladores, Motion Matching, prefabs, `.crshader` y vídeos sin `waitIdle`. Falta reimportar al cambiar el FBX, imágenes de UI/sprites/decals, capas de `.crterrain` y macOS. |
| 10 | ✅ Deshacer por instantánea JSON del mundo entero | Editor | `EditorApp.cpp:645-760`; `SceneSerializer.cpp:185, 231` | L | Cada acción serializa todo y cada Ctrl+Z reconstruye todas las entidades. Pasar a diferencias por entidad y componente. **✅ 2026-10-20 (R2):** `EditorHistory.cpp`: instantánea por entidad, huellas en paralelo por pool de EnTT y pasos con solo lo cambiado; Ctrl+Z aplica solo esos componentes (0,22 ms un paso entre 10 000 entidades). |
| 11 | ✅ `recordPrefabOverrides` en cada paso de deshacer | Editor | `EditorPrefabs.cpp:204-215`; `Prefab.cpp:282-330` | M | Vuelve a parsear cada `.crprefab` y serializa cada instancia por acción: O(instancias × entidades). **✅ 2026-10-20 (R2):** solo en las instancias tocadas, con el `.crprefab` parseado en caché por ruta, fecha y tamaño. |
| 12 | ✅ La física sincroniza todos los colliders cada frame en edición | Editor | `EditorPhysics.cpp:514-515`; `PhysicsSystem.cpp:2952-3060` | M | Ordena y firma cada collider sin mirar `Transform::version`. Suma ms por frame sin mover nada. **✅ 2026-10-20 (R2):** sincronización incremental (`physics.IncrementalEditSync`): lista rehecha solo con la estructura, `Transform::version`, `touchEdit`/`touchAllEdit` y tanda rotatoria de 256. Falta medirla en el editor con Ashbound. |
| 13 | ✅ El editor carga todas las celdas de World Partition | Núcleo/Editor | `CramionCore/include/CramionCore/world/WorldPartition.h:2-30` | L | Causa directa de los 200-300 ms/frame. Falta elegir qué regiones cargar (como las Loaded Regions de Unreal 5). **✅ 2026-10-20 (R2):** regiones cargadas: radio de la cámara, celdas fijadas y lo seleccionado, carga y borrado por trozos, guardar lo descargado y Play que hereda. Pendiente: `UndoHistory::absorb` para no calcular huellas del mundo entero en cada frame con celdas entrando. |
| 14 | 🟡 `waitIdle()` en caminos de juego | Render | `VulkanRenderer.cpp` :1319, :2181, :4424, :8210, :8395, :8404, :8999; `VulkanRendererRayScene.cpp:307` | M | Decals, `.crshader` recién compilados, Render Textures, UI y escena de rayos paran la GPU. Es lo que queda del tema del streaming: cola de retirada por fence (ya existe `retired_models_`). **🟡 2026-10-20 (R1):** `VulkanDevice::retireAfterGpu` (cola por fence) en decals, `.crshader`, UI y crecimiento del culling en GPU y del vidrio. Faltan: Render Textures (ahora `waitFramesInFlight` y solo si un material la usa; sin espera necesita sets de material por frame), escena de rayos (`buildRayTracingScene` y su liberación tras 10 s) y `applyShadowCache`. |
| 15 | ✅ Desactivar una entidad destruye su script C++ | Scripting | `CramionCore/src/scripting/CppScripts.cpp` ~1736-1754 | M | Sin OnEnable/OnDisable el pool de objetos (minions, proyectiles) es imposible. **✅ 2026-10-20 (R1):** `onEnable`/`onDisable`; desactivar conserva la instancia; un `activeInHierarchy()` por script. |
| 16 | ✅ Instanciar parsea JSON y no hay pool | Núcleo | `CramionCore/src/ecs/PrefabSpawn.cpp:20-35, 94-99` | M | Parte de los tirones al instanciar muchos prefabs. Plantilla binaria compilada y `Scene.pool/release`. **✅ 2026-10-20 (R3):** `ecs::PrefabTemplate` (cintas binarias por componente, referencias internas como índices, compilada en los hilos de IO) para `Scene.instantiate`, `instantiateAsync` y el pool: 9-15 veces más rápido (913 entidades: 6,3 → 0,7 ms). `Scene.pool/acquire/release/poolCount` con onEnable/onDisable. |
| 17 | 🟡 Un viaje IPC por instancia y por evento | Scripting | `CppScripts.cpp` ~2589-2640, ~1194, ~772 | M | 500 entidades con script = más de 1500 cambios de contexto por frame aunque `update` esté vacío. **🟡 2026-10-20 (R1):** `Op::CallMany`, un viaje por fase (fixedUpdate, colisiones, start+update+lateUpdate, cierre). Falta saltar los virtuales no sobrescritos y juntar callbacks, mensajes y `Create`. |
| 18 | ✅ API generada por JSON, síncrona incluso en `void` | Scripting | `sdk/cramion/Script.h`, `ScriptMain.cpp:160`, `NativeApi.cpp:508`, `tools/SdkGen.cpp` | L | `setQuaternion`, `playAnimation`, `setText`... cuestan decenas de µs. Índice numérico, codificación binaria, `void` en lote y `SetTransforms`. **✅ 2026-10-20 (R3):** `ApiResolve` (id por clave y sesión) + `ApiCall` binario (`NativeApi::bridgeBinary`); set y funciones sin «Devuelve» en lote (`ApiCallSend`); `Entity::setQuaternion` y `Scene::setTransforms` en lote (`SetTransforms`). Motor: `setQuaternion` 3,97 → 0,21 µs. Falta el modo en el mismo proceso para el juego exportado (3.3). |
| 19 | ✅ Llamar a la API desde otro hilo corrompe el canal IPC | Scripting | `CramionEditor/host/ScriptHost.cpp:85, 180-188`; `ScriptMain.cpp` | S | Sin mutex ni comprobación de hilo: respuestas cruzadas. Como mínimo, error claro fuera del hilo principal. **✅ 2026-10-20 (R1):** el SDK y `CramionScriptHost` no envían desde otro hilo, devuelven vacío y avisan en la Consola. |
| 20 | ✅ Mando no funciona en Windows ni Linux | Scripting/Input | `CramionDM/src/Window.cpp`, `WindowX11.cpp`, `Input.cpp` | M | Solo `WindowAndroid.cpp` emite eventos de gamepad. Falta XInput/GameInput (o SDL), varios mandos y vibración. **✅ 2026-10-20 (R3):** XInput (DLL al vuelo) y DS4/DualSense por HID en Windows, evdev con inotify en Linux; 4 mandos en caliente, vibración, zona muerta radial y dispositivo activo. Faltan GameInput, HID genéricos en Windows, vibración en Android y compilar `WindowX11.cpp` en Linux. |
| 21 | ✅ Mover o renombrar archivos rompe referencias en silencio | Editor | `EditorProject.cpp:701-718, 1240-1248` | M | Audio, imágenes de UI, scripts y escenas por nombre quedan apuntando a la ruta vieja; solo se arreglan los VideoPlayer de la escena abierta. **✅ 2026-10-20 (R2):** `assets::rewriteProjectReferences` reescribe las rutas en todos los JSON del proyecto, en la escena abierta (por reflexión) y en las otras pestañas. Siguen sin UUID: rutas dentro del código `.cpp` y escenas por nombre. |
| 22 | No hay CI ni CTest | Plataforma | raíz (sin `.github/`), todos los `CMakeLists` (sin `add_test`) | L | ~60 ejecutables de tests a mano; Android y shaders compatibles se compilan a mano. Las regresiones se ven al jugar. |

---

## 3. Por área

Cada ítem: **prioridad · tamaño** — dónde — por qué. Comparación con Unreal 5 (UE5), Unity 6 y Godot 4 cuando aporta.

### 3.1 Render y gráficos (CramionFX)

#### Reparar

- ✅ **ErrorDeviceLost sin recuperación ni diagnóstico** — P0 · M — ver tabla, punto 1. *Hecho 2026-10-20 (R1): `VK_EXT_device_fault` siempre activo, marcas NV, informe en disco y aviso al editor. No se recupera el dispositivo (se guarda y se sale).* Capturar en `drawFrame` y en los hilos de subida; marcadores por pase (`vkCmdWriteBufferMarker2AMD` / NV checkpoints) volcados junto con `VK_EXT_device_fault`; diálogo y guardado de emergencia. UE5 tiene GPU breadcrumbs y Aftermath.
- 🟡 **`waitIdle()` en caminos de juego** — *R5: fuera de la caché de sombras, la escena de rayos, la TLAS y las Render Textures; decal en un hilo. Quedan sombras locales, cambio de resolución y filtrado de texturas.* — P1 · M — *R5: fuera de la caché de sombras, la escena de rayos, la TLAS y las Render Textures; decal en un hilo. Quedan sombras locales, cambio de resolución y filtrado de texturas.* — ver tabla, punto 14. *2026-10-20 (R1): decals, `.crshader`, UI y culling sin `waitIdle`; faltan Render Textures, escena de rayos y caché de sombras (y el stbi del decal sigue en el hilo principal).* La carga de la textura del decal además se hace con stbi en el hilo principal.
- ✅ **LOD automático que destruye el follaje** — P1 · M — *2026-10-20 (R1): los materiales con recorte por alfa ya no usan Prune ni sloppy (briznas: 6000 triángulos en todos los niveles; antes 187). Falta `simplifyWithAttributes` en los rígidos, el ajuste por modelo, importar `_LODn` y el crossfade. R5: `simplifyWithAttributes` en rígidos, LOD por modelo (Apagado/Auto/A mano), `_LODn` del archivo y fundido con tramado (sin él en personajes, sombras, mesh shaders y compat).* — `CramionFX/src/asset/MeshLod.cpp:144-149` (`meshopt_SimplifyPrune`), :236-245 (`simplifySloppy`); `AssetManager.cpp:346`. Prune quita las cartas de hoja y sloppy suelda por celdas, sin excluir submallas `alpha_masked`. Los rígidos no usan `simplifyWithAttributes` (UV y normal). Falta un ajuste de LOD por modelo (off/auto/manual), importar `_LOD0.._LODn` y dither crossfade. UE5 y Unity 6 tienen LOD por malla e importan LODs del FBX.
- **FSR 3 y DLSS sin máscara reactiva** — P1 · M — `TemporalUpscalers.cpp/.h:39-55`; `VulkanRenderer.cpp:6209-6252`. Partículas, VFX, fuego y vidrio entran en el color antes del escalado y sin máscara: estelas y fantasmas en chispas y efectos de habilidades. Escribir un R8 reactivo y pasarlo a `ffxDispatch` y a NGX; usarlo también en el TAA propio.
- ✅ **Ruido de `.crshader` con facetas** — P2 · S — *Hecho 2026-10-20 (R3): gradiente con derivadas, simplex, fbm, ridged, voronoi y curl con hash PCG; `noise()` igual. Falta compartir el voronoi con el Shader Graph.* — `CramionFX/shaders/surface_common.glsl:49-80`. Ruido de valor con hash `fract(p*123.34)`: celdas visibles, derivada discontinua y bloques con coordenadas grandes. Añadir simplex/gradiente con derivadas, voronoi compartido con Shader Graph y hash de enteros (PCG); mantener `noise()`.
- ✅ **`vertex()` de `.crshader` sin sombra y con vectores de movimiento mal** — P2 · S — *Hecho 2026-10-20 (R3): pipeline de sombra propia (cascadas, luces locales, lluvia y cielo) y frame anterior con `TIME - DELTA_TIME`. Falta escribir la sombra en el juego exportado (`EditorExport.cpp`).* — `CramionFX/shaders/surface.vert:108-134`. La sombra no sigue el desplazamiento y en el frame anterior se reusa el mismo `offset`: banderas y olas por `TIME` dejan estelas con TAA/FSR/DLSS. Variante de sombra con `vertex()` y evaluación con `TIME - dt`. UE5 (WPO con Output Velocity) y Godot lo hacen bien.

#### Añadir

- ✅ **Transparencia general iluminada** — P1 · L — *2026-10-20 (R3): `.crmat` con `"blend"` alpha/additive/glass iluminado (sol con sombra, luces principales y de clúster, sonda), emisión con bloom en todos, orden de atrás adelante, `"unlit"` en `.crmat` y `domain transparent` (unlit) en `.crshader`. Faltan niebla en lo transparente, normal map en el vidrio, luces en el `domain transparent` y Unlit/Transparente en el Shader Graph y en modo compatible. R5: niebla en lo transparente, normal map en el vidrio, `domain transparent` iluminado y dominios en el Shader Graph y en compat.* — `CramionFX/shaders/glass.frag:375-378`; `Model.h`. Lo único transparente es el vidrio: sin emisión (`push.emissive` no se usa), sin normal map, sin luces locales ni niebla, y los `.crshader` no tienen modo transparente. Faltan escudos, hologramas, cristales de maná, agua estilizada. Pasada forward ordenada con blend alfa/aditivo/premultiplicado, emisión, luces y niebla, más dominios «Transparente» y «Unlit» en `.crshader` y Shader Graph.
- ✅ **Color y profundidad de escena en `.crshader` y Shader Graph** — P1 · M — *2026-10-20 (R3): `sceneColor`, `sceneDepth`, `sceneLinearDepth`, `sceneWorldPosition`, `sceneEmission`, `depthFade` y `SCREEN_UV` con `domain transparent`. Faltan los nodos del Shader Graph y el buffer de distorsión para partículas. R5: nodos «Escena» del Shader Graph y partículas de distorsión.* — `surface_common.glsl:25-39` (struct Surface); `ShaderGraph.cpp`. Sin `SCREEN_UV`, `SCENE_COLOR` ni `SCENE_DEPTH` no hay distorsión, refracción ni fundido de orilla; las partículas tampoco distorsionan. La copia de escena ya existe para el vidrio. Va dentro del dominio transparente del punto anterior, más un buffer de distorsión para partículas. UE5, Unity 6 y Godot (`hint_screen_texture`) lo dan.
- ✅ **Color por vértice** — P1 · M — *Hecho 2026-10-20 (R3): COLOR_0 en RGBA8 aparte (caché v13, `.crdata`, batching, UV2), `VERTEX_COLOR`, nodo del Shader Graph y `"vertex_color"` en `.crmat`. Hay que reimportar los modelos; falta `RuntimeMesh.colors` y la interfaz en el editor.* — `Model.h:24-35` (SkinnedVertex); `ShaderGraph.cpp:139-141, 751` (el nodo devuelve blanco). Muchos packs llevan viento, AO o tinte en el color de vértice. Flujo RGBA8 aparte (para no engordar los 80 B), importado con Assimp y expuesto como `VERTEX_COLOR` y en `RuntimeMesh`.
- ✅ **Parámetros de material por instancia** — P1 · M — *2026-10-20 (R3): `MeshRenderer.tint`, `alpha` (disolución por tramado), `emission_scale` y `receive_decals` en el registro por instancia, sin romper el instancing. Faltan `INSTANCE_DATA(i)` en `.crshader`, `entity.setMaterialParam()`, el tinte por entidad en las partículas y la opacidad en las sombras. R5: `INSTANCE_DATA(0..1)`, `setMaterialParam()`, tinte/alfa en partículas y opacidad en sombras (falta `INSTANCE_DATA` en `vertex()` y su nodo).* — `Components.h:98-114` (MeshRenderer); `EntityApi.gen.inc:576-581`. Color de equipo, destello de golpe, disolución y resaltado obligan a otro `.crmat` (y otra variante). Es una versión ligera de B8 de MEJORA (sin esperar a bindless): 2-4 `vec4` por instancia junto a `bone_offset`, `INSTANCE_DATA(i)` en `.crshader` y `entity.setMaterialParam()`. Incluir un tinte/alfa global por entidad en las partículas (hoy solo `color_start/end` y `alpha_start/end` por sistema, `physics/Particles.h:63-66`). UE5 Custom Primitive Data, Unity MaterialPropertyBlock, Godot instance uniforms.
- **Personajes animados en la escena de rayos** — P1 · L — `VulkanRenderer.cpp:1803-1805, 2152-2172`. Solo entran los rígidos de un hueso: reflejos, GI por rayos y path tracing no ven a los héroes. Skinning previo por compute (reutilizable en G-buffer y sombras, ahorra trabajo) y BLAS con refit.
- **Outlines de juego, capas de render y arma en primera persona** — P1 · M — `VulkanRenderer.h:518-521` (setOutlinedActors, solo editor); `Components.h` (Camera sin máscara). Faltan silueta a través de paredes (MOBA), bitmask de capas en MeshRenderer y Camera, y modo «viewmodel» con FOV propio. UE5 Custom Depth/Stencil, Unity Rendering Layers, Godot `cull_mask`.
- ✅ **VFX Graph: ribbons, mallas, eventos y subemisores, luz real** — P1 · L — *2026-10-20 (R3): salidas «Cinta» y «Malla 3D» y eventos en la GPU (al morir y al chocar: otro `.crvfx`, un nivel). Falta la luz real (luces locales en el humo). R5: luz real (sol con sombra, principales y clúster).* — `VfxPass.h:89-90`; `vfx.frag:54`; `vfx_common.glsl`. Solo quads; `VFX_LIT` multiplica por una luz. Estelas de espada y proyectil, casquillos, impacto que lanza otro sistema y humo con luces locales. Niagara, VFX Graph de Unity y GPUParticles3D de Godot lo traen.
- **Baja latencia (Reflex / Anti-Lag 2 / present_wait)** — P1 · M — `drawFrame`; `VulkanSwapchain.cpp`; `VulkanCommon.h:61`. Necesario en shooters y para la generación de frames. UE5 y Unity 6 tienen Reflex.
- **Sondas de reflexión colocables con caja** — P2 · M — `ReflectionProbe.h:16-46`; `lighting.frag:73`. Una sola sonda pegada a la cámara; en gama media es la fuente principal de reflejos. Array de sondas horneadas con paralaje de caja.
- ✅ **Herramientas de depuración de GPU** — P2 · S — *2026-10-20 (R1): etiquetas de debug utils en `markPass` con la validación activa; faltan los nombres de recursos y `CRAMION_VK_SYNC_VALIDATION` / `CRAMION_VK_GPU_AV`. R5: nombres de recursos y `CRAMION_VK_SYNC_VALIDATION` / `CRAMION_VK_GPU_AV` (faltan nombres en pipelines gráficas y texturas de modelos).* — `VulkanInstance.cpp:26-90, 166`; `markPass`. Etiquetas `vkCmdBeginDebugUtilsLabelEXT` en `markPass`, nombres de recursos y `CRAMION_VK_SYNC_VALIDATION` / `CRAMION_VK_GPU_AV`. Ayuda directamente a cazar los DeviceLost.
- **Salida HDR10/scRGB** — P2 · M — `VulkanSwapchain.cpp:101, 170-185` (solo `eSrgbNonlinear`). El motor ya renderiza en HDR con AgX.
- **Piel con subsurface en pantalla y modelo de ojo** — P3 · M — `disney_brdf.glsl`; `lighting.frag:1531`. Para primeros planos (selección de héroe, tienda).
- **XeSS y generación de frames** — P3 · M — `GraphicsSettings.h:22`. Depende de la baja latencia.
- **LUT 3D importable (.cube)** — P3 · S — `composite.frag:15-49`.

#### Mejorar

- ✅ **Tope de 32 luces puntuales y 8 focos** — P1 · L — *Hecho 2026-10-20 (R3): las 32+8 principales como antes y el resto por clústeres (16×9×24) en `lighting.frag` y el vidrio, sin tope (red en 16 384), sin `waitIdle` ni memoria por frame. Las de clúster usan su mapa de sombra si tienen hueco; faltan sombras por rayos, niebla volumétrica, agua y VFX para ellas y el atlas de sombras por prioridad.* — `Light.h:13-14`; `RenderSync.cpp:4052-4053` (keepNearest); `lighting.frag:81-87`; `LocalLightShadows.h:20-21`; `volumetric.frag:45-46`. Habilidades, torretas y fogonazos compiten por 32 plazas y las luces aparecen y desaparecen al mover la cámara. SSBO sin tope, clústeres 3D y atlas de sombras por prioridad. UE5/Unity HDRP usan clústeres con miles de luces; Godot Forward+ 512 por defecto.
- ✅ **Escena de rayos incremental** — P1 · L — *2026-10-20 (R4): un modelo que se dibuja sin estar en la escena se apunta y entra en la siguiente reconstrucción aunque ya no se dibuje, y la TLAS se crea con la mayor capacidad ya usada (antes empezaba en 256 y crecía con un `waitIdle` en cada paso). Banco con rayos: de 7 reconstrucciones seguidas de 100-260 ms por ciclo a 2 en total (ciclos 2 y 3, la primera vez que entra cada modelo). Falta lo de fondo: BLAS por modelo al subirlo, sin reconstrucción entera. R5: cada modelo entra solo con su BLAS en el frame, sin reconstrucción entera ni `waitIdle` (queda la reconstrucción al editar un modelo ya en la escena).* — `VulkanRendererRayScene.cpp:305-420`; `RayTracing.cpp:391-433`. Lo que entra por streaming no aparece en los rayos hasta la reconstrucción total (cada 4 s, 100-1000 ms con `waitIdle`). BLAS por modelo al subirlo y TLAS por frame.
- ✅ **Captura de la sonda sin esperar al frame anterior** — P2 · M — *Hecho 2026-10-20 (R4b): tercer hueco de frame solo para la sonda (`kProbeFrameSlot`); la cara se envía sin esperar y el cubo se cierra en `finishProbeCapture` cuando acaba su fence. Banco `--rt 0`: p99 17,5 → 12,4 ms, frames > 16,7 ms ~170 → 8, ninguno por la sonda (con vsync 224 → 4).*
- ✅ **Decals** — P1 · M — *2026-10-20 (R3): solo los que tocan la vista, por teselas de pantalla, y «no recibir decals» por objeto. Faltan el atlas, los canales normal/ORM/emisión y subir de 64. R5: atlas, canales normal/ORM/emisión y 256 decals (sin recorte por clúster 3D).* — `gbuffer_surface.glsl:14-43, 199-230`; `GpuTypes.h:311-327` (`kMaxDecals = 64`). Bucle sobre todos los decals en cada píxel, sin normal ni emisión, y el indicador de AoE pinta a los personajes. Decals por clúster, atlas, canales normal/ORM/emisión, «no recibir decals» por objeto.
- **Niebla en froxels para transparentes** — P2 · L — `volumetric.frag`; `vfx.frag`, `particle.frag`, `glass.frag`, `sprite.frag` sin niebla. Todo lo transparente brilla a través de la niebla. Volumen 3D compartido como en UE5, HDRP y Godot.
- **GTAO con normales dobladas y oclusión especular** — P2 · M — `ssao.frag:1-16`. XeGTAO (MIT).
- **Terreno: agujeros, triplanar, mezcla por altura** — P2 · M — `terrain.frag`; `Terrain.h:38` (8 capas). Agujeros para cuevas y caminos, sin estiramiento en acantilados.
- **HLOD de World Partition con texturas horneadas** — P2 · M — `ProxyMesh.h:1-30`; `WorldPartition.cpp:140-152, 1201`. Hoy un color medio por grupo de materiales («plastilina»). Distinto de B11/B12 de MEJORA: es lo que le falta al HLOD que ya existe.
- **Orden entre familias de transparentes / OIT** — P3 · L — `VulkanRenderer.cpp:6209-6240`. El humo detrás de una ventana se pinta encima del vidrio.

### 3.2 Núcleo y runtime (ECS, prefabs, assets, animación, física, navegación, audio, red)

#### Reparar

- ✅ **FBX con piel sin tomas importado como estático** — P1 · S — *Hecho 2026-10-20 (R1).* — tabla, punto 6. Unity, Unreal y Godot crean el esqueleto siempre que hay piel.
- 🟡 **FBX con toma rota rechazado entero** — P1 · S→L — *2026-10-20 (R1): reparación hecha; falta ufbx. R5: ufbx sin añadir (2-4 semanas con pruebas de paridad).* — tabla, punto 7.
- ✅ **`create_model_materials` con nombres genéricos** — P1 · S — *Hecho 2026-10-20 (R1).* — tabla, punto 8. Unity deja elegir el ámbito de la búsqueda (carpeta local, recursivo hacia arriba, todo el proyecto).

#### Añadir

- ✅ **Recarga en caliente de assets editados en disco** — P1 · L — *2026-10-20 (R2): recarga por tipo y rescan incremental; falta reimportar si cambia el origen. R5: reimportar modelos al cambiar el original (sin skyboxes ni cambios con el editor cerrado).* — tabla, punto 9. Recarga por tipo (textura, malla, clip, material) y reimportación si cambia el origen, con su opción. Unity Auto Refresh, UE5 Auto Reimport, Godot al recuperar el foco.
- **Prefabs anidados y variantes** — P1 · L — `Prefab.h:24`; `Prefab.cpp:172` (flattenNested), :227, :412. Héroes con skins, minions por facción y piezas reutilizadas obligan a duplicar. Unity Nested/Variants, UE5 Child Blueprints, Godot escenas heredadas.
- **Animator con capas, máscara de huesos y aditivas** — P1 · L — `AnimatorController.h`; `CramionFX/anim/Animator.h` (evaluateBlend). Lanzar o apuntar con el torso mientras se corre, retroceso, aim offsets.
- **Root motion** — P1 · M — `Components.h:120-142`; `RenderSyncMotion.cpp:269`. Ataques con avance, esquivas, saltos de habilidad.
- **Eventos (notifies) y curvas en los clips, con ventana de clips en el editor** — P1 · M — `Model.h:224-235`; `EditorAnimator.cpp` (solo el grafo). El fotograma del golpe y el cast point de un MOBA se sincronizan con esto. Incluye una ventana para previsualizar y recorrer un clip y marcar eventos.
- **Buses de audio y volumen maestro, de música y de efectos** — P1 · M — `audio/Audio.h:38-66, 109-190`; `CoreApi.cpp:408-444`. Ni siquiera hay deslizadores para el menú de opciones; después, ducking e instantáneas. Unity AudioMixer, UE5 Submix, Godot AudioServer.
- **Red autoritativa con predicción y reconciliación** — P1 · XL — `net/Network.h:1-45`; `NetworkObject.h`. El dueño es la autoridad de su posición: trampas y mala sensación con latencia. UE5 CharacterMovement con predicción, Unity Netcode for Entities.
- **Montages / slots de animación puntual** — P2 · M — depende de capas y eventos.
- **Morph targets** — P2 · L — `ModelLoader.cpp`, `Model.h`, `SkinnedPass.cpp` (no se lee `mAnimMeshes`).
- **Opciones de importación: cortar clips, bucle, esqueleto compartido, LODs y colisión UCX** — P2 · M — `Importer.h:43-67`. Los `_LODn` resuelven también el problema del follaje.
- **Carga de escenas asíncrona y aditiva, entidades persistentes** — P2 · M — `CoreApi.cpp:102-111`; `PlayerMain.cpp:1209-1260`. HUD, lobby, precargar la siguiente partida.
- **Grafo de dependencias de assets** — P2 · M — `AssetDatabase.h`; reutilizar `DataPack.h:59` (collectDependencies). Base para exportar solo lo usado (3.5), el visor de referencias y el borrado seguro (3.4).
- **Seguimiento y presupuesto de RAM (incluido Android)** — P2 · M — `AssetManager.h:127-147` (`models_` sin expulsión); `profiling/`; `AndroidSupport.cpp` (sin `onTrimMemory`). La VRAM ya tiene LRU (2.5.2); la RAM no, y en Android el proceso muere sin informe.
- **Audio: variaciones aleatorias, prioridades, curvas de atenuación y HRTF** — P2 · M — `Audio.h:36-66`; `Audio.cpp:23`.
- **Red: RPC por objeto, mensajes no fiables, delta y presupuesto** — P2 · M — `Network.h:10-12`; `Network.cpp:416-437, 559-579`.
- **Navmesh horneada y guardada** — P2 · M — `NavigationSystem.cpp:1-21, 711-721`. Hoy se rehace en cada arranque y en el servidor.
- **Navegación: enlaces fuera de malla, áreas con coste, varios tipos de agente** — P2 · L — `Navigation.h:57-120`.
- **Juntas de física (bisagra, fija, muelle, 6DOF)** — P2 · M — `PhysicsComponents.h`. Jolt ya las trae.
- **Consultas: boxCast, capsuleCast, overlapBox, raycastAll en 3D** — P2 · S — `CoreApi.cpp:352`; `Api.gen.h:1605`.
- **Ragdoll con motores y parcial** — P3 · M — `Ragdoll.h`; `PhysicsSystem.cpp:794-951`.
- **Behavior Trees con subárboles; EQS** — P3 · M — `ai/BehaviorTree.h`.
- **Captura de fallos en el servidor dedicado** — P3 · S — `CramionEditor/server/ServerMain.cpp` (sin `set_terminate` ni minidump, que sí tienen editor y player).

#### Mejorar

- ✅ **Instanciar sin JSON y pool de objetos** — P1 · M — *Hecho 2026-10-20 (R3).* — tabla, punto 16. UE5 spawn desde el CDO; Unity clona en memoria y tiene `UnityEngine.Pool`.
- ✅ **El editor carga todas las celdas de World Partition** — P1 · L — *Hecho 2026-10-20 (R2): regiones cargadas.* — tabla, punto 13.
- **Animación en un solo hilo** — P2 · M — `RenderSync.cpp:3416-3710`. Un `parallelFor` sobre los Animators (el JobSystem ya existe).
- **Compresión de clips** — P2 · M — `Model.h:224-229`. Reducción de claves y cuantización (UE5 usa ACL).
- **Texturas dentro de cada `.crdata`** — P2 · L — `Importer.h:75-78`. Se duplican entre modelos y cambiar una obliga a reimportar.
- **Lag compensation con hitboxes por hueso** — P2 · M — `Api.gen.h:1471-1476`; `ServerMain.cpp:227-464`. El servidor no anima; headshots y skillshots contra hitboxes reales.

### 3.3 Scripting y API del SDK (C++, IPC, UI de juego, input, MCP)

El aislamiento de scripts en `CramionScriptHost.exe` es más robusto que en Unity, Unreal o Godot; el problema es su coste y su ciclo de vida. Ya existe y está bien: Enhanced Input con rebind, localización, 9-slice, rich text básico, Blueprints con breakpoints, `instantiateAsync` y `preloadPrefab`.

> Nota sobre problemas ya conocidos: `UIEventTrigger` **ya tiene** clic derecho, doble clic, arrastrar y soltar (`UI.h:251-258`); lo que falta es lo de abajo. Las mallas de código **ya recalculan** el AABB en `Mesh::markModified()` (`RuntimeMesh.cpp:125-139`) y hay `Mesh.recalculateBounds` en la API; si sigue fallando, revisar el camino que no pasa por `markModified`.

#### Reparar

- ✅ **Desactivar destruye el script (sin OnEnable/OnDisable)** — P1 · M — *Hecho 2026-10-20 (R1).* — tabla, punto 15. Además `sync()` llama a `activeInHierarchy()` para cada CppScript cada frame.
- ✅ **Llamadas a la API desde otro hilo** — P1 · S — *Hecho 2026-10-20 (R1).* — tabla, punto 19.
- ✅ **Mando en escritorio** — P1 · M — *Hecho 2026-10-20 (R3), con el dispositivo activo (`Input.getActiveDevice` / `activeDeviceChanged`).* — tabla, punto 20. También un evento de «cambió el dispositivo activo» para los iconos de botones.
- **Orden de ejecución no determinista** — P2 · S — `CppScripts.cpp:1111` (`unordered_map`), ~1872. Errores de «un frame tarde» irreproducibles; impide lockstep, replays y rollback. Orden estable y prioridad por clase (Unity Script Execution Order, UE5 TickGroups).
- **Respuestas de más de 4 MB truncadas sin aviso** — P3 · S — `CppScripts.cpp` ~778, ~799; `ScriptHost.cpp:158`.

#### Añadir

- **Rotación y escala en RectTransform** — P1 · M — `UI.h:69-76, 261, 266`; `UiRenderer.cpp`. Brújulas, minimapa que gira, indicadores de daño, «pop» por escala; hit-test con la transformación inversa.
- **Corrutinas y temporizadores para el juego** — P2 · M — `Script.h`; `Test.h` ya tiene la maquinaria (`Task`, `waitSeconds`, `waitUntil`). Sacarla a `cramion::Task<T>` cancelada en onDestroy/onDisable.
- **Depuración de C++** — P2 · M — `CppScripts.cpp:68-74` (TimeoutMs 2000, Optimize true). Botón «Adjuntar depurador», `-O0` en modo Depurar, sin tiempo máximo con `IsDebuggerPresent`, `launch.json`; perfil por clase.
- **Inspector en Play con valores vivos de `Property<T>`** — P2 · M — `CppScripts.cpp` ~1776-1785; `EditorCppScripts.cpp`. `Op::Snapshot` ya existe.
- **UIEventTrigger y Button: central, rueda, pointer down/up, pulsación larga** — P2 · S — `UI.h:124-134, 245-259, 302-335`; `UiRenderer.cpp:317-335`.
- **Navegación de la UI con mando y teclado (Selectable)** — P2 · M — `UI.cpp` (foco solo para InputField).
- **Máscaras no rectangulares y relleno radial** — P2 · M — `UI.h:231`; `UI.cpp:828-830`. Minimapa circular, retratos, cooldown radial.
- **Texto: iconos en línea, enlaces, auto-size, elipsis; CanvasGroup y tweens** — P2 · M — `UI.h`, `UI.cpp` (parseRichText ~1600).
- **API de Entity: hijos, `valid()` sin RPC, `script<T>()` indexado** — P2 · S — `Script.h:137, 1031`.
- **MCP: lote, `find_entities` paginado y herramientas de Play/UI/NavMesh/Animator** — P2 · M — `EditorMcp.cpp` ~171-515. 300 props = 300 viajes HTTP; `list_entities` devuelve el árbol entero.

#### Mejorar

- ✅ **Un viaje IPC por instancia y evento** — P1 · M — *2026-10-20 (R1): `CallMany` hecho; falta detectar los virtuales sobrescritos. R5: virtuales sobrescritos detectados por tipo; sin eventos vacíos.* — tabla, punto 17. Detectar los virtuales sobrescritos y `Op::CallMany`.
- ✅ **API generada por JSON y síncrona; scripts fuera de proceso también en el juego exportado** — P1 · L — *2026-10-20 (R3): camino binario por índice con lote para lo que no devuelve (tabla, punto 18). Falta el modo en el mismo proceso (o anillo en memoria compartida) del juego exportado. R5: scripts en el mismo proceso en el juego exportado de Windows (Android sigue con proceso).* — tabla, punto 18; `CppScripts.h:1-25`; `PlayerMain.cpp:956-966`. Además del camino binario, valorar un modo en el mismo proceso (o anillo de comandos en memoria compartida) para el juego exportado.
- **Recarga en caliente que recompila todo a `-O2`** — P1 · M — `CppScripts.cpp:2279-2440`. Un objeto por fuente con caché por hash, compilación en paralelo, PCH de `cramion/Script.h` y `-O0/-O1` en el editor.
- **Crear muchas instancias con script: 3 viajes por instancia** — P2 · M — `CppScripts.cpp` ~1756-1800, ~2627-2632. `Op::CreateMany` y presupuesto por frame (resto del tema streaming, fuera de lo que reparan los otros agentes).
- **UI de juego sobre listas de ImGui** — P2 · L — `UiRenderer.cpp:71-90`; `PlayerMain.cpp:1956`; `UI.cpp:387, 667`. Contorno de texto con 8-16 copias y reconstrucción completa cada frame. Pasada propia con texto SDF/MSDF, lotes por atlas y canvas con dirty flag.
- **API generada sin tipos** — P2 · M — `SdkGen.cpp` → `Api.gen.h:983-986`. Firmas C++ reales; los errores salen hoy solo en Play.

### 3.4 Editor y flujo de trabajo

#### Reparar

- ✅ **Deshacer por instantánea JSON del mundo** — P1 · L — *Hecho 2026-10-20 (R2): diferencias por entidad y componente, 500 pasos. Falta la ventana Historial.* — tabla, punto 10. Además el tope de 256 MB deja pocos pasos en escenas grandes y no hay ventana Historial. Unity, UE5 y Godot guardan solo lo que cambia.
- ✅ **`recordPrefabOverrides` en cada paso** — P1 · M — *Hecho 2026-10-20 (R2).* — tabla, punto 11.
- ✅ **Mover o renombrar rompe referencias** — P1 · M — *Hecho 2026-10-20 (R2): reescritura de rutas (sin UUID para todo).* — tabla, punto 21. UUID para todo lo referenciable o reescribir rutas con el grafo de dependencias. UE5 redirectores, Godot 4.4 UIDs.
- ✅ **Iconos de la Escena y clic recorren el mundo entero** — P2 · S — *Hecho 2026-10-20 (R2): vistas por componente, solo lo activo; el clic elige el más cercano.* — `EditorSceneView.cpp:786-793, 656-668`. Pasar a `forEachActiveDepthFirst` o a vistas por componente, como ya se hizo con otros recorridos en 2.5.2.

#### Añadir

- ✅ **Autoguardado y recuperación** — P1 · S — *Hecho 2026-10-20 (R1).* — tabla, punto 3. En otro hilo, a partir de la instantánea, en `Library/Autosave`.
- **Play con N clientes y servidor** — P1 · M — `EditorPhysics.cpp` ~620-700. Imprescindible para probar un 5v5. UE5 Number of Players, Unity Multiplayer Play Mode, Godot Run Multiple Instances.
- **Dibujo de depuración desde scripts** — P1 · S — `Api.gen.h:190-197` (solo `Debug.warn`). `Debug.drawLine/Sphere/Box/Ray/Text` con duración; el editor ya tiene `overlayLine`.
- **Selección por recuadro, ajuste a vértice y superficie, bajar al suelo** — P2 · M — `EditorViewport.cpp:123-143`; `EditorSceneView.cpp:612`.
- **Visor de referencias y borrado seguro** — P2 · M — `EditorProject.cpp:1719-1770` (el modal además recorre carpetas cada frame). Depende del grafo de dependencias (3.2).
- **Shader Graph: subgrafos y materiales derivados (padre e instancias)** — P2 · L — `ShaderGraph.cpp:110-340`; `EditorGraphs.cpp` (vista previa en CPU); `MaterialAsset.h`. Los nodos de color/profundidad de escena y color de vértice van en 3.1.
- **Timeline con pistas de transformación, propiedad, animación, audio y eventos** — P2 · L — `Cinematics.h:199-230`.
- **Perfilador: GPU en las capturas, memoria, frame debugger y perfilar el juego exportado** — P2 · L — `EditorInsights.cpp`; `EditorApp.cpp:2070-2135`; `profiling/`.
- **Editar varias escenas a la vez (aditivas o subniveles)** — P2 · L — `EditorWorkspaces.cpp:13`. Arte, gameplay e iluminación por separado y sin conflictos de Git.
- **Extensiones del editor desde código (ventanas, inspectores, gizmos, menús)** — P2 · L — no hay API de editor en el SDK. Para el editor de carriles y spawns del MOBA.

#### Mejorar

- 🟡 **Play/Stop y el modo prefab serializan el mundo entero** — P1 · L — *2026-10-20 (R2): entrar en Play y guardar la pestaña arman el texto con los registros del historial; parar Play y volver a una pestaña siguen rehaciendo el mundo (falta el segundo `ecs::World`). R5: Stop incremental por el historial (`editor.play.IncrementalStop`, apagado); falta el segundo `ecs::World`.* — `EditorPhysics.cpp:188, 305`; `EditorWorkspaces.cpp:457, 495, 593`. Un segundo `ecs::World` para el escenario del prefab.
- ✅ **Física en edición** — P1 · M — *Hecho 2026-10-20 (R2).* — tabla, punto 12.
- ✅ **Selección múltiple con coste cuadrático** — P2 · S — *2026-10-20 (R2): `SelectionSet` y `topLevelSelection` en O(n·profundidad); falta la caché de valores mezclados del Inspector (`EditorInspector.cpp` ~470-484). R5: caché de valores mezclados del Inspector.* — `EditorApp.h:137-141, 1389`; `EditorApp.cpp:811-820`; `EditorInspector.cpp:470-484`. `unordered_set` y caché de valores mezclados.
- ✅ **Segundo plano fijo (10 fps, escena cada 2 s)** — P2 · S — *Hecho 2026-10-20 (R2): Ahorro / Vista en vivo / Sin límite, 5-60 fps, ventana tapada u oculta y temporizador de alta resolución.* — `CramionEditor/src/main.cpp:35-52, 188-191, 252-255`. Lo que se ve como «5 FPS en segundo plano». Opción con 3 modos, detectar ventana tapada o minimizada en vez del foco y temporizador de alta resolución como el del player. Encaja con F2-12 de MEJORA.
- **Pintar prefabs en modo instanciado** — P2 · M — `EditorPrefabPaint.cpp:1-17`. Hoy cada roca es una instancia completa que entra en todos los costes de arriba.
- **Consola: hora, filtros por nivel, agrupar repetidos, ir al objeto** — P2 · M — `EditorLog.h:19-23, 61`; `EditorApp.cpp:2140-2210` (recuento con mutex cada frame, sin clipper con filtro).
- **Filtro de la Jerarquía** — P2 · S — `EditorHierarchy.cpp:363-374`. Caché y prefijos `t:`/`l:`/`p:`.
- **Git: bloqueos LFS y `.gitattributes` completo** — P2 · M — `EditorPlatform.cpp:138-175`. Faltan `.crbake` y `.crterrain` en LFS.
- **Disposiciones con nombre, escala de la interfaz y atajos configurables** — P3 · M — `ImGuiLayer.cpp:563-568`; `EditorApp.cpp:1350-1380`. Parte de F2-12.

### 3.5 Plataformas, estabilidad y calidad

Ya existe y está bien: manejador de crash con minidump en Windows, escritura atómica de escenas, caché de pipelines validada, `.crwpack` con CRC, actualizador con SHA-256 y rollback, firma de APK, MCP solo en loopback.

#### Reparar

- ✅ **JobSystem sin `try/catch`** — P0 · S — *Hecho 2026-10-20 (R1).* — tabla, punto 2. Los hilos de streaming (`VulkanDevice.cpp:1167-1173`) sí lo tienen.
- ✅ **Release sin símbolos** — P1 · S — *Hecho 2026-10-20 (R1).* — tabla, punto 4.
- ✅ **MCP sin token** — P1 · S — *Hecho 2026-10-20 (R1), con la comprobación de Host y la casilla.* — tabla, punto 5. Añadir comprobación de la cabecera Host y una casilla para apagarlo.
- **Licencias en los juegos exportados y dependencias sin versión fija** — P2 · S — `EditorExport.cpp`; `PackageRelease.cmake:73-81`; `CramionFX/CMakeLists.txt:61, 83` (stb y FidelityFX con `GIT_TAG master`). Revisar si SPIRV-Tools/SPIRV-Cross se enlazan y faltan en `THIRD_PARTY_NOTICES.md`.

#### Añadir

- **Diagnóstico de DeviceLost** — ver 3.1 (mismo ítem, no se cuenta dos veces).
- **Envío de informes de crash; informe completo en Android y en el player** — P1 · M — `LoadingScreen.cpp`; `EditorCrashReport.cpp:20-21`; `AndroidSupport.cpp:270-290` (funciones vacías). Subir el `.dmp` al `web-api` propio.
- **CI con CTest** — P1 · L — tabla, punto 22. Windows + Android + Linux (servidor), tests, shaders compatibles, regenerar SDK y manual.
- **Pruebas del render con imágenes de referencia** — P1 · L — ningún test usa `VulkanRenderer` ni `VulkanDevice`. Capturas comparadas, pasada con validación que falle y umbrales en `RenderBench` y `StreamingBench`.
- **Sanitizers y fuzzing de formatos externos** — P2 · M — `.crwpack`, `.datapack`, escenas y red. Clang ya es el compilador.
- **Player nativo para Linux / Steam Deck** — P2 · L — `EditorExport.cpp:128-132`; ya existen `WindowX11.cpp` y el preset `linux-release`.
- **iOS, macOS, web y consolas** — P2 · XL — solo Vulkan; MoltenVK sería la ruta en Apple. Para Overthrow el objetivo móvil sigue siendo el cliente web en Safari de iPhone, así que no bloquea el port.

#### Mejorar

- **`.crpack` descomprimido entero al primer arranque y sin verificar** — P1 · L — `PlayerMain.cpp:452-540`; `Pack.cpp:425`. Doble espacio en disco (grave en móvil), primer arranque lento y assets modificables (trampas). VFS que lea del paquete con `readPackFile` (ya hay bloques zstd) y firma.
- **Paquetes de la Store con C++ que se compila sin avisar** — P1 · M — `EditorPackageManager.cpp`; `CppScripts.cpp:941`; `web-api/cramion_api.py`. Mostrar los `.cpp` y pedir confirmación; marcar en la web los paquetes con código.
- **Exportar solo lo que se usa** — P2 · M — `EditorExport.cpp:341-343` (`pack_folder(Assets)` entero). Depende del grafo de dependencias.
- **Actualizador: autenticidad (Ed25519) y firma Authenticode** — P2 · S — `CramionUpdater/src/Update.cpp:686-712` (`if (expected.empty()) return true;`), :208-239 (acepta http).
- **`.crshader` compilados en cada arranque en Windows; caché de pipelines solo al iniciar y cerrar** — P2 · S — `RenderSync.cpp:313-318`; `EditorExport.cpp:619-645`; `VulkanDevice.cpp:1291, 1334`. Empaquetar SPIR-V también en Windows y guardar la caché periódicamente.
- **Documentación desfasada** — P3 · S — `README.md` (l.8 «DirectX 12», l.927 «Solo Windows», 11 menciones de Lua); `Api.gen.h` (25 ejemplos `function(...) end`); `UI.h:132`; `CppScripts.h` («Solo Windows»). Generar ejemplos en C++ y comprobar en CI que el manual está al día.
- ✅ **Esperas a la GPU sin tope** — P3 · S — *Hecho 2026-10-20 (R1): `VulkanDevice::waitFence`, 10 s por defecto (`CRAMION_GPU_TIMEOUT_MS`).* — `VulkanDevice.cpp:884`, `GpuTicket::wait` (~990) con `UINT64_MAX`. Con un tope de unos 5 s se trata como DeviceLost y se escribe el informe.
- **`android:allowBackup="true"` fijo** — P3 · S — `AndroidBuild.cpp:572`. Opción en la configuración de compilación, apagada por defecto.

---

## 4. Orden sugerido

**2.5.2 — Estabilidad (ahora, junto con compilar lo del 2026-10-19 de PENDIENTES.md)**
*Estado 2026-10-20 (ronda R1): hecho todo salvo la validación de sincronización y lo marcado 🟡 arriba (ufbx, parte del LOD del follaje, nombres de recursos).*
JobSystem con `try/catch` · DeviceLost capturado con marcadores y `device_fault` · autoguardado y recuperación · Release con PDB · token del MCP · etiquetas de debug utils y validación de sincronización (para cazar los DeviceLost) · esperas con tope · FBX con piel sin tomas · reintento de FBX con toma rota · lista de nombres genéricos en `create_model_materials` · LOD sin Prune/sloppy en follaje con recorte alfa · OnEnable/OnDisable · protección de hilo en la API de scripts.

**2.5.3 — Editor rápido con escenas grandes**
*Estado 2026-10-20 (R2): hecho todo salvo lo marcado 🟡 arriba (reimportar al cambiar el FBX, caché de valores mezclados del Inspector, parar Play y volver de una pestaña sin rehacer el mundo).*
Iconos y clic con recorridos activos · selección con conjunto · física de edición incremental · `recordPrefabOverrides` con caché · deshacer por diferencias · segundo plano configurable · recarga en caliente de assets con `ReadDirectoryChangesW` · referencias que sobreviven a mover y renombrar · carga por regiones de World Partition en el editor · Play/prefab sin reserializar el mundo.

**2.6 — Render y VFX para juego**
*Estado 2026-10-20 (R3): hechos luces por clúster sin tope, color de vértice, ruido de gradiente y `vertex()` en sombras y velocidades; parciales (🟡 arriba) transparencia forward y dominios, color y profundidad de escena, parámetros por instancia, VFX ribbons/mallas/eventos (falta la luz real) y decals por clúster. Siguen pendientes la máscara reactiva, outlines/capas/viewmodel y la baja latencia.*
Cola de retirada por fence (adiós a los `waitIdle`) · luces por clúster sin tope · transparencia forward con emisión y dominios Transparente/Unlit · color y profundidad de escena · color de vértice · parámetros por instancia · máscara reactiva FSR/DLSS · outlines, capas y viewmodel · VFX ribbons, mallas y eventos · decals por clúster · ruido de gradiente · `vertex()` en sombras y velocidades · baja latencia.

**2.7 — Gameplay y herramientas**
*Estado 2026-10-20 (R3): hechos plantillas de prefab y pool, API binaria de los scripts y mando en PC.*
Prefabs anidados y variantes · plantillas de prefab y pool · scripts: CallMany, API binaria, recarga incremental con PCH, corrutinas, orden determinista · mando en PC · capas, aditivas, root motion, eventos y montages · buses de audio · dibujo de depuración · Play multijugador · UI con rotación, máscaras, navegación y texto SDF · depuración de C++ e Inspector en Play.

**2.8 — Producción y plataforma**
CI con CTest y pruebas del render · informes de crash enviados · VFS sobre `.crpack` y firma · grafo de dependencias, exportar solo lo usado y visor de referencias · aviso de C++ en paquetes de la Store · firma del actualizador · sanitizers y fuzzing · licencias en el export · player de Linux.

**3.0 — AAA y red competitiva**
Red autoritativa con predicción y reconciliación, RPC y hitboxes por hueso · RT incremental y personajes en la escena de rayos · niebla en froxels · sondas con caja · GTAO · HDR10 · terreno avanzado · HLOD texturizado · morph targets · Timeline con pistas · perfilador de memoria y GPU · edición multiescena · extensiones del editor. Y lo grande de MEJORA.md: B7/B8 (bindless e instancia con material), B3, B11/B12.

---

## 5. Relación con los otros documentos

- **[`PENDIENTES.md`](PENDIENTES.md):** lo primero sigue siendo compilar y probar el trabajo del 2026-10-19. Los restos menores que ya lista (deshacer de otras pestañas al renombrar un Blueprint, luces de una capa oculta que siguen iluminando, regenerar el manual) no se repiten aquí.
- **[`MEJORA.md`](MEJORA.md):** las optimizaciones de la sección 3 (B1-B19) y el plan de gama baja (F2-x, F3-x) siguen vigentes. Aquí solo se citan: B8 (la versión ligera de parámetros por instancia de 3.1 no la sustituye), B11/B12 (el HLOD texturizado de 3.1 es lo que le falta al HLOD que ya existe), B14, B17 y F2-12 (el modo de segundo plano y las preferencias del editor de 3.4).
- **[`PLAN-LIGHTMAPS.md`](PLAN-LIGHTMAPS.md):** los lightmaps UV2 no se tratan aquí. Su xatlas se puede reutilizar para hornear el HLOD texturizado.
- **En curso por otros agentes (no contados como pendientes nuevos):** streaming de GPU, memoria y VMA, duplicados, sombras de contacto. De esos temas aquí solo queda lo que sigue sin cubrir: los `waitIdle` de decals, shaders, Render Textures, UI y escena de rayos; la creación de scripts por IPC al instanciar; y la RAM sin presupuesto.

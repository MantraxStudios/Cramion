# Cramion: hoja de ruta (qué reparar, añadir y mejorar)

Análisis del 2026-10-20 sobre el código de `D:/Trabajo/NewProyects/Cramion` (2.5.1, con lo de 2.5.2 sin compilar). Solo lectura: cada punto se comprobó con grep o leyendo el archivo que se cita. El contexto es portar Overthrow Arena (MOBA) con calidad AAA y dejar el motor listo también para shooters.

No se repite aquí lo que ya está en [`MEJORA.md`](MEJORA.md) (bindless B7, instancia con material B8, vértices comprimidos B3, sombras por GPU B5, impostores B11, HLOD por clúster B12, static batching en editor B14, ASTC B17, F2-12), en [`PENDIENTES.md`](PENDIENTES.md) (probar lo del 2026-10-19, restos menores) ni en [`PLAN-LIGHTMAPS.md`](PLAN-LIGHTMAPS.md). Tampoco lo que otros agentes están reparando ahora (streaming de GPU, memoria, duplicados, sombras de contacto); solo se señala lo que queda de esos temas.

Tamaños: **S** (horas a 2 días), **M** (1-2 semanas), **L** (3-6 semanas), **XL** (meses). Prioridades: **P0** cierre o corrupción, **P1** bloquea un juego AAA o el rendimiento, **P2** calidad o productividad, **P3** deseable.

---

## 1. Resumen

El motor ya está muy por encima de lo habitual en un motor propio: diferido Vulkan 1.3 con modo compatible, PBR de Disney completo, PCSS, RT (sombras, reflejos, DDGI, path tracing), FSR 3.1 y DLSS 4, océano FFT, follaje en GPU, Hi-Z, meshlets, VFX Graph en GPU, World Partition con HLOD, Jolt (ragdoll, vehículos, tela, fractura), Recast/Detour con multitudes, servidor dedicado con compensación de lag, Motion Matching, Enhanced Input y un editor completo con MCP. Lo que falta no son técnicas sueltas, sino **robustez** y **arquitectura para escenas grandes**:

1. **El editor no escala del todo:** parar Play y volver de una pestaña de prefab siguen rehaciendo el mundo entero.
2. **Flujo de assets:** el importador de FBX sigue sin ufbx.
3. **Prefabs y spawn:** no hay prefabs anidados ni variantes.
4. **Render para juego:** sin máscara reactiva para DLSS.
5. **Animación de gameplay:** no hay capas con máscara, aditivas, root motion, eventos (notifies) ni montages.
6. **Plataforma y seguridad:** no hay CI ni CTest y el `.crpack` se descomprime entero al primer arranque.

---

## 2. Reparar primero (P0 / P1)

Lo que más rinde por esfuerzo, en orden de ataque. Lista completa por área en la sección 3.

| # | Título | Área | Dónde | Tam. | Por qué |
|---|---|---|---|---|---|
| 1 | 🟡 FBX con una toma rota | Núcleo | `CramionFX/src/asset/ModelLoader.cpp:1059-1074` | L | ufbx como cargador de FBX, como hizo Godot 4.3. **🟡 2026-10-20 (R1):** la relectura sin validar + `repairAnimations` ya está. Falta ufbx. |
| 2 | 🟡 Recarga de assets editados en disco | Núcleo/Editor | `EditorProject.cpp:91-133`; `AssetDatabase.cpp:70-150` | L | **🟡 2026-10-20 (R2, R5):** faltan imágenes de UI, sprites, capas de `.crterrain`, texturas incrustadas en `.crdata`, skyboxes, los cambios hechos con el editor cerrado y el vigilante en macOS. |
| 3 | 🟡 Física de edición incremental sin medir | Editor | `EditorPhysics.cpp:514-515`; `PhysicsSystem.cpp:2952-3060` | S | Falta medir en el editor con Ashbound la «Física» en reposo con `physics.IncrementalEditSync` a 0 y a 1. |
| 4 | 🟡 World Partition en el editor: huellas del mundo entero | Núcleo/Editor | `CramionCore/include/CramionCore/world/WorldPartition.h:2-30` | — | Pendiente: `UndoHistory::absorb` para no calcular huellas del mundo entero en cada frame con celdas entrando. |
| 5 | 🟡 `waitIdle()` en caminos de juego | Render | `VulkanRenderer.cpp` | M | **🟡 (R1, R5):** quedan las sombras locales, el cambio de resolución y el filtrado de texturas. |
| 6 | 🟡 Viajes IPC de los scripts | Scripting | `CppScripts.cpp` ~2589-2640, ~1194, ~772 | M | **🟡 (R1, R5):** falta juntar callbacks, mensajes y `Create`. |
| 7 | 🟡 Mando: lo que falta | Scripting/Input | `CramionDM/src/Window.cpp`, `WindowX11.cpp`, `Input.cpp` | M | Faltan GameInput, HID genéricos en Windows, vibración en Android y compilar `WindowX11.cpp` en Linux. |
| 8 | 🟡 Referencias sin UUID | Editor | `EditorProject.cpp:701-718, 1240-1248` | M | Siguen sin UUID las rutas dentro del código `.cpp` y las escenas por nombre. |
| 9 | No hay CI ni CTest | Plataforma | raíz (sin `.github/`), todos los `CMakeLists` (sin `add_test`) | L | ~60 ejecutables de tests a mano; Android y shaders compatibles se compilan a mano. Las regresiones se ven al jugar. |

---

## 3. Por área

Cada ítem: **prioridad · tamaño** — dónde — por qué. Comparación con Unreal 5 (UE5), Unity 6 y Godot 4 cuando aporta.

### 3.1 Render y gráficos (CramionFX)

#### Reparar

- 🟡 **`waitIdle()` en caminos de juego** — P1 · M — ver tabla, punto 5. *Quedan sombras locales, cambio de resolución y filtrado de texturas.*
- 🟡 **LOD automático** — P1 · M — *Falta el fundido con tramado en personajes, sombras, mesh shaders y modo compatible.* — `CramionFX/src/asset/MeshLod.cpp`.
- **DLSS sin máscara reactiva** — P1 · M — `TemporalUpscalers.cpp/.h:39-55`. FSR 3 y el TAA propio ya la reciben (transparentes y reflejos nítidos); falta pasarla a NGX.
- 🟡 **Ruido de `.crshader`** — P2 · S — *Falta compartir el voronoi con el Shader Graph.* — `CramionFX/shaders/surface_common.glsl`.

#### Añadir

- 🟡 **Color por vértice** — P1 · M — *Falta `RuntimeMesh.colors` y la interfaz en el editor.* — `Model.h:24-35` (SkinnedVertex).
- 🟡 **Parámetros de material por instancia** — P1 · M — *Falta `INSTANCE_DATA` en `vertex()` y su nodo.* — `Components.h:98-114` (MeshRenderer). Es una versión ligera de B8 de MEJORA (sin esperar a bindless).
- **Outlines de juego, capas de render y arma en primera persona** — P1 · M — `VulkanRenderer.h:518-521` (setOutlinedActors, solo editor); `Components.h` (Camera sin máscara). Faltan silueta a través de paredes (MOBA), bitmask de capas en MeshRenderer y Camera, y modo «viewmodel» con FOV propio. UE5 Custom Depth/Stencil, Unity Rendering Layers, Godot `cull_mask`.
- **Baja latencia (Reflex / Anti-Lag 2 / present_wait)** — P1 · M — `drawFrame`; `VulkanSwapchain.cpp`; `VulkanCommon.h:61`. Necesario en shooters y para la generación de frames. UE5 y Unity 6 tienen Reflex.
- **Sondas de reflexión colocables con caja** — P2 · M — `ReflectionProbe.h:16-46`; `lighting.frag:73`. Una sola sonda pegada a la cámara; en gama media es la fuente principal de reflejos. Array de sondas horneadas con paralaje de caja.
- 🟡 **Herramientas de depuración de GPU** — P2 · S — *Faltan nombres en las pipelines gráficas y en las texturas de los modelos.* — `VulkanInstance.cpp:26-90, 166`.
- **Salida HDR10/scRGB** — P2 · M — `VulkanSwapchain.cpp:101, 170-185` (solo `eSrgbNonlinear`). El motor ya renderiza en HDR con AgX.
- **Piel con subsurface en pantalla y modelo de ojo** — P3 · M — `disney_brdf.glsl`; `lighting.frag:1531`. Para primeros planos (selección de héroe, tienda).
- **XeSS y generación de frames** — P3 · M — `GraphicsSettings.h:22`. Depende de la baja latencia.
- **LUT 3D importable (.cube)** — P3 · S — `composite.frag:15-49`.

#### Mejorar

- 🟡 **Luces locales por clústeres** — P1 · L — *Faltan sombras por rayos, niebla volumétrica, agua y VFX para las de clúster y el atlas de sombras por prioridad.* — `LocalLightShadows.h:20-21`; `volumetric.frag:45-46`.
- 🟡 **Escena de rayos incremental** — P1 · L — *Queda la reconstrucción entera al editar un modelo que ya está en la escena.* — `VulkanRendererRayScene.cpp`.
- 🟡 **Decals** — P1 · M — *Falta el recorte por clúster 3D.* — `gbuffer_surface.glsl`.
- **Niebla en froxels para transparentes** — P2 · L — `volumetric.frag`; `vfx.frag`, `particle.frag`, `glass.frag`, `sprite.frag` sin niebla. Todo lo transparente brilla a través de la niebla. Volumen 3D compartido como en UE5, HDRP y Godot.
- **GTAO con normales dobladas y oclusión especular** — P2 · M — `ssao.frag:1-16`. XeGTAO (MIT).
- **Terreno: agujeros, triplanar, mezcla por altura** — P2 · M — `terrain.frag`; `Terrain.h:38` (8 capas). Agujeros para cuevas y caminos, sin estiramiento en acantilados.
- **HLOD de World Partition con texturas horneadas** — P2 · M — `ProxyMesh.h:1-30`; `WorldPartition.cpp:140-152, 1201`. Hoy un color medio por grupo de materiales («plastilina»). Distinto de B11/B12 de MEJORA: es lo que le falta al HLOD que ya existe.
- **Orden entre familias de transparentes / OIT** — P3 · L — `VulkanRenderer.cpp:6209-6240`. El humo detrás de una ventana se pinta encima del vidrio.

### 3.2 Núcleo y runtime (ECS, prefabs, assets, animación, física, navegación, audio, red)

#### Reparar

- 🟡 **FBX con toma rota** — P1 · L — *Falta ufbx (2-4 semanas con pruebas de paridad).* — tabla, punto 1.

#### Añadir

- 🟡 **Recarga en caliente de assets editados en disco** — P1 · L — *Faltan algunos tipos, los skyboxes y los cambios con el editor cerrado.* — tabla, punto 2.
- **Prefabs anidados y variantes** — P1 · L — `Prefab.h:24`; `Prefab.cpp:172` (flattenNested), :227, :412. Héroes con skins, minions por facción y piezas reutilizadas obligan a duplicar. Unity Nested/Variants, UE5 Child Blueprints, Godot escenas heredadas.
- **Animator con capas, máscara de huesos y aditivas** — P1 · L — `AnimatorController.h`; `CramionFX/anim/Animator.h` (evaluateBlend). Lanzar o apuntar con el torso mientras se corre, retroceso, aim offsets.
- **Root motion** — P1 · M — `Components.h:120-142`; `RenderSyncMotion.cpp:269`. Ataques con avance, esquivas, saltos de habilidad.
- **Eventos (notifies) y curvas en los clips, con ventana de clips en el editor** — P1 · M — `Model.h:224-235`; `EditorAnimator.cpp` (solo el grafo). El fotograma del golpe y el cast point de un MOBA se sincronizan con esto. Incluye una ventana para previsualizar y recorrer un clip y marcar eventos.
- **Buses de audio y volumen maestro, de música y de efectos** — P1 · M — `audio/Audio.h:38-66, 109-190`; `CoreApi.cpp:408-444`. Ni siquiera hay deslizadores para el menú de opciones; después, ducking e instantáneas. Unity AudioMixer, UE5 Submix, Godot AudioServer.
- **Red autoritativa con predicción y reconciliación** — P1 · XL — `net/Network.h:1-45`; `NetworkObject.h`. El dueño es la autoridad de su posición: trampas y mala sensación con latencia. UE5 CharacterMovement con predicción, Unity Netcode for Entities.
- **Montages / slots de animación puntual** — P2 · M — depende de capas y eventos.
- **Morph targets** — P2 · L — `ModelLoader.cpp`, `Model.h`, `SkinnedPass.cpp` (no se lee `mAnimMeshes`).
- **Opciones de importación: cortar clips, bucle, esqueleto compartido y colisión UCX** — P2 · M — `Importer.h:43-67`.
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

- 🟡 **World Partition en el editor** — *Falta `UndoHistory::absorb`.* — tabla, punto 4.
- **Compresión de clips** — P2 · M — `Model.h:224-229`. Reducción de claves y cuantización (UE5 usa ACL).
- **Texturas dentro de cada `.crdata`** — P2 · L — `Importer.h:75-78`. Se duplican entre modelos y cambiar una obliga a reimportar.
- **Lag compensation con hitboxes por hueso** — P2 · M — `Api.gen.h:1471-1476`; `ServerMain.cpp:227-464`. El servidor no anima; headshots y skillshots contra hitboxes reales.

### 3.3 Scripting y API del SDK (C++, IPC, UI de juego, input, MCP)

El aislamiento de scripts en `CramionScriptHost.exe` es más robusto que en Unity, Unreal o Godot; el problema es su coste y su ciclo de vida. Ya existe y está bien: Enhanced Input con rebind, localización, 9-slice, rich text básico, Blueprints con breakpoints, `instantiateAsync` y `preloadPrefab`.

> Nota sobre problemas ya conocidos: `UIEventTrigger` **ya tiene** clic derecho, doble clic, arrastrar y soltar (`UI.h:251-258`); lo que falta es lo de abajo. Las mallas de código **ya recalculan** el AABB en `Mesh::markModified()` (`RuntimeMesh.cpp:125-139`) y hay `Mesh.recalculateBounds` en la API; si sigue fallando, revisar el camino que no pasa por `markModified`.

#### Reparar

- 🟡 **Mando en escritorio** — P1 · M — *Faltan GameInput, HID genéricos, vibración en Android y `WindowX11.cpp` en Linux.* — tabla, punto 7.
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

- 🟡 **Viajes IPC de los scripts** — P1 · M — *Falta juntar callbacks, mensajes y `Create`.* — tabla, punto 6.
- **Recarga en caliente que recompila todo a `-O2`** — P1 · M — `CppScripts.cpp:2279-2440`. Un objeto por fuente con caché por hash, compilación en paralelo, PCH de `cramion/Script.h` y `-O0/-O1` en el editor.
- **Crear muchas instancias con script: 3 viajes por instancia** — P2 · M — `CppScripts.cpp` ~1756-1800, ~2627-2632. `Op::CreateMany` y presupuesto por frame (resto del tema streaming, fuera de lo que reparan los otros agentes).
- **UI de juego sobre listas de ImGui** — P2 · L — `UiRenderer.cpp:71-90`; `PlayerMain.cpp:1956`; `UI.cpp:387, 667`. Contorno de texto con 8-16 copias y reconstrucción completa cada frame. Pasada propia con texto SDF/MSDF, lotes por atlas y canvas con dirty flag.
- **API generada sin tipos** — P2 · M — `SdkGen.cpp` → `Api.gen.h:983-986`. Firmas C++ reales; los errores salen hoy solo en Play.

### 3.4 Editor y flujo de trabajo

#### Reparar

- 🟡 **Deshacer** — P1 · L — *Falta la ventana Historial.* — `EditorHistory.cpp`.
- 🟡 **Mover o renombrar rompe referencias** — P1 · M — *Sin UUID para todo.* — tabla, punto 8. UE5 redirectores, Godot 4.4 UIDs.

#### Añadir

- **Play con N clientes y servidor** — P1 · M — `EditorPhysics.cpp` ~620-700. Imprescindible para probar un 5v5. UE5 Number of Players, Unity Multiplayer Play Mode, Godot Run Multiple Instances.
- **Dibujo de depuración desde scripts** — P1 · S — `Api.gen.h:190-197` (solo `Debug.warn`). `Debug.drawLine/Sphere/Box/Ray/Text` con duración; el editor ya tiene `overlayLine`.
- **Selección por recuadro, ajuste a vértice y superficie, bajar al suelo** — P2 · M — `EditorViewport.cpp:123-143`; `EditorSceneView.cpp:612`.
- **Visor de referencias y borrado seguro** — P2 · M — `EditorProject.cpp:1719-1770` (el modal además recorre carpetas cada frame). Depende del grafo de dependencias (3.2).
- **Shader Graph: subgrafos y materiales derivados (padre e instancias)** — P2 · L — `ShaderGraph.cpp:110-340`; `EditorGraphs.cpp` (vista previa en CPU); `MaterialAsset.h`.
- **Timeline con pistas de transformación, propiedad, animación, audio y eventos** — P2 · L — `Cinematics.h:199-230`.
- **Perfilador: GPU en las capturas, memoria, frame debugger y perfilar el juego exportado** — P2 · L — `EditorInsights.cpp`; `EditorApp.cpp:2070-2135`; `profiling/`.
- **Editar varias escenas a la vez (aditivas o subniveles)** — P2 · L — `EditorWorkspaces.cpp:13`. Arte, gameplay e iluminación por separado y sin conflictos de Git.
- **Extensiones del editor desde código (ventanas, inspectores, gizmos, menús)** — P2 · L — no hay API de editor en el SDK. Para el editor de carriles y spawns del MOBA.

#### Mejorar

- 🟡 **Parar Play y el modo prefab rehacen el mundo entero** — P1 · L — *Falta el segundo `ecs::World` (el Stop incremental, `editor.play.IncrementalStop`, sigue apagado).* — `EditorPhysics.cpp:188, 305`; `EditorWorkspaces.cpp:457, 495, 593`. Un segundo `ecs::World` para el escenario del prefab.
- 🟡 **Física en edición** — P1 · S — *Falta medirla.* — tabla, punto 3.
- **Pintar prefabs en modo instanciado** — P2 · M — `EditorPrefabPaint.cpp:1-17`. Hoy cada roca es una instancia completa que entra en todos los costes de arriba.
- **Consola: hora, filtros por nivel, agrupar repetidos, ir al objeto** — P2 · M — `EditorLog.h:19-23, 61`; `EditorApp.cpp:2140-2210` (recuento con mutex cada frame, sin clipper con filtro).
- **Filtro de la Jerarquía** — P2 · S — `EditorHierarchy.cpp:363-374`. Caché y prefijos `t:`/`l:`/`p:`.
- **Git: bloqueos LFS y `.gitattributes` completo** — P2 · M — `EditorPlatform.cpp:138-175`. Faltan `.crbake` y `.crterrain` en LFS.
- **Disposiciones con nombre, escala de la interfaz y atajos configurables** — P3 · M — `ImGuiLayer.cpp:563-568`; `EditorApp.cpp:1350-1380`. Parte de F2-12.

### 3.5 Plataformas, estabilidad y calidad

Ya existe y está bien: manejador de crash con minidump en Windows, escritura atómica de escenas, caché de pipelines validada, `.crwpack` con CRC, actualizador con SHA-256 y rollback, firma de APK, MCP solo en loopback.

#### Reparar

- **Licencias en los juegos exportados y dependencias sin versión fija** — P2 · S — `EditorExport.cpp`; `PackageRelease.cmake:73-81`; `CramionFX/CMakeLists.txt:61, 83` (stb y FidelityFX con `GIT_TAG master`). Revisar si SPIRV-Tools/SPIRV-Cross se enlazan y faltan en `THIRD_PARTY_NOTICES.md`.

#### Añadir

- **Envío de informes de crash; informe completo en Android y en el player** — P1 · M — `LoadingScreen.cpp`; `EditorCrashReport.cpp:20-21`; `AndroidSupport.cpp:270-290` (funciones vacías). Subir el `.dmp` al `web-api` propio.
- **CI con CTest** — P1 · L — tabla, punto 9. Windows + Android + Linux (servidor), tests, shaders compatibles, regenerar SDK y manual.
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
- **`android:allowBackup="true"` fijo** — P3 · S — `AndroidBuild.cpp:572`. Opción en la configuración de compilación, apagada por defecto.

---

## 4. Orden sugerido

**2.5.2 — Estabilidad (lo que queda)**
ufbx como cargador de FBX.

**2.5.3 — Editor rápido con escenas grandes (lo que queda)**
Recarga en caliente de los tipos que faltan · parar Play y volver de una pestaña sin rehacer el mundo · `UndoHistory::absorb` · medir la física de edición.

**2.6 — Render y VFX para juego (lo que queda)**
Los últimos `waitIdle` · máscara reactiva en DLSS · outlines, capas y viewmodel · decals por clúster 3D · baja latencia.

**2.7 — Gameplay y herramientas**
Prefabs anidados y variantes · scripts: recarga incremental con PCH, corrutinas, orden determinista · capas, aditivas, root motion, eventos y montages · buses de audio · dibujo de depuración · Play multijugador · UI con rotación, máscaras, navegación y texto SDF · depuración de C++ e Inspector en Play.

**2.8 — Producción y plataforma**
CI con CTest y pruebas del render · informes de crash enviados · VFS sobre `.crpack` y firma · grafo de dependencias, exportar solo lo usado y visor de referencias · aviso de C++ en paquetes de la Store · firma del actualizador · sanitizers y fuzzing · licencias en el export · player de Linux.

**3.0 — AAA y red competitiva**
Red autoritativa con predicción y reconciliación, RPC y hitboxes por hueso · niebla en froxels · sondas con caja · GTAO · HDR10 · terreno avanzado · HLOD texturizado · morph targets · Timeline con pistas · perfilador de memoria y GPU · edición multiescena · extensiones del editor. Y lo grande de MEJORA.md: B7/B8 (bindless e instancia con material), B3, B11/B12.

---

## 5. Relación con los otros documentos

- **[`PENDIENTES.md`](PENDIENTES.md):** lo primero sigue siendo probar el trabajo del 2026-10-19 en los proyectos. Los restos menores que ya lista (deshacer de otras pestañas al renombrar un Blueprint, luces de una capa oculta que siguen iluminando, regenerar el manual) no se repiten aquí.
- **[`MEJORA.md`](MEJORA.md):** las optimizaciones de la sección 3 (B1-B19) y el plan de gama baja (F2-x, F3-x) siguen vigentes. Aquí solo se citan: B8 (la versión ligera de parámetros por instancia de 3.1 no la sustituye), B11/B12 (el HLOD texturizado de 3.1 es lo que le falta al HLOD que ya existe), B14, B17 y F2-12 (las preferencias del editor de 3.4).
- **[`PLAN-LIGHTMAPS.md`](PLAN-LIGHTMAPS.md):** los lightmaps UV2 no se tratan aquí. Su xatlas se puede reutilizar para hornear el HLOD texturizado.
- **En curso por otros agentes (no contados como pendientes nuevos):** streaming de GPU, memoria y VMA, duplicados, sombras de contacto. De esos temas aquí solo queda lo que sigue sin cubrir: los `waitIdle` de las sombras locales, el cambio de resolución y el filtrado de texturas; la creación de scripts por IPC al instanciar; y la RAM sin presupuesto.

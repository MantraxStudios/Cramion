# Pendientes de Cramion

Actualizado: 2026-10-19. El rendimiento va aparte, en `MEJORA.md`, y los lightmaps en `PLAN-LIGHTMAPS.md` (fases 0-2 escritas, 3 casi y 4 a medias; sin compilar).

## Lo primero: compilar y probar el trabajo del 2026-10-19
Todo está sin commitear. **Compilado el 2026-10-19** (build-release, Windows): CramionEditor, CramionPlayer, cramion_sdkgen y los 60 ejecutables de tests, que pasan todos (BuildConfigTests con `CramionPlayer.exe assets/icon.png` y AndroidBuildTests desde la raíz del repo). El SDK regenerado con `cramion_sdkgen` sale igual que el editado a mano. Falta probarlo en el editor y en el juego (riesgos de abajo).
- **Falta compilar:** Android `main` (arm64 y armeabi-v7a, de uno en uno) y los shaders en modo compatible (`CRAMION_VK_COMPAT=1`). Repetir `CramionTemplateTests coches` varias veces.
- **Regenerar el manual** con `python docs-src/build_manual.py` (se editó `docs-src`: vídeo, pruebas, red, mundos grandes, Blueprints, Visual Script, render, iluminación horneada).
- **Documentar en Graphics** las opciones nuevas: `borderless`, `window_size`, `monitor_*`, `windowed_max_*`, `fps_limit` y `lighting_quality`. Si se quiere, cubrir el modo 3 (sin bordes) en `BuildConfigTests`.

### Riesgos a probar, por área
- **Memoria y subidas (VMA, lotes, cola de transferencia):** capas de validación sin errores de propiedad entre colas ni de barreras; línea «Cola de transferencia dedicada»; texturas bien al hacer streaming y al cambiar el LOD (si falla la dependencia entre lotes saldrían basura o negro); repetir con `CRAMION_VK_NO_TRANSFER_QUEUE=1` y `CRAMION_VK_COMPAT=1`; al cerrar, ni «Quedan N recursos sin liberar» ni asserts de VMA; cambiar de escena mientras se suben modelos; cambiar el filtrado de texturas; comparar «Subir modelos nuevos» y la carga; clips en streaming y vóxeles.
- **Mundo estático y sombras:** ciudad Static con `render.streaming.GpuResidency` (bajan «Recorrer entidades» y «Actores y animación», no se redibujan todas las cascadas); sombras de objetos pequeños lejanos; en Keryan, VRAM y «fuera» de los modelos solo-base, ningún `.crmat` invisible y cambios de material en vivo; sombras de focos y puntuales con cosas moviéndose; sockets; selección de objetos lejanos; vidrio. Con los parches 0013/0015: giro rápido de cámara con el sol bajo (sin sombras que falten; si el redibujo por giro cuesta mucho, subir el 0,22); clústeres del lote estático en el borde de la vista; animados entre cascadas en gama baja. Terreno: esculpir o hacer streaming redibuja las cascadas (con un frame de retraso) y la sonda de reflexión no fuerza redibujos.
- **Trazado de rayos y tiling:** que el binding 9 pase la validación; un `.crmat` con tiling y desplazamiento en raster, sombras con alfa, parallax, teselado (y su sombra), vidrio, `.crshader`, meshlets, modo compatible, reflejos y GI por rayos, path tracing (también `CRAMION_SER=1`) y follaje con micromapa; un objeto con variante nueva tras cargar aparece en los rayos al segundo; comparar BLAS y VRAM con 5f7a7d3.
- **Gama baja (GTX 1050 / `CRAMION_HARDWARE_TIER=low`):** GI horneada sin sondas dinámicas (el pase «GI» debería bajar a 2-3 ms; buscar costuras); sol horneado: «sombras … (0 llamadas)» y cascadas que vuelven al añadir agua, vidrio, volumétrica o fluidos, sin fugas de luz; «Agua», «Ondas» y «Vegetación» por debajo de 0,3 ms, árboles fuera de los ríos, flotación y cámara bajo el agua; sombras hasta 60 m en Bajo y `lod_triangles` frente a Alto; el test «gama baja: llega a 60 FPS en <15 s» puede cambiar (ya no arranca en Lod 1); VR y móvil sin cambios.
- **Lightmaps:** `skinned.tesc` pasa el hueco de instancia (`uint`) sin `flat` (si glslang lo rechaza, como `float`); en la casa, «Generar UV de lightmap» (anotar el tiempo de White1), hornear en Media con el sol Horneado y comparar con `set_lighting_mode`; `invalid_texels_percent`; la ciudad con el tope de 8 M téxeles (RAM durante UV2 y BVH); modo compatible sin el aviso de layout de SkinnedPass; exportar con static batching («N con lightmap se quedan fuera del lote») y verlos en el player.
- **Fallos conocidos arreglados:** sombra con `base_color.a` (textura opaca con alfa 0,3 no da sombra; hoja 0,6 × 0,7 recorta igual que en la imagen); humo con mezcla alfa y miles de partículas sin parpadeo, y el coste en el perfilador; dos lavas con distinta corteza; esquinas de paredes con sol rasante con TAA y con FSR (si queda algo, mirar el rebote de Catmull-Rom del TAA).
- **Render de Keryan:** con validación, FSR 1 con bloom, rayos de luz y auto-exposición apagados desde el arranque (0017) y el océano que deja de verse (0019); luces completas con RT/path tracing y teselas en el editor con segunda vista, VR y Render Textures; `lighting.frag` compila con el UBO de ~11,6 KB; que no desaparezca vidrio visible (de personajes y con LOD distinto de 0), también en el juego exportado sin `glass_cull.comp.spv`; early-Z: follaje, rejas y BC1 de DDS siguen recortando y no hay cuelgue al cerrar el juego; en el juego no hay copia de vista y en el editor el visor se ve desde el primer frame; vectores de movimiento sin saltos al entrar o salir personajes; superficies coplanares terreno-calle (0007).
- **Mallas:** LOD de personajes a 6-15 m (cara, manos, cuernos, arco) y el log `[Modelo] ... error x %`; modelos de 1 hueso con animaciones (ahora tienen LOD); ondas con un océano a poca altura; Bone Socket bajo un padre con escala ≠ 1 (cuidado con escala no uniforme y rotada).
- **Texturas BC7 (0028, 0036):** primer arranque con árboles o terreno (comprime en segundo plano en `%LOCALAPPDATA%/Cramion/Cache`) y segundo arranque con «cache …»; cerrar a mitad de la compresión sin colgarse; un `.bc7` truncado cae a RGBA8; coste de `hashMipLayers` (~132 MB de árboles) en cada arranque; vistas BC7 de los árboles en los rayos (SRGB a UNORM); exportar con imágenes de la UI nombradas en `.cpp`/`.crscene` (la clave del juego tiene que coincidir).
- **Ventana y FPS (0002, 0037, 0039):** con `vsync=0` y sin Mailbox (iGPU AMD) va sin tope y con tearing; el presupuesto apunta al menor de `target_fps` y `fps_limit` (`VulkanRenderer::setFrameRateCap`): comprobar que con tope la calidad no baja.
- **UI (0020-0022):** los scripts dejan de recibir el clic que cae sobre la UI (cambio buscado); un `UIEventTrigger` encima tapa el joystick táctil; en las interfaces en el mundo (VR) el cursor del campo de texto es aproximado.
- **Vídeo:** Android arm64 y armeabi-v7a con un mp4 H.264 de Assets (imagen, colores, audio, bucle, búsqueda), `https://` en Android 9+ y el mensaje en Android 8, HLS con su error claro; si AImageReader no da imagen en 3 s sale un error. Windows sin FFmpeg igual que antes. Linux opcional con `-DCRAMION_WITH_FFMPEG=ON` y un `rtsp://`.
- **VR / adaptativo:** los cambios de escala ya no deben dar el tirón ni «[Vulkan] Filtrado de texturas» (las texturas se ven algo más suaves mientras el presupuesto baja la resolución).
- **Reproductor `--run-tests`:** `start /wait Juego.exe --run-tests --junit r.xml` con y sin pruebas, código de salida y XML. Sin `schema.json`, que falten las pruebas sale como fallo (1) y no como 2.
- **World Partition y red:** en el editor, rejilla y capa nuevas, ocultar la capa, activarla y apagarla en Play, dependencia con dos seleccionados, guardar con «Celdas en disco» y reabrir una escena antigua; el test de 20.000 objetos sigue por debajo de 2 ms; partida real con un jugador a más de 2 km. El protocolo 2 no juega con juegos ni `CramionServer` anteriores (intencionado).
- **Blueprints fase 2:** función pura y con ejecución y cambio de firma con enlaces; macro con dos salidas y Delay; Timeline en Play; Construction Script que escala por variable y otro con Spawn (mover el actor sin copias acumuladas, Ctrl+Z) y su coste en escenas con muchos actores; cables, burbujas y «Vigilar este valor»; renombrar y mover un Blueprint puesto en una escena con su pestaña abierta; un `.crgraph` antiguo guardado sin cambios queda igual.

## Decisiones y restos del 2026-10-19
- **SVGF con sondas dinámicas:** hoy la copia sin SVGF solo va con `!dynamicProbesActive()` (criterio de F1-6). La 0.1 de lightmaps lo quitaba siempre en modo horneado; para eso basta con borrar ese `if`.
- **Escena con lightmaps y sin sondas:** `bakedGiActive()` exige sondas o superficie, así que iría por la rama SSGI y la palanca Gi no baja a 0. Comprobar si un `.crbake` v4 siempre lleva sondas.
- **`LightmapAtlas`** reserva con `vk::raii::DeviceMemory` y `findMemoryType`, sin VMA, y su subida es síncrona (`submitOneTime`).
- **`ShadowCascades::casterMargin()`:** falta el accesor; el 60 del margen está copiado a mano en `VulkanRenderer` (parche 0013).
- **Parche 0032 de Keryan** (LOD por parches): pospuesto, ver `MEJORA.md` §7.
- **`updateModelMaterial`** rehace `alpha_masked` en vivo pero no pide reconstruir la escena de rayos: la división opaco/recortado de la BLAS se queda vieja hasta la siguiente (ya pasaba antes).
- **Restos menores:** `tiny_actors_` sigue por índice; Do Once/Gate dentro de una macro de Blueprint guardan estado por objeto y no por llamada; al renombrar un Blueprint no se actualiza el deshacer de otras pestañas; las luces de una capa oculta en el editor siguen iluminando; asignación automática de rejilla por tamaño en World Partition; FFmpeg: sus DLL no se copian al editor ni al juego, la opción no llega al ExternalProject de Android y `rtsp://` no tiene política de dominio.

## Publicación
- **Cramion Store pública:** subir la API al servidor (copiar `web-api/cramion_api.py` y arrancarlo en lugar del `python3 -m http.server`; pasos en `web-api/README.md`). Hasta entonces el Package Manager dice «No se pudo conectar».
- **Próxima versión:** regenerar el zip con `cramion_package` desde `main` (el de la 2.4.13 se hizo antes de las tandas de optimización). Recompilar Android antes del zip, de uno en uno:
  ```
  cmake --build build-release -j 1 --target cramion_android_arm64-v8a
  cmake --build build-release -j 1 --target cramion_android_armeabi-v7a
  ```
- **Juegos exportados:** volver a exportarlos para que lleven el reproductor nuevo.
- **Iluminación, mientras los lightmaps no estén probados:** con el sol en «Mixta» y volviendo a hornear, las sombras salen como en tiempo real y solo se hornea el rebote.

## Funciones a medias
Escritas el 2026-10-19 (sin compilar); quitar cada línea cuando pasen sus pruebas:
- **Vídeo fase 2:** hecho el backend de Android (MediaCodec) y FFmpeg opcional (`CRAMION_WITH_FFMPEG`, apagado: RTSP/RTMP/DASH, HLS en Android y Linux).
- **World Partition fase 1, estilo Unreal:** hechas varias rejillas, Data Layers y dependencias entre celdas.
- **Red** en coordenadas absolutas a más de 2 km: hecho (`NetPosition`, protocolo 2).
- **Blueprints fase 2:** hechos Construction Script, funciones, macros, Timeline, depuración en vivo y renombrar o mover un Blueprint con su `.crgraph`.
- **VR:** el sesgo de mips ya no depende del presupuesto adaptativo, así que no se rehacen los samplers al cambiar de escala.
- `--run-tests` en el reproductor: hecho (solo Windows).

## Probar en Windows (port sin Lua; solo se compiló desde Linux)
- Crear un proyecto de cada plantilla y darle a Play; el minigolf.
- El autocompletado y la consola de C++.
- Un mod con `main.crgraph`.
- `<Juego>Server.exe` con scripts.
- El editor de máquinas de estados.
- `CppScriptTests` con el compilador incluido: debe decir «clang (incluido)».
- Exportar a Android con el NDK: el `.so` de los scripts y `libcramion_scripthost.so` deben ir en `lib/<abi>/`.

## Fallos conocidos
Arreglados el 2026-10-19 (sin compilar ni probar); quitar cuando se comprueben:
- ~~`CramionTemplateTests` «bloques»: tras romper un tronco no se recoge el objeto. La prueba «coches» falla a veces.~~ El clic se suelta al romperse el bloque; los conos de la plantilla de coches ya no están delante de la salida.
- ~~Líneas oscuras finas en los bordes de las paredes.~~ La normal geométrica elige el lado plano con dos vecinos (`lighting.frag`, `rt_common.glsl`). Confianza media.
- ~~El recorte de sombras no usa `base_color.a`.~~ `alpha_factor` en la sombra y `alpha_masked = texture_alpha || base_color.w < 0.5`.
- ~~Las partículas no se ordenan.~~ Ordenación bitónica por distancia en `vfx_sim.comp` para los efectos con mezcla alfa.
- ~~La corteza de la lava toma el máximo de los materiales.~~ Media ponderada por píxel (destino RG16F nuevo).
- ~~Con el escalado apagado, el DOF y el motion blur acaban en el historial que leen SSR y SSGI.~~ Ya estaba arreglado en 515d2ce.

## Recordatorios
- Cerrar `CramionEditor.exe` antes de compilar (si no, falla el enlace).
- Una sola compilación a la vez; Android aparte. Pruebas de una en una.
- Sin commits ni subidas sin pedirlo.
- El MCP del editor puede quedar en el puerto 7778 si el 7777 sigue ocupado (`CRAMION_MCP_PORT=7778`).
- Restos que se pueden borrar cuando no hagan falta: `<scratchpad>/e2e/test` (copia de 3,5 GB del proyecto `test`, para las pruebas del horneado) y `<scratchpad>/store-test` (Store local de prueba). `<scratchpad>` = `C:\Users\tupap\AppData\Local\Temp\claude\D--Trabajo-NewProyects-Cramion\08c827e0-a428-4eb8-8c9d-cc768252f90b\scratchpad`.

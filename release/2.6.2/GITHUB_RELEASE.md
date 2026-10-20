# Cramion 2.6.2

**Streaming sin tirones, luces sin tope y un editor rápido con escenas grandes.** Llega justo después de la 2.5.1 (de la 2.5.2 a la 2.6.1 no se publicaron) y junta cinco rondas de trabajo: el mundo entra y sale en otros hilos, la escena de rayos es incremental, la sonda de reflexión ya no para la CPU y el editor deja de rehacer la escena entera en cada acción.

## Descarga

- `Cramion-win64.zip`: descomprime y abre `CramionEditor.exe`. Si ya tienes Cramion, el actualizador te avisa solo.
- SHA-256: `bc55e2e1f109656190a6e07683cae5f74326faf200f447d4f5d07b3ce60c976e`

## Lo nuevo

### Streaming sin tirones
- **`Scene.instantiateAsync` / `Scene.destroyDeferred`**: el prefab se lee en los hilos de IO y sus entidades se crean o se borran por tandas con un presupuesto por frame (`scene.SpawnBudgetMs`, `scene.DestroyBudgetMs`). Con `Scene.spawnProgress`, `Scene.spawnResult`, `Scene.cancelSpawn` y `Scene.preloadPrefab`.
- **Plantillas de prefab compiladas**: `Scene.instantiate` es de 9 a 15 veces más rápido (913 entidades: de 6,3 a 0,7 ms).
- **Pool de objetos**: `Scene.pool`, `Scene.acquire`, `Scene.release` y `Scene.poolCount`.
- **Texturas de los árboles en otro hilo**: desaparece el parón de **1 a 2,3 s** del primer árbol.
- **Escena de rayos incremental**: con trazado de rayos, de **7 reconstrucciones seguidas por ciclo** a que cada modelo que entra por streaming se añada solo. La TLAS se cambia sin `waitIdle`.
- **Sonda de reflexión sin parar la CPU**: captura en su propio hueco de frame. Banco `--rt 0`, frames calientes: p99 de **17,5 a 12,4 ms** y frames de más de 16,7 ms de **~170 a 8**.
- Las especies de árboles ya no cambian con cada celda que entra o sale, y los `.crshader` se compilan en segundo plano la primera vez que se usan.

### Memoria
- **Presupuesto de VRAM con LRU**: por encima del 80 % salen primero los modelos que hace más que no se ven.
- Con trazado de rayos, expulsar un modelo ya libera sus mallas (solo se quedan sus texturas).
- La RAM de las texturas de los modelos sin uso se suelta a los 30 s, la caché de prefabs tiene tope (48 MB) y las formas de física se podan.

### Estabilidad
- Arreglado el **`ErrorDeviceLost` con el trazado de rayos apagado** (la captura de la sonda dibujaba la vegetación con comandos sin recortar).
- **Informe de dispositivo perdido** en `%LOCALAPPDATA%/Cramion/Crashes/` y autoguardado de la escena antes de cerrar.
- **Esperas a la GPU con tope** (10 s, `CRAMION_GPU_TIMEOUT_MS`): un cuelgue ya no deja el programa colgado.
- El JobSystem ya no cierra el programa si una tarea lanza una excepción.
- FBX con piel y sin tomas se importan animados, y los FBX con una toma rota se reparan en vez de rechazarse.

### Luces y materiales
- **Luces locales sin tope** por clústeres: antes solo se veían las 32 puntuales y 8 focos más cercanos.
- **Transparentes iluminados** en los `.crmat` (`"blend": "alpha"` y `"additive"`), `"unlit": true`, niebla en todo lo transparente y normal map en el vidrio.
- **Color de vértice** en el `.crmat`, los `.crshader` y el Shader Graph.
- **Parámetros por instancia** en el `MeshRenderer` (`tint`, `alpha`, `emission_scale`, `receive_decals`) sin romper el instancing, y `entity().setMaterialParam()`.
- **Decals**: atlas de texturas, hasta 256, canales normal, ORM y emisión, y «Recibe decals» por objeto.
- **LOD**: por modelo en los ajustes de importación, niveles del archivo (`Roca_LOD0`, `Roca_LOD1`…) y fundido con tramado al cambiar de nivel.
- Sombras de contacto que se funden en la distancia (`contact_shadow_distance`) y luz volumétrica sin cortes al final de las cascadas.

### `.crshader`, Shader Graph y VFX
- Ruidos nuevos sin facetas (`gradientNoise`, `simplexNoise`, `voronoi`, `curlNoise`…), `vertex()` con sombra y vectores de movimiento, y `INSTANCE_DATA(0..1)`.
- **`domain transparent`** iluminado que lee la escena (`sceneColor`, `sceneDepth`, `depthFade`…): distorsión de calor, ondas de choque y refracción.
- Shader Graph con los dominios Opaco / Sin luz / Transparente / Transparente sin luz y la categoría «Escena».
- **VFX Graph**: salidas «Cinta» y «Malla 3D», eventos en la GPU (al morir o chocar, otro efecto) y humo iluminado. Partículas de distorsión en el ParticleSystem.
- Salida **`s.refraction`** en los `.crshader` transparentes (pin «Refracción» en el Shader Graph) y arreglado el error «sin renderizador» de los `.crshader` compilados antes del renderizador.

### VFX Graph completo
- **Cintas**: varias tiras por emisor, suavizado con subdivisiones, orientación propia con giro, UV por distancia que no resbala, y la estela se apaga sola al destruir el objeto. Plantilla «Estela de proyectil».
- **Luz por partícula**: las partículas iluminan el mundo por clústeres, con tope por emisor, global (`render.vfx.MaxParticleLights`) y por calidad.
- **Eventos de la GPU con canales**: «Emitir al morir / al chocar / cada t s» y «Al recibir un evento de la GPU», sin pasar por la CPU; los scripts escriben con `Effects.pushEvent`.
- **Partículas de malla**: variantes y LOD, quad de lejos, «Hacia la cámara», rotación fija, giro en Update y material `.crmat`.
- **Distorsión** en quads, cintas y mallas (apagada en calidad Baja), y **flipbook con fundido** entre cuadros.
- **Escalabilidad por tipo** (Héroe, Combate, Impacto, Ambiente): distancia, número, partículas y salidas por calidad y plataforma, con pool de instancias, ráfagas repartidas y estadísticas por tipo.
- Ruido curl horneado, hasta 64 texturas y escala de tiempo de los efectos.

### Editor
- **Deshacer por diferencias** por entidad y componente: con 10 000 entidades, un paso cuesta 0,22 ms. Hasta 500 pasos.
- **Regiones cargadas de World Partition**: el editor carga solo las celdas cerca de la cámara, las fijadas y las de lo seleccionado.
- **Recarga en caliente** de modelos, materiales, texturas, clips, prefabs, `.crshader` y vídeos editados en disco, y **reimportar al cambiar el FBX original**.
- Mover o renombrar en el panel Proyecto **actualiza las referencias** en todo el proyecto.
- **Autoguardado** con recuperación al abrir la escena, también si el editor se cierra por un error.
- Física de edición incremental, selección y clic de la Escena más rápidos, y segundo plano configurable (Ahorro, Vista en vivo, Sin límite).
- Arreglado el **tirón de ~200 ms cada 2 s** del editor (también con la escena vacía).
- Logos sin fondo negro y pantalla de carga del proyecto sin letras rotas.

### Scripts de C++
- **`onEnable()` / `onDisable()`**: desactivar un script ya no lo destruye.
- Un viaje al proceso por fase y API por índice y en binario (`setQuaternion` de 3,97 a 0,21 µs en el motor), sin enviar los eventos que la clase no implementa.
- `Entity::setQuaternion` y `Scene::setTransforms` en lote.
- **Juego exportado (Windows)**: los scripts se cargan en el propio proceso, sin `CramionScriptHost`.

### Mando y MCP
- **Mando en Windows y Linux**: XInput, DualShock 4 y DualSense (USB y Bluetooth) y evdev, hasta 4 a la vez, con vibración (`Input.setGamepadVibration`) y dispositivo activo para los iconos (`Input.getActiveDevice`).
- **MCP con token de sesión**: el editor exige un token y rechaza las peticiones de otras webs.

### Para depurar
- Release con símbolos (`.pdb` archivados por versión), nombres y etiquetas de Vulkan con la validación activa, `CRAMION_GPU_CHECKPOINTS=1`, `CRAMION_VK_SYNC_VALIDATION=1` y `CRAMION_VK_GPU_AV=1`.
- `cramion_stream_bench` con `--async`, `--vsync` y el desglose de cada frame lento; `cramion_render_bench --far`.

## Cambios a tener en cuenta
- **`.crshader`**: los juegos exportados que los usan hay que volver a exportarlos (cambió el registro por instancia).
- **`domain transparent`** ahora se ilumina; para el comportamiento de antes, `domain transparent unlit`.
- **Scripts de C++ en el juego exportado (Windows)**: van en el propio proceso, sin aislamiento (un fallo de memoria en un script cierra el juego). `script.cpp.InProcess=false` vuelve al proceso aparte.
- **Scripts de C++**: desactivar y reactivar ya no repite `awake`/`start`; el reinicio de un objeto reutilizado va en `onEnable`. Una función cuyo `api::Doc` no dice «Devuelve» pero devuelve algo recibiría nil. El motor y `CramionScriptHost` tienen que salir del mismo build.
- **MCP**: los clientes HTTP directos reciben 403. Usa el puente (`CramionMcp.exe`), manda el token o desmarca «Exigir token MCP».
- **GPU**: una espera de más de 10 s se trata como dispositivo perdido; `CRAMION_GPU_TIMEOUT_MS` la sube.
- **JobSystem**: `wait()` vuelve a lanzar la excepción de una tarea del grupo.
- **Modelos**: la caché pasa a la versión 13 (se rehace sola); para el color de vértice hay que reimportar. Las herramientas externas que lean el `.crdata` sin usar el tamaño de cada pieza tropiezan con el bloque nuevo del final.
- **Deshacer**: un componente que cambia al deshacer se quita y se vuelve a poner. Si algún `reflect` tiene estado mutable, `editor.undo.Parallel=0`.
- **Física en edición**: un collider cambiado sin moverlo, sin Inspector y sin deshacer puede tardar unos segundos en recogerse; `physics.IncrementalEditSync=0` vuelve a lo de antes.
- **World Partition en el editor**: hornear luz, navmesh y static batching solo ven las celdas cargadas (pulsa «Cargar todo» antes).
- **Mando**: con DS4Windows o Steam Input el mismo mando puede verse dos veces; `CRAMION_GAMEPAD_HID=0` o HidHide. La zona muerta ahora es radial.
- **Clic en iconos superpuestos** de la Escena: elige el más cercano, no el primero de la jerarquía.
- **Vidrio**: su set pasa de 10 a 12 bindings. **VFX Graph** en modo compatible pide 10 storage buffers por shader (antes 7).

- **VFX Graph**: el precalentado se reparte en varios frames (`vfx.prewarmFrames 1` para el de antes) y la turbulencia usa ruido horneado (`vfx.curlNoiseBaked false`). Los `.crvfx` de antes no tienen tipo y no se recortan.

El detalle completo está en `CHANGELOG.md`.

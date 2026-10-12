# Cramion 2.4.8

**Blueprints de actor, cliente WebSocket, hierba realista y un mundo que se carga en otros hilos.**

Esta versión sigue a la 2.4.6 (la 2.4.7 no se publicó). Trae Blueprints de actor con un editor de nodos al estilo de Unreal y 56 nodos nuevos, un cliente WebSocket para los scripts en Windows, Linux y Android, hierba y árboles con volumen, y streaming del terreno, la física, la navegación y el World Partition fuera del hilo principal. También corrige los siete fallos del informe de fallos y muchos fallos de render, de escenas y del editor.

## Lo más destacado

- **Blueprints de actor**: un prefab con su Event Graph y su propia pestaña en el editor.
- **Editor de nodos estilo Unreal**: reroutes, minimapa, comentarios, alinear, búsqueda sensible al contexto, deshacer y *Compilar* (F7).
- **Cliente WebSocket**: `ws://` y `wss://` desde C++ (`WsClient`) y Visual Scripting (*On WebSocket*).
- **Hierba realista**: hojas con volumen, sombreado propio, auto-sombra dentro del césped y cantidad según el hardware.
- **Árboles con volumen**: hojas mate, auto-sombra de la copa y sombras cercanas con la malla de detalle.
- **Streaming en otros hilos** con presupuesto de milisegundos por frame, y servidor dedicado que carga alrededor de cada jugador.
- **Filtrado anisotrópico y sesgo de mips** en los ajustes gráficos.

## Blueprints de actor

- Un Blueprint es un `.crprefab` con un bloque `"blueprint"` (padre, grafo, descripción, categoría, tick) y un `.crgraph` al lado.
- Se crea desde un modelo, desde un prefab o vacío, y un prefab existente se puede convertir. El doble clic en un modelo abre el modal *Nuevo Blueprint de actor* (con la opción *Solo ponerlo en la escena*).
- Pestaña Blueprint con los paneles Componentes, Mi Blueprint, Detalles, Ventana gráfica, Event Graph y Resultados de compilación. Insignia «BP» en el navegador del proyecto.
- Ctrl+S guarda el Blueprint y Ctrl+Z/Y van al grafo cuando está enfocado. Antes de Play se compilan y guardan los Blueprints con cambios. Las variables expuestas salen en el Inspector.
- Los huesos de un modelo se crean como objetos desde el núcleo, y `BoneSocket` puede apuntar a un objeto hueso.

## Editor de nodos y Visual Scripting

- Nodos compactos, de variable y reroute; pin de ejecución en pentágono.
- Cables seleccionables: doble clic crea un reroute, Alt+clic corta, Ctrl+arrastrar mueve los enlaces.
- Minimapa (M), comentario de la selección (C), enderezar (Q), alinear (Mayús+WASD) y teclas de nodo (B/S/D/N/O/G/1-4).
- Snap a la rejilla **mientras arrastras** los nodos, como Unreal (Alt para moverlos libres).
- Arrastra un objeto desde **Componentes** al Event Graph: crea su referencia relativa a Self (ruta desde la raíz, así funciona en cada instancia). Con Ctrl, menú con sus componentes y **Get Component** ya conectado.
- Búsqueda sensible al contexto según el tipo del pin arrastrado y conversión automática entre tipos.
- Nodos desactivados, avisos y burbujas. Deshacer por documento y *Compilar* (F7).
- **56 nodos nuevos** de flujo (Switch on Int/String, For Loop/For Each with Break, MultiGate, Retriggerable Delay, Is Valid, Select), matemáticas, conversiones, texto, listas, componentes, física (Line Trace, Break Hit Result), vectores y red (On WebSocket).

## Cliente WebSocket

- API de script: `WebSocket.connect`, `send`, `sendBinary`, `sendJson`, `close`, `closeAll`, `on`, `off`, `poll`, `state`, `info`, `find`, `list`, `count`, `setHeader`, y `Base64.encode`/`decode`.
- SDK de C++: `WsClient` (`sdk/cramion/WsClient.h`), con `attach(nombre)` para recuperar una conexión tras una recarga.
- Windows usa WinHTTP. Linux y Android usan un motor RFC 6455 propio sobre TCP, con OpenSSL en Linux y `SSLSocket` por JNI en Android.
- `wss://` exige TLS y un certificado válido. `ws://` sin cifrar solo se permite hacia este PC, salvo que se cambie `net.WsInsecure`.
- Reconexión con espera creciente, heartbeat opcional, límites de tamaño de mensaje y de cola, y cierre limpio al salir.
- Las conexiones con nombre sobreviven a `Scene.load`; las que no tienen nombre se cierran.

## Vegetación

- **Hierba**: normal redondeada por píxel, dos rejillas y tres distancias, ancho mínimo de unos 3/4 de píxel y desvanecido al final.
- **Nuevo modelo de sombreado de hierba**: difuso envolvente, brillo de un lóbulo y translucidez fina. El modelo de pelo no cambia.
- **Suelo bajo la hierba** con el color del césped, que se apaga con las briznas.
- **Cantidad por nivel**: densidad ×0,35 (Bajo), ×0,6 (Medio), ×1 (Alto) y ×1,6 (Ultra).
- **Nuevos valores por defecto**: densidad 500 (antes 70), anchura 0,007 (antes 0,028), altura 0,35 (antes 0,45), detalle 12 m (antes 22).
- **Árboles**: hojas mate con inclinación propia, normal que sigue al temblor, auto-sombra de la copa, variación de color, borde con ruido (no en Bajo) y sombras cercanas con la malla de detalle en Alto y Ultra.

## Streaming y rendimiento

- **Jobs** con prioridades, un grupo aparte para el disco, cancelación y presupuesto por frame.
- **Terreno**: teselas leídas en el hilo de disco y cosidas en un trabajador; al soltar una tesela se guarda en segundo plano.
- **Física**: collider del terreno en segundo plano y `MeshCollider` con carga diferida del modelo.
- **Navegación**: triángulos del terreno desde una foto inmutable en los hilos de navegación.
- **GPU**: terrenos preparados en otro hilo, subidas con tope por frame, pool de bloques de hierba, capas leídas en segundo plano y vegetación subida sin parar la GPU.
- **World Partition**: celdas leídas, parseadas y escritas en otros hilos, y recreadas con presupuesto por frame.
- **Servidor dedicado**: carga alrededor de cada jugador y del punto de aparición, no de (0,0,0).
- **Informe de frames lentos** con sección de streaming, en el editor y en el Player con `CRAMION_PERF_LOG=1`.
- **Sonda de reflexión** más barata: intervalo, tiempo mínimo y distancia según el nivel de la GPU, y sin hierba en la captura.

## Texturas

- Filtrado: Automático (4x/8x/16x/16x según el nivel; en móvil 2x o 4x), apagado, 2x, 4x, 8x o 16x. Sesgo de mips de −1 a 2.
- Nueva sección *Texturas* en los ajustes gráficos. Las opciones que la GPU no admite salen desactivadas.
- API de script: `Graphics.setTextureQuality`, `setTextureFiltering`, `setTextureLodBias` y sus lecturas. También en el MCP.

## Fallos corregidos

**Del informe de fallos**
- Las pruebas `CRAMION_TEST` con `co_await` pasaban sin ejecutarse.
- El MCP no decía que las posiciones son locales al origen flotante. Ahora lo dice y acepta `absolute: true`.
- El editor bajaba a 10 FPS sin foco durante Play. Hay aviso en consola si la física pierde tiempo, y `Physics.simulatedTime`.
- La recarga en caliente con corrutinas vivas ahora avisa, y las pruebas a medias se cierran como fallidas.
- La consola en modo edición: `Scene.destroy` y `Entity:destroy` borran, `e.parent` se puede escribir y se puede deshacer.
- Compilar desde GitHub sin `assets/` funciona (sin icono).

**Render**
- NaN de la niebla con densidad 0; nubes desde dentro o encima de la capa.
- Normales espejadas con escala negativa; vectores de velocidad inf/NaN (también en FSR/DLSS).
- Costura de luz entre trozos del terreno y escalón entre teselas al esculpir.
- Cierre por falta de descriptores del terreno: los pools crecen bajo demanda.
- Desplazamiento del origen flotante (matrices del frame anterior, sondas, oclusión del cielo).
- Barreras de la swapchain y de la historia SSR; cascadas de sombra con cámara ortográfica.
- Con más de 16 masas de agua se usan las más cercanas.

**Escenas, prefabs y editor**
- La pausa destruía personajes y ragdolls.
- `Scene.load` en Play dejaba restos del nivel anterior (World Partition, multitudes, iluminación).
- Iluminación horneada aplicada a la escena equivocada.
- Referencias entre entidades al duplicar, pegar o instanciar, y UUID repetidos.
- World Partition con origen flotante y Static Batching con objetos descargables.
- Rutas con caracteres fuera de ANSI, *Combinar mallas*, reimportar con Blueprints y el editor de rejilla de terreno.
- Cambio de tipo de área en la interfaz e IDs de ImGui repetidos.
- Al cambiar de espacio de trabajo (escena ↔ Visual Script, prefab o Blueprint) cada área conserva el editor elegido, también tras reiniciar.
- Ids de callback de C++ que chocaban a partir del proceso 512, `onDestroy` con el objeto aún válido y JSON con UTF-8 inválido.

## Cambios incompatibles y migración

- **Pruebas**: las pruebas con `co_await` ahora se ejecutan de verdad. Las que pasaban en falso pueden fallar, y una corrutina que no llega a empezar cuenta como fallida.
- **Recarga en caliente**: las corrutinas no se conservan, así que hay que relanzarlas en `onAfterReload`. Las escuchas WebSocket de C++ se pierden: usa `WsClient::attach(nombre)`.
- **SDK de C++**: el proceso ocupa 31 bits en los ids de callback, y hay `WsClient.h` y `Entity::setParent`. Los scripts se recompilan solos.
- **Prefabs**: se conservan las claves que el motor no conoce. Las referencias internas de una instancia se guardan con los ids del prefab.
- **Hierba**: las escenas con los valores de fábrica sin tocar (0,028 / 70) se dibujan con 0,007 / 500. La altura, el color y la distancia guardados no cambian.
- **World Partition**: celdas con cabecera v2 (la v1 se sigue leyendo), calculadas con la posición absoluta.
- **Servidor dedicado**: hace streaming alrededor de los jugadores y del punto de aparición.
- **MCP**: las posiciones son locales al origen flotante; `absolute: true` es nuevo.
- **Editor**: el doble clic en un modelo abre el modal de Blueprint, el editor de Visual Script ya no guarda solo y `Scene.destroy` en la consola borra en modo edición.
- **Compilación**: `CMAKE_CXX_SCAN_FOR_MODULES OFF`; `assets/` es opcional. En Linux, `wss://` necesita `libssl-dev` (`CRAMION_WITH_OPENSSL`).

## CVars nuevas

| CVar | Por defecto | Qué hace |
|---|---|---|
| `stream.AsyncTerrain` | true | Teselas leídas y cosidas en otros hilos |
| `stream.MainBudgetMs` | 2.0 | ms del hilo principal para integrar |
| `stream.MaxInFlight` | −1 (auto) | Teselas cargándose a la vez |
| `stream.IoThreads` | −1 (auto) | Hilos de disco |
| `stream.UploadMBPerFrame` | 16 | MB de terreno subidos a la GPU por frame |
| `render.terrain.AsyncUpload` | true | Preparar terrenos en otro hilo |
| `render.terrain.AsyncPrepares` | 2 | Terrenos preparándose a la vez |
| `render.terrain.GrassPool` | 6 | Bloques de hierba compartidos (0 = uno por tesela) |
| `render.terrain.AsyncLayers` | true | Texturas de capas en otro hilo |
| `render.foliage.AsyncUpload` | true | Vegetación subida sin parar la GPU |
| `render.rt.AsyncTerrain` | true | Malla del terreno para rayos en otro hilo |
| `render.probe.FaceInterval` | 0 (auto) | Frames entre caras de la sonda |
| `render.probe.MinSeconds` | 0 (auto) | Segundos mínimos entre capturas |
| `render.probe.DrawDistance` | 0 (auto) | Distancia de dibujo de la captura |
| `render.probe.Terrain` | true | Terreno en la captura |
| `physics.AsyncTerrain` | true | Collider de terreno en segundo plano |
| `physics.AsyncTerrainBudgetMs` | 1.0 | ms por frame para meter colliders |
| `physics.AsyncTerrainMaxWaitFrames` | 3 | Espera antes de hacerlo en el hilo principal |
| `physics.DeferMeshLoad` | true | `MeshCollider` pide el modelo en segundo plano |
| `physics.DeferMeshMaxFrames` | 120 | Frames de espera al modelo |
| `nav.AsyncTerrainGather` | true | Triángulos del terreno en hilos de navegación |
| `wp.AsyncCells` | true | Celdas leídas y parseadas en otros hilos |
| `wp.RestoreBudgetMs` | 2.0 | ms por frame para recrear objetos |
| `wp.MaxIoInFlight` | 8 | Celdas leyéndose a la vez |
| `perf.SlowFrameMs` | 150 | Umbral del informe de frames lentos |
| `net.WsInsecure` | 0 | `ws://` sin cifrar (0 este PC, 1 red local, 2 cualquiera) |
| `net.WsMaxConnections` | 16 | Conexiones WebSocket a la vez |
| `net.WsMaxMessageKB` | 4096 | Tamaño máximo de mensaje recibido |
| `net.WsMaxEventsPerFrame` | 512 | Eventos entregados a scripts por frame |
| `net.WsLog` | 0 | Registro de WebSocket en consola |

Variable de entorno nueva: `CRAMION_PERF_LOG=1` (Player).

## Limitaciones conocidas

- **Hierba**: no proyecta sombra en el mapa de sombras (su auto-sombra es analítica) y no sale en la sonda de reflexión.
- **Trazado de rayos**: cada reconstrucción de la malla del terreno sigue esperando a la GPU; solo se limita la frecuencia.
- **Filtrado de texturas**: cambiarlo espera a la GPU y rehace los samplers.
- **Blueprints**: renombrar el `.crgraph` rompe la ruta. Construction Script, funciones, macros, dispatchers y depuración paso a paso llegarán más adelante. Con «Huesos: Todos», cada instancia lleva una entidad por hueso (estimado: de 60 a 200 en un modelo típico).
- **WebSocket**: sin OpenSSL en Linux no hay `wss://`. Con WinHTTP, el timeout de inactividad automático solo se activa con heartbeat. Falta probarlo en dispositivos Android y en redes reales.
- **Documentación**: las CVars de streaming, física y World Partition y `perf.SlowFrameMs` aún no están en el manual.

## Descarga

1. Descarga **`Cramion-win64.zip`** y **`Cramion-win64.zip.sha256`** de los archivos de esta versión.
2. Comprueba el zip. En PowerShell, en la carpeta de descarga:

   ```powershell
   (Get-FileHash .\Cramion-win64.zip -Algorithm SHA256).Hash.ToLower()
   Get-Content .\Cramion-win64.zip.sha256
   ```

   Los dos valores deben coincidir. En Linux o Git Bash: `sha256sum -c Cramion-win64.zip.sha256`.
3. Descomprime y abre `CramionEditor.exe`. Si ya tienes Cramion, el actualizador del editor ofrece la 2.4.8.

Requisitos: Windows 10/11 x64 y una GPU con Vulkan.

La única descarga oficial es la de esta página, y es gratis. El registro completo está en [CHANGELOG.md](https://github.com/MantraxStudios/Cramion/blob/main/CHANGELOG.md).

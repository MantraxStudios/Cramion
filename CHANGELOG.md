# Cambios

## 1.4.0

### DataPacks (como los AssetBundles de Unity)
- **Escenas enteras y objetos** con todo lo que usan (modelos, materiales, texturas, prefabs, scripts, sonidos, animaciones, shaders, terrenos, interfaces...) en un solo archivo `.datapack`, **sin el ejecutable**: niveles descargables, DLC, mods, skins, vehículos o personajes que se reparten aparte del juego.
- **Archivo > Exportar escena como DataPack...** y **clic derecho en un objeto de la Jerarquía > Empaquetar y exportar como DataPack...** (el objeto con sus hijos se guarda como prefab y se empaqueta). La ventana deja mezclar escenas y objetos, elegir nombre y carpeta, y exporta **en segundo plano con barra de progreso y Cancelar**, como *Exportar juego*.
- Las dependencias se buscan solas, recursivamente: por los UUID de los assets y por las rutas que nombran los componentes y los scripts Lua (`"Sonidos/disparo.wav"`, `Scene.load("Nivel2")`...). Lo que no se usa no entra.
- Lua: `DataPack.loadScene("Nivel2")`, `DataPack.instantiate("Vehiculos", "Deportivo", Vec3(0, 1, 5))`, `DataPack.load`, `DataPack.info` (sin montar), `DataPack.unload`, `DataPack.list`, `DataPack.isLoaded`. El paquete se busca junto al juego, en `DataPacks/` o por ruta.
- Al montarse **nunca sobrescribe** los archivos del juego; en el editor se quita al parar Play (el proyecto queda como estaba) y en el juego al cerrarlo. Si se cerró de golpe, se limpia al volver a abrir.
- MCP: herramienta `export_datapack` (escenas, prefabs o una entidad; `async` para verlo con la barra de progreso). Manual: *DataPacks (bundles)*.

### Editor
- **Al dar Play**: flecha junto a *Paso* para elegir, como en Unity, si se salta a la pestaña **Juego** o se queda en la vista actual. Se recuerda entre sesiones.
- **IntelliSense** completado: `DataPack`, `Screen` y `XR` entre las globales con su ayuda, `Input.getActions`, `Input.isActionOngoing`, `Input.clearMappingContexts`, `Mathf.negativeInfinity` y `Scene.instantiate` con prefabs por ruta.

### Sombras por rayos de las luces locales
- Con el trazado de rayos activado, las **luces puntuales y los focos proyectan sombras trazadas** contra la escena real (`rt_shadows.comp`): en cada píxel, las 4 luces que más le aportan. Vale para **todas** las luces locales con sombra, no solo las 8 puntuales y 8 focos que tienen mapa de sombras.
- **Radio de la fuente** nuevo en el componente Light (0,05 m por defecto): bombilla pequeña = sombra nítida; lámpara grande = penumbra que se ensancha con la distancia.
- Los personajes animados, el terreno y los vóxeles siguen con los mapas de sombras; se usa la más oscura de las dos. Coste medido: ~0,5 ms con 16 luces (RTX 4060 Ti).

### Trazado de rayos de las GPUs actuales
- **Opacity micromaps** (`VK_EXT_opacity_micromap`): el alfa del follaje recortado se hornea en la GPU (`rt_omm_bake.comp`, 256 microtriángulos por triángulo) dentro de las estructuras de rayos. Donde la hoja es opaca o el hueco transparente seguro, el hardware decide sin ejecutar el shader; lo dudoso lo sigue probando el shader, así que la imagen no cambia (comprobado: diferencia media < 1/255 en reflejos de espejo). `CRAMION_NO_OMM=1` lo desactiva.
- **Shader Execution Reordering** (`VK_EXT_ray_tracing_invocation_reorder`): path tracing con pipeline de rayos (`path_trace.rgen`, any-hit para el alfa) que reordena los hilos por material antes de sombrear. **Opcional** (`CRAMION_SER=1`): con el sombreado del motor medí 0,31 ms por muestra con el compute de siempre, 0,37 con pipeline de rayos y 0,48 con SER (RTX 4060 Ti), así que por defecto sigue el compute.
- Detección de micromaps, pipeline de rayos, SER y mesh shaders al crear el dispositivo (se ve en la consola).

### Mesh shaders (`VK_EXT_mesh_shader`)
- Cada clúster de los escenarios se parte al cargar en **meshlets** de hasta 64 vértices y 124 triángulos (meshoptimizer), con su esfera y su cono de normales, y se detecta si la malla es **cerrada** (cada arista entre dos triángulos, soldando las costuras).
- **Sombras del sol por meshlets** (`shadow_meshlet.task/.mesh`): la GPU descarta los meshlets fuera de cada cascada y, en mallas cerradas, los que miran en contra de la luz (la profundidad del mapa no cambia). Lo recortado por alfa y las luces locales siguen como antes. Medido en el proyecto *minecraft* en Play (RTX 4060 Ti): sombras 10,6 → 9,5 ms, de 56 a 61 FPS.
- **Geometría por meshlets** en mallas cerradas (`gbuffer_meshlet.task/.mesh` + el fragment shader de siempre): usa los clústeres visibles que ya decide el culling en GPU (frustum y oclusión Hi-Z en dos fases) y descarta por meshlet lo que queda fuera o de espaldas. Las mallas abiertas (hierba, follaje) siguen por el camino de siempre: ahí los meshlets no ahorraban nada.
- Misma imagen con y sin (comprobado píxel a píxel). `CRAMION_NO_MESH=1` los desactiva.

### Corregido
- **Plano lejano de la cámara**: cambiar *Far* (y *Near*) en el componente Camera no hacía nada; ahora recorta la vista del juego y la de las cámaras con Render Texture.
- **Visión nocturna**: lo que ilumina una farola se descoloraba al apartar la vista de la bombilla o al alejarse (se aplicaba a la imagen final según la luz de cada píxel). Ahora se aplica en la iluminación solo a la luz de la luna y del cielo nocturno, con la fuerza que marca la hora: farolas, antorchas y focos conservan su color siempre.

## 1.3.0

### Iluminación de día y de noche
- **Visión nocturna** (efecto Purkinje): con poca luz el ojo ve con los bastones, que no distinguen colores y son más sensibles al azul. Lo que solo ilumina la luna se vuelve gris azulado; lo que alumbra una farola o una antorcha conserva su color. Ajuste *Visión nocturna* (0..1) en el PostProcessing, grupo *Exposición* (1 por defecto; de día no cambia nada).
- **Cielo nocturno** nuevo: luna con mares, cráteres, oscurecimiento del borde y halo en la bruma; estrellas en dos capas (pocas brillantes, muchas tenues) con el color de su temperatura; Vía Láctea tenue con nubes de polvo. La luna tapa las estrellas de detrás.
- **Luz de la luna** más tenue (0,35 → 0,22 del sol del motor) y casi blanca, y ambiente nocturno menos saturado: el azul lo pone la visión nocturna.
- Salida con la **curva sRGB exacta** en vez de una gamma 2.2: sombras más profundas, no lavadas.
- **Luces dentro de su lámpara**: el plano cercano de las sombras de luces puntuales y focos pasa de 5 a 20 cm (el de Unity). Una luz puesta dentro de la bombilla o del poste de una farola quedaba tapada por ellos y solo alumbraba por las rendijas.

### Sombras más rápidas
- Los **personajes animados** ya no obligan a redibujar cada frame las cascadas lejanas del sol (las que cubren todo el mapa): en una escena con mucha hierba, Play pasó de 39 a 58 FPS (sombras de 18 a 10 ms).
- **Caché de lo estático** en las cascadas (como los *cached shadow maps* de Unreal): lo estático se dibuja por turnos en una copia y cada frame solo se añaden los personajes animados encima. Hasta mapas de 4096.

### Terminal con IA en el editor
- **Ventana > Terminal (IA)**: consolas de Windows de verdad (ConPTY) dentro del editor, con pestañas, colores, historial, selección y copiar/pegar. Mientras escribes ahí, los atajos del editor no se disparan.
- Botón **Claude Code**: lo abre en la carpeta del proyecto y ya conectado al servidor MCP del editor (sin tocar la configuración global de Claude). También PowerShell, CMD, Codex, Gemini CLI e *Instalar Claude Code*.
- MCP: `set_gizmo` con `show_gizmos` (capturas limpias de la vista de escena) y `performance_stats` con la exposición y la luminancia de la escena.

### Escalado
- **AMD FSR 3.1** y **NVIDIA DLSS 4** (modelo transformer), marcados *(INESTABLE)*. El contexto ya no se rehace cada vez que el presupuesto adaptativo cambia la resolución (daba tirones y más lag); al elegirlos pasan a *Calidad* (en *Nativa* solo hacen antialiasing y cuestan FPS). Solo ganan FPS si la escena depende de la resolución.
- Cambiar la resolución interna rehace solo los destinos de render, no la swapchain.

## 1.2.0

### Input Actions (como el Enhanced Input de Unreal)
- **Archivo > Entrada del proyecto**: acciones con tipo de valor **Bool, Axis1D (float), Axis2D (Vec2) y Axis3D (Vec3)** y **varios contextos** (configuraciones) que asignan a cada acción muchas teclas, botones del ratón, del mando, sticks o el joystick táctil. Se guarda en `ProjectSettings/InputActions.json`.
- **Modificadores** por tecla y por acción: Negate, Swizzle (YXZ, ZYX, XZY...), Dead Zone (radial o por eje) y Scale. **Triggers**: Down, Pressed, Released, Hold, Hold And Release, Tap, Pulse y Chord, con eventos started / ongoing / triggered / completed / canceled.
- Contextos con **prioridad**: el de más prioridad se queda las teclas compartidas. Se activan y quitan en el juego.
- Pestaña **Depurar** en Play con el valor y el estado de cada acción; botón para asignar una tecla pulsándola.
- Lua: `Input.getAction`, `Input.getActionState`, `Input.isActionTriggered`, `Input.wasActionStarted/Completed/Canceled`, `Input.bindAction`, `Input.addMappingContext`, `Input.removeMappingContext`, `Input.rebind`, `Input.saveBindings`, `Input.resetBindings`, `Input.getBindings`, `Input.anyKeyPressed` (menú de opciones de controles).
- Configuración por defecto: Move, Look, Jump, Sprint, Fire, Aim, Interact, Zoom (contexto Default) y Fly (contexto Vuelo). Manual: *Input Actions*.

### Realidad virtual (OpenXR)
- **Juegos en VR con cualquier casco de PC** por OpenXR: SteamVR (Index, Vive, Pico), Meta Quest por Link/Air Link, Windows Mixed Reality, Varjo... Cada ojo con todo el render del motor; la ventana hace de espejo. Sin casco, el juego arranca normal.
- **Configuraciones de compilación > Realidad virtual (OpenXR)** para el juego exportado y **Editar > Play en realidad virtual** para probar en el editor.
- **XR Origin** y **XR Controller** (como en Unity): *GameObject > Realidad virtual > XR Origin* crea el rig con la cámara (la mueve el casco) y las dos manos. Origen de seguimiento suelo (de pie) u ojos (sentado). Sin rig, cualquier escena se ve en VR desde su cámara principal.
- Los mandos en **Input Actions** (`XR Left Stick`, `XR Right Trigger`, `XR Right Primary`...), ya puestos en las acciones por defecto: un juego hecho con acciones funciona en VR sin tocar código. Perfiles: Meta Touch, Index, Vive, WMR y el genérico.
- Lua: tabla **`XR`** con cabeza, mandos (mano y puntero), `getAimRay`, gatillo, agarre, stick, botones, `vibrate`, origen de seguimiento. Manual: *Realidad virtual (XR)*.

### Telas (Cloth)
- Componente **Tela (Cloth)**: simulacion de tela con los soft bodies de Jolt. Cuelga, se arruga, ondea con **viento y rachas** y **choca con colliders y rigidbodies** (y los empuja). Particulas fijadas a la entidad (borde de arriba, esquinas, borde izquierdo, centro) que la siguen al moverla; rigidez, resistencia a doblarse, masa, friccion, grosor e iteraciones.
- Se dibuja con el skinning por GPU (una particula = un hueso): material .crmat del Mesh Renderer o color propio, doble cara, sombras, motion blur y culling con la caja real de la tela.
- *GameObject > Fisica > Tela: cortina / bandera / sabana que cae*. Lua: `entity:resetCloth()`, `entity:addClothImpulse(Vec3)` y los campos con `setField("Cloth", ...)`.

### Cuerpos blandos (gelatina)
- Componente **Cuerpo blando (gelatina)** (`SoftBody`): esfera o cubo blando simulado con Jolt, con **presion interna** que conserva el volumen. Cae, rebota, se aplasta, tiembla y vuelve a su forma; choca con colliders y rigidbodies y los empuja. Firmeza, presion, frenado, rebote, friccion, masa y resolucion.
- La entidad sigue al centro del cuerpo (scripts y camaras). Se dibuja con el skinning por GPU como las telas, con el .crmat del Mesh Renderer o su color.
- *GameObject > Fisica > Cuerpo blando: gelatina (cubo) / pelota*. Lua: `entity:addSoftBodyImpulse(Vec3)`, `entity:resetSoftBody()` y `setField("SoftBody", ...)`.

### Editor de scripts
- **IntelliSense completo**: la API sale del propio motor (todas las tablas y funciones, también `XR` y `Screen`), sabe el tipo de cada expresión (entidades, `Vec3`, `Quat`, mallas, choques de `Physics.raycast`) y lo sigue por las variables del script, y dentro de los textos sugiere lo del proyecto: componentes y sus campos, acciones y contextos de entrada, teclas, tags, objetos, escenas, prefabs, sonidos, materiales y botones de VR. Firma de la función con el argumento actual resaltado.
- Corregido el **temblor al escribir**: el texto coloreado se dibujaba un frame tarde respecto al cursor; el scroll ya no salta al añadir líneas al final y el cursor no parpadea mientras escribes.

## 1.0.0

### Android
- **Exportar a Android**: en *Configuraciones de compilación*, *Plataforma: Android*. Sale un **APK** para instalar directamente, un **AAB** para Google Play, o los dos, y con **OBB** si los assets van aparte (`main.<versión>.<paquete>.obb`). Todo con las herramientas del Android SDK, sin Gradle ni Android Studio abierto.
- Configurable: paquete, código de versión, Android mínimo y objetivo, orientación inicial, permisos (Internet, vibrar, micrófono), icono con sus 5 densidades, firma con tu keystore (o una clave de depuración que se crea sola) y perfil móvil (calidad inicial y FPS objetivo con el presupuesto adaptativo).
- **Exportar e instalar**: lo instala por USB o en el emulador con adb, sube el OBB y abre el juego.
- El juego en el móvil es el mismo reproductor: render Vulkan 1.3, física, Lua, audio, interfaz, red y `Http` (con la pila TLS de Android). Se pausa y vuelve de segundo plano sin perder nada; los logs van a `logcat`.
- Los shaders propios (`.crshader`) se precompilan al exportar (en el móvil no hay compilador).
- El motor compila el runtime de Android solo si está instalado el Android NDK (`CRAMION_ANDROID`), y el zip lo trae listo.
- Funciona en **GPUs de móvil sin `drawIndirectCount` ni `multiDrawIndirect`** (Mali y muchas Adreno): el culling en GPU dibuja entonces comando a comando. Probado en una tablet con Mali-G52.
- **Perfil móvil**: según la calidad elegida se apagan los efectos que un móvil no aguanta (motion blur, profundidad de campo, volumétrica, GI y reflejos de pantalla, nubes en Baja/Media) aunque la escena los pida, y el cielo y el IBL se recalculan cada 8 frames.
- **Arranque rápido**: los assets se leen directamente del APK (sin copiarlos fuera) y se descomprimen en paralelo con todos los núcleos: 46 MB en medio segundo en una tablet de gama media. La descompresión en paralelo también acelera el primer arranque en Windows.
- Si el móvil no tiene Vulkan 1.3 el juego lo explica en el log (qué le falta) y se cierra limpio en vez de fallar.

### Controles táctiles (como el Touch Interface de Unreal)
- **Archivo > Controles táctiles**: joystick que aparece donde se apoya el pulgar, zona para mirar arrastrando, botones que pulsan teclas o el ratón (Saltar = Espacio, Disparar = Mouse0...). Se colocan arrastrándolos sobre una vista previa del móvil y se guardan en el proyecto (`ProjectSettings/TouchInterface.json`).
- Un script hecho para teclado y ratón funciona sin cambios: el joystick mueve `Horizontal`/`Vertical`, arrastrar es `mouseDelta`, un toque corto es un clic.
- Los botones de la interfaz (Canvas) tienen prioridad sobre el joystick.
- Botón **Táctil** en la vista Juego: se prueban en Play con el ratón como dedo.
- Lua: `Input.setTouchControls`, `Input.setTouchButton(texto, visible)`, `Input.setTouchJoystick`, `Input.setTouchLook`, `Input.touchCount`, `Input.getTouch`, `Input.isMobile`, `Input.vibrate`.

### Mando y pantalla
- **Mando** (Bluetooth o USB en Android): `Input.getGamepadButton`, `Input.getGamepadAxis`, `Input.isGamepadConnected`; el stick izquierdo y la cruceta mueven `Horizontal`/`Vertical` y el derecho mira.
- **Orientación desde Lua**: `Screen.setOrientation("auto" | "landscape" | "portrait" | "landscape_fixed" | "portrait_fixed")`, y `Screen.width`, `Screen.height`, `Screen.orientation`.

### Render
- Preset de post-procesado **Ultra realista**: todos los efectos prendidos con valores de cámara real (GI, reflejos, SSAO, volumétrica, bokeh, motion blur y LOD casi sin error).
- **Los cambios de ajustes gráficos se aplican al momento**: al cambiar la calidad, activar un efecto, las sombras, el trazado de rayos o un preset, el presupuesto adaptativo devuelve lo que había bajado y vuelve a medir. Antes había que reiniciar el motor.

### MCP
- Herramientas `export_game` (Windows o Android, e instalar) y `export_status`.

### Corregido
- **Cierre del editor y del juego con error** en el driver: un pase de las sombras de las nubes se liberaba después del dispositivo.

## 0.8.6

### Corregido
- La 0.8.5 se **cerraba al abrir y al cerrar el editor** (error en `nvoglv64.dll`, el driver de NVIDIA). Al subir el máximo de imágenes de decal a 32, dos listas internas seguían siendo de 8 y se escribía fuera de ellas al arrancar. Si la 0.8.5 no te abre, descarga la 0.8.6 a mano desde la página de descargas.

## 0.8.5

### Luces
- Con **más luces que huecos** (32 puntuales y 8 focos a la vez) se dibujan las que más cuentan para la cámara: las cercanas y las de mucho alcance. Antes se quedaban las primeras de la Jerarquía y, en un mapa grande, las salas del final se veían a oscuras aunque fueran las únicas a la vista.
- Una luz **apagada** (intensidad 0) ya no ocupa hueco: se pueden tener muchas luces apagadas por la escena y encenderlas por script sin que otras desaparezcan.

### Decals
- Hasta **32 imágenes de decal distintas** en una escena (antes 8). Con más, las que no cabían salían como una mancha gris borrosa.

## 0.8.0

### Agua
- Las **olas de detras ya no se ven delante**: el agua guarda su propia profundidad (una copia de la de la escena mas las olas), asi que cada cresta tapa lo que tiene detras. Antes solo se probaba contra la escena y las olas se dibujaban en el orden de los triangulos.
- **Borreguitos**: con oleaje, las crestas mas altas y comprimidas rompen en espuma a manchas, y la espuma se queda un poco en la cara de atras de la ola.
- A lo lejos el rizado fino se convierte en un reflejo suave en vez de rayas de "metal cepillado" a ras de agua.

### Iluminacion
- **Sombras del sol con penumbra real** (PCSS, como las de Unreal y HDRP): el sol mide 0.53 grados, asi que la sombra es nitida al pie de un objeto y se suaviza con la distancia a lo que la proyecta (la copa de un arbol, un tejado, un voladizo). En las cascadas cercanas; las lejanas siguen con su filtro.
- **Cielo cubierto**: con muchas nubes la luz ambiente del cielo pasa del azul del cielo despejado a un gris neutro, como en un dia nublado.

### Nubes
- Nubes volumetricas nuevas: un **mapa de clima** reparte claros y masas por el cielo (ya no es un manto uniforme), cada zona tiene su **tipo** (estratos bajos y planos, cumulos, cumulonimbos que suben como torres), bordes mas finos cerca con una erosion que se retuerce, cimas que van por delante con el viento y formas que cambian despacio.
- **Sombras de las nubes** sobre el suelo: sus sombras recorren el terreno con el viento (componente Sky: *Sombras en el suelo* y su fuerza).
- Todo se ajusta en el componente **Sky**: cobertura, densidad, tipo, altura, grosor, velocidad y direccion del viento.
- Rayo adaptativo: pasos largos por el aire y finos dentro de la nube; el horizonte ya no se ve a trozos.

### Animacion mas realista
- **Transiciones inerciales** (como el nodo Inertialization de Unreal): al cambiar de estado el cuerpo pasa a la animacion nueva conservando el impulso de cada hueso, sin las poses que flotan del fundido cruzado. Es la mezcla por defecto; en cada transicion se puede elegir *Fundido cruzado*.
- **Continuar el ciclo** en una transicion: el estado nuevo sigue en el mismo punto del ciclo (andar -> correr con el mismo pie delante).
- Sin controlador, cambiar de clip (Lua `entity:play`) tambien es suave: *Suavizado* en el Animator.
- **Pies bloqueados** en el IK (anti-patinaje): el pie que la animacion apoya se queda clavado en el suelo hasta que la animacion lo levanta, aunque el personaje avance algo mas o menos que el clip o gire en el sitio.
- La plantilla *Tercera persona avanzada* los usa, y se inclina al arrancar, frenar y girar.

### Presets
- **Presets de componentes** (como los de Unity): clic derecho en un componente > *Presets*, o el boton *Presets...* del Post-procesado. Aplicar uno o guardar los valores actuales como preset del proyecto (`Assets/Presets/<Componente>/*.crpreset`), a uno o a varios objetos.
- El Post-procesado trae presets de fabrica: Realista, Cinematografico, Dia soleado, Atardecer, Noche de luna, Terror, Tormenta, Invierno, Retro (VHS / PSX) y Blanco y negro. En un volumen se conservan su forma, tamano, prioridad y peso.

### Corregido
- **Manchas que parpadeaban encima de los objetos** con muchos objetos en escena: con carga el presupuesto adaptativo simplifica mas las mallas que ve la camara, y los reflejos y la GI por trazado de rayos salian de esa malla simplificada, por dentro de la completa, y chocaban con el propio objeto. Los rayos salen ahora por encima de lo que se puede desviar el LOD.
- Las sombras del sol cubren tambien lo que se desvia el LOD de la camara, y la malla de la sombra ya no se simplifica mas cuando el presupuesto baja el detalle (se sombreaba a si misma y cambiaba cada vez que el presupuesto cambiaba de nivel).

### Path tracing
- Boton **Path Tracing** en la barra de la vista Escena (como el Path Tracer de Unreal): una imagen de referencia con luz fisicamente correcta con los rayos por hardware. El primer punto sale del G-buffer (normal maps, terreno, voxeles) y los caminos rebotan por la escena real muestreando el material (difuso y especular GGX con VNDF), con luz directa del sol (con su disco: penumbra real) y de las luces locales con rayos de sombra, cielo, emision, niebla y ruleta rusa.
- Suma un camino por pixel y frame mientras nada cambie y se limpia solo; mover la camara, una luz, un objeto o cambiar un material empieza de nuevo. Clic derecho: rebotes, muestras maximas y empezar de nuevo. MCP: `graphics_settings` con `path_tracing`, `path_tracing_bounces` y `path_tracing_samples`.

### Efectos de camara
- **Motion blur** de la camara y de cada objeto con los vectores de movimiento: intensidad como el obturador (0.5 = 180 grados) y tope del rastro; el fondo quieto no se arrastra sobre lo que se mueve delante.
- **Profundidad de campo** con bokeh y la formula de una lente real: distancia de enfoque o autoenfoque, apertura (numero f) y focal en milimetros.
- **Distorsion de la lente** (barril o cojin) y **destellos del sol** (fantasmas, halo y estrella) que se apagan si algo tapa el sol.
- Todo en el Post-procesado global y en los volumenes, cada seccion con su *Sobrescribir*.

### Luces
- **Fuerza de la sombra** en cada luz (Strength de Unity, 0 a 1) para el sol, las puntuales y los focos; la respetan tambien la GI y los reflejos por trazado de rayos y el path tracing.

### Materiales de los modelos
- Clic derecho en un modelo del Proyecto (o en varios seleccionados) > **Crear materiales y asignarlos**, como *Extract Materials* de Unity: un `.crmat` por cada material del modelo en `Materials/<modelo>`, con sus factores y sus texturas.
- Las texturas que trae el modelo se sacan a `Textures/<modelo>`; el metal y la rugosidad empaquetados de glTF, la oclusion, la reflectancia y la cavidad quedan en mapas sueltos.
- Las que le faltan (un FBX con las rutas rotas, un pack con las texturas aparte) se buscan en el proyecto y junto al archivo original por el nombre del material, de la malla o del modelo con los sufijos habituales (`_BaseColor`, `_Albedo`, `_Normal`, `_Roughness`, `_Metallic`, `_AO`...); las de fuera se copian al proyecto.
- Cada material va a su hueco en cada pieza: las instancias de la escena abierta y todas las que se pongan despues ya salen con sus materiales. Los `.crmat` que ya existen no se sobrescriben.

## 0.7.4

### Sombras de focos y luces puntuales
- Se acabaron los **cuadros negros** y las manchas que salian en paredes, techos y modelos cerca de un foco o una luz puntual, y que desaparecian al acercarse. De lejos la camara dibuja los modelos simplificados (LOD) y el mapa de sombra de la luz tenia el modelo completo: donde la cara simplificada quedaba unos centimetros por detras, la superficie se sombreaba a si misma. El desplazamiento de la sombra cubre ahora lo que el LOD puede desviarse (como mucho un pixel de pantalla), sin despegar la sombra.
- Las sombras de las luces locales ya no **parpadean ni cambian de forma** al mover la camara: cada objeto proyecta con un detalle que solo depende de su distancia a la luz, y mas fino que antes (medio texel del mapa de esa luz; en focos de cono estrecho eran varios texeles y dejaban acne en la propia superficie).
- **Terreno**: se dibuja en el mapa de cada luz con sus propios trozos, recortados por el volumen de la luz (tambien lo que queda fuera de pantalla), y el mapa guardado se rehace cuando cambia el detalle del terreno cerca de la luz. Antes el terreno se sombreaba a si mismo a manchas al moverse la camara.
- Una **lampara que parpadea** (intensidad 0 a ratos) conserva su sombra durante el apagado en vez de soltar su hueco y obligar a redibujar las demas. El hueco se reconoce por la luz y no por su posicion en la lista: encender o apagar otra luz ya no reordena las sombras.

### Niebla
- El color de la niebla es la luz **media** del cielo mas el halo del sol, no el cielo de esa direccion: ya no "pinta" sobre las paredes el degradado, las nubes o la luna del skybox. De noche conserva un minimo con el color ambiente.

## 0.7.2

### Plantilla Tercera persona avanzada (Mixamo)
- Plantilla nueva con el personaje y las animaciones del **Locomotion Pack** de Mixamo. Usa el tuyo (gratis en mixamo.com; su licencia no deja repartirlo con el motor): el Hub lo encuentra en **Descargas** (el `.zip` o la carpeta) o donde le digas, y lo importa al crear el proyecto.
- Cada animacion pasa a un clip `.cranim` **en el sitio** y **medido** (lo que avanza por segundo, lo que gira, cuando despega y aterriza el salto): el script mueve al personaje a la misma velocidad y los pies no patinan.
- **Animator Controller** con un **Blend Tree 2D** (parado, andar, correr, de lado andando y corriendo, y de espaldas con el clip de andar al reves), el salto y cuatro giros en el sitio.
- Todo el juego en **Lua**: escalones y rampas sin saltar, pegado al suelo al bajar, salto sincronizado con la animacion, **apuntar** (clic derecho) con desplazamiento lateral y **giros de 90 y 180 grados en el sitio**, energia al correr, **IK de pies**, **mirada** y **mano** (E en una palanca: la agarra y la baja), camara con brazo de muelle que no atraviesa paredes, palancas, compuertas, cristales y depuracion (F1).

### Escala al importar (Scale Factor)
- Los FBX de Mixamo, Maya o 3ds Max van en **centimetros** y salian **100 veces mas grandes**. Ahora, como Unity: **Convert Units** usa la unidad que dice el propio archivo y **Scale Factor** multiplica el tamano.
- Clic en un modelo del Proyecto: el Inspector muestra sus **ajustes de importacion** (Scale Factor con botones x0.01 a x100, Convert Units, la escala final, el **alto en metros**, personaje animado, normal maps de DirectX, combinar mallas) con **Aplicar** (reimporta y rehace sus instancias) y **Revertir**.
- La escala va **dentro del modelo** (malla, huesos y animaciones), no en el Transform de la raiz: mide lo mismo lo pongas como lo pongas. Los modelos importados antes tienen Convert Units apagado: si alguno salia gigante, activalo y pulsa Aplicar.

### Animacion
- **IK suave**: la mirada ya no salta de golpe de un objeto a otro (el punto mirado se desplaza y el peso sube y baja poco a poco), y los pies, la cadera, las patas de los animales y los objetivos de manos y pies se amortiguan al cambiar de altura o de objetivo.
- Un clip de un Blend Tree con **velocidad negativa** suena al reves (andar de espaldas con el clip de andar).
- Un modelo sin animaciones propias (un personaje de Mixamo "sin animacion") se anima con un Animator Controller de clips `.cranim`.

### Editor
- En Play, el juego solo recibe teclado y raton con la vista **Juego** enfocada (como Unity): en la pestana Escena, WASD y el raton son solo de la camara del editor (antes movian las dos cosas a la vez). Al pasar a la Escena se suelta el raton capturado.

### Render Textures
- Cambiar el tamano de una Render Texture ya no cierra el editor aunque se este viendo su vista previa o la del material.
- La camara no ve las superficies que muestran su propia textura (una pantalla delante de su camara salia dentro de si misma una y otra vez).
- MCP: `inspect_asset` abre en el Inspector un material, una Render Texture o los ajustes de importacion de un modelo.

### Manual
- Plantilla *Tercera persona avanzada* en *Plantillas*; *Escala al importar (Scale Factor)* en *Proyecto e importacion*.

## 0.7.1

### Render Textures
- **Render Texture** nueva (`.crrt`, *+ Añadir > Render Texture*), como en Unity: una **Camera con Target Texture** dibuja lo que ve en esa textura en vez de en la pantalla, y cualquier **material** la usa en el hueco de **Color** o de **Emision**. Pantallas, camaras de seguridad, espejos, retrovisores, minimapas o retratos. Funciona en el editor (tambien fuera de Play) y en el juego exportado, con el mismo post-procesado que la pantalla.
- Inspector de la Render Texture con su tamano (botones 128 a 2048 y 16:9) y lo que tiene en ese momento; si cambia el tamano, se rehace sola.
- Desde Lua, `entity:setField("Camera", "target_texture", uuid)`: `getField`/`setField` ahora leen y cambian referencias a assets (materiales, Target Texture...) por su UUID.

### Vistas del editor
- **Free Aspect**: la vista Escena ocupa todo el panel y se dibuja a su tamano exacto (antes tenia la proporcion de la ventana, con bandas). Menu de **proporcion** en la Escena y en el Juego como el de Unity: Free Aspect, 16:9, 16:10, 21:9, 32:9, 4:3, 5:4, 3:2, 1:1, vertical 9:16 y 9:19,5, resoluciones fijas (720p, 1080p, 1440p, 4K, 1080x1920 movil) y una **resolucion propia**. Se recuerda por proyecto.
- **Modo de dibujo** de la vista Escena: **Lit**, **Unlit** (solo el color de los materiales), **Wireframe** (las lineas de mallas, terreno, voxeles y vegetacion) y **Lit + Wireframe**.

### Manual
- Pagina nueva **Render Textures**; modos de dibujo y proporcion de la vista en *Vista Escena y gizmos*; la proporcion de la vista Juego en *Play*; Render Textures en los huecos de textura de *Materiales*. 66 paginas y 784 entradas en el buscador.

### Corregido
- Aviso de Vulkan (imagen en layout `UNDEFINED`) en el primer frame tras abrir un proyecto o cambiar el tamano de la vista.

## 0.7.0

### Multijugador
- **Red** nueva (UDP con ENet) y API **`Network`** en Lua: `Network.host(puerto)` crea la partida (el servidor tambien juega) y `Network.connect(ip)` se une. Todo pasa por el servidor, que reenvia a los demas y puede comprobar lo que llega.
- **Mensajes** con cualquier valor de Lua (numeros, textos, `Vec3`, tablas): `Network.send("chat", datos[, destino])` y `Network.on("chat", function(datos, de) end)`, fiables y en orden. Avisos al entrar y salir jugadores (`onPlayerJoined`, `onPlayerLeft`, `onConnected`, `onDisconnected`), ping y estadisticas.
- **Objetos de red**: `Network.spawn(prefab, posicion, dueno)` crea un prefab en todos; su dueno lo mueve (`entity:isMine()`) y los demas lo ven suavizado, con su Rigidbody cinematico. **Variables sincronizadas** (`setNetVar` / `getNetVar`). Quien entra tarde recibe todo como esta; si alguien se va, sus objetos desaparecen. `Network.loadScene` cambia la escena de todos. Componente **Objeto de red** para ajustar frecuencia y suavizado, y **Fisica local en los demas**: los objetos con fisica del servidor (balones, cajas) se simulan tambien en cada cliente y se corrigen con lo que llega, asi cualquier jugador los empuja al pasar y no solo el que crea la partida.
- La barra de la Escena muestra la red durante el Play (servidor o cliente y jugadores). Salir de Play o cerrar el juego cierra la partida.

### HTTPS: servidores y webs
- API **`Http`** en Lua para mandar y recibir datos de servidores y webs de forma segura: `Http.get(url, funcion)`, `Http.post(url, datos, funcion)` (una tabla se manda como JSON) y `Http.request{...}` para cualquier metodo, con cabeceras propias (tokens). La respuesta llega a la funcion con `ok`, `status`, `body`, `headers`, `error` y **`data`** (el JSON ya decodificado). Van en segundo plano: el juego no se para.
- **Seguridad**: solo `https://` con TLS 1.2/1.3 y el certificado comprobado por Windows (caducado, de otro dominio o autofirmado = rechazado); `http://` solo a `localhost` para pruebas; nunca redirige de https a http; cabeceras sin saltos de linea; tiempo maximo (20 s) y tamano maximo de la respuesta (32 MB). Al salir de Play se cancela lo pendiente.
- **`Json`**: `Json.encode(tabla)` y `Json.decode(texto)`. Tambien `Http.urlEncode`, `Http.query`, `Http.pending` y `Http.cancelAll`.

### Plantilla Online (todo en Lua)
- Nueva plantilla en el Hub: menu para **crear partida o unirse por IP**, **jugadores sincronizados** con su color, **chat** con nombres, **marcador** con puntos y ping, **monedas, un balon y cajas** con fisica del servidor (F patea), **porterias y goles**, rondas y **eventos** (lluvia de monedas, monedas dobles). El servidor decide los puntos. Scripts `Red.lua`, `JugadorRed.lua` y `MonedaRed.lua`.

### Mundo abierto y vegetacion
- Componente **Vegetacion** (*GameObject > Vegetacion (bosque)*): **millones de arboles** (pinos, robles y abedules) sembrados en segundo plano sobre el terreno, sin agua, pendientes fuertes ni **claros**. Se dibujan en la GPU: un compute shader los recorta y elige entre **3 niveles de detalle**, con sombras cercanas y viento.
- Plantilla **Mundo abierto (rendimiento)**: una **isla de 8 x 8 km** con relieve, playas y nieve, **oceano** alrededor y unos **2 millones de arboles**, con un personaje en tercera persona y un **panel de rendimiento** (FPS, ms de CPU y GPU, arboles y triangulos). Teclas para cambiar calidad, densidad, distancia, sombras y viento. En una RTX 4060 Ti: unos 400.000 arboles dibujados a mas de 100 FPS.
- **Niebla ajustable** en el post-procesado (densidad y caida con la altura), tambien en el agua. La de siempre era demasiado espesa para mundos de kilometros.
- `Graphics.get` da las cifras de la vegetacion (`foliage_trees`, `foliage_visible`, `foliage_triangles`).

### Animacion: Blend Trees
- Un estado del Animator puede ser un **Blend Tree 1D** (andar, trotar y correr segun `Velocidad`) o **2D** (moverse en 8 direcciones con dos parametros), con los pasos de los clips sincronizados. Grafico en el Inspector con el valor actual y el peso de cada clip; *Plantilla 8 direcciones*.
- **Fundido** en las transiciones: la pose pasa del estado viejo al nuevo en los segundos que elijas (antes el cambio era seco).

### Actualizaciones
- **SHA-256**: el paquete se publica con su `Cramion-win64.zip.sha256` y el actualizador no instala una descarga que no coincida.
- **Canal beta**: opcion *Estable / Beta* en el Hub y en el actualizador para recibir tambien las versiones previas (`0.7.0-beta.1`), marcadas como *Beta*.

### Lua
- **Autocompletado** completo en el editor de scripts: `Network`, `Http`, `Json`, la respuesta de Http (`res.`) y los contactos de choque (`contact.`), los metodos de red de las entidades, `OnNetVar`, las claves nuevas de `Graphics` y lo que faltaba de la 0.6.1: huesos, IK, ragdoll, sockets, `getField`/`setField`, las propiedades de interfaz (`text`, `value`, `color`, `interactable`) y los metodos de las mallas por codigo (`mesh:apply()`, `mesh.vertices`...).

### Manual
- Paginas nuevas **Network (multijugador)** y **Http y Json**; plantillas Online y Mundo abierto; Vegetacion en *Herramientas de mundo*; Blend Trees y fundido en el Animator; niebla en *Render*; SHA-256 y canal beta en *Actualizaciones*. 65 paginas y 774 entradas en el buscador. `OnNetVar` y `OnOriginShift` en *Ciclo de vida*; seccion Red en *Entity*.

## 0.6.2

### Actualizaciones
- **Actualizador** nuevo, `CramionUpdater.exe`: aplicacion aparte con su propia interfaz (Direct3D 11, funciona aunque Vulkan falle). Busca la ultima version en GitHub, ensena las novedades, descarga `Cramion-win64.zip` con progreso y velocidad, lo descomprime comprobando cada archivo (CRC) e instala sobre la carpeta del motor. Si algo falla **todo vuelve a como estaba**. Tambien **Reinstalar** (reparar) y **Omitir esta version**.
- **Se guarda todo antes de actualizar**: el editor guarda la escena (una escena nueva va a `Assets/Scenes`), los prefabs abiertos, los scripts, el material y el Animator, se cierra, y al terminar se **vuelve a abrir el mismo proyecto**. Si el actualizador se abre por su cuenta, pide lo mismo a cada editor abierto.
- El editor **avisa** cuando hay version nueva (abajo a la derecha, y *Ayuda > Buscar actualizaciones*). Se puede desactivar la busqueda al abrir.
- **Reinstalar** desde el Hub (*Actualizaciones*), desde *Ayuda > Buscar actualizaciones* o desde el actualizador: descarga otra vez la version publicada y repara los archivos que falten o esten danados, guardando todo antes igual que al actualizar.

### Hub
- Rediseno: barra lateral con iconos y secciones, tarjeta de la version (al dia / version nueva), **Actualizaciones** (instalada y publicada, novedades, actualizar, opciones) y **Aprender** (manual, Lua, shaders, Discord, GitHub). Proyectos en **tarjetas o lista** y ordenados por fecha o nombre.

### Corregido
- **Sin consola**: `CramionEditor.exe` y `cramion.exe` abrian una ventana de consola negra detras. Ahora son aplicaciones de ventana, como el player (los mensajes siguen en el panel *Consola* del editor).
- **Camara de la Escena**: al volar con el boton derecho, el raton chocaba con el borde de la vista o de la pantalla y la camara dejaba de girar hasta soltar y volver al centro. Ahora el cursor se oculta y se queda fijo mientras se vuela (el giro llega en bruto, sin tope) y al soltar vuelve donde estaba.
- **Polvo (luz volumetrica)**: con el presupuesto adaptativo activado, el polvo se apagaba cuando el frame iba justo y solo volvia al mirar al cielo o al sol (el frame se abarataba). Ahora el presupuesto solo lo abarata (la mitad de tramos por rayo y sin rayos en pantalla); nunca lo quita.
- **IK de animales**: la inclinacion del cuerpo con la pendiente iba al reves (al bajar un escalon levantaba el morro y se torcia) y ahora gira alrededor del centro entre caderas y hombros. **Mirar** con el cuello ya no puede pasar del angulo maximo: el giro total se calcula una vez y se reparte entre los huesos (antes, con el objetivo detras, el cuello se retorcia).
- Ejecutar Lua fuera de Play (consola del editor o `run_lua` del MCP) con `Scene.find` cerraba el editor.
- **Ajustes de render**: el interruptor de sombras y el desplegable de su resolucion se llamaban los dos "Sombras" (ImGui avisaba de un ID repetido). Ahora son *Activar sombras* y *Sombras* (resolucion).

### Manual
- Pagina nueva **Actualizaciones** (avisos, guardar todo antes, instalar, reinstalar y opciones); el Hub nuevo en *Interfaz y pestanas*; el polvo con el presupuesto adaptativo en *Render*. 63 paginas y 697 entradas en el buscador.

## 0.6.1

### Esqueletos: IK de animales, ragdoll y phys bones
- **Ragdoll** (componente nuevo, *Fisica*) con Jolt: una capsula por hueso unidas por articulaciones *swing-twist* con limites de doblar y girar, masa repartida y friccion en las articulaciones. Apagado sigue a la animacion; al activarlo (`entity.ragdoll = true`) **cae con la velocidad que llevaba la animacion**, su propio collider sale de la simulacion y la entidad va con el cuerpo; al apagarlo **vuelve a la animacion mezclando**. Los huesos se eligen solos para **humanos y animales** (cuerpo, columna, cuello, cabeza, patas y cola) o se generan en el Inspector para ajustarlos. Empujones con `entity:addRagdollForce(impulso, hueso)`. En la Escena se ven sus capsulas.
- **Phys Bones** (componente nuevo) como los de VRChat: pelo, coletas, colas, orejas, faldas y capas con **pull, spring, stiffness, gravedad y gravity falloff, immobile, angulo maximo, radio (y en la punta) y punta extra**, sin estirarse. **Phys Bone Collider**: esfera, capsula o plano (tambien "dentro"). *Detectar pelo, colas, orejas...* crea las cadenas por el nombre de los huesos.
- **IK para animales**: cadenas de **1 a 16 huesos** (FABRIK con *pole*): patas de 3 huesos de perros y caballos, cuellos, colas, tentaculos. **Patas al suelo** en cualquier esqueleto (escaleras, piedras, pendientes): el cuerpo baja lo que baje la pata mas baja y, con 3 o mas patas, **se inclina con la pendiente**. **Mirar** con cualquier hueso repartiendo el giro por el cuello. Objetivos como entidad o como **punto del mundo**. *Configurar automaticamente* reconoce patas (delante/detras, izquierda/derecha), cabeza, cuello y cola por la forma del esqueleto; *Crear objetivos* pone uno en cada cadena.
- **Esqueleto (huesos)** (componente nuevo): dibuja los huesos en la Escena (clic en una articulacion para resaltarla, nombres), los lista en el Inspector en arbol con buscador y permite **girar, desplazar y escalar** cualquier hueso encima de la animacion.
- **Bone Socket** (componente nuevo): una entidad **sigue a un hueso** (espada en la mano, sombrero, collider en la cabeza) o **mueve el hueso** (posar con el gizmo). *Socket aqui* en el Esqueleto lo crea.
- Los componentes de esqueleto pueden ir en la **raiz del modelo**: las piezas de un personaje (cuerpo, ropa, pelo) comparten la pose, el IK y el ragdoll. Lo enganchado a un socket no los hereda.

### Lua
- **Graphics**: toda la configuracion grafica desde un script (como `QualitySettings` + `Screen` de Unity): `Graphics.setQuality("Alta")`, escalado (`upscaler`, `resolution`, `resolution_scale`), nitidez, VSync, presupuesto adaptativo y FPS objetivo, **calidad de sombras y de texturas**, trazado de rayos, sonda de reflexion, occlusion culling, **ventana** (maximizada, pantalla completa, ventana con tamano) y resoluciones del monitor. `Graphics.options()` para montar un menu y `Graphics.save()` guarda lo que elige el jugador en el juego exportado. **`Graphics.post`** cambia el post-procesado de la escena (`Graphics.post.bloom = false`). En el editor, lo que cambien los scripts se deshace al parar el Play.
- **Cualquier campo de cualquier componente**: `entity:getField("Light", "intensity")`, `entity:setField("PhysBones", "chains[1].pull", 0.5)` (listas desde 1; `[n+1]` anade) y `entity:getFields("Ragdoll")`.
- Huesos: `getBones`, `getBonePosition`, `getBoneRotation`, `setBoneRotation`, `setBoneOffset`, `setBoneScale`, `resetBone(s)`, `showBones`. IK: `setIKTarget` (entidad, punto o nil), `setIKHint`, `setIKWeight`, `setLookAt`, `setFootGrounding`, `setupCreatureIK`. Ragdoll: `entity.ragdoll`, `addRagdollForce`, `setupRagdoll`. `setupPhysBones`. Sockets: `attachToBone`, `detachFromBone`.

### Plantilla Criaturas
- Nueva plantilla en el Hub: un **perro** que recorre escaleras, una rampa y piedras apoyando cada pata con IK, con la cola y las orejas fisicas, un sombrero enganchado a la cabeza y la cabeza siguiendo a una pelota; y un **maniqui** que apoya un pie en un escalon, mueve su coleta y cae como un ragdoll. R, F, B, I y P para probarlo todo. Los dos modelos (esqueleto, malla skinneada y animaciones) se generan por codigo.

### Manual
- Pagina nueva **Esqueletos: IK, ragdoll y phys bones** y pagina **Graphics**; referencia de los componentes nuevos; plantilla Criaturas. 62 paginas y 686 entradas en el buscador.

## 0.6.0

### Espacios de trabajo (pestañas)
- **Pestañas debajo del menú**, como los editores de assets de Unreal: **Escena** (todo el editor, como siempre) y una pestaña por cada **prefab** o **script** abierto. Clic para cambiar; la ✕ cierra.
- **Editar un prefab en su pestaña**: doble clic en el `.crprefab` del Proyecto, *Abrir (editar)* en su menú, el botón **Abrir** de la barra azul del Inspector o *Prefab → Abrir prefab* en la Jerarquía.
  - Escenario propio con el prefab solo: **Jerarquía a la izquierda, vista en el centro, componentes (Inspector) a la derecha**, Proyecto y Consola abajo. Con el cielo de la escena y una luz (que no salen en la Jerarquía).
  - Todo lo que se crea o se suelta va dentro de la raíz del prefab. Deshacer/rehacer propio de la pestaña.
  - **Guardar** (Ctrl+S o el botón de la banda azul) escribe el `.crprefab` y **todas sus instancias de la escena se actualizan** al volver, respetando sus cambios propios.
  - Cerrar con cambios pregunta *Guardar / Descartar / Cancelar*. Al salir del editor se guardan solos.
  - Play, abrir o crear escena y Exportar vuelven antes a la pestaña Escena.
- **Scripts en su pestaña**, a toda la ventana, con el **árbol de carpetas y assets a la derecha** (buscar, clic en un `.lua`/`.crshader` lo abre, doble clic en un prefab o escena lo abre, menú: nuevo script, copiar ruta, mostrar en el Proyecto).

### Plantilla MMO RPG (todo el juego en Lua)
- Nueva plantilla en el Hub: **MMO RPG**. El motor solo pone el mundo y la interfaz; **todos los sistemas son scripts de Lua** (`Heroe.lua`, `Enemigo.lua`, `Bot.lua`, `NPC.lua`) que se pueden leer y cambiar.
  - Mundo: pueblo (Villa Alba) con casas, plaza, mercado y capilla; Bosque Gris con lobos; campamento goblin con empalizada y chamanes; guarida del **Rey Goblin** (jefe con pisotón en área que avisa antes).
  - Combate: objetivo con **Tab** o clic mirando al enemigo, **Golpe**, **Bola de fuego** (con barra de lanzamiento que se interrumpe al moverse), **Curar** (nivel 2), **Torbellino** en área (nivel 3), pociones, maná, enfriamientos y enfriamiento global, críticos, ataque automático, números flotantes.
  - Progresión: experiencia y 10 niveles, estadísticas, **equipo** (arma y armadura), **inventario** de 20 huecos, **tienda** (comprar y vender), **5 misiones** (matar y recoger) con diálogos y marcas `!` sobre los personajes.
  - Enemigos con IA: patrulla, aggro, *leash* (vuelven a casa curándose), bolsas de **botín** y reaparición.
  - **Otros jugadores simulados** que cazan, mueren, reaparecen y hablan por el chat.
  - Interfaz: marcos de jugador y objetivo, barra de habilidades con enfriamientos, experiencia, chat, seguimiento de misiones, **minimapa**, inventario, personaje, diario, diálogos, muerte y ayuda (H).
  - La partida se guarda sola (Prefs); F9 la borra.

### Manual
- Manual completado: nuevo grupo **El editor** (interfaz y pestañas, vista Escena y gizmos, Jerarquía e Inspector, Proyecto e importación, Play y deshacer, herramientas de mundo, ventanas de herramientas, materiales, render y rendimiento, exportar y compilación, plantillas y atajos de teclado).
- Nuevo grupo **Componentes**: la referencia de los 46 componentes con **todas sus propiedades** (nombre, clave, tipo, valor por defecto, rango u opciones y ayuda), generada desde el propio motor con `cramion_docgen` para que no se quede atrás.
- 60 páginas y 616 entradas en el buscador.

### Corregido
- **Sombras de contacto** rehechas: recorren la profundidad **píxel a píxel** en pantalla (como las *screen space shadows* de Days Gone) y su rayo solo cubre lo que la cascada no resuelve (unos 10 texels de su mapa; *Largo máximo* es solo un tope). Antes el rayo de 0,5 m sacaba de la pantalla la sombra entera del personaje: se veía en **escalones**, duplicaba la de la cascada y **se deformaba al mover la cámara**. Sin TAA ya no usan ruido; con TAA la penumbra se suaviza con un ruido distinto cada frame. Se acabaron también los puntitos negros, el *acné* en troncos y cilindros y el negro de lejos.
- **Luz volumétrica (polvo)**: ahora también la ilumina el **cielo** desde todas direcciones (como el *Sky Light* de la niebla volumétrica de Unreal). Antes solo se veía mirando hacia el sol: de espaldas el polvo solo oscurecía.
- **Revisión de todos los shaders** (8.200 líneas):
  - Recorte alfa (`surface`, `skinned`, `voxel`): el `discard` iba antes de calcular derivadas (antialiasing especular, decals, mips); en los bordes de hojas y rejas salían valores indefinidos. Ahora se descarta al final.
  - Terreno: las capas se leían con mip automático dentro de una rama; en la mezcla entre capas salían brillos y costuras. Ahora con `textureGrad`.
  - Modelos con parallax: las texturas eligen el mip con la UV original (menos aliasing en los escalones del relieve).
  - Normales con **escala no uniforme** (cajas estiradas, esferas achatadas): se transformaban con la matriz del modelo tal cual y la luz caía mal; ahora con su inversa traspuesta (como ya hacía el trazado de rayos).
  - TAA: evitados píxeles blancos de un frame (*fireflies*) al deshacer la compresión de la historia.
  - Reflejos del agua: el ruido del trazado cambia cada frame (antes un patrón quieto). SSR: coordenada sin inicializar al salir de pantalla.

### Audio: efectos, oclusión y reverberación
- **Oclusión** en el **Audio Listener** (activar/desactivar con su casilla, `Audio.setOcclusion(on)` o `camara.audioOcclusion = false`): un sonido 3D con colliders en medio (paredes, una puerta cerrada, el techo de una casa) se oye **tapado**, más bajo y sin agudos, con fundido al abrir o cerrar. Ajustes: agudos tras una pared, volumen por pared, paredes como mucho, rapidez. No cuentan triggers, el propio sonido ni el cuerpo del jugador.
- **Efectos por Audio Source**: paso bajo, paso alto, eco (retardo, repetición, mezcla) y envío a reverberación; `entidad:setSoundEffect("lowpass", true, 800)`.
- **Audio Reverb Zone** (componente nuevo): esfera con tipo Habitación, Baño, Sala grande, Cueva, Estadio, Bosque, Bajo el agua o personalizado.
- **Paso bajo general** en el Audio Listener (bajo el agua, pausa): `Audio.setLowPass(true, 600)`.
- En Play, el Inspector del sonido dice cuántas paredes lo tapan y la Escena dibuja la línea al oyente (roja si está tapado), además del alcance y las zonas.

### Configuraciones de compilación
- *Archivo → Configuraciones de compilación…*: perfiles de exportación como los Build Profiles de Unity (`ProjectSettings/BuildConfigs.json`, viajan con el proyecto). Nueva, Duplicar, Borrar; doble clic o *Usar esta configuración* la hace activa.
  - **Nombre del juego**: el `.exe`, la carpeta exportada, el título de la ventana y la pantalla de carga.
  - **Versión** (opcional en el título de la ventana).
  - **Icono de la app**: suelta una imagen del Proyecto (PNG, JPG, TGA, BMP) o elige un `.ico`. Se escribe **dentro del .exe** (16 a 256 px): Explorador, barra de tareas y ventana.
  - **Escena inicial**, **ventana** (maximizada, pantalla completa sin bordes o ventana con tamaño), **static batching** y **Mostrar FPS** de desarrollo.
- La ventana *Exportar juego* elige la configuración y muestra el `.exe` que va a salir.

### Pintar prefabs
- **Herramienta de pintado de prefabs** (botón **B Pintar** en la vista Escena, tecla **B**, o *Ventana → Pintar prefabs*), como el Foliage de Unreal pero con prefabs de verdad: árboles con su script, rocas con su collider, cofres…
  - **Grupos de pintado** (`.crpaint` en `Assets/PaintGroups`): arrastra prefabs del Proyecto a la ventana. Cada uno con su **peso** (cuántos salen frente a los demás), **escala aleatoria**, **alinear al suelo** (0 = vertical como un árbol, 1 = sigue la pendiente como una roca), **giro aleatorio** y **hundir** en el suelo.
  - Se pinta **todo el grupo** o **solo el prefab elegido**.
  - **Pincel**: radio (Ctrl + rueda o `[` `]`), **densidad** por 100 m² (repasar no amontona: rellena hasta esa densidad), **separación mínima** entre objetos y **pendiente máxima**. **Mayús + arrastrar borra.**
  - Apoya en los colliders (también Mesh Collider) y en los terrenos; lo ya pintado no cuenta como suelo.
  - Cada trazo es un paso de deshacer. Lo pintado queda bajo *Pintado - &lt;grupo&gt;* en la Jerarquía, como instancias normales de su prefab. *Seleccionarlos* y *Borrar todos* en la ventana.
- MCP: herramienta `paint_prefabs`.

### Volúmenes de post-procesado
- El componente **Post-procesado** es ahora un **Volume como el de Unity**: forma **Global**, **Caja** o **Esfera** (con la posición, el giro y la escala de la entidad).
  - Con la cámara dentro de una caja o esfera se usa ese volumen; fuera, el global. La **distancia de mezcla** hace la transición suave al acercarse; **peso** y **prioridad** deciden cómo se combinan varios.
  - Un volumen local **solo cambia las secciones que marca como Sobrescribir** (exposición, bloom, color, viñeta, lente, efectos…); el resto sale del global.
  - *GameObject → Volumen de post-procesado → Global / Caja / Esfera*. Uno nuevo empieza con el aspecto del global actual.
  - En la Escena se ven la forma y, más tenue, la zona de transición.
- Las escenas antiguas siguen igual: su post-procesado es global.

### Agua
- **Olas interactivas** (como en Red Dead Redemption): **todo lo que tiene collider** (Box, Sphere, Capsule, Mesh, Wheel), Rigidbody o es un agente de navegación y cruza la superficie empuja el agua, se mueva por física, por script, por animación o con el gizmo del editor. Lo que está **quieto** y atraviesa el agua (postes, rocas, pilares; hasta 10 m) es un **obstáculo: las ondas chocan y rebotan**. Al caer, **salpicadura con espuma** según la velocidad; al moverse, **estela**. Las ondas se propagan, se cruzan, rebotan y se apagan solas. Simulación de la ecuación de onda en 48 × 48 m alrededor de la cámara (celdas de 25 cm, 60 Hz), en el océano, los lagos y los ríos.
- **Ríos**: la corriente sigue el camino de los puntos. Antes el rizado y la espuma se movían en el mundo con la dirección de cada sitio y se deformaban cada vez más (y más cuantos más puntos). La curva es ahora un Catmull-Rom centrípeto: sin bucles ni panzas con puntos a distancias desiguales.
- **Cáusticas bajo el agua**: con la cámara dentro del agua, el fondo y los objetos sumergidos reciben las cáusticas del sol (con su sombra). Antes solo se veían desde fuera.

### Animación
- **Cinemática inversa (IK)**: componente **IK** (*Animación*) sobre la pose animada de cualquier modelo con esqueleto.
  - **Manos y pies** a una entidad objetivo (IK de dos huesos analítico, exacto), con **codo/rodilla hacia** otra entidad (pole), peso y *copiar giro*.
  - **Mirar**: la cabeza (y un poco el cuello) sigue a una entidad, con ángulo máximo.
  - **Pies en el suelo**: cada pie se apoya en lo que tiene debajo (escaleras, pendientes, rocas), la cadera baja para que la pierna llegue y el pie se inclina con el suelo. No cuenta el propio personaje.
  - **Cadenas** de dos huesos por nombre para esqueletos no humanos (colas, brazos robóticos).
  - Los objetivos son entidades normales: se mueven a mano, por script o con física.
- **Animación procedural**: componente **Animación procedural** (*Animación*), encima de la animación o sin ella.
  - **Huesos con muelle** (pelo, colas, capas, antenas, pendientes): cada cadena se mueve con inercia, gravedad, rigidez y amortiguación, sin estirarse, y **choca con el cuerpo** (cabeza, pecho, cadera). Al mover el personaje se quedan atrás y rebotan.
  - **Patas procedurales** (arañas, cangrejos, robots, dragones): cada pie se queda clavado en el suelo y **da un paso en arco** cuando el cuerpo se aleja; los grupos se turnan (diagonales, trípode), el pie se adelanta según la velocidad y el cuerpo sube, baja y se inclina con el suelo que pisan.
  - **Capas**: **respirar** (el pecho sube y baja), **inclinarse** al acelerar, frenar o girar, y **ruido** suave en huesos sueltos (antenas, colas en reposo).
  - Orden por frame: animación → capas y patas → IK → huesos con muelle.
- **Humanoides y retargeting** (como el Avatar de Unity): los esqueletos humanos se reconocen solos por los nombres de sus huesos (Mixamo, Unreal, Blender/Rigify, 3ds Max Biped y nombres sueltos). Un clip `.cranim` de un humanoide **se usa en otro humanoide** aunque sus huesos se llamen distinto, tengan otros ejes, otras proporciones, otra escala (cm/m), otro "arriba" (Z o Y) o esté en pose A en vez de T: se convierte al cargarlo, hueso a hueso en el espacio del personaje, y la cadera se desplaza escalada por la altura. Los `.cranim` guardan ahora su esqueleto (vuelve a extraer los antiguos para convertirlos).
- **Navegador**: los modelos y clips humanoides llevan una **insignia** (círculo con el icono de SkinnedMesh) abajo a la derecha de su miniatura.

### Editor
- **Abrir un proyecto no congela la ventana**: el Hub se cierra y sale un diálogo con el logo del motor y una **barra de progreso** (como Unity): abrir el proyecto, leer la escena, cargar los modelos **en otro hilo** (con el nombre de cada uno), subirlos a la GPU y preparar la física.
- **Add Component rediseñado**: ventana grande centrada en la pantalla, buscador con el **filtro de categorías en un desplegable** a su derecha y los componentes en una **rejilla de fichas** con icono, nombre y el color de su categoría (como Unreal). Enter añade el primero.

## 0.5.1

### Gizmos
- **El gizmo de mover, rotar y escalar se dibuja siempre encima y opaco**, como en Unreal. Antes se probaba contra la profundidad de la escena y, dentro del objeto seleccionado (casi siempre), se veía al 22 % y desteñido.
- **Más grueso y más grande**: líneas de 5 px, flechas y cubos más gordos, un 30 % más grande en pantalla. Color plano, sin iluminación.
- **Rotar**: sin el círculo blanco de la vista, que estorbaba. Nueva **bola central de giro libre**: al arrastrarla el objeto gira alrededor de su centro siguiendo el ratón (horizontal = eje arriba de la vista, vertical = eje derecho), como en Blender o Maya. Un paso de deshacer por arrastre.
- **Mesh Collider completo**: antes solo se dibujaban sus primeros 12.000 triángulos y faltaba un trozo. Ahora, si hay demasiados, se reparten por toda la malla. Los triángulos se guardan por objeto en vez de pedirlos a Jolt en cada frame.

### Materiales
- **Nuevos mapas en el `.crmat`**, los de los packs de escaneos (Megascans, Poly Haven…):
  - **Altura / Displacement** con *parallax occlusion mapping*: piedras y grietas con profundidad real al mirarlas de lado. *Relieve → Profundidad (m)*, en metros (0,03 = 3 cm), igual para un suelo que se repite que para un escaneo con toda la malla en una sola UV. Desplazamiento limitado en ángulos rasantes (sin estirones) y se apaga solo a más de 30 m.
  - **Cavidad**: las grietas pierden reflejo y luz ambiental (con *Fuerza cavidad*).
  - **Specular**: reflectancia por píxel (0,5 = la del material), como el Specular de Unreal.
  - **Gloss**: brillo = 1 − rugosidad, si no hay mapa de rugosidad.
  - **Bump**: relieve fino convertido en normal map, si no hay normal map.
- **Crear material desde una imagen** encuentra solas sus compañeras por el sufijo: `_Normal`, `_Roughness`, `_AO`, `_Displacement`, `_Cavity`, `_Specular`, `_Gloss`, `_Bump`… Un pack de Megascans queda completo arrastrando su BaseColor.
- Los mapas nuevos se empaquetan en las texturas que ya existían (sin más memoria de descriptores ni pasadas).

### MCP
- Nueva herramienta `set_gizmo` (mover, rotar, escalar o ninguno; ejes locales o del mundo).
- `create_material` acepta `from_image` (busca las compañeras de un pack) y los mapas nuevos: `height_texture` y `height_scale`, `roughness_texture`, `occlusion_texture`, `cavity_texture`, `specular_texture` y `gloss_texture`.

## 0.5.0

### Rendimiento
- **Presupuesto adaptativo** (nuevo sistema del motor, activado por defecto). Objetivo: que el juego llegue a los FPS pedidos en cualquier PC, también de gama baja.
  - **Perfil de hardware** al arrancar (VRAM y GPU integrada o dedicada): Bajo, Medio, Alto o Ultra. De él dependen la resolución del mapa de sombras y desde dónde empieza el gobernador.
  - **Mapa de sombras según el hardware**: 1536 / 2048 / 4096 / 6144 por cascada. Antes siempre era 6144, **576 MB de VRAM**; en un PC de gama baja ahora ocupa 36 MB. Se puede fijar a mano en *Gráficos → Sombras*.
  - **Gobernador de frame con costes aprendidos**: cada frame lee el tiempo de GPU de cada pasada. Si no llega al objetivo, baja un paso la palanca que más milisegundos ahorra por cada punto de calidad perdido. Las palancas son: detalle de mallas, detalle de sombras, luz volumétrica, sombras de contacto, SSAO, reflejos, GI y resolución interna con FSR 1. Tras cada cambio mide lo que ahorró de verdad en ese PC y en esa escena, y lo recuerda. Con eso sube la calidad cuando sobra tiempo, sin pasarse y sin oscilar. Nunca activa algo que el usuario apagó.
  - **Sombras por texel**: cada cascada elige el LOD que su resolución puede mostrar y no dibuja los objetos más pequeños que su texel. Ya no depende de la cámara, así que las cascadas guardadas dejan de rehacerse al moverse. Con el detalle de sombras bajado, las cascadas lejanas se actualizan con menos frecuencia.
  - **Objetos de menos de un píxel**: la cámara no los dibuja, pero su sombra sigue. Los LODs se eligen con la resolución interna: si baja la resolución, las mallas también pueden ser más simples.
  - *Gráficos → Presupuesto adaptativo*: activarlo, FPS objetivo (30 a 240) y estado en vivo de cada palanca. Se guarda en `Graphics.ini` y el juego exportado lo usa igual.
  - MCP: herramienta `graphics_settings`; `performance_stats` incluye el estado del presupuesto.
  - Prueba con una GPU simulada 3 veces más lenta: pasa de 21 a 60 FPS en 3,5 s y se queda estable.
- **LODs automáticos** (meshoptimizer). Al cargar cada modelo estático de más de 4096 triángulos se generan hasta 5 niveles simplificados, cada uno con la mitad de triángulos que el anterior. En follaje también se quitan las briznas y hojas que ya no se ven. En cada frame, cada objeto se dibuja con el nivel más simple cuyo error en pantalla no pasa de **1 píxel** (el criterio de Nanite): no hay distancias que ajustar. **Las sombras usan el mismo nivel.** Se ajustan en *Post-procesado → Rendimiento* (*LODs automáticos* y *Error máximo (px)*). El `.crdata` no cambia: los LODs viven solo en memoria. *Estadísticas* y la herramienta MCP `performance_stats` muestran los triángulos con LOD y cuántos objetos van simplificados. Ejemplo real: una mata de hierba de 185.850 triángulos baja a 5.807 a lo lejos, y un acantilado escaneado de 2 millones, a 62.000.
- **Static batching al exportar.** Nueva casilla **Static** en el Inspector (junto al nombre; pregunta si aplicarla a los hijos). Al exportar con *Combinar mallas estáticas*, las mallas Static de cada escena se hornean en un lote: materiales iguales por contenido (factores y bytes de las texturas) en una sola llamada, clusteres espaciales para que el culling en GPU siga descartando lo que no se ve, una pieza por modo de sombra, `.crmat` conservados. No se combinan las mallas repetidas y grandes (ya van instanciadas), ni lo que tiene Animator, Script o un Rigidbody no estático. El proyecto no cambia: solo la copia que va al juego.
- **Batching de sombras y del pase de cámara**: los tramos visibles seguidos del buffer de índices se dibujan en una llamada (sombras opacas: sin importar el material; recortadas y cámara: por material). Nuevo contador *llamadas de sombras* en Estadísticas.
- **Clusteres independientes de la escala**: rejilla relativa a la caja de cada malla y un mínimo de 1024 triángulos por cluster. Antes, un FBX en centímetros acababa en celdas de 5 cm (miles de submallas). Los `.crdata` antiguos se reagrupan al cargarlos; reimportar los guarda así.
- **Agua**: las ondas más finas que un píxel no calculan senos ni cosenos (solo cuentan como rugosidad).

### Iluminación
- **Caché de radiancia en el mundo** (como SHaRC de NVIDIA o la *surface cache* de Lumen). Es una tabla hash de celdas en el mundo, de 25 cm cerca de la cámara y más grandes lejos, con una entrada por cada orientación de la superficie. Guarda la luz que llega a cada superficie: el cielo que ve de verdad más la luz rebotada.
  - La llenan los píxeles de la GI y rayos secundarios desde los impactos, que cubren también lo que está fuera de pantalla.
  - Cuando un rayo de GI o de reflejo choca, lee esa celda. Antes suponía que cualquier punto veía la mitad del cielo.
  - **Rebotes infinitos**: cada frame encadena un rebote más sobre la luz guardada. Las esquinas y los interiores reciben la luz que les corresponde.
  - **Sin fugas de cielo** en cuevas y habitaciones.
  - **Más estable**: la media vive en el mundo, no en los píxeles.
  - Se vacía sola al desplazarse el origen flotante o al cambiar de escena. Solo con trazado de rayos.
- **Sombras de contacto** del sol en pantalla (12 pasos, a menos de 60 m, solo donde la cascada no ha oscurecido ya). *Post-procesado → Efectos → Sombras de contacto* y su largo.
- **Compensación de energía por dispersión múltiple** (Kulla-Conty / Fdez-Agüera) en la luz directa y en el IBL.
- **Sol con tamaño angular** (0,53°) en el especular: sin puntos blancos que parpadean en superficies pulidas.
- **Oclusión del horizonte** en los reflejos con normal maps.

### Agua
- **Oleaje JONSWAP**: 24 ondas de Gerstner muestreadas del espectro del mar real (direcciones repartidas alrededor del viento, fases pseudoaleatorias). *Altura de ola* = altura significativa. Misma tabla en el shader y en la flotación.
- Olas, texturas del terreno y nubes **relativas a su propio origen**: no saltan con el origen flotante.
- **Reflejo del cielo real** (con las nubes volumétricas) cuando esa parte del cielo está en pantalla; sin la franja marrón en el horizonte.
- **Dispersión subsuperficial** (modelo de Atlas, GDC 2019) en las crestas a contraluz.
- **Espuma** de vetas y burbujas de ruido deformado (sin celdas poligonales) y solo donde rompen las olas grandes.
- Brillo del sol con GGX completo (Smith, Fresnel en el ángulo medio) y rizado fino repartido alrededor del viento.
- El agua recibe la **niebla por altura** y la **luz volumétrica** (hasta su superficie), como el resto de la escena.

### Mundos grandes
- **Origen flotante**: a más de 2 km de la cámara el mundo se desplaza en pasos de 1024 m. Se desplazan entidades, cuerpos de Jolt (con su velocidad), partículas, audio (sin saltos de Doppler), cinemáticas, secciones de bloques, sonda de reflexión y caches del renderizador; la navegación convierte en sus consultas sin rehacer la malla; los bloques guardan coordenadas absolutas. La escena guarda `origin` y posiciones relativas (sin perder precisión); deshacer, Play y abrir escenas vuelven a alinear. El Inspector muestra la *Posición real*.
- Lua: `Scene.origin()`, `Scene.toAbsolute(pos)`, `Scene.toLocal(x, y, z)` y `function Script:OnOriginShift(offset)`.

### Editor
- **Componente Profiler** (Add Component → Depuración): FPS medio y mínimo, CPU (ms y %), GPU (ms por timestamps y %), nombre de la GPU, RAM y gráfica; esquina, tamaño y opacidad configurables.
- **Importación con progreso**: ventana con el archivo, la etapa y el porcentaje real (assimp informa de su avance); cola de importación de 2 en 2.
- **Reimportar (combinar mallas)** en el clic derecho de un modelo: junta por material las piezas de modelos muy troceados (una palmera pasa de ~70 objetos a 2), rehace sus instancias en la escena conservando transformaciones, materiales y Static, y guarda una copia del original en `Library/ReimportBackups`. Los modelos nuevos con más de 16 piezas se combinan solos al importarlos.
- **Miniatura del proyecto en el Hub**: se captura la vista Escena en `Library/thumbnail.png` al abrir el proyecto y al guardar la escena.
- **Botón Gizmos** (tecla G) en la barra de la vista Escena: oculta los iconos y ayudas (luces, cámaras, decals, física, cinemáticas, agua y asas de colliders). El gizmo de mover, rotar y escalar se queda, y la navegación sigue con su botón Nav.
- MCP: herramientas `performance_stats` y `reimport_model`.
- Arrastrar un material con varios objetos seleccionados: el Inspector no cambia (se abre al soltar sin arrastrar) y el material se aplica a todos.

### Juego exportado
- **Carga de escenas por etapas** con pantalla de carga: leer la escena, modelos en otro hilo, subida a la GPU, física/navegación/scripts. También con `Scene.load()`.
- **Mensaje claro** si la GPU se queda sin memoria de vídeo (en vez del código de Vulkan).
- **Cámara orbital por defecto** si la escena no tiene ninguna Camera.
- El reagrupado de modelos antiguos y el static batching evitan escenas que antes agotaban la VRAM.

### Notas
- El agua sigue siendo Gerstner (24 ondas), no un océano FFT.
- Los scripts que guardan posiciones del mundo en variables deben usar `OnOriginShift` (o `Scene.toAbsolute` al guardar partidas).
- Un objeto marcado Static no debe moverse, ocultarse ni cambiar de material por script en el juego exportado: su malla va dentro del lote.

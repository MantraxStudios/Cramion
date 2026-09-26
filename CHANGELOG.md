# Cambios

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

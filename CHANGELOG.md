# Cambios

## 0.5.0

### Rendimiento
- **LODs automáticos** (meshoptimizer). Al cargar cada modelo estático de más de 4096 triángulos se generan hasta 5 niveles simplificados, cada uno con la mitad de triángulos que el anterior. En follaje también se quitan las briznas y hojas que ya no se ven. En cada frame, cada objeto se dibuja con el nivel más simple cuyo error en pantalla no pasa de **1 píxel** (el criterio de Nanite): no hay distancias que ajustar. **Las sombras usan el mismo nivel.** Se ajustan en *Post-procesado → Rendimiento* (*LODs automáticos* y *Error máximo (px)*). El `.crdata` no cambia: los LODs viven solo en memoria. *Estadísticas* y la herramienta MCP `performance_stats` muestran los triángulos con LOD y cuántos objetos van simplificados. Ejemplo real: una mata de hierba de 185.850 triángulos baja a 5.807 a lo lejos, y un acantilado escaneado de 2 millones, a 62.000.
- **Static batching al exportar.** Nueva casilla **Static** en el Inspector (junto al nombre; pregunta si aplicarla a los hijos). Al exportar con *Combinar mallas estáticas*, las mallas Static de cada escena se hornean en un lote: materiales iguales por contenido (factores y bytes de las texturas) en una sola llamada, clusteres espaciales para que el culling en GPU siga descartando lo que no se ve, una pieza por modo de sombra, `.crmat` conservados. No se combinan las mallas repetidas y grandes (ya van instanciadas), ni lo que tiene Animator, Script o un Rigidbody no estático. El proyecto no cambia: solo la copia que va al juego.
- **Batching de sombras y del pase de cámara**: los tramos visibles seguidos del buffer de índices se dibujan en una llamada (sombras opacas: sin importar el material; recortadas y cámara: por material). Nuevo contador *llamadas de sombras* en Estadísticas.
- **Clusteres independientes de la escala**: rejilla relativa a la caja de cada malla y un mínimo de 1024 triángulos por cluster. Antes, un FBX en centímetros acababa en celdas de 5 cm (miles de submallas). Los `.crdata` antiguos se reagrupan al cargarlos; reimportar los guarda así.
- **Agua**: las ondas más finas que un píxel no calculan senos ni cosenos (solo cuentan como rugosidad).

### Iluminación
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

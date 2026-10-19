# Cramion 2.5.1

**Lightmaps UV2, memoria de la GPU con VMA y mundos grandes mucho más rápidos.** Es la versión más grande desde la 2.4: el render se reorganiza para no repetir trabajo por frame, llegan los lightmaps por texel y entran 39 parches de rendimiento de la comunidad (gracias, Keryan).

## Descarga

- `Cramion-win64.zip`: descomprime y abre `CramionEditor.exe`. Si ya tienes Cramion, el actualizador te avisa solo.
- SHA-256: `9324fa6e47f46e8660a7d54cb48242e50b3d1b2a8097cbe1c581fb980732c2a3`

## Lo nuevo

### Nueva imagen
- **Nuevo logo de Cramion** en el editor, los ejecutables, la web y el manual, y nuevo banner «Game powered by Cramion» al arrancar los juegos exportados.

### Lightmaps UV2
- UV de lightmap automáticas con **xatlas** (*Generar UV de lightmap*, vista **LM** de la Escena y la herramienta MCP `generate_lightmap_uvs`).
- **Lightmapper propio**: luz directa, rebote con su filtro y luces Horneadas.
- `.crbake` v4 con los lightmaps, atlas en el render y lote estático al exportar.

### Memoria y subidas
- **VMA 3.3.0**: los buffers y las texturas ya no hacen una asignación de Vulkan cada uno. Anillo de staging persistente de 64 MB (16 MB en modo compatible).
- **Un lote por modelo**: un command buffer y una fence, por una cola de transferencia dedicada si la GPU la tiene. El modelo aparece cuando termina, sin bloquear el frame.
- Los modelos se cargan en su propio grupo de hilos; fuera `std::async`.

### Mundos grandes y sombras
- **Identidad estable de los actores**: un objeto que entra o sale ya no hace redibujar las 4 cascadas.
- Caché por entidad del modelo resuelto, sin `Animator` para lo que no se anima.
- **Sombras**: se descarta lo que no puede proyectar en la vista (barrido hacia el sol), recorte por clúster en las cascadas, cada cascada solo con sus trozos de terreno, y el terreno solo las redibuja si cambia.

### Trazado de rayos y tiling
- Las variantes `.crmat` comparten triángulos y **BLAS**; cada textura entra una vez.
- **Tiling por material** en un uniform, sin reescribir ni duplicar vértices.

### Gama baja
- GI horneada sin filtro SVGF, sol horneado sin cascadas si nadie las lee, ríos, ondas y flotación más baratos.
- Distancia de sombras y LOD por gama (60 / 80 / 100 m) con el presupuesto adaptativo.
- Nueva opción `lighting_quality` (auto, full, lite).

### Render
- Vidrio recortado con la **pirámide Hi-Z** y **luces puntuales por teselas** de pantalla.
- **Early-Z**: los materiales opacos sin alfa van por una pipeline sin `discard`.
- **LOD para personajes con esqueleto**, con atributos y tope de error de 0,5 px.
- Sin trabajo apagado: sin bloom ni auto-exposición si están apagados, océano sin FFT si no se ve, FSR 1 con cadena corta y sin FXAA encima de TAA, FSR 3 o DLSS.
- Texturas **BC7** para la UI, los árboles y las capas del terreno, con caché en disco.

### UI, física y ventana
- **El clic sobre la UI ya no llega al juego** (`UI.pointerOverUi`).
- `UIEventTrigger`, `Entity:uiScreenRect`, `UIInputField` con cursor, selección y portapapeles, y `UIImage` con `uv_rect` y 9-slice.
- Física para scripts: `layerMask`, `sphereCast` y `overlapSphere`. Los MeshCollider grandes se construyen en segundo plano.
- Ventana de tamaño exacto, modo **sin bordes** y **tope de FPS** (`fps_limit`); el presupuesto adaptativo respeta el tope.
- El cursor capturado vuelve a su sitio al soltarlo, y el editor en segundo plano deja de dibujar la escena.

### World Partition, red y Blueprints
- **World Partition**: varias rejillas, Data Layers (también desde scripts) y dependencias entre celdas.
- **Red a más de 2 km**: las posiciones viajan absolutas (sector de 1024 m + resto).
- **Blueprints fase 2**: Construction Script, funciones, macros, Timeline y depuración en vivo.

### Vídeo y reproductor
- **Vídeo en Android** con MediaCodec (archivos y `https://`). FFmpeg opcional para RTSP, RTMP y DASH.
- `<Juego>.exe --run-tests [--junit x.xml]` ejecuta las pruebas `CRAMION_TEST` del juego exportado.

## Cambios a tener en cuenta
- **Red**: el protocolo pasa a la versión 2. Los juegos y el `CramionServer` anteriores no se conectan con los nuevos.
- **UI**: los scripts ya no reciben el clic que cae sobre la interfaz.
- **Sin vsync**, en GPUs sin Mailbox el juego va sin tope y con tearing salvo que pongas `fps_limit`.
- **Gama Baja y Media**: las sombras acaban antes (60 y 80 m).
- **Formatos**: `.crgraph` v2 si usa algo nuevo, celdas de World Partition v3 y `.crbake` v4. Los antiguos se siguen leyendo.

El detalle completo está en `CHANGELOG.md`.

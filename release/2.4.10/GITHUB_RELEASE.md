# Cramion 2.4.10

**Corrección sobre la 2.4.9: cambio de escena sin terrenos desplazados ni restos de la escena anterior, el preset «Ultra realista» sin aspecto lavado, VR nítida y con la luz correcta, exposición automática que funciona de noche y horneado de luz que vuelve a funcionar, ahora con luz directa y sombras.**

Es una versión de corrección: solo trae estos arreglos sobre la 2.4.9. Se llama 2.4.10 y no 2.4.9.1 porque el actualizador compara tres números; así se ofrece sola a quien ya tiene la 2.4.9.

## A quién afecta

- **Cambio de escena**: a cualquier proyecto con terreno por teselas (`TerrainGrid`) que cambie de escena: abrir otra escena en el editor, `Scene.load` en Play, pulsar Stop, cambiar de pestaña de mundo, y los cambios de nivel en el juego exportado y en el servidor dedicado. Se nota sobre todo después de que el origen flotante se haya movido (alejarse más de 2 km en Play) o cuando las dos escenas guardan orígenes distintos.
- **«Ultra realista»**: a las escenas que usan los presets de post-procesado «Ultra realista» o «Realista».
- **Realidad virtual**: a todos los proyectos que se juegan con casco (OpenXR).
- **Exposición automática**: a las escenas con exposición automática, sobre todo de noche y con luces a la vista.
- **Horneado de luz**: a quien use *Iluminación > Hornear* (o `bake_lighting` en el MCP), sobre todo si salía «La escena no tiene mallas estáticas».

## Cambio de escena

**Qué pasaba**
- **Terreno desplazado**: las teselas del terreno de la escena anterior sobrevivían a la carga, y el origen del mundo cambiaba sin moverlas. Quedaban desplazadas justo lo que se había movido el origen flotante (por ejemplo 3072 m tras jugar lejos y pulsar Stop) hasta que el streamer las recolocaba, y el terreno daba un salto.
- **Restos de la escena anterior**: ningún cambio de escena vaciaba el streamer de terreno. Las teselas viejas entraban en la GPU, la física y la malla de navegación; en el juego y el servidor dedicado seguían ahí cuando se creaban la física y la navegación de la escena nueva. Además, lo que se cargaba en otros hilos para la escena anterior (teselas, vegetación, baldosas de navegación, celdas del World Partition) podía entrar en la nueva.

**Qué se ha corregido**
- Todos los cambios de escena pasan por **una única secuencia** (`resetSceneState`) que vacía teselas, almacén de terrenos, física, navegación, World Partition y los terrenos y la vegetación de la GPU, y deja el mundo vacío con el origen a 0. La usan el editor (abrir, nueva escena, `Scene.load` en Play, Stop, pestañas de mundo), el juego y el servidor dedicado.
- Cada escena tiene una **generación**: los trabajos en otros hilos tiran su resultado si la escena ya cambió, el streamer, el render, la navegación y el World Partition se vacían solos si detectan una generación nueva, y la física descarta los colliders de terreno pendientes de otra escena.
- Las teselas que se quedan a propósito (Stop, deshacer) se **recolocan** con el cambio de origen, en doble precisión.
- Una rejilla con el mismo UUID pero otra carpeta o tamaño (por ejemplo tras *Guardar como*) ya no deja las teselas viejas estiradas en la escena nueva.
- La navegación ya no deja de construirse tras varios cambios de escena a mitad de construcción.
- Las nubes ya no quedan desplazadas tras cargar una escena con otro origen.
- Una rejilla nueva ya no reutiliza la carpeta de otra rejilla de la escena.

## «Ultra realista» sin velo

**Qué pasaba**: el preset usaba AgX base (neutro y plano, pensado para gradar encima) sin compensación de exposición, con una auto-exposición pensada para ACES y un polvo volumétrico denso. Resultado: cielo gris, sombras grises y verdes apagados.

**Qué se ha corregido**
- **Nuevo tonemapper *AgX Punchy***: AgX con un look Punchy suave y la exposición igualada a ACES. Según una simulación en Python de las curvas del shader (no medido en el motor), en sRGB de 8 bits:

  | Entrada (lineal) | AgX (antes) | AgX Punchy (ahora) | ACES |
  |---|---|---|---|
  | Gris 0,18 | 128 | 101 | 91 |
  | Gris 0,02 | 37 | 17 | 11 |
  | Hierba (0,08; 0,16; 0,03) | (95, 118, 59) | (63, 93, 18) | (49, 83, 22) |

- **Aire limpio**: en «Ultra realista», el polvo volumétrico baja de 0,006 a 0,0008 /m (visibilidad estimada de unos 0,65 km a unos 4,9 km) y la niebla de 0,0008 a 0,0002 (velo estimado a 300 m de un 21 % a un 4 %). Los rayos de sol entre los árboles se siguen viendo.
- **«Realista»** también usa *AgX Punchy*, con aire más limpio.
- **Grano**: ya no levanta los negros (antes, con grano 0,03, hasta unos +10/255 en el negro). Es un cambio visible en las escenas con grano.
- **Nuevo ajuste *Nitidez*** (`sharpen`, 0 a 1): enfoque adaptativo al contraste tras el antialiasing. Vale 0 por defecto.
- *AgX* (el de antes) se sigue dibujando exactamente igual, así que **las escenas guardadas con AgX no cambian**.

## Realidad virtual nítida

**Qué pasaba**: el casco encendía siempre el presupuesto adaptativo, que podía bajar cada ojo al 50 % (y en gama baja cada sesión empezaba al 67 %); los presets de calidad de escritorio también encogían los ojos; los ojos no tenían antialiasing; y la luz se veía rara: la sonda de reflexión no se volvía a capturar con el casco (reflejos de día por la noche), los efectos de pantalla no casaban entre los dos ojos y la GI dinámica se actualizaba dos veces por frame.

**Qué se ha corregido**
- La VR sigue tu ajuste *adaptativo* y, con el casco activo, el presupuesto **nunca baja un ojo de 0,87**: el presupuesto baja antes la GI, la oclusión, los reflejos, la luz volumétrica, las sombras y los LOD.
- Nuevo ajuste **«Resolución VR»** (`vr_render_scale`, 1 por defecto) en *Gráficos*, `Graphics.ini` y el MCP, que sustituye al preset de escritorio mientras el casco está activo.
- Cada ojo usa **TAA con enfoque RCAS** a resolución nativa (FSR 1 solo si lo eliges), y las texturas reciben **sesgo de mips** al dibujar por debajo de la resolución de salida.
- El tope de VRAM crece con la memoria: en un ojo de Quest 3, 0,87 con 8 GB y sin tope desde 12 GB.
- La **sonda de reflexión** se vuelve a capturar en VR, desde la cabeza.
- **Destello de lente, viñeta y rayos de sol en pantalla** se apagan con el casco (la luz volumétrica sigue).
- La **GI dinámica** se actualiza una vez por frame, igual para los dos ojos.

## Exposición automática de noche

**Qué pasaba**: de noche la exposición se quedaba clavada en `max_ev`, y una luz no cambiaba nada hasta ocupar un tercio de la pantalla; luego la imagen caía de golpe.

**Qué se ha corregido**
- De noche ya **no se clava en el máximo**: por encima de +0,75 EV solo compensa una parte, así que la noche sigue pareciendo noche.
- Las luces **oscurecen poco a poco** según crecen en pantalla, y una bombilla a la vista baja la exposición sin pasar del nivel de día.
- **Más estable**: zona muerta pequeña, tope de 4 EV por segundo (un cambio grande tarda alrededor de 1,5 s) y protección contra NaN.
- Los **cortes de cámara** saltan directo a la exposición nueva; en **VR** se miden los dos ojos y comparten exposición.
- En el editor, con Escena y Juego visibles, la vista oculta ya no frena la adaptación.
- Nuevos valores por defecto: `min_ev`/`max_ev` −4/+2 («Ultra realista»: `min_ev` −5).

## Horneado de luz

**Qué pasaba**: «Nada que hornear: la escena no tiene mallas estáticas» salía aunque hubiera objetos Static. Se descartaban los modelos con alguna animación (casi todos los FBX y GLB), Static en el padre no contaba para los hijos, un solo objeto Static (aunque estuviera desactivado) dejaba fuera todo lo demás, y el terreno necesitaba su collider. Además el horneado solo guardaba luz rebotada: nada de luz directa ni sombras.

**Qué se ha corregido**
- **Static se hereda** del padre, los modelos animados se hornean en su pose de reposo, la regla de «solo Static» cuenta solo lo que se puede hornear (sin nada marcado se hornea todo lo que no se mueve) y el **terreno** se lee de su mapa de alturas, sin collider.
- Cada horneado deja un **informe en español** de qué entró y por qué no entró el resto, en el aviso, la ventana de Iluminación y `lighting_state` del MCP (`gather_report`, `last_error`).
- Cada luz tiene **«Modo (horneado)»**: *Mixta* (por defecto, como antes), *Horneada* (luz directa, sombras suaves y rebote horneados) o *Solo tiempo real*. El sol toma el modo de la Directional Light o del nuevo ajuste *Sol*.
- La luz directa y las **sombras suaves** se guardan en las sondas y en el lightmap de superficie (se enciende solo a 0,5 m). Formato `.crbake` versión 3; los de la versión 2 se siguen leyendo.
- MCP: `bake_lighting` acepta `sun_mode`, `surface_texel` y `direct_samples`.

**Límites**: no hay lightmaps con UV2 (las sombras horneadas tienen la nitidez de la celda, 0,5 m por defecto). Si el sol se mueve más de 2° con la hora del día, vuelve a tiempo real y hay que volver a hornear. Los objetos que se mueven no proyectan sombra de las luces horneadas, y un sol horneado no da sombra a los dinámicos (en exteriores deja el sol en *Mixta*). Las pasadas transparentes y *forward* siguen sumando el sol horneado.

## Qué tienes que hacer

- **Vuelve a aplicar el preset.** Una escena guarda los valores, no el preset: si ya usabas «Ultra realista» o «Realista», aplícalo de nuevo en el volumen de post-procesado para ver el aspecto nuevo.
- Si usas grano en otras escenas, revísalas: los negros ahora son negros.
- **VR**: si quieres que el presupuesto adaptativo baje calidad en VR, enciéndelo; ya no se activa solo. Ajusta «Resolución VR» si tu GPU no llega al refresco del casco.
- **Exposición**: las escenas que guardaron `max_ev` 1,4 conservan ese tope; súbelo a 2 para el comportamiento nuevo de noche.
- **Horneado**: vuelve a hornear. Para hornear la luz directa y las sombras, pon «Modo (horneado)» en *Horneada* en cada luz o en el ajuste *Sol*.
- Nada más: las escenas, prefabs y proyectos de la 2.4.9 se abren igual.

**Otros cambios de comportamiento**
- Abrir una escena con el JSON roto deja el editor con una escena nueva sin ruta (así Guardar no pisa la anterior). Si el archivo no existe, la escena abierta sigue como estaba.
- Abrir o crear una escena durante Play sale de Play primero.
- Al cambiar de escena en el editor, las teselas esculpidas sin guardar ya no se guardan solas: se descartan si eliges no guardar.

## Descarga

1. Descarga **`Cramion-win64.zip`** y **`Cramion-win64.zip.sha256`** de los archivos de esta versión.
2. Comprueba el zip. En PowerShell, en la carpeta de descarga:

   ```powershell
   (Get-FileHash .\Cramion-win64.zip -Algorithm SHA256).Hash.ToLower()
   Get-Content .\Cramion-win64.zip.sha256
   ```

   Los dos valores deben coincidir con:

   ```
   3b2f31dbabdff89ffb99ec5d31fa9241ab426991b9c077b78be2612c6a12f766
   ```

   En Linux o Git Bash: `sha256sum -c Cramion-win64.zip.sha256`.
3. Descomprime y abre `CramionEditor.exe`. Si ya tienes Cramion, el actualizador del editor ofrece la 2.4.10.

Requisitos: Windows 10/11 x64 y una GPU con Vulkan.

La única descarga oficial es la de esta página, y es gratis. El registro completo está en [CHANGELOG.md](https://github.com/MantraxStudios/Cramion/blob/main/CHANGELOG.md).

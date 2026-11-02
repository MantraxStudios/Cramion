# Cramion 2.7.1

**Animación por claves como en Blender y trazado de rayos que por fin se ve mejor que el raster.** Llega un Dope Sheet con gráficas Bézier para animar cualquier propiedad. Además, un repaso a fondo del RTX: por qué se veía sucio y ruidoso, los personajes animados dentro de los rayos y la escena de rayos mucho más barata de mantener.

## Descarga

- `Cramion-win64.zip`: descomprime y abre `CramionEditor.exe`. Si ya tienes Cramion, el actualizador te avisa solo.
- SHA-256 de `Cramion-win64.zip`: `e1f0d5c5c61de2a400102e258e17a2a3d6739c79be58bab9e33ba47a9dc0093b`

## Lo nuevo

### Animación por claves (como Blender)
- **Componente `KeyframeAnimation`** y ventana **Animación** con Dope Sheet y Gráficas.
- **Claves desde cualquier sitio**: rombos en el Inspector, `I` / `Alt+I` en la Escena y grabación automática al soltar el gizmo.
- **Curvas Bézier como las F-Curves**: asas automáticas, vector, alineadas y libres; interpolación constante, lineal o Bézier; extrapolación constante, lineal o en ciclo.
- **Anima cualquier propiedad** float, int, bool, enum, Vec2 o Vec3 de cualquier componente, de la propia entidad o de otra.
- Suena en Play y en los juegos exportados: una vez, en bucle o ida y vuelta, con velocidad.

### Trazado de rayos: lo que se veía mal
- **GI sin manchas que nadan**: todos los rayos de la GI salían por la misma espiral del hemisferio por un fallo del ruido. Ahora cubren el hemisferio entero y la GI converge.
- **Sin oscurecer dos veces**: con GI por rayos el SSAO ya no se aplica encima, y las esquinas y los interiores dejan de verse sucios.
- **Las sombras por rayos sustituyen a los mapas** cuando todo lo que proyecta sombra está en la escena de rayos: sin penumbras dobles, acné ni sombras despegadas.
- **Personajes animados en los rayos**: hasta 32, los más cercanos, salen en los reflejos (también fuera de pantalla), en la GI y en las sombras por rayos.
- **Reflejos estables**: lo pulido ya no resbala ni "hierve" al mover la cámara, y no deja estela con TAA ni FSR 3.
- **Penumbras sin parpadeo**: filtro nuevo que suaviza según el tamaño real de la penumbra y deja nítidas las sombras de contacto.
- **Más nitidez con TAA, FSR y DLSS**: los filtros de sombras, reflejos y GI ya no leen la historia medio píxel desplazada.
- **Lámparas y emisivos iluminan de verdad** en la GI por rayos.
- **Path tracing**: la niebla ya no sale tres veces más oscura y lo que lleva lightmap no se ve lavado.
- **Editar un material** se ve también en los reflejos y la GI por rayos.

### Trazado de rayos: coste
- **TLAS persistente**: solo se escribe lo que cambia y, si solo hay movimiento, se reajusta en vez de reconstruirse.
- **BLAS** por tandas y compactadas sin esperar a la GPU, con toda la memoria en VMA.
- Con la GI apagada, sus rayos ya no se calculan para nada.

### Luz volumétrica
- **Linternas y faroles en la niebla**: el fondo ya no se llena de puntos y niebla gris o blanca, sobre todo con pocos pasos de volumétrica (el presupuesto adaptativo).

### Rendimiento de CPU
- **Animación por claves unas 4 veces más rápida** en Play.
- **Poses de los personajes en paralelo** en varios hilos (`anim.ParallelPoses`).
- La jerarquía activa se recorre una vez, no cinco por frame, y la vegetación no rehace nada si no cambió.
- Las luces puntuales solo redibujan las caras de su sombra que toca algo animado.
- Los buffers que cambian cada frame van a la VRAM visible desde la CPU (ReBAR).

## Arreglos
- **Animar la posición ya no pisa el giro** que dejan la física o los scripts.

## Cambios a tener en cuenta
- **Con GI por rayos no hay SSAO**, porque la oclusión ya viene en la GI. Sin rayos, todo igual que antes.
- **Sombras por rayos con todos los actores en la escena de rayos**: los mapas de sombras ya no oscurecen lo que los rayos ven iluminado. La vista de cascadas del editor sigue mostrando los mapas.
- **Alfa de la imagen HDR**: además de los transparentes, lleva la máscara de los reflejos nítidos.
- **Personajes en la escena de rayos**: cada uno ocupa su BLAS y una copia de sus vértices en la GPU (hasta 32).
- **Linux**: esta versión sale solo para Windows. El paquete de Linux llegará aparte.

El detalle completo está en `CHANGELOG.md`.

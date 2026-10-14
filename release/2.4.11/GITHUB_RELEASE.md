# Cramion 2.4.11

**Corrección sobre la 2.4.10: al salir de una habitación y volver a entrar ya no se ve más oscura hasta que la luz vuelve de golpe en un momento cualquiera, los reflejos de pantalla (SSR) están reescritos, los interiores de día ya no salen oscuros y el trazado de rayos se enciende de verdad en el juego exportado.**

Es una versión de corrección: solo trae estos arreglos sobre la 2.4.10. El actualizador del editor la ofrece sola a quien ya tiene la 2.4.10.

## A quién afecta

- **Habitaciones más oscuras al volver**: a las escenas con sonda de reflexión (de ella salen los reflejos de respaldo y la luz rebotada de lo que no está en pantalla), sobre todo interiores con varias habitaciones o casas con exterior. Con el trazado de rayos activo la sonda no se usa.
- **Exposición automática**: a las escenas con exposición automática y zonas interiores de día.
- **Reflejos de pantalla (SSR)**: a quien use SSR sin trazado de rayos.
- **Trazado de rayos**: a los juegos exportados con `ray_tracing=1` en una GPU con rayos.
- **Otros**: luz volumétrica en interiores, reflejos del agua y el editor con Escena y Juego visibles a la vez.

## Habitación más oscura al volver a entrar

**Qué pasaba**: la sonda de reflexión es un cubo que sigue a la cámara (más el anterior, para fundir de uno a otro), y solo se volvía a capturar al pararse a más de 4 m o tras recorrer 15 m. Al salir de una habitación y volver andando, la habitación se iluminaba con la captura de fuera (los reflejos de respaldo y los rayos de la GI de pantalla que salen de ella), así que se veía más oscura. En cuanto el jugador se paraba, todo cambiaba de golpe.

**Qué se ha corregido**
- **La sonda sabe qué ve cada cubo**: al terminar una captura se leen sus distancias en la CPU. Si la cámara se va a donde el cubo no ve (otra habitación), se recaptura enseguida, sin esperar a que se pare. Una persona o un poste delante no cuentan.
- **Vuelve al cubo de la habitación sin capturar**: el cubo anterior se conserva junto con la luz con que se capturó. Si vuelves a su lado y la luz no ha cambiado, la habitación lo recupera con un fundido, también al entrar por la misma puerta. No va y viene entre los dos cubos en el canto de una puerta.
- **Pararse de verdad**: la recaptura al pararse pide 0,3 s despacio (darse la vuelta ya no cuenta), y se recaptura si te quedas quieto entre dos capturas, por ejemplo justo dentro de la puerta.
- **Sin saltos**: todos los cambios son fundidos de 0,5 s (antes 0,35 s), la GI de pantalla funde igual que la iluminación (antes cambiaba de golpe) y no hay un rato de solo cielo entre el cubo viejo y el nuevo.
- Los cubos se vacían al cambiar de escena, y una captura durante la que cambia la luz se repite.

## Exposición automática en interiores de día

**Qué pasaba**: el ajuste de noche de la 2.4.10 (por encima de +0,75 EV solo compensa una parte) también se aplicaba de día, así que los interiores salían más oscuros.

**Qué se ha corregido**: ahora depende de la luz del día. De día se compensa del todo, como en la 2.4.9; de noche, como en la 2.4.10; al atardecer, a medias.

## Reflejos de pantalla (SSR) reescritos

**Qué pasaba**: rayas y huecos en ángulos rasantes, objetos finos ensanchados, aciertos falsos debajo de las mesas, estelas al mover la cámara y superficies rugosas con reflejos casi de espejo.

**Qué se ha corregido**
- **Trazado sobre una pirámide de profundidad** (Hi-Z), como FidelityFX SSSR, con un grosor ajustado y una prueba de plano.
- **Rugosidad de verdad**: cada reflejo lee el mip de la imagen anterior que cubre su cono. Lo muy rugoso o muy lejano pasa a la sonda de forma suave, igual a cualquier resolución.
- **Sin estelas**: ya no usa historial temporal; un filtro espacial según la profundidad y la normal suaviza solo lo rugoso.
- La oclusión especular ya no oscurece el SSR, solo el respaldo (sonda o cielo).
- Las capturas de la sonda y la otra vista del editor ya no pisan la imagen que lee el SSR.

## Trazado de rayos en el juego exportado

**Qué pasaba**: la escena de rayos se construía vacía en la pantalla de carga y no se volvía a hacer, así que el juego exportado nunca usaba rayos aunque estuvieran activados.

**Qué se ha corregido**
- La escena de rayos se construye al terminar la carga.
- La caché de luz rebotada ya no se vacía al rehacer la escena de rayos (sí al cambiar de escena).
- Los **reflejos por rayos** ya no se arrastran al mover la cámara.
- Los objetos sin sombra (`cast_shadows` apagado) ya no proyectan sombra por rayos.

## Otros arreglos

- **Luz volumétrica**: el sol ya no se cuela unos frames en los interiores al girar la cámara.
- **Reflejos del agua**: ya no salen estirados y a bandas detrás de árboles y postes.
- **Editor con Escena y Juego a la vez**: el sol y el ambiente ya no se escalan dos veces por frame.
- **Cielo (IBL)**: sincronización correcta con los shaders de rayos que lo leen.

## Qué tienes que hacer

- Nada: las escenas, prefabs y proyectos de la 2.4.10 se abren igual.
- **Trazado de rayos**: si exportas con `ray_tracing=1`, el juego usa ahora reflejos y GI por rayos en las GPUs que los soportan. Revisa el rendimiento. Para ver el SSR, apaga el trazado de rayos.
- **Exposición**: los interiores de día se ven más claros que en la 2.4.10, como en la 2.4.9. Si subiste luces o la exposición para compensar, revísalo.

## Descarga

1. Descarga **`Cramion-win64.zip`** y **`Cramion-win64.zip.sha256`** de los archivos de esta versión.
2. Comprueba el zip. En PowerShell, en la carpeta de descarga:

   ```powershell
   (Get-FileHash .\Cramion-win64.zip -Algorithm SHA256).Hash.ToLower()
   Get-Content .\Cramion-win64.zip.sha256
   ```

   Los dos valores deben coincidir con:

   ```
   7f1add3a8a566fd2ed155a33afdce2b9d68c6bcdd11864211d621892f8349470
   ```

   En Linux o Git Bash: `sha256sum -c Cramion-win64.zip.sha256`.
3. Descomprime y abre `CramionEditor.exe`. Si ya tienes Cramion, el actualizador del editor ofrece la 2.4.11.

Requisitos: Windows 10/11 x64 y una GPU con Vulkan.

La única descarga oficial es la de esta página, y es gratis. El registro completo está en [CHANGELOG.md](https://github.com/MantraxStudios/Cramion/blob/main/CHANGELOG.md).

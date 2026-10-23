# Cramion 2.6.3

**Efectos nítidos con TAA y que no desaparecen en combate.** Una versión centrada en el VFX Graph: los efectos se ven tan nítidos como las mallas con el TAA y el escalado, ya no dejan estela, no se pierden al encadenar habilidades y los topes internos se sueltan solos y avisan.

## Descarga

- `Cramion-win64.zip`: descomprime y abre `CramionEditor.exe`. Si ya tienes Cramion, el actualizador te avisa solo.
- `Cramion-linux64.tar.gz`: Linux x64 (X11 o XWayland, Vulkan 1.3). `tar xzf Cramion-linux64.tar.gz` y abre `./CramionEditor`. En Linux se actualiza a mano.
- SHA-256 de `Cramion-win64.zip`: `c9104742bdf1ba2266c6a6ab08d12368940fde1723c17a3c4627deb22d9a5d85`
- SHA-256 de `Cramion-linux64.tar.gz`: `deba7801715cc436523edbb6137cc9c7d2f408c0b6c5437aa60a4cfb8e4aa616`

## Lo nuevo

### Nitidez con TAA y escalado
- **El VFX Graph, las partículas, la lluvia y los sprites se dibujan con el jitter del TAA**, como lo opaco. Antes la historia los emborronaba (~1,7 px de salida al 58 %).
- **Máscara reactiva**: los transparentes marcan dónde están y el TAA del motor da más peso al frame nuevo ahí. Así no dejan estela los proyectiles, las chispas ni el humo. FSR 3 la recibe como máscara reactiva y de transparencia.
- **CVars nuevas**: `render.taa.FrameWeight`, `render.taa.ReactiveWeight` y `render.taa.ReactiveStrength`.
- **Samplers de VFX y Decals** con anisotropía y el sesgo de mips del escalado.
- Los halos aditivos con alfa 0 ya no desaparecen.

### Presupuesto de efectos
- **Los efectos terminados ya no cuentan** en el tope de su tipo.
- **Grupos**: el componente `VfxGroup`, o la raíz del prefab, se crea o se quita entero. Una habilidad de 23 emisores cuenta como una.
- **Jugador local**: con `local_player` / `setEffectLocalPlayer(true)` sus efectos nunca se descartan. `priority` ordena el resto.
- **`Physics.raycast`, `sphereCast` y `overlapSphere`** tienen la opción `triggers` (tocarlos o ignorarlos).

### Topes que se sueltan
- **Texturas del VFX Graph**: hasta 256. Se sueltan las que nadie usa, también al salir de Play y al cambiar de escena.
- **Mallas de partículas**: 128, con un pool que crece sin esperas. Las que ningún efecto usa se liberan.
- Si se llega al tope, sale un aviso en la consola y en el editor del efecto.

### Dibujo de efectos
- **Partículas de malla con su propio material**: antes las hojas y astillas salían invisibles.
- **Hierba**: los decals la tiñen, y «Apartar hierba» en el VisualEffect la aparta sin física.
- **Fundido suave** en lo opaco (`MeshRenderer.smooth_fade`), sin ajedrezado.
- **Una cinta por proyectil**: el id de la cinta sale del evento.
- **Colisiones fuera de la vista**: contra el terreno bajo el emisor o contra un plano en el mundo.
- **KillBox** con la opción «En el mundo».

### Herramientas
- **Recarga en caliente** de los `.crvfx` y de las texturas de efectos y decals cambiadas con la misma ruta.
- **`.crmat`**: un número suelto en `shader_values` vale como `[x]`.
- **`.crshader`**: hasta 16 propiedades, con un error claro si te pasas.
- **Scripts de C++**: se compilan en segundo plano y Play entra al terminar, sin bloquear el editor.
- **MCP**: `cpp_compile` y `play` responden al momento; el resultado se consulta con `cpp_compile_status`.
- **`force`** descarta de verdad: borra el autoguardado y no muestra el diálogo de recuperar.

### Cursor
- `Input.setCursorLockMode("none" | "locked" | "confined")` y `Input.setCursorVisible`, como `Cursor.lockState` de Unity.

## Cambios a tener en cuenta
- **`.crshader`**: los juegos exportados que los usan hay que volver a exportarlos, porque el registro de parámetros pasa de 8 a 16.
- **Salida Malla del VFX Graph sin `.crmat`**: ahora usa la textura de su modelo. Para volver al comportamiento de antes, marca «Textura de la salida en la malla».
- **Alfa de la imagen HDR**: en la vista principal ahora es la máscara reactiva, no la distancia. Las capturas de la sonda no cambian.
- **Presupuesto**: los efectos Héroe, Combate e Impacto dentro de un prefab se agrupan por su raíz. `vfx.groupPrefabs 0` vuelve a contarlos sueltos. Un efecto terminado deja de simularse y vuelve a empezar desde cero.
- **MCP**: `cpp_compile` ya no trae los errores en la respuesta; pídelos con `cpp_compile_status` o pasa `sync:true`.
- **Transparentes grandes y quietos** (cristales con refracción): el TAA los suaviza algo menos. Se ajusta con `render.taa.ReactiveStrength`.

El detalle completo está en `CHANGELOG.md`.

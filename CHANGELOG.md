# Cambios

## 2.6.3 (sin publicar)

Efectos nítidos y que no desaparecen en combate: respuesta a la petición de Keryan del 21-10-2026 (VFX, imagen y nitidez), apartados 1 a 5.

### Nitidez de los efectos con TAA y escalado (apartado 1)
- **Jitter del TAA en todo lo que se dibuja encima** (`VulkanRendererVfx.cpp`, `VulkanRendererSprites.cpp`, `recordParticlePass`, `recordPrecipitationPass`): el VFX Graph, las partículas, la lluvia y los sprites se dibujan con la misma proyección con jitter que lo opaco y su profundidad (`jittered_projection_`, `current_jittered_view_projection_`). Antes iban sin él y la historia los emborronaba (~1 px interno, ~1,7 px de salida al 58 %), mientras las mallas `.crshader` transparentes salían nítidas. Las matrices sin jitter siguen para el culling, las luces y los vectores de movimiento.
- **Máscara reactiva**: los transparentes (VFX Graph, partículas, sprites, lluvia y `.crshader` transparentes) acumulan su opacidad en el alfa de la imagen HDR, que en la vista principal empieza a 0 (`lighting.frag` y el path tracing ya no escriben ahí la distancia; las capturas de la sonda sí). Los efectos aditivos marcan cuánto aportan. Sin attachments ni pases nuevos para el TAA del motor: `taa.frag` lee la máscara (dilatada 1 px con los vecinos que ya leía), sube el peso del frame nuevo hacia `render.taa.ReactiveWeight` y estrecha el recorte (de 1,25σ a 0,75σ) donde la hay. Sin estela en proyectiles, chispas y humo.
- **FSR 3 recibe la máscara** como `reactive` y `transparencyAndComposition` (`upscaler_reactive.frag`: un R8 a la resolución interna, solo con FSR 3; `UpscaleDispatch::reactive`).
- **CVars del TAA**: `render.taa.FrameWeight` (0,12, el peso de antes), `render.taa.ReactiveWeight` (0,5) y `render.taa.ReactiveStrength` (1; 0 apaga la máscara en el TAA y en FSR 3).
- **Samplers de VFX y Decals** con anisotropía y el sesgo de mips de la compensación del escalado (`upscaleMipBias`), como los modelos y el terreno. Antes, al 58 %, un mip más bajo y sin anisotropía. Se rehacen al cambiar la calidad (`VfxPass::setTextureFiltering`, `applyTextureFiltering`).
- **Halos aditivos** (`vfx.frag`, E6): en la mezcla premultiplicada, alfa 0 con color ya no se descarta ni sale negro (salvo con recorte de alfa propio).

### Las habilidades ya no desaparecen en combate (apartado 2)
- **Los efectos terminados no cuentan** (`VfxSystem::planFrame`, E21): un VisualEffect que no emite, sin eventos ni ráfagas pendientes y sin partículas vivas (lectura de la GPU, o pasada la vida máxima del bloque Lifetime, con al menos 0,25 s sin nacer nada) sale del presupuesto y deja de simularse (suelta su ranura de la GPU). `playEffect()`, un evento o un ciclo nuevo lo vuelven a empezar. Antes contaba hasta borrar la entidad: tres habilidades de 20-50 efectos llenaban el tope de Combate y, con DontCreate, las siguientes no salían.
- **Presupuesto por grupo** (E5; componente nuevo `VfxGroup`; sin él, la raíz del prefab con `vfx.groupPrefabs`, solo Héroe, Combate e Impacto): un grupo ocupa una instancia del tipo más importante de sus efectos y se crea o se quita entero. Una habilidad de 23 emisores ya no llena el tope de Baja ni sale a trozos. `playAt` con DontCreate cuenta grupos.
- **Efectos del jugador local** (`local_player` en el VisualEffect o en el VfxGroup, `entity().setEffectLocalPlayer(true)`): nunca se descartan, ni por distancia ni por tope; usan los límites de Héroe y entran los primeros. `priority` (`setEffectPriority(n)`, -100..100) ordena el resto: con el tope lleno salen antes los de otros jugadores y los de ambiente.
- **`Physics.raycast` / `sphereCast` / `overlapSphere` con `triggers`** (argumento tras `ignorar`: `true` los toca, `false` no, `nil` = Project Settings > Physics); en el SDK de C++, `Physics::QueryTriggers`. Las esferas trigger de los efectos desviaban la puntería.

### Topes de los efectos (apartado 3)
- **Ranuras de textura del VFX Graph que se sueltan** (E17): hasta 256 (antes 64; según `maxPerStageResources` del dispositivo). Sin sitio se suelta la que lleva más tiempo sin usarse (nunca una del frame en curso ni del anterior); al salir de Play o cambiar de escena se sueltan las que nadie usa. La imagen vieja vive hasta que la GPU acaba (`retireAfterGpu`, sin esperas). Si de verdad no caben, aviso en la consola («[VFX] AVISO: …») y en el editor del efecto.
- **Mallas de partículas sin el tope de 16** (E12): 128 ranuras y un pool de vértices e índices que crece ×2 sin `waitIdle` (hasta 2 M vértices). El tope cuenta las mallas vivas, no las cargadas en la sesión: las que ningún efecto usa se sueltan y su sitio se reutiliza tras los frames en vuelo. Aviso en la consola y en el editor del efecto.

### Fallos de dibujo de efectos (apartado 4)
- **Partícula de malla con su material** (E10): una salida Malla sin `.crmat` usa el color y la textura base de su modelo, con su UV. Antes usaba la textura de la salida y las hojas y astillas no se veían. «Textura de la salida en la malla» (`mesh_output_texture`) vuelve a lo de antes.
- **Hierba, decals y efectos a ras de suelo** (E16): los decals tiñen la hierba (cada brizna toma el decal de su raíz) y `VisualEffect.grass_radius` («Apartar hierba») aparta la hierba sin física, con los interactores de siempre.
- **Fundido suave en lo opaco** (E20): `MeshRenderer.smooth_fade` («Fundido suave», o `setMaterialParam("smoothFade", true)`): con opacidad < 1 el objeto se dibuja mezclado en la pasada de los transparentes, sin el ajedrezado del tramado. Por defecto, como antes.
- **Una cinta por proyectil** (E4): «Atributos de cinta» tiene «Id del evento» (una cinta por valor del evento, hasta 256 por efecto) y los bloques «Emitir…» tienen «Valor = id de la partícula». Antes el id salía de `gl_GlobalInvocationID.x % tiras`.
- **Colisiones sin la vista** (E3): «Colisión con la escena» tiene «Suelo sin la vista» (el terreno bajo el emisor hace de suelo aunque quede fuera de pantalla; `EmitOnCollision` y `TriggerOnCollide` se disparan igual). «Colisión con plano» tiene «En el mundo» y «Sobre el terreno».
- **KillBox** (E7): documentado que va en el espacio del emisor; opción nueva «En el mundo».

### Herramientas de efectos (apartado 5)
- **Recarga en caliente de `.crvfx`** (E1), y del `.crmat` de una salida Malla: los efectos que lo usan empiezan de nuevo con el del disco; el abierto en su editor también, si no tiene cambios sin guardar.
- **Texturas de efectos y de decals cambiadas con la misma ruta** (E2): se releen en su misma ranura o capa del atlas (recarga en caliente y al volver a pedirlas tras reabrir el proyecto). Ya no hace falta renombrarlas.
- **`.crmat`: número suelto en `shader_values`** (E19): `"brillo": 0.5` vale como `[0.5]`; lo que no es número ni lista se ignora con un aviso `[Material]` que nombra el archivo y la propiedad.
- **`.crshader`: 16 propiedades de números** (E11, antes 8) y error claro al pasarse. Antes la compilación síncrona compilaba el shader que no se había podido leer y el error de glslang tapaba el mensaje. Los juegos exportados con `.crshader` precompilados hay que volver a exportarlos.
- **Scripts de C++ sin parar el editor** (E14): Play con scripts por compilar compila en segundo plano («Compilando… N s») y entra solo al terminar; Stop lo cancela.
- **MCP asíncrono** (E9/E15): `cpp_compile` responde al momento con `{status:"compiling", job}` y el resultado llega con `cpp_compile_status`; `play` responde `mode:"starting"` mientras espera a la compilación. `sync:true` mantiene el modo anterior.
- **`force` descarta de verdad** (E8): `open_scene`, `new_scene`, `open_project` y `create_project` con `force=true` borran el autoguardado y no muestran el diálogo de recuperar.

### Cursor de los juegos
- `Input.setCursorLockMode("none" | "locked" | "confined")`, `setCursorVisible`, `getCursorLockMode` e `isCursorVisible`, como `Cursor.lockState` de Unity. Escape en el editor o perder el foco lo sueltan sin cambiar el modo del juego; un clic en la vista lo vuelve a aplicar.

## 2.6.2 (sin publicar)

### Render
- Sombras de contacto: ya no se cortan de golpe a 60 m (se funden en el último tercio de la nueva `contact_shadow_distance`, 60 m por defecto, en Post-procesado > Efectos) y nunca pasan del 85 % del alcance de las cascadas (solas eran manchas negras sueltas). Su largo sigue el texel de las cascadas de forma continua (antes saltaba en cada corte de cascada), el grosor deja siempre margen sobre el sesgo de lejos, el fundido en el borde de pantalla es de 16 px (era el 5 % de la imagen: se borraban antes del borde de la vista) y no se aplican a la hierba (su depth es ruido: motas negras).
- Luz volumétrica: la sombra del sol en el aire se desvanece al final de la última cascada igual que en el suelo (antes las columnas oscuras de la niebla acababan en un corte).
- `cramion_render_bench --far [--hour H]`: escena larga con niebla volumétrica para ver estos fundidos.

### Memoria en el streaming
- **Presupuesto de VRAM con LRU** (`streamModels`): por encima del 80 % del presupuesto que da `VK_EXT_memory_budget`, salen primero los modelos que hace más tiempo que no se ven (con al menos 1 s fuera de vista), aunque no lleven `render.streaming.IdleSeconds`, y el detalle de texturas baja a los 0,5 s en vez de a los 3 s. Lo que se expulsa se descuenta (estimado) hasta la siguiente medida de la VRAM, para no vaciarlo todo en ese medio segundo. Por debajo del 60 % no sale nada visto en los últimos 30 s, así que girar la cámara y volver ya no obliga a resubir. Antes la expulsión iba solo por tiempo: sin presupuesto, una escena con más variedad que VRAM hacía paginar al driver (tirones y riesgo de `ErrorDeviceLost`).
- **Trazado de rayos sin fijar modelos enteros**: al expulsar un modelo o cambiar el detalle de sus texturas con la escena de rayos construida, solo se quedan sus texturas (lo único que lee esa escena, porque los triángulos ya los copió). Mallas, descriptor sets y uniformes salen como los demás. Antes se quedaba el `SkinnedModel` entero hasta la siguiente reconstrucción: con RT, expulsar no liberaba nada y un cambio de detalle duplicaba el modelo. El log de `[Streaming]` cuenta ahora «texturas fijadas por RT».
- **RAM de los modelos sin uso** (`RenderSync::releaseIdleModelMemory`): un modelo fuera de la GPU y sin ninguna instancia durante `render.streaming.CpuReleaseSeconds` (30 s) suelta los píxeles de sus texturas que tienen archivo. Quedan «perezosas», como las de los `.crmat`, y al volver se releen de la caché BC en el hilo de streaming. Una pasada por segundo y como mucho 4 modelos por pasada. Se apaga con `render.streaming.ReleaseCpuTextures=0`. Antes cada modelo que había salido alguna vez se quedaba con todas sus texturas en la RAM hasta cambiar de escena.
- **Caché de texto de prefabs con tope** (`ScriptSystem::prefabText`): LRU de 48 MB. Antes guardaba para siempre el texto de cada prefab instanciado, y recorrer Ashbound (1780 celdas) eran unos 173 MB.
- **Formas de física**: `mesh_shapes` y `runtime_shapes` se podan; sale lo que solo tiene la caché (ningún cuerpo lo usa) en dos pasadas seguidas, cada 600 sincronizaciones. Antes solo se vaciaban al cambiar de escena, y una forma vieja podía quedar ligada a la dirección de una malla ya liberada. `runtimeMeshShape` recorre solo el tramo de su malla, no el mapa entero por cada cuerpo.

### Trabajo repetido en el streaming
- **Vegetación sembrada por bosque** (`RenderSync::syncFoliage`): cada `Foliage` tiene su firma propia (parámetros, posición, agua y los terrenos que pisa) y su siembra se guarda en caché. Al entrar o salir una celda solo se siembra la nueva; las demás se copian de la caché (y no se hace la foto del terreno de las que ya están). Antes cada celda volvía a sembrar todos los bosques cargados (en Ashbound, 1569 de las 1780 celdas traen uno). La caché se vacía al cambiar de escena y se poda por antigüedad pasadas las 512 entradas (unos 5 KB cada una).
- **Recorridos sin lo desactivado** (`World::forEachActiveDepthFirst`): no baja a los hijos de una entidad desactivada y la entidad ya no tiene que subir por sus padres con `activeInHierarchy`. Lo usan los recorridos de cada frame de `RenderSync` (actores, luces, cámara principal, agua y cámaras con Render Texture), la cámara principal del editor (audio) y la del juego, y el `Profiler` del juego. Las celdas que el juego deja desactivadas en caché y los mapas ocultos (Valley, ChaosProps: unas 30-40 mil entidades) ya no se visitan en cada frame.
- **Estados de animación**: la tabla por entidad solo se recorre para olvidar cuando sobra algún estado (antes, entera en cada frame con miles de props estáticos), y el índice entidad → actor reserva su tamaño antes de rehacerse.
- **Caché de texturas de la GPU** (`SkinnedModel.cpp`): una textura que otro hilo de streaming ya está decodificando y subiendo no se vuelve a hacer; el segundo hilo espera a que termine (con tope de 2 s) y depende de su lote. Antes, con `CRAMION_STREAM_THREADS=3` y celdas que traen los mismos materiales, cada hilo decodificaba (o comprimía a BC7) y subía la misma imagen. La clave de las texturas perezosas (las de los `.crmat`) incluye ahora su uso (sRGB o lineal, BC1/BC7): la misma imagen como albedo y como máscara compartía el formato del primero que llegaba. La ruta se normaliza en la clave (separadores y, en Windows, mayúsculas).

### Carga y streaming sin tirones
- **`Scene.instantiateAsync` / `Scene.destroyDeferred`** (nuevo, `SceneStreamingApi.cpp`, `ecs::PrefabSpawn`): el prefab se lee y se parsea en los hilos de IO (con caché LRU de documentos ya parseados, `scene.PrefabCacheMB` = 192 MB estimados en memoria —8 veces el texto, lo que ocupa el JSON parseado—, y la fecha del archivo como mucho cada 2 s) y sus entidades se crean por tandas con `scene.SpawnBudgetMs` (2 ms) por frame bajo una raíz desactivada que se activa una sola vez al final; el remapeo de referencias también va por tandas. Devuelve un id: `Scene.spawnProgress(id)` (0..1, -1 si falla), `Scene.spawnResult(id)` (la raíz, una vez), `Scene.cancelSpawn(id)` (lo ya creado se borra como `Scene.destroy`, con sus avisos) y `Scene.preloadPrefab(ruta)`. `Scene.destroyDeferred(e)` lo desactiva al momento y lo borra de las hojas hacia arriba con `scene.DestroyBudgetMs` (1 ms), con los mismos avisos que `Scene.destroy`. Medido antes: 9-43 ms por celda de Ashbound y 265-491 ms por `Valley.crprefab` en un solo frame. `Scene.instantiate` sigue igual (síncrono).
- **Especies de árboles estables** (`RenderSync::syncFoliage`): las especies y ajustes de la vegetación los decide un bosque «dueño» fijo mientras siga activo (si sale, otro con las mismas especies; con varios y ninguno igual, se quedan las de ahora). Antes era el primer `Foliage` de la vista de EnTT, que cambia con cada celda que entra o sale: cada alta o baja de celda rehacía las 9 mallas de árbol y cambiaba la forma de todos los árboles del mapa (70-135 ms medidos en «Vegetacion» por operación).
- **Especies sin `waitIdle`** (`FoliagePass::setSpecies`): si cambian, las mallas se construyen y suben en segundo plano (`VulkanDevice::runTask`) y `pollAsync` las cambia al estar listas; las de antes se retiran a los frames en vuelo. Antes paraba toda la GPU y construía en el hilo principal (era el sospechoso principal del `ErrorDeviceLost` de `cramion_stream_bench --rt 0`, pero no era la causa: la encontró la ronda R1, más abajo).
- **`VulkanDevice::waitIdle` toma también la cola de transferencia**: `vkDeviceWaitIdle` exige acceso exclusivo a todas las colas y un hilo de streaming podía enviar a la vez a la de transferencia. Un `ErrorDeviceLost` en `waitIdle` durante el desenrollado de otra excepción ya no acaba en `std::terminate` (se avisa en el log y sigue la primera).
- **Culling en GPU con crecimiento geométrico** (`GpuCulling::setClusters`): x1,5 y con margen (mínimo 4096 clústeres) y sin encoger. Antes se recreaba a la medida justa con un `waitIdle` cada vez («Culling en GPU: 5094 / 5096 / 5098… clusteres», decenas seguidas al cargar celdas).
- **Escena de rayos**: los modelos y variantes que entran por streaming la reconstruyen como mucho una vez cada 4 s (se juntan); antes, 1 s después de cada uno (100-1000 ms cada reconstrucción con `waitIdle`). Un modelo que ya tuvo BLAS la conserva aunque se quede sin instancias (antes, cada celda que salía y volvía pedía otra reconstrucción entera). Con las especies estables ya no se reconstruye en cada celda con árboles. Banco (4 ciclos): de ~212 reconstrucciones a 19-24; siguen las de los modelos que vuelven tras la expulsión por streaming (pendiente: BLAS incremental).
- **`cramion_stream_bench --async [ms]`**: instancia las celdas como `Scene.instantiateAsync`. Medido con Ashbound (RTX 4060 Ti, 4 ciclos): «Vegetacion» de 70-135 ms a 0 en los frames de alta y baja; RenderSync en el frame de instanciar de 82 ms de media a 1,7; con `--async`, instanciar pasa de 10 ms de media (p99 22) a 2 ms como máximo; frame caliente p95 de 198 a 14,5 ms. `--rt 0` perdía el dispositivo: arreglado en la ronda R1 (más abajo).
- **`.crshader` en segundo plano** (`render.AsyncSurfaceShaders`): la primera vez que un material lo usa se compila en un hilo y mientras se ve con el shader estándar; al terminar se rehacen sus materiales. `RenderSync::surfaceShader` (editor, MCP, auto-prueba) sigue siendo síncrono y espera a la pendiente. Medido antes: 661 ms en un frame al entrar la primera antorcha.

### Integración y medidas
- **`Scene.instantiate` (síncrono) más barato**: `ecs::instantiatePrefab` ya no copia en profundidad todos los registros del prefab (recorre los del documento), y `parse()` de `Prefab.cpp` devolvía con `?:` una copia del documento entero (ahora se mueve). Banco: `instantiatePrefab` de 9,4 ms de media (p99 28, máx. 43) a 5 ms (p99 10,7, máx. 15,7) por celda de Ashbound.
- **Especies de árboles con histéresis**: sin bosque dueño y con un solo bosque activo distinto, se adopta solo si sigue solo 2 s (antes, en el acto). Con streaming, el primer bosque de una zona nueva está solo unos frames y adoptarlo cambiaba las especies y rehacía la escena de rayos entera en cada pasada (banco, 4 ciclos: de 17 reconstrucciones por especies a 1).
- **Log de la escena de rayos**: «Escena de rayos: se rehace por modelos / terrenos / especies de arboles» en cada reconstrucción, para atribuir los tirones.
- **Marcas de diagnóstico de la GPU** (`CRAMION_GPU_CHECKPOINTS=1`, NVIDIA: `VK_NV_device_diagnostic_checkpoints`): cada `markPass` y cada parte de las cascadas (actores, terreno, vóxeles, vegetación) deja una marca, y un `ErrorDeviceLost` en `waitIdle` imprime en qué marca iba la GPU («Ultimas marcas de la GPU»).
- **`ErrorDeviceLost` con el trazado de rayos apagado** (arreglado en la ronda R1, «Estabilidad» más abajo) (`cramion_stream_bench --rt 0`; Overthrow juega con `ray_tracing=0`). Fallaba: 3 de 3 en 4 ciclos con vegetación y a veces sin ella. No es la subida asíncrona (falla con todo síncrono y sin cola de transferencia), ni las especies, ni las sombras de los árboles, ni las sondas, ni las capas implícitas; las capas de validación y GPU-AV no dicen nada. Con las marcas, la GPU se para justo después de las cascadas del sol. Con un solo bloque de celdas no falla; con dos, sí. Siguiente paso: `VK_EXT_device_fault` (el driver lo tiene) o NVIDIA Aftermath.
- `Scene.cancelSpawn` borra lo ya creado con `destroyLater` (con los avisos de `Scene.destroy`), no con `World::destroy`.

### Estabilidad (ronda R1 del ROADMAP, 2026-10-20)

#### GPU
- **`ErrorDeviceLost` con el trazado de rayos apagado: arreglado.** La captura de la sonda de reflexión dibujaba las sombras y el G-buffer de la vegetación sin recortarla (`FoliagePass::recordCull`): usaba los comandos indirectos que quedaban en el buffer de ese hueco de frame, que justo después de adoptar instancias nuevas (`pollAsync`, antes de la sonda) era un buffer recién creado sin escribir y, tras cambiar las especies, llevaba los rangos de índices de las mallas viejas. `VK_EXT_device_fault`: «instrucción que falló» y «lectura no válida» durante la «captura de la sonda de reflexión» (la marca «Sombras: vegetacion» era la última de las cascadas de la propia sonda). Ahora la sonda recorta desde su cara (sin avanzar el viento ni tocar las estadísticas) y la vegetación no dibuja con comandos que no se recortaron con las instancias y mallas actuales (`draw_generation_`). `cramion_stream_bench --rt 0` (4 ciclos): 7 de 7 corridas sin fallo, más una sin vegetación y una con validación (0 errores); antes, 3 de 3 fallaban en el ciclo 2.
- **Informe de dispositivo perdido**: `VK_EXT_device_fault` siempre activo si la GPU lo tiene. `VulkanDevice::reportDeviceLost` junta las marcas NV (`CRAMION_GPU_CHECKPOINTS=1`), el fallo del driver (tipo y dirección), la GPU, el driver y la VRAM, y lo guarda una vez por proceso en `%LOCALAPPDATA%/Cramion/Crashes/DeviceLost-AAAAMMDD-HHMMSS.txt`. `drawFrame` lo captura, llama al aviso de `VulkanDevice::setDeviceLostHandler` (el editor autoguarda la escena ahí) y relanza; después no dibuja. El editor dice al salir dónde quedó el informe.
- **Esperas a la GPU con tope** (`VulkanDevice::waitFence`): 10 s por defecto, `CRAMION_GPU_TIMEOUT_MS` (0 = sin tope), aviso en el log a los 2 s. Un cuelgue se trata como dispositivo perdido, con su informe, en vez de dejar el programa colgado. La usan `submitOneTime`, `GpuTicket::wait`, la fence del frame y la de la sonda.
- **Etiquetas de `VK_EXT_debug_utils`** por pase (`markPass`) con la validación activa, para RenderDoc y Nsight.
- **Cola de retirada por fence** (`VulkanDevice::retireAfterGpu` + `keepAlive`): un envío vacío con fence cubre todo lo enviado antes a la cola de gráficos y `collectAsync` suelta los recursos. Sin `waitIdle` al cargar una textura de decal, al recompilar un `.crshader`, al borrar texturas de UI ni al crecer el culling en GPU o el vidrio; los sets de cada frame pasan a lo nuevo cuando le toca a ese frame, y la visibilidad nueva del culling se borra en la GPU (`fillBuffer`) en vez de con `submitOneTime`. Las Render Textures esperan solo a los frames en vuelo (`waitFramesInFlight`) y solo si algún material las usa.
- La captura de la sonda ya no deja sus recuentos en las estadísticas de la vegetación del frame de pantalla (la copia de lectura solo se hace en las vistas que cuentan).

#### Núcleo y assets
- **JobSystem sin cierres por excepciones**: una tarea que lanza ya no acaba en `std::terminate`. Se avisa en el log con su nombre (`schedule(..., "Nombre")`, `scheduleIo`), el contador del grupo baja siempre (guarda RAII: quien espera ya no se cuelga) y `wait` / `parallelFor` vuelven a lanzar la primera excepción en el hilo que espera (`parallelFor` espera antes a todos los trozos). Nuevos `JobCounter::failed()`, `JobStats::failed` y `jobs::setFailureReport`. Cada trabajador instala su manejador de `std::terminate`: informe con la tarea en curso (cerr y `%TEMP%/cramion_jobs_terminate.txt`) antes del de la aplicación.
- **FBX con piel y sin tomas**: se importa como pieza animada con su esqueleto (antes, estático partido por nodos); basta con dos huesos distintos. La lectura de comprobación ya no carga materiales, texturas incrustadas, luces ni cámaras.
- **FBX con una toma rota**: si `ValidateDataStructure` lo rechaza, se relee sin post-proceso y `repairAnimations` quita las pistas nulas, vacías o con valores no finitos y las tomas sin pistas válidas, y alarga la duración si las claves pasan de ella (un aviso por arreglo); después se aplican los mismos pasos, validación incluida. Antes se rechazaba el modelo entero. También en `loadAnimationClips`. (Falta ufbx como cargador.)
- **`create_model_materials`**: los nombres genéricos (`Material`, `Material.001`, `lambert1`, `standardSurface1`, `Default`, `DefaultMaterial`, `phong1`, `blinn1`, `No Name`, `pCube1`…) ya no emparejan texturas, y la coincidencia es por palabras completas («Gear» con «OrcGear_BaseColor»; antes bastaban 3 letras: «Ore» cogía «Forest»). Se busca en la carpeta del modelo, en las `Textures` de sus carpetas padre hasta `Assets` y junto al original; antes, en todo `Assets/`.
- **LOD del follaje**: los materiales con recorte por alfa (por la textura, la cabecera PNG/TGA/DDS, `base_color.w < 0.5` o el nombre) se simplifican sin `Prune` ni `simplifySloppy` y con los bordes de las cartas fijos. Antes las hojas y briznas desaparecían a media distancia (prueba: de 6000 a 187 triángulos; ahora 6000 en todos los niveles).

#### Scripts de C++
- **`onEnable()` / `onDisable()`**: desactivar el script o su objeto ya no destruye la instancia; deja de recibir `start`/`update`/`fixedUpdate`/`lateUpdate` y colisiones y conserva sus variables (pool de objetos). Reactivarlo llama a `onEnable` sin `awake` ni `start`. Solo destruir el objeto, quitar el componente o cambiar de clase llama a `onDisable` + `onDestroy`. Orden: `awake`, `onEnable`, `start`. `sync()` hace un `activeInHierarchy()` por script en vez de dos.
- **La API desde otro hilo** ya no corrompe el canal con el motor: no se envía, devuelve un valor vacío y avisa en la Consola desde el hilo del script, con el nombre de la llamada y cuántas veces se hizo. Lo comprueban el SDK y, como red de seguridad, `CramionScriptHost`.
- **Un viaje al proceso por fase** (`Op::CallMany`): todos los pasos de `fixedUpdate`, las colisiones, `start`+`update`+`lateUpdate` y el cierre al parar. El tiempo máximo sigue siendo por llamada (`Block::progress`) y un fallo se atribuye a su script; un proceso que no conoce `CallMany` recibe los eventos uno a uno. `script.cpp.BatchCalls=0` lo apaga; `Stats::requests` cuenta los viajes. Medido (300 scripts vacíos): 600 viajes y 0,75 ms por frame uno a uno, 1 viaje y 0,31 ms en lote. Falta saltar los virtuales no sobrescritos y juntar callbacks, mensajes y `Create`.

#### Editor y plataforma
- **Autoguardado de la escena** (Editar > Autoguardado, cada 2 min por defecto): la instantánea de deshacer se escribe en un hilo en `Library/Autosave/<escena>.crscene.autosave` (temporal + rename), y también al cerrarse por un error (SEH, `terminate`, `abort`, el aviso de dispositivo perdido y una excepción que salga del bucle). Al abrir la escena, si su autoguardado es más nuevo, el editor ofrece Recuperar o Descartar; Recuperar se deshace con Ctrl+Z. Preferencias en `%LOCALAPPDATA%/Cramion/Settings/EditorSafety.ini`.
- **Release con símbolos** (`CRAMION_RELEASE_PDB=ON` por defecto, Clang en modo MSVC): `-g -gcodeview` y `/DEBUG:FULL /OPT:REF /OPT:ICF` (mismas optimizaciones). `cramion_package` archiva los `.pdb` de cada versión en `build-release/symbols/<versión>/` (con `SIMBOLOS.txt` y los SHA-256 de los `.exe`) y en `Cramion-<versión>-win64-symbols.zip`, aparte del paquete. `-DCRAMION_RELEASE_PDB=OFF` lo apaga.
- **MCP con token de sesión**: el editor escribe un token aleatorio de 256 bits en `%LOCALAPPDATA%/Cramion/mcp-token-<puerto>` y lo exige (`Authorization: Bearer` o `X-Cramion-Token`, 403 si falta); con el token se rechaza toda petición con `Origin`, y siempre se exige `Host` local (DNS rebinding). `GET /health` no lo pide. `CramionMcp.exe` y la Terminal (IA) lo mandan solos; la casilla «Exigir token MCP» (ventana MCP, activada por defecto) lo apaga para clientes HTTP antiguos.

#### Cambios a tener en cuenta
- **MCP**: los clientes HTTP directos (`claude mcp add --transport http cramion http://127.0.0.1:7777/mcp`) reciben 403. Pasar al puente (`claude mcp add -s user cramion -- "D:\CramionEngine\CramionMcp.exe" --port 7777`), mandar el token o desmarcar «Exigir token MCP».
- **Scripts de C++**: desactivar y reactivar ya no repite `awake`/`start`; el reinicio de un objeto reutilizado va en `onEnable`. El bloque de memoria compartida cambia (`progress`): el motor y `CramionScriptHost` tienen que salir del mismo build.
- **JobSystem**: `wait()` vuelve a lanzar la excepción de una tarea del grupo (antes, `std::terminate` en el trabajador).
- **GPU**: una espera de más de 10 s (un horneado enviado en un solo bloque) se trata como dispositivo perdido; `CRAMION_GPU_TIMEOUT_MS` la sube.
- **Release**: con PDB, compilar usa más memoria y tiempo.

#### Integración y medidas
- Compila entero `build-release` (Windows y Android arm64) sin avisos nuevos. Arreglado en la integración: `VulkanDevice.h` no declaraba `VulkanDevice` antes de `GpuTicket` (Android no compilaba: el `friend` solo vale como declaración en MSVC); el editor registra `setDeviceLostHandler` (autoguardado antes de relanzar); dos avisos (`getenv` en `McpBridge.cpp`, `localtime` en el modal de recuperación) y las macros redefinidas de `PlayerMain.cpp`.
- Pruebas: 60 de 61 suites pasan; `CramionFeatures21Tests` sigue con los 2 fallos de lightmaps de antes (143/145). `CramionCppScriptTests` (75 comprobaciones, con las nuevas de `onEnable`/`onDisable`, otro hilo y lote) falló una vez en las dos de clang-format y pasó al repetirla. SDK regenerado con `cramion_sdkgen`: sin cambios.
- `cramion_stream_bench` (Ashbound, RTX 4060 Ti, 4 ciclos), frames calientes: `--rt 0` p95 13,1-14,0 ms, p99 20,2-23,5, máximo 1010-1149 ms; con RT p95 15,1, p99 21,9, máximo 1709 ms (reconstrucciones de la escena de rayos, pendiente de la escena incremental, más el primer árbol). Antes de la ronda, con el arreglo de la sonda: p95 13,2-14,5, p99 20,0-21,7, máximo 996-1077 ms. El máximo es el primer árbol: `FoliagePass::createTextures` genera las texturas en el hilo principal para calcular el hash aunque estén en la caché BC7 (pendiente: pasarlo a segundo plano).

### Editor rápido con escenas grandes (ronda R2 del ROADMAP, 2026-10-20)

#### Deshacer y prefabs
- **Deshacer por diferencias por entidad y componente** (`EditorHistory.cpp`, ROADMAP 2 #10): la instantánea se guarda una vez y por entidad (su registro JSON, el de las escenas), y cada paso lleva solo el antes y el después de las entidades que cambiaron y el orden de los hijos. Para saber qué cambió se calcula una huella de cada entidad desde sus componentes, sin crear JSON, por pools de EnTT y repartida entre los hilos del JobSystem; solo las que difieren se serializan. Así entran los cambios del Inspector, los gizmos, el MCP y las herramientas sin que cada sitio avise. Ctrl+Z aplica solo esas entidades y, dentro, solo los componentes que difieren; lo borrado vuelve con su UUID, sus hijos y su sitio. Antes cada acción serializaba el mundo entero y cada Ctrl+Z reconstruía todas las entidades. Hasta 500 pasos (eran 200) con el mismo tope de 256 MB, que ahora cuenta también la instantánea. `editor.undo.Parallel=0` lo deja todo en el hilo principal. Medido (`CramionHistoryTests`, 10 000 entidades): un paso con un cambio 0,22 ms y deshacerlo 0,21 ms.
- Lo que hace el MCP entra en su propio paso (`flushCommit` tras `pollMcp`). Zonas nuevas del perfilador: «MCP (pollMcp)», «Deshacer (flushCommit)», «Deshacer: huellas / registros / aplicar / instantanea» y el contador «Deshacer: entidades en el paso»; un paso de más de 16 ms se explica en la Consola. El origen flotante ya no es un paso: cada paso guarda su origen y recoloca sus raíces al aplicarse.
- **`recordPrefabOverrides` solo en las instancias tocadas** (ROADMAP 2 #11), con el `.crprefab` parseado en caché por ruta, fecha y tamaño (`ecs::parsePrefabText`, `ecs::ParsedPrefab`). Antes cada paso volvía a parsear cada prefab dos veces por instancia y serializaba todas las instancias.
- **Entrar en Play y cambiar a una pestaña de prefab ya no reserializan el mundo**: el texto se arma con los registros del historial (`ecs::assembleScene`). El autoguardado lo arma en otro hilo, como mucho unos 3 s por detrás del último paso. Parar Play y volver a una pestaña siguen rehaciendo el mundo desde el texto (falta el segundo `ecs::World`).
- API nueva del núcleo: `ecs::serializeEntityRecord`, `ecs::applyEntityRecord`, `ecs::assembleScene`, `ecs::SceneHeader` y `ComponentType::pool`.

#### Física, selección y Escena
- **Física de edición incremental** (`physics.IncrementalEditSync`, `physics.EditSyncChecksPerFrame`; ROADMAP 2 #12): en edición, la lista de colliders solo se rehace cuando cambia la estructura del mundo, y ya sin lo desactivado. Cada frame solo se miran los que se movieron (`Transform::version`), los avisados (el Inspector con `PhysicsSystem::touchEdit`; cada acción, deshacer y rehacer con `touchAllEdit`), los que esperan algo (terrenos, mallas sin cargar o generadas) y una tanda rotatoria de 256. Antes se ordenaban y firmaban todos los colliders en cada frame. Simulando, todo sigue igual.
- **Iconos y clic de la Escena** por las vistas de sus componentes y solo lo activo; antes recorrían el mundo entero en profundidad. El clic elige el icono más cercano (a menos de 15 px) y los seleccionados se dibujan encima.
- **Selección con conjunto** (`SelectionSet`, `EditorSelection.h`): `isSelected` en O(1) y `topLevelSelection` en O(n·profundidad); antes las dos eran búsquedas lineales, cuadráticas con muchos seleccionados. Ya no admite duplicados. Falta la caché de valores mezclados del Inspector.
- **Segundo plano configurable** (Editar > Segundo plano, `%LOCALAPPDATA%/Cramion/Settings/EditorBackground.ini`): Ahorro (la interfaz a 10 fps y la escena cada 2 s, como antes), Vista en vivo o Sin límite, de 5 a 60 fps. Minimizado, en otro escritorio virtual o tapado entero por otra ventana (el juego a pantalla completa) no dibuja la escena. Espera con temporizador de alta resolución: con `Sleep`, los 10 fps salían a 5-8. Cargando un proyecto o una escena ya no se limita.

#### Assets
- **Recarga en caliente de assets editados en disco** (`assets::FileWatcher`: `ReadDirectoryChangesW` o inotify en su hilo; 300 ms sin cambios, como mucho 2 s; ROADMAP 2 #9): modelos (`.crdata`), materiales, texturas de los `.crmat`, clips (`.cranim`), Animator Controllers, bases de Motion Matching, prefabs, `.crshader` y vídeos. Lo nuevo se lee en otro hilo y los modelos se vuelven a subir en los hilos de streaming (`VulkanRenderer::reuploadModelAsync`): mientras, se ve el de antes, sin `waitIdle`. La GPU deja de compartir la textura vieja de un archivo cambiado (`gfx::invalidateSharedTextureFile`). Antes solo se recargaban prefabs y `.crshader`. Falta: reimportar al cambiar el FBX de origen; imágenes de UI, sprites, decals, capas de `.crterrain` y texturas incrustadas en `.crdata`; vigilante en macOS.
- **Rescan incremental** (`AssetDatabase::scanPaths` / `applyScan`): solo se releen las rutas avisadas, en un hilo de disco, y se distingue nuevo, borrado, movido y modificado (por la fecha). Antes cada aviso recorría `Assets/` entero en el hilo principal. Si se pierden avisos, se recorre todo como antes.
- **Mover o renombrar en el panel Proyecto actualiza las referencias por ruta en todo el proyecto** (`assets::rewriteProjectReferences`; ROADMAP 2 #21): audio, imágenes de UI, scripts, vídeos, texturas de materiales y escenas, en todas las escenas, prefabs, materiales y demás JSON (en paralelo, temporal + rename), en la escena abierta (todos los componentes, por reflexión, como un paso de deshacer) y en las otras pestañas, con un aviso de cuántos archivos cambió. Antes solo se arreglaban los VideoPlayer de la escena abierta. La reescritura en disco no se puede deshacer.

#### World Partition
- **Regiones cargadas en el editor** (ROADMAP 2 #13, como las Loaded Regions de Unreal 5): con «Celdas en disco», el editor ya no carga la escena entera. Carga las celdas a un radio de su cámara (256 m por defecto, ajustable), las fijadas en Ventana > Regiones cargadas (cuadrícula con clic, recuadro, «Cargar selección», «Cargar todo», «Descargar todo») y las de lo seleccionado. Las lee en otros hilos y las recrea con `wp.RestoreBudgetMs`; lo que descarga se desactiva al momento y se borra por trozos con `wp.EditorDestroyBudgetMs` (1 ms), y guardar cada celda está acotado con `wp.EditorUnloadBudgetMs` (4 ms). Abrir la escena no lee ninguna celda.
- Guardar escribe también las celdas descargadas, incluidas las editadas que siguen en la caché de la sesión. Las regiones fijadas y el radio se guardan en `Library/WorldPartition/<escena>.regions`.
- Al dar Play, el World Partition del juego toma las celdas descargadas del editor sin leerlas. Al parar Play o volver de una pestaña, las celdas que la instantánea no tenía enteras vuelven de su archivo. Entrar y salir celdas no crea pasos de deshacer (el historial se pone al día sin paso). Construir HLOD carga antes todas las celdas.
- `World::releaseUuids` y `ecs::parsedRootUuid`. WorldPartitionTests: 96 comprobaciones (radio, fijadas, borrado por trozos, guardar lo descargado, Play que hereda y el mundo rehecho).

#### Cambios a tener en cuenta
- **Deshacer**: un componente que cambia al deshacer se quita y se vuelve a poner (el Transform se escribe en su sitio); las entidades que no cambian conservan su estado de ejecución (antes se rehacía todo). Las funciones `reflect` de los componentes se llaman desde varios hilos con un visitante que solo lee; si alguna tuviera estado mutable, `editor.undo.Parallel=0`.
- **Física en edición**: un collider o Rigidbody cambiado sin mover la entidad, sin Inspector y sin acción de deshacer se recoge en la tanda rotatoria (hasta n/256 frames: unos 3 s con 50 000 colliders a 60 fps). `physics.IncrementalEditSync=0` vuelve a lo de antes.
- **Clic en iconos superpuestos**: elige el más cercano, no el primero de la jerarquía.
- **World Partition**: hornear luz, navmesh y static batching solo ven las celdas cargadas (pulsar «Cargar todo» antes). Lo editado y luego descargado vive en la caché de la sesión hasta guardar; el autoguardado solo tiene lo cargado.
- **Recarga de modelos**: un modelo con otra jerarquía reinicia las animaciones de sus objetos; las formas de colisión de malla y la escena de rayos siguen con las viejas hasta Play o cambiar de escena.

#### Integración y medidas
- Compila entero `build-release` (Windows y Android arm64-v8a y armeabi-v7a) sin avisos nuevos. Arreglado en la integración: `commit()`, deshacer y rehacer no avisaban a la física incremental (el `touchAllEdit` se había perdido al juntar los cambios de `EditorApp.cpp`). Nueva suite `CramionHistoryTests` (94 comprobaciones, en un hilo y en paralelo: campos, componentes, borrar y recrear con hijos, orden, cambio de padre, texto armado, origen flotante y 10 000 entidades).
- Pruebas: 61 de 62 suites pasan; `CramionFeatures21Tests` sigue con los 2 fallos de lightmaps de antes (143/145). SDK regenerado con `cramion_sdkgen`: sin cambios.
- `cramion_stream_bench --rt 0` (Ashbound, RTX 4060 Ti, 4 ciclos), frames calientes: p95 14,3 ms, p99 21,2, máximo 999 ms (el primer árbol, como en R1: p95 13,1-14,0, p99 20,2-23,5, máximo 1010-1149). Sin cambios: lo de esta ronda es sobre todo del editor. Falta medir dentro del editor: la «Física» en reposo con Ashbound con `physics.IncrementalEditSync` a 0 y a 1, y el segundo plano con el juego delante.

### Render, VFX, prefabs, API de scripts y mando (ronda R3 del ROADMAP, 2026-10-20)

#### Luces y materiales
- **Luces locales sin tope (clústeres)** (`VulkanRendererLights.cpp`, `light_clusters.glsl`): las 32 puntuales y 8 focos más cercanos que se ven siguen como siempre en `GpuLights`; todas las demás que tocan la vista van por clústeres (16×9 teselas × 24 rebanadas de profundidad exponenciales) a `lighting.frag` y al vidrio, con su mapa de sombra si tienen hueco. Antes `RenderSync` se quedaba solo 32 y 8 (Ashbound, ~1300 antorchas: casi todo el mapa a oscuras). `RenderSync` ordena con la distancia calculada una vez por luz y solo corta en `kMaxSceneLights` (16 384). El reparto lo hace la CPU en dos pasadas sin asignar memoria por frame; el buffer es por frame en vuelo, crece al doble y no usa `waitIdle`. Hasta 255 luces por clúster (las más cercanas). Sin sombras por rayos, niebla volumétrica, agua ni VFX; los rayos no las ven. `VulkanRenderer::clusteredLights()`.
- **Transparentes que no son vidrio**: `.crmat` con `"blend": "alpha"` (iluminado, premultiplicado: escudos, hologramas) o `"additive"` (y `"glass"`, el de antes): luz del sol con su sombra, de las luces principales, de las de clúster y del entorno con su sonda. La emisión se suma en todos (también en el vidrio) y entra en el bloom. Los transparentes se dibujan de atrás adelante.
- **Sin luz (Unlit)**: `"unlit": true` en el `.crmat`: el color tal cual más su emisión (opaco: modelo 8 del G-buffer; transparente: `glass.frag`). No en el modo compatible, ni en `.crshader` ni en el Shader Graph.
- **Color de vértice**: COLOR_0 del FBX/glTF se importa (RGBA8 aparte, sin engordar el vértice de 80 B), se guarda en la caché (versión 13) y en el `.crdata` (bloque al final de la pieza; los antiguos se siguen leyendo: `CrData.cpp` lee cada pieza hasta su final) y llega a todos los caminos de vértices (normal, teselado, meshlets, static batching, UV2 de lightmap): `VERTEX_COLOR` en los `.crshader`, el nodo «Color de vértice» del Shader Graph (antes blanco) y `"vertex_color": true` en el `.crmat` (multiplica al color base). **Hay que reimportar los modelos.**
- **Parámetros por instancia**: `MeshRenderer` tiene `tint`, `alpha`, `emission_scale` y `receive_decals` (Inspector y `setField("MeshRenderer", ...)`), sin otro `.crmat` y sin romper el instancing ni el lote por material: van en los 12 bytes libres del registro por instancia. Opacidad < 1 en lo opaco: disolución por tramado (la suaviza TAA/FSR/DLSS; esos dibujos pasan a la pipeline con recorte). Lo que tiene parámetros propios no entra en el static batching. La opacidad no se aplica a las sombras.
- **Decals por teselas**: solo los que tocan la vista ocupan los 64 huecos y cada píxel del G-buffer prueba solo los de su tesela de pantalla (16×9). «Recibe decals» por objeto (el indicador de un área ya no pinta al héroe).

#### `.crshader`
- **Ruido sin facetas**: `gradientNoise`/`gradientNoiseD` (con derivadas, 2D y 3D), `simplexNoise`, `fbmNoise(p, octavas)`, `ridgedNoise`, `voronoi`, `curlNoise`, `random` y `pcgHash/2/3`, con hash de enteros (PCG). `noise()` y `fbm()` no cambian.
- **`vertex()` con sombra y vectores de movimiento**: la sombra (cascadas, luces locales, mapas de lluvia y cielo) sigue el desplazamiento con su propia pipeline (`surface_shadow.vert/.frag`), y el frame anterior se evalúa con su pose y `TIME - DELTA_TIME`: banderas y olas por `TIME` ya no dejan estelas con TAA/FSR/DLSS. Nuevo `DELTA_TIME`. Mientras haya sombras de `vertex()` en escena (2 s desde la última), las cascadas siguen su turno como con el viento de la vegetación.
- **`domain transparent`** (`surface_forward.frag`): se dibuja en la pasada del vidrio (unlit, premultiplicado: color = albedo × alpha + emisión) y lee la escena con `sceneColor`, `sceneDepth`, `sceneLinearDepth`, `sceneWorldPosition`, `sceneEmission`, `depthFade` y `SCREEN_UV`: distorsión de calor, ondas de choque, refracción. Usar esas funciones sin `domain transparent` da un error claro con su línea.

#### VFX Graph
- **Salida «Cinta»**: tira continua entre las vivas ordenadas por edad. Ancho y color en la vida, de cara a la cámara o al eje Y del emisor, textura estirada o por longitud (sin costura cada 1024 repeticiones) y corte por tiempo. Nacen interpoladas en la posición del emisor dentro del frame (también con «Caudal por distancia»). Plantilla «Estela (cinta)».
- **Salida «Malla 3D»**: un modelo por partícula (vertex pulling, hasta 16 mallas y 65 536 vértices), giro al azar, por velocidad o fijo, y escala. Sin esperas a la GPU al subir la malla. Van en forward, sin sombra, con las caras traseras fuera.
- **Eventos en la GPU**: «Al morir: otro efecto» y «Al chocar: otro efecto» (probabilidad, rapidez mínima, velocidad heredada) hacen nacer partículas de otro `.crvfx` en ese punto el frame siguiente (subemisor, 512 eventos por efecto y frame; un solo nivel).
- **`prepare()` sin reservas por frame**: ya no reserva ~260 KB por frame y vista. Saltos de más de 20 m sin estela; `play()` no cuenta el salto del pool.

#### Prefabs
- **Plantillas de prefab compiladas** (`ecs::PrefabTemplate`, `compilePrefabTemplate`, `instantiatePrefabTemplate`; ROADMAP 2 #16): registros con el índice del padre, componentes ya leídos en cintas binarias (sus valores en el orden en que los visita su reflect) y referencias internas como índices (UUID nuevos antes de crear: sin pasada de remapeo). Se compilan en los hilos de IO y las usan `Scene.instantiate` (pasa a `SceneStreamingApi.cpp`), `Scene.instantiateAsync` y el pool, con la caché `scene.PrefabCacheMB`. Mismo resultado que por JSON (comprobado con celdas de Ashbound de 140 y 913 entidades y ChaosProps, 4406); instanciar es 9-15 veces más rápido (913 entidades: 6,3 → 0,7 ms; ChaosProps: 51 → 3,8 ms, a -O1). Si una cinta no casa con su componente, ese componente se lee del JSON.
- **Pool de objetos**: `Scene.pool(ruta, n)` (copias desactivadas, por tandas con `scene.SpawnBudgetMs`), `Scene.acquire(ruta, pos, rot, padre)` (Transform del prefab, onEnable; con el pool vacío crea una), `Scene.release(e)` (onDisable, a la raíz y de vuelta al pool; si no es de un pool, se destruye como `Scene.destroy`) y `Scene.poolCount(ruta)`. Una copia destruida por fuera sale del pool.

#### Scripts de C++
- **API por índice y en binario** (ROADMAP 2 #18): cada clave se resuelve una vez por sesión (`Rpc::ApiResolve`) y las llamadas van con su id y los valores en binario (`ApiCall`, `NativeApi::bridgeBinary`), sin JSON. Los set de propiedades y las funciones sin resultado (sin «Devuelve» en su `api::Doc`) van en el lote (`ApiCallSend`). Lado del motor: `setQuaternion` de 3,97 µs a 0,21 µs. Si el motor no da ids (fuera de Play, el proceso que describe las clases), se usa el JSON. Los scripts recompilan sin cambios; `kAbiVersion` no cambia (solo se añaden ops).
- **En lote**: `Entity::setQuaternion(Quat)` y `Scene::setTransforms(entidades, posiciones, giros, escalas, local)` (op `SetTransforms`, en trozos de 16 384); `Scene.setTransforms` también en la API normal (Visual Scripts y consola).

#### Mando
- **Mando en Windows y Linux** (ROADMAP 2 #20): hasta 4 mandos con conexión y desconexión en caliente. Windows: XInput (DLL cargada al vuelo solo de System32; los huecos vacíos se miran cada ~1 s y al enchufar algo) y DualShock 4 / DualSense por raw input HID, USB y Bluetooth (`CRAMION_GAMEPAD_HID=0` lo apaga). Linux: evdev con inotify. Llegan los mismos eventos `Gamepad*` que en Android, con `Event::gamepadIndex` y `gamepadKind`. Sin foco se sueltan los botones y se para la vibración (`Window::setGamepadsInBackground`).
- **Vibración**: `dm::setGamepadVibration(pad, grave, agudo, segundos)` e `Input.setGamepadVibration(...)` (XInput, informes HID de Sony, `FF_RUMBLE` en Linux). `Input.vibrate(ms)` en PC hace vibrar los mandos.
- **`dm::Input` con varios mandos**: consultas por mando, `gamepadCount`, `gamepadKind` y `activeGamepad`. Sin índice, cualquier mando: botones de todos, ejes del último usado. Zona muerta radial configurable (`setGamepadDeadZones`; antes por eje, mismo 0,15).
- **Dispositivo activo** para los iconos: `Input::activeDevice()` / `activeDeviceChanged()`; en scripts `Input.getActiveDevice()` / `Input.activeDeviceChanged()`. `Input.getGamepadButton/Down/Up/Axis` e `Input.isGamepadConnected` aceptan el número de mando (1..4); nuevas `Input.getGamepadCount()` e `Input.getGamepadType(n)`.

#### Cambios a tener en cuenta
- **Modelos**: la caché de modelos pasa a la versión 13 (se rehace sola); para el color de vértice hay que reimportar. Las herramientas externas que lean el `.crdata` sin usar el tamaño de cada pieza tropiezan con el bloque nuevo del final.
- **Scripts de C++**: una función cuyo `api::Doc` no dice «Devuelve» pero devuelve algo iría en lote y el script recibiría nil (el motor avisa una vez en la Consola). Los errores de lo que va en lote salen en la consola del motor (`[C++] Api::send: …`). El motor y `CramionScriptHost` tienen que salir del mismo build.
- **Mando**: el constructor de `dm::Window` (Windows y Linux) ya no es inline. Con DS4Windows o Steam Input el mismo mando puede verse dos veces (XInput y HID): `CRAMION_GAMEPAD_HID=0` o HidHide. La zona muerta radial se nota algo distinta en los sticks.
- **Vidrio**: su set pasa de 10 a 12 bindings (10 y 11, los clústeres). **VFX Graph** en modo compatible pide 10 storage buffers por shader (antes 7).
- **Juegos exportados**: no llevan la sombra de `vertex()` hasta que `EditorExport.cpp` escriba `<archivo>.shadow.vert.spv` (el cargador ya la busca).

#### Integración y medidas
- Compila entero `build-release` (Windows y Android arm64-v8a y armeabi-v7a) sin avisos nuevos. Arreglado en la integración: `VulkanRenderer::updateSurfaceShader` convertía un `vk::Pipeline` a `bool` (en armeabi-v7a los handles no dispatchables son enteros: no compilaba); las ranuras del puente binario (`NativeApi::slots_`) pasan a `std::deque`, para que una llamada anidada que resuelve otra clave no deje colgando la referencia de la que se está atendiendo; un comentario con «dFdx» en `surface_common.glsl` rompía la comprobación de `CramionShaderTests` (los vértices sin derivadas); `InputApiTests` cuenta las 5 funciones nuevas del mando; un aviso de función sin usar en `CoreApiTests.cpp`. `WindowX11.cpp` no se ha compilado entero (no hay cabeceras de X11 en Windows): compilarlo en Linux.
- Pruebas: 61 de 62 suites pasan; `CramionFeatures21Tests` sigue con los 2 fallos de lightmaps de antes (143/145). `CramionApiCoreApiTests` 128 (plantillas, pool, puente binario), `CramionCppScriptTests` 75 con el SDK nuevo. SDK regenerado con `cramion_sdkgen`: entran las funciones nuevas del mando (`Input.getGamepadCount/Type`, `setGamepadVibration`, `getActiveDevice`, `activeDeviceChanged` y el número de mando opcional); lo añadido a mano para el pool coincide con lo generado.
- `cramion_stream_bench --rt 0` (Ashbound, RTX 4060 Ti, 20 ciclos), frames calientes: p95 12,6 ms, p99 18,1, máximo 1159 ms (el primer árbol en el ciclo 2, como en R1 y R2: p95 14,3, p99 21,2, máximo 999). No empeora. Sin probar en GPU: una escena con luces por clúster, transparentes nuevos, cintas, mallas y eventos de VFX; tampoco el mando con hardware real.

### Ultra optimización (ronda R4 del ROADMAP, 2026-10-20)
- **Texturas de los árboles en otro hilo** (`FoliagePass::buildTextures` con `runStreamingTask`): generarlas cuesta ~1 s de CPU aunque estén en la caché de BC7 (su clave es el hash de lo generado), y se hacían en el hilo principal con los primeros árboles: el frame de 1000-2300 ms que quedaba en todas las rondas («el primer árbol»). Ahora se generan y suben en un hilo de streaming (la copia espera a su propio envío, no a la GPU entera); las primeras instancias esperan en `pollAsync` y entran en cuanto están (unos cientos de ms sin árboles la primera vez, en vez de parar el frame). Si fallan, no se dibujan árboles y no se reintenta cada frame.
- **Escena de rayos sin reconstrucciones en cadena**: un modelo que se dibuja sin estar en la escena se apunta en el acto y la siguiente reconstrucción le hace BLAS aunque para entonces ya no se dibuje. Antes solo entraban los que se dibujaban en ese momento: con celdas que salen antes de los 4 s de espera, cada reconstrucción se quedaba sin ellos y pedía otra (banco con rayos: 7 seguidas, 100-260 ms cada una).
- **TLAS con la capacidad ya usada** (`RayTracing::setInstances`): al rehacer la escena empezaba otra vez en 256 instancias y crecía x1,5 en los frames siguientes, con un `waitIdle` en cada paso (4-6 seguidos tras cada reconstrucción en Ashbound). Ahora se crea una vez con la mayor que hizo falta.
- **Sonda de reflexión sin parar la CPU**: la captura de cada cara tiene su propio hueco de frame (`kMaxFramesInFlight` 3: dos para la pantalla y `kProbeFrameSlot` para la sonda) con sus buffers, command buffer y fence; antes usaba el del frame y lo esperaba (~3 ms de GPU más ~4 ms esperando al frame anterior en cada cara). Antes de grabar copia al hueco de la sonda los clústeres, huesos y registros de lightmaps del frame; el cubo nuevo se cierra en `finishProbeCapture` en cuanto acaba su fence (unos 2 frames después). `TerrainPass::prepareView` escribe ahora los parámetros del terreno de su hueco (`writeParams`).
- `cramion_stream_bench --vsync`: medir con vsync, como se juega.
- **Desglose de los tirones**: `FrameTimings` separa dentro de los actores el streaming de la GPU, el culling, la escena de rayos y la TLAS, y aparte lo subido en segundo plano. `cramion_stream_bench` los imprime en cada frame lento y al final cuenta los frames de más de 16,6 / 33 ms por la parte mayor de cada uno.

#### Integración y medidas
- La ronda se cortó a mitad de la primera vuelta (se cerró la sesión que la llevaba); se terminó, revisó y midió aparte. Compila entero `build-release` (Windows y Android arm64-v8a y armeabi-v7a) sin avisos nuevos. Pruebas: 60 de 61 suites pasan; `CramionFeatures21Tests` sigue con los 2 fallos de lightmaps de antes (densidad por carta y un rebote más).
- `cramion_stream_bench` (Ashbound, RTX 4060 Ti, 20 ciclos), frames calientes, antes → después de la ronda:

| | p95 | p99 | máximo | > 33 ms | > 100 ms |
|---|---|---|---|---|---|
| `--rt 0` | 12,5 → 13,0 | 17,2 → 17,5 | 1099 → 29,7 | 1 → 0 | 1 → 0 |
| `--rt 0 --async` | 12,9 → 12,7 | 14,9 → 17,4 | 1047 → 20,1 | 1 → 0 | 1 → 0 |
| `--rt 1` | 14,1 → 9,1 | 17,8 → 11,3 | 1200 → 181 | 12 → 2 | 11 → 1 |
| `--rt 1 --async` | 14,8 → 9,0 | 17,4 → 10,4 | 2332 → 171 | 5 → 3 | 2 → 1 |

- Con la sonda en su hueco (medido después, mismo banco), frames calientes `--rt 0`: p95 13,0 → 9,3, p99 17,5 → 12,4, frames > 16,7 ms ~170 → 8 y ninguno por la sonda; con `--vsync`: p99 17,6 → 13,2, > 16,7 ms 224 → 4. El máximo (30-38 ms) es de un frame suelto de instanciar o RenderSync. Validación de Vulkan sin errores nuevos. Tras este cambio: compila entero `build-release` (con Android) y pasan 61 de 62 suites (`CramionFeatures21Tests`, los 2 de lightmaps de siempre).
- **Queda** (en `ROADMAP.md`): con rayos, 2 reconstrucciones de la escena de 80-180 ms la primera vez que entra cada modelo (falta la escena de rayos incremental).

### Lo que estaba a medias (ronda R5 del ROADMAP, 2026-10-20)
#### Trazado de rayos
- **Escena de rayos incremental**: cada modelo que entra por streaming se añade solo (su BLAS en el command buffer del frame, filas reservadas en la escena y buffers que crecen con una copia en la GPU), sin rehacer la escena entera. La TLAS se sustituye sin `waitIdle` y lo viejo se retira cuando la GPU acaba. Queda la reconstrucción entera para editar un modelo que ya está en la escena, cambios de terreno o de especies de árboles, y al pasar de ~1,5 veces los modelos iniciales.
- Sin `waitIdle` en la caché de sombras, la liberación de la escena de rayos en reposo y las Render Textures; la textura de un decal se decodifica en un hilo de streaming (unos frames en blanco hasta que llega).
#### LOD
- Modelos rígidos simplificados con `meshopt_simplifyWithAttributes` (UV y normal); `CRAMION_RIGID_LOD=old` vuelve al de antes.
- **LOD por modelo** en los ajustes de importación: Apagado / Automático / A mano. **LODs del archivo**: `Roca_LOD0`, `Roca_LOD1`… se importan como niveles (solo el LOD0 en la jerarquía).
- **Fundido con tramado** de 0,35 s al cambiar de nivel en los modelos rígidos (`CRAMION_LOD_FADE=0` lo quita). Sin fundido aún en personajes, sombras, mesh shaders y modo compatible.
#### Transparencias y Shader Graph
- Niebla en todo lo transparente (vidrio, `.crmat` alpha/aditivo, `.crshader` transparentes) y normal map en el vidrio (reflejo, brillo del sol y refracción).
- `domain transparent` de los `.crshader` **iluminado** (sol con sombra, luces principales y de clúster, sonda); `domain transparent unlit` es el de antes y `domain unlit` es opaco sin luz.
- **Shader Graph**: dominio Opaco / Sin luz / Transparente / Transparente sin luz y categoría «Escena» (UV de pantalla, color y profundidad de escena, posición y fundido por profundidad). En modo compatible los `.crshader` transparentes y sin luz caen al shader estándar manteniendo el dominio.
- **Partículas de distorsión**: mezcla «Distorsión» en el ParticleSystem (buffer RG16F que el composite usa para desplazar la escena).
#### Parámetros por instancia
- `INSTANCE_DATA(0..1)` en los `.crshader` (dos `vec4` por objeto sin romper el instancing; el registro por instancia pasa de 32 a 64 bytes) y `entity().setMaterialParam()` (tinte, opacidad, emisión, recibir decals y los dos `vec4`).
- Tinte y opacidad por entidad en las partículas; la opacidad se respeta en las sombras (tramado, y sin sombra a 0).
#### VFX y decals
- Humo de VFX iluminado de verdad: sol con sombra, luces principales y de clúster (en modo compatible solo las principales).
- **Decals**: atlas de texturas (array con mips que crece sin parar el frame), hasta 256 (antes 64) en un buffer propio, y canales normal, ORM y emisión, con sus botones en el Inspector.
#### Scripts de C++
- No se envían los eventos que la clase no implementa (`script.cpp.SkipEmptyEvents`): 300 scripts con solo `update` pasan de 600 a 300 viajes por frame.
- **Juego exportado (Windows)**: los scripts se cargan en el propio proceso, sin `CramionScriptHost` (`script.cpp.InProcess`, vuelve solo al proceso aparte si falla). Sin aislamiento: un fallo de memoria en un script cierra el juego.
#### Editor
- **Reimportar al cambiar el original** (`editor.assets.AutoReimport`, menú del panel Proyecto): un FBX reexportado se reimporta al volver al editor, o al momento si está dentro de Assets. No durante Play.
- Caché de valores mezclados del Inspector con muchos objetos seleccionados.
- Parar Play restaurando solo lo que cambió (`editor.play.IncrementalStop`, **apagado** por defecto).
#### Depuración de GPU
- Nombres de los recursos de Vulkan (buffers, imágenes, targets, G-buffer, command buffers, pipelines de compute, shaders) con la validación activa; `CRAMION_VK_SYNC_VALIDATION=1` y `CRAMION_VK_GPU_AV=1`.
#### Cambios a tener en cuenta
- Los juegos exportados con `.crshader` hay que volver a exportarlos (cambió el registro por instancia).
- `domain transparent` ahora se ilumina; para el comportamiento de antes, `domain transparent unlit`.

### Editor sin tirones, logos y pantalla de carga
- **Tirón de ~200 ms cada 2 s en el editor** (también con la escena vacía): mirar si cambiaron los scripts de C++ recorría Assets/ entero hasta tres veces en el hilo principal (peor en OneDrive). Ahora va en un hilo de disco.
- **Logos sin fondo negro**: los PNG del logo y del banner «Game powered by Cramion» eran opacos con fondo negro; ahora son transparentes (sin halo), y el `.ico` se regeneró en sus 7 tamaños.
- **Pantalla de carga de un proyecto**: recortaba trozos del banner viejo con coordenadas fijas (letras rotas sobre cajas negras); ahora dibuja el logo y «CRAMION» como texto.

### VFX Graph completo (petición de Keryan)
- **Cintas (Ribbon) de verdad**: bloque Initialize «Atributos de cinta» (id, tiras, ancho, giro, separación) para varias tiras sin unirse en un mismo emisor; suavizado Catmull-Rom / Hermite con 1-16 subdivisiones (fijas o adaptativas), orientación Cámara, Emisor, Vector propio o Lado propio con giro, UV Estirar, Distancia (no resbala) y Edad, con escala, desplazamiento y celda del atlas. Al destruir el objeto la estela ya no desaparece de golpe: se apaga sola. Plantilla nueva «Estela de proyectil».
- **Luz por partícula** (bloque Output «Luz por partícula»): las partículas vivas iluminan el mundo con las luces por clústeres (sin sombra), con radio, intensidad, caída física o lineal, máximo por emisor y prioridad; selección estable con fundidos de 0,1 s. Tope global `render.vfx.MaxParticleLights` (32) y por calidad (Baja 8, Media 16, Alta/Ultra 32). No llegan a la sonda ni al trazado de rayos.
- **`.crshader` transparente**: salida nueva `s.refraction` (y el pin «Refracción» del Shader Graph) para vidrio y agua que refractan sin fantasmas de lo que está delante. Arreglado el error **«sin renderizador»**: un `.crshader` compilado antes de que exista el renderizador se instala en el primer frame en vez de quedarse roto. La exportación a Android empaqueta también la sombra de `vertex()` (`.shadow.vert.spv`).
- **Flipbook con fundido** («Fundir cuadros»: sin saltos entre cuadros) y velocidad del flipbook al azar por partícula.
- **Eventos de la GPU con canales**: bloques Update «Emitir al morir», «Emitir al chocar» y «Emitir cada t s» escriben {tipo, posición, velocidad, color, valor} en un canal con nombre, y el bloque Spawn «Al recibir un evento de la GPU» hace nacer partículas ahí, sin pasar por la CPU (8 canales, 2048 eventos por canal y frame). Los scripts escriben en el mismo canal con `Effects.pushEvent(canal, tipo, posición, velocidad, color, valor)`; MCP `vfx_push_event`.
- **Partículas de malla**: hasta 3 modelos más por efecto (variantes con el bloque «Índice de malla» o LOD por distancia), un quad a partir de cierta distancia, orientación «Hacia la cámara» y «Fija» con rotación, escala al azar por eje, bloque Update «Velocidad de giro» (con rodar) y material `.crmat` (color, textura, emisión, opaco/alfa/aditivo, sin luz).
- **Distorsión**: mezcla «Distorsión» en quads, cintas y mallas (px a 1080p, mapa de normales o de alturas, lente blanda sin textura) sobre el mismo buffer que el ParticleSystem. `render.vfx.Distortion` y apagada en calidad Baja; sin coste si no se usa.
- **Escalabilidad por tipo de efecto**: cada `.crvfx` dice si es Héroe, Combate, Impacto o Ambiente y la calidad y la plataforma deciden distancia máxima, cuántos a la vez, cuántas partículas y qué salidas caras se apagan; al pasarse, no crear, quitar los lejanos o pausarlos. «Sin tipo» (lo de antes) no tiene límites. Pool de instancias reutilizadas, ráfagas y precalentado repartidos en varios frames, caja fija con recorte por la vista, orden de dibujo (`sort_order`) y estadísticas por tipo (editor y MCP `vfx_control stats`, `play_at`, `time_scale`). CVars `vfx.scalability`, `vfx.quality`, `vfx.platform`, `vfx.timeScale`, `vfx.prewarmFrames`.
- **Pequeñas mejoras**: ruido curl horneado en un volumen 32³ (una lectura por octava en vez de ~150 hashes; `vfx.curlNoiseBaked`), hasta 64 texturas por pase (antes 16, según el dispositivo) y escala de tiempo de los efectos.
- Arreglado: «Posición en malla» no sacaba ningún punto en el editor (leía la malla vacía del AssetManager).
#### Cambios a tener en cuenta
- El precalentado ahora se reparte en 6-16 frames (`vfx.prewarmFrames 1` para el de antes) y la turbulencia usa el ruido horneado, que se ve algo distinto (`vfx.curlNoiseBaked false`).
- Los efectos a los que se les ponga tipo se recortan por distancia y número según la calidad; los `.crvfx` de antes no tienen tipo y no cambian.
- Las partículas de malla no proyectan sombra y las opacas no escriben profundidad (se ordenan de atrás adelante).

## 2.5.1

**Novedad sobre la 2.4.13.** Memoria de la GPU con VMA y subidas por lotes en una cola de transferencia; mundos estáticos sin trabajo repetido por frame; trazado de rayos con una BLAS por geometría y tiling por material sin duplicar vértices; gama baja (GI horneada sin SVGF, sol horneado sin cascadas, ríos y ondas más baratos, distancia de sombras y LOD por gama); lightmaps UV2 (fases 0-2 del plan); vídeo en Android y FFmpeg opcional; World Partition con varias rejillas y Data Layers; red en coordenadas absolutas; Blueprints fase 2; y los parches de Keryan (39 de 44: render, mallas, texturas BC7, UI y ventana).

### Nueva imagen
- Nuevo logo (editor, iconos de los `.exe`, web y manual) y nuevo banner «Game powered by Cramion» de los juegos exportados.

### Memoria y subidas
- **VMA 3.3.0** (`CramionFX/vendor/vma`): `VulkanBuffer` y `VulkanTexture` ya no hacen una asignación de Vulkan por recurso; anillo de staging persistente de 64 MB (16 MB en modo compatible).
- **Un lote por modelo**: un command buffer y una fence; cola de transferencia dedicada si la GPU la tiene (`CRAMION_VK_NO_TRANSFER_QUEUE=1` la apaga); el modelo se instala cuando su fence acaba, sin bloquear.
- **Hilos**: los modelos van por un grupo propio de 3 hilos (`CRAMION_STREAM_THREADS`), los clips y los vóxeles por el `JobSystem`; fuera `std::async`.
- **Logs**: las líneas por modelo subido solo salen con `CRAMION_LOG_LEVEL=debug` o `CRAMION_VK_VERBOSE=1`.

### Mundos grandes y sombras
- **Identidad estable de los actores**: un objeto que entra o sale ya no redibuja las 4 cascadas ni hace copiar el `Animator` a todos los de detrás; los vectores de movimiento de los huesos también van por identidad.
- **Caché por entidad** del modelo resuelto y su variante, esfera por versión del Transform, sin `Animator` para lo que no se anima y cajas reutilizadas si nada cambia.
- **Modelo base de una variante**: no se sube ni se queda en la GPU si nadie lo dibuja tal cual.
- **Sombras**: rejilla de actores para las luces locales, proyectores fuera de la vista descartados (barrido hacia el sol), recorte por clúster en las cascadas, cada cascada solo con sus trozos de terreno y el terreno solo las redibuja si cambian sus trozos o alturas.

### Trazado de rayos y tiling
- **Una BLAS por geometría**: las variantes `.crmat` comparten triángulos y BLAS; cada textura entra una vez; los modelos sin instancias se saltan.
- **Tiling por material**: escala y desplazamiento de UV en un uniform (set 1, binding 9) en vez de reescribir los vértices; la variante comparte vértices e índices con su modelo.

### Gama baja
- **GI horneada sin SVGF** cuando no hay sondas dinámicas, y la palanca Gi a 0 mientras está horneada.
- **Sol horneado sin cascadas** si no las lee nadie (volumétrica, agua, fluidos o vidrio).
- **Ríos, vegetación, ondas y flotación**: la línea del río en caché, la lista de agua compartida y rehecha solo si cambia, ondas sin trabajo si no hay agua cerca (el océano por altura).
- **Distancia de sombras y LOD por gama** (escritorio con presupuesto adaptativo): 60 / 80 / 100 m y LOD ×2 en Bajo, ×1,5 en Medio. Bajo y Medio ya no arrancan en Lod 1.
- **Agua**: SSR de 12 pasos con la iluminación ligera; nueva opción `lighting_quality` (auto, full, lite).
- **VR y presupuesto adaptativo**: el sesgo de mips ya no depende de la resolución que cambia el presupuesto, así que no hay tirón al cambiar de escala.

### Lightmaps UV2 (fases 0-2 de `PLAN-LIGHTMAPS.md`)
- UV2 automáticas con xatlas («Generar UV de lightmap», vista LM de la Escena, MCP `generate_lightmap_uvs`), lightmapper propio (luz directa, rebote con su filtro y luces Horneadas), `.crbake` v4, atlas en el render (binding 10 del set 1), lote estático al exportar y el player. Fases 3 y 4 a medias.

### Render
- **Vidrio** recortado con la pirámide Hi-Z; **luces puntuales por teselas** de pantalla.
- **Early-Z**: los opacos sin alfa van por una pipeline del G-buffer sin `discard`.
- **Sin trabajo apagado**: sin bloom ni auto-exposición si están apagados, océano sin FFT si no se ve, FSR 1 con cadena LDR corta, sin FXAA encima de TAA, FSR 3 o DLSS, y la copia de vista solo si alguien la pide.
- **Otros**: el G-buffer dibuja el escenario antes que el terreno; parallax con capas limitadas por píxel; LOD de personajes con esqueleto (con atributos y tope de 0,5 px); orden de triángulos para la caché de vértices.
- **Fallos**: el recorte de sombras usa el alfa del color base; las partículas con mezcla alfa del VFX Graph se ordenan; la corteza de la lava ya no pasa a las otras lavas; menos líneas oscuras finas en las aristas de las paredes.

### Texturas
- **BC7** para las imágenes de la UI (si el PSNR ≥ 40 dB), los árboles y las capas del terreno, con caché en `%LOCALAPPDATA%/Cramion/Cache`.

### UI, física y ventana
- **UI**: el clic que cae sobre la UI ya no llega al juego (`UI.pointerOverUi`); `UIEventTrigger`, `Entity:uiScreenRect`, `UIInputField` con cursor, selección y portapapeles, y `UIImage` con `uv_rect` y 9-slice.
- **Física para scripts**: `layerMask`, `sphereCast` y `overlapSphere`. Los MeshCollider grandes se construyen en segundo plano.
- **Ventana**: tamaño exacto y modo «sin bordes» (`borderless`, `window_size`, `monitor_*`, `windowed_max_*`), tope de FPS (`fps_limit`, y el presupuesto adaptativo apunta al menor de `target_fps` y el tope) y Immediate sin vsync cuando no hay Mailbox. El cursor vuelve a su sitio al soltarlo.
- **Editor**: en segundo plano no dibuja la escena; el Bone Socket ya no encoge bajo un padre escalado; la navegación sin Nav Mesh Bounds no escanea las fuentes.

### Vídeo
- **Android** con MediaCodec (archivos y `https://` en Android 9+).
- **FFmpeg opcional** (`CRAMION_WITH_FFMPEG`, apagado por defecto): RTSP, RTMP y DASH; HLS en Android y Linux.

### World Partition y red
- **World Partition**: varias rejillas, Data Layers (también desde scripts: `WorldPartition.setDataLayer`) y dependencias entre celdas, con su UI en el editor. Las escenas y celdas antiguas se siguen leyendo.
- **Red a más de 2 km**: las posiciones viajan absolutas (sector de 1024 m + resto); protocolo versionado a 2.

### Blueprints fase 2
- Construction Script, funciones, macros, Timeline y depuración en vivo (cables, valores y «Vigilar este valor»). Renombrar o mover un Blueprint lleva su `.crgraph` y actualiza las referencias.

### Reproductor
- `<Juego>.exe --run-tests [--junit x.xml]` ejecuta las pruebas `CRAMION_TEST` del juego exportado (solo Windows; código de salida 0-3).

### Cambios a tener en cuenta
- **Red**: el protocolo 2 no es compatible con juegos ni `CramionServer` anteriores (se desconecta con el motivo).
- **UI**: los scripts ya no reciben el clic que cae sobre la UI.
- **Sin vsync** en GPUs sin Mailbox, el juego va sin tope y con tearing salvo que se ponga `fps_limit`.
- **Gama Baja y Media**: las sombras acaban antes (60 y 80 m).
- **Formatos**: `.crgraph` versión 2 si usa algo nuevo; celdas de World Partition v3; `.crbake` v4 con lightmaps.

### Pruebas
- Las 60 suites de pruebas pasan, con las nuevas: `testSharedVector`, `testVariantSharesMesh`, WaterTests (muestreo con y sin caché), MeshTests (gama y LOD, BC7), 19 de lightmaps en Features21Tests, WorldPartitionTests, NetworkTests, Features24ApiTests, VisualScriptRuntimeTests, BlueprintTests, VideoTests y los arreglos de CramionTemplateTests («bloques» y «coches»).
- Arreglado: los argumentos de clang++ de los scripts de C++ en Android mezclaban `/` y `\` en la ruta de `ScriptMain.cpp`.

## 2.4.13

**Novedad sobre la 2.4.11: la Cramion Store (cuentas, assets gratis y un Package Manager en el editor, como el de Unity, con versiones para actualizar o volver atrás, sobre el formato propio `.crwpack`), y el horneado de luz vuelve a ver los objetos Static de los modelos importados.** Con el editor abierto, el renderizador se queda con la malla de cada pieza que dibuja (para no tenerla dos veces en memoria) y en el AssetManager queda una copia vacía; el horneado leía esa copia, así que una ciudad entera marcada Static daba «Nada que hornear… Marca los objetos como Static». Ahora toma la malla de la escena del renderizador: en una casa de 101 piezas Static se pasa de 0 a 2.877.697 triángulos. El horneado también aguanta ciudades enteras (volumen automático que cabe en el tope de sondas, lightmap que crece en vez de fallar, sin cierres por falta de memoria), y las demás herramientas que leían la misma copia vacía (Fracturar, Blueprints, materiales del modelo, reimportar, VFX y colisiones ocultas) vuelven a funcionar.

### Cramion Store: assets gratis con cuenta propia
- **Web** (`docs/store/`, enlazada desde la portada y el manual): catálogo con búsqueda, categorías, orden y paginación; ficha de cada paquete con sus versiones, cambios, dependencias y descarga; registro e inicio de sesión; panel para crear paquetes, subir versiones (arrastrando el `.crwpack`), poner portada, retirar o borrar versiones y cambiar el perfil y la contraseña. Pensada también para el móvil.
- **Administración** (`store/admin.html`): resumen (usuarios, paquetes, versiones, descargas, espacio, disco libre), usuarios (roles user/admin/superadmin, suspender, borrar), paquetes (destacar, ocultar, borrar) y registro de actividad. En el teléfono las tablas pasan a tarjetas.
- **API** (`web-api/cramion_api.py`): solo la biblioteca estándar de Python (SQLite, scrypt, `http.server`); sirve la API en `/api` y la web, así que sustituye al `python -m http.server`. Contraseñas con scrypt, sesiones de 30 días, límite de intentos por IP, subidas por trozos de 8 MiB (Cloudflare corta los cuerpos de más de 100 MB), SHA-256 de cada versión, descargas con `Range` (se reanudan), 2 GB por paquete y 10 GB gratis por cuenta. `test_api.py` la recorre entera; el despliegue está en `web-api/README.md`.
- **Package Manager del editor** (Ventana › Package Manager), como el de Unity: «En el proyecto» y «Cramion Store», búsqueda y categorías, versión a elegir con Instalar / Actualizar a / Bajar a / Reinstalar / Quitar, y las dependencias del manifiesto se instalan solas. Los paquetes van a `Assets/Packages/<nombre>/`, lo instalado queda en `ProjectSettings/Packages.json` y cada versión descargada se guarda (con su contenido extraído) en `Library/PackageCache/`, así que volver a una versión ya usada no descarga nada. Se comprueba el SHA-256 de cada descarga. «+ …» instala un `.crwpack` del disco y abre la exportación; «Servidor de paquetes…» cambia la dirección de la API (para probar uno propio). Los modelos sin importar que traiga un paquete (`.fbx`, `.obj`…) se importan al instalarlo; mientras tanto otra instalación espera y quitar no se deja (si no, el modelo de la versión anterior acababa en la carpeta de la nueva).
- **Exportar un paquete**: Archivo › Exportar carpeta como paquete .crwpack… (identificador, versión, nombre, autor, descripción, dependencias, cambios y compresión rápida/normal/máxima). Lleva los modelos ya importados (`.crdata`, con su UUID), así que las escenas no pierden la referencia al cambiar de versión.
- **MCP**: herramientas `packages` (con `show`, abre la ventana), `package_versions`, `package_install`, `package_remove`, `package_state`, `package_registry` y `package_export`, para que una IA busque, instale, actualice, baje de versión, quite y exporte paquetes con las mismas funciones que la ventana.

### Formato .crwpack y herramienta crwpack
- Contenedor propio: cabecera de 64 bytes, manifiesto JSON sin comprimir (el servidor y la web lo leen sin descomprimir), el contenido de todos los archivos en un solo flujo cortado en bloques de 4 MiB comprimidos cada uno con zstd (o guardados tal cual si no ganan) con su CRC32, e índice con su propio CRC. Comprimir y extraer usan todos los núcleos; los archivos pequeños comparten bloque.
- Rutas con tildes y ñ, archivos vacíos; al extraer se rechazan rutas con `..`, absolutas o con `:`; un bloque dañado se detecta por su CRC.
- `crwpack.exe pack|unpack|list|info|verify` (con `--level fast|normal|max`, `--block-mb`, `-j`), ahora en el zip junto al editor.

### Horneado de luz: los modelos Static no entraban
- **Por qué pasaba**: la primera vez que se dibuja una pieza de un modelo (o una primitiva: cubo, esfera…), `RenderSync` la mueve del AssetManager a la escena del renderizador y el AssetManager se queda con un `ModelData` vacío. El horneado leía esa pieza: 0 triángulos sin ningún motivo en el informe, y el aviso pedía marcar Static aunque ya lo estuviera todo. Pasaba desde que existe el horneado (2.1.0) con cualquier modelo visible; las pruebas no lo veían porque no pasaban por el renderizador.
- **De dónde sale ahora la malla**, por orden: de la escena del renderizador (`RenderSync::partModelData`), de la pieza del AssetManager si aún la tiene y, si no, se relee del disco solo la geometría (una vez por modelo, solo las piezas que faltan y saltándose los bytes de las texturas; se suelta al terminar). No se guarda ninguna copia de las mallas.
- **Informe**: cada malla entra o sale con su motivo. Motivos nuevos: «sin geometría» (no se pudo leer la malla de su modelo, con el consejo de reimportarlo), «transparentes» y «con una malla vacía». El consejo de marcar Static solo sale cuando así entrarían más mallas.
- **Vidrio y agua**: los materiales transparentes ya no tapan la luz, como en las sombras en tiempo real. En la casa de prueba, 114.517 triángulos de cristal dejaban el interior sin sol ni cielo.

### Horneado de ciudades enteras
- **Volumen automático** (si la escena no tiene Light Probe Volume): cubre las mallas y el terreno si caben en 400 × 60 × 400 m; si no, va alrededor de las mallas sin salirse de la escena. De alto empieza en el suelo (antes una torre de más de 60 m lo despegaba). Si con la *Separación automática* pasaría de 200.000 sondas, las separa más (antes fallaba con «demasiadas sondas») y el informe dice el tamaño y la separación.
- **Sondas contadas antes de reservar memoria**: una caja enorme o una separación mínima se rechazan al momento con «demasiadas sondas» (antes podían pedir gigas y cerrar el editor).
- **Sin memoria ya no cierra el editor**: el horneado falla con un mensaje que dice qué bajar.
- **Lightmap de superficie**: los texeles se buscan antes que las sondas, así que si no caben se sabe al momento y no tras minutos de horneado. Si el lightmap se encendió solo (luces horneadas) y no cabe, el texel crece hasta 4 m; con el texel puesto a mano, el error nombra el ajuste. La búsqueda tiene barra de progreso y se puede cancelar.
- **Luz horneada fuera de los volúmenes**: en modo horneado esas luces no se dibujan en tiempo real, y las superficies fuera de los Light Probe Volume se quedaban sin su luz (en la casa de prueba, el tejado y lo alto de las paredes). Ahora el lightmap también cubre lo que alcanza una luz horneada (con el sol horneado, toda la escena), si con ello no pasa de un millón de texeles; si no cabe, se queda como antes y el informe lo dice.
- **Más de 16 volúmenes**: el informe avisa de que los más grandes no entran.
- **Fuera de Play**: en Play no se hornea (el World Partition descarga las celdas lejanas y lo que se mueve no está en su sitio); `bake_lighting` del MCP devuelve el motivo.
- **Terreno por teselas**: una sola reserva para las mallas y todas las teselas (antes cada tesela copiaba todos los triángulos ya juntados).

### Otras herramientas que leían la malla vacía
- **Fracturar** (Destrucción): «No salió ningún trozo» con cualquier objeto visible, primitivas incluidas.
- **Blueprints**: el esqueleto y los huesos de un modelo ya dibujado no aparecían; crear un Blueprint desde un modelo de la escena lo hace con todas sus mallas.
- **Materiales del modelo**: asignar los `.crmat` por pieza (ModelMaterials) a un modelo que ya está en la escena no asignaba ninguno.
- **Reimportar**: los `.crmat` asignados a cada hueco acababan todos en uno.
- **VFX «Position Mesh»**: las partículas salían del origen del efecto en vez de la malla (también en el juego exportado).
- **Trozos de la destrucción**: vuelven a salir con el color, el metálico y la rugosidad del material del modelo.
- **Colisiones y navegación**: un MeshCollider oculto, o en el lote estático del juego exportado, cuya pieza se dibuja en otro objeto se quedaba sin forma. La física y la navegación (editor y juego) piden ahora la malla con `RenderSync::collisionModelData`.

### Cambios a tener en cuenta
- **Vuelve a hornear** las escenas con modelos importados: el `.crbake` anterior se hizo sin ellos (solo terreno y mallas de código).
- **Vidrio**: los interiores detrás de cristaleras salen más claros, porque el vidrio ya no proyecta sombra en el horneado.
- **Luz horneada fuera de los volúmenes**: con luces horneadas el `.crbake` lleva más texeles y el horneado tarda algo más.
- **Navegación en el juego exportado**: los MeshCollider de los objetos del lote estático (Static, activo por defecto al exportar) entran en la malla de navegación que se construye en el juego, como ya pasaba en el editor y en la física.
- **Cramion Store**: el Package Manager usa por defecto `https://cramion.mantraxtools.store/api`; para probar un servidor propio, «+ …» › «Servidor de paquetes…» (http solo a localhost).

### Pruebas
- `Features21Tests`: la pieza de una primitiva y la de un `.crdata` real movidas al renderizador entran desde su escena; sin renderizador se regeneran o se releen del disco una vez, sin los bytes de las texturas y solo las piezas pedidas; con el archivo borrado sale «sin geometría» y no pide Static; el cristal no tapa la luz; malla vacía; Static sin nada útil; volumen automático de una ciudad (tope de sondas, empieza en el suelo, con terreno de 100 m y de 2 km); caja enorme rechazada antes de reservar memoria; sol horneado fuera de los volúmenes; texel automático que crece y texel a mano que falla con su nombre.
- Comprobado con un proyecto real (una casa de 101 piezas y una ciudad de 1.287 piezas a escala ×8) con el `RenderSync` del editor: 2.877.697 triángulos de la casa y 415.317 de la ciudad, iguales a los de sus `.crdata`.
- En el editor, por MCP: la misma escena de la casa hornea 97 mallas y 2.877.697 triángulos en 2,4 s, y en Play se niega. El Package Manager contra una Store local: catálogo, búsqueda y categorías, instalar con su dependencia, bajar a 1.0.0, subir a 1.1.0, volver a 1.2.0 desde la caché sin descargar, versión inexistente, exportar una carpeta e instalarla desde el archivo, quitar, y cambiar de versión mientras se importa un modelo de 2 millones de triángulos.
- `web-api/test_api.py`: registro, sesión, roles, subida por trozos, versiones, descargas con `Range`, retirar y administración.
- `crwpack`: ida y vuelta idéntica (rutas UTF-8, archivos vacíos) y bloques dañados detectados.

## 2.4.11

**Corrección sobre la 2.4.10: al salir de una habitación y volver a entrar ya no se ve más oscura hasta que, en un momento cualquiera, la luz vuelve de golpe; los reflejos de pantalla (SSR) están reescritos; y el trazado de rayos se enciende de verdad en el juego exportado.** La sonda de reflexión que sigue a la cámara (de ella salen los reflejos de respaldo y la luz rebotada de lo que no está en pantalla) se quedaba con la captura de fuera hasta que el jugador se paraba lejos. Ahora sabe qué ve cada cubo, recaptura en cuanto la cámara se va a otra habitación, vuelve al cubo de la habitación sin capturar al regresar, y todos los cambios son fundidos de 0,5 s. La exposición automática ya no oscurece los interiores de día con el ajuste de noche de la 2.4.10. El SSR traza sobre una pirámide de profundidad (como FidelityFX SSSR), lee el mip que corresponde a la rugosidad y ya no usa historial temporal.

### Habitación más oscura al volver a entrar
- **Por qué pasaba**: la sonda de reflexión (un cubo que sigue a la cámara, más el anterior para fundir) solo se recapturaba al pararse a más de 4 m o tras 15 m. Al salir y volver andando, la habitación se iluminaba con la captura de fuera (los reflejos de respaldo y los rayos de la GI de pantalla que salen de ella), y al pararse el jugador cambiaba todo de golpe.
- **Qué ve cada cubo**: al terminar una captura se leen en la CPU sus distancias (32×32 por cara, `ReflectionProbe::distanceToward`). Si la cámara se va a donde el cubo no ve, se recaptura enseguida, sin esperar a que se pare. Se mira una ventana de 5×5 direcciones, así que una persona o un poste delante no cuentan.
- **Vuelta al cubo anterior sin capturar**: el otro cubo se conserva junto con la luz con que se capturó. Si la cámara vuelve a su lado (lo ve y está más de 1 m más cerca, o el actual ya no la ve) y la luz no ha cambiado, se vuelve a él con un fundido, también al entrar por la misma puerta. Para volver a un cubo hace falta que vea la cámara sin margen y con una ventana más estrecha, y no se vuelve atrás junto al sitio donde se cambió: así no va y viene en el canto de una puerta.
- **Pararse de verdad**: la recaptura al pararse pide 0,3 s despacio (al darse la vuelta ya no captura fuera). Hay una recaptura nueva si la cámara se queda quieta entre dos capturas, por ejemplo justo dentro de la puerta.
- **Fundidos**: 0,5 s (antes 0,35 s). La GI de pantalla funde los dos cubos igual que la iluminación (antes cambiaba de golpe), y no hay un rato de solo cielo entre el cubo viejo y el nuevo.
- **Otros**: los cubos se vacían al cambiar de escena (la nueva ya no se ilumina con los de la anterior). Una captura durante la que cambia la luz se repite una vez y no se vuelve a ella. Con `render.probe.MinSeconds` puesta a mano, también esta recaptura respeta ese tiempo.

### Exposición automática en interiores de día
- El ajuste de noche de la 2.4.10 (por encima de +0,75 EV solo se recorre un 25 % de lo que falta) también se aplicaba de día, así que los interiores quedaban más oscuros. Ahora depende de la luz del día: de día se compensa del todo, como en la 2.4.9; de noche, como en la 2.4.10; al atardecer, a medias.

### Reflejos de pantalla (SSR) reescritos
- **Trazado sobre una pirámide de profundidad mínima** (Hi-Z, `ssr_hiz.comp`), como FidelityFX SSSR, de hasta 96 pasos: sin rayas ni huecos en ángulos rasantes.
- **Grosor**: 3 cm + 0,5 % de la distancia, con una prueba de plano que solo puede alejar la superficie. Los objetos finos ya no se ensanchan y no salen aciertos falsos debajo de las mesas.
- **Rugosidad**: la imagen del frame anterior tiene mips, y cada reflejo lee el mip que cubre su cono (GGX). Lo muy rugoso o muy lejano pasa a la sonda de forma suave, igual a cualquier resolución.
- **Sin historial temporal**: un filtro espacial de 3×3 según la profundidad y la normal, solo en lo rugoso (los espejos pasan tal cual). Ya no deja estelas al mover la cámara.
- **Origen del rayo**: sale de la normal geométrica (reconstruida de la profundidad) y se dobla en ángulos rasantes. Los reflejos que miran hacia la cámara se apagan de forma suave.
- **Oclusión especular**: ya no oscurece el SSR, solo el respaldo (sonda o cielo).
- **Imagen anterior**: es solo de la vista principal (con su vista-proyección con jitter). Las capturas de la sonda y la otra vista del editor ya no la pisan.

### Trazado de rayos en el juego exportado
- **No se encendía nunca**: la escena de rayos se construía vacía en la pantalla de carga y no se volvía a hacer. Ahora se programa al instalar cada modelo, y el Player la construye al terminar la carga.
- **Caché de radiancia**: ya no se vacía al rehacer la escena de rayos (sí al cambiar de escena) y envejece por tiempo (6 s), no por frames.
- **Reflejos por rayos**: el filtro temporal reproyecta con la distancia del impacto (el punto reflejado, no la superficie), así que los reflejos ya no se arrastran al mover la cámara.
- **Objetos sin sombra** (`cast_shadows` apagado) ya no proyectan sombra por rayos.

### Otros arreglos
- **Luz volumétrica**: un punto fuera de la cascada de sombras elegida contaba como iluminado, y el sol se colaba unos frames en los interiores al girar la cámara. Ahora usa la cascada siguiente, como la iluminación.
- **Reflejos del agua**: detrás de un árbol o un poste salían estirados y a bandas. Ahora el choque se afina con una búsqueda binaria y solo cuenta si el rayo queda pegado a la superficie.
- **Editor con Escena y Juego a la vez**: el sol y el ambiente se teñían y escalaban dos veces por frame (con intensidad 55, el sol ×55×55). Cada sincronización parte ahora del sol físico (`Scene::physicalSun`).
- **Cielo (IBL)**: las barreras del cubo del cielo incluyen los shaders de cómputo y de rayos que lo leen (antes solo los de la iluminación).

### Cambios a tener en cuenta
- **Trazado de rayos**: con `ray_tracing=1` en una GPU con rayos, el juego exportado usa ahora reflejos y GI por rayos (antes se quedaba sin querer en SSR y GI de pantalla). Para ver el SSR, apaga el trazado de rayos.
- **Exposición**: los interiores de día se ven más claros que en la 2.4.10, como en la 2.4.9.
- **Coste**: el SSR añade la pirámide de profundidad y una copia con mips de la imagen anterior (solo con SSR y sin rayos). La sonda recaptura más al pasar de una habitación a otra; cada captura sigue repartida en una cara cada pocos frames.

### Pruebas
- `CoreTests`: exposición en un interior de día (de día se compensa del todo, de noche se aplica el ajuste de la 2.4.10 y al atardecer queda a medias).

## 2.4.10

**Corrección sobre la 2.4.9: el cambio de escena ya no deja terrenos desplazados ni restos de la escena anterior, el preset «Ultra realista» deja de verse lavado, la VR se ve nítida y con la luz correcta, la exposición automática funciona de noche y el horneado de luz vuelve a funcionar, ahora con luz directa y sombras.** Todos los cambios de escena (abrir, nueva, `Scene.load` en Play, Stop, pestañas de mundo, el juego y el servidor dedicado) pasan por una única secuencia que vacía el terreno, la física, la navegación, el World Partition y la GPU y descarta los trabajos en otros hilos de la escena anterior. «Ultra realista» y «Realista» usan el nuevo tonemapper *AgX Punchy* con aire limpio. En VR cada ojo se dibuja por defecto a la resolución que pide el casco (salvo el tope de VRAM por debajo de 12 GB), con TAA por ojo. De noche la exposición ya no se queda clavada en el máximo y oscurece poco a poco al mirar una luz. El aviso «La escena no tiene mallas estáticas» ya no sale con modelos animados, Static en el padre o terreno sin collider, y cada luz puede hornear su luz directa y sus sombras suaves. (Se llama 2.4.10 y no 2.4.9.1 para que el actualizador, que compara tres números, la ofrezca a quien tiene la 2.4.9.)

### Cambio de escena: terrenos desplazados y restos de la anterior
- **Terreno desplazado tras cambiar de escena**: las teselas del terreno de la escena anterior sobrevivían a la carga y el origen del mundo se ponía a 0 (y luego al del archivo) sin moverlas, así que quedaban desplazadas justo lo que se había movido el origen flotante (por ejemplo 3072 m tras alejarse en Play y pulsar Stop) hasta el siguiente `update` del streamer. Ahora las teselas que se quedan (Stop, deshacer) se recolocan con origen viejo − origen nuevo en doble precisión (`World::clearPersistent(new_origin)`, `World::rebaseOrigin`).
- **Restos de la escena anterior**: ningún cambio de escena vaciaba el streamer de terreno, así que las teselas viejas entraban en la GPU, la física y la malla de navegación. En el juego y el servidor dedicado las teselas viejas seguían ahí cuando se creaban la física y la navegación de la escena nueva, y con `runtime_generation` apagado el terreno del nivel anterior quedaba horneado en la malla de navegación del nuevo.
- **Secuencia única `resetSceneState`** (`ecs/SceneReset.h`): sube la generación de escena (`World::sceneGeneration()`), cierra el World Partition sin restaurar, para la física, vacía la navegación, quita todas las teselas (también las huérfanas), vacía el almacén de terrenos si se pide, saca de la GPU los terrenos y la vegetación (`RenderSync::clearSceneStreaming`) y vacía el mundo con el origen a 0. La usan `openScene`, `newScene`, `Scene.load` en Play, Stop después de un `Scene.load`, el cambio de pestaña de mundo, el Player y el servidor dedicado.
- **Red de seguridad por generación**: `loadScene` vacía el mundo entero y sube la generación; el streamer de terreno, `RenderSync`, la navegación y el World Partition detectan la generación nueva y se vacían solos, y la física descarta los colliders de terreno pendientes de otra escena. Los trabajos en otros hilos (teselas, vegetación, baldosas de navegación, celdas) guardan la generación al lanzarse y tiran su resultado si ya no coincide.
- **Misma rejilla con otra carpeta o tamaño** (por ejemplo tras *Guardar como*, que conserva los UUID): `loadNow` dejaba las teselas viejas vivas, estiradas al tamaño nuevo y con los datos de la carpeta vieja. Ahora suelta las teselas con los ajustes viejos, recoloca las que existen, y `syncGrid` corrige la ruta de datos de cada tesela.
- **Rejillas que ya no existen** se quitan antes de cargar las vivas, también en `loadNow`.
- **`TerrainStore::get`** devuelve los datos que se están guardando en otro hilo en vez de leer un archivo viejo o a medio escribir.
- **Vegetación**: se reinicia el estado de la siembra asíncrona y una siembra terminada de otra escena no se sube.
- **Navegación**: el contador de trabajos en curso perdía trabajos en cada cambio de escena a mitad de construcción; tras varios cambios la malla dejaba de construirse para siempre. Las baldosas llevan una época y los resultados viejos se descartan.
- **Nubes desplazadas tras cargar**: `alignOriginAfterLoad` fija el origen absoluto del renderizador después de desplazarlo (antes acababa en 2·nuevo − viejo).
- **Carpeta compartida entre rejillas**: `createTerrainGridEntity` ya no reutiliza una carpeta que use otra rejilla de la escena y la crea en disco al momento.

### Preset «Ultra realista» lavado
- **Nuevo tonemapper *AgX Punchy*** (índice 4): AgX con un look Punchy suave (potencia 1,25 y saturación 1,2, más suave que el de Blender, que recortaba los verdes) y exposición igualada a ACES (×0,9). *AgX* (índice 3) se dibuja exactamente igual que antes, así que las escenas guardadas con AgX no cambian.
- **Por qué se veía lavado**: AgX base sin look ni compensación de exposición con una auto-exposición pensada para ACES, más un polvo volumétrico denso. Según una simulación en Python de las curvas del shader (no medido en el motor), en sRGB de 8 bits el gris 0,18 pasa de 128 a 101 (ACES: 91) y el gris 0,02 de 37 a 17 (ACES: 11).
- **Aire limpio en el preset**: polvo volumétrico de 0,006 a 0,0008 /m (visibilidad estimada de unos 0,65 km a unos 4,9 km) y niebla de 0,0008 a 0,0002 (velo estimado a 300 m de un 21 % a un 4 %). Los rayos de sol entre los árboles se siguen viendo (anisotropía 0,7).
- **Resto del preset**: contraste 1,1, saturación 1,05, viveza 0,18, bloom 0,03 con umbral 1,0, aberración cromática 0,02, sin distorsión de lente, apertura f/5,6 y nitidez 0,3.
- **«Realista»** también usa *AgX Punchy*, con polvo 0,001, niebla 0,0003 y nitidez 0,25.
- **Grano que levantaba los negros**: se sumaba en lineal y se recortaba en 0, así que en el negro solo sobrevivía la mitad positiva (con `film_grain` 0,03, hasta unos +10/255). Ahora se suma después de la curva sRGB, simétrico y con amplitud 0 en el negro.
- **Nuevo ajuste *Nitidez*** (`sharpen`, 0 a 1): enfoque adaptativo al contraste (tipo AMD CAS) tras el FXAA, en el slot del push constant que estaba libre. Vale 0 por defecto, así que las escenas existentes se ven igual.

### Realidad virtual: resolución baja y luz rara
- **Presupuesto adaptativo forzado en VR**: el casco encendía siempre el presupuesto adaptativo, que podía bajar cada ojo al 50 %. Ahora la VR sigue el ajuste *adaptativo* del usuario (con el refresco del casco como objetivo) y, mientras el casco está activo, la escala de cada ojo no baja de 0,87: el presupuesto baja antes la GI, la oclusión ambiental, los reflejos, la luz volumétrica, las sombras y los LOD. El tope se quita al salir de VR (`FrameBudget::setLevelCap`; el «x/max» del editor y del MCP muestra ese tope).
- **Gama baja empezaba al 67 %**: cada sesión de VR en una GPU de gama baja arrancaba con los ojos al 67 %; ahora arranca en 0,87.
- **Los presets de escritorio encogían los ojos**: nuevo ajuste **«Resolución VR»** (`vr_render_scale`, 0,5 a 1, 1 por defecto) en el panel *Gráficos*, en `Graphics.ini` y en el MCP. Mientras el casco está activo sustituye al preset de calidad de escritorio (en móvil el preset sigue poniendo el máximo).
- **Tope de VRAM**: los píxeles por ojo crecen con la memoria de vídeo (`vrVramScaleCap`): en un ojo de Quest 3, 0,87 con 8 GB, 0,675 con 4 GB (como antes) y sin tope desde 12 GB.
- **Ojos sin antialiasing y paso silencioso a FSR 1**: en VR, con el escalado apagado, FSR 3 o DLSS, cada ojo usa ahora TAA a resolución nativa con el enfoque RCAS. FSR 1 solo se usa si se elige. En escritorio no cambia nada.
- **Sesgo de mips al escalar**: al dibujar por debajo de la resolución de salida las texturas reciben un sesgo de log2(interna / salida), entre −1 y 0 (la mitad con FSR 1, para que no parpadeen). También afecta al escritorio con un escalador por debajo del 100 %.
- **Sonda de reflexión congelada en VR**: ya no se volvía a capturar con el casco, así que se quedaban los reflejos de antes de entrar (por ejemplo los de día por la noche). Ahora se captura en el primer ojo, mide la distancia desde la cabeza y cada cara ajusta sus sombras a su propia cámara.
- **Efectos de pantalla en VR**: destello de lente, viñeta y rayos de sol en espacio de pantalla se apagan con el casco (no casan entre los dos ojos). La luz volumétrica sigue, porque se traza en el mundo.
- **GI dinámica dos veces por frame**: las sondas de GI dinámica se actualizan una vez por frame de VR, no una por ojo; los dos ojos leen los mismos datos y el coste ya no se duplica.
- **Registro**: al cambiar los ajustes de VR la consola muestra una línea `[VR] Ojo ...` con la resolución de cada ojo, la escala y si usa TAA o FSR 1.

### Exposición automática de noche
- **Noche clavada en `max_ev`**: hasta +0,75 EV la exposición compensa del todo, como antes; por encima solo recorre un 25 % de lo que falta. Con luz de luna (en histogramas de prueba) queda en unos +1,59 EV en vez de quedarse en el tope.
- **Luces que oscurecen poco a poco**: una zona iluminada por una farola oscurece la imagen de forma gradual según crece (antes no cambiaba nada hasta que ocupaba un tercio de la pantalla y luego caía de golpe). Una bombilla brillante a la vista baja la exposición, pero nunca por debajo del nivel de día.
- **Medición**: la ventana del histograma pasa a 35–96 % (antes 50–92 %) y el peso del centro de la pantalla a 1–2× (antes 1–3×), así que una luz que pasa del borde al centro bombea menos.
- **Estabilidad**: zona muerta de 0,05 EV, tope de 4 EV por segundo (un cambio grande tarda alrededor de 1,5 s) y una protección que reinicia el estado si se vuelve NaN o infinito.
- **Cortes de cámara**: `invalidateHistory()` (cambio de vista, corte de cinemática, carga de escena) salta directo a la exposición nueva en vez de fundir. Los desplazamientos del origen flotante y los cambios de tamaño no lo hacen.
- **VR**: se miden los dos ojos y los dos usan siempre la misma exposición.
- **Editor**: con Escena y Juego visibles a la vez, la vista oculta ya no avanza el reloj del frame, así que la adaptación, las nubes y el tiempo van a velocidad real, como en el Player.
- **Valores por defecto**: `min_ev`/`max_ev` pasan a −4/+2 (antes −2/+1,4); «Ultra realista» usa `min_ev` −5 (`max_ev` sigue en 2,5).

### Horneado de luz: «La escena no tiene mallas estáticas» y luz directa horneada
- **Por qué decía que no había mallas estáticas**: el aviso salía siempre que el horneado reunía cero triángulos, y cuatro filtros lo tiraban todo: los modelos con alguna animación (la mayoría de FBX y GLB) se descartaban enteros; Static en el padre no contaba para los hijos (marcar la raíz de un modelo o prefab no horneaba nada); un solo objeto Static en la escena, aunque estuviera desactivado, excluía todas las demás mallas; y el terreno solo entraba si ya tenía su collider de física.
- **Arreglado**: Static se hereda de cualquier padre; los modelos animados se hornean en su pose de reposo (un Animator que reproduce solo excluye mallas que no sean Static); la regla de «solo Static» cuenta solo mallas que se pueden hornear y, si no hay ninguna marcada, se hornea todo lo que no se mueve; un Rigidbody dinámico solo excluye si el objeto no es Static (Static gana, con aviso); el terreno y las teselas de `TerrainGrid` se leen de su mapa de alturas, sin collider (hasta unos 400 000 triángulos); se usan los colores de los materiales sobrescritos (`.crmat`); y las luces se leen de la escena, no de la lista del renderizador, que solo guarda las más cercanas a la cámara.
- **Informe en español**: cada horneado explica qué entró y por qué no entró el resto. El aviso «Nada que hornear» muestra su primera línea; el informe completo sale en la ventana de Iluminación y en `lighting_state` del MCP (`gather_report` y `last_error`, que antes volvía vacío).
- **Modo de horneado por luz**: cada Light tiene **«Modo (horneado)»**: *Mixta* (por defecto: luz y sombras en tiempo real y rebote horneado, como antes), *Horneada* (luz directa, sombras suaves y rebote horneados; en modo Horneada la luz deja de dibujarse en tiempo real y no tiene mapa de sombras) y *Solo tiempo real* (el horneado la ignora). El sol toma el modo de la Directional Light, o de un nuevo ajuste *Sol* en la ventana de Iluminación si no hay ninguna.
- **Luz directa y sombras horneadas**: se guardan en las sondas y en el lightmap de superficie en espacio de mundo; si hay alguna luz horneada y ese lightmap está apagado, se enciende solo a 0,5 m. La penumbra sale del radio de cada luz. Si el sol está horneado, `lighting.frag` deja de sumar el sol en tiempo real, pero solo mientras la hora del día no lo mueva más de 2°; después vuelve a tiempo real y la ventana avisa de que hay que volver a hornear.
- **Formato**: `.crbake` versión 3 (los de la versión 2 se siguen leyendo). En tiempo real, o sin nada horneado, se ve exactamente igual que antes.
- **MCP**: `bake_lighting` acepta `sun_mode`, `surface_texel` y `direct_samples`.
- **Límites**: no hay lightmaps con UV2, así que la nitidez de las sombras horneadas es la de la celda del lightmap de superficie (0,5 m por defecto; 0,25 m da sombras más finas, más lento y con más memoria). Los objetos que se mueven reciben la luz horneada por las sondas pero no proyectan sombra de ella, y un sol horneado no da sombra a los objetos dinámicos (deja el sol en *Mixta* en exteriores). Si el sol se mueve más de 2° hay que volver a hornear. Las pasadas transparentes y *forward* siguen sumando el sol horneado. Las texturas solo oscurecen el color con un factor fijo (0,6).

### Cambios incompatibles y migración
- **Presets ya aplicados**: una escena guarda los valores, no el preset. Las escenas que ya usaban «Ultra realista» o «Realista» tienen que **volver a aplicar el preset** para verse con el aspecto nuevo.
- **Grano**: en las escenas con `film_grain` mayor que 0, los negros dejan de levantarse (cambio visible a propósito).
- **Abrir una escena con el JSON roto**: el mundo ya se ha vaciado, así que el editor queda con una escena nueva sin ruta (para que Guardar no pise la anterior). Un archivo que no existe deja la escena abierta como estaba.
- **`openScene` y `newScene` durante Play** salen de Play primero.
- **Teselas esculpidas sin guardar**: al cambiar de escena en el editor ya no se guardan solas; se descartan si se eligió no guardar. En Play y al cambiar de pestaña se quedan en memoria.
- **VR sin presupuesto por defecto**: con *adaptativo* apagado la VR va sin presupuesto, así que una GPU floja puede no llegar al refresco del casco. En VR los ojos usan siempre TAA, aunque el escalado esté apagado. Cuando el presupuesto cambia la escala, los samplers también se rehacen (espera a la GPU).
- **Exposición**: las escenas que guardaron `max_ev` 1,4 conservan ese tope, así que de noche siguen en ×2,64 (pero las luces oscurecen mucho antes). Los interiores oscuros de día que llegaban a +0,75 a +1,4 EV quedan hasta unos 0,5 EV más oscuros (+1,4 pasa a unos +0,91).
- **Horneado**: las escenas que antes horneaban siguen horneando igual (las luces empiezan en *Mixta*), pero ahora entran también las mallas animadas, los hijos de un padre Static y el terreno, así que el resultado puede cambiar al volver a hornear. Si cambias el modo de una luz después de hornear, vuelve a hornear.

### Pruebas
- Cambio de escena: nuevo ejecutable `CramionSceneChangeTests` (`SceneChangeTests.cpp`): recolocación de teselas al recargar con otro origen y al parar Play, `loadScene` sin raíces transitorias, cambio de A a B con cargas a medias y origen desplazado (10 km), `loadNow` sin pasar por `update`, misma rejilla con otra carpeta y tamaño, física sin el suelo de A, World Partition con lecturas de A en marcha y navegación con 12 cambios de escena a mitad de construcción.
- Post-proceso: `CoreTests` comprueba que «Ultra realista» usa *AgX Punchy*, polvo ≤ 0,001, niebla ≤ 0,0005 y nitidez > 0.
- VR, exposición y horneado: `CoreTests` (`testAutoExposureMetering`: día sin cambios, noche que no se clava en el tope, farola que oscurece de forma suave y en un solo sentido, bombilla que baja sin pasar del nivel de día, histograma vacío, NaN/infinito, reinicio, zona muerta y tope de EV por segundo), `MeshTests` (`testVrResolution`) y `Features21Tests` (`testBakeGather`: cubo Static, hijo de una raíz Static, terreno Static desde su mapa de alturas, reglas del Rigidbody, horneado vacío con su motivo y modos de luz; `testBakedDirect`: suelo al sol, suelo bajo un tejado en sombra y lectura del `.crbake` v3). Resultados: Core 76 comprobaciones (incluye la exposición), Mesh 111 (resolución VR) y Features21 62 (horneado); todas sin fallos.
- Resultados: SceneChangeTests 63 comprobaciones, Core 76, Terreno 126, World Partition 37, Física 128, Navegación 44, Prefabs 21, Blueprints 56, Visual Scripting 61, Shaders 17, Scripting 26 y scripts de C++ 66; todas sin fallos.

### Nuevo: reproductor de vídeo (VideoPlayer)
- **Componente `VideoPlayer`** (*Reproductor de video*, categoría Video), como el de Unity: fuente Clip (archivo de Assets: `.mp4 .m4v .mov .webm .mkv .avi .wmv`) o URL (https, o http solo a este PC; HLS `.m3u8` en Windows); reproducir al empezar, esperar al primer frame, bucle, saltar frames, velocidad 0,1-4, empezar en / fin (bucle de una sección) y reloj de audio, de juego o sin escalar.
- **Salidas de imagen**: Material Override (emisión, color o ambas, por hueco, con intensidad; también en modelos sin `.crmat` propio), Render Texture (`.crrt`: cualquier material o UI Image que la use), plano lejano o cercano de la vista del juego con opacidad, y solo para scripts. Modos de proporción Stretch, FitInside, FitOutside, FitHorizontally, FitVertically y NoScaling.
- **Audio por el motor**: Direct (3D desde el objeto), AudioSource (cada pista por una o varias Audio Source con sus efectos, oclusión y reverberación; el clip propio de la fuente se calla mientras tanto), para scripts o ninguno. Varias pistas con volumen y silencio por pista.
- **Editor**: arrastrar un vídeo a la Escena crea una *Pantalla de vídeo* (plano con la proporción del vídeo, emisión y sonido 3D, un solo deshacer); a la Jerarquía añade o reutiliza el VideoPlayer; a un campo del Inspector lo asigna; a un hueco de textura de un material crea una Render Texture de vídeo y su VideoPlayer. Pegar una URL `https://` en la Escena crea una pantalla con esa URL. Nuevo filtro *Vídeo* en el navegador, miniatura con la duración, datos al pasar el ratón, doble clic = ventana de vista previa y clic derecho *Volver a leer*. Mover un vídeo de carpeta actualiza los VideoPlayer de la escena abierta.
- **Importación al abrir el proyecto** (nunca al abrir una escena): los vídeos sin cache o cambiados se leen en 1-2 hilos y se guardan en `Library/Cache/Video/<hash>.json|.png`; la barra de estado muestra «Leyendo vídeos n/m».
- **Inspector del VideoPlayer**: vista previa sin Play (audio en 2D) y la imagen del juego en Play, reproducir, pausa, parar, frame a frame y barra de tiempo, *Probar URL* (en otro hilo, con el motivo exacto si no se puede), tabla de pistas con volumen, silencio, fuentes y *+ Crear fuente hija*, y estadísticas (hardware o software, códec, frames perdidos, búfer).
- **Ciclo de vida**: el vídeo arranca después del audio y se para siempre antes (suelta sus voces de stream) al parar Play, con `Scene.load`, al cambiar de escena (`SceneResetTargets::video`), al cerrar el proyecto y al cerrar el juego. Pausar el editor congela imagen y sonido; al dar Play se para la vista previa.
- **Exportar**: `PackInput::level` da un nivel de zstd por archivo (0 = el del paquete); los vídeos van a nivel 1. El formato `.crpack` no cambia y los paquetes anteriores se siguen leyendo.
- **Juego y servidor**: el Player registra, arranca, actualiza y para el vídeo; el servidor dedicado solo registra el componente (carga escenas con VideoPlayer sin decodificar). En Android el componente se carga pero aún no reproduce.
- **Documentación**: página nueva *Vídeo* en el manual y apartados en *Audio*, *Render y rendimiento* y *Novedades*.

## 2.4.9

**Blueprints de actor, cliente WebSocket, hierba realista y un mundo que se carga en otros hilos.** Los prefabs pasan a ser Blueprints con su propio editor de nodos al estilo de Unreal y 56 nodos nuevos; los scripts hablan con servidores por WebSocket (`ws://` y `wss://`) en Windows, Linux y Android; la hierba y los árboles se ven con volumen; y el terreno, la física, la navegación y el World Partition leen, preparan y suben lo que entra en otros hilos con un presupuesto de milisegundos por frame. Además, se corrigen los siete fallos del informe de fallos y una larga lista de fallos de render, de escenas y del editor. (La 2.4.7 y la 2.4.8 no se publicaron.)

### Blueprints de actor
- **Un Blueprint es un `.crprefab` con un bloque `"blueprint"`** (padre, grafo, descripción, categoría, tick) y un `.crgraph` al lado. Se crea desde un modelo, desde un prefab o vacío, y un prefab existente se puede convertir.
- **Pestaña Blueprint** con los paneles Componentes (con glifos de hueso, Blueprint y malla), Mi Blueprint, Detalles, Ventana gráfica, Event Graph y Resultados de compilación. Insignia «BP» en el navegador del proyecto.
- **Doble clic en un modelo** abre el modal *Nuevo Blueprint de actor*, con la opción *Solo ponerlo en la escena*.
- Ctrl+S guarda el Blueprint y Ctrl+Z/Y van al grafo cuando está enfocado. Antes de Play se compilan y guardan los Blueprints con cambios. Las variables expuestas salen en el Inspector.
- **Huesos sin RenderSync**: el esqueleto se enlaza y sus huesos se crean como objetos desde el núcleo (`bindSkeleton`, `createBoneEntities`); `BoneSocket` puede apuntar a un objeto hueso.
- `Prefab.cpp` conserva las claves que no conoce, así que un prefab normal se escribe igual que antes.

### Editor de nodos estilo Unreal
- **Lienzo nuevo**: nodos compactos, de variable y reroute, pin de ejecución en pentágono, cables seleccionables (doble clic crea un reroute, Alt+clic corta, Ctrl+arrastrar mueve los enlaces), minimapa (M), comentario de la selección (C), enderezar (Q), alinear (Mayús+WASD), teclas de nodo (B/S/D/N/O/G/1-4) con clic, y **snap a la rejilla mientras se arrastra** (de 16 en 16, como Unreal; con Alt se mueve libre).
- **Arrastrar desde Componentes al Event Graph**: la raíz crea **Self** y cualquier otro objeto crea un nodo con su nombre que guarda la **ruta desde la raíz** (`Cuerpo/Mano/Arma`, `Hueso[2]` si hay hermanos con el mismo nombre), así cada instancia encuentra su propio objeto. Con **Ctrl**, un menú con sus componentes crea **Get Component** ya conectado. También desde la Jerarquía.
- **Búsqueda sensible al contexto** según el tipo del pin arrastrado; conversión automática al conectar tipos distintos; nodos desactivados, avisos y burbujas.
- **Estado por documento, deshacer y Compilar (F7)**. El editor ya no guarda solo. El editor de `.crgraph` suelto usa el mismo componente.
- Nodos de la API tipados según lo que devuelve cada función. Al recargar se descarta el estado de los nodos que cambiaron.
- **56 nodos nuevos**: Switch on Int/String, For Loop/For Each with Break, MultiGate, Retriggerable Delay, Is Valid, Select; FInterp/VInterp To, Nearly Equal, In Range, Wrap, Sign, Frac, Asin, Acos, Atan, Exp, Log, grados↔radianes, Random Bool; conversiones entero↔decimal y booleano a entero; Format Text, Substring, Replace, Split, To Upper/Lower, Trim, Starts/Ends With, Is Empty; Make Array, Add, Get, Length, Contains, Find, Remove Index, Clear; Get Owner, Get/Has/Add/Remove Component; Line Trace, Break Hit Result; Rotate Vector, Get Unit Direction, Distance 2D; On WebSocket.

### Cliente WebSocket
- **`WebSocket.connect`, `send`, `sendBinary`, `sendJson`, `close`, `on`, `off`, `state`, `info`, `list`...** y `Base64.encode`/`decode` desde los scripts. En C++, `WsClient` (`sdk/cramion/WsClient.h`) con `attach(nombre)`. En Visual Scripting, el nodo *On WebSocket*.
- Windows usa WinHTTP; Linux y Android, un motor RFC 6455 propio sobre TCP, con OpenSSL en Linux (`CRAMION_WITH_OPENSSL`) y `SSLSocket` por JNI en Android.
- **Seguro por defecto**: `wss://` exige TLS y certificado válido; `ws://` sin cifrar solo a este PC salvo con `net.WsInsecure`.
- Reconexión con espera creciente, heartbeat opcional, límites de tamaño de mensaje y de cola, y cierre limpio al salir del editor, del juego y del servidor.
- Las conexiones con nombre sobreviven a `Scene.load`; las que no tienen nombre se cierran.

### Hierba y árboles realistas
- **Hierba con volumen**: normal redondeada por píxel y orientada a la cámara (adiós al aspecto de cartón), dos rejillas y tres distancias, ancho mínimo de unos 3/4 de píxel y desvanecido al final en vez de corte seco.
- **Nuevo modelo de sombreado de hierba**: difuso envolvente, brillo de un lóbulo y translucidez fina; auto-sombra y oclusión dentro del césped; nervio central, bordes más oscuros, vetas y giro de la hoja con el viento.
- **El suelo bajo la hierba** toma el color del césped y se apaga con las briznas (antes se veía un cuadrado con más hierba lejos y un círculo oscuro cerca).
- **Cantidad según el hardware**: densidad ×0,35 en Bajo, ×0,6 en Medio, ×1 en Alto y ×1,6 en Ultra, con distancias y tramos propios.
- **Nuevos valores por defecto**: densidad 500 (antes 70), anchura 0,007 (antes 0,028), altura 0,35 (antes 0,45), detalle 12 m (antes 22). Máximo del editor: densidad 2000.
- **Árboles**: la cara de cada hoja se decide en la GPU, hojas mate con inclinación propia en el normal map, normal que sigue al temblor, auto-sombra de la copa, variación de color por zonas, borde con ruido y sombras cercanas con la malla de detalle en Alto y Ultra.

### Streaming en otros hilos
- **Jobs con prioridades** (alta, normal y de fondo), un grupo aparte para el disco, cancelación y presupuesto por frame.
- **Teselas del terreno** leídas en el hilo de disco y cosidas en un hilo trabajador; el hilo principal solo las integra con un presupuesto de ms. Al soltar una tesela se guarda en segundo plano.
- **Física**: el collider del terreno se hace en segundo plano (el viejo se queda hasta que llega el nuevo) y `MeshCollider` carga el modelo en diferido.
- **Navegación**: los triángulos del terreno se sacan de una foto inmutable en los hilos de navegación.
- **GPU**: los terrenos se preparan en otro hilo, las subidas tienen un tope por frame, la hierba usa un pool de bloques compartidos, las capas se leen en segundo plano y la vegetación se sube sin parar la GPU.
- **World Partition**: las celdas se leen y se parsean en otros hilos, se recrean con un presupuesto por frame y se escriben en el hilo de disco.
- **Servidor dedicado**: carga alrededor de cada jugador y del punto de aparición (el del `CharacterController` o la etiqueta «Player»), no alrededor de (0,0,0).
- **Informe de frames lentos** con una sección de streaming (teselas leyendo, preparando GPU, adoptadas, MB subidos, jobs en cola), en el editor y en el Player con `CRAMION_PERF_LOG=1`.

### Texturas
- **Filtrado anisotrópico y sesgo de mips** en *Gráficos > Texturas*: Automático (4x/8x/16x/16x según el nivel), apagado, 2x, 4x, 8x o 16x, y un sesgo de −1 a 2. Las opciones que la GPU no admite salen desactivadas. Se aplica al momento.
- Desde un script: `Graphics.setTextureQuality`, `setTextureFiltering`, `setTextureLodBias` y sus lecturas. También en el MCP.

### Fallos del informe corregidos
- **Pruebas con `co_await`** que pasaban sin ejecutarse: ahora se ejecutan, y una corrutina que no llega a empezar cuenta como fallida.
- **MCP con origen flotante**: las posiciones son locales y lo dicen; `absolute: true` en `create_entity`, `modify_entity` y `ground_height`, y el origen sale en las respuestas.
- **Editor sin foco**: ya no baja a 10 FPS durante Play. Aviso en la consola si la física pierde tiempo, y `Physics.simulatedTime` para los scripts.
- **Recarga en caliente con corrutinas vivas**: aviso una vez por Play y las pruebas a medias se cierran como fallidas.
- **Sonda de reflexión** más barata: intervalo entre caras, tiempo mínimo y distancia de dibujo según el nivel de la GPU; sin hierba en la captura.
- **Consola en modo edición**: `Scene.destroy` y `Entity:destroy` borran, `e.parent` se puede escribir, `Entity::setParent` en el SDK y lo hecho desde la consola se puede deshacer.
- **Compilar desde GitHub**: sin `assets/` se compila sin icono (con un aviso) y el README explica el compilador, los módulos y los iconos.

### Render
- La niebla con densidad 0 ya no da NaN; las nubes se ven bien desde dentro y por encima de la capa.
- Normales correctas con escala negativa; vectores de velocidad sin inf/NaN (también para FSR y DLSS).
- Sin costura de luz entre trozos del terreno ni escalón entre teselas al esculpir (el pincel cruza teselas y deja un solo deshacer).
- Ya no se cierra el editor cuando se acaban los descriptores del terreno: los pools crecen bajo demanda.
- Al mover el origen flotante se mueven también las matrices del frame anterior, las sondas y la oclusión del cielo.
- Sincronización de la swapchain y de la historia SSR corregida; cascadas de sombra correctas con cámara ortográfica.
- Con más de 16 masas de agua se usan las más cercanas.

### Escenas, prefabs y editor
- **Pausa** que destruía personajes y ragdolls; *Pausa* sin Play entra en Play ya pausado.
- **`Scene.load` en Play** cierra el World Partition, las multitudes y la iluminación del nivel anterior y abre las del nuevo.
- **Iluminación horneada**: una escena nueva descarta sondas y lightmap, y el horneado solo se aplica a su escena.
- **Duplicar, pegar e instanciar** reasignan las referencias entre entidades a las copias, sin UUID repetidos.
- **World Partition** con origen flotante: las celdas se calculan con la posición absoluta y las celdas guardan el origen (cabecera v2; la v1 se sigue leyendo). El Static Batching solo agrupa lo que no se descarga.
- Rutas con caracteres fuera de ANSI; *Combinar mallas* sin punteros colgando; reimportar no rehace las instancias de Blueprint.
- **Interfaz**: el cambio de tipo de área funciona a la primera e intercambia las áreas si hace falta, y se corrigen los IDs de ImGui repetidos (pestañas, Inspector, rigging, scripts, Blueprint, cinemáticas, rejilla de terreno, animador).
- **Espacios de trabajo**: al volver a la escena desde un Visual Script, un prefab o un Blueprint, cada área conserva el editor que tenías delante (por ejemplo Proyecto); antes lo cambiaba el foco de las ventanas que reaparecían. Las ventanas compartidas ya no se desacoplan de los otros espacios y la elección de cada área se recupera al reiniciar el editor.
- Física: un cuerpo dinámico rehecho en Play conserva su velocidad, *Paso* avanza los ragdolls y el personaje ya no se mueve dos veces al desplazar el origen.
- Scripts de C++: ids de callback que ya no chocan a partir del proceso 512, `onDestroy` con el objeto aún válido, y JSON con UTF-8 inválido que ya no lanza.

### Cambios a tener en cuenta
- Las pruebas con `co_await` que pasaban en falso pueden fallar ahora.
- La recarga en caliente no conserva las corrutinas: relánzalas en `onAfterReload`. Las escuchas WebSocket de C++ se pierden: usa `WsClient::attach(nombre)`.
- El SDK de C++ cambia: los scripts se recompilan solos.
- Las escenas con la hierba de fábrica sin tocar (0,028 / 70) se dibujan con los valores nuevos (0,007 / 500).
- El editor de Visual Script ya no guarda solo y el doble clic en un modelo abre el modal de Blueprint.
- En Linux, `wss://` necesita `libssl-dev`.
- Compilación: `CMAKE_CXX_SCAN_FOR_MODULES OFF`.

### CVars nuevas
- Streaming: `stream.AsyncTerrain`, `stream.MainBudgetMs`, `stream.MaxInFlight`, `stream.IoThreads`, `stream.UploadMBPerFrame`.
- Render: `render.terrain.AsyncUpload`, `AsyncPrepares`, `GrassPool`, `AsyncLayers`; `render.foliage.AsyncUpload`; `render.rt.AsyncTerrain`; `render.probe.FaceInterval`, `MinSeconds`, `DrawDistance`, `Terrain`.
- Física y navegación: `physics.AsyncTerrain`, `AsyncTerrainBudgetMs`, `AsyncTerrainMaxWaitFrames`, `DeferMeshLoad`, `DeferMeshMaxFrames`; `nav.AsyncTerrainGather`.
- World Partition: `wp.AsyncCells`, `wp.RestoreBudgetMs`, `wp.MaxIoInFlight`.
- Rendimiento: `perf.SlowFrameMs`.
- WebSocket: `net.WsInsecure`, `net.WsMaxConnections`, `net.WsMaxMessageKB`, `net.WsMaxEventsPerFrame`, `net.WsLog`.

### Pruebas
- WebSocket: protocolo RFC 6455 (`WsProtocolTests`), cliente contra un servidor de prueba local (`WebSocketTests`, `WsTestServer`) y la API de scripts (`WebSocketApiTests`).
- Blueprints: nuevas pruebas en `BlueprintTests`, y más pruebas del intérprete de Visual Script en `VisualScriptRuntimeTests`.
- Editor: autotest de la pestaña Blueprint (pasos 630 a 634).

## 2.4.6

**Mundos de 12 a 16 km que nunca están enteros en memoria.** El terreno se reparte en teselas que se leen de disco solo alrededor de cada jugador, los mapas de alturas de World Machine, Gaea, Unity o Unreal se importan directamente, y el World Partition guarda en disco lo que descarga. El juego y el servidor dedicado cargan solo la zona de cada jugador: lo que pedía un MMO.

### Terreno grande por teselas
- **Nuevo componente `TerrainGrid`** (*GameObject > Terreno grande (teselas)*): un mundo de **12 × 12 km** por defecto (8 × 8 teselas de 1536 m con 1025 × 1025 alturas, **1,5 m por vértice**). Cada tesela es un archivo `tile_X_Z.crterrain` y comparte con sus vecinas los vértices del borde: **sin grietas**.
- **Carga por zonas en otros hilos**: solo están en memoria las teselas a la *Distancia de carga* de una `StreamingSource` (el jugador) o de la cámara. Se leen de disco en segundo plano, entran unas pocas por frame y las lejanas se sueltan (con histéresis). Antes de empezar la partida se carga la tesela del jugador para que no caiga al vacío.
- **También en el editor**: solo se cargan las teselas cerca de la cámara, así que un mundo de 144 km² se edita en un PC normal. Esculpir y pintar funcionan en cada tesela; al terminar el trazo, el borde pasa a las vecinas. Lo esculpido se guarda al soltar la tesela o al guardar la escena.
- Cada tesela cargada es un `Terrain` normal (render, física, hierba, vegetación, navegación) que no se guarda en la escena ni sale en la Jerarquía; las capas, la hierba, las sombras y la colisión se ponen una vez en el Terreno grande.
- **Juego y servidor dedicado**: el servidor carga las teselas (y su colisión) alrededor de todos los jugadores conectados.
- El Inspector dice el tamaño del mundo, los metros por vértice, lo que ocupa en disco y lo que hay cargado ahora.

### Importar mapas de alturas
- **RAW/R16 de 16 bits** (el de Unity, Unreal, World Machine y Gaea), **R32** (float; se normaliza si no está en 0..1) y **PNG de 16 bits** (o de 8 bits).
- A las teselas de un Terreno grande, en segundo plano con barra de progreso y *Cancelar*, o a un terreno normal (con deshacer). Muestreo **bicúbico**: un mapa pequeño se amplía suave, sin escalones, y los bordes de las teselas salen idénticos. *Voltear en Z* y *Pintar roca en las pendientes*.
- **Convertir en Terreno grande**: un terreno de la escena (el continente del Generador, por ejemplo) pasa a las teselas ampliado al tamaño del mundo.

### World Partition en disco
- **Lo descargado va a disco, no a memoria**: cada celda descargada se escribe en la caché de la sesión (en el temporal del sistema, que se borra al terminar) y al volver se lee en otro hilo y se recrea por trozos (*Objetos por frame*, sin tirones).
- **Celdas en disco** (nueva opción del `WorldPartition`): al guardar la escena, los objetos que se descargan van a `<escena>.cells/cell_X_Z.crcell`, un archivo por celda, y no a la escena. **El juego y el servidor solo leen las celdas cercanas a cada jugador**; las lejanas nunca entran en memoria. El editor las carga todas al abrir la escena (se ve y se edita entera). Se exportan con el juego.
- El Inspector del World Partition muestra lo guardado en disco y en memoria y las celdas que se están leyendo.

### Rendimiento
- **Texturas de las capas compartidas**: los terrenos con las mismas capas (las teselas) comparten las texturas en la GPU. Antes cada terreno leía y subía las suyas: con 25 teselas habrían sido varios GB de VRAM.
- **Quitar un terreno ya no para la GPU**: se borra unos frames después, cuando ningún frame en vuelo lo usa (antes, un `waitIdle` por terreno). Caben hasta 160 terrenos a la vez (antes 16).
- **Deshacer y parar Play no vuelven a subir los terrenos**: el render los reconoce por su UUID y las teselas sobreviven a rehacer la escena.

### Pruebas
- Terreno: leer un RAW de 16 bits, repartirlo en teselas con los bordes y la esquina idénticos, cargar solo la tesela de la cámara, que las teselas no se guarden en la escena ni se recreen al deshacer, coser el borde esculpido y guardarlo al soltar la tesela.
- World Partition: lo descargado va a disco y vuelve con el mismo UUID; celdas en disco de la escena (escribir, registrar sin leer, leer al acercarse, todo de vuelta al parar).

## 2.4.5

**Interfaz como Blender, texturas comprimidas como Unity y escenas grandes que abren sin congelarse.** El editor estrena el aspecto y la distribución de Blender; las texturas se comprimen a BC7 al abrir el proyecto (cuatro veces menos memoria de vídeo, a la vista igual) y una casa con cientos de texturas de 4K ya no deja el editor un minuto colgado ni lo cierra por falta de memoria. Además: pueblos y ciudades medievales, casas con interior, terreno que se aplana bajo los edificios, splines, interfaz completa, World Partition con HLOD y carga en hilos, netcode con servidor dedicado y chat de voz, multitudes, accesibilidad, mods, informe de cierres, el código fuente abierto y Linux.

### Editor con la interfaz de Blender
- **Tema de Blender 4** («Blender Dark»): áreas grises separadas por huecos oscuros, campos y botones redondeados en gris medio, y el **azul de Blender** para lo seleccionado (selección de la Jerarquía como el Outliner, pestaña con foco, deslizadores, botón principal). El rojo queda para los errores y el naranja de Blender para avisos, Play y el objeto activo. Ejes X/Y/Z con los colores de Blender.
- **Distribución del espacio «Layout»**: la Escena grande en el centro; a la derecha la Jerarquía (Outliner) arriba y el Inspector (Propiedades) debajo; abajo, bajo la Escena, Proyecto y Consola (donde Blender pone la línea de tiempo). Se aplica sola la primera vez; *Restablecer diseño* vuelve a ella. La pestaña de prefabs usa la misma.
- **Barra de estado abajo** (como la de Blender): el objeto activo y cuántos más hay seleccionados, Play/Pausa, el número de objetos, los FPS, el tiempo de GPU y la versión. Mientras se importan texturas, el progreso.
- Barra de pestañas de los espacios de trabajo como la de Blender (la activa con el gris de las áreas).
- **Botones de Play rehechos**: Play, Pausa, Paso, *Play on VR* y el desplegable de Play tienen el mismo alto, el mismo estilo y un margen dentro de la barra de título (antes iban pegados a los bordes y parecían cortados). *Play on VR* tiene sitio para *Conectando...* y no cambia de tamaño.
- **Arrastrar en la Jerarquía sin perder la selección** (como Unity): arrastrar un objeto que no está seleccionado mueve solo ese y la selección no cambia, así que se puede soltar en un campo del Inspector del objeto seleccionado. Un clic selecciona al soltar el botón, si no se arrastró.
- **10 FPS en segundo plano**: con el editor minimizado o con otra aplicación delante, el editor baja a 10 FPS y deja de gastar GPU y CPU. No se limita jugando con el casco (*Play on VR*), en el modo cine (MCP) ni en las pruebas automáticas.

### Texturas comprimidas (como Unity)
- **Compresión BC7 al importar**: las imágenes (PNG, JPG, TGA, BMP, TIF, PSD) se comprimen a BC7 con todos sus mipmaps, el formato «High Quality» de Unity. Una textura de 4K pasa de **85 a 21 MB** de VRAM y de RAM, y a la vista es igual (46 dB de PSNR en las pruebas). También las texturas que el motor junta de varios mapas (rugosidad, metal, oclusión, cavidad) y los normal maps hechos de un bump.
- **Importación al abrir el proyecto**: en segundo plano, el editor comprime lo que no esté ya hecho y lo guarda en `Library/Cache/Textures` (un `.dds` por textura). La clave es la ruta, el tamaño y la fecha del archivo, así que mirar la caché no lee la imagen, y si cambias una imagen se vuelve a comprimir sola. Al abrir una escena ya solo se leen los `.dds`: cargar es más rápido que decodificar los PNG.
- La misma textura pedida por dos modelos a la vez se comprime una sola vez.
- La compresión anota si la textura tiene recortes por alfa (los materiales opacos no pasan a «recortados»).
- En las GPU sin BC (casi todos los móviles) las texturas siguen como antes.
- Compresor: bc7enc (MIT), en `CramionFX/vendor/bc7enc`.

### Escenas grandes sin congelarse
- **Variantes de material en hilos y sin copias**: un objeto con materiales propios hacía en el hilo principal una copia del modelo y leía, decodificaba y guardaba **todas** sus texturas, en el mismo frame. Una casa con 94 de esos objetos y 240 texturas de 4K dejaba el editor **67 segundos congelado**, llegaba a **21 GB de RAM** y se cerraba por falta de memoria de vídeo. Ahora la variante solo apunta las rutas de sus texturas (las texturas juntadas, una receta); se leen y decodifican al subirlas a la GPU, en su hilo y una vez por textura. Se hacen unos milisegundos por frame y la pantalla de carga espera a que estén todas. La misma casa: el peor frame pasa de 67,8 s a menos de 1 s y el editor pasa de 21 GB de RAM a unos 0,8 GB durante la carga.
- **Subida de modelos en hilos**: los modelos y texturas nuevos se crean en hilos aparte y el hilo principal solo los pone en su sitio, en orden, sin esperar nunca a la GPU. La cola de Vulkan se comparte con un cerrojo (render, ImGui, OpenXR y las subidas).
- Con el **streaming de texturas** activo, los modelos nuevos entran con un cuarto de lado y el streaming sube el detalle de lo que se ve grande mientras quede VRAM.
- **Cuenta de la memoria de vídeo** en el informe del streaming: cuánto ocupan texturas, destinos de render, mallas y buffers visibles por la CPU.
- Más marcas del perfilador dentro de la sincronización de actores (modelos nuevos, variantes, subidas, sockets), para que una captura de tirón diga qué tardó.

### Realidad virtual
- **Modelos de los mandos de Meta Quest 3** (Touch Plus) integrados, en la pose de agarre: hijos de un XR Controller coinciden con el mando de verdad, para ver dónde está y ajustar las manos a ojo.

### Materiales y rigging
- **Find And Build**: las texturas de un modelo importado (las que trae y las que encuentra en el proyecto por nombre: `piedra_albedo.png` → `piedra_normal.png`, `Piedra-Rough.png`, `PiedraAO.png`...) en un `.crmat` por hueco, puestos en el objeto y en los demás seleccionados. Entiende los empaquetados de Unity (Metallic-Smoothness, Mask Map de HDRP) y ORM.
- **Huesos como objetos**: el esqueleto de un modelo como jerarquía de objetos (cada hueso colgando de su padre) que siguen la animación.

### Scripting
- **Lua retirado del motor**: los scripts de juego son de C++ (o Visual Scripts). La API del motor es nativa de C++ y los scripts la usan con sus espacios de nombres (`Physics::`, `Audio::`...) o por nombre con `Api::call`, `Api::get` y `Api::set`. Se quitan sol2, el intérprete de Lua, el alias `Lua::` y el componente *Script* de Lua: las escenas antiguas que lo tengan se abren sin él (con un aviso en la consola).
- **Editor sin Lua**: el autocompletado y la ayuda de la API, la consola y los textos del editor hablan en C++ (*Desde un script: `Physics::raycast`*). Las plantillas de proyecto y la demo de minigolf traen sus scripts en C++.
- **Scripts de C++ en el servidor dedicado**: `cramion_server` carga `scripts/game_scripts.dll` y ejecuta los scripts como el player.
- **Scripts de C++ en Android**: al exportar, los scripts se compilan con el `clang++` del NDK (`ANDROID_NDK_HOME`, `ANDROID_NDK_ROOT` o el NDK del SDK de Android) en un `lib/<abi>/libgame_scripts.so` por arquitectura, y el APK lleva el proceso que los ejecuta. Sin NDK, un proyecto con scripts de C++ no se exporta y lo dice. Arreglado de paso lo que no compilaba para Android con API 26 (`memfd_create`, `posix_spawn`, `backtrace` y `from_chars` de decimales).
- **Manual reescrito en C++**: las páginas de la API de Lua dan paso a la referencia de C++ generada del SDK, y todos los ejemplos del manual son scripts de C++.
- **Visual Scripts sin Lua**: un intérprete en C++ ejecuta los `.crgraph` directamente (cadenas de ejecución, nodos puros, variables por objeto, Delay que sigue donde iba, temporizadores, Custom Event y Send Event, choques, acciones de entrada, depuración con puntos de parada y recarga en caliente).
- **Máquinas de estados sin Lua**: las condiciones son expresiones (`vida < 50 and not alerta`) y los estados mandan `OnStateEnter`, `OnStateUpdate` y `OnStateExit` a los scripts de C++ del objeto. En el editor, la casilla *Enviar OnStateUpdate* sustituye al código de cada estado; el código Lua de los proyectos antiguos se ve (solo lectura) y se puede borrar.
- **Scripts de C++ en Linux**: se compilan con clang++ o g++ a `.so` y corren aislados en `CramionScriptHost` (memoria compartida y un socket), con el mismo trato de fallos que en Windows (puntero nulo, excepción, bucle infinito, pila llena, memoria agotada, `abort()`). `clang-format` y `clangd` también se buscan en Linux. El servidor dedicado de Linux vuelve a tener scripts.

### Licencia
- **Licencia del Motor Cramion 1.2**: el **código fuente** se puede descargar, compilar, modificar y compartir gratis, manteniendo el copyright y la licencia y diciendo que es una versión modificada. Sigue sin poderse vender, revender ni resubir el motor, y los juegos siguen siendo tuyos, sin regalías.

### Splines
- **Spline** (curva Catmull-Rom, Bezier o lineal, abierta o cerrada, ancho y peralte por punto), **Extrusión por spline** (carretera con líneas, camino, río que mueve su `WaterBody`, muro, valla con postes, tubería, raíles con traviesas, cinta) pegada al terreno, y **Seguir spline** (bucle, ida y vuelta, una vez). *GameObject > Spline*; puntos arrastrables en la Escena; *Aplicar al terreno* allana y pinta debajo (el río excava su cauce). C++: `Spline::create`, `entity().splinePoint`, `closestSplineDistance`, `playSplineFollower`...

### Interfaz
- **Scroll View** (rueda, arrastre con inercia, barra, recorte), **Desplegable**, **Layout Group** (columna, fila, rejilla, ajustar al contenido) y **Máscara**. Nuevos en *GameObject > UI*.
- **Texto enriquecido** (`<b> <i> <u> <s> <color> <size> <br>`), **fuentes propias** (.ttf/.otf de Assets), contorno, negrita, cursiva e interlineado.

### Mundos grandes
- **World Partition** (`WorldPartition`, `StreamingSource`, `AlwaysLoaded`): en Play y en el juego solo están cargadas las celdas cercanas; lo lejano sale del mundo y vuelve poco a poco (por frame). Rejilla en la Escena.
- **HLOD**: *Construir HLOD* junta y simplifica (meshoptimizer) las mallas de cada celda con su color medio; se ven de lejos con la celda descargada.
- **Streaming de texturas por mips**: cada modelo sube sus texturas al detalle que pide su tamaño en pantalla (`render.streaming.TextureMips`, `TextureBias`).
- **Job system** (`jobs::schedule`, `jobs::parallelFor`, CVar `jobs.Threads`): el relieve y la pintura del terreno y los lightmaps ya lo usan.
- **World Partition más rápido**: cada repaso solo mira las raíces nuevas, borradas o que cambiaron de celda (antes recorría el mundo entero, cada hijo y cada frame) y descarga unas pocas celdas por frame.

### Multijugador
- **Netcode**: interpolación con búfer por la hora del dueño, extrapolación, giro comprimido (smallest three), relevancia por distancia, velocidad máxima con corrección del dueño (anti-trampas), historia para la compensación de lag (`Network::lagCompensatedRaycast`, `Network::positionAt`), `Network::stats()` y simulador de latencia y pérdidas (`Network::simulate`).
- **Servidor dedicado** `CramionServer` / `<Juego>Server.exe` (sin ventana ni GPU, ritmo fijo, `--port --max-players --tick`), opción en *Configuraciones de compilación*; `Network::isDedicated()`.
- **Chat de voz** (micrófono a 16 kHz, mu-law, búfer anti-saltos, pulsar para hablar o por voz, volumen por distancia): `Voice::` en C++.

### IA
- **Multitudes** (`CrowdSpawner`): cientos de agentes con navmesh que deambulan, recorren puntos, siguen u huyen, con decisiones repartidas entre frames y culling por distancia.

### Render
- **Volumen de niebla** (`FogVolume`, caja o esfera, dentro de la luz volumétrica con sombras).
- **Pelo**: modelo de material *Pelo (hair cards)* (Kajiya-Kay con dos brillos desplazados).
- **Sondas dinámicas (DDGI)**: con iluminación horneada y trazado de rayos las sondas se rehacen solas (`rt_probes.comp`).
- **Lightmap de superficie**: texeles de luz rebotada en el mundo pegados a la geometría (tabla hash dispersa), con el detalle de un lightmap y sin UV2.
- **Filtro de daltonismo** en la composición (Machado 2009: corregir o simular).

### Gráficos: 40 errores de los shaders corregidos
Auditoría de todos los shaders (con las capas de validación de Vulkan en nueve configuraciones del banco de render: sin errores):
- **Normales y escalas**: objetos con escala no uniforme o negativa (espejo) se iluminaban mal en mallas animadas, superficies y meshlets (matriz de cofactores y signo del determinante). La esfera de culling de los meshlets usa la mayor escala y el cono solo con escala uniforme. Teselado y fluidos con la proyección invertida (`abs`).
- **Iluminación**: pelo iluminado por el sol con su sombra, niebla sin el `/π` que la oscurecía, la LUT del cielo sin costura, `linearDepth` correcto en cámaras ortográficas (también en SSR, volumétrica y fuego), GI horneada con `texelFetch` exacto, brillo de terciopelo (sheen) ×π, pesos del terreno solo de las capas que existen y sin normales NaN.
- **Trazado de rayos**: cara delantera/trasera en los impactos (las sondas ya no ven el interior de las paredes), caché de radiancia sin carreras al añadir, semilla aleatoria con hash (sin patrones), superficies con la normal geométrica, reflejos con el LOD de textura correcto y OMM con márgenes seguros.
- **Posproceso**: motion blur con la reproyección de TAA, DOF con el radio correcto, foco con 2 Vec4, RCAS limitado, historial de SSR premultiplicado, SSGI sin valores infinitos y swapchain en formato UNORM (antes, sRGB doble en algunas GPU).
- **Agua y efectos**: olas del océano con el «chop» hacia el lado correcto (crestas afiladas, valles anchos: prueba nueva), lagos girados que flotan bien, cáusticas del río sin saltos, vorticidad de los fluidos, partículas VFX en espacio lineal y premultiplicadas, flipbooks por fotogramas enteros, zonas de fuego ordenadas por profundidad y vóxeles con el tiempo del frame anterior.

### Accesibilidad
- *Ventana > Accesibilidad* y `Accessibility::` en C++: daltonismo, tamaño del texto y de los subtítulos, fondo de subtítulos, alto contraste, reducir movimiento y temblor de cámara (`Camera::shake`, nuevo).

### Mods
- *Permitir mods*: carpetas con `mod.json`, `Assets/` y un `main.crgraph` (Visual Script) o `main.crprefab`, o `.datapack`, en `<juego>/Mods` y en los datos del jugador; `Mods::` en C++. Un mod con `main.lua` se monta pero avisa de que su código ya no se ejecuta.

### Herramientas
- **Informe de cierres**: pila con funciones y líneas, contexto (versión, GPU, escena, memoria), últimas líneas del registro, `abort`/`terminate`/virtuales puras, y el aviso con el informe al volver a abrir el editor.
- **Linux**: capa de plataforma (ventana X11, teclado con XIM, ratón con captura, superficie Vulkan Xlib), preset `linux-release` y servidor dedicado nativo. El reproductor con ventana sigue siendo de Windows.

### Pueblos y ciudades medievales
- **Generador de pueblos** (*Ventana > Generador de casas y pueblos*, pestaña *Pueblo*; también en el generador de terreno y por MCP con `generate_settlement`): **aldea**, **pueblo** o **ciudad amurallada** sobre el terreno, en el sitio más llano y seco o delante de la cámara. Calles principales que salen de la plaza buscando lo llano y siguen como caminos, callejas, calle de ronda; plaza con **pozo** y **mercado**; **iglesia** con campanario, interior con bancos y altar; **taberna** con barra, barriles y cuartos; **herrerías** con fragua, yunque y cobertizo; tiendas; **graneros** con pajar y cuadras; **campos** con hierba seca, surcos y vallas; **molino** con aspas que giran en Play; y en las ciudades **murallas** con adarve, almenas y torres que siguen el terreno, una **puerta** con torreones y rastrillo en cada camino que entra y la **torre del homenaje** en lo más alto. Las casas se ponen con la puerta a la calle sin pisarse entre sí ni pisar calles, plaza, agua o murallas (con la caja real de cada modelo). Plaza, calles y el pie de la muralla se allanan y se pintan; la vegetación se aparta. Objetos sueltos: barriles, cajas, carros, pacas, leña, bancos. Misma semilla, mismo pueblo; Ctrl+Z lo deshace con su terreno.
- **Casas con interior**: hogar con fuego y luz, mesa y bancos, camas, arcones, alacenas y estanterías con tarros, barriles, alfombras, vigas y, en las de varias plantas, forjados con hueco de escalera, escalera y barandilla (todo con colisión). Usos: **vivienda, taberna, herrería y tienda**. Las ventanas ahora son de vidrio transparente (el `Vidrio.crmat` sin tocar se actualiza solo).
- Estilos nuevos: **casa de entramado** (planta baja de piedra, dos o tres plantas de vigas y enlucido que vuelan sobre la calle, tejado de teja) y **cabaña de paja**. Cinco materiales nuevos compartidos: enlucido, paja, teja, tela y vigas.
- Edificios sueltos (pestaña *Edificio*, MCP `generate_building`): iglesia, granero, pozo, puesto, torre del homenaje, puerta de muralla, molino y objetos.

### Terreno
- **Aplanar terreno** (`TerrainFlatten`): deja plano el terreno bajo el **AABB** de las mallas del objeto (girado con él), a la altura de su origen o de la base de la malla, con talud suave y tierra pintada debajo, para que el terreno no tape las casas. Lo llevan todas las casas y edificios generados. En el editor se rehace al soltar el objeto tras moverlo y el sitio de antes vuelve a como estaba (también al borrarlo o deshacer). MCP `flatten_terrain`.
- Nuevas herramientas de terreno en `TerrainTools`: `flattenFootprint`, `footprintHeight`, `paintFootprint`, `flattenPath`, `paintPath` y parches para restaurar.

### Rendimiento
- **Texturas compartidas entre modelos**: la misma imagen en varios modelos (materiales `.crmat` comunes) se sube **una sola vez** a la GPU (caché por contenido). Antes cada modelo llevaba su copia de todas las texturas: un pueblo con unas decenas de edificios distintos (14 materiales con texturas) pasaba de los 6 GB de VRAM y el PC se trababa.
- Las piezas de un modelo ya no cargan las texturas de los huecos de material que no usan (la puerta de una casa leía, decodificaba y subía las de los 14 materiales). Menos RAM y crear pueblos tarda menos.
- **GPU: no se calcula lo que no se ve**. Con el trazado de rutas activo, la GI, los reflejos, las sombras RT y el SSAO (que el trazado sustituye) ya no se calculan. Con la GI apagada y RT encendido, no corre su filtro (SVGF). La luz volumétrica sin densidad ni volúmenes de niebla no se dibuja.
- **Shader de iluminación más barato**: primero el término de sombra RT y, si el píxel ya está en sombra, sin mapa de sombras, sombra de contacto ni BRDF; lectura del G-buffer con `texelFetch`; colores de las luces ya en lineal desde la CPU (antes, `pow` por luz y píxel en diez shaders); el reflejo de reserva solo donde falta SSR. Menos pasos en los filtros à-trous (potencias con multiplicaciones) y en la volumétrica (transformación de cascada precalculada).
- **Sombras en cascada**: menos cambios de pipeline, descriptores y buffers (antes por cada objeto), una caja por actor para descartar antes y la matriz luz×objeto una sola vez.
- **CPU del render**: clave binaria para las variantes de material (antes, texto con el UUID por objeto y frame), rig solo en las mallas con esqueleto, los huesos se intercambian en vez de copiar el Animator entero y la lista de cristales se guarda por modelo.
- **Sistemas del juego**: océano con tabla de giros en la FFT (de 2,4 a 0,6 ms por frame), la línea del río una vez por paso de física (no por cuerpo), sprites 2D con su textura por ruta (de 3,6 a 0,13 ms con 5000), navegación, física 2D y Character Controllers que no reescriben lo que no cambió, IK que recalcula solo los huesos afectados y el audio sin rayos de oclusión para los sonidos parados.
- El perfilador de GPU da también los tiempos crudos del último frame (`lastTimings`), para medir sin el suavizado.

### Física
- **BoxCollider como en Unity**: al añadirlo (Inspector, `entity().addComponent`, MCP `set_component`) toma el **AABB de la malla** del objeto (o de las de sus hijos, con su giro y escala) y la escala del Transform lo multiplica igual que a la malla. También la esfera (mayor media medida) y la cápsula (a lo largo del eje más largo). Botón *Ajustar a la malla* y *Restablecer valores* lo recalculan; `entity().fitColliderToMesh()`, MCP `fit_collider`, C++ `PhysicsSystem::fitColliderToMesh` / `localMeshBounds`.

## 2.3.0

**Android en casi todos los móviles, PC de gama baja mucho más rápidos y trazado de rayos sin hormigueo.** El juego exportado a Android ya arranca en móviles con Vulkan 1.0 y 1.1 y en los de 32 bits, se dibuja a la resolución que el móvil puede mover y lee las texturas DDS en cualquier GPU. En un PC de gama baja las sombras del sol y los efectos apagados dejan de costar. Las sombras y los reflejos por rayos ya no hierven sobre los objetos. Y la realidad virtual como en Unity: coger y lanzar con las manos, UI en el mundo y Play on VR.

### Android
- **Modo compatible del render** (Vulkan 1.0 en adelante): en Android el renderizador usa siempre un camino que funciona con la GPU de cualquier móvil con Vulkan (Adreno, Mali, PowerVR, Xclipse); en PC, con GPU antiguas. El *dynamic rendering* y *synchronization2* se emulan con render passes y barreras clásicas, los shaders van en SPIR-V 1.0 y todo cabe en los límites que garantiza el perfil Android de Vulkan (16 texturas por shader, 4 storage buffers, 4 destinos de color, grupos de 128 hilos). Lo que no cabe (luz volumétrica, sondas de reflexión, la oclusión del cielo desde arriba; líquidos y VFX Graph en las GPU más limitadas) no se dibuja. Antes el juego pedía Vulkan 1.3 y en muchos móviles no arrancaba. Probado emulando los perfiles Android de Vulkan 2021 (Vulkan 1.0) y 2022 (1.1) con las capas de validación: sin errores.
- **Móviles de 32 bits** (`armeabi-v7a`: Android Go y mucha gama baja): el APK lleva también la librería de 32 bits (*Incluir armeabi-v7a*, activado por defecto). Si falta junto al editor, la exportación lo avisa.
- **Android 8.0 o superior** (API 26; antes Android 10).
- **Texturas DDS en todas las GPU**: las comprimidas (BC1-BC5 y BC7) se descomprimen al cargarlas si la GPU no las lee (casi todas las de móvil: Mali, PowerVR). Antes el modelo no se podía crear y el juego no arrancaba en esos móviles. Comprobado contra la GPU: BC7 sale idéntico; en BC1-BC5 se sigue la especificación (cada GPU redondea un poco a su manera).
- **Resolución de la imagen** (*Perfil móvil > Resolución*): el lado corto como mucho 720p en calidad Baja, 900p en Media y 1080p en Alta (la nativa en Ultra), o uno fijo. La pantalla la escala sin coste: un móvil de 2400x1080 tiene 2,25 veces los píxeles de 720p y dibujarlos todos era lo que más FPS quitaba. Los toques y la interfaz se ajustan solos.
- **Perfil móvil más ligero**: sombras de 1024 en Baja y Media (1536 en Alta), texturas de 1024 como mucho en Baja y Media (sin BC van sin comprimir y la memoria es la del teléfono) y la resolución interna ya no empieza recortada otra vez por el presupuesto (se quedaba en un tercio, borrosa).
- **El presupuesto adaptativo funciona en las GPU sin timestamps** (algunas Mali y PowerVR): usa el tiempo del frame. Antes veía 0 ms, creía que sobraba tiempo y subía la calidad al máximo.
- El APK lleva solo los shaders del modo compatible (la mitad de tamaño).
- Al exportar avisa si el proyecto tiene scripts de C++: todavía solo se ejecutan en Windows (Lua y los scripts visuales sí funcionan en Android).

### Rendimiento en PC de gama baja
- **Sombras del sol**: con la cámara, el sol y la escena quietos ya no se redibujan (antes la cascada cercana se redibujaba y se copiaba cada frame). Sin personajes animados, lo estático se dibuja directo al mapa, sin la copia de la caché (64 MB por cascada de 4096). Con las sombras del sol apagadas, el mapa se limpia una sola vez. Bistro con el preset Bajo: de 5,24 a 1,66 ms de GPU con la cámara quieta (RTX 4060 Ti, 1080p).
- **Cascadas intercaladas en gama baja**: la cercana se redibuja cada 2 frames y las demás en los frames de en medio (una cascada por frame en vez de dos): con la cámara girando, las sombras pasan de 2,25 a 0,63 ms. Si una cascada dibujada unos frames antes no llega a un punto (la cámara giró), se usa la siguiente en vez de dejarlo sin sombra.
- **Lo apagado ya no cuesta**: la GI de pantalla (0,7 ms), el dibujo del SSAO, los reflejos de pantalla con su historia y los rayos de luz (también de noche o con el sol fuera de la vista) ya no se calculan si están apagados.
- **El cielo y el IBL** solo se rehacen si cambia algo (sol, luna, entorno, suelo mojado): en una GPU integrada eran milisegundos cada frame para obtener lo mismo.
- **Iluminación ligera**: nueva palanca del presupuesto adaptativo, sin las sombras suaves de contacto del sol (PCSS) ni la oclusión del cielo desde arriba.
- **Perfil de gama baja** (GPU integrada o con menos de 3 GB de VRAM): arranca ya ligero (sombras de 1536 intercaladas, sin sombras de contacto, iluminación ligera y 67 % de resolución; lo que sobre lo devuelve) y, si va muy por encima del presupuesto, mide y baja más deprisa. `CRAMION_HARDWARE_TIER=low|medium|high|ultra` fuerza el perfil.
- Con calidad completa y trazado de rayos, Bistro pasa de 9,46 a 7,16 ms de GPU con la cámara quieta.

### Trazado de rayos
- **Sin hormigueo sobre los objetos**: las sombras por rayos (luces locales y sol) se acumulan en el tiempo por luz, con recorte por los vecinos para que no dejen estela al moverse; los reflejos por rayos tienen su propio filtro temporal y espacial según la rugosidad; sus rayos siguen las normales visibles (GGX) y todos salen de la normal geométrica, sin acné en el borde entre luz y sombra. Con la imagen quieta, los píxeles que cambian de un frame a otro pasan del 0,61 % al 0,03 %.

### Realidad virtual
- Botón **Play on VR** al lado de Play: conecta el casco, juega en él y **Stop VR** lo suelta (SteamVR o Meta vuelven a su casa). Ya no hace falta reabrir el editor; el Play normal nunca toca el casco.
- **Runtime de OpenXR** (*Editar > Runtime de OpenXR (VR)*, `vr_runtime=` en `game.ini`): en *Automático* se usa SteamVR si está abierto, luego el runtime activo de Windows y luego Meta, el primero que vea un casco. Un **Quest por Steam Link o Virtual Desktop** no se encontraba si el runtime activo de Windows era el de Meta (los juegos de OpenVR, como VRChat, sí iban). Si no hay casco, se explica qué pasó con cada runtime.
- **SteamVR se quedaba trabado**: el editor abría la sesión de VR pero no le daba frames fuera del Play. Al soltar el casco ahora se cierra la sesión de forma ordenada.
- **Una cámara por ojo, como Unreal**: los dos ojos son vistas principales del render, con lo mismo que se ve en el motor (TAA, trazado de rayos, SSR, GI, post-proceso) y **a la resolución del casco** (antes, la de la ventana estirada: borroso). Cada ojo lleva **su propia historia temporal** (TAA, reflejos y luz rebotada, su vista-proyección anterior y los mismos huesos previos), así que los ojos ya no se mezclan entre sí. Las sombras (con una cámara que cubre los dos ojos), el cielo, las simulaciones y la exposición se hacen una vez por frame. Con FSR 3 o DLSS, en VR se escala con el TAA del motor (sus SDK no llevan historia por ojo). Una sola imagen para los dos ojos sigue como opción (*Editar > VR: una cámara por ojo*, `vr_mono=1` en `game.ini`), pero sin profundidad todo parecía **gigante y las manos lejísimos**.
- **La luz y las sombras se ennegrecían al mover la cabeza**: la iluminación, el SSAO, la GI de pantalla, los reflejos, la luz volumétrica y el fuego reconstruían la posición desde la profundidad suponiendo una proyección centrada; la de cada ojo de un casco es asimétrica, así que en VR todo se calculaba en sitios desplazados. Ahora usan la proyección completa (en PC quita además el pequeño error del jitter del TAA).
- **De pie sin suelo**: si el runtime no da el suelo de la habitación (STAGE), los ojos quedaban a ras del suelo del XR Origin y todo se veía gigante. Ahora la cabeza va a la *Altura de la cabeza* del XR Origin; el registro dice qué suelo se usa.
- Los LODs en VR salen de la cabeza (la misma para los dos ojos), no de la cámara del editor.
- **Coger y lanzar con las manos** (como el XR Interaction Toolkit de Unity): **XR Interactor** en cada mano (agarre para coger lo que toca la mano o lo que apunta el rayo, láser visible, vibración) y **XR Grabbable** en los objetos con Rigidbody: mientras se sostienen siguen a la mano **con física** (chocan con mesas y paredes) o pegados a ella, se pueden coger por un punto de agarre (herramientas) y al soltarlos salen con la velocidad de la mano.
- **UI en el mundo**: el Canvas tiene *Modo: Mundo (VR)* (como World Space en Unity): un panel en la escena que se ve en el casco y en todas las vistas y se usa con el rayo y el gatillo (botones, casillas y sliders, con sus eventos a los scripts de Lua y de C++). Cada panel se pinta en su textura con las mismas letras e imágenes que la UI de la pantalla y se dibuja tras el tono, con la profundidad de la escena.
- **Lo que se coge ya no choca con el jugador**: los XR Grabbable atravesaban mal la cápsula del Jugador VR (al acercarlos al cuerpo te empujaban y al andar los arrastrabas). Ahora la cápsula del XR Player los ignora, con casco o sin él (*Chocar con lo que se coge* en el XR Player para volver a chocar). Nuevo `PhysicsSystem::ignoreCollision(a, b)` / `Physics.ignoreCollision(a, b)` en Lua y C++ (como `Physics.IgnoreCollision` de Unity): parejas que no chocan, también con la cápsula de un Character Controller; lo que ya se tocaba se suelta al momento.
- **Plantilla Realidad virtual (VR)** (como la de Unity): Jugador VR con física, mesa con objetos que se cogen y se lanzan, linterna con punto de agarre, muro de cajas y un panel de UI en el mundo con su script de **C++** (`MenuVR.h` + `MenuVR.cpp`).
- Herramienta `cramion_stereo_check`: el render en estéreo sin casco (como `-emulatestereo` de Unreal) en una ventana invisible con las capas de validación; guarda el ojo derecho y la misma cámara en PC para compararlos. `VulkanRenderer::readSceneImage` copia la última imagen a la CPU.
- **El render temblaba al mover la cabeza**: el juego iba a ~30 FPS en el casco porque la ventana volvía a dibujar toda la escena (lo mismo que costaba el casco), y el runtime repetía frames. Ahora el espejo de la ventana es la imagen del casco (no se dibuja otra vez), la segunda vista del editor no se dibuja en VR y la **resolución adaptativa va siempre a los Hz del casco** (72/90/120). En VR se quitan el motion blur, la profundidad de campo, la distorsión de lente, la aberración cromática y el grano.
- **Jugador VR en primera persona con física** (*GameObject > Realidad virtual > Jugador VR*, componente **XR Player**): XR Origin + Character Controller. Stick izquierdo para andar (hacia la cabeza o la mano), stick derecho para girar (por pasos o suave), A para saltar, pulsar el stick para correr. **De pie**: andar por la habitación mueve la cápsula chocando con las paredes y agacharse de verdad agacha la cápsula. **Sentado**: la altura de la cabeza del XR Origin; agacharse con el stick derecho abajo.
- `CharacterController` `move()` llamado cada frame ya no quita la interpolación (el personaje no da tirones).

### Arreglos
- **Una línea de rayas oscuras en el borde de abajo de la imagen**: la normal que se reconstruye de la profundidad salía con ruido en la primera y la última fila (y columna) y dejaba píxeles sueltos en sombra.
- El desenfoque del SSAO repetía vecinos en los bordes de la imagen.
- **El editor se cerraba al listar las plantillas** (*Nuevo proyecto*) si en Descargas había un archivo con un emoji en el nombre.
- Los líquidos daban un error de validación en el modo compatible.

### Herramientas
- `cramion_render_bench` (desarrollo del motor): banco del render sin ventana con el tiempo de GPU de cada pasada y el ruido temporal (el hormigueo), los escenarios de demostración, cámara en órbita, un personaje animado, perfil de hardware forzado, presupuesto congelado, emulación de Android y la comparación de la descompresión BC con la GPU. `CRAMION_SHADOW_MARKS=1` mide cada cascada por separado en el perfilador.

## 2.2.0

Rendimiento con **Insights** (perfilador de CPU con capturas para Perfetto) y presupuestos para animación, partículas y audio; **pies en el suelo rehechos** para humanos y animales (sin pies flotando, escaleras sin tirones, rodillas que siempre se doblan bien) y el audio que **se para de verdad** al dar Stop.

### Rendimiento
- **Insights** (*Ventana > Insights*): perfilador de CPU con árbol de zonas (media, propio, p95, máximo, llamadas), resumen del frame (p50/p95/p99, tirones), contadores y capturas **`.crtrace`** que se abren en Perfetto o `chrome://tracing`. Los **tirones se capturan solos** (`prof.HitchMs`) y la consola dice qué zona se comió el frame, también en el juego exportado. API: `CR_PROFILE_SCOPE`, Lua `Profiler.*`, herramienta MCP `profiler`.
- **LOD de animación**: los personajes lejanos o detrás de la cámara se animan menos veces por segundo (`anim.lod.*`).
- **Presupuesto de partículas** (`fx.particles.Budget`): los emisores frenan al acercarse al límite y nunca se pasa; los lejanos no emiten ni chocan.
- **Voces de audio virtuales** (`audio.MaxVoices`): solo se mezclan las que más se oyen; las demás siguen el tiempo y vuelven donde tocaría.
- **Lua profiler**: cada script es una zona y avisa si pasa de `lua.BudgetMs`.
- Los clips `.cranim` se leen más rápido (sin copiar los arrays de claves).

### Animación e IK
- **Pies en el suelo, rehechos** (humanos y patas de animales, un mismo solver `anim/FootPlacement`):
  - Los pies **ya no flotan** al andar ni de pie: el IK blando encogía la pierna casi recta aunque el objetivo fuera su propio pie (2-4 cm en el aire, más con *Bloquear pies*).
  - **Escaleras y obstáculos**: cada pie mira el suelo bajo el tobillo y bajo la punta (la punta no se mete en el escalón) y el pie que va por el aire sube antes de llegar a la tabica. El suelo de cada pie se guarda en el mundo: cuando el Character Controller sube o baja un escalón de golpe, el pie apoyado se queda y la cadera ya no pega un salto.
  - Solo los pies que la animación apoya bajan a buscar el suelo y sujetan la cadera; el que va por el aire sigue a la cadera.
  - **En el aire** (saltar, caer) el IK se aparta solo: el Character Controller dice si pisa suelo; sin él, por cómo sube o cae. Al despegar ya no hunde la cadera ni estira las piernas hacia el suelo.
  - **Rodillas**: con la pierna recta en la animación se doblaban hacia cualquier lado (hacia dentro, contra la otra pierna, o hacia atrás al subir un escalón); ahora siempre hacia delante. Los codos, hacia atrás. Las patas de animal se doblan como en su pose de reposo y ninguna articulación queda al revés (FABRIK con la pata recta no sabía hacia dónde doblarla).
  - El rayo de los pies ya no cuenta lo que empieza **dentro de una pared** o de un escalón alto (el pie saltaba hacia arriba y se giraba de lado), ni paredes o techos, ni colliders del **propio personaje** que cuelgan de su Rigidbody o Character Controller en otra rama.
  - **Bloquear pies** se suelta al girar, si la pierna ya no llega (antes despegaba el pie del suelo) o si el pie clavado quedaría al otro lado del cuerpo (las piernas se cruzaban).
- **Patas procedurales**: el pie ya no se sube a cajas o mesas que tiene al lado (el rayo salía ~0.75 m por encima del cuerpo) y el cuerpo se inclina como mucho 35°.
- **IK blando**: brazos y piernas ya no se bloquean rectos de golpe al llegar a su largo, y un objetivo fuera de alcance deja de tirar del miembro (antes cualquier cosa cercana lo estiraba).
- **Pies en el suelo** ya no pisan a otros personajes, ragdolls ni objetos dinámicos.
- **`crossFade(estado, segundos)`** (como `Animator.CrossFade`): saltar a un estado del Animator desde Lua o C++; espera a que su clip esté leído (nunca pose T). `animatorState()` y `animatorStateTime()`.
- **Retargeting** de esqueletos cuyo reposo no pisa el suelo (BVH, Motifect): ya no flotan. Se reconocen esqueletos que llaman `Leg` al muslo y `Shin` a la espinilla.
- **FBX solo de animación** (sin malla): se importan y sus clips se extraen; herramienta MCP `extract_clips`.

### Física
- El **ragdoll** de un personaje no choca con su propia cápsula y los personajes no se suben encima de un cuerpo caído.

### Arreglos
- **Audio al parar el Play**: la lluvia, el viento y los truenos del ambiente y la cola de la reverberación seguían sonando en el editor (fuera del Play nadie bajaba sus niveles). Ahora al parar se calla todo, cada sonido se libera y el volumen y el paso bajo del oyente vuelven a lo normal. **Pausar el Play** también pausa el audio (fuentes, sonidos sueltos, ambiente y ecos) y sigue donde iba.
- Cerrar el editor con scripts de C++ cargados podía colgarse o fallar al salir.
- El juego exportado siempre se cierra (si algo se atasca al salir, se termina a los 8 s) y deja en el log lo que tardó cada paso del cierre (`CRAMION_TRACE_SHUTDOWN=1`).
- Lluvia y viento sintetizados más bajos (la lluvia fina sonaba como un aguacero).
- MCP `list_assets` sin carpeta no devolvía ningún asset.

## 2.1.0

Incluye también todo lo de la 2.0.0 (más abajo), que no se publicó por separado.

### Grafos de nodos
- **Shader Graph** (`.crshadergraph`): materiales con nodos como el Shader Graph de Unity o el editor de materiales de Unreal. Entradas (UV, tiempo, posición, normal, vista, color de vértice...), propiedades que salen en el Inspector del material, texturas (también triplanar), matemáticas, ruido (valor, Perlin, Voronoi), Fresnel, panner, mezcla de normales y una salida con color, alfa, metal, rugosidad, oclusión, emisión, normal y desplazamiento de vértices. Genera un `.crshader` normal (recompila al momento, funciona en Android) con **vista previa** en una esfera, el **código generado** y los errores de compilación marcados en su nodo.
- **VFX Graph** (`.crvfx`): partículas simuladas **enteras en la GPU** (de cientos a cientos de miles) con contextos Spawn / Initialize / Update / Output: caudal, ráfagas, por distancia y por evento; formas (esfera, caja, cono, círculo, línea, superficie de un modelo); gravedad, viento, rozamiento, turbulencia (curl noise), atractores, vórtices, **colisión con la escena** por el buffer de profundidad, con planos y esferas; color y tamaño en la vida, color por velocidad, partículas suaves, flipbook, estiradas por la velocidad e iluminadas. **Parámetros expuestos** que cambian el Inspector y los scripts (`setEffectFloat`, `sendEffectEvent`...). 10 plantillas (fuego, humo, explosión, magia, nieve, lluvia...).
- **Visual Scripting** (`.crgraph`, como los Blueprints): eventos, flujo (Branch, Sequence, For, While, Delay, Do Once, Flip Flop, Gate), variables expuestas, matemáticas y **cualquier función de la API como nodo**. Corre en Play y en el juego; los nodos que se ejecutan **se iluminan**, los pines enseñan su último valor y hay **puntos de ruptura** (F9) que pausan el juego.
- **Behavior Trees** (`.crbt`) con pizarra, como los de Unreal: Selector, Sequence, Parallel, Random; tareas (Move To con NavMesh, Wait, animaciones, Run Script...), decoradores con **observer aborts**, servicios (vista con línea de visión, oído, el más cercano con un tag...). **Depuración en vivo**: la rama activa brilla, cada nodo enseña su último resultado y la pizarra se puede cambiar.
- **Diálogos** (`.crdialog`) con su editor de nodos: líneas, opciones con condición, condiciones, variables, eventos y saltos, una **caja de diálogo** de UI lista para usar y una prueba del diálogo sin dar Play.
- Un lienzo de nodos común para todos los editores: zoom, desplazar, selección por caja, copiar/pegar/duplicar, comentarios y un buscador de nodos (Espacio o clic derecho; soltar un enlace en el vacío crea un nodo conectado).

### Juego
- **Partidas guardadas**: `Save.save("slot1")` / `Save.load`, ranuras con fecha, escena y tiempo jugado, autoguardado, valores sueltos y el componente **Saveable** (transform, física, componentes elegidos y estado del script con `OnSave`/`OnLoad`). Se escriben en segundo plano. Ventana *Partidas guardadas*.
- **Localización**: tabla de textos por idioma (ventana *Localización*), CSV para traductores, argumentos `{0}` / `{nombre}`, **plurales** por idioma y una *Clave de localización* en los textos de la UI que cambian solos con el idioma (`Text.setLanguage`).
- **Repeticiones**: graba y reproduce el juego (objetos, animaciones y sonidos) con línea de tiempo, cámara lenta, cámara libre, marcadores y archivos `.crreplay`; con *Últimos N segundos* sirve para killcams. API `Replay.*`.
- **Motion Matching** (`.crmmdb`): animación por búsqueda en una base de fotogramas, como el Pose Search de Unreal: trayectoria predicha con muelles, pies y cadera, etiquetas e inercialización. Editor de la base y trayectorias de depuración en la Escena.
- **Destrucción**: *Ventana > Fracturar* parte una malla en trozos de Voronoi (con varios niveles y trozos pequeños alrededor de un punto) y el componente **Destructible** la rompe con golpes fuertes o con `entity:fracture(punto, fuerza)`; escombros que encogen y desaparecen y un máximo de trozos en la escena.
- **Vehículos completos**: curva de par, caja de cambios automática o manual, diferenciales (4×4 con reparto), barras estabilizadoras, curvas de fricción de los neumáticos, Input Actions, sonido de motor que sigue a las rpm y derrape con humo. **Asistente de vehículo** que monta las ruedas solo buscando su nombre. `vehicleState()`, `setGear`.

### 2D
- **Sprites** con *Sprite Editor* (cortes por rejilla, filas/columnas o automáticos, pivote, píxeles por unidad, filtro Point), **Sprite Animator**, **Luces 2D**, capas de orden y modo mosaico.
- **Tilemaps** por capas con **Rule Tiles** (autotiling de bordes y esquinas), **paleta** para pintar en la Escena (pincel, rectángulo, relleno, borrar, gotero) y **Tilemap Collider 2D** sin costuras.
- **Física 2D** propia (Rigidbody 2D, colliders de caja, círculo, polígono, tilemap y composite, triggers, plataformas de un sentido), eventos `OnCollisionEnter2D`/`OnTriggerEnter2D` y `Physics2D.raycast/overlap*`.
- **Vista 2D** en el editor y cámaras **ortográficas** en el juego.
- Plantilla **Plataformas 2D**.

### Iluminación horneada
- *Ventana > Iluminación*: la luz rebotada se hornea una vez en la CPU (todos los núcleos, BVH, varios rebotes, luces puntuales y focos con sombra, superficies emisivas y cielo) en **volúmenes de sondas** (componente *Light Probe Volume*; si no hay, uno automático). En el juego sustituye al SSGI o a los rayos y casi no cuesta: luz rebotada en **Android** y en PCs sin trazado de rayos, para lo estático y lo que se mueve. Se guarda junto a la escena (`.crbake`) y va con el juego.

### Flujo de trabajo y plataforma
- **Control de versiones (Git)** en el editor: cambios, diferencias, preparar, commit, push/pull, ramas e historial; inicializa el repositorio con `.gitignore` y Git LFS para los binarios.
- **Pruebas automáticas**: `Test.case` y `Assert` en `*.test.lua`, pruebas de edición y de Play (esperan frames, segundos o a que se cumpla algo), ventana *Pruebas (Test Runner)* y `CramionEditor.exe --run-tests <proyecto> --junit resultados.xml` para integración continua.
- **Steam** (Steamworks, sin incluir el SDK): logros, estadísticas, marcadores, Rich Presence, overlay, Steam Cloud, Workshop y salas; se configura en *Configuraciones de compilación* y el juego funciona igual sin Steam.
- **Recarga en caliente de C++ con estado**: guardar un `.cpp` en Play recompila y cambia la DLL **sin parar el juego**, conservando las propiedades y lo que guarden `onBeforeReload`/`onAfterReload`; un aviso dice si se recargó o el primer error.
- Avisos flotantes en el editor y herramientas MCP nuevas: `run_tests`, `test_results`, `bake_lighting`, `lighting_state`, `create_vfx`, `vfx_control`, `create_visual_script`, `create_shader_graph`, `create_behavior_tree`.
- Plantilla **Coches**: un coche con motor, marchas y cámara de persecución en una pista con rampas.

### Notas
- Linux: la plataforma *Linux (x64)* ya se puede elegir y configurar en *Configuraciones de compilación*, pero el reproductor nativo de Linux aún no existe (los juegos de Windows funcionan con Proton).
- La iluminación horneada usa sondas (no lightmaps por texel); el generador de UV de lightmap ya existe en el motor y queda para una versión siguiente.

## 2.0.0 (publicada dentro de la 2.1)

### Streaming de modelos y animaciones
- **Carga en segundo plano**: los modelos se leen en hilos de fondo (el `.crdata`, sus texturas y sus LODs) y aparecen al terminar, sin congelar el frame al abrir una escena ni al **instanciar en el juego**. Si otro sistema (la física de un Mesh Collider) necesita el mismo modelo, espera a ese hilo en vez de leerlo dos veces.
- **Subidas a la GPU poco a poco**: lo nuevo se sube en orden, unos milisegundos por frame. Antes, cada modelo nuevo hacía subir la escena **entera** (todas las mallas y texturas, parando la GPU): instanciar algo daba un tirón tanto mayor cuanto más grande la escena.
- **Animaciones en segundo plano**: los clips sueltos (`.cranim`, p. ej. los de Mixamo de un Animator) se leen y convierten en otro hilo y se **precargan** todos los del controlador en cuanto aparece el personaje; antes, el primer uso de cada clip era un tirón. Mientras su clip llega, el personaje no se dibuja (nunca en pose T).
- **Residencia en la memoria de vídeo**: un modelo que lleva unos segundos sin verse (más pequeño que medio píxel en pantalla) libera sus mallas y texturas de la GPU y vuelve solo, en segundo plano, en cuanto se acerca; nada visible desaparece. Estadísticas `models_resident` / `models_evicted` (`Graphics.get`).
- Se ajusta con las CVars `render.streaming.AsyncModels`, `UploadMs`, `GpuResidency`, `MinPixels` e `IdleSeconds` (ventana *Variables (CVars) y memoria*, consola o `CVar.set`).

### Gráficos: iluminación, trazado de rayos y agua
- **Escena de rayos completa**: los rayos ven ahora el **terreno** (una malla de hasta 512 × 512 celdas siempre por debajo de la que se dibuja, con el color de sus capas y de la hierba horneado) y los **árboles** de la Vegetación (la malla media de cada especie con sus hojas recortadas, a menos de 350 m). Antes los rayos de la GI y de los reflejos atravesaban el suelo y veían cielo por debajo: en exteriores la luz salía plana y demasiado clara, y bajo las copas no oscurecía. Esculpir o pintar el terreno lo actualiza en su sitio, sin parar la GPU.
- **Sombras del sol por rayos** (con RT): sombra exacta del escenario y del terreno, nítida al pie y difuminada con la distancia (el disco del sol), **también más allá de las cascadas** (una montaña tapa el sol del valle al atardecer). Se combina con las cascadas, que siguen poniendo las de los personajes, la hierba y los árboles que se mecen.
- **Luces locales sin fugas en la luz rebotada y los reflejos por rayos**: cada impacto traza un rayo de sombra a una luz elegida según lo que aporta. Antes la luz de una bombilla atravesaba las paredes.
- **El RT apagado no cuesta nada**: la escena de rayos (BLAS, TLAS, caché de radiancia) se construye solo cuando se activa el trazado o el path tracing y se libera a los 10 s de apagarlo; las imágenes de las sombras por rayos y de la acumulación del path tracing solo ocupan memoria si se usan (antes ~30 + 60 MB a 1440p siempre).
- **Sin tirones con RT**: la TLAS se reconstruye en la GPU dentro del frame (antes paraba la GPU cada vez que algo se movía); las BLAS se construyen en un solo envío y se **compactan** (menos memoria) y los pipelines de rayos se compilan una vez: reconstruir la escena pasa de 1,2 s a 30–130 ms.
- **Oclusión del cielo desde arriba** (sin RT, en *Ajustes de render*): dos mapas de profundidad vistos desde arriba alrededor de la cámara; el sotobosque y lo que está bajo tejados y voladizos ya no recibe todo el cielo aunque lo que lo tapa no salga en pantalla.
- **Bombillas con tamaño**: el *Radio de la fuente* de las luces puntuales y focos ensancha su brillo en las superficies pulidas (un disco, no un punto que parpadea).
- **Sombras suaves del sol en pendientes**: el PCSS compara cada muestra con el plano del receptor; antes la penumbra de un árbol o un tejado sobre una ladera salía más oscura y a manchas.
- **Luz rebotada sin estelas**: si la luz cambia de golpe (se apaga una lámpara, se abre una puerta) la GI se corrige en pocos frames en vez de arrastrar ~33.
- Tonemapper **AgX** (el de Blender 4): respuesta de película, las luces fuertes se desaturan como en una cámara. Lo usan los presets *Realista* y *Ultra realista*.
- **Autoenfoque suave**: la profundidad de campo ya no salta de enfoque; lo sigue como el motor de una lente, con su *Velocidad del autoenfoque*.
- **Árboles mucho más ligeros**: los niveles de detalle medio y lejano dejan de generar ramas más finas que un píxel (el pino pasa de 9.300 a 2.400 triángulos a media distancia y de 3.700 a 670 lejos) y las distancias de detalle se ajustan a la resolución y al presupuesto adaptativo. En el mundo de demo, de 40 a 6 millones de triángulos de vegetación por frame (geometría de 9,6 a 4,1 ms).
- **Agua**: los ríos en cuesta ya no se cortan ni desaparecen al acercarse (la hondura se mide perpendicular a la superficie); los ríos ya no salen blancos enteros ni con dos bandas de espuma en las orillas; la espuma de la costa ya no es opaca ni sigue los polígonos del terreno.
- **Niebla volumétrica más realista**: la densidad por defecto del polvo pasa de 0,02 a 0,004 (con 0,02 todo se veía lechoso). Las escenas guardadas con el valor viejo se actualizan al abrirlas.

### Vegetación realista: árboles y hierba
- **Copas a contraluz**: la luz que atraviesa una hoja sale más clara y amarillenta que la reflejada (transmisión difusa, como una hoja de verdad, además del brillo mirando al sol a través de ella) y la **luz del cielo también atraviesa las hojas**. Las copas vistas desde abajo ya no son siluetas negras. Vale para todo lo *subsurface* (hojas, hierba, piel fina).
- **Hojas sin rayas negras**: la silueta de las hojas de roble y de sauce (y la punta de las frondas de la palmera) daba NaN en su punta; pintaba rayas negras que cruzaban las hojas y, en los mips, ennegrecía las copas lejanas.
- **Cortezas nuevas**: el **roble**, con crestas largas que se separan y se vuelven a juntar, surcos en V y alguna grieta y liquen gris (antes, manchas de musgo verde y liquen amarillo); el **pino**, con placas gruesas y alargadas separadas por fisuras rojizas y escamas por capas (antes, un mosaico como piel de jirafa). Las **ramas finas tienen la corteza lisa**, y el **pino** es anaranjado en la parte alta del tronco y en las ramas, como el pino silvestre. El musgo pasa de verde lima a verde oliva apagado.
- **Pinos y abetos con copa llena**: cada borla del pino es, de cerca, **siete brotes de verdad en 3D** (cepillos de agujas cortos y gordos que salen en todas direcciones de la punta de la ramita: una mata esponjosa, no un abanico plano) y, de lejos, un abanico de brotes que cuesta menos; antes, un brote estrecho por tarjeta y la copa parecía ramas peladas con plumas sueltas. Las ramas del **abeto** son densas y triangulares (antes, peines ralos: parecía seco). Agujas más claras. Las tarjetas de agujas y de hojas de abedul tenían las proporciones del dibujo al revés: salía estrechado sobre la tarjeta (las agujas casi paralelas al brote).
- **Hierba como un prado**: las briznas crecen en **matas** que se abren desde su centro, más altas en el medio y que se mecen juntas; cada mata tiene su tono, el campo tiene **manchas** (frondosas y pobres) y hay puntas secas y alguna brizna de paja. La brizna es casi toda de su color (el *Color base* solo asoma abajo): antes el degradado más la oclusión dejaban el césped casi negro. Translucidez mayor en la punta. Lo escala la *Variación de color*.
- **Texturas de los árboles 3 veces más rápidas** (de 3,6 s a 1,1 s al abrir el editor o el juego): todas las capas a la vez, las cortezas por filas en paralelo, cada hoja recorre solo su caja y las lenticelas del abedul se pintan una a una (antes cada píxel miraba las 340). Mismo resultado, bit a bit.

### Scripts en C++ aislados
- **C++ es el lenguaje de scripting; Lua queda obsoleto** (sigue funcionando en los proyectos que lo usan). El editor crea scripts de C++ por defecto y marca Lua como *(obsoleto)*.
- **Manual en C++**: referencia completa de la API de C++ generada desde las cabeceras del SDK (30 páginas, cerca de 800 funciones y tipos con su firma y descripción: Entity, Scene, Input, Mathf/Vec3/Quat, Mesh, StateMachine, Audio, Network...), *Tu primer script*, ciclo de vida, propiedades y recarga en C++, y los **14 ejemplos en C++** (.h y .cpp), todos compilados con el SDK. La referencia de Lua pasa a *Referencia de Lua (obsoleta)*. La web principal presenta C++.
- C++ alcanza a Lua: **mensajes de la UI** (botones, sliders, casillas y campos) a los scripts de C++ con `on("OnJugar", ...)`/`onMessage`, **`entity.script<T>()`** para usar el script de otro objeto (como `GetComponent<T>()`), y los eventos **`onNetVar`** y **`onOriginShift`**.
- Arreglo: las propiedades de Lua (`entity.text`, `entity.speed`, `mesh.vertices`...) se generaban en C++ como métodos y fallaban; ahora son lectura (`text()`) y escritura (`setText(...)`).
- Scripts de **C++** (clase con `start`, `update`, colisiones... y `CRAMION_SCRIPT(Clase)`), componente **C++ Script** con propiedades. Se compilan solos al guardar (clang++ o Visual Studio) y se recargan en Play; al exportar, la DLL va con el juego.
- Corren en **otro proceso** (`CramionScriptHost.exe`): un puntero nulo, una excepción, una recursión infinita, memoria agotada, `abort()` o un bucle infinito **no tumban el motor**. La Consola da el error con **archivo y línea**, el script se desactiva y el proceso arranca de nuevo para los demás. Tope de tiempo por llamada y de memoria del proceso (Job Object).
- **Toda la API de Lua en C++** (Audio, Scene, Graphics, Navigation, Network, Http, Prefs, Voxel, Weather, XR... y todos los métodos de la entidad), generada desde el motor en `cramion/Api.gen.h` con el tipo dinámico `Value` (números, texto, vectores, entidades, listas, objetos y callbacks), además de llamadas propias rápidas (transformaciones, física, Character Controller, entrada, raycast, tiempo, log, CVars). `Lua::call` llama a cualquier función de Lua. MCP: `cpp_compile`.
- **Compilador incluido**: el editor y el release traen clang++ (llvm-mingw) en `toolchain/`; no hace falta instalar Visual Studio ni LLVM.
- **Como en Unity**: se arrastra el `.cpp` al objeto, al campo *Script* del componente o a *Add Component*; la clase se pone sola (si hay varias, se elige en una lista). Las **`Property<T>`** salen en el Inspector con su control: `bool`, números (con `Range`), texto, `Vec2/Vec3`, `Color`, listas desplegables (`Options` o `enum`), objetos de la escena (`Entity`, con `Requires("Light")`), assets (`Prefab`, `Model`, `Material`, `AnimationClip`...), archivos (`Texture`, `AudioClip`, `SourceFile`...) y listas `std::vector<T>` de todos ellos; con `Tooltip`, `Header` y `Label`.
- **Editor de código con IntelliSense de clangd** para `.cpp`/`.h`: sugerencias (también tras `.`, `->`, `::`), errores y avisos en vivo subrayados, información al pasar el ratón, firma de la función con el parámetro actual e **ir a la definición** (F12 / Ctrl+clic). Se escribe `compile_commands.json` para otros editores.
- Scripts de C++ en **dos archivos**: `Clase.h` (clase y propiedades) y `Clase.cpp` (métodos y `CRAMION_SCRIPT`); se puede arrastrar cualquiera de los dos. Botón **Abrir cabecera (.h) / Abrir .cpp** (Alt+O).
- **Formatear** C++ con clang-format incluido (Mayús+Alt+F), sin mover el cursor; **al guardar** si se activa en la nueva ventana **Editar > Configuración del motor** (estilo Cramion/LLVM/Google/Microsoft/... o el `.clang-format` del proyecto, sangría y largo de línea).
- **Consola C++** propia en el editor de C++: errores de compilación (doble clic va a la línea), `Debug::log` y fallos en Play, CVars/Lua; botón **Compilar** (F7).
- IntelliSense de C++ **completo**: las 351 funciones del motor llevan el **nombre de sus parámetros y su descripción** (al escribir, en la firma y al pasar el ratón), la parte escrita a mano del SDK (Entity, Input, Physics, Time, Scene, Debug, eventos del Script) está documentada, y se ven hasta 200 sugerencias (antes 60).
- SDK de C++: **`Mathf`**, **`Vec3`/`Vec2`**, **`Quat`** y **`Color`** completos como en Unity (lerp, smoothDamp, slerp, lookRotation, perlinNoise...), calculados en el script; clases **`Mesh`** y **`StateMachine`** para mallas por código y máquinas de estados.
- **Nuevo script C++** pide el **nombre** (es el de la clase y de `Nombre.h`/`Nombre.cpp`), avisando si no es válido o ya existe en el proyecto.
- El editor de código **cierra solo** `(` `[` `{` `"` `'` (y `<` de plantillas e `#include` en C++); escribir el que cierra pasa por encima, Retroceso entre `()` borra los dos y Enter entre `{}` baja la llave a su línea.

### Variables (CVars)
- **`CVar<T>`** (bool, int, float, double, texto) como las console variables de Unreal: nombre, límites, descripción y banderas (guardada, solo lectura, truco). Se guardan en `ProjectSettings/CVars.json`.
- Ventana **Variables (CVars) y memoria** (memoria del editor, del equipo, VRAM y del proceso de scripts), comandos en la consola (`nombre valor`, `cvars`, `reset`), Lua (`CVar.get/set/register/list`) y MCP (`cvar_list`, `cvar_set`).

### Arreglos
- Explorador de los editores de código: doble clic en un `.cpp`/`.h` lo abre (antes llevaba al Proyecto porque no se tomaban por scripts). El menú del botón derecho tiene **Buscar este script en el Proyecto**, que lo deja seleccionado y a la vista en el Proyecto.
- **Borrar** desde el Proyecto y desde el explorador de scripts (Lua y C++): ahora se pueden borrar scripts, imágenes, sonidos, shaders, assets y carpetas con contenido (menú o Supr, también varios a la vez). Va a la **Papelera de reciclaje**, cierra sus pestañas y ofrece borrar también el `.h`/`.cpp` de al lado. Antes los assets borrados seguían viéndose en el navegador.
- C++: la lista de sugerencias ya no tiembla al escribir (se filtra al momento y queda fija al principio de la palabra); los errores largos de la barra de estado ya no empujan la ventana.
- C++: el IntelliSense no funcionaba en la pestaña del script (las respuestas de clangd y la compilación al guardar solo se atendían en la pestaña Escena).
- C++: las cabeceras `.h` se analizaban como C en `compile_commands.json`; con errores de compilación ya no se recompila en bucle cada 2 s.
- Modelado: los encabezados de la ventana chocaban con los botones de modo ("conflicting ID" de ImGui).

### Modelado poligonal (como ProBuilder)
- Nuevo componente **Malla editable** (`EditableMesh`): mallas de polígonos (caras de N lados) que se guardan en la escena y se editan en la vista Escena por **vértices, aristas y caras** (teclas 1–4), con clic, caja, rayos X y el gizmo de siempre sobre lo elegido; **Mayús + arrastrar extruye**.
- **16 formas paramétricas**: cubo, plano, cilindro, cono, esfera, icoesfera, toro, tubo, prisma, rampa, escalera (maciza o de losa), escalera curva/de caracol, arco, pared con puerta, habitación y cápsula.
- Operaciones: extruir (grupo o separadas), inset, **bisel** (con esquinas), subdividir, conectar, **insertar bucle**, bucles y anillos de aristas, puentes, rellenar agujeros, unir caras, soldar, colapsar, triangular, separar en objeto, relajar, ruido, **Catmull-Clark**, **espejo con costura**, combinar y **booleanas** (unión, resta, intersección).
- Material por cara (color por hueco o `.crmat`), grupos de suavizado y suavizado automático, **UV por cara** (caja, plana o manual; repetir en metros, encajar o estirar; en el mundo) que no se estiran al mover vértices. Convertir modelos importados en editables, exportar a OBJ.
- Ventana **Modelado**, *GameObject > Malla editable* y MCP: `modeling_create`, `modeling_info`, `modeling_edit`, `modeling_convert`.

### Character Controller
- Nuevo componente **Character Controller** sobre el CharacterVirtual de Jolt: cápsula que choca y desliza, **sube escalones**, respeta la **pendiente máxima**, se pega al suelo al bajar, **sigue a las plataformas** que se mueven, empuja Rigidbody, entra en triggers y da eventos de colisión.
- Modo **Manual** (el `Move` de Unity) o **Integrado** (como el CharacterMovement de Unreal): gravedad, andar/correr/agacharse con aceleración, control en el aire, salto con **tiempo coyote**, **doble salto**, giro hacia donde anda y **teclado relativo a la cámara** (WASD, Mayús, Espacio, C/Ctrl).
- Lua: `entity:move`, `isGrounded`, `setMoveInput`, `jump`, `setCrouch`, `addCharacterVelocity`, `characterState`. *GameObject > Física > Personaje*.

## 1.9.0

### Ambiente y clima
- Nuevo componente **Ambiente** (`Environment`), como Enviro Sky de Unity: un solo objeto lleva el **clima** con transiciones suaves (despejado, nubes, cubierto, niebla, llovizna, lluvia, tormenta, nieve, ventisca...), la **hora y la fecha** (el sol y la luna siguen su recorrido real según la latitud), las **estaciones**, el **viento** global, los **rayos** con sus truenos y el **sonido ambiente**.
- **Lluvia y nieve** que caen de verdad (partículas en la GPU) y **nieve que se acumula** sobre el terreno, los modelos y la hierba, y se derrite dejando el suelo mojado. En otoño la vegetación amarillea y enrojece.
- Conduce lo que ya había (nubes volumétricas, niebla, humedad y charcos, hierba y árboles); funciona en el editor como vista previa. **Ventana > Ambiente (clima y hora)** con los botones de los climas.
- Lua y MCP: `set_weather`, `set_time`, `set_wind`, `lightning_strike`, `get_environment`.

### Océano FFT
- El océano usa ahora **oleaje FFT** (espectro JONSWAP en 4 cascadas de 128 × 128, como Crest), con **viento y fetch** que calculan la altura de las olas, **mar de fondo**, crestas afiladas y **espuma** que dura unos segundos y viaja con la ola. La flotación usa la misma superficie.
- Malla en **clipmap** alrededor de la cámara hasta el horizonte, sin grietas; a lo lejos el mar se ve mate y sin parpadeo.
- **Ríos** con la corriente en dos fases (*flow map*) y rápidos donde hay desnivel. **Bajo el agua**: absorción y luz dispersada, cáusticas, rayos de sol, partículas en suspensión, el menisco en la línea del agua y la superficie vista desde abajo con la ventana de Snell.

### Fuego
- Nuevo componente **Fuego** (`Fire`): **incendios que se propagan** por la hierba y el bosque, más rápido cuesta arriba y a favor del viento; los ríos, los lagos y la roca los frenan. Detrás quedan brasas, humo y el **suelo carbonizado** (terreno, modelos, árboles y hierba quemada).
- **Llamas y humo volumétricos**: lenguas de fuego que suben y una **columna de humo** que sube recta y el viento va tumbando con la altura, iluminada por el sol, el cielo y el resplandor de las llamas. Luces que parpadean iluminan los alrededores.
- El combustible sale de las capas del terreno. Lua y MCP: `fire_create`, `fire_ignite`, `fire_extinguish`, `fire_state`.

### Líquidos
- **Líquidos de partículas en la GPU**, como Obi Fluid o FleX: **agua, miel, lava, barro, sangre, aceite, ácido** y uno personalizado, con su viscosidad, cohesión y densidad. Componentes **Mundo de líquidos**, **Emisor** (grifo, caja, esfera) y **Desagüe**.
- Chocan con los colliders y el terreno, **empujan a los Rigidbody** (flotan según su densidad y los arrastra la corriente) y se dibujan como una **superficie continua** con refracción, absorción de color, reflejos, espuma y emisión (lava).
- **GameObject > Efectos > Líquidos** con un tanque de demostración. Lua (`Fluid`) y MCP: `fluid_create`, `fluid_spawn`, `fluid_state`, `fluid_clear`.

### Máquinas de estados (IA)
- **Máquinas de estados** como los State Graphs de Bolt: la IA se dibuja como un **grafo** de estados unidos por transiciones con condiciones (variables, triggers, temporizadores o una expresión Lua); en cada estado se programa en Lua solo lo que hace (`OnEnter`, `OnUpdate`, `OnExit`). Variables en una **pizarra** con tipo, propias de cada objeto.
- Se editan en su **propia pestaña a toda la ventana**, como los scripts. En Play se ven **en vivo**: el estado activo se ilumina, la última transición parpadea y las condiciones dicen si se cumplen; guardar recarga en caliente.
- Componente **StateMachine**, plantilla **IA con máquinas de estados** (enemigos que patrullan, persiguen, atacan, huyen y vuelven) y MCP: `create_state_machine`, `get_state_machine`, `update_state_machine`, `assign_state_machine`, `state_machine_debug`.

### Editor
- **Tema nuevo**: negros en capas, texto blanco, **rojo** para lo principal y la selección y **amarillo** para avisos.
- **Inspector rediseñado**: cada componente en una tarjeta con su icono, **buscador de propiedades**, menú ⋮ con **Copiar / Pegar valores**, presets y quitar, campos de referencia con el tipo de asset y ejes X/Y/Z de color.

### Corregido
- El Inspector dejaba **filas y cabeceras invisibles** (se veían huecos): el borde de las tarjetas, con esquinas redondeadas parciales, tapaba el panel.
- Los líquidos se veían como **bolitas** de cerca y con rayas en los bordes: suavizado en dos rondas con el radio en metros.
- El fuego se veía como un **bloque cortado**: las llamas eran un muro opaco del tamaño de la zona y el humo una losa naranja con bordes rectos.
- El humo salía **en diagonal hacia un lado** en vez de subir: ahora sube recto cerca del fuego y se tumba poco a poco.
- El color de los líquidos dejaba una **mancha por partícula**: el grosor y el material se suavizan con el mismo radio que la superficie.
- Un error de Lua dentro de un estado marcaba la **línea equivocada** (la del *traceback*, no la del error).
- El editor se **colgaba al salir del modo cine** (MCP `cinema`): el tamaño de la ventana se cambia ahora entre frames.

## 1.8.0

### Generador de terreno
- **Ventana > Generador de terreno**: un mundo entero en un paso, calculado en segundo plano (barra de progreso y *Cancelar*). Una isla de 2 km con 1025 vértices por lado tarda menos de un segundo.
- Formas: **isla, archipiélago, continente, cordilleras y cañones**. Relieve con fBm deformado (*domain warping*), crestas (*ridged multifractal*), colinas, mesetas y terrazas, y costa irregular.
- **Erosión** térmica (desmoronamiento por encima del ángulo de reposo) e **hidráulica por gotas** (Beyer/Lague), repartida en hilos: cauces, abanicos y sedimentos.
- **Agua**: océano, **ríos** que nacen arriba y bajan siguiendo el terreno hasta el mar (*priority-flood* y acumulación de flujo), con su cauce excavado, y **lagos** en las cuencas. Se crean como `WaterBody` normales (Océano, Río N, Lago N).
- **8 capas** pintadas por reglas (hierba, hierba seca, tierra, roca, arena, grava, nieve y barro) según la altura, la pendiente, la humedad y los sedimentos, con **texturas PBR procedurales** (color y normal) en `Assets/Terrains/Texturas`.
- Vegetación, hierba y aldea opcionales. La cámara del editor y la del juego alargan su distancia de dibujado según el tamaño del mundo (antes el terreno lejano desaparecía a 500 m).
- MCP: `generate_terrain` (todos los parámetros, más `houses`) y `ground_height`.

### Hierba en la GPU
- Nuevo componente **Grass** en el terreno: millones de briznas (hasta 3 millones) sin mallas. Un *compute shader* recorre una rejilla fija al mundo alrededor de la cámara, recorta por la vista y reparte en dos listas: **cerca con 7 tramos y lejos con 3** (más anchas y menos).
- Briznas con **perfil de hoja**, normales redondeadas, **matas** (las vecinas se parecen), rachas de viento que recorren el campo y **translucidez** a contraluz (Disney *subsurface*). La capa seca las amarillea.
- **Reacciona a la física**: los `Rigidbody` no estáticos cercanos (los 32 más próximos, hasta 120 m) la apartan y la aplastan.

### Árboles procedurales
- Los árboles de `Foliage` ya no son *low poly*: se generan como un SpeedTree sencillo, con **tronco y ramas de verdad** (hasta 3 niveles, filotaxia de 137,5°, gravedad, fototropismo y torsión), **racimos de hojas** con normales esféricas y translucidez, y **3 LOD** del mismo esqueleto.
- 6 especies (**pino, abeto, roble, abedul, palmera y sauce**); en *Especies* se elige cuál es cada una de las tres de la mezcla, su semilla, altura, hojas, ramas y lo torcidas que son.
- Texturas procedurales: **corteza de placas** (Voronoi estirado con surcos, fibras y musgo), abedul, **ramitas con 60-90 hojas pequeñas**, agujas y frondas, con mips que conservan la cobertura del alfa. Sombras con recorte por alfa.
- **No crecen en el agua**: se quitan del cauce y la orilla de los ríos y de lo que queda bajo el nivel de los lagos.

### Casas y cabañas procedurales
- **Ventana > Generador de casas**: **cabaña de troncos** (troncos redondos cruzados en las esquinas), **cabaña de tablas**, **casita de piedra** y **casa de campo** de dos plantas. Estilo, semilla, medidas, plantas, pendiente del tejado, alero, ventanas, porche con barandilla y escalones, chimenea y contraventanas.
- **Aldeas**: busca un sitio llano y seco del terreno (fuera de ríos, lagos y mar), reparte las casas alrededor de una plaza con la puerta hacia ella y abre claros en la vegetación. También desde el generador de terreno (apartado *Aldea*).
- **Para el juego**: 1.000-6.000 triángulos por casa con LOD automáticos; 9 materiales `.crmat` compartidos por todas (texturas PBR con altura para el parallax, creadas una vez en `Assets/Casas/Texturas`); las casas iguales son instancias del mismo modelo.
- Cada casa tiene los hijos **Casa** (`MeshCollider`) y **Puerta** (`BoxCollider`, con el origen en la bisagra: se abre girándola en Y desde Lua).
- MCP: `generate_house` (estilo, medidas, posición y giro, o `village` para una aldea).

### Agua
- El efecto de **bajo el agua** funciona con **todos los tipos**: océano, lagos y ríos. Solo se activa donde hay agua de verdad (dentro del lago, con el suelo por debajo de la superficie y la cámara por encima del fondo); en los ríos usa la superficie del punto donde está la cámara.

### Web y documentación
- **Documentación rediseñada** con el estilo de shadcn/ui: cabecera fija, menú lateral por secciones (en el móvil, un panel deslizante), buscador tipo *Command* con <kbd>Ctrl K</kbd> o <kbd>/</kbd>, "En esta página", migas de pan, tablas y bloques de código con botón de copiar, **modo claro y oscuro** y portada con tarjetas por sección.
- **Web nueva** con el mismo estilo: características en tarjetas, novedades de la 1.8 con capturas y las versiones anteriores en desplegables, y menú para el móvil.
- **Responsive** arreglado en la web y en el manual: nada se sale de la pantalla en el móvil.
- **Donaciones** con PayPal (https://paypal.me/evan2025) en la cabecera, en el menú lateral y en cada página del manual, y en la web.
- Nueva página del manual: **Mundo procedural**.

### Corregido
- Los troncos de los árboles se veían **negros con un borde brillante**: la normal de la corteza se invertía según la cara del triángulo. Las copas brillaban todas igual: menos translucidez y oclusión en el interior y la parte de abajo.

## 1.7.0

### Materiales de Disney
- **Modelo de Disney** (Burley 2012, *Physically Based Shading at Disney*, y 2015 para la transmisión), común al raster (`lighting.frag`) y al path tracing (`disney_brdf.glsl`).
- **Difuso de Burley** en todos los materiales, en lugar de Lambert: retro-reflexión en lo rugoso y bordes más oscuros en lo liso.
- Nuevo apartado **Modelo (Disney)** en el editor de materiales, con un modelo por material (como los *Shading Models* de Unreal):
  - **Barniz** (clearcoat): segunda capa especular dieléctrica con su rugosidad y su propio reflejo del entorno.
  - **Tela** (sheen): brillo de los bordes, con tinte del color; también con la luz del cielo.
  - **Piel / cera** (subsurface): difuso de Hanrahan-Krueger de Disney y **translucidez** a contraluz del sol, de las luces puntuales y de los focos. La sombra que se aplica es la de la cara de atrás a su **grosor**: una hoja fina brilla; un muro no deja pasar luz.
  - **Anisótropo**: GGX anisótropo con su visibilidad (Heitz) y el reflejo del entorno estirado (normal doblada), en la dirección de la tangente girada por el material.
  - **Transmisión** (vidrio, material transparente): refracción por **IOR**, **grosor**, tinte por color y **esmerilado** por rugosidad (12 muestras), en vez de mezclar por alfa.
  - **Tinte especular** de Disney en todos los modelos que lo admiten.
- G-buffer con un quinto destino (RGBA8: modelo y 3 parámetros); la dirección del anisótropo se guarda como ángulo sobre una base de la normal (Duff 2017).
- `.crmat`: `shading` y sus parámetros (`clearcoat`, `sheen`, `subsurface`, `translucency`, `anisotropy`, `ior`...). MCP `create_material` con los mismos campos. Se cambian en vivo.

### Corregido
- Error de validación de Vulkan de los mesh shaders de la 1.4 (`LocalSizeId` sin `maintenance4`).

## 1.6.0

### Relieve teselado
- Nuevo **Modo** del relieve en el editor de materiales: **Parallax** (como hasta ahora) o **Teselación**. Con teselación la malla se subdivide en la GPU (`skinned.tesc`/`.tese`) y cada vértice sube por su normal lo que dice el mapa de altura: silueta, sombras y oclusión reales.
- Cuánto se parte cada borde depende de los píxeles que ocupa (un vértice cada ~10 px), hasta la **Densidad** del material (1..64, 16 por defecto): de lejos casi no cuesta. Los parches fuera de la vista no se teselan. Los bordes compartidos se parten igual: sin grietas en mallas con normales suaves.
- **Sombras teseladas** del sol (cascadas) y de las luces locales, con el mismo reparto que ve la cámara (`skinned_shadow.tesc`/`.tese`).
- El relieve sube hacia fuera: la malla original queda debajo y el trazado de rayos no se tapa a sí mismo. Las cajas del culling crecen lo que sube el relieve.
- Sin normal map ni bump, en modo teselación la altura también hace de normal map. Si la GPU no tiene teselación, esos materiales usan el parallax.
- `.crmat`: `relief` (`parallax`/`tessellation`), `tessellation_density`, `parallax_shadows`. MCP `create_material` con los mismos campos.

### Parallax
- **Auto-sombra** (casilla en el material, activada por defecto): desde el punto que encuentra el parallax se traza hacia el sol, y las piedras que sobresalen dan sombra a las de al lado. Se guarda en el G-buffer junto a la metalicidad y solo afecta a la luz del sol.
- Los **shaders propios** (`.crshader`) con mapa de altura tienen el mismo parallax y la misma auto-sombra que el material estándar (código común en `parallax_common.glsl`).

### Donaciones
- Botón **Donar con PayPal** en el Hub (y tarjeta en *Aprender*) y en la web (menú, descarga, pie y manual): https://paypal.me/evan2025.

## 1.5.0

### Licencia
- **Nueva [Licencia del Motor Cramion](LICENSE)** en lugar de la MIT. Hacer juegos con Cramion y venderlos sigue siendo **gratis y sin regalías**; lo que ya no se permite es **vender o revender el motor, el editor o su código** (solos o en packs), resubir el editor a otras webs, quitar los créditos o hacerlo pasar por otro motor.
- **Código fuente bajo licencia**: deja de ser público. Compilarlo, modificarlo o usarlo requiere una licencia de código fuente, que se pide por correo a tupapienrakion1234@gmail.com. Para hacer juegos no hace falta: basta con el zip.
- **También la 1.4 y las anteriores** se distribuyen desde el 30 de septiembre de 2026 solo con esta licencia. Quien ya tuviera una copia con MIT la conserva, pero solo si mantiene intactos el copyright y la licencia; quitar los créditos deja la copia sin permiso.
- **[TRADEMARK.md](TRADEMARK.md)**: el nombre y el logo de Cramion solo se usan para el motor oficial y para decir "Hecho con Cramion Engine".
- **[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)** con todas las librerías de terceros y sus licencias; viaja en el zip junto a `LICENSE` y `TRADEMARK.md`.
- Web y manual: página **Licencia**, sección **Código fuente** en lugar de *Compilarlo*, y aviso de que la única descarga oficial es la de la web.
- Hub y actualizador: *Novedades* abre la web en vez de GitHub, y la tarjeta *GitHub* pasa a ser *Licencia*.

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

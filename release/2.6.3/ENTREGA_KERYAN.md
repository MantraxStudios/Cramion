# Entrega para Keryan: VFX, imagen y nitidez (Cramion 2.6.3)

Respuesta a «Cramion: fallos de VFX, imagen y nitidez que bloquean a Keryan» (21-10-2026). Base: 2.6.2, commit 2323d3e. Esta entrega cubre los apartados 1 a 5.

**Estado de la verificación**:
- Compilación: Windows (editor, player, herramientas y tests) y Android (arm64-v8a y armeabi-v7a), sin errores. Paquete `Cramion-win64.zip`, SHA-256 `c9104742bdf1ba2266c6a6ab08d12368940fde1723c17a3c4627deb22d9a5d85`.
- Tests: 61 de 62 suites pasan, incluidos los tests nuevos de presupuesto (jugador local, prioridad, tope, `effectFinished`), de `.crshader` con 16 propiedades y de `shader_values` con número suelto. En `CramionFeatures21Tests` siguen fallando las 2 comprobaciones antiguas de lightmaps, igual que en la 2.6.2.
- Pruebas de aceptación en el editor: no hechas todavía. Las capturas y medidas en la Vega 7 son vuestras.
- Coste en GPU: no medido en una Vega 7. No hay pases nuevos de pantalla completa para el TAA del motor; con FSR 3 se añade un pase R8 a la resolución interna. Pedimos vuestra medida, como indica el documento.
- Parches 0018, 0032 y 0045: ver el último apartado.

---

## 1. Nitidez de los VFX

### 1.1 Jitter del TAA en los VFX, las partículas, la lluvia y los sprites
| Archivo | Cambio |
|---|---|
| `CramionFX/include/CramionFX/vk/VulkanRenderer.h` | `jittered_projection_`: la proyección con jitter del frame |
| `CramionFX/src/vk/VulkanRenderer.cpp` | `updateUniforms` la guarda. `recordParticlePass` y `recordPrecipitationPass` usan `current_jittered_view_projection_`. Las matrices sin jitter siguen para el culling, las luces y la velocidad |
| `CramionFX/src/vk/VulkanRendererVfx.cpp` | `VfxView.projection` y `view_projection` con jitter. El depth también lleva jitter, así que las partículas suaves, la colisión y la distorsión quedan coherentes |
| `CramionFX/src/vk/VulkanRendererSprites.cpp` | Los sprites, con jitter |

### 1.2 Máscara reactiva / de transparencia
- **Diseño**: la máscara va en el alfa de `scene_color_` (la imagen HDR). En la vista principal ese canal no lo leía nadie; antes guardaba la distancia, que solo usa la captura de la sonda.
  - Por eso no hacen falta attachments ni pases nuevos para el TAA del motor.
  - En la vista principal (`GpuCamera.motion[2] = 1`) `lighting.frag` y el path tracing escriben 0.
  - Los transparentes acumulan su opacidad con `One, OneMinusSrcAlpha` en el alfa.

| Archivo | Cambio |
|---|---|
| `CramionFX/shaders/lighting.frag`, `path_trace_common.glsl` | Alfa 0 en la vista principal; la distancia, solo en las vistas aisladas (sonda) |
| `CramionFX/src/vk/VfxPass.cpp` (`createDrawPipeline`), `ParticlePass.cpp`, `PrecipitationPass.cpp`, `SpritePass.cpp`, `SkinnedPass.cpp` (`createSurfacePipeline`) | Mezcla del alfa «sobre» para acumular la máscara. Los `.crshader` transparentes escriben ahora también el alfa |
| `CramionFX/shaders/vfx.frag` | En la mezcla aditiva, el alfa lleva cuánto aporta el efecto |
| `CramionFX/shaders/taa.frag` | Lee la máscara, dilatada 1 px con los vecinos 3×3 que ya leía. Sube el peso del frame nuevo hacia `ReactiveWeight` y estrecha el recorte de 1,25σ a 0,75σ donde la hay |
| `CramionFX/include/CramionFX/vk/GpuTypes.h` | `GpuTaaPush.params` (112 → 128 bytes) |
| `CramionFX/shaders/upscaler_reactive.frag` (nuevo), `VulkanRenderer.cpp` (`upscaler_reactive_`, su pase en `recordVendorUpscale`) | R8 a la resolución interna, tope 0,9. Solo con FSR 3 |
| `CramionFX/include/CramionFX/vk/TemporalUpscalers.h`, `CramionFX/src/vk/TemporalUpscalers.cpp` | `UpscaleDispatch::reactive`, que se pasa a FFX como `reactive` y como `transparencyAndComposition` |

- **No escriben máscara**: el vidrio, el agua y el fuego, porque su mezcla usa el alfa de otra forma. DLSS no recibe máscara.

### 1.3 Samplers de VFX y Decals
| Archivo | Cambio |
|---|---|
| `CramionFX/include/CramionFX/vk/VfxPass.h`, `CramionFX/src/vk/VfxPass.cpp` | `setTextureFiltering`: anisotropía y `mipLodBias = upscaleMipBias`, como los modelos |
| `CramionFX/src/vk/VulkanRenderer.cpp` | Sampler de decals con el mismo filtrado. `applyTextureFiltering` rehace los dos al cambiar la calidad |

### 1.4 CVars del TAA
Están en `CramionCore/src/ecs/RenderSync.cpp` y se aplican con `VulkanRenderer::setTaaTuning`.

| CVar | Valor por defecto | Qué hace |
|---|---|---|
| `render.taa.FrameWeight` | 0,12 | Peso del frame nuevo; 0,12 es lo de antes |
| `render.taa.ReactiveWeight` | 0,5 | Peso del frame nuevo donde la máscara es 1 |
| `render.taa.ReactiveStrength` | 1 | 0 apaga la máscara, en el TAA y en FSR 3 |

### Cómo comprobarlo
- **Prueba**: la del documento. Calidad Recomendado y Alta, 1920 × 1080, con el TAA propio y con FSR 3.
- **Comparación A/B**: `render.taa.ReactiveStrength 0` desactiva la máscara.
- **Coste**: en el perfilador, los pases «TAA / escalado» y «FSR 3».

---

## 2. Presupuesto e instancias

| Archivo | Cambio |
|---|---|
| `CramionCore/include/CramionCore/vfx/VisualEffect.h` | `VisualEffect.local_player` y `priority`. Componente nuevo `VfxGroup` (`local_player`, `priority`). `Decision::Finished`. `effectFinished()` |
| `CramionCore/src/vfx/VisualEffect.cpp` | Ver el desglose de `planFrame` y del resto debajo |
| `CramionCore/src/scripting/native/CoreApi.cpp` | Argumento `triggers` tras `ignorar` en `Physics.raycast`, `sphereCast` y `overlapSphere`: `true`/`"collide"`, `false`/`"ignore"`, `nil` = Project Settings |
| `CramionCore/src/scripting/native/EffectsApi.cpp` | `entity().setEffectLocalPlayer(bool)` y `setEffectPriority(n)` |
| `CramionCore/sdk/cramion/Script.h`, `Api.gen.h`, `EntityApi.gen.inc` | `Physics::QueryTriggers` y los dos métodos nuevos en el SDK de C++ |
| `docs-src/pages/cpp-physics.html`, `cpp-entity.html`, `componentes-efectos.html`, `vfx-graph.html` | Documentación |
| `CramionCore/tests/Features21Tests.cpp` | Tests de jugador local, prioridad, tope y `effectFinished` |

**Cambios en `VisualEffect.cpp`**
- `planFrame` rehecho:
  - Agrupa por el `VfxGroup` más cercano hacia arriba. Si no hay ninguno, por la raíz del prefab (CVar `vfx.groupPrefabs`, activa por defecto). Esto último solo con Héroe, Combate e Impacto: Ambiente no se agrupa, para no juntar celdas enteras.
  - Cada grupo es un candidato con el tipo más importante de sus efectos. Todos sus efectos reciben la misma decisión.
  - Los efectos terminados no cuentan.
  - Los del jugador local cuentan como Héroe.
- `applyBudget`: lo esencial (jugador local) no se recorta por distancia, entra el primero y puede pasar del tope. El resto se ordena por prioridad y luego por distancia.
- Un efecto terminado no se simula y suelta su ranura. Si vuelve, empieza limpio.
- `playAt` con DontCreate cuenta grupos.

**Cuándo un efecto se da por terminado**
- No emite y no tiene eventos ni ráfagas pendientes.
- No tiene partículas vivas. Lo dice la lectura de la GPU, o, si no la hay, que haya pasado la vida máxima del bloque Lifetime.
- Lleva al menos 0,25 s sin que nazca nada.
- Nunca se dan por terminados los efectos en pausa ni los que escuchan canales de eventos de la GPU.

### Cómo comprobarlo
1. Poner `VfxGroup` en la raíz de cada habilidad, o dejar que agrupe por prefab.
2. Marcar las habilidades del jugador local con `setEffectLocalPlayer(true)`.
3. Encadenar 5 habilidades en 3 s, en Baja y en Alta, con `vfx.scalability` activo.
4. `vfx_control stats`: cada habilidad cuenta 1 y las terminadas salen de la cuenta.
5. `Physics.raycast(o, d, 100, nil, yo, false)` ya no toca los triggers.

**Riesgo**: si un `.crvfx` ata la vida de las partículas a un parámetro mayor que los valores del bloque, y además no hay lectura de la GPU, el efecto se daría por terminado antes de tiempo.

---

## 3. Topes internos

| Archivo | Cambio |
|---|---|
| `CramionFX/include/CramionFX/vk/VfxPass.h`, `CramionFX/src/vk/VfxPass.cpp` | Ver el desglose de texturas, mallas y avisos debajo |
| `CramionCore/include/CramionCore/vfx/VisualEffect.h`, `CramionCore/src/vfx/VisualEffect.cpp` | `syncResourceEpoch`: vuelve a pedir lo que se soltó. `clear()` (al entrar o salir de Play, o en otra escena) llama a `releaseUnused`. `resourceWarning(graph)` da el aviso para el editor |
| `CramionEditor/src/EditorEffects.cpp`, `EditorApp.h` | El aviso en amarillo en el editor del efecto. Al pasar el ratón, el uso de texturas y mallas frente al tope |
| `CramionEditor/src/EditorOrigin.cpp` | Al cambiar de escena, `vfx_.clear()` |

**Cambios en `VfxPass`**
- **Texturas**:
  - Hasta 256 ranuras, limitadas también por `maxPerStageResources`.
  - Cada ranura guarda su ruta, la fecha del archivo y el último frame en que se usó.
  - Sin sitio se suelta la menos usada, nunca la del frame actual ni la del anterior, con `retireAfterGpu`.
  - `releaseUnused()` suelta lo que no se usa.
  - Las rutas que fallan se reintentan a los 2 s.
- **Mallas**:
  - 128 ranuras.
  - El pool crece ×2 sin `waitIdle`, hasta 2 M vértices y 6 M índices. El límite por malla sigue en 65.536 vértices.
  - Los tramos libres se fusionan y no se reutilizan hasta 8 frames después.
- **Avisos**: `[VFX] AVISO: …` en la consola del editor, como mucho uno cada 30 s por cada causa.

### Cómo comprobarlo
- Más de 64 efectos con texturas distintas: ninguno sale sin textura.
- Más de 16 modelos de malla: todos se dibujan.
- Entrar y salir de Play varias veces: las mallas sin uso se sueltan.
- Si de verdad se llega al tope: aviso en la consola y en el editor del efecto.

---

## 4. Fallos de dibujo de efectos

| Id | Archivos | Cambio |
|---|---|---|
| E6 | `vfx.frag` | En la mezcla premultiplicada, alfa 0 con color ya no se descarta (salvo con recorte de alfa propio). La salida usa la opacidad sin la de la textura |
| E10 | `VisualEffect.h/.cpp` (`output.mesh_output_texture`, `modelLook`), `EditorEffects.cpp`, `EditorMcp.cpp` | La salida Malla sin `.crmat` usa el color y la textura de su modelo. Si la textura va dentro del archivo del modelo, solo su color, y avisa. La casilla «Textura de la salida en la malla» vuelve a lo de antes |
| E16 | `gbuffer_surface.glsl` (`surface_decal_root`), `grass.frag`, `VulkanRenderer.cpp` (teselas de decal +1 m), `RenderSync.cpp`, `VisualEffect.h/.cpp` (`grass_radius`) | Cada brizna toma el decal de su raíz. «Apartar hierba» en el VisualEffect se suma a los interactores de hierba que ya había (tope 32, por distancia) |
| E20 | `Components.h/.cpp` (`MeshRenderer.smooth_fade`), `Scene.h`, `VulkanRenderer.h/.cpp` (`recordGlassPass`), `EntityApi.cpp` (`setMaterialParam("smoothFade", …)`) | Con opacidad < 1 se dibuja mezclado en la pasada de transparentes. La sombra sigue con tramado. Por defecto, como antes |
| E4 | `vfx_sim.comp`, `VisualEffect.cpp` | «Atributos de cinta» tiene «Id del evento»: `id = (id + valor·tiras + tira) & 255`. Los bloques Emitir tienen «Valor = id de la partícula» |
| E3 | `vfx_sim.comp`, `VisualEffect.cpp` (`applyGround`), `RenderSync.h` (`terrainHeight`), `EditorEffects.cpp`, `PlayerMain.cpp` | «Colisión con la escena» tiene «Suelo sin la vista»: plano a la altura del terreno bajo el emisor. «Colisión con plano» tiene «En el mundo» y «Sobre el terreno» |
| E7 | `vfx_sim.comp`, `VisualEffect.cpp`, `vfx-graph.html` | KillBox documentado en el espacio del emisor; opción «En el mundo» (caja fija alineada con los ejes) |

**Límites conocidos**
- **E3**: el suelo es un plano a la altura del terreno bajo el emisor, no el relieve entero. Solo cuenta el terreno de Cramion, no las mallas.
- **E4**: hasta 256 ids por efecto.
- **E20**: mientras se funde, el objeto no escribe depth (pueden verse sus caras interiores) y sale con el sombreado del vidrio. Los `.crshader` opacos, con el shader estándar.

---

## 5. Herramientas

| Id | Archivos | Cambio |
|---|---|---|
| E1 | `CramionEditor/src/EditorProject.cpp`, `EditorEffects.cpp`, `VisualEffect.cpp` (`reloadAsset`) | La recarga en caliente incluye los `.crvfx` y los `.crmat` de las salidas Malla. Los efectos se reinician con lo del disco |
| E2 | `VfxPass.cpp` (fecha del archivo, `reloadTexture`), `VisualEffect.cpp` (`reloadTextureFile`), `DecalAtlas.h/.cpp` (`replace`), `VulkanRenderer.cpp` (`loadDecalTexture`, `startDecalTextureLoad`), `RenderSync.cpp` | Una textura de VFX o de decal cambiada en disco se relee en su misma ranura o capa, también tras `open_project` |
| E19 | `CramionCore/src/asset/MaterialAsset.cpp`, `MaterialTests.cpp` | Un número suelto vale como `[x]`. Lo que no vale se ignora con aviso `[Material]` |
| E11 | `SurfaceShader.h/.cpp`, `Model.h` (`kSurfaceParamCount`), `SkinnedModel.h`, `SkinnedPass.h`, `VulkanRenderer.h/.cpp`, `surface_common.glsl`, `RenderSync.cpp`, `ShaderTests.cpp` | 16 propiedades. Mensaje claro con el shader, la línea, la propiedad y el límite. La compilación síncrona ya no compila el shader que no se pudo leer, que era la causa del error de GLSL |
| E14 | `EditorApp.h`, `EditorCppScripts.cpp`, `EditorPhysics.cpp`, `EditorXr.cpp`, `EditorExport.cpp` | Compilación en segundo plano. Play espera sin bloquear y Stop lo cancela |
| E9/E15 | `EditorMcp.cpp` | `cpp_compile` responde `{status:"compiling", job}`. Herramienta nueva `cpp_compile_status`. `play` responde `mode:"starting"`. `sync:true` vuelve al modo anterior |
| E8 | `EditorApp.h/.cpp` (`forceDiscardUnsaved`), `EditorMcp.cpp` | Con `force`, `open_scene`, `new_scene`, `open_project` y `create_project` borran el autoguardado y no muestran el diálogo |

---

## Compatibilidad con los parches 0018, 0032 y 0045

- **0045**: no lo tenemos.
- **0018 y 0032**: solo tenemos la versión del paquete del 19-10, que ya no se aplica ni sobre 2323d3e sin cambios (`git apply --check` falla en `VulkanRenderer.cpp`, `RenderSync.cpp` y `MeshTests.cpp`). La versión que lleva vuestra rama está rebasada.
- **Lo que podemos asegurar**: esta entrega no toca la cadencia de las cascadas (0018) ni el LOD de los modelos (0032).
- **Zonas que sí comparten archivo con 0032**:
  - `VulkanRenderer.cpp`: E20 y el sampler de decals.
  - `RenderSync.cpp`: E16, CVars del TAA y E11.
- **Para confirmarlo**: necesitamos los tres parches tal como están en `keryan-parches-262` y os devolvemos el resultado.

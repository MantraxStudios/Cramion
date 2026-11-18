# RM-capas: capas de render, viewmodel, contornos de juego y mascara reactiva de DLSS

Rama `rm-capas` (desde `horizonz-1031`). Todo lo nuevo esta apagado o en su valor neutro por defecto: las escenas,
prefabs, `.crdata` y juegos de antes se ven igual (MeshRenderer en la capa 0, camaras y luces con "todas", sin
viewmodel, sin Outline). La unica diferencia por defecto es la mascara reactiva en DLSS (CVar `render.DlssReactiveMask`,
se apaga con 0).

## Entrada para CHANGELOG.md

### Capas de render, arma en primera persona y contornos de juego
- **Capas de render** (como las Rendering Layers / culling mask de Unity y el `cull_mask` de Godot): `MeshRenderer.render_layers`
  (mascara de 32 bits, por defecto la capa 0) con los nombres de las capas del proyecto (las mismas 32 de
  *Proyecto > Capas* / `Physics.json`, como en Unity). `Camera.culling_mask` (por defecto todas) decide que capas dibuja
  cada camara: la vista principal, las Render Textures y, con ellas, las sondas de reflexion (la captura reutiliza lo que
  ve la camara principal, sin el viewmodel). Lo que una camara no ve sigue proyectando su sombra (como "Solo sombras").
- **Sombras por capas**: `Light.shadow_layers` (por defecto todas) en el sol, las puntuales y los focos: cada luz solo
  dibuja en su mapa la sombra de los actores de esas capas. Cambiar las capas de una luz local rehace su mapa en cache.
- **Arma en primera persona (viewmodel)**: modo de la `Camera` (`viewmodel_layers`, `viewmodel_fov`, `viewmodel_near`,
  `viewmodel_depth`). Los actores de esas capas se dibujan con su propio campo de vision y comprimidos en
  `[0, viewmodel_depth]` del rango de profundidad (el `glDepthRange` del arma de Quake, el First Person Rendering de UE5):
  no atraviesan paredes, van al G-buffer como los demas (luz normal, sombras recibidas en su sitio junto a la camara) y sus
  vectores de movimiento son los de su transform real del frame anterior (TAA/FSR 3/DLSS sin estela). No proyectan sombra,
  no estan en la escena de rayos ni en las sondas. Tambien en modo compatible (van por el camino de CPU). En VR y en
  camaras ortograficas no se aplica.
- **Contornos de juego**: componente **Outline** (*Renderizado > Contorno*): color, opacidad, grosor en pixeles, *Siempre /
  Solo tapado / Solo visible*, relleno de la parte tapada (silueta a traves de paredes de un MOBA), pulso (Hz y minimo) e
  *Incluir hijos* (personajes por piezas). Funciona en el juego exportado y en la vista Juego (no es el contorno de
  seleccion del editor): mascara propia con la silueta entera y la parte visible (como el Custom Depth/Stencil de UE5) y
  composicion sobre la imagen final (`game_outline.frag`, hasta 15 estilos distintos a la vez). Se dibuja tambien en las
  Render Textures; no en las capturas de sondas ni en Wireframe.
- **DLSS con mascara reactiva**: la misma mascara que ya recibian FSR 3 y el TAA propio (transparentes, particulas,
  reflejos nitidos) llega a NGX como *Bias Current Color Mask* (`pInBiasCurrentColorMask`; `pInTransparencyMask` sigue
  reservada en el SDK 310.9). CVar `render.DlssReactiveMask` (1 por defecto).
- Los Mesh Renderer con capas distintas de la 0 no entran en el lote estatico del juego exportado (el lote es de la capa 0).
- Las escenas de antes cargan igual (sin los campos nuevos: capa 0, culling mask y sombras "todas", sin viewmodel).
  Pruebas: `CramionRenderLayersTests`.

## Archivos tocados

CramionFX
- `include/CramionFX/scene/RenderLayers.h` (nuevo): `layersVisible`, matriz y escalas del viewmodel, `OutlineStyle`,
  `GameOutlines` (empaquetado de push constants), pulso.
- `include/CramionFX/scene/Camera.h`: `setCullingMask/cullingMask`, `CameraViewmodel` + `setViewmodel/viewmodel`.
- `include/CramionFX/scene/Scene.h`: `Actor::layers`.
- `include/CramionFX/scene/Light.h`: `shadow_layers` en sol, puntual y foco.
- `include/CramionFX/scene/LocalLightShadows.h`, `src/scene/LocalLightShadows.cpp`: capas en `SpotShadow`/`PointShadow` y
  en la firma (cambiarlas rehace el mapa).
- `include/CramionFX/vk/VulkanRenderer.h`, `src/vk/VulkanRenderer.cpp`: culling por capas en `updateActors` (fuera de la
  mascara = `shadows_only`), viewmodel (`ActorDraw::viewmodel`, transform, fuera de los lotes de la GPU, de las sombras y
  de la TLAS; `drawViewmodelActors` con el viewport comprimido antes de `drawCpuActors`), mascara de capas en
  `recordActorShadows` (cascadas con el sol, luces locales con su `ShadowJob::layers`), `recordGameOutlinePass` +
  `setGameOutlines`, `setDlssReactiveMask` y la mascara reactiva tambien con DLSS en `recordVendorUpscale`.
- `include/CramionFX/vk/SkinnedPass.h`, `src/vk/SkinnedPass.cpp`: pipelines de la mascara de los contornos de juego.
- `include/CramionFX/vk/TemporalUpscalers.h`, `src/vk/TemporalUpscalers.cpp`: `pInBiasCurrentColorMask` en NGX.
- `shaders/game_outline_mask.frag`, `shaders/game_outline.frag` (nuevos).

CramionCore
- `include/CramionCore/ecs/Components.h`, `src/ecs/Components.cpp` (bloques localizados): `MeshRenderer.render_layers`,
  `Light.shadow_layers`, `Camera.culling_mask` y `viewmodel_*`, su `reflect()` y el registro de `Outline`.
- `include/CramionCore/ecs/Outline.h`, `src/ecs/Outline.cpp` (nuevos): componente Outline.
- `include/CramionCore/ecs/RenderLayers.h`, `src/ecs/RenderLayers.cpp` (nuevos): `applyCameraLayers`, `outlineStyle`,
  `collectOutlines`, `renderLayerMask`.
- `include/CramionCore/ecs/RenderSync.h`, `src/ecs/RenderSync.cpp`, `src/ecs/RenderSyncLayers.cpp` (nuevo): capas del
  actor, camara principal y de Render Textures, `shadow_layers` de las luces, `syncOutlines` (y la CVar de DLSS).
- `src/ecs/StaticBatching.cpp`: fuera del lote si `render_layers != 1` o lleva contorno (Outline propio o de un padre
  con *Incluir hijos*).
- `CMakeLists.txt` (CramionCore): objetivo `CramionRenderLayersTests` y `src/ecs/Outline.cpp` en las fuentes de
  `CramionCoreTests` (Components.cpp registra Outline).
- `tests/RenderLayersTests.cpp` (nuevo).

No se tocaron `CoreApi.cpp`, `Api.gen.h`, `Script.h` ni el SDK.

## CVars

- `render.DlssReactiveMask` (bool, 1, guardada): DLSS recibe la mascara reactiva.

## API nueva

- Componentes/campos (Inspector, `.crscene`, `set_component` por MCP y `setField` de los scripts, sin API nueva):
  `MeshRenderer.render_layers`, `Camera.culling_mask`, `Camera.viewmodel_layers/viewmodel_fov/viewmodel_near/
  viewmodel_depth`, `Light.shadow_layers`, componente `Outline` (`enabled, color, opacity, width, mode, occluded_fill,
  pulse_speed, pulse_min, include_children`). Las mascaras se pueden construir en scripts con `Physics.layerMask("Default",
  "Arma")` (son las mismas capas con nombre).
- C++ del motor: `scene::Camera::setCullingMask/setViewmodel`, `VulkanRenderer::setGameOutlines/setDlssReactiveMask/
  viewmodelDraws`, `ecs::applyCameraLayers/collectOutlines/outlineStyle/renderLayerMask`.

## Pruebas

`CramionRenderLayersTests` (nueva, sin GPU): valores por defecto, guardar/leer y escena de antes sin los campos,
culling por capas y mascara por nombres, matriz del viewmodel (misma posicion en pantalla que con su campo de vision,
fuera del plano cercano, pegado a la camara al moverla), empaquetado de estilos (128 bytes, 15 estilos, grosor/modo/
relleno), pulso, recogida de Outline del mundo (hijos, apagado, inactivo) y firma de las sombras locales por capas.
Resultado (build-rm, Release, clang): `CramionRenderLayersTests` 53/53, y sin regresiones en `CramionCoreTests` 102/102,
`CramionPrefabTests` 21/21 y `CramionSceneLoadTests` 43/43. Compilados: CramionFX, CramionCore y esas pruebas (no se
compilaron CramionEditor ni CramionPlayer: no se toco su codigo, pero incluyen las cabeceras cambiadas; compilarlos en la
integracion).

Los shaders nuevos se compilan con glslc al construir CramionFX.

## Banco / escena de prueba (a mano, con GPU)

Escena de prueba sugerida (proyecto vacio con suelo y un par de cubos):
1. *Proyecto > Capas*: nombrar la capa 8 "Arma".
2. Un cubo alargado hijo de la Camara principal a (0.25, -0.2, -0.5) con `render_layers` = Arma; en la Camara
   `viewmodel_layers` = Arma, `viewmodel_fov` = 55. Acercarse a una pared: el arma no la atraviesa. Con TAA, FSR 3 y DLSS,
   girar rapido: sin estela en el arma. Bajo una sombra el arma se oscurece.
3. Un personaje con `Outline` (modo *Solo tapado*, relleno 0.35) detras de una pared: se ve su silueta a traves. Probar
   *Siempre* / *Solo visible*, el grosor y el pulso; exportar el juego y comprobar que salen.
4. Un objeto en la capa 9 y la Camara con `culling_mask` sin la 9: no se ve, su sombra si. Un sol con `shadow_layers` sin
   la 9: ya no hay sombra. Lo mismo con un foco y una puntual.
5. Una camara con Render Texture y otra culling mask: en la pantalla de la textura se ve otra cosa.
6. Repetir 2-4 con el modo compatible (`--compat`/movil).
7. DLSS en una RTX con particulas/vidrio delante: menos estela; `render.DlssReactiveMask 0` para comparar.

## Pendiente

- Capas en particulas/VFX, terreno, vegetacion, voxeles, agua y sprites (siguen visibles para todas las camaras; solo
  los Mesh Renderer y personajes llevan capas). Luces por capas de render que iluminan ("light layers") tampoco.
- Mascara de capas propia para las sondas de reflexion (hoy usan la de la camara principal sin el viewmodel).
- El trazado de rayos ignora las mascaras de sombra por luz (las sombras por rayos usan `cast_shadows`).
- Viewmodel sin sombra propia (opcion para que proyecte) ni en VR.
- Contornos de juego: sin contorno del viewmodel; maximo 15 estilos por frame (los demas comparten el ultimo).
- Editor: el Inspector muestra las mascaras con los nombres de las capas del proyecto (el mismo widget que la fisica);
  no hay vista previa de la culling mask en la vista Escena.

## Riesgos

- El culling por capas se hace en `updateActors` marcando `shadows_only`: cualquier ruta que dibuje actores sin mirar
  `shadows_only` los veria (revisadas: G-buffer, vidrio, contornos, picking y sombras; todas lo miran).
- El viewmodel se ilumina en la posicion reconstruida de la profundidad comprimida (junto al plano cercano): SSAO, sombras
  de contacto y niebla lo ven pegado a la camara.
- Mascara reactiva de DLSS activa por defecto: si en algun juego empeora (parpadeo en transparentes), `render.DlssReactiveMask 0`.

## Revision (segundo agente)

Revisado el diff `horizonz-1031...rm-capas` (41af132). Lo pedido esta cubierto (con las limitaciones de *Pendiente*).
Retrocompatibilidad: los campos nuevos tienen el valor neutro por defecto y una escena sin ellos carga igual (prueba);
la camara de la vista Juego del editor se guarda y se devuelve entera (`saved_camera_`), asi que la culling mask y el
viewmodel no pasan a la vista Escena; ningun script de Horizonz usa el nombre `Outline`. Sin coste con la funcion sin
usar: una mascara por actor y luz, `view<Outline>().empty()` por frame y la pasada de contornos solo con alguno.

Arreglado:
- **Vidrio del viewmodel** (`recordGlassPass`): lo transparente del arma (mira, cristal) y su fundido suave se dibujaban
  sin comprimir la profundidad y en su posicion "real", que la matriz del viewmodel aleja hasta `near_camara /
  viewmodel_near` veces (x10 con los valores por defecto): cualquier pared a pocos metros lo tapaba, el culling del vidrio
  en GPU lo daba por oculto y el vidrio del mundo se mezclaba encima. Ahora va con el mismo viewport comprimido, el
  ultimo en el orden y con una caja de oclusion que nunca se da por tapada.
- **Contornos "Solo tapado" con ruido**: la mascara de lo visible se dibujaba siempre con LOD0 y sin jitter contra la
  profundidad del G-buffer (con LOD y con jitter, y con escalado contra la copia a resolucion de pantalla): partes que se
  ven salian "tapadas" (contorno y relleno a trozos en personajes lejanos con LOD y en superficies inclinadas con
  TAA/FSR/DLSS). Ahora usa el LOD que dibujo el G-buffer (con sus cajas por submalla) y la pipeline de lo visible de los
  contornos de juego lleva un margen de profundidad hacia la camara (depth bias -1 / pendiente -2.5). El contorno de
  seleccion del editor no cambia.
- **Cascadas del sol al cambiar `Light.shadow_layers`**: con la camara quieta y sin nada que se moviera las cascadas (y
  su cache de lo estatico) no se volvian a dibujar nunca, y la sombra de las capas quitadas seguia. Ahora un cambio de
  las capas del sol redibuja todas las cascadas (como un cambio del detalle de sombras).
- **Static batching y Outline**: una entidad Static con Outline (o hija de una con *Incluir hijos*) entraba en el lote
  del juego exportado y perdia su contorno (va por actor). Ahora se queda suelta.
- Quitado `game_outline_mask_` (imagen declarada y nunca usada).

Pruebas anadidas: en `CramionMeshTests` (static batching), capas y contornos fuera del lote (capa 8, Outline, hijo con
*Incluir hijos*; el hijo de un Outline sin el si entra).

Resultado tras la revision (build-rm, Release, clang; CramionFX y CramionCore recompilados, sin avisos nuevos):
`CramionRenderLayersTests` 53/53, `CramionMeshTests` 183/183 (con las 2 nuevas), `CramionCoreTests` 102/102,
`CramionPrefabTests` 21/21, `CramionSceneLoadTests` 43/43, de una en una y sin ventana. CramionEditor y CramionPlayer
siguen sin compilar en este worktree (no se toco su codigo; incluyen `VulkanRenderer.h`, que cambio): compilarlos en la
integracion.

Revisado y correcto (sin cambios): vectores de movimiento del viewmodel (las matrices del frame anterior se guardan con
`draw.transform`, que ya lleva la matriz del viewmodel), rigidos del viewmodel en modo compatible (sin el bit de
instancia: `push.model` x hueso), barreras de la mascara compartida con el contorno de seleccion (la segunda pasada
espera la lectura de la primera), mascara reactiva de DLSS (misma imagen, barrera y formato que FSR 3), camaras de
Render Texture (su culling mask y su viewmodel, `camera_override_`).

Sigue pendiente de verificar en GPU (no se pudo sin ventana): todo lo de *Banco de prueba*, y en particular el margen de
profundidad del modo *Solo tapado* (si deja ver como visible algo tapado por una pared pegada, bajar la pendiente).

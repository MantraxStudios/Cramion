# NOTAS-RM-anim: animacion de juego (capas, root motion, eventos, montages, ventana de clips)

Rama `rm-anim` (desde `horizonz-1031`). Bloque ANIMACION del ROADMAP (3.2: "Animator con capas, mascara de huesos y aditivas", "Root motion", "Eventos (notifies) y curvas en los clips, con ventana de clips", "Montages / slots").

## Entrada para el CHANGELOG (estilo 2.7.x)

### Animacion de juego: capas, aditivas, root motion, eventos, curvas y montages
- **Capas en el Animator Controller** (como las de Unity): ademas de la maquina de siempre (la capa base), `layers` con su propia maquina de estados, **peso**, modo **Override** o **Aditiva**, **mascara de huesos** (lista de huesos con o sin hijos, o un asset reutilizable `.crmask`) y opcion de **sincronizarse** con otra capa (sigue su estado y su ciclo con otros clips). Las aditivas restan la pose de referencia: el primer fotograma de cada clip o un clip y un instante elegidos. Las capas mezclan con fundido cruzado (la inercializacion sigue siendo de la base). Un controlador sin capas se guarda y funciona exactamente igual que antes (version 1 del `.cranimator`; con capas, version 2 con la clave `layers`).
- **Root motion** por Animator (`root_motion`: *Apagado* por defecto, *Transform*, *Character Controller*, *Rigidbody* o *Script*; `root_bone`, vacio = el hueso mas alto del esqueleto). Se saca de la raiz el avance en XZ y el giro en Y (relativos al primer fotograma de cada clip, con la escala y el giro del nodo padre, p. ej. el `Armature` 0.01 de un FBX) y el personaje anima en el sitio. Funciona con Blend Trees, fundidos, capas y montages (un montage de cuerpo entero manda en el movimiento segun su peso); en bucle cuenta las vueltas y hacia atras con velocidad negativa. En Play mueve el Transform, llama a `move()` del Character Controller o pone la velocidad del Rigidbody (la entidad o el padre mas cercano que lo tenga). En modo *Script* llega `OnAnimatorMove(deltaPosicion, deltaGiroGrados)` y lo aplica el script, como en Unity.
- **Eventos (notifies), curvas y secciones en los clips**. Se guardan junto al clip sin tocar el `.crdata`: en la cabecera del `.cranim` (clave `meta` con su `version`; los motores de antes la ignoran) o, para los clips de dentro de un modelo, en `<modelo>.crdata.crclipmeta`. Los eventos llegan a los scripts como el mensaje `OnAnimationEvent {name, clip, params, time, layer}` (`Script::onAnimationEvent(nombre, clip, params)`; `params` es texto o JSON) **una sola vez** aunque el frame salte varias vueltas; al entrar en un estado suenan los del primer instante; en un Blend Tree solo los del clip que mas pesa y en un fundido solo los del estado que entra. Tambien con **Motion Matching** (capa -2). Las curvas float se leen con `entity().animatorCurve("nombre")` (media por pesos de lo que suena).
- **Montages** (`entity().playMontage("Golpe", {slot, blendIn, blendOut, speed, section})`): un clip puntual sobre el grafo, en el cuerpo entero o en el slot de una capa (usa su mascara), con fundido de entrada y salida, **secciones** (con salto a otra o bucle) y eventos; al acabar vuelve solo al grafo. `stopMontage`, `montageJumpToSection`, `isMontagePlaying`, `montageTime`, `montageSection`. Tambien sin Animator Controller.
- **Capas en la ventana Animator**: selector de capa (Base o las de encima, + y x) con su panel (nombre, peso, Override/Aditiva, clip de referencia, sincronizar con otra capa y huesos de la mascara); el grafo edita los estados y transiciones de la capa elegida.
- **Ventana de clips** (Ventana > *Clips de animacion (eventos)*): elige un clip del objeto animado seleccionado, lo reproduce o recorre con la linea de tiempo sobre el propio modelo en la Escena (vista previa: `AnimatorRuntime::preview_clip`), marca eventos (doble clic), los arrastra (al fotograma, con FPS), edita nombre, parametros y tiempo, anade secciones y guarda en el `.cranim` o en el archivo lateral del modelo.
- API de scripts nueva (Entity): `setLayerWeight`, `layerWeight`, `layerState`, `crossFadeLayer`, `playMontage`, `stopMontage`, `montageJumpToSection`, `isMontagePlaying`, `montageTime`, `montageSection`, `animatorCurve`, `rootMotionDelta`, `setRootMotion`. Mensajes `OnAnimationEvent` y `OnAnimatorMove`.

## Archivos tocados

Nuevos:
- `CramionCore/src/ecs/RenderSyncAnim.cpp`: capas de encima, montage, eventos y curvas sobre las muestras de la base; mascaras en cache; nodo raiz; metadatos de los clips de los modelos.
- `CramionCore/src/scripting/native/AnimApi.cpp`: API de scripts, entrega de `OnAnimationEvent` y aplicacion del root motion (fase Events, solo en Play).
- `CramionEditor/src/EditorClips.cpp`: ventana de clips.
- `NOTAS-RM-anim.md` (este archivo).

Modificados:
- `CramionFX/include/CramionFX/asset/Model.h`: `ClipEvent`, `CurveKey`, `ClipCurve`, `ClipSection` y sus listas en `AnimationClip` (solo en memoria; no van en la cache ni en el `.crdata`).
- `CramionFX/include/CramionFX/anim/Animator.h`, `CramionFX/src/anim/Animator.cpp`: `ClipSample` con `delta` y `loop` (por defecto 0/true: lo de antes no cambia), `LocalPose`, `RootMotion`, `LayerBlendMode`, `PoseLayerJob`, `LayeredPosePlan`; `samplePose`, `restPose`, `applyPose`, `evaluateLayered`, `setRootMotionNode`/`rootMotion`; libres `clipRootMotion`, `rotateYaw`, `blendPoseOverride`, `makeAdditivePose`, `applyAdditivePose`, `collectClipEvents`, `ClipCurve::sample`. `evaluateBlend` se partio en `gatherActive` + `blendNode` con la misma cuenta (prueba: mismo resultado que `evaluate()`).
- `CramionCore/include/CramionCore/ecs/AnimatorController.h`, `src/ecs/AnimatorController.cpp`: mascaras (`BoneMaskEntry`, `BoneMask`, `.crmask`, `buildMaskWeights`), `AnimatorLayer` y `AnimatorController::layers`, guardado/lectura de capas, `stepStateMachine` (la base usa la misma funcion: `stepAnimatorController` la llama), `animatorStateSamples`, `stepAnimatorLayer`, `animatorLayerWeight`, runtime de capas (`AnimatorLayerRuntime`), montages (`AnimatorMontage`, `startMontage`, `stepMontage`, `clipSectionAt`), eventos pendientes (`AnimatorFiredEvent`, tope 64), curvas, root motion pendiente, vista previa, `RootMotionMode`, `ClipMeta` (cabecera del `.cranim` y archivo lateral), edicion de eventos, linea de tiempo y reproductor de la ventana. `.cranim`: la meta se escribe en la cabecera solo si el clip tiene algo.
- `CramionCore/include/CramionCore/ecs/Components.h`, `src/ecs/Components.cpp`: `Animator::root_motion` y `root_bone` (campos nuevos con valor por defecto *Apagado*; las escenas de antes cargan igual).
- `CramionCore/include/CramionCore/ecs/RenderSync.h`, `src/ecs/RenderSync.cpp`: ganchos localizados en `syncActors` (muestras con `delta`, `buildAnimationLayers`, pose aplazada 3 = `evaluateLayered` en paralelo, vista previa, root motion acumulado en `finish_actor`), precarga de los clips de las capas, limpieza al cambiar de escena.
- `CramionCore/src/ecs/RenderSyncMotion.cpp`: eventos del clip del Motion Matching.
- `CramionCore/include/CramionCore/asset/AssetTypes.h`, `src/asset/AssetDatabase.cpp`: tipo `BoneMask = 70` (`.crmask`).
- `CramionCore/src/scripting/native/Modules.h`: `registerAnimApi`.
- `CramionCore/sdk/cramion/Script.h`: `onAnimationEvent` y `onAnimatorMove` (desde `onMessage`).
- `CramionCore/sdk/cramion/EntityApi.gen.inc`: regenerado con `cramion_sdkgen` (solo las 13 funciones nuevas de Entity). `Api.gen.h` NO se toca: al regenerarlo salian cambios ajenos (el de `horizonz-1031` ya estaba desfasado: `Game.isEditor`, `Input.getTouchLayout`...); que lo regenere la integracion.
- `CramionEditor/src/EditorApp.h`, `EditorApp.cpp`: estado y menu de la ventana de clips y la capa elegida del Animator (pocas lineas).
- `CramionEditor/src/EditorAnimator.cpp`: panel de capas y edicion del grafo de cada capa.
- `CramionCore/tests/AnimationTests.cpp`: pruebas nuevas.
- `CramionCore/tests/native/AnimApiTests.cpp` (nuevo, objetivo `CramionApiAnimApiTests`): la API de scripts del modulo.

## CVars

Ninguna nueva: todo lo que cambia el comportamiento va apagado por defecto (root motion *Apagado*, sin capas, sin montage, sin eventos). `anim.ParallelPoses` tambien reparte la pose por capas.

## Pruebas

- `CramionAnimationTests` (suites nuevas *Root motion*, *Capas, mascaras y aditivas*, *Eventos y curvas de los clips*, *Capas del Animator Controller*, *Montages*, *Eventos y curvas en archivos*; las de antes intactas): **216 comprobaciones, 0 fallos**.
- `CramionApiAnimApiTests` (nueva: capas, montages, curvas, `OnAnimationEvent` una vez, root motion Transform/Script/apagado, registro y documentacion): **26 comprobaciones, 0 fallos**.
- `CramionApiEntityApiTests` (la de antes, por el componente Animator): **131 comprobaciones, 0 fallos**.
- Compilan tambien `CramionEditor` (ventana de clips y panel de capas) y `cramion_sdkgen`. No se abrio el editor ni Play: lo visual queda para probar a mano.

## Pendiente

- Ventana Animator: el estado "en vivo" solo se resalta en la base (en una capa el grafo se ve sin vista en vivo); no hay selector de `.crmask` (se quita, pero se asigna a mano en el JSON).
- Editor de `.crmask` (crear desde el Proyecto) y editor de curvas en la ventana de clips (se leen y se guardan; la ventana solo las cuenta).
- Recarga en caliente de `.crmask` y del archivo lateral cuando cambia fuera del editor (el editor llama a `reloadClipMeta()` al guardar).
- `DataPack`/exportacion: el archivo lateral `<modelo>.crdata.crclipmeta` no lo recoge el empaquetado por referencias; si el juego exportado copia la carpeta Assets entera, va. Lo seguro hoy: eventos en clips `.cranim`.
- Root motion junto con el Motion Matching activo en la misma entidad (el MM tiene su propio `strip_root_motion`): no combinarlos.
- Transiciones inerciales con capas: la inercializacion se calcula con la base; las capas se suman despues (se ve bien en las pruebas de logica, falta mirarlo en GPU).

## Riesgos

- `AnimationClip` gana tres vectores (vacios): copias de modelos un poco mas grandes en memoria (nada si no se usan).
- `AssetTypes.h` (contrato compartido) gana `BoneMask = 70`: si otro bloque usa el 70, cambiar el numero al fusionar.
- `Script.h`: dos virtuales nuevos; un script que ya tuviera un metodo `onAnimationEvent`/`onAnimatorMove` con otra firma los ocultaria (aviso del compilador, nada mas).
- Con root motion en modo Transform sobre un hijo de la raiz del modelo se mueve solo ese hijo: usar el modo Character Controller / Rigidbody (buscan en los padres) o Script.
- Con Escena y Juego visibles y el root motion activo fuera de Play, el desplazamiento se acumula y se descarta (tope de 1000 m y se limpia al empezar Play).

## Que probar a mano (GPU / editor)

1. Personaje Mixamo con un controlador con una capa *Torso* (mascara `Spine` con hijos) que reproduce un saludo mientras la base corre: las piernas siguen corriendo y el torso saluda; `setLayerWeight("Torso", 0..1)` lo funde.
2. Capa aditiva con un clip de "respirar"/inclinarse sobre andar y correr.
3. Root motion: un clip de ataque con avance (sin "in place") en modo Character Controller: avanza lo que el clip, sin patinar, y choca con paredes; con modo Transform en un objeto sin padre; con Script y `onAnimatorMove`.
4. Ventana de clips: elegir un clip, reproducir, arrastrar el cabezal (el modelo de la Escena sigue), doble clic para un evento, arrastrarlo, guardar; en Play, `onAnimationEvent` llega una vez por paso (tambien con el juego a pocos FPS).
5. `playMontage("Golpe", {slot = "Torso"})` mientras corre y `playMontage("Esquiva")` de cuerpo entero con root motion; secciones con bucle y `montageJumpToSection`.
6. Rendimiento con 100+ personajes sin capas: debe quedar igual (camino rapido sin meta) y con capas, la pose sigue en paralelo.

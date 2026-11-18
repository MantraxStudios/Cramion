# NOTAS rm-scripts (bloque SCRIPTS de la tanda ROADMAP, 18/11/2026)

Rama `rm-scripts` desde `horizonz-1031` (102bd30). Tres partes del ROADMAP: dibujo de depuracion
desde scripts, corrutinas y temporizadores de juego, y orden de ejecucion determinista de los
scripts de C++.

## Entrada para el CHANGELOG (estilo de CHANGELOG.md)

### Scripts: dibujo de depuracion, corrutinas y orden de ejecucion
- **Dibujo de depuracion desde los scripts** (como `Debug.DrawLine` de Unity; `scripting/DebugDraw.h`): `Debug.drawLine`, `drawRay`, `drawSphere`, `drawBox` (con giro: Quat o Vec3 en grados), `drawArrow` y `drawText` (texto en el mundo), con color (Vec3, `{r, g, b, a}`, `"#RRGGBB[AA]"` o un nombre: `"rojo"`, `"red"`...), duracion en segundos del juego (0 = un frame) y si se ve a traves de la geometria (`profundidad`; el texto, por defecto, a traves). Ademas `Debug.drawClear()` y `Debug.drawEnabled`. En Visual Scripts (nodo de llamada a la API), consola y C++ (`Debug::drawLine(a, b, Color(1, 0, 0), 2.0f)`, con tipos, en `Script.h`).
- Se ve en el **Play del editor**, en las vistas de **Escena y Juego**, y en **CramionPlayer** con la CVar `debug.draw.InGame` (apagada: un juego exportado no recoge nada, coste cero). Va por el pase de lineas de los gizmos (`OverlayPass`, con prueba de profundidad: lo tapado no se ve) mediante `VulkanRenderer::setDebugGeometry`; no sale en las RenderTexture ni en las capturas. El texto es una letra de trazos propia (`StrokeFont.h`: cifras, A-Z, puntuacion; tildes y enie como la letra base), de alto constante en pixeles.
- Tope de primitivas vivas: `debug.draw.MaxLines` (65536 segmentos; una esfera son 72) y `debug.draw.MaxTexts` (512); lo que pasa se descarta con un aviso por partida. Grosor: `debug.draw.LineWidth` (2 px). Lo dibujado envejece al empezar cada frame (solo una vez dibujado: lo de `fixedUpdate` tambien se ve) y se borra al parar.
- **Corrutinas y temporizadores en los scripts de C++** (`cramion/Coroutine.h`, como Unity): `startCoroutine(tarea())` con `Task<>` / `Task<T>`, `co_await waitSeconds(s)` (tiempo del juego), `waitSecondsRealtime(s)`, `waitFrames(n)`, `nextFrame()`, `waitUntil(f)`, `waitWhile(f)`, `co_await otraTask()` (con resultado: `int n = co_await contar(3)`) y `co_await coroutine` (esperar a otra en marcha). `stopCoroutine`, `stopAllCoroutines`, `Coroutine::running()`. Temporizadores: `invoke(fn, segundos)`, `invokeRepeating(fn, retraso, intervalo)`, `cancelInvoke`, `isInvoking`. Se paran solas en `onDisable` (desactivar el script o su objeto) y en `onDestroy`. Se reanudan una vez por frame tras todos los `update()` y antes de los `lateUpdate()`; una espera puesta en un frame se mira desde el siguiente. Una excepcion dentro de una corrutina sale en la Consola y para solo esa corrutina. `cramion/Test.h` sigue con su `Test::Task` (sin cambios).
- **Orden de ejecucion determinista de los scripts de C++** (como Script Execution Order de Unity; `scripting/ScriptOrder.h`): por prioridad de la clase (menor = antes; `static constexpr int kExecutionOrder = -100;` en la clase o los ajustes del proyecto con la CVar `script.ExecutionOrder` = `"Jugador=-100; Camara=50"`, que pisa a la clase) y, a igual prioridad, por orden de creacion. Vale para awake/onEnable/start/update/lateUpdate/fixedUpdate/onDisable/onDestroy y es el mismo en el editor, el juego y el servidor dedicado. Antes iban en el orden de una tabla hash. `script.cpp.StableOrder = false` vuelve al orden de antes.
- Pruebas: `CramionCoroutineTests` (nueva: planificador sin motor), `CramionApiDebugApiTests` (dibujo y letra de trazos) y `CramionCppScriptTests` (corrutinas, invoke, `Debug::draw*` y orden con un script compilado de verdad).

## Archivos tocados

Nuevos:
- `CramionCore/include/CramionCore/scripting/DebugDraw.h`, `CramionCore/src/scripting/DebugDraw.cpp`: almacen del dibujo de depuracion, CVars y paso a `gfx::OverlayGeometry`.
- `CramionCore/include/CramionCore/scripting/ScriptOrder.h`, `CramionCore/src/scripting/ScriptOrder.cpp`: `parseExecutionOrder` y la clave de orden.
- `CramionCore/sdk/cramion/Coroutine.h`: `Task<T>`, `Coroutine`, `Timer`, esperas (`cramion::co::...`).
- `CramionFX/include/CramionFX/vk/StrokeFont.h`, `CramionFX/src/vk/StrokeFont.cpp`: letra de trazos.
- `CramionCore/tests/CoroutineTests.cpp`.

Modificados (bloques localizados):
- `CramionFX/include/CramionFX/vk/OverlayGeometry.h`: `OverlayText` y `texts` (empty/clear lo cuentan).
- `CramionFX/include/CramionFX/vk/OverlayPass.h`, `CramionFX/src/vk/OverlayPass.cpp`: `appendClipSegment` (sacado de `appendLines`, misma cuenta) y `appendTexts`.
- `CramionFX/include/CramionFX/vk/VulkanRenderer.h`, `CramionFX/src/vk/VulkanRenderer.cpp`: `setDebugGeometry/debugGeometry`, `debug_geometry_`, `overlay_merged_`; la condicion del pase "Gizmos 3D" y `recordOverlayPass` (gizmos, depuracion o los dos juntos).
- `CramionCore/src/scripting/native/DebugApi.cpp`: `Debug.draw*`, `Debug.drawClear`, `Debug.drawEnabled`; envejece en `Phase::Begin` y borra en `onStop`.
- `CramionCore/sdk/cramion/Script.h`: incluye `Coroutine.h`; `Debug::drawLine...` con tipos; metodos de corrutinas/invoke en `Script`; `kExecutionOrder` (`detail::executionOrderOf`, `registerClass` con orden); `eventMask` marca siempre `Disable`.
- `CramionCore/sdk/cramion/ScriptMain.cpp`: el planificador (`coStart`, `coStop`, `coStopOwned`, `timerStart`, `coTick`...), `cramion_class_order`, y en `cramion_call` Time -> `coTick`, Disable/Destroy -> parar corrutinas; `cramion_destroy` tambien.
- `CramionCore/sdk/cramion/Api.gen.h`: regenerado con `cramion_sdkgen` (anade `Debug.drawClear`, `Debug.drawEnabled` y lo que ya estaba registrado y faltaba en el archivo: `Game.editor`, `Game.isEditor`, `Input.getTouch`...).
- `CramionCore/tools/SdkGen.cpp`: salta `Debug.drawLine/Ray/Sphere/Box/Arrow/Text` (estan a mano con tipos).
- `CramionCore/src/scripting/CppScripts.cpp`: CVars `script.cpp.StableOrder` y `script.ExecutionOrder`; `class_orders` (proceso aparte y misma DLL); `Instance::order`, `orderOf`, `sortByOrder` en `liveHandles`, creacion, onEnable/onDisable y `forEachLive`; ids nuevos en orden de creacion (la vista de EnTT va al reves); un evento `Time` siempre tras los update (antes solo si no se envio nada); `Coroutine.h` en la lista de `upToDate`.
- `CramionEditor/host/ScriptHost.cpp`: manda la prioridad de cada clase tras los eventos en la respuesta de Load.
- `CramionEditor/src/EditorPhysics.cpp`: en Play pasa el dibujo de depuracion al renderizador (vistas de Escena y Juego).
- `CramionEditor/player/PlayerMain.cpp`: lo mismo en el juego (solo hay algo con `debug.draw.InGame`).
- `CramionCore/CMakeLists.txt`: objetivo `CramionCoroutineTests`.
- `CramionCore/tests/native/DebugApiTests.cpp`, `CramionCore/tests/CppScriptTests.cpp`: pruebas nuevas.

## CVars nuevas
| CVar | Defecto | Que |
|---|---|---|
| `debug.draw.InGame` | false | Dibujo de depuracion en el juego exportado (en el Play del editor, siempre) |
| `debug.draw.MaxLines` | 65536 | Tope de segmentos vivos |
| `debug.draw.MaxTexts` | 512 | Tope de textos vivos |
| `debug.draw.LineWidth` | 2 | Grosor en pixeles |
| `script.cpp.StableOrder` | true | Orden de ejecucion fijo (false: el de antes) |
| `script.ExecutionOrder` | "" | Prioridad por clase de los ajustes del proyecto (`Clase=n; Otra=m`) |

Todas `Saved` (van a `ProjectSettings/CVars.json`, que leen el editor, el juego y el servidor).

## API nueva
- API normal (Visual Scripts, consola, puente de C++): `Debug.drawLine(desde, hasta, color, duracion, profundidad)`, `Debug.drawRay(origen, direccion, ...)`, `Debug.drawSphere(centro, radio, ...)`, `Debug.drawBox(centro, tamano, giro, color, duracion, profundidad)`, `Debug.drawArrow(desde, hasta, color, duracion, profundidad, punta)`, `Debug.drawText(posicion, texto, color, duracion, profundidad, tamano)`, `Debug.drawClear()`, `Debug.drawEnabled`.
- SDK de C++: `Debug::drawLine/drawRay/drawSphere/drawBox/drawArrow/drawText` con `Vec3`, `Color` (con alfa) y `Quat`; `Task<T>`, `Coroutine`, `Timer`; en `Script`: `startCoroutine`, `stopCoroutine`, `stopAllCoroutines`, `invoke`, `invokeRepeating`, `cancelInvoke`, `isInvoking`, `waitSeconds`, `waitSecondsRealtime`, `waitFrames`, `nextFrame`, `waitUntil`, `waitWhile` (tambien en `cramion::co::`); `static constexpr int kExecutionOrder` en la clase.
- Exportacion nueva opcional de la DLL: `cramion_class_order(int)` (sin ella, 0). El ABI (`kAbiVersion`) no cambia.

## Pruebas y resultado (Release, clang, build-rm)
- `CramionCoroutineTests`: 35 comprobaciones, 0 fallos.
- `CramionApiDebugApiTests`: 52 comprobaciones, 0 fallos.
- `CramionCppScriptTests`: 90 comprobaciones, 0 fallos (compila los scripts con el clang incluido y los corre en CramionScriptHost; incluye el banco de 300 scripts: el lote sigue en 1 viaje por frame). En una pasada anterior fallaron las dos comprobaciones de `clang-format` (formatSource, sin mensaje), que no se tocan aqui; en la siguiente pasaron: parece del entorno (lanzar el proceso), no del bloque.
- Compilan sin errores: CramionCore, CramionScriptHost, cramion_sdkgen, CramionEditor, CramionPlayer, CramionServer.
- Suites existentes relacionadas, sin fallos: CramionNativeApiTests (37/37), CramionScriptingTests (26), CramionApiCoreApiTests (138), CramionApiTestApiTests (42), CramionApiVisualScriptRuntimeTests (109). CramionCppScriptTests repetida tras regenerar el SDK: 90/0.

## Pendiente
- `Test.h` no se reescribio sobre `cramion::Task` (sigue con su `Test::Task`, como pedia "que Test.h siga funcionando"); se podria unificar mas adelante.
- El motor no tiene `Time.timeScale` global: `waitSeconds` usa el `deltaTime` que llega a los scripts (si algun dia se escala, la sigue sola); `waitSecondsRealtime` usa el reloj real.
- Corrutinas solo en C++ (Visual Scripts ya tiene sus nodos de espera). Sin `invoke` en Visual Scripts.
- Orden de ejecucion solo para los scripts de C++ (los modulos de la API de Lua/Visual Scripts van por fases fijas). Sin ventana en el editor para editar `script.ExecutionOrder` (se edita en Variables/CVars).
- El texto de depuracion no lleva fondo ni minusculas propias; los caracteres fuera de ASCII (salvo tildes, enie, cedilla, grados) salen como '?'.
- `Debug::draw*` desde C++ en un juego sin `debug.draw.InGame` aun envia la llamada en el lote (el motor la descarta al momento); se podria cachear `Debug.drawEnabled` por frame en el SDK.

## Riesgos
- **Orden de ejecucion encendido por defecto**: el orden de antes era el de una tabla hash (sin garantias), pero un juego que dependiera sin saberlo de el podria notar otro orden. Se vuelve atras con `script.cpp.StableOrder = false`.
- **Evento Time siempre** tras los update (uno por frame, en el mismo lote: sin viaje extra; con `script.cpp.BatchCalls = false` es un viaje mas por frame). Las DLL anteriores lo ignoran.
- **onDisable siempre se envia** (para parar corrutinas), aunque la clase no lo sobrescriba: un evento mas al desactivar, en el lote.
- Un fallo de hardware (puntero nulo) dentro de una corrutina se atribuye al script que recibio el evento Time (el primero del orden), no al dueno de la corrutina. Las excepciones de C++ si van a su corrutina.
- `Api.gen.h` regenerado: puede chocar al fusionar con otros bloques que tambien regeneren (regenerar otra vez tras la fusion).
- `OverlayPass::appendLines` se reescribio en `appendClipSegment` con la misma cuenta; en la vista de Escena con Play, los gizmos y la depuracion se juntan (las lineas de depuracion "a traves" salen con el grosor del gizmo, 5 px, y las tapadas con los rayos X del editor).

## Probar a mano (GPU / editor)
1. Editor, Play con un script de C++ que llame a `Debug::drawLine/drawSphere/drawBox/drawArrow/drawText` cada frame y con duracion: se ven en las vistas de Escena y Juego; lo de `depthTest = true` se oculta detras de los objetos y lo de `false` se ve a traves. El texto se lee a su tamano en pixeles y se mantiene al alejarse.
2. Lo mismo desde la consola (`Debug.drawSphere(Vec3(0,2,0), 1, "rojo", 5)`) y desde un Visual Script (nodo de llamada a `Debug.drawLine`).
3. Parar el Play: no queda nada dibujado. Pausa: lo dibujado se queda.
4. Juego exportado: sin `debug.draw.InGame` no se ve nada; con `debug.draw.InGame true` en `ProjectSettings/CVars.json`, si. RenderTexture (minimapa) y capturas sin dibujo.
5. Una escena con miles de `drawLine` por frame: el aviso de tope sale una vez y el editor no se arrastra.
6. Corrutinas en Play: un script con `startCoroutine` + `waitSeconds`, desactivar el objeto (se para), recarga en caliente (avisa y no las conserva, como antes).
7. Orden: varios scripts con `kExecutionOrder` y `script.ExecutionOrder` en Variables; el orden de sus `update` es el mismo en el editor, en el juego y en el servidor dedicado.
8. Horizonz: Play de la escena Main sin cambios visibles (orden estable activado) y el servidor dedicado arranca igual.

## Revision (revisor independiente, 18/11/2026)

Revisado el diff `horizonz-1031...rm-scripts` entero (orden, corrutinas, dibujo, renderizador, SDK, pruebas).

Comprobado sin cambios:
- Sincronizacion de Vulkan: cada `drawFrame` (vista de Escena, de Juego, ojos de VR) espera la valla de su hueco
  y avanza `current_frame_`, asi que el buffer por hueco de `OverlayPass` no se pisa aunque ahora se dibuje en
  las dos vistas en el mismo frame. RenderTexture (`render_texture_target_`) y capturas siguen sin dibujo.
- Hilos: la API nativa (y `debugdraw::shared()`) solo se usa en el hilo principal; los hilos de los scripts
  pasan por `apiRequest` (onScriptThread).
- Retrocompatibilidad: sin cambios de serializacion (escenas, prefabs, .crdata); la respuesta de Load anade
  las prioridades al final (un motor anterior no las lee; un host anterior no las manda: 0); una DLL anterior
  ignora el evento Time (solo pone el tiempo) y no exporta `cramion_class_order`. `Script` no cambia de
  tamano (solo metodos). Los scripts de Horizonz (Assets/Scripts) no usan `Task`, `Timer`, `Coroutine`,
  `invoke`, `waitSeconds`... ni `using namespace cramion::Test`: sin choques de nombres al incluir Coroutine.h.
- El jugador (`PlayerMain.cpp`) y el servidor (`ServerMain.cpp`) cargan `ProjectSettings/CVars.json`: `script.ExecutionOrder`
  vale igual en editor, juego y servidor.
- Pruebas: comprueban de verdad (orden "DC12A" con DLL compilada, corrutinas por frame, invoke, onDisable,
  colores/alfa y profundidad de lo dibujado).

Arreglado:
- **Editor: el dibujo de depuracion solo se recoge en Play.** Antes `setEditor(true)` iba siempre: en modo
  edicion (consola) se guardaba lo dibujado sin que nadie lo pintara ni lo envejeciera, hasta el tope
  (65536 segmentos), y aparecia de golpe al dar a Play. Ademas se ponia despues de correr los scripts del
  frame, asi que lo del primer frame de Play se perdia. Ahora `setEditor(playing())` al empezar
  `updatePhysics`, antes de los scripts (`EditorPhysics.cpp`).
- **`liveHandles` (cada frame) sin buscar cada instancia otra vez:** construye la clave de orden mientras
  recorre `instances` y ordena (las claves son unicas: `std::sort` es determinista). Con
  `script.cpp.StableOrder = false` ni ordena (`CppScripts.cpp`).

Queda (documentado, no arreglado):
- Una corrutina que se pone a esperar desde un callback o un mensaje (fuera de los eventos con tiempo) usa el
  numero de frame del ultimo evento, asi que `nextFrame()` ahi puede seguir en el mismo frame. Desde
  awake/start/update/lateUpdate/fixedUpdate e invoke va bien.
- La prioridad de cada instancia se calcula al crearla: cambiar `script.ExecutionOrder` en mitad del Play no
  reordena las ya creadas (como Unity, que lo aplica al cargar).
- En la vista de Escena durante Play, gizmos y depuracion comparten pase (grosor y rayos X del gizmo).

Tras la revision (build-rm, Release, clang): compilan CramionCore, CramionScriptHost, CramionEditor y
CramionPlayer (el SDK se regenera sin cambios). CramionCoroutineTests 35/0, CramionApiDebugApiTests 52/0,
CramionCppScriptTests 90/0 (el orden "DC12A" sigue igual con el `liveHandles` nuevo).

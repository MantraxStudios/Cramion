# Pendientes (para el siguiente agente)

Estado al 2026-10-11, commit en `main` y en la rama `claude/gallant-wright-9h54m6`. El trabajo se paró por
falta de crédito: varias partes del port sin Lua quedaron **a medias**. Este archivo dice qué está hecho, qué
falta y cómo comprobarlo. El plan general sigue en `PLAN-SIN-LUA.md`.

## 0. Primero: que todo compile otra vez

Compilación de Linux (el editor y el reproductor son de Windows; en Linux se compila el motor, las pruebas,
`CramionServer` y ahora también `CramionScriptHost`):

```bash
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang
cmake --build build-linux -j 4 -- -k 0
cd build-linux/CramionCore && for t in Cramion*Tests; do ./$t | tail -1; done   # se ejecutan desde aqui
```

Hoy **solo fallan dos archivos de pruebas** (el resto compila y todas las demás pruebas pasan):
- `CramionCore/tests/StateMachineTests.cpp`: usa `ConditionKind::Lua` (ahora `ConditionKind::Expression`, mismo
  valor 3) y la firma vieja de `conditionHolds`/`pickTransition` (ya no reciben la función de Lua). Reescribir sus
  partes de Lua: condiciones como expresiones y el código de los estados como mensajes capturados con
  `scripts.setMessageListener` (`OnStateEnter`, `OnStateUpdate`, `OnStateExit`).
- `CramionCore/tests/Features21Tests.cpp:227`: usa `CompileResult::lua`, que ya no existe (ver Visual Scripts).

`CramionShaderTests` da 1 fallo en Linux porque busca `shaderc_shared.dll` (solo Windows): no es un error.

Editor (Windows) desde Linux, solo sintaxis, con MinGW (`x86_64-w64-mingw32-g++-posix`). Hace falta Dear ImGui
`v1.92.9-docking` y ImGuizmo `18cef5e0` en una carpeta (`git clone --depth 1`):

```bash
x86_64-w64-mingw32-g++-posix -std=c++20 -fsyntax-only -w -DNOMINMAX -DWIN32_LEAN_AND_MEAN -DUNICODE -D_UNICODE \
  -DCRAMION_XR=1 -DDT_POLYREF64 -DVK_USE_PLATFORM_WIN32_KHR -DCRAMION_VERSION_STRING=\"2.9.0\" \
  -ICramionEditor/src -ICramionEditor -ICramionCore/include -ICramionCore/sdk -ICramionFX/include \
  -ICramionDM/include -ICramionUpdater/include -Ibuild-linux/_deps/entt-src/src \
  -Ibuild-linux/_deps/nlohmann_json-src/include -Ibuild-linux/_deps/enet-src/include -Ibuild-linux/_deps/miniaudio-src \
  -isystem CramionFX/vendor/include -isystem build-linux/_deps/joltphysics-src \
  -isystem build-linux/_deps/recastnavigation-src/Recast/Include -isystem build-linux/_deps/recastnavigation-src/Detour/Include \
  -isystem build-linux/_deps/recastnavigation-src/DetourCrowd/Include \
  -isystem <imgui> -isystem <imgui>/backends -isystem <imgui>/misc/cpp -isystem <imguizmo>/src \
  CramionEditor/src/EditorX.cpp
```
Fallos conocidos que no son errores: `host/ScriptHost.cpp` (usa `__except` de MSVC) y `player/AndroidSupport.cpp`
(cabeceras de Android). **Hoy falla `CramionEditor/src/EditorStateMachine.cpp`** (líneas 323, 1089-1101, 1163:
`ConditionKind::Lua` y `conditionHolds` con 4 argumentos). Ver la sección 2.

## 1. Hecho y verificado (no tocar salvo errores)
- Auditoría de shaders (40 errores) y optimizaciones de CPU/GPU: en el CHANGELOG (2.9.0, «Gráficos» y «Rendimiento»).
- Runtime de scripts sin Lua: `CramionCore/src/scripting/Scripting.cpp`, `ScriptRuntime.h` (fases del frame y
  ganchos) y los módulos de la API en `src/scripting/native/` con sus pruebas en `tests/native/`: Debug, Core,
  Console, Entity, Rig, Input, Network, Http/Json, World, Graphics, Mesh, Effects, Features24, Gameplay, DataPack,
  **Steam** y **Test** (SteamApiTests y TestApiTests pasan).
- Perfilador de los scripts: zona `Scripts` por fase, contador `Scripts (ms)`, CVar `scripts.BudgetMs`.
- Protocolo de los scripts de C++: `Rpc::Api`/`ApiSend` y `CppScriptSystem::setScriptSystem`.
- El código Lua viejo está en `CramionCore/legacy_lua/` **solo como referencia** (no se compila). Se borra al final.

## 2. A medias (terminar)

**IA: StateMachine y BehaviorTree** (`src/ai/`, `include/CramionCore/ai/`, `native/AiApi.cpp`)
- Hecho: `ai/Expression.h/.cpp` (evaluador de condiciones: números, textos, variables, `+ - * / %`,
  comparaciones, `and/or/not`, `&& || !`, paréntesis; se analiza una vez). `ConditionKind::Expression` (= 3,
  los archivos viejos con condiciones de Lua se siguen leyendo). `AiApi.cpp` porta la API de
  `legacy_lua/StateMachineScripting.inl` y `BehaviorTreeScripting.inl` (unas 1000 líneas). Diseño: el código de
  los estados ya no se ejecuta, los estados mandan mensajes a los scripts de C++ (`OnStateEnter/Update/Exit`).
  En el BT, Run Script manda `OnBtTask` y espera a `BehaviorTree.finishTask`; abortar manda `OnBtAbort`.
- Falta: comprobar que `AiApi.cpp` cubre todos los nombres antiguos, escribir `tests/native/AiApiTests.cpp`,
  reescribir `tests/StateMachineTests.cpp` (sección 0) y arreglar `CramionEditor/src/EditorStateMachine.cpp`
  (campo de texto para la expresión; en los estados, una nota de que la lógica va en un script de C++).

**Visual Scripts** (`src/scripting/VisualScript.cpp`, `include/CramionCore/scripting/VisualScript.h`,
`native/VisualScriptRuntime.cpp`)
- Hecho: se quitó el compilador a Lua; `compileGraph` ahora solo lee y valida el grafo.
- **Falta lo principal: el intérprete.** `native/VisualScriptRuntime.cpp` sigue vacío, así que **los Visual
  Scripts no se ejecutan**. Hay que escribirlo: grafo cargado una vez (y recargado con `rt.onReload`); por
  instancia, cadenas de ejecución, nodos puros evaluados al pedirlos, variables propias y nodos latentes
  (Delay...) como continuaciones (sin corrutinas); las llamadas a la API van por `rt.native.call/get/set`.
  Start/Update en `Phase::Update`, LateUpdate en `Phase::Late`, eventos (choques en `rt.frame_events`, interfaz con
  `rt.onEvent`), depuración (`VisualScriptDebugHost`, `rt.vs_debug`). La semántica de los ~112 nodos está en el
  compilador viejo (`git show 67c80d9:CramionCore/src/scripting/VisualScript.cpp`, líneas 1100-1962) y el runtime
  viejo en `legacy_lua/VisualScriptScripting.inl`. Pruebas: `tests/native/VisualScriptRuntimeTests.cpp` y
  `tests/Features21Tests.cpp` (sección 0). Revisar `CramionEditor/src/EditorGraphs.cpp` (hoy compila).

**SDK de los scripts de C++** (`CramionCore/sdk/cramion/`, `tools/SdkGen.cpp`)
- Hecho: `detail::lua*` → `detail::api*`, `namespace Api { call/get/set }` (y `Lua::` como alias obsoleto),
  `SdkGen.cpp` genera `Api.gen.h`/`EntityApi.gen.inc` desde `ScriptSystem::apiRegistry()` (ya regenerados una vez),
  y `Test.h` nuevo (pruebas del juego en C++: `CRAMION_TEST`, `co_await Test::waitSeconds/waitFrames/waitUntil`,
  `Assert::...`, el script `CramionTests` que las ejecuta y avisa con `Test.begin/check/finish/done/log`).
- Falta: compilar un script de ejemplo con `Test.h` y varias funciones de `Api.gen.h` (clang y MinGW,
  `-fsyntax-only -I CramionCore/sdk`); **regenerar `Api.gen.h` al final** (`build-linux/cramion_sdkgen`) cuando
  estén la IA y los Visual Scripts, y revisar la lista de símbolos que desaparecen (el generador la imprime);
  quitar los restos de «Lua» de `CramionEditor/src/EditorCppScripts.cpp` (líneas ~188, ~425-434, ~940-952).

**Scripts de C++ en Linux** (`src/scripting/CppScripts.cpp`, `CramionEditor/host/ScriptHost.cpp`, `CMakeLists.txt`)
- Hecho: camino POSIX en `CppScripts.cpp` y `ScriptHost.cpp`; `CramionScriptHost` se compila en Linux.
- Falta: que `tests/CppScriptTests.cpp` ejecute de verdad la parte de C++ en Linux (hoy dice «sin compilador
  de C++: se salta»): compilar los scripts que fallan a propósito (puntero nulo, excepción, bucle infinito,
  recursión, memoria, abort) y comprobar que el motor sigue vivo. Cambiar las comprobaciones del puente con Lua
  (`Lua::set`, `lua.run`) por la API (`Api::call`, `scripts.nativeApi()`). Con eso el servidor dedicado de Linux
  vuelve a tener scripts.

**Pruebas del juego en el editor** (`CramionEditor/src/EditorPlatform.cpp`, hoy compila)
- Hecho: el Test Runner ya no carga `.test.lua`. En Play crea un objeto «Pruebas» con el script `CramionTests` y
  muestra `Test.results()`.
- Falta: `--run-tests` en `CramionEditor/player/PlayerMain.cpp` (mismo cambio) y probarlo en Windows.

## 3. Sin empezar
1. **Plantillas de proyecto en C++**: `CramionEditor/src/ProjectTemplates.cpp` y `Template21Scripts.h`,
   `TemplateCreatureScripts.h`, `TemplateLocomotionScripts.h`, `TemplateOnlineScripts.h`,
   `TemplateOpenWorldScripts.h`, `TemplateMmoScripts.h` (son Lua). El modelo es `TemplateVrScripts.h`, que ya es
   C++. Pruebas: `CramionEditor/tests/TemplateTests.cpp` (usa `getScript()`, que ya no existe).
2. **Demo del minigolf**: `CramionCore/tools/minigolf/Scripts/*.lua` (8) y `tools/MiniGolfDemo.cpp`, a C++.
3. **Editor**: el autocompletado (`LuaCompletion.cpp/.h` → generado desde `ScriptSystem::apiRegistry()`),
   `EditorLuaSymbols.cpp`, el editor y la consola de Lua de `EditorScripting.cpp`, el inspector del componente
   Script (Lua, obsoleto), `EditorMcp.cpp` (`run_lua`, `create_script` en Lua), `EditorInsights.cpp:194`
   (`lua.BudgetMs` → `scripts.BudgetMs`), `EditorHouseGenerator.cpp`, `EditorWorkspaces.cpp`, `EditorMcp21.cpp`,
   `EditorProject.cpp`, `EditorHub.cpp` y `CramionEditor/tests/CompletionTests.cpp`.
4. **Mods** (`CramionCore/src/project/Mods.cpp`, `main.lua`) y `PlayerMain.cpp` (mods, nombres «Lua» en el
   perfilador y en los pasos de cierre).
5. **Android**: los scripts de C++ dentro del APK (compilados con el NDK al exportar y cargados en el mismo
   proceso). Hoy `EditorExport.cpp` avisa de que solo van en Windows.
6. **Manual** (`docs-src/`): `cpp-lua.html` → `cpp-api.html`, el grupo «Referencia de Lua (obsoleta)» de
   `pages.json`, `build_manual.py` y `gen_cpp_api.py`. Actualizar `README.md`.
7. **Borrar Lua**: `CramionCore/legacy_lua/`, el componente `Script` cuando ya no haga falta leer escenas viejas
   (hoy se carga y avisa), y comprobar que no queda nada: `git grep -nP '[Ll]ua(?![a-z])|\bsol::'`. Muchas
   menciones que quedan son solo comentarios («desde Lua» → «desde los scripts»).
8. CHANGELOG (2.9.0, sección «Scripting»: hoy dice «Empieza la retirada de Lua») y `PLAN-SIN-LUA.md`.

## 4. Rendimiento: lo que queda
- **Volver a medir** con la máquina libre: `build-linux/bench/cramion_render_bench` con Lavapipe bajo Xvfb y las
  capas de validación (0 errores esperados), y comparar con la medida de antes de las optimizaciones.
- Opcionales (ahorran poco): lista de candidatos por luz puntual con sombras (hoy las 6 caras recorren todos los
  objetos con una prueba de esfera), recordar la última clave de `batch_lookup_` en `RenderSync::updateActors`,
  menos tablas hash por frame en `RenderSync::updateRipples`.
- Detalles gráficos conocidos sin arreglar: líneas oscuras finas en los bordes de las paredes (probablemente
  FSR/TAA), el recorte de sombras no usa `base_color.a`, las partículas no se ordenan, la corteza de la lava toma
  el máximo de los materiales, y con el escalado apagado el DOF/motion blur acaban en el historial que leen SSR y SSGI.

## Reglas del proyecto (las que se usaron)
- Comentarios en español **sin tildes**, con el estilo y la densidad del código de alrededor.
- La API conserva los nombres de siempre (`Audio.playOneShot`, `Entity:translate`...). Los argumentos opcionales
  usan `c.number(i, por_defecto)`; las entidades, `rt.entityArg`/`rt.selfEntity`; los errores, `throw api::Error`.
  Los callbacks se guardan como `api::Value` y se llaman con `rt.native.invoke` (asíncronos, se olvidan en
  `onStop`). Los eventos a los scripts de C++ van con `rt.message_listener(entidad, "OnAlgo", json)`.
- Una prueba por módulo en `CramionCore/tests/native/<Modulo>Tests.cpp` con `ApiTest.h` (CMake las encuentra solas).
- Una sola compilación a la vez (4 núcleos y 16 GB de RAM).

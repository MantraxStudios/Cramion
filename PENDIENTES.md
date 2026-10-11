# Pendientes (para el siguiente agente)

Estado al 2026-10-11, rama `claude/pendientes-seccion-3`. El port sin Lua está terminado (secciones 1 y 3):
Lua ya no está en el motor. Lo que queda es probar en Windows y Android (sección 2) y el rendimiento (sección 4).
`PLAN-SIN-LUA.md` queda como historia del plan.

## 0. Compilar y probar

Compilación de Linux (el editor y el reproductor son de Windows; en Linux se compila el motor, las pruebas,
`CramionServer` y `CramionScriptHost`):

```bash
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang
cmake --build build-linux -j 4 -- -k 0
cd build-linux/CramionCore && for t in Cramion*Tests; do ./$t | tail -1; done   # se ejecutan desde aqui
```

Todo compila y **todas las pruebas pasan**. `CramionShaderTests` da 1 fallo en Linux porque busca
`shaderc_shared.dll` (solo Windows): no es un error. Ojo: el `glslc` de Ubuntu 24.04 no conoce
`GL_EXT_shader_invocation_reorder` y falla en `CramionFX/shaders/path_trace.rgen`; hace falta un shaderc más
nuevo (o pasar otro con `-DGLSLC_EXECUTABLE=...`). `CramionCppScriptTests` necesita `clang++` o `g++` en el PATH
(sin compilador se salta la parte de C++).

Editor (Windows) desde Linux, solo sintaxis, con MinGW (`apt-get install g++-mingw-w64-x86-64-posix`). Hace
falta Dear ImGui `v1.92.9-docking` y ImGuizmo `18cef5e0` en una carpeta (`git clone`):

```bash
x86_64-w64-mingw32-g++-posix -std=c++20 -fsyntax-only -w -DNOMINMAX -DWIN32_LEAN_AND_MEAN -DUNICODE -D_UNICODE \
  -DCRAMION_XR=1 -DDT_POLYREF64 -DVK_USE_PLATFORM_WIN32_KHR -DCRAMION_VERSION_STRING=\"2.4.5\" \
  -ICramionEditor/src -ICramionEditor -ICramionCore/include -ICramionCore/sdk -ICramionFX/include \
  -ICramionDM/include -ICramionUpdater/include -Ibuild-linux/_deps/entt-src/src \
  -Ibuild-linux/_deps/nlohmann_json-src/include -Ibuild-linux/_deps/enet-src/include -Ibuild-linux/_deps/miniaudio-src \
  -isystem CramionFX/vendor/include -isystem build-linux/_deps/joltphysics-src \
  -isystem build-linux/_deps/recastnavigation-src/Recast/Include -isystem build-linux/_deps/recastnavigation-src/Detour/Include \
  -isystem build-linux/_deps/recastnavigation-src/DetourCrowd/Include \
  -isystem <imgui> -isystem <imgui>/backends -isystem <imgui>/misc/cpp -isystem <imguizmo>/src \
  CramionEditor/src/EditorX.cpp
```
Hoy pasan todos los `.cpp` de `CramionEditor/src` y `CramionEditor/player` (menos `player/AndroidSupport.cpp`,
que usa cabeceras de Android, y `host/ScriptHost.cpp`, que usa `__except` de MSVC).

## 1. Hecho y verificado (no tocar salvo errores)
- Auditoría de shaders (40 errores) y optimizaciones de CPU/GPU: en el CHANGELOG (2.4.5, «Gráficos» y «Rendimiento»).
- Runtime de scripts sin Lua: `CramionCore/src/scripting/Scripting.cpp`, `ScriptRuntime.h` (fases del frame y
  ganchos) y los módulos de la API en `src/scripting/native/` con sus pruebas en `tests/native/`: Debug, Core,
  Console, Entity, Rig, Input, Network, Http/Json, World, Graphics, Mesh, Effects, Features24, Gameplay, DataPack,
  Steam, Test, **Ai** y **VisualScriptRuntime**.
- Perfilador de los scripts: zona `Scripts` por fase, contador `Scripts (ms)`, CVar `scripts.BudgetMs` (también
  en la ventana de Insights del editor).
- Protocolo de los scripts de C++: `Rpc::Api`/`ApiSend` y `CppScriptSystem::setScriptSystem`.
- **IA**: `ai/Expression` (condiciones), `ConditionKind::Expression`, `AiApi.cpp` (StateMachine y BehaviorTree,
  mensajes `OnStateEnter/Update/Exit`, `OnBtTask/OnBtAbort/OnBtService`). Pruebas: `tests/StateMachineTests.cpp`
  (reescrita sin Lua: expresiones, mensajes, API, consola, recarga) y `tests/native/AiApiTests.cpp` (árboles).
  Editor (`EditorStateMachine.cpp`): campo de expresión con su error de lectura, casilla «Enviar OnStateUpdate»
  en cada estado, nota de cómo se hace en C++ y el código Lua antiguo en solo lectura con un botón para borrarlo.
  Las herramientas MCP `create/update/get_state_machine` ya no hablan de código Lua (`send_update` por estado).
- **Visual Scripts**: `native/VisualScriptRuntime.cpp` es el intérprete (grafo cargado una vez por archivo; por
  objeto, variables, nodos puros evaluados al pedirlos y Delay como continuaciones; eventos Start/Update/Late/
  Fixed/Destroy, teclas, acciones de entrada, choques, Custom Event, temporizadores, Send Event; depuración con
  `VisualScriptDebugHost` y puntos de parada; recarga en caliente). Pruebas: `tests/native/VisualScriptRuntimeTests.cpp`
  y `tests/Features21Tests.cpp`.
- **SDK**: `Api.gen.h`/`EntityApi.gen.inc` regenerados con la IA y los Visual Scripts (no desaparece ningún
  símbolo). Un script de ejemplo con `Test.h` y funciones de `Api.gen.h` compila con clang++ y g++
  (`-fsyntax-only -I CramionCore/sdk`). Quitados los restos de «Lua» de `EditorCppScripts.cpp` (la consola de C++
  ahora dice «API»; `Lua` sigue reservado como nombre de clase porque `Lua::` existe como alias obsoleto).
- **Scripts de C++ en Linux**: `CppScripts.cpp` tiene el camino POSIX completo: `CramionScriptHost` con memoria
  compartida (`memfd`) y un `socketpair` (fd 3 y 4), tope de memoria con `RLIMIT_DATA`/`prlimit`, compilación con
  `clang++` (incluido en `toolchain/bin`, `CRAMION_CXX`, el PATH o `/usr/lib/llvm-N`) o `g++` a `.so`, y
  `clang-format`/`clangd` buscados igual. `tests/CppScriptTests.cpp` se ejecuta entero en Linux (65 comprobaciones:
  puntero nulo, excepción, bucle infinito, recursión, memoria, `abort()`...) y usa la API (`Api::call`,
  `scripts.nativeApi()`) en lugar de `Lua::set`/`lua.run`.
- El código Lua viejo está en `CramionCore/legacy_lua/` **solo como referencia** (no se compila). Se borra al final.

## 2. Falta (pequeño)
- **Probar en Windows** lo de la sección 3: crear un proyecto de cada plantilla y darle a Play, la demo del
  minigolf, el autocompletado y la consola de C++, un mod con `main.crgraph`, `<Juego>Server.exe` con scripts, y
  exportar a Android con el NDK instalado (un proyecto con scripts de C++ en un móvil; el `.so` de los scripts y
  `libcramion_scripthost.so` deben ir en `lib/<abi>/`). Desde Linux solo se pudo comprobar la sintaxis de Android
  con las cabeceras de bionic, sin el NDK.
- **Probar en Windows** lo de la sección 1 que solo se pudo compilar desde Linux con MinGW: el editor de máquinas
  de estados, la consola de C++ y `CppScriptTests` con el compilador incluido (allí comprueba que el tipo es
  «clang (incluido)»).
- `--run-tests` en `CramionEditor/player/PlayerMain.cpp`: el reproductor **nunca tuvo** pruebas (solo el editor:
  `CramionEditor/src/main.cpp` y `EditorPlatform.cpp`). Añadirlo es una función nueva: decidir si hace falta.

## 3. Hecho: Lua retirado (2026-10-11)
Las plantillas de proyecto y el minigolf son C++; el editor (autocompletado, consola, MCP, textos) no usa Lua;
los mods arrancan con un `main.crgraph` o `main.crprefab` (`scripting/ModEntry.cpp`); el servidor dedicado ejecuta
scripts de C++; los scripts de C++ se compilan con el NDK al exportar a Android; el manual está reescrito en C++;
y se borraron `legacy_lua/`, el componente `Script` y el alias `Lua::`. Lo que queda de «Lua» en el código es a
propósito: leer proyectos viejos (el código Lua de las máquinas de estados, `main.lua` de los mods, la extensión
`.lua` en `TemplateTests.cpp`) y la historia del CHANGELOG y del README.

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

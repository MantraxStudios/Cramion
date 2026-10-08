# Plan: quitar Lua de Cramion (empezado el 2026-10-07)

Decisiones del usuario:
- **La API pasa a C++ nativo.** Cada función (`Audio.playOneShot`, `Entity:translate`...) se escribe en C++ en el motor. Los scripts de C++ la llaman sin pasar por Lua.
- **En Android, los scripts de C++ van dentro del APK** (se compilan con el NDK al exportar y corren en el mismo proceso).
- **Todo lo que ya existe en Lua se porta a C++**: las plantillas, la demo del minigolf, StateMachine/BehaviorTree, el autocompletado y el manual.

## Cómo está hoy
- `ScriptSystem` (Scripting.cpp + 9 `.inl`, unas 9.000 líneas de sol2) contiene la API **y** sistemas que no son de Lua: la red, el HTTP, Prefs, el gameplay, las acciones de entrada...
- Scripts de C++: `Api.gen.h` → `detail::lua("Tabla.fn", args)` → RPC `Lua`/`LuaSend` (JSON) → `ScriptSystem::bridgeCall` → Lua.
- Visual Scripts: `compileGraph` genera Lua.
- StateMachine/BehaviorTree: el código de los estados y las condiciones están en Lua.
- Pruebas automáticas del juego: `.test.lua` (Test/Assert).
- `cramion_sdkgen` genera `Api.gen.h` a partir de la documentación de `LuaCompletion.cpp`.

## Estrategia: migrar sin romper la compilación
`ScriptSystem::Impl` sigue siendo el runtime del juego; solo cambia la forma de registrar la API.
La API nativa (`scripting/NativeApi.h`: `api::Value`, `api::Registry`) se consulta **primero** en
`bridgeCall`. Si una función todavía no está en ella, la llamada sigue yendo a Lua. Así se puede
migrar tabla a tabla, comprobando en cada paso que todo sigue funcionando.

## Fases
1. [ ] **Núcleo de la API nativa**: Value, Registry (funciones, métodos de Entity y de los handles, propiedades), handles y callbacks; `bridgeCall` mira primero la nativa. Con pruebas.
2. [ ] **Migrar la API, tabla a tabla** (de sol2 a la Registry): Debug, Time, Game, Prefs, CVar, Input, Screen, Physics, Audio, Scene, Entity, Navigation, Graphics, Voxel, Fluid, Mesh, Http/Json, Network, DataPack, Profiler, XR, Fire/Weather y los `.inl` (Gameplay, Platform, Effects, Features24, Character, StateMachine, BehaviorTree).
3. [ ] **`cramion_sdkgen` lee la Registry** (nombres, argumentos y documentación). `Api.gen.h` deja de usar `detail::lua` y el RPC pasa a llamarse `Api`.
4. [ ] **Visual Scripting**: un intérprete de grafos en C++ (sustituye a `compileGraph`→Lua; los Delay pasan a ser estado del intérprete, no corrutinas).
5. [ ] **StateMachine / BehaviorTree**: estados con una clase de C++ (`Script`) y condiciones con variables, triggers, temporizadores y expresiones simples evaluadas en C++.
6. [ ] **Pruebas del juego**: Test/Assert pasan a C++ (`CRAMION_TEST`), en lugar de `.test.lua`.
7. [ ] **Android**: compilar los scripts de C++ con el NDK al exportar y ejecutarlos en el mismo proceso (sin CramionScriptHost).
8. [ ] **Contenido**: las plantillas (`Template*Scripts.h`, `ProjectTemplates.cpp`), el minigolf, el autocompletado (LuaCompletion → C++) y el manual (`cpp-lua.html`).
9. [ ] **Borrar Lua**: el componente `Script`, sol2, `cramion_lua`, LuaMath, `lua.BudgetMs` y la CMake. Actualizar el CHANGELOG y el README.

## Reglas (16 GB de RAM)
- Una sola compilación a la vez: `cmake --build build -j 4 -- -k 0`.
- Ejecutar las pruebas de una en una.
- No hacer commit sin preguntar.

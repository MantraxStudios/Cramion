#ifndef CRAMION_CORE_SCRIPTING_NATIVE_MODULES_H
#define CRAMION_CORE_SCRIPTING_NATIVE_MODULES_H

// Los modulos de la API de scripting. Cada uno registra sus funciones en
// rt.native y se engancha a las fases del frame (ScriptRuntime.h). Se
// registran una vez, al crear el ScriptSystem, en el orden de abajo (que es
// el orden dentro de cada fase del frame).
//
// Reglas para escribir un modulo:
//   - Los nombres son los de siempre (los mismos que tenia la API de Lua y
//     que usa el SDK de C++): "Audio.playOneShot", "Entity:translate"...
//   - Argumentos opcionales con los de c.number(i, por_defecto)...; las
//     entidades con rt.entityArg / rt.selfEntity; un error, throw api::Error.
//   - Varios resultados: una lista (api::Value::Array), como antes.
//   - Callbacks: guardar el api::Value de la funcion y llamarla con
//     rt.native.invoke(fn, args) (llega al script un poco despues y no
//     devuelve nada). Se olvidan en onStop.
//   - Objetos del motor: un api::Handle (rt.native.intern para no repetir).
//   - Documentacion: {"argumentos", "una linea", "lo que devuelve"}: sale en
//     el autocompletado y en el SDK (Api.gen.h).
//   - Su estado, en un shared_ptr que capturan sus lambdas (no en Impl).

#include "../ScriptRuntime.h"

namespace cramion::scripting::native {

using Runtime = ScriptSystem::Impl;

// Mesh (MeshApi.cpp): la malla como handle "Mesh" y al reves (Entity.mesh).
api::Value meshValue(Runtime& rt, std::shared_ptr<ecs::Mesh> mesh);
std::shared_ptr<ecs::Mesh> meshOf(const api::Value& v);

void registerDebugApi(Runtime& rt);          // Debug.*, print
void registerCoreApi(Runtime& rt);           // Time, Scene, Prefs, Game, Profiler, CVar, Physics, Audio, Random
void registerConsole(Runtime& rt);           // ScriptSystem::run (consola)
void registerEntityApi(Runtime& rt);         // Entity (transformacion, componentes, fisica, sonido, UI...)
void registerRigApi(Runtime& rt);            // huesos, IK, ragdoll, sockets, navegacion, CharacterController
void registerInputApi(Runtime& rt);          // Input (teclas, acciones), Screen, XR
void registerNetworkApi(Runtime& rt);        // Network y lo de red de las entidades
void registerHttpApi(Runtime& rt);           // Http, Json
void registerWorldApi(Runtime& rt);          // Fire, Weather / Environment, Voxel, Fluid
void registerGraphicsApi(Runtime& rt);       // Graphics, Graphics.post
void registerMeshApi(Runtime& rt);           // Mesh
void registerEffectsApi(Runtime& rt);        // VFX, 2D, Physics2D, destruccion, vehiculos, Motion Matching, Replay
void registerFeatures24Api(Runtime& rt);     // splines, listas, scroll, WorldPartition, Accessibility, Camera, Voice...
void registerGameplayApi(Runtime& rt);       // Save, Text, Dialogue
void registerDataPackApi(Runtime& rt);       // DataPack
void registerSteamApi(Runtime& rt);          // Steam
void registerTestApi(Runtime& rt);           // Test / Assert (pruebas del juego)
void registerVisualScriptRuntime(Runtime& rt);  // Visual Scripts (.crgraph)
void registerAiApi(Runtime& rt);             // StateMachine, BehaviorTree

// Todos, en orden (ScriptSystem::ScriptSystem).
inline void registerAll(Runtime& rt) {
    registerDebugApi(rt);
    registerCoreApi(rt);
    registerConsole(rt);
    registerEntityApi(rt);
    registerRigApi(rt);
    registerInputApi(rt);
    registerNetworkApi(rt);
    registerHttpApi(rt);
    registerWorldApi(rt);
    registerGraphicsApi(rt);
    registerMeshApi(rt);
    registerEffectsApi(rt);
    registerFeatures24Api(rt);
    registerGameplayApi(rt);
    registerDataPackApi(rt);
    registerSteamApi(rt);
    registerTestApi(rt);
    registerVisualScriptRuntime(rt);
    registerAiApi(rt);
}

}  // namespace cramion::scripting::native

#endif  // CRAMION_CORE_SCRIPTING_NATIVE_MODULES_H

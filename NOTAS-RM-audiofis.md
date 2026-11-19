# Notas del bloque AUDIO + FISICA (rama rm-audiofis)

Base: `horizonz-1031` (102bd30). Worktree `D:/Trabajo/NewProyects/Cramion-rm-audiofis`, build `build-rm` (Ninja, Release, clang++ de LLVM, `CRAMION_ANDROID=OFF`).

## Entrada para CHANGELOG.md (estilo de la 2.7.1)

### Buses de audio (mezclador)
- **Buses como el AudioMixer de Unity / los Submix de Unreal** (`audio/Audio.h`, `Audio.cpp`): siempre existen `Master` y, colgando de el, `Musica`, `Efectos`, `Ambiente`, `Voces` y `UI`; el juego anade los suyos (`addBus(nombre, padre)` o simplemente escribiendo un nombre nuevo en `AudioSource::bus`, que se crea bajo Master). Cada bus tiene volumen (lineal 0..4 o en dB), silencio y padre; lo que llega a un bus es el producto de su volumen, el de sus padres, el silencio y el ducking. Los nombres no distinguen mayusculas y aceptan los de Unity/Unreal en ingles (`Music`, `SFX`, `Ambient`, `Voice`...).
- **`AudioSource.bus`** (Inspector: *Bus*; vacio = *Efectos*). Los sonidos sueltos (`playOneShot`) van por Efectos; **la lluvia, el viento y los truenos del clima van por Ambiente**. Una voz con su bus callado deja de ocupar una voz real (se virtualiza y vuelve donde tocaria).
- El volumen del bus se multiplica en la ganancia de cada voz (`VoiceFx::gain`, que ya se fundia por bloque: sin chasquidos) y tambien afecta a lo que manda a la reverberacion. **Con los valores por defecto (todo a 1) el producto es 1 y se oye exactamente igual que antes** (comprobado en `CramionScriptingTests`).
- **Ducking** (sidechain): `addDucking("Voces", "Musica", -12, ataque, liberacion, umbral)`: mientras suena algo por un bus (o sus hijos), el otro baja esos dB con ataque y liberacion lineales en dB.
- **Instantaneas** (Snapshots): `setSnapshot("Pausa", {Musica = 0.3, Efectos = 0})`, `saveSnapshot("Normal")` y `transitionToSnapshot(nombre, segundos)` (fundido en dB).
- Los cambios de los buses duran lo que el `AudioSystem` (no los toca `stop()`: son las opciones del juego y sobreviven a los cambios de escena); `resetBuses()` lo deja como al empezar.
- API de scripts: `Audio.setVolume/getVolume` (el bus Master), `Audio.setBusVolume/getBusVolume`, `setBusVolumeDb/getBusVolumeDb`, `setBusMute/isBusMuted`, `getBusGain`, `getBuses`, `addBus`, `setBusParent`, `resetBuses`, `addDucking/removeDucking`, `setSnapshot/saveSnapshot/transitionToSnapshot/getSnapshots`. En C++: `AudioSystem::setBusVolume`... y el SDK (`Api.gen.h`, `Audio::setBusVolume(...)`).

### Consultas de fisica 3D
- **`boxCast`, `capsuleCast`, `sphereCastAll`, `boxCastAll`, `capsuleCastAll`, `overlapCapsule`** en `PhysicsSystem` (con `CastShape`/`CollideShape` de Jolt), con mascara de capas, `ignore` y triggers como `raycast`. La capsula va como en Unity (centros de sus dos semiesferas y radio). Los `*All` dan un impacto por cuerpo, del mas cercano al mas lejano.
- API de scripts: `Physics.boxCast`, `capsuleCast`, `raycastAll`, `sphereCastAll`, `boxCastAll`, `capsuleCastAll`, `overlapBox`, `overlapCapsule` (los argumentos de cola `mascara, ignorar, triggers` como en `raycast`). SDK de C++ a mano en `Script.h` (`Physics::boxCast(...)` con `RaycastHit*`, `Physics::raycastAll(...)` -> `std::vector<RaycastHit>`...).
- Los gizmos de consultas del editor las dibujan con los tipos de siempre (linea del recorrido y la forma donde acaba).

### Juntas de fisica (Jolt)
- **Componentes `FixedJoint`, `HingeJoint`, `SliderJoint`, `DistanceJoint`, `SpringJoint` y `ConfigurableJoint` (6DOF)** (`physics/Joints.h`, `src/physics/Joints.cpp`, `JointSet.h`), con las constraints de Jolt (`Fixed`, `Hinge`, `Slider`, `Distance`, `SixDOF`). Comun: cuerpo conectado (entidad con cuerpo; vacio = el mundo), anclaje y eje locales, anclaje conectado automatico o a mano, fuerza y par de rotura, y *Chocan entre si* (por defecto no, como Unity).
- **HingeJoint**: limites en grados, muelle hacia un angulo (rigidez/amortiguacion), motor (velocidad y par maximo) y friccion. **SliderJoint**: lo mismo en metros. **DistanceJoint**: distancia minima/maxima (-1 = la de partida) y muelle (Hz). **SpringJoint**: un DistanceJoint con muelle de 2 Hz por defecto. **ConfigurableJoint**: cada eje lineal y de giro Bloqueado/Limitado/Libre, limites, limites lineales blandos y empuje a posicion/giro.
- La pose de partida es la del primer paso simulado (al empezar el Play o al anadir la junta) y se guarda local a cada cuerpo: si la constraint se rehace (cuerpo recreado, cambio de limites de la 6DOF) los angulos y posiciones siguen contando desde ahi. Motor, muelle, limites de bisagra/corredera y distancias se cambian en marcha sin rehacerla (`setField` desde un script).
- **Rotura**: con `break_force`/`break_torque` > 0 se rompe si en un paso aguanta mas; los scripts de la entidad reciben **`OnJointBreak` {joint, force, torque, connected}** y `PhysicsSystem::addJointBreakListener` avisa en C++. `PhysicsSystem::breakJoint`, `repairJoint`, `jointState` (activa, rota, fuerza, par, angulo, posicion), `jointDebug` y `activeJointCount`.
- API de scripts: `Physics.createJoint(entidad, "Hinge", conectado, {anchor = ..., axis = ..., use_limits = true, ...})`, `Physics.removeJoint`, `Physics.breakJoint`, `Physics.repairJoint`, `Physics.jointState`. Tambien con `addComponent("HingeJoint")` + `setField`.
- Inspector y escenas por la reflexion (registradas en *Fisica*); las escenas sin juntas se abren igual. Gizmo en el editor para lo seleccionado: punto de anclaje, linea al punto conectado y eje (rojo si esta rota).
- En modo edicion (fisica sin simular) no se crean juntas.

## Archivos tocados
- Audio: `CramionCore/include/CramionCore/audio/Audio.h`, `CramionCore/src/audio/Audio.cpp`.
- Fisica: `CramionCore/include/CramionCore/physics/PhysicsSystem.h`, `CramionCore/src/physics/PhysicsSystem.cpp` (bloques: helpers `castShape/overlapShape/recordCast` al final de `Impl`, las consultas nuevas tras `overlapBox`, `JointSet` junto a `ignored_pairs`, ganchos en `destroyEntry`, `stepOnce`, `stop`, `update`, `singleStep`, y la API de juntas antes de `setRecordQueries`), `CramionCore/src/physics/PhysicsComponents.cpp` (una linea: `registerJointComponents()`).
- Nuevos: `CramionCore/include/CramionCore/physics/Joints.h`, `CramionCore/src/physics/Joints.cpp`, `CramionCore/src/physics/JointSet.h`, `CramionCore/src/scripting/native/JointsApi.cpp`.
- Scripts: `CramionCore/src/scripting/native/CoreApi.cpp` (dos bloques nuevos: `registerPhysicsShapeQueries` y `registerAudioBuses`, y una llamada a cada uno), `CramionCore/src/scripting/native/Modules.h` (`registerJointsApi`), `CramionCore/sdk/cramion/Script.h` (Physics: boxCast, capsuleCast, *All), `CramionCore/sdk/cramion/Api.gen.h` (regenerado con `cramion_sdkgen`; trae tambien funciones de antes que no estaban regeneradas en la base: tactil, packs de texturas, `Voxel.structureChests`...), `CramionCore/tools/SdkGen.cpp` (las consultas tipadas de `Script.h` en `kSkipMembers`).
- Editor: `CramionEditor/src/EditorPhysics.cpp` (gizmo de juntas, un bloque).
- Pruebas: `CramionCore/tests/PhysicsTests.cpp` (`testShapeQueries`, `testJoints`), `CramionCore/tests/ScriptingTests.cpp` (`testAudioBuses`), `CramionCore/tests/native/CoreApiTests.cpp` (API de buses y de consultas).

## CVars
Ninguna nueva. Nada cambia el comportamiento de antes: con los buses a 1 se oye igual, las consultas son nuevas y las juntas solo existen si se anade su componente.

## Pruebas y resultado
Compilado en `build-rm` (Release, clang++): `CramionCore`, `CramionEditor`, `CramionPlayer`, `CramionScriptHost`, `cramion_sdkgen` y las pruebas de abajo, sin errores.
- `CramionPhysicsTests`: **172 comprobaciones, 0 fallos**. Nuevas: `testShapeQueries` (boxCast con y sin giro, triggers, capsuleCast de pie y tumbada, sphereCastAll/boxCastAll/capsuleCastAll en orden, mascara vacia, ignorar, overlapCapsule con y sin triggers y degenerada) y `testJoints` (pendulo con bisagra, limites a +-10 grados, motor a 90 grados/s, fija al mundo y encadenada, rotura por peso con evento, corredera con limite, cuerda rigida de 3 m, muelle que se estira, 6DOF con solo Y libre, breakJoint/repairJoint, quitar el componente, jointDebug, cuerpo conectado destruido, stop, unidos sin colision, ida y vuelta por la escena, nombres de tipos).
- `CramionScriptingTests`: **49 comprobaciones, 0 fallos**. Nueva `testAudioBuses` con la mezcla sin dispositivo (RMS): por defecto igual que antes, Efectos a 0.5 = la mitad, Master multiplica, silencio, bus nuevo desde `AudioSource::bus`, padres, ducking de 12 dB y liberacion, instantaneas con fundido en dB (a mitad -10 dB), resetBuses, clima por Ambiente, guardado del campo y escena vieja sin el.
- `CramionApiCoreApiTests`: **149 comprobaciones, 0 fallos** (API de scripts de buses y de las consultas nuevas).
- `CramionVehicleTests`: sin fallos. `CramionCppScriptTests`: 76 comprobaciones, 1 fallo que no es de este bloque (`usa el compilador incluido con el motor (toolchain/)`: el build `build-rm` esta configurado con `CRAMION_BUNDLE_TOOLCHAIN=OFF`); los scripts de prueba compilan con el `Script.h` nuevo.

## Pendiente
- Buses definidos por proyecto en un archivo (`Audio.json` en Project Settings) y una ventana **Mezclador** en el editor con deslizadores y vumetros; hoy los buses del juego se crean por API o al usarlos en `AudioSource::bus`.
- Efectos por bus (paso bajo/reverb en un bus), envios entre buses y medidores de nivel.
- Juntas: `ConfigurableJoint` no tiene drives por eje separados (uno lineal y uno de giro para todos los ejes no bloqueados) ni limites de giro blandos; `HingeJoint` no tiene "free spin". `connectedMassScale`/`massScale` de Unity no estan. Las juntas no se replican por red (cada cliente simula las suyas como el resto de la fisica).
- Gizmo de las juntas: solo punto, linea y eje (sin arcos de limites ni manipuladores para mover el anclaje).

## Riesgos
- `AudioSource` tiene un campo nuevo `bus` que se guarda en las escenas: una escena guardada con esta version y abierta con una anterior pierde el campo (se ignora), sin romperse.
- `PhysicsSystem::Impl::pairIgnored` ahora tambien mira las parejas de las juntas (se lee desde los hilos de Jolt; solo cambia en `sync`, fuera del paso, como `ignored_pairs`).
- Las constraints se quitan en `destroyEntry` y en `stop()` antes que sus cuerpos (si no, Jolt se quedaria con punteros a cuerpos destruidos).

## Probar a mano (GPU / editor)
- Inspector: anadir cada junta en *Add Component > Fisica*, editar campos y guardar/abrir la escena; arrastrar un objeto a *Cuerpo conectado*.
- Play: puerta con `HingeJoint` (limites 0..100), una cadena de `DistanceJoint`, un puente con `FixedJoint` y `break_force`; un script con `on("OnJointBreak", ...)`.
- Gizmos de las juntas con el objeto seleccionado (anclaje, linea al conectado, eje).
- Audio en el juego: `Audio.setBusVolume("Musica", 0)` en un menu de opciones; ducking de Musica con Voces; instantanea de pausa; la lluvia baja con el bus Ambiente.

## Revision (segundo agente)
Revisado el diff `horizonz-1031...rm-audiofis` entero. Lo que estaba mal y se arreglo:
- **Rotura con `collision_steps` > 1**: Jolt da los impulsos de las constraints del ultimo subpaso, y la fuerza se calculaba con el paso entero (`lambda / dt`): con 4 subpasos una junta aguantaba 4 veces su `break_force`. Ahora `afterStepJoints(dt / collision_steps)` (`PhysicsSystem.cpp`).
- **Origen flotante** (Horizonz lo usa): las juntas unidas al mundo guardaban su punto en coordenadas del mundo y `PhysicsSystem::shiftOrigin` mueve todos los cuerpos, asi que el cuerpo se iba de golpe hacia el punto viejo. Nuevo `JointSet::shiftOrigin`: mueve ese punto y rehace la constraint en el siguiente `sync` (antes del paso; la bisagra no pierde el angulo porque Jolt lo mide con los ejes locales).
- **`OnJointBreak` sin oyente**: el oyente de roturas solo se enganchaba en `onStart` si la fisica ya estaba puesta; con `setPhysics` despues de `start` (como en las pruebas de la API) no llegaba ningun evento. Ahora tambien se engancha en la fase Events si la fisica llego o cambio (`JointsApi.cpp`).
- **Ducking que se baja a si mismo**: `addDucking` con un bus y su padre (p. ej. Voces -> Master o Master -> Musica) medía un nivel que ya llevaba la bajada y oscilaba. Ahora devuelve -1 en ese caso.
- **Sonidos sueltos sin bus**: `playOneShot` iba siempre por Efectos, tambien el audio de las lineas de dialogo. Nuevo parametro opcional `bus` en `AudioSystem::playOneShot` y en `Audio.playOneShot(clip, posicion, volumen, bus)`; los dialogos (`GameplayApi.cpp`) van por **Voces**. Con los valores por defecto se oye igual.
- **Editor**: al parar el Play se llama a `audio_.resetBuses()` (`EditorPhysics.cpp`): un juego que bajaba Master o callaba un bus dejaba el editor asi. En los cambios de escena dentro del Play no se toca (opciones del juego).
- SDK regenerado con `cramion_sdkgen` (solo cambia la firma de `Audio.playOneShot`).

Revisado sin cambios: serializacion (campo `bus` nuevo opcional; juntas solo si se anaden; `connected_body` es un campo de entidad y lo remapean los prefabs como los demas), coste por frame sin uso (el mezclador recalcula 6 buses; `syncJoints` recorre 6 vistas vacias), hilos (las parejas sin colision solo cambian fuera de `system->Update`, como `ignored_pairs`), orden de destruccion (constraints antes que cuerpos en `destroyEntry` y `stop`).

Pruebas nuevas de la revision: `testJoints` (bisagra al mundo con origen desplazado 100 m; rotura con 4 subpasos: rompe la de 5 N y no la de 20 N, fuerza medida ~9.8 N), `testAudioBuses` (ducking padre/hijo rechazado; `playOneShot` por Voces con Efectos callado), `testJointsApi` en CoreApiTests (createJoint con opciones y campo desconocido, tipo invalido, jointState, breakJoint -> OnJointBreak una vez con la fisica puesta tras start, repairJoint, removeJoint).
Resultado: `CramionPhysicsTests` 174/0, `CramionScriptingTests` 51/0, `CramionApiCoreApiTests` 159/0, `CramionVehicleTests` 0 fallos, `CramionCppScriptTests` 76/1 (el mismo fallo del toolchain no incluido de `build-rm`). Compilan `CramionEditor`, `CramionPlayer`, `CramionScriptHost` y `cramion_sdkgen`.

Queda (de la revision): las repeticiones (`OneShotListener`) no guardan el bus de un sonido suelto (al reproducirlas va por Efectos); `setBusParent` puede crear despues una pareja de ducking padre/hijo (solo se comprueba en `addDucking`); si la voz no pudo crear su nodo de efectos (`fx_ok` falso, fallo de miniaudio) el volumen del bus no le llega.

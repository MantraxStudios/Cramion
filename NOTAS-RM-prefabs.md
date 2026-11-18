# NOTAS rm-prefabs: prefabs anidados y variantes

Rama `rm-prefabs` (desde `horizonz-1031`). ROADMAP 3.2 «Prefabs anidados y variantes» (P1 · L).

## Entrada de changelog (estilo CHANGELOG.md)

#### Prefabs anidados y variantes
- **Prefabs anidados** (`Prefab.h`, `PrefabNesting.cpp` nuevo): un registro de `"entities"` con `"nestedPrefab": {"uuid", "path", "name"}` es una instancia de otro prefab guardada como enlace, con sus diferencias en `"patches"` (campos, componentes añadidos o quitados, entidades borradas; los hijos añadidos son registros propios cuyo padre es una entidad del anidado). Cambiar el prefab hijo llega a todos los que lo usan y a sus instancias en las escenas. Los ids de las entidades anidadas se derivan del id de la raíz del anidado y del suyo en el hijo: son estables al guardar y al cambiar el hijo, así que los `PrefabLink` y los cambios propios de las instancias siguen valiendo. Al crear o aplicar un prefab, las instancias que tiene dentro se guardan como enlaces solo con **`prefab.SaveNested`** (apagada por defecto: sin ella se siguen guardando como copias sueltas, como antes).
- **Variantes**: `"variantOf": {"uuid", "path", "name"}` arriba del todo. La variante tiene las entidades de su base (mismos ids) con sus diferencias más las suyas; los cambios de la base llegan salvo lo sobrescrito; se pueden encadenar (variante de variante). Un ciclo (variantes en círculo o un prefab dentro de sí mismo, aunque sea a través de otros) se rechaza con un error y el archivo no se toca.
- **Mismo resultado en todas partes**: `readPrefabFile` devuelve el documento ya resuelto (plano, como los de siempre), y también lo resuelven `instantiatePrefab(texto)`, `syncInstance`, `recordOverrides`, `parsePrefabText` y `parsePrefabDocument` (plantilla compilada, `Scene.instantiate`, `Scene.instantiateAsync`, pool). Su `"revision"` mezcla la propia con la de lo que usa: cambiar un hijo o la base deja atrás a las instancias y `syncOutdatedInstances` las pone al día. Las cachés de texto y de plantillas (`ScriptSystem::prefabText`, la de `SceneStreamingApi`) se vacían cuando se escribe un `.crprefab` (`ecs::prefabWriteGeneration`), porque un prefab con anidados cambia aunque su archivo no.
- **Aplicar guarda al revés**: lo que difiere de la base o del anidado queda como diferencia y lo demás se sigue heredando. Una variante conserva su nombre al aplicar.
- **Editor**: *Crear variante* en el menú de un `.crprefab` del Proyecto, en el «...» de la barra azul de la instancia y en Jerarquía → Prefab (la instancia pasa a la variante con sus cambios propios como diferencias); *Abrir la base*, *Desvincular de la base* (la variante pasa a ser un prefab normal con lo que tenía); Jerarquía → Prefab → *Abrir prefab anidado* y *Desvincular prefab anidado*. En el **Inspector**, los campos con cambio propio llevan una barra azul a la izquierda y, con clic derecho en la etiqueta, *Revertir* / *Aplicar al prefab* / *Aplicar a la base* (en la pestaña del prefab: las diferencias respecto a la base o al anidado, con *Revertir al valor de la base* y *Aplicar a la base*).
- **DataPacks y exportación**: los enlaces llevan el UUID del prefab usado, así que `collectDependencies` ya mete los hijos y las bases; la exportación del juego empaqueta Assets entero. El editor y el jugador publican dónde está cada prefab (`ecs::setPrefabLocations`) para los enlaces que no estén en su ruta relativa (DataPacks montados, archivos movidos).
- Retrocompatible: un `.crprefab` sin `nestedPrefab` ni `variantOf` se lee tal cual (ni se parsea: solo se buscan las dos claves) y se escribe igual que antes.

## Archivos tocados
- `CramionCore/include/CramionCore/ecs/Prefab.h`: formato y API nueva.
- `CramionCore/src/ecs/PrefabNesting.h` / `PrefabNesting.cpp` (nuevos): resolver, deshacer la resolución al guardar (diferencias), ubicaciones, variantes, desvincular, orígenes, overrides heredados, CVar.
- `CramionCore/src/ecs/Prefab.cpp`: `readPrefabFile` resuelve; `createPrefab`/`applyInstance` con estructura y `prefab.SaveNested`; overrides sueltos (`revertOverride`, `applyOverride`, `*InheritedOverride`), `createVariantFromInstance`.
- `CramionCore/src/ecs/PrefabSpawn.cpp`: `parsePrefabDocument` resuelve.
- `CramionCore/src/scripting/Scripting.cpp`, `ScriptRuntime.h`, `native/SceneStreamingApi.cpp`: cachés vaciadas con `prefabWriteGeneration`.
- `CramionCore/tests/PrefabTests.cpp`: pruebas nuevas.
- Editor: `EditorApp.h`, `EditorPrefabs.cpp`, `EditorInspector.cpp`, `PropertyInspector.h/.cpp`, `EditorProject.cpp`, `EditorWorkspaces.cpp`; jugador: `player/PlayerMain.cpp` (ubicaciones).
- Ninguno de los compartidos de la lista (Components.h, CoreApi.cpp, Api.gen.h, CMakeLists.txt, VulkanRenderer.cpp, Script.h). Sin API de scripts nueva (no hace falta regenerar el SDK).

## CVars
- `prefab.SaveNested` (bool, Saved, por defecto `false`): al crear o aplicar un prefab, las instancias de otros prefabs que tenga dentro se guardan como enlaces.

## API nueva (C++, `cramion::ecs`)
`readPrefabFileRaw`, `setPrefabLocations`, `locatePrefab`, `prefabWriteGeneration`, `prefabNeedsResolve`, `resolvePrefabText`, `prefabVariantBase`, `prefabDependencies`, `createPrefabVariant`, `createVariantFromInstance`, `detachPrefabVariant`, `unlinkNestedPrefab`, `PrefabOrigin` + `prefabEntityOrigin`, `prefabInheritedOverrides`, `revertOverride`, `applyOverride(…, to_base)`, `revertInheritedOverride`, `applyInheritedOverride`.

## Pruebas
Build propio `build-rm` (Ninja, Release, clang++). Compilan CramionCore, CramionEditor y CramionPlayer (sin avisos nuevos).
- `CramionPrefabTests`: **67 comprobaciones, 0 fallos** (antes 21). Nuevas: anidado de 3 niveles (Rueda > Eje > Coche: enlaces con ruta relativa, ids derivados, referencias internas, cambio del hijo que llega a la escena, override del eje que sobrevive, guardar con diferencias e hijo añadido, desvincular), variantes (variante de variante, override que sobrevive a un cambio de la base, revertir/aplicar un override a la variante y a la base, pestaña del prefab, desvincular, variante desde una instancia), ciclos rechazados (variantes en círculo; prefab dentro de sí mismo a través de otros: el archivo no cambia), paridad JSON / plantilla compilada / por tandas / texto sin resolver en 4 prefabs, dependencias del DataPack y retrocompatibilidad (prefab plano leído tal cual).
- `CramionApiBlueprintTests` 65/0, `CramionApiCoreApiTests` 138/0, `CramionApiDataPackApiTests` 21/0 (sin cambios).

## Pendiente
- Overrides de entidades borradas (`id|-`) y de `+Comp`/`-Comp` en la instancia: se aplican con *Aplicar* (toda la instancia), no sueltos desde el Inspector (el Inspector solo marca campos).
- En la pestaña del prefab, las marcas del Inspector son las diferencias *guardadas* (se rehacen al guardar), no las que aún no se han guardado.
- Jerarquía: no hay icono distinto para las partes anidadas ni para las variantes en el Proyecto.
- Cambiar la base de una variante a otro prefab (re-parent) no está.
- Editar un `.crprefab` hijo a mano (fuera del editor) con el juego en marcha: las cachés de los padres se rehacen por fecha del propio archivo o al escribir un prefab desde el motor; un cambio externo del hijo no invalida al padre hasta que cambie algo más.
- MCP: sin herramientas nuevas para variantes/anidados.

## Riesgos
- `prefab.SaveNested` cambia el formato de lo que se guarda (enlaces): un motor anterior a esta rama no sabe leer esos prefabs (vería entidades sin las del hijo). Por eso va apagada.
- Las variantes se crean solo a petición (menú), pero un prefab variante no lo entiende un motor anterior.
- `"revision"` del documento resuelto de un prefab con anidados/variante es una mezcla (hash), no un número creciente; nada del motor compara revisiones con `<`/`>` (solo `!=`).
- `orderParentsFirst` quita registros cuyo padre desaparece (p. ej. un hijo añadido bajo una entidad que el hijo anidado borró): sale un aviso en la consola.
- Valores nuevos de reflect (campos que el hijo no tenía al guardarse) aparecen como diferencia al aplicar el padre (igual que ya pasaba con los cambios propios).

## Para probar a mano (editor/GPU)
1. Con `prefab.SaveNested 1`: arrastrar dos instancias de un prefab dentro de otro objeto, *Crear prefab*; abrir el `.crprefab` en texto: dos `nestedPrefab`. Cambiar el hijo en su pestaña y guardar: el padre y sus instancias en la escena cambian.
2. Jerarquía → Prefab → *Abrir prefab anidado* sobre una parte anidada; *Desvincular prefab anidado*.
3. Proyecto → clic derecho en un `.crprefab` → *Crear variante*; ponerla en la escena, cambiar un campo, *Aplicar*: el `.crprefab` de la variante guarda solo esa diferencia. Cambiar la base: la variante recibe lo no sobrescrito.
4. Inspector: barra azul en los campos con cambio propio; clic derecho en la etiqueta → Revertir / Aplicar al prefab / Aplicar a la base. En la pestaña de una variante: las diferencias guardadas marcadas.
5. Play con `Scene.instantiate`, `Scene.instantiateAsync` y `Scene.pool/acquire` de una variante y de un prefab con anidados (mismo resultado que en el editor). Parar Play, cambiar el hijo, volver a Play: se ve el cambio.
6. Exportar un DataPack con un prefab con anidados: incluye los hijos; montarlo en un juego y `DataPack.instantiate`.

## Revisión (segunda pasada, revisor independiente)
Revisado el diff `horizonz-1031...rm-prefabs` (núcleo, editor, jugador, pruebas). Lo pedido está cubierto salvo lo ya listado en *Pendiente*. Arreglado:
- **Blueprint aplanaba variantes y anidados** (`Blueprint.cpp`, `writeBlueprintInfo`): leía con `readPrefabFile` (ya resuelto) y lo volvía a escribir, así que guardar los datos de Blueprint de una variante o de un prefab con anidados (y `relocateBlueprintGraph`, que acaba ahí) lo dejaba plano para siempre y le copiaba las entidades de la base. Ahora lee el texto de disco (`readPrefabFileRaw`).
- **Ciclo rechazado a medias** (`Prefab.cpp`, `linkInnerInstances`): con `prefab.SaveNested`, si el ciclo estaba en una instancia de dentro que iba después de otra válida, la de delante ya había perdido su `PrefabInstance` y sus `PrefabLink` (el archivo no cambiaba, pero la escena sí). Ahora primero se comprueba todo y luego se cambia.
- **Texto que no se resuelve** (falta la base de una variante o hay un ciclo): `instantiatePrefab(texto)`, `parsePrefabText` y sobre todo `syncInstance` seguían con el texto sin resolver; `syncInstance` podía borrar de la instancia todo lo que no estuviera en los registros de enlace (sin componentes). Ahora no se instancia ni se toca la instancia (`workingText`).
- **Caché de plantillas de los scripts** (`SceneStreamingApi.cpp`): una lectura en otro hilo lanzada antes de escribirse un `.crprefab` guardaba en la caché la plantilla de antes (con la fecha del archivo del padre, que no cambia), y se quedaba vieja hasta reiniciar. Ahora sirve para lo que se pidió pero no entra en la caché (`ParseJob::writes`).
- **Coste en frames en el editor** (`EditorPrefabs.cpp`, `EditorApp.h`): el submenú Jerarquía → Prefab resolvía el `.crprefab` entero (`prefabEntityOrigin`) en cada frame mientras estaba abierto, y el «...» de la barra de la instancia leía el archivo en cada frame; con prefabs grandes (Valley) se notaba. Ahora pasan por caché (`cachedPrefabOrigin`, `cachedPrefabVariantBase`, vaciadas con `forgetPrefabTexts`). Además `prefabEntityOrigin`, `prefabInheritedOverrides` y `prefabDependencies` no parsean un prefab de los de siempre (solo buscan las claves): la pestaña de un prefab plano ya no lo parsea entero tras cada guardado.
- Pruebas nuevas en `CramionPrefabTests` (sección *Revision*): ciclo detrás de una instancia válida (la escena no cambia), variante sin base (ni JSON ni plantilla, y `syncInstance` no borra nada), `writeBlueprintInfo` en una variante (sigue siendo variante, sin las entidades de la base) y consultas del editor sobre un prefab plano.

Resultado tras la revisión: `CramionPrefabTests` **73 comprobaciones, 0 fallos**; `CramionApiBlueprintTests` 65/0, `CramionApiDataPackApiTests` 21/0, `CramionApiCoreApiTests` 138/0. CramionEditor y CramionPlayer compilan sin avisos nuevos (sin abrirlos).

Visto y sin cambios: los prefabs de siempre se leen y escriben igual (`readPrefabFile` solo busca dos claves); sin coste por frame en el juego (las cachés por fecha solo miran además un contador atómico); `setPrefabLocations`/`locatePrefab` con mutex y `shared_ptr` inmutable (se leen desde los hilos de IO); la revisión mezclada cabe en el rango de `PrefabInstance::revision` (`< 2^30`); `collectDependencies` lee el texto de disco (ve los UUID de los enlaces). No hay Vulkan de por medio.

Riesgos que quedan (además de los de arriba):
- Una variante de un Blueprint hereda el bloque `blueprint` de la base (mismo grafo); *Desvincular de la base* se lo queda como propio.
- Referencias desde una parte anidada a entidades de fuera de ella: al *Aplicar a la base* (al anidado) quedan con ids del padre, que en el hijo no existen.
- `createVariantFromInstance` y «Crear variante» no comprueban si la instancia es la raíz de la pestaña de un prefab.

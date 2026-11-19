# NOTAS RM — ufbx (ROADMAP P1 #1 y morph targets)

Rama `rm-ufbx` (desde `horizonz-1031`). Bloque UFBX de la tanda del 18/11/2026.

## Entrada para el CHANGELOG (estilo de CHANGELOG.md)

### Cargador de FBX con ufbx (detrás de una CVar)
- **ufbx 0.23.1 como cargador de FBX** (`CramionFX/vendor/ufbx`, MIT, un solo `.c`; `UfbxLoader.cpp`), como hizo Godot 4.3. Se elige con la CVar **`asset.fbx.Loader`** (`anterior` | `ufbx`, guardada en `ProjectSettings/CVars.json`). **Por defecto sigue el anterior (assimp)**: la paridad no sale igual en todos los FBX (ver abajo). Con `ufbx` lo usan la importación de assets (pieza animada y partida en piezas, LODs `_LOD0..` por nombre), la reimportación, `loadModel` (con su caché aparte, `.ufbx.cramcache`), `fileUnitScale` (Convert Units), `sourceIsAnimated` y las tomas sueltas de `loadAnimationClips` (retargeting de LocomotionPack). glTF y DAE siguen con assimp; OBJ con el lector propio.
- Da las mismas estructuras que el camino de assimp: jerarquía con la transformación local completa (pivotes incluidos) y la raíz con los ejes del archivo como `correctRootTransform` de assimp, una submalla por material, triángulos de cualquier polígono, vértices fusionados, UV con `v = 1 - v`, tangentes del archivo o calculadas, color de vértice, esqueleto (un hueso por nombre y matriz de enlace) con las 4 influencias de más peso, materiales (las mismas propiedades de FBX que assimp: difuso por su factor, `ShininessExponent` → rugosidad, `Maya|*`, `3dsMax|*`, convenio ORCA `_Specular`, `Bump` como normal map si lo es, opacidad) y texturas incrustadas o en archivo, tomas con las claves desde el inicio de la toma (LocalStart), y el escenario horneado (`force_static`).
- **Mejor que assimp**: lee FBX 6.1 (2006-2010) y archivos que assimp rechaza (274 de 714 del corpus de ufbx), triangula bien los n-gonos cóncavos, respeta la interpolación de las curvas (escalón, Bézier; assimp interpola lineal entre claves), hornea las transformaciones geométricas (pivotes de 3ds Max) en los vértices y compensa la escala sin herencia de Maya. Una toma rota no tumba el archivo: las pistas con valores no finitos se quitan con aviso (lo que hacía `repairAnimations`).
- **Pruebas de paridad**: `CramionFbxTests <carpetas>` (o `CRAMION_FBX_PARITY_DIRS`) carga cada FBX con los dos cargadores y compara triángulos, vértices, materiales, huesos, nodos y su pose en reposo, la piel en reposo, tomas, duraciones y la pose global de los nodos en 5 tiempos, con tolerancia relativa al tamaño del modelo. `--volcar archivo.fbx` enseña lo que sale de cada uno.

### Formas de mezcla (morph targets): importación y guardado
- **Se importan** de FBX (ufbx: canales de BlendShape con sus deltas de posición y normal; assimp: `aiMesh::mAnimMeshes`, también glTF) y **se guardan** en el modelo (`ModelData::blend_shapes`: nombre, peso por defecto, vértices y deltas dispersos; las de varias mallas con el mismo nombre son una) y en el `.crdata` y la caché, en un **bloque `BSHP` versionado** al final de la pieza (detrás del color de vértice): los lectores viejos lo ignoran, una versión futura del bloque se salta entera y un bloque dañado deja el modelo sin formas. Sin formas, los bytes son los de siempre.
- **Pesos animados por toma**: las curvas `DeformPercent` del FBX (ufbx) y las de morph de glTF (assimp) van en `AnimationClip::blend_tracks`.
- `applyBlendShapes` (referencia en la CPU), `sampleBlendShapeTrack`, `findBlendShape`; `scaleModel` escala las deltas. CVar **`asset.import.BlendShapes`** (activa; solo datos: no cambia cómo se ve nada).
- **Falta aplicarlas en la GPU** (ver Pendiente): de momento los personajes con formas se ven con la malla base, como antes.

## Archivos tocados

| Archivo | Qué |
|---|---|
| `CramionFX/vendor/ufbx/{ufbx.c,ufbx.h,LICENSE}` | ufbx 0.23.1 (commit 26a482ae66871d7de36eb722aa060bce95bce274), sin tocar |
| `CramionFX/CMakeLists.txt` | `enable_language(C)` y `ufbx.c` en CramionFX (-O2, sin avisos) — bloque localizado tras bc7enc |
| `CramionFX/src/asset/UfbxLoader.{h,cpp}` | nuevo: conversión ufbx → `ModelData`, unidades, tomas sueltas, `ufbxSourceIsAnimated` |
| `CramionFX/src/asset/ModelLoader.cpp` | elección del cargador (`useUfbx`), formas y pistas de morph de assimp, caché aparte con ufbx, `scaleModel` de las deltas, `applyBlendShapes`/`sampleBlendShapeTrack`/`findBlendShape`, `setFbxLoader`/`setImportBlendShapes` |
| `CramionFX/src/asset/ModelCache.cpp` | bloque `BSHP` v1 (escritura, lectura tolerante, salto por tamaño) |
| `CramionFX/include/CramionFX/asset/Model.h` | `BlendShape`, `BlendShapeKey`, `BlendShapeTrack`, `AnimationClip::blend_tracks`, `ModelData::blend_shapes`, `FbxLoader` y funciones nuevas |
| `CramionCore/src/asset/Importer.cpp` | CVars `asset.fbx.Loader` y `asset.import.BlendShapes`; `sourceIsAnimated` con ufbx |
| `CramionCore/tests/FbxTests.cpp`, `CramionCore/CMakeLists.txt` | suite nueva `CramionFbxTests` |
| `CramionCore/tests/MeshTests.cpp` | `testBlendShapeStorage` |
| `THIRD_PARTY_NOTICES.md` | fila de ufbx |

No se tocan Components.h, CoreApi.cpp, Api.gen.h, VulkanRenderer.cpp ni Script.h.

## CVars

- `asset.fbx.Loader` (texto, `anterior` | `ufbx`, Saved). Por defecto `anterior`.
- `asset.import.BlendShapes` (bool, Saved). Por defecto `true` (solo añade el bloque `BSHP` a los `.crdata` de modelos con formas).

## API nueva (C++, CramionFX; no hay API de scripts nueva)

```cpp
struct BlendShape { std::string name; float default_weight; std::vector<std::uint32_t> vertices;
                    std::vector<core::Vec3> position_deltas, normal_deltas; };
struct BlendShapeKey { float time, weight; };
struct BlendShapeTrack { std::uint32_t shape; std::vector<BlendShapeKey> keys; };
// AnimationClip::blend_tracks, ModelData::blend_shapes
std::vector<SkinnedVertex> applyBlendShapes(const ModelData&, const std::vector<float>& weights);
float sampleBlendShapeTrack(const BlendShapeTrack&, float time);
std::int32_t findBlendShape(const ModelData&, const std::string& name);
enum class FbxLoader { Assimp, Ufbx }; void setFbxLoader(FbxLoader); FbxLoader fbxLoader();
void setImportBlendShapes(bool); bool importBlendShapes();
int ufbxSourceIsAnimated(const std::filesystem::path&);  // -1 no aplica, 0/1
```

## Pruebas y resultado

- `CramionFbxTests` (nueva, sin argumentos): **50 comprobaciones, 0 fallos**. FBX ASCII escritos por la propia prueba: caja con dos huesos, forma `Bulge`, material Phong y toma `Wave` (los dos cargadores: piel girada a 1 s, reposo a 0 s, rugosidad de `ShininessExponent`, centímetros); con ufbx además la forma (12 vértices, delta, aplicada), la pista de peso (0.5 a mitad, 1 al final) y `loadAnimationClips`; escenario con `Rock_LOD0`/`Rock_LOD1` (piezas, espacio de cada nodo, `importAny` entero con LODs a mano, horneado); toma rota con un NaN (se quita la pista y el modelo carga); la CVar cambia el cargador. Paridad assimp/ufbx en esos archivos: igual.
- `CramionMeshTests`: **195 comprobaciones, 0 fallos** (14 nuevas de `testBlendShapeStorage`: ida y vuelta del bloque, color de vértice intacto, bytes idénticos sin formas, `.crdata` antiguo, versión futura del bloque, bloque dañado, aplicar, muestrear, escalar).
- **Paridad en FBX reales**: en el repo y en `D:/Trabajo/NewProyects` **no hay ningún `.fbx`** (búsqueda completa, también en build y worktrees; los modelos de ejemplo van en zips que no están). Se usó el corpus de pruebas de ufbx 0.23.1 (`data/`, 714 FBX sin contar los 694 de `fuzz/`; no se copia al repo). Resultado, cada archivo en su proceso, pieza animada y partida en piezas:

| Resultado | FBX |
|---|---|
| Iguales con los dos | 311 |
| Solo los lee ufbx (FBX 6.1 de 2006-2010, binarios que assimp rechaza: "FBX-DOM unsupported", "No meshes remaining"...) | 274 |
| Solo los lee assimp (`synthetic_node_depth_fail`, `synthetic_unsupported_cube_fail_8000`: ufbx los rechaza a propósito) | 2 |
| Ninguno (rotos a propósito) | 10 |
| Sin probar (nombre con caracteres Unicode que el guion no pasó bien) | 2 |
| Con alguna diferencia | 115 |

Diferencias (líneas; un archivo puede tener varias): pose distinta en una toma 57 (curvas escalón/Bézier o capas de animación: ufbx sigue la interpolación de FBX, assimp interpola lineal entre claves; p. ej. `maya_anim_linear`: con una clave "Constant" assimp desliza y ufbx salta), nodos en reposo distintos 47 (orden y espacio de rotación `casegen_rotation_*`, escala sin herencia de Maya, transformaciones geométricas de 3ds Max, cámaras y luces), caja de vértices distinta 31 (transformaciones geométricas `max_geometry_transform*`, `max_transformed_skin`, n-gonos), número de vértices 28 y de triángulos 14 (n-gonos cóncavos que assimp triangula mal: `blender_300_ngon_irregular` 7 / 41 triángulos), huesos sin peso 12 (assimp crea su entrada; ufbx no), materiales usados 12 y con textura 4 (materiales sin conectar / capas), tomas 6 y duración 4 (tomas sin curvas que assimp descarta o con LocalStop distinto de las claves), piel en reposo 8 (pivotes geométricos en mallas con piel). Revisados a mano con `--volcar`: en todos los mirados ufbx da lo que dice el archivo y assimp no (los FBX del corpus vienen con su referencia `.obj` de ufbx). Los FBX "normales" de Blender salen iguales (cubo, escena por defecto, Suzanne, color de vértice, conjuntos de UV, texturas incrustadas, piel parcial, formas con pesos animados); el personaje `blender_293_barbarian` solo difiere en 2 entradas de hueso sin peso.

Arreglado por la paridad: assimp gira la raíz con los ejes del archivo (`correctRootTransform`) y no la escala con UnitScaleFactor (lo lee como double y lo guarda como float); ufbx hace lo mismo ahora. Los Transform de los clusters son malla→hueso (como los escriben el FBX SDK y Blender).

- Los 694 FBX de `fuzz/` (archivos rotos a propósito): ningún cuelgue ni caída de ninguno de los dos cargadores.

Por eso **`asset.fbx.Loader` arranca en `anterior`**: el criterio era "igual en todos los FBX encontrados" y en el corpus no lo es (casi siempre porque ufbx acierta donde assimp no, pero cambia lo que ya se ve en proyectos importados).

## Pendiente

1. **Formas de mezcla en la GPU** (no hecho). Plan: en `VulkanRenderer` por modelo con `blend_shapes`, subir las deltas dispersas a un storage buffer (`vertex`, `delta_pos`, `delta_nrm`, con un rango por forma) y, por instancia, un buffer de pesos; un compute (`morph.comp`, antes de la pasada de geometría y de `rt_skin.comp`) escribe `posición/normal base + Σ peso·delta` en un buffer de vértices por instancia que `skinned.vert` (G-buffer, sombras, vidrio) y `rt_skin.comp` leen en vez del compartido; sin formas activas (todos los pesos 0) se usa el de siempre. El LOD/meshlets no aplican a modelos con piel, así que basta con el camino de `SkinnedPass`. Pesos por entidad: campo `blend_weights` (vector) en el componente del modelo animado (`Components.h`), `setBlendShapeWeight(nombre|índice, peso)` / `getBlendShapeWeight` / `blendShapeCount` en `CoreApi.cpp` + SDK regenerado con `cramion_sdkgen`; el `Animator` muestrea `blend_tracks` de la toma activa (`sampleBlendShapeTrack`) y los suma a los pesos del script.
2. **Clips sueltos (`.cranim`, `extract_clips`)**: no guardan `blend_tracks` todavía (solo el `.crdata` del modelo).
3. **Formas al partir en piezas**: la importación por piezas (`importModelHierarchy`) no las importa (partir remapea los vértices); solo la pieza animada (personajes), que es donde están.
4. **In-betweens de FBX** (varias formas por canal, `FullWeights`): se toma la forma de peso 100.
5. **Cambiar el valor por defecto a `ufbx`**: revisar en el editor con modelos reales del proyecto (ver abajo) y decidir; con proyectos ya importados, reimportar puede mover/girar algo en los casos de la tabla.

## Riesgos

- Con `asset.fbx.Loader = ufbx` un FBX reimportado puede cambiar respecto a assimp: n-gonos (más triángulos), pivotes geométricos de 3ds Max (malla en otro sitio dentro de su nodo), curvas escalón/Bézier (pose distinta entre claves), escala sin herencia, materiales sin conectar. Por eso va apagado.
- Huesos sin ningún peso: assimp crea su entrada y ufbx no (`blender_293_barbarian`: 56 / 54). No afecta al dibujo; el Animator mueve nodos.
- El bloque `BSHP` hace más grandes los `.crdata` de personajes con muchas formas (deltas dispersas, 28 bytes por vértice movido). Se puede apagar con `asset.import.BlendShapes`.
- `ufbx.c` se compila como C en CramionFX (`enable_language(C)`): en Android/Linux hace falta un compilador de C en la toolchain (el NDK y gcc/clang lo traen).
- `Model.h` cambia (miembros nuevos al final de `AnimationClip` y `ModelData`): todo lo que incluye Model.h se recompila; no se compiló CramionEditor/CramionPlayer porque no se tocó su código.

## Qué probar a mano (editor/GPU)

1. Con `asset.fbx.Loader ufbx` en la consola: importar un personaje de Mixamo (con piel y tomas), uno de 3ds Max con pivotes, un escenario de Blender (ejes Z arriba) y un FBX con `_LOD0/_LOD1`; comparar con el mismo importado con `anterior` (tamaño, giro, materiales, texturas incrustadas, LODs, tomas en el Animator, retargeting con LocomotionPack).
2. Reimportar un modelo ya importado con cada cargador y que la escena/prefab que lo usa siga igual.
3. Un FBX con formas (cara con blend shapes): que importe sin errores, que el `.crdata` crezca y que se vea igual que antes (la GPU todavía no las aplica). Abrirlo con un editor de la versión anterior: debe cargar igual (ignora el bloque).
4. `loadModel` de una escena de demostración con un FBX suelto, con los dos cargadores (cachés `.cramcache` y `.ufbx.cramcache` separadas).

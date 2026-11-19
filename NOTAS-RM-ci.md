# NOTAS rm-ci — CI y calidad (CTest, GitHub Actions, sanitizers, fuzzing, dependencias fijas)

Rama `rm-ci` (desde `horizonz-1031`, 102bd30). ROADMAP: tabla punto 9 («No hay CI ni CTest»), 3.6 «CI con CTest», «Sanitizers y fuzzing de formatos externos», «dependencias sin versión fija».

## Entrada de changelog (estilo CHANGELOG.md)

### CI y calidad: CTest, GitHub Actions, sanitizers y fuzzing
- **CTest**: los ~60 ejecutables de pruebas (`CramionCore/tests`, `tests/native`, `CramionEditor/tests`, `CramionUpdater/tests`) son tests de CTest con etiquetas y tiempo máximo (`cmake/CramionTests.cmake`): `rapidos` (sin GPU, sin ventana, sin Internet, sin sonido), `lentos` (compilan scripts de C++, generan mundos, empaquetan un APK), `editor`, `red` (solo `127.0.0.1`) y `gpu` (`cramion_stereo_check`, `cramion_render_bench`: ventana invisible, nunca en la CI). Objetivo `cramion_tests` (compila todas las pruebas sin GPU) y presets de pruebas `rapidos`, `todas-sin-gpu`, `gpu`, `rapidos-asan`, `fuzz`, `ci-windows`, `ci-linux`: `ctest --preset rapidos`. Todas corren con `CRAMION_TESTS_SILENT=1`: `CramionScriptingTests` ya no abre los altavoces. Manual nuevo: `PRUEBAS.md`.
- **GitHub Actions** (`.github/workflows/ci.yml`): Windows con LLVM/Clang + Ninja + Vulkan SDK (shaders normales y del modo compatible, librerías, pruebas `rapidos` y `cramion_sdkgen` sin diferencias en `CramionCore/sdk`) y Linux con Clang + Ninja (CramionCore, `CramionServer`, `crwpack` y las pruebas del núcleo y del servidor). Caché de compilación con sccache/ccache.
- **Sanitizers** (`-DCRAMION_SANITIZE=ON`, preset `clang-asan`): ASan + UBSan en todo el motor y sus dependencias (`cmake/CramionSanitize.cmake`); CTest pone la DLL de ASan en el `PATH`; supresiones de UBSan solo para terceros (`cmake/ubsan.supp`: ENet, Detour). Apagado por defecto: el build normal no cambia.
- **Fuzzing con libFuzzer** (`-DCRAMION_FUZZ=ON`, preset `clang-fuzz`): `cramion_fuzz_crwpack`, `cramion_fuzz_datapack` (también el contenedor `.crpack`), `cramion_fuzz_scene` (`.crscene`/`.crprefab`), `cramion_fuzz_crdata` y `cramion_fuzz_net` (valores, posiciones y la sesión como servidor y cliente). Cada objetivo escribe sus semillas a partir del formato actual (`--seeds=`). En Windows el build de fuzzing va con el CRT estático (`/MT`), sin DLSS y sin las anotaciones de contenedores de la STL, como la libFuzzer que trae LLVM.
- **Fallos reales encontrados por el fuzzing y ASan, arreglados**:
  - `.crpack`/`.datapack` (`Pack.cpp`): un tamaño enorme en el índice hacía `reserve()` de terabytes (`bad_alloc`, cierre del juego); ahora se reserva como mucho 64 MiB y se corta si lo descomprimido pasa del tamaño del índice (también al extraer: no se llena el disco). Un número de archivos que no cabe en el índice se rechaza antes de reservar. Una ruta con UTF-8 no válido hacía lanzar `std::system_error` a `std::filesystem::path` (cierre): ahora es un paquete dañado.
  - `.datapack` (`DataPack.cpp`): un manifiesto que no es un objeto o con un campo de otro tipo (`"files": 5`) lanzaba `type_error` de nlohmann (cierre): ahora «no es un DataPack», comprobando los tipos sin excepciones. Una ruta absoluta en `files` (`C:/...`, `/...`, `C:relativa`) o con UTF-8 no válido ya no hace fallar el montaje entero: esa entrada se salta y el resto se monta; nada se escribe fuera de `Assets`.
  - `.crdata` y caché de modelos (`ModelCache.cpp`, `CrData.cpp`): los contadores de vectores, cadenas, materiales, texturas, nodos, huesos, animaciones y canales se comprueban contra los bytes que quedan en el archivo antes de reservar (un `.crdata` dañado pedía cientos de GB). El cielo HDR (`readEnvironment`) ya no reserva un tamaño enorme fuera de un `try` (cierre por `bad_alloc`).
  - `CramionApiMeshApiTests`: un `for` sobre `t.get(...).items()` recorría un temporal ya destruido (uso tras liberar).
- **Red**: `NetworkSession::receiveForTest(rol, de, paquete)` procesa un paquete sin red (pruebas y fuzzing; con una sesión abierta no hace nada).
- **SDK de C++ al día**: `Api.gen.h` regenerado con `cramion_sdkgen` (le faltaban `Game.isEditor`, los controles táctiles de `Input`, `Network.sendFast` y funciones nuevas de `Voxel` de 1.0.31); la CI comprueba desde ahora que no se desfase.
- **Dependencias con versión fija**: stb (commit `4e9b368`, el que usaba `build-release`) y FidelityFX FSR 1 (commit `a21ffb8`, FSR 1.0.2) por URL del commit con SHA256, en vez de `GIT_TAG master`. Las demás ya iban a una etiqueta o commit. `THIRD_PARTY_NOTICES.md`: shaderc y SPIRV-Tools (van dentro de `shaderc_shared.dll`, que se reparte con el editor y los juegos), FSR 1, llvm-mingw y los commits de stb/FSR 1; SPIRV-Cross no se enlaza (solo cabeceras en `vendor/include`).
- Pruebas: `CramionApiInputApiTests` cuenta las funciones por módulo (68 de Input con las 12 de los controles táctiles de 1.0.31); `CramionBuildConfigTests` y `CramionAndroidBuildTests` reciben sus argumentos desde CTest (antes fallaban sin ellos); `CramionPackTests` prueba los paquetes y `.crdata` dañados.

## Archivos tocados

Nuevos:
- `cmake/CramionTests.cmake` — registro de pruebas (`cramion_test`, `cramion_register_tests`, objetivo `cramion_tests`).
- `cmake/CramionSanitize.cmake` — `CRAMION_SANITIZE`, `CRAMION_FUZZ`, runtime de ASan. `cmake/ubsan.supp`.
- `CramionCore/fuzz/` — `CMakeLists.txt`, `FuzzCommon.h`, `FuzzCrwPack.cpp`, `FuzzDataPack.cpp`, `FuzzScene.cpp`, `FuzzCrData.cpp`, `FuzzNet.cpp`.
- `.github/workflows/ci.yml`, `PRUEBAS.md`, este archivo.

Modificados (bloques pequeños y localizados):
- `CMakeLists.txt` (raíz): `enable_testing()` + dos `include` tras `CMAKE_EXPORT_COMPILE_COMMANDS`; `cramion_register_tests()` al final (y antes del `return()` de Linux).
- `CMakePresets.json`: presets `ci-windows`, `ci-linux`, `clang-asan`, `clang-fuzz` y los `testPresets` (solo se añade).
- `CramionCore/CMakeLists.txt`: include de `src/` para `CramionPackTests`; `add_subdirectory(fuzz)` al final, solo con `CRAMION_FUZZ`.
- `CramionFX/CMakeLists.txt`: stb y FSR 1 con versión fija.
- `CramionCore/include/CramionCore/net/Network.h`, `CramionCore/src/net/Network.cpp`: `receiveForTest`.
- `CramionCore/src/project/DataPack.cpp`, `CramionCore/src/project/Pack.cpp`, `CramionCore/src/asset/CrData.cpp`, `CramionFX/src/asset/ModelCache.cpp`: los arreglos del fuzzing.
- `CramionCore/sdk/cramion/Api.gen.h`: regenerado con `cramion_sdkgen` (no a mano).
- Pruebas: `CramionCore/tests/PackTests.cpp` (+17 comprobaciones), `ScriptingTests.cpp`, `HttpTests.cpp` (su manejador de excepciones de diagnóstico no se instala con ASan), `tests/native/InputApiTests.cpp`, `tests/native/MeshApiTests.cpp`.
- `THIRD_PARTY_NOTICES.md`.

## CVars / opciones / API

- Sin CVars nuevas.
- Opciones de CMake nuevas (apagadas por defecto): `CRAMION_SANITIZE`, `CRAMION_FUZZ`.
- Variable de entorno `CRAMION_TESTS_SILENT` (la pone CTest): las pruebas no abren el dispositivo de audio.
- API de C++ del motor: `net::NetworkSession::receiveForTest(NetRole, std::uint32_t, const std::string&)`. Nada nuevo en la API de scripts.

## Pruebas y resultado

Builds propios: `build-rm` (Release, Clang 23, Ninja, como `build-release`), `build-asan` (`CRAMION_SANITIZE`) y `build-fuzz` (`CRAMION_FUZZ`).

- **`ctest --test-dir build-rm -L rapidos -LE gpu -j 1`: 58 de 59 pasan (68 s).** Falla solo `CramionFeatures21Tests`, que ya fallaba en la base sin estos cambios: dos comprobaciones del lightmapper del editor, «(d) cada carta con su densidad» (cartas a 1.27 y 1.34 con el tope en 1.25) y «con un rebote más, aumenta» (0.651 frente a 0.652). No es de este bloque; la CI de Windows la verá en rojo hasta que se arregle.
- `lentos`: `CramionCppScriptTests` (con el SDK regenerado), `CramionTemplateTests`, `CramionAndroidBuildTests` (ahora con sus argumentos; APK y AAB reales con el Android SDK local) pasan; `CramionTerrainTests` pasó a `rapidos` (3 s). `CramionVoxelTests` falla, ya en la base: comprobaciones de tiempo («< 4 ms por frame», «se generan rápido», con otras compilaciones en marcha) y de contenido del generador («densidad 1… bloque a bloque», «charcas con nenúfares», «el terreno lejano…»).
- **ASan + UBSan** (`build-asan`, CRT dinámico): 54 de 59 `rapidos`. Además de `Features21`: `CramionWorldPartitionTests` (comprobación de tiempo «menos de 2 ms», más lenta con ASan); `CramionApiCoreApiTests`: **uso tras liberar dentro de miniaudio 0.11.25** (`ma_resource_manager_data_buffer_node_acquire`: al cargar un archivo que no existe libera el nodo y después lee `pDataBufferNode->isDataOwnedByResourceManager`, miniaudio.h ~70917-70926; de terceros, sin tocar); `CramionApiConsoleTests` y `CramionApiVisualScriptRuntimeTests` caen dentro de `_CxxThrowException` al lanzar sus excepciones de error de script (`Console.cpp:306`, `VisualScriptRuntime.cpp:1212`). Con un ejemplo mínimo (nlohmann, ASan + UBSan, /MD) las excepciones funcionan, así que queda por mirar si es del motor o de ASan en Windows. `CramionNavigationTests` y `CramionApiMeshApiTests` pasan tras las supresiones de Detour y el arreglo de la prueba.
- **Fuzzing** (ASan + UBSan, 3-4 min por objetivo): `net` ~2,1 M ejecuciones, `scene` ~94 mil, `datapack` ~50 mil, `crwpack` ~30 mil y `crdata` ~24 mil, **sin fallos tras los arreglos** (antes: 2 en `.crdata` y 3 en `.datapack`/`.crpack`, todos arreglados, reproducidos con su caso y con prueba en `CramionPackTests`). `ctest --test-dir build-fuzz -L fuzz`: humo de 20 s por objetivo (pasan crwpack, scene, crdata y net; datapack pasó en la pasada de 4 min tras el último arreglo).
- `cramion_sdkgen` sobre una copia del SDK de la base daba diferencias (SDK desfasado): regenerado; ahora la comprobación de la CI pasa en local.
- El workflow se validó como YAML (pyyaml) y los presets con `cmake --list-presets`; no se ejecutó en GitHub (sin push).

## Pendiente

- **Linux**: el job de Linux no se pudo compilar en local (no hay Linux ni WSL); la lista `LINUX_TESTS` del workflow es la que debería compilar, pero hay que mirar la primera pasada.
- Android en la CI (NDK) y la comprobación del manual (`cramion_docgen`).
- ASan: miniaudio (subir de versión o parche), las excepciones de `Console`/`VisualScriptRuntime` con ASan, y dejar `rapidos-asan` en verde para ponerlo en la CI.
- Las comprobaciones de tiempo (`WorldPartition`, `Voxel`) deberían relajarse con sanitizers o en máquinas cargadas (el define `CRAMION_SANITIZE` ya existe).
- Fuzzing más largo (horas) y un diccionario para el JSON de escenas; objetivos para `.crcontroller`/`.crui` y las tramas de WebSocket.
- Los fallos previos de `CramionFeatures21Tests` (lightmapper) y `CramionVoxelTests` (no son de este bloque).

## Riesgos

- `ModelCache.cpp` (carga de modelos) comprueba los contadores contra el tamaño del archivo: un `.crdata`/`.cramcache` válido nunca pide más de lo que queda (los datos van detrás), así que con archivos buenos no cambia nada; se mira el final del archivo una vez por pieza (un `seekg`).
- `readDataPackManifest`: un manifiesto con un campo de tipo inesperado ya no se acepta (antes cerraba el juego); `bytes` no entero se deja en 0.
- `Api.gen.h` regenerado: solo añade funciones y renombra dos parámetros (`min`/`max` → `min_`/`max_`); los scripts siguen compilando (`CramionCppScriptTests` y `CramionTemplateTests` pasan). Otros bloques que regeneren el SDK tendrán conflicto aquí: se resuelve regenerando.
- El build de fuzzing en Windows (`/MT`, sin DLSS) no sirve para las pruebas normales: en él solo se registran las de fuzzing.
- La CI no se ha ejecutado nunca: las acciones de terceros (`jakoch/install-vulkan-sdk-action`, `hendrikmuhs/ccache-action`, `seanmiddleditch/gha-setup-ninja`) y la versión del Vulkan SDK pueden necesitar ajustes en la primera pasada.

## Probar a mano (GPU / editor)

- `ctest --preset gpu` en un PC con GPU (abre una ventana invisible: `GpuStereoCheck`, `GpuRenderBench`); no se pasó aquí por la regla de no abrir ventanas.
- Subir la rama a GitHub y mirar la primera pasada del workflow (no se hizo push), sobre todo la instalación de LLVM y del Vulkan SDK y el job de Linux.
- Abrir en el editor un proyecto con modelos importados (`.crdata`) y montar un DataPack de verdad en Horizonz: los cambios solo afectan a archivos dañados, pero conviene verlo con datos reales.

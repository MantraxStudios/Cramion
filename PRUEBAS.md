# Pruebas, CI, sanitizers y fuzzing

Cómo se prueban el motor y sus formatos. Las pruebas son ejecutables de consola (sin ventana, sin GPU y sin sonido salvo las marcadas `gpu`) registradas en **CTest**; la CI de GitHub (`.github/workflows/ci.yml`) las compila y las pasa en cada push y pull request.

## CTest

Cada ejecutable de pruebas (`CramionCore/tests`, `CramionCore/tests/native`, `CramionEditor/tests`, `CramionUpdater/tests`) es un test con etiquetas y tiempo máximo. El registro está en `cmake/CramionTests.cmake` (`cramion_test(...)`); una prueba nueva se añade allí con su etiqueta.

| Etiqueta | Qué es | ¿CI? |
|---|---|---|
| `rapidos` | Sin GPU, sin ventana, sin Internet y sin sonido. Segundos. | Sí (Windows; en Linux las del núcleo y el servidor) |
| `lentos` | Sin GPU, pero tardan: compilan scripts de C++ (`CramionCppScriptTests`, `CramionTemplateTests`), generan mundos (`CramionVoxelTests`, `CramionTerrainTests`) o empaquetan un APK (`CramionAndroidBuildTests`). | No (a mano: `ctest --preset todas-sin-gpu`) |
| `editor` | El código del editor (Inspector, historial, terminal...) en consola. | Sí si además es `rapidos` |
| `red` | Abren sockets en `127.0.0.1` (servidor y clientes en el mismo proceso). Nunca salen a Internet. | Sí |
| `gpu` | Necesitan GPU con Vulkan y abren una ventana invisible (`cramion_stereo_check`, `cramion_render_bench`). | **Nunca** |
| `fuzz` | Prueba de humo de los objetivos de libFuzzer (solo con `CRAMION_FUZZ`). | No |

Todas las pruebas corren con `CRAMION_TESTS_SILENT=1`: las que abrirían el dispositivo de audio de verdad (`CramionScriptingTests`) se saltan esa parte y no suena nada.

```bat
:: Compilar todas las pruebas sin GPU (librerías incluidas)
cmake --build build-release --target cramion_tests

:: Las rápidas (lo que pasa la CI)
ctest --preset rapidos
:: o, con cualquier carpeta de compilación:
ctest --test-dir build-release -L rapidos -LE gpu -j 4 --output-on-failure

:: Todas menos las de GPU (también las lentas)
ctest --preset todas-sin-gpu

:: Las de GPU, a mano y en un PC con GPU (abren una ventana invisible)
ctest --preset gpu
```

## CI (GitHub Actions)

`.github/workflows/ci.yml`, en cada push a `main`, `horizonz-*` y `rm-*` y en los pull requests:

- **Windows** (preset `ci-windows`, carpeta `build-ci`): LLVM/Clang + Ninja + Vulkan SDK de LunarG (`glslc` y `shaderc_shared.dll`). Compila todos los shaders (`CramionFX_shaders`: los normales con `--target-env=vulkan1.3` y los del **modo compatible** con `vulkan1.0 -DCRAMION_COMPAT=1`), las librerías y todas las pruebas sin GPU; pasa `ctest --preset ci-windows` (las `rapidos`) y comprueba que **`cramion_sdkgen CramionCore/sdk/cramion` no cambia nada** (si falla: regenerar el SDK y subirlo).
- **Linux** (preset `ci-linux`): Clang + Ninja; compila `CramionCore`, `CramionServer`, `crwpack` y las pruebas del núcleo y del servidor (`LINUX_TESTS` en el workflow) y las pasa.
- Caché de compilación con sccache (Windows) y ccache (Linux). Si una prueba falla se sube `LastTest.log`.

Sin Android: la exportación (NDK) se sigue compilando a mano.

## Sanitizers (ASan + UBSan)

`-DCRAMION_SANITIZE=ON` (preset `clang-asan`, carpeta `build-asan`) compila todo el motor y sus dependencias con AddressSanitizer y UndefinedBehaviorSanitizer (`cmake/CramionSanitize.cmake`). Apagado por defecto: sin la opción, el build no cambia.

```bat
cmake --preset clang-asan
cmake --build --preset clang-asan        :: cramion_tests
ctest --preset rapidos-asan
```

- Solo con Clang. En Windows, ASan necesita el CRT de Release: `Release` o `RelWithDebInfo`, nunca `Debug`.
- CTest pone en el `PATH` la carpeta de `clang_rt.asan_dynamic-x86_64.dll` y usa `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1` y `UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1` (el primer error hace fallar la prueba).
- Fuera de CTest, el `.exe` necesita esa DLL al lado o en el `PATH` (`C:\Program Files\LLVM\lib\clang\<versión>\lib\windows`).
- Del grupo `undefined` se quita `function` (llamadas por puntero a funciones C de terceros con prototipos compatibles pero no idénticos).

## Fuzzing (libFuzzer)

`-DCRAMION_FUZZ=ON` (preset `clang-fuzz`, carpeta `build-fuzz`; implica `CRAMION_SANITIZE`) añade la cobertura de libFuzzer a todo el código y compila un objetivo por cada formato que llega de fuera (`CramionCore/fuzz`):

| Objetivo | Formato | Qué ejercita |
|---|---|---|
| `cramion_fuzz_crwpack` | `.crwpack` (Store) | cabecera, manifiesto, índice, bloques zstd, CRC, extracción |
| `cramion_fuzz_datapack` | `.datapack` y `.crpack` | índice, entradas zstd, manifiesto, montaje en un `Assets` temporal |
| `cramion_fuzz_scene` | `.crscene` / `.crprefab` (JSON) | `deserializeWorld` (y la ida y vuelta), `parseSceneText` + `restoreParsedEntities`, `pasteEntities` |
| `cramion_fuzz_crdata` | `.crdata` | cabecera, modelos (completo y solo geometría), cielos HDR |
| `cramion_fuzz_net` | paquetes de red | `decodeValue`, `decodePosition` y la sesión (`NetworkSession::receiveForTest`) como servidor y como cliente |

Las semillas no están en el repositorio: cada objetivo las escribe con `--seeds=<carpeta>` (o `CRAMION_FUZZ_SEEDS`) a partir del formato actual, así siguen al formato si cambia.

```bat
cmake --preset clang-fuzz
cmake --build --preset clang-fuzz        :: cramion_fuzz
cd build-fuzz\fuzz
cramion_fuzz_scene --seeds=corpus\scene corpus\scene -max_total_time=600 -timeout=30 -rss_limit_mb=4096
:: humo: 20 s por objetivo
ctest --preset fuzz
```

Un fallo deja el caso en `fallo_<objetivo>_crash-...`; se reproduce con `cramion_fuzz_<objetivo> <archivo>`. Los objetivos que leen archivos escriben cada entrada en `%TEMP%\cramion_fuzz_<pid>` y no escriben nada fuera de esa carpeta.

## Dependencias con versión fija

Todas las dependencias de FetchContent van a una etiqueta de versión o a un commit concreto (stb y FidelityFX FSR 1 por URL del commit con su SHA256). Para subir una: cambiar el commit/etiqueta y, si es una URL, su `URL_HASH` (`cmake -E sha256sum <archivo>`), y actualizar `THIRD_PARTY_NOTICES.md`.

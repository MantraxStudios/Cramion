# Notas del bloque ANDROID / MEMORIA (rama rm-android)

Base: horizonz-1031 (102bd30). Tres partes: presupuesto de RAM + avisos de memoria baja, informe de cierre completo en Android y envio opcional de informes, y texturas ASTC/ETC2 para Android.

## Entrada para el CHANGELOG (estilo de CHANGELOG.md)

### Android y memoria
- **RAM de los modelos con presupuesto** (`AssetManager`, `platform/MemoryPressure.h`): el AssetManager mide lo que ocupa cada modelo en memoria (mallas, texturas, animaciones; lo que RenderSync ya movio a la escena no cuenta) y recuerda cuando se uso por ultima vez. Con un presupuesto (`memory.AssetRamBudgetMB`, o `memory.AssetRamBudgetAuto` segun la RAM del aparato: 256 MB con 2-3 GB, 384 con 4, 512 con 6, 768 con 8, 1 GB con mas) suelta los modelos que nadie mas usa, del usado hace mas tiempo al mas reciente (lo usado en los ultimos 2 s se respeta); si se vuelven a pedir se leen otra vez del `.crdata`. Por defecto sin presupuesto: como antes, nunca se suelta nada.
- **Avisos de memoria baja en Android**: `onLowMemory` llega por la glue (`APP_CMD_LOW_MEMORY`) y `onTrimMemory` se lee cada 2 s con `ActivityManager.getMyMemoryState().lastTrimLevel` (NativeActivity no recibe la llamada de Java). Moderado (`RUNNING_MODERATE`, `UI_HIDDEN`): se sueltan los modelos libres sin usar en 30 s; bajo (`RUNNING_LOW`, `BACKGROUND`) y critico (`RUNNING_CRITICAL`, `COMPLETE`, `onLowMemory`): todos los libres y `mallopt(M_PURGE)`. CVar `memory.TrimOnPressure` (encendida). Se avisa en el log (`[Memoria] Aviso del sistema...`).
- **Cifras**: el perfilador lleva los contadores "RAM modelos (MB)" y "RAM proceso (MB)", y la linea de `[Rendimiento]` del juego termina con `| RAM modelos N MB (m modelos, s soltados, presupuesto ...) | RAM proceso N MB` (al final: lo de antes no se mueve).
- **Informe de cierre completo en Android**: el manejador de senales escribe como el de Linux: motivo, donde fallo (modulo + desplazamiento + funcion), direccion del acceso, contexto (`setCrashContext`), memoria (`/proc/meminfo`, `VmRSS`), la pila (`_Unwind_Backtrace` + `dladdr`) y las ultimas 80 lineas del registro (cada linea de logcat va tambien a las migas), en `Crashes/Game_AAAAMMDD_HHMMSS.txt` de los datos de la app, con la marca `Game.pending` para el siguiente arranque. Pila aparte (`sigaltstack`) para los desbordamientos de pila del hilo del juego y `std::terminate` con el texto de la excepcion. `crashBreadcrumb`, `setCrashContext`, `setCrashLogFile`, `pendingCrashReport` y `readCrashReport` ya no estan vacias en Android. Despues sigue el del sistema (tombstone), como antes.
- **Envio de informes de cierre** (`platform/CrashUpload.h`, apagado por defecto): en *Configuracion de compilacion > Informes de cierre* una direccion https del servidor del proyecto (`crash_report_url=` en game.ini). Al arrancar, el juego (Windows, Linux y Android) pone el informe del cierre anterior en una cola (`Crashes/<juego>.upload`) y la envia en segundo plano: POST JSON `{format: "cramion-crash-1", app, version, platform, report_name, report}` con `X-Cramion-Crash: 1`; lo que falla se reintenta en los arranques siguientes (3 veces). En el editor, con la CVar `crash.ReportUrl`, la ventana del informe tiene un boton *Enviar informe*. Sin direccion no se envia nada. Solo https (http solo a este PC).
- **Texturas ASTC 4x4 y ETC2 para Android** (`asset/MobileTextures.h`; opcion *Texturas de movil* del perfil movil, apagada por defecto): al exportar, cada textura (con sus mips, a partir de la BC1/BC7 de la cache del editor o de los pixeles BC del `.crdata`) se pasa a ASTC 4x4 con astc-encoder 5.3.0 de Arm (Apache 2.0, version y SHA-256 fijados) y, si se pide, tambien a ETC2 RGBA8 (codificador propio) para las GPU sin ASTC; van al paquete como `TextureCache/<clave>.astc` / `.etc2` y lo convertido queda en `Library/Cache/MobileTextures`. En el movil, si la GPU lee el formato (se activan `textureCompressionASTC_LDR` / `ETC2` en el dispositivo), la carga sube la textura tal cual (sin decodificar en la CPU, 4 veces menos RAM/VRAM que RGBA8); si no esta o la GPU no la lee, como antes (RGBA8). CVar `render.MobileTextures` para apagarlo. Los `.crdata`, escenas y prefabs no cambian.
- Pruebas: `CramionMobileTextureTests` y `CramionRamCrashTests`.

## Archivos tocados

Nuevos:
- `CramionFX/include/CramionFX/asset/MobileTextures.h`, `CramionFX/src/asset/MobileTextures.cpp` (ETC2/EAC, envoltorio de astcenc, `.crtx`, claves, cache del juego, exportacion).
- `CramionCore/include/CramionCore/platform/MemoryPressure.h`, `CramionCore/src/platform/MemoryPressure.cpp`.
- `CramionCore/include/CramionCore/platform/CrashUpload.h`, `CramionCore/src/platform/CrashUpload.cpp`.
- `CramionCore/tests/MobileTextureTests.cpp`, `CramionCore/tests/RamCrashTests.cpp`.

Modificados:
- `CramionFX/CMakeLists.txt` (astcenc por FetchContent, solo fuera de Android: libreria `cramion_astcenc`, `CRAMION_HAS_ASTC_ENCODER`).
- `CramionFX/include/CramionFX/asset/Model.h` (`TextureFormat::Astc4x4`, `Etc2Rgba8` al final del enum: solo en memoria, nunca se escriben en un `.crdata`), `Dds.cpp` (`blockBytes`), `BcDecode.cpp` (ETC2 en `decodeBlockCompressed`; ASTC devuelve vacio).
- `CramionFX/src/asset/ModelLoader.cpp` (gancho al principio de `decodeTexture` y `resolveLazyTexture`, solo con la cache de movil activa).
- `CramionFX/src/vk/SkinnedModel.cpp` (formatos de Vulkan ASTC/ETC2 y subida tal cual si la GPU los lee).
- `CramionFX/src/vk/VulkanDevice.cpp` (pide `textureCompressionASTC_LDR` y `textureCompressionETC2` si la GPU los tiene; las consultas ya existian).
- `CramionCore/include/CramionCore/asset/AssetManager.h`, `CramionCore/src/asset/AssetManager.cpp` (`AssetRamStats`, `TrimLevel`, `setRamBudget`, `enforceRamBudget`, `trim`, `ramStats`, `modelBytes`; marca de ultimo uso).
- `CramionCore/CMakeLists.txt` (dos ejecutables de pruebas).
- `CramionDM/include/CramionDM/Window.h`, `CramionDM/src/WindowAndroid.cpp` (`Window::takeLowMemoryEvents`, `APP_CMD_LOW_MEMORY`).
- `CramionEditor/player/AndroidSupport.cpp/.h` (manejador de cierre completo, migas desde logcat, `pollMemoryPressure`).
- `CramionEditor/player/PlayerMain.cpp` (informe pendiente + envio, `setCrashLogFile` en Android, cache de texturas de movil, `tickAssetRam` por frame, campos nuevos de `[Rendimiento]`, CVar `render.MobileTextures`).
- `CramionEditor/src/BuildConfig.h/.cpp` (`crash_report_url`, `android.texture_format`; JSON retrocompatible: sin los campos, los de antes), `EditorBuildConfigs.cpp` (seccion *Informes de cierre*), `EditorAndroidBuild.cpp` (combo *Texturas de movil*), `EditorExport.cpp` + `EditorApp.h` (paso de exportacion de texturas de movil), `EditorCrashReport.cpp` (boton *Enviar informe*).
- `THIRD_PARTY_NOTICES.md` (astc-encoder, Apache-2.0).

No se tocaron: Components.h, CoreApi.cpp, Api.gen.h, VulkanRenderer.cpp, Script.h, CMakeLists.txt raiz. Sin API de scripts nueva (no hace falta regenerar el SDK).

## CVars nuevas

| CVar | Por defecto | Que hace |
|---|---|---|
| `memory.AssetRamBudgetMB` (Saved) | 0 | Presupuesto de RAM de los modelos; 0 = sin limite (como antes) |
| `memory.AssetRamBudgetAuto` (Saved) | false | Presupuesto segun la RAM del aparato (manda sobre la anterior) |
| `memory.TrimOnPressure` (Saved) | true | Atender los avisos de memoria baja del sistema (solo llegan en Android) |
| `crash.ReportUrl` (Saved) | "" | Direccion a la que el editor puede enviar el informe (boton) |
| `render.MobileTextures` (RequiresRestart, solo tiene efecto en Android) | true | Usar las texturas ASTC/ETC2 del paquete si las trae |

game.ini: `crash_report_url=` (solo si la configuracion tiene direccion). BuildConfigs.json: `crash_report_url` y `android.texture_format` (0/1/2).

## API nueva (C++ del motor, no de scripts)

- `assets::AssetManager::{setRamBudget, ramBudget, enforceRamBudget, trim, ramStats, modelBytes}`, `assets::AssetRamStats`, `assets::TrimLevel`.
- `memory::{notifyTrimMemory, notifyLowMemory, takePendingTrim, trimLevelFor, processResidentBytes, physicalMemoryBytes, autoAssetRamBudget, configuredAssetRamBudget, tickAssetRam, lastAssetRamStats, lastProcessResidentBytes}`.
- `crash::{UploadConfig, buildUploadBody, buildUploadRequest, queueReport, queuedReports, uploadQueued, uploadQueuedAsync, platformName, editorReportUrl}`.
- `asset::{MobileFormat, encodeAstc4x4Level, decodeAstc4x4Level, encodeEtc2Rgba8Level, decodeEtc2Rgba8Level, decodeEtc2Rgba8Block, convertToMobileTexture, writeMobileTextureFile, readMobileTextureFile, mobileTextureKey, setMobileTextureCache, loadMobileTexture, mobileTextureCounts, prepareMobileExportTexture}`.
- `dm::Window::takeLowMemoryEvents()` (Android), `android::pollMemoryPressure(vm)`.

## Pruebas

Compilado en `build-rm` (Windows, Release, clang, como build-release; Android apagado): CramionFX, CramionCore, CramionMobileTextureTests, CramionRamCrashTests, CramionTextureTests, CramionPlayer y CramionEditor, sin errores. Android arm64-v8a en `build-android` (NDK 30, android-26, c++_static, Release; lo mismo que hace `cramion_android_arm64-v8a`): `main` (libmain.so) y `cramion_scripthost`, sin errores. No se instalo nada en ningun aparato.

- `CramionMobileTextureTests` (nueva): **TODO OK** (36 comprobaciones). ETC2 128x128 a 37,4 dB, ASTC 4x4 64x48 a 45,2 dB, modos plano y T del decodificador, BC7 con 7 mips -> ASTC/ETC2 con los mismos mips, `.crtx` (ida y vuelta y rechazo de rotos), exportar (incrustada, de archivo y BC del `.crdata`; la segunda vez de la cache) y cargar como en el movil (ASTC sin decodificar, ETC2 solo si la GPU lo lee, tamano maximo quitando mips, sin la cache de movil RGBA8 como antes).
- `CramionRamCrashTests` (nueva): **TODO OK** (42 comprobaciones). Presupuesto y LRU del AssetManager, trim por nivel, avisos de memoria, presupuesto por gama, CVars y `tickAssetRam`; JSON, direcciones permitidas, cola y envio con un servidor falso (nada sale a la red).
- `CramionTextureTests` (la de antes): **TODO OK** (la compresion BC del PC no cambia).

## Pendiente

- El grueso de la RAM de un modelo dibujado esta en la escena (RenderSync lo mueve) y en `RenderSync::loaded_`, que guarda el `shared_ptr` de todos los modelos: el presupuesto solo puede soltar lo que nadie usa (modelos ocultos sin dibujar, lo leido para fisica o miniaturas, lo que quedo de escenas anteriores). Siguiente paso: que RenderSync suelte `loaded_` de los modelos sin instancias y que la escena cuente sus mallas en la misma cifra.
- Mas caches que soltar en un aviso de memoria baja (texturas compartidas en CPU, cache de pipelines en memoria, audio decodificado).
- `onTrimMemory` llega por sondeo (2 s); con una actividad Java propia (GameActivity) llegaria al momento.
- El servidor que recibe los informes (endpoint en `web-api`) no esta hecho: solo el cliente y el formato JSON. Tampoco se pide permiso al jugador en el juego (la decision es del autor del juego al poner la direccion; documentarlo en su politica de privacidad).
- El informe de Android se guarda solo para los cierres del hilo del juego con pila aparte; en otros hilos sin `sigaltstack` un desbordamiento de pila no llega al manejador (si el resto).
- ETC2: el codificador usa solo los modos de ETC1 (sin T/H/plano): calidad de respaldo, por debajo de ASTC. La conversion parte de la BC1/BC7 del editor (doble compresion en lo que era BC1); partir de la imagen original daria algo mas de calidad.
- Texturas que no pasan por los modelos (interfaz, terreno, arboles, VFX) siguen como antes en el movil.
- Linea de `[Rendimiento]` y overlay del editor: el editor no muestra todavia la RAM de los modelos.

## Riesgos

- `TextureFormat` tiene dos valores nuevos al final: codigo que haga `switch` sin `default` sobre el enum da aviso (`createUiTextureCompressed` en VulkanRenderer.cpp, que devuelve 0 igual). Nunca se escriben en un `.crdata` (solo en memoria y en los `.crtx` del paquete).
- La clave de las texturas de movil de las perezosas es la portable (ruta dentro de Assets): la cache del editor va por el nombre de la `.dds` de la cache de BC (lleva fecha), asi que si la imagen cambia se reconvierte.
- `memory.AssetRamBudgetMB` encendido puede hacer que un modelo soltado se vuelva a leer del disco (tiron) si se pide a menudo y nadie guarda su `shared_ptr`.
- El envio de informes usa un hilo suelto (detach) al arrancar; si el juego se cierra antes, se reintenta la proxima vez.

## Probar a mano (GPU / editor / movil)

1. Editor: Configuracion de compilacion Android > Perfil movil > *Texturas de movil* = ASTC 4x4; exportar Horizonz o un proyecto con modelos; mirar en el log `[Exportar] Texturas de movil (...)` y el tamano del APK. Segunda exportacion: rapida (cache).
2. En el movil (Mali/Adreno): log `[Texturas] Texturas de movil del paquete: ASTC si`, que se ve igual, y comparar RAM/VRAM y tiempo de carga con la opcion apagada. Con `render.MobileTextures=0` debe verse como antes.
3. Probar *ASTC + ETC2* en una GPU sin ASTC (o forzando ASTC fuera) para ver el respaldo.
4. Android: provocar un cierre (testCrash) y comprobar `Crashes/Game_*.txt` en los datos de la app (pila con `libmain.so + 0x...`, ultimas lineas) y en el arranque siguiente `[Crash] La sesion anterior se cerro...`.
5. Con un servidor propio de pruebas (https o http a localhost en escritorio), poner la direccion y comprobar que llega el JSON y que la cola se vacia.
6. Android con memoria baja: `adb shell am send-trim-memory <paquete> RUNNING_LOW` y ver `[Memoria] Aviso del sistema (nivel 10)...` en el log; la linea `[Rendimiento]` con la RAM.
7. Editor: con `crash.ReportUrl` puesta, tras un cierre, el boton *Enviar informe*.

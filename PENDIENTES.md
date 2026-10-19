# Pendientes de Cramion

Actualizado: 2026-10-19. El rendimiento va aparte, en `MEJORA.md`, y los lightmaps en `PLAN-LIGHTMAPS.md` (sin empezar).

## Publicación
- **Cramion Store pública:** subir la API al servidor (copiar `web-api/cramion_api.py` y arrancarlo en lugar del `python3 -m http.server`; pasos en `web-api/README.md`). Hasta entonces el Package Manager dice «No se pudo conectar».
- **Próxima versión:** regenerar el zip con `cramion_package` desde `main` (el de la 2.4.13 se hizo antes de las tandas de optimización). Recompilar Android antes del zip, de uno en uno:
  ```
  cmake --build build-release -j 1 --target cramion_android_arm64-v8a
  cmake --build build-release -j 1 --target cramion_android_armeabi-v7a
  ```
- **Juegos exportados:** volver a exportarlos para que lleven el reproductor nuevo.
- **Iluminación, mientras no haya lightmaps:** con el sol en «Mixta» y volviendo a hornear, las sombras salen como en tiempo real y solo se hornea el rebote.

## Funciones a medias
- **Vídeo fase 2:** Android (MediaCodec) y FFmpeg opcional (RTSP/DASH).
- **World Partition fase 1, estilo Unreal:** varias rejillas, Data Layers y dependencias entre celdas.
- **Red** en coordenadas absolutas a más de 2 km.
- **Blueprints fase 2:** Construction Script, funciones, macros, Timeline y depuración en vivo. Renombrar un Blueprint no mueve su `.crgraph`.
- **VR:** el presupuesto adaptativo rehace los samplers al cambiar de escala (pequeño tirón).
- `--run-tests` en el reproductor (`CramionEditor/player/PlayerMain.cpp`) no existe: decidir si hace falta.

## Probar en Windows (port sin Lua; solo se compiló desde Linux)
- Crear un proyecto de cada plantilla y darle a Play; el minigolf.
- El autocompletado y la consola de C++.
- Un mod con `main.crgraph`.
- `<Juego>Server.exe` con scripts.
- El editor de máquinas de estados.
- `CppScriptTests` con el compilador incluido: debe decir «clang (incluido)».
- Exportar a Android con el NDK: el `.so` de los scripts y `libcramion_scripthost.so` deben ir en `lib/<abi>/`.

## Fallos conocidos
- `CramionTemplateTests` «bloques»: tras romper un tronco no se recoge el objeto (3 comprobaciones). La prueba «coches» falla a veces.
- Líneas oscuras finas en los bordes de las paredes (probablemente FSR/TAA).
- El recorte de sombras no usa `base_color.a`.
- Las partículas no se ordenan.
- La corteza de la lava toma el máximo de los materiales.
- Con el escalado apagado, el DOF y el motion blur acaban en el historial que leen SSR y SSGI.

## Recordatorios
- Cerrar `CramionEditor.exe` antes de compilar (si no, falla el enlace).
- Una sola compilación a la vez; Android aparte. Pruebas de una en una.
- Sin commits ni subidas sin pedirlo.
- El MCP del editor puede quedar en el puerto 7778 si el 7777 sigue ocupado (`CRAMION_MCP_PORT=7778`).
- Restos que se pueden borrar cuando no hagan falta: `<scratchpad>/e2e/test` (copia de 3,5 GB del proyecto `test`, para las pruebas del horneado) y `<scratchpad>/store-test` (Store local de prueba). `<scratchpad>` = `C:\Users\tupap\AppData\Local\Temp\claude\D--Trabajo-NewProyects-Cramion\08c827e0-a428-4eb8-8c9d-cc768252f90b\scratchpad`.

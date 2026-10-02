# Pendiente para terminar la 2.1 (2026-10-02)

Estado: **todo el código de la 2.1 está escrito y la compilación completa pasó** (build11 y build12: exit 0, sin errores, incluido Android arm64). No hay nada en commit: todo son cambios locales.

## Reglas para continuar (el PC tiene 16 GB)
- **Un solo agente** y **una sola compilación** a la vez: `cmake --build build -j 4 -- -k 0` (o `-j 2` si el PC sufre).
- **No ejecutar las pruebas en bucle ni en paralelo.** Ejecutar una cada vez y comprobar la RAM antes.
- No editar cabeceras mientras se compila. No hacer commit ni publicar sin preguntar.

## Hecho el 2026-10-02 (tarde)
1. **Pruebas, una a una: todas bien**, salvo los fallos que ya existían (NavigationTests 1, AndroidBuildTests 2).
   - Features21 45/45; TemplateTests 143/143. La prueba esperaba 10 plantillas: ahora espera 12 y juega *Plataformas 2D* (el jugador se apoya en el tilemap, 7 monedas, cámara ortográfica) y *Coches* (4 ruedas en el suelo; a fondo, 39 km/h y 29 m en 4 s). `play()` ahora también mueve System2D.
   - BuildConfigTests: ejecutarlo desde `build/` → `./CramionEditor/CramionBuildConfigTests.exe ./CramionPlayer.exe ../assets/icon.png`.
2. **Documentación**: `cramion_docgen` (85 componentes, 13 páginas), `cramion_sdkgen` (sin cambios) y `build_manual.py` (138 páginas). `plantillas.html` ya incluye *IA con máquinas de estados*, *Plataformas 2D* y *Coches*.

## Paquete 2.1.0 listo (2026-10-02, noche)
- La 2.0 y la 2.1 se publican juntas como **2.1.0** (`CMakeLists.txt`, CHANGELOG, README, web y manual).
- build-release compilado; sus 31 pruebas dan 0 fallos; autoprueba del editor release (desde el paso 58): TODO OK.
- `build-release/Cramion-win64.zip`: 151 MB (79 MB son el compilador de C++ de los scripts). SHA-256 `194378cd6ec5c64051609595efd659f54c9b84d8a540c2d9fb1b6147da9e8c70`.
- `docs/docs.zip` regenerado; notas en `build-release/RELEASE-2.1.0-github.md` y `RELEASE-2.1.0-discord.txt`.

## Solo falta (lo hace el usuario)
- Commit, release `v2.1` en GitHub con el zip, subir `docs.zip` a la web y el anuncio de Discord.

## Ya hecho en esta sesión
- CHANGELOG: sección `## 2.1.0 (sin publicar)`.
- README: «Novedades de la 2.1» y su enlace en el índice.
- Linux: la página del manual (`docs-src/pages/linux.html`) y los mensajes del editor (EditorPlatform.cpp, EditorExport.cpp) ahora dicen la verdad: el reproductor nativo de Linux **aún no existe** (no hay ventana X11, ni preset `linux-release`, ni CMake para Linux); solo está la plataforma en *Configuraciones de compilación*. Mientras tanto, Proton.

## Notas honestas para el informe final
- La iluminación horneada usa **volúmenes de sondas**, no lightmaps por texel (UV2).
- Exportar a la web: solo se evaluó y no se implementó (haría falta un renderer WebGPU).
- Linux nativo: por hacer (WindowX11, la superficie xlib, el `main` del reproductor y el preset de CMake).
- Avisos de clangd en `EditorDestruction.cpp` (`drawPhysicsToolMenu`, `lightingEditor`): son antiguos; la compilación real pasa. Revisarlos solo si vuelven a salir al compilar.

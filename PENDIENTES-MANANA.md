# Pendientes para mañana (15-10-2026)

Estado al cerrar la sesión de la noche del 14 al 15.

**Todo el trabajo hasta ahora está en tus dos commits, ya subidos a `origin/main`:**
- `2fd1af3` «New optimize mode»: la 2.4.13 entera (Cramion Store, arreglo del horneado, `crwpack`, documentación) más la primera tanda de optimización.
- `5f7a7d3` «new update»: la segunda tanda de optimización.

Después de esos commits solo hay dos archivos nuevos sin commitear: `PLAN-OPTIMIZACION.md` y `PLAN-LIGHTMAPS.md` (ver abajo).

He parado todos los agentes. No queda nada compilando ni abierto.

---

## 1. Primero: comprobar que lo commiteado compila

El segundo agente terminó sus cambios y pasó sus pruebas:
- `CramionMeshTests`: 161 de 161.
- `CramionOptimizationTests`: 38 de 38.
- `CramionBuildConfigTests`: 13 de 13.

Lo paré **mientras compilaba el editor y el reproductor**, así que esa compilación quedó a medias. El `CramionEditor.exe` de `build-release` es anterior al commit `5f7a7d3`.

1. Cerrar el editor y lo pesado (juegos, navegador).
2. Compilar:
   ```
   set CMAKE_BUILD_PARALLEL_LEVEL=2
   cmake --build build-release -j 2 --target CramionEditor
   ```
3. Si falla, arreglarlo antes que nada.
4. Pasar todas las pruebas de una en una:
   ```
   python -I <scratchpad>/build/run_tests.py D:/Trabajo/NewProyects/Cramion/build-release tests.log
   ```
   `CramionBuildConfigTests` va aparte: `build-release/CramionEditor/CramionBuildConfigTests.exe build-release/CramionPlayer.exe docs/img/icon.png`.
   - Ya fallaba antes de estos cambios: `CramionTemplateTests` «bloques» (3 comprobaciones: tras romper un tronco no se recoge el objeto). La 2.4.13 no toca ese código.
   - Es intermitente: «coches».

## 2. Versión 2.4.13: decidir qué se publica

**El zip de `build-release/Cramion-win64.zip` (SHA `e1e76a67…0990`) se hizo ANTES de las tandas de optimización.** No coincide con ningún commit, porque `2fd1af3` ya lleva el presupuesto nuevo.

Opciones:
- **(a) Recomendada.** Terminar la optimización fase 1 (punto 3), compilar todo, regenerar el zip con `cramion_package` y publicarlo como 2.4.13. Hay que actualizar el SHA en `release/2.4.13/GITHUB_RELEASE.md` y añadir la optimización al CHANGELOG y a las notas.
- **(b)** Publicar ahora como 2.4.13 lo que hay, y la optimización en la 2.4.14. También habría que regenerar el zip desde `main`.

**Para que funcione la Store pública** hay que subir la API al servidor: copiar `web-api/cramion_api.py` y arrancarlo en lugar del `python3 -m http.server`. Los pasos están en `web-api/README.md`. Hasta entonces, el Package Manager de la 2.4.13 dirá «No se pudo conectar».

Recordatorios para publicar:
- **Android:** el `PlayerMain.cpp` del juego de Android cambió. Recompilar antes del zip:
  ```
  cmake --build build-release -j 1 --target cramion_android_arm64-v8a
  cmake --build build-release -j 1 --target cramion_android_armeabi-v7a
  ```
  con `CMAKE_BUILD_PARALLEL_LEVEL=2`. **No compilar todo junto:** la última vez se acabó la RAM.
- **Juegos exportados:** hay que **volver a exportarlos** para que lleven el reproductor nuevo.

## 3. Optimización para gama baja: fase 1

Plan completo en **`PLAN-OPTIMIZACION.md`** (raíz del proyecto).

| Ítem | Qué es | Estado |
|---|---|---|
| F1-1 | Arreglo de la «media 0» del presupuesto | Hecho y probado (`2fd1af3`) |
| F1-3 | Palanca «Desenfoque de cámara» (DOF + motion blur) | Hecho y probado (`2fd1af3`) |
| F1-4 | Arranque Bajo/Medio más sobrio | Hecho y probado (`2fd1af3`) |
| F1-7 | Resolución interna con suelo y sin saltos | Hecho y probado (`2fd1af3`) |
| F1-9 | Volumétrica en 3 niveles y palanca de nubes | Hecho y probado (`2fd1af3`) |
| F1-11 | Objetos diminutos según la gama | Hecho y probado (`2fd1af3`) |
| F1-2 | El juego exportado se adapta a cada PC (`graphics_lock`, ini v2, RT fuera por debajo de Alto) | Hecho (`5f7a7d3`); falta compilar el editor |
| F1-5 | Log `[Rendimiento]` más útil y capturas de tirones sin parar el juego | Hecho (`5f7a7d3`); falta compilar el editor |
| F1-13 | Render Textures a menos frecuencia en gama baja | Hecho (`5f7a7d3`); falta compilar el editor |
| F1-6 | GI horneada sin el filtro SVGF | **Pendiente** |
| F1-8 | Sol horneado sin cascadas si nadie las lee | **Pendiente** |
| F1-10 | LODs en las variantes `.crmat`, más el arreglo de `drawCpuActors` en modo compatible | **Pendiente** |
| F1-12 | CPU: ríos, vegetación, ondas y flotación sin trabajo repetido | **Pendiente** |

**Después:**
- Verificación completa:
  - todas las pruebas;
  - medir en copias de BlackHouse, test y Keryan con `CRAMION_HARDWARE_TIER=low` y `CRAMION_NO_MESH=1`, con el reproductor viejo y con el nuevo (ver «Cómo medirlo» en el plan);
  - **no tocar** `%LOCALAPPDATA%\Cramion\Saves` ni `Games`: copiar y renombrar.
- Entradas en el CHANGELOG.
- Aún por hacer en la interfaz: probar a ojo el DOF a 32 muestras con TAA, y las nubes a 64 y 32 pasos.

**Cómo retomarlo:** el script del workflow está en `~/.claude/projects/D--Trabajo-NewProyects-Cramion/08c827e0-…/workflows/scripts/cramion-lowend-phase1-wf_faa44226-c86.js`. Es mejor lanzar uno nuevo solo con el grupo «render» (F1-6, F1-8, F1-10, F1-12) y la verificación, porque los grupos 1 y 2 ya están en `main`.

**Preguntas que tienes que decidir:**
- ¿Puede un desarrollador entregar su juego con `adaptive=0`? Ahora solo se puede con la casilla «Respetar los ajustes gráficos del proyecto».
- En gráficas integradas, ¿objetivo 60 FPS, o 30 estables?

## 4. Iluminación horneada profesional (lightmaps UV2)

El horneado actual se ve mal porque no usa lightmaps de verdad:
- el sol horneado va a celdas de 0,5 m pegadas al mundo;
- el rebote sale de sondas cada 2 m.

Lo comprobé con capturas de tu casa en tiempo real, Horneada y Mixta.

**Diseño completo en `PLAN-LIGHTMAPS.md`:**
- UV2 con xatlas, una librería MIT; hay que anotarla en `THIRD_PARTY_NOTICES`.
- 20 texeles/m.
- **El sol siempre se calcula en tiempo real y solo se hornea su sombra (visibilidad) y su rebote.** Así «Horneada» se ve igual que antes de hornear y además no dibuja cascadas (gama baja).

**Fases:**
- **0. Arreglos rápidos:**
  - albedo real de las texturas en vez de ×0,6;
  - cielo real;
  - sin SVGF;
  - comando MCP `set_lighting_mode` para comparar A/B.
- **1. UV2 con xatlas.**
- **2. Sombras del sol horneadas nítidas.** Es la que arregla lo que viste.
- **3. Rebote en los lightmaps.**
- **4. Gama baja.**
- **5. Exportación y Android.**
- **6. Calidad.**

**Mientras tanto, en tu escena:** pon el sol (Luz direccional) en **«Mixta»** y vuelve a hornear. Las sombras salen como en tiempo real y solo se hornea el rebote.

**Para comparar:** las capturas de antes están en `<scratchpad>/e2e/views/`. El script `views.py` pone la cámara en las 3 vistas (exterior, interior1 e interior2). La copia de tu proyecto está en `<scratchpad>/e2e/test`.

## 5. README de lo que falta optimizar (`OPTIMIZACION.md`)

Lo pediste antes de dormir. El análisis se paró: solo había terminado el resumen de estado. Quedaban los 6 buscadores de huecos:
- tirones y carga;
- memoria;
- CPU (física, animación, scripts, hilos);
- GPU no cubierta;
- Android e integradas;
- herramientas de medida.

Para retomarlo, con los agentes ya terminados guardados:
```
Workflow resume: scriptPath …/workflows/scripts/cramion-optimization-gaps-readme-wf_a0baecfc-bd2.js
                 resumeFromRunId wf_a0baecfc-bd2
```
Al terminar, escribir `OPTIMIZACION.md` en la raíz.

## 6. Lo que seguía pendiente de antes

- Ya no aplica:
  - La 2.4.10 está publicada.
  - El horneado «0 mallas» está arreglado en la 2.4.13.
  - El reproductor de vídeo ya compila y `CramionVideoTests` pasa (65 de 65).
- **Vídeo fase 2:** Android (MediaCodec) y FFmpeg opcional (RTSP/DASH).
- **World Partition fase 1 estilo Unreal:** varias rejillas, Data Layers y dependencias entre celdas.
- **Red en coordenadas absolutas a más de 2 km.**
- **Blueprints fase 2:** Construction Script, funciones, macros, Timeline y depuración en vivo. Renombrar un Blueprint no mueve su `.crgraph`.
- **VR:** el presupuesto adaptativo rehace los samplers al cambiar de escala, lo que da un pequeño tirón.
- **Plantilla «bloques»:** el objeto del tronco no se recoge (`CramionTemplateTests`).
- Pendientes del port sin Lua, traídos del antiguo `PENDIENTES.md` (borrado el 15-10-2026):
  - **Probar en Windows lo que solo se compiló desde Linux:**
    - Crear un proyecto de cada plantilla y darle a Play.
    - El minigolf.
    - El autocompletado y la consola de C++.
    - Un mod con `main.crgraph`.
    - `<Juego>Server.exe` con scripts.
    - El editor de máquinas de estados.
    - `CppScriptTests` con el compilador incluido: debe decir «clang (incluido)».
    - Exportar a Android con el NDK: el `.so` de los scripts y `libcramion_scripthost.so` deben ir en `lib/<abi>/`.
  - `--run-tests` en el reproductor (`CramionEditor/player/PlayerMain.cpp`) no existe: decidir si hace falta.
  - **Rendimiento opcional:**
    - Lista de candidatos por luz puntual con sombras.
    - Recordar la última clave de `batch_lookup_`.
    - Menos tablas hash en `RenderSync::updateRipples`.
  - **Detalles gráficos conocidos:**
    - Líneas oscuras finas en los bordes de las paredes (probablemente FSR/TAA).
    - El recorte de sombras no usa `base_color.a`.
    - Las partículas no se ordenan.
    - La corteza de la lava toma el máximo de los materiales.
    - Con el escalado apagado, el DOF y el motion blur acaban en el historial que leen SSR y SSGI.

## 6b. Ciudad gigante: bajones de FPS con objetos «Static» (analizado, sin tocar código)

**Diagnóstico (15-10-2026):**
- `EntityInfo::is_static` **no se usa en tiempo de ejecución**: solo lo leen `StaticBatching.cpp` (al exportar) y `ProbeBaker.cpp`. En el editor y en Play cada objeto Static es un actor suelto. Para el renderer, «estático» = malla rígida (`per_submesh`), con o sin la casilla.
- Cada frame se rehace todo (coste lineal con el número de objetos):
  - `RenderSync::syncActors` (`RenderSync.cpp` ~2946) recorre todas las entidades, consulta `animations_` (hash map), crea un `Animator` incluso para los edificios y vuelve a copiar transform y bounds.
  - `VulkanRenderer::updateActors` (`VulkanRenderer.cpp:1584`) recalcula AABB, LOD y clusters, ordena los lotes y sube `setClusters`.
- **Causa probable de los picos:** la caché de sombras se invalida entera.
  - El movimiento se compara **por posición en la lista** (`VulkanRenderer.cpp:1789-1797`): si un actor entra o sale, todos los de detrás cuentan como «movidos».
  - Si cambia el número de actores (`VulkanRenderer.cpp:1932`), `actor_set_changed_` hace `redraw_all` de las 4 cascadas, `local_static_dirty_` (todas las luces locales) y el reinicio del path tracing.
  - El streaming `render.streaming.GpuResidency` (activado por defecto) descarga y salta modelos (`VulkanRenderer.cpp:1647`) mientras la cámara se mueve, y eso cambia el número continuamente.
  - Las sombras se dibujan **por la CPU, actor por actor** (`recordActorShadows`, `VulkanRenderer.cpp:2086`), y `actorsTouch` compara cada luz con todas las esferas (luces × N).

**Confirmar primero:** perfilador (zonas «Recorrer entidades» y sombras; `CRAMION_SHADOW_MARKS=1`). Prueba rápida: desactivar `render.streaming.GpuResidency`. Si los picos se van, el diagnóstico es correcto.

**Arreglos (por orden):**
1. Comparar el movimiento por identidad estable (`scene_actor` o entidad), no por índice. Que un actor que entra o sale marque solo su propia esfera, sin `redraw_all` ni ensuciar todas las luces. Es pequeño y quitaría la mayoría de los picos.
2. Usar `is_static` en tiempo de ejecución:
   - En RenderSync, saltar la animación de lo estático sin Animator y reutilizar el slot si `Transform::version` no cambia.
   - En el renderer, un buffer persistente de clusters estáticos.
3. Sombras de lo estático por la GPU (draws indirectos con el mismo culling por cluster que usa la vista principal).
4. Opcional: static batching también en el editor y en Play, no solo al exportar.

## 7. Limpieza (cuando ya no haga falta)

- `<scratchpad>/e2e/test`: copia de 3,5 GB de tu proyecto `test`, para las pruebas del horneado. Puede borrarse al terminar los lightmaps.
- `<scratchpad>/store-test`: Store local de prueba con paquetes `com.cramiontest.*`.

`<scratchpad>` = `C:\Users\tupap\AppData\Local\Temp\claude\D--Trabajo-NewProyects-Cramion\08c827e0-a428-4eb8-8c9d-cc768252f90b\scratchpad`

## Recordatorios

- Cerrar `CramionEditor.exe` antes de compilar (si no, falla el enlace).
- Una sola compilación a la vez, con `-j 2`; Android aparte. Pruebas de una en una.
- Sin commits ni subidas sin pedirlo.
- El MCP del editor puede quedar en el puerto 7778 si el 7777 sigue ocupado por un editor que se acaba de cerrar (`CRAMION_MCP_PORT=7778`).

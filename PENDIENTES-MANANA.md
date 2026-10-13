# Pendientes para mañana (13-10-2026)

Estado al cerrar la sesión. Nada está commiteado ni subido.

## 1. Listo para publicar: 2.4.10

- `build-release/Cramion-win64.zip` + `Cramion-win64.zip.sha256`
  (SHA-256 `3b2f31dbabdff89ffb99ec5d31fa9241ab426991b9c077b78be2612c6a12f766`).
- Notas: `release/2.4.10/GITHUB_RELEASE.md` y `release/2.4.10/DISCORD.md`.
- Incluye: cambio de escena arreglado, «Ultra realista» (AgX Punchy), VR nítida,
  exposición automática de noche y horneado de luz.
- **Ojo:** el horneado de luz de la 2.4.10 todavía tiene el fallo del punto 2.
  Mejor publicar después de arreglarlo (o publicar y sacar luego la 2.4.11).
- Falta: commit, etiqueta `v2.4.10` y subir a GitHub (solo cuando lo digas).

## 2. Horneado de luz: «0 de 105 mallas entran» (URGENTE)

- En el proyecto `test` (escena Main) la casa «Rectangle House» está marcada
  Static, pero sus 101 mallas aportan **0 triángulos** al horneado.
- El informe no lo cuenta: solo nombra los 4 «Mando Quest 3» (sin Static).
- Sitio: `CramionCore/src/lighting/ProbeBaker.cpp`, `gatherBakeScene`
  (~líneas 1041-1095: `meshes_baked` solo sube si se añadieron triángulos).
- Hay un workflow que lo estaba diagnosticando con los datos reales
  (`cramion-bake-zero-triangles`): revisar su resultado, compilar y probar.
- Al terminar: hornear la casa por MCP (`http://127.0.0.1:7777/mcp`, script
  de ayuda `mcp.py` en el scratchpad de la sesión) y comprobarlo en el editor.

## 3. Reproductor de vídeo (sin compilar)

- Código escrito y revisado (fase 1: Media Foundation en Windows, Render
  Texture / material / plano de cámara / API, audio Directo o a varios
  AudioSource, eventos, API C++ y Visual Scripting, arrastrar desde el
  navegador, vista previa, empaquetado).
- La compilación se cortó por **falta de RAM** (iba en el paso 4 de 312, sin
  errores). Cerrar programas pesados y compilar con `-j 2`:
  `cmake --build build-release -j 2 -- -k 0`
- Después: arreglar errores de compilación, correr `CramionVideoTests` y las
  pruebas de audio/scripting/cambio de escena de una en una, y regenerar
  `Api.gen.h` con `cramion_sdkgen` para confirmar las entradas escritas a mano.
- Fase 2 (más adelante): Android (MediaCodec), FFmpeg opcional (RTSP/DASH).
- Saldría en la **2.4.11**.

## 4. Cosas que quedaron fuera (para otra versión)

- World Partition fase 1 estilo Unreal: varias rejillas, Data Layers,
  dependencias entre celdas, fuentes con predicción, formato de celda v3
  (plan en `wp_plan.md` del scratchpad).
- Red en coordenadas absolutas a más de 2 km (el cliente manda posiciones
  locales; spawn/despawn se mandan a todos).
- Blueprints fase 2: Construction Script, funciones, macros, Timeline,
  depuración en vivo; renombrar/mover un Blueprint no mueve su `.crgraph`.
- Horneado: lightmaps con UV2 (sombras más nítidas), el sol horneado se suma
  dos veces si el sol se mueve > 2° hasta volver a hornear, los pases
  transparentes/forward siguen sumando el sol horneado.
- VR: el presupuesto adaptativo reconstruye samplers (pequeño tirón) al
  cambiar de escala.

## Recordatorios

- Cerrar `CramionEditor.exe` antes de compilar (si no, falla el enlace).
- Una sola compilación a la vez; pruebas de una en una; sin commits sin pedirlo.

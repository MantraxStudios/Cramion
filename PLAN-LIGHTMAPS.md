# Lightmaps UV2 profesionales en Cramion: especificación de implementación

**Para:** el agente ingeniero. **Estado:** diseño cerrado. En esta fase solo se leyó código: no se compiló ni se editó nada. **Fecha:** 2026-10-14.

**Etiquetas usadas en el documento:**
- **[V]** verificado leyendo el código (archivo:línea).
- **[M]** medido (logs o scripts del scratchpad).
- **[W]** verificado en la fuente de xatlas (master, GitHub).
- **[S]** supuesto o estimación: hay que medirlo.

### Estado de la implementación (2026-10-19, sin compilar ni probar todavía)

- **Fase 0** — 0.1 hecho (copia de `gi_raw_` a `gi_image_`, sin SVGF; al fusionar con F1-6 de `MEJORA.md` quedó una sola rama, solo con `!dynamicProbesActive()`: con sondas dinámicas el SVGF sigue, decisión pendiente en `PENDIENTES.md`) · 0.2 hecho (`TextureStats`, `AssetManager::readMaterialColors`, `resolveBakeMaterials` en el hilo del horneado; además albedo de rebote 1.5) · 0.3 hecho (`BakeLights.h`) · 0.4 hecho · 0.5 hecho · 0.6 hecho (`readSkyIrradianceSh`, `eTransferSrc` en IblProbe) · 0.7 hecho · 0.8 hecho (`probeCoverage` + `volumeAround` de un subconjunto) · 0.9 pendiente (es una prueba en el editor) · MCP `set_lighting_mode` hecho (+ `probe_coverage`, `seconds`) · documentación hecha · pruebas hechas (sin ejecutar).
- **Fase 1** — 1.1 hecho (xatlas `f700c77` en `CramionCore/vendor/xatlas`, CMake y THIRD_PARTY_NOTICES) · 1.2 hecho · 1.3 hecho (`LightmapUnwrap.cpp`) · 1.4 hecho · 1.5 hecho · 1.6 hecho, con un cambio: la UV2 va en el **binding 10** del set 1 (el 9 lo usa ahora el tiling del material, `kMaterialUvBinding`, de otra tarea) · 1.7 hecho (botón «Generar UV de lightmap», vista con el botón LM de la vista Escena) · MCP `generate_lightmap_uvs` hecho (asíncrono: el resultado va en `lighting_state.uv_generation`) · documentación hecha · pruebas hechas (sin ejecutar) · medir White1: pendiente.
- **Fase 2** — 2.1 hecho (`BakeTracer`) · 2.2 hecho (cuatro TU; receptores desde `BakeScene::meshes` de `gatherBakeScene`) · 2.3 hecho · 2.4 hecho (`BakedLightingFile`; registro LMRC de 112 B) · 2.5 hecho, con un cambio: los valores de `decal_info.yzw` van en `GpuWeather::lightmap` (al final del UBO: `decal_info.yzw` ya lo usa el parallax) · 2.6 hecho (la lectura del atlas solo en `skinned.frag`/`surface.frag`, con `CRAMION_LIGHTMAP`) · 2.7 hecho (`RenderSyncLightmaps.cpp`) · 2.8 hecho (flujo, ventana, MCP, lote estático al exportar, player) · 2.9 hecho (`iluminacion-horneada.html`; falta regenerar con `build_manual.py`) · 2.10 hecho (sin ejecutar) · 2.11 pendiente (capturas).
- **Fase 3** — 3.1 parcial (sin la rejilla de luces por alcance) · 3.2 hecho · 3.3 hecho · 3.4 hecho · 3.5 pendiente (las sondas siguen saliendo de `bakeProbes`, ya sin la directa del sol) · 3.6 hecho · UI y MCP hechos · pruebas hechas salvo `testProbesFromLightmaps` · aceptación visual pendiente.
- **Fase 4** — 4.1 hecho (`ShadowCascades::fitStatic` + `testStaticSunFit`) · 4.2 pendiente (no se ha conectado a `updateUniforms`/`recordShadowPass`: otra tarea está cambiando las cascadas) · 4.3 pendiente (medir en móvil) · UI pendiente.
- **Fases 5 y 6** — pendientes.

**Aviso:** `CramionFX/src/vk/VulkanRenderer.cpp` y `VulkanRenderer.h` tienen cambios sin confirmar de otra tarea: presupuesto adaptativo, nubes y luz volumétrica (`git diff --stat`: +205/−66 en el .cpp). En esos dos archivos los números de línea son aproximados (≈): búsquelos por el nombre de la función. El resto de referencias archivo:línea son del árbol actual.

---

## 0. Diagnóstico: por qué el horneado actual no puede parecerse al tiempo real

| Síntoma en las capturas | Causa | Evidencia |
|---|---|---|
| Manchas diagonales borrosas; sin parches de sol nítidos | La luz directa horneada no usa UV2: vive en celdas 3D de 0.5 m. Cada celda tiene un punto representativo y la **suma** de las normales, un solo lóbulo L1, mezcla trilineal de 8 celdas, a media resolución y después pasa por SVGF (temporal + 5 à-trous). | [V] ProbeBaker.h:97-101; ProbeBaker.cpp:576-583, 619-623, 647-669; baked_gi.frag:113-177; VulkanRenderer.cpp `recordSsgiPass` (≈6669-6741) |
| Fachada casi negra | En una pared fina las dos caras caen en la misma celda y la normal sumada se anula. Además se descartan las celdas con ≥40% de rayos contra caras traseras: en la casa quedan válidas 1859 de 3087. | [V] ProbeBaker.cpp:576-583, 621-623, 645; [M] scratchpad/verify-r2-realdata/cells.log:56,61 |
| Imagen gris y turbia | Varias causas a la vez (ver lista debajo de la tabla). | [V] ProbeBaker.cpp:1197-1200, 1365-1368, 1385-1389, 216-223, 381-384; ProbeBaker.h:116-117; skinned.frag:59, 161-162; lighting.frag:1438-1440, 1494-1503, 1582 |
| Sombras "nada que ver" | Con el sol Horneado el render pone su radiancia a 0 (también el especular). Toda la luz del sol sale entonces de la GI horneada. | [V] lighting.frag:1442-1450, 1616-1619; VulkanRenderer.cpp `updateUniforms` (`sky_map_depth.w = bakedSunActive(...)`, ≈4341) |
| Banda oscura en la pared derecha (Mixta) | El rebote solo viene de sondas cada 2 m. Los Light Probe Volume del usuario no cubren la casa: el A no la toca y el B deja fuera los 1.2 m de arriba. Fuera de los volúmenes no hay rebote y se usa el cielo entero. | [M] cells.log:56-58; [V] baked_gi.frag:181-221 |
| Foco horneado distinto del tiempo real | El horneado usa smoothstep para el cono; el render usa cono² lineal. | [V] ProbeBaker.cpp:259-260, 340-341 frente a lighting.frag:1759-1766 |
| Horneados no reproducibles | El generador aleatorio (mt19937) se siembra por hilo o por trozo de trabajo. | [V] ProbeBaker.cpp:615, 871-872; [M] verify-r2-realdata/house.log:77,86 |
| La sombra de la casa en el agua desaparece en Horneada | **El código no lo explica.** El agua usa las cascadas (water.frag:338-348, 515) y `recordShadowPass` no depende del modo de iluminación (VulkanRenderer.cpp ≈5660). Ver la tarea 0.9. | [S] |
| Horneada más lenta que tiempo real (31.8 frente a 23.5 ms) | Tabla hash a media resolución (hasta 48 pasos × 8 celdas), más SVGF, y las cascadas se siguen dibujando. | [M] barra de estado de las capturas; [V] baked_gi.frag:97-111 |

Causas de la imagen gris y turbia:
- Los materiales con textura usan albedo = color base × 0.6.
- El cielo del horneado es un degradado fijo.
- El último rebote suma cielo sin sombra.
- La emisión no lleva el factor ×6 que aplica el render.
- El SSAO y el Fresnel se aplican también al sol horneado.

**Conclusión:** el problema es estructural. Faltan UV2, y el sol se hornea en un sitio sin resolución suficiente. Ningún ajuste lo arregla.

---

## 1. Resumen y decisiones

### 1.1 Qué se construye

Un lightmapper de CPU por texel de UV2, al estilo del Progressive Lightmapper de Unity y de Lightmass. Funciona para los MeshRenderer **Static y rígidos**.

**Horneado:**
- UV2 automáticas con **xatlas**: vendorizado, solo en el editor, cacheadas en `Library/` y guardadas en forma compacta dentro del `.crbake`.
- Por texel:
  - punto, normal y validez con 16 submuestras;
  - **visibilidad del sol** supermuestreada sobre el disco de 0.53° y la huella del texel;
  - luz directa de las luces locales Horneadas;
  - **luz rebotada** multi-rebote por recolección sobre el propio atlas (radiosidad en espacio de lightmap), con el cielo real del render y los emisivos;
  - filtro de ruido à-trous guiado;
  - dilatación de bordes.
- Páginas RGBA16F:
  - `rgb` = irradiancia en el convenio del render (difuso = albedo × rgb);
  - `a` = visibilidad del sol.

**Render:**
- La UV2 llega por un SSBO del modelo y los datos de cada instancia por un SSBO paralelo a los huesos.
- El G-buffer suma la luz horneada en el canal de emisión y marca el píxel.
- `lighting.frag` evalúa el sol **en tiempo real con la misma BRDF** y usa la visibilidad horneada como sombra. "Horneada" se ve igual que el tiempo real (mismas intensidades, mismo especular, normal maps) y no dibuja mapas de sombra por frame.

### 1.2 Decisiones

| Tema | Decisión | Por qué |
|---|---|---|
| Generador de UV2 | **xatlas vendorizado** (MIT; `CramionCore/vendor/xatlas/`, `XA_MULTITHREADED=0`). Solo en el editor: Android no lo compila. Se conservan la tubería de LightmapUv (hash, aplicar, E/S, granularidad por pieza) y se reescribe el formato. | La réplica del generador propio sobre la casa real da 137 211 cartas (52% de un solo triángulo), un 73-97% de relleno en la mayoría de piezas, solapes de hasta 0.5% sin detectar y hasta 3.9× de estiramiento [M] (uv2-analysis/rows_all2.json). Arreglarlo es reescribir xatlas. Además: xatlas conserva el orden de caras de entrada (`indexArray[chart->mapFaceToSourceFace(f)*3+j]`) [W], es determinista (KISSRng con semilla fija) [W] y permite cancelar desde el callback de progreso [W]. |
| Piezas diminutas | Componentes con diagonal < `min_component_texels` (2 texeles; 10 cm a 20 t/m) y caras degeneradas: **sin UV2** (centinela). Las iluminan las sondas y el sol en tiempo real. | Son el 37.8% de los triángulos de la casa pero el 0.26% del área [M]. |
| Densidad | **20 texeles/m** (5 cm por texel). Ajustes predefinidos: Vista previa 8 · Media 20 · Alta 40. Presupuesto total de **8 M texeles**: si se pasa, baja la densidad de toda la escena y lo avisa. | A 20 t/m el borde de una ventana queda en 1-2 texeles. La casa ocupa una página de 2048×1024. |
| Atlas | Páginas RGBA16F; `rgb` = irradiancia y `a` = visibilidad del sol. Un **único `sampler2DArray`** (capa = página). Sin mips en la primera versión (`textureLod(…,0)`). | RGBA16F ya funciona en el modo compatible [V] (GBuffer.h:45-46). Con un solo sampler el vidrio llega justo a 16 en el modo compatible (tabla 1.4). |
| Fichero | `<Escena>.crbake` **v4** con trozos y tabla de contenido al final, comprimido con zstd por trozo. El modo sigue en el byte 8. Contiene sondas, páginas, registros por instancia y disposiciones UV2. Se siguen leyendo v1-v3. Caché de UV en `Library/Cache/LightmapUV/`. | Todo lo generado va junto a la escena o en Library (regla del usuario). El export empaqueta `Assets/` entero [V] (EditorExport.cpp:316-336). |
| Filtro de ruido | à-trous propio en CPU, en el espacio del atlas, guiado por posición, normal e instancia. **Solo la indirecta.** La visibilidad del sol no se filtra (ya va supermuestreada). OIDN queda como opción para la Fase 6. | Sin dependencias pesadas (16 GB, -j 2). |
| Trazador | `BakeTracer` nuevo: BVH binario con SAH por cubos, recorrido del hijo más cercano primero, devuelve las baricéntricas. | El BVH actual divide por la mediana y no devuelve las baricéntricas [V] (ProbeBaker.cpp:46-205). |
| UV2 hacia la GPU | **SSBO por modelo** en el set de material, binding 9: unorm16×2 por vértice, `0xFFFFFFFF` = sin lightmap. Se lee con `gl_VertexIndex` (en mesh shaders, con el índice del vértice). **El vértice de 80 bytes no se toca.** | Todas las llamadas de dibujo usan `vertexOffset` 0 [V] (cull.comp:136-139; VulkanRenderer.cpp `drawCpuActors` `drawIndexed(..., 0, 0)`). `SkinnedVertex` va tal cual al .crdata y a los meshlets [V] (SkinnedModel.cpp:23; meshlet_common.glsl). |
| Datos por instancia | SSBO por frame en vuelo en el **set 0, binding 8**, de 32 B por hueco: `{vec4 escala_desplazamiento; uvec4 (capa, …)}`. Se indexa con el mismo hueco que la matriz de la instancia. | Las push constants ya ocupan 128 B [V] (GpuTypes.h:197). El hueco de la instancia es `bone_offset + huesos` [V] (VulkanRenderer.cpp `updateActors` ≈1733); los shaders lo leen como `gl_InstanceIndex`, `push.bone_offset+1` o `payload.instance` [V] (skinned.vert:58; gbuffer_meshlet.mesh:63). |
| Composición | El G-buffer suma `albedo·(1−F)·(1−metal)·AO·E` en `material.rgb` (emisión). El píxel se marca con el **signo** de `material.a`. La visibilidad del sol va en el nivel de "sombra propia", que pasa de 8 a **32 niveles** con dither. | El G-buffer no tiene canales libres [V] (gbuffer_surface.glsl:368-372). `lighting.frag` no puede tomar más texturas en el modo compatible. Solo `lighting.frag` y `path_trace_common.glsl` leen `material.a` [V] (grep). |
| Sol | Su luz directa **nunca** se hornea como irradiancia (en v4 ni en sondas ni en lightmaps): solo su **visibilidad** y su **rebote**. Se evalúa siempre en tiempo real. | Mismo resultado que el tiempo real; sin sol doble cuando la hora del día lo mueve; el modo del sol se puede cambiar sin volver a hornear. |
| Luces locales Horneadas | Su luz directa difusa con sombra suave va al `rgb` del lightmap y a las sondas. No se dibujan en tiempo real (igual que hoy y que Unity). | El render ya las quita [V] (RenderSync.cpp:3450-3468). |
| Objetos dinámicos | Sondas (indirecta, cielo y luces locales Horneadas) más el sol en tiempo real. Con el sol Horneado, sus sombras salen del mapa estático (Fase 4). | Mejor que en Unity: con sol Horneado, Unity ilumina lo dinámico solo con sondas. |
| Terreno, follaje, vóxeles | Sin lightmap: sondas más sol en tiempo real. El terreno con lightmap llega en la Fase 6. Los `.crbake` v4 no generan el "lightmap de superficie" (celdas). | Las celdas causan las manchas; la v1-v3 se sigue leyendo. |
| Static batching al exportar | Las instancias con lightmap **se quedan fuera** del lote desde la Fase 2. Un lote que respete los lightmaps llega en la Fase 6. | El lote pierde la UV2 y crea UUID nuevos [V] (StaticBatching.cpp:350, 430-448); con él el juego exportado perdería los lightmaps. |

### 1.3 Qué hace cada modo de luz después del cambio

| Modo (Light.bake_mode) | Directa sobre lo Static con lightmap | Sombras sobre lo Static | Directa y sombras sobre lo dinámico | Rebote | Coste en el juego |
|---|---|---|---|---|---|
| **Tiempo real** | Tiempo real | Cascadas o mapas de sombra | Tiempo real con cascadas | No se hornea | El de siempre |
| **Mixta** (por defecto) | Tiempo real (misma BRDF) | Cascadas por frame (estático y dinámico) | Tiempo real con cascadas | Lightmap y sondas | Cascadas por frame |
| **Horneada (sol)** | **Tiempo real con la misma BRDF, especular y normal maps incluidos** | **Visibilidad horneada** (más sombras de contacto en PC) | Sol en tiempo real con el **mapa de sombra estático** dibujado una vez (Fase 4). Lo dinámico no proyecta sombra del sol. | Lightmap y sondas | Sin cascadas por frame (gama baja) |
| **Horneada (puntual o foco)** | Difusa horneada en el lightmap (sin especular, como en Unity) | Horneadas, suaves (radio de la fuente) | Desde las sondas | Lightmap y sondas | Cero |

- Si la hora del día mueve el sol Horneado más de 2° respecto al horneado, se usan las cascadas como en Mixta. No hay doble sol porque la directa del sol no está horneada.
- El modo de iluminación (`LightingMode`) **Tiempo real** ignora todo lo horneado. **Horneada** usa lightmaps y sondas.

### 1.4 Presupuesto de descriptores en el modo compatible (Android, Vulkan 1.0)

Mínimos de Vulkan: 16 samplers y 4 SSBO por etapa [V] (VulkanCompat.h:45-51).

| Layout | Samplers de fragmento hoy | Con lightmaps | SSBO de vértice | SSBO de fragmento |
|---|---|---|---|---|
| Vidrio (frame, material, vidrio) | 6 + 5 + 4 = **15** [V] (SkinnedPass.cpp:98-130, 395-421; SkinnedPass.h:141) | **16** (+ atlas) | huesos + params + UV2 = 3 | params + registros = 2 |
| Geometría (frame, material) | 11 | 12 | 3 | 2 |
| Terreno, agua | 11, 12 [V] según el investigador | 12, 13 | sin cambios | +1 |
| `lighting.frag` | 16 de 16 [V] (LightingPass.cpp:237-243) | **sin cambios (prohibido añadir)** | — | — |

Regla: **una sola** textura nueva en el set de frame (binding 7) y ninguna en el set de material. En la Fase 2 se añade un comprobador: en el modo compatible, al crear cada pipeline layout, contar samplers y SSBO por etapa y avisar con `std::cerr` si se pasa de 16 o de 4.

### 1.5 Unidades: paridad con el tiempo real

**Lo que hace el render:**
- Difuso = albedo × radiancia × NdotL, con π incluido en las intensidades [V] (lighting.frag:332-346).
- Sol: `toLinear(color)·intensidad`; RenderSync ya multiplica el sol físico por el color y la intensidad del Light [V] (RenderSync.cpp:3592-3593).
- Atenuación: `ventana²/(d²+1)` [V] (lighting.frag:295-299; igual en ProbeBaker.cpp:225-229).
- Cono de foco: `x²` lineal [V] (lighting.frag:1759-1766).
- Cielo: armónicos esféricos de irradiancia ya divididos por π [V] (lighting.frag:136, 349-360).

**Lo que guarda el lightmap**, E en las mismas unidades:
- Directa local: `I · atenuación · cono · max(0, n·l) · (1 − fuerza·(1 − vis))`.
- Indirecta: la media de `L_in` sobre rayos distribuidos por coseno.
- Cielo: `L_cielo(d) = max(0, Σ_k c_k / kBanda_k · Y_k(d))`, con `kBanda = {1, 2/3, 2/3, 2/3, 1/4 × 5}` [V] (ProbeBaker.cpp:402) y `c_k` = los 9 coeficientes que usa el render.
- Comprobación: con un cielo constante, `irradianceSh(n) = L` [V] por la cuenta.

**Materiales:**
- Albedo lineal = `base_color × media(toLinear(textura))` [V] (skinned.frag:103-104 y la conversión de lighting.frag).
- Emisión = `emisivo × 6 × media(toLinear(textura emisiva))` [V] (skinned.frag:59, 161-162).
- Albedo de rebote = `albedo·(1−metal) + F0·rugosidad·0.5`, con F0 = mix(0.04, albedo, metal) [S] (convenio del meta pass de Unity).
- El albedo se limita a 0.95.

---

## 2. Fases

### Reglas comunes a todas las fases

- **Compilación:** una a la vez y con `-j 2`; nunca dos builds a la vez.
  - PC: `cmake --build build-release --target CramionEditor -j 2`; después `--target CramionPlayer` y `--target CramionFeatures21Tests`.
  - Android, solo al final de cada fase: `cmake --build build-android --target main -j 2`.
- **Pruebas:** de una en una; nunca varias a la vez.
- **Commits:** ninguno sin preguntar al usuario.
- **Estilo:** comentarios de código en español sin tildes. Textos que ve el usuario (UI, avisos, MCP, documentación) en español con tildes.
- **Modo compatible:** se prueba en PC con `CRAMION_VK_COMPAT=1` [V] (VulkanDevice.cpp:47-52). Los shaders se compilan también con `-DCRAMION_COMPAT=1 --target-env=vulkan1.0` [V] (CramionFX/CMakeLists.txt:357-376); no puede romperse en ninguna fase.
- **Assets del usuario:** nunca se modifican. Lo generado va a `<Escena>.crbake` o a `Library/`. Un `.crscene` solo cambia si el usuario edita un campo nuevo (`lightmap_scale`) y guarda.
- **Aceptación visual:** reutilizar `scratchpad/e2e/views.py` (cámaras exterior, interior1 e interior2 ya definidas) sobre la copia del proyecto en `scratchpad/e2e/test`. Por cada vista se hacen dos capturas con la misma cámara: `set_lighting_mode {"mode":"realtime"}` y después `{"mode":"baked"}`. Se compara leyendo los PNG; donde se indique, también con números. Para comparar con justicia se pasa `exposure` (exposición fija) a `set_lighting_mode`; ver la Fase 0.

---

### Fase 0: arreglos rápidos y base común (sin lightmaps)

**Lo que verá el usuario:** GI horneada más nítida (sin SVGF), con colores correctos y sin el relleno azul-gris, tanto en Mixta como en Horneada. Horneados reproducibles. Un aviso, y un volumen automático, cuando las sondas no cubren lo Static. Un comando MCP para comparar A/B. El formato no cambia.

**0.1 Sin SVGF en modo horneado.** En `VulkanRenderer::recordSsgiPass`, rama `else if (baked)` (≈6669-6682):
- Después de `drawFullscreen(baked_gi_pass_, …, gi_raw_)`:
  1. barrera de `gi_raw_` a `TransferSrcOptimal`;
  2. `cmd.copyImage(gi_raw_ → gi_image_ (General))`;
  3. barrera de `gi_image_` a lectura en el fragment;
  4. `gi_filter_history_valid_ = false`.
- Saltar el temporal, los à-trous y la copia de momentos, pero conservar el cierre (`ssr_history_ready_`, `scene_history_valid_`).
- `gi_raw_` necesita `eTransferSrc` en su creación (≈3553). Los dos usan el mismo formato y tamaño [V] (≈3553-3571).

**0.2 Paridad de materiales.**
- Mover `averageTexture` de `CramionFX/src/asset/ProxyMesh.cpp:31-58` a un archivo nuevo, `CramionFX/include/CramionFX/asset/TextureStats.h` + `src/asset/TextureStats.cpp`:

  ```cpp
  // Color medio lineal de una textura (BC o RGBA8; las perezosas se resuelven fuera).
  // opaque_fraction: parte de los texeles con alfa >= 0.5 (recorte por alfa).
  bool averageTextureLinear(const TextureData& t, core::Vec3& rgb, float* opaque_fraction = nullptr);
  ```

  ProxyMesh pasa a usarla.
- Nueva función estática `AssetManager::readMaterialColors(uuid, file, wanted)`. Lee el `.crdata` con texturas (`crdata::readModel`), resuelve las perezosas (`asset::resolveLazyTexture`) y devuelve, por pieza y por material, `{albedo, emission, opaque_fraction, transparent, metallic, roughness}`. Libera las texturas al terminar.
- `gatherBakeScene` (ProbeBaker.cpp:1176-1458) usa esos valores en lugar de `×0.6` (1197-1200, 1367, 1387) y de `md.emissive` (1388). Los `.crmat` pasan por `assets::toMaterialData` [V] (MaterialAsset.cpp:262-268) y sus texturas por la misma media.
- La lectura se hace en el hilo del horneado: en el hilo principal solo se reúnen las rutas.

**0.3 Cono de foco.** Añadir `inline float spotCone(float cos_angle, float cos_inner, float cos_outer)` que devuelve `x*x` (copia de lighting.frag:1759-1766) en el archivo nuevo `CramionCore/include/CramionCore/lighting/BakeLights.h`. Usarla en ProbeBaker.cpp:259-260 y 340-341.

**0.4 Sombras de las luces locales.** En `Tracer::direct` y `bakedDirect`:
- Respetar `cast_shadows` (si es false, no se lanzan rayos de sombra).
- Aplicar `shadow_strength` (`vis' = 1 − s·(1 − vis)`) (ProbeBaker.cpp:238-263, 316-345).
- Nuevo `BakeScene::sun_shadow_strength`, que se lee de `Light.shadow_strength`.

**0.5 Quitar el cielo sin sombra del último rebote** (ProbeBaker.cpp:381-384).

**0.6 Cielo real.**
- Nuevos campos `BakeScene::sky_sh` (`std::array<core::Vec3,9>`) y `has_sky_sh`.
- Nuevo `VulkanRenderer::readSkyIrradianceSh()`: `waitIdle`, copiar `IblProbe::irradiance_` a un buffer visible y leerlo. El buffer necesita `eTransferSrc` (IblProbe.cpp:122-123).
- `EditorApp::startLightingBake` (EditorLighting.cpp:117) rellena `sky_sh`.
- `Tracer::sky(d)` (ProbeBaker.cpp:216-223) usa la reconstrucción de 1.5 si `has_sky_sh`; si no, el degradado (para las pruebas).

**0.7 Generador aleatorio determinista.** Semilla por sonda (`hash32(job.index)`) y por celda (`hash32(i)`) en lugar de por hilo o por trozo (ProbeBaker.cpp:615, 871-872).

**0.8 Cobertura de sondas.**
- Tras `gatherBakeScene`, si hay volúmenes del usuario: calcular qué parte de los triángulos de mallas Static (por su centroide) queda fuera de todos.
- Si pasa del 5%: añadir `volumeAround(triángulos_no_cubiertos, auto_spacing, max_probes)` (nueva sobrecarga que acepta un subconjunto) y escribir en el informe: «Se añadió un volumen automático de X × Y × Z m: el N % de las mallas Static quedaba fuera de los Light Probe Volume».

**0.9 Diagnóstico de la sombra en el agua [S].**
- En modo Horneada, con `CRAMION_SHADOW_MARKS=1` y el perfilador del MCP, comprobar si se dibujan las cascadas.
- Activar la vista de cascadas (`cascade_debug_`) y repetir la captura del exterior.
- Si se reproduce, anotar la causa. La Fase 4 lo cubre de todos modos.

**MCP.**
- Nueva herramienta **`set_lighting_mode`**, definida en EditorMcp.cpp:455-460 y despachada en EditorMcp21.cpp:58-69. Argumentos:
  - `mode`: `"realtime"` | `"baked"`.
  - `persist` (bool, por defecto false). Si es true, se parchea el modo en el fichero (Fase 2); en la Fase 0 se usa el guardado actual.
  - `exposure` (número opcional). Fija una exposición manual mientras dure la comparación. Implementación: miembro `exposure_override_` del renderizador, aplicado donde Unlit fuerza la exposición (≈4741-4743).
  - Devuelve `lighting_state`.
- `lighting_state` gana `probe_coverage` (0..1) y `seconds`.

**Documentación:** en `iluminacion-horneada.html`, sección «Cómo funciona», explicar que el albedo y la emisión vienen de las texturas y que el cielo es el del render.

**Pruebas** (Features21Tests.cpp; registrar las funciones en `main`, 809-824):
- `testBakeDeterminism`: la misma escena con `threads = 1` y con `threads = 3` da bytes de sondas idénticos.
- `testSpotConeParity`: `spotCone` frente a la fórmula de lighting.frag en 20 ángulos (error < 1e-6).
- `testAverageTexture`: textura RGBA8 de 2×2 con valores conocidos; media lineal correcta y `opaque_fraction` con un texel de alfa 0.
- `testBakeSkySh`: cielo constante (c0 = 3.5449, es decir L = 1) sobre un suelo de albedo 0.5 sin sol. La sonda a 1 m ve hacia abajo una luz de ≈ 0.5 ± 10%.
- Las pruebas existentes (259-781) siguen pasando.

**Aceptación visual:** `views.py` antes y después en Mixta y en Horneada. Se espera:
- sin el velo azul en el interior;
- rebote cálido del suelo;
- menos borrón (sin SVGF);
- los milisegundos de GPU de Horneada bajan ≥ 1 ms [S].

**Hecho cuando:** pasan las pruebas, compilan el editor, el player y Android, y están las capturas.

---

### Fase 1: UV de lightmap (xatlas), caché y carga

**Lo que verá el usuario:** «Generar UV de lightmap» en la ventana de Iluminación y una vista de depuración «UV de lightmap» con las cartas sobre la casa. Las piezas diminutas salen en magenta: sin lightmap.

#### 1.1 Vendorizar xatlas

- Copiar `xatlas.h`, `xatlas.cpp` y `LICENSE` en `CramionCore/vendor/xatlas/`.
- Fijar un commit:
  - el de Godot (`f700c7790aaa030e794b52ba7791a05c085faf0c`, según su thirdparty/README.md [S]);
  - o el master actual, comprobado con la prueba de orden de caras.
- En `CramionCore/CMakeLists.txt`, junto a CramionAssets (164-178):

```cmake
if(ANDROID)
    list(FILTER CRAMION_ASSETS_SOURCES EXCLUDE REGEX "LightmapUnwrap\\.cpp$")
endif()
add_library(CramionAssets STATIC ${CRAMION_ASSETS_SOURCES})
...
if(NOT ANDROID)
    target_sources(CramionAssets PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/vendor/xatlas/xatlas.cpp")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/vendor/xatlas/xatlas.cpp"
        PROPERTIES COMPILE_OPTIONS "-O2;-w" COMPILE_DEFINITIONS "XA_MULTITHREADED=0")
    target_include_directories(CramionAssets SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/vendor/xatlas")
    target_compile_definitions(CramionAssets PUBLIC CRAMION_HAS_LIGHTMAP_UNWRAP=1)
endif()
```

Es el mismo patrón que bc7enc [V] (CramionFX/CMakeLists.txt:200-206). Con `XA_MULTITHREADED=0` [W] el paralelismo lo pone el horneado (una malla por hilo, como mucho 2 a la vez para acotar la RAM).

#### 1.2 `CramionCore/include/CramionCore/asset/LightmapUv.h` (reescritura)

Se elimina el generador antiguo y el registro global (LightmapUv.cpp:35-548, 634-678): es código muerto [V].

```cpp
struct LightmapUvSettings {
    float texels_per_unit = 20.0f;        // texeles por unidad del modelo
    std::uint32_t padding = 2;            // texeles entre cartas (a esa densidad)
    float min_component_texels = 2.0f;    // piezas sueltas mas pequenas: sin UV2 (sondas)
    std::uint32_t max_size = 4096;        // lado maximo de la disposicion
    int chart_iterations = 1;             // xatlas ChartOptions::maxIterations (2 en Alta)
    std::uint64_t hash() const;           // texels_per_unit redondeado a 1/64 + kUnwrapVersion + commit de xatlas
};

// Disposicion COMPACTA: los vertices originales siguen en su sitio; los partidos
// por las costuras de las cartas se anaden al final; solo cambian las esquinas
// que apuntan a un vertice anadido. Mismos triangulos, mismo orden.
struct LightmapUvLayout {
    std::uint64_t source_hash = 0;        // lightmapMeshHash de la pieza tal como sale del .crdata
    std::uint32_t source_vertices = 0, source_indices = 0;
    std::uint64_t settings_hash = 0;
    std::uint64_t layout_hash = 0;        // huella de lo de abajo (la guarda cada registro del .crbake)
    std::uint32_t width = 0, height = 0;  // texeles a texels_per_unit
    float texels_per_unit = 0.0f;
    std::vector<std::uint32_t> duplicates;    // vertice original de cada anadido (indice V + i)
    std::vector<std::uint32_t> patch_corner;  // esquina (posicion en indices) ...
    std::vector<std::uint32_t> patch_vertex;  // ... y su vertice nuevo
    std::vector<std::uint32_t> uv;            // V + D valores unorm16x2 (u en los 16 bits bajos); 0xFFFFFFFF = sin lightmap
    std::uint32_t charts = 0, ignored_triangles = 0;
    float utilization = 0.0f;                 // texeles cubiertos / (width * height)
    float lightmapped_area = 0.0f;            // unidades del modelo al cuadrado
};

struct LightmapUvPartKey { Uuid model; int part = 0; bool operator==(const LightmapUvPartKey&) const = default; };
// Donde esta cada disposicion (un .cruv de Library o un trozo UVLY de un .crbake).
// Se lee perezosamente en los hilos de carga.
struct LightmapUvSet {
    struct Source { std::filesystem::path file; std::uint64_t offset = 0, stored = 0, raw = 0; bool zstd = true; };
    std::unordered_map<LightmapUvPartKey, Source, /*hash*/> parts;
    std::shared_ptr<const LightmapUvLayout> load(const Uuid& model, int part) const;  // nullptr si no hay o esta danada
};

std::uint64_t lightmapMeshHash(const asset::ModelData& model);       // se mantiene (FNV de indices + posiciones)
#if CRAMION_HAS_LIGHTMAP_UNWRAP
bool unwrapLightmapUvs(const asset::ModelData& part, const LightmapUvSettings& s, LightmapUvLayout& out,
                       const std::function<bool(float)>& progress, std::string* error);  // false si se cancela
#endif
bool applyLightmapUvs(asset::ModelData& model, const LightmapUvLayout& layout);  // false sin tocar nada si no cuadra
std::vector<std::uint8_t> encodeLightmapUvLayout(const LightmapUvLayout&);       // binario sin comprimir
bool decodeLightmapUvLayout(const std::uint8_t* data, std::size_t size, LightmapUvLayout& out);
bool saveLightmapUvLayout(const std::filesystem::path&, const LightmapUvLayout&);  // .cruv v2: "CRUV", v2, zstd(encode)
bool loadLightmapUvLayout(const std::filesystem::path&, LightmapUvLayout&);
```

En `Model.h:256-259` añadir `std::uint64_t lightmap_layout_hash = 0;`: solo en memoria, no se guarda nunca.

#### 1.3 Algoritmo de `unwrapLightmapUvs` (nuevo `CramionCore/src/asset/LightmapUnwrap.cpp`)

```text
1. Soldar posiciones (rejilla de 1e-6 x diagonal, como weldPositions) -> union-find de
   triangulos que comparten vertice soldado -> componentes.
2. Por componente: diagonal_bbox * texels_per_unit < min_component_texels -> excluida
   (centinela). Caras con area < 1e-12 * diagonal^2 -> excluidas.
3. Trozos: componentes enteras, ordenadas, en trozos de <= 65536 caras (una componente
   mayor va sola). Por trozo: vertices locales unicos + indices locales.
4. xatlas::Create(); SetProgressCallback(progreso; devolver false = cancelar);
   por trozo AddMesh({vertexPositionData = &vertices[0].position, stride 80,
   vertexNormalData = &vertices[0].normal, stride 80, indexFormat UInt32, ...}).
5. ComputeCharts(atlas, {maxIterations = chart_iterations, fixWinding = true});
   PackCharts(atlas, {padding, bilinear = true, texelsPerUnit, resolution = 0,
   rotateCharts = true, rotateChartsToAxis = true, blockAlign = false, bruteForce = false}).
6. Si atlas->width o height > max_size: Destroy; texels_per_unit *= 0.98 * max_size / max(w, h);
   volver a 4 (como mucho 3 veces) y avisar: "<pieza>: se baja a X texeles/m".
7. Forma compacta (las caras de salida tienen el orden de entrada dentro de cada trozo [W]):
     slot_uv[V] = sin asignar; dup_de[(trozo, v_salida)] = ninguno
     para cada trozo c, cara local f, esquina k:
        esquina = cara_de_la_pieza(c, f) * 3 + k
        vs = mesh[c].indexArray[f*3+k]; xv = mesh[c].vertexArray[vs]
        vp = vertices_locales[c][xv.xref]                  // vertice de la pieza
        uv = xv.atlasIndex < 0 ? CENTINELA : unorm16(xv.uv / (W, H))
        si slot_uv[vp] sin asignar: slot_uv[vp] = uv; dueno[vp] = (c, vs)
        si no, si dueno[vp] != (c, vs): (crear el duplicado si no existe:
              duplicates.push(vp), uv_dup.push(uv)); patch(esquina, V + indice_dup)
     los vertices nunca asignados -> CENTINELA
     uv = slot_uv ++ uv_dup; layout_hash = FNV(duplicates, patches, uv, width, height)
8. utilization = texeles cubiertos / (W*H); lightmapped_area en unidades^2.
```

`applyLightmapUvs`:

```text
comprobar source_vertices, source_indices y lightmapMeshHash == source_hash
vertices.resize(V + D); vertices[V+i] = vertices[duplicates[i]]
indices[patch_corner[j]] = patch_vertex[j]
lightmap_uvs[i] = uv == 0xFFFFFFFF ? (-1,-1) : unpack
lightmap_layout_hash = layout_hash
si hay LODs: generateLods(model)
```

#### 1.4 Carga con UV2 (AssetManager)

- `AssetManager.h`: nuevo `void setLightmapUvs(std::shared_ptr<const lighting::LightmapUvSet>)`. Usar una declaración adelantada del tipo; el puntero compartido se guarda en un miembro.
- `Loader::Job` (AssetManager.cpp:21-26) gana `std::shared_ptr<const lighting::LightmapUvSet> uvs`. `requestModel` (87-109) captura el actual; `loadModel` (259-298) también.
- `readModel` (165-230) gana el parámetro `const lighting::LightmapUvSet* uvs = nullptr`:
  - Justo después de `crdata::readModel` y **antes** de reagrupar (193-203) y de generar los LODs (212): `if (uvs) for (p) if (auto l = uvs->load(uuid, p)) if (!applyLightmapUvs(part, *l)) std::cerr << "[Lightmap] Las UV de lightmap de <pieza> son de otra version de la malla: vuelve a hornear\n";`.
  - Lo mismo en la rama de primitivas (167-169).
  - El hash coincide con `readModelGeometry`, que no reagrupa [V] (232-257).
- `StaticBatching` y las miniaturas llaman a `readModel` sin conjunto de UV, así que no reciben UV2 [V] (StaticBatching.cpp:350).

#### 1.5 Otros cambios en el núcleo

- `RenderSync::buildVariant` (RenderSync.cpp:717-721): copiar `lightmap_uvs` y `lightmap_layout_hash`.
- `MeshLod.cpp:84-95`: si `!model.lightmap_uvs.empty()`, no usar `meshopt_simplifySloppy`. Con `meshopt_simplify` normal, las costuras de las cartas quedan como costuras [V] según el investigador.
- `MeshRenderer` (Components.h:94-107): `float lightmap_scale = 1.0f;`. En su reflect (Components.cpp:33-46): `v.field({"lightmap_scale", "Escala en lightmap", "Resolución de su lightmap respecto a la de la escena. 0 = sin lightmap (lo iluminan las sondas)"}, lightmap_scale, FloatRange{0.0f, 8.0f, 0.05f, "%.2f"});`.

#### 1.6 GPU: plumbing de la UV2

- `GpuTypes.h:120-134`: `static constexpr std::uint32_t kFlagLightmap = 1u << 6;` (los bits 6 y 7 están libres [V]).
- `SkinnedPass.cpp:136-154` (set de material): construir los bindings en un `std::vector`:
  - los `materialBindingCount()` samplers;
  - `{binding 9, eStorageBuffer, 1, eVertex | mesh_stages}`.
  - Lo usan también el layout del vidrio y el de las sombras: el binding tiene que existir siempre.
- `SkinnedPass` crea un buffer ficticio de 4 bytes (`0xFFFFFFFF`), `dummyLightmapUvs()`, para los modelos sin UV2.
- `SkinnedModel::create` (SkinnedModel.cpp:282-314, 398-493):
  - Si `model.lightmap_uvs.size() == model.vertices.size()`: `lightmap_uvs_ = createDeviceLocal(uint32 empaquetados)` y `gpu.shader_flags |= kFlagLightmap` en todos sus materiales.
  - Al pool del material se le añade `{eStorageBuffer, material_count}`; se escribe el binding 9 en cada set de material.
  - Nuevo `bool hasLightmapUvs() const`.
- Shaders (comentarios sin tildes):

```glsl
// skinned.vert / surface.vert (y el mismo bloque en gbuffer_meshlet.mesh con `vertex` y payload.instance)
layout(std430, set = 1, binding = 9) readonly buffer LightmapUvs { uint lightmap_uvs[]; };
layout(location = 7) out vec2 v_lightmap_uv;
layout(location = 8) flat out uint v_lightmap_slot;
const uint kFlagLightmap = 1u << 6;
...
v_lightmap_uv = vec2(-1.0);
v_lightmap_slot = 0xFFFFFFFFu;
if ((push.flags & kFlagLightmap) != 0u) {
    uint packed = lightmap_uvs[gl_VertexIndex];
    if (packed != 0xFFFFFFFFu) {
        v_lightmap_uv = unpackUnorm2x16(packed);
        // Hueco de la instancia: el de su matriz en el buffer de huesos.
        v_lightmap_slot = (push.flags & 1u) != 0u ? uint(gl_InstanceIndex) : push.bone_offset + 1u;
    }
}
```

- `skinned.tesc` y `skinned.tese` (35-51): pasar las localizaciones 7 y 8. La UV se interpola con baricéntricas y el hueco se toma del vértice 0.
- `VulkanRenderer::drawCpuActors` (≈6168): `push.flags = material.shader_flags & (draw.per_submesh ? ~0u : ~GpuSkinnedPush::kFlagLightmap);`.
- **Vista de depuración** `SceneDrawMode::LightmapUv = 6` (VulkanRenderer.h:129-134; menú en EditorSceneView.cpp:175):
  - El G-buffer escribe en la emisión un tablero de 32×32 coloreado con `fract(uv)`, o magenta si `uv.x < 0`.
  - `lighting.frag` muestra la emisión sin luz: modo `shadows.params.z = 4` y exposición 1, como Unlit (≈4538-4545, 4741).
  - Hasta la Fase 2 la vista se activa con `decal_info.w = 3` del UBO del clima (`GpuWeather`, GpuTypes.h:302-319; `decal_info.yzw` libres [V]).

#### 1.7 Editor

- Ventana de Iluminación, nueva sección «Lightmaps (UV2)» con el campo «Texeles por metro» y el botón «Generar UV de lightmap».
- El botón:
  1. reúne los receptores (ver la Fase 2);
  2. calcula `texels_per_unit` por malla = densidad × máx(escala del mundo de sus instancias × `lightmap_scale`);
  3. en un hilo, con progreso por pieza y Cancelar, busca `Library/Cache/LightmapUV/<source_hash>_<settings_hash>.cruv` o desenvuelve la pieza;
  4. construye un `LightmapUvSet` en memoria;
  5. llama a `asset_manager_->setLightmapUvs(set)` y a `sync_->reset(scene_)`, la misma recarga que el reimport [V] (EditorReimport.cpp:75-76).
- En esta fase no se persiste: la Fase 2 lo guarda en el `.crbake`.

**MCP.**
- `generate_lightmap_uvs {texels_per_meter}` devuelve `{parts, cache_hits, charts, utilization, ignored_triangles_percent, max_layout_size, seconds, peak_mb}`.
- `set_lighting_mode` gana `debug_view: "uv2"`.

**Documentación:** sección nueva «UV de lightmap (automáticas)»: qué son, la caché en Library, las piezas diminutas y la Escala en lightmap.

**Pruebas** (Features21Tests.cpp):
- `testLightmapUvUnwrap`. Malla de prueba: caja-habitación de 4×3×4 m (24 vértices), esfera UV de 32×16, 200 cubos de 3 cm y un triángulo degenerado; desenvuelta a 20 t/m con relleno 2. Comprobaciones:
  - (a) Todo triángulo no excluido tiene sus 3 UV en [0,1]², sin centinela.
  - (b) **Sin solapes:** rasterizar con cobertura conservadora a W×H; ningún texel lo cubren dos cartas distintas. Las cartas son componentes conexas en el espacio UV.
  - (c) Separación: tras dilatar 1 texel cada carta, tampoco hay texeles compartidos.
  - (d) Densidad: (área UV en texeles)/(área en el mundo × 20²) dentro de [0.8, 1.25] para cada carta de más de 4 texeles.
  - (e) Cubos y triángulo degenerado → centinela.
  - (f) Ocupación > 0.5.
  - (g) **Orden de caras:** las posiciones de `indices` tras aplicar coinciden, triángulo a triángulo, con las originales.
  - (h) Determinismo: dos desenvueltas dan el mismo `layout_hash`.
  - (i) `apply`: `lightmap_uvs.size() == vertices.size()`.
  - (j) Ida y vuelta del `.cruv`.
- `testLightmapUvApplyRejects`: con el hash cambiado (una posición movida) devuelve false y no toca la malla.
- `testLightmapUvReadModel`:
  1. escribir un `.crdata` temporal con `assets::writeGeneratedModel` [V] (lo usa StaticBatching.cpp:421);
  2. `LightmapUvSet` con su `.cruv`;
  3. `AssetManager::readModel(..., &set)` devuelve la pieza partida con `lightmap_uvs`;
  4. los LODs, si los hay, se generan sobre ella.

**Aceptación.**
- Vista «UV de lightmap» de las tres cámaras: cartas grandes y limpias en paredes y suelos; los muebles curvos sin estirar; lo diminuto en magenta.
- **Medir y apuntar** el tiempo y el pico de RAM de White1 (1.45 M de triángulos) [S]. Objetivo: < 120 s y < 3 GB. Si no se cumple: trozos de 32 768 caras y `blockAlign = true`, y volver a medir.

---

### Fase 2: lightmapper v1. Sombras del sol horneadas, .crbake v4 y render

**Lo que verá el usuario:** con el sol en **Horneada**, sombras **nítidas e iguales a las de tiempo real**: parches de sol por las ventanas y fachada iluminada, con la misma intensidad, el especular y los normal maps. La luz rebotada sigue saliendo de las sondas (la Fase 3 la mejora).

#### 2.1 `BakeTracer` (nuevos `CramionCore/include/CramionCore/lighting/BakeTracer.h` + `src/lighting/BakeTracer.cpp`)

```cpp
struct BakeHit { float t; std::uint32_t triangle; float u, v; bool backface; };
class BakeTracer {
public:
    // Triangulos en el mundo; coverage[material] < 1 = recorte por alfa (pasa con esa probabilidad).
    void build(std::vector<core::Vec3> v0, std::vector<core::Vec3> e1, std::vector<core::Vec3> e2,
               std::vector<std::uint32_t> material, std::vector<float> coverage, BakeProgress* p);
    bool intersect(const core::Vec3& o, const core::Vec3& d, float tmax, BakeHit& hit, std::uint32_t seed) const;
    bool occluded(const core::Vec3& o, const core::Vec3& d, float tmax, std::uint32_t seed) const;  // early-out
};
core::Vec3 offsetRay(const core::Vec3& p, const core::Vec3& n);  // Wachter-Binder (Ray Tracing Gems, cap. 6)
```

- BVH binario con SAH de 16 cubos; hojas de ≤ 4 triángulos; nodos de 32 B; recorrido que visita primero el hijo más cercano (según el signo de la dirección en el eje de corte); pila de 64.
- Möller-Trumbore de doble cara.
- Recorte por alfa: el rayo pasa si `hash(seed, triángulo) > coverage`, sin texturas por ahora.

#### 2.2 Lightmapper (nuevos `lighting/Lightmapper.h` y cuatro TU: `LightmapperScene.cpp`, `LightmapperRaster.cpp`, `LightmapperLight.cpp`, `LightmapperFilter.cpp`)

Se parte en cuatro TU para compilar con -j 2 y RAM acotada. En Android se excluyen con `list(FILTER CRAMION_CORE_SOURCES EXCLUDE REGEX "/src/lighting/Lightmapper")`.

```cpp
struct LightmapReceiver {              // un MeshRenderer Static rigido
    Uuid entity, model; int part = 0;
    core::Mat4 world;                  // entidad x reposo de la pieza (restPosePositions, ProbeBaker.cpp)
    float lightmap_scale = 1.0f;
    std::filesystem::path file; std::string name;
    std::vector<Uuid> material_overrides;
};
struct LightmapBakeSettings {
    bool enabled = true;
    float texels_per_meter = 20.0f;
    std::uint32_t max_page_size = 2048;
    std::uint32_t texel_budget = 8u << 20;
    int sun_samples = 64;              // Vista previa 16, Alta 128
    int indirect_samples = 128;        // Fase 3 (32 / 128 / 512)
    bool denoise = true;               // Fase 3
    float min_component_texels = 2.0f;
    int threads = 0;                   // 0 = nucleos - 2 (minimo 1), prioridad por debajo de la normal
};
struct LightmapBakeInput {
    BakeScene scene;                   // luces, sky_sh, volumenes y triangulos que no llevan lightmap
    std::vector<LightmapReceiver> receivers;
    LightmapBakeSettings lightmaps;
    std::filesystem::path uv_cache;    // <proyecto>/Library/Cache/LightmapUV
    Uuid scene_uuid; std::array<double, 3> world_origin{};
};
struct LightmapBakeStats {
    std::size_t receivers = 0, probe_only = 0, layouts = 0, layout_cache_hits = 0;
    std::size_t pages = 0, texels = 0, valid_texels = 0, invalid_texels = 0, dilated_texels = 0;
    float texels_per_meter = 0.0f, effective_texels_per_meter = 0.0f;
    double rays = 0.0, rays_per_second = 0.0;
    double seconds_uv = 0, seconds_geometry = 0, seconds_bvh = 0, seconds_raster = 0,
           seconds_validity = 0, seconds_sun = 0, seconds_indirect = 0, seconds_denoise = 0,
           seconds_probes = 0, seconds_save = 0;
};
bool bakeLighting(const LightmapBakeInput& in, gfx::BakedLighting& out,
                  std::vector<std::shared_ptr<const LightmapUvLayout>>& layouts,
                  BakeProgress* progress, LightmapBakeStats* stats, std::string* error);
```

**Receptores.** En `startLightingBake`, en el hilo principal: MeshRenderer activo y visible, `staticInHierarchy`, modelo de asset (no una malla de código), pieza rígida (`animations.empty()`, ≤ 1 hueso), sin Rigidbody Dynamic, sin Animator que reproduce, `lightmap_scale > 0` y con algún material opaco. Lo demás Static solo tapa la luz y lo iluminan las sondas. Se cuenta en el informe.

**Etapas del hilo del horneado**, con pesos de progreso y texto en español:

```text
A. UV (10-40%): por (modelo, pieza): readModelGeometry(wanted) -> hash -> .cruv de la cache
   o unwrapLightmapUvs -> applyLightmapUvs sobre la copia cruda (partida). Memoria: posicion,
   normal y UV2 por vertice (32 B); se suelta el vertice de 80 B.
B. Escala por instancia: s_i = sqrt(area_mundo_i / area_modelo) * lightmap_scale_i;
   densidad efectiva = min(texels_per_meter, sqrt(presupuesto * 0.6 / area_total)) (con aviso).
   texels_per_unit(malla) = densidad_efectiva * max_i(s_i).
C. Paquete de paginas (skyline bottom-left, a mano, ~100 lineas):
     rect_i = (ceil(W_m * s_i / s_max), ceil(H_m * s_i / s_max)) + hueco de 2 texeles
     ordenar por alto descendente; meter en la primera pagina de P x P donde quepa (si no, nueva);
     un rect mayor que P se escala a P - 2 (aviso "<objeto> no cabe en una pagina").
     Al final cada pagina se recorta a la potencia de 2 minima (>= 256) en ancho y alto;
     ancho y alto del array = los maximos. scale_offset = (w_i/AW, h_i/AH, x_i/AW, y_i/AH).
D. Geometria del trazador: los triangulos de BakeScene (lo que no lleva lightmap)
   + los de los receptores (de la copia partida, en el mundo). Material: albedo, emision,
   cobertura (readMaterialColors, Fase 0). BakeTracer::build.
E. Raster de texeles (por pagina, en tiles de 32x32 sobre un contador atomico):
     para cada instancia i y triangulo t con UV2 valida (no transparente):
        A,B,C = uv2 * (w_i, h_i) + (x_i, y_i)                    // pixeles de la pagina
        para cada texel de su caja (+1):
           cob = numero de sub-muestras (4x4) dentro de ABC; bary = media de sus baricentricas
           si cob == 0 y ABC toca el cuadrado del texel: cob = 0.5, bary = punto de ABC mas cercano al centro
           si cob > mejor[texel].cob: dueno = (i, t, bary, cob)
     por texel con dueno: posicion, normal suave (interpolada, normalizada), normal geometrica
     (orientada como la suave), J = d(mundo)/d(uv) del triangulo, lado = sqrt(area_mundo / area_uv).
F. Validez (por sub-muestra; 16 por texel):
     p_k = posicion + J * (uv_k - uv_centro) (en el plano del triangulo dueno)
     o = offsetRay(p_k, n_geo); dentro = algun rayo de {+T, -T, +B, -B} (tangentes de J)
         hasta 1.5 * lado choca con una cara TRASERA
     submuestras validas = las que no estan dentro; texel invalido si no queda ninguna.
     punto representativo = la valida mas cercana al centro.
G. Visibilidad del sol (si el sol aporta: Mixta u Horneada, intensidad > 0):
     M = sun_samples; rotacion de Cranley-Patterson = hash(indice del texel)
     para j < M: p = validas[j % n_validas]; (r1, r2) = R2(j) + rotacion
        d = normalize(L + 0.00465 * sqrt(r1) * (cos(2 pi r2) T_L + sin(2 pi r2) B_L))   // kSunSize, lighting.frag:1664
        lit += !occluded(offsetRay(p, n_geo), d, inf, seed)
     a = lit / M   (sin filtro: ya esta supermuestreada)
H. Dilatacion: (relleno + 2) pasadas; cada texel vacio o invalido vecino de uno valido toma la
   media de sus vecinos validos (8-conexo). Despues no queda ningun texel sin dato dentro de los rects.
I. Salida: paginas RGBA16F (rgb = 0 en la Fase 2, a = vis), registros, limites de lo horneado.
J. Sondas: la funcion actual bakeProbes con dos banderas nuevas en BakeSettings:
   exclude_sun_direct = true (bakedDirect sin el sol) y surface_texel = 0 (sin celdas).
```

Generador aleatorio: siempre `hash(texel, muestra, pasada)`, nunca por hilo. Así el resultado es idéntico con 1 o con 14 hilos.

#### 2.3 Contrato con el render (`CramionFX/include/CramionFX/vk/BakedGi.h:73-101`)

```cpp
inline constexpr std::uint32_t kLightmapNone = 0xFFFFFFFFu;
struct LightmapPage { std::uint32_t width = 0, height = 0; std::vector<std::uint16_t> texels; };  // half x4; se vacia al subirla
struct LightmapRecord {
    std::array<std::uint64_t, 2> entity{}, model{};
    std::int32_t part = 0;
    std::uint32_t page = kLightmapNone;
    core::Vec4 scale_offset{1.0f, 1.0f, 0.0f, 0.0f};   // uv_atlas = uv2 * xy + zw
    std::uint64_t layout_hash = 0;
    std::array<float, 12> world{};                   // 3x4 al hornear (relativa a world_origin): aviso si se movio
};
struct LightmapSet {
    std::uint32_t width = 0, height = 0;             // tamano del array
    std::vector<LightmapPage> pages;
    std::vector<LightmapRecord> records;
    float texels_per_meter = 0.0f;
    bool sun_visibility = false;                     // el canal a vale
    bool has_irradiance = false;                     // Fase 3
    core::Vec3 bounds_min{}, bounds_max{};           // de lo horneado (mapa de sombra estatico, Fase 4)
    bool empty() const { return pages.empty() || records.empty(); }
};
// En BakedLighting: std::uint32_t version = 3; bool sun_in_probes = true;  // v1-3: el sol esta en las sondas
//                   LightmapSet lightmaps; std::array<double, 3> world_origin{}; Uuid... (como 2 uint64)
// empty(): ... && lightmaps.empty()
```

#### 2.4 `.crbake` v4 (nuevos `CramionCore/include/CramionCore/asset/BakedLightingFile.h` + `src/asset/BakedLightingFile.cpp`)

Va en CramionAssets porque necesita zstd [V] (CramionCore/CMakeLists.txt:172-177). `saveBakedLighting` y `loadBakedLighting` (ProbeBaker.h:241-246) pasan a definirse ahí con la misma firma, y el lector v1-v3 se mueve tal cual (ProbeBaker.cpp:1648-1733).

```text
Cabecera (32 B): "CRBK", u32 version = 4, u32 modo (offset 8, como v1-3), u32 flags
                 (bit0 sun_visibility, bit1 lightmaps, bit2 has_irradiance), u64 offset_toc, u32 n_toc, u32 0
Trozos (en orden de escritura); TOC al final: {char id[4]; u32 flags (bit0 zstd); u64 offset, guardado, crudo; u64 fnv}
  INFO  version del motor (texto), UUID de la escena, world_origin (3 double), ajustes usados (JSON),
        sol al hornear (direccion, color, intensidad, modo)
  VOLS  u32 n; por volumen 7 f32 + 4 u32 (como v3)
  PRBS  sondas (zstd)
  LGTS  u32 sun_visibility, f32[3] direccion del sol, u32 n, n x 16 B UUID de las locales Horneadas
  LMAP  u32 paginas, ancho, alto, formato (0 = RGBA16F), f32 texels/m, f32[6] limites, u32 has_irradiance
  PAGE  (uno por pagina) u32 indice, ancho, alto + texeles half x4 (zstd)
  LMRC  u32 n; registros fijos de 104 B (entity 16, model 16, part 4, page 4, scale_offset 16, layout_hash 8,
        world 48 -> 112; usar static_assert del tamano real)
  UVLY  (uno por disposicion) UUID del modelo 16, i32 pieza + encodeLightmapUvLayout (zstd)
```

**Escritura:** en el hilo del horneado, a `<Escena>.crbake.tmp` y después `rename` [V] (el patrón ya existe en ProbeBaker.cpp:1600-1645).

**Lectura:** por la TOC. `UVLY` no se descomprime: se registra en un `LightmapUvSet` con `{file, offset, stored, raw}`. Las páginas sí se leen.

**Cambio de modo:** nuevo `bool setBakedLightingMode(const std::filesystem::path&, gfx::LightingMode)` que reescribe el u32 del offset 8; vale para v1-v4. Sustituye a la reescritura completa de EditorLighting.cpp:248-256, que con páginas las perdería.

#### 2.5 Render

- **`LightmapAtlas`** (nuevos `CramionFX/include/CramionFX/vk/LightmapAtlas.h` + `src/vk/LightmapAtlas.cpp`):
  - Imagen 2D array RGBA16F con vista **siempre** `e2DArray`. VulkanImage pone vista 2D si hay 1 capa [V] (VulkanImage.cpp:48): crear la vista aquí o añadir un parámetro `force_array_view`.
  - Subida por staging con `copyBufferToImage` por capa, usando `submitOneTime` [V] (VulkanDevice.h:101).
  - Muestreador lineal con `clamp-to-edge` y `maxLod = 0`.
  - Ficticio de 1×1×1 con `(0,0,0,1)`.
  - Cuenta de VRAM (`GpuMemoryKind::Lightmap`: nueva categoría en GpuMemoryStats.h:17-23).
- **Set de frame** (SkinnedPass.cpp:98-134): array de 9 bindings.
  - `[7]` = combined image sampler, fragmento (atlas).
  - `[8]` = storage buffer, fragmento (registros).
  - Pool (VulkanRenderer.cpp ≈1341): +1 sampler y +1 SSBO por frame.
  - Escrituras en la creación de `skin_sets_` (≈1360-1418) con el ficticio y un buffer de registros mínimo.
- **Registros:**
  - `GpuLightmapRecord {core::Vec4 scale_offset; std::uint32_t layer; std::uint32_t pad[3];}` (32 B, std430).
  - Buffer por frame en vuelo, visible por el host, que crece como `ensureBoneCapacity` (≈1566-1583) y reescribe el binding 8 como `writeBoneDescriptor`.
  - En `updateActors` (≈1585-1950): tras `bone_staging_.push_back(... la instancia)`, si `model.hasLightmapUvs()`: escribir `records[instance] = {actor.lightmap_scale_offset, actor.lightmap_layer >= 0 && lightmapsActive() ? capa : kLightmapNone}`. El tamaño es el número de entradas de la mitad actual de `bone_staging_`.
- **`setBakedLighting`** (VulkanRendererBaked.cpp:161-221): sube las páginas al atlas, **vacía `texels`** en `baked_`, reescribe el binding 7 en todos los `skin_sets_` (ya hace `waitIdle`) y guarda `lightmaps_active_`.
- **Modo del sol:**
  - Nuevo `setSunBakeMode(bool baked)`; RenderSync lo llama junto a `setSunShadowsEnabled` (RenderSync.cpp:3594) con `light.bake_mode == LightBakeMode::Baked`.
  - Nuevo `enum class SunShadowSource { Cascades, LegacyBaked, BakedVisibility }`.
  - `sunShadowSource(dir)`:
    - `LegacyBaked` si `version <= 3 && bakedSunMatches(...)` (comportamiento actual);
    - `BakedVisibility` si `version >= 4 && lightmaps.sun_visibility && sun_bake_mode_ && bakedSunMatches(...)`;
    - si no, `Cascades`.
  - En `updateUniforms` (≈4341): `light_data.sky_map_depth.w = 0, 1 o 2` según la fuente.
- **UBO del clima** (GpuWeather; comentario en GpuTypes.h:308):
  - `decal_info.y` = 0 sin lightmaps, 1 solo visibilidad, 2 con irradiancia (Fase 3);
  - `decal_info.z` = `sunShadows() ? sun_shadow_strength_ : 0`;
  - `decal_info.w` = vista de depuración (1 irradiancia, 2 visibilidad del sol, 3 UV2/densidad).
  - Con Unlit, Wireframe o path tracing (`pathTraceReplacesLighting()`): `decal_info.y = 0`.

#### 2.6 Shaders

`gbuffer_surface.glsl`: añadir tras las declaraciones (43-61) y al final de `writeSurface`, después de la nieve y antes del antialiasing especular (360).

```glsl
// Atlas de lightmaps (BakedGi.h, LightmapSet): capa = pagina; rgb = irradiancia
// (difuso = albedo * rgb), a = visibilidad del sol. Una sola textura: en el modo
// compatible el vidrio llega justo a 16 (SkinnedPass).
layout(set = 0, binding = 7) uniform sampler2DArray lightmap_atlas;
// Los rellena skinned.frag / surface.frag (el terreno, el follaje y los voxeles no: z = -1).
vec3 surface_lightmap = vec3(0.0, 0.0, -1.0);   // xy = UV del atlas, z = capa
vec3 surface_view = vec3(0.0);                  // hacia la camara (0 = sin Fresnel)
float lightmapDither(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }
...
    // --- Lightmap ---
    bool lightmapped = surface_lightmap.z >= 0.0 && weather.decal_info.y > 0.5;
    float sun_visible = clamp(surface_sun_shadow, 0.0, 1.0);
    if (lightmapped) {
        vec4 lm = textureLod(lightmap_atlas, surface_lightmap, 0.0);
        sun_visible *= mix(1.0, clamp(lm.a, 0.0, 1.0), weather.decal_info.z);
        // (Fase 3: irradiancia en la emision; vistas de depuracion segun decal_info.w)
    }
    ...
    // Metalicidad + 2 x nivel de sombra del sol (32 niveles con dither); negativo = con lightmap.
    float level = clamp(floor((1.0 - sun_visible) * 31.0 + lightmapDither(gl_FragCoord.xy)), 0.0, 31.0);
    float packed_a = clamp(metallic, 0.0, 1.0) + 2.0 * level;
    out_material = vec4(emissive, lightmapped ? -(packed_a + 0.5) : packed_a);
```

Nuevo `lightmap_common.glsl`, incluido por `skinned.frag` y `surface.frag` antes de `writeSurface` (skinned.frag:188; surface.frag:128):

```glsl
struct LightmapRecord { vec4 scale_offset; uvec4 info; };
layout(std430, set = 0, binding = 8) readonly buffer LightmapRecords { LightmapRecord lightmap_records[]; };
layout(location = 7) in vec2 v_lightmap_uv;
layout(location = 8) flat in uint v_lightmap_slot;
void setupLightmap(vec3 world_position, vec3 camera_position) {
    if (v_lightmap_slot != 0xFFFFFFFFu && v_lightmap_uv.x >= 0.0) {
        LightmapRecord r = lightmap_records[v_lightmap_slot];
        if (r.info.x != 0xFFFFFFFFu)
            surface_lightmap = vec3(v_lightmap_uv * r.scale_offset.xy + r.scale_offset.zw, float(r.info.x));
    }
    surface_view = normalize(camera_position - world_position);
}
```

`lighting.frag`:

```glsl
// 1418-1423: decodificacion
vec4 material_sample = texelFetch(g_material, frag_pixel, 0);
vec3 emission = material_sample.rgb;
bool lightmapped = material_sample.a < 0.0;                 // gbuffer_surface.glsl
float packed_a = lightmapped ? -material_sample.a - 0.5 : material_sample.a;
float self_shadow_level = floor(packed_a * 0.5);            // 0..31
float metallic = packed_a - 2.0 * self_shadow_level;
float relief_shadow = 1.0 - self_shadow_level / 31.0;       // sombra propia del relieve x visibilidad horneada
// 1447: modos del sol
int sun_mode = int(lights.sky_map_depth.w + 0.5);           // 0 cascadas, 1 legado (sol en sondas), 2 visibilidad
bool sun_baked = sun_mode == 1;
bool sun_from_lightmap = sun_mode == 2 && lightmapped;
// 1620-1656: dentro de if (sun_lit)
if (sun_from_lightmap) {
    shadow = 1.0;   // la sombra horneada ya va en relief_shadow
    if (!kLite) shadow = mix(1.0, contactShadow(viewFromDepth(v_uv, depth), mat3(camera.view) * geometric_normal,
                                    mat3(camera.view) * sun_direction, geometric_n_dot_l, 0.02), shadows.params.y);
} else { /* lo de siempre: rayos, cascadas, contacto y mix con la fuerza */ }
// 1675: traslucidez solo con !sun_baked && !sun_from_lightmap
```

`path_trace_common.glsl:305`: `float a = material_sample.a < 0.0 ? -material_sample.a - 0.5 : material_sample.a; s.metallic = a - 2.0 * floor(a * 0.5);`.

**Vistas de depuración** (`SceneDrawMode::LightmapIrradiance = 4`, `SunVisibility = 5`, `LightmapUv = 6`):
- El G-buffer escribe en la emisión lo que toque según `decal_info.w`:
  - 1: `lm.rgb`;
  - 2: `vec3(lm.a)`;
  - 3: tablero cada texel del atlas (`textureSize(lightmap_atlas,0)`), con color por instancia.
- `lighting.frag`, con `shadows.params.z == 4`: `color = emission`; magenta oscuro si `!lightmapped`.

#### 2.7 RenderSync

- Mapa `lightmap_index_` (UUID de la entidad → índice del registro). Se rehace cuando cambia la generación del horneado (contador del renderizador que sube en `setBakedLighting`).
- En el bucle de actores (RenderSync.cpp:3327-3361), poner primero `actor.lightmap_layer = -1`. Después, si el renderizador tiene lightmaps activos y existe un registro cuyos `model` y `part` coinciden con los del MeshRenderer, y además `lightmap_scale > 0`, el `ModelData` del actor tiene UV2 y `lightmap_layout_hash` del `ModelData` == `record.layout_hash`:
  - `actor.lightmap_layer = page`;
  - `actor.lightmap_scale_offset = scale_offset`.
- Con materiales sustituidos, el actor apunta a la variante, que ya lleva la UV2 (1.5).
- Contar los registros «desactualizados» (matriz que difiere en más de 1 mm o de 0.1° de la del horneado, corregida por el origen) y exponerlos en `staleLightmaps()`. El lightmap **se sigue usando**, como en Unity, con un aviso.

#### 2.8 Editor

**Flujo del horneado** (EditorLighting.cpp:82-238):
- En el hilo principal: `gatherBakeScene` (como hoy), los receptores (con ruta del `.crdata` desde `assets.database().find`), `readSkyIrradianceSh`, `uv_cache = <proyecto>/Library/Cache/LightmapUV` y el UUID de la escena.
- En el hilo: `bakeLighting(...)` y `saveBakedLighting` v4.
- Al terminar (hilo principal):
  1. `loadSceneLighting()` relee del disco; ya no se copia `st.result`.
  2. Si alguna disposición es nueva o cambió respecto a la que llevan los modelos cargados, `sync_->reset(scene_)` y toast «Aplicando lightmaps: recargando N modelos». Si no cambió ninguna, se aplica al instante.
  3. `refreshDatabase()`.
- `loadSceneLighting` (68-80): carga el v4, construye el `LightmapUvSet` del fichero y llama a `asset_manager_->setLightmapUvs`. Sin `.crbake` v4, el conjunto queda vacío.
- Combo «Modo» (245-257): usa `setBakedLightingMode`.

**Ventana de Iluminación**, nueva sección «Lightmaps» (todo `BeginDisabled(st.running)`):
- `[x] Lightmaps (objetos Static)`.
- Calidad: «Vista previa / Media / Alta / Personalizada».
- «Texeles por metro»: DragFloat 1-100.
- «Tamaño máximo de página»: combo 1024/2048/4096.
- «Presupuesto (millones de texeles)».
- «Muestras de sombra del sol».
- «Tamaño mínimo de pieza (texeles)».
- Estimación antes de hornear, con las áreas de los receptores sacadas de las copias de la escena: «≈ X texeles · Y páginas de A×B · Z MB · densidad efectiva D t/m».
- Tras hornear: «N objetos con lightmap, M con sondas (pequeños, escala 0 o que se mueven) · P páginas · V % de texeles no válidos · tiempos por etapa».
- Avisos: «N objetos se movieron desde el horneado», «El sol se ha movido: usa las cascadas hasta que vuelvas a hornear», «Iluminación de una versión anterior: genera de nuevo para tener lightmaps».
- Se quita de la UI el checkbox «Lightmap de superficie»: v4 no lo genera.
- Etiquetas del sol (335-336): «Mixto (luz y sombras en tiempo real)», «Horneado (sombras horneadas, sin coste de sombras)», «Solo tiempo real (no se hornea)». Actualizar también la ayuda de `bake_mode` (Components.cpp:96-100).

**MCP:**
- `bake_lighting` gana `lightmaps` (bool), `texels_per_meter`, `max_page_size`, `texel_budget_millions`, `sun_samples`, `min_piece_texels` y `preset` (`"preview"|"medium"|"high"`).
- `lighting_state` gana:
  - `version`;
  - `lightmaps: {enabled, pages, atlas:[w,h], format:"RGBA16F", megabytes, records, lightmapped_objects, probe_only_objects, texels_per_meter, effective_texels_per_meter, invalid_texels_percent, layouts, layout_cache_hits, stale_records, has_irradiance}`;
  - `sun: {bake_mode, shadow_source:"cascades"|"lightmap"|"legacy_probes", direction_matches}`;
  - `timings: {uv2, geometry, bvh, raster, validity, sun, indirect, denoise, probes, save}`;
  - `debug_view`.
- `set_lighting_mode` gana `debug_view: "lightmap"|"sun_visibility"|"uv2"|"none"`.

**Static batching:**
- `StaticBatchOptions` (StaticBatching.h:64-71) gana `std::function<bool(const Uuid& entity)> keep_out`.
- El filtro de candidatos (StaticBatching.cpp:335-372) cuenta `report.kept_lightmapped`.
- `EditorExport.cpp:418-462` carga el `.crbake` de cada escena (solo `LMRC`) y pasa los UUID con registro. Mensaje: «N con lightmap se quedan fuera del lote».

**Player:** en `PlayerMain.cpp:1080-1090`, tras `loadBakedLighting`, `asset_manager.setLightmapUvs(set)`. Ocurre antes del hilo de modelos (1113) [V]. Android usa este mismo archivo [V].

#### 2.9 Documentación

Reescribir `docs-src/pages/iluminacion-horneada.html` y regenerar con `python docs-src/build_manual.py`:
- «Lightmaps»: qué son, Static rígido, escala, piezas pequeñas por sondas, UV automáticas.
- Tabla de modos (1.3).
- «Ajustes» con los nuevos y los ajustes predefinidos.
- «Problemas típicos»: líneas en costuras, sangrado, texeles no válidos (vista de visibilidad), objetos movidos, sol movido.
- «Qué va con el juego»: el `.crbake`, y que lo que lleva lightmap no entra en el lote estático.
- Nota de Android.

#### 2.10 Pruebas (Features21Tests.cpp)

**`testLightmapAtlasPacking`:** 300 rects aleatorios. Sin solapes, huecos ≥ 2, todo dentro de las páginas, y el `scale_offset` lleva (0,0) y (1,1) a las esquinas del rect.

**`testLightmapWindowPatch`** (aceptación principal):
- Escena:
  - habitación cerrada de 4×3×4 m: suelo y = 0, x y z de 0 a 4;
  - pared x = 4 con una ventana de 1×1 m (z de 1.5 a 2.5, y de 1 a 2);
  - receptor: el suelo, a 20 t/m;
  - sol con rayos `(-1,-1,0)/√2`, sin cielo;
  - el parche analítico en el suelo es x ∈ [2,3], z ∈ [1.5,2.5].
- Muestreando la página con bilineal (como la GPU) a lo largo de z = 2:
  - (a) vis(2.5, 2.0) > 0.98;
  - (b) vis(1.7) < 0.02 y vis(3.3) < 0.02;
  - (c) la transición del 10% al 90% en los bordes x = 2 y x = 3 mide **< 2 texeles (10 cm)** y está centrada a ±1 texel del borde analítico. La penumbra física es de 0.4-0.7 texeles.

**`testLightmapThinWall`:**
- Pared de 2 cm (dos caras) en x = 0, con el suelo pasando por debajo y sol rasante (30°) desde −x.
- Texel a x = −0.1: vis > 0.95. A x = +0.1: vis < 0.05. Es la regresión del fallo de las celdas.

**`testLightmapValidity`:**
- Caja cerrada de 0.6 m apoyada en el suelo, sol cenital.
- Los texeles del suelo bajo el centro de la caja quedan invalidados o a vis 0.
- El texel a 1.5 texeles del borde exterior tiene vis > 0.9: sin halo oscuro más allá de 1 texel.

**`testLightmapDeterminism`:** el mismo horneado con 1 y con 4 hilos da páginas idénticas byte a byte.

**`testCrbakeV4`:**
- Ida y vuelta (sondas, páginas, registros, disposiciones; `LightmapUvSet::load` las devuelve iguales).
- Un fichero v3 construido byte a byte se sigue leyendo (con sol en sondas).
- `setBakedLightingMode` cambia el modo y deja las páginas intactas.
- Una versión 5 se rechaza.

**`testStaticBatchKeepOut`:** dos cubos Static, uno con `keep_out`. Sale un lote con uno y el otro se queda.

#### 2.11 Aceptación visual

`views.py` con el sol en Horneada, la escena horneada a 20 t/m y `set_lighting_mode realtime` frente a `baked`:
- interior1: parches de sol en el suelo con la misma forma y posición (comparación visual; si hay Pillow, IoU ≥ 0.8 de la máscara de luminancia > p95 del suelo).
- exterior: fachada con brillo similar (proporción baked/realtime de la luminancia media de la fachada entre 0.85 y 1.2) y sin negro.
- Capturas de «Visibilidad del sol» y «Densidad UV2» adjuntas.
- `lighting_state.lightmaps.invalid_texels_percent` < 5% [S].
- Con `CRAMION_VK_COMPAT=1`: el editor arranca y se ve igual, salvo la PCSS.

---

### Fase 3: luz rebotada en lightmaps (indirecta, cielo, luces Horneadas) y sondas desde lightmaps

**Lo que verá el usuario:** interiores con rebote cálido y nítido, sin manchas de 2 m ni velo gris, con la misma exposición que el tiempo real o mejor GI. Desaparece la banda de la pared derecha también en Mixta.

#### 3.1 Datos por texel en `LightmapperLight.cpp`

```text
albedo_t   = albedo de rebote del material del triangulo dueno (tabla de la Fase 0)
emision_t  = emision del material
E_sol(t)   = toLinear(color_sol) * intensidad * max(0, n_s . L) * vis(t)        // Mixta y Horneada: rebotan
E_loc_b(t) = sum luces locales Horneadas: I * atenuacion * spotCone^? * max(0, n_s . l)
             * (1 - fuerza * (1 - vis_l)), vis_l con M = direct_samples (16..32), cono solido
E_loc_m(t) = igual para las Mixtas pero con 4 muestras (solo para el rebote)
```

Para muchas luces, una rejilla uniforme de luces por alcance: cada texel solo mira las que lo tocan.

#### 3.2 Recolección (gather) iterada en el atlas

```text
rad[t] = albedo_t * (E_sol + E_loc_b + E_loc_m)(t) + emision_t ; dilatar rad
para k en 1..rebotes:
   para cada texel valido t (tiles, contador atomico):
      suma = 0; suma2 = 0; dir = 0
      para j < N (= indirect_samples):
         p = validas[j % n]; (r1, r2) = R2(j) + hash(t, k)
         d = coseno alrededor de n_s; si d . n_geo <= 0: d = reflejar en el plano geometrico
         si intersect(offsetRay(p, n_geo), d, inf, h):
             si h.triangulo es de un receptor con UV2 valida: L = bilineal(rad, uv2_atlas(h))
             si no: L = albedo_mat * E_directa_NEE(h) + emision_mat     // 1 rayo de sombra por luz
         si no: L = L_cielo(d) * intensidad_del_cielo                    // 1.5, sin el disco del sol
         suma += L; suma2 += lum(L)^2
      E_ind[t] = suma / N; var[t] = (suma2 / N - lum(E_ind)^2) / N
   (entre rebotes: 2 iteraciones del filtro 3.3 sobre E_ind)
   rad[t] = albedo_t * (E_sol + E_loc_b + E_loc_m + E_ind)(t) + emision_t ; dilatar rad
```

`rad` ocupa 8 B por texel (RGBA16F) para todas las páginas. Los datos por texel se procesan página a página.

#### 3.3 Filtro (`LightmapperFilter.cpp`)

à-trous con 4 iteraciones (pasos 1, 2, 4, 8) y núcleo B3 de 5×5.

```text
w = h[dx] * h[dy]
    * exp(-|p_x - p_y|^2 / (2 (1.0 * lado_x * paso)^2))     // misma superficie
    * max(0, n_x . n_y)^32
    * exp(-|lum_x - lum_y| / (4 * sqrt(var_x) + 1e-4))
    * (instancia_x == instancia_y)
```

Solo a `E_ind`. Nunca a `vis` ni a `E_loc_b`.

#### 3.4 Composición de la página

- `rgb = E_ind_filtrada + E_loc_b`; `a = vis`.
- Después: dilatación y `has_irradiance = true`.

#### 3.5 Sondas desde los lightmaps

Reimplementar la etapa de sondas con `BakeTracer` y deja de llamarse a `bakeProbes`:
- Rayos de Fibonacci con giro `hash(índice)`.
- Los choques con receptores leen `rad`; el resto, NEE.
- Proyección L2 de lo que choca y visibilidad L1 de lo que escapa, con el mismo convenio que hoy [V] (ProbeBaker.cpp:885-917).
- `bakedDirect` solo con las locales Horneadas.
- Validez con 25% de caras traseras.
- El render de las sondas no cambia [V] (baked_gi.frag).

#### 3.6 Render

`gbuffer_surface.glsl`, dentro de `if (lightmapped)`:

```glsl
if (weather.decal_info.y > 1.5) {
    vec3 lin = toLinear(albedo.rgb);
    vec3 f0 = mix(vec3(reflectance), lin, metallic);
    float nv = dot(surface_view, surface_view) > 0.0 ? clamp(dot(normal, surface_view), 1e-4, 1.0) : 1.0;
    vec3 fresnel = f0 + (max(vec3(1.0 - roughness), f0) - f0) * pow(1.0 - nv, 5.0);
    emissive += max(lm.rgb, vec3(0.0)) * lin * (1.0 - fresnel) * (1.0 - metallic) * multiBounceAoSurface(occlusion, lin);
}
```

`multiBounceAoSurface` es una copia de lighting.frag:1003-1008.

`lighting.frag`:
- `bool lm_irradiance = lightmapped && lights.probe_trust.z > 0.5;`. `probe_trust.zw` están libres [V] (GpuTypes.h:102; solo se leen `.xy`).
- Si `lm_irradiance`:
  - `diffuse_light` = solo el relleno nocturno y el destello del rayo, sin cielo SH ni `gi.rgb` (1494-1502);
  - sin traslucidez del cielo (1506-1509);
  - la emisión se suma como `emission * mix(vec3(1.0), multiBounceAo(ssao, albedo), lights.probe_trust.w)` (1800);
  - `probe_trust.w` = «SSAO sobre lo horneado» (0..1, por defecto 1, como el *AO Static Fraction* de Unreal [S]).
- El especular, la oclusión especular (`gi.a` de las sondas), el SSR, las sondas de reflexión y las luces Mixtas o de Tiempo real no cambian.

**UI.** «Muestras de luz rebotada», «Rebotes» (compartido), `[x] Quitar ruido` y «SSAO sobre lo horneado» (ajuste del renderizador que se guarda con el horneado). Vista de depuración «Lightmap (irradiancia)».

**MCP.** `bake_lighting` gana `indirect_samples`, `denoise` y `lightmap_ao`. `lighting_state.lightmaps.has_irradiance` = true y `timings.indirect` / `denoise` con valor.

**Documentación.** Sección «Luz rebotada», ajustes predefinidos y tiempos orientativos (sección 3 de este documento).

**Pruebas:**
- `testLightmapSkyEnergy`: plano de 10×10 m a la intemperie con cielo constante (L = 1). La irradiancia del texel central = `irradianceSh(arriba)` = 1.0 ± 2%. **La energía coincide con el tiempo real.**
- `testLightmapPointLightParity`: puntual Horneada a 2 m (I = 10, alcance 10) sobre un suelo. `E(x)` en x = 0, 1 y 2 m frente a `I·atenuacion·cosθ` de lighting.frag ± 3%.
- `testLightmapBounce`: suelo blanco (albedo 0.8) al sol y pared vecina en sombra. Su `E_ind` es > 0 y < `albedo × E_sol × 1` (cota física). Con un rebote más, aumenta.
- `testLightmapDenoise`: en una zona plana, la desviación tras el filtro es < 30% de la de antes; en una arista (pared-suelo) la diferencia media entre lados se conserva ≥ 80%.
- `testProbesFromLightmaps`: la sonda a 0.5 m de un suelo al sol, mirando hacia abajo, da `≈ rad` del suelo ± 10%.
- `testLightmapDeterminism`, repetida con la indirecta.

**Aceptación visual:**
- `views.py` en Horneada y en Mixta frente a tiempo real: sin manchas diagonales ni banda de 2 m; rebote coherente; interior no más oscuro que el tiempo real con el mismo `exposure`.
- Capturas de «Lightmap (irradiancia)».
- El número de las sondas, más de las 1210 actuales, se vigila en `lighting_state.probes`.

---

### Fase 4: camino de gama baja. Sin cascadas por frame con el sol Horneado

**Lo que verá el usuario:** en Horneada la GPU no dibuja sombras del sol cada frame. La casa vuelve a dar sombra en el agua; si el fallo de 0.9 existía, queda resuelto por esta vía. Horneada tiene que costar **igual o menos** que el tiempo real.

**4.1** `ShadowCascades` (CramionFX/include/CramionFX/scene/ShadowCascades.h:53+):

```cpp
// Las cuatro cascadas con el mismo encuadre fijo: la caja `world_box` vista desde el
// sol (ortografica, ajustada a texeles). split_distance enorme: siempre la 0.
void fitStatic(const core::Aabb& world_box, const DirectionalLight& sun, std::uint32_t shadow_map_size);
```

**4.2** `VulkanRenderer::updateUniforms`, en la sección de cascadas (≈4385-4525):
- Con `sunShadowSource() == BakedVisibility` y el ajuste «Sombras del sol horneadas sin cascadas» activado (por defecto sí):
  - `cascades_.fitStatic(caja, sol, res)`. La caja son los límites del `.crbake`, ampliados un 10%. Si pasa de 512 m de lado: 512 m centrados en la cámara redondeada a 128 m.
  - `due[0]` solo si cambia el encuadre, `actor_set_changed_`, se mueve algo estático (las esferas de `moved_spheres_` no animadas) o cambia el detalle. `due[1..3] = false`; `cascade_animated_[*] = false`.
- `recordShadowPass` (≈5603-5700): dibujar solo la capa 0 con `ShadowActors::Static`, más terreno, vóxeles y follaje, por la rama sin caché.
- `lighting.frag`, el agua, el vidrio y la volumétrica usan el mismo UBO, así que sin tocarlos leen el mapa estático.
- Los objetos dinámicos no proyectan sombra del sol (está documentado); en PC quedan las sombras de contacto.

**4.3** En el modo compatible, `baked_gi` sigue igual. Medir; si el pase pesa más de 1 ms en un móvil, bajarlo a 1/4 de resolución [S].

**UI.** En la sección «Lightmaps»: `[x] Sin cascadas con el sol Horneado (gama baja)`. Al lado: «Lo que se mueve no hará sombra del sol».

**MCP.** `lighting_state.sun.shadow_source = "static_map"`; `performance_stats` ya da los milisegundos de GPU.

**Pruebas:** `testStaticSunFit`. El encuadre contiene los 8 vértices de la caja, el tamaño de texel = extensión/resolución, y con el sol cenital el lado coincide con la caja ± 1 texel. Es CPU puro y enlaza con Cramion::Core → FX.

**Aceptación:**
- exterior en Horneada: sombra de la casa en el agua;
- en el perfilador, el pase de cascadas ≈ 0 ms con la cámara moviéndose;
- GPU en Horneada ≤ GPU en tiempo real en las tres vistas: hoy 31.8 frente a 23.5 ms en el exterior [M].

---

### Fase 5: exportación, Android, DataPacks y escenas movidas

- **Juego exportado (PC):**
  - exportar la copia del proyecto con static batching, como viene por defecto [V] (BuildConfig.h:84);
  - comprobar que el `.crbake` va en el paquete y que lo que lleva lightmap se queda fuera del lote;
  - capturas en el player iguales a las del editor.
- **Android:**
  - compilar `main` (build-android);
  - con `CRAMION_VK_COMPAT=1` en PC, revisar el comprobador de descriptores (1.4): sin avisos;
  - en un dispositivo, si hay uno, que cargue y se vea [S].
  - Memoria: la casa en Media ocupa unos 17 MB de atlas. Opción de exportación «Lightmaps a media resolución en móvil»: reduce las páginas a la mitad con un filtro que respete las cartas y vuelve a dilatar [S]. Es opcional y está detrás de un ajuste.
- **DataPacks:** añadir `bakedLightingFile(escena)` a las dependencias de cada `.crscene` (DataPack.cpp:112-188). Es un error que ya existe [V].
- **Mover, renombrar, borrar o duplicar una escena:** llevar consigo `<escena>.crbake` y `<escena>.cells/` (AssetDatabase.cpp:215-236; EditorProject.cpp:697-710, 1227-1230). Es un error que ya existe [V].
- **Origen flotante:** `shiftOrigin` (VulkanRenderer.cpp ≈7373) debe restar el desplazamiento a los mínimos de los volúmenes de sondas y reescribir su UBO. Los lightmaps no lo necesitan, porque viven en el espacio del objeto.
- **Pruebas:** `testDataPackIncludesCrbake` y `testSceneMoveCarriesCrbake`, esta última con `AssetDatabase::move` sobre una carpeta temporal.

---

### Fase 6: calidad y velocidad (opcional; por orden de valor)

1. **Shadowmask del sol en Mixta.** La visibilidad ya está en el canal `a`: no hace falta volver a hornear. Lo estático usa la visibilidad; las cascadas solo dibujan lo dinámico hasta una distancia.
2. **Lightmaps direccionales:** capa extra con la dirección dominante (`xyz·0.5+0.5`), para que se vean los normal maps bajo la luz rebotada y las luces locales.
3. **Albedo por texel** muestreado en UV0 y **test alfa real** al chocar (hojas).
4. **Costuras y mips:** cosido por mínimos cuadrados y mips que respeten las cartas.
5. **Lightmap de terreno** con las UV del mapa de alturas.
6. **Horneado incremental:** caché del G-buffer de texeles por instancia, «Seguir horneando» y vista previa durante el horneado.
7. **Lote estático compatible con lightmaps:** `uv2·escala+desplazamiento` horneado en el lote, lotes partidos por página, trozo `LMUV` opcional en CrData y `.crbake` parcheado al exportar.
8. **Compresión de páginas:** BC6H en PC y RGB9E5 en móvil.
9. **OIDN** como complemento opcional, solo en el editor.
10. **Limpieza:** quitar `bakeProbes` (legado) y el escritor de celdas, y migrar sus pruebas (Features21Tests.cpp:259-781).
11. **Reflejos por rayos** que lean los lightmaps.

---

## 3. Tiempos y memoria

Supuestos de velocidad del trazador [S]:
- Hoy: unos 0.25 Mrayos/s por hilo (calculado por el investigador a partir de los logs).
- `BakeTracer`: 1.0-1.5 Mrayos/s por hilo.
- 14 hilos en el Ryzen 7 5700X [M].

### 3.1 La casa: 2.88 M triángulos opacos, 1796 m² opacos

Datos [M] (verify-realdata/house.log:63,72,93): 2.88 M triángulos opacos, 1796 m² de superficie opaca; las piezas diminutas son el 0.26% del área.

| | Vista previa (8 t/m) | **Media (20 t/m)** | Alta (40 t/m) |
|---|---|---|---|
| Texeles ideales | 115 k | 716 k | 2.87 M |
| Con relleno (ocupación 0.55-0.65 [S]) | ≈ 190 k (una página de 512²) | ≈ 1.2 M (una página de 2048×1024) | ≈ 4.4 M (4096×2048 o dos de 2048²) |
| VRAM del atlas (RGBA16F) | 2 MB | **17 MB** | 67 MB |
| Rayos (validez ~40 + sol M + rebotes×N) | 16 sol, 1×32 | 64 sol, 2×128 → ≈ 300 M | 128 sol, 3×512 → ≈ 6 G |
| Tiempo con `BakeTracer` [S] | ≈ 5 s | **≈ 15-25 s** | ≈ 5-7 min |
| Tiempo con el BVH actual [S] | ≈ 20 s | ≈ 90 s | ≈ 30 min |

**Coste fijo:**

| Concepto | Cuánto |
|---|---|
| UV2 la primera vez | **30-300 s [S] (medir en la Fase 1)**; después, caché |
| Lectura del `.crdata` (339 MB) | 2-6 s [S] |
| Medias de texturas | 2-5 s [S] |
| BVH | 2-4 s [S] |
| Raster, filtro, dilatación y guardado | < 5 s [S] |

Media completa: unos 30-60 s con la caché de UV, más la desenvuelta la primera vez.

**RAM del horneado (Media):**
- geometría partida: 4.05 M vértices × 32 B ≈ 130 MB, más 36 MB de índices;
- trazador: 2.88 M × 48 B ≈ 140 MB, más unos 46 MB de nodos;
- texeles: unos 100 B × 2.1 M ≈ 210 MB;
- `rad`: 17 MB;
- xatlas en White1: **≤ 1.5 GB [S]**.

Pico de unos 2 GB durante las UV y de unos 0.6 GB mientras se traza. El editor ya ocupa lo suyo; cabe en 16 GB.

**Runtime:**
- atlas: 17 MB;
- UV2: 4.05 M × 4 B ≈ 16 MB de VRAM;
- vértices partidos: +5-10% (+20-40 MB) [S];
- registros: 32 B por hueco por frame (despreciable);
- disposiciones: se cargan perezosas y se liberan tras aplicarlas;
- en disco: zstd, unos 15-25 MB en el `.crbake` [S].

### 3.2 La ciudad de prueba

Datos [M] (verify-r2-realdata/city_x1.log y city_x8_defaults.log): 1287 mallas Static y 415 317 triángulos.
- **×1:** 13 852 m², caja de 42×14×39 m.
- **×8:** 886 524 m², 336×112×313 m.

| | Ciudad ×1 | Ciudad ×8 |
|---|---|---|
| Texeles a 20 t/m | 5.5 M ideales → unos 9 M con relleno: **supera el presupuesto de 8 M** | 354 M: imposible |
| Densidad efectiva con 8 M | ≈ 18 t/m | **≈ 2.3 t/m** (texel de 43 cm) |
| Páginas y VRAM | 2 de 2048², unos 67 MB | 2 de 2048², unos 67 MB |
| Rayos (Media) | ≈ 2.5 G | ≈ 2.5 G |
| Tiempo [S] | 2-3 min, más las UV la primera vez (1287 mallas; las repetidas, una sola vez): 1-10 min [S] | ≈ 2-3 min; el BVH, si hay decenas de millones de triángulos, ocupa 1-2 GB y tarda unos 30 s en construirse |
| Recomendación | Media; sol Horneado o Mixto | **Sol Mixto (cascadas) + lightmaps de rebote a baja densidad + sondas.** Las sombras del sol a 43 cm serían borrosas: la ventana lo avisa |

### 3.3 Valores por defecto que lo mantienen razonable

- 20 t/m y presupuesto de 8 M texeles. Si se supera, baja la densidad de toda la escena y lo avisa con la cifra.
- Página de 2048 (4096 solo en Alta o a mano).
- Piezas con diagonal < 2 texeles → sondas. `Escala en lightmap` por objeto (0 = sondas).
- Sol: 64 muestras. Rebote: 2 × 128. Filtro activado.
- Hilos = núcleos − 2, con prioridad por debajo de la normal y en tiles de 32×32 con cancelación por tile. Nunca `parallelFor` con prioridad alta durante minutos [V] (ProbeBaker.cpp:614; JobSystem.cpp:271-292).
- xatlas: una iteración (dos en Alta), trozos de ≤ 64 k caras, como mucho 2 mallas a la vez y las enormes de una en una.
- Volumen de sondas extra automático si la cobertura de lo Static baja del 95%.
- Sin lightmap: terreno, follaje, vóxeles, objetos Static con Rigidbody Dynamic o con Animator que reproduce, y mallas de código.

---

## 4. Riesgos, compatibilidad y qué NO hacer

### 4.1 Riesgos y mitigación

| Riesgo | Mitigación |
|---|---|
| Tiempo y RAM de xatlas en White1 (1.45 M caras) sin medir [S] | Medirlo es el criterio de la Fase 1. Trozos de 64 k, después 32 k con `blockAlign`. Una pieza a la vez. Cancelación por el callback [W]. |
| Orden de caras de xatlas en el commit fijado | Prueba (g) de la Fase 1. En master se conserva [W]. |
| El hash depende del orden de los índices | Se aplica **antes** de reagrupar [V] (AssetManager.cpp:193-203). El horneado lee la geometría cruda con `readModelGeometry` y nunca usa la copia de la escena para el hash. |
| Reimportar un modelo cambia el hash | `applyLightmapUvs` lo rechaza y avisa; esa pieza pasa a sondas y la ventana dice «vuelve a hornear». |
| `Scene.load` en Play no relee los modelos compartidos [V] (SceneReset.cpp:57 según el investigador) | Si la otra escena usa otra disposición, comprobar el hash hace que se usen sondas, con un aviso en la consola. Con la misma densidad la disposición es idéntica (caché y determinismo). |
| Visibilidad cuantizada a 32 niveles | Dither en el G-buffer; error ≤ 1/31 solo en las penumbras. |
| SSAO sobre la emisión real de superficies emisivas con lightmap | Aceptado: Unity hace lo mismo en diferido [S]. Se puede ajustar con «SSAO sobre lo horneado». |
| Geometría de una sola cara (cortinas): la cara de atrás muestra la luz rebotada de la de delante | El sol se evalúa con la normal del píxel, así que la directa sale bien. Para esos objetos, «Escala en lightmap = 0». Documentado. |
| Sangrado entre cartas en instancias reducidas (`s_i/s_max < 0.5`) | Aviso por objeto en el informe. Mips y costuras en la Fase 6. |
| Normales o caras invertidas en el modelo | La validez solo cuenta caras traseras a ≤ 1.5 texeles (tangentes). Un techo invertido lejos no invalida el suelo. |
| La otra tarea modifica `VulkanRenderer.cpp`, `GpuTypes.h` y `FrameBudget` | Integrar sobre su estado; usar solo `decal_info.yzw`, `probe_trust.zw` y `sky_map_depth.zw`, que están libres en el árbol actual [V]. |
| Un motor 2.4.13 o anterior abre un v4 | Da «archivo de iluminación dañado» y pasa a tiempo real [V] (ProbeBaker.cpp:1658). Inofensivo; se menciona en las notas de versión. |
| Reflejos por rayos y DDGI iluminan con luces en tiempo real | Incoherencia conocida (Fase 6). Las sondas dinámicas no deben inyectar la directa del sol: verificar `recordProbeUpdate` [S]. |

### 4.2 Compatibilidad

- **`.crbake` v1-v3:** se leen y se dibujan exactamente como hoy (celdas; sol en sondas con `sun_in_probes = true` y `sky_map_depth.w = 1`). El editor propone volver a hornear.
- **Android:**
  - un sampler y dos SSBO más, dentro de los mínimos (tabla 1.4);
  - RGBA16F, ya usado en el modo compatible [V];
  - `unpackUnorm2x16` está en SPIR-V 1.0;
  - xatlas y el lightmapper no se compilan en Android;
  - zstd ya está en CramionAssets.
- **Juegos exportados:**
  - el `.crbake` va con `Assets/` [V];
  - el lote estático excluye lo que lleva lightmap (Fase 2);
  - los UUID de las entidades persisten [V] según el investigador (IdComponent; World Partition y prefabs).
- **Static batching:** con la exclusión hay más llamadas de dibujo: en la casa, unas 100-150 [S]. Mejora en la Fase 6.

### 4.3 Qué NO hacer

1. No cambiar `asset::SkinnedVertex` ni su tamaño de 80 B, ni el formato de `.crdata` o de ModelCache.
2. No añadir texturas a `lighting.frag`, ni una segunda textura a los sets de frame o de material: el modo compatible se pasaría de 16.
3. No hornear la **directa del sol** como irradiancia en los v4: ni en sondas, ni en lightmaps, ni en celdas.
4. No filtrar la visibilidad del sol ni la directa de las luces locales: solo la indirecta.
5. No escribir datos generados en `.crscene`, `.crdata` ni `.crmat`. Solo `<Escena>.crbake` y `Library/`.
6. No usar un registro global de UV. El hilo de exportación y el lote estático leen modelos sin el conjunto de la escena abierta.
7. No aplicar disposiciones después de reagrupar ni calcular el hash sobre la copia de la escena.
8. No leer los 339 MB de geometría ni desenvolver UV en el hilo principal.
9. No reescribir el `.crbake` entero al cambiar el modo: se parchea el byte 8.
10. No guardar las páginas en la copia del renderizador después de subirlas, ni duplicar `st.result`.
11. No usar `meshopt_simplifySloppy` en mallas con UV2.
12. No compilar Embree ni OIDN desde sus fuentes. No hacer builds en paralelo; `-j 2`.
13. No usar `jobs::parallelFor` con prioridad alta en trabajos de minutos.
14. No usar RGBM8: con un sol de intensidad 55 se recorta y da bandas.
15. No hacer commits sin preguntar.

---

## 5. Supuestos que hay que medir o confirmar

| # | Supuesto | Dónde se confirma |
|---|---|---|
| 1 | Tiempo y RAM de xatlas en White1 y ocupación real del atlas (se estima 0.55-0.65 frente al 0.34 del generador actual [M]) | Fase 1 |
| 2 | Determinismo de xatlas en el commit fijado | Prueba (h) |
| 3 | Velocidad de `BakeTracer` (1-1.5 Mrayos/s por hilo) | Log de estadísticas de la Fase 2 |
| 4 | Causa de la sombra perdida en el agua | Tarea 0.9 |
| 5 | Que el SSAO a fracción 1 se vea bien en interiores. La fórmula del albedo de rebote es la del meta pass de Unity | Capturas de la Fase 3 |
| 6 | Coste del pase `baked_gi` en móviles | Fase 4.3 |
| 7 | Que las sondas dinámicas (DDGI) no metan la directa del sol | Fase 3 |
| 8 | Que los compiladores de shaders de Android acepten `flat` por vértice en mesh shaders | No aplica: Android no usa mesh shaders [V]. Comprobar solo en PC |
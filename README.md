# Cramion

<img width="1919" height="1027" alt="image" src="https://github.com/user-attachments/assets/b7fa1750-d77f-4293-9c66-b843d8dfd66e" />


Motor de render en tiempo real para Windows con **renderizador diferido en Vulkan 1.3** e iluminación física: PBR metal/rugosidad, cielo atmosférico, IBL, sombras en cascada, oclusión ambiental e iluminación global en espacio de pantalla, con post-proceso HDR (bloom, auto-exposición por histograma, rayos de luz, tonemapping).

La capa de plataforma (ventana, entrada y dispositivo DirectX 12) es una librería estática propia, **CramionDM**. Todo se compila con **CMake + Clang + Ninja**.

Incluye dos escenas de demostración, iluminadas por una sola luz direccional (el sol de día, la luna de noche) y su cielo:
- **Catedral de Šibenik** (por defecto): 75 mil triángulos. Un interior en el que el sol entra por los ventanales.
- **San Miguel**: ≈10 millones de triángulos, 287 materiales y 266 texturas. Un patio exterior.

---

## Índice

- [Requisitos](#requisitos)
- [Compilar y ejecutar](#compilar-y-ejecutar)
- [Controles](#controles)
- [Qué hace cada frame](#qué-hace-cada-frame)
- [Iluminación](#iluminación)
- [Sombras](#sombras)
- [Post-proceso](#post-proceso)
- [Geometría, carga de modelos y culling](#geometría-carga-de-modelos-y-culling)
- [Ajustes útiles](#ajustes-útiles)
- [Usar el motor en tu código](#usar-el-motor-en-tu-código)
- [Estructura del proyecto](#estructura-del-proyecto)
- [CramionDM: ventana y entrada](#cramiondm-ventana-y-entrada)
- [Limitaciones conocidas](#limitaciones-conocidas)
- [Créditos y licencias](#créditos-y-licencias)

---

## Requisitos

| | |
|---|---|
| Sistema | Windows 10/11 x64 con Windows SDK (DirectX 12) |
| GPU | Compatible con **Vulkan 1.3** (`dynamicRendering`, `synchronization2`), con una cola que haga gráficos y cómputo |
| Vulkan SDK | Instalado, con la variable `VULKAN_SDK` definida: el build usa su `glslc` para compilar los shaders |
| Herramientas | CMake ≥ 3.21, Clang/Clang++ (LLVM), Ninja |
| Memoria | San Miguel usa ~2 GB de RAM y ~2 GB de VRAM. La primera importación llega a unos 3.5 GB de RAM |

Probado con una RTX 4060 Ti (8 GB), 16 GB de RAM y Clang 22.

Las cabeceras de Vulkan y `vulkan-1.lib` vienen en `vendor/`. **assimp** y **stb** se descargan solas al configurar (FetchContent).

## Compilar y ejecutar

### 1. Las escenas

Los modelos no están en el repositorio: pesan mucho y sus licencias no permiten redistribuirlos. Descárgalos de la [McGuire Computer Graphics Archive](https://casual-effects.com/data/) y deja los zips en la raíz del proyecto con estos nombres:

| Escena | Zip | Se extrae en |
|---|---|---|
| Catedral de Šibenik | **`sibenik.zip`** (~2 MB) | `build/assets/sibenik/` |
| San Miguel (versión 2017) | **`San_Miguel.zip`** (~510 MB) | `build/assets/san-miguel/` (solo el OBJ completo, el MTL y las texturas) |

Al configurar, CMake extrae **una sola vez** cada zip que encuentre. Basta con tener uno de los dos.

### 2. Compilar

```powershell
# Debug (con capas de validación de Vulkan)
cmake --preset clang-ninja
cmake --build --preset clang-ninja

# Release (recomendado para jugar con la escena: mucho más rápido)
cmake --preset clang-ninja-release
cmake --build --preset clang-ninja-release
```

Los shaders GLSL de `shaders/` se compilan a SPIR-V en `build/shaders/` como parte del build.

### 3. Ejecutar

```powershell
.\build\cramion.exe              # catedral de Šibenik (por defecto)
.\build\cramion.exe san-miguel   # San Miguel
```

La primera vez que se abre una escena, su OBJ se importa y se guarda una caché binaria al lado (`<modelo>.obj.cramcache`). Las siguientes veces se lee la caché:

| Escena | Primera carga | Siguientes | FPS en Debug (1920×1040) |
|---|---|---|---|
| Šibenik | 0.3 s | inmediato | ~250 |
| San Miguel | ~50 s (OBJ de 1.1 GB) | ~4 s hasta el primer frame | ~50 |

Medido con la GPU de prueba. En Release va bastante más rápido. La caché se regenera sola si cambia el OBJ o el formato interno del motor.

## Controles

| Tecla | Acción |
|---|---|
| **W A S D** | Moverse |
| **Botón derecho + ratón** | Mirar |
| **Espacio / Control** | Subir / bajar |
| **Shift** | Correr (×4) |
| **Rueda** | Velocidad de movimiento |
| **T** | Activar/pausar el ciclo día/noche (~52 s por día) |
| **N** | Saltar 12 horas (día ↔ noche) |
| **G** | Sombras on/off |
| **C** | Ver las cascadas de sombra en colores |
| **O** | SSAO on/off |
| **I** | Iluminación global (luz rebotada) on/off |
| **B** | Bloom on/off |
| **L** | Rayos de luz del sol on/off |
| **E** | Auto-exposición / exposición manual |
| **RePág / AvPág** | Compensación de exposición ±0.5 EV |
| **K** | Tonemapper: Khronos PBR Neutral / ACES |
| **X** | Antialiasing (FXAA) on/off |
| **Esc** | Salir |

El **título de la ventana** muestra, entre otros datos:
- FPS y milisegundos por frame.
- Triángulos del modelo.
- Clústeres visibles / total, y cuántos se dibujan en las sombras.
- Posición de la cámara, hora del día y el estado de cada efecto.
- La exposición actual.

## Qué hace cada frame

```
 1. Sombras del sol        4 cascadas (CSM) desde el sol, con culling por cascada
    Sombras locales        focos y caras de cubo de luces puntuales (con caché)
 2. Cielo                  LUT de dispersión atmosférica (256×128)
    IBL (compute)          cubo de entorno prefiltrado + irradiancia en armónicos esféricos
 3. Geometría              G-buffer: albedo, normal+rugosidad, emisión+metalicidad, profundidad
 4. SSAO                   oclusión ambiental de pantalla
    SSGI                   luz rebotada a media resolución
 5. Iluminación            pasada diferida a pantalla completa → imagen HDR (RGBA16F)
 6. Bloom                  cadena de 6 niveles (bajada y subida)
 7. Rayos de luz           desenfoque radial del cielo hacia el sol (media resolución)
 8. Auto-exposición        histograma de luminancia + adaptación (compute)
 9. Composición            bloom + rayos → exposición → tonemapping → gradación → gamma
10. FXAA                   → swapchain
```

### G-buffer

| Destino | Formato | Contenido |
|---|---|---|
| albedo | RGBA8 | rgb = color base (sRGB), a = oclusión ambiental del material |
| normal | RGBA16F | rg = normal en octaedro, b = rugosidad |
| material | RGBA16F | rgb = emisión (radiancia HDR lineal), a = metalicidad |
| profundidad | D32 | la posición del mundo se reconstruye de aquí |

La posición se reconstruye deshaciendo solo la proyección (`z = P[3][2] / (d + P[2][2])`) y la rotación de la cámara. Usar la matriz inversa completa perdía varios centímetros de precisión a distancia.

## Iluminación

### Modelo de material: PBR metal/rugosidad

El mismo modelo que glTF 2.0 y Unreal:
- **Difuso:** Lambert.
- **Especular:** Cook-Torrance con **GGX**, visibilidad de **Smith con correlación de altura** y **Fresnel de Schlick**.
- **Dieléctricos:** F0 = 4%.
- **Metales:** F0 = su color base, y sin difuso.

Mapas que se aprovechan de cada material:

| Mapa | Uso |
|---|---|
| Color base | sRGB, con recorte por alfa (hojas, rejas) |
| Metal/rugosidad | glTF: B = metal, G = rugosidad |
| Normal map | en espacio tangente, con tangentes por vértice (MikkTSpace simplificado) |
| Oclusión | AO horneada del modelo |
| Emisión | escalada a HDR para que el bloom la recoja |

En los OBJ (que no tienen PBR):
- **Relieve:** los `map_Bump`/`bump` que son **mapas de alturas** (Šibenik) se convierten al cargar en normal maps, con la pendiente por diferencias centrales.
- **Rugosidad:** se deduce del exponente de Phong: `α = √(2/(Ns+2))`, rugosidad = √α, con un mínimo de 0.3.
- **Normal maps:** los `map_Bump` con prefijo `N_` se usan como normal map.

### Luz directa

- **Sol o luna:** una luz direccional que sigue un ciclo día/noche. Se vuelve rojiza al amanecer y al atardecer, y fría y tenue de noche.
- **Luces locales:** el motor admite hasta **32 luces puntuales** y **8 focos** con sombras. Las escenas de demostración no usan ninguna.

### Cielo físico

`sky_lut.frag` calcula cada frame la radiancia del cielo en todas las direcciones sobre una LUT de 256×128. Es dispersión simple en una atmósfera terrestre:
- **Rayleigh:** el azul del cielo y el naranja del ocaso.
- **Mie:** el halo del sol y la bruma.
- **Absorción de ozono.**
- **Término isótropo** que aproxima la dispersión múltiple.

Es la misma idea que el *Sky Atmosphere* de Unreal (Hillaire, 2020), en versión reducida.

Encima se pintan:
- el disco solar, con oscurecimiento del limbo;
- la luna;
- un campo de estrellas.

### IBL (image based lighting)

`IblProbe` regenera el entorno **cada frame** a partir del cielo. Por encima del horizonte usa el cielo; por debajo, el suelo iluminado por el sol y el cielo. Así los reflejos y la luz ambiente siguen al atardecer y a la noche sin capturas.
- **Especular:** cubo de 128 px con **6 niveles de rugosidad**, prefiltrados con GGX por *importance sampling*. Es el split-sum de Karis (UE4).
- **Difuso:** irradiancia en **armónicos esféricos de orden 2** (9 coeficientes).
- **LUT de la BRDF:** 128×128, calculada una vez al arrancar.
- **Fresnel con rugosidad** y **oclusión especular** de Lagarde.

### Oclusión ambiental

- **SSAO:** 16 muestras en la semiesfera de la normal. La rotación sigue un patrón Bayer 4×4, y se desenfoca con pesos de profundidad y normal.
- **AO con rebote múltiple** (Jiménez, GTAO): en superficies claras la oclusión oscurece menos.
- **AO horneada** de los materiales que la traen.

### Iluminación global en espacio de pantalla (SSGI)

`ssgi.frag` recoge la **luz rebotada**: en el patio, las paredes terracota tiñen de cálido las sombras y el suelo soleado ilumina los techos de los pórticos. Cómo funciona:
- **Rayos:** a media resolución, cada píxel lanza **6 rayos** con distribución coseno y recorre el depth buffer hasta **3 m**.
- **Choques:** donde un rayo choca, toma la luz de esa superficie en el **frame anterior**, reproyectada. Como esa imagen ya incluía la GI previa, los rebotes se acumulan frame a frame.
- **Escapes:** la fracción de rayos que escapa es la **visibilidad del cielo**, que ocluye el IBL a gran escala.
- **Reescalado:** se lleva a resolución completa con un filtro bilateral (profundidad + normal).

### Atmósfera y emisión

- **Niebla exponencial por altura:** se integra a lo largo del rayo y se ilumina al mirar hacia el sol (dispersión hacia delante).
- **Emisión:** se escribe como radiancia HDR en el G-buffer y la recoge el bloom.

## Sombras

### Sol: sombras en cascada (CSM)

| | |
|---|---|
| Cascadas | 4, reparto mixto logarítmico/uniforme, hasta 100 m |
| Resolución | 6144×6144 por cascada (array de profundidad) |
| Estabilidad | cada cascada envuelve una **esfera** (no depende de la orientación de la cámara) y se ajusta a la **rejilla de texels**: los bordes no "hierven" al moverse |
| Transiciones | mezcla entre cascadas en el 12% final de cada una; desvanecimiento al final de la última |
| Filtrado | **PCF de tienda 3×3** con 4 muestras (comparación por hardware) en todas las cascadas |
| Anti-acné | desplazamiento por la **normal geométrica** (reconstruida del depth, no la del normal map) proporcional al tamaño del texel y al ángulo con la luz, más *slope bias* en el raster |
| Proyectores | con *depth clamp*: lo que queda entre el sol y la cascada también proyecta sombra en ella |
| Depuración | tecla **C**: cada cascada de un color |

### Luces locales

- **Focos:** un mapa de 2048² por foco, en perspectiva.
- **Puntuales:** un cubo de 6 caras de 1024², con banda de guarda para el PCF.
- **Caché:** los mapas de una luz que no se ha movido no se redibujan.
- **Luces puntuales con sombra:** las 8 más cercanas a la cámara. Su sombra **se funde** antes de ceder el hueco a otra, para que no salte.

### Todas las sombras

- **Recorte por alfa:** las hojas proyectan la sombra de la hoja, no la de su rectángulo.
- **Doble cara:** la vegetación suele ser de una sola cara.

## Post-proceso

Todo el render es **HDR lineal** (RGBA16F) hasta la composición.

| Efecto | Detalle |
|---|---|
| **Bloom** | 6 niveles a mitad de resolución cada uno. Bajada con el filtro de 13 muestras de *Call of Duty: Advanced Warfare* y promedio de Karis en el primer nivel (sin destellos). Subida con filtro de tienda aditivo. Sin umbral: todo contribuye un poco, como una lente real |
| **Rayos de luz** | Máscara del cielo alrededor del sol + desenfoque radial de 48 muestras hacia el sol, a media resolución. Se apagan cuando el sol sale de pantalla o se pone |
| **Auto-exposición** | Como la de Unreal. Histograma de 256 cubos de log-luminancia (compute), **medición ponderada al centro**, media entre percentiles (se ignora el 50% más oscuro y el 8% más brillante), **adaptación temporal** (rápida hacia la luz, lenta hacia la oscuridad), límites de EV y **compensación** con RePág/AvPág |
| **Tonemapping** | **Khronos PBR Neutral** por defecto: conserva el tono y la saturación de los materiales. **ACES** (ajuste de Stephen Hill) como alternativa, con tecla **K** |
| **Gradación** | Contraste en escala logarítmica alrededor del gris medio, saturación, *vibrance* (satura más lo apagado) y viñeta |
| **Salida** | Gamma sRGB, *dithering* de ±1/255 contra el bandeado y **FXAA** |

## Geometría, carga de modelos y culling

### Formatos

| Formato | Cómo se carga |
|---|---|
| **OBJ + MTL** | Lector propio y paralelo (`asset/ObjLoader`). Parsea el archivo en trozos con todos los núcleos y procesa cada material en su propio hilo (vértices únicos, tangentes y clústeres). Con assimp, San Miguel tardaba más de 6 minutos |
| **glTF 2.0 / FBX** | assimp, con esqueleto, animaciones y texturas incrustadas o externas |

### Caché y texturas

- **Caché binaria** (`<modelo>.cramcache`): la primera carga guarda el modelo ya convertido; las siguientes lo leen en ~1.5 s.
- **Texturas:** se guardan comprimidas y se decodifican **en paralelo** al cargar. En la GPU tienen **mipmaps** generados en la propia GPU.

### Modelos

Todos los modelos usan el mismo pipeline de *skinning* en GPU: los escenarios estáticos tienen un solo hueso.

### Frustum culling

- **Clústeres:** cada material se parte en **clústeres espaciales de 5 m**, cada uno con su caja envolvente. San Miguel queda en 1880 clústeres.
- **Qué se descarta:** cada frame, los clústeres fuera del campo de visión de la **cámara**, de cada **cascada de sombra** y de cada **luz local**.
- **Actores animados:** se descartan enteros por su esfera envolvente.
- **Materiales:** se vinculan una sola vez por grupo de clústeres consecutivos.

### Materiales no soportados

Vidrio y agua (materiales semitransparentes) se omiten: un renderizador diferido no puede mezclarlos (ver [limitaciones](#limitaciones-conocidas)).

## Ajustes útiles

| Qué | Dónde |
|---|---|
| Escenas disponibles y posición inicial de la cámara de cada una | `src/main.cpp` (`kScenes`) y `cramion_extract_scene(...)` en `CMakeLists.txt` |
| Intensidad y color del sol/luna, ciclo día/noche | `src/cpp/scene/Scene.cpp` (`updateSun`) |
| Brillo del cielo frente al sol | `kSunIlluminance` en `src/cpp/vk/VulkanRenderer.cpp` |
| Dispersión atmosférica | constantes de `shaders/sky_lut.frag` |
| Brillo objetivo, límites y velocidad de la auto-exposición | `shaders/exposure_average.comp` (`kTargetLuminance`, `kMin/MaxLogExposure`, `kSpeedUp/Down`) y percentiles en `GpuExposurePush` (`src/include/vk/GpuTypes.h`) |
| Fuerza del bloom y de los rayos | `recordCompositePass` en `VulkanRenderer.cpp` |
| Radio e intensidad del SSAO | `shaders/ssao.frag` |
| Rayos, pasos y radio de la GI | `shaders/ssgi.frag` (`kRays`, `kSteps`, `kRadius`) |
| Niebla | `shaders/lighting.frag` (`kFogDensity`, `kFogBaseHeight`, `kFogHeightFalloff`) |
| Contraste, *vibrance*, saturación, viñeta | `shaders/composite.frag` y `GpuCompositePush` |
| Resolución y distancia de las sombras | `ShadowMap::kResolution`, `ShadowCascades::shadow_distance_`, `LocalShadowMaps` |
| Tamaño de los clústeres de culling | `kClusterSize` en `src/cpp/asset/ObjLoader.cpp` |

## Usar el motor en tu código

El flujo mínimo (lo que hace `src/main.cpp`):

```cpp
#include <CramionDM/CramionDM.h>
#include "scene/Scene.h"
#include "vk/VulkanRenderer.h"

using namespace cramion;

dm::Window window;
window.create({.title = L"Mi escena", .width = 1600, .height = 900});
dm::Input input;

scene::Scene scene;
scene.initialize();                                        // sol + cielo
const auto model = scene.loadModel("assets/mi_escena.obj"); // OBJ, glTF o FBX
scene.spawnStatic(model);                                  // escenario tal cual
// scene.spawnActor(model, x, z, altura, yaw);             // personaje animado sobre y = 0
scene.placeCamera({0.0f, 1.7f, -5.0f}, {0.0f, 1.7f, 0.0f});

gfx::VulkanRenderer renderer;
renderer.initialize({.app_name = "Mi app", .engine_name = "Cramion"},
                    window.handle(), window.width(), window.height());
renderer.uploadModels(scene);                              // sube mallas, texturas y materiales

while (window.isOpen()) {
    window.pumpEvents();
    scene.update(input, delta_seconds);                    // cámara, sol, animaciones
    renderer.drawFrame(scene);
    input.newFrame();
}
renderer.shutdown();
```

Cada efecto se puede encender o apagar desde código: `setShadowsEnabled`, `setSsaoEnabled`, `setGiEnabled`, `setBloomEnabled`, `setLightShaftsEnabled`, `setAutoExposureEnabled`, `setExposureCompensation`, `setAcesTonemapper`, `setAntialiasingEnabled` y `setCascadeDebug`.

Las luces locales se añaden a `LightSet::points` y `LightSet::spots` (`src/include/scene/Light.h`).

## Estructura del proyecto

```
Cramion/
├── CMakeLists.txt          # Proyecto, dependencias, shaders y extracción de la escena
├── CMakePresets.json       # Presets Clang + Ninja (Debug / Release)
├── sibenik.zip             # (no incluido) escena de demostración
├── San_Miguel.zip          # (no incluido) escena de demostración
├── CramionDM/              # Librería estática: ventana Win32, entrada, dispositivo DX12
├── shaders/                # GLSL → SPIR-V
│   ├── skinned.*           # G-buffer de los modelos (PBR completo)
│   ├── skinned_shadow.*    # Sombras con recorte por alfa
│   ├── lighting.*          # Pasada diferida: PBR, IBL, sombras, niebla
│   ├── sky_lut.frag        # Cielo atmosférico
│   ├── ibl_*.comp, brdf_lut.comp, ibl_common.glsl   # IBL
│   ├── ssao.frag, ssgi.frag                         # Oclusión y luz rebotada
│   ├── bloom_*.frag, light_shafts.frag              # Bloom y rayos de luz
│   ├── exposure_*.comp                              # Auto-exposición
│   ├── composite.frag                               # Exposición, tono y gradación
│   └── fxaa.frag
├── src/
│   ├── main.cpp            # Arranque, bucle principal y teclas
│   ├── include/ , cpp/
│   │   ├── anim/           # Animator: esqueletos y clips
│   │   ├── asset/          # Model, ModelLoader (assimp), ObjLoader, ModelCache
│   │   ├── core/           # Math, Frustum, Clock
│   │   ├── scene/          # Scene, Camera, Light, ShadowCascades, LocalLightShadows
│   │   └── vk/             # Renderizador Vulkan: dispositivo, swapchain, pasadas, IBL...
└── vendor/                 # Cabeceras de Vulkan y vulkan-1.lib
```

## CramionDM: ventana y entrada

CramionDM ofrece dos formas complementarias de manejar la entrada.

**Eventos (callbacks)**, para reaccionar a acciones puntuales:

```cpp
window.setEventCallback([&](Event& e) {
    switch (e.type) {
        case EventType::WindowResize:  /* e.width, e.height */ break;
        case EventType::KeyPressed:    /* e.key, e.repeat, e.mods */ break;
        case EventType::MouseMoved:    /* e.mouseX/Y, e.deltaX/Y */ break;
        case EventType::MouseScrolled: /* e.scrollY */ break;
        case EventType::FileDropped:   /* e.paths: arrastrar y soltar */ break;
        default: break;
    }
});
```

Eventos de ventana:
- `WindowClose` y `WindowResize`.
- Foco: `WindowFocus` y `WindowLostFocus`.
- Estado: `WindowMoved`, `WindowMinimized`, `WindowMaximized` y `WindowRestored`.
- `WindowDpiChanged`.

Eventos de teclado y ratón:
- Teclado: `KeyPressed` y `KeyReleased`, y `TextInput` para texto en Unicode.
- Ratón: botones, movimiento con delta, rueda vertical y horizontal, y entrada y salida de la ventana.

**Estado consultable (polling)**, por frame:

```cpp
if (input.isKeyDown(Key::W))        mover();    // mantenida
if (input.isKeyPressed(Key::Space)) saltar();   // solo el frame de la pulsación
if (input.isMouseButtonDown(MouseButton::Left)) disparar();
input.newFrame();
```

Las teclas (`KeyCode.h`) siguen los Virtual-Key Codes de Windows. `keyName()` da un nombre legible.

## Limitaciones conocidas

- **Sin transparencias:** el renderizador es diferido, así que el vidrio y el agua se omiten. Se reconocen por `d`/`Tr`, por la transmisión `Tf` o por los modelos `illum` de vidrio. Por eso la fuente de San Miguel no tiene agua, y las vidrieras de colores de Šibenik dejan pasar la luz sin teñirla.
- **Luz del cielo en interiores:** el IBL es la luz de un exterior. En el interior de Šibenik solo la ocluyen el SSAO y la visibilidad de cielo de la SSGI (rayos de 3 m), así que el ambiente queda más claro de lo real en las zonas lejos de las paredes.
- **La SSGI es de pantalla:** lo que queda fuera de la vista no rebota luz, y la luz rebotada puede cambiar algo al girar la cámara.
- **Arranque minimizado:** si el programa arranca con la ventana minimizada, se cierra (la swapchain no admite tamaño 0).
- **Solo Windows** (Win32 + DirectX 12 en la capa de plataforma).
- **Avisos de validación en Debug:** las capas de validación avisan de atributos de vértice que los pipelines de sombra no usan. Es inofensivo.

## Créditos y licencias

- **Código de Cramion:** licencia MIT (ver [LICENSE](LICENSE)).
- **Catedral de Šibenik:** de Marko Dabrović ([RNA studio](http://www.rna.hr)). Huecos corregidos por Kenzie Lamar (Vicarious Visions), texturas y mapas de relieve de Morgan McGuire; publicada en [casual-effects.com](https://casual-effects.com/data/). No se redistribuye con este repositorio.
- **San Miguel:** modelado por Guillermo M. Leal Llaguno (Evolución Visual). Versión 2017 mejorada por Morgan McGuire, Guedis Cárdenas, Michael Mara y Nicholas Hull, publicada en [casual-effects.com](https://casual-effects.com/data/). **Solo para uso educativo y de investigación, con atribución.** No se redistribuye con este repositorio.
- **Dependencias:**
  - [assimp](https://github.com/assimp/assimp): BSD-3.
  - [stb_image](https://github.com/nothings/stb): dominio público / MIT.
  - [Vulkan SDK](https://vulkan.lunarg.com/): Apache 2.0.
- **Técnicas en las que se basa:**
  - Karis, *Real Shading in Unreal Engine 4* (2013).
  - Hillaire, *A Scalable and Production Ready Sky and Atmosphere Rendering Technique* (2020).
  - Jiménez, *Next Generation Post Processing in Call of Duty: Advanced Warfare* (2014).
  - Jiménez et al., *Practical Realtime Strategies for Accurate Indirect Occlusion* (GTAO, 2016).
  - Lagarde y de Rousiers, *Moving Frostbite to PBR* (2014).
  - Ramamoorthi y Hanrahan, *An Efficient Representation for Irradiance Environment Maps* (2001).
  - Khronos, *PBR Neutral Tone Mapper* (2024).

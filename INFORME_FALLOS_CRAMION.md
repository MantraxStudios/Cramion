# Informe de fallos y observaciones — Cramion 2.4.6

Fecha: 12 de octubre de 2026
Proyecto: Keryan (MMORPG, mundo de 12 × 12 km con `TerrainGrid`)

## Entorno

| | |
|---|---|
| Sistema | Windows 11 Pro 10.0.26200 |
| GPU | AMD Radeon(TM) Graphics (integrada; el motor la clasifica como gama "Bajo") |
| Editor | binarios oficiales 2.4.6 (`CramionEditor.exe`, compilado el 12-10-2026) |
| Código fuente | `github.com/MantraxStudios/Cramion`, rama `main`, commit `3504d25` |
| Compilador de scripts | clang incluido con el motor: `clang version 23.1.2 (85ac560262434c9ccfc0c183ec22d4138ed647fb)`, `--target=x86_64-w64-mingw32` |
| Compilación del motor | Visual Studio 2022 17.14, Clang 19.1.5 (componente de VS), MSVC 14.44, Windows SDK 26100, Vulkan SDK 1.4.363.0, Ninja |

Resumen por gravedad:

| # | Asunto | Gravedad |
|---|---|---|
| 1 | Las pruebas `CRAMION_TEST` con `co_await` se dan por buenas sin ejecutarse | **Alta**: resultados falsos |
| 2 | `create_entity` y `ground_height` (MCP) usan coordenadas locales del origen flotante sin indicarlo | Media |
| 3 | El editor se frena a ~9 FPS sin foco y, con `max_substeps` = 4, la física se ralentiza en Play y en `run_tests` | Media |
| 4 | Recargar un script de C++ en caliente durante Play con una corrutina viva deja el estado incoherente | Media (por confirmar) |
| 5 | La captura de la sonda de reflexión cuesta 105–180 ms por frame con un `TerrainGrid` grande | Media (rendimiento) |
| 6 | Consola: `Scene.destroy` no actúa en modo edición y `e.parent` es de solo lectura | Baja |
| 7 | Compilar desde el código de GitHub: falta la carpeta `assets/`, hace falta desactivar el escaneo de módulos y las etiquetas van atrasadas | Baja |

---

## 1. Las pruebas `CRAMION_TEST` con `co_await` se dan por buenas sin ejecutarse

**Qué pasa.** Cualquier prueba que use `co_await` (por ejemplo `co_await Test::waitFrames(1)`) termina al momento:
- estado `passed`, 0 segundos y 0 comprobaciones;
- la consola muestra `[Scripts] [Test] N bien, 0 mal`;
- el cuerpo no llega a ejecutarse: un `Debug::log` antes del primer `co_await` no aparece en la consola.

Las pruebas sin `co_await` funcionan.

**Causa.** En `CramionCore/sdk/cramion/Test.h`, `RootPromise::get_return_object()` devuelve `void` y es donde se rellena `slot->handle` y `slot->promise`. Con el clang 23.1.2 que trae el motor, **`get_return_object()` no se llega a llamar** cuando la función corrutina devuelve `void`. Así, `CramionTests::begin()` ve `slot.handle` vacío y cierra la prueba como superada (`end(nullptr)`).

**Reproducción fuera del motor** (mismo compilador y mismas opciones que usa el editor):

```cpp
// r.cpp — clang++ --target=x86_64-w64-mingw32 -std=c++20 -O2 -static r.cpp -o r.exe
#include <coroutine>
#include <cstdio>
struct Slot { std::coroutine_handle<> h; };
struct Context { Slot* slot = nullptr; };
struct RootPromise {
    Slot* slot;
    explicit RootPromise(Context c) : slot(c.slot) { std::printf("ctor slot=%p\n", (void*)c.slot); }
    void get_return_object() noexcept {
        std::printf("gro slot=%p\n", (void*)slot);
        if (slot) slot->h = std::coroutine_handle<RootPromise>::from_promise(*this);
    }
    std::suspend_always initial_suspend() noexcept { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }
    void return_void() noexcept {}
    void unhandled_exception() noexcept {}
};
namespace std { template <> struct coroutine_traits<void, ::Context> { using promise_type = ::RootPromise; }; }
static void prueba(Context) { std::puts("cuerpo 1"); co_await std::suspend_always{}; std::puts("cuerpo 2"); }
int main() {
    Slot s;
    std::printf("main slot=%p\n", (void*)&s);
    prueba(Context{&s});
    std::printf("handle %s\n", s.h ? "SI" : "NULO");
    if (s.h) { s.h.resume(); s.h.resume(); }
}
```

Salida obtenida, igual con `-O0` y con `-O2`. No aparece ninguna línea `gro`, y el cuerpo no se ejecuta:

```text
main slot=000000AD5354F960
ctor slot=000000AD5354F960
handle NULO
```

Con una corrutina que devuelve un objeto (un `promise_type` cuyo `get_return_object()` devuelve un tipo distinto de `void`), el mismo compilador funciona bien: `handle SI`, `cuerpo 1`, `cuerpo 2`.

**Propuesta de corrección.** No dejar el registro en un `get_return_object()` que devuelve `void`. Cualquiera de estas dos opciones sirve:
- registrar el *handle* en el constructor de la promesa:
  `explicit RootPromise(Context c) : slot(c.slot) { leaf = handle::from_promise(*this); if (slot) { slot->handle = leaf; slot->promise = this; } }`
- o hacerlo en el `await_suspend(handle)` de un *awaiter* propio devuelto por `initial_suspend()`.

Convendría además que `CramionTests` marcara como **fallida**, y no como superada, una prueba que termina con 0 comprobaciones y sin haberse ejecutado.

**Solución provisional en nuestro proyecto.** Usamos un script propio (`Script` con `update()`) que reanuda una corrutina con tipo de retorno no `void`.

---

## 2. `create_entity` y `ground_height` usan coordenadas locales del origen flotante sin indicarlo

**Qué pasa.** Cuando el origen flotante se ha desplazado, las coordenadas que se pasan por MCP no son absolutas, y la descripción de las herramientas no lo dice:
- `create_entity` interpreta `position` en coordenadas **locales** (relativas al origen actual).
- `ground_height` también recibe `x`, `z` **locales**.

**Ejemplo.** Con el origen en (5120, 0, −6144):
- `create_entity(position = [0, −100, −12000])` dejó la entidad en la posición absoluta (5120, −100, −18144).
- `Scene.toLocal(0, −100, −12000)` devolvía (−5120, −100, −5856).
- `ground_height(5700, −6500)` devolvía `found: false`, y `ground_height(580, −356)` (la misma posición en local) devolvía la altura correcta.

**Propuesta.** Indicar en cada herramienta si sus coordenadas son locales o absolutas, o aceptar un parámetro `absolute: true`. Esto es especialmente importante en mundos grandes con `TerrainGrid`.

---

## 3. El editor se frena sin foco y la física simulada se ralentiza en Play y en `run_tests`

**Qué pasa.**
1. Con la ventana del editor en segundo plano, el editor baja a unos **9 FPS** (CPU ~109 ms por frame, casi todo en "Editor e interfaz (incluye la lógica en Play)"). Con la ventana delante y la misma escena, va a **60 FPS** (CPU 11 ms).
2. Con `max_substeps` = 4 (valor por defecto en `ProjectSettings/Physics.json`, paso fijo de 1/60 s), a ~9 FPS la física solo simula unos 0,067 s por frame de 0,105 s. Mientras tanto, `Time::time()` avanza en tiempo real.

**Consecuencia.** Un `CharacterController` con `walk_speed` 4 se midió a **2,3 m/s**, y a 3,7 m/s con `run_speed` 7,5. Con `max_substeps` = 12 se midió exactamente 4,0 y 7,5 m/s. Las pruebas automáticas lanzadas por MCP (`run_tests`) con el editor sin foco dan por tanto tiempos y velocidades falsos.

**Propuesta.**
- No frenar el editor en segundo plano mientras está en Play o ejecutando pruebas, o documentarlo y avisar en la consola cuando ocurre.
- Avisar en la consola cuando la física pierde tiempo por `max_substeps`.
- Ofrecer en la API el tiempo de física simulado, para medir velocidades sin depender de los FPS.

---

## 4. Recargar un script de C++ en caliente durante Play deja el estado incoherente (por confirmar)

**Qué pasa.** Con Play activo y un script ejecutando una corrutina propia (tramas en el montón del DLL), se modificó y guardó el `.cpp`. El editor lo recompiló y lo recargó en caliente: consola `Scripts recargados en caliente (2 con su estado)`. A partir de ahí, el personaje que conducía el script dejó de seguir el recorrido de forma coherente.

**Nota.** Durante ese mismo rato el editor también estaba sin foco (punto 3), así que la causa exacta queda por confirmar.

**Propuesta.** Avisar de que la recarga en caliente no conserva las corrutinas vivas, o detenerlas de forma controlada antes de cambiar de DLL.

---

## 5. La captura de la sonda de reflexión es muy cara con un `TerrainGrid` grande

**Qué pasa.** La escena tiene:
- un `TerrainGrid` de 8 × 8 teselas de 1500 m con 1025 alturas y `load_range` 2500;
- un `Terrain` lejano de 2049 alturas sin colisión;
- océano, 3 lagos, 2 ríos y unos 300 cubos de blockout.

En Play, el campo `sonda` del aviso "Frame lento" marca **105–180 ms por frame** en la GPU integrada mientras la cámara se mueve. Corresponde a `captureProbeFace()` en `VulkanRenderer.cpp` (`frame_timings_.probe_ms`).

Ejemplo de la consola:

```text
[Rendimiento] Frame lento de 284 ms: ... render 281 (espera GPU 99, actores 0, sonda 179, ...) | GPU 111 ms, 309 actores
```

**Propuesta.** Un ajuste (CVar) para limitar la frecuencia o la distancia de dibujo de la captura de la sonda, o excluir el terreno lejano y las teselas distantes de la captura.

---

## 6. Consola de la API en modo edición

- `Scene.destroy(e)` no borra la entidad en modo edición. Tuvimos que usar `delete_entity` por MCP.
- `e.parent` es de solo lectura desde la consola. Las copias de `Scene.instantiate` heredan el padre de la plantilla, así que la única forma de colocarlas en un grupo es crear la plantilla dentro de ese grupo.

**Propuesta.** Documentarlo, o permitir `Scene.destroy` y asignar `parent` en modo edición.

---

## 7. Compilar el motor desde el código de GitHub

- **Falta la carpeta `assets/`.** El icono `icon.ico` (que referencia `cramion.rc`) y los iconos de gizmos no están en el repositorio. Los reconstruimos a partir de los binarios oficiales.
- **CMake busca `clang-scan-deps`.** Hace falta `-DCMAKE_CXX_SCAN_FOR_MODULES=OFF` para compilar con el Clang de Visual Studio.
- **El clang incluido no compila el motor.** La toolchain mingw que trae el motor no tiene la parte de libc++ que se necesita; hay que usar el componente Clang de Visual Studio 2022.
- **Etiquetas atrasadas.** Las etiquetas del repositorio no corresponden a la 2.4.6; el código 2.4.6 está en la rama `main`.

**Propuesta.** Incluir `assets/` en el repositorio, documentar los requisitos de compilación y etiquetar las versiones publicadas.

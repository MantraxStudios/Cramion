# Cramion

Motor construido con **CMake + Clang + Ninja** sobre una librería estática propia de **DirectX 12**: `CramionDM`.

## Requisitos

- Windows 10/11 con Windows SDK (DirectX 12)
- CMake ≥ 3.21
- Clang / Clang++
- Ninja
- *(Opcional)* "Graphics Tools" de Windows para la capa de depuración de DX12

## Compilar

```powershell
# Configurar (Debug)
cmake --preset clang-ninja

# Compilar
cmake --build --preset clang-ninja

# Ejecutar
.\build\cramion.exe
```

Para una build de Release usa el preset `clang-ninja-release`.

## Estructura

```
Cramion/
├── CMakeLists.txt            # Proyecto raíz, enlaza Cramion::DM
├── CMakePresets.json         # Presets Clang + Ninja (Debug / Release)
├── src/
│   └── main.cpp              # Arranque: inicializa CramionDM y reporta la GPU
└── CramionDM/                # Librería estática DirectX 12
    ├── CMakeLists.txt        # Enlaza d3d12 / dxgi / d3dcompiler / dxguid / user32 / gdi32
    ├── include/CramionDM/
    │   ├── CramionDM.h       # Cabecera pública principal (incluye todo)
    │   ├── Device.h          # Dispositivo DX12 (factory, adaptador, cola)
    │   ├── Window.h          # Ventana Win32 + bombeo de eventos
    │   ├── Event.h           # Tipos de evento (ventana / teclado / ratón)
    │   ├── Input.h           # Estado de entrada consultable por frame
    │   └── KeyCode.h         # Teclas, botones y modificadores
    └── src/
        ├── Device.cpp
        ├── Window.cpp
        ├── Event.cpp
        ├── Input.cpp
        └── KeyCode.cpp
```

## Sistema de eventos y entrada

CramionDM ofrece **dos formas complementarias** de manejar la entrada:

### 1) Eventos (callbacks) — reaccionar a acciones puntuales

```cpp
window.setEventCallback([&](Event& e) {
    switch (e.type) {
        case EventType::WindowResize: /* e.width, e.height */ break;
        case EventType::KeyPressed:   /* e.key, e.repeat, e.mods */ break;
        case EventType::MouseMoved:   /* e.mouseX/Y, e.deltaX/Y */ break;
        case EventType::MouseScrolled:/* e.scrollY */ break;
        default: break;
    }
});
```

Eventos soportados:

- **Ventana:** `WindowClose`, `WindowResize`, `WindowFocus`, `WindowLostFocus`,
  `WindowMoved`, `WindowMinimized`, `WindowMaximized`, `WindowRestored`,
  `WindowDpiChanged` (con `e.dpiScale`).
- **Teclado:** `KeyPressed` (con `e.repeat` y `e.mods`), `KeyReleased`, `TextInput` (Unicode).
- **Ratón:** `MouseButtonPressed`, `MouseButtonReleased`, `MouseMoved` (con delta),
  `MouseScrolled` (vertical/horizontal), `MouseEnter`, `MouseLeave`.
- **Archivos:** `FileDropped` — arrastrar y soltar desde el explorador; `e.paths`
  contiene las rutas y `e.mouseX/e.mouseY` la posición donde se soltaron.

```cpp
case EventType::FileDropped:
    for (const auto& ruta : e.paths) cargarArchivo(ruta);
    break;
```

### 2) Estado consultable (polling) — típico de un motor por frame

```cpp
Input input;
window.setEventCallback([&](Event& e){ input.onEvent(e); });

while (window.isOpen()) {
    window.pumpEvents();
    if (input.isKeyDown(Key::W))       mover();      // mantenida
    if (input.isKeyPressed(Key::Space)) saltar();    // solo el frame de la pulsación
    if (input.isMouseButtonDown(MouseButton::Left)) disparar();
    input.newFrame();  // limpiar estados de un frame
}
```

**Teclas** (`KeyCode.h`): todas las teclas mapeadas a los Virtual-Key Codes de
Windows — alfanuméricas, F1–F12, navegación, teclado numérico, puntuación OEM y
modificadores izquierda/derecha (Shift, Ctrl, Alt, Super). `keyName()` da un
nombre legible. **Botones de ratón**: `Left`, `Right`, `Middle`, `X1`, `X2`.


## Uso de la librería

```cpp
#include <CramionDM/CramionDM.h>

cramion::dm::Device device;
if (device.initialize()) {
    // device.device()        -> ID3D12Device*
    // device.graphicsQueue()  -> ID3D12CommandQueue*
}
```

Enlázala en tu target con:

```cmake
target_link_libraries(mi_app PRIVATE Cramion::DM)
```

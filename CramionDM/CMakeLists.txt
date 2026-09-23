# Librería estática CramionDM — capa de DirectX 12

add_library(CramionDM STATIC
    src/Device.cpp
    src/Event.cpp
    src/Input.cpp
    src/KeyCode.cpp
    src/Window.cpp
)

# Alias para poder enlazar como Cramion::DM
add_library(Cramion::DM ALIAS CramionDM)

target_include_directories(CramionDM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(CramionDM PUBLIC cxx_std_20)

# DirectX 12 y dependencias del Windows SDK
target_link_libraries(CramionDM PUBLIC
    d3d12
    dxgi
    d3dcompiler
    dxguid
    user32    # Ventanas y mensajes
    gdi32     # Contexto gráfico de la ventana
    shell32   # Arrastrar y soltar archivos (DragQueryFile)
)

# DX12 es solo Windows
target_compile_definitions(CramionDM PUBLIC
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_compile_options(CramionDM PRIVATE
    -Wall
    -Wextra
)

# Librería estática CramionDM — capa de DirectX 12

if(ANDROID)
    # NativeActivity: la ventana (ANativeWindow) y la entrada (tactil,
    # teclas, mando) llegan por android_native_app_glue.
    set(CRAMION_APP_GLUE "${ANDROID_NDK}/sources/android/native_app_glue")
    add_library(cramion_app_glue STATIC "${CRAMION_APP_GLUE}/android_native_app_glue.c")
    target_include_directories(cramion_app_glue PUBLIC "${CRAMION_APP_GLUE}")
    target_link_libraries(cramion_app_glue PUBLIC android log)
    target_compile_options(cramion_app_glue PRIVATE -w)

    add_library(CramionDM STATIC
        src/Event.cpp
        src/Input.cpp
        src/KeyCode.cpp
        src/TouchControls.cpp
        src/WindowAndroid.cpp
    )
    add_library(Cramion::DM ALIAS CramionDM)
    target_include_directories(CramionDM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_features(CramionDM PUBLIC cxx_std_20)
    target_link_libraries(CramionDM PUBLIC cramion_app_glue android log)
    target_compile_options(CramionDM PRIVATE -Wall -Wextra)
    return()
endif()

if(UNIX AND NOT APPLE)
    # Linux: ventana Xlib (X11 o XWayland), sin DirectX.
    find_package(X11 REQUIRED)
    add_library(CramionDM STATIC
        src/Event.cpp
        src/Input.cpp
        src/KeyCode.cpp
        src/TouchControls.cpp
        src/WindowX11.cpp
    )
    add_library(Cramion::DM ALIAS CramionDM)
    target_include_directories(CramionDM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_compile_features(CramionDM PUBLIC cxx_std_20)
    target_link_libraries(CramionDM PUBLIC X11::X11)
    target_compile_options(CramionDM PRIVATE -Wall -Wextra)
    return()
endif()

add_library(CramionDM STATIC
    src/Device.cpp
    src/Event.cpp
    src/Input.cpp
    src/KeyCode.cpp
    src/TouchControls.cpp
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

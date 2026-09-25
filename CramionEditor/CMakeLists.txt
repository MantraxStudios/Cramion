# CramionEditor: editor de escenas al estilo de Unity sobre CramionFX, con la
# interfaz en Dear ImGui (rama docking: paneles acoplables).
#
#   Escena (la vista del motor) | Jerarquia | Inspector | Render |
#   Estadisticas (perfilador de GPU) | Consola
#
# El ejecutable queda junto a cramion.exe (build/): comparte los shaders y la
# carpeta assets/ con las escenas.

include(FetchContent)

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.9-docking
    GIT_SHALLOW TRUE
)
# ImGuizmo: gizmos de mover/rotar/escalar sobre la vista (fijado a un commit).
FetchContent_Declare(imguizmo
    GIT_REPOSITORY https://github.com/CedricGuillemet/ImGuizmo.git
    GIT_TAG 18cef5e031d8c6973d80284c67f60549fafd78c1
    # Solo sus fuentes: su CMakeLists (ejemplos) no se usa.
    SOURCE_SUBDIR no-cmake
)
FetchContent_MakeAvailable(imgui imguizmo)

# Dear ImGui + sus backends de Win32 y Vulkan. Las cabeceras de Vulkan y
# vulkan-1.lib vienen de CramionFX.
add_library(cramion_imgui STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/imgui_demo.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp"
    "${imguizmo_SOURCE_DIR}/src/ImGuizmo.cpp"
    "${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp"
)
target_include_directories(cramion_imgui SYSTEM PUBLIC
    "${imgui_SOURCE_DIR}"
    "${imgui_SOURCE_DIR}/backends"
    "${imguizmo_SOURCE_DIR}/src"
    "${imgui_SOURCE_DIR}/misc/cpp"
)
target_link_libraries(cramion_imgui PUBLIC Cramion::FX dwmapi imm32)
# Codigo de terceros: sin nuestros avisos y optimizado tambien en Debug.
target_compile_options(cramion_imgui PRIVATE -O2 -w)

file(GLOB CRAMION_EDITOR_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
)

add_executable(CramionEditor ${CRAMION_EDITOR_SOURCES} "${CRAMION_ICON_RC}")
target_link_libraries(CramionEditor PRIVATE Cramion::Core cramion_imgui comdlg32 shell32 ole32 gdiplus dbghelp)
# Las escenas de demostracion (src/DemoScenes.h) las comparte con cramion.exe.
target_include_directories(CramionEditor PRIVATE "${CMAKE_SOURCE_DIR}/src")
set_target_properties(CramionEditor PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
cramionfx_deploy(CramionEditor)

# Iconos del editor (assets/gizmos) junto al ejecutable, en editor_icons/.
# Si faltan alli, el editor los busca en la carpeta del codigo.
target_compile_definitions(CramionEditor PRIVATE
    CRAMION_EDITOR_ICON_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/assets/gizmos"
    CRAMION_EDITOR_LOGO_SOURCE="${CMAKE_SOURCE_DIR}/assets/icon.png")
add_custom_command(TARGET CramionEditor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/gizmos" "${CMAKE_BINARY_DIR}/editor_icons"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/assets/icon.png" "${CMAKE_BINARY_DIR}/editor_icons/logo.png"
    COMMENT "Copiando los iconos del editor")

target_compile_options(CramionEditor PRIVATE
    -Wall
    -Wextra
)

# Prueba de las miniaturas de modelos (consola, sin GPU).
add_executable(CramionPreviewTests "${CMAKE_CURRENT_SOURCE_DIR}/tests/PreviewTests.cpp"
                                   "${CMAKE_CURRENT_SOURCE_DIR}/src/ModelPreviews.cpp")
target_link_libraries(CramionPreviewTests PRIVATE Cramion::Core)

# Prueba del autocompletado de Lua (consola).
add_executable(CramionCompletionTests "${CMAKE_CURRENT_SOURCE_DIR}/tests/CompletionTests.cpp"
                                      "${CMAKE_CURRENT_SOURCE_DIR}/src/LuaCompletion.cpp")

# Prueba de la edicion multiple del Inspector (consola).
add_executable(CramionMultiEditTests "${CMAKE_CURRENT_SOURCE_DIR}/tests/MultiEditTests.cpp"
                                     "${CMAKE_CURRENT_SOURCE_DIR}/src/PropertyInspector.cpp")
target_include_directories(CramionMultiEditTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(CramionMultiEditTests PRIVATE Cramion::Core cramion_imgui)

# --- CramionPlayer: el juego exportado (Archivo > Exportar juego) ---
# Banner del motor al abrir, la escena inicial y todos los sistemas del
# juego (sin el editor). Sin consola.
add_executable(CramionPlayer
    "${CMAKE_CURRENT_SOURCE_DIR}/player/PlayerMain.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ImGuiLayer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/UiRenderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/LoadingScreen.cpp"
    "${CRAMION_ICON_RC}")
target_include_directories(CramionPlayer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(CramionPlayer PRIVATE Cramion::Core cramion_imgui shell32 ole32 gdiplus dbghelp)
set_target_properties(CramionPlayer PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}" WIN32_EXECUTABLE TRUE)
target_link_options(CramionPlayer PRIVATE "-Xlinker" "/entry:mainCRTStartup")
target_compile_definitions(CramionPlayer PRIVATE
    CRAMION_EDITOR_ICON_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/assets/gizmos"
    CRAMION_EDITOR_LOGO_SOURCE="${CMAKE_SOURCE_DIR}/assets/icon.png")
cramionfx_deploy(CramionPlayer)
# El banner del motor junto a los ejecutables (lo copia la exportacion).
add_custom_command(TARGET CramionPlayer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/assets/bannerintro.png" "${CMAKE_BINARY_DIR}/player_banner.png"
    COMMENT "Banner del juego exportado")
add_dependencies(CramionEditor CramionPlayer)

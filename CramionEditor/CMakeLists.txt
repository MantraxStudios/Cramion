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

# --- Android: libmain.so, el juego exportado (NativeActivity) ---
# El mismo player que en Windows (player/PlayerMain.cpp) sin el editor. La
# entrada de ImGui la da el propio player (tactil convertido a raton), asi
# que solo hace falta el backend de Vulkan.
if(ANDROID)
    add_library(cramion_imgui STATIC
        "${imgui_SOURCE_DIR}/imgui.cpp"
        "${imgui_SOURCE_DIR}/imgui_draw.cpp"
        "${imgui_SOURCE_DIR}/imgui_tables.cpp"
        "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
        "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp"
        "${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp"
    )
    target_include_directories(cramion_imgui SYSTEM PUBLIC
        "${imgui_SOURCE_DIR}"
        "${imgui_SOURCE_DIR}/backends"
        "${imgui_SOURCE_DIR}/misc/cpp"
    )
    target_link_libraries(cramion_imgui PUBLIC Cramion::FX)
    target_compile_options(cramion_imgui PRIVATE -O2 -w)

    add_library(main SHARED
        "${CMAKE_CURRENT_SOURCE_DIR}/player/PlayerMain.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/player/AndroidSupport.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/GraphicsConfig.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ImGuiLayer.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/UiRenderer.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ProfilerOverlay.cpp"
    )
    target_include_directories(main PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(main PRIVATE Cramion::Core cramion_imgui android log)
    # La glue exporta ANativeActivity_onCreate: que el enlazador no la quite.
    target_link_options(main PRIVATE "-u" "ANativeActivity_onCreate")
    target_compile_options(main PRIVATE -Wall -Wextra)
    return()
endif()

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
target_link_libraries(CramionEditor PRIVATE Cramion::Core cramion_imgui cramion_update comdlg32 shell32 ole32 gdiplus dbghelp ws2_32)
# Las escenas de demostracion (src/DemoScenes.h) las comparte con cramion.exe.
target_include_directories(CramionEditor PRIVATE "${CMAKE_SOURCE_DIR}/src")
set_target_properties(CramionEditor PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
# En Release, aplicacion de ventana: sin la consola negra detras (los mensajes
# siguen en el panel Consola). En Debug se deja la consola para desarrollar.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set_target_properties(CramionEditor PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_options(CramionEditor PRIVATE "-Xlinker" "/entry:mainCRTStartup")
endif()
cramionfx_deploy(CramionEditor)

# Iconos del editor (assets/gizmos) junto al ejecutable, en editor_icons/.
# Si faltan alli, el editor los busca en la carpeta del codigo.
target_compile_definitions(CramionEditor PRIVATE
    CRAMION_EDITOR_ICON_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/assets/gizmos"
    CRAMION_EDITOR_LOGO_SOURCE="${CMAKE_SOURCE_DIR}/assets/icon.png"
    CRAMION_VERSION_STRING="${CRAMION_VERSION_FULL}")
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

# Prueba de la terminal integrada: emulador VT y pseudoconsola (consola).
add_executable(CramionTerminalTests "${CMAKE_CURRENT_SOURCE_DIR}/tests/TerminalTests.cpp"
                                    "${CMAKE_CURRENT_SOURCE_DIR}/src/Terminal.cpp")
target_include_directories(CramionTerminalTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

# Prueba del autocompletado de Lua (consola).
add_executable(CramionCompletionTests "${CMAKE_CURRENT_SOURCE_DIR}/tests/CompletionTests.cpp"
                                      "${CMAKE_CURRENT_SOURCE_DIR}/src/LuaCompletion.cpp")
target_link_libraries(CramionCompletionTests PRIVATE Cramion::Core)

# Prueba de la edicion multiple del Inspector (consola).
add_executable(CramionMultiEditTests "${CMAKE_CURRENT_SOURCE_DIR}/tests/MultiEditTests.cpp"
                                     "${CMAKE_CURRENT_SOURCE_DIR}/src/PropertyInspector.cpp"
                                     "${CMAKE_CURRENT_SOURCE_DIR}/src/EditorTheme.cpp")
target_include_directories(CramionMultiEditTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(CramionMultiEditTests PRIVATE Cramion::Core cramion_imgui)

# Prueba de las plantillas de proyecto (consola): crea, abre y juega cada una.
add_executable(CramionTemplateTests "${CMAKE_CURRENT_SOURCE_DIR}/tests/TemplateTests.cpp"
                                    "${CMAKE_CURRENT_SOURCE_DIR}/src/ProjectTemplates.cpp"
                                    "${CMAKE_CURRENT_SOURCE_DIR}/src/CreatureModels.cpp"
                                    "${CMAKE_CURRENT_SOURCE_DIR}/src/LocomotionPack.cpp")
target_include_directories(CramionTemplateTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(CramionTemplateTests PRIVATE Cramion::Core cramion_update)
# Las plantillas se juegan con sus scripts de C++ compilados de verdad.
target_compile_definitions(CramionTemplateTests PRIVATE
    CRAMION_SDK_DIR="${CMAKE_SOURCE_DIR}/CramionCore/sdk" CRAMION_BIN_DIR="${CMAKE_BINARY_DIR}")

# Prueba de las configuraciones de compilacion (consola): perfiles, game.ini
# y el icono escrito dentro de una copia del reproductor.
add_executable(CramionBuildConfigTests "${CMAKE_CURRENT_SOURCE_DIR}/tests/BuildConfigTests.cpp"
                                       "${CMAKE_CURRENT_SOURCE_DIR}/src/BuildConfig.cpp")
target_include_directories(CramionBuildConfigTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(CramionBuildConfigTests PRIVATE Cramion::Core shell32)

# Prueba de la exportacion a Android (consola): controles tactiles, ajustes,
# manifiesto y un APK real si hay Android SDK.
add_executable(CramionAndroidBuildTests "${CMAKE_CURRENT_SOURCE_DIR}/tests/AndroidBuildTests.cpp"
                                        "${CMAKE_CURRENT_SOURCE_DIR}/src/AndroidBuild.cpp"
                                        "${CMAKE_CURRENT_SOURCE_DIR}/src/BuildConfig.cpp")
target_include_directories(CramionAndroidBuildTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(CramionAndroidBuildTests PRIVATE Cramion::Core shell32)

# --- CramionServer: servidor dedicado sin ventana ni GPU (consola) ---
# Simula la escena (fisica, navegacion, scripts) a un ritmo fijo y abre la
# partida en red. Se copia junto al juego al exportar con "Servidor dedicado".
add_executable(CramionServer "${CMAKE_CURRENT_SOURCE_DIR}/server/ServerMain.cpp")
target_link_libraries(CramionServer PRIVATE Cramion::Core)
set_target_properties(CramionServer PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# --- CramionPlayer: el juego exportado (Archivo > Exportar juego) ---
# Banner del motor al abrir, la escena inicial y todos los sistemas del
# juego (sin el editor). Sin consola.
add_executable(CramionPlayer
    "${CMAKE_CURRENT_SOURCE_DIR}/player/PlayerMain.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/GraphicsConfig.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ImGuiLayer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/UiRenderer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/LoadingScreen.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ProfilerOverlay.cpp"
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

# --- CramionMcp.exe: puente MCP por stdio (Claude Desktop y otros) hacia el ---
# servidor MCP del editor (http://127.0.0.1:7777/mcp).
add_executable(CramionMcp "${CMAKE_CURRENT_SOURCE_DIR}/mcp/McpBridge.cpp")
target_link_libraries(CramionMcp PRIVATE ws2_32)
set_target_properties(CramionMcp PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
add_dependencies(CramionEditor CramionMcp)

# --- CramionScriptHost.exe: el proceso aislado de los scripts de C++ ---
# Carga la DLL de los scripts del juego y ejecuta sus eventos; si un script
# falla, cae este proceso y no el motor (ver CramionCore/scripting/CppScripts.h).
# Sin dependencias del motor: solo las cabeceras del canal y del SDK.
add_executable(CramionScriptHost "${CMAKE_CURRENT_SOURCE_DIR}/host/ScriptHost.cpp")
target_include_directories(CramionScriptHost PRIVATE "${CMAKE_SOURCE_DIR}/CramionCore/include" "${CMAKE_SOURCE_DIR}/CramionCore/sdk")
target_link_libraries(CramionScriptHost PRIVATE dbghelp)
target_compile_options(CramionScriptHost PRIVATE -Wall -Wextra)
set_target_properties(CramionScriptHost PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
# El SDK (cabeceras y el arranque de la DLL) junto a los ejecutables, en sdk/:
# el editor compila con el y la exportacion lo usa.
# (en cada compilacion: un cambio solo en una cabecera del SDK tambien llega).
add_custom_target(cramion_sdk_copy ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
            "${CMAKE_SOURCE_DIR}/CramionCore/sdk" "${CMAKE_BINARY_DIR}/sdk"
    COMMENT "SDK de los scripts de C++")
add_dependencies(cramion_sdk_copy cramion_sdkgen)
add_dependencies(CramionScriptHost cramion_sdk_copy)
add_dependencies(CramionEditor CramionScriptHost)
add_dependencies(CramionPlayer CramionScriptHost)
add_dependencies(CramionCppScriptTests CramionScriptHost)
add_dependencies(CramionTemplateTests CramionScriptHost)
add_dependencies(cramion_minigolf CramionScriptHost)

# --- Compilador incluido para los scripts de C++ y su IntelliSense ---
# llvm-mingw (LLVM/clang + lld + libc++ + cabeceras de mingw-w64, todo
# redistribuible): clang++ compila la DLL de los scripts sin Visual Studio ni
# LLVM instalados y clangd da el IntelliSense del editor de codigo. Se baja una
# vez a %LOCALAPPDATA%/Cramion/downloads (compartido entre build y
# build-release) y se copia lo justo (x86_64) a <bin>/toolchain.
option(CRAMION_BUNDLE_TOOLCHAIN "Incluir clang/clangd (llvm-mingw) para los scripts de C++" ON)
if(CRAMION_BUNDLE_TOOLCHAIN)
    set(CRAMION_TOOLCHAIN_VERSION 20260922)
    set(_tc_name "llvm-mingw-${CRAMION_TOOLCHAIN_VERSION}-ucrt-x86_64")
    set(_tc_cache "$ENV{LOCALAPPDATA}/Cramion/downloads")
    set(_tc_zip "${_tc_cache}/${_tc_name}.zip")
    set(_tc_dir "${CMAKE_BINARY_DIR}/toolchain")
    set(_tc_stamp "${_tc_dir}/.cramion-${CRAMION_TOOLCHAIN_VERSION}-2")  # -2: con clang-format
    if(NOT EXISTS "${_tc_stamp}")
        if(NOT EXISTS "${_tc_zip}")
            message(STATUS "Descargando ${_tc_name} (unos 190 MB, una sola vez)...")
            file(DOWNLOAD
                "https://github.com/mstorsjo/llvm-mingw/releases/download/${CRAMION_TOOLCHAIN_VERSION}/${_tc_name}.zip"
                "${_tc_zip}.part"
                EXPECTED_HASH SHA256=e3ad77d117a4bea19a7a3b333341824d79a5a371004a10e25b8504e7b3047666
                STATUS _tc_status)
            list(GET _tc_status 0 _tc_code)
            if(_tc_code EQUAL 0)
                file(RENAME "${_tc_zip}.part" "${_tc_zip}")
            else()
                file(REMOVE "${_tc_zip}.part")
                message(WARNING "No se pudo descargar llvm-mingw (${_tc_status}): los scripts de C++ usaran el clang o Visual Studio del sistema.")
            endif()
        endif()
        if(EXISTS "${_tc_zip}")
            message(STATUS "Preparando el compilador de los scripts de C++ en ${_tc_dir}...")
            file(REMOVE_RECURSE "${_tc_dir}" "${CMAKE_BINARY_DIR}/_toolchain_extract")
            file(ARCHIVE_EXTRACT INPUT "${_tc_zip}" DESTINATION "${CMAKE_BINARY_DIR}/_toolchain_extract"
                PATTERNS
                    "${_tc_name}/LICENSE.TXT"
                    "${_tc_name}/bin/clang++.exe" "${_tc_name}/bin/clang.exe" "${_tc_name}/bin/clang-[0-9]*.exe"
                    "${_tc_name}/bin/x86_64-w64-mingw32-clang++.exe" "${_tc_name}/bin/ld.lld.exe" "${_tc_name}/bin/clangd.exe"
                    "${_tc_name}/bin/clang-format.exe"
                    "${_tc_name}/bin/libLLVM-*.dll" "${_tc_name}/bin/libclang-cpp.dll" "${_tc_name}/bin/libc++.dll"
                    "${_tc_name}/bin/libunwind.dll" "${_tc_name}/bin/libwinpthread-1.dll"
                    "${_tc_name}/bin/x86_64-*.cfg" "${_tc_name}/bin/mingw32-common.cfg"
                    "${_tc_name}/lib/clang/*/include/*" "${_tc_name}/lib/clang/*/lib/windows/libclang_rt.builtins-x86_64.a"
                    "${_tc_name}/include/*" "${_tc_name}/x86_64-w64-mingw32/lib/*")
            file(RENAME "${CMAKE_BINARY_DIR}/_toolchain_extract/${_tc_name}" "${_tc_dir}" RESULT _tc_rename)
            file(REMOVE_RECURSE "${CMAKE_BINARY_DIR}/_toolchain_extract")
            if(_tc_rename EQUAL 0)
                file(WRITE "${_tc_stamp}" "${_tc_name}\n")
            else()
                # Un editor abierto tiene clangd.exe en uso: se reintenta en la proxima configuracion.
                message(WARNING "No se pudo preparar ${_tc_dir} (${_tc_rename}): cierra el editor (clangd en uso) y vuelve a compilar.")
            endif()
        endif()
    endif()
endif()

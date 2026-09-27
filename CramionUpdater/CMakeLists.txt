# CramionUpdater: el actualizador de Cramion.
#
#   cramion_update       libreria comun (buscar la version nueva en GitHub,
#                        descargar, descomprimir el zip, instalar con vuelta
#                        atras, guardar y cerrar los editores abiertos). La usan
#                        el actualizador y el editor (Hub y aviso).
#   CramionUpdater.exe   aplicacion aparte con su propia interfaz (Win32 +
#                        Direct3D 11 + Dear ImGui: no depende de Vulkan).
#   CramionUpdateTests   prueba de consola (versiones, feed, zip, instalacion).
#
# Se anade despues de CramionEditor: reutiliza las fuentes de Dear ImGui y stb
# que descargan el editor y CramionFX.

include(FetchContent)
FetchContent_GetProperties(imgui)
FetchContent_GetProperties(stb)

add_library(cramion_update STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Update.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Inflate.cpp")
target_include_directories(cramion_update PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(cramion_update PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(cramion_update SYSTEM PRIVATE "${stb_SOURCE_DIR}")
target_link_libraries(cramion_update PUBLIC nlohmann_json::nlohmann_json winhttp shell32 ole32)
target_compile_definitions(cramion_update PRIVATE CRAMION_VERSION_STRING="${PROJECT_VERSION}")
target_compile_options(cramion_update PRIVATE -Wall -Wextra)
# stb (Inflate.cpp) sin avisos.
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/Inflate.cpp" PROPERTIES COMPILE_OPTIONS "-w")
add_library(Cramion::Update ALIAS cramion_update)

# Dear ImGui con los backends de Win32 y Direct3D 11 (sin Vulkan).
add_library(cramion_imgui_dx11 STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp")
target_include_directories(cramion_imgui_dx11 SYSTEM PUBLIC "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends")
target_link_libraries(cramion_imgui_dx11 PUBLIC d3d11 dxgi d3dcompiler dwmapi imm32)
target_compile_options(cramion_imgui_dx11 PRIVATE -O2 -w)

add_executable(CramionUpdater "${CMAKE_CURRENT_SOURCE_DIR}/src/UpdaterMain.cpp" "${CRAMION_ICON_RC}")
target_include_directories(CramionUpdater PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(CramionUpdater PRIVATE cramion_update cramion_imgui_dx11)
set_target_properties(CramionUpdater PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}" WIN32_EXECUTABLE TRUE)
target_link_options(CramionUpdater PRIVATE "-Xlinker" "/entry:mainCRTStartup")
target_compile_options(CramionUpdater PRIVATE -Wall -Wextra)
add_dependencies(CramionEditor CramionUpdater)

# Prueba de consola. El zip de prueba se crea al configurar.
set(_update_fixture "${CMAKE_CURRENT_BINARY_DIR}/fixture")
file(REMOVE_RECURSE "${_update_fixture}")
file(WRITE "${_update_fixture}/pkg/Cramion-9.9.9-win64/CramionEditor.exe" "editor nuevo")
file(WRITE "${_update_fixture}/pkg/Cramion-9.9.9-win64/CramionUpdater.exe" "actualizador nuevo")
file(WRITE "${_update_fixture}/pkg/Cramion-9.9.9-win64/shaders/mesh.vert.spv" "shader nuevo")
file(WRITE "${_update_fixture}/pkg/Cramion-9.9.9-win64/docs/manual/index.html" "<html>manual nuevo</html>")
string(REPEAT "Cramion comprime bien este texto repetido. " 400 _update_big)
file(WRITE "${_update_fixture}/pkg/Cramion-9.9.9-win64/README.md" "${_update_big}")
# Como el paquete de verdad (cmake/PackageRelease.cmake): carpeta de primer nivel.
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar cf "${_update_fixture}/Cramion-win64.zip" --format=zip "Cramion-9.9.9-win64"
    WORKING_DIRECTORY "${_update_fixture}/pkg")
add_executable(CramionUpdateTests "${CMAKE_CURRENT_SOURCE_DIR}/tests/UpdateTests.cpp")
target_link_libraries(CramionUpdateTests PRIVATE cramion_update)
target_compile_definitions(CramionUpdateTests PRIVATE
    CRAMION_UPDATE_FIXTURE="${_update_fixture}"
    CRAMION_VERSION_STRING="${PROJECT_VERSION}")

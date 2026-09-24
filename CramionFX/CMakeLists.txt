# Libreria estatica CramionFX: el renderizador Vulkan del motor.
#
# Uso desde cualquier proyecto CMake:
#
#   add_subdirectory(CramionDM)   # ventana y entrada (lo necesita CramionFX)
#   add_subdirectory(CramionFX)
#   target_link_libraries(mi_juego PRIVATE Cramion::FX)
#   cramionfx_deploy(mi_juego)    # copia los shaders SPIR-V junto al .exe
#
# y en el codigo: #include <CramionFX/CramionFX.h>
#
# Todo lo que necesita va dentro de esta carpeta: fuentes, cabeceras, shaders
# GLSL (se compilan a SPIR-V con la libreria) y las cabeceras de Vulkan
# (vendor/). Las dependencias de terceros (assimp, stb) se descargan al
# configurar y quedan privadas: quien use la libreria no las ve.
#
# Requisitos: Windows, Vulkan SDK (glslc), compilador C++20 (clang).

cmake_minimum_required(VERSION 3.21)

if(NOT TARGET Cramion::DM)
    message(FATAL_ERROR
        "CramionFX necesita CramionDM: anade add_subdirectory(CramionDM) antes que CramionFX.")
endif()

# -----------------------------------------------------------------------------
# Dependencias descargadas en la configuracion
#
#   assimp -> carga de modelos (FBX, glTF) con esqueleto, animaciones y texturas.
#   stb    -> decodificacion de las imagenes (PNG/JPG/TGA/HDR).
# -----------------------------------------------------------------------------

include(FetchContent)

# Solo los importadores que se usan: compilar los ~40 formatos de assimp tarda
# mucho. Sin exportadores, tests ni herramientas.
set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ALL_EXPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_FBX_IMPORTER ON CACHE BOOL "" FORCE)
# glTF 2.0 (.gltf / .glb): el formato de los materiales PBR metal/rugosidad.
set(ASSIMP_BUILD_GLTF_IMPORTER ON CACHE BOOL "" FORCE)
set(ASSIMP_NO_EXPORT ON CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
set(ASSIMP_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(ASSIMP_INJECT_DEBUG_POSTFIX OFF CACHE BOOL "" FORCE)
# El FBX binario guarda arrays comprimidos con zlib: se usa la copia de assimp.
set(ASSIMP_BUILD_ZLIB ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(assimp
    GIT_REPOSITORY https://github.com/assimp/assimp.git
    GIT_TAG v6.0.2
    GIT_SHALLOW TRUE
)

FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG master
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(assimp stb)

# Cabeceros de assimp como SYSTEM: sus avisos no son nuestros.
set_target_properties(assimp PROPERTIES SYSTEM TRUE)

# assimp optimizado tambien en Debug: sin optimizar, importar el FBX de Bistro
# (120 MB) tarda mas de 3 minutos. No hace falta depurar su interior.
target_compile_options(assimp PRIVATE -O2)

# -----------------------------------------------------------------------------
# La libreria
# -----------------------------------------------------------------------------

file(GLOB_RECURSE CRAMIONFX_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
file(GLOB_RECURSE CRAMIONFX_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
)

add_library(CramionFX STATIC ${CRAMIONFX_SOURCES} ${CRAMIONFX_HEADERS})
add_library(Cramion::FX ALIAS CramionFX)

target_compile_features(CramionFX PUBLIC cxx_std_20)

# Publico: #include <CramionFX/...>. Las cabeceras de Vulkan tambien, porque
# las del renderizador las incluyen (SYSTEM: sus avisos no contaminan el
# build de quien use la libreria).
target_include_directories(CramionFX PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_include_directories(CramionFX SYSTEM PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/include"
)
target_include_directories(CramionFX SYSTEM PRIVATE
    "${stb_SOURCE_DIR}"
)

target_link_libraries(CramionFX
    PUBLIC
        Cramion::DM
        "${CMAKE_CURRENT_SOURCE_DIR}/vendor/lib/vulkan-1.lib"
    PRIVATE
        assimp::assimp
)

target_compile_definitions(CramionFX PUBLIC
    VK_USE_PLATFORM_WIN32_KHR
    VULKAN_HPP_HANDLE_ERROR_OUT_OF_DATE_AS_SUCCESS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_compile_options(CramionFX PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

# La carga de escenarios (parsear 1 GB de OBJ, leer la cache, decodificar
# cientos de PNG) se optimiza tambien en Debug: sin optimizar tardaria varias
# veces mas y no aporta nada depurar esos bucles paso a paso.
set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/asset/ObjLoader.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/asset/ModelCache.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/asset/ModelLoader.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/asset/StbImage.cpp"
    PROPERTIES COMPILE_OPTIONS "-O2"
)

# -----------------------------------------------------------------------------
# Shaders: GLSL -> SPIR-V con glslc (viene con el Vulkan SDK)
#
# Se compilan todos los .vert/.frag/.comp de shaders/ al construir la
# libreria. En tiempo de ejecucion el motor los busca en <carpeta del
# ejecutable>/shaders (gfx::shaders::directory()): cramionfx_deploy() los
# copia ahi.
# -----------------------------------------------------------------------------

find_program(GLSLC_EXECUTABLE
    NAMES glslc glslc.exe
    HINTS
        "$ENV{VULKAN_SDK}/Bin"
        "$ENV{VULKAN_SDK}/bin"
    DOC "Compilador de shaders glslc del Vulkan SDK"
)

if(NOT GLSLC_EXECUTABLE)
    message(FATAL_ERROR
        "No se encontro glslc. Instala el Vulkan SDK o define VULKAN_SDK.")
endif()

file(GLOB CRAMIONFX_SHADER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.comp"
)
# Cabeceras que incluyen los shaders (#include): si cambian, se recompilan todos.
file(GLOB CRAMIONFX_SHADER_INCLUDES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/*.glsl"
)

set(CRAMIONFX_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
set(CRAMIONFX_SHADER_BINARIES "")

foreach(SHADER_SOURCE IN LISTS CRAMIONFX_SHADER_SOURCES)
    get_filename_component(SHADER_NAME "${SHADER_SOURCE}" NAME)
    set(SHADER_BINARY "${CRAMIONFX_SHADER_DIR}/${SHADER_NAME}.spv")

    add_custom_command(
        OUTPUT "${SHADER_BINARY}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CRAMIONFX_SHADER_DIR}"
        COMMAND "${GLSLC_EXECUTABLE}" --target-env=vulkan1.3 -O
                "${SHADER_SOURCE}" -o "${SHADER_BINARY}"
        DEPENDS "${SHADER_SOURCE}" ${CRAMIONFX_SHADER_INCLUDES}
        COMMENT "CramionFX: compilando shader ${SHADER_NAME}"
        VERBATIM
    )

    list(APPEND CRAMIONFX_SHADER_BINARIES "${SHADER_BINARY}")
endforeach()

add_custom_target(CramionFX_shaders DEPENDS ${CRAMIONFX_SHADER_BINARIES})
add_dependencies(CramionFX CramionFX_shaders)

# Para cramionfx_deploy(), que se llama desde otro CMakeLists.
set_property(GLOBAL PROPERTY CRAMIONFX_SHADER_DIR "${CRAMIONFX_SHADER_DIR}")

# cramionfx_deploy(<ejecutable>)
#
# Deja los shaders SPIR-V en <carpeta del ejecutable>/shaders, donde el motor
# los busca al arrancar. Se repite en cada build (son pocos KB), asi que un
# shader editado llega siempre aunque el ejecutable no se vuelva a enlazar.
function(cramionfx_deploy target)
    get_property(shader_dir GLOBAL PROPERTY CRAMIONFX_SHADER_DIR)
    add_custom_target(${target}_cramionfx_shaders ALL
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${shader_dir}" "$<TARGET_FILE_DIR:${target}>/shaders"
        DEPENDS CramionFX_shaders
        COMMENT "CramionFX: copiando los shaders junto a ${target}"
        VERBATIM
    )
    add_dependencies(${target} ${target}_cramionfx_shaders)

    # Varios ejecutables en la misma carpeta (cramion y CramionEditor): las
    # copias van una detras de otra, no a la vez sobre los mismos archivos.
    get_property(previous GLOBAL PROPERTY CRAMIONFX_LAST_DEPLOY)
    if(previous)
        add_dependencies(${target}_cramionfx_shaders ${previous})
    endif()
    set_property(GLOBAL PROPERTY CRAMIONFX_LAST_DEPLOY ${target}_cramionfx_shaders)
endfunction()

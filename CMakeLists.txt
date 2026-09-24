cmake_minimum_required(VERSION 3.21)

project(Cramion
    VERSION 0.1.0
    DESCRIPTION "Motor con capa de plataforma DirectX 12 (CramionDM) y renderizador Vulkan"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Capa de plataforma (ventana, entrada, dispositivo DX12).
add_subdirectory(CramionDM)

# -----------------------------------------------------------------------------
# Dependencias descargadas en la configuracion
#
#   assimp -> carga de modelos (FBX) con esqueleto, animaciones y texturas.
#   stb    -> decodificacion de las imagenes (PNG/JPG/TGA), incrustadas en el
#             FBX o sueltas junto a el.
# -----------------------------------------------------------------------------

include(FetchContent)

# Solo el importador de FBX: compilar los ~40 formatos de assimp tarda mucho y
# no se usan. Sin exportadores, tests ni herramientas.
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

# -----------------------------------------------------------------------------
# Fuentes
# -----------------------------------------------------------------------------

file(GLOB_RECURSE VENDOR_CPP CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/lib/*.cpp"
)

file(GLOB_RECURSE VENDOR_LIBS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/lib/*.lib"
)

# src/main.cpp + src/cpp/**/*.cpp
file(GLOB_RECURSE ENGINE_CPP CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

add_executable(cramion
    ${ENGINE_CPP}
    ${VENDOR_CPP}
)

# -----------------------------------------------------------------------------
# Includes
# -----------------------------------------------------------------------------

target_include_directories(cramion PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/include"
)

# SYSTEM para que los avisos de los cabeceros de Vulkan no contaminen el build.
target_include_directories(cramion SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/include"
    "${stb_SOURCE_DIR}"
)

# -----------------------------------------------------------------------------
# Shaders: GLSL -> SPIR-V con glslc (viene con el Vulkan SDK)
#
# Los .spv se generan en <directorio del ejecutable>/shaders, que es donde el
# motor los busca en tiempo de ejecucion (ver gfx::shaders::directory()).
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

set(SHADER_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/lighting.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/lighting.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fxaa.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/ssao.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/ssgi.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/bloom_down.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/bloom_up.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/composite.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/sky_lut.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/light_shafts.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/exposure_histogram.comp"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/exposure_average.comp"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/ibl_prefilter.comp"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/ibl_sh.comp"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/brdf_lut.comp"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/skinned.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/skinned.frag"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/skinned_shadow.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/skinned_shadow.frag"
)

# Junto al ejecutable (Ninja de una sola configuracion: el raiz del build).
# Cabeceras que incluyen los shaders (#include): si cambian, se recompilan todos.
set(SHADER_INCLUDES
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/ibl_common.glsl"
)

set(SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders")
set(SHADER_BINARIES "")

foreach(SHADER_SOURCE IN LISTS SHADER_SOURCES)
    get_filename_component(SHADER_NAME "${SHADER_SOURCE}" NAME)
    set(SHADER_BINARY "${SHADER_OUTPUT_DIR}/${SHADER_NAME}.spv")

    add_custom_command(
        OUTPUT "${SHADER_BINARY}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SHADER_OUTPUT_DIR}"
        COMMAND "${GLSLC_EXECUTABLE}" --target-env=vulkan1.3 -O
                "${SHADER_SOURCE}" -o "${SHADER_BINARY}"
        DEPENDS "${SHADER_SOURCE}" ${SHADER_INCLUDES}
        COMMENT "Compilando shader ${SHADER_NAME}"
        VERBATIM
    )

    list(APPEND SHADER_BINARIES "${SHADER_BINARY}")
endforeach()

add_custom_target(shaders DEPENDS ${SHADER_BINARIES})
add_dependencies(cramion shaders)

# -----------------------------------------------------------------------------
# Enlazado y opciones
# -----------------------------------------------------------------------------

target_link_libraries(cramion PRIVATE
    Cramion::DM
    assimp::assimp
    ${VENDOR_LIBS}
)

# La carga de escenarios (parsear 1 GB de OBJ, leer la cache, decodificar
# cientos de PNG) se optimiza tambien en Debug: sin optimizar tardaria varias
# veces mas y no aporta nada depurar esos bucles paso a paso.
set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/asset/ObjLoader.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/asset/ModelCache.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/asset/ModelLoader.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/cpp/asset/StbImage.cpp"
    PROPERTIES COMPILE_OPTIONS "-O2"
)

# -----------------------------------------------------------------------------
# Escenario: San Miguel
#
# Se extrae de San_Miguel.zip una sola vez, al configurar, en assets/ junto al
# ejecutable, que es donde el motor lo busca. Solo la version completa (10 M de
# triangulos); la "low-poly" se puede anadir a PATTERNS y elegir en
# src/main.cpp.
#
# El OBJ no se lee con assimp sino con el lector propio en paralelo
# (asset/ObjLoader), y la primera carga deja al lado una cache binaria
# (san-miguel.obj.cramcache) para que las siguientes tarden segundos.
# -----------------------------------------------------------------------------

set(SAN_MIGUEL_ZIP "${CMAKE_CURRENT_SOURCE_DIR}/San_Miguel.zip")
set(SAN_MIGUEL_DIR "${CMAKE_BINARY_DIR}/assets/san-miguel")

if(EXISTS "${SAN_MIGUEL_ZIP}" AND NOT EXISTS "${SAN_MIGUEL_DIR}/san-miguel.obj")
    message(STATUS "Extrayendo San_Miguel.zip en ${SAN_MIGUEL_DIR} (una sola vez)...")
    file(ARCHIVE_EXTRACT
        INPUT "${SAN_MIGUEL_ZIP}"
        DESTINATION "${SAN_MIGUEL_DIR}"
        PATTERNS "san-miguel.obj" "san-miguel.mtl" "textures/*" "license.txt"
    )
elseif(NOT EXISTS "${SAN_MIGUEL_DIR}/san-miguel.obj")
    message(WARNING "No se encontro San_Miguel.zip ni ${SAN_MIGUEL_DIR}/san-miguel.obj")
endif()

target_compile_definitions(cramion PRIVATE
    VK_USE_PLATFORM_WIN32_KHR
    VULKAN_HPP_HANDLE_ERROR_OUT_OF_DATE_AS_SUCCESS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_compile_options(cramion PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

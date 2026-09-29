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

# meshoptimizer (MIT): simplificacion de mallas para los LODs automaticos.
FetchContent_Declare(meshoptimizer
    GIT_REPOSITORY https://github.com/zeux/meshoptimizer.git
    GIT_TAG v1.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(meshoptimizer)
set_target_properties(meshoptimizer PROPERTIES SYSTEM TRUE)
# Optimizado tambien en Debug: simplificar un escaneo de 2 M de triangulos sin
# optimizar tarda decenas de segundos.
target_compile_options(meshoptimizer PRIVATE -O2)

# AMD FidelityFX Super Resolution 1 (MIT): solo las cabeceras de shader
# (ffx_a.h, ffx_fsr1.h), que incluyen fsr_easu.frag y fsr_rcas.frag.
FetchContent_Declare(fidelityfx_fsr1
    GIT_REPOSITORY https://github.com/GPUOpen-Effects/FidelityFX-FSR.git
    GIT_TAG master
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR ffx-fsr
)
FetchContent_MakeAvailable(fidelityfx_fsr1)

# OpenXR (realidad virtual: SteamVR, Oculus/Meta, WMR, Pico...): el loader
# oficial de Khronos, estatico, sin capas ni pruebas. Solo en PC.
option(CRAMION_XR "Realidad virtual con OpenXR" ON)
if(CRAMION_XR AND NOT ANDROID)
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
    set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_LOADER ON CACHE BOOL "" FORCE)
    set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
    set(BUILD_WITH_SYSTEM_JSONCPP OFF CACHE BOOL "" FORCE)
    set(INSTALL_TO_ARCHITECTURE_PREFIXES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(openxr
        GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
        GIT_TAG release-1.1.63
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(openxr)
    target_compile_options(openxr_loader PRIVATE -w)
endif()

# --- Escaladores temporales: AMD FSR 3.1 y NVIDIA DLSS 4 ---
# Solo los archivos que hacen falta de cada SDK (cabeceras, DLL firmadas y
# licencias), no los repositorios enteros. Se descargan una vez en _deps.
#   FSR 3.1: FidelityFX SDK 1.1.4 (la ultima con Vulkan), API "ffx" con la DLL
#            firmada amd_fidelityfx_vk.dll. Se carga en tiempo de ejecucion.
#   DLSS 4:  DLSS SDK 310.9 (NGX para Vulkan; modelo transformer). Enlaza la
#            lib estatica de NGX, que carga nvngx_dlss.dll junto al .exe.
option(CRAMION_FSR3 "AMD FSR 3.1 (FidelityFX)" ON)
option(CRAMION_DLSS "NVIDIA DLSS 4 (NGX)" ON)
function(cramion_fetch_files ok_var dest base)
    set(ok TRUE)
    foreach(rel IN LISTS ARGN)
        if(NOT EXISTS "${dest}/${rel}")
            message(STATUS "Descargando ${rel}")
            file(DOWNLOAD "${base}/${rel}" "${dest}/${rel}.part" STATUS status TLS_VERIFY ON)
            list(GET status 0 code)
            if(code EQUAL 0)
                file(RENAME "${dest}/${rel}.part" "${dest}/${rel}")
            else()
                file(REMOVE "${dest}/${rel}.part")
                message(WARNING "No se pudo descargar ${base}/${rel}: ${status}")
                set(ok FALSE)
            endif()
        endif()
    endforeach()
    set(${ok_var} ${ok} PARENT_SCOPE)
endfunction()

set(CRAMION_FSR3_READY FALSE)
set(CRAMION_DLSS_READY FALSE)
if(WIN32 AND NOT ANDROID)
    if(CRAMION_FSR3)
        set(ffx_dir "${CMAKE_BINARY_DIR}/_deps/fidelityfx")
        cramion_fetch_files(ffx_ok "${ffx_dir}"
            "https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/raw/v1.1.4"
            ffx-api/include/ffx_api/ffx_api.h
            ffx-api/include/ffx_api/ffx_api_types.h
            ffx-api/include/ffx_api/ffx_api_loader.h
            ffx-api/include/ffx_api/ffx_upscale.h
            ffx-api/include/ffx_api/ffx_framegeneration.h
            ffx-api/include/ffx_api/vk/ffx_api_vk.h
            PrebuiltSignedDLL/amd_fidelityfx_vk.dll
            LICENSE.txt)
        if(ffx_ok)
            set(CRAMION_FSR3_READY TRUE)
            set_property(GLOBAL PROPERTY CRAMIONFX_FSR3_DLL "${ffx_dir}/PrebuiltSignedDLL/amd_fidelityfx_vk.dll")
            set_property(GLOBAL PROPERTY CRAMIONFX_FSR3_LICENSE "${ffx_dir}/LICENSE.txt")
        endif()
    endif()
    if(CRAMION_DLSS)
        set(dlss_dir "${CMAKE_BINARY_DIR}/_deps/dlss")
        set(dlss_headers nvsdk_ngx.h nvsdk_ngx_defs.h nvsdk_ngx_defs_dlssd.h nvsdk_ngx_defs_dlssg.h nvsdk_ngx_defs_vk.h
            nvsdk_ngx_helpers.h nvsdk_ngx_helpers_vk.h nvsdk_ngx_helpers_dlssd.h nvsdk_ngx_helpers_dlssd_vk.h
            nvsdk_ngx_helpers_dlssg.h nvsdk_ngx_helpers_dlssg_vk.h nvsdk_ngx_params.h nvsdk_ngx_params_dlssd.h
            nvsdk_ngx_params_dlssg.h nvsdk_ngx_vk.h)
        list(TRANSFORM dlss_headers PREPEND "include/")
        cramion_fetch_files(dlss_ok "${dlss_dir}" "https://github.com/NVIDIA/DLSS/raw/v310.9.1"
            ${dlss_headers}
            lib/Windows_x86_64/x64/nvsdk_ngx_d.lib
            lib/Windows_x86_64/x64/nvsdk_ngx_d_dbg.lib
            lib/Windows_x86_64/rel/nvngx_dlss.dll
            LICENSE.txt)
        if(dlss_ok)
            set(CRAMION_DLSS_READY TRUE)
            set_property(GLOBAL PROPERTY CRAMIONFX_DLSS_DLL "${dlss_dir}/lib/Windows_x86_64/rel/nvngx_dlss.dll")
            set_property(GLOBAL PROPERTY CRAMIONFX_DLSS_LICENSE "${dlss_dir}/LICENSE.txt")
        endif()
    endif()
endif()

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

if(ANDROID)
    # libvulkan.so del sistema (NDK).
    set(CRAMIONFX_VULKAN_LIB vulkan)
else()
    set(CRAMIONFX_VULKAN_LIB "${CMAKE_CURRENT_SOURCE_DIR}/vendor/lib/vulkan-1.lib")
endif()
target_link_libraries(CramionFX
    PUBLIC
        Cramion::DM
        ${CRAMIONFX_VULKAN_LIB}
    PRIVATE
        assimp::assimp
        meshoptimizer
)
if(CRAMION_FSR3_READY)
    target_include_directories(CramionFX SYSTEM PRIVATE "${CMAKE_BINARY_DIR}/_deps/fidelityfx/ffx-api/include")
    target_compile_definitions(CramionFX PUBLIC CRAMION_FSR3=1)
endif()
if(CRAMION_DLSS_READY)
    set(dlss_lib_dir "${CMAKE_BINARY_DIR}/_deps/dlss/lib/Windows_x86_64/x64")
    target_include_directories(CramionFX SYSTEM PRIVATE "${CMAKE_BINARY_DIR}/_deps/dlss/include")
    target_link_libraries(CramionFX PRIVATE
        "$<IF:$<CONFIG:Debug>,${dlss_lib_dir}/nvsdk_ngx_d_dbg.lib,${dlss_lib_dir}/nvsdk_ngx_d.lib>")
    target_compile_definitions(CramionFX PUBLIC CRAMION_DLSS=1)
endif()
if(TARGET openxr_loader)
    target_link_libraries(CramionFX PRIVATE openxr_loader)
    target_include_directories(CramionFX SYSTEM PRIVATE "${openxr_SOURCE_DIR}/include" "${openxr_BINARY_DIR}/include")
    target_compile_definitions(CramionFX PUBLIC CRAMION_XR=1)
endif()

if(ANDROID)
    target_compile_definitions(CramionFX PUBLIC
        VK_USE_PLATFORM_ANDROID_KHR
        VULKAN_HPP_HANDLE_ERROR_OUT_OF_DATE_AS_SUCCESS
    )
else()
    target_compile_definitions(CramionFX PUBLIC
        VK_USE_PLATFORM_WIN32_KHR
        VULKAN_HPP_HANDLE_ERROR_OUT_OF_DATE_AS_SUCCESS
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
endif()

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
                -I "${fidelityfx_fsr1_SOURCE_DIR}/ffx-fsr"
                "${SHADER_SOURCE}" -o "${SHADER_BINARY}"
        DEPENDS "${SHADER_SOURCE}" ${CRAMIONFX_SHADER_INCLUDES}
        COMMENT "CramionFX: compilando shader ${SHADER_NAME}"
        VERBATIM
    )

    list(APPEND CRAMIONFX_SHADER_BINARIES "${SHADER_BINARY}")
endforeach()

# Fuentes de los shaders de superficie del usuario (.crshader): el motor
# junta en tiempo de ejecucion surface.vert/.frag con el codigo del usuario y
# sus includes (shaders/source), y los compila con shaderc_shared.dll.
set(CRAMIONFX_SURFACE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/surface.vert"
    "${CMAKE_CURRENT_SOURCE_DIR}/shaders/surface.frag"
    ${CRAMIONFX_SHADER_INCLUDES})
set(CRAMIONFX_SURFACE_STAMP "${CRAMIONFX_SHADER_DIR}/source/.copiado")
add_custom_command(
    OUTPUT "${CRAMIONFX_SURFACE_STAMP}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CRAMIONFX_SHADER_DIR}/source"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${CRAMIONFX_SURFACE_SOURCES} "${CRAMIONFX_SHADER_DIR}/source"
    COMMAND "${CMAKE_COMMAND}" -E touch "${CRAMIONFX_SURFACE_STAMP}"
    DEPENDS ${CRAMIONFX_SURFACE_SOURCES}
    COMMENT "CramionFX: fuentes de los shaders de superficie"
    VERBATIM
)
list(APPEND CRAMIONFX_SHADER_BINARIES "${CRAMIONFX_SURFACE_STAMP}")

# El compilador de GLSL en tiempo de ejecucion (Vulkan SDK). Se copia junto a
# cada ejecutable que usa cramionfx_deploy() y viaja con el paquete y los
# juegos exportados.
find_file(CRAMIONFX_SHADERC_DLL shaderc_shared.dll
    HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin"
    DOC "shaderc_shared.dll del Vulkan SDK (shaders de superficie del usuario)")
if(NOT CRAMIONFX_SHADERC_DLL)
    message(WARNING "No se encontro shaderc_shared.dll: los shaders propios (.crshader) no compilaran")
endif()
set_property(GLOBAL PROPERTY CRAMIONFX_SHADERC_DLL "${CRAMIONFX_SHADERC_DLL}")

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

    # Los escaladores (FSR 3.1 y DLSS 4) junto al ejecutable.
    foreach(prop CRAMIONFX_FSR3_DLL CRAMIONFX_DLSS_DLL)
        get_property(sdk_dll GLOBAL PROPERTY ${prop})
        if(sdk_dll)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${sdk_dll}" "$<TARGET_FILE_DIR:${target}>"
                VERBATIM)
        endif()
    endforeach()

    get_property(shaderc_dll GLOBAL PROPERTY CRAMIONFX_SHADERC_DLL)
    if(shaderc_dll)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${shaderc_dll}" "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    endif()

    # Varios ejecutables en la misma carpeta (cramion y CramionEditor): las
    # copias van una detras de otra, no a la vez sobre los mismos archivos.
    get_property(previous GLOBAL PROPERTY CRAMIONFX_LAST_DEPLOY)
    if(previous)
        add_dependencies(${target}_cramionfx_shaders ${previous})
    endif()
    set_property(GLOBAL PROPERTY CRAMIONFX_LAST_DEPLOY ${target}_cramionfx_shaders)
endfunction()

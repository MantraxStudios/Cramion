cmake_minimum_required(VERSION 3.21)

project(Cramion
    VERSION 0.1.0
    DESCRIPTION "Motor con capa de plataforma DirectX 12 (CramionDM) y renderizador Vulkan (CramionFX)"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Capa de plataforma (ventana, entrada, dispositivo DX12).
add_subdirectory(CramionDM)

# Renderizador Vulkan (libreria): todo el motor grafico y sus shaders.
add_subdirectory(CramionFX)

# -----------------------------------------------------------------------------
# Aplicacion de ejemplo: carga un escenario y lo recorre con la camara.
# -----------------------------------------------------------------------------

add_executable(cramion src/main.cpp)
target_link_libraries(cramion PRIVATE Cramion::FX)
# Shaders SPIR-V en build/shaders, junto a cramion.exe.
cramionfx_deploy(cramion)

target_compile_options(cramion PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

# -----------------------------------------------------------------------------
# Escenarios
#
# Cada zip de la raiz del proyecto se extrae una sola vez, al configurar, en
# assets/<nombre>/ junto al ejecutable, que es donde el motor los busca. La
# escena se elige al ejecutar (cramion.exe <nombre>; ver src/main.cpp).
#
# Los OBJ no se leen con assimp sino con el lector propio en paralelo
# (asset/ObjLoader), y la primera carga deja al lado una cache binaria
# (<modelo>.obj.cramcache) para que las siguientes tarden segundos.
# -----------------------------------------------------------------------------

# cramion_extract_scene(<zip> <carpeta> <archivo que indica que ya esta>
#                       [PATTERNS ...])
function(cramion_extract_scene zip_name folder marker)
    set(zip "${CMAKE_CURRENT_SOURCE_DIR}/${zip_name}")
    set(destination "${CMAKE_BINARY_DIR}/assets/${folder}")
    if(EXISTS "${destination}/${marker}")
        return()
    endif()
    if(NOT EXISTS "${zip}")
        message(STATUS "Escena ${folder}: no se encontro ${zip_name} (opcional)")
        return()
    endif()

    message(STATUS "Extrayendo ${zip_name} en ${destination} (una sola vez)...")
    cmake_parse_arguments(SCENE "" "" "PATTERNS" ${ARGN})
    if(SCENE_PATTERNS)
        file(ARCHIVE_EXTRACT INPUT "${zip}" DESTINATION "${destination}"
             PATTERNS ${SCENE_PATTERNS})
    else()
        file(ARCHIVE_EXTRACT INPUT "${zip}" DESTINATION "${destination}")
    endif()
endfunction()

# Catedral de Sibenik: 75 mil triangulos.
cramion_extract_scene(sibenik.zip sibenik sibenik.obj)

# Amazon Lumberyard Bistro v5.2 (exterior 2.8 M de triangulos, interior 1 M):
# FBX con texturas DDS comprimidas.
cramion_extract_scene(Bistro_v5_2.zip bistro Bistro_v5_2/BistroExterior.fbx)

# San Miguel: solo la version completa (10 M de triangulos); la "low-poly" se
# puede anadir a PATTERNS y registrar en src/main.cpp.
cramion_extract_scene(San_Miguel.zip san-miguel san-miguel.obj
    PATTERNS "san-miguel.obj" "san-miguel.mtl" "textures/*" "license.txt")

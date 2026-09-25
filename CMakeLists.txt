cmake_minimum_required(VERSION 3.21)

project(Cramion
    VERSION 0.1.0
    DESCRIPTION "Motor con capa de plataforma DirectX 12 (CramionDM) y renderizador Vulkan (CramionFX)"
    LANGUAGES CXX
)

# Recursos de Windows (el icono de los .exe: assets/icon.ico).
enable_language(RC)
# C: Lua y miniaudio (scripting y audio de CramionCore).
enable_language(C)
set(CRAMION_ICON_RC "${CMAKE_CURRENT_SOURCE_DIR}/assets/cramion.rc")

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

add_executable(cramion src/main.cpp "${CRAMION_ICON_RC}")
target_link_libraries(cramion PRIVATE Cramion::FX)
# Shaders SPIR-V en build/shaders, junto a cramion.exe.
cramionfx_deploy(cramion)

target_compile_options(cramion PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

# Capa de juego: ECS, componentes, assets y proyectos.
add_subdirectory(CramionCore)

# Editor de escenas (Dear ImGui) sobre la misma libreria.
add_subdirectory(CramionEditor)

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
# CRAMION_EXTRACT_SCENES=OFF: no extraer los zips (builds auxiliares).
option(CRAMION_EXTRACT_SCENES "Extraer los zips de escenas en assets/" ON)

function(cramion_extract_scene zip_name folder marker)
    if(NOT CRAMION_EXTRACT_SCENES)
        return()
    endif()
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

# Coche deportivo (OBJ de 3ds Max, sin texturas). No trae suelo: se genera
# un plano de hormigon de 60 x 60 m en y = 0, donde apoyan las ruedas, para
# que tenga sombra y reflejo (ver "sportscar" en src/main.cpp).
cramion_extract_scene(sportsCar.zip sportscar sportsCar.obj)
set(SPORTSCAR_DIR "${CMAKE_BINARY_DIR}/assets/sportscar")
if(EXISTS "${SPORTSCAR_DIR}/sportsCar.obj" AND NOT EXISTS "${SPORTSCAR_DIR}/ground.obj")
    # Ns 76 -> rugosidad ~0.4 (conversion de Phong de ObjLoader): hormigon
    # alisado, con un reflejo suave del coche.
    file(WRITE "${SPORTSCAR_DIR}/ground.mtl"
        "newmtl Concrete\nKd 0.25 0.25 0.24\nKs 0.04 0.04 0.04\nNs 76\nd 1\nillum 2\n")
    file(WRITE "${SPORTSCAR_DIR}/ground.obj"
        "mtllib ground.mtl\n"
        "v -30 0 -30\nv -30 0 30\nv 30 0 30\nv 30 0 -30\n"
        "vt 0 0\nvt 0 30\nvt 30 30\nvt 30 0\n"
        "vn 0 1 0\n"
        "usemtl Concrete\n"
        "f 1/1/1 2/2/1 3/3/1\nf 1/1/1 3/3/1 4/4/1\n")
endif()

# San Miguel: solo la version completa (10 M de triangulos); la "low-poly" se
# puede anadir a PATTERNS y registrar en src/main.cpp.
cramion_extract_scene(San_Miguel.zip san-miguel san-miguel.obj
    PATTERNS "san-miguel.obj" "san-miguel.mtl" "textures/*" "license.txt")

# CramionCore: la capa de juego sobre CramionFX, como el "runtime" de Unity.
#
#   CramionAssets (Cramion::Assets)  proyectos, assets .crdata con UUID,
#                                    importador y base de datos de assets
#   CramionCore   (Cramion::Core)    ECS (EnTT), componentes (Transform,
#                                    MeshRenderer, Animator, Light, Camera,
#                                    PostProcessing...), escenas .crscene, el
#                                    puente con el renderizador y la fisica
#                                    (Jolt Physics: cuerpos, colliders,
#                                    eventos, triggers, raycasts, capas y
#                                    particulas que chocan)
#
# Uso:
#   add_subdirectory(CramionDM)
#   add_subdirectory(CramionFX)
#   add_subdirectory(CramionCore)
#   target_link_libraries(mi_juego PRIVATE Cramion::Core)
#
# Estructura:
#   include/CramionCore/Uuid.h
#   include/CramionCore/asset/     (contrato: AssetTypes, AssetDatabase,
#                                   AssetManager, Importer)
#   include/CramionCore/project/   (contrato: Project)
#   include/CramionCore/...        ECS, componentes, escena, reflexion
#   src/asset/  src/project/       -> CramionAssets
#   src/ (el resto)                -> CramionCore

cmake_minimum_required(VERSION 3.21)

include(FetchContent)

# EnTT: ECS por "sparse sets": cada tipo de componente en su propio pool de
# memoria contigua. Solo cabeceras.
FetchContent_Declare(entt
    GIT_REPOSITORY https://github.com/skypjack/entt.git
    GIT_TAG v3.16.0
    GIT_SHALLOW TRUE
)
# JSON para escenas, proyectos y ajustes (solo cabeceras).
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(entt nlohmann_json)

# Jolt Physics: el motor de fisica (cuerpos rigidos, colliders, sensores,
# consultas). Libreria estatica; su CMake esta en Build/. Sin tests, ejemplos
# ni visor, y sin tocar las opciones de compilacion del resto del proyecto.
set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
set(TARGET_VIEWER OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)
set(CPP_RTTI_ENABLED ON CACHE BOOL "" FORCE)
set(CPP_EXCEPTIONS_ENABLED ON CACHE BOOL "" FORCE)
FetchContent_Declare(JoltPhysics
    GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
    GIT_TAG v5.3.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR Build
)
FetchContent_MakeAvailable(JoltPhysics)
set_target_properties(Jolt PROPERTIES SYSTEM TRUE)
# Optimizado tambien en Debug (como assimp): la simulacion sin optimizar va
# muy lenta y no hace falta depurar su interior.
target_compile_options(Jolt PRIVATE -O2 -w)

# --- Scripting (Lua 5.4 + sol2) y audio (miniaudio) ---------------------------
# Lua: el interprete compilado como biblioteca C (sin el lua.exe). sol2: solo
# cabeceras (el enlace C++ <-> Lua). miniaudio: audio 2D/3D (WAV, MP3, FLAC y
# OGG con stb_vorbis). Sin sus CMakeLists (SOURCE_SUBDIR inexistente).
FetchContent_Declare(lua
    GIT_REPOSITORY https://github.com/lua/lua.git
    GIT_TAG v5.4.8
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR _sin_cmake
)
FetchContent_Declare(sol2
    GIT_REPOSITORY https://github.com/ThePhD/sol2.git
    GIT_TAG v3.5.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR _sin_cmake
)
FetchContent_Declare(miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG 0.11.25
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR _sin_cmake
)
FetchContent_MakeAvailable(lua sol2 miniaudio)

# Recast/Detour: la navegacion (la misma biblioteca que usa Unreal). Recast
# genera la malla, Detour la consulta y DetourCrowd mueve a los agentes.
# Referencias de 64 bits: mundos grandes con muchas baldosas.
set(RECASTNAVIGATION_DEMO OFF CACHE BOOL "" FORCE)
set(RECASTNAVIGATION_TESTS OFF CACHE BOOL "" FORCE)
set(RECASTNAVIGATION_EXAMPLES OFF CACHE BOOL "" FORCE)
set(RECASTNAVIGATION_DT_POLYREF64 ON CACHE BOOL "" FORCE)
FetchContent_Declare(recastnavigation
    GIT_REPOSITORY https://github.com/recastnavigation/recastnavigation.git
    GIT_TAG v1.6.0
    GIT_SHALLOW TRUE
)
# Su CMakeLists pide cmake_minimum_required(3.0), que CMake 4 ya no acepta.
set(CRAMION_SAVED_POLICY_MINIMUM "${CMAKE_POLICY_VERSION_MINIMUM}")
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
FetchContent_MakeAvailable(recastnavigation)
set(CMAKE_POLICY_VERSION_MINIMUM "${CRAMION_SAVED_POLICY_MINIMUM}")
foreach(target Recast Detour DetourCrowd DetourTileCache DebugUtils)
    if(TARGET ${target})
        set_target_properties(${target} PROPERTIES SYSTEM TRUE)
        # Optimizado tambien en Debug (como Jolt): generar baldosas sin
        # optimizar es muchas veces mas lento.
        target_compile_options(${target} PRIVATE -O2 -w)
    endif()
endforeach()

set(CRAMION_LUA_FILES lapi lauxlib lbaselib lcode lcorolib lctype ldblib ldebug ldo ldump lfunc lgc linit
    liolib llex lmathlib lmem loadlib lobject lopcodes loslib lparser lstate lstring lstrlib ltable ltablib
    ltm lundump lutf8lib lvm lzio)
set(CRAMION_LUA_SOURCES "")
foreach(name IN LISTS CRAMION_LUA_FILES)
    list(APPEND CRAMION_LUA_SOURCES "${lua_SOURCE_DIR}/${name}.c")
endforeach()
add_library(cramion_lua STATIC ${CRAMION_LUA_SOURCES})
target_include_directories(cramion_lua PUBLIC "${lua_SOURCE_DIR}")
target_compile_options(cramion_lua PRIVATE -O2 -w)

add_library(cramion_miniaudio STATIC "${CMAKE_CURRENT_SOURCE_DIR}/src/audio/miniaudio_impl.c")
target_include_directories(cramion_miniaudio PUBLIC "${miniaudio_SOURCE_DIR}")
target_compile_options(cramion_miniaudio PRIVATE -O2 -w)

# zstd: compresion de los paquetes .crpack del juego exportado. Solo la
# biblioteca estatica (su CMake esta en build/cmake).
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zstd
    URL https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR build/cmake
)
FetchContent_MakeAvailable(zstd)
target_compile_options(libzstd_static PRIVATE -O2 -w)

# --- CramionAssets -----------------------------------------------------------

file(GLOB_RECURSE CRAMION_ASSETS_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/asset/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/project/*.cpp"
)
add_library(CramionAssets STATIC ${CRAMION_ASSETS_SOURCES})
add_library(Cramion::Assets ALIAS CramionAssets)
target_include_directories(CramionAssets PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(CramionAssets
    PUBLIC Cramion::FX
    PRIVATE nlohmann_json::nlohmann_json shell32 ole32 assimp libzstd_static
)
target_include_directories(CramionAssets PRIVATE "${zstd_SOURCE_DIR}/lib")
target_compile_options(CramionAssets PRIVATE -Wall -Wextra)

# Herramienta de consola de los assets: crea un proyecto temporal, importa
# modelos y un cielo, y los vuelve a cargar (cramion_asset_tool --help).
add_executable(cramion_asset_tool "${CMAKE_CURRENT_SOURCE_DIR}/tools/AssetTool.cpp")
target_link_libraries(cramion_asset_tool PRIVATE Cramion::Assets)
target_compile_options(cramion_asset_tool PRIVATE -Wall -Wextra)

# --- CramionCore -------------------------------------------------------------

file(GLOB_RECURSE CRAMION_CORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
list(FILTER CRAMION_CORE_SOURCES EXCLUDE REGEX "/src/(asset|project)/")

# Los objetos del ECS se compilan aparte (OBJECT) y solo con las cabeceras de
# los assets: asi se pueden compilar aunque CramionAssets este a medias.
add_library(CramionCoreObjects OBJECT ${CRAMION_CORE_SOURCES})
target_include_directories(CramionCoreObjects PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(CramionCoreObjects PUBLIC Cramion::FX EnTT::EnTT nlohmann_json::nlohmann_json
    PRIVATE Jolt cramion_lua cramion_miniaudio RecastNavigation::Recast RecastNavigation::Detour
            RecastNavigation::DetourCrowd)
target_include_directories(CramionCoreObjects PRIVATE "${sol2_SOURCE_DIR}/include")
target_compile_options(CramionCoreObjects PRIVATE -Wall -Wextra)

# Debug agil: lo que se ejecuta en cada frame con muchos objetos (sync del
# render, ECS, fisica, particulas) y el JSON de las escenas y del deshacer se
# optimizan tambien en Debug, como los cargadores de CramionFX. Sin esto, el
# editor en Debug cae a ~40 FPS con unos cientos de objetos. OFF para poder
# depurar esos archivos paso a paso.
option(CRAMION_FAST_DEBUG "Optimizar en Debug el codigo caliente de CramionCore" ON)
if(CRAMION_FAST_DEBUG)
    set_source_files_properties(
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ecs/RenderSync.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ecs/World.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ecs/SceneSerializer.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ecs/MathUtil.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/physics/PhysicsSystem.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/physics/Particles.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/navigation/NavigationSystem.cpp"
        TARGET_DIRECTORY CramionCoreObjects
        PROPERTIES COMPILE_OPTIONS "$<$<CONFIG:Debug>:-O2>"
    )
endif()

add_library(CramionCore STATIC $<TARGET_OBJECTS:CramionCoreObjects>)
add_library(Cramion::Core ALIAS CramionCore)
target_include_directories(CramionCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(CramionCore
    PUBLIC Cramion::Assets Cramion::FX EnTT::EnTT nlohmann_json::nlohmann_json
    PRIVATE Jolt cramion_lua cramion_miniaudio RecastNavigation::Recast RecastNavigation::Detour
            RecastNavigation::DetourCrowd
)

# --- Pruebas del ECS (consola) ---
# Compila las fuentes del ECS que no dependen de la implementacion de los
# assets: se puede probar aunque CramionAssets este a medias.
add_executable(CramionCoreTests
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/CoreTests.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ecs/World.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ecs/Components.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ecs/Reflection.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ecs/MathUtil.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ecs/SceneSerializer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ecs/ModelInstantiation.cpp"
)
target_include_directories(CramionCoreTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(CramionCoreTests PRIVATE Cramion::FX EnTT::EnTT nlohmann_json::nlohmann_json)
target_compile_options(CramionCoreTests PRIVATE -Wall -Wextra)

# --- Pruebas de la fisica (consola, sin GPU) ---
# Jolt a traves de PhysicsSystem: caidas, eventos, triggers, consultas con
# capas, cinematicos, fuerzas y particulas que chocan.
add_executable(CramionPhysicsTests "${CMAKE_CURRENT_SOURCE_DIR}/tests/PhysicsTests.cpp")
target_link_libraries(CramionPhysicsTests PRIVATE Cramion::Core)
target_compile_options(CramionPhysicsTests PRIVATE -Wall -Wextra)

# --- Pruebas de las cinematicas (consola) ---
add_executable(CramionCinematicsTests "${CMAKE_CURRENT_SOURCE_DIR}/tests/CinematicsTests.cpp")
target_link_libraries(CramionCinematicsTests PRIVATE Cramion::Core)
target_compile_options(CramionCinematicsTests PRIVATE -Wall -Wextra)

# --- Pruebas del terreno (consola) ---
add_executable(CramionTerrainTests "${CMAKE_CURRENT_SOURCE_DIR}/tests/TerrainTests.cpp")
target_link_libraries(CramionTerrainTests PRIVATE Cramion::Core)
target_compile_options(CramionTerrainTests PRIVATE -Wall -Wextra)

# --- Pruebas de los materiales (consola) ---
add_executable(CramionMaterialTests "${CMAKE_CURRENT_SOURCE_DIR}/tests/MaterialTests.cpp")
target_link_libraries(CramionMaterialTests PRIVATE Cramion::Core)
target_compile_options(CramionMaterialTests PRIVATE -Wall -Wextra)

# --- Pruebas del agua (consola) ---
add_executable(CramionWaterTests "${CMAKE_CURRENT_SOURCE_DIR}/tests/WaterTests.cpp")
target_link_libraries(CramionWaterTests PRIVATE Cramion::Core)
target_compile_options(CramionWaterTests PRIVATE -Wall -Wextra)

# --- Pruebas del scripting (Lua) y del audio (consola) ---
add_executable(CramionScriptingTests "${CMAKE_CURRENT_SOURCE_DIR}/tests/ScriptingTests.cpp")
target_link_libraries(CramionScriptingTests PRIVATE Cramion::Core)
target_compile_options(CramionScriptingTests PRIVATE -Wall -Wextra)

# --- Pruebas de la interfaz del juego (consola) ---
add_executable(CramionUiTests "${CMAKE_CURRENT_SOURCE_DIR}/tests/UiTests.cpp")
target_link_libraries(CramionUiTests PRIVATE Cramion::Core)

# --- Pruebas de los vehiculos (consola) ---
add_executable(CramionPackTests "${CMAKE_CURRENT_SOURCE_DIR}/tests/PackTests.cpp")
target_link_libraries(CramionPackTests PRIVATE Cramion::Core)

add_executable(CramionVehicleTests "${CMAKE_CURRENT_SOURCE_DIR}/tests/VehicleTests.cpp")
target_link_libraries(CramionVehicleTests PRIVATE Cramion::Core)

# --- Pruebas de la navegacion (consola) ---
# Recast/Detour: malla dentro del volumen, caminos que rodean obstaculos,
# regeneracion al mover un collider, modificadores y agentes que llegan.
add_executable(CramionNavigationTests "${CMAKE_CURRENT_SOURCE_DIR}/tests/NavigationTests.cpp")
target_link_libraries(CramionNavigationTests PRIVATE Cramion::Core)
target_compile_options(CramionNavigationTests PRIVATE -Wall -Wextra)

# Demo MiniGolf (kit de Kenney): crea el proyecto con sus escenas y lo prueba
# sin ventana (cramion_minigolf <kit> tools/minigolf <destino> --probar).
add_executable(cramion_minigolf "${CMAKE_CURRENT_SOURCE_DIR}/tools/MiniGolfDemo.cpp")
target_link_libraries(cramion_minigolf PRIVATE Cramion::Core)

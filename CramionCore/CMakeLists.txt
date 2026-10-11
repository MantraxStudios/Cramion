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

# --- Audio (miniaudio) ---------------------------------------------------------
# miniaudio: audio 2D/3D (WAV, MP3, FLAC y OGG con stb_vorbis). Sin su
# CMakeLists (SOURCE_SUBDIR inexistente). El scripting es C++ (sin Lua desde
# la 2.9).
FetchContent_Declare(miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG 0.11.25
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR _sin_cmake
)
FetchContent_MakeAvailable(miniaudio)

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

# ENet: red por UDP (mensajes fiables y no fiables, conexiones, ping) para el
# multijugador (net/Network). Libreria estatica pequena en C.
FetchContent_Declare(enet
    GIT_REPOSITORY https://github.com/lsalzman/enet.git
    GIT_TAG v1.3.18
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(enet)
target_compile_options(enet PRIVATE -O2 -w)

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
    PRIVATE nlohmann_json::nlohmann_json assimp libzstd_static
)
target_include_directories(CramionAssets PRIVATE "${zstd_SOURCE_DIR}/lib")
target_compile_options(CramionAssets PRIVATE -Wall -Wextra)

# Herramienta de consola de los assets: crea un proyecto temporal, importa
# modelos y un cielo, y los vuelve a cargar (cramion_asset_tool --help).
if(WIN32)
    target_link_libraries(CramionAssets PRIVATE shell32 ole32)
endif()
if(NOT ANDROID)
add_executable(cramion_asset_tool "${CMAKE_CURRENT_SOURCE_DIR}/tools/AssetTool.cpp")
target_link_libraries(cramion_asset_tool PRIVATE Cramion::Assets)
target_compile_options(cramion_asset_tool PRIVATE -Wall -Wextra)
endif()

# --- CramionCore -------------------------------------------------------------

file(GLOB_RECURSE CRAMION_CORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
list(FILTER CRAMION_CORE_SOURCES EXCLUDE REGEX "/src/(asset|project)/")

# Los objetos del ECS se compilan aparte (OBJECT) y solo con las cabeceras de
# los assets: asi se pueden compilar aunque CramionAssets este a medias.
add_library(CramionCoreObjects OBJECT ${CRAMION_CORE_SOURCES})
target_include_directories(CramionCoreObjects PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(CramionCoreObjects PUBLIC Cramion::FX EnTT::EnTT nlohmann_json::nlohmann_json
    PRIVATE Jolt cramion_miniaudio RecastNavigation::Recast RecastNavigation::Detour
            RecastNavigation::DetourCrowd)
target_include_directories(CramionCoreObjects PRIVATE "${enet_SOURCE_DIR}/include")
# SDK de los scripts de C++ (cramion/CppProtocol.h lo comparten motor y juegos).
target_include_directories(CramionCoreObjects PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/sdk")
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
        "${CMAKE_CURRENT_SOURCE_DIR}/src/voxel/Generator.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/voxel/Mesher.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/voxel/VoxelSystem.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/voxel/TextureGen.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/foliage/Foliage.cpp"
        TARGET_DIRECTORY CramionCoreObjects
        PROPERTIES COMPILE_OPTIONS "$<$<CONFIG:Debug>:-O2>"
    )
endif()

add_library(CramionCore STATIC $<TARGET_OBJECTS:CramionCoreObjects>)
add_library(Cramion::Core ALIAS CramionCore)
target_include_directories(CramionCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/sdk")
target_link_libraries(CramionCore
    PUBLIC Cramion::Assets Cramion::FX EnTT::EnTT nlohmann_json::nlohmann_json
    PRIVATE Jolt cramion_miniaudio RecastNavigation::Recast RecastNavigation::Detour
            RecastNavigation::DetourCrowd enet
)

if(WIN32)
    target_link_libraries(CramionCore PRIVATE ws2_32 winmm winhttp)
elseif(ANDROID)
    # miniaudio (AAudio/OpenSL por dlopen) y el log del sistema.
    target_link_libraries(CramionCore PRIVATE android log OpenSLES dl)
endif()

# Android: solo las librerias (el juego); pruebas y herramientas son de PC.
if(ANDROID)
    return()
endif()

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
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ecs/ComponentPresets.cpp"
    # Components.cpp registra el componente Environment (reflect).
    "${CMAKE_CURRENT_SOURCE_DIR}/src/environment/Environment.cpp"
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

# --- Pruebas de los sistemas de la 2.1 (consola) ---
add_executable(CramionFeatures21Tests "${CMAKE_CURRENT_SOURCE_DIR}/tests/Features21Tests.cpp")
target_link_libraries(CramionFeatures21Tests PRIVATE Cramion::Core)
target_compile_options(CramionFeatures21Tests PRIVATE -Wall -Wextra)

# --- Pruebas de las optimizaciones del roadmap (Insights, particulas, scripting) ---
add_executable(CramionOptimizationTests "${CMAKE_CURRENT_SOURCE_DIR}/tests/OptimizationTests.cpp")
target_link_libraries(CramionOptimizationTests PRIVATE Cramion::Core)
target_compile_options(CramionOptimizationTests PRIVATE -Wall -Wextra)

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

# --- Pruebas del scripting y del audio (consola) ---
# Multijugador: servidor y clientes en el mismo proceso por 127.0.0.1.
add_executable(CramionNetworkTests "${CMAKE_CURRENT_SOURCE_DIR}/tests/NetworkTests.cpp")
target_link_libraries(CramionNetworkTests PRIVATE Cramion::Core)
target_compile_options(CramionNetworkTests PRIVATE -Wall -Wextra)

# HTTP: un servidor local con winsock (solo Windows).
if(WIN32)
    add_executable(CramionHttpTests "${CMAKE_CURRENT_SOURCE_DIR}/tests/HttpTests.cpp")
    target_link_libraries(CramionHttpTests PRIVATE Cramion::Core ws2_32)
    target_compile_options(CramionHttpTests PRIVATE -Wall -Wextra)
endif()

add_executable(CramionScriptingTests "${CMAKE_CURRENT_SOURCE_DIR}/tests/ScriptingTests.cpp")
target_link_libraries(CramionScriptingTests PRIVATE Cramion::Core)
target_compile_options(CramionScriptingTests PRIVATE -Wall -Wextra)

# --- Pruebas de la entrada por acciones (Enhanced Input) y su API ---
add_executable(CramionInputTests "${CMAKE_CURRENT_SOURCE_DIR}/tests/InputTests.cpp")
target_link_libraries(CramionInputTests PRIVATE Cramion::Core)
target_compile_options(CramionInputTests PRIVATE -Wall -Wextra)

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

# --- Pruebas de los shaders de superficie del usuario (consola; shaderc) ---
add_executable(CramionShaderTests "${CMAKE_CURRENT_SOURCE_DIR}/tests/ShaderTests.cpp")
target_link_libraries(CramionShaderTests PRIVATE Cramion::Core)
target_compile_options(CramionShaderTests PRIVATE -Wall -Wextra)
cramionfx_deploy(CramionShaderTests)

# --- Pruebas de la animacion (consola) ---
add_executable(CramionAnimationTests "${CMAKE_CURRENT_SOURCE_DIR}/tests/AnimationTests.cpp")
target_link_libraries(CramionAnimationTests PRIVATE Cramion::Core)
target_compile_options(CramionAnimationTests PRIVATE -Wall -Wextra)

# --- Pruebas de las maquinas de estados de IA (consola) ---
add_executable(CramionStateMachineTests "${CMAKE_CURRENT_SOURCE_DIR}/tests/StateMachineTests.cpp")
target_link_libraries(CramionStateMachineTests PRIVATE Cramion::Core)
target_compile_options(CramionStateMachineTests PRIVATE -Wall -Wextra)

# --- Pruebas de la API de scripting en C++ (consola) ---
add_executable(CramionNativeApiTests "${CMAKE_CURRENT_SOURCE_DIR}/tests/NativeApiTests.cpp")
target_link_libraries(CramionNativeApiTests PRIVATE Cramion::Core)
target_compile_options(CramionNativeApiTests PRIVATE -Wall -Wextra)

# --- Pruebas de cada modulo de la API de scripting (tests/native, consola) ---
# Una por modulo (src/scripting/native): <Modulo>Tests.cpp -> CramionApi<Modulo>Tests.
file(GLOB CRAMION_API_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/native/*Tests.cpp")
foreach(test_source IN LISTS CRAMION_API_TESTS)
    get_filename_component(test_name "${test_source}" NAME_WE)
    add_executable(CramionApi${test_name} "${test_source}")
    target_link_libraries(CramionApi${test_name} PRIVATE Cramion::Core)
    target_compile_options(CramionApi${test_name} PRIVATE -Wall -Wextra)
endforeach()

# --- Pruebas de los prefabs (consola) ---
add_executable(CramionPrefabTests "${CMAKE_CURRENT_SOURCE_DIR}/tests/PrefabTests.cpp")
target_link_libraries(CramionPrefabTests PRIVATE Cramion::Core)
target_compile_options(CramionPrefabTests PRIVATE -Wall -Wextra)

# --- Pruebas del World Partition (consola) ---
add_executable(CramionWorldPartitionTests "${CMAKE_CURRENT_SOURCE_DIR}/tests/WorldPartitionTests.cpp")
target_link_libraries(CramionWorldPartitionTests PRIVATE Cramion::Core)
target_compile_options(CramionWorldPartitionTests PRIVATE -Wall -Wextra)

# --- Pruebas de las mallas creadas por codigo (consola) ---
add_executable(CramionMeshTests "${CMAKE_CURRENT_SOURCE_DIR}/tests/MeshTests.cpp")
target_link_libraries(CramionMeshTests PRIVATE Cramion::Core)
target_compile_options(CramionMeshTests PRIVATE -Wall -Wextra)

# --- Pruebas del modelado poligonal (mallas editables, consola) ---
add_executable(CramionModelingTests "${CMAKE_CURRENT_SOURCE_DIR}/tests/ModelingTests.cpp")
target_link_libraries(CramionModelingTests PRIVATE Cramion::Core)
target_compile_options(CramionModelingTests PRIVATE -Wall -Wextra)

# --- Pruebas de las CVars y de los scripts de C++ aislados (consola) ---
# Compila scripts que fallan a proposito y los ejecuta en CramionScriptHost.
add_executable(CramionCppScriptTests "${CMAKE_CURRENT_SOURCE_DIR}/tests/CppScriptTests.cpp")
target_link_libraries(CramionCppScriptTests PRIVATE Cramion::Core)
target_compile_options(CramionCppScriptTests PRIVATE -Wall -Wextra)
target_compile_definitions(CramionCppScriptTests PRIVATE
    CRAMION_SDK_DIR="${CMAKE_CURRENT_SOURCE_DIR}/sdk" CRAMION_BIN_DIR="${CMAKE_BINARY_DIR}")

# --- Pruebas del mundo de bloques (consola) ---
add_executable(CramionVoxelTests "${CMAKE_CURRENT_SOURCE_DIR}/tests/VoxelTests.cpp")
target_link_libraries(CramionVoxelTests PRIVATE Cramion::Core)
target_compile_options(CramionVoxelTests PRIVATE -Wall -Wextra)

# Demo MiniGolf (kit de Kenney): crea el proyecto con sus escenas y lo prueba
# sin ventana (cramion_minigolf <kit> tools/minigolf <destino> --probar).
add_executable(cramion_minigolf "${CMAKE_CURRENT_SOURCE_DIR}/tools/MiniGolfDemo.cpp")
target_link_libraries(cramion_minigolf PRIVATE Cramion::Core)
target_compile_definitions(cramion_minigolf PRIVATE
    CRAMION_SDK_DIR="${CMAKE_CURRENT_SOURCE_DIR}/sdk" CRAMION_BIN_DIR="${CMAKE_BINARY_DIR}")

# Referencia de componentes del manual (docs-src/pages/componentes-*.html),
# generada desde la reflexion: cramion_docgen <carpeta docs-src/pages>.
# La API para los scripts de C++ (sdk/cramion/Api.gen.h), generada desde el
# registro de la API (ScriptSystem::apiRegistry). Mientras se porta la API de
# la version con Lua no se regenera al compilar: se ejecuta a mano
# (cramion_sdkgen sdk/cramion) cuando el registro este completo.
add_executable(cramion_sdkgen "${CMAKE_CURRENT_SOURCE_DIR}/tools/SdkGen.cpp" "${CMAKE_SOURCE_DIR}/CramionEditor/src/LuaCompletion.cpp")
target_include_directories(cramion_sdkgen PRIVATE "${CMAKE_SOURCE_DIR}/CramionEditor/src")
target_link_libraries(cramion_sdkgen PRIVATE Cramion::Core)

add_executable(cramion_docgen "${CMAKE_CURRENT_SOURCE_DIR}/tools/DocGen.cpp")
target_link_libraries(cramion_docgen PRIVATE Cramion::Core)

# Herramienta: tiempo de carga y conversion de clips (.cranim)
add_executable(cramion_cliptime "${CMAKE_CURRENT_SOURCE_DIR}/tests/ClipTimingTool.cpp")
target_link_libraries(cramion_cliptime PRIVATE Cramion::Core)

# Herramienta: el render en estereo de VR sin casco (ventana invisible, capas
# de validacion si estan). Junto al editor: usa su carpeta shaders/.
add_executable(cramion_stereo_check "${CMAKE_CURRENT_SOURCE_DIR}/tools/StereoRenderCheck.cpp")
target_link_libraries(cramion_stereo_check PRIVATE Cramion::Core)
set_target_properties(cramion_stereo_check PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# Prueba de la compresion de texturas (BC1/BC7 por uso, claves portables del
# juego exportado): solo CramionFX, sin GPU.
add_executable(CramionTextureTests "${CMAKE_CURRENT_SOURCE_DIR}/tests/TextureCompressionTests.cpp")
target_link_libraries(CramionTextureTests PRIVATE Cramion::FX)

# Herramienta: banco de pruebas del render (ventana invisible): tiempos de GPU
# por pasada y ruido temporal con la camara quieta (el "hormigueo" del
# trazado de rayos). En su propia carpeta (bench/) con sus shaders: compilarla
# sola no toca los del editor.
add_executable(cramion_render_bench "${CMAKE_CURRENT_SOURCE_DIR}/tools/RenderBench.cpp")
target_link_libraries(cramion_render_bench PRIVATE Cramion::FX)
set_target_properties(cramion_render_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bench")
cramionfx_deploy(cramion_render_bench)

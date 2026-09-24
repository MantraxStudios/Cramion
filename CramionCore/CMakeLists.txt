# CramionCore: la capa de juego sobre CramionFX, como el "runtime" de Unity.
#
#   CramionAssets (Cramion::Assets)  proyectos, assets .crdata con UUID,
#                                    importador y base de datos de assets
#   CramionCore   (Cramion::Core)    ECS (EnTT), componentes (Transform,
#                                    MeshRenderer, Animator, Light, Camera,
#                                    PostProcessing...), escenas .crscene y el
#                                    puente con el renderizador
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
    PRIVATE nlohmann_json::nlohmann_json shell32 ole32 assimp
)
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
target_link_libraries(CramionCoreObjects PUBLIC Cramion::FX EnTT::EnTT nlohmann_json::nlohmann_json)
target_compile_options(CramionCoreObjects PRIVATE -Wall -Wextra)

add_library(CramionCore STATIC $<TARGET_OBJECTS:CramionCoreObjects>)
add_library(Cramion::Core ALIAS CramionCore)
target_include_directories(CramionCore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(CramionCore
    PUBLIC Cramion::Assets Cramion::FX EnTT::EnTT nlohmann_json::nlohmann_json
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

cmake_minimum_required(VERSION 3.21)

project(Cramion
    VERSION 2.9.0
    DESCRIPTION "Motor con capa de plataforma DirectX 12 (CramionDM) y renderizador Vulkan (CramionFX)"
    LANGUAGES CXX
)

# Version previa (canal beta del actualizador): -DCRAMION_VERSION_SUFFIX=beta.1
# da "0.7.0-beta.1". Vacio en las versiones estables.
set(CRAMION_VERSION_SUFFIX "" CACHE STRING "Sufijo de version previa (beta.1); vacio = estable")
set(CRAMION_VERSION_FULL "${PROJECT_VERSION}")
if(CRAMION_VERSION_SUFFIX)
    string(APPEND CRAMION_VERSION_FULL "-${CRAMION_VERSION_SUFFIX}")
endif()

# Recursos de Windows (el icono de los .exe: assets/icon.ico).
if(WIN32)
    enable_language(RC)
endif()
# C: miniaudio (el audio de CramionCore) y zstd.
enable_language(C)
set(CRAMION_ICON_RC "${CMAKE_CURRENT_SOURCE_DIR}/assets/cramion.rc")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Compilaciones a la vez segun la RAM: Ninja lanza una por nucleo + 2 y los
# archivos pesados (VulkanRenderer.cpp, RenderSync.cpp) llegan a ~1,5 GB; con 16
# nucleos y 16 GB eso agota la memoria y congela el PC. Se reservan 4 GB para
# el sistema y se cuentan 1,5 GB por compilacion. -DCRAMION_COMPILE_JOBS=N lo fija
# a mano; enlazar va de 2 en 2 (el editor y el player enlazan a la vez).
if(CMAKE_GENERATOR MATCHES "Ninja")
    cmake_host_system_information(RESULT _cramion_ram_mb QUERY TOTAL_PHYSICAL_MEMORY)
    cmake_host_system_information(RESULT _cramion_cores QUERY NUMBER_OF_LOGICAL_CORES)
    math(EXPR _cramion_jobs "(${_cramion_ram_mb} - 4096) / 1536")
    if(_cramion_jobs LESS 1)
        set(_cramion_jobs 1)
    endif()
    if(_cramion_jobs GREATER _cramion_cores)
        set(_cramion_jobs ${_cramion_cores})
    endif()
    set(CRAMION_COMPILE_JOBS ${_cramion_jobs} CACHE STRING "Compilaciones en paralelo (por defecto, segun la RAM)")
    set_property(GLOBAL PROPERTY JOB_POOLS cramion_compile=${CRAMION_COMPILE_JOBS} cramion_link=2)
    set(CMAKE_JOB_POOL_COMPILE cramion_compile)
    set(CMAKE_JOB_POOL_LINK cramion_link)
    message(STATUS "Cramion: ${CRAMION_COMPILE_JOBS} compilaciones a la vez (${_cramion_ram_mb} MB de RAM, ${_cramion_cores} nucleos)")
endif()

# Capa de plataforma (ventana, entrada, dispositivo DX12).
add_subdirectory(CramionDM)

# Renderizador Vulkan (libreria): todo el motor grafico y sus shaders.
add_subdirectory(CramionFX)

# -----------------------------------------------------------------------------
# Aplicacion de ejemplo: carga un escenario y lo recorre con la camara.
# -----------------------------------------------------------------------------

# Android (NDK): solo el juego (libmain.so, CramionEditor/android); el
# editor, el actualizador y las herramientas son de Windows.
if(ANDROID)
    add_subdirectory(CramionCore)
    add_subdirectory(CramionEditor)
    return()
endif()

# Linux (preset linux-release): el motor (Core, FX, la ventana Xlib) y el
# servidor dedicado CramionServer. El editor y el reproductor siguen siendo
# de Windows (usan Win32/GDI+ en su interfaz y carga).
if(UNIX AND NOT APPLE)
    add_subdirectory(CramionCore)
    add_executable(CramionServer CramionEditor/server/ServerMain.cpp)
    target_link_libraries(CramionServer PRIVATE Cramion::Core)
    set_target_properties(CramionServer PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    # El proceso aislado de los scripts de C++ (dlopen de la .so del juego) y
    # el SDK junto a los ejecutables (sdk/), como en Windows.
    add_executable(CramionScriptHost CramionEditor/host/ScriptHost.cpp)
    target_include_directories(CramionScriptHost PRIVATE "${CMAKE_SOURCE_DIR}/CramionCore/include" "${CMAKE_SOURCE_DIR}/CramionCore/sdk")
    target_link_libraries(CramionScriptHost PRIVATE ${CMAKE_DL_LIBS})
    target_compile_options(CramionScriptHost PRIVATE -Wall -Wextra)
    set_target_properties(CramionScriptHost PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    add_custom_target(cramion_sdk_copy ALL
        COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
                "${CMAKE_SOURCE_DIR}/CramionCore/sdk" "${CMAKE_BINARY_DIR}/sdk"
        COMMENT "SDK de los scripts de C++")
    add_dependencies(CramionScriptHost cramion_sdk_copy)
    add_dependencies(CramionServer CramionScriptHost)
    add_dependencies(CramionCppScriptTests CramionScriptHost)
    return()
endif()

add_executable(cramion src/main.cpp "${CRAMION_ICON_RC}")
# En Release, sin consola (como el editor y el player).
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set_target_properties(cramion PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_options(cramion PRIVATE "-Xlinker" "/entry:mainCRTStartup")
endif()
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

# Actualizador (CramionUpdater.exe) y su libreria comun con el editor.
add_subdirectory(CramionUpdater)

# -----------------------------------------------------------------------------
# Runtime de Android: el juego compilado con el NDK (android/<abi>/libmain.so
# junto al editor) para exportar APK/AAB. Se compila en su propia carpeta
# (build-android, build-android-x64) despues del editor, con la mitad de
# compilaciones en paralelo para no sumar RAM a la del resto.
# -----------------------------------------------------------------------------
set(_cramion_ndk "")
if(DEFINED ENV{ANDROID_NDK_HOME} AND EXISTS "$ENV{ANDROID_NDK_HOME}/build/cmake/android.toolchain.cmake")
    set(_cramion_ndk "$ENV{ANDROID_NDK_HOME}")
else()
    file(GLOB _cramion_ndks "$ENV{LOCALAPPDATA}/Android/Sdk/ndk/*")
    list(SORT _cramion_ndks COMPARE NATURAL ORDER DESCENDING)
    foreach(_ndk IN LISTS _cramion_ndks)
        if(EXISTS "${_ndk}/build/cmake/android.toolchain.cmake")
            set(_cramion_ndk "${_ndk}")
            break()
        endif()
    endforeach()
endif()
if(_cramion_ndk)
    set(_cramion_android_default ON)
else()
    set(_cramion_android_default OFF)
endif()
option(CRAMION_ANDROID "Compilar el runtime de Android (exportar a APK/AAB); necesita el Android NDK" ${_cramion_android_default})
set(CRAMION_ANDROID_NDK "${_cramion_ndk}" CACHE PATH "Carpeta del Android NDK")
# arm64-v8a: casi todos los moviles; armeabi-v7a: los de sistema de 32 bits
# (Android Go, gama baja); x86_64: emuladores.
set(CRAMION_ANDROID_ABIS "arm64-v8a;armeabi-v7a" CACHE STRING "ABIs de Android (arm64-v8a;armeabi-v7a;x86_64 para emuladores)")
if(CRAMION_ANDROID)
    if(NOT EXISTS "${CRAMION_ANDROID_NDK}/build/cmake/android.toolchain.cmake")
        message(FATAL_ERROR "CRAMION_ANDROID=ON pero no se encontro el NDK (CRAMION_ANDROID_NDK)")
    endif()
    include(ExternalProject)
    file(GLOB _cramion_strip "${CRAMION_ANDROID_NDK}/toolchains/llvm/prebuilt/*/bin/llvm-strip*")
    list(GET _cramion_strip 0 _cramion_strip)
    math(EXPR _cramion_android_jobs "${CRAMION_COMPILE_JOBS} / 2")
    if(_cramion_android_jobs LESS 1)
        set(_cramion_android_jobs 1)
    endif()
    foreach(_abi IN LISTS CRAMION_ANDROID_ABIS)
        if(_abi STREQUAL "arm64-v8a")
            set(_dir "${CMAKE_SOURCE_DIR}/build-android")
        else()
            string(REPLACE "x86_64" "x64" _suffix "${_abi}")
            set(_dir "${CMAKE_SOURCE_DIR}/build-android-${_suffix}")
        endif()
        ExternalProject_Add(cramion_android_${_abi}
            SOURCE_DIR "${CMAKE_SOURCE_DIR}"
            BINARY_DIR "${_dir}"
            CMAKE_GENERATOR Ninja
            CMAKE_ARGS
                "-DCMAKE_TOOLCHAIN_FILE=${CRAMION_ANDROID_NDK}/build/cmake/android.toolchain.cmake"
                "-DANDROID_ABI=${_abi}"
                # Android 8.0 (Vulkan y AAudio): el modo compatible del
                # renderizador funciona desde Vulkan 1.0.
                -DANDROID_PLATFORM=android-26
                -DANDROID_STL=c++_static
                -DCMAKE_BUILD_TYPE=Release
                "-DCRAMION_COMPILE_JOBS=${_cramion_android_jobs}"
            BUILD_COMMAND "${CMAKE_COMMAND}" --build "${_dir}" --target main
            # Sin simbolos (la de la carpeta de compilacion los conserva para
            # llvm-symbolizer): de ~250 MB a ~25 MB dentro del APK.
            INSTALL_COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/android/${_abi}"
                    COMMAND "${_cramion_strip}" --strip-unneeded "${_dir}/CramionEditor/libmain.so"
                            -o "${CMAKE_BINARY_DIR}/android/${_abi}/libmain.so"
            BUILD_ALWAYS TRUE
            USES_TERMINAL_BUILD TRUE)
        add_dependencies(cramion_android_${_abi} CramionEditor)
    endforeach()
    # bundletool (Google, Apache 2.0) para los AAB de Google Play.
    set(_bundletool "${CMAKE_BINARY_DIR}/android/bundletool.jar")
    if(NOT EXISTS "${_bundletool}")
        message(STATUS "Descargando bundletool (AAB de Android)...")
        file(DOWNLOAD "https://github.com/google/bundletool/releases/download/1.18.3/bundletool-all-1.18.3.jar"
             "${_bundletool}.part" STATUS _status TLS_VERIFY ON)
        list(GET _status 0 _code)
        if(_code EQUAL 0)
            file(RENAME "${_bundletool}.part" "${_bundletool}")
        else()
            file(REMOVE "${_bundletool}.part")
            message(WARNING "No se pudo descargar bundletool: sin el, Exportar a Android solo hace APK")
        endif()
    endif()
endif()

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

# -----------------------------------------------------------------------------
# Paquete descargable: cmake --build --preset clang-ninja-release --target cramion_package
# Deja <build>/Cramion-win64.zip con los ejecutables, shaders, iconos y el
# runtime de C++ (sin las escenas de demostracion). Ver cmake/PackageRelease.cmake.
# -----------------------------------------------------------------------------
add_custom_target(cramion_package
    COMMAND ${CMAKE_COMMAND}
            -DBIN_DIR=${CMAKE_BINARY_DIR}
            -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
            -DVERSION=${CRAMION_VERSION_FULL}
            -P "${CMAKE_SOURCE_DIR}/cmake/PackageRelease.cmake"
    COMMENT "Empaquetando Cramion ${CRAMION_VERSION_FULL} (Cramion-win64.zip)"
    VERBATIM)
add_dependencies(cramion_package cramion CramionEditor CramionPlayer CramionUpdater)
if(CRAMION_ANDROID)
    foreach(_abi IN LISTS CRAMION_ANDROID_ABIS)
        add_dependencies(cramion_package cramion_android_${_abi})
    endforeach()
endif()

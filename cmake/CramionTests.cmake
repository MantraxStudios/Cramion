# Registro de las pruebas en CTest (ROADMAP 3.6, "CI con CTest").
#
# Cada ejecutable de pruebas del motor (CramionCore/tests, tests/native,
# CramionEditor/tests, CramionUpdater/tests) es un test de CTest con etiquetas
# y tiempo maximo. Las etiquetas:
#
#   rapidos  sin GPU, sin ventana, sin Internet y sin sonido: los que corre la
#            CI (ctest -L rapidos). Tardan segundos.
#   lentos   sin GPU pero tardan (compilan scripts de C++, generan mundos).
#            La CI de Windows tambien los corre; la de Linux no.
#   editor   pruebas del codigo del editor (CramionEditor/tests), sin ventana.
#   red      abren sockets en 127.0.0.1 (servidor y clientes en el proceso).
#   gpu      necesitan GPU con Vulkan y abren una ventana invisible: NUNCA en
#            la CI (ctest -LE gpu). Solo a mano en un PC con GPU.
#
# Uso (ver PRUEBAS.md):
#   ctest --test-dir build-release -L rapidos -j 4 --output-on-failure
#   ctest --preset rapidos
#
# Se llama al final del CMakeLists de la raiz (cramion_register_tests()),
# cuando ya existen todos los objetivos: los que no se compilan en esta
# plataforma (CramionHttpTests fuera de Windows...) se saltan solos.

include_guard(GLOBAL)

# cramion_test(<objetivo> [LABELS ...] [TIMEOUT s] [ARGS ...] [NAME nombre])
function(cramion_test target)
    cmake_parse_arguments(T "" "TIMEOUT;NAME" "LABELS;ARGS" ${ARGN})
    if(NOT TARGET ${target})
        return()
    endif()
    if(NOT T_TIMEOUT)
        set(T_TIMEOUT 120)
    endif()
    if(NOT T_NAME)
        set(T_NAME ${target})
    endif()
    if(NOT T_LABELS)
        set(T_LABELS rapidos)
    endif()
    # cramion_tests compila todas las pruebas que no necesitan GPU (la CI).
    if(NOT "gpu" IN_LIST T_LABELS AND TARGET cramion_tests)
        add_dependencies(cramion_tests ${target})
    endif()
    # Se ejecutan en la carpeta del .exe, como al lanzarlas a mano (sus
    # temporales y sus shaders quedan junto al ejecutable).
    add_test(NAME ${T_NAME} COMMAND ${target} ${T_ARGS}
             WORKING_DIRECTORY "$<TARGET_FILE_DIR:${target}>")
    # CRAMION_TESTS_SILENT: las pruebas que abririan el dispositivo de audio
    # de verdad se lo saltan (nada suena al pasar la CI ni en local).
    set(env "CRAMION_TESTS_SILENT=1")
    set_tests_properties(${T_NAME} PROPERTIES
        LABELS "${T_LABELS}"
        TIMEOUT ${T_TIMEOUT}
        ENVIRONMENT "${env}")
    # Sanitizers en Windows: la DLL de ASan (clang_rt.asan_dynamic) tiene que
    # estar en el PATH del proceso de la prueba.
    if(CRAMION_SANITIZER_RUNTIME_DIR AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.22)
        set_property(TEST ${T_NAME} APPEND PROPERTY ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:${CRAMION_SANITIZER_RUNTIME_DIR}")
    endif()
    if(CRAMION_SANITIZE)
        # ASan y UBSan: detener en el primer error (codigo distinto de 0) y
        # sin la deteccion de fugas al salir (no existe en Windows).
        set_property(TEST ${T_NAME} APPEND PROPERTY ENVIRONMENT
            "ASAN_OPTIONS=detect_leaks=0:halt_on_error=1:abort_on_error=0:windows_hook_rtl_allocators=0"
            "UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1:suppressions='${CMAKE_SOURCE_DIR}/cmake/ubsan.supp'")
    endif()
endfunction()

function(cramion_register_tests)
    # Objetivo que compila todas las pruebas sin GPU:
    #   cmake --build build-release --target cramion_tests
    add_custom_target(cramion_tests)
    # El build de fuzzing (CRAMION_FUZZ) es solo para los objetivos de
    # libFuzzer: en Windows, con la cobertura de libFuzzer y sin enlazar
    # libFuzzer, las excepciones de C++ fallan. Las pruebas normales con
    # sanitizers van en el build de CRAMION_SANITIZE sin CRAMION_FUZZ.
    if(CRAMION_FUZZ)
        if(COMMAND cramion_register_fuzz_tests)
            cramion_register_fuzz_tests()
        endif()
        return()
    endif()
    # --- CramionCore/tests (consola, sin GPU) ---
    cramion_test(CramionCoreTests)
    cramion_test(CramionPhysicsTests)
    cramion_test(CramionFeatures21Tests)
    cramion_test(CramionOptimizationTests)
    cramion_test(CramionCinematicsTests)
    cramion_test(CramionKeyframeTests)
    cramion_test(CramionTerrainTests TIMEOUT 300)
    cramion_test(CramionMaterialTests)            # shaderc (DLL del Vulkan SDK), sin GPU
    cramion_test(CramionVideoTests)
    cramion_test(CramionWaterTests)
    cramion_test(CramionNetworkTests LABELS rapidos red TIMEOUT 300)
    cramion_test(CramionHttpTests LABELS rapidos red)          # solo 127.0.0.1 (sin --online)
    cramion_test(CramionWsProtocolTests)
    cramion_test(CramionWebSocketTests LABELS rapidos red)
    cramion_test(CramionScriptingTests)           # audio: sin altavoces con CRAMION_TESTS_SILENT
    cramion_test(CramionInputTests)
    cramion_test(CramionUiTests)
    cramion_test(CramionPackTests)
    cramion_test(CramionVehicleTests)
    cramion_test(CramionNavigationTests)
    cramion_test(CramionShaderTests)
    cramion_test(CramionAnimationTests)
    cramion_test(CramionStateMachineTests)
    cramion_test(CramionNativeApiTests)
    cramion_test(CramionPrefabTests)
    cramion_test(CramionWorldPartitionTests)
    cramion_test(CramionSceneChangeTests)
    cramion_test(CramionSceneLoadTests)
    cramion_test(CramionMeshTests)
    cramion_test(CramionModelingTests)
    cramion_test(CramionCppScriptTests LABELS lentos TIMEOUT 900)  # compila scripts de C++ con clang
    cramion_test(CramionVoxelTests LABELS lentos TIMEOUT 900)
    cramion_test(CramionTextureTests)

    # --- CramionCore/tests/native: una por modulo de la API de scripting ---
    file(GLOB api_tests "${CMAKE_SOURCE_DIR}/CramionCore/tests/native/*Tests.cpp")
    foreach(source IN LISTS api_tests)
        get_filename_component(name "${source}" NAME_WE)
        if(name MATCHES "^(Http|WebSocket|Network)ApiTests$")
            cramion_test(CramionApi${name} LABELS rapidos red)
        else()
            cramion_test(CramionApi${name})
        endif()
    endforeach()

    # --- CramionEditor/tests (el codigo del editor, en consola) ---
    cramion_test(CramionPreviewTests LABELS rapidos editor)
    cramion_test(CramionTerminalTests LABELS rapidos editor)
    cramion_test(CramionCompletionTests LABELS rapidos editor)
    cramion_test(CramionMultiEditTests LABELS rapidos editor)
    cramion_test(CramionHistoryTests LABELS rapidos editor)
    # El icono se escribe en una copia de un .exe cualquiera (el de la propia
    # prueba: CramionPlayer no hace falta compilarlo) con el PNG de la web.
    cramion_test(CramionBuildConfigTests LABELS rapidos editor
                 ARGS "$<TARGET_FILE:CramionBuildConfigTests>" "${CMAKE_SOURCE_DIR}/docs/img/icon.png")
    # Un APK real si hay Android SDK (con un libmain.so de relleno y el icono de la web).
    cramion_test(CramionAndroidBuildTests LABELS lentos editor TIMEOUT 900
                 ARGS "sin-libmain.so" "${CMAKE_SOURCE_DIR}/docs/img/icon.png")
    cramion_test(CramionTemplateTests LABELS lentos editor TIMEOUT 1800)      # juega cada plantilla con sus scripts

    # --- CramionUpdater ---
    cramion_test(CramionUpdateTests)

    # --- Con GPU y ventana invisible: nunca en la CI ---
    cramion_test(cramion_stereo_check NAME GpuStereoCheck LABELS gpu TIMEOUT 300)
    cramion_test(cramion_render_bench NAME GpuRenderBench LABELS gpu
                 ARGS --frames 30 --warmup 10 --lite TIMEOUT 300)

    # --- Fuzzing (CRAMION_FUZZ): cada objetivo con su corpus semilla unos
    # segundos, como prueba de humo (las pasadas largas, a mano). ---
    if(COMMAND cramion_register_fuzz_tests)
        cramion_register_fuzz_tests()
    endif()
endfunction()

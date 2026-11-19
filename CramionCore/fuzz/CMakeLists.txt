# Fuzzing con libFuzzer (CRAMION_FUZZ=ON, ROADMAP 3.6): un ejecutable por
# formato que llega de fuera del juego. Ver PRUEBAS.md.
#
#   cramion_fuzz_crwpack   paquetes .crwpack de la Store
#   cramion_fuzz_datapack  .datapack (y el contenedor .crpack que usan)
#   cramion_fuzz_scene     escenas .crscene / prefabs (JSON)
#   cramion_fuzz_crdata    assets importados .crdata (modelos y cielos)
#   cramion_fuzz_net       paquetes de red (valores, posiciones, sesion)
#
# Uso (en <build>/fuzz):
#   cramion_fuzz_scene --seeds=corpus/scene corpus/scene -max_total_time=300
# --seeds=<carpeta> (o CRAMION_FUZZ_SEEDS) escribe antes las semillas.

set(_fuzz_targets crwpack:FuzzCrwPack datapack:FuzzDataPack scene:FuzzScene crdata:FuzzCrData net:FuzzNet)
set(CRAMION_FUZZ_TARGETS "")
foreach(entry IN LISTS _fuzz_targets)
    string(REPLACE ":" ";" entry "${entry}")
    list(GET entry 0 name)
    list(GET entry 1 file)
    set(target cramion_fuzz_${name})
    set(source "${CMAKE_CURRENT_SOURCE_DIR}/${file}.cpp")
    add_executable(${target} "${source}")
    if(name STREQUAL "crwpack")
        # Solo zstd y JSON: enlaza en segundos.
        target_link_libraries(${target} PRIVATE Cramion::CrwPack)
    else()
        target_link_libraries(${target} PRIVATE Cramion::Core)
    endif()
    # CrData.h es interna de CramionAssets (src/asset).
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../src")
    target_compile_options(${target} PRIVATE -fsanitize=fuzzer -Wall -Wextra)
    target_link_options(${target} PRIVATE -fsanitize=fuzzer)
    set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/fuzz")
    cramion_copy_sanitizer_runtime(${target})
    list(APPEND CRAMION_FUZZ_TARGETS ${target})
endforeach()
set(CRAMION_FUZZ_TARGETS "${CRAMION_FUZZ_TARGETS}" CACHE INTERNAL "Objetivos de fuzzing")

add_custom_target(cramion_fuzz DEPENDS ${CRAMION_FUZZ_TARGETS})

# Prueba de humo en CTest (etiqueta fuzz): semillas + 20 s por objetivo.
# Las pasadas largas se lanzan a mano (PRUEBAS.md).
function(cramion_register_fuzz_tests)
    foreach(target IN LISTS CRAMION_FUZZ_TARGETS)
        string(REPLACE "cramion_fuzz_" "" name "${target}")
        set(corpus "${CMAKE_BINARY_DIR}/fuzz/corpus/${name}")
        file(MAKE_DIRECTORY "${corpus}")
        add_test(NAME Fuzz_${name}
                 COMMAND ${target} "--seeds=${corpus}" "${corpus}" -max_total_time=20 -rss_limit_mb=4096
                         -timeout=30 "-artifact_prefix=${CMAKE_BINARY_DIR}/fuzz/fallo_${name}_"
                 WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/fuzz")
        set_tests_properties(Fuzz_${name} PROPERTIES LABELS "fuzz" TIMEOUT 300)
        if(CRAMION_SANITIZER_RUNTIME_DIR)
            set_property(TEST Fuzz_${name} APPEND PROPERTY ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:${CRAMION_SANITIZER_RUNTIME_DIR}")
        endif()
        set_property(TEST Fuzz_${name} APPEND PROPERTY ENVIRONMENT
            "ASAN_OPTIONS=detect_leaks=0:windows_hook_rtl_allocators=0" "CRAMION_TESTS_SILENT=1"
            "UBSAN_OPTIONS=print_stacktrace=1:suppressions='${CMAKE_SOURCE_DIR}/cmake/ubsan.supp'")
    endforeach()
endfunction()

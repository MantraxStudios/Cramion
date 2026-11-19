# Sanitizers y fuzzing (ROADMAP 3.6). Apagados por defecto: sin las opciones
# el motor se compila exactamente igual que antes.
#
#   -DCRAMION_SANITIZE=ON   ASan + UBSan en todo el motor y sus dependencias
#                           (preset clang-asan, carpeta build-asan). Con
#                           ctest -L rapidos se pasan las pruebas con ellos.
#   -DCRAMION_FUZZ=ON       objetivos de libFuzzer (CramionCore/fuzz) con la
#                           cobertura de libFuzzer en todo el codigo; implica
#                           CRAMION_SANITIZE (preset clang-fuzz, build-fuzz).
#
# Solo con Clang (el compilador del motor). En Windows, ASan necesita el CRT
# dinamico de Release (/MD): se usa con CMAKE_BUILD_TYPE=Release o
# RelWithDebInfo, nunca Debug. La DLL de ASan (clang_rt.asan_dynamic-x86_64.dll)
# se busca en el PATH al ejecutar: CTest lo pone solo (CramionTests.cmake) y los
# objetivos de fuzzing la llevan copiada al lado.

include_guard(GLOBAL)

option(CRAMION_SANITIZE "ASan + UBSan en todo el motor (Clang; para pruebas)" OFF)
option(CRAMION_FUZZ "Objetivos de fuzzing con libFuzzer (Clang; implica CRAMION_SANITIZE)" OFF)

set(CRAMION_SANITIZER_RUNTIME_DIR "")

if(NOT CRAMION_SANITIZE AND NOT CRAMION_FUZZ)
    return()
endif()

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "CRAMION_SANITIZE / CRAMION_FUZZ necesitan Clang (CMAKE_CXX_COMPILER=clang++)")
endif()
if(WIN32 AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(FATAL_ERROR "ASan en Windows necesita el CRT de Release: usa CMAKE_BUILD_TYPE=Release o RelWithDebInfo")
endif()
if(CRAMION_FUZZ AND NOT CRAMION_SANITIZE)
    set(CRAMION_SANITIZE ON CACHE BOOL "ASan + UBSan en todo el motor (Clang; para pruebas)" FORCE)
endif()

# ASan + UBSan. Del grupo "undefined" Clang quita solo lo que no existe en la
# plataforma (vptr sin RTTI). -fno-sanitize-recover no: UBSan sigue tras el
# primer aviso y UBSAN_OPTIONS=halt_on_error=1 lo convierte en fallo en CTest.
set(_cramion_san "-fsanitize=address,undefined")
# Lo que UBSan da por error y no lo es en este motor:
#  - function: llamadas por puntero a funciones C de terceros (miniaudio, enet)
#    con prototipos compatibles pero no identicos.
set(_cramion_san_off "-fno-sanitize=function")
add_compile_options(
    "$<$<COMPILE_LANGUAGE:C,CXX>:${_cramion_san}>"
    "$<$<COMPILE_LANGUAGE:C,CXX>:${_cramion_san_off}>"
    "$<$<COMPILE_LANGUAGE:C,CXX>:-fno-omit-frame-pointer>"
    "$<$<COMPILE_LANGUAGE:C,CXX>:-g>")
add_link_options("${_cramion_san}")
add_compile_definitions(CRAMION_SANITIZE=1)

if(CRAMION_FUZZ)
    # La cobertura de libFuzzer en todo (sin su main): los objetivos de
    # CramionCore/fuzz enlazan con -fsanitize=fuzzer.
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:-fsanitize=fuzzer-no-link>")
    add_compile_definitions(CRAMION_FUZZ=1)
    # La libFuzzer que trae LLVM para Windows (clang_rt.fuzzer-x86_64.lib)
    # esta compilada con el CRT estatico (/MT): todo el build de fuzzing va
    # con /MT para que enlace (ASan lo admite con su "static runtime thunk").
    # Tampoco lleva las anotaciones de ASan en std::string/std::vector de la
    # STL de MSVC (/failifmismatch annotate_string): se quitan en todo el build
    # de fuzzing (se pierde solo la deteccion de "container-overflow").
    # La lib de NGX (DLSS) del SDK de NVIDIA es /MD: sin DLSS en este build.
    if(WIN32)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
        add_compile_definitions(_DISABLE_STRING_ANNOTATION _DISABLE_VECTOR_ANNOTATION)
        set(CRAMION_DLSS OFF CACHE BOOL "NVIDIA DLSS 4 (NGX)" FORCE)
    endif()
endif()

# Carpeta de los runtimes de Clang (la DLL de ASan en Windows).
execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -print-resource-dir
                OUTPUT_VARIABLE _cramion_res OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(WIN32 AND _cramion_res AND EXISTS "${_cramion_res}/lib/windows")
    set(CRAMION_SANITIZER_RUNTIME_DIR "${_cramion_res}/lib/windows")
    file(TO_CMAKE_PATH "${CRAMION_SANITIZER_RUNTIME_DIR}" CRAMION_SANITIZER_RUNTIME_DIR)
endif()
message(STATUS "Cramion: sanitizers ASan + UBSan (runtimes: ${CRAMION_SANITIZER_RUNTIME_DIR})")
if(CRAMION_FUZZ)
    message(STATUS "Cramion: fuzzing con libFuzzer (CramionCore/fuzz)")
endif()

# cramion_copy_sanitizer_runtime(<objetivo>): la DLL de ASan junto al .exe
# (para ejecutarlo fuera de CTest, como los objetivos de fuzzing).
function(cramion_copy_sanitizer_runtime target)
    if(WIN32 AND CRAMION_SANITIZER_RUNTIME_DIR
       AND EXISTS "${CRAMION_SANITIZER_RUNTIME_DIR}/clang_rt.asan_dynamic-x86_64.dll")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${CRAMION_SANITIZER_RUNTIME_DIR}/clang_rt.asan_dynamic-x86_64.dll"
                    "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    endif()
endfunction()

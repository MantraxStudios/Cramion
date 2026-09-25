# Empaqueta el motor compilado en un zip listo para descargar:
#
#   Cramion-<version>-win64/
#     CramionEditor.exe, CramionPlayer.exe, cramion.exe
#     shaders/, editor_icons/, player_banner.png
#     msvcp140.dll, vcruntime140*.dll   (runtime de C++, si se encuentra)
#     LICENSE, README.md, LEEME.txt
#
# Lo llama el target cramion_package:
#   cmake -DBIN_DIR=... -DSOURCE_DIR=... -DVERSION=... -P PackageRelease.cmake
#
# El zip queda en <BIN_DIR>/Cramion-win64.zip: nombre sin version para que el
# enlace de descarga de la web (releases/latest/download/...) no cambie.
# Las escenas de demostracion no se incluyen: pesan mucho y sus licencias no
# permiten redistribuirlas.

foreach(var BIN_DIR SOURCE_DIR VERSION)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "PackageRelease.cmake: falta -D${var}=")
    endif()
endforeach()

set(name "Cramion-${VERSION}-win64")
set(stage "${BIN_DIR}/package/${name}")
set(zip "${BIN_DIR}/Cramion-win64.zip")

file(REMOVE_RECURSE "${BIN_DIR}/package")
file(MAKE_DIRECTORY "${stage}")

foreach(exe CramionEditor.exe CramionPlayer.exe cramion.exe)
    if(NOT EXISTS "${BIN_DIR}/${exe}")
        message(FATAL_ERROR "Falta ${BIN_DIR}/${exe}: compila antes el proyecto.")
    endif()
    file(COPY "${BIN_DIR}/${exe}" DESTINATION "${stage}")
endforeach()
file(COPY "${BIN_DIR}/shaders" "${BIN_DIR}/editor_icons" "${BIN_DIR}/player_banner.png" DESTINATION "${stage}")
file(COPY "${SOURCE_DIR}/LICENSE" "${SOURCE_DIR}/README.md" DESTINATION "${stage}")

# Runtime de Visual C++ junto a los .exe (despliegue local, permitido por la
# licencia del redistribuible). El editor copia los .dll de su carpeta en cada
# juego exportado, asi que tambien los llevan los juegos.
file(GLOB crt_dirs
    "$ENV{ProgramFiles}/Microsoft Visual Studio/*/*/VC/Redist/MSVC/*/x64/Microsoft.VC14*.CRT"
    "$ENV{ProgramFiles\(x86\)}/Microsoft Visual Studio/*/*/VC/Redist/MSVC/*/x64/Microsoft.VC14*.CRT")
list(SORT crt_dirs COMPARE NATURAL ORDER DESCENDING)
set(crt_found FALSE)
foreach(dir IN LISTS crt_dirs)
    if(EXISTS "${dir}/msvcp140.dll" AND EXISTS "${dir}/vcruntime140.dll" AND EXISTS "${dir}/vcruntime140_1.dll")
        file(COPY "${dir}/msvcp140.dll" "${dir}/vcruntime140.dll" "${dir}/vcruntime140_1.dll" DESTINATION "${stage}")
        message(STATUS "Runtime de C++: ${dir}")
        set(crt_found TRUE)
        break()
    endif()
endforeach()
if(NOT crt_found)
    message(WARNING "No se encontro el redistribuible de Visual C++: el zip necesitara que el usuario lo instale.")
endif()

file(WRITE "${stage}/LEEME.txt"
"Cramion ${VERSION} para Windows x64
=====================================

CramionEditor.exe   Editor de escenas: crea un proyecto, edita y pulsa Play.
CramionPlayer.exe   Ejecutable de los juegos exportados (Archivo > Exportar juego).
cramion.exe         Demo de render. Necesita las escenas de demostracion en
                    assets/ (no incluidas; ver README.md).

Requisitos
----------
- Windows 10/11 x64.
- GPU y controlador con Vulkan 1.3 (el controlador instala vulkan-1.dll).
- Si falta MSVCP140.dll o VCRUNTIME140.dll, instala el redistribuible de
  Visual C++ x64: https://aka.ms/vs/17/release/vc_redist.x64.exe

No separes los .exe de las carpetas shaders/ y editor_icons/.

Documentacion de scripting: docs/scripting.html en el repositorio.
")

file(REMOVE "${zip}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar cf "${zip}" --format=zip "${name}"
    WORKING_DIRECTORY "${BIN_DIR}/package"
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "No se pudo crear ${zip}")
endif()
file(SIZE "${zip}" size)
math(EXPR size_mb "${size} / 1048576")
message(STATUS "Paquete listo: ${zip} (${size_mb} MB)")

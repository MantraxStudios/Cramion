# Empaqueta el motor compilado en un zip listo para descargar:
#
#   Cramion-<version>-win64/
#     CramionEditor.exe, CramionPlayer.exe, cramion.exe, CramionMcp.exe (puente MCP),
#     CramionUpdater.exe (actualizador)
#     shaders/ (con source/ de los .crshader), editor_icons/, player_banner.png
#     android/ (libmain.so por ABI: Exportar a Android)
#     shaderc_shared.dll                  (compila los shaders propios)
#     msvcp140.dll, vcruntime140*.dll   (runtime de C++, si se encuentra)
#     LICENSE, TRADEMARK.md, THIRD_PARTY_NOTICES.md, README.md, CHANGELOG.md, LEEME.txt, docs/ (web y referencia de scripting)
#
# Lo llama el target cramion_package:
#   cmake -DBIN_DIR=... -DSOURCE_DIR=... -DVERSION=... -P PackageRelease.cmake
#
# Al lado, Cramion-win64.zip.sha256 (subelo tambien a la release: el
# actualizador comprueba el zip con el antes de instalar).
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

foreach(exe CramionEditor.exe CramionPlayer.exe cramion.exe CramionMcp.exe CramionUpdater.exe)
    if(NOT EXISTS "${BIN_DIR}/${exe}")
        message(FATAL_ERROR "Falta ${BIN_DIR}/${exe}: compila antes el proyecto.")
    endif()
    file(COPY "${BIN_DIR}/${exe}" DESTINATION "${stage}")
endforeach()
file(COPY "${BIN_DIR}/shaders" "${BIN_DIR}/editor_icons" "${BIN_DIR}/player_banner.png" DESTINATION "${stage}")
# Runtime de Android (exportar APK/AAB), si se compilo con el NDK.
if(EXISTS "${BIN_DIR}/android/arm64-v8a/libmain.so")
    # Sin bundletool.jar (32 MB): el editor lo descarga el primer AAB.
    file(COPY "${BIN_DIR}/android" DESTINATION "${stage}" PATTERN "*.jar" EXCLUDE)
else()
    message(WARNING "Falta android/arm64-v8a/libmain.so: el paquete no podra exportar a Android.")
endif()
# Compilador de los shaders de superficie del usuario (.crshader).
if(EXISTS "${BIN_DIR}/shaderc_shared.dll")
    file(COPY "${BIN_DIR}/shaderc_shared.dll" DESTINATION "${stage}")
else()
    message(WARNING "Falta shaderc_shared.dll: los shaders propios no compilaran en el paquete.")
endif()
# Escaladores de los fabricantes (se cargan al elegirlos; sin ellos, TAA) y
# sus licencias: AMD FSR 3.1 (MIT) y el runtime de NVIDIA DLSS (redistribuible
# con la aplicacion segun la licencia del SDK).
foreach(dll amd_fidelityfx_vk.dll nvngx_dlss.dll)
    if(EXISTS "${BIN_DIR}/${dll}")
        file(COPY "${BIN_DIR}/${dll}" DESTINATION "${stage}")
    else()
        message(WARNING "Falta ${dll}: ese escalador no estara disponible en el paquete.")
    endif()
endforeach()
file(MAKE_DIRECTORY "${stage}/licencias")
if(EXISTS "${BIN_DIR}/_deps/fidelityfx/LICENSE.txt")
    file(COPY_FILE "${BIN_DIR}/_deps/fidelityfx/LICENSE.txt" "${stage}/licencias/AMD-FidelityFX-LICENSE.txt")
endif()
if(EXISTS "${BIN_DIR}/_deps/dlss/LICENSE.txt")
    file(COPY_FILE "${BIN_DIR}/_deps/dlss/LICENSE.txt" "${stage}/licencias/NVIDIA-DLSS-LICENSE.txt")
endif()
file(COPY "${SOURCE_DIR}/LICENSE" "${SOURCE_DIR}/TRADEMARK.md" "${SOURCE_DIR}/THIRD_PARTY_NOTICES.md" "${SOURCE_DIR}/README.md" DESTINATION "${stage}")
if(EXISTS "${SOURCE_DIR}/CHANGELOG.md")
    file(COPY "${SOURCE_DIR}/CHANGELOG.md" DESTINATION "${stage}")
endif()
# La documentacion viaja con el motor (se abre sin conexion salvo el estilo).
# Solo lo que la web necesita (html, manual, imagenes): sin el video de fondo,
# el docs.zip para subirla ni restos que acaben en la carpeta (una copia de
# 16 MB de docs.zip con otro nombre llego a colarse en el zip).
file(MAKE_DIRECTORY "${stage}/docs")
file(GLOB docs_pages "${SOURCE_DIR}/docs/*.html" "${SOURCE_DIR}/docs/.nojekyll")
file(COPY ${docs_pages} DESTINATION "${stage}/docs")
foreach(folder manual img)
    if(EXISTS "${SOURCE_DIR}/docs/${folder}")
        file(COPY "${SOURCE_DIR}/docs/${folder}" DESTINATION "${stage}/docs" PATTERN "*.mp4" EXCLUDE)
    endif()
endforeach()

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
CramionUpdater.exe  Actualizador: busca, descarga e instala la version nueva
                    (el editor avisa solo y guarda todo antes).
cramion.exe         Demo de render. Necesita las escenas de demostracion en
                    assets/ (no incluidas; ver README.md).

Requisitos
----------
- Windows 10/11 x64.
- GPU y controlador con Vulkan 1.3 (el controlador instala vulkan-1.dll).
- Si falta MSVCP140.dll o VCRUNTIME140.dll, instala el redistribuible de
  Visual C++ x64: https://aka.ms/vs/17/release/vc_redist.x64.exe

No separes los .exe de las carpetas shaders/ y editor_icons/.

Documentacion: docs/manual/index.html (como programar en Lua: API completa y
ejemplos) y docs/index.html. Para empezar, crea un proyecto desde el Hub con
una plantilla (Tercera persona, IA y navegacion o Mundo de bloques) y dale a
Play.

Licencia
--------
Gratis para hacer juegos y venderlos, sin regalias. No se permite vender,
revender ni resubir el motor o el editor (ver LICENSE y TRADEMARK.md).
La unica descarga oficial es https://cramion.mantraxtools.store
El codigo fuente se entrega solo con licencia de codigo fuente:
tupapienrakion1234@gmail.com
Librerias de terceros: THIRD_PARTY_NOTICES.md y la carpeta licencias/.
")

file(REMOVE "${zip}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar cf "${zip}" --format=zip "${name}"
    WORKING_DIRECTORY "${BIN_DIR}/package"
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "No se pudo crear ${zip}")
endif()
# SHA-256 del zip: se sube a la release junto a el (Cramion-win64.zip.sha256)
# y el actualizador lo comprueba antes de instalar.
file(SHA256 "${zip}" zip_hash)
file(WRITE "${zip}.sha256" "${zip_hash}  Cramion-win64.zip\n")
message(STATUS "SHA-256: ${zip_hash}")
file(SIZE "${zip}" size)
math(EXPR size_mb "${size} / 1048576")
message(STATUS "Paquete listo: ${zip} (${size_mb} MB)")

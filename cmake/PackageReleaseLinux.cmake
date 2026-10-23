# Empaqueta el motor compilado para Linux en un tar.gz listo para descargar:
#
#   Cramion-<version>-linux64/
#     CramionEditor, CramionPlayer, CramionServer, CramionMcp (puente MCP),
#     CramionScriptHost (scripts de C++), crwpack (paquetes .crwpack de la Cramion Store)
#     shaders/, editor_icons/, sdk/, player_banner.png, cramion.png
#     LICENSE, TRADEMARK.md, THIRD_PARTY_NOTICES.md, README.md, CHANGELOG.md, LEEME.txt, docs/
#
# Lo llama el target cramion_package del preset linux-release:
#   cmake -DBIN_DIR=... -DSOURCE_DIR=... -DVERSION=... -P PackageReleaseLinux.cmake
#
# El tar queda en <BIN_DIR>/Cramion-linux64.tar.gz (sin version en el nombre,
# como el zip de Windows, para que el enlace releases/latest/download/... no
# cambie) y al lado Cramion-linux64.tar.gz.sha256. Es un tar y no un zip para
# que los ejecutables conserven el permiso de ejecucion.

foreach(var BIN_DIR SOURCE_DIR VERSION)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "PackageReleaseLinux.cmake: falta -D${var}=")
    endif()
endforeach()

set(name "Cramion-${VERSION}-linux64")
set(stage "${BIN_DIR}/package/${name}")
set(archive "${BIN_DIR}/Cramion-linux64.tar.gz")

file(REMOVE_RECURSE "${BIN_DIR}/package")
file(MAKE_DIRECTORY "${stage}")

foreach(exe CramionEditor CramionPlayer CramionServer CramionMcp CramionScriptHost crwpack)
    if(NOT EXISTS "${BIN_DIR}/${exe}")
        message(FATAL_ERROR "Falta ${BIN_DIR}/${exe}: compila antes el proyecto.")
    endif()
    file(COPY "${BIN_DIR}/${exe}" DESTINATION "${stage}"
         FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
endforeach()
file(COPY "${BIN_DIR}/shaders" "${BIN_DIR}/editor_icons" "${BIN_DIR}/sdk" "${BIN_DIR}/player_banner.png" DESTINATION "${stage}")
if(EXISTS "${SOURCE_DIR}/assets/icon.png")
    file(COPY_FILE "${SOURCE_DIR}/assets/icon.png" "${stage}/cramion.png")
endif()
file(COPY "${SOURCE_DIR}/LICENSE" "${SOURCE_DIR}/TRADEMARK.md" "${SOURCE_DIR}/THIRD_PARTY_NOTICES.md" "${SOURCE_DIR}/README.md" DESTINATION "${stage}")
if(EXISTS "${SOURCE_DIR}/CHANGELOG.md")
    file(COPY "${SOURCE_DIR}/CHANGELOG.md" DESTINATION "${stage}")
endif()
# La documentacion, como en el zip de Windows (sin el video ni docs.zip).
file(MAKE_DIRECTORY "${stage}/docs")
file(GLOB docs_pages "${SOURCE_DIR}/docs/*.html" "${SOURCE_DIR}/docs/.nojekyll")
file(COPY ${docs_pages} DESTINATION "${stage}/docs")
foreach(folder manual img store)
    if(EXISTS "${SOURCE_DIR}/docs/${folder}")
        file(COPY "${SOURCE_DIR}/docs/${folder}" DESTINATION "${stage}/docs" PATTERN "*.mp4" EXCLUDE)
    endif()
endforeach()

file(WRITE "${stage}/LEEME.txt"
"Cramion ${VERSION} para Linux x64
===================================

CramionEditor       Editor de escenas: crea un proyecto, edita y pulsa Play.
CramionPlayer       Ejecutable de los juegos exportados (Archivo > Exportar juego).
CramionServer       Servidor dedicado de los juegos en red.
CramionScriptHost   Proceso de los scripts de C++ (lo lanza el editor).
crwpack             Paquetes .crwpack de la Cramion Store desde la consola:
                    crwpack pack|unpack|list|info|verify.

Para empezar:  ./CramionEditor

Requisitos
----------
- Linux x64 con X11 o XWayland.
- GPU y controlador con Vulkan 1.3 (libvulkan1 / vulkan-loader).
- zenity (dialogos de abrir y guardar) y curl (Store y novedades).
- Scripts de C++: clang++ instalado (clangd opcional, para el autocompletado).
- Shaders propios (.crshader): libshaderc (paquete libshaderc1 o shaderc).

No separes los ejecutables de las carpetas shaders/, editor_icons/ y sdk/.
En Linux el motor no se actualiza solo: descarga el tar nuevo de la web.

Documentacion: docs/manual/index.html y docs/index.html.

Licencia
--------
Gratis para hacer juegos y venderlos, sin regalias. No se permite vender,
revender ni resubir el motor o el editor (ver LICENSE y TRADEMARK.md).
La unica descarga oficial es https://cramion.mantraxtools.store
El codigo fuente se entrega solo con licencia de codigo fuente:
tupapienrakion1234@gmail.com
Librerias de terceros: THIRD_PARTY_NOTICES.md.
")

file(REMOVE "${archive}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar czf "${archive}" "${name}"
    WORKING_DIRECTORY "${BIN_DIR}/package"
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "No se pudo crear ${archive}")
endif()
file(SHA256 "${archive}" archive_hash)
file(WRITE "${archive}.sha256" "${archive_hash}  Cramion-linux64.tar.gz\n")
message(STATUS "SHA-256: ${archive_hash}")
file(SIZE "${archive}" size)
math(EXPR size_mb "${size} / 1048576")
message(STATUS "Paquete listo: ${archive} (${size_mb} MB)")

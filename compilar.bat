```bat
@echo off
rem ============================================================
rem Cramion Engine - Compilacion con Clang + Ninja
rem
rem compilar.bat            -> Usa todos los procesadores logicos
rem compilar.bat 4          -> Usa 4 hilos
rem compilar.bat 2          -> Usa 2 hilos
rem compilar.bat limpio     -> Limpia y recompila con todos
rem compilar.bat 4 limpio   -> Limpia y recompila con 4 hilos
rem
rem Muestra la salida en consola y guarda el log completo.
rem ============================================================

setlocal
cd /d "%~dp0"

set "JOBS=%~1"
set "CLEAN=%~2"
set "BUILD_DIR=build-release"
set "PRESET=clang-ninja-release"
set "LOG_FILE=%BUILD_DIR%\compilar.log"

rem Si el primer argumento es "limpio", activar limpieza.
if /i "%~1"=="limpio" (
    set "CLEAN=limpio"
    set "JOBS="
)

rem Sin numero de hilos, detectar todos los procesadores logicos.
if "%JOBS%"=="" (
    for /f %%C in ('powershell -NoProfile -Command "[Environment]::ProcessorCount"') do set "JOBS=%%C"
)

if "%JOBS%"=="" set "JOBS=4"

rem Comprobar CMake.
where cmake >nul 2>nul
if errorlevel 1 (
    echo [ERROR] No se encuentra CMake en el PATH.
    goto fin_error
)

rem Comprobar PowerShell.
where powershell >nul 2>nul
if errorlevel 1 (
    echo [ERROR] No se encuentra PowerShell en el PATH.
    goto fin_error
)

rem Configurar el proyecto si no existe la caché.
if not exist "%BUILD_DIR%\CMakeCache.txt" (
    echo.
    echo [CONFIGURACION] Aplicando preset %PRESET%...
    cmake --preset "%PRESET%"
    if errorlevel 1 goto fin_error
)

rem Crear directorio si hace falta.
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

rem Limpiar si se solicita.
if /i "%CLEAN%"=="limpio" (
    echo.
    echo [LIMPIEZA] Eliminando archivos compilados...
    cmake --build "%BUILD_DIR%" --target clean
    if errorlevel 1 goto fin_error
)

echo.
echo ============================================================
echo                 CRAMION ENGINE
echo ============================================================
echo Directorio : %BUILD_DIR%
echo Procesadores/hilos: %JOBS%
echo Registro   : %LOG_FILE%
echo ============================================================
echo.

set "START=%TIME%"

rem Compilar, mostrar la salida y guardar el registro completo.
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "& { $ErrorActionPreference = 'Continue'; & cmake --build '%BUILD_DIR%' --parallel %JOBS% -- '-k 0' 2>&1 | Tee-Object -FilePath '%LOG_FILE%'; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE } }"

set "RESULT=%ERRORLEVEL%"

echo.
echo ============================================================

if not "%RESULT%"=="0" (
    echo [ERROR] LA COMPILACION FALLO.
    echo.
    echo ===== ERRORES DETECTADOS =====
    findstr /n /i /c:"error:" /c:"FAILED:" "%LOG_FILE%"
    echo.
    echo Registro completo: %LOG_FILE%
    goto fin_error
)

echo [OK] COMPILACION COMPLETADA.
echo Inicio: %START%
echo Fin:    %TIME%
echo Registro: %LOG_FILE%
echo ============================================================
pause
exit /b 0

:fin_error
echo.
echo [ERROR] Proceso terminado con errores.
pause
exit /b 1
```
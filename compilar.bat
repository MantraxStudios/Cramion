@echo off
rem Compila Cramion (Clang + Ninja).
rem   compilar.bat            -> Release con 4 hilos
rem   compilar.bat 2          -> Release con 2 hilos (si el PC se traba)
rem   compilar.bat 4 limpio   -> borra lo compilado y compila todo de nuevo
rem 16 GB de RAM: no pasar de -j 4 ni compilar dos cosas a la vez.

setlocal
cd /d "%~dp0"

set JOBS=%1
if "%JOBS%"=="" set JOBS=4
set BUILD_DIR=build-release
set PRESET=clang-ninja-release

where cmake >nul 2>nul || (echo [ERROR] No se encuentra cmake en el PATH. & goto fin_error)

if /i "%2"=="limpio" (
    echo Limpiando %BUILD_DIR%...
    cmake --build %BUILD_DIR% --target clean
)

rem Sin cache (o de otra carpeta): se configura con el preset.
if not exist "%BUILD_DIR%\CMakeCache.txt" (
    echo Configurando %PRESET%...
    cmake --preset %PRESET% || goto fin_error
)

echo Compilando %BUILD_DIR% con %JOBS% hilos...
echo Registro: %BUILD_DIR%\compilar.log
set START=%TIME%
cmake --build %BUILD_DIR% -j %JOBS% -- -k 0 > "%BUILD_DIR%\compilar.log" 2>&1
set RESULT=%ERRORLEVEL%

if not "%RESULT%"=="0" (
    echo.
    echo ===== ERRORES =====
    findstr /n /i /c:"error" /c:"FAILED" "%BUILD_DIR%\compilar.log"
    echo.
    echo [ERROR] La compilacion fallo. Registro completo: %BUILD_DIR%\compilar.log
    goto fin_error
)

echo.
echo [OK] Compilado. Empezo %START%, termino %TIME%.
pause
exit /b 0

:fin_error
pause
exit /b 1

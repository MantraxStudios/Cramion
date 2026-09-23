cmake_minimum_required(VERSION 3.21)

project(Cramion
    VERSION 0.1.0
    DESCRIPTION "Motor con librería DirectX 12 CramionDM (CMake + Clang + Ninja)"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Exportar compile_commands.json para clangd / herramientas
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Librería estática propia basada en DirectX 12
add_subdirectory(CramionDM)

# Ejecutable de ejemplo / arranque
add_executable(cramion
    src/main.cpp
)

target_link_libraries(cramion PRIVATE Cramion::DM)

target_compile_options(cramion PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

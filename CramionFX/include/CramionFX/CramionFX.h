#pragma once

// CramionFX: renderizador Vulkan del motor Cramion (diferido PBR, IBL, sombras
// en cascada, GI y reflejos en pantalla o por trazado de rayos, lluvia,
// vidrio, post-proceso). Un solo include para usarlo:
//
//   #include <CramionFX/CramionFX.h>
//
// y en CMake:
//
//   add_subdirectory(CramionFX)
//   target_link_libraries(mi_juego PRIVATE Cramion::FX)
//   cramionfx_deploy(mi_juego)   # shaders SPIR-V junto al ejecutable
//
// La ventana y la entrada son de CramionDM (se enlaza con CramionFX).

#include "CramionFX/core/Clock.h"             // Tiempo por frame
#include "CramionFX/core/Math.h"              // Vec2/3/4, Mat4 y utilidades
#include "CramionFX/scene/Camera.h"           // Camara en primera persona
#include "CramionFX/scene/Light.h"            // Sol, luces puntuales y focos
#include "CramionFX/scene/Scene.h"            // Modelos, actores, luces y camara
#include "CramionFX/vk/VulkanRenderer.h"      // El renderizador
#include "CramionFX/vk/VulkanShader.h"        // Donde se buscan los shaders

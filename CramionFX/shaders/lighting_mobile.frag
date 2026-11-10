#version 450
#extension GL_GOOGLE_include_directive : require

// Iluminacion del movil en Baja (LightingPass::mobilePipeline): solo mobileMain() de
// lighting_body.glsl; el shader completo no entra (menos registros en una Mali).
#define CRAMION_LIGHTING_MOBILE 1
#include "lighting_body.glsl"

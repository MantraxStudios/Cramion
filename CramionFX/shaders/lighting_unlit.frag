#version 450
#extension GL_GOOGLE_include_directive : require

// UNLIT: iluminacion del modo Unlit del juego (gama ultra baja; LightingPass::unlitPipeline): solo
// unlitMain() de lighting_body.glsl (color del material, ambiente del cielo, Lambert del sol sin
// sombras, emision y niebla). El shader completo no entra (menos registros en una GPU de movil).
#define CRAMION_LIGHTING_MOBILE 1
#define CRAMION_LIGHTING_UNLIT 1
#include "lighting_body.glsl"

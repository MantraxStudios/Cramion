#version 460
#extension GL_EXT_ray_tracing : require

// Miss del path tracing con pipeline de rayos. No se ejecuta nunca: el raygen
// usa hit objects y mira si fallo con hitObjectIsHitEXT (el cielo lo pone el).
// La tabla de shaders necesita uno.

void main() {}

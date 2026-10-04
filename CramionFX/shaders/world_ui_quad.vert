#version 450

// UI en el mundo: el panel de un Canvas en modo Mundo (dos triangulos sin
// buffer de vertices). transform lleva el cuadrado unidad (-0.5..0.5, de cara
// a +Z) al espacio de recorte: vista-proyeccion * mundo * tamano en metros.

layout(push_constant) uniform Push {
    mat4 transform;
    vec4 params;  // x = opacidad
} push;

layout(location = 0) out vec2 v_uv;

const vec2 kCorners[6] = vec2[](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
                                vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));

void main() {
    vec2 corner = kCorners[gl_VertexIndex];
    // La textura tiene la fila de arriba primero (como la UI): v = 0 arriba.
    v_uv = vec2(corner.x, 1.0 - corner.y);
    gl_Position = push.transform * vec4(corner - 0.5, 0.0, 1.0);
}

#version 450

// Geometria de ayuda en 3D (gizmos, alcance de luces...): OverlayPass. Los
// vertices llegan ya en espacio de recorte, calculados en la CPU: las lineas
// se expanden alli a quads de grosor constante en pixeles.

layout(location = 0) in vec4 in_clip;
layout(location = 1) in vec4 in_color;  // RGBA8 (UNORM)
layout(location = 2) in float in_edge;  // pixeles desde el eje de la linea (0 en triangulos)

layout(location = 0) out vec4 v_color;
layout(location = 1) out float v_edge;

void main() {
    v_color = in_color;
    v_edge = in_edge;
    gl_Position = in_clip;
}

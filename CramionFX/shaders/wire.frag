#version 460

// Lineas de las mallas encima de la imagen iluminada (vista Escena en modo
// "Lit + Wireframe"). Negro semitransparente: se ve igual con cualquier
// exposicion. El alfa de la imagen HDR (distancia de la superficie) no se toca
// (ver la mezcla de la pipeline).

layout(location = 0) out vec4 out_color;

void main() {
    out_color = vec4(0.0, 0.0, 0.0, 0.55);
}

#version 450

// Triangulo a pantalla completa generado sin buffer de vertices: se dibuja con
// draw(3, 1, 0, 0) y cubre todo el viewport.

layout(location = 0) out vec2 v_uv;

void main() {
    // (0,0), (2,0), (0,2) en UV -> un triangulo que envuelve el cuadrado [0,1].
    v_uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(v_uv * 2.0 - 1.0, 0.0, 1.0);
}

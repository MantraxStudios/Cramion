#version 450

// Sombras de los modelos con recorte por alfa: lo que el G-buffer descarta
// (hojas, rejas, pelo) tampoco debe proyectar sombra. Sin esto cada hoja
// haria la sombra de su rectangulo entero.

layout(set = 1, binding = 0) uniform sampler2D albedo_map;

layout(location = 0) in vec2 v_uv;

void main() {
    if (texture(albedo_map, v_uv).a < 0.5) {
        discard;
    }
}

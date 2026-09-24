#version 450

// Particulas: disco de borde suave (mas denso en el centro), mezclado sobre
// la imagen HDR. Las aditivas suman su color * opacidad; las transparentes
// se mezclan por alfa.

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;

layout(location = 0) out vec4 out_color;

void main() {
    float r2 = dot(v_uv, v_uv);
    if (r2 >= 1.0) {
        discard;
    }
    float falloff = 1.0 - r2;
    float alpha = v_color.a * falloff * falloff;
    if (alpha <= 0.002) {
        discard;
    }
    out_color = vec4(v_color.rgb, alpha);
}

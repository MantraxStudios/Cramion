#version 450

// Precipitacion: forma de cada particula dentro de su quad (v_uv -1..1).

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 2) flat in int v_mode;
layout(location = 3) in float v_age;

layout(location = 0) out vec4 out_color;

void main() {
    float alpha;
    if (v_mode == 0 || v_mode == 2) {
        // Gota con estela: fina en el centro, se apaga en las puntas.
        float across = 1.0 - v_uv.x * v_uv.x;
        float along = 1.0 - pow(abs(v_uv.y), 3.0);
        alpha = across * along;
    } else if (v_mode == 1) {
        // Copo: disco suave.
        float r2 = dot(v_uv, v_uv);
        if (r2 >= 1.0) discard;
        alpha = (1.0 - r2) * (1.0 - r2 * 0.5);
    } else if (v_mode == 3) {
        // Salpicadura: corona que se abre (arco que sube por los lados).
        vec2 p = vec2(v_uv.x, v_uv.y * 0.5 + 0.5);  // y: 0 suelo .. 1 arriba
        float crown = abs(p.x) * 0.9;
        float ring = 1.0 - smoothstep(0.0, 0.22, abs(p.y - (1.0 - crown * crown) * (0.4 + 0.6 * v_age)));
        float rim = smoothstep(1.0, 0.75, abs(p.x));
        alpha = ring * rim * smoothstep(0.0, 0.08, p.y + 0.02);
    } else {
        // Rayo: nucleo brillante con un halo.
        float x = abs(v_uv.x);
        float core = exp(-x * x * 18.0);
        float glow = exp(-x * x * 3.0) * 0.35;
        out_color = vec4(v_color.rgb * (core + glow), 0.0);
        return;
    }
    alpha *= v_color.a;
    if (alpha <= 0.002) discard;
    out_color = vec4(v_color.rgb, alpha);
}

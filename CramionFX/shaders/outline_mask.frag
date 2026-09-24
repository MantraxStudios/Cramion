#version 450

// Mascara del contorno de seleccion: cada objeto seleccionado se dibuja dos
// veces con skinned.vert (la misma cuenta que el G-buffer, asi que la
// profundidad sale identica): sin prueba de profundidad en el canal R (la
// silueta entera) y con prueba contra el G-buffer en el canal G (lo que se
// ve). El pipeline decide que canal se escribe (colorWriteMask).

layout(location = 0) out vec4 out_mask;

void main() {
    out_mask = vec4(1.0);
}

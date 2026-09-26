// Codigo del usuario de un shader de superficie. Este es el de por defecto
// (no cambia nada): sirve para compilar surface.vert/.frag al construir el
// motor y comprobar las plantillas. En tiempo de ejecucion el motor pone en
// su lugar el .crshader (ver CramionCore/asset/SurfaceShader).
//
// Si el .crshader tiene `void vertex(inout Vertex v)`, el motor define
// CRAMION_HAS_VERTEX.

void surface(inout Surface s) {
}

// cramion_docgen: la referencia de componentes del manual, generada desde la
// reflexion (la misma que usan el Inspector y las escenas). Cada componente
// sale con su introduccion y una tabla con todas sus propiedades: nombre en
// el Inspector, clave en el archivo, tipo, valor por defecto, rango u
// opciones y la ayuda del Inspector. Asi la referencia nunca se queda atras.
//
//   cramion_docgen <docs-src/pages>
//
// Escribe componentes.html (el indice) y componentes-<grupo>.html; despues
// hay que ejecutar docs-src/build_manual.py.

#include <CramionCore/CramionCore.h>
#include <CramionCore/xr/XrRig.h>
#include <CramionCore/fluid/Fluid.h>

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace cramion;

namespace {

std::string html(const std::string& s) {
    std::string out;
    for (const char c : s) {
        switch (c) {
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '&': out += "&amp;"; break;
            case '"': out += "&quot;"; break;
            case '\n': out += "<br>"; break;
            default: out += c;
        }
    }
    return out;
}

std::string number(float v) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%g", static_cast<double>(v));
    std::string s = buffer;
    for (char& c : s) {
        if (c == '.') c = ',';  // como el resto del manual
    }
    return s;
}

std::string vec(const core::Vec3& v) { return "(" + number(v.x) + "; " + number(v.y) + "; " + number(v.z) + ")"; }

const char* vecKind(ecs::Vec3Kind kind) {
    switch (kind) {
        case ecs::Vec3Kind::Position: return "Vec3 (metros)";
        case ecs::Vec3Kind::Direction: return "Vec3 (direccion)";
        case ecs::Vec3Kind::Scale: return "Vec3 (escala)";
        case ecs::Vec3Kind::Euler: return "Vec3 (grados)";
        case ecs::Vec3Kind::Color: return "Color";
        case ecs::Vec3Kind::ColorHdr: return "Color HDR";
    }
    return "Vec3";
}

struct Row {
    int depth = 0;       // 0 propiedad; 1+ dentro de una lista
    bool group = false;  // cabecera de grupo o de lista
    std::string label, key, type, value, range, help;
};

// Recorre las propiedades de un componente y apunta una fila por cada una.
class DocVisitor : public ecs::PropertyVisitor {
public:
    std::vector<Row> rows;

    bool beginGroup(const char* label, bool) override {
        Row r;
        r.group = true;
        r.label = label;
        r.depth = depth_;
        rows.push_back(r);
        return true;
    }
    bool field(const ecs::Meta& m, float& v, const ecs::FloatRange& range) override {
        std::string limits;
        if (range.min != range.max) limits = number(range.min) + " a " + number(range.max);
        add(m, "numero", number(v), limits);
        return false;
    }
    bool field(const ecs::Meta& m, int& v, int min, int max) override {
        add(m, "entero", std::to_string(v), min != max ? std::to_string(min) + " a " + std::to_string(max) : "");
        return false;
    }
    bool field(const ecs::Meta& m, bool& v) override {
        add(m, "si/no", v ? "si" : "no", "");
        return false;
    }
    bool field(const ecs::Meta& m, std::string& v) override {
        add(m, "texto", v.empty() ? "(vacio)" : "\"" + v + "\"", "");
        return false;
    }
    bool field(const ecs::Meta& m, core::Vec3& v, ecs::Vec3Kind kind) override {
        add(m, vecKind(kind), vec(v), "");
        return false;
    }
    bool field(const ecs::Meta& m, core::Vec2& v, float) override {
        add(m, "Vec2", "(" + number(v.x) + "; " + number(v.y) + ")", "");
        return false;
    }
    bool enumeration(const ecs::Meta& m, int& v, std::span<const char* const> names) override {
        std::string options;
        for (std::size_t i = 0; i < names.size(); ++i) options += (i ? ", " : "") + std::string(names[i]);
        const std::string current = v >= 0 && static_cast<std::size_t>(v) < names.size() ? names[static_cast<std::size_t>(v)] : "?";
        add(m, "opcion", current, options);
        return false;
    }
    bool asset(const ecs::Meta& m, assets::AssetRef&, assets::AssetType type) override {
        add(m, std::string("asset (") + assets::assetTypeName(type) + ")", "(ninguno)", "arrastrar desde el Proyecto");
        return false;
    }
    bool entity(const ecs::Meta& m, Uuid&) override {
        add(m, "entidad", "(ninguna)", "arrastrar desde la Jerarquia");
        return false;
    }
    bool layerMask(const ecs::Meta& m, std::uint32_t& mask) override {
        add(m, "capas de fisica", mask == 0xFFFFFFFFu ? "todas" : std::to_string(mask), "");
        return false;
    }
    // Las listas: su cabecera y los campos de un elemento (una vez).
    bool beginList(const ecs::Meta& m, std::size_t& count) override {
        Row r;
        r.group = true;
        r.label = std::string(m.label) + " (lista)";
        r.key = m.key;
        r.help = m.tooltip != nullptr ? m.tooltip : "";
        r.depth = depth_;
        rows.push_back(r);
        list_count_ = count;
        if (count == 0) count = 1;  // un elemento de muestra para ver sus campos
        ++depth_;
        return true;
    }
    bool beginListItem(std::size_t index) override { return index == 0; }
    int endList() override {
        --depth_;
        return -1;
    }

private:
    void add(const ecs::Meta& m, std::string type, std::string value, std::string range) {
        Row r;
        r.depth = depth_;
        r.label = m.label;
        r.key = m.key;
        r.type = std::move(type);
        r.value = std::move(value);
        r.range = std::move(range);
        r.help = m.tooltip != nullptr ? m.tooltip : "";
        rows.push_back(std::move(r));
    }
    int depth_ = 0;
    std::size_t list_count_ = 0;
};

// Introduccion de cada componente (lo que la reflexion no cuenta).
const std::map<std::string, std::string>& intros() {
    static const std::map<std::string, std::string> kIntros = {
        {"Transform", "Posicion, giro y escala <strong>locales</strong> (respecto al padre en la Jerarquia). Lo tienen todas las "
                      "entidades. Desde Lua: <code>entity.position</code> (mundo), <code>localPosition</code>, "
                      "<code>rotation</code> (grados), <code>quaternion</code> y <code>scale</code>."},
        {"MeshRenderer", "Dibuja una pieza de un modelo importado (<code>.crdata</code>) o una primitiva integrada (cubo, esfera, "
                         "plano, cilindro, capsula) con sus materiales (<code>.crmat</code>), uno por submalla. Tambien puede dibujar "
                         "una malla creada por codigo (<code>entity.mesh</code>, ver <a href=\"mallas.html\">Mallas por codigo</a>). "
                         "<em>Static</em> (casilla del Inspector) lo prepara para el static batching y las sondas."},
        {"Light", "Luz direccional (el sol: su eje hacia delante es la direccion de los rayos), puntual o foco. Las direccionales "
                  "usan sombras en cascada; las puntuales y los focos pueden tener sombras propias (con un tope por escena). "
                  "En la Escena, los focos y las puntuales tienen asas para su alcance y sus angulos."},
        {"Camera", "La camara del juego (la vista <strong>Juego</strong> y el juego exportado). La vista Escena del editor usa la "
                   "suya. Con <em>Principal</em> es la que se usa; con un <code>Camera Brain</code> la mueven las camaras "
                   "virtuales."},
        {"Decal", "Proyecta sobre todo lo que queda dentro de su caja (el cubo unidad escalado por el Transform) a lo largo de su "
                  "eje Y local hacia abajo: <strong>Estampa</strong> (una imagen o un color: grafitis, suciedad, logos), "
                  "<strong>Charco</strong> (agua acumulada) o <strong>Humedad</strong>. La herramienta <em>T Estampar</em> de la "
                  "Escena los pone con un clic."},
        {"PostProcessing", "Post-procesado como un <em>Volume</em> de Unity: exposicion, bloom, color, vineta, lente, "
                           "oclusion, niebla y efectos. <strong>Global</strong> vale para toda la escena; <strong>Caja</strong> y "
                           "<strong>Esfera</strong> solo con la camara dentro (con distancia de mezcla), y solo cambian las "
                           "secciones marcadas <em>Sobrescribir</em>. Se ordenan por prioridad."},
        {"Sky", "Cielo: un mapa HDR importado o el cielo fisico con hora del dia, nubes volumetricas y ciclo dia/noche. "
                "La luz direccional de la escena hace de sol."},
        {"Weather", "Lluvia, suelo mojado, charcos y una zona inundada. Los <code>Decal</code> de tipo Charco se suman a los "
                    "charcos de la lluvia."},
        {"Terrain", "Terreno de mapa de alturas (como el Landscape de Unreal): tamano, altura maxima, hasta 8 capas de textura y "
                    "su archivo de datos (<code>.crterrain</code> en Assets). La entidad marca la esquina (x, z minimas) y la altura "
                    "0; no gira ni escala. Herramientas del Inspector: esculpir, suavizar, aplanar, rampa, ruido, erosion, "
                    "terrazas, pintar capas y generar un relieve."},
        {"TerrainFlatten", "Aplana el terreno bajo el objeto: su huella (el AABB de sus mallas en el suelo, girado con el "
                           "objeto, mas el margen) queda plana a la altura de su origen (o de la base de la malla) con un "
                           "talud suave alrededor, y debajo se pinta la capa Tierra para que la hierba no atraviese el "
                           "suelo. Lo llevan las casas y edificios generados. En el editor se rehace al soltar el objeto "
                           "despues de moverlo o girarlo, y el sitio de antes vuelve a como estaba (tambien al borrarlo). "
                           "Ver <a href=\"mundo-procedural.html#pueblos\">Pueblos medievales</a>."},
        {"WaterBody", "Agua procedural: <strong>Oceano</strong> (plano infinito hasta el horizonte con oleaje FFT en 4 "
                      "cascadas, viento, fetch y mar de fondo), <strong>Lago</strong> (rectangulo con olas suaves) o "
                      "<strong>Rio</strong> (cinta que sigue sus puntos, con corriente y rapidos). Los Rigidbody flotan, "
                      "bajo el agua se ven la ventana de Snell, rayos de sol, causticas y particulas, y los cuerpos que se "
                      "mueven dejan ondas. Ver <a href=\"agua.html\">Agua</a>."},
        {"Foliage", "Bosques de <strong>millones de arboles procedurales</strong> (pino, abeto, roble, abedul, palmera o sauce: "
                    "tres especies por mezcla) sembrados en segundo plano sobre el terreno, evitando los rios, los lagos, las "
                    "pendientes y los <em>claros</em>. Tronco y ramas de verdad, cortezas de crestas y de placas, borlas de "
                    "agujas, racimos de hojas que se iluminan a contraluz, 3 LODs, culling en la GPU, sombras y viento. Ver "
                    "<a href=\"mundo-procedural.html#arboles\">Arboles procedurales</a>."},
        {"Grass", "Hierba en la GPU sobre la capa del terreno que se elija: millones de briznas alrededor de la camara, sin "
                  "mallas, en matas que se abren desde su centro, con manchas de color, puntas secas y alguna brizna de "
                  "paja, viento y translucidez, que se <strong>apartan y se aplastan</strong> con los Rigidbody que pasan "
                  "por encima. Va en la misma entidad que el Terrain. Ver "
                  "<a href=\"mundo-procedural.html#hierba\">Hierba (GPU)</a>."},
        {"VoxelWorld", "Un mundo de bloques infinito como Minecraft: biomas, cuevas, minerales, arboles, agua y luz por bloques. "
                       "Se genera y se malla en hilos de fondo; en el editor, con <em>Vista previa</em>. Ver "
                       "<a href=\"voxel.html\">Mundo de bloques</a> para la API de Lua."},
        {"Rigidbody", "Cuerpo de fisica (Jolt): <strong>Dinamico</strong> (cae, choca, se empuja), <strong>Cinematico</strong> "
                      "(lo mueves tu y empuja a los demas) o <strong>Estatico</strong>. Necesita al menos un collider en la entidad "
                      "o en sus hijos. Desde Lua: <code>velocity</code>, <code>addForce</code>, <code>addImpulse</code>..."},
        {"BoxCollider", "Colision en forma de caja: su tamano y centro en el espacio del objeto, multiplicados por la escala "
                        "del Transform. Como en Unity, al anadirlo (Add Component, <code>addComponent</code>, MCP) toma el "
                        "<strong>AABB de la malla</strong> del objeto o, si no tiene, el de las mallas de sus hijos; "
                        "<em>Ajustar a la malla</em> (o <em>Restablecer valores</em>) lo vuelve a calcular. Sin Rigidbody es un "
                        "obstaculo estatico. Con <em>Es trigger</em> no choca: avisa (<code>OnTriggerEnter</code>)."},
        {"SphereCollider", "Colision en forma de esfera (radio y centro). Al anadirla se centra en el AABB de la malla con la "
                           "mayor media medida de radio."},
        {"CapsuleCollider", "Colision en forma de capsula (personajes): radio, altura y eje. Al anadirla toma el AABB de la "
                            "malla, a lo largo de su eje mas largo."},
        {"MeshCollider", "La geometria de la pieza del MeshRenderer de la misma entidad. Malla de triangulos: solo estatica o "
                         "cinematica; <em>Convexa</em>: vale para cuerpos dinamicos (envolvente convexa)."},
        {"PlaneCollider", "Plano infinito por el origen de la entidad con su +Y como normal (el suelo de una escena). Siempre "
                          "estatico."},
        {"Vehicle", "Vehiculo con ruedas (el de Jolt): en el objeto con Rigidbody dinamico; sus ruedas son los "
                    "<code>WheelCollider</code> de los hijos. Motor, cambio automatico y diferenciales. Se conduce con "
                    "<code>entity:setVehicleInput(acelerador, direccion, freno, freno de mano)</code> o con el teclado."},
        {"CppScript",
         "Script de C++ (la clase de CRAMION_SCRIPT en un .cpp de Assets). Corre aislado en otro proceso: si falla, el "
         "motor sigue y la Consola dice el archivo y la linea. Ver <a href=\"scripts-cpp.html\">Scripts en C++</a>."},
        {"EditableMesh",
         "Malla de poligonos que se edita en la vista Escena por vertices, aristas y caras (como ProBuilder): genera la "
         "malla del Mesh Renderer y del Mesh Collider. Se crea con GameObject &gt; Malla editable o se convierte un "
         "modelo con la ventana Modelado. Ver <a href=\"modelado.html\">Modelado poligonal</a>."},
        {"CharacterController",
         "Personaje que se mueve chocando y deslizando (el CharacterController de Unity y el CharacterMovement de "
         "Unreal, sobre el CharacterVirtual de Jolt): capsula vertical que sube escalones, no sube rampas mas "
         "empinadas que su limite, se pega al suelo al bajar, sigue a las plataformas que se mueven y empuja a los "
         "Rigidbody. Es collider: la tocan los rayos, entra en los triggers y da eventos Collision. "
         "<em>Manual</em>: solo <code>entity:move(desplazamiento)</code> desde un script (sin gravedad, como Unity). "
         "<em>Integrado</em>: gravedad, salto (con tiempo coyote y doble salto), andar/correr/agacharse con "
         "aceleracion; con el teclado (WASD relativo a la camara principal, Shift, Espacio, C/Ctrl) o con "
         "<code>entity:setMoveInput(dir, correr)</code>, <code>entity:jump()</code> y "
         "<code>entity:setCrouch(true)</code>. Crear uno listo: GameObject &gt; Fisica &gt; Personaje."},
        {"WheelCollider","Una rueda de un <code>Vehicle</code>: suspension por raycast, direccion, traccion y frenos. "
                          "<em>Visual</em> es el objeto que gira con ella."},
        {"ParticleSystem", "Emisor de particulas como el de Unity: emision continua y rafagas, forma, velocidad, tamano y color "
                           "a lo largo de la vida, gravedad, rozamiento y colision con los colliders (rebotan y envian eventos). "
                           "Se ve en el editor sin Play."},
        {"Animator", "Reproduce las animaciones de un modelo con esqueleto (en la entidad del MeshRenderer animado). Con un "
                     "<strong>Animator Controller</strong> (<code>.cranimator</code>, editor visual en la ventana Animator) su "
                     "maquina de estados elige clip, velocidad y bucle segun sus parametros; sin el mandan Clip / Nombre del "
                     "clip. Desde Lua: <code>setAnimatorFloat</code>, <code>setAnimatorBool</code>, "
                     "<code>setAnimatorTrigger</code>, <code>playAnimation</code>."},
        {"InverseKinematics", "Cinematica inversa sobre la pose animada de humanos y animales: manos y pies que llegan a un "
                              "objetivo (una entidad o un punto), la cabeza que mira a algo repartiendo el giro por el cuello, "
                              "pies que se apoyan en el suelo (escaleras, pendientes) y cadenas de 1 a 16 huesos para cualquier "
                              "esqueleto: patas de 3 huesos de perros y caballos con \"Al suelo\", cuellos, colas. El cuerpo baja "
                              "y se inclina con la pendiente. \"Configurar automaticamente\" en el Inspector detecta patas, "
                              "cabeza y cuello. Puede ir en el modelo o en su raiz. Ver <a href=\"esqueletos.html\">Esqueletos</a>."},
        {"Skeleton", "Ver y mover los huesos de un modelo: los dibuja en la Escena (clic en una articulacion para "
                     "resaltarla), los lista en el Inspector con un buscador y cada uno se puede girar, desplazar o escalar "
                     "encima de la animacion. \"Socket aqui\" engancha una entidad al hueso resaltado. Ver "
                     "<a href=\"esqueletos.html\">Esqueletos</a>."},
        {"BoneSocket", "Engancha una entidad a un hueso del modelo de un antepasado: Seguir al hueso (una espada en la mano, "
                       "un sombrero, un collider en la cabeza) o Mover el hueso (el hueso sigue a la entidad: posar con el "
                       "gizmo). Lua: <code>espada:attachToBone(personaje, \"RightHand\")</code>."},
        {"PhysBones", "Huesos que se mueven solos con inercia, gravedad y choques, como los PhysBone de VRChat: pelo, "
                      "coletas, colas, orejas, faldas, capas. Cada cadena parte de un hueso raiz con pull, spring, stiffness, "
                      "gravedad, immobile, angulo maximo y radio. \"Detectar pelo, colas, orejas...\" las crea por el nombre "
                      "de los huesos."},
        {"PhysBoneCollider", "Esfera, capsula o plano con el que chocan los Phys Bones (la cabeza, el cuerpo, el suelo). "
                             "Ponlo en una entidad con Bone Socket para que siga a un hueso."},
        {"Ragdoll", "Muneco de trapo con la fisica (Jolt): una capsula por hueso unidas por articulaciones con limites de "
                    "giro. Apagado sigue a la animacion; al activarlo (<code>entity.ragdoll = true</code>) cae con la "
                    "velocidad que llevaba y al apagarlo vuelve a la animacion mezclando. Sin huesos en la lista se eligen "
                    "solos para humanos y animales; \"Generar huesos\" los rellena para ajustarlos."},
        {"ProceduralAnimation", "Animacion procedural: huesos con muelle (pelo, colas, capas), patas que dan pasos solas "
                                "(aranas, robots) y capas de respirar, inclinarse y ruido. Con o sin Animator."},
        {"AudioSource", "Un sonido en una entidad: clip (WAV, MP3, FLAC, OGG), volumen, tono, bucle, 2D o 3D con atenuacion y "
                        "Doppler, y efectos: paso bajo, paso alto, eco y envio a reverberacion. Ver "
                        "<a href=\"audio.html\">Audio</a>."},
        {"AudioListener", "Los oidos (normalmente en la camara). Activa la <strong>oclusion</strong>: sonidos con paredes en "
                          "medio se oyen tapados. Tambien un paso bajo general y el volumen general."},
        {"AudioReverbZone", "Esfera de reverberacion (habitacion, bano, sala, cueva, estadio, bosque, bajo el agua): con el "
                            "oyente dentro, los sonidos reverberan."},
        {"VirtualCamera", "Una camara virtual (como Cinemachine): prioridad, lente, a quien sigue y a quien mira, como se mueve "
                          "(Transposer, riel, orbita...) y como apunta, con amortiguacion y ruido. La camara real con un "
                          "<code>Camera Brain</code> usa la de mas prioridad y mezcla al cambiar."},
        {"CameraBrain", "En la camara real: elige la camara virtual activa (o la del plano de una cinematica) y mezcla entre "
                        "ellas."},
        {"DollyTrack", "Un riel: puntos unidos por una curva (Catmull-Rom) en el espacio local de la entidad, abierto o en "
                       "bucle. Lo recorren los <code>Dolly Cart</code> y las camaras virtuales con cuerpo de riel."},
        {"DollyCart", "Mueve su entidad por un riel a una velocidad (en metros o normalizado)."},
        {"CinematicSequence", "Una cinematica (como el Timeline de Unity): planos (que camara virtual, cuando, cuanto dura y con "
                              "que mezcla entra) y objetos que se activan por tramos. Se edita en la ventana Cinematica."},
        {"NavMeshBounds", "El volumen donde se genera la malla de navegacion (NavMeshBoundsVolume de Unreal). Sin ninguno no hay "
                          "navegacion. La malla sale de los colliders y se rehace sola por baldosas al mover objetos. P en la "
                          "Escena la muestra."},
        {"NavModifier", "Caja que quita la navegacion de su interior (<strong>Bloquear</strong>) o la encarece "
                        "(<strong>Evitar</strong>: los caminos la rodean si pueden)."},
        {"NavAgent", "Lo que se mueve por la malla (Character Movement + AIController de Unreal): <code>entity:moveTo</code>, "
                     "velocidad, aceleracion y esquivar a los demas agentes. Ver <a href=\"navegacion.html\">Navegacion</a>."},
        {"Script", "Un script de Lua (<code>.lua</code> en Assets) y los valores de sus <code>properties</code> para esta "
                   "entidad. Ver <a href=\"primer-script.html\">Tu primer script</a>."},
        {"NetworkObject", "Un objeto que existe en todos los jugadores de una partida en red (lo crea "
                          "<code>Network.spawn</code>). Su dueno manda la posicion y el giro; los demas lo siguen suavizado. "
                          "En un prefab ajusta la frecuencia y el suavizado; si no lo lleva, se anade solo. Ver "
                          "<a href=\"red.html#objetos\">Network</a>."},
        {"UICanvas", "La raiz de una interfaz: todo lo que cuelga de el se dibuja encima del juego. <em>Referencia</em> es la "
                     "resolucion de diseno (1920x1080) y se escala con la pantalla."},
        {"RectTransform", "Rectangulo de un elemento de interfaz: anclas (0..1 del padre), pivote, posicion desde el ancla y "
                          "tamano. Anclas separadas estiran el elemento con su padre. Desde Lua: <code>uiPosition</code> y "
                          "<code>uiSize</code>."},
        {"UIImage", "Rectangulo de color o con imagen (<code>entity.texture</code>, <code>color</code>, <code>alpha</code>), con "
                    "esquinas redondeadas."},
        {"UIText", "Texto (<code>entity.text</code>), tamano, color, alineacion, ajuste de linea y sombra."},
        {"UIButton", "Boton: colores por estado y el metodo del script al que llama al pulsarlo (<em>Al pulsar</em>, en el "
                     "objeto <em>Destino</em> o en el propio boton): <code>function X:OnJugar(boton)</code>."},
        {"UISlider", "Barra con un valor entre minimo y maximo (<code>entity.value</code>) que avisa al cambiar."},
        {"UIToggle", "Casilla de si/no (<code>entity.value</code> 0 o 1) que avisa al cambiar."},
        {"UIInputField", "Campo de texto editable (<code>entity.text</code>) que avisa al cambiar y al confirmar."},
        {"Profiler", "Muestra en una esquina del juego los FPS, la CPU, la GPU y la memoria con una grafica del tiempo de cada "
                     "frame (como <em>stat fps</em> de Unreal). Basta uno en la escena."},
        {"PrefabInstance", "Lo pone el editor en la raiz de cada instancia de un prefab: que prefab, en que revision y sus "
                           "cambios propios. No se anade a mano. Ver <a href=\"prefabs.html\">Prefabs</a>."},
        {"PrefabLink", "Lo pone el editor en cada entidad de una instancia: a que entidad del prefab corresponde."},
        {"Spline", "Curva por puntos de control (Catmull-Rom, Bezier o lineal), abierta o cerrada, con ancho y peralte por "
                   "punto. En la Escena los puntos se arrastran (Mayus+clic anade, Ctrl+clic quita). Lua: "
                   "<code>entity:splinePoint(t)</code>, <code>Spline.create(puntos, \"road\")</code>."},
        {"SplineExtrude", "Genera una malla a lo largo de la Spline: carretera (con lineas), camino, rio (mueve el WaterBody), "
                          "muro, valla con postes, tuberia, railes con traviesas o cinta. Se pega al terreno y <em>Aplicar al "
                          "terreno</em> lo allana y pinta debajo."},
        {"SplineFollower", "Mueve la entidad por una Spline (camaras, vagonetas, patrullas): bucle, ida y vuelta o una vez, "
                           "mirando hacia delante."},
        {"WorldPartition", "Divide el mundo en celdas: en Play y en el juego solo estan cargadas las cercanas a las fuentes de "
                           "carga (o a la camara); lo lejano sale del mundo y vuelve al acercarse. <em>Construir HLOD</em> crea "
                           "una malla simplificada por celda que se ve de lejos."},
        {"StreamingSource", "Lo que carga el mundo a su alrededor con World Partition (el jugador)."},
        {"AlwaysLoaded", "Esta entidad no se descarga nunca con World Partition."},
        {"HlodProxy", "La malla simplificada de una celda (la crea <em>Construir HLOD</em>): solo se ve cuando su celda esta "
                      "descargada."},
        {"FogVolume", "Niebla local (caja o esfera) con su densidad, color, borde suave y ruido, iluminada por el sol, el cielo "
                      "y las luces con sus sombras. Necesita la luz volumetrica del post-proceso."},
        {"CrowdSpawner", "Crea en Play muchas copias de un prefab en la navmesh y las mueve (deambular, recorrer puntos, seguir "
                         "u huir), repartiendo las decisiones entre frames y ocultando las lejanas."},
        {"CrowdAgent", "Lo pone el Crowd Spawner en cada agente que crea."},
        {"UIDropdown", "Desplegable: opciones, la elegida y el evento al cambiar (indice desde 1). Lua: "
                       "<code>entity.dropdownValue</code>, <code>entity:setDropdownOptions{...}</code>."},
        {"UIScrollView", "Zona con desplazamiento (rueda, arrastre con inercia y barra) que recorta a sus hijos. Con un hijo "
                         "con Layout Group y <em>Ajustar al contenido</em> crece solo."},
        {"UILayoutGroup", "Coloca a los hijos en columna, fila o rejilla, con separacion, margen y alineacion; "
                          "<em>Ajustar al contenido</em> hace crecer el rectangulo."},
        {"UIMask", "Recorta a los hijos a su rectangulo."},
    };
    return kIntros;
}

struct Page {
    std::string slug;
    std::string title;
    std::vector<std::string> categories;
    std::string intro;
};

const std::vector<Page>& pages() {
    static const std::vector<Page> kPages = {
        {"componentes-basicos", "Transform y renderizado", {"General", "Renderizado"},
         "Lo que tiene toda entidad (Transform) y lo que se dibuja: mallas, luces, camaras, decals y post-procesado."},
        {"componentes-entorno", "Entorno", {"Entorno"}, "Cielo, clima, terreno, agua y mundos de bloques."},
        {"componentes-fisica", "Fisica", {"Fisica"},
         "Cuerpos, colliders y vehiculos (Jolt). Las capas y la matriz de colisiones estan en la ventana Fisica."},
        {"componentes-efectos", "Particulas y depuracion", {"Efectos", "Depuracion"}, "Sistemas de particulas y el Profiler."},
        {"componentes-animacion", "Animacion", {"Animacion"}, "Animator, cinematica inversa y animacion procedural."},
        {"componentes-audio", "Audio", {"Audio"}, "Fuentes de sonido, oyente y zonas de reverberacion."},
        {"componentes-cinematicas", "Camaras y cinematicas", {"Cinematicas"},
         "Camaras virtuales, rieles y secuencias (como Cinemachine y Timeline)."},
        {"componentes-navegacion", "Navegacion", {"Navegacion"}, "Volumenes de navmesh, modificadores y agentes."},
        {"componentes-ui", "Interfaz (UI)", {"UI"}, "Canvas, rectangulos, imagenes, textos, botones, sliders, casillas y campos."},
        {"componentes-scripting", "Scripts y prefabs", {"Scripting", "Prefab"}, "El componente Script y los que usan los prefabs."},
        {"componentes-2d", "2D", {"2D"},
         "Sprites, animacion por fotogramas, luces 2D, tilemaps y fisica 2D (ver la pagina 2D del editor)."},
        {"componentes-iluminacion", "Iluminacion horneada", {"Iluminacion"},
         "Volumenes de sondas de luz rebotada (ver Iluminacion horneada en el editor)."},
        {"componentes-vr", "Realidad virtual", {"Realidad virtual"},
         "XR Origin (el suelo de la habitacion, con la camara que sigue al casco) y los mandos (XR Controller)."},
        {"componentes-mundo", "Splines y mundos grandes", {"Splines", "Mundo"},
         "Splines (carreteras, caminos, rios, muros, vallas, tuberias, railes), World Partition y HLOD."},
        {"componentes-ia", "Multitudes", {"IA"}, "Multitudes de agentes con navmesh (Crowd Spawner)."},
    };
    return kPages;
}

std::string componentHtml(const ecs::ComponentType& type) {
    ecs::World world;
    ecs::Entity e = world.create("Doc");
    if (!type.has(world, e.handle())) type.add(world, e.handle());
    DocVisitor visitor;
    type.reflect(world, e.handle(), visitor);

    std::ostringstream o;
    o << "<h3 id=\"" << type.name << "\">" << html(type.label) << " <code>" << type.name << "</code></h3>\n";
    const auto intro = intros().find(type.name);
    if (intro != intros().end()) o << "<p>" << intro->second << "</p>\n";
    if (visitor.rows.empty()) {
        o << "<p class=\"text-slate-400\">Sin propiedades.</p>\n";
        return o.str();
    }
    o << "<table>\n  <thead><tr><th>Propiedad</th><th>Tipo</th><th>Por defecto</th><th>Valores</th><th>Descripcion</th></tr></thead>\n  <tbody>\n";
    for (const Row& r : visitor.rows) {
        const std::string indent(static_cast<std::size_t>(r.depth) * 4, ' ');
        std::string pad;
        for (int i = 0; i < r.depth; ++i) pad += "&nbsp;&nbsp;&nbsp;";
        if (r.group) {
            o << "    <tr><td colspan=\"5\"><strong class=\"text-white\">" << pad << html(r.label) << "</strong>";
            if (!r.key.empty()) o << " <code>" << r.key << "</code>";
            if (!r.help.empty()) o << " — " << html(r.help);
            o << "</td></tr>\n";
            continue;
        }
        // Las etiquetas con sangria ("  Corte") ya van debajo de su casilla.
        std::string label = r.label;
        while (!label.empty() && label.front() == ' ') label.erase(label.begin());
        o << "    <tr><td>" << pad << html(label) << "<br><code>" << r.key << "</code></td><td>" << html(r.type) << "</td><td>"
          << html(r.value) << "</td><td>" << html(r.range) << "</td><td>" << html(r.help) << "</td></tr>\n";
    }
    o << "  </tbody>\n</table>\n";
    return o.str();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("uso: cramion_docgen <docs-src/pages>\n");
        return 2;
    }
    const std::filesystem::path out = argv[1];
    physics::registerPhysicsComponents();
    ecs::registerPrefabComponents();
    cinema::registerCinematicComponents();
    terrain::registerTerrainComponents();
    water::registerWaterComponents();
    foliage::registerFoliageComponents();
    fire::registerFireComponents();
    fluid::registerFluidComponents();
    navigation::registerNavigationComponents();
    voxel::registerVoxelComponents();
    audio::registerAudioComponents();
    scripting::registerScriptComponents();
    ui::registerUiComponents();
    xr::registerXrComponents();
    const std::vector<ecs::ComponentType>& types = ecs::ComponentRegistry::instance().types();

    // Indice: todos los componentes por pagina.
    std::ostringstream index;
    index << "<p>\n  Todos los componentes del motor, con <strong class=\"text-white\">todas sus propiedades</strong>: el nombre que "
             "ves en el Inspector, la clave con la que se guarda en la escena, el tipo, el valor por defecto, el rango u "
             "opciones y la ayuda. Esta referencia se genera desde el propio motor (<code>cramion_docgen</code>), asi que "
             "siempre esta al dia.\n</p>\n<p>En el editor se anaden con <strong class=\"text-white\">Add Component</strong> "
             "en el Inspector; desde Lua con <code>entity:addComponent(\"Rigidbody\")</code> (la clave de la tabla).</p>\n";
    int documented = 0;
    for (const Page& page : pages()) {
        index << "<h3 id=\"" << page.slug << "\"><a href=\"" << page.slug << ".html\">" << html(page.title) << "</a></h3>\n<ul>\n";
        std::ostringstream body;
        body << "<p>" << html(page.intro) << " Referencia completa generada desde el motor.</p>\n";
        for (const std::string& category : page.categories) {
            for (const ecs::ComponentType& t : types) {
                if (t.category != category) continue;
                body << componentHtml(t);
                index << "  <li><a href=\"" << page.slug << ".html#" << t.name << "\">" << html(t.label) << "</a> <code>" << t.name
                      << "</code></li>\n";
                ++documented;
            }
        }
        index << "</ul>\n";
        std::ofstream(out / (page.slug + ".html"), std::ios::binary) << body.str();
    }
    std::ofstream(out / "componentes.html", std::ios::binary) << index.str();
    if (documented != static_cast<int>(types.size())) {
        std::printf("AVISO: %d componentes sin pagina (categoria nueva?)\n", static_cast<int>(types.size()) - documented);
        for (const ecs::ComponentType& t : types) {
            bool found = false;
            for (const Page& p : pages()) {
                for (const std::string& c : p.categories) found = found || c == t.category;
            }
            if (!found) std::printf("  %s (%s)\n", t.name.c_str(), t.category.c_str());
        }
        return 1;
    }
    std::printf("%d componentes documentados en %zu paginas\n", documented, pages().size());
    return 0;
}

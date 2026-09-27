# Cambios

## 0.7.1

### Render Textures
- **Render Texture** nueva (`.crrt`, *+ Añadir > Render Texture*), como en Unity: una **Camera con Target Texture** dibuja lo que ve en esa textura en vez de en la pantalla, y cualquier **material** la usa en el hueco de **Color** o de **Emision**. Pantallas, camaras de seguridad, espejos, retrovisores, minimapas o retratos. Funciona en el editor (tambien fuera de Play) y en el juego exportado, con el mismo post-procesado que la pantalla.
- Inspector de la Render Texture con su tamano (botones 128 a 2048 y 16:9) y lo que tiene en ese momento; si cambia el tamano, se rehace sola.
- Desde Lua, `entity:setField("Camera", "target_texture", uuid)`: `getField`/`setField` ahora leen y cambian referencias a assets (materiales, Target Texture...) por su UUID.

### Vistas del editor
- **Free Aspect**: la vista Escena ocupa todo el panel y se dibuja a su tamano exacto (antes tenia la proporcion de la ventana, con bandas). Menu de **proporcion** en la Escena y en el Juego como el de Unity: Free Aspect, 16:9, 16:10, 21:9, 32:9, 4:3, 5:4, 3:2, 1:1, vertical 9:16 y 9:19,5, resoluciones fijas (720p, 1080p, 1440p, 4K, 1080x1920 movil) y una **resolucion propia**. Se recuerda por proyecto.
- **Modo de dibujo** de la vista Escena: **Lit**, **Unlit** (solo el color de los materiales), **Wireframe** (las lineas de mallas, terreno, voxeles y vegetacion) y **Lit + Wireframe**.

### Manual
- Pagina nueva **Render Textures**; modos de dibujo y proporcion de la vista en *Vista Escena y gizmos*; la proporcion de la vista Juego en *Play*; Render Textures en los huecos de textura de *Materiales*. 66 paginas y 784 entradas en el buscador.

### Corregido
- Aviso de Vulkan (imagen en layout `UNDEFINED`) en el primer frame tras abrir un proyecto o cambiar el tamano de la vista.

## 0.7.0

### Multijugador
- **Red** nueva (UDP con ENet) y API **`Network`** en Lua: `Network.host(puerto)` crea la partida (el servidor tambien juega) y `Network.connect(ip)` se une. Todo pasa por el servidor, que reenvia a los demas y puede comprobar lo que llega.
- **Mensajes** con cualquier valor de Lua (numeros, textos, `Vec3`, tablas): `Network.send("chat", datos[, destino])` y `Network.on("chat", function(datos, de) end)`, fiables y en orden. Avisos al entrar y salir jugadores (`onPlayerJoined`, `onPlayerLeft`, `onConnected`, `onDisconnected`), ping y estadisticas.
- **Objetos de red**: `Network.spawn(prefab, posicion, dueno)` crea un prefab en todos; su dueno lo mueve (`entity:isMine()`) y los demas lo ven suavizado, con su Rigidbody cinematico. **Variables sincronizadas** (`setNetVar` / `getNetVar`). Quien entra tarde recibe todo como esta; si alguien se va, sus objetos desaparecen. `Network.loadScene` cambia la escena de todos. Componente **Objeto de red** para ajustar frecuencia y suavizado, y **Fisica local en los demas**: los objetos con fisica del servidor (balones, cajas) se simulan tambien en cada cliente y se corrigen con lo que llega, asi cualquier jugador los empuja al pasar y no solo el que crea la partida.
- La barra de la Escena muestra la red durante el Play (servidor o cliente y jugadores). Salir de Play o cerrar el juego cierra la partida.

### HTTPS: servidores y webs
- API **`Http`** en Lua para mandar y recibir datos de servidores y webs de forma segura: `Http.get(url, funcion)`, `Http.post(url, datos, funcion)` (una tabla se manda como JSON) y `Http.request{...}` para cualquier metodo, con cabeceras propias (tokens). La respuesta llega a la funcion con `ok`, `status`, `body`, `headers`, `error` y **`data`** (el JSON ya decodificado). Van en segundo plano: el juego no se para.
- **Seguridad**: solo `https://` con TLS 1.2/1.3 y el certificado comprobado por Windows (caducado, de otro dominio o autofirmado = rechazado); `http://` solo a `localhost` para pruebas; nunca redirige de https a http; cabeceras sin saltos de linea; tiempo maximo (20 s) y tamano maximo de la respuesta (32 MB). Al salir de Play se cancela lo pendiente.
- **`Json`**: `Json.encode(tabla)` y `Json.decode(texto)`. Tambien `Http.urlEncode`, `Http.query`, `Http.pending` y `Http.cancelAll`.

### Plantilla Online (todo en Lua)
- Nueva plantilla en el Hub: menu para **crear partida o unirse por IP**, **jugadores sincronizados** con su color, **chat** con nombres, **marcador** con puntos y ping, **monedas, un balon y cajas** con fisica del servidor (F patea), **porterias y goles**, rondas y **eventos** (lluvia de monedas, monedas dobles). El servidor decide los puntos. Scripts `Red.lua`, `JugadorRed.lua` y `MonedaRed.lua`.

### Mundo abierto y vegetacion
- Componente **Vegetacion** (*GameObject > Vegetacion (bosque)*): **millones de arboles** (pinos, robles y abedules) sembrados en segundo plano sobre el terreno, sin agua, pendientes fuertes ni **claros**. Se dibujan en la GPU: un compute shader los recorta y elige entre **3 niveles de detalle**, con sombras cercanas y viento.
- Plantilla **Mundo abierto (rendimiento)**: una **isla de 8 x 8 km** con relieve, playas y nieve, **oceano** alrededor y unos **2 millones de arboles**, con un personaje en tercera persona y un **panel de rendimiento** (FPS, ms de CPU y GPU, arboles y triangulos). Teclas para cambiar calidad, densidad, distancia, sombras y viento. En una RTX 4060 Ti: unos 400.000 arboles dibujados a mas de 100 FPS.
- **Niebla ajustable** en el post-procesado (densidad y caida con la altura), tambien en el agua. La de siempre era demasiado espesa para mundos de kilometros.
- `Graphics.get` da las cifras de la vegetacion (`foliage_trees`, `foliage_visible`, `foliage_triangles`).

### Animacion: Blend Trees
- Un estado del Animator puede ser un **Blend Tree 1D** (andar, trotar y correr segun `Velocidad`) o **2D** (moverse en 8 direcciones con dos parametros), con los pasos de los clips sincronizados. Grafico en el Inspector con el valor actual y el peso de cada clip; *Plantilla 8 direcciones*.
- **Fundido** en las transiciones: la pose pasa del estado viejo al nuevo en los segundos que elijas (antes el cambio era seco).

### Actualizaciones
- **SHA-256**: el paquete se publica con su `Cramion-win64.zip.sha256` y el actualizador no instala una descarga que no coincida.
- **Canal beta**: opcion *Estable / Beta* en el Hub y en el actualizador para recibir tambien las versiones previas (`0.7.0-beta.1`), marcadas como *Beta*.

### Lua
- **Autocompletado** completo en el editor de scripts: `Network`, `Http`, `Json`, la respuesta de Http (`res.`) y los contactos de choque (`contact.`), los metodos de red de las entidades, `OnNetVar`, las claves nuevas de `Graphics` y lo que faltaba de la 0.6.1: huesos, IK, ragdoll, sockets, `getField`/`setField`, las propiedades de interfaz (`text`, `value`, `color`, `interactable`) y los metodos de las mallas por codigo (`mesh:apply()`, `mesh.vertices`...).

### Manual
- Paginas nuevas **Network (multijugador)** y **Http y Json**; plantillas Online y Mundo abierto; Vegetacion en *Herramientas de mundo*; Blend Trees y fundido en el Animator; niebla en *Render*; SHA-256 y canal beta en *Actualizaciones*. 65 paginas y 774 entradas en el buscador. `OnNetVar` y `OnOriginShift` en *Ciclo de vida*; seccion Red en *Entity*.

## 0.6.2

### Actualizaciones
- **Actualizador** nuevo, `CramionUpdater.exe`: aplicacion aparte con su propia interfaz (Direct3D 11, funciona aunque Vulkan falle). Busca la ultima version en GitHub, ensena las novedades, descarga `Cramion-win64.zip` con progreso y velocidad, lo descomprime comprobando cada archivo (CRC) e instala sobre la carpeta del motor. Si algo falla **todo vuelve a como estaba**. Tambien **Reinstalar** (reparar) y **Omitir esta version**.
- **Se guarda todo antes de actualizar**: el editor guarda la escena (una escena nueva va a `Assets/Scenes`), los prefabs abiertos, los scripts, el material y el Animator, se cierra, y al terminar se **vuelve a abrir el mismo proyecto**. Si el actualizador se abre por su cuenta, pide lo mismo a cada editor abierto.
- El editor **avisa** cuando hay version nueva (abajo a la derecha, y *Ayuda > Buscar actualizaciones*). Se puede desactivar la busqueda al abrir.
- **Reinstalar** desde el Hub (*Actualizaciones*), desde *Ayuda > Buscar actualizaciones* o desde el actualizador: descarga otra vez la version publicada y repara los archivos que falten o esten danados, guardando todo antes igual que al actualizar.

### Hub
- Rediseno: barra lateral con iconos y secciones, tarjeta de la version (al dia / version nueva), **Actualizaciones** (instalada y publicada, novedades, actualizar, opciones) y **Aprender** (manual, Lua, shaders, Discord, GitHub). Proyectos en **tarjetas o lista** y ordenados por fecha o nombre.

### Corregido
- **Sin consola**: `CramionEditor.exe` y `cramion.exe` abrian una ventana de consola negra detras. Ahora son aplicaciones de ventana, como el player (los mensajes siguen en el panel *Consola* del editor).
- **Camara de la Escena**: al volar con el boton derecho, el raton chocaba con el borde de la vista o de la pantalla y la camara dejaba de girar hasta soltar y volver al centro. Ahora el cursor se oculta y se queda fijo mientras se vuela (el giro llega en bruto, sin tope) y al soltar vuelve donde estaba.
- **Polvo (luz volumetrica)**: con el presupuesto adaptativo activado, el polvo se apagaba cuando el frame iba justo y solo volvia al mirar al cielo o al sol (el frame se abarataba). Ahora el presupuesto solo lo abarata (la mitad de tramos por rayo y sin rayos en pantalla); nunca lo quita.
- **IK de animales**: la inclinacion del cuerpo con la pendiente iba al reves (al bajar un escalon levantaba el morro y se torcia) y ahora gira alrededor del centro entre caderas y hombros. **Mirar** con el cuello ya no puede pasar del angulo maximo: el giro total se calcula una vez y se reparte entre los huesos (antes, con el objetivo detras, el cuello se retorcia).
- Ejecutar Lua fuera de Play (consola del editor o `run_lua` del MCP) con `Scene.find` cerraba el editor.
- **Ajustes de render**: el interruptor de sombras y el desplegable de su resolucion se llamaban los dos "Sombras" (ImGui avisaba de un ID repetido). Ahora son *Activar sombras* y *Sombras* (resolucion).

### Manual
- Pagina nueva **Actualizaciones** (avisos, guardar todo antes, instalar, reinstalar y opciones); el Hub nuevo en *Interfaz y pestanas*; el polvo con el presupuesto adaptativo en *Render*. 63 paginas y 697 entradas en el buscador.

## 0.6.1

### Esqueletos: IK de animales, ragdoll y phys bones
- **Ragdoll** (componente nuevo, *Fisica*) con Jolt: una capsula por hueso unidas por articulaciones *swing-twist* con limites de doblar y girar, masa repartida y friccion en las articulaciones. Apagado sigue a la animacion; al activarlo (`entity.ragdoll = true`) **cae con la velocidad que llevaba la animacion**, su propio collider sale de la simulacion y la entidad va con el cuerpo; al apagarlo **vuelve a la animacion mezclando**. Los huesos se eligen solos para **humanos y animales** (cuerpo, columna, cuello, cabeza, patas y cola) o se generan en el Inspector para ajustarlos. Empujones con `entity:addRagdollForce(impulso, hueso)`. En la Escena se ven sus capsulas.
- **Phys Bones** (componente nuevo) como los de VRChat: pelo, coletas, colas, orejas, faldas y capas con **pull, spring, stiffness, gravedad y gravity falloff, immobile, angulo maximo, radio (y en la punta) y punta extra**, sin estirarse. **Phys Bone Collider**: esfera, capsula o plano (tambien "dentro"). *Detectar pelo, colas, orejas...* crea las cadenas por el nombre de los huesos.
- **IK para animales**: cadenas de **1 a 16 huesos** (FABRIK con *pole*): patas de 3 huesos de perros y caballos, cuellos, colas, tentaculos. **Patas al suelo** en cualquier esqueleto (escaleras, piedras, pendientes): el cuerpo baja lo que baje la pata mas baja y, con 3 o mas patas, **se inclina con la pendiente**. **Mirar** con cualquier hueso repartiendo el giro por el cuello. Objetivos como entidad o como **punto del mundo**. *Configurar automaticamente* reconoce patas (delante/detras, izquierda/derecha), cabeza, cuello y cola por la forma del esqueleto; *Crear objetivos* pone uno en cada cadena.
- **Esqueleto (huesos)** (componente nuevo): dibuja los huesos en la Escena (clic en una articulacion para resaltarla, nombres), los lista en el Inspector en arbol con buscador y permite **girar, desplazar y escalar** cualquier hueso encima de la animacion.
- **Bone Socket** (componente nuevo): una entidad **sigue a un hueso** (espada en la mano, sombrero, collider en la cabeza) o **mueve el hueso** (posar con el gizmo). *Socket aqui* en el Esqueleto lo crea.
- Los componentes de esqueleto pueden ir en la **raiz del modelo**: las piezas de un personaje (cuerpo, ropa, pelo) comparten la pose, el IK y el ragdoll. Lo enganchado a un socket no los hereda.

### Lua
- **Graphics**: toda la configuracion grafica desde un script (como `QualitySettings` + `Screen` de Unity): `Graphics.setQuality("Alta")`, escalado (`upscaler`, `resolution`, `resolution_scale`), nitidez, VSync, presupuesto adaptativo y FPS objetivo, **calidad de sombras y de texturas**, trazado de rayos, sonda de reflexion, occlusion culling, **ventana** (maximizada, pantalla completa, ventana con tamano) y resoluciones del monitor. `Graphics.options()` para montar un menu y `Graphics.save()` guarda lo que elige el jugador en el juego exportado. **`Graphics.post`** cambia el post-procesado de la escena (`Graphics.post.bloom = false`). En el editor, lo que cambien los scripts se deshace al parar el Play.
- **Cualquier campo de cualquier componente**: `entity:getField("Light", "intensity")`, `entity:setField("PhysBones", "chains[1].pull", 0.5)` (listas desde 1; `[n+1]` anade) y `entity:getFields("Ragdoll")`.
- Huesos: `getBones`, `getBonePosition`, `getBoneRotation`, `setBoneRotation`, `setBoneOffset`, `setBoneScale`, `resetBone(s)`, `showBones`. IK: `setIKTarget` (entidad, punto o nil), `setIKHint`, `setIKWeight`, `setLookAt`, `setFootGrounding`, `setupCreatureIK`. Ragdoll: `entity.ragdoll`, `addRagdollForce`, `setupRagdoll`. `setupPhysBones`. Sockets: `attachToBone`, `detachFromBone`.

### Plantilla Criaturas
- Nueva plantilla en el Hub: un **perro** que recorre escaleras, una rampa y piedras apoyando cada pata con IK, con la cola y las orejas fisicas, un sombrero enganchado a la cabeza y la cabeza siguiendo a una pelota; y un **maniqui** que apoya un pie en un escalon, mueve su coleta y cae como un ragdoll. R, F, B, I y P para probarlo todo. Los dos modelos (esqueleto, malla skinneada y animaciones) se generan por codigo.

### Manual
- Pagina nueva **Esqueletos: IK, ragdoll y phys bones** y pagina **Graphics**; referencia de los componentes nuevos; plantilla Criaturas. 62 paginas y 686 entradas en el buscador.

## 0.6.0

### Espacios de trabajo (pestañas)
- **Pestañas debajo del menú**, como los editores de assets de Unreal: **Escena** (todo el editor, como siempre) y una pestaña por cada **prefab** o **script** abierto. Clic para cambiar; la ✕ cierra.
- **Editar un prefab en su pestaña**: doble clic en el `.crprefab` del Proyecto, *Abrir (editar)* en su menú, el botón **Abrir** de la barra azul del Inspector o *Prefab → Abrir prefab* en la Jerarquía.
  - Escenario propio con el prefab solo: **Jerarquía a la izquierda, vista en el centro, componentes (Inspector) a la derecha**, Proyecto y Consola abajo. Con el cielo de la escena y una luz (que no salen en la Jerarquía).
  - Todo lo que se crea o se suelta va dentro de la raíz del prefab. Deshacer/rehacer propio de la pestaña.
  - **Guardar** (Ctrl+S o el botón de la banda azul) escribe el `.crprefab` y **todas sus instancias de la escena se actualizan** al volver, respetando sus cambios propios.
  - Cerrar con cambios pregunta *Guardar / Descartar / Cancelar*. Al salir del editor se guardan solos.
  - Play, abrir o crear escena y Exportar vuelven antes a la pestaña Escena.
- **Scripts en su pestaña**, a toda la ventana, con el **árbol de carpetas y assets a la derecha** (buscar, clic en un `.lua`/`.crshader` lo abre, doble clic en un prefab o escena lo abre, menú: nuevo script, copiar ruta, mostrar en el Proyecto).

### Plantilla MMO RPG (todo el juego en Lua)
- Nueva plantilla en el Hub: **MMO RPG**. El motor solo pone el mundo y la interfaz; **todos los sistemas son scripts de Lua** (`Heroe.lua`, `Enemigo.lua`, `Bot.lua`, `NPC.lua`) que se pueden leer y cambiar.
  - Mundo: pueblo (Villa Alba) con casas, plaza, mercado y capilla; Bosque Gris con lobos; campamento goblin con empalizada y chamanes; guarida del **Rey Goblin** (jefe con pisotón en área que avisa antes).
  - Combate: objetivo con **Tab** o clic mirando al enemigo, **Golpe**, **Bola de fuego** (con barra de lanzamiento que se interrumpe al moverse), **Curar** (nivel 2), **Torbellino** en área (nivel 3), pociones, maná, enfriamientos y enfriamiento global, críticos, ataque automático, números flotantes.
  - Progresión: experiencia y 10 niveles, estadísticas, **equipo** (arma y armadura), **inventario** de 20 huecos, **tienda** (comprar y vender), **5 misiones** (matar y recoger) con diálogos y marcas `!` sobre los personajes.
  - Enemigos con IA: patrulla, aggro, *leash* (vuelven a casa curándose), bolsas de **botín** y reaparición.
  - **Otros jugadores simulados** que cazan, mueren, reaparecen y hablan por el chat.
  - Interfaz: marcos de jugador y objetivo, barra de habilidades con enfriamientos, experiencia, chat, seguimiento de misiones, **minimapa**, inventario, personaje, diario, diálogos, muerte y ayuda (H).
  - La partida se guarda sola (Prefs); F9 la borra.

### Manual
- Manual completado: nuevo grupo **El editor** (interfaz y pestañas, vista Escena y gizmos, Jerarquía e Inspector, Proyecto e importación, Play y deshacer, herramientas de mundo, ventanas de herramientas, materiales, render y rendimiento, exportar y compilación, plantillas y atajos de teclado).
- Nuevo grupo **Componentes**: la referencia de los 46 componentes con **todas sus propiedades** (nombre, clave, tipo, valor por defecto, rango u opciones y ayuda), generada desde el propio motor con `cramion_docgen` para que no se quede atrás.
- 60 páginas y 616 entradas en el buscador.

### Corregido
- **Sombras de contacto** rehechas: recorren la profundidad **píxel a píxel** en pantalla (como las *screen space shadows* de Days Gone) y su rayo solo cubre lo que la cascada no resuelve (unos 10 texels de su mapa; *Largo máximo* es solo un tope). Antes el rayo de 0,5 m sacaba de la pantalla la sombra entera del personaje: se veía en **escalones**, duplicaba la de la cascada y **se deformaba al mover la cámara**. Sin TAA ya no usan ruido; con TAA la penumbra se suaviza con un ruido distinto cada frame. Se acabaron también los puntitos negros, el *acné* en troncos y cilindros y el negro de lejos.
- **Luz volumétrica (polvo)**: ahora también la ilumina el **cielo** desde todas direcciones (como el *Sky Light* de la niebla volumétrica de Unreal). Antes solo se veía mirando hacia el sol: de espaldas el polvo solo oscurecía.
- **Revisión de todos los shaders** (8.200 líneas):
  - Recorte alfa (`surface`, `skinned`, `voxel`): el `discard` iba antes de calcular derivadas (antialiasing especular, decals, mips); en los bordes de hojas y rejas salían valores indefinidos. Ahora se descarta al final.
  - Terreno: las capas se leían con mip automático dentro de una rama; en la mezcla entre capas salían brillos y costuras. Ahora con `textureGrad`.
  - Modelos con parallax: las texturas eligen el mip con la UV original (menos aliasing en los escalones del relieve).
  - Normales con **escala no uniforme** (cajas estiradas, esferas achatadas): se transformaban con la matriz del modelo tal cual y la luz caía mal; ahora con su inversa traspuesta (como ya hacía el trazado de rayos).
  - TAA: evitados píxeles blancos de un frame (*fireflies*) al deshacer la compresión de la historia.
  - Reflejos del agua: el ruido del trazado cambia cada frame (antes un patrón quieto). SSR: coordenada sin inicializar al salir de pantalla.

### Audio: efectos, oclusión y reverberación
- **Oclusión** en el **Audio Listener** (activar/desactivar con su casilla, `Audio.setOcclusion(on)` o `camara.audioOcclusion = false`): un sonido 3D con colliders en medio (paredes, una puerta cerrada, el techo de una casa) se oye **tapado**, más bajo y sin agudos, con fundido al abrir o cerrar. Ajustes: agudos tras una pared, volumen por pared, paredes como mucho, rapidez. No cuentan triggers, el propio sonido ni el cuerpo del jugador.
- **Efectos por Audio Source**: paso bajo, paso alto, eco (retardo, repetición, mezcla) y envío a reverberación; `entidad:setSoundEffect("lowpass", true, 800)`.
- **Audio Reverb Zone** (componente nuevo): esfera con tipo Habitación, Baño, Sala grande, Cueva, Estadio, Bosque, Bajo el agua o personalizado.
- **Paso bajo general** en el Audio Listener (bajo el agua, pausa): `Audio.setLowPass(true, 600)`.
- En Play, el Inspector del sonido dice cuántas paredes lo tapan y la Escena dibuja la línea al oyente (roja si está tapado), además del alcance y las zonas.

### Configuraciones de compilación
- *Archivo → Configuraciones de compilación…*: perfiles de exportación como los Build Profiles de Unity (`ProjectSettings/BuildConfigs.json`, viajan con el proyecto). Nueva, Duplicar, Borrar; doble clic o *Usar esta configuración* la hace activa.
  - **Nombre del juego**: el `.exe`, la carpeta exportada, el título de la ventana y la pantalla de carga.
  - **Versión** (opcional en el título de la ventana).
  - **Icono de la app**: suelta una imagen del Proyecto (PNG, JPG, TGA, BMP) o elige un `.ico`. Se escribe **dentro del .exe** (16 a 256 px): Explorador, barra de tareas y ventana.
  - **Escena inicial**, **ventana** (maximizada, pantalla completa sin bordes o ventana con tamaño), **static batching** y **Mostrar FPS** de desarrollo.
- La ventana *Exportar juego* elige la configuración y muestra el `.exe` que va a salir.

### Pintar prefabs
- **Herramienta de pintado de prefabs** (botón **B Pintar** en la vista Escena, tecla **B**, o *Ventana → Pintar prefabs*), como el Foliage de Unreal pero con prefabs de verdad: árboles con su script, rocas con su collider, cofres…
  - **Grupos de pintado** (`.crpaint` en `Assets/PaintGroups`): arrastra prefabs del Proyecto a la ventana. Cada uno con su **peso** (cuántos salen frente a los demás), **escala aleatoria**, **alinear al suelo** (0 = vertical como un árbol, 1 = sigue la pendiente como una roca), **giro aleatorio** y **hundir** en el suelo.
  - Se pinta **todo el grupo** o **solo el prefab elegido**.
  - **Pincel**: radio (Ctrl + rueda o `[` `]`), **densidad** por 100 m² (repasar no amontona: rellena hasta esa densidad), **separación mínima** entre objetos y **pendiente máxima**. **Mayús + arrastrar borra.**
  - Apoya en los colliders (también Mesh Collider) y en los terrenos; lo ya pintado no cuenta como suelo.
  - Cada trazo es un paso de deshacer. Lo pintado queda bajo *Pintado - &lt;grupo&gt;* en la Jerarquía, como instancias normales de su prefab. *Seleccionarlos* y *Borrar todos* en la ventana.
- MCP: herramienta `paint_prefabs`.

### Volúmenes de post-procesado
- El componente **Post-procesado** es ahora un **Volume como el de Unity**: forma **Global**, **Caja** o **Esfera** (con la posición, el giro y la escala de la entidad).
  - Con la cámara dentro de una caja o esfera se usa ese volumen; fuera, el global. La **distancia de mezcla** hace la transición suave al acercarse; **peso** y **prioridad** deciden cómo se combinan varios.
  - Un volumen local **solo cambia las secciones que marca como Sobrescribir** (exposición, bloom, color, viñeta, lente, efectos…); el resto sale del global.
  - *GameObject → Volumen de post-procesado → Global / Caja / Esfera*. Uno nuevo empieza con el aspecto del global actual.
  - En la Escena se ven la forma y, más tenue, la zona de transición.
- Las escenas antiguas siguen igual: su post-procesado es global.

### Agua
- **Olas interactivas** (como en Red Dead Redemption): **todo lo que tiene collider** (Box, Sphere, Capsule, Mesh, Wheel), Rigidbody o es un agente de navegación y cruza la superficie empuja el agua, se mueva por física, por script, por animación o con el gizmo del editor. Lo que está **quieto** y atraviesa el agua (postes, rocas, pilares; hasta 10 m) es un **obstáculo: las ondas chocan y rebotan**. Al caer, **salpicadura con espuma** según la velocidad; al moverse, **estela**. Las ondas se propagan, se cruzan, rebotan y se apagan solas. Simulación de la ecuación de onda en 48 × 48 m alrededor de la cámara (celdas de 25 cm, 60 Hz), en el océano, los lagos y los ríos.
- **Ríos**: la corriente sigue el camino de los puntos. Antes el rizado y la espuma se movían en el mundo con la dirección de cada sitio y se deformaban cada vez más (y más cuantos más puntos). La curva es ahora un Catmull-Rom centrípeto: sin bucles ni panzas con puntos a distancias desiguales.
- **Cáusticas bajo el agua**: con la cámara dentro del agua, el fondo y los objetos sumergidos reciben las cáusticas del sol (con su sombra). Antes solo se veían desde fuera.

### Animación
- **Cinemática inversa (IK)**: componente **IK** (*Animación*) sobre la pose animada de cualquier modelo con esqueleto.
  - **Manos y pies** a una entidad objetivo (IK de dos huesos analítico, exacto), con **codo/rodilla hacia** otra entidad (pole), peso y *copiar giro*.
  - **Mirar**: la cabeza (y un poco el cuello) sigue a una entidad, con ángulo máximo.
  - **Pies en el suelo**: cada pie se apoya en lo que tiene debajo (escaleras, pendientes, rocas), la cadera baja para que la pierna llegue y el pie se inclina con el suelo. No cuenta el propio personaje.
  - **Cadenas** de dos huesos por nombre para esqueletos no humanos (colas, brazos robóticos).
  - Los objetivos son entidades normales: se mueven a mano, por script o con física.
- **Animación procedural**: componente **Animación procedural** (*Animación*), encima de la animación o sin ella.
  - **Huesos con muelle** (pelo, colas, capas, antenas, pendientes): cada cadena se mueve con inercia, gravedad, rigidez y amortiguación, sin estirarse, y **choca con el cuerpo** (cabeza, pecho, cadera). Al mover el personaje se quedan atrás y rebotan.
  - **Patas procedurales** (arañas, cangrejos, robots, dragones): cada pie se queda clavado en el suelo y **da un paso en arco** cuando el cuerpo se aleja; los grupos se turnan (diagonales, trípode), el pie se adelanta según la velocidad y el cuerpo sube, baja y se inclina con el suelo que pisan.
  - **Capas**: **respirar** (el pecho sube y baja), **inclinarse** al acelerar, frenar o girar, y **ruido** suave en huesos sueltos (antenas, colas en reposo).
  - Orden por frame: animación → capas y patas → IK → huesos con muelle.
- **Humanoides y retargeting** (como el Avatar de Unity): los esqueletos humanos se reconocen solos por los nombres de sus huesos (Mixamo, Unreal, Blender/Rigify, 3ds Max Biped y nombres sueltos). Un clip `.cranim` de un humanoide **se usa en otro humanoide** aunque sus huesos se llamen distinto, tengan otros ejes, otras proporciones, otra escala (cm/m), otro "arriba" (Z o Y) o esté en pose A en vez de T: se convierte al cargarlo, hueso a hueso en el espacio del personaje, y la cadera se desplaza escalada por la altura. Los `.cranim` guardan ahora su esqueleto (vuelve a extraer los antiguos para convertirlos).
- **Navegador**: los modelos y clips humanoides llevan una **insignia** (círculo con el icono de SkinnedMesh) abajo a la derecha de su miniatura.

### Editor
- **Abrir un proyecto no congela la ventana**: el Hub se cierra y sale un diálogo con el logo del motor y una **barra de progreso** (como Unity): abrir el proyecto, leer la escena, cargar los modelos **en otro hilo** (con el nombre de cada uno), subirlos a la GPU y preparar la física.
- **Add Component rediseñado**: ventana grande centrada en la pantalla, buscador con el **filtro de categorías en un desplegable** a su derecha y los componentes en una **rejilla de fichas** con icono, nombre y el color de su categoría (como Unreal). Enter añade el primero.

## 0.5.1

### Gizmos
- **El gizmo de mover, rotar y escalar se dibuja siempre encima y opaco**, como en Unreal. Antes se probaba contra la profundidad de la escena y, dentro del objeto seleccionado (casi siempre), se veía al 22 % y desteñido.
- **Más grueso y más grande**: líneas de 5 px, flechas y cubos más gordos, un 30 % más grande en pantalla. Color plano, sin iluminación.
- **Rotar**: sin el círculo blanco de la vista, que estorbaba. Nueva **bola central de giro libre**: al arrastrarla el objeto gira alrededor de su centro siguiendo el ratón (horizontal = eje arriba de la vista, vertical = eje derecho), como en Blender o Maya. Un paso de deshacer por arrastre.
- **Mesh Collider completo**: antes solo se dibujaban sus primeros 12.000 triángulos y faltaba un trozo. Ahora, si hay demasiados, se reparten por toda la malla. Los triángulos se guardan por objeto en vez de pedirlos a Jolt en cada frame.

### Materiales
- **Nuevos mapas en el `.crmat`**, los de los packs de escaneos (Megascans, Poly Haven…):
  - **Altura / Displacement** con *parallax occlusion mapping*: piedras y grietas con profundidad real al mirarlas de lado. *Relieve → Profundidad (m)*, en metros (0,03 = 3 cm), igual para un suelo que se repite que para un escaneo con toda la malla en una sola UV. Desplazamiento limitado en ángulos rasantes (sin estirones) y se apaga solo a más de 30 m.
  - **Cavidad**: las grietas pierden reflejo y luz ambiental (con *Fuerza cavidad*).
  - **Specular**: reflectancia por píxel (0,5 = la del material), como el Specular de Unreal.
  - **Gloss**: brillo = 1 − rugosidad, si no hay mapa de rugosidad.
  - **Bump**: relieve fino convertido en normal map, si no hay normal map.
- **Crear material desde una imagen** encuentra solas sus compañeras por el sufijo: `_Normal`, `_Roughness`, `_AO`, `_Displacement`, `_Cavity`, `_Specular`, `_Gloss`, `_Bump`… Un pack de Megascans queda completo arrastrando su BaseColor.
- Los mapas nuevos se empaquetan en las texturas que ya existían (sin más memoria de descriptores ni pasadas).

### MCP
- Nueva herramienta `set_gizmo` (mover, rotar, escalar o ninguno; ejes locales o del mundo).
- `create_material` acepta `from_image` (busca las compañeras de un pack) y los mapas nuevos: `height_texture` y `height_scale`, `roughness_texture`, `occlusion_texture`, `cavity_texture`, `specular_texture` y `gloss_texture`.

## 0.5.0

### Rendimiento
- **Presupuesto adaptativo** (nuevo sistema del motor, activado por defecto). Objetivo: que el juego llegue a los FPS pedidos en cualquier PC, también de gama baja.
  - **Perfil de hardware** al arrancar (VRAM y GPU integrada o dedicada): Bajo, Medio, Alto o Ultra. De él dependen la resolución del mapa de sombras y desde dónde empieza el gobernador.
  - **Mapa de sombras según el hardware**: 1536 / 2048 / 4096 / 6144 por cascada. Antes siempre era 6144, **576 MB de VRAM**; en un PC de gama baja ahora ocupa 36 MB. Se puede fijar a mano en *Gráficos → Sombras*.
  - **Gobernador de frame con costes aprendidos**: cada frame lee el tiempo de GPU de cada pasada. Si no llega al objetivo, baja un paso la palanca que más milisegundos ahorra por cada punto de calidad perdido. Las palancas son: detalle de mallas, detalle de sombras, luz volumétrica, sombras de contacto, SSAO, reflejos, GI y resolución interna con FSR 1. Tras cada cambio mide lo que ahorró de verdad en ese PC y en esa escena, y lo recuerda. Con eso sube la calidad cuando sobra tiempo, sin pasarse y sin oscilar. Nunca activa algo que el usuario apagó.
  - **Sombras por texel**: cada cascada elige el LOD que su resolución puede mostrar y no dibuja los objetos más pequeños que su texel. Ya no depende de la cámara, así que las cascadas guardadas dejan de rehacerse al moverse. Con el detalle de sombras bajado, las cascadas lejanas se actualizan con menos frecuencia.
  - **Objetos de menos de un píxel**: la cámara no los dibuja, pero su sombra sigue. Los LODs se eligen con la resolución interna: si baja la resolución, las mallas también pueden ser más simples.
  - *Gráficos → Presupuesto adaptativo*: activarlo, FPS objetivo (30 a 240) y estado en vivo de cada palanca. Se guarda en `Graphics.ini` y el juego exportado lo usa igual.
  - MCP: herramienta `graphics_settings`; `performance_stats` incluye el estado del presupuesto.
  - Prueba con una GPU simulada 3 veces más lenta: pasa de 21 a 60 FPS en 3,5 s y se queda estable.
- **LODs automáticos** (meshoptimizer). Al cargar cada modelo estático de más de 4096 triángulos se generan hasta 5 niveles simplificados, cada uno con la mitad de triángulos que el anterior. En follaje también se quitan las briznas y hojas que ya no se ven. En cada frame, cada objeto se dibuja con el nivel más simple cuyo error en pantalla no pasa de **1 píxel** (el criterio de Nanite): no hay distancias que ajustar. **Las sombras usan el mismo nivel.** Se ajustan en *Post-procesado → Rendimiento* (*LODs automáticos* y *Error máximo (px)*). El `.crdata` no cambia: los LODs viven solo en memoria. *Estadísticas* y la herramienta MCP `performance_stats` muestran los triángulos con LOD y cuántos objetos van simplificados. Ejemplo real: una mata de hierba de 185.850 triángulos baja a 5.807 a lo lejos, y un acantilado escaneado de 2 millones, a 62.000.
- **Static batching al exportar.** Nueva casilla **Static** en el Inspector (junto al nombre; pregunta si aplicarla a los hijos). Al exportar con *Combinar mallas estáticas*, las mallas Static de cada escena se hornean en un lote: materiales iguales por contenido (factores y bytes de las texturas) en una sola llamada, clusteres espaciales para que el culling en GPU siga descartando lo que no se ve, una pieza por modo de sombra, `.crmat` conservados. No se combinan las mallas repetidas y grandes (ya van instanciadas), ni lo que tiene Animator, Script o un Rigidbody no estático. El proyecto no cambia: solo la copia que va al juego.
- **Batching de sombras y del pase de cámara**: los tramos visibles seguidos del buffer de índices se dibujan en una llamada (sombras opacas: sin importar el material; recortadas y cámara: por material). Nuevo contador *llamadas de sombras* en Estadísticas.
- **Clusteres independientes de la escala**: rejilla relativa a la caja de cada malla y un mínimo de 1024 triángulos por cluster. Antes, un FBX en centímetros acababa en celdas de 5 cm (miles de submallas). Los `.crdata` antiguos se reagrupan al cargarlos; reimportar los guarda así.
- **Agua**: las ondas más finas que un píxel no calculan senos ni cosenos (solo cuentan como rugosidad).

### Iluminación
- **Caché de radiancia en el mundo** (como SHaRC de NVIDIA o la *surface cache* de Lumen). Es una tabla hash de celdas en el mundo, de 25 cm cerca de la cámara y más grandes lejos, con una entrada por cada orientación de la superficie. Guarda la luz que llega a cada superficie: el cielo que ve de verdad más la luz rebotada.
  - La llenan los píxeles de la GI y rayos secundarios desde los impactos, que cubren también lo que está fuera de pantalla.
  - Cuando un rayo de GI o de reflejo choca, lee esa celda. Antes suponía que cualquier punto veía la mitad del cielo.
  - **Rebotes infinitos**: cada frame encadena un rebote más sobre la luz guardada. Las esquinas y los interiores reciben la luz que les corresponde.
  - **Sin fugas de cielo** en cuevas y habitaciones.
  - **Más estable**: la media vive en el mundo, no en los píxeles.
  - Se vacía sola al desplazarse el origen flotante o al cambiar de escena. Solo con trazado de rayos.
- **Sombras de contacto** del sol en pantalla (12 pasos, a menos de 60 m, solo donde la cascada no ha oscurecido ya). *Post-procesado → Efectos → Sombras de contacto* y su largo.
- **Compensación de energía por dispersión múltiple** (Kulla-Conty / Fdez-Agüera) en la luz directa y en el IBL.
- **Sol con tamaño angular** (0,53°) en el especular: sin puntos blancos que parpadean en superficies pulidas.
- **Oclusión del horizonte** en los reflejos con normal maps.

### Agua
- **Oleaje JONSWAP**: 24 ondas de Gerstner muestreadas del espectro del mar real (direcciones repartidas alrededor del viento, fases pseudoaleatorias). *Altura de ola* = altura significativa. Misma tabla en el shader y en la flotación.
- Olas, texturas del terreno y nubes **relativas a su propio origen**: no saltan con el origen flotante.
- **Reflejo del cielo real** (con las nubes volumétricas) cuando esa parte del cielo está en pantalla; sin la franja marrón en el horizonte.
- **Dispersión subsuperficial** (modelo de Atlas, GDC 2019) en las crestas a contraluz.
- **Espuma** de vetas y burbujas de ruido deformado (sin celdas poligonales) y solo donde rompen las olas grandes.
- Brillo del sol con GGX completo (Smith, Fresnel en el ángulo medio) y rizado fino repartido alrededor del viento.
- El agua recibe la **niebla por altura** y la **luz volumétrica** (hasta su superficie), como el resto de la escena.

### Mundos grandes
- **Origen flotante**: a más de 2 km de la cámara el mundo se desplaza en pasos de 1024 m. Se desplazan entidades, cuerpos de Jolt (con su velocidad), partículas, audio (sin saltos de Doppler), cinemáticas, secciones de bloques, sonda de reflexión y caches del renderizador; la navegación convierte en sus consultas sin rehacer la malla; los bloques guardan coordenadas absolutas. La escena guarda `origin` y posiciones relativas (sin perder precisión); deshacer, Play y abrir escenas vuelven a alinear. El Inspector muestra la *Posición real*.
- Lua: `Scene.origin()`, `Scene.toAbsolute(pos)`, `Scene.toLocal(x, y, z)` y `function Script:OnOriginShift(offset)`.

### Editor
- **Componente Profiler** (Add Component → Depuración): FPS medio y mínimo, CPU (ms y %), GPU (ms por timestamps y %), nombre de la GPU, RAM y gráfica; esquina, tamaño y opacidad configurables.
- **Importación con progreso**: ventana con el archivo, la etapa y el porcentaje real (assimp informa de su avance); cola de importación de 2 en 2.
- **Reimportar (combinar mallas)** en el clic derecho de un modelo: junta por material las piezas de modelos muy troceados (una palmera pasa de ~70 objetos a 2), rehace sus instancias en la escena conservando transformaciones, materiales y Static, y guarda una copia del original en `Library/ReimportBackups`. Los modelos nuevos con más de 16 piezas se combinan solos al importarlos.
- **Miniatura del proyecto en el Hub**: se captura la vista Escena en `Library/thumbnail.png` al abrir el proyecto y al guardar la escena.
- **Botón Gizmos** (tecla G) en la barra de la vista Escena: oculta los iconos y ayudas (luces, cámaras, decals, física, cinemáticas, agua y asas de colliders). El gizmo de mover, rotar y escalar se queda, y la navegación sigue con su botón Nav.
- MCP: herramientas `performance_stats` y `reimport_model`.
- Arrastrar un material con varios objetos seleccionados: el Inspector no cambia (se abre al soltar sin arrastrar) y el material se aplica a todos.

### Juego exportado
- **Carga de escenas por etapas** con pantalla de carga: leer la escena, modelos en otro hilo, subida a la GPU, física/navegación/scripts. También con `Scene.load()`.
- **Mensaje claro** si la GPU se queda sin memoria de vídeo (en vez del código de Vulkan).
- **Cámara orbital por defecto** si la escena no tiene ninguna Camera.
- El reagrupado de modelos antiguos y el static batching evitan escenas que antes agotaban la VRAM.

### Notas
- El agua sigue siendo Gerstner (24 ondas), no un océano FFT.
- Los scripts que guardan posiciones del mundo en variables deben usar `OnOriginShift` (o `Scene.toAbsolute` al guardar partidas).
- Un objeto marcado Static no debe moverse, ocultarse ni cambiar de material por script en el juego exportado: su malla va dentro del lote.

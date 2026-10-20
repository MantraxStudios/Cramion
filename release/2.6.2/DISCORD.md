## 🚀 Cramion 2.6.2 ya está aquí

Llega después de la 2.5.1 con cinco rondas de trabajo: mundos que entran y salen sin tirones, luces sin tope y un editor rápido con escenas grandes.

**🌲 Streaming sin tirones**
`Scene.instantiateAsync` crea los prefabs por tandas en otros hilos, `Scene.instantiate` es hasta 15 veces más rápido y hay pool de objetos. Adiós al parón de 1-2,3 s del primer árbol.

**⚡ Rayos y sonda**
La escena de rayos es incremental: de 7 reconstrucciones seguidas por ciclo a que cada modelo entre solo. La sonda de reflexión ya no para la CPU (p99 de 17,5 a 12,4 ms). Presupuesto de VRAM con LRU.

**💡 Luces y materiales**
Luces locales sin tope por clústeres, transparentes iluminados, color de vértice, parámetros por instancia, decals con normal y emisión, y LOD con fundido.

**🎨 Shaders y VFX**
`domain transparent` iluminado con refracción y ruidos nuevos. VFX Graph completo: cintas de verdad, luz por partícula, eventos en la GPU (`Effects.pushEvent`), mallas con LOD, distorsión y escalabilidad por tipo de efecto.

**🛠️ Editor**
Deshacer por diferencias, regiones cargadas de World Partition, recarga en caliente, autoguardado con recuperación y adiós al tirón de 200 ms cada 2 s.

**🎮 Y además**
Mando en Windows y Linux con vibración, `onEnable`/`onDisable` en C++, scripts en el propio proceso del juego exportado y MCP con token.

⚠️ **Ojo**: vuelve a exportar los juegos con `.crshader`; `domain transparent` ahora se ilumina (`domain transparent unlit` es el de antes); y en el juego exportado de Windows los scripts van en el propio proceso (`script.cpp.InProcess=false` para volver). Los clientes HTTP del MCP necesitan el token.

📥 El editor te avisa solo para actualizar, o descárgalo en GitHub: https://github.com/MantraxStudios/Cramion/releases/latest

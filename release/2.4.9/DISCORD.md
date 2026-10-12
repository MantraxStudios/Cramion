## 🚀 Cramion 2.4.9 ya está aquí

Blueprints de actor, cliente WebSocket, hierba realista y un mundo que se carga en otros hilos. (La 2.4.7 y la 2.4.8 no se publicaron.)

**🧩 Blueprints de actor**
- Un prefab con su Event Graph y su propia pestaña: Componentes, Detalles, Ventana gráfica y Resultados de compilación
- Doble clic en un modelo para crear un Blueprint
- Editor de nodos estilo Unreal: snap mientras arrastras, reroutes, minimapa, comentarios, búsqueda sensible al contexto y Compilar (F7)
- Arrastra un componente al Event Graph y se referencia a sí mismo en cada instancia
- 56 nodos nuevos: Switch, For Loop with Break, MultiGate, texto, listas, Line Trace...

**🌐 Cliente WebSocket**
- `ws://` y `wss://` desde C++ (`WsClient`) y Visual Scripting (On WebSocket)
- Windows, Linux y Android, con reconexión y heartbeat

**🌿 Vegetación**
- Hierba con volumen, sombreado propio y auto-sombra dentro del césped
- Árboles con hojas mate y auto-sombra de la copa

**⚡ Streaming en otros hilos**
- Terreno, física, navegación y World Partition se cargan fuera del hilo principal
- El servidor dedicado carga alrededor de cada jugador
- Filtrado anisotrópico y sesgo de mips en los ajustes gráficos

**🛠️ Fallos corregidos**
- Pruebas con `co_await` que no se ejecutaban
- Editor a 10 FPS sin foco durante Play
- Las áreas ya no cambian de editor al cambiar de espacio de trabajo
- Niebla, nubes, escalones entre teselas, pausa, duplicar y pegar, y más

**⚠️ Antes de actualizar**
- Las pruebas con `co_await` ahora se ejecutan de verdad: alguna puede fallar
- La recarga en caliente no conserva las corrutinas: relánzalas en `onAfterReload`
- El editor de Visual Script ya no guarda solo (F7 o Guardar)

📥 Descarga y notas completas: https://github.com/MantraxStudios/Cramion/releases/tag/v2.4.9

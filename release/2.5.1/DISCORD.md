## 🚀 Cramion 2.5.1 ya está aquí

La versión más grande desde la 2.4: el render deja de repetir trabajo por frame, llegan los lightmaps y entran 39 parches de rendimiento de la comunidad (¡gracias, Keryan!).

**✨ Nuevo logo**
Nuevo emblema en el editor, la web y los ejecutables, y nuevo banner «Game powered by Cramion» al arrancar tus juegos.

**💡 Lightmaps UV2**
UV automáticas con xatlas y un lightmapper propio con rebote y luces Horneadas.

**🧠 Memoria con VMA**
Buffers y texturas sin una asignación por recurso, y cada modelo sube en un solo lote por una cola de transferencia. Menos tirones al cargar.

**🏙️ Ciudades más rápidas**
Las sombras ya no se redibujan cuando entra o sale un objeto, se descarta lo que no puede verse y el terreno solo las rehace si cambia. Las variantes `.crmat` comparten BLAS y vértices.

**🎨 Render**
Vidrio con oclusión Hi-Z, luces por teselas, early-Z, LOD de personajes con esqueleto y texturas BC7 para UI, árboles y terreno. Gama baja más ligera (GI y sol horneados, ríos y ondas más baratos).

**🖱️ UI y ventana**
El clic ya no atraviesa la interfaz, `UIInputField` con selección y portapapeles, `UIImage` con 9-slice, modo sin bordes y tope de FPS (`fps_limit`).

**🌍 Y además**
World Partition con varias rejillas y Data Layers, red para mundos de más de 2 km, Blueprints con funciones, macros y Timeline, vídeo en Android y `--run-tests` en el juego exportado.

⚠️ **Ojo**: el protocolo de red pasa a la v2 (los juegos y servidores anteriores no se conectan con los nuevos), y los scripts ya no reciben el clic que cae sobre la UI.

📥 El editor te avisa solo para actualizar, o descárgalo en GitHub: https://github.com/MantraxStudios/Cramion/releases/latest

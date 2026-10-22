## 🚀 Cramion 2.6.3 ya está aquí

Una versión para los efectos: nítidos con TAA, sin estela y sin perderse en combate.

**✨ Nitidez**
El VFX Graph, las partículas, la lluvia y los sprites se dibujan con el jitter del TAA, como lo opaco. La máscara reactiva quita la estela de proyectiles, chispas y humo, tanto con el TAA del motor como con FSR 3. Además hay samplers con anisotropía y CVars `render.taa.*`.

**⚔️ Habilidades completas**
Los efectos terminados ya no ocupan sitio, los grupos (`VfxGroup` o el prefab) entran enteros o no entran, y los efectos del jugador local nunca se descartan. `Physics.raycast` puede ignorar los triggers.

**📦 Sin topes ocultos**
Hasta 256 texturas y 128 mallas de partículas, que se sueltan solas cuando nadie las usa. Si se llega al tope, avisa en la consola y en el editor.

**🎨 Dibujo**
Partículas de malla con su material, decals que tiñen la hierba, «Apartar hierba» sin física, fundido suave sin ajedrezado, una cinta por proyectil, colisiones fuera de la vista y KillBox en el mundo.

**🛠️ Herramientas**
Recarga en caliente de los `.crvfx` y de las texturas, `.crshader` con 16 propiedades, compilación de C++ sin bloquear el editor y MCP asíncrono (`cpp_compile_status`).

**🖱️ Cursor**
`Input.setCursorLockMode` («locked», «confined») y `setCursorVisible`.

⚠️ **Ojo**: vuelve a exportar los juegos con `.crshader`. La salida Malla sin `.crmat` usa ahora la textura del modelo. `cpp_compile` por MCP devuelve el resultado en `cpp_compile_status`.

📥 El editor te avisa solo para actualizar, o descárgalo en GitHub: https://github.com/MantraxStudios/Cramion/releases/latest

# Cramion 2.4.13

**Novedad sobre la 2.4.11: llega la Cramion Store, con assets gratis, cuentas y un Package Manager en el editor como el de Unity (instalar, actualizar o volver a una versión anterior) sobre el nuevo formato `.crwpack`. Y el horneado de luz vuelve a ver los objetos Static de los modelos importados: una ciudad entera marcada Static ya no da «Nada que hornear».**

El actualizador del editor la ofrece sola a quien ya tiene la 2.4.11.

## Cramion Store

Una tienda de assets **gratis** de la comunidad: modelos, materiales, audio, efectos, scripts, plantillas...

- **Web**: catálogo con búsqueda, categorías y orden, y una ficha por paquete con sus versiones, cambios, dependencias y descarga.
- **Tu cuenta**: registro e inicio de sesión. Con ella creas tus paquetes, subes sus versiones arrastrando el `.crwpack`, pones portada, retiras o borras versiones y cambias tu perfil. Subir es gratis.
- **Administración** para los moderadores: usuarios y roles, paquetes destacados u ocultos y registro de actividad. Funciona también en el móvil.
- **Servidor propio**: la API (`web-api/cramion_api.py`) es un solo archivo de Python sin dependencias.

## Package Manager en el editor

**Ventana › Package Manager**, como el de Unity.

- **En el proyecto** (lo instalado, con aviso si hay versión nueva) y **Cramion Store** (el catálogo).
- **Versiones**: eliges una y el botón es **Instalar**, **Actualizar a**, **Bajar a** o **Reinstalar**. **Quitar** lo borra del proyecto.
- **Dependencias**: las que pide el paquete se instalan solas.
- **Seguridad**: cada descarga se comprueba con su SHA-256 antes de instalarla.
- Los paquetes van a `Assets/Packages/<nombre>`, lo instalado queda en `ProjectSettings/Packages.json` y cada versión se guarda en `Library/PackageCache`, así que volver a una versión ya usada no descarga nada.
- **Crear un paquete**: Archivo › Exportar carpeta como paquete .crwpack... y luego súbelo en la web. Lleva los modelos ya importados, así que tus escenas no pierden la referencia al cambiar de versión.
- **MCP**: una IA puede buscar, instalar, actualizar, bajar de versión, quitar y exportar paquetes (`packages`, `package_install`...).

## Formato .crwpack

El contenedor propio de los paquetes:

- Manifiesto JSON legible sin descomprimir.
- Bloques de 4 MiB comprimidos con zstd en todos los núcleos, cada uno con su CRC32.
- Índice con su propio CRC.
- Al extraer se rechazan las rutas peligrosas.

`crwpack.exe` (ahora en el zip) hace lo mismo desde la consola: `pack`, `unpack`, `list`, `info` y `verify`.

## Horneado de luz: los modelos Static no entraban

**Qué pasaba**: con el editor abierto, el renderizador se queda con la malla de cada pieza que dibuja, para no tenerla dos veces en memoria, y en el gestor de assets queda una copia vacía. El horneado leía esa copia, así que los modelos importados (y las primitivas) no aportaban ni un triángulo. El aviso decía «Nada que hornear... Marca los objetos como Static» aunque toda la escena ya fuera Static. Pasaba desde que existe el horneado.

**Qué se ha corregido**
- La malla sale de la escena del renderizador; si no está, se relee del disco solo la geometría, sin texturas ni copias que se queden en memoria. En una casa de 101 piezas Static se pasa de 0 a 2.877.697 triángulos.
- **Informe claro**: cada malla entra o sale con su motivo («sin geometría», «transparentes», «malla vacía», «sin marcar Static»). Solo pide marcar Static cuando así entrarían más mallas.
- **Vidrio y agua** ya no tapan la luz, como en las sombras en tiempo real.

## Ciudades enteras

- **Volumen automático**: cabe en el tope de sondas (antes fallaba con «demasiadas sondas») y empieza en el suelo. Cubre el terreno si cabe y, si no, va alrededor de las mallas.
- **Lightmap de superficie**: si no cabe, se sabe al momento. Encendido automáticamente, agranda el texel en vez de fallar.
- **Luces horneadas fuera de los Light Probe Volume**: ahora llegan también fuera de los volúmenes. Antes, por ejemplo, el tejado y lo alto de las paredes se quedaban sin sol horneado.
- **Sin memoria** ya no cierra el editor: el horneado falla con un mensaje.
- No se hornea en Play.

## Otras herramientas arregladas

Leían la misma copia vacía:

- **Fracturar** ya no dice «No salió ningún trozo».
- Los **Blueprints** muestran el esqueleto y los huesos.
- Los **materiales por pieza** de un modelo se asignan.
- **Reimportar** conserva los `.crmat` de cada hueco.
- El bloque **Position Mesh** del VFX Graph vuelve a sacar partículas de la malla.
- Los **trozos de la destrucción** recuperan el color del modelo.
- Los **MeshCollider** de objetos ocultos vuelven a tener forma.

## Qué tienes que hacer

- **Vuelve a hornear** las escenas con modelos importados: el `.crbake` anterior se hizo sin ellos.
- **Vidrio**: los interiores detrás de cristaleras salen más claros en el horneado.
- **Navegación**: en el juego exportado, los MeshCollider del lote estático entran ahora en la malla de navegación que se construye al jugar, como ya pasaba en el editor.
- Lo demás se abre igual que en la 2.4.11.

## Descarga

1. Descarga **`Cramion-win64.zip`** y **`Cramion-win64.zip.sha256`** de los archivos de esta versión.
2. Comprueba el zip. En PowerShell, en la carpeta de descarga:

   ```powershell
   (Get-FileHash .\Cramion-win64.zip -Algorithm SHA256).Hash.ToLower()
   Get-Content .\Cramion-win64.zip.sha256
   ```

   Los dos valores deben coincidir con:

   ```
   e1e76a67d2d18d9b201636ad7c50b6d4b5f51704efc94dda2c2b80a2be6a0990
   ```

   En Linux o Git Bash: `sha256sum -c Cramion-win64.zip.sha256`.
3. Descomprime y abre `CramionEditor.exe`. Si ya tienes Cramion, el actualizador del editor ofrece la 2.4.13.

Requisitos: Windows 10/11 x64 y una GPU con Vulkan.

La única descarga oficial es la de esta página, y es gratis. El registro completo está en [CHANGELOG.md](https://github.com/MantraxStudios/Cramion/blob/main/CHANGELOG.md).

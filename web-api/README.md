# Cramion Store: API de cuentas y paquetes

`cramion_api.py` es el servidor de la Cramion Store. Gestiona las cuentas, los paquetes `.crwpack`, las versiones, las descargas y la administración. Además sirve la web (`docs/`), así que **reemplaza al `python3 -m http.server`** que servía la página.

- **Requisitos:** solo Python 3.10 o superior, sin `pip install`. Usa sqlite3, scrypt y http.server.
- **Web:** `docs/store/` (catálogo, cuenta, panel y admin) llama a la API en `/api` del mismo dominio.
- **Editor:** el Package Manager (`Ventana › Package Manager`) usa `https://cramion.mantraxtools.store/api`.
- **Herramienta de consola:** `crwpack.exe` crea, extrae y verifica paquetes. Va en el zip del motor, junto al editor (al compilar, en `build-release/`).

## Poner en marcha en el servidor (Ubuntu)

```bash
# 1. Copiar la API junto a la web (la web es la carpeta docs/ del repo)
scp web-api/cramion_api.py rhythm@192.168.1.57:~/cramion-api/

# 2. En el servidor: parar el "python3 -m http.server" que servía la web y arrancar esto
cd ~/cramion-api
CRAMION_SUPERADMIN=tu@email.com python3 cramion_api.py serve \
    --static ~/Escritorio/cramionweb --port 8000 --data ~/cramion-data
```

Usa el **mismo puerto** que tenía el `http.server`, así nginx y Cloudflare no cambian. Si era otro puerto distinto de 8000, pásalo con `--port`.

### Superadmin

Hay dos formas de tener la cuenta con el rol máximo:

- Arranca el servidor con `CRAMION_SUPERADMIN=tu@email.com` (o `--superadmin`) y regístrate en la web con ese email. La cuenta nace como superadmin.
- Si la cuenta ya existe, ejecuta:

```bash
python3 cramion_api.py --data ~/cramion-data superadmin tu_usuario
python3 cramion_api.py --data ~/cramion-data users              # lista de cuentas
python3 cramion_api.py --data ~/cramion-data passwd usuario NUEVA  # resetear una contraseña
```

El superadmin cambia roles (`user`, `admin`, `superadmin`) y borra cuentas. Los admin moderan: suspenden usuarios, ocultan, destacan y borran paquetes. El panel está en `/store/admin.html` y funciona en el móvil.

### Como servicio (systemd)

`/etc/systemd/system/cramion-store.service`:

```ini
[Unit]
Description=Cramion Store (web + API)
After=network.target

[Service]
User=rhythm
WorkingDirectory=/home/rhythm/cramion-api
Environment=CRAMION_SUPERADMIN=tu@email.com
ExecStart=/usr/bin/python3 /home/rhythm/cramion-api/cramion_api.py serve --static /home/rhythm/Escritorio/cramionweb --port 8000 --data /home/rhythm/cramion-data --quiet
Restart=always

[Install]
WantedBy=multi-user.target
```

```bash
sudo systemctl daemon-reload && sudo systemctl enable --now cramion-store
journalctl -u cramion-store -f
```

### nginx (si la web la sirve nginx en vez de Python)

Si quieres que nginx siga sirviendo `docs/`, deja la API en otro puerto (por ejemplo `--port 8010`, sin `--static`) y redirige solo `/api/`:

```nginx
location /api/ {
    proxy_pass http://127.0.0.1:8010;
    proxy_set_header Host $host;
    proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    client_max_body_size 20m;       # las subidas van en trozos de 8 MB
    proxy_request_buffering off;
    proxy_read_timeout 300s;
}
```

## Límites y notas

- **Subidas por trozos de 8 MiB.** Cloudflare corta los cuerpos de más de 100 MB, así que un paquete puede pesar hasta `--max-package-mb` (2048 por defecto).
- **Espacio gratis por usuario:** `--quota-mb`, 10240 por defecto. Los admin no tienen límite.
- **Datos:** `--data` guarda `cramion.db` (SQLite) y las carpetas `packages/<nombre>/<version>.crwpack`, `images/` y `uploads/` (trozos a medias). Para la copia de seguridad, copia esa carpeta entera.
- **Seguridad:**
  - Contraseñas con scrypt; sesiones de 30 días (token en el navegador, `Authorization: Bearer`).
  - Límite de intentos por IP en el registro y el login. La IP real sale de `CF-Connecting-IP`/`X-Forwarded-For`.
  - Las rutas de dentro del `.crwpack` se validan al extraer (sin `..` ni rutas absolutas). El servidor comprueba que el nombre y la versión del manifiesto coinciden con el formulario y guarda su SHA-256. El editor rechaza la descarga si no coincide.
- **Versiones:** se siguen con semver (`1.2.0`, `1.2.0-beta.1`).
  - **Retirar** una versión la quita como «última», pero se puede seguir instalando (para volver atrás).
  - **Borrar** una versión elimina el archivo.

## Pruebas

```bash
python3 test_api.py   # levanta un servidor temporal y recorre toda la API
```

## Formato .crwpack

Está en `CramionCore/include/CramionCore/crwpack/CrwPack.h`:

1. **Cabecera de 64 bytes.**
2. **Manifiesto JSON sin comprimir.** El servidor y la web lo leen sin descomprimir nada.
3. **Bloques comprimidos.** El contenido de todos los archivos se concatena y se corta en bloques de 4 MiB. Cada bloque se comprime con zstd por separado (o se guarda tal cual si no gana) y lleva su CRC32, así que comprimir y extraer usan todos los núcleos.
4. **Índice.** Lista de archivos y bloques, con su propio CRC.

```text
crwpack pack <carpeta> salida.crwpack --name com.autor.rocas --version 1.0.0 [--level fast|normal|max] [-j hilos]
crwpack unpack paquete.crwpack carpeta
crwpack list | info | verify paquete.crwpack
```

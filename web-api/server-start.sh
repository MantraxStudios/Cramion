#!/usr/bin/env bash
# En el servidor (~/cramion/start.sh, lo copia deploy.sh): deja nginx de
# cramion.mantraxtools.store en limpio apuntando a la Store y levanta la web
# (docs/) y la API en un screen llamado "cramion-store". Todo vive en esta
# carpeta: cramion_api.py, vendor/, web/ (docs), data/ (paquetes) y logs/.
#
#   ./start.sh            # nginx + MySQL + screen
#   ./start.sh --no-nginx # solo (re)arranca el screen
#
# Configuracion en cramion.env (al lado): CRAMION_PORT, DB_HOST, DB_PORT,
# DB_USER, DB_PASSWORD, DB_NAME, CRAMION_SUPERADMIN.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$here"
[[ -f cramion.env ]] || { echo "[ERROR] Falta $here/cramion.env" >&2; exit 1; }
set -a; . ./cramion.env; set +a
port="${CRAMION_PORT:-2323}"
domain="${CRAMION_DOMAIN:-cramion.mantraxtools.store}"
data="${CRAMION_DATA:-$here/data}"
session=cramion-store
# Sin terminal (despliegue automatico): sudo pide la clave a SUDO_ASKPASS.
if [[ -n "${SUDO_ASKPASS:-}" ]]; then sudo() { command sudo -A "$@"; }; fi

command -v python3 >/dev/null || { echo "[ERROR] Falta python3" >&2; exit 1; }
command -v screen >/dev/null || { echo "[ERROR] Falta screen: sudo apt install screen" >&2; exit 1; }

# --- nginx ---------------------------------------------------------------------
setup_nginx() {
    command -v nginx >/dev/null || { echo "[nginx] no esta instalado: se salta"; return; }
    # Solo se toca el archivo de este dominio; el resto de sitios ni se lee.
    local site="/etc/nginx/sites-available/$domain"
    local backup="$here/nginx-backup-$(date +%Y%m%d-%H%M%S)"
    mkdir -p "$backup"
    if sudo test -f "$site"; then
        sudo cp -a "$site" "$backup/$domain"
        echo "[nginx] copia de la config anterior en $backup/$domain"
    fi
    # Otro archivo activo que tambien sirva el dominio daria "conflicting server name".
    local f
    while IFS= read -r f; do
        [[ "$(readlink -f "$f")" == "$site" ]] && continue
        echo "[nginx][ERROR] $f tambien declara $domain: quitalo a mano y repite." >&2
        exit 1
    done < <(sudo grep -rlE "server_name[^;]*[[:space:]]${domain//./\\.}[[:space:];]" /etc/nginx/sites-enabled /etc/nginx/conf.d 2>/dev/null || true)

    local le="/etc/letsencrypt/live/$domain"
    local proxy="
    client_max_body_size 20m;        # subidas en trozos de 8 MB
    proxy_request_buffering off;
    proxy_buffering off;             # descargas grandes de .crwpack
    proxy_read_timeout 300s;
    proxy_send_timeout 300s;

    location / {
        proxy_pass http://127.0.0.1:$port;
        proxy_http_version 1.1;
        proxy_set_header Host \$host;
        proxy_set_header X-Real-IP \$remote_addr;
        proxy_set_header X-Forwarded-For \$proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto \$scheme;
    }"
    local conf
    if sudo test -f "$le/fullchain.pem"; then
        # HTTPS con el certificado de Certbot (renueva solo, no cambia) y
        # el puerto 80 redirige a https.
        conf="# Cramion Store: web (docs/) + API en 127.0.0.1:$port. Lo genera ~/cramion/start.sh
server {
    server_name $domain;
$proxy

    listen 443 ssl;
    ssl_certificate $le/fullchain.pem;
    ssl_certificate_key $le/privkey.pem;
    include /etc/letsencrypt/options-ssl-nginx.conf;
    ssl_dhparam /etc/letsencrypt/ssl-dhparams.pem;
}
server {
    listen 80;
    server_name $domain;
    return 301 https://\$host\$request_uri;
}"
    else
        conf="# Cramion Store: web (docs/) + API en 127.0.0.1:$port. Lo genera ~/cramion/start.sh
server {
    listen 80;
    server_name $domain;
$proxy
}"
    fi
    echo "$conf" | sudo tee "$site" >/dev/null
    sudo ln -sfn "$site" "/etc/nginx/sites-enabled/$domain"
    if ! sudo nginx -t; then
        echo "[nginx][ERROR] la config nueva no vale: vuelvo a la anterior" >&2
        if [[ -f "$backup/$domain" ]]; then sudo cp -a "$backup/$domain" "$site"; else sudo rm -f "$site" "/etc/nginx/sites-enabled/$domain"; fi
        sudo nginx -t && sudo systemctl reload nginx
        exit 1
    fi
    sudo systemctl reload nginx
    echo "[nginx] https://$domain -> 127.0.0.1:$port"
}
[[ "${1:-}" == "--no-nginx" ]] || setup_nginx

# --- Store en un screen ------------------------------------------------------------
screen -S "$session" -X quit >/dev/null 2>&1 || true
# El "python3 -m http.server 9878" que servia la web antes ya no hace falta.
pkill -u "$USER" -f "http.server 9878" >/dev/null 2>&1 && echo "[web] parado el http.server viejo del puerto 9878" || true
sleep 1
if ss -ltn "sport = :$port" | grep -q LISTEN; then
    echo "[ERROR] El puerto $port lo usa otro programa:" >&2
    sudo ss -ltnp "sport = :$port" >&2 || true
    exit 1
fi
mkdir -p "$data" "$here/logs"
screen -dmS "$session" bash -c "cd '$here' && set -a && . ./cramion.env && set +a && \
    exec python3 cramion_api.py serve --db mysql --data '$data' --static '$here/web' \
    --host 127.0.0.1 --port $port 2>&1 | tee -a '$here/logs/store.log'"

for _ in $(seq 1 20); do
    if curl -fsS "http://127.0.0.1:$port/api/health" >/dev/null 2>&1; then
        echo "[OK] Store en marcha: http://127.0.0.1:$port  ->  https://$domain"
        echo "     Ver la consola: screen -r $session   (salir sin pararlo: Ctrl+A, D)"
        exit 0
    fi
    sleep 1
done
echo "[ERROR] La Store no responde. Ultimas lineas del log:" >&2
tail -30 "$here/logs/store.log" >&2
exit 1

#!/usr/bin/env bash
# Sube la Store al servidor y la arranca (nginx + MySQL + screen):
#
#   web-api/deploy.sh                       # rhythm@192.168.1.57
#   web-api/deploy.sh usuario@host
#
# Todo en una carpeta, ~/cramion/: la API (cramion_api.py, vendor/,
# cramion.env, start.sh) y la web (docs/ en web/); data/ y logs/ los crea
# start.sh alli mismo y no se borran al volver a subir. Luego ejecuta start.sh. Pide la contrasena de SSH una
# vez (conexion compartida) y la de sudo para nginx.
set -euo pipefail

target="${1:-rhythm@192.168.1.57}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$here")"
[[ -f "$here/cramion.env" ]] || { echo "[ERROR] Falta web-api/cramion.env (copia cramion.env.example)" >&2; exit 1; }

ctl="$(mktemp -u /tmp/cramion-ssh-XXXXXX)"
ssh_opts=(-o ControlMaster=auto -o ControlPath="$ctl" -o ControlPersist=120)
trap 'ssh "${ssh_opts[@]}" -O exit "$target" 2>/dev/null || true' EXIT

stage="$(mktemp -d)"
trap 'rm -rf "$stage"; ssh "${ssh_opts[@]}" -O exit "$target" 2>/dev/null || true' EXIT
mkdir -p "$stage/cramion/web"
cp "$here/cramion_api.py" "$here/cramion.env" "$stage/cramion/"
cp "$here/server-start.sh" "$stage/cramion/start.sh"
cp -r "$here/vendor" "$stage/cramion/"
# La web sin docs.zip (es para subirla a mano) ni scripts de Windows.
(cd "$root/docs" && tar cf - --exclude=docs.zip --exclude='*.bat' .) | (cd "$stage/cramion/web" && tar xf -)
chmod 600 "$stage/cramion/cramion.env"
chmod +x "$stage/cramion/start.sh"

echo "Subiendo a $target:~/cramion ..."
ssh "${ssh_opts[@]}" "$target" 'mkdir -p ~/cramion && rm -rf ~/cramion/web ~/cramion/vendor'
scp "${ssh_opts[@]}" -rq "$stage/cramion/." "$target:cramion/"
[[ -n "${DEPLOY_NO_START:-}" ]] && exit 0  # solo subir
ssh "${ssh_opts[@]}" -t "$target" 'cd ~/cramion && ./start.sh'

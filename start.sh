#!/usr/bin/env bash
# Levanta la web (docs/) y la API de la Cramion Store (web-api/cramion_api.py)
# en un solo servidor: la web en / y la API en /api (la que usa el Package
# Manager del editor).
#
#   ./start.sh                 # puerto 8000
#   ./start.sh 8080            # otro puerto
#   CRAMION_SUPERADMIN=tu@email.com ./start.sh   # esa cuenta nace superadmin al registrarse
#
# Variables opcionales: CRAMION_PORT, CRAMION_HOST (0.0.0.0), CRAMION_DATA
# (web-api/cramion-data: BD y paquetes subidos), CRAMION_SUPERADMIN.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
port="${1:-${CRAMION_PORT:-8000}}"
host="${CRAMION_HOST:-0.0.0.0}"
data="${CRAMION_DATA:-$root/web-api/cramion-data}"

python="$(command -v python3 || true)"
if [[ -z "$python" ]]; then
    echo "[ERROR] No se encuentra python3 (hace falta 3.10 o superior)." >&2
    exit 1
fi
if ! "$python" -c 'import sys; sys.exit(sys.version_info < (3, 10))'; then
    echo "[ERROR] $("$python" --version) es demasiado viejo: hace falta Python 3.10 o superior." >&2
    exit 1
fi

mkdir -p "$data"

lan_ip="$(hostname -I 2>/dev/null | awk '{print $1}')"
echo "Cramion Store"
echo "  Web:            http://localhost:$port"
echo "  Store:          http://localhost:$port/store/"
echo "  API (editor):   http://localhost:$port/api"
[[ -n "$lan_ip" && "$host" == "0.0.0.0" ]] && echo "  En la red:      http://$lan_ip:$port"
echo "  Datos:          $data"
echo "En el editor: Package Manager > Servidor de paquetes..., pon la API de arriba."
echo "Ctrl+C para parar."
echo

args=(serve --data "$data" --static "$root/docs" --host "$host" --port "$port")
[[ -n "${CRAMION_SUPERADMIN:-}" ]] && args+=(--superadmin "$CRAMION_SUPERADMIN")
exec "$python" "$root/web-api/cramion_api.py" "${args[@]}"

#!/usr/bin/env python3
"""Cramion Store: la API de cuentas y paquetes de assets (.crwpack).

Solo usa la biblioteca estandar de Python (3.10+): http.server, sqlite3 y
hashlib.scrypt. Sirve la API en /api/ y, con --static, tambien la web (docs/),
asi que reemplaza al "python3 -m http.server" que servia la pagina.

Base de datos: SQLite (--db sqlite, por defecto; cramion.db en --data) o
MySQL/MariaDB (--db mysql, con DB_HOST, DB_PORT, DB_USER, DB_PASSWORD y
DB_NAME). Con MySQL crea la base de datos y las tablas al arrancar, y si esta
vacia y hay un cramion.db en --data, copia sus datos. El driver (PyMySQL)
va en vendor/: no hace falta pip.

    python3 cramion_api.py serve --static ~/Escritorio/cramionweb --port 8000
    python3 cramion_api.py superadmin <usuario-o-email>     # dar el rol maximo
    python3 cramion_api.py users                             # listar cuentas

Rutas (JSON salvo las descargas; autenticacion con "Authorization: Bearer <token>"):

  GET    /api/health
  POST   /api/auth/register              {username, email, password}
  POST   /api/auth/login                 {login, password}       -> {token, user}
  POST   /api/auth/logout
  GET    /api/auth/me
  PATCH  /api/auth/me                    {display_name, bio, password, new_password}
  GET    /api/categories
  GET    /api/packages?q=&category=&sort=recent|downloads|name&page=&limit=&owner=
  POST   /api/packages                   {name, display_name, description, category, tags, license, repo_url}
  GET    /api/packages/<name>
  PATCH  /api/packages/<name>
  DELETE /api/packages/<name>
  GET    /api/packages/<name>/image
  PUT    /api/packages/<name>/image      (cuerpo: png/jpg/webp)
  POST   /api/packages/<name>/uploads    {version, size, changelog} -> {upload_id, chunk_size}
  PUT    /api/uploads/<id>?offset=N      (cuerpo: un trozo del .crwpack)
  POST   /api/uploads/<id>/finish        -> la version publicada
  DELETE /api/uploads/<id>
  GET    /api/packages/<name>/versions/<version|latest>/download   (admite Range)
  PATCH  /api/packages/<name>/versions/<version>   {yanked, changelog}
  DELETE /api/packages/<name>/versions/<version>
  GET    /api/me/packages
  GET    /api/admin/stats
  GET    /api/admin/users?q=&page=
  PATCH  /api/admin/users/<id>           {role, banned}
  DELETE /api/admin/users/<id>
  GET    /api/admin/packages?q=&page=
  PATCH  /api/admin/packages/<name>      {hidden, featured}
  GET    /api/admin/log

Las subidas van por trozos (8 MiB por defecto) porque Cloudflare corta los
cuerpos de mas de 100 MB; asi un paquete puede pesar mucho mas.
"""

from __future__ import annotations

import argparse
import base64
import collections
import decimal
import hashlib
import hmac
import http.server
import json
import os
import re
import secrets
import shutil
import socketserver
import sqlite3
import struct
import sys
import threading
import time
import traceback
import urllib.parse
from pathlib import Path

API_VERSION = "1.0"

# --- Formato .crwpack (lo mismo que CramionCore/include/CramionCore/pack/CrwPack.h) ---
CRW_MAGIC = b"CRWPACK\0"
# magic, formato, flags, blockSize, fileCount, blockCount, rawSize, manifestOffset,
# manifestSize, indexSize, indexOffset
CRW_HEADER = struct.Struct("<8sIIIIQQQIIQ")   # 64 bytes
CRW_MAX_MANIFEST = 1 << 20

CATEGORIES = [
    ("3d", "Modelos 3D"), ("2d", "Sprites y 2D"), ("materiales", "Materiales y texturas"),
    ("entornos", "Entornos"), ("personajes", "Personajes"), ("animaciones", "Animaciones"),
    ("audio", "Audio"), ("vfx", "Efectos (VFX)"), ("shaders", "Shaders"), ("scripts", "Scripts C++"),
    ("ui", "Interfaz (UI)"), ("herramientas", "Herramientas del editor"), ("plantillas", "Plantillas de proyecto"),
    ("otros", "Otros"),
]
CATEGORY_IDS = {c for c, _ in CATEGORIES}

NAME_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{1,63}$")
USER_RE = re.compile(r"^[A-Za-z0-9_][A-Za-z0-9_.-]{2,31}$")
EMAIL_RE = re.compile(r"^[^@\s]{1,64}@[^@\s]{1,255}\.[^@\s]{2,}$")
SEMVER_RE = re.compile(r"^(0|[1-9]\d{0,8})\.(0|[1-9]\d{0,8})\.(0|[1-9]\d{0,8})(?:-([0-9A-Za-z.-]{1,32}))?$")

ROLES = ("user", "admin", "superadmin")
SESSION_DAYS = 30
CHUNK_SIZE = 8 << 20
MAX_JSON = 64 << 10
MAX_IMAGE = 2 << 20


# =============================================================================
# Utilidades
# =============================================================================

def now() -> int:
    return int(time.time())


def semver_key(v: str):
    m = SEMVER_RE.match(v)
    if not m:
        return (0, 0, 0, 0, "")
    pre = m.group(4)
    # Una version sin sufijo va despues de sus prereleases (1.0.0-beta < 1.0.0).
    return (int(m.group(1)), int(m.group(2)), int(m.group(3)), 0 if pre else 1, pre or "")


def hash_password(pw: str) -> str:
    salt = secrets.token_bytes(16)
    n, r, p = 1 << 14, 8, 1
    dk = hashlib.scrypt(pw.encode(), salt=salt, n=n, r=r, p=p, dklen=32, maxmem=64 << 20)
    return "scrypt$%d$%d$%d$%s$%s" % (n, r, p, base64.b64encode(salt).decode(), base64.b64encode(dk).decode())


def check_password(pw: str, stored: str) -> bool:
    try:
        algo, n, r, p, salt, dk = stored.split("$")
        if algo != "scrypt":
            return False
        got = hashlib.scrypt(pw.encode(), salt=base64.b64decode(salt), n=int(n), r=int(r), p=int(p),
                             dklen=len(base64.b64decode(dk)), maxmem=64 << 20)
        return hmac.compare_digest(got, base64.b64decode(dk))
    except Exception:
        return False


def token_hash(token: str) -> str:
    return hashlib.sha256(token.encode()).hexdigest()


def read_crwpack_manifest(path: Path) -> dict:
    """Lee la cabecera y el manifiesto (JSON sin comprimir) de un .crwpack."""
    size = path.stat().st_size
    with open(path, "rb") as f:
        raw = f.read(CRW_HEADER.size)
        if len(raw) < CRW_HEADER.size:
            raise ValueError("el archivo es demasiado corto para ser un .crwpack")
        (magic, fmt, _flags, _block_size, file_count, block_count, raw_size,
         manifest_off, manifest_size, index_size, index_off) = CRW_HEADER.unpack(raw)
        if magic != CRW_MAGIC:
            raise ValueError("no es un .crwpack (firma incorrecta)")
        if fmt != 1:
            raise ValueError("version de formato .crwpack no soportada: %d" % fmt)
        if manifest_size == 0 or manifest_size > CRW_MAX_MANIFEST or manifest_off + manifest_size > size:
            raise ValueError("manifiesto del .crwpack invalido")
        if index_off + index_size > size:
            raise ValueError("indice del .crwpack fuera del archivo")
        f.seek(manifest_off)
        manifest = json.loads(f.read(manifest_size).decode("utf-8"))
        if not isinstance(manifest, dict):
            raise ValueError("el manifiesto no es un objeto JSON")
        manifest["_fileCount"] = file_count
        manifest["_rawSize"] = raw_size
        manifest["_blockCount"] = block_count
        return manifest


class ApiError(Exception):
    def __init__(self, status: int, message: str, code: str = "error"):
        super().__init__(message)
        self.status, self.message, self.code = status, message, code


class RateLimiter:
    def __init__(self):
        self.lock = threading.Lock()
        self.hits: dict[tuple, collections.deque] = {}

    def check(self, key: tuple, limit: int, window: int):
        t = time.monotonic()
        with self.lock:
            q = self.hits.setdefault(key, collections.deque())
            while q and t - q[0] > window:
                q.popleft()
            if len(q) >= limit:
                raise ApiError(429, "Demasiados intentos; espera unos minutos.", "rate_limited")
            q.append(t)
            if len(self.hits) > 50000:  # que no crezca sin limite
                self.hits.clear()


# =============================================================================
# Base de datos
# =============================================================================

SCHEMA = """
CREATE TABLE IF NOT EXISTS users (
  id INTEGER PRIMARY KEY,
  username TEXT NOT NULL UNIQUE COLLATE NOCASE,
  email TEXT NOT NULL UNIQUE COLLATE NOCASE,
  pass_hash TEXT NOT NULL,
  display_name TEXT NOT NULL DEFAULT '',
  bio TEXT NOT NULL DEFAULT '',
  role TEXT NOT NULL DEFAULT 'user',
  banned INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL,
  last_login INTEGER
);
CREATE TABLE IF NOT EXISTS sessions (
  token_hash TEXT PRIMARY KEY,
  user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  created_at INTEGER NOT NULL,
  expires_at INTEGER NOT NULL,
  ip TEXT, user_agent TEXT
);
CREATE TABLE IF NOT EXISTS packages (
  id INTEGER PRIMARY KEY,
  name TEXT NOT NULL UNIQUE,
  display_name TEXT NOT NULL,
  description TEXT NOT NULL DEFAULT '',
  category TEXT NOT NULL DEFAULT 'otros',
  tags TEXT NOT NULL DEFAULT '[]',
  license TEXT NOT NULL DEFAULT 'CC0',
  repo_url TEXT NOT NULL DEFAULT '',
  owner_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  hidden INTEGER NOT NULL DEFAULT 0,
  featured INTEGER NOT NULL DEFAULT 0,
  downloads INTEGER NOT NULL DEFAULT 0,
  image_type TEXT,
  created_at INTEGER NOT NULL,
  updated_at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS versions (
  id INTEGER PRIMARY KEY,
  package_id INTEGER NOT NULL REFERENCES packages(id) ON DELETE CASCADE,
  version TEXT NOT NULL,
  size INTEGER NOT NULL,
  sha256 TEXT NOT NULL,
  raw_size INTEGER NOT NULL DEFAULT 0,
  file_count INTEGER NOT NULL DEFAULT 0,
  changelog TEXT NOT NULL DEFAULT '',
  engine TEXT NOT NULL DEFAULT '',
  dependencies TEXT NOT NULL DEFAULT '{}',
  yanked INTEGER NOT NULL DEFAULT 0,
  downloads INTEGER NOT NULL DEFAULT 0,
  created_at INTEGER NOT NULL,
  UNIQUE(package_id, version)
);
CREATE TABLE IF NOT EXISTS uploads (
  id TEXT PRIMARY KEY,
  package_id INTEGER NOT NULL REFERENCES packages(id) ON DELETE CASCADE,
  user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  version TEXT NOT NULL,
  size INTEGER NOT NULL,
  received INTEGER NOT NULL DEFAULT 0,
  changelog TEXT NOT NULL DEFAULT '',
  created_at INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS audit_log (
  id INTEGER PRIMARY KEY,
  actor_id INTEGER,
  action TEXT NOT NULL,
  target TEXT NOT NULL DEFAULT '',
  detail TEXT NOT NULL DEFAULT '',
  created_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_versions_pkg ON versions(package_id);
CREATE INDEX IF NOT EXISTS idx_packages_owner ON packages(owner_id);
CREATE INDEX IF NOT EXISTS idx_sessions_user ON sessions(user_id);
"""

# Las mismas tablas en MySQL/MariaDB (los indices van dentro: MySQL no tiene
# CREATE INDEX IF NOT EXISTS). utf8mb4_unicode_ci no distingue mayusculas,
# como el COLLATE NOCASE de SQLite en usuarios y emails.
MYSQL_SCHEMA = [
    """CREATE TABLE IF NOT EXISTS users (
  id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,
  username VARCHAR(32) NOT NULL UNIQUE,
  email VARCHAR(254) NOT NULL UNIQUE,
  pass_hash VARCHAR(255) NOT NULL,
  display_name VARCHAR(255) NOT NULL DEFAULT '',
  bio VARCHAR(1000) NOT NULL DEFAULT '',
  role VARCHAR(16) NOT NULL DEFAULT 'user',
  banned TINYINT NOT NULL DEFAULT 0,
  created_at BIGINT NOT NULL,
  last_login BIGINT NULL
)""",
    """CREATE TABLE IF NOT EXISTS sessions (
  token_hash CHAR(64) NOT NULL PRIMARY KEY,
  user_id BIGINT NOT NULL,
  created_at BIGINT NOT NULL,
  expires_at BIGINT NOT NULL,
  ip VARCHAR(64) NULL, user_agent TEXT NULL,
  INDEX idx_sessions_user (user_id),
  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
)""",
    """CREATE TABLE IF NOT EXISTS packages (
  id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,
  name VARCHAR(64) NOT NULL UNIQUE,
  display_name VARCHAR(255) NOT NULL,
  description TEXT NOT NULL,
  category VARCHAR(32) NOT NULL DEFAULT 'otros',
  tags TEXT NOT NULL,
  license VARCHAR(128) NOT NULL DEFAULT 'CC0',
  repo_url VARCHAR(512) NOT NULL DEFAULT '',
  owner_id BIGINT NOT NULL,
  hidden TINYINT NOT NULL DEFAULT 0,
  featured TINYINT NOT NULL DEFAULT 0,
  downloads BIGINT NOT NULL DEFAULT 0,
  image_type VARCHAR(8) NULL,
  created_at BIGINT NOT NULL,
  updated_at BIGINT NOT NULL,
  INDEX idx_packages_owner (owner_id),
  FOREIGN KEY (owner_id) REFERENCES users(id) ON DELETE CASCADE
)""",
    """CREATE TABLE IF NOT EXISTS versions (
  id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,
  package_id BIGINT NOT NULL,
  version VARCHAR(64) NOT NULL,
  size BIGINT NOT NULL,
  sha256 CHAR(64) NOT NULL,
  raw_size BIGINT NOT NULL DEFAULT 0,
  file_count BIGINT NOT NULL DEFAULT 0,
  changelog TEXT NOT NULL,
  engine VARCHAR(64) NOT NULL DEFAULT '',
  dependencies TEXT NOT NULL,
  yanked TINYINT NOT NULL DEFAULT 0,
  downloads BIGINT NOT NULL DEFAULT 0,
  created_at BIGINT NOT NULL,
  UNIQUE KEY uq_versions (package_id, version),
  FOREIGN KEY (package_id) REFERENCES packages(id) ON DELETE CASCADE
)""",
    """CREATE TABLE IF NOT EXISTS uploads (
  id VARCHAR(64) NOT NULL PRIMARY KEY,
  package_id BIGINT NOT NULL,
  user_id BIGINT NOT NULL,
  version VARCHAR(64) NOT NULL,
  size BIGINT NOT NULL,
  received BIGINT NOT NULL DEFAULT 0,
  changelog TEXT NOT NULL,
  created_at BIGINT NOT NULL,
  FOREIGN KEY (package_id) REFERENCES packages(id) ON DELETE CASCADE,
  FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
)""",
    """CREATE TABLE IF NOT EXISTS audit_log (
  id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,
  actor_id BIGINT NULL,
  action VARCHAR(64) NOT NULL,
  target VARCHAR(512) NOT NULL DEFAULT '',
  detail VARCHAR(1000) NOT NULL DEFAULT '',
  created_at BIGINT NOT NULL
)""",
]
# Orden de copia desde SQLite (las claves ajenas primero).
TABLES = ("users", "sessions", "packages", "versions", "uploads", "audit_log")


class Row(dict):
    """Fila de MySQL que, como sqlite3.Row, se lee por nombre o por posicion."""

    def __getitem__(self, key):
        if isinstance(key, int):
            return list(self.values())[key]
        return dict.__getitem__(self, key)


class MySqlDb:
    """Conexion a MySQL con la forma de sqlite3 que usa el resto del archivo:
    execute() con parametros "?" y filas Row. En autocommit, como SQLite con
    isolation_level=None."""

    def __init__(self, cfg):
        import pymysql
        import pymysql.cursors

        class Cursor(pymysql.cursors.DictCursor):
            def _conv_row(self, row):
                if row is None:
                    return None
                # SUM() devuelve DECIMAL: a int, para el JSON.
                return Row(zip(self._fields, (int(v) if isinstance(v, decimal.Decimal) else v for v in row)))

        kw = dict(user=cfg.db_user, password=cfg.db_password, charset="utf8mb4", autocommit=True,
                  cursorclass=Cursor, connect_timeout=10)
        # En la misma maquina, por el socket: MySQL 8 (caching_sha2_password)
        # no pide RSA ni TLS por ahi.
        sock = next((s for s in ("/run/mysqld/mysqld.sock", "/var/run/mysqld/mysqld.sock", "/tmp/mysql.sock")
                     if cfg.db_host in ("localhost", "127.0.0.1") and os.path.exists(s)), None)
        if sock:
            kw["unix_socket"] = sock
        else:
            kw.update(host=cfg.db_host, port=cfg.db_port)
        self.conn = pymysql.connect(database=cfg.db_name, **kw)
        self.last_use = time.monotonic()

    def execute(self, sql: str, args=()):
        # Una conexion parada mucho rato la cierra el servidor (wait_timeout).
        if time.monotonic() - self.last_use > 60:
            self.conn.ping(reconnect=True)
        self.last_use = time.monotonic()
        cur = self.conn.cursor()
        cur.execute(sql.replace("%", "%%").replace("?", "%s"), tuple(args))
        return cur


def mysql_bootstrap(cfg):
    """Crea la base de datos (si no existe) y sus tablas."""
    import pymysql
    if not re.fullmatch(r"[A-Za-z0-9_]{1,64}", cfg.db_name):
        sys.exit("DB_NAME no valido: %r (solo letras, numeros y _)" % cfg.db_name)
    name = cfg.db_name
    cfg.db_name = None
    try:
        boot = MySqlDb(cfg)
    finally:
        cfg.db_name = name
    boot.execute("CREATE DATABASE IF NOT EXISTS `%s` CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci" % name)
    boot.conn.close()
    db = MySqlDb(cfg)
    for stmt in MYSQL_SCHEMA:
        db.execute(stmt + " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci")
    return db, pymysql.err.IntegrityError


def import_sqlite(db: MySqlDb, sqlite_path: Path):
    """Copia un cramion.db de SQLite a la base de MySQL recien creada (vacia)."""
    if not sqlite_path.exists() or db.execute("SELECT COUNT(*) FROM users").fetchone()[0]:
        return
    src = sqlite3.connect(sqlite_path)
    src.row_factory = sqlite3.Row
    print("Copiando %s a MySQL..." % sqlite_path)
    for table in TABLES:
        rows = src.execute("SELECT * FROM " + table).fetchall()
        if not rows:
            continue
        cols = rows[0].keys()
        sql = "INSERT INTO %s(%s) VALUES (%s)" % (table, ", ".join(cols), ",".join("?" * len(cols)))
        for r in rows:
            db.execute(sql, tuple(r))
        print("  %s: %d" % (table, len(rows)))
    src.close()


class Store:
    def __init__(self, data_dir: Path, cfg=None):
        self.data_dir = data_dir
        self.pkg_dir = data_dir / "packages"
        self.tmp_dir = data_dir / "uploads"
        self.img_dir = data_dir / "images"
        for d in (self.pkg_dir, self.tmp_dir, self.img_dir):
            d.mkdir(parents=True, exist_ok=True)
        self.db_path = data_dir / "cramion.db"
        self.local = threading.local()
        self.upload_locks: dict[str, threading.Lock] = collections.defaultdict(threading.Lock)
        self.mysql = cfg if cfg is not None and getattr(cfg, "db", "sqlite") == "mysql" else None
        if self.mysql:
            db, self.integrity_error = mysql_bootstrap(self.mysql)
            import_sqlite(db, self.db_path)
            self.pool: list[MySqlDb] = [db]
            self.pool_lock = threading.Lock()
        else:
            self.integrity_error = sqlite3.IntegrityError
            with self.db() as c:
                c.executescript(SCHEMA)

    def db(self):
        c = getattr(self.local, "conn", None)
        if c is None and self.mysql:
            # Cada peticion va en su hilo: la conexion se toma de la reserva y
            # vuelve a ella al terminar (release), sin conectar cada vez.
            with self.pool_lock:
                c = self.pool.pop() if self.pool else None
            self.local.conn = c or MySqlDb(self.mysql)
            return self.local.conn
        if c is None:
            c = sqlite3.connect(self.db_path, timeout=10, isolation_level=None)
            c.row_factory = sqlite3.Row
            c.execute("PRAGMA journal_mode=WAL")
            c.execute("PRAGMA foreign_keys=ON")
            c.execute("PRAGMA busy_timeout=10000")
            self.local.conn = c
        return c

    def release(self):
        c = getattr(self.local, "conn", None)
        if c is None or not self.mysql:
            return
        self.local.conn = None
        if not c.conn.open:  # se corto a media peticion: no se reutiliza
            return
        with self.pool_lock:
            if len(self.pool) < 16:
                self.pool.append(c)
                return
        c.conn.close()

    def log(self, actor_id, action: str, target: str = "", detail: str = ""):
        self.db().execute("INSERT INTO audit_log(actor_id, action, target, detail, created_at) VALUES (?,?,?,?,?)",
                          (actor_id, action, target, detail[:500], now()))

    def package_file(self, name: str, version: str) -> Path:
        return self.pkg_dir / name / (version + ".crwpack")


# =============================================================================
# Serializacion
# =============================================================================

def user_public(u) -> dict:
    return {"id": u["id"], "username": u["username"], "display_name": u["display_name"] or u["username"]}


def user_private(u) -> dict:
    d = user_public(u)
    d.update(email=u["email"], bio=u["bio"], role=u["role"], banned=bool(u["banned"]),
             created_at=u["created_at"], last_login=u["last_login"])
    return d


def version_json(v) -> dict:
    return {
        "version": v["version"], "size": v["size"], "sha256": v["sha256"], "raw_size": v["raw_size"],
        "file_count": v["file_count"], "changelog": v["changelog"], "engine": v["engine"],
        "dependencies": json.loads(v["dependencies"] or "{}"), "yanked": bool(v["yanked"]),
        "downloads": v["downloads"], "created_at": v["created_at"],
    }


# =============================================================================
# Manejador HTTP
# =============================================================================

class Handler(http.server.SimpleHTTPRequestHandler):
    server_version = "CramionStore/" + API_VERSION
    store: Store
    cfg: argparse.Namespace
    limiter = RateLimiter()

    # --- Archivos estaticos -------------------------------------------------
    def list_directory(self, path):  # sin listados de carpetas
        self.send_error(404, "No encontrado")
        return None

    def end_headers(self):
        if not self.path.startswith("/api/"):
            self.send_header("X-Content-Type-Options", "nosniff")
        super().end_headers()

    def log_message(self, fmt, *args):
        if not self.cfg.quiet:
            sys.stderr.write("%s [%s] %s\n" % (self.client_ip(), self.log_date_time_string(), fmt % args))

    # --- Enrutado -----------------------------------------------------------
    def do_GET(self):
        self.dispatch("GET")

    def do_HEAD(self):
        self.dispatch("HEAD")

    def do_POST(self):
        self.dispatch("POST")

    def do_PUT(self):
        self.dispatch("PUT")

    def do_PATCH(self):
        self.dispatch("PATCH")

    def do_DELETE(self):
        self.dispatch("DELETE")

    def do_OPTIONS(self):
        if self.path.startswith("/api/"):
            self.send_response(204)
            self.cors()
            self.send_header("Access-Control-Allow-Methods", "GET, HEAD, POST, PUT, PATCH, DELETE, OPTIONS")
            self.send_header("Access-Control-Allow-Headers", "Authorization, Content-Type, Range")
            self.send_header("Access-Control-Max-Age", "86400")
            self.send_header("Content-Length", "0")
            self.end_headers()
        else:
            self.send_error(405)

    def dispatch(self, method: str):
        parsed = urllib.parse.urlsplit(self.path)
        if not parsed.path.startswith("/api/") and parsed.path != "/api":
            if method in ("GET", "HEAD") and self.cfg.static:
                return super().do_GET() if method == "GET" else super().do_HEAD()
            return self.send_error(404, "No encontrado")
        self.query = {k: v[-1] for k, v in urllib.parse.parse_qs(parsed.query).items()}
        parts = [urllib.parse.unquote(p) for p in parsed.path.split("/")[2:] if p]
        self._user = None
        self._user_loaded = False
        try:
            route = ROUTES.get((method if method != "HEAD" else "GET", self.match_key(parts)))
            if route is None:
                raise ApiError(404, "Ruta de la API desconocida", "not_found")
            fn, argidx = route
            result = fn(self, *[parts[i] for i in argidx])
            if result is not None:
                self.send_json(200 if not isinstance(result, tuple) else result[0],
                               result if not isinstance(result, tuple) else result[1])
        except ApiError as e:
            self.send_json(e.status, {"error": e.message, "code": e.code})
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception:  # noqa: BLE001
            traceback.print_exc()
            try:
                self.send_json(500, {"error": "Error interno del servidor", "code": "internal"})
            except Exception:
                pass
        finally:
            self.store.release()

    # --- Respuestas ---------------------------------------------------------
    def cors(self):
        origin = self.headers.get("Origin")
        allowed = self.cfg.cors
        if origin and (allowed == "*" or origin in allowed.split(",")):
            self.send_header("Access-Control-Allow-Origin", origin if allowed != "*" else "*")
            self.send_header("Vary", "Origin")
            self.send_header("Access-Control-Expose-Headers", "Content-Length, Content-Disposition, ETag, X-Crw-Sha256")

    def send_json(self, status: int, obj):
        body = json.dumps(obj, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        self.send_response(status)
        self.cors()
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def client_ip(self) -> str:
        if getattr(self, "cfg", None) and self.cfg.trust_proxy:
            ip = self.headers.get("CF-Connecting-IP") or (self.headers.get("X-Forwarded-For") or "").split(",")[0].strip()
            if ip:
                return ip
        return self.client_address[0] if self.client_address else "?"

    # --- Cuerpo de la peticion ---------------------------------------------
    def content_length(self, limit: int) -> int:
        if "chunked" in (self.headers.get("Transfer-Encoding") or "").lower():
            raise ApiError(411, "Hace falta Content-Length", "length_required")
        try:
            n = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            raise ApiError(400, "Content-Length invalido")
        if n < 0 or n > limit:
            raise ApiError(413, "El cuerpo de la peticion es demasiado grande", "too_large")
        return n

    def body_json(self) -> dict:
        n = self.content_length(MAX_JSON)
        if n == 0:
            return {}
        try:
            data = json.loads(self.rfile.read(n).decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            raise ApiError(400, "JSON invalido", "bad_json")
        if not isinstance(data, dict):
            raise ApiError(400, "Se esperaba un objeto JSON", "bad_json")
        return data

    def body_to_file(self, f, limit: int) -> int:
        n = self.content_length(limit)
        left = n
        while left > 0:
            buf = self.rfile.read(min(left, 1 << 20))
            if not buf:
                raise ApiError(400, "La conexion se corto antes de recibir todo el cuerpo", "short_body")
            f.write(buf)
            left -= len(buf)
        return n

    # --- Sesion -------------------------------------------------------------
    def user(self, required=True):
        if not self._user_loaded:
            self._user_loaded = True
            auth = self.headers.get("Authorization") or ""
            if auth.startswith("Bearer "):
                row = self.store.db().execute(
                    "SELECT u.* FROM sessions s JOIN users u ON u.id = s.user_id WHERE s.token_hash = ? AND s.expires_at > ?",
                    (token_hash(auth[7:].strip()), now())).fetchone()
                if row is not None and not row["banned"]:
                    self._user = row
        if required and self._user is None:
            raise ApiError(401, "Inicia sesion para continuar", "unauthorized")
        return self._user

    def admin(self, superadmin=False):
        u = self.user()
        if u["role"] not in (("superadmin",) if superadmin else ("admin", "superadmin")):
            raise ApiError(403, "No tienes permisos de administrador", "forbidden")
        return u

    def package(self, name: str, include_hidden=False):
        p = self.store.db().execute("SELECT * FROM packages WHERE name = ?", (name,)).fetchone()
        if p is None:
            raise ApiError(404, "Ese paquete no existe", "not_found")
        if p["hidden"] and not include_hidden:
            u = self.user(required=False)
            if u is None or (u["id"] != p["owner_id"] and u["role"] == "user"):
                raise ApiError(404, "Ese paquete no existe", "not_found")
        return p

    def own_package(self, name: str):
        u = self.user()
        p = self.package(name, include_hidden=True)
        if p["owner_id"] != u["id"] and u["role"] == "user":
            raise ApiError(403, "Ese paquete no es tuyo", "forbidden")
        return u, p


# =============================================================================
# Rutas
# =============================================================================

ROUTES: dict[tuple[str, str], tuple] = {}


def route(method: str, pattern: str):
    def deco(fn):
        argidx = [i for i, p in enumerate(pattern.split("/")) if p == "*"] if pattern else []
        ROUTES[(method, pattern)] = (fn, argidx)
        return fn
    return deco


def text_field(d: dict, key: str, maxlen: int, default: str = "") -> str:
    v = d.get(key, default)
    if v is None:
        v = default
    if not isinstance(v, str):
        raise ApiError(400, "El campo '%s' debe ser texto" % key)
    v = v.strip()
    if len(v) > maxlen:
        raise ApiError(400, "El campo '%s' es demasiado largo (max. %d)" % (key, maxlen))
    return v


def package_summary(h: Handler, p, latest=None) -> dict:
    db = h.store.db()
    if latest is None:
        vs = db.execute("SELECT * FROM versions WHERE package_id = ? AND yanked = 0", (p["id"],)).fetchall()
        latest = max(vs, key=lambda v: semver_key(v["version"]), default=None)
    owner = db.execute("SELECT id, username, display_name FROM users WHERE id = ?", (p["owner_id"],)).fetchone()
    return {
        "name": p["name"], "display_name": p["display_name"], "description": p["description"],
        "category": p["category"], "tags": json.loads(p["tags"] or "[]"), "license": p["license"],
        "repo_url": p["repo_url"], "downloads": p["downloads"], "featured": bool(p["featured"]),
        "hidden": bool(p["hidden"]), "has_image": bool(p["image_type"]),
        "created_at": p["created_at"], "updated_at": p["updated_at"],
        "author": user_public(owner) if owner else None,
        "latest": version_json(latest) if latest else None,
    }


@route("GET", "health")
def r_health(h: Handler):
    return {"ok": True, "service": "cramion-store", "api": API_VERSION, "time": now()}


@route("GET", "categories")
def r_categories(h: Handler):
    return {"categories": [{"id": c, "name": n} for c, n in CATEGORIES]}


# --- Cuentas -----------------------------------------------------------------

def new_session(h: Handler, user_id: int) -> str:
    token = secrets.token_urlsafe(32)
    db = h.store.db()
    db.execute("DELETE FROM sessions WHERE expires_at < ?", (now(),))
    db.execute("INSERT INTO sessions(token_hash, user_id, created_at, expires_at, ip, user_agent) VALUES (?,?,?,?,?,?)",
               (token_hash(token), user_id, now(), now() + SESSION_DAYS * 86400, h.client_ip(),
                (h.headers.get("User-Agent") or "")[:200]))
    db.execute("UPDATE users SET last_login = ? WHERE id = ?", (now(), user_id))
    return token


@route("POST", "auth/register")
def r_register(h: Handler):
    h.limiter.check(("register", h.client_ip()), 5, 3600)
    d = h.body_json()
    username = text_field(d, "username", 32)
    email = text_field(d, "email", 254).lower()
    password = d.get("password") or ""
    if not USER_RE.match(username):
        raise ApiError(400, "El usuario debe tener 3-32 caracteres: letras, numeros, '_', '.' o '-'", "bad_username")
    if not EMAIL_RE.match(email):
        raise ApiError(400, "Ese email no es valido", "bad_email")
    if not isinstance(password, str) or len(password) < 8 or len(password) > 200:
        raise ApiError(400, "La contrasena debe tener al menos 8 caracteres", "bad_password")
    db = h.store.db()
    if db.execute("SELECT 1 FROM users WHERE username = ? OR email = ?", (username, email)).fetchone():
        raise ApiError(409, "Ese usuario o email ya esta registrado", "taken")
    role = "user"
    boot = (h.cfg.superadmin or "").lower()
    if boot and boot in (email, username.lower()):
        role = "superadmin"
    cur = db.execute("INSERT INTO users(username, email, pass_hash, display_name, role, created_at) VALUES (?,?,?,?,?,?)",
                     (username, email, hash_password(password), text_field(d, "display_name", 60) or username, role, now()))
    uid = cur.lastrowid
    h.store.log(uid, "register", username)
    token = new_session(h, uid)
    u = db.execute("SELECT * FROM users WHERE id = ?", (uid,)).fetchone()
    return 201, {"token": token, "user": user_private(u)}


@route("POST", "auth/login")
def r_login(h: Handler):
    h.limiter.check(("login", h.client_ip()), 20, 600)
    d = h.body_json()
    login = text_field(d, "login", 254)
    password = d.get("password") or ""
    u = h.store.db().execute("SELECT * FROM users WHERE username = ? OR email = ?", (login, login.lower())).fetchone()
    if u is None or not isinstance(password, str) or not check_password(password, u["pass_hash"]):
        raise ApiError(401, "Usuario o contrasena incorrectos", "bad_credentials")
    if u["banned"]:
        raise ApiError(403, "Esta cuenta esta suspendida", "banned")
    token = new_session(h, u["id"])
    return {"token": token, "user": user_private(u)}


@route("POST", "auth/logout")
def r_logout(h: Handler):
    auth = h.headers.get("Authorization") or ""
    if auth.startswith("Bearer "):
        h.store.db().execute("DELETE FROM sessions WHERE token_hash = ?", (token_hash(auth[7:].strip()),))
    return {"ok": True}


@route("GET", "auth/me")
def r_me(h: Handler):
    return {"user": user_private(h.user())}


@route("PATCH", "auth/me")
def r_me_update(h: Handler):
    u = h.user()
    d = h.body_json()
    db = h.store.db()
    if "display_name" in d:
        db.execute("UPDATE users SET display_name = ? WHERE id = ?", (text_field(d, "display_name", 60) or u["username"], u["id"]))
    if "bio" in d:
        db.execute("UPDATE users SET bio = ? WHERE id = ?", (text_field(d, "bio", 500), u["id"]))
    if d.get("new_password"):
        if not check_password(str(d.get("password") or ""), u["pass_hash"]):
            raise ApiError(403, "La contrasena actual no es correcta", "bad_credentials")
        npw = d["new_password"]
        if not isinstance(npw, str) or len(npw) < 8 or len(npw) > 200:
            raise ApiError(400, "La contrasena nueva debe tener al menos 8 caracteres", "bad_password")
        db.execute("UPDATE users SET pass_hash = ? WHERE id = ?", (hash_password(npw), u["id"]))
        # Cierra las demas sesiones.
        auth = (h.headers.get("Authorization") or "")[7:].strip()
        db.execute("DELETE FROM sessions WHERE user_id = ? AND token_hash != ?", (u["id"], token_hash(auth)))
    return {"user": user_private(db.execute("SELECT * FROM users WHERE id = ?", (u["id"],)).fetchone())}


# --- Paquetes ----------------------------------------------------------------

def list_packages(h: Handler, where: list[str], args: list, admin_view=False):
    q = (h.query.get("q") or "").strip()[:100]
    if q:
        # "!" como escape: "\\" no se escribe igual en SQLite y en MySQL.
        like = "%" + q.replace("!", "!!").replace("%", "!%").replace("_", "!_") + "%"
        where.append("(p.name LIKE ? ESCAPE '!' OR p.display_name LIKE ? ESCAPE '!' OR p.description LIKE ? ESCAPE '!' OR p.tags LIKE ? ESCAPE '!')")
        args += [like, like, like, like]
    cat = h.query.get("category")
    if cat:
        where.append("p.category = ?")
        args.append(cat)
    owner = h.query.get("owner")
    if owner:
        where.append("p.owner_id = (SELECT id FROM users WHERE username = ?)")
        args.append(owner)
    if not admin_view:
        where.append("EXISTS (SELECT 1 FROM versions v WHERE v.package_id = p.id AND v.yanked = 0)")
    order = {"downloads": "p.downloads DESC", "name": "LOWER(p.display_name) ASC",
             "featured": "p.featured DESC, p.downloads DESC"}.get(h.query.get("sort", ""), "p.updated_at DESC")
    try:
        page = max(1, int(h.query.get("page", 1)))
        limit = min(100, max(1, int(h.query.get("limit", 24))))
    except ValueError:
        raise ApiError(400, "page/limit invalidos")
    sql_where = (" WHERE " + " AND ".join(where)) if where else ""
    db = h.store.db()
    total = db.execute("SELECT COUNT(*) FROM packages p" + sql_where, args).fetchone()[0]
    rows = db.execute("SELECT p.* FROM packages p" + sql_where + " ORDER BY " + order + " LIMIT ? OFFSET ?",
                      args + [limit, (page - 1) * limit]).fetchall()
    return {"total": total, "page": page, "limit": limit, "packages": [package_summary(h, p) for p in rows]}


@route("GET", "packages")
def r_packages(h: Handler):
    return list_packages(h, ["p.hidden = 0"], [])


@route("GET", "me/packages")
def r_my_packages(h: Handler):
    u = h.user()
    rows = h.store.db().execute("SELECT * FROM packages WHERE owner_id = ? ORDER BY updated_at DESC", (u["id"],)).fetchall()
    out = []
    for p in rows:
        s = package_summary(h, p)
        s["versions"] = [version_json(v) for v in sorted(
            h.store.db().execute("SELECT * FROM versions WHERE package_id = ?", (p["id"],)).fetchall(),
            key=lambda v: semver_key(v["version"]), reverse=True)]
        out.append(s)
    return {"packages": out}


def package_fields(d: dict, creating: bool) -> dict:
    f = {}
    if creating or "display_name" in d:
        f["display_name"] = text_field(d, "display_name", 80)
        if not f["display_name"]:
            raise ApiError(400, "Pon un nombre visible al paquete")
    if creating or "description" in d:
        f["description"] = text_field(d, "description", 5000)
    if creating or "category" in d:
        c = text_field(d, "category", 32, "otros") or "otros"
        if c not in CATEGORY_IDS:
            raise ApiError(400, "Categoria desconocida")
        f["category"] = c
    if creating or "license" in d:
        f["license"] = text_field(d, "license", 60, "CC0") or "CC0"
    if creating or "repo_url" in d:
        url = text_field(d, "repo_url", 300)
        if url and not re.match(r"^https?://", url):
            raise ApiError(400, "El enlace debe empezar por http:// o https://")
        f["repo_url"] = url
    if creating or "tags" in d:
        tags = d.get("tags") or []
        if isinstance(tags, str):
            tags = [t for t in re.split(r"[,\s]+", tags) if t]
        if not isinstance(tags, list) or len(tags) > 12:
            raise ApiError(400, "Hasta 12 etiquetas")
        tags = sorted({str(t).strip().lower()[:24] for t in tags if str(t).strip()})
        f["tags"] = json.dumps(tags, ensure_ascii=False)
    return f


@route("POST", "packages")
def r_package_create(h: Handler):
    u = h.user()
    h.limiter.check(("create", u["id"]), 20, 3600)
    d = h.body_json()
    name = text_field(d, "name", 64).lower()
    if not NAME_RE.match(name):
        raise ApiError(400, "El identificador debe tener 2-64 caracteres en minusculas: letras, numeros, '.', '_' o '-' "
                            "(ej. com.tunombre.rocas)", "bad_name")
    db = h.store.db()
    if db.execute("SELECT 1 FROM packages WHERE name = ?", (name,)).fetchone():
        raise ApiError(409, "Ya existe un paquete con ese identificador", "taken")
    f = package_fields(d, True)
    db.execute("INSERT INTO packages(name, display_name, description, category, tags, license, repo_url, owner_id, created_at, updated_at)"
               " VALUES (?,?,?,?,?,?,?,?,?,?)",
               (name, f["display_name"], f["description"], f["category"], f["tags"], f["license"], f["repo_url"],
                u["id"], now(), now()))
    h.store.log(u["id"], "package.create", name)
    return 201, {"package": package_summary(h, h.package(name, True))}


@route("GET", "packages/*")
def r_package_get(h: Handler, name: str):
    p = h.package(name)
    s = package_summary(h, p)
    vs = h.store.db().execute("SELECT * FROM versions WHERE package_id = ?", (p["id"],)).fetchall()
    s["versions"] = [version_json(v) for v in sorted(vs, key=lambda v: semver_key(v["version"]), reverse=True)]
    return {"package": s}


@route("PATCH", "packages/*")
def r_package_update(h: Handler, name: str):
    u, p = h.own_package(name)
    f = package_fields(h.body_json(), False)
    if f:
        sets = ", ".join(k + " = ?" for k in f)
        h.store.db().execute("UPDATE packages SET " + sets + ", updated_at = ? WHERE id = ?", list(f.values()) + [now(), p["id"]])
    return {"package": package_summary(h, h.package(name, True))}


def remove_package(h: Handler, p, actor_id):
    db = h.store.db()
    db.execute("DELETE FROM packages WHERE id = ?", (p["id"],))
    shutil.rmtree(h.store.pkg_dir / p["name"], ignore_errors=True)
    for ext in ("png", "jpg", "webp"):
        (h.store.img_dir / (p["name"] + "." + ext)).unlink(missing_ok=True)
    h.store.log(actor_id, "package.delete", p["name"])


@route("DELETE", "packages/*")
def r_package_delete(h: Handler, name: str):
    u, p = h.own_package(name)
    remove_package(h, p, u["id"])
    return {"ok": True}


IMAGE_TYPES = {b"\x89PNG": ("png", "image/png"), b"\xff\xd8\xff": ("jpg", "image/jpeg"), b"RIFF": ("webp", "image/webp")}


@route("PUT", "packages/*/image")
def r_image_put(h: Handler, name: str):
    u, p = h.own_package(name)
    n = h.content_length(MAX_IMAGE)
    data = h.rfile.read(n)
    kind = next((v for k, v in IMAGE_TYPES.items() if data.startswith(k)), None)
    if kind is None or (kind[0] == "webp" and data[8:12] != b"WEBP"):
        raise ApiError(400, "La imagen debe ser PNG, JPG o WEBP (max. 2 MB)", "bad_image")
    for ext in ("png", "jpg", "webp"):
        (h.store.img_dir / (name + "." + ext)).unlink(missing_ok=True)
    (h.store.img_dir / (name + "." + kind[0])).write_bytes(data)
    h.store.db().execute("UPDATE packages SET image_type = ?, updated_at = ? WHERE id = ?", (kind[0], now(), p["id"]))
    return {"ok": True}


@route("GET", "packages/*/image")
def r_image_get(h: Handler, name: str):
    p = h.package(name)
    if not p["image_type"]:
        raise ApiError(404, "El paquete no tiene imagen", "not_found")
    path = h.store.img_dir / (name + "." + p["image_type"])
    data = path.read_bytes()
    h.send_response(200)
    h.cors()
    h.send_header("Content-Type", {"png": "image/png", "jpg": "image/jpeg", "webp": "image/webp"}[p["image_type"]])
    h.send_header("Content-Length", str(len(data)))
    h.send_header("Cache-Control", "public, max-age=300")
    h.end_headers()
    if h.command != "HEAD":
        h.wfile.write(data)


# --- Subida de versiones por trozos ------------------------------------------

@route("POST", "packages/*/uploads")
def r_upload_start(h: Handler, name: str):
    u, p = h.own_package(name)
    h.limiter.check(("upload", u["id"]), 30, 3600)
    d = h.body_json()
    version = text_field(d, "version", 48)
    if not SEMVER_RE.match(version):
        raise ApiError(400, "La version debe ser semver: MAYOR.MENOR.PARCHE (ej. 1.0.0 o 1.2.0-beta)", "bad_version")
    try:
        size = int(d.get("size") or 0)
    except (TypeError, ValueError):
        size = 0
    max_size = h.cfg.max_package_mb << 20
    if size <= CRW_HEADER.size or size > max_size:
        raise ApiError(413, "El paquete debe pesar menos de %d MB" % h.cfg.max_package_mb, "too_large")
    db = h.store.db()
    if db.execute("SELECT 1 FROM versions WHERE package_id = ? AND version = ?", (p["id"], version)).fetchone():
        raise ApiError(409, "La version %s ya esta publicada; sube una version nueva" % version, "version_exists")
    used = db.execute("SELECT COALESCE(SUM(v.size),0) FROM versions v JOIN packages q ON q.id = v.package_id WHERE q.owner_id = ?",
                      (u["id"],)).fetchone()[0]
    if u["role"] == "user" and used + size > (h.cfg.quota_mb << 20):
        raise ApiError(413, "Superas tu espacio gratuito (%d MB)" % h.cfg.quota_mb, "quota")
    # Limpia subidas abandonadas (mas de un dia).
    for row in db.execute("SELECT id FROM uploads WHERE created_at < ?", (now() - 86400,)).fetchall():
        (h.store.tmp_dir / (row["id"] + ".part")).unlink(missing_ok=True)
        db.execute("DELETE FROM uploads WHERE id = ?", (row["id"],))
    uid = secrets.token_hex(16)
    db.execute("INSERT INTO uploads(id, package_id, user_id, version, size, changelog, created_at) VALUES (?,?,?,?,?,?,?)",
               (uid, p["id"], u["id"], version, size, text_field(d, "changelog", 5000), now()))
    (h.store.tmp_dir / (uid + ".part")).write_bytes(b"")
    return 201, {"upload_id": uid, "chunk_size": CHUNK_SIZE, "size": size}


def get_upload(h: Handler, uid: str):
    u = h.user()
    if not re.fullmatch(r"[0-9a-f]{32}", uid):
        raise ApiError(404, "Subida desconocida", "not_found")
    up = h.store.db().execute("SELECT * FROM uploads WHERE id = ?", (uid,)).fetchone()
    if up is None or up["user_id"] != u["id"]:
        raise ApiError(404, "Subida desconocida o caducada", "not_found")
    return u, up


@route("PUT", "uploads/*")
def r_upload_chunk(h: Handler, uid: str):
    u, up = get_upload(h, uid)
    try:
        offset = int(h.query.get("offset", "-1"))
    except ValueError:
        offset = -1
    with h.store.upload_locks[uid]:
        up = h.store.db().execute("SELECT * FROM uploads WHERE id = ?", (uid,)).fetchone()
        if offset != up["received"]:
            raise ApiError(409, "Trozo fuera de orden", "bad_offset") if offset >= 0 else ApiError(400, "Falta offset")
        limit = min(CHUNK_SIZE * 2, up["size"] - up["received"])
        part = h.store.tmp_dir / (uid + ".part")
        with open(part, "r+b") as f:
            f.seek(offset)
            f.truncate()
            n = h.body_to_file(f, limit)
        h.store.db().execute("UPDATE uploads SET received = ? WHERE id = ?", (offset + n, uid))
    return {"received": offset + n, "size": up["size"]}


@route("DELETE", "uploads/*")
def r_upload_cancel(h: Handler, uid: str):
    u, up = get_upload(h, uid)
    (h.store.tmp_dir / (uid + ".part")).unlink(missing_ok=True)
    h.store.db().execute("DELETE FROM uploads WHERE id = ?", (uid,))
    return {"ok": True}


@route("POST", "uploads/*/finish")
def r_upload_finish(h: Handler, uid: str):
    u, up = get_upload(h, uid)
    db = h.store.db()
    with h.store.upload_locks[uid]:
        part = h.store.tmp_dir / (uid + ".part")
        if up["received"] != up["size"] or not part.exists() or part.stat().st_size != up["size"]:
            raise ApiError(400, "La subida no esta completa", "incomplete")
        p = db.execute("SELECT * FROM packages WHERE id = ?", (up["package_id"],)).fetchone()
        try:
            manifest = read_crwpack_manifest(part)
        except (ValueError, UnicodeDecodeError, json.JSONDecodeError) as e:
            part.unlink(missing_ok=True)
            db.execute("DELETE FROM uploads WHERE id = ?", (uid,))
            raise ApiError(400, "Archivo invalido: %s" % e, "bad_pack")
        mname = str(manifest.get("name") or "")
        mver = str(manifest.get("version") or "")
        problem = None
        if mname and mname != p["name"]:
            problem = "el .crwpack es del paquete '%s', no de '%s'" % (mname, p["name"])
        elif mver and mver != up["version"]:
            problem = "el .crwpack dice version %s y en el formulario pusiste %s" % (mver, up["version"])
        if problem:
            part.unlink(missing_ok=True)
            db.execute("DELETE FROM uploads WHERE id = ?", (uid,))
            raise ApiError(400, "Archivo invalido: " + problem, "manifest_mismatch")
        sha = hashlib.sha256()
        with open(part, "rb") as f:
            for buf in iter(lambda: f.read(1 << 20), b""):
                sha.update(buf)
        deps = manifest.get("dependencies") if isinstance(manifest.get("dependencies"), dict) else {}
        dest = h.store.package_file(p["name"], up["version"])
        dest.parent.mkdir(parents=True, exist_ok=True)
        os.replace(part, dest)
        try:
            db.execute("INSERT INTO versions(package_id, version, size, sha256, raw_size, file_count, changelog, engine, dependencies, created_at)"
                       " VALUES (?,?,?,?,?,?,?,?,?,?)",
                       (p["id"], up["version"], up["size"], sha.hexdigest(), int(manifest.get("_rawSize") or 0),
                        int(manifest.get("_fileCount") or 0), up["changelog"] or str(manifest.get("changelog") or "")[:5000],
                        str(manifest.get("engine") or "")[:40], json.dumps(deps), now()))
        except h.store.integrity_error:
            raise ApiError(409, "Esa version ya esta publicada", "version_exists")
        db.execute("DELETE FROM uploads WHERE id = ?", (uid,))
        db.execute("UPDATE packages SET updated_at = ? WHERE id = ?", (now(), p["id"]))
        h.store.log(u["id"], "version.publish", p["name"] + "@" + up["version"])
    h.store.upload_locks.pop(uid, None)
    v = db.execute("SELECT * FROM versions WHERE package_id = ? AND version = ?", (p["id"], up["version"])).fetchone()
    return 201, {"version": version_json(v)}


# --- Versiones ----------------------------------------------------------------

def find_version(h: Handler, p, version: str, allow_yanked=True):
    db = h.store.db()
    if version == "latest":
        vs = db.execute("SELECT * FROM versions WHERE package_id = ? AND yanked = 0", (p["id"],)).fetchall()
        v = max(vs, key=lambda r: semver_key(r["version"]), default=None)
    else:
        v = db.execute("SELECT * FROM versions WHERE package_id = ? AND version = ?", (p["id"], version)).fetchone()
        if v is not None and v["yanked"] and not allow_yanked:
            v = None
    if v is None:
        raise ApiError(404, "Esa version no existe", "not_found")
    return v


@route("GET", "packages/*/versions/*/download")
def r_download(h: Handler, name: str, version: str):
    p = h.package(name)
    v = find_version(h, p, version)
    path = h.store.package_file(p["name"], v["version"])
    if not path.exists():
        raise ApiError(410, "El archivo de esta version no esta en el servidor", "gone")
    size = path.stat().st_size
    start, end = 0, size - 1
    rng = h.headers.get("Range")
    status = 200
    if rng:
        m = re.fullmatch(r"bytes=(\d*)-(\d*)", rng.strip())
        if not m or (not m.group(1) and not m.group(2)):
            raise ApiError(416, "Range invalido", "bad_range")
        if m.group(1):
            start = int(m.group(1))
            end = min(int(m.group(2)), size - 1) if m.group(2) else size - 1
        else:
            start = max(0, size - int(m.group(2)))
        if start > end or start >= size:
            h.send_response(416)
            h.cors()
            h.send_header("Content-Range", "bytes */%d" % size)
            h.send_header("Content-Length", "0")
            h.end_headers()
            return None
        status = 206
    if start == 0 and h.command != "HEAD":
        db = h.store.db()
        db.execute("UPDATE versions SET downloads = downloads + 1 WHERE id = ?", (v["id"],))
        db.execute("UPDATE packages SET downloads = downloads + 1 WHERE id = ?", (p["id"],))
    h.send_response(status)
    h.cors()
    h.send_header("Content-Type", "application/x-crwpack")
    h.send_header("Content-Disposition", 'attachment; filename="%s-%s.crwpack"' % (p["name"], v["version"]))
    h.send_header("Content-Length", str(end - start + 1))
    h.send_header("Accept-Ranges", "bytes")
    h.send_header("ETag", '"%s"' % v["sha256"])
    h.send_header("X-Crw-Sha256", v["sha256"])
    h.send_header("X-Crw-Version", v["version"])
    if status == 206:
        h.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, size))
    h.end_headers()
    if h.command == "HEAD":
        return None
    with open(path, "rb") as f:
        f.seek(start)
        left = end - start + 1
        while left > 0:
            buf = f.read(min(left, 1 << 20))
            if not buf:
                break
            h.wfile.write(buf)
            left -= len(buf)
    return None


@route("PATCH", "packages/*/versions/*")
def r_version_update(h: Handler, name: str, version: str):
    u, p = h.own_package(name)
    v = find_version(h, p, version)
    d = h.body_json()
    db = h.store.db()
    if "yanked" in d:
        db.execute("UPDATE versions SET yanked = ? WHERE id = ?", (1 if d["yanked"] else 0, v["id"]))
        h.store.log(u["id"], "version.yank" if d["yanked"] else "version.unyank", name + "@" + v["version"])
    if "changelog" in d:
        db.execute("UPDATE versions SET changelog = ? WHERE id = ?", (text_field(d, "changelog", 5000), v["id"]))
    return {"version": version_json(db.execute("SELECT * FROM versions WHERE id = ?", (v["id"],)).fetchone())}


@route("DELETE", "packages/*/versions/*")
def r_version_delete(h: Handler, name: str, version: str):
    u, p = h.own_package(name)
    v = find_version(h, p, version)
    h.store.db().execute("DELETE FROM versions WHERE id = ?", (v["id"],))
    h.store.package_file(name, v["version"]).unlink(missing_ok=True)
    h.store.log(u["id"], "version.delete", name + "@" + v["version"])
    return {"ok": True}


# --- Administracion -------------------------------------------------------------

@route("GET", "admin/stats")
def r_admin_stats(h: Handler):
    h.admin()
    db = h.store.db()
    one = lambda sql, *a: db.execute(sql, a).fetchone()[0]  # noqa: E731
    week = now() - 7 * 86400
    return {
        "users": one("SELECT COUNT(*) FROM users"),
        "users_week": one("SELECT COUNT(*) FROM users WHERE created_at > ?", week),
        "banned": one("SELECT COUNT(*) FROM users WHERE banned = 1"),
        "packages": one("SELECT COUNT(*) FROM packages"),
        "hidden": one("SELECT COUNT(*) FROM packages WHERE hidden = 1"),
        "versions": one("SELECT COUNT(*) FROM versions"),
        "versions_week": one("SELECT COUNT(*) FROM versions WHERE created_at > ?", week),
        "downloads": one("SELECT COALESCE(SUM(downloads),0) FROM packages"),
        "storage": one("SELECT COALESCE(SUM(size),0) FROM versions"),
        "disk_free": shutil.disk_usage(h.store.data_dir).free,
        "sessions": one("SELECT COUNT(*) FROM sessions WHERE expires_at > ?", now()),
        "top": [dict(name=r["name"], display_name=r["display_name"], downloads=r["downloads"]) for r in
                db.execute("SELECT name, display_name, downloads FROM packages ORDER BY downloads DESC LIMIT 5")],
    }


@route("GET", "admin/users")
def r_admin_users(h: Handler):
    h.admin()
    q = (h.query.get("q") or "").strip()[:100]
    try:
        page = max(1, int(h.query.get("page", 1)))
    except ValueError:
        page = 1
    where, args = "", []
    if q:
        where = " WHERE u.username LIKE ? OR u.email LIKE ? OR u.display_name LIKE ?"
        args = ["%" + q + "%"] * 3
    db = h.store.db()
    total = db.execute("SELECT COUNT(*) FROM users u" + where, args).fetchone()[0]
    rows = db.execute("SELECT u.*, (SELECT COUNT(*) FROM packages p WHERE p.owner_id = u.id) AS npkg,"
                      " (SELECT COALESCE(SUM(v.size),0) FROM versions v JOIN packages p ON p.id = v.package_id WHERE p.owner_id = u.id) AS used"
                      " FROM users u" + where + " ORDER BY u.created_at DESC LIMIT 50 OFFSET ?", args + [(page - 1) * 50]).fetchall()
    return {"total": total, "page": page, "users": [dict(user_private(r), packages=r["npkg"], storage=r["used"]) for r in rows]}


@route("PATCH", "admin/users/*")
def r_admin_user_update(h: Handler, uid: str):
    me = h.admin()
    d = h.body_json()
    db = h.store.db()
    t = db.execute("SELECT * FROM users WHERE id = ?", (uid,)).fetchone()
    if t is None:
        raise ApiError(404, "Usuario desconocido", "not_found")
    if t["id"] == me["id"]:
        raise ApiError(400, "No puedes cambiar tu propia cuenta desde aqui")
    if t["role"] != "user" and me["role"] != "superadmin":
        raise ApiError(403, "Solo el superadmin puede tocar a otros administradores", "forbidden")
    if "role" in d:
        if me["role"] != "superadmin":
            raise ApiError(403, "Solo el superadmin cambia roles", "forbidden")
        if d["role"] not in ROLES:
            raise ApiError(400, "Rol desconocido")
        db.execute("UPDATE users SET role = ? WHERE id = ?", (d["role"], t["id"]))
        h.store.log(me["id"], "user.role", t["username"], d["role"])
    if "banned" in d:
        b = 1 if d["banned"] else 0
        db.execute("UPDATE users SET banned = ? WHERE id = ?", (b, t["id"]))
        if b:
            db.execute("DELETE FROM sessions WHERE user_id = ?", (t["id"],))
        h.store.log(me["id"], "user.ban" if b else "user.unban", t["username"])
    r = db.execute("SELECT * FROM users WHERE id = ?", (t["id"],)).fetchone()
    return {"user": user_private(r)}


@route("DELETE", "admin/users/*")
def r_admin_user_delete(h: Handler, uid: str):
    me = h.admin(superadmin=True)
    db = h.store.db()
    t = db.execute("SELECT * FROM users WHERE id = ?", (uid,)).fetchone()
    if t is None:
        raise ApiError(404, "Usuario desconocido", "not_found")
    if t["id"] == me["id"]:
        raise ApiError(400, "No puedes borrar tu propia cuenta")
    for p in db.execute("SELECT * FROM packages WHERE owner_id = ?", (t["id"],)).fetchall():
        remove_package(h, p, me["id"])
    db.execute("DELETE FROM users WHERE id = ?", (t["id"],))
    h.store.log(me["id"], "user.delete", t["username"])
    return {"ok": True}


@route("GET", "admin/packages")
def r_admin_packages(h: Handler):
    h.admin()
    res = list_packages(h, [], [], admin_view=True)
    db = h.store.db()
    for s in res["packages"]:
        s["version_count"] = db.execute("SELECT COUNT(*) FROM versions v JOIN packages p ON p.id = v.package_id WHERE p.name = ?",
                                        (s["name"],)).fetchone()[0]
        s["storage"] = db.execute("SELECT COALESCE(SUM(v.size),0) FROM versions v JOIN packages p ON p.id = v.package_id WHERE p.name = ?",
                                  (s["name"],)).fetchone()[0]
    return res


@route("PATCH", "admin/packages/*")
def r_admin_package_update(h: Handler, name: str):
    me = h.admin()
    p = h.package(name, include_hidden=True)
    d = h.body_json()
    db = h.store.db()
    for key in ("hidden", "featured"):
        if key in d:
            db.execute("UPDATE packages SET " + key + " = ? WHERE id = ?", (1 if d[key] else 0, p["id"]))
            h.store.log(me["id"], "package." + key, name, str(bool(d[key])))
    return {"package": package_summary(h, h.package(name, True))}


@route("GET", "admin/log")
def r_admin_log(h: Handler):
    h.admin()
    rows = h.store.db().execute("SELECT l.*, u.username FROM audit_log l LEFT JOIN users u ON u.id = l.actor_id"
                                " ORDER BY l.id DESC LIMIT 200").fetchall()
    return {"log": [dict(id=r["id"], actor=r["username"], action=r["action"], target=r["target"], detail=r["detail"],
                         created_at=r["created_at"]) for r in rows]}


# Enrutado: cada ruta es un patron como "packages/*/versions/*/download"; gana
# el que encaja con mas partes literales.
def match_key(parts: list[str]) -> str:
    best = None
    for _, pat in ROUTES:
        ps = pat.split("/") if pat else []
        if len(ps) == len(parts) and all(a == "*" or a == b for a, b in zip(ps, parts)):
            score = sum(a != "*" for a in ps)
            if best is None or score > best[0]:
                best = (score, pat)
    return best[1] if best else ""


Handler.match_key = staticmethod(match_key)


# =============================================================================
# Arranque
# =============================================================================

class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True
    request_queue_size = 128


def make_handler(store: Store, cfg):
    static = os.path.abspath(os.path.expanduser(cfg.static)) if cfg.static else None

    class H(Handler):
        def __init__(self, *a, **kw):
            super().__init__(*a, directory=static or os.getcwd(), **kw)

    H.store = store
    H.cfg = cfg
    H.extensions_map = dict(Handler.extensions_map, **{".wasm": "application/wasm", ".crwpack": "application/x-crwpack",
                                                        ".mp4": "video/mp4", ".webp": "image/webp", ".svg": "image/svg+xml",
                                                        ".js": "text/javascript", ".json": "application/json"})
    return H


def main(argv=None):
    ap = argparse.ArgumentParser(description="Cramion Store API")
    sub = ap.add_subparsers(dest="cmd")
    s = sub.add_parser("serve", help="arranca el servidor (por defecto)")
    for p in (ap, s):
        p.add_argument("--data", default=os.environ.get("CRAMION_DATA", "./cramion-data"), help="carpeta de datos (BD y paquetes)")
        p.add_argument("--db", choices=("sqlite", "mysql"), default=os.environ.get("CRAMION_DB", "sqlite"),
                       help="sqlite (cramion.db en --data) o mysql (DB_HOST, DB_PORT, DB_USER, DB_PASSWORD, DB_NAME)")
    s.add_argument("--host", default=os.environ.get("CRAMION_HOST", "0.0.0.0"))
    s.add_argument("--port", type=int, default=int(os.environ.get("CRAMION_PORT", "8000")))
    s.add_argument("--static", default=os.environ.get("CRAMION_STATIC", ""), help="carpeta de la web (docs/) a servir en /")
    s.add_argument("--cors", default=os.environ.get("CRAMION_CORS", "*"), help="origenes permitidos ('*' o lista con comas)")
    s.add_argument("--superadmin", default=os.environ.get("CRAMION_SUPERADMIN", ""),
                   help="email o usuario que sera superadmin al registrarse")
    s.add_argument("--max-package-mb", type=int, default=int(os.environ.get("CRAMION_MAX_PACKAGE_MB", "2048")))
    s.add_argument("--quota-mb", type=int, default=int(os.environ.get("CRAMION_QUOTA_MB", "10240")),
                   help="espacio gratis por usuario")
    s.add_argument("--no-trust-proxy", dest="trust_proxy", action="store_false",
                   help="no leer la IP de CF-Connecting-IP / X-Forwarded-For")
    s.add_argument("--quiet", action="store_true")
    a = sub.add_parser("superadmin", help="da el rol de superadmin a una cuenta")
    a.add_argument("who")
    r = sub.add_parser("role", help="cambia el rol de una cuenta")
    r.add_argument("who")
    r.add_argument("role", choices=ROLES)
    sub.add_parser("users", help="lista las cuentas")
    pw = sub.add_parser("passwd", help="cambia la contrasena de una cuenta")
    pw.add_argument("who")
    pw.add_argument("password")

    args = ap.parse_args(argv)
    if args.cmd is None:
        args = ap.parse_args(["serve"] + (argv if argv is not None else sys.argv[1:]))
    args.db_host = os.environ.get("DB_HOST", "localhost")
    args.db_port = int(os.environ.get("DB_PORT", "3306"))
    args.db_user = os.environ.get("DB_USER", "root")
    args.db_password = os.environ.get("DB_PASSWORD", "")
    args.db_name = os.environ.get("DB_NAME", "cramion_store")
    store = Store(Path(args.data).expanduser().resolve(), args)
    db = store.db()

    if args.cmd in ("superadmin", "role", "passwd"):
        u = db.execute("SELECT * FROM users WHERE username = ? OR email = ?", (args.who, args.who.lower())).fetchone()
        if u is None:
            sys.exit("No existe la cuenta '%s'. Registrala primero en la web." % args.who)
        if args.cmd == "passwd":
            db.execute("UPDATE users SET pass_hash = ? WHERE id = ?", (hash_password(args.password), u["id"]))
            db.execute("DELETE FROM sessions WHERE user_id = ?", (u["id"],))
            print("Contrasena de %s cambiada." % u["username"])
        else:
            role = "superadmin" if args.cmd == "superadmin" else args.role
            db.execute("UPDATE users SET role = ?, banned = 0 WHERE id = ?", (role, u["id"]))
            store.log(None, "cli.role", u["username"], role)
            print("%s ahora es %s." % (u["username"], role))
        return
    if args.cmd == "users":
        for u in db.execute("SELECT * FROM users ORDER BY id"):
            print("%4d  %-24s %-32s %-10s %s" % (u["id"], u["username"], u["email"], u["role"], "SUSPENDIDO" if u["banned"] else ""))
        return

    store.release()
    srv = Server((args.host, args.port), make_handler(store, args))
    print("Cramion Store API %s en http://%s:%d/api/  (datos: %s, BD: %s%s)" % (
        API_VERSION, args.host, args.port, store.data_dir,
        "mysql %s/%s" % (args.db_host, args.db_name) if args.db == "mysql" else "sqlite",
        ", web: " + args.static if args.static else ""))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


# PyMySQL viaja en vendor/ (sin pip).
sys.path.insert(0, str(Path(__file__).resolve().parent / "vendor"))

if __name__ == "__main__":
    main()

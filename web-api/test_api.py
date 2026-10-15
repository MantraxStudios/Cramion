#!/usr/bin/env python3
"""Prueba de humo de cramion_api.py: arranca un servidor temporal y recorre
registro, login, paquete, subida por trozos, descarga (con Range) y admin.

    python3 test_api.py
"""

import json
import struct
import sys
import tempfile
import threading
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import cramion_api as api  # noqa: E402


def fake_crwpack(name: str, version: str, payload: bytes) -> bytes:
    manifest = json.dumps({"format": 1, "name": name, "version": version, "dependencies": {"com.base": "1.0.0"}}).encode()
    header = api.CRW_HEADER.pack(api.CRW_MAGIC, 1, 0, 1 << 20, 1, 1, len(payload),
                                 api.CRW_HEADER.size, len(manifest), 0, api.CRW_HEADER.size + len(manifest) + len(payload))
    return header + manifest + payload


def main():
    tmp = tempfile.mkdtemp(prefix="crw-api-")
    args = api.argparse.Namespace(static="", cors="*", superadmin="root@cramion.test", max_package_mb=64, quota_mb=100,
                                  trust_proxy=False, quiet=True)
    store = api.Store(Path(tmp))
    srv = api.Server(("127.0.0.1", 0), api.make_handler(store, args))
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    base = "http://127.0.0.1:%d/api" % srv.server_address[1]

    def call(method, path, body=None, token=None, raw=None, headers=None, expect=200):
        data = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
        req = urllib.request.Request(base + path, data=data, method=method, headers=headers or {})
        if body is not None:
            req.add_header("Content-Type", "application/json")
        if token:
            req.add_header("Authorization", "Bearer " + token)
        try:
            with urllib.request.urlopen(req) as r:
                status, out = r.status, r.read()
        except urllib.error.HTTPError as e:
            status, out = e.code, e.read()
        assert status == expect, "%s %s -> %d (esperaba %d): %s" % (method, path, status, expect, out[:300])
        ctype = "json"
        try:
            return json.loads(out)
        except ValueError:
            _ = ctype
            return out

    assert call("GET", "/health")["ok"]
    root = call("POST", "/auth/register", {"username": "root", "email": "root@cramion.test", "password": "supersecreta"}, expect=201)
    assert root["user"]["role"] == "superadmin"
    bob = call("POST", "/auth/register", {"username": "bob", "email": "bob@x.test", "password": "contrasena1"}, expect=201)
    call("POST", "/auth/register", {"username": "bob", "email": "otro@x.test", "password": "contrasena1"}, expect=409)
    call("POST", "/auth/login", {"login": "bob", "password": "mal"}, expect=401)
    tb = call("POST", "/auth/login", {"login": "bob@x.test", "password": "contrasena1"})["token"]
    assert call("GET", "/auth/me", token=tb)["user"]["username"] == "bob"

    call("POST", "/packages", {"name": "com.bob.rocas", "display_name": "Rocas", "category": "3d", "tags": "roca, piedra"},
         token=tb, expect=201)
    call("POST", "/packages", {"name": "Mal Nombre", "display_name": "x"}, token=tb, expect=400)
    # Sin versiones no sale en el catalogo publico.
    assert call("GET", "/packages")["total"] == 0

    payload = bytes(range(256)) * 50000  # ~12 MB: dos trozos
    for ver in ("1.0.0", "1.1.0"):
        pack = fake_crwpack("com.bob.rocas", ver, payload)
        up = call("POST", "/packages/com.bob.rocas/uploads", {"version": ver, "size": len(pack), "changelog": "v" + ver},
                  token=tb, expect=201)
        off = 0
        while off < len(pack):
            chunk = pack[off:off + up["chunk_size"]]
            call("PUT", "/uploads/%s?offset=%d" % (up["upload_id"], off), raw=chunk, token=tb,
                 headers={"Content-Type": "application/octet-stream"})
            off += len(chunk)
        v = call("POST", "/uploads/%s/finish" % up["upload_id"], token=tb, expect=201)["version"]
        assert v["version"] == ver and v["dependencies"] == {"com.base": "1.0.0"}

    # Nombre equivocado dentro del .crwpack.
    bad = fake_crwpack("com.otro", "2.0.0", b"x")
    up = call("POST", "/packages/com.bob.rocas/uploads", {"version": "2.0.0", "size": len(bad)}, token=tb, expect=201)
    call("PUT", "/uploads/%s?offset=0" % up["upload_id"], raw=bad, token=tb)
    call("POST", "/uploads/%s/finish" % up["upload_id"], token=tb, expect=400)

    lst = call("GET", "/packages?q=piedra")
    assert lst["total"] == 1 and lst["packages"][0]["latest"]["version"] == "1.1.0", lst
    det = call("GET", "/packages/com.bob.rocas")["package"]
    assert [v["version"] for v in det["versions"]] == ["1.1.0", "1.0.0"]

    data = call("GET", "/packages/com.bob.rocas/versions/1.0.0/download")
    assert data == fake_crwpack("com.bob.rocas", "1.0.0", payload)
    part = call("GET", "/packages/com.bob.rocas/versions/latest/download", headers={"Range": "bytes=8-11"}, expect=206)
    assert part == struct.pack("<I", 1)

    # Retirar la ultima: "latest" pasa a ser la anterior; la retirada se sigue pudiendo bajar.
    call("PATCH", "/packages/com.bob.rocas/versions/1.1.0", {"yanked": True}, token=tb)
    assert call("GET", "/packages/com.bob.rocas")["package"]["latest"]["version"] == "1.0.0"
    call("GET", "/packages/com.bob.rocas/versions/1.1.0/download")

    # Admin.
    call("GET", "/admin/stats", token=tb, expect=403)
    tr = root["token"]
    st = call("GET", "/admin/stats", token=tr)
    assert st["users"] == 2 and st["versions"] == 2 and st["downloads"] >= 2, st
    bob_id = bob["user"]["id"]
    call("PATCH", "/admin/users/%d" % bob_id, {"banned": True}, token=tr)
    call("GET", "/auth/me", token=tb, expect=401)
    call("PATCH", "/admin/users/%d" % bob_id, {"banned": False, "role": "admin"}, token=tr)
    call("PATCH", "/admin/packages/com.bob.rocas", {"featured": True}, token=tr)
    assert call("GET", "/admin/packages", token=tr)["packages"][0]["featured"]
    assert len(call("GET", "/admin/log", token=tr)["log"]) > 3
    call("DELETE", "/packages/com.bob.rocas", token=tr)
    call("GET", "/packages/com.bob.rocas", expect=404)
    srv.shutdown()
    print("OK: todas las pruebas de la API pasaron (%s)" % tmp)


if __name__ == "__main__":
    main()

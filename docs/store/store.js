// Cramion Store: lo comun de todas las paginas (API, sesion, cabecera, avisos).
// La API vive en /api del mismo servidor (web-api/cramion_api.py). Abierta
// como archivo local o desde otro dominio, va al servidor publico.
(function () {
  "use strict";

  const PUBLIC_API = "https://cramion.mantraxtools.store/api";
  const API = window.CRAMION_API ||
    (location.protocol === "file:" ? PUBLIC_API : location.origin + "/api");
  const TOKEN_KEY = "cramion-token";
  const USER_KEY = "cramion-user";

  // --- Almacenamiento (puede fallar en modo privado) ------------------------
  const store = {
    get(k) { try { return localStorage.getItem(k); } catch (e) { return null; } },
    set(k, v) { try { v == null ? localStorage.removeItem(k) : localStorage.setItem(k, v); } catch (e) {} },
  };

  // --- Utilidades -----------------------------------------------------------
  const esc = s => String(s ?? "").replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
  const $ = (sel, root = document) => root.querySelector(sel);
  const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));
  const fmtBytes = n => {
    n = Number(n) || 0;
    if (n >= 1 << 30) return (n / (1 << 30)).toFixed(2).replace(".", ",") + " GB";
    if (n >= 1 << 20) return (n / (1 << 20)).toFixed(1).replace(".", ",") + " MB";
    if (n >= 1024) return Math.round(n / 1024) + " KB";
    return n + " B";
  };
  const fmtNum = n => (Number(n) || 0).toLocaleString("es");
  const fmtDate = t => t ? new Date(t * 1000).toLocaleDateString("es", { day: "numeric", month: "short", year: "numeric" }) : "";
  const timeAgo = t => {
    if (!t) return "";
    const s = Date.now() / 1000 - t;
    if (s < 60) return "ahora";
    if (s < 3600) return "hace " + Math.floor(s / 60) + " min";
    if (s < 86400) return "hace " + Math.floor(s / 3600) + " h";
    if (s < 86400 * 30) return "hace " + Math.floor(s / 86400) + " d";
    return fmtDate(t);
  };
  const initials = u => ((u && (u.display_name || u.username)) || "?").trim().slice(0, 2).toUpperCase();
  const param = k => new URLSearchParams(location.search).get(k);
  const imageUrl = name => API + "/packages/" + encodeURIComponent(name) + "/image";
  const downloadUrl = (name, version) => API + "/packages/" + encodeURIComponent(name) + "/versions/" + encodeURIComponent(version || "latest") + "/download";

  // --- API ------------------------------------------------------------------
  class ApiError extends Error {
    constructor(message, status, code) { super(message); this.status = status; this.code = code; }
  }

  async function api(method, path, body, opts = {}) {
    const headers = {};
    const token = store.get(TOKEN_KEY);
    if (token) headers.Authorization = "Bearer " + token;
    let payload;
    if (opts.raw !== undefined) {
      payload = opts.raw;
      headers["Content-Type"] = opts.type || "application/octet-stream";
    } else if (body !== undefined) {
      payload = JSON.stringify(body);
      headers["Content-Type"] = "application/json";
    }
    let res;
    try {
      res = await fetch(API + path, { method, headers, body: payload, signal: opts.signal });
    } catch (e) {
      if (e.name === "AbortError") throw e;
      throw new ApiError("No se pudo conectar con el servidor. Revisa tu conexión.", 0, "network");
    }
    let data = null;
    const text = await res.text();
    try { data = text ? JSON.parse(text) : null; } catch (e) { data = null; }
    if (!res.ok) {
      if (res.status === 401 && token && !opts.keepSession) { setSession(null, null); }
      throw new ApiError((data && data.error) || ("Error " + res.status), res.status, data && data.code);
    }
    return data;
  }

  // --- Sesion ---------------------------------------------------------------
  let user = null;
  try { user = JSON.parse(store.get(USER_KEY) || "null"); } catch (e) { user = null; }

  function setSession(token, u) {
    store.set(TOKEN_KEY, token);
    store.set(USER_KEY, u ? JSON.stringify(u) : null);
    user = u;
    renderUser();
  }

  async function refreshUser() {
    if (!store.get(TOKEN_KEY)) { user = null; renderUser(); return null; }
    try {
      const r = await api("GET", "/auth/me");
      user = r.user;
      store.set(USER_KEY, JSON.stringify(user));
    } catch (e) {
      if (e.status === 401) setSession(null, null);
    }
    renderUser();
    return user;
  }

  async function logout() {
    try { await api("POST", "/auth/logout", {}); } catch (e) {}
    setSession(null, null);
    location.href = "index.html";
  }

  function requireLogin() {
    if (!store.get(TOKEN_KEY)) {
      location.replace("cuenta.html?next=" + encodeURIComponent(location.pathname.split("/").pop() + location.search));
      return false;
    }
    return true;
  }

  const isAdmin = u => !!u && (u.role === "admin" || u.role === "superadmin");

  // --- Iconos ---------------------------------------------------------------
  const ICONS = {
    moon: '<path d="M20 14.5A8 8 0 0 1 9.5 4a8 8 0 1 0 10.5 10.5Z"/>',
    sun: '<circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4"/>',
    menu: '<path d="M4 6h16M4 12h16M4 18h16"/>',
    x: '<path d="M18 6 6 18M6 6l12 12"/>',
    search: '<circle cx="11" cy="11" r="7"/><path d="m20 20-3.5-3.5"/>',
    box: '<path d="M21 8 12 3 3 8v8l9 5 9-5V8Z"/><path d="M3 8l9 5 9-5M12 13v8"/>',
    download: '<path d="M12 4v11M7 10l5 5 5-5M5 20h14"/>',
    upload: '<path d="M12 15V3M7 8l5-5 5 5"/><path d="M5 13v6a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2v-6"/>',
    plus: '<path d="M12 5v14M5 12h14"/>',
    user: '<circle cx="12" cy="8" r="4"/><path d="M4 21a8 8 0 0 1 16 0"/>',
    grid: '<rect x="3" y="3" width="7" height="7" rx="1"/><rect x="14" y="3" width="7" height="7" rx="1"/><rect x="3" y="14" width="7" height="7" rx="1"/><rect x="14" y="14" width="7" height="7" rx="1"/>',
    shield: '<path d="M12 3 4 6v6c0 5 3.5 8 8 9 4.5-1 8-4 8-9V6l-8-3Z"/>',
    logout: '<path d="M15 4h3a2 2 0 0 1 2 2v12a2 2 0 0 1-2 2h-3M10 17l5-5-5-5M15 12H3"/>',
    edit: '<path d="M4 20h4L19 9l-4-4L4 16v4Z"/><path d="m14 6 4 4"/>',
    trash: '<path d="M4 7h16M10 11v6M14 11v6M6 7l1 13h10l1-13M9 7V4h6v3"/>',
    star: '<path d="m12 3 2.8 5.7 6.2.9-4.5 4.4 1 6.2L12 17.3 6.5 20.2l1-6.2L3 9.6l6.2-.9L12 3Z"/>',
    eye: '<path d="M2 12s3.5-7 10-7 10 7 10 7-3.5 7-10 7S2 12 2 12Z"/><circle cx="12" cy="12" r="3"/>',
    eyeoff: '<path d="M3 3l18 18M10.6 5.1A10 10 0 0 1 12 5c6.5 0 10 7 10 7a17 17 0 0 1-3.2 4.2M6.6 6.6A17 17 0 0 0 2 12s3.5 7 10 7a9.7 9.7 0 0 0 5.4-1.6"/><path d="M9.9 9.9a3 3 0 0 0 4.2 4.2"/>',
    ban: '<circle cx="12" cy="12" r="9"/><path d="m5.6 5.6 12.8 12.8"/>',
    log: '<path d="M8 6h13M8 12h13M8 18h13M3 6h.01M3 12h.01M3 18h.01"/>',
    image: '<rect x="3" y="4" width="18" height="16" rx="2"/><circle cx="9" cy="10" r="2"/><path d="m21 16-5-5L5 20"/>',
    arrow: '<path d="M5 12h14M13 6l6 6-6 6"/>',
    back: '<path d="M19 12H5M11 18l-6-6 6-6"/>',
    copy: '<rect x="9" y="9" width="12" height="12" rx="2"/><path d="M5 15V5a2 2 0 0 1 2-2h10"/>',
    book: '<path d="M4 5a2 2 0 0 1 2-2h13v16H6a2 2 0 0 0-2 2V5Z"/><path d="M4 19a2 2 0 0 1 2-2h13"/>',
    globe: '<circle cx="12" cy="12" r="9"/><path d="M3 12h18M12 3a14 14 0 0 1 0 18M12 3a14 14 0 0 0 0 18"/>',
  };
  const icon = (name, cls = "ic") => '<svg class="' + cls + '" viewBox="0 0 24 24" aria-hidden="true">' + (ICONS[name] || "") + "</svg>";

  // --- Cabecera y pie ----------------------------------------------------------
  function header(active) {
    const el = document.getElementById("site-header");
    if (!el) return;
    const links = [
      ["../index.html", "Inicio", ""],
      ["../manual/index.html", "Documentación", ""],
      ["index.html", "Assets", "store"],
      ["panel.html", "Publicar", "panel"],
    ];
    const nav = links.map(([href, text, key]) =>
      '<a href="' + href + '"' + (key && key === active ? ' aria-current="page"' : "") + ">" + text + "</a>").join("");
    el.className = "site-header";
    el.innerHTML =
      '<div class="wrap nav">' +
      '<a href="index.html" class="brand" aria-label="Cramion Store, inicio"><img src="../img/icon.png" alt="" width="28" height="28">Cramion <small>Store</small></a>' +
      '<nav class="nav-links" aria-label="Secciones">' + nav + "</nav>" +
      '<div class="nav-right">' +
      '<button class="icon-btn theme-btn" id="theme-btn" aria-label="Cambiar entre tema claro y oscuro" title="Tema">' + icon("moon", "ic i-moon") + icon("sun", "ic i-sun") + "</button>" +
      '<div id="user-slot"></div>' +
      '<button class="icon-btn menu-btn" id="menu-btn" aria-label="Menú" aria-expanded="false" aria-controls="mobile-menu">' + icon("menu") + "</button>" +
      "</div></div>" +
      '<div id="mobile-menu" class="mobile-menu" hidden><div class="wrap">' + nav + '<span id="mobile-user"></span></div></div>';

    const root = document.documentElement;
    $("#theme-btn").addEventListener("click", () => {
      root.dataset.theme = root.dataset.theme === "dark" ? "light" : "dark";
      store.set("cramion-theme", root.dataset.theme);
    });
    const btn = $("#menu-btn"), panel = $("#mobile-menu");
    const setMenu = open => { panel.hidden = !open; btn.setAttribute("aria-expanded", String(open)); btn.innerHTML = icon(open ? "x" : "menu"); };
    btn.addEventListener("click", () => setMenu(panel.hidden));
    addEventListener("keydown", e => { if (e.key === "Escape") { setMenu(false); closeDropdown(); } });
    renderUser();

    const foot = document.getElementById("site-footer");
    if (foot) {
      foot.className = "site-footer";
      foot.innerHTML = '<div class="wrap"><span>Cramion Store · assets gratis para Cramion Engine</span>' +
        '<span><a href="../index.html">Inicio</a> · <a href="../manual/index.html">Documentación</a> · <a href="../manual/licencia.html">Licencia</a></span></div>';
    }
  }

  function closeDropdown() {
    const d = $("#user-dropdown");
    if (d) { d.hidden = true; const b = $("#user-btn"); if (b) b.setAttribute("aria-expanded", "false"); }
  }

  function renderUser() {
    const slot = $("#user-slot"), mobile = $("#mobile-user");
    if (!slot) return;
    if (!user) {
      slot.innerHTML = '<a class="btn btn-primary btn-sm" href="cuenta.html">' + icon("user", "ic ic-sm") + "Entrar</a>";
      if (mobile) mobile.innerHTML = '<a href="cuenta.html">Entrar o crear cuenta</a>';
      return;
    }
    const admin = isAdmin(user);
    slot.innerHTML =
      '<div class="user-menu"><button class="user-btn" id="user-btn" aria-haspopup="menu" aria-expanded="false">' +
      '<span class="avatar">' + esc(initials(user)) + "</span><span>" + esc(user.display_name || user.username) + "</span></button>" +
      '<div class="dropdown" id="user-dropdown" role="menu" hidden>' +
      '<div class="who"><b>' + esc(user.display_name || user.username) + '</b><span class="muted">@' + esc(user.username) +
      (user.role !== "user" ? ' · <span class="badge blue">' + esc(user.role) + "</span>" : "") + "</span></div>" +
      '<a href="panel.html" role="menuitem">' + icon("grid", "ic ic-sm") + "Mis paquetes</a>" +
      '<a href="panel.html#perfil" role="menuitem">' + icon("user", "ic ic-sm") + "Mi perfil</a>" +
      (admin ? '<a href="admin.html" role="menuitem">' + icon("shield", "ic ic-sm") + "Administración</a>" : "") +
      '<button type="button" id="logout-btn" role="menuitem">' + icon("logout", "ic ic-sm") + "Cerrar sesión</button>" +
      "</div></div>";
    const b = $("#user-btn"), d = $("#user-dropdown");
    b.addEventListener("click", e => { e.stopPropagation(); d.hidden = !d.hidden; b.setAttribute("aria-expanded", String(!d.hidden)); });
    document.addEventListener("click", e => { if (!d.contains(e.target)) closeDropdown(); });
    $("#logout-btn").addEventListener("click", logout);
    if (mobile) {
      mobile.innerHTML = (admin ? '<a href="admin.html">Administración</a>' : "") +
        '<a href="panel.html#perfil">Mi perfil</a><a href="#" id="logout-m">Cerrar sesión</a>';
      $("#logout-m").addEventListener("click", e => { e.preventDefault(); logout(); });
    }
  }

  // --- Avisos ---------------------------------------------------------------
  function toast(message, kind = "") {
    let box = $(".toast-box");
    if (!box) { box = document.createElement("div"); box.className = "toast-box"; box.setAttribute("aria-live", "polite"); document.body.appendChild(box); }
    const t = document.createElement("div");
    t.className = "toast " + kind;
    t.textContent = message;
    box.appendChild(t);
    setTimeout(() => t.remove(), kind === "err" ? 7000 : 4000);
  }

  function confirmDialog(title, text, okText = "Aceptar", danger = true) {
    return new Promise(resolve => {
      const d = document.createElement("dialog");
      d.className = "modal";
      d.innerHTML = '<div class="modal-head"><h2>' + esc(title) + '</h2></div><div class="modal-body"><p style="margin:0">' + esc(text) +
        '</p></div><div class="modal-foot"><button class="btn btn-outline" value="no">Cancelar</button><button class="btn ' +
        (danger ? "btn-danger" : "btn-primary") + '" value="si">' + esc(okText) + "</button></div>";
      document.body.appendChild(d);
      d.querySelectorAll("button").forEach(b => b.addEventListener("click", () => { d.close(b.value); }));
      d.addEventListener("close", () => { resolve(d.returnValue === "si"); d.remove(); });
      d.showModal();
    });
  }

  // --- Subida por trozos -------------------------------------------------------
  async function uploadVersion(name, file, version, changelog, onProgress, signal) {
    const start = await api("POST", "/packages/" + encodeURIComponent(name) + "/uploads",
      { version, size: file.size, changelog }, { signal });
    const chunk = start.chunk_size || (8 << 20);
    let offset = 0;
    try {
      while (offset < file.size) {
        const part = file.slice(offset, Math.min(file.size, offset + chunk));
        let tries = 0;
        for (;;) {
          try {
            await api("PUT", "/uploads/" + start.upload_id + "?offset=" + offset, undefined, { raw: part, signal });
            break;
          } catch (e) {
            if (e.name === "AbortError" || ++tries >= 3 || (e.status && e.status < 500)) throw e;
            await new Promise(r => setTimeout(r, 1000 * tries));
          }
        }
        offset += part.size;
        if (onProgress) onProgress(offset, file.size);
      }
      return (await api("POST", "/uploads/" + start.upload_id + "/finish", {}, { signal })).version;
    } catch (e) {
      api("DELETE", "/uploads/" + start.upload_id).catch(() => {});
      throw e;
    }
  }

  // Lee el manifiesto de un .crwpack en el navegador (cabecera de 64 bytes + JSON).
  async function readManifest(file) {
    const head = new DataView(await file.slice(0, 64).arrayBuffer());
    const magic = new TextDecoder().decode(new Uint8Array(head.buffer, 0, 7));
    if (magic !== "CRWPACK") throw new Error("No es un archivo .crwpack");
    const fmt = head.getUint32(8, true);
    if (fmt !== 1) throw new Error("Versión de .crwpack no soportada (" + fmt + ")");
    const off = Number(head.getBigUint64(40, true)), size = head.getUint32(48, true);
    if (size <= 0 || size > (1 << 20) || off + size > file.size) throw new Error("El .crwpack está dañado");
    const json = JSON.parse(await file.slice(off, off + size).text());
    json._fileCount = head.getUint32(20, true);
    json._rawSize = Number(head.getBigUint64(32, true));
    return json;
  }

  let categoriesCache = null;
  async function categories() {
    if (!categoriesCache) {
      try { categoriesCache = (await api("GET", "/categories")).categories; } catch (e) { categoriesCache = []; }
    }
    return categoriesCache;
  }
  const categoryName = (list, id) => (list.find(c => c.id === id) || { name: id || "" }).name;

  window.Store = {
    API, api, ApiError, esc, $, $$, fmtBytes, fmtNum, fmtDate, timeAgo, initials, param, icon, imageUrl, downloadUrl,
    header, toast, confirmDialog, setSession, refreshUser, requireLogin, isAdmin, logout, uploadVersion, readManifest,
    categories, categoryName,
    get user() { return user; },
  };
})();

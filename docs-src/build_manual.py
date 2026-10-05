# Genera el manual (docs/manual/*.html) a partir de:
#   docs-src/pages.json     grupos, orden, titulo y descripcion de cada pagina
#   docs-src/pages/*.html   el contenido de cada pagina (HTML con h3, p, table, pre...)
# Uso: python docs-src/build_manual.py
# Cada pagina sale con el menu lateral, el buscador (docs/manual/search.js),
# el estilo de docs-src/manual.css (shadcn/ui, sin compilar), modo claro/oscuro,
# "En esta pagina", anterior/siguiente, donaciones y el boton de copiar en el codigo.
import html
import json
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DOCS = os.path.join(ROOT, "docs")
OUT = os.path.join(DOCS, "manual")
DISCORD = "https://discord.gg/zG7rSsUGEz"

# Antes de nada: la referencia de C++ desde las cabeceras del SDK y los ejemplos.
import sys
sys.path.insert(0, HERE)
import gen_cpp_api  # noqa: E402
import examples_cpp  # noqa: E402
gen_cpp_api.main()
examples_cpp.main()

data = json.load(open(os.path.join(HERE, "pages.json"), encoding="utf-8"))
intro_html = data["intro"]
examples_intro = data["examples_intro"]
LUA_GROUP = "Referencia de Lua (obsoleta)"
GROUP_ICONS = {"API de C++": "code", LUA_GROUP: "code", "Primeros pasos": "book", "El editor": "window", "Componentes": "cube", "Referencia de la API": "code", "Gráficos": "paint", "Ejemplos": "spark"}
GROUPS = []
for g in data["groups"]:
    GROUPS.append((g["group"], GROUP_ICONS.get(g["group"], "book"), g["pages"]))


ICONS = {
    "paint": '<path d="M12 3a9 9 0 1 0 0 18c1.1 0 1.5-.8 1.5-1.5 0-.9-.7-1.2-.7-2 0-.8.6-1.5 1.5-1.5H16a5 5 0 0 0 5-5c0-4.4-4-8-9-8Z"/><circle cx="7.5" cy="11" r="1"/><circle cx="10" cy="7" r="1"/><circle cx="15" cy="7.5" r="1"/>',
    "book": '<path d="M4 5a2 2 0 0 1 2-2h13v16H6a2 2 0 0 0-2 2V5Z"/><path d="M4 19a2 2 0 0 1 2-2h13"/>',
    "code": '<path d="m8 8-4 4 4 4"/><path d="m16 8 4 4-4 4"/><path d="m14 4-4 16"/>',
    "window": '<rect x="3" y="4" width="18" height="16" rx="2"/><path d="M3 9h18M9 9v11"/>',
    "cube": '<path d="m12 3 8 4.5v9L12 21l-8-4.5v-9L12 3Z"/><path d="m4 7.5 8 4.5 8-4.5M12 12v9"/>',
    "spark": '<path d="M12 3v4M12 17v4M3 12h4M17 12h4M6 6l2.5 2.5M15.5 15.5 18 18M6 18l2.5-2.5M15.5 8.5 18 6"/>',
}


def icon(name, cls="h-4 w-4"):
    return (f'<svg class="{cls}" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" '
            f'stroke-linecap="round" stroke-linejoin="round">{ICONS[name]}</svg>')


def strip(text):
    return html.unescape(re.sub(r"<.*?>", "", text)).strip()


def slug(text):
    t = strip(text).lower()
    for a, b in zip("áéíóúñü", "aeiounu"):
        t = t.replace(a, b)
    return re.sub(r"[^a-z0-9]+", "-", t).strip("-") or "seccion"


pages = []  # dicts: slug, title, desc, group, icon, content
for group, gicon, items in GROUPS:
    for item in items:
        content = open(os.path.join(HERE, "pages", item["slug"] + ".html"), encoding="utf-8").read()
        pages.append(dict(slug=item["slug"], title=item["title"], desc=item["desc"], group=group, icon=gicon,
                          content=content.strip()))
examples = [p for p in pages if p["group"] == "Ejemplos"]

# Donde esta cada ancla (enlaces "#x" entre paginas).
anchor_page = {}
for p in pages:
    for s in re.findall(r'id="([^"]+)"', p["content"]):
        anchor_page.setdefault(s, p["slug"])
old_ids = {p["slug"]: p["slug"] for p in pages}

search = []
for p in pages:
    content = p["content"]
    if p["group"] == LUA_GROUP:
        content = ('<div class="note"><strong class="text-white">Lua está obsoleto.</strong> Sigue funcionando en los '
                   'proyectos que lo usan, pero lo nuevo se escribe en C++: ver la <a href="cpp-script.html">API de C++</a> '
                   'y <a href="primer-script.html">Tu primer script</a>.</div>\n' + content)

    # h3 con id (para el indice de la derecha y el buscador).
    def add_id(m):
        return f'<h3 id="{slug(m.group(1))}">{m.group(1)}</h3>'
    content = re.sub(r"<h3>(.*?)</h3>", add_id, content)

    # Enlaces internos "#x" -> su pagina nueva.
    def fix_link(m):
        target = m.group(1)
        if target in old_ids:
            return f'href="{old_ids[target]}.html"'
        if target in anchor_page and anchor_page[target] != p["slug"]:
            return f'href="{anchor_page[target]}.html#{target}"'
        return m.group(0)
    content = re.sub(r'href="#([^"]+)"', fix_link, content)
    p["content"] = content

    search.append({"t": p["title"], "g": p["group"], "u": p["slug"] + ".html", "d": p["desc"]})
    for hid, text in re.findall(r'<h3 id="([^"]+)">(.*?)</h3>', content):
        search.append({"t": strip(text), "g": p["title"], "u": f'{p["slug"]}.html#{hid}'})
    seen = set()
    for code in re.findall(r"<tr><td><code>(.*?)</code>", content):
        name = strip(code)
        if name in seen:
            continue
        seen.add(name)
        search.append({"t": name, "g": p["title"], "u": p["slug"] + ".html", "c": 1})

os.makedirs(OUT, exist_ok=True)
with open(os.path.join(OUT, "search.js"), "w", encoding="utf-8", newline="\n") as f:
    f.write("// Generado por gen_manual.py: indice del buscador del manual.\n")
    f.write("window.CRAMION_SEARCH = " + json.dumps(search, ensure_ascii=False) + ";\n")

# --- Plantilla ---------------------------------------------------------------------
# Estilo shadcn/ui en CSS propio (docs-src/manual.css, se copia a docs/manual/):
# cabecera con buscador tipo Command (Ctrl+K o /), menu lateral que en el movil es
# un panel deslizante, "En esta pagina", tarjeta de donacion y modo claro/oscuro.
import shutil
shutil.copyfile(os.path.join(HERE, "manual.css"), os.path.join(OUT, "manual.css"))

DONATE = "https://paypal.me/evan2025"

UI_ICONS = {
    "search": '<circle cx="11" cy="11" r="7"/><path d="m20 20-3.5-3.5"/>',
    "menu": '<path d="M4 6h16M4 12h16M4 18h16"/>',
    "x": '<path d="M18 6 6 18M6 6l12 12"/>',
    "sun": '<circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4"/>',
    "moon": '<path d="M20 14.5A8 8 0 0 1 9.5 4a8 8 0 1 0 10.5 10.5Z"/>',
    "heart": '<path d="M19 14c1.5-1.5 3-3.2 3-5.5A5.5 5.5 0 0 0 16.5 3c-1.8 0-3 .5-4.5 2-1.5-1.5-2.7-2-4.5-2A5.5 5.5 0 0 0 2 8.5c0 2.3 1.5 4 3 5.5l7 7Z"/>',
    "chev": '<path d="m9 6 6 6-6 6"/>',
    "chevdown": '<path d="m6 9 6 6 6-6"/>',
    "arrow": '<path d="M5 12h14M13 6l6 6-6 6"/>',
    "arrowl": '<path d="M19 12H5M11 6l-6 6 6 6"/>',
    "copy": '<rect x="9" y="9" width="12" height="12" rx="2"/><path d="M5 15V5a2 2 0 0 1 2-2h10"/>',
    "check": '<path d="M20 6 9 17l-5-5"/>',
    "file": '<path d="M14 3H7a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2V8Z"/><path d="M14 3v5h5"/>',
    "hash": '<path d="M4 9h16M4 15h16M10 3 8 21M16 3l-2 18"/>',
    "discord": '<path d="M8.5 16.5c-2 .6-3.6 0-3.6 0C3.7 13 4 9 5.4 6.6 6.8 5.6 8.5 5.3 8.5 5.3l.6 1.2c1.9-.4 3.9-.4 5.8 0l.6-1.2s1.7.3 3.1 1.3C20 9 20.3 13 19.1 16.5c0 0-1.6.6-3.6 0l-.8-1.3"/><circle cx="9.5" cy="12" r="1"/><circle cx="14.5" cy="12" r="1"/>',
    "home": '<path d="m3 11 9-8 9 8"/><path d="M5 10v10h14V10"/>',
}
ICONS.update(UI_ICONS)

HEAD = """<!doctype html>
<html lang="es" data-theme="dark">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>{title}</title>
  <meta name="description" content="{desc}">
  <meta name="theme-color" content="#09090b">
  <link rel="icon" href="../img/icon.png">
  <script>
    // Tema antes de pintar (sin parpadeo). Oscuro por defecto.
    try {{ const t = localStorage.getItem("cramion-theme"); if (t) document.documentElement.dataset.theme = t; }} catch (e) {{}}
  </script>
  <link rel="preconnect" href="https://fonts.googleapis.com">
  <link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
  <link href="https://fonts.googleapis.com/css2?family=Geist:wght@400;500;600;700&family=Geist+Mono:wght@400;500&display=swap" rel="stylesheet">
  <link rel="stylesheet" href="https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/styles/base16/tomorrow-night.min.css">
  <link rel="stylesheet" href="manual.css">
  <script src="https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/highlight.min.js"></script>
  <script src="https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/languages/lua.min.js"></script>
  <script src="https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/languages/glsl.min.js"></script>
  <script src="https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/languages/json.min.js"></script>
  <script src="https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/languages/bash.min.js"></script>
  <script src="search.js"></script>
</head>
<body>
"""

HEADER = f"""
  <header class="site-header">
    <div class="header-inner">
      <button id="menu-btn" class="icon-btn menu-btn" aria-label="Abrir el menú">{icon("menu")}</button>
      <a href="index.html" class="brand"><img src="../img/icon.png" alt=""><span>Cramion</span><span class="tag">Docs</span></a>
      <nav class="main-nav" aria-label="Secciones">
        <a href="index.html" data-nav="index">Documentación</a>
        <a href="editor-interfaz.html" data-nav="El editor">Editor</a>
        <a href="componentes.html" data-nav="Componentes">Componentes</a>
        <a href="cpp-script.html" data-nav="API de C++">API de C++</a>
        <a href="ejemplo-girar.html" data-nav="Ejemplos">Ejemplos</a>
      </nav>
      <div class="header-right">
        <button id="search-open" class="search-trigger" aria-label="Buscar en la documentación">
          {icon("search")}<span>Buscar en la documentación...</span><kbd>Ctrl K</kbd>
        </button>
        <a href="{DISCORD}" target="_blank" rel="noopener" class="icon-btn hide-mobile" aria-label="Discord" title="Discord">{icon("discord")}</a>
        <button id="theme" class="icon-btn" aria-label="Cambiar entre tema claro y oscuro" title="Tema">
          <span class="i-sun">{icon("sun")}</span><span class="i-moon">{icon("moon")}</span>
        </button>
        <a href="{DONATE}" target="_blank" rel="noopener" class="btn btn-donate btn-sm hide-mobile">{icon("heart")}Donar</a>
        <a href="{DONATE}" target="_blank" rel="noopener" class="icon-btn only-mobile" aria-label="Donar con PayPal" title="Donar">{icon("heart")}</a>
      </div>
    </div>
  </header>
  <div class="overlay" id="overlay"></div>
"""

SEARCH_DIALOG = f"""
  <div id="cmd" class="cmd-backdrop" hidden>
    <div class="cmd" role="dialog" aria-modal="true" aria-label="Buscar">
      <div class="cmd-input">{icon("search")}<input id="search" type="search" autocomplete="off" spellcheck="false"
        placeholder="Buscar funciones, componentes, páginas..."><kbd>Esc</kbd></div>
      <div id="results" class="cmd-list"></div>
      <div class="cmd-foot"><span><kbd>↑</kbd><kbd>↓</kbd> moverse</span><span><kbd>Enter</kbd> abrir</span><span><kbd>Esc</kbd> cerrar</span></div>
    </div>
  </div>
"""

FOOTER = f"""
  <footer class="site-footer">
    <div class="footer-inner">
      <span>Cramion Engine · manual de la {{version}}. Gratis para hacer y vender tus juegos.</span>
      <nav>
        <a href="../index.html">Web</a>
        <a href="licencia.html">Licencia</a>
        <a href="{DISCORD}" target="_blank" rel="noopener">Discord</a>
        <a href="{DONATE}" target="_blank" rel="noopener">Donar con PayPal</a>
      </nav>
    </div>
  </footer>
"""

DONATE_CARD = f"""
      <div class="donate-card">
        <div class="t">{icon("heart")}Apoya Cramion</div>
        <p>El motor es gratis y lo seguirá siendo. Una donación ayuda a seguir añadiendo cosas.</p>
        <a href="{DONATE}" target="_blank" rel="noopener" class="btn btn-donate btn-sm btn-block">Donar con PayPal</a>
      </div>"""


def read_version():
    try:
        text = open(os.path.join(ROOT, "CMakeLists.txt"), encoding="utf-8").read()
        m = re.search(r"project\(Cramion\s+VERSION\s+(\d+)\.(\d+)\.(\d+)", text)
        if m:
            v = f"{m.group(1)}.{m.group(2)}"
            return v if m.group(3) == "0" else f"{v}.{m.group(3)}"
    except OSError:
        pass
    return ""


VERSION = read_version()


def sidebar(current):
    out = [f'<div class="sheet-head"><a href="index.html" class="brand"><img src="../img/icon.png" alt=""><span>Cramion</span></a>'
           f'<button id="menu-close" class="icon-btn" aria-label="Cerrar el menú">{icon("x")}</button></div>']
    out.append('<div class="group"><p class="group-title">' + icon("home") + 'Inicio</p>'
               + '<a href="index.html"{}>Introducción</a>'.format(' class="active" aria-current="page"' if current == "index" else "")
               + '<a href="../index.html">Web de Cramion</a></div>')
    for group, gicon, _ in GROUPS:
        links = []
        for p in pages:
            if p["group"] == group:
                active = ' class="active" aria-current="page"' if p["slug"] == current else ""
                links.append(f'<a href="{p["slug"]}.html"{active}>{p["title"]}</a>')
        out.append(f'<div class="group"><p class="group-title">{icon(gicon)}{group}</p>' + "".join(links) + "</div>")
    out.append(DONATE_CARD.replace('class="donate-card"', 'class="donate-card side-donate"'))
    return "\n        ".join(out)


SCRIPT = """
  <script>
    // Solo el codigo con lenguaje (los arboles de carpetas y la salida quedan tal cual).
    hljs.configure({ cssSelector: 'pre code[class*="language-"]' });
    hljs.highlightAll();
    const ICON_COPY = '__COPY__', ICON_CHECK = '__CHECK__';

    // Tema claro / oscuro.
    const root = document.documentElement;
    const syncTheme = () => {
      const dark = root.dataset.theme === "dark";
      document.querySelector("#theme .i-sun").style.display = dark ? "inline-flex" : "none";
      document.querySelector("#theme .i-moon").style.display = dark ? "none" : "inline-flex";
    };
    syncTheme();
    document.getElementById("theme").addEventListener("click", () => {
      root.dataset.theme = root.dataset.theme === "dark" ? "light" : "dark";
      try { localStorage.setItem("cramion-theme", root.dataset.theme); } catch (e) {}
      syncTheme();
    });

    // Seccion de la cabecera marcada.
    const group = document.body.dataset.group;
    document.querySelectorAll(".main-nav a").forEach(a => a.classList.toggle("on", a.dataset.nav === group));

    // Menu lateral (panel deslizante en el movil).
    const side = document.getElementById("side");
    const setMenu = open => document.body.classList.toggle("menu-open", open);
    document.getElementById("menu-btn").addEventListener("click", () => setMenu(true));
    document.getElementById("menu-close").addEventListener("click", () => setMenu(false));
    document.getElementById("overlay").addEventListener("click", () => setMenu(false));
    const current = side.querySelector("a.active");
    if (current) side.scrollTop = current.offsetTop - side.clientHeight / 2;

    // Ancla de cada seccion y boton de copiar en el codigo.
    document.querySelectorAll(".doc h3[id]").forEach(h => {
      const a = document.createElement("a");
      a.className = "anchor";
      a.href = "#" + h.id;
      a.setAttribute("aria-label", "Enlace a esta sección");
      a.textContent = "#";
      h.appendChild(a);
    });
    document.querySelectorAll(".doc pre").forEach(pre => {
      const b = document.createElement("button");
      b.className = "copy";
      b.setAttribute("aria-label", "Copiar el código");
      b.innerHTML = ICON_COPY;
      b.addEventListener("click", async () => {
        try {
          await navigator.clipboard.writeText(pre.querySelector("code")?.innerText ?? pre.innerText);
          b.innerHTML = ICON_CHECK;
        } catch (e) { b.textContent = "!"; }
        setTimeout(() => (b.innerHTML = ICON_COPY), 1400);
      });
      pre.appendChild(b);
    });

    // Tablas anchas: se desplazan dentro de su marco.
    document.querySelectorAll(".doc table").forEach(t => {
      const w = document.createElement("div");
      w.className = "table-wrap";
      t.replaceWith(w);
      w.appendChild(t);
    });

    // "En esta pagina": los h3, con la seccion visible marcada (columna y desplegable).
    const heads = [...document.querySelectorAll(".doc h3[id]")];
    const lists = [...document.querySelectorAll(".onpage")];
    if (heads.length < 2) {
      document.querySelectorAll(".toc-col > div, .toc-inline").forEach(el => el.remove());
    } else {
      lists.forEach(list => list.innerHTML = heads.map(h => `<a href="#${h.id}">${h.firstChild.textContent}</a>`).join(""));
      const links = [...document.querySelectorAll(".onpage a")];
      const obs = new IntersectionObserver(entries => {
        for (const e of entries) {
          if (e.isIntersecting) links.forEach(a => a.classList.toggle("active", a.getAttribute("href") === "#" + e.target.id));
        }
      }, { rootMargin: "-80px 0px -70% 0px" });
      heads.forEach(h => obs.observe(h));
      document.querySelectorAll(".toc-inline a").forEach(a => a.addEventListener("click", () => a.closest("details").open = false));
    }

    // Buscador (Command): titulos, secciones y nombres de la API.
    const cmd = document.getElementById("cmd");
    const input = document.getElementById("search");
    const box = document.getElementById("results");
    const norm = s => s.toLowerCase().normalize("NFD").replace(/[\\u0300-\\u036f]/g, "");
    const esc = s => s.replace(/[&<>"]/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));
    const ICON_PAGE = '__FILE__', ICON_HASH = '__HASH__', ICON_CODE = '__CODE__';
    let hits = [], sel = 0;
    const suggested = window.CRAMION_SEARCH.filter(e => e.d).slice(0, 8);
    const render = () => {
      const q = input.value.trim();
      const list = q ? hits : suggested;
      if (!list.length) { box.innerHTML = '<p class="none">Sin resultados para "' + esc(q) + '".</p>'; return; }
      box.innerHTML = (q ? "" : '<p class="heading">Páginas</p>') + list.map((h, i) => `
        <a href="${h.u}" class="${i === sel ? "sel" : ""}">
          ${h.c ? ICON_CODE : h.d ? ICON_PAGE : ICON_HASH}
          <span><span class="t${h.c ? " code" : ""}">${esc(h.t)}</span>
          <span class="g">${esc(h.g)}</span></span>
        </a>`).join("");
      box.querySelector("a.sel")?.scrollIntoView({ block: "nearest" });
    };
    const openSearch = () => { cmd.hidden = false; document.body.style.overflow = "hidden"; sel = 0; render(); setTimeout(() => input.focus(), 0); };
    const closeSearch = () => { cmd.hidden = true; document.body.style.overflow = ""; input.value = ""; hits = []; };
    document.getElementById("search-open").addEventListener("click", openSearch);
    cmd.addEventListener("click", ev => { if (ev.target === cmd) closeSearch(); });
    input.addEventListener("input", () => {
      const q = norm(input.value.trim());
      sel = 0;
      if (!q) { hits = []; render(); return; }
      const words = q.split(/\\s+/);
      hits = window.CRAMION_SEARCH
        .map(e => {
          const t = norm(e.t), all = t + " " + norm(e.g) + " " + norm(e.d || "");
          if (!words.every(w => all.includes(w))) return null;
          const score = (t.startsWith(q) ? 0 : t.includes(q) ? 1 : 2) + (e.c ? 0.5 : 0) - (e.d ? 0.3 : 0);
          return { e, score };
        })
        .filter(Boolean).sort((a, b) => a.score - b.score).slice(0, 20).map(x => x.e);
      render();
    });
    input.addEventListener("keydown", ev => {
      const list = input.value.trim() ? hits : suggested;
      if (ev.key === "ArrowDown") { sel = Math.min(sel + 1, list.length - 1); render(); ev.preventDefault(); }
      if (ev.key === "ArrowUp") { sel = Math.max(sel - 1, 0); render(); ev.preventDefault(); }
      if (ev.key === "Enter" && list[sel]) { location.href = list[sel].u; closeSearch(); }
    });
    document.addEventListener("keydown", ev => {
      const typing = /INPUT|TEXTAREA/.test(document.activeElement.tagName);
      if ((ev.key === "k" && (ev.ctrlKey || ev.metaKey)) || (ev.key === "/" && !typing)) { ev.preventDefault(); cmd.hidden ? openSearch() : closeSearch(); }
      if (ev.key === "Escape") { if (!cmd.hidden) closeSearch(); setMenu(false); }
    });
  </script>
</body>
</html>
"""
SCRIPT = (SCRIPT.replace("__COPY__", icon("copy")).replace("__CHECK__", icon("check"))
          .replace("__FILE__", icon("file")).replace("__HASH__", icon("hash")).replace("__CODE__", icon("code")))


def layout(current, main_html, with_onpage=True):
    toc = f"""
    <aside class="toc-col" aria-label="En esta página">
      <div>
        <p class="toc-title">En esta página</p>
        <nav class="onpage"></nav>
      </div>
      {DONATE_CARD}
    </aside>""" if with_onpage else ""
    return f"""
  <div class="layout{'' if with_onpage else ' no-toc'}">
    <aside id="side" class="sidebar" aria-label="Menú del manual">
      <nav>
        {sidebar(current)}
      </nav>
    </aside>
{main_html}{toc}
  </div>
"""


def write(name, title, desc, body, group="index"):
    text = (HEAD.format(title=html.escape(title), desc=html.escape(desc))
            + HEADER + SEARCH_DIALOG + body + FOOTER.replace("{version}", VERSION or "última versión") + SCRIPT)
    text = text.replace("<body>", f'<body data-group="{html.escape(group)}">', 1)
    with open(os.path.join(OUT, name), "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


# --- Paginas de contenido -----------------------------------------------------------
for i, p in enumerate(pages):
    prev_p = pages[i - 1] if i > 0 else None
    next_p = pages[i + 1] if i + 1 < len(pages) else None
    nav = '<nav class="pager" aria-label="Paginación">'
    if prev_p:
        nav += f'<a href="{prev_p["slug"]}.html"><span>← Anterior</span><b>{prev_p["title"]}</b></a>'
    if next_p:
        nav += f'<a class="next" href="{next_p["slug"]}.html"><span>Siguiente →</span><b>{next_p["title"]}</b></a>'
    nav += "</nav>"
    main = f"""
    <main class="doc">
      <div class="doc-inner">
        <nav class="crumbs" aria-label="Ruta"><a href="index.html">Documentación</a>{icon("chev")}<span>{p["group"]}</span>{icon("chev")}<span class="here">{p["title"]}</span></nav>
        <h1>{p["title"]}</h1>
        <p class="lede">{html.escape(p["desc"])}</p>
        <details class="toc-inline"><summary>En esta página {icon("chevdown")}</summary><nav class="onpage"></nav></details>
        <div class="body">
{p["content"]}
        </div>
        {nav}
        <div class="help"><span>¿Algo no queda claro o falta algo? Pregunta en el
          <a href="{DISCORD}" target="_blank" rel="noopener">Discord</a>.</span>
          <a href="{DONATE}" target="_blank" rel="noopener" class="btn btn-outline btn-sm">{icon("heart")}Apoyar el proyecto</a></div>
      </div>
    </main>"""
    write(p["slug"] + ".html", f'{strip(p["title"])} · Documentación de Cramion', p["desc"], layout(p["slug"], main), p["group"])

# --- Portada del manual -------------------------------------------------------------
def group_count(group):
    return sum(1 for p in pages if p["group"] == group)


group_notes = {
    "Primeros pasos": "Cómo funciona un script y cómo se trabaja con él en el editor.",
    "El editor": "Paneles, herramientas, materiales, render y exportar el juego.",
    "Componentes": "Cada componente con todas sus propiedades.",
    "Referencia de la API": "Todo lo que se puede usar desde Lua, por tema.",
    "Gráficos": "Shaders propios: el aspecto de las superficies en GLSL.",
    "Ejemplos": strip(examples_intro),
}
cards_html = ""
for group, gicon, _ in GROUPS:
    items = [p for p in pages if p["group"] == group]
    mono = group == "Referencia de la API"
    shown = items[:8]
    cls = ' class="code"' if mono else ""
    rows = "".join(
        f'<li><a href="{p["slug"]}.html"{cls} title="{html.escape(p["desc"])}">'
        f'<span>{p["title"]}</span>{icon("arrow")}</a></li>' for p in shown)
    more = f'<p class="more">y {len(items) - len(shown)} más en el menú</p>' if len(items) > len(shown) else ""
    note = group_notes.get(group, "")
    cards_html += f"""
        <section class="card">
          <div class="card-head"><span class="card-icon">{icon(gicon)}</span>
            <div><h2>{group}</h2><small>{len(items)} páginas</small></div></div>
          {f"<p>{note}</p>" if note else ""}
          <ul>{rows}</ul>
          {more}
        </section>"""

hub_main = f"""
    <main class="doc">
      <div class="doc-inner" style="max-width: 1040px">
        <section class="hero">
          <a class="badge" href="exportar.html"><b>Nuevo en la {VERSION}</b> Android en casi todos los móviles, gama baja más rápida y VR {icon("arrow")}</a>
          <h1>Documentación de Cramion</h1>
          <div class="intro">{intro_html}</div>
          <div class="actions">
            <a href="primer-script.html" class="btn btn-primary">Empezar {icon("arrow")}</a>
            <a href="cpp-script.html" class="btn btn-outline">API de C++</a>
            <button class="btn btn-ghost" onclick="document.getElementById('search-open').click()">{icon("search")}Buscar</button>
          </div>
        </section>

        <h2 class="section-title">Empieza aquí</h2>
        <p class="section-sub">Tu primer script en tres pasos.</p>
        <ol class="steps">
          <li><div><strong>Crea un script.</strong> Proyecto &gt; Crear &gt; <em>Script Lua</em>, o <em>Nuevo script</em> en el Inspector.</div></li>
          <li><div><strong>Engánchalo a un objeto.</strong> Arrastra el <code>.lua</code> a un objeto de la Jerarquía o de la Escena.</div></li>
          <li><div><strong>Dale a Play.</strong> El motor llama a <code>Start</code>, <code>Update(dt)</code>... Guarda con Ctrl+S y se recarga en caliente (<a href="ciclo-de-vida.html">ciclo de vida</a>).</div></li>
        </ol>

        <h2 class="section-title">Todo el manual</h2>
        <p class="section-sub">{len(pages)} páginas en {len(GROUPS)} secciones. Pulsa <kbd>Ctrl K</kbd> para buscar cualquier función o componente.</p>
        <div class="cards">{cards_html}
        </div>

        <section class="donate-wide">
          <div><h2>Cramion es gratis</h2><p>Hacer y vender juegos con él no cuesta nada. Si te sirve, puedes apoyar el desarrollo.</p></div>
          <a href="{DONATE}" target="_blank" rel="noopener" class="btn btn-donate">{icon("heart")}Donar con PayPal</a>
        </section>

        <div class="help"><span>La referencia completa del código está en <code>CramionCore/include/CramionCore/scripting/Scripting.h</code>.
          ¿Dudas? Pregunta en el <a href="{DISCORD}" target="_blank" rel="noopener">Discord</a>.</span></div>
      </div>
    </main>"""
write("index.html", "Documentación de Cramion", "Documentación de Cramion: el editor, los componentes, la API de Lua y ejemplos.",
      layout("index", hub_main, with_onpage=False))


print(len(pages), "paginas;", len(search), "entradas en el buscador")

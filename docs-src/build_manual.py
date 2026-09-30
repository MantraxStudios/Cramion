# Genera el manual (docs/manual/*.html) a partir de:
#   docs-src/pages.json     grupos, orden, titulo y descripcion de cada pagina
#   docs-src/pages/*.html   el contenido de cada pagina (HTML con h3, p, table, pre...)
# Uso: python docs-src/build_manual.py
# Cada pagina sale con el menu lateral, el buscador (docs/manual/search.js),
# el estilo de docs-src/manual.css (sin frameworks),
# "En esta pagina", anterior/siguiente y el boton de copiar en el codigo.
import html
import json
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DOCS = os.path.join(ROOT, "docs")
OUT = os.path.join(DOCS, "manual")
DISCORD = "https://discord.gg/zG7rSsUGEz"

data = json.load(open(os.path.join(HERE, "pages.json"), encoding="utf-8"))
intro_html = data["intro"]
examples_intro = data["examples_intro"]
GROUP_ICONS = {"Primeros pasos": "book", "El editor": "window", "Componentes": "cube", "Referencia de la API": "code", "Gráficos": "paint", "Ejemplos": "spark"}
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
# Sin frameworks: el estilo esta en docs-src/manual.css (se copia a docs/manual/).
import shutil
shutil.copyfile(os.path.join(HERE, "manual.css"), os.path.join(OUT, "manual.css"))

HEAD = """<!doctype html>
<html lang="es">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>{title}</title>
  <meta name="description" content="{desc}">
  <link rel="icon" href="../img/icon.png">
  <link rel="preconnect" href="https://fonts.googleapis.com">
  <link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
  <link href="https://fonts.googleapis.com/css2?family=IBM+Plex+Sans:wght@400;500;600;700&family=IBM+Plex+Mono:wght@400;500&display=swap" rel="stylesheet">
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

HEADER = """
  <header class="top">
    <div class="top-inner">
      <button id="menu-btn" class="menu-btn" aria-label="Menú">Menú</button>
      <a href="index.html" class="brand"><img src="../img/icon.png" alt=""><b>Cramion</b><span>manual</span></a>
      <div class="search">
        <input id="search" type="search" autocomplete="off" placeholder="Buscar (funciones, componentes, páginas)">
        <kbd>/</kbd>
        <div id="results" class="results" hidden></div>
      </div>
      <nav class="top-links">
        <a href="../index.html">Web</a>
        <a href="{discord}" target="_blank" rel="noopener">Discord</a>
        <a href="licencia.html">Licencia</a>
        <a href="https://paypal.me/evan2025" target="_blank" rel="noopener">Donar</a>
      </nav>
    </div>
  </header>
"""


def sidebar(current):
    out = ['<a href="index.html"{}>Índice</a>'.format(' class="active"' if current == "index" else "")]
    for group, _, _ in GROUPS:
        out.append(f'<p class="group">{group}</p>')
        for p in pages:
            if p["group"] == group:
                active = ' class="active"' if p["slug"] == current else ""
                out.append(f'<a href="{p["slug"]}.html"{active}>{p["title"]}</a>')
    return "\n        ".join(out)


SCRIPT = """
  <script>
    // Solo el codigo con lenguaje (los arboles de carpetas y la salida quedan tal cual).
    hljs.configure({ cssSelector: 'pre code[class*="language-"]' });
    hljs.highlightAll();

    // Menu lateral en el movil.
    const side = document.getElementById("side");
    document.getElementById("menu-btn").addEventListener("click", () => document.body.classList.toggle("menu-open"));
    document.addEventListener("click", ev => {
      if (document.body.classList.contains("menu-open") && !ev.target.closest("#side, #menu-btn")) {
        document.body.classList.remove("menu-open");
      }
    });
    // El enlace activo a la vista dentro del menu (sin mover la pagina).
    const current = side.querySelector("a.active");
    if (current) side.scrollTop = current.offsetTop - side.clientHeight / 2;

    // Ancla de cada seccion y boton de copiar en el codigo.
    document.querySelectorAll(".doc h3[id]").forEach(h => {
      const a = document.createElement("a");
      a.className = "anchor";
      a.href = "#" + h.id;
      a.textContent = "#";
      h.appendChild(a);
    });
    document.querySelectorAll(".doc pre").forEach(pre => {
      const b = document.createElement("button");
      b.className = "copy";
      b.textContent = "copiar";
      b.addEventListener("click", async () => {
        try {
          await navigator.clipboard.writeText(pre.querySelector("code")?.innerText ?? pre.innerText);
          b.textContent = "copiado";
        } catch { b.textContent = "no se pudo"; }
        setTimeout(() => (b.textContent = "copiar"), 1400);
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

    // "En esta pagina": los h3 con la seccion visible marcada.
    const onpage = document.getElementById("onpage");
    if (onpage) {
      const heads = [...document.querySelectorAll(".doc h3[id]")];
      if (heads.length < 2) {
        onpage.closest("aside").style.visibility = "hidden";
      } else {
        onpage.innerHTML = heads.map(h => `<a href="#${h.id}">${h.firstChild.textContent}</a>`).join("");
        const links = [...onpage.querySelectorAll("a")];
        const obs = new IntersectionObserver(entries => {
          for (const e of entries) {
            if (e.isIntersecting) links.forEach(a => a.classList.toggle("active", a.getAttribute("href") === "#" + e.target.id));
          }
        }, { rootMargin: "-70px 0px -70% 0px" });
        heads.forEach(h => obs.observe(h));
      }
    }

    // Buscador: titulos, secciones y nombres de la API.
    const input = document.getElementById("search");
    const box = document.getElementById("results");
    const norm = s => s.toLowerCase().normalize("NFD").replace(/[\\u0300-\\u036f]/g, "");
    const esc = s => s.replace(/[&<>"]/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));
    let hits = [], sel = 0;
    const render = () => {
      const q = input.value.trim();
      if (!hits.length) {
        box.innerHTML = q ? '<p class="none">Sin resultados.</p>' : "";
        box.hidden = !q;
        return;
      }
      box.innerHTML = hits.map((h, i) => `
        <a href="${h.u}" class="${i === sel ? "sel" : ""}">
          <span class="t${h.c ? " code" : ""}">${esc(h.t)}</span><span class="g">${esc(h.g)}</span>
          ${h.d ? `<span class="d">${esc(h.d)}</span>` : ""}
        </a>`).join("");
      box.hidden = false;
    };
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
        .filter(Boolean).sort((a, b) => a.score - b.score).slice(0, 12).map(x => x.e);
      render();
    });
    input.addEventListener("keydown", ev => {
      if (ev.key === "ArrowDown") { sel = Math.min(sel + 1, hits.length - 1); render(); ev.preventDefault(); }
      if (ev.key === "ArrowUp") { sel = Math.max(sel - 1, 0); render(); ev.preventDefault(); }
      if (ev.key === "Enter" && hits[sel]) location.href = hits[sel].u;
      if (ev.key === "Escape") { input.value = ""; hits = []; render(); input.blur(); }
    });
    document.addEventListener("keydown", ev => {
      if (ev.key === "/" && document.activeElement !== input) { ev.preventDefault(); input.focus(); }
    });
    document.addEventListener("click", ev => { if (!ev.target.closest("#results, #search")) box.hidden = true; });
    input.addEventListener("focus", () => { if (input.value.trim()) render(); });
  </script>
</body>
</html>
"""


def layout(current, main_html, with_onpage=True):
    onpage = """
    <aside class="onpage-col">
      <p>En esta página</p>
      <nav id="onpage" class="onpage"></nav>
    </aside>""" if with_onpage else ""
    return f"""
  <div class="frame">
    <aside id="side" class="side">
      <nav>
        {sidebar(current)}
      </nav>
    </aside>
{main_html}{onpage}
  </div>
"""


def write(name, title, desc, body):
    text = (HEAD.format(title=html.escape(title), desc=html.escape(desc))
            + HEADER.format(discord=DISCORD) + body + SCRIPT)
    with open(os.path.join(OUT, name), "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


# --- Paginas de contenido -----------------------------------------------------------
for i, p in enumerate(pages):
    prev_p = pages[i - 1] if i > 0 else None
    next_p = pages[i + 1] if i + 1 < len(pages) else None
    nav = '<nav class="pager">'
    if prev_p:
        nav += f'<a href="{prev_p["slug"]}.html"><span>← anterior</span><b>{prev_p["title"]}</b></a>'
    if next_p:
        nav += f'<a class="next" href="{next_p["slug"]}.html"><span>siguiente →</span><b>{next_p["title"]}</b></a>'
    nav += "</nav>"
    main = f"""
    <main class="doc">
      <div class="crumbs"><a href="index.html">Manual</a> / {p["group"]}</div>
      <h1>{p["title"]}</h1>
      <p class="lede">{html.escape(p["desc"])}</p>
      <div class="body">
{p["content"]}
      </div>
      {nav}
      <p class="help">¿Algo no queda claro o falta algo? Pregunta en el
        <a href="{DISCORD}" target="_blank" rel="noopener">Discord</a>.</p>
    </main>"""
    write(p["slug"] + ".html", f'{strip(p["title"])} · Manual de Cramion', p["desc"], layout(p["slug"], main))

# --- Portada del manual: un indice --------------------------------------------------
def toc(group, mono=False):
    rows = []
    for p in pages:
        if p["group"] != group:
            continue
        cls = ' class="code"' if mono else ""
        rows.append(f'<li><a href="{p["slug"]}.html"{cls}>{p["title"]}</a><span>{html.escape(p["desc"])}</span></li>')
    return "\n          ".join(rows)


def group_count(group):
    return sum(1 for p in pages if p["group"] == group)


group_notes = {
    "Primeros pasos": "Cómo funciona un script y cómo se trabaja con él en el editor.",
    "Referencia de la API": "Todo lo que se puede usar desde Lua, por tema.",
    "Gráficos": "Shaders propios: el aspecto de las superficies en GLSL.",
    "Ejemplos": strip(examples_intro),
}
toc_html = ""
for number, (group, _, _) in enumerate(GROUPS, start=1):
    note = group_notes.get(group, "")
    toc_html += f'''
      <section class="toc-group">
        <h2><small>{number:02d}</small>{group}</h2>
        {f"<p>{note}</p>" if note else ""}
        <ul class="toc">
          {toc(group, mono=group == "Referencia de la API")}
        </ul>
      </section>'''

hub_main = f"""
    <main class="doc cover">
      <div class="crumbs">Manual · Lua 5.4 · {len(pages)} páginas</div>
      <h1>Manual de Cramion</h1>
      <div class="intro">{intro_html}</div>
      <div class="start">
        <a href="primer-script.html">Tu primer script</a>
        <a href="entity.html">Referencia de la API</a>
        <a href="{examples[0]["slug"]}.html">Ejemplos</a>
        <a href="exportar.html">Exportar el juego</a>
      </div>
      <section class="toc-group">
        <h2><small>00</small>Empieza aquí</h2>
        <ol class="steps">
          <li><div><strong>Crea un script.</strong> Proyecto &gt; Crear &gt; <em>Script Lua</em>, o <em>Nuevo script</em> en el Inspector.</div></li>
          <li><div><strong>Engánchalo a un objeto.</strong> Arrastra el <code>.lua</code> a un objeto de la Jerarquía o de la Escena.</div></li>
          <li><div><strong>Dale a Play.</strong> El motor llama a <code>Start</code>, <code>Update(dt)</code>... Guarda con Ctrl+S y se recarga en caliente (<a href="ciclo-de-vida.html">ciclo de vida</a>).</div></li>
        </ol>
      </section>
{toc_html}
      <p class="help">La referencia completa del código está en <code>CramionCore/include/CramionCore/scripting/Scripting.h</code>.
        ¿Dudas? Pregunta en el <a href="{DISCORD}" target="_blank" rel="noopener">Discord</a>.</p>
    </main>"""
write("index.html", "Manual de Cramion", "Manual de Cramion: el editor, los componentes, la API de Lua y ejemplos.",
      layout("index", hub_main, with_onpage=False))


print(len(pages), "paginas;", len(search), "entradas en el buscador")

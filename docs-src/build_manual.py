# Genera el manual (docs/manual/*.html) a partir de:
#   docs-src/pages.json     grupos, orden, titulo y descripcion de cada pagina
#   docs-src/pages/*.html   el contenido de cada pagina (HTML con h3, p, table, pre...)
# Uso: python docs-src/build_manual.py
# Cada pagina sale con el menu lateral, el buscador (docs/manual/search.js),
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
GITHUB = "https://github.com/MantraxStudios/Cramion"

data = json.load(open(os.path.join(HERE, "pages.json"), encoding="utf-8"))
intro_html = data["intro"]
examples_intro = data["examples_intro"]
GROUP_ICONS = {"Primeros pasos": "book", "Referencia de la API": "code", "Gráficos": "paint", "Ejemplos": "spark"}
GROUPS = []
for g in data["groups"]:
    GROUPS.append((g["group"], GROUP_ICONS.get(g["group"], "book"), g["pages"]))


ICONS = {
    "paint": '<path d="M12 3a9 9 0 1 0 0 18c1.1 0 1.5-.8 1.5-1.5 0-.9-.7-1.2-.7-2 0-.8.6-1.5 1.5-1.5H16a5 5 0 0 0 5-5c0-4.4-4-8-9-8Z"/><circle cx="7.5" cy="11" r="1"/><circle cx="10" cy="7" r="1"/><circle cx="15" cy="7.5" r="1"/>',
    "book": '<path d="M4 5a2 2 0 0 1 2-2h13v16H6a2 2 0 0 0-2 2V5Z"/><path d="M4 19a2 2 0 0 1 2-2h13"/>',
    "code": '<path d="m8 8-4 4 4 4"/><path d="m16 8 4 4-4 4"/><path d="m14 4-4 16"/>',
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
HEAD = """<!doctype html>
<html lang="es" class="scroll-smooth">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>{title}</title>
  <meta name="description" content="{desc}">
  <link rel="icon" href="../img/icon.png">
  <link rel="preconnect" href="https://fonts.googleapis.com">
  <link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
  <link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700;800&family=JetBrains+Mono:wght@400;500&display=swap" rel="stylesheet">
  <script src="https://cdn.jsdelivr.net/npm/@tailwindcss/browser@4"></script>
  <link rel="stylesheet" href="https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/styles/atom-one-dark.min.css">
  <script src="https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/highlight.min.js"></script>
  <script src="https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/languages/lua.min.js"></script>
  <script src="https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/languages/glsl.min.js"></script>
  <script src="https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/languages/json.min.js"></script>
  <script src="https://cdnjs.cloudflare.com/ajax/libs/highlight.js/11.9.0/languages/bash.min.js"></script>
  <script src="search.js"></script>
  <style type="text/tailwindcss">
    @theme {{
      --color-ink: #0c0e12;
      --color-panel: #13161d;
      --color-line: #232834;
      --color-brand: #0ab0ff;
      --font-sans: "Inter", ui-sans-serif, system-ui, sans-serif;
      --font-mono: "JetBrains Mono", ui-monospace, monospace;
    }}
    body {{ background: var(--color-ink); }}
    .hljs {{ background: transparent !important; padding: 0 !important; }}

    .doc h3 {{ @apply mt-12 scroll-mt-24 border-b border-line pb-2 text-xl font-semibold text-white first:mt-0; }}
    .doc p  {{ @apply mt-4 leading-7 text-slate-400; }}
    .doc ul {{ @apply mt-4 list-disc space-y-1.5 pl-5 text-slate-400; }}
    .doc :not(pre) > code {{ @apply rounded bg-panel px-1.5 py-0.5 font-mono text-[0.85em] text-sky-300; }}
    .doc pre {{ @apply relative mt-4 overflow-x-auto rounded-xl border border-line bg-panel p-5 text-sm leading-relaxed; }}
    .doc table {{ @apply mt-4 w-full border-collapse text-left text-sm; }}
    .doc th {{ @apply border-b border-line py-2 pr-4 font-semibold text-slate-200; }}
    .doc td {{ @apply border-b border-line/60 py-2.5 pr-4 align-top text-slate-400; }}
    .doc td:first-child {{ @apply whitespace-nowrap; }}
    .doc .table-wrap {{ @apply mt-4 overflow-x-auto rounded-xl border border-line px-4; }}
    .doc .table-wrap table {{ @apply mt-0; }}
    .doc .table-wrap tr:last-child td {{ @apply border-b-0; }}
    .note {{ @apply mt-5 rounded-xl border border-brand/40 bg-brand/10 p-4 text-sm leading-6 text-slate-300; }}
    .file {{ @apply !mb-0 !mt-6 flex items-center gap-2 font-mono text-xs !text-slate-500; }}
    .file + pre {{ @apply !mt-2; }}

    .side a {{ @apply block rounded-md px-3 py-1.5 text-sm text-slate-400 transition hover:bg-panel hover:text-white; }}
    .side a.active {{ @apply bg-brand/10 font-medium text-brand; }}
    .onpage a {{ @apply block border-l border-line py-1 pl-3 text-[13px] text-slate-500 transition hover:text-white; }}
    .onpage a.active {{ @apply border-brand text-brand; }}
    .copy {{ @apply absolute right-3 top-3 rounded-md border border-line bg-ink/80 px-2 py-1 font-sans text-xs text-slate-400 opacity-0 transition hover:text-white; }}
    pre:hover .copy {{ @apply opacity-100; }}
  </style>
</head>
<body class="font-sans text-slate-300 antialiased">
"""

HEADER = """
  <header class="sticky top-0 z-40 border-b border-line/70 bg-ink/85 backdrop-blur">
    <nav class="mx-auto flex max-w-[88rem] items-center gap-4 px-4 py-3 sm:px-6">
      <button id="menu-btn" class="rounded-md p-1.5 text-slate-400 hover:bg-panel hover:text-white lg:hidden" aria-label="Menú">
        <svg class="h-5 w-5" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M4 6h16M4 12h16M4 18h16"/></svg>
      </button>
      <a href="index.html" class="flex shrink-0 items-center gap-2.5">
        <img src="../img/icon.png" alt="" class="h-8 w-8">
        <span class="hidden text-lg font-bold tracking-wide text-white sm:inline">CRAMION</span>
        <span class="ml-1 hidden rounded border border-line px-2 py-0.5 text-xs text-slate-400 sm:inline">Manual</span>
      </a>
      <div class="relative mx-auto min-w-0 max-w-md flex-1">
        <input id="search" type="search" autocomplete="off" placeholder="Buscar en el manual..."
               class="w-full rounded-lg border border-line bg-panel py-2 pl-9 pr-10 text-sm text-white placeholder:text-slate-500 focus:border-brand focus:outline-none">
        <svg class="pointer-events-none absolute left-3 top-2.5 h-4 w-4 text-slate-500" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><circle cx="11" cy="11" r="7"/><path d="m20 20-3.5-3.5"/></svg>
        <kbd class="pointer-events-none absolute right-2.5 top-2 hidden rounded border border-line px-1.5 font-mono text-[11px] text-slate-500 sm:block">/</kbd>
        <div id="results" class="absolute inset-x-0 top-full mt-2 hidden max-h-[70vh] overflow-y-auto rounded-xl border border-line bg-panel p-2 shadow-2xl shadow-black/60"></div>
      </div>
      <div class="hidden shrink-0 items-center gap-5 text-sm md:flex">
        <a href="../index.html" class="hover:text-white">Inicio</a>
        <a href="{discord}" target="_blank" rel="noopener" class="hover:text-white">Discord</a>
        <a href="{github}" target="_blank" rel="noopener" class="hover:text-white">GitHub</a>
      </div>
    </nav>
  </header>
"""


def sidebar(current):
    out = ['<a href="index.html" class="{}">Inicio del manual</a>'.format("active" if current == "index" else "")]
    for group, gicon, _ in GROUPS:
        out.append(f'<p class="mb-1 mt-7 flex items-center gap-2 px-3 text-xs font-semibold uppercase tracking-wider text-slate-500">{icon(gicon, "h-3.5 w-3.5")}{group}</p>')
        for p in pages:
            if p["group"] == group:
                active = ' class="active"' if p["slug"] == current else ""
                out.append(f'<a href="{p["slug"]}.html"{active}>{p["title"]}</a>')
    return "\n        ".join(out)


SCRIPT = """
  <script>
    hljs.highlightAll();

    // Menu lateral en el movil.
    const side = document.getElementById("side");
    const shade = document.getElementById("shade");
    const toggle = open => {
      side.classList.toggle("-translate-x-full", !open);
      shade.classList.toggle("hidden", !open);
    };
    document.getElementById("menu-btn").addEventListener("click", () => toggle(side.classList.contains("-translate-x-full")));
    shade.addEventListener("click", () => toggle(false));
    // El enlace activo a la vista dentro del menu (sin mover la pagina).
    const current = side.querySelector("a.active");
    if (current) side.scrollTop = current.offsetTop - side.clientHeight / 2;

    // Boton de copiar en cada bloque de codigo.
    document.querySelectorAll(".doc pre").forEach(pre => {
      const b = document.createElement("button");
      b.className = "copy";
      b.textContent = "Copiar";
      b.addEventListener("click", async () => {
        try {
          await navigator.clipboard.writeText(pre.querySelector("code")?.innerText ?? pre.innerText);
          b.textContent = "Copiado";
        } catch { b.textContent = "No se pudo"; }
        setTimeout(() => (b.textContent = "Copiar"), 1400);
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

    // "En esta pagina": los h3 con resaltado de la seccion visible.
    const onpage = document.getElementById("onpage");
    if (onpage) {
      const heads = [...document.querySelectorAll(".doc h3[id]")];
      if (heads.length < 2) {
        onpage.closest("aside").classList.add("xl:invisible");
      } else {
        onpage.innerHTML = heads.map(h => `<a href="#${h.id}">${h.textContent}</a>`).join("");
        const links = [...onpage.querySelectorAll("a")];
        const obs = new IntersectionObserver(entries => {
          for (const e of entries) {
            if (e.isIntersecting) links.forEach(a => a.classList.toggle("active", a.getAttribute("href") === "#" + e.target.id));
          }
        }, { rootMargin: "-80px 0px -70% 0px" });
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
      if (!hits.length) {
        box.innerHTML = input.value.trim() ? '<p class="px-3 py-2 text-sm text-slate-500">Sin resultados.</p>' : "";
        box.classList.toggle("hidden", !input.value.trim());
        return;
      }
      box.innerHTML = hits.map((h, i) => `
        <a href="${h.u}" class="block rounded-lg px-3 py-2 ${i === sel ? "bg-brand/15" : "hover:bg-ink"}">
          <span class="${h.c ? "font-mono text-sky-300" : "font-medium text-white"} text-sm">${esc(h.t)}</span>
          <span class="ml-2 text-xs text-slate-500">${esc(h.g)}</span>
          ${h.d ? `<span class="mt-0.5 block text-xs text-slate-400">${esc(h.d)}</span>` : ""}
        </a>`).join("");
      box.classList.remove("hidden");
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
    document.addEventListener("click", ev => { if (!ev.target.closest("#results, #search")) box.classList.add("hidden"); });
    input.addEventListener("focus", () => { if (input.value.trim()) render(); });
  </script>
</body>
</html>
"""


def layout(current, main_html, with_onpage=True):
    onpage = """
    <aside class="sticky top-16 hidden h-[calc(100vh-4rem)] w-52 shrink-0 overflow-y-auto py-10 xl:block">
      <p class="mb-3 text-xs font-semibold uppercase tracking-wider text-slate-500">En esta página</p>
      <nav id="onpage" class="onpage"></nav>
    </aside>""" if with_onpage else ""
    return f"""
  <div id="shade" class="fixed inset-0 z-30 hidden bg-black/60 lg:hidden"></div>
  <div class="mx-auto flex max-w-[88rem] gap-10 px-4 sm:px-6">
    <aside id="side" class="side fixed inset-y-0 left-0 z-40 w-72 -translate-x-full overflow-y-auto border-r border-line bg-ink px-3 py-6 transition-transform lg:sticky lg:top-16 lg:z-0 lg:h-[calc(100vh-4rem)] lg:w-60 lg:shrink-0 lg:translate-x-0 lg:border-0 lg:px-0 lg:py-10">
      <nav>
        {sidebar(current)}
      </nav>
    </aside>
{main_html}{onpage}
  </div>
"""


def write(name, title, desc, body):
    text = (HEAD.format(title=html.escape(title), desc=html.escape(desc))
            + HEADER.format(discord=DISCORD, github=GITHUB) + body + SCRIPT)
    with open(os.path.join(OUT, name), "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


# --- Paginas de contenido -----------------------------------------------------------
for i, p in enumerate(pages):
    prev_p = pages[i - 1] if i > 0 else None
    next_p = pages[i + 1] if i + 1 < len(pages) else None
    nav = '<div class="mt-16 grid gap-4 border-t border-line pt-8 sm:grid-cols-2">'
    if prev_p:
        nav += f'''
        <a href="{prev_p["slug"]}.html" class="group rounded-xl border border-line p-4 transition hover:border-brand/60 hover:bg-panel">
          <span class="text-xs text-slate-500">← Anterior</span>
          <span class="mt-1 block font-semibold text-white group-hover:text-brand">{prev_p["title"]}</span>
        </a>'''
    else:
        nav += "<span></span>"
    if next_p:
        nav += f'''
        <a href="{next_p["slug"]}.html" class="group rounded-xl border border-line p-4 text-right transition hover:border-brand/60 hover:bg-panel">
          <span class="text-xs text-slate-500">Siguiente →</span>
          <span class="mt-1 block font-semibold text-white group-hover:text-brand">{next_p["title"]}</span>
        </a>'''
    nav += "</div>"
    main = f"""
    <main class="doc min-w-0 flex-1 overflow-x-hidden py-10 lg:max-w-3xl">
      <div class="flex items-center gap-2 text-sm text-slate-500">
        <a href="index.html" class="hover:text-white">Manual</a><span>/</span>
        <span class="flex items-center gap-1.5 text-brand">{icon(p["icon"], "h-3.5 w-3.5")}{p["group"]}</span>
      </div>
      <h1 class="mt-3 text-4xl font-extrabold tracking-tight text-white">{p["title"]}</h1>
      <p class="!mt-3 text-lg !text-slate-400">{html.escape(p["desc"])}</p>
      <div class="mt-10">
{p["content"]}
      </div>
      {nav}
      <p class="mt-10 text-sm text-slate-500">¿Algo no queda claro o falta algo? Pregunta en el
        <a href="{DISCORD}" target="_blank" rel="noopener" class="text-brand hover:text-white">Discord</a>.</p>
    </main>"""
    write(p["slug"] + ".html", f'{strip(p["title"])} · Manual de Cramion', p["desc"], layout(p["slug"], main))

# --- Portada del manual --------------------------------------------------------------
def cards(group, mono=False):
    items = []
    for p in pages:
        if p["group"] != group:
            continue
        title_cls = "font-mono text-sky-300" if mono else "font-semibold text-white"
        items.append(f'''
          <a href="{p["slug"]}.html" class="group rounded-xl border border-line bg-panel/60 p-5 transition hover:-translate-y-0.5 hover:border-brand/60 hover:bg-panel">
            <span class="{title_cls} group-hover:text-brand">{p["title"]}</span>
            <span class="mt-2 block text-sm leading-6 text-slate-400">{html.escape(p["desc"])}</span>
          </a>''')
    return "".join(items)


steps = [
    ("1", "Crea un script", "Proyecto &gt; Crear &gt; <strong class=\"text-white\">Script Lua</strong>, o <em>Nuevo script</em> en el Inspector.", "primer-script"),
    ("2", "Engánchalo a un objeto", "Arrastra el <code>.lua</code> a un objeto de la Jerarquía o de la Escena.", "primer-script"),
    ("3", "Dale a Play", "El motor llama a <code>Start</code>, <code>Update(dt)</code>... Guarda con Ctrl+S y se recarga en caliente.", "ciclo-de-vida"),
]
steps_html = "".join(f'''
          <a href="{u}.html" class="group rounded-xl border border-line bg-panel/60 p-5 transition hover:border-brand/60">
            <span class="flex h-8 w-8 items-center justify-center rounded-full bg-brand/15 font-mono text-sm font-semibold text-brand">{n}</span>
            <span class="mt-4 block font-semibold text-white group-hover:text-brand">{t}</span>
            <span class="mt-1 block text-sm leading-6 text-slate-400">{d}</span>
          </a>''' for n, t, d, u in steps)

def section(gicon, title, text, inner, cols="sm:grid-cols-2 xl:grid-cols-3"):
    return f'''
      <section class="mt-16">
        <h2 class="flex items-center gap-2.5 text-2xl font-bold text-white"><span class="rounded-lg bg-brand/15 p-1.5 text-brand">{icon(gicon, "h-5 w-5")}</span>{title}</h2>
        <p class="mt-2 text-slate-400">{text}</p>
        <div class="mt-6 grid gap-4 {cols}">{inner}
        </div>
      </section>'''

hub_main = f"""
    <main class="doc min-w-0 flex-1 py-10">
      <div class="relative overflow-hidden rounded-2xl border border-line bg-gradient-to-br from-brand/15 via-panel to-ink p-8 sm:p-12">
        <div class="pointer-events-none absolute -right-24 -top-24 h-72 w-72 rounded-full bg-brand/20 blur-3xl"></div>
        <p class="!mt-0 text-sm font-semibold uppercase tracking-widest text-brand">Scripting en Lua 5.4</p>
        <h1 class="mt-2 text-4xl font-extrabold tracking-tight text-white sm:text-5xl">Manual de Cramion</h1>
        <div class="max-w-2xl">{intro_html}</div>
        <div class="mt-8 flex flex-wrap gap-3">
          <a href="primer-script.html" class="rounded-lg bg-brand px-5 py-2.5 font-semibold text-ink hover:bg-white">Empezar →</a>
          <a href="entity.html" class="rounded-lg border border-line bg-ink/60 px-5 py-2.5 font-semibold text-white hover:border-brand">Referencia de la API</a>
          <a href="{examples[0]["slug"]}.html" class="rounded-lg border border-line bg-ink/60 px-5 py-2.5 font-semibold text-white hover:border-brand">Ver ejemplos</a>
        </div>
      </div>
{section("book", "Empieza aquí", "Tu primer script en tres pasos.", steps_html, "sm:grid-cols-3")}
{section("book", "Primeros pasos", "Cómo funciona un script y cómo se trabaja con él en el editor.", cards("Primeros pasos"))}
{section("code", "Referencia de la API", "Todo lo que puedes usar desde Lua, por tema.", cards("Referencia de la API", mono=True))}
{section("paint", "Gráficos", "Tus propios shaders: el aspecto de las superficies en GLSL.", cards("Gráficos"))}
{section("spark", "Ejemplos", strip(examples_intro), cards("Ejemplos"))}
      <p class="mt-16 border-t border-line pt-8 text-sm text-slate-500">
        La referencia completa del código está en <code>CramionCore/include/CramionCore/scripting/Scripting.h</code>.
        ¿Dudas? Pregunta en el <a href="{DISCORD}" target="_blank" rel="noopener" class="text-brand hover:text-white">Discord</a>.
      </p>
    </main>"""
write("index.html", "Manual de Cramion", "Manual de scripting en Lua de Cramion: primeros pasos, referencia de la API y ejemplos.",
      layout("index", hub_main, with_onpage=False))


print(len(pages), "paginas;", len(search), "entradas en el buscador")

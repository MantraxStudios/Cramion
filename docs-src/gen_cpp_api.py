# Genera la referencia de la API de C++ del manual (docs-src/pages/cpp-*.html y
# el grupo "API de C++" de pages.json) leyendo las cabeceras del SDK:
#   CramionCore/sdk/cramion/Script.h, Types.h, Api.gen.h, EntityApi.gen.inc
# Cada funcion, campo y tipo publico sale con su firma y su comentario ///.
# Asi la documentacion nunca se queda atras del SDK (Api.gen.h la genera
# cramion_sdkgen desde la API real del motor).
# Uso: python docs-src/gen_cpp_api.py   (build_manual.py lo llama solo)
import html
import json
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SDK = os.path.join(ROOT, "CramionCore", "sdk", "cramion")
PAGES = os.path.join(HERE, "pages")
GROUP = "API de C++"

# Ambitos que no son API publica.
HIDDEN = {"detail", "math_detail", "cppproto", "std"}


def read(name):
    return open(os.path.join(SDK, name), encoding="utf-8").read()


def parse(text):
    """Devuelve {ambito: [(doc, declaracion)]} con 'Entity', 'Mathf', 'cramion'..."""
    out = {}
    order = []
    i, n = 0, len(text)
    stack = []  # (nombre, es_clase, privado)
    doc = []
    stmt = ""

    def scope_name():
        names = [s[0] for s in stack if s[0] != "cramion"]
        return "::".join(names) if names else "cramion"

    def hidden():
        return any(s[0] in HIDDEN for s in stack) or any(s[1] and s[2] for s in stack[-1:])

    def add(decl):
        nonlocal doc
        decl = " ".join(decl.split())
        if decl and not hidden():
            key = scope_name()
            if key not in out:
                out[key] = []
                order.append(key)
            out[key].append((" ".join(doc).strip(), decl))
        doc = []

    def skip_block(j):
        depth = 0
        while j < n:
            c = text[j]
            if c == '"' or c == "'":
                q = c
                j += 1
                while j < n and text[j] != q:
                    j += 2 if text[j] == "\\" else 1
            elif text.startswith("//", j):
                j = text.find("\n", j)
                if j < 0:
                    return n
            elif c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    return j + 1
            j += 1
        return n

    while i < n:
        c = text[i]
        if text.startswith("///", i):
            end = text.find("\n", i)
            doc.append(text[i + 3:end].strip())
            i = end + 1
            continue
        if text.startswith("//", i):
            i = text.find("\n", i) + 1 or n
            continue
        if text.startswith("/*", i):
            i = text.find("*/", i) + 2
            continue
        if c == "#" and stmt.strip() == "":
            i = text.find("\n", i) + 1 or n
            continue
        if c == '"' or c == "'":
            q = c
            j = i + 1
            while j < n and text[j] != q:
                j += 2 if text[j] == "\\" else 1
            stmt += text[i:j + 1]
            i = j + 1
            continue
        if c == "}":
            if stack:
                stack.pop()
            stmt = ""
            doc = []
            i += 1
            continue
        if c == ";":
            s = stmt.strip()
            if s:
                add(s)
            stmt = ""
            i += 1
            continue
        if c == ":" and stack and stack[-1][1] and stmt.strip() in ("public", "private", "protected"):
            name, is_class, _ = stack[-1]
            stack[-1] = (name, is_class, stmt.strip() != "public")
            stmt = ""
            doc = []
            i += 1
            continue
        if c == "{" and stmt.count("(") > stmt.count(")"):
            # Dentro de los parametros (const T0& x = {}): es parte de la declaracion.
            j = skip_block(i)
            stmt += text[i:j]
            i = j
            continue
        if c == "{":
            s = " ".join(stmt.split())
            m = re.match(r"^(?:template\s*<.*?>\s*)?(namespace|class|struct)\s+(\w+)\b([^()]*)$", s)
            if m and not s.startswith("enum"):
                kind, name, _rest = m.groups()
                if kind != "namespace" and doc:
                    add(f"{kind} {name}")
                stack.append((name, kind != "namespace", kind == "class"))
                stmt = ""
                doc = []
                i += 1
                continue
            if s.startswith("enum"):
                j = skip_block(i)
                body = text[i + 1:j - 1]
                values = ", ".join(v.strip().split("=")[0].strip() for v in body.split(",") if v.strip())
                add(f"{s} {{ {values} }}")
                stmt = ""
                i = j
                continue
            # Inicializador con llaves (Vec3 v{...}) dentro de una declaracion: se copia.
            head = re.sub(r"^template\s*<[^>]*>\s*", "", s)
            before = head.split("(")[0]
            initializer = "(" not in head or ("=" in before and "operator" not in before)
            if initializer:
                j = skip_block(i)
                stmt += text[i:j]
                i = j
                continue
            # Cuerpo de funcion: se quita.
            add(s)
            stmt = ""
            i = skip_block(i)
            # ; opcional tras el cuerpo
            continue
        stmt += c
        i += 1
    return out, order


def clean(decl):
    d = decl
    d = re.sub(r"^template\s*<[^>]*(<[^>]*>[^>]*)*>\s*", "", d)
    for word in ("inline ", "constexpr ", "IMGUI_API ", "explicit ", "virtual "):
        d = d.replace(word, "")
    d = re.sub(r"const T\d+& (\w+) = \{\}", r"\1", d)
    d = re.sub(r"\w+&&\.\.\. \w+", "...", d)
    d = re.sub(r"\s*:\s*\w+\(.*$", "", d) if re.match(r"^\w+\(", d) else d  # constructores con inicializadores
    d = d.replace(" override", "")
    return d.strip()


def skip(decl):
    d = decl.strip()
    if d.startswith(("friend ", "using namespace", "static_assert", "class Script;")):
        return True
    if "operator" in d and "operator bool" not in d and "operator Value" not in d:
        return False
    if re.match(r"^(private|public|protected)\b", d):
        return True
    return False


def describe(doc):
    # Las generadas: "Tabla.fn(args) descripcion". Se ensena la descripcion.
    m = re.match(r"^([A-Za-z]+[.:][A-Za-z_]+)\((.*?)\)\s*(.*)$", doc)
    if m:
        lua, args, text = m.groups()
        return text if text else (f"Argumentos: {args}" if args and args != "..." else "")
    return doc


def table(entries):
    rows = []
    seen = set()
    for doc, decl in entries:
        if skip(decl):
            continue
        sig = clean(decl)
        if not sig or sig in seen or sig.startswith(("~",)):
            continue
        seen.add(sig)
        rows.append(f'<tr><td><code>{html.escape(sig)}</code></td><td>{html.escape(describe(doc))}</td></tr>')
    if not rows:
        return ""
    return "<table>\n  <tbody>\n    " + "\n    ".join(rows) + "\n  </tbody>\n</table>\n"


# Paginas: (slug, titulo, descripcion, intro, [ambitos])
TITLES = {
    "Audio": "Sonidos sueltos, filtros y oclusión por paredes.",
    "CharacterController": "Constantes del Character Controller (con qué chocó).",
    "DataPack": "Cargar escenas y objetos de un .datapack en marcha.",
    "Environment": "Hora, fecha, estaciones, clima, viento y niebla.",
    "Weather": "Lo mismo que Environment (otro nombre).",
    "Fire": "Incendios: encender, apagar y consultar.",
    "Fluid": "Líquidos por partículas: emisores, densidad y tipos.",
    "Game": "El juego: salir.",
    "Graphics": "Configuración gráfica y post-procesado.",
    "Http": "Peticiones HTTPS a servidores y webs.",
    "Json": "Texto JSON a valores y al revés.",
    "Navigation": "NavMesh: caminos, puntos y rayos sobre la malla de navegación.",
    "Network": "Multijugador: crear y unirse, mensajes, objetos de red.",
    "Prefs": "Datos guardados entre partidas (como PlayerPrefs).",
    "Random": "Aleatorios con semilla.",
    "Screen": "Tamaño y orientación de la pantalla.",
    "Voxel": "Mundo de bloques: leer y poner bloques, mundos guardados.",
    "XR": "Realidad virtual: casco, mandos, botones y vibración.",
}


def main():
    api_text = read("Api.gen.h")
    script_text = read("Script.h").replace('#include "EntityApi.gen.inc"', read("EntityApi.gen.inc"))
    script_text = script_text.replace('#include "Api.gen.h"', "")
    scopes = {}
    for text in (read("Types.h"), script_text, api_text):
        parsed, _ = parse(text)
        for k, v in parsed.items():
            scopes.setdefault(k, []).extend(v)

    def page(slug, title, desc, intro, sections):
        body = [f"<p>{intro}</p>\n"]
        for heading, keys in sections:
            entries = []
            for k in keys:
                entries.extend(scopes.get(k, []))
            t = table(entries)
            if t:
                body.append(f'<h3 id="{re.sub(r"[^a-z0-9]+", "-", heading.lower()).strip("-")}">{html.escape(heading)}</h3>\n{t}')
        open(os.path.join(PAGES, slug + ".html"), "w", encoding="utf-8").write("\n".join(body))
        return {"slug": slug, "title": title, "desc": desc}

    pages = []
    pages.append(page("cpp-script", "Script y propiedades", "La clase Script, sus eventos, Property<T> y sus opciones, assets y archivos.",
                      "Cada script es una clase que hereda de <code>Script</code> y se registra con <code>CRAMION_SCRIPT(Clase)</code>. "
                      "Ver también <a href=\"scripts-cpp.html\">Scripts en C++</a>.",
                      [("Script: eventos", ["Script"]), ("Property<T>", ["Property"]),
                       ("Opciones de las propiedades", ["Range", "Tooltip", "Header", "Label", "Options", "Requires"]),
                       ("Assets y archivos", ["cramion", "AssetRef", "FileRef"]), ("Collision", ["Collision"])]))
    pages.append(page("cpp-entity", "Entity", "Un objeto de la escena: transformación, componentes, física, sonido, animación, IK, red...",
                      "Un objeto de la escena. El del script es <code>entity()</code>; otros se buscan con "
                      "<a href=\"cpp-scene.html\">Scene</a> o llegan en los eventos (<code>Collision::other</code>).",
                      [("Entity", ["Entity"])]))
    pages.append(page("cpp-matematicas", "Mathf, Vec3, Quat y Color", "Matemáticas como las de Unity, calculadas en el script.",
                      "Tipos y funciones matemáticas. Se calculan dentro del script (no llaman al motor), así que son rápidas.",
                      [("Mathf", ["Mathf"]), ("Vec3", ["Vec3"]), ("Vec2", ["Vec2"]), ("Quat", ["Quat"]), ("Color", ["Color"])]))
    for name, title, desc in (("Scene", "Scene", "Buscar, crear, instanciar y destruir objetos; cambiar de escena."),
                              ("Input", "Input", "Teclado, ratón, ejes, mando, táctil e Input Actions."),
                              ("Physics", "Physics", "Raycasts."),
                              ("Time", "Time", "Tiempo del frame y del juego."),
                              ("Debug", "Debug", "Mensajes en la Consola."),
                              ("Lua", "Lua (puente)", "Llamar a código Lua desde C++ (proyectos antiguos)."),
                              ("CVars", "CVars", "Variables de configuración del motor y del juego.")):
        extra = {"Input": ["Input::MouseState", "MouseState"], "Physics": ["RaycastHit"]}.get(name, [])
        pages.append(page("cpp-" + name.lower(), title, desc, desc, [(name, [name] + extra)]))
    pages.append(page("cpp-mesh", "Mesh (mallas por código)", "Crear y cambiar mallas desde C++.",
                      "Mallas creadas por código: <code>Mesh m = Mesh::cube(2.0f);</code>, cambia vértices y llama a <code>apply()</code>.",
                      [("Mesh", ["Mesh"])]))
    pages.append(page("cpp-statemachine", "StateMachine", "Máquinas de estados desde C++.",
                      "La máquina de estados de un objeto: <code>StateMachine sm = entity().getStateMachine();</code>",
                      [("StateMachine", ["StateMachine"])]))
    done = {"Scene", "Input", "Physics", "Time", "Debug", "Lua", "CVars", "Mesh", "StateMachine", "Entity", "Mathf", "Vec2",
            "Vec3", "Quat", "Color", "Script", "Property"}
    for name in sorted(k for k in scopes if "::" not in k and k not in done and k != "cramion" and k in TITLES):
        pages.append(page("cpp-" + name.lower(), name, TITLES[name], TITLES[name], [(name, [name])]))

    data = json.load(open(os.path.join(HERE, "pages.json"), encoding="utf-8"))
    intro = [p for p in data["groups"] if p["group"] == GROUP]
    first = [{"slug": "scripts-cpp", "title": "Scripts en C++ y CVars",
              "desc": "Cómo se escriben, el Inspector, el editor con IntelliSense, el aislamiento y las CVars."}]
    group = {"group": GROUP, "pages": first + pages}
    groups = [g for g in data["groups"] if g["group"] != GROUP]
    # Justo despues de "Primeros pasos".
    at = next((k + 1 for k, g in enumerate(groups) if g["group"] == "Primeros pasos"), 1)
    groups.insert(at, group)
    data["groups"] = groups
    json.dump(data, open(os.path.join(HERE, "pages.json"), "w", encoding="utf-8"), ensure_ascii=False, indent=2)
    total = sum(open(os.path.join(PAGES, p["slug"] + ".html"), encoding="utf-8").read().count("<tr>") for p in pages)
    print(f"API de C++: {len(pages)} paginas, {total} entradas")


if __name__ == "__main__":
    main()

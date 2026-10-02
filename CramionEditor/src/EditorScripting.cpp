// Scripting en el editor: el editor de scripts Lua integrado (pestanas,
// resaltado de sintaxis, numeros de linea, sangria automatica, Ctrl+S guarda
// y recarga en caliente, errores marcados en su linea), la consola de Lua, el
// Inspector del componente Script (propiedades del script con su control) y
// el del AudioSource (arrastrar un clip, escucharlo).

#include "EditorApp.h"

#include <CramionCore/cvar/CVar.h>

#include "Dialogs.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

namespace cramion::editor {

using core::Vec3;

namespace {

// --- Resaltado de Lua ---
enum class Token { Text, Keyword, Api, Number, String, Comment, Function };

ImU32 tokenColor(Token t) {
    switch (t) {
        case Token::Keyword: return IM_COL32(197, 134, 192, 255);
        case Token::Api: return IM_COL32(78, 201, 176, 255);
        case Token::Number: return IM_COL32(181, 206, 168, 255);
        case Token::String: return IM_COL32(206, 145, 120, 255);
        case Token::Comment: return IM_COL32(106, 153, 85, 255);
        case Token::Function: return IM_COL32(220, 220, 170, 255);
        default: return IM_COL32(212, 212, 212, 255);
    }
}

bool isKeyword(std::string_view w) {
    static constexpr const char* kWords[] = {"and",   "break", "do",    "else", "elseif", "end",    "false",
                                             "for",   "function", "goto", "if",   "in",     "local",  "nil",
                                             "not",   "or",    "repeat", "return", "then", "true",   "until",
                                             "while"};
    for (const char* k : kWords) {
        if (w == k) return true;
    }
    return false;
}

// Tablas y funciones globales del motor (la lista la da el propio motor).
bool isApi(std::string_view w) { return luaIsApiName(w); }

// --- Resaltado de GLSL (shaders de superficie, .crshader) ---
bool isGlslKeyword(std::string_view w) {
    static constexpr const char* kWords[] = {
        "void", "float", "int", "uint", "bool", "vec2", "vec3", "vec4", "ivec2", "ivec3", "ivec4", "mat2", "mat3",
        "mat4", "sampler2D", "if", "else", "for", "while", "do", "return", "break", "continue", "discard", "in",
        "out", "inout", "const", "struct", "true", "false", "property", "range", "color", "vector", "texture2D"};
    for (const char* k : kWords) {
        if (w == k) return true;
    }
    return false;
}

bool isGlslApi(std::string_view w) {
    static constexpr const char* kNames[] = {
        "Surface", "Vertex", "TIME", "CAMERA_POSITION", "texture", "mix", "clamp", "smoothstep", "step", "sin", "cos",
        "tan", "pow", "exp", "sqrt", "abs", "min", "max", "floor", "fract", "mod", "dot", "cross", "normalize",
        "length", "distance", "reflect", "noise", "fbm", "hash", "fresnel", "remap", "dFdx", "dFdy", "saturate"};
    for (const char* k : kNames) {
        if (w == k) return true;
    }
    return false;
}

// --- Resaltado de C++ (scripts de C++) ---
bool isCppKeyword(std::string_view w) {
    static constexpr const char* kWords[] = {
        "alignas", "auto", "bool", "break", "case", "catch", "char", "class", "const", "constexpr", "continue", "decltype",
        "default", "delete", "do", "double", "else", "enum", "explicit", "extern", "false", "float", "for", "friend", "if",
        "inline", "int", "long", "mutable", "namespace", "new", "noexcept", "nullptr", "operator", "override", "private",
        "protected", "public", "return", "short", "signed", "sizeof", "static", "static_cast", "struct", "switch",
        "template", "this", "throw", "true", "try", "typedef", "typename", "union", "unsigned", "using", "virtual", "void",
        "volatile", "while", "final", "concept", "requires", "co_await", "dynamic_cast", "reinterpret_cast", "const_cast",
        "#include", "#pragma", "#define", "#if", "#ifdef", "#ifndef", "#endif", "#else"};
    for (const char* k : kWords) {
        if (w == k) return true;
    }
    return false;
}

bool isCppApi(std::string_view w) {
    static constexpr const char* kNames[] = {
        "cramion", "Script", "Entity", "Vec2", "Vec3", "Quat", "Color", "Value", "Values", "Property", "Range", "Tooltip",
        "Header", "Label", "Options", "Requires", "Prefab", "Model", "Material", "Texture", "AudioClip", "SceneAsset",
        "Collision", "RaycastHit", "Debug", "Input", "Physics", "Scene", "Time", "Lua", "CVar", "CVars", "Audio", "UI",
        "Network", "Navigation", "Graphics", "Prefs", "Voxel", "Http", "Random", "Screen", "Game", "XR", "Weather", "Fire",
        "Fluid", "Environment", "DataPack", "CRAMION_SCRIPT", "CR_PROPERTY", "std", "string", "vector", "uint32_t",
        "uint64_t", "size_t"};
    for (const char* k : kNames) {
        if (w == k) return true;
    }
    return false;
}

// Tokens de una linea. `in_block_comment` sigue de una linea a la siguiente.
// `glsl`: comentarios // y /* */ y las palabras de GLSL (o de C++ con `cpp`).
void tokenizeLine(std::string_view line, bool& in_block_comment,
                  std::vector<std::pair<std::string_view, Token>>& out, bool glsl = false, bool cpp = false) {
    if (cpp) glsl = true;  // mismos comentarios y textos
    out.clear();
    std::size_t i = 0;
    const auto push = [&](std::size_t from, std::size_t to, Token t) {
        if (to > from) out.emplace_back(line.substr(from, to - from), t);
    };
    bool after_function = false;
    while (i < line.size()) {
        if (in_block_comment) {
            const std::size_t end = line.find(glsl ? "*/" : "]]", i);
            const std::size_t stop = end == std::string_view::npos ? line.size() : end + 2;
            push(i, stop, Token::Comment);
            if (end != std::string_view::npos) in_block_comment = false;
            i = stop;
            continue;
        }
        const char c = line[i];
        if (glsl && c == '/' && i + 1 < line.size() && (line[i + 1] == '/' || line[i + 1] == '*')) {
            if (line[i + 1] == '*') {
                in_block_comment = true;
                continue;
            }
            push(i, line.size(), Token::Comment);
            break;
        }
        if (!glsl && c == '-' && i + 1 < line.size() && line[i + 1] == '-') {
            if (line.substr(i, 4) == "--[[") {
                in_block_comment = true;
                continue;
            }
            push(i, line.size(), Token::Comment);
            break;
        }
        if (c == '"' || c == '\'') {
            std::size_t j = i + 1;
            while (j < line.size() && line[j] != c) j += line[j] == '\\' ? 2 : 1;
            push(i, std::min(j + 1, line.size()), Token::String);
            i = std::min(j + 1, line.size());
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && i + 1 < line.size() && std::isdigit(static_cast<unsigned char>(line[i + 1])))) {
            std::size_t j = i;
            while (j < line.size() && (std::isalnum(static_cast<unsigned char>(line[j])) || line[j] == '.')) ++j;
            push(i, j, Token::Number);
            i = j;
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || (cpp && c == '#')) {
            std::size_t j = i + 1;
            while (j < line.size() && (std::isalnum(static_cast<unsigned char>(line[j])) || line[j] == '_')) ++j;
            const std::string_view word = line.substr(i, j - i);
            Token t = Token::Text;
            if (cpp ? isCppKeyword(word) : (glsl ? isGlslKeyword(word) : isKeyword(word))) {
                t = Token::Keyword;
            } else if (cpp ? isCppApi(word) : (glsl && isGlslApi(word))) {
                t = Token::Api;
            } else if (after_function || (j < line.size() && line[j] == '(')) {
                t = Token::Function;
            } else if (!glsl && isApi(word)) {
                t = Token::Api;
            }
            after_function = word == "function" || (after_function && (line.substr(j, 1) == ":" || line.substr(j, 1) == "."));
            push(i, j, t);
            i = j;
            continue;
        }
        std::size_t j = i + 1;
        while (j < line.size() && !std::isalnum(static_cast<unsigned char>(line[j])) && line[j] != '_' &&
               line[j] != '"' && line[j] != '\'' && line[j] != (glsl ? '/' : '-')) {
            ++j;
        }
        push(i, j, Token::Text);
        i = j;
    }
}

struct EditState {
    int cursor = 0;
    bool tab = false;
    bool newline = false;
    bool typed = false;       // se escribio un caracter este frame
    char last_char = 0;
    int restore_cursor = -1;  // flechas usadas por la lista de sugerencias
    const std::string* accept = nullptr;  // sugerencia elegida
    int accept_start = 0;
    // Cerrar solos ( [ { " ' (y < en C++): el que cierra se pone detras del cursor.
    bool pairs = true;
    bool cpp = false;
    char pair_close = 0;
    int skip_over = 0;          // se escribio el que cierra y ya estaba: se salta
    bool delete_after = false;  // Retroceso entre "()": se borran los dos
};

char closerFor(char c) {
    switch (c) {
        case '(': return ')';
        case '[': return ']';
        case '{': return '}';
        case '"': return '"';
        case '\'': return '\'';
        case '<': return '>';
        default: return 0;
    }
}

bool isWordChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// C++: la lista de clangd filtrada por lo escrito (empiezan por eso primero).
std::vector<LuaCompletion> filterCppCompletions(const std::vector<LuaCompletion>& raw, const std::string& prefix) {
    std::string low = prefix;
    for (char& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    std::vector<LuaCompletion> first, second;
    for (const LuaCompletion& c : raw) {
        std::string lower_label = c.label;
        for (char& ch : lower_label) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        const std::size_t at = lower_label.find(low);
        if (!low.empty() && at == std::string::npos) continue;
        (at == 0 ? first : second).push_back(c);
    }
    first.insert(first.end(), second.begin(), second.end());
    if (first.size() > 200) first.resize(200);
    return first;
}

int editCallback(ImGuiInputTextCallbackData* data) {
    auto* state = static_cast<EditState*>(data->UserData);
    if (data->EventFlag == ImGuiInputTextFlags_CallbackCharFilter) {
        state->typed = true;
        state->last_char = static_cast<char>(data->EventChar);
        // Parejas: el texto de ahora (el del campo) y el cursor.
        const ImGuiInputTextState* input = ImGui::GetInputTextState(data->ID);
        if (state->pairs && input != nullptr && data->SelectionStart == data->SelectionEnd && data->EventChar < 128) {
            const char* buf = input->TextA.Data;
            const int len = input->TextLen;
            const int cur = data->CursorPos;
            const char c = static_cast<char>(data->EventChar);
            const char next = buf != nullptr && cur < len ? buf[cur] : 0;
            const char prev = buf != nullptr && cur > 0 && cur <= len ? buf[cur - 1] : 0;
            // El que cierra y ya esta delante: se pasa por encima.
            if ((c == ')' || c == ']' || c == '}' || c == '"' || c == '\'' || (c == '>' && state->cpp)) && next == c) {
                ++state->skip_over;
                return 1;
            }
            char close = closerFor(c);
            if (close != 0) {
                const bool free_after = next == 0 || std::isspace(static_cast<unsigned char>(next)) ||
                                        std::strchr(")]};,>", next) != nullptr;
                if (!free_after) close = 0;
                // Comillas: no dentro de una palabra (don't, x'...).
                if ((c == '"' || c == '\'') && (isWordChar(prev) || prev == c)) close = 0;
                if ((c == '\'' ) && (isWordChar(prev) || !free_after)) close = 0;
                // < solo en C++: plantillas (vector<, Property<) y #include <...>
                if (c == '<') {
                    bool include = false;
                    int ls = cur;
                    while (ls > 0 && buf[ls - 1] != '\n') --ls;
                    while (ls < cur && (buf[ls] == ' ' || buf[ls] == '\t')) ++ls;
                    include = cur - ls >= 8 && std::strncmp(buf + ls, "#include", 8) == 0;
                    if (!state->cpp || !(include || isWordChar(prev))) close = 0;
                }
                state->pair_close = close;
            }
        }
        if (data->EventChar == '\t') {
            data->EventChar = ' ';  // tabulador = 4 espacios (los 3 que faltan, abajo)
            state->tab = true;
        } else if (data->EventChar == '\n') {
            state->newline = true;
        }
        return 0;
    }
    if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways) {
        if (state->restore_cursor >= 0) {
            data->CursorPos = data->SelectionStart = data->SelectionEnd = std::min(state->restore_cursor, data->BufTextLen);
        }
        if (state->accept != nullptr) {
            const int start = std::clamp(state->accept_start, 0, data->CursorPos);
            data->DeleteChars(start, data->CursorPos - start);
            data->InsertChars(start, state->accept->c_str());
            state->accept = nullptr;
        }
        if (state->tab) {
            state->tab = false;
            data->InsertChars(data->CursorPos, "   ");
        }
        if (state->skip_over > 0) {
            data->CursorPos = std::min(data->CursorPos + state->skip_over, data->BufTextLen);
            data->SelectionStart = data->SelectionEnd = data->CursorPos;
            state->skip_over = 0;
        }
        if (state->pair_close != 0) {
            const char close[2] = {state->pair_close, 0};
            data->InsertChars(data->CursorPos, close);
            data->CursorPos -= 1;  // entre los dos
            data->SelectionStart = data->SelectionEnd = data->CursorPos;
            state->pair_close = 0;
        }
        if (state->delete_after) {
            if (data->CursorPos < data->BufTextLen) data->DeleteChars(data->CursorPos, 1);
            state->delete_after = false;
        }
        if (state->newline) {
            state->newline = false;
            // Sangria automatica: la de la linea anterior (+4 tras then/do/function/{).
            const int line_end = data->CursorPos - 1;
            int line_start = line_end;
            while (line_start > 0 && data->Buf[line_start - 1] != '\n') --line_start;
            int indent = 0;
            while (line_start + indent < line_end && data->Buf[line_start + indent] == ' ') ++indent;
            std::string previous(data->Buf + line_start, static_cast<std::size_t>(std::max(line_end - line_start, 0)));
            while (!previous.empty() && std::isspace(static_cast<unsigned char>(previous.back()))) previous.pop_back();
            const auto ends = [&](const char* suffix) {
                const std::size_t n = std::strlen(suffix);
                return previous.size() >= n && previous.compare(previous.size() - n, n, suffix) == 0;
            };
            const int base_indent = indent;
            const bool brace = ends("{");
            if (ends("then") || ends("do") || ends("{") || ends("else") || ends("repeat") ||
                previous.find("function") != std::string::npos && ends(")")) {
                indent += 4;
            }
            if (indent > 0) data->InsertChars(data->CursorPos, std::string(static_cast<std::size_t>(indent), ' ').c_str());
            if (brace && data->CursorPos < data->BufTextLen && data->Buf[data->CursorPos] == '}') {
                const int keep = data->CursorPos;
                data->InsertChars(keep, ("\n" + std::string(static_cast<std::size_t>(base_indent), ' ')).c_str());
                data->CursorPos = data->SelectionStart = data->SelectionEnd = keep;
            }
        }
        state->cursor = data->CursorPos;
    }
    return 0;
}

std::string readAll(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string safeClassName(std::string name) {
    std::string out;
    bool upper = true;
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out += upper ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
            upper = false;
        } else {
            upper = true;
        }
    }
    return out.empty() ? std::string("Script") : out;
}

}  // namespace

// Ruta dentro de Assets (con /) de un archivo del proyecto.
std::string EditorApp::assetRelative(const std::filesystem::path& file) const {
    std::error_code error;
    const std::filesystem::path relative = std::filesystem::relative(file, project_.assetsFolder(), error);
    std::string text = dialogs::utf8(error ? file.filename() : relative);
    std::replace(text.begin(), text.end(), '\\', '/');
    return text;
}

void EditorApp::createScriptAsset(const std::filesystem::path& folder, ecs::Entity attach_to) {
    const std::filesystem::path target_folder = folder.empty() ? project_.assetsFolder() / "Scripts" : folder;
    std::error_code error;
    std::filesystem::create_directories(target_folder, error);
    const std::string base = safeClassName(attach_to.valid() ? attach_to.name() : std::string("NuevoScript"));
    std::filesystem::path path = target_folder / dialogs::fromUtf8(base + ".lua");
    for (int i = 2; std::filesystem::exists(path); ++i) path = target_folder / dialogs::fromUtf8(base + std::to_string(i) + ".lua");
    std::ofstream(path, std::ios::binary) << scripting::scriptTemplate(dialogs::utf8(path.stem()));
    refreshDatabase();
    std::cout << "[Editor] Script creado: " << dialogs::utf8(path.filename()) << "\n";
    if (attach_to.valid()) {
        scripting::Script& script = attach_to.has<scripting::Script>() ? attach_to.get<scripting::Script>()
                                                                      : attach_to.add<scripting::Script>();
        script.file = assetRelative(path);
        commit();
    }
    openScript(path);
}

void EditorApp::openScript(const std::filesystem::path& file) {
    show_script_editor_ = true;
    focus_script_editor_ = true;
    for (std::size_t i = 0; i < script_tabs_.size(); ++i) {
        if (script_tabs_[i].path == file) {
            active_script_tab_ = static_cast<int>(i);
            script_tabs_[i].select = true;
            return;
        }
    }
    ScriptTab tab;
    tab.path = file;
    tab.relative = assetRelative(file);
    tab.text = readAll(file);
    tab.saved = tab.text;
    tab.select = true;
    {
        std::string ext = dialogs::utf8(file.extension());
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        tab.cpp = ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".h" || ext == ".hpp" || ext == ".inl";
    }
    script_tabs_.push_back(std::move(tab));
    active_script_tab_ = static_cast<int>(script_tabs_.size()) - 1;
    openScriptWorkspace(file);  // su pestana arriba
}

bool EditorApp::saveScript(ScriptTab& tab) {
    if (tab.on_save) {  // codigo de un estado de una maquina: lo guarda su dueno
        tab.on_save();
        tab.saved = tab.text;
        return true;
    }
    if (tab.cpp && cppFormatOnSave()) formatCppTab(tab, true);  // Configuracion del motor > C++
    std::ofstream out(tab.path, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "[Editor] No se pudo guardar " << dialogs::utf8(tab.path) << "\n";
        return false;
    }
    out << tab.text;
    out.close();
    tab.saved = tab.text;
    // Shader de superficie: se recompila y los materiales que lo usan lo ven ya.
    if (tab.path.extension() == assets::kSurfaceShaderExtension) {
        std::cout << "[Editor] Shader guardado: " << tab.relative << "\n";
        if (sync_) sync_->reloadSurfaceShaders();
        return true;
    }
    // C++: se compila ya (y en Play se recarga con la DLL nueva).
    if (tab.path.extension() != ".lua") {
        std::cout << "[Editor] Script C++ guardado: " << tab.relative << " (compilando...)\n";
        cpp_compile_errors_.clear();
        cpp_last_check_ = -10.0;
        return true;
    }
    scripts_.clearErrors();
    // En Play: recarga en caliente (las instancias conservan sus datos).
    scripts_.reloadFile(tab.relative);
    std::cout << "[Editor] Script guardado: " << tab.relative << "\n";
    return true;
}

void EditorApp::drawCodeEditor(ScriptTab& tab) {
    ImGuiIO& io = ImGui::GetIO();
    const float line_height = ImGui::GetTextLineHeight();
    ImFont* font = ImGui::GetFont();
    const float font_size = ImGui::GetFontSize();

    // Lineas y ancho del texto (el campo de texto no se desplaza: lo hace la
    // ventana que lo contiene, asi el resaltado cae encima exacto). Se miden
    // otra vez despues del campo: lo que se escribio este frame se dibuja ya
    // (antes salia un frame tarde y el texto "temblaba" detras del cursor).
    std::vector<std::string_view> lines;
    float widest = 0.0f;
    const auto measure = [&] {
        lines.clear();
        widest = 0.0f;
        std::string_view all(tab.text);
        std::size_t start = 0;
        while (true) {
            const std::size_t end = all.find('\n', start);
            lines.push_back(all.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
            if (end == std::string_view::npos) break;
            start = end + 1;
        }
        for (const std::string_view line : lines) {
            widest = std::max(widest, font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, line.data(), line.data() + line.size()).x);
        }
    };
    measure();

    // Errores de este archivo.
    int error_line = -1;
    std::string error_message;
    std::vector<scripting::ScriptError> all_errors = scripts_.errors();
    all_errors.insert(all_errors.end(), cpp_compile_errors_.begin(), cpp_compile_errors_.end());
    all_errors.insert(all_errors.end(), cpp_scripts_.errors().begin(), cpp_scripts_.errors().end());
    for (const scripting::ScriptError& error : all_errors) {
        if (error.file == tab.relative) {
            error_line = error.line;
            error_message = error.message;
        }
    }
    // Shader de superficie: el ultimo error al compilarlo ("Nombre.crshader:12: error: ...").
    const bool glsl = tab.path.extension() == assets::kSurfaceShaderExtension;
    const bool cpp = tab.cpp;
    // C++: los errores y avisos en vivo de clangd (y los de la ultima compilacion).
    std::vector<ClangdDiagnostic> diagnostics;
    if (cpp) {
        ensureClangd();
        if (!clangd_.isOpen(tab.path)) syncClangd(tab);
        diagnostics = clangd_.diagnostics(tab.path);
        for (const ClangdDiagnostic& d : diagnostics) {
            if (d.severity == 1 && (error_line < 0 || d.line + 1 < error_line)) {
                error_line = d.line + 1;
                error_message = d.message;
            }
        }
    }
    if (glsl && sync_) {
        const std::string message = sync_->surfaceShaderError(tab.relative);
        const std::string name = dialogs::utf8(tab.path.filename()) + ":";
        if (const std::size_t at = message.find(name); at != std::string::npos) {
            error_line = std::atoi(message.c_str() + at + name.size());
            const std::size_t end = message.find('\n', at);
            error_message = message.substr(at, end == std::string::npos ? std::string::npos : end - at);
        } else if (!message.empty()) {
            error_message = message.substr(0, message.find('\n'));
        }
    }

    const float gutter = ImGui::CalcTextSize("00000").x + 14.0f;
    const ImVec2 padding = ImGui::GetStyle().FramePadding;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(30, 30, 30, 255));
    const float bottom = tab.cpp ? ImGui::GetFrameHeightWithSpacing() * 2.2f + cpp_console_height_ : ImGui::GetFrameHeightWithSpacing() * 2.2f;
    ImGui::BeginChild("##code", ImVec2(0.0f, -bottom), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_AlwaysVerticalScrollbar);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float scroll_y = ImGui::GetScrollY();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const int first_visible = std::max(0, static_cast<int>(scroll_y / line_height) - 1);
    int last_visible = std::min(static_cast<int>(lines.size()), static_cast<int>((scroll_y + avail.y) / line_height) + 2);

    // El texto editable (invisible) ...
    ImGui::SetCursorScreenPos(ImVec2(origin.x + gutter, origin.y));
    // Holgura: unas letras a la derecha y media vista por debajo, asi al
    // escribir al final o en una linea larga el scroll hacia el cursor no se
    // queda corto (el contenido crece un frame despues).
    const ImVec2 size{std::max(widest + font_size * 8.0f, avail.x - gutter),
                      std::max(static_cast<float>(lines.size() + 2) * line_height + padding.y * 2.0f + avail.y * 0.5f,
                               avail.y)};
    EditState state;
    if (tab.set_cursor >= 0 && tab.was_active) {  // ir a la definicion, MCP...: mover el cursor (con el campo activo)
        state.restore_cursor = tab.set_cursor;
        tab.cursor = tab.set_cursor;
        tab.set_cursor = -1;
    }
    // Autocompletado: con la lista abierta, las flechas eligen y Enter/Tab
    // completan (se le quitan al campo de texto). Escape nunca deshace el texto.
    const ImGuiID key_owner = ImGui::GetID("##completion_keys");
    const bool list_open = tab.completion_open && !tab.completions.empty() && tab.was_active;
    std::string accepted;
    if (tab.was_active) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) tab.completion_open = false;
        ImGui::SetKeyOwner(ImGuiKey_Escape, key_owner, ImGuiInputFlags_LockThisFrame);
    }
    if (list_open) {
        const int count = static_cast<int>(tab.completions.size());
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
            tab.completion_selected = (tab.completion_selected + 1) % count;
            state.restore_cursor = tab.cursor;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
            tab.completion_selected = (tab.completion_selected + count - 1) % count;
            state.restore_cursor = tab.cursor;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
            ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
            accepted = tab.completions[std::clamp(tab.completion_selected, 0, count - 1)].insert;
            state.accept = &accepted;
            state.accept_start = static_cast<int>(tab.completion_context.prefix_start);
            tab.completion_open = false;
            ImGui::SetKeyOwner(ImGuiKey_Enter, key_owner, ImGuiInputFlags_LockThisFrame);
            ImGui::SetKeyOwner(ImGuiKey_KeypadEnter, key_owner, ImGuiInputFlags_LockThisFrame);
            ImGui::SetKeyOwner(ImGuiKey_Tab, key_owner, ImGuiInputFlags_LockThisFrame);
        }
    }
    if (tab.reload_text) {
        if (ImGuiInputTextState* input = ImGui::GetInputTextState(ImGui::GetID("##source"))) input->ReloadUserBufAndKeepSelection();
        tab.reload_text = false;
    }
    state.cpp = cpp;
    state.pairs = true;
    if (tab.was_active && ImGui::IsKeyPressed(ImGuiKey_Backspace) && !io.KeyCtrl) {
        if (const ImGuiInputTextState* input = ImGui::GetInputTextState(ImGui::GetID("##source")); input != nullptr && !input->HasSelection()) {
            const int cur = input->GetCursorPos();
            const char* buf = input->TextA.Data;
            if (buf != nullptr && cur > 0 && cur < input->TextLen && closerFor(buf[cur - 1]) == buf[cur] && buf[cur] != 0) {
                state.delete_after = true;
            }
        }
    }
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, IM_COL32(38, 79, 120, 255));
    ImGui::InputTextMultiline("##source", &tab.text, size,
                              ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_CallbackCharFilter |
                                  ImGuiInputTextFlags_CallbackAlways | ImGuiInputTextFlags_NoHorizontalScroll,
                              editCallback, &state);
    const bool active = ImGui::IsItemActive();
    const ImGuiID source_id = ImGui::GetItemID();
    tab.was_active = active;
    measure();  // el texto de este frame (con lo que se acaba de escribir)
    last_visible = std::min(static_cast<int>(lines.size()), static_cast<int>((scroll_y + avail.y) / line_height) + 2);

    // Numeros de linea.
    for (int i = first_visible; i < last_visible; ++i) {
        char number[16];
        std::snprintf(number, sizeof(number), "%d", i + 1);
        const float y = origin.y + padding.y + static_cast<float>(i) * line_height;
        const bool is_error = i + 1 == error_line;
        draw->AddText(ImVec2(origin.x + gutter - 10.0f - ImGui::CalcTextSize(number).x, y),
                      is_error ? IM_COL32(255, 90, 90, 255) : IM_COL32(110, 118, 129, 255), number);
    }

    if (state.typed || !accepted.empty()) tab.last_edit = ImGui::GetTime();
    const ImVec2 text_origin{ImGui::GetItemRectMin().x + padding.x, ImGui::GetItemRectMin().y + padding.y};
    if (active) {
        const bool typed_ident = state.typed && accepted.empty() &&
                                 (std::isalnum(static_cast<unsigned char>(state.last_char)) || state.last_char == '_' ||
                                  state.last_char == '.' || state.last_char == ':' || state.last_char == '"' ||
                                  state.last_char == '\'' || state.last_char == '/' || state.last_char == ' ');
        const bool forced = io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Space, false);
        const bool edited_while_open = tab.completion_open && state.cursor != tab.cursor;
        if (cpp) {
            // C++: clangd. Tras letras, '.', '->', '::' o Ctrl+Espacio.
            const char c = state.last_char;
            const int cur = std::min<int>(state.cursor, static_cast<int>(tab.text.size()));
            const char before = cur >= 2 ? tab.text[static_cast<std::size_t>(cur - 2)] : 0;
            const bool member = state.typed && accepted.empty() &&
                                (c == '.' || (c == '>' && before == '-') || (c == ':' && before == ':'));
            const bool ident = state.typed && accepted.empty() && (std::isalnum(static_cast<unsigned char>(c)) || c == '_');
            if (ident || member || forced || edited_while_open) {
                syncClangd(tab);
                int line = 0, line_start = 0;
                for (int k = 0; k < cur; ++k) {
                    if (tab.text[static_cast<std::size_t>(k)] == '\n') {
                        ++line;
                        line_start = k + 1;
                    }
                }
                int word = cur;
                while (word > line_start && (std::isalnum(static_cast<unsigned char>(tab.text[static_cast<std::size_t>(word - 1)])) ||
                                             tab.text[static_cast<std::size_t>(word - 1)] == '_')) {
                    --word;
                }
                if (word == cur && !member && !forced) {
                    tab.completion_open = false;
                } else {
                    // Ya hay lista de esta palabra: se filtra ya (clangd la afina despues).
                    if (!member && !tab.cpp_raw.empty() && tab.cpp_raw_start == static_cast<std::size_t>(word)) {
                        const std::string prefix = tab.text.substr(static_cast<std::size_t>(word), static_cast<std::size_t>(cur - word));
                        const std::string selected = tab.completion_open && !tab.completions.empty()
                                                         ? tab.completions[std::clamp(tab.completion_selected, 0, static_cast<int>(tab.completions.size()) - 1)].label
                                                         : std::string();
                        tab.completions = filterCppCompletions(tab.cpp_raw, prefix);
                        tab.completion_context.prefix_start = static_cast<std::size_t>(word);
                        tab.completion_context.prefix = prefix;
                        tab.completion_open = !tab.completions.empty();
                        tab.completion_selected = 0;
                        for (std::size_t k = 0; k < tab.completions.size(); ++k) {
                            if (tab.completions[k].label == selected) tab.completion_selected = static_cast<int>(k);
                        }
                    } else if (member) {
                        tab.cpp_raw.clear();
                        tab.cpp_raw_start = std::string::npos;
                    }
                    const std::uint64_t request = ++tab.completion_request;
                    const std::filesystem::path file = tab.path;
                    clangd_.completion(file, line, cur - line_start, [this, file, request, line_start, forced](std::vector<ClangdCompletion> items) {
                        ScriptTab* t = scriptTabFor(file);
                        if (t == nullptr || t->completion_request != request) return;
                        // Lo que se esta escribiendo ahora (pudo seguir escribiendo).
                        int start = t->cursor;
                        while (start > line_start && (std::isalnum(static_cast<unsigned char>(t->text[static_cast<std::size_t>(start - 1)])) ||
                                                      t->text[static_cast<std::size_t>(start - 1)] == '_')) {
                            --start;
                        }
                        std::string prefix = t->text.substr(static_cast<std::size_t>(start), static_cast<std::size_t>(t->cursor - start));
                        // Llego tarde y ya se escribio otra cosa ('(', ';', espacio...): no se abre.
                        const char before = t->cursor > 0 && t->cursor <= static_cast<int>(t->text.size())
                                                ? t->text[static_cast<std::size_t>(t->cursor - 1)] : 0;
                        if (!forced && prefix.empty() && before != '.' && before != '>' && before != ':') {
                            t->completion_open = false;
                            return;
                        }
                        // Primero lo del motor (From "cramion/..."), luego lo del script, y al final
                        // la biblioteca de C/C++ (atan2f, alloca...: si no, tapaba la API del motor).
                        std::vector<LuaCompletion> raw;
                        std::vector<int> rank;
                        raw.reserve(items.size());
                        for (const ClangdCompletion& item : items) {
                            if (!item.label.empty() && item.label[0] == '_' && (prefix.empty() || prefix[0] != '_')) continue;
                            const std::string& from = item.documentation;
                            const bool engine = from.rfind("From \"cramion/", 0) == 0;
                            const bool library = !engine && from.rfind("From ", 0) == 0;
                            // "getFloat(std::string_view c, ...) const": el nombre en la lista y la firma al lado.
                            const std::size_t paren = item.label.find('(');
                            std::string label = paren == std::string::npos ? item.label : item.label.substr(0, paren);
                            LuaCompletion c;
                            c.label = label;
                            c.insert = item.insert.empty() ? label : item.insert;
                            c.detail = paren == std::string::npos ? item.detail : item.detail + " " + item.label;
                            if (!item.documentation.empty()) {
                                // La primera linea que no sea "From <cabecera>".
                                std::string doc;
                                std::size_t at = 0;
                                while (at < item.documentation.size()) {
                                    const std::size_t end = item.documentation.find('\n', at);
                                    const std::string line =
                                        item.documentation.substr(at, end == std::string::npos ? std::string::npos : end - at);
                                    at = end == std::string::npos ? item.documentation.size() : end + 1;
                                    if (!line.empty() && line.rfind("From ", 0) != 0) {
                                        doc = line;
                                        break;
                                    }
                                }
                                if (!doc.empty()) c.detail += (c.detail.empty() ? "" : "  -  ") + doc;
                            }
                            switch (item.kind) {
                                case 2: case 3: case 4: c.kind = 2; break;               // metodo, funcion, constructor
                                case 5: case 10: case 20: case 21: c.kind = 3; break;    // campo, propiedad, enum, constante
                                case 6: case 12: c.kind = 4; break;                      // variable, valor
                                case 7: case 8: case 9: case 13: case 22: case 25: c.kind = 1; break;  // tipos, namespace
                                case 14: c.kind = 0; break;                              // palabra clave
                                default: c.kind = 4; break;
                            }
                            if (engine && c.kind != 0) c.kind = 5;  // icono del motor
                            rank.push_back(engine ? 0 : (library ? 2 : 1));
                            raw.push_back(std::move(c));
                        }
                        {
                            std::vector<std::size_t> order(raw.size());
                            for (std::size_t k = 0; k < order.size(); ++k) order[k] = k;
                            std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return rank[a] < rank[b]; });
                            std::vector<LuaCompletion> sorted;
                            sorted.reserve(raw.size());
                            for (const std::size_t k : order) sorted.push_back(std::move(raw[k]));
                            raw = std::move(sorted);
                        }
                        // Lo elegido con las flechas se mantiene si sigue en la lista.
                        const std::string selected = t->completion_open && !t->completions.empty()
                                                         ? t->completions[std::clamp(t->completion_selected, 0, static_cast<int>(t->completions.size()) - 1)].label
                                                         : std::string();
                        t->cpp_raw = std::move(raw);
                        t->cpp_raw_start = static_cast<std::size_t>(start);
                        t->completions = filterCppCompletions(t->cpp_raw, prefix);
                        t->completion_context = LuaCompletionContext{};
                        t->completion_context.prefix_start = static_cast<std::size_t>(start);
                        t->completion_context.prefix = prefix;
                        t->completion_open = !t->completions.empty();
                        t->completion_selected = 0;
                        for (std::size_t k = 0; k < t->completions.size(); ++k) {
                            if (t->completions[k].label == selected) t->completion_selected = static_cast<int>(k);
                        }
                    });
                }
            } else if (state.typed || (state.cursor != tab.cursor && state.restore_cursor < 0)) {
                tab.completion_open = false;
            }
            // Firma: al abrir un parentesis o tras una coma; se cierra con ')'.
            if (state.typed && accepted.empty() && (c == '(' || c == ',')) {
                syncClangd(tab);
                int line = 0, line_start = 0;
                for (int k = 0; k < cur; ++k) {
                    if (tab.text[static_cast<std::size_t>(k)] == '\n') {
                        ++line;
                        line_start = k + 1;
                    }
                }
                const std::filesystem::path file = tab.path;
                clangd_.signatureHelp(file, line, cur - line_start, [this, file](std::vector<ClangdSignature> sigs, int active) {
                    ScriptTab* t = scriptTabFor(file);
                    if (t == nullptr) return;
                    t->signatures = std::move(sigs);
                    t->signature_active = active;
                    t->signature_open = !t->signatures.empty();
                });
            } else if (state.typed && (c == ')' || c == ';' || c == '\n')) {
                tab.signature_open = false;
            }
            // Ir a la definicion: F12.
            if (ImGui::IsKeyPressed(ImGuiKey_F12, false)) goToDefinition(tab, state.cursor);
        } else if (!glsl && (typed_ident || forced || edited_while_open)) {
            refreshLuaSymbols();
            LuaCompletionContext context;
            if (luaCompletionContext(tab.text, static_cast<std::size_t>(state.cursor), context) &&
                (forced || !context.prefix.empty() || context.accessor != 0 || context.in_string)) {
                tab.completion_context = context;
                tab.completions = luaCompletions(tab.text, context);
                tab.completion_open = !tab.completions.empty();
                tab.completion_selected = 0;
            } else {
                tab.completion_open = false;
            }
        } else if (state.typed || (state.cursor != tab.cursor && state.restore_cursor < 0)) {
            tab.completion_open = false;  // otro caracter o el cursor se fue a otro sitio
        }
    } else {
        tab.completion_open = false;
    }
    ImGui::PopStyleColor(3);
    if (tab.focus) {
        // Activar el campo para escribir (ir a la definicion, MCP): SetKeyboardFocusHere
        // no lo activaba con la ventana recien enfocada.
        if (!active) {
            ImGui::ActivateItemByID(source_id);  // se insiste hasta que quede activo
        } else {
            tab.focus = false;
            ImGui::SetNavCursorVisible(false);  // sin el marco amarillo de la navegacion con teclado
        }
    }

    // ... y encima, con color.
    if (error_line >= 1) {
        const float y = text_origin.y + static_cast<float>(error_line - 1) * line_height;
        draw->AddRectFilled(ImVec2(origin.x, y), ImVec2(origin.x + size.x + gutter, y + line_height), IM_COL32(120, 30, 30, 110));
    }
    bool block_comment = false;
    std::vector<std::pair<std::string_view, Token>> tokens;
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        tokenizeLine(lines[i], block_comment, tokens, glsl, cpp);  // (las anteriores cuentan para --[[ ]] y /* */)
        if (i < first_visible || i >= last_visible) continue;
        float x = text_origin.x;
        const float y = text_origin.y + static_cast<float>(i) * line_height;
        for (const auto& [text, token] : tokens) {
            draw->AddText(font, font_size, ImVec2(x, y), tokenColor(token), text.data(), text.data() + text.size());
            x += font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, text.data(), text.data() + text.size()).x;
        }
    }

    // C++: subrayado de errores (rojo) y avisos (amarillo) de clangd.
    const auto column_x = [&](int line, int column) {
        if (line < 0 || line >= static_cast<int>(lines.size())) return text_origin.x;
        const std::string_view l = lines[static_cast<std::size_t>(line)];
        const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(std::max(column, 0)), l.size());
        return text_origin.x + font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, l.data(), l.data() + n).x;
    };
    for (const ClangdDiagnostic& d : diagnostics) {
        if (d.severity > 2 || d.line < first_visible || d.line >= last_visible) continue;
        const int end_column = d.end_line == d.line ? std::max(d.end_column, d.column + 1)
                                                    : static_cast<int>(lines[static_cast<std::size_t>(d.line)].size());
        float x0 = column_x(d.line, d.column);
        float x1 = column_x(d.line, end_column);
        if (x1 - x0 < 6.0f) x1 = x0 + 6.0f;
        const float y = text_origin.y + static_cast<float>(d.line + 1) * line_height - 2.0f;
        const ImU32 color = d.severity == 1 ? IM_COL32(255, 80, 80, 255) : IM_COL32(230, 190, 60, 255);
        for (float x = x0; x < x1; x += 4.0f) {
            draw->AddLine(ImVec2(x, y), ImVec2(std::min(x + 2.0f, x1), y + 2.0f), color, 1.2f);
            draw->AddLine(ImVec2(std::min(x + 2.0f, x1), y + 2.0f), ImVec2(std::min(x + 4.0f, x1), y), color, 1.2f);
        }
    }
    if (cpp) {
        // El raton: informacion de clangd (y el diagnostico de debajo) y Ctrl+clic = ir a la definicion.
        const ImVec2 mouse = io.MousePos;
        const bool over = ImGui::IsWindowHovered() && mouse.x >= text_origin.x && mouse.y >= text_origin.y;
        int mouse_offset = -1;
        int mouse_line = -1, mouse_column = 0;
        if (over) {
            mouse_line = static_cast<int>((mouse.y - text_origin.y) / line_height);
            if (mouse_line >= 0 && mouse_line < static_cast<int>(lines.size())) {
                const std::string_view l = lines[static_cast<std::size_t>(mouse_line)];
                while (mouse_column < static_cast<int>(l.size()) && column_x(mouse_line, mouse_column + 1) < mouse.x) ++mouse_column;
                if (mouse_column < static_cast<int>(l.size()) &&
                    (std::isalnum(static_cast<unsigned char>(l[static_cast<std::size_t>(mouse_column)])) || l[static_cast<std::size_t>(mouse_column)] == '_')) {
                    mouse_offset = static_cast<int>(l.data() - tab.text.data()) + mouse_column;
                }
            }
        }
        if (mouse_offset >= 0 && io.KeyCtrl && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
            ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 2.0f).x == 0.0f) {
            goToDefinition(tab, mouse_offset);
        }
        if (std::abs(mouse.x - tab.hover_mouse.x) > 2.0f || std::abs(mouse.y - tab.hover_mouse.y) > 2.0f) {
            tab.hover_mouse = mouse;
            tab.hover_since = ImGui::GetTime();
            if (mouse_offset != tab.hover_offset) tab.hover_text.clear();
        }
        if (mouse_offset >= 0 && ImGui::GetTime() - tab.hover_since > 0.45 && mouse_offset != tab.hover_offset) {
            tab.hover_offset = mouse_offset;
            tab.hover_text.clear();
            syncClangd(tab);
            const std::filesystem::path file = tab.path;
            clangd_.hover(file, mouse_line, mouse_column, [this, file, mouse_offset](std::string text) {
                ScriptTab* t = scriptTabFor(file);
                if (t != nullptr && t->hover_offset == mouse_offset) t->hover_text = std::move(text);
            });
        }
        std::string tip;
        if (mouse_line >= 0) {
            for (const ClangdDiagnostic& d : diagnostics) {
                if (d.line == mouse_line && mouse_column >= d.column && (d.end_line > d.line || mouse_column <= d.end_column)) {
                    tip += std::string(d.severity == 1 ? "error: " : "aviso: ") + d.message + "\n";
                }
            }
        }
        if (mouse_offset >= 0 && mouse_offset == tab.hover_offset && !tab.hover_text.empty()) tip += tab.hover_text;
        if (!tip.empty() && over && !tab.completion_open) {
            ImGui::BeginTooltip();
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
            ImGui::TextUnformatted(tip.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }

    // Cursor (el del campo es invisible con el texto): linea y columna.
    if (active) {
        tab.cursor = state.cursor;
        int line = 0;
        int column_start = 0;
        for (int i = 0; i < std::min<int>(state.cursor, static_cast<int>(tab.text.size())); ++i) {
            if (tab.text[i] == '\n') {
                ++line;
                column_start = i + 1;
            }
        }
        tab.line = line + 1;
        tab.column = state.cursor - column_start + 1;
        const float cx = text_origin.x + font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, tab.text.data() + column_start,
                                                             tab.text.data() + std::min<int>(state.cursor, tab.text.size())).x;
        const float cy = text_origin.y + static_cast<float>(line) * line_height;
        const double since_edit = ImGui::GetTime() - tab.last_edit;
        if (since_edit < 0.6 || std::fmod(since_edit, 1.0) < 0.6) {
            draw->AddLine(ImVec2(cx, cy), ImVec2(cx, cy + line_height), IM_COL32(230, 230, 230, 255), 1.5f);
        }
        if (!tab.completion_open) tab.popup_width = 0.0f;
        if (tab.completion_open && !tab.completions.empty()) {
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            const int count = static_cast<int>(tab.completions.size());
            const int visible = std::min(count, 9);
            const int first = std::clamp(tab.completion_selected - visible + 1, 0, std::max(count - visible, 0));
            const float row = line_height + 4.0f;
            // Ancho: solo crece mientras esta abierta; sitio: el principio de la
            // palabra (no el cursor), y arriba/abajo con el alto maximo: no salta al teclear.
            float width = std::max(420.0f, tab.popup_width);
            for (int i = 0; i < visible; ++i) {
                width = std::max(width, ImGui::CalcTextSize(tab.completions[first + i].label.c_str()).x + 60.0f);
            }
            width = std::min(width, 720.0f);
            tab.popup_width = width;
            const std::string& detail = tab.completions[tab.completion_selected].detail;
            const float detail_h = row + 4.0f;
            float anchor_x = cx - ImGui::CalcTextSize(tab.completion_context.prefix.c_str()).x;
            const std::size_t word_start = tab.completion_context.prefix_start;
            if (word_start >= static_cast<std::size_t>(column_start) && word_start <= static_cast<std::size_t>(state.cursor) &&
                word_start <= tab.text.size()) {
                anchor_x = text_origin.x + font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, tab.text.data() + column_start,
                                                               tab.text.data() + word_start).x;
            }
            ImVec2 min{anchor_x, cy + line_height + 2.0f};
            const ImVec2 display = ImGui::GetIO().DisplaySize;
            if (min.y + row * 9.0f + detail_h > display.y) min.y = cy - row * visible - detail_h - 2.0f;
            min.x = std::clamp(min.x, 0.0f, display.x - width);
            const ImVec2 max{min.x + width, min.y + row * visible + detail_h};
            fg->AddRectFilled(ImVec2(min.x + 3, min.y + 4), ImVec2(max.x + 3, max.y + 4), IM_COL32(0, 0, 0, 90), 6.0f);
            fg->AddRectFilled(min, max, IM_COL32(37, 37, 42, 250), 5.0f);
            fg->AddRect(min, max, IM_COL32(70, 90, 120, 255), 5.0f);
            static constexpr ImU32 kKindColor[] = {IM_COL32(197, 134, 192, 255), IM_COL32(78, 201, 176, 255),
                                                   IM_COL32(220, 220, 170, 255), IM_COL32(156, 220, 254, 255),
                                                   IM_COL32(180, 180, 180, 255), IM_COL32(255, 180, 90, 255)};
            static constexpr const char* kKindLetter[] = {"k", "T", "f", "p", "l", "e"};
            for (int i = 0; i < visible; ++i) {
                const LuaCompletion& item = tab.completions[first + i];
                const float y = min.y + row * static_cast<float>(i);
                if (first + i == tab.completion_selected) {
                    fg->AddRectFilled(ImVec2(min.x + 2, y + 1), ImVec2(max.x - 2, y + row - 1), IM_COL32(4, 57, 94, 255), 4.0f);
                }
                const int kind = std::clamp(item.kind, 0, 5);
                fg->AddText(ImVec2(min.x + 8, y + 2), kKindColor[kind], kKindLetter[kind]);
                fg->AddText(ImVec2(min.x + 26, y + 2), IM_COL32(230, 230, 230, 255), item.label.c_str());
            }
            if (!detail.empty()) {
                const float y = min.y + row * visible;
                fg->AddLine(ImVec2(min.x + 6, y + 1), ImVec2(max.x - 6, y + 1), IM_COL32(70, 70, 80, 255));
                fg->PushClipRect(min, max, true);
                fg->AddText(ImVec2(min.x + 8, y + 4), IM_COL32(160, 170, 185, 255), detail.c_str());
                fg->PopClipRect();
            }
            if (count > visible) {
                char more[32];
                std::snprintf(more, sizeof(more), "%d/%d", tab.completion_selected + 1, count);
                fg->AddText(ImVec2(max.x - ImGui::CalcTextSize(more).x - 8, min.y + 2), IM_COL32(120, 130, 145, 255), more);
            }
        } else if (cpp && tab.signature_open && !tab.signatures.empty()) {
            // C++: la firma de clangd con el parametro actual resaltado.
            const ClangdSignature& sig =
                tab.signatures[static_cast<std::size_t>(std::clamp(tab.signature_active, 0, static_cast<int>(tab.signatures.size()) - 1))];
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            std::string line = sig.label;
            if (tab.signatures.size() > 1) line += "   (" + std::to_string(tab.signatures.size()) + " sobrecargas)";
            const ImVec2 text_size = ImGui::CalcTextSize(line.c_str());
            ImVec2 min{cx, cy - text_size.y - 10.0f};
            const ImVec2 display = ImGui::GetIO().DisplaySize;
            if (min.y < 0.0f) min.y = cy + line_height + 4.0f;
            min.x = std::clamp(min.x, 0.0f, std::max(0.0f, display.x - text_size.x - 16.0f));
            const ImVec2 max{min.x + text_size.x + 16.0f, min.y + text_size.y + 8.0f};
            fg->AddRectFilled(min, max, IM_COL32(37, 37, 42, 245), 4.0f);
            fg->AddRect(min, max, IM_COL32(70, 90, 120, 255), 4.0f);
            fg->AddText(ImVec2(min.x + 8, min.y + 4), IM_COL32(200, 205, 215, 255), line.c_str());
            if (sig.parameter_start >= 0 && sig.parameter_end > sig.parameter_start &&
                sig.parameter_end <= static_cast<int>(sig.label.size())) {
                const float x0 = ImGui::CalcTextSize(sig.label.c_str(), sig.label.c_str() + sig.parameter_start).x;
                fg->AddText(ImVec2(min.x + 8 + x0, min.y + 4), IM_COL32(255, 200, 90, 255), sig.label.c_str() + sig.parameter_start,
                            sig.label.c_str() + sig.parameter_end);
            }
        } else if (!glsl && !cpp) {
            // Firma de la funcion que se esta llamando, con el argumento actual.
            LuaSignature signature;
            if (luaSignatureAt(tab.text, static_cast<std::size_t>(state.cursor), signature)) {
                ImDrawList* fg = ImGui::GetForegroundDrawList();
                std::string line = signature.label;
                if (!signature.detail.empty()) line += "   " + signature.detail;
                const ImVec2 text_size = ImGui::CalcTextSize(line.c_str());
                ImVec2 min{cx, cy - text_size.y - 10.0f};
                const ImVec2 display = ImGui::GetIO().DisplaySize;
                if (min.y < 0.0f) min.y = cy + line_height + 4.0f;
                min.x = std::clamp(min.x, 0.0f, std::max(0.0f, display.x - text_size.x - 16.0f));
                const ImVec2 max{min.x + text_size.x + 16.0f, min.y + text_size.y + 8.0f};
                fg->AddRectFilled(min, max, IM_COL32(37, 37, 42, 245), 4.0f);
                fg->AddRect(min, max, IM_COL32(70, 90, 120, 255), 4.0f);
                // El argumento que se esta escribiendo, resaltado.
                const std::size_t open = signature.label.find('(');
                std::size_t arg_start = open == std::string::npos ? 0 : open + 1;
                for (int k = 0; k < signature.argument && arg_start < signature.label.size(); ++k) {
                    const std::size_t comma = signature.label.find(',', arg_start);
                    if (comma == std::string::npos) {
                        arg_start = std::string::npos;
                        break;
                    }
                    arg_start = comma + 1;
                }
                fg->AddText(ImVec2(min.x + 8, min.y + 4), IM_COL32(200, 205, 215, 255), line.c_str());
                if (open != std::string::npos && arg_start != std::string::npos && arg_start < signature.label.size()) {
                    std::size_t arg_end = signature.label.find_first_of(",)", arg_start);
                    if (arg_end == std::string::npos) arg_end = signature.label.size();
                    const float x0 = ImGui::CalcTextSize(signature.label.c_str(), signature.label.c_str() + arg_start).x;
                    fg->AddText(ImVec2(min.x + 8 + x0, min.y + 4), IM_COL32(255, 200, 90, 255),
                                signature.label.c_str() + arg_start, signature.label.c_str() + arg_end);
                }
            }
        }
        // Que el cursor no se salga de la vista.
        if (cy < origin.y + scroll_y - padding.y) ImGui::SetScrollY(static_cast<float>(line) * line_height);
        if (cy + line_height > origin.y + scroll_y + avail.y) {
            ImGui::SetScrollY(static_cast<float>(line + 1) * line_height - avail.y + padding.y * 2.0f);
        }
    }
    if (tab.goto_line > 0) {
        ImGui::SetScrollY(std::max(0.0f, static_cast<float>(tab.goto_line - 5) * line_height));
        tab.goto_line = -1;
    }
    if (cpp && tab.text != tab.clangd_synced && ImGui::GetTime() - tab.last_edit > 0.3) syncClangd(tab);
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // Barra de estado.
    if (!error_message.empty()) {
        // Una sola linea (recortada): si crece, todo lo de debajo salta al teclear.
        std::string shown = error_message.substr(0, error_message.find('\n'));
        const float room = ImGui::GetContentRegionAvail().x * (cpp ? 0.6f : 1.0f);
        if (ImGui::CalcTextSize(shown.c_str()).x > room) {
            while (!shown.empty() && ImGui::CalcTextSize((shown + "...").c_str()).x > room) shown.pop_back();
            shown += "...";
        }
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 110, 110, 255));
        ImGui::TextUnformatted(shown.c_str());
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered() && shown != error_message) ImGui::SetTooltip("%s", error_message.c_str());
        if (ImGui::IsItemClicked() && error_line > 0) tab.goto_line = error_line;
    } else {
        ImGui::TextDisabled("Linea %d, columna %d   |   %s%s   |   Ctrl+S guarda%s", tab.line, tab.column,
                            tab.relative.c_str(), tab.text != tab.saved ? " *" : "",
                            playing() ? " y recarga en el juego" : "");
    }
    if (cpp) {
        // IntelliSense: si clangd no esta, se dice (y por que).
        int errors = 0, warnings = 0;
        for (const ClangdDiagnostic& d : diagnostics) (d.severity == 1 ? errors : warnings) += d.severity <= 2 ? 1 : 0;
        const bool ready = clangd_.running();
        ImGui::SameLine();
        ImGui::TextColored(ready ? ImVec4(0.45f, 0.8f, 0.5f, 1.0f) : ImVec4(1.0f, 0.55f, 0.4f, 1.0f), "   |   IntelliSense: %s",
                           ready ? (std::to_string(errors) + " errores, " + std::to_string(warnings) + " avisos  (Ctrl+Espacio, F12)").c_str()
                                 : (clangd_.status().empty() ? std::string("sin clangd") : clangd_.status()).c_str());
    }
    if (active && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) saveScript(tab);
    if (cpp && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        if (io.KeyShift && io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_F, false)) formatCppTab(tab);
        if (io.KeyAlt && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_O, false)) {
            const std::filesystem::path other = cppCounterpart(tab.path);
            if (!other.empty()) openScript(other);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F7, false)) {
            if (tab.text != tab.saved) saveScript(tab);
            cpp_last_check_ = -10.0;
        }
    }
}

// Cada script abierto es su propia ventana (como las pestanas de un IDE): se
// abren acopladas juntas, y arrastrandolas se ven varias lado a lado o se
// sacan a otro monitor. Sin scripts abiertos queda la ventana "Scripts" con
// Nuevo y la consola.
void EditorApp::drawScriptToolbar(ScriptTab* tab) {
    if (ImGui::Button("Nuevo script C++")) createCppScriptAsset(current_folder_.empty() ? std::filesystem::path{} : current_folder_, {});
    ImGui::SameLine();
    ImGui::BeginDisabled(tab == nullptr);
    if (ImGui::Button("Guardar (Ctrl+S)") && tab != nullptr) saveScript(*tab);
    ImGui::SameLine();
    if (ImGui::Button("Descartar cambios") && tab != nullptr) {
        tab->text = readAll(tab->path);
        tab->saved = tab->text;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled(playing() ? "En Play: guardar recarga el script sin perder el estado" : "");
}

// Consola de Lua: una linea que se ejecuta (en Play, dentro del juego).
void EditorApp::drawLuaConsole() {
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputTextWithHint("##lua_console", "Lua> o CVar (nombre valor, cvars filtro). Enter ejecuta", &lua_console_,
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
        // Primero las CVars ("nombre valor", "cvars filtro", "reset nombre"); si no, Lua.
        bool handled = false;
        const std::string cvar_output = cvar::Registry::instance().execute(lua_console_, &handled);
        if (handled) {
            std::cout << "[CVar] > " << lua_console_ << "\n" << cvar_output << "\n";
        } else {
            std::string output;
            const bool ok = scripts_.run(lua_console_, &output, &world_);
            (ok ? std::cout : std::cerr) << "[Lua] > " << lua_console_ << (output.empty() ? "" : "  ->  ") << output << "\n";
        }
        lua_console_.clear();
        ImGui::SetKeyboardFocusHere(-1);
    }
}

void EditorApp::drawScriptEditor() {
    const ImGuiID default_dock = script_dock_id_ != 0 ? script_dock_id_ : scene_dock_id_;
    if (script_tabs_.empty()) {
        if (focus_script_editor_) {
            ImGui::SetNextWindowFocus();
            focus_script_editor_ = false;
        }
        if (default_dock != 0) ImGui::SetNextWindowDockID(default_dock, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(900.0f, 600.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Scripts", &show_script_editor_)) {
            drawScriptToolbar(nullptr);
            ImGui::Spacing();
            ImGui::TextDisabled("Sin scripts abiertos. Doble clic en un .lua del Proyecto, o Nuevo.");
            ImGui::TextDisabled("Cada script se abre en su ventana: arrastra su pestana para ver varios a la vez.");
            drawLuaConsole();
        }
        ImGui::End();
        return;
    }
    focus_script_editor_ = false;

    int close = -1;
    // En su pestana de espacio de trabajo: solo ese script, a toda la ventana.
    const std::filesystem::path only = activeScriptWorkspace();
    for (int i = 0; i < static_cast<int>(script_tabs_.size()); ++i) {
        ScriptTab& tab = script_tabs_[i];
        if (!only.empty() && tab.path != only) continue;
        if (!only.empty() && script_workspace_dock_ != 0) ImGui::SetNextWindowDockID(script_workspace_dock_, ImGuiCond_Always);
        // El titulo cambia (el * de sin guardar); el ID (### ruta) no.
        const std::string title = dialogs::utf8(tab.path.filename()) + (tab.text != tab.saved ? " *" : "") +
                                  "###script:" + tab.relative;
        if (tab.select) {
            // Recien abierto (o pedido otra vez): al frente, junto a los demas scripts.
            if (default_dock != 0) ImGui::SetNextWindowDockID(default_dock, ImGuiCond_Once);
            ImGui::SetNextWindowFocus();
            tab.select = false;
        }
        ImGui::SetNextWindowSize(ImVec2(900.0f, 600.0f), ImGuiCond_FirstUseEver);
        bool open = true;
        const ImGuiWindowFlags flags = tab.text != tab.saved ? ImGuiWindowFlags_UnsavedDocument : 0;
        const bool visible = ImGui::Begin(title.c_str(), &open, flags);
        if (ImGui::GetWindowDockID() != 0) script_dock_id_ = ImGui::GetWindowDockID();
        if (visible) {
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) active_script_tab_ = i;
            if (tab.cpp) {
                drawCppToolbar(tab);
                drawCodeEditor(tab);  // deja sitio debajo para la consola
                drawCppConsole(tab);
            } else {
                drawScriptToolbar(&tab);
                drawCodeEditor(tab);  // deja sitio debajo para la consola
                drawLuaConsole();
            }
        }
        ImGui::End();
        if (!open) close = i;
    }
    if (close >= 0) {
        // Cerrar con cambios: se guardan (como el resto del editor, sin perder nada).
        if (script_tabs_[close].text != script_tabs_[close].saved) saveScript(script_tabs_[close]);
        if (script_tabs_[close].cpp) clangd_.close(script_tabs_[close].path);
        script_tabs_.erase(script_tabs_.begin() + close);
        active_script_tab_ = std::min(active_script_tab_, static_cast<int>(script_tabs_.size()) - 1);
        if (script_tabs_.empty()) show_script_editor_ = false;
    }
}

// Inspector del componente Script: el archivo (soltar un .lua), editar, crear
// y las propiedades que declara el script con su control.
void EditorApp::drawScriptInspector(ecs::Entity entity) {
    scripting::Script* script = entity.tryGet<scripting::Script>();
    if (script == nullptr) return;
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
    ImGui::Button(script->file.empty() ? "Suelta aqui un script (.lua)" : script->file.c_str(), ImVec2(w, 0.0f));
    ImGui::PopStyleColor();
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kScriptPayload)) {
            script->file = assetRelative(dialogs::fromUtf8(static_cast<const char*>(payload->Data)));
            commit();
        }
        ImGui::EndDragDropTarget();
    }
    const float half = (w - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    ImGui::BeginDisabled(script->file.empty());
    if (ImGui::Button("Editar", ImVec2(half, 0.0f))) openScript(project_.assetsFolder() / dialogs::fromUtf8(script->file));
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Nuevo script", ImVec2(half, 0.0f))) createScriptAsset({}, entity);

    if (script->file.empty()) return;
    std::string error;
    const std::vector<scripting::ScriptProperty> declared = scripts_.describe(script->file, &error);
    if (!error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 110, 110, 255));
        ImGui::TextWrapped("%s", error.c_str());
        ImGui::PopStyleColor();
    }
    bool edited = false;
    for (const scripting::ScriptProperty& decl : declared) {
        // El valor del Inspector (si se cambio) o el del script.
        auto it = std::find_if(script->properties.begin(), script->properties.end(),
                               [&](const scripting::ScriptProperty& p) { return p.name == decl.name; });
        scripting::ScriptProperty current = it != script->properties.end() && it->type == decl.type ? *it : decl;
        bool changed = false;
        ImGui::PushID(decl.name.c_str());
        ImGui::SetNextItemWidth(-110.0f);
        switch (decl.type) {
            case scripting::PropertyType::Number: {
                float v = std::strtof(current.value.c_str(), nullptr);
                if (ImGui::DragFloat(decl.name.c_str(), &v, 0.05f)) {
                    char buffer[64];
                    std::snprintf(buffer, sizeof(buffer), "%g", v);
                    current.value = buffer;
                    changed = true;
                }
                break;
            }
            case scripting::PropertyType::Bool: {
                bool v = current.value == "true";
                if (ImGui::Checkbox(decl.name.c_str(), &v)) {
                    current.value = v ? "true" : "false";
                    changed = true;
                }
                break;
            }
            case scripting::PropertyType::Text:
                changed = ImGui::InputText(decl.name.c_str(), &current.value);
                break;
            case scripting::PropertyType::Vector: {
                float v[3] = {0.0f, 0.0f, 0.0f};
                std::sscanf(current.value.c_str(), "%f %f %f", &v[0], &v[1], &v[2]);
                if (ImGui::DragFloat3(decl.name.c_str(), v, 0.05f)) {
                    char buffer[96];
                    std::snprintf(buffer, sizeof(buffer), "%g %g %g", v[0], v[1], v[2]);
                    current.value = buffer;
                    changed = true;
                }
                break;
            }
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) edited = true;
        ImGui::PopID();
        if (changed) {
            if (it != script->properties.end()) {
                *it = current;
            } else {
                script->properties.push_back(current);
            }
            if (decl.type == scripting::PropertyType::Bool) edited = true;
        }
    }
    if (declared.empty() && error.empty()) {
        ImGui::TextDisabled("El script no declara 'properties' editables.");
    }
    if (edited) commit();
    for (const scripting::ScriptError& e : scripts_.errors()) {
        if (e.file != script->file) continue;
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 110, 110, 255));
        ImGui::TextWrapped("%s", e.message.c_str());
        ImGui::PopStyleColor();
    }
}

// Inspector del AudioSource: soltar un clip y escucharlo sin Play.
void EditorApp::drawAudioInspector(ecs::Entity entity) {
    audio::AudioSource* source = entity.tryGet<audio::AudioSource>();
    if (source == nullptr) return;
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
    ImGui::Button(source->clip.empty() ? "Suelta aqui un audio (WAV, MP3, OGG, FLAC)" : source->clip.c_str(),
                  ImVec2(w - 70.0f, 0.0f));
    ImGui::PopStyleColor();
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kAudioPayload)) {
            source->clip = assetRelative(dialogs::fromUtf8(static_cast<const char*>(payload->Data)));
            commit();
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::SameLine();
    const bool previewing = audio_.previewing();
    ImGui::BeginDisabled(source->clip.empty());
    if (ImGui::Button(previewing ? "Parar" : "Oir", ImVec2(-1.0f, 0.0f))) {
        if (previewing) {
            audio_.stopPreview();
        } else {
            audio_.preview(project_.assetsFolder() / dialogs::fromUtf8(source->clip));
        }
    }
    ImGui::EndDisabled();
    if (!audio_.available()) ImGui::TextDisabled("Sin dispositivo de audio.");
    // En Play: si una pared lo tapa ahora (oclusion del Audio Listener).
    if (const float walls = audio_.occlusionOf(entity); walls >= 0.0f) {
        if (walls > 0.05f) {
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.4f, 1.0f), "Tapado: %.1f pared(es)", walls);
        } else {
            ImGui::TextDisabled("Se oye directo (sin paredes en medio)");
        }
    }
}

}  // namespace cramion::editor

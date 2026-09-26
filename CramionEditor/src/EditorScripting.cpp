// Scripting en el editor: el editor de scripts Lua integrado (pestanas,
// resaltado de sintaxis, numeros de linea, sangria automatica, Ctrl+S guarda
// y recarga en caliente, errores marcados en su linea), la consola de Lua, el
// Inspector del componente Script (propiedades del script con su control) y
// el del AudioSource (arrastrar un clip, escucharlo).

#include "EditorApp.h"

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

bool isApi(std::string_view w) {
    static constexpr const char* kNames[] = {"self",  "Vec3",  "Entity", "Scene",  "Input", "Time",  "Physics",
                                             "Audio", "Debug", "Mathf",  "print",  "math",  "string", "table",
                                             "pairs", "ipairs", "tostring", "tonumber", "type", "setmetatable"};
    for (const char* k : kNames) {
        if (w == k) return true;
    }
    return false;
}

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

// Tokens de una linea. `in_block_comment` sigue de una linea a la siguiente.
// `glsl`: comentarios // y /* */ y las palabras de GLSL.
void tokenizeLine(std::string_view line, bool& in_block_comment,
                  std::vector<std::pair<std::string_view, Token>>& out, bool glsl = false) {
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
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            std::size_t j = i;
            while (j < line.size() && (std::isalnum(static_cast<unsigned char>(line[j])) || line[j] == '_')) ++j;
            const std::string_view word = line.substr(i, j - i);
            Token t = Token::Text;
            if (glsl ? isGlslKeyword(word) : isKeyword(word)) {
                t = Token::Keyword;
            } else if (glsl && isGlslApi(word)) {
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
};

int editCallback(ImGuiInputTextCallbackData* data) {
    auto* state = static_cast<EditState*>(data->UserData);
    if (data->EventFlag == ImGuiInputTextFlags_CallbackCharFilter) {
        state->typed = true;
        state->last_char = static_cast<char>(data->EventChar);
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
            if (ends("then") || ends("do") || ends("{") || ends("else") || ends("repeat") ||
                previous.find("function") != std::string::npos && ends(")")) {
                indent += 4;
            }
            if (indent > 0) data->InsertChars(data->CursorPos, std::string(static_cast<std::size_t>(indent), ' ').c_str());
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
    script_tabs_.push_back(std::move(tab));
    active_script_tab_ = static_cast<int>(script_tabs_.size()) - 1;
}

bool EditorApp::saveScript(ScriptTab& tab) {
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
    // ventana que lo contiene, asi el resaltado cae encima exacto).
    std::vector<std::string_view> lines;
    {
        std::string_view all(tab.text);
        std::size_t start = 0;
        while (true) {
            const std::size_t end = all.find('\n', start);
            lines.push_back(all.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
            if (end == std::string_view::npos) break;
            start = end + 1;
        }
    }
    float widest = 0.0f;
    for (const std::string_view line : lines) {
        widest = std::max(widest, font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, line.data(), line.data() + line.size()).x);
    }

    // Errores de este archivo.
    int error_line = -1;
    std::string error_message;
    for (const scripting::ScriptError& error : scripts_.errors()) {
        if (error.file == tab.relative) {
            error_line = error.line;
            error_message = error.message;
        }
    }
    // Shader de superficie: el ultimo error al compilarlo ("Nombre.crshader:12: error: ...").
    const bool glsl = tab.path.extension() == assets::kSurfaceShaderExtension;
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
    ImGui::BeginChild("##code", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing() * 2.2f), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float scroll_y = ImGui::GetScrollY();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const int first_visible = std::max(0, static_cast<int>(scroll_y / line_height) - 1);
    const int last_visible = std::min(static_cast<int>(lines.size()), static_cast<int>((scroll_y + avail.y) / line_height) + 2);

    // Numeros de linea.
    for (int i = first_visible; i < last_visible; ++i) {
        char number[16];
        std::snprintf(number, sizeof(number), "%d", i + 1);
        const float y = origin.y + padding.y + static_cast<float>(i) * line_height;
        const bool is_error = i + 1 == error_line;
        draw->AddText(ImVec2(origin.x + gutter - 10.0f - ImGui::CalcTextSize(number).x, y),
                      is_error ? IM_COL32(255, 90, 90, 255) : IM_COL32(110, 118, 129, 255), number);
    }

    // El texto editable (invisible) ...
    ImGui::SetCursorScreenPos(ImVec2(origin.x + gutter, origin.y));
    const ImVec2 size{std::max(widest + font_size * 4.0f, avail.x - gutter),
                      std::max(static_cast<float>(lines.size() + 2) * line_height + padding.y * 2.0f, avail.y)};
    EditState state;
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
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, IM_COL32(38, 79, 120, 255));
    ImGui::InputTextMultiline("##source", &tab.text, size,
                              ImGuiInputTextFlags_AllowTabInput | ImGuiInputTextFlags_CallbackCharFilter |
                                  ImGuiInputTextFlags_CallbackAlways | ImGuiInputTextFlags_NoHorizontalScroll,
                              editCallback, &state);
    const bool active = ImGui::IsItemActive();
    tab.was_active = active;
    const ImVec2 text_origin{ImGui::GetItemRectMin().x + padding.x, ImGui::GetItemRectMin().y + padding.y};
    if (active) {
        const bool typed_ident = state.typed && accepted.empty() &&
                                 (std::isalnum(static_cast<unsigned char>(state.last_char)) || state.last_char == '_' ||
                                  state.last_char == '.' || state.last_char == ':');
        const bool forced = io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Space, false);
        const bool edited_while_open = tab.completion_open && state.cursor != tab.cursor;
        if (!glsl && (typed_ident || forced || edited_while_open)) {
            LuaCompletionContext context;
            if (luaCompletionContext(tab.text, static_cast<std::size_t>(state.cursor), context) &&
                (forced || !context.prefix.empty() || context.accessor != 0)) {
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
        ImGui::SetKeyboardFocusHere(-1);
        tab.focus = false;
    }

    // ... y encima, con color.
    if (error_line >= 1) {
        const float y = text_origin.y + static_cast<float>(error_line - 1) * line_height;
        draw->AddRectFilled(ImVec2(origin.x, y), ImVec2(origin.x + size.x + gutter, y + line_height), IM_COL32(120, 30, 30, 110));
    }
    bool block_comment = false;
    std::vector<std::pair<std::string_view, Token>> tokens;
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        tokenizeLine(lines[i], block_comment, tokens, glsl);  // (las anteriores cuentan para --[[ ]] y /* */)
        if (i < first_visible || i >= last_visible) continue;
        float x = text_origin.x;
        const float y = text_origin.y + static_cast<float>(i) * line_height;
        for (const auto& [text, token] : tokens) {
            draw->AddText(font, font_size, ImVec2(x, y), tokenColor(token), text.data(), text.data() + text.size());
            x += font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, text.data(), text.data() + text.size()).x;
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
        if (std::fmod(ImGui::GetTime(), 1.0) < 0.6) {
            draw->AddLine(ImVec2(cx, cy), ImVec2(cx, cy + line_height), IM_COL32(230, 230, 230, 255), 1.5f);
        }
        if (tab.completion_open && !tab.completions.empty()) {
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            const int count = static_cast<int>(tab.completions.size());
            const int visible = std::min(count, 9);
            const int first = std::clamp(tab.completion_selected - visible + 1, 0, std::max(count - visible, 0));
            const float row = line_height + 4.0f;
            const float width = 420.0f;
            const std::string& detail = tab.completions[tab.completion_selected].detail;
            const float detail_h = detail.empty() ? 0.0f : row + 4.0f;
            ImVec2 min{cx - ImGui::CalcTextSize(tab.completion_context.prefix.c_str()).x, cy + line_height + 2.0f};
            const ImVec2 display = ImGui::GetIO().DisplaySize;
            if (min.y + row * visible + detail_h > display.y) min.y = cy - row * visible - detail_h - 2.0f;
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
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // Barra de estado.
    if (!error_message.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 110, 110, 255));
        ImGui::TextWrapped("%s", error_message.c_str());
        ImGui::PopStyleColor();
        if (ImGui::IsItemClicked() && error_line > 0) tab.goto_line = error_line;
    } else {
        ImGui::TextDisabled("Linea %d, columna %d   |   %s%s   |   Ctrl+S guarda%s", tab.line, tab.column,
                            tab.relative.c_str(), tab.text != tab.saved ? " *" : "",
                            playing() ? " y recarga en el juego" : "");
    }
    if (active && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) saveScript(tab);
}

// Cada script abierto es su propia ventana (como las pestanas de un IDE): se
// abren acopladas juntas, y arrastrandolas se ven varias lado a lado o se
// sacan a otro monitor. Sin scripts abiertos queda la ventana "Scripts" con
// Nuevo y la consola.
void EditorApp::drawScriptToolbar(ScriptTab* tab) {
    if (ImGui::Button("Nuevo")) createScriptAsset(current_folder_.empty() ? std::filesystem::path{} : current_folder_, {});
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
    if (ImGui::InputTextWithHint("##lua_console", "Lua> (Enter ejecuta; en Play, dentro del juego)", &lua_console_,
                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
        std::string output;
        const bool ok = scripts_.run(lua_console_, &output);
        (ok ? std::cout : std::cerr) << "[Lua] > " << lua_console_ << (output.empty() ? "" : "  ->  ") << output << "\n";
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
    for (int i = 0; i < static_cast<int>(script_tabs_.size()); ++i) {
        ScriptTab& tab = script_tabs_[i];
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
            drawScriptToolbar(&tab);
            drawCodeEditor(tab);  // deja sitio debajo para la consola
            drawLuaConsole();
        }
        ImGui::End();
        if (!open) close = i;
    }
    if (close >= 0) {
        // Cerrar con cambios: se guardan (como el resto del editor, sin perder nada).
        if (script_tabs_[close].text != script_tabs_[close].saved) saveScript(script_tabs_[close]);
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
}

}  // namespace cramion::editor

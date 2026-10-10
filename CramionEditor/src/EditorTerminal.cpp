// Terminal integrada (Ventana > Terminal (IA)): consolas de Windows dentro del
// editor, con pestanas, para usar una IA de linea de comandos sin salir del
// motor. "Claude Code" la abre ya conectada al servidor MCP del editor (ve y
// cambia la escena, crea scripts, da Play...).

#include "EditorApp.h"

#include "Dialogs.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace cramion::editor {

namespace {

constexpr std::uint32_t kTermBackground = 0x0C0C0C;
constexpr std::uint32_t kTermForeground = 0xCCCCCC;

ImU32 rgb(std::uint32_t c, float alpha = 1.0f) {
    return IM_COL32((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, static_cast<int>(alpha * 255.0f));
}

std::uint32_t resolve(TermColor color, bool foreground, bool bold) {
    if (color == kTermDefault) return foreground ? (bold ? 0xF2F2F2 : kTermForeground) : kTermBackground;
    if ((color >> 24) == 1) {
        int index = static_cast<int>(color & 0xFF);
        if (foreground && bold && index < 8) index += 8;
        return TerminalScreen::paletteColor(index);
    }
    return color & 0xFFFFFF;
}

void appendUtf8(std::string& out, unsigned int c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

// El PATH del registro (usuario + sistema): lo que se instalo con el editor
// abierto (Claude Code, Node) aparece sin reiniciarlo.
constexpr const char* kRefreshPath =
    "$env:Path = [Environment]::GetEnvironmentVariable('Path','User') + ';' + "
    "[Environment]::GetEnvironmentVariable('Path','Machine'); ";

std::wstring widen(const std::string& utf8) {
    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(std::max(size, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), size);
    return out;
}

}  // namespace

void EditorApp::openTerminal(TerminalKind kind) {
    auto session = std::make_unique<TerminalSession>();
    std::string command;
    switch (kind) {
        case TerminalKind::PowerShell:
            session->name = "PowerShell";
            command = std::string("powershell.exe -NoLogo -NoExit -Command \"") + kRefreshPath + "\"";
            break;
        case TerminalKind::Cmd:
            session->name = "CMD";
            command = "cmd.exe";
            break;
        case TerminalKind::ClaudeCode: {
            session->name = "Claude Code";
            // Conectada al editor: el servidor MCP encendido y su direccion en
            // un --mcp-config (no toca la configuracion global de Claude).
            if (!mcp_.running()) startMcp();
            std::error_code e;
            const std::filesystem::path folder =
                has_project_ ? project_.libraryFolder() : std::filesystem::temp_directory_path() / "Cramion";
            std::filesystem::create_directories(folder, e);
            const std::filesystem::path config = folder / "claude_mcp.json";
            {
                std::ofstream out(config, std::ios::binary | std::ios::trunc);
                out << "{\n  \"mcpServers\": {\n    \"cramion\": {\n      \"type\": \"http\",\n      \"url\": "
                       "\"http://127.0.0.1:"
                    << mcp_.port() << "/mcp\"\n    }\n  }\n}\n";
            }
            const std::string prompt =
                "Estas dentro del editor del motor de juegos Cramion. Usa las herramientas MCP del servidor cramion "
                "(empieza por la herramienta help) para ver y cambiar la escena, crear objetos, scripts Lua, "
                "materiales y shaders, dar Play y hacer capturas. Responde en el idioma del usuario.";
            command = std::string("powershell.exe -NoLogo -NoExit -Command \"") + kRefreshPath +
                      "if (Get-Command claude -ErrorAction SilentlyContinue) { claude --mcp-config '" +
                      dialogs::utf8(config) + "' --append-system-prompt '" + prompt +
                      "' } else { Write-Host 'Claude Code no esta instalado.' -ForegroundColor Yellow; "
                      "Write-Host 'Instalalo desde el menu Mas > Instalar Claude Code (o: irm https://claude.ai/install.ps1 | iex)' }\"";
            break;
        }
        case TerminalKind::Codex:
            session->name = "Codex";
            command = std::string("powershell.exe -NoLogo -NoExit -Command \"") + kRefreshPath +
                      "if (Get-Command codex -ErrorAction SilentlyContinue) { codex } else { Write-Host 'Codex no esta "
                      "instalado (npm install -g @openai/codex)' -ForegroundColor Yellow }\"";
            break;
        case TerminalKind::Gemini:
            session->name = "Gemini";
            command = std::string("powershell.exe -NoLogo -NoExit -Command \"") + kRefreshPath +
                      "if (Get-Command gemini -ErrorAction SilentlyContinue) { gemini } else { Write-Host 'Gemini CLI no "
                      "esta instalado (npm install -g @google/gemini-cli)' -ForegroundColor Yellow }\"";
            break;
        case TerminalKind::InstallClaude:
            session->name = "Instalar Claude Code";
            command = "powershell.exe -NoLogo -NoExit -ExecutionPolicy Bypass -Command \"irm https://claude.ai/install.ps1 | iex\"";
            break;
    }
    const std::filesystem::path folder = has_project_ ? project_.folder : std::filesystem::path{};
    std::string error;
    if (!session->start(widen(command), folder, terminal_cols_, terminal_rows_, error)) {
        terminal_error_ = error;
        return;
    }
    terminal_error_.clear();
    terminals_.push_back(std::move(session));
    terminal_select_ = static_cast<int>(terminals_.size()) - 1;
    show_terminal_ = true;
}

void EditorApp::drawTerminalWindow() {
    // La salida se lee aunque el panel este cerrado (no se acumula).
    for (auto& session : terminals_) session->pump();
    // CRAMION_OPEN_TERMINAL=claude|powershell: abrirla al tener un proyecto
    // (pruebas y accesos directos).
    static bool env_checked = false;
    if (!env_checked && has_project_) {
        env_checked = true;
        if (const char* open = std::getenv("CRAMION_OPEN_TERMINAL")) {
            openTerminal(std::string(open) == "claude" ? TerminalKind::ClaudeCode : TerminalKind::PowerShell);
        }
    }
    if (!show_terminal_) return;

    ImGui::SetNextWindowSize(ImVec2(900.0f, 520.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Terminal (IA)", &show_terminal_, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End();
        return;
    }

    if (ImGui::Button("Claude Code")) openTerminal(TerminalKind::ClaudeCode);
    ImGui::SetItemTooltip("Claude Code conectado al editor por MCP: ve y cambia la escena, crea scripts, da Play...");
    ImGui::SameLine();
    if (ImGui::Button("PowerShell")) openTerminal(TerminalKind::PowerShell);
    ImGui::SameLine();
    if (ImGui::Button("Más...")) ImGui::OpenPopup("##terminal_more");
    if (ImGui::BeginPopup("##terminal_more")) {
        if (ImGui::MenuItem("CMD")) openTerminal(TerminalKind::Cmd);
        if (ImGui::MenuItem("Codex (OpenAI)")) openTerminal(TerminalKind::Codex);
        if (ImGui::MenuItem("Gemini CLI (Google)")) openTerminal(TerminalKind::Gemini);
        ImGui::Separator();
        if (ImGui::MenuItem("Instalar Claude Code")) openTerminal(TerminalKind::InstallClaude);
        if (ImGui::MenuItem("Conectar otra IA (MCP)...")) show_mcp_ = true;
        ImGui::EndPopup();
    }
    if (!terminal_error_.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "%s", terminal_error_.c_str());
    }

    if (terminals_.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("Abre una terminal para trabajar con una IA sin salir del motor. \"Claude Code\" se abre en "
                           "la carpeta del proyecto y conectado al editor: le puedes pedir que cree la escena, "
                           "escriba scripts o pruebe el juego.");
        ImGui::End();
        return;
    }

    int close_index = -1;
    TerminalSession* active = nullptr;
    if (ImGui::BeginTabBar("##terminals", ImGuiTabBarFlags_Reorderable | ImGuiTabBarFlags_AutoSelectNewTabs |
                                               ImGuiTabBarFlags_FittingPolicyScroll)) {
        for (int i = 0; i < static_cast<int>(terminals_.size()); ++i) {
            TerminalSession& session = *terminals_[i];
            bool open = true;
            const std::string label = session.name + (session.running() ? "" : " (terminada)") + "###term" +
                                      std::to_string(reinterpret_cast<std::uintptr_t>(&session));
            const ImGuiTabItemFlags flags = terminal_select_ == i ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(label.c_str(), &open, flags)) {
                active = &session;
                ImGui::EndTabItem();
            }
            if (!open) close_index = i;
        }
        terminal_select_ = -1;
        ImGui::EndTabBar();
    }
    if (active != nullptr) drawTerminalSession(*active);
    if (close_index >= 0) terminals_.erase(terminals_.begin() + close_index);
    ImGui::End();
}

void EditorApp::drawTerminalSession(TerminalSession& session) {
    ImFont* mono = imgui_.monoFont();
    if (mono != nullptr) ImGui::PushFont(mono, 0.0f);
    // El avance real de la fuente (monoespaciada) a este tamano.
    const float cell_w = ImGui::GetFontBaked()->GetCharAdvance(static_cast<ImWchar>('M'));
    const float cell_h = ImGui::GetTextLineHeight();
    const float font_size = ImGui::GetFontSize();
    ImFont* font = ImGui::GetFont();

    ImGui::PushStyleColor(ImGuiCol_ChildBg, rgb(kTermBackground));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f, 4.0f));
    ImGui::BeginChild("##terminal_view", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNavInputs);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const int cols = std::max(static_cast<int>(avail.x / cell_w), 10);
    const int rows = std::max(static_cast<int>(avail.y / cell_h), 3);
    terminal_cols_ = cols;
    terminal_rows_ = rows;
    session.resize(cols, rows);
    TerminalScreen& screen = session.screen();

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##terminal_input", ImVec2(cols * cell_w, rows * cell_h));
    const bool hovered = ImGui::IsItemHovered();
    const bool focused = ImGui::IsWindowFocused();
    ImGuiIO& io = ImGui::GetIO();

    const int total = screen.totalLines();
    const int max_scroll = std::max(total - rows, 0);
    session.scroll_offset = std::clamp(session.scroll_offset, 0, max_scroll);
    const int first = total - rows - session.scroll_offset;

    // --- Raton: rueda (historial) y seleccion ---
    if (hovered && io.MouseWheel != 0.0f) {
        session.scroll_offset = std::clamp(session.scroll_offset + static_cast<int>(io.MouseWheel * 3.0f), 0, max_scroll);
    }
    const auto cell_at = [&](ImVec2 p, int& line, int& col) {
        col = std::clamp(static_cast<int>((p.x - origin.x) / cell_w), 0, cols - 1);
        line = std::clamp(first + static_cast<int>((p.y - origin.y) / cell_h), 0, total - 1);
    };
    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        cell_at(io.MousePos, session.sel_start_line, session.sel_start_col);
        session.sel_end_line = session.sel_start_line;
        session.sel_end_col = session.sel_start_col;
        session.selecting = true;
        session.has_selection = false;
    }
    if (session.selecting) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            cell_at(io.MousePos, session.sel_end_line, session.sel_end_col);
            session.has_selection = session.sel_end_line != session.sel_start_line ||
                                    session.sel_end_col != session.sel_start_col;
            // Arrastrar por encima o por debajo mueve el historial.
            if (io.MousePos.y < origin.y) session.scroll_offset = std::min(session.scroll_offset + 1, max_scroll);
            if (io.MousePos.y > origin.y + rows * cell_h) session.scroll_offset = std::max(session.scroll_offset - 1, 0);
        } else {
            session.selecting = false;
        }
    }

    const auto paste = [&]() {
        const char* clip = ImGui::GetClipboardText();
        if (clip == nullptr || *clip == 0) return;
        std::string text;
        for (const char* c = clip; *c != 0; ++c) {
            if (*c == '\r') continue;
            text += *c == '\n' ? '\r' : *c;
        }
        if (screen.bracketedPaste()) text = "\x1b[200~" + text + "\x1b[201~";
        session.write(text);
        session.scroll_offset = 0;
    };
    const auto copy = [&]() {
        const std::string text = session.selectedText();
        if (!text.empty()) ImGui::SetClipboardText(text.c_str());
    };

    if (ImGui::BeginPopupContextItem("##terminal_menu")) {
        if (ImGui::MenuItem("Copiar", "Ctrl+C", false, session.has_selection)) copy();
        if (ImGui::MenuItem("Pegar", "Ctrl+V")) paste();
        if (ImGui::MenuItem("Seleccionar todo")) {
            session.sel_start_line = 0;
            session.sel_start_col = 0;
            session.sel_end_line = total - 1;
            session.sel_end_col = cols - 1;
            session.has_selection = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Reiniciar")) {
            // La misma pestana con una PowerShell nueva.
            std::string error;
            session.start(L"powershell.exe -NoLogo", has_project_ ? project_.folder : std::filesystem::path{}, cols,
                          rows, error);
        }
        ImGui::EndPopup();
    }

    // --- Teclado ---
    if (focused) {
        // Los atajos del editor (Supr, W/E/R, Ctrl+S...) no se disparan.
        io.WantTextInput = true;
        std::string out;
        const bool ctrl = io.KeyCtrl;
        const bool alt = io.KeyAlt;
        const bool shift = io.KeyShift;
        // AltGr llega como Ctrl+Alt: son caracteres (@, #...), no atajos.
        const bool ctrl_only = ctrl && !alt;
        for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
            const unsigned int c = io.InputQueueCharacters[i];
            if (c < 0x20 || c == 0x7F) continue;
            appendUtf8(out, c);
        }
        const int mod = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0);
        const auto key = [&](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };
        const auto arrow = [&](char final_char) {
            if (mod > 1) return std::string("\x1b[1;") + std::to_string(mod) + final_char;
            return std::string(screen.applicationCursorKeys() ? "\x1bO" : "\x1b[") + final_char;
        };
        const auto tilde = [&](int code) {
            return "\x1b[" + std::to_string(code) + (mod > 1 ? ";" + std::to_string(mod) : std::string()) + "~";
        };
        if (key(ImGuiKey_Enter) || key(ImGuiKey_KeypadEnter)) out += (shift || alt) ? "\x1b\r" : "\r";
        if (key(ImGuiKey_Backspace)) out += ctrl ? "\x17" : "\x7f";
        if (key(ImGuiKey_Tab)) out += shift ? "\x1b[Z" : "\t";
        if (key(ImGuiKey_Escape)) out += "\x1b";
        if (key(ImGuiKey_UpArrow)) out += arrow('A');
        if (key(ImGuiKey_DownArrow)) out += arrow('B');
        if (key(ImGuiKey_RightArrow)) out += arrow('C');
        if (key(ImGuiKey_LeftArrow)) out += arrow('D');
        if (key(ImGuiKey_Home)) out += arrow('H');
        if (key(ImGuiKey_End)) out += arrow('F');
        if (key(ImGuiKey_Insert)) out += tilde(2);
        if (key(ImGuiKey_Delete)) out += tilde(3);
        if (shift && key(ImGuiKey_PageUp)) {
            session.scroll_offset = std::min(session.scroll_offset + rows / 2, max_scroll);
        } else if (key(ImGuiKey_PageUp)) {
            out += tilde(5);
        }
        if (shift && key(ImGuiKey_PageDown)) {
            session.scroll_offset = std::max(session.scroll_offset - rows / 2, 0);
        } else if (key(ImGuiKey_PageDown)) {
            out += tilde(6);
        }
        static constexpr const char* kF1to4[] = {"P", "Q", "R", "S"};
        static constexpr int kF5to12[] = {15, 17, 18, 19, 20, 21, 23, 24};
        for (int f = 0; f < 12; ++f) {
            if (!key(static_cast<ImGuiKey>(ImGuiKey_F1 + f))) continue;
            out += f < 4 ? std::string("\x1bO") + kF1to4[f] : tilde(kF5to12[f - 4]);
        }
        for (int k = ImGuiKey_A; k <= ImGuiKey_Z; ++k) {
            if (!ImGui::IsKeyPressed(static_cast<ImGuiKey>(k), true)) continue;
            const char letter = static_cast<char>('a' + (k - ImGuiKey_A));
            if (ctrl_only) {
                if (letter == 'c' && (shift || session.has_selection)) {
                    copy();
                    session.has_selection = false;
                } else if (letter == 'v') {
                    paste();
                } else if (!shift) {
                    out += static_cast<char>(letter - 'a' + 1);
                }
            } else if (alt && !ctrl) {
                out += '\x1b';
                out += shift ? static_cast<char>(letter - 32) : letter;
            }
        }
        if (!out.empty()) {
            session.write(out);
            session.scroll_offset = 0;
            session.has_selection = false;
        }
    }

    // --- Dibujo ---
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const auto selected = [&](int line, int col) {
        if (!session.has_selection) return false;
        int l0 = session.sel_start_line, c0 = session.sel_start_col, l1 = session.sel_end_line, c1 = session.sel_end_col;
        if (l1 < l0 || (l1 == l0 && c1 < c0)) {
            std::swap(l0, l1);
            std::swap(c0, c1);
        }
        if (line < l0 || line > l1) return false;
        if (line == l0 && col < c0) return false;
        if (line == l1 && col > c1) return false;
        return true;
    };
    for (int r = 0; r < rows; ++r) {
        const int index = first + r;
        if (index < 0 || index >= total) continue;
        const TerminalScreen::Line& line = screen.line(index);
        const float y = origin.y + r * cell_h;
        const int width = std::min(static_cast<int>(line.size()), cols);
        // Fondos (por tramos del mismo color).
        for (int x = 0; x < width;) {
            const TermCell& cell = line[x];
            const bool inverse = (cell.attrs & TerminalScreen::kInverse) != 0;
            const std::uint32_t bg = inverse ? resolve(cell.fg, true, false) : resolve(cell.bg, false, false);
            int end = x + 1;
            while (end < width) {
                const TermCell& next = line[end];
                const bool next_inverse = (next.attrs & TerminalScreen::kInverse) != 0;
                const std::uint32_t next_bg = next_inverse ? resolve(next.fg, true, false) : resolve(next.bg, false, false);
                if (next_bg != bg) break;
                ++end;
            }
            if (bg != kTermBackground) {
                draw->AddRectFilled(ImVec2(origin.x + x * cell_w, y), ImVec2(origin.x + end * cell_w, y + cell_h), rgb(bg));
            }
            x = end;
        }
        // Texto: cada caracter en su celda (con tramos, el avance de la
        // fuente y el de la rejilla no coincidian del todo y las palabras
        // quedaban apretadas con huecos dobles entre ellas).
        char glyph[5];
        for (int x = 0; x < width; ++x) {
            const TermCell& cell = line[x];
            if (cell.wide_tail) continue;
            const bool bold = (cell.attrs & TerminalScreen::kBold) != 0;
            const bool inverse = (cell.attrs & TerminalScreen::kInverse) != 0;
            std::uint32_t fg = inverse ? resolve(cell.bg, false, false) : resolve(cell.fg, true, bold);
            if (inverse && cell.bg == kTermDefault) fg = kTermBackground;
            const float alpha = (cell.attrs & TerminalScreen::kDim) != 0 ? 0.6f : 1.0f;
            const ImU32 color = rgb(fg, alpha);
            const float cx = origin.x + x * cell_w;
            if ((cell.attrs & TerminalScreen::kUnderline) != 0) {
                draw->AddLine(ImVec2(cx, y + cell_h - 1.0f), ImVec2(cx + cell_w, y + cell_h - 1.0f), color);
            }
            if ((cell.attrs & TerminalScreen::kStrike) != 0) {
                draw->AddLine(ImVec2(cx, y + cell_h * 0.5f), ImVec2(cx + cell_w, y + cell_h * 0.5f), color);
            }
            if (cell.ch == U' ') continue;
            std::string encoded;
            appendUtf8(encoded, static_cast<unsigned int>(cell.ch));
            std::memcpy(glyph, encoded.data(), encoded.size());
            draw->AddText(font, font_size, ImVec2(cx, y), color, glyph, glyph + encoded.size());
        }
        // Seleccion.
        if (session.has_selection) {
            for (int x = 0; x < cols; ++x) {
                if (selected(index, x)) {
                    draw->AddRectFilled(ImVec2(origin.x + x * cell_w, y), ImVec2(origin.x + (x + 1) * cell_w, y + cell_h),
                                        theme::withAlpha(theme::kAccent, 90));
                }
            }
        }
    }
    // Cursor (bloque que parpadea; con el panel sin foco, solo el contorno).
    if (session.running() && screen.cursorVisible() && session.scroll_offset == 0) {
        const ImVec2 a(origin.x + screen.cursorX() * cell_w, origin.y + screen.cursorY() * cell_h);
        const ImVec2 b(a.x + cell_w, a.y + cell_h);
        if (!focused) {
            draw->AddRect(a, b, IM_COL32(204, 204, 204, 160));
        } else if (std::fmod(ImGui::GetTime(), 1.0) < 0.6) {
            draw->AddRectFilled(a, b, IM_COL32(204, 204, 204, 170));
        }
    }
    if (session.scroll_offset > 0) {
        const std::string note = "Historial (-" + std::to_string(session.scroll_offset) + ")  Shift+RePag/AvPag";
        const ImVec2 size = ImGui::CalcTextSize(note.c_str());
        const ImVec2 p(origin.x + cols * cell_w - size.x - 8.0f, origin.y + 2.0f);
        draw->AddRectFilled(ImVec2(p.x - 4.0f, p.y), ImVec2(p.x + size.x + 4.0f, p.y + size.y), IM_COL32(40, 40, 40, 220));
        draw->AddText(p, IM_COL32(200, 200, 200, 255), note.c_str());
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    if (mono != nullptr) ImGui::PopFont();
}

}  // namespace cramion::editor

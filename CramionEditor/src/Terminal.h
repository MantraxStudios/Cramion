#ifndef CRAMION_EDITOR_TERMINAL_H
#define CRAMION_EDITOR_TERMINAL_H

// Terminal integrada del editor (Ventana > Terminal): una consola de Windows
// de verdad (ConPTY, Windows 10 1809+) con su emulador VT, para trabajar con
// una IA de linea de comandos (Claude Code, Codex, Gemini...) o con
// PowerShell sin salir del motor.
//
//   TerminalScreen   el emulador: interpreta la salida (UTF-8 + secuencias
//                    VT/ANSI: cursor, borrado, colores de 16/256/24 bits,
//                    region de scroll, pantalla alternativa) sobre una
//                    rejilla de celdas con historial.
//   TerminalSession  el proceso en su pseudoconsola: un hilo lee su salida,
//                    el editor la pasa a la pantalla cada frame y le escribe
//                    lo que se teclea.

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cramion::editor {

// Color de una celda: 0 = el de por defecto; 0x01000000 | indice (paleta de
// 256); 0x02000000 | RGB.
using TermColor = std::uint32_t;
inline constexpr TermColor kTermDefault = 0;

struct TermCell {
    char32_t ch = U' ';
    TermColor fg = kTermDefault;
    TermColor bg = kTermDefault;
    std::uint8_t attrs = 0;  // kBold | kDim | ...
    bool wide_tail = false;  // segunda mitad de un caracter ancho (CJK, emoji)
};

class TerminalScreen {
public:
    enum Attr : std::uint8_t { kBold = 1, kDim = 2, kItalic = 4, kUnderline = 8, kInverse = 16, kStrike = 32 };
    using Line = std::vector<TermCell>;

    TerminalScreen(int cols = 80, int rows = 24);

    void resize(int cols, int rows);
    // Salida del proceso (UTF-8 con secuencias VT). Lo que el terminal debe
    // contestar (posicion del cursor, atributos) se anade a `reply`.
    void feed(const char* data, std::size_t size, std::string& reply);

    int cols() const { return cols_; }
    int rows() const { return rows_; }
    // Lineas del historial (las que salieron por arriba) + las de la pantalla.
    int totalLines() const { return static_cast<int>(scrollback().size()) + rows_; }
    // Linea `index` de 0 (la mas antigua del historial) a totalLines() - 1.
    const Line& line(int index) const;
    int cursorX() const { return cursor_x_; }
    int cursorY() const { return cursor_y_; }
    bool cursorVisible() const { return cursor_visible_; }
    bool applicationCursorKeys() const { return app_cursor_keys_; }
    bool bracketedPaste() const { return bracketed_paste_; }
    bool alternateScreen() const { return alternate_; }
    const std::string& title() const { return title_; }
    // Cambia con cada salida (para seguir al final al llegar texto).
    std::uint64_t generation() const { return generation_; }

    static std::uint32_t paletteColor(int index);  // 0xRRGGBB

private:
    const std::deque<Line>& scrollback() const { return alternate_ ? empty_scrollback_ : scrollback_; }
    std::vector<Line>& screen() { return alternate_ ? alt_lines_ : lines_; }

    void print(char32_t ch);
    void control(char c);
    void escape(char c);
    void csi(char final_char);
    void osc();
    void sgr();
    void setMode(bool on);
    void lineFeed();
    void reverseIndex();
    void scrollUp(int top, int bottom, int count);
    void scrollDown(int top, int bottom, int count);
    void clearCells(Line& line, int from, int to);
    TermCell blank() const;
    void clampCursor();
    int param(std::size_t index, int fallback) const;

    int cols_ = 80;
    int rows_ = 24;
    std::vector<Line> lines_;
    std::vector<Line> alt_lines_;
    std::deque<Line> scrollback_;
    std::deque<Line> empty_scrollback_;
    static constexpr std::size_t kMaxScrollback = 5000;

    int cursor_x_ = 0;
    int cursor_y_ = 0;
    bool wrap_pending_ = false;
    int saved_x_ = 0;
    int saved_y_ = 0;
    TermColor fg_ = kTermDefault;
    TermColor bg_ = kTermDefault;
    std::uint8_t attrs_ = 0;
    int scroll_top_ = 0;
    int scroll_bottom_ = 23;
    bool cursor_visible_ = true;
    bool app_cursor_keys_ = false;
    bool bracketed_paste_ = false;
    bool autowrap_ = true;
    bool alternate_ = false;
    std::string title_;
    std::uint64_t generation_ = 0;

    // Parser
    enum class State { Ground, Escape, EscapeCharset, Csi, Osc, OscEscape, Ignore, IgnoreEscape };
    State state_ = State::Ground;
    std::vector<int> params_;
    bool param_started_ = false;
    char private_ = 0;
    char intermediate_ = 0;
    std::string osc_;
    std::string* reply_ = nullptr;
    // UTF-8 a medias
    char32_t utf8_code_ = 0;
    int utf8_left_ = 0;
};

class TerminalSession {
public:
    TerminalSession() = default;
    ~TerminalSession();
    TerminalSession(const TerminalSession&) = delete;
    TerminalSession& operator=(const TerminalSession&) = delete;

    // `command_line`: lo que se ejecuta (p. ej. "powershell.exe -NoLogo").
    bool start(const std::wstring& command_line, const std::filesystem::path& folder, int cols, int rows,
               std::string& error);
    void stop();
    bool running() const { return running_.load(); }
    bool exited() const { return started_ && !running_.load(); }

    // Pasa la salida pendiente al emulador (hilo del editor). true si hubo.
    bool pump();
    void write(const std::string& bytes);
    void resize(int cols, int rows);

    TerminalScreen& screen() { return screen_; }
    const TerminalScreen& screen() const { return screen_; }

    std::string name;  // pestana
    // Vista: lineas subidas desde el final (0 = siguiendo la salida) y seleccion.
    int scroll_offset = 0;
    bool selecting = false;
    bool has_selection = false;
    int sel_start_line = 0, sel_start_col = 0, sel_end_line = 0, sel_end_col = 0;
    std::string selectedText() const;

private:
    void readLoop();

    TerminalScreen screen_;
    void* pseudo_console_ = nullptr;  // HPCON
    void* input_write_ = nullptr;     // HANDLE
    void* output_read_ = nullptr;
    void* process_ = nullptr;
    std::thread reader_;
    std::mutex mutex_;
    std::string pending_;
    std::atomic<bool> running_{false};
    bool started_ = false;
};

}  // namespace cramion::editor

#endif  // CRAMION_EDITOR_TERMINAL_H

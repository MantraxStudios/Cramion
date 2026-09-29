#include "Terminal.h"

#include <algorithm>
#include <cstring>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace cramion::editor {

namespace {

// Columnas que ocupa un caracter: 0 las marcas que se combinan con el
// anterior, 2 los anchos (CJK, emoji), 1 el resto.
int charWidth(char32_t c) {
    if (c < 0x300) return 1;
    if ((c >= 0x300 && c <= 0x36F) || (c >= 0x200B && c <= 0x200F) || (c >= 0xFE00 && c <= 0xFE0F) ||
        (c >= 0x20D0 && c <= 0x20FF) || c == 0x200D) {
        return 0;
    }
    if ((c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0x303E) || (c >= 0x3041 && c <= 0x33FF) ||
        (c >= 0x3400 && c <= 0x4DBF) || (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xA000 && c <= 0xA4CF) ||
        (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE4F) ||
        (c >= 0xFF00 && c <= 0xFF60) || (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x1F300 && c <= 0x1F64F) ||
        (c >= 0x1F900 && c <= 0x1F9FF) || (c >= 0x1FA70 && c <= 0x1FAFF) || (c >= 0x20000 && c <= 0x3FFFD)) {
        return 2;
    }
    return 1;
}

void appendUtf8(std::string& out, char32_t c) {
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

}  // namespace

// =============================================================================
// TerminalScreen
// =============================================================================

TerminalScreen::TerminalScreen(int cols, int rows) {
    cols_ = std::max(cols, 2);
    rows_ = std::max(rows, 2);
    lines_.assign(rows_, Line(cols_));
    alt_lines_.assign(rows_, Line(cols_));
    scroll_bottom_ = rows_ - 1;
}

std::uint32_t TerminalScreen::paletteColor(int index) {
    // Los 16 de Windows Terminal (Campbell).
    static constexpr std::uint32_t kBase[16] = {0x0C0C0C, 0xC50F1F, 0x13A10E, 0xC19C00, 0x0037DA, 0x881798,
                                                0x3A96DD, 0xCCCCCC, 0x767676, 0xE74856, 0x16C60C, 0xF9F1A5,
                                                0x3B78FF, 0xB4009E, 0x61D6D6, 0xF2F2F2};
    index = std::clamp(index, 0, 255);
    if (index < 16) return kBase[index];
    if (index < 232) {
        const int i = index - 16;
        const auto level = [](int v) { return v == 0 ? 0 : 55 + v * 40; };
        return static_cast<std::uint32_t>((level(i / 36) << 16) | (level((i / 6) % 6) << 8) | level(i % 6));
    }
    const int gray = 8 + (index - 232) * 10;
    return static_cast<std::uint32_t>((gray << 16) | (gray << 8) | gray);
}

const TerminalScreen::Line& TerminalScreen::line(int index) const {
    const std::deque<Line>& history = scrollback();
    if (index < static_cast<int>(history.size())) return history[std::max(index, 0)];
    const std::vector<Line>& current = alternate_ ? alt_lines_ : lines_;
    return current[std::clamp(index - static_cast<int>(history.size()), 0, rows_ - 1)];
}

TermCell TerminalScreen::blank() const {
    TermCell cell;
    cell.bg = bg_;
    return cell;
}

void TerminalScreen::resize(int cols, int rows) {
    cols = std::max(cols, 2);
    rows = std::max(rows, 2);
    if (cols == cols_ && rows == rows_) return;
    for (std::vector<Line>* lines : {&lines_, &alt_lines_}) {
        const bool main = lines == &lines_;
        // Menos filas: las de arriba (si el cursor esta abajo) van al historial.
        while (static_cast<int>(lines->size()) > rows) {
            const bool cursor_here = main != alternate_;
            if (cursor_here && cursor_y_ >= rows) {
                if (main) {
                    scrollback_.push_back(std::move(lines->front()));
                    if (scrollback_.size() > kMaxScrollback) scrollback_.pop_front();
                }
                lines->erase(lines->begin());
                --cursor_y_;
            } else {
                lines->pop_back();
            }
        }
        while (static_cast<int>(lines->size()) < rows) lines->emplace_back(cols);
        for (Line& line : *lines) line.resize(cols);
    }
    cols_ = cols;
    rows_ = rows;
    scroll_top_ = 0;
    scroll_bottom_ = rows_ - 1;
    wrap_pending_ = false;
    clampCursor();
    ++generation_;
}

void TerminalScreen::clampCursor() {
    cursor_x_ = std::clamp(cursor_x_, 0, cols_ - 1);
    cursor_y_ = std::clamp(cursor_y_, 0, rows_ - 1);
}

int TerminalScreen::param(std::size_t index, int fallback) const {
    return index < params_.size() && params_[index] >= 0 ? params_[index] : fallback;
}

void TerminalScreen::clearCells(Line& line, int from, int to) {
    from = std::clamp(from, 0, cols_);
    to = std::clamp(to, 0, cols_);
    for (int x = from; x < to; ++x) line[x] = blank();
}

void TerminalScreen::scrollUp(int top, int bottom, int count) {
    std::vector<Line>& lines = screen();
    count = std::clamp(count, 1, bottom - top + 1);
    for (int i = 0; i < count; ++i) {
        if (!alternate_ && top == 0) {
            scrollback_.push_back(lines[top]);
            if (scrollback_.size() > kMaxScrollback) scrollback_.pop_front();
        }
        lines.erase(lines.begin() + top);
        lines.insert(lines.begin() + bottom, Line(cols_, blank()));
    }
}

void TerminalScreen::scrollDown(int top, int bottom, int count) {
    std::vector<Line>& lines = screen();
    count = std::clamp(count, 1, bottom - top + 1);
    for (int i = 0; i < count; ++i) {
        lines.erase(lines.begin() + bottom);
        lines.insert(lines.begin() + top, Line(cols_, blank()));
    }
}

void TerminalScreen::lineFeed() {
    if (cursor_y_ == scroll_bottom_) {
        scrollUp(scroll_top_, scroll_bottom_, 1);
    } else if (cursor_y_ < rows_ - 1) {
        ++cursor_y_;
    }
}

void TerminalScreen::reverseIndex() {
    if (cursor_y_ == scroll_top_) {
        scrollDown(scroll_top_, scroll_bottom_, 1);
    } else if (cursor_y_ > 0) {
        --cursor_y_;
    }
}

void TerminalScreen::print(char32_t ch) {
    const int width = charWidth(ch);
    if (width == 0) return;
    if (wrap_pending_ && autowrap_) {
        cursor_x_ = 0;
        lineFeed();
    }
    wrap_pending_ = false;
    if (width == 2 && cursor_x_ == cols_ - 1) {
        if (!autowrap_) return;
        screen()[cursor_y_][cursor_x_] = blank();
        cursor_x_ = 0;
        lineFeed();
    }
    Line& line = screen()[cursor_y_];
    TermCell& cell = line[cursor_x_];
    cell.ch = ch;
    cell.fg = fg_;
    cell.bg = bg_;
    cell.attrs = attrs_;
    cell.wide_tail = false;
    if (width == 2) {
        TermCell& tail = line[cursor_x_ + 1];
        tail = cell;
        tail.ch = U' ';
        tail.wide_tail = true;
    }
    cursor_x_ += width;
    if (cursor_x_ >= cols_) {
        cursor_x_ = cols_ - 1;
        wrap_pending_ = autowrap_;
    }
}

void TerminalScreen::control(char c) {
    switch (c) {
        case '\b':
            if (cursor_x_ > 0) --cursor_x_;
            wrap_pending_ = false;
            break;
        case '\t':
            cursor_x_ = std::min(cols_ - 1, (cursor_x_ / 8 + 1) * 8);
            break;
        case '\n':
        case '\v':
        case '\f':
            lineFeed();
            wrap_pending_ = false;
            break;
        case '\r':
            cursor_x_ = 0;
            wrap_pending_ = false;
            break;
        case 0x1B:
            state_ = State::Escape;
            break;
        default:
            break;  // BEL, SO/SI...
    }
}

void TerminalScreen::escape(char c) {
    switch (c) {
        case '7':
            saved_x_ = cursor_x_;
            saved_y_ = cursor_y_;
            break;
        case '8':
            cursor_x_ = saved_x_;
            cursor_y_ = saved_y_;
            wrap_pending_ = false;
            clampCursor();
            break;
        case 'D':
            lineFeed();
            break;
        case 'E':
            cursor_x_ = 0;
            lineFeed();
            break;
        case 'M':
            reverseIndex();
            break;
        case 'c': {
            const int cols = cols_;
            const int rows = rows_;
            *this = TerminalScreen(cols, rows);
            break;
        }
        default:
            break;
    }
}

void TerminalScreen::setMode(bool on) {
    for (std::size_t i = 0; i < params_.size(); ++i) {
        switch (params_[i]) {
            case 1: app_cursor_keys_ = on; break;
            case 7: autowrap_ = on; break;
            case 25: cursor_visible_ = on; break;
            case 2004: bracketed_paste_ = on; break;
            case 47:
            case 1047:
            case 1049:
                if (on && !alternate_) {
                    if (params_[i] == 1049) {
                        saved_x_ = cursor_x_;
                        saved_y_ = cursor_y_;
                    }
                    alternate_ = true;
                    alt_lines_.assign(rows_, Line(cols_));
                } else if (!on && alternate_) {
                    alternate_ = false;
                    if (params_[i] == 1049) {
                        cursor_x_ = saved_x_;
                        cursor_y_ = saved_y_;
                        clampCursor();
                    }
                }
                break;
            default:
                break;
        }
    }
}

void TerminalScreen::sgr() {
    if (params_.empty()) params_.push_back(0);
    for (std::size_t i = 0; i < params_.size(); ++i) {
        const int p = std::max(params_[i], 0);
        if (p == 0) {
            fg_ = bg_ = kTermDefault;
            attrs_ = 0;
        } else if (p == 1) {
            attrs_ |= kBold;
        } else if (p == 2) {
            attrs_ |= kDim;
        } else if (p == 3) {
            attrs_ |= kItalic;
        } else if (p == 4) {
            attrs_ |= kUnderline;
        } else if (p == 7) {
            attrs_ |= kInverse;
        } else if (p == 9) {
            attrs_ |= kStrike;
        } else if (p == 21 || p == 22) {
            attrs_ &= static_cast<std::uint8_t>(~(kBold | kDim));
        } else if (p == 23) {
            attrs_ &= static_cast<std::uint8_t>(~kItalic);
        } else if (p == 24) {
            attrs_ &= static_cast<std::uint8_t>(~kUnderline);
        } else if (p == 27) {
            attrs_ &= static_cast<std::uint8_t>(~kInverse);
        } else if (p == 29) {
            attrs_ &= static_cast<std::uint8_t>(~kStrike);
        } else if (p >= 30 && p <= 37) {
            fg_ = 0x01000000u | static_cast<std::uint32_t>(p - 30);
        } else if (p >= 90 && p <= 97) {
            fg_ = 0x01000000u | static_cast<std::uint32_t>(p - 90 + 8);
        } else if (p >= 40 && p <= 47) {
            bg_ = 0x01000000u | static_cast<std::uint32_t>(p - 40);
        } else if (p >= 100 && p <= 107) {
            bg_ = 0x01000000u | static_cast<std::uint32_t>(p - 100 + 8);
        } else if (p == 39) {
            fg_ = kTermDefault;
        } else if (p == 49) {
            bg_ = kTermDefault;
        } else if (p == 38 || p == 48) {
            TermColor color = kTermDefault;
            const int mode = param(i + 1, -1);
            if (mode == 5) {
                color = 0x01000000u | static_cast<std::uint32_t>(std::clamp(param(i + 2, 0), 0, 255));
                i += 2;
            } else if (mode == 2) {
                const auto c = [&](std::size_t k) { return static_cast<std::uint32_t>(std::clamp(param(k, 0), 0, 255)); };
                color = 0x02000000u | (c(i + 2) << 16) | (c(i + 3) << 8) | c(i + 4);
                i += 4;
            } else {
                continue;
            }
            (p == 38 ? fg_ : bg_) = color;
        }
    }
}

void TerminalScreen::csi(char final_char) {
    if (private_ == '?') {
        if (final_char == 'h' || final_char == 'l') setMode(final_char == 'h');
        return;
    }
    if (private_ == '>' && final_char == 'c') {
        if (reply_ != nullptr) *reply_ += "\x1b[>0;10;1c";
        return;
    }
    if (private_ != 0) return;
    if (intermediate_ != 0) {
        if (intermediate_ == '!' && final_char == 'p') {  // reinicio suave
            fg_ = bg_ = kTermDefault;
            attrs_ = 0;
            scroll_top_ = 0;
            scroll_bottom_ = rows_ - 1;
            cursor_visible_ = true;
        }
        return;
    }
    const int n = std::max(param(0, 1), 1);
    std::vector<Line>& lines = screen();
    const bool in_region = cursor_y_ >= scroll_top_ && cursor_y_ <= scroll_bottom_;
    switch (final_char) {
        case '@': {
            Line& line = lines[cursor_y_];
            const int count = std::min(n, cols_ - cursor_x_);
            line.insert(line.begin() + cursor_x_, count, blank());
            line.resize(cols_);
            break;
        }
        case 'A':
            cursor_y_ = std::max(cursor_y_ - n, in_region ? scroll_top_ : 0);
            break;
        case 'B':
        case 'e':
            cursor_y_ = std::min(cursor_y_ + n, in_region ? scroll_bottom_ : rows_ - 1);
            break;
        case 'C':
        case 'a':
            cursor_x_ += n;
            break;
        case 'D':
            cursor_x_ -= n;
            break;
        case 'E':
            cursor_y_ = std::min(cursor_y_ + n, in_region ? scroll_bottom_ : rows_ - 1);
            cursor_x_ = 0;
            break;
        case 'F':
            cursor_y_ = std::max(cursor_y_ - n, in_region ? scroll_top_ : 0);
            cursor_x_ = 0;
            break;
        case 'G':
        case '`':
            cursor_x_ = n - 1;
            break;
        case 'H':
        case 'f':
            cursor_y_ = std::max(param(0, 1), 1) - 1;
            cursor_x_ = std::max(param(1, 1), 1) - 1;
            break;
        case 'd':
            cursor_y_ = n - 1;
            break;
        case 'J': {
            const int mode = param(0, 0);
            if (mode == 0) {
                clearCells(lines[cursor_y_], cursor_x_, cols_);
                for (int y = cursor_y_ + 1; y < rows_; ++y) clearCells(lines[y], 0, cols_);
            } else if (mode == 1) {
                clearCells(lines[cursor_y_], 0, cursor_x_ + 1);
                for (int y = 0; y < cursor_y_; ++y) clearCells(lines[y], 0, cols_);
            } else if (mode == 2) {
                for (Line& line : lines) clearCells(line, 0, cols_);
            } else if (mode == 3) {
                scrollback_.clear();
            }
            break;
        }
        case 'K': {
            const int mode = param(0, 0);
            Line& line = lines[cursor_y_];
            if (mode == 0) clearCells(line, cursor_x_, cols_);
            else if (mode == 1) clearCells(line, 0, cursor_x_ + 1);
            else clearCells(line, 0, cols_);
            break;
        }
        case 'L':
            if (in_region) scrollDown(cursor_y_, scroll_bottom_, n);
            cursor_x_ = 0;
            break;
        case 'M':
            if (in_region) {
                // Borrar lineas no las manda al historial.
                std::vector<Line>& region = screen();
                const int count = std::clamp(n, 1, scroll_bottom_ - cursor_y_ + 1);
                for (int i = 0; i < count; ++i) {
                    region.erase(region.begin() + cursor_y_);
                    region.insert(region.begin() + scroll_bottom_, Line(cols_, blank()));
                }
            }
            cursor_x_ = 0;
            break;
        case 'P': {
            Line& line = lines[cursor_y_];
            const int count = std::min(n, cols_ - cursor_x_);
            line.erase(line.begin() + cursor_x_, line.begin() + cursor_x_ + count);
            line.resize(cols_, blank());
            break;
        }
        case 'S':
            scrollUp(scroll_top_, scroll_bottom_, n);
            break;
        case 'T':
            scrollDown(scroll_top_, scroll_bottom_, n);
            break;
        case 'X':
            clearCells(lines[cursor_y_], cursor_x_, cursor_x_ + n);
            break;
        case 'm':
            sgr();
            break;
        case 'r': {
            const int top = std::max(param(0, 1), 1) - 1;
            const int bottom = std::min(std::max(param(1, rows_), 1), rows_) - 1;
            if (top < bottom) {
                scroll_top_ = top;
                scroll_bottom_ = bottom;
            }
            cursor_x_ = 0;
            cursor_y_ = 0;
            break;
        }
        case 's':
            saved_x_ = cursor_x_;
            saved_y_ = cursor_y_;
            break;
        case 'u':
            cursor_x_ = saved_x_;
            cursor_y_ = saved_y_;
            break;
        case 'n':
            if (reply_ != nullptr) {
                if (param(0, 0) == 6) {
                    *reply_ += "\x1b[" + std::to_string(cursor_y_ + 1) + ";" + std::to_string(cursor_x_ + 1) + "R";
                } else if (param(0, 0) == 5) {
                    *reply_ += "\x1b[0n";
                }
            }
            break;
        case 'c':
            if (reply_ != nullptr) *reply_ += "\x1b[?1;2c";
            break;
        default:
            break;
    }
    wrap_pending_ = false;
    clampCursor();
}

void TerminalScreen::osc() {
    const std::size_t semicolon = osc_.find(';');
    if (semicolon == std::string::npos) return;
    const std::string code = osc_.substr(0, semicolon);
    if (code == "0" || code == "2") title_ = osc_.substr(semicolon + 1);
}

void TerminalScreen::feed(const char* data, std::size_t size, std::string& reply) {
    reply_ = &reply;
    for (std::size_t i = 0; i < size; ++i) {
        const unsigned char b = static_cast<unsigned char>(data[i]);
        switch (state_) {
            case State::Ground:
                if (utf8_left_ > 0) {
                    if ((b & 0xC0) == 0x80) {
                        utf8_code_ = (utf8_code_ << 6) | (b & 0x3F);
                        if (--utf8_left_ == 0) print(utf8_code_);
                        break;
                    }
                    utf8_left_ = 0;
                    print(U'�');
                }
                if (b < 0x20 || b == 0x7F) {
                    control(static_cast<char>(b));
                } else if (b < 0x80) {
                    print(b);
                } else if ((b & 0xE0) == 0xC0) {
                    utf8_code_ = b & 0x1F;
                    utf8_left_ = 1;
                } else if ((b & 0xF0) == 0xE0) {
                    utf8_code_ = b & 0x0F;
                    utf8_left_ = 2;
                } else if ((b & 0xF8) == 0xF0) {
                    utf8_code_ = b & 0x07;
                    utf8_left_ = 3;
                } else {
                    print(U'�');
                }
                break;
            case State::Escape:
                state_ = State::Ground;
                if (b == '[') {
                    state_ = State::Csi;
                    params_.clear();
                    param_started_ = false;
                    private_ = 0;
                    intermediate_ = 0;
                } else if (b == ']') {
                    state_ = State::Osc;
                    osc_.clear();
                } else if (b == 'P' || b == 'X' || b == '^' || b == '_') {
                    state_ = State::Ignore;
                } else if (b == '(' || b == ')' || b == '*' || b == '+') {
                    state_ = State::EscapeCharset;
                } else if (b == 0x1B) {
                    state_ = State::Escape;
                } else {
                    escape(static_cast<char>(b));
                }
                break;
            case State::EscapeCharset:
                state_ = State::Ground;
                break;
            case State::Csi:
                if (b >= '0' && b <= '9') {
                    if (!param_started_) {
                        params_.push_back(0);
                        param_started_ = true;
                    }
                    params_.back() = std::min(params_.back() * 10 + (b - '0'), 100000);
                } else if (b == ';' || b == ':') {
                    if (!param_started_) params_.push_back(-1);
                    param_started_ = false;
                } else if (b >= 0x3C && b <= 0x3F) {
                    private_ = static_cast<char>(b);
                } else if (b >= 0x20 && b <= 0x2F) {
                    intermediate_ = static_cast<char>(b);
                } else if (b >= 0x40 && b <= 0x7E) {
                    state_ = State::Ground;
                    csi(static_cast<char>(b));
                } else if (b == 0x1B) {
                    state_ = State::Escape;
                } else if (b < 0x20) {
                    control(static_cast<char>(b));
                }
                break;
            case State::Osc:
                if (b == 0x07) {
                    osc();
                    state_ = State::Ground;
                } else if (b == 0x1B) {
                    state_ = State::OscEscape;
                } else if (osc_.size() < 4096) {
                    osc_ += static_cast<char>(b);
                }
                break;
            case State::OscEscape:
                if (b == '\\') osc();
                state_ = State::Ground;
                break;
            case State::Ignore:
                if (b == 0x1B) state_ = State::IgnoreEscape;
                else if (b == 0x07) state_ = State::Ground;
                break;
            case State::IgnoreEscape:
                state_ = b == '\\' ? State::Ground : State::Ignore;
                break;
        }
    }
    reply_ = nullptr;
    ++generation_;
}

// =============================================================================
// TerminalSession (ConPTY)
// =============================================================================

#if defined(_WIN32)

namespace {

using CreatePseudoConsoleFn = HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, void**);
using ResizePseudoConsoleFn = HRESULT(WINAPI*)(void*, COORD);
using ClosePseudoConsoleFn = void(WINAPI*)(void*);

struct ConPtyApi {
    CreatePseudoConsoleFn create = nullptr;
    ResizePseudoConsoleFn resize = nullptr;
    ClosePseudoConsoleFn close = nullptr;
};

const ConPtyApi& conPty() {
    static const ConPtyApi api = [] {
        ConPtyApi a;
        if (HMODULE kernel = GetModuleHandleW(L"kernel32.dll")) {
            a.create = reinterpret_cast<CreatePseudoConsoleFn>(GetProcAddress(kernel, "CreatePseudoConsole"));
            a.resize = reinterpret_cast<ResizePseudoConsoleFn>(GetProcAddress(kernel, "ResizePseudoConsole"));
            a.close = reinterpret_cast<ClosePseudoConsoleFn>(GetProcAddress(kernel, "ClosePseudoConsole"));
        }
        return a;
    }();
    return api;
}

COORD coord(int cols, int rows) {
    return COORD{static_cast<SHORT>(std::clamp(cols, 2, 1000)), static_cast<SHORT>(std::clamp(rows, 2, 1000))};
}

}  // namespace

TerminalSession::~TerminalSession() { stop(); }

bool TerminalSession::start(const std::wstring& command_line, const std::filesystem::path& folder, int cols, int rows,
                            std::string& error) {
    stop();
    const ConPtyApi& api = conPty();
    if (api.create == nullptr) {
        error = "Esta version de Windows no tiene pseudoconsolas (hace falta Windows 10 1809 o mas nuevo)";
        return false;
    }
    screen_ = TerminalScreen(cols, rows);
    HANDLE input_read = nullptr;
    HANDLE input_write = nullptr;
    HANDLE output_read = nullptr;
    HANDLE output_write = nullptr;
    if (!CreatePipe(&input_read, &input_write, nullptr, 0) || !CreatePipe(&output_read, &output_write, nullptr, 0)) {
        error = "no se pudieron crear las tuberias";
        return false;
    }
    void* console = nullptr;
    const HRESULT hr = api.create(coord(cols, rows), input_read, output_write, 0, &console);
    // La pseudoconsola tiene sus copias.
    CloseHandle(input_read);
    CloseHandle(output_write);
    if (FAILED(hr)) {
        CloseHandle(input_write);
        CloseHandle(output_read);
        error = "CreatePseudoConsole fallo";
        return false;
    }

    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<unsigned char> attr_buffer(attr_size);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buffer.data());
    InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size);
    UpdateProcThreadAttribute(attrs, 0, 0x00020016 /* PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE */, console, sizeof(console),
                              nullptr, nullptr);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(STARTUPINFOEXW);
    startup.lpAttributeList = attrs;
    // Sin esto, si el editor tiene la salida redirigida (a un archivo), el
    // proceso hereda esas manijas en lugar de las de la pseudoconsola.
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    PROCESS_INFORMATION info{};
    std::wstring command = command_line;
    const std::wstring cwd = folder.empty() ? std::wstring() : folder.wstring();
    const BOOL ok = CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                                   EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, nullptr,
                                   cwd.empty() ? nullptr : cwd.c_str(), &startup.StartupInfo, &info);
    DeleteProcThreadAttributeList(attrs);
    if (!ok) {
        api.close(console);
        CloseHandle(input_write);
        CloseHandle(output_read);
        error = "no se pudo ejecutar el programa (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    CloseHandle(info.hThread);
    pseudo_console_ = console;
    input_write_ = input_write;
    output_read_ = output_read;
    process_ = info.hProcess;
    started_ = true;
    running_ = true;
    reader_ = std::thread([this] { readLoop(); });
    return true;
}

void TerminalSession::readLoop() {
    std::vector<char> buffer(64 * 1024);
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(static_cast<HANDLE>(output_read_), buffer.data(), static_cast<DWORD>(buffer.size()), &read,
                      nullptr) ||
            read == 0) {
            break;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.append(buffer.data(), read);
    }
}

void TerminalSession::stop() {
    if (pseudo_console_ != nullptr) {
        // Cierra la consola (sus procesos reciben el cierre); el hilo lector
        // termina cuando se cierra su lado de la tuberia.
        conPty().close(pseudo_console_);
        pseudo_console_ = nullptr;
    }
    if (input_write_ != nullptr) CloseHandle(static_cast<HANDLE>(input_write_));
    input_write_ = nullptr;
    if (reader_.joinable()) {
        if (output_read_ != nullptr) CancelIoEx(static_cast<HANDLE>(output_read_), nullptr);
        reader_.join();
    }
    if (output_read_ != nullptr) CloseHandle(static_cast<HANDLE>(output_read_));
    output_read_ = nullptr;
    if (process_ != nullptr) CloseHandle(static_cast<HANDLE>(process_));
    process_ = nullptr;
    running_ = false;
}

bool TerminalSession::pump() {
    std::string data;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        data.swap(pending_);
    }
    if (!data.empty()) {
        std::string reply;
        screen_.feed(data.data(), data.size(), reply);
        if (!reply.empty()) write(reply);
    }
    if (running_ && process_ != nullptr && WaitForSingleObject(static_cast<HANDLE>(process_), 0) == WAIT_OBJECT_0) {
        // Termino solo (exit): se cierra la consola; la pantalla se queda.
        stop();
        std::string reply;
        const char* note = "\r\n\x1b[90m[Proceso terminado]\x1b[0m\r\n";
        screen_.feed(note, std::strlen(note), reply);
    }
    return !data.empty();
}

void TerminalSession::write(const std::string& bytes) {
    if (input_write_ == nullptr || bytes.empty()) return;
    DWORD written = 0;
    WriteFile(static_cast<HANDLE>(input_write_), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
}

void TerminalSession::resize(int cols, int rows) {
    if (cols == screen_.cols() && rows == screen_.rows()) return;
    screen_.resize(cols, rows);
    if (pseudo_console_ != nullptr && conPty().resize != nullptr) conPty().resize(pseudo_console_, coord(cols, rows));
}

#else

TerminalSession::~TerminalSession() = default;
bool TerminalSession::start(const std::wstring&, const std::filesystem::path&, int, int, std::string& error) {
    error = "la terminal solo existe en Windows";
    return false;
}
void TerminalSession::readLoop() {}
void TerminalSession::stop() {}
bool TerminalSession::pump() { return false; }
void TerminalSession::write(const std::string&) {}
void TerminalSession::resize(int cols, int rows) { screen_.resize(cols, rows); }

#endif

std::string TerminalSession::selectedText() const {
    if (!has_selection) return {};
    int l0 = sel_start_line, c0 = sel_start_col, l1 = sel_end_line, c1 = sel_end_col;
    if (l1 < l0 || (l1 == l0 && c1 < c0)) {
        std::swap(l0, l1);
        std::swap(c0, c1);
    }
    std::string out;
    for (int l = l0; l <= l1 && l < screen_.totalLines(); ++l) {
        const TerminalScreen::Line& line = screen_.line(l);
        const int from = l == l0 ? c0 : 0;
        const int to = l == l1 ? std::min(c1 + 1, static_cast<int>(line.size())) : static_cast<int>(line.size());
        std::string text;
        for (int x = std::max(from, 0); x < to; ++x) {
            if (!line[x].wide_tail) appendUtf8(text, line[x].ch);
        }
        while (!text.empty() && text.back() == ' ') text.pop_back();
        out += text;
        if (l != l1) out += "\r\n";
    }
    return out;
}

}  // namespace cramion::editor

// Prueba de la terminal integrada (consola): el emulador VT con secuencias
// conocidas y una pseudoconsola de verdad (cmd.exe) de principio a fin.

#include "Terminal.h"

#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

using namespace cramion::editor;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::cout << (ok ? "[OK]    " : "[FALLO] ") << what << "\n";
    if (!ok) ++failures;
}

std::string lineText(const TerminalScreen& screen, int index) {
    std::string out;
    for (const TermCell& cell : screen.line(index)) {
        if (cell.wide_tail) continue;
        if (cell.ch < 0x80) out += static_cast<char>(cell.ch);
        else out += '?';
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

bool screenContains(const TerminalScreen& screen, const std::string& text) {
    for (int i = 0; i < screen.totalLines(); ++i) {
        const std::string line = lineText(screen, i);
        // El comando tecleado tambien sale: vale la linea que es solo eso.
        if (text == "VERDE" ? line == text : line.find(text) != std::string::npos) return true;
    }
    return false;
}

void feed(TerminalScreen& screen, const std::string& data) {
    std::string reply;
    screen.feed(data.data(), data.size(), reply);
}

void emulator() {
    TerminalScreen s(20, 5);
    feed(s, "hola\r\nmundo");
    check(lineText(s, 0) == "hola" && lineText(s, 1) == "mundo", "texto y salto de linea");
    feed(s, "\x1b[1;1Hxy");
    check(lineText(s, 0) == "xyla", "CUP + sobrescribir");
    feed(s, "\x1b[2;3H\x1b[K");
    check(lineText(s, 1) == "mu", "EL borra hasta el final");
    feed(s, "\x1b[31;1mR\x1b[0m");
    const TermCell& red = s.line(1)[2];
    check(red.ch == U'R' && red.fg == (0x01000000u | 1) && (red.attrs & TerminalScreen::kBold), "SGR color + negrita");
    feed(s, "\x1b[38;2;10;20;30mT");
    check(s.line(1)[3].fg == (0x02000000u | (10u << 16) | (20u << 8) | 30u), "color de 24 bits");
    feed(s, "\x1b[2J\x1b[H");
    check(lineText(s, 0).empty() && s.cursorX() == 0 && s.cursorY() == 0, "ED 2 + cursor al inicio");
    // Desplazamiento: 7 lineas en 5 filas, 2 al historial.
    feed(s, "1\r\n2\r\n3\r\n4\r\n5\r\n6\r\n7");
    check(s.totalLines() >= 7 && lineText(s, s.totalLines() - 1) == "7", "scroll al historial");
    feed(s, "\x1b[?1049h");
    check(s.alternateScreen() && lineText(s, s.totalLines() - 1).empty(), "pantalla alternativa");
    feed(s, "\x1b[?1049l");
    check(!s.alternateScreen() && lineText(s, s.totalLines() - 1) == "7", "vuelve a la principal");
    feed(s, "\x1b[2J\x1b[H\xe2\x8f\xba \xe4\xb8\xad!");  // ⏺ + un caracter ancho
    check(s.line(s.totalLines() - 5)[0].ch == 0x23FA && s.line(s.totalLines() - 5)[3].wide_tail &&
              s.line(s.totalLines() - 5)[4].ch == U'!',
          "UTF-8 y caracteres anchos");
    std::string reply;
    const std::string dsr = "\x1b[6n";
    s.feed(dsr.data(), dsr.size(), reply);
    check(reply == "\x1b[1;6R", "responde a la posicion del cursor");
    feed(s, "\x1b]0;Titulo\x07");
    check(s.title() == "Titulo", "titulo (OSC 0)");
    s.resize(10, 3);
    check(s.cols() == 10 && s.rows() == 3, "redimensionar");
}

bool waitFor(TerminalSession& session, const std::string& text, int seconds) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < end) {
        session.pump();
        if (screenContains(session.screen(), text)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

void conpty() {
    TerminalSession session;
    std::string error;
    const bool started = session.start(L"cmd.exe /q /k prompt $G", std::filesystem::temp_directory_path(), 100, 30, error);
    check(started, ("arranca cmd.exe en una pseudoconsola " + error).c_str());
    if (!started) return;
    session.write("echo CRAMION_%OS%\r");
    check(waitFor(session, "CRAMION_Windows_NT", 10), "ejecuta un comando y se ve su salida");
    session.write("powershell -NoLogo -NoProfile -Command \"Write-Host -ForegroundColor Green VERDE\"\r");
    const bool green = waitFor(session, "VERDE", 20);
    check(green, "PowerShell dentro de la terminal");
    bool colored = false;
    const TerminalScreen& s = session.screen();
    for (int i = 0; i < s.totalLines() && green; ++i) {
        const auto& line = s.line(i);
        for (std::size_t x = 0; x + 4 < line.size(); ++x) {
            if (line[x].ch == U'V' && line[x + 1].ch == U'E' && line[x].fg != kTermDefault) colored = true;
        }
    }
    if (!colored) {
        for (int i = 0; i < s.totalLines(); ++i) {
            const std::string text = lineText(s, i);
            if (text.find("VERDE") == std::string::npos) continue;
            const std::size_t at = text.rfind("VERDE");
            std::cout << "   '" << text << "' fg=" << std::hex << s.line(i)[at].fg << std::dec << "\n";
        }
    }
    check(colored, "los colores llegan (SGR de ConPTY)");
    session.resize(60, 20);
    session.write("exit\r");
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (session.running() && std::chrono::steady_clock::now() < end) {
        session.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    check(!session.running(), "detecta que el proceso termino");
}

}  // namespace

int main() {
    emulator();
    conpty();
    std::cout << (failures == 0 ? "TODO OK\n" : "HAY FALLOS\n");
    return failures == 0 ? 0 : 1;
}

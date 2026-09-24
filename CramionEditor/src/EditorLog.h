#ifndef CRAMION_EDITOR_LOG_H
#define CRAMION_EDITOR_LOG_H

#include <deque>
#include <mutex>
#include <streambuf>
#include <string>

namespace cramion::editor {

// Consola del editor: recoge todo lo que el motor escribe en std::cout y
// std::cerr (carga de modelos, avisos de Vulkan...) sin dejar de escribirlo
// en la consola de Windows. La carga de modelos usa varios hilos: todo va
// bajo un mutex.
class EditorLog {
public:
    enum class Level { Info, Warning, Error };

    struct Entry {
        Level level = Level::Info;
        std::string text;
    };

    static EditorLog& instance();

    // Engancha std::cout y std::cerr. uninstall() los deja como estaban.
    void install();
    void uninstall();

    void add(Level level, std::string text);
    void clear();

    // Para leer las entradas: bloquear mutex() mientras se recorren.
    std::mutex& mutex() { return mutex_; }
    const std::deque<Entry>& entries() const { return entries_; }

private:
    // Reenvia al streambuf original y parte el texto en lineas.
    class TeeBuffer : public std::streambuf {
    public:
        TeeBuffer(std::streambuf* original, Level level) : original_(original), level_(level) {}
        std::streambuf* original() const { return original_; }

    protected:
        int overflow(int c) override;
        std::streamsize xsputn(const char* s, std::streamsize n) override;
        int sync() override;

    private:
        void flushLine();

        std::streambuf* original_;
        Level level_;
        std::string line_;
        std::mutex line_mutex_;
    };

    static constexpr std::size_t kMaxEntries = 4000;

    std::mutex mutex_;
    std::deque<Entry> entries_;
    TeeBuffer* out_ = nullptr;
    TeeBuffer* err_ = nullptr;
};

}  // namespace cramion::editor

#endif  // CRAMION_EDITOR_LOG_H

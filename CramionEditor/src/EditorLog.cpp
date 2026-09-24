#include "EditorLog.h"

#include <iostream>

namespace cramion::editor {

EditorLog& EditorLog::instance() {
    static EditorLog log;
    return log;
}

void EditorLog::install() {
    if (out_ != nullptr) {
        return;
    }
    out_ = new TeeBuffer(std::cout.rdbuf(), Level::Info);
    err_ = new TeeBuffer(std::cerr.rdbuf(), Level::Error);
    std::cout.rdbuf(out_);
    std::cerr.rdbuf(err_);
}

void EditorLog::uninstall() {
    if (out_ == nullptr) {
        return;
    }
    std::cout.flush();
    std::cerr.flush();
    std::cout.rdbuf(out_->original());
    std::cerr.rdbuf(err_->original());
    delete out_;
    delete err_;
    out_ = nullptr;
    err_ = nullptr;
}

void EditorLog::add(Level level, std::string text) {
    // Los avisos de Vulkan llegan por cout: se reconocen por su prefijo.
    if (level == Level::Info &&
        (text.find("AVISO") != std::string::npos || text.find("WARN") != std::string::npos)) {
        level = Level::Warning;
    }
    if (text.find("ERROR") != std::string::npos || text.find("Error") != std::string::npos) {
        level = Level::Error;
    }
    std::lock_guard lock(mutex_);
    entries_.push_back(Entry{level, std::move(text)});
    while (entries_.size() > kMaxEntries) {
        entries_.pop_front();
    }
}

void EditorLog::clear() {
    std::lock_guard lock(mutex_);
    entries_.clear();
}

int EditorLog::TeeBuffer::overflow(int c) {
    if (c == traits_type::eof()) {
        return traits_type::not_eof(c);
    }
    std::lock_guard lock(line_mutex_);
    original_->sputc(static_cast<char>(c));
    if (c == '\n') {
        flushLine();
    } else {
        line_.push_back(static_cast<char>(c));
    }
    return c;
}

std::streamsize EditorLog::TeeBuffer::xsputn(const char* s, std::streamsize n) {
    std::lock_guard lock(line_mutex_);
    original_->sputn(s, n);
    for (std::streamsize i = 0; i < n; ++i) {
        if (s[i] == '\n') {
            flushLine();
        } else {
            line_.push_back(s[i]);
        }
    }
    return n;
}

int EditorLog::TeeBuffer::sync() {
    return original_->pubsync();
}

void EditorLog::TeeBuffer::flushLine() {
    if (!line_.empty()) {
        EditorLog::instance().add(level_, std::move(line_));
    }
    line_.clear();
}

}  // namespace cramion::editor

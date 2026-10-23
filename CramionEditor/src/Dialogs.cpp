#include "Dialogs.h"

#if defined(_WIN32)
#include <commdlg.h>
#include <shlobj.h>
#include <shobjidl.h>
#else
#include "SystemUtil.h"

#include <cctype>
#endif

#include <thread>

namespace cramion::editor::dialogs {

#if defined(_WIN32)

namespace {

std::wstring initialFolderText(const std::filesystem::path& folder) {
    std::error_code error;
    return (!folder.empty() && std::filesystem::is_directory(folder, error)) ? folder.wstring()
                                                                              : std::wstring{};
}

}  // namespace

std::filesystem::path openFile(HWND owner, const wchar_t* filter,
                               const std::filesystem::path& initial_folder) {
    std::vector<std::filesystem::path> files = openFiles(owner, filter, initial_folder);
    return files.empty() ? std::filesystem::path{} : files.front();
}

std::vector<std::filesystem::path> openFiles(HWND owner, const wchar_t* filter,
                                             const std::filesystem::path& initial_folder) {
    // Con varios archivos, Win32 devuelve "carpeta\0archivo1\0archivo2\0\0".
    std::vector<wchar_t> buffer(32 * 1024, L'\0');
    const std::wstring folder = initialFolderText(initial_folder);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buffer.data();
    ofn.nMaxFile = static_cast<DWORD>(buffer.size());
    ofn.lpstrInitialDir = folder.empty() ? nullptr : folder.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_ALLOWMULTISELECT |
                OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) {
        return {};
    }
    std::vector<std::filesystem::path> result;
    const std::wstring first(buffer.data());
    const wchar_t* next = buffer.data() + first.size() + 1;
    if (*next == L'\0') {
        result.emplace_back(first);  // un solo archivo: ruta completa
        return result;
    }
    while (*next != L'\0') {
        const std::wstring name(next);
        result.emplace_back(std::filesystem::path(first) / name);
        next += name.size() + 1;
    }
    return result;
}

std::filesystem::path saveFile(HWND owner, const wchar_t* filter, const wchar_t* default_extension,
                               const std::filesystem::path& initial_folder,
                               const std::wstring& default_name) {
    wchar_t buffer[MAX_PATH] = L"";
    if (!default_name.empty()) {
        wcsncpy_s(buffer, default_name.c_str(), _TRUNCATE);
    }
    const std::wstring folder = initialFolderText(initial_folder);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buffer;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = default_extension;
    ofn.lpstrInitialDir = folder.empty() ? nullptr : folder.c_str();
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) {
        return {};
    }
    return std::filesystem::path(buffer);
}

std::filesystem::path pickFolder(HWND owner, const std::filesystem::path& initial_folder) {
    std::filesystem::path result;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dialog)))) {
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        const std::wstring folder = initialFolderText(initial_folder);
        if (!folder.empty()) {
            IShellItem* start = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&start)))) {
                dialog->SetFolder(start);
                start->Release();
            }
        }
        if (SUCCEEDED(dialog->Show(owner))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    result = std::filesystem::path(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
    }
    if (SUCCEEDED(init)) {
        CoUninitialize();
    }
    return result;
}

#else  // Linux: zenity o kdialog

namespace {

std::string toUtf8(const std::wstring& wide) { return utf8(std::filesystem::path(wide)); }

// Los pares "descripcion\0*.png;*.jpg\0" -> {descripcion, {"*.png", "*.jpg"}}.
struct Filter {
    std::string name;
    std::vector<std::string> patterns;
};
std::vector<Filter> parseFilter(const wchar_t* filter) {
    std::vector<Filter> out;
    if (filter == nullptr) return out;
    const wchar_t* p = filter;
    while (*p != L'\0') {
        const std::wstring name(p);
        p += name.size() + 1;
        if (*p == L'\0') break;
        const std::wstring pattern(p);
        p += pattern.size() + 1;
        Filter f;
        f.name = toUtf8(name);
        std::string patterns = toUtf8(pattern);
        std::size_t begin = 0;
        while (begin <= patterns.size()) {
            std::size_t end = patterns.find(';', begin);
            if (end == std::string::npos) end = patterns.size();
            std::string one = patterns.substr(begin, end - begin);
            if (!one.empty()) {
                if (one == "*.*") one = "*";
                f.patterns.push_back(one);
                // Linux distingue mayusculas: *.PNG tambien.
                std::string upper = one;
                for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                if (upper != one) f.patterns.push_back(upper);
            }
            begin = end + 1;
        }
        out.push_back(std::move(f));
    }
    return out;
}

std::string startPath(const std::filesystem::path& folder, const std::string& name = {}) {
    std::error_code error;
    if (!folder.empty() && std::filesystem::is_directory(folder, error)) return utf8(folder / name);
    return name;
}

bool useKdialog() {
    const std::string desktop = sys::env("XDG_CURRENT_DESKTOP");
    const bool kde = desktop.find("KDE") != std::string::npos;
    if (kde && !sys::findInPath("kdialog").empty()) return true;
    return sys::findInPath("zenity").empty() && !sys::findInPath("kdialog").empty();
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t begin = 0;
    while (begin < text.size()) {
        std::size_t end = text.find('\n', begin);
        if (end == std::string::npos) end = text.size();
        if (end > begin) lines.push_back(text.substr(begin, end - begin));
        begin = end + 1;
    }
    return lines;
}

enum class Mode { Open, OpenMany, Save, Folder };

std::vector<std::filesystem::path> runDialog(Mode mode, const wchar_t* filter, const std::filesystem::path& folder,
                                             const std::string& default_name, const std::string& title) {
    const std::vector<Filter> filters = parseFilter(filter);
    std::vector<std::string> argv;
    if (useKdialog()) {
        argv = {"kdialog", "--title", title};
        std::string kfilter;
        for (const Filter& f : filters) {
            if (!kfilter.empty()) kfilter += "\n";
            std::string patterns;
            for (const std::string& p : f.patterns) patterns += (patterns.empty() ? "" : " ") + p;
            kfilter += patterns + "|" + f.name;
        }
        const std::string start = startPath(folder, default_name);
        switch (mode) {
            case Mode::Open: argv.insert(argv.end(), {"--getopenfilename", start.empty() ? "." : start}); break;
            case Mode::OpenMany:
                argv.insert(argv.end(), {"--getopenfilename", start.empty() ? "." : start, "--multiple", "--separate-output"});
                break;
            case Mode::Save: argv.insert(argv.end(), {"--getsavefilename", start.empty() ? "." : start}); break;
            case Mode::Folder: argv.insert(argv.end(), {"--getexistingdirectory", start.empty() ? "." : start}); break;
        }
        if (mode != Mode::Folder && !kfilter.empty()) argv.push_back(kfilter);
    } else {
        argv = {"zenity", "--file-selection", "--title=" + title};
        if (mode == Mode::OpenMany) argv.insert(argv.end(), {"--multiple", "--separator=\n"});
        if (mode == Mode::Save) argv.push_back("--save");
        if (mode == Mode::Folder) argv.push_back("--directory");
        std::string start = startPath(folder, default_name);
        if (!start.empty()) {
            // Una carpeta se indica con la barra final.
            if (default_name.empty() && start.back() != '/') start += '/';
            argv.push_back("--filename=" + start);
        }
        if (mode != Mode::Folder) {
            for (const Filter& f : filters) {
                std::string arg = "--file-filter=" + f.name + " |";
                for (const std::string& p : f.patterns) arg += " " + p;
                argv.push_back(arg);
            }
        }
    }
    std::string output;
    if (sys::runCapture(argv, output) != 0) return {};
    std::vector<std::filesystem::path> result;
    for (const std::string& line : splitLines(output)) result.push_back(fromUtf8(line));
    return result;
}

}  // namespace

std::filesystem::path openFile(HWND, const wchar_t* filter, const std::filesystem::path& initial_folder) {
    std::vector<std::filesystem::path> files = runDialog(Mode::Open, filter, initial_folder, {}, "Abrir");
    return files.empty() ? std::filesystem::path{} : files.front();
}

std::vector<std::filesystem::path> openFiles(HWND, const wchar_t* filter, const std::filesystem::path& initial_folder) {
    return runDialog(Mode::OpenMany, filter, initial_folder, {}, "Abrir");
}

std::filesystem::path saveFile(HWND, const wchar_t* filter, const wchar_t* default_extension,
                               const std::filesystem::path& initial_folder, const std::wstring& default_name) {
    std::vector<std::filesystem::path> files =
        runDialog(Mode::Save, filter, initial_folder, toUtf8(default_name), "Guardar como");
    if (files.empty()) return {};
    std::filesystem::path file = files.front();
    // Como lpstrDefExt: sin extension se pone la de por defecto.
    if (file.extension().empty() && default_extension != nullptr && *default_extension != L'\0') {
        file += "." + toUtf8(default_extension);
    }
    return file;
}

std::filesystem::path pickFolder(HWND, const std::filesystem::path& initial_folder) {
    std::vector<std::filesystem::path> folders =
        runDialog(Mode::Folder, nullptr, initial_folder, {}, "Elegir carpeta");
    return folders.empty() ? std::filesystem::path{} : folders.front();
}

#endif  // _WIN32

std::shared_ptr<AsyncFolderPick> pickFolderAsync(const std::filesystem::path& initial_folder) {
    auto pick = std::make_shared<AsyncFolderPick>();
    // Sin ventana duena: una duena de otro hilo quedaria deshabilitada (y el
    // editor tambien) si el dialogo se cuelga.
    std::thread([pick, initial_folder] {
        pick->result = pickFolder(nullptr, initial_folder);
        pick->done = true;
    }).detach();
    return pick;
}

std::string utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::filesystem::path fromUtf8(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::filesystem::path documentsFolder() {
#if !defined(_WIN32)
    // XDG: ~/Documentos (o ~/Documents) segun el idioma; si no, la carpeta personal.
    std::string out;
    if (sys::runCapture({"xdg-user-dir", "DOCUMENTS"}, out) == 0) {
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
        std::error_code error;
        if (!out.empty() && std::filesystem::is_directory(out, error)) return fromUtf8(out);
    }
    const std::string home = sys::env("HOME");
    return home.empty() ? std::filesystem::current_path() : fromUtf8(home);
#else
    PWSTR path = nullptr;
    std::filesystem::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &path))) {
        result = std::filesystem::path(path);
    }
    CoTaskMemFree(path);
    return result;
#endif
}

}  // namespace cramion::editor::dialogs

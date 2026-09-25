#include "Dialogs.h"

#include <commdlg.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <thread>

namespace cramion::editor::dialogs {

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
    PWSTR path = nullptr;
    std::filesystem::path result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &path))) {
        result = std::filesystem::path(path);
    }
    CoTaskMemFree(path);
    return result;
}

}  // namespace cramion::editor::dialogs

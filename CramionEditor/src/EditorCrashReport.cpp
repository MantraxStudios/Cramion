// Informe del cierre anterior: si el editor se cerro por un error, al volver
// a abrirlo se ensena el informe (motivo, funcion, pila y ultimas lineas del
// registro) con botones para copiarlo o abrir la carpeta (con el minidump),
// para mandarlo por Discord o correo.

#include "EditorApp.h"
#include "LoadingScreen.h"

#include <imgui.h>

#include <shellapi.h>

#ifndef CRAMION_VERSION_STRING
#define CRAMION_VERSION_STRING "dev"
#endif

namespace cramion::editor {

void EditorApp::initCrashReporting() {
    setCrashContext("Version de Cramion", CRAMION_VERSION_STRING);
    setCrashContext("GPU", renderer_.device().name());
    const std::filesystem::path pending = pendingCrashReport("CramionEditor");
    if (!pending.empty()) {
        crash_report_path_ = pending;
        crash_report_text_ = readCrashReport(pending);
        crash_report_open_ = true;
    }
}

void EditorApp::drawCrashReportWindow() {
    if (!crash_report_open_) return;
    ImGui::SetNextWindowSize(ImVec2(760.0f, 520.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    if (!ImGui::Begin("El editor se cerro la ultima vez", &crash_report_open_, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    ImGui::TextWrapped("Cramion se cerro por un error en la sesion anterior. Este es el informe; si lo mandas (Discord o "
                       "correo) junto al .dmp de su carpeta, se puede saber exactamente donde fallo.");
    ImGui::TextDisabled("%s", dialogs::utf8(crash_report_path_).c_str());
    const float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;
    if (ImGui::Button("Copiar informe", ImVec2(w, 0.0f))) {
        ImGui::SetClipboardText(crash_report_text_.c_str());
        pushToast("Informe copiado al portapapeles");
    }
    ImGui::SameLine();
    if (ImGui::Button("Abrir carpeta", ImVec2(w, 0.0f))) {
        const std::wstring args = L"/select,\"" + crash_report_path_.wstring() + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }
    ImGui::SameLine();
    if (ImGui::Button("Cerrar", ImVec2(w, 0.0f))) crash_report_open_ = false;
    ImGui::Separator();
    ImGui::PushFont(imgui_.monoFont() != nullptr ? imgui_.monoFont() : ImGui::GetFont(), 0.0f);
    ImGui::InputTextMultiline("##informe", crash_report_text_.data(), crash_report_text_.size() + 1, ImVec2(-1.0f, -1.0f),
                              ImGuiInputTextFlags_ReadOnly);
    ImGui::PopFont();
    ImGui::End();
}

}  // namespace cramion::editor

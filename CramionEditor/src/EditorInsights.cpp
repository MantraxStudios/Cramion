// Ventana > Insights: el perfilador de CPU del motor (CramionCore/profiling).
// Arbol de zonas con media, propio, p95, maximo y llamadas; grafica de los
// frames; contadores; capturas .crtrace (Perfetto / chrome://tracing) y los
// ajustes de las optimizaciones (LOD de animacion, presupuesto de particulas,
// voces de audio, presupuesto de Lua y umbral de tirones).

#include "EditorApp.h"

#include "Dialogs.h"
#include "Theme.h"

#include <CramionCore/cvar/CVar.h>
#include <CramionCore/profiling/Profiler.h>

#include <imgui.h>

#include <algorithm>
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace cramion::editor {

namespace {

double numberOf(const cvar::CVarBase& c) {
    double v = 0.0;
    if (c.type() == cvar::Type::Bool) {
        bool b = false;
        cvar::detail::parseBool(c.toString(), b);
        return b ? 1.0 : 0.0;
    }
    cvar::detail::parseNumber(c.toString(), v);
    return v;
}

void cvarFloat(const char* name, const char* label, float speed, float min, float max, const char* fmt) {
    cvar::CVarBase* c = cvar::Registry::instance().find(name);
    if (c == nullptr) return;
    float v = static_cast<float>(numberOf(*c));
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::DragFloat(label, &v, speed, min, max, fmt)) c->fromString(std::to_string(v));
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s\n%s", name, c->description().c_str());
}

void cvarInt(const char* name, const char* label, int min, int max) {
    cvar::CVarBase* c = cvar::Registry::instance().find(name);
    if (c == nullptr) return;
    int v = static_cast<int>(numberOf(*c));
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::DragInt(label, &v, 1.0f, min, max)) c->fromString(std::to_string(v));
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s\n%s", name, c->description().c_str());
}

void cvarBool(const char* name, const char* label) {
    cvar::CVarBase* c = cvar::Registry::instance().find(name);
    if (c == nullptr) return;
    bool v = numberOf(*c) != 0.0;
    if (ImGui::Checkbox(label, &v)) c->fromString(v ? "true" : "false");
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s\n%s", name, c->description().c_str());
}

ImVec4 msColor(double ms, double budget) {
    if (ms > budget * 0.5) return ImVec4(1.0f, 0.45f, 0.35f, 1.0f);
    if (ms > budget * 0.2) return ImVec4(1.0f, 0.8f, 0.35f, 1.0f);
    return ImVec4(0.75f, 0.78f, 0.82f, 1.0f);
}

}  // namespace

void EditorApp::drawInsightsWindow() {
    if (!show_insights_) return;
    ImGui::SetNextWindowSize(ImVec2(860.0f, 640.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Insights (perfilador)###insights", &show_insights_)) {
        ImGui::End();
        return;
    }
    static int window_frames = 120;
    static bool paused = false;
    static std::vector<prof::ZoneStats> zones;
    static std::vector<prof::CounterStats> counters;
    static std::vector<float> frames;
    static prof::FrameSummary summary;
    static double last_refresh = -1.0;
    static std::string message;
    const double now = ImGui::GetTime();
    if (!paused && now - last_refresh > 0.25) {
        last_refresh = now;
        zones = prof::stats(window_frames);
        counters = prof::counters(window_frames);
        frames = prof::frameTimes();
        summary = prof::summary(300);
    }

    // --- Cabecera: resumen y botones ---
    bool on = prof::enabled();
    if (ImGui::Checkbox("Medir", &on)) prof::setEnabled(on);
    ImGui::SameLine();
    ImGui::Checkbox("Congelar", &paused);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    ImGui::SliderInt("Frames", &window_frames, 10, 600);
    ImGui::SameLine();
    if (ImGui::Button("Capturar .crtrace")) {
        const std::filesystem::path file =
            prof::captureFolder() / ("captura_" + std::to_string(prof::frameIndex()) + ".crtrace");
        std::string error;
        message = prof::saveTrace(file, 600, &error) ? "Guardada: " + dialogs::utf8(file) : "Error: " + error;
    }
    ImGui::SetItemTooltip("Guarda los ultimos 600 frames (zonas por hilo y contadores).\n"
                          "Se abre en ui.perfetto.dev o chrome://tracing (arrastrando el archivo).");
    ImGui::SameLine();
    if (ImGui::Button("Abrir carpeta")) {
        std::error_code ec;
        std::filesystem::create_directories(prof::captureFolder(), ec);
#ifdef _WIN32
        ShellExecuteW(nullptr, L"open", prof::captureFolder().wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
    }
    if (!message.empty()) ImGui::TextDisabled("%s", message.c_str());
    if (const std::filesystem::path hitch = prof::lastHitchCapture(); !hitch.empty()) {
        ImGui::TextDisabled("Ultimo tiron capturado: %s", dialogs::utf8(hitch.filename()).c_str());
    }

    const double budget_ms = 1000.0 / std::max(60.0, 1.0);
    ImGui::Text("Frame: media %.2f ms  |  p50 %.2f  p95 %.2f  p99 %.2f  max %.2f ms  |  tirones %d", summary.avg_ms,
                summary.p50_ms, summary.p95_ms, summary.p99_ms, summary.max_ms, summary.hitches);
    if (!frames.empty()) {
        float top = 0.0f;
        for (float f : frames) top = std::max(top, f);
        char overlay[64];
        std::snprintf(overlay, sizeof(overlay), "ultimo %.2f ms", static_cast<double>(frames.back()));
        ImGui::PlotHistogram("##frames", frames.data(), static_cast<int>(frames.size()), 0, overlay, 0.0f,
                             std::max(top, 20.0f), ImVec2(-1.0f, 70.0f));
    }

    // --- Arbol de zonas ---
    if (ImGui::BeginTable("##zonas", 6,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_Resizable,
                          ImVec2(0.0f, ImGui::GetContentRegionAvail().y * 0.6f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Zona", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Media ms", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Propio", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("p95", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Max", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Llamadas", ImGuiTableColumnFlags_WidthFixed, 64.0f);
        ImGui::TableHeadersRow();
        for (const prof::ZoneStats& z : zones) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Dummy(ImVec2(static_cast<float>(z.depth) * 14.0f, 0.0f));
            ImGui::SameLine(0.0f, 0.0f);
            if (z.thread != 0) {
                ImGui::TextDisabled("[hilo %u] %s", z.thread, z.name.c_str());
            } else {
                ImGui::TextUnformatted(z.name.c_str());
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", z.path.c_str());
            ImGui::TableNextColumn();
            ImGui::TextColored(msColor(z.avg_ms, budget_ms), "%.3f", z.avg_ms);
            ImGui::TableNextColumn();
            ImGui::TextColored(msColor(z.self_ms, budget_ms), "%.3f", z.self_ms);
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", z.p95_ms);
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", z.max_ms);
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", z.calls);
        }
        ImGui::EndTable();
    }

    // --- Contadores y ajustes ---
    if (ImGui::BeginTable("##abajo", 2, ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("Contadores (media / max)");
        for (const prof::CounterStats& c : counters) {
            ImGui::Text("%s: %.1f / %.1f", c.name.c_str(), c.avg, c.max);
        }
        ImGui::TableNextColumn();
        ImGui::TextDisabled("Optimizaciones");
        cvarBool("anim.lod.Enabled", "LOD de animacion");
        cvarFloat("anim.lod.Near", "Cada frame hasta (m)", 0.5f, 1.0f, 1000.0f, "%.0f");
        cvarFloat("anim.lod.Far", "1 de 4 hasta (m)", 0.5f, 1.0f, 5000.0f, "%.0f");
        cvarInt("fx.particles.Budget", "Particulas como mucho", 100, 2000000);
        cvarFloat("fx.particles.CollisionDistance", "Choques hasta (m)", 0.5f, 0.0f, 10000.0f, "%.0f");
        cvarInt("audio.MaxVoices", "Voces de audio reales", 1, 512);
        cvarFloat("lua.BudgetMs", "Presupuesto por script (ms)", 0.05f, 0.0f, 100.0f, "%.2f");
        cvarFloat("prof.HitchMs", "Tiron a partir de (ms)", 1.0f, 0.0f, 10000.0f, "%.0f");
        ImGui::EndTable();
    }
    ImGui::End();
}

}  // namespace cramion::editor

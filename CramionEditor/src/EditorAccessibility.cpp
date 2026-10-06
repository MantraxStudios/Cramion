// Ventana > Accesibilidad: prueba en el editor las opciones de accesibilidad
// que el jugador tendra en el juego (daltonismo corregido o simulado, tamano
// del texto y de los subtitulos, reducir movimiento, temblor de camara) y
// el estado del job system. Se guardan en Library/Accessibility.json.

#include "EditorApp.h"

#include <CramionCore/gameplay/Accessibility.h>
#include <CramionCore/jobs/JobSystem.h>

#include <imgui.h>

namespace cramion::editor {

void EditorApp::drawAccessibilityWindow() {
    if (!show_accessibility_) return;
    ImGui::SetNextWindowSize(ImVec2(420.0f, 430.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Accesibilidad", &show_accessibility_)) {
        ImGui::End();
        return;
    }
    gameplay::AccessibilitySettings& a = gameplay::accessibility();
    bool changed = false;
    ImGui::SeparatorText("Daltonismo");
    static constexpr const char* kModes[] = {"Ninguno", "Protanopia (rojo)", "Deuteranopia (verde)", "Tritanopia (azul)",
                                             "Acromatopsia (sin color)"};
    changed |= ImGui::Combo("Tipo", &a.colorblind_mode, kModes, 5);
    int correct = a.colorblind_correct ? 0 : 1;
    static constexpr const char* kCorrect[] = {"Corregir (para quien lo tiene)", "Simular (para probar el juego)"};
    if (ImGui::Combo("Filtro", &correct, kCorrect, 2)) {
        a.colorblind_correct = correct == 0;
        changed = true;
    }
    changed |= ImGui::SliderFloat("Fuerza", &a.colorblind_strength, 0.0f, 1.0f, "%.2f");
    ImGui::SeparatorText("Texto");
    changed |= ImGui::SliderFloat("Tamano de la interfaz", &a.text_scale, 0.5f, 3.0f, "%.2fx");
    changed |= ImGui::SliderFloat("Tamano de los subtitulos", &a.subtitle_scale, 0.5f, 3.0f, "%.2fx");
    changed |= ImGui::Checkbox("Fondo detras de los subtitulos", &a.subtitle_background);
    changed |= ImGui::Checkbox("Alto contraste (contorno en el texto)", &a.high_contrast_ui);
    ImGui::SeparatorText("Movimiento");
    changed |= ImGui::Checkbox("Reducir movimiento", &a.reduce_motion);
    ImGui::SetItemTooltip("Sin motion blur, distorsion de lente ni aberracion cromatica; temblor de camara a la mitad");
    changed |= ImGui::SliderFloat("Temblor de camara", &a.camera_shake, 0.0f, 2.0f, "%.2fx");
    if (ImGui::Button("Probar temblor")) gameplay::addCameraShake(0.6f, 0.6f);
    ImGui::SameLine();
    if (ImGui::Button("Restablecer")) {
        a = gameplay::AccessibilitySettings{};
        changed = true;
    }
    if (changed) gameplay::saveAccessibility();
    ImGui::TextDisabled("Lua: Accessibility.set{colorblind = 2, textScale = 1.3}, Accessibility.save()");
    ImGui::SeparatorText("Job system");
    const jobs::JobStats js = jobs::stats();
    ImGui::Text("Hilos: %d   tareas ejecutadas: %llu   en cola: %d", js.workers,
                static_cast<unsigned long long>(js.executed), js.queued);
    ImGui::TextDisabled("CVar jobs.Threads (-1 = automatico)");
    ImGui::End();
}

}  // namespace cramion::editor

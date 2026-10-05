// Seccion Android de Configuraciones de compilacion: paquete, versiones,
// orientacion, salida (APK / AAB / OBB), permisos, icono, firma, perfil movil
// y los controles tactiles con una vista previa de la pantalla donde se
// arrastran los botones.

#include "EditorApp.h"

#include "Dialogs.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>

namespace cramion::editor {

namespace {

std::filesystem::path editorFolder() {
    wchar_t buffer[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    return std::filesystem::path(buffer).parent_path();
}

}  // namespace

bool EditorApp::drawAndroidBuildSettings(BuildConfig& c) {
    bool changed = false;
    AndroidBuildSettings& a = c.android;
    const float label_w = 150.0f;
    const auto row = [&](const char* label) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(label_w);
        ImGui::SetNextItemWidth(-1.0f);
    };
    const auto indent = [&] {
        ImGui::Dummy(ImVec2(label_w - ImGui::GetStyle().ItemSpacing.x, 0.0f));
        ImGui::SameLine(label_w);
    };
    const std::string game = c.game_name.empty() ? project_.name : c.game_name;

    // Lo que falta para compilar (SDK, Java, la libmain.so del motor).
    const std::filesystem::path folder = editorFolder();
    const bool has_arm64 = std::filesystem::exists(folder / "android" / "arm64-v8a" / "libmain.so");
    const bool has_armv7 = std::filesystem::exists(folder / "android" / "armeabi-v7a" / "libmain.so");
    const bool has_runtime = has_arm64 || has_armv7;
    static AndroidToolchain tools;
    static bool tools_checked = false;
    if (!tools_checked) {
        tools = findAndroidToolchain(folder, a.target_sdk);
        tools_checked = true;
    }
    if (!has_runtime) {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.35f, 1.0f),
                           "Falta el runtime de Android (android/arm64-v8a o armeabi-v7a/libmain.so)");
        ImGui::TextWrapped("Compila el motor con el Android NDK instalado (CRAMION_ANDROID=ON) para poder exportar.");
    }
    if (!tools.ok()) {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.35f, 1.0f), "%s", tools.error.c_str());
    } else {
        ImGui::TextDisabled("SDK: %s  (build-tools %s, android-%d)", dialogs::utf8(tools.sdk).c_str(),
                            dialogs::utf8(tools.build_tools.filename()).c_str(), tools.platform);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Volver a buscar")) tools_checked = false;

    if (ImGui::CollapsingHeader("Aplicacion", ImGuiTreeNodeFlags_DefaultOpen)) {
        row("Paquete");
        const std::string suggested = defaultAndroidPackage(game);
        changed |= ImGui::InputTextWithHint("##package", suggested.c_str(), &a.package);
        ImGui::SetItemTooltip("El identificador unico de la app (com.estudio.juego). Google Play no deja cambiarlo despues.");
        if (!a.package.empty() && !validAndroidPackage(a.package)) {
            indent();
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "No valido: minimo dos partes (a.b), letras, numeros y _");
        }
        row("Codigo de version");
        if (ImGui::InputInt("##vcode", &a.version_code)) {
            a.version_code = std::clamp(a.version_code, 1, 2100000000);
            changed = true;
        }
        ImGui::SetItemTooltip("Numero entero que sube en cada version que se publica (Google Play lo exige).\n"
                              "El nombre de version es el campo Version de arriba (%s).",
                              c.version.c_str());
        row("Orientacion");
        static const char* kOrientations[] = {"Horizontal (gira entre las dos)", "Vertical (gira entre las dos)",
                                              "Libre (gira a cualquier lado)", "Horizontal fija", "Vertical fija"};
        changed |= ImGui::Combo("##orientation", &a.orientation, kOrientations, 5);
        ImGui::SetItemTooltip("Al abrir el juego. Desde Lua se cambia cuando se quiera:\n"
                              "Screen.setOrientation(\"auto\" | \"landscape\" | \"portrait\" | \"landscape_fixed\" | \"portrait_fixed\")");
        row("Android minimo");
        static const int kLevels[] = {26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36};
        static const char* kLevelNames[] = {"8.0 (API 26)",  "8.1 (API 27)",  "9 (API 28)",   "10 (API 29)",
                                            "11 (API 30)",   "12 (API 31)",   "12L (API 32)", "13 (API 33)",
                                            "14 (API 34)",   "15 (API 35)",   "16 (API 36)"};
        const auto level_combo = [&](const char* id, int& value, int minimum) {
            int index = 0;
            for (int i = 0; i < 11; ++i) {
                if (kLevels[i] == value) index = i;
            }
            if (ImGui::BeginCombo(id, kLevelNames[index])) {
                for (int i = 0; i < 11; ++i) {
                    if (kLevels[i] < minimum) continue;
                    if (ImGui::Selectable(kLevelNames[i], i == index)) {
                        value = kLevels[i];
                        changed = true;
                    }
                }
                ImGui::EndCombo();
            }
        };
        level_combo("##min_sdk", a.min_sdk, 26);
        ImGui::SetItemTooltip("Desde Android 8 con Vulkan (cualquier version, 1.0 incluida): en los moviles el\n"
                              "renderizador va en modo compatible (Mali, Adreno, PowerVR, gama baja incluida).\n"
                              "El manifiesto pide Vulkan: Google Play oculta el juego a los que no lo tienen.");
        row("Android objetivo");
        level_combo("##target_sdk", a.target_sdk, a.min_sdk);
        if (a.target_sdk < a.min_sdk) a.target_sdk = a.min_sdk;
    }

    if (ImGui::CollapsingHeader("Salida", ImGuiTreeNodeFlags_DefaultOpen)) {
        indent();
        changed |= ImGui::Checkbox("APK (instalar directamente)", &a.make_apk);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("AAB (Google Play)", &a.make_aab);
        if (!a.make_apk && !a.make_aab) a.make_apk = true;
        indent();
        ImGui::BeginDisabled(!a.make_apk);
        changed |= ImGui::Checkbox("Assets aparte en un OBB (main.<version>.<paquete>.obb)", &a.split_obb);
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("El APK queda pequeno y los assets van en el archivo de expansion .obb.\n"
                              "Se instala en /sdcard/Android/obb/<paquete>/ (\"Exportar y jugar\" lo sube solo).\n"
                              "Desmarcado: todo dentro del APK. El AAB siempre lleva todo dentro.");
        indent();
        const bool has_x86 = std::filesystem::exists(folder / "android" / "x86_64" / "libmain.so");
        ImGui::BeginDisabled(!has_armv7);
        changed |= ImGui::Checkbox("Incluir armeabi-v7a (moviles de 32 bits)", &a.armeabi_v7a);
        ImGui::EndDisabled();
        if (!has_armv7) {
            ImGui::SetItemTooltip("No esta compilado el runtime de 32 bits (android/armeabi-v7a/libmain.so).");
        } else {
            ImGui::SetItemTooltip("Moviles baratos y Android Go con sistema de 32 bits: sin esto no se pueden instalar.");
        }
        indent();
        ImGui::BeginDisabled(!has_x86);
        changed |= ImGui::Checkbox("Incluir x86_64 (emuladores)", &a.x86_64);
        ImGui::EndDisabled();
        if (!has_x86) ImGui::SetItemTooltip("No esta compilado el runtime x86_64 (android/x86_64/libmain.so).");
        if (a.make_aab && tools.bundletool.empty()) {
            indent();
            ImGui::TextDisabled("El primer AAB descarga bundletool (32 MB, una sola vez).");
        }
    }

    if (ImGui::CollapsingHeader("Permisos")) {
        indent();
        changed |= ImGui::Checkbox("Internet (multijugador, Http)", &a.internet);
        indent();
        changed |= ImGui::Checkbox("Vibrar (Input.vibrate)", &a.vibrate);
        indent();
        changed |= ImGui::Checkbox("Microfono", &a.record_audio);
    }

    if (ImGui::CollapsingHeader("Icono")) {
        row("Icono de Android");
        changed |= ImGui::InputTextWithHint("##android_icon", "(el de la configuracion o el del motor)", &a.icon);
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kImagePayload)) {
                const std::filesystem::path file = dialogs::fromUtf8(static_cast<const char*>(payload->Data));
                std::error_code e;
                const std::filesystem::path relative = std::filesystem::relative(file, project_.folder, e);
                a.icon = dialogs::utf8(!e && !relative.empty() && *relative.begin() != ".." ? relative : file);
                changed = true;
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SetItemTooltip("PNG/JPG cuadrado (512 px o mas). Se hacen las 5 densidades (48..192 px).\n"
                              "Suelta aqui una imagen del Proyecto.");
        indent();
        if (ImGui::SmallButton("Examinar...##android_icon")) {
            const std::filesystem::path file =
                dialogs::openFile(window_.handle(), L"Imagenes (*.png;*.jpg;*.tga;*.bmp)\0*.png;*.jpg;*.jpeg;*.tga;*.bmp\0",
                                  project_.assetsFolder());
            if (!file.empty()) {
                std::error_code e;
                const std::filesystem::path relative = std::filesystem::relative(file, project_.folder, e);
                a.icon = dialogs::utf8(!e && !relative.empty() && *relative.begin() != ".." ? relative : file);
                changed = true;
            }
        }
    }

    if (ImGui::CollapsingHeader("Firma")) {
        ImGui::TextWrapped("Sin keystore se firma con una clave de depuracion (se crea sola): sirve para probar e "
                           "instalar, no para Google Play. Las contrasenas no se guardan en el proyecto.");
        row("Keystore");
        changed |= ImGui::InputTextWithHint("##keystore", "(clave de depuracion)", &a.keystore);
        indent();
        if (ImGui::SmallButton("Examinar...##keystore")) {
            const std::filesystem::path file =
                dialogs::openFile(window_.handle(), L"Keystore (*.jks;*.keystore)\0*.jks;*.keystore\0Todos\0*.*\0", project_.folder);
            if (!file.empty()) {
                a.keystore = dialogs::utf8(file);
                changed = true;
            }
        }
        ImGui::BeginDisabled(a.keystore.empty());
        row("Alias");
        changed |= ImGui::InputText("##alias", &a.key_alias);
        row("Contrasena");
        ImGui::InputText("##store_pass", &a.store_password, ImGuiInputTextFlags_Password);
        row("Contrasena de la clave");
        ImGui::InputTextWithHint("##key_pass", "(la misma)", &a.key_password, ImGuiInputTextFlags_Password);
        ImGui::EndDisabled();
    }

    if (ImGui::CollapsingHeader("Perfil movil")) {
        row("Calidad inicial");
        static const char* kQuality[] = {"Baja (recomendada)", "Media", "Alta", "Ultra"};
        changed |= ImGui::Combo("##quality", &a.quality, kQuality, 4);
        ImGui::SetItemTooltip("Punto de partida en el movil: el presupuesto adaptativo baja o sube efectos\n"
                              "para llegar a los FPS objetivo. Sin trazado de rayos.");
        row("FPS objetivo");
        if (ImGui::SliderInt("##fps", &a.target_fps, 20, 120)) changed = true;
        row("Resolucion");
        static constexpr int kResolutions[] = {0, -1, 480, 540, 720, 900, 1080, 1440};
        static const char* kResolutionNames[] = {"Segun la calidad (recomendada)", "Nativa de la pantalla", "480p", "540p",
                                                 "720p", "900p", "1080p", "1440p"};
        int selected = 0;
        for (int i = 0; i < 8; ++i) {
            if (kResolutions[i] == a.resolution) selected = i;
        }
        if (ImGui::Combo("##resolution", &selected, kResolutionNames, 8)) {
            a.resolution = kResolutions[selected];
            changed = true;
        }
        ImGui::SetItemTooltip("Lado corto de la imagen del juego; la pantalla la escala sin coste.\n"
                              "Un movil de 2400x1080 tiene 2.25 veces mas pixeles que 720p: dibujarlos\n"
                              "todos es lo que mas FPS quita. Segun la calidad: 720p en Baja, 900p en Media,\n"
                              "1080p en Alta y la nativa en Ultra.");
    }

    ImGui::Separator();
    ImGui::TextDisabled("Los controles tactiles son del proyecto: Archivo > Controles tactiles.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Abrir")) show_touch_interface_ = true;
    return changed;
}

}  // namespace cramion::editor

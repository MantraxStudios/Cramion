// Partidas, localizacion y dialogos en el editor:
//
//   Ventana > Localización      tabla de textos por idioma (claves, idiomas,
//                               importar/exportar CSV, ver lo que falta)
//   Ventana > Partidas          las ranuras guardadas en Play (Library/Saves)
//   Editor de diálogos          grafo de un .crdialog (doble clic en el asset):
//                               nodos de linea, opciones, condiciones,
//                               variables, eventos y saltos; panel de detalles,
//                               variables y una vista previa para probarlo sin
//                               dar Play. En Play se ilumina el nodo que corre.

#include "EditorApp.h"

#include "Dialogs.h"
#include "NodeGraph.h"

#include <CramionCore/gameplay/Dialogue.h>
#include <CramionCore/gameplay/Localization.h>
#include <CramionCore/gameplay/SaveGame.h>

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cstring>
#include <iostream>

namespace cramion::editor {

using gameplay::DialogueAsset;
using gameplay::DialogueNode;
using gameplay::DialogueNodeType;

struct GameplayEditorState {
    // Localizacion
    bool show_localization = false;
    bool localization_dirty = false;
    std::string loc_filter;
    std::string loc_new_key;
    std::string loc_new_code;
    std::string loc_new_name;
    bool loc_only_missing = false;
    std::string loc_status;

    // Partidas
    bool show_saves = false;
    std::string save_slot = "slot1";

    // Dialogo abierto
    bool show_dialogue = false;
    bool dialogue_focus = false;
    std::filesystem::path dialogue_path;
    DialogueAsset dialogue;
    bool dialogue_dirty = false;
    nodegraph::Canvas canvas;
    std::string search;
    core::Vec2 create_at{};
    std::optional<nodegraph::PinRef> create_from;
    int selected = 0;
    // Vista previa
    gameplay::DialogueSystem preview;
    bool previewing = false;
    std::vector<std::string> preview_log;
};

namespace {

const char* const kOps[] = {"==", "!=", "<", "<=", ">", ">="};
const char* const kSetOps[] = {"=", "+=", "-=", "toggle"};

ImU32 nodeColor(DialogueNodeType type) {
    switch (type) {
        case DialogueNodeType::Start: return IM_COL32(60, 140, 70, 255);
        case DialogueNodeType::Line: return IM_COL32(55, 95, 160, 255);
        case DialogueNodeType::Choice: return IM_COL32(150, 110, 40, 255);
        case DialogueNodeType::Condition: return IM_COL32(120, 70, 150, 255);
        case DialogueNodeType::SetVariable: return IM_COL32(60, 120, 120, 255);
        case DialogueNodeType::Event: return IM_COL32(160, 70, 60, 255);
        case DialogueNodeType::Jump: return IM_COL32(90, 90, 100, 255);
        case DialogueNodeType::End: return IM_COL32(130, 40, 40, 255);
    }
    return IM_COL32(80, 80, 80, 255);
}

// Las salidas de un nodo como punteros a su "next".
std::vector<int*> outputsOf(DialogueNode& n) {
    std::vector<int*> out;
    switch (n.type) {
        case DialogueNodeType::Start:
        case DialogueNodeType::Line:
        case DialogueNodeType::SetVariable:
        case DialogueNodeType::Event: out.push_back(&n.next); break;
        case DialogueNodeType::Choice:
            for (gameplay::DialogueOption& o : n.options) out.push_back(&o.next);
            break;
        case DialogueNodeType::Condition:
            out.push_back(&n.next);
            out.push_back(&n.next_false);
            break;
        case DialogueNodeType::Jump:
        case DialogueNodeType::End: break;
    }
    return out;
}

std::string shortText(const std::string& text, std::size_t max = 60) {
    std::string s = text;
    for (char& c : s) {
        if (c == '\n' || c == '\r') c = ' ';
    }
    if (s.size() > max) s = s.substr(0, max) + "...";
    return s;
}

std::string conditionText(const gameplay::DialogueCondition& c) {
    if (c.variable.empty()) return "(siempre)";
    return c.variable + " " + c.op + " " + c.value;
}

void conditionEditor(gameplay::DialogueCondition& c, bool& changed) {
    ImGui::PushID(&c);
    ImGui::SetNextItemWidth(110.0f);
    changed |= ImGui::InputTextWithHint("##var", "variable", &c.variable);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(56.0f);
    if (ImGui::BeginCombo("##op", c.op.c_str())) {
        for (const char* op : kOps) {
            if (ImGui::Selectable(op, c.op == op)) {
                c.op = op;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    changed |= ImGui::InputTextWithHint("##value", "valor o $otra", &c.value);
    ImGui::PopID();
}

}  // namespace

GameplayEditorState& EditorApp::gameplayEditor() {
    if (!gameplay_editor_) gameplay_editor_ = std::make_shared<GameplayEditorState>();
    return *gameplay_editor_;
}

// --- Localizacion ------------------------------------------------------------------

void EditorApp::loadLocalization() {
    if (!has_project_ && project_.folder.empty()) return;
    std::string error;
    if (!gameplay::localization().load(project_.settingsFolder() / "Localization", &error)) {
        std::cerr << "[Localizacion] " << error << "\n";
    }
    gameplayEditor().localization_dirty = false;
}

void EditorApp::saveLocalization() {
    std::string error;
    if (!gameplay::localization().save(project_.settingsFolder() / "Localization", &error)) {
        std::cerr << "[Localizacion] No se pudo guardar: " << error << "\n";
        gameplayEditor().loc_status = "Error: " + error;
        return;
    }
    gameplayEditor().localization_dirty = false;
    gameplayEditor().loc_status = "Guardado en ProjectSettings/Localization";
}

void EditorApp::drawLocalizationWindow() {
    GameplayEditorState& st = gameplayEditor();
    if (!st.show_localization) return;
    ImGui::SetNextWindowSize(ImVec2(900.0f, 560.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Localización###localization_window", &st.show_localization)) {
        ImGui::End();
        return;
    }
    gameplay::Localization& loc = gameplay::localization();

    // --- Barra ---
    if (ImGui::Button("Guardar")) saveLocalization();
    ImGui::SameLine();
    ImGui::TextDisabled(st.localization_dirty ? "(sin guardar)" : "(guardado)");
    ImGui::SameLine();
    if (ImGui::Button("Importar CSV...")) {
        const std::filesystem::path file =
            dialogs::openFile(window_.handle(), L"CSV (*.csv)\0*.csv\0Todos\0*.*\0", project_.folder);
        if (!file.empty()) {
            std::string error;
            if (loc.importCsv(file, true, &error)) {
                st.localization_dirty = true;
                st.loc_status = "Importado: " + dialogs::utf8(file.filename());
            } else {
                st.loc_status = "Error al importar: " + error;
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Exportar CSV...")) {
        const std::filesystem::path file =
            dialogs::saveFile(window_.handle(), L"CSV (*.csv)\0*.csv\0", L"csv", project_.folder, L"Textos.csv");
        if (!file.empty()) {
            std::string error;
            st.loc_status = loc.exportCsv(file, &error) ? "Exportado: " + dialogs::utf8(file.filename()) : "Error: " + error;
        }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::BeginCombo("Idioma actual", loc.language().c_str())) {
        for (const gameplay::LocalizationLanguage& l : loc.languages) {
            if (ImGui::Selectable((l.name + " (" + l.code + ")").c_str(), l.code == loc.language())) loc.setLanguage(l.code);
        }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("El idioma que ven los textos con clave en la vista Juego (y Text.get)");
    if (!st.loc_status.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", st.loc_status.c_str());
    }

    // --- Idiomas ---
    if (ImGui::CollapsingHeader("Idiomas")) {
        int remove = -1;
        for (std::size_t i = 0; i < loc.languages.size(); ++i) {
            gameplay::LocalizationLanguage& l = loc.languages[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::Text("%s", l.code.c_str());
            ImGui::SameLine(70.0f);
            ImGui::SetNextItemWidth(160.0f);
            if (ImGui::InputText("##name", &l.name)) st.localization_dirty = true;
            ImGui::SameLine();
            if (ImGui::RadioButton("Por defecto", loc.default_language == l.code)) {
                loc.default_language = l.code;
                st.localization_dirty = true;
            }
            ImGui::SameLine();
            const std::size_t missing = loc.missing(l.code).size();
            if (missing > 0) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%zu sin traducir", missing);
            else ImGui::TextDisabled("completo");
            ImGui::SameLine();
            if (ImGui::SmallButton("Quitar")) remove = static_cast<int>(i);
            ImGui::PopID();
        }
        if (remove >= 0) {
            loc.removeLanguage(loc.languages[static_cast<std::size_t>(remove)].code);
            st.localization_dirty = true;
        }
        ImGui::SetNextItemWidth(70.0f);
        ImGui::InputTextWithHint("##code", "código", &st.loc_new_code);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputTextWithHint("##lname", "nombre (Français)", &st.loc_new_name);
        ImGui::SameLine();
        if (ImGui::Button("Añadir idioma") && !st.loc_new_code.empty() && !loc.hasLanguage(st.loc_new_code)) {
            loc.addLanguage(st.loc_new_code, st.loc_new_name.empty() ? st.loc_new_code : st.loc_new_name);
            st.loc_new_code.clear();
            st.loc_new_name.clear();
            st.localization_dirty = true;
        }
        if (ImGui::Checkbox("Usar el idioma del sistema al empezar el juego", &loc.use_system_language)) st.localization_dirty = true;
    }

    // --- Claves ---
    ImGui::SetNextItemWidth(240.0f);
    ImGui::InputTextWithHint("##filter", "Buscar clave o texto", &st.loc_filter);
    ImGui::SameLine();
    ImGui::Checkbox("Solo sin traducir", &st.loc_only_missing);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.0f);
    const bool add = ImGui::InputTextWithHint("##newkey", "nueva.clave", &st.loc_new_key, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Añadir clave") || add) && !st.loc_new_key.empty() && loc.table.find(st.loc_new_key) == loc.table.end()) {
        for (const gameplay::LocalizationLanguage& l : loc.languages) loc.setText(st.loc_new_key, l.code, "");
        st.loc_new_key.clear();
        st.localization_dirty = true;
    }
    ImGui::TextDisabled("Argumentos: {0}, {nombre}. Plurales: clave#one / clave#other ({n}). En Lua: Text.get(\"clave\", ...).");

    const int columns = 2 + static_cast<int>(loc.languages.size());
    if (ImGui::BeginTable("loc_table", columns,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
                          ImVec2(0.0f, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableSetupColumn("Clave", ImGuiTableColumnFlags_WidthFixed, 200.0f);
        for (const gameplay::LocalizationLanguage& l : loc.languages) ImGui::TableSetupColumn(l.name.c_str());
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 30.0f);
        ImGui::TableHeadersRow();
        std::string remove_key;
        std::string rename_from;
        std::string rename_to;
        int row = 0;
        for (auto& [key, texts] : loc.table) {
            bool missing = false;
            bool match = st.loc_filter.empty() || nodegraph::fuzzyMatch(key, st.loc_filter);
            for (const gameplay::LocalizationLanguage& l : loc.languages) {
                const auto it = texts.find(l.code);
                if (it == texts.end() || it->second.empty()) missing = true;
                else if (!match && nodegraph::fuzzyMatch(it->second, st.loc_filter)) match = true;
            }
            if (!match || (st.loc_only_missing && !missing)) continue;
            ImGui::TableNextRow();
            ImGui::PushID(row++);
            ImGui::TableNextColumn();
            std::string name = key;
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputText("##key", &name, ImGuiInputTextFlags_EnterReturnsTrue) && name != key && !name.empty()) {
                rename_from = key;
                rename_to = name;
            }
            for (const gameplay::LocalizationLanguage& l : loc.languages) {
                ImGui::TableNextColumn();
                std::string text = texts.count(l.code) ? texts[l.code] : std::string();
                ImGui::SetNextItemWidth(-1.0f);
                const bool empty = text.empty();
                if (empty) ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.35f, 0.22f, 0.10f, 0.6f));
                if (ImGui::InputText(("##" + l.code).c_str(), &text)) {
                    texts[l.code] = text;
                    loc.touch();
                    st.localization_dirty = true;
                }
                if (empty) ImGui::PopStyleColor();
            }
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("x")) remove_key = key;
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (!remove_key.empty()) {
            loc.removeKey(remove_key);
            st.localization_dirty = true;
        }
        if (!rename_from.empty()) {
            loc.renameKey(rename_from, rename_to);
            st.localization_dirty = true;
        }
    }
    // Se guarda solo al dejar de escribir.
    if (st.localization_dirty && !ImGui::IsAnyItemActive() && ImGui::GetIO().MouseDownDuration[0] < 0.0f) saveLocalization();
    ImGui::End();
}

// --- Partidas ---------------------------------------------------------------------

void EditorApp::drawSavesWindow() {
    GameplayEditorState& st = gameplayEditor();
    if (!st.show_saves) return;
    ImGui::SetNextWindowSize(ImVec2(620.0f, 380.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Partidas guardadas###saves_window", &st.show_saves)) {
        ImGui::End();
        return;
    }
    gameplay::SaveSystem& saves = scripts_.saveSystem();
    ImGui::TextDisabled("Carpeta: %s", dialogs::utf8(saves.folder()).c_str());
    ImGui::TextWrapped("En el editor las partidas van a Library/Saves; en el juego exportado a %%APPDATA%%/<juego>/saves. "
                       "Marca los objetos con el componente Saveable. En Lua: Save.save(\"slot1\"), Save.load(\"slot1\").");
    if (playing()) {
        ImGui::SetNextItemWidth(160.0f);
        ImGui::InputText("##slot", &st.save_slot);
        ImGui::SameLine();
        if (ImGui::Button("Guardar ahora") && !st.save_slot.empty()) {
            std::string error;
            if (!saves.save(world_, st.save_slot, "Editor", &error)) std::cerr << "[Partidas] " << error << "\n";
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Tiempo jugado: %.0f s", saves.playtime());
    } else {
        ImGui::TextDisabled("Da Play para guardar o cargar partidas.");
    }
    ImGui::Separator();
    const std::vector<gameplay::SaveSlotInfo> slots = saves.list();
    if (slots.empty()) ImGui::TextDisabled("No hay partidas guardadas.");
    if (ImGui::BeginTable("saves", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Ranura");
        ImGui::TableSetupColumn("Escena");
        ImGui::TableSetupColumn("Fecha");
        ImGui::TableSetupColumn("Jugado");
        ImGui::TableSetupColumn("Tamaño");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (const gameplay::SaveSlotInfo& s : slots) {
            ImGui::TableNextRow();
            ImGui::PushID(s.slot.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(s.slot.c_str());
            if (!s.label.empty()) ImGui::SetItemTooltip("%s", s.label.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(s.scene.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(s.date.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%d:%02d", static_cast<int>(s.playtime) / 60, static_cast<int>(s.playtime) % 60);
            ImGui::TableNextColumn();
            ImGui::Text("%.1f KB", static_cast<double>(s.size_bytes) / 1024.0);
            ImGui::TableNextColumn();
            if (playing()) {
                if (ImGui::SmallButton("Cargar")) scripts_.run("Save.load(\"" + s.slot + "\")");
                ImGui::SameLine();
            }
            if (ImGui::SmallButton("Borrar")) saves.remove(s.slot);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

// --- Dialogos -----------------------------------------------------------------------

void EditorApp::createDialogueAsset(const std::filesystem::path& folder, bool example) {
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    std::filesystem::path path = folder / (example ? "Mercader.crdialog" : "Dialogo.crdialog");
    for (int i = 2; std::filesystem::exists(path); ++i) {
        path = folder / dialogs::fromUtf8((example ? "Mercader " : "Dialogo ") + std::to_string(i) + ".crdialog");
    }
    const std::string name = dialogs::utf8(path.stem());
    DialogueAsset asset;
    if (example) {
        asset = gameplay::makeExampleDialogue(name);
    } else {
        asset.name = name;
        const int start = asset.add(DialogueNodeType::Start, core::Vec2{40.0f, 120.0f}).id;
        const int line = asset.add(DialogueNodeType::Line, core::Vec2{280.0f, 100.0f}).id;
        const int end = asset.add(DialogueNodeType::End, core::Vec2{580.0f, 120.0f}).id;
        asset.find(start)->next = line;
        asset.find(line)->speaker = "Personaje";
        asset.find(line)->text = "Hola.";
        asset.find(line)->next = end;
    }
    asset.uuid = Uuid::generate();
    std::string error;
    if (!gameplay::saveDialogue(path, asset, &error)) {
        std::cerr << "[Editor] No se pudo crear el dialogo: " << error << "\n";
        return;
    }
    refreshDatabase();
    std::cout << "[Editor] Dialogo creado: " << dialogs::utf8(path.filename()) << "\n";
    openDialogueEditor(path);
}

void EditorApp::openDialogueEditor(const std::filesystem::path& file) {
    GameplayEditorState& st = gameplayEditor();
    if (st.dialogue_dirty) saveDialogueEditor();
    const std::filesystem::path path = file.is_absolute() ? file : project_.assetsFolder() / file;
    DialogueAsset asset;
    std::string error;
    if (!gameplay::loadDialogue(path, asset, &error)) {
        std::cerr << "[Editor] No se pudo abrir el dialogo: " << error << "\n";
        return;
    }
    if (asset.name.empty()) asset.name = dialogs::utf8(path.stem());
    st.dialogue = std::move(asset);
    st.dialogue_path = path;
    st.dialogue_dirty = false;
    st.selected = 0;
    st.previewing = false;
    st.canvas.selection.clear();
    st.show_dialogue = true;
    st.dialogue_focus = true;
    std::vector<nodegraph::NodeView> views;
    for (DialogueNode& n : st.dialogue.nodes) {
        nodegraph::NodeView v;
        v.id = n.id;
        v.position = &n.position;
        v.width = 220.0f;
        views.push_back(v);
    }
    st.canvas.frame(views);
}

void EditorApp::saveDialogueEditor() {
    GameplayEditorState& st = gameplayEditor();
    if (st.dialogue_path.empty()) return;
    std::string error;
    if (!gameplay::saveDialogue(st.dialogue_path, st.dialogue, &error)) {
        std::cerr << "[Editor] No se pudo guardar el dialogo: " << error << "\n";
        return;
    }
    st.dialogue_dirty = false;
}

EditorApp::GraphDoc EditorApp::graphDocDialogue() {
    GameplayEditorState& st = gameplayEditor();
    GraphDoc d;
    d.show = &st.show_dialogue;
    d.focus = &st.dialogue_focus;
    d.dirty = st.dialogue_dirty;
    d.path = st.dialogue_path;
    d.name = st.dialogue.name;
    return d;
}

void EditorApp::drawDialogueWindow() {
    GameplayEditorState& st = gameplayEditor();
    if (!st.show_dialogue) return;
    const std::string title = "Diálogo: " + st.dialogue.name + (st.dialogue_dirty ? " *" : "") + "###dialogue_editor";
    if (!beginGraphWorkspace(GraphKind::Dialogue, title.c_str())) return;  // en su pestana de arriba
    if (!st.show_dialogue && st.dialogue_dirty) saveDialogueEditor();
    DialogueAsset& d = st.dialogue;

    // --- Barra ---
    if (ImGui::Button("Guardar")) saveDialogueEditor();
    ImGui::SameLine();
    if (ImGui::Button(st.previewing ? "Parar prueba" : "Probar")) {
        if (st.previewing) {
            st.preview.stop();
            st.previewing = false;
        } else {
            st.preview = gameplay::DialogueSystem{};
            st.preview_log.clear();
            for (const gameplay::DialogueVariable& v : d.variables) st.preview.setVariable(v.name, v.value);
            st.previewing = st.preview.start(d);
        }
    }
    ImGui::SetItemTooltip("Recorre el diálogo aquí mismo (sin dar Play)");
    ImGui::SameLine();
    ImGui::TextDisabled("Clic derecho o Espacio: crear nodo · arrastrar desde un pin: enlazar · Supr: borrar");

    // Que nodo corre (Play o la prueba).
    int running_node = -1;
    if (st.previewing && st.preview.active()) running_node = st.preview.line().node;
    if (playing()) {
        const gameplay::DialogueSystem& live = scripts_.dialogueSystem();
        if (live.active() && live.dialogueName() == d.name) running_node = live.line().node;
    }

    // --- Izquierda: variables ---
    ImGui::BeginChild("dlg_left", ImVec2(240.0f, 0.0f), ImGuiChildFlags_ResizeX | ImGuiChildFlags_Borders);
    ImGui::SeparatorText("Variables");
    ImGui::TextDisabled("Compartidas por todos los diálogos\ny guardadas en las partidas.\nEn el texto: {$nombre}");
    int remove_var = -1;
    for (std::size_t i = 0; i < d.variables.size(); ++i) {
        gameplay::DialogueVariable& v = d.variables[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::SetNextItemWidth(90.0f);
        if (ImGui::InputText("##n", &v.name)) st.dialogue_dirty = true;
        ImGui::SameLine();
        std::string value = v.value.is_string() ? v.value.get<std::string>() : v.value.dump();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 26.0f);
        if (ImGui::InputText("##v", &value)) {
            if (value == "true" || value == "false") v.value = value == "true";
            else {
                char* end = nullptr;
                const double number = std::strtod(value.c_str(), &end);
                if (!value.empty() && end != nullptr && *end == 0) v.value = number;
                else v.value = value;
            }
            st.dialogue_dirty = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) remove_var = static_cast<int>(i);
        ImGui::PopID();
    }
    if (remove_var >= 0) {
        d.variables.erase(d.variables.begin() + remove_var);
        st.dialogue_dirty = true;
    }
    if (ImGui::Button("+ Variable")) {
        d.variables.push_back(gameplay::DialogueVariable{"nueva" + std::to_string(d.variables.size() + 1), false});
        st.dialogue_dirty = true;
    }
    if (st.previewing || (playing() && scripts_.dialogueSystem().active())) {
        ImGui::SeparatorText("Valores ahora");
        const nlohmann::json& vars = st.previewing ? st.preview.variables() : scripts_.dialogueSystem().variables();
        for (auto it = vars.begin(); it != vars.end(); ++it) ImGui::TextDisabled("%s = %s", it.key().c_str(), it.value().dump().c_str());
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // --- Centro: grafo ---
    std::vector<nodegraph::NodeView> views;
    std::vector<nodegraph::LinkView> links;
    views.reserve(d.nodes.size());
    const ImU32 flow_color = IM_COL32(230, 230, 235, 255);
    for (DialogueNode& n : d.nodes) {
        nodegraph::NodeView v;
        v.id = n.id;
        v.position = &n.position;
        v.width = n.type == DialogueNodeType::Line || n.type == DialogueNodeType::Choice ? 240.0f : 190.0f;
        v.header = nodeColor(n.type);
        v.title = gameplay::dialogueNodeTypeName(n.type);
        v.active = n.id == running_node;
        if (n.type != DialogueNodeType::Start) v.inputs.push_back(nodegraph::PinView{"", flow_color, true, false});
        switch (n.type) {
            case DialogueNodeType::Line:
                v.title = n.speaker.empty() ? "Línea" : n.speaker;
                v.body = shortText(n.text_key.empty() ? n.text : "[" + n.text_key + "] " + gameplay::localization().get(n.text_key), 36);
                v.outputs.push_back(nodegraph::PinView{"", flow_color, true, n.next >= 0});
                break;
            case DialogueNodeType::Choice:
                for (const gameplay::DialogueOption& o : n.options) {
                    v.outputs.push_back(nodegraph::PinView{shortText(o.text_key.empty() ? o.text : "[" + o.text_key + "]", 28),
                                                           flow_color, true, o.next >= 0});
                }
                break;
            case DialogueNodeType::Condition: {
                std::string text;
                for (const auto& c : n.conditions) text += (text.empty() ? "" : (n.require_all ? " y " : " o ")) + conditionText(c);
                v.body = shortText(text, 34);
                v.outputs.push_back(nodegraph::PinView{"Sí", IM_COL32(90, 200, 110, 255), true, n.next >= 0});
                v.outputs.push_back(nodegraph::PinView{"No", IM_COL32(220, 90, 80, 255), true, n.next_false >= 0});
                break;
            }
            case DialogueNodeType::SetVariable:
                v.body = n.variable + " " + n.op + " " + n.value;
                v.outputs.push_back(nodegraph::PinView{"", flow_color, true, n.next >= 0});
                break;
            case DialogueNodeType::Event:
                v.body = n.event + (n.method.empty() ? "" : " -> " + n.target + ":" + n.method);
                v.outputs.push_back(nodegraph::PinView{"", flow_color, true, n.next >= 0});
                break;
            case DialogueNodeType::Jump:
                v.body = n.jump_dialogue.empty() ? "a nodo " + std::to_string(n.jump_node) : n.jump_dialogue;
                break;
            case DialogueNodeType::Start: v.outputs.push_back(nodegraph::PinView{"", flow_color, true, n.next >= 0}); break;
            case DialogueNodeType::End: break;
        }
        if (!n.comment.empty()) v.tooltip = n.comment;
        views.push_back(std::move(v));
    }
    for (DialogueNode& n : d.nodes) {
        const std::vector<int*> outs = outputsOf(n);
        for (std::size_t k = 0; k < outs.size(); ++k) {
            if (*outs[k] < 0 || d.find(*outs[k]) == nullptr) continue;
            nodegraph::LinkView l;
            l.from_node = n.id;
            l.from_pin = static_cast<int>(k);
            l.to_node = *outs[k];
            l.to_pin = 0;
            l.exec = true;
            l.color = IM_COL32(220, 220, 225, 220);
            links.push_back(l);
        }
    }
    const float right_width = 340.0f;
    const ImVec2 graph_size(std::max(ImGui::GetContentRegionAvail().x - right_width - 8.0f, 200.0f), 0.0f);
    const nodegraph::Events ev = st.canvas.draw("dlg_graph", views, links, graph_size);
    if (ev.link) {
        if (DialogueNode* from = d.find(ev.link->from_node)) {
            std::vector<int*> outs = outputsOf(*from);
            if (ev.link->from_pin >= 0 && ev.link->from_pin < static_cast<int>(outs.size())) {
                *outs[static_cast<std::size_t>(ev.link->from_pin)] = ev.link->to_node;
                st.dialogue_dirty = true;
            }
        }
    }
    if (ev.unlink) {
        for (DialogueNode& n : d.nodes) {
            for (int* out : outputsOf(n)) {
                if (*out == ev.unlink->node) *out = -1;
            }
        }
        st.dialogue_dirty = true;
    }
    if (!ev.erase.empty()) {
        for (const int id : ev.erase) {
            const DialogueNode* n = d.find(id);
            if (n != nullptr && n->type == DialogueNodeType::Start) continue;  // el Inicio se queda
            d.remove(id);
        }
        st.canvas.selection.clear();
        st.selected = 0;
        st.dialogue_dirty = true;
    }
    if (ev.moved) st.dialogue_dirty = true;
    if (ev.clicked != 0) st.selected = ev.clicked;
    if (st.canvas.selection.size() == 1) st.selected = *st.canvas.selection.begin();
    if (ev.background_menu || ev.dropped) {
        st.create_at = ev.menu_position;
        st.create_from = ev.dropped;
        ImGui::OpenPopup("dlg_create");
    }
    if (ev.duplicate) {
        for (const int id : std::set<int>(st.canvas.selection)) {
            const DialogueNode* src = d.find(id);
            if (src == nullptr || src->type == DialogueNodeType::Start) continue;
            DialogueNode copy = *src;
            DialogueNode& added = d.add(copy.type, core::Vec2{copy.position.x + 30.0f, copy.position.y + 30.0f});
            const int new_id = added.id;
            added = copy;
            added.id = new_id;
            added.position = core::Vec2{copy.position.x + 30.0f, copy.position.y + 30.0f};
            added.next = -1;
            added.next_false = -1;
            for (gameplay::DialogueOption& o : added.options) o.next = -1;
        }
        st.dialogue_dirty = true;
    }
    {
        std::vector<nodegraph::SearchItem> items;
        for (int t = 1; t < gameplay::kDialogueNodeTypeCount; ++t) {
            const auto type = static_cast<DialogueNodeType>(t);
            items.push_back(nodegraph::SearchItem{"Diálogo", gameplay::dialogueNodeTypeName(type), gameplay::dialogueNodeTypeKey(type), {}, t});
        }
        const int chosen = nodegraph::searchPopup("dlg_create", items, st.search);
        if (chosen > 0) {
            // add() ya pone lo de cada tipo (dos opciones, una condicion...).
            DialogueNode& n = d.add(static_cast<DialogueNodeType>(chosen), st.create_at);
            if (n.type == DialogueNodeType::SetVariable && !d.variables.empty()) n.variable = d.variables.front().name;
            const int new_id = n.id;
            if (st.create_from && st.create_from->output) {
                if (DialogueNode* from = d.find(st.create_from->node)) {
                    std::vector<int*> outs = outputsOf(*from);
                    if (st.create_from->pin >= 0 && st.create_from->pin < static_cast<int>(outs.size())) {
                        *outs[static_cast<std::size_t>(st.create_from->pin)] = new_id;
                    }
                }
            }
            st.canvas.select(new_id);
            st.selected = new_id;
            st.dialogue_dirty = true;
        }
    }
    ImGui::SameLine();

    // --- Derecha: detalles y prueba ---
    ImGui::BeginChild("dlg_right", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    if (st.previewing) {
        ImGui::SeparatorText("Prueba");
        gameplay::DialogueSystem& p = st.preview;
        p.update(ImGui::GetIO().DeltaTime);
        for (const gameplay::DialogueEvent& e : p.takeEvents()) {
            if (e.kind == gameplay::DialogueEvent::Kind::Event) st.preview_log.push_back("Evento: " + e.name + " (" + e.argument + ")");
            if (e.kind == gameplay::DialogueEvent::Kind::Ended) st.preview_log.push_back("Fin del diálogo");
        }
        if (p.active()) {
            if (p.hasLine()) {
                ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.35f, 1.0f), "%s", p.line().speaker.c_str());
                ImGui::TextWrapped("%s", p.line().text.c_str());
            }
            if (p.waitingChoice()) {
                for (const gameplay::DialogueChoice& c : p.choices()) {
                    ImGui::BeginDisabled(!c.enabled);
                    if (ImGui::Button((std::to_string(c.index + 1) + ". " + c.text).c_str(), ImVec2(-1.0f, 0.0f))) p.choose(c.index);
                    ImGui::EndDisabled();
                }
            } else if (ImGui::Button("Seguir", ImVec2(-1.0f, 0.0f))) {
                p.advance();
            }
        } else {
            ImGui::TextDisabled("Terminó.");
        }
        for (const std::string& line : st.preview_log) ImGui::TextDisabled("%s", line.c_str());
    }
    DialogueNode* n = d.find(st.selected);
    ImGui::SeparatorText(n != nullptr ? gameplay::dialogueNodeTypeName(n->type) : "Detalles");
    bool changed = false;
    if (n == nullptr) {
        ImGui::TextDisabled("Selecciona un nodo.");
    } else {
        switch (n->type) {
            case DialogueNodeType::Start: ImGui::TextDisabled("Aquí empieza el diálogo."); break;
            case DialogueNodeType::Line:
                changed |= ImGui::InputText("Quién habla", &n->speaker);
                changed |= ImGui::InputTextMultiline("Texto", &n->text, ImVec2(-1.0f, 80.0f));
                changed |= ImGui::InputTextWithHint("Clave", "localización (opcional)", &n->text_key);
                if (!n->text_key.empty()) ImGui::TextDisabled("= %s", gameplay::localization().get(n->text_key).c_str());
                changed |= assetFilePicker("dlg_audio", project_.assetsFolder(), kAudioFileExts, "audio", n->audio);
                ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
                ImGui::SetNextItemWidth(ImGui::CalcItemWidth() - (ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x));
                changed |= ImGui::InputTextWithHint("Audio", "Audio/linea1.ogg", &n->audio);
                changed |= ImGui::DragFloat("Avanzar solo (s)", &n->auto_advance, 0.05f, 0.0f, 60.0f, "%.1f");
                ImGui::SetItemTooltip("0 = espera al jugador");
                break;
            case DialogueNodeType::Choice: {
                int remove = -1;
                for (std::size_t i = 0; i < n->options.size(); ++i) {
                    gameplay::DialogueOption& o = n->options[i];
                    ImGui::PushID(static_cast<int>(i));
                    ImGui::SeparatorText(("Opción " + std::to_string(i + 1)).c_str());
                    changed |= ImGui::InputText("Texto", &o.text);
                    changed |= ImGui::InputTextWithHint("Clave", "localización (opcional)", &o.text_key);
                    ImGui::TextDisabled("Condición:");
                    conditionEditor(o.condition, changed);
                    changed |= ImGui::Checkbox("Ocultar si no se cumple", &o.hide_if_false);
                    if (ImGui::SmallButton("Quitar opción")) remove = static_cast<int>(i);
                    ImGui::PopID();
                }
                if (remove >= 0) {
                    n->options.erase(n->options.begin() + remove);
                    changed = true;
                }
                if (ImGui::Button("+ Opción")) {
                    n->options.push_back(gameplay::DialogueOption{"Opción", {}, {}, true, -1});
                    changed = true;
                }
                break;
            }
            case DialogueNodeType::Condition: {
                changed |= ImGui::Checkbox("Todas (si no, cualquiera)", &n->require_all);
                int remove = -1;
                for (std::size_t i = 0; i < n->conditions.size(); ++i) {
                    ImGui::PushID(static_cast<int>(i));
                    conditionEditor(n->conditions[i], changed);
                    ImGui::SameLine();
                    if (ImGui::SmallButton("x")) remove = static_cast<int>(i);
                    ImGui::PopID();
                }
                if (remove >= 0) {
                    n->conditions.erase(n->conditions.begin() + remove);
                    changed = true;
                }
                if (ImGui::Button("+ Condición")) {
                    n->conditions.push_back(gameplay::DialogueCondition{});
                    changed = true;
                }
                break;
            }
            case DialogueNodeType::SetVariable:
                changed |= ImGui::InputText("Variable", &n->variable);
                if (ImGui::BeginCombo("Operación", n->op.c_str())) {
                    for (const char* op : kSetOps) {
                        if (ImGui::Selectable(op, n->op == op)) {
                            n->op = op;
                            changed = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                changed |= ImGui::InputText("Valor", &n->value);
                break;
            case DialogueNodeType::Event:
                changed |= ImGui::InputText("Evento", &n->event);
                ImGui::SetItemTooltip("Dialogue.onEvent(function(nombre, argumento) ... end)");
                changed |= ImGui::InputText("Argumento", &n->argument);
                changed |= ImGui::InputTextWithHint("Objeto", "nombre en la escena (opcional)", &n->target);
                changed |= ImGui::InputTextWithHint("Método", "de su script (opcional)", &n->method);
                break;
            case DialogueNodeType::Jump:
                changed |= ImGui::InputInt("Nodo", &n->jump_node);
                {
                    static const std::vector<std::string> kDialogs = {".crdialog"};
                    changed |= assetFilePicker("dlg_jump", project_.assetsFolder(), kDialogs, "diálogo .crdialog", n->jump_dialogue);
                }
                ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
                ImGui::SetNextItemWidth(ImGui::CalcItemWidth() - (ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x));
                changed |= ImGui::InputTextWithHint("Otro diálogo", "nombre o ruta (vacío = este)", &n->jump_dialogue);
                break;
            case DialogueNodeType::End: ImGui::TextDisabled("Termina el diálogo."); break;
        }
        changed |= ImGui::InputTextWithHint("Nota", "comentario del nodo", &n->comment);
    }
    if (changed) st.dialogue_dirty = true;
    ImGui::EndChild();

    if (st.dialogue_dirty && !ImGui::IsAnyItemActive() && ImGui::GetIO().MouseDownDuration[0] < 0.0f) saveDialogueEditor();
    ImGui::End();
}

// --- Enganches -------------------------------------------------------------------

void EditorApp::drawGameplayWindows() {
    if (!has_project_) return;
    drawLocalizationWindow();
    drawSavesWindow();
    drawDialogueWindow();
}

void EditorApp::drawGameplayWindowMenu() {
    GameplayEditorState& st = gameplayEditor();
    ImGui::MenuItem("Localización (idiomas)", nullptr, &st.show_localization);
    ImGui::MenuItem("Partidas guardadas", nullptr, &st.show_saves);
}

}  // namespace cramion::editor

#ifndef CRAMION_CORE_GAMEPLAY_DIALOGUE_UI_H
#define CRAMION_CORE_GAMEPLAY_DIALOGUE_UI_H

// Dibujo e interaccion de la caja de dialogo (componente DialogueBox) dentro
// del UiSystem: lo llama UiSystem::update por cada caja visible.

#include "CramionCore/gameplay/Dialogue.h"
#include "CramionCore/ui/UI.h"

#include <vector>

namespace cramion::gameplay {

// Anade a `out` el panel, el nombre, el texto y las opciones del dialogo
// activo (nada si no hay). Con `interactive` atiende clics y teclas. Devuelve
// true si el raton esta sobre la caja (el juego no debe usar ese clic).
bool updateDialogueBox(const DialogueBox& box, const ui::UiRect& rect, float scale, const ui::UiInput& input,
                       bool interactive, entt::entity entity, std::vector<ui::UiDrawCommand>& out);

}  // namespace cramion::gameplay

#endif  // CRAMION_CORE_GAMEPLAY_DIALOGUE_UI_H

#ifndef CRAMION_CORE_ECS_RENDER_LAYERS_H
#define CRAMION_CORE_ECS_RENDER_LAYERS_H

// Capas de render, viewmodel y contornos de juego: del ECS al renderizador
// (RM-capas). Sin GPU: RenderSync las llama cada frame y las pruebas, sueltas.
//
//   MeshRenderer.render_layers -> scene::Actor::layers
//   Camera.culling_mask / viewmodel_* -> scene::Camera (vista principal y
//                                        Render Textures)
//   Light.shadow_layers -> scene::*Light::shadow_layers
//   Outline -> scene::GameOutlines (VulkanRenderer::setGameOutlines)
//
// Los nombres de las 32 capas son los del proyecto (ProjectSettings/
// Physics.json, Proyecto > Capas): las mismas que usa la fisica, como en Unity.

#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/Outline.h"

#include <CramionFX/scene/Camera.h>
#include <CramionFX/scene/RenderLayers.h>

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string_view>
#include <vector>

namespace cramion::ecs {

class World;
class Entity;

// La culling mask y el viewmodel del componente en la camara del render.
void applyCameraLayers(const Camera& camera, scene::Camera& view);

// El estilo del contorno en el instante `time_seconds` (con el pulso).
scene::OutlineStyle outlineStyle(const Outline& outline, float time_seconds);

// Los contornos del frame: cada entidad activa con Outline (activo) y sus
// actores. `actors_of(entidad, con_hijos)` da los indices en scene.actors()
// de su Mesh Renderer (y los de sus hijos): RenderSync::actorIndicesInSubtree.
scene::GameOutlines collectOutlines(World& world, float time_seconds,
                                    const std::function<std::vector<std::uint32_t>(Entity, bool)>& actors_of);

// Mascara de las capas con esos nombres (las del proyecto; las que no existen
// se ignoran): como Physics.layerMask, para las capas de render.
std::uint32_t renderLayerMask(std::initializer_list<std::string_view> names);

}  // namespace cramion::ecs

#endif  // CRAMION_CORE_ECS_RENDER_LAYERS_H

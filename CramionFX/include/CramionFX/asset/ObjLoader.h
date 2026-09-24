#ifndef CRAMION_ASSET_OBJ_LOADER_H
#define CRAMION_ASSET_OBJ_LOADER_H

#include "CramionFX/asset/Model.h"

#include <filesystem>

namespace cramion::asset {

// Lector de OBJ + MTL propio, pensado para escenarios enormes (San Miguel:
// 1.1 GB de texto, 10 millones de triangulos).
//
// assimp los lee en un solo hilo y aplica pasos globales muy caros (unir
// vertices de todo el modelo, tangentes, optimizar la cache de vertices):
// minutos y varios GB de RAM. Aqui el archivo se parsea en trozos en paralelo,
// y despues cada material se procesa en su propio hilo (vertices unicos,
// tangentes, indices), porque los materiales no comparten vertices.
//
// El resultado es un ModelData estatico: un solo nodo y un solo hueso, asi que
// se dibuja con el mismo pipeline que los modelos con esqueleto. Las texturas
// quedan comprimidas (TextureData::encoded) para decodificarlas en paralelo.
// `by_object`: agrupa ademas por objeto/grupo ("o"/"g"): cada uno es un nodo
// (ModelData::nodes, transform identidad) y sus submallas llevan su indice en
// SubMesh::node. Sin el, solo por material (lo mas rapido de dibujar).
ModelData loadObj(const std::filesystem::path& path, bool by_object = false);

}  // namespace cramion::asset

#endif  // CRAMION_ASSET_OBJ_LOADER_H

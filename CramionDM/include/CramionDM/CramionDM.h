#pragma once

// Cabecera principal de la librería CramionDM (DirectX 12).
// Incluir esto da acceso a toda la API pública de la librería.

#include "CramionDM/Device.h"    // Dispositivo DX12
#include "CramionDM/Event.h"     // Sistema de eventos
#include "CramionDM/Input.h"     // Estado de entrada (polling)
#include "CramionDM/KeyCode.h"   // Teclas, botones y modificadores
#include "CramionDM/Window.h"    // Ventana Win32 + bombeo de eventos

namespace cramion::dm {

// Versión de la librería.
inline constexpr int kVersionMajor = 0;
inline constexpr int kVersionMinor = 1;
inline constexpr int kVersionPatch = 0;

}  // namespace cramion::dm

#include <CramionDM/CramionDM.h>

#include <cstdlib>
#include <iostream>

using namespace cramion::dm;

int main() {
	// 1) Inicializar el dispositivo DirectX 12.
	Device device;
	if (!device.initialize(/*enableDebugLayer=*/true)) {
		std::wcerr << L"Error: no se pudo inicializar el dispositivo DirectX 12.\n";
		return EXIT_FAILURE;
	}
	std::wcout << L"CramionDM inicializado. GPU: " << device.adapterName() << L"\n";

	// 2) Crear la ventana.
	Window window;
	if (!window.create({.title = L"CramionDM - Eventos y Teclas", .width = 1280, .height = 720})) {
		std::wcerr << L"Error: no se pudo crear la ventana.\n";
		return EXIT_FAILURE;
	}

	// 3) Estado de entrada para consultas por frame.
	Input input;

	// 4) Registrar el callback de eventos.
	window.setEventCallback([&](Event& e) {
		input.onEvent(e);  // Alimentar el estado de entrada.

		switch (e.type) {
			case EventType::WindowClose:
				std::cout << "[Evento] Cierre de ventana solicitado\n";
				break;
			case EventType::WindowResize:
				std::cout << "[Evento] Redimension: " << e.width << "x" << e.height << "\n";
				break;
			case EventType::WindowMinimized:
				std::cout << "[Evento] Ventana minimizada\n";
				break;
			case EventType::WindowMaximized:
				std::cout << "[Evento] Ventana maximizada\n";
				break;
			case EventType::WindowRestored:
				std::cout << "[Evento] Ventana restaurada\n";
				break;
			case EventType::WindowDpiChanged:
				std::cout << "[Evento] Cambio de DPI, escala = " << e.dpiScale << "\n";
				break;
			case EventType::FileDropped:
				std::cout << "[Drop]   " << e.paths.size() << " archivo(s) en ("
						  << e.mouseX << ", " << e.mouseY << "):\n";
				for (const auto& p : e.paths) {
					std::wcout << L"           " << p << L"\n";
				}
				break;
			case EventType::KeyPressed:
				std::cout << "[Tecla ↓] " << keyName(e.key)
						  << (e.repeat ? " (repeat)" : "") << "\n";
				break;
			case EventType::KeyReleased:
				std::cout << "[Tecla ↑] " << keyName(e.key) << "\n";
				break;
			case EventType::MouseButtonPressed:
				std::cout << "[Raton]  boton " << static_cast<int>(e.button)
						  << " en (" << e.mouseX << ", " << e.mouseY << ")\n";
				break;
			case EventType::MouseScrolled:
				std::cout << "[Raton]  scroll " << e.scrollY << "\n";
				break;
			default:
				break;
		}
	});

	std::cout << "Ventana abierta. Pulsa ESC para salir.\n";

	// 5) Bucle principal.
	while (window.isOpen()) {
		window.pumpEvents();

		// Consulta de estado por frame (polling).
		if (input.isKeyPressed(Key::Escape)) {
			std::cout << "ESC pulsada: cerrando.\n";
			break;
		}

		input.newFrame();  // Limpiar estados de un frame al final del frame.
	}

	return EXIT_SUCCESS;
}

// Menu.cpp: menu de inicio; jugar, salir y el record guardado.
#include <cramion/Script.h>

#include <string>

using namespace cramion;

class Menu : public Script {
public:
    void awake() override {
        on("OnJugar", [this](const Value&) { jugar(); });
        on("OnSalir", [](const Value&) {
            Audio::playOneShot("Audio/click.wav");
            Game::quit();
        });
    }

    void start() override {
        const int record = Prefs::getInt("record", 0).asInt();
        if (Entity texto = Scene::find("Menu_Record")) {
            texto.setText(record > 0 ? "Mejor partida: " + std::to_string(record) + " golpes" : std::string("¡Juega tu primera partida!"));
        }
    }

    void update(float) override {
        if (Input::keyDown("Enter") || Input::keyDown("Space")) jugar();
    }

private:
    void jugar() {
        Audio::playOneShot("Audio/click.wav");
        for (int i = 1; i <= 5; ++i) Prefs::deleteKey("golpes_" + std::to_string(i));
        Scene::load("Nivel1");
    }
};

CRAMION_SCRIPT(Menu)

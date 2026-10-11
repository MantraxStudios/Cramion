// Resultados.cpp: tarjeta final; golpes por hoyo, total contra el par y record.
#include <cramion/Script.h>

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

using namespace cramion;

namespace {

std::vector<std::string> separar(const std::string& texto) {
    std::vector<std::string> lista;
    std::stringstream in(texto);
    for (std::string parte; std::getline(in, parte, ',');) {
        if (!parte.empty()) lista.push_back(parte);
    }
    return lista;
}

std::string conSigno(int diferencia, const char* cero) {
    if (diferencia > 0) return "+" + std::to_string(diferencia);
    return diferencia == 0 ? std::string(cero) : std::to_string(diferencia);
}

}  // namespace

class Resultados : public Script {
public:
    Property<std::string> pares{this, "pares", "2,3,3,3,4"};
    Property<std::string> nombres{this, "nombres", "Primer golpe,La curva,El tunel,El castillo,El molino"};

    void awake() override {
        on("OnOtraVez", [](const Value&) {
            Audio::playOneShot("Audio/click.wav");
            for (int i = 1; i <= 5; ++i) Prefs::deleteKey("golpes_" + std::to_string(i));
            Scene::load("Nivel1");
        });
        on("OnMenu", [](const Value&) {
            Audio::playOneShot("Audio/click.wav");
            Scene::load("Menu");
        });
    }

    void start() override {
        const std::vector<std::string> listaPares = separar(pares);
        const std::vector<std::string> listaNombres = separar(nombres);
        std::string lineas;
        int total = 0, totalPar = 0;
        for (std::size_t i = 0; i < listaPares.size(); ++i) {
            const int golpes = Prefs::getInt("golpes_" + std::to_string(i + 1), 0).asInt();
            int par = 3;
            try {
                par = std::stoi(listaPares[i]);
            } catch (...) {
            }
            total += golpes;
            totalPar += par;
            char linea[160];
            std::snprintf(linea, sizeof(linea), "%d.  %-14s  par %d   %2d golpes   %s", static_cast<int>(i + 1),
                          i < listaNombres.size() ? listaNombres[i].c_str() : "", par, golpes, conSigno(golpes - par, "E").c_str());
            if (!lineas.empty()) lineas += "\n";
            lineas += linea;
        }
        if (Entity tarjeta = Scene::find("Res_Tarjeta")) tarjeta.setText(lineas);
        if (Entity resumen = Scene::find("Res_Total")) {
            resumen.setText("TOTAL  " + std::to_string(total) + " golpes  (" + conSigno(total - totalPar, "al par") + ")");
        }
        const int record = Prefs::getInt("record", 0).asInt();
        Entity aviso = Scene::find("Res_Record");
        if (total > 0 && (record == 0 || total < record)) {
            Prefs::setInt("record", total);
            if (aviso) aviso.setText("¡NUEVO RECORD!");
        } else if (aviso) {
            aviso.setText("Record: " + std::to_string(record) + " golpes");
        }
        Audio::playOneShot("Audio/fanfarria.wav");
    }
};

CRAMION_SCRIPT(Resultados)

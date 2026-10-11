// Pruebas de TestApi.cpp: Test.begin/check/finish/done/log/results (lo que
// llama el script CramionTests del SDK), el borrado al empezar Play y el
// TestHost que lee el ejecutor del editor (loadTestFile/testCall/testStep).

#include "ApiTest.h"

#include <filesystem>
#include <fstream>
#include <string>

using namespace cramion;
using namespace cramion::apitest;

int main() {
    std::printf("TestApi\n");
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "cramion_test_api_test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "Tests");
    std::ofstream(root / "Tests" / "Jugador.cpp", std::ios::binary) << "CRAMION_TEST(salta) {}\n";

    ApiFixture t(false);
    t.scripts.setAssetsRoot(root);

    // Sin Play el ejecutor no puede cargar nada.
    std::string error;
    check(!t.scripts.loadTestFile("", &error) && error == "no hay Play", "loadTestFile sin Play: \"no hay Play\"");
    t.scripts.start(t.world);

    // --- Sin casos ---
    Value r = t.call("Test.results");
    check(r.isObject() && !r["done"].truthy() && r["passed"].asNumber() == 0 && r["failed"].asNumber() == 0 &&
              r["cases"].isArray() && r["cases"].size() == 0,
          "results al empezar: {done = false, passed = 0, failed = 0, cases = {}}");
    check(json::parse(t.scripts.testStep(0.0f)).value("state", "") == "running", "testStep sin done: running");

    // --- Un caso que falla y otro que va bien ---
    check(t.call("Test.begin", {Value("suma")}).isNil(), "Test.begin");
    check(t.call("Test.check", {Value(true), Value("bien")}).truthy(), "Test.check(true) devuelve true");
    t.frame(0.5f);
    check(!t.call("Test.check", {Value(false), Value("2 + 2 no es 5")}).truthy(), "Test.check(false) devuelve false");
    check(t.logged("[Test] suma: 2 + 2 no es 5"), "el fallo sale en la consola");
    t.call("Test.log", {Value("una linea")});
    check(t.logged("[Test] una linea"), "Test.log escribe en la consola");
    check(!t.call("Test.finish").truthy(), "Test.finish devuelve false si fallo");
    check(t.call("Test.finish").isNil(), "Test.finish sin caso abierto: nil");
    t.call("Test.begin", {Value("resta"), Value("Tests/Jugador.cpp")});
    t.call("Test.check", {Value(1)});
    t.call("Test.begin", {Value("vacio")});  // cierra "resta"
    check(json::parse(t.scripts.testStep(1.0f / 60.0f)).value("state", "") == "running", "testStep antes de done: running");
    t.call("Test.done");
    check(t.logged("[Test] 2 bien, 1 mal"), "Test.done escribe el resumen");

    r = t.call("Test.results");
    check(r["done"].truthy() && r["passed"].asNumber() == 2 && r["failed"].asNumber() == 1 && r["cases"].size() == 3,
          "results: done, 2 bien, 1 mal, 3 casos");
    const Value& suma = r["cases"][0];
    check(suma["name"].asString() == "suma" && !suma["passed"].truthy() && suma["assertions"].asNumber() == 2 &&
              suma["failures"].asNumber() == 1,
          "caso suma: passed = false, 2 comprobaciones, 1 fallo");
    check(suma["messages"].size() == 2 && suma["messages"][0].asString() == "2 + 2 no es 5" &&
              suma["messages"][1].asString() == "una linea",
          "caso suma: messages = {fallo, log}");
    check(suma["failure"].asString() == "2 + 2 no es 5", "caso suma: failure = el primer fallo");
    check(suma["seconds"].asNumber() > 0.49 && suma["seconds"].asNumber() < 0.51, "caso suma: seconds (Time.time) = 0.5");
    const Value& resta = r["cases"][1];
    check(resta["name"].asString() == "resta" && resta["passed"].truthy() && resta["assertions"].asNumber() == 1 &&
              resta["file"].asString() == "Tests/Jugador.cpp",
          "caso resta: bien, 1 comprobacion, con su archivo");
    check(r["cases"][2]["passed"].truthy() && r["cases"][2]["assertions"].asNumber() == 0, "caso vacio: bien sin comprobaciones");

    // Por el puente (como lo llama el SDK).
    const json b = t.bridge({{"fn", "Test.results"}, {"args", json::array()}});
    check(b.value("ok", false) && b["result"]["done"] == true && b["result"]["cases"].size() == 3 &&
              b["result"]["cases"][0]["messages"][0] == "2 + 2 no es 5",
          "puente: Test.results");

    // --- TestHost: lo que lee el ejecutor del editor ---
    const json step = json::parse(t.scripts.testStep(0.1f));
    check(step.value("state", "") == "failed" && step.value("message", "") == "suma: 2 + 2 no es 5" &&
              step.value("assertions", 0) == 3 && step.value("passed", 0) == 2 && step.value("failed", 0) == 1 &&
              step["cases"].size() == 3 && step["log"].size() == 2,
          "testStep tras done: failed con el primer fallo");
    const json results = json::parse(t.scripts.testCall("results"));
    check(results["done"] == true && results["failed"] == 1 && results["cases"][1]["name"] == "resta", "testCall(\"results\")");
    check(json::parse(t.scripts.testCall("cases")).size() == 3 && json::parse(t.scripts.testCall("__cramion_test_list")).size() == 3,
          "testCall(\"cases\") y el nombre de antes (__cramion_test_list)");
    check(t.scripts.testCall("run_edit") == "[]", "run_edit: no hay pruebas de edicion");
    check(t.scripts.testCall("done") == "true", "testCall(\"done\")");
    const json status = json::parse(t.scripts.testCall("__cramion_scene_status"));
    check(status["result"] == "failed" && status["failures"] == 1, "__cramion_scene_status");
    check(t.scripts.testCall("no_existe").empty(), "operacion desconocida: vacio");

    check(t.scripts.loadTestFile("", &error), "loadTestFile(\"\") = todas");
    check(t.scripts.loadTestFile("Tests/Jugador.cpp", &error) && t.scripts.testCall("file") == "Tests/Jugador.cpp",
          "loadTestFile de un .cpp de Assets");
    check(!t.scripts.loadTestFile("Tests/NoEsta.cpp", &error) && error.find("NoEsta.cpp") != std::string::npos,
          "loadTestFile de un archivo que no existe: error");
    check(t.scripts.testCall("class") == "CramionTests", "la clase por defecto es CramionTests");
    check(t.scripts.loadTestFile("MisPruebas", &error) && t.scripts.testCall("class") == "MisPruebas",
          "loadTestFile(nombre sin extension) = la clase");

    // begin: una ejecucion nueva; un caso que va bien -> passed.
    check(t.scripts.testCall("begin") == "ok", "testCall(\"begin\") = ok");
    check(t.call("Test.results")["cases"].size() == 0, "begin olvida los resultados");
    t.call("Test.check", {Value(true)});  // sin Test.begin: un caso sin nombre
    check(t.call("Test.results")["cases"][0]["name"].asString() == "(sin caso)", "Test.check sin begin: caso \"(sin caso)\"");
    t.call("Test.done");
    check(json::parse(t.scripts.testStep(0.0f)).value("state", "") == "passed", "testStep: passed si no hay fallos");
    t.scripts.testCall("__cramion_test_abort");
    const json aborted = json::parse(t.scripts.testStep(0.0f));
    check(aborted.value("state", "") == "failed" && aborted.value("message", "") == "cancelada", "abort: failed, cancelada");

    // --- Al empezar Play se borra todo ---
    t.call("Test.begin", {Value("x")});
    t.call("Test.check", {Value(false)});
    t.scripts.stop();
    check(json::parse(t.scripts.testCall("results"))["cases"].size() == 2, "los resultados siguen al parar (para leerlos)");
    t.scripts.start(t.world);
    r = t.call("Test.results");
    check(!r["done"].truthy() && r["cases"].size() == 0 && r["failed"].asNumber() == 0, "Play nuevo: resultados vacios");
    check(t.scripts.testCall("class") == "CramionTests", "Play nuevo: la clase vuelve a CramionTests");
    check(json::parse(t.scripts.testStep(0.0f)).value("state", "") == "running", "Play nuevo: testStep running");
    t.call("Test.check", {Value(false)});
    check(t.call("Test.results")["cases"][0]["messages"][0].asString() == "comprobacion fallida",
          "Test.check(false) sin mensaje: \"comprobacion fallida\"");

    std::filesystem::remove_all(root);
    return finish();
}

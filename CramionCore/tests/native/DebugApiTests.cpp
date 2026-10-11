// Pruebas de native/DebugApi.cpp: Debug.log / warn / error y print.

#include "ApiTest.h"

using namespace cramion;
using namespace cramion::apitest;

int main() {
    std::printf("Debug\n");
    ApiFixture t;
    t.call("Debug.log", {Value("hola"), Value(3)});
    check(!t.log.empty() && t.log.back().first == 0 && t.log.back().second == "hola 3", "Debug.log une los argumentos");
    t.call("Debug.warn", {Value("cuidado")});
    check(t.log.back().first == 1, "Debug.warn es un aviso");
    const json r = t.bridge({{"fn", "Debug.error"}, {"args", {"mal"}}});
    check(r["ok"] == true && t.log.back().first == 2 && t.log.back().second == "mal", "Debug.error por el puente");
    t.call("print", {Value(1.5)});
    check(t.log.back().second == "1.5", "print");
    return finish();
}

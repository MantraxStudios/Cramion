// Prueba del autocompletado de Lua (consola). Devuelve 0 si todo va.
#include "../src/LuaCompletion.h"

#include <CramionCore/scripting/Scripting.h>

#include <cstdio>
#include <string>

using namespace cramion::editor;

int failures = 0;
void check(bool ok, const char* what) {
    std::printf("  %s %s\n", ok ? "OK   " : "FALLO", what);
    if (!ok) ++failures;
}
bool has(const std::vector<LuaCompletion>& list, const std::string& label) {
    for (const auto& c : list) if (c.label == label) return true;
    return false;
}
std::vector<LuaCompletion> at(const std::string& text) {
    LuaCompletionContext ctx;
    if (!luaCompletionContext(text, text.size(), ctx)) return {};
    return luaCompletions(text, ctx);
}

int main(int argc, char** argv) {
    // La API real del motor (como hace el editor al abrir).
    std::map<std::string, std::vector<LuaApiMember>> reference;
    for (const auto& [owner, members] : cramion::scripting::ScriptSystem::apiReference()) {
        for (const auto& m : members) reference[owner].push_back(LuaApiMember{m.name, m.function});
    }
    if (argc > 1 && std::string(argv[1]) == "--dump") {
        for (const auto& [owner, members] : reference) {
            std::printf("[%s] ", owner.c_str());
            for (const auto& m : members) std::printf("%s%s ", m.name.c_str(), m.function ? "()" : "");
            std::printf("\n");
        }
    }
    setLuaApiReference(reference);
    LuaProjectSymbols project;
    project.components = {{"Light", "Luz", {{"intensity", "Intensidad"}, {"color", "Color"}}},
                          {"XrOrigin", "XR Origin (VR)", {{"tracking", "Origen del seguimiento"}, {"camera_y_offset", "Altura"}}}};
    project.actions = {"Move", "Jump", "Fire"};
    project.contexts = {"Default", "Vuelo"};
    project.keys = {"W", "Space", "LeftShift"};
    project.tags = {"Player", "Enemy"};
    project.entities = {"Main Camera", "Jugador"};
    project.scenes = {"Nivel1", "Nivel2"};
    project.prefabs = {"Prefabs/Bala"};
    project.audio = {"Audio/disparo.wav"};
    setLuaProjectSymbols(project);

    const std::string head = "local J = { properties = { velocidad = 5, salto = 2, destino = Vec3(0, 1, 0) } }\nfunction J:Update(dt)\n    ";
    check(has(at(head + "Inp"), "Input"), "globales: Inp -> Input");
    check(has(at(head + "Input."), "getKey") && has(at(head + "Input.getA"), "getAxis"), "Input. -> getKey, getAxis");
    check(has(at(head + "self."), "velocidad") && has(at(head + "self."), "entity"), "self. -> propiedades y entity");
    check(has(at(head + "self.entity:"), "translate") && has(at(head + "self.entity:add"), "addForce"),
          "self.entity: -> metodos de Entity");
    check(has(at(head + "self.entity."), "position"), "self.entity. -> propiedades de Entity");
    check(has(at(head + "Vec3."), "up"), "Vec3. -> up");
    check(has(at("local C = {}\nfunction C:"), "OnCollisionEnter"), "function C: -> metodos del motor");
    check(at(head + "-- Inp").empty(), "nada dentro de un comentario");
    check(has(at(head + "fu"), "function"), "palabras clave");
    check(has(at(head + "Network."), "spawn") && has(at(head + "Network.on"), "onPlayerJoined"), "Network. -> spawn, onPlayerJoined");
    check(has(at(head + "Http."), "post") && has(at(head + "Json."), "decode"), "Http. y Json.");
    check(has(at(head + "res."), "data") && has(at(head + "contact."), "relativeVelocity"), "res. (Http) y contact.");
    check(has(at(head + "self.entity:"), "setIKTarget") && has(at(head + "self.entity:"), "isMine") &&
              has(at(head + "self.entity:"), "getField") && has(at(head + "self.entity."), "ragdoll"),
          "entidad: IK, red, getField y ragdoll");
    check(has(at(head + "self.entity.mesh:"), "apply") && has(at(head + "local malla = Mesh.new('a')\nmalla."), "vertices"),
          "mallas: mesh:apply, malla.vertices");
    check(has(at("local C = {}\nfunction C:"), "OnNetVar"), "function C: -> OnNetVar");
    check(has(at(head + "Graphics."), "foliage_visible") && has(at(head + "Voxel."), "blockCount"), "Graphics.foliage_*, Voxel.blockCount");
    const auto list = at(head + "Scene.f");
    check(!list.empty() && list[0].label.rfind("f", 0) == 0, "primero lo que empieza igual");

    std::printf("API del motor\n");
    check(reference.contains("XR") && reference.contains("Entity:") && reference.contains("Vec3:"), "el motor da XR, Entity: y Vec3:");
    check(has(at(head + "XR."), "getAimRay") && has(at(head + "XR."), "vibrate"), "XR. -> getAimRay, vibrate");
    check(has(at(head + "Screen."), "setOrientation"), "Screen. -> setOrientation");
    check(has(at(head + "X"), "XR") && has(at(head + "Scr"), "Screen"), "tablas nuevas entre las globales");
    check(has(at(head + "Input.getGamepad"), "getGamepadButton"), "Input.getGamepadButton");

    std::printf("Tipos\n");
    check(luaExpressionType(head, "self.entity.position") == "Vec3", "self.entity.position es Vec3");
    check(luaExpressionType(head, "Scene.find(\"a\")") == "Entity", "Scene.find(...) es Entity");
    check(luaExpressionType(head, "Quat.euler(0, 90, 0)") == "Quat", "Quat.euler(...) es Quat");
    check(luaExpressionType(head, "self.destino") == "Vec3", "propiedad Vec3 de self");
    check(luaExpressionType(head, "self.entity.position + Vec3.up * 2") == "Vec3", "Vec3 + Vec3 * n es Vec3");
    check(has(at(head + "local p = self.entity.position\n    p."), "x") && has(at(head + "local p = self.entity.position\n    p:"), "normalized"),
          "local p = posicion -> p. x, p:normalized");
    check(has(at(head + "local e = Scene.find('Jugador')\n    e:"), "addForce"), "local e = Scene.find -> e:addForce");
    check(has(at(head + "Scene.find('Jugador'):"), "lookAt") && has(at(head + "Scene.find('Jugador').position:"), "normalized"),
          "cadenas con llamadas: Scene.find(..):, .position:");
    check(has(at(head + "local hit = Physics.raycast(a, b, 10)\n    if hit then hit."), "entity") &&
              has(at(head + "local h = Physics.raycast(a, b)\n    h.entity:"), "destroy"),
          "Physics.raycast -> hit.entity, hit.entity:");
    check(has(at(head + "local q = XR.getHeadRotation()\n    q:"), "toEuler"), "XR.getHeadRotation() -> Quat");
    check(has(at(head + "for _, e in ipairs(Scene.findAllWithTag('Enemy')) do\n        e:"), "moveTo"),
          "for ... in ipairs(findAllWithTag) -> entidad");
    check(has(at("function J:OnCollisionEnter(other, contact)\n    other:"), "destroy"), "other: en OnCollisionEnter");
    check(!has(at(head + "local s = 'a' .."), "x"), "'..' no es un acceso");

    std::printf("Textos del proyecto\n");
    check(has(at(head + "self.entity:addComponent(\"L"), "Light"), "addComponent(\" -> componentes");
    check(has(at(head + "self.entity:getField(\"Light\", \"in"), "intensity"), "getField(\"Light\", \" -> campos");
    check(has(at(head + "self.entity:setField('XrOrigin', '"), "tracking"), "setField('XrOrigin', ' -> tracking");
    check(has(at(head + "Input.getAction(\""), "Jump") && has(at(head + "Input.isActionTriggered('F"), "Fire"),
          "acciones de entrada");
    check(has(at(head + "Input.addMappingContext(\""), "Vuelo"), "contextos");
    check(has(at(head + "Input.getKeyDown(\"Sp"), "Space"), "teclas");
    check(has(at(head + "Scene.find(\""), "Jugador") && has(at(head + "Scene.findWithTag(\""), "Enemy"), "objetos y tags");
    check(has(at(head + "Scene.load(\""), "Nivel2") && has(at(head + "Scene.instantiate(\""), "Prefabs/Bala"), "escenas y prefabs");
    check(has(at(head + "Audio.playOneShot(\""), "Audio/disparo.wav"), "sonidos");
    check(has(at(head + "XR.getButtonDown(\"right\", \""), "trigger") && has(at(head + "XR.vibrate(\""), "left"),
          "manos y botones de VR");
    check(has(at(head + "Input.bindAction(\"Jump\", \""), "started"), "eventos de bindAction");
    check(at(head + "print(\"hola ").empty(), "un texto cualquiera no sugiere nada");

    std::printf("Firma\n");
    LuaSignature sig;
    check(luaSignatureAt(head + "Input.getAction(", (head + "Input.getAction(").size(), sig) &&
              sig.label.find("getAction") != std::string::npos,
          "firma de Input.getAction");
    const std::string two = head + "self.entity:addForce(Vec3.up, ";
    check(luaSignatureAt(two, two.size(), sig) && sig.argument == 1, "segundo argumento de addForce");

    std::printf("\n%d fallos\n", failures);
    return failures == 0 ? 0 : 1;
}

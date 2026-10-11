// Pruebas de native/GraphicsApi.cpp: Graphics (con un host de mentira) y
// Graphics.post (el post-procesado global de la escena).

#include "ApiTest.h"

#include "CramionCore/ecs/Components.h"

#include <map>
#include <string>
#include <variant>

using namespace cramion;
using namespace cramion::apitest;
using scripting::GraphicsOption;
using scripting::GraphicsValue;

namespace {

// Un host de opciones como el del editor o el juego, sin ventana.
class FakeGraphicsHost final : public scripting::GraphicsHost {
public:
    FakeGraphicsHost() {
        values_["vsync"] = true;
        values_["texture_max_size"] = 2048.0;
        values_["shadow_quality"] = std::string("high");
        values_["save"] = std::string("opcion que se llama como una funcion");
        values_["gpu"] = std::string("Falsa 3000");  // solo lectura
    }

    std::vector<GraphicsOption> options() const override {
        std::vector<GraphicsOption> out;
        for (const auto& [key, value] : values_) {
            GraphicsOption o;
            o.key = key;
            o.value = value;
            o.writable = key != "gpu";
            o.description = "opcion " + key;
            if (key == "shadow_quality") o.choices = {"low", "medium", "high"};
            out.push_back(o);
        }
        return out;
    }
    bool set(const std::string& key, const GraphicsValue& value, std::string& error) override {
        const auto it = values_.find(key);
        if (it == values_.end()) {
            error = "no existe la opcion '" + key + "'";
            return false;
        }
        if (key == "gpu") {
            error = "es de solo lectura";
            return false;
        }
        if (it->second.index() != value.index()) {
            error = "tipo equivocado";
            return false;
        }
        it->second = value;
        quality_ = "Personalizada";
        return true;
    }
    std::vector<std::string> qualityLevels() const override { return {"Baja", "Media", "Alta", "Ultra"}; }
    bool setQuality(const std::string& level, std::string& error) override {
        for (const std::string& l : qualityLevels()) {
            if (l == level) {
                quality_ = level;
                return true;
            }
        }
        error = "no existe la calidad '" + level + "'";
        return false;
    }
    std::string quality() const override { return quality_; }
    std::vector<std::pair<int, int>> resolutions() const override { return {{1920, 1080}, {1280, 720}}; }
    bool save(std::string& error) override {
        ++saves;
        if (fail_save) {
            error = "disco lleno";
            return false;
        }
        return true;
    }

    std::map<std::string, GraphicsValue> values_;
    std::string quality_ = "Alta";
    int saves = 0;
    bool fail_save = false;
};

bool throwsGet(ApiFixture& t, const std::string& key) {
    try {
        t.get(key);
    } catch (const scripting::api::Error&) {
        return true;
    }
    return false;
}

std::size_t globalVolumes(ecs::World& world) {
    std::size_t n = 0;
    for (const entt::entity h : world.registry().view<ecs::PostProcessing>()) {
        if (world.registry().get<ecs::PostProcessing>(h).isGlobal()) ++n;
    }
    return n;
}

void testWithoutHost() {
    std::printf("Graphics sin host\n");
    ApiFixture t;
    check(!t.call("Graphics.setQuality", {Value("Alta")}).truthy() && t.logged("no permite cambiar"),
          "Graphics.setQuality sin host avisa y da false");
    t.log.clear();
    check(!t.call("Graphics.set", {Value("vsync"), Value(true)}).truthy() && t.logged("no permite cambiar"),
          "Graphics.set sin host avisa");
    t.log.clear();
    t.set("Graphics.vsync", Value(false));
    check(t.logged("no permite cambiar"), "Graphics.vsync = false sin host avisa");
    check(t.get("Graphics.vsync").isNil(), "Graphics.vsync sin host: nil");
    check(t.call("Graphics.get", {Value("vsync")}).isNil(), "Graphics.get sin host: nil");
    check(t.call("Graphics.getQuality").asString().empty(), "Graphics.getQuality sin host: vacio");
    check(t.call("Graphics.qualityLevels").size() == 0 && t.call("Graphics.resolutions").size() == 0 &&
              t.call("Graphics.options").size() == 0,
          "listas vacias sin host");
    check(t.call("Graphics.getAll").isObject() && t.call("Graphics.getAll").fields().empty(), "Graphics.getAll vacio");
    check(!t.call("Graphics.save").truthy(), "Graphics.save sin host: false");
}

void testHost() {
    std::printf("Graphics con host\n");
    ApiFixture t;
    FakeGraphicsHost host;
    t.scripts.setGraphics(&host);

    check(t.call("Graphics.get", {Value("vsync")}).truthy(), "Graphics.get(\"vsync\")");
    check(t.get("Graphics.texture_max_size").asNumber() == 2048, "Graphics.texture_max_size (dinamica)");
    t.set("Graphics.vsync", Value(false));
    check(std::get<bool>(host.values_["vsync"]) == false, "Graphics.vsync = false llega al host");
    check(t.call("Graphics.set", {Value("shadow_quality"), Value("low")}).truthy() &&
              std::get<std::string>(host.values_["shadow_quality"]) == "low",
          "Graphics.set(\"shadow_quality\", \"low\")");

    Value table = Value::object();
    table.set("vsync", Value(true));
    table.set("texture_max_size", Value(1024));
    check(t.call("Graphics.set", {table}).truthy() && std::get<bool>(host.values_["vsync"]) &&
              std::get<double>(host.values_["texture_max_size"]) == 1024.0,
          "Graphics.set{ vsync = true, texture_max_size = 1024 }");
    table.set("gpu", Value("otra"));
    t.log.clear();
    check(!t.call("Graphics.set", {table}).truthy() && t.logged("Graphics.gpu: es de solo lectura"),
          "Graphics.set{...} da false si una falla (y aplica las demas)");
    t.log.clear();
    check(!t.call("Graphics.set", {Value("vsync")}).truthy() && t.logged("Graphics.set: usa"),
          "Graphics.set sin valor avisa de como se usa");
    t.log.clear();
    check(!t.call("Graphics.set", {Value("vsync"), Value(core::Vec3{1, 2, 3})}).truthy() &&
              t.logged("el valor debe ser true/false"),
          "Graphics.set con un Vec3 avisa");
    t.log.clear();
    t.set("Graphics.nada", Value(1));
    check(t.logged("Graphics.nada: no existe"), "Graphics.nada = 1 avisa con el error del host");
    check(t.get("Graphics.nada").isNil(), "Graphics.nada sin opcion: nil");

    const Value all = t.call("Graphics.getAll");
    check(all.isObject() && all["shadow_quality"].asString() == "low" && all["vsync"].truthy(), "Graphics.getAll");
    const Value options = t.call("Graphics.options");
    bool found_choices = false;
    for (const Value& o : options.items()) {
        if (o["key"].asString() == "shadow_quality") {
            found_choices = o["choices"].size() == 3 && o["choices"][0].asString() == "low" && o["writable"].truthy() &&
                            o["description"].asString() == "opcion shadow_quality";
        }
        if (o["key"].asString() == "gpu" && o["writable"].truthy()) found_choices = false;
    }
    check(options.size() == host.values_.size() && found_choices, "Graphics.options con choices y writable");

    // Calidades rapidas.
    const Value levels = t.call("Graphics.qualityLevels");
    check(levels.size() == 4 && levels[0].asString() == "Baja", "Graphics.qualityLevels");
    check(t.call("Graphics.setQuality", {Value(0)}).truthy() && host.quality_ == "Baja", "Graphics.setQuality(0)");
    check(t.call("Graphics.setQuality", {Value("Ultra")}).truthy() && t.call("Graphics.getQuality").asString() == "Ultra",
          "Graphics.setQuality(\"Ultra\") / getQuality");
    t.log.clear();
    check(!t.call("Graphics.setQuality", {Value(9)}).truthy() && t.logged("Graphics.setQuality: calidad desconocida"),
          "Graphics.setQuality(9) avisa");
    t.log.clear();
    check(!t.call("Graphics.setQuality", {Value("Epica")}).truthy() && t.logged("no existe la calidad 'Epica'"),
          "Graphics.setQuality con un nombre que no existe avisa");

    const Value res = t.call("Graphics.resolutions");
    check(res.size() == 2 && res[0]["width"].asNumber() == 1920 && res[1]["height"].asNumber() == 720,
          "Graphics.resolutions");

    // Lo registrado manda sobre las opciones con el mismo nombre.
    check(t.call("Graphics.save").truthy() && host.saves == 1, "Graphics.save llama al host");
    check(throwsGet(t, "Graphics.save"), "Graphics.save no es la opcion \"save\" del host");
    host.fail_save = true;
    t.log.clear();
    check(!t.call("Graphics.save").truthy() && t.logged("Graphics.save: disco lleno"), "Graphics.save que falla avisa");

    // Por el puente de los scripts de C++.
    json r = t.bridge({{"op", "set"}, {"fn", "Graphics.vsync"}, {"value", false}});
    check(r["ok"] == true && std::get<bool>(host.values_["vsync"]) == false, "Graphics.vsync = false por el puente");
    r = t.bridge({{"op", "get"}, {"fn", "Graphics.shadow_quality"}});
    check(r["ok"] == true && r["result"] == "low", "Graphics.shadow_quality por el puente");
    r = t.bridge({{"fn", "Graphics.set"}, {"args", {json{{"vsync", true}}}}});
    check(r["ok"] == true && r["result"] == true && std::get<bool>(host.values_["vsync"]),
          "Graphics.set{...} por el puente");
    t.scripts.setGraphics(nullptr);
}

void testPost() {
    std::printf("Graphics.post\n");
    ApiFixture t;
    // Sin volumen: los valores por defecto (y no se crea nada al leer).
    check(t.get("Graphics.post.bloom").isBool(), "Graphics.post.bloom sin volumen: el valor por defecto");
    check(t.call("Graphics.getPost", {Value("exposure_compensation")}).isNumber(),
          "Graphics.getPost sin volumen");
    check(globalVolumes(t.world) == 0, "leer no crea el volumen");
    const Value keys = t.call("Graphics.postKeys");
    bool has_bloom = false, has_shape = false;
    for (const Value& k : keys.items()) {
        has_bloom = has_bloom || k.asString() == "bloom";
        has_shape = has_shape || k.asString() == "shape" || k.asString() == "priority";
    }
    check(keys.size() > 10 && has_bloom && !has_shape, "Graphics.postKeys (sin las opciones del volumen)");

    // Al cambiar algo se crea el volumen global.
    t.set("Graphics.post.bloom", Value(false));
    check(globalVolumes(t.world) == 1, "Graphics.post.bloom = false crea el volumen global");
    check(!t.get("Graphics.post.bloom").truthy(), "Graphics.post.bloom vale false");
    check(t.call("Graphics.setPost", {Value("bloom"), Value(1)}).truthy() && t.get("Graphics.post.bloom").truthy(),
          "Graphics.setPost(\"bloom\", 1): un numero vale para una casilla");
    check(t.call("Graphics.setPost", {Value("tonemapper"), Value("ACES")}).truthy() &&
              t.call("Graphics.getPost", {Value("tonemapper")}).asString() == "ACES",
          "Graphics.setPost de una enumeracion por su nombre");
    check(t.call("Graphics.setPost", {Value("bloom_tint"), Value(core::Vec3{1, 0.5f, 0.25f})}).truthy() &&
              t.get("Graphics.post.bloom_tint").asVec3().y == 0.5f,
          "Graphics.post de un Vec3");
    t.log.clear();
    check(!t.call("Graphics.setPost", {Value("nada"), Value(1)}).truthy() && t.logged("no existe 'nada'"),
          "Graphics.setPost de una clave que no existe avisa");
    t.log.clear();
    check(!t.call("Graphics.setPost", {Value("tonemapper"), Value("Rosa")}).truthy() &&
              t.logged("'Rosa' no es un valor de tonemapper"),
          "Graphics.setPost con un valor que no es de la enumeracion avisa");
    t.log.clear();
    t.set("Graphics.post.bloom", Value(Value::Array{Value(1)}));
    check(t.logged("Graphics.post.bloom: valor no valido"), "Graphics.post con una lista avisa");
    check(t.get("Graphics.post.nada").isNil(), "Graphics.post.nada: nil");

    // Graphics.post entero: clave -> valor.
    const Value post = t.get("Graphics.post");
    check(post.isObject() && post["bloom"].truthy() && post["tonemapper"].asString() == "ACES",
          "Graphics.post: el volumen como objeto");

    // Manda el global de mayor prioridad.
    ecs::Entity high = t.world.create("Post alto");
    ecs::PostProcessing& p = high.add<ecs::PostProcessing>();
    p.priority = 10;
    p.settings.bloom = false;
    check(!t.get("Graphics.post.bloom").truthy(), "lee el volumen global de mayor prioridad");
    t.set("Graphics.post.bloom", Value(true));
    check(p.settings.bloom && globalVolumes(t.world) == 2, "y cambia ese volumen");

    json r = t.bridge({{"op", "set"}, {"fn", "Graphics.post.vignette"}, {"value", true}});
    check(r["ok"] == true && p.settings.vignette, "Graphics.post.vignette = true por el puente");
    r = t.bridge({{"op", "get"}, {"fn", "Graphics.post.vignette"}});
    check(r["ok"] == true && r["result"] == true, "Graphics.post.vignette por el puente");
}

}  // namespace

int main() {
    testWithoutHost();
    testHost();
    testPost();
    return finish();
}

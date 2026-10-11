// Graphics: la configuracion grafica (QualitySettings + Screen de Unity) y
// el post-procesado global de la escena.
//   Graphics.setQuality("Baja")          calidades rapidas
//   Graphics.vsync = true                 cualquier opcion por su clave
//   Graphics.set{ textures = 2048, shadows = true }
//   Graphics.post.bloom = false           post-procesado global de la escena
//   Graphics.save()                       el juego la recupera al abrirse
// Las opciones las da el host (setGraphics); sin host las funciones avisan y
// no hacen nada. Graphics.post funciona siempre: es de la escena.

#include "Modules.h"

#include "CramionCore/ecs/Components.h"
#include "CramionCore/ecs/Reflection.h"

#include <algorithm>
#include <cmath>

#include "../ComponentFields.h"

namespace cramion::scripting::native {
namespace {

constexpr const char* kNoHost = "Graphics: este programa no permite cambiar la configuracion grafica";

// --- Post-procesado global ---

// Volumen de post-procesado global que manda (el de mayor prioridad). Si la
// escena no tiene, se crea uno al cambiar algo.
ecs::Entity globalPostVolume(Runtime& rt, bool create) {
    if (rt.world == nullptr) return {};
    ecs::World& world = *rt.world;
    ecs::Entity best;
    int best_priority = 0;
    for (const entt::entity h : world.registry().view<ecs::PostProcessing>()) {
        const ecs::PostProcessing& p = world.registry().get<ecs::PostProcessing>(h);
        if (!p.isGlobal()) continue;
        if (!best.valid() || p.priority > best_priority) {
            best = world.wrap(h);
            best_priority = p.priority;
        }
    }
    if (!best.valid() && create) {
        best = world.create("Post-procesado global");
        best.add<ecs::PostProcessing>();
    }
    return best;
}

bool reflectPost(Runtime& rt, ecs::Entity e, PostFieldVisitor& visitor) {
    const ecs::ComponentType* type = ecs::ComponentRegistry::instance().find("PostProcessing");
    if (type == nullptr || !e.valid() || rt.world == nullptr) return false;
    return type->reflect(*rt.world, e.handle(), visitor);
}

// Los valores por defecto del motor (sin volumen en la escena): los de un
// volumen nuevo.
void reflectDefaultPost(PostFieldVisitor& visitor) {
    ecs::World scratch;
    ecs::Entity temp = scratch.create("temp");
    temp.add<ecs::PostProcessing>();
    if (const ecs::ComponentType* type = ecs::ComponentRegistry::instance().find("PostProcessing")) {
        type->reflect(scratch, temp.handle(), visitor);
    }
}

// Recorre el volumen global (o los valores por defecto si no hay).
void visitCurrentPost(Runtime& rt, PostFieldVisitor& visitor) {
    const ecs::Entity volume = globalPostVolume(rt, false);
    if (volume.valid()) {
        reflectPost(rt, volume, visitor);
    } else {
        reflectDefaultPost(visitor);
    }
}

api::Value postToValue(const PostValue& v) {
    switch (v.type) {
        case PostValue::Type::Bool: return v.flag;
        case PostValue::Type::Number: return v.number;
        case PostValue::Type::Text: return v.text;
        case PostValue::Type::Vector: return v.vector;
        case PostValue::Type::None: break;
    }
    return {};
}

// Una entidad vale por su UUID (campos que guardan una referencia) y nil
// deja el texto vacio.
bool postFromValue(Runtime& rt, const api::Value& o, PostValue& out) {
    switch (o.type()) {
        case api::Value::Type::Bool:
            out.type = PostValue::Type::Bool;
            out.flag = o.truthy();
            return true;
        case api::Value::Type::Number:
            out.type = PostValue::Type::Number;
            out.number = o.asNumber();
            return true;
        case api::Value::Type::String:
            out.type = PostValue::Type::Text;
            out.text = o.asString();
            return true;
        case api::Value::Type::Vec3:
            out.type = PostValue::Type::Vector;
            out.vector = o.asVec3();
            return true;
        case api::Value::Type::Entity: {
            const ecs::Entity x = rt.entity(o.asEntity());
            out.type = PostValue::Type::Text;
            out.text = x.valid() ? x.uuid().toString() : std::string();
            return true;
        }
        case api::Value::Type::Nil:
            out.type = PostValue::Type::Text;
            out.text.clear();
            return true;
        default: return false;
    }
}

bool setPostField(Runtime& rt, const std::string& key, const api::Value& value) {
    PostValue v;
    if (!postFromValue(rt, value, v)) {
        rt.write(1, "Graphics.post." + key + ": valor no valido");
        return false;
    }
    const ecs::Entity volume = globalPostVolume(rt, true);
    if (!volume.valid()) return false;
    PostFieldVisitor visitor(PostFieldVisitor::Mode::Set, key, v);
    reflectPost(rt, volume, visitor);
    if (!visitor.found()) {
        rt.write(1, "Graphics.post: no existe '" + key + "' (mira Graphics.postKeys())");
        return false;
    }
    if (!visitor.error().empty()) {
        rt.write(1, "Graphics.post." + key + ": " + visitor.error());
        return false;
    }
    return true;
}

api::Value getPostField(Runtime& rt, const std::string& key) {
    PostFieldVisitor visitor(PostFieldVisitor::Mode::Get, key);
    visitCurrentPost(rt, visitor);
    return visitor.found() ? postToValue(visitor.value()) : api::Value{};
}

// --- Opciones del host ---

api::Value graphicsToValue(const GraphicsValue& v) {
    if (const bool* b = std::get_if<bool>(&v)) return *b;
    if (const double* d = std::get_if<double>(&v)) return *d;
    return std::get<std::string>(v);
}

bool graphicsFromValue(const api::Value& o, GraphicsValue& out) {
    switch (o.type()) {
        case api::Value::Type::Bool: out = o.truthy(); return true;
        case api::Value::Type::Number: out = o.asNumber(); return true;
        case api::Value::Type::String: out = o.asString(); return true;
        default: return false;
    }
}

// Graphics.set / Graphics.<clave> = valor. Devuelve si se aplico.
bool setGraphicsOption(Runtime& rt, const std::string& key, const api::Value& value) {
    if (rt.graphics == nullptr) {
        rt.write(1, kNoHost);
        return false;
    }
    GraphicsValue v;
    if (!graphicsFromValue(value, v)) {
        rt.write(1, "Graphics." + key + ": el valor debe ser true/false, un numero o un texto");
        return false;
    }
    std::string error;
    if (!rt.graphics->set(key, v, error)) {
        rt.write(1, "Graphics." + key + ": " + error);
        return false;
    }
    return true;
}

api::Value getGraphicsOption(Runtime& rt, const std::string& key) {
    if (rt.graphics == nullptr) return {};
    for (const GraphicsOption& o : rt.graphics->options()) {
        if (o.key == key) return graphicsToValue(o.value);
    }
    return {};
}

}  // namespace

void registerGraphicsApi(Runtime& rt) {
    api::NativeApi& n = rt.native;

    n.function("Graphics.get", [&rt](api::Call& c) { return getGraphicsOption(rt, c.string(0)); },
               {"\"texture_quality\"", "valor de una opcion", "bool, numero o texto"});
    // Graphics.set("vsync", true) o Graphics.set{ vsync = true, textures = 2048 }.
    n.function("Graphics.set", [&rt](api::Call& c) -> api::Value {
        const api::Value& first = c.arg(0);
        if (first.isObject() || first.isArray()) {
            // Una lista no tiene claves de texto: no cambia nada.
            bool all = true;
            for (const auto& [key, value] : first.fields()) all = setGraphicsOption(rt, key, value) && all;
            return all;
        }
        if (!first.isString() || !c.has(1)) {
            rt.write(1, "Graphics.set: usa Graphics.set(\"clave\", valor) o Graphics.set{ clave = valor }");
            return false;
        }
        return setGraphicsOption(rt, first.asString(), c.arg(1));
    }, {"\"vsync\", true", "cambia una opcion; o Graphics.set{ clave = valor, ... }", "bool"});
    n.function("Graphics.getAll", [&rt](api::Call&) {
        api::Value t = api::Value::object();
        if (rt.graphics != nullptr) {
            for (const GraphicsOption& o : rt.graphics->options()) t.set(o.key, graphicsToValue(o.value));
        }
        return t;
    }, {"", "tabla clave -> valor", "objeto"});
    // Lista para montar un menu de opciones: clave, valor, si se puede
    // cambiar, descripcion y valores posibles.
    n.function("Graphics.options", [&rt](api::Call&) {
        api::Value::Array list;
        if (rt.graphics == nullptr) return api::Value(std::move(list));
        for (const GraphicsOption& o : rt.graphics->options()) {
            api::Value item = api::Value::object();
            item.set("key", o.key);
            item.set("value", graphicsToValue(o.value));
            item.set("writable", o.writable);
            item.set("description", o.description);
            if (!o.choices.empty()) {
                api::Value::Array choices;
                for (const std::string& choice : o.choices) choices.emplace_back(choice);
                item.set("choices", api::Value(std::move(choices)));
            }
            list.push_back(std::move(item));
        }
        return api::Value(std::move(list));
    }, {"", "lista {key, value, writable, description, choices} para un menu", "lista de objetos"});
    n.function("Graphics.setQuality", [&rt](api::Call& c) -> api::Value {
        if (rt.graphics == nullptr) {
            rt.write(1, kNoHost);
            return false;
        }
        const api::Value& level = c.arg(0);
        std::string name;
        if (level.isNumber()) {
            // 0..3 como el QualitySettings.SetQualityLevel de Unity.
            const std::vector<std::string> levels = rt.graphics->qualityLevels();
            const int index = static_cast<int>(level.asNumber());
            if (index >= 0 && index < static_cast<int>(levels.size())) name = levels[static_cast<std::size_t>(index)];
        } else if (level.isString()) {
            name = level.asString();
        }
        std::string error;
        if (name.empty() || !rt.graphics->setQuality(name, error)) {
            rt.write(1, "Graphics.setQuality: " + (error.empty() ? std::string("calidad desconocida") : error));
            return false;
        }
        return true;
    }, {"\"Alta\"", "calidad rapida: Baja, Media, Alta, Ultra (o 0..3)", "bool"});
    n.function("Graphics.getQuality", [&rt](api::Call&) -> api::Value {
        return rt.graphics != nullptr ? rt.graphics->quality() : std::string();
    }, {"", "la ultima calidad rapida o Personalizada", "texto"});
    n.function("Graphics.qualityLevels", [&rt](api::Call&) {
        api::Value::Array t;
        if (rt.graphics != nullptr) {
            for (const std::string& level : rt.graphics->qualityLevels()) t.emplace_back(level);
        }
        return api::Value(std::move(t));
    }, {"", "lista de calidades", "lista de texto"});
    n.function("Graphics.resolutions", [&rt](api::Call&) {
        api::Value::Array t;
        if (rt.graphics != nullptr) {
            for (const auto& [w, h] : rt.graphics->resolutions()) {
                api::Value r = api::Value::object();
                r.set("width", w);
                r.set("height", h);
                t.push_back(std::move(r));
            }
        }
        return api::Value(std::move(t));
    }, {"", "resoluciones del monitor {width, height}", "lista de {width, height}"});
    n.function("Graphics.save", [&rt](api::Call&) -> api::Value {
        if (rt.graphics == nullptr) return false;
        std::string error;
        if (!rt.graphics->save(error)) {
            if (!error.empty()) rt.write(1, "Graphics.save: " + error);
            return false;
        }
        return true;
    }, {"", "guarda la configuracion del jugador (juego exportado)", "bool"});

    // Post-procesado global: Graphics.post.bloom = false,
    // Graphics.getPost("exposure_compensation"), Graphics.postKeys().
    n.function("Graphics.getPost", [&rt](api::Call& c) { return getPostField(rt, c.string(0)); },
               {"\"bloom\"", "campo del post-procesado global", "bool, numero, texto o Vec3"});
    n.function("Graphics.setPost", [&rt](api::Call& c) -> api::Value {
        return setPostField(rt, c.string(0), c.arg(1));
    }, {"\"bloom\", false", "cambia el post-procesado global", "bool"});
    n.function("Graphics.postKeys", [](api::Call&) {
        // Las claves son las mismas en todos los volumenes: las de uno nuevo.
        PostFieldVisitor visitor(PostFieldVisitor::Mode::Collect);
        reflectDefaultPost(visitor);
        api::Value::Array t;
        for (const auto& field : visitor.fields()) t.emplace_back(field.first);
        return api::Value(std::move(t));
    }, {"", "claves del post-procesado", "lista de texto"});
    // Graphics.post entero: clave -> valor del volumen global (o los valores
    // por defecto). Sus campos se leen y cambian por su clave (abajo).
    n.property("Graphics", "post", [&rt](api::Call&) {
        PostFieldVisitor visitor(PostFieldVisitor::Mode::Collect);
        visitCurrentPost(rt, visitor);
        api::Value t = api::Value::object();
        for (const auto& [key, value] : visitor.fields()) t.set(key, postToValue(value));
        return t;
    }, {}, {"", "post-procesado global: Graphics.post.bloom = false", "objeto"});
    rt.native.dynamicProperties(
        "Graphics.post", [&rt](api::Call& c) { return getPostField(rt, c.string(0)); },
        [&rt](api::Call& c) {
            setPostField(rt, c.string(0), c.arg(1));
            return api::Value{};
        });

    // Graphics.vsync, Graphics.textures = 2048...: las claves que no son
    // funciones van a las opciones del host (lo registrado arriba manda).
    rt.native.dynamicProperties(
        "Graphics", [&rt](api::Call& c) { return getGraphicsOption(rt, c.string(0)); },
        [&rt](api::Call& c) {
            setGraphicsOption(rt, c.string(0), c.arg(1));
            return api::Value{};
        });
}

}  // namespace cramion::scripting::native

#include "CramionCore/project/TouchInterface.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

namespace cramion::project {

using json = nlohmann::json;

dm::TouchLayout loadTouchInterface(const std::filesystem::path& file) {
    dm::TouchLayout l = dm::defaultTouchLayout();
    std::ifstream in(file, std::ios::binary);
    if (!in) return l;
    try {
        const json t = json::parse(in);
        l.enabled = t.value("enabled", l.enabled);
        l.joystick = t.value("joystick", l.joystick);
        l.look = t.value("look", l.look);
        l.look_sensitivity = std::clamp(t.value("sensitivity", l.look_sensitivity), 0.05f, 10.0f);
        l.scale = std::clamp(t.value("scale", l.scale), 0.3f, 3.0f);
        l.opacity = std::clamp(t.value("opacity", l.opacity), 0.0f, 1.0f);
        l.tap_clicks = t.value("tap_clicks", l.tap_clicks);
        if (const auto list = t.find("buttons"); list != t.end() && list->is_array()) {
            l.buttons.clear();
            for (const json& bj : *list) {
                dm::TouchButton b;
                b.label = bj.value("label", b.label);
                b.action = bj.value("action", b.action);
                b.x = std::clamp(bj.value("x", b.x), 0.0f, 1.0f);
                b.y = std::clamp(bj.value("y", b.y), 0.0f, 1.0f);
                b.size = std::clamp(bj.value("size", b.size), 0.3f, 3.0f);
                b.visible = bj.value("visible", b.visible);
                l.buttons.push_back(b);
            }
        }
    } catch (const std::exception&) {
        return dm::defaultTouchLayout();
    }
    return l;
}

bool saveTouchInterface(const std::filesystem::path& file, const dm::TouchLayout& l) {
    json buttons = json::array();
    for (const dm::TouchButton& b : l.buttons) {
        buttons.push_back({{"label", b.label}, {"action", b.action}, {"x", b.x}, {"y", b.y}, {"size", b.size},
                           {"visible", b.visible}});
    }
    const json root = {{"format", "CramionTouchInterface"},
                       {"version", 1},
                       {"enabled", l.enabled},
                       {"joystick", l.joystick},
                       {"look", l.look},
                       {"sensitivity", l.look_sensitivity},
                       {"scale", l.scale},
                       {"opacity", l.opacity},
                       {"tap_clicks", l.tap_clicks},
                       {"buttons", buttons}};
    std::error_code e;
    std::filesystem::create_directories(file.parent_path(), e);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << root.dump(2);
    return static_cast<bool>(out);
}

}  // namespace cramion::project

#include "CramionCore/gameplay/Accessibility.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace cramion::gameplay {

namespace {
AccessibilitySettings g_settings;
std::filesystem::path g_file;
}  // namespace

AccessibilitySettings& accessibility() { return g_settings; }

const char* colorblindModeName(int mode) {
    switch (mode) {
        case 1: return "Protanopia (rojo)";
        case 2: return "Deuteranopia (verde)";
        case 3: return "Tritanopia (azul)";
        case 4: return "Acromatopsia (sin color)";
        default: return "Ninguno";
    }
}

void setAccessibilityFile(const std::filesystem::path& file) { g_file = file; }
const std::filesystem::path& accessibilityFile() { return g_file; }

std::string accessibilityToJson(const AccessibilitySettings& s) {
    nlohmann::json j;
    j["colorblind_mode"] = s.colorblind_mode;
    j["colorblind_strength"] = s.colorblind_strength;
    j["colorblind_correct"] = s.colorblind_correct;
    j["text_scale"] = s.text_scale;
    j["subtitle_scale"] = s.subtitle_scale;
    j["subtitle_background"] = s.subtitle_background;
    j["reduce_motion"] = s.reduce_motion;
    j["camera_shake"] = s.camera_shake;
    j["high_contrast_ui"] = s.high_contrast_ui;
    return j.dump(2);
}

bool accessibilityFromJson(const std::string& text, AccessibilitySettings& s) {
    const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;
    s.colorblind_mode = std::clamp(j.value("colorblind_mode", s.colorblind_mode), 0, 4);
    s.colorblind_strength = std::clamp(j.value("colorblind_strength", s.colorblind_strength), 0.0f, 1.0f);
    s.colorblind_correct = j.value("colorblind_correct", s.colorblind_correct);
    s.text_scale = std::clamp(j.value("text_scale", s.text_scale), 0.5f, 3.0f);
    s.subtitle_scale = std::clamp(j.value("subtitle_scale", s.subtitle_scale), 0.5f, 3.0f);
    s.subtitle_background = j.value("subtitle_background", s.subtitle_background);
    s.reduce_motion = j.value("reduce_motion", s.reduce_motion);
    s.camera_shake = std::clamp(j.value("camera_shake", s.camera_shake), 0.0f, 2.0f);
    s.high_contrast_ui = j.value("high_contrast_ui", s.high_contrast_ui);
    return true;
}

bool loadAccessibility() {
    if (g_file.empty()) return false;
    std::ifstream in(g_file, std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    return accessibilityFromJson(ss.str(), g_settings);
}

bool saveAccessibility() {
    if (g_file.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(g_file.parent_path(), ec);
    std::ofstream out(g_file, std::ios::binary);
    if (!out) return false;
    out << accessibilityToJson(g_settings);
    return static_cast<bool>(out);
}

namespace {
struct Shake {
    float trauma = 0.0f;
    float decay = 2.0f;  // trauma por segundo
    float frequency = 18.0f;
    float time = 0.0f;
} g_shake;

// Ruido suave (suma de senos con fases distintas por canal), -1..1.
float wave(float t, float seed) {
    return (std::sin(t * 1.0f + seed) * 0.5f + std::sin(t * 2.31f + seed * 1.7f) * 0.3f +
            std::sin(t * 4.79f + seed * 2.9f) * 0.2f);
}
}  // namespace

void addCameraShake(float intensity, float duration, float frequency) {
    g_shake.trauma = std::clamp(g_shake.trauma + std::max(intensity, 0.0f), 0.0f, 1.0f);
    g_shake.decay = 1.0f / std::max(duration, 0.05f);
    g_shake.frequency = std::clamp(frequency, 1.0f, 60.0f);
}

void clearCameraShake() { g_shake.trauma = 0.0f; }

bool cameraShakeOffset(float dt, core::Vec3& position, core::Vec3& rotation) {
    position = core::Vec3{};
    rotation = core::Vec3{};
    if (g_shake.trauma <= 0.0f) return false;
    g_shake.time += std::max(dt, 0.0f) * g_shake.frequency;
    float amount = g_shake.trauma * g_shake.trauma;  // se nota poco con poco trauma
    amount *= std::clamp(g_settings.camera_shake, 0.0f, 2.0f) * (g_settings.reduce_motion ? 0.5f : 1.0f);
    const float t = g_shake.time;
    position = core::Vec3{wave(t, 1.3f), wave(t, 7.1f), 0.0f} * (0.12f * amount);
    rotation = core::Vec3{wave(t, 3.7f) * 2.5f, wave(t, 5.3f) * 2.5f, wave(t, 9.2f) * 3.0f} * amount;
    g_shake.trauma = std::max(g_shake.trauma - g_shake.decay * std::max(dt, 0.0f), 0.0f);
    return amount > 0.0f;
}

}  // namespace cramion::gameplay

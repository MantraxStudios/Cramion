#include "CramionCore/ecs/Tags.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

namespace cramion::ecs {

namespace {

std::vector<std::string>& storage() {
    static std::vector<std::string> tags = defaultTags();
    return tags;
}

// Integradas primero, sin repetidos ni vacios.
std::vector<std::string> normalized(const std::vector<std::string>& tags) {
    std::vector<std::string> result = defaultTags();
    for (const std::string& tag : tags) {
        if (!tag.empty() && std::find(result.begin(), result.end(), tag) == result.end()) result.push_back(tag);
    }
    return result;
}

}  // namespace

std::vector<std::string> defaultTags() {
    return {kUntagged, "Respawn", "Finish", "EditorOnly", "MainCamera", "Player", "GameController"};
}

bool isBuiltinTag(std::string_view tag) {
    for (const std::string& builtin : defaultTags()) {
        if (builtin == tag) return true;
    }
    return false;
}

const std::vector<std::string>& projectTags() {
    return storage();
}

void setProjectTags(std::vector<std::string> tags) {
    storage() = normalized(tags);
}

bool addProjectTag(const std::string& tag) {
    std::vector<std::string>& tags = storage();
    if (tag.empty() || std::find(tags.begin(), tags.end(), tag) != tags.end()) return false;
    tags.push_back(tag);
    return true;
}

bool loadTags(const std::filesystem::path& file, std::vector<std::string>& tags) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    try {
        const nlohmann::json json = nlohmann::json::parse(in);
        std::vector<std::string> loaded;
        if (const auto list = json.find("tags"); list != json.end() && list->is_array()) {
            for (const auto& t : *list) {
                if (t.is_string()) loaded.push_back(t.get<std::string>());
            }
        }
        tags = normalized(loaded);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool saveTags(const std::filesystem::path& file, const std::vector<std::string>& tags) {
    nlohmann::json json;
    json["tags"] = normalized(tags);
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << json.dump(2);
    return static_cast<bool>(out);
}

}  // namespace cramion::ecs

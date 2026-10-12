#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>

namespace taskbar_config {

enum class Profile {
    neutral,
    glass,
    rose,
    midnight,
    mint,
    custom,
};

enum class Easing {
    linear,
    ease_out,
    ease_in_out,
};

struct Selector {
    std::string semantic;
    std::string name;
    std::string type;
    std::string ancestor_name;
    std::vector<Selector> parents;
    nlohmann::json where = nlohmann::json::object();
    uint32_t index = 0;
    bool root = false;
    bool exact_type = false;
};

struct Animation {
    Selector selector;
    double hover_scale = 1.0;
    double press_scale = 1.0;
    double hover_opacity = 1.0;
    double press_opacity = 1.0;
    uint32_t hover_duration_ms = 180;
    uint32_t press_duration_ms = 90;
    Easing easing = Easing::ease_out;
};

struct Rule {
    Selector selector;
    nlohmann::json properties = nlohmann::json::object();
    std::string state_group;
    std::string state;
    bool enabled = true;
};

struct Config {
    bool enabled = true;
    Profile profile = Profile::glass;

    std::string surface_color = "#1B2330FF";
    double surface_opacity = 0.94;
    double surface_radius = 14.0;
    double surface_inset_horizontal = 0.0;
    double surface_inset_vertical = 0.0;
    std::string surface_width_mode = "native";
    double surface_width = 960.0;
    double surface_height = 0.0;
    std::string surface_alignment = "native";
    double surface_offset_x = 0.0;
    double surface_offset_y = 0.0;

    std::string button_background_color = "#00000000";
    std::string button_foreground_color = "#FFFFFFFF";
    double button_corner_radius = 8.0;
    double button_padding_horizontal = 6.0;
    double button_padding_vertical = 4.0;
    double button_spacing = 2.0;
    double button_width = 0.0;
    double button_height = 0.0;
    double button_rotation_degrees = 0.0;
    double button_scale = 1.0;
    double button_hover_scale = 1.06;
    double button_press_scale = 0.96;
    double button_hover_opacity = 1.0;
    double button_press_opacity = 0.92;
    uint32_t button_hover_duration_ms = 180;
    uint32_t button_press_duration_ms = 90;
    Easing button_easing = Easing::ease_out;

    std::string start_background_color = "#00000000";
    std::string start_foreground_color = "#FFFFFFFF";
    double start_scale = 1.0;
    double start_hover_scale = 1.08;
    uint32_t start_animation_duration_ms = 180;

    std::string tray_background_color = "#00000000";
    std::string tray_foreground_color = "#FFFFFFFF";
    double tray_corner_radius = 8.0;
    double tray_padding_horizontal = 5.0;
    double tray_padding_vertical = 3.0;
    double tray_spacing = 2.0;
    double tray_scale = 1.0;
    double tray_hover_scale = 1.05;
    uint32_t tray_animation_duration_ms = 160;

    std::string indicator_color = "#65C8FFFF";
    double indicator_thickness = 2.0;
    double indicator_radius = 2.0;
    bool indicator_visible = true;
    nlohmann::json variables = nlohmann::json::object();
    nlohmann::json captures = nlohmann::json::array();
    nlohmann::json resources = nlohmann::json::object();
    std::vector<Rule> rules;
    std::vector<Animation> animations;
};

struct ParseResult {
    Config config;
    std::string error;

    explicit operator bool() const noexcept { return error.empty(); }
};

inline constexpr std::size_t kMaximumRules = 256;
inline constexpr std::size_t kMaximumAnimations = 128;

ParseResult ParseSettings(const char* snapshot);
ParseResult ParseSettingsJson(const nlohmann::json& snapshot);
bool ValidPropertyValue(std::string_view name, const nlohmann::json& value);
std::string XamlPropertyName(std::string_view name);
std::string XamlValueMarkup(const nlohmann::json& value, bool color_value = false);
std::string XmlEscape(std::string_view text);
nlohmann::json EvaluateExpression(std::string_view expression, const nlohmann::json& variables);
Selector ParseSelectorJson(const nlohmann::json& value);

}  // namespace taskbar_config

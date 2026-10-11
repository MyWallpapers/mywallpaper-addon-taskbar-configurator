#include "taskbar_config.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string_view>

namespace taskbar_config {
namespace {
using Json = nlohmann::json;

bool IsFiniteNumber(const Json& value) {
    if (!value.is_number()) return false;
    const double number = value.get<double>();
    return std::isfinite(number);
}

bool ReadNumber(const Json& object, const char* key, double minimum,
    double maximum, double* output, std::string* error) {
    const auto found = object.find(key);
    if (found == object.end()) return true;
    if (!IsFiniteNumber(*found)) {
        *error = std::string("'") + key + "' must be a finite number";
        return false;
    }
    const double number = found->get<double>();
    if (number < minimum || number > maximum) {
        *error = std::string("'") + key + "' is outside its supported range";
        return false;
    }
    *output = number;
    return true;
}

bool ReadInteger(const Json& object, const char* key, int64_t minimum,
    int64_t maximum, uint32_t* output, std::string* error) {
    const auto found = object.find(key);
    if (found == object.end()) return true;
    if (!found->is_number_integer()) {
        *error = std::string("'") + key + "' must be an integer";
        return false;
    }
    int64_t number = 0;
    try {
        number = found->get<int64_t>();
    } catch (...) {
        *error = std::string("'") + key + "' is outside its supported range";
        return false;
    }
    if (number < minimum || number > maximum) {
        *error = std::string("'") + key + "' is outside its supported range";
        return false;
    }
    *output = static_cast<uint32_t>(number);
    return true;
}

bool ReadBoolean(const Json& object, const char* key, bool* output,
    std::string* error) {
    const auto found = object.find(key);
    if (found == object.end()) return true;
    if (!found->is_boolean()) {
        *error = std::string("'") + key + "' must be a boolean";
        return false;
    }
    *output = found->get<bool>();
    return true;
}

bool ReadString(const Json& object, const char* key, std::string* output,
    std::string* error) {
    const auto found = object.find(key);
    if (found == object.end()) return true;
    if (!found->is_string()) {
        *error = std::string("'") + key + "' must be a string";
        return false;
    }
    *output = found->get<std::string>();
    return true;
}

bool IsHexColor(std::string_view value) {
    if (value.size() != 7 && value.size() != 9) return false;
    if (value.front() != '#') return false;
    return std::all_of(value.begin() + 1, value.end(), [](char character) {
        return (character >= '0' && character <= '9') ||
            (character >= 'a' && character <= 'f') ||
            (character >= 'A' && character <= 'F');
    });
}

bool ReadColor(const Json& object, const char* key, std::string* output,
    std::string* error) {
    if (!ReadString(object, key, output, error)) return false;
    if (object.contains(key) && !IsHexColor(*output)) {
        *error = std::string("'") + key + "' must be #RRGGBB or #RRGGBBAA";
        return false;
    }
    return true;
}

bool ParseEasing(const Json& value, Easing* easing) {
    if (!value.is_string()) return false;
    const auto text = value.get<std::string>();
    if (text == "linear") *easing = Easing::linear;
    else if (text == "ease-out") *easing = Easing::ease_out;
    else if (text == "ease-in-out") *easing = Easing::ease_in_out;
    else return false;
    return true;
}

bool ReadEasing(const Json& object, const char* key, Easing* easing,
    std::string* error) {
    const auto found = object.find(key);
    if (found == object.end()) return true;
    if (!ParseEasing(*found, easing)) {
        *error = std::string("'") + key + "' must be linear, ease-out, or ease-in-out";
        return false;
    }
    return true;
}

bool ParseSelector(const Json& value, Selector* selector, std::string* error) {
    if (!value.is_object()) {
        *error = "selector must be an object";
        return false;
    }
    if (!ReadString(value, "semantic", &selector->semantic, error) ||
        !ReadString(value, "name", &selector->name, error) ||
        !ReadString(value, "type", &selector->type, error) ||
        !ReadString(value, "ancestorName", &selector->ancestor_name, error)) {
        return false;
    }
    if (selector->semantic.empty() && selector->name.empty() &&
        selector->type.empty()) {
        *error = "selector needs semantic, name, or type";
        return false;
    }
    if (!selector->semantic.empty() && selector->semantic != "surface" &&
        selector->semantic != "button" && selector->semantic != "startButton" &&
        selector->semantic != "systemTray" && selector->semantic != "indicator") {
        *error = "selector semantic must be surface, button, startButton, systemTray, or indicator";
        return false;
    }
    return true;
}

bool ValidPropertyValue(std::string_view name, const Json& value) {
    static constexpr std::array<std::string_view, 8> string_properties = {
        "background", "foreground", "fill", "stroke", "visibility",
        "horizontalAlignment", "verticalAlignment", "text"
    };
    static constexpr std::array<std::string_view, 12> number_properties = {
        "opacity", "cornerRadius", "strokeThickness", "width", "height",
        "fontSize", "rotation", "minWidth", "maxWidth", "minHeight",
        "maxHeight", "spacing"
    };
    static constexpr std::array<std::string_view, 2> bool_properties = {
        "isHitTestVisible", "isEnabled"
    };
    static constexpr std::array<std::string_view, 2> vector_properties = {
        "margin", "padding"
    };
    if (std::find(string_properties.begin(), string_properties.end(), name) != string_properties.end()) {
        if (!value.is_string()) return false;
        const auto text = value.get<std::string>();
        if (name == "visibility") return text == "visible" || text == "collapsed";
        if (name == "horizontalAlignment") return text == "left" || text == "center" || text == "right" || text == "stretch";
        if (name == "verticalAlignment") return text == "top" || text == "center" || text == "bottom" || text == "stretch";
        if (name == "background" || name == "foreground" || name == "fill" || name == "stroke") {
            if (IsHexColor(text)) return true;
            if (text == "transparent") return true;
            return false;
        }
        return true;
    }
    if (std::find(number_properties.begin(), number_properties.end(), name) != number_properties.end()) {
        if (!IsFiniteNumber(value)) return false;
        const double number = value.get<double>();
        if (name == "opacity") return number >= 0.0 && number <= 1.0;
        if (name == "rotation") return number >= -360.0 && number <= 360.0;
        if (name == "fontSize") return number >= 0.0 && number <= 256.0;
        if (name == "spacing" || name == "strokeThickness" || name == "cornerRadius")
            return number >= 0.0 && number <= 256.0;
        return number >= 0.0 && number <= 4096.0;
    }
    if (std::find(bool_properties.begin(), bool_properties.end(), name) != bool_properties.end()) {
        return value.is_boolean();
    }
    if (std::find(vector_properties.begin(), vector_properties.end(), name) != vector_properties.end()) {
        const double minimum = name == "margin" ? -512.0 : 0.0;
        if (IsFiniteNumber(value)) return value.get<double>() >= minimum && value.get<double>() <= 512;
        if (!value.is_array() || (value.size() != 2 && value.size() != 4)) return false;
        return std::all_of(value.begin(), value.end(), [minimum](const Json& component) {
            return IsFiniteNumber(component) && component.get<double>() >= minimum && component.get<double>() <= 512;
        });
    }
    return false;
}

bool ParseAdvanced(const Json& settings, Config* config, std::string* error) {
    const auto found = settings.find("advancedProfileJson");
    if (found == settings.end() || !found->is_string()) return found == settings.end();
    const std::string text = found->get<std::string>();
    if (text.empty()) return true;
    Json profile;
    try {
        profile = Json::parse(text);
    } catch (const std::exception& exception) {
        *error = std::string("advancedProfileJson is invalid JSON: ") + exception.what();
        return false;
    }
    if (!profile.is_object() || !profile.contains("schemaVersion") ||
        !profile["schemaVersion"].is_number_integer() || profile["schemaVersion"] != 1) {
        *error = "advancedProfileJson must be an object with schemaVersion 1";
        return false;
    }
    const auto rules = profile.find("rules");
    if (rules != profile.end()) {
        if (!rules->is_array() || rules->size() > kMaximumRules) {
            *error = "rules must be an array of at most 256 entries";
            return false;
        }
        config->rules.reserve(rules->size());
        for (const Json& item : *rules) {
            if (!item.is_object() || !item.contains("selector") || !item.contains("properties") ||
                !item["properties"].is_object() || item["properties"].empty()) {
                *error = "each rule needs a selector and a non-empty properties object";
                return false;
            }
            Rule rule;
            if (!ParseSelector(item["selector"], &rule.selector, error)) return false;
            for (auto property = item["properties"].begin(); property != item["properties"].end(); ++property) {
                if (!ValidPropertyValue(property.key(), property.value())) {
                    *error = std::string("unsupported value for property '") + property.key() + "'";
                    return false;
                }
            }
            rule.properties = item["properties"];
            config->rules.push_back(std::move(rule));
        }
    }
    const auto animations = profile.find("animations");
    if (animations != profile.end()) {
        if (!animations->is_array() || animations->size() > kMaximumAnimations) {
            *error = "animations must be an array of at most 128 entries";
            return false;
        }
        config->animations.reserve(animations->size());
        for (const Json& item : *animations) {
            if (!item.is_object() || !item.contains("selector")) {
                *error = "each animation needs a selector";
                return false;
            }
            Animation animation;
            if (!ParseSelector(item["selector"], &animation.selector, error) ||
                !ReadNumber(item, "hoverScale", 0.5, 1.5, &animation.hover_scale, error) ||
                !ReadNumber(item, "pressScale", 0.5, 1.5, &animation.press_scale, error) ||
                !ReadNumber(item, "hoverOpacity", 0.0, 1.0, &animation.hover_opacity, error) ||
                !ReadNumber(item, "pressOpacity", 0.0, 1.0, &animation.press_opacity, error) ||
                !ReadInteger(item, "hoverDurationMs", 0, 2000, &animation.hover_duration_ms, error) ||
                !ReadInteger(item, "pressDurationMs", 0, 2000, &animation.press_duration_ms, error) ||
                !ReadEasing(item, "easing", &animation.easing, error)) {
                return false;
            }
            config->animations.push_back(std::move(animation));
        }
    }
    return true;
}

void ApplyPreset(Config* config) {
    if (config->profile == Profile::custom) return;
    switch (config->profile) {
    case Profile::neutral:
        config->surface_color = "#202020FF";
        config->surface_opacity = 1.0;
        config->surface_radius = 0.0;
        config->surface_inset_horizontal = 0.0;
        config->surface_inset_vertical = 0.0;
        config->button_background_color = "#00000000";
        config->button_foreground_color = "#FFFFFFFF";
        config->button_corner_radius = 4.0;
        config->start_background_color = "#00000000";
        config->start_foreground_color = "#FFFFFFFF";
        config->tray_background_color = "#00000000";
        config->tray_foreground_color = "#FFFFFFFF";
        config->indicator_color = "#60CDFFFF";
        break;
    case Profile::glass:
        config->surface_color = "#1B2330FF";
        config->surface_opacity = 0.94;
        config->surface_radius = 14.0;
        config->button_background_color = "#00000000";
        config->button_foreground_color = "#FFFFFFFF";
        config->button_corner_radius = 8.0;
        config->start_background_color = "#00000000";
        config->start_foreground_color = "#FFFFFFFF";
        config->tray_background_color = "#00000000";
        config->tray_foreground_color = "#FFFFFFFF";
        config->indicator_color = "#65C8FFFF";
        break;
    case Profile::rose:
        config->surface_color = "#7A1746FF";
        config->surface_opacity = 0.96;
        config->surface_radius = 18.0;
        config->surface_inset_horizontal = 0.0;
        config->surface_inset_vertical = 0.0;
        config->button_background_color = "#A52D68FF";
        config->button_foreground_color = "#FFF5FAFF";
        config->button_corner_radius = 12.0;
        config->start_background_color = "#C74382FF";
        config->start_foreground_color = "#FFFFFFFF";
        config->tray_background_color = "#A52D68FF";
        config->tray_foreground_color = "#FFF5FAFF";
        config->indicator_color = "#FFD1E6FF";
        break;
    case Profile::midnight:
        config->surface_color = "#11151DFF";
        config->surface_opacity = 0.98;
        config->surface_radius = 10.0;
        config->surface_inset_horizontal = 0.0;
        config->surface_inset_vertical = 0.0;
        config->button_background_color = "#202838FF";
        config->button_foreground_color = "#E9F0FFFF";
        config->button_corner_radius = 7.0;
        config->start_background_color = "#263B5EFF";
        config->start_foreground_color = "#FFFFFFFF";
        config->tray_background_color = "#202838FF";
        config->tray_foreground_color = "#E9F0FFFF";
        config->indicator_color = "#82B7FFFF";
        break;
    case Profile::mint:
        config->surface_color = "#163C37FF";
        config->surface_opacity = 0.96;
        config->surface_radius = 16.0;
        config->surface_inset_horizontal = 0.0;
        config->surface_inset_vertical = 0.0;
        config->button_background_color = "#225E55FF";
        config->button_foreground_color = "#EFFFFBFF";
        config->button_corner_radius = 10.0;
        config->start_background_color = "#328B79FF";
        config->start_foreground_color = "#FFFFFFFF";
        config->tray_background_color = "#225E55FF";
        config->tray_foreground_color = "#EFFFFBFF";
        config->indicator_color = "#95F2D7FF";
        break;
    case Profile::custom:
        break;
    }
}

bool ParseDeviceSettings(const Json& settings, Config* config, std::string* error) {
    if (!settings.is_object()) {
        *error = "deviceSettings must be an object";
        return false;
    }
    if (!ReadBoolean(settings, "enabled", &config->enabled, error)) return false;
    std::string profile;
    if (!ReadString(settings, "profile", &profile, error)) return false;
    if (settings.contains("profile")) {
        if (profile == "neutral") config->profile = Profile::neutral;
        else if (profile == "glass") config->profile = Profile::glass;
        else if (profile == "rose") config->profile = Profile::rose;
        else if (profile == "midnight") config->profile = Profile::midnight;
        else if (profile == "mint") config->profile = Profile::mint;
        else if (profile == "custom") config->profile = Profile::custom;
        else { *error = "profile is not a supported preset"; return false; }
    }
    if (!ReadColor(settings, "surfaceColor", &config->surface_color, error) ||
        !ReadNumber(settings, "surfaceOpacity", 0.0, 1.0, &config->surface_opacity, error) ||
        !ReadNumber(settings, "surfaceRadius", 0.0, 64.0, &config->surface_radius, error) ||
        !ReadNumber(settings, "surfaceInsetHorizontal", -64.0, 64.0, &config->surface_inset_horizontal, error) ||
        !ReadNumber(settings, "surfaceInsetVertical", -64.0, 64.0, &config->surface_inset_vertical, error) ||
        !ReadColor(settings, "buttonBackgroundColor", &config->button_background_color, error) ||
        !ReadColor(settings, "buttonForegroundColor", &config->button_foreground_color, error) ||
        !ReadNumber(settings, "buttonCornerRadius", 0.0, 48.0, &config->button_corner_radius, error) ||
        !ReadNumber(settings, "buttonPaddingHorizontal", 0.0, 64.0, &config->button_padding_horizontal, error) ||
        !ReadNumber(settings, "buttonPaddingVertical", 0.0, 64.0, &config->button_padding_vertical, error) ||
        !ReadNumber(settings, "buttonSpacing", 0.0, 64.0, &config->button_spacing, error) ||
        !ReadNumber(settings, "buttonScale", 0.5, 1.5, &config->button_scale, error) ||
        !ReadNumber(settings, "buttonHoverScale", 0.5, 1.5, &config->button_hover_scale, error) ||
        !ReadNumber(settings, "buttonPressScale", 0.5, 1.5, &config->button_press_scale, error) ||
        !ReadNumber(settings, "buttonHoverOpacity", 0.0, 1.0, &config->button_hover_opacity, error) ||
        !ReadNumber(settings, "buttonPressOpacity", 0.0, 1.0, &config->button_press_opacity, error) ||
        !ReadInteger(settings, "buttonHoverDurationMs", 0, 2000, &config->button_hover_duration_ms, error) ||
        !ReadInteger(settings, "buttonPressDurationMs", 0, 2000, &config->button_press_duration_ms, error) ||
        !ReadEasing(settings, "buttonEasing", &config->button_easing, error) ||
        !ReadColor(settings, "startBackgroundColor", &config->start_background_color, error) ||
        !ReadColor(settings, "startForegroundColor", &config->start_foreground_color, error) ||
        !ReadNumber(settings, "startScale", 0.5, 1.5, &config->start_scale, error) ||
        !ReadNumber(settings, "startHoverScale", 0.5, 1.5, &config->start_hover_scale, error) ||
        !ReadInteger(settings, "startAnimationDurationMs", 0, 2000, &config->start_animation_duration_ms, error) ||
        !ReadColor(settings, "trayBackgroundColor", &config->tray_background_color, error) ||
        !ReadColor(settings, "trayForegroundColor", &config->tray_foreground_color, error) ||
        !ReadNumber(settings, "trayCornerRadius", 0.0, 48.0, &config->tray_corner_radius, error) ||
        !ReadNumber(settings, "trayPaddingHorizontal", 0.0, 64.0, &config->tray_padding_horizontal, error) ||
        !ReadNumber(settings, "trayPaddingVertical", 0.0, 64.0, &config->tray_padding_vertical, error) ||
        !ReadNumber(settings, "traySpacing", 0.0, 64.0, &config->tray_spacing, error) ||
        !ReadNumber(settings, "trayScale", 0.5, 1.5, &config->tray_scale, error) ||
        !ReadNumber(settings, "trayHoverScale", 0.5, 1.5, &config->tray_hover_scale, error) ||
        !ReadInteger(settings, "trayAnimationDurationMs", 0, 2000, &config->tray_animation_duration_ms, error) ||
        !ReadColor(settings, "indicatorColor", &config->indicator_color, error) ||
        !ReadNumber(settings, "indicatorThickness", 0.0, 16.0, &config->indicator_thickness, error) ||
        !ReadNumber(settings, "indicatorRadius", 0.0, 16.0, &config->indicator_radius, error)) {
        return false;
    }
    ApplyPreset(config);
    return ParseAdvanced(settings, config, error);
}
} // namespace

ParseResult ParseSettings(const char* snapshot) {
    if (!snapshot) return ParseResult{Config{}, "settings snapshot is null"};
    try {
        return ParseSettingsJson(Json::parse(snapshot));
    } catch (const std::exception& exception) {
        return ParseResult{Config{}, std::string("settings snapshot is invalid JSON: ") + exception.what()};
    }
}

ParseResult ParseSettingsJson(const nlohmann::json& snapshot) {
    if (!snapshot.is_object()) return ParseResult{Config{}, "settings snapshot must be an object"};
    const auto found = snapshot.find("deviceSettings");
    const nlohmann::json& settings = found == snapshot.end() ? snapshot : *found;
    Config config;
    std::string error;
    try {
        if (!ParseDeviceSettings(settings, &config, &error)) return ParseResult{Config{}, std::move(error)};
        return ParseResult{std::move(config), {}};
    } catch (const std::exception& exception) {
        return ParseResult{Config{}, std::string("settings values are invalid: ") + exception.what()};
    }
}

} // namespace taskbar_config

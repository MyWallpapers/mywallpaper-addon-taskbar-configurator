#include "taskbar_config.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
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

bool ParseSelector(const Json& value, Selector* selector, std::string* error, unsigned depth = 0) {
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
    if (depth > 16 || !ReadInteger(value, "index", 0, 4096, &selector->index, error) ||
        !ReadBoolean(value, "root", &selector->root, error) ||
        !ReadBoolean(value, "exactType", &selector->exact_type, error)) return false;
    if (value.contains("where")) {
        if (!value["where"].is_object() || value["where"].size() > 32) { *error = "where must be a property map of at most 32 entries"; return false; }
        for (const auto& item : value["where"].items()) {
            if (item.key().empty() || item.key().size() > 128 || !item.value().is_primitive()) {
                *error = "where compares named properties with primitive values";
                return false;
            }
        }
        selector->where = value["where"];
    }
    if (value.contains("parents")) {
        if (!value["parents"].is_array() || value["parents"].size() > 16) { *error = "parents must be an array of at most 16 selectors, nearest first"; return false; }
        for (const auto& parent : value["parents"]) { Selector parsed; if (!ParseSelector(parent, &parsed, error, depth + 1)) return false; selector->parents.push_back(std::move(parsed)); }
    }
    if (selector->semantic.empty() && selector->name.empty() &&
        selector->type.empty()) {
        *error = "selector needs semantic, name, or type";
        return false;
    }
    if (!selector->semantic.empty() && selector->semantic != "surface" &&
        selector->semantic != "button" && selector->semantic != "startButton" &&
        selector->semantic != "systemTray" && selector->semantic != "indicator" &&
        selector->semantic != "search" && selector->semantic != "widgets" &&
        selector->semantic != "clock" && selector->semantic != "icon" && selector->semantic != "label") {
        *error = "unknown semantic target";
        return false;
    }
    return true;
}

bool LegacyPropertyValue(std::string_view name, const Json& value) {
    if ((name == "width" || name == "height") && value.is_string()) {
        return value == "auto";
    }
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
    if (found == settings.end()) return true;
    if (!found->is_string()) { *error = "advancedProfileJson must be a string"; return false; }
    const std::string text = found->get<std::string>();
    if (text.size() > 1024 * 1024) { *error = "advancedProfileJson exceeds 1 MiB"; return false; }
    if (std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); })) return true;
    Json profile;
    try {
        profile = Json::parse(text);
    } catch (const std::exception& exception) {
        *error = std::string("advancedProfileJson is invalid JSON: ") + exception.what();
        return false;
    }
    if (!profile.is_object() || !profile.contains("schemaVersion") ||
        !profile["schemaVersion"].is_number_integer() || (profile["schemaVersion"] != 1 && profile["schemaVersion"] != 2)) {
        *error = "advancedProfileJson must be an object with schemaVersion 1 or 2";
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
            if (!ReadString(item, "stateGroup", &rule.state_group, error) ||
                !ReadString(item, "state", &rule.state, error) ||
                !ReadBoolean(item, "enabled", &rule.enabled, error)) return false;
            if (rule.state.empty() != rule.state_group.empty()) { *error = "state and stateGroup must be set together"; return false; }
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
    for (const auto* key : {"variables", "resources"}) {
        if (profile.contains(key) && (!profile[key].is_object() || profile[key].size() > 128)) { *error = std::string(key) + " must be an object of at most 128 entries"; return false; }
    }
    config->variables = profile.value("variables", Json::object());
    config->resources = profile.value("resources", Json::object());
    if (profile.contains("captures")) {
        if (!profile["captures"].is_array() || profile["captures"].size() > 128) { *error = "captures must be an array of at most 128 entries"; return false; }
        for (const auto& capture : profile["captures"]) {
            Selector target; std::string property, name;
            if (!capture.is_object() || !capture.contains("selector") || !ParseSelector(capture["selector"], &target, error) ||
                !ReadString(capture, "property", &property, error) || !ReadString(capture, "name", &name, error) || property.empty() || name.empty()) { *error = "each capture requires selector, property and name"; return false; }
        }
        config->captures = profile["captures"];
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

bool ParseAppearance(const Json& settings, Config* config, std::string* error) {
    const auto found = settings.find("appearanceJson");
    if (found == settings.end()) return true;
    if (!found->is_string() || found->get_ref<const std::string&>().size() > 1024 * 1024) {
        *error = "appearanceJson must be a string of at most 1 MiB"; return false;
    }
    const auto& text = found->get_ref<const std::string&>();
    if (std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); })) return true;
    auto appearance = Json::parse(text);
    if (!appearance.is_object()) { *error = "appearanceJson must be an object"; return false; }
    for (auto region = appearance.begin(); region != appearance.end(); ++region) {
        if ((region.key() != "surface" && region.key() != "button" && region.key() != "startButton" && region.key() != "systemTray" && region.key() != "indicator") || !region->is_object()) {
            *error = "appearanceJson has an unsupported element"; return false;
        }
        for (auto state = region->begin(); state != region->end(); ++state) {
            if ((state.key() != "normal" && state.key() != "hover" && state.key() != "pressed" && state.key() != "active") || !state->is_object() ||
                (region.key() == "surface" && state.key() != "normal") ||
                (region.key() == "indicator" && state.key() != "normal" && state.key() != "active") ||
                ((region.key() == "startButton" || region.key() == "systemTray") && state.key() == "active")) {
                *error = "appearanceJson has an unsupported state"; return false;
            }
            if (state.key() == "active") {
                Rule subscription; subscription.selector.semantic = region.key();
                subscription.state_group = "RunningIndicatorStates"; subscription.state = "ActiveRunningIndicator";
                config->rules.push_back(std::move(subscription));
            }
            for (auto property = state->begin(); property != state->end(); ++property) {
                const auto& key = property.key();
                if ((key != "background" && key != "foreground" && key != "opacity" && key != "cornerRadius" && key != "borderBrush" && key != "borderThickness") ||
                    !ValidPropertyValue(XamlPropertyName(key), *property) || property->is_null()) {
                    *error = "appearanceJson has an invalid property: " + key; return false;
                }
                if ((key == "opacity" || key == "cornerRadius" || key == "borderThickness") &&
                    (!IsFiniteNumber(*property) || property->get<double>() < 0 || property->get<double>() > (key == "opacity" ? 1 : key == "cornerRadius" ? 64 : 12))) {
                    *error = "appearanceJson number is outside its bounds: " + key; return false;
                }
            }
        }
    }
    config->appearance = std::move(appearance);
    return true;
}

bool ParseConsumerMotion(const Json& settings, Config* config, std::string* error) {
    const auto found = settings.find("motionJson"); if (found == settings.end()) return true;
    if (!found->is_string() || found->get_ref<const std::string&>().size() > 16384) { *error = "motionJson must be a string of at most 16 KiB"; return false; }
    const auto& text=found->get_ref<const std::string&>();
    if (std::all_of(text.begin(),text.end(),[](unsigned char c){return std::isspace(c);})) return true;
    auto motion=Json::parse(text); if (!motion.is_object()) { *error="motionJson must be an object"; return false; }
    for (auto region=motion.begin();region!=motion.end();++region) {
        if ((region.key()!="button" && region.key()!="startButton" && region.key()!="systemTray") || !region->is_object()) { *error="motionJson has an unsupported element"; return false; }
        for(auto field=region->begin();field!=region->end();++field) {
            const auto& key=field.key();
            if (key=="easing") { if (*field=="linear"||*field=="ease-out"||*field=="ease-in-out") continue; }
            else {
                const bool size=key=="size"||key=="hoverSize"||key=="pressSize",opacity=key=="hoverOpacity"||key=="pressOpacity",duration=key=="hoverDurationMs"||key=="pressDurationMs";
                if ((size||opacity||duration) && IsFiniteNumber(*field) && field->get<double>() >= (size?.5:0) && field->get<double>() <= (size?1.5:opacity?1:2000) && (!duration||field->is_number_integer())) continue;
            }
            *error="motionJson has an invalid field: "+key; return false;
        }
    }
    config->motion=std::move(motion); return true;
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
    // Legacy palette identifiers supply defaults only. Explicit saved values
    // always remain editable and take precedence over that fallback.
    ApplyPreset(config);
    if (!ReadString(settings, "surfaceWidthMode", &config->surface_width_mode, error) ||
        !ReadString(settings, "surfaceAlignment", &config->surface_alignment, error)) return false;
    if (config->surface_width_mode != "native" && config->surface_width_mode != "content" &&
        config->surface_width_mode != "fixed") {
        *error = "surfaceWidthMode must be native, content, or fixed";
        return false;
    }
    if (config->surface_alignment != "native" && config->surface_alignment != "left" &&
        config->surface_alignment != "center" && config->surface_alignment != "right") {
        *error = "surfaceAlignment must be native, left, center, or right";
        return false;
    }
    if (!ReadColor(settings, "surfaceColor", &config->surface_color, error) ||
        !ReadNumber(settings, "surfaceOpacity", 0.0, 1.0, &config->surface_opacity, error) ||
        !ReadNumber(settings, "surfaceRadius", 0.0, 64.0, &config->surface_radius, error) ||
        !ReadNumber(settings, "surfaceInsetHorizontal", -64.0, 64.0, &config->surface_inset_horizontal, error) ||
        !ReadNumber(settings, "surfaceInsetVertical", -64.0, 64.0, &config->surface_inset_vertical, error) ||
        !ReadNumber(settings, "surfaceWidth", 240.0, 4096.0, &config->surface_width, error) ||
        !ReadNumber(settings, "surfaceHeight", 0.0, 96.0, &config->surface_height, error) ||
        !ReadNumber(settings, "surfaceOffsetX", -64.0, 64.0, &config->surface_offset_x, error) ||
        !ReadNumber(settings, "surfaceOffsetY", -32.0, 32.0, &config->surface_offset_y, error) ||
        !ReadColor(settings, "buttonBackgroundColor", &config->button_background_color, error) ||
        !ReadColor(settings, "buttonForegroundColor", &config->button_foreground_color, error) ||
        !ReadNumber(settings, "buttonCornerRadius", 0.0, 48.0, &config->button_corner_radius, error) ||
        !ReadNumber(settings, "buttonPaddingHorizontal", 0.0, 64.0, &config->button_padding_horizontal, error) ||
        !ReadNumber(settings, "buttonPaddingVertical", 0.0, 64.0, &config->button_padding_vertical, error) ||
        !ReadNumber(settings, "buttonSpacing", 0.0, 64.0, &config->button_spacing, error) ||
        !ReadNumber(settings, "buttonWidth", 0.0, 160.0, &config->button_width, error) ||
        !ReadNumber(settings, "buttonHeight", 0.0, 96.0, &config->button_height, error) ||
        !ReadNumber(settings, "buttonRotationDegrees", -180.0, 180.0, &config->button_rotation_degrees, error) ||
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
        !ReadNumber(settings, "indicatorRadius", 0.0, 16.0, &config->indicator_radius, error) ||
        !ReadBoolean(settings, "indicatorVisible", &config->indicator_visible, error)) {
        return false;
    }
    return ParseAdvanced(settings, config, error) && ParseAppearance(settings, config, error) && ParseConsumerMotion(settings, config, error);
}
} // namespace

Selector ParseSelectorJson(const Json &value) {
    Selector selector;
    std::string error;
    if (!ParseSelector(value, &selector, &error))
        throw std::runtime_error(error);
    return selector;
}

bool ValidPropertyValue(std::string_view name, const Json &value) {
    if (name.empty() || name.size() > 128 ||
        !std::all_of(name.begin(), name.end(),
                     [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '.'; }))
        return false;
    if (XamlPropertyName(name) == "ActualWidth" || XamlPropertyName(name) == "ActualHeight")
        return false;
    if (LegacyPropertyValue(name, value))
        return true;
    // Lowercase legacy spellings keep their validation, while XAML names and
    // structured values enable the complete native property surface.
    if (value.is_object()) {
        if (value.contains("ref"))
            return value.size() == 1 && value["ref"].is_string();
        if (value.contains("expression"))
            return value.size() == 1 && value["expression"].is_string() &&
                   value["expression"].get_ref<const std::string &>().size() <= 4096;
        if (value.contains("resource"))
            return value.size() == 1 && value["resource"].is_string();
        if (value.contains("xaml"))
            return value.size() == 1 && value["xaml"].is_string() &&
                   value["xaml"].get_ref<const std::string &>().size() <= 65536;
        if (!value.contains("kind") || !value["kind"].is_string())
            return false;
        const auto kind = value["kind"].get<std::string>();
        const auto bounded = [&](const char *key, double low, double high) {
            const auto field = value.find(key);
            return field == value.end() || (IsFiniteNumber(*field) && field->get<double>() >= low &&
                                            field->get<double>() <= high);
        };
        const auto colorValue = [&](const Json &field) {
            return field.is_object() &&
                       (field.contains("resource") || field.contains("ref") ||
                        field.contains("expression")) &&
                       ValidPropertyValue("Color", field) ||
                   field.is_string() &&
                       (IsHexColor(field.get<std::string>()) || field == "transparent");
        };
        const auto color = [&](const char *key, bool required = false) {
            const auto field = value.find(key);
            return field == value.end() ? !required : colorValue(*field);
        };
        if (kind == "blur")
            return color("color", true) && color("fallbackColor") && bounded("radius", 0, 100) &&
                   bounded("saturation", 0, 4) && bounded("luminosity", 0, 2) &&
                   bounded("noiseOpacity", 0, 0.25) && bounded("tintOpacity", 0, 1) &&
                   (!value.contains("overlay") || (value["overlay"].is_object() && value["overlay"].value("kind", "") == "gradient" && ValidPropertyValue("Background", value["overlay"])));
        if (kind == "solid" && !color("color", true))
            return false;
        if (kind == "acrylic" &&
            (!color("color", true) || !color("fallbackColor") || !bounded("tintOpacity", 0, 1)))
            return false;
        if (kind == "gradient") {
            if (!bounded("angle", -180, 180) || !bounded("repetitions", 1, 16) || !bounded("duration", 1, 120) ||
                !bounded("centerX", 0, 1) || !bounded("centerY", 0, 1) || !bounded("radiusX", 0.01, 2) || !bounded("radiusY", 0.01, 2)) return false;
            for (const auto& [key, options] : std::vector<std::pair<const char*, std::vector<std::string>>>{
                {"shape", {"linear", "radial"}}, {"extend", {"clamp", "wrap", "mirror"}}, {"motion", {"none", "scroll", "pingpong"}}})
                if (value.contains(key) && (!value[key].is_string() || std::find(options.begin(), options.end(), value[key].get<std::string>()) == options.end())) return false;
            if (!value.contains("stops") || !value["stops"].is_array() ||
                value["stops"].size() < 2 || value["stops"].size() > 64)
                return false;
            for (const auto &stop : value["stops"])
                if (!stop.is_object() || !stop.contains("offset") ||
                    !IsFiniteNumber(stop["offset"]) || stop["offset"].get<double>() < 0 ||
                    stop["offset"].get<double>() > 1 || !stop.contains("color") ||
                    !colorValue(stop["color"]))
                    return false;
            for (const auto *point : {"start", "end"})
                if (value.contains(point) &&
                    (!value[point].is_array() || value[point].size() != 2 ||
                     !std::all_of(value[point].begin(), value[point].end(), IsFiniteNumber)))
                    return false;
        }
        if (kind == "columns" || kind == "rows") {
            if (!value.contains("sizes") || !value["sizes"].is_array() ||
                value["sizes"].size() > 32)
                return false;
            for (const auto &size : value["sizes"]) {
                if (IsFiniteNumber(size) && size.get<double>() >= 0)
                    continue;
                if (!size.is_string())
                    return false;
                const auto text = size.get<std::string>();
                if (text == "Auto" || text == "auto" || text == "*")
                    continue;
                const auto literal =
                    !text.empty() && text.back() == '*' ? text.substr(0, text.size() - 1) : text;
                try {
                    size_t used = 0;
                    const double number = std::stod(literal, &used);
                    if (used != literal.size() || !std::isfinite(number) || number < 0)
                        return false;
                } catch (...) {
                    return false;
                }
            }
        }
        if (kind == "transform") {
            static constexpr std::array<std::string_view, 10> fields = {
                "scaleX", "scaleY", "rotation", "translateX", "translateY",
                "skewX",  "skewY",  "centerX",  "centerY",    "kind"};
            for (auto it = value.begin(); it != value.end(); ++it)
                if (std::find(fields.begin(), fields.end(), it.key()) == fields.end() ||
                    it.key() != "kind" && !IsFiniteNumber(it.value()))
                    return false;
        }
        if (kind == "transform3d" || kind == "projection" || kind == "perspective") {
            for (auto it = value.begin(); it != value.end(); ++it)
                if (it.key() != "kind" && !IsFiniteNumber(it.value()))
                    return false;
            if (kind == "perspective" && !bounded("depth", 1, 100000))
                return false;
        }
        if (kind == "image" && (!value.contains("source") || !value["source"].is_string() ||
                                value["source"].get_ref<const std::string &>().empty()))
            return false;
        if (kind == "transition" && !bounded("durationMs", 0, 5000))
            return false;
        try {
            (void)XamlValueMarkup(value);
            return true;
        } catch (...) {
            return false;
        }
    }
    if (value.is_null())
        return true;
    if (value.is_array()) {
        const auto native = XamlPropertyName(name);
        if (native != "Margin" && native != "Padding" && native != "BorderThickness" &&
            native != "CornerRadius" && native != "RenderTransformOrigin")
            return false;
        return (native == "RenderTransformOrigin" ? value.size() == 2
                                                  : value.size() == 2 || value.size() == 4) &&
               std::all_of(value.begin(), value.end(), [](const auto &v) {
                   return v.is_number() && std::isfinite(v.template get<double>());
               });
    }
    // Explicit native XAML names resolve and validate against the actual type.
    if (!std::isupper(static_cast<unsigned char>(name.front())) && name.find('.') == name.npos &&
        name != "borderBrush" && name != "borderThickness" && name != "radiusX" &&
        name != "radiusY" && name != "fontFamily" && name != "fontWeight" &&
        name != "textAlignment" && name != "renderTransform" && name != "renderTransformOrigin" &&
        name != "backgroundTransition")
        return false;
    return value.is_string() || value.is_boolean() ||
           (value.is_number() && std::isfinite(value.get<double>()));
}

ParseResult ParseSettings(const char *snapshot) {
    if (!snapshot)
        return ParseResult{Config{}, "settings snapshot is null"};
    try {
        return ParseSettingsJson(Json::parse(snapshot));
    } catch (const std::exception &exception) {
        return ParseResult{Config{},
                           std::string("settings snapshot is invalid JSON: ") + exception.what()};
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

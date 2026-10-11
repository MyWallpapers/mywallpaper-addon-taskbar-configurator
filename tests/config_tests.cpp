#include "taskbar_config.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

using taskbar_config::ParseSettingsJson;
using Json = nlohmann::json;

namespace {
int failures = 0;

void Check(bool condition, const char* label) {
    if (condition) return;
    std::cerr << "FAIL: " << label << '\n';
    ++failures;
}

Json FullDefaults() {
    return Json{
        {"enabled", true}, {"profile", "glass"},
        {"surfaceColor", "#1B2330FF"}, {"surfaceOpacity", 0.94},
        {"surfaceRadius", 14}, {"surfaceInsetHorizontal", 0}, {"surfaceInsetVertical", 0},
        {"buttonBackgroundColor", "#00000000"}, {"buttonForegroundColor", "#FFFFFFFF"},
        {"buttonCornerRadius", 8}, {"buttonPaddingHorizontal", 6},
        {"buttonPaddingVertical", 4}, {"buttonSpacing", 2}, {"buttonScale", 1},
        {"buttonHoverScale", 1.06}, {"buttonPressScale", 0.96},
        {"buttonHoverOpacity", 1}, {"buttonPressOpacity", 0.92},
        {"buttonHoverDurationMs", 180}, {"buttonPressDurationMs", 90}, {"buttonEasing", "ease-out"},
        {"startBackgroundColor", "#00000000"}, {"startForegroundColor", "#FFFFFFFF"},
        {"startScale", 1}, {"startHoverScale", 1.08}, {"startAnimationDurationMs", 180},
        {"trayBackgroundColor", "#00000000"}, {"trayForegroundColor", "#FFFFFFFF"},
        {"trayCornerRadius", 8}, {"trayPaddingHorizontal", 5}, {"trayPaddingVertical", 3},
        {"traySpacing", 2}, {"trayScale", 1}, {"trayHoverScale", 1.05},
        {"trayAnimationDurationMs", 160}, {"indicatorColor", "#65C8FFFF"},
        {"indicatorThickness", 2}, {"indicatorRadius", 2},
        {"advancedProfileJson", ""}
    };
}

void TestDefaultsAndPresets() {
    auto defaults = ParseSettingsJson(Json{{"deviceSettings", FullDefaults()},
        {"layerSettings", Json::object()}, {"scratchDirectory", ""}});
    Check(static_cast<bool>(defaults), "full process-v2 settings defaults parse");
    Check(defaults.config.surface_color == "#1B2330FF", "glass profile uses its surface color");

    auto neutral_settings = FullDefaults();
    neutral_settings["profile"] = "neutral";
    auto neutral = ParseSettingsJson(neutral_settings);
    Check(static_cast<bool>(neutral), "neutral profile with all persisted settings parses");
    Check(neutral.config.surface_color == "#202020FF", "neutral profile applies its fixed dark surface");

    auto rose_settings = FullDefaults();
    rose_settings["profile"] = "rose";
    auto rose = ParseSettingsJson(rose_settings);
    Check(static_cast<bool>(rose), "rose profile with all persisted control defaults parses");
    Check(rose.config.surface_color == "#7A1746FF", "rose preset overrides stale color defaults");
    Check(rose.config.button_background_color == "#A52D68FF", "rose preset reaches button background");

    auto custom_settings = FullDefaults();
    custom_settings["profile"] = "custom";
    custom_settings["surfaceColor"] = "#11223380";
    auto custom = ParseSettingsJson(custom_settings);
    Check(static_cast<bool>(custom), "custom profile parses");
    Check(custom.config.surface_color == "#11223380", "custom profile preserves RRGGBBAA color");
}

void TestValidation() {
    auto settings = FullDefaults();
    settings["buttonHoverOpacity"] = 1.5;
    Check(!ParseSettingsJson(settings), "direct opacity rejects values above one");

    settings = FullDefaults();
    settings["surfaceColor"] = "#1B2330B8";
    settings["profile"] = "custom";
    Check(static_cast<bool>(ParseSettingsJson(settings)), "alpha-last color is accepted");

    settings["surfaceColor"] = "#1B2333B";
    Check(!ParseSettingsJson(settings), "malformed color is rejected");

    settings = FullDefaults();
    settings["advancedProfileJson"] = R"({"schemaVersion":1,"rules":[{"selector":{"semantic":"surface"},"properties":{"opacity":2}}]})";
    Check(!ParseSettingsJson(settings), "advanced opacity rejects values above one");

    settings["advancedProfileJson"] = R"({"schemaVersion":1,"rules":[{"selector":{"semantic":"surface"},"properties":{"visibility":"hidden"}}]})";
    Check(!ParseSettingsJson(settings), "unsupported XAML visibility value is rejected");

    settings["advancedProfileJson"] = R"({"schemaVersion":1,"rules":[{"selector":{"name":"TaskbarFrame"},"properties":{"opacity":0.7,"margin":[4,2]}}],"animations":[{"selector":{"type":"Button"},"hoverScale":1.12,"pressScale":0.9,"easing":"ease-in-out"}]})";
    auto advanced = ParseSettingsJson(settings);
    Check(static_cast<bool>(advanced), "valid advanced rules and animation parse");
    Check(advanced.config.rules.size() == 1, "advanced selector rule retained");
    Check(advanced.config.animations.size() == 1, "advanced animation retained");
}
} // namespace

int main() {
    TestDefaultsAndPresets();
    TestValidation();
    if (failures) return EXIT_FAILURE;
    std::cout << "Configuration contract tests passed.\n";
    return EXIT_SUCCESS;
}

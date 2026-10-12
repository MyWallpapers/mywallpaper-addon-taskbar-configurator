#include "taskbar_config.hpp"

#include <cstdlib>
#include <fstream>
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
    Check(neutral.config.surface_color == "#1B2330FF", "saved values override legacy palette defaults");
    Check(ParseSettingsJson(Json{{"profile","neutral"}}).config.surface_color == "#202020FF", "partial legacy profiles retain their fallback palette");

    auto rose_settings = FullDefaults();
    rose_settings["profile"] = "rose";
    auto rose = ParseSettingsJson(rose_settings);
    Check(static_cast<bool>(rose), "rose profile with all persisted control defaults parses");
    Check(rose.config.surface_color == "#1B2330FF", "a legacy identifier never overwrites saved colors");
    Check(rose.config.button_background_color == "#00000000", "button colors remain editable in a legacy profile");

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
void TestStructuredProfiles() {
    auto settings=FullDefaults();
    settings["advancedProfileJson"]=R"({"schemaVersion":2,"variables":{"gap":12},"resources":{"panel":{"light":"#FFFFFFFF","dark":"#202020FF"}},"captures":[{"name":"taskbarWidth","selector":{"name":"TaskbarFrame"},"property":"ActualWidth"}],"rules":[{"selector":{"type":"Rectangle","parents":[{"type":"Grid"}],"index":1,"where":{"Visibility":0}},"stateGroup":"CommonStates","state":"PointerOver","properties":{"Background":{"kind":"blur","color":"#20202080","radius":18,"noiseOpacity":0.02},"Grid.Column":2,"RenderTransform":{"kind":"transform","scaleX":1.2},"Margin":{"expression":"taskbarWidth > 0 ? gap : 0"},"Shadow":null}}]})";
    const auto parsed=ParseSettingsJson(settings);
    Check(static_cast<bool>(parsed),"typed materials, visual states, precise targets and captures parse together");
    Check(parsed.config.rules.size()==1 && parsed.config.rules[0].state=="PointerOver","native visual state survives parsing");
    Check(taskbar_config::EvaluateExpression("false ? 1 / 0 : max(3, gap * 2)",Json{{"gap",12}})==24,"unused conditional branches are not evaluated");
    Check(taskbar_config::EvaluateExpression("true ? 'Visible' : missing + 1",Json::object())=="Visible","conditional text and missing captures are lazy");
    Check(taskbar_config::EvaluateExpression("true ? skip() : 12",Json::object()).contains("$skip"),"skip is distinct from clearing a local value");
    bool rejected=false;try{(void)taskbar_config::EvaluateExpression("1/0",Json::object());}catch(...){rejected=true;}Check(rejected,"division by zero rejects the calculated value");
    Check(taskbar_config::XamlValueMarkup(Json("#11223380"), true)=="#80112233","alpha-last colors convert to native ARGB");
    Check(taskbar_config::XamlValueMarkup(Json("#11223380"))=="#11223380","text resembling a color remains exact");
    Check(taskbar_config::XamlValueMarkup(Json{{"kind","solid"},{"color",Json{{"resource","SystemAccentColor"}}}}).find("ThemeResource")!=std::string::npos,"native accent references survive brush construction");
    Check(taskbar_config::XmlEscape("a\"<&") == "a&quot;&lt;&amp;","XAML attributes cannot escape the synthetic setter");
    Check(!taskbar_config::ValidPropertyValue("width",Json::array({1,2})),"vector values cannot masquerade as dimensions");
    Check(!taskbar_config::ValidPropertyValue("Background",Json{{"kind","blur"},{"color","#202020FF"},{"tintOpacity",2}}),"invalid effect opacity is rejected before injection");
    Check(!taskbar_config::ValidPropertyValue("ActualWidth",42),"read-only dimensions are captured instead of written");
    Check(!taskbar_config::ValidPropertyValue("RenderTransformOrigin",Json::array({0,0,0,0})),"origins require exactly two components");
    Check(!taskbar_config::ValidPropertyValue("ColumnDefinitions",Json{{"kind","columns"},{"sizes",Json::array({"64oops"})}}),"malformed grid sizes are rejected before application");
    Check(!taskbar_config::ValidPropertyValue("Fill",Json{{"kind","gradient"},{"stops",Json::array({Json{{"offset",0},{"color","invalid"}},Json{{"offset",1},{"color","#FFFFFFFF"}}})}}),"gradient stop colors are validated");
    settings["advancedProfileJson"]=42; Check(!ParseSettingsJson(settings),"non-string structured settings produce a real error");
    settings["advancedProfileJson"]=" \n\t"; Check(static_cast<bool>(ParseSettingsJson(settings)),"blank documents remove custom rules");
    settings["advancedProfileJson"]=std::string(1024*1024+1,' ');Check(!ParseSettingsJson(settings),"profile size bounds precede JSON parsing");
    settings["advancedProfileJson"]=R"({"schemaVersion":2,"rules":[{"selector":{"semantic":"button"},"state":"PointerOver","properties":{"Opacity":0.5}}]})";
    Check(!ParseSettingsJson(settings),"a visual state requires its group");
}
void TestGeometryAndManifest(const char* manifest_path) {
    auto settings = FullDefaults();
    settings["surfaceWidthMode"] = "content";
    settings["surfaceAlignment"] = "center";
    settings["surfaceOffsetX"] = -12;
    settings["buttonRotationDegrees"] = 25;
    settings["indicatorVisible"] = false;
    auto parsed = ParseSettingsJson(settings);
    Check(static_cast<bool>(parsed), "geometry and visibility parse independently of a legacy palette");
    Check(parsed.config.surface_width_mode == "content" &&
        parsed.config.surface_alignment == "center" &&
        parsed.config.surface_offset_x == -12 &&
        parsed.config.button_rotation_degrees == 25 &&
        !parsed.config.indicator_visible, "geometry survives legacy palette application");
    for (const auto& invalid : {Json{{"surfaceWidthMode", "unknown"}},
        Json{{"surfaceAlignment", "top"}}, Json{{"buttonRotationDegrees", 181}},
        Json{{"surfaceWidth", 0}}, Json{{"surfaceHeight", 97}},
        Json{{"indicatorVisible", "false"}}}) {
        auto candidate = settings;
        candidate.update(invalid);
        Check(!ParseSettingsJson(candidate), "unsupported geometry is rejected before XAML application");
    }
    settings["advancedProfileJson"] = R"({"schemaVersion":1,"rules":[{"selector":{"name":"TaskbarFrame"},"properties":{"width":"auto","height":"auto","rotation":30}}]})";
    Check(static_cast<bool>(ParseSettingsJson(settings)), "automatic XAML dimensions are accepted");
    settings["advancedProfileJson"] = R"({"schemaVersion":1,"rules":[{"selector":{"name":"TaskbarFrame"},"properties":{"width":"stretch"}}]})";
    Check(!ParseSettingsJson(settings), "only the automatic dimension keyword is accepted");

    std::ifstream stream(manifest_path);
    Check(stream.good(), "published manifest can be read");
    if (!stream.good()) return;
    const Json manifest = Json::parse(stream);
    Json defaults = Json::object();
    for (const auto& definition : manifest["settings"]) {
        if (definition.contains("default")) defaults[definition["id"].get<std::string>()] = definition["default"];
    }
    Check(static_cast<bool>(ParseSettingsJson(defaults)), "all published defaults reach the production parser");
    for (const auto& preset : manifest["presets"]) {
        auto candidate = defaults;
        candidate.update(preset["settings"]);
        auto config = ParseSettingsJson(candidate);
        Check(static_cast<bool>(config), "every published preset reaches the production parser");
        Check(config.config.profile == taskbar_config::Profile::custom,
            "published presets leave colors editable rather than freezing a palette");
        candidate["surfaceColor"] = "#12345678";
        Check(ParseSettingsJson(candidate).config.surface_color == "#12345678",
            "a color edit after a preset remains effective");
    }
}
void TestConsumerAppearance() {
    Json gradient{{"kind","gradient"},{"shape","linear"},{"angle",45},{"extend","wrap"},{"repetitions",3},{"motion","scroll"},{"duration",12},
        {"stops",Json::array({Json{{"offset",.375},{"color","#FF4499AA"}},Json{{"offset",1},{"color","#4488FFFF"}}})}};
    auto settings = FullDefaults();
    Json appearance{{"surface",Json{{"normal",Json{{"background",gradient}}}}},{"button",Json{{"hover",Json{{"opacity",.6}}}}}};
    settings["appearanceJson"] = appearance.dump();
    auto parsed = ParseSettingsJson(settings);
    Check(static_cast<bool>(parsed) && parsed.config.appearance == appearance, "sparse consumer regions and states parse without duplicating inherited fields");
    Check(parsed.config.surface_color == "#1B2330FF" && parsed.config.button_hover_scale == 1.06, "consumer editing preserves legacy user values");
    for (const auto& field : {"angle","repetitions","duration","radiusX"}) {
        auto invalid = gradient; invalid[field] = -500; Check(!taskbar_config::ValidPropertyValue("Fill",invalid),"native gradient parameters are bounded");
    }
    Json glass{{"kind","blur"},{"color","#20202080"},{"overlay",gradient}};
    Check(taskbar_config::ValidPropertyValue("Background",glass),"glass and gradient compose as one material");
    glass["overlay"]["motion"]="unknown"; Check(!taskbar_config::ValidPropertyValue("Background",glass),"invalid nested material motion is rejected");
    for (const auto& invalid : {Json{{"surface",Json{{"hover",Json::object()}}}},Json{{"unknown",Json::object()}},Json{{"button",Json{{"normal",Json{{"opacity",2}}}}}}}) {
        settings["appearanceJson"]=invalid.dump(); Check(!ParseSettingsJson(settings),"unsupported consumer target/state/value is rejected");
    }
    settings["appearanceJson"]=""; Check(ParseSettingsJson(settings).config.appearance.empty(),"empty consumer configuration preserves the existing appearance");
}
void TestConsumerMotion() {
    auto settings = FullDefaults();
    const Json motion{{"button",Json{{"hoverSize",1.15},{"pressDurationMs",80},{"easing","ease-in-out"}}}};
    settings["motionJson"]=motion.dump(); auto parsed=ParseSettingsJson(settings);
    Check(static_cast<bool>(parsed) && parsed.config.motion==motion,"consumer motion is sparse and uses native ratios internally");
    Check(parsed.config.button_hover_scale==1.06,"consumer motion preserves historical persisted values");
    for(const auto& invalid: {Json{{"button",Json{{"hoverSize",9}}}},Json{{"button",Json{{"pressDurationMs",.5}}}},Json{{"surface",Json::object()}}}){
        settings["motionJson"]=invalid.dump(); Check(!ParseSettingsJson(settings),"invalid motion is rejected before Explorer receives it");
    }
    auto settings_active=FullDefaults();settings_active["appearanceJson"]=R"({"indicator":{"active":{"background":{"kind":"solid","color":"#FF4488FF"}}}})";
    auto active=ParseSettingsJson(settings_active);Check(static_cast<bool>(active) && active.config.rules.size()==1 && active.config.rules[0].state_group=="RunningIndicatorStates","consumer active state reuses native state subscriptions");
}
} // namespace

int main(int argc, char** argv) {
    TestDefaultsAndPresets();
    TestValidation();
    TestStructuredProfiles();
    TestConsumerAppearance();
    TestConsumerMotion();
    if (argc > 1) TestGeometryAndManifest(argv[1]);
    if (failures) return EXIT_FAILURE;
    std::cout << "Configuration contract tests passed.\n";
    return EXIT_SUCCESS;
}

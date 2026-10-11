#define TASKBAR_CONFIGURATOR_BUILD
#include "mw_native_sdk.hpp"
#include "mw_call_gate.hpp"
#include "taskbar_config.hpp"
#include "taskbar_diagnostics.h"

#include <Windows.h>
#include <ocidl.h>
#include <xamlom.h>
#undef GetCurrentTime

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cwchar>
#include <cwctype>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <process.h>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace std::chrono_literals;

namespace {
namespace xaml = winrt::Windows::UI::Xaml;
namespace controls = winrt::Windows::UI::Xaml::Controls;
namespace shapes = winrt::Windows::UI::Xaml::Shapes;
namespace media = winrt::Windows::UI::Xaml::Media;
namespace composition = winrt::Windows::UI::Composition;
namespace foundation = winrt::Windows::Foundation;
namespace numerics = winrt::Windows::Foundation::Numerics;
using Json = nlohmann::json;
using taskbar_config::Config;

constexpr GUID kDiagnosticsClass = {0x04f4a6b7, 0x47c8, 0x4c41,
    {0x9a, 0x90, 0x57, 0x5c, 0x89, 0x34, 0xe2, 0x61}};
constexpr DWORD kUiDispatchTimeoutMs = 1200;
constexpr DWORD kDrainTimeoutMs = 250;
constexpr DWORD kDiagnosticsStartupTimeoutMs = 5000;

std::atomic<bool> g_active{false};
std::atomic<bool> g_cleanup_pending{false};
std::atomic<uint64_t> g_lifecycle_generation{0};
mw_native::CallGate g_call_gate;

std::mutex g_dispatcher_mutex;
winrt::agile_ref<winrt::Windows::UI::Core::CoreDispatcher> g_dispatcher;

std::mutex g_settings_mutex;
Config g_pending_config;
uint64_t g_pending_revision = 0;
bool g_apply_queued = false;
Config g_ui_config;

// These objects are reused across Start/Stop cycles. The engine keeps the
// image resident, so callback code and hook continuation storage remain valid.
winrt::com_ptr<IXamlDiagnostics> g_xaml_diagnostics;
winrt::com_ptr<IVisualTreeService> g_visual_tree;
winrt::com_ptr<IVisualTreeServiceCallback2> g_observer;
winrt::com_ptr<IUnknown> g_site_object;
bool g_tree_advised = false;
HWND g_taskbar_window = nullptr;
DWORD g_taskbar_thread = 0;
HANDLE g_start_operation = nullptr;
std::atomic<HRESULT> g_start_operation_result{E_PENDING};

struct UnadviseOperation {
    IVisualTreeService* visual_tree = nullptr;
    IVisualTreeServiceCallback2* observer = nullptr;
    HRESULT result = E_PENDING;
};

HANDLE g_unadvise_operation = nullptr;
UnadviseOperation* g_unadvise_context = nullptr;

struct MotionSettings {
    double hover_scale = 1.0;
    double press_scale = 1.0;
    double hover_opacity = 1.0;
    double press_opacity = 1.0;
    uint32_t hover_duration_ms = 0;
    uint32_t press_duration_ms = 0;
    taskbar_config::Easing easing = taskbar_config::Easing::ease_out;
};

struct PropertyState {
    xaml::DependencyProperty property{nullptr};
    std::string name;
    foundation::IInspectable original{nullptr};
    foundation::IInspectable desired{nullptr};
    int64_t callback_token = 0;
    bool has_callback = false;
};

struct ElementEntry {
    uint64_t handle = 0;
    xaml::FrameworkElement element{nullptr};
    xaml::UIElement ui_element{nullptr};
    std::string name;
    std::string type;
    std::vector<std::string> ancestor_names;
    std::string semantic;
    std::vector<PropertyState> properties;
    bool writing = false;
    bool matched = false;

    bool has_visual = false;
    bool pointer_handlers = false;
    bool pointer_inside = false;
    bool pointer_pressed = false;
    bool animation_counted = false;
    composition::Visual visual{nullptr};
    numerics::float3 original_scale{};
    float original_opacity = 1.0f;
    float original_rotation = 0.0f;
    bool rotation_saved = false;
    double base_scale = 1.0;
    MotionSettings motion;
    winrt::event_token entered_token{};
    winrt::event_token exited_token{};
    winrt::event_token pressed_token{};
    winrt::event_token released_token{};
};

std::map<uint64_t, ElementEntry> g_entries;

void SetDiagnostic(uint64_t* field, uint64_t value) noexcept {
    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(field),
        static_cast<LONG64>(value));
}

void IncrementDiagnostic(uint64_t* field) noexcept {
    InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(field));
}

void DecrementDiagnostic(uint64_t* field) noexcept {
    InterlockedDecrement64(reinterpret_cast<volatile LONG64*>(field));
}

void SetLastError(HRESULT error) noexcept {
    InterlockedExchange64(&MwTaskbarConfiguratorDiagnosticsBlock.lastError,
        static_cast<LONG64>(error));
}

struct WorkerCounter final {
    WorkerCounter() noexcept { IncrementDiagnostic(&MwTaskbarConfiguratorDiagnosticsBlock.workerCount); }
    ~WorkerCounter() { DecrementDiagnostic(&MwTaskbarConfiguratorDiagnosticsBlock.workerCount); }
};

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t character) { return static_cast<wchar_t>(std::towlower(character)); });
    return value;
}

std::string ToUtf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), length, nullptr, nullptr) != length) {
        return {};
    }
    return result;
}

std::wstring BstrText(BSTR value) {
    return value ? std::wstring(value, SysStringLen(value)) : std::wstring{};
}

bool ContainsInsensitive(const std::string& value, const char* fragment) {
    std::string lower = value;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::string query = fragment;
    std::transform(query.begin(), query.end(), query.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower.find(query) != std::string::npos;
}

bool IsHex(char value, uint8_t* digit) noexcept {
    if (value >= '0' && value <= '9') *digit = static_cast<uint8_t>(value - '0');
    else if (value >= 'a' && value <= 'f') *digit = static_cast<uint8_t>(value - 'a' + 10);
    else if (value >= 'A' && value <= 'F') *digit = static_cast<uint8_t>(value - 'A' + 10);
    else return false;
    return true;
}

bool ParseColor(const std::string& text, xaml::Media::SolidColorBrush* brush) {
    if (!brush || (text.size() != 7 && text.size() != 9) || text.front() != '#') return false;
    std::array<uint8_t, 8> bytes{};
    for (std::size_t i = 1; i < text.size(); ++i) {
        uint8_t digit = 0;
        if (!IsHex(text[i], &digit)) return false;
        const std::size_t channel = (i - 1) / 2;
        bytes[channel] = static_cast<uint8_t>((bytes[channel] << 4) | digit);
    }
    // MyWallpaper stores alpha last (#RRGGBBAA); XAML's Color constructor is ARGB.
    const uint8_t alpha = text.size() == 9 ? bytes[3] : 0xff;
    *brush = xaml::Media::SolidColorBrush{winrt::Windows::UI::Color{
        alpha, bytes[0], bytes[1], bytes[2]}};
    return true;
}

foundation::IInspectable BrushValue(const Json& value) {
    if (!value.is_string()) return nullptr;
    const std::string text = value.get<std::string>();
    if (text == "transparent") {
        xaml::Media::SolidColorBrush brush{winrt::Windows::UI::Color{0, 0, 0, 0}};
        return brush.as<foundation::IInspectable>();
    }
    xaml::Media::SolidColorBrush brush{nullptr};
    if (!ParseColor(text, &brush)) return nullptr;
    return brush.as<foundation::IInspectable>();
}

xaml::DependencyProperty PropertyFor(const xaml::FrameworkElement& element,
    const std::string& name) {
    if (name == "opacity") return xaml::UIElement::OpacityProperty();
    if (name == "width") return xaml::FrameworkElement::WidthProperty();
    if (name == "height") return xaml::FrameworkElement::HeightProperty();
    if (name == "minWidth") return xaml::FrameworkElement::MinWidthProperty();
    if (name == "maxWidth") return xaml::FrameworkElement::MaxWidthProperty();
    if (name == "minHeight") return xaml::FrameworkElement::MinHeightProperty();
    if (name == "maxHeight") return xaml::FrameworkElement::MaxHeightProperty();
    if (name == "margin" || name == "spacing") return xaml::FrameworkElement::MarginProperty();
    if (name == "horizontalAlignment") return xaml::FrameworkElement::HorizontalAlignmentProperty();
    if (name == "verticalAlignment") return xaml::FrameworkElement::VerticalAlignmentProperty();
    if (name == "visibility") return xaml::UIElement::VisibilityProperty();
    if (name == "isHitTestVisible") return xaml::UIElement::IsHitTestVisibleProperty();
    if (name == "rotation") return nullptr; // Composition Visual property; restored independently.

    if (auto border = element.try_as<controls::Border>()) {
        if (name == "background") return controls::Border::BackgroundProperty();
        if (name == "foreground") return nullptr;
        if (name == "cornerRadius") return controls::Border::CornerRadiusProperty();
        if (name == "strokeThickness") return controls::Border::BorderThicknessProperty();
        if (name == "padding") return controls::Border::PaddingProperty();
    }
    if (auto control = element.try_as<controls::Control>()) {
        if (name == "background") return controls::Control::BackgroundProperty();
        if (name == "foreground") return controls::Control::ForegroundProperty();
        if (name == "padding") return controls::Control::PaddingProperty();
        if (name == "fontSize") return controls::Control::FontSizeProperty();
        if (name == "isEnabled") return controls::Control::IsEnabledProperty();
    }
    if (auto presenter = element.try_as<controls::ContentPresenter>()) {
        if (name == "background") return controls::ContentPresenter::BackgroundProperty();
        if (name == "foreground") return controls::ContentPresenter::ForegroundProperty();
        if (name == "padding") return controls::ContentPresenter::PaddingProperty();
        if (name == "cornerRadius") return controls::ContentPresenter::CornerRadiusProperty();
    }
    if (auto shape = element.try_as<shapes::Shape>()) {
        if (name == "background" || name == "fill") return shapes::Shape::FillProperty();
        if (name == "stroke") return shapes::Shape::StrokeProperty();
        if (name == "strokeThickness") return shapes::Shape::StrokeThicknessProperty();
    }
    if (auto text = element.try_as<controls::TextBlock>()) {
        if (name == "foreground") return controls::TextBlock::ForegroundProperty();
        if (name == "fontSize") return controls::TextBlock::FontSizeProperty();
        if (name == "text") return controls::TextBlock::TextProperty();
    }
    return nullptr;
}

foundation::IInspectable MakePropertyValue(const xaml::FrameworkElement& element,
    const std::string& name, const Json& value) {
    if (name == "background" || name == "foreground" || name == "fill" || name == "stroke") {
        return BrushValue(value);
    }
    if (name == "opacity" || name == "width" || name == "height" ||
        name == "minWidth" || name == "maxWidth" || name == "minHeight" ||
        name == "maxHeight" || name == "fontSize" || name == "rotation") {
        return winrt::box_value(static_cast<float>(value.get<double>()));
    }
    if (name == "cornerRadius") {
        const double radius = value.get<double>();
        const xaml::CornerRadius corner{radius, radius, radius, radius};
        return winrt::box_value(corner);
    }
    if (name == "strokeThickness") {
        const double thickness = value.get<double>();
        if (element.try_as<shapes::Shape>()) return winrt::box_value(static_cast<float>(thickness));
        const xaml::Thickness border{thickness, thickness, thickness, thickness};
        return winrt::box_value(border);
    }
    if (name == "margin" || name == "padding" || name == "spacing") {
        double left = 0, top = 0, right = 0, bottom = 0;
        if (value.is_number()) {
            left = top = right = bottom = value.get<double>();
        } else if (value.size() == 2) {
            left = right = value[0].get<double>();
            top = bottom = value[1].get<double>();
        } else {
            left = value[0].get<double>(); top = value[1].get<double>();
            right = value[2].get<double>(); bottom = value[3].get<double>();
        }
        return winrt::box_value(xaml::Thickness{left, top, right, bottom});
    }
    if (name == "horizontalAlignment") {
        const auto text = value.get<std::string>();
        const auto alignment = text == "left" ? xaml::HorizontalAlignment::Left :
            text == "center" ? xaml::HorizontalAlignment::Center :
            text == "right" ? xaml::HorizontalAlignment::Right :
            xaml::HorizontalAlignment::Stretch;
        return winrt::box_value(alignment);
    }
    if (name == "verticalAlignment") {
        const auto text = value.get<std::string>();
        const auto alignment = text == "top" ? xaml::VerticalAlignment::Top :
            text == "center" ? xaml::VerticalAlignment::Center :
            text == "bottom" ? xaml::VerticalAlignment::Bottom :
            xaml::VerticalAlignment::Stretch;
        return winrt::box_value(alignment);
    }
    if (name == "visibility") {
        const auto text = value.get<std::string>();
        return winrt::box_value(text == "collapsed" ? xaml::Visibility::Collapsed : xaml::Visibility::Visible);
    }
    if (name == "isHitTestVisible" || name == "isEnabled") {
        return winrt::box_value(value.get<bool>());
    }
    if (name == "text") return winrt::box_value(winrt::to_hstring(value.get<std::string>()));
    return nullptr;
}

bool SelectorMatches(const taskbar_config::Selector& selector, const ElementEntry& entry) {
    if (!selector.semantic.empty() && selector.semantic != entry.semantic) return false;
    if (!selector.name.empty() && selector.name != entry.name) return false;
    if (!selector.type.empty() && entry.type.find(selector.type) == std::string::npos) return false;
    if (!selector.ancestor_name.empty() &&
        std::find(entry.ancestor_names.begin(), entry.ancestor_names.end(),
            selector.ancestor_name) == entry.ancestor_names.end()) return false;
    return true;
}

void AddDesiredProperty(ElementEntry& entry, std::vector<PropertyState>* desired,
    const std::string& property_name, const Json& value) {
    if (property_name == "rotation") {
        if (!entry.has_visual) {
            entry.visual = winrt::Windows::UI::Xaml::Hosting::ElementCompositionPreview::GetElementVisual(entry.ui_element);
            entry.original_scale = entry.visual.Scale();
            entry.original_opacity = entry.visual.Opacity();
            entry.original_rotation = entry.visual.RotationAngle();
            entry.rotation_saved = true;
            entry.has_visual = true;
        }
        PropertyState state;
        state.name = property_name;
        state.desired = winrt::box_value(static_cast<float>(value.get<double>()));
        desired->push_back(std::move(state));
        return;
    }
    const auto property = PropertyFor(entry.element, property_name);
    if (!property) return;
    auto inspectable = MakePropertyValue(entry.element, property_name, value);
    if (!inspectable) return;
    PropertyState state;
    state.property = property;
    state.name = property_name;
    state.desired = std::move(inspectable);
    desired->push_back(std::move(state));
}

void AddColor(ElementEntry& entry, std::vector<PropertyState>* desired,
    const char* property, const std::string& color) {
    AddDesiredProperty(entry, desired, property, Json(color));
}

void AddNumber(ElementEntry& entry, std::vector<PropertyState>* desired,
    const char* property, double number) {
    AddDesiredProperty(entry, desired, property, Json(number));
}

void AddPadding(ElementEntry& entry, std::vector<PropertyState>* desired,
    const char* property, double horizontal, double vertical) {
    AddDesiredProperty(entry, desired, property, Json::array({horizontal, vertical}));
}

bool HasDirectAnimation(const ElementEntry& entry) {
    return entry.semantic == "button" || entry.semantic == "startButton" ||
        entry.semantic == "systemTray";
}

void ConfigureMotion(ElementEntry& entry, const Config& config,
    std::vector<PropertyState>* desired) {
    entry.base_scale = 1.0;
    entry.motion = MotionSettings{};
    if (entry.semantic == "button") {
        entry.base_scale = config.button_scale;
        entry.motion = MotionSettings{config.button_hover_scale,
            config.button_press_scale, config.button_hover_opacity,
            config.button_press_opacity, config.button_hover_duration_ms,
            config.button_press_duration_ms, config.button_easing};
        AddColor(entry, desired, "background", config.button_background_color);
        AddColor(entry, desired, "foreground", config.button_foreground_color);
        AddNumber(entry, desired, "cornerRadius", config.button_corner_radius);
        AddPadding(entry, desired, "padding", config.button_padding_horizontal,
            config.button_padding_vertical);
        AddPadding(entry, desired, "spacing", config.button_spacing / 2.0,
            config.button_spacing / 2.0);
    } else if (entry.semantic == "startButton") {
        entry.base_scale = config.start_scale;
        entry.motion.hover_scale = config.start_hover_scale;
        entry.motion.press_scale = config.button_press_scale;
        entry.motion.hover_opacity = config.button_hover_opacity;
        entry.motion.press_opacity = config.button_press_opacity;
        entry.motion.hover_duration_ms = config.start_animation_duration_ms;
        entry.motion.press_duration_ms = config.button_press_duration_ms;
        entry.motion.easing = config.button_easing;
        AddColor(entry, desired, "background", config.start_background_color);
        AddColor(entry, desired, "foreground", config.start_foreground_color);
    } else if (entry.semantic == "systemTray") {
        entry.base_scale = config.tray_scale;
        entry.motion.hover_scale = config.tray_hover_scale;
        entry.motion.press_scale = config.button_press_scale;
        entry.motion.hover_opacity = config.button_hover_opacity;
        entry.motion.press_opacity = config.button_press_opacity;
        entry.motion.hover_duration_ms = config.tray_animation_duration_ms;
        entry.motion.press_duration_ms = config.button_press_duration_ms;
        entry.motion.easing = config.button_easing;
        AddColor(entry, desired, "background", config.tray_background_color);
        AddColor(entry, desired, "foreground", config.tray_foreground_color);
        AddNumber(entry, desired, "cornerRadius", config.tray_corner_radius);
        AddPadding(entry, desired, "padding", config.tray_padding_horizontal,
            config.tray_padding_vertical);
        AddPadding(entry, desired, "spacing", config.tray_spacing / 2.0,
            config.tray_spacing / 2.0);
    }
}

std::size_t RulePropertyCount(const ElementEntry& entry, const Config& config) {
    if (!config.enabled) return 0;
    std::size_t count = 0;
    for (const auto& rule : config.rules) {
        if (SelectorMatches(rule.selector, entry)) count += rule.properties.size();
    }
    return count;
}

void UpdateDiagnostics() {
    uint64_t matched = 0;
    uint64_t properties = 0;
    uint64_t handlers = 0;
    uint64_t animations = 0;
    for (const auto& pair : g_entries) {
        const auto& entry = pair.second;
        if (entry.matched) ++matched;
        properties += static_cast<uint64_t>(entry.properties.size());
        if (entry.pointer_handlers) handlers += 4;
        if (entry.animation_counted) ++animations;
    }
    SetDiagnostic(&MwTaskbarConfiguratorDiagnosticsBlock.matchedElements, matched);
    SetDiagnostic(&MwTaskbarConfiguratorDiagnosticsBlock.retainedProperties, properties);
    SetDiagnostic(&MwTaskbarConfiguratorDiagnosticsBlock.eventHandlers, handlers);
    SetDiagnostic(&MwTaskbarConfiguratorDiagnosticsBlock.animations, animations);
}

void RemovePropertyCallback(ElementEntry& entry, PropertyState& state) noexcept {
    if (!state.has_callback) return;
    try { entry.element.UnregisterPropertyChangedCallback(state.property, state.callback_token); }
    catch (...) { SetLastError(winrt::to_hresult()); }
    state.has_callback = false;
    state.callback_token = 0;
}

bool IsUnset(const foundation::IInspectable& value) {
    return value == xaml::DependencyProperty::UnsetValue();
}

void RestoreProperty(ElementEntry& entry, PropertyState& state) {
    RemovePropertyCallback(entry, state);
    if (state.name == "rotation") {
        if (entry.has_visual && entry.rotation_saved) {
            entry.visual.StopAnimation(L"RotationAngle");
            entry.visual.RotationAngle(entry.original_rotation);
            entry.rotation_saved = false;
            IncrementDiagnostic(&MwTaskbarConfiguratorDiagnosticsBlock.restoreCount);
        }
        return;
    }
    auto object = entry.element.try_as<xaml::DependencyObject>();
    if (!object) return;
    if (IsUnset(state.original)) object.ClearValue(state.property);
    else object.SetValue(state.property, state.original);
    IncrementDiagnostic(&MwTaskbarConfiguratorDiagnosticsBlock.restoreCount);
}

void PropertyChanged(uint64_t handle, xaml::DependencyProperty property,
    foundation::IInspectable const& sender) noexcept;

bool ApplyProperty(ElementEntry& entry, PropertyState& desired) {
    if (desired.name == "rotation") {
        if (!entry.has_visual) return false;
        if (!entry.rotation_saved) {
            entry.original_rotation = entry.visual.RotationAngle();
            entry.rotation_saved = true;
        }
        entry.visual.RotationAngle(winrt::unbox_value<float>(desired.desired));
        const auto existing = std::find_if(entry.properties.begin(), entry.properties.end(),
            [](const PropertyState& item) { return item.name == "rotation"; });
        if (existing == entry.properties.end()) entry.properties.push_back(std::move(desired));
        else existing->desired = desired.desired;
        return true;
    }
    auto object = entry.element.try_as<xaml::DependencyObject>();
    if (!object) return false;
    if (entry.element.GetBindingExpression(desired.property)) return false;

    auto state = std::find_if(entry.properties.begin(), entry.properties.end(),
        [&](const PropertyState& candidate) { return candidate.name == desired.name; });
    if (state == entry.properties.end()) {
        PropertyState stored;
        stored.property = desired.property;
        stored.name = desired.name;
        stored.original = object.ReadLocalValue(stored.property);
        stored.desired = desired.desired;
        try {
            stored.callback_token = entry.element.RegisterPropertyChangedCallback(stored.property,
                [handle = entry.handle, property = stored.property](auto const& sender, auto const&) {
                    PropertyChanged(handle, property, sender);
                });
            stored.has_callback = true;
        } catch (...) {
            SetLastError(winrt::to_hresult());
        }
        entry.properties.push_back(std::move(stored));
        state = std::prev(entry.properties.end());
    } else {
        state->desired = desired.desired;
    }

    entry.writing = true;
    try {
        object.SetValue(state->property, state->desired);
    } catch (...) {
        entry.writing = false;
        SetLastError(winrt::to_hresult());
        RestoreProperty(entry, *state);
        entry.properties.erase(state);
        return false;
    }
    entry.writing = false;
    return true;
}

void RestoreVisual(ElementEntry& entry) noexcept {
    if (!entry.has_visual) return;
    try {
        entry.visual.StopAnimation(L"Scale");
        entry.visual.StopAnimation(L"Opacity");
        entry.visual.Scale(entry.original_scale);
        entry.visual.Opacity(entry.original_opacity);
    } catch (...) { SetLastError(winrt::to_hresult()); }
    entry.pointer_inside = false;
    entry.pointer_pressed = false;
    entry.animation_counted = false;
}

void RevokePointerHandlers(ElementEntry& entry) noexcept {
    if (!entry.pointer_handlers) return;
    try {
        entry.element.PointerEntered(entry.entered_token);
        entry.element.PointerExited(entry.exited_token);
        entry.element.PointerPressed(entry.pressed_token);
        entry.element.PointerReleased(entry.released_token);
    } catch (...) { SetLastError(winrt::to_hresult()); }
    entry.pointer_handlers = false;
}

void Animate(ElementEntry& entry, bool pressed) {
    if (!entry.has_visual) return;
    const double scale_factor = pressed ? entry.motion.press_scale :
        entry.pointer_inside ? entry.motion.hover_scale : 1.0;
    const double opacity_factor = pressed ? entry.motion.press_opacity :
        entry.pointer_inside ? entry.motion.hover_opacity : 1.0;
    const uint32_t duration = pressed ? entry.motion.press_duration_ms :
        entry.motion.hover_duration_ms;
    const numerics::float3 start = entry.visual.Scale();
    const numerics::float3 finish{
        static_cast<float>(entry.original_scale.x * entry.base_scale * scale_factor),
        static_cast<float>(entry.original_scale.y * entry.base_scale * scale_factor),
        static_cast<float>(entry.original_scale.z * scale_factor)};
    const float opacity_start = entry.visual.Opacity();
    const float opacity_finish = static_cast<float>(entry.original_opacity * opacity_factor);
    try {
        if (duration == 0) {
            entry.visual.StopAnimation(L"Scale");
            entry.visual.StopAnimation(L"Opacity");
            entry.visual.Scale(finish);
            entry.visual.Opacity(opacity_finish);
            const bool count_animation = pressed || entry.pointer_inside;
            if (entry.animation_counted != count_animation) {
                entry.animation_counted = count_animation;
                UpdateDiagnostics();
            }
            return;
        }
        auto compositor = entry.visual.Compositor();
        auto scale = compositor.CreateVector3KeyFrameAnimation();
        scale.Duration(std::chrono::milliseconds(duration));
        scale.InsertKeyFrame(0.0f, start);
        const auto easing = entry.motion.easing == taskbar_config::Easing::linear
            ? composition::CompositionEasingFunction{nullptr}
            : entry.motion.easing == taskbar_config::Easing::ease_out
                ? compositor.CreateCubicBezierEasingFunction(
                    numerics::float2{0.0f, 0.0f}, numerics::float2{0.58f, 1.0f})
                : compositor.CreateCubicBezierEasingFunction(
                    numerics::float2{0.42f, 0.0f}, numerics::float2{0.58f, 1.0f});
        if (easing) scale.InsertKeyFrame(1.0f, finish, easing);
        else scale.InsertKeyFrame(1.0f, finish);
        entry.visual.StartAnimation(L"Scale", scale);

        auto opacity = compositor.CreateScalarKeyFrameAnimation();
        opacity.Duration(std::chrono::milliseconds(duration));
        opacity.InsertKeyFrame(0.0f, opacity_start);
        if (easing) opacity.InsertKeyFrame(1.0f, opacity_finish, easing);
        else opacity.InsertKeyFrame(1.0f, opacity_finish);
        entry.visual.StartAnimation(L"Opacity", opacity);
        const bool count_animation = pressed || entry.pointer_inside;
        if (entry.animation_counted != count_animation) {
            entry.animation_counted = count_animation;
            UpdateDiagnostics();
        }
    } catch (...) {
        SetLastError(winrt::to_hresult());
    }
}

void ConfigurePointerHandlers(ElementEntry& entry) {
    const bool should_animate = HasDirectAnimation(entry) ||
        std::any_of(g_ui_config.animations.begin(), g_ui_config.animations.end(),
            [&](const auto& animation) { return SelectorMatches(animation.selector, entry); });
    if (!should_animate || !g_ui_config.enabled) {
        RevokePointerHandlers(entry);
        RestoreVisual(entry);
        return;
    }
    if (!entry.has_visual) {
        entry.visual = winrt::Windows::UI::Xaml::Hosting::ElementCompositionPreview::GetElementVisual(entry.ui_element);
        entry.original_scale = entry.visual.Scale();
        entry.original_opacity = entry.visual.Opacity();
        entry.has_visual = true;
    }
    if (!entry.pointer_handlers) {
        const auto handle = entry.handle;
        entry.entered_token = entry.element.PointerEntered([handle](auto const&, auto const&) {
            auto guard = g_call_gate.Enter();
            if (!guard) return;
            const auto found = g_entries.find(handle);
            if (found == g_entries.end()) return;
            found->second.pointer_inside = true;
            Animate(found->second, found->second.pointer_pressed);
        });
        entry.exited_token = entry.element.PointerExited([handle](auto const&, auto const&) {
            auto guard = g_call_gate.Enter();
            if (!guard) return;
            const auto found = g_entries.find(handle);
            if (found == g_entries.end()) return;
            found->second.pointer_inside = false;
            Animate(found->second, false);
        });
        entry.pressed_token = entry.element.PointerPressed([handle](auto const&, auto const&) {
            auto guard = g_call_gate.Enter();
            if (!guard) return;
            const auto found = g_entries.find(handle);
            if (found == g_entries.end()) return;
            found->second.pointer_pressed = true;
            Animate(found->second, true);
        });
        entry.released_token = entry.element.PointerReleased([handle](auto const&, auto const&) {
            auto guard = g_call_gate.Enter();
            if (!guard) return;
            const auto found = g_entries.find(handle);
            if (found == g_entries.end()) return;
            found->second.pointer_pressed = false;
            Animate(found->second, false);
        });
        entry.pointer_handlers = true;
    }
    const float scale = static_cast<float>(entry.original_scale.x * entry.base_scale);
    entry.visual.Scale(numerics::float3{scale, scale, entry.original_scale.z});
}

void BuildDirectProperties(ElementEntry& entry, const Config& config,
    std::vector<PropertyState>* desired) {
    if (!config.enabled) return;
    if (entry.semantic == "surface") {
        AddColor(entry, desired, "background", config.surface_color);
        AddNumber(entry, desired, "opacity", config.surface_opacity);
        AddNumber(entry, desired, "cornerRadius", config.surface_radius);
        if (entry.name == "TaskbarFrame") {
            AddDesiredProperty(entry, desired, "margin", Json::array({
                config.surface_inset_horizontal, config.surface_inset_vertical}));
        }
    } else if (entry.semantic == "indicator") {
        AddColor(entry, desired, "fill", config.indicator_color);
        AddNumber(entry, desired, "strokeThickness", config.indicator_thickness);
        AddNumber(entry, desired, "cornerRadius", config.indicator_radius);
    } else {
        ConfigureMotion(entry, config, desired);
    }

    for (const auto& rule : config.rules) {
        if (!SelectorMatches(rule.selector, entry)) continue;
        for (auto property = rule.properties.begin(); property != rule.properties.end(); ++property) {
            AddDesiredProperty(entry, desired, property.key(), property.value());
        }
    }
}

void ConfigureAdvancedMotion(ElementEntry& entry, const Config& config) {
    if (!HasDirectAnimation(entry)) {
        entry.base_scale = 1.0;
        entry.motion = MotionSettings{};
    }
    for (const auto& animation : config.animations) {
        if (!SelectorMatches(animation.selector, entry)) continue;
        entry.motion = MotionSettings{animation.hover_scale, animation.press_scale,
            animation.hover_opacity, animation.press_opacity,
            animation.hover_duration_ms, animation.press_duration_ms,
            animation.easing};
    }
}

void ApplyEntry(ElementEntry& entry) {
    std::vector<PropertyState> desired;
    BuildDirectProperties(entry, g_ui_config, &desired);

    for (auto it = entry.properties.begin(); it != entry.properties.end();) {
        const bool still_desired = std::any_of(desired.begin(), desired.end(),
            [&](const PropertyState& target) { return target.property == it->property; });
        if (still_desired) { ++it; continue; }
        try { RestoreProperty(entry, *it); }
        catch (...) { SetLastError(winrt::to_hresult()); }
        it = entry.properties.erase(it);
    }

    entry.writing = true;
    for (auto& target : desired) {
        try { (void)ApplyProperty(entry, target); }
        catch (...) { SetLastError(winrt::to_hresult()); }
    }
    entry.writing = false;

    ConfigureAdvancedMotion(entry, g_ui_config);
    ConfigurePointerHandlers(entry);
    entry.matched = !entry.properties.empty() || entry.pointer_handlers;
    UpdateDiagnostics();
}

void RestoreEntry(ElementEntry& entry) noexcept {
    RevokePointerHandlers(entry);
    RestoreVisual(entry);
    for (auto it = entry.properties.rbegin(); it != entry.properties.rend(); ++it) {
        try { RestoreProperty(entry, *it); }
        catch (...) { SetLastError(winrt::to_hresult()); }
    }
    entry.properties.clear();
    entry.matched = false;
}

void PropertyChanged(uint64_t handle, xaml::DependencyProperty property,
    foundation::IInspectable const& sender) noexcept {
    auto guard = g_call_gate.Enter();
    if (!guard) return;
    const auto found = g_entries.find(handle);
    if (found == g_entries.end()) return;
    auto& entry = found->second;
    if (entry.writing) return;
    const auto state = std::find_if(entry.properties.begin(), entry.properties.end(),
        [&](const PropertyState& candidate) { return candidate.property == property; });
    if (state == entry.properties.end()) return;
    try {
        auto object = sender.try_as<xaml::DependencyObject>();
        if (!object) return;
        if (entry.element.GetBindingExpression(property)) {
            RemovePropertyCallback(entry, *state);
            entry.properties.erase(state);
            entry.matched = !entry.properties.empty() || entry.pointer_handlers;
            UpdateDiagnostics();
            return;
        }
        state->original = object.ReadLocalValue(property);
        entry.writing = true;
        object.SetValue(property, state->desired);
        entry.writing = false;
    } catch (...) {
        entry.writing = false;
        SetLastError(winrt::to_hresult());
    }
}

HRESULT DispatchToUi(const std::function<void()>& action, DWORD timeout_ms) noexcept {
    try {
        winrt::Windows::UI::Core::CoreDispatcher dispatcher{nullptr};
        {
            std::lock_guard lock(g_dispatcher_mutex);
            if (g_dispatcher) dispatcher = g_dispatcher.get();
        }
        if (!dispatcher) return E_PENDING;
        struct Completion {
            HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            HRESULT result = E_PENDING;
            ~Completion() { if (event) CloseHandle(event); }
        };
        auto completion = std::make_shared<Completion>();
        if (!completion->event) return HRESULT_FROM_WIN32(GetLastError());
        dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
            [completion, action] {
                try { action(); completion->result = S_OK; }
                catch (...) { completion->result = winrt::to_hresult(); }
                SetEvent(completion->event);
            });
        const DWORD wait = WaitForSingleObject(completion->event, timeout_ms);
        return wait == WAIT_OBJECT_0 ? completion->result :
            wait == WAIT_TIMEOUT ? HRESULT_FROM_WIN32(WAIT_TIMEOUT) : HRESULT_FROM_WIN32(GetLastError());
    } catch (...) {
        return winrt::to_hresult();
    }
}

void ApplyAllOnUi() {
    for (auto& pair : g_entries) ApplyEntry(pair.second);
    IncrementDiagnostic(&MwTaskbarConfiguratorDiagnosticsBlock.settingsApplied);
}

bool ScheduleSettingsApply(const std::shared_ptr<Config>& config) {
    auto queue = [&] {
        std::lock_guard lock(g_settings_mutex);
        g_pending_config = *config;
        ++g_pending_revision;
        if (g_apply_queued) return false;
        g_apply_queued = true;
        return true;
    }();
    if (!queue) return true;
    const uint64_t generation = g_lifecycle_generation.load(std::memory_order_acquire);
    winrt::Windows::UI::Core::CoreDispatcher dispatcher{nullptr};
    {
        std::lock_guard lock(g_dispatcher_mutex);
        if (g_dispatcher) dispatcher = g_dispatcher.get();
    }
    if (!dispatcher) {
        std::lock_guard lock(g_settings_mutex);
        g_apply_queued = false;
        return false;
    }
    try {
        dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
            [generation] {
                auto guard = g_call_gate.Enter();
                if (!guard || generation != g_lifecycle_generation.load(std::memory_order_acquire)) {
                    std::lock_guard lock(g_settings_mutex);
                    g_apply_queued = false;
                    return;
                }
                for (;;) {
                    Config next;
                    uint64_t revision = 0;
                    {
                        std::lock_guard lock(g_settings_mutex);
                        next = g_pending_config;
                        revision = g_pending_revision;
                    }
                    g_ui_config = std::move(next);
                    ApplyAllOnUi();
                    std::lock_guard lock(g_settings_mutex);
                    if (revision == g_pending_revision) {
                        g_apply_queued = false;
                        return;
                    }
                }
            });
        return true;
    } catch (...) {
        SetLastError(winrt::to_hresult());
        std::lock_guard lock(g_settings_mutex);
        g_apply_queued = false;
        return false;
    }
}

bool IsTaskbarWindow(HWND window) noexcept {
    DWORD owner = 0;
    wchar_t class_name[64]{};
    return IsWindow(window) && GetWindowThreadProcessId(window, &owner) != 0 &&
        owner == GetCurrentProcessId() && GetClassNameW(window, class_name, 64) != 0 &&
        wcscmp(class_name, L"Shell_TrayWnd") == 0;
}

void Classify(ElementEntry& entry) {
    const auto lower_name = Lower(std::wstring(entry.name.begin(), entry.name.end()));
    const auto lower_type = Lower(std::wstring(entry.type.begin(), entry.type.end()));
    if (entry.name == "BackgroundFill" || entry.name == "BackgroundStroke" ||
        entry.name == "TaskbarFrame") {
        entry.semantic = "surface";
    } else if (lower_name.find(L"indicator") != std::wstring::npos ||
        lower_name.find(L"underline") != std::wstring::npos) {
        entry.semantic = "indicator";
    } else if (lower_name.find(L"start") != std::wstring::npos) {
        entry.semantic = "startButton";
    } else if (lower_name.find(L"systemtray") != std::wstring::npos ||
        lower_name.find(L"notificationarea") != std::wstring::npos ||
        lower_name.find(L"clock") != std::wstring::npos ||
        lower_name.find(L"tray") != std::wstring::npos) {
        entry.semantic = "systemTray";
    } else if (lower_name.find(L"taskbaritem") != std::wstring::npos ||
        lower_name.find(L"tasklistbutton") != std::wstring::npos ||
        lower_type.find(L"button") != std::wstring::npos ||
        lower_type.find(L"taskbar") != std::wstring::npos) {
        entry.semantic = "button";
    }
}

struct CallbackStrings {
    VisualElement element{};
    ~CallbackStrings() {
        SysFreeString(element.SrcInfo.FileName);
        SysFreeString(element.SrcInfo.Hash);
        SysFreeString(element.Type);
        SysFreeString(element.Name);
    }
};

struct VisualTreeWatcher : winrt::implements<VisualTreeWatcher,
    IVisualTreeServiceCallback2> {
    HRESULT STDMETHODCALLTYPE OnVisualTreeChange(ParentChildRelation,
        VisualElement value, VisualMutationType mutation) noexcept override {
        CallbackStrings owned{value};
        auto guard = g_call_gate.Enter();
        if (!guard || !g_active.load(std::memory_order_acquire)) return S_OK;
        if (GetCurrentThreadId() != g_taskbar_thread) return S_OK;
        const uint64_t handle = static_cast<uint64_t>(value.Handle);
        if (mutation != Add) {
            const auto found = g_entries.find(handle);
            if (found != g_entries.end()) {
                RestoreEntry(found->second);
                g_entries.erase(found);
                UpdateDiagnostics();
            }
            return S_OK;
        }
        if (g_entries.contains(handle)) return S_OK;
        try {
            foundation::IInspectable inspectable{nullptr};
            winrt::check_hresult(g_xaml_diagnostics->GetIInspectableFromHandle(value.Handle,
                reinterpret_cast<::IInspectable**>(winrt::put_abi(inspectable))));
            auto framework = inspectable.try_as<xaml::FrameworkElement>();
            auto ui = inspectable.try_as<xaml::UIElement>();
            if (!framework || !ui) return S_OK;

            bool belongs_to_taskbar = false;
            std::vector<std::string> ancestor_names;
            xaml::DependencyObject parent = media::VisualTreeHelper::GetParent(framework);
            for (int depth = 0; parent && depth < 64; ++depth) {
                if (auto parent_element = parent.try_as<xaml::FrameworkElement>()) {
                    const auto name = parent_element.Name();
                    if (!name.empty()) {
                        const auto utf8_name = ToUtf8(name);
                        if (name == L"TaskbarFrame") belongs_to_taskbar = true;
                        ancestor_names.push_back(utf8_name);
                    }
                }
                parent = media::VisualTreeHelper::GetParent(parent);
            }
            const std::wstring own_name = BstrText(value.Name);
            const std::wstring own_type = BstrText(value.Type);
            if (!belongs_to_taskbar && own_name != L"TaskbarFrame") return S_OK;

            ElementEntry entry;
            entry.handle = handle;
            entry.element = framework;
            entry.ui_element = ui;
            entry.name = ToUtf8(own_name);
            entry.type = ToUtf8(own_type);
            entry.ancestor_names = std::move(ancestor_names);
            Classify(entry);
            {
                std::lock_guard lock(g_dispatcher_mutex);
                if (!g_dispatcher) g_dispatcher = winrt::agile_ref(framework.Dispatcher());
            }
            auto [it, inserted] = g_entries.emplace(handle, std::move(entry));
            if (inserted) {
                ApplyEntry(it->second);
                UpdateDiagnostics();
            }
            return S_OK;
        } catch (...) {
            SetLastError(winrt::to_hresult());
            return S_OK;
        }
    }

    HRESULT STDMETHODCALLTYPE OnElementStateChanged(InstanceHandle,
        VisualElementState, LPCWSTR) noexcept override {
        return S_OK;
    }
};

struct DiagnosticsSite : winrt::implements<DiagnosticsSite, IObjectWithSite> {
    winrt::com_ptr<IUnknown> site;

    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* value) noexcept override {
        try {
            site.copy_from(value);
            if (!site) return S_OK;
            g_xaml_diagnostics = site.as<IXamlDiagnostics>();
            g_visual_tree = site.as<IVisualTreeService>();
            g_observer = winrt::make_self<VisualTreeWatcher>().as<IVisualTreeServiceCallback2>();
            g_site_object = site;
            return S_OK;
        } catch (...) { return winrt::to_hresult(); }
    }

    HRESULT STDMETHODCALLTYPE GetSite(REFIID iid, void** result) noexcept override {
        if (!result) return E_POINTER;
        *result = nullptr;
        return site ? site->QueryInterface(iid, result) : E_FAIL;
    }
};

struct DiagnosticsFactory : winrt::implements<DiagnosticsFactory, IClassFactory> {
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid,
        void** result) noexcept override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        try { return winrt::make_self<DiagnosticsSite>()->QueryInterface(iid, result); }
        catch (...) { return winrt::to_hresult(); }
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL) noexcept override { return S_OK; }
};

HRESULT AdviseTree() noexcept {
    if (!g_visual_tree || !g_observer) return E_NOINTERFACE;
    HRESULT result = E_FAIL;
    try {
        WorkerCounter worker_counter;
        std::thread worker([&] { result = g_visual_tree->AdviseVisualTreeChange(g_observer.get()); });
        worker.join();
        g_tree_advised = SUCCEEDED(result);
        return result;
    } catch (...) { return winrt::to_hresult(); }
}

unsigned __stdcall UnadviseTreeWorker(void* raw) noexcept {
    WorkerCounter worker_counter;
    auto* operation = static_cast<UnadviseOperation*>(raw);
    operation->result = operation->visual_tree->UnadviseVisualTreeChange(
        operation->observer);
    return 0;
}

HRESULT UnadviseTree() noexcept {
    if (!g_tree_advised) return S_OK;
    if (!g_visual_tree || !g_observer) return E_NOINTERFACE;

    if (!g_unadvise_operation) {
        auto context = std::unique_ptr<UnadviseOperation>(new (std::nothrow) UnadviseOperation{});
        if (!context) return E_OUTOFMEMORY;
        context->visual_tree = g_visual_tree.get();
        context->observer = g_observer.get();
        context->visual_tree->AddRef();
        context->observer->AddRef();
        unsigned thread_id = 0;
        const uintptr_t thread = _beginthreadex(nullptr, 0,
            &UnadviseTreeWorker, context.get(), 0, &thread_id);
        if (!thread) {
            context->visual_tree->Release();
            context->observer->Release();
            return HRESULT_FROM_WIN32(GetLastError());
        }
        g_unadvise_context = context.release();
        g_unadvise_operation = reinterpret_cast<HANDLE>(thread);
    }

    const DWORD wait = WaitForSingleObject(g_unadvise_operation, kDrainTimeoutMs);
    if (wait == WAIT_TIMEOUT) return E_PENDING;
    if (wait != WAIT_OBJECT_0) return HRESULT_FROM_WIN32(GetLastError());

    const HRESULT result = g_unadvise_context->result;
    CloseHandle(g_unadvise_operation);
    g_unadvise_operation = nullptr;
    g_unadvise_context->visual_tree->Release();
    g_unadvise_context->observer->Release();
    delete g_unadvise_context;
    g_unadvise_context = nullptr;
    if (SUCCEEDED(result)) g_tree_advised = false;
    return result;
}

HRESULT ConnectDiagnostics() noexcept {
    if (g_visual_tree && g_xaml_diagnostics && g_observer) return S_OK;
    HMODULE module = GetModuleHandleW(L"Windows.UI.Xaml.dll");
    if (!module) return HRESULT_FROM_WIN32(GetLastError());
    using Initialize = HRESULT (WINAPI*)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, CLSID, LPCWSTR);
    auto initialize = reinterpret_cast<Initialize>(GetProcAddress(module, "InitializeXamlDiagnosticsEx"));
    if (!initialize) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);

    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&ConnectDiagnostics), &self)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(self, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length >= path.size()) return HRESULT_FROM_WIN32(GetLastError());
    path.resize(length);

    for (unsigned attempt = 1; attempt <= 8; ++attempt) {
        const std::wstring endpoint = L"VisualDiagConnection" + std::to_wstring(attempt);
        HRESULT result = E_FAIL;
        {
            WorkerCounter worker_counter;
            std::thread worker([&] {
                result = initialize(endpoint.c_str(), GetCurrentProcessId(), nullptr,
                    path.c_str(), kDiagnosticsClass, nullptr);
            });
            worker.join();
        }
        if (SUCCEEDED(result) && g_visual_tree && g_xaml_diagnostics && g_observer) return S_OK;
        if (FAILED(result) && result != HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) SetLastError(result);
    }
    return E_FAIL;
}

unsigned __stdcall StartDiagnosticsOperation(void*) noexcept {
    WorkerCounter worker_counter;
    HRESULT result = ConnectDiagnostics();
    if (SUCCEEDED(result)) {
        result = g_active.load(std::memory_order_acquire)
            ? AdviseTree() : HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }
    g_start_operation_result.store(result, std::memory_order_release);
    return 0;
}

bool ReapStartDiagnosticsOperation(HRESULT* result) noexcept {
    if (!g_start_operation) return false;
    if (WaitForSingleObject(g_start_operation, 0) != WAIT_OBJECT_0) return false;
    const HRESULT operation_result = g_start_operation_result.load(std::memory_order_acquire);
    CloseHandle(g_start_operation);
    g_start_operation = nullptr;
    if (result) *result = operation_result;
    return true;
}

void RestoreAllOnUi() {
    for (auto& pair : g_entries) RestoreEntry(pair.second);
    g_entries.clear();
    UpdateDiagnostics();
}

MwStatus FinishCleanup() noexcept {
    if (g_call_gate.WaitForDrain(kDrainTimeoutMs) != mw_native::CallGate::WaitResult::drained) {
        return MW_ERROR_DRAIN_PENDING;
    }

    if (g_start_operation && !ReapStartDiagnosticsOperation(nullptr)) {
        // The closed and drained call gate prevents both XAML observation and
        // UI callbacks from touching g_entries. Restore any styles already
        // applied, but leave COM state alone until the startup worker exits.
        if (!g_entries.empty()) {
            const HRESULT restored = DispatchToUi(RestoreAllOnUi, kUiDispatchTimeoutMs);
            if (FAILED(restored)) {
                SetLastError(restored);
                return restored == HRESULT_FROM_WIN32(WAIT_TIMEOUT) || restored == E_PENDING
                    ? MW_ERROR_DRAIN_PENDING : MW_ERROR_ADDON;
            }
        }
        return MW_ERROR_DRAIN_PENDING;
    }

    const HRESULT unadvise = UnadviseTree();
    if (FAILED(unadvise)) SetLastError(unadvise);

    HRESULT restored = S_OK;
    if (!g_entries.empty()) restored = DispatchToUi(RestoreAllOnUi, kUiDispatchTimeoutMs);
    if (FAILED(restored)) {
        SetLastError(restored);
        return restored == HRESULT_FROM_WIN32(WAIT_TIMEOUT) || restored == E_PENDING
            ? MW_ERROR_DRAIN_PENDING : MW_ERROR_ADDON;
    }
    if (FAILED(unadvise)) return MW_ERROR_DRAIN_PENDING;

    {
        std::lock_guard lock(g_dispatcher_mutex);
        g_dispatcher = {};
    }
    {
        std::lock_guard lock(g_settings_mutex);
        g_apply_queued = false;
    }
    g_taskbar_window = nullptr;
    g_taskbar_thread = 0;
    g_cleanup_pending.store(false, std::memory_order_release);
    return MW_OK;
}

MwStatus MW_CALL Start(const MwHostApi* host, const char* settings_json) {
    if (!mw_native::IsCompatibleHost(host)) return MW_ERROR_ABI;
    if (g_active.load(std::memory_order_acquire) ||
        g_cleanup_pending.load(std::memory_order_acquire) ||
        !g_call_gate.IsClosed() || g_call_gate.InFlight())
        return MW_ERROR_STATE;
    auto parsed = taskbar_config::ParseSettings(settings_json);
    if (!parsed) {
        SetLastError(E_INVALIDARG);
        return MW_ERROR_ARGUMENT;
    }
    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!IsTaskbarWindow(taskbar)) return MW_ERROR_STATE;
    g_taskbar_window = taskbar;
    g_taskbar_thread = GetWindowThreadProcessId(taskbar, nullptr);
    g_ui_config = parsed.config;
    {
        std::lock_guard lock(g_settings_mutex);
        g_pending_config = parsed.config;
        g_pending_revision = 0;
        g_apply_queued = false;
    }
    g_lifecycle_generation.fetch_add(1, std::memory_order_acq_rel);
    if (!g_call_gate.Open()) return MW_ERROR_STATE;
    g_active.store(true, std::memory_order_release);

    g_start_operation_result.store(E_PENDING, std::memory_order_release);
    unsigned thread_id = 0;
    const uintptr_t thread = _beginthreadex(nullptr, 0,
        &StartDiagnosticsOperation, nullptr, 0, &thread_id);
    HRESULT result = S_OK;
    if (!thread) {
        result = HRESULT_FROM_WIN32(errno ? static_cast<DWORD>(errno) : ERROR_NOT_ENOUGH_MEMORY);
    } else {
        g_start_operation = reinterpret_cast<HANDLE>(thread);
        const DWORD wait = WaitForSingleObject(g_start_operation,
            kDiagnosticsStartupTimeoutMs);
        if (wait == WAIT_OBJECT_0) {
            (void)ReapStartDiagnosticsOperation(&result);
        } else {
            g_active.store(false, std::memory_order_release);
            g_cleanup_pending.store(true, std::memory_order_release);
            g_lifecycle_generation.fetch_add(1, std::memory_order_acq_rel);
            g_call_gate.Close();
            SetLastError(wait == WAIT_TIMEOUT
                ? HRESULT_FROM_WIN32(WAIT_TIMEOUT)
                : HRESULT_FROM_WIN32(GetLastError()));
            return MW_ERROR_DRAIN_PENDING;
        }
    }
    if (FAILED(result)) {
        g_active.store(false, std::memory_order_release);
        g_cleanup_pending.store(true, std::memory_order_release);
        g_call_gate.Close();
        SetLastError(result);
        const MwStatus cleanup = FinishCleanup();
        return cleanup == MW_OK ? MW_ERROR_ADDON : cleanup;
    }
    IncrementDiagnostic(&MwTaskbarConfiguratorDiagnosticsBlock.settingsApplied);
    return MW_OK;
}

MwStatus MW_CALL SettingsChanged(const char* settings_json) {
    if (!g_active.load(std::memory_order_acquire) || g_call_gate.IsClosed()) return MW_ERROR_STATE;
    auto parsed = taskbar_config::ParseSettings(settings_json);
    if (!parsed) {
        SetLastError(E_INVALIDARG);
        return MW_ERROR_ARGUMENT;
    }
    try {
        return ScheduleSettingsApply(std::make_shared<Config>(std::move(parsed.config)))
            ? MW_OK : MW_ERROR_IO;
    } catch (...) {
        SetLastError(E_OUTOFMEMORY);
        return MW_ERROR_ADDON;
    }
}

MwStatus MW_CALL Stop() {
    const bool was_active = g_active.exchange(false, std::memory_order_acq_rel);
    if (!was_active && !g_cleanup_pending.load(std::memory_order_acquire)) return MW_OK;
    if (was_active) {
        g_cleanup_pending.store(true, std::memory_order_release);
        g_lifecycle_generation.fetch_add(1, std::memory_order_acq_rel);
    }
    g_call_gate.Close();
    return FinishCleanup();
}
} // namespace

extern "C" __declspec(dllexport) MwTaskbarConfiguratorDiagnostics
    MwTaskbarConfiguratorDiagnosticsBlock = {1, 0, 0, 0, 0, 0, 0, 0, 0};

extern "C" __declspec(dllexport) HRESULT __stdcall MwTaskbarConfiguratorGetClassObject(
    REFCLSID clsid, REFIID iid, void** result) {
    if (!result) return E_POINTER;
    *result = nullptr;
    if (clsid != kDiagnosticsClass) return CLASS_E_CLASSNOTAVAILABLE;
    try { return winrt::make_self<DiagnosticsFactory>()->QueryInterface(iid, result); }
    catch (...) { return winrt::to_hresult(); }
}

extern "C" __declspec(dllexport) HRESULT __stdcall MwTaskbarConfiguratorCanUnloadNow() {
    return S_FALSE;
}

extern "C" MW_EXPORT MwStatus MW_CALL MwAddonGetApi(uint32_t requested_abi,
    MwAddonApi* api) {
    if (!api) return MW_ERROR_ARGUMENT;
    if (requested_abi != MW_NATIVE_ABI_VERSION) return MW_ERROR_ABI;
    *api = MwAddonApi{sizeof(MwAddonApi), MW_NATIVE_ABI_VERSION,
        "taskbar-configurator", &Start, &SettingsChanged, &Stop};
    return MW_OK;
}

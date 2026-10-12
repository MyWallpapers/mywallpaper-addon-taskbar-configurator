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
#ifdef MW_TASKBAR_CALLBACK_TESTING
#include <DispatcherQueue.h>
#include <winrt/Windows.System.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <functional>
#include <iterator>
#include <limits>
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

struct UiCompletion {
    UiCompletion() noexcept : event(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~UiCompletion() { if (event) CloseHandle(event); }

    void Complete(HRESULT value) noexcept {
        result.store(value, std::memory_order_release);
        if (event) SetEvent(event);
    }

    HANDLE event = nullptr;
    std::atomic<HRESULT> result{E_PENDING};
};

struct UiCompletionResult {
    bool had_operation = false;
    bool completed = false;
    HRESULT result = S_OK;
};

class UiCompletionSlot {
public:
    using Schedule = std::function<HRESULT(const std::shared_ptr<UiCompletion>&)>;

    UiCompletionResult RunOrJoin(const Schedule& schedule, DWORD timeout_ms) noexcept {
        std::shared_ptr<UiCompletion> completion;
        bool should_schedule = false;
        try {
            {
                std::lock_guard lock(mutex_);
                completion = pending_;
                if (!completion) {
                    completion = std::make_shared<UiCompletion>();
                    if (!completion->event) {
                        return {false, true, HRESULT_FROM_WIN32(GetLastError())};
                    }
                    pending_ = completion;
                    should_schedule = true;
                }
            }

            if (should_schedule) {
                HRESULT scheduled = E_FAIL;
                try { scheduled = schedule(completion); }
                catch (...) { scheduled = winrt::to_hresult(); }
                if (FAILED(scheduled)) completion->Complete(scheduled);
            }
            return WaitAndConsume(completion, timeout_ms);
        } catch (...) {
            return {false, true, winrt::to_hresult()};
        }
    }

    UiCompletionResult WaitForExisting(DWORD timeout_ms) noexcept {
        std::shared_ptr<UiCompletion> completion;
        {
            std::lock_guard lock(mutex_);
            completion = pending_;
        }
        if (!completion) return {};
        return WaitAndConsume(completion, timeout_ms);
    }

private:
    UiCompletionResult WaitAndConsume(
        const std::shared_ptr<UiCompletion>& completion, DWORD timeout_ms) noexcept {
        const DWORD wait = WaitForSingleObject(completion->event, timeout_ms);
        if (wait != WAIT_OBJECT_0) {
            const HRESULT failure = wait == WAIT_TIMEOUT
                ? HRESULT_FROM_WIN32(WAIT_TIMEOUT)
                : HRESULT_FROM_WIN32(GetLastError());
            return {true, false, failure};
        }

        const HRESULT result = completion->result.load(std::memory_order_acquire);
        {
            std::lock_guard lock(mutex_);
            if (pending_ == completion) pending_.reset();
        }
        return {true, true, result};
    }

    std::mutex mutex_;
    std::shared_ptr<UiCompletion> pending_;
};

UiCompletionSlot g_cleanup_restore_operation;

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
    bool callback_enabled = false;
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
    bool entered_handler = false;
    bool exited_handler = false;
    bool pressed_handler = false;
    bool released_handler = false;
    bool pointer_animation_enabled = false;
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

template <typename Register>
HRESULT RegisterTrackedHandler(bool& registered, winrt::event_token& token,
    Register&& register_handler) noexcept {
    if (registered) return S_OK;
    try {
        token = register_handler();
        registered = true;
        return S_OK;
    } catch (...) {
        return winrt::to_hresult();
    }
}

template <typename Revoke>
HRESULT RevokeTrackedHandler(bool& registered, Revoke&& revoke_handler) noexcept {
    if (!registered) return S_OK;
    try {
        revoke_handler();
        registered = false;
        return S_OK;
    } catch (...) {
        return winrt::to_hresult();
    }
}

uint64_t PointerHandlerCount(const ElementEntry& entry) noexcept {
    return static_cast<uint64_t>(entry.entered_handler) +
        static_cast<uint64_t>(entry.exited_handler) +
        static_cast<uint64_t>(entry.pressed_handler) +
        static_cast<uint64_t>(entry.released_handler);
}

void RecordFirstFailure(HRESULT* first_error, HRESULT result) noexcept {
    if (first_error && SUCCEEDED(*first_error) && FAILED(result)) *first_error = result;
}

template <typename RevokeEntered, typename RevokeExited,
    typename RevokePressed, typename RevokeReleased>
HRESULT RevokeFourTrackedHandlers(
    bool& entered_registered, RevokeEntered&& revoke_entered,
    bool& exited_registered, RevokeExited&& revoke_exited,
    bool& pressed_registered, RevokePressed&& revoke_pressed,
    bool& released_registered, RevokeReleased&& revoke_released) noexcept {
    HRESULT result = S_OK;
    RecordFirstFailure(&result, RevokeTrackedHandler(entered_registered,
        std::forward<RevokeEntered>(revoke_entered)));
    RecordFirstFailure(&result, RevokeTrackedHandler(exited_registered,
        std::forward<RevokeExited>(revoke_exited)));
    RecordFirstFailure(&result, RevokeTrackedHandler(pressed_registered,
        std::forward<RevokePressed>(revoke_pressed)));
    RecordFirstFailure(&result, RevokeTrackedHandler(released_registered,
        std::forward<RevokeReleased>(revoke_released)));
    return result;
}

bool HasTrackedState(const ElementEntry& entry) noexcept {
    return !entry.properties.empty() || PointerHandlerCount(entry) != 0 ||
        entry.has_visual || entry.rotation_saved;
}

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
    if ((name == "width" || name == "height") && value == "auto") {
        return winrt::box_value(std::numeric_limits<double>::quiet_NaN());
    }
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
        if (config.button_width > 0.0) AddNumber(entry, desired, "width", config.button_width);
        if (config.button_height > 0.0) AddNumber(entry, desired, "height", config.button_height);
        if (config.button_rotation_degrees != 0.0)
            AddNumber(entry, desired, "rotation", config.button_rotation_degrees);
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
        handlers += PointerHandlerCount(entry);
        if (entry.animation_counted) ++animations;
    }
    SetDiagnostic(&MwTaskbarConfiguratorDiagnosticsBlock.matchedElements, matched);
    SetDiagnostic(&MwTaskbarConfiguratorDiagnosticsBlock.retainedProperties, properties);
    SetDiagnostic(&MwTaskbarConfiguratorDiagnosticsBlock.eventHandlers, handlers);
    SetDiagnostic(&MwTaskbarConfiguratorDiagnosticsBlock.animations, animations);
}

HRESULT RemovePropertyCallback(ElementEntry& entry, PropertyState& state) noexcept {
    state.callback_enabled = false;
    if (!state.has_callback) return S_OK;
    try {
        entry.element.UnregisterPropertyChangedCallback(state.property, state.callback_token);
        state.has_callback = false;
        state.callback_token = 0;
        return S_OK;
    } catch (...) {
        const HRESULT error = winrt::to_hresult();
        SetLastError(error);
        return error;
    }
}

bool IsUnset(const foundation::IInspectable& value) {
    return value == xaml::DependencyProperty::UnsetValue();
}

/*
 * The original value is restored even if callback removal fails. The gate
 * keeps callbacks from re-entering cleanup, and retained state lets a later
 * Stop retry the exact failed unregister.
 */
HRESULT RestoreProperty(ElementEntry& entry, PropertyState& state) noexcept {
    HRESULT result = RemovePropertyCallback(entry, state);
    if (state.name == "rotation") {
        if (entry.rotation_saved) {
            if (!entry.has_visual || !entry.visual) return FAILED(result) ? result : E_PENDING;
            bool restored = true;
            try { entry.visual.StopAnimation(L"RotationAngle"); }
            catch (...) {
                const HRESULT error = winrt::to_hresult();
                SetLastError(error);
                RecordFirstFailure(&result, error);
                restored = false;
            }
            try {
                entry.visual.RotationAngle(entry.original_rotation);
                IncrementDiagnostic(&MwTaskbarConfiguratorDiagnosticsBlock.restoreCount);
            } catch (...) {
                const HRESULT error = winrt::to_hresult();
                SetLastError(error);
                RecordFirstFailure(&result, error);
                restored = false;
            }
            if (restored) {
                entry.rotation_saved = false;
            } else if (SUCCEEDED(result)) {
                result = E_PENDING;
            }
        }
        if (!entry.rotation_saved && PointerHandlerCount(entry) == 0 && !entry.animation_counted) {
            entry.has_visual = false;
            entry.visual = nullptr;
        }
        return result;
    }

    try {
        auto object = entry.element.try_as<xaml::DependencyObject>();
        if (!object) return FAILED(result) ? result : E_PENDING;
        // Once another component installs a binding, it owns the property's
        // value. Remove our callback, but never overwrite that binding.
        if (entry.element.GetBindingExpression(state.property)) return result;
        entry.writing = true;
        if (IsUnset(state.original)) object.ClearValue(state.property);
        else object.SetValue(state.property, state.original);
        entry.writing = false;
        IncrementDiagnostic(&MwTaskbarConfiguratorDiagnosticsBlock.restoreCount);
    } catch (...) {
        entry.writing = false;
        const HRESULT error = winrt::to_hresult();
        SetLastError(error);
        RecordFirstFailure(&result, error);
    }
    return result;
}

void PropertyChanged(uint64_t handle, xaml::DependencyProperty property,
    foundation::IInspectable const& sender) noexcept;

HRESULT RegisterPropertyCallback(ElementEntry& entry, PropertyState& state) noexcept {
    if (state.has_callback) {
        state.callback_enabled = true;
        return S_OK;
    }
    try {
        state.callback_token = entry.element.RegisterPropertyChangedCallback(state.property,
            [handle = entry.handle, property = state.property](auto const& sender, auto const&) {
                PropertyChanged(handle, property, sender);
            });
        state.has_callback = true;
        state.callback_enabled = true;
        return S_OK;
    } catch (...) {
        const HRESULT error = winrt::to_hresult();
        SetLastError(error);
        state.callback_enabled = false;
        return error;
    }
}

bool ApplyProperty(ElementEntry& entry, PropertyState& desired) {
    if (desired.name == "rotation") {
        if (!entry.has_visual) return false;
        if (!entry.rotation_saved) {
            entry.original_rotation = entry.visual.RotationAngle();
            entry.rotation_saved = true;
        }
        entry.visual.RotationAngleInDegrees(winrt::unbox_value<float>(desired.desired));
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
        (void)RegisterPropertyCallback(entry, stored);
        entry.properties.push_back(std::move(stored));
        state = std::prev(entry.properties.end());
    } else {
        state->desired = desired.desired;
        (void)RegisterPropertyCallback(entry, *state);
    }

    entry.writing = true;
    try {
        object.SetValue(state->property, state->desired);
    } catch (...) {
        entry.writing = false;
        SetLastError(winrt::to_hresult());
        const HRESULT restored = RestoreProperty(entry, *state);
        if (SUCCEEDED(restored)) entry.properties.erase(state);
        else SetLastError(restored);
        return false;
    }
    entry.writing = false;
    return true;
}

HRESULT RestoreVisual(ElementEntry& entry) noexcept {
    if (!entry.has_visual) return S_OK;
    if (!entry.visual) return E_PENDING;

    HRESULT result = S_OK;
    bool restored = true;
    try { entry.visual.StopAnimation(L"Scale"); }
    catch (...) {
        const HRESULT error = winrt::to_hresult();
        SetLastError(error);
        RecordFirstFailure(&result, error);
        restored = false;
    }
    try { entry.visual.StopAnimation(L"Opacity"); }
    catch (...) {
        const HRESULT error = winrt::to_hresult();
        SetLastError(error);
        RecordFirstFailure(&result, error);
        restored = false;
    }
    try {
        entry.visual.Scale(entry.original_scale);
        IncrementDiagnostic(&MwTaskbarConfiguratorDiagnosticsBlock.restoreCount);
    } catch (...) {
        const HRESULT error = winrt::to_hresult();
        SetLastError(error);
        RecordFirstFailure(&result, error);
        restored = false;
    }
    try {
        entry.visual.Opacity(entry.original_opacity);
        IncrementDiagnostic(&MwTaskbarConfiguratorDiagnosticsBlock.restoreCount);
    } catch (...) {
        const HRESULT error = winrt::to_hresult();
        SetLastError(error);
        RecordFirstFailure(&result, error);
        restored = false;
    }

    if (!restored) return FAILED(result) ? result : E_PENDING;
    entry.pointer_inside = false;
    entry.pointer_pressed = false;
    entry.animation_counted = false;
    if (!entry.rotation_saved) {
        entry.has_visual = false;
        entry.visual = nullptr;
    }
    return S_OK;
}

HRESULT RevokePointerHandlers(ElementEntry& entry) noexcept {
    if (PointerHandlerCount(entry) == 0) return S_OK;
    if (!entry.element) {
        SetLastError(E_PENDING);
        return E_PENDING;
    }

    const HRESULT result = RevokeFourTrackedHandlers(
        entry.entered_handler, [&] { entry.element.PointerEntered(entry.entered_token); },
        entry.exited_handler, [&] { entry.element.PointerExited(entry.exited_token); },
        entry.pressed_handler, [&] { entry.element.PointerPressed(entry.pressed_token); },
        entry.released_handler, [&] { entry.element.PointerReleased(entry.released_token); });
    if (FAILED(result)) SetLastError(result);
    return result;
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
        entry.pointer_animation_enabled = false;
        const HRESULT revoked = RevokePointerHandlers(entry);
        const HRESULT restored = RestoreVisual(entry);
        if (FAILED(revoked)) SetLastError(revoked);
        if (FAILED(restored)) SetLastError(restored);
        return;
    }
    if (!entry.has_visual) {
        entry.visual = winrt::Windows::UI::Xaml::Hosting::ElementCompositionPreview::GetElementVisual(entry.ui_element);
        entry.original_scale = entry.visual.Scale();
        entry.original_opacity = entry.visual.Opacity();
        entry.has_visual = true;
    }
    const auto handle = entry.handle;
    HRESULT result = RegisterTrackedHandler(entry.entered_handler, entry.entered_token, [&] {
        return entry.element.PointerEntered([handle](auto const&, auto const&) {
            auto guard = g_call_gate.Enter();
            if (!guard) return;
            const auto found = g_entries.find(handle);
            if (found == g_entries.end() || !found->second.pointer_animation_enabled) return;
            found->second.pointer_inside = true;
            Animate(found->second, found->second.pointer_pressed);
        });
    });
    if (SUCCEEDED(result)) result = RegisterTrackedHandler(entry.exited_handler, entry.exited_token, [&] {
        return entry.element.PointerExited([handle](auto const&, auto const&) {
            auto guard = g_call_gate.Enter();
            if (!guard) return;
            const auto found = g_entries.find(handle);
            if (found == g_entries.end() || !found->second.pointer_animation_enabled) return;
            found->second.pointer_inside = false;
            Animate(found->second, false);
        });
    });
    if (SUCCEEDED(result)) result = RegisterTrackedHandler(entry.pressed_handler, entry.pressed_token, [&] {
        return entry.element.PointerPressed([handle](auto const&, auto const&) {
            auto guard = g_call_gate.Enter();
            if (!guard) return;
            const auto found = g_entries.find(handle);
            if (found == g_entries.end() || !found->second.pointer_animation_enabled) return;
            found->second.pointer_pressed = true;
            Animate(found->second, true);
        });
    });
    if (SUCCEEDED(result)) result = RegisterTrackedHandler(entry.released_handler, entry.released_token, [&] {
        return entry.element.PointerReleased([handle](auto const&, auto const&) {
            auto guard = g_call_gate.Enter();
            if (!guard) return;
            const auto found = g_entries.find(handle);
            if (found == g_entries.end() || !found->second.pointer_animation_enabled) return;
            found->second.pointer_pressed = false;
            Animate(found->second, false);
        });
    });
    if (FAILED(result)) {
        SetLastError(result);
        entry.pointer_animation_enabled = false;
        (void)RevokePointerHandlers(entry);
        (void)RestoreVisual(entry);
        entry.matched = HasTrackedState(entry);
        UpdateDiagnostics();
        return;
    }
    entry.pointer_animation_enabled = true;
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
                config.surface_inset_horizontal + config.surface_offset_x,
                config.surface_inset_vertical + config.surface_offset_y,
                config.surface_inset_horizontal - config.surface_offset_x,
                config.surface_inset_vertical - config.surface_offset_y}));
            if (config.surface_width_mode == "content")
                AddDesiredProperty(entry, desired, "width", Json("auto"));
            else if (config.surface_width_mode == "fixed")
                AddNumber(entry, desired, "width", config.surface_width);
            if (config.surface_height > 0.0)
                AddNumber(entry, desired, "height", config.surface_height);
            if (config.surface_alignment != "native")
                AddDesiredProperty(entry, desired, "horizontalAlignment", Json(config.surface_alignment));
        }
    } else if (entry.semantic == "indicator") {
        AddColor(entry, desired, "fill", config.indicator_color);
        AddNumber(entry, desired, "strokeThickness", config.indicator_thickness);
        AddNumber(entry, desired, "cornerRadius", config.indicator_radius);
        if (!config.indicator_visible)
            AddDesiredProperty(entry, desired, "visibility", Json("collapsed"));
    } else {
        ConfigureMotion(entry, config, desired);
    }

    // Let the existing XAML layout measure the dock; hit testing follows that
    // layout rather than a detached Composition offset or a second taskbar.
    if (entry.name == "RootGrid" && config.surface_width_mode == "content" &&
        std::find(entry.ancestor_names.begin(), entry.ancestor_names.end(),
            "TaskbarFrame") != entry.ancestor_names.end()) {
        AddDesiredProperty(entry, desired, "width", Json("auto"));
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
        const HRESULT restored = RestoreProperty(entry, *it);
        if (SUCCEEDED(restored)) it = entry.properties.erase(it);
        else {
            SetLastError(restored);
            ++it;
        }
    }

    entry.writing = true;
    for (auto& target : desired) {
        try { (void)ApplyProperty(entry, target); }
        catch (...) { SetLastError(winrt::to_hresult()); }
    }
    entry.writing = false;

    ConfigureAdvancedMotion(entry, g_ui_config);
    ConfigurePointerHandlers(entry);
    entry.matched = HasTrackedState(entry);
    UpdateDiagnostics();
}

HRESULT RestoreEntry(ElementEntry& entry) noexcept {
    entry.pointer_animation_enabled = false;
    HRESULT result = RevokePointerHandlers(entry);
    RecordFirstFailure(&result, RestoreVisual(entry));
    for (std::size_t index = entry.properties.size(); index > 0; --index) {
        const HRESULT restored = RestoreProperty(entry, entry.properties[index - 1]);
        if (SUCCEEDED(restored)) entry.properties.erase(entry.properties.begin() + index - 1);
        else {
            SetLastError(restored);
            RecordFirstFailure(&result, restored);
        }
    }
    const bool rotation_state_tracked = std::any_of(entry.properties.begin(),
        entry.properties.end(), [](const PropertyState& property) {
            return property.name == "rotation";
        });
    if (entry.rotation_saved && !rotation_state_tracked) {
        PropertyState orphaned_rotation;
        orphaned_rotation.name = "rotation";
        const HRESULT restored = RestoreProperty(entry, orphaned_rotation);
        RecordFirstFailure(&result, restored);
    }
    entry.matched = HasTrackedState(entry);
    if (!entry.matched && SUCCEEDED(result)) return S_OK;
    return FAILED(result) ? result : E_PENDING;
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
    if (state == entry.properties.end() || !state->callback_enabled) return;
    try {
        auto object = sender.try_as<xaml::DependencyObject>();
        if (!object) return;
        if (entry.element.GetBindingExpression(property)) {
            const HRESULT removed = RemovePropertyCallback(entry, *state);
            if (SUCCEEDED(removed)) entry.properties.erase(state);
            else SetLastError(removed);
            entry.matched = HasTrackedState(entry);
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

void ApplyAllOnUi() {
    SetLastError(S_OK);
    for (auto& pair : g_entries) ApplyEntry(pair.second);
    IncrementDiagnostic(&MwTaskbarConfiguratorDiagnosticsBlock.settingsApplied);
}

bool ClearApplyQueuedForGeneration(uint64_t generation) noexcept {
    std::lock_guard lock(g_settings_mutex);
    if (generation != g_lifecycle_generation.load(std::memory_order_acquire)) return false;
    g_apply_queued = false;
    return true;
}

void AdvanceLifecycleGeneration() noexcept {
    std::lock_guard lock(g_settings_mutex);
    g_lifecycle_generation.fetch_add(1, std::memory_order_acq_rel);
}

bool ScheduleSettingsApply(const std::shared_ptr<Config>& config) {
    uint64_t generation = 0;
    auto queue = [&] {
        std::lock_guard lock(g_settings_mutex);
        generation = g_lifecycle_generation.load(std::memory_order_acquire);
        g_pending_config = *config;
        ++g_pending_revision;
        if (g_apply_queued) return false;
        g_apply_queued = true;
        return true;
    }();
    if (!queue) return true;
    winrt::Windows::UI::Core::CoreDispatcher dispatcher{nullptr};
    {
        std::lock_guard lock(g_dispatcher_mutex);
        if (g_dispatcher) dispatcher = g_dispatcher.get();
    }
    if (!dispatcher) {
        (void)ClearApplyQueuedForGeneration(generation);
        return false;
    }
    try {
        dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
            [generation] {
                auto guard = g_call_gate.Enter();
                {
                    std::lock_guard lock(g_settings_mutex);
                    if (generation != g_lifecycle_generation.load(std::memory_order_acquire)) return;
                    if (!guard) {
                        g_apply_queued = false;
                        return;
                    }
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
                        if (generation == g_lifecycle_generation.load(std::memory_order_acquire)) {
                            g_apply_queued = false;
                        }
                        return;
                    }
                }
            });
        return true;
    } catch (...) {
        SetLastError(winrt::to_hresult());
        (void)ClearApplyQueuedForGeneration(generation);
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

#ifdef MW_TASKBAR_CALLBACK_TESTING
extern "C" void WINAPI MwTaskbarTestRecordForbiddenBstrFree(BSTR) noexcept;
#define SysFreeString MwTaskbarTestRecordForbiddenBstrFree
#endif

struct VisualTreeWatcher : winrt::implements<VisualTreeWatcher,
    IVisualTreeServiceCallback2> {
    explicit VisualTreeWatcher(winrt::com_ptr<IXamlDiagnostics> diagnostics)
        : diagnostics_(std::move(diagnostics)) {}

    foundation::IInspectable ResolveElement(InstanceHandle handle) const {
        foundation::IInspectable inspectable{nullptr};
        // Handles belong to the cache that produced this observer's callbacks.
        // A later SetSite must not change the cache underneath an older callback.
        if (!diagnostics_) winrt::throw_hresult(E_UNEXPECTED);
        const HRESULT result = diagnostics_->GetIInspectableFromHandle(handle,
            reinterpret_cast<::IInspectable**>(winrt::put_abi(inspectable)));
        // Windows can release an element between reporting it and resolving it.
        // Skip this stale notification; the next live element is still usable.
        if (result == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) return nullptr;
        winrt::check_hresult(result);
        return inspectable;
    }

    HRESULT STDMETHODCALLTYPE OnVisualTreeChange(ParentChildRelation,
        VisualElement value, VisualMutationType mutation) noexcept override {
        auto guard = g_call_gate.Enter();
        if (!guard || !g_active.load(std::memory_order_acquire)) return S_OK;
        if (GetCurrentThreadId() != g_taskbar_thread) return S_OK;
        const uint64_t handle = static_cast<uint64_t>(value.Handle);
        if (mutation != Add) {
            const auto found = g_entries.find(handle);
            if (found != g_entries.end()) {
                if (SUCCEEDED(RestoreEntry(found->second))) g_entries.erase(found);
                UpdateDiagnostics();
            }
            return S_OK;
        }
        if (g_entries.contains(handle)) return S_OK;
        try {
            const auto inspectable = ResolveElement(value.Handle);
            if (!inspectable) return S_OK;
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

private:
    const winrt::com_ptr<IXamlDiagnostics> diagnostics_;
};

struct DiagnosticsSite : winrt::implements<DiagnosticsSite, IObjectWithSite> {
    winrt::com_ptr<IUnknown> site;

    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* value) noexcept override {
        try {
            site.copy_from(value);
            if (!site) return S_OK;
            auto diagnostics = site.as<IXamlDiagnostics>();
            g_visual_tree = site.as<IVisualTreeService>();
            g_observer = winrt::make_self<VisualTreeWatcher>(std::move(diagnostics))
                .as<IVisualTreeServiceCallback2>();
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
    if (g_visual_tree && g_site_object && g_observer) return S_OK;
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
        if (SUCCEEDED(result) && g_visual_tree && g_site_object && g_observer) return S_OK;
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

HRESULT RestoreAllOnUi() noexcept {
    HRESULT result = S_OK;
    for (auto it = g_entries.begin(); it != g_entries.end();) {
        const HRESULT restored = RestoreEntry(it->second);
        if (SUCCEEDED(restored) && !HasTrackedState(it->second)) {
            it = g_entries.erase(it);
        } else {
            RecordFirstFailure(&result, FAILED(restored) ? restored : E_PENDING);
            ++it;
        }
    }
    UpdateDiagnostics();
    if (!g_entries.empty()) return FAILED(result) ? result : E_PENDING;
    return result;
}

HRESULT ScheduleRestoreAllOnUi(
    const std::shared_ptr<UiCompletion>& completion) noexcept {
    try {
        winrt::Windows::UI::Core::CoreDispatcher dispatcher{nullptr};
        {
            std::lock_guard lock(g_dispatcher_mutex);
            if (g_dispatcher) dispatcher = g_dispatcher.get();
        }
        if (!dispatcher) return E_PENDING;
        dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
            [completion] {
                HRESULT result = E_FAIL;
                try { result = RestoreAllOnUi(); }
                catch (...) { result = winrt::to_hresult(); }
                completion->Complete(result);
            });
        return S_OK;
    } catch (...) {
        return winrt::to_hresult();
    }
}

MwStatus FinishCleanup() noexcept {
    if (g_call_gate.WaitForDrain(kDrainTimeoutMs) != mw_native::CallGate::WaitResult::drained) {
        SetLastError(E_PENDING);
        return MW_ERROR_DRAIN_PENDING;
    }

    // A timed-out UI callback can still be traversing g_entries after its last
    // erase. Reap the exact operation before reading the map or allowing Start.
    const UiCompletionResult previous_restore =
        g_cleanup_restore_operation.WaitForExisting(kUiDispatchTimeoutMs);
    if (previous_restore.had_operation &&
        (!previous_restore.completed || FAILED(previous_restore.result))) {
        SetLastError(previous_restore.result);
        return MW_ERROR_DRAIN_PENDING;
    }

    if (g_start_operation && !ReapStartDiagnosticsOperation(nullptr)) {
        // The closed and drained call gate prevents both XAML observation and
        // UI callbacks from touching g_entries. Restore any styles already
        // applied, but leave COM state alone until the startup worker exits.
        if (!g_entries.empty()) {
            const UiCompletionResult restored = g_cleanup_restore_operation.RunOrJoin(
                &ScheduleRestoreAllOnUi, kUiDispatchTimeoutMs);
            if (!restored.completed || FAILED(restored.result)) {
                SetLastError(restored.result);
                return MW_ERROR_DRAIN_PENDING;
            }
        }
        return MW_ERROR_DRAIN_PENDING;
    }

    const HRESULT unadvise = UnadviseTree();
    if (FAILED(unadvise)) SetLastError(unadvise);

    if (!g_entries.empty()) {
        const UiCompletionResult result = g_cleanup_restore_operation.RunOrJoin(
            &ScheduleRestoreAllOnUi, kUiDispatchTimeoutMs);
        if (!result.completed || FAILED(result.result)) {
            SetLastError(result.result);
            return MW_ERROR_DRAIN_PENDING;
        }
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
    SetLastError(S_OK);
    g_cleanup_pending.store(false, std::memory_order_release);
    return MW_OK;
}

MwStatus MW_CALL Start(const MwHostApi* host, const char* settings_json) {
    if (!mw_native::IsCompatibleHost(host)) return MW_ERROR_ABI;
    if (g_active.load(std::memory_order_acquire) ||
        g_cleanup_pending.load(std::memory_order_acquire) ||
        !g_call_gate.IsClosed() || g_call_gate.InFlight())
        return MW_ERROR_STATE;
    SetLastError(S_OK);
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
        g_lifecycle_generation.fetch_add(1, std::memory_order_acq_rel);
        g_pending_config = parsed.config;
        g_pending_revision = 0;
        g_apply_queued = false;
    }
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
            AdvanceLifecycleGeneration();
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
        if (cleanup == MW_OK) SetLastError(result);
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
        AdvanceLifecycleGeneration();
    }
    g_call_gate.Close();
    return FinishCleanup();
}
} // namespace

#ifdef MW_TASKBAR_CALLBACK_TESTING
extern "C" HRESULT __stdcall MwTaskbarTestRotationAndAutoDimension() noexcept {
    try {
        struct Apartment {
            Apartment() { winrt::init_apartment(winrt::apartment_type::single_threaded); }
            ~Apartment() { winrt::clear_factory_cache(); winrt::uninit_apartment(); }
        } apartment;
        winrt::clear_factory_cache();
        winrt::Windows::System::DispatcherQueueController queue{nullptr};
        const DispatcherQueueOptions options{sizeof(DispatcherQueueOptions),
            DQTYPE_THREAD_CURRENT, DQTAT_COM_STA};
        winrt::check_hresult(CreateDispatcherQueueController(options,
            reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(queue))));
        composition::Compositor compositor;
        if (!compositor) return E_POINTER;
        ElementEntry entry;
        entry.visual = compositor.CreateContainerVisual();
        if (!entry.visual) return E_POINTER;
        entry.has_visual = true;
        entry.visual.RotationAngle(0.5f);
        const auto visual = entry.visual;
        PropertyState desired;
        desired.name = "rotation";
        desired.desired = winrt::box_value(30.0f);
        if (!ApplyProperty(entry, desired) ||
            std::abs(entry.visual.RotationAngleInDegrees() - 30.0f) > 0.01f)
            return E_FAIL;
        if (FAILED(RestoreProperty(entry, entry.properties.front())) ||
            std::abs(visual.RotationAngle() - 0.5f) > 0.001f || entry.has_visual)
            return E_FAIL;
        auto automatic = MakePropertyValue(nullptr, "width", Json("auto"));
        return std::isnan(winrt::unbox_value<double>(automatic)) ? S_OK : E_FAIL;
    } catch (...) { return winrt::to_hresult(); }
}

struct DiagnosticCacheFixture : winrt::implements<DiagnosticCacheFixture,
    IXamlDiagnostics> {
    explicit DiagnosticCacheFixture(int32_t marker)
        : object(winrt::box_value(marker)) {}

    HRESULT STDMETHODCALLTYPE GetIInspectableFromHandle(InstanceHandle handle,
        ::IInspectable** result) noexcept override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (handle == 101) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        if (handle == 303) return E_ACCESSDENIED;
        if (handle != 202) return E_INVALIDARG;
        return reinterpret_cast<::IUnknown*>(winrt::get_abi(object))
            ->QueryInterface(__uuidof(::IInspectable), reinterpret_cast<void**>(result));
    }
    HRESULT STDMETHODCALLTYPE GetDispatcher(::IInspectable**) noexcept override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetUiLayer(::IInspectable**) noexcept override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetApplication(::IInspectable**) noexcept override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetHandleFromIInspectable(::IInspectable*, InstanceHandle*) noexcept override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE HitTest(RECT, unsigned int*, InstanceHandle**) noexcept override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE RegisterInstance(::IInspectable*, InstanceHandle*) noexcept override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetInitializationData(BSTR*) noexcept override { return E_NOTIMPL; }
    foundation::IInspectable object;
};

extern "C" HRESULT __stdcall MwTaskbarTestDiagnosticCacheResolution(
    uint32_t* expired_skipped, uint32_t* correct_cache,
    uint32_t* cache_retained, uint32_t* unexpected_error_preserved) noexcept {
    if (!expired_skipped || !correct_cache || !cache_retained ||
        !unexpected_error_preserved) return E_POINTER;
    *expired_skipped = *correct_cache = *cache_retained = *unexpected_error_preserved = 0;
    try {
        struct Apartment {
            Apartment() { winrt::init_apartment(); }
            ~Apartment() { winrt::uninit_apartment(); }
        } apartment;
        auto first = winrt::make_self<DiagnosticCacheFixture>(7).as<IXamlDiagnostics>();
        auto second = winrt::make_self<DiagnosticCacheFixture>(9).as<IXamlDiagnostics>();
        auto first_watcher = winrt::make_self<VisualTreeWatcher>(first);
        auto second_watcher = winrt::make_self<VisualTreeWatcher>(second);
        *expired_skipped = !first_watcher->ResolveElement(101);
        *correct_cache = winrt::unbox_value<int32_t>(first_watcher->ResolveElement(202)) == 7 &&
            winrt::unbox_value<int32_t>(second_watcher->ResolveElement(202)) == 9;
        first = nullptr;
        *cache_retained = winrt::unbox_value<int32_t>(first_watcher->ResolveElement(202)) == 7;
        try {
            (void)first_watcher->ResolveElement(303);
        } catch (const winrt::hresult_error& error) {
            *unexpected_error_preserved = error.code() == E_ACCESSDENIED;
        }
        return S_OK;
    } catch (...) { return winrt::to_hresult(); }
}

extern "C" HRESULT __stdcall MwTaskbarTestInvokeOnVisualTreeChange(
    VisualElement element) noexcept {
    try {
        auto watcher = winrt::make_self<VisualTreeWatcher>(nullptr);
        return watcher->OnVisualTreeChange({}, element, Add);
    } catch (...) {
        return winrt::to_hresult();
    }
}

extern "C" HRESULT __stdcall MwTaskbarTestPartialHandlerRollback(
    uint32_t* registered_before_rollback, uint32_t* attempted_rollback,
    uint32_t* remaining_after_rollback, HRESULT* registration_result) noexcept {
    if (!registered_before_rollback || !attempted_rollback ||
        !remaining_after_rollback || !registration_result) return E_POINTER;
    bool registered[4]{};
    winrt::event_token tokens[4]{};
    *registration_result = RegisterTrackedHandler(registered[0], tokens[0],
        [] { return winrt::event_token{}; });
    if (SUCCEEDED(*registration_result)) {
        *registration_result = RegisterTrackedHandler(registered[1], tokens[1],
            [] { return winrt::event_token{}; });
    }
    if (SUCCEEDED(*registration_result)) {
        *registration_result = RegisterTrackedHandler(registered[2], tokens[2],
            []() -> winrt::event_token { throw winrt::hresult_error(E_FAIL); });
    }
    *registered_before_rollback = 0;
    for (uint32_t index = 0; index < 4; ++index) {
        if (registered[index]) *registered_before_rollback |= (1u << index);
    }
    *attempted_rollback = 0;
    const HRESULT rollback = RevokeFourTrackedHandlers(
        registered[0], [&] { *attempted_rollback |= 1u; },
        registered[1], [&] { *attempted_rollback |= 2u; },
        registered[2], [&] { *attempted_rollback |= 4u; },
        registered[3], [&] { *attempted_rollback |= 8u; });
    *remaining_after_rollback = 0;
    for (uint32_t index = 0; index < 4; ++index) {
        if (registered[index]) *remaining_after_rollback |= (1u << index);
    }
    return rollback;
}

extern "C" HRESULT __stdcall MwTaskbarTestIndependentHandlerRevocation(
    uint32_t* first_attempted, uint32_t* first_remaining,
    uint32_t* retry_attempted, uint32_t* final_remaining) noexcept {
    if (!first_attempted || !first_remaining || !retry_attempted ||
        !final_remaining) return E_POINTER;
    bool registered[4]{true, true, true, true};
    *first_attempted = 0;
    const HRESULT first = RevokeFourTrackedHandlers(
        registered[0], [&] { *first_attempted |= 1u; },
        registered[1], [&] { *first_attempted |= 2u; throw winrt::hresult_error(E_FAIL); },
        registered[2], [&] { *first_attempted |= 4u; },
        registered[3], [&] { *first_attempted |= 8u; });
    *first_remaining = 0;
    for (uint32_t index = 0; index < 4; ++index) {
        if (registered[index]) *first_remaining |= (1u << index);
    }
    *retry_attempted = 0;
    const HRESULT retry = RevokeFourTrackedHandlers(
        registered[0], [&] { *retry_attempted |= 1u; },
        registered[1], [&] { *retry_attempted |= 2u; },
        registered[2], [&] { *retry_attempted |= 4u; },
        registered[3], [&] { *retry_attempted |= 8u; });
    *final_remaining = 0;
    for (uint32_t index = 0; index < 4; ++index) {
        if (registered[index]) *final_remaining |= (1u << index);
    }
    if (FAILED(first)) return first;
    return retry;
}

extern "C" HRESULT __stdcall MwTaskbarTestLateCleanupCompletion(
    uint32_t* scheduled_count, uint32_t* timeout_before_callback,
    uint32_t* timeout_during_diagnostics, uint32_t* map_reads_after_initial_stop,
    uint32_t* map_reads_after_pending_retries,
    uint32_t* map_reads_after_completion, uint32_t* diagnostics_reads,
    uint32_t* start_allowed_before_completion,
    uint32_t* start_allowed_after_completion) noexcept {
    if (!scheduled_count || !timeout_before_callback ||
        !timeout_during_diagnostics || !map_reads_after_initial_stop ||
        !map_reads_after_pending_retries || !map_reads_after_completion ||
        !diagnostics_reads || !start_allowed_before_completion ||
        !start_allowed_after_completion) return E_POINTER;
    *scheduled_count = 0;
    *timeout_before_callback = 0;
    *timeout_during_diagnostics = 0;
    *map_reads_after_initial_stop = 0;
    *map_reads_after_pending_retries = 0;
    *map_reads_after_completion = 0;
    *diagnostics_reads = 0;
    *start_allowed_before_completion = 0;
    *start_allowed_after_completion = 0;

    struct TestEvent {
        HANDLE value = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ~TestEvent() { if (value) CloseHandle(value); }
    } entered, finish_diagnostics;
    if (!entered.value || !finish_diagnostics.value) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    UiCompletionSlot slot;
    std::function<void()> queued_callback;
    std::map<uint32_t, uint32_t> simulated_entries{{1, 1}};
    bool simulated_cleanup_pending = true;
    uint32_t safe_map_reads = 0;
    const auto schedule = [&](const std::shared_ptr<UiCompletion>& completion) -> HRESULT {
        ++*scheduled_count;
        queued_callback = [&, completion] {
            simulated_entries.clear();
            SetEvent(entered.value);
            const DWORD wait = WaitForSingleObject(finish_diagnostics.value, 5000);
            if (wait != WAIT_OBJECT_0) {
                completion->Complete(HRESULT_FROM_WIN32(wait == WAIT_TIMEOUT
                    ? WAIT_TIMEOUT : GetLastError()));
                return;
            }
            (void)simulated_entries.empty(); // mirrors the trailing diagnostics map walk
            ++*diagnostics_reads;
            completion->Complete(S_OK);
        };
        return S_OK;
    };
    const auto stop_retry = [&]() -> bool {
        const UiCompletionResult previous = slot.WaitForExisting(25);
        if (previous.had_operation &&
            (!previous.completed || FAILED(previous.result))) {
            return false; // FinishCleanup returns before inspecting g_entries.
        }
        ++safe_map_reads;
        if (simulated_entries.empty()) {
            simulated_cleanup_pending = false;
            return true;
        }

        const UiCompletionResult restore = slot.RunOrJoin(schedule, 25);
        if (!restore.completed || FAILED(restore.result)) return false;
        ++safe_map_reads;
        if (simulated_entries.empty()) {
            simulated_cleanup_pending = false;
            return true;
        }
        return false;
    };

    if (stop_retry()) return E_UNEXPECTED;
    ++*timeout_before_callback;
    *map_reads_after_initial_stop = safe_map_reads;
    if (*scheduled_count != 1 || safe_map_reads != 1 || !simulated_cleanup_pending) return E_FAIL;

    if (stop_retry()) return E_UNEXPECTED;
    ++*timeout_before_callback;
    if (*scheduled_count != 1 || safe_map_reads != 1 || !simulated_cleanup_pending) return E_FAIL;

    std::thread callback_thread;
    try {
        callback_thread = std::thread(queued_callback);
    } catch (...) {
        SetEvent(finish_diagnostics.value);
        return E_OUTOFMEMORY;
    }
    const DWORD entered_wait = WaitForSingleObject(entered.value, 2000);
    if (entered_wait != WAIT_OBJECT_0) {
        SetEvent(finish_diagnostics.value);
        callback_thread.join();
        return HRESULT_FROM_WIN32(entered_wait == WAIT_TIMEOUT
            ? WAIT_TIMEOUT : GetLastError());
    }

    if (stop_retry()) {
        SetEvent(finish_diagnostics.value);
        callback_thread.join();
        return E_UNEXPECTED;
    }
    ++*timeout_during_diagnostics;
    *map_reads_after_pending_retries = safe_map_reads;
    *start_allowed_before_completion = simulated_cleanup_pending ? 0u : 1u;
    if (*scheduled_count != 1 || safe_map_reads != 1 || !simulated_cleanup_pending) {
        SetEvent(finish_diagnostics.value);
        callback_thread.join();
        return E_FAIL;
    }

    SetEvent(finish_diagnostics.value);
    callback_thread.join();
    const bool cleanup_complete = stop_retry();
    *map_reads_after_completion = safe_map_reads;
    *start_allowed_after_completion = simulated_cleanup_pending ? 0u : 1u;
    if (!cleanup_complete || *scheduled_count != 1 || *diagnostics_reads != 1 ||
        *map_reads_after_initial_stop != 1 ||
        *map_reads_after_pending_retries != 1 ||
        *map_reads_after_completion != 2 ||
        *start_allowed_before_completion != 0 ||
        *start_allowed_after_completion != 1) return E_FAIL;
    return S_OK;
}

extern "C" HRESULT __stdcall MwTaskbarTestStaleApplyGeneration(
    uint32_t* stale_preserved_queue, uint32_t* current_cleared_queue) noexcept {
    if (!stale_preserved_queue || !current_cleared_queue) return E_POINTER;
    const uint64_t previous_generation = g_lifecycle_generation.load(std::memory_order_acquire);
    bool previous_queued = false;
    {
        std::lock_guard lock(g_settings_mutex);
        previous_queued = g_apply_queued;
        g_lifecycle_generation.store(17, std::memory_order_release);
        g_apply_queued = true;
    }
    (void)ClearApplyQueuedForGeneration(16);
    {
        std::lock_guard lock(g_settings_mutex);
        *stale_preserved_queue = g_apply_queued ? 1u : 0u;
    }
    (void)ClearApplyQueuedForGeneration(17);
    {
        std::lock_guard lock(g_settings_mutex);
        *current_cleared_queue = g_apply_queued ? 0u : 1u;
        g_apply_queued = previous_queued;
        g_lifecycle_generation.store(previous_generation, std::memory_order_release);
    }
    return (*stale_preserved_queue == 1 && *current_cleared_queue == 1)
        ? S_OK : E_FAIL;
}
#endif

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

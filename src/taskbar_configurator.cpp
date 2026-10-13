#define TASKBAR_CONFIGURATOR_BUILD
#include "mw_native_sdk.hpp"
#include "mw_call_gate.hpp"
#include "taskbar_config.hpp"
#include "taskbar_diagnostics.h"
#include "xaml_values.hpp"

#include <Windows.h>
#include <ocidl.h>
#include <xamlom.h>
#undef GetCurrentTime

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Automation.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Media.Media3D.h>
#include <winrt/Windows.UI.Xaml.Markup.h>
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
#include <cstdio>
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
uint64_t g_ui_profile_revision = 0;

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
// One process-lifetime observer per resident add-on image; styling and all
// behavior subscriptions still stop. Fields: version, observers, initial
// enumerations, UI snapshots and inactive notifications.
extern "C" __declspec(dllexport) uint64_t MwTaskbarConfiguratorLifecycleDiagnosticsBlock[5] = {
    1, 0, 0, 0, 0};

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

UiCompletionSlot g_ui_lifecycle_operation;

struct MotionSettings {
    double hover_scale = 1.0;
    double press_scale = 1.0;
    double hover_opacity = 1.0;
    double press_opacity = 1.0;
    uint32_t hover_duration_ms = 0;
    uint32_t press_duration_ms = 0;
    taskbar_config::Easing easing = taskbar_config::Easing::ease_out;
    bool operator==(const MotionSettings&) const = default;
};

struct PropertyState {
    xaml::DependencyProperty property{nullptr};
    std::string name;
    foundation::IInspectable original{nullptr};
    foundation::IInspectable desired{nullptr};
    int64_t callback_token = 0;
    bool has_callback = false;
    bool callback_enabled = false;
    Json collection_values;
    Json source_value;
    std::vector<foundation::IInspectable> collection_original;
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
    std::vector<std::pair<xaml::VisualStateGroup, winrt::event_token>> state_handlers;
    std::vector<std::pair<xaml::DependencyProperty, int64_t>> capture_handlers;
    uint64_t subscription_revision = 0;
    winrt::event_token theme_token{};
    bool theme_handler = false;

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
const MwHostApi* g_host_api = nullptr;
std::map<std::string, bool> g_reported_rules;
void ReportRule(const std::string& message) {
    if (g_reported_rules.size() >= 64 || g_reported_rules.contains(message)) return;
    g_reported_rules.emplace(message,true);
    if (g_host_api && g_host_api->log) (void)g_host_api->log(g_host_api->addon_context,message.c_str());
}

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
        entry.has_visual || entry.rotation_saved || !entry.state_handlers.empty() || !entry.capture_handlers.empty() || entry.theme_handler;
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
    if (auto panel = element.try_as<controls::Panel>()) {
        if (name == "background") return controls::Panel::BackgroundProperty();
    }
    if (element.try_as<shapes::Rectangle>()) {
        if (name == "radiusX") return shapes::Rectangle::RadiusXProperty();
        if (name == "radiusY") return shapes::Rectangle::RadiusYProperty();
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
        name == "maxHeight" || name == "fontSize" || name == "rotation" || name == "radiusX" || name == "radiusY") {
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

Json ReadPrimitive(const foundation::IInspectable& value) {
    if (auto property = value.try_as<foundation::IPropertyValue>()) {
        switch (property.Type()) {
        case foundation::PropertyType::Double:
            return property.GetDouble();
        case foundation::PropertyType::Single:
            return property.GetSingle();
        case foundation::PropertyType::Int32:
            return property.GetInt32();
        case foundation::PropertyType::UInt32:
            return property.GetUInt32();
        case foundation::PropertyType::Boolean:
            return property.GetBoolean();
        case foundation::PropertyType::String:
            return winrt::to_string(property.GetString());
        default:
            break;
        }
    }
    return nullptr;
}
void Classify(ElementEntry &entry);
xaml::DependencyProperty CaptureProperty(const ElementEntry &entry, const std::string &property);
bool SelectorMatches(const taskbar_config::Selector &selector, const ElementEntry &entry) {
    if (!selector.semantic.empty() && selector.semantic != entry.semantic)
        return false;
    if (!selector.name.empty() && selector.name != entry.name)
        return false;
    if (!selector.type.empty() &&
        (selector.exact_type ? entry.type != selector.type
                             : entry.type.find(selector.type) == std::string::npos))
        return false;
    if (!selector.ancestor_name.empty() &&
        std::find(entry.ancestor_names.begin(), entry.ancestor_names.end(),
                  selector.ancestor_name) == entry.ancestor_names.end())
        return false;
    auto parent = media::VisualTreeHelper::GetParent(entry.element);
    if (selector.root && parent)
        return false;
    if (selector.index) {
        if (!parent ||
            selector.index >
                static_cast<uint32_t>(media::VisualTreeHelper::GetChildrenCount(parent)) ||
            media::VisualTreeHelper::GetChild(parent, static_cast<int>(selector.index) - 1) !=
                entry.element)
            return false;
    }
    for (auto item = selector.where.begin(); item != selector.where.end(); ++item) {
        try {
            const auto property = CaptureProperty(entry, item.key());
            if (ReadPrimitive(entry.element.GetValue(property)) != item.value())
                return false;
        } catch (...) {
            return false;
        }
    }
    for (const auto &expected : selector.parents) {
        auto element = parent.try_as<xaml::FrameworkElement>();
        if (!element)
            return false;
        ElementEntry ancestor;
        ancestor.element = element;
        ancestor.name = winrt::to_string(element.Name());
        ancestor.type = winrt::to_string(winrt::get_class_name(element));
        Classify(ancestor);
        auto relative = media::VisualTreeHelper::GetParent(element);
        for (unsigned depth = 0; relative && depth < 64; ++depth) {
            if (auto named = relative.try_as<xaml::FrameworkElement>())
                ancestor.ancestor_names.push_back(winrt::to_string(named.Name()));
            relative = media::VisualTreeHelper::GetParent(relative);
        }
        if (!SelectorMatches(expected, ancestor))
            return false;
        parent = media::VisualTreeHelper::GetParent(parent);
    }
    return true;
}

Json g_live_variables = Json::object();
std::map<std::string, foundation::IInspectable> g_simple_value_cache;
Json ExpandValue(const ElementEntry &entry, const Json &value, unsigned depth = 0) {
    if (depth > 16)
        throw std::runtime_error("cyclic value reference");
    if (value.is_array()) {
        auto result = Json::array();
        for (const auto &v : value)
            result.push_back(ExpandValue(entry, v, depth + 1));
        return result;
    }
    if (!value.is_object())
        return value;
    if (value.contains("expression"))
        return taskbar_config::EvaluateExpression(value.at("expression").get<std::string>(),
                                                  g_live_variables);
    if (value.contains("ref")) {
        const auto found = g_live_variables.find(value.at("ref").get<std::string>());
        return found == g_live_variables.end() ? Json(nullptr)
                                               : ExpandValue(entry, *found, depth + 1);
    }
    if (value.contains("resource")) {
        const auto found = g_ui_config.resources.find(value.at("resource").get<std::string>());
        if (found != g_ui_config.resources.end()) {
            const bool dark = entry.element.ActualTheme() == xaml::ElementTheme::Dark;
            if (found->is_object() && found->contains(dark ? "dark" : "light"))
                return ExpandValue(entry, found->at(dark ? "dark" : "light"), depth + 1);
            return ExpandValue(entry, *found, depth + 1);
        }
    }
    auto result = value;
    for (auto it = result.begin(); it != result.end(); ++it)
        it.value() = ExpandValue(entry, it.value(), depth + 1);
    return result;
}

void AddDesiredProperty(ElementEntry &entry, std::vector<PropertyState> *desired,
                        const std::string &property_name, const Json &value, bool strict = false) {
    if (property_name == "ColumnDefinitions" || property_name == "RowDefinitions") {
        if (!entry.element.try_as<controls::Grid>()) {
            SetLastError(E_INVALIDARG);
            return;
        }
        PropertyState state;
        state.name = property_name;
        state.collection_values = ExpandValue(entry, value);
        const auto existing = std::find_if(desired->begin(), desired->end(), [&](const auto &item) {
            return item.name == property_name;
        });
        if (existing == desired->end())
            desired->push_back(std::move(state));
        else
            *existing = std::move(state);
        return;
    }
    if (property_name == "cornerRadius" && entry.element.try_as<shapes::Rectangle>()) {
        AddDesiredProperty(entry, desired, "radiusX", value, strict);
        AddDesiredProperty(entry, desired, "radiusY", value, strict);
        return;
    }
    if (property_name == "rotation") {
        const auto expanded = ExpandValue(entry, value);
        if (expanded.is_null() || expanded.is_object())
            return;
        if (!entry.has_visual) {
            entry.visual =
                winrt::Windows::UI::Xaml::Hosting::ElementCompositionPreview::GetElementVisual(
                    entry.ui_element);
            entry.original_scale = entry.visual.Scale();
            entry.original_opacity = entry.visual.Opacity();
            entry.original_rotation = entry.visual.RotationAngle();
            entry.rotation_saved = true;
            entry.has_visual = true;
        }
        PropertyState state;
        state.name = property_name;
        state.desired = winrt::box_value(static_cast<float>(expanded.get<double>()));
        desired->push_back(std::move(state));
        return;
    }
    auto property = PropertyFor(entry.element, property_name);
    // A guided semantic target spans different native types. Skip a legacy
    // control that has no corresponding property on this particular element;
    // explicit rules use strict resolution and report incompatible targets.
    if (!property && !strict && !property_name.empty() &&
        std::islower(static_cast<unsigned char>(property_name.front()))) return;
    foundation::IInspectable inspectable{nullptr};
    Json expanded;
    try {
        expanded = ExpandValue(entry, value);
        if (expanded.is_object() && expanded.contains("$skip"))
            return;
        if (value.is_object() && (value.contains("expression") || value.contains("ref")) &&
            expanded.is_null())
            return;
        const auto previous = std::find_if(entry.properties.begin(), entry.properties.end(), [&](const auto& state) {
            return property ? state.property == property : state.name == property_name;
        });
        if (previous != entry.properties.end() &&
            (previous->source_value == expanded || taskbar_xaml::TryUpdateBrush(previous->desired, previous->source_value, expanded))) {
            property = previous->property;
            inspectable = previous->desired;
        }
        if (!inspectable && property && !expanded.is_object() && !expanded.is_null() &&
            !(property_name == "cornerRadius" && expanded.is_array())) {
            const auto key = entry.type + ":" + property_name + ":" + expanded.dump();
            auto found = g_simple_value_cache.find(key);
            if (found != g_simple_value_cache.end())
                inspectable = found->second;
            else {
                inspectable = MakePropertyValue(entry.element, property_name, expanded);
                if (inspectable) {
                    if (g_simple_value_cache.size() >= 1024)
                        g_simple_value_cache.clear();
                    g_simple_value_cache.emplace(key, inspectable);
                }
            }
        }
        if (!property || !inspectable) {
            const auto resolved = taskbar_xaml::Resolve(entry.element, property_name, expanded, property);
            property = resolved.property;
            inspectable = resolved.value;
        }
    } catch (...) {
        const auto failure = winrt::to_hresult();
        SetLastError(failure);
        ReportRule("Cannot apply " + property_name + " on " + entry.type + "#" + entry.name +
                   " (HRESULT " + std::to_string(static_cast<int32_t>(failure)) + ")");
        return;
    }
    PropertyState state;
    state.property = property;
    state.name = property_name;
    state.desired = std::move(inspectable);
    state.source_value = std::move(expanded);
    const auto previous = std::find_if(desired->begin(), desired->end(),
                                       [&](const auto &v) { return v.property == state.property; });
    if (previous == desired->end())
        desired->push_back(std::move(state));
    else
        *previous = std::move(state);
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
        handlers += PointerHandlerCount(entry) + entry.state_handlers.size() + entry.capture_handlers.size() + static_cast<uint64_t>(entry.theme_handler);
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

bool IsUnset(const foundation::IInspectable &value) {
    return value == xaml::DependencyProperty::UnsetValue();
}
bool SameProperty(const PropertyState &a, const PropertyState &b) {
    return a.property && b.property ? a.property == b.property
                                    : !a.property && !b.property && a.name == b.name;
}
xaml::GridLength GridSize(const Json &value) {
    if (value.is_number())
        return {value.get<double>(), xaml::GridUnitType::Pixel};
    const auto text = value.get<std::string>();
    if (text == "Auto" || text == "auto")
        return {1, xaml::GridUnitType::Auto};
    const bool star = !text.empty() && text.back() == '*';
    const auto literal = star ? text.substr(0, text.size() - 1) : text;
    size_t used = 0;
    const double size = literal.empty() && star ? 1 : std::stod(literal, &used);
    if ((!literal.empty() && used != literal.size()) || !std::isfinite(size) || size < 0)
        winrt::throw_hresult(E_INVALIDARG);
    return {size, star ? xaml::GridUnitType::Star : xaml::GridUnitType::Pixel};
}
void SetGridCollection(ElementEntry &entry, const PropertyState &state, bool restore) {
    const auto grid = entry.element.as<controls::Grid>();
    if (state.name == "ColumnDefinitions") {
        std::vector<controls::ColumnDefinition> next;
        if (restore)
            for (const auto &value : state.collection_original)
                next.push_back(value.as<controls::ColumnDefinition>());
        else if (!state.collection_values.is_null())
            for (const auto &value : state.collection_values.at("sizes")) {
                controls::ColumnDefinition column;
                column.Width(GridSize(value));
                next.push_back(column);
            }
        grid.ColumnDefinitions().ReplaceAll(next);
    } else {
        std::vector<controls::RowDefinition> next;
        if (restore)
            for (const auto &value : state.collection_original)
                next.push_back(value.as<controls::RowDefinition>());
        else if (!state.collection_values.is_null())
            for (const auto &value : state.collection_values.at("sizes")) {
                controls::RowDefinition row;
                row.Height(GridSize(value));
                next.push_back(row);
            }
        grid.RowDefinitions().ReplaceAll(next);
    }
}

/*
 * The original value is restored even if callback removal fails. The gate
 * keeps callbacks from re-entering cleanup, and retained state lets a later
 * Stop retry the exact failed unregister.
 */
HRESULT RestoreProperty(ElementEntry& entry, PropertyState& state) noexcept {
    HRESULT result = RemovePropertyCallback(entry, state);
    if(state.name=="ColumnDefinitions" || state.name=="RowDefinitions") {
        try { SetGridCollection(entry,state,true);IncrementDiagnostic(&MwTaskbarConfiguratorDiagnosticsBlock.restoreCount); }
        catch (...) { RecordFirstFailure(&result,winrt::to_hresult()); }
        return result;
    }
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

bool ApplyProperty(ElementEntry &entry, PropertyState &desired) {
    if (desired.name == "ColumnDefinitions" || desired.name == "RowDefinitions") {
        auto stored = std::find_if(entry.properties.begin(), entry.properties.end(),
                                   [&](const auto &state) { return SameProperty(state, desired); });
        if (stored == entry.properties.end()) {
            const auto grid = entry.element.as<controls::Grid>();
            if (desired.name == "ColumnDefinitions")
                for (const auto &column : grid.ColumnDefinitions())
                    desired.collection_original.push_back(column);
            else
                for (const auto &row : grid.RowDefinitions())
                    desired.collection_original.push_back(row);
            entry.properties.push_back(std::move(desired));
            stored = std::prev(entry.properties.end());
        } else if (stored->collection_values == desired.collection_values)
            return true;
        else
            stored->collection_values = desired.collection_values;
        try {
            SetGridCollection(entry, *stored, false);
            return true;
        } catch (...) {
            SetLastError(winrt::to_hresult());
            if (SUCCEEDED(RestoreProperty(entry, *stored)))
                entry.properties.erase(stored);
            return false;
        }
    }
    if (desired.name == "rotation") {
        if (!entry.has_visual)
            return false;
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
    if (entry.element.GetBindingExpression(desired.property)) {ReportRule("Preserved Windows binding: "+desired.name+" on "+entry.type+"#"+entry.name);return false;}

    auto state = std::find_if(entry.properties.begin(), entry.properties.end(),
        [&](const PropertyState& candidate) { return candidate.property == desired.property; });
    if (state == entry.properties.end()) {
        PropertyState stored;
        stored.property = desired.property;
        stored.name = desired.name;
        stored.original = object.ReadLocalValue(stored.property);
        stored.desired = desired.desired;
        stored.source_value = desired.source_value;
        (void)RegisterPropertyCallback(entry, stored);
        entry.properties.push_back(std::move(stored));
        state = std::prev(entry.properties.end());
    } else {
        if (state->desired == desired.desired) { state->source_value = desired.source_value; return true; }
        state->desired = desired.desired;
        state->source_value = desired.source_value;
        (void)RegisterPropertyCallback(entry, *state);
    }

    entry.writing = true;
    try {
        if (IsUnset(state->desired)) object.ClearValue(state->property);
        else object.SetValue(state->property, state->desired);
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

void ApplyEntry(ElementEntry& entry, bool configure_motion = true);
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
            if (g_ui_config.appearance.contains(found->second.semantic)) ApplyEntry(found->second, false);
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
            found->second.pointer_pressed = false;
            if (g_ui_config.appearance.contains(found->second.semantic)) ApplyEntry(found->second, false);
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
            if (g_ui_config.appearance.contains(found->second.semantic)) ApplyEntry(found->second, false);
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
            if (g_ui_config.appearance.contains(found->second.semantic)) ApplyEntry(found->second, false);
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

bool IsActiveAppearanceState(std::string_view semantic,
    std::wstring_view group_name, std::wstring_view state_name) noexcept {
    if (semantic == "button") {
        return group_name == L"CommonStates" &&
            (state_name == L"ActiveNormal" || state_name == L"ActivePointerOver" ||
             state_name == L"ActivePressed");
    }
    return semantic == "indicator" && group_name == L"RunningIndicatorStates" &&
        state_name == L"ActiveRunningIndicator";
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
        AddNumber(entry, desired, "height", config.indicator_thickness);
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
        if (!rule.enabled || !SelectorMatches(rule.selector, entry)) continue;
        if (!rule.state.empty()) {
            bool active = false;
            xaml::DependencyObject current = entry.element;
            for (int depth = 0; current && depth < 64 && !active; ++depth) {
                if (auto target = current.try_as<xaml::FrameworkElement>())
                    for (const auto& group : xaml::VisualStateManager::GetVisualStateGroups(target))
                        if (winrt::to_string(group.Name()) == rule.state_group && group.CurrentState() && winrt::to_string(group.CurrentState().Name()) == rule.state) active = true;
                current = media::VisualTreeHelper::GetParent(current);
            }
            if (!active) continue;
        }
        for (auto property = rule.properties.begin(); property != rule.properties.end(); ++property) {
            AddDesiredProperty(entry, desired, property.key(), property.value(), true);
        }
    }
    const auto region = config.appearance.find(entry.semantic);
    if (region != config.appearance.end()) {
        // Consumer choices override the inherited flat controls/expert rules.
        // Hover inherits normal, pressed inherits both, without duplicated data.
        bool active = false;
        if (region->contains("active")) {
            xaml::DependencyObject current = entry.element;
            for (unsigned depth = 0; current && depth < 32 && !active; ++depth) {
                if (auto target = current.try_as<xaml::FrameworkElement>())
                    for (const auto& group : xaml::VisualStateManager::GetVisualStateGroups(target)) {
                        const auto state = group.CurrentState();
                        if (state && IsActiveAppearanceState(entry.semantic, group.Name(), state.Name()))
                            active = true;
                    }
                current = media::VisualTreeHelper::GetParent(current);
            }
        }
        for (const auto* state : {"normal", "active", "hover", "pressed"}) {
            if (strcmp(state, "active") == 0 && !active) continue;
            if ((strcmp(state, "hover") == 0 && !entry.pointer_inside) ||
                (strcmp(state, "pressed") == 0 && !entry.pointer_pressed)) continue;
            const auto values = region->find(state);
            if (values == region->end()) continue;
            for (auto property = values->begin(); property != values->end(); ++property) {
                auto key = property.key();
                if (entry.semantic == "indicator" && key == "background") key = "fill";
                if (entry.element.try_as<shapes::Rectangle>()) {
                    if (key == "borderBrush") key = "Stroke";
                    else if (key == "borderThickness") key = "StrokeThickness";
                }
                AddDesiredProperty(entry, desired, key, property.value());
            }
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

void ApplyAllOnUi();
bool g_dynamic_queued = false;
void QueueDynamicApply() noexcept {
    if (g_dynamic_queued || !g_active.load(std::memory_order_acquire))
        return;
    try {
        if (!g_dispatcher)
            return;
        const auto generation = g_lifecycle_generation.load(std::memory_order_acquire);
        g_dynamic_queued = true;
        g_dispatcher.get().RunAsync(
            winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [generation] {
                auto guard = g_call_gate.Enter();
                if (generation != g_lifecycle_generation.load(std::memory_order_acquire))
                    return;
                g_dynamic_queued = false;
                if (guard)
                    ApplyAllOnUi();
            });
    } catch (...) {
        g_dynamic_queued = false;
        SetLastError(winrt::to_hresult());
    }
}
HRESULT RevokeRuleHandlers(ElementEntry &entry) noexcept {
    HRESULT result = S_OK;
    for (auto it = entry.state_handlers.begin(); it != entry.state_handlers.end();) {
        try {
            it->first.CurrentStateChanged(it->second);
            it = entry.state_handlers.erase(it);
        } catch (...) {
            RecordFirstFailure(&result, winrt::to_hresult());
            ++it;
        }
    }
    for (auto it = entry.capture_handlers.begin(); it != entry.capture_handlers.end();) {
        try {
            entry.element.UnregisterPropertyChangedCallback(it->first, it->second);
            it = entry.capture_handlers.erase(it);
        } catch (...) {
            RecordFirstFailure(&result, winrt::to_hresult());
            ++it;
        }
    }
    if (entry.theme_handler) {
        try {
            entry.element.ActualThemeChanged(entry.theme_token);
            entry.theme_handler = false;
        } catch (...) {
            RecordFirstFailure(&result, winrt::to_hresult());
        }
    }
    if (SUCCEEDED(result))
        entry.subscription_revision = 0;
    return result;
}
xaml::DependencyProperty CaptureProperty(const ElementEntry &entry, const std::string &property) {
    if (property == "ActualWidth" || property == "actualWidth")
        return xaml::FrameworkElement::ActualWidthProperty();
    if (property == "ActualHeight" || property == "actualHeight")
        return xaml::FrameworkElement::ActualHeightProperty();
    return taskbar_xaml::Resolve(entry.element, property, nullptr).property;
}
void ConfigureRuleHandlers(ElementEntry &entry) {
    if (g_ui_profile_revision == entry.subscription_revision)
        return;
    if (FAILED(RevokeRuleHandlers(entry)))
        return;
    if (!g_ui_config.enabled) {
        entry.subscription_revision = g_ui_profile_revision;
        return;
    }
    try {
        for (const auto &rule : g_ui_config.rules) {
            if (!rule.enabled || rule.state_group.empty() || !SelectorMatches(rule.selector, entry))
                continue;
            xaml::DependencyObject current = entry.element;
            for (unsigned depth = 0; current && depth < 64; ++depth) {
                if (auto target = current.try_as<xaml::FrameworkElement>())
                    for (const auto &group :
                         xaml::VisualStateManager::GetVisualStateGroups(target)) {
                        if (winrt::to_string(group.Name()) != rule.state_group ||
                            std::any_of(entry.state_handlers.begin(), entry.state_handlers.end(),
                                        [&](const auto &pair) { return pair.first == group; }))
                            continue;
                        const auto token =
                            group.CurrentStateChanged([](auto const &, auto const &) {
                                auto guard = g_call_gate.Enter();
                                if (guard)
                                    QueueDynamicApply();
                            });
                        entry.state_handlers.emplace_back(group, token);
                    }
                current = media::VisualTreeHelper::GetParent(current);
            }
        }
        for (const auto &capture : g_ui_config.captures) {
            if (!SelectorMatches(taskbar_config::ParseSelectorJson(capture.at("selector")), entry))
                continue;
            const auto property = CaptureProperty(entry, capture.at("property").get<std::string>());
            if (std::any_of(entry.capture_handlers.begin(), entry.capture_handlers.end(),
                            [&](const auto &pair) { return pair.first == property; }))
                continue;
            const auto token = entry.element.RegisterPropertyChangedCallback(
                property, [handle = entry.handle](auto const &, auto const &) {
                    auto guard = g_call_gate.Enter();
                    if (!guard)
                        return;
                    const auto found = g_entries.find(handle);
                    if (found != g_entries.end() && !found->second.writing)
                        QueueDynamicApply();
                });
            entry.capture_handlers.emplace_back(property, token);
        }
        if (!g_ui_config.resources.empty() || !g_ui_config.rules.empty()) {
            entry.theme_token = entry.element.ActualThemeChanged([](auto const &, auto const &) {
                auto guard = g_call_gate.Enter();
                if (guard) {
                    taskbar_xaml::ClearCache();
                    QueueDynamicApply();
                }
            });
            entry.theme_handler = true;
        }
        entry.subscription_revision = g_ui_profile_revision;
    } catch (...) {
        SetLastError(winrt::to_hresult());
        (void)RevokeRuleHandlers(entry);
    }
}

void ReadCaptures() {
    g_live_variables = g_ui_config.variables;
    for (const auto &capture : g_ui_config.captures) {
        const auto selector = taskbar_config::ParseSelectorJson(capture.at("selector"));
        for (const auto &[handle, entry] : g_entries) {
            (void)handle;
            if (!SelectorMatches(selector, entry))
                continue;
            try {
                g_live_variables[capture.at("name").get<std::string>()] =
                    ReadPrimitive(entry.element.GetValue(
                        CaptureProperty(entry, capture.at("property").get<std::string>())));
                break;
            } catch (...) {
                SetLastError(winrt::to_hresult());
            }
        }
    }
}

void ApplyEntry(ElementEntry& entry, bool configure_motion) {
    const auto previous_motion = entry.motion;
    const auto previous_scale = entry.base_scale;
    ConfigureRuleHandlers(entry);
    std::vector<PropertyState> desired;
    BuildDirectProperties(entry, g_ui_config, &desired);

    for (auto it = entry.properties.begin(); it != entry.properties.end();) {
        const bool still_desired = std::any_of(desired.begin(), desired.end(),
            [&](const PropertyState& target) { return SameProperty(target, *it); });
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
    if (const auto motion = g_ui_config.motion.find(entry.semantic); motion != g_ui_config.motion.end()) {
        entry.base_scale = motion->value("size", entry.base_scale);
        entry.motion.hover_scale = motion->value("hoverSize", entry.motion.hover_scale);
        entry.motion.press_scale = motion->value("pressSize", entry.motion.press_scale);
        entry.motion.hover_opacity = motion->value("hoverOpacity", entry.motion.hover_opacity);
        entry.motion.press_opacity = motion->value("pressOpacity", entry.motion.press_opacity);
        entry.motion.hover_duration_ms = motion->value("hoverDurationMs", entry.motion.hover_duration_ms);
        entry.motion.press_duration_ms = motion->value("pressDurationMs", entry.motion.press_duration_ms);
        if (motion->contains("easing")) entry.motion.easing = motion->at("easing") == "linear" ? taskbar_config::Easing::linear : motion->at("easing") == "ease-in-out" ? taskbar_config::Easing::ease_in_out : taskbar_config::Easing::ease_out;
    }
    if (configure_motion || previous_motion != entry.motion || previous_scale != entry.base_scale) {
        ConfigurePointerHandlers(entry);
        if (entry.pointer_inside || entry.pointer_pressed) Animate(entry, entry.pointer_pressed);
    }
    entry.matched = HasTrackedState(entry);
    UpdateDiagnostics();
}

HRESULT RestoreEntry(ElementEntry& entry) noexcept {
    entry.pointer_animation_enabled = false;
    HRESULT result = RevokePointerHandlers(entry);
    RecordFirstFailure(&result, RevokeRuleHandlers(entry));
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
        if (IsUnset(state->desired)) object.ClearValue(property);
        else object.SetValue(property, state->desired);
        entry.writing = false;
    } catch (...) {
        entry.writing = false;
        SetLastError(winrt::to_hresult());
    }
}

void ApplyAllOnUi() {
    SetLastError(S_OK);
    ReadCaptures();
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
                    const auto previous_appearance = g_ui_config.appearance;
                    auto base_previous = g_ui_config, base_next = next;
                    base_previous.appearance = base_next.appearance = Json::object();
                    const bool appearance_only = base_previous == base_next;
                    if (g_ui_config.enabled != next.enabled || g_ui_config.rules != next.rules ||
                        g_ui_config.captures != next.captures || g_ui_config.resources != next.resources)
                        ++g_ui_profile_revision;
                    g_ui_config = std::move(next);
                    g_reported_rules.clear();
                    if (appearance_only) {
                        SetLastError(S_OK);
                        for (auto& [handle, entry] : g_entries) {
                            (void)handle;
                            const auto before = previous_appearance.find(entry.semantic);
                            const auto after = g_ui_config.appearance.find(entry.semantic);
                            if ((before == previous_appearance.end()) != (after == g_ui_config.appearance.end()) ||
                                (before != previous_appearance.end() && after != g_ui_config.appearance.end() && *before != *after))
                                ApplyEntry(entry, false);
                        }
                        IncrementDiagnostic(&MwTaskbarConfiguratorDiagnosticsBlock.settingsApplied);
                    } else ApplyAllOnUi();
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
    } else if (lower_name.find(L"start") != std::wstring::npos ||
        xaml::Automation::AutomationProperties::GetAutomationId(entry.element) == L"StartButton") {
        entry.semantic = "startButton";
    } else if (lower_name.find(L"search") != std::wstring::npos || lower_type.find(L"searchboxbutton") != std::wstring::npos) {
        entry.semantic = "search";
    } else if (lower_type.find(L"augmentedentrypoint") != std::wstring::npos) {
        entry.semantic = "widgets";
    } else if (lower_name.find(L"clock") != std::wstring::npos) {
        entry.semantic = "clock";
    } else if (lower_name == L"icon" || lower_type.find(L"image") != std::wstring::npos) {
        entry.semantic = "icon";
    } else if (lower_type.find(L"textblock") != std::wstring::npos) {
        entry.semantic = "label";
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

    void RememberRoot(const xaml::UIElement &root) {
        if (!root)
            return;
        for (auto it = roots_.begin(); it != roots_.end();) {
            const auto existing = it->get();
            if (!existing)
                it = roots_.erase(it);
            else {
                if (existing == root)
                    return;
                ++it;
            }
        }
        if (roots_.size() >= 8)
            winrt::throw_hresult(E_BOUNDS);
        roots_.push_back(winrt::make_weak(root));
    }

    HRESULT SnapshotOnUi() noexcept {
        try {
            size_t visited = 0;
            bool live_root = false;
            // Copy weak roots: observing descendants may discover another root.
            const auto roots = roots_;
            for (const auto &weak : roots)
                if (const auto root = weak.get()) {
                    live_root = true;
                    SnapshotElement(root, 0, visited, {});
                }
            if (!live_root)
                return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
            IncrementDiagnostic(&MwTaskbarConfiguratorLifecycleDiagnosticsBlock[3]);
            return S_OK;
        } catch (...) {
            return winrt::to_hresult();
        }
    }

    void ObserveElement(uint64_t handle, const xaml::FrameworkElement &framework,
                        std::wstring_view own_name, std::wstring_view own_type,
                        const std::vector<std::string> *snapshot_ancestors = nullptr) {
        if (g_entries.contains(handle))
            return;
        const auto ui = framework.try_as<xaml::UIElement>();
        if (!ui)
            return;
        bool belongs_to_taskbar = false;
        std::vector<std::string> ancestor_names;
        if (snapshot_ancestors)
            ancestor_names = *snapshot_ancestors;
        else {
            xaml::DependencyObject parent = media::VisualTreeHelper::GetParent(framework);
            for (int depth = 0; parent && depth < 64; ++depth) {
                if (auto parent_element = parent.try_as<xaml::FrameworkElement>()) {
                    const auto name = parent_element.Name();
                    if (!name.empty())
                        ancestor_names.push_back(ToUtf8(name));
                }
                parent = media::VisualTreeHelper::GetParent(parent);
            }
        }
        for (const auto &name : ancestor_names)
            if (name == "TaskbarFrame" || name == "SystemTrayFrame" ||
                name == "SystemTrayFrameGrid")
                belongs_to_taskbar = true;
        if (!belongs_to_taskbar && own_name != L"TaskbarFrame" && own_name != L"SystemTrayFrame" &&
            own_name != L"SystemTrayFrameGrid" &&
            own_type.find(L"Taskbar.") == std::wstring_view::npos &&
            own_type.find(L"SystemTray.") == std::wstring_view::npos)
            return;
        if (const auto root = framework.XamlRoot())
            RememberRoot(root.Content());
        else if (own_name == L"TaskbarFrame" || own_name == L"SystemTrayFrame" ||
                 own_type == L"Taskbar.TaskbarFrame" || own_type == L"SystemTray.SystemTrayFrame")
            // An island may be reported before XamlRoot is attached. Its weak
            // frame still lets the UI queue read the finished tree afterwards.
            RememberRoot(ui);
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
            if (!g_dispatcher) {
                const auto dispatcher = framework.Dispatcher();
                if (dispatcher)
                    g_dispatcher = winrt::agile_ref(dispatcher);
            }
        }
        auto [it, inserted] = g_entries.emplace(handle, std::move(entry));
        if (inserted) {
            ApplyEntry(it->second);
            if (!g_ui_config.captures.empty())
                QueueDynamicApply();
            UpdateDiagnostics();
        }
    }

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
        if (!guard || !g_active.load(std::memory_order_acquire)) {
            IncrementDiagnostic(&MwTaskbarConfiguratorLifecycleDiagnosticsBlock[4]);
            return S_OK;
        }
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
            if (framework)
                ObserveElement(handle, framework, BstrText(value.Name), BstrText(value.Type));
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
  void SnapshotElement(const xaml::DependencyObject &object, unsigned depth, size_t &visited,
                       const std::vector<std::string> &ancestors) {
      if (depth > 64 || ++visited > 32768)
          winrt::throw_hresult(E_BOUNDS);
      auto child_ancestors = ancestors;
      if (const auto framework = object.try_as<xaml::FrameworkElement>()) {
          InstanceHandle handle = 0;
          winrt::check_hresult(diagnostics_->GetHandleFromIInspectable(
              reinterpret_cast<::IInspectable *>(winrt::get_abi(framework)), &handle));
          const auto type = winrt::get_class_name(framework);
          const auto name = framework.Name();
          ObserveElement(handle, framework, name, type, &ancestors);
          if (!name.empty())
              child_ancestors.insert(child_ancestors.begin(), ToUtf8(name));
      }
      const int count = media::VisualTreeHelper::GetChildrenCount(object);
      for (int child = 0; child < count; ++child)
          SnapshotElement(media::VisualTreeHelper::GetChild(object, child), depth + 1, visited,
                          child_ancestors);
  }
    const winrt::com_ptr<IXamlDiagnostics> diagnostics_;
    std::vector<winrt::weak_ref<xaml::UIElement>> roots_;
};

winrt::com_ptr<VisualTreeWatcher> g_diagnostics_watcher;

struct DiagnosticsSite : winrt::implements<DiagnosticsSite, IObjectWithSite> {
    winrt::com_ptr<IUnknown> site;

    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* value) noexcept override {
        try {
            site.copy_from(value);
            if (!site) return S_OK;
            auto diagnostics = site.as<IXamlDiagnostics>();
            g_visual_tree = site.as<IVisualTreeService>();
            g_diagnostics_watcher = winrt::make_self<VisualTreeWatcher>(std::move(diagnostics));
            g_observer = g_diagnostics_watcher.as<IVisualTreeServiceCallback2>();
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
        if (g_tree_advised) {
            SetDiagnostic(&MwTaskbarConfiguratorLifecycleDiagnosticsBlock[1], 1);
            IncrementDiagnostic(&MwTaskbarConfiguratorLifecycleDiagnosticsBlock[2]);
        }
        return result;
    } catch (...) { return winrt::to_hresult(); }
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

HRESULT ScheduleResumeTreeOnUi(const std::shared_ptr<UiCompletion> &completion) noexcept {
    try {
        winrt::Windows::UI::Core::CoreDispatcher dispatcher{nullptr};
        {
            std::lock_guard lock(g_dispatcher_mutex);
            if (g_dispatcher)
                dispatcher = g_dispatcher.get();
        }
        if (!dispatcher || !g_diagnostics_watcher)
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        const uint64_t generation = g_lifecycle_generation.load(std::memory_order_acquire);
        dispatcher.RunAsync(
            winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [completion, generation] {
                auto guard = g_call_gate.Enter();
                if (!guard || !g_active.load(std::memory_order_acquire) ||
                    generation != g_lifecycle_generation.load(std::memory_order_acquire)) {
                    completion->Complete(HRESULT_FROM_WIN32(ERROR_CANCELLED));
                    return;
                }
                HRESULT result = g_diagnostics_watcher->SnapshotOnUi();
                if (SUCCEEDED(result)) {
                    try {
                        ReadCaptures();
                        for (auto &[handle, entry] : g_entries) {
                            (void)handle;
                            ApplyEntry(entry);
                        }
                    } catch (...) {
                        result = winrt::to_hresult();
                    }
                }
                completion->Complete(result);
            });
        return S_OK;
    } catch (...) {
        return winrt::to_hresult();
    }
}

unsigned __stdcall StartDiagnosticsOperation(void*) noexcept {
    WorkerCounter worker_counter;
    HRESULT result = ConnectDiagnostics();
    if (SUCCEEDED(result)) {
        if (!g_active.load(std::memory_order_acquire))
            result = HRESULT_FROM_WIN32(ERROR_CANCELLED);
        else {
            if (!g_tree_advised)
                result = AdviseTree();
            if (SUCCEEDED(result)) {
                // Initial diagnostics notifications can arrive before XAML has
                // attached descendants to their parents. Read the complete live
                // tree after enumeration, just as on later Start calls.
                result = g_ui_lifecycle_operation
                             .RunOrJoin(&ScheduleResumeTreeOnUi, kUiDispatchTimeoutMs)
                             .result;
            }
        }
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
    taskbar_xaml::ClearCache();
    g_simple_value_cache.clear();g_reported_rules.clear();
    g_live_variables = Json::object();
    g_dynamic_queued = false;
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
        g_ui_lifecycle_operation.WaitForExisting(kUiDispatchTimeoutMs);
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
            const UiCompletionResult restored =
                g_ui_lifecycle_operation.RunOrJoin(&ScheduleRestoreAllOnUi, kUiDispatchTimeoutMs);
            if (!restored.completed || FAILED(restored.result)) {
                SetLastError(restored.result);
                return MW_ERROR_DRAIN_PENDING;
            }
        }
        return MW_ERROR_DRAIN_PENDING;
    }

    if (!g_entries.empty()) {
        const UiCompletionResult result =
            g_ui_lifecycle_operation.RunOrJoin(&ScheduleRestoreAllOnUi, kUiDispatchTimeoutMs);
        if (!result.completed || FAILED(result.result)) {
            SetLastError(result.result);
            return MW_ERROR_DRAIN_PENDING;
        }
    }
    {
        std::lock_guard lock(g_dispatcher_mutex);
        // Keep only the dispatcher for the one resident diagnostics observer.
        // Inactive callbacks return at the closed call gate; no element,
        // property, animation, behavior handler or host pointer remains active.
        if (!g_tree_advised)
            g_dispatcher = {};
    }
    {
        std::lock_guard lock(g_settings_mutex);
        g_apply_queued = false;
    }
    g_taskbar_window = nullptr;
    g_taskbar_thread = 0;
    g_host_api = nullptr;
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
    g_host_api = host;
    ++g_ui_profile_revision;
    g_live_variables = g_ui_config.variables;
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
HRESULT TestReusedObservation(const controls::Grid &grid) noexcept;
extern "C" HRESULT __stdcall MwTaskbarTestRichXaml() noexcept {
    const char* stage="initialization";
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        winrt::Windows::System::DispatcherQueueController queue{nullptr};
        const DispatcherQueueOptions options{sizeof(DispatcherQueueOptions),DQTYPE_THREAD_CURRENT,DQTAT_COM_STA};
        winrt::check_hresult(CreateDispatcherQueueController(options,reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(queue))));
        auto manager=xaml::Hosting::WindowsXamlManager::InitializeForCurrentThread();
        {
        controls::Grid grid;
        stage="style application";
        ElementEntry entry;entry.element=grid;entry.ui_element=grid;entry.type="Windows.UI.Xaml.Controls.Grid";
        controls::ColumnDefinition original;
        original.Width({47, xaml::GridUnitType::Pixel});
        grid.ColumnDefinitions().Append(original);
        std::vector<PropertyState> desired;
        AddDesiredProperty(
            entry, &desired, "Background",
            Json{{"kind", "gradient"},
                 {"stops", Json::array({Json{{"offset", 0}, {"color", "#11223380"}},
                                        Json{{"offset", 1}, {"color", "#FFFFFFFF"}}})}});
        AddDesiredProperty(entry, &desired, "Opacity", 0.8);
        AddDesiredProperty(entry, &desired, "ColumnDefinitions",
                           Json{{"kind", "columns"}, {"sizes", Json::array({"Auto", "2*", "64"})}});
        for (auto &property : desired)
            if (!ApplyProperty(entry, property))
                winrt::throw_hresult(E_FAIL);
        stage = "styled grid values";
        if (grid.ColumnDefinitions().Size() != 3 ||
            grid.ColumnDefinitions().GetAt(1).Width().Value != 2 ||
            !grid.Background().try_as<media::LinearGradientBrush>() ||
            std::abs(grid.Opacity() - 0.8) > 0.00001)
            winrt::throw_hresult(E_FAIL);
        stage = "original grid restoration";
        if (FAILED(RestoreEntry(entry)) || grid.ColumnDefinitions().Size() != 1 ||
            grid.ColumnDefinitions().GetAt(0) != original || grid.Background() ||
            grid.Opacity() != 1 || HasTrackedState(entry))
            winrt::throw_hresult(E_FAIL);
        stage = "attached property";
        const auto column = taskbar_xaml::Resolve(grid, "Grid.Column", 2);
        grid.SetValue(column.property, column.value);
        if (controls::Grid::GetColumn(grid) != 2)
            return E_FAIL;
        stage = "compatible native property owners";
        const Json solid{{"kind", "solid"}, {"color", "#11223380"}};
        const auto grid_background = taskbar_xaml::Resolve(grid, "Background", solid);
        if (grid_background.property != controls::Panel::BackgroundProperty())
            return E_FAIL;
        grid.SetValue(grid_background.property, grid_background.value);
        grid.ClearValue(grid_background.property);
        shapes::Rectangle rectangle;
        const auto rectangle_background = taskbar_xaml::Resolve(
            rectangle, "background", solid, shapes::Shape::FillProperty());
        if (rectangle_background.property != shapes::Shape::FillProperty())
            return E_FAIL;
        rectangle.SetValue(rectangle_background.property, rectangle_background.value);
        rectangle.ClearValue(rectangle_background.property);
        stage = "consumer contour and original restoration";
        ElementEntry outline; outline.element = rectangle; outline.ui_element = rectangle;
        outline.type = "Windows.UI.Xaml.Shapes.Rectangle"; outline.name = "BackgroundFill"; outline.semantic = "surface";
        g_ui_config = Config{};
        g_ui_config.appearance = Json{{"surface",Json{{"normal",Json{{"borderBrush","#FF4488FF"},{"borderThickness",3}}}}}};
        ApplyEntry(outline);
        if (!rectangle.Stroke() || rectangle.StrokeThickness() != 3 || FAILED(RestoreEntry(outline)) || rectangle.Stroke()) return E_FAIL;
        g_ui_config = Config{};
        stage = "appearance fixture XAML parsing";
        auto fixture = xaml::Markup::XamlReader::Load(
            L"<ContentControl xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' "
            L"xmlns:x='http://schemas.microsoft.com/winfx/2006/xaml'>"
            L"<ContentControl.Template>"
            L"<ControlTemplate TargetType='ContentControl'>"
            L"<Border x:Name='BackgroundElement'>"
            L"<VisualStateManager.VisualStateGroups>"
            L"<VisualStateGroup x:Name='CommonStates'>"
            L"<VisualState x:Name='InactiveNormal'/><VisualState x:Name='ActiveNormal'/>"
            L"<VisualState x:Name='ActivePointerOver'/><VisualState x:Name='ActivePressed'/>"
            L"</VisualStateGroup></VisualStateManager.VisualStateGroups>"
            L"</Border></ControlTemplate></ContentControl.Template>"
            L"</ContentControl>").as<controls::ContentControl>();
        stage = "appearance fixture template application";
        fixture.ApplyTemplate();
        fixture.Measure({100, 40});
        fixture.Arrange({0, 0, 100, 40});
        fixture.UpdateLayout();

        std::function<xaml::FrameworkElement(xaml::DependencyObject, const wchar_t*)> find_named;
        find_named = [&](xaml::DependencyObject current, const wchar_t* name) -> xaml::FrameworkElement {
            if (auto element = current.try_as<xaml::FrameworkElement>()) {
                const auto element_name = element.Name();
                if (!element_name.empty() && std::wstring_view(element_name.c_str()) == name)
                    return element;
            }
            for (int32_t index = 0; index < media::VisualTreeHelper::GetChildrenCount(current); ++index) {
                if (auto found = find_named(media::VisualTreeHelper::GetChild(current, index), name))
                    return found;
            }
            return nullptr;
        };

        stage = "appearance fixture named element lookup";
        const auto template_root = find_named(fixture, L"BackgroundElement");
        auto background = template_root.try_as<controls::Border>();
        if (!template_root || !background)
            winrt::throw_hresult(E_FAIL);
        stage = "appearance fixture state group lookup";
        xaml::VisualStateGroup common_states{nullptr};
        for (const auto& group : xaml::VisualStateManager::GetVisualStateGroups(template_root)) {
            if (group.Name() == L"CommonStates") {
                common_states = group;
                break;
            }
        }
        if (!common_states)
            winrt::throw_hresult(E_FAIL);

        stage = "appearance fixture original values";
        const media::SolidColorBrush original_background({255, 1, 2, 3});
        background.Background(original_background);
        background.Opacity(1.0);

        ElementEntry appearance_entry;
        appearance_entry.element = background;
        appearance_entry.ui_element = background;
        appearance_entry.type = "Windows.UI.Xaml.Controls.Border";
        appearance_entry.name = "BackgroundElement";
        appearance_entry.semantic = "button";
        g_ui_config = Config{};
        g_ui_config.appearance = Json{{"button", Json{
            {"normal", Json{{"background", "#112233FF"}, {"opacity", 0.95}}},
            {"active", Json{{"background", "#AA2200FF"}, {"opacity", 0.65}}},
            {"hover", Json{{"background", "#00AA00FF"}}},
            {"pressed", Json{{"background", "#0000AAFF"}}}
        }}};

        auto transition_and_apply = [&](const wchar_t* state_name, bool hover, bool pressed) {
            appearance_entry.pointer_inside = hover;
            appearance_entry.pointer_pressed = pressed;
            if (!xaml::VisualStateManager::GoToState(fixture, state_name, false))
                winrt::throw_hresult(E_FAIL);
            const auto current_state = common_states.CurrentState();
            if (!current_state || current_state.Name() != state_name)
                winrt::throw_hresult(E_FAIL);
            ApplyEntry(appearance_entry, false);
        };
        auto expect_appearance = [&](uint8_t red, uint8_t green, uint8_t blue, double opacity) {
            const auto brush = background.Background().try_as<media::SolidColorBrush>();
            if (!brush) winrt::throw_hresult(E_FAIL);
            const auto color = brush.Color();
            if (color.R != red || color.G != green || color.B != blue || color.A != 255 ||
                std::abs(background.Opacity() - opacity) > 0.00001)
                winrt::throw_hresult(E_FAIL);
        };

        stage = "active appearance state and hover/pressed precedence";
        transition_and_apply(L"InactiveNormal", false, false);
        expect_appearance(0x11, 0x22, 0x33, 0.95);
        transition_and_apply(L"ActiveNormal", false, false);
        expect_appearance(0xAA, 0x22, 0x00, 0.65);
        transition_and_apply(L"ActivePointerOver", true, false);
        expect_appearance(0x00, 0xAA, 0x00, 0.65);
        transition_and_apply(L"ActivePressed", true, true);
        expect_appearance(0x00, 0x00, 0xAA, 0.65);
        transition_and_apply(L"InactiveNormal", false, false);
        expect_appearance(0x11, 0x22, 0x33, 0.95);

        stage = "active appearance original restoration";
        if (FAILED(RestoreEntry(appearance_entry)) || HasTrackedState(appearance_entry) ||
            background.Background() != original_background || background.Opacity() != 1.0)
            winrt::throw_hresult(E_FAIL);

        if (!IsActiveAppearanceState("indicator", L"RunningIndicatorStates", L"ActiveRunningIndicator") ||
            IsActiveAppearanceState("button", L"RunningIndicatorStates", L"ActiveRunningIndicator") ||
            IsActiveAppearanceState("indicator", L"CommonStates", L"ActiveNormal"))
            winrt::throw_hresult(E_FAIL);
        g_ui_config = Config{};
        try {
            (void)taskbar_xaml::Resolve(rectangle, "Foreground", solid);
            return E_FAIL;
        } catch (const winrt::hresult_error &) {
            // Control.Foreground must never be returned for a Rectangle.
        }
        controls::TextBlock label;
        stage = "typography";
        const auto family = taskbar_xaml::Resolve(label, "FontFamily", "Segoe UI");
        label.SetValue(family.property, family.value);
        if (label.FontFamily().Source() != L"Segoe UI")
            return E_FAIL;
        const auto text = taskbar_xaml::Resolve(label, "Text", "#11223380");
        label.SetValue(text.property, text.value);
        if (label.Text() != L"#11223380")
            return E_FAIL;
        stage = "native 3D transforms";
        const auto transform = taskbar_xaml::Resolve(
            grid, "Transform3D",
            Json{{"kind", "transform3d"}, {"rotationY", 12}, {"translateZ", 8}});
        grid.SetValue(transform.property, transform.value);
        if (grid.Transform3D().as<media::Media3D::CompositeTransform3D>().RotationY() != 12)
            return E_FAIL;
        const auto projection = taskbar_xaml::Resolve(
            grid, "Projection", Json{{"kind", "projection"}, {"rotationY", 10}});
        grid.SetValue(projection.property, projection.value);
        if (grid.Projection().as<media::PlaneProjection>().RotationY() != 10)
            return E_FAIL;
        stage = "state and capture subscriptions";
        grid.Name(L"captureGrid");
        entry.handle = 1001;
        entry.name = "captureGrid";
        auto group =
            xaml::Markup::XamlReader::Load(
                L"<VisualStateGroup "
                L"xmlns='http://schemas.microsoft.com/winfx/2006/xaml/presentation' "
                L"xmlns:x='http://schemas.microsoft.com/winfx/2006/xaml' x:Name='CommonStates'/>")
                .as<xaml::VisualStateGroup>();
        xaml::VisualStateManager::GetVisualStateGroups(grid).Append(group);
        g_ui_config = Config{};
        g_ui_config.variables = Json{{"gap", 12}};
        g_ui_config.captures = Json::array({Json{{"name", "taskbarWidth"},
                                                 {"selector", Json{{"name", "captureGrid"}}},
                                                 {"property", "ActualWidth"}}});
        taskbar_config::Rule state_rule;
        state_rule.selector.name = "captureGrid";
        state_rule.state_group = "CommonStates";
        state_rule.state = "PointerOver";
        state_rule.properties = Json{{"Opacity", 0.5}};
        g_ui_config.rules.push_back(state_rule);
        ++g_ui_profile_revision;
        g_entries.emplace(entry.handle, std::move(entry));
        auto &tracked = g_entries.at(1001);
        ConfigureRuleHandlers(tracked);
        if (tracked.state_handlers.size() != 1 || tracked.capture_handlers.size() != 1 ||
            !tracked.theme_handler)
            return E_FAIL;
        grid.Measure({320, 48});
        grid.Arrange({0, 0, 320, 48});
        ReadCaptures();
        if (g_live_variables.at("taskbarWidth") != 320 ||
            ExpandValue(tracked, Json{{"expression", "taskbarWidth - gap"}}) != 308)
            return E_FAIL;
        if (FAILED(RestoreEntry(tracked)) || HasTrackedState(tracked))
            return E_FAIL;
        g_entries.clear();
        g_ui_config = Config{};
        g_live_variables = Json::object();
        stage = "custom blur graph";
        taskbar_xaml::TestBlur(
            xaml::Hosting::ElementCompositionPreview::GetElementVisual(grid).Compositor(),
            Json{{"kind", "blur"},
                 {"color", "#20202080"},
                 {"radius", 8},
                 {"saturation", 1.2},
                 {"noiseOpacity", 0.02}});
        taskbar_xaml::ClearCache();
        g_simple_value_cache.clear();
        stage = "native gradient motion, glass overlay and incremental material updates";
        taskbar_xaml::TestMaterialUpdates(xaml::Hosting::ElementCompositionPreview::GetElementVisual(grid));
        stage = "reused observer snapshots";
        winrt::check_hresult(TestReusedObservation(grid));
        }
        manager.Close();
        if (MwTaskbarConfiguratorEffectDiagnosticsBlock.live_brushes != 0 ||
            MwTaskbarConfiguratorEffectDiagnosticsBlock.connected_brushes != 0 ||
            MwTaskbarConfiguratorEffectDiagnosticsBlock.failures != 0) return E_FAIL;
        return S_OK;
    } catch (...) {
        std::fprintf(stderr, "Rich XAML failure at %s\n", stage);
        return winrt::to_hresult();
    }
}
extern "C" HRESULT __stdcall MwTaskbarTestRotationAndAutoDimension() noexcept {
    try {
        struct Apartment {
            Apartment() {
                winrt::init_apartment(winrt::apartment_type::single_threaded);
            }
            ~Apartment() {
                winrt::clear_factory_cache();
                winrt::uninit_apartment();
            }
        } apartment;
        winrt::clear_factory_cache();
        winrt::Windows::System::DispatcherQueueController queue{nullptr};
        const DispatcherQueueOptions options{sizeof(DispatcherQueueOptions), DQTYPE_THREAD_CURRENT,
                                             DQTAT_COM_STA};
        winrt::check_hresult(CreateDispatcherQueueController(
            options, reinterpret_cast<ABI::Windows::System::IDispatcherQueueController **>(
                         winrt::put_abi(queue))));
        composition::Compositor compositor;
        if (!compositor)
            return E_POINTER;
        ElementEntry entry;
        entry.visual = compositor.CreateContainerVisual();
        if (!entry.visual)
            return E_POINTER;
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
    } catch (...) {
        return winrt::to_hresult();
    }
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
    HRESULT STDMETHODCALLTYPE GetHandleFromIInspectable(::IInspectable *value,
                                                        InstanceHandle *handle) noexcept override {
        if (!value || !handle)
            return E_POINTER;
        winrt::com_ptr<IUnknown> identity;
        const HRESULT result = value->QueryInterface(IID_IUnknown, identity.put_void());
        if (SUCCEEDED(result))
            *handle = reinterpret_cast<InstanceHandle>(identity.get());
        return result;
    }
    HRESULT STDMETHODCALLTYPE HitTest(RECT, unsigned int*, InstanceHandle**) noexcept override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE RegisterInstance(::IInspectable*, InstanceHandle*) noexcept override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetInitializationData(BSTR*) noexcept override { return E_NOTIMPL; }
    foundation::IInspectable object;
};

HRESULT TestReusedObservation(const controls::Grid &grid) noexcept {
    try {
        auto cache = winrt::make_self<DiagnosticCacheFixture>(7).as<IXamlDiagnostics>();
        auto watcher = winrt::make_self<VisualTreeWatcher>(cache);
        grid.Name(L"TaskbarFrame");
        g_ui_config = Config{};
        g_ui_config.enabled = false;
        controls::Button original;
        xaml::Automation::AutomationProperties::SetAutomationId(original, L"StartButton");
        grid.Children().Append(original);
        watcher->RememberRoot(grid);
        watcher->RememberRoot(grid);
        for (unsigned cycle = 0; cycle < 6; ++cycle) {
            grid.Measure({320, 48});
            grid.Arrange({0, 0, 320, 48});
            grid.UpdateLayout();
            InstanceHandle child_handle = 0;
            winrt::check_hresult(cache->GetHandleFromIInspectable(
                reinterpret_cast<::IInspectable *>(winrt::get_abi(grid.Children().GetAt(0))),
                &child_handle));
            winrt::check_hresult(watcher->SnapshotOnUi());
            if (g_entries.size() < 2 || !g_entries.contains(child_handle) ||
                g_entries.at(child_handle).semantic != "startButton") {
                std::fprintf(
                    stderr,
                    "snapshot cycle %u: %zu entries, %d visual children, tracked child %d\n", cycle,
                    g_entries.size(), media::VisualTreeHelper::GetChildrenCount(grid),
                    g_entries.contains(child_handle));
                return E_FAIL;
            }
            winrt::check_hresult(RestoreAllOnUi());
            if (!g_entries.empty())
                return E_FAIL;
            // The same weak root must discover replacement children after stop.
            grid.Children().Clear();
            controls::Button replacement;
            xaml::Automation::AutomationProperties::SetAutomationId(replacement, L"StartButton");
            grid.Children().Append(replacement);
        }
        grid.Children().Clear();
        grid.Name(L"captureGrid");
        g_ui_config = Config{};
        {
            std::lock_guard lock(g_dispatcher_mutex);
            g_dispatcher = {};
        }
        return S_OK;
    } catch (...) {
        return winrt::to_hresult();
    }
}

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

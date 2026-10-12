#pragma once
#include "taskbar_config.hpp"
#include <unknwn.h>
#undef GetCurrentTime
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Composition.h>
namespace taskbar_xaml {
// Add-on-local inspection counters; the native engine ABI is unchanged.
struct alignas(8) EffectDiagnostics {
    volatile int64_t version;
    volatile int64_t live_brushes;
    volatile int64_t connected_brushes;
    volatile int64_t failures;
    volatile int64_t last_error;
};
struct Value {
    winrt::Windows::UI::Xaml::DependencyProperty property{nullptr};
    winrt::Windows::Foundation::IInspectable value{nullptr};
};
Value Resolve(const winrt::Windows::UI::Xaml::FrameworkElement &element, std::string_view name,
              const nlohmann::json &value,
              const winrt::Windows::UI::Xaml::DependencyProperty &known_property = nullptr);
void ClearCache();
#ifdef MW_TASKBAR_CALLBACK_TESTING
void TestBlur(winrt::Windows::UI::Composition::Compositor const &compositor,
              nlohmann::json const &settings);
#endif
} // namespace taskbar_xaml
extern "C" __declspec(dllexport) taskbar_xaml::EffectDiagnostics
    MwTaskbarConfiguratorEffectDiagnosticsBlock;

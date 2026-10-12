#include "xaml_values.hpp"
#include <Windows.h>
#include <shcore.h>
#include <windows.graphics.effects.interop.h>
#include <initguid.h>
#include <d2d1effects.h>
#include <d2d1effects_2.h>
#include <d2d1_1.h>
#include <winrt/Windows.Graphics.Effects.h>
#include <winrt/Windows.UI.Xaml.Markup.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/Windows.System.Power.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <algorithm>
#include <map>

extern "C" {
__declspec(dllexport) taskbar_xaml::EffectDiagnostics MwTaskbarConfiguratorEffectDiagnosticsBlock{
    1, 0, 0, 0, 0};
}

namespace taskbar_xaml {
namespace xaml = winrt::Windows::UI::Xaml;
namespace media = xaml::Media;
namespace composition = winrt::Windows::UI::Composition;
namespace effects = winrt::Windows::Graphics::Effects;
namespace foundation = winrt::Windows::Foundation;
namespace interop = ABI::Windows::Graphics::Effects;
using Json = nlohmann::json;
namespace {
thread_local std::map<std::wstring, Value> cache;
// A small Direct2D effect node; the graph is built once when a brush connects.
struct Effect : winrt::implements<Effect, effects::IGraphicsEffect, effects::IGraphicsEffectSource,
                                  interop::IGraphicsEffectD2D1Interop> {
    GUID id;
    winrt::hstring name;
    std::vector<foundation::IInspectable> properties;
    std::vector<effects::IGraphicsEffectSource> sources;
    Effect(GUID effect, std::vector<foundation::IInspectable> props,
           std::vector<effects::IGraphicsEffectSource> inputs)
        : id(effect), properties(std::move(props)), sources(std::move(inputs)) {
    }
    winrt::hstring Name() const {
        return name;
    }
    void Name(winrt::hstring const &value) {
        name = value;
    }
    HRESULT __stdcall GetEffectId(GUID *value) noexcept override {
        if (!value)
            return E_POINTER;
        *value = id;
        return S_OK;
    }
    HRESULT __stdcall GetNamedPropertyMapping(
        LPCWSTR, UINT *, interop::GRAPHICS_EFFECT_PROPERTY_MAPPING *) noexcept override {
        return E_INVALIDARG;
    }
    HRESULT __stdcall GetPropertyCount(UINT *value) noexcept override {
        if (!value)
            return E_POINTER;
        *value = static_cast<UINT>(properties.size());
        return S_OK;
    }
    HRESULT __stdcall GetProperty(
        UINT index, ABI::Windows::Foundation::IPropertyValue **value) noexcept override {
        if (!value)
            return E_POINTER;
        *value = nullptr;
        if (index >= properties.size())
            return E_INVALIDARG;
        return winrt::get_unknown(properties[index])
            ->QueryInterface(__uuidof(ABI::Windows::Foundation::IPropertyValue),
                             reinterpret_cast<void **>(value));
    }
    HRESULT __stdcall GetSource(UINT index,
                                interop::IGraphicsEffectSource **value) noexcept override {
        if (!value)
            return E_POINTER;
        *value = nullptr;
        if (index >= sources.size())
            return E_INVALIDARG;
        return winrt::get_unknown(sources[index])
            ->QueryInterface(__uuidof(interop::IGraphicsEffectSource),
                             reinterpret_cast<void **>(value));
    }
    HRESULT __stdcall GetSourceCount(UINT *value) noexcept override {
        if (!value)
            return E_POINTER;
        *value = static_cast<UINT>(sources.size());
        return S_OK;
    }
};
effects::IGraphicsEffectSource Node(GUID id, std::vector<foundation::IInspectable> props,
                                    std::vector<effects::IGraphicsEffectSource> sources) {
    return winrt::make<Effect>(id, std::move(props), std::move(sources));
}
winrt::Windows::UI::Color Color(std::string text) {
    if (text == "transparent")
        return {0, 0, 0, 0};
    if (text.size() != 7 && text.size() != 9)
        winrt::throw_hresult(E_INVALIDARG);
    const auto byte = [&](std::size_t pos) {
        return static_cast<uint8_t>(std::stoul(text.substr(pos, 2), nullptr, 16));
    };
    return {text.size() == 9 ? byte(7) : static_cast<uint8_t>(255), byte(1), byte(3), byte(5)};
}
struct BlurBrush : media::XamlCompositionBrushBaseT<BlurBrush> {
    composition::Compositor compositor{nullptr};
    Json settings;
    media::LoadedImageSurface noise{nullptr};
    bool connected = false;
    bool HasCompositionBrush() {
        return static_cast<bool>(CompositionBrush());
    }
    BlurBrush(composition::Compositor value, Json parameters)
        : compositor(value), settings(std::move(parameters)) {
        FallbackColor(Color(settings.value("fallbackColor", "#202020FF")));
        InterlockedIncrement64(&MwTaskbarConfiguratorEffectDiagnosticsBlock.live_brushes);
    }
    ~BlurBrush() {
        OnDisconnected();
        InterlockedDecrement64(&MwTaskbarConfiguratorEffectDiagnosticsBlock.live_brushes);
    }
    void OnConnected() noexcept {
        try {
            if (CompositionBrush())
                return;
            if (!winrt::Windows::UI::ViewManagement::UISettings().AdvancedEffectsEnabled() ||
                winrt::Windows::System::Power::PowerManager::EnergySaverStatus() ==
                    winrt::Windows::System::Power::EnergySaverStatus::On)
                return;
            auto source = composition::CompositionEffectSourceParameter(L"backdrop");
            auto graph = Node(
                CLSID_D2D1GaussianBlur,
                {foundation::PropertyValue::CreateSingle(
                     static_cast<float>(settings.value("radius", 18.0))),
                 foundation::PropertyValue::CreateUInt32(D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED),
                 foundation::PropertyValue::CreateUInt32(D2D1_BORDER_MODE_HARD)},
                {source});
            const float saturation = static_cast<float>(settings.value("saturation", 1.0));
            const float brightness = static_cast<float>(settings.value("luminosity", 1.0));
            const float r = (1 - saturation) * 0.2126f, g = (1 - saturation) * 0.7152f,
                        b = (1 - saturation) * 0.0722f;
            const float matrix[] = {(r + saturation) * brightness,
                                    r * brightness,
                                    r * brightness,
                                    0,
                                    g * brightness,
                                    (g + saturation) * brightness,
                                    g * brightness,
                                    0,
                                    b * brightness,
                                    b * brightness,
                                    (b + saturation) * brightness,
                                    0,
                                    0,
                                    0,
                                    0,
                                    1,
                                    0,
                                    0,
                                    0,
                                    0};
            graph = Node(
                CLSID_D2D1ColorMatrix,
                {foundation::PropertyValue::CreateSingleArray(matrix),
                 foundation::PropertyValue::CreateUInt32(D2D1_COLORMATRIX_ALPHA_MODE_PREMULTIPLIED),
                 foundation::PropertyValue::CreateBoolean(true)},
                {graph});
            const auto tint = Color(settings.value("color", "#20202080"));
            const float opacity = static_cast<float>(settings.value("tintOpacity", tint.A / 255.0));
            const float rgba[] = {tint.R / 255.0f, tint.G / 255.0f, tint.B / 255.0f, opacity};
            auto flood =
                Node(CLSID_D2D1Flood, {foundation::PropertyValue::CreateSingleArray(rgba)}, {});
            graph = Node(CLSID_D2D1Composite,
                         {foundation::PropertyValue::CreateUInt32(D2D1_COMPOSITE_MODE_SOURCE_OVER)},
                         {graph, flood});
            const float grain = static_cast<float>(settings.value("noiseOpacity", 0.0));
            if (grain > 0) {
                auto repeated =
                    Node(CLSID_D2D1Border,
                         {foundation::PropertyValue::CreateUInt32(D2D1_BORDER_EDGE_MODE_WRAP),
                          foundation::PropertyValue::CreateUInt32(D2D1_BORDER_EDGE_MODE_WRAP)},
                         {composition::CompositionEffectSourceParameter(L"noise")});
                const float alpha[] = {1, 0, 0, 0, 0, 1,     0, 0, 0, 0,
                                       1, 0, 0, 0, 0, grain, 0, 0, 0, 0};
                repeated = Node(
                    CLSID_D2D1ColorMatrix,
                    {foundation::PropertyValue::CreateSingleArray(alpha),
                     foundation::PropertyValue::CreateUInt32(D2D1_COLORMATRIX_ALPHA_MODE_STRAIGHT),
                     foundation::PropertyValue::CreateBoolean(true)},
                    {repeated});
                graph =
                    Node(CLSID_D2D1Composite,
                         {foundation::PropertyValue::CreateUInt32(D2D1_COMPOSITE_MODE_SOURCE_OVER)},
                         {graph, repeated});
            }
            auto factory = compositor.CreateEffectFactory(graph.as<effects::IGraphicsEffect>());
            auto brush = factory.CreateBrush();
            brush.SetSourceParameter(L"backdrop", compositor.CreateBackdropBrush());
            if (grain > 0) {
                // A deterministic 64x64 bitmap: generated once per connected brush,
                // decoded by Windows, then tiled by Composition. No per-frame work.
                std::vector<uint8_t> bitmap(54 + 64 * 64 * 4);
                auto u32 = [&](size_t offset, uint32_t value) {
                    for (size_t i = 0; i < 4; ++i)
                        bitmap[offset + i] = static_cast<uint8_t>(value >> (8 * i));
                };
                bitmap[0] = 'B';
                bitmap[1] = 'M';
                u32(2, static_cast<uint32_t>(bitmap.size()));
                u32(10, 54);
                u32(14, 40);
                u32(18, 64);
                u32(22, 64);
                bitmap[26] = 1;
                bitmap[28] = 32;
                uint32_t seed = 0x4d575442;
                for (size_t i = 54; i < bitmap.size(); i += 4) {
                    seed ^= seed << 13;
                    seed ^= seed >> 17;
                    seed ^= seed << 5;
                    const auto shade = static_cast<uint8_t>(seed);
                    bitmap[i] = bitmap[i + 1] = bitmap[i + 2] = shade;
                    bitmap[i + 3] = 255;
                }
                winrt::com_ptr<IStream> memory;
                winrt::check_hresult(CreateStreamOnHGlobal(nullptr, TRUE, memory.put()));
                ULONG written = 0;
                winrt::check_hresult(
                    memory->Write(bitmap.data(), static_cast<ULONG>(bitmap.size()), &written));
                if (written != bitmap.size())
                    winrt::throw_hresult(E_FAIL);
                LARGE_INTEGER beginning{};
                winrt::check_hresult(memory->Seek(beginning, STREAM_SEEK_SET, nullptr));
                winrt::Windows::Storage::Streams::IRandomAccessStream stream{nullptr};
                winrt::check_hresult(CreateRandomAccessStreamOverStream(
                    memory.get(), BSOS_DEFAULT,
                    winrt::guid_of<winrt::Windows::Storage::Streams::IRandomAccessStream>(),
                    winrt::put_abi(stream)));
                noise = media::LoadedImageSurface::StartLoadFromStream(stream);
                auto surface = compositor.CreateSurfaceBrush(noise);
                surface.Stretch(composition::CompositionStretch::None);
                brush.SetSourceParameter(L"noise", surface);
            }
            CompositionBrush(brush);
            connected = true;
            InterlockedIncrement64(&MwTaskbarConfiguratorEffectDiagnosticsBlock.connected_brushes);
        } catch (...) {
            // A graphics-driver or effect failure must leave a solid fallback,
            // never an exception crossing back into Explorer's XAML callback.
            const HRESULT error = winrt::to_hresult();
            InterlockedIncrement64(&MwTaskbarConfiguratorEffectDiagnosticsBlock.failures);
            InterlockedExchange64(&MwTaskbarConfiguratorEffectDiagnosticsBlock.last_error, error);
            OnDisconnected();
            OutputDebugStringA(
                "Taskbar Configurator: backdrop effect unavailable; using fallback color.\n");
        }
    }
    void OnDisconnected() noexcept {
        if (connected) {
            connected = false;
            InterlockedDecrement64(&MwTaskbarConfiguratorEffectDiagnosticsBlock.connected_brushes);
        }
        try {
            if (auto brush = CompositionBrush()) {
                CompositionBrush(nullptr);
                brush.Close();
            }
        } catch (...) {
        }
        try {
            if (noise) {
                noise.Close();
                noise = nullptr;
            }
        } catch (...) {
            noise = nullptr;
        }
    }
};
std::wstring Document(std::string_view type, const std::string &property, const Json &value) {
    const auto dot = type.rfind('.');
    const auto tag =
        dot == type.npos ? std::string(type) : "target:" + std::string(type.substr(dot + 1));
    std::string xml =
        "<ResourceDictionary xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\" "
        "xmlns:x=\"http://schemas.microsoft.com/winfx/2006/xaml\"";
    if (dot != type.npos)
        xml += " xmlns:target=\"using:" + taskbar_config::XmlEscape(type.substr(0, dot)) + "\"";
    xml += "><Style TargetType=\"" + tag + "\"><Setter Property=\"" +
           taskbar_config::XmlEscape(property) + "\"";
    const bool color = property == "Background" || property == "Foreground" || property == "Fill" ||
                       property == "Stroke" || property.ends_with("Brush") ||
                       property.ends_with("Color");
    const auto text = taskbar_config::XamlValueMarkup(value, color);
    if (!text.empty() && text.front() == '<')
        xml += "><Setter.Value>" + text + "</Setter.Value></Setter>";
    else
        xml += " Value=\"" + taskbar_config::XmlEscape(text) + "\"/>";
    xml += "</Style></ResourceDictionary>";
    return std::wstring(winrt::to_hstring(xml));
}
} // namespace
void ClearCache() {
    cache.clear();
}
#ifdef MW_TASKBAR_CALLBACK_TESTING
void TestBlur(composition::Compositor const &compositor, Json const &settings) {
    auto brush = winrt::make_self<BlurBrush>(compositor, settings);
    brush->OnConnected();
    if (!brush->HasCompositionBrush() &&
        winrt::Windows::UI::ViewManagement::UISettings().AdvancedEffectsEnabled() &&
        winrt::Windows::System::Power::PowerManager::EnergySaverStatus() !=
            winrt::Windows::System::Power::EnergySaverStatus::On)
        winrt::throw_hresult(E_FAIL);
    brush->OnDisconnected();
    if (brush->HasCompositionBrush())
        winrt::throw_hresult(E_FAIL);
}
#endif
Value Resolve(const xaml::FrameworkElement &element, std::string_view name, const Json &value) {
    const auto type = winrt::to_string(winrt::get_class_name(element));
    const auto property = taskbar_config::XamlPropertyName(name);
    const bool custom = value.is_object() && value.value("kind", "") == "blur";
    const auto compositor =
        custom ? xaml::Hosting::ElementCompositionPreview::GetElementVisual(element).Compositor()
               : composition::Compositor(nullptr);
    const auto key = std::wstring(winrt::to_hstring(
        type + ":" + property + ":" + value.dump() +
        (custom ? ":" + std::to_string(reinterpret_cast<uintptr_t>(winrt::get_abi(compositor)))
                : "")));
    if (auto found = cache.find(key); found != cache.end())
        return found->second;
    Value result;
    std::vector<std::string> candidates{type, "Windows.UI.Xaml.Controls.Control",
                                        "Windows.UI.Xaml.FrameworkElement"};
    HRESULT last = E_INVALIDARG;
    for (const auto &candidate : candidates) {
        try {
            auto dictionary = xaml::Markup::XamlReader::Load(
                                  Document(candidate, property, custom ? Json(nullptr) : value))
                                  .as<xaml::ResourceDictionary>();
            auto style = dictionary.First().Current().Value().as<xaml::Style>();
            auto setter = style.Setters().GetAt(0).as<xaml::Setter>();
            result = {setter.Property(), setter.Value()};
            if (value.is_null())
                result.value = xaml::DependencyProperty::UnsetValue();
            if (custom)
                result.value = winrt::make<BlurBrush>(compositor, value);
            if (cache.size() >= 1024)
                cache.clear();
            cache.emplace(key, result);
            return result;
        } catch (...) {
            last = winrt::to_hresult();
        }
    }
    winrt::throw_hresult(last);
}
} // namespace taskbar_xaml

#include "xaml_values.hpp"
#include <Windows.h>
#include <shcore.h>
#include <windows.graphics.effects.interop.h>
#include <initguid.h>
#include <d2d1effects.h>
#include <d2d1effects_2.h>
#include <d2d1_1.h>
#include <winrt/Windows.Graphics.Effects.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>
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
#include <cmath>

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
        LPCWSTR property, UINT *index, interop::GRAPHICS_EFFECT_PROPERTY_MAPPING *mapping) noexcept override {
        if (!property || !index || !mapping) return E_POINTER;
        if (wcscmp(property, L"BlurAmount") == 0) { *index = 0; *mapping = interop::GRAPHICS_EFFECT_PROPERTY_MAPPING_DIRECT; return S_OK; }
        if (wcscmp(property, L"Color") == 0) { *index = 0; *mapping = interop::GRAPHICS_EFFECT_PROPERTY_MAPPING_COLOR_TO_VECTOR4; return S_OK; }
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
struct GradientBrush : media::XamlCompositionBrushBaseT<GradientBrush> {
    composition::Compositor compositor{nullptr};
    composition::Visual host{nullptr};
    Json settings;
    composition::CompositionGradientBrush gradient{nullptr};
    bool connected = false;
    composition::CompositionBrush NativeBrush() { return CompositionBrush(); }
    GradientBrush(composition::Visual visual, Json parameters)
        : compositor(visual.Compositor()), host(visual), settings(std::move(parameters)) {
        FallbackColor(Color(settings.at("stops").at(0).at("color").get<std::string>()));
        InterlockedIncrement64(&MwTaskbarConfiguratorEffectDiagnosticsBlock.live_brushes);
    }
    ~GradientBrush() { OnDisconnected(); InterlockedDecrement64(&MwTaskbarConfiguratorEffectDiagnosticsBlock.live_brushes); }
    void Expression(const wchar_t* property, const winrt::hstring& expression) {
        try {
            auto animation = compositor.CreateExpressionAnimation(expression);
            animation.SetReferenceParameter(L"host", host);
            gradient.StartAnimation(property, animation);
        } catch (...) {
#ifdef MW_TASKBAR_CALLBACK_TESTING
            std::fwprintf(stderr, L"Gradient expression %s: %s\n", property, expression.c_str());
#endif
            throw;
        }
    }
    void Update(Json parameters) {
        const char* update_stage = "parameters";
        try {
        settings = std::move(parameters);
        if (!gradient) return;
        update_stage = "stops";
        auto stops = settings.at("stops").get<std::vector<Json>>();
        std::stable_sort(stops.begin(), stops.end(), [](const auto& a, const auto& b) { return a.at("offset").template get<double>() < b.at("offset").template get<double>(); });
        while (gradient.ColorStops().Size() > stops.size()) {
            auto stop = gradient.ColorStops().GetAt(gradient.ColorStops().Size() - 1); stop.StopAnimation(L"Color"); gradient.ColorStops().RemoveAtEnd(); stop.Close();
        }
        for (uint32_t i = 0; i < stops.size(); ++i) {
            const auto& stop = stops[i];
            if (i >= gradient.ColorStops().Size()) gradient.ColorStops().Append(compositor.CreateColorGradientStop());
            auto native = gradient.ColorStops().GetAt(i);
            native.Offset(static_cast<float>(stop.at("offset").get<double>()));
            native.Color(Color(stop.at("color").get<std::string>()));
        }
        update_stage = "extend and offset";
        const auto extend = settings.value("extend", "clamp");
        gradient.ExtendMode(extend == "wrap" ? composition::CompositionGradientExtendMode::Wrap : extend == "mirror" ? composition::CompositionGradientExtendMode::Mirror : composition::CompositionGradientExtendMode::Clamp);
        gradient.StopAnimation(L"Offset");
        gradient.Offset({0, 0});
        if (auto linear = gradient.try_as<composition::CompositionLinearGradientBrush>()) {
            update_stage = "linear geometry";
            if (settings.contains("angle")) {
                // Absolute coordinates preserve the physical angle on a long,
                // thin taskbar. Expressions follow size changes in Composition.
                gradient.MappingMode(composition::CompositionMappingMode::Absolute);
                const auto radians = settings.value("angle", 0.0) * 3.14159265358979323846 / 180.0;
                const float c = static_cast<float>(std::cos(radians)), s = static_cast<float>(std::sin(radians));
                const auto length = winrt::to_hstring("(" + std::to_string(std::abs(c)) + " * host.Size.X + " + std::to_string(std::abs(s)) + " * host.Size.Y) / " + std::to_string(settings.value("repetitions", 1.0)));
                gradient.RotationAngleInDegrees(static_cast<float>(settings.value("angle", 0.0)));
                Expression(L"CenterPoint", L"host.Size * 0.5");
                Expression(L"StartPoint", L"host.Size * 0.5 - Vector2(" + length + L" * 0.5, 0)");
                Expression(L"EndPoint", L"host.Size * 0.5 + Vector2(" + length + L" * 0.5, 0)");
            } else {
                gradient.StopAnimation(L"StartPoint"); gradient.StopAnimation(L"EndPoint"); gradient.StopAnimation(L"CenterPoint");
                gradient.MappingMode(composition::CompositionMappingMode::Relative);
                const auto start = settings.value("start", Json::array({0,0})), end = settings.value("end", Json::array({1,1}));
                linear.StartPoint({start[0].get<float>(), start[1].get<float>()}); linear.EndPoint({end[0].get<float>(), end[1].get<float>()});
            }
        } else if (auto radial = gradient.try_as<composition::CompositionRadialGradientBrush>()) {
            gradient.MappingMode(composition::CompositionMappingMode::Relative);
            radial.EllipseCenter({static_cast<float>(settings.value("centerX", .5)), static_cast<float>(settings.value("centerY", .5))});
            const auto repeats = static_cast<float>(settings.value("repetitions", 1.0));
            radial.EllipseRadius({static_cast<float>(settings.value("radiusX", .5))/repeats, static_cast<float>(settings.value("radiusY", .5))/repeats});
        }
        update_stage = "motion";
        const auto motion = settings.value("motion", "none");
        if (motion != "none" && winrt::Windows::UI::ViewManagement::UISettings().AnimationsEnabled() &&
            winrt::Windows::System::Power::PowerManager::EnergySaverStatus() != winrt::Windows::System::Power::EnergySaverStatus::On) {
            auto animation = compositor.CreateVector2KeyFrameAnimation();
            const auto angle = settings.value("angle", 0.0) * 3.14159265358979323846 / 180.0;
            // End expressions use live bounds. No timer, controller RPC or
            // C++ callback executes on animation frames.
            const auto vector = gradient.MappingMode() == composition::CompositionMappingMode::Absolute
                ? winrt::to_hstring("Vector2(" + std::to_string(std::cos(angle) / settings.value("repetitions",1.0)) + "," + std::to_string(std::sin(angle) / settings.value("repetitions",1.0)) + ") * (" + std::to_string(std::abs(std::cos(angle))) + " * host.Size.X + " + std::to_string(std::abs(std::sin(angle))) + " * host.Size.Y)")
                : winrt::hstring(L"Vector2(1,0)");
            update_stage = "motion reference";
            animation.SetReferenceParameter(L"host", host);
            auto easing = compositor.CreateLinearEasingFunction();
            update_stage = "motion initial key";
            animation.InsertKeyFrame(0, {0,0}, easing);
            update_stage = "motion expression key";
            animation.InsertExpressionKeyFrame(motion == "pingpong" ? .5f : 1.f, vector, easing);
            if (motion == "pingpong") animation.InsertKeyFrame(1, {0,0}, easing);
            update_stage = "motion duration";
            animation.Duration(std::chrono::duration_cast<foundation::TimeSpan>(std::chrono::duration<double>(settings.value("duration",12.0))));
            animation.IterationBehavior(composition::AnimationIterationBehavior::Forever);
            update_stage = "motion start";
            gradient.StartAnimation(L"Offset", animation);
        }
        } catch (...) {
#ifdef MW_TASKBAR_CALLBACK_TESTING
            std::fprintf(stderr, "Gradient update failure at %s\n", update_stage);
#else
            (void)update_stage;
#endif
            throw;
        }
    }
    void OnConnected() noexcept {
        try {
            if (gradient) return;
            gradient = settings.value("shape", "linear") == "radial" ? compositor.CreateRadialGradientBrush().as<composition::CompositionGradientBrush>() : compositor.CreateLinearGradientBrush().as<composition::CompositionGradientBrush>();
            Update(settings); CompositionBrush(gradient); connected = true;
            InterlockedIncrement64(&MwTaskbarConfiguratorEffectDiagnosticsBlock.connected_brushes);
        } catch (...) {
            InterlockedIncrement64(&MwTaskbarConfiguratorEffectDiagnosticsBlock.failures);
            InterlockedExchange64(&MwTaskbarConfiguratorEffectDiagnosticsBlock.last_error, winrt::to_hresult()); OnDisconnected();
        }
    }
    void OnDisconnected() noexcept {
        if (connected) { connected = false; InterlockedDecrement64(&MwTaskbarConfiguratorEffectDiagnosticsBlock.connected_brushes); }
        try {
            CompositionBrush(nullptr);
            if (gradient) { for (auto stop : gradient.ColorStops()) stop.Close(); gradient.Close(); gradient = nullptr; }
        } catch (...) { gradient = nullptr; }
    }
};
struct BlurBrush : media::XamlCompositionBrushBaseT<BlurBrush> {
    composition::Compositor compositor{nullptr};
    Json settings;
    composition::Visual host{nullptr};
    winrt::com_ptr<GradientBrush> overlay;
    media::LoadedImageSurface noise{nullptr};
    bool connected = false;
    bool HasCompositionBrush() {
        return static_cast<bool>(CompositionBrush());
    }
    BlurBrush(composition::Compositor value, Json parameters, composition::Visual visual = nullptr)
        : compositor(value), settings(std::move(parameters)), host(visual) {
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
            graph.as<effects::IGraphicsEffect>().Name(L"Blur");
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
            flood.as<effects::IGraphicsEffect>().Name(L"Tint");
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
            if (settings.contains("overlay") && host)
                graph = Node(CLSID_D2D1Composite, {foundation::PropertyValue::CreateUInt32(D2D1_COMPOSITE_MODE_SOURCE_OVER)}, {graph, composition::CompositionEffectSourceParameter(L"overlay")});
            auto factory = compositor.CreateEffectFactory(graph.as<effects::IGraphicsEffect>(), {L"Blur.BlurAmount", L"Tint.Color"});
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
            if (settings.contains("overlay") && host) {
                overlay = winrt::make_self<GradientBrush>(host, settings.at("overlay")); overlay->OnConnected();
                if (!overlay->NativeBrush()) winrt::throw_hresult(E_FAIL);
                brush.SetSourceParameter(L"overlay", overlay->NativeBrush());
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
        if (overlay) { overlay->OnDisconnected(); overlay = nullptr; }
        try {
            if (noise) {
                noise.Close();
                noise = nullptr;
            }
        } catch (...) {
            noise = nullptr;
        }
    }
    void Update(Json parameters) {
        settings = std::move(parameters);
        FallbackColor(Color(settings.value("fallbackColor", "#202020FF")));
        if (auto brush = CompositionBrush().try_as<composition::CompositionEffectBrush>()) {
            brush.Properties().InsertScalar(L"Blur.BlurAmount", static_cast<float>(settings.value("radius",18.0)));
            auto tint = Color(settings.value("color", "#20202080")); tint.A = static_cast<uint8_t>(std::round(settings.value("tintOpacity", tint.A/255.0) * 255));
            brush.Properties().InsertColor(L"Tint.Color", tint);
            if (overlay) overlay->Update(settings.at("overlay"));
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
bool TryUpdateBrush(const foundation::IInspectable& value, const Json& previous, const Json& next) {
    if (!previous.is_object() || !next.is_object() || previous.value("kind", "") != next.value("kind", "")) return false;
    auto brush = value.try_as<media::XamlCompositionBrushBase>(); if (!brush) return false;
    if (next.value("kind", "") == "gradient" && previous.value("shape", "linear") == next.value("shape", "linear")) {
        winrt::get_self<GradientBrush>(brush.as<media::IXamlCompositionBrushBaseOverrides>())->Update(next); return true;
    }
    if (next.value("kind", "") == "blur") {
        auto a = previous, b = next;
        for (const auto key : {"color", "radius", "tintOpacity", "fallbackColor"}) { a.erase(key); b.erase(key); }
        if (a.contains("overlay") && b.contains("overlay") && a["overlay"].value("shape", "linear") == b["overlay"].value("shape", "linear")) { a.erase("overlay"); b.erase("overlay"); }
        if (a == b) { winrt::get_self<BlurBrush>(brush.as<media::IXamlCompositionBrushBaseOverrides>())->Update(next); return true; }
    }
    return false;
}
#ifdef MW_TASKBAR_CALLBACK_TESTING
void TestMaterialUpdates(composition::Visual const& host) {
    const auto initial_live = MwTaskbarConfiguratorEffectDiagnosticsBlock.live_brushes;
    const auto initial_connected = MwTaskbarConfiguratorEffectDiagnosticsBlock.connected_brushes;
    const auto initial_failures = MwTaskbarConfiguratorEffectDiagnosticsBlock.failures;
    Json settings{{"kind", "gradient"}, {"shape", "linear"}, {"angle", 45}, {"repetitions", 3}, {"extend", "wrap"}, {"motion", "scroll"}, {"duration", 3},
        {"stops", Json::array({Json{{"offset",1},{"color","#FF4499FF"}}, Json{{"offset",0},{"color","#11CCEEFF"}}})}};
    {
        auto gradient = winrt::make_self<GradientBrush>(host, settings); gradient->OnConnected();
        const auto native = gradient->NativeBrush(); if (!native) winrt::throw_hresult(static_cast<HRESULT>(MwTaskbarConfiguratorEffectDiagnosticsBlock.last_error));
        auto previous = settings;
        for (int i = 0; i < 200; ++i) {
            auto next = previous; next["angle"] = i % 180; next["stops"][0]["offset"] = (i%100) / 100.0;
            if (!TryUpdateBrush(gradient.as<foundation::IInspectable>(), previous, next) || gradient->NativeBrush() != native) winrt::throw_hresult(E_FAIL);
            previous = std::move(next);
        }
        gradient->OnDisconnected(); if (gradient->NativeBrush()) winrt::throw_hresult(E_FAIL);
    }
    settings["shape"] = "radial"; settings["motion"] = "pingpong";
    {
        auto radial = winrt::make_self<GradientBrush>(host, settings); radial->OnConnected(); if (!radial->NativeBrush()) winrt::throw_hresult(E_FAIL); radial->OnDisconnected();
    }
    {
        Json material{{"kind","blur"},{"color","#202020FF"},{"radius",18},{"tintOpacity",.2},{"noiseOpacity",.02},{"overlay",settings}};
        auto blur = winrt::make_self<BlurBrush>(host.Compositor(), material, host); blur->OnConnected();
        for (int i=0; i<100; ++i) {
            auto next=material; next["radius"]=i; next["tintOpacity"]=(i%100)/100.0; next["overlay"]["stops"][0]["color"]="#55FF99AA";
            if (!TryUpdateBrush(blur.as<foundation::IInspectable>(),material,next)) winrt::throw_hresult(E_FAIL); material=std::move(next);
        }
        blur->OnDisconnected(); if (blur->HasCompositionBrush()) winrt::throw_hresult(E_FAIL);
    }
    if (initial_live != MwTaskbarConfiguratorEffectDiagnosticsBlock.live_brushes || initial_connected != MwTaskbarConfiguratorEffectDiagnosticsBlock.connected_brushes || initial_failures != MwTaskbarConfiguratorEffectDiagnosticsBlock.failures) winrt::throw_hresult(E_FAIL);
}
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
Value Resolve(const xaml::FrameworkElement &element, std::string_view name, const Json &value,
              const xaml::DependencyProperty &known_property) {
    const auto type = winrt::to_string(winrt::get_class_name(element));
    auto property = taskbar_config::XamlPropertyName(name);
    // A semantic Rectangle background is Fill, not Control.Background.
    if (known_property) {
        if (known_property == xaml::Shapes::Shape::FillProperty())
            property = "Fill";
        else if (known_property == xaml::FrameworkElement::MarginProperty())
            property = "Margin";
        else if (known_property == xaml::Controls::Border::BorderThicknessProperty())
            property = "BorderThickness";
    }
    const bool custom = value.is_object() && (value.value("kind", "") == "blur" ||
        (value.value("kind", "") == "gradient" && (value.contains("shape") || value.contains("angle") || value.contains("extend") || value.contains("motion"))));
    const auto compositor =
        custom ? xaml::Hosting::ElementCompositionPreview::GetElementVisual(element).Compositor()
               : composition::Compositor(nullptr);
    const auto key = std::wstring(winrt::to_hstring(
        type + ":" + property + ":" + value.dump() +
        (custom ? ":" + std::to_string(reinterpret_cast<uintptr_t>(winrt::get_abi(compositor)))
                : "")));
    if (auto found = cache.find(key); !custom && found != cache.end())
        return found->second;
    Value result;
    std::vector<std::string> candidates{type};
    // A setter on an unrelated target class can resolve successfully. Passing
    // its property index to this element can access incompatible native storage.
    // Only interfaces actually implemented by the element are valid fallbacks.
    auto add_candidate = [&]<typename T>(const char *qualified_type) {
        if (element.try_as<T>() && type != qualified_type)
            candidates.emplace_back(qualified_type);
    };
    add_candidate.template operator()<xaml::Controls::Grid>("Windows.UI.Xaml.Controls.Grid");
    add_candidate.template operator()<xaml::Controls::Border>("Windows.UI.Xaml.Controls.Border");
    add_candidate.template operator()<xaml::Controls::ContentPresenter>("Windows.UI.Xaml.Controls.ContentPresenter");
    add_candidate.template operator()<xaml::Controls::TextBlock>("Windows.UI.Xaml.Controls.TextBlock");
    add_candidate.template operator()<xaml::Controls::Image>("Windows.UI.Xaml.Controls.Image");
    add_candidate.template operator()<xaml::Shapes::Rectangle>("Windows.UI.Xaml.Shapes.Rectangle");
    add_candidate.template operator()<xaml::Shapes::Shape>("Windows.UI.Xaml.Shapes.Shape");
    add_candidate.template operator()<xaml::Controls::Control>("Windows.UI.Xaml.Controls.Control");
    add_candidate.template operator()<xaml::Controls::Panel>("Windows.UI.Xaml.Controls.Panel");
    add_candidate.template operator()<xaml::FrameworkElement>("Windows.UI.Xaml.FrameworkElement");
    HRESULT last = E_INVALIDARG;
    for (const auto &candidate : candidates) {
        try {
            auto dictionary = xaml::Markup::XamlReader::Load(
                                  Document(candidate, property, custom ? Json(nullptr) : value))
                                  .as<xaml::ResourceDictionary>();
            auto style = dictionary.First().Current().Value().as<xaml::Style>();
            auto setter = style.Setters().GetAt(0).as<xaml::Setter>();
            result = {setter.Property(), setter.Value()};
            if (known_property && result.property != known_property)
                winrt::throw_hresult(E_INVALIDARG);
            if (value.is_null())
                result.value = xaml::DependencyProperty::UnsetValue();
            if (custom) {
                auto host = xaml::Hosting::ElementCompositionPreview::GetElementVisual(element);
                result.value = value.value("kind", "") == "gradient" ? winrt::make<GradientBrush>(host, value).as<foundation::IInspectable>() : winrt::make<BlurBrush>(compositor, value, host).as<foundation::IInspectable>();
            }
            if (cache.size() >= 1024)
                cache.clear();
            if (!custom) cache.emplace(key, result);
            return result;
        } catch (...) {
            last = winrt::to_hresult();
        }
    }
    winrt::throw_hresult(last);
}
} // namespace taskbar_xaml

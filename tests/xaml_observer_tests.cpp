#include "mw_native_sdk.h"
#include "resident_hooks.h"
#include <Windows.h>
#undef GetCurrentTime
#include <DispatcherQueue.h>
#include <windows.ui.xaml.hosting.desktopwindowxamlsource.h>
#include <psapi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>
#include <array>
#include <atomic>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>

namespace {
struct Host {
    MwHostApi api{};
    std::array<void*, 3> targets{};
    std::array<bool, 3> created{};
    Host() {
        api.struct_size = sizeof(api);
        api.abi_version = MW_NATIVE_ABI_VERSION;
        api.addon_context = this;
        api.log = [](void*, const char* message) -> MwStatus { std::cerr << message << '\n'; return MW_OK; };
        api.resolve_export = [](void*, const wchar_t* module, const char* name,
            void** target) -> MwStatus {
            auto handle = GetModuleHandleW(module);
            if (!handle) handle = LoadLibraryW(module);
            *target = handle ? reinterpret_cast<void*>(GetProcAddress(handle, name)) : nullptr;
            return *target ? MW_OK : MW_ERROR_SYMBOL_NOT_FOUND;
        };
        api.hook_create = [](void* context, void* target, void* detour,
            void** original, uint64_t* id) -> MwStatus {
            auto& host = *static_cast<Host*>(context);
            const size_t index = !host.created[1] ? 1 : 2;
            if (mw::hooks::Create(reinterpret_cast<ULONG_PTR>(context), target,
                detour, original) != MH_OK) return MW_ERROR_HOOK;
            host.targets[index] = target;
            host.created[index] = true;
            *id = index;
            return MW_OK;
        };
        api.hook_enable = [](void* context, uint64_t id) -> MwStatus {
            auto& host = *static_cast<Host*>(context);
            return mw::hooks::Enable(reinterpret_cast<ULONG_PTR>(context), host.targets[id]) == MH_OK ? MW_OK : MW_ERROR_HOOK;
        };
        api.hook_disable = [](void* context, uint64_t id) -> MwStatus {
            auto& host = *static_cast<Host*>(context);
            return mw::hooks::Disable(reinterpret_cast<ULONG_PTR>(context), host.targets[id]) == MH_OK ? MW_OK : MW_ERROR_HOOK;
        };
        api.hook_remove = [](void* context, uint64_t id) -> MwStatus {
            auto& host = *static_cast<Host*>(context);
            if (mw::hooks::Remove(reinterpret_cast<ULONG_PTR>(context), host.targets[id]) != MH_OK) return MW_ERROR_HOOK;
            host.created[id] = false;
            return MW_OK;
        };
    }
};

void Check(bool good, const char* message) {
    if (!good) throw std::runtime_error(message);
}
void Pump() {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    Sleep(5);
}

class SecondaryIsland {
public:
    SecondaryIsland() : thread_([this](std::stop_token stop) {
        try {
            winrt::init_apartment(winrt::apartment_type::single_threaded);
            winrt::Windows::System::DispatcherQueueController queue{nullptr};
            DispatcherQueueOptions options{sizeof(options), DQTYPE_THREAD_CURRENT, DQTAT_COM_STA};
            winrt::check_hresult(CreateDispatcherQueueController(options,
                reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(queue))));
            auto manager = winrt::Windows::UI::Xaml::Hosting::WindowsXamlManager::InitializeForCurrentThread();
            HWND window = CreateWindowExW(0, L"MyWallpaperIsolatedXamlObserverTest", L"Secondary XAML fixture",
                WS_OVERLAPPED, 0, 0, 160, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            Check(window != nullptr, "secondary hidden window");
            winrt::Windows::UI::Xaml::Hosting::DesktopWindowXamlSource source;
            winrt::check_hresult(source.as<IDesktopWindowXamlSourceNative>()->AttachToWindow(window));
            winrt::Windows::UI::Xaml::Controls::Grid content;
            content.Name(L"SecondaryObserverRoot");
            source.Content(content);
            result_.store(S_OK, std::memory_order_release);
            while (!stop.stop_requested()) Pump();
            source.Close();
            DestroyWindow(window);
        } catch (...) { result_.store(winrt::to_hresult(), std::memory_order_release); }
        done_.store(true, std::memory_order_release);
    }) {
        const auto deadline = GetTickCount64() + 2500;
        while (result_.load(std::memory_order_acquire) == E_PENDING && GetTickCount64() < deadline) Pump();
    }
    ~SecondaryIsland() {
        thread_.request_stop();
        while (!done_.load(std::memory_order_acquire)) Pump();
    }
    HRESULT Result() const { return result_.load(std::memory_order_acquire); }
private:
    std::atomic<HRESULT> result_{E_PENDING};
    std::atomic<bool> done_{false};
    std::jthread thread_;
};

size_t PrivateBytes() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    Check(GetProcessMemoryInfo(GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)) != FALSE,
        "private memory counters");
    return counters.PrivateUsage;
}
}

int wmain(int argc, wchar_t** argv) {
    const char* phase = "initialization";
    try {
        Check(argc >= 2 && argc <= 4, "usage: observer-tests fixture.dll [settings.json|-] [cycles]");
        const int cycles = argc == 4 ? std::stoi(argv[3]) : 3;
        Check(cycles >= 3 && cycles <= 500, "cycle range");
        std::string settings = "{\"deviceSettings\":{\"enabled\":true}}";
        if (argc >= 3 && wcscmp(argv[2], L"-") != 0) {
            std::ifstream file(argv[2], std::ios::binary);
            Check(file.good(), "settings file");
            settings.assign(std::istreambuf_iterator<char>(file), {});
        }
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        winrt::Windows::System::DispatcherQueueController queue{nullptr};
        DispatcherQueueOptions options{sizeof(options), DQTYPE_THREAD_CURRENT, DQTAT_COM_STA};
        winrt::check_hresult(CreateDispatcherQueueController(options,
            reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(queue))));
        auto manager = winrt::Windows::UI::Xaml::Hosting::WindowsXamlManager::InitializeForCurrentThread();
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"MyWallpaperIsolatedXamlObserverTest";
        Check(RegisterClassW(&wc) != 0, "fixture window class");
        HWND window = CreateWindowExW(0, wc.lpszClassName, L"XAML observer fixture",
            WS_OVERLAPPED, 0, 0, 320, 100, nullptr, nullptr, wc.hInstance, nullptr);
        Check(window != nullptr, "hidden fixture window");
        winrt::Windows::UI::Xaml::Hosting::DesktopWindowXamlSource source;
        auto native = source.as<IDesktopWindowXamlSourceNative>();
        winrt::check_hresult(native->AttachToWindow(window));
        HWND island = nullptr;
        winrt::check_hresult(native->get_WindowHandle(&island));
        SetWindowPos(island, nullptr, 0, 0, 320, 100, SWP_NOACTIVATE | SWP_NOZORDER);
        winrt::Windows::UI::Xaml::Controls::Grid root;
        root.Name(L"TaskbarFrame");
        root.Width(320);
        root.Height(100);
        const winrt::Windows::UI::Xaml::Media::SolidColorBrush original_fill(
            winrt::Windows::UI::Color{255, 19, 29, 39});
        winrt::Windows::UI::Xaml::Shapes::Rectangle surface;
        surface.Name(L"BackgroundFill");
        surface.Fill(original_fill);
        root.Children().Append(surface);
        source.Content(root);
        for (int i = 0; i < 10; ++i) Pump();
        SecondaryIsland secondary;
        winrt::check_hresult(secondary.Result());
        Check(MH_Initialize() == MH_OK, "hook initialization");
        Host host;
        const auto fixture = LoadLibraryW(argv[1]);
        Check(fixture != nullptr, "fixture DLL load");
        using Start = HRESULT(__stdcall*)(const MwHostApi*, const char*, DWORD);
        using Poll = HRESULT(__stdcall*)();
        using GetApi = MwStatus(MW_CALL*)(uint32_t, MwAddonApi*);
        const auto start = reinterpret_cast<Start>(GetProcAddress(fixture, "MwTaskbarTestStartOnHost"));
        const auto poll = reinterpret_cast<Poll>(GetProcAddress(fixture, "MwTaskbarTestPollOnHost"));
        const auto get_api = reinterpret_cast<GetApi>(GetProcAddress(fixture, "MwAddonGetApi"));
        const auto counts = reinterpret_cast<volatile LONG64*>(GetProcAddress(fixture, "MwTaskbarConfiguratorDiagnosticsBlock"));
        const auto lifecycle = reinterpret_cast<volatile LONG64*>(GetProcAddress(fixture, "MwTaskbarConfiguratorLifecycleDiagnosticsBlock"));
        const auto guard = reinterpret_cast<volatile LONG64*>(GetProcAddress(fixture, "MwTaskbarConfiguratorCompositionDiagnosticsBlock"));
        const auto stage = reinterpret_cast<volatile LONG64*>(GetProcAddress(fixture, "MwTaskbarConfiguratorStartupStage"));
        Check(start && poll && get_api && counts && lifecycle && guard && stage, "fixture exports");
        MwAddonApi addon_api{};
        Check(get_api(MW_NATIVE_ABI_VERSION, &addon_api) == MW_OK, "fixture ABI");
        size_t settled_memory = 0;
        for (int cycle = 0; cycle < cycles; ++cycle) {
            phase = "real diagnostics startup";
            winrt::check_hresult(start(&host.api, settings.c_str(), GetCurrentThreadId()));
            HRESULT result = E_PENDING;
            const auto deadline = GetTickCount64() + 8000;
            while (result == E_PENDING && GetTickCount64() < deadline) { Pump(); result = poll(); }
            std::cout << "cycle=" << cycle << " stage=" << *stage << " entries=" << counts[1]
                << " registry-reads=" << guard[2] << " hresult=0x" << std::hex << result << std::dec << '\n';
            winrt::check_hresult(result);
            Check(counts[1] > 0 && counts[8] == 0 && guard[2] > 0 && guard[3] == 0 && guard[4] == MW_OK, "real observer/guard not established");
            Check(lifecycle[1] == 1 && lifecycle[2] == 1, "observer was re-enumerated");
            phase = "real diagnostics stop";
            std::atomic<bool> finished{false};
            std::atomic<MwStatus> stopped{MW_ERROR_STATE};
            std::thread worker([&] { stopped = addon_api.stop(); finished = true; });
            while (!finished) Pump();
            worker.join();
            Check(stopped == MW_OK && counts[1] == 0 && counts[2] == 0 && counts[3] == 0 && counts[4] == 0 && counts[7] == 0 && counts[8] == 0, "stop resources");
            Check(surface.Fill() == original_fill, "original native fill was not restored");
            root.Children().Clear();
            surface = winrt::Windows::UI::Xaml::Shapes::Rectangle();
            surface.Name(L"BackgroundFill");
            surface.Fill(original_fill);
            root.Children().Append(surface);
            if (cycle == 4) settled_memory = PrivateBytes();
        }
        const auto hook_metrics = mw::hooks::GetMetrics();
        Check(hook_metrics.retained_records == 2 &&
            hook_metrics.registered_hooks == 0 && hook_metrics.active_hooks == 0, "resident bridge bound");
        std::cout << "private-bytes-after-warmup=" << settled_memory << " private-bytes-at-stop=" << PrivateBytes()
            << " resident-bridges=" << hook_metrics.retained_records << '\n';
        source.Close();
        DestroyWindow(window);
        std::cout << "Real XAML connection with two UI threads, scoped composition exclusion, " << cycles
            << " UI snapshots, and stop/restoration passed in an isolated process.\n";
        return 0;
    } catch (...) {
        std::cerr << phase << " HRESULT=0x" << std::hex << winrt::to_hresult() << '\n';
        return 1;
    }
}

#include "composition_diagnostics_guard.hpp"
#include "mw_native_sdk.hpp"
#include <Windows.h>
#include <cstring>
#include <cwchar>

extern "C" __declspec(dllexport) volatile LONG64
    MwTaskbarConfiguratorCompositionDiagnosticsBlock[5] = {1, 0, 0, 0, 0};

namespace taskbar_diagnostics {
namespace {
mw_native::Hook key_hook;
mw_native::Hook value_hook;
thread_local bool enumeration_active = false;
thread_local HKEY enumeration_key = nullptr;

void RecordState(MwStatus status) noexcept {
    InterlockedExchange64(&MwTaskbarConfiguratorCompositionDiagnosticsBlock[3],
        static_cast<LONG64>(key_hook.busy()) + static_cast<LONG64>(value_hook.busy()));
    InterlockedExchange64(&MwTaskbarConfiguratorCompositionDiagnosticsBlock[4], status);
}

LSTATUS WINAPI OpenObserverKey(HKEY parent, LPCWSTR path, DWORD options,
    REGSAM access, PHKEY result) noexcept {
    const auto original = reinterpret_cast<decltype(&RegOpenKeyExW)>(key_hook.original());
    if (!original) return ERROR_INVALID_FUNCTION;
    const LSTATUS status = original(parent, path, options, access, result);
    if (!enumeration_active || parent != HKEY_LOCAL_MACHINE || !path || !result ||
        _wcsicmp(path, L"Software\\Microsoft\\XAML\\Debug") != 0)
        return status;

    if (status == ERROR_SUCCESS) {
        enumeration_key = *result;
        return status;
    }
    if (status != ERROR_FILE_NOT_FOUND && status != ERROR_PATH_NOT_FOUND)
        return status;

    // Give Windows a real, normally closed key handle when its optional debug
    // key is absent. Only this thread and this handle can receive the override.
    const LSTATUS opened = original(parent, L"Software\\Microsoft", options, access, result);
    if (opened == ERROR_SUCCESS) enumeration_key = *result;
    return opened;
}

LSTATUS WINAPI ReadObserverFlag(HKEY key, LPCWSTR name, LPDWORD reserved,
    LPDWORD type, LPBYTE bytes, LPDWORD byte_count) noexcept {
    const auto original = reinterpret_cast<decltype(&RegQueryValueExW)>(value_hook.original());
    if (!original) return ERROR_INVALID_FUNCTION;
    if (!enumeration_active || !enumeration_key || key != enumeration_key ||
        !name || _wcsicmp(name, L"DisableCompositionDiag") != 0)
        return original(key, name, reserved, type, bytes, byte_count);

    if (reserved || (bytes && !byte_count)) return ERROR_INVALID_PARAMETER;
    const DWORD capacity = byte_count ? *byte_count : 0;
    if (type) *type = REG_DWORD;
    if (byte_count) *byte_count = sizeof(DWORD);
    if (bytes && capacity < sizeof(DWORD)) return ERROR_MORE_DATA;
    if (bytes) {
        const DWORD disabled = 1;
        std::memcpy(bytes, &disabled, sizeof(disabled));
    }
    InterlockedIncrement64(&MwTaskbarConfiguratorCompositionDiagnosticsBlock[2]);
    return ERROR_SUCCESS;
}
}

MwStatus RemoveCompositionGuard() noexcept {
    // Remove both even when one fails; the SDK retains failed cleanup handles
    // and the original slots, so Stop can retry without invalidating detours.
    const MwStatus value_status = value_hook.stop();
    const MwStatus key_status = key_hook.stop();
    const MwStatus status = value_status != MW_OK ? value_status : key_status;
    RecordState(status);
    return status;
}

MwStatus InstallCompositionGuard(const MwHostApi* host) noexcept {
    if (!mw_native::IsCompatibleHost(host)) return MW_ERROR_ABI;
    if (key_hook.busy() || value_hook.busy()) return MW_ERROR_STATE;
    void* open = nullptr;
    void* query = nullptr;
    MwStatus status = host->resolve_export(host->addon_context,
        L"advapi32.dll", "RegOpenKeyExW", &open);
    if (status == MW_OK) status = host->resolve_export(host->addon_context,
        L"advapi32.dll", "RegQueryValueExW", &query);
    if (status == MW_OK) status = key_hook.start(host, open,
        reinterpret_cast<void*>(&OpenObserverKey));
    if (status == MW_OK) status = value_hook.start(host, query,
        reinterpret_cast<void*>(&ReadObserverFlag));
    if (status != MW_OK) (void)RemoveCompositionGuard();
    RecordState(status);
    return status;
}

CompositionEnumerationScope::CompositionEnumerationScope() noexcept
    : previous_active_(enumeration_active), previous_key_(enumeration_key) {
    enumeration_key = nullptr;
    enumeration_active = true;
    InterlockedIncrement64(&MwTaskbarConfiguratorCompositionDiagnosticsBlock[1]);
}

CompositionEnumerationScope::~CompositionEnumerationScope() {
    enumeration_active = previous_active_;
    enumeration_key = static_cast<HKEY>(previous_key_);
}
}

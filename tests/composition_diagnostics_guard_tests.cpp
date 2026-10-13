#include "composition_diagnostics_guard.hpp"
#include "resident_hooks.h"
#include <Windows.h>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using Open = decltype(&RegOpenKeyExW);
using Query = decltype(&RegQueryValueExW);
const auto fixture_key = reinterpret_cast<HKEY>(static_cast<uintptr_t>(0x123456));
unsigned forwarded_reads = 0;
bool existing_debug_key = false;

LSTATUS WINAPI FixtureOpen(HKEY, LPCWSTR path, DWORD, REGSAM, PHKEY result) {
    if (!result) return ERROR_INVALID_PARAMETER;
    if (_wcsicmp(path, L"Software\\Microsoft") == 0 || existing_debug_key) {
        *result = fixture_key;
        return ERROR_SUCCESS;
    }
    return ERROR_FILE_NOT_FOUND;
}
LSTATUS WINAPI FixtureQuery(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD) {
    ++forwarded_reads;
    return ERROR_FILE_NOT_FOUND;
}

struct Host {
    MwHostApi api{};
    bool real = false;
    bool reject_query_enable = false;
    bool reject_query_disable = false;
    std::array<void*, 3> targets{};
    std::array<void*, 3> detours{};
    std::array<bool, 3> created{};
    std::array<bool, 3> enabled{};
    Host(bool actual = false) : real(actual) {
        api.struct_size = sizeof(api);
        api.abi_version = MW_NATIVE_ABI_VERSION;
        api.addon_context = this;
        api.log = [](void*, const char*) -> MwStatus { return MW_OK; };
        api.resolve_export = [](void* context, const wchar_t* module,
            const char* name, void** target) -> MwStatus {
            auto& host = *static_cast<Host*>(context);
            if (host.real) {
                const auto handle = GetModuleHandleW(module);
                *target = handle ? reinterpret_cast<void*>(GetProcAddress(handle, name)) : nullptr;
            } else {
                *target = std::strcmp(name, "RegOpenKeyExW") == 0
                    ? reinterpret_cast<void*>(&FixtureOpen) : reinterpret_cast<void*>(&FixtureQuery);
            }
            return *target ? MW_OK : MW_ERROR_SYMBOL_NOT_FOUND;
        };
        api.hook_create = [](void* context, void* target, void* detour,
            void** original, uint64_t* id) -> MwStatus {
            auto& host = *static_cast<Host*>(context);
            const size_t index = !host.created[1] ? 1 : 2;
            if (host.real) {
                if (mw::hooks::Create(reinterpret_cast<ULONG_PTR>(context), target,
                    detour, original) != MH_OK) return MW_ERROR_HOOK;
            } else {
                InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(original), target);
            }
            host.targets[index] = target;
            host.detours[index] = detour;
            host.created[index] = true;
            *id = index;
            return MW_OK;
        };
        api.hook_enable = [](void* context, uint64_t id) -> MwStatus {
            auto& host = *static_cast<Host*>(context);
            if (id == 2 && host.reject_query_enable) return MW_ERROR_HOOK;
            if (host.real && mw::hooks::Enable(reinterpret_cast<ULONG_PTR>(context), host.targets[id]) != MH_OK) return MW_ERROR_HOOK;
            host.enabled[id] = true;
            return MW_OK;
        };
        api.hook_disable = [](void* context, uint64_t id) -> MwStatus {
            auto& host = *static_cast<Host*>(context);
            if (id == 2 && host.reject_query_disable) return MW_ERROR_HOOK;
            if (host.real && mw::hooks::Disable(reinterpret_cast<ULONG_PTR>(context), host.targets[id]) != MH_OK) return MW_ERROR_HOOK;
            host.enabled[id] = false;
            return MW_OK;
        };
        api.hook_remove = [](void* context, uint64_t id) -> MwStatus {
            auto& host = *static_cast<Host*>(context);
            if (host.real && mw::hooks::Remove(reinterpret_cast<ULONG_PTR>(context), host.targets[id]) != MH_OK) return MW_ERROR_HOOK;
            host.created[id] = false;
            return MW_OK;
        };
    }
};

void Check(bool good, const char* message) {
    if (!good) throw std::runtime_error(message);
}

void CheckFlag(Open open, Query query, bool actual) {
    HKEY key = nullptr;
    Check(open(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\XAML\\Debug", 0,
        KEY_QUERY_VALUE, &key) == ERROR_SUCCESS, "debug key read failed");
    DWORD type = 0, size = 0;
    Check(query(key, L"DisableCompositionDiag", nullptr, &type, nullptr, &size) == ERROR_SUCCESS && type == REG_DWORD && size == 4, "size query");
    std::array<BYTE, 7> bytes{9,9,9,9,9,9,9};
    size = 3;
    Check(query(key, L"DisableCompositionDiag", nullptr, &type, bytes.data()+1, &size) == ERROR_MORE_DATA && size == 4 && bytes[1] == 9, "small buffer");
    size = 4;
    Check(query(key, L"DisableCompositionDiag", nullptr, &type, bytes.data()+1, &size) == ERROR_SUCCESS && bytes[0] == 9 && bytes[1] == 1 && bytes[2] == 0 && bytes[5] == 9, "unaligned DWORD");
    Check(query(key, L"DisableCompositionDiag", nullptr, &type, bytes.data(), nullptr) == ERROR_INVALID_PARAMETER, "missing count accepted");
    DWORD reserved = 0;
    Check(query(key, L"DisableCompositionDiag", &reserved, &type, nullptr, &size) == ERROR_INVALID_PARAMETER, "reserved pointer accepted");
    if (actual) RegCloseKey(key);
}
}

int main() {
    try {
        using namespace taskbar_diagnostics;
        Host fixture;
        Check(InstallCompositionGuard(&fixture.api) == MW_OK, "fake installation");
        const auto open = reinterpret_cast<Open>(fixture.detours[1]);
        const auto query = reinterpret_cast<Query>(fixture.detours[2]);
        DWORD size = 4;
        Check(query(fixture_key, L"DisableCompositionDiag", nullptr, nullptr, nullptr, &size) == ERROR_FILE_NOT_FOUND, "unscoped read was overridden");
        {
            CompositionEnumerationScope scope;
            CheckFlag(open, query, false);
            size = 4;
            Check(query(reinterpret_cast<HKEY>(1), L"DisableCompositionDiag", nullptr, nullptr, nullptr, &size) == ERROR_FILE_NOT_FOUND, "wrong key overridden");
            Check(query(fixture_key, L"OtherValue", nullptr, nullptr, nullptr, &size) == ERROR_FILE_NOT_FOUND, "other value overridden");
            LSTATUS other_thread = ERROR_SUCCESS;
            std::thread other([&] { DWORD count = 4; other_thread = query(fixture_key, L"DisableCompositionDiag", nullptr, nullptr, nullptr, &count); });
            other.join();
            Check(other_thread == ERROR_FILE_NOT_FOUND, "another thread was overridden");
            existing_debug_key = true;
            CheckFlag(open, query, false);
            existing_debug_key = false;
        }
        Check(query(fixture_key, L"DisableCompositionDiag", nullptr, nullptr, nullptr, &size) == ERROR_FILE_NOT_FOUND, "scope did not restore forwarding");
        fixture.reject_query_disable = true;
        Check(RemoveCompositionGuard() == MW_ERROR_HOOK && fixture.created[2] && !fixture.created[1], "failed cleanup state not retained");
        fixture.reject_query_disable = false;
        Check(RemoveCompositionGuard() == MW_OK && !fixture.created[2], "cleanup retry");
        fixture.reject_query_enable = true;
        Check(InstallCompositionGuard(&fixture.api) == MW_ERROR_HOOK && !fixture.created[1] && !fixture.created[2], "partial installation rollback");
        fixture.reject_query_enable = false;

        Check(MH_Initialize() == MH_OK, "native hook backend initialization");
        Host actual(true);
        auto advapi = LoadLibraryW(L"advapi32.dll");
        Check(advapi != nullptr, "registry API load");
        const auto actual_open = reinterpret_cast<Open>(GetProcAddress(advapi, "RegOpenKeyExW"));
        const auto actual_query = reinterpret_cast<Query>(GetProcAddress(advapi, "RegQueryValueExW"));
        for (int cycle = 0; cycle < 40; ++cycle) {
            Check(InstallCompositionGuard(&actual.api) == MW_OK, "real hook installation");
            {
                CompositionEnumerationScope scope;
                CheckFlag(actual_open, actual_query, true);
            }
            Check(RemoveCompositionGuard() == MW_OK, "real hook removal");
        }
        const auto metrics = mw::hooks::GetMetrics();
        Check(metrics.active_hooks == 0 && metrics.registered_hooks == 0 && metrics.retained_records == 2 && metrics.reuse_count == 78, "hook generations accumulated");
        // The engine deliberately retains the two bypassing bridges until
        // process exit, so an entered detour always has a valid continuation.
        Check(mw::hooks::Uninitialize() == MH_ERROR_UNABLE_TO_UNINITIALIZE,
            "resident bridge lifetime contract changed");
        FreeLibrary(advapi);
        std::cout << "Scoped registry reads, original forwarding, rollback/retry, and 40 real MinHook cycles passed; two retained bridges.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

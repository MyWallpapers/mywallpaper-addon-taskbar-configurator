#include <Windows.h>
#include <xamlom.h>

#include <atomic>
#include <cwchar>
#include <iostream>

namespace {
std::atomic<unsigned> g_forbidden_bstr_free_calls{0};

bool Matches(BSTR value, const wchar_t* expected) noexcept {
    if (!value) return false;
    const size_t expected_length = std::wcslen(expected);
    return SysStringLen(value) == expected_length &&
        std::wmemcmp(value, expected, expected_length) == 0;
}
}

extern "C" void WINAPI MwTaskbarTestRecordForbiddenBstrFree(BSTR) noexcept {
    g_forbidden_bstr_free_calls.fetch_add(1, std::memory_order_relaxed);
}

extern "C" HRESULT __stdcall MwTaskbarTestInvokeOnVisualTreeChange(
    VisualElement element) noexcept;
extern "C" HRESULT __stdcall MwTaskbarTestDiagnosticCacheResolution(
    uint32_t* expired_skipped, uint32_t* correct_cache,
    uint32_t* cache_retained, uint32_t* unexpected_error_preserved) noexcept;
extern "C" HRESULT __stdcall MwTaskbarTestPartialHandlerRollback(
    uint32_t* registered_before_rollback, uint32_t* attempted_rollback,
    uint32_t* remaining_after_rollback, HRESULT* registration_result) noexcept;
extern "C" HRESULT __stdcall MwTaskbarTestIndependentHandlerRevocation(
    uint32_t* first_attempted, uint32_t* first_remaining,
    uint32_t* retry_attempted, uint32_t* final_remaining) noexcept;
extern "C" HRESULT __stdcall MwTaskbarTestLateCleanupCompletion(
    uint32_t* scheduled_count, uint32_t* timeout_before_callback,
    uint32_t* timeout_during_diagnostics, uint32_t* map_reads_after_initial_stop,
    uint32_t* map_reads_after_pending_retries,
    uint32_t* map_reads_after_completion, uint32_t* diagnostics_reads,
    uint32_t* start_allowed_before_completion,
    uint32_t* start_allowed_after_completion) noexcept;
extern "C" HRESULT __stdcall MwTaskbarTestStaleApplyGeneration(
    uint32_t* stale_preserved_queue, uint32_t* current_cleared_queue) noexcept;

int wmain(int argc, wchar_t** argv) {
    unsigned expected_free_calls = 0;
    if (argc != 1) {
        if (argc != 3 || std::wcscmp(argv[1], L"--expect-free-calls") != 0) {
            std::wcerr << L"usage: taskbar_visual_tree_callback_tests "
                L"[--expect-free-calls count]\n";
            return 2;
        }
        wchar_t* end = nullptr;
        const unsigned long parsed = std::wcstoul(argv[2], &end, 10);
        if (!end || *end != L'\0' || parsed > 4) {
            std::wcerr << L"expected free-call count must be between 0 and 4\n";
            return 2;
        }
        expected_free_calls = static_cast<unsigned>(parsed);
    }

    VisualElement element{};
    element.SrcInfo.FileName = SysAllocString(L"taskbar.xaml");
    element.SrcInfo.Hash = SysAllocString(L"test-hash");
    element.Type = SysAllocString(L"TaskbarElement");
    element.Name = SysAllocString(L"TaskbarFrame");
    if (!element.SrcInfo.FileName || !element.SrcInfo.Hash ||
        !element.Type || !element.Name) {
        SysFreeString(element.SrcInfo.FileName);
        SysFreeString(element.SrcInfo.Hash);
        SysFreeString(element.Type);
        SysFreeString(element.Name);
        std::wcerr << L"SysAllocString failed\n";
        return 3;
    }

    const HRESULT result = MwTaskbarTestInvokeOnVisualTreeChange(element);
    const unsigned free_calls = g_forbidden_bstr_free_calls.load(
        std::memory_order_relaxed);
    const bool caller_still_owns_values =
        Matches(element.SrcInfo.FileName, L"taskbar.xaml") &&
        Matches(element.SrcInfo.Hash, L"test-hash") &&
        Matches(element.Type, L"TaskbarElement") &&
        Matches(element.Name, L"TaskbarFrame");

    SysFreeString(element.SrcInfo.FileName);
    SysFreeString(element.SrcInfo.Hash);
    SysFreeString(element.Type);
    SysFreeString(element.Name);

    if (result != S_OK || free_calls != expected_free_calls ||
        !caller_still_owns_values) {
        std::wcerr << L"callback HRESULT=0x" << std::hex << result
            << L", forbidden frees=" << std::dec << free_calls
            << L" (expected " << expected_free_calls << L"), caller-owned values="
            << (caller_still_owns_values ? L"intact" : L"invalid") << L'\n';
        return 1;
    }

    uint32_t registered_before_rollback = 0;
    uint32_t expired_skipped = 0, correct_cache = 0, cache_retained = 0;
    uint32_t unexpected_error_preserved = 0;
    const HRESULT cache_result = MwTaskbarTestDiagnosticCacheResolution(
        &expired_skipped, &correct_cache, &cache_retained,
        &unexpected_error_preserved);
    if (cache_result != S_OK || !expired_skipped || !correct_cache ||
        !cache_retained || !unexpected_error_preserved) {
        std::wcerr << L"diagnostic cache resolution failed: HRESULT=0x"
            << std::hex << cache_result << L", expired=" << expired_skipped
            << L", correct cache=" << correct_cache << L", retained="
            << cache_retained << L", real error=" << unexpected_error_preserved << L'\n';
        return 8;
    }

    uint32_t attempted_rollback = 0;
    uint32_t remaining_after_rollback = 0;
    HRESULT registration_result = S_OK;
    const HRESULT rollback_result = MwTaskbarTestPartialHandlerRollback(
        &registered_before_rollback, &attempted_rollback,
        &remaining_after_rollback, &registration_result);
    if (rollback_result != S_OK || registration_result != E_FAIL ||
        registered_before_rollback != 0x3 || attempted_rollback != 0x3 ||
        remaining_after_rollback != 0) {
        std::wcerr << L"partial registration rollback failed: registration=0x"
            << std::hex << registration_result << L", rollback=0x" << rollback_result
            << L", registered=0x" << registered_before_rollback
            << L", attempted=0x" << attempted_rollback
            << L", remaining=0x" << remaining_after_rollback << L'\n';
        return 4;
    }

    uint32_t first_attempted = 0;
    uint32_t first_remaining = 0;
    uint32_t retry_attempted = 0;
    uint32_t final_remaining = 0;
    const HRESULT revoke_result = MwTaskbarTestIndependentHandlerRevocation(
        &first_attempted, &first_remaining, &retry_attempted, &final_remaining);
    if (revoke_result != E_FAIL || first_attempted != 0xF ||
        first_remaining != 0x2 || retry_attempted != 0x2 || final_remaining != 0) {
        std::wcerr << L"independent handler revocation failed: result=0x"
            << std::hex << revoke_result << L", attempted=0x" << first_attempted
            << L", remaining=0x" << first_remaining << L", retry=0x"
            << retry_attempted << L", final=0x" << final_remaining << L'\n';
        return 5;
    }

    uint32_t scheduled_count = 0;
    uint32_t timeout_before_callback = 0;
    uint32_t timeout_during_diagnostics = 0;
    uint32_t map_reads_after_initial_stop = 0;
    uint32_t map_reads_after_pending_retries = 0;
    uint32_t map_reads_after_completion = 0;
    uint32_t diagnostics_reads = 0;
    uint32_t start_allowed_before_completion = 0;
    uint32_t start_allowed_after_completion = 0;
    const HRESULT cleanup_result = MwTaskbarTestLateCleanupCompletion(
        &scheduled_count, &timeout_before_callback,
        &timeout_during_diagnostics, &map_reads_after_initial_stop,
        &map_reads_after_pending_retries, &map_reads_after_completion,
        &diagnostics_reads, &start_allowed_before_completion,
        &start_allowed_after_completion);
    if (cleanup_result != S_OK || scheduled_count != 1 ||
        timeout_before_callback != 2 || timeout_during_diagnostics != 1 ||
        map_reads_after_initial_stop != 1 || map_reads_after_pending_retries != 1 ||
        map_reads_after_completion != 2 || diagnostics_reads != 1 ||
        start_allowed_before_completion != 0 || start_allowed_after_completion != 1) {
        std::wcerr << L"late cleanup completion failed: result=0x" << std::hex
            << cleanup_result << L", scheduled=" << std::dec << scheduled_count
            << L", pending-before-start=" << timeout_before_callback
            << L", pending-during-diagnostics=" << timeout_during_diagnostics
            << L", map-reads initial/pending-retries/completed="
            << map_reads_after_initial_stop << L"/" << map_reads_after_pending_retries
            << L"/" << map_reads_after_completion
            << L", diagnostics-reads=" << diagnostics_reads
            << L", start-allowed before/after=" << start_allowed_before_completion
            << L"/" << start_allowed_after_completion << L'\n';
        return 6;
    }

    uint32_t stale_preserved_queue = 0;
    uint32_t current_cleared_queue = 0;
    const HRESULT generation_result = MwTaskbarTestStaleApplyGeneration(
        &stale_preserved_queue, &current_cleared_queue);
    if (generation_result != S_OK || stale_preserved_queue != 1 ||
        current_cleared_queue != 1) {
        std::wcerr << L"stale settings generation changed current queue state: result=0x"
            << std::hex << generation_result << L", stale-preserved="
            << std::dec << stale_preserved_queue << L", current-cleared="
            << current_cleared_queue << L'\n';
        return 7;
    }

    std::wcout << L"callback HRESULT=S_OK, forbidden frees=" << free_calls
        << L", caller-owned values=intact; partial registration rolled back; "
        << L"all handlers attempted and failed handler retried; cleanup timeout reused "
        << L"one operation through late completion; stale generation preserved queue state\n";
    return 0;
}

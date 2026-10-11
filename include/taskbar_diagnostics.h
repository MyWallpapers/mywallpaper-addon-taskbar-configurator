#pragma once

#include <stdint.h>

/* Read-only to host diagnostics. The add-on alone updates this process-local
 * block; a test host reads it with ReadProcessMemory. All fields are aligned
 * 64-bit counters to keep remote snapshots coherent on x64. */
typedef struct MwTaskbarConfiguratorDiagnostics {
    uint64_t version;
    uint64_t matchedElements;
    uint64_t retainedProperties;
    uint64_t eventHandlers;
    uint64_t animations;
    uint64_t settingsApplied;
    uint64_t restoreCount;
    uint64_t workerCount;
    int64_t lastError;
} MwTaskbarConfiguratorDiagnostics;

#if defined(__cplusplus)
static_assert(sizeof(MwTaskbarConfiguratorDiagnostics) == 72,
    "The read-only diagnostics ABI must remain 72 bytes on x64");
#endif

#if defined(_WIN32)
#if defined(TASKBAR_CONFIGURATOR_BUILD)
#define TASKBAR_CONFIGURATOR_DIAGNOSTICS_API __declspec(dllexport)
#else
#define TASKBAR_CONFIGURATOR_DIAGNOSTICS_API __declspec(dllimport)
#endif
extern "C" TASKBAR_CONFIGURATOR_DIAGNOSTICS_API MwTaskbarConfiguratorDiagnostics
    MwTaskbarConfiguratorDiagnosticsBlock;
#undef TASKBAR_CONFIGURATOR_DIAGNOSTICS_API
#endif

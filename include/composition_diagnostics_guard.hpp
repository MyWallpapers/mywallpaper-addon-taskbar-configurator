#pragma once
#include "mw_native_sdk.h"

namespace taskbar_diagnostics {
// Process-local compatibility for the XAML observer's initial enumeration.
// This does not write registry settings or disable rendering/composition.
MwStatus InstallCompositionGuard(const MwHostApi* host) noexcept;
MwStatus RemoveCompositionGuard() noexcept;

class CompositionEnumerationScope final {
public:
    CompositionEnumerationScope() noexcept;
    ~CompositionEnumerationScope();
    CompositionEnumerationScope(const CompositionEnumerationScope&) = delete;
    CompositionEnumerationScope& operator=(const CompositionEnumerationScope&) = delete;
private:
    bool previous_active_;
    void* previous_key_;
};
}

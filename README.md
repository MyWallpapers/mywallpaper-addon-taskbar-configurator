# MyWallpaper Taskbar Configurator

This native-only MyWallpaper add-on styles parts of the Windows 11 taskbar from the standard add-on settings in MyWallpaper's sidebar. It does not create a separate window or web page. Its companion is supervised by MyWallpaper's native engine, which loads the add-on DLL into the matching Explorer taskbar process.

Engine ABI v1 permits one active instance per target. Device-scoped settings do not create a shared principal; activate this prototype in only one layer.

The add-on currently provides 39 settings: five palette profiles, an enable switch, direct controls for the surface, taskbar buttons, Start button, system tray and indicators, plus an advanced JSON profile. All persisted settings use device scope. The profile selector changes the palette and shape defaults; **Neutral dark** is a fixed palette and does not follow the current Windows theme. Choose **Custom** to edit the corresponding colors and surface geometry. Motion controls remain directly adjustable in every profile.

Direct styling uses the XAML visual tree exposed by the Windows XAML diagnostics interface. The add-on targets taskbar elements it recognizes by semantic names and types. For other elements, the advanced profile selects XAML elements by semantic, exact `name`, `type` substring, or exact `ancestorName` and applies the properties that exist on that element. A valid rule can match nothing on a particular Windows build; the add-on does not promise a stable private XAML tree across Windows updates.

Advanced settings accept an optional JSON object:

```json
{
  "schemaVersion": 1,
  "rules": [
    {
      "selector": { "name": "TaskbarFrame" },
      "properties": { "opacity": 0.9, "margin": [0, -2] }
    }
  ],
  "animations": [
    {
      "selector": { "semantic": "button" },
      "hoverScale": 1.1,
      "pressScale": 0.94,
      "hoverOpacity": 1,
      "pressOpacity": 0.88,
      "hoverDurationMs": 180,
      "pressDurationMs": 90,
      "easing": "ease-out"
    }
  ]
}
```

The rule property names are `background`, `foreground`, `fill`, `stroke`, `opacity`, `cornerRadius`, `strokeThickness`, `width`, `height`, `minWidth`, `maxWidth`, `minHeight`, `maxHeight`, `fontSize`, `rotation`, `margin`, `padding`, `spacing`, `visibility`, `horizontalAlignment`, `verticalAlignment`, `isEnabled`, `isHitTestVisible`, and `text`. Brush colors use `#RRGGBB` or MyWallpaper's alpha-last `#RRGGBBAA`; `transparent` is also accepted. `visibility` accepts `visible` or `collapsed`. Margin may use negative values. Properties that do not apply to the selected XAML type are ignored. `spacing` is expressed through element margins because XAML has no general spacing property. At most 256 rules and 128 animation selectors are accepted to bound repeated visual-tree application work. When multiple animation entries match, the last entry wins.

Property overrides are temporary. The add-on records the original local XAML value and restores it on stop or when an element leaves the taskbar tree. It skips properties that already have bindings, and it removes its property callbacks and pointer event handlers during cleanup. Pointer hover and press motion uses Windows Composition animations. The add-on does not replace Windows taskbar behavior, taskbar auto-hide, app grouping, notification logic, or Explorer's private animation/state machines.

Each visual-tree observer owns the XAML diagnostics cache that supplied its notifications. If Windows has already released a reported element, resolving its handle can return `ERROR_NOT_FOUND` (`0x80070490`); the add-on skips that stale notification and continues with live elements. Other HRESULT failures remain reported in its diagnostics. This changes neither the engine API nor the 72-byte diagnostics block.

XAML Diagnostics connection and cleanup COM calls run on retained worker threads. Startup waits up to five seconds; if COM does not return in time, the add-on disables itself and reports a pending drain. Stop remains retryable and does not wait indefinitely on the Explorer lifecycle callback. UI-dispatched restoration also has a bounded wait. If it times out, the same pending operation is retained; the next Stop waits on that operation again and reports pending again if it still exceeds the bound. It does not read the visual-tree registry or release cleanup state while that operation is outstanding. Start remains blocked until it has finished and been reaped. A COM call that never returns leaves the add-on in a draining state until Explorer exits, so it cannot be activated again in that process. This preserves module and COM object lifetimes without attempting unsafe thread cancellation.

## Build

Use Windows 11, Visual Studio 2022 C++ x64 build tools, CMake, Rustup, and Git. The build script installs the pinned Rust 1.94.1 MSVC toolchain if it is missing, checks out the engine revision recorded in `native/dependencies.lock.json`, builds the DLLs and companion, runs the settings parser tests, and writes a release-ready runtime bundle under `native/out/windows-x86_64/`. To use a separate engine checkout, set `MW_NATIVE_ENGINE_ROOT`; release builds require the exact clean pinned commit. `MW_ALLOW_UNPINNED_ENGINE=1` exists only for local integration testing against an in-progress sibling checkout.

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File native/build.ps1
```

The output contains `companion.exe`, `mw-native-agent.dll`, `addon.dll`, the engine descriptor, this add-on's license and the engine-generated third-party notices under `native/out/windows-x86_64/`. Each add-on bundle is self-contained; the engine reuses an already loaded agent only when its remote file hash matches exactly. Build intermediates, PDBs, import libraries, and source files are not copied into the runtime bundle.

## Verification boundary

The portable parser tests verify manifest-value interpretation, profile overrides, alpha-last color validation, range checks, advanced selectors and animation parsing. Windows CTest also checks ownership of the real XAML diagnostics callback's BSTR inputs, observer cache identity and lifetime, recovery after an expired handle, preservation of other HRESULT failures, partial event-subscription rollback and retry, a simulated UI-cleanup timeout followed by late completion, and stale settings-generation handling. The timeout test reuses the production completion slot with a deterministic scheduler; it is not an Explorer runtime qualification. This repository's build does not inject into or restart Explorer.

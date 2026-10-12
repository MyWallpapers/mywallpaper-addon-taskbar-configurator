# MyWallpaper Taskbar Configurator

This native-only MyWallpaper add-on styles parts of the Windows 11 taskbar from the standard add-on settings in MyWallpaper's sidebar. It does not create a separate window or web page. Its companion is supervised by MyWallpaper's native engine, which loads the add-on DLL into the matching Explorer taskbar process.

Engine ABI v1 permits one active instance per target. Device-scoped settings do not create a shared principal; activate this prototype in only one layer.

The add-on provides 49 settings, grouped by layout, surface, buttons, motion, Start, system tray, indicators and advanced rules. MyWallpaper supplies the settings editor and search. Five editable presets (macOS-inspired dock, compact dark, glass, rose and mint) apply ordinary settings and then leave every value adjustable. The glass preset uses alpha colors; it does not add a new blur renderer.

All persisted settings use device scope: compatible instances of this same add-on share one profile for the current account, source and settings-schema version. Distinct add-ons targeting Explorer have independent profiles. Sharing settings does not merge native sessions or arbitrate competing XAML property writes.

New configurations default to **Custom**. The legacy palette selector preserves older profiles; its fixed palette controls are hidden until Custom is selected. **Neutral dark** remains a fixed dark palette. For an editable palette, apply one of the presets. Version 0.1.7 uses device settings schema 2: MyWallpaper offers an explicit review/copy of compatible schema-1 values, while releases still using schema 1 keep their own profile.

Layout controls change content inside the existing taskbar window: Windows, content-fit or fixed width; left/center/right alignment; height, horizontal/vertical inset and offset; button width/height and rotation in degrees; indicator visibility. Dimensions use XAML logical pixels (DIPs). Zero height or button size keeps the native dimension. Work-area reservation, other screen edges, flyout positioning and content outside that window remain Windows responsibilities. A dock-like style can still include native space reserved for widgets and the tray, and large dimensions or motion can be clipped by the taskbar window.

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

The rule property names are `background`, `foreground`, `fill`, `stroke`, `opacity`, `cornerRadius`, `strokeThickness`, `width`, `height`, `minWidth`, `maxWidth`, `minHeight`, `maxHeight`, `fontSize`, `rotation`, `margin`, `padding`, `spacing`, `visibility`, `horizontalAlignment`, `verticalAlignment`, `isEnabled`, `isHitTestVisible`, and `text`. Brush colors use `#RRGGBB` or MyWallpaper's alpha-last `#RRGGBBAA`; `transparent` is also accepted. `visibility` accepts `visible` or `collapsed`. `width` and `height` also accept `"auto"`. Rotation uses degrees. Margin may use negative values. Properties that do not apply to the selected XAML type are ignored. `spacing` is expressed through element margins because XAML has no general spacing property. At most 256 rules and 128 animation selectors are accepted to bound repeated visual-tree application work. When multiple animation entries match, the last entry wins. The editor keeps text as a local draft until Apply, so partial JSON does not reach Explorer on every keystroke.

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

The portable parser tests verify manifest defaults and every editable preset, legacy profile overrides, alpha-last color validation, geometry range checks, advanced selectors and animation parsing. Windows CTest also checks ownership of the real XAML diagnostics callback's BSTR inputs, observer cache identity and lifetime, recovery after an expired handle, preservation of other HRESULT failures, partial event-subscription rollback and retry, a simulated UI-cleanup timeout followed by late completion, and stale settings-generation handling. An isolated Composition test verifies degree interpretation, restoration of an original radians value, release of the visual reference and XAML automatic width encoding. The timeout test reuses the production completion slot with a deterministic scheduler; it is not an Explorer runtime qualification. This repository's build does not inject into or restart Explorer.

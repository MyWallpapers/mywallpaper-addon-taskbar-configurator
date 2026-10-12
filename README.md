# MyWallpaper Taskbar Configurator

This native-only MyWallpaper add-on configures the existing Windows 11 taskbar from MyWallpaper's add-on settings sidebar. It has no separate window or web page. Its companion uses the pinned MyWallpaper native engine to load the add-on DLL into the matching Explorer process. Visual behavior and configuration remain in this repository; the engine ABI and implementation are unchanged by version 0.1.8.

Engine ABI v1 permits one active instance per target. Activate this prototype in only one layer. Device profiles are isolated by account, published add-on or development source, and settings-schema version. Compatible instances of this same add-on share their values; distinct add-ons targeting Explorer retain independent profiles. Shared values do not merge native sessions or arbitrate competing property writes.

## Configuration without themes

Version 0.1.8 retains the 49 setting IDs and device settings schema **2**, and removes its manifest presets. Existing saved values take precedence over the legacy palette selector. That selector, under Advanced, only fills omitted values in old partial configurations; it no longer locks the editable colors. This intentionally corrects older palette override behavior without discarding stored values. Advanced profile schemas 1 and 2 are both accepted.

The normal controls cover surface and button colors, opacity, rounding, dimensions, insets, position, activity indicators, Start, system tray and pointer animation parameters. Layout changes the content inside the existing taskbar window. Work-area reservation, other screen edges, auto-hide, flyout position, grouping and notification behavior remain Windows responsibilities. XAML dimensions use logical pixels (DIPs); large transformations can be clipped by the native taskbar window.

The optional `advancedProfileJson` field offers **guided fields and raw JSON editing of the same string** in MyWallpaper versions supporting `editor` manifest metadata. Changes stay in a draft until Apply; Discard cancels them. Empty text removes custom rules while retaining the other settings. Older compatible text editors can still edit the JSON directly. The schema belongs to the add-on; MyWallpaper supplies generic controls and the existing persistence path.

### Rules and values

The editor exposes 55 native property names and accepts other named properties or an explicit XAML fragment. A property must actually exist on the selected Windows XAML class. The implementation resolves its dependency property and typed value through Windows XAML, rather than maintaining a separate taskbar property catalog in the engine. Unsupported advanced properties report a diagnostic; ordinary semantic controls skip inapplicable element types.

Selectors combine a semantic target, exact element name, type substring or exact class, named ancestor, direct parent chain, visual root, one-based child index and primitive dependency-property filters. Semantic targets include surface, application buttons, Start, search, widgets, tray, clock, indicator, icon and label. Private names, classes, visual-state groups and child order can change between Windows versions; a valid selector can match nothing.

Supported value families include:

- Solid colors, linear gradients, image brushes, native acrylic, and a custom Composition backdrop blur with tint, saturation, luminosity multiplier and optional procedural grain.
- Margins, padding, corner radii, sizing and alignment; borders, typography and native attached properties such as `Grid.Column` and `Canvas.ZIndex`.
- Composite 2D/3D transforms, local `PlaneProjection`, shared `PerspectiveTransform3D`, transform origins and native brush transitions.
- Grid row and column collections with `Auto`, fixed DIP sizes and star sizes, restored with their original definition objects on cleanup.
- Shared values, explicit light/dark resources, Windows theme resources, captured dependency-property values, bounded numeric expressions and `null` to clear a local override.
- Existing Windows visual-state groups and states, and pointer hover/press Composition animations with configurable scales, opacity, timing and easing.

Colors in typed JSON use `#RRGGBB` or MyWallpaper's alpha-last `#RRGGBBAA`; `transparent` is accepted. Explicit raw XAML follows native XAML syntax, including its alpha-first color notation. Plain text is not color-rewritten. Read-only `ActualWidth` and `ActualHeight` are available for capture, not assignment. An expression supports arithmetic, comparisons, ternaries, `min`, `max`, variables and `skip()`; it is not a general-purpose script language. Exponents, logical operators and string concatenation are not supported. Avoid feedback loops such as continuously deriving an element's width from its own measured width.

```json
{
  "schemaVersion": 2,
  "variables": { "gap": 16 },
  "captures": [
    { "name": "barWidth", "selector": { "name": "TaskbarFrame" }, "property": "ActualWidth" }
  ],
  "rules": [
    {
      "selector": { "name": "BackgroundFill", "type": "Rectangle" },
      "properties": {
        "Fill": {
          "kind": "blur", "color": "#FF69B480", "radius": 18,
          "tintOpacity": 0.35, "saturation": 1.2, "luminosity": 1,
          "noiseOpacity": 0.02, "fallbackColor": "#7A1746FF"
        },
        "RadiusX": 16, "RadiusY": 16
      }
    },
    {
      "selector": { "type": "TaskListButton" },
      "properties": { "Width": { "expression": "max(40, min(60, barWidth / 100))" } }
    },
    {
      "selector": { "type": "TaskListButton" },
      "stateGroup": "CommonStates", "state": "PointerOver",
      "properties": { "Opacity": 0.8 }
    }
  ],
  "animations": [
    { "selector": { "semantic": "button" }, "hoverScale": 1.12, "pressScale": 0.96,
      "hoverDurationMs": 180, "pressDurationMs": 90, "easing": "ease-out" }
  ]
}
```

This example uses private taskbar names and must be checked on the target Windows build. Resources declared as `{ "light": ..., "dark": ... }` are re-evaluated on theme changes. The custom blur uses the target element's compositor, keeps a bounded per-thread value cache, builds its effect graph when connected, and closes its Composition brush and image surface on disconnect. Grain is generated locally; no texture download or Win2D dependency is needed. If advanced effects are disabled, battery saver is active at connection time, or graph creation fails, it uses a solid fallback. Live changes to those Windows preferences are not subscribed to in this prototype.

### Restoration and limits

Overrides record the original local XAML value and restore it on rule removal, stop or removal from the taskbar tree. Existing bindings are preserved instead of replaced. Windows changes to an overridden property's original value are tracked. State, capture and theme subscriptions are revoked with the existing retryable cleanup lifecycle; updates are coalesced on the UI dispatcher, without per-frame layout polling. Failed unregisters retain their cleanup state for retry.

The profile is bounded at 1 MiB, 256 rules, 128 animation selectors, 128 captures and 128 shared values/resources. Expressions are limited to 4096 characters and depth 32; reference expansion is limited to depth 16. Resolver caches hold at most 1024 entries and clear during profile changes and cleanup. These bounds constrain work, not what a full-trust native add-on may do outside this profile implementation.

The current observer handles the primary taskbar UI thread. Secondary taskbars on other UI threads are not qualified. Visual-state rules use existing Windows groups; they do not create new taskbar behavior or replace Explorer's animation/state machines. XAML hit testing does not make the surrounding HWND click-through. Moving the taskbar to another screen edge, changing its reserved desktop work area or implementing an arbitrary shell would require separate Windows integration and testing. Typed 3D transformations do not establish compatibility with every taskbar template or graphics driver.

Expired diagnostics handles can return `ERROR_NOT_FOUND` (`0x80070490`); these stale notifications are skipped. Other HRESULT failures remain visible in diagnostics. XAML diagnostics connect/cleanup COM calls run on retained workers with bounded waits. A call that never returns leaves the add-on draining until Explorer exits, rather than cancelling a thread unsafely or releasing referenced modules. Start is blocked until the pending cleanup completes. The existing 72-byte diagnostics block is unchanged; a separate add-on-local 40-byte effect counter block supports test measurements without changing the engine contract.

## Build

Use Windows 11, Visual Studio 2022 C++ x64 build tools, CMake, Rustup and Git:

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File native/build.ps1
```

The script installs Rust 1.94.1 if needed, verifies the clean engine commit in `native/dependencies.lock.json`, builds the add-on and companion, and runs four CTest suites. The runtime bundle under `native/out/windows-x86_64/` contains the companion, agent, add-on DLL, engine descriptor, license and third-party notices. Intermediate PDBs, libraries and source are not copied into it. The engine reuses an already resident agent only when its file hash matches exactly.

Set `MW_NATIVE_ENGINE_ROOT` to use a separate exact clean pinned engine checkout. `MW_ALLOW_UNPINNED_ENGINE=1` is only for local integration against in-progress source. Regenerate the add-on-owned settings metadata with `node scripts/update-editor.mjs`; no dependency installation is required for that generator.

## Verification boundary

Parser/manifest tests cover retained setting IDs, explicit values over legacy palette fallbacks, schema 1 compatibility, selector and material validation, expressions, nulls and read-only property rejection. Windows tests additionally cover real XAML diagnostics input/cache lifetimes, expired handles, rollback/retry and queued lifecycle generations; isolated Composition degree interpretation and restoration; and a real XAML host's typed brushes, grid identity restoration, 3D/projection values, captures and subscription revocation. The custom blur graph is connected and disconnected using that host's compositor.

These automated suites do not inject into Explorer. Runtime qualification is a separate local experiment recording DLL/agent hashes, the exact Explorer identity, effect counters, repeated reconfiguration, stop/resume cleanup and process memory. Its evidence applies only to the measured Windows build and test interval; source coverage does not establish parity with every Windhawk theme or universal Windows compatibility. There is no claim of hour-long memory behavior from a short run or of independent GPU leak detection from shared allocation counters.

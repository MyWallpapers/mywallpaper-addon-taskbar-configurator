$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$lockPath = Join-Path $PSScriptRoot 'dependencies.lock.json'
$lock = Get-Content -LiteralPath $lockPath -Raw | ConvertFrom-Json
$enginePin = [string]$lock.engine.revision
$allowUnpinned = $env:MW_ALLOW_UNPINNED_ENGINE -eq '1'
$engineRoot = $env:MW_NATIVE_ENGINE_ROOT
if ([string]::IsNullOrWhiteSpace($engineRoot)) {
    $engineRoot = Join-Path (Split-Path $repoRoot -Parent) 'mywallpaper-native-engine'
}
$engineRoot = [System.IO.Path]::GetFullPath($engineRoot)

function Convert-ToGitSafePath([string]$Path) {
    $fullPath = [System.IO.Path]::GetFullPath($Path)
    if ($fullPath.StartsWith('M:\', [System.StringComparison]::OrdinalIgnoreCase)) {
        return '//wsl.localhost/Ubuntu-26.04/' + $fullPath.Substring(3).Replace('\', '/')
    }
    return $fullPath.Replace('\', '/')
}

function Set-BuildGitSafePaths([string]$EnginePath) {
    # FetchContent invokes git itself. Scope safe.directory to the exact source
    # checkout paths used by this build process; never persist a global setting.
    $engineGitSafePath = Convert-ToGitSafePath $EnginePath
    $minhookGitSafePath = Convert-ToGitSafePath (Join-Path $repoRoot '.mywallpaper/native-build/windows-x86_64/_deps/mw_min_hook_source-src')
    $env:GIT_CONFIG_COUNT = '2'
    $env:GIT_CONFIG_KEY_0 = 'safe.directory'
    $env:GIT_CONFIG_VALUE_0 = $engineGitSafePath
    $env:GIT_CONFIG_KEY_1 = 'safe.directory'
    $env:GIT_CONFIG_VALUE_1 = $minhookGitSafePath
}

Set-BuildGitSafePaths $engineRoot

if (-not (Test-Path -LiteralPath (Join-Path $engineRoot 'CMakeLists.txt'))) {
    if (-not $enginePin -or $enginePin -eq 'PENDING_ENGINE_COMMIT') {
        throw 'The engine dependency has no pinned revision yet. Set MW_NATIVE_ENGINE_ROOT to its checkout and MW_ALLOW_UNPINNED_ENGINE=1 only for local development.'
    }
    $cacheRoot = Join-Path $repoRoot '.mywallpaper/native-engine-cache'
    New-Item -ItemType Directory -Force -Path $cacheRoot | Out-Null
    $engineRoot = Join-Path $cacheRoot $enginePin
    Set-BuildGitSafePaths $engineRoot
    if (-not (Test-Path -LiteralPath (Join-Path $engineRoot 'CMakeLists.txt'))) {
        & git clone --filter=blob:none --no-checkout -- $lock.engine.repository $engineRoot
        if ($LASTEXITCODE -ne 0) { throw "git clone failed with exit code $LASTEXITCODE" }
        & git -C $engineRoot checkout --detach $enginePin
        if ($LASTEXITCODE -ne 0) { throw "checking out native engine revision $enginePin failed" }
    }
}

# Recompute after the optional cache checkout so git and FetchContent use the
# actual engine path selected for this build.
$engineGitSafePath = Convert-ToGitSafePath $engineRoot
Set-BuildGitSafePaths $engineRoot

if ($enginePin -eq 'PENDING_ENGINE_COMMIT') {
    if (-not $allowUnpinned) { throw 'Pin the native engine commit in native/dependencies.lock.json before a release build.' }
    Write-Warning 'Building against an unpinned local engine checkout for development.'
} else {
    $actualEngineRevision = (& git -c "safe.directory=$engineGitSafePath" -C $engineRoot rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0 -or $actualEngineRevision -ne $enginePin) {
        throw "Native engine revision mismatch. Expected $enginePin, found $actualEngineRevision."
    }
    $engineChanges = & git -c "safe.directory=$engineGitSafePath" -C $engineRoot status --porcelain
    if ($LASTEXITCODE -ne 0) { throw 'Could not inspect the native engine checkout.' }
    if ($engineChanges -and -not $allowUnpinned) {
        throw 'The pinned native engine checkout has local modifications; use a clean checkout for a release build.'
    }
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'vswhere.exe was not found; install Visual Studio 2022 C++ build tools.' }
$visualStudio = (& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($visualStudio)) {
    throw 'Visual Studio 2022 x64 C++ tools were not found.'
}
$cmake = Join-Path $visualStudio 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
if (-not (Test-Path -LiteralPath $cmake)) { throw "CMake bundled with Visual Studio was not found at $cmake" }

$buildRoot = Join-Path $repoRoot '.mywallpaper/native-build/windows-x86_64'
New-Item -ItemType Directory -Force -Path $buildRoot | Out-Null
$configureArgs = @(
    '-S', $repoRoot,
    '-B', $buildRoot,
    '-G', 'Visual Studio 17 2022',
    '-A', 'x64',
    "-DMW_NATIVE_ENGINE_SOURCE_DIR=$engineRoot"
)
if ($env:MW_MINHOOK_SOURCE) { $configureArgs += "-DMW_MINHOOK_SOURCE=$env:MW_MINHOOK_SOURCE" }
& $cmake @configureArgs
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed with exit code $LASTEXITCODE" }

& $cmake --build $buildRoot --config Release --target taskbar_configurator mw_native_agent taskbar_config_tests
if ($LASTEXITCODE -ne 0) { throw "CMake build failed with exit code $LASTEXITCODE" }
& $cmake --build $buildRoot --config Release --target RUN_TESTS
if ($LASTEXITCODE -ne 0) { throw "CMake tests failed with exit code $LASTEXITCODE" }

$rustup = (Get-Command rustup.exe -ErrorAction SilentlyContinue).Source
if (-not $rustup) { throw 'rustup.exe was not found; install Rustup to build the companion.' }
$rustToolchain = '1.94.1-x86_64-pc-windows-msvc'
$installedRustToolchains = @(& $rustup toolchain list)
if ($LASTEXITCODE -ne 0) { throw 'Could not inspect the installed Rust toolchains.' }
$rustToolchainPattern = '^' + [regex]::Escape($rustToolchain) + '(?:\s|$)'
if (-not ($installedRustToolchains | Where-Object { $_ -match $rustToolchainPattern })) {
    Write-Host "Installing pinned Rust toolchain $rustToolchain."
    & $rustup toolchain install $rustToolchain --profile minimal --no-self-update
    if ($LASTEXITCODE -ne 0) { throw "Installing pinned Rust toolchain $rustToolchain failed." }
}
$cargoTarget = Join-Path $buildRoot 'cargo-target'
$previousRustFlags = $env:RUSTFLAGS
$previousRustupToolchain = $env:RUSTUP_TOOLCHAIN
try {
    $engineRemapPath = $engineRoot.Replace('\', '/')
    $env:RUSTFLAGS = "--remap-path-prefix=$engineRemapPath=. -C link-arg=/Brepro"
    $env:RUSTUP_TOOLCHAIN = $rustToolchain
    & $rustup run $rustToolchain cargo build --release --locked --target x86_64-pc-windows-msvc `
        --manifest-path (Join-Path $engineRoot 'controller/Cargo.toml') `
        --target-dir $cargoTarget
    if ($LASTEXITCODE -ne 0) { throw "Cargo build failed with exit code $LASTEXITCODE" }
} finally {
    if ($null -eq $previousRustFlags) { Remove-Item Env:RUSTFLAGS -ErrorAction SilentlyContinue }
    else { $env:RUSTFLAGS = $previousRustFlags }
    if ($null -eq $previousRustupToolchain) { Remove-Item Env:RUSTUP_TOOLCHAIN -ErrorAction SilentlyContinue }
    else { $env:RUSTUP_TOOLCHAIN = $previousRustupToolchain }
}

$outputRoot = [System.IO.Path]::GetFullPath((Join-Path $repoRoot 'native/out/windows-x86_64'))
$expectedRoot = [System.IO.Path]::GetFullPath((Join-Path $repoRoot 'native/out/windows-x86_64'))
if ($outputRoot -ne $expectedRoot -or -not $outputRoot.StartsWith($repoRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to clean an output directory outside this add-on repository: $outputRoot"
}
if (Test-Path -LiteralPath $outputRoot) {
    $outputItem = Get-Item -LiteralPath $outputRoot -Force
    if (($outputItem.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "Refusing to clean a reparse-point output directory: $outputRoot"
    }
    Remove-Item -LiteralPath $outputRoot -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $outputRoot 'notices') | Out-Null

$builtBin = Join-Path $buildRoot 'bin'
$addonDll = Join-Path $buildRoot 'Release/addon.dll'
$agentDll = Join-Path $builtBin 'mw-native-agent.dll'
$companionExe = Join-Path $cargoTarget 'x86_64-pc-windows-msvc/release/companion.exe'
foreach ($required in @($addonDll, $agentDll, $companionExe,
    (Join-Path $builtBin 'THIRD_PARTY_NOTICES.md'))) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Expected build output is missing: $required" }
}

Copy-Item -LiteralPath $addonDll -Destination (Join-Path $outputRoot 'addon.dll')
Copy-Item -LiteralPath $companionExe -Destination (Join-Path $outputRoot 'companion.exe')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'mywallpaper-native-engine.json') -Destination $outputRoot
Copy-Item -LiteralPath (Join-Path $repoRoot 'LICENSE') -Destination $outputRoot
Copy-Item -LiteralPath (Join-Path $builtBin 'THIRD_PARTY_NOTICES.md') -Destination $outputRoot
Copy-Item -LiteralPath $agentDll -Destination (Join-Path $outputRoot 'mw-native-agent.dll')

$builtNotices = Join-Path $builtBin 'notices'
if (-not (Test-Path -LiteralPath $builtNotices -PathType Container)) {
    throw "Third-party notices directory is missing: $builtNotices"
}
$noticeFiles = @(Get-ChildItem -LiteralPath $builtNotices -File -Recurse)
if ($noticeFiles.Count -eq 0) { throw 'No third-party notice files were produced.' }
Copy-Item -Path (Join-Path $builtNotices '*') -Destination (Join-Path $outputRoot 'notices') -Recurse -Force

Write-Host "Taskbar Configurator bundle built at $outputRoot"

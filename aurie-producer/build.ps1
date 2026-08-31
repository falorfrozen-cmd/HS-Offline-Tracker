param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",
    [string]$YytkSdkRoot = ""
)

$ErrorActionPreference = "Stop"
$sourceRoot = Split-Path -Parent $MyInvocation.MyCommand.Path

function Resolve-CMakeExecutable {
    $onPath = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if ($null -ne $onPath) {
        return $onPath.Source
    }

    $visualStudioRoots = Get-ChildItem `
        -LiteralPath "C:\Program Files\Microsoft Visual Studio" `
        -Directory `
        -ErrorAction SilentlyContinue
    foreach ($versionRoot in $visualStudioRoots) {
        foreach ($editionRoot in Get-ChildItem -LiteralPath $versionRoot.FullName -Directory -ErrorAction SilentlyContinue) {
            $candidate = Join-Path $editionRoot.FullName `
                "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
            if (Test-Path -LiteralPath $candidate) {
                return $candidate
            }
        }
    }

    $standalone = "C:\Program Files\CMake\bin\cmake.exe"
    if (Test-Path -LiteralPath $standalone) {
        return $standalone
    }

    throw "CMake was not found on PATH or in a Visual Studio installation."
}

$cmake = Resolve-CMakeExecutable
$ctest = Join-Path (Split-Path -Parent $cmake) "ctest.exe"
if (-not (Test-Path -LiteralPath $ctest)) {
    throw "CTest was not found next to CMake: $ctest"
}
if ([string]::IsNullOrWhiteSpace($YytkSdkRoot)) {
    $YytkSdkRoot = [System.IO.Path]::GetFullPath(
        (Join-Path $sourceRoot "..\..\ForgePact\plugin_build"))
}
$buildRoot = Join-Path $sourceRoot "build"

& $cmake -S $sourceRoot -B $buildRoot -A x64 `
    "-DYYTK_SDK_ROOT=$YytkSdkRoot" `
    "-DBUILD_TESTING=ON"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $cmake --build $buildRoot --config $Configuration
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $ctest --test-dir $buildRoot -C $Configuration --output-on-failure
exit $LASTEXITCODE

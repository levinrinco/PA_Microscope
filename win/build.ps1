[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$QtRoot,
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')][string]$Configuration = 'Release',
    [ValidatePattern('^Visual Studio ')][string]$Generator = 'Visual Studio 17 2022',
    [switch]$TestsOnly,
    [switch]$Deploy
)

$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT') { throw 'Run this script on Windows x64.' }
if ($TestsOnly -and $Deploy) { throw '-Deploy requires the application, not -TestsOnly.' }
$Repo = Split-Path -Parent $PSScriptRoot
$QtRoot = (Resolve-Path -LiteralPath $QtRoot).Path
$QtBin = Join-Path $QtRoot 'bin'
if (!(Test-Path (Join-Path $QtRoot 'lib/cmake/Qt6/Qt6Config.cmake'))) {
    throw 'QtRoot must point to a Qt 6 MSVC x64 kit, e.g. C:\Qt\6.8.3\msvc2022_64.'
}
foreach ($tool in @('cmake', 'ctest')) {
    if (!(Get-Command $tool -ErrorAction SilentlyContinue)) { throw "Missing tool: $tool" }
}
if (!$TestsOnly) {
    foreach ($name in @('spcm_win64_msvcpp.lib', 'spcm_win64.dll')) {
        if (!(Test-Path (Join-Path $PSScriptRoot "lib/$name"))) {
            throw "Missing win/lib/$name. See win/README.md or run with -TestsOnly."
        }
    }
}
$BuildPath = if ($TestsOnly) { Join-Path $Repo 'win/build-tests' } else { Join-Path $Repo 'win/build' }
$BuildApp = if ($TestsOnly) { 'OFF' } else { 'ON' }
$OldPath = $env:PATH
$OldPluginPath = $env:QT_PLUGIN_PATH
try {
    $env:PATH = "$QtBin;$OldPath"
    $env:QT_PLUGIN_PATH = Join-Path $QtRoot 'plugins'
    & cmake -S $Repo -B $BuildPath -G $Generator -A x64 `
        "-DCMAKE_PREFIX_PATH=$QtRoot" "-DPA_BUILD_APP=$BuildApp" '-DBUILD_TESTING=ON'
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
    & cmake --build $BuildPath --config $Configuration --parallel
    if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
    & ctest --test-dir $BuildPath -C $Configuration --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Offline tests failed.' }
    $BinPath = Join-Path $BuildPath "bin/$Configuration"
    Write-Host "Build and offline tests passed: $BinPath"

    if ($Deploy) {
        $DeployTool = Join-Path $QtBin 'windeployqt.exe'
        if (!(Test-Path $DeployTool)) { throw "Missing $DeployTool" }
        # Use a new directory so an earlier deployment cannot leave stale DLLs.
        $Stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
        $DeployPath = Join-Path $Repo "win/dist/windows-$Configuration-$Stamp"
        New-Item -ItemType Directory -Path $DeployPath | Out-Null
        foreach ($name in @('PA_Microscope.exe', 'spcm_win64.dll')) {
            Copy-Item -LiteralPath (Join-Path $BinPath $name) -Destination $DeployPath
        }
        $DeployMode = if ($Configuration -eq 'Debug') { '--debug' } else { '--release' }
        & $DeployTool $DeployMode --no-compiler-runtime --dir $DeployPath `
            (Join-Path $DeployPath 'PA_Microscope.exe')
        if ($LASTEXITCODE -ne 0) { throw 'Qt deployment failed.' }
        Write-Host "Deployment folder: $DeployPath"
        Write-Host 'Target PC also needs the official VC++ x64 runtime, Spectrum driver and GPU driver.'
    } elseif (!$TestsOnly) {
        Write-Host 'Use -Deploy to collect Qt DLLs/plugins into a standalone application folder.'
    }
} finally {
    $env:PATH = $OldPath
    $env:QT_PLUGIN_PATH = $OldPluginPath
}

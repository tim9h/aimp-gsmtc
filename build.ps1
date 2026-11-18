param(
    [ValidateSet('all', 'plugin', 'probe')][string]$Target = 'all',
    [string]$Configuration = 'Release'
)
# Builds the plugin DLL (and the standalone SMTC probe) with MinGW-w64 g++ (x64).
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$out = Join-Path $root 'build'
New-Item -ItemType Directory -Force $out | Out-Null

$sdk = Join-Path $root 'aimp_sdk\Sources\Cpp'
$common = @('-std=c++17', '-municode', '-DUNICODE', '-D_UNICODE', '-DWINVER=0x0A00', '-D_WIN32_WINNT=0x0A00',
    '-Wall', '-Wno-unknown-pragmas', '-Wno-missing-braces', '-Wno-overloaded-virtual', '-fno-exceptions', '-fno-rtti',
    '-I', (Join-Path $root 'src'), '-I', $sdk)
if ($Configuration -eq 'Release') { $common += @('-O2', '-DNDEBUG') } else { $common += @('-O0', '-g') }
$static = @('-static', '-static-libgcc', '-static-libstdc++')

function Invoke-Gpp([string[]]$gppArgs)
{
    & g++ @gppArgs
    if ($LASTEXITCODE -ne 0) { throw "g++ failed ($LASTEXITCODE)" }
}

if ($Target -in 'all', 'plugin')
{
    $dll = Join-Path $out 'aimp_mediacontrol.dll'
    Invoke-Gpp (@('-shared') + $common + @(
            (Join-Path $root 'src\plugin.cpp'),
            (Join-Path $root 'src\smtc_publisher.cpp'),
            '-o', $dll) + $static + @('-luser32', '-lole32', '-luuid'))
    Write-Host "Built $dll"
}

if ($Target -in 'all', 'probe')
{
    $exe = Join-Path $out 'smtc_probe.exe'
    Invoke-Gpp ($common + @(
            (Join-Path $root 'tests\smtc_probe\probe.cpp'),
            (Join-Path $root 'src\smtc_publisher.cpp'),
            '-o', $exe) + $static + @('-luser32', '-lole32', '-luuid'))
    Write-Host "Built $exe"

    # Mock AIMP host that loads the plugin DLL (tests only).
    $mock = Join-Path $out 'mock_host.exe'
    Invoke-Gpp ($common + @((Join-Path $root 'tests\mock_host\host.cpp'), '-o', $mock) + $static + @('-luser32', '-lole32', '-luuid'))
    Write-Host "Built $mock"
}


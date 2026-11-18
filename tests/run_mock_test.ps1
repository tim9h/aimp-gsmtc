# End-to-end test: loads build\aimp_mediacontrol.dll into a mock AIMP host and checks what Windows GSMTC reports.
# Prerequisite: .\build.ps1 (builds the plugin and the mock host).
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$build = Join-Path $root 'build'
$cmdFile = Join-Path $build 'mock_commands.txt'
$logFile = Join-Path $build 'mock_host.log'
$cover = Join-Path $build 'cover.png'
$reader = Join-Path $PSScriptRoot 'gsmtc_reader.ps1'

if (-not (Test-Path $cover))
{
    Add-Type -AssemblyName System.Drawing
    $bmp = New-Object System.Drawing.Bitmap 300, 300
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.Clear([System.Drawing.Color]::OrangeRed)
    $g.Dispose()
    $bmp.Save($cover, [System.Drawing.Imaging.ImageFormat]::Png)
}

$failures = 0
function Check([string]$name, [bool]$ok)
{
    if ($ok) { Write-Host "PASS  $name" } else { Write-Host "FAIL  $name"; $script:failures++ }
}
function Read-Session([string]$cmd = '')
{
    $psArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $reader, '-Match', 'mock_host')
    if ($cmd) { $psArgs += @('-Cmd', $cmd) }
    (& powershell.exe @psArgs) -join "`n"
}
function Send([string]$line) { Add-Content -Path $cmdFile -Value $line }

Set-Content -Path $cmdFile -Value ''
Remove-Item $logFile -ErrorAction SilentlyContinue
$env:AIMP_GSMTC_LOG = Join-Path $build 'plugin.log'
Remove-Item $env:AIMP_GSMTC_LOG -ErrorAction SilentlyContinue
$host_ = Start-Process -FilePath (Join-Path $build 'mock_host.exe') -ArgumentList "`"$(Join-Path $build 'aimp_mediacontrol.dll')`"", "`"$cmdFile`"", "`"$cover`"" `
    -RedirectStandardOutput $logFile -PassThru -WindowStyle Hidden
try
{
    Start-Sleep -Seconds 2
    Check 'plugin initializes' ((Get-Content $logFile -Raw) -match 'INIT 00000000')

    Send 'track 1'; Send 'state play'
    Start-Sleep -Seconds 2
    $s = Read-Session
    Check 'title/artist/album published' ($s -match 'title=Mock Title 1 \| artist=Mock Artist 1 \| album=Mock Album \| albumArtist=Mock Album Artist')
    Check 'status Playing' ($s -match 'status=Playing')
    Check 'timeline end = duration' ($s -match 'end=181s')
    Check 'thumbnail published' ($s -match 'thumbnail=present\(opens\)')
    Check 'transport controls enabled' ($s -match 'play=True pause=True stop=True next=True prev=True seek=True' -or $s -match 'pause=True stop=True next=True prev=True seek=True')

    $r = Read-Session 'pause'
    Start-Sleep -Seconds 1
    $s = Read-Session
    Check 'pause button reaches AIMP' ((Get-Content $logFile -Raw) -match 'PLAYER Pause')
    Check 'status Paused after pause' ($s -match 'status=Paused')

    $r = Read-Session 'play'
    Start-Sleep -Seconds 1
    $s = Read-Session
    Check 'play button resumes AIMP' ((Get-Content $logFile -Raw) -match 'PLAYER Resume')
    Check 'status Playing after play' ($s -match 'status=Playing')

    $r = Read-Session 'next'
    Start-Sleep -Seconds 2
    $s = Read-Session
    Check 'next button reaches AIMP' ((Get-Content $logFile -Raw) -match 'CMD next')
    Check 'metadata follows next track' ($s -match 'title=Mock Title 2 ')
    Check 'thumbnail republished for new track' ($s -match 'thumbnail=present\(opens\)')

    $r = Read-Session 'prev'
    Start-Sleep -Seconds 2
    Check 'previous button reaches AIMP' ((Get-Content $logFile -Raw) -match 'CMD prev')

    $r = Read-Session 'seek:100'
    Start-Sleep -Seconds 1
    $s = Read-Session
    Check 'seek request reaches AIMP' ((Get-Content $logFile -Raw) -match 'PLAYER SetPosition 100\.0')
    Check 'timeline position after seek' ($s -match 'pos=100s')

    Send 'pos 30'
    Start-Sleep -Seconds 1
    Check 'user seek inside AIMP updates timeline' ((Read-Session) -match 'pos=30s')

    $r = Read-Session 'stop'
    Start-Sleep -Seconds 1
    Check 'stop button reaches AIMP' ((Get-Content $logFile -Raw) -match 'PLAYER Stop')
    Check 'status Stopped after stop' ((Read-Session) -match 'status=Stopped')

    Send 'quit'
    Start-Sleep -Seconds 2
    $log = Get-Content $logFile -Raw
    Check 'plugin unhooks and unloads cleanly' ($log -match 'UNHOOKED' -and $log -match 'UNLOADED')
    Check 'session removed after unload' (-not ((Read-Session) -match 'SESSION'))
}
finally
{
    if (-not $host_.HasExited) { Stop-Process -Id $host_.Id }
}
if (Test-Path $env:AIMP_GSMTC_LOG) { Write-Host '--- plugin log ---'; Get-Content $env:AIMP_GSMTC_LOG }
if ($failures) { Write-Host "$failures check(s) failed"; exit 1 }
Write-Host 'All checks passed'


# Dumps Windows GSMTC sessions (and optionally sends a transport command). Runs on Windows PowerShell 5.1.
# Usage: powershell.exe -File tests\gsmtc_reader.ps1 [-Match AIMP] [-Cmd play|pause|stop|next|prev|seek:<seconds>]
param([string]$Match = '', [string]$Cmd = '')

Add-Type -AssemblyName System.Runtime.WindowsRuntime
$asTask = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
        $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]
function Await($op, $type)
{
    $t = $asTask.MakeGenericMethod($type).Invoke($null, @($op))
    $t.Wait(-1) | Out-Null
    $t.Result
}

$null = [Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager, Windows.Media.Control, ContentType = WindowsRuntime]
$null = [Windows.Storage.Streams.IRandomAccessStreamWithContentType, Windows.Storage.Streams, ContentType = WindowsRuntime]
$mgr = Await ([Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager]::RequestAsync()) ([Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager])

$found = 0
foreach ($s in $mgr.GetSessions())
{
    if ($Match -and $s.SourceAppUserModelId -notlike "*$Match*") { continue }
    $found++
    "SESSION app=$($s.SourceAppUserModelId)"
    $p = Await ($s.TryGetMediaPropertiesAsync()) ([Windows.Media.Control.GlobalSystemMediaTransportControlsSessionMediaProperties])
    $pb = $s.GetPlaybackInfo()
    $tl = $s.GetTimelineProperties()
    # PowerShell cannot cast the WinRT stream to read it, so only check that a thumbnail is present and opens.
    $thumb = 'none'
    if ($p.Thumbnail)
    {
        try
        {
            $st = Await ($p.Thumbnail.OpenReadAsync()) ([Windows.Storage.Streams.IRandomAccessStreamWithContentType])
            $thumb = if ($st) { 'present(opens)' } else { 'present(open failed)' }
        }
        catch { $thumb = "present(open error: $($_.Exception.Message))" }
    }
    "  status=$($pb.PlaybackStatus) type=$($pb.PlaybackType)"
    "  title=$($p.Title) | artist=$($p.Artist) | album=$($p.AlbumTitle) | albumArtist=$($p.AlbumArtist)"
    "  timeline pos=$([math]::Round($tl.Position.TotalSeconds,1))s end=$([math]::Round($tl.EndTime.TotalSeconds,1))s"
    "  thumbnail=$thumb"
    $c = $pb.Controls
    "  controls play=$($c.IsPlayEnabled) pause=$($c.IsPauseEnabled) stop=$($c.IsStopEnabled) next=$($c.IsNextEnabled) prev=$($c.IsPreviousEnabled) seek=$($c.IsPlaybackPositionEnabled)"
    if ($Cmd)
    {
        $op = switch -Regex ($Cmd)
        {
            '^play$' { $s.TryPlayAsync() }
            '^pause$' { $s.TryPauseAsync() }
            '^stop$' { $s.TryStopAsync() }
            '^next$' { $s.TrySkipNextAsync() }
            '^prev$' { $s.TrySkipPreviousAsync() }
            '^seek:(.+)$' { $s.TryChangePlaybackPositionAsync([long]([double]::Parse($Matches[1], [Globalization.CultureInfo]::InvariantCulture) * 10000000)) }
        }
        "  cmd $Cmd -> $(Await $op ([bool]))"
    }
}
"MATCHED $found"




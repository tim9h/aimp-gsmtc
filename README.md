# AIMP → Windows media overlay (GSMTC)

An AIMP plugin that publishes the currently playing track to the Windows **System Media Transport Controls**
(the media overlay / lock screen / keyboard media keys, and apps that read
`GlobalSystemMediaTransportControlsSession`).

Published to Windows:

- playback state (playing / paused / stopped)
- title, artist, album artist, album
- album art (thumbnail)
- timeline (position and duration, seeking from the overlay)

Handled from Windows and forwarded to AIMP: play, pause, stop, next, previous and seek.

Targets 64-bit AIMP (built against the AIMP 6.00 SDK in `aimp_sdk\`, only uses APIs that also exist in AIMP 5.x).

## Install

1. Build (see below) or take `build\aimp_mediacontrol.dll`.
2. Copy it to `<AIMP profile>\Plugins\aimp_mediacontrol\aimp_mediacontrol.dll`
   (usually `%APPDATA%\AIMP\Plugins\aimp_mediacontrol\`, or `<AIMP install dir>\Plugins\aimp_mediacontrol\`).
3. **Disable other SMTC plugins** such as the bundled `aimp_win10_mediacontrol`
   (*Settings → Plugins*). They publish to the same window/session and would overwrite each other.
4. Restart AIMP and enable *Media Control (Windows GSMTC)* in *Settings → Plugins*.

## Build

Requires MinGW-w64 `g++` (x64) on `PATH`; no Windows SDK needed.

```powershell
.\build.ps1                 # plugin DLL + test tools -> build\
.\build.ps1 -Target plugin  # plugin DLL only
```

The WinRT interfaces are not in MinGW's headers, so `src\winrt_decls.h` is generated from the system metadata
(`%WINDIR%\System32\WinMetadata\*.winmd`) with the .NET SDK. Only needed when changing the set of interfaces:

```powershell
dotnet run --project tools\DumpWinrt -- src\winrt_decls.h
```

## Tests

`tests\run_mock_test.ps1` loads the plugin into a mock AIMP host (`tests\mock_host`) and verifies, through the real
Windows GSMTC API (`tests\gsmtc_reader.ps1`, Windows PowerShell 5.1), that state, metadata, thumbnail, timeline and
all transport buttons work and that the session disappears when the plugin is unloaded:

```powershell
.\build.ps1
powershell -ExecutionPolicy Bypass -File tests\run_mock_test.ps1
```

`build\smtc_probe.exe` publishes a fake track without AIMP (`smtc_probe.exe [seconds] [cover-image]`).

## Troubleshooting

Set the environment variable `AIMP_GSMTC_LOG` to a file path before starting AIMP to get a diagnostic log
(also visible with DebugView as `[aimp-gsmtc]`).

## Layout

| Path | Purpose |
| --- | --- |
| `src\plugin.cpp` | AIMP plugin: event hook, player/album-art access, button and seek handling |
| `src\smtc_publisher.*` | SMTC wrapper (window binding, display updater, timeline, button events) |
| `src\winrt_runtime.h` | Runtime-loaded WinRT entry points, COM pointer, agile event handler |
| `src\winrt_decls.h` | Generated WinRT ABI declarations |
| `tools\DumpWinrt` | Generator for `winrt_decls.h` |
| `tests\` | Probe, mock AIMP host, GSMTC reader and the end-to-end test |

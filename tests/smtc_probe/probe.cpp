// Standalone harness for SmtcPublisher: publishes a fake track without AIMP so the GSMTC side can be verified.
// Usage: smtc_probe.exe [seconds-to-run] [cover-image-file]
#include <cstdio>
#include <cstdlib>
#include "smtc_publisher.h"

using namespace winrt_abi;

namespace {

const UINT kMsgButton = WM_APP + 1;
const UINT kMsgSeek = WM_APP + 2;

SmtcPublisher g_pub;
MediaPlaybackStatus g_status = MediaPlaybackStatus_Playing;
int g_track = 1;
double g_pos = 10, g_dur = 200;

void PublishTrack()
{
    SmtcPublisher::Meta m;
    m.title = L"Probe Title " + std::to_wstring(g_track);
    m.artist = L"Probe Artist";
    m.albumArtist = L"Probe Album Artist";
    m.album = L"Probe Album";
    g_pub.SetMeta(&m, true);
    g_pub.SetTimeline(g_pos, g_dur);
    g_pub.SetPlaybackStatus(g_status);
}

LRESULT CALLBACK WndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case kMsgButton:
        wprintf(L"BUTTON %d\n", static_cast<int>(wp));
        fflush(stdout);
        switch (wp)
        {
        case SystemMediaTransportControlsButton_Play: g_status = MediaPlaybackStatus_Playing; break;
        case SystemMediaTransportControlsButton_Pause: g_status = MediaPlaybackStatus_Paused; break;
        case SystemMediaTransportControlsButton_Stop: g_status = MediaPlaybackStatus_Stopped; break;
        case SystemMediaTransportControlsButton_Next: g_track++; g_pos = 0; break;
        case SystemMediaTransportControlsButton_Previous: g_track--; g_pos = 0; break;
        }
        PublishTrack();
        return 0;
    case kMsgSeek:
        g_pos = static_cast<double>(static_cast<INT64>(lp)) / 1e7;
        wprintf(L"SEEK %.1f\n", g_pos);
        fflush(stdout);
        g_pub.SetTimeline(g_pos, g_dur);
        return 0;
    case WM_TIMER:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    int seconds = argc > 1 ? _wtoi(argv[1]) : 30;
    if (argc > 3 && wcscmp(argv[3], L"sta") == 0) CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SmtcProbeWnd";
    RegisterClassW(&wc);
    HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"SMTC Probe", WS_OVERLAPPEDWINDOW, 0, 0, 200, 100, nullptr, nullptr,
                               wc.hInstance, nullptr);

    if (argc > 3 && wcscmp(argv[3], L"show") == 0) ShowWindow(wnd, SW_SHOWNA);
    if (!g_pub.Init({wnd}, wnd, kMsgButton, kMsgSeek))
    {
        wprintf(L"INIT FAILED\n");
        return 1;
    }
    wprintf(L"READY\n");
    fflush(stdout);

    PublishTrack();
    if (argc > 2)
    {
        if (FILE* f = _wfopen(argv[2], L"rb"))
        {
            std::vector<uint8_t> bytes;
            uint8_t buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
            fclose(f);
            g_pub.SetThumbnail(bytes.data(), bytes.size());
            wprintf(L"THUMBNAIL %zu bytes\n", bytes.size());
            fflush(stdout);
        }
    }

    SetTimer(wnd, 1, static_cast<UINT>(seconds) * 1000, nullptr);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    g_pub.Shutdown();
    wprintf(L"DONE\n");
    return 0;
}


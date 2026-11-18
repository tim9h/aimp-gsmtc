#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include "winrt_runtime.h"

// Thin wrapper over Windows.Media.SystemMediaTransportControls (GSMTC publisher side).
// Must be used from a single thread (the one that called Init). Transport button presses and seek requests
// are raised on WinRT worker threads and forwarded to `notifyWnd` as window messages:
//   msgButton: WPARAM = SystemMediaTransportControlsButton
//   msgSeek:   LPARAM = requested position in 100 ns ticks
class SmtcPublisher
{
public:
    struct Meta
    {
        std::wstring title, artist, albumArtist, album;
    };

    SmtcPublisher();
    ~SmtcPublisher();

    // `targets` are tried in order for ISystemMediaTransportControlsInterop::GetForWindow.
    bool Init(const std::vector<HWND>& targets, HWND notifyWnd, UINT msgButton, UINT msgSeek);
    void Shutdown();
    bool IsReady() const { return ready_; }

    void SetPlaybackStatus(winrt_abi::MediaPlaybackStatus status);
    // nullptr clears all metadata. keepThumbnail=false drops the previous track's artwork.
    void SetMeta(const Meta* meta, bool keepThumbnail);
    // nullptr/0 clears the artwork.
    void SetThumbnail(const uint8_t* data, size_t size);
    void SetTimeline(double positionSec, double durationSec);

private:
    void UpdateDisplay();

    rt::Api api_;
    bool apiLoaded_ = false;
    bool ready_ = false;
    bool roInitialized_ = false;

    rt::ComPtr<winrt_abi::ISystemMediaTransportControls> smtc_;
    rt::ComPtr<winrt_abi::ISystemMediaTransportControls2> smtc2_;
    rt::ComPtr<winrt_abi::ISystemMediaTransportControlsDisplayUpdater> updater_;
    rt::ComPtr<winrt_abi::IRandomAccessStreamReferenceStatics> streamRefStatics_;
    rt::ComPtr<winrt_abi::IRandomAccessStreamReference> thumbnail_;

    winrt_abi::EventRegistrationToken buttonToken_ = 0;
    winrt_abi::EventRegistrationToken seekToken_ = 0;
    bool buttonRegistered_ = false;
    bool seekRegistered_ = false;

    bool hasMeta_ = false;
    Meta meta_;
};

void Log(const wchar_t* fmt, ...);

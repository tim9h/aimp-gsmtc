// AIMP plugin that mirrors the playback state and the current track to Windows' System Media Transport
// Controls (GSMTC) and routes the transport buttons / seek requests back into AIMP.
#include <string>
#include <vector>
#include <cstdint>
#include <cwchar>
#include <memory>

#include "smtc_publisher.h"

// AIMP SDK headers (they define macros such as _C, so they come after the standard headers).
#include "apiPlugin.h"
#include "apiCore.h"
#include "apiObjects.h"
#include "apiPlayer.h"
#include "apiMessages.h"
#include "apiFileManager.h"
#include "apiAlbumArt.h"

using winrt_abi::MediaPlaybackStatus;

namespace {

const UINT kMsgButton = WM_APP + 1;   // WPARAM: SystemMediaTransportControlsButton
const UINT kMsgSeek = WM_APP + 2;     // LPARAM: position in 100 ns ticks
const UINT kMsgAimpEvent = WM_APP + 10; // WPARAM: 1 = position changed only
const UINT kMsgArt = WM_APP + 11;     // WPARAM: request generation, LPARAM: std::vector<uint8_t>*
const UINT_PTR kRefreshTimer = 1;
const UINT kRefreshDelayMs = 100;
const wchar_t kWindowClass[] = L"AimpGsmtcBridgeWindow";

struct Snapshot
{
    bool valid = false;
    int state = -1;
    bool hasTrack = false;
    std::wstring title, artist, album, albumArtist, artKey;
    double duration = 0;
};

struct Globals
{
    rt::ComPtr<IAIMPCore> core;
    rt::ComPtr<IAIMPServicePlayer> player;
    rt::ComPtr<IAIMPServiceMessageDispatcher> dispatcher;
    rt::ComPtr<IAIMPServiceAlbumArt> albumArt;
    rt::ComPtr<IAIMPFileInfo> artInfo; // keeps the request's file info alive while the search runs

    SmtcPublisher publisher;
    HMODULE module = nullptr;
    HWND bridgeWnd = nullptr;
    HWND fallbackWnd = nullptr;
    bool classRegistered = false;
    bool hooked = false;
    bool refreshQueued = false;
    bool timelineOnly = true;

    Snapshot last;
    uint64_t artGeneration = 0;
    bool artPending = false;
    TTaskHandle artTask = 0;
};

Globals* g = nullptr;
volatile HWND g_postTarget = nullptr; // read by the AIMP hook / album art callback, which may run on other threads

std::wstring GetString(IAIMPPropertyList* list, int id)
{
    rt::ComPtr<IAIMPString> s;
    if (SUCCEEDED(list->GetValueAsObject(id, IID_IAIMPString, s.PutVoid())) && s)
        return std::wstring(s->GetData(), static_cast<size_t>(s->GetLength()));
    return std::wstring();
}

std::wstring BaseNameWithoutExtension(const std::wstring& path)
{
    size_t slash = path.find_last_of(L"\\/");
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) name.resize(dot);
    return name;
}

bool IsUrl(const std::wstring& s) { return s.find(L"://") != std::wstring::npos; }

void CancelArtRequest()
{
    if (g->artPending && g->albumArt && g->artTask) g->albumArt->Cancel(g->artTask, 0);
    g->artPending = false;
    g->artTask = 0;
    g->artInfo.Reset();
    g->artGeneration++;
}

void WINAPI ArtCallback(IAIMPImage* image, IAIMPImageContainer* container, void* userData)
{
    // May run on a worker thread: copy the bytes and hand them to the main thread.
    auto* bytes = new std::vector<uint8_t>();
    if (container && container->GetDataSize() > 0 && container->GetData())
    {
        bytes->assign(container->GetData(), container->GetData() + container->GetDataSize());
    }
    else if (image && g_postTarget && g && g->core)
    {
        rt::ComPtr<IAIMPMemoryStream> mem;
        if (SUCCEEDED(g->core->CreateObject(IID_IAIMPMemoryStream, mem.PutVoid())) && mem &&
            SUCCEEDED(image->SaveToStream(mem.Get(), AIMP_IMAGE_FORMAT_PNG)))
        {
            const uint8_t* data = static_cast<const uint8_t*>(mem->GetData());
            INT64 size = mem->GetSize();
            if (data && size > 0) bytes->assign(data, data + size);
        }
    }
    HWND target = g_postTarget;
    if (!target || !PostMessageW(target, kMsgArt, reinterpret_cast<WPARAM>(userData), reinterpret_cast<LPARAM>(bytes)))
        delete bytes;
}

void RequestArt(IAIMPFileInfo* info)
{
    CancelArtRequest();
    if (!g->albumArt) return;
    g->artInfo.Reset();
    info->AddRef();
    *g->artInfo.Put() = info;
    TTaskHandle task = 0;
    g->artPending = true;
    HRESULT hr = g->albumArt->Get2(info, 0, &ArtCallback, reinterpret_cast<void*>(static_cast<uintptr_t>(g->artGeneration)), &task);
    if (FAILED(hr))
    {
        g->artPending = false;
        g->artInfo.Reset();
        Log(L"AlbumArt.Get2 failed: 0x%08lx", static_cast<unsigned long>(hr));
        return;
    }
    g->artTask = task;
}

MediaPlaybackStatus ToStatus(int state)
{
    switch (state)
    {
    case AIMP_PLAYER_STATE_PLAYING: return winrt_abi::MediaPlaybackStatus_Playing;
    case AIMP_PLAYER_STATE_PAUSED: return winrt_abi::MediaPlaybackStatus_Paused;
    default: return winrt_abi::MediaPlaybackStatus_Stopped;
    }
}

void Refresh(bool timelineOnly)
{
    if (!g || !g->player || !g->publisher.IsReady()) return;

    Snapshot cur;
    cur.valid = true;
    cur.state = g->player->GetState();

    rt::ComPtr<IAIMPFileInfo> info;
    if (SUCCEEDED(g->player->GetInfo(info.Put())) && info)
    {
        std::wstring fileName = GetString(info.Get(), AIMP_FILEINFO_PROPID_FILENAME);
        cur.title = GetString(info.Get(), AIMP_FILEINFO_PROPID_TITLE);
        cur.artist = GetString(info.Get(), AIMP_FILEINFO_PROPID_ARTIST);
        cur.album = GetString(info.Get(), AIMP_FILEINFO_PROPID_ALBUM);
        cur.albumArtist = GetString(info.Get(), AIMP_FILEINFO_PROPID_ALBUMARTIST);
        if (cur.title.empty() && !fileName.empty() && !IsUrl(fileName)) cur.title = BaseNameWithoutExtension(fileName);
        cur.hasTrack = !fileName.empty() || !cur.title.empty();
        cur.artKey = IsUrl(fileName) ? fileName + L"|" + cur.artist + L"|" + cur.title : fileName;
        double duration = 0;
        if (FAILED(info->GetValueAsFloat(AIMP_FILEINFO_PROPID_DURATION, &duration)) || duration <= 0)
            g->player->GetDuration(&duration);
        cur.duration = duration > 0 ? duration : 0;
    }

    const Snapshot& prev = g->last;
    bool first = !prev.valid;
    bool metaChanged = first || cur.hasTrack != prev.hasTrack || cur.title != prev.title || cur.artist != prev.artist ||
                       cur.album != prev.album || cur.albumArtist != prev.albumArtist;
    bool artChanged = first || cur.artKey != prev.artKey;
    bool stateChanged = first || cur.state != prev.state;
    bool durationChanged = first || cur.duration != prev.duration;

    if (metaChanged || artChanged)
    {
        if (cur.hasTrack)
        {
            SmtcPublisher::Meta meta;
            meta.title = cur.title;
            meta.artist = cur.artist;
            meta.albumArtist = cur.albumArtist;
            meta.album = cur.album;
            g->publisher.SetMeta(&meta, !artChanged);
        }
        else
        {
            g->publisher.SetMeta(nullptr, false);
        }
    }
    if (artChanged)
    {
        if (cur.hasTrack) RequestArt(info.Get());
        else CancelArtRequest();
    }
    if (stateChanged) g->publisher.SetPlaybackStatus(ToStatus(cur.state));
    if (timelineOnly || metaChanged || artChanged || stateChanged || durationChanged)
    {
        double position = 0;
        g->player->GetPosition(&position);
        g->publisher.SetTimeline(position, cur.duration);
    }
    g->last = cur;
}

void HandleButton(int button)
{
    if (!g || !g->player || !g->dispatcher) return;
    switch (button)
    {
    case winrt_abi::SystemMediaTransportControlsButton_Play:
        if (g->player->GetState() == AIMP_PLAYER_STATE_PAUSED) g->player->Resume();
        else g->dispatcher->Send(AIMP_MSG_CMD_PLAY, 0, nullptr);
        break;
    case winrt_abi::SystemMediaTransportControlsButton_Pause:
        if (g->player->GetState() == AIMP_PLAYER_STATE_PLAYING) g->player->Pause();
        break;
    case winrt_abi::SystemMediaTransportControlsButton_Stop:
        g->player->Stop();
        break;
    case winrt_abi::SystemMediaTransportControlsButton_Next:
        g->dispatcher->Send(AIMP_MSG_CMD_NEXT, 0, nullptr);
        break;
    case winrt_abi::SystemMediaTransportControlsButton_Previous:
        g->dispatcher->Send(AIMP_MSG_CMD_PREV, 0, nullptr);
        break;
    default:
        return;
    }
    // State events will follow, but make sure the overlay is refreshed even if AIMP stays silent.
    g->timelineOnly = false;
    SetTimer(g->bridgeWnd, kRefreshTimer, kRefreshDelayMs, nullptr);
}

LRESULT CALLBACK BridgeWndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case kMsgAimpEvent:
        if (!g) return 0;
        if (wp != 1) g->timelineOnly = false;
        SetTimer(wnd, kRefreshTimer, kRefreshDelayMs, nullptr);
        return 0;
    case WM_TIMER:
        if (wp == kRefreshTimer && g)
        {
            KillTimer(wnd, kRefreshTimer);
            bool timelineOnly = g->timelineOnly;
            g->timelineOnly = true;
            Refresh(timelineOnly);
        }
        return 0;
    case kMsgButton:
        HandleButton(static_cast<int>(wp));
        return 0;
    case kMsgSeek:
        if (g && g->player)
        {
            double seconds = static_cast<double>(static_cast<INT64>(lp)) / 1e7;
            if (seconds < 0) seconds = 0;
            g->player->SetPosition(seconds);
            g->timelineOnly = true;
            SetTimer(wnd, kRefreshTimer, kRefreshDelayMs, nullptr);
        }
        return 0;
    case kMsgArt:
    {
        std::unique_ptr<std::vector<uint8_t>> bytes(reinterpret_cast<std::vector<uint8_t>*>(lp));
        if (g && static_cast<uint64_t>(wp) == g->artGeneration)
        {
            g->artPending = false;
            g->artTask = 0;
            g->artInfo.Reset();
            if (bytes && !bytes->empty()) g->publisher.SetThumbnail(bytes->data(), bytes->size());
        }
        return 0;
    }
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

class MessageHook final : public IAIMPMessageHook
{
public:
    HRESULT WINAPI QueryInterface(REFIID riid, void** out) override
    {
        if (!out) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, IID_IAIMPMessageHook))
        {
            *out = static_cast<IAIMPMessageHook*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG WINAPI AddRef() override { return 2; }
    ULONG WINAPI Release() override { return 1; }

    void WINAPI CoreMessage(DWORD message, INT32 param1, void*, HRESULT*) override
    {
        // Only post: this may be called from any thread.
        HWND target = g_postTarget;
        if (!target) return;
        switch (message)
        {
        case AIMP_MSG_EVENT_STREAM_START:
        case AIMP_MSG_EVENT_STREAM_START_SUBTRACK:
        case AIMP_MSG_EVENT_STREAM_END:
        case AIMP_MSG_EVENT_PLAYER_STATE:
        case AIMP_MSG_EVENT_PLAYING_FILE_INFO:
            PostMessageW(target, kMsgAimpEvent, 0, 0);
            break;
        case AIMP_MSG_EVENT_PROPERTY_VALUE:
            if (param1 == AIMP_MSG_PROPERTY_PLAYER_POSITION) PostMessageW(target, kMsgAimpEvent, 1, 0);
            break;
        }
    }
};

MessageHook g_hook;

class Plugin final : public IAIMPPlugin
{
public:
    HRESULT WINAPI QueryInterface(REFIID riid, void** out) override
    {
        if (!out) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown))
        {
            *out = static_cast<IAIMPPlugin*>(this);
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG WINAPI AddRef() override { return 2; }
    ULONG WINAPI Release() override { return 1; }

    PChar WINAPI InfoGet(INT32 index) override
    {
        switch (index)
        {
        case AIMP_PLUGIN_INFO_NAME: return const_cast<PChar>(L"Media Control (Windows GSMTC)");
        case AIMP_PLUGIN_INFO_AUTHOR: return const_cast<PChar>(L"aimp-mediacontrol");
        case AIMP_PLUGIN_INFO_SHORT_DESCRIPTION:
            return const_cast<PChar>(L"Publishes the current track and playback state to the Windows media overlay (SMTC)");
        case AIMP_PLUGIN_INFO_FULL_DESCRIPTION:
            return const_cast<PChar>(
                L"Publishes playback state, title, artist, album, cover art and position of the current track to the Windows "
                L"Global System Media Transport Controls and handles play/pause/stop/next/previous/seek requests from it.");
        }
        return nullptr;
    }

    DWORD WINAPI InfoGetCategories() override { return AIMP_PLUGIN_CATEGORY_ADDONS; }

    HRESULT WINAPI Initialize(IAIMPCore* core) override;
    HRESULT WINAPI Finalize() override;

    void WINAPI SystemNotification(INT32, IUnknown*) override {}
};

Plugin g_plugin;

bool CreateBridgeWindow()
{
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&BridgeWndProc), &g->module);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = BridgeWndProc;
    wc.hInstance = g->module;
    wc.lpszClassName = kWindowClass;
    if (RegisterClassW(&wc)) g->classRegistered = true;
    g->bridgeWnd = CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, g->module, nullptr);
    return g->bridgeWnd != nullptr;
}

HRESULT Plugin::Initialize(IAIMPCore* core)
{
    if (g) return E_UNEXPECTED;
    g = new Globals();
    core->AddRef();
    *g->core.Put() = core;

    if (FAILED(core->QueryInterface(IID_IAIMPServicePlayer, g->player.PutVoid())) ||
        FAILED(core->QueryInterface(IID_IAIMPServiceMessageDispatcher, g->dispatcher.PutVoid())))
    {
        Log(L"required AIMP services are not available");
        Finalize();
        return E_FAIL;
    }
    core->QueryInterface(IID_IAIMPServiceAlbumArt, g->albumArt.PutVoid());

    if (!CreateBridgeWindow())
    {
        Log(L"cannot create bridge window");
        Finalize();
        return E_FAIL;
    }
    g_postTarget = g->bridgeWnd;

    std::vector<HWND> targets;
    for (int kind : {AIMP_MPH_MAINFORM, AIMP_MPH_APPLICATION})
    {
        HWND wnd = nullptr;
        if (SUCCEEDED(g->dispatcher->Send(AIMP_MSG_PROPERTY_HWND, kind, &wnd)) && wnd) targets.push_back(wnd);
    }
    bool ok = g->publisher.Init(targets, g->bridgeWnd, kMsgButton, kMsgSeek);
    if (!ok)
    {
        // Last resort: a hidden top-level window of our own.
        g->fallbackWnd = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, L"AIMP Media Control", WS_POPUP, 0, 0, 0, 0, nullptr,
                                         nullptr, g->module, nullptr);
        ok = g->fallbackWnd && g->publisher.Init({g->fallbackWnd}, g->bridgeWnd, kMsgButton, kMsgSeek);
    }
    if (!ok)
    {
        Log(L"SMTC initialization failed");
        Finalize();
        return E_FAIL;
    }

    g->dispatcher->Hook(&g_hook);
    g->hooked = true;
    Refresh(false);
    return S_OK;
}

HRESULT Plugin::Finalize()
{
    if (!g) return S_OK;
    g_postTarget = nullptr;
    if (g->hooked && g->dispatcher) g->dispatcher->Unhook(&g_hook);
    g->hooked = false;
    if (g->bridgeWnd) KillTimer(g->bridgeWnd, kRefreshTimer);
    if (g->artPending)
    {
        // A search thread may still call ArtCallback after the cancel request: keep this DLL loaded.
        HMODULE pinned = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCWSTR>(&ArtCallback), &pinned);
    }
    CancelArtRequest();
    g->publisher.Shutdown();
    if (g->bridgeWnd) DestroyWindow(g->bridgeWnd);
    if (g->fallbackWnd) DestroyWindow(g->fallbackWnd);
    if (g->classRegistered) UnregisterClassW(kWindowClass, g->module);
    Globals* old = g;
    g = nullptr;
    delete old;
    return S_OK;
}

} // namespace

extern "C" EXPORT HRESULT WINAPI AIMPPluginGetHeader(IAIMPPlugin** header)
{
    if (!header) return E_POINTER;
    *header = &g_plugin;
    return S_OK;
}


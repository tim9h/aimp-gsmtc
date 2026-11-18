#include "smtc_publisher.h"
#include <cstdarg>
#include <cstdio>

using namespace winrt_abi;

// Must not live in an anonymous namespace: with no visible implementation, GCC would treat calls through it as unreachable.
struct ISmtcInterop : public IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE GetForWindow(HWND window, REFIID riid, void** result) = 0;
};

namespace {

// ISystemMediaTransportControlsInterop (systemmediatransportcontrolsinterop.h)
const GUID IID_ISmtcInterop = {0xddb0472d, 0xc911, 0x4a1f, {0x86, 0xd9, 0xdc, 0x3d, 0x71, 0xa9, 0x5f, 0x5a}};

const wchar_t kSmtcClass[] = L"Windows.Media.SystemMediaTransportControls";
const wchar_t kTimelineClass[] = L"Windows.Media.SystemMediaTransportControlsTimelineProperties";

INT64 ToTicks(double seconds)
{
    if (seconds < 0) seconds = 0;
    return static_cast<INT64>(seconds * 10000000.0);
}

} // namespace

void Log(const wchar_t* fmt, ...)
{
    wchar_t buf[1024];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf(buf, 1000, fmt, args);
    va_end(args);
    buf[1000] = 0;
    OutputDebugStringW((std::wstring(L"[aimp-gsmtc] ") + buf + L"\n").c_str());

    wchar_t path[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"AIMP_GSMTC_LOG", path, MAX_PATH);
    if (n > 0 && n < MAX_PATH)
    {
        if (FILE* f = _wfopen(path, L"a, ccs=UTF-8"))
        {
            SYSTEMTIME t;
            GetLocalTime(&t);
            fwprintf(f, L"%02d:%02d:%02d.%03d %ls\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, buf);
            fclose(f);
        }
    }
}

SmtcPublisher::SmtcPublisher() = default;

SmtcPublisher::~SmtcPublisher() { Shutdown(); }

bool SmtcPublisher::Init(const std::vector<HWND>& targets, HWND notifyWnd, UINT msgButton, UINT msgSeek)
{
    if (ready_) return true;
    if (!apiLoaded_) apiLoaded_ = api_.Load();
    if (!apiLoaded_)
    {
        Log(L"WinRT entry points are not available");
        return false;
    }

    HRESULT hr = api_.RoInitialize(1 /* RO_INIT_MULTITHREADED */);
    if (SUCCEEDED(hr)) roInitialized_ = true; // RPC_E_CHANGED_MODE: thread already initialized as STA, still usable

    rt::ComPtr<ISmtcInterop> interop;
    {
        rt::HString cls(api_, kSmtcClass);
        hr = api_.RoGetActivationFactory(cls.Get(), IID_ISmtcInterop, interop.PutVoid());
    }
    if (FAILED(hr))
    {
        Log(L"RoGetActivationFactory(SystemMediaTransportControls) failed: 0x%08lx", static_cast<unsigned long>(hr));
        Shutdown();
        return false;
    }

    for (HWND wnd : targets)
    {
        if (!wnd) continue;
        hr = interop->GetForWindow(wnd, IID_ISystemMediaTransportControls, smtc_.PutVoid());
        if (SUCCEEDED(hr) && smtc_) break;
        Log(L"GetForWindow(%p) failed: 0x%08lx", static_cast<void*>(wnd), static_cast<unsigned long>(hr));
        smtc_.Reset();
    }
    if (!smtc_)
    {
        Shutdown();
        return false;
    }

    smtc_.As(IID_ISystemMediaTransportControls2, &smtc2_);
    if (FAILED(smtc_->get_DisplayUpdater(updater_.Put())) || !updater_)
    {
        Log(L"get_DisplayUpdater failed");
        Shutdown();
        return false;
    }

    smtc_->put_IsEnabled(TRUE);
    smtc_->put_IsPlayEnabled(TRUE);
    smtc_->put_IsPauseEnabled(TRUE);
    smtc_->put_IsStopEnabled(TRUE);
    smtc_->put_IsNextEnabled(TRUE);
    smtc_->put_IsPreviousEnabled(TRUE);

    {
        rt::HString cls(api_, RuntimeClass_RandomAccessStreamReference);
        hr = api_.RoGetActivationFactory(cls.Get(), IID_IRandomAccessStreamReferenceStatics, streamRefStatics_.PutVoid());
        if (FAILED(hr)) Log(L"RandomAccessStreamReference factory failed: 0x%08lx", static_cast<unsigned long>(hr));
    }

    auto* buttonHandler = new rt::EventHandler(IID_ButtonPressedHandler, [=](IInspectable*, IInspectable* args) {
        rt::ComPtr<ISystemMediaTransportControlsButtonPressedEventArgs> a;
        INT32 button = -1;
        if (args && SUCCEEDED(args->QueryInterface(IID_ISystemMediaTransportControlsButtonPressedEventArgs, a.PutVoid())) &&
            SUCCEEDED(a->get_Button(&button)))
            PostMessageW(notifyWnd, msgButton, static_cast<WPARAM>(button), 0);
    });
    hr = smtc_->add_ButtonPressed(buttonHandler, &buttonToken_);
    buttonHandler->Release();
    buttonRegistered_ = SUCCEEDED(hr);
    if (!buttonRegistered_) Log(L"add_ButtonPressed failed: 0x%08lx", static_cast<unsigned long>(hr));

    if (smtc2_)
    {
        auto* seekHandler = new rt::EventHandler(IID_PlaybackPositionChangeRequestedHandler, [=](IInspectable*, IInspectable* args) {
            rt::ComPtr<IPlaybackPositionChangeRequestedEventArgs> a;
            TimeSpan pos{};
            if (args && SUCCEEDED(args->QueryInterface(IID_IPlaybackPositionChangeRequestedEventArgs, a.PutVoid())) &&
                SUCCEEDED(a->get_RequestedPlaybackPosition(&pos)))
                PostMessageW(notifyWnd, msgSeek, 0, static_cast<LPARAM>(pos.Duration));
        });
        hr = smtc2_->add_PlaybackPositionChangeRequested(seekHandler, &seekToken_);
        seekHandler->Release();
        seekRegistered_ = SUCCEEDED(hr);
        if (!seekRegistered_) Log(L"add_PlaybackPositionChangeRequested failed: 0x%08lx", static_cast<unsigned long>(hr));
    }

    ready_ = true;
    Log(L"SMTC initialized");
    return true;
}

void SmtcPublisher::Shutdown()
{
    if (smtc_)
    {
        if (buttonRegistered_) smtc_->remove_ButtonPressed(buttonToken_);
        if (seekRegistered_ && smtc2_) smtc2_->remove_PlaybackPositionChangeRequested(seekToken_);
        smtc_->put_PlaybackStatus(MediaPlaybackStatus_Closed);
        if (updater_)
        {
            updater_->ClearAll();
            updater_->Update();
        }
        smtc_->put_IsEnabled(FALSE);
    }
    buttonRegistered_ = seekRegistered_ = false;
    thumbnail_.Reset();
    streamRefStatics_.Reset();
    updater_.Reset();
    smtc2_.Reset();
    smtc_.Reset();
    hasMeta_ = false;
    ready_ = false;
    if (roInitialized_)
    {
        api_.RoUninitialize();
        roInitialized_ = false;
    }
}

void SmtcPublisher::SetPlaybackStatus(MediaPlaybackStatus status)
{
    if (ready_) smtc_->put_PlaybackStatus(status);
}

void SmtcPublisher::SetMeta(const Meta* meta, bool keepThumbnail)
{
    if (!ready_) return;
    if (meta)
    {
        meta_ = *meta;
        hasMeta_ = true;
    }
    else
    {
        hasMeta_ = false;
        meta_ = Meta();
    }
    if (!keepThumbnail || !meta) thumbnail_.Reset();
    UpdateDisplay();
}

void SmtcPublisher::SetThumbnail(const uint8_t* data, size_t size)
{
    if (!ready_) return;
    thumbnail_.Reset();
    if (data && size && streamRefStatics_)
    {
        IStream* mem = api_.SHCreateMemStream(data, static_cast<UINT>(size));
        if (mem)
        {
            rt::ComPtr<IInspectable> stream;
            HRESULT hr = api_.CreateRandomAccessStreamOverStream(mem, 0 /* BSOS_DEFAULT */, IID_IRandomAccessStream, stream.PutVoid());
            mem->Release();
            if (SUCCEEDED(hr) && stream)
                hr = streamRefStatics_->CreateFromStream(stream.Get(), thumbnail_.Put());
            if (FAILED(hr))
            {
                Log(L"creating thumbnail failed: 0x%08lx", static_cast<unsigned long>(hr));
                thumbnail_.Reset();
            }
        }
    }
    UpdateDisplay();
}

void SmtcPublisher::SetTimeline(double positionSec, double durationSec)
{
    if (!ready_ || !smtc2_) return;
    rt::ComPtr<IInspectable> obj;
    rt::ComPtr<ISystemMediaTransportControlsTimelineProperties> props;
    rt::HString cls(api_, kTimelineClass);
    if (FAILED(api_.RoActivateInstance(cls.Get(), obj.Put())) ||
        FAILED(obj.As(IID_ISystemMediaTransportControlsTimelineProperties, &props)))
        return;
    if (durationSec < 0) durationSec = 0;
    if (positionSec < 0) positionSec = 0;
    if (durationSec > 0 && positionSec > durationSec) positionSec = durationSec;
    props->put_StartTime(TimeSpan{0});
    props->put_MinSeekTime(TimeSpan{0});
    props->put_EndTime(TimeSpan{ToTicks(durationSec)});
    props->put_MaxSeekTime(TimeSpan{ToTicks(durationSec)});
    props->put_Position(TimeSpan{ToTicks(positionSec)});
    HRESULT hr = smtc2_->UpdateTimelineProperties(props.Get());
    if (FAILED(hr)) Log(L"UpdateTimelineProperties failed: 0x%08lx", static_cast<unsigned long>(hr));
}

void SmtcPublisher::UpdateDisplay()
{
    updater_->ClearAll();
    if (hasMeta_)
    {
        updater_->put_Type(MediaPlaybackType_Music);
        rt::ComPtr<IMusicDisplayProperties> music;
        if (SUCCEEDED(updater_->get_MusicProperties(music.Put())) && music)
        {
            rt::HString title(api_, meta_.title), artist(api_, meta_.artist), albumArtist(api_, meta_.albumArtist);
            music->put_Title(title.Get());
            music->put_Artist(artist.Get());
            music->put_AlbumArtist(albumArtist.Get());
            rt::ComPtr<IMusicDisplayProperties2> music2;
            if (SUCCEEDED(music.As(IID_IMusicDisplayProperties2, &music2)))
            {
                rt::HString album(api_, meta_.album);
                music2->put_AlbumTitle(album.Get());
            }
        }
        if (thumbnail_) updater_->put_Thumbnail(thumbnail_.Get());
    }
    HRESULT hr = updater_->Update();
    if (FAILED(hr)) Log(L"DisplayUpdater.Update failed: 0x%08lx", static_cast<unsigned long>(hr));
}

// Mock AIMP host: loads aimp_mediacontrol.dll, provides fake AIMP services and drives them from a command file,
// so the plugin can be verified end-to-end against the real Windows GSMTC without touching a running AIMP.
//
// Usage: mock_host.exe <plugin.dll> <command-file> <cover-image>
// Command file lines (appended while running): track <n> | state play|pause|stop | pos <sec> | quit
#include <atomic>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "apiPlugin.h"
#include "apiCore.h"
#include "apiObjects.h"
#include "apiPlayer.h"
#include "apiMessages.h"
#include "apiFileManager.h"
#include "apiAlbumArt.h"

namespace {

#define NI { return E_NOTIMPL; }

template <class Base>
class Refcounted : public Base
{
public:
    virtual ~Refcounted() = default;
    HRESULT WINAPI QueryInterface(REFIID riid, void** out) override
    {
        if (IsEqualGUID(riid, IID_IUnknown)) { *out = static_cast<Base*>(this); AddRef(); return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG WINAPI AddRef() override { return ++ref_; }
    ULONG WINAPI Release() override
    {
        ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }
private:
    std::atomic<ULONG> ref_{1};
};

// Services live for the whole process; QueryInterface only needs to answer IUnknown.
template <class Base>
class Singleton : public Base
{
public:
    HRESULT WINAPI QueryInterface(REFIID riid, void** out) override
    {
        if (IsEqualGUID(riid, IID_IUnknown)) { *out = static_cast<Base*>(this); return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG WINAPI AddRef() override { return 2; }
    ULONG WINAPI Release() override { return 1; }
};

class MockString final : public Refcounted<IAIMPString>
{
public:
    explicit MockString(std::wstring s) : s_(std::move(s)) {}
    HRESULT WINAPI Get(INT32, TChar*) override NI
    PChar WINAPI GetData() override { return const_cast<PChar>(s_.c_str()); }
    INT32 WINAPI GetLength() override { return static_cast<INT32>(s_.size()); }
    INT32 WINAPI GetHashCode() override { return 0; }
    HRESULT WINAPI Set(INT32, TChar) override NI
    HRESULT WINAPI SetData(PChar, INT32) override NI
    HRESULT WINAPI Add(IAIMPString*) override NI
    HRESULT WINAPI Add2(PChar, INT32) override NI
    HRESULT WINAPI ChangeCase(INT32) override NI
    HRESULT WINAPI Clone(IAIMPString**) override NI
    HRESULT WINAPI Compare(IAIMPString*, INT32*, BOOL) override NI
    HRESULT WINAPI Compare2(PChar, INT32, INT32*, BOOL) override NI
    HRESULT WINAPI Delete(INT32, INT32) override NI
    HRESULT WINAPI Find(IAIMPString*, INT32*, INT32, INT32) override NI
    HRESULT WINAPI Find2(TChar*, INT32, INT32*, INT32, INT32) override NI
    HRESULT WINAPI Insert(INT32, IAIMPString*) override NI
    HRESULT WINAPI Insert2(INT32, TChar*, INT32) override NI
    HRESULT WINAPI Replace(IAIMPString*, IAIMPString*, INT32) override NI
    HRESULT WINAPI Replace2(TChar*, INT32, TChar*, INT32, INT32) override NI
    HRESULT WINAPI SubString(INT32, INT32, IAIMPString**) override NI
private:
    std::wstring s_;
};

class MockFileInfo final : public Refcounted<IAIMPFileInfo>
{
public:
    std::map<int, std::wstring> strings;
    double duration = 0;

    void WINAPI BeginUpdate() override {}
    void WINAPI EndUpdate() override {}
    HRESULT WINAPI Reset() override NI
    HRESULT WINAPI GetValueAsFloat(INT32 id, DOUBLE* v) override
    {
        if (id != AIMP_FILEINFO_PROPID_DURATION) return E_FAIL;
        *v = duration;
        return S_OK;
    }
    HRESULT WINAPI GetValueAsInt32(INT32, INT32*) override NI
    HRESULT WINAPI GetValueAsInt64(INT32, INT64*) override NI
    HRESULT WINAPI GetValueAsObject(INT32 id, CONSTIID, void** v) override
    {
        auto it = strings.find(id);
        if (it == strings.end()) return E_FAIL;
        *v = new MockString(it->second);
        return S_OK;
    }
    HRESULT WINAPI SetValueAsFloat(INT32, const DOUBLE) override NI
    HRESULT WINAPI SetValueAsInt32(INT32, INT32) override NI
    HRESULT WINAPI SetValueAsInt64(INT32, const INT64) override NI
    HRESULT WINAPI SetValueAsObject(INT32, IUnknown*) override NI
    HRESULT WINAPI Assign(IAIMPFileInfo*) override NI
    HRESULT WINAPI Clone(IAIMPFileInfo**) override NI
};

class MockImageContainer final : public Refcounted<IAIMPImageContainer>
{
public:
    explicit MockImageContainer(std::vector<BYTE> d) : data_(std::move(d)) {}
    HRESULT WINAPI CreateImage(IAIMPImage**) override NI
    HRESULT WINAPI GetInfo(SIZE*, INT32*) override NI
    BYTE* WINAPI GetData() override { return data_.data(); }
    DWORD WINAPI GetDataSize() override { return static_cast<DWORD>(data_.size()); }
    HRESULT WINAPI SetDataSize(DWORD) override NI
private:
    std::vector<BYTE> data_;
};

HWND g_mainWnd = nullptr;
const UINT kMsgCommand = WM_APP + 100;
std::vector<BYTE> g_cover;

class MockDispatcher;
MockDispatcher* g_dispatcher = nullptr;

void Say(const char* text)
{
    printf("%s\n", text);
    fflush(stdout);
}

class MockPlayer final : public Singleton<IAIMPServicePlayer>
{
public:
    int state = AIMP_PLAYER_STATE_STOPPED;
    double position = 0;
    int track = 0;

    HRESULT WINAPI Play(IAIMPPlaybackQueueItem*) override NI
    HRESULT WINAPI Play2(IAIMPPlaylistItem*) override NI
    HRESULT WINAPI Play3(IAIMPPlaylist*) override NI
    HRESULT WINAPI Play4(IAIMPString*, DWORD) override NI
    HRESULT WINAPI GoToNext() override NI
    HRESULT WINAPI GoToPrev() override NI
    HRESULT WINAPI GetDuration(DOUBLE* s) override { *s = track ? 180.0 + track : 0; return S_OK; }
    HRESULT WINAPI GetPosition(DOUBLE* s) override { *s = position; return S_OK; }
    HRESULT WINAPI SetPosition(const DOUBLE s) override { position = s; printf("PLAYER SetPosition %.1f\n", s); fflush(stdout); return S_OK; }
    HRESULT WINAPI GetMute(BOOL*) override NI
    HRESULT WINAPI SetMute(const BOOL) override NI
    HRESULT WINAPI GetVolume(SINGLE*) override NI
    HRESULT WINAPI SetVolume(const SINGLE) override NI
    HRESULT WINAPI GetInfo(IAIMPFileInfo** info) override
    {
        if (!track) return E_FAIL;
        auto* fi = new MockFileInfo();
        std::wstring n = std::to_wstring(track);
        fi->strings[AIMP_FILEINFO_PROPID_TITLE] = L"Mock Title " + n;
        fi->strings[AIMP_FILEINFO_PROPID_ARTIST] = L"Mock Artist " + n;
        fi->strings[AIMP_FILEINFO_PROPID_ALBUM] = L"Mock Album";
        fi->strings[AIMP_FILEINFO_PROPID_ALBUMARTIST] = L"Mock Album Artist";
        fi->strings[AIMP_FILEINFO_PROPID_FILENAME] = L"C:\\music\\track " + n + L".mp3";
        fi->duration = 180.0 + track;
        *info = fi;
        return S_OK;
    }
    HRESULT WINAPI GetPlaylistItem(IAIMPPlaylistItem**) override NI
    INT32 WINAPI GetState() override { return state; }
    HRESULT WINAPI Pause() override { SetState(AIMP_PLAYER_STATE_PAUSED); Say("PLAYER Pause"); return S_OK; }
    HRESULT WINAPI Resume() override { SetState(AIMP_PLAYER_STATE_PLAYING); Say("PLAYER Resume"); return S_OK; }
    HRESULT WINAPI Stop() override { SetState(AIMP_PLAYER_STATE_STOPPED); Say("PLAYER Stop"); return S_OK; }
    HRESULT WINAPI StopAfterTrack() override NI

    void SetState(int s);
};

class MockDispatcher final : public Singleton<IAIMPServiceMessageDispatcher>
{
public:
    std::vector<IAIMPMessageHook*> hooks;
    MockPlayer* player = nullptr;

    HRESULT WINAPI Send(DWORD msg, INT32 p1, void* p2) override
    {
        if (msg == AIMP_MSG_PROPERTY_HWND)
        {
            if (p1 == AIMP_MPH_MAINFORM) { *static_cast<HWND*>(p2) = g_mainWnd; return S_OK; }
            return E_FAIL;
        }
        if (msg == AIMP_MSG_CMD_PLAY)
        {
            Say("CMD play");
            if (!player->track) player->track = 1;
            player->SetState(AIMP_PLAYER_STATE_PLAYING);
            return S_OK;
        }
        if (msg == AIMP_MSG_CMD_NEXT)
        {
            Say("CMD next");
            player->track++;
            player->position = 0;
            Fire(AIMP_MSG_EVENT_STREAM_START, 0);
            return S_OK;
        }
        if (msg == AIMP_MSG_CMD_PREV)
        {
            Say("CMD prev");
            if (player->track > 1) player->track--;
            player->position = 0;
            Fire(AIMP_MSG_EVENT_STREAM_START, 0);
            return S_OK;
        }
        return E_NOTIMPL;
    }
    DWORD WINAPI Register(PChar) override { return 0; }
    HRESULT WINAPI Hook(IAIMPMessageHook* h) override { hooks.push_back(h); Say("HOOKED"); return S_OK; }
    HRESULT WINAPI Unhook(IAIMPMessageHook* h) override
    {
        for (size_t i = 0; i < hooks.size(); i++)
            if (hooks[i] == h) { hooks.erase(hooks.begin() + i); break; }
        Say("UNHOOKED");
        return S_OK;
    }
    void Fire(DWORD msg, INT32 p1)
    {
        HRESULT r = S_OK;
        for (auto* h : std::vector<IAIMPMessageHook*>(hooks)) h->CoreMessage(msg, p1, nullptr, &r);
    }
};

void MockPlayer::SetState(int s)
{
    state = s;
    g_dispatcher->Fire(AIMP_MSG_EVENT_PLAYER_STATE, s);
}

class MockAlbumArt final : public Singleton<IAIMPServiceAlbumArt>
{
public:
    HRESULT WINAPI Get(IAIMPString*, IAIMPString*, IAIMPString*, DWORD, TAIMPServiceAlbumArtReceiveProc*, void*, TTaskHandle*) override NI
    HRESULT WINAPI Get2(IAIMPFileInfo* info, DWORD, TAIMPServiceAlbumArtReceiveProc* cb, void* user, TTaskHandle* task) override
    {
        info->AddRef();
        *task = 1;
        std::thread([=]() {
            Sleep(300); // like AIMP: results arrive asynchronously on a worker thread
            auto* c = new MockImageContainer(g_cover);
            cb(nullptr, c, user);
            c->Release();
            info->Release();
        }).detach();
        Say("ART requested");
        return S_OK;
    }
    HRESULT WINAPI Cancel(TTaskHandle, DWORD) override { return S_OK; }
};

class MockCore final : public Singleton<IAIMPCore>
{
public:
    MockPlayer player;
    MockDispatcher dispatcher;
    MockAlbumArt albumArt;

    HRESULT WINAPI QueryInterface(REFIID riid, void** out) override
    {
        *out = nullptr;
        if (IsEqualGUID(riid, IID_IUnknown)) *out = static_cast<IAIMPCore*>(this);
        else if (IsEqualGUID(riid, IID_IAIMPServicePlayer)) *out = static_cast<IAIMPServicePlayer*>(&player);
        else if (IsEqualGUID(riid, IID_IAIMPServiceMessageDispatcher)) *out = static_cast<IAIMPServiceMessageDispatcher*>(&dispatcher);
        else if (IsEqualGUID(riid, IID_IAIMPServiceAlbumArt)) *out = static_cast<IAIMPServiceAlbumArt*>(&albumArt);
        return *out ? S_OK : E_NOINTERFACE;
    }
    HRESULT WINAPI CreateObject(CONSTIID, void**) override NI
    HRESULT WINAPI GetPath(int, IAIMPString**) override NI
    HRESULT WINAPI RegisterExtension(CONSTIID, IUnknown*) override NI
    HRESULT WINAPI RegisterService(IUnknown*) override NI
    HRESULT WINAPI UnregisterExtension(IUnknown*) override NI
};

MockCore* g_core = nullptr;

void ExecuteCommand(const std::wstring& line)
{
    MockPlayer& p = g_core->player;
    if (line.rfind(L"track ", 0) == 0)
    {
        p.track = _wtoi(line.c_str() + 6);
        p.position = 0;
        g_dispatcher->Fire(AIMP_MSG_EVENT_STREAM_START, 0);
        g_dispatcher->Fire(AIMP_MSG_EVENT_PLAYING_FILE_INFO, 0);
    }
    else if (line == L"state play") p.SetState(AIMP_PLAYER_STATE_PLAYING);
    else if (line == L"state pause") p.SetState(AIMP_PLAYER_STATE_PAUSED);
    else if (line == L"state stop") p.SetState(AIMP_PLAYER_STATE_STOPPED);
    else if (line.rfind(L"pos ", 0) == 0)
    {
        p.position = _wtof(line.c_str() + 4);
        g_dispatcher->Fire(AIMP_MSG_EVENT_PROPERTY_VALUE, AIMP_MSG_PROPERTY_PLAYER_POSITION);
    }
    wprintf(L"EXEC %ls\n", line.c_str());
    fflush(stdout);
}

LRESULT CALLBACK HostWndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == kMsgCommand)
    {
        std::wstring* s = reinterpret_cast<std::wstring*>(lp);
        if (*s == L"quit") PostQuitMessage(0);
        else ExecuteCommand(*s);
        delete s;
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 4) return 2;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); // like AIMP's (Delphi) main thread

    if (FILE* f = _wfopen(argv[3], L"rb"))
    {
        BYTE buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) g_cover.insert(g_cover.end(), buf, buf + n);
        fclose(f);
    }

    WNDCLASSW wc = {};
    wc.lpfnWndProc = HostWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MockAimpMainForm";
    RegisterClassW(&wc);
    g_mainWnd = CreateWindowExW(0, wc.lpszClassName, L"Mock AIMP", WS_OVERLAPPEDWINDOW, 0, 0, 300, 200, nullptr, nullptr,
                                wc.hInstance, nullptr);
    ShowWindow(g_mainWnd, SW_SHOWNA);

    g_core = new MockCore();
    g_dispatcher = &g_core->dispatcher;
    g_core->dispatcher.player = &g_core->player;

    HMODULE dll = LoadLibraryW(argv[1]);
    if (!dll) { printf("LOAD FAILED %lu\n", GetLastError()); return 1; }
    auto getHeader = reinterpret_cast<TAIMPPluginGetHeaderProc>(GetProcAddress(dll, "AIMPPluginGetHeader"));
    IAIMPPlugin* plugin = nullptr;
    if (!getHeader || FAILED(getHeader(&plugin)) || !plugin) { Say("NO HEADER"); return 1; }
    wprintf(L"PLUGIN %ls\n", plugin->InfoGet(AIMP_PLUGIN_INFO_NAME));
    HRESULT hr = plugin->Initialize(g_core);
    printf("INIT %08lx\n", static_cast<unsigned long>(hr));
    fflush(stdout);
    if (FAILED(hr)) return 1;

    std::wstring cmdFile = argv[2];
    std::thread([cmdFile]() {
        size_t consumed = 0;
        for (;;)
        {
            Sleep(100);
            std::wifstream in{std::string(cmdFile.begin(), cmdFile.end())};
            std::wstring line;
            size_t idx = 0;
            while (std::getline(in, line))
            {
                if (idx++ < consumed) continue;
                consumed++;
                while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
                if (!line.empty()) PostMessageW(g_mainWnd, kMsgCommand, 0, reinterpret_cast<LPARAM>(new std::wstring(line)));
            }
        }
    }).detach();

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    plugin->Finalize();
    Say("FINALIZED");
    FreeLibrary(dll);
    Say("UNLOADED");
    return 0;
}


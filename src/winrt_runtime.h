#pragma once
#include <windows.h>
#include <objidl.h>
#include <functional>
#include <string>
#include <utility>
#include "winrt_decls.h"

namespace rt {

template <class T>
class ComPtr
{
public:
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ~ComPtr() { Reset(); }

    T* Get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    T** Put() { Reset(); return &p_; }
    void** PutVoid() { Reset(); return reinterpret_cast<void**>(&p_); }

    void Reset()
    {
        if (p_) { p_->Release(); p_ = nullptr; }
    }

    template <class U>
    HRESULT As(const IID& iid, ComPtr<U>* out) const
    {
        if (!p_) return E_POINTER;
        return p_->QueryInterface(iid, out->PutVoid());
    }

private:
    T* p_ = nullptr;
};

// Entry points are resolved at runtime so the plugin has no hard import on WinRT libraries.
struct Api
{
    HRESULT (WINAPI *RoInitialize)(int) = nullptr;
    void (WINAPI *RoUninitialize)() = nullptr;
    HRESULT (WINAPI *RoGetActivationFactory)(HSTRING, REFIID, void**) = nullptr;
    HRESULT (WINAPI *RoActivateInstance)(HSTRING, IInspectable**) = nullptr;
    HRESULT (WINAPI *WindowsCreateString)(PCWSTR, UINT32, HSTRING*) = nullptr;
    HRESULT (WINAPI *WindowsDeleteString)(HSTRING) = nullptr;
    HRESULT (WINAPI *CreateRandomAccessStreamOverStream)(IStream*, int, REFIID, void**) = nullptr;
    IStream* (WINAPI *SHCreateMemStream)(const BYTE*, UINT) = nullptr;

    bool Load()
    {
        HMODULE combase = LoadLibraryW(L"combase.dll");
        HMODULE shcore = LoadLibraryW(L"shcore.dll");
        HMODULE shlwapi = LoadLibraryW(L"shlwapi.dll");
        if (!combase || !shcore || !shlwapi) return false;
        RoInitialize = reinterpret_cast<decltype(RoInitialize)>(GetProcAddress(combase, "RoInitialize"));
        RoUninitialize = reinterpret_cast<decltype(RoUninitialize)>(GetProcAddress(combase, "RoUninitialize"));
        RoGetActivationFactory = reinterpret_cast<decltype(RoGetActivationFactory)>(GetProcAddress(combase, "RoGetActivationFactory"));
        RoActivateInstance = reinterpret_cast<decltype(RoActivateInstance)>(GetProcAddress(combase, "RoActivateInstance"));
        WindowsCreateString = reinterpret_cast<decltype(WindowsCreateString)>(GetProcAddress(combase, "WindowsCreateString"));
        WindowsDeleteString = reinterpret_cast<decltype(WindowsDeleteString)>(GetProcAddress(combase, "WindowsDeleteString"));
        CreateRandomAccessStreamOverStream = reinterpret_cast<decltype(CreateRandomAccessStreamOverStream)>(
            GetProcAddress(shcore, "CreateRandomAccessStreamOverStream"));
        SHCreateMemStream = reinterpret_cast<decltype(SHCreateMemStream)>(GetProcAddress(shlwapi, "SHCreateMemStream"));
        return RoInitialize && RoUninitialize && RoGetActivationFactory && RoActivateInstance && WindowsCreateString &&
               WindowsDeleteString && CreateRandomAccessStreamOverStream && SHCreateMemStream;
    }
};

class HString
{
public:
    HString(const Api& api, const wchar_t* s) : api_(api)
    {
        api_.WindowsCreateString(s, static_cast<UINT32>(wcslen(s)), &h_);
    }
    HString(const Api& api, const std::wstring& s) : api_(api)
    {
        api_.WindowsCreateString(s.c_str(), static_cast<UINT32>(s.size()), &h_);
    }
    HString(const HString&) = delete;
    HString& operator=(const HString&) = delete;
    ~HString() { if (h_) api_.WindowsDeleteString(h_); }
    HSTRING Get() const { return h_; }

private:
    const Api& api_;
    HSTRING h_ = nullptr;
};

// ITypedEventHandler<TSender, TArgs>::Invoke has the same ABI for every instantiation.
struct IEventHandlerAbi : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE Invoke(IInspectable* sender, IInspectable* args) = 0;
};

static const GUID IID_IAgileObject_ = {0x94ea2b94, 0xe9cc, 0x49e0, {0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90}};

// Agile (free-threaded) delegate: invoked directly on the thread that raises the event, so the callback
// must only do thread-safe work (we post a window message).
class EventHandler final : public IEventHandlerAbi
{
public:
    using Callback = std::function<void(IInspectable*, IInspectable*)>;

    EventHandler(const GUID& iid, Callback cb) : iid_(iid), cb_(std::move(cb)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (!out) return E_POINTER;
        if (IsEqualGUID(riid, IID_IUnknown) || IsEqualGUID(riid, iid_) || IsEqualGUID(riid, IID_IAgileObject_))
        {
            *out = static_cast<IEventHandlerAbi*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&ref_)); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }
    HRESULT STDMETHODCALLTYPE Invoke(IInspectable* sender, IInspectable* args) override
    {
        cb_(sender, args);
        return S_OK;
    }

private:
    LONG ref_ = 1;
    GUID iid_;
    Callback cb_;
};

} // namespace rt

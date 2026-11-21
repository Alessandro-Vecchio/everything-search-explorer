#include "folder.h"

#include "version.h"

HINSTANCE g_hInst = nullptr;
LONG g_cRefDll = 0;

namespace {

class CClassFactory final : public IClassFactory {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(::InterlockedIncrement(&m_ref));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const LONG ref = ::InterlockedDecrement(&m_ref);
        if (ref == 0) {
            ::InterlockedDecrement(&g_cRefDll);
            delete this;
        }
        return static_cast<ULONG>(ref);
    }

    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        return CFolder::Create(riid, ppv);
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {
        if (lock) {
            ::InterlockedIncrement(&g_cRefDll);
        } else {
            ::InterlockedDecrement(&g_cRefDll);
        }
        return S_OK;
    }

private:
    LONG m_ref = 1;
};

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hInst = instance;
        ::DisableThreadLibraryCalls(instance);
        ESX_TRACE_LOG("DllMain attach (pid %lu)", ::GetCurrentProcessId());
    } else if (reason == DLL_PROCESS_DETACH) {
        ESX_TRACE_LOG("DllMain detach");
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    ESX_TRACE_LOG("DllGetClassObject(match=%d)", IsEqualCLSID(rclsid, CLSID_EverythingSearchFolder));
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    if (!IsEqualCLSID(rclsid, CLSID_EverythingSearchFolder)) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }

    auto* factory = new (std::nothrow) CClassFactory();
    if (!factory) return E_OUTOFMEMORY;

    ::InterlockedIncrement(&g_cRefDll);
    const HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    ESX_TRACE_LOG("DllGetClassObject -> 0x%08lX", hr);
    return hr;
}

STDAPI DllCanUnloadNow() {
    ESX_TRACE_LOG("DllCanUnloadNow(refs=%ld)", g_cRefDll);
    return g_cRefDll == 0 ? S_OK : S_FALSE;
}

// Exported so tooling (and the release check) can read the build version
// without parsing the version resource.
extern "C" const wchar_t* EsNseVersion() {
    return ESX_VERSION_WSTRING;
}

// The DLL's COM plumbing (architecture section 9.1).
//
// Registration is by package manifest, not by registry keys: the class is
// activated through a COM surrogate declared in the MSIX (see
// packaging/msix/AppxManifest.xml). So there is no DllRegisterServer here, and
// there should not be -- a shell extension that writes to HKCR is one the
// installer cannot cleanly remove.

#include <windows.h>

#include <wrl/implements.h>
#include <wrl/module.h>

#include "shellext.h"

using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;

namespace {

class ClassFactory final
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IClassFactory> {
public:
    ClassFactory() { morph::shellext::AddObject(); }
    ~ClassFactory() override { morph::shellext::ReleaseObject(); }

    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** out) override {
        if (out == nullptr) return E_POINTER;
        *out = nullptr;
        // Aggregation is not supported and saying so is the contract.
        if (outer != nullptr) return CLASS_E_NOAGGREGATION;
        return morph::shellext::CreateRootCommand(riid, out);
    }

    IFACEMETHODIMP LockServer(BOOL lock) override {
        lock ? morph::shellext::AddObject() : morph::shellext::ReleaseObject();
        return S_OK;
    }
};

}  // namespace

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** out) {
    if (out == nullptr) return E_POINTER;
    *out = nullptr;
    if (!::IsEqualCLSID(clsid, morph::shellext::kMorphContextMenuClsid)) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }

    auto factory = Microsoft::WRL::Make<ClassFactory>();
    if (!factory) return E_OUTOFMEMORY;
    return factory.CopyTo(riid, out);
}

STDAPI DllCanUnloadNow() {
    return morph::shellext::LiveObjects() == 0 ? S_OK : S_FALSE;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID /*reserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        // Nothing here may block, allocate at scale, or touch the disk: this
        // runs while the surrogate is loading and Explorer is waiting on the
        // activation (section 9.1). Disabling the thread notifications is the
        // one thing worth doing.
        ::DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

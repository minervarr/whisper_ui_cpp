#include "device_notify.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <objbase.h>

#include <atomic>

namespace audio {

namespace {

// Implementación de IMMNotificationClient. Refcounted, posteo asíncrono al hilo UI.
class NotificationClient : public IMMNotificationClient {
public:
    NotificationClient(HWND hwnd) : hwnd_(hwnd) {}

    // IUnknown ------------------------------------------------------------------
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ref_.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = ref_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void ** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    // IMMNotificationClient -----------------------------------------------------
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override {
        post(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override {
        post(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override {
        post(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        // Filtramos: solo nos interesa el default de captura del rol "console".
        if (flow == eCapture && role == eConsole) post();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override {
        // Friendly name puede cambiar; refrescamos por seguridad.
        post(); return S_OK;
    }

private:
    void post() {
        if (hwnd_) PostMessageW(hwnd_, WM_DEVICES_CHANGED, 0, 0);
    }

    HWND               hwnd_;
    std::atomic<ULONG> ref_{1};
};

} // namespace

struct DeviceNotifier::Impl {
    IMMDeviceEnumerator * enumer = nullptr;
    NotificationClient  * client = nullptr;
};

DeviceNotifier::~DeviceNotifier() { stop(); }

bool DeviceNotifier::start(HWND hwnd) {
    if (impl_) return true;
    if (!hwnd) return false;

    auto * impl = new Impl();
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  (void **) &impl->enumer);
    if (FAILED(hr) || !impl->enumer) {
        delete impl;
        return false;
    }

    impl->client = new NotificationClient(hwnd);  // refcount inicial = 1
    hr = impl->enumer->RegisterEndpointNotificationCallback(impl->client);
    if (FAILED(hr)) {
        impl->client->Release();
        impl->enumer->Release();
        delete impl;
        return false;
    }

    impl_ = impl;
    return true;
}

void DeviceNotifier::stop() {
    if (!impl_) return;
    if (impl_->enumer && impl_->client) {
        impl_->enumer->UnregisterEndpointNotificationCallback(impl_->client);
    }
    if (impl_->client) impl_->client->Release();
    if (impl_->enumer) impl_->enumer->Release();
    delete impl_;
    impl_ = nullptr;
}

} // namespace audio

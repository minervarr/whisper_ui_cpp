#include "device_enum.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <objbase.h>

#include "../../audio_engine/src/main/cpp/usb_audio.h"

namespace audio {

namespace {

template <typename T>
void safe_release(T *& p) {
    if (p) { p->Release(); p = nullptr; }
}

// Guard que llama CoInitializeEx(APARTMENTTHREADED) solo si COM aún no estaba inicializado
// en este hilo. Permite que tanto el hilo UI (ya en STA) como callers ad-hoc usen estas
// funciones sin pisarse.
struct ComGuard {
    bool initialized = false;
    ComGuard() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        initialized = SUCCEEDED(hr);
        // S_FALSE = ya estaba inicializado en el mismo apartamento; tampoco hace falta uninit.
    }
    ~ComGuard() {
        if (initialized) CoUninitialize();
    }
};

IMMDeviceEnumerator * create_enumerator() {
    IMMDeviceEnumerator * e = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  (void **) &e);
    if (FAILED(hr)) return nullptr;
    return e;
}

std::wstring read_id(IMMDevice * dev) {
    if (!dev) return {};
    LPWSTR id = nullptr;
    if (FAILED(dev->GetId(&id)) || !id) return {};
    std::wstring out = id;
    CoTaskMemFree(id);
    return out;
}

std::wstring read_friendly_name(IMMDevice * dev) {
    if (!dev) return {};
    IPropertyStore * ps = nullptr;
    if (FAILED(dev->OpenPropertyStore(STGM_READ, &ps)) || !ps) return {};
    PROPVARIANT pv;
    PropVariantInit(&pv);
    std::wstring out;
    if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) {
        out = pv.pwszVal;
    }
    PropVariantClear(&pv);
    ps->Release();
    return out;
}

} // namespace

std::vector<Device> enumerate_capture_devices() {
    std::vector<Device> result;
    ComGuard guard;

    IMMDeviceEnumerator * enumer = create_enumerator();
    if (!enumer) return result;

    std::wstring default_id;
    {
        IMMDevice * def = nullptr;
        if (SUCCEEDED(enumer->GetDefaultAudioEndpoint(eCapture, eConsole, &def)) && def) {
            default_id = read_id(def);
            safe_release(def);
        }
    }

    IMMDeviceCollection * col = nullptr;
    if (FAILED(enumer->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &col)) || !col) {
        safe_release(enumer);
        return result;
    }

    UINT count = 0;
    col->GetCount(&count);
    result.reserve(count);

    for (UINT i = 0; i < count; ++i) {
        IMMDevice * dev = nullptr;
        if (FAILED(col->Item(i, &dev)) || !dev) continue;

        Device d;
        d.id            = read_id(dev);
        d.friendly_name = read_friendly_name(dev);
        d.is_default    = !default_id.empty() && d.id == default_id;

        safe_release(dev);

        if (!d.id.empty()) result.push_back(std::move(d));
    }

    safe_release(col);
    safe_release(enumer);

    // Add Direct USB DAC devices
    auto usb_devices = UsbAudioDriver::enumerateUsbAudioDevices();
    for (const auto& udev : usb_devices) {
        Device d;
        wchar_t id_buf[64];
        swprintf_s(id_buf, L"LIBUSB:%04X:%04X", udev.vid, udev.pid);
        d.id = id_buf;

        int size = MultiByteToWideChar(CP_UTF8, 0, udev.name.c_str(), -1, nullptr, 0);
        if (size > 0) {
            std::wstring wname(size, 0);
            MultiByteToWideChar(CP_UTF8, 0, udev.name.c_str(), -1, &wname[0], size);
            d.friendly_name = L"[USB DAC] " + wname;
            d.friendly_name.resize(wcslen(d.friendly_name.c_str()));
        } else {
            d.friendly_name = L"[USB DAC] Dispositivo Desconocido";
        }
        result.push_back(std::move(d));
    }

    return result;
}

std::wstring default_capture_id() {
    ComGuard guard;
    IMMDeviceEnumerator * enumer = create_enumerator();
    if (!enumer) return {};

    std::wstring out;
    IMMDevice * def = nullptr;
    if (SUCCEEDED(enumer->GetDefaultAudioEndpoint(eCapture, eConsole, &def)) && def) {
        out = read_id(def);
        safe_release(def);
    }
    safe_release(enumer);
    return out;
}

bool device_exists(const std::wstring & id) {
    if (id.empty()) return false;
    auto devices = enumerate_capture_devices();
    for (const auto & d : devices) {
        if (d.id == id) return true;
    }
    return false;
}

std::wstring friendly_name_for(const std::wstring & id) {
    if (id.empty()) return {};

    if (id.rfind(L"LIBUSB:", 0) == 0) {
        auto devices = enumerate_capture_devices();
        for (const auto & d : devices) {
            if (d.id == id) return d.friendly_name;
        }
        return L"[USB DAC] Desconocido";
    }

    ComGuard guard;
    IMMDeviceEnumerator * enumer = create_enumerator();
    if (!enumer) return {};

    std::wstring out;
    IMMDevice * dev = nullptr;
    if (SUCCEEDED(enumer->GetDevice(id.c_str(), &dev)) && dev) {
        out = read_friendly_name(dev);
        safe_release(dev);
    }
    safe_release(enumer);
    return out;
}

} // namespace audio

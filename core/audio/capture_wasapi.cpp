// WASAPI capture backend (Windows) on the CaptureBackend seam. Port of the
// old src/audio/wasapi_capture.cpp WASAPI path: shared-mode event-driven
// client with AUTOCONVERTPCM asking for mono float32 16 kHz directly, so no
// conversion/resampling is needed. NOT built or verified on this machine —
// verify on the Windows PC.
#ifdef _WIN32

#include "core/audio/capture_backends.h"
#include "core/audio/capture_convert.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <ksmedia.h>
#include <avrt.h>
#include <objbase.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

namespace audio {
namespace detail {

namespace {

template <typename T>
void safe_release(T *& p) {
    if (p) { p->Release(); p = nullptr; }
}

std::string wide_to_utf8(const std::wstring & w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int) w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int) w.size(),
                        s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring utf8_to_wide(const std::string & s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int) s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int) s.size(), w.data(), n);
    return w;
}

// COM guard for ad-hoc enumeration calls from any thread.
struct ComGuard {
    bool initialized = false;
    ComGuard() { initialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)); }
    ~ComGuard() { if (initialized) CoUninitialize(); }
};

IMMDeviceEnumerator * create_enumerator() {
    IMMDeviceEnumerator * e = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                (void **) &e)))
        return nullptr;
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
    if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) &&
        pv.vt == VT_LPWSTR && pv.pwszVal)
        out = pv.pwszVal;
    PropVariantClear(&pv);
    ps->Release();
    return out;
}

// 200 ms in 100 ns units.
constexpr REFERENCE_TIME kBufferDuration = 2'000'000;

class WasapiBackend final : public CaptureBackend {
public:
    explicit WasapiBackend(std::string device_id) : device_id_(std::move(device_id)) {}

    ~WasapiBackend() override { stop(); }

    std::string start() override
    {
        if (running_.load(std::memory_order_acquire)) return "Ya hay una captura en curso.";

        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event_) return "No se pudo crear el evento de audio (CreateEventW falló).";

        buffer_ = std::make_shared<std::vector<float>>();
        abort_reason_.clear();
        start_error_.clear();
        stop_flag_.store(false, std::memory_order_release);
        running_.store(true, std::memory_order_release);
        thread_ = std::thread(&WasapiBackend::run, this);

        // Wait briefly for the thread to confirm startup or report an error.
        for (int i = 0; i < 50; ++i) {
            Sleep(20);
            if (!running_.load(std::memory_order_acquire) || started_.load()) break;
        }
        if (!start_error_.empty()) {
            if (thread_.joinable()) thread_.join();
            CloseHandle(event_);
            event_ = nullptr;
            return start_error_;
        }
        return "";
    }

    void stop() override
    {
        stop_flag_.store(true, std::memory_order_release);
        if (event_) SetEvent(event_);
        if (thread_.joinable()) thread_.join();
        if (event_) { CloseHandle(event_); event_ = nullptr; }
    }

    std::shared_ptr<std::vector<float>> take_buffer() override
    {
        auto b = buffer_;
        buffer_.reset();
        return b;
    }

    float peak() const override { return peak_.load(std::memory_order_acquire); }
    bool  running() const override { return running_.load(std::memory_order_acquire); }
    std::string abort_reason() const override { return abort_reason_; }

private:
    void run()
    {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool com_ok = SUCCEEDED(hr);

        IMMDeviceEnumerator * enumer  = nullptr;
        IMMDevice           * device  = nullptr;
        IAudioClient        * client  = nullptr;
        IAudioCaptureClient * capture = nullptr;
        HANDLE                mmcss   = nullptr;
        bool                  started = false;

        auto fail = [&](const char * msg) {
            start_error_ = msg;
            running_.store(false, std::memory_order_release);
        };

        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator), (void **) &enumer);
        if (FAILED(hr)) { fail("No se pudo crear el enumerador de dispositivos de audio."); goto cleanup; }

        if (device_id_.empty()) {
            hr = enumer->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
        } else {
            std::wstring wid = utf8_to_wide(device_id_);
            hr = enumer->GetDevice(wid.c_str(), &device);
            if (FAILED(hr)) {
                // Persisted endpoint is gone — fall back to the default.
                hr = enumer->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
            }
        }
        if (FAILED(hr)) { fail("No se encontró un micrófono activo en Windows."); goto cleanup; }

        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **) &client);
        if (FAILED(hr)) { fail("No se pudo activar IAudioClient."); goto cleanup; }

        {
            // Ask the engine directly for whisper's format: mono float32 16 kHz.
            WAVEFORMATEXTENSIBLE wfx = {};
            wfx.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
            wfx.Format.nChannels       = 1;
            wfx.Format.nSamplesPerSec  = (DWORD) kTargetSampleRate;
            wfx.Format.wBitsPerSample  = 32;
            wfx.Format.nBlockAlign     = (WORD)(wfx.Format.nChannels * wfx.Format.wBitsPerSample / 8);
            wfx.Format.nAvgBytesPerSec = wfx.Format.nSamplesPerSec * wfx.Format.nBlockAlign;
            wfx.Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
            wfx.Samples.wValidBitsPerSample = 32;
            wfx.dwChannelMask          = SPEAKER_FRONT_CENTER;
            wfx.SubFormat              = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

            const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK
                              | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
                              | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags,
                                    kBufferDuration, 0,
                                    (WAVEFORMATEX *) &wfx, nullptr);
            if (FAILED(hr)) { fail("IAudioClient::Initialize falló (AUTOCONVERTPCM no aceptado)."); goto cleanup; }
        }

        hr = client->SetEventHandle(event_);
        if (FAILED(hr)) { fail("SetEventHandle falló."); goto cleanup; }

        hr = client->GetService(__uuidof(IAudioCaptureClient), (void **) &capture);
        if (FAILED(hr)) { fail("GetService(IAudioCaptureClient) falló."); goto cleanup; }

        {
            DWORD task_index = 0;
            mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
            // MMCSS failure is non-critical for speech capture.
        }

        hr = client->Start();
        if (FAILED(hr)) { fail("IAudioClient::Start falló."); goto cleanup; }
        started = true;
        started_.store(true, std::memory_order_release);

        while (!stop_flag_.load(std::memory_order_acquire)) {
            DWORD wait = WaitForSingleObject(event_, 1000);
            if (wait == WAIT_TIMEOUT) continue;   // idle mic — keep waiting
            if (stop_flag_.load(std::memory_order_acquire)) break;
            if (wait != WAIT_OBJECT_0) break;

            UINT32 packet_size = 0;
            hr = capture->GetNextPacketSize(&packet_size);
            if (FAILED(hr)) {
                abort_reason_ = (hr == AUDCLNT_E_DEVICE_INVALIDATED)
                    ? "El micrófono se desconectó durante la grabación."
                    : "Fallo al leer del micrófono (WASAPI).";
                break;
            }

            while (packet_size > 0) {
                BYTE  * data       = nullptr;
                UINT32  num_frames = 0;
                DWORD   bflags     = 0;
                hr = capture->GetBuffer(&data, &num_frames, &bflags, nullptr, nullptr);
                if (FAILED(hr)) {
                    abort_reason_ = (hr == AUDCLNT_E_DEVICE_INVALIDATED)
                        ? "El micrófono se desconectó durante la grabación."
                        : "Fallo al leer del micrófono (WASAPI).";
                    goto cleanup;
                }

                const size_t old_size = buffer_->size();
                buffer_->resize(old_size + num_frames);
                float * dst = buffer_->data() + old_size;

                if ((bflags & AUDCLNT_BUFFERFLAGS_SILENT) || !data) {
                    std::fill_n(dst, num_frames, 0.0f);
                } else {
                    std::memcpy(dst, data, sizeof(float) * num_frames);
                }

                float pk = 0.0f;
                for (UINT32 i = 0; i < num_frames; ++i) {
                    float v = std::fabs(dst[i]);
                    if (v > pk) pk = v;
                }
                peak_.store(pk, std::memory_order_release);

                capture->ReleaseBuffer(num_frames);
                hr = capture->GetNextPacketSize(&packet_size);
                if (FAILED(hr)) goto cleanup;
            }
        }

    cleanup:
        if (started && client) client->Stop();
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
        safe_release(capture);
        safe_release(client);
        safe_release(device);
        safe_release(enumer);
        if (com_ok) CoUninitialize();
        peak_.store(0.0f, std::memory_order_release);
        running_.store(false, std::memory_order_release);
    }

    std::string                         device_id_;   // UTF-8 endpoint id, "" = default
    std::shared_ptr<std::vector<float>> buffer_;
    std::thread                         thread_;
    HANDLE                              event_ = nullptr;
    std::atomic<bool>                   running_{false};
    std::atomic<bool>                   started_{false};
    std::atomic<bool>                   stop_flag_{false};
    std::atomic<float>                  peak_{0.0f};
    std::string                         abort_reason_;
    std::string                         start_error_;
};

} // namespace

std::vector<CaptureDeviceInfo> enumerate_wasapi()
{
    std::vector<CaptureDeviceInfo> out;
    ComGuard guard;

    IMMDeviceEnumerator * enumer = create_enumerator();
    if (!enumer) return out;

    IMMDeviceCollection * col = nullptr;
    if (SUCCEEDED(enumer->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &col)) && col) {
        UINT count = 0;
        col->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice * dev = nullptr;
            if (FAILED(col->Item(i, &dev)) || !dev) continue;
            std::wstring id = read_id(dev);
            std::wstring name = read_friendly_name(dev);
            safe_release(dev);
            if (!id.empty())
                out.push_back({BackendKind::Wasapi, wide_to_utf8(id), wide_to_utf8(name)});
        }
        safe_release(col);
    }
    safe_release(enumer);
    return out;
}

std::unique_ptr<CaptureBackend> make_wasapi(const std::string & device_id)
{
    return std::make_unique<WasapiBackend>(device_id);
}

} // namespace detail
} // namespace audio

#endif // _WIN32

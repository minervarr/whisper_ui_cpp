#include "wasapi_capture.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <ksmedia.h>
#include <avrt.h>
#include <objbase.h>

#include <algorithm>
#include <cmath>
#include <sstream>

namespace audio {

std::atomic<float> g_peak{0.0f};

namespace {

template <typename T>
void safe_release(T *& p) {
    if (p) { p->Release(); p = nullptr; }
}

std::wstring hr_to_wstring(HRESULT hr, const wchar_t * context) {
    std::wstringstream ss;
    ss << context << L" (HRESULT 0x" << std::hex << (unsigned long) hr << L")";
    return ss.str();
}

// hnsBufferDuration en unidades de 100ns. 200 ms = 2_000_000.
constexpr REFERENCE_TIME kBufferDuration = 2'000'000;

// Reservar 60 min de audio en RAM evita reallocs durante la grabación.
// 16000 samples/s * 3600 s = 57.6 millones de floats = ~230 MB.
// Sin embargo, vector_reserve no asigna memoria física hasta que se use;
// Windows hace lazy commit. Usamos 60 min como techo seguro.
constexpr size_t kReserveSamples = 16000ull * 60 * 60;

} // namespace

Capture::Capture() = default;

Capture::~Capture() {
    if (running_.load()) {
        stop();
    }
}

std::wstring Capture::start() {
    return start_impl(false);
}

std::wstring Capture::resume_with_device(const std::wstring & new_id) {
    // Pre: el hilo previo abortó por su cuenta (running_ ya en false).
    if (running_.load()) {
        return L"No se puede reanudar mientras la captura sigue activa.";
    }
    // Si el hilo previo dejó el std::thread joinable, hacer join antes de relanzar.
    if (thread_.joinable()) {
        thread_.join();
    }
    if (!buffer_) {
        // Sin buffer existente: equivalente a un start() normal.
        device_id_ = new_id;
        return start_impl(false);
    }
    device_id_ = new_id;
    return start_impl(true);
}

std::wstring Capture::start_impl(bool keep_existing_buffer) {
    if (running_.load()) {
        return L"La captura ya está activa.";
    }

    stop_flag_.store(false);
    thread_error_.clear();
    abort_reason_.clear();
    if (!keep_existing_buffer || !buffer_) {
        buffer_ = std::make_shared<std::vector<float>>();
        buffer_->reserve(kReserveSamples);
    }
    // Si reanudamos, mantenemos los samples ya capturados — los nuevos se anexan.

    // El hilo previo (en caso de aborto) deja event_handle_ sin cerrar; lo
    // cerramos aquí antes de recrearlo para no acumular HANDLEs.
    if (event_handle_) {
        CloseHandle((HANDLE) event_handle_);
        event_handle_ = nullptr;
    }
    event_handle_ = (void *) CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_handle_) {
        return L"No se pudo crear el evento de audio (CreateEventW falló).";
    }

    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&Capture::run_capture, this);

    // Esperamos brevemente a que el hilo confirme arranque o reporte error.
    for (int i = 0; i < 50; ++i) {
        Sleep(20);
        if (!running_.load() || !thread_error_.empty()) break;
    }

    if (!thread_error_.empty()) {
        if (thread_.joinable()) thread_.join();
        std::wstring err = std::move(thread_error_);
        CloseHandle((HANDLE) event_handle_);
        event_handle_ = nullptr;
        running_.store(false);
        return err;
    }

    return {};
}

void Capture::stop() {
    if (!running_.load()) return;

    stop_flag_.store(true, std::memory_order_release);
    if (event_handle_) {
        SetEvent((HANDLE) event_handle_);
    }

    if (thread_.joinable()) {
        thread_.join();
    }

    if (event_handle_) {
        CloseHandle((HANDLE) event_handle_);
        event_handle_ = nullptr;
    }
    running_.store(false, std::memory_order_release);
    g_peak.store(0.0f);
}

std::shared_ptr<std::vector<float>> Capture::take_buffer() {
    auto b = buffer_;
    buffer_.reset();
    return b;
}

double Capture::duration_seconds() const {
    if (!buffer_) return 0.0;
    return double(buffer_->size()) / double(kTargetSampleRate);
}

void Capture::run_capture() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool com_initialized = SUCCEEDED(hr);

    IMMDeviceEnumerator * enumer  = nullptr;
    IMMDevice           * device  = nullptr;
    IAudioClient        * client  = nullptr;
    IAudioCaptureClient * capture = nullptr;
    HANDLE                mmcss   = nullptr;
    bool                  started = false;

    auto fail = [&](const wchar_t * msg, HRESULT hr_in = S_OK) {
        if (FAILED(hr_in)) {
            thread_error_ = hr_to_wstring(hr_in, msg);
        } else {
            thread_error_ = msg;
        }
        running_.store(false, std::memory_order_release);
    };

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                          CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                          (void **) &enumer);
    if (FAILED(hr)) { fail(L"No se pudo crear el enumerador de dispositivos de audio.", hr); goto cleanup; }

    if (device_id_.empty()) {
        hr = enumer->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
    } else {
        hr = enumer->GetDevice(device_id_.c_str(), &device);
        if (FAILED(hr)) {
            // El endpoint guardado ya no existe — caer al default sin levantar error.
            hr = enumer->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
        }
    }
    if (FAILED(hr)) { fail(L"No se encontró un dispositivo de captura predeterminado (verifica que tengas un micrófono activo en Windows).", hr); goto cleanup; }

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **) &client);
    if (FAILED(hr)) { fail(L"No se pudo activar IAudioClient.", hr); goto cleanup; }

    {
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
        if (FAILED(hr)) { fail(L"IAudioClient::Initialize falló (AUTOCONVERTPCM no aceptado por el motor de audio).", hr); goto cleanup; }
    }

    hr = client->SetEventHandle((HANDLE) event_handle_);
    if (FAILED(hr)) { fail(L"SetEventHandle falló.", hr); goto cleanup; }

    hr = client->GetService(__uuidof(IAudioCaptureClient), (void **) &capture);
    if (FAILED(hr)) { fail(L"GetService(IAudioCaptureClient) falló.", hr); goto cleanup; }

    {
        DWORD mmcss_task_index = 0;
        mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcss_task_index);
        // Si falla MMCSS, seguimos sin priorizar. No es crítico para grabación de habla.
    }

    hr = client->Start();
    if (FAILED(hr)) { fail(L"IAudioClient::Start falló.", hr); goto cleanup; }
    started = true;

    // Bucle de captura.
    while (!stop_flag_.load(std::memory_order_acquire)) {
        DWORD wait = WaitForSingleObject((HANDLE) event_handle_, 1000);
        if (wait == WAIT_TIMEOUT) {
            // Sin paquetes en 1s — micrófono inactivo. Continuamos.
            continue;
        }
        if (stop_flag_.load(std::memory_order_acquire)) break;
        if (wait != WAIT_OBJECT_0) break;

        UINT32 packet_size = 0;
        hr = capture->GetNextPacketSize(&packet_size);
        if (FAILED(hr)) {
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                abort_reason_ = L"El micrófono se desconectó o cambió durante la grabación.";
            } else {
                abort_reason_ = hr_to_wstring(hr, L"GetNextPacketSize falló");
            }
            break;
        }

        while (packet_size > 0) {
            BYTE  * data       = nullptr;
            UINT32  num_frames = 0;
            DWORD   flags      = 0;

            hr = capture->GetBuffer(&data, &num_frames, &flags, nullptr, nullptr);
            if (FAILED(hr)) {
                if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
                    abort_reason_ = L"El micrófono se desconectó o cambió durante la grabación.";
                } else {
                    abort_reason_ = hr_to_wstring(hr, L"GetBuffer falló");
                }
                goto cleanup;
            }

            const size_t old_size = buffer_->size();
            buffer_->resize(old_size + num_frames);
            float * dst = buffer_->data() + old_size;

            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                std::fill_n(dst, num_frames, 0.0f);
            } else if (data) {
                const float * src = reinterpret_cast<const float *>(data);
                memcpy(dst, src, sizeof(float) * num_frames);
            } else {
                std::fill_n(dst, num_frames, 0.0f);
            }

            // Pico de este paquete.
            float peak = 0.0f;
            for (UINT32 i = 0; i < num_frames; ++i) {
                float v = std::fabs(dst[i]);
                if (v > peak) peak = v;
            }
            g_peak.store(peak, std::memory_order_release);

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
    if (com_initialized) CoUninitialize();
    g_peak.store(0.0f);
    running_.store(false, std::memory_order_release);
}

} // namespace audio

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
#include <vector>

#include "../../audio_engine/src/main/cpp/usb_audio.h"
#include "../../soxr/src/soxr.h"

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
    if (device_id_.rfind(L"LIBUSB:", 0) == 0) {
        uint16_t vid = 0, pid = 0;
        swscanf_s(device_id_.c_str(), L"LIBUSB:%hx:%hx", &vid, &pid);

        auto fail = [&](const wchar_t * msg) {
            thread_error_ = msg;
            running_.store(false, std::memory_order_release);
        };

        UsbAudioDriver driver;
        if (!driver.open(vid, pid)) {
            fail(L"No se pudo abrir el dispositivo USB DAC.");
            return;
        }

        if (!driver.parseDescriptors()) {
            fail(L"Fallo al leer los descriptores del USB DAC.");
            return;
        }

        auto tuples = driver.getCaptureFormatTuples();
        if (tuples.empty()) {
            fail(L"El dispositivo USB DAC no reporta formatos de captura.");
            return;
        }

        int dev_rate = tuples[0], dev_ch = tuples[1], dev_bits = tuples[2];
        for (size_t i = 0; i < tuples.size(); i += 3) {
            if (tuples[i] == 16000 && tuples[i+1] == 1 && tuples[i+2] == 16) {
                dev_rate = 16000; dev_ch = 1; dev_bits = 16; break;
            }
        }

        if (!driver.configureCapture(dev_rate, dev_ch, dev_bits)) {
            fail(L"El dispositivo USB DAC no aceptó la configuración de captura.");
            return;
        }

        if (!driver.startCapture()) {
            fail(L"No se pudo iniciar la captura en el USB DAC.");
            return;
        }

        soxr_error_t sox_err = nullptr;
        soxr_t resampler = nullptr;
        if (dev_rate != kTargetSampleRate) {
            soxr_io_spec_t io_spec = soxr_io_spec(SOXR_FLOAT32_I, SOXR_FLOAT32_I);
            soxr_quality_spec_t q_spec = soxr_quality_spec(SOXR_HQ, 0);
            resampler = soxr_create(dev_rate, kTargetSampleRate, 1, &sox_err, &io_spec, &q_spec, nullptr);
            if (sox_err) {
                fail(L"Error al inicializar el resampler (soxr).");
                return;
            }
        }

        const int buf_bytes = 4096;
        std::vector<uint8_t> read_buf(buf_bytes);
        std::vector<float> float_buf;

        while (!stop_flag_.load(std::memory_order_acquire)) {
            int bytes_read = driver.readCapture(read_buf.data(), buf_bytes);
            if (bytes_read < 0) {
                abort_reason_ = L"Error al leer del USB DAC.";
                break;
            }

            if (bytes_read == 0) {
                Sleep(10);
                continue;
            }

            int bytes_per_sample = driver.getConfiguredCaptureSubslotSize();
            if (bytes_per_sample == 0) bytes_per_sample = dev_bits / 8;
            int num_frames = bytes_read / (bytes_per_sample * dev_ch);
            
            float_buf.resize(num_frames);
            
            for (int i = 0; i < num_frames; ++i) {
                float sum = 0.0f;
                for (int c = 0; c < dev_ch; ++c) {
                    const uint8_t* ptr = &read_buf[(i * dev_ch + c) * bytes_per_sample];
                    float val = 0.0f;
                    if (bytes_per_sample == 2) {
                        int16_t v = *reinterpret_cast<const int16_t*>(ptr);
                        val = v / 32768.0f;
                    } else if (bytes_per_sample == 3) {
                        int32_t v = (ptr[0]) | (ptr[1] << 8) | ((int8_t)ptr[2] << 16);
                        val = v / 8388608.0f;
                    } else if (bytes_per_sample == 4) {
                        int32_t v = *reinterpret_cast<const int32_t*>(ptr);
                        val = v / 2147483648.0f;
                    }
                    sum += val;
                }
                float_buf[i] = sum / dev_ch;
            }

            if (resampler) {
                size_t idone = 0, odone = 0;
                std::vector<float> out_buf(num_frames * kTargetSampleRate / dev_rate + 100);
                soxr_process(resampler, float_buf.data(), float_buf.size(), &idone, out_buf.data(), out_buf.size(), &odone);
                
                const size_t old_size = buffer_->size();
                buffer_->resize(old_size + odone);
                float * dst = buffer_->data() + old_size;
                
                float peak = 0.0f;
                for (size_t i = 0; i < odone; ++i) {
                    dst[i] = out_buf[i];
                    float abs_v = std::fabs(dst[i]);
                    if (abs_v > peak) peak = abs_v;
                }
                g_peak.store(peak, std::memory_order_release);
            } else {
                const size_t old_size = buffer_->size();
                buffer_->resize(old_size + num_frames);
                float * dst = buffer_->data() + old_size;
                
                float peak = 0.0f;
                for (int i = 0; i < num_frames; ++i) {
                    dst[i] = float_buf[i];
                    float abs_v = std::fabs(dst[i]);
                    if (abs_v > peak) peak = abs_v;
                }
                g_peak.store(peak, std::memory_order_release);
            }
        }

        if (resampler) {
            soxr_delete(resampler);
        }

        driver.stopCapture();
        driver.close();
        g_peak.store(0.0f);
        running_.store(false, std::memory_order_release);
        return;
    }

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

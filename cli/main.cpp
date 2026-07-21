// whisper_destilado_cli — headless end-to-end proof of the portable core.
//
//   whisper_destilado_cli <audio.(wav|mp3|flac)> [-l <lang>]
//   whisper_destilado_cli --capture <device-index> --seconds <N> [-l <lang>] [--wav <out.wav>]
//   whisper_destilado_cli --list-devices
//
// Loads the model from <exe_dir>/models/ (first *.bin|*.gguf), transcribes a
// file or a live capture, prints segments + confidence tier. Exits non-zero
// on any error.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/audio/capture.h"
#include "core/audio/file_reader.h"
#include "core/confidence.h"
#include "core/event_queue.h"
#include "core/model_loader.h"
#include "core/settings.h"
#include "core/transcribe.h"

namespace {

void usage()
{
    std::fprintf(stderr,
        "Uso:\n"
        "  whisper_destilado_cli <audio.(wav|mp3|flac)> [-l <idioma>]\n"
        "  whisper_destilado_cli --capture <indice> --seconds <N> [-l <idioma>] [--wav <salida.wav>]\n"
        "  whisper_destilado_cli --list-devices\n");
}

int list_devices()
{
    auto devices = audio::enumerate_capture_devices();
    if (devices.empty()) {
        std::printf("No se encontraron dispositivos de captura.\n");
        return 0;
    }
    const char* kind_names[] = {"WASAPI", "ALSA", "JACK", "USB"};
    for (size_t i = 0; i < devices.size(); ++i) {
        std::printf("[%zu] %-6s %-14s %s\n", i,
                    kind_names[(int) devices[i].kind],
                    devices[i].id.c_str(), devices[i].name.c_str());
    }
    return 0;
}

// Minimal RIFF writer for the --wav debug dump (mono float -> S16, 16 kHz).
bool write_wav_16k(const std::string & path, const std::vector<float> & samples)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    auto u32 = [&](uint32_t v) { f.write((const char*) &v, 4); };
    auto u16 = [&](uint16_t v) { f.write((const char*) &v, 2); };
    uint32_t data_bytes = (uint32_t)(samples.size() * 2);
    f.write("RIFF", 4); u32(36 + data_bytes); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(1); u16(1); u32(16000);
    u32(16000 * 2); u16(2); u16(16);
    f.write("data", 4); u32(data_bytes);
    for (float v : samples) {
        float c = v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
        int16_t s = (int16_t)(c * 32767.0f);
        f.write((const char*) &s, 2);
    }
    return (bool) f;
}

std::shared_ptr<std::vector<float>> capture_seconds(int index, int seconds,
                                                    std::string * err)
{
    auto devices = audio::enumerate_capture_devices();
    if (index < 0 || (size_t) index >= devices.size()) {
        *err = "Índice de dispositivo fuera de rango (usa --list-devices).";
        return nullptr;
    }
    std::printf("Capturando %d s desde: %s\n", seconds, devices[(size_t) index].name.c_str());

    auto backend = audio::make_capture(devices[(size_t) index]);
    if (!backend) {
        *err = "No se pudo crear el backend de captura.";
        return nullptr;
    }
    std::string e = backend->start();
    if (!e.empty()) {
        *err = e;
        return nullptr;
    }

    for (int i = 0; i < seconds * 10; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (i % 10 == 0) std::printf("  pico: %.2f\n", backend->peak());
        if (!backend->running()) break;
    }
    backend->stop();

    if (!backend->abort_reason().empty()) {
        *err = backend->abort_reason();
        return nullptr;
    }
    return backend->take_buffer();
}

} // namespace

int main(int argc, char ** argv)
{
    std::string audio_path;
    std::string lang = "auto";
    std::string wav_dump;
    int capture_index = -1;
    int seconds = 5;
    bool do_list = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-l" && i + 1 < argc)              lang = argv[++i];
        else if (a == "--capture" && i + 1 < argc)  capture_index = std::atoi(argv[++i]);
        else if (a == "--seconds" && i + 1 < argc)  seconds = std::atoi(argv[++i]);
        else if (a == "--wav" && i + 1 < argc)      wav_dump = argv[++i];
        else if (a == "--list-devices")             do_list = true;
        else if (a == "-h" || a == "--help")        { usage(); return 0; }
        else if (!a.empty() && a[0] != '-')         audio_path = a;
        else { usage(); return 2; }
    }

    if (do_list) return list_devices();

    if (audio_path.empty() && capture_index < 0) {
        usage();
        return 2;
    }

    // --- Get the samples (file or live capture) ---
    std::shared_ptr<std::vector<float>> samples;
    std::string err;
    if (capture_index >= 0) {
        samples = capture_seconds(capture_index, seconds, &err);
    } else {
        if (!audio::is_supported_audio_extension(audio_path)) {
            std::fprintf(stderr, "Formato no soportado: %s (usa .wav/.mp3/.flac)\n",
                         audio_path.c_str());
            return 2;
        }
        samples = audio::load_audio_file(audio_path, &err);
    }
    if (!samples) {
        std::fprintf(stderr, "Error: %s\n", err.c_str());
        return 1;
    }
    std::printf("Audio: %zu muestras (%.2f s a 16 kHz)\n",
                samples->size(), samples->size() / 16000.0);

    if (!wav_dump.empty()) {
        if (write_wav_16k(wav_dump, *samples))
            std::printf("WAV de depuración escrito: %s\n", wav_dump.c_str());
    }

    // --- Load the model ---
    inference::ModelLoader loader;
    std::printf("Cargando modelo: %s\n",
                loader.model_filename().empty() ? "(ninguno encontrado)"
                                                : loader.model_filename().c_str());
    loader.start(core::events());

    while (loader.state() == inference::LoadState::NotStarted ||
           loader.state() == inference::LoadState::Loading) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    core::events().drain();   // consume the ModelLoaded/ModelFailed event

    if (loader.state() != inference::LoadState::Loaded) {
        std::fprintf(stderr, "Error: %s\n", loader.error_message().c_str());
        return 1;
    }
    std::printf("Modelo cargado (%s).\n", loader.gpu_used() ? "GPU/Vulkan" : "CPU");

    // --- Transcribe ---
    cfg::Settings settings = cfg::Settings::fast_defaults();
    settings.language = lang;

    inference::transcribe_async(loader.context(), samples, settings, core::events());

    inference::Result * result = nullptr;
    while (!result) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        for (auto & ev : core::events().drain()) {
            if (ev.kind == core::AppEvent::TranscribeDone) result = ev.result;
        }
    }
    std::unique_ptr<inference::Result> owned(result);
    inference::join_pending_workers();

    if (!owned->error.empty()) {
        std::fprintf(stderr, "Error: %s\n", owned->error.c_str());
        return 1;
    }

    // --- Print ---
    if (owned->segments.empty()) {
        std::printf("(sin voz detectada)\n");
    }
    for (const auto & seg : owned->segments) {
        std::printf("[%6lld ms - %6lld ms] %s\n",
                    (long long) seg.t0_ms, (long long) seg.t1_ms, seg.text.c_str());
    }
    std::printf("\nIdioma detectado: %s\nConfianza: %.2f (%s)\n",
                owned->detected_language.c_str(),
                owned->confidence_overall,
                inference::tier_label(owned->tier));
    return 0;
}

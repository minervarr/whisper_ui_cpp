// whisper_destilado_cli — headless end-to-end proof of the portable core.
//
//   whisper_destilado_cli <audio.(wav|mp3|flac)> [-f <audio>] [-m <modelo>]
//       [-l <lang>] [--device <N>] [--word-timestamps] [--beam-size N]
//       [--best-of N] [--temperature N] [-o <fmt> ... | --output <fmt> | --full]
//       [--silence-trim | --no-silence-trim]
//   whisper_destilado_cli --capture <device-index> --seconds <N> [-l <lang>]
//       [--wav <out.wav>] [--device <N>] [--output <fmt>]
//   whisper_destilado_cli --list-devices
//   whisper_destilado_cli --list-gpus
//
// Loads the model from <exe_dir>/models/ (first *.bin|*.gguf), unless -m is
// given; transcribes a file or a live capture. Exits non-zero on any error.
//
// Sampling / output control (mirrors the upstream whisper.cpp CLI):
//   -m <path>        model file to use (overrides the auto-search)
//   -f <path>        audio file (same as the positional argument)
//   --word-timestamps  split on words and capture per-token detail
//   --beam-size N    beam search with N beams (N > 1 enables beam search)
//   --best-of N      best_of candidates for the greedy pass
//   --temperature N  sampling temperature (0..1)
//   --temperature-inc N  fallback increment (0 disables the ladder)
//   -o<fmt>          repeated: write one file per format beside the audio,
//                    named <audio>.txt|srt|vtt|lrc|csv|tsv|json|json-full
//   --output <fmt>   single format to stdout (pipeable; nothing else on it)
//   --full           --output json-full + max-information settings preset
//                    (keeps silence/music/special tokens, word split, and
//                    per-token detail: id, p, plog, times). Also usable with
//                    -o to apply the preset while writing your chosen formats.
//   --silence-trim   cut long silent gaps before transcribing (energy VAD;
//                    avoids whisper repetition loops; on by default)
//   --no-silence-trim  transcribe the audio unmodified (no silence trimming)

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

#include "core/audio/capture.h"
#include "core/audio/file_reader.h"
#include "core/confidence.h"
#include "core/event_queue.h"
#include "core/gpu_devices.h"
#include "core/model_loader.h"
#include "core/output_format.h"
#include "core/settings.h"
#include "core/transcribe.h"

namespace {

void usage()
{
    std::fprintf(stderr,
        "Uso:\n"
        "  whisper_destilado_cli <audio.(wav|mp3|flac)> [-f <audio>] [-m <modelo>]\n"
        "      [-l <idioma>] [--device <N>] [--word-timestamps] [--beam-size N]\n"
        "      [--best-of N] [--temperature N] [-o<fmt> ... | --output <fmt> | --full]\n"
        "  whisper_destilado_cli --capture <indice> --seconds <N> [-l <idioma>]\n"
        "      [--wav <salida.wav>] [--device <N>] [--output <fmt>]\n"
        "  whisper_destilado_cli --list-devices\n"
        "  whisper_destilado_cli --list-gpus\n"
        "\n"
        "Modelo y audio:\n"
        "  -m, --model <ruta>   Archivo de modelo a usar (anula la búsqueda\n"
        "                       automática en <exe>/models/).\n"
        "  -f, --file <ruta>    Archivo de audio (igual que el argumento posicional).\n"
        "  -l <idioma>          Idioma (por defecto auto; p. ej. es, en, fr).\n"
        "\n"
        "GPU:\n"
        "  --device <N>   Índice de GPU para la inferencia (--list-gpus para verlos).\n"
        "                 Sin el flag y con varios GPUs, se pregunta en pantalla;\n"
        "                 en ejecución no interactiva se usa el mejor disponible.\n"
        "\n"
        "Muestreo (igual que el CLI oficial de whisper.cpp):\n"
        "  --word-timestamps       Divide por palabras y captura detalle de tokens.\n"
        "  --beam-size N, -bs N    Beam search con N haces (N > 1 lo activa).\n"
        "  --best-of N, -bo N      Candidatos best_of en el paso greedy.\n"
        "  --temperature N, -tp N  Temperatura de muestreo (0..1).\n"
        "  --temperature-inc N     Incremento de fallback (0 desactiva la escalera).\n"
        "\n"
        "Prevención de alucinaciones:\n"
        "  --silence-trim         Recorta silencios largos antes de transcribir (VAD\n"
        "                         por energía; evita bucles de repetición de whisper\n"
        "                         en grabaciones largas). Activo por defecto.\n"
        "  --no-silence-trim      Transcribe el audio tal cual, sin recortar.\n"
        "\n"
        "Salida:\n"
        "  -o<fmt>          Repetible: escribe un archivo por formato junto al audio,\n"
        "                   llamado <audio>.txt|srt|vtt|lrc|csv|tsv|json|json-full.\n"
        "  --output <fmt>   Un solo formato a stdout (limpio para tuberías).\n"
        "  --full           Atajo: --output json-full + ajustes de máxima información\n"
        "                   (conserva tokens de silencio/música/especiales, división\n"
        "                   por palabras y detalle token a token: id, p, plog, tiempos).\n"
        "                   Con -o aplica el preset y escribe los formatos que elijas.\n");
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

int list_gpus()
{
    auto gpus = inference::enumerate_gpu_devices();
    if (gpus.empty()) {
        std::printf("No se encontraron GPUs (se usará CPU).\n");
        return 0;
    }
    for (const auto & g : gpus) {
        std::printf("[%d] %-9s %s\n", g.index,
                    g.discrete ? "Discreta" : "Integrada",
                    g.name.c_str());
        if (!g.description.empty() && g.description != g.name)
            std::printf("         %s\n", g.description.c_str());
    }
    return 0;
}

// Picks the GPU to transcribe on. Returns whisper's gpu_device index, or -1
// when there is no GPU at all.
//   * explicit --device wins (validated against the list),
//   * otherwise, with several GPUs and an interactive stdin, the user is
//     asked which one to use,
//   * otherwise the best available (first discrete, else first integrated).
int choose_gpu(int forced, const std::vector<inference::GpuDeviceInfo> & gpus)
{
    if (gpus.empty()) return -1;

    if (forced >= 0) {
        for (const auto & g : gpus) {
            if (g.index == forced) return g.index;
        }
        std::fprintf(stderr,
                     "Aviso: --device %d no coincide con ningún GPU listado "
                     "(--list-gpus). Se usará el mejor disponible.\n", forced);
    }

    int best = inference::best_gpu_device(gpus);
    if (best < 0) best = gpus[0].index;

#ifndef _WIN32
    const bool tty = ::isatty(STDIN_FILENO) != 0;
#else
    const bool tty = false;
#endif

    if (gpus.size() == 1 || !tty) {
        if (gpus.size() > 1) {
            std::string best_name = "?";
            for (const auto & g : gpus) if (g.index == best) best_name = g.name;
            std::fprintf(stderr,
                         "Hay %zu GPUs; usa --list-gpus y --device <N> para elegir.\n"
                         "Se usará: %s (índice %d).\n",
                         gpus.size(), best_name.c_str(), best);
        }
        return best;
    }

    std::printf("GPUs disponibles — ¿cuál uso?\n");
    for (const auto & g : gpus) {
        std::printf("  [%d] %s: %s\n", g.index,
                    g.discrete ? "Discreta" : "Integrada",
                    g.name.c_str());
    }
    std::printf("  (vacío = %d)\n> ", best);
    std::fflush(stdout);

    char buf[64];
    if (std::fgets(buf, sizeof buf, stdin)) {
        char * end = nullptr;
        long v = std::strtol(buf, &end, 10);
        if (end != buf && (*end == '\n' || *end == '\0')) {
            for (const auto & g : gpus) {
                if (g.index == (int) v) return g.index;
            }
        }
    }
    return best;
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
    std::fprintf(stderr, "Capturando %d s desde: %s\n", seconds,
                 devices[(size_t) index].name.c_str());

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
        if (i % 10 == 0) std::fprintf(stderr, "  pico: %.2f\n", backend->peak());
        if (!backend->running()) break;
    }
    backend->stop();

    if (!backend->abort_reason().empty()) {
        *err = backend->abort_reason();
        return nullptr;
    }
    return backend->take_buffer();
}

// Removes one trailing extension: /x/audio.wav -> /x/audio. Keeps the rest of
// the path so written files land beside the source audio.
std::string audio_stem(std::string path)
{
    const size_t slash = path.find_last_of("/\\");
    const size_t dot   = path.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
        path.erase(dot);
    }
    return path;
}

bool is_known_format(const std::string & fmt)
{
    return fmt == "txt" || fmt == "srt" || fmt == "vtt" || fmt == "lrc" ||
           fmt == "csv" || fmt == "tsv" || fmt == "json" || fmt == "json-full";
}

std::string render_format(const std::string & fmt, const inference::Result & r)
{
    if      (fmt == "txt")       return inference::format_txt(r);
    else if (fmt == "srt")       return inference::format_srt(r);
    else if (fmt == "vtt")       return inference::format_vtt(r);
    else if (fmt == "lrc")       return inference::format_lrc(r);
    else if (fmt == "csv")       return inference::format_csv(r);
    else if (fmt == "tsv")       return inference::format_tsv(r);
    else if (fmt == "json")      return inference::format_json(r);
    else if (fmt == "json-full") return inference::format_json_full(r);
    return std::string();
}

} // namespace

int main(int argc, char ** argv)
{
    std::string audio_path;
    std::string model_path;       // -m/--model override
    std::string lang = "auto";
    std::string wav_dump;
    int capture_index = -1;
    int seconds = 5;
    int gpu_device = -1;          // -1 = not forced (auto/prompt)
    bool do_list = false;
    bool do_list_gpus = false;
    std::string output_fmt;               // --output <fmt>: stdout mode
    std::vector<std::string> out_files;   // -o<fmt> (repeatable): file mode
    bool full = false;
    bool word_ts = false;
    int  beam_size = 0;                   // 0 = not given
    int  best_of   = 0;                   // 0 = not given
    float temperature      = -1.0f;       // < 0 = not given
    float temperature_inc  = -1.0f;       // < 0 = not given
    int   silence_trim     = -1;          // -1 = not given (default: on)

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char * flag) -> const char * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "Falta el argumento de %s.\n", flag);
                usage();
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "-l" || a == "--language")                      lang = need("-l");
        else if (a == "-m" || a == "--model")                    model_path = need("-m");
        else if (a == "-f" || a == "--file")                     audio_path = need("-f");
        else if (a == "--capture")                               capture_index = std::atoi(need("--capture"));
        else if (a == "--seconds")                               seconds = std::atoi(need("--seconds"));
        else if (a == "--wav")                                   wav_dump = need("--wav");
        else if (a == "--device")                                gpu_device = std::atoi(need("--device"));
        else if (a == "-dev")                                    gpu_device = std::atoi(need("-dev"));
        else if (a == "--beam-size" || a == "-bs")               beam_size = std::atoi(need("--beam-size"));
        else if (a == "--best-of" || a == "-bo")                 best_of = std::atoi(need("--best-of"));
        else if (a == "--temperature" || a == "-tp")             temperature = (float) std::atof(need("--temperature"));
        else if (a == "--temperature-inc" || a == "-tpi")        temperature_inc = (float) std::atof(need("--temperature-inc"));
        else if (a == "--word-timestamps" || a == "--split-on-word" || a == "-sow") word_ts = true;
        else if (a == "--output")                                output_fmt = need("--output");
        else if (a == "-o" && i + 1 < argc)                      out_files.push_back(need("-o"));
        else if (a.rfind("-o", 0) == 0 && a.size() > 2)          out_files.push_back(a.substr(2));
        else if (a == "--full")                                  full = true;
        else if (a == "--silence-trim")                         silence_trim = 1;
        else if (a == "--no-silence-trim")                      silence_trim = 0;
        else if (a == "--list-devices")                          do_list = true;
        else if (a == "--list-gpus")                             do_list_gpus = true;
        else if (a == "-h" || a == "--help")                     { usage(); return 0; }
        else if (!a.empty() && a[0] != '-')                      audio_path = a;
        else { usage(); return 2; }
    }

    if (do_list) return list_devices();
    if (do_list_gpus) return list_gpus();

    // Resolve output mode. --full: max-info settings preset always; in stdout
    // mode it also defaults the format to json-full so `--full` keeps working
    // exactly as before.
    if (full && output_fmt.empty() && out_files.empty()) output_fmt = "json-full";
    const bool max_info = full || output_fmt == "json-full";

    if (!output_fmt.empty() && !out_files.empty()) {
        std::fprintf(stderr,
                     "No mezcles --output (modo stdout) con -o (modo archivos):\n"
                     "  usa --output <fmt> para un documento en stdout, o -o<fmt>\n"
                     "  (repetible) para guardar cada formato junto al audio.\n");
        return 2;
    }
    if (!output_fmt.empty() && !is_known_format(output_fmt)) {
        std::fprintf(stderr,
                     "Formato de salida no válido: %s\n"
                     "  Válidos: txt|srt|vtt|lrc|csv|tsv|json|json-full\n",
                     output_fmt.c_str());
        return 2;
    }
    for (const auto & f : out_files) {
        if (!is_known_format(f)) {
            std::fprintf(stderr,
                         "Formato de salida no válido: %s\n"
                         "  Válidos: txt|srt|vtt|lrc|csv|tsv|json|json-full\n",
                         f.c_str());
            return 2;
        }
    }
    if (!out_files.empty() && capture_index >= 0) {
        std::fprintf(stderr,
                     "-o escribe archivos junto al audio y la captura no tiene ruta.\n"
                     "  Usa --output <fmt> para volcar a stdout con --capture.\n");
        return 2;
    }

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
    std::fprintf(stderr, "Audio: %zu muestras (%.2f s a 16 kHz)\n",
                 samples->size(), samples->size() / 16000.0);

    if (!wav_dump.empty()) {
        if (write_wav_16k(wav_dump, *samples))
            std::fprintf(stderr, "WAV de depuración escrito: %s\n", wav_dump.c_str());
    }

    // --- Load the model ---
    inference::ModelLoader loader;
    if (!model_path.empty()) loader.set_model_path(model_path);
    int gpu_dev = choose_gpu(gpu_device, inference::enumerate_gpu_devices());
    loader.set_gpu_device(gpu_dev);
    std::fprintf(stderr, "Cargando modelo: %s\n",
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
    std::fprintf(stderr, "Modelo cargado (%s).\n", loader.gpu_used() ? "GPU/Vulkan" : "CPU");

    // --- Transcribe ---
    cfg::Settings settings = cfg::Settings::fast_defaults();
    settings.language = lang;
    if (max_info) settings = settings.with_max_info_preset();
    if (word_ts) { settings.split_on_word = true; settings.keep_tokens = true; }
    if (beam_size > 0) {
        settings.beam_size       = beam_size;
        settings.use_beam_search = beam_size > 1;   // mirror upstream: -bs 1 == greedy
    }
    if (best_of > 0) settings.best_of = best_of;
    if (temperature     >= 0.0f) settings.temperature     = temperature;
    if (temperature_inc >= 0.0f) settings.temperature_inc = temperature_inc;
    if (silence_trim    >= 0)    settings.silence_trim    = silence_trim != 0;

    if (!settings.silence_trim)
        std::fprintf(stderr, "Recorte de silencios: DESACTIVADO.\n");

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

    // --- Output ---
    if (!out_files.empty()) {
        // File mode (upstream-compatible): one written file per -o, beside the
        // audio, named <audio>.<fmt>. Diagnostics go to stderr, stdout stays
        // clean so the transcript files are the only artifacts.
        bool all_ok = true;
        for (const auto & fmt : out_files) {
            std::string content = render_format(fmt, *owned);
            std::string out_path = audio_stem(audio_path) + "." + fmt;
            std::ofstream f(out_path, std::ios::binary | std::ios::trunc);
            if (!f) {
                std::fprintf(stderr, "No se pudo escribir: %s\n", out_path.c_str());
                all_ok = false;
                continue;
            }
            f.write(content.data(), (std::streamsize) content.size());
            f.close();
            std::fprintf(stderr, "Escrito: %s\n", out_path.c_str());
        }
        return all_ok ? 0 : 1;
    }

    if (!output_fmt.empty()) {
        // Document mode: exactly one format, nothing else, on stdout. The
        // console lines and footer below are skipped on purpose so the output
        // can be piped/redirected cleanly (the format carries all the info).
        std::string out = render_format(output_fmt, *owned);
        std::printf("%s", out.c_str());
        return 0;
    }

    if (owned->segments.empty()) {
        std::printf("(sin voz detectada)\n");
    }
    for (const auto & seg : owned->segments) {
        if (seg.rep_run) continue;   // collapse repetition-loop duplicates
        std::printf("[%6lld ms - %6lld ms] %s\n",
                    (long long) seg.t0_ms, (long long) seg.t1_ms, seg.text.c_str());
    }
    std::printf("\nIdioma detectado: %s\nConfianza: %.2f (%s)\n",
                owned->detected_language.c_str(),
                owned->confidence_overall,
                inference::tier_label(owned->tier));
    return 0;
}

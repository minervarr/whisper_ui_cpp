# whisper_destilado

Cross-platform (Linux + Windows) voice transcription app on
[whisper.cpp](https://github.com/ggml-org/whisper.cpp) with a Vulkan UI
(vk_canvas). Record from a real device, transcribe locally (GPU via Vulkan,
CPU fallback), inspect confidence, save as txt/srt/vtt/json.

## Build

**Linux** (Wayland session, Vulkan driver, `jack2` dev headers — *never*
pipewire-jack —, `alsa-lib`, `wayland-client`/`xkbcommon`, slangc):

```
scripts/linux/build.sh          # -> build/linux/
```

**Windows** (MSVC BuildTools + Vulkan SDK):

```
scripts\windows\build.bat       # -> build\windows\
```

## Model

Put one whisper model (`.bin` or `.gguf`, e.g. `ggml-large-v3.bin`) into
`models/` next to the executable (`build/linux/models/`). First one in
alphabetical order wins.

## Binaries

| Binary | What |
|---|---|
| `whisper_destilado` | the GUI (raw Wayland / raw Win32 hosts, vk_canvas rendering) |
| `whisper_destilado_cli` | headless: `<audio.(wav|mp3|flac)> [-l lang]`, `--capture <n> --seconds N`, `--list-devices` |
| `whisper_tests` | unit tests (stdlib harness) |

`whisper_destilado --selftest` runs the headless model+pipeline proof.

## Capture backends

| Backend | OS | Path to the hardware |
|---|---|---|
| ALSA | Linux | direct `hw:` device, no sound server (Audacity-style) |
| JACK2 | Linux | client of the running `jackd` you started (e.g. qjackctl); real libjack only, **never pipewire-jack** |
| USB DAC | both | libusb straight to the device (UAC1/UAC2) via first_party/audio_engine |
| WASAPI | Windows | shared-mode event-driven, AUTOCONVERTPCM to 16 kHz mono float |

## Layout

```
core/         portable app core (settings, inference, capture seam, formats)
cli/  gui/    headless proof · vk_canvas app (hosts in gui/os/)
framework/    vk_canvas (THE UI framework, submodule)
first_party/  audio_engine (minervarr, modifiable, submodule)
third_party/  whisper.cpp · soxr (submodules) · dr_libs (vendored, read-only)
scripts/      per-OS cmake build entries
tests/        unit tests
```

Commits/pushes go through `./git_wrapper` — never plain `git commit`.

# Arch packages

Four microarchitecture variants, built in one pass, meant to be **uploaded**.
Each is self-contained: a person downloads exactly one file and installs it.

```bash
scripts/linux/build.sh --packages     # -> dist/linux/*.pkg.tar.zst
```

or directly:

```bash
cd packaging/arch
makepkg                                  # all four -- makepkg 7.x has no
                                          # per-split-package build flag
```

Builds the last **pushed** commit on `main`, not your working tree. Commit and
push first with `./git_wrapper save` (or plain `git push`, including
submodules) — a parent commit pointing at unpushed submodule commits fails in
`prepare()`.

## For whoever downloads one

```bash
sudo pacman -U whisper-destilado-v3-0.1.0.rNN.gXXXXXXX-1-x86_64.pkg.tar.zst
```

Then point it at a model — nothing is shipped, models run 1-3+ GB and are
separately licensed. Preferred, no `sudo` needed (put it wherever you like):

```bash
export WHISPER_MODEL_DIR=~/Models/whisper       # or WHISPER_MODEL_PATH=/exact/file.bin
```

or, without touching your shell profile, drop it into the read-only install
(needs `sudo` once per model):

```bash
sudo cp ggml-large-v3-turbo.bin /opt/whisper_destilado/models/
```

Which one to pick:

| Package | Needs | Who |
|---|---|---|
| `whisper-destilado-universal` | nothing | **any** x86-64 CPU — the safe choice |
| `whisper-destilado-v3` | AVX2, BMI2 | Intel Haswell (2013+), AMD Zen (2017+) — what the project has always shipped |
| `whisper-destilado-v4` | AVX-512 | Intel Skylake-X / Ice Lake+, AMD Zen 4+ |
| `whisper-destilado-zen4` | Zen 4 tuning | AMD Ryzen 7000 / EPYC Genoa |

None of this affects GPU inference — that always runs through Vulkan
(`GGML_VULKAN`, forced on regardless of variant). The level only changes
ggml's CPU kernels (mel spectrogram, resampling, any op without a Vulkan
path).

They can check what their CPU supports without installing anything:

```bash
/lib/ld-linux-x86-64.so.2 --help | grep -A4 'Subdirectories of glibc-hwcaps'
```

Levels marked `(supported, searched)` will run. If in doubt, `universal`.

All four `provide` and `conflict` with `whisper-destilado`, so pacman refuses
to hold two at once and swapping variants is a single `pacman -U`.

## What gets installed

```
/opt/whisper_destilado/          root:root, read-only
  whisper_destilado  assets/shaders/  assets/fonts/  models/ (empty)
/usr/bin/whisper_destilado       -> symlink into the above
/usr/share/applications/whisper-destilado.desktop
/usr/share/icons/hicolor/scalable/apps/whisper-destilado.svg
/usr/share/licenses/whisper-destilado-<variant>/

~/.config/whisper_destilado/     theirs; survives `pacman -R`
  settings.ini  msdf.cache
```

**`/usr/bin/whisper_destilado` is a symlink, not a wrapper script.**
`readlink("/proc/self/exe")` resolves symlinks, so `inference::exe_dir()`
(`core/model_loader.cpp`) still reports `/opt/whisper_destilado/`. Nothing
else is needed — settings already go to `$XDG_CONFIG_HOME` /
`~/.config/whisper_destilado/` on their own (`core/settings.cpp`), so there is
no read-only-vs-writable split to engineer here the way Matrix_Player needed
one for its database and atlas cache.

**`<exe_dir>/models/` is the fallback, not the only option.** `ModelLoader`
(`core/model_loader.cpp`) checks `WHISPER_MODEL_PATH` (exact file) then
`WHISPER_MODEL_DIR` (folder, same alphabetical-first pick) before falling
back to `<exe_dir>/models/` — i.e. `/opt/whisper_destilado/models/` for this
package, which is root-owned and needs `sudo` to populate. Setting either env
var points the app at a folder the user actually owns instead.

**There are no `install()` rules in the tree**, so `package()` does all the
work by hand. Do not switch to `cmake --install`: soxr and freetype register
their own install rules into the top-level install set, so it would install
*their* headers and libraries and not `whisper_destilado`.

## Build flags

| Flag | Why |
|---|---|
| `-DWHISPER_ARCH_LEVEL=` *(per variant)* | `universal`/`v3`/`v4`/`znver4`; see the root `CMakeLists.txt`. |
| `-DVCE_SLANGC=...` | `slangc` is required at build time; no `.spv` is committed, and `VceShaders.cmake` otherwise falls back to a hardcoded Windows path. AUR has it as `shader-slang` (`/opt/shader-slang/bin/slangc`, not on `PATH`). |

**`-march=native` is deliberately never used here.** It bakes in whatever the
build machine supports and SIGILLs on anything older — fine for a local
install (`scripts/linux/build.sh`'s interactive menu offers it), never for an
upload.

## check()

Runs `whisper_tests` once **per variant**, straight from each
`build-<variant>/` — no separate Debug tree needed, since the target already
builds as part of a normal Release configure here (unlike Matrix_Player's
Debug-gated `dsp_null_test`).

A variant this machine cannot execute dies with SIGILL and is reported
`SKIPPED`, never folded into "passed".

## Cost

Four full Release builds, roughly 20-30 minutes. There is no sharing to be
had: changing the arch level invalidates everything, including the in-tree
ggml/whisper.cpp, soxr, freetype and msdfgen builds. The split package does at
least share one clone and one submodule fetch across all four.

## Known deviations from Arch policy

- **`prepare()` touches the network** (`git submodule update --init
  --recursive`). Four submodules up to four levels deep (`vk_canvas` ->
  `vulkan_font_engine` -> `freetype`); the full `git config submodule.*.url`
  dance is not worth writing for a release build run by hand. This is what
  makes it unsuitable for the AUR as written.
- **`makepkg` clones this repo into `build/packaging/src/`** (set via
  `SRCDEST` in `scripts/linux/build.sh --packages`, gitignored via the root
  `build/` pattern — see `packaging/arch/.gitignore` for the by-hand
  fallback). A copy of the tree does live under `build/` while building;
  nothing depends on its exact location.

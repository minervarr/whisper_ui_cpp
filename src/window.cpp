#include "window.h"
#include "resource.h"
#include "led_control.h"
#include "config.h"
#include "audio/wasapi_capture.h"
#include "audio/device_enum.h"
#include "audio/device_notify.h"
#include "audio/file_reader.h"
#include "inference/model_loader.h"
#include "inference/transcribe.h"
#include "inference/output_format.h"
#include "ui/result_dialog.h"
#include "ui/settings_dlg.h"
#include "ui/language_combo.h"
#include "ui/mic_picker.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr UINT_PTR kTimerId      = 1;
constexpr UINT     kTimerPeriod  = 33; // ~30 Hz
constexpr UINT     WM_FIRST_RUN_MIC_PICKER = WM_APP + 3;  // disparado tras WM_INITDIALOG

audio::Capture        g_capture;
audio::DeviceNotifier g_notifier;
inference::ModelLoader g_model;
cfg::Settings         g_settings;
std::chrono::steady_clock::time_point g_rec_start{};
bool g_is_recording = false;
bool g_is_transcribing = false;
inference::LoadState g_last_load_state = inference::LoadState::NotStarted;
std::shared_ptr<std::vector<float>> g_audio;  // mantiene el buffer vivo para reintentos (Fase 9)
std::wstring g_mic_fallback_msg;  // status diferido si el mic guardado no estaba disponible al arrancar

// --- Transcripción por lotes (varios archivos arrastrados de una vez) ---
// Cada archivo se transcribe uno por uno y su texto se guarda como <audio>.txt
// junto al original; no se abre el diálogo de resultado en modo tanda.
std::vector<std::wstring> g_batch_files;     // rutas de la tanda (pendientes + en curso)
size_t       g_batch_index   = 0;            // índice del archivo en curso
size_t       g_batch_total   = 0;            // total de la tanda
size_t       g_batch_ok      = 0;            // transcritos con éxito
size_t       g_batch_failed  = 0;            // fallidos (carga o transcripción)
bool         g_batch_mode    = false;        // true mientras corre una tanda de >1 archivo
std::wstring g_batch_current_path;           // archivo que se transcribe ahora mismo

void init_main_language_combo(HWND hdlg) {
    HWND combo = GetDlgItem(hdlg, IDC_LANG);
    ui::populate_language_combo(combo);
    ui::set_selected_language_code(combo, g_settings.language.c_str());
}

void set_unicode_strings(HWND hdlg) {
    SetWindowTextW(hdlg, L"whisper_destilado");
    SetDlgItemTextW(hdlg, IDC_REC,        L"●  Grabar");
    SetDlgItemTextW(hdlg, IDC_TIMER,      L"00:00");
    SetDlgItemTextW(hdlg, IDC_STATUS,     L"Cargando modelo…");
    SetDlgItemTextW(hdlg, IDC_LANG_LABEL, L"Idioma:");
    SetDlgItemTextW(hdlg, IDC_ADVANCED,   L"Avanzado…");
    SetDlgItemTextW(hdlg, IDC_TRANSCRIPT,
        L"Aquí aparecerá la transcripción cuando termines de grabar.");
}

void on_model_loaded(HWND hdlg) {
    EnableWindow(GetDlgItem(hdlg, IDC_REC), TRUE);
    const wchar_t * backend = g_model.gpu_used() ? L"GPU (Vulkan)" : L"CPU";
    std::wstring filename = g_model.model_filename();
    if (filename.empty()) filename = L"modelo";
    wchar_t status[256];
    swprintf(status, 256, L"Listo.  Modelo: %s  ·  %s", filename.c_str(), backend);
    std::wstring final_status = status;
    if (!g_mic_fallback_msg.empty()) {
        final_status += L"  ·  ";
        final_status += g_mic_fallback_msg;
        g_mic_fallback_msg.clear();
    }
    SetDlgItemTextW(hdlg, IDC_STATUS, final_status.c_str());
    led_set_state(GetDlgItem(hdlg, IDC_LED), LedState::Ready);
}

void on_model_failed(HWND hdlg) {
    EnableWindow(GetDlgItem(hdlg, IDC_REC), FALSE);
    SetDlgItemTextW(hdlg, IDC_STATUS, L"Modelo no disponible.");
    led_set_state(GetDlgItem(hdlg, IDC_LED), LedState::Error);
    MessageBoxW(hdlg, g_model.error_message().c_str(),
                L"whisper_destilado — error al cargar el modelo",
                MB_OK | MB_ICONERROR);
}

void poll_model_state(HWND hdlg) {
    auto s = g_model.state();
    if (s == g_last_load_state) return;
    g_last_load_state = s;
    if (s == inference::LoadState::Loaded) on_model_loaded(hdlg);
    else if (s == inference::LoadState::Failed) on_model_failed(hdlg);
}

void update_timer_label(HWND hdlg) {
    if (!g_is_recording) return;
    auto elapsed = std::chrono::steady_clock::now() - g_rec_start;
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
    wchar_t buf[16];
    swprintf(buf, 16, L"%02lld:%02lld", (long long)(secs / 60), (long long)(secs % 60));
    SetDlgItemTextW(hdlg, IDC_TIMER, buf);
}

LedState peak_to_led(float peak) {
    if (peak >= 0.95f) return LedState::RecordingClip;
    if (peak >= 0.80f) return LedState::RecordingWarn;
    return LedState::RecordingOk;
}

// Forward decls — definidas más abajo (start_recording / sync / stop), pero
// referenciadas por handle_mid_record_disconnect que vive aquí arriba.
void sync_settings_language_from_combo(HWND hdlg);

// Escapa los caracteres que SysLink interpreta como marcado (<, >, &).
std::wstring escape_syslink(const std::wstring & s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) {
        switch (c) {
            case L'&': out += L"&amp;";  break;
            case L'<': out += L"&lt;";   break;
            case L'>': out += L"&gt;";   break;
            default:   out += c;         break;
        }
    }
    return out;
}

// Repinta el SysLink con el nombre del mic actual. Si el id guardado quedó
// obsoleto, cae al default + status_msg.
void refresh_mic_link(HWND hdlg) {
    std::wstring id   = g_settings.mic_device_id;
    std::wstring name = id.empty() ? std::wstring{} : audio::friendly_name_for(id);

    if (!id.empty() && name.empty()) {
        // ID obsoleto: el dispositivo desapareció entre sesiones (o entre clicks).
        g_settings.mic_device_id.clear();
        g_capture.set_device_id(L"");
        cfg::save_settings(g_settings);
        g_mic_fallback_msg = L"Micrófono guardado no disponible, usando el predeterminado.";
        id.clear();
    }

    std::wstring display = id.empty() ? L"Predeterminado del sistema" : name;
    std::wstring text = L"Micrófono: <a>" + escape_syslink(display) + L"</a>";
    SetDlgItemTextW(hdlg, IDC_MIC_LINK, text.c_str());
}

void apply_mic_selection(HWND hdlg, const std::wstring & new_id) {
    if (new_id == g_settings.mic_device_id) return;
    g_settings.mic_device_id = new_id;
    cfg::save_settings(g_settings);
    if (!g_is_recording) {
        // Sólo cambiamos el dispositivo en frío. Si REC está activo, dejamos la
        // grabación con el mic en uso para no provocar saltos inesperados.
        g_capture.set_device_id(new_id);
    }
    refresh_mic_link(hdlg);
}

// Muestra TrackPopupMenu nativo bajo el SysLink con la lista de dispositivos.
void show_mic_popup_menu(HWND hdlg) {
    auto devices = audio::enumerate_capture_devices();

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    // ID 1 = "(usar default)". Reales empiezan en 2.
    const bool current_is_default = g_settings.mic_device_id.empty();
    AppendMenuW(menu, MF_STRING | (current_is_default ? MF_CHECKED : 0),
                1, L"(Usar el predeterminado del sistema)");
    if (!devices.empty()) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    for (size_t i = 0; i < devices.size(); ++i) {
        const auto & d = devices[i];
        const bool is_current = d.id == g_settings.mic_device_id;
        std::wstring label = d.friendly_name.empty() ? L"(sin nombre)" : d.friendly_name;
        if (d.is_default) label += L"  (predeterminado)";
        AppendMenuW(menu, MF_STRING | (is_current ? MF_CHECKED : 0),
                    (UINT_PTR)(i + 2), label.c_str());
    }

    // Ancla: justo debajo del SysLink.
    RECT rc{};
    GetWindowRect(GetDlgItem(hdlg, IDC_MIC_LINK), &rc);

    SetForegroundWindow(hdlg);
    int cmd = TrackPopupMenu(menu,
                             TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_NONOTIFY,
                             rc.left, rc.bottom, 0, hdlg, nullptr);
    DestroyMenu(menu);

    if (cmd <= 0) return;
    if (cmd == 1) {
        apply_mic_selection(hdlg, L"");
    } else {
        size_t idx = (size_t)(cmd - 2);
        if (idx < devices.size()) {
            apply_mic_selection(hdlg, devices[idx].id);
        }
    }
}

void start_recording(HWND hdlg);  // fwd

// Llamado por WM_TIMER cuando detecta que el hilo de captura abortó
// (mic desconectado durante la grabación). Pregunta al usuario qué dispositivo
// usar para continuar; si cancela, transcribe lo grabado hasta el corte.
void handle_mid_record_disconnect(HWND hdlg, const std::wstring & reason) {
    g_is_recording = false;
    led_set_state(GetDlgItem(hdlg, IDC_LED), LedState::Error);
    SetDlgItemTextW(hdlg, IDC_STATUS, L"Micrófono desconectado, esperando elección…");

    std::wstring chosen;
    bool accepted = ui::show_mic_picker(
        hdlg, L"", chosen,
        L"El micrófono se desconectó — elige otro para continuar");

    if (accepted) {
        // Persistir la elección y reanudar la captura sobre el MISMO buffer.
        g_settings.mic_device_id = chosen;
        cfg::save_settings(g_settings);
        g_capture.set_device_id(chosen);

        std::wstring err = g_capture.resume_with_device(chosen);
        if (err.empty()) {
            g_is_recording = true;
            // No reiniciamos g_rec_start: el contador sigue donde estaba.
            std::wstring name = chosen.empty()
                ? std::wstring(L"el predeterminado del sistema")
                : audio::friendly_name_for(chosen);
            std::wstring status = L"Reanudando con ";
            status += name.empty() ? L"micrófono seleccionado" : name;
            status += L"…";
            SetDlgItemTextW(hdlg, IDC_STATUS, status.c_str());
            led_set_state(GetDlgItem(hdlg, IDC_LED), LedState::RecordingOk);
            refresh_mic_link(hdlg);
            return;
        }

        // Si falló reanudar, caemos al path de cancelar (no perder lo grabado).
        MessageBoxW(hdlg,
                    (L"No se pudo reanudar con el nuevo micrófono:\n\n" + err +
                     L"\n\nSe transcribirá lo grabado hasta ahora.").c_str(),
                    L"whisper_destilado — micrófono",
                    MB_OK | MB_ICONWARNING);
    } else if (!reason.empty()) {
        // El usuario canceló — al menos dejamos el aviso del motivo en el status.
        SetDlgItemTextW(hdlg, IDC_STATUS, reason.c_str());
    }

    // Cancelado o fallo al reanudar → procesar el buffer parcial.
    g_audio = g_capture.take_buffer();
    SetDlgItemTextW(hdlg, IDC_REC,   L"●  Grabar");
    SetDlgItemTextW(hdlg, IDC_TIMER, L"00:00");

    if (!g_audio || g_audio->empty()) {
        SetDlgItemTextW(hdlg, IDC_STATUS, L"No se capturó audio. Verifica el micrófono.");
        led_set_state(GetDlgItem(hdlg, IDC_LED), LedState::Ready);
        return;
    }

    g_is_transcribing = true;
    EnableWindow(GetDlgItem(hdlg, IDC_REC), FALSE);
    SetDlgItemTextW(hdlg, IDC_STATUS, L"Transcribiendo lo grabado hasta el corte…");
    SetDlgItemTextW(hdlg, IDC_TRANSCRIPT, L"Procesando audio con whisper.cpp…");
    led_set_state(GetDlgItem(hdlg, IDC_LED), LedState::Processing);

    sync_settings_language_from_combo(hdlg);
    inference::transcribe_async(g_model.context(), g_audio, g_settings, hdlg);
}

void start_recording(HWND hdlg) {
    std::wstring err = g_capture.start();
    if (!err.empty()) {
        led_set_state(GetDlgItem(hdlg, IDC_LED), LedState::Error);
        MessageBoxW(hdlg, err.c_str(), L"whisper_destilado", MB_OK | MB_ICONERROR);
        return;
    }
    g_is_recording = true;
    g_rec_start = std::chrono::steady_clock::now();
    SetDlgItemTextW(hdlg, IDC_REC,    L"■  Detener");
    SetDlgItemTextW(hdlg, IDC_STATUS, L"Grabando…");
    led_set_state(GetDlgItem(hdlg, IDC_LED), LedState::RecordingOk);
}

void sync_settings_language_from_combo(HWND hdlg) {
    HWND combo = GetDlgItem(hdlg, IDC_LANG);
    g_settings.language = ui::get_selected_language_code(combo);
}

void stop_recording(HWND hdlg) {
    g_capture.stop();
    g_is_recording = false;
    g_audio = g_capture.take_buffer();
    SetDlgItemTextW(hdlg, IDC_REC,    L"●  Grabar");
    SetDlgItemTextW(hdlg, IDC_TIMER,  L"00:00");

    if (!g_audio || g_audio->empty()) {
        SetDlgItemTextW(hdlg, IDC_STATUS, L"No se capturó audio. Verifica el micrófono.");
        led_set_state(GetDlgItem(hdlg, IDC_LED), LedState::Ready);
        return;
    }

    // Lanza transcripción asíncrona.
    g_is_transcribing = true;
    EnableWindow(GetDlgItem(hdlg, IDC_REC), FALSE);
    SetDlgItemTextW(hdlg, IDC_STATUS, L"Transcribiendo…");
    SetDlgItemTextW(hdlg, IDC_TRANSCRIPT, L"Procesando audio con whisper.cpp…");
    led_set_state(GetDlgItem(hdlg, IDC_LED), LedState::Processing);

    sync_settings_language_from_combo(hdlg);
    inference::transcribe_async(g_model.context(), g_audio, g_settings, hdlg);
}

void start_retranscribe(HWND hdlg, const cfg::Settings & s, const wchar_t * status_msg) {
    if (!g_audio || g_audio->empty()) return;
    g_is_transcribing = true;
    EnableWindow(GetDlgItem(hdlg, IDC_REC), FALSE);
    SetDlgItemTextW(hdlg, IDC_STATUS, status_msg);
    SetDlgItemTextW(hdlg, IDC_TRANSCRIPT, L"Procesando audio en RAM (mismo buffer, sin volver a grabar)…");
    led_set_state(GetDlgItem(hdlg, IDC_LED), LedState::Processing);
    inference::transcribe_async(g_model.context(), g_audio, s, hdlg);
}

// Ruta de salida del texto: misma carpeta y nombre que el audio, con .txt.
std::wstring transcript_path_for(const std::wstring & audio_path) {
    size_t slash = audio_path.find_last_of(L"\\/");
    size_t dot   = audio_path.find_last_of(L'.');
    if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash))
        return audio_path.substr(0, dot) + L".txt";
    return audio_path + L".txt";
}

// Escribe texto UTF-8 a disco (sobrescribe). Devuelve true si se guardó entero.
bool write_utf8_file(const std::wstring & path, const std::string & utf8) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = utf8.empty() ||
              (WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, nullptr) &&
               written == (DWORD)utf8.size());
    CloseHandle(h);
    return ok;
}

// Arranca (o continúa) la tanda: toma el archivo en g_batch_index, carga su
// audio y lanza la transcripción asíncrona. Un archivo que no cargue se cuenta
// como fallo y se salta. Cuando no quedan archivos, cierra la tanda.
void start_next_batch_file(HWND hdlg) {
    while (g_batch_index < g_batch_files.size()) {
        const std::wstring & path = g_batch_files[g_batch_index];
        g_batch_current_path = path;

        std::wstring load_err;
        auto buf = audio::load_audio_file(path.c_str(), &load_err);
        if (!buf || buf->empty()) {
            ++g_batch_failed;
            ++g_batch_index;
            continue;  // salta al siguiente archivo
        }

        g_audio = buf;
        g_is_transcribing = true;

        size_t s = path.find_last_of(L"\\/");
        std::wstring name = (s == std::wstring::npos) ? path : path.substr(s + 1);
        wchar_t status[600];
        swprintf(status, 600, L"Transcribiendo (%zu/%zu): %s",
                 g_batch_index + 1, g_batch_total, name.c_str());
        SetDlgItemTextW(hdlg, IDC_STATUS, status);
        SetDlgItemTextW(hdlg, IDC_TRANSCRIPT, L"Procesando audio con whisper.cpp…");
        led_set_state(GetDlgItem(hdlg, IDC_LED), LedState::Processing);

        sync_settings_language_from_combo(hdlg);
        inference::transcribe_async(g_model.context(), g_audio, g_settings, hdlg);
        return;  // esperamos WM_TRANSCRIBE_DONE para continuar con el siguiente
    }

    // Sin archivos restantes: fin de la tanda.
    g_batch_mode = false;
    g_is_transcribing = false;
    EnableWindow(GetDlgItem(hdlg, IDC_REC), TRUE);
    led_set_state(GetDlgItem(hdlg, IDC_LED), LedState::Ready);
    wchar_t done[256];
    swprintf(done, 256, L"Tanda completada: %zu transcritos, %zu con error.",
             g_batch_ok, g_batch_failed);
    SetDlgItemTextW(hdlg, IDC_STATUS, done);
    SetDlgItemTextW(hdlg, IDC_TRANSCRIPT, done);
    g_batch_files.clear();
}

void on_transcribe_done(HWND hdlg, bool ok, inference::Result * result) {
    // Modo tanda: guardar el texto junto al audio y pasar al siguiente archivo
    // sin abrir el diálogo de resultado (transcripción desatendida).
    if (g_batch_mode) {
        if (result && ok && result->error.empty()) {
            std::wstring out = transcript_path_for(g_batch_current_path);
            if (write_utf8_file(out, inference::format_txt(*result))) ++g_batch_ok;
            else                                                      ++g_batch_failed;
        } else {
            ++g_batch_failed;
        }
        delete result;
        ++g_batch_index;
        start_next_batch_file(hdlg);  // siguiente archivo o cierre de la tanda
        return;
    }

    g_is_transcribing = false;
    EnableWindow(GetDlgItem(hdlg, IDC_REC), TRUE);
    led_set_state(GetDlgItem(hdlg, IDC_LED), LedState::Ready);
    SetDlgItemTextW(hdlg, IDC_STATUS, L"Listo. Pulsa Grabar para empezar.");

    if (!result) return;

    if (ok && result->error.empty()) {
        // Resumen en el panel principal.
        std::wstring summary;
        for (size_t i = 0; i < result->segments.size() && i < 3; ++i) {
            summary += result->segments[i].text;
            summary += L"\r\n";
        }
        if (result->segments.size() > 3) summary += L"…\r\n";
        SetDlgItemTextW(hdlg, IDC_TRANSCRIPT, summary.c_str());
    } else {
        SetDlgItemTextW(hdlg, IDC_TRANSCRIPT, result->error.c_str());
    }

    ui::ResultOutcome outcome = ui::show_result_dialog(hdlg, *result);
    delete result;

    switch (outcome.action) {
        case ui::ResultAction::Close:
            break;

        case ui::ResultAction::RetryQuality: {
            cfg::Settings q = g_settings.with_quality_preset();
            start_retranscribe(hdlg, q,
                L"Re-transcribiendo con más calidad (beam search, mismo audio en RAM)…");
            break;
        }

        case ui::ResultAction::RetryWithLanguage: {
            // Persistir el nuevo idioma en g_settings y reflejarlo en el combo principal.
            g_settings.language = outcome.new_language;
            ui::set_selected_language_code(GetDlgItem(hdlg, IDC_LANG),
                                           outcome.new_language.c_str());
            start_retranscribe(hdlg, g_settings,
                L"Re-transcribiendo con idioma forzado (mismo audio en RAM)…");
            break;
        }
    }
}

INT_PTR CALLBACK DialogProc(HWND hdlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_INITDIALOG: {
            g_settings = cfg::load_settings();
            set_unicode_strings(hdlg);
            init_main_language_combo(hdlg);
            led_set_state(GetDlgItem(hdlg, IDC_LED), LedState::Idle);
            EnableWindow(GetDlgItem(hdlg, IDC_REC), FALSE);

            // Validar el mic guardado. Si desapareció entre sesiones, caer al
            // default del sistema y guardarlo como status diferido — se mostrará
            // tras la carga del modelo (`on_model_loaded` sobrescribe el status).
            std::wstring eff_id = g_settings.mic_device_id;
            if (!eff_id.empty() && !audio::device_exists(eff_id)) {
                g_mic_fallback_msg = L"Micrófono guardado no disponible, usando el predeterminado.";
                eff_id.clear();
                g_settings.mic_device_id.clear();
                cfg::save_settings(g_settings);
            }
            g_capture.set_device_id(eff_id);
            refresh_mic_link(hdlg);

            // Empezar a recibir notificaciones de plug/unplug.
            g_notifier.start(hdlg);

            g_model.start();
            SetTimer(hdlg, kTimerId, kTimerPeriod, nullptr);

            // Primera ejecución: si no hay mic guardado y hay >1 dispositivo,
            // pedimos elegir tras WM_INITDIALOG (la ventana ya estará visible).
            if (g_settings.mic_device_id.empty()) {
                auto devs = audio::enumerate_capture_devices();
                if (devs.size() > 1) {
                    PostMessageW(hdlg, WM_FIRST_RUN_MIC_PICKER, 0, 0);
                }
            }

            // Habilitar arrastrar y soltar archivos de audio.
            DragAcceptFiles(hdlg, TRUE);
            return TRUE;
        }

        case WM_FIRST_RUN_MIC_PICKER: {
            std::wstring chosen;
            if (ui::show_mic_picker(hdlg, L"", chosen,
                                    L"Elige tu micrófono preferido")) {
                apply_mic_selection(hdlg, chosen);
            }
            return TRUE;
        }

        case audio::WM_DEVICES_CHANGED:
            refresh_mic_link(hdlg);
            return TRUE;

        case WM_NOTIFY: {
            auto * nm = (NMHDR *) lParam;
            if (nm && nm->idFrom == IDC_MIC_LINK &&
                (nm->code == NM_CLICK || nm->code == NM_RETURN)) {
                show_mic_popup_menu(hdlg);
                return TRUE;
            }
            break;
        }

        case WM_TIMER:
            if (wParam == kTimerId) {
                poll_model_state(hdlg);
                if (g_is_recording) {
                    // Detectar aborto del hilo de captura (mic desconectado, etc.).
                    if (!g_capture.is_running()) {
                        handle_mid_record_disconnect(hdlg, g_capture.abort_reason());
                    } else {
                        float peak = audio::g_peak.load(std::memory_order_acquire);
                        led_set_state(GetDlgItem(hdlg, IDC_LED), peak_to_led(peak));
                        update_timer_label(hdlg);
                    }
                }
            }
            return TRUE;

        case inference::WM_TRANSCRIBE_DONE:
            on_transcribe_done(hdlg, wParam == 0,
                               reinterpret_cast<inference::Result *>(lParam));
            return TRUE;

        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT * dis = (const DRAWITEMSTRUCT *) lParam;
            if (dis && dis->CtlID == IDC_LED) {
                led_on_draw_item(dis);
                return TRUE;
            }
            break;
        }

        case WM_COMMAND: {
            const WORD id   = LOWORD(wParam);
            const WORD code = HIWORD(wParam);
            switch (id) {
                case IDC_REC:
                    if (code == BN_CLICKED) {
                        if (g_is_recording) stop_recording(hdlg);
                        else                start_recording(hdlg);
                    }
                    return TRUE;

                case IDC_ADVANCED:
                    if (code == BN_CLICKED) {
                        // El combo principal y el del diálogo comparten g_settings.language;
                        // sincronizamos antes de abrir para que el diálogo refleje la selección actual.
                        sync_settings_language_from_combo(hdlg);
                        if (ui::show_settings_dialog(hdlg, g_settings)) {
                            // Refleja en el combo principal cualquier cambio de idioma hecho en Avanzado.
                            ui::set_selected_language_code(
                                GetDlgItem(hdlg, IDC_LANG), g_settings.language.c_str());
                        }
                    }
                    return TRUE;

                case IDCANCEL:  // Esc
                    if (g_is_recording) stop_recording(hdlg);
                    DestroyWindow(hdlg);
                    return TRUE;
            }
            break;
        }

        case WM_DROPFILES: {
            HDROP hdrop = reinterpret_cast<HDROP>(wParam);

            if (g_is_recording || g_is_transcribing || !g_model.context()) {
                DragFinish(hdrop);
                return TRUE;
            }

            // Reúne TODOS los archivos soltados que tengan extensión compatible,
            // conservando el orden en que Windows los entrega.
            UINT count = DragQueryFileW(hdrop, 0xFFFFFFFF, nullptr, 0);
            std::vector<std::wstring> files;
            for (UINT i = 0; i < count; ++i) {
                wchar_t path[MAX_PATH] = {};
                if (DragQueryFileW(hdrop, i, path, MAX_PATH) &&
                    audio::is_supported_audio_extension(path)) {
                    files.emplace_back(path);
                }
            }
            DragFinish(hdrop);

            if (files.empty()) {
                SetDlgItemTextW(hdlg, IDC_STATUS,
                    L"Ningún archivo compatible. Usa WAV, MP3, M4A, FLAC…");
                return TRUE;
            }

            EnableWindow(GetDlgItem(hdlg, IDC_REC), FALSE);

            if (files.size() == 1) {
                // Un solo archivo: comportamiento clásico con diálogo de resultado.
                g_batch_mode = false;
                SetDlgItemTextW(hdlg, IDC_STATUS, L"Cargando archivo de audio…");
                led_set_state(GetDlgItem(hdlg, IDC_LED), LedState::Processing);

                std::wstring load_err;
                auto buf = audio::load_audio_file(files[0].c_str(), &load_err);
                if (!buf || buf->empty()) {
                    SetDlgItemTextW(hdlg, IDC_STATUS, load_err.c_str());
                    led_set_state(GetDlgItem(hdlg, IDC_LED), LedState::Error);
                    EnableWindow(GetDlgItem(hdlg, IDC_REC), TRUE);
                    return TRUE;
                }

                g_audio = buf;
                g_is_transcribing = true;
                SetDlgItemTextW(hdlg, IDC_STATUS, L"Transcribiendo archivo…");
                SetDlgItemTextW(hdlg, IDC_TRANSCRIPT, L"Procesando audio con whisper.cpp…");
                sync_settings_language_from_combo(hdlg);
                inference::transcribe_async(g_model.context(), g_audio, g_settings, hdlg);
                return TRUE;
            }

            // Varios archivos: transcripción por lotes, un .txt junto a cada audio.
            g_batch_files  = std::move(files);
            g_batch_total  = g_batch_files.size();
            g_batch_index  = 0;
            g_batch_ok     = 0;
            g_batch_failed = 0;
            g_batch_mode   = true;
            start_next_batch_file(hdlg);
            return TRUE;
        }

        case WM_CLOSE:
            if (g_is_recording) {
                g_capture.stop();
                g_is_recording = false;
            }
            DestroyWindow(hdlg);
            return TRUE;

        case WM_DESTROY:
            KillTimer(hdlg, kTimerId);
            // Desregistrar callbacks COM antes del teardown.
            g_notifier.stop();
            // Asegurar que ningún worker quede usando whisper_context durante el teardown.
            inference::join_pending_workers();
            PostQuitMessage(0);
            return TRUE;
    }
    return FALSE;
}

} // namespace

HWND window_create(HINSTANCE hInstance) {
    INITCOMMONCONTROLSEX icc { sizeof(icc), ICC_STANDARD_CLASSES | ICC_LINK_CLASS };
    InitCommonControlsEx(&icc);

    HWND hdlg = CreateDialogParamW(hInstance, MAKEINTRESOURCEW(IDD_MAIN),
                                   nullptr, DialogProc, 0);
    if (hdlg) {
        ShowWindow(hdlg, SW_SHOWDEFAULT);
        UpdateWindow(hdlg);
    }
    return hdlg;
}
